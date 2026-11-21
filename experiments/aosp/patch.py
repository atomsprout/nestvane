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
