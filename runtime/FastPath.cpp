#include "FastPath.hpp"
#include "AArch64.hpp"
#include "GuestSpace.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <bit>
#include <cmath>
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

// Loads and stores of one SIMD and floating-point register: B, H, S, D or Q. A load writes the whole
// register, zeros above what it loads.
FastInstruction decodeVectorLoadStore(uint32_t word) {
    FastInstruction f;
    const unsigned size = word >> 30, opc = (word >> 22) & 3;
    if (opc >= 2 && size) return f;              // only Q has the high bit of opc
    f.size = uint8_t(opc >= 2 ? 16 : 1U << size);
    f.op = opc & 1;                               // 1: load
    f.rd = word & 31;
    f.rn = (word >> 5) & 31;
    const unsigned scale = f.size == 16 ? 4 : size;
    if ((word & 0x3b000000) == 0x39000000) {
        f.immediate = int64_t(((word >> 10) & 0xfff) << scale);
    } else if ((word & 0x3b200000) == 0x38000000) {
        f.immediate = int64_t(signExtend((word >> 12) & 0x1ff, 9));
        const unsigned mode = (word >> 10) & 3;
        if (mode == 2) return {};
        f.writeback = mode == 1 ? 1 : mode == 3 ? 2 : 0;
    } else if ((word & 0x3b200c00) == 0x38200800) {
        const unsigned option = (word >> 13) & 7;
        if (option != 2 && option != 3 && option != 6 && option != 7) return {};
        f.rm = (word >> 16) & 31;
        f.shift = uint8_t(option);
        f.amount = ((word >> 12) & 1) ? uint8_t(scale) : 0;
        f.op |= 2;
    } else {
        return {};
    }
    f.kind = FastInstruction::vectorLoadStore;
    return f;
}

FastInstruction decodeVectorPair(uint32_t word) {
    FastInstruction f;
    const unsigned opc = word >> 30, mode = (word >> 23) & 3;
    if (opc == 3 || mode == 0) return f;          // non-temporal pairs: left to the interpreter
    f.size = uint8_t(4U << opc);
    f.op = (word >> 22) & 1;
    f.rd = word & 31;
    f.ra = (word >> 10) & 31;
    f.rn = (word >> 5) & 31;
    f.immediate = int64_t(signExtend((word >> 15) & 0x7f, 7)) * f.size;
    f.writeback = mode == 1 ? 1 : mode == 3 ? 2 : 0;
    if (f.op && f.rd == f.ra) return {};
    f.kind = FastInstruction::vectorPair;
    return f;
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
    if ((word & 0xff000000) == 0x9b000000 && ((word >> 21) & 7) != 0) { // SMADDL, SMSUBL, SMULH, UMADDL, UMSUBL, UMULH
        const unsigned op31 = (word >> 21) & 7, o0 = (word >> 15) & 1;
        if (op31 != 1 && op31 != 2 && op31 != 5 && op31 != 6) return {};
        if ((op31 == 2 || op31 == 6) && (o0 || ((word >> 10) & 31) != 31)) return {};
        f.kind = FastInstruction::multiplyLong;
        f.op = uint8_t(op31);
        f.sign = o0; // subtract, for the long forms
        f.ra = (word >> 10) & 31;
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
    if ((word & 0xfffff01f) == 0xd503201f) { // every hint: NOP, YIELD, BTI, the PAC hints
        f.kind = FastInstruction::hint;
        return f;
    }
    if ((word & 0xfffff01f) == 0xd503301f && ((word >> 5) & 7) >= 4 && ((word >> 5) & 7) <= 6) { // DSB, DMB, ISB
        f.kind = FastInstruction::barrier;
        return f;
    }
    if ((word & 0xffffffe0) == 0xd53bd040 || (word & 0xffffffe0) == 0xd51bd040) { // MRS, MSR TPIDR_EL0
        f.kind = FastInstruction::threadPointer;
        f.op = (word >> 21) & 1; // 1: read
        return f;
    }
    if ((word & 0x3fe00410) == 0x3a400000) { // CCMN, CCMP (register or immediate)
        f.kind = FastInstruction::conditionalCompare;
        f.op = (word >> 30) & 1; // 1: CCMP
        f.shift = uint8_t((word >> 12) & 15);
        f.amount = uint8_t(word & 15);
        f.setFlags = (word >> 11) & 1; // 1: the immediate form
        return f;
    }
    if ((word & 0x7fff0000) == 0x5ac00000) { // RBIT, REV16, REV32, REV, CLZ, CLS
        const unsigned opcode = (word >> 10) & 63;
        if (opcode > 5 || (!wide && opcode == 3)) return {};
        f.kind = FastInstruction::singleSource;
        f.op = uint8_t(opcode);
        return f;
    }
    if ((word & 0x3a000000) == 0x38000000 && ((word >> 26) & 1)) return decodeVectorLoadStore(word);
    if ((word & 0x3a000000) == 0x28000000 && ((word >> 26) & 1)) return decodeVectorPair(word);
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
    // Scalar floating point, as the interpreter does it (AArch64.cpp, floatInstruction): the host's
    // arithmetic, rounding to nearest, no status flags.
    if ((word & 0xffa00c00) == 0x1e200800 && ((word >> 12) & 15) < 4) { // FMUL, FDIV, FADD, FSUB
        f.kind = FastInstruction::floatArithmetic;
        f.op = uint8_t((word >> 12) & 3);
        f.size = (word & 0x400000) ? 8 : 4;
        return f;
    }
    if ((word & 0xff800000) == 0x1f000000) { // FMADD, FMSUB, FNMADD, FNMSUB
        f.kind = FastInstruction::floatArithmetic;
        f.op = uint8_t(4 + (((word >> 21) & 1) << 1) + ((word >> 15) & 1));
        f.ra = uint8_t((word >> 10) & 31);
        f.size = (word & 0x400000) ? 8 : 4;
        return f;
    }
    if ((word & 0xff20fc07) == 0x1e202000 && !(word & 0x800000)) { // FCMP, FCMPE, with a register or with zero
        f.kind = FastInstruction::floatArithmetic;
        f.op = (word & 8) ? 9 : 8;
        f.size = (word & 0x400000) ? 8 : 4;
        return f;
    }
    // The rest of data processing, of the general registers and of the SIMD and floating-point ones
    // (EXTR, FMOV, the arithmetic of both kinds of register): the interpreter's own code runs it. It is
    // taken only when that code has the encoding, which does not depend on the registers' values.
    switch ((word >> 25) & 15) {
        case 5: case 7: case 8: case 9: case 13: case 15: {
            static const bool off = std::getenv("NESTVANE_NO_FAST_REGISTERS");
            if (off) return {};
            CPU scratch;
            if (!registerInstruction(scratch, word)) return {};
            f.kind = FastInstruction::registers;
            f.immediate = word;
            return f;
        }
        default:
            return {};
    }
}

namespace {

// Memory as the interpreter's Machine has it, with windows for what can be reached directly.
struct MachineMemory {
    Machine& machine;
    MemoryWindows* windows;
    bool load(uint64_t address, unsigned size, uint64_t& value) { return nestvane::load(machine, windows, address, size, value); }
    bool store(uint64_t address, unsigned size, uint64_t value) { return nestvane::store(machine, windows, address, size, value); }
    bool loadBytes(uint64_t address, unsigned size, uint8_t* bytes) { return machine.readInto(address, {bytes, size}); }
    bool storeBytes(uint64_t address, unsigned size, const uint8_t* bytes) { return machine.write(address, {bytes, size}); }
    void fault(uint64_t pc, uint64_t address) { machine.fault(pc, address); }
};

template <class Float, class UInt>
void floatOp(CPU& cpu, const FastInstruction& f) {
    const auto get = [&](unsigned index) {
        UInt bits;
        std::memcpy(&bits, cpu.v[index].data(), sizeof(UInt));
        return std::bit_cast<Float>(bits);
    };
    const auto store = [&](Float value) {
        cpu.v[f.rd].fill(0);
        const UInt bits = std::bit_cast<UInt>(value);
        std::memcpy(cpu.v[f.rd].data(), &bits, sizeof(UInt));
    };
    switch (f.op) {
        case 0: store(get(f.rn) * get(f.rm)); break;
        case 1: store(get(f.rn) / get(f.rm)); break;
        case 2: store(get(f.rn) + get(f.rm)); break;
        case 3: store(get(f.rn) - get(f.rm)); break;
        case 8: case 9: { // FCMP
            const Float a = get(f.rn), b = f.op == 9 ? Float(0) : get(f.rm);
            cpu.nzcv = std::isunordered(a, b) ? 3 : a < b ? 8 : a == b ? 6 : 2;
            break;
        }
        default: { // the fused forms: bit 1 of op - 4 negates, bit 0 subtracts
            const bool negate = (f.op - 4) & 2, subtract = (f.op - 4) & 1;
            const Float left = get(f.rn), addend = get(f.ra);
            store(std::fma(negate != subtract ? -left : left, get(f.rm), negate ? -addend : addend));
            break;
        }
    }
}

template <class Memory>
void runFast(CPU& cpu, Memory& memory, const FastInstruction& f) {
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
        case FastInstruction::multiplyLong: {
            const uint64_t a = readZero(cpu, f.rn), b = readZero(cpu, f.rm);
            uint64_t result;
            switch (f.op) {
                case 1: { // SMADDL, SMSUBL
                    const uint64_t product = uint64_t(int64_t(int32_t(a)) * int64_t(int32_t(b)));
                    result = f.sign ? readZero(cpu, f.ra) - product : readZero(cpu, f.ra) + product;
                    break;
                }
                case 5: { // UMADDL, UMSUBL
                    const uint64_t product = uint64_t(uint32_t(a)) * uint64_t(uint32_t(b));
                    result = f.sign ? readZero(cpu, f.ra) - product : readZero(cpu, f.ra) + product;
                    break;
                }
                case 2: result = uint64_t((__int128)int64_t(a) * (__int128)int64_t(b) >> 64); break; // SMULH
                default: result = uint64_t((unsigned __int128)a * b >> 64); break;                   // UMULH
            }
            writeZero(cpu, f.rd, result, 64);
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
            const uint64_t target = branchTarget(cpu, readZero(cpu, f.rn));
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
                if (!memory.load(address, f.size, value)) {
                    memory.fault(cpu.pc, address);
                    return;
                }
                if (f.writeback) writeStack(cpu, f.rn, base + offset, 64);
                writeZero(cpu, f.rd, f.sign ? signExtend(value, f.size * 8) : value, bits);
            } else {
                const uint64_t value = readZero(cpu, f.rd);
                if (!memory.store(address, f.size, value)) {
                    memory.fault(cpu.pc, address);
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
                    if (!memory.load(at, f.size, value)) {
                        memory.fault(cpu.pc, at);
                        return;
                    }
                    writeZero(cpu, registers[i], f.sign ? signExtend(value, 32) : value, bits);
                } else {
                    const uint64_t value = readZero(cpu, registers[i]);
                    if (!memory.store(at, f.size, value)) {
                        memory.fault(cpu.pc, at);
                        return;
                    }
                }
            }
            if (f.writeback) writeStack(cpu, f.rn, base + uint64_t(f.immediate), 64);
            break;
        }
        case FastInstruction::hint:
            break;
        case FastInstruction::barrier:
            std::atomic_thread_fence(std::memory_order_seq_cst);
            break;
        case FastInstruction::threadPointer:
            if (f.op) writeZero(cpu, f.rd, cpu.threadPointer, 64);
            else cpu.threadPointer = readZero(cpu, f.rd);
            break;
        case FastInstruction::conditionalCompare:
            if (condition(cpu.nzcv, f.shift)) {
                const uint64_t a = readZero(cpu, f.rn), b = f.setFlags ? f.rm : readZero(cpu, f.rm);
                if (f.op) addWithCarry(cpu, a, ~b, true, bits, true);
                else addWithCarry(cpu, a, b, false, bits, true);
            } else {
                cpu.nzcv = f.amount;
            }
            break;
        case FastInstruction::singleSource: {
            const uint64_t value = readZero(cpu, f.rn) & ones(bits);
            uint64_t result = 0;
            switch (f.op) {
                case 0: // RBIT
                    for (unsigned bit = 0; bit < bits; ++bit) result |= ((value >> bit) & 1) << (bits - 1 - bit);
                    break;
                case 1: // REV16
                    result = ((value & 0x00ff00ff00ff00ffULL) << 8) | ((value >> 8) & 0x00ff00ff00ff00ffULL);
                    break;
                case 2: // REV32 of a 64-bit register, REV of a 32-bit one
                    result = bits == 64 ? (uint64_t(__builtin_bswap32(uint32_t(value >> 32))) << 32) | __builtin_bswap32(uint32_t(value))
                                        : __builtin_bswap32(uint32_t(value));
                    break;
                case 3: result = __builtin_bswap64(value); break;
                case 4: result = value ? unsigned(__builtin_clzll(value)) - (64 - bits) : bits; break;
                default: { // CLS: the bits after the top one that are the same as it
                    // Bit i of `changes` (i >= 1) says whether bits i and i - 1 differ.
                    const uint64_t changes = (value ^ (value << 1)) & ones(bits) & ~uint64_t{1};
                    result = changes ? unsigned(__builtin_clzll(changes)) - (64 - bits) : bits - 1;
                    break;
                }
            }
            writeZero(cpu, f.rd, result, bits);
            break;
        }
        case FastInstruction::vectorLoadStore: {
            const uint64_t base = readStack(cpu, f.rn);
            const uint64_t offset = (f.op & 2) ? extended(readZero(cpu, f.rm), f.shift, f.amount, 64) : uint64_t(f.immediate);
            const uint64_t address = f.writeback == 1 ? base : base + offset;
            if (f.op & 1) {
                std::array<uint8_t, 16> bytes{};
                if (!memory.loadBytes(address, f.size, bytes.data())) {
                    memory.fault(cpu.pc, address);
                    return;
                }
                if (f.writeback) writeStack(cpu, f.rn, base + offset, 64);
                cpu.v[f.rd] = bytes;
            } else {
                const auto bytes = cpu.v[f.rd];
                if (!memory.storeBytes(address, f.size, bytes.data())) {
                    memory.fault(cpu.pc, address);
                    return;
                }
                if (f.writeback) writeStack(cpu, f.rn, base + offset, 64);
            }
            break;
        }
        case FastInstruction::vectorPair: {
            const uint64_t base = readStack(cpu, f.rn);
            const uint64_t address = f.writeback == 1 ? base : base + uint64_t(f.immediate);
            const unsigned registers[2] = {f.rd, f.ra};
            for (unsigned i = 0; i < 2; ++i) {
                const uint64_t at = address + i * f.size;
                if (f.op) {
                    std::array<uint8_t, 16> bytes{};
                    if (!memory.loadBytes(at, f.size, bytes.data())) {
                        memory.fault(cpu.pc, at);
                        return;
                    }
                    cpu.v[registers[i]] = bytes;
                } else {
                    const auto bytes = cpu.v[registers[i]];
                    if (!memory.storeBytes(at, f.size, bytes.data())) {
                        memory.fault(cpu.pc, at);
                        return;
                    }
                }
            }
            if (f.writeback) writeStack(cpu, f.rn, base + uint64_t(f.immediate), 64);
            break;
        }
        case FastInstruction::floatArithmetic:
            if (f.size == 8) floatOp<double, uint64_t>(cpu, f);
            else floatOp<float, uint32_t>(cpu, f);
            break;
        case FastInstruction::registers:
            // The interpreter would not have it either, and the app would get SIGILL there.
            if (!registerInstruction(cpu, uint32_t(f.immediate))) {
                memory.fault(cpu.pc, cpu.pc);
                return;
            }
            break;
        case FastInstruction::loadLiteral: {
            const uint64_t address = cpu.pc + uint64_t(f.immediate);
            uint64_t value = 0;
            if (!memory.load(address, f.size, value)) {
                memory.fault(cpu.pc, address);
                return;
            }
            writeZero(cpu, f.rd, f.sign ? signExtend(value, 32) : value, bits);
            break;
        }
    }
    cpu.pc = next;
    ++cpu.retired;
}

} // namespace

void executeFast(Machine& machine, const FastInstruction& instruction, MemoryWindows* windows) {
    MachineMemory memory{machine, windows};
    runFast(machine.cpu, memory, instruction);
}

namespace {

// Memory through the guest address space's tables alone, for a worker thread: only an access within
// one page that is mapped with the permission it needs and has host memory. Anything else makes the
// instruction stop the run, with nothing of it done.
struct SpaceMemory {
    const GuestSpace& space;
    bool failed = false;
    uint8_t* at(uint64_t address, unsigned size, int access) const {
        address &= 0x00ff'ffff'ffff'ffffULL; // top byte ignored, as Android's tagged pointers have it
        if (address >= GuestSpace::size || size > GuestSpace::page - (address & (GuestSpace::page - 1))) return nullptr;
        if (!space.mapped(address) || !(space.protection(address) & access) || !space.backed(address)) return nullptr;
        return space.host(address);
    }
    bool load(uint64_t address, unsigned size, uint64_t& value) {
        const auto* bytes = at(address, size, GuestSpace::read);
        if (!bytes) return !(failed = true);
        std::memcpy(&value, bytes, size);
        return true;
    }
    bool store(uint64_t address, unsigned size, uint64_t value) {
        auto* bytes = at(address, size, GuestSpace::write);
        if (!bytes) return !(failed = true);
        std::memcpy(bytes, &value, size);
        return true;
    }
    bool loadBytes(uint64_t address, unsigned size, uint8_t* bytes) {
        const auto* source = at(address, size, GuestSpace::read);
        if (!source) return !(failed = true);
        std::memcpy(bytes, source, size);
        return true;
    }
    bool storeBytes(uint64_t address, unsigned size, const uint8_t* bytes) {
        auto* target = at(address, size, GuestSpace::write);
        if (!target) return !(failed = true);
        std::memcpy(target, bytes, size);
        return true;
    }
    void fault(uint64_t, uint64_t) { failed = true; }
};

thread_local sigjmp_buf* faultJump = nullptr;

// Why runs ended: outside the code, a word that is no decoded instruction, memory, a pair, the budget
// or a stop, a host fault. And a small table of the words, by their top 12 bits (the encoding group).
std::array<std::atomic<uint64_t>, 6> stopReasons{};
std::array<std::atomic<uint64_t>, 4096> stopWords{};

struct CachedWord {
    uint64_t word = UINT64_MAX;
    FastInstruction instruction;
};

} // namespace

void* interpreterFaultJump() { return faultJump; }

std::string interpreterStops() {
    static constexpr const char* names[] = {"left the code", "not decoded", "memory", "pair", "budget or stop", "host fault"};
    std::string out = "  interpreter runs on workers ended:";
    for (size_t i = 0; i < stopReasons.size(); ++i)
        out += std::string(" ") + names[i] + " " + std::to_string(stopReasons[i].load(std::memory_order_relaxed)) + ",";
    std::array<std::pair<uint64_t, unsigned>, 4096> words{};
    for (unsigned i = 0; i < words.size(); ++i) words[i] = {stopWords[i].load(std::memory_order_relaxed), i};
    std::partial_sort(words.begin(), words.begin() + 10, words.end(), [](auto a, auto b) { return a.first > b.first; });
    out += " commonest words not decoded (top 12 bits):";
    char buffer[32];
    for (unsigned i = 0; i < 10 && words[i].first; ++i) {
        std::snprintf(buffer, sizeof(buffer), " %03xxxxxx:%llu", words[i].second, (unsigned long long)words[i].first);
        out += buffer;
    }
    return out + "\n";
}

uint64_t interpretInSpace(CPU& cpu, const GuestSpace& space, const SpaceCode& code, uint64_t budget,
                          const std::atomic<bool>& stop) {
    thread_local std::vector<CachedWord> cache(4096);
    SpaceMemory memory{space};
    // What a fault in the middle of an instruction has to undo: the registers a pair loads into.
    struct State {
        uint64_t ran = 0;
        unsigned first = 32, second = 32;
        uint64_t firstValue = 0, secondValue = 0;
    };
    static thread_local State state;
    state = {};
    sigjmp_buf jump;
    if (sigsetjmp(jump, 0)) {
        faultJump = nullptr;
        stopReasons[5].fetch_add(1, std::memory_order_relaxed);
        if (state.first < 31) cpu.x[state.first] = state.firstValue;
        if (state.second < 31) cpu.x[state.second] = state.secondValue;
        return state.ran;
    }
    faultJump = &jump;
    unsigned reason = 4;
    // Without a base, instructions come from the page of the address space pc is in.
    uint64_t pageStart = 0, pageEnd = 0;
    const uint8_t* pageBytes = nullptr;
    while (state.ran < budget) {
        if (!(state.ran & 1023) && stop.load(std::memory_order_relaxed)) break;
        const uint64_t pc = cpu.pc;
        if (pc - code.start >= code.end - code.start || (pc & 3)) { reason = 0; break; }
        uint32_t word;
        if (code.base) {
            std::memcpy(&word, code.base + (pc - code.start), sizeof(word));
        } else {
            if (pc - pageStart >= pageEnd - pageStart) {
                const auto* bytes = memory.at(pc, sizeof(word), GuestSpace::read);
                if (!bytes) { reason = 2; break; }
                pageStart = pc & ~(GuestSpace::page - 1);
                pageEnd = pageStart + GuestSpace::page;
                pageBytes = bytes - (pc - pageStart);
            }
            std::memcpy(&word, pageBytes + (pc - pageStart), sizeof(word));
        }
        auto& entry = cache[((word ^ (word >> 16)) * 0x9e3779b9U) >> 20];
        if (entry.word != word) {
            entry.word = word;
            entry.instruction = decodeFast(word);
        }
        const auto& f = entry.instruction;
        if (f.kind == FastInstruction::none) {
            reason = 1;
            stopWords[word >> 20].fetch_add(1, std::memory_order_relaxed);
            break;
        }
        state.first = state.second = 32;
        if (f.kind == FastInstruction::loadStorePair || f.kind == FastInstruction::vectorPair) {
            // Both halves have to be there before either is done: a pair that stopped half way would
            // run again whole on the main loop.
            const uint64_t base = f.rn < 31 ? cpu.x[f.rn] : cpu.sp;
            const uint64_t address = f.writeback == 1 ? base : base + uint64_t(f.immediate);
            const int access = f.op ? GuestSpace::read : GuestSpace::write;
            if (!memory.at(address, f.size, access) || !memory.at(address + f.size, f.size, access)) { reason = 3; break; }
            if (f.op && f.kind == FastInstruction::loadStorePair) {
                state.first = f.rd;
                state.firstValue = f.rd < 31 ? cpu.x[f.rd] : 0;
                state.second = f.ra;
                state.secondValue = f.ra < 31 ? cpu.x[f.ra] : 0;
            }
        }
        runFast(cpu, memory, f);
        if (memory.failed) { reason = 2; break; }
        ++state.ran;
    }
    faultJump = nullptr;
    stopReasons[reason].fetch_add(1, std::memory_order_relaxed);
    return state.ran;
}

} // namespace nestvane
