#include "AArch64.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nestvane {

namespace {

constexpr unsigned zero = 31;

uint64_t mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
}

uint64_t sx(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return ((value & mask(bits)) ^ sign) - sign;
}

uint64_t reg(const CPU& cpu, unsigned n, bool stack = false) {
    if (n < zero) return cpu.x[n];
    return stack ? cpu.sp : 0;
}

void put(CPU& cpu, unsigned n, uint64_t value, unsigned bits = 64, bool stack = false) {
    value &= mask(bits);
    if (n < zero) cpu.x[n] = value;
    else if (stack) cpu.sp = value;
}

uint64_t rotate(uint64_t value, unsigned amount, unsigned bits) {
    amount %= bits;
    value &= mask(bits);
    return amount ? ((value >> amount) | (value << (bits - amount))) & mask(bits) : value;
}

uint64_t shift(uint64_t value, unsigned type, unsigned amount, unsigned bits) {
    value &= mask(bits);
    switch (type) {
        case 0: return (value << amount) & mask(bits);
        case 1: return value >> amount;
        case 2:
            return (value >> amount) |
                   ((value >> (bits - 1)) ? (mask(bits) ^ mask(bits - amount)) : 0);
        case 3: return rotate(value, amount, bits);
    }
    return 0;
}

bool condition(uint8_t nzcv, unsigned code) {
    bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
    bool match = true;
    switch (code >> 1) {
        case 0: match = z; break;
        case 1: match = c; break;
        case 2: match = n; break;
        case 3: match = v; break;
        case 4: match = c && !z; break;
        case 5: match = n == v; break;
        case 6: match = n == v && !z; break;
        case 7: return true;
    }
    return code & 1 ? !match : match;
}

void flags(CPU& cpu, uint64_t result, unsigned bits, bool carry = false, bool overflow = false) {
    result &= mask(bits);
    cpu.nzcv = ((result >> (bits - 1)) << 3) | (result == 0 ? 4 : 0) |
               (carry ? 2 : 0) | (overflow ? 1 : 0);
}

void arithmeticFlags(CPU& cpu, uint64_t a, uint64_t b, uint64_t result, unsigned bits, bool sub) {
    const bool carry = sub ? a >= b : result < a;
    const uint64_t different = a ^ b;
    const bool overflow = ((sub ? different : ~different) & (a ^ result)) >> (bits - 1) & 1;
    flags(cpu, result, bits, carry, overflow);
}

bool logicalMask(unsigned n, unsigned immr, unsigned imms, unsigned bits, uint64_t& result) {
    unsigned encoding = n << 6 | (~imms & 63);
    if (encoding < 2) return false;
    unsigned size = 1U << (std::bit_width(encoding) - 1);
    if (size > bits) return false;
    unsigned s = imms & (size - 1);
    if (s == size - 1) return false;
    uint64_t pattern = rotate(mask(s + 1), immr & (size - 1), size);
    result = 0;
    for (unsigned position = 0; position < bits; position += size) result |= pattern << position;
    return true;
}

bool transfer(Machine& machine, uint64_t address, unsigned rt, unsigned size,
              bool load, bool sign = false, unsigned bits = 64) {
    if (load) {
        uint64_t value = 0;
        if (machine.readInto(address, {reinterpret_cast<uint8_t*>(&value), size})) {
            put(machine.cpu, rt, sign ? sx(value, size * 8) : value, bits);
            return true;
        }
    } else {
        const uint64_t value = reg(machine.cpu, rt);
        if (machine.write(address, {reinterpret_cast<const uint8_t*>(&value), size})) return true;
    }
    machine.fault(machine.cpu.pc, address);
    return false;
}

bool branches(CPU& cpu, uint32_t word, uint64_t& next) {
    if ((word & 0x7c000000) == 0x14000000) {
        if (word >> 31) cpu.x[30] = next;
        next = cpu.pc + sx(word & 0x3ffffff, 26) * 4;
    } else if ((word & 0xff000010) == 0x54000000) {
        if (condition(cpu.nzcv, word & 15)) next = cpu.pc + sx((word >> 5) & 0x7ffff, 19) * 4;
    } else if ((word & 0x7e000000) == 0x34000000) {
        const unsigned bits = word >> 31 ? 64 : 32;
        const bool nonzero = word & 0x01000000;
        if (bool(reg(cpu, word & 31) & mask(bits)) == nonzero) {
            next = cpu.pc + sx((word >> 5) & 0x7ffff, 19) * 4;
        }
    } else if ((word & 0x7e000000) == 0x36000000) {
        const unsigned bit = (word >> 31) * 32 + ((word >> 19) & 31);
        const bool nonzero = word & 0x01000000;
        if (bool((reg(cpu, word & 31) >> bit) & 1) == nonzero) {
            next = cpu.pc + sx((word >> 5) & 0x3fff, 14) * 4;
        }
    } else {
        const uint32_t op = word & 0xfffffc1f;
        if (op != 0xd61f0000 && op != 0xd63f0000 && op != 0xd65f0000) return false;
        const uint64_t target = reg(cpu, (word >> 5) & 31);
        if (op == 0xd63f0000) cpu.x[30] = next;
        next = target;
    }
    return true;
}

bool arithmetic(CPU& cpu, uint32_t word) {
    const unsigned bits = word >> 31 ? 64 : 32;
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const bool sub = word & 0x40000000, set = word & 0x20000000;
    const bool immediate = (word & 0x1f800000) == 0x11000000;
    uint64_t a = reg(cpu, rn, immediate) & mask(bits), b = 0;
    if ((word & 0x1fe0fc00) == 0x1a000000) { // ADC/ADCS/SBC/SBCS.
        b = (sub ? ~reg(cpu, rm) : reg(cpu, rm)) & mask(bits);
        const auto wide = static_cast<unsigned __int128>(a) + b + ((cpu.nzcv >> 1) & 1);
        const auto result = static_cast<uint64_t>(wide) & mask(bits);
        if (set) {
            const auto sign = uint64_t{1} << (bits - 1);
            const bool overflow = (~(a ^ b) & (a ^ result) & sign) != 0;
            cpu.nzcv = ((result & sign) ? 8 : 0) | (result == 0 ? 4 : 0) |
                       ((wide >> bits) ? 2 : 0) | (overflow ? 1 : 0);
        }
        put(cpu, rd, result, bits);
        return true;
    }
    if (immediate) {
        b = ((word >> 10) & 0xfff) << ((word & 0x400000) ? 12 : 0);
    } else if ((word & 0x1f200000) == 0x0b000000) {
        const unsigned type = (word >> 22) & 3, amount = (word >> 10) & 63;
        if (type == 3 || amount >= bits) return false;
        b = shift(reg(cpu, rm), type, amount, bits);
    } else if ((word & 0x1fe00000) == 0x0b200000) {
        const unsigned option = (word >> 13) & 7, amount = (word >> 10) & 7;
        if (amount > 4) return false;
        const unsigned width = 8U << (option & 3);
        a = reg(cpu, rn, true) & mask(bits);
        const uint64_t operand = reg(cpu, rm);
        b = ((option & 4 ? sx(operand, width) : operand & mask(width)) << amount) & mask(bits);
    } else {
        return false;
    }
    const uint64_t result = (sub ? a - b : a + b) & mask(bits);
    if (set) arithmeticFlags(cpu, a, b, result, bits, sub);
    put(cpu, rd, result, bits, !set && (immediate || (word & 0x00200000)));
    return true;
}

bool logic(CPU& cpu, uint32_t word) {
    const unsigned bits = word >> 31 ? 64 : 32;
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const unsigned opcode = (word >> 29) & 3;
    uint64_t b;
    bool stack = false;
    if ((word & 0x1f800000) == 0x12000000) {
        if (!logicalMask((word >> 22) & 1, (word >> 16) & 63, (word >> 10) & 63, bits, b)) return false;
        stack = opcode != 3;
    } else if ((word & 0x1f000000) == 0x0a000000) {
        const unsigned amount = (word >> 10) & 63;
        if (amount >= bits) return false;
        b = shift(reg(cpu, rm), (word >> 22) & 3, amount, bits);
        if (word & 0x200000) b = ~b;
    } else {
        return false;
    }
    const uint64_t a = reg(cpu, rn);
    const uint64_t result = opcode == 1 ? a | b : opcode == 2 ? a ^ b : a & b;
    if (opcode == 3) flags(cpu, result, bits);
    put(cpu, rd, result, bits, stack);
    return true;
}

bool memory(Machine& machine, uint32_t word) {
    auto& cpu = machine.cpu;
    const unsigned rt = word & 31, rn = (word >> 5) & 31;
    const uint64_t base = reg(cpu, rn, true);
    const unsigned size = 1U << (word >> 30);
    // Single-register LL/SC, with an explicit local monitor for one guest task.
    if ((word & 0x3fff7c00) == 0x085f7c00) {
        if (base % size) machine.fault(cpu.pc, base);
        else if (transfer(machine, base, rt, size, true)) {
            cpu.exclusiveAddress = machine.dataAddress(base);
            cpu.exclusiveSize = size;
        }
        return true;
    }
    if ((word & 0x3fe07c00) == 0x08007c00) {
        const unsigned rs = (word >> 16) & 31;
        const bool matched = cpu.exclusiveSize == size && cpu.exclusiveAddress == machine.dataAddress(base);
        cpu.exclusiveSize = 0;
        if (base % size) machine.fault(cpu.pc, base);
        else if (!matched) put(cpu, rs, 1, 32);
        else if (transfer(machine, base, rt, size, false)) put(cpu, rs, 0, 32);
        return true;
    }
    const bool loadExclusivePair = (word & 0xbfff0000) == 0x887f0000;
    const bool storeExclusivePair = (word & 0xbfe00000) == 0x88200000;
    if (loadExclusivePair || storeExclusivePair) {
        const unsigned rt2 = (word >> 10) & 31;
        const unsigned pairSize = size * 2;
        const auto address = machine.dataAddress(base);
        const bool matched = cpu.exclusiveSize == pairSize && cpu.exclusiveAddress == address;
        cpu.exclusiveSize = 0;
        std::array<uint8_t, 16> bytes{};
        const auto pair = std::span(bytes).first(pairSize);
        if (address % pairSize) {
            machine.fault(cpu.pc, base);
        } else if (loadExclusivePair) {
            if (!machine.readInto(address, pair)) {
                machine.fault(cpu.pc, base);
                return true;
            }
            uint64_t first = 0, second = 0;
            std::memcpy(&first, bytes.data(), size);
            std::memcpy(&second, bytes.data() + size, size);
            put(cpu, rt, first, size * 8);
            put(cpu, rt2, second, size * 8);
            cpu.exclusiveAddress = address;
            cpu.exclusiveSize = pairSize;
        } else {
            const unsigned rs = (word >> 16) & 31;
            if (!matched) {
                put(cpu, rs, 1, 32);
                return true;
            }
            const auto first = reg(cpu, rt), second = reg(cpu, rt2);
            std::memcpy(bytes.data(), &first, size);
            std::memcpy(bytes.data() + size, &second, size);
            if (machine.write(address, pair)) put(cpu, rs, 0, 32);
            else machine.fault(cpu.pc, base);
        }
        return true;
    }
    // Acquire and release ordering is inherent in the single-thread probe.
    if ((word & 0x3f9ffc00) == 0x089ffc00) {
        transfer(machine, base, rt, size, word & 0x400000);
        return true;
    }
    if ((word & 0x3e000000) == 0x28000000) {
        const unsigned type = word >> 30, mode = (word >> 23) & 3;
        const bool load = word & (1U << 22);
        if (type == 3 || (type == 1 && !load)) return false;
        const unsigned width = type == 2 ? 8 : 4;
        const uint64_t offset = sx((word >> 15) & 127, 7) * width;
        const uint64_t address = mode == 1 ? base : base + offset;
        const unsigned rt2 = (word >> 10) & 31;
        if (transfer(machine, address, rt, width, load, type == 1) &&
            transfer(machine, address + width, rt2, width, load, type == 1) && (mode == 1 || mode == 3)) {
            put(cpu, rn, base + offset, 64, true);
        }
        return true;
    }
    // Prefetch hints have no architectural result in the guest interpreter.
    if ((word & 0xffc00000) == 0xf9800000 || // PRFM (unsigned offset)
        (word & 0xffe02c00) == 0xf8a02800 || // PRFM (register offset)
        (word & 0xff000000) == 0xd8000000 || // PRFM (literal)
        (word & 0xffe00c00) == 0xf8800000) return true; // PRFUM (unscaled offset)
    if ((word & 0x3b000000) == 0x18000000 && !(word & 0x04000000)) {
        if (size > 4) return false;
        const uint64_t address = cpu.pc + sx((word >> 5) & 0x7ffff, 19) * 4;
        transfer(machine, address, rt, size == 2 ? 8 : 4, true, size == 4);
        return true;
    }
    if (word & 0x04000000) return false;
    const unsigned op = (word >> 22) & 3;
    if ((size == 8 && op >= 2) || (size == 4 && op == 3)) return false;
    uint64_t offset = 0;
    unsigned mode = 0;
    if ((word & 0x3b000000) == 0x39000000) {
        offset = ((word >> 10) & 0xfff) * size;
    } else if ((word & 0x3b200000) == 0x38000000) {
        mode = (word >> 10) & 3;
        if (mode == 2) return false;
        offset = sx((word >> 12) & 511, 9);
    } else if ((word & 0x3b200c00) == 0x38200800) {
        const unsigned option = (word >> 13) & 7, rs = (word >> 16) & 31;
        if (option == 2) offset = reg(cpu, rs) & UINT32_MAX;
        else if (option == 3 || option == 7) offset = reg(cpu, rs);
        else if (option == 6) offset = sx(reg(cpu, rs), 32);
        else return false;
        if (word & 0x1000) offset *= size;
    } else {
        return false;
    }
    if (transfer(machine, mode == 1 ? base : base + offset, rt, size, op != 0, op >= 2,
                 op == 3 ? 32 : 64) && (mode == 1 || mode == 3)) {
        put(cpu, rn, base + offset, 64, true);
    }
    return true;
}

bool vectorTransfer(Machine& m, uint64_t address, unsigned rt, unsigned size, bool load) {
    auto& vector = m.cpu.v[rt];
    if (load) {
        std::array<uint8_t, 16> value{};
        if (m.readInto(address, std::span(value).first(size))) {
            vector = value;
            return true;
        }
    } else if (m.write(address, std::span(vector).first(size))) {
        return true;
    }
    m.fault(m.cpu.pc, address);
    return false;
}

uint64_t lane(const std::array<uint8_t, 16>& vector, unsigned index, unsigned size) {
    uint64_t value = 0;
    std::memcpy(&value, vector.data() + index * size, size);
    return value;
}

void setLane(std::array<uint8_t, 16>& vector, unsigned index, unsigned size, uint64_t value) {
    std::memcpy(vector.data() + index * size, &value, size);
}

template<class Float, class UInt>
Float floatingMinMax(CPU& cpu, Float left, Float right, bool minimum, bool numeric) {
    const auto leftBits = std::bit_cast<UInt>(left), rightBits = std::bit_cast<UInt>(right);
    constexpr UInt quietBit = UInt{1} << (std::numeric_limits<Float>::digits - 2);
    const bool leftNaN = std::isnan(left), rightNaN = std::isnan(right);
    const bool leftSignaling = leftNaN && !(leftBits & quietBit);
    const bool rightSignaling = rightNaN && !(rightBits & quietBit);
    if (leftSignaling || rightSignaling) {
        cpu.fpsr |= 1; // Invalid Operation cumulative exception (IOC).
        return std::bit_cast<Float>((leftSignaling ? leftBits : rightBits) | quietBit);
    }
    if (leftNaN && (!numeric || rightNaN)) return left;
    if (rightNaN) return numeric ? left : right;
    if (leftNaN) return right;
    if (left == 0 && right == 0) {
        const bool negative = minimum ? std::signbit(left) || std::signbit(right)
                                      : std::signbit(left) && std::signbit(right);
        return negative ? Float{-0.0} : Float{0.0};
    }
    return minimum ? std::min(left, right) : std::max(left, right);
}

template <class Float>
Float roundToNearestEven(CPU& cpu, Float value) {
    using UInt = std::conditional_t<sizeof(Float) == 4, uint32_t, uint64_t>;
    if (std::isnan(value)) {
        constexpr UInt quietBit = UInt{1} << (std::numeric_limits<Float>::digits - 2);
        const auto bits = std::bit_cast<UInt>(value);
        if (!(bits & quietBit)) cpu.fpsr |= 1; // Invalid Operation (IOC).
        return std::bit_cast<Float>(bits | quietBit);
    }
    if (!std::isfinite(value)) return value;
    const auto magnitude = std::abs(value);
    const auto integral = std::floor(magnitude);
    const auto fraction = magnitude - integral;
    const bool increment = fraction > Float{0.5} ||
        (fraction == Float{0.5} && std::fmod(integral, Float{2}) != 0);
    return std::copysign(integral + increment, value);
}

bool vectorArithmetic(CPU& cpu, uint32_t word) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const auto& source = cpu.v[rn];
    const auto& other = cpu.v[rm];
    std::array<uint8_t, 16> result{};
    const unsigned width = word & 0x40000000 ? 16 : 8;
    unsigned size = 1U << ((word >> 22) & 3);
    const uint32_t op = word & 0xbf20fc00;
    const uint32_t fmov = word & 0xfffffc00;
    const bool scalar = word & 0x10000000;
    const auto integerElement = word & 0xbf00f400;
    const auto fpElement = word & 0xaf80f400;
    if (integerElement == 0x0f008000 || integerElement == 0x2f000000 || integerElement == 0x2f004000) {
        // MUL, MLA and MLS with an indexed element.
        if (size != 2 && size != 4) return false;
        const unsigned highIndex = (word >> 11) & 1;
        const unsigned lowIndex = (word >> 21) & 1;
        // In the halfword encoding, the high register bit selects a lane instead.
        const unsigned index = size == 2 ? highIndex * 4 + lowIndex * 2 + (rm >> 4) : highIndex * 2 + lowIndex;
        const auto multiplier = lane(cpu.v[size == 2 ? rm & 15 : rm], index, size);
        for (unsigned i = 0; i < width / size; ++i) {
            const auto product = lane(source, i, size) * multiplier;
            auto value = product;
            if (integerElement == 0x2f000000) value = lane(cpu.v[rd], i, size) + product;
            if (integerElement == 0x2f004000) value = lane(cpu.v[rd], i, size) - product;
            setLane(result, i, size, value);
        }
    } else if (fpElement == 0x0f801000 || fpElement == 0x0f805000 || fpElement == 0x0f809000) {
        // FMLA, FMLS and FMUL with an indexed element.
        const bool wide = word & 0x400000;
        if ((scalar && width != 16) || (wide && (width != 16 || (word & 0x200000)))) return false;
        const unsigned highIndex = (word >> 11) & 1;
        const unsigned index = wide ? highIndex : highIndex * 2 + ((word >> 21) & 1);
        const auto apply = [&]<class Float, class UInt>() {
            const auto get = [&](const auto& value, unsigned position) {
                return std::bit_cast<Float>(static_cast<UInt>(lane(value, position, sizeof(Float))));
            };
            const auto right = get(other, index);
            const unsigned lanes = scalar ? 1 : width / sizeof(Float);
            for (unsigned i = 0; i < lanes; ++i) {
                const auto left = get(source, i);
                const auto addend = get(cpu.v[rd], i);
                const auto value = fpElement == 0x0f809000 ? left * right :
                    std::fma(fpElement == 0x0f805000 ? -left : left, right, addend);
                setLane(result, i, sizeof(Float), std::bit_cast<UInt>(value));
            }
        };
        if (wide) apply.operator()<double, uint64_t>();
        else apply.operator()<float, uint32_t>();
    } else if ((word & 0xdfbffc00) == 0x5e21d800 ||
               (word & 0x9fbffc00) == 0x0e21d800) { // SCVTF/UCVTF with SIMD operands.
        const bool unsignedSource = word & 0x20000000;
        const bool wide = word & 0x400000;
        if (!scalar && wide && width == 8) return false;
        const auto convert = [&]<class Float, class UInt, class Int>() {
            for (unsigned i = 0; i < (scalar ? 1 : width / sizeof(UInt)); ++i) {
                const auto bits = static_cast<UInt>(lane(source, i, sizeof(UInt)));
                const auto value = unsignedSource ? static_cast<Float>(bits) :
                    static_cast<Float>(std::bit_cast<Int>(bits));
                setLane(result, i, sizeof(UInt), std::bit_cast<UInt>(value));
            }
        };
        if (wide) convert.operator()<double, uint64_t, int64_t>();
        else convert.operator()<float, uint32_t, int32_t>();
    } else if (const auto conversion = word & 0x8fbffc00;
               conversion == 0x0ea1b800 || conversion == 0x0e21a800 ||
               (word & 0x8f80fc00) == 0x0f00fc00) {
        // FCVT{N,Z}{S,U} in SIMD registers. Fixed-point immediates round toward zero.
        const bool nearest = conversion == 0x0e21a800;
        const bool fixed = word & 0x01000000;
        if (scalar && width != 16) return false;
        const auto immediate = (word >> 16) & 127;
        if (fixed && immediate < 32) return false;
        const bool wide = word & 0x400000;
        if (!scalar && wide && width == 8) return false;
        const bool unsignedResult = word & 0x20000000;
        size = wide ? 8 : 4;
        const unsigned bits = size * 8;
        const auto scale = fixed ? int(2 * bits - immediate) : 0;
        const auto limit = std::ldexp(1.0, unsignedResult ? bits : bits - 1);
        for (unsigned i = 0; i < (scalar ? 1 : width / size); ++i) {
            const auto input = lane(source, i, size);
            const double number = wide ? std::bit_cast<double>(input) :
                static_cast<double>(std::bit_cast<float>(uint32_t(input)));
            double value = fixed ? std::ldexp(number, scale) : number;
            if (nearest) {
                if (std::isfinite(number)) {
                    const auto lower = std::floor(number);
                    const auto fraction = number - lower;
                    value = lower + (fraction > 0.5 || (fraction == 0.5 && std::fmod(lower, 2.0) != 0));
                }
                const bool invalid = std::isnan(value) || value >= limit ||
                    value < (unsignedResult ? 0 : -limit);
                if (invalid) cpu.fpsr |= 1; // IOC
                else if (value != number) cpu.fpsr |= 1U << 4; // IXC
            }
            uint64_t converted = 0;
            if (!std::isnan(value)) {
                if (unsignedResult) {
                    if (value >= limit) converted = mask(bits);
                    else if (value > 0) converted = static_cast<uint64_t>(value);
                } else if (value <= -limit) {
                    converted = uint64_t{1} << (bits - 1);
                } else if (value >= limit) {
                    converted = (uint64_t{1} << (bits - 1)) - 1;
                } else {
                    converted = uint64_t(static_cast<int64_t>(value));
                }
            }
            setLane(result, i, size, converted);
        }
    } else if (const auto arithmetic = word & 0xbfa0fc00;
               arithmetic == 0x0e20d400 || arithmetic == 0x0ea0d400 ||
               arithmetic == 0x2e20dc00 || arithmetic == 0x2e20fc00 ||
               arithmetic == 0x0e20cc00 || arithmetic == 0x0ea0cc00 ||
               arithmetic == 0x2ea0d400 || (word & 0xffa0fc00) == 0x7ea0d400 ||
               (word & 0xffbffc00) == 0x7e30d800) {
        // Vector floating arithmetic, scalar FABD and scalar FADDP.
        const bool pair = (word & 0xffbffc00) == 0x7e30d800;
        const bool wide = word & 0x400000;
        if (!scalar && wide && width == 8) return false;
        const auto apply = [&]<class Float, class UInt>() {
            const auto get = [&](const auto& value, unsigned index) {
                return std::bit_cast<Float>(static_cast<UInt>(lane(value, index, sizeof(Float))));
            };
            for (unsigned i = 0; i < (scalar ? 1 : width / sizeof(Float)); ++i) {
                const auto left = get(source, i), right = pair ? get(source, 1) : get(other, i);
                Float value;
                switch (arithmetic) {
                    case 0x0e20d400: case 0x3e20d800: value = left + right; break;
                    case 0x0ea0d400: value = left - right; break;
                    case 0x2e20dc00: value = left * right; break;
                    case 0x2e20fc00: value = left / right; break;
                    case 0x0e20cc00: value = std::fma(left, right, get(cpu.v[rd], i)); break;
                    case 0x0ea0cc00: value = std::fma(-left, right, get(cpu.v[rd], i)); break;
                    default: value = std::abs(left - right); break;
                }
                setLane(result, i, sizeof(Float), std::bit_cast<UInt>(value));
            }
        };
        if (wide) apply.operator()<double, uint64_t>();
        else apply.operator()<float, uint32_t>();
    } else if (const auto comparison = word & 0xbfa0fc00;
               comparison == 0x0e20e400 || comparison == 0x2e20e400 || comparison == 0x2ea0e400) {
        // FCMEQ, FCMGE and FCMGT with vector register operands.
        const bool wide = word & 0x400000;
        if (wide && width == 8) return false;
        size = wide ? 8 : 4;
        const auto value = [&](const auto& reg, unsigned i) {
            const auto bits = lane(reg, i, size);
            return wide ? std::bit_cast<double>(bits) :
                          static_cast<double>(std::bit_cast<float>(uint32_t(bits)));
        };
        for (unsigned i = 0; i < width / size; ++i) {
            const auto left = value(source, i), right = value(other, i);
            const bool match = comparison == 0x0e20e400 ? left == right :
                               comparison == 0x2e20e400 ? left >= right : left > right;
            setLane(result, i, size, match ? UINT64_MAX : 0);
        }
    } else if (const auto fpCompareZero = word & 0xbfbffc00;
               fpCompareZero == 0x0ea0c800 || fpCompareZero == 0x2ea0c800 ||
               fpCompareZero == 0x0ea0d800 || fpCompareZero == 0x2ea0d800 ||
               fpCompareZero == 0x0ea0e800) { // FCMGT, FCMGE, FCMEQ, FCMLE, FCMLT (vector, zero).
        const bool wide = word & 0x400000;
        if (wide && width == 8) return false;
        size = wide ? 8 : 4;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto bits = lane(source, i, size);
            const double value = wide ? std::bit_cast<double>(bits) :
                                 static_cast<double>(std::bit_cast<float>(uint32_t(bits)));
            bool match = false;
            switch (fpCompareZero) {
                case 0x0ea0c800: match = value > 0; break;
                case 0x2ea0c800: match = value >= 0; break;
                case 0x0ea0d800: match = value == 0; break;
                case 0x2ea0d800: match = value <= 0; break;
                case 0x0ea0e800: match = value < 0; break;
            }
            setLane(result, i, size, match ? UINT64_MAX : 0);
        }
    } else if (const auto rounding = word & 0xbfbffc00;
               rounding == 0x0e219800 || rounding == 0x2e218800 ||
               rounding == 0x0ea18800 || rounding == 0x0e218800) { // FRINTM/A/P/N (vector).
        const auto roundToIntegral = [rounding, &cpu](auto value) {
            if (rounding == 0x0e219800) return std::floor(value);
            if (rounding == 0x0ea18800) return std::ceil(value);
            if (rounding == 0x2e218800) return std::round(value);

            return roundToNearestEven(cpu, value);
        };
        const bool wide = word & 0x400000;
        if (wide && width == 8) return false;
        size = wide ? 8 : 4;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto bits = lane(source, i, size);
            const uint64_t rounded = wide
                ? std::bit_cast<uint64_t>(roundToIntegral(std::bit_cast<double>(bits)))
                : std::bit_cast<uint32_t>(roundToIntegral(std::bit_cast<float>(uint32_t(bits))));
            setLane(result, i, size, rounded);
        }
    } else if ((word & 0x9fbffc00) == 0x0ea0f800) { // FABS / FNEG (vector).
        const bool wide = word & 0x400000;
        if (wide && width == 8) return false;
        size = wide ? 8 : 4;
        const bool negate = word & 0x20000000;
        const auto sign = uint64_t{1} << (size * 8 - 1);
        for (unsigned i = 0; i < width / size; ++i) {
            const auto value = lane(source, i, size);
            setLane(result, i, size, negate ? value ^ sign : value & ~sign);
        }
    } else if ((word & 0xbfbffc00) == 0x2ea1f800) { // FSQRT (vector).
        const bool wide = word & 0x400000;
        if (wide && width == 8) return false;
        size = wide ? 8 : 4;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto bits = lane(source, i, size);
            const uint64_t root = wide
                ? std::bit_cast<uint64_t>(std::sqrt(std::bit_cast<double>(bits)))
                : std::bit_cast<uint32_t>(std::sqrt(std::bit_cast<float>(uint32_t(bits))));
            setLane(result, i, size, root);
        }
    } else if ((word & 0xbffffc00) == 0x0e617800) { // FCVTL / FCVTL2 from S to D.
        const unsigned first = width == 16 ? 2 : 0;
        for (unsigned i = 0; i < 2; ++i) {
            const auto value = std::bit_cast<float>(uint32_t(lane(source, first + i, 4)));
            setLane(result, i, 8, std::bit_cast<uint64_t>(static_cast<double>(value)));
        }
    } else if ((word & 0xbffffc00) == 0x0e616800) { // FCVTN / FCVTN2 from D to S.
        const unsigned first = width == 16 ? 2 : 0;
        if (first) result = cpu.v[rd];
        for (unsigned i = 0; i < 2; ++i) {
            const auto value = std::bit_cast<double>(lane(source, i, 8));
            setLane(result, first + i, 4, std::bit_cast<uint32_t>(static_cast<float>(value)));
        }
    } else if (const auto minMax = word & 0xbf20fc00;
               minMax == 0x0e20f400 || minMax == 0x0e20c400) { // FMAX, FMIN, FMAXNM, FMINNM (vector).
        const bool wide = word & 0x400000, minimum = word & 0x800000;
        const bool numeric = minMax == 0x0e20c400;
        if (wide && width == 8) return false;
        const auto apply = [&]<class Float, class UInt>() {
            constexpr unsigned laneSize = sizeof(Float);
            for (unsigned i = 0; i < width / laneSize; ++i) {
                const auto left = std::bit_cast<Float>(UInt(lane(source, i, laneSize)));
                const auto right = std::bit_cast<Float>(UInt(lane(other, i, laneSize)));
                const auto value = floatingMinMax<Float, UInt>(cpu, left, right, minimum, numeric);
                setLane(result, i, laneSize, std::bit_cast<UInt>(value));
            }
        };
        if (wide) apply.operator()<double, uint64_t>();
        else apply.operator()<float, uint32_t>();
    } else if ((word & 0xbf3ffc00) == 0x0e209800) { // CMEQ (zero).
        for (unsigned i = 0; i < width / size; ++i) {
            setLane(result, i, size, lane(source, i, size) == 0 ? UINT64_MAX : 0);
        }
    } else if (const auto compareZero = word & 0xbf3ffc00;
               compareZero == 0x0e208800 || compareZero == 0x0e20a800 ||
               compareZero == 0x2e208800 || compareZero == 0x2e209800) { // CMGT, CMLT, CMGE, CMLE (zero).
        if (size == 8 && width == 8) return false;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto value = lane(source, i, size);
            const bool negative = value >> (size * 8 - 1);
            bool match = false;
            switch (compareZero) {
                case 0x0e208800: match = !negative && value != 0; break;
                case 0x0e20a800: match = negative; break;
                case 0x2e208800: match = !negative; break;
                case 0x2e209800: match = negative || value == 0; break;
            }
            setLane(result, i, size, match ? UINT64_MAX : 0);
        }
    } else if (const auto unaryInteger = word & 0xbf3ffc00;
               unaryInteger == 0x0e20b800 || unaryInteger == 0x2e20b800) { // ABS, NEG (vector).
        if (size == 8 && width == 8) return false;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto value = lane(source, i, size);
            const bool negate = unaryInteger == 0x2e20b800 || (value >> (size * 8 - 1));
            setLane(result, i, size, negate ? 0 - value : value);
        }
    } else if ((word & 0xbf3ffc00) == 0x2e213800) { // SHLL / SHLL2 by the source element width.
        if (size == 8) return false;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        for (unsigned i = 0; i < count; ++i)
            setLane(result, i, 2 * size, lane(source, start + i, size) << (8 * size));
    } else if ((word & 0x9f3ffc00) == 0x0e200800) { // REV32 and REV64 (vector).
        const unsigned groupSize = word & 0x20000000 ? 4 : 8;
        if (size >= groupSize) return false;
        const auto lanes = groupSize / size;
        for (unsigned i = 0; i < width / size; ++i) {
            setLane(result, i, size, lane(source, (i / lanes) * lanes + lanes - 1 - i % lanes, size));
        }
    } else if ((word & 0xbf3ffc00) == 0x2e204800) { // CLZ (vector).
        if (size == 8) return false;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto value = lane(source, i, size);
            setLane(result, i, size, std::countl_zero(value) - (64 - size * 8));
        }
    } else if ((word & 0xbffffc00) == 0x2e205800) { // MVN (vector).
        for (unsigned i = 0; i < width; ++i) result[i] = ~source[i];
    } else if ((word & 0xbffffc00) == 0x0e205800) { // CNT.
        for (unsigned i = 0; i < width; ++i) result[i] = std::popcount(source[i]);
    } else if ((word & 0xfffffc00) == 0x5ef1b800) { // ADDP Dd, Vn.2D.
        setLane(result, 0, 8, lane(source, 0, 8) + lane(source, 1, 8));
    } else if ((word & 0xbf3ffc00) == 0x0e31b800) { // ADDV.
        if (size == 8 || (size == 4 && width == 8)) return false;
        uint64_t sum = 0;
        for (unsigned i = 0; i < width / size; ++i) sum += lane(source, i, size);
        setLane(result, 0, size, sum);
    } else if ((word & 0x9f3efc00) == 0x0e30a800) { // SMAXV, SMINV, UMAXV, UMINV.
        if (size == 8 || (size == 4 && width == 8)) return false;
        const bool minimum = word & 0x00010000;
        const uint64_t signBit = word & 0x20000000 ? 0 : uint64_t{1} << (size * 8 - 1);
        // Flipping the sign bit orders two's-complement lanes as unsigned values.
        auto value = lane(source, 0, size) ^ signBit;
        for (unsigned i = 1; i < width / size; ++i) {
            const auto next = lane(source, i, size) ^ signBit;
            value = minimum ? std::min(value, next) : std::max(value, next);
        }
        setLane(result, 0, size, value ^ signBit);
    } else if (fmov == 0x1e260000 || fmov == 0x9e660000) {
        put(cpu, rd, lane(source, 0, word >> 31 ? 8 : 4), word >> 31 ? 64 : 32);
        return true;
    } else if (fmov == 0x1e270000 || fmov == 0x9e670000) {
        setLane(result, 0, word >> 31 ? 8 : 4, reg(cpu, rn));
    } else if (fmov == 0x1e604000 || fmov == 0x1e204000) {
        const unsigned size = word & 0x400000 ? 8 : 4;
        setLane(result, 0, size, lane(source, 0, size));
    } else if ((word & 0xbfe08c00) == 0x0e000000) { // TBL/TBX.
        const unsigned tableLength = 16 * (((word >> 13) & 3) + 1);
        const bool extend = word & 0x1000;
        for (unsigned i = 0; i < width; ++i) {
            const unsigned index = other[i];
            if (index < tableLength) result[i] = cpu.v[(rn + index / 16) & 31][index % 16];
            else if (extend) result[i] = cpu.v[rd][i];
        }
    } else if ((word & 0xbfe08400) == 0x2e000000) { // EXT.
        const unsigned offset = (word >> 11) & 15;
        if (offset >= width) return false;
        for (unsigned i = 0; i < width; ++i) {
            unsigned pos = i + offset;
            result[i] = pos < width ? source[pos] : other[pos - width];
        }
    } else if (const auto move = word & 0xbfe0fc00;
               move == 0x0e003c00 || move == 0x0e002c00) { // UMOV, SMOV.
        const unsigned encoded = (word >> 16) & 31;
        if (!encoded) return false;
        size = encoded & (~encoded + 1);
        const bool signedMove = move == 0x0e002c00, wide = width == 16;
        if (signedMove ? size > (wide ? 4 : 2) : size > 8 || (size == 8) != wide) return false;
        const auto value = lane(source, encoded / (size * 2), size);
        put(cpu, rd, signedMove ? sx(value, size * 8) : value, wide ? 64 : 32);
        return true;
    } else if ((word & 0xffe0fc00) == 0x4e001c00) { // INS (general register).
        const unsigned encoded = (word >> 16) & 31;
        if (!encoded) return false;
        size = encoded & (~encoded + 1);
        if (size > 8) return false;
        result = cpu.v[rd];
        setLane(result, encoded / (size * 2), size, reg(cpu, rn));
    } else if ((word & 0xffe08400) == 0x6e000400) { // INS (element).
        const unsigned encoded = (word >> 16) & 31;
        if (!encoded) return false;
        size = encoded & (~encoded + 1);
        if (size > 8) return false;
        result = cpu.v[rd];
        setLane(result, encoded / (size * 2), size, lane(source, ((word >> 11) & 15) / size, size));
    } else if (const auto duplicate = word & 0xffe0fc00;
               duplicate == 0x0e000400 || duplicate == 0x4e000400 ||
               duplicate == 0x5e000400) { // DUP (element), including scalar MOV.
        const bool scalar = duplicate == 0x5e000400;
        const unsigned encoded = (word >> 16) & 31;
        if (!encoded) return false;
        size = encoded & (~encoded + 1);
        if (size > 8 || (!scalar && size == 8 && width == 8)) return false;
        const auto value = lane(source, encoded / (size * 2), size);
        const unsigned lanes = scalar ? 1 : width / size;
        for (unsigned i = 0; i < lanes; ++i) setLane(result, i, size, value);
    } else if (op == 0x0e206400 || op == 0x0e206c00 || op == 0x2e206400 || op == 0x2e206c00) {
        // SMAX / SMIN and UMAX / UMIN.
        if (size == 8) return false;
        const auto sign = (word & 0x20000000) ? 0 : uint64_t{1} << (size * 8 - 1);
        const bool minimum = word & 0x800;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto a = lane(source, i, size), b = lane(other, i, size);
            const bool less = (a ^ sign) < (b ^ sign);
            setLane(result, i, size, less == minimum ? a : b);
        }
    } else if (op == 0x0e209c00 || op == 0x0e209400 || op == 0x2e209400 || op == 0x0e208400 || op == 0x2e208400) {
        if (size == 8 && (width == 8 || op == 0x0e209c00 || op == 0x0e209400 || op == 0x2e209400)) return false;
        for (unsigned i = 0; i < width / size; ++i) {
            auto a = lane(source, i, size), b = lane(other, i, size);
            uint64_t value = a + b;
            if (op == 0x0e209c00) value = a * b;
            else if (op == 0x0e209400) value = lane(cpu.v[rd], i, size) + a * b;
            else if (op == 0x2e209400) value = lane(cpu.v[rd], i, size) - a * b;
            else if (op == 0x2e208400) value = a - b;
            setLane(result, i, size, value);
        }
    } else if (op == 0x2e200400) { // UHADD (vector).
        if (size == 8) return false;
        for (unsigned i = 0; i < width / size; ++i)
            setLane(result, i, size, (lane(source, i, size) + lane(other, i, size)) >> 1);
    } else if (op == 0x2e200c00) { // UQADD (vector).
        if (size == 8 && width == 8) return false;
        const auto maximum = UINT64_MAX >> (64 - size * 8);
        for (unsigned i = 0; i < width / size; ++i) {
            const auto a = lane(source, i, size), b = lane(other, i, size);
            const bool saturated = a > maximum - b;
            if (saturated) cpu.fpsr |= fpsrSaturation;
            setLane(result, i, size, saturated ? maximum : a + b);
        }
    } else if (op == 0x2e20b400) { // SQRDMULH (vector).
        if (size != 2 && size != 4) return false;
        const unsigned shift = size * 8 - 1;
        const auto maximum = (int64_t{1} << shift) - 1;
        const auto rounding = int64_t{1} << (shift - 1);
        for (unsigned i = 0; i < width / size; ++i) {
            const auto a = int64_t(sx(lane(source, i, size), size * 8));
            const auto b = int64_t(sx(lane(other, i, size), size * 8));
            const auto rounded = (a * b + rounding) >> shift;
            if (rounded > maximum) cpu.fpsr |= fpsrSaturation;
            setLane(result, i, size, std::min(rounded, maximum));
        }
    } else if (const auto comparison = op & ~0x10000000;
               comparison == 0x0e208c00 || comparison == 0x2e208c00 ||
               comparison == 0x2e203c00 || comparison == 0x2e203400 ||
               comparison == 0x0e203c00 || comparison == 0x0e203400) {
        // CMTST, CMEQ, CMHS, CMHI, CMGE and CMGT, in vector and scalar D forms.
        if (scalar ? size != 8 || width != 16 : size == 8 && width == 8) return false;
        const auto sign = (comparison & 0x20000000) ? 0 : uint64_t{1} << (size * 8 - 1);
        for (unsigned i = 0; i < (scalar ? 1 : width / size); ++i) {
            const auto a = lane(source, i, size), b = lane(other, i, size);
            bool matches;
            if (comparison == 0x0e208c00) matches = (a & b) != 0;
            else if (comparison == 0x2e208c00) matches = a == b;
            else matches = (comparison & 0x800) ? (a ^ sign) >= (b ^ sign) : (a ^ sign) > (b ^ sign);
            setLane(result, i, size, matches ? UINT64_MAX : 0);
        }
    } else if ((word & 0x9f3fbc00) == 0x0e202800) { // SADDLP/UADDLP/SADALP/UADALP.
        if (size == 8) return false;
        const auto extend = [&](uint64_t value) { return word & 0x20000000 ? value : sx(value, size * 8); };
        for (unsigned i = 0; i < width / size / 2; ++i) {
            const auto prior = word & 0x4000 ? lane(cpu.v[rd], i, size * 2) : 0;
            setLane(result, i, size * 2, prior + extend(lane(source, 2 * i, size)) +
                                                extend(lane(source, 2 * i + 1, size)));
        }
    } else if (op == 0x0e003800 || op == 0x0e007800) { // ZIP1 / ZIP2.
        if (size == 8 && width == 8) return false;
        const unsigned pairs = width / size / 2;
        const unsigned start = op == 0x0e007800 ? pairs : 0;
        for (unsigned i = 0; i < pairs; ++i) {
            setLane(result, 2 * i, size, lane(source, start + i, size));
            setLane(result, 2 * i + 1, size, lane(other, start + i, size));
        }
    } else if (op == 0x0e002800 || op == 0x0e006800) { // TRN1 / TRN2.
        if (size == 8 && width == 8) return false;
        const unsigned pairs = width / size / 2, parity = op == 0x0e006800;
        for (unsigned i = 0; i < pairs; ++i) {
            setLane(result, 2 * i, size, lane(source, 2 * i + parity, size));
            setLane(result, 2 * i + 1, size, lane(other, 2 * i + parity, size));
        }
    } else if (op == 0x0e001800 || op == 0x0e005800) { // UZP1 / UZP2.
        if (size == 8 && width == 8) return false;
        const unsigned pairs = width / size / 2, parity = op == 0x0e005800;
        for (unsigned i = 0; i < pairs; ++i) {
            setLane(result, i, size, lane(source, 2 * i + parity, size));
            setLane(result, i + pairs, size, lane(other, 2 * i + parity, size));
        }
    } else if (op == 0x0e20bc00 || op == 0x2e20a400 || op == 0x2e20ac00) { // ADDP, UMAXP, UMINP.
        if (size == 8 && (width == 8 || op != 0x0e20bc00)) return false;
        const auto reduce = [op](uint64_t a, uint64_t b) {
            if (op == 0x0e20bc00) return a + b;
            return op == 0x2e20a400 ? std::max(a, b) : std::min(a, b);
        };
        const unsigned pairs = width / size / 2;
        for (unsigned i = 0; i < pairs; ++i) {
            setLane(result, i, size, reduce(lane(source, 2 * i, size), lane(source, 2 * i + 1, size)));
            setLane(result, i + pairs, size, reduce(lane(other, 2 * i, size), lane(other, 2 * i + 1, size)));
        }
    } else if (op == 0x0e20e000) { // PMULL / PMULL2.
        if (size != 1 && size != 8) return false;
        const unsigned start = width == 16 ? 8 / size : 0;
        for (unsigned i = 0; i < 8 / size; ++i) {
            const auto a = static_cast<unsigned __int128>(lane(source, start + i, size));
            const auto b = lane(other, start + i, size);
            unsigned __int128 product = 0;
            for (unsigned bit = 0; bit < size * 8; ++bit) {
                if ((b >> bit) & 1) product ^= a << bit;
            }
            if (size == 8) std::memcpy(result.data(), &product, sizeof(product));
            else setLane(result, i, size * 2, static_cast<uint64_t>(product));
        }
    } else if ((word & 0x9f20cc00) == 0x0e200000) { // Signed/unsigned widening add and subtract.
        if (size > 4) return false;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        const bool wideSource = word & 0x1000, subtract = word & 0x2000;
        const auto narrow = [&](const auto& vector, unsigned i) {
            const auto value = lane(vector, start + i, size);
            return (word & 0x20000000) ? value : sx(value, size * 8);
        };
        for (unsigned i = 0; i < count; ++i) {
            const auto left = wideSource ? lane(source, i, size * 2) : narrow(source, i);
            const auto right = narrow(other, i);
            setLane(result, i, size * 2, subtract ? left - right : left + right);
        }
    } else if (const auto wideningProduct = word & 0x9f20fc00;
               wideningProduct == 0x0e20c000 || wideningProduct == 0x0e208000 || wideningProduct == 0x0e20a000) {
        // Signed and unsigned widening multiply, multiply-add and multiply-subtract.
        if (size > 4) return false;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        const unsigned wideSize = 2 * size;
        const auto operand = [&](const auto& vector, unsigned i) {
            const auto value = lane(vector, start + i, size);
            return (word & 0x20000000) ? value : sx(value, size * 8);
        };
        for (unsigned i = 0; i < count; ++i) {
            const auto product = operand(source, i) * operand(other, i);
            auto value = product;
            if (wideningProduct == 0x0e208000) value = lane(cpu.v[rd], i, wideSize) + product;
            if (wideningProduct == 0x0e20a000) value = lane(cpu.v[rd], i, wideSize) - product;
            setLane(result, i, wideSize, value);
        }
    } else if (op == 0x0e204400 || op == 0x2e204400) { // SSHL / USHL with signed lane shift counts.
        if (size == 8 && width == 8) return false;
        const bool signedShift = op == 0x0e204400;
        const unsigned bits = size * 8;
        for (unsigned i = 0; i < width / size; ++i) {
            const int count = static_cast<int8_t>(lane(other, i, size));
            const auto value = lane(source, i, size);
            uint64_t shifted = 0;
            if (count >= 0 && unsigned(count) < bits) {
                shifted = value << count;
            } else if (count < 0) {
                const auto distance = unsigned(-count);
                if (distance < bits) shifted = shift(value, signedShift ? 2 : 1, distance, bits);
                else if (signedShift && value >> (bits - 1)) shifted = UINT64_MAX;
            }
            setLane(result, i, size, shifted);
        }
    } else if ((word & 0xbf20fc00) == 0x2e204000) { // RADDHN / RADDHN2.
        if (size == 8) return false;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        const unsigned bits = size * 8;
        const uint64_t rounding = uint64_t{1} << (bits - 1);
        if (start) result = cpu.v[rd];
        for (unsigned i = 0; i < count; ++i) {
            const auto sum = lane(source, i, size * 2) + lane(other, i, size * 2) + rounding;
            setLane(result, start + i, size, sum >> bits);
        }
    } else if (const auto narrow = word & 0xbf3ffc00;
               narrow == 0x0e212800 || narrow == 0x2e214800) { // XTN / XTN2, UQXTN / UQXTN2.
        if (size == 8) return false;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        const auto maximum = (uint64_t{1} << (size * 8)) - 1;
        if (start) result = cpu.v[rd];
        for (unsigned i = 0; i < count; ++i) {
            auto value = lane(source, i, size * 2);
            if (narrow == 0x2e214800 && value > maximum) {
                value = maximum;
                cpu.fpsr |= fpsrSaturation;
            }
            setLane(result, start + i, size, value);
        }
    } else if ((word & 0xbf80fc00) == 0x0f008400) { // SHRN / SHRN2.
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8 || immediate >= 64) return false;
        const unsigned bits = 1U << (std::bit_width(immediate) - 1);
        size = bits / 8;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        if (start) result = cpu.v[rd];
        for (unsigned i = 0; i < count; ++i) {
            setLane(result, start + i, size, lane(source, i, size * 2) >> (2 * bits - immediate));
        }
    } else if (const auto rightShift = word & 0x9f80fc00;
               rightShift == 0x0f000400 || rightShift == 0x0f001400 ||
               rightShift == 0x0f002400 || rightShift == 0x0f003400) { // SSHR, SSRA, SRSHR, SRSRA and unsigned forms.
        const bool signedShift = !(word & 0x20000000);
        const bool accumulate = rightShift & 0x1000, round = rightShift & 0x2000;
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8) return false;
        const unsigned bits = 1U << (std::bit_width(immediate) - 1);
        if (bits == 64 && width == 8) return false;
        const unsigned amount = 2 * bits - immediate;
        size = bits / 8;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto value = lane(source, i, size);
            const auto shifted = amount == bits ? (signedShift && (value >> (bits - 1)) ? UINT64_MAX : 0)
                                                : shift(value, signedShift ? 2 : 1, amount, bits);
            const auto accumulator = accumulate ? lane(cpu.v[rd], i, size) : 0;
            const auto rounding = round ? (value >> (amount - 1)) & 1 : 0;
            setLane(result, i, size, accumulator + shifted + rounding);
        }
    } else if ((word & 0xbf80fc00) == 0x2f005400) { // SLI (vector).
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8) return false;
        const unsigned bits = std::bit_floor(immediate);
        if (bits == 64 && width == 8) return false;
        size = bits / 8;
        const auto amount = immediate - bits;
        const auto preserved = mask(amount);
        for (unsigned i = 0; i < width / size; ++i) {
            const auto destination = lane(cpu.v[rd], i, size);
            const auto shifted = lane(source, i, size) << amount;
            setLane(result, i, size, (destination & preserved) | shifted);
        }
    } else if (const bool widen = (word & 0x9f80fc00) == 0x0f00a400;
               widen || (word & 0xbf80fc00) == 0x0f005400) { // SSHLL/USHLL, SHL (immediate).
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8) return false;
        const unsigned bits = 1U << (std::bit_width(immediate) - 1);
        size = bits / 8;
        if (widen && size == 8) return false;
        const unsigned start = widen && width == 16 ? 8 / size : 0;
        const unsigned count = (widen ? 8 : width) / size;
        for (unsigned i = 0; i < count; ++i) {
            const auto value = lane(source, start + i, size);
            const auto extended = widen && !(word & 0x20000000) ? sx(value, bits) : value;
            setLane(result, i, widen ? size * 2 : size, extended << (immediate - bits));
        }
    } else {
        return false;
    }
    cpu.v[rd] = result;
    return true;
}

template <class Float>
Float floatingImmediate(unsigned immediate) {
    const int exponent = int((immediate >> 4) & 3) + ((immediate & 64) ? -3 : 1);
    const auto magnitude = std::ldexp(Float{1} + Float(immediate & 15) / 16, exponent);
    return immediate & 128 ? -magnitude : magnitude;
}

template <class Float, class UInt>
bool floatInstruction(CPU& cpu, uint32_t word) {
    unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    auto get = [&](unsigned index) { return std::bit_cast<Float>(static_cast<UInt>(lane(cpu.v[index], 0, sizeof(Float)))); };
    auto store = [&](Float value) {
        cpu.v[rd].fill(0);
        setLane(cpu.v[rd], 0, sizeof(Float), std::bit_cast<UInt>(value));
    };
    const unsigned bits = word >> 31 ? 64 : 32;
    const bool fixedToInteger = (word & 0x7fbe0000) == 0x1e180000;
    const unsigned fractionalBits = fixedToInteger ? 64 - ((word >> 10) & 63) : 0;
    if (fractionalBits > bits) return false;
    const uint32_t unary = fixedToInteger ? 0x1e380000 | (word & 0x10000) : word & 0x7fbffc00;
    const uint32_t binary = word & 0xffa0fc00;
    const auto integerInput = [&] {
        return fixedToInteger ? std::ldexp(get(rn), int(fractionalBits)) : get(rn);
    };
    if ((word & 0xffa01fe0) == 0x1e201000) { // FMOV immediate.
        store(floatingImmediate<Float>((word >> 13) & 255U));
    } else if ((word & 0xfffffc00) == 0x1e22c000 || (word & 0xfffffc00) == 0x1e624000) { // FCVT between S and D.
        const auto source = get(rn);
        cpu.v[rd].fill(0);
        if constexpr (sizeof(Float) == 4)
            setLane(cpu.v[rd], 0, 8, std::bit_cast<uint64_t>(static_cast<double>(source)));
        else
            setLane(cpu.v[rd], 0, 4, std::bit_cast<uint32_t>(static_cast<float>(source)));
    } else if ((word & 0xffa00c00) == 0x1e200c00) { // FCSEL.
        store(get(condition(cpu.nzcv, (word >> 12) & 15) ? rn : rm));
    } else if ((word & 0xffbffc00) == 0x1e20c000) { // FABS.
        store(std::abs(get(rn)));
    } else if ((word & 0xffbffc00) == 0x1e21c000) { // FSQRT.
        store(std::sqrt(get(rn)));
    } else if ((word & 0xffbffc00) == 0x1e214000) { // FNEG.
        store(-get(rn));
    } else if ((word & 0xffa00c10) == 0x1e200400) { // FCCMP.
        const auto a = get(rn), b = get(rm);
        cpu.nzcv = !condition(cpu.nzcv, (word >> 12) & 15) ? word & 15 :
            std::isunordered(a, b) ? 3 : a < b ? 8 : a == b ? 6 : 2;
    } else if ((word & 0xffbffc00) == 0x1e244000) { // FRINTN.
        store(roundToNearestEven(cpu, get(rn)));
    } else if ((word & 0xffbffc00) == 0x1e24c000) { // FRINTP.
        store(std::ceil(get(rn)));
    } else if ((word & 0xffbffc00) == 0x1e254000) { // FRINTM.
        store(std::floor(get(rn)));
    } else if ((word & 0xffbffc00) == 0x1e25c000) { // FRINTZ.
        store(std::trunc(get(rn)));
    } else if ((word & 0xffbffc00) == 0x1e264000) { // FRINTA.
        store(std::round(get(rn)));
    } else if (unary == 0x1e220000 || unary == 0x1e230000 ||
               (word & 0x7fbe0000) == 0x1e020000) { // SCVTF / UCVTF, including fixed point.
        const bool fixedPoint = !(word & 0x200000);
        const unsigned fractionalBits = fixedPoint ? 64 - ((word >> 10) & 63) : 0;
        if (fractionalBits > bits) return false;
        const auto value = reg(cpu, rn) & mask(bits);
        const int64_t signedValue = bits == 64 ? std::bit_cast<int64_t>(value) :
                                                std::bit_cast<int32_t>(uint32_t(value));
        const Float number = word & 0x10000 ? static_cast<Float>(value) : static_cast<Float>(signedValue);
        store(std::ldexp(number, -int(fractionalBits)));
    } else if (unary == 0x1e380000 || unary == 0x1e240000 ||
               unary == 0x1e280000 || unary == 0x1e300000) { // FCVTZS / FCVTAS / FCVTPS / FCVTMS.
        const auto input = integerInput();
        const auto value = unary == 0x1e240000 ? std::round(input) :
                           unary == 0x1e280000 ? std::ceil(input) :
                           unary == 0x1e300000 ? std::floor(input) : input;
        const auto bound = std::ldexp(Float{1}, bits - 1);
        const auto sign = uint64_t{1} << (bits - 1);
        const auto integer = std::isnan(value) ? 0 : value >= bound ? sign - 1 :
            value <= -bound ? sign : uint64_t(int64_t(value));
        put(cpu, rd, integer, bits);
    } else if (unary == 0x1e290000 || unary == 0x1e390000 || unary == 0x1e250000) { // FCVTPU / FCVTZU / FCVTAU.
        const Float input = integerInput();
        const Float value = unary == 0x1e290000 ? std::ceil(input) : unary == 0x1e250000 ? std::round(input) : input;
        const Float bound = std::ldexp(Float{1}, bits);
        put(cpu, rd, !(value > 0) ? 0 : value >= bound ? mask(bits) : uint64_t(value), bits);
    } else if ((word & 0xff800000) == 0x1f000000) { // FMADD / FMSUB / FNMADD / FNMSUB.
        const bool negate = word & 0x200000;
        const bool subtract = word & 0x8000;
        const auto left = get(rn), addend = get((word >> 10) & 31);
        store(std::fma(negate != subtract ? -left : left, get(rm), negate ? -addend : addend));
    } else if (binary == 0x1e208800) { // FNMUL.
        store(-(get(rn) * get(rm)));
    } else if (binary == 0x1e200800 || binary == 0x1e201800 ||
               binary == 0x1e202800 || binary == 0x1e203800) {
        const auto left = get(rn), right = get(rm);
        switch (binary) {
            case 0x1e200800: store(left * right); break;
            case 0x1e201800: store(left / right); break;
            case 0x1e202800: store(left + right); break;
            case 0x1e203800: store(left - right); break;
        }
    } else if (binary == 0x1e204800 || binary == 0x1e205800 ||
               binary == 0x1e206800 || binary == 0x1e207800) { // FMAX, FMIN, FMAXNM, FMINNM.
        store(floatingMinMax<Float, UInt>(cpu, get(rn), get(rm), word & 0x1000, word & 0x2000));
    } else if (binary == 0x1e202000 && (rd == 0 || rd == 8)) {
        const auto a = get(rn), b = rd ? 0 : get(rm);
        cpu.nzcv = std::isunordered(a, b) ? 3 : a < b ? 8 : a == b ? 6 : 2;
    } else {
        return false;
    }
    return true;
}

bool vectors(Machine& m, uint32_t word) {
    auto& cpu = m.cpu;
    unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    if ((word & 0x3f000000) == 0x1c000000) { // LDR (literal, SIMD/FP)
        const unsigned scale = word >> 30;
        if (scale == 3) return false;
        const auto address = cpu.pc + sx((word >> 5) & 0x7ffff, 19) * 4;
        vectorTransfer(m, address, rd, 4U << scale, true);
        return true;
    }
    if ((word & 0x9ff8fc00) == 0x0f00f400) { // FMOV vector immediate.
        const auto immediate = ((word >> 11) & 0xe0) | ((word >> 5) & 31);
        const bool wide = word & 0x20000000;
        const unsigned width = word & 0x40000000 ? 16 : 8;
        if (wide && width == 8) return false;
        const unsigned size = wide ? 8 : 4;
        const uint64_t value = wide ? std::bit_cast<uint64_t>(floatingImmediate<double>(immediate)) :
                                     std::bit_cast<uint32_t>(floatingImmediate<float>(immediate));
        cpu.v[rd].fill(0);
        for (unsigned i = 0; i < width / size; ++i) setLane(cpu.v[rd], i, size, value);
        return true;
    }
    const bool logicalHalfword = (word & 0x9ff8dc00) == 0x0f009400;
    const bool logicalWord = (word & 0x9ff89c00) == 0x0f001400;
    if (logicalHalfword || logicalWord) { // ORR/BIC vector immediate.
        const unsigned bytes = logicalHalfword ? 2 : 4;
        const unsigned shift = ((word >> 13) & (bytes - 1)) * 8;
        const uint32_t immediate = (((word >> 11) & 0xe0) | ((word >> 5) & 31)) << shift;
        const unsigned width = word & 0x40000000 ? 16 : 8;
        for (unsigned i = 0; i < width / bytes; ++i) {
            const auto value = lane(cpu.v[rd], i, bytes);
            setLane(cpu.v[rd], i, bytes, word & 0x20000000 ? value & ~immediate : value | immediate);
        }
        std::fill(cpu.v[rd].begin() + width, cpu.v[rd].end(), 0);
        return true;
    }
    if ((word & 0x9ff8dc00) == 0x0f008400) { // MOVI/MVNI 4H/8H with an optional byte shift.
        const auto immediate = ((word >> 11) & 0xe0) | ((word >> 5) & 31);
        auto value = immediate << (word & 0x2000 ? 8 : 0);
        if (word & 0x20000000) value = ~value;
        const unsigned lanes = word & 0x40000000 ? 8 : 4;
        cpu.v[rd].fill(0);
        for (unsigned i = 0; i < lanes; ++i) setLane(cpu.v[rd], i, 2, value);
        return true;
    }
    if (const bool shiftingOnes = (word & 0x9ff8ec00) == 0x0f00c400;
        shiftingOnes || (word & 0x9ff89c00) == 0x0f000400) { // MOVI/MVNI 2S/4S.
        const auto immediate = ((word >> 11) & 0xe0) | ((word >> 5) & 31);
        const unsigned shift = shiftingOnes ? (word & 0x1000 ? 16 : 8) : ((word >> 13) & 3) * 8;
        auto value = (immediate << shift) | (shiftingOnes ? (uint32_t{1} << shift) - 1 : 0);
        if (word & 0x20000000) value = ~value;
        const unsigned lanes = word & 0x40000000 ? 4 : 2;
        cpu.v[rd].fill(0);
        for (unsigned i = 0; i < lanes; ++i) setLane(cpu.v[rd], i, 4, value);
        return true;
    }
    if ((word & 0xbff8fc00) == 0x0f00e400) { // MOVI with a replicated byte.
        const auto value = ((word >> 11) & 0xe0) | ((word >> 5) & 31);
        cpu.v[rd].fill(0);
        std::fill_n(cpu.v[rd].begin(), word & 0x40000000 ? 16 : 8, static_cast<uint8_t>(value));
        return true;
    }
    if ((word & 0xbff8fc00) == 0x2f00e400) { // MOVI D/2D with one bit per byte.
        const auto immediate = ((word >> 11) & 0xe0) | ((word >> 5) & 31);
        const unsigned width = word & 0x40000000 ? 16 : 8;
        cpu.v[rd].fill(0);
        for (unsigned i = 0; i < width; ++i) {
            if (immediate & (1U << (i % 8))) cpu.v[rd][i] = 0xff;
        }
        return true;
    }
    if ((word & 0xbfe0fc00) == 0x0e000c00) { // DUP with a general-purpose source.
        const unsigned size = (word >> 16) & 31;
        const unsigned width = word & 0x40000000 ? 16 : 8;
        if (!std::has_single_bit(size) || size > 8 || size == width) return false;
        const uint64_t value = reg(cpu, rn);
        cpu.v[rd].fill(0);
        for (unsigned index = 0; index < width; index += size) {
            std::memcpy(cpu.v[rd].data() + index, &value, size);
        }
        return true;
    }
    const uint64_t base = reg(cpu, rn, true);
    if ((word & 0xbf200000) == 0x0c000000) { // LD/ST multiple structures.
        unsigned count = 0;
        bool interleaved = false;
        switch ((word >> 12) & 15) {
            case 0: count = 4; interleaved = true; break;
            case 4: count = 3; interleaved = true; break;
            case 8: count = 2; interleaved = true; break;
            case 7: count = 1; break;
            case 10: count = 2; break;
            case 6: count = 3; break;
            case 2: count = 4; break;
            default: return false;
        }
        const bool postIndex = word & 0x800000;
        if (!postIndex && rm) return false;
        const unsigned width = word & 0x40000000 ? 16 : 8;
        const auto offset = rm == 31 ? count * width : reg(cpu, rm);
        const bool load = word & 0x400000;
        if (!interleaved) {
            for (unsigned i = 0; i < count; ++i)
                if (!vectorTransfer(m, base + i * width, (rd + i) & 31, width, load)) return true;
        } else {
            const auto element = 1U << ((word >> 10) & 3);
            if (width == 8 && element == 8) return false;
            for (unsigned i = 0; load && i < count; ++i) cpu.v[(rd + i) & 31].fill(0);
            auto address = base;
            for (unsigned pos = 0; pos < width; pos += element) {
                for (unsigned i = 0; i < count; ++i, address += element) {
                    auto* vector = cpu.v[(rd + i) & 31].data() + pos;
                    if (load) {
                        if (!m.readInto(address, {vector, element})) {
                            m.fault(cpu.pc, address);
                            return true;
                        }
                    } else if (!m.write(address, {vector, element})) {
                        m.fault(cpu.pc, address);
                        return true;
                    }
                }
            }
        }
        if (postIndex) put(cpu, rn, base + offset, 64, true);
        return true;
    }
    if ((word & 0xbf202000) == 0x0d000000 && ((word >> 14) & 3) != 3) { // LD1/ST1 single lane.
        const unsigned scale = (word >> 14) & 3;
        const unsigned encoded = ((word >> 27) & 8) | ((word >> 10) & 7);
        const unsigned size = scale == 2 && (encoded & 1) ? 8 : 1U << scale;
        if ((scale == 1 && (encoded & 1)) || (scale == 2 && (encoded & (size == 8 ? 6 : 3))))
            return false;
        const unsigned offset = encoded & ~(size - 1);
        const bool postIndex = word & 0x800000;
        if (!postIndex && rm) return false;
        auto* element = cpu.v[rd].data() + offset;
        if (word & 0x400000) {
            if (!m.readInto(base, {element, size})) { m.fault(cpu.pc, base); return true; }
        } else if (!m.write(base, {element, size})) {
            m.fault(cpu.pc, base);
            return true;
        }
        if (postIndex) put(cpu, rn, base + (rm == 31 ? size : reg(cpu, rm)), 64, true);
        return true;
    }
    if ((word & 0xbf60f000) == 0x0d40c000) { // LD1R for one vector.
        const unsigned size = 1U << ((word >> 10) & 3);
        const bool postIndex = word & 0x800000;
        if (!postIndex && rm) return false;
        if (!vectorTransfer(m, base, rd, size, true)) return true;
        unsigned width = word & 0x40000000 ? 16 : 8;
        uint64_t value = lane(cpu.v[rd], 0, size);
        for (unsigned i = 1; i < width / size; ++i) setLane(cpu.v[rd], i, size, value);
        if (postIndex) put(cpu, rn, base + (rm == 31 ? size : reg(cpu, rm)), 64, true);
        return true;
    }
    if ((word & 0x3e000000) == 0x2c000000) {
        unsigned log = word >> 30;
        if (log == 3) return false;
        unsigned size = 4U << log;
        unsigned mode = (word >> 23) & 3;
        uint64_t offset = sx((word >> 15) & 127, 7) * size;
        uint64_t address = mode == 1 ? base : base + offset;
        bool load = word & 0x400000;
        if (vectorTransfer(m, address, rd, size, load) &&
            vectorTransfer(m, address + size, (word >> 10) & 31, size, load) &&
            (mode == 1 || mode == 3)) put(cpu, rn, base + offset, 64, true);
        return true;
    }
    if ((word & 0x3f000000) == 0x3d000000 || (word & 0x3f200000) == 0x3c000000 ||
        (word & 0x3f200c00) == 0x3c200800) {
        unsigned log = word >> 30;
        if (word & 0x800000) {
            if (log) return false;
            log = 4;
        }
        unsigned size = 1U << log;
        bool scaled = word & 0x1000000;
        bool registerOffset = (word & 0x3f200c00) == 0x3c200800;
        unsigned mode = scaled || registerOffset ? 0 : (word >> 10) & 3;
        if (mode == 2) return false;
        uint64_t offset = scaled ? ((word >> 10) & 4095) * size : sx((word >> 12) & 511, 9);
        if (registerOffset) {
            const unsigned option = (word >> 13) & 7;
            if (option == 2) offset = reg(cpu, rm) & UINT32_MAX;
            else if (option == 3 || option == 7) offset = reg(cpu, rm);
            else if (option == 6) offset = sx(reg(cpu, rm), 32);
            else return false;
            if (word & 0x1000) offset *= size;
        }
        if (vectorTransfer(m, mode == 1 ? base : base + offset, rd, size, word & 0x400000) &&
            (mode == 1 || mode == 3)) put(cpu, rn, base + offset, 64, true);
        return true;
    }
    const auto logical = word & 0xbfe0fc00;
    if ((logical & 0x9f3ffc00) == 0x0e201c00) { // Bitwise SIMD register operations.
        const unsigned size = word & 0x40000000 ? 16 : 8;
        for (unsigned i = 0; i < size; ++i) {
            const uint8_t a = cpu.v[rn][i], b = cpu.v[rm][i], dest = cpu.v[rd][i];
            switch (logical) {
                case 0x0e201c00: cpu.v[rd][i] = a & b; break;                 // AND
                case 0x0e601c00: cpu.v[rd][i] = a & ~b; break;                // BIC
                case 0x0ea01c00: cpu.v[rd][i] = a | b; break;                 // ORR
                case 0x0ee01c00: cpu.v[rd][i] = a | ~b; break;                // ORN
                case 0x2e201c00: cpu.v[rd][i] = a ^ b; break;                 // EOR
                case 0x2e601c00: cpu.v[rd][i] = (a & dest) | (b & ~dest); break; // BSL
                case 0x2ea01c00: cpu.v[rd][i] = (a & b) | (dest & ~b); break; // BIT
                case 0x2ee01c00: cpu.v[rd][i] = (a & ~b) | (dest & b); break; // BIF
            }
        }
        if (size == 8) std::fill(cpu.v[rd].begin() + 8, cpu.v[rd].end(), 0);
        return true;
    }
    return vectorArithmetic(cpu, word) ||
           (word & 0x400000 ? floatInstruction<double, uint64_t>(cpu, word)
                            : floatInstruction<float, uint32_t>(cpu, word));
}

bool values(CPU& cpu, uint32_t word) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31;
    const unsigned bits = word >> 31 ? 64 : 32;
    if ((word & 0x1f000000) == 0x10000000) {
        uint64_t offset = sx(((word >> 5) & 0x7ffff) * 4 + ((word >> 29) & 3), 21);
        uint64_t value = word >> 31 ? (cpu.pc & ~uint64_t{4095}) + offset * 4096 : cpu.pc + offset;
        put(cpu, rd, value);
    } else if ((word & 0x1f800000) == 0x12800000) {
        const unsigned shift = ((word >> 21) & 3) * 16, op = (word >> 29) & 3;
        if (shift >= bits || op == 1) return false;
        uint64_t value = uint64_t((word >> 5) & 65535) << shift;
        if (op == 0) value = ~value;
        if (op == 3) value |= reg(cpu, rd) & ~(uint64_t{65535} << shift);
        put(cpu, rd, value, bits);
    } else if ((word & 0x7fa00000) == 0x13800000) { // EXTR, including ROR immediate.
        const unsigned rm = (word >> 16) & 31;
        const unsigned amount = (word >> 10) & 63;
        if (amount >= bits || ((word >> 22) & 1) != (word >> 31)) return false;
        uint64_t low = reg(cpu, rm) & mask(bits);
        uint64_t value = amount ? (low >> amount) | (reg(cpu, rn) << (bits - amount)) : low;
        put(cpu, rd, value, bits);
    } else if ((word & 0x1f800000) == 0x13000000) {
        const unsigned r = (word >> 16) & 63, s = (word >> 10) & 63, op = (word >> 29) & 3;
        if (r >= bits || s >= bits || op == 3 || ((word >> 22) & 1) != (bits == 64)) return false;
        const uint64_t from = reg(cpu, rn) & mask(bits);
        const unsigned count = s >= r ? s - r + 1 : s + 1;
        const unsigned position = s >= r ? 0 : bits - r;
        uint64_t value = (s >= r ? from >> r : from) & mask(count);
        if (op == 0) value = sx(value, count);
        value <<= position;
        if (op == 1) value |= reg(cpu, rd) & ~(mask(count) << position);
        put(cpu, rd, value, bits);
    } else if ((word & 0x7fe00000) == 0x1b000000) {
        const unsigned rm = (word >> 16) & 31, ra = (word >> 10) & 31;
        uint64_t product = reg(cpu, rn) * reg(cpu, rm);
        put(cpu, rd, (word & 0x8000) ? reg(cpu, ra) - product : reg(cpu, ra) + product, bits);
    } else if ((word & 0xffe0fc00) == 0x9bc07c00 || (word & 0xffe0fc00) == 0x9b407c00) {
        const unsigned rm = (word >> 16) & 31;
        uint64_t a = reg(cpu, rn), b = reg(cpu, rm);
        uint64_t high = (static_cast<unsigned __int128>(a) * b) >> 64;
        if (!(word & 0x800000)) {
            if (a >> 63) high -= b;
            if (b >> 63) high -= a;
        }
        put(cpu, rd, high);
    } else if ((word & 0xffe00000) == 0x9ba00000 || (word & 0xffe00000) == 0x9b200000) {
        const unsigned rm = (word >> 16) & 31, ra = (word >> 10) & 31;
        const bool unsignedProduct = word & 0x800000;
        uint64_t a = reg(cpu, rn) & UINT32_MAX, b = reg(cpu, rm) & UINT32_MAX;
        if (!unsignedProduct) { a = sx(a, 32); b = sx(b, 32); }
        uint64_t product = a * b;
        put(cpu, rd, (word & 0x8000) ? reg(cpu, ra) - product : reg(cpu, ra) + product);
    } else if ((word & 0x3fe00410) == 0x3a400000) {
        if (condition(cpu.nzcv, (word >> 12) & 15)) {
            const unsigned rm = (word >> 16) & 31;
            uint64_t a = reg(cpu, rn) & mask(bits);
            uint64_t b = ((word & 0x800) ? rm : reg(cpu, rm)) & mask(bits);
            const bool sub = word & 0x40000000;
            arithmeticFlags(cpu, a, b, (sub ? a - b : a + b) & mask(bits), bits, sub);
        } else {
            cpu.nzcv = word & 15;
        }
    } else if ((word & 0x3fe00800) == 0x1a800000) {
        const unsigned rm = (word >> 16) & 31;
        uint64_t value = reg(cpu, rn);
        if (!condition(cpu.nzcv, (word >> 12) & 15)) {
            value = reg(cpu, rm);
            if (word & 0x40000000) value = (word & 0x400) ? -value : ~value;
            else if (word & 0x400) ++value;
        }
        put(cpu, rd, value, bits);
    } else if ((word & 0x7fe0f800) == 0x1ac00800) { // UDIV / SDIV.
        const unsigned rm = (word >> 16) & 31;
        uint64_t a = reg(cpu, rn) & mask(bits), b = reg(cpu, rm) & mask(bits);
        const bool signedDivision = word & 0x400;
        const bool negA = signedDivision && a >> (bits - 1), negB = signedDivision && b >> (bits - 1);
        if (negA) a = -a & mask(bits);
        if (negB) b = -b & mask(bits);
        const uint64_t quotient = b ? a / b : 0;
        put(cpu, rd, negA != negB ? -quotient : quotient, bits);
    } else if ((word & 0x7fe0e000) == 0x1ac04000) { // CRC32 and CRC32C.
        const unsigned size = 1U << ((word >> 10) & 3);
        if ((size == 8) != (bits == 64)) return false;
        const uint32_t polynomial = word & 0x1000 ? 0x82f63b78 : 0xedb88320;
        auto data = reg(cpu, (word >> 16) & 31);
        auto checksum = static_cast<uint32_t>(reg(cpu, rn));
        for (unsigned i = 0; i < size * 8; ++i) {
            const bool feedback = (checksum ^ data) & 1;
            checksum = (checksum >> 1) ^ (feedback ? polynomial : 0);
            data >>= 1;
        }
        put(cpu, rd, checksum, 32);
    } else if ((word & 0x7fe0f000) == 0x1ac02000) {
        const unsigned rm = (word >> 16) & 31;
        put(cpu, rd, shift(reg(cpu, rn), (word >> 10) & 3, reg(cpu, rm) & (bits - 1), bits), bits);
    } else if ((word & 0x7ffffc00) == 0x5ac00000) { // RBIT.
        const auto a = reg(cpu, rn);
        put(cpu, rd, bits == 64 ? __builtin_bitreverse64(a) : __builtin_bitreverse32(uint32_t(a)), bits);
    } else if ((word & 0x7ffffc00) == 0x5ac01000) { // CLZ.
        uint64_t a = reg(cpu, rn);
        put(cpu, rd, bits == 64 ? std::countl_zero(a) : std::countl_zero(uint32_t(a)), bits);
    } else if ((word & 0x7ffffc00) == 0x5ac00400) { // REV16 (general register).
        const auto value = reg(cpu, rn);
        constexpr uint64_t low = 0x00ff00ff00ff00ff;
        put(cpu, rd, ((value & low) << 8) | ((value >> 8) & low), bits);
    } else if ((word & 0xfffffc00) == 0xdac00800) { // REV32 (general register).
        const auto a = reg(cpu, rn);
        const auto low = __builtin_bswap32(static_cast<uint32_t>(a));
        const auto high = __builtin_bswap32(static_cast<uint32_t>(a >> 32));
        put(cpu, rd, (uint64_t(high) << 32) | low);
    } else if ((word & 0xfffffc00) == 0xdac00c00 || (word & 0xfffffc00) == 0x5ac00800) {
        uint64_t a = reg(cpu, rn);
        put(cpu, rd, bits == 64 ? __builtin_bswap64(a) : __builtin_bswap32(uint32_t(a)), bits);
    } else if ((word & 0xffffffe0) == 0xd53b0020) { // CTR_EL0
        // Instruction fetches see current guest memory. Both cache maintenance loops are unnecessary.
        constexpr uint64_t coherentCache = (1ULL << 31) | (3ULL << 28) | (4ULL << 16) | (3ULL << 14) | 4;
        put(cpu, rd, coherentCache);
    } else if ((word & 0xffffffe0) == 0xd53b00e0) {
        put(cpu, rd, 16); // DCZID_EL0.DZP. This profile does not offer DC ZVA.
    } else if ((word & 0xffffffe0) == 0xd53b4200) { // MRS <Xt>, NZCV.
        put(cpu, rd, uint64_t(cpu.nzcv) << 28);
    } else if ((word & 0xffffffe0) == 0xd51b4200) { // MSR NZCV, <Xt>.
        cpu.nzcv = (reg(cpu, rd) >> 28) & 15;
    } else if ((word & 0xffffffe0) == 0xd53b4400) {
        put(cpu, rd, 0); // FPCR for the default guest floating-point environment.
    } else if ((word & 0xffffffe0) == 0xd51b4400) {
        if (reg(cpu, rd) != 0) return false;
    } else if ((word & 0xffffffe0) == 0xd53b4420) { // MRS <Xt>, FPSR.
        put(cpu, rd, cpu.fpsr);
    } else if ((word & 0xffffffe0) == 0xd51b4420) { // MSR FPSR, <Xt>.
        cpu.fpsr = uint32_t(reg(cpu, rd)) & fpsrMask;
    } else if ((word & 0xffffffe0) == 0xd53bd040) {
        put(cpu, rd, cpu.threadPointer);
    } else if ((word & 0xffffffe0) == 0xd51bd040) {
        cpu.threadPointer = reg(cpu, rd);
    } else {
        return false;
    }
    return true;
}

} // namespace

bool executeStartupInstruction(Machine& machine, uint32_t word) {
    auto& cpu = machine.cpu;
    uint64_t next = cpu.pc + 4;
    // A64 instruction bits 28:25 select the major encoding group.
    bool handled;
    switch ((word >> 25) & 15) {
        case 4: case 12:
            handled = memory(machine, word);
            break;
        case 6: case 7: case 14: case 15:
            handled = vectors(machine, word);
            break;
        case 5:
            handled = (word & 0x01000000) ? arithmetic(cpu, word) : logic(cpu, word);
            break;
        case 8: case 13:
            handled = arithmetic(cpu, word) || values(cpu, word);
            break;
        case 9:
            handled = logic(cpu, word) || values(cpu, word);
            break;
        case 10: {
            if ((word & 0xfffff0ff) == 0xd503305f) { // CLREX
                cpu.exclusiveSize = 0;
                cpu.pc = next;
                ++cpu.retired;
                return true;
            }
            if ((word & 0xffffffe0) == 0xd50b7a20) { // DC CVAC
                const auto address = reg(cpu, word & 31);
                // The interpreter has coherent guest memory. Preserve the EL0 read access check.
                constexpr unsigned readable = 4;
                if (!machine.read(address, 1, readable))
                    machine.fault(cpu.pc, address);
                handled = true;
                break;
            }
            // Ordering is inherent in the interpreter. PAC and BTI are disabled.
            const bool barrier = word == 0xd5033b9f || // DSB ISH, used by __clear_cache.
                                 word == 0xd50339bf || word == 0xd5033abf || word == 0xd5033bbf ||
                                 word == 0xd5033dbf || word == 0xd5033ebf || word == 0xd5033fbf || word == 0xd5033fdf;
            // CHKFEAT is a NOP on the guest CPU, which does not implement FEAT_CHK.
            const bool hint = barrier || word == 0xd503251f || word == 0xd503201f ||
                              word == 0xd503245f || word == 0xd503249f ||
                              word == 0xd50324df || word == 0xd503233f || word == 0xd50323bf ||
                              word == 0xd503237f || word == 0xd50323ff || word == 0xd50320ff ||
                              word == 0xd503211f || word == 0xd503215f || word == 0xd503219f || word == 0xd50321df;
            handled = hint || branches(cpu, word, next) || values(cpu, word);
            break;
        }
        case 11:
            handled = branches(cpu, word, next);
            break;
        default:
            return false;
    }
    if (!handled) return false;
    if (cpu.stop == Stop::running) {
        cpu.pc = next;
        ++cpu.retired;
    }
    return true;
}

} // namespace nestvane
