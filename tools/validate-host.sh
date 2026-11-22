#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

tools/build-fixtures.sh
xcrun clang++ -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/Validation.cpp generated/AOT.cpp tools/validate.cpp -o generated/validate
generated/validate generated/guests

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/AArch64.cpp runtime/ProbeLinux.cpp runtime/ProbeData.cpp runtime/ProbeBinder.cpp \
    runtime/Ext4.cpp tools/no-aot.cpp tools/validate-boot-abi.cpp \
    -lsqlite3 -lresolv -o generated/validate-boot-abi
generated/validate-boot-abi

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    tools/validate-dns-proxy.cpp -o generated/validate-dns-proxy
generated/validate-dns-proxy

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/GuestSpace.cpp tools/validate-guest-space.cpp -o generated/validate-guest-space
generated/validate-guest-space
