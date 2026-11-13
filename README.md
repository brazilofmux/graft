# graft

Binary patching: bsdiff-quality deltas, xdelta-style streaming,
RTPatch-style deployment (masked regions, patching from any of several old
versions, whole trees), and JAR32-style multi-version archives.

- `libgraft`: the delta engine (C ABI, C++20 implementation)
- `graft`: builds and applies patches for single files and whole trees
- `graft archive`: a versioned archive that can extract any version and
  generate a patch from any set of versions

Status: scoping. See [docs/SCOPE.md](docs/SCOPE.md).
