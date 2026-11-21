#!/usr/bin/env bash
# Runs the init probe with guest code executing natively: the root's ELF files are translated ahead of
# time (tools/build-native-code.py) and linked into the probe, and the kernel layer runs them in place
# of the interpreter. The root must come from an image built with experiments/aosp/patch.py.
# usage: probe-init-native.sh <init root> [<tree with unpacked APEXes>...]
set -euo pipefail
cd "$(dirname "$0")/.."

root="${1:-generated/android17r-init-root}"
NESTVANE_PAGE_SIZE=16384 tools/prepare-property-reader.sh "$root"
python3 tools/build-native-code.py generated/native-code "$root" "${@:2}" | tail -1
build=generated/native-build
mkdir -p "$build"
for source in Runtime GuestSpace AArch64 DynamicProbe ProbeLinux ProbeData ProbeBinder Ext4 NativeCode; do
    xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime -DNESTVANE_PAGE_SIZE=16384 -c runtime/$source.cpp -o "$build/$source.o" &
done
wait
xcrun clang++ -O2 -std=c++20 -Iruntime -DNESTVANE_PAGE_SIZE=16384 -c tools/no-aot.cpp -o "$build/no-aot.o"
xcrun clang++ -O2 -std=c++20 -Iruntime -DNESTVANE_PAGE_SIZE=16384 -c tools/probe-dynamic.cpp -o "$build/probe-dynamic.o"
xcrun clang -c runtime/NativeEntry.S -o "$build/NativeEntry.o"
xcrun clang -O1 -c generated/native-code/modules.c -o "$build/modules.o"
xcrun clang++ "$build"/*.o -Wl,-filelist,generated/native-code/objects.txt -lsqlite3 -o generated/probe-init-native 2> >(grep -v "branch island" >&2)
NESTVANE_NATIVE=1 generated/probe-init-native --init "$root"
