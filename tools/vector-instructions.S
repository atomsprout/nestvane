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
    .globl _vector_case_271
    .p2align 2
_vector_case_271:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintn v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_272
    .p2align 2
_vector_case_272:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintn v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_273
    .p2align 2
_vector_case_273:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintn v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_274
    .p2align 2
_vector_case_274:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintm v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_275
    .p2align 2
_vector_case_275:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintm v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_276
    .p2align 2
_vector_case_276:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintm v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_277
    .p2align 2
_vector_case_277:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintp v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_278
    .p2align 2
_vector_case_278:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintp v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_279
    .p2align 2
_vector_case_279:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintp v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_280
    .p2align 2
_vector_case_280:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintz v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_281
    .p2align 2
_vector_case_281:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintz v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_282
    .p2align 2
_vector_case_282:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintz v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_283
    .p2align 2
_vector_case_283:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frinta v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_284
    .p2align 2
_vector_case_284:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frinta v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_285
    .p2align 2
_vector_case_285:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frinta v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_286
    .p2align 2
_vector_case_286:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintx v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_287
    .p2align 2
_vector_case_287:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintx v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_288
    .p2align 2
_vector_case_288:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frintx v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_289
    .p2align 2
_vector_case_289:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frinti v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_290
    .p2align 2
_vector_case_290:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frinti v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_291
    .p2align 2
_vector_case_291:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frinti v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_292
    .p2align 2
_vector_case_292:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecpe v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_293
    .p2align 2
_vector_case_293:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecpe v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_294
    .p2align 2
_vector_case_294:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecpe v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_295
    .p2align 2
_vector_case_295:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrte v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_296
    .p2align 2
_vector_case_296:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrte v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_297
    .p2align 2
_vector_case_297:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrte v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_298
    .p2align 2
_vector_case_298:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtns v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_299
    .p2align 2
_vector_case_299:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtns v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_300
    .p2align 2
_vector_case_300:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtns v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_301
    .p2align 2
_vector_case_301:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtms v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_302
    .p2align 2
_vector_case_302:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtms v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_303
    .p2align 2
_vector_case_303:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtms v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_304
    .p2align 2
_vector_case_304:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtps v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_305
    .p2align 2
_vector_case_305:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtps v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_306
    .p2align 2
_vector_case_306:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtps v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_307
    .p2align 2
_vector_case_307:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_308
    .p2align 2
_vector_case_308:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_309
    .p2align 2
_vector_case_309:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_310
    .p2align 2
_vector_case_310:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtnu v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_311
    .p2align 2
_vector_case_311:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtnu v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_312
    .p2align 2
_vector_case_312:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtnu v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_313
    .p2align 2
_vector_case_313:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtmu v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_314
    .p2align 2
_vector_case_314:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtmu v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_315
    .p2align 2
_vector_case_315:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtmu v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_316
    .p2align 2
_vector_case_316:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtpu v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_317
    .p2align 2
_vector_case_317:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtpu v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_318
    .p2align 2
_vector_case_318:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtpu v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_319
    .p2align 2
_vector_case_319:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.4s, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_320
    .p2align 2
_vector_case_320:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.2d, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_321
    .p2align 2
_vector_case_321:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.2s, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_322
    .p2align 2
_vector_case_322:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecpe s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_323
    .p2align 2
_vector_case_323:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecpe d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_324
    .p2align 2
_vector_case_324:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrte s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_325
    .p2align 2
_vector_case_325:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrte d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_326
    .p2align 2
_vector_case_326:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtns s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_327
    .p2align 2
_vector_case_327:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtns d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_328
    .p2align 2
_vector_case_328:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtms s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_329
    .p2align 2
_vector_case_329:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtms d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_330
    .p2align 2
_vector_case_330:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtps s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_331
    .p2align 2
_vector_case_331:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtps d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_332
    .p2align 2
_vector_case_332:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_333
    .p2align 2
_vector_case_333:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_334
    .p2align 2
_vector_case_334:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtnu s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_335
    .p2align 2
_vector_case_335:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtnu d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_336
    .p2align 2
_vector_case_336:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtmu s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_337
    .p2align 2
_vector_case_337:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtmu d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_338
    .p2align 2
_vector_case_338:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtpu s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_339
    .p2align 2
_vector_case_339:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtpu d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_340
    .p2align 2
_vector_case_340:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_341
    .p2align 2
_vector_case_341:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_342
    .p2align 2
_vector_case_342:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt s0, s1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_343
    .p2align 2
_vector_case_343:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt d0, d1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_344
    .p2align 2
_vector_case_344:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq s0, s1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_345
    .p2align 2
_vector_case_345:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq d0, d1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_346
    .p2align 2
_vector_case_346:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmlt s0, s1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_347
    .p2align 2
_vector_case_347:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmlt d0, d1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_348
    .p2align 2
_vector_case_348:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge s0, s1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_349
    .p2align 2
_vector_case_349:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge d0, d1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_350
    .p2align 2
_vector_case_350:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmle s0, s1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_351
    .p2align 2
_vector_case_351:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmle d0, d1, #0.0
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_352
    .p2align 2
_vector_case_352:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecps v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_353
    .p2align 2
_vector_case_353:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecps v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_354
    .p2align 2
_vector_case_354:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecps v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_355
    .p2align 2
_vector_case_355:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecps s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_356
    .p2align 2
_vector_case_356:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frecps d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_357
    .p2align 2
_vector_case_357:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrts v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_358
    .p2align 2
_vector_case_358:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrts v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_359
    .p2align 2
_vector_case_359:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrts v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_360
    .p2align 2
_vector_case_360:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrts s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_361
    .p2align 2
_vector_case_361:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    frsqrts d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_362
    .p2align 2
_vector_case_362:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmulx v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_363
    .p2align 2
_vector_case_363:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmulx v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_364
    .p2align 2
_vector_case_364:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmulx v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_365
    .p2align 2
_vector_case_365:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmulx s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_366
    .p2align 2
_vector_case_366:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmulx d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_367
    .p2align 2
_vector_case_367:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facge v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_368
    .p2align 2
_vector_case_368:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facge v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_369
    .p2align 2
_vector_case_369:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facge v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_370
    .p2align 2
_vector_case_370:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facge s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_371
    .p2align 2
_vector_case_371:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facge d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_372
    .p2align 2
_vector_case_372:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facgt v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_373
    .p2align 2
_vector_case_373:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facgt v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_374
    .p2align 2
_vector_case_374:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facgt v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_375
    .p2align 2
_vector_case_375:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facgt s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_376
    .p2align 2
_vector_case_376:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    facgt d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_377
    .p2align 2
_vector_case_377:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_378
    .p2align 2
_vector_case_378:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_379
    .p2align 2
_vector_case_379:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_380
    .p2align 2
_vector_case_380:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_381
    .p2align 2
_vector_case_381:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmeq d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_382
    .p2align 2
_vector_case_382:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_383
    .p2align 2
_vector_case_383:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_384
    .p2align 2
_vector_case_384:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_385
    .p2align 2
_vector_case_385:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_386
    .p2align 2
_vector_case_386:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmge d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_387
    .p2align 2
_vector_case_387:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_388
    .p2align 2
_vector_case_388:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_389
    .p2align 2
_vector_case_389:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_390
    .p2align 2
_vector_case_390:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_391
    .p2align 2
_vector_case_391:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcmgt d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_392
    .p2align 2
_vector_case_392:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_393
    .p2align 2
_vector_case_393:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxp v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_394
    .p2align 2
_vector_case_394:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_395
    .p2align 2
_vector_case_395:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxp s0, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_396
    .p2align 2
_vector_case_396:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxp d0, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_397
    .p2align 2
_vector_case_397:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_398
    .p2align 2
_vector_case_398:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminp v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_399
    .p2align 2
_vector_case_399:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_400
    .p2align 2
_vector_case_400:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminp s0, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_401
    .p2align 2
_vector_case_401:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminp d0, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_402
    .p2align 2
_vector_case_402:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxnmp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_403
    .p2align 2
_vector_case_403:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxnmp v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_404
    .p2align 2
_vector_case_404:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxnmp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_405
    .p2align 2
_vector_case_405:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxnmp s0, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_406
    .p2align 2
_vector_case_406:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxnmp d0, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_407
    .p2align 2
_vector_case_407:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminnmp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_408
    .p2align 2
_vector_case_408:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminnmp v0.2d, v1.2d, v2.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_409
    .p2align 2
_vector_case_409:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminnmp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_410
    .p2align 2
_vector_case_410:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminnmp s0, v1.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_411
    .p2align 2
_vector_case_411:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminnmp d0, v1.2d
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_412
    .p2align 2
_vector_case_412:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxnmv s0, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_413
    .p2align 2
_vector_case_413:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminnmv s0, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_414
    .p2align 2
_vector_case_414:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fmaxv s0, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_415
    .p2align 2
_vector_case_415:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fminv s0, v1.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_416
    .p2align 2
_vector_case_416:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sabal v0.8h, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_417
    .p2align 2
_vector_case_417:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sabal v0.4s, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_418
    .p2align 2
_vector_case_418:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sabal v0.2d, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_419
    .p2align 2
_vector_case_419:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sabal2 v0.8h, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_420
    .p2align 2
_vector_case_420:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sabal2 v0.4s, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_421
    .p2align 2
_vector_case_421:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sabal2 v0.2d, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_422
    .p2align 2
_vector_case_422:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uabal v0.8h, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_423
    .p2align 2
_vector_case_423:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uabal v0.4s, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_424
    .p2align 2
_vector_case_424:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uabal v0.2d, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_425
    .p2align 2
_vector_case_425:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uabal2 v0.8h, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_426
    .p2align 2
_vector_case_426:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uabal2 v0.4s, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_427
    .p2align 2
_vector_case_427:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uabal2 v0.2d, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_428
    .p2align 2
_vector_case_428:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saba v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_429
    .p2align 2
_vector_case_429:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saba v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_430
    .p2align 2
_vector_case_430:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saba v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_431
    .p2align 2
_vector_case_431:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saba v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_432
    .p2align 2
_vector_case_432:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saba v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_433
    .p2align 2
_vector_case_433:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    saba v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_434
    .p2align 2
_vector_case_434:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaba v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_435
    .p2align 2
_vector_case_435:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaba v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_436
    .p2align 2
_vector_case_436:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaba v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_437
    .p2align 2
_vector_case_437:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaba v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_438
    .p2align 2
_vector_case_438:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaba v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_439
    .p2align 2
_vector_case_439:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uaba v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_440
    .p2align 2
_vector_case_440:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    smaxp v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_441
    .p2align 2
_vector_case_441:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    smaxp v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_442
    .p2align 2
_vector_case_442:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    smaxp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_443
    .p2align 2
_vector_case_443:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    smaxp v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_444
    .p2align 2
_vector_case_444:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    smaxp v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_445
    .p2align 2
_vector_case_445:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    smaxp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_446
    .p2align 2
_vector_case_446:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    umaxp v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_447
    .p2align 2
_vector_case_447:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    umaxp v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_448
    .p2align 2
_vector_case_448:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    umaxp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_449
    .p2align 2
_vector_case_449:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    umaxp v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_450
    .p2align 2
_vector_case_450:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    umaxp v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_451
    .p2align 2
_vector_case_451:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    umaxp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_452
    .p2align 2
_vector_case_452:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sminp v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_453
    .p2align 2
_vector_case_453:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sminp v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_454
    .p2align 2
_vector_case_454:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sminp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_455
    .p2align 2
_vector_case_455:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sminp v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_456
    .p2align 2
_vector_case_456:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sminp v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_457
    .p2align 2
_vector_case_457:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sminp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_458
    .p2align 2
_vector_case_458:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uminp v0.16b, v1.16b, v2.16b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_459
    .p2align 2
_vector_case_459:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uminp v0.8h, v1.8h, v2.8h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_460
    .p2align 2
_vector_case_460:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uminp v0.4s, v1.4s, v2.4s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_461
    .p2align 2
_vector_case_461:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uminp v0.8b, v1.8b, v2.8b
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_462
    .p2align 2
_vector_case_462:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uminp v0.4h, v1.4h, v2.4h
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_463
    .p2align 2
_vector_case_463:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uminp v0.2s, v1.2s, v2.2s
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_464
    .p2align 2
_vector_case_464:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd b0, b1, b2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_465
    .p2align 2
_vector_case_465:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd h0, h1, h2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_466
    .p2align 2
_vector_case_466:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_467
    .p2align 2
_vector_case_467:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqadd d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_468
    .p2align 2
_vector_case_468:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd b0, b1, b2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_469
    .p2align 2
_vector_case_469:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd h0, h1, h2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_470
    .p2align 2
_vector_case_470:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_471
    .p2align 2
_vector_case_471:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqadd d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_472
    .p2align 2
_vector_case_472:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub b0, b1, b2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_473
    .p2align 2
_vector_case_473:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub h0, h1, h2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_474
    .p2align 2
_vector_case_474:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_475
    .p2align 2
_vector_case_475:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqsub d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_476
    .p2align 2
_vector_case_476:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub b0, b1, b2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_477
    .p2align 2
_vector_case_477:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub h0, h1, h2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_478
    .p2align 2
_vector_case_478:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub s0, s1, s2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_479
    .p2align 2
_vector_case_479:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqsub d0, d1, d2
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_480
    .p2align 2
_vector_case_480:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs b0, b1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_481
    .p2align 2
_vector_case_481:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs h0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_482
    .p2align 2
_vector_case_482:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_483
    .p2align 2
_vector_case_483:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqabs d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_484
    .p2align 2
_vector_case_484:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg b0, b1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_485
    .p2align 2
_vector_case_485:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg h0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_486
    .p2align 2
_vector_case_486:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_487
    .p2align 2
_vector_case_487:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqneg d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_488
    .p2align 2
_vector_case_488:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn b0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_489
    .p2align 2
_vector_case_489:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn h0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_490
    .p2align 2
_vector_case_490:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtn s0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_491
    .p2align 2
_vector_case_491:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn b0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_492
    .p2align 2
_vector_case_492:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn h0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_493
    .p2align 2
_vector_case_493:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqxtn s0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_494
    .p2align 2
_vector_case_494:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun b0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_495
    .p2align 2
_vector_case_495:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun h0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_496
    .p2align 2
_vector_case_496:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqxtun s0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_497
    .p2align 2
_vector_case_497:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrn b0, h1, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_498
    .p2align 2
_vector_case_498:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrn h0, s1, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_499
    .p2align 2
_vector_case_499:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrn s0, d1, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_500
    .p2align 2
_vector_case_500:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrn s0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_501
    .p2align 2
_vector_case_501:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrn b0, h1, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_502
    .p2align 2
_vector_case_502:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrn h0, s1, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_503
    .p2align 2
_vector_case_503:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrn s0, d1, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_504
    .p2align 2
_vector_case_504:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrn s0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_505
    .p2align 2
_vector_case_505:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn b0, h1, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_506
    .p2align 2
_vector_case_506:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn h0, s1, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_507
    .p2align 2
_vector_case_507:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn s0, d1, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_508
    .p2align 2
_vector_case_508:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqshrn s0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_509
    .p2align 2
_vector_case_509:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn b0, h1, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_510
    .p2align 2
_vector_case_510:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn h0, s1, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_511
    .p2align 2
_vector_case_511:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn s0, d1, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_512
    .p2align 2
_vector_case_512:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    uqrshrn s0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_513
    .p2align 2
_vector_case_513:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun b0, h1, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_514
    .p2align 2
_vector_case_514:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun h0, s1, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_515
    .p2align 2
_vector_case_515:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun s0, d1, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_516
    .p2align 2
_vector_case_516:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqshrun s0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_517
    .p2align 2
_vector_case_517:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun b0, h1, #3
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_518
    .p2align 2
_vector_case_518:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun h0, s1, #16
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_519
    .p2align 2
_vector_case_519:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun s0, d1, #7
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_520
    .p2align 2
_vector_case_520:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    sqrshrun s0, d1, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_521
    .p2align 2
_vector_case_521:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf v0.4s, v1.4s, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_522
    .p2align 2
_vector_case_522:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf v0.2s, v1.2s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_523
    .p2align 2
_vector_case_523:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf v0.4s, v1.4s, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_524
    .p2align 2
_vector_case_524:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf v0.2d, v1.2d, #6
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_525
    .p2align 2
_vector_case_525:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf v0.2d, v1.2d, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_526
    .p2align 2
_vector_case_526:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf s0, s1, #9
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_527
    .p2align 2
_vector_case_527:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    scvtf d0, d1, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_528
    .p2align 2
_vector_case_528:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf v0.4s, v1.4s, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_529
    .p2align 2
_vector_case_529:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf v0.2s, v1.2s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_530
    .p2align 2
_vector_case_530:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf v0.4s, v1.4s, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_531
    .p2align 2
_vector_case_531:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf v0.2d, v1.2d, #6
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_532
    .p2align 2
_vector_case_532:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf v0.2d, v1.2d, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_533
    .p2align 2
_vector_case_533:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf s0, s1, #9
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_534
    .p2align 2
_vector_case_534:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    ucvtf d0, d1, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_535
    .p2align 2
_vector_case_535:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.4s, v1.4s, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_536
    .p2align 2
_vector_case_536:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.2s, v1.2s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_537
    .p2align 2
_vector_case_537:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.4s, v1.4s, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_538
    .p2align 2
_vector_case_538:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.2d, v1.2d, #6
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_539
    .p2align 2
_vector_case_539:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs v0.2d, v1.2d, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_540
    .p2align 2
_vector_case_540:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs s0, s1, #9
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_541
    .p2align 2
_vector_case_541:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzs d0, d1, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_542
    .p2align 2
_vector_case_542:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.4s, v1.4s, #15
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_543
    .p2align 2
_vector_case_543:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.2s, v1.2s, #1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_544
    .p2align 2
_vector_case_544:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.4s, v1.4s, #32
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_545
    .p2align 2
_vector_case_545:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.2d, v1.2d, #6
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_546
    .p2align 2
_vector_case_546:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu v0.2d, v1.2d, #64
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_547
    .p2align 2
_vector_case_547:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu s0, s1, #9
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_548
    .p2align 2
_vector_case_548:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtzu d0, d1, #40
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_549
    .p2align 2
_vector_case_549:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvt h0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_550
    .p2align 2
_vector_case_550:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvt h0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_551
    .p2align 2
_vector_case_551:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvt s0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_552
    .p2align 2
_vector_case_552:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvt d0, h1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_553
    .p2align 2
_vector_case_553:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtas s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_554
    .p2align 2
_vector_case_554:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtas d0, d1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_555
    .p2align 2
_vector_case_555:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtau s0, s1
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
    .globl _vector_case_556
    .p2align 2
_vector_case_556:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    fcvtau d0, d1
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
    .quad _vector_case_271
    .quad L_name_271
    .quad _vector_case_272
    .quad L_name_272
    .quad _vector_case_273
    .quad L_name_273
    .quad _vector_case_274
    .quad L_name_274
    .quad _vector_case_275
    .quad L_name_275
    .quad _vector_case_276
    .quad L_name_276
    .quad _vector_case_277
    .quad L_name_277
    .quad _vector_case_278
    .quad L_name_278
    .quad _vector_case_279
    .quad L_name_279
    .quad _vector_case_280
    .quad L_name_280
    .quad _vector_case_281
    .quad L_name_281
    .quad _vector_case_282
    .quad L_name_282
    .quad _vector_case_283
    .quad L_name_283
    .quad _vector_case_284
    .quad L_name_284
    .quad _vector_case_285
    .quad L_name_285
    .quad _vector_case_286
    .quad L_name_286
    .quad _vector_case_287
    .quad L_name_287
    .quad _vector_case_288
    .quad L_name_288
    .quad _vector_case_289
    .quad L_name_289
    .quad _vector_case_290
    .quad L_name_290
    .quad _vector_case_291
    .quad L_name_291
    .quad _vector_case_292
    .quad L_name_292
    .quad _vector_case_293
    .quad L_name_293
    .quad _vector_case_294
    .quad L_name_294
    .quad _vector_case_295
    .quad L_name_295
    .quad _vector_case_296
    .quad L_name_296
    .quad _vector_case_297
    .quad L_name_297
    .quad _vector_case_298
    .quad L_name_298
    .quad _vector_case_299
    .quad L_name_299
    .quad _vector_case_300
    .quad L_name_300
    .quad _vector_case_301
    .quad L_name_301
    .quad _vector_case_302
    .quad L_name_302
    .quad _vector_case_303
    .quad L_name_303
    .quad _vector_case_304
    .quad L_name_304
    .quad _vector_case_305
    .quad L_name_305
    .quad _vector_case_306
    .quad L_name_306
    .quad _vector_case_307
    .quad L_name_307
    .quad _vector_case_308
    .quad L_name_308
    .quad _vector_case_309
    .quad L_name_309
    .quad _vector_case_310
    .quad L_name_310
    .quad _vector_case_311
    .quad L_name_311
    .quad _vector_case_312
    .quad L_name_312
    .quad _vector_case_313
    .quad L_name_313
    .quad _vector_case_314
    .quad L_name_314
    .quad _vector_case_315
    .quad L_name_315
    .quad _vector_case_316
    .quad L_name_316
    .quad _vector_case_317
    .quad L_name_317
    .quad _vector_case_318
    .quad L_name_318
    .quad _vector_case_319
    .quad L_name_319
    .quad _vector_case_320
    .quad L_name_320
    .quad _vector_case_321
    .quad L_name_321
    .quad _vector_case_322
    .quad L_name_322
    .quad _vector_case_323
    .quad L_name_323
    .quad _vector_case_324
    .quad L_name_324
    .quad _vector_case_325
    .quad L_name_325
    .quad _vector_case_326
    .quad L_name_326
    .quad _vector_case_327
    .quad L_name_327
    .quad _vector_case_328
    .quad L_name_328
    .quad _vector_case_329
    .quad L_name_329
    .quad _vector_case_330
    .quad L_name_330
    .quad _vector_case_331
    .quad L_name_331
    .quad _vector_case_332
    .quad L_name_332
    .quad _vector_case_333
    .quad L_name_333
    .quad _vector_case_334
    .quad L_name_334
    .quad _vector_case_335
    .quad L_name_335
    .quad _vector_case_336
    .quad L_name_336
    .quad _vector_case_337
    .quad L_name_337
    .quad _vector_case_338
    .quad L_name_338
    .quad _vector_case_339
    .quad L_name_339
    .quad _vector_case_340
    .quad L_name_340
    .quad _vector_case_341
    .quad L_name_341
    .quad _vector_case_342
    .quad L_name_342
    .quad _vector_case_343
    .quad L_name_343
    .quad _vector_case_344
    .quad L_name_344
    .quad _vector_case_345
    .quad L_name_345
    .quad _vector_case_346
    .quad L_name_346
    .quad _vector_case_347
    .quad L_name_347
    .quad _vector_case_348
    .quad L_name_348
    .quad _vector_case_349
    .quad L_name_349
    .quad _vector_case_350
    .quad L_name_350
    .quad _vector_case_351
    .quad L_name_351
    .quad _vector_case_352
    .quad L_name_352
    .quad _vector_case_353
    .quad L_name_353
    .quad _vector_case_354
    .quad L_name_354
    .quad _vector_case_355
    .quad L_name_355
    .quad _vector_case_356
    .quad L_name_356
    .quad _vector_case_357
    .quad L_name_357
    .quad _vector_case_358
    .quad L_name_358
    .quad _vector_case_359
    .quad L_name_359
    .quad _vector_case_360
    .quad L_name_360
    .quad _vector_case_361
    .quad L_name_361
    .quad _vector_case_362
    .quad L_name_362
    .quad _vector_case_363
    .quad L_name_363
    .quad _vector_case_364
    .quad L_name_364
    .quad _vector_case_365
    .quad L_name_365
    .quad _vector_case_366
    .quad L_name_366
    .quad _vector_case_367
    .quad L_name_367
    .quad _vector_case_368
    .quad L_name_368
    .quad _vector_case_369
    .quad L_name_369
    .quad _vector_case_370
    .quad L_name_370
    .quad _vector_case_371
    .quad L_name_371
    .quad _vector_case_372
    .quad L_name_372
    .quad _vector_case_373
    .quad L_name_373
    .quad _vector_case_374
    .quad L_name_374
    .quad _vector_case_375
    .quad L_name_375
    .quad _vector_case_376
    .quad L_name_376
    .quad _vector_case_377
    .quad L_name_377
    .quad _vector_case_378
    .quad L_name_378
    .quad _vector_case_379
    .quad L_name_379
    .quad _vector_case_380
    .quad L_name_380
    .quad _vector_case_381
    .quad L_name_381
    .quad _vector_case_382
    .quad L_name_382
    .quad _vector_case_383
    .quad L_name_383
    .quad _vector_case_384
    .quad L_name_384
    .quad _vector_case_385
    .quad L_name_385
    .quad _vector_case_386
    .quad L_name_386
    .quad _vector_case_387
    .quad L_name_387
    .quad _vector_case_388
    .quad L_name_388
    .quad _vector_case_389
    .quad L_name_389
    .quad _vector_case_390
    .quad L_name_390
    .quad _vector_case_391
    .quad L_name_391
    .quad _vector_case_392
    .quad L_name_392
    .quad _vector_case_393
    .quad L_name_393
    .quad _vector_case_394
    .quad L_name_394
    .quad _vector_case_395
    .quad L_name_395
    .quad _vector_case_396
    .quad L_name_396
    .quad _vector_case_397
    .quad L_name_397
    .quad _vector_case_398
    .quad L_name_398
    .quad _vector_case_399
    .quad L_name_399
    .quad _vector_case_400
    .quad L_name_400
    .quad _vector_case_401
    .quad L_name_401
    .quad _vector_case_402
    .quad L_name_402
    .quad _vector_case_403
    .quad L_name_403
    .quad _vector_case_404
    .quad L_name_404
    .quad _vector_case_405
    .quad L_name_405
    .quad _vector_case_406
    .quad L_name_406
    .quad _vector_case_407
    .quad L_name_407
    .quad _vector_case_408
    .quad L_name_408
    .quad _vector_case_409
    .quad L_name_409
    .quad _vector_case_410
    .quad L_name_410
    .quad _vector_case_411
    .quad L_name_411
    .quad _vector_case_412
    .quad L_name_412
    .quad _vector_case_413
    .quad L_name_413
    .quad _vector_case_414
    .quad L_name_414
    .quad _vector_case_415
    .quad L_name_415
    .quad _vector_case_416
    .quad L_name_416
    .quad _vector_case_417
    .quad L_name_417
    .quad _vector_case_418
    .quad L_name_418
    .quad _vector_case_419
    .quad L_name_419
    .quad _vector_case_420
    .quad L_name_420
    .quad _vector_case_421
    .quad L_name_421
    .quad _vector_case_422
    .quad L_name_422
    .quad _vector_case_423
    .quad L_name_423
    .quad _vector_case_424
    .quad L_name_424
    .quad _vector_case_425
    .quad L_name_425
    .quad _vector_case_426
    .quad L_name_426
    .quad _vector_case_427
    .quad L_name_427
    .quad _vector_case_428
    .quad L_name_428
    .quad _vector_case_429
    .quad L_name_429
    .quad _vector_case_430
    .quad L_name_430
    .quad _vector_case_431
    .quad L_name_431
    .quad _vector_case_432
    .quad L_name_432
    .quad _vector_case_433
    .quad L_name_433
    .quad _vector_case_434
    .quad L_name_434
    .quad _vector_case_435
    .quad L_name_435
    .quad _vector_case_436
    .quad L_name_436
    .quad _vector_case_437
    .quad L_name_437
    .quad _vector_case_438
    .quad L_name_438
    .quad _vector_case_439
    .quad L_name_439
    .quad _vector_case_440
    .quad L_name_440
    .quad _vector_case_441
    .quad L_name_441
    .quad _vector_case_442
    .quad L_name_442
    .quad _vector_case_443
    .quad L_name_443
    .quad _vector_case_444
    .quad L_name_444
    .quad _vector_case_445
    .quad L_name_445
    .quad _vector_case_446
    .quad L_name_446
    .quad _vector_case_447
    .quad L_name_447
    .quad _vector_case_448
    .quad L_name_448
    .quad _vector_case_449
    .quad L_name_449
    .quad _vector_case_450
    .quad L_name_450
    .quad _vector_case_451
    .quad L_name_451
    .quad _vector_case_452
    .quad L_name_452
    .quad _vector_case_453
    .quad L_name_453
    .quad _vector_case_454
    .quad L_name_454
    .quad _vector_case_455
    .quad L_name_455
    .quad _vector_case_456
    .quad L_name_456
    .quad _vector_case_457
    .quad L_name_457
    .quad _vector_case_458
    .quad L_name_458
    .quad _vector_case_459
    .quad L_name_459
    .quad _vector_case_460
    .quad L_name_460
    .quad _vector_case_461
    .quad L_name_461
    .quad _vector_case_462
    .quad L_name_462
    .quad _vector_case_463
    .quad L_name_463
    .quad _vector_case_464
    .quad L_name_464
    .quad _vector_case_465
    .quad L_name_465
    .quad _vector_case_466
    .quad L_name_466
    .quad _vector_case_467
    .quad L_name_467
    .quad _vector_case_468
    .quad L_name_468
    .quad _vector_case_469
    .quad L_name_469
    .quad _vector_case_470
    .quad L_name_470
    .quad _vector_case_471
    .quad L_name_471
    .quad _vector_case_472
    .quad L_name_472
    .quad _vector_case_473
    .quad L_name_473
    .quad _vector_case_474
    .quad L_name_474
    .quad _vector_case_475
    .quad L_name_475
    .quad _vector_case_476
    .quad L_name_476
    .quad _vector_case_477
    .quad L_name_477
    .quad _vector_case_478
    .quad L_name_478
    .quad _vector_case_479
    .quad L_name_479
    .quad _vector_case_480
    .quad L_name_480
    .quad _vector_case_481
    .quad L_name_481
    .quad _vector_case_482
    .quad L_name_482
    .quad _vector_case_483
    .quad L_name_483
    .quad _vector_case_484
    .quad L_name_484
    .quad _vector_case_485
    .quad L_name_485
    .quad _vector_case_486
    .quad L_name_486
    .quad _vector_case_487
    .quad L_name_487
    .quad _vector_case_488
    .quad L_name_488
    .quad _vector_case_489
    .quad L_name_489
    .quad _vector_case_490
    .quad L_name_490
    .quad _vector_case_491
    .quad L_name_491
    .quad _vector_case_492
    .quad L_name_492
    .quad _vector_case_493
    .quad L_name_493
    .quad _vector_case_494
    .quad L_name_494
    .quad _vector_case_495
    .quad L_name_495
    .quad _vector_case_496
    .quad L_name_496
    .quad _vector_case_497
    .quad L_name_497
    .quad _vector_case_498
    .quad L_name_498
    .quad _vector_case_499
    .quad L_name_499
    .quad _vector_case_500
    .quad L_name_500
    .quad _vector_case_501
    .quad L_name_501
    .quad _vector_case_502
    .quad L_name_502
    .quad _vector_case_503
    .quad L_name_503
    .quad _vector_case_504
    .quad L_name_504
    .quad _vector_case_505
    .quad L_name_505
    .quad _vector_case_506
    .quad L_name_506
    .quad _vector_case_507
    .quad L_name_507
    .quad _vector_case_508
    .quad L_name_508
    .quad _vector_case_509
    .quad L_name_509
    .quad _vector_case_510
    .quad L_name_510
    .quad _vector_case_511
    .quad L_name_511
    .quad _vector_case_512
    .quad L_name_512
    .quad _vector_case_513
    .quad L_name_513
    .quad _vector_case_514
    .quad L_name_514
    .quad _vector_case_515
    .quad L_name_515
    .quad _vector_case_516
    .quad L_name_516
    .quad _vector_case_517
    .quad L_name_517
    .quad _vector_case_518
    .quad L_name_518
    .quad _vector_case_519
    .quad L_name_519
    .quad _vector_case_520
    .quad L_name_520
    .quad _vector_case_521
    .quad L_name_521
    .quad _vector_case_522
    .quad L_name_522
    .quad _vector_case_523
    .quad L_name_523
    .quad _vector_case_524
    .quad L_name_524
    .quad _vector_case_525
    .quad L_name_525
    .quad _vector_case_526
    .quad L_name_526
    .quad _vector_case_527
    .quad L_name_527
    .quad _vector_case_528
    .quad L_name_528
    .quad _vector_case_529
    .quad L_name_529
    .quad _vector_case_530
    .quad L_name_530
    .quad _vector_case_531
    .quad L_name_531
    .quad _vector_case_532
    .quad L_name_532
    .quad _vector_case_533
    .quad L_name_533
    .quad _vector_case_534
    .quad L_name_534
    .quad _vector_case_535
    .quad L_name_535
    .quad _vector_case_536
    .quad L_name_536
    .quad _vector_case_537
    .quad L_name_537
    .quad _vector_case_538
    .quad L_name_538
    .quad _vector_case_539
    .quad L_name_539
    .quad _vector_case_540
    .quad L_name_540
    .quad _vector_case_541
    .quad L_name_541
    .quad _vector_case_542
    .quad L_name_542
    .quad _vector_case_543
    .quad L_name_543
    .quad _vector_case_544
    .quad L_name_544
    .quad _vector_case_545
    .quad L_name_545
    .quad _vector_case_546
    .quad L_name_546
    .quad _vector_case_547
    .quad L_name_547
    .quad _vector_case_548
    .quad L_name_548
    .quad _vector_case_549
    .quad L_name_549
    .quad _vector_case_550
    .quad L_name_550
    .quad _vector_case_551
    .quad L_name_551
    .quad _vector_case_552
    .quad L_name_552
    .quad _vector_case_553
    .quad L_name_553
    .quad _vector_case_554
    .quad L_name_554
    .quad _vector_case_555
    .quad L_name_555
    .quad _vector_case_556
    .quad L_name_556
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
L_name_271: .asciz "frintn v0.4s, v1.4s"
L_name_272: .asciz "frintn v0.2d, v1.2d"
L_name_273: .asciz "frintn v0.2s, v1.2s"
L_name_274: .asciz "frintm v0.4s, v1.4s"
L_name_275: .asciz "frintm v0.2d, v1.2d"
L_name_276: .asciz "frintm v0.2s, v1.2s"
L_name_277: .asciz "frintp v0.4s, v1.4s"
L_name_278: .asciz "frintp v0.2d, v1.2d"
L_name_279: .asciz "frintp v0.2s, v1.2s"
L_name_280: .asciz "frintz v0.4s, v1.4s"
L_name_281: .asciz "frintz v0.2d, v1.2d"
L_name_282: .asciz "frintz v0.2s, v1.2s"
L_name_283: .asciz "frinta v0.4s, v1.4s"
L_name_284: .asciz "frinta v0.2d, v1.2d"
L_name_285: .asciz "frinta v0.2s, v1.2s"
L_name_286: .asciz "frintx v0.4s, v1.4s"
L_name_287: .asciz "frintx v0.2d, v1.2d"
L_name_288: .asciz "frintx v0.2s, v1.2s"
L_name_289: .asciz "frinti v0.4s, v1.4s"
L_name_290: .asciz "frinti v0.2d, v1.2d"
L_name_291: .asciz "frinti v0.2s, v1.2s"
L_name_292: .asciz "frecpe v0.4s, v1.4s"
L_name_293: .asciz "frecpe v0.2d, v1.2d"
L_name_294: .asciz "frecpe v0.2s, v1.2s"
L_name_295: .asciz "frsqrte v0.4s, v1.4s"
L_name_296: .asciz "frsqrte v0.2d, v1.2d"
L_name_297: .asciz "frsqrte v0.2s, v1.2s"
L_name_298: .asciz "fcvtns v0.4s, v1.4s"
L_name_299: .asciz "fcvtns v0.2d, v1.2d"
L_name_300: .asciz "fcvtns v0.2s, v1.2s"
L_name_301: .asciz "fcvtms v0.4s, v1.4s"
L_name_302: .asciz "fcvtms v0.2d, v1.2d"
L_name_303: .asciz "fcvtms v0.2s, v1.2s"
L_name_304: .asciz "fcvtps v0.4s, v1.4s"
L_name_305: .asciz "fcvtps v0.2d, v1.2d"
L_name_306: .asciz "fcvtps v0.2s, v1.2s"
L_name_307: .asciz "fcvtzs v0.4s, v1.4s"
L_name_308: .asciz "fcvtzs v0.2d, v1.2d"
L_name_309: .asciz "fcvtzs v0.2s, v1.2s"
L_name_310: .asciz "fcvtnu v0.4s, v1.4s"
L_name_311: .asciz "fcvtnu v0.2d, v1.2d"
L_name_312: .asciz "fcvtnu v0.2s, v1.2s"
L_name_313: .asciz "fcvtmu v0.4s, v1.4s"
L_name_314: .asciz "fcvtmu v0.2d, v1.2d"
L_name_315: .asciz "fcvtmu v0.2s, v1.2s"
L_name_316: .asciz "fcvtpu v0.4s, v1.4s"
L_name_317: .asciz "fcvtpu v0.2d, v1.2d"
L_name_318: .asciz "fcvtpu v0.2s, v1.2s"
L_name_319: .asciz "fcvtzu v0.4s, v1.4s"
L_name_320: .asciz "fcvtzu v0.2d, v1.2d"
L_name_321: .asciz "fcvtzu v0.2s, v1.2s"
L_name_322: .asciz "frecpe s0, s1"
L_name_323: .asciz "frecpe d0, d1"
L_name_324: .asciz "frsqrte s0, s1"
L_name_325: .asciz "frsqrte d0, d1"
L_name_326: .asciz "fcvtns s0, s1"
L_name_327: .asciz "fcvtns d0, d1"
L_name_328: .asciz "fcvtms s0, s1"
L_name_329: .asciz "fcvtms d0, d1"
L_name_330: .asciz "fcvtps s0, s1"
L_name_331: .asciz "fcvtps d0, d1"
L_name_332: .asciz "fcvtzs s0, s1"
L_name_333: .asciz "fcvtzs d0, d1"
L_name_334: .asciz "fcvtnu s0, s1"
L_name_335: .asciz "fcvtnu d0, d1"
L_name_336: .asciz "fcvtmu s0, s1"
L_name_337: .asciz "fcvtmu d0, d1"
L_name_338: .asciz "fcvtpu s0, s1"
L_name_339: .asciz "fcvtpu d0, d1"
L_name_340: .asciz "fcvtzu s0, s1"
L_name_341: .asciz "fcvtzu d0, d1"
L_name_342: .asciz "fcmgt s0, s1, #0.0"
L_name_343: .asciz "fcmgt d0, d1, #0.0"
L_name_344: .asciz "fcmeq s0, s1, #0.0"
L_name_345: .asciz "fcmeq d0, d1, #0.0"
L_name_346: .asciz "fcmlt s0, s1, #0.0"
L_name_347: .asciz "fcmlt d0, d1, #0.0"
L_name_348: .asciz "fcmge s0, s1, #0.0"
L_name_349: .asciz "fcmge d0, d1, #0.0"
L_name_350: .asciz "fcmle s0, s1, #0.0"
L_name_351: .asciz "fcmle d0, d1, #0.0"
L_name_352: .asciz "frecps v0.4s, v1.4s, v2.4s"
L_name_353: .asciz "frecps v0.2d, v1.2d, v2.2d"
L_name_354: .asciz "frecps v0.2s, v1.2s, v2.2s"
L_name_355: .asciz "frecps s0, s1, s2"
L_name_356: .asciz "frecps d0, d1, d2"
L_name_357: .asciz "frsqrts v0.4s, v1.4s, v2.4s"
L_name_358: .asciz "frsqrts v0.2d, v1.2d, v2.2d"
L_name_359: .asciz "frsqrts v0.2s, v1.2s, v2.2s"
L_name_360: .asciz "frsqrts s0, s1, s2"
L_name_361: .asciz "frsqrts d0, d1, d2"
L_name_362: .asciz "fmulx v0.4s, v1.4s, v2.4s"
L_name_363: .asciz "fmulx v0.2d, v1.2d, v2.2d"
L_name_364: .asciz "fmulx v0.2s, v1.2s, v2.2s"
L_name_365: .asciz "fmulx s0, s1, s2"
L_name_366: .asciz "fmulx d0, d1, d2"
L_name_367: .asciz "facge v0.4s, v1.4s, v2.4s"
L_name_368: .asciz "facge v0.2d, v1.2d, v2.2d"
L_name_369: .asciz "facge v0.2s, v1.2s, v2.2s"
L_name_370: .asciz "facge s0, s1, s2"
L_name_371: .asciz "facge d0, d1, d2"
L_name_372: .asciz "facgt v0.4s, v1.4s, v2.4s"
L_name_373: .asciz "facgt v0.2d, v1.2d, v2.2d"
L_name_374: .asciz "facgt v0.2s, v1.2s, v2.2s"
L_name_375: .asciz "facgt s0, s1, s2"
L_name_376: .asciz "facgt d0, d1, d2"
L_name_377: .asciz "fcmeq v0.4s, v1.4s, v2.4s"
L_name_378: .asciz "fcmeq v0.2d, v1.2d, v2.2d"
L_name_379: .asciz "fcmeq v0.2s, v1.2s, v2.2s"
L_name_380: .asciz "fcmeq s0, s1, s2"
L_name_381: .asciz "fcmeq d0, d1, d2"
L_name_382: .asciz "fcmge v0.4s, v1.4s, v2.4s"
L_name_383: .asciz "fcmge v0.2d, v1.2d, v2.2d"
L_name_384: .asciz "fcmge v0.2s, v1.2s, v2.2s"
L_name_385: .asciz "fcmge s0, s1, s2"
L_name_386: .asciz "fcmge d0, d1, d2"
L_name_387: .asciz "fcmgt v0.4s, v1.4s, v2.4s"
L_name_388: .asciz "fcmgt v0.2d, v1.2d, v2.2d"
L_name_389: .asciz "fcmgt v0.2s, v1.2s, v2.2s"
L_name_390: .asciz "fcmgt s0, s1, s2"
L_name_391: .asciz "fcmgt d0, d1, d2"
L_name_392: .asciz "fmaxp v0.4s, v1.4s, v2.4s"
L_name_393: .asciz "fmaxp v0.2d, v1.2d, v2.2d"
L_name_394: .asciz "fmaxp v0.2s, v1.2s, v2.2s"
L_name_395: .asciz "fmaxp s0, v1.2s"
L_name_396: .asciz "fmaxp d0, v1.2d"
L_name_397: .asciz "fminp v0.4s, v1.4s, v2.4s"
L_name_398: .asciz "fminp v0.2d, v1.2d, v2.2d"
L_name_399: .asciz "fminp v0.2s, v1.2s, v2.2s"
L_name_400: .asciz "fminp s0, v1.2s"
L_name_401: .asciz "fminp d0, v1.2d"
L_name_402: .asciz "fmaxnmp v0.4s, v1.4s, v2.4s"
L_name_403: .asciz "fmaxnmp v0.2d, v1.2d, v2.2d"
L_name_404: .asciz "fmaxnmp v0.2s, v1.2s, v2.2s"
L_name_405: .asciz "fmaxnmp s0, v1.2s"
L_name_406: .asciz "fmaxnmp d0, v1.2d"
L_name_407: .asciz "fminnmp v0.4s, v1.4s, v2.4s"
L_name_408: .asciz "fminnmp v0.2d, v1.2d, v2.2d"
L_name_409: .asciz "fminnmp v0.2s, v1.2s, v2.2s"
L_name_410: .asciz "fminnmp s0, v1.2s"
L_name_411: .asciz "fminnmp d0, v1.2d"
L_name_412: .asciz "fmaxnmv s0, v1.4s"
L_name_413: .asciz "fminnmv s0, v1.4s"
L_name_414: .asciz "fmaxv s0, v1.4s"
L_name_415: .asciz "fminv s0, v1.4s"
L_name_416: .asciz "sabal v0.8h, v1.8b, v2.8b"
L_name_417: .asciz "sabal v0.4s, v1.4h, v2.4h"
L_name_418: .asciz "sabal v0.2d, v1.2s, v2.2s"
L_name_419: .asciz "sabal2 v0.8h, v1.16b, v2.16b"
L_name_420: .asciz "sabal2 v0.4s, v1.8h, v2.8h"
L_name_421: .asciz "sabal2 v0.2d, v1.4s, v2.4s"
L_name_422: .asciz "uabal v0.8h, v1.8b, v2.8b"
L_name_423: .asciz "uabal v0.4s, v1.4h, v2.4h"
L_name_424: .asciz "uabal v0.2d, v1.2s, v2.2s"
L_name_425: .asciz "uabal2 v0.8h, v1.16b, v2.16b"
L_name_426: .asciz "uabal2 v0.4s, v1.8h, v2.8h"
L_name_427: .asciz "uabal2 v0.2d, v1.4s, v2.4s"
L_name_428: .asciz "saba v0.16b, v1.16b, v2.16b"
L_name_429: .asciz "saba v0.8h, v1.8h, v2.8h"
L_name_430: .asciz "saba v0.4s, v1.4s, v2.4s"
L_name_431: .asciz "saba v0.8b, v1.8b, v2.8b"
L_name_432: .asciz "saba v0.4h, v1.4h, v2.4h"
L_name_433: .asciz "saba v0.2s, v1.2s, v2.2s"
L_name_434: .asciz "uaba v0.16b, v1.16b, v2.16b"
L_name_435: .asciz "uaba v0.8h, v1.8h, v2.8h"
L_name_436: .asciz "uaba v0.4s, v1.4s, v2.4s"
L_name_437: .asciz "uaba v0.8b, v1.8b, v2.8b"
L_name_438: .asciz "uaba v0.4h, v1.4h, v2.4h"
L_name_439: .asciz "uaba v0.2s, v1.2s, v2.2s"
L_name_440: .asciz "smaxp v0.16b, v1.16b, v2.16b"
L_name_441: .asciz "smaxp v0.8h, v1.8h, v2.8h"
L_name_442: .asciz "smaxp v0.4s, v1.4s, v2.4s"
L_name_443: .asciz "smaxp v0.8b, v1.8b, v2.8b"
L_name_444: .asciz "smaxp v0.4h, v1.4h, v2.4h"
L_name_445: .asciz "smaxp v0.2s, v1.2s, v2.2s"
L_name_446: .asciz "umaxp v0.16b, v1.16b, v2.16b"
L_name_447: .asciz "umaxp v0.8h, v1.8h, v2.8h"
L_name_448: .asciz "umaxp v0.4s, v1.4s, v2.4s"
L_name_449: .asciz "umaxp v0.8b, v1.8b, v2.8b"
L_name_450: .asciz "umaxp v0.4h, v1.4h, v2.4h"
L_name_451: .asciz "umaxp v0.2s, v1.2s, v2.2s"
L_name_452: .asciz "sminp v0.16b, v1.16b, v2.16b"
L_name_453: .asciz "sminp v0.8h, v1.8h, v2.8h"
L_name_454: .asciz "sminp v0.4s, v1.4s, v2.4s"
L_name_455: .asciz "sminp v0.8b, v1.8b, v2.8b"
L_name_456: .asciz "sminp v0.4h, v1.4h, v2.4h"
L_name_457: .asciz "sminp v0.2s, v1.2s, v2.2s"
L_name_458: .asciz "uminp v0.16b, v1.16b, v2.16b"
L_name_459: .asciz "uminp v0.8h, v1.8h, v2.8h"
L_name_460: .asciz "uminp v0.4s, v1.4s, v2.4s"
L_name_461: .asciz "uminp v0.8b, v1.8b, v2.8b"
L_name_462: .asciz "uminp v0.4h, v1.4h, v2.4h"
L_name_463: .asciz "uminp v0.2s, v1.2s, v2.2s"
L_name_464: .asciz "sqadd b0, b1, b2"
L_name_465: .asciz "sqadd h0, h1, h2"
L_name_466: .asciz "sqadd s0, s1, s2"
L_name_467: .asciz "sqadd d0, d1, d2"
L_name_468: .asciz "uqadd b0, b1, b2"
L_name_469: .asciz "uqadd h0, h1, h2"
L_name_470: .asciz "uqadd s0, s1, s2"
L_name_471: .asciz "uqadd d0, d1, d2"
L_name_472: .asciz "sqsub b0, b1, b2"
L_name_473: .asciz "sqsub h0, h1, h2"
L_name_474: .asciz "sqsub s0, s1, s2"
L_name_475: .asciz "sqsub d0, d1, d2"
L_name_476: .asciz "uqsub b0, b1, b2"
L_name_477: .asciz "uqsub h0, h1, h2"
L_name_478: .asciz "uqsub s0, s1, s2"
L_name_479: .asciz "uqsub d0, d1, d2"
L_name_480: .asciz "sqabs b0, b1"
L_name_481: .asciz "sqabs h0, h1"
L_name_482: .asciz "sqabs s0, s1"
L_name_483: .asciz "sqabs d0, d1"
L_name_484: .asciz "sqneg b0, b1"
L_name_485: .asciz "sqneg h0, h1"
L_name_486: .asciz "sqneg s0, s1"
L_name_487: .asciz "sqneg d0, d1"
L_name_488: .asciz "sqxtn b0, h1"
L_name_489: .asciz "sqxtn h0, s1"
L_name_490: .asciz "sqxtn s0, d1"
L_name_491: .asciz "uqxtn b0, h1"
L_name_492: .asciz "uqxtn h0, s1"
L_name_493: .asciz "uqxtn s0, d1"
L_name_494: .asciz "sqxtun b0, h1"
L_name_495: .asciz "sqxtun h0, s1"
L_name_496: .asciz "sqxtun s0, d1"
L_name_497: .asciz "sqshrn b0, h1, #3"
L_name_498: .asciz "sqshrn h0, s1, #16"
L_name_499: .asciz "sqshrn s0, d1, #7"
L_name_500: .asciz "sqshrn s0, d1, #32"
L_name_501: .asciz "sqrshrn b0, h1, #3"
L_name_502: .asciz "sqrshrn h0, s1, #16"
L_name_503: .asciz "sqrshrn s0, d1, #7"
L_name_504: .asciz "sqrshrn s0, d1, #32"
L_name_505: .asciz "uqshrn b0, h1, #3"
L_name_506: .asciz "uqshrn h0, s1, #16"
L_name_507: .asciz "uqshrn s0, d1, #7"
L_name_508: .asciz "uqshrn s0, d1, #32"
L_name_509: .asciz "uqrshrn b0, h1, #3"
L_name_510: .asciz "uqrshrn h0, s1, #16"
L_name_511: .asciz "uqrshrn s0, d1, #7"
L_name_512: .asciz "uqrshrn s0, d1, #32"
L_name_513: .asciz "sqshrun b0, h1, #3"
L_name_514: .asciz "sqshrun h0, s1, #16"
L_name_515: .asciz "sqshrun s0, d1, #7"
L_name_516: .asciz "sqshrun s0, d1, #32"
L_name_517: .asciz "sqrshrun b0, h1, #3"
L_name_518: .asciz "sqrshrun h0, s1, #16"
L_name_519: .asciz "sqrshrun s0, d1, #7"
L_name_520: .asciz "sqrshrun s0, d1, #32"
L_name_521: .asciz "scvtf v0.4s, v1.4s, #15"
L_name_522: .asciz "scvtf v0.2s, v1.2s, #1"
L_name_523: .asciz "scvtf v0.4s, v1.4s, #32"
L_name_524: .asciz "scvtf v0.2d, v1.2d, #6"
L_name_525: .asciz "scvtf v0.2d, v1.2d, #64"
L_name_526: .asciz "scvtf s0, s1, #9"
L_name_527: .asciz "scvtf d0, d1, #40"
L_name_528: .asciz "ucvtf v0.4s, v1.4s, #15"
L_name_529: .asciz "ucvtf v0.2s, v1.2s, #1"
L_name_530: .asciz "ucvtf v0.4s, v1.4s, #32"
L_name_531: .asciz "ucvtf v0.2d, v1.2d, #6"
L_name_532: .asciz "ucvtf v0.2d, v1.2d, #64"
L_name_533: .asciz "ucvtf s0, s1, #9"
L_name_534: .asciz "ucvtf d0, d1, #40"
L_name_535: .asciz "fcvtzs v0.4s, v1.4s, #15"
L_name_536: .asciz "fcvtzs v0.2s, v1.2s, #1"
L_name_537: .asciz "fcvtzs v0.4s, v1.4s, #32"
L_name_538: .asciz "fcvtzs v0.2d, v1.2d, #6"
L_name_539: .asciz "fcvtzs v0.2d, v1.2d, #64"
L_name_540: .asciz "fcvtzs s0, s1, #9"
L_name_541: .asciz "fcvtzs d0, d1, #40"
L_name_542: .asciz "fcvtzu v0.4s, v1.4s, #15"
L_name_543: .asciz "fcvtzu v0.2s, v1.2s, #1"
L_name_544: .asciz "fcvtzu v0.4s, v1.4s, #32"
L_name_545: .asciz "fcvtzu v0.2d, v1.2d, #6"
L_name_546: .asciz "fcvtzu v0.2d, v1.2d, #64"
L_name_547: .asciz "fcvtzu s0, s1, #9"
L_name_548: .asciz "fcvtzu d0, d1, #40"
L_name_549: .asciz "fcvt h0, s1"
L_name_550: .asciz "fcvt h0, d1"
L_name_551: .asciz "fcvt s0, h1"
L_name_552: .asciz "fcvt d0, h1"
L_name_553: .asciz "fcvtas s0, s1"
L_name_554: .asciz "fcvtas d0, d1"
L_name_555: .asciz "fcvtau s0, s1"
L_name_556: .asciz "fcvtau d0, d1"
