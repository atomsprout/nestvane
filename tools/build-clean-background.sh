#!/bin/zsh
# Builds fixtures' background cleaner (tools/clean-background/CleanBackground.java) into a jar the guest's
# app_process runs, and puts it into each guest root given: system/framework/nestvane-clean.jar.
# Needs a JDK and the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with a platform and d8.
set -euo pipefail
cd "${0:A:h}/.."
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
platform=$(ls -d "$sdk"/platforms/android-* | sort -V | tail -1)
d8=$(ls "$sdk"/build-tools/*/d8 | sort -V | tail -1)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
javac -source 17 -target 17 -Xlint:-options -cp "$platform/android.jar" -d "$work/classes" tools/clean-background/CleanBackground.java
"$d8" --release --min-api 34 --lib "$platform/android.jar" --output "$work" "$work/classes/CleanBackground.class"
(cd "$work" && zip -q -X nestvane-clean.jar classes.dex)
for root in "$@"; do
    mkdir -p "$root/system/framework"
    cp "$work/nestvane-clean.jar" "$root/system/framework/nestvane-clean.jar"
done
