#pragma once

#include "Runtime.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace nestvane {

// The instructions ordinary compiled code is mostly made of, decoded once into a form that runs
// without looking at the encoding again: integer arithmetic and logic, moves, bitfields, selects,
// multiply and divide, branches, and loads and stores of general registers. The interpreter
// (AArch64.cpp) decodes every instruction from its bits each time it runs; an app's library that
// has no translation spent about a third of its time there. Anything not here, and every encoding
// whose result the architecture leaves open, is left to the interpreter (kind none).
struct FastInstruction {
    enum Kind : uint8_t {
        none,
        addImmediate, addShifted, addExtended, // with op set: subtraction
        logicImmediate, logicShifted,
        moveWide, address, bitfield, select, multiplyAdd, multiplyLong, divide, shiftVariable,
        branch, branchCondition, compareBranch, testBranch, branchRegister,
        loadStore, loadStorePair, loadLiteral,
        hint, barrier, threadPointer, conditionalCompare, singleSource,
        vectorLoadStore, vectorPair,
    };
    Kind kind = none;
    uint8_t rd = 0, rn = 0, rm = 0, ra = 0; // ra: the third source, or the second register of a pair
    uint8_t bits = 64;                      // operand width: 32 or 64
    uint8_t op = 0;                         // which operation of the kind
    uint8_t setFlags = 0;
    uint8_t shift = 0, amount = 0;          // a shift's type and amount; an extend's option and amount
    uint8_t size = 0;                       // bytes a load or store moves
    uint8_t writeback = 0;                  // 0 none, 1 after the access (post-index), 2 before (pre-index)
    uint8_t sign = 0;                       // a load that extends the sign, to `bits`
    int64_t immediate = 0;
    uint64_t mask = 0, mask2 = 0;           // a logical immediate, or a bitfield's two masks
};

// Host memory that loads and stores go to directly (Machine::dataWindow), for a run of instructions
// in which the mappings do not change: no system call between. A few windows for reading and a few
// for writing, the most recently found first.
struct MemoryWindows {
    struct Window {
        uint64_t start = 0, end = 0;
        uint8_t* base = nullptr;
    };
    std::array<Window, 4> read{}, write{};
    void clear() { read = {}; write = {}; }
};

class GuestSpace;

// Code a worker thread may interpret: immutable instructions from `start` to `end`, at `base` in host
// memory, or, with no base, in the guest address space, fetched through its tables.
struct SpaceCode {
    uint64_t start = 0, end = 0;
    const uint8_t* base = nullptr;
};
// Interprets the decoded instructions on a worker thread, beside the main loop, with memory reached
// only through the tables of the guest address space: from cpu.pc for up to `budget` instructions,
// until an instruction that is not one of them, memory the space cannot give directly, code outside
// `code`, or `stop`. Returns how many ran; cpu.pc is at the first one that did not. A host fault in
// memory unmapped meanwhile by another thread ends the run before the instruction it happened in.
uint64_t interpretInSpace(CPU& cpu, const GuestSpace& space, const SpaceCode& code, uint64_t budget,
                          const std::atomic<bool>& stop);
// The jump a fault signal on a thread that runs interpretInSpace takes, or null. For the signal handler.
void* interpreterFaultJump();
// Why runs of interpretInSpace ended, for the statistics: counts by reason and the commonest words
// that were not decoded instructions.
std::string interpreterStops();

FastInstruction decodeFast(uint32_t word);
// Runs a decoded instruction (kind is not none) as the interpreter would: the registers, the flags,
// memory, pc and the count of retired instructions; a fault as Machine::fault leaves it. With
// windows, memory in them is reached directly, and windows are added as they are found.
void executeFast(Machine& machine, const FastInstruction& instruction, MemoryWindows* windows = nullptr);

} // namespace nestvane
