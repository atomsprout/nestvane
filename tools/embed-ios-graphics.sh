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
executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Info.plist")
python3 - "$app/$executable" "$backend" "$angle/libEGL.framework/libEGL" \
    "$angle/libGLESv2.framework/libGLESv2" <<'PYTHON'
import pathlib
import subprocess
import sys


def minimum_ios_version(binary):
    info = subprocess.check_output(["xcrun", "vtool", "-show-build", binary], text=True)
    for line in info.splitlines():
        fields = line.split()
        if fields[:1] == ["minos"]:
            return tuple(int(part) for part in fields[1].split("."))
    raise SystemExit(f"Cannot determine minimum OS version for {binary}")


app, *frameworks = sys.argv[1:]
minimum = minimum_ios_version(app)
for framework in frameworks:
    required = minimum_ios_version(framework)
    if required > minimum:
        name = pathlib.Path(framework).name
        required_version = ".".join(map(str, required))
        app_version = ".".join(map(str, minimum))
        raise SystemExit(f"{name} requires iOS {required_version}, but the app supports iOS {app_version}.")
PYTHON

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
        "CFBundleIdentifier": "com.atomsprout.nestvane.gfxstream",
        "CFBundleName": "NestvaneGfxstream",
        "CFBundleExecutable": "NestvaneGfxstream",
        "CFBundlePackageType": "FMWK",
        "CFBundleVersion": "1",
        "CFBundleShortVersionString": "0.1",
        "CFBundleSupportedPlatforms": ["iPhoneOS"],
        "MinimumOSVersion": "17.0",
    }, output)
PY

for name in libEGL libGLESv2; do
    ditto "$angle/$name.framework" "$frameworks/$name.framework"
    # The App Store asks every bundle for a version string, which ANGLE's build leaves out.
    /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string 1.0" "$frameworks/$name.framework/Info.plist" 2>/dev/null || true
    /usr/bin/codesign --force --sign "$identity" "$frameworks/$name.framework"
done
/usr/bin/codesign --force --sign "$identity" "$gfxstream"
/usr/bin/codesign --force --sign "$identity" --preserve-metadata=identifier,entitlements,requirements,flags "$packed"
/usr/bin/codesign --verify --deep --strict "$packed"
mv "$packed" "$output"
