#!/bin/sh
# Download third-party reference data (not committed to this repo).
set -eu
cd "$(dirname "$0")/../third_party"

# PSP NID -> function name database (GPL-2.0), from https://github.com/pspdev/psplibdoc
curl -sSfL -o PSPLibDoc.xml \
  https://raw.githubusercontent.com/pspdev/psplibdoc/master/generated-xml/PSPLibDoc.xml
echo "fetched third_party/PSPLibDoc.xml"
