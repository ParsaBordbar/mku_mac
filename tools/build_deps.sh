#!/bin/sh
# Build the two libraries the game links, as small static libraries, so that
# players need nothing from Homebrew. Maintainers run this once; the result
# (deps/) goes into the Mac app. SDL2 is zlib-licensed, this FFmpeg build is LGPL.
#
#   tools/build_deps.sh [source-cache-dir]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/build/deps-src}"
OUT="$ROOT/deps"
MIN_MACOS=12.0
SDL=SDL2-2.32.10
FFMPEG=ffmpeg-7.1.1
export MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS"
JOBS="$(sysctl -n hw.ncpu)"

mkdir -p "$SRC" "$OUT"
fetch() {   # url file sha256
    [ -f "$SRC/$2" ] || curl -sSfL -o "$SRC/$2" "$1"
    echo "$3  $SRC/$2" | shasum -a 256 -c - >/dev/null
}
fetch "https://github.com/libsdl-org/SDL/releases/download/release-${SDL#SDL2-}/$SDL.tar.gz" "$SDL.tar.gz" "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
fetch "https://ffmpeg.org/releases/$FFMPEG.tar.xz" "$FFMPEG.tar.xz" "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"

cd "$SRC"
rm -rf "$SDL" "$FFMPEG"
tar xzf "$SDL.tar.gz"
tar xf "$FFMPEG.tar.xz"

(cd "$SDL" && ./configure --prefix="$OUT" --disable-shared --enable-static >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)

# Only the ATRAC3 decoder: the one thing the game's music needs.
(cd "$FFMPEG" && ./configure --prefix="$OUT" --enable-static --disable-shared \
    --disable-everything --enable-decoder=atrac3 \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale \
    --extra-cflags="-mmacosx-version-min=$MIN_MACOS" --extra-ldflags="-mmacosx-version-min=$MIN_MACOS" >/dev/null \
    && make -j"$JOBS" >/dev/null && make install >/dev/null)

# What linking static SDL needs, without this machine's paths.
"$OUT/bin/sdl2-config" --static-libs | tr ' ' '\n' | grep -v '^-L' | sed "s|^$OUT/lib/libSDL2.a\$|-lSDL2|" | tr '\n' ' ' > "$OUT/sdl2-libs.txt"
cp "$SRC/$SDL/LICENSE.txt" "$OUT/SDL2-LICENSE.txt"
cp "$SRC/$FFMPEG/COPYING.LGPLv2.1" "$OUT/FFMPEG-LICENSE.txt"
rm -rf "$OUT/bin" "$OUT/share" "$OUT/lib/pkgconfig" "$OUT/lib/cmake" "$OUT"/lib/libSDL2main.a "$OUT"/lib/libSDL2_test.a "$OUT"/lib/*.la
echo "built $OUT:"; ls -la "$OUT/lib"
