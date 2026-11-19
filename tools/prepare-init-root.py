#!/usr/bin/env python3
"""Collect Android 17 boot inputs from an ARM64 Android SDK emulator."""
import argparse
from collections import deque
from pathlib import Path
import re
import subprocess
import tempfile
from xml.etree import ElementTree

from android_policy import query_access


BOOT_APEX_MODULES = ("runtime", "i18n", "sdkext", "art", "tzdata", "os.statsd", "tethering", "conscrypt", "adbd")


def public_preload_libraries(config):
    for line in config.splitlines():
        fields = line.partition("#")[0].split()
        if not fields or "nopreload" in fields[1:] or "32" in fields[1:]:
            continue
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.so", fields[0]):
            raise SystemExit(f"Unexpected Android public library entry: {line}")
        yield fields[0]


def reference_classpath_packages(classpath, apex_info):
    required = {"BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH", "SYSTEMSERVERCLASSPATH", "STANDALONE_SYSTEMSERVER_JARS"}
    exports = {}
    for kind, value in re.findall(r"^export ([A-Z0-9_]+) ([^\r\n]*)$", classpath, re.MULTILINE):
        if kind in required:
            if kind in exports:
                raise SystemExit(f"Duplicate reference classpath: {kind}")
            exports[kind] = value
    if exports.keys() != required or not exports["BOOTCLASSPATH"] or not exports["SYSTEMSERVERCLASSPATH"]:
        raise SystemExit("Reference Java classpaths are incomplete")

    modules = {f"com.android.{name}" for name in BOOT_APEX_MODULES}
    platform = set()
    for value in exports.values():
        for jar in value.split(":") if value else ():
            if re.fullmatch(r"/system/framework/[A-Za-z0-9_.+-]+\.jar", jar):
                platform.add(jar)
                continue
            match = re.fullmatch(r"/apex/([A-Za-z0-9_.-]+)/javalib/[A-Za-z0-9_.+-]+\.jar", jar)
            if not match:
                raise SystemExit(f"Unsupported reference classpath entry: {jar}")
            modules.add(match[1])

    factory = {
        apex.get("moduleName"): apex.get("modulePath", "")
        for apex in ElementTree.fromstring(apex_info).findall("apex-info")
        if apex.get("isActive") == "true" and apex.get("isFactory") == "true"
    }
    packages = set()
    for module in sorted(modules):
        path = factory.get(module, "")
        if not re.fullmatch(r"/system/apex/[A-Za-z0-9_.-]+\.apex", path):
            raise SystemExit(f"Expected an active factory system APEX file for {module}")
        packages.add(path)
    return platform, packages


def collect(serial, readelf, destination, small_art_heap):
    adb = ["adb", "-s", serial]
    search = ("/system/lib64/bootstrap", "/system/lib64", "/system_ext/lib64")

    def shell(*args):
        return subprocess.check_output(adb + ["shell", *args], text=True).strip()

    if not serial.startswith("emulator-") or shell("getprop", "ro.build.version.sdk") != "37":
        raise SystemExit("Select a running API 37 SDK emulator with adb root enabled")
    if shell("getprop", "ro.product.cpu.abi") != "arm64-v8a":
        raise SystemExit("The reference emulator must use ARM64")
    print("Android build:", shell("getprop", "ro.build.id"), flush=True)
    classpath = shell("cat", "/data/system/environ/classpath")
    framework, packages = reference_classpath_packages(classpath, shell("cat", "/apex/apex-info-list.xml"))
    available = {}
    for directory in search:
        for name in shell("ls", "-1", directory).splitlines():
            available.setdefault(name, directory + "/" + name)

    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        root = Path(temporary) / "root"
        system_self_test = "/system/bin/boringssl_self_test64"
        vendor_self_test = "/vendor/bin/boringssl_self_test64"
        managers = ("/system/bin/servicemanager", "/system/system_ext/bin/hwservicemanager")
        pending = deque(["/system/bin/init", "/system/bin/bootstrap/linker64", "/system/bin/apexd",
                         system_self_test, *managers])
        copied = set()
        while pending:
            guest = pending.popleft()
            if guest in copied:
                continue
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True)
            copied.add(guest)
            info = subprocess.check_output([readelf, "-l", "-d", str(local)], text=True)
            for interpreter in re.findall(r"Requesting program interpreter: ([^\]]+)", info):
                expected = "/system/bin/linker64" if guest == system_self_test else "/system/bin/bootstrap/linker64"
                if interpreter != expected:
                    raise SystemExit(f"Unexpected interpreter for {guest}: {interpreter}")
            for name in re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", info):
                if name == "ld-android.so":
                    continue  # Provided by the ELF interpreter itself.
                if name not in available:
                    raise SystemExit(f"{guest} needs {name}, absent from the bootstrap search paths")
                pending.append(available[name])
        for guest in (vendor_self_test, "/vendor/lib64/libcrypto.so", "/vendor/lib64/libc++.so",
                      "/vendor/etc/boringssl_self_test.zygote64.rc"):
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True)
            copied.add(guest)
        # These binaries use the regular Bionic interpreter.
        vold = "/system/bin/vold"
        service = "/system/bin/service"
        cmd = "/system/bin/cmd"
        vdc = "/system/bin/vdc"
        keystore = "/system/bin/keystore2"
        casefold = "/system/bin/casefolding_remover"
        vold_prepare = "/system/bin/vold_prepare_subdirs"
        odsign = "/system/bin/odsign"
        zygote = "/system/bin/app_process64"
        installd = "/system/bin/installd"
        system_suspend = "/system/bin/hw/android.system.suspend-service"
        surfaceflinger = "/system/bin/surfaceflinger"
        bootanimation = "/system/bin/bootanimation"
        cameraserver = "/system/bin/cameraserver"
        aconfigd = "/system/bin/aconfigd-system"
        bpfloader = "/system/bin/bpfloader"
        netd = "/system/bin/netd"
        iptables = "/system/bin/iptables"
        uprobestats = "/apex/com.android.uprobestats/bin/uprobestatsbpfload"
        keymint = "/vendor/bin/hw/android.hardware.security.keymint-service"
        lights = "/vendor/bin/hw/android.hardware.lights-service.example"
        health = "/vendor/bin/hw/android.hardware.health-service.example"
        composer = "/vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu"
        allocator = "/vendor/bin/hw/android.hardware.graphics.allocator-service.ranchu"
        camera_provider = "/vendor/bin/hw/android.hardware.camera.provider.ranchu"
        for programs, search_paths in (
            ((vold, service, cmd, vdc, keystore, casefold, vold_prepare, odsign, bpfloader, iptables), ("/system/lib64",)),
            ((zygote, installd, system_suspend, surfaceflinger, bootanimation, aconfigd, netd, cameraserver), ("/system/lib64", "/system_ext/lib64")),
            ((keymint, lights, health, composer, allocator, camera_provider), ("/vendor/lib64", "/system/lib64")),
        ):
            dependencies = {}
            for directory in search_paths:
                for name in shell("ls", "-1", directory).splitlines():
                    dependencies.setdefault(name, f"{directory}/{name}")
            apex_libraries = {"libc.so", "libm.so", "libdl.so", "ld-android.so",
                              "libandroidicu.so", "libicui18n.so", "libicuuc.so", "libstatspull.so", "libstatssocket.so"}
            if zygote in programs:
                for module in BOOT_APEX_MODULES:
                    apex_libraries.update(shell(f"ls -1 /apex/com.android.{module}/lib64 2>/dev/null || true").splitlines())
                apex_libraries.difference_update(dependencies)
            pending = deque(programs)
            if odsign in programs:
                pending.extend(("/system/lib64/heapprofd_client_api.so", "/system/lib64/libartpalette-system.so"))
            if zygote in programs:
                # Preloaded libraries and APEX-to-system imports are not in app_process64's DT_NEEDED.
                pending.extend(("/system/lib64/libincident.so", "/system/lib64/libadbd_fs.so",
                                "/system/lib64/libadbd_auth.so", "/system/lib64/libandroid_servers.so",
                                "/system/lib64/libdrmframework_jni.so", "/system/lib64/libmedia_jni.so",
                                "/system/lib64/libsoundpool.so", "/system/lib64/libaudioeffect_jni.so",
                                "/system/lib64/librtp_jni.so", "/system/lib64/libvintf_jni.so",
                                "/system/lib64/librs_jni.so", "/system/lib64/libcompiler_rt.so",
                                "/system/lib64/libwebviewchromium_loader.so"))
                for name in public_preload_libraries(shell("cat", "/system/etc/public.libraries.txt")):
                    if name in apex_libraries or name in {"libc.so", "libm.so", "libdl.so"}:
                        continue
                    if name not in dependencies:
                        raise SystemExit(f"Public preload library {name} absent from platform and packaged APEX")
                    pending.append(dependencies[name])
            if composer in programs:
                # EGL and mapper implementations are selected at runtime.
                pending.extend(("/vendor/lib64/egl/libEGL_emulation.so",
                                "/vendor/lib64/egl/libGLESv1_CM_emulation.so",
                                "/vendor/lib64/egl/libGLESv2_emulation.so",
                                "/vendor/lib64/hw/mapper.ranchu.so", "/vendor/lib64/libqemupipe.ranchu.so"))
            inspected = set()
            while pending:
                guest = pending.popleft()
                if guest in inspected:
                    continue
                inspected.add(guest)
                local = root / guest.lstrip("/")
                if guest not in copied:
                    local.parent.mkdir(parents=True, exist_ok=True)
                    subprocess.run(adb + ["pull", guest, str(local)], check=True)
                    copied.add(guest)
                info = subprocess.check_output([readelf, "-l", "-d", str(local)], text=True)
                interpreters = re.findall(r"Requesting program interpreter: ([^\]]+)", info)
                if guest in programs and interpreters != ["/system/bin/linker64"]:
                    raise SystemExit(f"Unexpected interpreter for {guest}: {interpreters}")
                for name in re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", info):
                    if name in apex_libraries:
                        continue
                    if name not in dependencies:
                        raise SystemExit(f"{guest} needs {name}, absent from its ELF search paths")
                    pending.append(dependencies[name])

        guest = "/init.environ.rc"
        subprocess.run(adb + ["pull", guest, str(root / guest.lstrip("/"))], check=True)
        copied.add(guest)

        fstab = "/vendor/etc/fstab.ranchu"
        local = root / fstab.lstrip("/")
        local.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(adb + ["pull", fstab, str(local)], check=True)
        copied.add(fstab)

        property_inputs = {
            "/system": "plat",
            "/system_ext": "system_ext",
            "/product": "product",
            "/vendor": "vendor",
            "/odm": "odm",
        }
        for partition, label in property_inputs.items():
            for suffix in (f"/etc/selinux/{label}_property_contexts", f"/etc/selinux/{label}_file_contexts",
                           f"/etc/selinux/{label}_service_contexts", f"/etc/selinux/{label}_hwservice_contexts",
                           f"/etc/selinux/{label}_seapp_contexts",
                           f"/etc/selinux/{label}_keystore2_key_contexts",
                           f"/etc/selinux/{label}_mac_permissions.xml", "/build.prop", "/etc/build.prop"):
                guest = partition + suffix
                if not shell("ls", "-d", guest, "2>/dev/null", "||", "true"):
                    if guest == "/system/etc/selinux/plat_mac_permissions.xml":
                        raise SystemExit(f"Missing required Android app-label policy: {guest}")
                    continue
                local = root / guest.lstrip("/")
                local.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(adb + ["pull", guest, str(local)], check=True)
                copied.add(guest)
        init_directories = ("/system/etc/init", "/system_ext/etc/init", "/product/etc/init",
                            "/vendor/etc/init", "/odm/etc/init")
        for directory in init_directories:
            for guest in shell(f"find {directory} -type f -name '*.rc' 2>/dev/null || true").splitlines():
                if not guest.startswith(directory + "/") or not guest.endswith(".rc"):
                    raise SystemExit(f"Unexpected path from init directory: {guest}")
                local = root / guest.lstrip("/")
                local.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(adb + ["pull", guest, str(local)], check=True)
                copied.add(guest)
        lockfile = "/system/etc/xtables.lock"
        local = root / lockfile.lstrip("/")
        local.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(adb + ["pull", lockfile, str(local)], check=True)
        copied.add(lockfile)
        for partition in ("/system", "/vendor", "/odm", "/product", "/system_ext"):
            guest = partition + "/etc/ueventd.rc"
            if not shell("ls", "-d", guest, "2>/dev/null", "||", "true"):
                continue
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True)
            copied.add(guest)
        for partition in property_inputs:
            for kind in ("vintf", "permissions", "sysconfig", "compatconfig"):
                directory = f"{partition}/etc/{kind}"
                for guest in shell(f"find {directory} -type f -name '*.xml' 2>/dev/null || true").splitlines():
                    if not guest.startswith(directory + "/") or not re.fullmatch(r"[A-Za-z0-9_./@+-]+\.xml", guest):
                        raise SystemExit(f"Unexpected reference XML path: {guest}")
                    local = root / guest.lstrip("/")
                    local.parent.mkdir(parents=True, exist_ok=True)
                    subprocess.run(adb + ["pull", guest, str(local)], check=True)
                    copied.add(guest)
        for partition in ("/system", "/system_ext", "/product"):
            directory = f"{partition}/etc/classpaths"
            for guest in shell(f"find {directory} -type f -name '*.pb' 2>/dev/null || true").splitlines():
                if not guest.startswith(directory + "/") or not re.fullmatch(r"[A-Za-z0-9_./+-]+\.pb", guest):
                    raise SystemExit(f"Unexpected classpath fragment: {guest}")
                local = root / guest.lstrip("/")
                local.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(adb + ["pull", guest, str(local)], check=True)
                copied.add(guest)
        # APEX archives carry their own BPF objects. The loader also reads these partition inputs.
        for partition in property_inputs:
            directory = f"{partition}/etc/bpf"
            pattern = rf"{directory}/(?:[A-Za-z0-9_.@+-]+/)*[A-Za-z0-9_.@+-]+\.(?:o|bpf)"
            command = rf"find {directory} -type f \( -name '*.o' -o -name '*.bpf' \) 2>/dev/null || true"
            for guest in shell(command).splitlines():
                if not re.fullmatch(pattern, guest) or Path(guest).as_posix() != guest or ".." in Path(guest).parts:
                    raise SystemExit(f"Unexpected BPF object path: {guest}")
                local = root / guest.lstrip("/")
                local.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(adb + ["pull", guest, str(local)], check=True)
                with local.open("rb") as file:
                    header = file.read(20)
                if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 247:
                    raise SystemExit(f"Expected a 64-bit little-endian BPF ELF: {guest}")
                copied.add(guest)
        # Use the reference system's generated classpaths to select platform JARs.
        if not {"/system/framework/framework.jar", "/system/framework/services.jar"} <= framework:
            raise SystemExit("Reference Android classpaths are incomplete")
        framework.update(("/system/framework/framework-res.apk", "/system/etc/preloaded-classes",
                          "/system/etc/dirty-image-objects", "/system/etc/boot-image.prof",
                          "/system/etc/cgroups.json", "/system/etc/task_profiles.json"))
        # Zygote caches these class loaders ahead of launching apps.
        for jar in ("/system/framework/android.hidl.base-V1.0-java.jar",
                    "/system/framework/android.hidl.manager-V1.0-java.jar",
                    "/system/framework/android.test.base.jar", "/system/framework/org.apache.http.legacy.jar",
                    "/system_ext/framework/androidx.window.extensions.jar",
                    "/system_ext/framework/androidx.window.sidecar.jar"):
            if shell("ls", "-d", jar, "2>/dev/null", "||", "true"):
                framework.add(jar)
        for name in shell("ls", "-1", "/system/framework/arm64").splitlines():
            if re.fullmatch(r"boot(?:-[A-Za-z0-9_.+-]+)?\.(?:art|oat|vdex)", name):
                framework.add(f"/system/framework/arm64/{name}")
        for guest in sorted(framework):
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True)
            copied.add(guest)

        # Java preload parses the system font configuration and reads Aconfig's partition tables.
        for partition in ("/system", "/system_ext", "/product", "/vendor"):
            for name in ("fonts.xml", "font_fallback.xml", "fonts_customization.xml",
                         "aconfig/package.map", "aconfig/flag.map", "aconfig/flag.val", "aconfig/flag.info"):
                guest = f"{partition}/etc/{name}"
                if not shell(f"test -f {guest} && echo yes || true"):
                    continue
                local = root / guest.lstrip("/")
                local.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(adb + ["pull", guest, str(local)], check=True)
                copied.add(guest)
            directory = f"{partition}/fonts"
            if shell(f"test -d {directory} && echo yes || true"):
                local = root / directory.lstrip("/")
                local.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(adb + ["pull", directory, str(local)], check=True)
                copied.update("/" + path.relative_to(root).as_posix() for path in local.rglob("*") if path.is_file())

        for guest in sorted(packages):
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True)
            copied.add(guest)

        # Retain the domain transitions returned by the SDK image's policy.
        init_context = shell("cat", "/proc/1/attr/current").rstrip("\0")
        process_class = int(shell("cat", "/sys/fs/selinux/class/process/index"))
        transitions = []
        boot_domains = []
        sdkext = "/apex/com.android.sdkext/bin"
        programs = ("/system/bin/apexd", system_self_test, vendor_self_test, vold, vdc, keystore, keymint,
                    casefold, vold_prepare, odsign, zygote, installd, system_suspend, lights, health, surfaceflinger,
                    bootanimation, composer, allocator, aconfigd, netd, iptables, cameraserver, camera_provider,
                    f"{sdkext}/derive_sdk", f"{sdkext}/derive_classpath",
                    "/apex/com.android.art/bin/art_boot", "/apex/com.android.art/bin/odrefresh",
                    "/apex/com.android.tethering/bin/netbpfload", uprobestats, bpfloader,
                    "/apex/com.android.conscrypt/bin/boringssl_self_test64",
                    "/apex/com.android.configinfrastructure/bin/aconfigd-mainline",
                    "/apex/com.android.os.statsd/bin/statsd", *managers)
        for path in programs:
            file_context = shell("ls", "-Zd", path).split()[0]
            source = {
                vold_prepare: "u:r:vold:s0",
                uprobestats: "u:r:bpfloader:s0",
                bpfloader: "u:r:bpfloader:s0",
                iptables: "u:r:netd:s0",
                "/apex/com.android.art/bin/odrefresh": "u:r:odsign:s0",
            }.get(path, init_context)
            query = f"{source} {file_context} {process_class}"
            if not re.fullmatch(r"[A-Za-z0-9_:]+ [A-Za-z0-9_:]+ [0-9]+", query):
                raise SystemExit("Unexpected Android SELinux labels")
            command = f"exec 3<>/sys/fs/selinux/create && printf '%s' '{query}' >&3 && cat <&3"
            result = subprocess.check_output(adb + ["shell", command]).decode().rstrip("\0\r\n")
            if not re.fullmatch(r"u:r:[a-z0-9_]+:s0", result):
                raise SystemExit(f"Unexpected domain transition for {path}: {result!r}")
            transitions.append(f"{path} {query} {result}\n")
            boot_domains.append(result)
        manifest = root / "nestvane-metadata" / "selinux-process-transition.txt"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("".join(transitions))
        seapp = root / "system/etc/selinux/plat_seapp_contexts"
        system_domains = re.findall(r"^isSystemServer=true\s+domain=([a-z0-9_]+)$", seapp.read_text(), re.MULTILINE)
        if len(system_domains) != 1:
            raise SystemExit("Expected one system server domain in the reference seapp contexts")
        startup_domain = f"u:r:{system_domains[0]}:s0"
        system_server_domain = "u:r:system_server:s0"
        setcurrent, dyntransition = (
            1 << (int(shell("cat", f"/sys/fs/selinux/class/process/perms/{name}")) - 1)
            for name in ("setcurrent", "dyntransition")
        )

        def permits(source, target, permission):
            query = f"{source} {target} {process_class} {permission:x}"
            command = f"exec 3<>/sys/fs/selinux/access && printf '%s' '{query}' >&3 && cat <&3"
            decision = shell(command).split()
            return len(decision) == 6 and bool(int(decision[0], 16) & permission)

        current = [("u:r:zygote:s0", startup_domain)]
        if startup_domain != system_server_domain:
            current.append((startup_domain, system_server_domain))
        for source, target in current:
            if not permits(source, source, setcurrent) or not permits(source, target, dyntransition):
                raise SystemExit(f"The reference policy denied the process domain transition {source} -> {target}")
        (manifest.parent / "selinux-current-transition.txt").write_text(
            "".join(f"{source} {target}\n" for source, target in current))
        (manifest.parent / "reference-classpath.txt").write_text(classpath + "\n")
        aliases = []
        for guest in ("/system/bin/linker64", "/system/lib64/libc.so", "/system/lib64/libm.so",
                      "/system/lib64/libdl.so"):
            target = shell("readlink", guest)
            if not re.fullmatch(r"/apex/com\.android\.runtime/(?:bin|lib64/bionic)/[a-z0-9.]+", target):
                raise SystemExit(f"Unexpected Runtime APEX alias: {guest} -> {target}")
            aliases.append(f"{guest} {target}\n")
        for guest in ("/system/bin/iptables-restore", "/system/bin/ip6tables-restore"):
            target = shell("readlink", guest)
            if target != "iptables":
                raise SystemExit(f"Unexpected iptables executable alias: {guest} -> {target}")
            aliases.append(f"{guest} {iptables}\n")
        (manifest.parent / "runtime-links.txt").write_text("".join(aliases))

        for partition in ("/system", "/vendor", "/product", "/system_ext", "/odm"):
            guest = partition + "/etc/linker.config.pb"
            if not shell("ls", "-d", guest, "2>/dev/null", "||", "true"):
                continue
            local = root / guest.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", guest, str(local)], check=True)
            copied.add(guest)
        for name in ("llndk.libraries.txt", "sanitizer.libraries.txt", "public.libraries.txt"):
            guest = "/system/etc/" + name
            if shell("ls", "-d", guest, "2>/dev/null", "||", "true"):
                subprocess.run(adb + ["pull", guest, str(root / guest.lstrip("/"))], check=True)
                copied.add(guest)
        subjects = tuple(dict.fromkeys((init_context, "u:r:vendor_init:s0", "u:r:ueventd:s0", *boot_domains,
                                       startup_domain, system_server_domain)))
        for name, permissions, label_suffix, output in (
            ("property_service", ("set",), "property_contexts", "property"),
            ("service_manager", ("add", "find", "list"), "_service_contexts", "service"),
            ("hwservice_manager", ("add", "find", "list"), "_hwservice_contexts", "hwservice"),
            ("process", ("setfscreate", "setsockcreate"), None, "fscreate"),
            ("keystore2", (
                "add_auth", "change_password", "change_user", "clear_ns", "clear_uid", "delete_all_keys",
                "early_boot_ended", "get_attestation_key", "get_auth_token", "get_last_auth_time",
                "get_state", "list", "lock", "pull_metrics", "report_off_body", "reset", "unlock",
            ), None, "keystore"),
            ("keystore2_key", (
                "convert_storage_key_to_ephemeral", "delete", "gen_unique_id", "get_info", "grant", "manage_blob",
                "rebind", "req_forced_op", "update", "use", "use_dev_id",
            ), "_keystore2_key_contexts", "keystore-key"),
        ):
            path = f"/sys/fs/selinux/class/{name}"
            index = int(shell("cat", f"{path}/index"))
            positions = [int(shell("cat", f"{path}/perms/{perm}")) for perm in permissions]
            if not 0 < index <= 65535 or any(not 1 <= bit <= 32 for bit in positions):
                raise SystemExit(f"Invalid SELinux class or permission index: {name}")
            requested = sum(1 << (bit - 1) for bit in set(positions))
            if label_suffix is None:
                contexts = {"u:r:keystore:s0"}
            else:
                contexts = ({"u:r:keystore:s0"} if name == "keystore2_key" else
                            set(subjects) if "list" in permissions else {"u:object_r:default_prop:s0"})
                for context_file in root.rglob(f"*{label_suffix}"):
                    for line in context_file.read_text().splitlines():
                        words = line.split()
                        if len(words) >= 2 and not line.lstrip().startswith("#"):
                            if re.fullmatch(r"u:object_r:[a-z0-9_]+:s0", words[1]):
                                contexts.add(words[1])
            sources = (*subjects, "u:r:shell:s0")
            if name == "process":
                queries = sorted((source, source) for source in sources)
            else:
                queries = sorted((source, target) for source in sources for target in contexts)
            decisions = query_access(adb, index, requested, queries)
            rows = [f"{source} {target} {decision}" for (source, target), decision in decisions.items()]
            header = " ".join(map(str, (index, *positions)))
            (manifest.parent / f"selinux-{output}-access.txt").write_text(
                header + "\n" + "\n".join(rows) + "\n")
        if small_art_heap:
            properties = root / "system/build.prop"
            values = properties.read_text()
            for key in ("dalvik.vm.image-dex2oat", "dalvik.vm.dex2oat"):
                for suffix, size in (("-Xms", 16), ("-Xmx", 96)):
                    values, count = re.subn(
                        rf"^{re.escape(key + suffix)}=[^\r\n]*$",
                        f"{key}{suffix}={size}m", values, flags=re.MULTILINE
                    )
                    if count != 1:
                        raise SystemExit(f"Expected exactly one reference setting for {key}{suffix}")
            properties.write_text(values)
            print("Experimental dex2oat heap profile: 16 MiB initial, 96 MiB maximum")
        root.rename(destination)
    print(f"Collected {len(copied)} Android files in {destination}", flush=True)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("serial")
parser.add_argument("--readelf", default="llvm-readelf")
parser.add_argument("--small-art-heap", action="store_true",
                    help="Use a 16–96 MiB ART compiler heap for constrained boot diagnostics")
parser.add_argument("--output", type=Path, default=Path("generated/android17-init-root"))
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
if args.output.exists():
    parser.error(f"Choose a fresh output path: {args.output}")
collect(args.serial, args.readelf, args.output, args.small_art_heap)
