// Runs vector instructions on the processor and in the interpreter with the same operands, and
// compares what comes out: the destination register and the sticky saturation flag. The cases are
// the functions in tools/vector-instructions.S.
#include "AArch64.hpp"
#include "Runtime.hpp"

#include <cstdio>
#include <cstring>
#include <random>

using namespace nestvane;

struct Case {
    void (*run)(void*);
    const char* name;
};
extern "C" const Case vector_cases[];

int main() {
    std::mt19937_64 random(20261123);
    Machine machine(Image{0x4000, {}, {}});
    unsigned cases = 0, failures = 0;
    for (const Case* entry = vector_cases; entry->run; ++entry, ++cases) {
        uint32_t word;
        std::memcpy(&word, reinterpret_cast<const uint8_t*>(entry->run) + 12, sizeof(word));
        bool reported = false;
        for (unsigned round = 0; round < 4000 && !reported; ++round) {
            struct { uint8_t v[3][16]; uint64_t fpsr; } state{};
            for (auto& vector : state.v) {
                // Mostly the ends of each lane's range, where saturation and rounding show.
                for (unsigned at = 0; at < 16;) {
                    const unsigned laneSize = 1U << (random() & 3);
                    uint64_t value;
                    switch (random() % 8) {
                        case 0: value = 0; break;
                        case 1: value = ~uint64_t{0}; break;
                        case 2: value = uint64_t{1} << (laneSize * 8 - 1); break;
                        case 3: value = (uint64_t{1} << (laneSize * 8 - 1)) - 1; break;
                        case 4: value = random() & 0x3f; break;
                        case 5: value = uint64_t(-int64_t(random() & 0x3f)); break;
                        default: value = random(); break;
                    }
                    for (unsigned byte = 0; byte < laneSize && at < 16; ++byte, ++at) vector[at] = uint8_t(value >> (8 * byte));
                }
            }
            // Now and then numbers a conversion can do something with.
            if (round % 4 == 0) {
                const double choices[] = {0.5, -0.5, 1.5, -2.5, 2147483647.5, -2147483648.5, 4294967295.5, 1e30, -1e30, 3.4e38, 65504.0, 65520.0, 1e-8};
                for (unsigned index = 0; index < 2; ++index) {
                    const double pick = choices[random() % std::size(choices)] * double(1 + random() % 3);
                    if (round % 8 == 0) std::memcpy(state.v[1] + index * 8, &pick, 8);
                    else for (unsigned half = 0; half < 2; ++half) { const float narrow = float(pick); std::memcpy(state.v[1] + index * 8 + half * 4, &narrow, 4); }
                }
            }
            auto expected = state;
            entry->run(&expected);

            machine.cpu = CPU{};
            machine.cpu.pc = 0x4000;
            for (unsigned index = 0; index < 3; ++index) std::memcpy(machine.cpu.v[index].data(), state.v[index], 16);
            if (!executeStartupInstruction(machine, word)) {
                std::printf("FAIL %s (%08x): the interpreter does not have it\n", entry->name, word);
                reported = true;
                break;
            }
            constexpr uint64_t saturated = uint64_t{1} << 27;
            const bool sameValue = std::memcmp(machine.cpu.v[0].data(), expected.v[0], 16) == 0;
            const bool sameFlag = ((machine.cpu.fpsr ^ expected.fpsr) & saturated) == 0;
            if (!sameValue || !sameFlag) {
                std::printf("FAIL %s (%08x) round %u:%s%s\n  v0=", entry->name, word, round, sameValue ? "" : " value", sameFlag ? "" : " saturation flag");
                for (int b = 15; b >= 0; --b) std::printf("%02x", state.v[0][b]);
                std::printf(" v1=");
                for (int b = 15; b >= 0; --b) std::printf("%02x", state.v[1][b]);
                std::printf(" v2=");
                for (int b = 15; b >= 0; --b) std::printf("%02x", state.v[2][b]);
                std::printf("\n  processor=");
                for (int b = 15; b >= 0; --b) std::printf("%02x", expected.v[0][b]);
                std::printf(" interpreter=");
                for (int b = 15; b >= 0; --b) std::printf("%02x", machine.cpu.v[0][b]);
                std::printf(" flags %llx / %llx\n", (unsigned long long)expected.fpsr, (unsigned long long)machine.cpu.fpsr);
                reported = true;
            }
        }
        failures += reported;
    }
    std::printf("vector instructions against the processor: cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
