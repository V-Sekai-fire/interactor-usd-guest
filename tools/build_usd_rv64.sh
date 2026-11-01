#!/usr/bin/env bash
# Build OpenUSD 26.05 (the org's fork, branch riscv64-sandbox) and oneTBB 2021.12.0 static for
# riscv64, then usd.elf against them. Recreates the gates/0g-openusd build, which was never
# committed and was lost with the desk it ran on.
#
#   tools/build_usd_rv64.sh <weft root> <out dir>
#
# <weft root> is a manifest-layout checkout holding 5-repository/riscv64-sysroot,
# 2-contract/guest-runtime, 2-contract/guest-common and 3-interactor/usd-guest. <out dir> gets
# src/ (OpenUSD and oneTBB checkouts), inst/ (oneTBB installed), usd/ (the OpenUSD build tree
# usd.cmake reads) and elf/ (the guest build). Needs clang, lld, cmake, ninja and git.
set -euo pipefail
WEFT=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd)
TC="$WEFT/5-repository/riscv64-sysroot/toolchain.cmake"
USD_REF=7b709ed624bed1a25dd20e4a1875595bc77ac7c3 # dev/sdf-text-writer-no-printf: riscv64-sandbox df3f6b4a + attribute values written without printf
TBB_REF=v2021.12.0
J=${J:-$(nproc)}
# Through CFLAGS/CXXFLAGS, not -DCMAKE_CXX_FLAGS: that replaces the toolchain file's
# CMAKE_CXX_FLAGS_INIT, whose -isystem paths are where <type_traits> comes from.
# The prefix maps keep the build machine's paths out of the ELF (OpenUSD's TF_ messages name
# their source files).
MAP="-ffile-prefix-map=$OUT/src/usd/=openusd/ -ffile-prefix-map=$OUT/src/oneTBB/=onetbb/"
export CFLAGS="-march=rv64gc -mabi=lp64d $MAP" CXXFLAGS="-march=rv64gc -mabi=lp64d -DTBB_USE_EXCEPTIONS=1 $MAP"
mkdir -p "$OUT/src"

fetch() { # url ref dir
	[ -d "$3/.git" ] || git init -q "$3"
	git -C "$3" fetch -q --depth 1 "$1" "$2" && git -C "$3" checkout -q FETCH_HEAD
	echo "$3 at $(git -C "$3" rev-parse HEAD)"
}
fetch https://github.com/uxlfoundation/oneTBB.git "$TBB_REF" "$OUT/src/oneTBB"
fetch https://github.com/V-Sekai-fire/OpenUSD-fork.git "$USD_REF" "$OUT/src/usd"

if [ ! -f "$OUT/inst/lib/libtbb.a" ]; then
	cmake -S "$OUT/src/oneTBB" -B "$OUT/tbb" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT/inst" -DBUILD_SHARED_LIBS=OFF \
		-DTBB_TEST=OFF -DTBB_EXAMPLES=OFF -DTBB_STRICT=OFF -DTBBMALLOC_BUILD=OFF
	ninja -C "$OUT/tbb" -j"$J" install
fi

if [ ! -f "$OUT/usd/pxr/usd/usdSkel/libusd_usdSkel.a" ]; then
	cmake -S "$OUT/src/usd" -B "$OUT/usd" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DPXR_BUILD_MONOLITHIC=OFF \
		-DPXR_ENABLE_PYTHON_SUPPORT=OFF -DPXR_BUILD_IMAGING=OFF -DPXR_BUILD_USD_IMAGING=OFF \
		-DPXR_BUILD_USDVIEW=OFF -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF \
		-DPXR_BUILD_TUTORIALS=OFF -DPXR_BUILD_USD_TOOLS=OFF -DPXR_BUILD_HTML_DOCUMENTATION=OFF \
		-DPXR_ENABLE_MATERIALX_SUPPORT=OFF -DPXR_ENABLE_OPENVDB_SUPPORT=OFF \
		-DPXR_ENABLE_PTEX_SUPPORT=OFF -DPXR_BUILD_ALEMBIC_PLUGIN=OFF -DPXR_BUILD_DRACO_PLUGIN=OFF \
		-DPXR_ENABLE_OSL_SUPPORT=OFF -DPXR_PREFER_SAFETY_OVER_SPEED=ON \
		-DTBB_ROOT="$OUT/inst" -DTBB_DIR="$OUT/inst/lib/cmake/TBB"
	ninja -C "$OUT/usd" -j"$J"
fi
# usd.cmake reads the generated headers from <build>/include and oneTBB's from inst/include.
[ -d "$OUT/usd/include/pxr" ] || { echo "error: no $OUT/usd/include/pxr" >&2; exit 1; }

cmake -S "$WEFT/3-interactor/usd-guest/tools/standalone" -B "$OUT/elf" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DWEFT_ROOT="$WEFT" \
	-DUSD_RV64_DIR="$OUT" -DUSD_RV64_SRC="$OUT/src/usd" -DDOUBLE_PRECISION=ON -DSANDBOX_RISCV_EXT_V=OFF
ninja -C "$OUT/elf" -j"$J" usd
E="$WEFT/3-interactor/usd-guest/tools/standalone/usd.elf"; ls -la "$E"
sha256sum "$E"
