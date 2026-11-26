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
    # Settings' Safety and emergency page asks its gesture provider and stops without it.
    "com.android.emergency",
    "com.android.externalstorage",
    "com.android.inputdevices",
    "com.android.inputmethod.latin",
    "com.android.keychain",
    "com.android.launcher3",
    "com.android.location.fused",
    # Text messages: the app that shows them. Nothing sends or receives one, there is no phone
    # process and no radio; the app writes a message the person composed into the provider below.
    "com.android.messaging",
    "com.android.modulemetadata",
    # Brings the Ethernet interface up and checks the network: without it nothing is ever connected.
    "com.android.networkstack",
    "com.android.packageinstaller",
    # Providers apps ask for and stop without: contacts, calendar, blocked numbers, and the user
    # dictionary the keyboard and Settings' language page read.
    "com.android.providers.blockednumber",
    "com.android.providers.calendar",
    "com.android.providers.contacts",
    "com.android.providers.downloads",
    "com.android.providers.downloads.ui",
    "com.android.providers.settings",
    "com.android.providers.telephony",
    "com.android.providers.userdictionary",
    "com.android.server.telecom",
    "com.android.settings",
    "com.android.shell",
    "com.android.systemui",
)

FRAMEWORK_APP_APEXES = ("com.android.extservices", "com.android.mediaprovider", "com.android.permission")
LATIN_IME_LIBRARY = "/product/lib64/libjni_latinime.so"
EMERGENCY_PERMISSIONS = "/system_ext/etc/permissions/com.android.emergency.xml"
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
    # Emergency info's privileged permissions: without the allowlist the package manager refuses it.
    allowlist = guest / EMERGENCY_PERMISSIONS.lstrip("/")
    allowlist.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(adb + ["pull", EMERGENCY_PERMISSIONS, str(allowlist)], check=True)
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


# Class loader contexts from the corresponding factory artifacts.
LOCATION_PROVIDER = "/system/framework/com.android.location.provider.jar"
WINDOW_EXTENSIONS = "/system_ext/framework/androidx.window.extensions.jar"
WINDOW_SIDECAR = "/system_ext/framework/androidx.window.sidecar.jar"
WINDOW_CONTEXT = f"PCL[]{{PCL[{WINDOW_EXTENSIONS}]#PCL[{WINDOW_SIDECAR}]}}"
FRAMEWORK_LAUNCH_APPS = (
    "/system_ext/priv-app/SystemUI/SystemUI.apk",
    "/system_ext/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
)


def precompile_framework_code(adb, remote, guest, stage, bootclasspath):
    targets = (("/system/framework/services.jar", f"PCL[{LOCATION_PROVIDER}]"),) + tuple(
        (source, WINDOW_CONTEXT) for source in FRAMEWORK_LAUNCH_APPS)
    check_reference_files(remote, guest, {source for source, _ in targets} |
                          {LOCATION_PROVIDER, WINDOW_EXTENSIONS, WINDOW_SIDECAR})
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


def complete_boot_image(adb, remote, root, output, classpaths, active_modules, precompile_apps):
    primary = classpaths["DEX2OATBOOTCLASSPATH"]
    full = classpaths["BOOTCLASSPATH"]
    server = classpaths["SYSTEMSERVERCLASSPATH"]
    standalone = classpaths["STANDALONE_SYSTEMSERVER_JARS"]
    if full[:len(primary)] != primary or len(full) == len(primary):
        raise SystemExit("Unexpected primary and Mainline boot classpaths")
    if len(set(server + standalone)) != len(server + standalone):
        raise SystemExit("Duplicate system-server classpath entries")

    reference_files = set()
    if precompile_apps:
        reference_files.update((*FRAMEWORK_LAUNCH_APPS, WINDOW_EXTENSIONS, WINDOW_SIDECAR))
    for jar in full + server + standalone:
        if jar.startswith("/apex/"):
            module = jar.split("/")[2]
            apex = active_modules.get(module, "")
            if not re.fullmatch(r"/system/apex/[A-Za-z0-9_.-]+\.apex", apex):
                raise SystemExit(f"Expected an active factory APEX for {module}")
            reference_files.add(apex)
        else:
            reference_files.add(jar)
    check_reference_files(remote, root, reference_files)

    stage = remote("mktemp", "-d", "/data/local/tmp/nestvane-preopt.XXXXXX")
    if not re.fullmatch(r"/data/local/tmp/nestvane-preopt\.[A-Za-z0-9]+", stage):
        raise SystemExit("Unexpected reference work directory")

    def pull(source, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(adb + ["pull", source, str(destination)], check=True)
        if destination.stat().st_size == 0:
            raise SystemExit(f"Reference produced an empty {destination.name}")

    def compile_code(options, jars):
        command = ["/apex/com.android.art/bin/dex2oat64",
                   "--runtime-arg", "-Xbootclasspath:" + ":".join(full),
                   "--runtime-arg", "-Xgc:CC", "--runtime-arg", "-Xmx384m", "-j2",
                   "--instruction-set=arm64", "--instruction-set-features=default",
                   "--avoid-storing-invocation", *options]
        for jar in jars:
            command.extend(("--dex-file=" + jar, "--dex-location=" + jar))
        remote(*command)

    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        remote("mkdir", stage + "/arm64")
        for suffix in ("art", "oat", "vdex"):
            name = f"boot.{suffix}"
            source = root / "system/framework/arm64" / name
            subprocess.run(adb + ["push", str(source), f"{stage}/arm64/{name}"], check=True)

        with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
            guest = Path(temporary) / "guest"
            shutil.copytree(root, guest)
            extension_jars = full[len(primary):]
            extension_name = "boot-" + Path(extension_jars[0]).stem
            remote("mkdir", stage + "/extension")
            stem = stage + "/extension/boot"
            extension = stage + "/arm64/" + extension_name
            compile_code(["--compiler-filter=verify", "--single-image", "--image-format=uncompressed",
                          "--boot-image=" + stage + "/boot.art", "--image=" + stem + ".art",
                          "--oat-file=" + stem + ".oat",
                          f"--oat-location=/system/framework/arm64/{extension_name}.oat",
                          "--output-vdex=" + stem + ".vdex"], extension_jars)
            # For a boot extension dex2oat adds the first JAR's name to ART/OAT, but not VDEX.
            for suffix in ("art", "oat", "vdex"):
                file = (stem if suffix == "vdex" else f"{stage}/extension/{extension_name}") + "." + suffix
                destination = guest / "system/framework/arm64" / f"{extension_name}.{suffix}"
                pull(file, destination)
                remote("mv", file, extension + "." + suffix)

            boot_image = f"--boot-image={stage}/boot.art:{stage}/{extension_name}.art"

            def compile_artifact(source, context, location, stem):
                compile_code(["--compiler-filter=speed", boot_image,
                              "--class-loader-context=" + context, "--oat-file=" + stem + ".odex",
                              "--oat-location=" + str(location) + ".odex",
                              "--output-vdex=" + stem + ".vdex"], [source])
                for suffix in ("odex", "vdex"):
                    pull(f"{stem}.{suffix}", guest / (str(location)[1:] + "." + suffix))

            for index, jar in enumerate(server + standalone):
                source = Path(jar)
                if jar.startswith("/apex/"):
                    encoded = jar.lstrip("/").replace("/", "@") + "@classes"
                    location = Path("/system/framework/oat/arm64") / encoded
                else:
                    location = source.parent / "oat/arm64" / source.stem
                parents = ":".join(server[:index])
                context = f"PCL[{parents}]"
                if index >= len(server):
                    context = "PCL[];" + context
                compile_artifact(jar, context, location, f"{stage}/{index:02d}")
            if precompile_apps:
                for apk in FRAMEWORK_LAUNCH_APPS:
                    source = Path(apk)
                    location = source.parent / "oat/arm64" / source.stem
                    compile_artifact(apk, WINDOW_CONTEXT, location, f"{stage}/{source.stem}")
            guest.rename(output)
    finally:
        remote("rm", "-rf", stage)

def replace_line(path, pattern, value):
    updated, count = re.subn(pattern, value, path.read_text(), flags=re.MULTILINE)
    if count != 1:
        raise SystemExit(f"Expected one match for {pattern!r} in {path}")
    path.write_text(updated)


def set_property(path, key, value):
    """Sets a property in a build.prop file to one value, whatever lines it had for it."""
    lines = [line for line in path.read_text().splitlines() if not line.startswith(key + "=")]
    path.write_text("\n".join(lines + [f"{key}={value}"]) + "\n")


def replace_build_names(guest, build_id, release):
    """Every partition's properties may name the build: the one read last is what counts."""
    values = {"ro.build.display.id": build_id, "ro.build.description": f"nestvane {release} {build_id}".replace("  ", " "),
              "ro.build.user": "nestvane", "ro.build.host": "nestvane"}
    for path in [guest / "system/build.prop", *sorted(guest.glob("*/etc/build.prop")), *sorted(guest.glob("*/build.prop"))]:
        if not path.is_file():
            continue
        text = path.read_text()
        for key, value in values.items():
            if value and (path == guest / "system/build.prop" or f"\n{key}=" in "\n" + text):
                set_property(path, key, value)


def add_low_memory_killer(adb, guest):
    """lmkd, which the init root has a service entry for but not the program.

    Without it ActivityManager waits three seconds for its socket whenever it adjusts a process's
    priority, with the lock held that every activity start needs.
    """
    program = guest / "system/bin/lmkd"
    if not program.is_file():
        subprocess.run(adb + ["pull", "/system/bin/lmkd", str(program)], check=True, stdout=subprocess.DEVNULL)
        program.chmod(0o755)
    transitions = guest / "nestvane-metadata/selinux-process-transition.txt"
    if "/system/bin/lmkd " not in transitions.read_text():
        with transitions.open("a") as file:
            file.write("/system/bin/lmkd u:r:init:s0 u:object_r:lmkd_exec:s0 2 u:r:lmkd:s0\n")


def add_content_tool(adb, guest):
    """Android's content tool, which fixtures/android17-text.sh uses to store a text message."""
    for path in ("system/framework/content.jar", "system/framework/oat/arm64/content.odex",
                 "system/framework/oat/arm64/content.vdex"):
        if not (guest / path).is_file():
            (guest / path).parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", "/" + path, str(guest / path)], check=True, stdout=subprocess.DEVNULL)


def add_declared_libraries(adb, guest):
    """The shared libraries that /system/etc/permissions declares (the USB accessory library, the test
    libraries and others). Without the file, Android refuses every app that lists one as required
    (Bilibili, Zhihu); they were not taken into the image before 1.13."""
    for declaration in (guest / "system/etc/permissions").glob("*.xml"):
        for path in re.findall(r'file="/(system/framework/[A-Za-z0-9_.+-]+\.jar)"', declaration.read_text(encoding="utf-8")):
            if not (guest / path).is_file():
                subprocess.run(adb + ["pull", "/" + path, str(guest / path)], check=True, stdout=subprocess.DEVNULL)


def add_overlay_daemon(adb, guest):
    """idmap2d, which the overlay manager starts to put resource overlays into effect, and idmap2,
    which zygote runs before anything else for the overlays of the framework's own resources.

    Without idmap2d the overlays that tools/build-overlays.sh installs are scanned and never applied.
    Without idmap2 the ones that target "android" are left out of every process: zygote logs
    "'idmap2 create-multiple' failed" and the framework keeps its own values.
    """
    for path in ("system/bin/idmap2d", "system/bin/idmap2", "system/lib64/libidmap2.so"):
        if not (guest / path).is_file():
            subprocess.run(adb + ["pull", "/" + path, str(guest / path)], check=True, stdout=subprocess.DEVNULL)
    (guest / "system/bin/idmap2d").chmod(0o755)
    (guest / "system/bin/idmap2").chmod(0o755)
    transitions = guest / "nestvane-metadata/selinux-process-transition.txt"
    if "/system/bin/idmap2 " not in transitions.read_text():
        with transitions.open("a") as file:
            file.write("/system/bin/idmap2 u:r:zygote:s0 u:object_r:idmap_exec:s0 2 u:r:idmap:s0\n")
    if "/system/bin/idmap2d " not in transitions.read_text():
        with transitions.open("a") as file:
            file.write("/system/bin/idmap2d u:r:init:s0 u:object_r:idmap_exec:s0 2 u:r:idmap:s0\n")
    # The policy rows were captured for the domains of the processes the root had then: the daemon's
    # own domain may add and find its service.
    services = guest / "nestvane-metadata/selinux-service-access.txt"
    if "u:r:idmap:s0 u:object_r:idmap_service:s0 " not in services.read_text():
        with services.open("a") as file:
            file.write("u:r:idmap:s0 u:object_r:idmap_service:s0 3 ffffffff 0 ffffffff 1 0\n")


MEDIA_SERVER_FILES = ("system/bin/mediaserver", "system/lib64/libmediaplayerservice.so",
                      "system/lib64/libresourcemanagerservice.so", "system/lib64/libstats_media_metrics.so",
                      "system/lib64/libandroid_net.so", "system/lib64/libstagefright_httplive.so")
# What the media server looks up besides what the audio server does.
MEDIA_SERVER_FINDS = ("audioserver_service", "mediaextractor_service", "mediadrmserver_service", "drmserver_service",
                      "media_session_service", "cameraserver_service", "surfaceflinger_service", "gpu_service",
                      "processinfo_service", "hal_codec2_service", "hal_graphics_allocator_service",
                      "media_native_packages_service", "network_management_service", "netd_service", "dnsresolver_service")


def allow_domain_like(guest, domain, template, exec_type, programs, services=None):
    """The policy rows for a domain the captured policy has none of, made from another domain's.

    The rows were captured for the domains of the processes the root had then. A daemon added later
    gets the rows of a daemon with much the same rights (`template`); `services` gives the rights on
    service types that differ, and "*" there stands for every type. They are not read from the
    reference's policy.
    """
    services = services or {}
    metadata = guest / "nestvane-metadata"
    transitions = metadata / "selinux-process-transition.txt"
    for program in programs:
        if f"{program} " not in transitions.read_text():
            with transitions.open("a") as file:
                file.write(f"{program} u:r:init:s0 u:object_r:{exec_type}:s0 2 u:r:{domain}:s0\n")
    ours, theirs = f"u:r:{domain}:s0", f"u:r:{template}:s0"
    for name in ("selinux-service-access.txt", "selinux-hwservice-access.txt", "selinux-property-access.txt",
                 "selinux-fscreate-access.txt", "selinux-keystore-access.txt", "selinux-keystore-key-access.txt"):
        path = metadata / name
        lines = path.read_text().splitlines()
        if any(line.startswith(ours + " ") for line in lines):
            continue
        added = []
        for line in lines:
            fields = line.split(" ")
            if fields[0] == theirs:
                fields[0] = ours
                if fields[1] == theirs:
                    fields[1] = ours
                kind = fields[1].removeprefix("u:object_r:").removesuffix(":s0")
                if name in ("selinux-service-access.txt", "selinux-hwservice-access.txt") and fields[1].startswith("u:object_r:"):
                    fields[2] = services.get(kind, services.get("*", fields[2]))
                added.append(" ".join(fields))
            elif fields[1] == theirs:
                fields[1] = ours
                added.append(" ".join(fields))
        path.write_text("\n".join(lines + added) + "\n")


def allow_media_server(guest):
    """The media server's domain: the audio server's rows, and every service to add or find. Naming the
    ones it needs one by one left out the graphics mapper, without which it stops when a video starts."""
    # Two services it would ask for are not in the root: refused, it goes on without them at once; allowed, it waits for them.
    allow_domain_like(guest, "mediaserver", "audioserver", "mediaserver_exec", ["/system/bin/mediaserver"],
                      {"*": "3", "mediacodeclist_generator_service": "0", "hal_omx_hwservice": "0"})


MEDIA_HELPER_FILES = ("system/bin/mediaextractor", "system/bin/mediametrics", "system/lib64/libmediaextractorservice.so",
                      "system/lib64/libmediametricsservice.so", "system/lib64/android.media.codec-aconfig-cc.so",
                      "system/lib64/libavservices_minijail.so", "system/lib64/libminijail.so")


def add_media_helpers(guest, full):
    """The two services the media server asks for when something is played: the extractor, which
    reads the file formats in a sandbox of its own, and the metrics service. Without the extractor the
    player waits for it and gives up."""
    for path in MEDIA_HELPER_FILES:
        shutil.copyfile(full / path, guest / path)
    for program in ("mediaextractor", "mediametrics"):
        (guest / "system/bin" / program).chmod(0o755)
    policies = guest / "system/etc/seccomp_policy"
    policies.mkdir(parents=True, exist_ok=True)
    for policy in (full / "system/etc/seccomp_policy").iterdir():
        shutil.copyfile(policy, policies / policy.name)
    allow_domain_like(guest, "mediaextractor", "audioserver", "mediaextractor_exec", ["/system/bin/mediaextractor"], {"*": "3"})
    allow_domain_like(guest, "mediametrics", "audioserver", "mediametrics_exec", ["/system/bin/mediametrics"], {"*": "3"})


SOFTWARE_CODECS = "system/nestvane-swcodec"
SOFTWARE_CODECS_RC = """# The software codecs of the com.android.media.swcodec module. The root has the module's files
# unpacked (tools/prepare-boot-image.py) and not the module, so they are put where the module would be
# mounted, after the modules are, and the loader gets the configuration the module carries.
on post-fs-data
    mkdir /apex/com.android.media.swcodec 0755 root root
    mount none /system/nestvane-swcodec /apex/com.android.media.swcodec bind rec
    mkdir /linkerconfig/com.android.media.swcodec 0755 root root
    copy /system/nestvane-swcodec/etc/ld.config.txt /linkerconfig/com.android.media.swcodec/ld.config.txt
    chmod 644 /linkerconfig/com.android.media.swcodec/ld.config.txt

service media.swcodec /apex/com.android.media.swcodec/bin/mediaswcodec
    class main
    user mediacodec
    group camera drmrpc mediadrm
"""


def add_software_codecs(guest, module, declaration):
    """Audio and video codecs: the media.swcodec service and its libraries, from the unpacked module.

    All of them: the service stops with an error when one of the codecs it knows of is not there.
    """
    target = guest / SOFTWARE_CODECS
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(module, target, ignore=lambda directory, names: [
        name for name in names if re.fullmatch(r".*\.32rc|mediaswcodec\.rc", name)])
    (target / "bin/mediaswcodec").chmod(0o755)
    # The module's own loader configuration is from before the platform's libraries were spread over
    # modules: the platform namespace also finds what the platform's libraries need from those.
    configuration = target / "etc/ld.config.txt"
    extra = "".join(f"namespace.platform.search.paths += /apex/{module}/${{LIB}}\nnamespace.platform.permitted.paths += /apex/{module}/${{LIB}}\n"
                    for module in ("com.android.i18n", "com.android.art", "com.android.os.statsd", "com.android.media",
                                   "com.android.tethering", "com.android.conscrypt", "com.android.configinfrastructure"))
    # The graphics buffer mapper is a vendor library in the hw directory, by its file name.
    extra += "namespace.sphal.search.paths += /vendor/${LIB}/hw\n"
    configuration.write_text(configuration.read_text() + "\n" + extra)
    (guest / "system/etc/init/nestvane-swcodec.rc").write_text(SOFTWARE_CODECS_RC)
    shutil.copyfile(declaration, guest / "system/etc/vintf/manifest/manifest_media_c2_software.xml")
    allow_domain_like(guest, "mediaswcodec", "audioserver", "mediaswcodec_exec",
                      ["/apex/com.android.media.swcodec/bin/mediaswcodec", f"/{SOFTWARE_CODECS}/bin/mediaswcodec"], {"*": "3"})


def add_media_server(adb, guest):
    """mediaserver, which has the list of codecs behind MediaCodecList.

    Without it whoever asks for the list waits for the service for ever: the storage manager does at
    boot, to see whether video can be transcoded, and so never mounted the shared storage.
    """
    for path in MEDIA_SERVER_FILES:
        if not (guest / path).is_file():
            subprocess.run(adb + ["pull", "/" + path, str(guest / path)], check=True, stdout=subprocess.DEVNULL)
    (guest / "system/bin/mediaserver").chmod(0o755)
    allow_media_server(guest)


def precompile_apex_apps(adb, remote, guest):
    """Compiles the apps that live inside APEX modules, which the build leaves without compiled code.

    Without it they run in ART's interpreter: the media provider alone took 2.2 s of a 14 s boot. The
    reference's own package manager compiles each package, so that the class loader context in the
    result is the one the runtime will compare it with. ART looks for the code of anything under /apex
    in /system/framework/oat, named after the file's path, which is also the name in the dalvik cache.
    """
    present = {path.stem for path in (guest / "system/apex").glob("*.apex")}
    count = 0
    for line in remote("pm", "list", "packages", "-f").splitlines():
        match = re.fullmatch(r"package:(/apex/([A-Za-z0-9_.-]+)/[A-Za-z0-9_.@/-]+\.apk)=([A-Za-z0-9_.]+)", line.strip())
        if not match or match[2] not in present:
            continue
        apk, package = match[1], match[3]
        name = apk.lstrip("/").replace("/", "@") + "@classes"
        cached = f"/data/dalvik-cache/arm64/{name}"
        subprocess.run(adb + ["shell", shlex.join(["pm", "compile", "-m", "speed", "-f", package])],
                       check=True, stdout=subprocess.DEVNULL)
        if remote("sh", "-c", f"test -s {shlex.quote(cached + '.dex')} && echo yes || echo no") != "yes":
            continue  # a package without code
        for cache_suffix, suffix in (("dex", "odex"), ("vdex", "vdex")):
            local = guest / "system/framework/oat/arm64" / f"{name}.{suffix}"
            local.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(adb + ["pull", f"{cached}.{cache_suffix}", str(local)], check=True, stdout=subprocess.DEVNULL)
        count += 1
    print(f"Compiled {count} apps of APEX modules", flush=True)


def copy_prebuilt_code(adb, remote, guest):
    """Takes the image's own compiled code for every JAR and APK that is in the guest root."""
    listing = remote("find", "/system", "/system_ext", "/product", "/vendor", "-path", "*/oat/arm64/*", "-type", "f")
    count = 0
    for path in sorted(listing.splitlines()):
        match = re.fullmatch(r"(/[A-Za-z0-9_.+/-]+)/oat/arm64/([A-Za-z0-9_.+@-]+)\.(odex|vdex|art)", path)
        if not match:
            continue
        owner = guest / match[1].lstrip("/")
        # Code of an APEX JAR is under /system/framework, named after the JAR's path.
        if not any((owner / (match[2] + suffix)).is_file() for suffix in (".apk", ".jar")) and match[1] != "/system/framework":
            continue
        local = guest / path.lstrip("/")
        local.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(adb + ["pull", path, str(local)], check=True, stdout=subprocess.DEVNULL)
        count += 1
    print(f"Added {count} files of compiled code from the image", flush=True)


# Hardware services that the image declares and the guest root does not carry. A declared service that
# never registers is waited for, five seconds at a time, with locks held that the launcher needs.
# Not Wi-Fi either: with the feature the system starts its Wi-Fi service, which answers apps that ask
# for the Wi-Fi state (it says off), but it cost the start 0.3 s on an iPhone 14 (1.11). Apps are told
# the network is Wi-Fi all the same (fixtures/overlay-connectivity).
ABSENT_HARDWARE = ("android.hardware.wifi-service.xml", "android.hardware.wifi.hostapd.xml",
                   "android.hardware.wifi.supplicant.xml", "android.hardware.biometrics.fingerprint-service.ranchu.xml",
                   "android.hardware.media.c2-service-goldfish.xml")
ABSENT_FEATURES = ("android.hardware.wifi", "android.hardware.wifi.direct", "android.hardware.wifi.passpoint",
                   "android.hardware.wifi.aware", "android.hardware.wifi.rtt", "android.hardware.uwb",
                   "android.hardware.fingerprint")


def remove_absent_hardware(guest):
    for name in ABSENT_HARDWARE:
        (guest / "vendor/etc/vintf/manifest" / name).unlink(missing_ok=True)
    # The software codecs' service is declared only together with its files (add_software_codecs):
    # whoever lists the codecs waits for ever for a service that is declared and never comes.
    if not (guest / SOFTWARE_CODECS).is_dir():
        (guest / "system/etc/vintf/manifest/manifest_media_c2_software.xml").unlink(missing_ok=True)
    config = guest / "system/etc/sysconfig/nestvane-hardware.xml"
    text = config.read_text()
    extra = "".join(f'    <unavailable-feature name="{name}" />\n' for name in ABSENT_FEATURES if f'"{name}"' not in text)
    config.write_text(text.replace("</config>", extra + "</config>"))


# Modules whose apps never run here: for hardware the guest does not have, or disabled by
# fixtures/android17-trim.sh. The compiled code of these apps is taken out; their dex files stay.
UNUSED_COMPILED = ("com.android.bt@", "com.android.wifi@", "com.android.uwb@", "com.android.nfcservices@",
                   "com.android.adservices@", "com.android.ondevicepersonalization@", "com.android.healthfitness@",
                   "com.android.rkpd@", "com.android.virt@", "com.android.npumanager@",
                   "com.android.webapp@", "com.android.uprobestats@", "com.android.profiling@")


def declare_network(guest):
    """The guest's network is an Ethernet interface (runtime/HostSocket.hpp): the feature starts the
    Ethernet service, and fixtures/overlay-connectivity gives the interface its fixed configuration."""
    feature = guest / "system/etc/permissions/nestvane-ethernet.xml"
    feature.write_text('<?xml version="1.0" encoding="utf-8"?>\n<permissions>\n'
                       '    <feature name="android.hardware.ethernet" />\n</permissions>\n')


def remove_unused_files(guest):
    """Takes out what only adds to the size of the image."""
    # A second, serif family for Chinese, Japanese and Korean: the sans family stays.
    (guest / "system/fonts/NotoSerifCJK-Regular.ttc").unlink(missing_ok=True)
    compiled = guest / "system/framework/oat/arm64"
    if compiled.is_dir():
        for file in compiled.iterdir():
            # Only apps: the system server loads the modules' service jars whether their hardware exists or not.
            if file.name.startswith("apex@") and any(module in file.name for module in UNUSED_COMPILED) \
                    and ("@priv-app@" in file.name or "@app@" in file.name) and file.suffix in (".odex", ".art"):
                file.unlink()


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
    if image is not None:
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
    system_properties = guest / "system/build.prop"
    if image is not None:
        replace_line(guest / "product/etc/build.prop", r"^ro\.dalvik\.vm\.enable_uffd_gc=[^\r\n]*$",
                     "ro.dalvik.vm.enable_uffd_gc=false")
    else:
        # The image's code is compiled for the mark-compact collector. The guest kernel has no
        # userfaultfd, so the collector compacts with the world stopped.
        set_property(system_properties, "ro.dalvik.vm.force_cmc_stw_compaction", "true")
        # The boot image's code is translated for the address it was compiled for.
        set_property(system_properties, "dalvik.vm.extra-opts", "-Xnorelocate")
        # A phone gives one app far less memory than this image expects of a device: Android's own
        # low-memory profile keeps fewer processes and caches.
        set_property(system_properties, "ro.config.low_ram", "true")
        # Low-RAM devices get a memory cgroup for each app, which lmkd sets limits on; the guest kernel
        # has none, and lmkd logged a failure for every app it was told of.
        set_property(system_properties, "ro.config.per_app_memcg", "false")
        # Code that the guest compiles for itself has no translation, and a process that maps it is
        # interpreted from then on. The package manager only verifies; compiled code comes with the image.
        for reason in ("first-boot", "boot-after-ota", "boot-after-mainline-update", "post-boot", "install",
                       "install-fast", "install-bulk", "install-bulk-secondary", "install-bulk-downgraded",
                       "install-bulk-secondary-downgraded", "bg-dexopt", "ab-ota", "inactive", "cmdline", "shared"):
            set_property(system_properties, "pm.dexopt." + reason, "verify")
        remove_absent_hardware(guest)
        remove_unused_files(guest)
        declare_network(guest)
    set_property(system_properties, "dalvik.vm.usejit", "false")
    # The guest is one app's window on a phone that has a lock screen of its own.
    set_property(system_properties, "ro.lockscreen.disable.default", "true")
    # The guest's pages are the iPhone's, 16 KiB. Most apps' own libraries are still laid out for
    # 4 KiB pages, and the linker refuses them unless it is told to load them the compatible way.
    set_property(system_properties, "bionic.linker.16kb.app_compat.enabled", "true")
    # Nothing can reach the guest's adbd, and left on it reconnects to the framework every five seconds.
    set_property(system_properties, "persist.sys.usb.config", "none")
    # SurfaceFlinger compiles a hundred shaders ahead of use while the boot waits for it: 3.9 s on
    # an iPhone 15 Pro. Each is compiled when it is first drawn with instead.
    set_property(system_properties, "service.sf.prime_shader_cache", "0")
    # What Settings > About phone shows as the device: the app, not the emulator image it was built from.
    for key in ("model", "brand", "manufacturer"):
        set_property(system_properties, f"ro.product.{key}", "Nestvane")
    # The build number that page shows is the build's id alone, as on a released device, instead of the
    # development build's description with the name of whoever built it. The fingerprints keep theirs:
    # the package manager takes a changed fingerprint for a system update.
    properties = dict(line.split("=", 1) for line in system_properties.read_text().splitlines() if "=" in line)
    build_id = properties.get("ro.build.id", "")
    replace_build_names(guest, build_id, properties.get("ro.build.version.release", ""))
    set_property(system_properties, "ro.hw_timeout_multiplier", "10")
    # Android plays media at full level: how loud it is belongs to the phone's own volume buttons, which the
    # guest never sees. Android's default is a third of the way up.
    set_property(system_properties, "ro.config.media_vol_steps", "15")
    set_property(system_properties, "ro.config.media_vol_default", "15")
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
    # How large one app's Java heap may grow. Every process sets the whole of it aside as address
    # space, which is what there is least of here, so it was 96 MB; a large app such as Douyin runs
    # out of that within a minute of starting. 256 MB is what phones give.
    for key in ("heapgrowthlimit", "heapsize"):
        replace_line(guest / "vendor/build.prop", rf"^dalvik\.vm\.{key}=[^\r\n]*$", f"dalvik.vm.{key}=256m")
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
    parser.add_argument("--complete-existing-image", action="store_true",
                        help="Add Mainline ART and system-server artifacts to an assembled guest root")
    parser.add_argument("--framework-diagnostic", action="store_true",
                        help="Produce a complete offline diagnostic guest root using the compiled image")
    parser.add_argument("--framework-apps", action="store_true",
                        help="Include launcher, SystemUI and package-management apps from the matching reference")
    parser.add_argument("--precompile-framework-code", action="store_true",
                        help="Add offline speed-compiled services.jar, SystemUI and Launcher code")
    parser.add_argument("--prebuilt-code", action="store_true",
                        help="The image carries compiled code for its own collector (a build patched by "
                             "experiments/aosp/patch.py): keep its boot image and take its compiled code")
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
    if args.complete_existing_image and (args.framework_diagnostic or args.compiler_filter != "verify"):
        parser.error("--complete-existing-image reuses the staged primary image and cannot reassemble the guest")
    if args.defer_zygote_preload and not args.framework_diagnostic:
        parser.error("--defer-zygote-preload requires --framework-diagnostic")
    if args.prebuilt_code and (not args.framework_diagnostic or args.precompile_framework_code):
        parser.error("--prebuilt-code requires --framework-diagnostic and replaces --precompile-framework-code")
    if args.framework_apps and not args.framework_diagnostic:
        parser.error("--framework-apps requires --framework-diagnostic")
    if args.precompile_framework_code and not (args.framework_apps or args.complete_existing_image):
        parser.error("--precompile-framework-code requires --framework-apps or --complete-existing-image")
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
    if (args.framework_diagnostic or args.complete_existing_image) and args.root.resolve() in args.output.resolve().parents:
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
    if args.complete_existing_image:
        classpaths = {"DEX2OATBOOTCLASSPATH": jars}
        for name in ("BOOTCLASSPATH", "SYSTEMSERVERCLASSPATH", "STANDALONE_SYSTEMSERVER_JARS"):
            match = re.search(rf"^export {name} (\S+)$", path_input, re.MULTILINE)
            if not match:
                raise SystemExit(f"The reference did not export {name}")
            paths = match[1].split(":")
            if any(not re.fullmatch(r"/(?:apex|system|system_ext)/[A-Za-z0-9_.+/-]+\.jar", path) or
                   ".." in Path(path).parts for path in paths):
                raise SystemExit(f"Unexpected JAR path in {name}")
            classpaths[name] = paths
        complete_boot_image(adb, remote, args.root, args.output, classpaths, active_modules,
                            args.precompile_framework_code)
        return

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
        if not args.prebuilt_code:
            subprocess.run(adb + ["shell", shlex.join(command)], check=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
            root = Path(temporary) / "arm64"
            root.mkdir()
            for suffix in () if args.prebuilt_code else ("art", "oat", "vdex"):
                name = f"boot.{suffix}"
                subprocess.run(adb + ["pull", f"{stage}/{name}", str(root / name)], check=True)
                if (root / name).stat().st_size == 0:
                    raise SystemExit(f"Reference produced an empty {name}")
            if args.framework_diagnostic:
                guest = Path(temporary) / "guest"
                make_framework_root(args.root, None if args.prebuilt_code else root, guest, args.defer_zygote_preload,
                                    args.hardware_timeout_multiplier, tethering_loader)
                if args.framework_apps:
                    copy_framework_apps(adb, remote, active_modules, guest)
                    capture_framework_app_policy(adb, remote, guest)
                if args.prebuilt_code:
                    copy_prebuilt_code(adb, remote, guest)
                    add_low_memory_killer(adb, guest)
                    add_overlay_daemon(adb, guest)
                    add_content_tool(adb, guest)
                    add_declared_libraries(adb, guest)
                    add_media_server(adb, guest)
                    precompile_apex_apps(adb, remote, guest)
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
