.text
.global _start
_start:
    mov x0, #-100
    adr x1, device
    mov x2, #0x802                 // O_RDWR | O_NONBLOCK
    mov x3, #0
    mov x8, #56                    // openat
    svc #0
    cmp x0, #0
    b.lt fail
    mov x19, x0

    adr x1, service
    mov x2, #14
    mov x8, #64                    // write
    svc #0
    cmp x0, #14
    b.ne fail

    mov x0, x19
    adr x1, request
    mov x2, #12
    mov x8, #64                    // write
    svc #0
    cmp x0, #12
    b.ne fail

    mov x0, x19
    adr x1, response
    mov x2, #4
    mov x8, #63                    // read
    svc #0
    cmp x0, #4
    b.ne fail
    adr x1, response
    ldr w1, [x1]
    cmp w1, #1
    b.ne fail

    mov x0, x19
    mov x8, #57                    // close
    svc #0
    cbnz x0, fail

    mov x8, #93                    // exit(0)
    svc #0
fail:
    mov x0, #1
    mov x8, #93
    svc #0

.section .rodata
device:
    .asciz "/dev/goldfish_pipe_dprctd"
service:
    .asciz "pipe:opengles"
.p2align 2
request:
    .word 0, 10000, 8              // Stream flags, rcGetRendererVersion, command size.
.data
.p2align 2
response:
    .word 0
