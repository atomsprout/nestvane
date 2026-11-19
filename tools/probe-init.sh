#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

tools/prepare-property-reader.sh "${1:-generated/android17-init-root}"
mkdir -p generated
xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/AArch64.cpp runtime/DynamicProbe.cpp runtime/ProbeLinux.cpp runtime/ProbeData.cpp runtime/ProbeBinder.cpp runtime/Ext4.cpp \
    tools/no-aot.cpp tools/probe-dynamic.cpp -lsqlite3 -o generated/probe-init
generated/probe-init --init "${1:-generated/android17-init-root}" "${@:2}"
