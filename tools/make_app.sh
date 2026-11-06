#!/bin/sh
# Build dist/MK Unchained.app and a zip of it, for Apple Silicon Macs.
# Holds our sources, the helper libraries and no game code: `git ls-files`
# decides what goes in, and the last step refuses anything game-derived.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f deps/sdl2-libs.txt ] || { echo "run tools/build_deps.sh first"; exit 1; }
[ -f third_party/xbrz/xbrz.cpp ] || sh tools/fetch_third_party.sh >/dev/null
APP="dist/MK Unchained.app"
rm -rf dist
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/game"
cp launcher/macos/Info.plist "$APP/Contents/Info.plist"
swiftc -O -parse-as-library -target arm64-apple-macos13 launcher/macos/Launcher.swift -o "$APP/Contents/MacOS/MK Unchained"

GAME="$APP/Contents/Resources/game"
git ls-files -- mku-play Makefile config runtime tools third_party/README.md | while read -r f; do
    mkdir -p "$GAME/$(dirname "$f")"
    cp -p "$f" "$GAME/$f"
done
cp -Rp third_party/PSPLibDoc.xml third_party/xbrz third_party/stb "$GAME/third_party/"
cp -Rp deps "$GAME/deps"
cp README.md "$APP/Contents/Resources/README.md"
mkdir -p "$APP/Contents/Resources/screens"
cp docs/screens/*.jpg "$APP/Contents/Resources/screens/"
cat > "$APP/Contents/Resources/THIRD-PARTY.txt" <<'NOTES'
Bundled, in game/third_party and game/deps:
- SDL2 (zlib): https://libsdl.org. Licence: game/deps/SDL2-LICENSE.txt
- FFmpeg 7.1.1, ATRAC3 decoder only (LGPL-2.1): https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
  Licence: game/deps/FFMPEG-LICENSE.txt. tools/build_deps.sh rebuilds it; the game is linked
  on your computer, so you can replace these libraries.
- xBRZ (GPL-3.0), by Zenju, as vendored by PPSSPP: game/third_party/xbrz
- stb_image, stb_image_write (public domain / MIT): game/third_party/stb
- PSPLibDoc.xml (GPL-2.0): https://github.com/pspdev/psplibdoc
NOTES
codesign --force --deep -s - "$APP"   # ad-hoc: Apple Silicon requires a signature

# Nothing may point back at this machine...
if grep -rIl "$ROOT" "$APP/Contents/Resources/game/deps"; then
    echo "absolute paths from this machine in deps: refusing"; exit 1
fi
# ...and nothing derived from the game may be in here.
if find "$APP" \( -name '*.iso' -o -name '*.elf' -o -name 'image.bin' -o -name 'funcs.h' -o -name 'mku' -o -name 'functions.csv' -o -path '*/build/*' -o -path '*/recomp/*' \) | grep .; then
    echo "game-derived files in the app: refusing"; exit 1
fi
ditto -c -k --sequesterRsrc --keepParent "$APP" "dist/MK-Unchained-mac.zip"
du -sh "$APP" dist/MK-Unchained-mac.zip
