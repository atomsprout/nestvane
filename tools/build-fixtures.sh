#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p generated/guests
linker="$(command -v ld.lld)"
arguments=()
for name in arithmetic memory; do
    object="generated/$name.o"
    elf="generated/guests/$name.elf"
    xcrun clang --target=aarch64-linux-gnu -c "fixtures/$name.S" -o "$object"
    "$linker" -m aarch64elf -static --no-relax -T fixtures/guest.ld "$object" -o "$elf"
    symbols="$(xcrun llvm-nm -P "$elf")"
    start="$(awk '$1 == "hot_start" {print $3}' <<< "$symbols")"
    end="$(awk '$1 == "hot_end" {print $3}' <<< "$symbols")"
    test -n "$start"
    test -n "$end"
    arguments+=("$name" "$elf" "$start" "$end")
done

xcrun clang++ -O2 -std=c++20 -Iruntime runtime/Runtime.cpp tools/no-aot.cpp tools/aot.cpp     -o generated/aot
generated/aot "${arguments[@]}" > generated/AOT.cpp.tmp
mv generated/AOT.cpp.tmp generated/AOT.cpp
