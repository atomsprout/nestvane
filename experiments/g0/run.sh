#!/bin/sh
# Builds a G0 probe app, installs it on a connected iPhone and runs the named probes without a debugger.
#   G0_TEAM=<team id> experiments/g0/run.sh <device udid> <G0|G0Extended|G0Code|G0Guest> <probe>...
# G0Code embeds G0_CODE_COUNT frameworks of G0_CODE_MIB each (default 8 x 64 MiB).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
device=$1
target=$2
shift 2
out="$root/generated/g0"
frameworks="$out/frameworks-${G0_CODE_COUNT:-8}x${G0_CODE_MIB:-64}"
mkdir -p "$out/reports"

if [ "$target" = G0Code ] && [ ! -d "$frameworks" ]; then
    mkdir -p "$frameworks.tmp"
    cc -O2 -o "$out/gen-code" "$here/gen-code.c"
    index=0
    while [ "$index" -lt "${G0_CODE_COUNT:-8}" ]; do
        name=G0Code$index
        bundle="$frameworks.tmp/$name.framework"
        mkdir -p "$bundle"
        "$out/gen-code" "$out/$name.bin" "${G0_CODE_MIB:-64}" "$((index + 1))"
        cat > "$out/$name.s" <<EOF
.section __TEXT,__text,regular,pure_instructions
.globl _g0_code_begin
.globl _g0_code_end
.p2align 14
_g0_code_begin:
.incbin "$out/$name.bin"
_g0_code_end:
ret
EOF
        xcrun -sdk iphoneos clang -target arm64-apple-ios26.0 -dynamiclib \
            -install_name "@rpath/$name.framework/$name" -o "$bundle/$name" "$out/$name.s"
        cat > "$bundle/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$name</string>
<key>CFBundleIdentifier</key><string>com.everettjf.nestvane.g0code.$name</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>$name</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>MinimumOSVersion</key><string>26.0</string>
</dict></plist>
EOF
        rm "$out/$name.bin" "$out/$name.s"
        index=$((index + 1))
    done
    mv "$frameworks.tmp" "$frameworks"
fi

if [ "$target" = G0Guest ]; then
    [ -f "$out/e3/toybox-hostlr.s" ] || { echo "run experiments/g0/e3/build.sh first"; exit 1; }
    python3 "$here/e3/compare.py" --export "$out/e3/scripts.bin"
fi

cd "$here"
xcodegen generate --quiet
xcodebuild -project g0.xcodeproj -scheme "$target" -configuration Release -destination "id=$device" \
    -derivedDataPath "$out/derived" -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$G0_TEAM" G0_FRAMEWORKS="$frameworks" build > "$out/build-$target.log" 2>&1 \
    || { tail -40 "$out/build-$target.log"; exit 1; }
app="$out/derived/Build/Products/Release-iphoneos/$target.app"
bundle=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Info.plist")
du -sh "$app"
codesign -d --entitlements - "$app" 2>/dev/null | grep -E 'kernel|get-task-allow' || true
xcrun devicectl device install app --device "$device" "$app" > "$out/install-$target.log" 2>&1 \
    || { tail -20 "$out/install-$target.log"; exit 1; }

for probe in "$@"; do
    xcrun devicectl device process launch --device "$device" --terminate-existing --console ${G0_ENV:+--environment-variables "$G0_ENV"} "$bundle" "$probe" \
        > "$out/launch-$target-$probe.log" 2>&1 || echo "launch of $probe exited with $?"
    xcrun devicectl device copy from --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
        --source "Documents/$probe.txt" --destination "$out/reports/$target-$probe.txt" > /dev/null 2>&1 \
        || echo "no report for $probe"
    cat "$out/reports/$target-$probe.txt" 2>/dev/null || tail -20 "$out/launch-$target-$probe.log"
done
