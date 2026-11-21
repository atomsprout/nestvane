#include "Runtime.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace nestvane;

namespace {
// Predecode the single immutable executable segment of each fixture.
struct Cell;
using Handler = void (*)(Machine&, const Cell&);
struct Cell {
    Handler execute = nullptr;
    Instruction instruction{};
    uint64_t address = 0;
    const Cell* next = nullptr;
    const Cell* taken = nullptr;
};

template<Op op>
void handle(Machine& machine, const Cell& cell) {
    auto& cpu = machine.cpu;
    const auto& ins = cell.instruction;
    const Cell* next = cell.next;
    if constexpr (op == Op::svc || op == Op::unsupported) {
        cpu.pc = cell.address;
        executeDecoded(machine, ins);
        return;
    } else if constexpr (op == Op::movz) {
        cpu.x[ins.rd] = ins.immediate;
    } else if constexpr (op == Op::addImm) {
        cpu.x[ins.rd] = cpu.x[ins.rn] + ins.immediate;
    } else if constexpr (op == Op::subImm) {
        cpu.x[ins.rd] = cpu.x[ins.rn] - ins.immediate;
    } else if constexpr (op == Op::addReg) {
        cpu.x[ins.rd] = cpu.x[ins.rn] + cpu.x[ins.rm];
    } else if constexpr (op == Op::adr) {
        cpu.x[ins.rd] = cell.address + ins.immediate;
    } else if constexpr (op == Op::load || op == Op::store) {
        const auto address = cpu.x[ins.rn] + ins.immediate;
        const bool ok = op == Op::load ? machine.load64(address, cpu.x[ins.rd]) : machine.store64(address, cpu.x[ins.rd]);
        if (!ok) {
            machine.fault(cell.address, address);
            return;
        }
    } else if constexpr (op == Op::cbnz) {
        if (cpu.x[ins.rd]) next = cell.taken;
    }
    ++cpu.retired;
    if (!next) {
        cpu.pc = cell.address + (op == Op::cbnz && cpu.x[ins.rd] ? ins.immediate : 4);
        return;
    }
    if (cpu.retired == fuel) {
        cpu.pc = next->address;
        return;
    }
    [[clang::musttail]] return next->execute(machine, *next);
}

constexpr std::array handlers{
    handle<Op::movz>, handle<Op::addImm>, handle<Op::subImm>, handle<Op::addReg>, handle<Op::adr>,
    handle<Op::load>, handle<Op::store>, handle<Op::cbnz>, handle<Op::svc>, handle<Op::unsupported>,
};
static_assert(handlers.size() == size_t(Op::unsupported) + 1);

class PreparedProgram {
public:
    explicit PreparedProgram(const Segment& code) : first(code.address), cells(code.accessibleSize() / 4) {
        for (size_t i = 0; i < cells.size(); ++i) {
            uint32_t word;
            std::memcpy(&word, code.data() + i * 4, sizeof(word));
            const auto instruction = decode(word);
            const auto address = first + i * 4;
            cells[i] = {handlers[size_t(instruction.op)], instruction, address, at(address + 4),
                        instruction.op == Op::cbnz ? at(address + instruction.immediate) : nullptr};
        }
    }

    PreparedProgram(const PreparedProgram&) = delete;
    PreparedProgram& operator=(const PreparedProgram&) = delete;

    void run(Machine& machine) const {
        auto& cpu = machine.cpu;
        while (cpu.retired < fuel && cpu.stop == Stop::running) {
            const auto* entry = at(cpu.pc);
            if (entry) entry->execute(machine, *entry);
            else machine.fault(cpu.pc, cpu.pc);
        }
        if (cpu.stop == Stop::running) cpu.stop = Stop::fuelExhausted;
    }

private:
    const Cell* at(uint64_t address) const {
        if (address < first || (address - first) % 4 || (address - first) / 4 >= cells.size()) return nullptr;
        return &cells[(address - first) / 4];
    }
    uint64_t first;
    std::vector<Cell> cells;
};

void compare(const Image& image, std::string_view label, Stop expected) {
    const auto code = std::ranges::find_if(image.segments, [](const Segment& segment) {
        return (segment.flags & 1) != 0;
    });
    assert(code != image.segments.end() && !(code->flags & 2));
    const PreparedProgram program(*code);
    using Clock = std::chrono::steady_clock;
    std::vector<double> baselineTimes, preparedTimes;
    uint64_t retired = 0;
    for (unsigned i = 0; i < 60; ++i) {
        Machine baseline(image), prepared(image);
        const auto time = [](auto task) {
            const auto started = Clock::now();
            task();
            return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        };
        const auto base = [&] {
            baselineTimes.push_back(time([&] { run(baseline, Mode::blocks, image.entry, label); }));
        };
        const auto fast = [&] {
            preparedTimes.push_back(time([&] { program.run(prepared); }));
        };
        if (i % 2) {
            fast();
            base();
        } else {
            base();
            fast();
        }
        assert(baseline.cpu.stop == expected && baseline.cpu == prepared.cpu);
        for (const auto& segment : image.segments) {
            if (!(segment.flags & 4) || (segment.flags & 1)) continue;
            std::vector<uint8_t> expected(segment.accessibleSize()), observed(expected.size());
            assert(baseline.readInto(segment.address, expected) && prepared.readInto(segment.address, observed));
            assert(expected == observed);
        }
        retired = prepared.cpu.retired;
    }
    if (expected == Stop::fault) {
        std::cout << label << ": matching CPU state and bytes after " << retired << " instructions\n";
        return;
    }
    std::ranges::sort(baselineTimes);
    std::ranges::sort(preparedTimes);
    std::cout << label << ": " << retired << " instructions; cached blocks " << baselineTimes[30]
              << "ms; linked handlers " << preparedTimes[30] << "ms; ratio "
              << baselineTimes[30] / preparedTimes[30] << "x\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <guest fixture directory>\n";
        return 2;
    }
    for (const std::string name : {"arithmetic", "memory"}) {
        auto image = loadELF(readFile((std::filesystem::path(argv[1]) / (name + ".elf")).string()));
        compare(image, name, Stop::exit);
        if (name == "memory") {
            for (auto& segment : image.segments) segment.flags &= ~2U;
            compare(image, "write fault", Stop::fault);
        }
    }
}
