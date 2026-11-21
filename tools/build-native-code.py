#!/usr/bin/env python3
"""Translates the ELF files of Android trees into host code for the runtime's native execution.

Each distinct file is rewritten for one load address (experiments/g0/e3/rewrite.py) and assembled.
The runtime finds a module by the content key of the file a guest maps, not by its path, so the trees
only have to hold the same bytes as the image the guest boots: an init root collected by
tools/prepare-init-root.py, and unpacked APEX directories from the build.

usage: build-native-code.py <output directory> <tree>... [--only <substring>]...
Writes <output>/obj/*.o, <output>/modules.c and <output>/objects.txt (for the linker's -filelist).
Translations are kept between runs while the file, its address and the rewriter stay the same.
"""
import concurrent.futures
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REWRITER = os.path.join(HERE, "../experiments/g0/e3/rewrite.py")
OPTIONS = ["--table", "--chunk-shift", "20", "--reuse", "--split-index"]
# Below the kernel layer's mapping arena (4 GiB and up) and above the boot image and the first stack.
FIRST_BASE, LIMIT = 0x8000_0000, 0x1_0000_0000
KEY_BYTES = 16384


def content_key(path):
    """FNV-1a over the start of the file, mixed with its size. NativeCode.cpp computes the same."""
    with open(path, "rb") as file:
        data = file.read(KEY_BYTES)
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return ((value ^ os.path.getsize(path)) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF


def load_size(path):
    """The span of an ELF's loadable segments, or None unless it is arm64 code."""
    with open(path, "rb") as file:
        data = file.read(64)
        if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or struct.unpack_from("<H", data, 18)[0] != 183:
            return None
        phoff, = struct.unpack_from("<Q", data, 32)
        size, count = struct.unpack_from("<HH", data, 54)
        file.seek(phoff)
        headers = file.read(size * count)
    loads = [(struct.unpack_from("<I", headers, index * size + 4)[0], struct.unpack_from("<Q", headers, index * size + 16)[0],
              struct.unpack_from("<Q", headers, index * size + 40)[0])
             for index in range(count) if struct.unpack_from("<I", headers, index * size)[0] == 1]
    if not loads or not any(flags & 1 for flags, _, _ in loads):
        return None
    return max(address + length for _, address, length in loads)


def translate(job):
    symbol, path, base, objects, stamp = job
    assembly, output, record = (os.path.join(objects, symbol + suffix) for suffix in (".S", ".o", ".stamp"))
    if os.path.exists(output) and os.path.exists(record) and open(record).read() == stamp:
        return symbol, "kept"
    result = subprocess.run([sys.executable, REWRITER, *OPTIONS, "--base", hex(base), "--name", symbol, path, assembly],
                            capture_output=True, text=True)
    if result.returncode:
        return symbol, "FAILED " + result.stderr.strip().splitlines()[-1]
    subprocess.run(["clang", "-arch", "arm64", "-x", "assembler", "-c", assembly, "-o", output], check=True)
    os.remove(assembly)  # the largest are gigabytes
    open(record, "w").write(stamp)
    return symbol, result.stdout.splitlines()[0]


def main():
    arguments = [argument for index, argument in enumerate(sys.argv[1:], 1) if not argument.startswith("--") and sys.argv[index - 1] != "--only"]
    only = [sys.argv[index + 1] for index, argument in enumerate(sys.argv) if argument == "--only"]
    out, trees = os.path.abspath(arguments[0]), [os.path.abspath(tree) for tree in arguments[1:]]
    objects = os.path.join(out, "obj")
    os.makedirs(objects, exist_ok=True)
    version = str(os.path.getmtime(REWRITER)) + " ".join(OPTIONS)

    files = {}
    for tree in trees:
        for directory, _, names in os.walk(tree):
            for name in names:
                path = os.path.join(directory, name)
                if os.path.islink(path) or (only and not any(part in path for part in only)):
                    continue
                size = load_size(path)
                if size is None:
                    continue
                key = content_key(path)
                files.setdefault(key, (path, size, path[len(tree):]))
    jobs, base = [], FIRST_BASE
    for key, (path, size, guest) in sorted(files.items(), key=lambda item: item[1][2]):
        image = path[:-4] + ".art"
        if path.endswith(".oat") and os.path.exists(image):
            # Compiled boot class path code. ART maps it where its image expects it, which the image
            # header records as oat_file_begin.
            with open(image, "rb") as file:
                start, = struct.unpack_from("<I", file.read(36), 32)
        else:
            start = base
            base = (base + size + 0x1FFFF) & ~0xFFFF
        symbol = "m%016x" % key
        jobs.append((symbol, path, start, objects, "%s %x %x" % (version, key, start), guest, key, os.path.getsize(path)))
    if base > LIMIT:
        sys.exit("the modules need 0x%x bytes of guest addresses and 0x%x are set aside" % (base - FIRST_BASE, LIMIT - FIRST_BASE))
    print("modules=%d prelinked 0x%x-0x%x" % (len(jobs), FIRST_BASE, base), flush=True)
    failed = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(4, (os.cpu_count() or 8) - 2)) as pool:
        for (symbol, summary), job in zip(pool.map(translate, [job[:5] for job in jobs]), jobs):
            if summary.startswith("FAILED"):
                failed.add(symbol)
            if summary != "kept":
                print("%-70s base=0x%x %s" % (job[5][-70:], job[2], " ".join(summary.split()[:3])), flush=True)
    jobs = [job for job in jobs if job[0] not in failed]

    lines = ["#include <stdint.h>",
             "struct NativeModule { uint64_t key, size; const uint64_t* info; uint64_t base; const char* name; };"]
    lines += ["extern const uint64_t guest_module_%s[];" % job[0] for job in jobs]
    lines.append("const struct NativeModule nestvane_native_modules[] = {")
    lines += ['    {0x%xull, %d, guest_module_%s, 0x%xull, "%s"},' % (job[6], job[7], job[0], job[2], job[5]) for job in jobs]
    lines += ["};", "const int nestvane_native_module_count = %d;" % len(jobs)]
    open(os.path.join(out, "modules.c"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(out, "objects.txt"), "w").write("".join(os.path.join(objects, job[0] + ".o") + "\n" for job in jobs))
    print("translated=%d failed=%d" % (len(jobs), len(failed)))


if __name__ == "__main__":
    main()
