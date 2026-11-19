.text
.global _start
_start:
    sub sp, sp, #64

    mov x0, #-100
    adr x1, final_path
    mov x2, #0
    mov x3, #0
    mov x8, #56
    svc #0
    cmp x0, #0
    b.ge verify_file
    cmn x0, #2
    b.ne failed

    mov x0, #-100
    adr x1, pending_path
    mov x2, #0xc1
    mov x3, #0x180
    mov x8, #56
    svc #0
    cmp x0, #0
    b.lt failed
    mov x19, x0
    adr x1, expected
    mov x2, #24
    mov x8, #64
    svc #0
    cmp x0, #24
    b.ne failed

    mov x0, #-100
    adr x1, pending_path
    mov x2, #-100
    adr x3, final_path
    mov x8, #38
    svc #0
    cbnz x0, failed

    mov x0, x19
    mov x8, #82
    svc #0
    cbnz x0, failed
    mov x0, x19
    mov x8, #57
    svc #0
    cbnz x0, failed
    adr x1, created
    mov x2, #18
    b print_result

verify_file:
    mov x19, x0
    mov x1, sp
    mov x2, #25
    mov x8, #63
    svc #0
    cmp x0, #24
    b.ne failed
    adr x1, expected
    mov x2, sp
    mov x3, #24
compare:
    ldrb w4, [x1], #1
    ldrb w5, [x2], #1
    cmp w4, w5
    b.ne failed
    subs x3, x3, #1
    b.ne compare
    mov x0, x19
    mov x8, #57
    svc #0
    cbnz x0, failed
    adr x1, reopened
    mov x2, #18
print_result:
    mov x0, #1
    mov x8, #64
    svc #0
    cmp x0, #18
    b.ne failed
    mov x0, #0
    b exit
failed:
    mov x0, #1
exit:
    mov x8, #93
    svc #0

.rodata
final_path:
    .asciz "/data/.nestvane-storage-probe"
pending_path:
    .asciz "/data/.nestvane-storage-probe.pending"
expected:
    .ascii "nestvane-guest-data-v1!\n"
created:
    .ascii "guest data synced\n"
reopened:
    .ascii "guest data loaded\n"
