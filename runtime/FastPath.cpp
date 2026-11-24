#include "FastPath.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace nestvane {
namespace {

constexpr uint64_t ones(unsigned bits) { return bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1; }

constexpr uint64_t signExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return ((value & ones(bits)) ^ sign) - sign;
}

constexpr uint64_t rotateRight(uint64_t value, unsigned amount, unsigned bits) {
    value &= ones(bits);
    amount %= bits;
    return amount ? ((value >> amount) | (value << (bits - amount))) & ones(bits) : value;
}

// The architecture's DecodeBitMasks: the masks of a logical immediate or of a bitfield move.
bool bitMasks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned bits, uint64_t& wmask, uint64_t& tmask) {
    const unsigned combined = (n << 6) | (~imms & 63);
    if (!combined) return false;
    unsigned length = 31 - unsigned(__builtin_clz(combined));
    if (length < 1) return false;
    const unsigned levels = (1U << length) - 1;
    if (immediate && (imms & levels) == levels) return false;
    const unsigned s = imms & levels, r = immr & levels, element = 1U << length;
    if (element > bits) return false;
    const unsigned difference = (s - r) & levels;
    const uint64_t welem = ones(s + 1), telem = ones(difference + 1);
    uint64_t w = rotateRight(welem, r, element), t = telem;
    for (unsigned at = element; at < bits; at *= 2) {
        w |= w << at;
        t |= t << at;
    }
    wmask = w & ones(bits);
    tmask = t & ones(bits);
    return true;
}

uint64_t shifted(uint64_t value, unsigned type, unsigned amount, unsigned bits) {
    value &= ones(bits);
    switch (type) {
        case 0: return (value << amount) & ones(bits);
        case 1: return value >> amount;
        case 2: return uint64_t(int64_t(signExtend(value, bits)) >> amount) & ones(bits);
        default: return rotateRight(value, amount, bits);
    }
}

uint64_t extended(uint64_t value, unsigned option, unsigned amount, unsigned bits) {
    switch (option) {
        case 0: value &= 0xff; break;
        case 1: value &= 0xffff; break;
        case 2: value &= 0xffffffff; break;
        case 4: value = signExtend(value, 8); break;
        case 5: value = signExtend(value, 16); break;
        case 6: value = signExtend(value, 32); break;
        default: break;
    }
    return (value << amount) & ones(bits);
}

bool condition(uint8_t nzcv, unsigned code) {
    const bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
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

// The architecture's AddWithCarry, for additions and (with the second operand inverted and a carry
// of one) subtractions. Sets the flags when asked.
uint64_t addWithCarry(CPU& cpu, uint64_t a, uint64_t b, bool carry, unsigned bits, bool setFlags) {
    a &= ones(bits);
    b &= ones(bits);
    const unsigned __int128 sum = (unsigned __int128)a + b + carry;
    const uint64_t result = uint64_t(sum) & ones(bits);
    if (setFlags) {
        const bool carryOut = bits == 64 ? bool(sum >> 64) : bool(uint64_t(sum) >> bits);
        const uint64_t sign = uint64_t{1} << (bits - 1);
        const bool overflow = (~(a ^ b) & (a ^ result) & sign) != 0;
        cpu.nzcv = ((result & sign) ? 8 : 0) | (result == 0 ? 4 : 0) | (carryOut ? 2 : 0) | (overflow ? 1 : 0);
    }
    return result;
}

inline uint64_t readZero(const CPU& cpu, unsigned n) { return n < 31 ? cpu.x[n] : 0; }
inline uint64_t readStack(const CPU& cpu, unsigned n) { return n < 31 ? cpu.x[n] : cpu.sp; }
inline void writeZero(CPU& cpu, unsigned n, uint64_t value, unsigned bits) {
    if (n < 31) cpu.x[n] = value & ones(bits);
}
inline void writeStack(CPU& cpu, unsigned n, uint64_t value, unsigned bits) {
    if (n < 31) cpu.x[n] = value & ones(bits);
    else cpu.sp = value & ones(bits);
}

// A load or a store of up to 8 bytes, through a window when there is one.
bool load(Machine& machine, MemoryWindows* windows, uint64_t address, unsigned size, uint64_t& value) {
    if (windows) {
        for (const auto& window : windows->read) {
            if (address - window.start < window.end - window.start && size <= window.end - address) {
                std::memcpy(&value, window.base + (address - window.start), size);
                return true;
            }
        }
    }
    if (!machine.readInto(address, {reinterpret_cast<uint8_t*>(&value), size})) return false;
    MemoryWindows::Window found;
    if (windows && machine.dataWindow(address, false, found.start, found.end, found.base)) {
        std::copy_backward(windows->read.begin(), windows->read.end() - 1, windows->read.end());
        windows->read[0] = found;
    }
    return true;
}

bool store(Machine& machine, MemoryWindows* windows, uint64_t address, unsigned size, uint64_t value) {
    if (windows) {
        for (const auto& window : windows->write) {
            if (address - window.start < window.end - window.start && size <= window.end - address) {
                std::memcpy(window.base + (address - window.start), &value, size);
                return true;
            }
        }
    }
    if (!machine.write(address, {reinterpret_cast<const uint8_t*>(&value), size})) return false;
    MemoryWindows::Window found;
    if (windows && machine.dataWindow(address, true, found.start, found.end, found.base)) {
        std::copy_backward(windows->write.begin(), windows->write.end() - 1, windows->write.end());
        windows->write[0] = found;
    }
    return true;
}

FastInstruction decodeLoadStore(uint32_t word) {
    FastInstruction f;
    if ((word >> 26) & 1) return f; // SIMD and floating-point registers
    const unsigned size = word >> 30, opc = (word >> 22) & 3;
    f.rd = word & 31;
    f.rn = (word >> 5) & 31;
    f.size = uint8_t(1U << size);
    if (opc == 0) {
        f.op = 0; // store
    } else if (opc == 1) {
        f.op = 1;
        f.bits = size == 3 ? 64 : 32;
    } else if (opc == 2) {
        if (size == 3) return {}; // prefetch
        f.op = 1;
        f.sign = 1;
        f.bits = 64;
    } else {
        if (size >= 2) return {};
        f.op = 1;
        f.sign = 1;
        f.bits = 32;
    }
    if ((word & 0x3b000000) == 0x39000000) { // unsigned offset
        f.immediate = int64_t(((word >> 10) & 0xfff) << size);
    } else if ((word & 0x3b200000) == 0x38000000) { // 9-bit offset: unscaled, unprivileged, post- or pre-index
        f.immediate = int64_t(signExtend((word >> 12) & 0x1ff, 9));
        const unsigned mode = (word >> 10) & 3;
        if (mode == 2) return {}; // LDTR, STTR: the interpreter's, which does not have them
        f.writeback = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        if (f.writeback && f.rn == f.rd && f.rn != 31) return {};
    } else if ((word & 0x3b200c00) == 0x38200800) { // register offset
        const unsigned option = (word >> 13) & 7;
        if (option != 2 && option != 3 && option != 6 && option != 7) return {};
        f.rm = (word >> 16) & 31;
        f.shift = uint8_t(option);
        f.amount = ((word >> 12) & 1) ? uint8_t(size) : 0;
        f.op |= 2; // the offset is in rm
    } else {
        return {};
    }
    f.kind = FastInstruction::loadStore;
    return f;
}

FastInstruction decodePair(uint32_t word) {
    FastInstruction f;
    if ((word >> 26) & 1) return f;
    const unsigned opc = word >> 30, mode = (word >> 23) & 3;
    const bool load = (word >> 22) & 1;
    if (opc == 3 || (opc == 1 && (!load || mode == 0))) return f;
    f.size = opc == 2 ? 8 : 4;
    f.sign = opc == 1;
    f.bits = opc == 0 ? 32 : 64;
    f.op = load;
    f.rd = word & 31;
    f.ra = (word >> 10) & 31;
    f.rn = (word >> 5) & 31;
    f.immediate = int64_t(signExtend((word >> 15) & 0x7f, 7)) * f.size;
    f.writeback = mode == 1 ? 1 : mode == 3 ? 2 : 0;
    if (load && f.rd == f.ra) return {};
    if (f.writeback && f.rn != 31 && (f.rn == f.rd || f.rn == f.ra)) return {};
    f.kind = FastInstruction::loadStorePair;
    return f;
}

} // namespace

FastInstruction decodeFast(uint32_t word) {
    FastInstruction f;
    const bool wide = word >> 31;
    f.bits = wide ? 64 : 32;
    f.rd = word & 31;
    f.rn = (word >> 5) & 31;
    f.rm = (word >> 16) & 31;
    if ((word & 0x1f800000) == 0x11000000) { // ADD, SUB (immediate), with flags or not
        f.kind = FastInstruction::addImmediate;
        f.op = (word >> 30) & 1;
        f.setFlags = (word >> 29) & 1;
        f.immediate = int64_t(((word >> 10) & 0xfff) << (((word >> 22) & 1) * 12));
        return f;
    }
    if ((word & 0x1f200000) == 0x0b000000) { // ADD, SUB (shifted register)
        const unsigned type = (word >> 22) & 3, amount = (word >> 10) & 63;
        if (type == 3 || (!wide && amount >= 32)) return {};
        f.kind = FastInstruction::addShifted;
        f.op = (word >> 30) & 1;
        f.setFlags = (word >> 29) & 1;
        f.shift = uint8_t(type);
        f.amount = uint8_t(amount);
        return f;
    }
    if ((word & 0x1fe00000) == 0x0b200000) { // ADD, SUB (extended register)
        const unsigned amount = (word >> 10) & 7;
        if (amount > 4) return {};
        f.kind = FastInstruction::addExtended;
        f.op = (word >> 30) & 1;
        f.setFlags = (word >> 29) & 1;
        f.shift = uint8_t((word >> 13) & 7);
        f.amount = uint8_t(amount);
        return f;
    }
    if ((word & 0x1f000000) == 0x0a000000) { // AND, ORR, EOR, ANDS and their inverted forms (shifted register)
        const unsigned amount = (word >> 10) & 63;
        if (!wide && amount >= 32) return {};
        f.kind = FastInstruction::logicShifted;
        f.op = uint8_t(((word >> 29) & 3) | (((word >> 21) & 1) << 2));
        f.shift = uint8_t((word >> 22) & 3);
        f.amount = uint8_t(amount);
        return f;
    }
    if ((word & 0x1f800000) == 0x12000000) { // AND, ORR, EOR, ANDS (immediate)
        const unsigned n = (word >> 22) & 1;
        if (!wide && n) return {};
        uint64_t unused;
        if (!bitMasks(n, (word >> 10) & 63, (word >> 16) & 63, true, f.bits, f.mask, unused)) return {};
        f.kind = FastInstruction::logicImmediate;
        f.op = (word >> 29) & 3;
        return f;
    }
    if ((word & 0x1f800000) == 0x12800000) { // MOVN, MOVZ, MOVK
        const unsigned opc = (word >> 29) & 3, hw = (word >> 21) & 3;
        if (opc == 1 || (!wide && hw > 1)) return {};
        f.kind = FastInstruction::moveWide;
        f.op = uint8_t(opc);
        f.amount = uint8_t(hw * 16);
        f.immediate = int64_t((word >> 5) & 0xffff);
        return f;
    }
    if ((word & 0x1f000000) == 0x10000000) { // ADR, ADRP
        f.kind = FastInstruction::address;
        f.op = word >> 31;
        f.bits = 64;
        f.immediate = int64_t(signExtend((((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3), 21));
        return f;
    }
    if ((word & 0x1f800000) == 0x13000000) { // SBFM, BFM, UBFM: shifts, extensions and bitfield moves
        const unsigned opc = (word >> 29) & 3, n = (word >> 22) & 1, immr = (word >> 16) & 63, imms = (word >> 10) & 63;
        if (opc == 3 || n != unsigned(wide) || (!wide && (immr >= 32 || imms >= 32))) return {};
        if (!bitMasks(n, imms, immr, false, f.bits, f.mask, f.mask2)) return {};
        f.kind = FastInstruction::bitfield;
        f.op = uint8_t(opc);
        f.shift = uint8_t(immr);
        f.amount = uint8_t(imms);
        return f;
    }
    if ((word & 0x3fe00000) == 0x1a800000) { // CSEL, CSINC, CSINV, CSNEG
        const unsigned op2 = (word >> 10) & 3;
        if (op2 > 1) return {};
        f.kind = FastInstruction::select;
        f.op = uint8_t((((word >> 30) & 1) << 1) | op2);
        f.shift = uint8_t((word >> 12) & 15);
        return f;
    }
    if ((word & 0x7fe00000) == 0x1b000000) { // MADD, MSUB
        f.kind = FastInstruction::multiplyAdd;
        f.op = (word >> 15) & 1;
        f.ra = (word >> 10) & 31;
        return f;
    }
    if ((word & 0x7fe00000) == 0x1ac00000) { // UDIV, SDIV, LSLV, LSRV, ASRV, RORV
        const unsigned opcode = (word >> 10) & 63;
        if (opcode == 2 || opcode == 3) {
            f.kind = FastInstruction::divide;
            f.op = uint8_t(opcode & 1);
        } else if (opcode >= 8 && opcode <= 11) {
            f.kind = FastInstruction::shiftVariable;
            f.op = uint8_t(opcode - 8);
        }
        return f.kind == FastInstruction::none ? FastInstruction{} : f;
    }
    if ((word & 0x7c000000) == 0x14000000) { // B, BL
        f.kind = FastInstruction::branch;
        f.op = word >> 31;
        f.immediate = int64_t(signExtend(word & 0x3ffffff, 26)) * 4;
        return f;
    }
    if ((word & 0xff000010) == 0x54000000) { // B.cond
        f.kind = FastInstruction::branchCondition;
        f.shift = uint8_t(word & 15);
        f.immediate = int64_t(signExtend((word >> 5) & 0x7ffff, 19)) * 4;
        return f;
    }
    if ((word & 0x7e000000) == 0x34000000) { // CBZ, CBNZ
        f.kind = FastInstruction::compareBranch;
        f.op = (word >> 24) & 1;
        f.immediate = int64_t(signExtend((word >> 5) & 0x7ffff, 19)) * 4;
        return f;
    }
    if ((word & 0x7e000000) == 0x36000000) { // TBZ, TBNZ
        f.kind = FastInstruction::testBranch;
        f.op = (word >> 24) & 1;
        f.amount = uint8_t(((word >> 26) & 32) | ((word >> 19) & 31));
        f.immediate = int64_t(signExtend((word >> 5) & 0x3fff, 14)) * 4;
        return f;
    }
    if (const uint32_t form = word & 0xfffffc1f; form == 0xd61f0000 || form == 0xd63f0000 || form == 0xd65f0000) {
        f.kind = FastInstruction::branchRegister; // BR, BLR, RET
        f.op = form == 0xd63f0000;
        return f;
    }
    if ((word & 0x3a000000) == 0x38000000) return decodeLoadStore(word);
    if ((word & 0x3a000000) == 0x28000000) return decodePair(word);
    if ((word & 0x3b000000) == 0x18000000) { // LDR (literal), LDRSW (literal)
        const unsigned opc = word >> 30;
        if (opc == 3 || ((word >> 26) & 1)) return {};
        f.kind = FastInstruction::loadLiteral;
        f.size = opc == 1 ? 8 : 4;
        f.sign = opc == 2;
        f.bits = opc == 0 ? 32 : 64;
        f.immediate = int64_t(signExtend((word >> 5) & 0x7ffff, 19)) * 4;
        return f;
    }
    return {};
}

void executeFast(Machine& machine, const FastInstruction& f, MemoryWindows* windows) {
    auto& cpu = machine.cpu;
    const unsigned bits = f.bits;
    uint64_t next = cpu.pc + 4;
    switch (f.kind) {
        case FastInstruction::none:
            return;
        case FastInstruction::addImmediate: {
            const uint64_t a = readStack(cpu, f.rn);
            const uint64_t result = f.op ? addWithCarry(cpu, a, ~uint64_t(f.immediate), true, bits, f.setFlags)
                                         : addWithCarry(cpu, a, uint64_t(f.immediate), false, bits, f.setFlags);
            if (f.setFlags) writeZero(cpu, f.rd, result, bits);
            else writeStack(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::addShifted: {
            const uint64_t a = readZero(cpu, f.rn), b = shifted(readZero(cpu, f.rm), f.shift, f.amount, bits);
            const uint64_t result = f.op ? addWithCarry(cpu, a, ~b, true, bits, f.setFlags)
                                         : addWithCarry(cpu, a, b, false, bits, f.setFlags);
            writeZero(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::addExtended: {
            const uint64_t a = readStack(cpu, f.rn), b = extended(readZero(cpu, f.rm), f.shift, f.amount, bits);
            const uint64_t result = f.op ? addWithCarry(cpu, a, ~b, true, bits, f.setFlags)
                                         : addWithCarry(cpu, a, b, false, bits, f.setFlags);
            if (f.setFlags) writeZero(cpu, f.rd, result, bits);
            else writeStack(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::logicShifted: {
            const uint64_t a = readZero(cpu, f.rn) & ones(bits);
            uint64_t b = shifted(readZero(cpu, f.rm), f.shift, f.amount, bits);
            if (f.op & 4) b = ~b & ones(bits);
            uint64_t result;
            switch (f.op & 3) {
                case 0: result = a & b; break;
                case 1: result = a | b; break;
                case 2: result = a ^ b; break;
                default:
                    result = a & b;
                    cpu.nzcv = ((result >> (bits - 1)) & 1 ? 8 : 0) | (result == 0 ? 4 : 0);
                    break;
            }
            writeZero(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::logicImmediate: {
            const uint64_t a = readZero(cpu, f.rn) & ones(bits);
            uint64_t result;
            switch (f.op) {
                case 0: result = a & f.mask; break;
                case 1: result = a | f.mask; break;
                case 2: result = a ^ f.mask; break;
                default:
                    result = a & f.mask;
                    cpu.nzcv = ((result >> (bits - 1)) & 1 ? 8 : 0) | (result == 0 ? 4 : 0);
                    writeZero(cpu, f.rd, result, bits);
                    cpu.pc = next;
                    ++cpu.retired;
                    return;
            }
            writeStack(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::moveWide: {
            const uint64_t part = uint64_t(f.immediate) << f.amount;
            if (f.op == 0) writeZero(cpu, f.rd, ~part, bits);
            else if (f.op == 2) writeZero(cpu, f.rd, part, bits);
            else writeZero(cpu, f.rd, (readZero(cpu, f.rd) & ~(uint64_t{0xffff} << f.amount)) | part, bits);
            break;
        }
        case FastInstruction::address:
            writeZero(cpu, f.rd, f.op ? (cpu.pc & ~uint64_t{0xfff}) + (uint64_t(f.immediate) << 12)
                                      : cpu.pc + uint64_t(f.immediate), 64);
            break;
        case FastInstruction::bitfield: {
            const uint64_t source = readZero(cpu, f.rn) & ones(bits);
            const uint64_t rotated = rotateRight(source, f.shift, bits);
            uint64_t result;
            if (f.op == 2) {
                result = rotated & f.mask & f.mask2;
            } else if (f.op == 0) {
                const uint64_t top = (source >> f.amount) & 1 ? ones(bits) : 0;
                result = (top & ~f.mask2) | (rotated & f.mask & f.mask2);
            } else {
                const uint64_t destination = readZero(cpu, f.rd) & ones(bits);
                const uint64_t bottom = (destination & ~f.mask) | (rotated & f.mask);
                result = (destination & ~f.mask2) | (bottom & f.mask2);
            }
            writeZero(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::select: {
            uint64_t result;
            if (condition(cpu.nzcv, f.shift)) {
                result = readZero(cpu, f.rn);
            } else {
                const uint64_t value = readZero(cpu, f.rm);
                switch (f.op) {
                    case 0: result = value; break;
                    case 1: result = value + 1; break;
                    case 2: result = ~value; break;
                    default: result = uint64_t(0) - value; break;
                }
            }
            writeZero(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::multiplyAdd: {
            const uint64_t product = readZero(cpu, f.rn) * readZero(cpu, f.rm);
            writeZero(cpu, f.rd, f.op ? readZero(cpu, f.ra) - product : readZero(cpu, f.ra) + product, bits);
            break;
        }
        case FastInstruction::divide: {
            const uint64_t a = readZero(cpu, f.rn) & ones(bits), b = readZero(cpu, f.rm) & ones(bits);
            uint64_t result = 0;
            if (b) {
                if (f.op) {
                    const int64_t sa = int64_t(signExtend(a, bits)), sb = int64_t(signExtend(b, bits));
                    result = (sb == -1) ? uint64_t(0) - uint64_t(sa) : uint64_t(sa / sb);
                } else {
                    result = a / b;
                }
            }
            writeZero(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::shiftVariable:
            writeZero(cpu, f.rd, shifted(readZero(cpu, f.rn), f.op, unsigned(readZero(cpu, f.rm) % bits), bits), bits);
            break;
        case FastInstruction::branch:
            if (f.op) cpu.x[30] = next;
            next = cpu.pc + uint64_t(f.immediate);
            break;
        case FastInstruction::branchCondition:
            if (condition(cpu.nzcv, f.shift)) next = cpu.pc + uint64_t(f.immediate);
            break;
        case FastInstruction::compareBranch:
            if (bool(readZero(cpu, f.rd) & ones(bits)) == bool(f.op)) next = cpu.pc + uint64_t(f.immediate);
            break;
        case FastInstruction::testBranch:
            if (bool((readZero(cpu, f.rd) >> f.amount) & 1) == bool(f.op)) next = cpu.pc + uint64_t(f.immediate);
            break;
        case FastInstruction::branchRegister: {
            const uint64_t target = readZero(cpu, f.rn);
            if (f.op) cpu.x[30] = next;
            next = target;
            break;
        }
        case FastInstruction::loadStore: {
            const uint64_t base = readStack(cpu, f.rn);
            const uint64_t offset = (f.op & 2) ? extended(readZero(cpu, f.rm), f.shift, f.amount, 64) : uint64_t(f.immediate);
            const uint64_t address = f.writeback == 1 ? base : base + offset;
            if (f.op & 1) {
                uint64_t value = 0;
                if (!load(machine, windows, address, f.size, value)) {
                    machine.fault(cpu.pc, address);
                    return;
                }
                if (f.writeback) writeStack(cpu, f.rn, base + offset, 64);
                writeZero(cpu, f.rd, f.sign ? signExtend(value, f.size * 8) : value, bits);
            } else {
                const uint64_t value = readZero(cpu, f.rd);
                if (!store(machine, windows, address, f.size, value)) {
                    machine.fault(cpu.pc, address);
                    return;
                }
                if (f.writeback) writeStack(cpu, f.rn, base + offset, 64);
            }
            break;
        }
        case FastInstruction::loadStorePair: {
            // Two accesses one after the other, as the interpreter makes them: when the second faults,
            // the first has happened.
            const uint64_t base = readStack(cpu, f.rn);
            const uint64_t address = f.writeback == 1 ? base : base + uint64_t(f.immediate);
            const unsigned registers[2] = {f.rd, f.ra};
            for (unsigned i = 0; i < 2; ++i) {
                const uint64_t at = address + i * f.size;
                if (f.op) {
                    uint64_t value = 0;
                    if (!load(machine, windows, at, f.size, value)) {
                        machine.fault(cpu.pc, at);
                        return;
                    }
                    writeZero(cpu, registers[i], f.sign ? signExtend(value, 32) : value, bits);
                } else {
                    const uint64_t value = readZero(cpu, registers[i]);
                    if (!store(machine, windows, at, f.size, value)) {
                        machine.fault(cpu.pc, at);
                        return;
                    }
                }
            }
            if (f.writeback) writeStack(cpu, f.rn, base + uint64_t(f.immediate), 64);
            break;
        }
        case FastInstruction::loadLiteral: {
            const uint64_t address = cpu.pc + uint64_t(f.immediate);
            uint64_t value = 0;
            if (!machine.readInto(address, {reinterpret_cast<uint8_t*>(&value), f.size})) {
                machine.fault(cpu.pc, address);
                return;
            }
            writeZero(cpu, f.rd, f.sign ? signExtend(value, 32) : value, bits);
            break;
        }
    }
    cpu.pc = next;
    ++cpu.retired;
}

} // namespace nestvane
