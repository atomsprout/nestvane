#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

root=${1:-generated/android17-init-root}
libc="$root/system/lib64/bootstrap/libc.so"
mkdir -p generated
xcrun clang --target=aarch64-linux-android -c fixtures/android17-properties.S -o generated/android17-properties.o
ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/bootstrap/linker64 -z max-page-size=${NESTVANE_PAGE_SIZE:-4096} \
    -e _start --allow-shlib-undefined generated/android17-properties.o "$libc" \
    -o "$root/system/bin/nestvane-get-properties"

xcrun clang --target=aarch64-linux-android -c fixtures/android17-data.S -o generated/android17-data.o
ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/bootstrap/linker64 -z max-page-size=${NESTVANE_PAGE_SIZE:-4096} \
    -e _start --allow-shlib-undefined generated/android17-data.o --no-as-needed "$libc" \
    -o "$root/system/bin/nestvane-guest-data"

if [[ -f "$root/system/bin/cmd" ]]; then
    xcrun clang --target=aarch64-linux-android -std=gnu17 -O2 -Wall -Wextra -Werror \
        -ffreestanding -fno-stack-protector -fPIE -c fixtures/android17-apk-install.c -o generated/android17-apk-install.o
    ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/bootstrap/linker64 -z max-page-size=${NESTVANE_PAGE_SIZE:-4096} \
        -e _start --allow-shlib-undefined generated/android17-apk-install.o --no-as-needed "$libc" \
        -o "$root/system/bin/nestvane-apk-install"
    mkdir -p "$root/system/etc/nestvane"
    cp fixtures/android17-apk-ready.sh "$root/system/etc/nestvane/apk-ready.sh"
    cp fixtures/android17-trim.sh "$root/system/etc/nestvane/trim.sh"
    cp fixtures/android17-text.sh "$root/system/etc/nestvane/text.sh"
    cp fixtures/android17-sms.sh "$root/system/etc/nestvane/sms.sh"
    cp fixtures/android17-rotation.sh "$root/system/etc/nestvane/rotation.sh"
    tools/build-sms-broadcast.sh "$root"
    tools/build-rotation-watch.sh "$root"
    tools/build-phone-permissions.sh "$root"
fi
