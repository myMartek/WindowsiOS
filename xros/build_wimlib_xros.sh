#!/bin/bash
# visionOS spike: libwim (wimlib 1.14.4, LGPLv3+) as a static library for Apple Vision Pro or its simulator.
# Usage: xros/build_wimlib_xros.sh [xros|xrsimulator]     Output: xros/deps/<platform>/{lib,include}
# The source is fetched from wimlib.net into work/downloads if it is not there, and checked against its SHA-256.
set -uo pipefail
PLATFORM="${1:-xros}"
case "$PLATFORM" in xros) SUFFIX="" ;; xrsimulator) SUFFIX="-simulator" ;; *) echo "usage: $0 [xros|xrsimulator]" >&2; exit 2 ;; esac
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARBALL="$ROOT/work/downloads/wimlib-1.14.4.tar.gz"
SRC="$ROOT/work/wimlib-src/$PLATFORM"; PREFIX="$ROOT/xros/deps/$PLATFORM"
URL="https://wimlib.net/downloads/wimlib-1.14.4.tar.gz"
SHA256="3633db2b6c8b255eb86d3bf3df3059796bd1f08e50b8c9728c7eb66662e51300"
if [ ! -s "$TARBALL" ]; then
    mkdir -p "$(dirname "$TARBALL")"
    echo "[get ] $URL"
    curl -fL --retry 3 --retry-delay 5 -o "$TARBALL.part" "$URL" && mv "$TARBALL.part" "$TARBALL" || { echo "download failed: $URL" >&2; exit 1; }
fi
[ "$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)" = "$SHA256" ] || { echo "$TARBALL is not wimlib 1.14.4 as published (SHA-256 differs)" >&2; exit 1; }
rm -rf "$SRC"; mkdir -p "$SRC" "$PREFIX"
tar -xf "$TARBALL" -C "$SRC" || exit 1
SDK="$(xcrun --sdk "$PLATFORM" --show-sdk-path)"
TARGET="-target arm64-apple-xros2.0$SUFFIX"
export CC="$(xcrun --sdk "$PLATFORM" --find clang) $TARGET"
export AR="$(xcrun --sdk "$PLATFORM" --find ar)" RANLIB="$(xcrun --sdk "$PLATFORM" --find ranlib)"
export CFLAGS="-arch arm64 -isysroot $SDK -O2" LDFLAGS="-arch arm64 -isysroot $SDK"
cd "$SRC/wimlib-1.14.4" && ./configure --host=aarch64-apple-darwin --prefix="$PREFIX" --enable-static --disable-shared \
    --without-ntfs-3g --without-fuse > "$SRC/configure.log" 2>&1 || { echo "configure failed, see $SRC/configure.log"; tail -5 "$SRC/configure.log"; exit 1; }
# Only the library: the command-line programs are not for a device.
make -j"$(sysctl -n hw.ncpu)" libwim.la > "$SRC/make.log" 2>&1 || { echo "make failed, see $SRC/make.log"; grep -E "error" "$SRC/make.log" | head -5; exit 1; }
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp .libs/libwim.a "$PREFIX/lib/" && cp include/wimlib.h "$PREFIX/include/" && cp COPYING COPYING.LGPLv3 "$PREFIX/" 
xcrun vtool -show-build "$PREFIX/lib/libwim.a" 2>/dev/null | grep -m1 -A3 "LC_BUILD_VERSION" | tr '\n' ' '; echo; ls -la "$PREFIX/lib/libwim.a"
