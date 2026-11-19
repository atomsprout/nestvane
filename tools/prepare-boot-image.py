#!/usr/bin/env python3
"""Precompile a read-barrier-compatible ARM64 boot image on the Android SDK reference."""
import argparse
import hashlib
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
from xml.etree import ElementTree


def replace_line(path, pattern, value):
    updated, count = re.subn(pattern, value, path.read_text(), flags=re.MULTILINE)
    if count != 1:
        raise SystemExit(f"Expected one match for {pattern!r} in {path}")
    path.write_text(updated)


def make_framework_root(base, image, guest, defer_zygote_preload):
    shutil.copytree(base, guest)
    target = guest / "system/framework/arm64"
    for path in target.iterdir():
        if re.fullmatch(r"boot(?:-[A-Za-z0-9_.+-]+)?\.(?:art|oat|vdex)", path.name):
            path.unlink()
    for path in image.iterdir():
        shutil.copyfile(path, target / path.name)

    init_rc = guest / "system/etc/init/hw/init.rc"
    replace_line(init_rc, r"^([ \t]+)trigger load-bpf-programs$",
                 r"\1# Omitted in the offline framework diagnostic")
    # The host supplies /data for this profile, so fs_mgr does not queue its normal event.
    replace_line(init_rc, r"^([ \t]+)trigger boot$", r"\1trigger boot\n\1trigger nonencrypted")
    replace_line(guest / "product/etc/build.prop", r"^ro\.dalvik\.vm\.enable_uffd_gc=[^\r\n]*$",
                 "ro.dalvik.vm.enable_uffd_gc=false")
    replace_line(guest / "system/build.prop", r"^dalvik\.vm\.usejit=[^\r\n]*$", "dalvik.vm.usejit=false")
    for key in ("heapgrowthlimit", "heapsize"):
        replace_line(guest / "vendor/build.prop", rf"^dalvik\.vm\.{key}=[^\r\n]*$", f"dalvik.vm.{key}=96m")
    if (guest / "vendor/lib64/egl/libEGL_emulation.so").is_file():
        # The SDK normally selects these drivers from Ranchu's early-init actions.
        vendor_properties = guest / "vendor/build.prop"
        values = vendor_properties.read_text().rstrip("\n")
        for key, value in (("ro.hardware.egl", "emulation"), ("ro.hardware.gralloc", "ranchu"),
                           ("ro.boot.qemu.gltransport.name", "pipe")):
            if not re.search(rf"^{re.escape(key)}=", values, flags=re.MULTILINE):
                values += f"\n{key}={value}"
        vendor_properties.write_text(values + "\n")
    if defer_zygote_preload:
        replace_line(guest / "system/etc/init/hw/init.zygote64.rc",
                     r"^(service zygote /system/bin/app_process64 [^\r\n]*--start-system-server[^\r\n]*)$",
                     r"\1 --enable-lazy-preload")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial")
    parser.add_argument("--root", type=Path, required=True, help="Unpacked boot input used to check the reference build")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--framework-diagnostic", action="store_true",
                        help="Produce a complete offline diagnostic guest root using the compiled image")
    parser.add_argument("--defer-zygote-preload", action="store_true",
                        help="Use Android's lazy-preload option in the offline framework diagnostic")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Choose a new output directory")
    if args.defer_zygote_preload and not args.framework_diagnostic:
        parser.error("--defer-zygote-preload requires --framework-diagnostic")
    if args.framework_diagnostic and args.root.resolve() in args.output.resolve().parents:
        parser.error("Choose an output directory outside the input guest root")
    adb = ["adb", "-s", args.serial]

    def remote(*words):
        return subprocess.check_output(adb + ["shell", shlex.join(words)], text=True).strip()

    properties = dict(line.split("=", 1) for line in (args.root / "system/build.prop").read_text().splitlines()
                      if line and not line.startswith("#") and "=" in line)
    if (not args.serial.startswith("emulator-") or remote("getprop", "ro.build.version.sdk") != "37" or
            remote("getprop", "ro.product.cpu.abi") != "arm64-v8a" or
            not properties.get("ro.build.id") or remote("getprop", "ro.build.id") != properties["ro.build.id"]):
        raise SystemExit("Reference must be the matching Android 17 ARM64 SDK emulator")

    path_input = remote("cat", "/data/system/environ/classpath")
    match = re.search(r"^export DEX2OATBOOTCLASSPATH (\S+)$", path_input, re.MULTILINE)
    if not match:
        raise SystemExit("The reference did not export DEX2OATBOOTCLASSPATH")
    jars = match[1].split(":")
    if not jars or any(not re.fullmatch(r"/(?:apex|system)/[A-Za-z0-9_.+/\-]+\.jar", jar) for jar in jars):
        raise SystemExit("Unexpected boot class path")

    apex_info = ElementTree.fromstring(remote("cat", "/apex/apex-info-list.xml"))
    active_modules = {
        module.attrib["moduleName"]: module.attrib["modulePath"]
        for module in apex_info.findall("apex-info")
        if module.get("isActive") == "true" and module.get("isFactory") == "true"
    }
    reference_files = set()
    for jar in jars:
        if jar.startswith("/system/"):
            reference_files.add(jar)
            continue
        module = jar.split("/")[2]
        factory_apex = active_modules.get(module, "")
        if not re.fullmatch(r"/system/apex/[A-Za-z0-9_.-]+\.apex", factory_apex):
            raise SystemExit(f"Expected an active factory APEX for {module}")
        reference_files.add(factory_apex)
    digests = dict((path, value) for value, path in (
        line.split(maxsplit=1) for line in remote("sha256sum", *sorted(reference_files)).splitlines()
    ))
    for guest in sorted(reference_files):
        digest = hashlib.sha256()
        with (args.root / guest.lstrip("/")).open("rb") as file:
            for chunk in iter(lambda: file.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != digests.get(guest):
            raise SystemExit(f"Boot input differs from SDK reference: {guest}")
    print(f"Matched {len(reference_files)} boot inputs to {properties['ro.build.id']}", flush=True)

    stage = remote("mktemp", "-d", "/data/local/tmp/nestvane-preopt.XXXXXX")
    if not re.fullmatch(r"/data/local/tmp/nestvane-preopt\.[A-Za-z0-9]+", stage):
        raise SystemExit("Unexpected reference work directory")
    try:
        base = f"{stage}/boot"
        command = [
            "/apex/com.android.art/bin/dex2oat64",
            "--runtime-arg", "-Xbootclasspath:" + ":".join(jars),
            "--runtime-arg", "-Xgc:CC",
            "--runtime-arg", "-Xmx1g",
            "--base=0x70000000",
            "--instruction-set=arm64", "--instruction-set-features=default",
            "--compiler-filter=verify", "--single-image", "--image-format=uncompressed",
            "--image=" + base + ".art", "--oat-file=" + base + ".oat",
            "--oat-location=/system/framework/arm64/boot.oat",
            "--output-vdex=" + base + ".vdex", "--avoid-storing-invocation",
        ]
        for jar in jars:
            command.extend(("--dex-file=" + jar, "--dex-location=" + jar))
        subprocess.run(adb + ["shell", shlex.join(command)], check=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
            root = Path(temporary) / "arm64"
            root.mkdir()
            for suffix in ("art", "oat", "vdex"):
                name = f"boot.{suffix}"
                subprocess.run(adb + ["pull", f"{stage}/{name}", str(root / name)], check=True)
                if (root / name).stat().st_size == 0:
                    raise SystemExit(f"Reference produced an empty {name}")
            if args.framework_diagnostic:
                guest = Path(temporary) / "guest"
                make_framework_root(args.root, root, guest, args.defer_zygote_preload)
                guest.rename(args.output)
            else:
                root.rename(args.output)
    finally:
        remote("rm", "-rf", stage)


if __name__ == "__main__":
    main()
