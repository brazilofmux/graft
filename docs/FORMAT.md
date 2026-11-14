# `.gft` patch format, version 1

A `.gft` file turns one source blob into one target blob. All integers are
little-endian. `uvarint` is LEB128 (7 bits per byte, low bits first, at most
10 bytes, the value must fit in 64 bits, and the encoding must be minimal: no
trailing `0x00` byte). `svarint` is a zigzag-encoded
`uvarint`.

```
patch   := header [masks] block* end
header  := magic[8] version:u16 flags:u16 source_size:u64 target_size:u64
           source_hash[32] target_hash[32] max_stream:u32           (96 bytes)
block   := 0x01 ctrl_raw ctrl_comp diff_raw diff_comp extra_raw extra_comp  (6 × uvarint)
           ctrl_bytes[ctrl_comp] diff_bytes[diff_comp] extra_bytes[extra_comp]
end     := 0x00 patch_hash[32]
masks   := count:uvarint mask[count]                       (present iff flags bit 0)
mask    := old_offset old_length new_offset new_length  (4 × uvarint)  mode:u8 pad:u8
```

- `magic` is `89 47 46 54 0D 0A 1A 0A` (`\x89GFT\r\n\x1a\n`). As in PNG, the
  high byte and the CR/LF pair catch 7-bit and line-ending damage in transit.
- `version` is 1. A decoder rejects other versions as *unsupported*.
- `flags` bit 0 means a mask table follows the header. A decoder rejects any
  other set bit as *unsupported*.
- `source_hash`, `target_hash` and `patch_hash` are BLAKE3-256. `patch_hash`
  covers every byte before it, including the `0x00` end tag.
- `max_stream` is the largest `*_raw` value in the file. Decoders compare it
  against their memory limit before reading any block.

## Streams

Each block carries three streams. A stream with `raw = 0` has `comp = 0` and
no bytes. Otherwise it is one or more zstd frames that decompress to exactly
`raw` bytes.

- **ctrl:** a sequence of triples `add:uvarint copy:uvarint seek:svarint`.
- **diff:** `add` bytes per triple, added (mod 256) to source bytes.
- **extra:** `copy` bytes per triple, copied straight to the output.

## Masked regions

Masked regions are bytes that differ per install, such as serials and keys.
The applier carries them from the installed file into the output instead of
taking them from the patch.

- **Masks:** 1 to 4096 of them, sorted by `old_offset`. Old regions must not
  overlap each other, and new regions must not overlap each other. No region
  may be empty, and every region must lie within its file.
- **`mode`:** 0 means *exact*, and the lengths must be equal. 1 means *pad*:
  `new_length ≥ old_length`, and the extra bytes are `pad`. 2 means
  *truncate*: `new_length ≤ old_length`, and the leading bytes are kept.
  Any other value is corrupt.
- **Normalized files:** with masks present, every masked region in the old
  and new files is filled with `0x00`. `source_hash`, `target_hash` and the
  delta itself are all computed over these normalized forms.
- **Applying with masks:** the decoder reads each old region from the
  installed file, applies the delta to the normalized source, and writes the
  saved bytes over each new region of the output: the first
  `min(old_length, new_length)` bytes, followed by `pad` bytes if the region
  grew. `target_hash` is checked against the output *before* the saved bytes
  are written.

## Applying

The decoder keeps `oldpos` (a source offset, starting at 0) across blocks. For
each triple:

1. Output `diff[i] + source[oldpos + i]` for `i` in `[0, add)`, then
   `oldpos += add`.
2. Output the next `copy` bytes of `extra`.
3. `oldpos += seek`.

These are bsdiff's control semantics. Encoders may split a triple at a block
boundary.

A decoder **must** reject the patch as corrupt if:
- `oldpos + add` exceeds `source_size`, or `oldpos + seek` falls outside
  `[0, source_size]`
- a block's triples don't consume its `diff` and `extra` streams exactly, or
  its `ctrl` stream ends partway through a triple
- the total output differs from `target_size`
- any hash fails to match, or bytes follow `patch_hash`

A source whose size or hash doesn't match the header is rejected as the
*wrong source*, before any output is produced.

Output is produced before the trailing `patch_hash` and `target_hash` can be
checked, so a caller must treat output as provisional until apply returns
success. The `graft` CLI writes to a temporary file and renames it into place.
