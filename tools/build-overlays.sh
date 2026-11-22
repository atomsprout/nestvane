#!/usr/bin/env bash
# Builds the resource overlays under fixtures/overlay-* and puts them into a boot root as static
# overlays of the product partition. They change framework settings without rebuilding the framework.
# Also builds fixtures/home, the app without code that gives the home screen its clock and its layout.
# usage: build-overlays.sh <root>...   (needs the Android SDK build tools; signs with the debug key)
set -euo pipefail
cd "$(dirname "$0")/.."
tools=$(ls -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}"/build-tools/* | sort -V | tail -1)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
packages=()
for source in fixtures/overlay-*; do
    name=Nestvane$(basename "$source" | cut -d- -f2 | awk '{print toupper(substr($0,1,1)) substr($0,2)}')
    "$tools/aapt2" compile --dir "$source/res" -o "$work/$name.zip"
    "$tools/aapt2" link -o "$work/$name-unsigned.apk" --manifest "$source/AndroidManifest.xml" \
        -I "$1/system/framework/framework-res.apk" --min-sdk-version 37 --target-sdk-version 37 "$work/$name.zip"
    "$tools/zipalign" -f 4 "$work/$name-unsigned.apk" "$work/$name.apk"
    "$tools/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android "$work/$name.apk"
    for root in "$@"; do
        mkdir -p "$root/product/overlay/$name"
        cp "$work/$name.apk" "$root/product/overlay/$name/$name.apk"
    done
    packages+=("$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$source/AndroidManifest.xml" | head -1)")
    echo "$name: $(stat -f %z "$work/$name.apk") bytes"
done
# What the home screen starts with (fixtures/home) is an app of the product partition.
"$tools/aapt2" compile --dir fixtures/home/res -o "$work/NestvaneHome.zip"
"$tools/aapt2" link -o "$work/NestvaneHome-unsigned.apk" --manifest fixtures/home/AndroidManifest.xml \
    -I "$1/system/framework/framework-res.apk" --min-sdk-version 37 --target-sdk-version 37 "$work/NestvaneHome.zip"
"$tools/zipalign" -f 4 "$work/NestvaneHome-unsigned.apk" "$work/NestvaneHome.apk"
"$tools/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android "$work/NestvaneHome.apk"
for root in "$@"; do
    mkdir -p "$root/product/app/NestvaneHome"
    cp "$work/NestvaneHome.apk" "$root/product/app/NestvaneHome/NestvaneHome.apk"
done
echo "NestvaneHome: $(stat -f %z "$work/NestvaneHome.apk") bytes"
# The partition's overlay configuration is what turns an overlay on from the first boot.
for root in "$@"; do
    mkdir -p "$root/product/overlay/config"
    {
        echo '<?xml version="1.0" encoding="utf-8"?>'
        echo '<config>'
        for package in "${packages[@]}"; do echo "    <overlay package=\"$package\" mutable=\"false\" enabled=\"true\" />"; done
        echo '</config>'
    } > "$root/product/overlay/config/config.xml"
done
