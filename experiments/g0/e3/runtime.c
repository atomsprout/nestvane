// Host runtime for guests rewritten by rewrite.py: a guest thread is a host thread, guest memory is
// host memory at an offset, fork is a copy-on-write vm_remap, and Linux system calls are served by
// the host. It implements what toybox and ART's dalvikvm need, static (musl) or dynamic (Bionic).
//
// Guest memory comes in chunks (16 MiB unless CHUNK_SHIFT says otherwise), each with its own offset to host memory. Built plainly, one
// process is one block of SEGMENT bytes and every chunk has the same offset, which rewritten code
// keeps in a register. Built with -DSPARSE (for code rewritten with --table), the guest has a 39-bit
// address space and a chunk gets host memory only when something in it is mapped: see "guest memory".
//
// usage: <program> <guest elf> <guest argv...>       one static module, files are the host's
//        <program> <guest root> <guest argv...>      Android layout: argv[0] is a path under the root
//
// The modules linked into the program are listed in guest_modules (see modules.py).

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef SPARSE
#undef SEGMENT
#define SEGMENT (1ull << 39)
#define LAZY_MINIMUM (32ull << 20)  // anonymous mappings from this size get host memory when first touched
#else
#ifndef SEGMENT
#define SEGMENT (1ull << 30)  // guest address space per process; -DSEGMENT=... to change
#endif
#define LAZY_MINIMUM (~0ull)
#endif
#define PAGE 16384ull
#define STACK_SIZE (8ull << 20)
#define BRK_LIMIT (128ull << 20)
#define ROOT_BRK_START (64ull << 20)  // with a guest root, modules are prelinked above the heap
#define MAX_FDS 256
#define MAX_PROCESSES 1024
#define MAX_THREADS 256

struct Process;
struct Thread;

struct Context {
    uint64_t x[31];
    uint64_t sp;  // host address
    __uint128_t q[32];
    uint64_t nzcv, fpcr, tpidr;
    uint64_t pc;  // host address
    uint64_t base;  // the offset from guest to host addresses of the chunks that hold this thread's stack
    uint64_t table, host_sp;
    uint64_t memory_table;  // sparse: the process's per-chunk offsets from guest to host addresses
    struct Process* process;
    struct Thread* thread;
    uint64_t spare;  // sparse: scratch for rewritten code that sets sp
};
_Static_assert(__builtin_offsetof(struct Context, sp) == 248, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, q) == 256, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, nzcv) == 768, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, tpidr) == 784, "rewrite.py");
_Static_assert(__builtin_offsetof(struct Context, pc) == 792, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, host_sp) == 816, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, memory_table) == 824, "entry.S, rewrite.py");
_Static_assert(__builtin_offsetof(struct Context, spare) == 848, "rewrite.py");
#ifndef CHUNK_SHIFT
#define CHUNK_SHIFT 24  // must match rewrite.py --chunk-shift
#endif
#define CHUNK (1ull << CHUNK_SHIFT)
#define CHUNKS (SEGMENT >> CHUNK_SHIFT)
// The offset of a chunk without host memory. It leads above every address a process can map, on
// macOS and on iOS, so an access faults and the fault address still tells the guest address.
#define VOID_OFFSET (1ull << 47)
// Unmapped host pages around each run of chunks. An access whose offset from its base register
// leaves the run lands here: pairs reach 1 KiB below the base, scaled offsets 64 KiB above.
#define GUARD_BEFORE PAGE
#define GUARD_AFTER (5 * PAGE)
#define ADDRESS_MASK 0x00ffffffffffffffull  // Bionic tags heap pointers in the top byte
#define SIGBIT(number) (1ull << ((number) - 1))  // Linux signal masks: signal n is bit n - 1

// One translated ELF. `info` is the block rewrite.py emits: text start, text end, entry, table of
// host code per instruction, and the entry stubs (pairs of instruction index and host code, count).
struct Module {
    const char* guest_path;  // NULL for the single static module named on the command line
    const uint64_t* info;
    uint64_t base;
    // Filled at startup.
    const char* host_path;
    const uint8_t* image;
    uint64_t end, headers;  // guest addresses: end of the image, and its program headers
    int32_t device;
    uint64_t inode;
};
extern struct Module guest_modules[];
extern const int guest_module_count;

// Consecutive guest chunks that are consecutive in host memory.
struct Run { uint64_t guest, length; uint8_t* host; };

struct Process {
    int pid, parent;
    int exited, status, reaped, vfork_done;
    const struct Module* executable;
    uint64_t brk_start, brk, map_low, map_high;
    uint64_t map_hint;  // where the next search for free pages starts
    // The address space. The three tables are one block, so that fork copies them lazily.
    void* space;
    uint64_t* chunks;     // per chunk: host address minus guest address, or VOID_OFFSET
    uint8_t* used;        // one bit per guest page that is mapped
    uint8_t* protection;  // two bits per guest page: PROT_READ, PROT_WRITE
    struct Run* runs;
    int run_count, run_capacity;
    int fds[MAX_FDS];
    uint8_t cloexec[MAX_FDS];
    DIR* directories[MAX_FDS];
    struct { uint64_t handler, flags, mask; } actions[65];  // signal dispositions as the guest set them
    uint64_t pending;  // signals sent to the process and not yet taken by a thread
    uint8_t queued[65][128];  // siginfo for signals sent with rt_tgsigqueueinfo
    uint64_t has_queued;
    struct Thread* threads[MAX_THREADS];
    int live_threads, exiting;
    char command_line[2048];  // argv as /proc/self/cmdline shows it: strings, each ending in a zero byte
    int command_length;
    pthread_mutex_t memory_lock;  // guards the address space and the descriptor table; recursive
};

// One guest thread is one host thread. The first thread's tid is the process's pid.
struct Thread {
    int tid, dead;
    struct Process* process;
    pthread_t host;
    uint64_t blocked;
    uint64_t pending;  // signals sent to this thread in particular
    uint64_t clear_tid;  // CLONE_CHILD_CLEARTID: zeroed and woken when the thread exits
    uint64_t alternate_stack, alternate_size;  // sigaltstack, as guest addresses
    // The host fault being handled: where it happened, and what the guest is told if it is the guest's.
    struct { int code; uint64_t address, host_address, host_pc, repeat_pc, repeat_address; int repeats; } fault;
    struct Context context;
};

void guest_enter(struct Context*) __attribute__((noreturn));
void guest_resume(struct Context*) __attribute__((noreturn));
void guest_signal_return(void);  // entry.S: what a guest handler returns to

static const char* root;  // host directory that is the guest's "/", or NULL
static size_t root_length;
static uint64_t code_low, code_high;
static uint64_t* code_table;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct Process* processes[MAX_PROCESSES];
static int next_pid = 100;
static int trace;
static uint64_t syscall_count, fork_count, fork_ns, lazy_count, emulated_count;
static int64_t reserved_bytes, reserved_peak;  // host address space held by guest memory, over all processes

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t page_up(uint64_t value) { return (value + PAGE - 1) & ~(PAGE - 1); }

static long linux_errno(int error) {
    static const short table[][2] = {{11, 35}, {35, 11}, {36, 115}, {37, 114}, {38, 88}, {39, 89}, {40, 90}, {41, 91}, {42, 92},
        {43, 93}, {45, 95}, {47, 97}, {48, 98}, {49, 99}, {50, 100}, {51, 101}, {54, 104}, {55, 105}, {56, 106}, {57, 107},
        {60, 110}, {61, 111}, {62, 40}, {63, 36}, {64, 112}, {65, 113}, {66, 39}, {69, 122}, {70, 116}, {77, 37}, {78, 38},
        {84, 75}, {89, 125}, {92, 84}, {93, 61}};
    for (size_t index = 0; index < sizeof table / sizeof table[0]; ++index)
        if (table[index][0] == error) return -table[index][1];
    return error <= 34 ? -error : -5;
}
#define RESULT(value) ({ long result_ = (long)(value); result_ < 0 ? linux_errno(errno) : result_; })

static uint64_t materialize(struct Process* process, uint64_t chunk);
static int page_used(const struct Process* process, uint64_t page) { return process->used[page / 8] >> (page % 8) & 1; }

// The host address of a guest address. The CPU ignores a pointer's tag on access, but the host kernel
// does not. Memory that has not been touched yet gets its host memory here, since the host kernel
// cannot fault it in on the guest's behalf.
static void* host(struct Process* process, uint64_t address) {
    address &= ADDRESS_MASK;
    if (!address) return NULL;
    if (address >= SEGMENT) return (void*)(address + VOID_OFFSET);
    uint64_t offset = process->chunks[address >> CHUNK_SHIFT];
    if (offset == VOID_OFFSET && page_used(process, address / PAGE)) offset = materialize(process, address >> CHUNK_SHIFT);
    return (void*)(address + offset);
}

// Descriptors are 32-bit arguments. The upper half of the register is not defined.
static int host_fd(struct Process* process, long wide) {
    const int fd = (int)wide;
    return fd >= 0 && fd < MAX_FDS ? process->fds[fd] : -1;
}

static long install_fd(struct Process* process, int descriptor, int minimum, int cloexec) {
    if (descriptor < 0) return linux_errno(errno);
    pthread_mutex_lock(&process->memory_lock);
    for (int fd = minimum; fd < MAX_FDS; ++fd)
        if (process->fds[fd] < 0) {
            process->fds[fd] = descriptor;
            process->cloexec[fd] = (uint8_t)cloexec;
            pthread_mutex_unlock(&process->memory_lock);
            return fd;
        }
    pthread_mutex_unlock(&process->memory_lock);
    close(descriptor);
    return -24;
}

static void close_fd(struct Process* process, int fd) {
    if (process->fds[fd] < 0) return;
    if (process->directories[fd]) closedir(process->directories[fd]);
    process->directories[fd] = NULL;
    close(process->fds[fd]);
    process->fds[fd] = -1;
}

// ---------------------------------------------------------------- modules

static const struct Module* module_for_stat(const struct stat* info) {
    for (int index = 0; index < guest_module_count; ++index)
        if (guest_modules[index].image && guest_modules[index].inode == info->st_ino && guest_modules[index].device == info->st_dev)
            return &guest_modules[index];
    return NULL;
}

static const struct Module* module_for_file(const char* path) {
    struct stat info;
    return stat(path, &info) == 0 ? module_for_stat(&info) : NULL;
}

// Maps every translated ELF and builds the one table that turns a guest pc into host code.
static int load_modules(const char* single) {
    code_low = ~0ull;
    for (int index = 0; index < guest_module_count; ++index) {
        struct Module* module = &guest_modules[index];
        char path[MAXPATHLEN];
        if (module->guest_path && root) snprintf(path, sizeof path, "%s%s", root, module->guest_path);
        else if (!module->guest_path && single) snprintf(path, sizeof path, "%s", single);
        else continue;
        module->host_path = realpath(path, NULL);
        const int descriptor = module->host_path ? open(module->host_path, O_RDONLY) : -1;
        struct stat info;
        if (descriptor < 0 || fstat(descriptor, &info) != 0) {
            fprintf(stderr, "[runtime] cannot open module %s\n", path);
            return 0;
        }
        module->image = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
        close(descriptor);
        module->device = info.st_dev;
        module->inode = info.st_ino;
        const uint64_t phoff = *(uint64_t*)(module->image + 32);
        const uint16_t phentsize = *(uint16_t*)(module->image + 54), phnum = *(uint16_t*)(module->image + 56);
        for (int header = 0; header < phnum; ++header) {
            const uint8_t* entry = module->image + phoff + (uint64_t)header * phentsize;
            if (*(uint32_t*)entry != 1) continue;
            const uint64_t high = module->base + *(uint64_t*)(entry + 16) + *(uint64_t*)(entry + 40);
            if (page_up(high) > module->end) module->end = page_up(high);
            if (*(uint64_t*)(entry + 8) == 0) module->headers = module->base + *(uint64_t*)(entry + 16) + phoff;
        }
        if (module->info[0] < code_low) code_low = module->info[0];
        if (module->info[1] > code_high) code_high = module->info[1];
    }
    if (code_low >= code_high) return 0;
    // One extra entry: the guest address code_high is where signal handlers return.
    code_table = mmap(NULL, (code_high - code_low) * 2 + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    code_table[(code_high - code_low) / 4] = (uint64_t)&guest_signal_return;
    for (int index = 0; index < guest_module_count; ++index) {
        const struct Module* module = &guest_modules[index];
        if (!module->image) continue;
        memcpy(code_table + (module->info[0] - code_low) / 4, (const void*)module->info[3], (module->info[1] - module->info[0]) * 2);
        // An instruction that relies on what the one before left in the scratch register is entered
        // through a stub when control arrives any other way than by falling into it.
        const uint64_t* stubs = (const uint64_t*)module->info[4];
        for (uint64_t index = 0; index < module->info[5]; ++index)
            code_table[(module->info[0] - code_low) / 4 + stubs[index * 2]] = stubs[index * 2 + 1];
    }
    return 1;
}

static uint64_t host_code(uint64_t guest_pc) { return code_table[(guest_pc - code_low) / 4]; }

// The guest instruction whose translation contains a host pc, or 0 outside translated code.
static uint64_t guest_code(uint64_t host_pc) {
    for (int index = 0; index < guest_module_count; ++index) {
        const struct Module* module = &guest_modules[index];
        if (!module->image) continue;
        const uint64_t* table = (const uint64_t*)module->info[3];
        uint64_t low = 0, high = (module->info[1] - module->info[0]) / 4;
        // The entry after the last instruction is the module's closing brk, so `high` is a valid index.
        if (host_pc < table[0] || host_pc >= table[high - 1] + 64) continue;
        while (high - low > 1) {
            const uint64_t middle = (low + high) / 2;
            if (table[middle] <= host_pc) low = middle; else high = middle;
        }
        return module->info[0] + low * 4;
    }
    return 0;
}

// Where straight-line execution reaches a guest instruction, as opposed to host_code, which is where a
// jump to it lands. 0 outside the modules.
static uint64_t inline_code(uint64_t guest_pc) {
    for (int index = 0; index < guest_module_count; ++index) {
        const struct Module* module = &guest_modules[index];
        if (module->image && guest_pc >= module->info[0] && guest_pc < module->info[1])
            return ((const uint64_t*)module->info[3])[(guest_pc - module->info[0]) / 4];
    }
    return 0;
}

// ---------------------------------------------------------------- guest memory
//
// A chunk has host memory ("is backed") or its offset is VOID_OFFSET. Backed chunks are grouped in
// runs: one host reservation each, with guard pages on both sides. Within a run, guest memory is
// contiguous in host memory. A guest page is mapped when its bit in `used` is set, and its
// protection is in `protection`. A mapped page in a chunk that is not backed is anonymous memory
// that nothing has touched: the first access faults and backs the chunk (materialize).
//
// What rewritten code relies on:
//   - a thread's stack lies within one run, because sp is a host address and moves natively;
//   - an access that leaves the run of its base register faults in a guard or in the void, and
//     is then carried out by emulate_access.

static uint64_t space_size(void) { return page_up(CHUNKS * 8 + SEGMENT / PAGE / 8 + SEGMENT / PAGE / 4); }

static void attach_space(struct Process* process, void* block) {
    process->space = block;
    process->chunks = block;
    process->used = (uint8_t*)block + CHUNKS * 8;
    process->protection = process->used + SEGMENT / PAGE / 8;
}

static int chunk_backed(const struct Process* process, uint64_t chunk) { return process->chunks[chunk] != VOID_OFFSET; }
static int page_protection(const struct Process* process, uint64_t page) { return process->protection[page / 4] >> (page % 4 * 2) & 3; }

static int chunk_empty(const struct Process* process, uint64_t chunk) {
    const uint8_t* bits = process->used + chunk * (CHUNK / PAGE / 8);
    for (uint64_t index = 0; index < CHUNK / PAGE / 8; ++index)
        if (bits[index]) return 0;
    return 1;
}

// The run that holds a guest address, or NULL.
static struct Run* run_at(struct Process* process, uint64_t guest) {
    for (int index = 0; index < process->run_count; ++index)
        if (guest - process->runs[index].guest < process->runs[index].length) return &process->runs[index];
    return NULL;
}

// The run whose memory or guards hold a host address, or NULL.
static struct Run* run_of_host(struct Process* process, uint64_t address) {
    for (int index = 0; index < process->run_count; ++index) {
        const struct Run* run = &process->runs[index];
        if (address - ((uint64_t)run->host - GUARD_BEFORE) < GUARD_BEFORE + run->length + GUARD_AFTER) return &process->runs[index];
    }
    return NULL;
}

static struct Run* add_run(struct Process* process) {
    if (process->run_count == process->run_capacity) {
        process->run_capacity = process->run_capacity ? process->run_capacity * 2 : 16;
        process->runs = realloc(process->runs, (size_t)process->run_capacity * sizeof *process->runs);
    }
    return &process->runs[process->run_count++];
}

static void count_reserved(int64_t bytes) {
    const int64_t now = __atomic_add_fetch(&reserved_bytes, bytes, __ATOMIC_RELAXED);
    if (now > reserved_peak) reserved_peak = now;
}

static uint8_t* reserve(uint8_t* hint, uint64_t length) {
    void* result = mmap(hint, length, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result != MAP_FAILED) count_reserved((int64_t)length);
    return result == MAP_FAILED ? NULL : result;
}

static void unreserve(uint8_t* memory, uint64_t length) {
    munmap(memory, length);
    count_reserved(-(int64_t)length);
}

// Gives host memory to chunks [first, last), none of which has any. It continues the run on either
// side when the host addresses next to it are free, so that neighbours in the guest stay neighbours
// in the host.
static int back_range(struct Process* process, uint64_t first, uint64_t last) {
    const uint64_t guest = first << CHUNK_SHIFT, length = (last - first) << CHUNK_SHIFT;
    struct Run* before = first ? run_at(process, guest - 1) : NULL;
    struct Run* after = last < CHUNKS ? run_at(process, guest + length) : NULL;
    struct Run* run = NULL;
    if (before) {  // the new chunks take over its trailing guard, and end with a new one
        uint8_t* wanted = before->host + before->length + GUARD_AFTER;
        uint8_t* got = reserve(wanted, length);
        if (got == wanted) {
            before->length += length;
            run = before;
        } else if (got) {
            unreserve(got, length);
        }
    }
    if (!run && after) {
        uint8_t* wanted = after->host - GUARD_BEFORE - length;
        uint8_t* got = reserve(wanted, length);
        if (got == wanted) {
            after->host -= length;
            after->guest -= length;
            after->length += length;
            run = after;
        } else if (got) {
            unreserve(got, length);
        }
    }
    if (!run) {
        uint8_t* got = reserve(NULL, GUARD_BEFORE + length + GUARD_AFTER);
        if (!got) return 0;
        run = add_run(process);
        *run = (struct Run){guest, length, got + GUARD_BEFORE};
    }
    for (uint64_t chunk = first; chunk < last; ++chunk) process->chunks[chunk] = (uint64_t)run->host - run->guest;
    // Mapped pages in these chunks were untouched anonymous memory. Now they are host memory.
    for (uint64_t page = guest / PAGE; page < (guest + length) / PAGE;) {
        const int protection = page_used(process, page) ? page_protection(process, page) : 0;
        uint64_t end = page + 1;
        while (end < (guest + length) / PAGE && (page_used(process, end) ? page_protection(process, end) : 0) == protection) ++end;
        if (protection) mprotect(run->host + (page * PAGE - run->guest), (end - page) * PAGE, protection);
        page = end;
    }
    return 1;
}

// Backs every chunk that [address, address + length) touches.
static int back(struct Process* process, uint64_t address, uint64_t length) {
    const uint64_t end = (address + length + CHUNK - 1) >> CHUNK_SHIFT;
    for (uint64_t chunk = address >> CHUNK_SHIFT; chunk < end;) {
        if (chunk_backed(process, chunk)) { ++chunk; continue; }
        uint64_t last = chunk + 1;
        while (last < end && !chunk_backed(process, last)) ++last;
        if (!back_range(process, chunk, last)) return 0;
        chunk = last;
    }
    return 1;
}

// Backs one chunk on first use. Returns its offset.
static uint64_t materialize(struct Process* process, uint64_t chunk) {
    pthread_mutex_lock(&process->memory_lock);
    if (!chunk_backed(process, chunk) && back_range(process, chunk, chunk + 1)) __atomic_add_fetch(&lazy_count, 1, __ATOMIC_RELAXED);
    const uint64_t offset = process->chunks[chunk];
    pthread_mutex_unlock(&process->memory_lock);
    return offset;
}

#ifdef SPARSE
// Returns a chunk's host memory. The guards of what remains of its run are kept.
static void free_chunk(struct Process* process, uint64_t chunk) {
    const uint64_t guest = chunk << CHUNK_SHIFT;
    struct Run* run = run_at(process, guest);
    uint8_t* memory = run->host + (guest - run->guest);
    const int first = guest == run->guest, last = guest + CHUNK == run->guest + run->length;
    if (first && last) {
        unreserve(memory - GUARD_BEFORE, GUARD_BEFORE + CHUNK + GUARD_AFTER);
        *run = process->runs[--process->run_count];
    } else if (first) {
        mmap(memory + CHUNK - GUARD_BEFORE, GUARD_BEFORE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        unreserve(memory - GUARD_BEFORE, CHUNK);
        run->host += CHUNK;
        run->guest += CHUNK;
        run->length -= CHUNK;
    } else if (last) {
        mmap(memory, GUARD_AFTER, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        unreserve(memory + GUARD_AFTER, CHUNK);
        run->length -= CHUNK;
    } else {
        const struct Run right = {guest + CHUNK, run->guest + run->length - guest - CHUNK, memory + CHUNK};
        run->length = guest - run->guest;
        mmap(memory, GUARD_AFTER, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        mmap(memory + CHUNK - GUARD_BEFORE, GUARD_BEFORE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        unreserve(memory + GUARD_AFTER, CHUNK - GUARD_AFTER - GUARD_BEFORE);
        count_reserved(GUARD_AFTER + GUARD_BEFORE);  // the two runs have a guard each where they had none
        *add_run(process) = right;
    }
    process->chunks[chunk] = VOID_OFFSET;
}
#endif

// Returns the host memory of every run. The tables stay: other threads of a process that is
// exiting may still look at them.
static void free_space(struct Process* process) {
    for (int index = 0; index < process->run_count; ++index)
        unreserve(process->runs[index].host - GUARD_BEFORE, GUARD_BEFORE + process->runs[index].length + GUARD_AFTER);
    process->run_count = 0;
}

// The rest, once no thread of the process can run any more.
static void free_tables(struct Process* process) {
    if (process->space) munmap(process->space, space_size());
    free(process->runs);
    process->space = process->runs = NULL;
    process->run_capacity = 0;
}

static void* empty_space;

static void make_empty_space(void) {
    uint64_t* block = mmap(NULL, space_size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (uint64_t chunk = 0; chunk < CHUNKS; ++chunk) block[chunk] = VOID_OFFSET;
    empty_space = block;
}

// Empties the address space. A sparse one starts with no host memory, a plain one with all of it.
static int reset_space(struct Process* process) {
    // The tables of an empty address space are a copy-on-write copy of one that is filled in once, so
    // that a process only pays for the parts of them it uses.
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, make_empty_space);
    void* empty = empty_space;
    free_space(process);
    if (process->space) munmap(process->space, space_size());
    vm_address_t block = 0;
    vm_prot_t current, maximum;
    if (vm_remap(mach_task_self(), &block, space_size(), 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)empty, TRUE, &current, &maximum,
                 VM_INHERIT_COPY) != KERN_SUCCESS)
        return 0;
    attach_space(process, (void*)block);
#ifndef SPARSE
    if (!back_range(process, 0, CHUNKS)) return 0;
#endif
    return 1;
}

// How much of [address, address + length) is contiguous in host memory from its start.
static uint64_t piece(const struct Process* process, uint64_t address, uint64_t length) {
    const uint64_t offset = process->chunks[address >> CHUNK_SHIFT];
    uint64_t end = (address | (CHUNK - 1)) + 1;
    while (end < address + length && process->chunks[end >> CHUNK_SHIFT] == offset) end += CHUNK;
    return end - address < length ? end - address : length;
}

// Sets the mapped bit and the protection of whole pages.
static void mark(struct Process* process, uint64_t address, uint64_t length, int used, int protection) {
    uint64_t page = address / PAGE;
    const uint64_t end = (address + length) / PAGE;
    while (page < end) {
        if (page % 8 == 0 && end - page >= 8) {  // whole bytes of both tables
            const uint64_t pages = (end - page) & ~7ull;
            memset(process->used + page / 8, used ? 0xff : 0, pages / 8);
            memset(process->protection + page / 4, protection * 0x55, pages / 4);
            page += pages;
            continue;
        }
        if (used) process->used[page / 8] |= (uint8_t)(1 << page % 8);
        else process->used[page / 8] &= (uint8_t)~(1 << page % 8);
        process->protection[page / 4] = (uint8_t)((process->protection[page / 4] & ~(3 << page % 4 * 2)) | protection << page % 4 * 2);
        ++page;
    }
}

// Changes only the protection that is recorded, for the pages that are mapped.
static void record_protection(struct Process* process, uint64_t address, uint64_t length, int protection) {
    for (uint64_t page = address / PAGE; page < (address + length) / PAGE; ++page)
        process->protection[page / 4] = (uint8_t)((process->protection[page / 4] & ~(3 << page % 4 * 2)) | protection << page % 4 * 2);
}

// mmap with MAP_FIXED on the host memory of a guest range. Chunks without host memory are skipped:
// they hold nothing, and get their protection when they are backed.
static int map_pages(struct Process* process, uint64_t address, uint64_t length, int protection, int flags, int descriptor, uint64_t offset) {
    for (uint64_t done = 0; done < length;) {
        const uint64_t part = piece(process, address + done, length - done);
        if (chunk_backed(process, (address + done) >> CHUNK_SHIFT) &&
            mmap((void*)(address + done + process->chunks[(address + done) >> CHUNK_SHIFT]), part, protection, MAP_FIXED | flags, descriptor,
                 descriptor < 0 ? 0 : (off_t)(offset + done)) == MAP_FAILED)
            return 0;
        done += part;
    }
    return 1;
}

static void protect_pages(struct Process* process, uint64_t address, uint64_t length, int protection) {
    for (uint64_t done = 0; done < length;) {
        const uint64_t part = piece(process, address + done, length - done);
        if (chunk_backed(process, (address + done) >> CHUNK_SHIFT))
            mprotect((void*)(address + done + process->chunks[(address + done) >> CHUNK_SHIFT]), part, protection);
        done += part;
    }
}

// Copies between host memory and a guest range that need not be contiguous in host memory.
static void copy_in(struct Process* process, uint64_t address, const void* from, uint64_t length) {
    for (uint64_t done = 0; done < length;) {
        uint8_t* target = host(process, address + done);
        const uint64_t part = piece(process, address + done, length - done);
        memcpy(target, (const uint8_t*)from + done, part);
        done += part;
    }
}

static void copy_out(struct Process* process, void* to, uint64_t address, uint64_t length) {
    for (uint64_t done = 0; done < length;) {
        const uint8_t* source = host(process, address + done);
        const uint64_t part = piece(process, address + done, length - done);
        memcpy((uint8_t*)to + done, source, part);
        done += part;
    }
}

// The host parts of a guest buffer, for readv and writev. Returns how many there are, at most `room`.
static int buffer_parts(struct Process* process, uint64_t address, uint64_t length, struct iovec* parts, int room) {
    int count = 0;
    address &= ADDRESS_MASK;
    for (uint64_t done = 0; done < length && count < room; ++count) {
        parts[count].iov_base = host(process, address + done);
        parts[count].iov_len = piece(process, address + done, length - done);
        done += parts[count].iov_len;
    }
    return count;
}

// Whether a mapping may be placed at an address: a mapping that gets host memory at once has all of
// it in one run, which is what a stack needs. Its chunks are all backed alike or all not backed.
static int placeable(const struct Process* process, uint64_t address, uint64_t length) {
    const uint64_t offset = process->chunks[address >> CHUNK_SHIFT];
    for (uint64_t chunk = address >> CHUNK_SHIFT; chunk <= (address + length - 1) >> CHUNK_SHIFT; ++chunk)
        if (process->chunks[chunk] != offset) return 0;
    return 1;
}

// First fit from the top of the mapping area, like Linux's top-down layout.
static uint64_t allocate(struct Process* process, uint64_t length, int in_one_run) {
    const uint64_t pages = length / PAGE, low = process->map_low / PAGE;
    // Continue below the previous allocation, then retry once from the top.
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint64_t run = 0;
        for (uint64_t page = (attempt ? process->map_high : process->map_hint) / PAGE; page-- > low;) {
            run = page_used(process, page) ? 0 : run + 1;
            if (run < pages) continue;
            if (in_one_run && !placeable(process, page * PAGE, length)) continue;  // slide down, out of the mixed chunks
            process->map_hint = page * PAGE;
            return page * PAGE;
        }
    }
    return 0;
}

static void release(struct Process* process, uint64_t address, uint64_t length) {
    if (address & (PAGE - 1) || address + length > SEGMENT || !length) return;
    map_pages(process, address, length, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mark(process, address, length, 0, 0);
#ifdef SPARSE
    for (uint64_t chunk = address >> CHUNK_SHIFT; chunk <= (address + length - 1) >> CHUNK_SHIFT; ++chunk)
        if (chunk_backed(process, chunk) && chunk_empty(process, chunk)) free_chunk(process, chunk);
#endif
}

static int range_free(const struct Process* process, uint64_t address, uint64_t length) {
    if (address < PAGE || address & (PAGE - 1) || address + length > SEGMENT) return 0;
    for (uint64_t page = address / PAGE; page < (address + length) / PAGE; ++page)
        if (page_used(process, page)) return 0;
    return 1;
}

// Maps anonymous memory at a fixed place. Large mappings get host memory when they are touched.
static int map_anonymous(struct Process* process, uint64_t address, uint64_t length, int protection) {
    if (length < LAZY_MINIMUM && !back(process, address, length)) return 0;
    mark(process, address, length, 1, protection);
    return map_pages(process, address, length, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
}

// Moves the pages of [from, from + length) to `to` without copying them. Both ranges are mapped.
static int move_pages(struct Process* process, uint64_t from, uint64_t to, uint64_t length) {
    for (uint64_t done = 0; done < length;) {
        uint64_t part = piece(process, from + done, length - done);
        if (chunk_backed(process, (from + done) >> CHUNK_SHIFT)) {
            vm_address_t destination = (vm_address_t)host(process, to + done);  // backs the target if it was not
            const uint64_t room = piece(process, to + done, length - done);
            if (room < part) part = room;
            vm_prot_t current, maximum;
            if (vm_remap(mach_task_self(), &destination, part, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                         (vm_address_t)host(process, from + done), FALSE, &current, &maximum, VM_INHERIT_COPY) != KERN_SUCCESS)
                return 0;
        }
        done += part;
    }
    return 1;
}

// mremap. Flags: 1 MREMAP_MAYMOVE, 2 MREMAP_FIXED, 4 MREMAP_DONTUNMAP (the old range stays mapped and
// reads as zero afterwards, which ART's compacting collector uses to empty a space).
static long guest_mremap(struct Process* process, uint64_t address, uint64_t old, uint64_t wanted, int flags, uint64_t target) {
    if (address & (PAGE - 1) || address + old > SEGMENT || !wanted) return -22;
    const int protection = page_protection(process, address / PAGE);
    if (!(flags & 6)) {
        if (wanted <= old) {
            if (wanted < old) release(process, address + wanted, old - wanted);
            return (long)address;
        }
        // Grow in place when the pages above are free.
        int room = address + wanted <= process->map_high;
        for (uint64_t page = (address + old) / PAGE; room && page < (address + wanted) / PAGE; ++page) room = !page_used(process, page);
        if (room) return map_anonymous(process, address + old, wanted - old, protection) ? (long)address : -12;
        if (!(flags & 1)) return -12;
    }
    if (flags & 2) {
        if (target & (PAGE - 1) || target + wanted > SEGMENT || (target < address + old && address < target + wanted)) return -22;
    } else if (!(target = allocate(process, wanted, wanted < LAZY_MINIMUM))) {
        return -12;
    }
    const uint64_t moved = old < wanted ? old : wanted;
    if (!map_anonymous(process, target, wanted, protection) || !move_pages(process, address, target, moved)) return -12;
    if (flags & 4) map_pages(process, address, old, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
    else release(process, address, old);
    return (long)target;
}

static long guest_mmap(struct Process* process, uint64_t address, uint64_t length, int protection, int flags, long fd, uint64_t offset) {
    length = page_up(length);
    if (!length) return -22;
    const int fixed = flags & 0x10, no_replace = flags & 0x100000, anonymous = flags & 0x20, shared = flags & 1;
    const int lazy = anonymous && length >= LAZY_MINIMUM;
    protection &= PROT_READ | PROT_WRITE;
    if (fixed || no_replace) {
        if (address & (PAGE - 1) || address + length > SEGMENT) return -22;
        if (no_replace && !fixed && !range_free(process, address, length)) return -17;  // MAP_FIXED_NOREPLACE
    } else if (address && range_free(process, address, length)) {
        // A hint is honoured when the range is free: ART finds memory below 4 GiB this way.
    } else if (!(address = allocate(process, length, !lazy))) {
        return -12;
    }
    if (anonymous) return map_anonymous(process, address, length, protection) ? (long)address : -12;
    if (!back(process, address, length)) return -12;
    mark(process, address, length, 1, protection);
    if (!map_pages(process, address, length, protection, shared ? MAP_SHARED : MAP_PRIVATE, host_fd(process, fd), offset)) return linux_errno(errno);
    return (long)address;
}

static long guest_mprotect(struct Process* process, uint64_t address, uint64_t length, int protection) {
    if (address & (PAGE - 1) || address + length > SEGMENT) return -22;
    // Memory that becomes accessible is about to be used: back it now rather than at the first fault.
    if (protection && length < LAZY_MINIMUM && !back(process, address, length)) return -12;
    record_protection(process, address, length, protection);
    protect_pages(process, address, length, protection);
    return 0;
}

// MADV_DONTNEED must leave private anonymous pages reading as zero.
static void discard(uint8_t* start, uint64_t length) {
    for (uint64_t offset = 0; offset < length;) {
        vm_address_t address = (vm_address_t)(start + offset);
        vm_size_t size = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &address, &size, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS) break;
        if (address >= (vm_address_t)start + length) break;
        uint8_t* from = (uint8_t*)address > start + offset ? (uint8_t*)address : start + offset;
        uint8_t* to = (uint8_t*)address + size < start + length ? (uint8_t*)address + size : start + length;
        if (!info.external_pager && info.protection != VM_PROT_NONE && info.pages_resident)
            mmap(from, (size_t)(to - from), info.protection & (PROT_READ | PROT_WRITE), MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        offset = (uint64_t)(to - start);
    }
}

// Copies an ELF's loadable segments to module->base.
static int map_module(struct Process* process, const struct Module* module) {
    const uint8_t* image = module->image;
    const uint64_t phoff = *(uint64_t*)(image + 32);
    const uint16_t phentsize = *(uint16_t*)(image + 54), phnum = *(uint16_t*)(image + 56);
    for (int index = 0; index < phnum; ++index) {
        const uint8_t* header = image + phoff + (uint64_t)index * phentsize;
        if (*(uint32_t*)header != 1) continue;
        const uint64_t address = module->base + *(uint64_t*)(header + 16);
        const uint64_t low = address & ~(PAGE - 1), high = page_up(address + *(uint64_t*)(header + 40));
        if (!back(process, low, high - low)) return 0;
        // Segments share pages at their edges, so pages already mapped keep their contents.
        for (uint64_t page = low; page < high; page += PAGE)
            if (!page_used(process, page / PAGE)) {
                mark(process, page, PAGE, 1, PROT_READ | PROT_WRITE);
                map_pages(process, page, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            }
        copy_in(process, address, image + *(uint64_t*)(header + 8), *(uint64_t*)(header + 32));
    }
    return 1;
}

// The guest value of a context's stack pointer, and setting it from one.
static uint64_t guest_sp(const struct Context* context) { return context->sp - context->base; }

static void set_guest_sp(struct Process* process, struct Context* context, uint64_t sp) {
    sp &= ADDRESS_MASK;
    context->sp = (uint64_t)host(process, sp);
    context->base = context->sp - sp;
}

// Puts the executable, its interpreter and a fresh stack into the process's address space and points
// its context at the entry, as the kernel does for execve.
static int load_image(struct Thread* thread, const struct Module* executable, char** argv, char** envp) {
    struct Process* process = thread->process;
    const uint8_t* image = executable->image;
    const uint64_t phoff = *(uint64_t*)(image + 32);
    const uint16_t phentsize = *(uint16_t*)(image + 54), phnum = *(uint16_t*)(image + 56);
    const struct Module* interpreter = NULL;
    for (int index = 0; index < phnum; ++index) {
        const uint8_t* header = image + phoff + (uint64_t)index * phentsize;
        if (*(uint32_t*)header != 3) continue;  // PT_INTERP
        char path[MAXPATHLEN];
        snprintf(path, sizeof path, "%s%s", root ? root : "", (const char*)image + *(uint64_t*)(header + 8));
        if (!(interpreter = module_for_file(path))) {
            fprintf(stderr, "[runtime] interpreter %s is not a translated module\n", path);
            return 0;
        }
    }
    if (!reset_space(process) || !map_module(process, executable) || (interpreter && !map_module(process, interpreter))) return 0;
    process->executable = executable;

    uint64_t prelinked = 0;
    for (int index = 0; index < guest_module_count; ++index)
        if (guest_modules[index].end > prelinked) prelinked = guest_modules[index].end;
    process->brk_start = process->brk = executable->guest_path ? ROOT_BRK_START : executable->end;
    process->map_low = process->brk_start + BRK_LIMIT;
    if (prelinked + PAGE > process->map_low) process->map_low = prelinked + PAGE;
    const uint64_t stack_top = SEGMENT - PAGE;
    process->map_high = process->map_hint = stack_top - STACK_SIZE - PAGE;
    if (!map_anonymous(process, stack_top - STACK_SIZE, STACK_SIZE, PROT_READ | PROT_WRITE)) return 0;
    uint8_t* base = (uint8_t*)host(process, stack_top - STACK_SIZE) - (stack_top - STACK_SIZE);  // the stack is in one run

    int argc = 0, envc = 0;
    while (argv[argc]) ++argc;
    while (envp[envc]) ++envc;
    process->command_length = 0;
    for (int index = 0; index < argc; ++index) {
        const int length = (int)strlen(argv[index]) + 1;
        if (process->command_length + length > (int)sizeof process->command_line) break;
        memcpy(process->command_line + process->command_length, argv[index], length);
        process->command_length += length;
    }
    uint64_t cursor = stack_top;
    uint64_t pointers[argc + envc + 2];
    for (int index = 0; index < argc + envc; ++index) {
        const char* text = index < argc ? argv[index] : envp[index - argc];
        cursor -= strlen(text) + 1;
        strcpy((char*)base + cursor, text);
        pointers[index] = cursor;
    }
    cursor -= 16;
    arc4random_buf(base + cursor, 16);
    const uint64_t random = cursor;
    const uint64_t auxv[] = {3, executable->headers, 4, phentsize, 5, phnum, 6, PAGE, 7, interpreter ? interpreter->base : 0,
                             9, executable->info[2], 11, 0, 12, 0, 13, 0, 14, 0, 16, 0, 23, 0, 25, random, 31, pointers[0], 0, 0};
    const uint64_t words = 1 + argc + 1 + envc + 1 + sizeof auxv / 8;
    cursor = (cursor - words * 8) & ~15ull;
    uint64_t* stack = (uint64_t*)(base + cursor);
    *stack++ = (uint64_t)argc;
    for (int index = 0; index < argc; ++index) *stack++ = pointers[index];
    *stack++ = 0;
    for (int index = 0; index < envc; ++index) *stack++ = pointers[argc + index];
    *stack++ = 0;
    memcpy(stack, auxv, sizeof auxv);

    struct Context* context = &thread->context;
    const uint64_t host_sp = context->host_sp;
    memset(context, 0, sizeof *context);
    context->host_sp = host_sp;
    context->process = process;
    context->thread = thread;
    context->memory_table = (uint64_t)process->chunks;
    context->table = (uint64_t)code_table - 2 * code_low;
    set_guest_sp(process, context, cursor);
    context->pc = host_code(interpreter ? interpreter->info[2] : executable->info[2]);
    return 1;
}

// ---------------------------------------------------------------- processes

static struct Thread* new_thread(struct Process* process, int tid) {
    struct Thread* thread = calloc(1, sizeof *thread);
    thread->tid = tid;
    thread->process = process;
    for (int index = 0; index < MAX_THREADS; ++index)
        if (!process->threads[index]) {
            process->threads[index] = thread;
            ++process->live_threads;
            return thread;
        }
    free(thread);
    return NULL;
}

// A new process with its first thread. The caller fills in memory and descriptors.
static struct Thread* new_process(int parent) {
    struct Process* process = calloc(1, sizeof *process);
    for (int fd = 0; fd < MAX_FDS; ++fd) process->fds[fd] = -1;
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&process->memory_lock, &attributes);
    pthread_mutex_lock(&lock);
    process->pid = next_pid++;
    process->parent = parent;
    processes[process->pid % MAX_PROCESSES] = process;
    struct Thread* thread = new_thread(process, process->pid);
    pthread_mutex_unlock(&lock);
    return thread;
}

static void futex_wake(uint8_t* address, int count);

// Ends the calling thread only. The thread's memory stays until the process is reaped.
static void finish_thread(struct Thread* thread) __attribute__((noreturn));
static void finish_thread(struct Thread* thread) {
    struct Process* process = thread->process;
    if (thread->clear_tid && !process->exiting) {
        *(int*)host(process, thread->clear_tid) = 0;
        futex_wake(host(process, thread->clear_tid), 1);
    }
    pthread_mutex_lock(&lock);
    thread->dead = 1;
    --process->live_threads;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    pthread_exit(NULL);
}

static void on_thread_stop(int number) {
    (void)number;
    pthread_exit(NULL);
}

// Ends the whole process from one of its threads.
static void terminate(struct Thread* thread, int status) __attribute__((noreturn));
static void terminate(struct Thread* thread, int status) {
    struct Process* process = thread->process;
    pthread_mutex_lock(&lock);
    if (process->exiting) {  // another thread got here first
        pthread_mutex_unlock(&lock);
        finish_thread(thread);
    }
    process->exiting = 1;
    // Stop the other threads: they are host threads running guest code or blocked in the host.
    for (int index = 0; index < MAX_THREADS; ++index) {
        struct Thread* other = process->threads[index];
        if (other && other != thread && !other->dead) {
            other->dead = 1;
            --process->live_threads;
            pthread_kill(other->host, SIGUSR1);
        }
    }
    pthread_mutex_unlock(&lock);
    for (int fd = 0; fd < MAX_FDS; ++fd) close_fd(process, fd);
    free_space(process);  // without the lock: a thread stopped above may have held it
    pthread_mutex_lock(&lock);
    struct Process* parent = processes[process->parent % MAX_PROCESSES];
    if (parent && parent->pid == process->parent) parent->pending |= SIGBIT(17);  // SIGCHLD
    process->exited = 1;
    process->status = status;
    process->vfork_done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    pthread_exit(NULL);
}

static void* run_thread(void* raw) {
    // Host fault handlers need their own stack: the guest's may be the thing that overflowed.
    stack_t stack = {malloc(64 * 1024), 64 * 1024, 0};
    sigaltstack(&stack, NULL);
    guest_enter(&((struct Thread*)raw)->context);
}

static void start_thread(struct Thread* thread) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1 << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread->host, &attributes, run_thread, thread);
}

static long guest_fork(struct Thread* caller, uint64_t flags, uint64_t child_stack, uint64_t child_tid) {
    const uint64_t begin = now_ns();
    struct Process* parent = caller->process;
    struct Thread* thread = new_process(parent->pid);
    struct Process* child = thread->process;
    // The child's memory is a copy-on-write copy of each run, and of the tables that describe it.
    pthread_mutex_lock(&parent->memory_lock);
    vm_address_t address = 0;
    vm_prot_t current, maximum;
    int copied = vm_remap(mach_task_self(), &address, space_size(), 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)parent->space, TRUE,
                          &current, &maximum, VM_INHERIT_COPY) == KERN_SUCCESS;
    if (copied) attach_space(child, (void*)address);
    for (int index = 0; copied && index < parent->run_count; ++index) {
        const struct Run* run = &parent->runs[index];
        uint8_t* memory = reserve(NULL, GUARD_BEFORE + run->length + GUARD_AFTER);
        address = (vm_address_t)(memory + GUARD_BEFORE);
        copied = memory && vm_remap(mach_task_self(), &address, run->length, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                    (vm_address_t)run->host, TRUE, &current, &maximum, VM_INHERIT_COPY) == KERN_SUCCESS;
        if (!memory) break;
        *add_run(child) = (struct Run){run->guest, run->length, memory + GUARD_BEFORE};
        for (uint64_t chunk = run->guest >> CHUNK_SHIFT; chunk < (run->guest + run->length) >> CHUNK_SHIFT; ++chunk)
            child->chunks[chunk] = (uint64_t)memory + GUARD_BEFORE - run->guest;
    }
    pthread_mutex_unlock(&parent->memory_lock);
    if (!copied) {  // out of address space: the child never runs
        free_space(child);
        pthread_mutex_lock(&lock);
        processes[child->pid % MAX_PROCESSES] = NULL;
        pthread_mutex_unlock(&lock);
        return -12;
    }
    child->executable = parent->executable;
    child->brk_start = parent->brk_start;
    child->brk = parent->brk;
    child->map_low = parent->map_low;
    child->map_high = parent->map_high;
    child->map_hint = parent->map_hint;
    memcpy(child->actions, parent->actions, sizeof child->actions);
    memcpy(child->command_line, parent->command_line, sizeof child->command_line);
    child->command_length = parent->command_length;
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if (parent->fds[fd] >= 0) {
            child->fds[fd] = dup(parent->fds[fd]);
            child->cloexec[fd] = parent->cloexec[fd];
        }
    // Registers hold guest addresses, so only the base and the stack pointer change.
    thread->blocked = caller->blocked;
    thread->clear_tid = flags & 0x00200000 ? child_tid : 0;
    thread->context = caller->context;
    thread->context.process = child;
    thread->context.thread = thread;
    thread->context.memory_table = (uint64_t)child->chunks;
    set_guest_sp(child, &thread->context, child_stack ? child_stack : guest_sp(&caller->context));
    thread->context.x[0] = 0;
    if (flags & 0x01000000 && child_tid) *(int*)host(child, child_tid) = child->pid;  // CLONE_CHILD_SETTID
    __atomic_add_fetch(&fork_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&fork_ns, now_ns() - begin, __ATOMIC_RELAXED);
    const int pid = child->pid;
    start_thread(thread);
    if (flags & 0x4000) {  // CLONE_VFORK: the parent waits until the child execs or exits
        pthread_mutex_lock(&lock);
        while (!child->vfork_done) pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
    }
    return pid;
}

// clone with CLONE_VM | CLONE_THREAD: a new thread in the same address space.
static long guest_clone_thread(struct Thread* caller, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls, uint64_t child_tid) {
    struct Process* process = caller->process;
    pthread_mutex_lock(&lock);
    struct Thread* thread = new_thread(process, next_pid++);
    pthread_mutex_unlock(&lock);
    if (!thread) return -11;
    thread->blocked = caller->blocked;
    thread->clear_tid = flags & 0x00200000 ? child_tid : 0;
    thread->context = caller->context;
    thread->context.thread = thread;
    set_guest_sp(process, &thread->context, stack);
    thread->context.x[0] = 0;
    if (flags & 0x00080000) thread->context.tpidr = tls;  // CLONE_SETTLS
    if (flags & 0x00100000 && parent_tid) *(int*)host(process, parent_tid) = thread->tid;  // CLONE_PARENT_SETTID
    if (flags & 0x01000000 && child_tid) *(int*)host(process, child_tid) = thread->tid;
    const int tid = thread->tid;
    start_thread(thread);
    return tid;
}

static long guest_wait(struct Process* process, long pid, uint64_t status_address, long options) {
    pthread_mutex_lock(&lock);
    for (;;) {
        int children = 0;
        for (int index = 0; index < MAX_PROCESSES; ++index) {
            struct Process* child = processes[index];
            if (!child || child->parent != process->pid || child->reaped || (pid > 0 && child->pid != pid)) continue;
            ++children;
            if (!child->exited) continue;
            child->reaped = 1;
            processes[index] = NULL;
            free_tables(child);
            const int result = child->pid;
            if (status_address) *(int*)host(process, status_address) = child->status;
            pthread_mutex_unlock(&lock);
            return result;  // the process record is small and stays: late thread signals may still look at it
        }
        if (!children) { pthread_mutex_unlock(&lock); return -10; }
        if (options & 1) { pthread_mutex_unlock(&lock); return 0; }
        pthread_cond_wait(&changed, &lock);
    }
}

static char** copy_strings(struct Process* process, uint64_t address) {
    int count = 0;
    const uint64_t* list = host(process, address);
    while (list && list[count]) ++count;
    char** copy = calloc((size_t)count + 1, sizeof *copy);
    for (int index = 0; index < count; ++index) copy[index] = strdup(host(process, list[index]));
    return copy;
}

static long guest_exec(struct Thread* thread, const char* path, uint64_t argv, uint64_t envp) {
    struct Process* process = thread->process;
    struct stat info;
    if (stat(path, &info) != 0) return linux_errno(errno);
    const struct Module* module = module_for_stat(&info);
    if (!module) return -8;
    char** arguments = copy_strings(process, argv);
    char** environment = copy_strings(process, envp);
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if (process->cloexec[fd]) close_fd(process, fd);
    if (!load_image(thread, module, arguments, environment)) terminate(thread, 127 << 8);
    pthread_mutex_lock(&lock);
    process->vfork_done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

// ---------------------------------------------------------------- files

// Translates a guest path to a host path. Two results can be alive at once (rename).
static const char* host_path_in(struct Process* process, const char* path, int slot) {
    static _Thread_local char buffers[2][MAXPATHLEN];
    if (!path) return NULL;
    if (strcmp(path, "/proc/self/exe") == 0) return process->executable->host_path;
    if (!root || path[0] != '/') return path;
    if (strncmp(path, "/dev/", 5) == 0 && (!strcmp(path + 5, "null") || !strcmp(path + 5, "zero") || !strcmp(path + 5, "urandom") ||
                                           !strcmp(path + 5, "random") || !strcmp(path + 5, "tty")))
        return path;
    snprintf(buffers[slot], MAXPATHLEN, "%s%s", root, path);
    return buffers[slot];
}
#define HP(address) host_path_in(p, host(p, address), 0)
#define HP2(address) host_path_in(p, host(p, address), 1)

// The guest's name for a host path: the part below the root.
static const char* guest_path(const char* path) {
    if (root && strncmp(path, root, root_length) == 0) return path[root_length] ? path + root_length : "/";
    return path;
}

// /proc files the guest reads about itself, generated into an unlinked temporary file.
// Returns a host descriptor, or -1 when the path is not one of them.
static int open_proc_file(struct Process* p, const char* path) {
    if (strncmp(path, "/proc/self/", 11) != 0) return -1;
    const char* name = path + 11;
    const int is_maps = !strcmp(name, "maps"), is_stat = !strcmp(name, "stat"), is_command = !strcmp(name, "cmdline");
    if (!is_maps && !is_stat && !is_command) return -1;
    char file[1024];  // an iOS app can only write below its own temporary directory
    snprintf(file, sizeof file, "%s/nestvane-proc-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    const int descriptor = mkstemp(file);
    if (descriptor < 0) return -1;
    unlink(file);
    FILE* out = fdopen(dup(descriptor), "w");
    const uint64_t stack_top = SEGMENT - PAGE;
    if (is_command) {
        fwrite(p->command_line, 1, p->command_length, out);
    } else if (is_stat) {
        // Field 28 is the start of the stack. Bionic looks it up in maps to size the main thread's stack.
        fprintf(out, "%d (guest) R %d %d %d 0 -1 0 0 0 0 0 0 0 0 0 20 0 %d 0 0 0 0 -1 0 0 %llu", p->pid, p->parent, p->pid, p->pid,
                p->live_threads, stack_top - 256);
        for (int field = 29; field <= 52; ++field) fprintf(out, " 0");
        fprintf(out, "\n");
    } else {
        // Mapped ranges with one protection each. Chunks that hold nothing are skipped whole.
        for (uint64_t page = 1; page < SEGMENT / PAGE;) {
            if (page % (CHUNK / PAGE) == 0 && chunk_empty(p, page / (CHUNK / PAGE))) { page += CHUNK / PAGE; continue; }
            if (!page_used(p, page)) { ++page; continue; }
            const int protection = page_protection(p, page);
            uint64_t stop = page + 1;
            while (stop < SEGMENT / PAGE && page_used(p, stop) && page_protection(p, stop) == protection) ++stop;
            fprintf(out, "%llx-%llx %c%c-p 00000000 00:00 0%s\n", page * PAGE, stop * PAGE, protection & PROT_READ ? 'r' : '-',
                    protection & PROT_WRITE ? 'w' : '-', stop * PAGE == stack_top ? "                          [stack]" : "");
            page = stop;
        }
    }
    fclose(out);
    lseek(descriptor, 0, SEEK_SET);
    return descriptor;
}

static int host_directory(struct Process* process, long fd) { return (int)fd == -100 ? AT_FDCWD : host_fd(process, fd); }

static int open_flags(long flags) {
    int result = (int)(flags & 3);
    if (flags & 0100) result |= O_CREAT;
    if (flags & 0200) result |= O_EXCL;
    if (flags & 0400) result |= O_NOCTTY;
    if (flags & 01000) result |= O_TRUNC;
    if (flags & 02000) result |= O_APPEND;
    if (flags & 04000) result |= O_NONBLOCK;
    if (flags & 040000) result |= O_DIRECTORY;
    if (flags & 0100000) result |= O_NOFOLLOW;
    return result | O_CLOEXEC;
}

struct LinuxStat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    uint32_t unused[2];
};

static long put_stat(int result, const struct stat* from, struct LinuxStat* to) {
    if (result != 0) return linux_errno(errno);
    memset(to, 0, sizeof *to);
    to->dev = (uint64_t)from->st_dev;
    to->ino = from->st_ino;
    to->mode = from->st_mode;
    to->nlink = from->st_nlink;
    to->uid = from->st_uid;
    to->gid = from->st_gid;
    to->rdev = (uint64_t)from->st_rdev;
    to->size = from->st_size;
    to->blksize = from->st_blksize;
    to->blocks = from->st_blocks;
    to->atime = from->st_atimespec.tv_sec;
    to->atime_ns = from->st_atimespec.tv_nsec;
    to->mtime = from->st_mtimespec.tv_sec;
    to->mtime_ns = from->st_mtimespec.tv_nsec;
    to->ctime = from->st_ctimespec.tv_sec;
    to->ctime_ns = from->st_ctimespec.tv_nsec;
    return 0;
}

static long guest_getdents(struct Process* process, long fd, uint8_t* buffer, uint64_t size) {
    const int descriptor = host_fd(process, fd);
    if (descriptor < 0) return -9;
    if (!process->directories[fd] && !(process->directories[fd] = fdopendir(dup(descriptor)))) return linux_errno(errno);
    DIR* directory = process->directories[fd];
    uint64_t used = 0;
    for (;;) {
        const long position = telldir(directory);
        struct dirent* entry = readdir(directory);
        if (!entry) break;
        const uint64_t length = (19 + strlen(entry->d_name) + 1 + 7) & ~7ull;
        if (used + length > size) { seekdir(directory, position); break; }
        uint8_t* record = buffer + used;
        *(uint64_t*)record = entry->d_ino;
        *(int64_t*)(record + 8) = telldir(directory);
        *(uint16_t*)(record + 16) = (uint16_t)length;
        record[18] = entry->d_type;
        strcpy((char*)record + 19, entry->d_name);
        used += length;
    }
    return (long)used;
}

static long vector_io(struct Process* process, long fd, uint64_t vector, long count, int writing) {
    struct iovec vectors[128];
    if (count < 0 || count > 64) return -22;
    const uint64_t* guest = host(process, vector);
    int parts = 0;
    for (long index = 0; index < count; ++index) parts += buffer_parts(process, guest[index * 2], guest[index * 2 + 1], vectors + parts, 128 - parts);
    return writing ? writev(host_fd(process, fd), vectors, parts) : readv(host_fd(process, fd), vectors, parts);
}

// read, write, pread and pwrite on a guest buffer, which may be in several pieces of host memory.
static long buffer_io(struct Process* process, long fd, uint64_t address, uint64_t length, int writing, int positioned, off_t offset) {
    struct iovec parts[8];
    const int descriptor = host_fd(process, fd), count = buffer_parts(process, address, length, parts, 8);
    if (count <= 1) {
        void* buffer = count ? parts[0].iov_base : NULL;
        if (positioned) return writing ? pwrite(descriptor, buffer, length, offset) : pread(descriptor, buffer, length, offset);
        return writing ? write(descriptor, buffer, length) : read(descriptor, buffer, length);
    }
    if (positioned) return writing ? pwritev(descriptor, parts, count, offset) : preadv(descriptor, parts, count, offset);
    return writing ? writev(descriptor, parts, count) : readv(descriptor, parts, count);
}

// A write to a pipe with no reader kills a Linux process unless it handles SIGPIPE.
static long written(struct Thread* thread, long result) {
    if (result < 0 && errno == EPIPE && thread->process->actions[13].handler == 0) terminate(thread, 13);
    return RESULT(result);
}

static long guest_readlink(struct Process* p, long directory, const char* path, char* buffer, uint64_t size) {
    char target[MAXPATHLEN];
    const char* text = NULL;
    if (strcmp(path, "/proc/self/exe") == 0) {
        text = p->executable->guest_path ? p->executable->guest_path : p->executable->host_path;
    } else if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        // The dynamic linker asks for the real path of each library it has opened.
        if (fcntl(host_fd(p, atol(path + 14)), F_GETPATH, target) != 0) return -2;
        text = guest_path(target);
    }
    if (!text) return RESULT(readlinkat(host_directory(p, directory), host_path_in(p, path, 0), buffer, size));
    uint64_t length = strlen(text);
    if (length > size) length = size;
    memcpy(buffer, text, length);
    return (long)length;
}

// ---------------------------------------------------------------- signals

// The frame Linux arm64 puts on the stack for a handler: siginfo, then ucontext with the interrupted
// registers. Handlers read and change it (ART redirects pc from it), and rt_sigreturn loads it back.
enum {
    FRAME_UCONTEXT = 128,
    FRAME_STACK = FRAME_UCONTEXT + 16,
    FRAME_MASK = FRAME_UCONTEXT + 40,
    FRAME_MACHINE = FRAME_UCONTEXT + 176,  // fault address, x0-x30, sp, pc, pstate
    FRAME_VECTORS = FRAME_MACHINE + 288,   // fpsimd_context: magic, size, fpsr, fpcr, v0-v31
    FRAME_SIZE = FRAME_VECTORS + 4096,
};

// Points the context at the guest handler for `number`, with the interrupted state in a frame.
// context->pc must be the host address of a guest instruction boundary.
static void enter_handler(struct Thread* t, struct Context* context, int number, int code, uint64_t address, uint64_t restore_mask) {
    struct Process* p = t->process;
    uint64_t sp = guest_sp(context);
    const int on_alternate = t->alternate_size && sp > t->alternate_stack && sp <= t->alternate_stack + t->alternate_size;
    if (p->actions[number].flags & 0x08000000 && t->alternate_size && !on_alternate) sp = t->alternate_stack + t->alternate_size;  // SA_ONSTACK
    sp = (sp - FRAME_SIZE) & ~15ull;
    const uint64_t interrupted_sp = guest_sp(context);
    set_guest_sp(p, context, sp);  // a signal stack is one mapping, so the frame is contiguous
    uint8_t* frame = (uint8_t*)context->sp;
    memset(frame, 0, FRAME_VECTORS + 544);
    if (p->has_queued & SIGBIT(number)) {  // the sender supplied the siginfo
        memcpy(frame, p->queued[number], 128);
        p->has_queued &= ~SIGBIT(number);
    } else {
        *(int*)frame = number;
        *(int*)(frame + 8) = code;
        *(uint64_t*)(frame + 16) = address;
    }
    *(uint64_t*)(frame + FRAME_STACK) = t->alternate_stack;
    *(int*)(frame + FRAME_STACK + 8) = t->alternate_size ? on_alternate : 2;  // SS_ONSTACK or SS_DISABLE
    *(uint64_t*)(frame + FRAME_STACK + 16) = t->alternate_size;
    *(uint64_t*)(frame + FRAME_MASK) = restore_mask;
    uint64_t* machine = (uint64_t*)(frame + FRAME_MACHINE);
    machine[0] = address;
    memcpy(machine + 1, context->x, sizeof context->x);
    machine[32] = interrupted_sp;
    machine[33] = guest_code(context->pc);
    machine[34] = context->nzcv & 0xf0000000;
    uint32_t* vectors = (uint32_t*)(frame + FRAME_VECTORS);
    vectors[0] = 0x46508001;
    vectors[1] = 528;
    memcpy(vectors + 4, context->q, sizeof context->q);

    t->blocked |= p->actions[number].mask | (p->actions[number].flags & 0x40000000 ? 0 : SIGBIT(number));  // SA_NODEFER
    context->x[0] = (uint64_t)number;
    context->x[1] = sp;
    context->x[2] = sp + FRAME_UCONTEXT;
    context->x[30] = code_high;
    context->pc = host_code(p->actions[number].handler & ADDRESS_MASK);
}

// rt_sigreturn: the handler has returned with sp at the frame.
static void leave_handler(struct Thread* t, struct Context* context) {
    const uint8_t* frame = (const uint8_t*)context->sp;
    const uint64_t* machine = (const uint64_t*)(frame + FRAME_MACHINE);
    if (machine[33] < code_low || machine[33] >= code_high) terminate(t, 11);
    t->blocked = *(const uint64_t*)(frame + FRAME_MASK) & ~(SIGBIT(9) | SIGBIT(19));
    memcpy(context->x, machine + 1, sizeof context->x);
    set_guest_sp(t->process, context, machine[32]);
    context->pc = host_code(machine[33]);
    context->nzcv = machine[34];
    if (trace > 1) fprintf(stderr, "[runtime] pid %d: sigreturn to guest pc 0x%llx, x30 0x%llx\n", t->process->pid, machine[33], context->x[30]);
    memcpy(context->q, frame + FRAME_VECTORS + 16, sizeof context->q);
}

// Runs the handler of one pending, unblocked signal on top of the current context.
// `restore_mask` is the mask to return to afterwards.
static void deliver_signals(struct Thread* t, struct Context* context, uint64_t restore_mask) {
    struct Process* p = t->process;
    for (;;) {
        pthread_mutex_lock(&lock);
        const uint64_t ready = (p->pending | t->pending) & ~t->blocked;
        const int number = ready ? __builtin_ctzll(ready) + 1 : 0;
        if (number && t->pending & SIGBIT(number)) t->pending &= ~SIGBIT(number);
        else if (number) p->pending &= ~SIGBIT(number);
        pthread_mutex_unlock(&lock);
        if (!number) return;
        const uint64_t handler = p->actions[number].handler;
        if (handler == 1) continue;  // SIG_IGN
        if (handler == 0) {
            if (number == 17 || number == 18 || number == 23 || number == 28) continue;  // ignored by default
            terminate(t, number);
        }
        enter_handler(t, context, number, 0, 0, restore_mask);
        return;
    }
}

// The guest address that a host address stands for: in the void, or in a run or its guards.
static int guest_address(struct Process* p, uint64_t address, uint64_t* guest) {
    if (address - VOID_OFFSET < SEGMENT + CHUNK) {
        *guest = address - VOID_OFFSET;
        return 1;
    }
    const struct Run* run = run_of_host(p, address);
    if (!run) return 0;
    *guest = address - ((uint64_t)run->host - run->guest);
    return 1;
}

// Carries out the load or store at the faulting host instruction on guest memory, piece by piece.
// It is for an access that left the run of its base register, which the instruction itself cannot
// do. Returns 1 when done, 0 when the instruction is not one it knows, and -1 when the guest may
// not make this access, with the fault filled in.
static int emulate_access(struct Thread* t, struct Context* context) {
    struct Process* p = t->process;
    const uint32_t w = *(const uint32_t*)t->fault.host_pc;
    const unsigned rn = w >> 5 & 31;
    unsigned vector = w >> 26 & 1, registers[4] = {w & 31, 0, 0, 0};
    int count = 1, load, extend = 0, wide = 1, size_log = (int)(w >> 30);
    int64_t offset = 0, write_back = 0;
    if ((w & 0x3A000000) == 0x28000000) {  // pair
        const unsigned opc = w >> 30, mode = w >> 23 & 3;
        size_log = vector ? 2 + (int)opc : opc & 2 ? 3 : 2;
        const int64_t immediate = ((int64_t)(w >> 15 & 0x7f) << 57 >> 57) * (1ll << size_log);
        load = w >> 22 & 1;
        extend = !vector && opc == 1;  // ldpsw
        registers[1] = w >> 10 & 31;
        count = 2;
        offset = mode == 1 ? 0 : immediate;
        write_back = mode & 1 ? immediate : 0;
    } else if ((w & 0x3B000000) == 0x39000000 || (w & 0x3B200000) == 0x38000000 || (w & 0x3B200C00) == 0x38200800) {
        const unsigned opc = w >> 22 & 3;
        if (vector) {
            if (opc & 2) size_log = 4;
            load = opc & 1;
        } else {
            if (size_log == 3 && opc == 2) return 1;  // a prefetch does nothing
            load = opc != 0;
            extend = opc >= 2;
            wide = opc != 3;  // opc 3 sign-extends to 32 bits
        }
        if ((w & 0x3B000000) == 0x39000000) {
            offset = (int64_t)(w >> 10 & 0xfff) << size_log;
        } else if (w >> 21 & 1) {  // register offset
            const unsigned rm = w >> 16 & 31, option = w >> 13 & 7;
            const uint64_t index = rm == 31 ? 0 : context->x[rm];
            const int64_t extended = option == 2 ? (int64_t)(uint32_t)index : option == 6 ? (int64_t)(int32_t)index : (int64_t)index;
            offset = extended * (1ll << (w >> 12 & 1 ? size_log : 0));
        } else {
            const int64_t immediate = (int64_t)(w >> 12 & 0x1ff) << 55 >> 55;
            const unsigned mode = w >> 10 & 3;
            offset = mode == 1 ? 0 : immediate;
            write_back = mode & 1 ? immediate : 0;
        }
    } else if ((w & 0xBFBF0000) == 0x0C000000) {  // ld1 and st1 of one to four whole vector registers
        const unsigned opcode = w >> 12 & 15;
        count = opcode == 7 ? 1 : opcode == 10 ? 2 : opcode == 6 ? 3 : opcode == 2 ? 4 : 0;
        if (!count) return 0;  // ld2 to ld4 interleave their elements
        for (int index = 1; index < count; ++index) registers[index] = (registers[0] + (unsigned)index) & 31;
        vector = 1;
        size_log = w >> 30 & 1 ? 4 : 3;
        load = w >> 22 & 1;
    } else {
        return 0;
    }
    const uint64_t size = 1ull << size_log, total = size * (uint64_t)count;
    uint64_t address;
    if (!guest_address(p, ((rn == 31 ? context->sp : context->x[rn]) + (uint64_t)offset) & ADDRESS_MASK, &address)) return 0;
    for (uint64_t page = address / PAGE; page <= (address + total - 1) / PAGE; ++page)
        if (page >= SEGMENT / PAGE || !page_used(p, page) || !(page_protection(p, page) & (load ? PROT_READ : PROT_WRITE))) {
            t->fault.address = page * PAGE > address ? page * PAGE : address;
            t->fault.code = page < SEGMENT / PAGE && page_used(p, page) ? 2 : 1;
            return -1;
        }
    uint8_t bytes[64] = {0};
    if (load) {
        copy_out(p, bytes, address, total);
        for (int index = 0; index < count; ++index) {
            if (vector) {
                context->q[registers[index]] = 0;
                memcpy(&context->q[registers[index]], bytes + (uint64_t)index * size, size);
            } else if (registers[index] != 31) {
                uint64_t value = 0;
                memcpy(&value, bytes + (uint64_t)index * size, size);
                if (extend && size < 8) value = (uint64_t)((int64_t)(value << (64 - size * 8)) >> (64 - size * 8));
                context->x[registers[index]] = wide ? value : (uint32_t)value;
            }
        }
    } else {
        for (int index = 0; index < count; ++index) {
            if (vector) memcpy(bytes + (uint64_t)index * size, &context->q[registers[index]], size);
            else memcpy(bytes + (uint64_t)index * size, registers[index] == 31 ? (const uint64_t[]){0} : &context->x[registers[index]], size);
        }
        copy_in(p, address, bytes, total);
    }
    if (write_back && rn == 31) context->sp += (uint64_t)write_back;
    else if (write_back) context->x[rn] += (uint64_t)write_back;
    __atomic_add_fetch(&emulated_count, 1, __ATOMIC_RELAXED);
    return 1;
}

// A host memory fault in translated code. The host handler only records the state and redirects the
// thread here, on its own stack, where it may block or exit. The fault is one of:
//   - the first touch of memory that has no host memory yet: back it and run the instruction again;
//   - an access that left the run of its base register: carry it out here and continue after it;
//   - the guest's own fault, which becomes its SIGSEGV.
static void continue_fault(struct Context* context) __attribute__((noreturn));
static void continue_fault(struct Context* context) {
    struct Thread* t = context->thread;
    struct Process* p = t->process;
    if (p->exiting) finish_thread(t);  // the fault is from the process's memory going away
    uint64_t address = t->fault.host_address;
    t->fault.address = address;
    pthread_mutex_lock(&p->memory_lock);  // other threads change the runs
    const int known = guest_address(p, t->fault.host_address, &address);
    const struct Run* run = run_of_host(p, t->fault.host_address);
    const int in_void = known && !run, in_run = run && t->fault.host_address - (uint64_t)run->host < run->length;
    const int mapped = known && address < SEGMENT && page_used(p, address / PAGE);
    const int untouched = mapped && in_void && !chunk_backed(p, address >> CHUNK_SHIFT);
    if (untouched) materialize(p, address >> CHUNK_SHIFT);
    if (known) t->fault.address = address;
    if (mapped) t->fault.code = 2;  // SEGV_ACCERR, unless one of the cases below applies
    const int done = mapped && !untouched && !in_run ? emulate_access(t, context) : 0;
    pthread_mutex_unlock(&p->memory_lock);
    if (known) {
        if (mapped) {
            if (untouched) guest_resume(context);
            if (!in_run) {
                if (done > 0) {
                    // Resuming loads the scratch register with the target, so the rest of this guest
                    // instruction, which can only be its write-back, is done here, and the next one is
                    // entered the way a jump would enter it.
                    const uint64_t guest = guest_code(t->fault.host_pc), next = inline_code(guest + 4);
                    uint64_t pc = t->fault.host_pc + 4;
                    for (; next && pc < next; pc += 4) {
                        const uint32_t w = *(const uint32_t*)pc;
                        const unsigned rd = w & 31, rn = w >> 5 & 31;
                        if (rd == 31 || rn == 31) break;
                        if ((w & 0xFFC00000) == 0x91000000) context->x[rd] = context->x[rn] + (w >> 10 & 0xfff);
                        else if ((w & 0xFFC00000) == 0xD1000000) context->x[rd] = context->x[rn] - (w >> 10 & 0xfff);
                        else if ((w & 0xFFE0FC00) == 0x8B000000) context->x[rd] = context->x[rn] + ((w >> 16 & 31) == 31 ? 0 : context->x[w >> 16 & 31]);
                        else break;
                    }
                    context->pc = next && pc == next ? host_code(guest + 4) : t->fault.host_pc + 4;
                    guest_resume(context);
                }
                // Not an instruction emulate_access knows. If another thread backed the chunk between
                // the fault and now, running the instruction again succeeds. Give that a few tries.
                const int repeated = t->fault.repeat_pc == t->fault.host_pc && t->fault.repeat_address == t->fault.host_address;
                t->fault.repeats = repeated ? t->fault.repeats + 1 : 0;
                t->fault.repeat_pc = t->fault.host_pc;
                t->fault.repeat_address = t->fault.host_address;
                if (done == 0 && t->fault.repeats < 3) guest_resume(context);
                if (done == 0)
                    fprintf(stderr, "[runtime] pid %d: cannot emulate instruction %08x across chunks at guest address 0x%llx\n", p->pid,
                            *(const uint32_t*)t->fault.host_pc, address);
            }
        }
    }
    if (p->actions[11].handler <= 1 || t->blocked & SIGBIT(11)) {
        fprintf(stderr, "[runtime] pid %d: segmentation fault at guest address 0x%llx, pc 0x%llx\n", p->pid, t->fault.address,
                guest_code(context->pc));
        terminate(t, 11);
    }
    if (trace > 1) fprintf(stderr, "[runtime] pid %d: fault at 0x%llx, guest pc 0x%llx, x30 0x%llx\n", p->pid, t->fault.address,
                           guest_code(context->pc), context->x[30]);
    enter_handler(t, context, 11, t->fault.code, t->fault.address, t->blocked);
    guest_resume(context);
}

static void on_host_fault(int number, siginfo_t* info, void* raw) {
    ucontext_t* host_context = raw;
    struct __darwin_arm_thread_state64* state = &host_context->uc_mcontext->__ss;
    const uint64_t guest_pc = guest_code(state->__pc);
    if (!guest_pc) {  // a fault in the runtime itself: let it crash where it happened
        signal(number, SIG_DFL);
        return;
    }
    struct Context* context = (struct Context*)state->__x[25];
    struct Thread* t = context->thread;
    memcpy(context->x, state->__x, 29 * 8);
    context->x[29] = state->__fp;
    context->x[30] = state->__lr;
    context->sp = state->__sp;
    context->nzcv = state->__cpsr;
    memcpy(context->q, host_context->uc_mcontext->__ns.__v, sizeof context->q);
    context->pc = host_code(guest_pc);  // the access has had no effect yet: report the whole instruction
    t->fault.host_address = (uint64_t)info->si_addr & ADDRESS_MASK;
    t->fault.host_pc = state->__pc;
    t->fault.code = 1;  // SEGV_MAPERR until continue_fault finds the page mapped
    state->__x[0] = (uint64_t)context;
    state->__sp = context->host_sp;
    state->__lr = 0;
    state->__pc = (uint64_t)&continue_fault;
}

// ---------------------------------------------------------------- logd

// Android logs through a datagram socket to logd. The guest gets one end of a socket pair and this
// thread prints what arrives, so that the guest's own diagnostics are visible.
static void* print_log(void* raw) {
    const int descriptor = (int)(intptr_t)raw;
    static const char priorities[] = "??VDIWEF";
    static uint8_t packet[65536];
    for (;;) {
        const ssize_t length = recv(descriptor, packet, sizeof packet - 1, 0);
        if (length <= 0) break;
        packet[length] = 0;
        if (length < 13) continue;  // header: log id, tid, time; then priority, tag, message
        const char* tag = (const char*)packet + 12;
        const char* message = tag + strlen(tag) + 1;
        if (message >= (const char*)packet + length) message = "";
        fprintf(stderr, "[logcat] %c %s: %s%s", priorities[packet[11] & 7], tag, message, message[0] && message[strlen(message) - 1] == '\n' ? "" : "\n");
    }
    close(descriptor);
    return NULL;
}

static int open_log_socket(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) return -1;
    const int buffer = 256 << 10;  // the default limits a datagram to 2 KiB, and abort messages are longer
    setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof buffer);
    setsockopt(pair[1], SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
    pthread_t thread;
    pthread_create(&thread, NULL, print_log, (void*)(intptr_t)pair[1]);
    pthread_detach(thread);
    return pair[0];
}

// ---------------------------------------------------------------- futex

// Waiters are keyed by host address, so a futex in memory shared between processes is not matched.
struct Waiter { uint8_t* address; pthread_cond_t wake; int woken; struct Waiter* next; };
static pthread_mutex_t futex_lock = PTHREAD_MUTEX_INITIALIZER;
static struct Waiter* waiters;

static void futex_wake(uint8_t* address, int count) {
    pthread_mutex_lock(&futex_lock);
    for (struct Waiter* waiter = waiters; waiter && count > 0; waiter = waiter->next)
        if (waiter->address == address && !waiter->woken) {
            waiter->woken = 1;
            pthread_cond_signal(&waiter->wake);
            --count;
        }
    pthread_mutex_unlock(&futex_lock);
}

static long guest_futex(struct Process* p, uint64_t address, int operation, uint32_t value, uint64_t timeout_address,
                        uint64_t second_address, uint32_t expected) {
    uint8_t* word = host(p, address);
    const int command = operation & 0x7f;
    if (command == 3 || command == 4) {  // FUTEX_REQUEUE, FUTEX_CMP_REQUEUE: wake some, move the rest to another word
        uint8_t* target = host(p, second_address);
        uint64_t moves = timeout_address;  // the fourth argument is a count here
        long total = 0;
        pthread_mutex_lock(&futex_lock);
        if (command == 4 && *(uint32_t*)word != expected) {
            pthread_mutex_unlock(&futex_lock);
            return -11;
        }
        for (struct Waiter* waiter = waiters; waiter; waiter = waiter->next) {
            if (waiter->address != word || waiter->woken) continue;
            if (value) {
                waiter->woken = 1;
                pthread_cond_signal(&waiter->wake);
                --value;
            } else if (moves) {
                waiter->address = target;
                --moves;
            } else {
                break;
            }
            ++total;
        }
        pthread_mutex_unlock(&futex_lock);
        return total;
    }
    if (command == 1 || command == 10) {  // FUTEX_WAKE, FUTEX_WAKE_BITSET
        int woken = 0;
        pthread_mutex_lock(&futex_lock);
        for (struct Waiter* waiter = waiters; waiter && (uint32_t)woken < value; waiter = waiter->next)
            if (waiter->address == word && !waiter->woken) {
                waiter->woken = 1;
                pthread_cond_signal(&waiter->wake);
                ++woken;
            }
        pthread_mutex_unlock(&futex_lock);
        return woken;
    }
    if (command != 0 && command != 9) return -38;  // FUTEX_WAIT, FUTEX_WAIT_BITSET
    struct Waiter waiter = {word, PTHREAD_COND_INITIALIZER, 0, NULL};
    pthread_mutex_lock(&futex_lock);
    if (*(uint32_t*)word != value) {
        pthread_mutex_unlock(&futex_lock);
        return -11;
    }
    waiter.next = waiters;
    waiters = &waiter;
    int result = 0;
    const int64_t* timeout = host(p, timeout_address);
    if (timeout) {
        // FUTEX_WAIT takes a relative time. FUTEX_WAIT_BITSET takes an absolute one on the guest's clock.
        struct timespec now, until = {timeout[0], timeout[1]};
        if (command == 9) {
            clock_gettime(operation & 256 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
            until.tv_sec -= now.tv_sec;
            until.tv_nsec -= now.tv_nsec;
            if (until.tv_nsec < 0) { until.tv_nsec += 1000000000; --until.tv_sec; }
            if (until.tv_sec < 0) until.tv_sec = until.tv_nsec = 0;
        }
        while (!waiter.woken && result == 0) result = pthread_cond_timedwait_relative_np(&waiter.wake, &futex_lock, &until);
    } else {
        while (!waiter.woken) pthread_cond_wait(&waiter.wake, &futex_lock);
    }
    for (struct Waiter** link = &waiters; *link; link = &(*link)->next)
        if (*link == &waiter) { *link = waiter.next; break; }
    pthread_mutex_unlock(&futex_lock);
    return waiter.woken ? 0 : -110;
}

// ---------------------------------------------------------------- system calls

// For an abort at the highest trace level: the words on the stack that point into low memory, where
// modules and the boot image are, which shows the chain of return addresses and methods.
static void dump_stack(struct Process* p, struct Context* context) {
    const uint64_t* stack = (const uint64_t*)context->sp;
    const struct Run* run = run_of_host(p, context->sp);
    for (int index = 0; run && index < 16384 && (uint8_t*)(stack + index) < run->host + run->length; ++index)
        if (stack[index] >= 0x10000000 && stack[index] < 0x80000000)
            fprintf(stderr, "[stack] sp+%05x %llx: %llx\n", index * 8, (uint64_t)(stack + index) - context->base, stack[index]);
}

void guest_syscall(struct Context* context) {
    struct Thread* t = context->thread;
    struct Process* p = t->process;
    if (p->exiting) finish_thread(t);
    const uint64_t* a = context->x;
    const long number = (long)a[8];
    long r;
    ++syscall_count;
    switch (number) {
    case 17: {
        char path[MAXPATHLEN];
        if (!getcwd(path, sizeof path)) { r = linux_errno(errno); break; }
        const char* visible = guest_path(path);
        if (strlen(visible) + 1 > a[1]) { r = -34; break; }
        strcpy(host(p, a[0]), visible);
        r = (long)strlen(visible) + 1;
        break;
    }
    case 23: r = install_fd(p, dup(host_fd(p, (long)a[0])), 0, 0); break;
    case 24:
        if (host_fd(p, (long)a[0]) < 0 || a[1] >= MAX_FDS) { r = -9; break; }
        close_fd(p, (int)a[1]);
        p->fds[a[1]] = dup(host_fd(p, (long)a[0]));
        p->cloexec[a[1]] = (a[2] & 02000000) != 0;
        r = (long)a[1];
        break;
    case 25: {
        const int descriptor = host_fd(p, (long)a[0]);
        if (descriptor < 0) { r = -9; break; }
        switch (a[1]) {
        case 0: case 1030: r = install_fd(p, dup(descriptor), (int)a[2], a[1] == 1030); break;
        case 1: r = p->cloexec[a[0]]; break;
        case 2: p->cloexec[a[0]] = a[2] & 1; r = 0; break;
        case 3: {
            const int flags = fcntl(descriptor, F_GETFL);
            r = flags < 0 ? linux_errno(errno) : (flags & 3) | (flags & O_APPEND ? 02000 : 0) | (flags & O_NONBLOCK ? 04000 : 0);
            break;
        }
        case 4: r = RESULT(fcntl(descriptor, F_SETFL, (a[2] & 02000 ? O_APPEND : 0) | (a[2] & 04000 ? O_NONBLOCK : 0))); break;
        default: r = -22;
        }
        break;
    }
    case 29:
        if (a[1] == 0x5413) r = RESULT(ioctl(host_fd(p, (long)a[0]), TIOCGWINSZ, host(p, a[2])));
        else if (a[1] == 0x5401 && isatty(host_fd(p, (long)a[0]))) { memset(host(p, a[2]), 0, 36); r = 0; }
        else r = -25;
        break;
    case 34: r = RESULT(mkdirat(host_directory(p, (long)a[0]), HP(a[1]), (mode_t)a[2])); break;
    case 35: r = RESULT(unlinkat(host_directory(p, (long)a[0]), HP(a[1]), a[2] & 0x200 ? AT_REMOVEDIR : 0)); break;
    case 36: r = RESULT(symlinkat(host(p, a[0]), host_directory(p, (long)a[1]), HP(a[2]))); break;
    case 38: case 276: r = RESULT(renameat(host_directory(p, (long)a[0]), HP(a[1]), host_directory(p, (long)a[2]), HP2(a[3]))); break;
    case 46: r = RESULT(ftruncate(host_fd(p, (long)a[0]), (off_t)a[1])); break;
    case 48: case 439: {
        // Execute permission comes from the mode bits: the host's answer reflects its own exec policy
        // (iOS denies it for every file an app can write), and the guest never execs through the host.
        struct stat info;
        const int directory = host_directory(p, (long)a[0]);
        const char* path = HP(a[1]);
        r = RESULT(faccessat(directory, path, (int)a[2] & ~X_OK, 0));
        if (r == 0 && a[2] & X_OK && (fstatat(directory, path, &info, 0) != 0 || !(info.st_mode & 0111))) r = -13;
        break;
    }
    case 49: r = RESULT(chdir(HP(a[0]))); break;
    case 52: r = RESULT(fchmod(host_fd(p, (long)a[0]), (mode_t)a[1])); break;
    case 53: r = RESULT(fchmodat(host_directory(p, (long)a[0]), HP(a[1]), (mode_t)a[2], 0)); break;
    case 56: {
        int descriptor = open_proc_file(p, host(p, a[1]));
        if (descriptor < 0) descriptor = openat(host_directory(p, (long)a[0]), HP(a[1]), open_flags((long)a[2]), (mode_t)a[3]);
        r = install_fd(p, descriptor, 0, (a[2] & 02000000) != 0);
        break;
    }
    case 57:
        if (host_fd(p, (long)a[0]) < 0) { r = -9; break; }
        close_fd(p, (int)a[0]);
        r = 0;
        break;
    case 59: {
        int pair[2];
        if (pipe(pair) != 0) { r = linux_errno(errno); break; }
        int* out = host(p, a[0]);
        out[0] = (int)install_fd(p, pair[0], 0, (a[1] & 02000000) != 0);
        out[1] = (int)install_fd(p, pair[1], 0, (a[1] & 02000000) != 0);
        r = 0;
        break;
    }
    case 61: r = guest_getdents(p, (long)a[0], host(p, a[1]), a[2]); break;
    case 62: r = RESULT(lseek(host_fd(p, (long)a[0]), (off_t)a[1], (int)a[2])); break;
    case 63: r = RESULT(buffer_io(p, (long)a[0], a[1], a[2], 0, 0, 0)); break;
    case 64: r = written(t, buffer_io(p, (long)a[0], a[1], a[2], 1, 0, 0)); break;
    case 65: r = RESULT(vector_io(p, (long)a[0], a[1], (long)a[2], 0)); break;
    case 66: r = written(t, vector_io(p, (long)a[0], a[1], (long)a[2], 1)); break;
    case 67: r = RESULT(buffer_io(p, (long)a[0], a[1], a[2], 0, 1, (off_t)a[3])); break;
    case 68: r = RESULT(buffer_io(p, (long)a[0], a[1], a[2], 1, 1, (off_t)a[3])); break;
    case 73: {
        struct pollfd* guest = host(p, a[0]);
        struct pollfd fds[64];
        if (a[1] > 64) { r = -22; break; }
        for (uint64_t index = 0; index < a[1]; ++index) fds[index] = (struct pollfd){host_fd(p, guest[index].fd), guest[index].events, 0};
        const int64_t* timeout = host(p, a[2]);
        r = RESULT(poll(fds, (nfds_t)a[1], timeout ? (int)(timeout[0] * 1000 + timeout[1] / 1000000) : -1));
        for (uint64_t index = 0; index < a[1]; ++index) guest[index].revents = fds[index].revents;
        break;
    }
    case 78: r = guest_readlink(p, (long)a[0], host(p, a[1]), host(p, a[2]), a[3]); break;
    case 79: {
        struct stat info;
        const char* path = host(p, a[1]);
        if (a[3] & 0x1000 && !*path) r = put_stat(fstat(host_fd(p, (long)a[0]), &info), &info, host(p, a[2]));
        else r = put_stat(fstatat(host_directory(p, (long)a[0]), host_path_in(p, path, 0), &info, a[3] & 0x100 ? AT_SYMLINK_NOFOLLOW : 0), &info, host(p, a[2]));
        break;
    }
    case 80: {
        struct stat info;
        r = put_stat(fstat(host_fd(p, (long)a[0]), &info), &info, host(p, a[1]));
        break;
    }
    case 82: case 83: r = RESULT(fsync(host_fd(p, (long)a[0]))); break;
    case 88: r = RESULT(utimensat(host_directory(p, (long)a[0]), HP(a[1]), host(p, a[2]), a[3] & 0x100 ? AT_SYMLINK_NOFOLLOW : 0)); break;
    case 93:  // exit: this thread only, unless it is the last one
        if (p->live_threads > 1) finish_thread(t);
        terminate(t, (int)(a[0] & 0xff) << 8);
    case 94: terminate(t, (int)(a[0] & 0xff) << 8);
    case 96: t->clear_tid = a[0]; r = t->tid; break;
    case 98: r = guest_futex(p, a[0], (int)a[1], (uint32_t)a[2], a[3], a[4], (uint32_t)a[5]); break;
    case 99: r = 0; break;  // set_robust_list
    case 132: {  // sigaltstack: sp, flags, size
        uint64_t* old = host(p, a[1]);
        const uint64_t* new = host(p, a[0]);
        const uint64_t previous[3] = {t->alternate_stack, t->alternate_size ? 0 : 2, t->alternate_size};
        if (new) {
            t->alternate_stack = new[0];
            t->alternate_size = (uint32_t)new[1] & 2 ? 0 : new[2];  // SS_DISABLE
        }
        if (old) memcpy(old, previous, sizeof previous);
        r = 0;
        break;
    }
    case 118: case 119: case 120: case 121: case 122: r = 0; break;  // scheduler parameters: SCHED_OTHER
    case 140: r = 0; break;   // setpriority
    case 141: r = 20; break;  // getpriority: nice 0, in the kernel's 20 - nice form
    case 270: {  // process_vm_readv on the caller itself: ART copies memory this way when it may be unmapped
        const uint64_t* local = host(p, a[1]);
        const uint64_t* remote = host(p, a[3]);
        if ((long)a[0] != p->pid || a[2] != 1 || a[4] != 1) { r = -38; break; }
        const uint64_t from = remote[0] & ADDRESS_MASK, length = remote[1] < local[1] ? remote[1] : local[1];
        int mapped = from >= PAGE && from + length <= SEGMENT && length <= 1 << 20;
        for (uint64_t page = from / PAGE; mapped && length && page <= (from + length - 1) / PAGE; ++page)
            mapped = page_used(p, page) && page_protection(p, page) & PROT_READ;
        if (!mapped) { r = -14; break; }
        uint8_t* copy = malloc(length);
        copy_out(p, copy, from, length);
        copy_in(p, local[0] & ADDRESS_MASK, copy, length);
        free(copy);
        r = (long)length;
        break;
    }
    case 43: case 44: {  // statfs, fstatfs: report an ext4 volume
        uint64_t* out = host(p, a[1]);
        memset(out, 0, 120);
        out[0] = 0xEF53;
        out[1] = PAGE;
        out[2] = out[3] = out[4] = 1 << 20;
        out[8] = 255;
        out[9] = PAGE;
        r = 0;
        break;
    }
    case 198:  // socket: only the datagram socket that Android logging uses
        if (a[0] == 1 && (a[1] & 0xf) == 2) r = install_fd(p, open_log_socket(), 0, (a[1] & 02000000) != 0);
        else r = -13;
        break;
    case 203: r = host_fd(p, (long)a[0]) < 0 ? -9 : 0; break;  // connect on the log socket
    case 240: {  // rt_tgsigqueueinfo: a signal with caller-supplied siginfo, as the unwinder sends to threads
        if (a[2] == 6 && trace > 2) dump_stack(p, context);
        if (a[2] < 1 || a[2] > 64 || (long)a[0] != p->pid) { r = -22; break; }
        pthread_mutex_lock(&lock);
        memcpy(p->queued[a[2]], host(p, a[3]), 128);
        p->has_queued |= SIGBIT(a[2]);
        p->pending |= SIGBIT(a[2]);
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        r = 0;
        break;
    }
    case 101: r = RESULT(nanosleep(host(p, a[0]), host(p, a[1]))); break;
    case 113: r = RESULT(clock_gettime(a[0] == 0 ? CLOCK_REALTIME : a[0] == 2 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_MONOTONIC, host(p, a[1]))); break;
    case 123: memset(host(p, a[2]), 0, a[1]); *(uint8_t*)host(p, a[2]) = 1; r = 8; break;
    case 124: sched_yield(); r = 0; break;
    case 129: case 130: case 131: {  // kill, tkill, tgkill
        const uint64_t signal = a[number == 131 ? 2 : 1];
        if (signal > 64) { r = -22; break; }
        if (signal == 6 && trace > 2) dump_stack(p, context);
        pthread_mutex_lock(&lock);
        struct Process* target = number == 129 && a[0] != 0 && (long)a[0] != p->pid ? processes[a[0] % MAX_PROCESSES] : p;
        if (target && number == 129 && a[0] != 0 && target->pid != (long)a[0]) target = NULL;
        // tkill and tgkill name one thread of the caller's process.
        struct Thread* thread = NULL;
        const long tid = number == 130 ? (long)(int)a[0] : number == 131 ? (long)(int)a[1] : 0;
        for (int index = 0; tid && index < MAX_THREADS; ++index)
            if (p->threads[index] && p->threads[index]->tid == tid && !p->threads[index]->dead) thread = p->threads[index];
        if (tid && !thread) target = NULL;
        if (target && signal) {
            if (thread) thread->pending |= SIGBIT(signal);
            else target->pending |= SIGBIT(signal);
        }
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        r = target ? 0 : -3;
        break;
    }
    case 133: {  // rt_sigsuspend: wait with a temporary mask for a signal that has a handler to run
        const uint64_t saved = t->blocked;
        pthread_mutex_lock(&lock);
        t->blocked = *(uint64_t*)host(p, a[0]);
        while (!((p->pending | t->pending) & ~t->blocked)) pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
        context->x[0] = (uint64_t)-4;
        const uint64_t temporary = t->blocked;
        deliver_signals(t, context, saved);  // the handler's frame returns to the original mask
        if (t->blocked == temporary) t->blocked = saved;  // nothing was delivered
        return;
    }
    case 134: {  // rt_sigaction: handler, flags, restorer, mask
        if (a[0] < 1 || a[0] > 64) { r = -22; break; }
        uint64_t* old = host(p, a[2]);
        const uint64_t* new = host(p, a[1]);
        const uint64_t previous[4] = {p->actions[a[0]].handler, p->actions[a[0]].flags, 0, p->actions[a[0]].mask};
        if (new) {
            p->actions[a[0]].handler = new[0];
            p->actions[a[0]].flags = new[1];
            p->actions[a[0]].mask = new[3];
        }
        if (old) memcpy(old, previous, sizeof previous);
        r = 0;
        break;
    }
    case 135: {  // rt_sigprocmask
        const uint64_t* set = host(p, a[1]);
        uint64_t* old = host(p, a[2]);
        const uint64_t previous = t->blocked;
        if (set) t->blocked = a[0] == 0 ? previous | *set : a[0] == 1 ? previous & ~*set : *set;
        t->blocked &= ~(SIGBIT(9) | SIGBIT(19));  // SIGKILL and SIGSTOP cannot be blocked
        if (old) *old = previous;
        r = 0;
        break;
    }
    case 137: {  // rt_sigtimedwait: take one of the signals in the set, as ART's signal catcher thread does
        const uint64_t set = *(uint64_t*)host(p, a[0]);
        const int64_t* timeout = host(p, a[2]);
        pthread_mutex_lock(&lock);
        r = -11;
        for (;;) {
            const uint64_t ready = (p->pending | t->pending) & set;
            if (ready) {
                r = __builtin_ctzll(ready) + 1;
                if (t->pending & SIGBIT(r)) t->pending &= ~SIGBIT(r);
                else p->pending &= ~SIGBIT(r);
                if (a[1]) {
                    memset(host(p, a[1]), 0, 128);
                    *(int*)host(p, a[1]) = (int)r;
                }
                break;
            }
            if (timeout) {
                struct timespec wait = {timeout[0], timeout[1]};
                if (pthread_cond_timedwait_relative_np(&changed, &lock, &wait) != 0) break;
            } else {
                pthread_cond_wait(&changed, &lock);
            }
        }
        pthread_mutex_unlock(&lock);
        break;
    }
    case 139:  // rt_sigreturn
        leave_handler(t, context);
        deliver_signals(t, context, t->blocked);
        return;
    case 154: case 157: r = 0; break;
    case 155: case 156: r = p->pid; break;
    case 160: {
        char* name = host(p, a[0]);
        memset(name, 0, 65 * 6);
        strcpy(name, "Linux");
        strcpy(name + 65, "nestvane");
        strcpy(name + 130, "6.6.0");
        strcpy(name + 195, "#1");
        strcpy(name + 260, "aarch64");
        r = 0;
        break;
    }
    case 163: case 261: {
        uint64_t* limit = host(p, number == 163 ? a[1] : a[3]);
        const uint64_t resource = number == 163 ? a[0] : a[1];
        if (limit) {
            limit[0] = resource == 3 ? STACK_SIZE : resource == 7 ? MAX_FDS : ~0ull;
            limit[1] = resource == 7 ? MAX_FDS : ~0ull;
        }
        r = 0;
        break;
    }
    case 165: memset(host(p, a[1]), 0, 144); r = 0; break;
    case 166: r = umask((mode_t)a[0]); break;
    case 167: r = 0; break;
    case 169: r = RESULT(gettimeofday(host(p, a[0]), NULL)); break;
    case 172: r = p->pid; break;
    case 178: r = t->tid; break;
    case 173: r = p->parent; break;
    case 174: case 175: case 176: case 177: r = 0; break;  // the guest sees itself as root, like the reference run
    case 179: memset(host(p, a[0]), 0, 112); r = 0; break;
    case 214:
        pthread_mutex_lock(&p->memory_lock);
        if (a[0] >= p->brk_start && a[0] <= p->brk_start + BRK_LIMIT) {
            const uint64_t old = page_up(p->brk), wanted = page_up(a[0]);
            if (wanted > old) {
                map_anonymous(p, old, wanted - old, PROT_READ | PROT_WRITE);
            } else if (wanted < old) {
                release(p, wanted, old - wanted);
            }
            p->brk = a[0];
        }
        r = (long)p->brk;
        pthread_mutex_unlock(&p->memory_lock);
        break;
    case 215:
        pthread_mutex_lock(&p->memory_lock);
        release(p, a[0] & ADDRESS_MASK, page_up(a[1]));
        pthread_mutex_unlock(&p->memory_lock);
        r = 0;
        break;
    case 216:
        pthread_mutex_lock(&p->memory_lock);
        r = guest_mremap(p, a[0] & ADDRESS_MASK, page_up(a[1]), page_up(a[2]), (int)a[3], a[4] & ADDRESS_MASK);
        pthread_mutex_unlock(&p->memory_lock);
        break;
    case 220:
        if (a[0] & 0x10000) r = guest_clone_thread(t, a[0], a[1], a[2], a[3], a[4]);  // CLONE_THREAD
        else r = guest_fork(t, a[0], a[1], a[4]);
        break;
    case 221:
        r = guest_exec(t, HP(a[0]), a[1], a[2]);
        if (r == 0) return;  // the context now describes the new image
        break;
    case 222:
        pthread_mutex_lock(&p->memory_lock);
        r = guest_mmap(p, a[0] & ADDRESS_MASK, a[1], (int)a[2], (int)a[3], (long)a[4], a[5]);
        pthread_mutex_unlock(&p->memory_lock);
        break;
    case 226:
        pthread_mutex_lock(&p->memory_lock);
        r = guest_mprotect(p, a[0] & ADDRESS_MASK, page_up(a[1]), (int)a[2] & (PROT_READ | PROT_WRITE));
        pthread_mutex_unlock(&p->memory_lock);
        break;
    case 227: case 232:  // msync, mincore: ART probes with these for unmapped address space
        r = range_free(p, a[0] & ~(PAGE - 1), PAGE) ? -12 : 0;
        if (r == 0 && number == 232) memset(host(p, a[2]), 1, page_up(a[1]) / PAGE);
        break;
    case 233: {  // madvise: only MADV_DONTNEED has an effect the guest can see
        r = 0;
        if (a[2] != 4) break;
        const uint64_t start = a[0] & ADDRESS_MASK, length = page_up(a[1]);
        if (start & (PAGE - 1) || start + length > SEGMENT) { r = -22; break; }
        pthread_mutex_lock(&p->memory_lock);
        for (uint64_t done = 0; done < length;) {
            const uint64_t part = piece(p, start + done, length - done);
            if (chunk_backed(p, (start + done) >> CHUNK_SHIFT)) discard((uint8_t*)(start + done + p->chunks[(start + done) >> CHUNK_SHIFT]), part);
            done += part;
        }
        pthread_mutex_unlock(&p->memory_lock);
        break;
    }
    case 260: r = guest_wait(p, (long)(int)a[0], a[1], (long)a[2]); break;
    case 278: arc4random_buf(host(p, a[0]), a[1]); r = (long)a[1]; break;
    case 0x4e56: {
        // The dynamic linker asks where a library must be loaded for its translated code to apply.
        struct stat info;
        const struct Module* module = fstat(host_fd(p, (long)a[0]), &info) == 0 ? module_for_stat(&info) : NULL;
        r = module ? (long)module->base : -2;
        break;
    }
    default:
        if (trace) fprintf(stderr, "[runtime] pid %d: unimplemented system call %ld\n", p->pid, number);
        r = -38;
    }
    if (trace > 1) fprintf(stderr, "[runtime] pid %d: %ld(%llx, %llx, %llx) = %ld\n", p->pid, number, a[0], a[1], a[2], r);
    if (trace > 1 && r < 0 && (number == 56 || number == 79 || number == 48 || number == 78))
        fprintf(stderr, "[runtime] pid %d:   path %s\n", p->pid, (const char*)host(p, number == 56 || number == 79 || number == 48 || number == 78 ? context->x[1] : 0));
    context->x[0] = (uint64_t)r;
    if ((p->pending | t->pending) & ~t->blocked) deliver_signals(t, context, t->blocked);
}

#ifndef GUEST_MAIN
#define GUEST_MAIN main  // an embedding app renames the entry point
#endif

int GUEST_MAIN(int argc, char** argv, char** envp) {
    (void)envp;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <guest elf or guest root> <guest argv...>\n", argv[0]);
        return 2;
    }
    trace = getenv("G0E3_TRACE") ? atoi(getenv("G0E3_TRACE")) : 0;
    struct stat info;
    if (stat(argv[1], &info) != 0) { perror(argv[1]); return 2; }
    if (S_ISDIR(info.st_mode)) {
        root = realpath(argv[1], NULL);
        root_length = strlen(root);
    }
    if (!code_table && !load_modules(root ? NULL : argv[1])) return 2;
    char path[MAXPATHLEN];
    snprintf(path, sizeof path, "%s%s", root ? root : "", root ? argv[2] : argv[1]);
    const struct Module* executable = module_for_file(path);
    if (!executable) { fprintf(stderr, "[runtime] %s is not a translated module\n", path); return 2; }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_thread_stop);
    struct sigaction fault = {.sa_sigaction = on_host_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGSEGV, &fault, NULL);
    sigaction(SIGBUS, &fault, NULL);

    const uint64_t begin = now_ns();
    struct Thread* main_thread = new_process(1);
    struct Process* first = main_thread->process;
    for (int fd = 0; fd < 3; ++fd) first->fds[fd] = dup(fd);
    char* environment[] = {root ? "PATH=/system/bin" : "PATH=/nonexistent", "HOME=/", "TERM=dumb", "LANG=C",
                           "ANDROID_ROOT=/system", "ANDROID_DATA=/data", NULL};
    if (!load_image(main_thread, executable, argv + 2, environment)) return 2;
    const int pid = first->pid;
    start_thread(main_thread);

    pthread_mutex_lock(&lock);
    while (!first->exited) pthread_cond_wait(&changed, &lock);
    const int status = first->status;
    processes[first->pid % MAX_PROCESSES] = NULL;
    free_tables(first);
    pthread_mutex_unlock(&lock);
    if (getenv("G0E3_STATS"))
        fprintf(stderr, "[runtime] pid=%d status=0x%x elapsed_ms=%.1f system_calls=%llu forks=%llu fork_mean_us=%.1f chunks_backed_on_touch=%llu "
                "accesses_emulated=%llu reserved_peak_mib=%lld reserved_now_mib=%lld\n", pid, status, (double)(now_ns() - begin) / 1e6, syscall_count,
                fork_count, fork_count ? (double)fork_ns / 1e3 / (double)fork_count : 0, lazy_count, emulated_count, reserved_peak >> 20,
                reserved_bytes >> 20);
    return status & 0x7f ? 128 + (status & 0x7f) : status >> 8;
}
