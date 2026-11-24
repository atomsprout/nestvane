#include "AArch64.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_acle.h>
#include <arm_neon.h>
#include <sys/sysctl.h>
#endif

namespace nestvane {

namespace {

constexpr unsigned zero = 31;

constexpr std::array<uint32_t, 256> crcTable(uint32_t polynomial) {
    std::array<uint32_t, 256> table{};
    for (uint32_t byte = 0; byte < table.size(); ++byte) {
        uint32_t checksum = byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            checksum = (checksum >> 1) ^ ((checksum & 1) ? polynomial : 0);
        table[byte] = checksum;
    }
    return table;
}

#if defined(__APPLE__) && defined(__aarch64__)
__attribute__((target("crc")))
uint32_t nativeCrc32(uint32_t checksum, uint64_t data, unsigned size, bool castagnoli) {
    switch (size) {
        case 1: return castagnoli ? __crc32cb(checksum, data) : __crc32b(checksum, data);
        case 2: return castagnoli ? __crc32ch(checksum, data) : __crc32h(checksum, data);
        case 4: return castagnoli ? __crc32cw(checksum, data) : __crc32w(checksum, data);
        default: return castagnoli ? __crc32cd(checksum, data) : __crc32d(checksum, data);
    }
}
#endif

uint32_t crc32(uint32_t checksum, uint64_t data, unsigned size, bool castagnoli) {
#if defined(__APPLE__) && defined(__aarch64__)
    static const bool native = [] {
        int supported = 0;
        size_t size = sizeof(supported);
        return sysctlbyname("hw.optional.armv8_crc32", &supported, &size, nullptr, 0) == 0 &&
               size == sizeof(supported) && supported == 1;
    }();
    if (native) return nativeCrc32(checksum, data, size, castagnoli);
#endif

    static constexpr auto ieee = crcTable(0xedb88320);
    static constexpr auto castagnoliTable = crcTable(0x82f63b78);
    const auto& table = castagnoli ? castagnoliTable : ieee;
    for (unsigned i = 0; i < size; ++i) {
        checksum = (checksum >> 8) ^ table[uint8_t(checksum ^ data)];
        data >>= 8;
    }
    return checksum;
}

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
        const uint64_t target = branchTarget(cpu, reg(cpu, (word >> 5) & 31));
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
    // The atomic instructions of ARMv8.1. Each is one read and one write here; nothing else runs
    // between them on this thread.
    if ((word & 0x3fa07c00) == 0x08a07c00) { // CAS and its acquire and release forms
        const unsigned rs = (word >> 16) & 31;
        uint64_t held = 0;
        if (base % size) { machine.fault(cpu.pc, base); return true; }
        if (!machine.readInto(base, {reinterpret_cast<uint8_t*>(&held), size})) { machine.fault(cpu.pc, base); return true; }
        const uint64_t all = size == 8 ? UINT64_MAX : (uint64_t{1} << (size * 8)) - 1;
        if (held == (reg(cpu, rs) & all)) {
            const uint64_t replacement = reg(cpu, rt);
            if (!machine.write(base, {reinterpret_cast<const uint8_t*>(&replacement), size})) { machine.fault(cpu.pc, base); return true; }
        }
        put(cpu, rs, held, size == 8 ? 64 : 32);
        return true;
    }
    if ((word & 0x3f200c00) == 0x38200000 && ((word >> 12) & 15) <= 8) { // LDADD, LDCLR, LDEOR, LDSET, the four extremes, SWP
        const unsigned rs = (word >> 16) & 31, operation = (word >> 12) & 15, laneBits = size * 8;
        uint64_t held = 0;
        if (base % size) { machine.fault(cpu.pc, base); return true; }
        if (!machine.readInto(base, {reinterpret_cast<uint8_t*>(&held), size})) { machine.fault(cpu.pc, base); return true; }
        const uint64_t all = size == 8 ? UINT64_MAX : (uint64_t{1} << laneBits) - 1;
        const uint64_t operand = reg(cpu, rs) & all;
        const int64_t signedHeld = int64_t(sx(held, laneBits)), signedOperand = int64_t(sx(operand, laneBits));
        uint64_t stored;
        switch (operation) {
            case 0: stored = held + operand; break;
            case 1: stored = held & ~operand; break;
            case 2: stored = held ^ operand; break;
            case 3: stored = held | operand; break;
            case 4: stored = signedHeld > signedOperand ? held : operand; break;
            case 5: stored = signedHeld < signedOperand ? held : operand; break;
            case 6: stored = held > operand ? held : operand; break;
            case 7: stored = held < operand ? held : operand; break;
            default: stored = operand; break;
        }
        stored &= all;
        if (!machine.write(base, {reinterpret_cast<const uint8_t*>(&stored), size})) { machine.fault(cpu.pc, base); return true; }
        if (rt != 31) put(cpu, rt, held, size == 8 ? 64 : 32);
        return true;
    }
    // The second register field of a store-exclusive should be all ones, and Go's assembler leaves it zero.
    if ((word & 0x3fe00000) == 0x08000000) {
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

int64_t signedDoublingMultiplyHigh(CPU& cpu, uint64_t left, uint64_t right,
                                   unsigned bits, bool round) {
    const auto shift = bits - 1;
    const auto maximum = (int64_t{1} << shift) - 1;
    const auto rounding = round ? int64_t{1} << (shift - 1) : 0;
    const auto high = (int64_t(sx(left, bits)) * int64_t(sx(right, bits)) + rounding) >> shift;
    if (high > maximum) cpu.fpsr |= fpsrSaturation;
    return std::min(high, maximum);
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

// More of the vector instructions: the ones that apps' own libraries were found to use and the
// interpreter did not have (tools/validate-vector-instructions.cpp runs each against the processor).
// The result goes to |result|; false when the instruction is not one of these.
bool moreVector(CPU& cpu, uint32_t word, std::array<uint8_t, 16>& result) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const auto& first = cpu.v[rn];
    const auto& second = cpu.v[rm];
    const unsigned width = word & 0x40000000 ? 16 : 8;
    const bool isUnsigned = word & 0x20000000;
    const unsigned size = 1U << ((word >> 22) & 3), bits = size * 8;
    using Wide = __int128;
    const auto get = [](const std::array<uint8_t, 16>& vector, unsigned index, unsigned laneSize, bool asSigned) -> Wide {
        const uint64_t raw = lane(vector, index, laneSize);
        return asSigned ? Wide(int64_t(sx(raw, laneSize * 8))) : Wide(raw);
    };
    // The value held to what a lane of |laneBits| can hold, with the sticky flag set when it could not.
    const auto saturate = [&](Wide value, unsigned laneBits, bool toUnsigned) -> uint64_t {
        const Wide low = toUnsigned ? 0 : -(Wide(1) << (laneBits - 1));
        const Wide high = toUnsigned ? (Wide(1) << laneBits) - 1 : (Wide(1) << (laneBits - 1)) - 1;
        if (value < low) { cpu.fpsr |= fpsrSaturation; value = low; }
        if (value > high) { cpu.fpsr |= fpsrSaturation; value = high; }
        return uint64_t(value);
    };
    // A shift by a signed count, left when it is positive: what SSHL and its relatives do.
    const auto shiftBy = [&](Wide value, int count, bool round, bool saturating, bool toUnsigned) -> uint64_t {
        if (count >= 0) {
            if (!saturating) return count >= int(bits) ? 0 : uint64_t(value << count);
            if (count >= int(bits)) return value ? saturate(value > 0 ? Wide(1) << 100 : -(Wide(1) << 100), bits, toUnsigned) : 0;
            return saturate(value << count, bits, toUnsigned);
        }
        const unsigned right = unsigned(-count);
        Wide shifted;
        if (right > bits) shifted = round ? 0 : (value < 0 ? -1 : 0);
        else shifted = (value + (round ? Wide(1) << (right - 1) : 0)) >> right;
        return saturating ? saturate(shifted, bits, toUnsigned) : uint64_t(shifted);
    };

    const auto load8 = [](const std::array<uint8_t, 16>& vector) { return vld1q_u8(vector.data()); };
    const auto load32 = [](const std::array<uint8_t, 16>& vector) { return vreinterpretq_u32_u8(vld1q_u8(vector.data())); };
    const auto rotateRight = [](uint64_t value, unsigned count) { return count ? (value >> count) | (value << (64 - count)) : value; };
    // AES and SHA-1 are run by the processor's own instructions: every iPhone that runs this has them.
    if (const auto aes = word & 0xfffffc00; aes == 0x4e284800 || aes == 0x4e285800 || aes == 0x4e286800 || aes == 0x4e287800) {
        uint8x16_t value;
        switch (aes) {
            case 0x4e284800: value = vaeseq_u8(load8(cpu.v[rd]), load8(first)); break;
            case 0x4e285800: value = vaesdq_u8(load8(cpu.v[rd]), load8(first)); break;
            case 0x4e286800: value = vaesmcq_u8(load8(first)); break;
            default: value = vaesimcq_u8(load8(first)); break;
        }
        vst1q_u8(result.data(), value);
        return true;
    }
    if (const auto sha1 = word & 0xffe0fc00; sha1 == 0x5e000000 || sha1 == 0x5e001000 || sha1 == 0x5e002000 || sha1 == 0x5e003000) {
        const uint32_t e = uint32_t(lane(first, 0, 4));
        uint32x4_t value;
        switch (sha1) {
            case 0x5e000000: value = vsha1cq_u32(load32(cpu.v[rd]), e, load32(second)); break;
            case 0x5e001000: value = vsha1pq_u32(load32(cpu.v[rd]), e, load32(second)); break;
            case 0x5e002000: value = vsha1mq_u32(load32(cpu.v[rd]), e, load32(second)); break;
            default: value = vsha1su0q_u32(load32(cpu.v[rd]), load32(first), load32(second)); break;
        }
        vst1q_u8(result.data(), vreinterpretq_u8_u32(value));
        return true;
    }
    if ((word & 0xfffffc00) == 0x5e280800) { // SHA1H
        setLane(result, 0, 4, vsha1h_u32(uint32_t(lane(first, 0, 4))));
        return true;
    }
    if ((word & 0xfffffc00) == 0x5e281800) { // SHA1SU1
        vst1q_u8(result.data(), vreinterpretq_u8_u32(vsha1su1q_u32(load32(cpu.v[rd]), load32(first))));
        return true;
    }
    // SHA-512 and SHA-3 are worked out here: the iPhone XS and XR do not have the instructions.
    if (const auto sha512 = word & 0xffe0fc00; sha512 == 0xce608000 || sha512 == 0xce608400 || sha512 == 0xce608800) {
        const uint64_t w0 = lane(cpu.v[rd], 0, 8), w1 = lane(cpu.v[rd], 1, 8);
        const uint64_t x0 = lane(first, 0, 8), x1 = lane(first, 1, 8), y0 = lane(second, 0, 8), y1 = lane(second, 1, 8);
        uint64_t low, high;
        if (sha512 == 0xce608000) { // SHA512H
            const auto sigma = [&](uint64_t v) { return rotateRight(v, 14) ^ rotateRight(v, 18) ^ rotateRight(v, 41); };
            high = ((y1 & x0) ^ (~y1 & x1)) + sigma(y1) + w1;
            const uint64_t sum = high + y0;
            low = ((sum & y1) ^ (~sum & x0)) + sigma(sum) + w0;
        } else if (sha512 == 0xce608400) { // SHA512H2
            const auto sigma = [&](uint64_t v) { return rotateRight(v, 28) ^ rotateRight(v, 34) ^ rotateRight(v, 39); };
            high = ((x0 & y1) ^ (x0 & y0) ^ (y1 & y0)) + sigma(y0) + w1;
            low = ((high & y0) ^ (high & y1) ^ (y1 & y0)) + sigma(high) + w0;
        } else { // SHA512SU1
            const auto sigma = [&](uint64_t v) { return rotateRight(v, 19) ^ rotateRight(v, 61) ^ (v >> 6); };
            high = w1 + sigma(x1) + y1;
            low = w0 + sigma(x0) + y0;
        }
        setLane(result, 0, 8, low);
        setLane(result, 1, 8, high);
        return true;
    }
    if ((word & 0xfffffc00) == 0xcec08000) { // SHA512SU0
        const auto sigma = [&](uint64_t v) { return rotateRight(v, 1) ^ rotateRight(v, 8) ^ (v >> 7); };
        const uint64_t w0 = lane(cpu.v[rd], 0, 8), w1 = lane(cpu.v[rd], 1, 8);
        setLane(result, 0, 8, w0 + sigma(w1));
        setLane(result, 1, 8, w1 + sigma(lane(first, 0, 8)));
        return true;
    }
    if ((word & 0xffe08000) == 0xce000000 || (word & 0xffe08000) == 0xce200000) { // EOR3, BCAX
        const auto& third = cpu.v[(word >> 10) & 31];
        const bool clear = word & 0x00200000;
        for (unsigned i = 0; i < 16; ++i) result[i] = first[i] ^ (clear ? second[i] & ~third[i] : second[i] ^ third[i]);
        return true;
    }
    if ((word & 0xffe0fc00) == 0xce608c00) { // RAX1
        for (unsigned i = 0; i < 2; ++i) setLane(result, i, 8, lane(first, i, 8) ^ rotateRight(lane(second, i, 8), 63));
        return true;
    }
    if ((word & 0xffe00000) == 0xce800000) { // XAR
        for (unsigned i = 0; i < 2; ++i) setLane(result, i, 8, rotateRight(lane(first, i, 8) ^ lane(second, i, 8), (word >> 10) & 63));
        return true;
    }
    if ((word & 0xdf80fc00) == 0x5f005400 || (word & 0xdf80cc00) == 0x5f000400 || (word & 0xff80fc00) == 0x7f004400) {
        // Shifts of one D register by a number: SHL and SLI, the right shifts with and without rounding
        // and adding, and SRI.
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 64) return false;
        const unsigned opcode = (word >> 11) & 31;
        const uint64_t value = lane(first, 0, 8), before = lane(cpu.v[rd], 0, 8);
        uint64_t out;
        if (opcode == 0x0a) { // SHL, SLI
            const unsigned left = immediate - 64;
            out = isUnsigned ? (before & ((uint64_t{1} << left) - 1)) | (value << left) : value << left;
        } else if (opcode == 0x08) { // SRI
            const unsigned right = 128 - immediate;
            out = right == 64 ? before : (before & ~(UINT64_MAX >> right)) | (value >> right);
        } else { // SSHR, SSRA, SRSHR, SRSRA and the unsigned ones
            const unsigned right = 128 - immediate;
            const bool accumulate = opcode & 2, round = opcode & 4;
            const Wide wide = isUnsigned ? Wide(value) : Wide(int64_t(value));
            const Wide shifted = (wide + (round ? Wide(1) << (right - 1) : 0)) >> right;
            out = uint64_t(shifted) + (accumulate ? before : 0);
        }
        setLane(result, 0, 8, out);
        return true;
    }
    if ((word & 0xbf00d400) == 0x2f00d000) { // SQRDMLAH, SQRDMLSH by one element of the second operand.
        if (size != 2 && size != 4) return false;
        const bool subtract = word & 0x2000;
        const unsigned index = size == 2 ? ((word >> 9) & 4) | ((word >> 20) & 3) : ((word >> 10) & 2) | ((word >> 21) & 1);
        const auto& element = cpu.v[size == 2 ? rm & 15 : rm];
        const Wide multiplier = get(element, index, size, true);
        for (unsigned i = 0; i < width / size; ++i) {
            const Wide accumulator = get(cpu.v[rd], i, size, true) << bits;
            const Wide product = 2 * get(first, i, size, true) * multiplier;
            const Wide sum = (subtract ? accumulator - product : accumulator + product) + (Wide(1) << (bits - 1));
            setLane(result, i, size, saturate(sum >> bits, bits, false));
        }
        return true;
    }
    if ((word & 0xbfa0fc00) == 0x2e20d400) { // FADDP of whole vectors: neighbours added, first operand's then the second's.
        const bool wide = word & 0x400000;
        if (wide && width == 8) return false;
        const unsigned laneSize = wide ? 8 : 4, count = width / laneSize;
        const auto value = [&](const std::array<uint8_t, 16>& vector, unsigned index) {
            return wide ? std::bit_cast<double>(lane(vector, index, 8)) : double(std::bit_cast<float>(uint32_t(lane(vector, index, 4))));
        };
        for (unsigned i = 0; i < count; ++i) {
            const auto& from = i < count / 2 ? first : second;
            const unsigned at = (i % (count / 2)) * 2;
            if (wide) setLane(result, i, 8, std::bit_cast<uint64_t>(value(from, at) + value(from, at + 1)));
            else setLane(result, i, 4, std::bit_cast<uint32_t>(float(std::bit_cast<float>(uint32_t(lane(from, at, 4))) +
                                                                     std::bit_cast<float>(uint32_t(lane(from, at + 1, 4))))));
        }
        return true;
    }
    if ((word & 0xdf20fc00) == 0x5e204400) { // SSHL, USHL of one D register by a signed count.
        if (size != 8) return false;
        const Wide value = isUnsigned ? Wide(lane(first, 0, 8)) : Wide(int64_t(lane(first, 0, 8)));
        setLane(result, 0, 8, shiftBy(value, int8_t(second[0]), false, false, isUnsigned));
        return true;
    }
    if ((word & 0xbffffc00) == 0x2e605800) { // RBIT: the bits of each byte in the opposite order.
        for (unsigned i = 0; i < width; ++i) {
            uint8_t byte = first[i], reversed = 0;
            for (unsigned bit = 0; bit < 8; ++bit, byte >>= 1) reversed = uint8_t(reversed << 1 | (byte & 1));
            result[i] = reversed;
        }
        return true;
    }
    if ((word & 0xbffffc00) == 0x0e217800) { // FCVTL, FCVTL2 from half precision.
        const unsigned start = width == 16 ? 4 : 0;
        for (unsigned i = 0; i < 4; ++i) {
            const float value = float(std::bit_cast<_Float16>(uint16_t(lane(first, start + i, 2))));
            setLane(result, i, 4, std::bit_cast<uint32_t>(value));
        }
        return true;
    }
    if ((word & 0xff20fc00) == 0x5e208400 || (word & 0xff20fc00) == 0x7e208400) { // ADD, SUB of two D registers.
        if (size != 8) return false;
        const uint64_t a = lane(first, 0, 8), b = lane(second, 0, 8);
        setLane(result, 0, 8, word & 0x20000000 ? a - b : a + b);
        return true;
    }
    if ((word & 0x9f200400) == 0x0e200400) { // Three registers of the same kind.
        const unsigned opcode = (word >> 11) & 31;
        const bool asSigned = !isUnsigned;
        for (unsigned i = 0; i < width / size; ++i) {
            const Wide a = get(first, i, size, asSigned), b = get(second, i, size, asSigned);
            uint64_t value;
            switch (opcode) {
                case 0x00: if (size == 8) return false; value = uint64_t((a + b) >> 1); break;      // SHADD, UHADD
                case 0x01: value = saturate(a + b, bits, isUnsigned); break;                         // SQADD, UQADD
                case 0x02: if (size == 8) return false; value = uint64_t((a + b + 1) >> 1); break;  // SRHADD, URHADD
                case 0x04: if (size == 8) return false; value = uint64_t((a - b) >> 1); break;      // SHSUB, UHSUB
                case 0x05: value = saturate(a - b, bits, isUnsigned); break;                         // SQSUB, UQSUB
                case 0x09: case 0x0a: case 0x0b: {                                                   // SQSHL, SRSHL, SQRSHL and unsigned
                    const int count = int8_t(lane(second, i, 1 * size) & 0xff);
                    value = shiftBy(a, count, opcode != 0x09, opcode != 0x0a, isUnsigned);
                    break;
                }
                case 0x16: {                                                                          // SQDMULH, SQRDMULH
                    if (size != 2 && size != 4) return false;
                    const Wide x = get(first, i, size, true), y = get(second, i, size, true);
                    value = saturate((2 * x * y + (isUnsigned ? Wide(1) << (bits - 1) : 0)) >> bits, bits, false);
                    break;
                }
                default: return false;
            }
            setLane(result, i, size, value);
        }
        return true;
    }
    if ((word & 0xbf20f400) == 0x2e008400) { // SQRDMLAH, SQRDMLSH.
        if (size != 2 && size != 4) return false;
        const bool subtract = word & 0x800;
        for (unsigned i = 0; i < width / size; ++i) {
            const Wide accumulator = get(cpu.v[rd], i, size, true) << bits;
            const Wide product = 2 * get(first, i, size, true) * get(second, i, size, true);
            const Wide sum = (subtract ? accumulator - product : accumulator + product) + (Wide(1) << (bits - 1));
            setLane(result, i, size, saturate(sum >> bits, bits, false));
        }
        return true;
    }
    if ((word & 0x9f3e0c00) == 0x0e200800) { // Two registers.
        const unsigned opcode = (word >> 12) & 31;
        if (opcode == 0x01 && !isUnsigned && size == 1) { // REV16
            for (unsigned i = 0; i < width; i += 2) { result[i] = first[i + 1]; result[i + 1] = first[i]; }
            return true;
        }
        if (opcode == 0x03) { // SUQADD, USQADD: the other operand is of the other signedness.
            for (unsigned i = 0; i < width / size; ++i)
                setLane(result, i, size, saturate(get(cpu.v[rd], i, size, !isUnsigned) + get(first, i, size, isUnsigned), bits, isUnsigned));
            return true;
        }
        if (opcode == 0x07) { // SQABS, SQNEG
            for (unsigned i = 0; i < width / size; ++i) {
                const Wide value = get(first, i, size, true);
                setLane(result, i, size, saturate(isUnsigned || value < 0 ? -value : value, bits, false));
            }
            return true;
        }
        if (opcode == 0x14 || (opcode == 0x12 && isUnsigned)) { // SQXTN, UQXTN, SQXTUN, and the upper-half forms.
            if (size == 8) return false;
            const unsigned count = 8 / size, start = width == 16 ? count : 0;
            if (start) result = cpu.v[rd];
            const bool sourceSigned = opcode == 0x12 || !isUnsigned;
            for (unsigned i = 0; i < count; ++i)
                setLane(result, start + i, size, saturate(get(first, i, size * 2, sourceSigned), bits, isUnsigned));
            if (!start) std::fill(result.begin() + 8, result.end(), uint8_t{0});
            return true;
        }
        if (opcode == 0x16 && !isUnsigned) { // FCVTN and its upper-half form.
            const bool fromDouble = word & 0x400000;
            const unsigned narrow = fromDouble ? 4 : 2, count = 8 / narrow, start = width == 16 ? count : 0;
            if (start) result = cpu.v[rd];
            for (unsigned i = 0; i < count; ++i) {
                if (fromDouble) {
                    const float value = float(std::bit_cast<double>(lane(first, i, 8)));
                    setLane(result, start + i, 4, std::bit_cast<uint32_t>(value));
                } else {
                    const _Float16 value = _Float16(std::bit_cast<float>(uint32_t(lane(first, i, 4))));
                    setLane(result, start + i, 2, std::bit_cast<uint16_t>(value));
                }
            }
            return true;
        }
        if (opcode == 0x1c && !(word & 0x800000)) { // FCVTAS, FCVTAU: to the nearest whole number, halves away from zero.
            const bool wide = word & 0x400000;
            if (wide && width == 8) return false;
            const unsigned laneSize = wide ? 8 : 4;
            for (unsigned i = 0; i < width / laneSize; ++i) {
                const double value = wide ? std::bit_cast<double>(lane(first, i, 8))
                                          : double(std::bit_cast<float>(uint32_t(lane(first, i, 4))));
                uint64_t converted;
                const double rounded = std::round(value);
                const double low = isUnsigned ? 0.0 : -std::ldexp(1.0, int(laneSize * 8 - 1));
                const double high = std::ldexp(1.0, int(laneSize * 8 - (isUnsigned ? 0 : 1)));
                if (std::isnan(value)) converted = 0;
                else if (rounded < low) converted = isUnsigned ? 0 : uint64_t(1) << (laneSize * 8 - 1);
                else if (rounded >= high) converted = isUnsigned ? UINT64_MAX : (uint64_t(1) << (laneSize * 8 - 1)) - 1;
                else converted = isUnsigned ? uint64_t(rounded) : uint64_t(int64_t(rounded));
                setLane(result, i, laneSize, converted);
            }
            return true;
        }
        return false;
    }
    if ((word & 0x9f800400) == 0x0f000400) { // Shifts by a number written in the instruction.
        const unsigned opcode = (word >> 11) & 31;
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8) return false;
        const unsigned laneBits = std::bit_floor(immediate), laneSize = laneBits / 8;
        if (opcode >= 0x10 && opcode <= 0x13) { // The narrowing ones that hold the result to what fits.
            // 0x10 SQSHRUN, 0x11 SQRSHRUN (signed in, unsigned out, with U set); 0x12 and 0x13 with U set: UQSHRN, UQRSHRN.
            if (!isUnsigned || laneBits == 64) return false;
            const bool sourceSigned = opcode < 0x12, round = opcode & 1;
            const unsigned count = 8 / laneSize, start = width == 16 ? count : 0;
            const unsigned right = 2 * laneBits - immediate;
            if (start) result = cpu.v[rd];
            for (unsigned i = 0; i < count; ++i) {
                const Wide value = get(first, i, laneSize * 2, sourceSigned);
                const Wide shifted = (value + (round ? Wide(1) << (right - 1) : 0)) >> right;
                setLane(result, start + i, laneSize, saturate(shifted, laneBits, true));
            }
            if (!start) std::fill(result.begin() + 8, result.end(), uint8_t{0});
            return true;
        }
        if (opcode == 0x0e || (opcode == 0x0c && isUnsigned)) { // SQSHL, UQSHL, SQSHLU by a number.
            if (laneBits == 64 && width == 8) return false;
            const unsigned left = immediate - laneBits;
            const bool sourceSigned = opcode == 0x0c || !isUnsigned;
            for (unsigned i = 0; i < width / laneSize; ++i)
                setLane(result, i, laneSize, saturate(get(first, i, laneSize, sourceSigned) << left, laneBits, isUnsigned));
            return true;
        }
        return false;
    }
    return false;
}

// A third lot of vector instructions, found in the libraries of the fifty apps tried for 1.5
// (tools/scan-interpreter-coverage.py). The floating-point ones are run by the processor's own
// instruction on the whole register, so every bit of the result is the processor's; what the guest's
// instruction leaves alone is cleared afterwards.
bool laterVector(CPU& cpu, uint32_t word, std::array<uint8_t, 16>& result) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const auto& first = cpu.v[rn];
    const auto& second = cpu.v[rm];
    const bool q = word & 0x40000000, isUnsigned = word & 0x20000000, scalar = word & 0x10000000;
    const bool wide = word & 0x400000; // double precision, in the floating-point encodings
    const unsigned size = 1U << ((word >> 22) & 3), bits = size * 8;
    using Wide = __int128;
    const auto f32 = [](const std::array<uint8_t, 16>& vector) { return vreinterpretq_f32_u8(vld1q_u8(vector.data())); };
    const auto f64 = [](const std::array<uint8_t, 16>& vector) { return vreinterpretq_f64_u8(vld1q_u8(vector.data())); };
    const auto finish = [&](uint8x16_t value, unsigned keep) {
        vst1q_u8(result.data(), value);
        std::fill(result.begin() + keep, result.end(), uint8_t{0});
        return true;
    };
    const unsigned fpKeep = scalar ? (wide ? 8 : 4) : q ? 16 : 8;
    const bool fpShape = scalar ? q : !(wide && !q); // a scalar is written with Q set; there is no one-lane vector of doubles
    const auto get = [](const std::array<uint8_t, 16>& vector, unsigned index, unsigned laneSize, bool asSigned) -> Wide {
        const uint64_t raw = lane(vector, index, laneSize);
        return asSigned ? Wide(int64_t(sx(raw, laneSize * 8))) : Wide(raw);
    };
    const auto saturate = [&](Wide value, unsigned laneBits, bool toUnsigned) -> uint64_t {
        const Wide low = toUnsigned ? 0 : -(Wide(1) << (laneBits - 1));
        const Wide high = toUnsigned ? (Wide(1) << laneBits) - 1 : (Wide(1) << (laneBits - 1)) - 1;
        if (value < low) { cpu.fpsr |= fpsrSaturation; value = low; }
        if (value > high) { cpu.fpsr |= fpsrSaturation; value = high; }
        return uint64_t(value);
    };
#define NESTVANE_FP1(single, twice) \
    return fpShape && finish(wide ? vreinterpretq_u8_f64(twice(f64(first))) : vreinterpretq_u8_f32(single(f32(first))), fpKeep)
#define NESTVANE_FP1_TO(single, twice, to32, to64) \
    return fpShape && finish(wide ? vreinterpretq_u8_##to64(twice(f64(first))) : vreinterpretq_u8_##to32(single(f32(first))), fpKeep)
#define NESTVANE_FP2(single, twice) \
    return fpShape && finish(wide ? vreinterpretq_u8_f64(twice(f64(first), f64(second))) \
                                  : vreinterpretq_u8_f32(single(f32(first), f32(second))), fpKeep)
#define NESTVANE_FP2_TO(single, twice) \
    return fpShape && finish(wide ? vreinterpretq_u8_u64(twice(f64(first), f64(second))) \
                                  : vreinterpretq_u8_u32(single(f32(first), f32(second))), fpKeep)

    // One operand: the opcode with Q, the scalar bit and the precision taken out.
    if (((word >> 17) & 31) == 16 && (word & 0x0f000c00) == 0x0e000800) {
        const uint32_t operation = word & 0xafbffc00;
        // FRINTX and FRINTI round the way the control register says.
        const auto byMode = [&]() {
            const unsigned mode = (cpu.fpcr >> 22) & 3;
            if (wide) {
                const auto value = f64(first);
                return finish(vreinterpretq_u8_f64(mode == 0 ? vrndnq_f64(value) : mode == 1 ? vrndpq_f64(value) :
                                                   mode == 2 ? vrndmq_f64(value) : vrndq_f64(value)), fpKeep);
            }
            const auto value = f32(first);
            return finish(vreinterpretq_u8_f32(mode == 0 ? vrndnq_f32(value) : mode == 1 ? vrndpq_f32(value) :
                                               mode == 2 ? vrndmq_f32(value) : vrndq_f32(value)), fpKeep);
        };
        if (!scalar) switch (operation) {
            case 0x0e218800: NESTVANE_FP1(vrndnq_f32, vrndnq_f64); // FRINTN
            case 0x0e219800: NESTVANE_FP1(vrndmq_f32, vrndmq_f64); // FRINTM
            case 0x0ea18800: NESTVANE_FP1(vrndpq_f32, vrndpq_f64); // FRINTP
            case 0x0ea19800: NESTVANE_FP1(vrndq_f32, vrndq_f64);   // FRINTZ
            case 0x2e218800: NESTVANE_FP1(vrndaq_f32, vrndaq_f64); // FRINTA
            case 0x2e219800: case 0x2ea19800: return fpShape && byMode(); // FRINTX, FRINTI
        }
        switch (operation) {
            case 0x0ea1d800: NESTVANE_FP1(vrecpeq_f32, vrecpeq_f64);   // FRECPE
            case 0x2ea1d800: NESTVANE_FP1(vrsqrteq_f32, vrsqrteq_f64); // FRSQRTE
            case 0x0e21a800: NESTVANE_FP1_TO(vcvtnq_s32_f32, vcvtnq_s64_f64, s32, s64); // FCVTNS
            case 0x0e21b800: NESTVANE_FP1_TO(vcvtmq_s32_f32, vcvtmq_s64_f64, s32, s64); // FCVTMS
            case 0x0ea1a800: NESTVANE_FP1_TO(vcvtpq_s32_f32, vcvtpq_s64_f64, s32, s64); // FCVTPS
            case 0x0ea1b800: NESTVANE_FP1_TO(vcvtq_s32_f32, vcvtq_s64_f64, s32, s64);   // FCVTZS
            case 0x2e21a800: NESTVANE_FP1_TO(vcvtnq_u32_f32, vcvtnq_u64_f64, u32, u64); // FCVTNU
            case 0x2e21b800: NESTVANE_FP1_TO(vcvtmq_u32_f32, vcvtmq_u64_f64, u32, u64); // FCVTMU
            case 0x2ea1a800: NESTVANE_FP1_TO(vcvtpq_u32_f32, vcvtpq_u64_f64, u32, u64); // FCVTPU
            case 0x2ea1b800: NESTVANE_FP1_TO(vcvtq_u32_f32, vcvtq_u64_f64, u32, u64);   // FCVTZU
            case 0x0e21c800: NESTVANE_FP1_TO(vcvtaq_s32_f32, vcvtaq_s64_f64, s32, s64); // FCVTAS
            case 0x2e21c800: NESTVANE_FP1_TO(vcvtaq_u32_f32, vcvtaq_u64_f64, u32, u64); // FCVTAU
            case 0x0ea0c800: NESTVANE_FP1_TO(vcgtzq_f32, vcgtzq_f64, u32, u64); // FCMGT with zero
            case 0x0ea0d800: NESTVANE_FP1_TO(vceqzq_f32, vceqzq_f64, u32, u64); // FCMEQ with zero
            case 0x0ea0e800: NESTVANE_FP1_TO(vcltzq_f32, vcltzq_f64, u32, u64); // FCMLT with zero
            case 0x2ea0c800: NESTVANE_FP1_TO(vcgezq_f32, vcgezq_f64, u32, u64); // FCMGE with zero
            case 0x2ea0d800: NESTVANE_FP1_TO(vclezq_f32, vclezq_f64, u32, u64); // FCMLE with zero
        }
    }
    // Two operands of the same shape.
    if ((word & 0x0f200400) == 0x0e200400) {
        switch (word & 0xafa0fc00) {
            case 0x0e20fc00: NESTVANE_FP2(vrecpsq_f32, vrecpsq_f64);   // FRECPS
            case 0x0ea0fc00: NESTVANE_FP2(vrsqrtsq_f32, vrsqrtsq_f64); // FRSQRTS
            case 0x0e20dc00: NESTVANE_FP2(vmulxq_f32, vmulxq_f64);     // FMULX
            case 0x2e20ec00: NESTVANE_FP2_TO(vcageq_f32, vcageq_f64);  // FACGE
            case 0x2ea0ec00: NESTVANE_FP2_TO(vcagtq_f32, vcagtq_f64);  // FACGT
            case 0x0e20e400: NESTVANE_FP2_TO(vceqq_f32, vceqq_f64);    // FCMEQ
            case 0x2e20e400: NESTVANE_FP2_TO(vcgeq_f32, vcgeq_f64);    // FCMGE
            case 0x2ea0e400: NESTVANE_FP2_TO(vcgtq_f32, vcgtq_f64);    // FCMGT
        }
        if (!scalar && !q && !wide) { // Two lanes each: the pair of the first operand, then the second's.
            const auto both = vcombine_f32(vget_low_f32(f32(first)), vget_low_f32(f32(second)));
            switch (word & 0xafa0fc00) {
                case 0x2e20f400: return finish(vreinterpretq_u8_f32(vpmaxq_f32(both, both)), 8);
                case 0x2ea0f400: return finish(vreinterpretq_u8_f32(vpminq_f32(both, both)), 8);
                case 0x2e20c400: return finish(vreinterpretq_u8_f32(vpmaxnmq_f32(both, both)), 8);
                case 0x2ea0c400: return finish(vreinterpretq_u8_f32(vpminnmq_f32(both, both)), 8);
            }
        }
        if (!scalar) switch (word & 0xafa0fc00) { // Neighbours taken in pairs.
            case 0x2e20f400: NESTVANE_FP2(vpmaxq_f32, vpmaxq_f64);     // FMAXP
            case 0x2ea0f400: NESTVANE_FP2(vpminq_f32, vpminq_f64);     // FMINP
            case 0x2e20c400: NESTVANE_FP2(vpmaxnmq_f32, vpmaxnmq_f64); // FMAXNMP
            case 0x2ea0c400: NESTVANE_FP2(vpminnmq_f32, vpminnmq_f64); // FMINNMP
        }
    }
#undef NESTVANE_FP1
#undef NESTVANE_FP1_TO
#undef NESTVANE_FP2
#undef NESTVANE_FP2_TO
    // The two lanes of one register, to a scalar.
    switch (word & 0xffbffc00) {
        case 0x7e30c800: if (wide) setLane(result, 0, 8, std::bit_cast<uint64_t>(vpmaxnmqd_f64(f64(first))));
                         else setLane(result, 0, 4, std::bit_cast<uint32_t>(vpmaxnms_f32(vget_low_f32(f32(first))))); return true;
        case 0x7eb0c800: if (wide) setLane(result, 0, 8, std::bit_cast<uint64_t>(vpminnmqd_f64(f64(first))));
                         else setLane(result, 0, 4, std::bit_cast<uint32_t>(vpminnms_f32(vget_low_f32(f32(first))))); return true;
        case 0x7e30f800: if (wide) setLane(result, 0, 8, std::bit_cast<uint64_t>(vpmaxqd_f64(f64(first))));
                         else setLane(result, 0, 4, std::bit_cast<uint32_t>(vpmaxs_f32(vget_low_f32(f32(first))))); return true;
        case 0x7eb0f800: if (wide) setLane(result, 0, 8, std::bit_cast<uint64_t>(vpminqd_f64(f64(first))));
                         else setLane(result, 0, 4, std::bit_cast<uint32_t>(vpmins_f32(vget_low_f32(f32(first))))); return true;
    }
    // All four lanes, to a scalar.
    switch (word & 0xfffffc00) {
        case 0x6e30c800: setLane(result, 0, 4, std::bit_cast<uint32_t>(vmaxnmvq_f32(f32(first)))); return true; // FMAXNMV
        case 0x6eb0c800: setLane(result, 0, 4, std::bit_cast<uint32_t>(vminnmvq_f32(f32(first)))); return true; // FMINNMV
        case 0x6e30f800: setLane(result, 0, 4, std::bit_cast<uint32_t>(vmaxvq_f32(f32(first)))); return true;   // FMAXV
        case 0x6eb0f800: setLane(result, 0, 4, std::bit_cast<uint32_t>(vminvq_f32(f32(first)))); return true;   // FMINV
    }

    const uint32_t threeSame = word & 0x9f20fc00;
    if (threeSame == 0x0e205000) { // SABAL, UABAL: the difference without its sign, added in at twice the width.
        if (size == 8) return false;
        const unsigned count = 8 / size, start = q ? count : 0;
        for (unsigned i = 0; i < count; ++i) {
            const Wide difference = get(first, start + i, size, !isUnsigned) - get(second, start + i, size, !isUnsigned);
            setLane(result, i, size * 2, lane(cpu.v[rd], i, size * 2) + uint64_t(difference < 0 ? -difference : difference));
        }
        return true;
    }
    if (threeSame == 0x0e207c00) { // SABA, UABA.
        if (size == 8) return false;
        for (unsigned i = 0; i < (q ? 16 : 8) / size; ++i) {
            const Wide difference = get(first, i, size, !isUnsigned) - get(second, i, size, !isUnsigned);
            setLane(result, i, size, lane(cpu.v[rd], i, size) + uint64_t(difference < 0 ? -difference : difference));
        }
        return true;
    }
    if (threeSame == 0x0e20a400 || threeSame == 0x0e20ac00) { // SMAXP, UMAXP, SMINP, UMINP.
        if (size == 8) return false;
        const unsigned count = (q ? 16 : 8) / size;
        const bool minimum = threeSame == 0x0e20ac00;
        for (unsigned i = 0; i < count; ++i) {
            const auto& from = i < count / 2 ? first : second;
            const unsigned at = (i % (count / 2)) * 2;
            const Wide left = get(from, at, size, !isUnsigned), right = get(from, at + 1, size, !isUnsigned);
            setLane(result, i, size, uint64_t(minimum ? std::min(left, right) : std::max(left, right)));
        }
        return true;
    }
    // One lane of each register, held to what fits.
    if (const uint32_t one = word & 0xdf20fc00; one == 0x5e200c00 || one == 0x5e202c00) { // SQADD, UQADD, SQSUB, UQSUB.
        const Wide left = get(first, 0, size, !isUnsigned), right = get(second, 0, size, !isUnsigned);
        setLane(result, 0, size, saturate(one == 0x5e200c00 ? left + right : left - right, bits, isUnsigned));
        return true;
    }
    if ((word & 0xdf3ffc00) == 0x5e207800) { // SQABS, SQNEG.
        const Wide value = get(first, 0, size, true);
        setLane(result, 0, size, saturate(isUnsigned || value < 0 ? -value : value, bits, false));
        return true;
    }
    if (const uint32_t narrow = word & 0xff3ffc00; narrow == 0x5e214800 || narrow == 0x7e214800 || narrow == 0x7e212800) {
        if (size == 8) return false; // SQXTN, UQXTN, SQXTUN: half the width.
        const bool sourceSigned = narrow != 0x7e214800;
        setLane(result, 0, size, saturate(get(first, 0, size * 2, sourceSigned), bits, narrow != 0x5e214800));
        return true;
    }
    if ((word & 0xdf80e400) == 0x5f008400) { // SQSHRN, SQRSHRN, UQSHRN, UQRSHRN, SQSHRUN, SQRSHRUN of one lane.
        const unsigned immediate = (word >> 16) & 127, opcode = (word >> 11) & 31;
        if (immediate < 8 || immediate >= 64) return false;
        if (opcode < 0x10 || opcode > 0x13 || (opcode < 0x12 && !isUnsigned)) return false;
        const unsigned laneBits = std::bit_floor(immediate), right = 2 * laneBits - immediate;
        const bool round = opcode & 1, sourceSigned = opcode < 0x12 || !isUnsigned;
        const Wide value = get(first, 0, laneBits / 4, sourceSigned);
        const Wide shifted = (value + (round ? Wide(1) << (right - 1) : 0)) >> right;
        setLane(result, 0, laneBits / 8, saturate(shifted, laneBits, isUnsigned));
        return true;
    }
    // Whole numbers with a binary point to floating point, and back.
    if (const uint32_t fixed = word & 0x8f80fc00; fixed == 0x0f00e400 || fixed == 0x0f00fc00) {
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 32) return false;
        const unsigned laneBits = immediate >= 64 ? 64 : 32, laneSize = laneBits / 8;
        const int fraction = int(2 * laneBits - immediate);
        if (scalar ? !q : (laneBits == 64 && !q)) return false;
        const unsigned count = scalar ? 1 : (q ? 16 : 8) / laneSize;
        for (unsigned i = 0; i < count; ++i) {
            const uint64_t raw = lane(first, i, laneSize);
            if (fixed == 0x0f00e400) { // SCVTF, UCVTF
                if (laneBits == 32) {
                    const float number = isUnsigned ? float(uint32_t(raw)) : float(int32_t(uint32_t(raw)));
                    setLane(result, i, 4, std::bit_cast<uint32_t>(std::ldexp(number, -fraction)));
                } else {
                    const double number = isUnsigned ? double(raw) : double(int64_t(raw));
                    setLane(result, i, 8, std::bit_cast<uint64_t>(std::ldexp(number, -fraction)));
                }
            } else { // FCVTZS, FCVTZU
                const double number = laneBits == 32 ? double(std::bit_cast<float>(uint32_t(raw))) : std::bit_cast<double>(raw);
                const double scaled = std::trunc(std::ldexp(number, fraction));
                const double low = isUnsigned ? 0.0 : -std::ldexp(1.0, int(laneBits - 1));
                const double high = std::ldexp(1.0, int(laneBits - (isUnsigned ? 0 : 1)));
                uint64_t converted;
                if (std::isnan(number)) converted = 0;
                else if (scaled < low) converted = isUnsigned ? 0 : uint64_t{1} << (laneBits - 1);
                else if (scaled >= high) converted = isUnsigned ? ~uint64_t{0} : (uint64_t{1} << (laneBits - 1)) - 1;
                else converted = isUnsigned ? uint64_t(scaled) : uint64_t(int64_t(scaled));
                setLane(result, i, laneSize, converted);
            }
        }
        return true;
    }
    return false;
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
    const bool doublingHighElement = integerElement == 0x0f00c000 || integerElement == 0x0f00d000;
    const auto longElement = integerElement & ~0x20000000U;
    const bool wideningElement = longElement == 0x0f00a000 || longElement == 0x0f002000 ||
        longElement == 0x0f006000;
    if (integerElement == 0x0f008000 || integerElement == 0x2f000000 ||
        integerElement == 0x2f004000 || doublingHighElement || wideningElement) {
        // Integer multiply and its widening or saturating variants with an indexed element.
        if (size != 2 && size != 4) return false;
        const unsigned highIndex = (word >> 11) & 1;
        const unsigned lowIndex = (word >> 21) & 1;
        // In the halfword encoding, the high register bit selects a lane instead.
        const unsigned index = size == 2 ? highIndex * 4 + lowIndex * 2 + (rm >> 4) : highIndex * 2 + lowIndex;
        const auto multiplier = lane(cpu.v[size == 2 ? rm & 15 : rm], index, size);
        const unsigned lanes = (wideningElement ? 8 : width) / size;
        const unsigned first = wideningElement && width == 16 ? lanes : 0;
        for (unsigned i = 0; i < lanes; ++i) {
            const auto left = lane(source, first + i, size);
            if (wideningElement) {
                const bool unsignedOperands = word & 0x20000000;
                const auto product = unsignedOperands ? left * multiplier :
                    sx(left, size * 8) * sx(multiplier, size * 8);
                const auto accumulator = lane(cpu.v[rd], i, size * 2);
                auto value = product;
                if (longElement == 0x0f002000) value = accumulator + product;
                if (longElement == 0x0f006000) value = accumulator - product;
                setLane(result, i, size * 2, value);
                continue;
            }
            if (doublingHighElement) {
                setLane(result, i, size, signedDoublingMultiplyHigh(
                    cpu, left, multiplier, size * 8, integerElement == 0x0f00d000));
                continue;
            }
            const auto product = left * multiplier;
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
    } else if ((word & 0x9f3ffc00) == 0x0e303800) { // SADDLV, UADDLV: the lanes added up in twice their width.
        if (size == 8 || (size == 4 && width == 8)) return false;
        const bool isSigned = !(word & 0x20000000);
        uint64_t sum = 0;
        for (unsigned i = 0; i < width / size; ++i)
            sum += isSigned ? sx(lane(source, i, size), size * 8) : lane(source, i, size);
        setLane(result, 0, size * 2, sum);
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
    } else if (const auto difference = op & ~0x20000000;
               difference == 0x0e207400 || difference == 0x0e207000) { // ABD and ABDL (signed or unsigned).
        if (size == 8) return false;
        const bool widen = difference == 0x0e207000;
        const unsigned count = (widen ? 8 : width) / size;
        const unsigned first = widen && width == 16 ? count : 0;
        const auto bias = (word & 0x20000000) ? 0 : uint64_t{1} << (size * 8 - 1);
        for (unsigned i = 0; i < count; ++i) {
            const auto left = lane(source, first + i, size) ^ bias;
            const auto right = lane(other, first + i, size) ^ bias;
            setLane(result, i, widen ? size * 2 : size, left >= right ? left - right : right - left);
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
        for (unsigned i = 0; i < width / size; ++i) {
            setLane(result, i, size, signedDoublingMultiplyHigh(
                cpu, lane(source, i, size), lane(other, i, size), size * 8, true));
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
    } else if ((word & 0x9f20dc00) == 0x0e204000) { // ADDHN/SUBHN and rounded variants.
        if (size > 4) return false;
        const unsigned count = 8 / size;
        const unsigned bits = size * 8;
        const unsigned start = width == 16 ? count : 0;
        const bool subtract = word & 0x2000, rounded = word & 0x20000000;
        const uint64_t bias = rounded ? uint64_t{1} << (bits - 1) : 0;
        if (start) result = cpu.v[rd];
        for (unsigned i = 0; i < count; ++i) {
            const auto left = lane(source, i, size * 2);
            const auto right = lane(other, i, size * 2);
            const auto value = subtract ? left - right : left + right;
            setLane(result, start + i, size, (value + bias) >> bits);
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
    } else if (const auto narrow = word & 0xbf3ffc00;
               narrow == 0x0e212800 || narrow == 0x2e214800 || narrow == 0x2e212800) {
        // XTN, UQXTN and signed-to-unsigned SQXTUN, including upper-half forms.
        if (size == 8) return false;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        const auto maximum = (uint64_t{1} << (size * 8)) - 1;
        if (start) result = cpu.v[rd];
        for (unsigned i = 0; i < count; ++i) {
            auto value = lane(source, i, size * 2);
            const bool negative = narrow == 0x2e212800 && (value >> (size * 16 - 1));
            if (narrow != 0x0e212800 && (negative || value > maximum)) {
                value = negative ? 0 : maximum;
                cpu.fpsr |= fpsrSaturation;
            }
            setLane(result, start + i, size, value);
        }
    } else if (const auto narrowingShift = word & 0xbf80f400;
               narrowingShift == 0x0f008400 || narrowingShift == 0x0f009400) {
        // SHRN/RSHRN and signed SQSHRN/SQRSHRN, including the upper-half forms.
        const bool saturate = narrowingShift == 0x0f009400;
        const bool round = word & 0x800;
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8 || immediate >= 64) return false;
        const unsigned bits = 1U << (std::bit_width(immediate) - 1);
        size = bits / 8;
        const unsigned count = 8 / size;
        const unsigned start = width == 16 ? count : 0;
        if (start) result = cpu.v[rd];
        const unsigned shift = 2 * bits - immediate;
        for (unsigned i = 0; i < count; ++i) {
            const auto value = lane(source, i, size * 2);
            const auto rounding = round ? (value >> (shift - 1)) & 1 : 0;
            auto narrowed = (value >> shift) + rounding;
            if (saturate) {
                const auto signedValue = static_cast<int64_t>(sx(value, 2 * bits));
                const auto shifted = (signedValue >> shift) + static_cast<int64_t>(rounding);
                const auto limit = int64_t{1} << (bits - 1);
                const auto clamped = std::clamp(shifted, -limit, limit - 1);
                if (shifted != clamped) cpu.fpsr |= fpsrSaturation;
                narrowed = static_cast<uint64_t>(clamped);
            }
            setLane(result, start + i, size, narrowed);
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
    } else if ((word & 0xbf80fc00) == 0x2f004400) { // SRI (vector): the source shifted right, the destination's top bits kept.
        const unsigned immediate = (word >> 16) & 127;
        if (immediate < 8) return false;
        const unsigned bits = std::bit_floor(immediate);
        if (bits == 64 && width == 8) return false;
        size = bits / 8;
        const unsigned amount = 2 * bits - immediate;  // 1 to bits
        const uint64_t all = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
        const uint64_t inserted = amount == bits ? 0 : all >> amount;
        for (unsigned i = 0; i < width / size; ++i) {
            const auto destination = lane(cpu.v[rd], i, size);
            const auto shifted = amount == bits ? 0 : lane(source, i, size) >> amount;
            setLane(result, i, size, (destination & ~inserted & all) | (shifted & inserted));
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
    } else if (!moreVector(cpu, word, result)) {
        result = {};
        if (!laterVector(cpu, word, result)) return false;
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
    } else if ((word & 0xffbffc00) == 0x1e274000 || (word & 0xffbffc00) == 0x1e27c000) {
        // FRINTX and FRINTI round the way the control register says, which is to the nearest, halves to even.
        const unsigned mode = (cpu.fpcr >> 22) & 3;
        store(mode == 0 ? roundToNearestEven(cpu, get(rn)) : mode == 1 ? std::ceil(get(rn)) :
              mode == 2 ? std::floor(get(rn)) : std::trunc(get(rn)));
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
    } else if (unary == 0x1e290000 || unary == 0x1e390000 || unary == 0x1e250000 || unary == 0x1e310000) { // FCVTPU / FCVTZU / FCVTAU / FCVTMU.
        const Float input = integerInput();
        const Float value = unary == 0x1e290000 ? std::ceil(input) : unary == 0x1e250000 ? std::round(input) :
                            unary == 0x1e310000 ? std::floor(input) : input;
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
    } else if (unary == 0x1e200000 || unary == 0x1e210000) { // FCVTNS, FCVTNU: to the nearest, halves to even.
        const Float value = roundToNearestEven(cpu, get(rn));
        if (unary == 0x1e210000) {
            const Float bound = std::ldexp(Float{1}, bits);
            put(cpu, rd, !(value > 0) ? 0 : value >= bound ? mask(bits) : uint64_t(value), bits);
        } else {
            const auto bound = std::ldexp(Float{1}, bits - 1);
            const auto sign = uint64_t{1} << (bits - 1);
            put(cpu, rd, std::isnan(value) ? 0 : value >= bound ? sign - 1 : value <= -bound ? sign : uint64_t(int64_t(value)), bits);
        }
    } else if (binary == 0x1e202000 && !(rd & 7)) { // FCMP and FCMPE, with a register or with zero.
        const auto a = get(rn), b = rd & 8 ? 0 : get(rm);
        cpu.nzcv = std::isunordered(a, b) ? 3 : a < b ? 8 : a == b ? 6 : 2;
    } else {
        return false;
    }
    return true;
}

#if defined(__APPLE__) && defined(__aarch64__)
__attribute__((target("sha2")))
bool sha256(CPU& cpu, uint32_t word, unsigned rd, unsigned rn, unsigned rm) {
    const auto load = [&cpu](unsigned reg) {
        uint32x4_t value;
        std::memcpy(&value, cpu.v[reg].data(), sizeof(value));
        return value;
    };
    uint32x4_t value;
    switch (word & 0xffe0fc00) {
        case 0x5e004000: value = vsha256hq_u32(load(rd), load(rn), load(rm)); break;
        case 0x5e005000: value = vsha256h2q_u32(load(rd), load(rn), load(rm)); break;
        case 0x5e006000: value = vsha256su1q_u32(load(rd), load(rn), load(rm)); break;
        default:
            if ((word & 0xfffffc00) != 0x5e282800) return false;
            value = vsha256su0q_u32(load(rd), load(rn));
    }
    std::memcpy(cpu.v[rd].data(), &value, sizeof(value));
    return true;
}
#endif

bool vectors(Machine& m, CPU& cpu, uint32_t word) {
    unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
#if defined(__APPLE__) && defined(__aarch64__)
    if ((word & 0xff000000) == 0x5e000000 && hasNativeSha256() && sha256(cpu, word, rd, rn, rm)) return true;
#endif
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
    if ((word & 0xbf000000) == 0x0d000000 && ((word >> 14) & 3) != 3) { // LD/ST single-lane structures.
        const unsigned count = 1 + ((word >> 21) & 1) + ((word >> 12) & 2);
        const unsigned scale = (word >> 14) & 3;
        const unsigned encoded = ((word >> 27) & 8) | ((word >> 10) & 7);
        const unsigned size = scale == 2 && (encoded & 1) ? 8 : 1U << scale;
        if ((scale == 1 && (encoded & 1)) || (scale == 2 && (encoded & (size == 8 ? 6 : 3))))
            return false;
        const unsigned offset = encoded & ~(size - 1);
        const bool postIndex = word & 0x800000;
        if (!postIndex && rm) return false;
        std::array<uint8_t, 32> data;
        const auto bytes = std::span(data).first(size * count);
        if (word & 0x400000) {
            if (!m.readInto(base, bytes)) { m.fault(cpu.pc, base); return true; }
            for (unsigned i = 0; i < count; ++i)
                std::memcpy(cpu.v[(rd + i) & 31].data() + offset, bytes.data() + i * size, size);
        } else {
            for (unsigned i = 0; i < count; ++i)
                std::memcpy(bytes.data() + i * size, cpu.v[(rd + i) & 31].data() + offset, size);
            if (!m.write(base, bytes)) { m.fault(cpu.pc, base); return true; }
        }
        if (postIndex) put(cpu, rn, base + (rm == 31 ? bytes.size() : reg(cpu, rm)), 64, true);
        return true;
    }
    if ((word & 0xbf40d000) == 0x0d40c000) { // LD1R, LD2R, LD3R, LD4R: one element each, in every lane.
        const unsigned size = 1U << ((word >> 10) & 3);
        const unsigned count = 1 + ((word >> 21) & 1) + 2 * ((word >> 13) & 1);
        const bool postIndex = word & 0x800000;
        if (!postIndex && rm) return false;
        const unsigned width = word & 0x40000000 ? 16 : 8;
        // Read all of it before any register changes: a fault has to leave them as they were.
        std::array<uint8_t, 32> bytes{};
        if (!m.readInto(base, std::span(bytes).first(count * size))) { m.fault(cpu.pc, base); return true; }
        for (unsigned element = 0; element < count; ++element) {
            auto& vector = cpu.v[(rd + element) & 31];
            vector = {};
            uint64_t value = 0;
            std::memcpy(&value, bytes.data() + element * size, size);
            for (unsigned i = 0; i < width / size; ++i) setLane(vector, i, size, value);
        }
        if (postIndex) put(cpu, rn, base + (rm == 31 ? count * size : reg(cpu, rm)), 64, true);
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
    // FCVT to and from half precision, which every ARMv8 processor has even without the rest of FP16.
    if (const uint32_t half = word & 0xfffffc00; half == 0x1e23c000 || half == 0x1e63c000 || half == 0x1ee24000 || half == 0x1ee2c000) {
        const unsigned rd = word & 31, rn = (word >> 5) & 31;
        const auto source = cpu.v[rn];
        cpu.v[rd].fill(0);
        switch (half) {
            case 0x1e23c000: setLane(cpu.v[rd], 0, 2, std::bit_cast<uint16_t>(_Float16(std::bit_cast<float>(uint32_t(lane(source, 0, 4)))))); break;
            case 0x1e63c000: setLane(cpu.v[rd], 0, 2, std::bit_cast<uint16_t>(_Float16(std::bit_cast<double>(lane(source, 0, 8))))); break;
            case 0x1ee24000: setLane(cpu.v[rd], 0, 4, std::bit_cast<uint32_t>(float(std::bit_cast<_Float16>(uint16_t(lane(source, 0, 2)))))); break;
            default: setLane(cpu.v[rd], 0, 8, std::bit_cast<uint64_t>(double(std::bit_cast<_Float16>(uint16_t(lane(source, 0, 2)))))); break;
        }
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
        const auto data = reg(cpu, (word >> 16) & 31);
        const auto checksum = static_cast<uint32_t>(reg(cpu, rn));
        put(cpu, rd, crc32(checksum, data, size, word & 0x1000), 32);
    } else if ((word & 0x7fe0f000) == 0x1ac02000) {
        const unsigned rm = (word >> 16) & 31;
        put(cpu, rd, shift(reg(cpu, rn), (word >> 10) & 3, reg(cpu, rm) & (bits - 1), bits), bits);
    } else if ((word & 0x7ffffc00) == 0x5ac00000) { // RBIT.
        const auto a = reg(cpu, rn);
        put(cpu, rd, bits == 64 ? __builtin_bitreverse64(a) : __builtin_bitreverse32(uint32_t(a)), bits);
    } else if ((word & 0x7ffffc00) == 0x5ac01400) { // CLS: how many bits after the sign bit repeat it.
        const uint64_t a = reg(cpu, rn);
        put(cpu, rd, bits == 64 ? __builtin_clrsbll(int64_t(a)) : __builtin_clrsb(int32_t(uint32_t(a))), bits);
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
    } else if ((word & 0xffffffe0) == 0xd53be040 || (word & 0xffffffe0) == 0xd53be020) {
        // MRS <Xt>, CNTVCT_EL0 or CNTPCT_EL0: the host's counter, which is what translated code reads.
#if defined(__aarch64__)
        put(cpu, rd, __builtin_arm_rsr64("CNTVCT_EL0"));
#else
        return false;
#endif
    } else if ((word & 0xffffffe0) == 0xd53be000) { // MRS <Xt>, CNTFRQ_EL0.
#if defined(__aarch64__)
        put(cpu, rd, __builtin_arm_rsr64("CNTFRQ_EL0"));
#else
        return false;
#endif
    } else if ((word & 0xffffffe0) == 0xd53b4200) { // MRS <Xt>, NZCV.
        put(cpu, rd, uint64_t(cpu.nzcv) << 28);
    } else if ((word & 0xffffffe0) == 0xd51b4200) { // MSR NZCV, <Xt>.
        cpu.nzcv = (reg(cpu, rd) >> 28) & 15;
    } else if ((word & 0xffffffe0) == 0xd53b4400) {
        put(cpu, rd, cpu.fpcr); // FPCR: what the guest last wrote there.
    } else if ((word & 0xffffffe0) == 0xd51b4400) {
        // Rounding mode, flush to zero, default NaN: bits Linux lets a program set. See CPU::fpcr for
        // how little of it is acted on.
        cpu.fpcr = uint32_t(reg(cpu, rd)) & 0x07c00000;
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

bool registerInstruction(CPU& cpu, uint32_t word) {
    switch ((word >> 25) & 15) {
        case 5: return (word & 0x01000000) ? arithmetic(cpu, word) : logic(cpu, word);
        case 8: case 13: return arithmetic(cpu, word) || values(cpu, word);
        case 9: return logic(cpu, word) || values(cpu, word);
        case 7: case 15: {
            if ((word & 0xfffffc00) == 0x9eae0000) { // FMOV Xd, Vn.D[1]
                put(cpu, word & 31, lane(cpu.v[(word >> 5) & 31], 1, 8), 64);
                return true;
            }
            if ((word & 0xfffffc00) == 0x9eaf0000) { // FMOV Vd.D[1], Xn
                setLane(cpu.v[word & 31], 1, 8, reg(cpu, (word >> 5) & 31));
                return true;
            }
            // Data processing of the SIMD and floating-point registers: vectors() reaches memory only
            // for the loads and stores of groups 6 and 14, so its machine is never used here.
            static Machine none(Image{0, {}, {}});
            return vectors(none, cpu, word);
        }
        default: return false;
    }
}

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
            if ((word & 0xfffffc00) == 0x9eae0000) { // FMOV Xd, Vn.D[1]
                put(cpu, word & 31, lane(cpu.v[(word >> 5) & 31], 1, 8), 64);
                handled = true;
            } else if ((word & 0xfffffc00) == 0x9eaf0000) { // FMOV Vd.D[1], Xn
                setLane(cpu.v[word & 31], 1, 8, reg(cpu, (word >> 5) & 31));
                handled = true;
            } else {
                handled = vectors(machine, cpu, word);
            }
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
            if ((word & 0xffffffe0) == 0xd50b7420) { // DC ZVA
                // Translated code sees the host's DCZID_EL0 and zeroes memory this way; when it has to be
                // carried on here after a fault, the block is the host's: 64 bytes.
                const auto address = reg(cpu, word & 31) & ~uint64_t{63};
                static constexpr std::array<uint8_t, 64> zeros{};
                if (!machine.write(address, zeros))
                    machine.fault(cpu.pc, address);
                handled = true;
                break;
            }
            // IC IVAU, with DC CVAU what a program does after writing code of its own (__clear_cache).
            // Nothing here keeps decoded instructions, so there is nothing to throw away.
            if ((word & 0xffffffe0) == 0xd50b7520) {
                handled = true;
                break;
            }
            if ((word & 0xfffffee0) == 0xd50b7a20 || (word & 0xfffffee0) == 0xd50b7c20 ||
                (word & 0xffffffe0) == 0xd50b7e20) { // DC CVAC, CVAU, CVAP, CVADP, CIVAC
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
            // Every hint does nothing here, YIELD among them; so do the other barriers, and writing
            // PSTATE.DIT, which asks for timing that does not depend on data.
            const bool anyHint = (word & 0xfffff01f) == 0xd503201f;
            const unsigned barrierKind = (word >> 5) & 7;
            const bool anyBarrier = (word & 0xfffff01f) == 0xd503301f && (barrierKind == 4 || barrierKind == 5 || barrierKind == 6);
            const bool dit = (word & 0xfffff0ff) == 0xd503405f;
            // The identification registers read as Linux lets a program read them when it offers
            // nothing beyond the base: zero. What the guest is told it may use is in AT_HWCAP.
            if ((word & 0xffffffe0) == 0xd53b42a0) { // MRS of DIT: never set
                put(cpu, word & 31, 0, 64);
                handled = true;
                break;
            }
            if ((word & 0xffffffe0) == 0xd51b42a0) { // MSR to DIT
                handled = true;
                break;
            }
            if ((word & 0xfffff000) == 0xd5380000) {
                put(cpu, word & 31, 0, 64);
                handled = true;
                break;
            }
            const bool hint = anyHint || anyBarrier || dit || barrier || word == 0xd503251f || word == 0xd503201f ||
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
