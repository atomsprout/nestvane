#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $# != 1 || ! -r "$1/linker64" || ! -r "$1/libc.so" || ! -r "$1/libdl.so" ]]; then
    echo "Usage: tools/prepare-dynamic-probe.sh /absolute/path/to/android17-runtime" >&2
    exit 2
fi

mkdir -p generated/guests
xcrun clang --target=aarch64-linux-android -c fixtures/android17-dynamic.S -o generated/android17-dynamic.o
ld.lld -m aarch64elf -pie --dynamic-linker /system/bin/linker64 -z max-page-size=4096     -e _start --allow-shlib-undefined generated/android17-dynamic.o "$1/libc.so"     -o generated/guests/android17-dynamic.elf
cp "$1/linker64" generated/guests/android17-linker64.elf
cp "$1/libc.so" generated/guests/android17-libc.so
cp "$1/libdl.so" generated/guests/android17-libdl.so
