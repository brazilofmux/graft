# graft

Binary patching: bsdiff-quality deltas, xdelta-style streaming,
RTPatch-style deployment (masked regions, patching from any of several old
versions, whole trees), and JAR32-style multi-version archives.

- `libgraft`: the delta engine (C ABI, C++20 implementation)
- `graft`: builds and applies patches for single files and whole trees
- `graft archive`: a versioned archive that can extract any version and
  generate a patch from any set of versions

[![ci](https://github.com/brazilofmux/graft/actions/workflows/ci.yml/badge.svg)](https://github.com/brazilofmux/graft/actions/workflows/ci.yml)

**Status: experimental.** Everything in [docs/SCOPE.md](docs/SCOPE.md)'s
milestones M0–M5 works: single files (with masked regions and executable
filters), multi-version directory-tree packages with transactional update,
append-only versioned archives, and two matchers. Some caveats:

- **The formats aren't frozen.** [`.gft`](docs/FORMAT.md),
  [`.gfp`](docs/PACKAGE.md) and [`.gfa`](docs/ARCHIVE.md) are version 1 and
  may still change incompatibly before a 1.0 release.
- **POSIX only for now** (macOS, Linux). The library uses `std::filesystem`,
  but Windows specifics (replacing files that are in use, ACLs) aren't done.
- **Patches, packages and archives are treated as untrusted:** every field is
  bounds-checked, and the parsers are fuzzed continuously in CI. The code
  hasn't had an independent security review.

```sh
graft diff OLD NEW PATCH        # -l LEVEL (zstd 1-22, default 19), -b BLOCK (bytes)
graft apply OLD PATCH NEW       # writes NEW only if every check passes
graft verify OLD PATCH
graft info PATCH
```

Executable filters make moved code diff better. `-f z80` relativizes the
absolute calls, jumps and data addresses of 8080/Z80 code: on CP/M 2.2's
BDOS with a 3-byte fix inserted, the patch is a third smaller than bsdiff's.
Packages take `--filter "GLOB=z80"`.

Very large files: `-m rolling` (automatic above 512 MiB of source) trades
a little ratio for far less time and memory: 2 GiB in 7 seconds and 4.4 GB
instead of 64 seconds and 19 GB.

The applier also builds for [SLOW-32](https://github.com/brazilofmux/slow32-public)
(`tools/slow32/build.sh`), so untrusted patches can be applied inside its
sandbox.

Masked regions keep each install's own bytes (serial numbers, keys). They
are located when the patch is built, in both versions:

```sh
graft diff --mask "marker=SERIAL:,len=19" v1.exe v2.exe update.gft
graft apply customer.exe update.gft customer-v2.exe   # keeps the customer's serial
```

Whole trees: one package updates any of several old releases, all or
nothing ([docs/PACKAGE.md](docs/PACKAGE.md)):

```sh
graft pack --label 3.0 --mask "bin/*.exe=marker=SERIAL:,len=19" \
           -o update.gfp release-3.0/ release-1.0/=1.0 release-2.0/=2.0
graft list update.gfp
graft update --dry-run update.gfp /opt/app      # show the plan; change nothing
graft update update.gfp /opt/app                # conflicts stop it before any change
```

Versioned archives keep every release in one append-only file and can
produce a package from any of them ([docs/ARCHIVE.md](docs/ARCHIVE.md)):

```sh
graft archive add releases.gfa release-1.0/ 1.0   # creates the archive
graft archive add releases.gfa release-2.0/ 2.0
graft archive list releases.gfa
graft archive diff releases.gfa 1.0 2.0
graft archive extract releases.gfa 1.0 /tmp/old
graft archive mkpatch -o update.gfp releases.gfa 2.0   # from every other version
graft archive drop releases.gfa 1.0
```

## Building

Needs CMake ≥ 3.24, Ninja and a C++20 compiler. Dependencies (zstd, libsais,
BLAKE3) come from an installed package if one is found, and are otherwise
downloaded at a pinned version and SHA-256 (`GRAFT_DEPS=auto|system|fetch`).

```sh
brew install ninja zstd libsais blake3     # macOS; optional, see above
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

| Preset | Use |
|---|---|
| `dev` | Debug, warnings as errors |
| `release` | RelWithDebInfo |
| `asan` | Debug + AddressSanitizer + UBSan |
| `fetch` | Like `dev`, but downloads every dependency (as on a clean machine) |
| `fuzz` | libFuzzer targets, using `clang` from PATH |
| `fuzz-macos` | libFuzzer targets, using Homebrew LLVM (`brew install llvm`); Apple clang has no libFuzzer |

```sh
cmake --preset fuzz-macos && cmake --build --preset fuzz-macos
build/fuzz/fuzz/fuzz_hash fuzz/corpus/hash -max_total_time=60
```

## Benchmarks

```sh
python3 -I bench/synth.py /tmp/corpus            # synthetic old/new pairs
bench/bench.sh /tmp/corpus                       # xdelta3, bsdiff, zstd, graft
```

`bench.sh` reports patch size, encode and decode time, and peak RSS, and
verifies each round trip. Real version pairs (which can't be redistributed)
go in `bench/corpus/private/<name>/{old,new}`, which git ignores.

## License

MIT; see [LICENSE](LICENSE). Dependencies and their licenses are listed in
[THIRD-PARTY.md](THIRD-PARTY.md).
