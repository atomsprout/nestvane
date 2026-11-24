#!/usr/bin/env bash
# Builds and runs the interpreter benchmark (tools/bench-interpreter.cpp): the same compiled C on this
# Mac's processor and in the interpreter. Optional argument: how much work (default 20).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p generated
xcrun clang -O2 -ffreestanding -fno-builtin -fno-jump-tables -fno-vectorize -fno-slp-vectorize -arch arm64 \
    -c tools/interpreter-bench.c -o generated/interpreter-bench.o
xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime \
    runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp runtime/FastPath.cpp runtime/NativeEntry.S runtime/NoNativeCode.cpp \
    runtime/AArch64.cpp tools/no-aot.cpp tools/bench-interpreter.cpp generated/interpreter-bench.o \
    -o generated/bench-interpreter
generated/bench-interpreter "$@"
