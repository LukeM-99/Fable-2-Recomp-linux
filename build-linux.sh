#!/usr/bin/env bash
# Build the Fable 2 ReXGlue project on Linux (Vulkan backend).
#
# Toolchain lives under out/tooling (no root needed; fetch it with
# tools/setup-linux-toolchain.sh):
#   LLVM-20.1.8-Linux-X64/  official LLVM release (clang, lld, libc++ headers)
#   libcxx/                 libc++/libc++abi 20.1.8 rebuilt with
#                           LIBCXXABI_USE_LLVM_UNWINDER=OFF (see below)
#   bin/ninja               static ninja
#   sysroot/                headers/.so links unpacked from distro -dev .debs
#                           (alsa, pulse, pipewire, xcb, xss, udev, decor)
# The SDK is built from source (thirdparty/rexglue-sdk), including the
# rexglue codegen tool, which needs default.xex in the project root.
#
# Usage:
#   ./build-linux.sh                 build fable_2 (Release)
#   ./build-linux.sh -debug [target] build as Debug
#   ./build-linux.sh <target>        build any other CMake target
#   ./build-linux.sh -clean          wipe the build dir for the config
# Env: NINJA_ARGS (e.g. "-j4"), CMAKE_EXTRA_ARGS (extra -D options)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
T="$ROOT/out/tooling"
LLVM="$T/LLVM-20.1.8-Linux-X64"
SYSROOT="$T/sysroot/usr"

CONFIG=Release
TARGET=fable_2
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        -debug|-d) CONFIG=Debug ;;
        -clean|-c) CLEAN=1 ;;
        *) TARGET="$arg" ;;
    esac
done
BUILD="$ROOT/out/build/linux-${CONFIG,,}"

if [ "$CLEAN" = 1 ]; then
    rm -rf "$BUILD" && echo "Cleaned $BUILD"
    exit 0
fi

[ -x "$LLVM/bin/clang++" ] || { echo "Missing $LLVM (run tools/setup-linux-toolchain.sh)" >&2; exit 1; }
[ -x "$T/bin/ninja" ] || { echo "Missing $T/bin/ninja" >&2; exit 1; }
[ -f "$ROOT/thirdparty/rexglue-sdk/CMakeLists.txt" ] || \
    { echo "Missing thirdparty/rexglue-sdk (git clone --recursive https://github.com/himdo/rexglue-sdk.git)" >&2; exit 1; }
# Linux SDK fixes (thirdparty/rexglue-sdk-linux.patch, made with `git diff` in
# the submodule): apply once to a fresh checkout.
SDK="$ROOT/thirdparty/rexglue-sdk"
if git -C "$SDK" apply --check "$ROOT/thirdparty/rexglue-sdk-linux.patch" 2>/dev/null; then
    git -C "$SDK" apply "$ROOT/thirdparty/rexglue-sdk-linux.patch" && echo "Applied rexglue-sdk-linux.patch"
elif ! git -C "$SDK" apply -R --check "$ROOT/thirdparty/rexglue-sdk-linux.patch" 2>/dev/null; then
    echo "thirdparty/rexglue-sdk-linux.patch does not apply to the SDK checkout" >&2; exit 1
fi
[ -f "$ROOT/default.xex" ] || { echo "Missing default.xex in project root (codegen input)" >&2; exit 1; }

export PATH="$T/bin:$LLVM/bin:$PATH"
# libc++ comes from $T/libcxx, NOT the LLVM release: the release libc++abi
# uses LLVM libunwind, while glibc's pthread_exit unwinds with libgcc_s; the
# mix crashes on every thread exit (the SDK's SIGSEGV handler then spins
# forever). Our build uses libgcc_s only. Built tools (rexglue codegen) need
# it at run time too.
LIBCXX="$T/libcxx/lib"
[ -f "$LIBCXX/libc++.so.1" ] || { echo "Missing $LIBCXX (libc++ built with libgcc_s unwinder)" >&2; exit 1; }
export LD_LIBRARY_PATH="$LIBCXX${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$SYSROOT/lib/x86_64-linux-gnu/pkgconfig:$SYSROOT/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

CFLAGS="-march=x86-64-v2 -idirafter $SYSROOT/include"
LDFLAGS="-fuse-ld=lld -stdlib=libc++ -L$LIBCXX -L$SYSROOT/lib/x86_64-linux-gnu"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE="$CONFIG" \
        -DCMAKE_MAKE_PROGRAM="$T/bin/ninja" \
        -DCMAKE_C_COMPILER="$LLVM/bin/clang" \
        -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" \
        -DCMAKE_C_FLAGS="$CFLAGS" \
        -DCMAKE_CXX_FLAGS="$CFLAGS -stdlib=libc++" \
        -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
        -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
        -DCMAKE_MODULE_LINKER_FLAGS="$LDFLAGS" \
        -DCMAKE_PREFIX_PATH="$SYSROOT" \
        -DREXSDK_DIR="$ROOT/thirdparty/rexglue-sdk" \
        -DREXGLUE_ENABLE_TRACY=OFF \
        -DREXGLUE_BUILD_TESTS=OFF \
        -DFABLE2_FORCE_REMOTE_CONTROL=OFF \
        ${CMAKE_EXTRA_ARGS:-}
# generated/default/sources.cmake is only read at configure time, so on a
# fresh checkout run codegen first and reconfigure to pick up its output.
if [ ! -f "$ROOT/generated/default/sources.cmake" ]; then
    cmake --build "$BUILD" --target fable_2_codegen -- ${NINJA_ARGS:-}
    cmake -S "$ROOT" -B "$BUILD"
fi
cmake --build "$BUILD" --target "$TARGET" -- ${NINJA_ARGS:-}
