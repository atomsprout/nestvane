#!/bin/sh
# Packs the iOS translation libraries (tools/build-native-code.py ... --ios) as frameworks that the
# G0Code app embeds, for probe e6:
#   experiments/g0/native-frameworks.sh
#   G0_TEAM=<team id> G0_FRAMEWORKS_DIR=generated/g0/frameworks-native experiments/g0/run.sh <device udid> G0Code e6
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/generated/g0/frameworks-native"
rm -rf "$out"
mkdir -p "$out"
for library in "$root"/generated/native-code/ios/lib/libnative-*.dylib; do
    number=$(basename "$library" .dylib | sed 's/libnative-//')
    name=G0CodeNative$number
    bundle="$out/$name.framework"
    mkdir -p "$bundle"
    cp "$library" "$bundle/$name"
    install_name_tool -id "@rpath/$name.framework/$name" "$bundle/$name" 2>/dev/null
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
done
du -sh "$out"
