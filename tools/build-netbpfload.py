#!/usr/bin/env python3
"""Build the Android 17 guest loader for an isolated Nestvane diagnostic."""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess
import tempfile


TAG = "refs/tags/android-17.0.0_r1"
CONNECTIVITY = "platform/packages/modules/Connectivity"
HERE = Path(__file__).resolve().parent
FLAG = "com.android.tethering.readonly.flags.use_libbpf"

# The official Soong target generates the flags header. The small NDK build
# reads the equivalent read-only flag and links to the paired reference APEX.
LIBRARIES = {
    "libbase.so": "/apex/com.android.tethering/lib64/libbase.so",
    "liblog.so": "/system/lib64/liblog.so",
    "libbpf.so": "/apex/com.android.tethering/lib64/libbpf.so",
    "libc++.so": "/apex/com.android.tethering/lib64/libc++.so",
}


def fetch(root, repo, source, destination):
    url = f"https://android.googlesource.com/{repo}/+/{TAG}/{source}?format=TEXT"
    encoded = subprocess.check_output(["curl", "-fsSL", "--retry", "2", "--max-time", "30", url])
    path = root / destination
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(base64.b64decode(encoded))


def stage_sources(root):
    inputs = [
        (CONNECTIVITY, f"bpf/loader/{name}", f"bpf/loader/{name}")
        for name in ("NetBpfLoad.cpp", "Android.bp")
    ]
    inputs.extend([
        (CONNECTIVITY, "bpf/syscall_wrappers/include/BpfSyscallWrappers.h", "include/BpfSyscallWrappers.h"),
        (CONNECTIVITY, "bpf/progs/include/bpf_map_def.h", "include/bpf_map_def.h"),
        ("platform/system/core", "libcutils/include/private/android_filesystem_config.h",
         "include/cutils/android_filesystem_config.h"),
    ])
    for name in ("BpfUtils.h", "KernelUtils.h"):
        inputs.append((CONNECTIVITY, f"bpf/headers/include/bpf/{name}", f"include/bpf/{name}"))
    with ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(lambda entry: fetch(root, *entry), inputs))
    for repository in ("system/libbase", "system/logging", "external/libbpf"):
        source = root / repository
        subprocess.run([
            "git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1",
            "--branch", TAG.rsplit("/", 1)[1], f"https://android.googlesource.com/platform/{repository}",
            str(source),
        ], check=True)
    for source, destination in (("system/libbase/include/android-base", "android-base"),
                                ("system/logging/liblog/include/log", "log")):
        shutil.copytree(root / source, root / "include" / destination)
    for name in ("btf", "libbpf", "libbpf_common", "libbpf_legacy", "libbpf_version"):
        shutil.copyfile(root / "external/libbpf/src" / f"{name}.h", root / "include/bpf" / f"{name}.h")
    subprocess.run(
        ["git", "apply", "--check", str(HERE / "patches/aosp-netbpfload-interpreter.patch")],
        cwd=root, check=True,
    )
    subprocess.run(
        ["git", "apply", str(HERE / "patches/aosp-netbpfload-interpreter.patch")],
        cwd=root, check=True,
    )


def stage_reference(root, adb):
    sdk = subprocess.check_output([*adb, "shell", "getprop", "ro.build.version.sdk"], text=True).strip()
    if sdk != "37":
        raise SystemExit(f"Expected an API 37 reference device, got {sdk!r}")
    fingerprint = subprocess.check_output([*adb, "shell", "getprop", "ro.build.fingerprint"], text=True).strip()
    flags = subprocess.check_output([*adb, "shell", "aflags", "list"], text=True)
    values = [fields[1] for line in flags.splitlines() if len(fields := line.split()) > 1 and fields[0] == FLAG]
    if len(values) != 1 or values[0] not in ("enabled", "disabled"):
        raise SystemExit(f"Unable to read {FLAG} from the reference device")
    enabled = str(values[0] == "enabled").lower()
    (root / "include/com_android_tethering_readonly_flags.h").write_text(
        "#pragma once\n"
        f"#define COM_ANDROID_TETHERING_READONLY_FLAGS_USE_LIBBPF {enabled}\n"
        "namespace com::android::tethering::readonly::flags {\n"
        "constexpr bool use_libbpf() { return COM_ANDROID_TETHERING_READONLY_FLAGS_USE_LIBBPF; }\n"
        "}\n"
    )
    (root / "lib").mkdir()
    for name, remote in LIBRARIES.items():
        subprocess.run([*adb, "pull", remote, str(root / "lib" / name)], check=True)
    return fingerprint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="adb serial for the paired ARM64 API 37 reference image")
    parser.add_argument("--toolchain", required=True, type=Path, help="NDK toolchains/llvm/prebuilt/<host> directory")
    parser.add_argument("--output", required=True, type=Path, help="Path for the diagnostic ARM64 ELF")
    args = parser.parse_args()
    toolchain = args.toolchain.resolve(strict=True)
    clang = toolchain / "bin/aarch64-linux-android31-clang++"
    if not clang.is_file():
        parser.error(f"NDK compiler not found at {clang}")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    adb = ["adb", "-s", args.serial]
    with tempfile.TemporaryDirectory(prefix="nestvane-netbpfload-", dir=output.parent) as temp:
        root = Path(temp)
        stage_sources(root)
        fingerprint = stage_reference(root, adb)
        config = (toolchain / "sysroot/usr/include/c++/v1/__config_site").read_text()
        ndk_namespace = "#define _LIBCPP_ABI_NAMESPACE __ndk1"
        if config.count(ndk_namespace) != 1:
            raise SystemExit("Expected an Android NDK libc++ configuration")
        # Link to the platform's libc++.so ABI rather than the NDK's libc++_shared.
        (root / "include/__config_site").write_text(
            config.replace(ndk_namespace, "#define _LIBCPP_ABI_NAMESPACE __1")
        )
        subprocess.run([
            str(clang), "-std=gnu++20", "-DNESTVANE_INTERPRETED_BPF", "-D__ANDROID_APEX__",
            "-U_FORTIFY_SOURCE", "-O2", "-fPIE", "-I", "include", "-c", "bpf/loader/NetBpfLoad.cpp",
            "-o", "netbpfload.o",
        ], cwd=root, check=True)
        subprocess.run([
            str(clang), "-nostdlib++", "-pie", "netbpfload.o", "-Llib",
            "-lbase", "-llog", "-lbpf", "-l:libc++.so", "-o", "netbpfload",
        ], cwd=root, check=True)
        shutil.move(root / "netbpfload", output)
    print(f"{output} ({TAG.rsplit('/', 1)[1]}, reference {fingerprint})")


if __name__ == "__main__":
    main()
