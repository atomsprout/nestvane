#!/usr/bin/env bash
# Builds fixtures/webview, the app that shows a page in a WebView and logs how long each stage takes,
# into generated/nestvane-webview-test.apk. Signs with the test fixtures' key (tools/build-smoke-apk.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
build_tools="${NESTVANE_ANDROID_BUILD_TOOLS:-$sdk/build-tools/36.1.0}"
android_jar="${NESTVANE_ANDROID_JAR:-$sdk/platforms/android-37.0/android.jar}"
output="$PWD/generated/nestvane-webview-test.apk"
key="$PWD/generated/android17-smoke.p12"
[[ -f "$key" ]] || tools/build-smoke-apk.sh > /dev/null
temporary="$(mktemp -d "$PWD/generated/webview-apk.XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
mkdir "$temporary/classes"
javac --release 8 -Xlint:-options -cp "$android_jar" -d "$temporary/classes" fixtures/webview/*.java
"$build_tools/d8" --release --lib "$android_jar" --min-api 23 --output "$temporary" \
    "$temporary/classes/dev/nestvane/webview/"*.class
"$build_tools/aapt2" link -I "$android_jar" --manifest fixtures/webview/AndroidManifest.xml -o "$temporary/resources.apk"
(cd "$temporary" && zip -q resources.apk classes.dex)
"$build_tools/zipalign" -f 4 "$temporary/resources.apk" "$temporary/aligned.apk"
"$build_tools/apksigner" sign --ks "$key" --ks-pass pass:nestvane-fixture \
    --out "$temporary/signed.apk" "$temporary/aligned.apk"
mv "$temporary/signed.apk" "$output"
echo "$output"
