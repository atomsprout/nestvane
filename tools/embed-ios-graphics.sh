#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    printf 'Usage: %s input.app gfxstream.dylib angle-build-directory output.app\n' "$0" >&2
    exit 2
fi

app=$1
backend=$2
angle=$3
output=$4
if [[ ! -d "$app" || ! -f "$backend" || ! -f "$angle/libEGL.framework/libEGL" ||
      ! -f "$angle/libGLESv2.framework/libGLESv2" || "$output" != *.app || -e "$output" ]]; then
    printf 'Provide a signed app, built renderer frameworks and a new output app path.\n' >&2
    exit 2
fi
identity=$(/usr/bin/codesign -dv --verbose=4 "$app" 2>&1 | sed -n 's/^Authority=\(Apple Development: .*\)$/\1/p' | head -1)
if [[ -z "$identity" ]]; then
    printf 'The input must use an Apple Development signature available on this Mac.\n' >&2
    exit 2
fi

tmp=$(mktemp -d "$(dirname "$output")/.nestvane-graphics.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
packed="$tmp/$(basename "$output")"
ditto "$app" "$packed"
frameworks="$packed/Frameworks"
gfxstream="$frameworks/NestvaneGfxstream.framework"
mkdir -p "$gfxstream"
cp "$backend" "$gfxstream/NestvaneGfxstream"
install_name_tool -id @rpath/NestvaneGfxstream.framework/NestvaneGfxstream "$gfxstream/NestvaneGfxstream"
python3 - "$gfxstream/Info.plist" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "wb") as output:
    plistlib.dump({
        "CFBundleIdentifier": "com.everettjf.nestvane.gfxstream",
        "CFBundleName": "NestvaneGfxstream",
        "CFBundleExecutable": "NestvaneGfxstream",
        "CFBundlePackageType": "FMWK",
        "CFBundleVersion": "1",
        "CFBundleShortVersionString": "0.1",
        "CFBundleSupportedPlatforms": ["iPhoneOS"],
    }, output)
PY

for name in libEGL libGLESv2; do
    ditto "$angle/$name.framework" "$frameworks/$name.framework"
    /usr/bin/codesign --force --sign "$identity" "$frameworks/$name.framework"
done
/usr/bin/codesign --force --sign "$identity" "$gfxstream"
/usr/bin/codesign --force --sign "$identity" --preserve-metadata=identifier,entitlements,requirements,flags "$packed"
/usr/bin/codesign --verify --deep --strict "$packed"
mv "$packed" "$output"
