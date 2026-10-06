#!/bin/bash
# Package an already-built local edition as a self-contained macOS app.
set -euo pipefail
if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <build-directory> <Qt-prefix> <destination.app>" >&2
    exit 2
fi
build_dir="$(cd "$1" && pwd)"
qt_prefix="$(cd "$2" && pwd)"
destination="$3"
source_app="$build_dir/FinceptTerminal.app"
identifier=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$source_app/Contents/Info.plist")
if [ "$identifier" != "dev.aric.fincept.local" ]; then
    echo "Expected a local edition build; refusing to package an official build." >&2
    exit 1
fi
if [ -e "$destination" ]; then
    echo "Destination already exists: $destination. Choose a new path to preserve the existing app." >&2
    exit 1
fi
mkdir -p "$(dirname "$destination")"
package_dir="${destination%.app}.packaging.$$.app"
trap 'rm -rf "$package_dir"' EXIT
/usr/bin/ditto "$source_app" "$package_dir"
"$qt_prefix/bin/macdeployqt" "$package_dir" -no-strip -always-overwrite \
    -libpath="$build_dir/_deps/qgeoview-build/lib"
# Only SQLite is required. Other SQL drivers can depend on absent external libs.
rm -f "$package_dir/Contents/PlugIns/sqldrivers/libqsqlodbc.dylib" \
      "$package_dir/Contents/PlugIns/sqldrivers/libqsqlpsql.dylib" \
      "$package_dir/Contents/PlugIns/sqldrivers/libqsqlmimer.dylib"
# Match the minimum macOS version to the dependencies actually bundled. Local
# Homebrew OpenSSL can require a newer OS than the application's CMake target.
minimum_os=$({
    /usr/bin/vtool -show-build "$package_dir/Contents/MacOS/FinceptTerminal"
    for dependency in "$package_dir/Contents/Frameworks/libssl.3.dylib" "$package_dir/Contents/Frameworks/libcrypto.3.dylib"; do
        /usr/bin/vtool -show-build "$dependency"
    done
} | awk '$1 == "minos" { print $2 }' | sort -t . -k1,1n -k2,2n -k3,3n | tail -1)
if [ -n "$minimum_os" ]; then
    /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $minimum_os" "$package_dir/Contents/Info.plist" 2>/dev/null || \
        /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $minimum_os" "$package_dir/Contents/Info.plist"
fi
/usr/bin/codesign --force --deep --sign - "$package_dir"
/usr/bin/codesign --verify --deep --strict "$package_dir"
mv "$package_dir" "$destination"
echo "Packaged: $destination"
