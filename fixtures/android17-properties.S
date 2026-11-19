.text
.global _start
_start:
    ldr x19, [sp]
    cmp x19, #4
    b.ne read_default_properties
    ldr x0, [sp, #32] // argv[3] is a single property to verify.
    bl print_property
    b exit
read_default_properties:
    adr x0, sdk
    bl print_property
    cbnz x0, exit
    adr x0, abi
    bl print_property
    cbnz x0, exit
    cmp x19, #2
    b.lt exit
    bl verify_runtime
    cbnz x0, exit
    cmp x19, #3
    b.ne exit
    adr x0, servicemanager_ready
    bl print_property
exit:
    movz x8, #93
    svc #0

print_property:
    stp x29, x30, [sp, #-112]!
    add x1, sp, #16
    bl __system_property_get
    cmp w0, #0
    b.le failed
    add x2, x0, #1
    add x1, sp, #16
    movz w3, #10
    strb w3, [x1, x0]
    add x29, x2, #0
    movz x0, #1
    bl write
    cmp x0, x29
    b.ne failed
    movz x0, #0
    b done
failed:
    movz x0, #1
done:
    ldp x29, x30, [sp], #112
    ret

verify_runtime:
    sub sp, sp, #32
    mov x0, #-100
    adr x1, runtime_linker
    mov x2, #0
    mov x3, #0
    mov x8, #56
    svc #0
    cmp x0, #0
    b.lt bad_runtime
    add x1, sp, #0
    mov x2, #24
    mov x8, #63
    svc #0
    cmp x0, #24
    b.ne bad_runtime
    ldr w4, [sp]
    movz w5, #0x457f
    movk w5, #0x464c, lsl #16
    cmp w4, w5
    b.ne bad_runtime
    ldrb w4, [sp, #4] // ELFCLASS64
    cmp w4, #2
    b.ne bad_runtime
    ldrh w4, [sp, #18] // EM_AARCH64
    cmp w4, #183
    b.ne bad_runtime
    add sp, sp, #32
    mov x0, #1
    adr x1, runtime_verified
    mov x2, #26
    mov x8, #64
    svc #0
    cmp x0, #26
    cset x0, ne
    ret
bad_runtime:
    add sp, sp, #32
    mov x0, #1
    ret

.rodata
sdk:
    .asciz "ro.build.version.sdk"
abi:
    .asciz "ro.product.cpu.abilist"
servicemanager_ready:
    .asciz "servicemanager.ready"
runtime_linker:
    .asciz "/apex/com.android.runtime/bin/linker64"
runtime_verified:
    .ascii "Runtime APEX ELF verified\n"
