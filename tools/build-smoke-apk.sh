#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
build_tools="${NESTVANE_ANDROID_BUILD_TOOLS:-$sdk/build-tools/36.1.0}"
android_jar="${NESTVANE_ANDROID_JAR:-$sdk/platforms/android-37.0/android.jar}"
output="$PWD/generated/android17-smoke.apk"
key="$PWD/generated/android17-smoke.p12"
mkdir -p generated
temporary="$(mktemp -d "$PWD/generated/smoke-apk.XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
mkdir "$temporary/classes"
javac --release 8 -Xlint:-options -cp "$android_jar" -d "$temporary/classes" fixtures/apk/*.java
"$build_tools/d8" --release --lib "$android_jar" --min-api 23 --output "$temporary" \
    "$temporary/classes/dev/nestvane/smoke/"*.class
"$build_tools/aapt2" link -I "$android_jar" --manifest fixtures/apk/AndroidManifest.xml -o "$temporary/resources.apk"
(cd "$temporary" && zip -q resources.apk classes.dex)
"$build_tools/zipalign" -f 4 "$temporary/resources.apk" "$temporary/aligned.apk"
if [[ ! -f "$key" ]]; then
    (umask 077; keytool -genkeypair -storetype PKCS12 -keystore "$temporary/smoke.p12" -alias nestvane-fixture \
        -keyalg RSA -keysize 2048 -validity 3650 -dname "CN=Nestvane Test Fixture" \
        -storepass nestvane-fixture -keypass nestvane-fixture)
    ln "$temporary/smoke.p12" "$key" 2>/dev/null || test -f "$key"
fi
"$build_tools/apksigner" sign --ks "$key" --ks-pass pass:nestvane-fixture \
    --out "$temporary/signed.apk" "$temporary/aligned.apk"
"$build_tools/apksigner" verify "$temporary/signed.apk"
mv "$temporary/signed.apk" "$output"
echo "$output"
