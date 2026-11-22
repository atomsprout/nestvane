#!/bin/zsh
# Packs a built app into an IPA that carries no signature and no provisioning profile, for people to
# sign with their own identity (see docs/installing.md).
#   package-ipa.sh <built .app with graphics embedded> <output .ipa> <NOTICE of the AOSP build>
# The licenses the app is distributed under go inside it, in Licenses/: Nestvane's own, the notices of
# the Android system (the NOTICE.txt that the emulator image build writes), and those of the graphics
# libraries (ios/Licenses/THIRD-PARTY-NOTICES.txt).
set -euo pipefail

app=${1:A} output=${2:A} aosp=${3:A}
repository=${0:A:h:h}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/Payload"
cp -Rc "$app" "$stage/Payload/Nestvane.app"
bundle="$stage/Payload/Nestvane.app"

mkdir "$bundle/Licenses"
cp "$repository/LICENSE" "$bundle/Licenses/LICENSE.txt"
cp "$repository/NOTICE" "$bundle/Licenses/NOTICE.txt"
cp "$repository/ios/Licenses/THIRD-PARTY-NOTICES.txt" "$bundle/Licenses/"
cp "$aosp" "$bundle/Licenses/NOTICE-AOSP.txt"

# The profile names the devices and the team it was made for, and the signatures name the signer.
rm -f "$bundle/embedded.mobileprovision"
for code in "$bundle"/Frameworks/*.framework "$bundle"; do
    codesign --remove-signature "$code"
done
find "$bundle" -name _CodeSignature -type d -prune -exec rm -rf {} +
if find "$bundle" -name embedded.mobileprovision -o -name _CodeSignature | grep -q .; then
    echo "signing material left in the bundle" >&2
    exit 1
fi

rm -f "$output"
(cd "$stage" && zip -q -6 -r -y "$output" Payload)
ls -l "$output" | awk '{printf "%s: %.2f GB\n", $9, $5 / 1e9}'
shasum -a 256 "$output"
