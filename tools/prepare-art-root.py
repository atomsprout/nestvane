#!/usr/bin/env python3
"""Collect the SDK's ARM64 ART and compile a minimal DEX fixture."""

import argparse
from collections import deque
from pathlib import Path
import re
import subprocess
import tempfile


def collect(serial, readelf, d8, destination):
    adb = ["adb", "-s", serial]

    def shell(*args):
        return subprocess.check_output(adb + ["shell", *args], text=True).strip()

    if not serial.startswith("emulator-") or shell("getprop", "ro.build.version.sdk") != "37":
        raise SystemExit("Select a running API 37 SDK emulator")
    if shell("getprop", "ro.product.cpu.abi") != "arm64-v8a":
        raise SystemExit("Select an ARM64 SDK image")
    print("Android build:", shell("getprop", "ro.build.id"), flush=True)

    art = "/apex/com.android.art"
    runtime = "/apex/com.android.runtime"
    i18n = "/apex/com.android.i18n"
    apex_libraries = (f"{art}/lib64", f"{runtime}/lib64/bionic", f"{i18n}/lib64")
    mounted_apex = tuple(
        path for path in shell("ls", "-d", "/apex/com.android.*/lib64").splitlines()
        if re.fullmatch(r"/apex/com\.android\.[a-z0-9_.]+/lib64", path) and path not in apex_libraries
    )
    libraries = (*apex_libraries, *mounted_apex, "/system/lib64", "/system_ext/lib64",
                 "/product/lib64", "/vendor/lib64")
    inventory = shell(
        "for path in " + " ".join(libraries) +
        '; do if [ -d "$path" ]; then for file in "$path"/*.so; do [ -f "$file" ] && echo "$file"; '
        "done; fi; done"
    )
    available = {directory: set() for directory in libraries}
    for path in inventory.splitlines():
        directory, _, name = path.rpartition("/")
        if directory in available and re.fullmatch(r"[A-Za-z0-9_.+@-]+\.so", name):
            available[directory].add(name)
    pending = deque((f"{art}/bin/dalvikvm64", f"{art}/lib64/libart.so",
                     f"{runtime}/bin/linker64", "/system/lib64/libartpalette-system.so",
                     "/linkerconfig/ld.config.txt", "/linkerconfig/com.android.art/ld.config.txt",
                     "/apex/apex-info-list.xml", "/system/etc/public.libraries.txt"))
    timezone = "/apex/com.android.tzdata/etc"
    for path in shell("find", timezone, "-type", "f").splitlines():
        if path.startswith(timezone + "/") and re.fullmatch(r"[A-Za-z0-9_./-]+", path):
            pending.append(path)
    for line in shell("cat", "/system/etc/public.libraries.txt").splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#") or "nopreload" in fields or "32" in fields:
            continue
        name = fields[0]
        candidates = [f"{directory}/{name}" for directory in libraries if name in available[directory]]
        if not candidates:
            raise SystemExit(f"Missing preloadable SDK library {name}")
        pending.extend(candidates)
    for directory in (f"{art}/javalib", f"{i18n}/javalib", f"{i18n}/etc/icu", f"{art}/etc"):
        for name in shell("ls", "-1", directory).splitlines():
            if re.fullmatch(r"[A-Za-z0-9_.+@-]+\.(?:jar|dat|prof|pb|map)", name):
                pending.append(f"{directory}/{name}")
    for directory in apex_libraries:
        for name in sorted(available[directory]):
            if re.fullmatch(r"[A-Za-z0-9_.+@-]+\.so", name):
                pending.append(f"{directory}/{name}")

    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        temp = Path(temporary)
        root = temp / "root"
        copied = set()
        while pending:
            guest = pending.popleft()
            if guest in copied:
                continue
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True, stdout=subprocess.DEVNULL)
            copied.add(guest)
            if not guest.endswith(".so") and "/bin/" not in guest:
                continue
            tags = subprocess.check_output([readelf, "-d", str(local)], text=True)
            for name in re.findall(r"\(NEEDED\).*?\[([A-Za-z0-9_.+@-]+)\]", tags):
                candidates = [f"{directory}/{name}" for directory in libraries if name in available[directory]]
                if not candidates and name not in ("ld-android.so", "libdl_android.so"):
                    raise SystemExit(f"{guest} requires missing library {name}")
                pending.extend(candidates)

        fixture = Path(__file__).resolve().parent.parent / "fixtures/NestvaneArt.java"
        subprocess.run(["javac", "--release", "8", "-Xlint:-options", "-d", str(temp), str(fixture)], check=True)
        dex = root / "data/local/tmp"
        dex.mkdir(parents=True, exist_ok=True)
        subprocess.run([d8, "--min-api", "28", "--output", str(dex),
                        str(temp / "NestvaneArt.class")], check=True)
        (dex / "classes.dex").rename(dex / "nestvane-art-probe.dex")
        root.rename(destination)
    print(f"Collected {len(copied)} Android files and a DEX fixture in {destination}", flush=True)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("serial")
parser.add_argument("--readelf", default="llvm-readelf")
parser.add_argument("--d8", required=True)
parser.add_argument("--output", type=Path, default=Path("generated/android17-art-root"))
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
if args.output.exists():
    parser.error(f"Choose a fresh output path: {args.output}")
collect(args.serial, args.readelf, args.d8, args.output)
