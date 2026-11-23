// Reads instruction words (hex, one per line) and prints those the interpreter has no implementation for.
#include "Runtime.hpp"
#include "AArch64.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using namespace nestvane;
int main() {
    const uint64_t base = 0x100000;
    std::vector<uint8_t> data(0x10000, 0x5a);
    Machine machine(Image{base, {{base, data, 6}}, {}});
    std::string line;
    while (std::getline(std::cin, line)) {
        const uint32_t word = uint32_t(std::strtoul(line.c_str(), nullptr, 16));
        if (decode(word).op != Op::unsupported) continue;
        machine.cpu = CPU{};
        machine.cpu.pc = base + 0x8000;
        machine.cpu.sp = base + 0x4000;
        for (auto& x : machine.cpu.x) x = base + 0x2000;
        if (!executeStartupInstruction(machine, word)) std::printf("%08x\n", word);
    }
}
