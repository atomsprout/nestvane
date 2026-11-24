// The pre-decoded instructions (runtime/FastPath.cpp) against the interpreter (runtime/AArch64.cpp):
// random encodings of every kind the fast path takes, each run both ways on the same random
// registers, flags and memory, with base registers that point into memory, near its end, or
// nowhere. Registers, flags, pc, the retired count, a fault and memory must all come out the same.
#include "AArch64.hpp"
#include "FastPath.hpp"
#include "Runtime.hpp"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace nestvane;

namespace {

constexpr uint64_t codeAddress = 0x10000, codeSize = 0x20000, wordAddress = 0x20000;
constexpr uint64_t dataAddress = 0x200000, dataSize = 0x10000;

struct Template { uint32_t mask, value; const char* name; };
constexpr Template templates[] = {
    {0x1f800000, 0x11000000, "add/sub immediate"},  {0x1f200000, 0x0b000000, "add/sub shifted"},
    {0x1fe00000, 0x0b200000, "add/sub extended"},   {0x1f000000, 0x0a000000, "logic shifted"},
    {0x1f800000, 0x12000000, "logic immediate"},    {0x1f800000, 0x12800000, "move wide"},
    {0x1f000000, 0x10000000, "adr/adrp"},           {0x1f800000, 0x13000000, "bitfield"},
    {0x3fe00000, 0x1a800000, "conditional select"}, {0x7fe00000, 0x1b000000, "multiply-add"},
    {0x7fe00000, 0x1ac00000, "divide/shift"},       {0xff000000, 0x9b000000, "multiply long/high"},       {0x7c000000, 0x14000000, "b/bl"},
    {0xff000010, 0x54000000, "b.cond"},             {0x7e000000, 0x34000000, "cbz/cbnz"},
    {0x7e000000, 0x36000000, "tbz/tbnz"},           {0xfffffc1f, 0xd61f0000, "br"},
    {0xfffffc1f, 0xd63f0000, "blr"},                {0xfffffc1f, 0xd65f0000, "ret"},
    {0x3a000000, 0x38000000, "load/store"},         {0x3a000000, 0x28000000, "load/store pair"},
    {0x3b000000, 0x18000000, "load literal"},
    {0xfffff01f, 0xd503201f, "hint"},               {0xfffff01f, 0xd503301f, "barrier"},
    {0xffdfffe0, 0xd51bd040, "tpidr_el0"},          {0x3fe00410, 0x3a400000, "ccmp/ccmn"},
    {0x7fff0000, 0x5ac00000, "one source"},         {0x3e000000, 0x3c000000, "vector load/store"},
    {0x3e000000, 0x2c000000, "vector pair"},
};

Machine makeMachine(uint32_t word, const std::vector<uint8_t>& code, const std::vector<uint8_t>& data) {
    constexpr unsigned execute = 1, write = 2, read = 4;
    std::vector<uint8_t> codeBytes = code;
    std::memcpy(codeBytes.data() + (wordAddress - codeAddress), &word, sizeof(word));
    Image image{wordAddress, {}, {}};
    image.segments.push_back(Segment{codeAddress, DenseBytes(std::move(codeBytes)), read | execute});
    image.segments.push_back(Segment{dataAddress, DenseBytes(std::vector<uint8_t>(data)), read | write});
    return Machine(image);
}

} // namespace

int main(int argc, char** argv) {
    const unsigned rounds = argc > 1 ? unsigned(std::atoi(argv[1])) : 4000;
    std::mt19937_64 random(20261124);
    std::vector<uint8_t> code(codeSize), data(dataSize);
    unsigned tested = 0, failures = 0;
    for (const auto& form : templates) {
        unsigned decoded = 0, formFailures = 0;
        for (unsigned round = 0; round < rounds && formFailures < 5; ++round) {
            const uint32_t word = (uint32_t(random()) & ~form.mask) | form.value;
            const auto fast = decodeFast(word);
            if (fast.kind == FastInstruction::none) continue;
            ++decoded;
            for (auto& byte : code) byte = uint8_t(random());
            for (auto& byte : data) byte = uint8_t(random());
            CPU state;
            for (auto& value : state.x) {
                switch (random() % 6) {
                    case 0: value = random(); break;
                    case 1: value = random() & 0xff; break;
                    case 2: value = uint64_t(-int64_t(random() & 0xff)); break;
                    case 3: value = dataAddress + dataSize - (random() & 0x3f); break; // near the end
                    default: value = dataAddress + (random() % dataSize); break;
                }
            }
            state.sp = dataAddress + ((random() % dataSize) & ~uint64_t{15});
            state.nzcv = uint8_t(random() & 15);
            state.pc = wordAddress;
            auto interpreted = makeMachine(word, code, data);
            auto pre = makeMachine(word, code, data);
            interpreted.cpu = state;
            pre.cpu = state;
            if (!executeStartupInstruction(interpreted, word)) {
                std::printf("FAIL %s %08x: the fast path takes an instruction the interpreter does not have\n", form.name, word);
                ++formFailures;
                continue;
            }
            executeFast(pre, fast);
            const auto* a = interpreted.read(dataAddress, dataSize, 4);
            const auto* b = pre.read(dataAddress, dataSize, 4);
            const bool sameMemory = a && b && std::memcmp(a, b, dataSize) == 0;
            if (interpreted.cpu == pre.cpu && sameMemory) continue;
            ++formFailures;
            std::printf("FAIL %s %08x:%s\n", form.name, word, sameMemory ? "" : " memory differs");
            const auto& x = interpreted.cpu;
            const auto& y = pre.cpu;
            for (unsigned i = 0; i < 31; ++i)
                if (x.x[i] != y.x[i]) std::printf("  x%u interpreter %016llx fast %016llx (was %016llx)\n", i,
                                                  (unsigned long long)x.x[i], (unsigned long long)y.x[i], (unsigned long long)state.x[i]);
            if (x.sp != y.sp) std::printf("  sp %llx / %llx\n", (unsigned long long)x.sp, (unsigned long long)y.sp);
            if (x.pc != y.pc) std::printf("  pc %llx / %llx\n", (unsigned long long)x.pc, (unsigned long long)y.pc);
            if (x.nzcv != y.nzcv) std::printf("  nzcv %x / %x (was %x)\n", x.nzcv, y.nzcv, state.nzcv);
            if (x.retired != y.retired) std::printf("  retired %llu / %llu\n", (unsigned long long)x.retired, (unsigned long long)y.retired);
            if (x.stop != y.stop || x.faultAddress != y.faultAddress)
                std::printf("  stop %d / %d, fault %llx / %llx\n", int(x.stop), int(y.stop),
                            (unsigned long long)x.faultAddress, (unsigned long long)y.faultAddress);
            if (!(x == y) && x.x == y.x && x.sp == y.sp && x.pc == y.pc && x.nzcv == y.nzcv) std::printf("  other state differs\n");
        }
        std::printf("%-20s %5u decoded, %u failures\n", form.name, decoded, formFailures);
        tested += decoded;
        failures += formFailures;
    }
    std::printf("fast path: %u instructions compared, %u failures\n", tested, failures);
    return failures ? 1 : 0;
}
