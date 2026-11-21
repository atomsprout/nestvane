#!/usr/bin/env python3
"""Runs the scripts of compare.py through the translated Bionic guest on this Mac and through the
same binaries on a reference Linux kernel (chroot on a rooted Android emulator), and compares them.

usage: compare-android.py <generated/g0/e3/android> <adb serial> [--runs N]
       compare-android.py <build> --against <other build> [--runs N]   two builds on this Mac
       compare-android.py --export <file>      scripts for the iPhone run
"""
import functools
import hashlib
import os
import shutil
import subprocess
import sys
import time

from compare import BENCHMARKS, FUNCTIONAL, SETUP

print = functools.partial(print, flush=True)
DEVICE_ROOT = "/data/local/tmp/nv"
WORK = "/data/local/tmp/work"
TIMEOUT = 120
# The musl build runs its own toybox as ./toybox and has a toybox shell. Here commands are on the
# PATH and the shell is mksh. bc is not part of Android's toybox.
SKIP = {"bc"}


def adapt(text):
    return text.replace("./toybox ", "toybox ").replace("cat toybox", "cat /system/bin/toybox")


def scripts():
    result = {"setup": adapt(SETUP)}
    result.update({"f_" + name: adapt(text) for name, text in FUNCTIONAL.items() if name not in SKIP})
    result.update({"b_" + name.replace(" ", "_"): adapt(text) for name, text in BENCHMARKS.items() if not name.startswith("bc")})
    return result


def digest(data):
    return hashlib.sha256(data).hexdigest()[:12]


def mac(build, name):
    begin = time.perf_counter()
    try:
        program = os.path.join(build, "toybox-android")
        result = subprocess.run([program if os.path.exists(program) else os.path.join(build, "guest"), os.path.join(build, "root"), "/system/bin/sh", "-c",
                                 f"cd {WORK} && PATH=/system/bin sh scripts/{name}"], capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return b"", 124, TIMEOUT * 1000
    return result.stdout, result.returncode, (time.perf_counter() - begin) * 1000


def linux(serial, name):
    command = (f"S=$({DEVICE_ROOT}/mono); timeout {TIMEOUT} chroot {DEVICE_ROOT} /system/bin/sh -c "
               f"'cd {WORK} && PATH=/system/bin sh scripts/{name}' > /data/local/tmp/nv-out.bin 2>/dev/null; "
               f"R=$?; E=$({DEVICE_ROOT}/mono); echo $R $S $E")
    status = subprocess.run(["adb", "-s", serial, "shell", command], capture_output=True, text=True).stdout.split()
    output = subprocess.run(["adb", "-s", serial, "exec-out", "cat /data/local/tmp/nv-out.bin"], capture_output=True).stdout
    return output, int(status[0]), (int(status[2]) - int(status[1])) / 1e6  # the device shell's arithmetic is 32-bit


def against(build, other):
    """The same scripts through two builds on this Mac, such as the plain and the sparse runtime."""
    runs = int(sys.argv[sys.argv.index("--runs") + 1]) if "--runs" in sys.argv else 3
    for directory in (build, other):
        work = os.path.join(directory, "root") + WORK
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(os.path.join(work, "scripts"))
        for name, text in scripts().items():
            open(os.path.join(work, "scripts", name), "w").write(text + "\n")
        print("setup %s exit=%d ms=%.0f" % ((os.path.basename(directory),) + mac(directory, "setup")[1:]))
    different = 0
    for name in scripts():
        if name == "setup":
            continue
        benchmark = name.startswith("b_")
        best, outputs = [1e18, 1e18], [None, None]
        for _ in range(runs if benchmark else 1):
            for index, directory in enumerate((build, other)):
                output, status, elapsed = mac(directory, name)
                outputs[index] = (digest(output), status)
                best[index] = min(best[index], elapsed)
        different += outputs[0] != outputs[1]
        print("%-22s exit=%d sha=%s other=%s ms=%.0f other_ms=%.0f ratio=%.3f" % (
            name, outputs[0][1], outputs[0][0], "same" if outputs[0] == outputs[1] else "DIFFERENT exit=%d sha=%s" % outputs[1][::-1],
            best[0], best[1], best[0] / best[1]))
    print("total scripts=%d different=%d" % (len(scripts()) - 1, different))


def main():
    if sys.argv[1] == "--export":
        open(sys.argv[2], "wb").write(b"".join(name.encode() + b"\0" + text.encode() + b"\0" for name, text in scripts().items()))
        return
    if sys.argv[2] == "--against":
        return against(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[3]))
    build, serial = os.path.abspath(sys.argv[1]), sys.argv[2]
    runs = int(sys.argv[sys.argv.index("--runs") + 1]) if "--runs" in sys.argv else 3
    work = os.path.join(build, "root") + WORK
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "scripts"))
    for name, text in scripts().items():
        open(os.path.join(work, "scripts", name), "w").write(text + "\n")

    adb = ["adb", "-s", serial]
    subprocess.run(adb + ["shell", f"umount {DEVICE_ROOT}/dev {DEVICE_ROOT}/proc 2>/dev/null; rm -rf {DEVICE_ROOT}"], check=True)
    archive = subprocess.run(["tar", "czf", "-", "-C", os.path.join(build, "root"), "."], capture_output=True, check=True).stdout
    subprocess.run(adb + ["shell", f"mkdir -p {DEVICE_ROOT} && tar xzf - -C {DEVICE_ROOT}"], input=archive, check=True)
    subprocess.run(adb + ["push", os.path.join(build, "..", "mono"), f"{DEVICE_ROOT}/mono"], check=True, capture_output=True)
    subprocess.run(adb + ["shell", f"chmod 755 {DEVICE_ROOT}/mono {DEVICE_ROOT}/system/bin/*; mkdir -p {DEVICE_ROOT}/dev && "
                                    f"mount -o bind /dev {DEVICE_ROOT}/dev && mkdir -p {DEVICE_ROOT}/proc && mount -t proc proc {DEVICE_ROOT}/proc"], check=True)

    targets = {"linux": functools.partial(linux, serial), "mac": functools.partial(mac, build)}
    for label, run in targets.items():
        print("setup %s exit=%d ms=%.0f" % ((label,) + run("setup")[1:]))
    different = 0
    functional = [name for name in scripts() if name.startswith("f_")]
    for name in functional:
        expected, actual = linux(serial, name)[:2], mac(build, name)[:2]
        same = expected == actual
        different += not same
        print("functional %-15s exit=%d bytes=%-6d sha=%s mac=%s" % (name[2:], expected[1], len(expected[0]), digest(expected[0]),
              "same" if same else "DIFFERENT(exit %d, %s)" % (actual[1], digest(actual[0]))))
    print("functional_total tests=%d different=%d" % (len(functional), different))
    print("benchmark runs=%d, best time in ms" % runs)
    for name in [name for name in scripts() if name.startswith("b_")]:
        best, outputs = {}, set()
        for _ in range(runs):
            for label, run in targets.items():
                output, status, elapsed = run(name)
                outputs.add((status, output))
                best[label] = min(best.get(label, 1e18), elapsed)
        print("benchmark %-20s outputs_agree=%d linux=%.0f mac=%.0f mac_over_linux=%.3f" % (
            name[2:], len(outputs) == 1, best["linux"], best["mac"], best["mac"] / best["linux"]))
    subprocess.run(adb + ["shell", f"umount {DEVICE_ROOT}/dev {DEVICE_ROOT}/proc"], check=False)


if __name__ == "__main__":
    main()
