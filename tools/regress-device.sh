#!/bin/zsh
# The regression on a phone, in one command: builds the app, installs it on the device, starts Android
# several times with the app's own test switches, and prints what it measured.
#
#   NESTVANE_TEAM=<development team> tools/regress-device.sh <device UDID> [<an APK to install> <its package name>]
#
# The phone has to be unlocked and has to stay so. Android's apps and data on it are kept.
# With many apps installed Android starts more slowly and uses more memory: compare like with like.
# What it checks: the time to the home screen and the memory there (twice), frames a second while a
# finger keeps moving a list at 120 Hz and at 60 Hz, a text message reaching the broadcast, and, with
# an APK, that it installs and that its first screen is drawn. Results are also left in
# generated/regress/, with the frames it saved.
set -uo pipefail
cd "${0:A:h}/.."
device=${1:?usage: regress-device.sh <device UDID> [<apk> <package>]}
apk=${2:-}
package=${3:-}
bundle=com.atomsprout.nestvane
out=generated/regress
mkdir -p $out

if [[ -z "${NESTVANE_SKIP_BUILD:-}" ]]; then
    xcodebuild -project nestvane.xcodeproj -scheme nestvane-native -configuration Release -destination "id=$device" \
        -derivedDataPath generated/native-derived -allowProvisioningUpdates DEVELOPMENT_TEAM=${NESTVANE_TEAM:?set NESTVANE_TEAM} \
        build > $out/build.log 2>&1 || { echo "build failed: see $out/build.log"; exit 1; }
    rm -rf generated/native-app-graphics && mkdir -p generated/native-app-graphics
    tools/embed-ios-graphics.sh generated/native-derived/Build/Products/Release-iphoneos/nestvane-native.app \
        generated/graphics/gfxstream-ios/libgfxstream_backend.dylib generated/graphics/angle-ios \
        generated/native-app-graphics/nestvane-native.app > $out/embed.log 2>&1 || { echo "embedding graphics failed"; exit 1; }
    xcrun devicectl device install app --device $device generated/native-app-graphics/nestvane-native.app > $out/install.log 2>&1 ||
        { echo "install failed: see $out/install.log"; exit 1; }
fi

# run <environment as JSON> <seconds to wait> <files to fetch from Documents>...
run() {
    local environment=$1 wait=$2; shift 2
    xcrun devicectl device process launch --device $device --terminate-existing --environment-variables "$environment" $bundle \
        > $out/launch.log 2>&1 || { echo "launch failed (is the phone unlocked?): see $out/launch.log"; exit 1; }
    sleep $wait
    for file in "$@"; do
        rm -f $out/$file
        xcrun devicectl device copy from --device $device --domain-type appDataContainer --domain-identifier $bundle \
            --source Documents/$file --destination $out/$file > /dev/null 2>&1
    done
}
home() { grep -a 'Displayed com.android.launcher3' $out/boot-milestones.txt 2>/dev/null | tail -1 | sed 's/^\[\([0-9]*\) ms\].*/\1/'; }
memory() { grep -a 'iOS physical footprint bytes' $out/guest-live.txt 2>/dev/null | head -1 | awk '{printf "%.2f", $NF / 1e9}'; }
rates() { grep -E 'composed ([4-9][0-9]|1[0-9][0-9]),' $out/display-rate.txt 2>/dev/null | sed 's/.*shown \([0-9]*\) frames.*/\1/' | sort -n | sed -n '1p;$p' | tr '\n' ' '; }

echo "| Check | Result |"
echo "| --- | --- |"
for boot in 1 2; do
    run '{"NESTVANE_INIT_SNAPSHOT_SECONDS":"25"}' 36 boot-milestones.txt guest-live.txt
    echo "| Start $boot, to the home screen | $(home) ms, $(memory) GB |"
done
run '{"NESTVANE_DISPLAY_HZ":"120","NESTVANE_TEST_SWIPE":"1","NESTVANE_DISPLAY_RATE":"1","NESTVANE_TEST_TAPS":"drag:0:0:10000"}' 42 display-rate.txt
echo "| Frames a second, finger moving, 120 Hz (lowest and highest second) | $(rates)|"
run '{"NESTVANE_DISPLAY_HZ":"60","NESTVANE_TEST_SWIPE":"1","NESTVANE_DISPLAY_RATE":"1","NESTVANE_TEST_TAPS":"drag:0:0:10000"}' 42 display-rate.txt
echo "| The same at 60 Hz | $(rates)|"
# With the whole log: the filtered log is cut off at 256 KiB, which the start of Android alone can
# fill, and then the line this step looks for is not in it.
run '{"NESTVANE_TEST_TEXT":"555-0100|Regression 482913","NESTVANE_LOG_ALL":"1"}' 50 boot-progress.txt
echo "| Text message announced to apps | $(grep -a -c 'NestvaneSms: sent SMS_RECEIVED' $out/boot-progress.txt 2>/dev/null) of 1 |"
if [[ -n "$apk" && -n "$package" ]]; then
    name=${apk:t}
    xcrun devicectl device copy to --device $device --domain-type appDataContainer --domain-identifier $bundle \
        --source "$apk" --destination Documents/$name > /dev/null 2>&1
    run "{\"NESTVANE_TEST_SWIPE\":\"1\",\"NESTVANE_TEST_APK\":\"$name\",\"NESTVANE_TEST_TAPS\":\"launch:$package:0:15\",\"NESTVANE_LOG_MATCH\":\"Displayed $package,AndroidRuntime: FATAL\"}" 60 apk-result.txt boot-progress.txt touch-tap-0.png
    echo "| Installing $name | $(cut -d'|' -f4,5 $out/apk-result.txt 2>/dev/null | head -1) |"
    echo "| Opening $package | first screen drawn $(grep -a -c "Displayed $package" $out/boot-progress.txt 2>/dev/null) time(s), crashes $(grep -a -c 'FATAL EXCEPTION' $out/boot-progress.txt 2>/dev/null) |"
fi
