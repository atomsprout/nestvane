#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" == --art ]]; then
    set -- --art "${2:-generated/android17-art-root}"
else
    tools/prepare-dynamic-probe.sh "$@"
    set -- generated/guests
fi
mkdir -p generated
xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/AArch64.cpp runtime/DynamicProbe.cpp runtime/ProbeLinux.cpp \
    runtime/ProbeData.cpp runtime/ProbeBinder.cpp runtime/Ext4.cpp tools/no-aot.cpp tools/probe-dynamic.cpp \
    -lsqlite3 -lresolv -o generated/probe-dynamic
generated/probe-dynamic "$@"
