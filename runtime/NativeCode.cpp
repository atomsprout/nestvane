#include "NativeCode.hpp"
#include "FastPath.hpp"

#include <csetjmp>

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
    // Code built without x18 and x25-x28 set aside runs each instruction that names one of them with a
    // register of x9-x17 lent in its place (rewrite.py, Rewriter.lent). While that instruction can
    // fault, `borrowed` holds up to four 10-bit entries, the lent register and above it the guest
    // register it stands for, and `lent` holds the lent registers' own values, in the same order.
    uint64_t borrowed = 0;
    uint64_t lent[4];
};
int nestvane_native_enter(NativeContext*);
void nestvane_native_exit();
void nestvane_native_svc();
void nestvane_native_trap();
void nestvane_native_signal_return();
void nestvane_native_stray();
}

static_assert(offsetof(NativeContext, sp) == 248 && offsetof(NativeContext, q) == 256 && offsetof(NativeContext, nzcv) == 768);
static_assert(offsetof(NativeContext, tpidr) == 784 && offsetof(NativeContext, pc) == 792 && offsetof(NativeContext, base) == 800);
static_assert(offsetof(NativeContext, table) == 808 && offsetof(NativeContext, hostSp) == 816);
static_assert(offsetof(NativeContext, memoryTable) == 824 && offsetof(NativeContext, spare) == 848 && offsetof(NativeContext, fpsr) == 856);
static_assert(offsetof(NativeContext, svcEntry) == 864 && offsetof(NativeContext, trapEntry) == 872);  // tools/build-native-code.py
static_assert(offsetof(NativeContext, signalReturn) == 880);  // NativeEntry.S
static_assert(offsetof(NativeContext, borrowed) == 904 && offsetof(NativeContext, lent) == 912);  // rewrite.py

namespace nestvane::native {
namespace {

constexpr int leftBySyscall = 1, leftByFault = 2, leftByPreemption = 3, leftByTrap = 4, leftByStray = 5,
              leftByInterpreter = 6;
constexpr uint64_t sliceNanoseconds = 1'000'000;
constexpr int preemptionSignal = SIGUSR2;

// The parts of a module's info block (experiments/g0/e3/rewrite.py --final-table).
struct Loaded {
    const Module* module;
    const uint16_t* lengths; // per guest instruction, how many host instructions its translation has
    const uint32_t* marks;   // where the translation of every 64th instruction starts, from codeStart
    uint64_t count;
    uint64_t codeStart;
    uint64_t trap;           // host code that leaves native execution at the module's entry point
    uint64_t stubsBegin;     // host address where the code ends and the entry stubs begin
    uint64_t stubCount;
    const uint32_t* stubs;   // for instructions entered through a stub: index, and where the stub is from codeStart
    bool active = false;

    // Where the translation of an instruction starts, from codeStart.
    uint64_t start(uint64_t index) const {
        uint64_t offset = marks[index / 64];
        for (uint64_t each = index & ~uint64_t{63}; each < index; ++each) offset += uint64_t{lengths[each]} * 4;
        return offset;
    }
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
    std::vector<size_t> byGuest;                               // modules in the order of their guest addresses
    std::vector<bool> made;                                    // per page of the table: its entries exist
    int tableFile = -1;                                        // what the table's pages are kept in
    uint64_t madePages = 0;
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
        // `high` is where the prelinked modules end; WebView's library is translated for an address of
        // its own above the addresses for code without a translation (webViewBase), and `top` takes it in.
        uint64_t low = UINT64_MAX, high = 0, top = 0;
        for (int index = 0; index < nestvane_native_module_count; ++index) {
            const auto& module = nestvane_native_modules[index];
            result.modules.push_back({&module, reinterpret_cast<const uint16_t*>(module.info[3]),
                                      reinterpret_cast<const uint32_t*>(module.info[4]),
                                      (module.info[1] - module.info[0]) / 4, module.info[5], module.info[6],
                                      module.info[7], module.info[8], reinterpret_cast<const uint32_t*>(module.info[9])});
            low = std::min(low, module.info[0]);
            if (module.info[0] >= webViewBase && module.info[1] <= webViewBase + webViewSize)
                top = std::max(top, module.info[1]);
            else
                high = std::max(high, module.info[1]);
        }
        std::sort(result.modules.begin(), result.modules.end(),
                  [](const Loaded& a, const Loaded& b) { return a.codeStart < b.codeStart; });
        for (size_t index = 0; index < result.modules.size(); ++index) {
            result.byKey[result.modules[index].module->key].push_back(index);
            result.byModule[result.modules[index].module] = index;
        }
        result.byGuest.resize(result.modules.size());
        for (size_t index = 0; index < result.byGuest.size(); ++index) result.byGuest[index] = index;
        std::sort(result.byGuest.begin(), result.byGuest.end(), [&](size_t a, size_t b) {
            return result.modules[a].module->info[0] < result.modules[b].module->info[0];
        });
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
        high = std::max<uint64_t>(foreignLow, (top + 0xffff) & ~uint64_t{0xffff});
        result.low = low;
        result.high = high;
        // Only address space: a module's table is mapped in when a process first maps the module, and
        // a look-up anywhere else faults, which sends the guest's jump to the interpreter.
        // The table also spans the addresses of code without a translation. Its pages are made when
        // they are first looked up in (see makePage), and they are pages of a file: the system can
        // write them out and take them back, which it cannot do with memory that has no file.
        result.covered = std::max(high, webViewBase + webViewSize);
        const size_t bytes = (result.covered - low) * 2;
        std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/nestvane-code-table.XXXXXX";
        result.tableFile = mkstemp(path.data());
        if (result.tableFile >= 0) unlink(path.c_str());
        if (result.tableFile < 0 || ftruncate(result.tableFile, off_t(bytes)) ||
            (result.table = static_cast<uint64_t*>(mmap(nullptr, bytes, PROT_NONE, MAP_SHARED, result.tableFile, 0))) == MAP_FAILED) {
            result.modules.clear();
            result.failure = "no file or no address space for the table of translated code";
            return result;
        }
        result.made.assign(bytes / GuestSpace::page, false);
        // The trampoline's entries: whichever of its instructions a handler returns to, native code
        // leaves there and the interpreter runs the trampoline.
        uint64_t* entries = result.table + (result.signalPage - low) / 4;
        if (mmap(entries, GuestSpace::page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED) {
            entries[1] = reinterpret_cast<uint64_t>(&nestvane_native_signal_return);
            result.traps.insert(entries[1]);
            result.made[(result.signalPage - low) * 2 / GuestSpace::page] = true;
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
    // The last mark at or before the offset, and from there by the lengths.
    uint64_t low = 0, high = (loaded->count + 63) / 64;
    while (high - low > 1) {
        const uint64_t middle = (low + high) / 2;
        if (loaded->marks[middle] <= offset) low = middle; else high = middle;
    }
    uint64_t index = low * 64, begins = loaded->marks[low];
    while (index + 1 < loaded->count && begins + uint64_t{loaded->lengths[index]} * 4 <= offset) begins += uint64_t{loaded->lengths[index++]} * 4;
    guestPc = loaded->module->info[0] + index * 4;
    atStart = begins == offset;
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
    uint64_t interpreterOffloads = 0, workerInterpreted = 0;
    std::atomic<uint64_t> workerEnds[8] = {};  // how runs of the interpreter on a worker ended: by native reason, 0 in the interpreter
} counters;

void onSignal(int number, siginfo_t* info, void* raw) {
    // A worker interpreting (interpretInSpace) touched memory that another thread unmapped meanwhile:
    // the run ends before that instruction.
    if (number == SIGSEGV || number == SIGBUS) {
        if (auto* jump = static_cast<sigjmp_buf*>(interpreterFaultJump())) siglongjmp(*jump, 1);
    }
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
    // x18 is the system's on iOS: the guest's is kept in its slot, as NativeEntry.S keeps it.
    std::memcpy(context.x, state.__x, sizeof(uint64_t) * 18);
    std::memcpy(context.x + 19, state.__x + 19, sizeof(uint64_t) * 6);
    context.x[29] = state.__fp;
    context.x[30] = state.__lr;
    context.sp = state.__sp;
    context.nzcv = state.__cpsr & 0xf0000000u;
    std::memcpy(context.q, user->uc_mcontext->__ns.__v, sizeof(context.q));
    context.fpsr = user->uc_mcontext->__ns.__fpsr;
    context.faultPc = pc;
    context.faultAddress = reinterpret_cast<uint64_t>(info->si_addr);
    // A fault while registers were lent: each lent register holds the guest register it stands for,
    // and its own value is in `lent`. Put them back where the guest has them.
    for (unsigned index = 0; index < 4 && (context.borrowed >> (10 * index) & 0x3ff); ++index) {
        const unsigned lender = context.borrowed >> (10 * index) & 31, guest = context.borrowed >> (10 * index + 5) & 31;
        context.x[guest] = context.x[lender];
        context.x[lender] = context.lent[index];
    }
    context.borrowed = 0;
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
    // A library stored uncompressed in an APK is mapped from the APK, at its place there: WebView's.
    // Its bytes run from that offset to the end of its section headers, which come last.
    if (offset && offset < file.size() && file.size() - offset >= 64 && std::memcmp(file.data() + offset, "\x7f" "ELF", 4) == 0 &&
        std::memcmp(file.data(), "\x7f" "ELF", 4) != 0) {
        const uint8_t* elf = file.data() + offset;
        uint64_t sections;
        uint16_t entrySize, count;
        std::memcpy(&sections, elf + 40, 8);
        std::memcpy(&entrySize, elf + 58, 2);
        std::memcpy(&count, elf + 60, 2);
        const uint64_t size = sections + uint64_t{entrySize} * count;
        if (sections > file.size() - offset || size > file.size() - offset) return nullptr;
        return mappedCode(file.subspan(offset, size), address, 0);
    }
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

namespace {

// Makes the page of the table that has the entry for guest pc, if a module that is in use has code
// there. The entries are written to the file first and the page is made readable after, so that
// another host thread never reads a page that is half made. Called on the main loop's thread only.
bool makePage(uint64_t pc) {
    auto& all = registry();
    if (pc < all.low || pc >= all.covered) return false;
    const size_t page = (pc - all.low) * 2 / GuestSpace::page;
    if (all.made[page]) return true;
    const auto next = std::upper_bound(all.byGuest.begin(), all.byGuest.end(), pc, [&](uint64_t address, size_t index) {
        return address < all.modules[index].module->info[0];
    });
    if (next == all.byGuest.begin()) return false;
    const Loaded& loaded = all.modules[*std::prev(next)];
    if (!loaded.active || pc >= loaded.module->info[1]) return false;
    constexpr size_t entries = GuestSpace::page / 8;
    static uint64_t* const buffer = new uint64_t[entries];
    const uint64_t first = all.low + page * (GuestSpace::page / 2), base = loaded.module->info[0];
    // What is no instruction of the module leaves native code when it is jumped to.
    std::fill_n(buffer, entries, reinterpret_cast<uint64_t>(&nestvane_native_stray));
    const uint64_t from = std::max(first, base), to = std::min(first + GuestSpace::page / 2, loaded.module->info[1]);
    uint64_t index = (from - base) / 4, offset = loaded.start(index);
    for (uint64_t address = from; address < to; address += 4, ++index) {
        buffer[(address - first) / 4] = loaded.codeStart + offset;
        offset += uint64_t{loaded.lengths[index]} * 4;
    }
    // Instructions that are entered through a stub.
    uint64_t low = 0, high = loaded.stubCount;
    const uint64_t firstIndex = (from - base) / 4, endIndex = (to - base) / 4;
    while (low < high) {
        const uint64_t middle = (low + high) / 2;
        if (loaded.stubs[middle * 2] < firstIndex) low = middle + 1; else high = middle;
    }
    for (; low < loaded.stubCount && loaded.stubs[low * 2] < endIndex; ++low)
        buffer[(base + uint64_t{loaded.stubs[low * 2]} * 4 - first) / 4] = loaded.codeStart + loaded.stubs[low * 2 + 1];
    if (loaded.trap && all.traps.contains(loaded.trap) && loaded.module->info[2] >= from && loaded.module->info[2] < to)
        buffer[(loaded.module->info[2] - first) / 4] = loaded.trap;
    const off_t position = off_t(page * GuestSpace::page);
    if (pwrite(all.tableFile, buffer, GuestSpace::page, position) != ssize_t(GuestSpace::page) ||
        mprotect(reinterpret_cast<uint8_t*>(all.table) + position, GuestSpace::page, PROT_READ)) return false;
    all.made[page] = true;
    ++all.madePages;
    return true;
}

} // namespace

bool activate(const Module& module) {
    auto& all = registry();
    const auto found = all.byModule.find(&module);
    if (found == all.byModule.end()) return false;
    all.modules[found->second].active = true;
    return true;
}

void trap(uint64_t pc) {
    auto& all = registry();
    for (auto& loaded : all.modules) {
        if (!loaded.active || loaded.module->info[2] != pc || !loaded.trap) continue;
        if (!all.traps.insert(loaded.trap).second) return;
        // A page that exists already gets the entry now; one that is made later gets it then.
        const size_t page = (pc - all.low) * 2 / GuestSpace::page;
        if (all.made[page]) pwrite(all.tableFile, &loaded.trap, 8, off_t((pc - all.low) * 2));
        return;
    }
}

namespace {

// Puts the thread's registers into a context that enters native code at cpu.pc. False if the
// instruction there is not entered natively.
// The context for entering native code at cpu.pc, whose page of the table exists. False if pc is not
// the start of translated code.
bool loadContext(const CPU& cpu, const GuestSpace& space, uint64_t stack, NativeContext& context) {
    auto& all = registry();
    const uint64_t entry = all.table[(cpu.pc - all.low) / 4];
    if (!entry || entry == reinterpret_cast<uint64_t>(&nestvane_native_stray) || all.traps.contains(entry)) return false;
    std::memcpy(context.x, cpu.x.data(), sizeof(context.x));
    std::memcpy(context.q, cpu.v.data(), sizeof(context.q));
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

bool load(Machine& machine, NativeContext& context) {
    auto& all = registry();
    auto& cpu = machine.cpu;
    if (cpu.pc < all.low || cpu.pc >= all.high || cpu.pc % 4 || !makePage(cpu.pc)) return false;
    return loadContext(cpu, *machine.space, machine.dataAddress(cpu.sp), context);
}

// The same on a worker, which does not make pages of the table: only the main loop does.
bool loadOnWorker(const CPU& cpu, const GuestSpace& space, NativeContext& context) {
    auto& all = registry();
    if (cpu.pc < all.low || cpu.pc >= all.high || cpu.pc % 4) return false;
    if (!all.made[(cpu.pc - all.low) * 2 / GuestSpace::page]) return false;
    return loadContext(cpu, space, cpu.sp & 0x00ff'ffff'ffff'ffffULL, context);
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
    // x25 to x28 are the runtime's own in rewritten code. Hand-written assembly that uses them all the
    // same has their guest values kept in the context's slots (Rewriter.reserved in rewrite.py).
    std::memcpy(cpu.x.data() + 25, context.x + 25, sizeof(uint64_t) * 4);
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
    if (result.reason == leftByInterpreter) {
        counters.workerInterpreted += result.interpreted;
        return Exit::interpret;
    }
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
    if (result.reason == leftByStray) {
        // A jump into a module, to what is no instruction of it.
        ++counters.lost;
        machine.fault(cpu.pc, cpu.pc);
        return Exit::lost;
    }
    if (result.faultPc >= all.codeStart && result.faultPc < all.codeEnd && guestCode(result.faultPc, guestPc, atStart)) {
        cpu.pc = guestPc;
        lastFault = result.faultAddress;
        // A look-up in a page of the table that had not been made yet: now it is, if a module has code there.
        const uint64_t tableStart = reinterpret_cast<uint64_t>(all.table);
        if (result.faultAddress >= tableStart && result.faultAddress < tableStart + (all.covered - all.low) * 2) {
            const uint64_t target = all.low + (result.faultAddress - tableStart) / 2;
            if (!all.made[(target - all.low) * 2 / GuestSpace::page] && makePage(target)) return Exit::retry;
        }
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
    // A run of the interpreter instead (offloadInterpreter): the thread's registers and its code.
    bool interpreting = false;
    CPU cpu;
    SpaceCode code;
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
        Result result;
        if (worker->interpreting) {
            // The thread's code without a translation is interpreted here, and what it calls that has one
            // runs natively here too, as the main loop would do, for about 25 ms. The run ends at
            // anything else: what the interpreter here does not do, a system call, a fault. When it ends
            // in native code, the main loop takes the registers from the native context.
            const uint64_t until = mach_absolute_time() + machTime(25'000'000);
            const auto& code = worker->code;
            uint64_t interpreted = 0;
            for (;;) {
                interpreted += interpretInSpace(worker->cpu, *worker->space, code, 2'000'000, worker->running.recall);
                const uint64_t pc = worker->cpu.pc;
                if (pc - code.start < code.end - code.start || worker->running.recall.load(std::memory_order_relaxed) ||
                    mach_absolute_time() >= until || !loadOnWorker(worker->cpu, *worker->space, worker->running.context)) {
                    counters.workerEnds[pc - code.start < code.end - code.start ? 0 : 7].fetch_add(1, std::memory_order_relaxed);
                    result = {};
                    result.reason = leftByInterpreter;
                    result.pc = pc;
                    break;
                }
                result = enter(worker->running);
                if (result.reason == leftByTrap && result.pc - code.start < code.end - code.start &&
                    !worker->running.recall.load(std::memory_order_relaxed) && mach_absolute_time() < until) {
                    // Back in the code being interpreted.
                    store(worker->running.context, worker->cpu);
                    worker->cpu.pc = result.pc;
                    continue;
                }
                counters.workerEnds[std::min(result.reason, 6)].fetch_add(1, std::memory_order_relaxed);
                worker->interpreting = false;
                break;
            }
            result.interpreter = true;
            result.interpreted = interpreted;
        } else {
            result = enter(worker->running);
        }
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

bool offloadInterpreter(Machine& machine, uint64_t ticket, const SpaceCode& code) {
    if (pool.outstanding.load(std::memory_order_relaxed) == pool.workers.size() || !machine.space) return false;
    std::lock_guard lock(pool.mutex);
    for (auto& worker : pool.workers) {
        if (worker->state != Worker::State::idle) continue;
        worker->interpreting = true;
        worker->cpu = machine.cpu;
        worker->cpu.output.clear();
        worker->code = code;
        worker->ticket = ticket;
        worker->space = machine.space;
        worker->running.recall.store(false, std::memory_order_relaxed);
        worker->state = Worker::State::assigned;
        pool.outstanding.fetch_add(1, std::memory_order_relaxed);
        ++counters.interpreterOffloads;
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
    if (cpu && worker->interpreting) {
        // The main loop's count of instructions and output stay its own.
        const auto retired = cpu->retired;
        auto output = std::move(cpu->output);
        *cpu = worker->cpu;
        cpu->retired = retired;
        cpu->output = std::move(output);
    } else if (cpu) {
        store(worker->running.context, *cpu);
    }
    worker->interpreting = false;
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
        << ", lost " << counters.lost << ", runs on " << pool.workers.size() << " workers " << counters.offloads
        << ", interpreted on workers " << counters.workerInterpreted << " instructions in " << counters.interpreterOffloads << " runs\n";
    if (counters.interpreterOffloads) {
        out << interpreterStops() << "  and ended: in the interpreter " << counters.workerEnds[0] << ", native for a system call "
            << counters.workerEnds[leftBySyscall] << ", a fault " << counters.workerEnds[leftByFault] << ", preemption "
            << counters.workerEnds[leftByPreemption] << ", a trap elsewhere " << counters.workerEnds[leftByTrap]
            << ", stray " << counters.workerEnds[leftByStray] << ", not entered natively " << counters.workerEnds[7] << '\n';
    }
    static const mach_timebase_info_data_t base = [] { mach_timebase_info_data_t value; mach_timebase_info(&value); return value; }();
    const auto milliseconds = [&](uint64_t time) { return time * base.numer / base.denom / 1'000'000; };
    out << "  native execution time: " << milliseconds(counters.nativeTime) << " ms in guest code, "
        << registry().madePages * GuestSpace::page / (1024 * 1024) << " MiB of code table made";
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
