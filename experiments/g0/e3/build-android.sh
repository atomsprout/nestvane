#!/bin/bash
# Translates a Bionic toybox, the dynamic linker and their libraries (built from AOSP with
# experiments/aosp/patch.py) and links them with the host runtime.
#   experiments/g0/e3/build-android.sh <directory holding system/bin and system/lib64 from the build>
# Output: generated/g0/e3/android/{root,toybox-android}. Run with
#   generated/g0/e3/android/toybox-android generated/g0/e3/android/root /system/bin/toybox <args>
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
source=$(cd "$1" && pwd)
out=$(cd "$here/../../.." && pwd)/generated/g0/e3/android
ndk=${NDK:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | tail -1)}/toolchains/llvm/prebuilt/darwin-x86_64
segment=${SEGMENT:-(16ull << 30)}
rm -rf "$out"
mkdir -p "$out/root/system/bin" "$out/root/system/lib64" "$out/root/data/local/tmp" "$out/obj"

# The guest root. On a device the Bionic libraries live in the runtime APEX and /system/lib64 only
# links to them, so the bootstrap copies stand in.
for program in toybox sh; do
    [ -f "$source/system/bin/$program" ] && cp "$source/system/bin/$program" "$out/root/system/bin/$program"
done
cp "$source/system/bin/bootstrap/linker64" "$out/root/system/bin/linker64"
for library in "$source"/system/lib64/*.so "$source"/system/lib64/bootstrap/*.so; do
    [ -f "$library" ] && cp "$library" "$out/root/system/lib64/$(basename "$library")"
done

# Test programs of our own, built like the platform: dynamic, against Bionic, registers reserved.
for test in "$here"/guest-tests/*.c; do
    "$ndk/bin/clang" --target=aarch64-linux-android35 -O2 -pie -ffixed-x18 -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28 \
        -Wl,-z,max-page-size=16384 "$test" -o "$out/root/system/bin/test-$(basename "$test" .c)"
done

# Every module gets a fixed load address. The linker asks the runtime for it (system call 0x4e56).
base=$((0x10000000))
modules=()
for file in "$out"/root/system/bin/* "$out"/root/system/lib64/*.so; do
    name=$(basename "$file" | tr -c 'A-Za-z0-9\n' '_')
    guest=${file#"$out/root"}
    used=$("$ndk/bin/llvm-objdump" -d --no-show-raw-insn "$file" | grep -cE '\b[xw](25|26|27|28)\b' || true)
    printf '%-28s base=0x%x reserved_register_uses=%s ' "$guest" "$base" "$used"
    python3 "$here/rewrite.py" --base "$base" --name "$name" "$file" "$out/obj/$name.S" | head -1
    clang -arch arm64 -c "$out/obj/$name.S" -o "$out/obj/$name.o"
    modules+=("$name=$guest@$base")
    size=$(python3 -c 'import struct, sys
data = open(sys.argv[1], "rb").read()
phoff, = struct.unpack_from("<Q", data, 32)
entry_size, count = struct.unpack_from("<HH", data, 54)
print(max(struct.unpack_from("<Q", data, phoff + i * entry_size + 16)[0] + struct.unpack_from("<Q", data, phoff + i * entry_size + 40)[0]
          for i in range(count) if struct.unpack_from("<I", data, phoff + i * entry_size)[0] == 1))' "$file")
    base=$(( (base + size + 0x1ffff) & ~0xffff ))
done
python3 "$here/modules.py" "$out/obj/modules.c" "${modules[@]}"
clang -O2 -arch arm64 -DSEGMENT="$segment" "$here/runtime.c" "$here/entry.S" "$out/obj/modules.c" "$out"/obj/*.o -o "$out/toybox-android"

# toybox picks its command from the name it is run as, so give every command a link, as Android does.
for applet in $("$out/toybox-android" "$out/root" /system/bin/toybox); do
    [ -e "$out/root/system/bin/$applet" ] || ln -s toybox "$out/root/system/bin/$applet"
done
ls -la "$out/toybox-android"
echo "applets=$(ls "$out/root/system/bin" | wc -l | tr -d ' ')"
