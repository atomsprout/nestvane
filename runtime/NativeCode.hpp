#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace nestvane {

struct Machine;

// Guest code that was translated ahead of time into host code (tools/build-native-code.py) and is
// linked into this program. A translation is for one load address, so the kernel layer loads each
// translated file there, and code that is not translated, or not where it belongs, is interpreted.
namespace native {

struct Module {
    uint64_t key, size;    // of the ELF file: contentKey of its bytes, and its length
    const uint64_t* info;  // text start, text end, entry, table of host code per instruction, entry stubs, their count
    uint64_t base;         // the load address the translation is for
    const char* name;
};

// Whether guest code runs natively: translations are linked in and NESTVANE_NATIVE is set.
bool enabled();
// Why translations that are linked in and wanted cannot be used, or null.
const char* failure();
uint64_t contentKey(std::span<const uint8_t> file);
// A translation of an ELF file, or null. Files with the same bytes can exist under several paths and
// be loaded together, so there may be several translations, each for an address of its own: `copy`
// counts them from 0.
const Module* moduleFor(std::span<const uint8_t> file, unsigned copy = 0);
// The module, if mapping `file` from `offset` at `address` puts its code where it was translated for.
const Module* mappedCode(std::span<const uint8_t> file, uint64_t address, uint64_t offset);
// Makes a module's code reachable: maps its table into the table of all translated code. Until then
// a jump to it from native code faults in the look-up and is interpreted. False if it cannot be mapped.
bool activate(const Module& module);
// Makes native code that jumps to `pc` leave native execution there, so that the caller sees the
// guest arrive. `pc` is the entry point of an active module that is a program.
void trap(uint64_t pc);

enum class Exit {
    syscall,    // at an svc, which the caller carries out
    fault,      // at an instruction that cannot run natively as things are: the caller interprets it
    preempted,  // the time slice ended, between two instructions
    interpret,  // the instruction at pc is not entered natively
    lost,       // control left translated code: machine.fault has the details
};
// Runs the machine's current thread natively from cpu.pc, which is in translated code that is mapped
// in machine.space. Returns with the registers in machine.cpu and pc at a guest instruction.
Exit run(Machine& machine);
std::string statistics();

} // namespace native
} // namespace nestvane
