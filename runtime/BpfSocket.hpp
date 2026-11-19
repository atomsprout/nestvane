#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace nestvane {

class BpfSocketFilter {
public:
    static constexpr size_t maxInstructions = 4096;

    bool load(std::span<const uint8_t> bytes) {
        if (bytes.empty() || bytes.size() % sizeof(Instruction) || bytes.size() / sizeof(Instruction) > maxInstructions)
            return false;
        std::vector<Instruction> decoded(bytes.size() / sizeof(Instruction));
        std::memcpy(decoded.data(), bytes.data(), bytes.size());
        for (size_t pc = 0; pc < decoded.size(); ++pc) {
            const auto& i = decoded[pc];
            if (i.destination() > 10 || i.source() > 10) return false;
            switch (i.code) {
                case 0x18: // Load a 64-bit immediate split over two instruction slots.
                    if (++pc >= decoded.size() || decoded[pc].code || decoded[pc].registers || decoded[pc].offset)
                        return false;
                    break;
                case 0x05: case 0x15: case 0x55: {
                    const auto target = int64_t(pc) + 1 + i.offset;
                    if (target < 0 || uint64_t(target) >= decoded.size() || !decoded[target].code) return false;
                    break;
                }
                case 0x85: // bpf_skb_load_bytes is the only helper supported by this packet filter.
                    if (i.registers || i.offset || i.immediate != 26) return false;
                    break;
                case 0x07: case 0x4f: case 0x57: case 0x61: case 0x79:
                case 0x7b: case 0x95: case 0xa7: case 0xaf: case 0xb7: case 0xbf:
                    break;
                default: return false;
            }
        }
        instructions = std::move(decoded);
        return true;
    }

    size_t accept(std::span<const uint8_t> packet) const {
        constexpr uint64_t context = 0x10000;
        std::array<uint8_t, 512> stack{};
        std::array<uint64_t, 11> registers{};
        registers[1] = context;
        registers[10] = stack.size();
        const auto spanAt = [&](uint64_t address, uint64_t count) -> std::span<uint8_t> {
            if (address >= stack.size() || count > stack.size() - address) return {};
            return std::span(stack).subspan(address, count);
        };

        size_t pc = 0;
        // All guest addresses are numeric offsets into the stack or the synthetic packet context.
        for (size_t fuel = 0; fuel < 16384 && pc < instructions.size(); ++fuel) {
            const auto& i = instructions[pc++];
            auto& dst = registers[i.destination()];
            const auto src = registers[i.source()];
            const auto imm = uint64_t(int64_t(i.immediate));
            switch (i.code) {
                case 0x18:
                    dst = uint32_t(imm) | (uint64_t(uint32_t(instructions[pc++].immediate)) << 32);
                    break;
                case 0x05: pc += i.offset; break;
                case 0x07: dst += imm; break;
                case 0x15: if (dst == imm) pc += i.offset; break;
                case 0x4f: dst |= src; break;
                case 0x55: if (dst != imm) pc += i.offset; break;
                case 0x57: dst &= imm; break;
                case 0x61:
                    if (src + i.offset != context) return 0;
                    dst = uint32_t(packet.size()); // __sk_buff.len
                    break;
                case 0x79: {
                    const auto from = spanAt(src + i.offset, sizeof(dst));
                    if (from.empty()) return 0;
                    std::memcpy(&dst, from.data(), sizeof(dst));
                    break;
                }
                case 0x7b: {
                    const auto to = spanAt(dst + i.offset, sizeof(src));
                    if (to.empty()) return 0;
                    std::memcpy(to.data(), &src, sizeof(src));
                    break;
                }
                case 0x85: {
                    const auto offset = uint32_t(registers[2]), length = uint32_t(registers[4]);
                    const auto to = spanAt(registers[3], length);
                    if (registers[1] != context || to.empty()) return 0;
                    if (offset > packet.size() || length > packet.size() - offset) {
                        std::ranges::fill(to, 0);
                        registers[0] = uint64_t(-14);
                    } else {
                        std::ranges::copy(packet.subspan(offset, length), to.begin());
                        registers[0] = 0;
                    }
                    std::fill_n(registers.begin() + 1, 5, 0);
                    break;
                }
                case 0x95: return std::min<size_t>(uint32_t(registers[0]), packet.size());
                case 0xa7: dst ^= imm; break;
                case 0xaf: dst ^= src; break;
                case 0xb7: dst = imm; break;
                case 0xbf: dst = src; break;
                default: return 0;
            }
        }
        return 0; // Invalid instruction flow or fuel exhausted.
    }

private:
    struct Instruction {
        uint8_t code, registers;
        int16_t offset;
        int32_t immediate;
        unsigned destination() const { return registers & 15; }
        unsigned source() const { return registers >> 4; }
    };
    static_assert(sizeof(Instruction) == 8);
    std::vector<Instruction> instructions;
};

} // namespace nestvane
