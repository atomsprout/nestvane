#!/bin/zsh
# Puts Android System WebView into each guest root given: product/app/webview/webview.apk, where the
# AOSP build puts it. The package is AOSP's own prebuilt (platform/external/chromium-webview), fetched
# into generated/webview when it is not there yet and checked against the digest below. It is kept as
# AOSP ships it: its native library is stored uncompressed in the package and mapped from there.
set -euo pipefail
cd "${0:A:h}/.."
version=128.0.6613.88
digest=492c3d054e4123ae0c1e6f3541e5ab515633e1fb4006e33bf343749ef1b66ae2
apk=generated/webview/webview.apk
if [[ ! -s $apk ]] || [[ $(shasum -a 256 $apk | cut -d' ' -f1) != $digest ]]; then
    mkdir -p generated/webview
    curl -sf --max-time 1200 \
        "https://android.googlesource.com/platform/external/chromium-webview/+/refs/heads/main/$version/arm64/webview.apk?format=TEXT" \
        | base64 -d > $apk.part
    mv $apk.part $apk
fi
[[ $(shasum -a 256 $apk | cut -d' ' -f1) == $digest ]] || { echo "webview.apk: wrong digest" >&2; exit 1; }
for root in "$@"; do
    mkdir -p "$root/product/app/webview"
    cp $apk "$root/product/app/webview/webview.apk"
done
echo "webview $version: $(stat -f %z $apk) bytes"
