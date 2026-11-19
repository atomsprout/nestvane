.text
.global _start
_start:
    adr x0, message
    bl strlen
    add x2, x0, #0
    adr x1, message
    movz x0, #1
    bl write
    sub x0, x0, #(message_end - message - 1)
    movz x8, #93
    svc #0

.rodata
message:
    .asciz "nestvane bionic ready!\n"
message_end:
