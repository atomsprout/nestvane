#!/bin/bash
# Copies the parts of a finished build that the guest experiments run from the build machine:
# /system without its apps, every APEX as the build lays it out unpacked, and the environment that
# init would set. The result is a tree for experiments/g0/e3/build_guest.py.
#   fetch.sh <ssh host> <product out directory on it> <local directory>
#   e.g. fetch.sh nestvane-build aosp/out/target/product/emu64a16k generated/g0/android-full
set -euo pipefail

host=$1 product=$2 target=$3
mkdir -p "$target"
# Apps and their data are not needed to start the runtime, and are most of the size.
ssh "$host" "cd $product && tar cf - --exclude='system/app' --exclude='system/priv-app' --exclude='apex/*/app' \
    --exclude='apex/*/priv-app' --exclude='*.debug' --exclude='*.testing' --exclude='system/apex' \
    system apex root/init.environ.rc | zstd -3 -T0" | zstd -d | tar xf - -C "$target"
du -sh "$target/system" "$target/apex"
find "$target" -type f | wc -l
