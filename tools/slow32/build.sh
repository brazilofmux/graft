#!/usr/bin/env bash
# build.sh: build graft-apply (libgraft's decoder + tools/slow32/apply_main.cpp)
# for SLOW-32, using an existing SLOW-32 toolchain read-only.
#
#   tools/slow32/build.sh [OUT_DIR]          (default: build/slow32)
#
# Needs, overridable by environment:
#   SLOW32_HOME    the slow-32 tree (runtime, assembler, linker)  [~/slow-32]
#   LLVM_BIN       clang/clang++/llc with the SLOW-32 backend     [~/llvm-project/build/bin]
#   SLOW32_LIBCXX  libc++/libc++abi built for SLOW-32             [~/llvm-project/build-slow32-libcxx]
#   GRAFT_DEPS_SRC zstd and BLAKE3 sources, as fetched by the     [build/fetch/_deps]
#                  `fetch` preset (cmake --preset fetch)
#
# Only the decoder side is built: zstd's decompressor, portable BLAKE3, and
# graft's decode/filter/hash code. Every step mirrors slow-32's s32c++:
# clang -> LLVM IR -> llc -> slow32asm -> s32-ld.

set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$root/build/slow32}

S32=${SLOW32_HOME:-$HOME/slow-32}
LLVM=${LLVM_BIN:-$HOME/llvm-project/build/bin}
CXXH=${SLOW32_LIBCXX:-$HOME/llvm-project/build-slow32-libcxx}
DEPS=${GRAFT_DEPS_SRC:-$root/build/fetch/_deps}
ZSTD=$DEPS/graft_zstd-src/lib
B3=$DEPS/graft_blake3-src/c

for need in "$LLVM/clang" "$LLVM/llc" "$S32/tools/assembler/slow32asm" "$S32/tools/linker/s32-ld" \
            "$S32/runtime/crt0.s32o" "$CXXH/lib/libc++.a" "$ZSTD/zstd.h" "$B3/blake3.h"; do
  [[ -e $need ]] || { echo "build.sh: missing $need (see the header of this script)" >&2; exit 1; }
done

mkdir -p "$out/obj"
target=(-target slow32-unknown-none -O2 -ffunction-sections)
common_inc=(-isystem "$S32/runtime/include")
objs=()

compile() {  # compile SRC LANG [flags...]
  local src=$1 lang=$2; shift 2
  local base; base=$(echo "${src#$root/}" | tr '/.' '__')
  local ll=$out/obj/$base.ll s=$out/obj/$base.s o=$out/obj/$base.s32o
  if [[ $lang == c ]]; then
    "$LLVM/clang" "${target[@]}" -std=c11 -S -emit-llvm "${common_inc[@]}" "$@" "$src" -o "$ll"
  else
    "$LLVM/clang++" "${target[@]}" -std=c++20 -S -emit-llvm -nostdinc++ -isystem "$CXXH/include/c++/v1" \
      "${common_inc[@]}" -fexceptions -frtti -funwind-tables "$@" "$src" -o "$ll"
  fi
  "$LLVM/llc" -mtriple=slow32-unknown-none "$ll" -o "$s"
  "$S32/tools/assembler/slow32asm" "$s" "$o" >"$o.log" 2>&1 || { cat "$o.log" >&2; exit 1; }
  objs+=("$o")
}

zflags=(-DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DZSTD_NO_INTRINSICS -DZSTD_MULTITHREAD=0 -I "$ZSTD" -I "$ZSTD/common")
for f in common/entropy_common.c common/error_private.c common/fse_decompress.c common/xxhash.c \
         common/zstd_common.c decompress/huf_decompress.c decompress/zstd_ddict.c \
         decompress/zstd_decompress.c decompress/zstd_decompress_block.c; do
  compile "$ZSTD/$f" c "${zflags[@]}"
done
bflags=(-DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 -I "$B3")
for f in blake3.c blake3_dispatch.c blake3_portable.c; do compile "$B3/$f" c "${bflags[@]}"; done
gflags=(-I "$root/include" -I "$root/src" -I "$ZSTD" -I "$B3")
for f in src/decode.cpp src/filter.cpp src/hash.cpp src/io.cpp src/status.cpp tools/slow32/apply_main.cpp; do
  compile "$root/$f" cxx "${gflags[@]}"
done

"$S32/tools/linker/s32-ld" -o "$out/graft-apply.s32x" --mmio 64K --heap-size 64M \
  "$S32/runtime/crt0.s32o" "${objs[@]}" "$CXXH/lib/libc++.a" "$CXXH/lib/libc++abi.a" \
  "$S32/runtime/libc_mmio.s32a" "$S32/runtime/libs32.s32a" >"$out/ld.log" 2>&1 \
  || { cat "$out/ld.log" >&2; exit 1; }
echo "$out/graft-apply.s32x"
