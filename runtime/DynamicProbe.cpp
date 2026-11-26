#include "DynamicProbe.hpp"
#include "AArch64.hpp"
#include "FastPath.hpp"
#include "NativeCode.hpp"
#include "AndroidBootAot.hpp"
#include "ProbeLinux.hpp"

#include <map>
#include <set>
#include <new>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sstream>
#include <stdexcept>

namespace nestvane {

namespace {
// Export Diagnostics (requestDiagnostics): the request, and whether an Android runs to answer it.
std::mutex diagnosticsMutex;
std::function<void(std::string)> diagnosticsDone;
std::atomic_bool diagnosticsWanted{false}, androidRunning{false};
} // namespace

bool requestDiagnostics(std::function<void(std::string)> done) {
    if (!androidRunning.load()) return false;
    std::lock_guard lock(diagnosticsMutex);
    diagnosticsDone = std::move(done);
    diagnosticsWanted.store(true);
    return true;
}

namespace {

std::string pathArgument(const Machine& machine, uint64_t address) {
    std::string path;
    for (unsigned i = 0; i < 192 && address < UINT64_MAX; ++i, ++address) {
        const auto* byte = machine.read(address, 1, 4);
        if (!byte || (*byte && (*byte < 32 || *byte > 126))) return {};
        if (!*byte) return path;
        path.push_back(static_cast<char>(*byte));
    }
    return {};
}

void describeCall(std::ostream& report, const CPU& cpu, uint64_t number) {
    report << "  syscall " << number << " PC=0x" << std::hex << cpu.pc;
    for (unsigned i = 0; i < 6; ++i) report << " x" << i << "=0x" << cpu.x[i];
    report << std::dec;
}

std::mutex mappedImageLock;
std::map<const uint8_t*, MappedImageFile> mappedImages;

class ReadOnlyImageFile {
public:
    ReadOnlyImageFile(const std::filesystem::path& path, size_t bytes) : size(bytes) {
        if (!size) return;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot open " + path.string());
        const auto* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (mapped == MAP_FAILED) throw std::runtime_error("cannot map " + path.string());
        data = static_cast<const uint8_t*>(mapped);
        const std::lock_guard guard(mappedImageLock);
        mappedImages[data] = {path.string(), data, size};
    }
    ReadOnlyImageFile(ReadOnlyImageFile&& other) noexcept
        : data(std::exchange(other.data, nullptr)), size(other.size) {}
    ReadOnlyImageFile(const ReadOnlyImageFile&) = delete;
    ~ReadOnlyImageFile() {
        if (!data) return;
        {
            const std::lock_guard guard(mappedImageLock);
            mappedImages.erase(data);
        }
        ::munmap(const_cast<uint8_t*>(data), size);
    }
    std::span<const uint8_t> bytes() const { return data ? std::span(data, size) : std::span<const uint8_t>{}; }

private:
    const uint8_t* data = nullptr;
    size_t size;
};

enum class ProbeKind { dynamic, init, properties, runtimeApex, serviceReady, bootGate, data, art };

uint64_t initLimit(const char* name, uint64_t fallback, uint64_t maximum, uint64_t minimum = 1) {
    const auto* raw = std::getenv(name);
    if (!raw) return fallback;
    std::string_view value(raw);
    uint64_t limit = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
    if (error != std::errc{} || end != value.data() + value.size() || limit < minimum || limit > maximum)
        throw std::runtime_error(std::string("invalid init probe limit: ") + name);
    return limit;
}

DynamicReport runProbe(const Image& image, ProbeLinux& linux, ProbeKind kind, std::string_view expectedGate = "true\n",
                       std::stop_token stop = {}, const BootProgress& liveSnapshot = {}, const AndroidBootAot* bootAot = nullptr) {
    const bool init = kind == ProbeKind::init;
    std::ostringstream report;
    switch (kind) {
        case ProbeKind::init: report << "Android 17 second-stage init probe\n"; break;
        case ProbeKind::serviceReady:
        case ProbeKind::runtimeApex:
        case ProbeKind::properties: report << "Android 17 Bionic property reader\n"; break;
        case ProbeKind::bootGate: report << "Android 17 Bionic boot gate reader\n"; break;
        case ProbeKind::art: report << "Android 17 original ART probe\n"; break;
        case ProbeKind::data: report << "Android 17 guest data probe\n"; break;
        case ProbeKind::dynamic: report << "Android 17 dynamic ELF probe\n"; break;
    }
    bool reachedExecutable = false;
    const bool nativeExecution = native::enabled();
    bool interpretNext = false;
    // After native code faults, the thread is interpreted for a stretch before it goes back. A fault
    // costs as much as thousands of interpreted instructions, and a loop that reaches across the end
    // of a run of host memory (GuestSpace) used to fault on every turn: one fault, one instruction
    // interpreted, native again, the same fault. The stretch doubles while faults keep coming.
    uint64_t stretchLeft = 0, stretchThread = 0;
    unsigned stretchLength = 256;
    std::chrono::steady_clock::time_point lastNativeFault{};
    bool afterNative = false;
    uint64_t nativeFaults = 0;
    using namespace std::chrono;
    // Where the host thread's time goes besides guest code: in system calls, and in the scheduler,
    // which is also where it waits when no guest thread can run.
    steady_clock::duration scheduleTime{}, syscallTime{};
    // Code without a translation run in place in native processes: instructions, and time in them.
    uint64_t inPlaceInstructions = 0, inPlaceStretched = 0;
    steady_clock::duration inPlaceTime{};
    std::array<std::pair<steady_clock::duration, uint64_t>, 512> syscallCost{};
    const auto timeReport = [&] {
        std::ostringstream out;
        out << "  host thread time: " << duration_cast<milliseconds>(syscallTime).count() << " ms in system calls, "
            << duration_cast<milliseconds>(scheduleTime).count() << " ms in the scheduler, of which "
            << duration_cast<milliseconds>(linux.idleTime).count() << " ms asleep in " << linux.idleWaits << " waits; "
            << linux.threadSwitches << " thread switches; interpreted in place " << inPlaceInstructions
            << " instructions in " << duration_cast<milliseconds>(inPlaceTime).count() << " ms ("
            << inPlaceInstructions / uint64_t(std::max<int64_t>(1, duration_cast<microseconds>(inPlaceTime).count()))
            << " million a second), " << inPlaceStretched << " of them in stretches after a fault; costliest calls:";
        std::array<size_t, 8> top{};
        std::vector<size_t> order(syscallCost.size());
        for (size_t index = 0; index < order.size(); ++index) order[index] = index;
        std::partial_sort(order.begin(), order.begin() + top.size(), order.end(),
                          [&](size_t a, size_t b) { return syscallCost[a].first > syscallCost[b].first; });
        for (size_t index = 0; index < top.size(); ++index)
            out << " " << order[index] << ":" << duration_cast<milliseconds>(syscallCost[order[index]].first).count() << "ms/"
                << syscallCost[order[index]].second;
        out << '\n';
        return out.str();
    };
    std::string faultSamples;
    auto machine = linux.createMachine(image);
    auto& cpu = machine.cpu;
    const auto& startup = image.startup.value();
    linux.setCommandLine(startup, &machine);
    report << std::hex << "  linker PC=0x" << image.entry << " base=0x" << startup.interpreterBase
           << " executable entry=0x" << startup.executableEntry << " phdr=0x" << startup.programHeaders
           << " initial SP=0x" << cpu.sp << std::dec << '\n';
    if (init) report << "  launch: /system/bin/init second_stage (guest PID 1)\n";
    const bool interactive = init && stop.stop_possible();
    const uint64_t instructionLimit = init ? initLimit("NESTVANE_INIT_INSTRUCTION_LIMIT",
        interactive ? UINT64_MAX : 9'000'000'000, UINT64_MAX) : kind == ProbeKind::art ? 1'500'000'000 : fuel;
    const auto wallTime = std::chrono::seconds(init ?
        initLimit("NESTVANE_INIT_WALL_SECONDS", interactive ? 0 : 360, UINT32_MAX) : 360);
    if (init) {
        report << "  diagnostic limits: ";
        if (instructionLimit == UINT64_MAX) report << "unlimited guest instructions";
        else report << instructionLimit << " guest instructions";
        if (wallTime.count()) report << ", " << wallTime.count() << " seconds\n";
        else report << ", no wall-time deadline\n";
    }
    uint32_t word = 0;
    // Decode depends only on the fetched bits, so shared code, mappings and code edits
    // need no invalidation. Cache unknown words as well as the basic instructions.
    struct DecodedWord {
        uint64_t word = UINT64_MAX;
        Instruction instruction;
    };
    std::array<DecodedWord, 2048> decodedWords{};
    // The common instructions decoded once into a form that runs directly (FastPath.hpp), by the same
    // reasoning keyed by the word alone.
    struct FastWord {
        uint64_t word = UINT64_MAX;
        FastInstruction instruction;
    };
    std::vector<FastWord> fastWords(8192);
    // NESTVANE_NO_FAST_PATH=1 leaves every instruction to the interpreter and fetches each one by
    // itself, for comparison.
    static const bool fastPath = !std::getenv("NESTVANE_NO_FAST_PATH");
    const auto fastWord = [&](uint32_t value) -> const FastInstruction& {
        auto& entry = fastWords[((value ^ (value >> 16)) * 0x9e3779b9U) >> 19];
        if (entry.word != value) {
            entry.word = value;
            entry.instruction = fastPath ? decodeFast(value) : FastInstruction{};
        }
        return entry.instruction;
    };
    uint64_t syscallCount = 0;
    const unsigned traceLimit = init
        ? initLimit("NESTVANE_INIT_TRACE_SYSCALLS", interactive ? 0 : 100000, 1000000, 0)
        : kind == ProbeKind::art ? 16000 : 20000;
    unsigned protectionLogs = 0, syncLogs = 0, suppressedLogs = 0, zygoteSyscalls = 0, zygoteSamples = 0;
    unsigned zygoteMappings = 0, zygoteMemoryFiles = 0, zygoteChildSyscalls = 0, zygoteChildPolicyCalls = 0;
    uint64_t zygotePid = 0;
    std::vector<uint64_t> graphicsPids;
    unsigned graphicsCalls = 0;
    uint64_t deliveredFaults = 0, restoredSignals = 0, illegalInstructions = 0;
    std::optional<uint64_t> unhandled;
    const auto start = std::chrono::steady_clock::now();
    bool idle = false, timedOut = false;
    const auto deadline = wallTime.count() ? start + wallTime : std::chrono::steady_clock::time_point::max();
    using namespace std::chrono;
    const auto snapshotInterval = seconds(init && liveSnapshot
        ? initLimit("NESTVANE_INIT_SNAPSHOT_SECONDS", 0, UINT32_MAX) : 0);
    auto nextSnapshot = snapshotInterval.count() ? start + snapshotInterval : steady_clock::time_point::max();
    auto nextMemoryCheck = start + seconds(1);
    auto scheduleDeadline = std::min(deadline, nextSnapshot);
    constexpr uint64_t sampleStride = 1'048'576;
    const bool sampleBoot = init && initLimit("NESTVANE_INIT_SAMPLE_CODE", 0, 1, 0);
    uint64_t nextSample = sampleBoot ? sampleStride : UINT64_MAX;
    uint64_t samples = 0, unattributedSamples = 0;
    // Time a dispersed subset of syscall handlers. The guest's wait after a
    // blocked syscall is outside this measurement.
    constexpr uint64_t syscallSampleStride = 256;
    uint64_t nextSyscallSample = sampleBoot ? syscallSampleStride : UINT64_MAX;
    uint64_t syscallSamples = 0, intervalSyscallSamples = 0;
    struct SyscallTiming {
        uint64_t calls = 0, samples = 0, nanoseconds = 0;
    };
    constexpr size_t otherSyscall = 512;
    std::array<SyscallTiming, otherSyscall + 1> intervalSyscalls{};
    uint64_t sha256AotEntries = 0, sha256AotInstructions = 0;
    struct NativeLoop {
        const uint8_t* entry = nullptr;
        uint64_t entries = 0, instructions = 0;
    } dexAot, dexVerifierAot, artReturnAot, artBitmapAot, artAsciiAot, bionicFillAot, bionicCopyAot, fontScanAot;
    const auto runNativeLoop = [&]<typename Loop>(NativeLoop& stats, const uint8_t* bytes) {
        if (cpu.pc % pageSize != Loop::fileOffset % pageSize || !androidLoopMatches(machine, bytes, Loop::words))
            return false;
        if (stats.entry != bytes) {
            const auto location = linux.guestCodeFile(bytes);
            if (!location || !location->first.ends_with(Loop::moduleSuffix) || location->second != Loop::fileOffset)
                return false;
            stats.entry = bytes;
        }
        const auto budget = std::min({instructionLimit - cpu.retired, nextSample - cpu.retired,
                                      uint64_t{4096} - (cpu.retired & 4095)});
        constexpr auto loopSize = Loop::maxIterationInstructions;
        if (budget < loopSize || !linux.hasSliceBudget(cpu.retired, loopSize)) return false;
        constexpr auto batch = Loop::batchSize * loopSize;
        const auto iterations = budget >= batch && linux.hasSliceBudget(cpu.retired, batch) ? Loop::batchSize : 1;
        const auto retired = Loop::execute(machine, iterations);
        stats.entries += retired != 0;
        stats.instructions += retired;
        return retired != 0;
    };
    // File offsets survive ASLR. Keep the cumulative page ranking and report
    // smaller regions for each snapshot interval so later boot phases stay visible.
    using CodeLocation = std::pair<std::string, uint64_t>;
    using CodeSamples = std::map<CodeLocation, uint64_t>;
    CodeSamples codeSamples, intervalCodeSamples;
    uint64_t intervalStart = 0, intervalSamples = 0, intervalUnattributed = 0;
    uint64_t previousSnapshotRetired = 0, previousSnapshotAot = 0;
    const auto aotInstructions = [&] {
        return sha256AotInstructions + dexAot.instructions + dexVerifierAot.instructions + artReturnAot.instructions +
               artBitmapAot.instructions + artAsciiAot.instructions + bionicFillAot.instructions + bionicCopyAot.instructions + fontScanAot.instructions;
    };
    const auto appendRanking = [](std::ostream& out, const CodeSamples& histogram, size_t limit, bool byModule = false) {
        CodeSamples modules;
        if (byModule) {
            for (const auto& [location, count] : histogram) modules[{location.first, 0}] += count;
        }
        const auto& counts = byModule ? modules : histogram;
        std::vector<std::pair<CodeLocation, uint64_t>> ranked(counts.begin(), counts.end());
        const auto top = ranked.begin() + std::min(ranked.size(), limit);
        std::partial_sort(ranked.begin(), top, ranked.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        for (auto entry = ranked.begin(); entry != top; ++entry) {
            const auto& [location, count] = *entry;
            out << "    " << count << ' ' << location.first;
            if (!byModule) out << "+0x" << std::hex << location.second << std::dec;
            out << '\n';
        }
    };
    const auto appendBootProfile = [&](std::ostream& out, uint64_t elapsedMs) {
        const auto aot = aotInstructions();
        out << "  Android AOT " << (bootAot ? "enabled" : "disabled") << ": " << aot << " of " << cpu.retired
            << " guest instructions, " << (aot - previousSnapshotAot) << " of "
            << (cpu.retired - previousSnapshotRetired) << " in " << intervalStart << '-' << elapsedMs << " ms\n";
        if (bootAot) {
            out << "  Android SHA-256 AOT entries: " << sha256AotEntries
                << ", guest instructions: " << sha256AotInstructions << '\n'
                << "  Android DEX hash AOT entries: " << dexAot.entries
                << ", guest instructions: " << dexAot.instructions << '\n'
                << "  Android DEX ASCII verifier AOT entries: " << dexVerifierAot.entries
                << ", guest instructions: " << dexVerifierAot.instructions << '\n'
                << "  Android ART collector return AOT entries: " << artReturnAot.entries
                << ", guest instructions: " << artReturnAot.instructions << '\n'
                << "  Android ART collector bitmap AOT entries: " << artBitmapAot.entries
                << ", guest instructions: " << artBitmapAot.instructions << '\n'
                << "  Android ART ASCII scan AOT entries: " << artAsciiAot.entries
                << ", guest instructions: " << artAsciiAot.instructions << '\n'
                << "  Android Bionic fill AOT entries: " << bionicFillAot.entries
                << ", guest instructions: " << bionicFillAot.instructions << '\n'
                << "  Android Bionic copy AOT entries: " << bionicCopyAot.entries
                << ", guest instructions: " << bionicCopyAot.instructions << '\n'
                << "  Android font family scan AOT entries: " << fontScanAot.entries
                << ", guest instructions: " << fontScanAot.instructions << '\n';
        }
        if (!sampleBoot) return;
        out << "  guest instruction samples: " << samples << ", unattributed: " << unattributedSamples << '\n';
        appendRanking(out, codeSamples, 20);
        out << "  guest instruction samples in " << intervalStart << '-' << elapsedMs
            << " ms (64-byte file ranges): " << intervalSamples << ", unattributed: " << intervalUnattributed << '\n';
        appendRanking(out, intervalCodeSamples, 12);
        out << "  guest file totals in " << intervalStart << '-' << elapsedMs << " ms:\n";
        appendRanking(out, intervalCodeSamples, 12, true);
        out << "  guest syscall handler samples in " << intervalStart << '-' << elapsedMs
            << " ms: " << intervalSyscallSamples << '\n';
        std::vector<size_t> ranked;
        for (size_t syscall = 0; syscall < intervalSyscalls.size(); ++syscall)
            if (intervalSyscalls[syscall].samples) ranked.push_back(syscall);
        const auto top = ranked.begin() + std::min<size_t>(ranked.size(), 10);
        std::partial_sort(ranked.begin(), top, ranked.end(), [&](size_t a, size_t b) {
            if (intervalSyscalls[a].nanoseconds != intervalSyscalls[b].nanoseconds)
                return intervalSyscalls[a].nanoseconds > intervalSyscalls[b].nanoseconds;
            return a < b;
        });
        for (auto entry = ranked.begin(); entry != top; ++entry) {
            const auto& timing = intervalSyscalls[*entry];
            out << "    syscall ";
            if (*entry == otherSyscall) out << "other";
            else out << *entry;
            out << ": " << timing.calls << " calls, " << timing.samples << " samples, "
                << timing.nanoseconds << " sampled handler ns\n";
        }
    };
    const auto captureSnapshot = [&](steady_clock::time_point now) {
        // NESTVANE_DUMP_JAVA=<main thread name> has that process print its threads' stacks at each snapshot.
        if (const char* dumped = std::getenv("NESTVANE_DUMP_JAVA")) linux.requestJavaDump(dumped);
        // NESTVANE_EXPORT_FILES=<directory> writes the guest's files under it to <NESTVANE_EXPORT_TO>, such
        // as the stack traces that a dump leaves in /data/anr.
        if (const char* directory = std::getenv("NESTVANE_EXPORT_FILES"), *target = std::getenv("NESTVANE_EXPORT_TO"); directory && target)
            if (std::FILE* out = std::fopen(target, "w")) {
                const auto text = linux.guestFiles(directory, 2 * 1024 * 1024);
                std::fwrite(text.data(), 1, text.size(), out);
                std::fclose(out);
            }
        std::ostringstream snapshot;
        const auto elapsedMs = duration_cast<milliseconds>(now - start).count();
        snapshot << "Android init at " << elapsedMs << " ms, " << cpu.retired << " guest instructions, "
                 << syscallCount << " guest syscalls\n";
        appendBootProfile(snapshot, elapsedMs);
        previousSnapshotRetired = cpu.retired;
        previousSnapshotAot = aotInstructions();
        intervalCodeSamples.clear();
        intervalStart = elapsedMs;
        intervalSamples = 0;
        intervalUnattributed = 0;
        intervalSyscalls.fill({});
        intervalSyscallSamples = 0;
        if (nativeExecution) snapshot << native::statistics() << timeReport() << faultSamples << linux.memoryReport(machine);
        snapshot << linux.workerReport(machine, linux.processId());
        // NESTVANE_INIT_LOG_TAIL=<bytes> keeps more of the guest's log in each snapshot.
        static const size_t androidTailLimit = std::getenv("NESTVANE_INIT_LOG_TAIL")
            ? size_t(std::max(4096, std::atoi(std::getenv("NESTVANE_INIT_LOG_TAIL")))) : 16 * 1024;
        const std::string_view android = linux.recentAndroidOutput.empty()
            ? linux.androidOutput : linux.recentAndroidOutput;
        if (!android.empty()) {
            size_t start = android.size() > androidTailLimit ? android.size() - androidTailLimit : 0;
            if (start) {
                if (const auto lineBreak = android.find('\n', start - 1); lineBreak != std::string_view::npos &&
                    lineBreak + 1 < android.size()) start = lineBreak + 1;
            }
            snapshot << "  recent guest Android log:\n" << android.substr(start);
        }
        liveSnapshot(snapshot.str());
        nextSnapshot = now + snapshotInterval;
        scheduleDeadline = std::min(deadline, nextSnapshot);
    };
    // Export Diagnostics: the state now, then the system server's Java stacks, which it logs line by
    // line once asked; a few seconds later the report goes out with all that was logged meanwhile.
    std::string diagnosticsReport;
    steady_clock::time_point diagnosticsDue{};
    const auto serveDiagnostics = [&](steady_clock::time_point now) {
        if (diagnosticsReport.empty()) {
            if (!diagnosticsWanted.exchange(false)) return;
            std::ostringstream state;
            state << "Android has run " << duration_cast<milliseconds>(now - start).count() << " ms, "
                  << cpu.retired << " guest instructions, " << syscallCount << " guest syscalls\n";
            if (nativeExecution) state << native::statistics() << timeReport() << linux.memoryReport(machine);
            state << linux.workerReport(machine, linux.processId());
            const bool dumping = linux.requestJavaDump("system_server");
            state << "\n== Android's log before the report was asked for (seconds, process, tag: message)\n"
                  << linux.diagnosticLogText();
            state << "\n== Logged in the " << (dumping ? "seconds after: the system server's Java threads"
                                                         : "seconds after (the system server could not be asked for its Java threads)")
                  << "\n";
            diagnosticsReport = state.str();
            linux.diagnosticCapture.clear();
            linux.diagnosticCapturing = true;
            diagnosticsDue = now + seconds(4);
            return;
        }
        if (now < diagnosticsDue) return;
        diagnosticsReport += linux.diagnosticCapture;
        linux.diagnosticCapture.clear();
        linux.diagnosticCapturing = false;
        std::function<void(std::string)> done;
        {
            std::lock_guard lock(diagnosticsMutex);
            done = std::move(diagnosticsDone);
            diagnosticsDone = {};
        }
        if (done) done(std::exchange(diagnosticsReport, {}));
        diagnosticsReport.clear();
    };
    if (init) androidRunning.store(true);
    struct RunningMark {
        bool init;
        ~RunningMark() { if (init) androidRunning.store(false); }
    } runningMark{init};
    while (cpu.retired < instructionLimit) {
        // The host can run out of memory for what a guest process does, anywhere here: that process
        // ends, as Linux's out-of-memory killer would end it, and Android goes on. It used to end everything.
        try {
            if (cpu.stop == Stop::fault) {
                const auto pc = cpu.pc, address = cpu.faultAddress, lr = cpu.x[30], sp = cpu.sp;
                const auto* code = machine.read(pc, sizeof(uint32_t), 1);
                uint32_t faultWord = 0;
                if (code) std::memcpy(&faultWord, code, sizeof(faultWord));
                if (!linux.deliverMemoryFault(machine)) break;
                if (deliveredFaults++ < 32) {
                    std::ostringstream fault;
                    fault << "guest SIGSEGV delivered to PID=" << linux.processId() << " TID=" << linux.threadId()
                          << std::hex << " PC=0x" << pc << " address=0x" << address << " word=0x" << faultWord
                          << " SP=0x" << sp << " LR=0x" << lr << " handler=0x" << cpu.pc << std::dec;
                    report << "  " << fault.str() << '\n';
                    if (init && linux.androidLogObserver) {
                        const auto message = fault.str();
                        linux.androidLogObserver(message);
                    }
                }
            } else if (cpu.stop != Stop::running) break;
            if (init && (linux.blocked() || !(cpu.retired & 4095))) {
                if (stop.stop_requested()) break;
                const auto now = steady_clock::now();
                if (now >= deadline) {
                    timedOut = true;
                    break;
                }
                if (now >= nextSnapshot) captureSnapshot(now);
                serveDiagnostics(now);
                if (now >= nextMemoryCheck) {
                    // After ending one, look again soon: one app may not have been enough.
                    nextMemoryCheck = now + (linux.relieveMemoryPressure() ? milliseconds(250) : seconds(1));
                }
            }
            // Timed after a stretch of native code only: after each interpreted instruction, reading the
            // clock twice would cost more than the instruction.
            const bool timeSchedule = nativeExecution && afterNative;
            afterNative = false;
            const auto scheduleBegan = timeSchedule ? steady_clock::now() : steady_clock::time_point{};
            const bool scheduled = linux.schedule(machine, scheduleDeadline, stop);
            if (timeSchedule) scheduleTime += steady_clock::now() - scheduleBegan;
            if (!scheduled) {
                const auto now = steady_clock::now();
                if (now >= nextSnapshot && now < deadline && !stop.stop_requested()) {
                    // A timed guest wait can become runnable while the snapshot deadline expires.
                    // Capture before retrying, even when the instruction counter is mid-slice.
                    captureSnapshot(now);
                    continue;
                }
                timedOut = now >= deadline;
                idle = !timedOut;
                break;
            }
            if (cpu.stop != Stop::running) continue;
            if (init && linux.hasChildEntryAt(cpu.pc)) {
                if (auto event = linux.observeEntry(cpu.pc); !event.empty()) {
                    if (event.find(" entered /system/bin/app_process64 ") != std::string::npos)
                        zygotePid = linux.processId();
                    if (event.find(" entered /system/bin/surfaceflinger ") != std::string::npos ||
                        event.find(" entered /vendor/bin/hw/android.hardware.graphics.") != std::string::npos)
                        graphicsPids.push_back(linux.processId());
                    report << event;
                }
            }
            if (!reachedExecutable && cpu.pc == startup.executableEntry) {
                reachedExecutable = true;
                if (init) report << "  init entry reached after " << cpu.retired << " guest instructions\n";
            }
            // A run on a worker that has ended is looked at now, at the thread's next turn.
            auto taken = nativeExecution ? linux.takeNativeResult() : std::nullopt;
            // The instruction native code left to the interpreter is interpreted, whatever it is.
            bool leftToInterpreter = false;
            if (!taken && interpretNext) {
                interpretNext = false;
                leftToInterpreter = true;
            } else if (const bool stretching = stretchLeft && linux.threadId() == stretchThread;
                       taken || (!stretching && nativeExecution && linux.nativeAt(machine) && !(init && linux.hasChildEntryAt(cpu.pc)))) {
                if (!taken && linux.offloadNative(machine)) continue;
                // A thread that waits in a system call comes back to its svc whenever the wait is tried
                // again. Entering native code there only to leave it at once costs more than the call.
                const auto atCall = [&] {
                    const auto* code = machine.readInstruction(cpu.pc);
                    uint32_t next = 0;
                    if (code) std::memcpy(&next, code, sizeof(next));
                    return next == 0xd4000001;
                };
                const auto left = taken ? native::settle(machine, *taken) : atCall() ? native::Exit::syscall : native::run(machine);
                afterNative = true;
                if (left == native::Exit::fault && !(++nativeFaults & 0xffff)) {
                    // A sample of what native code could not do itself, for finding memory it cannot reach.
                    std::ostringstream sample;
                    uint64_t address = 0;
                    bool inRun = false;
                    const bool known = machine.space->guestAddress(native::lastFaultAddress(), address, &inRun);
                    sample << "native execution: fault sample PID=" << linux.processId() << " PC=0x" << std::hex << cpu.pc
                           << " address=0x" << (known ? address : 0) << (known ? inRun ? " (in a run)" : " (void or guard)" : " (unknown)");
                    for (const auto& segment : machine.segments) {
                        if (address < segment.address || address - segment.address >= segment.mappedSize()) continue;
                        sample << " segment 0x" << segment.address << "+0x" << segment.mappedSize() << " flags=" << segment.flags
                               << (segment.shared ? " shared" : "") << (segment.placed ? " placed" : "")
                               << (segment.anonymousPages ? segment.anonymousPages->guestSpace() ? " native-pages" : " pages" : "")
                               << (segment.imageView ? " view" : "") << (segment.beyondFileEnd ? " beyond-file-end" : "")
                               << (segment.anonymous ? " anonymous" : "") << " backed=" << machine.space->backed(address)
                               << " mapped=" << machine.space->mapped(address);
                        break;
                    }
                    if (faultSamples.size() > 4096) faultSamples.erase(0, faultSamples.find('\n', 2048) + 1);
                    faultSamples += "  " + sample.str() + '\n';
                }
                switch (left) {
                    case native::Exit::preempted:
                        // The scheduler counts instructions: a slice of native code counts as one of its own.
                        cpu.retired += linux.sliceLength;
                        linux.noteNativeTurnUsed();
                        break;
                    case native::Exit::lost:
                    case native::Exit::retry:
                        break;
                    default:
                        // An svc, or something else that the interpreter does in native code's place.
                        cpu.retired += 16;
                        interpretNext = true;
                        // NESTVANE_NO_STRETCH=1 goes back to native code after one instruction, for comparison.
                        static const bool stretches = !std::getenv("NESTVANE_NO_STRETCH");
                        // Only in apps: in Android's own processes a fault is mostly a single one, and a
                        // stretch there ran their translated code in the interpreter while Android started.
                        if (left == native::Exit::fault && stretches && linux.currentIsApp()) {
                            const auto now = steady_clock::now();
                            const bool again = linux.threadId() == stretchThread && now - lastNativeFault < milliseconds(2);
                            stretchLength = again ? std::min(stretchLength * 2, 65536U) : 256;
                            stretchThread = linux.threadId();
                            stretchLeft = stretchLength;
                            lastNativeFault = now;
                        }
                        break;
                }
                continue;
            }
            if (nativeExecution && machine.space && !machine.space->interpreted()) {
                // In a process that runs natively, what comes here is a library without a translation, or
                // one instruction that native code left to the interpreter. The library is run in a loop
                // of its own up to the next thing the code around this loop looks after: a system call,
                // the end of the thread's turn, translated code, a fault.
                bool ran = false, stretchEnded = false;
                const auto inPlaceStart = steady_clock::now();
                const auto retiredBefore = cpu.retired;
                // Instructions are fetched from a window onto the code, looked up again when pc leaves
                // it: nothing in this loop changes the mappings (a system call ends it).
                uint64_t windowStart = 0, windowEnd = 0;
                const uint8_t* window = nullptr;
                // Likewise the memory loads and stores go to.
                MemoryWindows dataWindows;
                for (;;) {
                    const uint8_t* code;
                    if (cpu.pc - windowStart < windowEnd - windowStart && !(cpu.pc & 3)) {
                        code = window + (cpu.pc - windowStart);
                    } else if (fastPath && machine.codeWindow(cpu.pc, windowStart, windowEnd, window)) {
                        code = window + (cpu.pc - windowStart);
                    } else {
                        windowStart = windowEnd = 0;
                        code = machine.readInstruction(cpu.pc);
                    }
                    if (!code) break;
                    std::memcpy(&word, code, sizeof(word));
                    // In translated code only on a stretch after a fault; and not for an exclusive or atomic
                    // access there, which other threads running natively beside this one would not see as one.
                    const bool translated = machine.space->hasCode(cpu.pc);
                    const bool forced = std::exchange(leftToInterpreter, false);
                    if (forced) {
                        // as before: this one instruction, then on as below
                    } else if (translated) {
                        const bool atomic = (word & 0x3f000000) == 0x08000000 || (word & 0x3b200c00) == 0x38200000;
                        if (!stretchLeft || linux.threadId() != stretchThread || atomic) {
                            if (stretchLeft && linux.threadId() == stretchThread) stretchEnded = true;
                            stretchLeft = 0;
                            break;
                        }
                        --stretchLeft;
                        ++inPlaceStretched;
                    }
                    // The common instructions from their decoded form; the rest from the full interpreter,
                    // here too: going round the outer loop for each, through the scheduler and its checks,
                    // cost more than the instruction itself. A system call, or an instruction the
                    // interpreter does not have (false, nothing done), goes round, where the app gets SIGILL.
                    if (const auto& fast = fastWord(word); fast.kind != FastInstruction::none) {
                        executeFast(machine, fast, &dataWindows);
                    } else if (word == 0xd4000001) {
                        break;
                    } else if (!executeStartupInstruction(machine, word)) {
                        // Translated code the interpreter does not have goes back to native code.
                        if (translated && !forced) { stretchLeft = 0; stretchEnded = true; }
                        break;
                    }
                    ran = true;
                    if (cpu.stop != Stop::running || !(cpu.retired & 4095) || !linux.hasSliceBudget(cpu.retired, 1)) break;
                }
                inPlaceTime += steady_clock::now() - inPlaceStart;
                inPlaceInstructions += cpu.retired - retiredBefore;
                // A thread that used its whole turn here goes on on a worker, beside the main loop, for as
                // long as its code is the kind a worker can interpret (FastPath.hpp, interpretInSpace).
                if (ran && cpu.stop == Stop::running && !linux.hasSliceBudget(cpu.retired, 1) && !machine.space->hasCode(cpu.pc)) {
                    // The whole code segment when the worker can fetch it through the address space; else the window.
                    uint64_t segmentStart = 0, segmentEnd = 0;
                    const auto code = machine.codeInSpace(cpu.pc, segmentStart, segmentEnd) ? SpaceCode{segmentStart, segmentEnd, nullptr}
                                    : window && cpu.pc - windowStart < windowEnd - windowStart ? SpaceCode{windowStart, windowEnd, window}
                                    : SpaceCode{};
                    if (code.end > code.start && linux.offloadInterpreter(machine, code)) continue;
                }
                if (ran || stretchEnded) continue;
            }
            const auto* bytes = machine.readInstruction(cpu.pc);
            if (!bytes) {
                machine.fault(cpu.pc, cpu.pc);
                continue;
            }
            if (cpu.retired >= nextSample) {
                ++samples;
                ++intervalSamples;
                nextSample = cpu.retired + sampleStride + ((samples * 0x9e37) & 0xffff);
                if (const auto location = linux.guestCodeFile(bytes)) {
                    const auto [path, offset] = *location;
                    const auto module = linux.guestCodeFileName(path);
                    ++codeSamples[{module, offset & ~uint64_t(4095)}];
                    ++intervalCodeSamples[{module, offset & ~uint64_t(63)}];
                } else {
                    ++unattributedSamples;
                    ++intervalUnattributed;
                }
            }
            if (const auto* block = bootAot ? bootAot->lookup(bytes) : nullptr;
                block && linux.hasSliceBudget(cpu.retired, block->words.size()) &&
                block->words.size() <= instructionLimit - cpu.retired) {
                block->execute(cpu);
                cpu.pc += block->words.size_bytes();
                cpu.retired += block->words.size();
                ++sha256AotEntries;
                sha256AotInstructions += block->words.size();
                continue;
            }
            std::memcpy(&word, bytes, sizeof(word));
            if (bootAot) {
                if (word == AndroidDexHashLoop::words.front() &&
                    runNativeLoop.operator()<AndroidDexHashLoop>(dexAot, bytes)) continue;
                if (word == AndroidDexAsciiVerifierLoop::words.front() &&
                    runNativeLoop.operator()<AndroidDexAsciiVerifierLoop>(dexVerifierAot, bytes)) continue;
                if (word == AndroidFontFamilyScan::words.front() &&
                    runNativeLoop.operator()<AndroidFontFamilyScan>(fontScanAot, bytes)) continue;
                if (word == AndroidArtGcBitmap::words.front() &&
                    runNativeLoop.operator()<AndroidArtGcBitmap>(artBitmapAot, bytes)) continue;
                if (word == AndroidArtAsciiScan::words.front() &&
                    runNativeLoop.operator()<AndroidArtAsciiScan>(artAsciiAot, bytes)) continue;
                if (word == AndroidArtGcReturn::words.front() &&
                    runNativeLoop.operator()<AndroidArtGcReturn>(artReturnAot, bytes)) continue;
                if (word == AndroidBionicFillLoop::words.front() &&
                    runNativeLoop.operator()<AndroidBionicFillLoop>(bionicFillAot, bytes)) continue;
                if (word == AndroidBionicCopyLoop::words.front() &&
                    runNativeLoop.operator()<AndroidBionicCopyLoop>(bionicCopyAot, bytes)) continue;
            }
            if (const auto& fast = fastWord(word); fast.kind != FastInstruction::none) {
                executeFast(machine, fast);
                continue;
            }
            auto& decoded = decodedWords[((word ^ (word >> 16)) * 0x9e3779b9U) >> 21];
            if (decoded.word != word) {
                decoded.word = word;
                decoded.instruction = decode(word);
            }
            const auto& instruction = decoded.instruction;
            if (instruction.op != Op::unsupported) {
                if (instruction.op != Op::svc) {
                    executeDecoded(machine, instruction);
                    continue;
                }
                const uint64_t syscall = cpu.x[8];
                ++syscallCount;
                bool trace = false;
                if (traceLimit) {
                    const bool repetitive = (syscall == 226 && protectionLogs++ >= (kind == ProbeKind::art ? 512U : 8U)) ||
                        (kind == ProbeKind::art && syscall == 227 && syncLogs++ >= 32);
                    const bool stackMemory = kind == ProbeKind::art && (syscall == 226 || syscall == 233) &&
                        cpu.x[0] >= 0x7f000000 && cpu.x[0] < 0x80000000;
                    bool zygoteTrace = false;
                    if (zygotePid && syscallCount > traceLimit && linux.processId() == zygotePid) {
                        const auto count = zygoteSyscalls++;
                        const bool memoryFile = syscall == 279 || (syscall == 25 && (cpu.x[1] == 1033 || cpu.x[1] == 1034));
                        zygoteTrace = count < 2048 || (syscall == 222 && zygoteMappings++ < 256) ||
                                      (memoryFile && zygoteMemoryFiles++ < 40) ||
                                      (count % 1'000'000 == 0 && zygoteSamples++ < 128);
                    } else if (zygotePid && syscallCount > traceLimit && linux.parentProcessId() == zygotePid) {
                        zygoteTrace = zygoteChildSyscalls++ < 2048;
                        if (!zygoteTrace && zygoteChildPolicyCalls < 64) {
                            const bool securityControl = syscall == 167 && (cpu.x[0] == 22 || cpu.x[0] == 38);
                            const auto path = syscall == 56 || syscall == 79 ? pathArgument(machine, cpu.x[1]) : std::string{};
                            zygoteTrace = securityControl || (syscall == 280 && cpu.x[0] == 7) || path.ends_with("_seapp_contexts") ||
                                path.ends_with("/attr/current") || path == "/sys/fs/selinux/context" || path.starts_with("/dev/ashmem");
                            zygoteChildPolicyCalls += zygoteTrace;
                        }
                    }
                    const bool graphicsTrace = graphicsCalls < 512 && syscallCount > traceLimit &&
                        (syscall == 56 || syscall == 29) &&
                        std::ranges::find(graphicsPids, linux.processId()) != graphicsPids.end();
                    graphicsCalls += graphicsTrace;
                    trace = stackMemory || zygoteTrace || graphicsTrace || (!repetitive && syscallCount <= traceLimit);
                }
                if (!trace) ++suppressedLogs;
                if (trace) {
                    describeCall(report, cpu, syscall);
                    if (linux.threadId() != 1 && init) report << " tid=" << linux.threadId();
                    switch (syscall) {
                        case 36:
                            report << " link=" << pathArgument(machine, cpu.x[2])
                                   << " points_to=" << pathArgument(machine, cpu.x[0]);
                            break;
                        case 39: case 43: case 221: case 279:
                            report << " path=" << pathArgument(machine, cpu.x[0]);
                            break;
                        case 280:
                            if (cpu.x[0] == 7 && cpu.x[2] >= sizeof(uint64_t)) {
                                uint64_t name;
                                if (machine.load64(cpu.x[1], name)) report << " path=" << pathArgument(machine, name);
                            }
                            break;
                        case 33: case 34: case 35: case 48: case 53: case 54: case 56: case 60: case 78: case 79:
                            report << " path=" << pathArgument(machine, cpu.x[1]);
                            break;
                        case 200: case 203:
                            if (cpu.x[2] > 2 && cpu.x[1] <= UINT64_MAX - 2)
                                report << " unix_path=" << pathArgument(machine, cpu.x[1] + 2);
                            break;
                        case 40:
                            report << " source=" << pathArgument(machine, cpu.x[0])
                                   << " target=" << pathArgument(machine, cpu.x[1])
                                   << " type=" << pathArgument(machine, cpu.x[2]);
                            break;
                        default:
                            break;
                    }
                }
                auto& timing = intervalSyscalls[std::min<uint64_t>(syscall, otherSyscall)];
                if (sampleBoot) ++timing.calls;
                const bool timeSyscall = syscallCount >= nextSyscallSample;
                const auto syscallStart = timeSyscall ? steady_clock::now() : steady_clock::time_point{};
                const auto callBegan = nativeExecution ? steady_clock::now() : steady_clock::time_point{};
                // NESTVANE_TRACE_PROGRAM=<part of a program's path> puts that program's system calls into
                // the Android log: the number, the first four arguments, and what came back.
                static const char* const tracedProgram = std::getenv("NESTVANE_TRACE_PROGRAM");
                const bool traceCall = tracedProgram && linux.androidLogObserver && !linux.blocked() &&
                    linux.currentExecutable().find(tracedProgram) != std::string::npos;
                const std::array<uint64_t, 4> callArguments{cpu.x[0], cpu.x[1], cpu.x[2], cpu.x[3]};
                const auto callProcess = traceCall ? linux.processId() : 0;
                bool handled;
                const auto registersBefore = cpu.x;
                try {
                    handled = linux.syscall(machine);
                } catch (const std::bad_alloc&) {
                    // The host had no memory for what this call needed. Room is made as for a new process (the
                    // cached app that matters least ends) and the call runs once more; if that fails too, the caller
                    // is told so, as Linux would. Letting it go on unwinding ended all of Android.
                    bool again = false;
                    if (linux.makeRoom()) {
                        cpu.x = registersBefore;
                        try {
                            handled = linux.syscall(machine);
                            again = true;
                        } catch (const std::bad_alloc&) {}
                    }
                    if (!again) {
                        cpu.x = registersBefore;
                        cpu.x[0] = uint64_t(-12);
                        handled = true;
                    }
                    if (linux.androidLogObserver) {
                        std::ostringstream event;
                        event << "Nestvane: system call " << syscall << " in PID " << linux.processId()
                              << (again ? " found no host memory; made room and ran it again" : " found no host memory; told ENOMEM");
                        linux.androidLogObserver(event.str());
                    }
                }
                if (traceCall) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "native execution: call %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx) in PID %llu -> %s0x%llx",
                                  static_cast<unsigned long long>(syscall), static_cast<unsigned long long>(callArguments[0]),
                                  static_cast<unsigned long long>(callArguments[1]), static_cast<unsigned long long>(callArguments[2]),
                                  static_cast<unsigned long long>(callArguments[3]), static_cast<unsigned long long>(callProcess),
                                  !handled ? "not handled " : linux.blocked() ? "waits " : "", static_cast<unsigned long long>(cpu.x[0]));
                    linux.androidLogObserver(line);
                }
                // NESTVANE_TRACE_FAILED=<part of an SELinux domain> puts the calls that fail in processes of
                // that domain into the Android log, such as isolated_app for WebView's renderers.
                if (static const char* const failedDomain = std::getenv("NESTVANE_TRACE_FAILED");
                    failedDomain && handled && !linux.blocked() && linux.androidLogObserver &&
                    (cpu.x[0] >= uint64_t(-4095) || ((syscall == 222 || syscall == 215 || syscall == 216 || syscall == 226 ||
                                                      syscall == 233) && callArguments[1] >= 1024 * 1024)) &&
                    linux.currentContext().find(failedDomain) != std::string::npos) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "native execution: traced call %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx) in PID %llu -> 0x%llx",
                                  static_cast<unsigned long long>(syscall), static_cast<unsigned long long>(callArguments[0]),
                                  static_cast<unsigned long long>(callArguments[1]), static_cast<unsigned long long>(callArguments[2]),
                                  static_cast<unsigned long long>(callArguments[3]), static_cast<unsigned long long>(linux.processId()),
                                  static_cast<unsigned long long>(cpu.x[0]));
                    // A Unix socket's path, for connect and bind.
                    const auto path = (syscall == 203 || syscall == 200) ? pathArgument(machine, callArguments[1] + 2) : std::string();
                    linux.androidLogObserver(path.empty() ? std::string(line) : std::string(line) + " " + path);
                }
                // NESTVANE_TRACE_DENIED=1: the calls that end in "permission denied", with where the path leads.
                if (static const bool traced = std::getenv("NESTVANE_TRACE_DENIED"); traced && cpu.x[0] == uint64_t(-13) && linux.androidLogObserver) {
                    const auto path = pathArgument(machine, cpu.x[1]);
                    linux.androidLogObserver("native execution: denied call " + std::to_string(syscall) + " " + path + " " + linux.pathReport(path));
                }
                if (nativeExecution) {
                    const auto spent = steady_clock::now() - callBegan;
                    syscallTime += spent;
                    auto& slot = syscallCost[std::min<uint64_t>(syscall, syscallCost.size() - 1)];
                    slot.first += spent;
                    ++slot.second;
                }
                if (timeSyscall) {
                    timing.nanoseconds += duration_cast<nanoseconds>(steady_clock::now() - syscallStart).count();
                    ++timing.samples;
                    ++intervalSyscallSamples;
                    ++syscallSamples;
                    nextSyscallSample = syscallCount + syscallSampleStride + ((syscallSamples * 0x9e37) & 255);
                }
                if (handled) {
                    if (linux.blocked()) {
                        if (trace) report << " [blocked]\n";
                        continue;
                    }
                    if (linux.replacedImage()) {
                        ++cpu.retired;
                        if (trace) report << " [loaded guest image]\n";
                        continue;
                    }
                    if (linux.restoredSignalContext()) {
                        ++cpu.retired;
                        if (trace) report << " [restored signal context]\n";
                        if (restoredSignals++ < 32)
                            report << "  guest rt_sigreturn restored TID=" << linux.threadId()
                                   << " PC=0x" << std::hex << cpu.pc << std::dec << '\n';
                        continue;
                    }
                    if (cpu.stop == Stop::fault) {
                        ++cpu.retired;
                        if (trace) report << " [fault]\n";
                        continue;
                    }
                    cpu.pc += 4;
                    ++cpu.retired;
                } else if (syscall == 93 || syscall == 94) {
                    executeDecoded(machine, instruction);
                } else {
                    if (!trace) describeCall(report, cpu, syscall);
                    const auto caller = linux.processId();
                    if (linux.suspendChild()) {
                        report << " [unimplemented in guest PID " << caller << ", suspended]\n";
                        // In Android's log too, with what its first argument is when that is a descriptor: a
                        // suspended process can hold up others that wait for it, far from where it stopped.
                        // Once for each process and call: a signal wakes a suspended thread, and it tries again.
                        static std::set<std::pair<uint32_t, uint64_t>> reported;
                        if (linux.androidLogObserver && reported.size() < 4096 && reported.emplace(caller, syscall).second) {
                            std::ostringstream event;
                            event << "Nestvane: system call " << syscall << " not implemented, PID " << caller << " suspended:" << std::hex;
                            for (int i = 0; i < 6; ++i) event << " x" << i << "=0x" << cpu.x[i];
                            event << std::dec << "; x0 is " << linux.describeDescriptor(cpu.x[0]);
                            if (syscall == 21) event << "; x2 is " << linux.describeDescriptor(cpu.x[2]);  // epoll_ctl's target
                            linux.androidLogObserver(event.str());
                        }
                        continue;
                    }
                    report << " [unimplemented]\n";
                    unhandled = syscall;
                    cpu.stop = Stop::unsupported;
                    break;
                }
                if (trace) report << " result=0x" << std::hex << cpu.x[0] << std::dec << '\n';
            } else if (!executeStartupInstruction(machine, word)) {
                // An instruction the interpreter does not have, in an app's own library: that app gets
                // SIGILL and Android goes on. It used to end the whole run, and the screen stood still.
                const auto pc = cpu.pc;
                const auto process = linux.processId(), thread = linux.threadId();
                // The callers, by the frame records, before the signal frame changes the registers.
                std::ostringstream callers;
                callers << std::hex << " LR=0x" << cpu.x[30];
                for (uint64_t frame = cpu.x[29], depth = 0; frame && depth < 8; ++depth) {
                    std::array<uint64_t, 2> record{};
                    if (!machine.readInto(frame, {reinterpret_cast<uint8_t*>(record.data()), sizeof(record)})) break;
                    callers << " <- 0x" << record[1];
                    if (record[0] <= frame) break;
                    frame = record[0];
                }
                // NESTVANE_TRAP_DUMP=[*]<address>:<bytes> adds guest memory to the report, at the address or,
                // with *, at the address read from there.
                if (const char* dump = std::getenv("NESTVANE_TRAP_DUMP")) {
                    const bool indirect = *dump == '*';
                    uint64_t address = std::strtoull(dump + indirect, nullptr, 16);
                    const char* colon = std::strchr(dump, ':');
                    const uint64_t bytes = std::min<uint64_t>(colon ? std::strtoull(colon + 1, nullptr, 10) : 16, 64);
                    if (indirect && !machine.readInto(address, {reinterpret_cast<uint8_t*>(&address), sizeof(address)})) address = 0;
                    std::vector<uint8_t> memory(bytes);
                    callers << " memory@0x" << address << ":";
                    if (address && machine.readInto(address, memory))
                        for (const auto byte : memory) callers << ' ' << unsigned(byte);
                }
                if (init && linux.deliverIllegalInstruction(machine)) {
                    std::ostringstream message;
                    message << ((word & 0xffe0001f) == 0xd4200000 || (word & 0xffe0001f) == 0xd4400000 ? "native execution: breakpoint instruction, SIGTRAP to PID="
                                                                   : "native execution: no such instruction in the interpreter, SIGILL to PID=") << process
                            << " TID=" << thread << std::hex << " PC=0x" << pc << " word=0x" << word << std::dec << callers.str();
                    if (illegalInstructions++ < 32) report << "  " << message.str() << '\n';
                    if (linux.androidLogObserver) linux.androidLogObserver(message.str());
                    continue;
                }
                cpu.stop = Stop::unsupported;
            }
        } catch (const std::bad_alloc&) {
            // Init cannot be ended: the app that matters least is, and init tries again.
            if (!init || !(linux.endCurrentProcess(9, "no host memory for it") || linux.endAppForMemory())) throw;
            cpu.stop = Stop::running;
        }
    }

    if (init) appendBootProfile(report, duration_cast<milliseconds>(steady_clock::now() - start).count());
    if (suppressedLogs) report << "  omitted syscall log entries: " << suppressedLogs << '\n';
    if (deliveredFaults || restoredSignals)
        report << "  guest SIGSEGV deliveries: " << deliveredFaults << ", signal context returns: " << restoredSignals << '\n';
    report << "  retired guest instructions: " << cpu.retired << ", guest syscalls observed: "
           << syscallCount << ", thread pointer: 0x" << std::hex << cpu.threadPointer << std::dec << '\n';
    if (init && (unhandled || cpu.stop == Stop::fault || cpu.stop == Stop::unsupported))
        report << "  stopped guest PID=" << linux.processId() << " TID=" << linux.threadId() << '\n';
    if (unhandled) {
        report << "  stopped at unimplemented guest syscall " << *unhandled << '\n';
    } else if (cpu.stop == Stop::fault) {
        report << std::hex << "  guest memory fault at PC=0x" << cpu.pc << " address=0x" << cpu.faultAddress
               << " word=0x" << word << " SP=0x" << cpu.sp << " LR=0x" << cpu.x[30] << std::dec
               << " TID=" << linux.threadId() << " x0=0x" << std::hex << cpu.x[0] << std::dec << '\n';
    } else if (cpu.stop == Stop::unsupported) {
        report << std::hex << "  unsupported instruction at PC=0x" << cpu.pc << " word=0x" << word;
        if (cpu.pc >= startup.interpreterBase && std::any_of(image.segments.begin(), image.segments.end(),
                [&](const Segment& segment) {
                    return cpu.pc >= segment.address && cpu.pc - segment.address < segment.mappedSize();
                })) {
            report << " linker offset=0x" << cpu.pc - startup.interpreterBase;
        }
        report << std::dec << '\n';
    } else if (cpu.stop == Stop::exit) {
        report << "  guest exit code: " << cpu.exitCode << '\n';
    } else if (stop.stop_requested()) {
        report << "  Android session stopped\n";
    } else if (timedOut) {
        report << "  diagnostic wall-time deadline reached\n";
    } else if (idle) {
        report << "  no runnable guest threads\n";
    } else {
        report << "  guest instruction limit reached at PC=0x" << std::hex << cpu.pc << std::dec << '\n';
    }
    if (!linux.watchdogReport.empty())
        report << "  guest threads at Android Watchdog termination:\n" << linux.watchdogReport;
    if (nativeExecution) report << native::statistics() << timeReport() << faultSamples;
    else if (native::failure()) report << "  native execution is off: " << native::failure() << '\n';
    report << linux.workerReport(machine,
        init && (unhandled || cpu.stop == Stop::fault || cpu.stop == Stop::unsupported) ? linux.processId() : 0);
    if (!linux.stderrOutput.empty()) report << "  guest stderr: " << linux.stderrOutput;
    if (!linux.kernelOutput.empty()) report << "  guest kernel log:\n" << linux.kernelOutput;
    if (!linux.recentKernelOutput.empty())
        report << "  recent guest kernel log:\n" << linux.recentKernelOutput;
    if (!linux.androidOutput.empty()) report << "  guest Android log:\n" << linux.androidOutput;
    if (!linux.recentAndroidOutput.empty())
        report << "  recent guest Android log:\n" << linux.recentAndroidOutput;
    if (!cpu.output.empty()) report << "  guest output: " << cpu.output;
    report << "  executable reached: " << (reachedExecutable ? "yes" : "no") << '\n';
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    report << "  guest execution wall time: " << elapsed.count() << " ms\n";
    if (kind == ProbeKind::art) {
        uint64_t resident = 0;
        for (const auto& area : machine.segments) resident += area.bytes.size();
        report << "  resident dense private guest mapping bytes: " << resident << '\n';
    }
    if (kind == ProbeKind::art || init)
        report << "  dirty anonymous guest backing bytes (all guest processes): " << linux.dirtyAnonymousBytes() << '\n';
    const bool initStarted = linux.kernelOutput.find("<6>init: init second stage started!\n") != std::string::npos;
    if (init) report << "  second-stage startup log observed: " << (initStarted ? "yes" : "no") << '\n';
    const auto expectedOutput = kind == ProbeKind::art ? "nestvane ART computed 42\n" :
        kind == ProbeKind::serviceReady ? "37\narm64-v8a\nRuntime APEX ELF verified\ntrue\n" :
        kind == ProbeKind::runtimeApex ? "37\narm64-v8a\nRuntime APEX ELF verified\n" :
        kind == ProbeKind::bootGate ? expectedGate :
        kind == ProbeKind::properties ? "37\narm64-v8a\n" : "nestvane bionic ready!\n";
    const bool matched = kind == ProbeKind::data ? cpu.output == "guest data synced\n" ||
        cpu.output == "guest data loaded\n" : cpu.output == expectedOutput;
    const bool passed = reachedExecutable && (init ? initStarted : cpu.stop == Stop::exit && cpu.exitCode == 0 && matched);
    return {passed, report.str()};
}

} // namespace

DynamicReport probeDynamic(const std::string& directory) {
    try {
        auto executable = readFile(directory + "/android17-dynamic.elf");
        auto linker = readFile(directory + "/android17-linker64.elf");
        auto libc = readFile(directory + "/android17-libc.so");
        auto libdl = readFile(directory + "/android17-libdl.so");
        constexpr auto filename = "/data/local/tmp/nestvane-p2";
        const auto image = loadDynamicELF(executable, linker, filename);
        ProbeLinux linux;
        linux.mount(filename, executable);
        linux.alias("/etc", "/system/etc");
        linux.alias("/proc/self/exe", filename);
        linux.mount("/apex/com.android.runtime/bin/linker64", linker);
        linux.alias("/system/bin/linker64", "/apex/com.android.runtime/bin/linker64");
        linux.mount("/apex/com.android.runtime/lib64/bionic/libc.so", libc);
        linux.alias("/system/lib64/libc.so", "/apex/com.android.runtime/lib64/bionic/libc.so");
        linux.mount("/apex/com.android.runtime/lib64/bionic/libdl.so", libdl);
        linux.alias("/system/lib64/libdl.so", "/apex/com.android.runtime/lib64/bionic/libdl.so");
        return runProbe(image, linux, ProbeKind::dynamic);
    } catch (const std::exception& error) {
        return {false, "Android 17 dynamic ELF probe\n  load error: " + std::string(error.what()) + "\n"};
    }
}

DynamicReport probeArt(const std::string& directory) {
    try {
        namespace fs = std::filesystem;
        constexpr std::string_view jitless = "-Xint";
        constexpr auto executable = "/apex/com.android.art/bin/dalvikvm64";
        constexpr auto interpreter = "/apex/com.android.runtime/bin/linker64";
        constexpr auto classPath = "/data/local/tmp/nestvane-art-probe.dex";
        constexpr auto bootClassPath =
            "-Xbootclasspath:/apex/com.android.art/javalib/core-oj.jar:"
            "/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:"
            "/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:"
            "/apex/com.android.i18n/javalib/core-icu4j.jar";
        std::map<std::string, ReadOnlyImageFile, std::less<>> files;
        uint64_t remaining = 256 * 1024 * 1024;
        const fs::path root(directory);
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            const auto type = entry.symlink_status();
            if (fs::is_directory(type)) continue;
            if (!fs::is_regular_file(type) || files.size() >= 1024)
                throw std::runtime_error("invalid ART guest root");
            const auto size = entry.file_size();
            if (size > remaining) throw std::runtime_error("ART guest root exceeds its size limit");
            remaining -= size;
            files.try_emplace("/" + entry.path().lexically_relative(root).generic_string(), entry.path(), size);
        }
        const bool bootImage = files.contains("/system/framework/arm64/boot.art");
        std::vector<std::string> arguments{std::string(jitless), "-Xms8m", "-Xmx32m",
                                           "-Xnoimage-dex2oat", bootClassPath};
        if (bootImage) arguments.emplace_back("-Ximage:/system/framework/boot.art");
        arguments.insert(arguments.end(), {"-cp", classPath, "NestvaneArt"});
        auto image = loadDynamicELF(files.at(executable).bytes(), files.at(interpreter).bytes(), executable, "/system/bin/linker64",
                                    std::move(arguments), 0);
        image.startup->environment = {"ANDROID_ROOT=/system", "ANDROID_DATA=/data", "ANDROID_ART_ROOT=/apex/com.android.art",
            "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_RUNTIME_ROOT=/apex/com.android.runtime",
            "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata", "LANG=en_US.UTF-8"};
        ProbeLinux linux(1, 0, 512 * 1024 * 1024);
        linux.captureAndroidLogs = true;
        for (const auto& [path, file] : files) linux.mount(path, file.bytes());
        linux.setBootContext("u:r:shell:s0");
        linux.addBootTmpfs("/data", 0771, 1000, 6);
        linux.alias("/proc/self/exe", executable);
        linux.alias("/system/bin/linker64", interpreter);
        linux.alias("/system/lib64/ld-android.so", interpreter);
        for (const auto file : {"libc.so", "libdl.so", "libm.so"}) {
            linux.alias(std::string("/system/lib64/") + file, std::string("/apex/com.android.runtime/lib64/bionic/") + file);
        }
        return runProbe(image, linux, ProbeKind::art);
    } catch (const std::exception& error) {
        return {false, "Android 17 original ART probe\n  load error: " + std::string(error.what()) + "\n"};
    }
}

DynamicReport probeInit(const std::string& directory, const std::string& databasePath, const BootProgress& progress,
                        GraphicsPipeFactory graphics, std::shared_ptr<GuestTouch> touch,
                        std::shared_ptr<GuestCameraSource> camera, std::shared_ptr<GuestApkInbox> apkInbox,
                        std::stop_token stop, const BootProgress& watchdog, const BootProgress& liveSnapshot,
                        std::shared_ptr<GuestColorDma> graphicsDma) {
    try {
        if (progress) progress("Starting Android 17 init");
        namespace fs = std::filesystem;
        const fs::path root(directory);
        constexpr auto filename = "/system/bin/init";
        constexpr auto interpreter = "/system/bin/bootstrap/linker64";
        std::map<std::string, ReadOnlyImageFile, std::less<>> files;
        // File-backed input includes the offline ART boot and system-server artifacts.
        constexpr uint64_t maxRootBytes = 4096ULL * 1024 * 1024;  // the files are mapped, not read
        uint64_t total = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            const auto type = entry.symlink_status();
            if (fs::is_directory(type)) continue;
            if (!fs::is_regular_file(type)) throw std::runtime_error("expected an unpacked regular-file guest root");
            const auto size = entry.file_size();
            if (files.size() >= 4096 || size > maxRootBytes - total) {
                throw std::runtime_error("init guest root exceeds the probe limits");
            }
            total += size;
            const auto guestPath = "/" + entry.path().lexically_relative(root).generic_string();
            files.try_emplace(guestPath, entry.path(), size);
        }
        DynamicReport storage{true, {}};
        if (!databasePath.empty()) {
            constexpr auto dataProbe = "/system/bin/nestvane-guest-data";
            ProbeLinux writer(1, 0);
            for (const auto& [path, file] : files) writer.mount(path, file.bytes());
            if (const char* shared = std::getenv("NESTVANE_SHARED_STORAGE"); shared && *shared) writer.shareStorage(shared);
            writer.mountData(databasePath);
            const auto dataImage = loadDynamicELF(files.at(dataProbe).bytes(), files.at(interpreter).bytes(), dataProbe, interpreter);
            storage = runProbe(dataImage, writer, ProbeKind::data);
            if (!storage.passed) return storage;
        }
        const auto image = loadDynamicELF(files.at(filename).bytes(), files.at(interpreter).bytes(), filename,
                                          interpreter, {"second_stage"}, 0);
        ProbeLinux linux(1, 0, 512 * 1024 * 1024);
        const auto startupTouch = graphics ? touch : nullptr;
        const bool wakeGuestDisplay = startupTouch && files.contains("/system/bin/cmd");
        linux.setGraphicsPipeFactory(std::move(graphics));
        linux.setGraphicsDma(std::move(graphicsDma));
        linux.setCamera(std::move(camera));
        linux.setGuestTouch(std::move(touch));
        if (const auto loader = files.find("/nestvane-metadata/tethering-netbpfload.elf"); loader != files.end()) {
            const auto bytes = loader->second.bytes();
            if (bytes.size() > 1024 * 1024 || elfInterpreter(bytes) != "/system/bin/linker64")
                throw std::runtime_error("invalid staged Tethering loader");
            linux.setTetheringLoader(bytes);
        }
        linux.captureAndroidLogs = true;
        linux.watchdogObserver = watchdog;
        // NESTVANE_NO_NETWORK=1 leaves the guest with its loopback only.
        if (!std::getenv("NESTVANE_NO_NETWORK")) linux.enableHostNetwork();
        linux.enableBattery();
        if (!std::getenv("NESTVANE_NO_SOUND")) linux.enableSound();
        const bool expectSmoke = files.contains("/system/etc/nestvane/smoke.apk");
        if (apkInbox || expectSmoke) {
            for (const auto input : {GuestApkInbox::installer, std::string_view("/system/bin/cmd"),
                                      std::string_view("/system/bin/sh"), std::string_view("/system/bin/dumpsys"),
                                      std::string_view("/system/bin/toybox"),
                                      std::string_view("/system/etc/nestvane/apk-ready.sh"),
                                      std::string_view("/system/etc/nestvane/text.sh"),
                                      std::string_view("/system/etc/nestvane/sms.sh"),
                                      std::string_view("/system/framework/nestvane-sms.jar"),
                                      std::string_view("/system/etc/nestvane/rotation.sh"),
                                      std::string_view("/system/framework/nestvane-rotation.jar")}) {
                if (!files.contains(input))
                    throw std::runtime_error("missing Android APK installer input: " + std::string(input));
            }
        }
        if (apkInbox) {
            linux.setApkInbox(std::move(apkInbox));
            static constexpr std::string_view installer = R"rc(
on property:sys.boot_completed=1
    start nestvane-apk-install

service nestvane-apk-install /system/bin/sh /system/etc/nestvane/apk-ready.sh
    user shell
    group shell
    seclabel u:r:shell:s0
    disabled
    stdio_to_kmsg

# Text messages the person writes in the app (GuestApkInbox::submitText).
on property:sys.boot_completed=1
    start nestvane-text

service nestvane-text /system/bin/sh /system/etc/nestvane/text.sh
    user shell
    group shell
    seclabel u:r:shell:s0
    disabled
    stdio_to_kmsg

# The broadcast that tells apps a text message arrived (GuestApkInbox::takeBroadcast). Root, because
# only the system may send it.
on property:sys.boot_completed=1
    start nestvane-sms

service nestvane-sms /system/bin/sh /system/etc/nestvane/sms.sh
    user root
    group root
    seclabel u:r:shell:s0
    disabled
    stdio_to_kmsg

# Which way up the display is, for the app that shows it (tools/rotation-watch).
on property:sys.boot_completed=1
    start nestvane-rotation

service nestvane-rotation /system/bin/sh /system/etc/nestvane/rotation.sh
    user shell
    group shell
    seclabel u:r:shell:s0
    disabled
    stdio_to_kmsg

on property:sys.boot_completed=1
    start nestvane-trim

# The guest keeps the host's time zone (NESTVANE_TIMEZONE, by way of the boot configuration): after
# the saved properties are loaded, so that it is the host's that counts, and before anything reads it.
on load_persist_props_action
    setprop persist.sys.timezone ${ro.boot.nestvane.timezone}

# The display's density, when the host gives one (NESTVANE_DISPLAY_DENSITY): before the compositor
# starts, which is what reads it.
on early-init && property:ro.boot.nestvane.density=*
    setprop qemu.sf.lcd_density ${ro.boot.nestvane.density}

service nestvane-trim /system/bin/sh /system/etc/nestvane/trim.sh
    user root
    group root
    seclabel u:r:shell:s0
    disabled
    oneshot
    stdio_to_kmsg
)rc";
            linux.mount("/system/etc/init/nestvane-apk-install.rc",
                        {reinterpret_cast<const uint8_t*>(installer.data()), installer.size()});
            linux.alias("/dev/kmsg_debug", "/dev/kmsg");
            if (!std::getenv("NESTVANE_NO_TOYBOX_LINKS")) linux.addToyboxLinks();
            // Built-in root, when the person has turned it on.
            if (auto root = ProbeLinux::rootPolicy(); root.enabled && root.enabled()) {
                linux.enableRoot();
                linux.rootDecision = std::move(root.decision);
                linux.rootAnswers = std::move(root.answers);
                linux.rootRemember = std::move(root.remember);
            }
        }
        for (const bool directory : {false, true}) {
            const auto manifest = directory ? "/nestvane-metadata/selinux-package-relabel-dir.txt" :
                                              "/nestvane-metadata/selinux-package-relabel-file.txt";
            if (const auto part = files.find(manifest); part != files.end()) {
                const auto bytes = part->second.bytes();
                if (!linux.loadPackageRelabelPolicy({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, directory))
                    throw std::runtime_error("invalid guest package relabel policy");
            }
        }
        bool smokeActivityOpened = false;
        bool bootBroadcastCompleted = false, displayInactive = false, launcherDrawn = false, wakeSent = false;
        if (progress || expectSmoke || startupTouch) linux.androidLogObserver = [&](std::string_view message) {
            const auto line = message.substr(0, message.find('\n'));
            constexpr std::string_view bootBroadcast =
                "SystemServerTiming: broadcastIntent-0-android.intent.action.BOOT_COMPLETED took to complete:";
            constexpr std::string_view launcherDisplay = "ActivityTaskManager: Displayed com.android.launcher3/";
            const bool viewportChanged = line.starts_with("InputManager-JNI: Viewport [0] to add: local:");
            if (viewportChanged) displayInactive = line.ends_with("isActive: false");
            if (line.starts_with(bootBroadcast)) bootBroadcastCompleted = true;
            if (line.starts_with(launcherDisplay) && !displayInactive) launcherDrawn = true;
            if (line.starts_with(launcherDisplay)) {
                linux.appsHeld = false;
                if (progress) {
                    const auto volume = ProbeLinux::hostVolume();
                    progress("Nestvane: host volume " + std::to_string(volume.total) + " bytes, " +
                             std::to_string(volume.available) + " available");
                }
            }
            const bool queueWake = startupTouch && !wakeSent && !launcherDrawn && bootBroadcastCompleted &&
                displayInactive && (viewportChanged || line.starts_with(bootBroadcast));
            if (queueWake && startupTouch->press(GuestTouch::Key::wake)) {
                wakeSent = true;
                if (progress) progress("Queued WAKEUP key for inactive Android startup display");
            }
            if (expectSmoke && message.starts_with("NestvaneSmoke: Opened activity with count "))
                smokeActivityOpened = true;
            if (!progress) return;
            // NESTVANE_LOG_ALL=1 passes every line on, for finding what a boot waits for.
            // NESTVANE_LOG_ALL=full keeps the lines after the first too: the stack under an exception.
            if (static const char* const all = std::getenv("NESTVANE_LOG_ALL"); all) {
                static const bool whole = std::string_view(all) == "full";
                progress(whole ? message.substr(0, 256 * 1024) : message.substr(0, message.find('\n')));
                return;
            }
            // NESTVANE_LOG_MATCH=<part>,<part>... passes on the lines that have one of the parts, when
            // everything would be too much to keep.
            if (static const char* const wanted = std::getenv("NESTVANE_LOG_MATCH"); wanted) {
                for (std::string_view parts = wanted; !parts.empty();) {
                    const auto part = parts.substr(0, parts.find(','));
                    if (!part.empty() && message.find(part) != std::string_view::npos) {
                        progress(message.substr(0, 256 * 1024));  // all of it: a thread dump is one message
                        return;
                    }
                    parts.remove_prefix(std::min(parts.size(), part.size() + 1));
                }
            }
            constexpr std::string_view milestones[] = {
                "Zygote: Preloading classes", "Zygote: ...preloaded", "Zygote: end preload",
                "Resources: Preloading resources",
                "nativeloader: Load libperfetto_framework_jni.so", "nativeloader: Load libcompiler_rt.so",
                "SystemServer: Entered the Android system server", "SystemServerTiming: ",
                "PackageManagerTiming: ", "EventHub: New device:", "HealthLoop: ", "Binder capacity: ",
                "mmap APK: ", "guest fork EAGAIN: ", "Zygote: Failed to fork child process:",
                "PackageManager: Failed to parse", "PackageManager: Time to scan packages",
                "PackageManager: Finished scanning system apps.",
                "PackageManager: Failed to create app data", "PackageManager: reconcileAppsData",
                "installd: Failed to restorecon", "ActivityTaskManager: Displayed com.android.launcher3/",
                "CameraProviderManager: Adding new camera device:", "CameraService: addStates",
                "NestvaneSmoke: ", "Nestvane: ended ", "Nestvane: system call ", "Nestvane: SIGSEGV ", "ActivityTaskManager: Displayed dev.nestvane.smoke/",
                "ActivityTaskManager: Displayed com.android.settings/.FallbackHome",
                "SystemUIFactory: Failed to initialize WMComponent", "libc: Fatal signal ",
                "Guest native abort: ", "InputTransport: ", "InputDispatcher: ", "InputManager-JNI: ",
                "PowerManagerService: Waking up", "PowerManagerService: Going to sleep",
                "SurfaceFlinger: SurfaceFlinger is starting", "SurfaceFlinger: Using HWComposer service",
                "SurfaceFlinger: OpenGL ES informations", "mapper.ranchu: GoldfishAddressSpace",
                "SurfaceFlinger: Can't create AidlComposerClient", "SurfaceFlinger: Loaded AIDL composer3 HAL",
                "SurfaceFlinger: Connecting display", "RanchuHwc: init display",
                "RanchuHwc: createHostComposerDisplayInfo", "GraphicBufferAllocator: Failed to allocate",
                "BootAnimation: BootAnimationStartTiming", "BootAnimation: BootAnimationShownTiming",
                "BootAnimation: Unable to", "BootAnimation: egl", "guest SIGSEGV delivered",
                "libEGL: couldn't find an OpenGL ES implementation", "libEGL: eglInitialize(",
                "Watchdog: *** WATCHDOG KILLING SYSTEM PROCESS",
                // What happens to an app after the boot: its process, its activity, its death.
                "ActivityTaskManager: ", "ActivityManager: Start proc", "ActivityManager: Process ",
                "ActivityManager: ANR", "ActivityManager: Killing", "AndroidRuntime: ", "DEBUG: ",
                "PackageInstaller", "PackageManager: Package ", "installd: ", "Zygote: Process ",
                "native execution: ", "Nestvane: KernelSU", "Nestvane: root granted", "KernelSU: "
            };
            if (message.starts_with("Zygote: System zygote died") ||
                message.starts_with("SystemServer: Failure") ||
                message.starts_with("AndroidRuntime: FATAL EXCEPTION")) {
                progress(message.substr(0, 2048));
                return;
            }
            const bool graphicsTransportFailure = message.starts_with("GFXSTREAM: ") &&
                (message.find("Failed to connect to host") != std::string_view::npos ||
                 message.find("Could not open '/dev/goldfish") != std::string_view::npos);
            const bool launcherProcess = line.starts_with("ActivityManager: ") &&
                line.find("com.android.launcher3") != std::string_view::npos;
            if (launcherProcess || graphicsTransportFailure || std::any_of(std::begin(milestones), std::end(milestones),
                            [message](auto prefix) { return message.starts_with(prefix); }))
                progress(message.substr(0, message.find('\n')));
        };
        linux.setBootContext("u:r:init:s0");
        for (const auto path : {"/apex", "/bootstrap-apex", "/linkerconfig", "/dev/socket",
                                "/mnt/vendor", "/mnt/product", "/storage", "/data_mirror"})
            linux.addDirectory(path, 0755);
        linux.addBootTmpfs("/mnt", 0755, 1000, 14);
        linux.addBootTmpfs("/debug_ramdisk", 0755, 0, 14);
        linux.addBootTmpfs("/second_stage_resources", 0755, 0, 6);
        if (files.contains("/system/etc/cgroups.json")) linux.addBootCgroups();
        if (files.contains("/system/bin/aconfigd-system")) {
            // Second-stage init expects first-stage init to have mounted the metadata partition.
            linux.addBootTmpfs("/metadata", 0755, 0, 6);
        }
        // NESTVANE_SHARED_STORAGE=<host directory> is the guest's shared storage, file for file.
        if (const char* shared = std::getenv("NESTVANE_SHARED_STORAGE"); shared && *shared) linux.shareStorage(shared);
        if (!databasePath.empty()) linux.mountData(databasePath);
        for (const auto& [path, file] : files) linux.mount(path, file.bytes());
        if (const auto bootconfig = files.find("/nestvane-metadata/bootconfig"); bootconfig != files.end()) {
            static std::string configuration;
            const auto bytes = bootconfig->second.bytes();
            configuration.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const std::string_view zone = std::getenv("NESTVANE_TIMEZONE") ? std::getenv("NESTVANE_TIMEZONE") : "";
            if (!zone.empty() && zone.size() <= 64 && std::ranges::all_of(zone, [](unsigned char c) {
                    return std::isalnum(c) || c == '/' || c == '_' || c == '-' || c == '+'; })) {
                if (!configuration.empty() && configuration.back() != '\n') configuration += '\n';
                configuration += "androidboot.nestvane.timezone = \"" + std::string(zone) + "\"\n";
            }
            // NESTVANE_DISPLAY_HZ is the rate the guest's display composer ticks at.
            if (const int hertz = std::atoi(std::getenv("NESTVANE_DISPLAY_HZ") ? std::getenv("NESTVANE_DISPLAY_HZ") : ""); hertz >= 10 && hertz <= 240) {
                if (!configuration.empty() && configuration.back() != '\n') configuration += '\n';
                configuration += "androidboot.qemu.vsync = \"" + std::to_string(hertz) + "\"\n";
            }
            // NESTVANE_DISPLAY_DENSITY is how many of the display's pixels make an inch for Android.
            if (const int density = std::atoi(std::getenv("NESTVANE_DISPLAY_DENSITY") ? std::getenv("NESTVANE_DISPLAY_DENSITY") : ""); density >= 120 && density <= 640) {
                if (!configuration.empty() && configuration.back() != '\n') configuration += '\n';
                configuration += "androidboot.nestvane.density = \"" + std::to_string(density) + "\"\n";
            }
            linux.mount("/proc/bootconfig", {reinterpret_cast<const uint8_t*>(configuration.data()), configuration.size()});
        }
        if (auto profile = files.find("/nestvane-metadata/selinux-file-labels.txt"); profile != files.end()) {
            const auto bytes = profile->second.bytes();
            if (!linux.loadBootFileLabels({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid guest file-label manifest");
        }
        if (auto profile = files.find("/nestvane-metadata/selinux-process-transition.txt"); profile != files.end()) {
            const auto bytes = profile->second.bytes();
            if (!linux.loadBootTransition({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid reference SELinux transition");
        }
        if (auto profile = files.find("/nestvane-metadata/selinux-current-transition.txt"); profile != files.end()) {
            const auto bytes = profile->second.bytes();
            if (!linux.loadCurrentContextPolicy({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid reference SELinux current-context transitions");
        }
        if (auto profile = files.find("/nestvane-metadata/selinux-property-access.txt"); profile != files.end()) {
            const auto bytes = profile->second.bytes();
            if (!linux.loadPropertyPolicy({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid reference SELinux property decisions");
        }
        for (const auto path : {"/system/etc/selinux/plat_seapp_contexts", "/system_ext/etc/selinux/system_ext_seapp_contexts",
                                "/product/etc/selinux/product_seapp_contexts", "/vendor/etc/selinux/vendor_seapp_contexts",
                                "/odm/etc/selinux/odm_seapp_contexts"}) {
            if (const auto policy = files.find(path); policy != files.end()) {
                const auto bytes = policy->second.bytes();
                if (!linux.loadSeappContexts({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                    throw std::runtime_error("invalid seapp file label types");
            }
        }
        if (const auto policy = files.find("/nestvane-metadata/selinux-app-transition.txt"); policy != files.end()) {
            const auto bytes = policy->second.bytes();
            if (!linux.loadAppTransitionDomains({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid app process transition policy");
        }
        for (const auto hardware : {false, true}) {
            const auto path = hardware ? "/nestvane-metadata/selinux-hwservice-access.txt" :
                                         "/nestvane-metadata/selinux-service-access.txt";
            const auto profile = files.find(path);
            if (profile == files.end()) continue;
            const auto bytes = profile->second.bytes();
            if (!linux.loadServicePolicy({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, hardware))
                throw std::runtime_error("invalid reference SELinux service decisions");
        }
        if (auto profile = files.find("/nestvane-metadata/selinux-fscreate-access.txt"); profile != files.end()) {
            const auto bytes = profile->second.bytes();
            if (!linux.loadFsCreatePolicy({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid reference SELinux file creation decisions");
        }
        for (const auto key : {false, true}) {
            const auto path = key ? "/nestvane-metadata/selinux-keystore-key-access.txt" :
                                    "/nestvane-metadata/selinux-keystore-access.txt";
            const auto profile = files.find(path);
            if (profile == files.end()) continue;
            const auto bytes = profile->second.bytes();
            if (!linux.loadKeystorePolicy({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, key))
                throw std::runtime_error("invalid reference SELinux Keystore decisions");
        }
        if (const auto profile = files.find("/nestvane-metadata/selinux-policyvers.txt"); profile != files.end()) {
            const auto bytes = profile->second.bytes();
            if (!linux.loadSelinuxPolicyVersion({reinterpret_cast<const char*>(bytes.data()), bytes.size()}))
                throw std::runtime_error("invalid reference SELinux policy version");
        }
        if (const auto inputs = files.find("/nestvane-metadata/runtime-links.txt"); inputs != files.end()) {
            const auto bytes = inputs->second.bytes();
            std::istringstream manifest(std::string(bytes.begin(), bytes.end()));
            std::string path, target;
            while (manifest >> path) {
                if (!(manifest >> target)) throw std::runtime_error("incomplete runtime alias");
                const bool runtimeLink = (path.starts_with("/system/bin/") || path.starts_with("/system/lib64/")) &&
                    target.starts_with("/apex/com.android.runtime/");
                const bool firewallLink = (path == "/system/bin/iptables-restore" || path == "/system/bin/ip6tables-restore") &&
                    target == "/system/bin/iptables" && files.contains(target);
                if ((!runtimeLink && !firewallLink) || fs::path(path).lexically_normal() != fs::path(path) ||
                    fs::path(target).lexically_normal() != fs::path(target))
                    throw std::runtime_error("invalid runtime linker alias");
                linux.alias(path, target);
            }
        }
        const bool expectShellCheck = files.contains("/system/bin/service") &&
                                      files.contains("/nestvane-metadata/selinux-service-access.txt");
        if (expectShellCheck) {
            static constexpr std::string_view client = R"rc(
on late-fs
    start nestvane-check-vold-shell

service nestvane-check-vold-shell /system/bin/service check vold
    user shell
    group shell
    seclabel u:r:shell:s0
    disabled
    oneshot
    stdio_to_kmsg
)rc";
            linux.mount("/system/etc/init/nestvane-check-vold.rc",
                        {reinterpret_cast<const uint8_t*>(client.data()), client.size()});
            linux.alias("/dev/kmsg_debug", "/dev/kmsg");
        }
        if (wakeGuestDisplay) {
            // Long cold boots can let the virtual display time out before Launcher draws.
            static constexpr std::string_view wakeDisplay = R"rc(
on property:sys.boot_completed=1 && property:init.svc.bootanim=stopped
    start nestvane-wake-display

service nestvane-wake-display /system/bin/cmd power wakeup
    user shell
    group shell
    seclabel u:r:shell:s0
    disabled
    oneshot
    stdio_to_kmsg
)rc";
            linux.mount("/system/etc/init/nestvane-wake-display.rc",
                        {reinterpret_cast<const uint8_t*>(wakeDisplay.data()), wakeDisplay.size()});
            linux.alias("/dev/kmsg_debug", "/dev/kmsg");
        }
        if (expectSmoke) {
            static constexpr std::string_view smoke = R"rc(
on property:sys.boot_completed=1
    start nestvane-smoke

service nestvane-smoke /system/bin/sh /system/etc/nestvane/apk-ready.sh /system/etc/nestvane/smoke.apk
    user shell
    group shell
    seclabel u:r:shell:s0
    disabled
    oneshot
    stdio_to_kmsg
)rc";
            linux.mount("/system/etc/init/nestvane-smoke.rc",
                        {reinterpret_cast<const uint8_t*>(smoke.data()), smoke.size()});
            linux.alias("/dev/kmsg_debug", "/dev/kmsg");
        }
        linux.alias("/etc", "/system/etc");
        linux.alias("/system/system_ext/etc/compatconfig", "/system_ext/etc/compatconfig");
        linux.alias("/proc/self/exe", filename);
        linux.alias("/system/bin/ueventd", "/system/bin/init");
        AndroidBootAot bootAot;
        const bool enableAot = initLimit("NESTVANE_INIT_AOT", 1, 1, 0);
        if (enableAot) {
            if (const auto crypto = files.find("/system/lib64/libcrypto.so"); crypto != files.end())
                bootAot.bind(crypto->second.bytes());
        }
        auto result = runProbe(image, linux, ProbeKind::init, "true\n", stop, liveSnapshot, enableAot ? &bootAot : nullptr);
        if (const auto reference = files.find("/nestvane-metadata/reference-classpath.txt"); reference != files.end()) {
            const auto expected = reference->second.bytes();
            const auto actual = linux.bootGuestFile("/data/system/environ/classpath");
            const bool match = std::ranges::equal(actual, expected);
            result.report += match ? "  generated Java classpaths match SDK reference: yes\n"
                                   : "  generated Java classpaths match SDK reference: no\n";
        }
        if (!databasePath.empty()) {
            const bool committed = linux.syncData();
            result.report.insert(0, storage.report + "\n");
            const auto data = linux.bootGuestFile("/data/.nestvane-storage-probe");
            constexpr std::string_view expected = "nestvane-guest-data-v1!\n";
            const bool visible = data.size() == expected.size() &&
                                 std::memcmp(data.data(), expected.data(), expected.size()) == 0;
            result.report += visible ? "  data visible in original Android boot: yes\n" :
                                       "  data visible in original Android boot: no\n";
            result.report += committed ? "  Android guest data committed: yes\n" :
                                         "  Android guest data committed: no\n";
            result.passed &= committed && visible;
        }
        bool shellPolicyVerified = !expectShellCheck;
        if (expectShellCheck) {
            shellPolicyVerified = linux.kernelOutput.find("name=vold scontext=u:r:shell:s0") != std::string::npos &&
                linux.kernelOutput.find("Service vold: not found\n") != std::string::npos;
            result.report += shellPolicyVerified ? "  stock shell lookup obeyed reference vold access denial: yes\n"
                                    : "  stock shell lookup obeyed reference vold access denial: no\n";
        }
        const bool expectVdc = files.contains("/system/bin/vdc");
        bool checkpointVerified = !expectVdc;
        if (expectVdc) {
            const auto exited = [&](std::string_view command) {
                std::istringstream lines(linux.kernelOutput);
                for (std::string line; std::getline(lines, line);) {
                    if (line.starts_with("<6>init: Service 'exec ") &&
                        line.find(command) != std::string::npos &&
                        line.find("exited with status 0 ") != std::string::npos) return true;
                }
                return false;
            };
            checkpointVerified = exited("(/system/bin/vdc checkpoint markBootAttempt)") &&
                                   exited("(/system/bin/vdc checkpoint prepareCheckpoint)");
            result.report += checkpointVerified ? "  stock vdc checkpoint commands verified: yes\n"
                                       : "  stock vdc checkpoint commands verified: no\n";
        }
        if (expectSmoke) {
            const auto succeeded = [&](std::string_view prefix) {
                const auto found = [&](const std::string& log) {
                    std::istringstream lines(log);
                    for (std::string line; std::getline(lines, line);) {
                        if (line.starts_with(prefix) && line.find("exited with status 0 ") != std::string::npos)
                            return true;
                    }
                    return false;
                };
                return found(linux.kernelOutput) || found(linux.recentKernelOutput);
            };
            result.report += succeeded("<6>init: Service 'nestvane-smoke' ") ?
                "  guest smoke APK install and launch commands: passed\n" :
                "  guest smoke APK install and launch commands: unverified\n";
            result.report += smokeActivityOpened ?
                "  guest smoke APK activity creation: verified\n" : "  guest smoke APK activity creation: unverified\n";
        }
        constexpr auto propertyReader = "/system/bin/nestvane-get-properties";
        if (!result.passed) return result;
        if (!files.contains(propertyReader)) {
            result.report += "  property reader fixture missing\n";
            result.passed = false;
            return result;
        }
        ProbeLinux reader(2, 1000);
        for (const auto& [path, file] : files) reader.mount(path, file.bytes());
        reader.alias("/proc/self/exe", propertyReader);
        const auto count = linux.exportFiles(reader, "/dev/__properties__/");
        result.report += "  init property files visible to reader: " + std::to_string(count) + "\n\n";
        const bool expectRuntime = files.contains("/system/apex/com.android.runtime.apex");
        const bool mountedRuntime = expectRuntime && linux.exportExt4Mount(reader, "/apex/com.android.runtime");
        if (expectRuntime) result.report += mountedRuntime ? "  Runtime APEX mount visible to reader: yes\n"
                                                           : "  Runtime APEX mount visible to reader: no\n";
        const bool expectManager = mountedRuntime && files.contains("/nestvane-metadata/selinux-service-access.txt");
        std::vector<std::string> readerArgs;
        if (mountedRuntime) readerArgs.emplace_back("runtime");
        if (expectManager) readerArgs.emplace_back("servicemanager");
        const auto readImage = loadDynamicELF(files.at(propertyReader).bytes(), files.at(interpreter).bytes(),
            propertyReader, interpreter, readerArgs);
        const auto kind = expectManager ? ProbeKind::serviceReady :
            mountedRuntime ? ProbeKind::runtimeApex : ProbeKind::properties;
        const auto read = runProbe(readImage, reader, kind);
        result.report += read.report;
        result.report += read.passed ? "  property reader verified: yes\n" : "  property reader verified: no\n";
        if (expectManager) result.report += read.passed ? "  servicemanager.ready=true verified: yes\n"
                                                       : "  servicemanager.ready=true verified: no\n";
        const auto verifyGate = [&](const char* name, std::string_view expected) {
            const auto gateImage = loadDynamicELF(files.at(propertyReader).bytes(), files.at(interpreter).bytes(),
                propertyReader, interpreter, {"runtime", "servicemanager", name});
            const auto gate = runProbe(gateImage, reader, ProbeKind::bootGate, expected);
            result.report += gate.report;
            result.report += "  " + std::string(name) + "=" + std::string(expected.substr(0, expected.size() - 1)) +
                (gate.passed ? " verified: yes\n" : " verified: no\n");
        };
        if (files.contains("/nestvane-metadata/selinux-keystore-access.txt"))
            verifyGate("keystore.module_hash.sent", "true\n");
        if (files.contains("/system/bin/odsign")) {
            verifyGate("odsign.key.done", "1\n");
            verifyGate("odsign.verification.done", "1\n");
            verifyGate("odsign.verification.success", "1\n");
        }
        if (files.contains("/system_ext/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk"))
            verifyGate("sys.boot_completed", "1\n");
        const bool activated = linux.kernelOutput.find("apexd-bootstrap: OnBootstrap done,") != std::string::npos;
        const bool runtimeVerified = activated && mountedRuntime && read.passed;
        if (expectRuntime) result.report += runtimeVerified ? "  Runtime APEX activation verified: yes\n"
                                                            : "  Runtime APEX activation verified: no\n";
        const bool expectConfig = files.contains("/nestvane-metadata/runtime-links.txt");
        bool configVerified = false;
        if (expectConfig) {
            const auto bytes = linux.bootGuestFile("/linkerconfig/ld.config.txt");
            const auto runtimeBytes = linux.bootGuestFile("/linkerconfig/com.android.runtime/ld.config.txt");
            const auto readConfig = [](std::span<const uint8_t> data) {
                return data.empty() ? std::string_view{} :
                    std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
            };
            const auto config = readConfig(bytes);
            const auto runtimeConfig = readConfig(runtimeBytes);
            configVerified = linux.kernelOutput.find("linkerconfig generated /linkerconfig with mounted APEX modules info")
                                != std::string::npos &&
                config.find("namespace.default.search.paths") != std::string_view::npos &&
                runtimeConfig.find("[com.android.runtime]") != std::string_view::npos &&
                runtimeConfig.find("namespace.com_android_runtime.search.paths = /apex/com.android.runtime/")
                    != std::string_view::npos;
            result.report += "  generated linker config: " + std::to_string(bytes.size()) +
                " bytes, Runtime APEX config: " + std::to_string(runtimeBytes.size()) + " bytes\n";
            result.report += configVerified ? "  linkerconfig generation verified: yes\n"
                                            : "  linkerconfig generation verified: no\n";
        }
        result.passed = read.passed && (!expectRuntime || runtimeVerified) && (!expectConfig || configVerified) &&
                        shellPolicyVerified && checkpointVerified;
        return result;
    } catch (const std::exception& error) {
        return {false, "Android 17 second-stage init probe\n  load error: " + std::string(error.what()) + "\n"};
    }
}

void visitMappedImageFiles(const std::function<void(const MappedImageFile&)>& visit) {
    const std::lock_guard guard(mappedImageLock);
    for (const auto& [data, file] : mappedImages) visit(file);
}

} // namespace nestvane
