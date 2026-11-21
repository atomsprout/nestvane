// Transitions between rewritten guest code and the host runtime. Offsets match struct Context.
#define CTX_SP 248
#define CTX_Q 256
#define CTX_NZCV 768
#define CTX_PC 792
#define CTX_BASE 800
#define CTX_TABLE 808
#define CTX_HOST_SP 816
#define CTX_MEMORY_TABLE 824

.text
.p2align 2

// Reached from a rewritten svc with the return address in x28. Linux preserves every register
// except x0 across a system call, so all of them are saved before C code runs.
.globl _guest_svc
_guest_svc:
	stp x0, x1, [x25, #0]
	stp x2, x3, [x25, #16]
	stp x4, x5, [x25, #32]
	stp x6, x7, [x25, #48]
	stp x8, x9, [x25, #64]
	stp x10, x11, [x25, #80]
	stp x12, x13, [x25, #96]
	stp x14, x15, [x25, #112]
	stp x16, x17, [x25, #128]
	stp x18, x19, [x25, #144]
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
	// The handler runs on the host thread's own stack so that exec can replace the guest memory.
	ldr x0, [x25, #CTX_HOST_SP]
	mov sp, x0
	mov x0, x25
	bl _guest_syscall
	mov x0, x25
	b _guest_resume

// Where a guest signal handler returns: rt_sigreturn, issued like a rewritten svc.
.globl _guest_signal_return
_guest_signal_return:
	mov x8, #139
	adr x28, _guest_signal_return
	b _guest_svc

// void guest_enter(struct Context*): records the host stack and starts running the context.
.globl _guest_enter
_guest_enter:
	mov x9, sp
	str x9, [x0, #CTX_HOST_SP]

// Loads every register from the context in x0 and continues at its pc. Does not return.
.globl _guest_resume
_guest_resume:
	mov x25, x0
	ldr x26, [x25, #CTX_TABLE]
#ifdef SPARSE
	ldr x27, [x25, #CTX_MEMORY_TABLE]
#else
	ldr x27, [x25, #CTX_BASE]
#endif
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
