#!/usr/bin/env python3
"""Translates the ELF files of Android trees into host code for the runtime's native execution.

Each distinct file is rewritten for one load address (experiments/g0/e3/rewrite.py) and assembled.
The runtime finds a module by the content key of the file a guest maps, not by its path, so the trees
only have to hold the same bytes as the image the guest boots: an init root collected by
tools/prepare-init-root.py, and unpacked APEX directories from the build.

usage: build-native-code.py <output directory> <tree>... [--only <substring>]... [--ios]
Writes <output>/modules.c and the translations as dynamic libraries, <output>/lib/libnative-<n>.dylib:
one image cannot hold them all. <output>/libraries.txt lists the libraries for the linker.
Translations are kept between runs while the file, its address and the rewriter stay the same.
--ios builds for iOS devices instead of this Mac, into <output>/ios.
"""
import concurrent.futures
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REWRITER = os.path.join(HERE, "../experiments/g0/e3/rewrite.py")
OPTIONS = ["--table", "--chunk-shift", "20", "--reuse", "--split-index", "--final-table"]
# Below the kernel layer's mapping arena (4 GiB and up) and above the boot image and the first stack.
FIRST_BASE, LIMIT = 0x8000_0000, 0x1_0000_0000
KEY_BYTES = 16384
IOS = "--ios" in sys.argv
TARGET = (["-target", "arm64-apple-ios26.0", "-isysroot", subprocess.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"],
           capture_output=True, text=True).stdout.strip()] if IOS else ["-arch", "arm64"])
LIBRARIES = 12  # a fixed number, so that the app project can name them
# Rewritten code reaches the runtime's entries through the context: a call across images would go
# through a linker stub, which uses registers that belong to the guest. x27 is the runtime's and is
# loaded again when the guest is entered. The offsets are NativeContext::svcEntry and trapEntry.
SVC = """.text
.p2align 2
.private_extern _guest_svc
_guest_svc:
\tldr x27, [x25, #864]
\tbr x27
.private_extern _guest_trap
_guest_trap:
\tldr x27, [x25, #872]
\tbr x27
"""


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
    subprocess.run(["clang", *TARGET, "-x", "assembler", "-c", assembly, "-o", output], check=True)
    os.remove(assembly)  # the largest are gigabytes
    open(record, "w").write(stamp)
    return symbol, result.stdout.splitlines()[0]


def main():
    arguments = [argument for index, argument in enumerate(sys.argv[1:], 1) if not argument.startswith("--") and sys.argv[index - 1] != "--only"]
    modules_file = os.path.join(os.path.abspath(arguments[0]), "modules.c")
    only = [sys.argv[index + 1] for index, argument in enumerate(sys.argv) if argument == "--only"]
    out, trees = os.path.abspath(arguments[0]), [os.path.abspath(tree) for tree in arguments[1:]]
    if IOS:
        out = os.path.join(out, "ios")
    objects, libraries = os.path.join(out, "obj"), os.path.join(out, "lib")
    os.makedirs(objects, exist_ok=True)
    os.makedirs(libraries, exist_ok=True)
    version = str(os.path.getmtime(REWRITER)) + " ".join(OPTIONS)

    # Files with the same bytes under several paths each get a translation, at an address of their own:
    # one process may load several of them, such as the system's and the vendor's copy of a library.
    files, copies = {}, {}
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
                copy = copies[key] = copies.get(key, -1) + 1
                files[(key, copy)] = (path, size, path[len(tree):])
    # One process can load a file twice. An app's class loader namespace loads its own instance of
    # every platform library that is not public, and the system server loads the compiled code of its
    # class path again after the zygote has. Those files, and libraries whose bytes exist under several
    # paths, get one translation to spare. Spares sort last, so that adding one moves nothing else.
    for key, last in list(copies.items()):
        path, size, guest = files[(key, 0)]
        twice = any(files[(key, copy)][2].startswith(("/system/lib64/", "/system_ext/lib64/", "/product/lib64/")) or
                    ("/framework/oat/" in files[(key, copy)][2] and ".apk@classes" not in files[(key, copy)][2])
                    for copy in range(last + 1))
        if last > 0 or twice:
            files[(key, last + 1)] = (path, size, "~spare of " + guest)
    jobs, base = [], FIRST_BASE
    for (key, copy), (path, size, guest) in sorted(files.items(), key=lambda item: item[1][2]):
        image = path[:-4] + ".art"
        if path.endswith(".oat") and os.path.exists(image):
            # Compiled boot class path code. ART maps it where its image expects it, which the image
            # header records as oat_file_begin.
            with open(image, "rb") as file:
                start, = struct.unpack_from("<I", file.read(36), 32)
        else:
            # Bionic's CFI shadow has one entry per 256 KiB, which tells whose code is there: two
            # libraries must not share one (the linker's kLibraryAlignment).
            start = base
            base = (base + size + 0x10000 + 0x3FFFF) & ~0x3FFFF
        symbol = "m%016x_%d" % (key, copy)
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
    open(modules_file, "w").write("\n".join(lines) + "\n")

    sizes = [os.path.getsize(os.path.join(objects, job[0] + ".o")) for job in jobs]
    groups, size, share = [[] for _ in range(LIBRARIES)], 0, sum(sizes) / LIBRARIES
    for job, length in zip(jobs, sizes):
        groups[min(LIBRARIES - 1, int(size / share))].append(job[0])
        size += length
    svc = os.path.join(objects, "svc.o")
    subprocess.run(["clang", *TARGET, "-x", "assembler", "-c", "-", "-o", svc], input=SVC, text=True, check=True)

    def link(item):
        index, symbols = item
        library, listing = os.path.join(libraries, "libnative-%d.dylib" % index), os.path.join(out, "objects-%d.txt" % index)
        content = "".join(os.path.join(objects, symbol + ".o") + "\n" for symbol in symbols)
        if os.path.exists(library) and os.path.exists(listing) and open(listing).read() == content and \
                all(os.path.getmtime(os.path.join(objects, symbol + ".o")) < os.path.getmtime(library) for symbol in symbols):
            return
        open(listing, "w").write(content)
        result = subprocess.run(["clang", *TARGET, "-dynamiclib", "-o", library, "-install_name", "@rpath/" + os.path.basename(library),
                                 "-Wl,-filelist," + listing, svc], capture_output=True, text=True)
        if result.returncode:
            sys.exit("linking %s failed: %s" % (library, result.stderr[-2000:]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(link, enumerate(groups)))
    for name in os.listdir(libraries):
        if name not in {"libnative-%d.dylib" % index for index in range(len(groups))}:
            os.remove(os.path.join(libraries, name))
    open(os.path.join(out, "libraries.txt"), "w").write("".join("-lnative-%d\n" % index for index in range(len(groups))))
    print("translated=%d failed=%d libraries=%d" % (len(jobs), len(failed), len(groups)))


if __name__ == "__main__":
    main()
