#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

tools/build-fixtures.sh
xcrun clang++ -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/Validation.cpp generated/AOT.cpp tools/validate.cpp -o generated/validate
generated/validate generated/guests

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/AArch64.cpp runtime/ProbeLinux.cpp runtime/ProbeData.cpp runtime/ProbeBinder.cpp \
    runtime/Ext4.cpp tools/no-aot.cpp tools/validate-boot-abi.cpp \
    -lsqlite3 -o generated/validate-boot-abi
generated/validate-boot-abi
