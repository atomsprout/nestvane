#include "NativeCode.hpp"

#include "GuestSpace.hpp"
#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
extern const nestvane::native::Module nestvane_native_modules[];
extern const int nestvane_native_module_count;

// What rewritten code and NativeEntry.S see through x25. The offsets up to `spare` are fixed by
// experiments/g0/e3/rewrite.py.
struct NativeContext {
    uint64_t x[31];
    uint64_t sp;  // host address
    __uint128_t q[32];
    uint64_t nzcv, fpcr, tpidr;
    uint64_t pc;            // host address: where to enter, and after an svc where it returns
    uint64_t base;          // the offset from guest to host addresses of the chunks that hold the stack
    uint64_t table, hostSp;
    uint64_t memoryTable;   // the address space's per-chunk offsets from guest to host addresses
    uint64_t unused[2];
    uint64_t spare;         // scratch for rewritten code that sets sp
    uint64_t fpsr;
    uint64_t svcEntry;      // nestvane_native_svc: rewritten code in other images jumps to it through here
    uint64_t trapEntry;     // nestvane_native_trap, likewise
    uint64_t signalReturn;  // the guest pc of the signal return trampoline
    // Filled by the signal handler.
    uint64_t faultPc, faultAddress;
};
int nestvane_native_enter(NativeContext*);
void nestvane_native_exit();
void nestvane_native_svc();
void nestvane_native_trap();
void nestvane_native_signal_return();
}

static_assert(offsetof(NativeContext, sp) == 248 && offsetof(NativeContext, q) == 256 && offsetof(NativeContext, nzcv) == 768);
static_assert(offsetof(NativeContext, tpidr) == 784 && offsetof(NativeContext, pc) == 792 && offsetof(NativeContext, base) == 800);
static_assert(offsetof(NativeContext, table) == 808 && offsetof(NativeContext, hostSp) == 816);
static_assert(offsetof(NativeContext, memoryTable) == 824 && offsetof(NativeContext, spare) == 848 && offsetof(NativeContext, fpsr) == 856);
static_assert(offsetof(NativeContext, svcEntry) == 864 && offsetof(NativeContext, trapEntry) == 872);  // tools/build-native-code.py
static_assert(offsetof(NativeContext, signalReturn) == 880);  // NativeEntry.S

namespace nestvane::native {
namespace {

constexpr int leftBySyscall = 1, leftByFault = 2, leftByPreemption = 3, leftByTrap = 4;
constexpr uint64_t sliceNanoseconds = 1'000'000;
constexpr int preemptionSignal = SIGUSR2;

// The parts of a module's info block (experiments/g0/e3/rewrite.py --final-table).
struct Loaded {
    const Module* module;
    const uint64_t* table;   // host code to jump to per guest instruction: its translation, or its entry stub
    const uint32_t* stubStarts;  // for instructions entered through a stub: index, and where the translation starts from codeStart
    uint64_t count;
    uint64_t codeStart;
    uint64_t trap;           // host code that leaves native execution at the module's entry point
    uint64_t stubsBegin;     // host address where the code ends and the entry stubs begin
    uint64_t stubCount;
    bool active = false;
};

struct Registry {
    std::vector<Loaded> modules;                      // ordered by host code address
    std::unordered_map<uint64_t, std::vector<size_t>> byKey;   // the translations of one file content, by address
    std::unordered_map<const Module*, size_t> byModule;
    std::unordered_set<uint64_t> traps;                        // host code of the entry traps in use
    uint64_t low = 0, high = 0;                       // guest addresses of all translated code
    uint64_t covered = 0;                             // where the table's address space ends, as a guest address
    uint64_t* table = nullptr;                        // host code for guest pc at table[(pc - low) / 4]
    uint64_t codeStart = 0, codeEnd = 0;              // host addresses of all translated code
    bool wanted = false;
    const char* failure = nullptr;
    uint64_t signalPage = 0;                                   // guest address of the signal return trampoline's page
};

Registry& registry() {
    static Registry instance = [] {
        Registry result;
#ifdef NESTVANE_NATIVE_DEFAULT
        result.wanted = true;
#else
        result.wanted = std::getenv("NESTVANE_NATIVE") != nullptr;
#endif
        if (!result.wanted || nestvane_native_module_count == 0) return result;
        uint64_t low = UINT64_MAX, high = 0;
        for (int index = 0; index < nestvane_native_module_count; ++index) {
            const auto& module = nestvane_native_modules[index];
            result.modules.push_back({&module, reinterpret_cast<const uint64_t*>(module.info[3]),
                                      reinterpret_cast<const uint32_t*>(module.info[4]),
                                      (module.info[1] - module.info[0]) / 4, module.info[5], module.info[6],
                                      module.info[7], module.info[8]});
            low = std::min(low, module.info[0]);
            high = std::max(high, module.info[1]);
        }
        std::sort(result.modules.begin(), result.modules.end(),
                  [](const Loaded& a, const Loaded& b) { return a.codeStart < b.codeStart; });
        for (size_t index = 0; index < result.modules.size(); ++index) {
            result.byKey[result.modules[index].module->key].push_back(index);
            result.byModule[result.modules[index].module] = index;
        }
        for (auto& [key, copies] : result.byKey)
            std::sort(copies.begin(), copies.end(), [&](size_t a, size_t b) {
                return result.modules[a].module->base < result.modules[b].module->base;
            });
        // The entry for guest pc is 2 * pc from a page boundary, in this table as in each module's own.
        low &= ~uint64_t{0xffff};
        high = (high + 0xffff) & ~uint64_t{0xffff};
        // One more page of guest addresses, for the signal return trampoline (see signalReturnPage). It
        // is the last page below the addresses of code without a translation: `high` is where the last
        // translation's code ends, and that file's data follows it.
        result.signalPage = foreignLow - 0x10000;
        if (high > result.signalPage) {
            result.modules.clear();
            result.failure = "the translations leave no room for the signal return page";
            return result;
        }
        high = foreignLow;
        result.low = low;
        result.high = high;
        // Only address space: a module's table is mapped in when a process first maps the module, and
        // a look-up anywhere else faults, which sends the guest's jump to the interpreter.
        // The table also spans the addresses of code without a translation, if there is room for it.
        result.covered = std::max(high, foreignHigh);
        result.table = static_cast<uint64_t*>(mmap(nullptr, (result.covered - low) * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
        if (result.table == MAP_FAILED) {
            result.covered = high;
            result.table = static_cast<uint64_t*>(mmap(nullptr, (high - low) * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
        }
        if (result.table == MAP_FAILED) {
            result.modules.clear();
            result.failure = "no address space for the table of translated code";
            return result;
        }
        // The trampoline's entries: whichever of its instructions a handler returns to, native code
        // leaves there and the interpreter runs the trampoline.
        uint64_t* entries = result.table + (result.signalPage - low) / 4;
        if (mmap(entries, GuestSpace::page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED) {
            entries[1] = reinterpret_cast<uint64_t>(&nestvane_native_signal_return);
            result.traps.insert(entries[1]);
        } else {
            result.signalPage = 0;
        }
        result.codeStart = result.modules.front().codeStart;
        result.codeEnd = result.modules.back().stubsBegin;
        return result;
    }();
    return instance;
}

Loaded* loadedFor(uint64_t hostPc) {
    auto& all = registry().modules;
    auto next = std::upper_bound(all.begin(), all.end(), hostPc, [](uint64_t pc, const Loaded& entry) { return pc < entry.codeStart; });
    return next == all.begin() ? nullptr : &*std::prev(next);
}

// The guest instruction whose translation starts at or before a host pc, and whether it starts
// exactly there. False outside translated code.
bool guestCode(uint64_t hostPc, uint64_t& guestPc, bool& atStart) {
    const auto* loaded = loadedFor(hostPc);
    // Stubs and out-of-line paths follow a module's last instruction. Nothing in them can fault, and
    // the translations are in several images, so what lies beyond is not known to be code at all.
    if (hostPc >= loaded->stubsBegin) return false;
    const uint64_t offset = hostPc - loaded->codeStart;
    // The table has the host address per instruction, in order, except where it has an entry stub.
    const auto start = [&](uint64_t index) -> uint64_t {
        const uint64_t entry = loaded->table[index];
        if (entry < loaded->stubsBegin) return entry - loaded->codeStart;
        uint64_t first = 0, last = loaded->stubCount;
        while (last - first > 1) {
            const uint64_t middle = (first + last) / 2;
            if (loaded->stubStarts[middle * 2] <= index) first = middle; else last = middle;
        }
        return loaded->stubStarts[first * 2 + 1];
    };
    uint64_t low = 0, high = loaded->count;
    while (high - low > 1) {
        const uint64_t middle = (low + high) / 2;
        if (start(middle) <= offset) low = middle; else high = middle;
    }
    guestPc = loaded->module->info[0] + low * 4;
    atStart = start(low) == offset;
    return true;
}

// A host thread that runs guest code: the main loop's, or one of the workers beside it.
struct Running {
    NativeContext context;
    pthread_t thread;
    std::atomic<bool> active{false};
    std::atomic<uint64_t> since{0};  // mach time when the current run began, 0 when none
    std::atomic<bool> recall{false}; // the main loop wants the guest thread back
    uint64_t slice = 0;              // mach time after which a run is ended
    uint64_t preemptedPc = 0;
    int signal = 0;
};
Running mainRunning;
thread_local Running* self = nullptr;
// What the watchdog looks at. Entries are added and never removed.
std::array<std::atomic<Running*>, 16> watched{};

struct Counters {
    uint64_t runs = 0, syscalls = 0, faults = 0, preemptions = 0, traps = 0, lost = 0, ticks = 0;
    uint64_t faultsBySignal[32] = {};
    uint64_t nativeTime = 0, activationTime = 0, tableBytes = 0;  // mach time units
    uint64_t unmapped = 0, backed = 0;
    uint64_t sliceSignals = 0, sliceOutside = 0;
    uint64_t offloads = 0;
} counters;

void onSignal(int number, siginfo_t* info, void* raw) {
    auto* user = static_cast<ucontext_t*>(raw);
    auto& state = user->uc_mcontext->__ss;
    Running* const mine = self;
    const bool ours = mine && mine->active.load(std::memory_order_relaxed);
    const uint64_t pc = state.__pc;
    const auto& all = registry();
    const bool inCode = pc >= all.codeStart && pc < all.codeEnd;
    int reason = leftByFault;
    if (number == preemptionSignal) {
        // Only between two guest instructions is everything the guest can see in its registers.
        uint64_t guestPc;
        bool atStart;
        ++counters.sliceSignals;
        if (!ours || !inCode) { ++counters.sliceOutside; return; }
        if (!guestCode(pc, guestPc, atStart) || !atStart) return;
        mine->preemptedPc = guestPc;
        reason = leftByPreemption;
    } else if (!ours || (pc >= reinterpret_cast<uint64_t>(&nestvane_native_enter) && pc < reinterpret_cast<uint64_t>(&nestvane_native_exit) + 256)) {
        // Not the guest's doing: let it happen again without us, so that the crash is reported.
        signal(number, SIG_DFL);
        return;
    }
    auto& context = mine->context;
    std::memcpy(context.x, state.__x, sizeof(uint64_t) * 25);
    context.x[29] = state.__fp;
    context.x[30] = state.__lr;
    context.sp = state.__sp;
    context.nzcv = state.__cpsr & 0xf0000000u;
    std::memcpy(context.q, user->uc_mcontext->__ns.__v, sizeof(context.q));
    context.fpsr = user->uc_mcontext->__ns.__fpsr;
    context.faultPc = pc;
    context.faultAddress = reinterpret_cast<uint64_t>(info->si_addr);
    mine->signal = number;
    mine->active.store(false, std::memory_order_relaxed);
    state.__x[0] = uint64_t(reason);
    state.__x[25] = reinterpret_cast<uint64_t>(&context);
    __darwin_arm_thread_state64_set_pc_fptr(state, &nestvane_native_exit);
}

uint64_t machTime(uint64_t nanoseconds) {
    static const mach_timebase_info_data_t base = [] { mach_timebase_info_data_t value; mach_timebase_info(&value); return value; }();
    return nanoseconds * base.denom / base.numer;
}

// Ends the time slice of guest code that has run long enough without leaving by itself. The signal
// only takes effect between two guest instructions, so it is repeated until it does.
void watch() {
    for (;;) {
        bool overdue = false;
        const uint64_t now = mach_absolute_time();
        for (auto& entry : watched) {
            Running* const running = entry.load(std::memory_order_acquire);
            if (!running) break;
            const uint64_t since = running->since.load(std::memory_order_relaxed);
            if (!since || !running->active.load(std::memory_order_relaxed)) continue;
            if (now - since < running->slice && !running->recall.load(std::memory_order_relaxed)) continue;
            overdue = true;
            ++counters.ticks;
            pthread_kill(running->thread, preemptionSignal);
        }
        usleep(overdue ? 40 : 500);
    }
}

void prepareThread(Running& running, uint64_t sliceNanos) {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    running.thread = pthread_self();
    running.slice = machTime(sliceNanos);
    self = &running;
    for (auto& entry : watched) {
        Running* none = nullptr;
        if (entry.compare_exchange_strong(none, &running)) break;
    }
    // Guest code runs on the guest's stack, which a handler must not write to.
    stack_t stack{};
    stack.ss_size = 256 * 1024;
    stack.ss_sp = mmap(nullptr, stack.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    sigaltstack(&stack, nullptr);
    // Threads of a dispatch queue start with asynchronous signals blocked, and the guest may run on one.
    sigset_t wanted;
    sigemptyset(&wanted);
    sigaddset(&wanted, preemptionSignal);
    pthread_sigmask(SIG_UNBLOCK, &wanted, nullptr);
    static bool installed = false;
    if (installed) return;
    installed = true;
    struct sigaction action{};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&action.sa_mask);
    for (const int number : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, preemptionSignal}) sigaction(number, &action, nullptr);
    std::thread(watch).detach();
}

} // namespace

bool enabled() { return !registry().modules.empty(); }

const char* failure() { return registry().failure; }

bool covers(uint64_t start, uint64_t end) {
    const auto& all = registry();
    return !all.modules.empty() && start >= all.low && end <= all.covered;
}

uint64_t signalReturnPage() { return registry().modules.empty() ? 0 : registry().signalPage; }

uint64_t contentKey(std::span<const uint8_t> file) {
    uint64_t value = 0xcbf29ce484222325ULL;
    for (const uint8_t byte : file.first(std::min<size_t>(file.size(), 16384))) value = (value ^ byte) * 0x100000001b3ULL;
    return (value ^ file.size()) * 0x100000001b3ULL;
}

const Module* moduleFor(std::span<const uint8_t> file, unsigned copy) {
    auto& all = registry();
    if (all.modules.empty() || file.size() < 64 || std::memcmp(file.data(), "\x7f" "ELF", 4) != 0) return nullptr;
    // The dynamic linker looks at each library many times, and hashing 16 KiB each time adds up. The
    // loader and the mounted images keep a file's bytes in place, so the key is remembered by where
    // they are.
    static std::unordered_map<const uint8_t*, std::pair<size_t, uint64_t>> keys;
    auto& known = keys[file.data()];
    if (known.first != file.size()) known = {file.size(), contentKey(file)};
    const auto found = all.byKey.find(known.second);
    if (found == all.byKey.end() || copy >= found->second.size()) return nullptr;
    const auto* module = all.modules[found->second[copy]].module;
    return module->size == file.size() ? module : nullptr;
}

const Module* mappedCode(std::span<const uint8_t> file, uint64_t address, uint64_t offset) {
    if (!moduleFor(file)) return nullptr;
    uint64_t headers;
    uint16_t entrySize, count;
    std::memcpy(&headers, file.data() + 32, 8);
    std::memcpy(&entrySize, file.data() + 54, 2);
    std::memcpy(&count, file.data() + 56, 2);
    if (headers > file.size() || uint64_t{entrySize} * count > file.size() - headers) return nullptr;
    for (unsigned index = 0; index < count; ++index) {
        const uint8_t* entry = file.data() + headers + index * entrySize;
        uint32_t type, flags;
        uint64_t fileOffset, virtualAddress;
        std::memcpy(&type, entry, 4);
        std::memcpy(&flags, entry + 4, 4);
        std::memcpy(&fileOffset, entry + 8, 8);
        std::memcpy(&virtualAddress, entry + 16, 8);
        if (type != 1 || !(flags & 1)) continue;
        constexpr uint64_t mask = ~(GuestSpace::page - 1);
        if ((fileOffset & mask) != offset) continue;
        for (unsigned copy = 0; const auto* module = moduleFor(file, copy); ++copy)
            if (module->base + (virtualAddress & mask) == address) return module;
    }
    return nullptr;
}

bool activate(const Module& module) {
    auto& all = registry();
    const auto found = all.byModule.find(&module);
    if (found == all.byModule.end()) return false;
    auto& loaded = all.modules[found->second];
    if (loaded.active) return true;
    const uint64_t began = mach_absolute_time();
    // The module's table is laid out to match: whole pages of it become pages of the table, shared
    // with the program file until something writes to them.
    constexpr uint64_t pageMask = GuestSpace::page - 1;
    const uint64_t front = reinterpret_cast<uint64_t>(loaded.table) & pageMask;
    const uint64_t length = (front + loaded.count * 8 + pageMask) & ~pageMask;
    vm_address_t target = reinterpret_cast<vm_address_t>(all.table + (module.info[0] - all.low) / 4) - front;
    vm_prot_t current, maximum;
    if ((target & pageMask) ||
        vm_remap(mach_task_self(), &target, length, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                 reinterpret_cast<vm_address_t>(loaded.table) - front, TRUE, &current, &maximum, VM_INHERIT_COPY) != KERN_SUCCESS) {
        ++counters.unmapped;
        return false;
    }
    loaded.active = true;
    counters.activationTime += mach_absolute_time() - began;
    counters.tableBytes += length;
    return true;
}

void trap(uint64_t pc) {
    auto& all = registry();
    for (auto& loaded : all.modules) {
        if (!loaded.active || loaded.module->info[2] != pc || !loaded.trap) continue;
        uint64_t* entry = all.table + (pc - all.low) / 4;
        if (*entry == loaded.trap) return;
        void* page = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(entry) & ~(GuestSpace::page - 1));
        mprotect(page, GuestSpace::page, PROT_READ | PROT_WRITE);
        *entry = loaded.trap;
        mprotect(page, GuestSpace::page, PROT_READ);
        all.traps.insert(loaded.trap);
        return;
    }
}

namespace {

// Puts the thread's registers into a context that enters native code at cpu.pc. False if the
// instruction there is not entered natively.
bool load(Machine& machine, NativeContext& context) {
    auto& all = registry();
    auto& cpu = machine.cpu;
    auto& space = *machine.space;
    if (cpu.pc < all.low || cpu.pc >= all.high || cpu.pc % 4) return false;
    const uint64_t entry = all.table[(cpu.pc - all.low) / 4];
    if (!entry || all.traps.contains(entry)) return false;
    std::memcpy(context.x, cpu.x.data(), sizeof(context.x));
    std::memcpy(context.q, cpu.v.data(), sizeof(context.q));
    const uint64_t stack = machine.dataAddress(cpu.sp);
    context.base = stack < GuestSpace::size ? space.offsets()[stack >> GuestSpace::chunkShift] : GuestSpace::voidOffset;
    context.sp = cpu.sp + context.base;
    context.nzcv = uint64_t{cpu.nzcv} << 28;
    context.fpsr = cpu.fpsr;
    context.tpidr = cpu.threadPointer;
    context.pc = entry;
    context.table = reinterpret_cast<uint64_t>(all.table) - all.low * 2;
    context.memoryTable = reinterpret_cast<uint64_t>(space.offsets());
    context.svcEntry = reinterpret_cast<uint64_t>(&nestvane_native_svc);
    context.trapEntry = reinterpret_cast<uint64_t>(&nestvane_native_trap);
    context.signalReturn = all.signalPage + 4;
    return true;
}

// Runs the context on this host thread until guest code leaves, and tells how it left.
Result enter(Running& running) {
    const uint64_t entered = mach_absolute_time();
    running.since.store(entered, std::memory_order_relaxed);
    running.active.store(true, std::memory_order_relaxed);
    const int reason = nestvane_native_enter(&running.context);
    running.active.store(false, std::memory_order_relaxed);
    running.since.store(0, std::memory_order_relaxed);
    Result result;
    result.reason = reason;
    result.signal = running.signal;
    result.pc = running.context.pc;
    result.faultPc = running.context.faultPc;
    result.faultAddress = running.context.faultAddress;
    result.preemptedPc = running.preemptedPc;
    result.ran = mach_absolute_time() - entered;
    result.brief = result.ran < machTime(200'000);
    return result;
}

// The registers the guest can see, back from a context.
void store(const NativeContext& context, CPU& cpu) {
    std::memcpy(cpu.x.data(), context.x, sizeof(uint64_t) * 25);
    cpu.x[29] = context.x[29];
    cpu.x[30] = context.x[30];
    std::memcpy(cpu.v.data(), context.q, sizeof(context.q));
    cpu.sp = context.sp - context.base;
    cpu.nzcv = uint8_t(context.nzcv >> 28);
    cpu.fpsr = uint32_t(context.fpsr);
    cpu.threadPointer = context.tpidr;
    cpu.exclusiveSize = 0;
}

uint64_t lastFault = 0;

} // namespace

Exit settle(Machine& machine, const Result& result) {
    auto& all = registry();
    auto& cpu = machine.cpu;
    auto& space = *machine.space;
    counters.nativeTime += result.ran;
    ++counters.runs;
    uint64_t guestPc = 0;
    bool atStart = false;
    if (result.reason == leftBySyscall) {
        // x28 is where the svc returns: the start of the next instruction, unless out-of-line code
        // was put in between, and then it is still within the svc's own translation.
        ++counters.syscalls;
        if (!guestCode(result.pc, guestPc, atStart)) {
            machine.fault(cpu.pc, result.pc);
            return Exit::lost;
        }
        cpu.pc = atStart ? guestPc - 4 : guestPc;
        return Exit::syscall;
    }
    if (result.reason == leftByTrap) {
        ++counters.traps;
        cpu.pc = result.pc;  // the entry trap puts the guest pc there
        return Exit::interpret;
    }
    if (result.reason == leftByPreemption) {
        ++counters.preemptions;
        cpu.pc = result.preemptedPc;
        return Exit::preempted;
    }
    if (result.faultPc >= all.codeStart && result.faultPc < all.codeEnd && guestCode(result.faultPc, guestPc, atStart)) {
        cpu.pc = guestPc;
        lastFault = result.faultAddress;
        // Mapped memory that nothing has written yet has no host pages. The interpreter reads it as
        // zeros without giving it any, so native code would fault there again and again.
        uint64_t address = 0;
        if ((result.signal == SIGSEGV || result.signal == SIGBUS) && space.guestAddress(result.faultAddress, address) &&
            address < GuestSpace::size && space.mapped(address) && !space.backed(address) && space.touch(address)) {
            ++counters.backed;
            return Exit::retry;
        }
        ++counters.faults;
        ++counters.faultsBySignal[result.signal & 31];
        return Exit::fault;
    }
    // A jump to something that is no translated code. (On iOS the system kills the process for it
    // before this is reached.)
    ++counters.lost;
    machine.fault(cpu.pc, result.faultPc);
    return Exit::lost;
}

Exit run(Machine& machine) {
    prepareThread(mainRunning, sliceNanoseconds);
    if (!load(machine, mainRunning.context)) return Exit::interpret;
    const Result result = enter(mainRunning);
    store(mainRunning.context, machine.cpu);
    return settle(machine, result);
}

// Host threads that run guest threads' native code beside the main loop. The main loop hands a
// thread over with its registers, goes on with other threads, and takes the registers back when the
// native code has left: for a system call, a fault, or because the main loop asked for it.
namespace {

struct Worker {
    Running running;
    enum class State { idle, assigned, finished };
    State state = State::idle;  // under Pool::mutex
    uint64_t ticket = 0;
    Result result;
    std::shared_ptr<GuestSpace> space;  // keeps the memory the guest code runs in
};

struct Pool {
    std::mutex mutex;
    std::condition_variable assigned;
    std::vector<std::unique_ptr<Worker>> workers;
    std::mutex hookMutex;
    std::function<void()> finished;  // under hookMutex
    std::condition_variable ended;   // with mutex: a run has ended
    std::atomic<unsigned> outstanding{0}, done{0};
} pool;

void work(Worker* worker) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    // A worker has a core to itself, so its turn is long: it ends for the main loop's sake.
    prepareThread(worker->running, 20 * sliceNanoseconds);
    for (;;) {
        {
            std::unique_lock lock(pool.mutex);
            pool.assigned.wait(lock, [&] { return worker->state == Worker::State::assigned; });
        }
        const Result result = enter(worker->running);
        {
            std::lock_guard lock(pool.mutex);
            worker->result = result;
            worker->state = Worker::State::finished;
            worker->running.recall.store(false, std::memory_order_relaxed);
        }
        pool.done.fetch_add(1, std::memory_order_release);
        pool.ended.notify_all();
        std::lock_guard lock(pool.hookMutex);
        if (pool.finished) pool.finished();
    }
}

Worker* workerFor(uint64_t ticket) {
    for (auto& worker : pool.workers)
        if (worker->state != Worker::State::idle && worker->ticket == ticket) return worker.get();
    return nullptr;
}

} // namespace

void setFinishedHook(std::function<void()> finished) {
    std::lock_guard lock(pool.hookMutex);
    pool.finished = std::move(finished);
}

void waitFinished(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(pool.mutex);
    pool.ended.wait_until(lock, deadline, [] { return pool.done.load(std::memory_order_acquire) != 0; });
}

void startWorkers(unsigned count) {
    if (!enabled() || !pool.workers.empty()) return;
    prepareThread(mainRunning, sliceNanoseconds);
    for (unsigned index = 0; index < std::min(count, 8u); ++index) {
        pool.workers.push_back(std::make_unique<Worker>());
        std::thread(work, pool.workers.back().get()).detach();
    }
}

unsigned workers() { return unsigned(pool.workers.size()); }
unsigned outstanding() { return pool.outstanding.load(std::memory_order_relaxed); }

bool offload(Machine& machine, uint64_t ticket) {
    if (pool.outstanding.load(std::memory_order_relaxed) == pool.workers.size()) return false;
    std::lock_guard lock(pool.mutex);
    for (auto& worker : pool.workers) {
        if (worker->state != Worker::State::idle) continue;
        if (!load(machine, worker->running.context)) return false;
        worker->ticket = ticket;
        worker->space = machine.space;
        worker->state = Worker::State::assigned;
        pool.outstanding.fetch_add(1, std::memory_order_relaxed);
        ++counters.offloads;
        pool.assigned.notify_all();
        return true;
    }
    return false;
}

bool finished(uint64_t& ticket) {
    if (!pool.done.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(pool.mutex);
    for (auto& worker : pool.workers)
        if (worker->state == Worker::State::finished) { ticket = worker->ticket; return true; }
    return false;
}

bool take(uint64_t ticket, CPU* cpu, Result& result) {
    std::lock_guard lock(pool.mutex);
    Worker* const worker = workerFor(ticket);
    if (!worker || worker->state != Worker::State::finished) return false;
    if (cpu) store(worker->running.context, *cpu);
    result = worker->result;
    worker->space.reset();
    worker->state = Worker::State::idle;
    pool.done.fetch_sub(1, std::memory_order_release);
    pool.outstanding.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void recall(uint64_t ticket, bool wait) {
    for (;;) {
        {
            std::lock_guard lock(pool.mutex);
            Worker* const worker = workerFor(ticket);
            if (!worker || worker->state == Worker::State::finished) return;
            worker->running.recall.store(true, std::memory_order_relaxed);
        }
        if (!wait) return;  // the watchdog ends the run
        usleep(50);
    }
}

uint64_t lastFaultAddress() { return lastFault; }

std::string statistics() {
    std::ostringstream out;
    size_t active = 0;
    for (const auto& loaded : registry().modules) active += loaded.active;
    out << "  native execution: modules " << active << "/" << registry().modules.size() << ", runs " << counters.runs
        << ", syscalls " << counters.syscalls << ", memory given on first touch " << counters.backed
        << ", interpreted after a fault " << counters.faults
        << " (SIGSEGV " << counters.faultsBySignal[SIGSEGV] << ", SIGBUS " << counters.faultsBySignal[SIGBUS]
        << ", SIGILL " << counters.faultsBySignal[SIGILL] << ", SIGTRAP " << counters.faultsBySignal[SIGTRAP]
        << "), preemptions " << counters.preemptions << " of " << counters.ticks << " signals ("
        << counters.sliceSignals << " handled, " << counters.sliceOutside << " outside guest code), traps " << counters.traps
        << ", lost " << counters.lost << ", runs on " << pool.workers.size() << " workers " << counters.offloads << '\n';
    static const mach_timebase_info_data_t base = [] { mach_timebase_info_data_t value; mach_timebase_info(&value); return value; }();
    const auto milliseconds = [&](uint64_t time) { return time * base.numer / base.denom / 1'000'000; };
    out << "  native execution time: " << milliseconds(counters.nativeTime) << " ms in guest code, "
        << milliseconds(counters.activationTime) << " ms mapping " << counters.tableBytes / (1024 * 1024)
        << " MiB of code tables";
    if (counters.unmapped) out << ", " << counters.unmapped << " modules could not be mapped";
    out << '\n';
    // NESTVANE_NATIVE_USED=1 lists the translations that were mapped, for leaving the others out of a build.
    if (static const bool list = std::getenv("NESTVANE_NATIVE_USED"); list)
        for (const auto& loaded : registry().modules)
            if (loaded.active)
                out << "  native module used: " << std::hex << loaded.module->key << ' ' << loaded.module->base << std::dec
                    << ' ' << loaded.module->name << '\n';
    return out.str();
}

} // namespace nestvane::native
