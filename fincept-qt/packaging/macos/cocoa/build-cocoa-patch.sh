#!/bin/bash
# Build an isolated Qt 6.8.3 Cocoa plugin. The installed Qt SDK stays intact.
set -euo pipefail
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <Qt-6.8.3-prefix> <work-directory>" >&2
    exit 2
fi
qt_prefix="$(cd "$1" && pwd)"
mkdir -p "$2"
work="$(cd "$2" && pwd)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
archive="$work/qtbase-6.8.3.tar.gz"
if [ ! -f "$archive" ]; then
    curl --fail --location --retry 2 https://codeload.github.com/qt/qtbase/tar.gz/refs/tags/v6.8.3 -o "$archive.download"
    mv "$archive.download" "$archive"
fi
printf '%s  %s\n' cea5c8f2c20d9cbd684f8a402721e63b87a2886e906f6ec7e0f7e1ff69c83206 "$archive" | shasum -a 256 -c -
source_dir="$work/qtbase-6.8.3"
patch_hash=$(shasum -a 256 "$here/retain-table-cells.patch" | awk '{print $1}')
if [ ! -f "$source_dir/.fincept-cocoa-patched" ] || [ "$(cat "$source_dir/.fincept-cocoa-patched")" != "$patch_hash" ]; then
    mkdir -p "$source_dir"
    tar -xzf "$archive" -C "$source_dir" --strip-components=1 \
        qtbase-6.8.3/src/plugins/platforms/cocoa qtbase-6.8.3/LICENSES
    (cd "$source_dir" && patch -p1 < "$here/retain-table-cells.patch")
    printf '%s\n' "$patch_hash" > "$source_dir/.fincept-cocoa-patched"
fi
cmake -S "$here" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_PREFIX_PATH="$qt_prefix" -DCOCOA_SOURCE_DIR="$source_dir/src/plugins/platforms/cocoa"
cmake --build "$work/build" --parallel 4
mkdir -p "$work/plugins/platforms"
cp "$work/build/libqcocoa.dylib" "$work/plugins/platforms/"
"$work/build/cocoa_smoke" "$work/plugins"
echo "Verified plugin: $work/plugins/platforms/libqcocoa.dylib"
