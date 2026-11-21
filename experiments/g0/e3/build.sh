#!/bin/sh
# Builds the E3 experiment: musl, the soft-float builtins and toybox with x18 and x25-x28 reserved,
# then rewrites toybox into Mach-O code and links it with the host runtime.
#   experiments/g0/e3/build.sh
# Needs the Android NDK (for an ELF-targeting clang and lld), Xcode, git, GNU make and GNU sed.
# Everything lands in generated/g0/e3. Compare against a reference kernel with compare.py.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=$(cd "$here/../../.." && pwd)/generated/g0/e3
ndk=${NDK:-$(ls -d "$HOME"/Library/Android/sdk/ndk/* | tail -1)}/toolchains/llvm/prebuilt/darwin-x86_64
reserved="-ffixed-x18 -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28"
mkdir -p "$out/src" "$out/bin" "$out/obj/rt"
cd "$out"

fetch() {  # <directory> <tag> <url> [sparse path]
    [ -d "src/$1" ] && return
    if [ -n "${4:-}" ]; then
        git clone -q --depth 1 --filter=blob:none --sparse --branch "$2" "$3" "src/$1"
        git -C "src/$1" sparse-checkout set "$4"
    else
        git clone -q --depth 1 --branch "$2" "$3" "src/$1"
    fi
}
fetch musl v1.2.5 https://git.musl-libc.org/git/musl
fetch toybox 0.8.12 https://github.com/landley/toybox
fetch llvm llvmorg-21.1.0 https://github.com/llvm/llvm-project compiler-rt/lib/builtins

# The only source change: longjmp must not restore the runtime's registers, because after a fork the
# base register saved in a jmp_buf would be the parent's.
python3 - <<'EOF'
path = "src/musl/src/setjmp/aarch64/longjmp.s"
text = open(path).read()
text = text.replace("\tldp x25, x26, [x0,#48]\n\tldp x27, x28, [x0,#64]\n", "\t// x25-x28 belong to the host runtime and are not restored.\n")
open(path, "w").write(text)
EOF

if [ ! -f sysroot/lib/libc.a ]; then
    (cd src/musl && CC="$ndk/bin/clang --target=aarch64-linux-musl" AR="$ndk/bin/llvm-ar" RANLIB="$ndk/bin/llvm-ranlib" \
        CFLAGS="-O2 $reserved -fno-stack-protector" LIBCC=" " \
        ./configure --target=aarch64-linux-musl --disable-shared --prefix="$out/sysroot" > "$out/musl-configure.log" 2>&1 &&
        make -j8 install > "$out/musl-build.log" 2>&1)
    # Kernel headers that toybox includes. The NDK ships the same UAPI headers.
    for name in linux asm-generic mtd; do cp -R "$ndk/sysroot/usr/include/$name" sysroot/include/; done
    cp -R "$ndk/sysroot/usr/include/aarch64-linux-android/asm" sysroot/include/asm
fi

compiler() {  # <name> <flags>
    cat > "bin/$1" <<EOF
#!/bin/sh
# Cross compiler for guest programs: musl, static, non-PIE.
S=$out/sysroot
link=1
for a in "\$@"; do case "\$a" in -c|-E|-S|-M|-MM) link=0;; esac; done
F="--target=aarch64-linux-musl --sysroot=\$S -isystem \$S/include -ffixed-x18 $2 -fno-stack-protector -fno-pie"
if [ \$link = 1 ]; then
  exec $ndk/bin/clang \$F -nostdlib -static -fuse-ld=lld \$S/lib/crt1.o \$S/lib/crti.o "\$@" -L\$S/lib -lc \$S/lib/libguestrt.a -lc \$S/lib/crtn.o
fi
exec $ndk/bin/clang \$F "\$@"
EOF
    chmod +x "bin/$1"
}
compiler guest-cc "-ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28"
compiler guest-cc-plain ""  # the same toolchain without the reservation, to measure its cost

# The NDK's prebuilt compiler-rt uses x25 and x26, so the long double helpers are rebuilt.
for name in addtf3 subtf3 multf3 divtf3 comparetf2 extenddftf2 extendsftf2 trunctfdf2 trunctfsf2 fixtfdi fixtfsi fixtfti \
    fixunstfdi fixunstfsi fixunstfti floatditf floatsitf floattitf floatunditf floatunsitf floatuntitf powitf2 multi3 udivti3 \
    umodti3 divti3 modti3 udivmodti4 multc3 divtc3 fp_mode; do
    bin/guest-cc -O2 -c "src/llvm/compiler-rt/lib/builtins/$name.c" -o "obj/rt/$name.o"
done
rm -f sysroot/lib/libguestrt.a
"$ndk/bin/llvm-ar" rc sysroot/lib/libguestrt.a obj/rt/*.o

toybox() {  # <source copy> <compiler> <output>
    [ -d "src/$1" ] || cp -R src/toybox "src/$1"
    (cd "src/$1" && rm -rf generated/obj generated/unstripped && make defconfig > /dev/null 2>&1 &&
        gsed -i 's/^CONFIG_TOYBOX_ZHELP=y/# CONFIG_TOYBOX_ZHELP is not set/; s/^# CONFIG_\(SH\|BC\|DIFF\|GZIP\) is not set/CONFIG_\1=y/' .config &&
        CC="$out/bin/$2" HOSTCC=cc CFLAGS=-O2 NOSTRIP=1 LDOPTIMIZE=-Wl,--gc-sections LDASNEEDED=-Wl,--as-needed make > "$out/$3.log" 2>&1)
    cp "src/$1/generated/unstripped/toybox" "$3"
}
toybox toybox guest-cc toybox.elf
toybox toybox-plain guest-cc-plain toybox-plain.elf

for file in toybox.elf toybox-plain.elf; do
    echo "$file bytes=$(wc -c < $file | tr -d ' ') instructions_using_x25_to_x28=$("$ndk/bin/llvm-objdump" -d $file | grep -cE '\b[xw](25|26|27|28)\b')"
done

printf '#include <stdio.h>\n#include <time.h>\nint main(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);printf("%%lld\\n",t.tv_sec*1000000000LL+t.tv_nsec);return 0;}\n' > mono.c
bin/guest-cc -O2 mono.c -o mono

# hostlr and guestlr use one base register. table translates through a per-chunk table, with guest return addresses.
for mode in hostlr guestlr table; do
    flag=$(case $mode in hostlr) echo --host-lr;; table) echo --table;; esac)
    define=$([ $mode = table ] && echo -DSPARSE || true)
    python3 "$here/rewrite.py" $flag toybox.elf toybox-$mode.s
    clang -arch arm64 -c toybox-$mode.s -o toybox-$mode.o
    clang -O2 -arch arm64 -DSEGMENT='(4ull << 30)' $define "$here/runtime.c" "$here/entry.S" toybox-$mode.o -o toybox-$mode
done
