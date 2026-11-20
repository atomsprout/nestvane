#include "Validation.hpp"
#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>

namespace nestvane {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::array modes{Mode::step, Mode::blocks, Mode::aot};
constexpr size_t iterations = 20;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const char* stopName(Stop stop) {
    switch (stop) {
        case Stop::running: return "running";
        case Stop::exit: return "exit";
        case Stop::fault: return "fault";
        case Stop::unsupported: return "unsupported";
        case Stop::fuelExhausted: return "fuel";
    }
    return "unknown";
}

struct Sample {
    Machine machine;
    Counters counters;
    double elapsedMS;
};

Sample execute(const Image& image, Mode mode, std::string_view fixture) {
    Machine machine(image);
    auto start = Clock::now();
    auto counters = run(machine, mode, image.entry, fixture);
    double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return {std::move(machine), counters, elapsed};
}

void checkEqual(const Machine& expected, const Machine& actual) {
    require(expected.cpu == actual.cpu, "final CPU state differs");
    require(expected.segments == actual.segments, "mapped memory differs");
}

struct Expected {
    Stop stop;
    uint64_t sum;
    uint64_t counter = 0;
    bool compiled = true;
};

double runCase(std::ostream& report, std::string_view label, std::string_view fixture,
               const Image& image, Expected expected, size_t count) {
    Sample reference = execute(image, Mode::step, fixture);
    const auto& cpu = reference.machine.cpu;
    report << label << ": " << stopName(cpu.stop) << ", pc=0x" << std::hex << cpu.pc
           << ", fault=0x" << cpu.faultAddress << std::dec << ", retired=" << cpu.retired << '\n';
    require(cpu.stop == expected.stop, "unexpected stop reason");
    if (expected.stop == Stop::exit) {
        require(cpu.exitCode == 0, "guest exit code differs");
        require(cpu.x[19] == expected.sum, "guest checksum differs");
        std::string output(fixture);
        output += " ready\n";
        require(cpu.output == output, "guest stdout differs");
    } else {
        require(cpu.x[0] == expected.sum, "fault checkpoint differs");
        require(cpu.pc == image.entry + 16 && cpu.faultAddress % pageSize == pageSize - 4,
                "incorrect guest fault address or PC");
    }
    if (expected.counter) {
        uint64_t counter = 0;
        require(reference.machine.load64(cpu.x[3], counter) && counter == expected.counter,
                "guest counter differs");
        require(cpu.x[3] % pageSize == pageSize - 4, "load did not cross a guest page");
    }

    std::array<std::vector<double>, modes.size()> times;
    std::array<Counters, modes.size()> counters{};
    for (size_t i = 0; i < count; ++i) {
        for (size_t offset = 0; offset < modes.size(); ++offset) {
            size_t j = (i + offset) % modes.size();
            auto measured = execute(image, modes[j], fixture);
            checkEqual(reference.machine, measured.machine);
            times[j].push_back(measured.elapsedMS);
            counters[j] = measured.counters;
            if (modes[j] == Mode::aot) {
                require(measured.counters.aotEntries == (expected.compiled ? 1 : 0), "unexpected AOT entry count");
                require(measured.counters.rejectedMappings == (expected.compiled ? 0 : 1), "unexpected AOT rejection count");
                require(measured.counters.fallbackDispatches > 0, "interpreter fallback was not exercised");
            }
        }
    }

    if (count == 1) {
        const auto& native = counters.back();
        report << "  modes agree, AOT entries=" << native.aotEntries
               << ", fallback=" << native.fallbackDispatches << ", rejected=" << native.rejectedMappings << '\n';
        return 0;
    }

    report << "  checksum=" << cpu.x[19] << " stdout=" << cpu.output;
    std::array<double, modes.size()> medians{};
    for (size_t i = 0; i < modes.size(); ++i) {
        auto& values = times[i];
        std::sort(values.begin(), values.end());
        medians[i] = (values[(count - 1) / 2] + values[count / 2]) / 2;
        double p95 = values[(count * 95 + 99) / 100 - 1];
        const auto& stats = counters[i];
        report << "  " << modeName(modes[i]) << ": median=" << medians[i] << " ms, p95=" << p95
               << " ms, interpreted=" << stats.interpreted << ", block dispatches=" << stats.blockDispatches
               << ", AOT entries=" << stats.aotEntries << ", fallback=" << stats.fallbackDispatches << '\n';
    }
    double speedup = medians[1] / medians[2];
    report << "  AOT vs blocks: " << speedup << "x\n";
    return speedup;
}

} // namespace

Validation validate(const std::string& directory) {
    std::ostringstream report;
    report << std::fixed << std::setprecision(3)
           << "nestvane P1 | AArch64 ELF | " << iterations << " timed runs per mode\n"
           << "Timing includes binding, dispatch, and guest execution. ELF loading is outside the timer.\n\n";
    try {
        auto arithBytes = readFile(directory + "/arithmetic.elf");
        auto memoryBytes = readFile(directory + "/memory.elf");
        auto arithmetic = loadELF(arithBytes);
        auto memory = loadELF(memoryBytes);
        double arithSpeed = runCase(report, "arithmetic", "arithmetic", arithmetic,
                                    {Stop::exit, 1'250'175'000}, iterations);
        double memorySpeed = runCase(report, "cross-page reads/writes", "memory", memory,
                                     {Stop::exit, 1'250'425'000, 50'007}, iterations);
        runCase(report, "rebased ELF", "memory", loadELF(memoryBytes, 0x200000),
                {Stop::exit, 1'250'425'000, 50'007}, 1);

        auto changed = arithmetic;
        auto& code = changed.segments.front().bytes;
        uint32_t addFour = 0x91001000; // Replace the first hot instruction, ADD X0, X0, #3.
        std::memcpy(code.mutableData() + 8, &addFour, sizeof(addFour));
        runCase(report, "changed instruction", "arithmetic", changed,
                {Stop::exit, 1'250'225'000, 0, false}, 1);

        auto fault = memory;
        fault.segments.back().bytes.resize(pageSize);
        runCase(report, "fault inside AOT block", "memory", fault, {Stop::fault, 1}, 1);

        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            report << "\nPeak process RSS: " << usage.ru_maxrss << " bytes on Darwin\n";
        }
        report << "Correctness: PASS (5 cases).\n"
               << "2x throughput gate on both fixtures: "
               << (arithSpeed >= 2 && memorySpeed >= 2 ? "PASS" : "FAIL") << '\n';
        return {true, report.str()};
    } catch (const std::exception& error) {
        report << "Correctness: FAIL (" << error.what() << ")\n";
        return {false, report.str()};
    }
}
} // namespace nestvane
