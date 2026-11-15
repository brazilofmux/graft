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
- **Executable filters** (preprocessing that makes executables delta better).
  Planned as BCJ-style rel→abs rewrites; measurement showed that for this
  encoder the useful direction is abs→rel (see M5a below). Instruction
  knowledge comes from the sibling emulator projects: [x86](https://github.com/brazilofmux/x86) (8086
  through P4), [z80](https://github.com/brazilofmux/z80), and [SLOW-32](https://github.com/brazilofmux/slow32-public).
- C ABI (`graft.h`) over a C++20 implementation.

### 2. `graft`: patch packages (RTPatch-class)
- **Trees:** add, delete and modify files, permissions and symlinks, with
  rename and move detection by content hash.
- **Multi-source:** one package updates from *any* of V1…Vn to Vn+1. Each
  installed file's version is identified by its (masked) hash, and deltas are
  shared across sources wherever the source bytes match.
- **Masked regions** (decided 2026-11-13):
  - *Located when the patch is built*, by offset or by a marker that must
    occur exactly once, in both the old and the new version. The patch
    records the mapping (old offset and length to new offset and length),
    so the machine applying it never searches.
  - *Apply:* the installed file's bytes in each region are lifted out, and
    the region is overwritten with a fixed placeholder so every install
    looks identical; the source hash covers this normalized form. An
    ordinary delta runs, and the saved bytes are written into the new file
    at the new offsets. The delta engine itself knows nothing about masks.
  - *Size changes* fail when the patch is built, unless the builder gives
    an explicit `pad=BYTE` or `truncate` rule. A bad guess about a serial
    field should surface on the builder's machine, not on a user's.
  - Searching at apply time (a region at a different offset on each
    install) and a *regenerate* hook are deferred until a real case needs
    them.
- **Apply:** dry-run verify, transactional apply with backup and rollback,
  resume after interruption, and path-traversal and symlink-attack hardening.

### 3. `graft archive`: versioned archive (JAR32-class)
- A `.gfa` archive holds many versions of a file set. *Changed 2026-11-13:*
  forward deltas with chains capped at 8 (a full copy restarts the chain),
  rather than RCS-style reverse deltas, so the archive is append-only. Adding
  a version never rewrites what's there, an interrupted add can't damage
  earlier versions, and extraction cost stays bounded. Identical contents
  are stored once.
- Operations: `add` a version, `list`, `extract`, `diff`, `drop` (rewrites).
- **Bridge to (2):** `graft archive mkpatch -o up.gfp archive.gfa 6 3 4 5`
  turns an archive into a multi-source package.

## Test corpus and platforms (from the sibling projects)
- **A real multi-version corpus:** DOS and CP/M software that shipped
  serialized point releases (WordPerfect, WordStar, Turbo Pascal, DOS and
  Windows versions, as run by the [x86](https://github.com/brazilofmux/x86) emulator). This is the kind of software
  RTPatch and JAR32 were built for, and its serial-stamped binaries exercise
  masked regions directly. The copyrighted binaries stay out of the repo, as
  they do in the emulator projects.
- **End-to-end tests inside emulators:** apply patches to disk images and
  boot them in `dos-monster` and `z80-monster`, so "it still runs" is
  checked as well as hashes matching.
- **Portability and sandboxing:** keep the *applier* core freestanding C/C++
  so it also builds for **SLOW-32** (clang `slow32-unknown-none`) and runs
  in that sandbox. That keeps the decoder honest about its dependencies,
  gives a 32-bit target, and provides a way to apply untrusted patches in
  isolation. A 16-bit DOS applier is a stretch goal.
- **Toolchains:** the SLOW-32 build uses an existing SLOW-32 toolchain
  (its LLVM backend, libc++, assembler and linker) read-only; see
  `tools/slow32/build.sh` for the variables that locate it. Fuzzing on macOS
  uses Homebrew's `llvm` (Apple clang has no libFuzzer).

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
| **M3** | Directory trees and multi-source packages, transactional apply. Done 2026-11-13: one delta per distinct old content, shared across versions; renames by content or file name; POSIX file replacement behind std::filesystem, Windows specifics (locked files, ACLs) deferred |
| **M4** | Versioned archive, plus `mkpatch` from an archive. Done 2026-11-13 |
| **M5a** | Executable filters. Done 2026-11-13. Finding: for a bsdiff-class encoder, relativize *absolute* references and leave relative ones alone. Z80 (boundary-walking, CALL/JP plus data addresses) cut CP/M patches by a third against bsdiff. BCJ-style rel→abs filters (x86, ARM64) made real builds 12–36% worse, so they're explicit-only. Next for executables: an x86-16 filter that relativizes absolute data references and far calls (needs an instruction-length decoder and DOS version pairs), and Courgette-style label alignment for modern PIC code |
| **M5** | Done 2026-11-13. Rolling-hash encoder (`-m rolling`; AUTO above 512 MiB of source): on 2 GiB inputs, 6.8 s and 4.4 GB against the suffix array's 64 s and 18.8 GB, at the same patch size on scattered edits; 1-11% larger on executables. The encoder now writes blocks one at a time. SLOW-32 applier: `tools/slow32/build.sh` builds graft-apply from the decoder, zstd's decompressor and portable BLAKE3 with the existing SLOW-32 toolchain (read-only); `tools/slow32/test.sh` and `-DGRAFT_SLOW32_TEST=ON` check it against host-made patches |
| **Later** | VCDIFF import/export (xdelta3 interop), self-applying patches, 16-bit DOS applier |

## Open questions
1. ~~License?~~ MIT (2026-11-13), "The graft authors" as in z80-monster and dos-monster; dependency licenses in THIRD-PARTY.md.
2. ~~How should masked regions behave when the new version moves or resizes the region?~~ Decided: see Masked regions above.
3. ~~One delta per source, or chains?~~ One delta per distinct old content, shared by every version with that content; no chains.
4. ~~How important is Windows?~~ POSIX first; file replacement sits behind std::filesystem so a Windows version can slot in.
