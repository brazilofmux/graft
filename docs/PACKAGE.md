# `.gfp` package format, version 1

A package updates a directory tree from any of several old versions to one
new version. It carries one entry per path that differs, plus the blobs
(deltas and full copies) those entries use. Integer encodings are as in
[FORMAT.md](FORMAT.md): little-endian fixed width, minimal LEB128 `uv`.
`str` is a `uv` length followed by that many bytes (at most 4096).

```
package  := header blob* manifest footer hash
header   := magic[8] version:u16 flags:u16                         (12 bytes)
footer   := manifest_offset:u64 manifest_comp:u64 manifest_raw:u64  (24 bytes)
hash     := BLAKE3-256 of every byte before it
```

- `magic` is `89 47 46 50 0D 0A 1A 0A` (`\x89GFP\r\n\x1a\n`).
- `version` is 1 and `flags` is 0. Anything else is *unsupported*.
- The manifest is one zstd frame occupying exactly the bytes from
  `manifest_offset` to the footer. It decompresses to exactly
  `manifest_raw` bytes, at most 256 MiB.
- The manifest comes last so a builder can stream blobs straight out. An
  updater verifies `hash` over the whole file before it parses anything.

## Manifest

```
manifest := target_label:str  n:uv old_label:str[n]  n:uv blob[n]  n:uv entry[n]
blob     := kind:u8 offset:uv length:uv raw_size:uv
entry    := path:str kind:u8 body
```

**Blobs.** `kind` 0 is a `.gft` patch (FORMAT.md). `kind` 1 is a zstd frame
holding a whole file of `raw_size` bytes. `offset` and `length` locate the
blob in the file, between the header and the manifest.

**Entries**, by `kind`:

| kind | body | meaning |
|---|---|---|
| 0 file | `mode:uv size:uv current[32] n:uv (offset:uv length:uv)[n] n:uv recipe[n]` | make this file |
| 1 dir | `mode:uv` | make this directory |
| 2 symlink | `link:str n:uv known:str[n]` | make this symlink, possibly replacing one pointing at a `known` target |
| 3 remove | `what:u8` then, for a file, `n:uv hash[32][n]`; for a symlink, `n:uv link:str[n]`; for a directory, nothing | remove this, if it's one of the known contents |
| 4 seed | as a file entry, with no masked regions and exactly one recipe, a full copy | a **user-owned** file (configuration, data): install it only if the path is absent |

```
recipe   := kind:u8 [source_path:str if kind = 0] blob:uv n:uv label:uv[n]
```

- **File entries.** `current` is the BLAKE3 of the new file with its masked
  regions (the `(offset, length)` list) zeroed. An installed file of length
  `size` whose masked hash equals `current` is already up to date.
- **Recipes.** A *delta* recipe (`kind` 0) patches the installed file at
  `source_path` using a `.gft` blob. The path usually matches the entry's
  own; it differs when the file was renamed. A *full* recipe (`kind` 1)
  writes a whole copy and applies only when the file is absent. `label`
  lists the old versions the recipe serves, for display only.
- **Order.** Entries are sorted by path. One path may appear twice, only as
  a removal followed by a creation, which is how a kind changes (for
  example, a file becoming a directory).
- **Paths** are relative and `/`-separated. Every one of these is rejected:
  empty, `.` or `..` components, a leading `/`, a backslash, a colon,
  control characters, the reserved top-level `.graft-txn`, and two paths
  that differ only by ASCII case.
- **Modes** are POSIX permission bits, at most `07777`.

## Updating

1. **Plan.** Every entry is checked against the installed tree, and nothing
   changes yet. A file to patch must match one recipe's source hash. A file
   or symlink to remove must have known contents. No path may pass through a
   symlink or a non-directory. Any mismatch is a *conflict*, and the update
   stops. Paths the package doesn't mention are never touched, and neither
   is an existing user-owned (seed) file: it's never read, compared,
   changed or removed, so it can't cause a conflict.
2. **Stage.** Every new file and symlink is built under `.graft-txn/stage`
   and checked against `size` and `current`.
3. **Commit.** Each step is recorded in `.graft-txn/journal` and synced
   before it runs, in this order: back up removals (moved to
   `.graft-txn/backup`), remove directories deepest first (only if empty),
   create directories shallowest first, back up and install files, change
   modes, then write `commit`.
4. **Failure** during the commit undoes the journaled steps in reverse.
   An updater that finds `.graft-txn` left over finishes the cleanup if the
   journal ends in `commit`, and otherwise rolls the steps back.
