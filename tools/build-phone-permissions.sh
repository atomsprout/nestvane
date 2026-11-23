#!/bin/zsh
# Builds fixtures/phone-permissions, a package without code that shares the phone UID, and puts it into
# each guest root given: system/app/NestvanePhonePermissions. A package may share that UID only when it
# is signed with the platform's key. The image is AOSP's, signed with AOSP's test keys, which are
# public: the key is fetched from AOSP when generated/aosp-test-keys does not have it yet.
# Needs the Android SDK build tools (ANDROID_HOME, default ~/Library/Android/sdk).
set -euo pipefail
cd "${0:A:h}/.."
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
tools=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
keys=generated/aosp-test-keys
mkdir -p $keys
for file in platform.pk8 platform.x509.pem; do
    [[ -s $keys/$file ]] || curl -s -L --max-time 60 \
        "https://android.googlesource.com/platform/build/+/refs/heads/main/target/product/security/$file?format=TEXT" | base64 -d > $keys/$file
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$tools/aapt2" link -o "$work/unsigned.apk" --manifest fixtures/phone-permissions/AndroidManifest.xml \
    -I "$1/system/framework/framework-res.apk" --min-sdk-version 37 --target-sdk-version 37
"$tools/zipalign" -f 4 "$work/unsigned.apk" "$work/NestvanePhonePermissions.apk"
"$tools/apksigner" sign --key $keys/platform.pk8 --cert $keys/platform.x509.pem "$work/NestvanePhonePermissions.apk"
for root in "$@"; do
    mkdir -p "$root/system/app/NestvanePhonePermissions"
    cp "$work/NestvanePhonePermissions.apk" "$root/system/app/NestvanePhonePermissions/NestvanePhonePermissions.apk"
done
echo "NestvanePhonePermissions: $(stat -f %z "$work/NestvanePhonePermissions.apk") bytes"
