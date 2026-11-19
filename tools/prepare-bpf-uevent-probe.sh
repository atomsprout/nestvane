#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"

root=${1:?Usage: tools/prepare-bpf-uevent-probe.sh /path/to/unpacked-guest}
sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
ndk="${ANDROID_NDK_HOME:-$sdk/ndk/29.0.14206865}"
compiler="$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang"
object="$root/vendor/etc/bpf/filterPowerSupplyEvents.o"
test -f "$object" || { echo "Health BPF object missing in $root" >&2; exit 1; }

temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT
"${LLVM_OBJCOPY:-llvm-objcopy}" --dump-section "skfilter/power_supply=$temporary/health-filter.bin" \
    "$object" "$temporary/filter-copy.o"
test -s "$temporary/health-filter.bin"
"$compiler" -O2 -Wall -Wextra -Werror "$repo/fixtures/android17-bpf-uevent.c" -o "$temporary/nestvane-health-bpf-check"
mkdir -p "$root/system/etc/nestvane" "$root/system/bin" "$root/system/etc/init"
cp "$temporary/health-filter.bin" "$root/system/etc/nestvane/health-filter.bin"
cp "$temporary/nestvane-health-bpf-check" "$root/system/bin/nestvane-health-bpf-check"
cat > "$root/system/etc/init/nestvane-health-bpf-check.rc" <<'RC'
on boot
    exec_start nestvane-health-bpf-check

service nestvane-health-bpf-check /system/bin/nestvane-health-bpf-check
    user root
    group root
    seclabel u:r:init:s0
    disabled
    oneshot
    stdio_to_kmsg
RC
