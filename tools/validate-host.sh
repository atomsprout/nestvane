#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

tools/build-fixtures.sh
xcrun clang++ -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/Validation.cpp generated/AOT.cpp tools/validate.cpp -o generated/validate
generated/validate generated/guests

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/AArch64.cpp runtime/FastPath.cpp runtime/ProbeLinux.cpp runtime/ProbeData.cpp runtime/ProbeBinder.cpp \
    runtime/Ext4.cpp tools/no-aot.cpp tools/validate-boot-abi.cpp \
    -lsqlite3 -lresolv -o generated/validate-boot-abi
generated/validate-boot-abi

# Vector instructions, each run on this Mac's processor and in the interpreter with the same operands.
xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/AArch64.cpp runtime/FastPath.cpp \
    tools/no-aot.cpp tools/vector-instructions.S tools/validate-vector-instructions.cpp -o generated/validate-vector-instructions

# The pre-decoded common instructions against the interpreter, on random encodings and state.
xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/AArch64.cpp runtime/FastPath.cpp \
    tools/no-aot.cpp tools/validate-fast-path.cpp -o generated/validate-fast-path
generated/validate-fast-path 4000
generated/validate-vector-instructions

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    tools/validate-dns-proxy.cpp -o generated/validate-dns-proxy
generated/validate-dns-proxy

xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/GuestSpace.cpp tools/validate-guest-space.cpp -o generated/validate-guest-space
generated/validate-guest-space

# The rewriter, on instructions that name x25-x28 as ordinary registers: each run as written and as
# translated on this Mac's processor.
python3 tools/validate-rewriter.py generated/validate-rewriter
