#pragma once
#include "Runtime.hpp"

namespace nestvane {

// Additional scalar instructions needed by the dynamic ELF startup probe.
// Returns false without advancing the PC for an unsupported encoding.
bool executeStartupInstruction(Machine& machine, uint32_t word);

// The instructions of the encoding groups that change only registers (data processing, of the
// general registers and of the SIMD and floating-point ones), as executeStartupInstruction does
// them, without advancing the pc. False for an encoding it does not have.
bool registerInstruction(CPU& cpu, uint32_t word);

} // namespace nestvane
