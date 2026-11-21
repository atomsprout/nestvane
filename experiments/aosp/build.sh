#!/bin/bash
# Builds targets of the patched AOSP tree for the Nestvane guest image.
#   build.sh <log name> [out directory inside the tree] -- <m targets...>
# A second out directory lets a small build run while a full build holds out/.
cd ~/aosp
log=~/logs/$1.log
shift
if [ "$1" != "--" ]; then export OUT_DIR=$1; shift; fi
shift
mkdir -p ~/logs
source build/envsetup.sh > /dev/null 2>&1
lunch sdk_phone16k_arm64-cp2a-userdebug > "$log" 2>&1
start=$(date +%s)
m -k "$@" >> "$log" 2>&1
echo "BUILD_EXIT=$? seconds=$(( $(date +%s) - start ))" >> "$log"
