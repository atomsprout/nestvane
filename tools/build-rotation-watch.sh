#!/bin/zsh
# Builds the rotation watcher (tools/rotation-watch/RotationWatch.java) into a jar the guest's
# app_process runs, and puts it into each guest root given: system/framework/nestvane-rotation.jar.
# Needs a JDK and the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with a platform and d8.
# The watcher uses three classes of the framework that the SDK does not offer; it is compiled against
# stand-ins for them (tools/rotation-watch/stubs), which stay out of the jar.
set -euo pipefail
cd "${0:A:h}/.."
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
platform=$(ls -d "$sdk"/platforms/android-* | sort -V | tail -1)
d8=$(ls "$sdk"/build-tools/*/d8 | sort -V | tail -1)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
javac -source 17 -target 17 -Xlint:-options -cp "$platform/android.jar" -d "$work/stubs" $(find tools/rotation-watch/stubs -name '*.java')
javac -source 17 -target 17 -Xlint:-options -cp "$work/stubs:$platform/android.jar" -d "$work/classes" tools/rotation-watch/RotationWatch.java
"$d8" --release --min-api 34 --lib "$platform/android.jar" --classpath "$work/stubs" --output "$work" "$work"/classes/*.class
(cd "$work" && zip -q -X nestvane-rotation.jar classes.dex)
for root in "$@"; do
    mkdir -p "$root/system/framework"
    cp "$work/nestvane-rotation.jar" "$root/system/framework/nestvane-rotation.jar"
done
