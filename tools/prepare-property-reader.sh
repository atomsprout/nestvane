#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

root=${1:-generated/android17-init-root}
libc="$root/system/lib64/bootstrap/libc.so"
mkdir -p generated
xcrun clang --target=aarch64-linux-android -c fixtures/android17-properties.S -o generated/android17-properties.o
ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/bootstrap/linker64 -z max-page-size=4096 \
    -e _start --allow-shlib-undefined generated/android17-properties.o "$libc" \
    -o "$root/system/bin/nestvane-get-properties"

xcrun clang --target=aarch64-linux-android -c fixtures/android17-data.S -o generated/android17-data.o
ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/bootstrap/linker64 -z max-page-size=4096 \
    -e _start --allow-shlib-undefined generated/android17-data.o --no-as-needed "$libc" \
    -o "$root/system/bin/nestvane-guest-data"

if [[ -f "$root/system/bin/cmd" ]]; then
    xcrun clang --target=aarch64-linux-android -std=gnu17 -O2 -Wall -Wextra -Werror \
        -ffreestanding -fno-stack-protector -fPIE -c fixtures/android17-apk-install.c -o generated/android17-apk-install.o
    ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/bootstrap/linker64 -z max-page-size=4096 \
        -e _start --allow-shlib-undefined generated/android17-apk-install.o --no-as-needed "$libc" \
        -o "$root/system/bin/nestvane-apk-install"
fi
