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
if [ "$("$qt_prefix/bin/qmake" -query QT_VERSION)" = "6.8.3" ] && [ -z "${FINCEPT_COCOA_PLUGIN:-}" ]; then
    echo "Qt 6.8.3 requires the Cocoa accessibility fix. Build packaging/macos/cocoa/build-cocoa-patch.sh and set FINCEPT_COCOA_PLUGIN." >&2
    exit 1
fi
if [ -n "${FINCEPT_COCOA_PLUGIN:-}" ] && [ ! -f "$FINCEPT_COCOA_PLUGIN" ]; then
    echo "Cocoa plugin does not exist: $FINCEPT_COCOA_PLUGIN" >&2
    exit 1
fi
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
# Optional isolated Qt 6.8.3 accessibility ownership fix. Replace AFTER
# macdeployqt so its -always-overwrite cannot put the original plugin back.
if [ -n "${FINCEPT_COCOA_PLUGIN:-}" ]; then
    cp "$FINCEPT_COCOA_PLUGIN" "$package_dir/Contents/PlugIns/platforms/libqcocoa.dylib"
    # Remove SDK build rpaths: the packaged plugin must use bundled frameworks.
    while IFS= read -r rpath; do
        case "$rpath" in
            /*) /usr/bin/install_name_tool -delete_rpath "$rpath" "$package_dir/Contents/PlugIns/platforms/libqcocoa.dylib" ;;
        esac
    done < <(/usr/bin/otool -l "$package_dir/Contents/PlugIns/platforms/libqcocoa.dylib" | \
        awk '/cmd LC_RPATH/ { found=1; next } found && $1 == "path" { print $2; found=0 }')
    mkdir -p "$package_dir/Contents/Resources/qt-cocoa-fix"
    cp "$(dirname "$0")/cocoa/retain-table-cells.patch" "$(dirname "$0")/cocoa/SOURCE-NOTICE.txt" "$package_dir/Contents/Resources/qt-cocoa-fix/"
    cp -R "$(dirname "$0")/cocoa/LICENSES" "$package_dir/Contents/Resources/qt-cocoa-fix/"
fi
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
