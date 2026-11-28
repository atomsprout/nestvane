#!/bin/zsh
# The check that comes before every release: Android's start must not get slower.
#
#   tools/boot-check.sh <device UDID> [--record]
#
# Starts the installed app six times. The first start may be a cold one and is only reported; the median
# of the other five, from launch to the Android home screen, is compared with the baseline of the same
# kind of device in results/boot-baseline.txt. Slower than the baseline fails (exit status 1). With
# --record a result that is not slower becomes the new baseline. A device that is not thermally nominal
# is not measured: numbers from a hot device say nothing. It rests a minute between starts; the whole
# check takes about ten minutes.
#
# BOOT_CHECK_ENV='"NAME":"value",...' adds settings to every start, for comparing one change on and off
# with the same build; such a run is not a release check and is never recorded.
#
# Compare like with like: the same device, the same apps installed (more apps, slower start), the device
# cooled down and left alone during the run. Build and install the app first (tools/regress-device.sh
# does both). The phone has to be unlocked and has to stay so.
set -uo pipefail
cd "${0:A:h}/.."
device=${1:?usage: boot-check.sh <device UDID> [--record]}
record=${2:-}
bundle=com.atomsprout.nestvane
out=generated/boot-check
baseline=results/boot-baseline.txt
mkdir -p $out

model=$(xcrun devicectl list devices 2>/dev/null | grep "$device" | grep -o '([A-Za-z]*[0-9]*,[0-9]*)' | tr -d '()')
[[ -n "$model" ]] || { echo "device $device not found"; exit 2; }
version=$(grep 'MARKETING_VERSION:' project.yml | tail -1 | sed 's/.*"\(.*\)".*/\1/')
commit=$(git rev-parse --short HEAD)

times=() memories=()
for start in 1 2 3 4 5 6; do
    # A rest between starts, with the app ended: an iPhone 14 gets warm after four starts in a row.
    if (( start > 1 )); then
        ${0:A:h}/end-app.sh $device $bundle
        sleep 60
    fi
    # A start with the screen off leaves the app behind the lock screen, where it is given less of the
    # processor: that start woke the screen, so it is made again, twice at most, and not counted.
    # An app that stays "not active" through that has something of the system's over it (after a
    # restart of the phone, an alert that only its owner can answer): it is in front all the same and
    # runs as fast, so the start is measured and the line says so. One in the background is not measured.
    for attempt in 1 2 3; do
        xcrun devicectl device process launch --device $device --terminate-existing \
            --environment-variables "{\"NESTVANE_INIT_SNAPSHOT_SECONDS\":\"25\"${BOOT_CHECK_ENV:+,$BOOT_CHECK_ENV}}" $bundle > $out/launch.log 2>&1 ||
            { echo "launch failed (is the device unlocked?): see $out/launch.log"; exit 2; }
        sleep 6
        rm -f $out/boot-milestones.txt
        xcrun devicectl device copy from --device $device --domain-type appDataContainer --domain-identifier $bundle \
            --source Documents/boot-milestones.txt --destination $out/boot-milestones.txt > /dev/null 2>&1
        head -1 $out/boot-milestones.txt 2>/dev/null | grep -q 'app not active\|app in the background' || break
        [[ -n "${covered:-}" ]] && break
        (( attempt == 3 )) && { covered=1; break; }
        echo "start $start: the app was not in front (screen off?); starting it again"
        ${0:A:h}/end-app.sh $device $bundle
        sleep 20
    done
    sleep 30
    for file in boot-milestones.txt guest-live.txt; do
        rm -f $out/$file
        xcrun devicectl device copy from --device $device --domain-type appDataContainer --domain-identifier $bundle \
            --source Documents/$file --destination $out/$file > /dev/null 2>&1
    done
    if head -1 $out/boot-milestones.txt 2>/dev/null | grep -q 'low power mode'; then
        echo "start $start: the device is in Low Power Mode, which slows it; turn that off and run this again"
        exit 2
    fi
    if head -1 $out/boot-milestones.txt 2>/dev/null | grep -q 'app in the background'; then
        echo "start $start: the app is in the background; bring it to the front and run this again"
        exit 2
    fi
    note=""
    head -1 $out/boot-milestones.txt 2>/dev/null | grep -q 'app not active' && note=" (the app was not active: something of the system's is over it)"
    thermal=$(head -1 $out/boot-milestones.txt 2>/dev/null | sed 's/.*thermal //; s/ | pages read.*//; s/ | app not active//')
    pages=$(head -1 $out/boot-milestones.txt 2>/dev/null | sed -n 's/.*pages read from storage \([0-9]*\).*/\1/p')
    if [[ "$thermal" != "nominal → nominal" ]]; then
        echo "start $start: thermal state '$thermal'; let the device cool down and run this again"
        exit 2
    fi
    home=$(grep -a 'Displayed com.android.launcher3' $out/boot-milestones.txt | tail -1 | sed 's/^\[\([0-9]*\) ms\].*/\1/')
    memory=$(grep -a 'iOS physical footprint bytes' $out/guest-live.txt 2>/dev/null | head -1 | awk '{printf "%.2f", $NF / 1e9}')
    [[ -n "$home" ]] || { echo "start $start: the home screen was not reached within 36 s"; exit 1; }
    echo "start $start: $home ms to the home screen, $memory GB${pages:+, $pages pages read from storage}$note"
    if (( start > 1 )); then times+=($home); memories+=($memory); fi
done

median=$(printf '%s\n' $times | sort -n | sed -n 3p)
memory=$(printf '%s\n' $memories | sort -n | sed -n 3p)
echo "$model, version $version ($commit): median of starts 2-6 is $median ms, $memory GB"
previous=$(grep -v '^#' $baseline 2>/dev/null | awk -v model=$model '$1 == model' | tail -1)
if [[ -n "$previous" ]]; then
    best=$(echo $previous | awk '{print $3}')
    echo "baseline: $previous"
    if (( median > best )); then
        echo "FAILED: $median ms is slower than the baseline's $best ms. Find out why before releasing."
        exit 1
    fi
    echo "passed: not slower than the baseline ($median ms against $best ms)"
else
    echo "no baseline for $model yet"
fi
if [[ -n "${BOOT_CHECK_ENV:-}" ]]; then
    echo "with $BOOT_CHECK_ENV: not recorded"
elif [[ "$record" == "--record" ]]; then
    echo "$model $version $median $memory $(date +%Y-%m-%d) $commit" >> $baseline
    echo "recorded in $baseline"
fi
