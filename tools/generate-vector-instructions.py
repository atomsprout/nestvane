#!/usr/bin/env python3
"""Writes tools/vector-instructions.S: one small function for each instruction that
tools/validate-vector-instructions.cpp runs on the processor and in the interpreter.

Every instruction takes v0 as its destination, which is also an input where the instruction reads its
destination, and v1 and v2 as its operands. Run this again after adding to the list."""
import os

cases = []
def add(*lines): cases.extend(lines)

for op in ('sqadd', 'uqadd', 'sqsub', 'uqsub'):
    for t in ('16b', '8h', '4s', '2d', '8b', '2s'): add(f'{op} v0.{t}, v1.{t}, v2.{t}')
for op in ('shadd', 'uhadd', 'srhadd', 'urhadd', 'shsub', 'uhsub'):
    for t in ('16b', '8h', '4s', '8b'): add(f'{op} v0.{t}, v1.{t}, v2.{t}')
for op in ('srshl', 'urshl', 'sqshl', 'uqshl', 'sqrshl', 'uqrshl'):
    for t in ('16b', '8h', '4s', '2d', '4h'): add(f'{op} v0.{t}, v1.{t}, v2.{t}')
for op in ('sqdmulh', 'sqrdmulh', 'sqrdmlah', 'sqrdmlsh'):
    for t in ('8h', '4s', '4h', '2s'): add(f'{op} v0.{t}, v1.{t}, v2.{t}')
add('rev16 v0.16b, v1.16b', 'rev16 v0.8b, v1.8b')
for op in ('suqadd', 'usqadd', 'sqabs', 'sqneg'):
    for t in ('16b', '8h', '4s', '2d', '8b'): add(f'{op} v0.{t}, v1.{t}')
for op in ('sqxtn', 'uqxtn', 'sqxtun'):
    for d, s in (('8b', '8h'), ('4h', '4s'), ('2s', '2d')): add(f'{op} v0.{d}, v1.{s}')
    for d, s in (('16b', '8h'), ('8h', '4s'), ('4s', '2d')): add(f'{op}2 v0.{d}, v1.{s}')
add('fcvtn v0.2s, v1.2d', 'fcvtn v0.4h, v1.4s', 'fcvtn2 v0.4s, v1.2d', 'fcvtn2 v0.8h, v1.4s')
for op in ('fcvtas', 'fcvtau'):
    add(f'{op} v0.4s, v1.4s', f'{op} v0.2d, v1.2d', f'{op} v0.2s, v1.2s')
for op in ('sqshrun', 'sqrshrun', 'uqshrn', 'uqrshrn'):
    add(f'{op} v0.8b, v1.8h, #3', f'{op} v0.4h, v1.4s, #16', f'{op} v0.2s, v1.2d, #7',
        f'{op}2 v0.16b, v1.8h, #8', f'{op}2 v0.8h, v1.4s, #1', f'{op}2 v0.4s, v1.2d, #32')
for op in ('sqshl', 'uqshl', 'sqshlu'):
    add(f'{op} v0.16b, v1.16b, #3', f'{op} v0.8h, v1.8h, #15', f'{op} v0.4s, v1.4s, #0',
        f'{op} v0.2d, v1.2d, #40', f'{op} v0.4h, v1.4h, #7')
add('uaddlv h0, v1.16b', 'uaddlv h0, v1.8b', 'uaddlv s0, v1.8h', 'uaddlv s0, v1.4h', 'uaddlv d0, v1.4s',
    'saddlv h0, v1.16b', 'saddlv s0, v1.8h', 'saddlv d0, v1.4s')
add('add d0, d1, d2', 'sub d0, d1, d2')
add('sri v0.4s, v1.4s, #20', 'sri v0.16b, v1.16b, #3', 'sri v0.2d, v1.2d, #64', 'sri v0.8h, v1.8h, #16',
    'sri v0.2s, v1.2s, #1', 'sli v0.4s, v1.4s, #12')
# AES, SHA-1, SHA-512 and SHA-3.
add('aese v0.16b, v1.16b', 'aesd v0.16b, v1.16b', 'aesmc v0.16b, v1.16b', 'aesimc v0.16b, v1.16b')
add('sha1c q0, s1, v2.4s', 'sha1p q0, s1, v2.4s', 'sha1m q0, s1, v2.4s', 'sha1h s0, s1',
    'sha1su0 v0.4s, v1.4s, v2.4s', 'sha1su1 v0.4s, v1.4s')
add('sha512h q0, q1, v2.2d', 'sha512h2 q0, q1, v2.2d', 'sha512su0 v0.2d, v1.2d', 'sha512su1 v0.2d, v1.2d, v2.2d')
add('eor3 v0.16b, v1.16b, v2.16b, v0.16b', 'bcax v0.16b, v1.16b, v2.16b, v0.16b', 'rax1 v0.2d, v1.2d, v2.2d',
    'xar v0.2d, v1.2d, v2.2d, #17', 'xar v0.2d, v1.2d, v2.2d, #0', 'xar v0.2d, v1.2d, v2.2d, #63')
# One D register shifted by a number.
for op in ('sshr', 'ushr', 'ssra', 'usra', 'srshr', 'urshr', 'srsra', 'ursra', 'sri'):
    add(f'{op} d0, d1, #1', f'{op} d0, d1, #33', f'{op} d0, d1, #64')
for op in ('shl', 'sli'):
    add(f'{op} d0, d1, #0', f'{op} d0, d1, #32', f'{op} d0, d1, #63')
for op in ('sqrdmlah', 'sqrdmlsh'):
    add(f'{op} v0.4s, v1.4s, v2.s[0]', f'{op} v0.4s, v1.4s, v2.s[3]', f'{op} v0.8h, v1.8h, v2.h[5]',
        f'{op} v0.4h, v1.4h, v2.h[7]', f'{op} v0.2s, v1.2s, v2.s[1]')
add('fcvtl v0.4s, v1.4h', 'fcvtl2 v0.4s, v1.8h')
add('faddp v0.4s, v1.4s, v2.4s', 'faddp v0.2s, v1.2s, v2.2s', 'faddp v0.2d, v1.2d, v2.2d')
add('sshl d0, d1, d2', 'ushl d0, d1, d2', 'rbit v0.16b, v1.16b', 'rbit v0.8b, v1.8b')

# The third lot (laterVector).
for op in ('frintn', 'frintm', 'frintp', 'frintz', 'frinta', 'frintx', 'frinti', 'frecpe', 'frsqrte',
           'fcvtns', 'fcvtms', 'fcvtps', 'fcvtzs', 'fcvtnu', 'fcvtmu', 'fcvtpu', 'fcvtzu'):
    add(f'{op} v0.4s, v1.4s', f'{op} v0.2d, v1.2d', f'{op} v0.2s, v1.2s')
for op in ('frecpe', 'frsqrte', 'fcvtns', 'fcvtms', 'fcvtps', 'fcvtzs', 'fcvtnu', 'fcvtmu', 'fcvtpu', 'fcvtzu'):
    add(f'{op} s0, s1', f'{op} d0, d1')
for op in ('fcmgt', 'fcmeq', 'fcmlt', 'fcmge', 'fcmle'):
    add(f'{op} s0, s1, #0.0', f'{op} d0, d1, #0.0')
for op in ('frecps', 'frsqrts', 'fmulx', 'facge', 'facgt', 'fcmeq', 'fcmge', 'fcmgt'):
    add(f'{op} v0.4s, v1.4s, v2.4s', f'{op} v0.2d, v1.2d, v2.2d', f'{op} v0.2s, v1.2s, v2.2s',
        f'{op} s0, s1, s2', f'{op} d0, d1, d2')
for op in ('fmaxp', 'fminp', 'fmaxnmp', 'fminnmp'):
    add(f'{op} v0.4s, v1.4s, v2.4s', f'{op} v0.2d, v1.2d, v2.2d', f'{op} v0.2s, v1.2s, v2.2s',
        f'{op} s0, v1.2s', f'{op} d0, v1.2d')
for op in ('fmaxnmv', 'fminnmv', 'fmaxv', 'fminv'): add(f'{op} s0, v1.4s')
for op in ('sabal', 'uabal'):
    add(f'{op} v0.8h, v1.8b, v2.8b', f'{op} v0.4s, v1.4h, v2.4h', f'{op} v0.2d, v1.2s, v2.2s',
        f'{op}2 v0.8h, v1.16b, v2.16b', f'{op}2 v0.4s, v1.8h, v2.8h', f'{op}2 v0.2d, v1.4s, v2.4s')
for op in ('saba', 'uaba', 'smaxp', 'umaxp', 'sminp', 'uminp'):
    for t in ('16b', '8h', '4s', '8b', '4h', '2s'): add(f'{op} v0.{t}, v1.{t}, v2.{t}')
for op in ('sqadd', 'uqadd', 'sqsub', 'uqsub'):
    for r in 'bhsd': add(f'{op} {r}0, {r}1, {r}2')
for op in ('sqabs', 'sqneg'):
    for r in 'bhsd': add(f'{op} {r}0, {r}1')
for op in ('sqxtn', 'uqxtn', 'sqxtun'):
    add(f'{op} b0, h1', f'{op} h0, s1', f'{op} s0, d1')
for op in ('sqshrn', 'sqrshrn', 'uqshrn', 'uqrshrn', 'sqshrun', 'sqrshrun'):
    add(f'{op} b0, h1, #3', f'{op} h0, s1, #16', f'{op} s0, d1, #7', f'{op} s0, d1, #32')
for op in ('scvtf', 'ucvtf', 'fcvtzs', 'fcvtzu'):
    add(f'{op} v0.4s, v1.4s, #15', f'{op} v0.2s, v1.2s, #1', f'{op} v0.4s, v1.4s, #32', f'{op} v0.2d, v1.2d, #6',
        f'{op} v0.2d, v1.2d, #64', f'{op} s0, s1, #9', f'{op} d0, d1, #40')
add('fcvt h0, s1', 'fcvt h0, d1', 'fcvt s0, h1', 'fcvt d0, h1')
add('fcvtas s0, s1', 'fcvtas d0, d1', 'fcvtau s0, s1', 'fcvtau d0, d1')

out = ['''// Written by tools/generate-vector-instructions.py: do not edit.
// One small function for each instruction, which runs it on the processor. x0 points at three
// 16-byte vectors and a 64-bit word: v0, v1 and v2 go in, v0 and the status register come out.
// The instruction is the fourth word of its function.
    .arch armv8.5-a+crypto+sha3
    .text
''']
for i, c in enumerate(cases):
    out.append(f'''    .globl _vector_case_{i}
    .p2align 2
_vector_case_{i}:
    ldp q0, q1, [x0]
    ldr q2, [x0, #32]
    msr fpsr, xzr
    {c}
    str q0, [x0]
    mrs x1, fpsr
    str x1, [x0, #48]
    ret
''')
out.append('    .section __DATA,__const\n    .globl _vector_cases\n    .p2align 3\n_vector_cases:\n')
for i, c in enumerate(cases):
    out.append(f'    .quad _vector_case_{i}\n    .quad L_name_{i}\n')
out.append('    .quad 0\n    .quad 0\n    .section __TEXT,__cstring\n')
for i, c in enumerate(cases):
    out.append(f'L_name_{i}: .asciz "{c}"\n')
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vector-instructions.S')
open(path, 'w').write(''.join(out))
print(len(cases), 'cases')
