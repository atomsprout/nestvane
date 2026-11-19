#pragma once
#include "Runtime.hpp"

namespace nestvane {

// Additional scalar instructions needed by the dynamic ELF startup probe.
// Returns false without advancing the PC for an unsupported encoding.
bool executeStartupInstruction(Machine& machine, uint32_t word);

} // namespace nestvane
