#!/bin/sh
# Package the macOS 10.6 (Snow Leopard) x86_64 build into a
# self-contained zip: binary + data files + README. No installer is
# needed; unzip anywhere and run the binary from the bundle root (the
# compiled-in data path is the relative 'data' directory).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# 1. Fresh build (bakes the relative 'data' default path in).
./cross-compat/build-10.6.sh >/dev/null

VERSION="$(git rev-parse --short HEAD 2>/dev/null || echo 2023.01)"
NAME="glmark2-${VERSION}-macos-10.6-x86_64"
STAGE="dist/${NAME}"

rm -rf "$STAGE" "${STAGE}.zip"
mkdir -p "$STAGE"

# 2. Stage binary, data files and README.
cp build-106/src/glmark2-macos "$STAGE/"
cp -R data "$STAGE/data"
rm -f "$STAGE/data/meson.build"
cp cross-compat/README-10.6.md "$STAGE/README.md"

# 3. Zip (zip preserves the executable bit).
cd dist
rm -f "${NAME}.zip"
zip -q -r "${NAME}.zip" "$NAME"

echo "Packaged: dist/${NAME}.zip"
unzip -l "${NAME}.zip" | tail -3
