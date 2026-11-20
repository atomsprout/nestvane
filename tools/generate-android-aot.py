#!/usr/bin/env python3
"""Compile straight-line arithmetic from the pinned Android BoringSSL SHA-256 routine."""

import argparse
import hashlib
import struct
from pathlib import Path

# Android 17 SDK default ARM64 image CE2A.260420.051.A3.
LIBCRYPTO_SHA256 = "6cb85e255d51ed24b3ef276e60bc84c63c242511ed31db2a8222e8a881d94357"
SHA256_START = 0x125DC0
SHA256_END = 0x126BE8
MIN_BLOCK = 4
MAX_BLOCK = 32


def register(index):
    return f"uint32_t(x[{index}])" if index != 31 else "0u"


def signed_right_shift(operand, amount):
    return f"uint32_t(std::bit_cast<int32_t>({operand}) >> {amount})"


def shift(operand, kind, amount):
    if amount == 0:
        return operand
    if kind == 0:
        return f"({operand} << {amount})"
    if kind == 1:
        return f"({operand} >> {amount})"
    if kind == 2:
        return signed_right_shift(operand, amount)
    return f"std::rotr({operand}, {amount})"


def translate(word):
    rd, rn, rm = word & 31, (word >> 5) & 31, (word >> 16) & 31
    kind, amount = (word >> 22) & 3, (word >> 10) & 63
    if rd == 31 or amount >= 32:
        return None
    left, right = register(rn), register(rm)

    if word & 0xFFE00000 == 0x13800000 and rn == rm:
        return f"x[{rd}] = std::rotr({left}, {amount});"
    if word & 0xFFFFFC00 == 0x5AC00800:
        return f"x[{rd}] = __builtin_bswap32({left});"
    opcode = word & 0xFF200000
    operators = {0x0B000000: "+", 0x0A000000: "&", 0x0A200000: "&",
                 0x2A000000: "|", 0x4A000000: "^"}
    if opcode not in operators or (opcode == 0x0B000000 and kind == 3):
        return None
    right = shift(right, kind, amount)
    if opcode == 0x0A200000:
        right = f"~{right}"
    return f"x[{rd}] = {left} {operators[opcode]} {right};"


def collect(data):
    if hashlib.sha256(data).hexdigest() != LIBCRYPTO_SHA256:
        return []
    blocks = []
    words, code = [], []
    start = SHA256_START
    for offset in range(SHA256_START, SHA256_END, 4):
        word = struct.unpack_from("<I", data, offset)[0]
        instruction = translate(word)
        if instruction is None or len(words) == MAX_BLOCK or offset % 4096 == 0:
            if len(words) >= MIN_BLOCK:
                blocks.append((start, words, code))
            words, code = [], []
        if instruction is not None:
            if not words:
                start = offset
            words.append(word)
            code.append(instruction)
    if len(words) >= MIN_BLOCK:
        blocks.append((start, words, code))
    return blocks


def generate(blocks):
    lines = ["// Generated from the packaged Android ELF. Do not edit.", "namespace nestvane {"]
    for start, words, code in blocks:
        name = f"sha256Block_{start:x}"
        lines.append(f"inline constexpr std::array<uint32_t, {len(words)}> {name}Words{{")
        for index in range(0, len(words), 6):
            group = (f"0x{word:08x}" for word in words[index:index + 6])
            lines.append("    " + ", ".join(group) + ",")
        lines.extend(["};", f"inline void {name}(CPU& cpu) {{", "    auto& x = cpu.x;"])
        lines.extend(f"    {instruction}" for instruction in code)
        lines.append("}")
    lines.append(f"inline constexpr std::array<AndroidBootAotBlock, {len(blocks)}> androidBootGeneratedBlocks{{{{")
    for start, _, _ in blocks:
        name = f"sha256Block_{start:x}"
        lines.append(f"    {{0x{start:x}, {name}Words, {name}}},")
    lines.extend(["}};", "} // namespace nestvane", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    blocks = collect(args.module.read_bytes()) if args.module.is_file() else []
    temporary = args.output.with_suffix(".tmp")
    temporary.write_text(generate(blocks))
    temporary.replace(args.output)
    if blocks:
        count = sum(len(words) for _, words, _ in blocks)
        print(f"Compiled {count} Android SHA-256 instructions across {len(blocks)} AOT blocks")


if __name__ == "__main__":
    main()
