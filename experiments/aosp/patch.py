#!/usr/bin/env python3
"""Patches an AOSP android-17.0.0_r1 checkout so that guest code never uses x25-x28.

The Nestvane rewriter (experiments/g0/e3/rewrite.py) keeps the host runtime's state in those
registers, so every guest binary has to leave them alone. x18 is also kept out of generated code
because iOS zeroes it on every kernel entry.

usage: patch.py <aosp root>      (safe to run again; every edit checks whether it is already applied)
"""
import os
import sys

ROOT = sys.argv[1]
RESERVED = ("x25", "x26", "x27", "x28")


def edit(path, replacements, marker):
    """Applies (old, new) pairs once. `marker` is text that only exists after patching."""
    full = os.path.join(ROOT, path)
    text = open(full).read()
    if marker in text:
        print("already patched", path)
        return
    for old, new in replacements:
        if text.count(old) != 1:
            sys.exit("%s: expected exactly one match for %r, found %d" % (path, old[:60], text.count(old)))
        text = text.replace(old, new)
    if marker not in text:
        sys.exit("%s: marker missing after patch" % path)
    open(full, "w").write(text)
    print("patched", path)


# C, C++ and assembly for every arm64 device module.
edit("build/soong/cc/config/arm64_device.go", [(
    '''		"-fstack-clash-protection",
	}
''', '''		"-fstack-clash-protection",

		// Nestvane: registers owned by the host runtime.
		"-ffixed-x18",
		"-ffixed-x25",
		"-ffixed-x26",
		"-ffixed-x27",
		"-ffixed-x28",
	}
''')], "Nestvane: registers owned by the host runtime")

# Rust for every arm64 device module. These are LLVM subtarget features.
edit("build/soong/rust/config/arm64_device.go", [(
    '''		"-C force-frame-pointers=y",
''', '''		"-C force-frame-pointers=y",
		// Nestvane: registers owned by the host runtime.
		"-C target-feature=+reserve-x18,+reserve-x25,+reserve-x26,+reserve-x27,+reserve-x28",
''')], "Nestvane: registers owned by the host runtime")

# setjmp must neither scramble the reserved registers in place nor restore them: the rewritten
# code uses them between any two guest instructions, and after a fork a saved value is the parent's.
edit("bionic/libc/arch-arm64/bionic/setjmp.S", [
    ("  eor x25, x25, \\reg\n  eor x26, x26, \\reg\n  eor x27, x27, \\reg\n  eor x28, x28, \\reg\n",
     "  // Nestvane: x25-x28 belong to the host runtime and are neither mangled, saved nor restored.\n"),
    ("  stp x28, x29, [x0, #(_JB_X28_X29 * 8)]\n  stp x26, x27, [x0, #(_JB_X26_X27 * 8)]\n  stp x24, x25, [x0, #(_JB_X24_X25 * 8)]\n",
     "  stp xzr, x29, [x0, #(_JB_X28_X29 * 8)]\n  stp xzr, xzr, [x0, #(_JB_X26_X27 * 8)]\n  stp x24, xzr, [x0, #(_JB_X24_X25 * 8)]\n"),
    ("  ldp x28, x29, [x0, #(_JB_X28_X29 * 8)]\n  ldp x26, x27, [x0, #(_JB_X26_X27 * 8)]\n  ldp x24, x25, [x0, #(_JB_X24_X25 * 8)]\n",
     "  ldr x29, [x0, #(_JB_X28_X29 * 8 + 8)]\n  ldr x24, [x0, #(_JB_X24_X25 * 8)]\n"),
], "Nestvane: x25-x28 belong to the host runtime")

# ---------------------------------------------------------------- ART
# Compiled code must not allocate the reserved registers.
edit("art/compiler/optimizing/code_generator_arm64.h", [(
    "Location ARM64ReturnLocation(DataType::Type return_type);\n",
    "// Nestvane: registers owned by the host runtime. Compiled code never touches them.\n"
    "const vixl::aarch64::CPURegList nestvane_reserved_core_registers(\n"
    "    vixl::aarch64::x25, vixl::aarch64::x26, vixl::aarch64::x27, vixl::aarch64::x28);\n"
    "Location ARM64ReturnLocation(DataType::Type return_type);\n")], "nestvane_reserved_core_registers")
edit("art/compiler/optimizing/code_generator_arm64.cc", [(
    "  reserved_core_registers.Combine(runtime_reserved_core_registers);\n",
    "  reserved_core_registers.Combine(runtime_reserved_core_registers);\n"
    "  reserved_core_registers.Combine(nestvane_reserved_core_registers);\n")], "nestvane_reserved_core_registers")
edit("art/compiler/optimizing/fast_compiler_arm64.cc", [(
    "  vixl::aarch64::x24,\n  vixl::aarch64::x25,\n  vixl::aarch64::x26,\n  vixl::aarch64::x27,\n  vixl::aarch64::x28,\n",
    "  vixl::aarch64::x24,\n  // Nestvane: x25-x28 belong to the host runtime.\n")], "Nestvane: x25-x28 belong to the host runtime")


def edit_all(path, old, new, count, marker):
    full = os.path.join(ROOT, path)
    text = open(full).read()
    if marker in text:
        print("already patched", path)
        return
    if text.count(old) != count:
        sys.exit("%s: expected %d matches for %r, found %d" % (path, count, old[:60], text.count(old)))
    open(full, "w").write(text.replace(old, new))
    print("patched", path)


# JNI stubs save and restore these lists around native calls, and a native call can be fork.
edit_all("art/compiler/jni/quick/arm64/calling_convention_arm64.cc",
         "    Arm64ManagedRegister::FromXRegister(X24),\n    Arm64ManagedRegister::FromXRegister(X25),\n"
         "    Arm64ManagedRegister::FromXRegister(X26),\n    Arm64ManagedRegister::FromXRegister(X27),\n"
         "    Arm64ManagedRegister::FromXRegister(X28),\n",
         "    Arm64ManagedRegister::FromXRegister(X24),\n    // Nestvane: x25-x28 belong to the host runtime.\n",
         2, "Nestvane: x25-x28 belong to the host runtime")

# Runtime entrypoints keep their frame layouts and still store the registers, but never load them
# back: a frame written before a fork holds the parent's values.
NOTE = "    // Nestvane: x25-x28 belong to the host runtime and are not restored.\n"
edit("art/runtime/arch/arm64/asm_support_arm64.S", [
    ("    RESTORE_TWO_REGS x25, x26, 48\n    RESTORE_TWO_REGS x27, x28, 64\n", NOTE),
    ("    RESTORE_TWO_REGS_BASE \\base, x25, x26, 176\n    RESTORE_TWO_REGS_BASE \\base, x27, x28, 192\n", NOTE),
], "Nestvane: x25-x28 belong to the host runtime")
edit("art/runtime/arch/arm64/jni_entrypoints_arm64.S", [
    ("    RESTORE_TWO_REGS_BASE x29, x25, x26, 176\n    RESTORE_TWO_REGS_BASE x29, x27, x28, 192\n", NOTE),
    # x24 is saved in the same frame and restored afterwards, like x28 was.
    ("    mov   x28, x0\n", "    mov   x24, x0\n"),
    ("    mov   x0, x28\n", "    mov   x0, x24\n"),
], "Nestvane: x25-x28 belong to the host runtime")
edit("art/runtime/arch/arm64/quick_entrypoints_arm64.S", [
    ("    RESTORE_TWO_REGS x25, x26, 464\n    RESTORE_TWO_REGS x27, x28, 480\n", NOTE),
    ("    RESTORE_TWO_REGS x25, x26, 64\n    RESTORE_TWO_REGS x27, x28, 80\n", NOTE),
    ("    RESTORE_TWO_REGS x25, x26, 176\n    RESTORE_TWO_REGS x27, x28, 192\n", NOTE),
    ("    ldp x24, x25, [sp, #192]\n    ldp x26, x27, [sp, #208]\n    ldp x28, x29, [sp, #224]\n",
     "    ldr x24, [sp, #192]\n    // Nestvane: x25-x28 belong to the host runtime and are not restored.\n    ldr x29, [sp, #232]\n"),
    # The generic JNI trampoline kept the managed sp in x28. x24 is free there and restored at the end.
    ("    mov x28, sp\n    .cfi_def_cfa_register x28\n", "    mov x24, sp\n    .cfi_def_cfa_register x24\n"),
    ("    mov x1, x28     // SP for the managed frame.\n", "    mov x1, x24     // SP for the managed frame.\n"),
    ("    // Tear down the alloca.\n    mov sp, x28\n", "    // Tear down the alloca.\n    mov sp, x24\n"),
    ("    CFI_RESTORE_STATE_AND_DEF_CFA x28, FRAME_SIZE_SAVE_REFS_AND_ARGS\n", "    CFI_RESTORE_STATE_AND_DEF_CFA x24, FRAME_SIZE_SAVE_REFS_AND_ARGS\n"),
], "Nestvane: x25-x28 belong to the host runtime")

# nterp is hand-written around x25-x28. The switch interpreter is used until it is ported.
edit("art/runtime/interpreter/mterp/nterp.cc", [(
    "    case InstructionSet::kThumb2:\n    case InstructionSet::kArm64:\n      return kReserveMarkingRegister && !kUseTableLookupReadBarrier;\n",
    "    case InstructionSet::kThumb2:\n      return kReserveMarkingRegister && !kUseTableLookupReadBarrier;\n"
    "    case InstructionSet::kArm64:\n      return false;  // Nestvane: nterp uses x25-x28, which belong to the host runtime.\n")],
    "Nestvane: nterp uses x25-x28")

# ---------------------------------------------------------------- dynamic linker
# The host translates each system ELF ahead of time for one load address, and tells the linker in
# the auxiliary vector: entry 0x4e56 points at records of (device, inode, address) and entry 0x4e57
# is their count. On a real kernel the entries are absent and nothing changes. It is not a system
# call, because seccomp filters kill a process that makes one they do not know.
edit("bionic/linker/linker_phdr.cpp", [(
    "    start = ReserveWithAlignmentPadding(load_size_, kLibraryAlignment, start_alignment, &gap_start_,\n"
    "                                        &gap_size_);\n",
    "    // Nestvane: the host lists the address each file was translated for in the auxiliary vector.\n"
    "    start = nullptr;\n"
    "    {\n"
    "      struct NestvaneFile { uint64_t device, inode, address; };\n"
    "      const NestvaneFile* files = reinterpret_cast<const NestvaneFile*>(getauxval(0x4e56));\n"
    "      const uint64_t count = getauxval(0x4e57);\n"
    "      struct stat info;\n"
    "      if (files != nullptr && fstat(fd_, &info) == 0) {\n"
    "        for (uint64_t i = 0; i < count; ++i) {\n"
    "          if (files[i].device != static_cast<uint64_t>(info.st_dev) || files[i].inode != info.st_ino) continue;\n"
    "          void* fixed = mmap(reinterpret_cast<void*>(files[i].address), load_size_, PROT_NONE,\n"
    "                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);\n"
    "          if (fixed != MAP_FAILED) {\n"
    "            start = fixed;\n"
    "            gap_start_ = nullptr;\n"
    "            gap_size_ = 0;\n"
    "          }\n"
    "          break;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (start == nullptr) {\n"
    "      start = ReserveWithAlignmentPadding(load_size_, kLibraryAlignment, start_alignment, &gap_start_,\n"
    "                                          &gap_size_);\n"
    "    }\n"),
    ('#include "linker_phdr.h"\n', '#include "linker_phdr.h"\n\n#include <sys/auxv.h>  // Nestvane\n#include <sys/stat.h>\n')],
    "Nestvane: the host lists")

# ---------------------------------------------------------------- hand-written assembly in libraries
# BoringSSL's generated assembly uses x25-x28 as working registers. Its C implementations do not.
edit("external/boringssl/Android.bp", [(
    '''        "-DBORINGSSL_SHARED_LIBRARY",
        "-DOPENSSL_SMALL",
''', '''        "-DBORINGSSL_SHARED_LIBRARY",
        "-DOPENSSL_SMALL",
        "-DOPENSSL_NO_ASM", // Nestvane: the assembly uses registers owned by the host runtime.
''')], "Nestvane: the assembly uses registers")

# Follow-ups to the ART register lists above: both have static assertions about their length.
# The JNI compiler takes up to four scratch registers from the list. They were X22-X29; they are now
# X22, X23, X24 and X29.
edit("art/compiler/jni/quick/arm64/calling_convention_arm64.cc", [(
    "  // Use X22-X29 from native callee saves.\n  constexpr size_t kStart = 3u;\n  constexpr size_t kLength = 8u;\n",
    "  // Nestvane: use X22-X24 and X29. X25-X28 are not in the list any more.\n  constexpr size_t kStart = 3u;\n  constexpr size_t kLength = 4u;\n"),
], "Nestvane: use X22-X24 and X29")
edit("art/compiler/optimizing/fast_compiler_arm64.cc", [
    ("  vixl::aarch64::x11,\n  vixl::aarch64::x12,\n  vixl::aarch64::x13,\n  vixl::aarch64::x14,\n  vixl::aarch64::x15,\n};",
     "  vixl::aarch64::x11,\n  // Nestvane: four registers per class, to match the callee-saved list.\n};"),
    ("  vixl::aarch64::d11,\n  vixl::aarch64::d12,\n  vixl::aarch64::d13,\n  vixl::aarch64::d14,\n  vixl::aarch64::d15,\n};", "  vixl::aarch64::d11,\n};"),
    ("  vixl::aarch64::d19,\n  vixl::aarch64::d20,\n  vixl::aarch64::d21,\n  vixl::aarch64::d22,\n  vixl::aarch64::d23,\n};", "  vixl::aarch64::d19,\n};"),
], "Nestvane: four registers per class")

# TensorFlow Lite's hand-written arm64 depthwise convolution names x25-x28. GOOGLE_L4T is its own
# switch for building without that assembly.
edit("external/tensorflow/tensorflow/lite/Android.bp", [(
    '''    name: "tflite_defaults",
    vendor_available: true,
    host_supported: true,
    header_libs: [
        "tensorflow_headers",
    ],
    cflags: [
''', '''    name: "tflite_defaults",
    vendor_available: true,
    host_supported: true,
    header_libs: [
        "tensorflow_headers",
    ],
    cflags: [
        "-DGOOGLE_L4T", // Nestvane: no assembly that uses the host runtime's registers.
''')], "Nestvane: no assembly that uses")

# ruy's arm64 kernels are inline assembly whose operands and clobbers need more registers than are
# left. Its NEON paths cannot be built without them, so ruy is built with its portable C++ paths.
edit("external/ruy/ruy/platform.h", [(
    "#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !RUY_PLATFORM_X86\n#define RUY_PLATFORM_NEON 1\n",
    "// Nestvane: the NEON kernels are assembly that does not fit in the registers left to the guest.\n"
    "#if 0\n#define RUY_PLATFORM_NEON 1\n")],
    "Nestvane: the NEON kernels")

# The same switch for TensorFlow Lite's dot-product kernels, whose assembly takes more operands
# than there are registers left. Without them it uses its NEON intrinsics.
edit_all("external/tensorflow/tensorflow/lite/kernels/internal/optimized/neon_tensor_utils.cc",
         "#ifdef __aarch64__\n", "#if defined(__aarch64__) && !defined(GOOGLE_L4T)  // Nestvane\n", 10, "!defined(GOOGLE_L4T)  // Nestvane")

# gemmlowp's arm64 kernels are assembly of the same kind. TensorFlow Lite only instantiates them
# when it is not told to use ruy for everything.
edit("external/tensorflow/tensorflow/lite/Android.bp", [(
    '        "-DGOOGLE_L4T", // Nestvane: no assembly that uses the host runtime\'s registers.\n',
    '        "-DGOOGLE_L4T", // Nestvane: no assembly that uses the host runtime\'s registers.\n'
    '        "-DTFLITE_WITH_RUY", // Nestvane: no gemmlowp kernels, which are assembly on arm64.\n')],
    "-DTFLITE_WITH_RUY")

# The static library that most users link is built from its own list of flags.
edit("external/tensorflow/tensorflow/lite/tflite_static.bp", [(
    '        "-DNAMESPACE_FOR_HASH_FUNCTIONS=farmhash",\n',
    '        "-DNAMESPACE_FOR_HASH_FUNCTIONS=farmhash",\n'
    '        "-DGOOGLE_L4T", // Nestvane: no assembly kernels, as in tflite_defaults.\n'
    '        "-DTFLITE_WITH_RUY",\n')],
    "-DGOOGLE_L4T")

# There is no JIT on the host: a method that is not compiled ahead of time runs in the interpreter
# for good. So the boot image, the mainline boot class path extension, the system server and the
# apps are compiled in full rather than by profile, and nothing is left for the device to compile.
edit_all("build/soong/java/dexpreopt_config.go", 'compilerFilter:       "speed-profile",', 'compilerFilter:       "speed", // Nestvane', 2, '"speed", // Nestvane')
edit("build/soong/java/dexpreopt_config.go", [
    ('mainlineBcpCompilerFilter := "verify"', 'mainlineBcpCompilerFilter := "speed" // Nestvane: was verify'),
    ('mainlineBcpCompilerFilter = "speed-profile"', 'mainlineBcpCompilerFilter = "speed"')],
    "Nestvane: was verify")
edit("device/generic/goldfish/64bitonly/product/sdk_phone16k_arm64.mk", [(
    "\nPRODUCT_NAME := sdk_phone16k_arm64\n",
    "\n# Nestvane: compile everything ahead of time, there is no JIT on the host.\n"
    "PRODUCT_SYSTEM_SERVER_COMPILER_FILTER := speed\nPRODUCT_DEX_PREOPT_DEFAULT_COMPILER_FILTER := speed\n"
    "\nPRODUCT_NAME := sdk_phone16k_arm64\n")],
    "Nestvane: compile everything ahead of time")

# So is the copy that the federated compute library builds, through the flex delegate.
edit("external/tensorflow/Android.bp", [(
    '        "-DTF_ANDROID_ENABLE_LOGSINK",\n',
    '        "-DTF_ANDROID_ENABLE_LOGSINK",\n'
    '        "-DGOOGLE_L4T", // Nestvane: no assembly kernels, as in tflite_defaults.\n'
    '        "-DTFLITE_WITH_RUY",\n')],
    "-DGOOGLE_L4T")

# An ART test lists every callee-saved register as clobbered by its assembly, which the compiler
# refuses for reserved ones.
edit("art/runtime/arch/stub_test.cc", [(
    '"x22", "x23", "x24", "x25", "x26", "x27", "x28", "x30",', '"x22", "x23", "x24", "x30",  // Nestvane: without x25-x28')],
    "Nestvane: without x25-x28")

# Compiled in full, the images no longer fit in the emulator's 1.8 GiB of dynamic partitions.
edit("device/generic/goldfish/64bitonly/product/sdk_phone16k_arm64.mk", [(
    "BOARD_EMULATOR_DYNAMIC_PARTITIONS_SIZE ?= $(shell expr 1800 \\* 1048576 )",
    "BOARD_EMULATOR_DYNAMIC_PARTITIONS_SIZE ?= $(shell expr 4096 \\* 1048576 )  # Nestvane: was 1800")],
    "Nestvane: was 1800")

# Cronet carries its own BoringSSL, twice, with the same assembly.
for flavour in ("stable", "tot"):
    edit("external/cronet/%s/third_party/boringssl/Android.bp" % flavour, [(
        '    name: "%s_cronet_boringssl_defaults",\n    stl: "none",\n' % flavour,
        '    name: "%s_cronet_boringssl_defaults",\n    stl: "none",\n'
        '    cflags: ["-DOPENSSL_NO_ASM"], // Nestvane: the assembly uses registers owned by the host runtime.\n'
        '    asflags: ["-DOPENSSL_NO_ASM"],\n' % flavour)],
        "Nestvane: the assembly uses registers")

# ---------------------------------------------------------------- behaviour of the guest (1.16)
# The guest has no Wi-Fi hardware: its Wi-Fi service only reports that Wi-Fi is off, but apps,
# Settings and the system server's own services expect it to exist. Taken through the boot with the
# other services it costs about 0.3 s, so where ro.nestvane.late_wifi is set the Wi-Fi services are
# created with the others but go through the boot phases, and are told of the user, once the boot
# has completed. Without the property (the reference emulator) nothing changes.
SSM = "frameworks/base/services/core/java/com/android/server/SystemServiceManager.java"
edit(SSM, [
    ("    /** Disallow starting new services after this call. */\n",
     "    // Nestvane: see experiments/aosp/patch.py in the Nestvane repository.\n"
     "    private final boolean mLateWifi =\n"
     "            android.os.SystemProperties.getBoolean(\"ro.nestvane.late_wifi\", false);\n"
     "    private volatile boolean mLateStarted;\n"
     "\n"
     "    private boolean isLate(SystemService service) {\n"
     "        return mLateWifi && !mLateStarted\n"
     "                && service.getClass().getName().startsWith(\"com.android.server.wifi.\");\n"
     "    }\n"
     "\n"
     "    private void startLateServices() {\n"
     "        final ArrayList<SystemService> late = new ArrayList<>();\n"
     "        for (int i = 0; i < mServices.size(); i++) {\n"
     "            if (isLate(mServices.get(i))) late.add(mServices.get(i));\n"
     "        }\n"
     "        mLateStarted = true;\n"
     "        final TargetUser user;\n"
     "        synchronized (mTargetUsers) {\n"
     "            user = mCurrentUser;\n"
     "        }\n"
     "        final boolean unlocked = user != null && LocalServices.getService(UserManagerInternal.class)\n"
     "                .isUserUnlockingOrUnlocked(user.getUserIdentifier());\n"
     "        final int[] phases = {SystemService.PHASE_WAIT_FOR_DEFAULT_DISPLAY,\n"
     "                SystemService.PHASE_WAIT_FOR_SENSOR_SERVICE,\n"
     "                SystemService.PHASE_LOCK_SETTINGS_READY,\n"
     "                SystemService.PHASE_SYSTEM_SERVICES_READY,\n"
     "                SystemService.PHASE_DEVICE_SPECIFIC_SERVICES_READY,\n"
     "                SystemService.PHASE_ACTIVITY_MANAGER_READY,\n"
     "                SystemService.PHASE_THIRD_PARTY_APPS_CAN_START,\n"
     "                SystemService.PHASE_BOOT_COMPLETED};\n"
     "        for (SystemService service : late) {\n"
     "            final String name = service.getClass().getName();\n"
     "            try {\n"
     "                for (int phase : phases) {\n"
     "                    if (phase <= mCurrentPhase) service.onBootPhase(phase);\n"
     "                }\n"
     "                if (user != null && service.isUserSupported(user)) {\n"
     "                    service.onUserStarting(user);\n"
     "                    if (unlocked) {\n"
     "                        service.onUserUnlocking(user);\n"
     "                        service.onUserUnlocked(user);\n"
     "                    }\n"
     "                }\n"
     "                Slog.i(TAG, \"Started late: \" + name);\n"
     "            } catch (RuntimeException e) {\n"
     "                Slog.e(TAG, \"Starting late: \" + name, e);\n"
     "            }\n"
     "        }\n"
     "    }\n"
     "\n"
     "    /** Disallow starting new services after this call. */\n"),
    ("                final SystemService service = mServices.get(i);\n"
     "                if (!Flags.parallelizeOnbootphase() || service.getBootPhaseSerial(mCurrentPhase)) {\n",
     "                final SystemService service = mServices.get(i);\n"
     "                if (isLate(service)) continue;\n"
     "                if (!Flags.parallelizeOnbootphase() || service.getBootPhaseSerial(mCurrentPhase)) {\n"),
    ("            final String serviceName = service.getClass().getName();\n"
     "            boolean supported = service.isUserSupported(curUser);\n",
     "            final String serviceName = service.getClass().getName();\n"
     "            if (isLate(service)) continue;\n"
     "            boolean supported = service.isUserSupported(curUser);\n"),
    ("        if (phase == SystemService.PHASE_BOOT_COMPLETED) {\n"
     "            final long totalBootTime",
     "        if (phase == SystemService.PHASE_BOOT_COMPLETED) {\n"
     "            if (mLateWifi) {\n"
     "                new android.os.Handler(android.os.Looper.getMainLooper())\n"
     "                        .postDelayed(this::startLateServices, 3000);\n"
     "            }\n"
     "            final long totalBootTime")],
    "ro.nestvane.late_wifi")

# The home screen follows the device's rotation on a phone too. Launcher3 only does so on a large
# screen; the setting that allowed it on phones is gone. Auto-rotate being off still holds it still.
edit("packages/apps/Launcher3/src/com/android/launcher3/states/RotationHelper.java", [(
    "            activityFlags = SCREEN_ORIENTATION_NOSENSOR;\n",
    "            activityFlags = SCREEN_ORIENTATION_UNSPECIFIED;  // Nestvane: was NOSENSOR\n")],
    "Nestvane: was NOSENSOR")

# At a tablet's density the taskbar's buttons are told where the recents button is before the
# taskbar's controllers are set, and the launcher stops as it starts.
edit("packages/apps/Launcher3/quickstep/src/com/android/launcher3/taskbar/TaskbarNavButtonController.java", [(
    "        if (cueBarAceMigration() && mControllers.cueBarController != null) {\n"
    "            mControllers.cueBarController.onRecentsButtonLayoutChanged(bounds);\n",
    "        if (cueBarAceMigration() && mControllers != null  // Nestvane: not set yet at the first layout\n"
    "                && mControllers.cueBarController != null) {\n"
    "            mControllers.cueBarController.onRecentsButtonLayoutChanged(bounds);\n")],
    "Nestvane: not set yet at the first layout")

# The image's codec service for an emulator hands H.264 and HEVC streams to the host (the phone's
# decoders, runtime/GuestVideo.hpp). It can have the host draw a picture into a colour buffer or take
# it as bytes; the host here only gives bytes, so the service always asks for them.
for codec, name in (("avc", "Avc"), ("hevc", "Hevc")):
    edit("device/generic/goldfish/codecs/c2/decoders/%sdec/C2Goldfish%sDec.cpp" % (codec, name), [(
        "                mEnableAndroidNativeBuffers = true;\n",
        "                mEnableAndroidNativeBuffers = false;  // Nestvane: the host gives bytes\n")],
        "Nestvane: the host gives bytes")

# The codec service shares 32 MiB with the host and stops if it cannot have them. Its second 16 MiB
# are only where the decoders after the first keep their parameters; with half of it the first decoder
# still has room for a 3840x2160 picture, and the others start in the first 16 MiB.
edit("device/generic/goldfish/codecs/c2/decoders/base/goldfish_media_utils.cpp", [
    ("            int slot = search_order[i];\n",
     "            int slot = search_order[i];\n"
     "            if (slot >= 16) continue;  // Nestvane: the block is 16 MiB\n"),
    ("    static constexpr size_t kOutputSizeBytes = 4096 * 4096; // 16M\n",
     "    static constexpr size_t kOutputSizeBytes = 0;\n")],
    "Nestvane: the block is 16 MiB")
