#include "NativeCode.hpp"

#include "GuestSpace.hpp"
#include "Runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
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
    // Filled by the signal handler.
    uint64_t faultPc, faultAddress;
};
int nestvane_native_enter(NativeContext*);
void nestvane_native_exit();
}

static_assert(offsetof(NativeContext, sp) == 248 && offsetof(NativeContext, q) == 256 && offsetof(NativeContext, nzcv) == 768);
static_assert(offsetof(NativeContext, tpidr) == 784 && offsetof(NativeContext, pc) == 792 && offsetof(NativeContext, base) == 800);
static_assert(offsetof(NativeContext, table) == 808 && offsetof(NativeContext, hostSp) == 816);
static_assert(offsetof(NativeContext, memoryTable) == 824 && offsetof(NativeContext, spare) == 848 && offsetof(NativeContext, fpsr) == 856);

namespace nestvane::native {
namespace {

constexpr int leftBySyscall = 1, leftByFault = 2, leftByPreemption = 3;
// A table entry that is no host code: jumping to it faults, and the fault tells the guest pc.
constexpr uint64_t trapTag = uint64_t{1} << 62;
constexpr uint64_t sliceNanoseconds = 1'000'000;
constexpr int preemptionSignal = SIGUSR2;

struct Loaded {
    const Module* module;
    const uint64_t* table;  // host code per guest instruction, in address order
    uint64_t count;
    uint64_t codeStart;
    bool active = false;
};

struct Registry {
    std::vector<Loaded> modules;                      // ordered by host code address
    std::unordered_map<uint64_t, size_t> byKey;
    uint64_t low = 0, high = 0;                       // guest addresses of all translated code
    uint64_t* table = nullptr;                        // host code for guest pc at table[(pc - low) / 4]
    uint64_t codeStart = 0, codeEnd = 0;              // host addresses of all translated code
    bool wanted = false;
};

Registry& registry() {
    static Registry instance = [] {
        Registry result;
        result.wanted = std::getenv("NESTVANE_NATIVE") != nullptr;
        if (!result.wanted || nestvane_native_module_count == 0) return result;
        uint64_t low = UINT64_MAX, high = 0;
        for (int index = 0; index < nestvane_native_module_count; ++index) {
            const auto& module = nestvane_native_modules[index];
            const auto* table = reinterpret_cast<const uint64_t*>(module.info[3]);
            result.modules.push_back({&module, table, (module.info[1] - module.info[0]) / 4, table[0]});
            low = std::min(low, module.info[0]);
            high = std::max(high, module.info[1]);
        }
        std::sort(result.modules.begin(), result.modules.end(),
                  [](const Loaded& a, const Loaded& b) { return a.codeStart < b.codeStart; });
        for (size_t index = 0; index < result.modules.size(); ++index) result.byKey[result.modules[index].module->key] = index;
        result.low = low;
        result.high = high;
        // Untouched entries cost nothing: modules are copied in when a process first maps them.
        result.table = static_cast<uint64_t*>(mmap(nullptr, (high - low) * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        if (result.table == MAP_FAILED) {
            result.modules.clear();
            return result;
        }
        result.codeStart = result.modules.front().codeStart;
        // The last module's stubs and far paths follow its code. Nothing else of ours is that close.
        result.codeEnd = result.modules.back().table[result.modules.back().count - 1] + (uint64_t{64} << 20);
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
    if (!loaded || hostPc >= registry().codeEnd) return false;
    uint64_t low = 0, high = loaded->count;
    while (high - low > 1) {
        const uint64_t middle = (low + high) / 2;
        if (loaded->table[middle] <= hostPc) low = middle; else high = middle;
    }
    guestPc = loaded->module->info[0] + low * 4;
    atStart = loaded->table[low] == hostPc;
    return true;
}

struct Running {
    NativeContext context;
    pthread_t thread;
    std::atomic<bool> active{false};
    std::atomic<uint64_t> since{0};  // mach time when the current run began, 0 when none
    uint64_t preemptedPc = 0;
    int signal = 0;
};
Running running;

struct Counters {
    uint64_t runs = 0, syscalls = 0, faults = 0, preemptions = 0, traps = 0, lost = 0, ticks = 0;
    uint64_t faultsBySignal[32] = {};
} counters;

void onSignal(int number, siginfo_t* info, void* raw) {
    auto* user = static_cast<ucontext_t*>(raw);
    auto& state = user->uc_mcontext->__ss;
    const bool ours = running.active.load(std::memory_order_relaxed) && pthread_equal(pthread_self(), running.thread);
    const uint64_t pc = state.__pc;
    const auto& all = registry();
    const bool inCode = pc >= all.codeStart && pc < all.codeEnd;
    int reason = leftByFault;
    if (number == preemptionSignal) {
        // Only between two guest instructions is everything the guest can see in its registers.
        uint64_t guestPc;
        bool atStart;
        if (!ours || !inCode || !guestCode(pc, guestPc, atStart) || !atStart) return;
        running.preemptedPc = guestPc;
        reason = leftByPreemption;
    } else if (!ours || (pc >= reinterpret_cast<uint64_t>(&nestvane_native_enter) && pc < reinterpret_cast<uint64_t>(&nestvane_native_exit) + 256)) {
        // Not the guest's doing: let it happen again without us, so that the crash is reported.
        signal(number, SIG_DFL);
        return;
    }
    auto& context = running.context;
    std::memcpy(context.x, state.__x, sizeof(uint64_t) * 25);
    context.x[29] = state.__fp;
    context.x[30] = state.__lr;
    context.sp = state.__sp;
    context.nzcv = state.__cpsr & 0xf0000000u;
    std::memcpy(context.q, user->uc_mcontext->__ns.__v, sizeof(context.q));
    context.fpsr = user->uc_mcontext->__ns.__fpsr;
    context.faultPc = pc;
    context.faultAddress = reinterpret_cast<uint64_t>(info->si_addr);
    running.signal = number;
    running.active.store(false, std::memory_order_relaxed);
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
    const uint64_t slice = machTime(sliceNanoseconds);
    for (;;) {
        const uint64_t since = running.since.load(std::memory_order_relaxed);
        const bool overdue = since && mach_absolute_time() - since >= slice;
        if (overdue && running.active.load(std::memory_order_relaxed)) {
            ++counters.ticks;
            pthread_kill(running.thread, preemptionSignal);
        }
        usleep(overdue ? 40 : 500);
    }
}

void prepareThread() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    // Guest code runs on the guest's stack, which a handler must not write to.
    stack_t stack{};
    stack.ss_size = 256 * 1024;
    stack.ss_sp = mmap(nullptr, stack.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    sigaltstack(&stack, nullptr);
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

uint64_t contentKey(std::span<const uint8_t> file) {
    uint64_t value = 0xcbf29ce484222325ULL;
    for (const uint8_t byte : file.first(std::min<size_t>(file.size(), 16384))) value = (value ^ byte) * 0x100000001b3ULL;
    return (value ^ file.size()) * 0x100000001b3ULL;
}

const Module* moduleFor(std::span<const uint8_t> file) {
    auto& all = registry();
    if (all.modules.empty() || file.size() < 64 || std::memcmp(file.data(), "\x7f" "ELF", 4) != 0) return nullptr;
    const auto found = all.byKey.find(contentKey(file));
    if (found == all.byKey.end() || all.modules[found->second].module->size != file.size()) return nullptr;
    return all.modules[found->second].module;
}

const Module* mappedCode(std::span<const uint8_t> file, uint64_t address, uint64_t offset) {
    const auto* module = moduleFor(file);
    if (!module) return nullptr;
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
        if ((fileOffset & mask) == offset && module->base + (virtualAddress & mask) == address) return module;
    }
    return nullptr;
}

void activate(const Module& module) {
    auto& all = registry();
    const auto found = all.byKey.find(module.key);
    if (found == all.byKey.end()) return;
    auto& loaded = all.modules[found->second];
    if (loaded.active) return;
    loaded.active = true;
    uint64_t* entries = all.table + (module.info[0] - all.low) / 4;
    std::memcpy(entries, loaded.table, loaded.count * 8);
    // An instruction that relies on what the one before left in the scratch register is entered
    // through a stub when control arrives any other way than by falling into it.
    const auto* stubs = reinterpret_cast<const uint64_t*>(module.info[4]);
    for (uint64_t index = 0; index < module.info[5]; ++index) entries[stubs[index * 2]] = stubs[index * 2 + 1];
}

void trap(uint64_t pc) {
    auto& all = registry();
    if (pc >= all.low && pc < all.high) all.table[(pc - all.low) / 4] = trapTag | pc;
}

Exit run(Machine& machine) {
    auto& all = registry();
    auto& cpu = machine.cpu;
    auto& space = *machine.space;
    if (cpu.pc < all.low || cpu.pc >= all.high || cpu.pc % 4) return Exit::interpret;
    const uint64_t entry = all.table[(cpu.pc - all.low) / 4];
    if (!entry || (entry & trapTag)) return Exit::interpret;
    prepareThread();

    auto& context = running.context;
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

    running.thread = pthread_self();
    running.since.store(mach_absolute_time(), std::memory_order_relaxed);
    running.active.store(true, std::memory_order_relaxed);
    const int reason = nestvane_native_enter(&context);
    running.active.store(false, std::memory_order_relaxed);
    running.since.store(0, std::memory_order_relaxed);
    ++counters.runs;

    std::memcpy(cpu.x.data(), context.x, sizeof(uint64_t) * 25);
    cpu.x[29] = context.x[29];
    cpu.x[30] = context.x[30];
    std::memcpy(cpu.v.data(), context.q, sizeof(context.q));
    cpu.sp = context.sp - context.base;
    cpu.nzcv = uint8_t(context.nzcv >> 28);
    cpu.fpsr = uint32_t(context.fpsr);
    cpu.threadPointer = context.tpidr;
    cpu.exclusiveSize = 0;

    uint64_t guestPc = 0;
    bool atStart = false;
    if (reason == leftBySyscall) {
        // x28 is where the svc returns: the start of the next instruction, unless out-of-line code
        // was put in between, and then it is still within the svc's own translation.
        ++counters.syscalls;
        if (!guestCode(context.pc, guestPc, atStart)) {
            machine.fault(cpu.pc, context.pc);
            return Exit::lost;
        }
        cpu.pc = atStart ? guestPc - 4 : guestPc;
        return Exit::syscall;
    }
    if (reason == leftByPreemption) {
        ++counters.preemptions;
        cpu.pc = running.preemptedPc;
        return Exit::preempted;
    }
    if (context.faultPc >= all.codeStart && context.faultPc < all.codeEnd && guestCode(context.faultPc, guestPc, atStart)) {
        ++counters.faults;
        ++counters.faultsBySignal[running.signal & 31];
        cpu.pc = guestPc;
        return Exit::fault;
    }
    // A jump to something that is no host code: a trapped entry, or guest code without a translation.
    const uint64_t target = context.faultPc & ~trapTag & 0x00ff'ffff'ffff'ffffULL;
    if (running.signal != SIGILL && running.signal != SIGTRAP && target && target % 4 == 0 && target < GuestSpace::size) {
        ++counters.traps;
        cpu.pc = target;
        return Exit::interpret;
    }
    ++counters.lost;
    machine.fault(cpu.pc, context.faultPc);
    return Exit::lost;
}

std::string statistics() {
    std::ostringstream out;
    size_t active = 0;
    for (const auto& loaded : registry().modules) active += loaded.active;
    out << "  native execution: modules " << active << "/" << registry().modules.size() << ", runs " << counters.runs
        << ", syscalls " << counters.syscalls << ", interpreted after a fault " << counters.faults
        << " (SIGSEGV " << counters.faultsBySignal[SIGSEGV] << ", SIGBUS " << counters.faultsBySignal[SIGBUS]
        << ", SIGILL " << counters.faultsBySignal[SIGILL] << ", SIGTRAP " << counters.faultsBySignal[SIGTRAP]
        << "), preemptions " << counters.preemptions << " of " << counters.ticks << " signals, traps " << counters.traps
        << ", lost " << counters.lost << '\n';
    return out.str();
}

} // namespace nestvane::native
