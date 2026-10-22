#!/bin/sh
# Download third-party reference data (not committed to this repo).
set -eu
cd "$(dirname "$0")/../third_party"

# PSP NID -> function name database (GPL-2.0), from https://github.com/pspdev/psplibdoc
curl -sSfL -o PSPLibDoc.xml \
  https://raw.githubusercontent.com/pspdev/psplibdoc/master/generated-xml/PSPLibDoc.xml
echo "fetched third_party/PSPLibDoc.xml"

# xBRZ texture upscaler (GPL-3.0, Zenju), as vendored by PPSSPP:
# https://github.com/hrydgard/ppsspp/tree/master/ext/xbrz
mkdir -p xbrz
for f in xbrz.cpp xbrz.h config.h; do
  curl -sSfL -o "xbrz/$f" "https://raw.githubusercontent.com/hrydgard/ppsspp/master/ext/xbrz/$f"
done
echo "fetched third_party/xbrz/"

# stb_image / stb_image_write (public domain / MIT): PNG for texture dumps and packs
mkdir -p stb
for f in stb_image.h stb_image_write.h; do
  curl -sSfL -o "stb/$f" "https://raw.githubusercontent.com/nothings/stb/master/$f"
done
echo "fetched third_party/stb/"
