# `.gfa` archive format, version 1

An archive holds many versions of a directory tree. Contents are stored
once, and each is either a full copy or a delta against an earlier content.
The file is **append-only**: adding a version writes new contents, then a
*segment* describing them and the version, then a *footer*. Nothing already
written changes. Encodings are as in [FORMAT.md](FORMAT.md) and
[PACKAGE.md](PACKAGE.md).

```
archive  := header (content* segment footer)*
header   := magic[8] version:u16 flags:u16                    (12 bytes)
footer   := seg_offset:u64 seg_comp:u64 seg_raw:u64 prev:u64
            seg_hash[32] "GFASEG\0\x01"                        (72 bytes)
```

- `magic` is `89 47 46 41 0D 0A 1A 0A` (`\x89GFA\r\n\x1a\n`). `version` is
  1 and `flags` is 0.
- **Footers.** Each footer locates its segment, a single zstd frame of
  `seg_comp` bytes that ends where the footer begins and decompresses to
  `seg_raw` bytes (at most 256 MiB). `seg_hash` is the BLAKE3 of the
  compressed segment. `prev` is the offset of the previous footer, or
  `2^64 - 1` for the first.
- **Opening** starts at the last footer and follows `prev` back to the first.
  Segments are then read in order, numbering contents 0, 1, 2, … across the
  whole archive.

## Segments

```
segment := n:uv content[n] version
content := hash[32] size:uv kind:u8 [base:uv if kind = 1] offset:uv length:uv
version := label:str time:uv n:uv entry[n]
entry   := path:str kind:u8 body
           file (0): mode:uv content:uv    dir (1): mode:uv    symlink (2): link:str
```

- **Contents.** `hash` and `size` describe the content itself. A content of
  `kind` 0 is a single zstd frame. A content of `kind` 1 is a `.gft` patch
  against an *earlier* content `base`, and the delta chain under it is at
  most 64 long. Writers keep chains to 8 and store a delta only when it's
  smaller than a full copy. `offset` and `length` must lie between the
  header and the segment.
- **Versions.** `label` is unique in the archive, 1 to 256 bytes, with no
  control characters. `time` is seconds since 1970.
- **Entries** follow the path rules in PACKAGE.md. They're sorted, every
  parent is present *and is a directory*, and no two paths differ only by
  case. So extracting a version can't write through a symlink or escape its
  directory.

## Robustness

- **Interrupted adds.** An add writes and syncs its contents before the
  segment and footer, so a footer always points at complete data. An add
  interrupted before its footer leaves a *torn tail*: readers ignore
  everything after the last valid footer, and the next add trims it off.
- **Damage.** A footer at the very end of the file that fails its checks is
  damage, not an interruption, and opening fails. Each content is checked
  against its `hash` whenever it's read.
- **Pruning** rewrites the archive with only the versions kept, and renames
  the result over the original.
