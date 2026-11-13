# graft — scope

Binary patching and the machinery around it: the delta quality of bsdiff,
the streaming and large-file handling of xdelta, the deployment features of
RTPatch (masked regions, patching from any of several old versions), and the
multi-version archive of JAR32.

## Lineage, and what each one contributes

| Tool | Takes from it | Leaves behind |
|---|---|---|
| **bsdiff** | Suffix-array matching with *approximate* matches split into diff and extra streams, which suits executables, where a small code change shifts many addresses | O(n) memory with a big constant, a single file only, a weak format, a history of memory-safety CVEs |
| **xdelta3** | Windowed, streaming encode and decode for files of any size, and VCDIFF (RFC 3284) | Weaker on executables, a single blob only |
| **RTPatch** | Masked regions (bytes such as serials, keys, timestamps and signatures that differ per install and must not break matching or be overwritten), one patch that works from any of N old versions, whole directory trees, verify and rollback | Closed source, proprietary format |
| **JAR32 / ARJ** | One archive holding many versions of a file set, any version extractable | DOS-era limits, closed format |
| **zstd `--patch-from`** | Used as a benchmark baseline, and zstd is our entropy backend | — |

## Components

### 1. `libgraft`: delta engine (single blob)
- **Encoders:**
  - *suffix-array* (bsdiff-class, via libsais) for the best ratio on executables;
  - *rolling-hash* (xdelta-class) for speed, streaming, and files larger than RAM.
- **Decoder:** streaming, with bounded memory and an upper limit on every
  size field the patch declares. It never trusts the patch file.
- **Container:** our own `.gft` format, versioned, with zstd-compressed
  streams and a hash of the source, the target and the patch itself (BLAKE3).
  It refuses to run against the wrong source.
- **Executable filters** (preprocessing that makes executables delta better):
  rewrite relative branch and call targets to absolute ones before
  diffing, and back again after (BCJ, Courgette or Zucchini style). Instruction
  decoding can come from our own emulators: **x86** (`~/x86`, 8086 through P4,
  16 and 32 bit), **Z80** (`~/z80`), and **SLOW-32** (`~/slow-32`).
- C ABI (`graft.h`) over a C++20 implementation.

### 2. `graft`: patch packages (RTPatch-class)
- **Trees:** add, delete and modify files, permissions and symlinks, with
  rename and move detection by content hash.
- **Multi-source:** one package updates from *any* of V1…Vn to Vn+1. Each
  installed file's version is identified by its (masked) hash, and deltas are
  shared across sources wherever the source bytes match.
- **Masked regions:**
  - *Specify* a region by offset and length, by marker or signature search
    (find `"SERIAL:"` and take the next 16 bytes), or by per-file rules.
  - *Build:* masked bytes are wildcards for hashing, version identification
    and matching.
  - *Apply:* the installed file's bytes for that region are carried into
    the new file at the corresponding location, either **preserve** or
    **relocate** (open question: whether we also need a **regenerate**
    hook).
- **Apply:** dry-run verify, transactional apply with backup and rollback,
  resume after interruption, and path-traversal and symlink-attack hardening.

### 3. `graft archive`: versioned archive (JAR32-class)
- A `.gfa` archive holds many versions of a file set: the newest version is
  stored in full, older ones as reverse deltas (RCS style), with periodic
  keyframes to bound extraction cost.
- Operations: `add` a version, `list`, `extract -v N`, `diff N M`, `prune`.
- **Bridge to (2):** `graft archive mkpatch --from 3,4,5 --to 6` turns an
  archive into a multi-source patch package.

## Test corpus and platforms (from the sibling projects)
- **A real multi-version corpus:** DOS and CP/M software that shipped
  serialized point releases (WordPerfect, WordStar, Turbo Pascal, DOS and
  Windows versions on `~/x86` disk images). This is the kind of software
  RTPatch and JAR32 were built for, and its serial-stamped binaries exercise
  masked regions directly. The copyrighted binaries stay out of the repo, as
  they do in `~/z80` and `~/x86`.
- **End-to-end tests inside emulators:** apply patches to disk images and
  boot them in `dos-monster` and `z80-monster`, so "it still runs" is
  checked as well as hashes matching.
- **Portability and sandboxing:** keep the *applier* core freestanding C/C++
  so it also builds for **SLOW-32** (clang `slow32-unknown-none`) and runs
  in that sandbox. That keeps the decoder honest about its dependencies,
  gives a 32-bit target, and provides a way to apply untrusted patches in
  isolation. A 16-bit DOS applier is a stretch goal.

## Non-goals
- Version control (branches, merges, history semantics); the archive is
  linear versions.
- An update delivery service, networking, or auto-updater UI.
- Code signing itself. We verify hashes and leave a hook for signature
  checks.
- A GUI.

## Engineering requirements
- CMake. Builds on macOS, Linux and Windows.
- Untrusted input everywhere: libFuzzer harnesses for every parser,
  ASan/UBSan in CI, and allocation caps.
- A benchmark harness that compares against xdelta3, bsdiff, zstd
  `--patch-from` and courgette or zucchini where available, on ratio, encode
  time, decode time and peak RSS.
- Dependencies (vendored or found): zstd (BSD), libsais (Apache-2.0),
  BLAKE3 (CC0/Apache-2.0).

## Milestones
| | Deliverable |
|---|---|
| **M0** | Repo, CMake skeleton, CI, fuzz and sanitizer builds, benchmark harness scaffold |
| **M1** | `libgraft` single blob: suffix-array encoder, streaming decoder, `.gft` v1, `graft diff` / `graft apply`, first benchmarks |
| **M2** | Masked regions on single files |
| **M3** | Directory trees and multi-source packages, transactional apply |
| **M4** | Versioned archive, plus `mkpatch` from an archive |
| **M5** | Executable filters (x86, Z80, SLOW-32), rolling-hash encoder for very large files, SLOW-32 applier build |
| **Later** | VCDIFF import/export (xdelta3 interop), self-applying patches, 16-bit DOS applier |

## Open questions
1. License (MIT, BSD-2, or Apache-2.0)? It has to be compatible with the vendored deps.
2. How should masked regions behave when the *new* version moves or resizes the region? Relocate by marker, or fail?
3. Should multi-source packages carry one delta per source (bigger, simple) or chain through intermediates (smaller, slower, more failure points)? Probably the encoder chooses per file.
4. How important is Windows? (It affects the apply-side file locking, ACLs and rename semantics.)
