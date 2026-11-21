#!/usr/bin/env bash
# Prepares the nestvane-native app target: translates the guest root's code for iOS devices and stages
# the root as the app's resource. The root comes from an image built with experiments/aosp/patch.py
# (tools/prepare-init-root.py, then tools/prepare-boot-image.py --prebuilt-code).
# usage: prepare-ios-native.sh <guest root> [<tree with unpacked APEXes>...]
set -euo pipefail
cd "$(dirname "$0")/.."

root="${1:?guest root}"
NESTVANE_PAGE_SIZE=16384 tools/prepare-property-reader.sh "$root"
tools/build-fixtures.sh
python3 tools/build-native-code.py generated/native-code "$root" "${@:2}" --ios | tail -1
rm -rf generated/native-app
mkdir -p generated/native-app
cp -cR "$root" generated/native-app/android17-init-root
xcodegen generate --spec project.yml
