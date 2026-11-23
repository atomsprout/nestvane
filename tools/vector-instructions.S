// Written by tools/generate-vector-instructions.py: do not edit.
// One small function for each instruction, which runs it on the processor. x0 points at three
// 16-byte vectors and a 64-bit word: v0, v1 and v2 go in, v0 and the status register come out.
// The instruction is the fourth word of its function.
    .arch armv8.5-a+crypto+sha3
    .text
    .globl _vector_case_0
    .p2align 2
_vector_case_0:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_1
    .p2align 2
_vector_case_1:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_2
    .p2align 2
_vector_case_2:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_3
    .p2align 2
_vector_case_3:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_4
    .p2align 2
_vector_case_4:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_5
    .p2align 2
_vector_case_5:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_6
    .p2align 2
_vector_case_6:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_7
    .p2align 2
_vector_case_7:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_8
    .p2align 2
_vector_case_8:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_9
    .p2align 2
_vector_case_9:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_10
    .p2align 2
_vector_case_10:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_11
    .p2align 2
_vector_case_11:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_12
    .p2align 2
_vector_case_12:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_13
    .p2align 2
_vector_case_13:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_14
    .p2align 2
_vector_case_14:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_15
    .p2align 2
_vector_case_15:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_16
    .p2align 2
_vector_case_16:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_17
    .p2align 2
_vector_case_17:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_18
    .p2align 2
_vector_case_18:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_19
    .p2align 2
_vector_case_19:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_20
    .p2align 2
_vector_case_20:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_21
    .p2align 2
_vector_case_21:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_22
    .p2align 2
_vector_case_22:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_23
    .p2align 2
_vector_case_23:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_24
    .p2align 2
_vector_case_24:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shadd v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_25
    .p2align 2
_vector_case_25:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shadd v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_26
    .p2align 2
_vector_case_26:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shadd v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_27
    .p2align 2
_vector_case_27:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shadd v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_28
    .p2align 2
_vector_case_28:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhadd v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_29
    .p2align 2
_vector_case_29:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhadd v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_30
    .p2align 2
_vector_case_30:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhadd v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_31
    .p2align 2
_vector_case_31:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhadd v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_32
    .p2align 2
_vector_case_32:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srhadd v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_33
    .p2align 2
_vector_case_33:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srhadd v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_34
    .p2align 2
_vector_case_34:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srhadd v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_35
    .p2align 2
_vector_case_35:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srhadd v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_36
    .p2align 2
_vector_case_36:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urhadd v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_37
    .p2align 2
_vector_case_37:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urhadd v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_38
    .p2align 2
_vector_case_38:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urhadd v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_39
    .p2align 2
_vector_case_39:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urhadd v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_40
    .p2align 2
_vector_case_40:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shsub v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_41
    .p2align 2
_vector_case_41:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shsub v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_42
    .p2align 2
_vector_case_42:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shsub v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_43
    .p2align 2
_vector_case_43:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shsub v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_44
    .p2align 2
_vector_case_44:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhsub v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_45
    .p2align 2
_vector_case_45:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhsub v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_46
    .p2align 2
_vector_case_46:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhsub v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_47
    .p2align 2
_vector_case_47:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uhsub v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_48
    .p2align 2
_vector_case_48:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshl v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_49
    .p2align 2
_vector_case_49:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshl v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_50
    .p2align 2
_vector_case_50:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshl v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_51
    .p2align 2
_vector_case_51:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshl v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_52
    .p2align 2
_vector_case_52:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshl v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_53
    .p2align 2
_vector_case_53:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshl v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_54
    .p2align 2
_vector_case_54:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshl v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_55
    .p2align 2
_vector_case_55:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshl v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_56
    .p2align 2
_vector_case_56:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshl v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_57
    .p2align 2
_vector_case_57:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshl v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_58
    .p2align 2
_vector_case_58:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_59
    .p2align 2
_vector_case_59:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_60
    .p2align 2
_vector_case_60:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_61
    .p2align 2
_vector_case_61:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_62
    .p2align 2
_vector_case_62:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_63
    .p2align 2
_vector_case_63:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_64
    .p2align 2
_vector_case_64:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_65
    .p2align 2
_vector_case_65:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_66
    .p2align 2
_vector_case_66:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_67
    .p2align 2
_vector_case_67:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_68
    .p2align 2
_vector_case_68:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshl v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_69
    .p2align 2
_vector_case_69:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshl v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_70
    .p2align 2
_vector_case_70:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshl v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_71
    .p2align 2
_vector_case_71:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshl v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_72
    .p2align 2
_vector_case_72:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshl v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_73
    .p2align 2
_vector_case_73:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshl v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_74
    .p2align 2
_vector_case_74:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshl v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_75
    .p2align 2
_vector_case_75:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshl v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_76
    .p2align 2
_vector_case_76:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshl v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_77
    .p2align 2
_vector_case_77:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshl v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_78
    .p2align 2
_vector_case_78:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqdmulh v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_79
    .p2align 2
_vector_case_79:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqdmulh v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_80
    .p2align 2
_vector_case_80:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqdmulh v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_81
    .p2align 2
_vector_case_81:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqdmulh v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_82
    .p2align 2
_vector_case_82:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmulh v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_83
    .p2align 2
_vector_case_83:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmulh v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_84
    .p2align 2
_vector_case_84:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmulh v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_85
    .p2align 2
_vector_case_85:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmulh v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_86
    .p2align 2
_vector_case_86:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_87
    .p2align 2
_vector_case_87:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_88
    .p2align 2
_vector_case_88:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_89
    .p2align 2
_vector_case_89:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_90
    .p2align 2
_vector_case_90:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_91
    .p2align 2
_vector_case_91:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_92
    .p2align 2
_vector_case_92:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_93
    .p2align 2
_vector_case_93:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_94
    .p2align 2
_vector_case_94:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    rev16 v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_95
    .p2align 2
_vector_case_95:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    rev16 v0.8b, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_96
    .p2align 2
_vector_case_96:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    suqadd v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_97
    .p2align 2
_vector_case_97:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    suqadd v0.8h, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_98
    .p2align 2
_vector_case_98:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    suqadd v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_99
    .p2align 2
_vector_case_99:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    suqadd v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_100
    .p2align 2
_vector_case_100:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    suqadd v0.8b, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_101
    .p2align 2
_vector_case_101:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usqadd v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_102
    .p2align 2
_vector_case_102:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usqadd v0.8h, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_103
    .p2align 2
_vector_case_103:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usqadd v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_104
    .p2align 2
_vector_case_104:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usqadd v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_105
    .p2align 2
_vector_case_105:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usqadd v0.8b, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_106
    .p2align 2
_vector_case_106:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_107
    .p2align 2
_vector_case_107:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs v0.8h, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_108
    .p2align 2
_vector_case_108:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_109
    .p2align 2
_vector_case_109:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_110
    .p2align 2
_vector_case_110:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs v0.8b, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_111
    .p2align 2
_vector_case_111:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_112
    .p2align 2
_vector_case_112:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg v0.8h, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_113
    .p2align 2
_vector_case_113:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_114
    .p2align 2
_vector_case_114:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_115
    .p2align 2
_vector_case_115:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg v0.8b, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_116
    .p2align 2
_vector_case_116:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn v0.8b, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_117
    .p2align 2
_vector_case_117:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn v0.4h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_118
    .p2align 2
_vector_case_118:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn v0.2s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_119
    .p2align 2
_vector_case_119:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn2 v0.16b, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_120
    .p2align 2
_vector_case_120:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn2 v0.8h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_121
    .p2align 2
_vector_case_121:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn2 v0.4s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_122
    .p2align 2
_vector_case_122:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn v0.8b, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_123
    .p2align 2
_vector_case_123:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn v0.4h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_124
    .p2align 2
_vector_case_124:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn v0.2s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_125
    .p2align 2
_vector_case_125:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn2 v0.16b, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_126
    .p2align 2
_vector_case_126:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn2 v0.8h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_127
    .p2align 2
_vector_case_127:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn2 v0.4s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_128
    .p2align 2
_vector_case_128:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun v0.8b, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_129
    .p2align 2
_vector_case_129:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun v0.4h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_130
    .p2align 2
_vector_case_130:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun v0.2s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_131
    .p2align 2
_vector_case_131:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun2 v0.16b, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_132
    .p2align 2
_vector_case_132:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun2 v0.8h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_133
    .p2align 2
_vector_case_133:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun2 v0.4s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_134
    .p2align 2
_vector_case_134:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtn v0.2s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_135
    .p2align 2
_vector_case_135:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtn v0.4h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_136
    .p2align 2
_vector_case_136:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtn2 v0.4s, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_137
    .p2align 2
_vector_case_137:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtn2 v0.8h, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_138
    .p2align 2
_vector_case_138:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtas v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_139
    .p2align 2
_vector_case_139:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtas v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_140
    .p2align 2
_vector_case_140:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtas v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_141
    .p2align 2
_vector_case_141:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtau v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_142
    .p2align 2
_vector_case_142:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtau v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_143
    .p2align 2
_vector_case_143:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtau v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_144
    .p2align 2
_vector_case_144:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun v0.8b, v1.8h, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_145
    .p2align 2
_vector_case_145:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun v0.4h, v1.4s, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_146
    .p2align 2
_vector_case_146:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun v0.2s, v1.2d, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_147
    .p2align 2
_vector_case_147:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun2 v0.16b, v1.8h, #8
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_148
    .p2align 2
_vector_case_148:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun2 v0.8h, v1.4s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_149
    .p2align 2
_vector_case_149:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun2 v0.4s, v1.2d, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_150
    .p2align 2
_vector_case_150:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun v0.8b, v1.8h, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_151
    .p2align 2
_vector_case_151:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun v0.4h, v1.4s, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_152
    .p2align 2
_vector_case_152:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun v0.2s, v1.2d, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_153
    .p2align 2
_vector_case_153:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun2 v0.16b, v1.8h, #8
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_154
    .p2align 2
_vector_case_154:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun2 v0.8h, v1.4s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_155
    .p2align 2
_vector_case_155:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun2 v0.4s, v1.2d, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_156
    .p2align 2
_vector_case_156:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn v0.8b, v1.8h, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_157
    .p2align 2
_vector_case_157:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn v0.4h, v1.4s, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_158
    .p2align 2
_vector_case_158:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn v0.2s, v1.2d, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_159
    .p2align 2
_vector_case_159:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn2 v0.16b, v1.8h, #8
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_160
    .p2align 2
_vector_case_160:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn2 v0.8h, v1.4s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_161
    .p2align 2
_vector_case_161:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn2 v0.4s, v1.2d, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_162
    .p2align 2
_vector_case_162:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn v0.8b, v1.8h, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_163
    .p2align 2
_vector_case_163:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn v0.4h, v1.4s, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_164
    .p2align 2
_vector_case_164:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn v0.2s, v1.2d, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_165
    .p2align 2
_vector_case_165:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn2 v0.16b, v1.8h, #8
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_166
    .p2align 2
_vector_case_166:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn2 v0.8h, v1.4s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_167
    .p2align 2
_vector_case_167:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn2 v0.4s, v1.2d, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_168
    .p2align 2
_vector_case_168:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.16b, v1.16b, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_169
    .p2align 2
_vector_case_169:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.8h, v1.8h, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_170
    .p2align 2
_vector_case_170:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.4s, v1.4s, #0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_171
    .p2align 2
_vector_case_171:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.2d, v1.2d, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_172
    .p2align 2
_vector_case_172:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshl v0.4h, v1.4h, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_173
    .p2align 2
_vector_case_173:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.16b, v1.16b, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_174
    .p2align 2
_vector_case_174:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.8h, v1.8h, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_175
    .p2align 2
_vector_case_175:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.4s, v1.4s, #0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_176
    .p2align 2
_vector_case_176:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.2d, v1.2d, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_177
    .p2align 2
_vector_case_177:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshl v0.4h, v1.4h, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_178
    .p2align 2
_vector_case_178:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshlu v0.16b, v1.16b, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_179
    .p2align 2
_vector_case_179:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshlu v0.8h, v1.8h, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_180
    .p2align 2
_vector_case_180:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshlu v0.4s, v1.4s, #0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_181
    .p2align 2
_vector_case_181:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshlu v0.2d, v1.2d, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_182
    .p2align 2
_vector_case_182:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshlu v0.4h, v1.4h, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_183
    .p2align 2
_vector_case_183:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaddlv h0, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_184
    .p2align 2
_vector_case_184:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaddlv h0, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_185
    .p2align 2
_vector_case_185:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaddlv s0, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_186
    .p2align 2
_vector_case_186:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaddlv s0, v1.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_187
    .p2align 2
_vector_case_187:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaddlv d0, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_188
    .p2align 2
_vector_case_188:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saddlv h0, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_189
    .p2align 2
_vector_case_189:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saddlv s0, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_190
    .p2align 2
_vector_case_190:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saddlv d0, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_191
    .p2align 2
_vector_case_191:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    add d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_192
    .p2align 2
_vector_case_192:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sub d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_193
    .p2align 2
_vector_case_193:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri v0.4s, v1.4s, #20
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_194
    .p2align 2
_vector_case_194:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri v0.16b, v1.16b, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_195
    .p2align 2
_vector_case_195:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri v0.2d, v1.2d, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_196
    .p2align 2
_vector_case_196:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri v0.8h, v1.8h, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_197
    .p2align 2
_vector_case_197:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri v0.2s, v1.2s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_198
    .p2align 2
_vector_case_198:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sli v0.4s, v1.4s, #12
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_199
    .p2align 2
_vector_case_199:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    aese v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_200
    .p2align 2
_vector_case_200:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    aesd v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_201
    .p2align 2
_vector_case_201:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    aesmc v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_202
    .p2align 2
_vector_case_202:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    aesimc v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_203
    .p2align 2
_vector_case_203:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha1c q0, s1, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_204
    .p2align 2
_vector_case_204:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha1p q0, s1, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_205
    .p2align 2
_vector_case_205:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha1m q0, s1, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_206
    .p2align 2
_vector_case_206:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha1h s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_207
    .p2align 2
_vector_case_207:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha1su0 v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_208
    .p2align 2
_vector_case_208:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha1su1 v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_209
    .p2align 2
_vector_case_209:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha512h q0, q1, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_210
    .p2align 2
_vector_case_210:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha512h2 q0, q1, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_211
    .p2align 2
_vector_case_211:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha512su0 v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_212
    .p2align 2
_vector_case_212:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sha512su1 v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_213
    .p2align 2
_vector_case_213:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    eor3 v0.16b, v1.16b, v2.16b, v0.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_214
    .p2align 2
_vector_case_214:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    bcax v0.16b, v1.16b, v2.16b, v0.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_215
    .p2align 2
_vector_case_215:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    rax1 v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_216
    .p2align 2
_vector_case_216:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    xar v0.2d, v1.2d, v2.2d, #17
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_217
    .p2align 2
_vector_case_217:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    xar v0.2d, v1.2d, v2.2d, #0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_218
    .p2align 2
_vector_case_218:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    xar v0.2d, v1.2d, v2.2d, #63
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_219
    .p2align 2
_vector_case_219:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sshr d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_220
    .p2align 2
_vector_case_220:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sshr d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_221
    .p2align 2
_vector_case_221:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sshr d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_222
    .p2align 2
_vector_case_222:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ushr d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_223
    .p2align 2
_vector_case_223:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ushr d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_224
    .p2align 2
_vector_case_224:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ushr d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_225
    .p2align 2
_vector_case_225:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ssra d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_226
    .p2align 2
_vector_case_226:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ssra d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_227
    .p2align 2
_vector_case_227:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ssra d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_228
    .p2align 2
_vector_case_228:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usra d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_229
    .p2align 2
_vector_case_229:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usra d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_230
    .p2align 2
_vector_case_230:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    usra d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_231
    .p2align 2
_vector_case_231:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshr d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_232
    .p2align 2
_vector_case_232:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshr d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_233
    .p2align 2
_vector_case_233:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srshr d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_234
    .p2align 2
_vector_case_234:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshr d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_235
    .p2align 2
_vector_case_235:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshr d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_236
    .p2align 2
_vector_case_236:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    urshr d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_237
    .p2align 2
_vector_case_237:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srsra d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_238
    .p2align 2
_vector_case_238:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srsra d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_239
    .p2align 2
_vector_case_239:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    srsra d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_240
    .p2align 2
_vector_case_240:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ursra d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_241
    .p2align 2
_vector_case_241:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ursra d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_242
    .p2align 2
_vector_case_242:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ursra d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_243
    .p2align 2
_vector_case_243:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri d0, d1, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_244
    .p2align 2
_vector_case_244:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri d0, d1, #33
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_245
    .p2align 2
_vector_case_245:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sri d0, d1, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_246
    .p2align 2
_vector_case_246:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shl d0, d1, #0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_247
    .p2align 2
_vector_case_247:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shl d0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_248
    .p2align 2
_vector_case_248:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    shl d0, d1, #63
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_249
    .p2align 2
_vector_case_249:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sli d0, d1, #0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_250
    .p2align 2
_vector_case_250:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sli d0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_251
    .p2align 2
_vector_case_251:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sli d0, d1, #63
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_252
    .p2align 2
_vector_case_252:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.4s, v1.4s, v2.s[0]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_253
    .p2align 2
_vector_case_253:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.4s, v1.4s, v2.s[3]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_254
    .p2align 2
_vector_case_254:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.8h, v1.8h, v2.h[5]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_255
    .p2align 2
_vector_case_255:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.4h, v1.4h, v2.h[7]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_256
    .p2align 2
_vector_case_256:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlah v0.2s, v1.2s, v2.s[1]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_257
    .p2align 2
_vector_case_257:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.4s, v1.4s, v2.s[0]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_258
    .p2align 2
_vector_case_258:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.4s, v1.4s, v2.s[3]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_259
    .p2align 2
_vector_case_259:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.8h, v1.8h, v2.h[5]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_260
    .p2align 2
_vector_case_260:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.4h, v1.4h, v2.h[7]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_261
    .p2align 2
_vector_case_261:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrdmlsh v0.2s, v1.2s, v2.s[1]
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_262
    .p2align 2
_vector_case_262:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtl v0.4s, v1.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_263
    .p2align 2
_vector_case_263:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtl2 v0.4s, v1.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_264
    .p2align 2
_vector_case_264:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    faddp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_265
    .p2align 2
_vector_case_265:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    faddp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_266
    .p2align 2
_vector_case_266:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    faddp v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_267
    .p2align 2
_vector_case_267:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sshl d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_268
    .p2align 2
_vector_case_268:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ushl d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_269
    .p2align 2
_vector_case_269:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    rbit v0.16b, v1.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_270
    .p2align 2
_vector_case_270:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    rbit v0.8b, v1.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .section __DATA,__const
    .globl _vector_cases
    .p2align 3
_vector_cases:
    .quad _vector_case_0
    .quad L_name_0
    .quad _vector_case_1
    .quad L_name_1
    .quad _vector_case_2
    .quad L_name_2
    .quad _vector_case_3
    .quad L_name_3
    .quad _vector_case_4
    .quad L_name_4
    .quad _vector_case_5
    .quad L_name_5
    .quad _vector_case_6
    .quad L_name_6
    .quad _vector_case_7
    .quad L_name_7
    .quad _vector_case_8
    .quad L_name_8
    .quad _vector_case_9
    .quad L_name_9
    .quad _vector_case_10
    .quad L_name_10
    .quad _vector_case_11
    .quad L_name_11
    .quad _vector_case_12
    .quad L_name_12
    .quad _vector_case_13
    .quad L_name_13
    .quad _vector_case_14
    .quad L_name_14
    .quad _vector_case_15
    .quad L_name_15
    .quad _vector_case_16
    .quad L_name_16
    .quad _vector_case_17
    .quad L_name_17
    .quad _vector_case_18
    .quad L_name_18
    .quad _vector_case_19
    .quad L_name_19
    .quad _vector_case_20
    .quad L_name_20
    .quad _vector_case_21
    .quad L_name_21
    .quad _vector_case_22
    .quad L_name_22
    .quad _vector_case_23
    .quad L_name_23
    .quad _vector_case_24
    .quad L_name_24
    .quad _vector_case_25
    .quad L_name_25
    .quad _vector_case_26
    .quad L_name_26
    .quad _vector_case_27
    .quad L_name_27
    .quad _vector_case_28
    .quad L_name_28
    .quad _vector_case_29
    .quad L_name_29
    .quad _vector_case_30
    .quad L_name_30
    .quad _vector_case_31
    .quad L_name_31
    .quad _vector_case_32
    .quad L_name_32
    .quad _vector_case_33
    .quad L_name_33
    .quad _vector_case_34
    .quad L_name_34
    .quad _vector_case_35
    .quad L_name_35
    .quad _vector_case_36
    .quad L_name_36
    .quad _vector_case_37
    .quad L_name_37
    .quad _vector_case_38
    .quad L_name_38
    .quad _vector_case_39
    .quad L_name_39
    .quad _vector_case_40
    .quad L_name_40
    .quad _vector_case_41
    .quad L_name_41
    .quad _vector_case_42
    .quad L_name_42
    .quad _vector_case_43
    .quad L_name_43
    .quad _vector_case_44
    .quad L_name_44
    .quad _vector_case_45
    .quad L_name_45
    .quad _vector_case_46
    .quad L_name_46
    .quad _vector_case_47
    .quad L_name_47
    .quad _vector_case_48
    .quad L_name_48
    .quad _vector_case_49
    .quad L_name_49
    .quad _vector_case_50
    .quad L_name_50
    .quad _vector_case_51
    .quad L_name_51
    .quad _vector_case_52
    .quad L_name_52
    .quad _vector_case_53
    .quad L_name_53
    .quad _vector_case_54
    .quad L_name_54
    .quad _vector_case_55
    .quad L_name_55
    .quad _vector_case_56
    .quad L_name_56
    .quad _vector_case_57
    .quad L_name_57
    .quad _vector_case_58
    .quad L_name_58
    .quad _vector_case_59
    .quad L_name_59
    .quad _vector_case_60
    .quad L_name_60
    .quad _vector_case_61
    .quad L_name_61
    .quad _vector_case_62
    .quad L_name_62
    .quad _vector_case_63
    .quad L_name_63
    .quad _vector_case_64
    .quad L_name_64
    .quad _vector_case_65
    .quad L_name_65
    .quad _vector_case_66
    .quad L_name_66
    .quad _vector_case_67
    .quad L_name_67
    .quad _vector_case_68
    .quad L_name_68
    .quad _vector_case_69
    .quad L_name_69
    .quad _vector_case_70
    .quad L_name_70
    .quad _vector_case_71
    .quad L_name_71
    .quad _vector_case_72
    .quad L_name_72
    .quad _vector_case_73
    .quad L_name_73
    .quad _vector_case_74
    .quad L_name_74
    .quad _vector_case_75
    .quad L_name_75
    .quad _vector_case_76
    .quad L_name_76
    .quad _vector_case_77
    .quad L_name_77
    .quad _vector_case_78
    .quad L_name_78
    .quad _vector_case_79
    .quad L_name_79
    .quad _vector_case_80
    .quad L_name_80
    .quad _vector_case_81
    .quad L_name_81
    .quad _vector_case_82
    .quad L_name_82
    .quad _vector_case_83
    .quad L_name_83
    .quad _vector_case_84
    .quad L_name_84
    .quad _vector_case_85
    .quad L_name_85
    .quad _vector_case_86
    .quad L_name_86
    .quad _vector_case_87
    .quad L_name_87
    .quad _vector_case_88
    .quad L_name_88
    .quad _vector_case_89
    .quad L_name_89
    .quad _vector_case_90
    .quad L_name_90
    .quad _vector_case_91
    .quad L_name_91
    .quad _vector_case_92
    .quad L_name_92
    .quad _vector_case_93
    .quad L_name_93
    .quad _vector_case_94
    .quad L_name_94
    .quad _vector_case_95
    .quad L_name_95
    .quad _vector_case_96
    .quad L_name_96
    .quad _vector_case_97
    .quad L_name_97
    .quad _vector_case_98
    .quad L_name_98
    .quad _vector_case_99
    .quad L_name_99
    .quad _vector_case_100
    .quad L_name_100
    .quad _vector_case_101
    .quad L_name_101
    .quad _vector_case_102
    .quad L_name_102
    .quad _vector_case_103
    .quad L_name_103
    .quad _vector_case_104
    .quad L_name_104
    .quad _vector_case_105
    .quad L_name_105
    .quad _vector_case_106
    .quad L_name_106
    .quad _vector_case_107
    .quad L_name_107
    .quad _vector_case_108
    .quad L_name_108
    .quad _vector_case_109
    .quad L_name_109
    .quad _vector_case_110
    .quad L_name_110
    .quad _vector_case_111
    .quad L_name_111
    .quad _vector_case_112
    .quad L_name_112
    .quad _vector_case_113
    .quad L_name_113
    .quad _vector_case_114
    .quad L_name_114
    .quad _vector_case_115
    .quad L_name_115
    .quad _vector_case_116
    .quad L_name_116
    .quad _vector_case_117
    .quad L_name_117
    .quad _vector_case_118
    .quad L_name_118
    .quad _vector_case_119
    .quad L_name_119
    .quad _vector_case_120
    .quad L_name_120
    .quad _vector_case_121
    .quad L_name_121
    .quad _vector_case_122
    .quad L_name_122
    .quad _vector_case_123
    .quad L_name_123
    .quad _vector_case_124
    .quad L_name_124
    .quad _vector_case_125
    .quad L_name_125
    .quad _vector_case_126
    .quad L_name_126
    .quad _vector_case_127
    .quad L_name_127
    .quad _vector_case_128
    .quad L_name_128
    .quad _vector_case_129
    .quad L_name_129
    .quad _vector_case_130
    .quad L_name_130
    .quad _vector_case_131
    .quad L_name_131
    .quad _vector_case_132
    .quad L_name_132
    .quad _vector_case_133
    .quad L_name_133
    .quad _vector_case_134
    .quad L_name_134
    .quad _vector_case_135
    .quad L_name_135
    .quad _vector_case_136
    .quad L_name_136
    .quad _vector_case_137
    .quad L_name_137
    .quad _vector_case_138
    .quad L_name_138
    .quad _vector_case_139
    .quad L_name_139
    .quad _vector_case_140
    .quad L_name_140
    .quad _vector_case_141
    .quad L_name_141
    .quad _vector_case_142
    .quad L_name_142
    .quad _vector_case_143
    .quad L_name_143
    .quad _vector_case_144
    .quad L_name_144
    .quad _vector_case_145
    .quad L_name_145
    .quad _vector_case_146
    .quad L_name_146
    .quad _vector_case_147
    .quad L_name_147
    .quad _vector_case_148
    .quad L_name_148
    .quad _vector_case_149
    .quad L_name_149
    .quad _vector_case_150
    .quad L_name_150
    .quad _vector_case_151
    .quad L_name_151
    .quad _vector_case_152
    .quad L_name_152
    .quad _vector_case_153
    .quad L_name_153
    .quad _vector_case_154
    .quad L_name_154
    .quad _vector_case_155
    .quad L_name_155
    .quad _vector_case_156
    .quad L_name_156
    .quad _vector_case_157
    .quad L_name_157
    .quad _vector_case_158
    .quad L_name_158
    .quad _vector_case_159
    .quad L_name_159
    .quad _vector_case_160
    .quad L_name_160
    .quad _vector_case_161
    .quad L_name_161
    .quad _vector_case_162
    .quad L_name_162
    .quad _vector_case_163
    .quad L_name_163
    .quad _vector_case_164
    .quad L_name_164
    .quad _vector_case_165
    .quad L_name_165
    .quad _vector_case_166
    .quad L_name_166
    .quad _vector_case_167
    .quad L_name_167
    .quad _vector_case_168
    .quad L_name_168
    .quad _vector_case_169
    .quad L_name_169
    .quad _vector_case_170
    .quad L_name_170
    .quad _vector_case_171
    .quad L_name_171
    .quad _vector_case_172
    .quad L_name_172
    .quad _vector_case_173
    .quad L_name_173
    .quad _vector_case_174
    .quad L_name_174
    .quad _vector_case_175
    .quad L_name_175
    .quad _vector_case_176
    .quad L_name_176
    .quad _vector_case_177
    .quad L_name_177
    .quad _vector_case_178
    .quad L_name_178
    .quad _vector_case_179
    .quad L_name_179
    .quad _vector_case_180
    .quad L_name_180
    .quad _vector_case_181
    .quad L_name_181
    .quad _vector_case_182
    .quad L_name_182
    .quad _vector_case_183
    .quad L_name_183
    .quad _vector_case_184
    .quad L_name_184
    .quad _vector_case_185
    .quad L_name_185
    .quad _vector_case_186
    .quad L_name_186
    .quad _vector_case_187
    .quad L_name_187
    .quad _vector_case_188
    .quad L_name_188
    .quad _vector_case_189
    .quad L_name_189
    .quad _vector_case_190
    .quad L_name_190
    .quad _vector_case_191
    .quad L_name_191
    .quad _vector_case_192
    .quad L_name_192
    .quad _vector_case_193
    .quad L_name_193
    .quad _vector_case_194
    .quad L_name_194
    .quad _vector_case_195
    .quad L_name_195
    .quad _vector_case_196
    .quad L_name_196
    .quad _vector_case_197
    .quad L_name_197
    .quad _vector_case_198
    .quad L_name_198
    .quad _vector_case_199
    .quad L_name_199
    .quad _vector_case_200
    .quad L_name_200
    .quad _vector_case_201
    .quad L_name_201
    .quad _vector_case_202
    .quad L_name_202
    .quad _vector_case_203
    .quad L_name_203
    .quad _vector_case_204
    .quad L_name_204
    .quad _vector_case_205
    .quad L_name_205
    .quad _vector_case_206
    .quad L_name_206
    .quad _vector_case_207
    .quad L_name_207
    .quad _vector_case_208
    .quad L_name_208
    .quad _vector_case_209
    .quad L_name_209
    .quad _vector_case_210
    .quad L_name_210
    .quad _vector_case_211
    .quad L_name_211
    .quad _vector_case_212
    .quad L_name_212
    .quad _vector_case_213
    .quad L_name_213
    .quad _vector_case_214
    .quad L_name_214
    .quad _vector_case_215
    .quad L_name_215
    .quad _vector_case_216
    .quad L_name_216
    .quad _vector_case_217
    .quad L_name_217
    .quad _vector_case_218
    .quad L_name_218
    .quad _vector_case_219
    .quad L_name_219
    .quad _vector_case_220
    .quad L_name_220
    .quad _vector_case_221
    .quad L_name_221
    .quad _vector_case_222
    .quad L_name_222
    .quad _vector_case_223
    .quad L_name_223
    .quad _vector_case_224
    .quad L_name_224
    .quad _vector_case_225
    .quad L_name_225
    .quad _vector_case_226
    .quad L_name_226
    .quad _vector_case_227
    .quad L_name_227
    .quad _vector_case_228
    .quad L_name_228
    .quad _vector_case_229
    .quad L_name_229
    .quad _vector_case_230
    .quad L_name_230
    .quad _vector_case_231
    .quad L_name_231
    .quad _vector_case_232
    .quad L_name_232
    .quad _vector_case_233
    .quad L_name_233
    .quad _vector_case_234
    .quad L_name_234
    .quad _vector_case_235
    .quad L_name_235
    .quad _vector_case_236
    .quad L_name_236
    .quad _vector_case_237
    .quad L_name_237
    .quad _vector_case_238
    .quad L_name_238
    .quad _vector_case_239
    .quad L_name_239
    .quad _vector_case_240
    .quad L_name_240
    .quad _vector_case_241
    .quad L_name_241
    .quad _vector_case_242
    .quad L_name_242
    .quad _vector_case_243
    .quad L_name_243
    .quad _vector_case_244
    .quad L_name_244
    .quad _vector_case_245
    .quad L_name_245
    .quad _vector_case_246
    .quad L_name_246
    .quad _vector_case_247
    .quad L_name_247
    .quad _vector_case_248
    .quad L_name_248
    .quad _vector_case_249
    .quad L_name_249
    .quad _vector_case_250
    .quad L_name_250
    .quad _vector_case_251
    .quad L_name_251
    .quad _vector_case_252
    .quad L_name_252
    .quad _vector_case_253
    .quad L_name_253
    .quad _vector_case_254
    .quad L_name_254
    .quad _vector_case_255
    .quad L_name_255
    .quad _vector_case_256
    .quad L_name_256
    .quad _vector_case_257
    .quad L_name_257
    .quad _vector_case_258
    .quad L_name_258
    .quad _vector_case_259
    .quad L_name_259
    .quad _vector_case_260
    .quad L_name_260
    .quad _vector_case_261
    .quad L_name_261
    .quad _vector_case_262
    .quad L_name_262
    .quad _vector_case_263
    .quad L_name_263
    .quad _vector_case_264
    .quad L_name_264
    .quad _vector_case_265
    .quad L_name_265
    .quad _vector_case_266
    .quad L_name_266
    .quad _vector_case_267
    .quad L_name_267
    .quad _vector_case_268
    .quad L_name_268
    .quad _vector_case_269
    .quad L_name_269
    .quad _vector_case_270
    .quad L_name_270
    .quad 0
    .quad 0
    .section __TEXT,__cstring
L_name_0: .asciz "sqadd v0.16b, v1.16b, v2.16b"
L_name_1: .asciz "sqadd v0.8h, v1.8h, v2.8h"
L_name_2: .asciz "sqadd v0.4s, v1.4s, v2.4s"
L_name_3: .asciz "sqadd v0.2d, v1.2d, v2.2d"
L_name_4: .asciz "sqadd v0.8b, v1.8b, v2.8b"
L_name_5: .asciz "sqadd v0.2s, v1.2s, v2.2s"
L_name_6: .asciz "uqadd v0.16b, v1.16b, v2.16b"
L_name_7: .asciz "uqadd v0.8h, v1.8h, v2.8h"
L_name_8: .asciz "uqadd v0.4s, v1.4s, v2.4s"
L_name_9: .asciz "uqadd v0.2d, v1.2d, v2.2d"
L_name_10: .asciz "uqadd v0.8b, v1.8b, v2.8b"
L_name_11: .asciz "uqadd v0.2s, v1.2s, v2.2s"
L_name_12: .asciz "sqsub v0.16b, v1.16b, v2.16b"
L_name_13: .asciz "sqsub v0.8h, v1.8h, v2.8h"
L_name_14: .asciz "sqsub v0.4s, v1.4s, v2.4s"
L_name_15: .asciz "sqsub v0.2d, v1.2d, v2.2d"
L_name_16: .asciz "sqsub v0.8b, v1.8b, v2.8b"
L_name_17: .asciz "sqsub v0.2s, v1.2s, v2.2s"
L_name_18: .asciz "uqsub v0.16b, v1.16b, v2.16b"
L_name_19: .asciz "uqsub v0.8h, v1.8h, v2.8h"
L_name_20: .asciz "uqsub v0.4s, v1.4s, v2.4s"
L_name_21: .asciz "uqsub v0.2d, v1.2d, v2.2d"
L_name_22: .asciz "uqsub v0.8b, v1.8b, v2.8b"
L_name_23: .asciz "uqsub v0.2s, v1.2s, v2.2s"
L_name_24: .asciz "shadd v0.16b, v1.16b, v2.16b"
L_name_25: .asciz "shadd v0.8h, v1.8h, v2.8h"
L_name_26: .asciz "shadd v0.4s, v1.4s, v2.4s"
L_name_27: .asciz "shadd v0.8b, v1.8b, v2.8b"
L_name_28: .asciz "uhadd v0.16b, v1.16b, v2.16b"
L_name_29: .asciz "uhadd v0.8h, v1.8h, v2.8h"
L_name_30: .asciz "uhadd v0.4s, v1.4s, v2.4s"
L_name_31: .asciz "uhadd v0.8b, v1.8b, v2.8b"
L_name_32: .asciz "srhadd v0.16b, v1.16b, v2.16b"
L_name_33: .asciz "srhadd v0.8h, v1.8h, v2.8h"
L_name_34: .asciz "srhadd v0.4s, v1.4s, v2.4s"
L_name_35: .asciz "srhadd v0.8b, v1.8b, v2.8b"
L_name_36: .asciz "urhadd v0.16b, v1.16b, v2.16b"
L_name_37: .asciz "urhadd v0.8h, v1.8h, v2.8h"
L_name_38: .asciz "urhadd v0.4s, v1.4s, v2.4s"
L_name_39: .asciz "urhadd v0.8b, v1.8b, v2.8b"
L_name_40: .asciz "shsub v0.16b, v1.16b, v2.16b"
L_name_41: .asciz "shsub v0.8h, v1.8h, v2.8h"
L_name_42: .asciz "shsub v0.4s, v1.4s, v2.4s"
L_name_43: .asciz "shsub v0.8b, v1.8b, v2.8b"
L_name_44: .asciz "uhsub v0.16b, v1.16b, v2.16b"
L_name_45: .asciz "uhsub v0.8h, v1.8h, v2.8h"
L_name_46: .asciz "uhsub v0.4s, v1.4s, v2.4s"
L_name_47: .asciz "uhsub v0.8b, v1.8b, v2.8b"
L_name_48: .asciz "srshl v0.16b, v1.16b, v2.16b"
L_name_49: .asciz "srshl v0.8h, v1.8h, v2.8h"
L_name_50: .asciz "srshl v0.4s, v1.4s, v2.4s"
L_name_51: .asciz "srshl v0.2d, v1.2d, v2.2d"
L_name_52: .asciz "srshl v0.4h, v1.4h, v2.4h"
L_name_53: .asciz "urshl v0.16b, v1.16b, v2.16b"
L_name_54: .asciz "urshl v0.8h, v1.8h, v2.8h"
L_name_55: .asciz "urshl v0.4s, v1.4s, v2.4s"
L_name_56: .asciz "urshl v0.2d, v1.2d, v2.2d"
L_name_57: .asciz "urshl v0.4h, v1.4h, v2.4h"
L_name_58: .asciz "sqshl v0.16b, v1.16b, v2.16b"
L_name_59: .asciz "sqshl v0.8h, v1.8h, v2.8h"
L_name_60: .asciz "sqshl v0.4s, v1.4s, v2.4s"
L_name_61: .asciz "sqshl v0.2d, v1.2d, v2.2d"
L_name_62: .asciz "sqshl v0.4h, v1.4h, v2.4h"
L_name_63: .asciz "uqshl v0.16b, v1.16b, v2.16b"
L_name_64: .asciz "uqshl v0.8h, v1.8h, v2.8h"
L_name_65: .asciz "uqshl v0.4s, v1.4s, v2.4s"
L_name_66: .asciz "uqshl v0.2d, v1.2d, v2.2d"
L_name_67: .asciz "uqshl v0.4h, v1.4h, v2.4h"
L_name_68: .asciz "sqrshl v0.16b, v1.16b, v2.16b"
L_name_69: .asciz "sqrshl v0.8h, v1.8h, v2.8h"
L_name_70: .asciz "sqrshl v0.4s, v1.4s, v2.4s"
L_name_71: .asciz "sqrshl v0.2d, v1.2d, v2.2d"
L_name_72: .asciz "sqrshl v0.4h, v1.4h, v2.4h"
L_name_73: .asciz "uqrshl v0.16b, v1.16b, v2.16b"
L_name_74: .asciz "uqrshl v0.8h, v1.8h, v2.8h"
L_name_75: .asciz "uqrshl v0.4s, v1.4s, v2.4s"
L_name_76: .asciz "uqrshl v0.2d, v1.2d, v2.2d"
L_name_77: .asciz "uqrshl v0.4h, v1.4h, v2.4h"
L_name_78: .asciz "sqdmulh v0.8h, v1.8h, v2.8h"
L_name_79: .asciz "sqdmulh v0.4s, v1.4s, v2.4s"
L_name_80: .asciz "sqdmulh v0.4h, v1.4h, v2.4h"
L_name_81: .asciz "sqdmulh v0.2s, v1.2s, v2.2s"
L_name_82: .asciz "sqrdmulh v0.8h, v1.8h, v2.8h"
L_name_83: .asciz "sqrdmulh v0.4s, v1.4s, v2.4s"
L_name_84: .asciz "sqrdmulh v0.4h, v1.4h, v2.4h"
L_name_85: .asciz "sqrdmulh v0.2s, v1.2s, v2.2s"
L_name_86: .asciz "sqrdmlah v0.8h, v1.8h, v2.8h"
L_name_87: .asciz "sqrdmlah v0.4s, v1.4s, v2.4s"
L_name_88: .asciz "sqrdmlah v0.4h, v1.4h, v2.4h"
L_name_89: .asciz "sqrdmlah v0.2s, v1.2s, v2.2s"
L_name_90: .asciz "sqrdmlsh v0.8h, v1.8h, v2.8h"
L_name_91: .asciz "sqrdmlsh v0.4s, v1.4s, v2.4s"
L_name_92: .asciz "sqrdmlsh v0.4h, v1.4h, v2.4h"
L_name_93: .asciz "sqrdmlsh v0.2s, v1.2s, v2.2s"
L_name_94: .asciz "rev16 v0.16b, v1.16b"
L_name_95: .asciz "rev16 v0.8b, v1.8b"
L_name_96: .asciz "suqadd v0.16b, v1.16b"
L_name_97: .asciz "suqadd v0.8h, v1.8h"
L_name_98: .asciz "suqadd v0.4s, v1.4s"
L_name_99: .asciz "suqadd v0.2d, v1.2d"
L_name_100: .asciz "suqadd v0.8b, v1.8b"
L_name_101: .asciz "usqadd v0.16b, v1.16b"
L_name_102: .asciz "usqadd v0.8h, v1.8h"
L_name_103: .asciz "usqadd v0.4s, v1.4s"
L_name_104: .asciz "usqadd v0.2d, v1.2d"
L_name_105: .asciz "usqadd v0.8b, v1.8b"
L_name_106: .asciz "sqabs v0.16b, v1.16b"
L_name_107: .asciz "sqabs v0.8h, v1.8h"
L_name_108: .asciz "sqabs v0.4s, v1.4s"
L_name_109: .asciz "sqabs v0.2d, v1.2d"
L_name_110: .asciz "sqabs v0.8b, v1.8b"
L_name_111: .asciz "sqneg v0.16b, v1.16b"
L_name_112: .asciz "sqneg v0.8h, v1.8h"
L_name_113: .asciz "sqneg v0.4s, v1.4s"
L_name_114: .asciz "sqneg v0.2d, v1.2d"
L_name_115: .asciz "sqneg v0.8b, v1.8b"
L_name_116: .asciz "sqxtn v0.8b, v1.8h"
L_name_117: .asciz "sqxtn v0.4h, v1.4s"
L_name_118: .asciz "sqxtn v0.2s, v1.2d"
L_name_119: .asciz "sqxtn2 v0.16b, v1.8h"
L_name_120: .asciz "sqxtn2 v0.8h, v1.4s"
L_name_121: .asciz "sqxtn2 v0.4s, v1.2d"
L_name_122: .asciz "uqxtn v0.8b, v1.8h"
L_name_123: .asciz "uqxtn v0.4h, v1.4s"
L_name_124: .asciz "uqxtn v0.2s, v1.2d"
L_name_125: .asciz "uqxtn2 v0.16b, v1.8h"
L_name_126: .asciz "uqxtn2 v0.8h, v1.4s"
L_name_127: .asciz "uqxtn2 v0.4s, v1.2d"
L_name_128: .asciz "sqxtun v0.8b, v1.8h"
L_name_129: .asciz "sqxtun v0.4h, v1.4s"
L_name_130: .asciz "sqxtun v0.2s, v1.2d"
L_name_131: .asciz "sqxtun2 v0.16b, v1.8h"
L_name_132: .asciz "sqxtun2 v0.8h, v1.4s"
L_name_133: .asciz "sqxtun2 v0.4s, v1.2d"
L_name_134: .asciz "fcvtn v0.2s, v1.2d"
L_name_135: .asciz "fcvtn v0.4h, v1.4s"
L_name_136: .asciz "fcvtn2 v0.4s, v1.2d"
L_name_137: .asciz "fcvtn2 v0.8h, v1.4s"
L_name_138: .asciz "fcvtas v0.4s, v1.4s"
L_name_139: .asciz "fcvtas v0.2d, v1.2d"
L_name_140: .asciz "fcvtas v0.2s, v1.2s"
L_name_141: .asciz "fcvtau v0.4s, v1.4s"
L_name_142: .asciz "fcvtau v0.2d, v1.2d"
L_name_143: .asciz "fcvtau v0.2s, v1.2s"
L_name_144: .asciz "sqshrun v0.8b, v1.8h, #3"
L_name_145: .asciz "sqshrun v0.4h, v1.4s, #16"
L_name_146: .asciz "sqshrun v0.2s, v1.2d, #7"
L_name_147: .asciz "sqshrun2 v0.16b, v1.8h, #8"
L_name_148: .asciz "sqshrun2 v0.8h, v1.4s, #1"
L_name_149: .asciz "sqshrun2 v0.4s, v1.2d, #32"
L_name_150: .asciz "sqrshrun v0.8b, v1.8h, #3"
L_name_151: .asciz "sqrshrun v0.4h, v1.4s, #16"
L_name_152: .asciz "sqrshrun v0.2s, v1.2d, #7"
L_name_153: .asciz "sqrshrun2 v0.16b, v1.8h, #8"
L_name_154: .asciz "sqrshrun2 v0.8h, v1.4s, #1"
L_name_155: .asciz "sqrshrun2 v0.4s, v1.2d, #32"
L_name_156: .asciz "uqshrn v0.8b, v1.8h, #3"
L_name_157: .asciz "uqshrn v0.4h, v1.4s, #16"
L_name_158: .asciz "uqshrn v0.2s, v1.2d, #7"
L_name_159: .asciz "uqshrn2 v0.16b, v1.8h, #8"
L_name_160: .asciz "uqshrn2 v0.8h, v1.4s, #1"
L_name_161: .asciz "uqshrn2 v0.4s, v1.2d, #32"
L_name_162: .asciz "uqrshrn v0.8b, v1.8h, #3"
L_name_163: .asciz "uqrshrn v0.4h, v1.4s, #16"
L_name_164: .asciz "uqrshrn v0.2s, v1.2d, #7"
L_name_165: .asciz "uqrshrn2 v0.16b, v1.8h, #8"
L_name_166: .asciz "uqrshrn2 v0.8h, v1.4s, #1"
L_name_167: .asciz "uqrshrn2 v0.4s, v1.2d, #32"
L_name_168: .asciz "sqshl v0.16b, v1.16b, #3"
L_name_169: .asciz "sqshl v0.8h, v1.8h, #15"
L_name_170: .asciz "sqshl v0.4s, v1.4s, #0"
L_name_171: .asciz "sqshl v0.2d, v1.2d, #40"
L_name_172: .asciz "sqshl v0.4h, v1.4h, #7"
L_name_173: .asciz "uqshl v0.16b, v1.16b, #3"
L_name_174: .asciz "uqshl v0.8h, v1.8h, #15"
L_name_175: .asciz "uqshl v0.4s, v1.4s, #0"
L_name_176: .asciz "uqshl v0.2d, v1.2d, #40"
L_name_177: .asciz "uqshl v0.4h, v1.4h, #7"
L_name_178: .asciz "sqshlu v0.16b, v1.16b, #3"
L_name_179: .asciz "sqshlu v0.8h, v1.8h, #15"
L_name_180: .asciz "sqshlu v0.4s, v1.4s, #0"
L_name_181: .asciz "sqshlu v0.2d, v1.2d, #40"
L_name_182: .asciz "sqshlu v0.4h, v1.4h, #7"
L_name_183: .asciz "uaddlv h0, v1.16b"
L_name_184: .asciz "uaddlv h0, v1.8b"
L_name_185: .asciz "uaddlv s0, v1.8h"
L_name_186: .asciz "uaddlv s0, v1.4h"
L_name_187: .asciz "uaddlv d0, v1.4s"
L_name_188: .asciz "saddlv h0, v1.16b"
L_name_189: .asciz "saddlv s0, v1.8h"
L_name_190: .asciz "saddlv d0, v1.4s"
L_name_191: .asciz "add d0, d1, d2"
L_name_192: .asciz "sub d0, d1, d2"
L_name_193: .asciz "sri v0.4s, v1.4s, #20"
L_name_194: .asciz "sri v0.16b, v1.16b, #3"
L_name_195: .asciz "sri v0.2d, v1.2d, #64"
L_name_196: .asciz "sri v0.8h, v1.8h, #16"
L_name_197: .asciz "sri v0.2s, v1.2s, #1"
L_name_198: .asciz "sli v0.4s, v1.4s, #12"
L_name_199: .asciz "aese v0.16b, v1.16b"
L_name_200: .asciz "aesd v0.16b, v1.16b"
L_name_201: .asciz "aesmc v0.16b, v1.16b"
L_name_202: .asciz "aesimc v0.16b, v1.16b"
L_name_203: .asciz "sha1c q0, s1, v2.4s"
L_name_204: .asciz "sha1p q0, s1, v2.4s"
L_name_205: .asciz "sha1m q0, s1, v2.4s"
L_name_206: .asciz "sha1h s0, s1"
L_name_207: .asciz "sha1su0 v0.4s, v1.4s, v2.4s"
L_name_208: .asciz "sha1su1 v0.4s, v1.4s"
L_name_209: .asciz "sha512h q0, q1, v2.2d"
L_name_210: .asciz "sha512h2 q0, q1, v2.2d"
L_name_211: .asciz "sha512su0 v0.2d, v1.2d"
L_name_212: .asciz "sha512su1 v0.2d, v1.2d, v2.2d"
L_name_213: .asciz "eor3 v0.16b, v1.16b, v2.16b, v0.16b"
L_name_214: .asciz "bcax v0.16b, v1.16b, v2.16b, v0.16b"
L_name_215: .asciz "rax1 v0.2d, v1.2d, v2.2d"
L_name_216: .asciz "xar v0.2d, v1.2d, v2.2d, #17"
L_name_217: .asciz "xar v0.2d, v1.2d, v2.2d, #0"
L_name_218: .asciz "xar v0.2d, v1.2d, v2.2d, #63"
L_name_219: .asciz "sshr d0, d1, #1"
L_name_220: .asciz "sshr d0, d1, #33"
L_name_221: .asciz "sshr d0, d1, #64"
L_name_222: .asciz "ushr d0, d1, #1"
L_name_223: .asciz "ushr d0, d1, #33"
L_name_224: .asciz "ushr d0, d1, #64"
L_name_225: .asciz "ssra d0, d1, #1"
L_name_226: .asciz "ssra d0, d1, #33"
L_name_227: .asciz "ssra d0, d1, #64"
L_name_228: .asciz "usra d0, d1, #1"
L_name_229: .asciz "usra d0, d1, #33"
L_name_230: .asciz "usra d0, d1, #64"
L_name_231: .asciz "srshr d0, d1, #1"
L_name_232: .asciz "srshr d0, d1, #33"
L_name_233: .asciz "srshr d0, d1, #64"
L_name_234: .asciz "urshr d0, d1, #1"
L_name_235: .asciz "urshr d0, d1, #33"
L_name_236: .asciz "urshr d0, d1, #64"
L_name_237: .asciz "srsra d0, d1, #1"
L_name_238: .asciz "srsra d0, d1, #33"
L_name_239: .asciz "srsra d0, d1, #64"
L_name_240: .asciz "ursra d0, d1, #1"
L_name_241: .asciz "ursra d0, d1, #33"
L_name_242: .asciz "ursra d0, d1, #64"
L_name_243: .asciz "sri d0, d1, #1"
L_name_244: .asciz "sri d0, d1, #33"
L_name_245: .asciz "sri d0, d1, #64"
L_name_246: .asciz "shl d0, d1, #0"
L_name_247: .asciz "shl d0, d1, #32"
L_name_248: .asciz "shl d0, d1, #63"
L_name_249: .asciz "sli d0, d1, #0"
L_name_250: .asciz "sli d0, d1, #32"
L_name_251: .asciz "sli d0, d1, #63"
L_name_252: .asciz "sqrdmlah v0.4s, v1.4s, v2.s[0]"
L_name_253: .asciz "sqrdmlah v0.4s, v1.4s, v2.s[3]"
L_name_254: .asciz "sqrdmlah v0.8h, v1.8h, v2.h[5]"
L_name_255: .asciz "sqrdmlah v0.4h, v1.4h, v2.h[7]"
L_name_256: .asciz "sqrdmlah v0.2s, v1.2s, v2.s[1]"
L_name_257: .asciz "sqrdmlsh v0.4s, v1.4s, v2.s[0]"
L_name_258: .asciz "sqrdmlsh v0.4s, v1.4s, v2.s[3]"
L_name_259: .asciz "sqrdmlsh v0.8h, v1.8h, v2.h[5]"
L_name_260: .asciz "sqrdmlsh v0.4h, v1.4h, v2.h[7]"
L_name_261: .asciz "sqrdmlsh v0.2s, v1.2s, v2.s[1]"
L_name_262: .asciz "fcvtl v0.4s, v1.4h"
L_name_263: .asciz "fcvtl2 v0.4s, v1.8h"
L_name_264: .asciz "faddp v0.4s, v1.4s, v2.4s"
L_name_265: .asciz "faddp v0.2s, v1.2s, v2.2s"
L_name_266: .asciz "faddp v0.2d, v1.2d, v2.2d"
L_name_267: .asciz "sshl d0, d1, d2"
L_name_268: .asciz "ushl d0, d1, d2"
L_name_269: .asciz "rbit v0.16b, v1.16b"
L_name_270: .asciz "rbit v0.8b, v1.8b"
