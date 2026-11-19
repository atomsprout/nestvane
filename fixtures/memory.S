.text
.global _start, hot_start, hot_end
_start:
    movz x0, #0
    movz x9, #50000
    adr x3, counter
hot_start:
    add x0, x0, #1
    ldr x4, [x3]
    add x4, x4, #1
    str x4, [x3]
    add x0, x0, x4
    sub x9, x9, #1
    cbnz x9, hot_start
hot_end:
    add x19, x0, #0
    adr x1, message
    movz x0, #1
    movz x2, #13
    movz x8, #64
    svc #0
    movz x0, #0
    movz x8, #93
    svc #0

message:
    .ascii "memory ready\n"

.data
    .space 4092
counter:
    .quad 7
