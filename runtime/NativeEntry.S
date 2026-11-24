// Transitions between the runtime and rewritten guest code. Offsets match NativeContext in
// NativeCode.cpp, whose first part is laid out as experiments/g0/e3/rewrite.py expects.
#define CTX_SP 248
#define CTX_Q 256
#define CTX_NZCV 768
#define CTX_PC 792
#define CTX_TABLE 808
#define CTX_HOST_SP 816
#define CTX_MEMORY_TABLE 824
#define CTX_FPSR 856
#define CTX_SIGNAL_RETURN 880

.text
.p2align 2

// int nestvane_native_enter(NativeContext*): runs guest code from the context until it leaves, and
// returns why: 1 for a system call, or what the signal handler passed to nestvane_native_exit.
.globl _nestvane_native_enter
_nestvane_native_enter:
	stp x29, x30, [sp, #-176]!
	mov x29, sp
	stp x19, x20, [sp, #16]
	stp x21, x22, [sp, #32]
	stp x23, x24, [sp, #48]
	stp x25, x26, [sp, #64]
	stp x27, x28, [sp, #80]
	stp d8, d9, [sp, #96]
	stp d10, d11, [sp, #112]
	stp d12, d13, [sp, #128]
	stp d14, d15, [sp, #144]
	mrs x9, fpsr
	str x9, [sp, #160]
	mov x9, sp
	str x9, [x0, #CTX_HOST_SP]
	mov x25, x0
	ldr x26, [x25, #CTX_TABLE]
	ldr x27, [x25, #CTX_MEMORY_TABLE]
	ldr x0, [x25, #CTX_FPSR]
	msr fpsr, x0
	ldr x0, [x25, #CTX_SP]
	mov sp, x0
	add x0, x25, #CTX_Q
	ldp q0, q1, [x0, #0]
	ldp q2, q3, [x0, #32]
	ldp q4, q5, [x0, #64]
	ldp q6, q7, [x0, #96]
	ldp q8, q9, [x0, #128]
	ldp q10, q11, [x0, #160]
	ldp q12, q13, [x0, #192]
	ldp q14, q15, [x0, #224]
	ldp q16, q17, [x0, #256]
	ldp q18, q19, [x0, #288]
	ldp q20, q21, [x0, #320]
	ldp q22, q23, [x0, #352]
	ldp q24, q25, [x0, #384]
	ldp q26, q27, [x0, #416]
	ldp q28, q29, [x0, #448]
	ldp q30, q31, [x0, #480]
	ldr x0, [x25, #CTX_NZCV]
	msr nzcv, x0
	ldp x0, x1, [x25, #0]
	ldp x2, x3, [x25, #16]
	ldp x4, x5, [x25, #32]
	ldp x6, x7, [x25, #48]
	ldp x8, x9, [x25, #64]
	ldp x10, x11, [x25, #80]
	ldp x12, x13, [x25, #96]
	ldp x14, x15, [x25, #112]
	ldp x16, x17, [x25, #128]
	ldr x19, [x25, #152]
	ldp x20, x21, [x25, #160]
	ldp x22, x23, [x25, #176]
	ldr x24, [x25, #192]
	ldp x29, x30, [x25, #232]
	ldr x28, [x25, #CTX_PC]
	br x28

// Puts everything the guest can see into the context, with x28 as its pc. The kernel layer reads and
// writes the context as the guest's registers.
.macro SAVE_GUEST
	stp x0, x1, [x25, #0]
	stp x2, x3, [x25, #16]
	stp x4, x5, [x25, #32]
	stp x6, x7, [x25, #48]
	stp x8, x9, [x25, #64]
	stp x10, x11, [x25, #80]
	stp x12, x13, [x25, #96]
	stp x14, x15, [x25, #112]
	stp x16, x17, [x25, #128]
	str x19, [x25, #152]
	stp x20, x21, [x25, #160]
	stp x22, x23, [x25, #176]
	str x24, [x25, #192]
	stp x29, x30, [x25, #232]
	str x28, [x25, #CTX_PC]
	mrs x0, nzcv
	str x0, [x25, #CTX_NZCV]
	mov x0, sp
	str x0, [x25, #CTX_SP]
	add x0, x25, #CTX_Q
	stp q0, q1, [x0, #0]
	stp q2, q3, [x0, #32]
	stp q4, q5, [x0, #64]
	stp q6, q7, [x0, #96]
	stp q8, q9, [x0, #128]
	stp q10, q11, [x0, #160]
	stp q12, q13, [x0, #192]
	stp q14, q15, [x0, #224]
	stp q16, q17, [x0, #256]
	stp q18, q19, [x0, #288]
	stp q20, q21, [x0, #320]
	stp q22, q23, [x0, #352]
	stp q24, q25, [x0, #384]
	stp q26, q27, [x0, #416]
	stp q28, q29, [x0, #448]
	stp q30, q31, [x0, #480]
	mrs x0, fpsr
	str x0, [x25, #CTX_FPSR]
.endm

// Reached from a rewritten svc with the host address after it in x28, by way of the context (the
// translations are in images of their own).
.globl _nestvane_native_svc
_nestvane_native_svc:
	SAVE_GUEST
	mov w0, #1
	b _nestvane_native_exit

// Reached from a module's entry trap with the guest pc of the program's entry point in x28: a jump to
// it leaves native code, so that the kernel layer sees the program arrive there.
.globl _nestvane_native_trap
_nestvane_native_trap:
	SAVE_GUEST
	mov w0, #4

	b _nestvane_native_exit

// What the table of translated code holds for an address in a module that is no instruction: a jump
// there leaves native code, and the kernel layer makes it the guest's fault.
.globl _nestvane_native_stray
_nestvane_native_stray:
	SAVE_GUEST
	mov w0, #5
	b _nestvane_native_exit

// What the table of translated code holds for the kernel layer's signal return trampoline, which is
// no translated code: a handler that returns to it leaves native code there.
.globl _nestvane_native_signal_return
_nestvane_native_signal_return:
	ldr x28, [x25, #CTX_SIGNAL_RETURN]
	b _nestvane_native_trap

// What the table of translated code holds for interpreterReturnPc (Runtime.hpp), the last instruction
// of the signal return page: native code returning there leaves, and the runtime goes on at the return
// address it stands for, in code that is interpreted.
.globl _nestvane_native_interpreter_return
_nestvane_native_interpreter_return:
	ldr x28, [x25, #CTX_SIGNAL_RETURN]
	add x28, x28, #8
	b _nestvane_native_trap

// Leaves guest code with the reason in w0 and the context in x25. The guest's registers are in the
// context already: SAVE_GUEST put them there, or the signal handler did before it sent the thread here.
.globl _nestvane_native_exit
_nestvane_native_exit:
	ldr x9, [x25, #CTX_HOST_SP]
	mov sp, x9
	ldr x9, [sp, #160]
	msr fpsr, x9
	ldp x19, x20, [sp, #16]
	ldp x21, x22, [sp, #32]
	ldp x23, x24, [sp, #48]
	ldp x25, x26, [sp, #64]
	ldp x27, x28, [sp, #80]
	ldp d8, d9, [sp, #96]
	ldp d10, d11, [sp, #112]
	ldp d12, d13, [sp, #128]
	ldp d14, d15, [sp, #144]
	ldp x29, x30, [sp], #176
	ret
