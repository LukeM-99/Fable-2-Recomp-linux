#!/usr/bin/env bash
# Package the Linux build (./build-linux.sh) as Fable2-Recompiled-x86_64.AppImage.
# Game content is NOT bundled: AppRun points the game at a writable folder
# (see packaging/linux/AppRun) and can extract default.xex + data/ from an ISO.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-release}"
BUILD="$ROOT/out/build/linux-$CONFIG"
LIBCXX="$ROOT/out/tooling/libcxx/lib"
APPDIR="$ROOT/out/appimage/AppDir"
OUT="$ROOT/out/appimage/Fable2-Recompiled-x86_64.AppImage"

[ -x "$BUILD/fable_2" ] || { echo "Build first: ./build-linux.sh" >&2; exit 1; }

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/fable2/lua"
cp "$BUILD/fable_2" "$BUILD/fable2_launcher" "$APPDIR/usr/bin/"
# Shared runtime + runtime-loaded GPU plugin (found via $ORIGIN rpath).
cp -P "$BUILD"/*.so* "$APPDIR/usr/bin/"
cp "$BUILD/fable2_build.json" "$ROOT/assets/app-icon.png" "$APPDIR/usr/bin/"
# libc++ (libgcc_s-unwinder build, see build-linux.sh; not on most distros).
cp -L "$LIBCXX/libc++.so.1" "$LIBCXX/libc++abi.so.1" "$APPDIR/usr/bin/"
cp -L "$(/sbin/ldconfig -p | awk '/libatomic.so.1 .*x86-64/{print $NF; exit}')" "$APPDIR/usr/bin/"
cp "$ROOT/tools/gdfx_extract.py" "$APPDIR/usr/bin/"
cp "$ROOT"/src/lua/*.lua "$APPDIR/usr/share/fable2/lua/"

cp "$ROOT/packaging/linux/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cp "$ROOT/packaging/linux/fable2-recomp.desktop" "$APPDIR/"
cp "$ROOT/assets/app-icon.png" "$APPDIR/fable2-recomp.png"

# Every bundled ELF must resolve without the build tree.
missing="$(cd "$APPDIR/usr/bin" && for f in fable_2 fable2_launcher *.so*; do
    LD_LIBRARY_PATH=. ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|$f: |"; done || true)"
if [ -n "$missing" ]; then echo "Unresolved libraries:"$'\n'"$missing" >&2; exit 1; fi

# appimagetool needs desktop-file-validate (out/tooling/bin, see setup script).
PATH="$ROOT/out/tooling/bin:$PATH" APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$ROOT/out/tooling/bin/appimagetool" \
    --no-appstream "$APPDIR" "$OUT"
echo "Built $OUT"
