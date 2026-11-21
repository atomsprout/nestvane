#!/usr/bin/env python3
"""Turns a tree of AOSP build outputs into a guest root plus one host program that runs it.

Every ELF file in the tree is translated for a fixed load address and linked with the runtime.
The tree is laid out as on a device (system/..., apex/...), from a build patched by
experiments/aosp/patch.py.

usage: build_guest.py <source tree> <output directory> [--segment '(16ull << 30)'] [--sparse [--chunk-shift 24]] [--reuse]
Run the result with: <output>/guest <output>/root /system/bin/sh -c '...'

--sparse translates memory accesses through the per-chunk table and gives the guest a 39-bit address
space whose chunks get host memory when they are used. Without it one process is one block of
--segment bytes.
"""
import concurrent.futures
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRST_BASE = 0x10000000
RESERVED = ["-ffixed-x18", "-ffixed-x25", "-ffixed-x26", "-ffixed-x27", "-ffixed-x28"]
CHUNK_SHIFT = sys.argv[sys.argv.index("--chunk-shift") + 1] if "--chunk-shift" in sys.argv else "24"
SPARSE = ["--table", "--chunk-shift", CHUNK_SHIFT] if "--sparse" in sys.argv else []
REUSE = ["--reuse"] if "--reuse" in sys.argv else []


def ndk():
    root = os.environ.get("NDK") or sorted(os.listdir(os.path.expanduser("~/Library/Android/sdk/ndk")))[-1]
    if not os.path.isabs(root):
        root = os.path.expanduser("~/Library/Android/sdk/ndk/" + root)
    return root + "/toolchains/llvm/prebuilt/darwin-x86_64/bin/"


def copy_tree(source, root):
    """Copies the tree. Absolute links point into the guest root on a device, so they become relative."""
    for directory, _, files in os.walk(source):
        relative = os.path.relpath(directory, source)
        os.makedirs(os.path.join(root, relative), exist_ok=True)
        for name in files:
            path, target = os.path.join(directory, name), os.path.join(root, relative, name)
            if os.path.islink(path):
                link = os.readlink(path)
                if link.startswith("/"):
                    link = os.path.relpath(os.path.join(root, link.lstrip("/")), os.path.dirname(target))
                os.symlink(link, target)
            else:
                shutil.copy2(path, target)
    for name in os.listdir(source):  # directory links at the top, such as system/apex
        path = os.path.join(source, name)
        if os.path.islink(path) and not os.path.lexists(os.path.join(root, name)):
            os.symlink(os.readlink(path), os.path.join(root, name))


def load_size(path):
    """The span of an ELF's loadable segments, or None if the file is not a 64-bit arm64 ELF."""
    with open(path, "rb") as file:
        data = file.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or struct.unpack_from("<H", data, 18)[0] != 183:
        return None
    phoff, = struct.unpack_from("<Q", data, 32)
    size, count = struct.unpack_from("<HH", data, 54)
    ends = [struct.unpack_from("<Q", data, phoff + index * size + 16)[0] + struct.unpack_from("<Q", data, phoff + index * size + 40)[0]
            for index in range(count) if struct.unpack_from("<I", data, phoff + index * size)[0] == 1]
    executable = any(struct.unpack_from("<I", data, phoff + index * size)[0] == 1 and struct.unpack_from("<I", data, phoff + index * size + 4)[0] & 1
                     for index in range(count))
    return max(ends) if ends and executable else None


def translate(job):
    name, path, base, objects = job
    assembly = os.path.join(objects, name + ".S")
    summary = subprocess.run([sys.executable, os.path.join(HERE, "rewrite.py"), *SPARSE, *REUSE, "--base", hex(base), "--name", name, path, assembly],
                             capture_output=True, text=True, check=True).stdout.splitlines()[0]
    subprocess.run(["clang", "-arch", "arm64", "-c", assembly, "-o", os.path.join(objects, name + ".o")], check=True)
    return name, summary


def main():
    source, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    segment = sys.argv[sys.argv.index("--segment") + 1] if "--segment" in sys.argv else "(16ull << 30)"
    root, objects = os.path.join(out, "root"), os.path.join(out, "obj")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(objects)
    copy_tree(source, root)
    os.makedirs(os.path.join(root, "data/local/tmp"), exist_ok=True)
    os.makedirs(os.path.join(root, "system/bin"), exist_ok=True)

    # On a device the linker and the Bionic libraries live in the runtime APEX and /system links to them.
    # The bootstrap copies are for early boot, and the bootstrap linker would prefer them: use them only
    # when the tree has no runtime APEX, and never keep both.
    apex_runtime = os.path.join(root, "apex/com.android.runtime")
    linker = os.path.join(root, "system/bin/linker64")
    if os.path.lexists(linker) and not os.path.exists(linker):
        os.remove(linker)
    if not os.path.exists(linker):
        if os.path.exists(os.path.join(apex_runtime, "bin/linker64")):
            os.symlink("../../apex/com.android.runtime/bin/linker64", linker)
        else:
            shutil.copy2(os.path.join(root, "system/bin/bootstrap/linker64"), linker)
    for library in ("libc.so", "libm.so", "libdl.so"):
        target = os.path.join(root, "system/lib64", library)
        if not os.path.exists(target):
            if os.path.lexists(target):
                os.remove(target)
            shutil.copy2(os.path.join(root, "system/lib64/bootstrap", library), target)
    shutil.rmtree(os.path.join(root, "system/bin/bootstrap"), ignore_errors=True)
    shutil.rmtree(os.path.join(root, "system/lib64/bootstrap"), ignore_errors=True)

    # libnativeloader refuses to start without the list of public libraries. A full image has it.
    public = os.path.join(root, "system/etc/public.libraries.txt")
    if not os.path.exists(public):
        os.makedirs(os.path.dirname(public), exist_ok=True)
        open(public, "w").write("libc.so\nlibm.so\nlibdl.so\nliblog.so\n")

    # A minimal linker configuration: one default namespace that can load everything, plus the "system"
    # namespace libnativeloader asks for, which holds nothing itself and resolves through the default
    # one so that no library is loaded twice. A booted device generates this with linkerconfig.
    libraries = sorted({os.path.dirname(os.path.join(directory, name))[len(root):] for directory, _, files in os.walk(root)
                        for name in files if name.endswith(".so") and "/hwasan" not in directory})
    binaries = sorted({directory[len(root):] for directory, _, files in os.walk(root) if os.path.basename(directory) == "bin"})
    # libnativeloader also looks up one namespace per APEX, named after it.
    extra = ["system"] + sorted(name.replace(".", "_") for name in os.listdir(os.path.join(root, "apex"))) if os.path.isdir(os.path.join(root, "apex")) else ["system"]
    lines = ["dir.guest = %s/" % directory for directory in binaries] + [
        "[guest]", "additional.namespaces = " + ",".join(extra), "namespace.default.isolated = false", "namespace.default.visible = true",
        "namespace.default.search.paths = " + ":".join(libraries), "namespace.default.links = " + ",".join(extra)]
    for name in extra:
        lines += ["namespace.default.link.%s.allow_all_shared_libs = true" % name, "namespace.%s.isolated = false" % name,
                  "namespace.%s.visible = true" % name, "namespace.%s.search.paths = /nonexistent" % name,
                  "namespace.%s.links = default" % name, "namespace.%s.link.default.allow_all_shared_libs = true" % name]
    os.makedirs(os.path.join(root, "linkerconfig"), exist_ok=True)
    open(os.path.join(root, "linkerconfig/ld.config.txt"), "w").write("\n".join(lines) + "\n")

    # Test programs of our own, built like the platform: dynamic, against Bionic, registers reserved.
    for name in sorted(os.listdir(os.path.join(HERE, "guest-tests"))):
        if name.endswith(".c"):
            subprocess.run([ndk() + "clang", "--target=aarch64-linux-android35", "-O2", "-pie", *RESERVED, "-Wl,-z,max-page-size=16384",
                            os.path.join(HERE, "guest-tests", name), "-o", os.path.join(root, "system/bin/test-" + name[:-2])], check=True)

    # The Java test program, built beforehand from guest-tests/Hello.java with javac and d8.
    hello = os.path.join(os.path.dirname(out), "dex/hello.jar")
    if os.path.exists(hello):
        shutil.copy2(hello, os.path.join(root, "data/local/tmp/hello.jar"))

    # One module per distinct ELF file. Links to the same file share its translation.
    jobs, seen, base = [], set(), FIRST_BASE
    for directory, _, files in sorted(os.walk(root)):
        for name in sorted(files):
            path = os.path.join(directory, name)
            if os.path.islink(path):
                continue
            size = load_size(path)
            identity = os.stat(path).st_ino
            if size is None or identity in seen:
                continue
            seen.add(identity)
            guest = path[len(root):]
            symbol = "".join(character if character.isalnum() else "_" for character in guest.strip("/"))
            image = path[:-4] + ".art"
            if path.endswith(".oat") and os.path.exists(image):
                # Compiled boot class path code. ART maps it where its image expects it (with -Xnorelocate),
                # which the image header records as oat_file_begin.
                with open(image, "rb") as file:
                    jobs.append((symbol, path, struct.unpack_from("<I", file.read(36), 32)[0], objects, guest))
                continue
            jobs.append((symbol, path, base, objects, guest))
            base = (base + size + 0x1ffff) & ~0xffff
    print("modules=%d prelinked 0x%x-0x%x" % (len(jobs), FIRST_BASE, base))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        summaries = dict(pool.map(translate, [job[:4] for job in jobs]))
    for symbol, path, start, _, guest in jobs:
        print("%-58s base=0x%x %s" % (guest, start, " ".join(summaries[symbol].split()[:3])))

    subprocess.run([sys.executable, os.path.join(HERE, "modules.py"), os.path.join(objects, "modules.c")]
                   + ["%s=%s@%d" % (symbol, guest, start) for symbol, _, start, _, guest in jobs], check=True)
    program = os.path.join(out, "guest")
    subprocess.run(["clang", "-O2", "-arch", "arm64", "-DCHUNK_SHIFT=" + CHUNK_SHIFT, "-DSPARSE" if SPARSE else "-DSEGMENT=" + segment,
                    os.path.join(HERE, "runtime.c"), os.path.join(HERE, "entry.S"),
                    os.path.join(objects, "modules.c")] + [os.path.join(objects, job[0] + ".o") for job in jobs] + ["-o", program], check=True)

    # toybox picks its command from the name it is run as, so give every command a link, as Android does.
    toybox = os.path.join(root, "system/bin/toybox")
    if os.path.exists(toybox):
        for applet in subprocess.run([program, root, "/system/bin/toybox"], capture_output=True, text=True).stdout.split():
            if not os.path.lexists(os.path.join(root, "system/bin", applet)):
                os.symlink("toybox", os.path.join(root, "system/bin", applet))
    print("program=%s bytes=%d" % (program, os.path.getsize(program)))

    # A launcher for dalvikvm with the boot class path in the order the boot image was compiled for.
    if os.path.exists(os.path.join(root, "apex/com.android.art/bin/dalvikvm64")):
        jars = ["/apex/com.android.art/javalib/%s.jar" % name for name in ("core-oj", "core-libart", "okhttp", "bouncycastle", "apache-xml")]
        jars += ["/system/framework/%s.jar" % name for name in ("framework", "framework-graphics", "framework-location", "ext", "telephony-common",
                                                               "voip-common", "ims-common", "framework-ondeviceintelligence-platform")]
        jars.append("/apex/com.android.i18n/javalib/core-icu4j.jar")
        jars = [jar for jar in jars if os.path.exists(root + jar)]
        launcher = os.path.join(out, "run-art.sh")
        open(launcher, "w").write("""#!/bin/sh
# usage: run-art.sh [dalvikvm options...] -cp <jar> <class> [args]
# Add -Ximage:/system/framework/boot.art -Xnorelocate to run the compiled boot class path, or -Xint to interpret.
here=$(dirname "$0")
exec "$here/${GUEST:-guest}" "$here/root" /system/bin/toybox env ANDROID_ROOT=/system ANDROID_DATA=/data \\
  ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \\
  /apex/com.android.art/bin/dalvikvm64 -Xusejit:false -Xnoimage-dex2oat -Xbootclasspath:%s "$@"
""" % ":".join(jars))
        os.chmod(launcher, 0o755)
        # The same runs as scripts for the iOS probe app (G0Art), which runs each by name under mksh.
        # exec, so that the shell does not fork: every process reserves a whole segment of address space.
        command = ("exec /system/bin/toybox env ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n "
                   "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata /apex/com.android.art/bin/dalvikvm64 -Xusejit:false -Xnoimage-dex2oat "
                   "-Xbootclasspath:%s %%s -cp /data/local/tmp/hello.jar Hello %%s\n" % ":".join(jars))
        image = "-Xgc:CMC -Ximage:/system/framework/boot.art -Xnorelocate"
        scripts = [("art_interpreted", command % ("-Xint", "interpreted")), ("art_image_interpreted", command % ("-Xint " + image, "image")),
                   ("art_compiled", command % (image, "compiled")), ("b_art_interpreted", command % ("-Xint", "interpreted")),
                   ("b_art_compiled", command % (image, "compiled"))]
        tests = [("t_" + name[5:], name + "\n") for name in sorted(os.listdir(os.path.join(root, "system/bin"))) if name.startswith("test-")]
        subprocess.run([sys.executable, os.path.join(HERE, "compare-android.py"), "--export", os.path.join(out, "scripts-android.bin")], check=True)
        with open(os.path.join(out, "scripts-android.bin"), "a") as file:
            file.write("".join("%s\0%s\0" % script for script in tests + scripts))


if __name__ == "__main__":
    main()
