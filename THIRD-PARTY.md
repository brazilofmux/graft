# Third-party software

graft itself is MIT-licensed (see [LICENSE](LICENSE)). It uses these
libraries, which aren't in this repository. They come from an installed
package or are downloaded at a pinned version and SHA-256 by
`cmake/Deps.cmake`. A binary that links them statically must carry their
notices too.

| Library | Version | License | Used for |
|---|---|---|---|
| [zstd](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause (or GPL-2.0, at your option) | compressing patch streams, packages and archives |
| [libsais](https://github.com/IlyaGrebnov/libsais) | 2.12.2 | Apache-2.0 | suffix arrays for the encoder (not needed to apply patches) |
| [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) | 1.8.7 | CC0-1.0, or Apache-2.0, or Apache-2.0 with LLVM exception | hashing |

The applier (`graft-apply`, including the SLOW-32 build) uses only zstd's
decompressor and BLAKE3. It doesn't include libsais.

The matching in `src/encode.cpp` follows the algorithm of Colin Percival's
bsdiff 4 (BSD-2-Clause), reimplemented rather than copied, with a corrected
suffix search.
