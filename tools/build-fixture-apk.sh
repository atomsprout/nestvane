#!/usr/bin/env bash
# Builds one of the small test apps in fixtures/<name> (package dev.nestvane.<name>) into
# generated/nestvane-<name>-test.apk, signed with the test fixtures' key (tools/build-smoke-apk.sh):
#
#   tools/build-fixture-apk.sh webview   shows a page in a WebView and logs how long each stage takes
#   tools/build-fixture-apk.sh root      asks for root with su and logs what it got
#   tools/build-fixture-apk.sh mic       records three seconds and logs how loud it was
set -euo pipefail
cd "$(dirname "$0")/.."
name="${1:?usage: build-fixture-apk.sh <fixture name>}"

sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
build_tools="${NESTVANE_ANDROID_BUILD_TOOLS:-$sdk/build-tools/36.1.0}"
android_jar="${NESTVANE_ANDROID_JAR:-$sdk/platforms/android-37.0/android.jar}"
output="$PWD/generated/nestvane-$name-test.apk"
key="$PWD/generated/android17-smoke.p12"
[[ -f "$key" ]] || tools/build-smoke-apk.sh > /dev/null
temporary="$(mktemp -d "$PWD/generated/$name-apk.XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
mkdir "$temporary/classes"
javac --release 8 -Xlint:-options -cp "$android_jar" -d "$temporary/classes" fixtures/$name/*.java
"$build_tools/d8" --release --lib "$android_jar" --min-api 23 --output "$temporary" \
    "$temporary/classes/dev/nestvane/$name/"*.class
"$build_tools/aapt2" link -I "$android_jar" --manifest fixtures/$name/AndroidManifest.xml -o "$temporary/resources.apk"
(cd "$temporary" && zip -q resources.apk classes.dex)
"$build_tools/zipalign" -f 4 "$temporary/resources.apk" "$temporary/aligned.apk"
"$build_tools/apksigner" sign --ks "$key" --ks-pass pass:nestvane-fixture \
    --out "$temporary/signed.apk" "$temporary/aligned.apk"
mv "$temporary/signed.apk" "$output"
echo "$output"
