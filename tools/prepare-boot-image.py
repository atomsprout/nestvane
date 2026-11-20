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
from zipfile import BadZipFile, ZipFile

from android_policy import capture_framework_app_policy


FRAMEWORK_APPS = (
    # PackageManager resolves this required shared library from its factory APK.
    "android.ext.shared",
    "com.android.documentsui",
    "com.android.externalstorage",
    "com.android.inputdevices",
    "com.android.inputmethod.latin",
    "com.android.keychain",
    "com.android.launcher3",
    "com.android.location.fused",
    "com.android.modulemetadata",
    "com.android.packageinstaller",
    "com.android.providers.downloads",
    "com.android.providers.downloads.ui",
    "com.android.providers.settings",
    "com.android.server.telecom",
    "com.android.settings",
    "com.android.shell",
    "com.android.systemui",
)

FRAMEWORK_APP_APEXES = ("com.android.extservices", "com.android.mediaprovider", "com.android.permission")
LATIN_IME_LIBRARY = "/product/lib64/libjni_latinime.so"
TETHERING_APEX = "/system/apex/com.android.tethering.apex"
TETHERING_LOADER = "nestvane-metadata/tethering-netbpfload.elf"


def copy_framework_apps(adb, remote, active_modules, guest):
    paths = set()
    for package in FRAMEWORK_APPS:
        files = remote("pm", "path", package).splitlines()
        if not files:
            raise SystemExit(f"Reference system APK is missing: {package}")
        for entry in files:
            match = re.fullmatch(r"package:(/(?:system|system_ext|product)/(?:app|priv-app)/"
                                 r"[A-Za-z0-9_.+-]+/[A-Za-z0-9_.+-]+\.apk)", entry)
            if not match:
                raise SystemExit(f"Expected a factory system APK for {package}: {entry}")
            paths.add(match[1])
    for path in sorted(paths):
        local = guest / path.lstrip("/")
        local.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(adb + ["pull", path, str(local)], check=True)
        with ZipFile(local) as apk:
            if "AndroidManifest.xml" not in apk.namelist():
                raise SystemExit(f"Invalid factory APK: {path}")
    # The factory keyboard loads its JNI library from the product namespace.
    keyboard_library = guest / LATIN_IME_LIBRARY.lstrip("/")
    keyboard_library.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(adb + ["pull", LATIN_IME_LIBRARY, str(keyboard_library)], check=True)
    with keyboard_library.open("rb") as library:
        header = library.read(20)
    if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 183:
        raise SystemExit(f"Expected an ARM64 ELF for {LATIN_IME_LIBRARY}")
    for module in FRAMEWORK_APP_APEXES:
        path = active_modules.get(module, "")
        if not re.fullmatch(r"/system/apex/[A-Za-z0-9_.-]+\.apex", path):
            raise SystemExit(f"Expected an active factory system APEX file for {module}")
        local = guest / path.lstrip("/")
        if not local.is_file():
            subprocess.run(adb + ["pull", path, str(local)], check=True)
    print(f"Added {len(paths)} factory APKs with {len(FRAMEWORK_APP_APEXES)} framework APEX dependencies", flush=True)


def check_reference_files(remote, root, files):
    digests = dict((path, value) for value, path in (
        line.split(maxsplit=1) for line in remote("sha256sum", *sorted(files)).splitlines()
    ))
    for path in sorted(files):
        digest = hashlib.sha256()
        with (root / path.lstrip("/")).open("rb") as file:
            for chunk in iter(lambda: file.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != digests.get(path):
            raise SystemExit(f"Boot input differs from SDK reference: {path}")


def precompile_framework_code(adb, remote, guest, stage, bootclasspath):
    # These are the class loader contexts of the matching SDK's prebuilt files.
    location_provider = "/system/framework/com.android.location.provider.jar"
    extensions = "/system_ext/framework/androidx.window.extensions.jar"
    sidecar = "/system_ext/framework/androidx.window.sidecar.jar"
    targets = (
        ("/system/framework/services.jar", f"PCL[{location_provider}]"),
        ("/system_ext/priv-app/SystemUI/SystemUI.apk", f"PCL[]{{PCL[{extensions}]#PCL[{sidecar}]}}"),
    )
    check_reference_files(remote, guest, {source for source, _ in targets} |
                          {location_provider, extensions, sidecar})
    # ART resolves the boot image location through the arm64 subdirectory.
    remote("mkdir", stage + "/arm64")
    for suffix in ("art", "oat", "vdex"):
        remote("mv", f"{stage}/boot.{suffix}", f"{stage}/arm64/boot.{suffix}")
    for source, context in targets:
        dex = Path(source.lstrip("/"))
        folder = guest / dex.parent / "oat/arm64"
        folder.mkdir(parents=True, exist_ok=True)
        base = f"{stage}/{dex.stem}"
        command = [
            "/apex/com.android.art/bin/dex2oat64",
            "--runtime-arg", "-Xbootclasspath:" + ":".join(bootclasspath),
            "--runtime-arg", "-Xgc:CC", "--runtime-arg", "-Xmx384m", "-j2",
            "--instruction-set=arm64", "--instruction-set-features=default",
            "--boot-image=" + stage + "/boot.art", "--compiler-filter=speed",
            "--dex-file=" + source, "--dex-location=" + source,
            "--class-loader-context=" + context, "--avoid-storing-invocation",
            "--oat-file=" + base + ".odex", "--output-vdex=" + base + ".vdex",
        ]
        subprocess.run(adb + ["shell", shlex.join(command)], check=True)
        for suffix in ("odex", "vdex"):
            artifact = folder / f"{dex.stem}.{suffix}"
            subprocess.run(adb + ["pull", f"{base}.{suffix}", str(artifact)], check=True)
            if artifact.stat().st_size == 0:
                raise SystemExit(f"Reference produced an empty {artifact.name}")


def replace_line(path, pattern, value):
    updated, count = re.subn(pattern, value, path.read_text(), flags=re.MULTILINE)
    if count != 1:
        raise SystemExit(f"Expected one match for {pattern!r} in {path}")
    path.write_text(updated)


def write_guest_hardware_profile(guest):
    # The guest has no Bluetooth controller or location hardware bridge.
    config = guest / "system/etc/sysconfig/nestvane-hardware.xml"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text("""<?xml version="1.0" encoding="utf-8"?>
<config>
    <unavailable-feature name="android.hardware.bluetooth" />
    <unavailable-feature name="android.hardware.bluetooth_le" />
    <unavailable-feature name="android.hardware.location" />
    <unavailable-feature name="android.hardware.location.gps" />
    <unavailable-feature name="android.hardware.location.network" />
</config>
""", encoding="utf-8")


def make_framework_root(base, image, guest, defer_zygote_preload, hardware_timeout_multiplier=None,
                        tethering_loader=None):
    shutil.copytree(base, guest)
    write_guest_hardware_profile(guest)
    target = guest / "system/framework/arm64"
    for path in target.iterdir():
        if re.fullmatch(r"boot(?:-[A-Za-z0-9_.+-]+)?\.(?:art|oat|vdex)", path.name):
            path.unlink()
    for path in image.iterdir():
        shutil.copyfile(path, target / path.name)

    init_rc = guest / "system/etc/init/hw/init.rc"
    target = guest / TETHERING_LOADER
    if tethering_loader is None:
        target.unlink(missing_ok=True)
        replace_line(init_rc, r"^([ \t]+)trigger load-bpf-programs$",
                     r"\1# Omitted in the offline framework diagnostic")
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(tethering_loader)
        print(f"Staged Tethering loader SHA-256 {hashlib.sha256(tethering_loader).hexdigest()}", flush=True)
    # The host supplies /data for this profile, so fs_mgr does not queue its normal event.
    replace_line(init_rc, r"^([ \t]+)trigger boot$", r"\1trigger boot\n\1trigger nonencrypted")
    replace_line(guest / "product/etc/build.prop", r"^ro\.dalvik\.vm\.enable_uffd_gc=[^\r\n]*$",
                 "ro.dalvik.vm.enable_uffd_gc=false")
    system_properties = guest / "system/build.prop"
    replace_line(system_properties, r"^dalvik\.vm\.usejit=[^\r\n]*$", "dalvik.vm.usejit=false")
    # ART creates a code-cache memory file for every app even with JIT disabled.
    values = system_properties.read_text().rstrip("\n")
    cache = "dalvik.vm.jitmaxsize=8m"
    values, count = re.subn(r"^dalvik\.vm\.jitmaxsize=[^\r\n]*$", cache, values, flags=re.MULTILINE)
    system_properties.write_text((values if count else f"{values}\n{cache}") + "\n")
    if hardware_timeout_multiplier is not None:
        values = system_properties.read_text().rstrip("\n")
        setting = f"ro.hw_timeout_multiplier={hardware_timeout_multiplier}"
        values, count = re.subn(r"^ro\.hw_timeout_multiplier=[^\r\n]*$", setting, values, flags=re.MULTILINE)
        system_properties.write_text((values if count else f"{values}\n{setting}") + "\n")
    for key in ("heapgrowthlimit", "heapsize"):
        replace_line(guest / "vendor/build.prop", rf"^dalvik\.vm\.{key}=[^\r\n]*$", f"dalvik.vm.{key}=96m")
    if (guest / "vendor/lib64/egl/libEGL_emulation.so").is_file():
        # The SDK normally selects these drivers from Ranchu's early-init actions.
        vendor_properties = guest / "vendor/build.prop"
        values = vendor_properties.read_text().rstrip("\n")
        drivers = {"ro.hardware.egl": "emulation", "ro.hardware.gralloc": "ranchu",
                   "ro.boot.qemu.gltransport.name": "pipe"}
        if (guest / "vendor/bin/hw/android.hardware.camera.provider.ranchu").is_file():
            # Android derives ro.boot.* in init from the kernel bootconfig.
            (guest / "nestvane-metadata").mkdir(parents=True, exist_ok=True)
            (guest / "nestvane-metadata/bootconfig").write_text('androidboot.qemu.camera_protocol_ver = "1"\n')
            (guest / "system/etc/init/nestvane-camera.rc").write_text(
                "on property:ro.hardware.gralloc=ranchu\n"
                "    start vendor.camera-provider-ranchu\n"
            )
        for key, value in drivers.items():
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
    parser.add_argument("--compiler-filter", choices=("verify", "speed"), default="verify",
                        help="ART filter for the offline boot image")
    parser.add_argument("--framework-diagnostic", action="store_true",
                        help="Produce a complete offline diagnostic guest root using the compiled image")
    parser.add_argument("--framework-apps", action="store_true",
                        help="Include launcher, SystemUI and package-management apps from the matching reference")
    parser.add_argument("--precompile-framework-code", action="store_true",
                        help="Add offline speed-compiled services.jar and SystemUI code")
    parser.add_argument("--smoke-apk", type=Path,
                        help="Stage the APK from tools/build-smoke-apk.sh for an install and launch check")
    parser.add_argument("--tethering-loader", type=Path,
                        help="Use the output of tools/build-netbpfload.py and run the standard BPF init actions")
    parser.add_argument("--defer-zygote-preload", action="store_true",
                        help="Use Android's lazy-preload option in the offline framework diagnostic")
    parser.add_argument("--hardware-timeout-multiplier", type=int,
                        help="Scale Android's hardware timeouts in the offline framework diagnostic")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Choose a new output directory")
    if args.defer_zygote_preload and not args.framework_diagnostic:
        parser.error("--defer-zygote-preload requires --framework-diagnostic")
    if args.framework_apps and not args.framework_diagnostic:
        parser.error("--framework-apps requires --framework-diagnostic")
    if args.precompile_framework_code and not args.framework_apps:
        parser.error("--precompile-framework-code requires --framework-apps")
    if args.smoke_apk is not None:
        if not args.framework_apps:
            parser.error("--smoke-apk requires --framework-apps")
        try:
            with ZipFile(args.smoke_apk) as apk:
                if not {"AndroidManifest.xml", "classes.dex"}.issubset(apk.namelist()):
                    parser.error("--smoke-apk must contain an Android manifest and DEX")
        except (OSError, ValueError, BadZipFile) as error:
            parser.error(f"Invalid --smoke-apk: {error}")
    tethering_loader = None
    if args.tethering_loader is not None:
        if not args.framework_diagnostic:
            parser.error("--tethering-loader requires --framework-diagnostic")
        try:
            with args.tethering_loader.open("rb") as elf:
                tethering_loader = elf.read(1024 * 1024 + 1)
        except OSError as error:
            parser.error(f"Cannot read --tethering-loader: {error}")
        if (not 64 <= len(tethering_loader) <= 1024 * 1024 or
                tethering_loader[:7] != b"\x7fELF\x02\x01\x01" or
                tethering_loader[16:20] != b"\x03\x00\xb7\x00"):
            parser.error("--tethering-loader must be an AArch64 position-independent ELF executable")
    if args.hardware_timeout_multiplier is not None:
        if not args.framework_diagnostic or not 1 <= args.hardware_timeout_multiplier <= 0x7fffffff:
            parser.error("--hardware-timeout-multiplier requires --framework-diagnostic and a positive Android int")
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
    bootclasspath = []
    if args.precompile_framework_code:
        full_classpath = re.search(r"^export BOOTCLASSPATH (\S+)$", path_input, re.MULTILINE)
        if not full_classpath:
            raise SystemExit("The reference did not export BOOTCLASSPATH")
        bootclasspath = full_classpath[1].split(":")
    if not jars or any(not re.fullmatch(r"/(?:apex|system)/[A-Za-z0-9_.+/\-]+\.jar", jar)
                       for jar in jars + bootclasspath):
        raise SystemExit("Unexpected boot class path")

    apex_info = ElementTree.fromstring(remote("cat", "/apex/apex-info-list.xml"))
    active_modules = {
        module.attrib["moduleName"]: module.attrib["modulePath"]
        for module in apex_info.findall("apex-info")
        if module.get("isActive") == "true" and module.get("isFactory") == "true"
    }
    reference_files = set()
    if args.framework_diagnostic:
        for module, path in (
            ("com.android.hardware.power", "/vendor/apex/com.android.hardware.power.apex"),
            ("com.android.hardware.gatekeeper", "/vendor/apex/com.android.hardware.gatekeeper.nonsecure.apex"),
        ):
            if active_modules.get(module) != path or not (args.root / path[1:]).is_file():
                raise SystemExit(f"Recollect the init root with the matching factory APEX for {module}")
            reference_files.add(path)
    if tethering_loader is not None:
        if active_modules.get("com.android.tethering") != TETHERING_APEX:
            raise SystemExit("Expected the factory Tethering APEX at " + TETHERING_APEX)
        reference_files.add(TETHERING_APEX)
    for jar in jars + bootclasspath:
        if jar.startswith("/system/"):
            reference_files.add(jar)
            continue
        module = jar.split("/")[2]
        factory_apex = active_modules.get(module, "")
        if not re.fullmatch(r"/system/apex/[A-Za-z0-9_.-]+\.apex", factory_apex):
            raise SystemExit(f"Expected an active factory APEX for {module}")
        reference_files.add(factory_apex)
    if args.precompile_framework_code:
        # These factory modules are pulled and checked when assembling the guest.
        reference_files.difference_update(active_modules[module] for module in FRAMEWORK_APP_APEXES
                                          if module in active_modules)
    check_reference_files(remote, args.root, reference_files)
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
            f"--compiler-filter={args.compiler_filter}", "--single-image", "--image-format=uncompressed",
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
                make_framework_root(args.root, root, guest, args.defer_zygote_preload,
                                    args.hardware_timeout_multiplier, tethering_loader)
                if args.framework_apps:
                    copy_framework_apps(adb, remote, active_modules, guest)
                    capture_framework_app_policy(adb, remote, guest)
                if args.precompile_framework_code:
                    check_reference_files(
                        remote, guest, {active_modules[module] for module in FRAMEWORK_APP_APEXES}
                    )
                    precompile_framework_code(adb, remote, guest, stage, bootclasspath)
                if args.smoke_apk is not None:
                    target = guest / "system/etc/nestvane/smoke.apk"
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(args.smoke_apk, target)
                guest.rename(args.output)
            else:
                root.rename(args.output)
    finally:
        remote("rm", "-rf", stage)


if __name__ == "__main__":
    main()
