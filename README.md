# graft

Binary patching: bsdiff-quality deltas, xdelta-style streaming,
RTPatch-style deployment (masked regions, patching from any of several old
versions, whole trees), and JAR32-style multi-version archives.

- `libgraft`: the delta engine (C ABI, C++20 implementation)
- `graft`: builds and applies patches for single files and whole trees
- `graft archive`: a versioned archive that can extract any version and
  generate a patch from any set of versions

Status: **M3**. Single files (with masked regions) and whole directory trees work, in the `.gft` v1 format
([docs/FORMAT.md](docs/FORMAT.md)). See [docs/SCOPE.md](docs/SCOPE.md) for the plan.

```sh
graft diff OLD NEW PATCH        # -l LEVEL (zstd 1-22, default 19), -b BLOCK (bytes)
graft apply OLD PATCH NEW       # writes NEW only if every check passes
graft verify OLD PATCH
graft info PATCH
```

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
