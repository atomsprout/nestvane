#!/bin/bash
# Rebuilds the toolchain runtime libraries that AOSP takes as prebuilts (compiler-rt builtins,
# libunwind, libc++ and libc++abi for arm64 Android) with x18 and x25-x28 reserved, and installs
# them over the prebuilts in the checkout. The recipe follows toolchain/llvm_android's builders.py
# for the same toolchain revision, minus LTO and profile flags.
#
#   build-runtimes.sh <aosp root> <llvm-project at the toolchain's base revision> <llvm_android> <ndk>
set -euo pipefail

aosp=$1 llvm=$2 llvm_android=$3 ndk=$4
clang=$(ls -d "$aosp"/prebuilts/clang/host/linux-x86/clang-r* | tail -1)
resource=$(ls -d "$clang"/lib/clang/*/lib/linux)
sysroot=$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot
work=${WORK:-$HOME/tc/build}
reserved="-ffixed-x18 -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28"
mkdir -p "$work"

# The Android patches that touch the runtimes, in the order the toolchain applied them.
if [ ! -f "$llvm/.nestvane-patched" ]; then
    grep -o 'patches/[^)]*\.patch' "$clang/clang_source_info.md" | while read -r patch; do
        git -C "$llvm" apply --include='libcxx/*' --include='libcxxabi/*' --include='libunwind/*' \
            --include='compiler-rt/lib/builtins/*' --include='runtimes/*' "$llvm_android/$patch" 2>/dev/null &&
            echo "applied $patch" || true
    done
    touch "$llvm/.nestvane-patched"
fi

configure() {  # <name> <api level> <source dir> <extra c flags> <extra ld flags> <cmake defines...>
    local name=$1 api=$2 source=$3 cflags=$4 ldflags=$5
    shift 5
    # Execute-only code needs a read-only segment, which the driver only emits from API 29.
    local xom_compile="" xom_link=""
    if [ "$api" -ge 29 ]; then xom_compile=-mexecute-only xom_link=-Wl,--execute-only; fi
    local flags="--target=aarch64-linux-android$api --sysroot=$sysroot -ffunction-sections -fdata-sections -D__BIONIC_NO_PAGE_SIZE_MACRO $xom_compile $reserved $cflags"
    local link="-fuse-ld=lld -rtlib=compiler-rt -Wl,-z,defs -Wl,--gc-sections -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 $xom_link -resource-dir $clang/lib/clang/$(basename "$(dirname "$(dirname "$resource")")") $ldflags"
    rm -rf "$work/$name"
    cmake -G Ninja -S "$source" -B "$work/$name" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DANDROID=TRUE -DANDROID_PLATFORM_LEVEL="$api" -DANDROID_NATIVE_API_LEVEL="$api" \
        -DCMAKE_C_COMPILER="$clang/bin/clang" -DCMAKE_CXX_COMPILER="$clang/bin/clang++" -DCMAKE_ASM_COMPILER="$clang/bin/clang" \
        -DCMAKE_AR="$clang/bin/llvm-ar" -DCMAKE_RANLIB="$clang/bin/llvm-ranlib" -DCMAKE_NM="$clang/bin/llvm-nm" \
        -DCMAKE_C_COMPILER_TARGET="aarch64-linux-android$api" -DCMAKE_CXX_COMPILER_TARGET="aarch64-linux-android$api" \
        -DCMAKE_ASM_COMPILER_TARGET="aarch64-linux-android$api" -DCMAKE_SYSROOT="$sysroot" \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_ASM_FLAGS="$flags" \
        -DCMAKE_EXE_LINKER_FLAGS="$link" -DCMAKE_SHARED_LINKER_FLAGS="$link" -DCMAKE_MODULE_LINKER_FLAGS="$link" \
        -DLLVM_ENABLE_ASSERTIONS=OFF "$@" > "$work/$name.configure.log" 2>&1
    nice ninja -C "$work/$name" > "$work/$name.build.log" 2>&1
}

keep() {  # back a prebuilt up once, then replace it
    [ -f "$2.prebuilt" ] || cp "$2" "$2.prebuilt"
    cp "$1" "$2"
    echo "installed $2"
}

configure builtins 21 "$llvm/compiler-rt/lib/builtins" "" "" \
    -DCOMPILER_RT_BUILTINS_HIDE_SYMBOLS=TRUE -DCOMPILER_RT_DEFAULT_TARGET_ONLY=TRUE \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF -DCOMPILER_RT_OS_DIR=android
built=$(find "$work/builtins" -name 'libclang_rt.builtins-aarch64-android.a' | head -1)
keep "$built" "$resource/libclang_rt.builtins-aarch64-android.a"
keep "$built" "$clang/runtimes_ndk_cxx/libclang_rt.builtins-aarch64-android.a"

for variant in hermetic exported; do
    hide=$([ $variant = hermetic ] && echo TRUE || echo FALSE)
    api=$([ $variant = hermetic ] && echo 21 || echo 30)
    configure "libunwind-$variant" $api "$llvm/runtimes" "-D_LIBUNWIND_USE_DLADDR=0" "-unwindlib=none -nostdlib++" \
        -DLLVM_ENABLE_RUNTIMES=libunwind -DLIBUNWIND_HIDE_SYMBOLS=$hide -DLIBUNWIND_ENABLE_SHARED=FALSE \
        -DLIBUNWIND_ENABLE_ASSERTIONS=FALSE -DLIBUNWIND_USE_FRAME_HEADER_CACHE=$([ $variant = exported ] && echo TRUE || echo FALSE) \
        -DLIBUNWIND_TARGET_TRIPLE="aarch64-linux-android$api"
done
keep "$work/libunwind-hermetic/lib/libunwind.a" "$resource/aarch64/libunwind.a"
keep "$work/libunwind-hermetic/lib/libunwind.a" "$clang/runtimes_ndk_cxx/aarch64/libunwind.a"
keep "$work/libunwind-exported/lib/libunwind.a" "$resource/aarch64/libunwind-exported.a"

libcxx() {  # <name> <api> <unwind flag> <defines...>
    local name=$1 api=$2 unwind=$3
    shift 3
    configure "$name" "$api" "$llvm/runtimes" "-fstack-protector-strong -DHAS_THREAD_LOCAL" "-nostdlib++ $unwind" \
        -DLLVM_ENABLE_RUNTIMES='libcxx;libcxxabi' -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
        -DLIBCXXABI_NON_DEMANGLING_TERMINATE=ON -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=OFF -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
        -DLIBCXX_STATICALLY_LINK_ABI_IN_SHARED_LIBRARY=ON -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF "$@"
}
libcxx libcxx-platform 30 "-unwindlib=none -Wl,--pack-dyn-relocs=android+relr" \
    -DLIBCXXABI_STATIC_DEMANGLE_LIBRARY=ON -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_STATIC_OUTPUT_NAME=c++_static \
    -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON
libcxx libcxx-platform-noexcept 30 "-unwindlib=none -Wl,--pack-dyn-relocs=android+relr" \
    -DLIBCXXABI_STATIC_DEMANGLE_LIBRARY=ON -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_ENABLE_EXCEPTIONS=OFF -DLIBCXX_STATIC_OUTPUT_NAME=c++_static_noexcept \
    -DLIBCXXABI_STATIC_OUTPUT_NAME=c++abi_noexcept -DLIBCXXABI_DEMANGLE_STATIC_OUTPUT_NAME=c++demangle_noexcept \
    -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON
libcxx libcxx-ndk 21 "-unwindlib=libunwind" \
    -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_STATIC_OUTPUT_NAME=c++_static -DLIBCXX_SHARED_OUTPUT_NAME=c++_shared \
    -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=OFF -DLIBCXX_ABI_VERSION=1 -DLIBCXX_ABI_NAMESPACE=__ndk1

for name in libc++abi.a libc++_static.a libc++.so libc++demangle.a; do
    keep "$work/libcxx-platform/lib/$name" "$clang/android_libc++/platform/aarch64/lib/$name"
done
for name in libc++abi_noexcept.a libc++_static_noexcept.a libc++demangle_noexcept.a; do
    keep "$work/libcxx-platform-noexcept/lib/$name" "$clang/android_libc++/platform_noexcept/aarch64/lib/$name"
done
for name in libc++abi.a libc++_static.a libc++_shared.so; do
    keep "$work/libcxx-ndk/lib/$name" "$clang/android_libc++/ndk/aarch64/lib/$name"
done

# Report what is left: only libunwind's register save and restore should mention the registers.
for file in "$resource/libclang_rt.builtins-aarch64-android.a" "$resource"/aarch64/libunwind*.a \
    "$clang"/android_libc++/platform/aarch64/lib/* "$clang"/android_libc++/platform_noexcept/aarch64/lib/* \
    "$clang"/android_libc++/ndk/aarch64/lib/*; do
    case $file in *.prebuilt) continue;; esac
    echo "$(basename "$(dirname "$(dirname "$file")")")/$(basename "$file") uses_x25_x28=$("$clang/bin/llvm-objdump" -d --no-show-raw-insn "$file" 2>/dev/null | grep -cE '\b[xw](25|26|27|28)\b')"
done
