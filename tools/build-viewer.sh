#!/bin/zsh
# Builds fixtures/viewer, the app that opens pictures, videos and sounds from a file manager (the
# image has no gallery and no player), and puts it into each guest root given: system/app/NestvaneViewer.
# Signed with AOSP's public test key for apps, which tools/build-phone-permissions.sh also fetches.
# Needs the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with the android-37.0 platform.
set -euo pipefail
cd "${0:A:h}/.."
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
tools=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
android_jar=$sdk/platforms/android-37.0/android.jar
keys=generated/aosp-test-keys
mkdir -p $keys
for file in testkey.pk8 testkey.x509.pem; do
    [[ -s $keys/$file ]] || curl -s -L --max-time 60 \
        "https://android.googlesource.com/platform/build/+/refs/heads/main/target/product/security/$file?format=TEXT" | base64 -d > $keys/$file
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/classes"
javac --release 17 -Xlint:-options -cp "$android_jar" -d "$work/classes" fixtures/viewer/src/dev/nestvane/viewer/*.java
"$tools/d8" --release --lib "$android_jar" --min-api 36 --output "$work" $(find "$work/classes" -name '*.class')
"$tools/aapt2" link -I "$android_jar" --manifest fixtures/viewer/AndroidManifest.xml -o "$work/unsigned.apk"
(cd "$work" && zip -q unsigned.apk classes.dex)
"$tools/zipalign" -f -p 4 "$work/unsigned.apk" "$work/NestvaneViewer.apk"
"$tools/apksigner" sign --key $keys/testkey.pk8 --cert $keys/testkey.x509.pem "$work/NestvaneViewer.apk"
for root in "$@"; do
    mkdir -p "$root/system/app/NestvaneViewer"
    cp "$work/NestvaneViewer.apk" "$root/system/app/NestvaneViewer/NestvaneViewer.apk"
done
echo "NestvaneViewer: $(stat -f %z "$work/NestvaneViewer.apk") bytes"
