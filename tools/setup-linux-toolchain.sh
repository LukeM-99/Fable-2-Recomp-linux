#!/usr/bin/env bash
# Fetch the Linux build toolchain into out/tooling (no root needed), as
# expected by build-linux.sh and make-appimage.sh:
#   LLVM-20.1.8-Linux-X64/  official LLVM release (clang, lld, libc++ headers)
#   libcxx/                 libc++/libc++abi 20.1.8 rebuilt with
#                           LIBCXXABI_USE_LLVM_UNWINDER=OFF (the release
#                           libc++abi + glibc pthread_exit crash on thread exit)
#   bin/ninja, bin/appimagetool
#   sysroot/                -dev package headers unpacked with apt-get download
#                           (Debian/Ubuntu/Devuan). On other distros install the
#                           equivalent -devel packages system-wide instead and
#                           pass --no-sysroot.
# Host requirements: cmake >= 3.25, python3, curl, tar, xz, git.
# Usage: tools/setup-linux-toolchain.sh [--no-sysroot]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/out/tooling"
LLVM_VER=20.1.8
SYSROOT=1
[ "${1:-}" = "--no-sysroot" ] && SYSROOT=0
mkdir -p "$T/bin"
cd "$T"

if [ ! -x "$T/LLVM-$LLVM_VER-Linux-X64/bin/clang++" ]; then
    echo "== LLVM $LLVM_VER"
    curl -fL -o llvm.tar.xz "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.xz"
    tar xf llvm.tar.xz && rm llvm.tar.xz
fi

if [ ! -x "$T/bin/ninja" ]; then
    echo "== ninja"
    curl -fL -o ninja.zip https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip
    python3 -I -m zipfile -e ninja.zip bin && chmod +x bin/ninja && rm ninja.zip
fi

if [ ! -x "$T/bin/appimagetool" ]; then
    echo "== appimagetool"
    curl -fL -o bin/appimagetool https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage
    chmod +x bin/appimagetool
fi

# appimagetool refuses to run without desktop-file-validate.
if ! command -v desktop-file-validate >/dev/null && [ ! -x "$T/bin/desktop-file-validate" ]; then
    echo "== desktop-file-validate"
    mkdir -p debs dfu
    (cd debs && apt-get download desktop-file-utils)
    dpkg-deb -x debs/desktop-file-utils_*.deb dfu
    cp dfu/usr/bin/desktop-file-validate bin/ && rm -rf dfu
fi

if [ ! -f "$T/libcxx/lib/libc++.so.1" ]; then
    echo "== libc++ $LLVM_VER (libgcc_s unwinder)"
    SRC="llvm-project-$LLVM_VER.src"
    if [ ! -d "$SRC/libc" ]; then
        curl -fL -o llvm-src.tar.xz "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$SRC.tar.xz"
        tar xf llvm-src.tar.xz "$SRC/runtimes" "$SRC/libcxx" "$SRC/libcxxabi" "$SRC/libunwind" \
            "$SRC/libc" "$SRC/cmake" "$SRC/llvm/cmake" "$SRC/llvm/utils" "$SRC/third-party"
        rm llvm-src.tar.xz
    fi
    L="$T/LLVM-$LLVM_VER-Linux-X64/bin"
    rm -rf build-libcxx
    cmake -S "$SRC/runtimes" -B build-libcxx -G Ninja -DCMAKE_MAKE_PROGRAM="$T/bin/ninja" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$L/clang" -DCMAKE_CXX_COMPILER="$L/clang++" \
        -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DCMAKE_INSTALL_PREFIX="$T/libcxx"
    "$T/bin/ninja" -C build-libcxx install-cxx install-cxxabi
fi

if [ "$SYSROOT" = 1 ] && [ ! -d "$T/sysroot/usr/include" ]; then
    echo "== sysroot (-dev headers)"
    mkdir -p debs sysroot
    (cd debs && apt-get download libasound2-dev libpulse-dev libx11-xcb-dev libxss-dev \
        libudev-dev libdecor-0-dev libpipewire-0.3-dev libspa-0.2-dev libxtst-dev)
    for d in debs/*.deb; do dpkg-deb -x "$d" sysroot; done
    # The .pc files use /usr paths (prefix=, libdir=, ...); point them at the sysroot.
    sed -i "s|^\([a-z_]*\)=/usr|\1=$T/sysroot/usr|"sysroot/usr/lib/x86_64-linux-gnu/pkgconfig/*.pc sysroot/usr/share/pkgconfig/*.pc 2>/dev/null || true
    # The -dev .so links are relative to runtime libs the debs don't carry;
    # point them at the host's copies.
    for l in sysroot/usr/lib/x86_64-linux-gnu/*.so; do
        [ -e "$l" ] || ln -sf "/usr/lib/x86_64-linux-gnu/$(readlink "$l")" "$l"
    done
fi

echo "Toolchain ready in $T. Next: put default.xex in $ROOT and run ./build-linux.sh"
