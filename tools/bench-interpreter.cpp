// How fast the interpreter runs ordinary compiled code, the way it runs an app's library that has no
// translation: tools/interpreter-bench.c on this Mac's processor, then the same bytes in the
// interpreter with the loop the runtime uses for such code (runtime/DynamicProbe.cpp). The results
// have to agree. Prints the time of each and the interpreter's instructions a second.
//
//   tools/bench-interpreter.sh [scale]
#include "AArch64.hpp"
#include "FastPath.hpp"
#include "Runtime.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nestvane;

extern "C" int64_t interpreter_bench(uint8_t* buffer, int64_t scale);
extern const uint8_t benchStart __asm("section$start$__TEXT$__bench");
extern const uint8_t benchEnd __asm("section$end$__TEXT$__bench");

int main(int argc, char** argv) {
    const int64_t scale = argc > 1 ? std::atoll(argv[1]) : 20;
    constexpr uint64_t bufferAddress = 0x1000'0000, bufferSize = 1 << 20, returnAddress = 0xdead'0000;
    constexpr unsigned execute = 1, write = 2, read = 4;

    std::vector<uint8_t> buffer(bufferSize);
    auto started = std::chrono::steady_clock::now();
    const auto expected = interpreter_bench(buffer.data(), scale);
    const auto nativeTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // The code at the address it has in this process, so that its branches land where they should.
    const auto codeStart = reinterpret_cast<uint64_t>(&benchStart) & ~(pageSize - 1);
    const auto codeEnd = (reinterpret_cast<uint64_t>(&benchEnd) + pageSize - 1) & ~(pageSize - 1);
    std::vector<uint8_t> code(codeEnd - codeStart);
    std::memcpy(code.data() + (reinterpret_cast<uint64_t>(&benchStart) - codeStart), &benchStart,
                size_t(&benchEnd - &benchStart));
    Image image{codeStart, {}, {}};
    image.segments.push_back(Segment{codeStart, DenseBytes(std::move(code)), read | execute});
    image.segments.push_back(Segment{bufferAddress, DenseBytes(std::vector<uint8_t>(bufferSize)), read | write});
    Machine machine(image);
    auto& cpu = machine.cpu;
    cpu.pc = reinterpret_cast<uint64_t>(&interpreter_bench);
    cpu.x[0] = bufferAddress;
    cpu.x[1] = uint64_t(scale);
    cpu.x[30] = returnAddress;
    cpu.sp = 0x8000'0000;

    // The runtime's loop for code without a translation (DynamicProbe.cpp): the common instructions
    // from their decoded form, everything else through the full interpreter. NESTVANE_BENCH_SLOW=1
    // leaves the decoded form out, for comparison.
    const bool slow = std::getenv("NESTVANE_BENCH_SLOW");
    struct FastWord { uint64_t word = UINT64_MAX; FastInstruction instruction; };
    std::vector<FastWord> fastWords(8192);
    started = std::chrono::steady_clock::now();
    uint64_t windowStart = 0, windowEnd = 0;
    const uint8_t* window = nullptr;
    while (cpu.pc != returnAddress) {
        const uint8_t* bytes;
        if (cpu.pc - windowStart < windowEnd - windowStart && !(cpu.pc & 3)) bytes = window + (cpu.pc - windowStart);
        else if (!slow && machine.codeWindow(cpu.pc, windowStart, windowEnd, window)) bytes = window + (cpu.pc - windowStart);
        else { windowStart = windowEnd = 0; bytes = machine.readInstruction(cpu.pc); }
        if (!bytes) { std::printf("FAIL: no instruction at 0x%llx\n", (unsigned long long)cpu.pc); return 1; }
        uint32_t word;
        std::memcpy(&word, bytes, sizeof(word));
        auto& entry = fastWords[((word ^ (word >> 16)) * 0x9e3779b9U) >> 19];
        if (entry.word != word) {
            entry.word = word;
            entry.instruction = slow ? FastInstruction{} : decodeFast(word);
        }
        if (entry.instruction.kind != FastInstruction::none) {
            executeFast(machine, entry.instruction);
        } else if (!executeStartupInstruction(machine, word)) {
            std::printf("FAIL: the interpreter does not have %08x at 0x%llx\n", word, (unsigned long long)cpu.pc);
            return 1;
        }
        if (cpu.stop != Stop::running) {
            std::printf("FAIL: stopped at 0x%llx, fault address 0x%llx\n", (unsigned long long)cpu.pc,
                        (unsigned long long)cpu.faultAddress);
            return 1;
        }
    }
    const auto interpretedTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto result = int64_t(cpu.x[0]);
    std::printf("native %.3f s, interpreted %.3f s (%.0fx), %llu instructions, %.1f million a second\n",
                nativeTime, interpretedTime, interpretedTime / nativeTime, (unsigned long long)cpu.retired,
                double(cpu.retired) / interpretedTime / 1e6);
    if (result != expected) {
        std::printf("FAIL: result %lld, expected %lld\n", (long long)result, (long long)expected);
        return 1;
    }
    std::printf("results agree\n");
    return 0;
}
