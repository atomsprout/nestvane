// Host runtime for a guest rewritten by rewrite.py: one guest process is one 1 GiB segment and one
// host thread, fork is a copy-on-write vm_remap of the segment, and Linux system calls are served by
// the host. It implements what toybox needs and nothing more.
//
// usage: <program> <guest elf> <guest argv...>

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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef SEGMENT
#define SEGMENT (1ull << 30)  // guest address space per process; -DSEGMENT=... to change
#endif
#define PAGE 16384ull
#define STACK_SIZE (8ull << 20)
#define BRK_LIMIT (128ull << 20)
#define MAX_FDS 256
#define MAX_PROCESSES 1024

struct Process;

struct Context {
    uint64_t x[31];
    uint64_t sp;  // host address
    __uint128_t q[32];
    uint64_t nzcv, fpcr, tpidr;
    uint64_t pc;  // host address
    uint64_t base, table, host_sp;
    struct Process* process;
};
_Static_assert(__builtin_offsetof(struct Context, sp) == 248, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, q) == 256, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, nzcv) == 768, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, tpidr) == 784, "rewrite.py");
_Static_assert(__builtin_offsetof(struct Context, pc) == 792, "entry.S");
_Static_assert(__builtin_offsetof(struct Context, host_sp) == 816, "entry.S");

struct Process {
    int pid, parent;
    int exited, status, reaped, vfork_done;
    uint8_t* base;
    uint64_t brk_start, brk, map_low, map_high;
    uint8_t used[SEGMENT / PAGE / 8];  // one bit per page handed out by mmap
    int fds[MAX_FDS];
    uint8_t cloexec[MAX_FDS];
    DIR* directories[MAX_FDS];
    uint64_t pipe_handler;  // 0 default, 1 ignore, otherwise a guest handler
    struct Context context;
};

extern const uint64_t guest_text_start, guest_text_end, guest_entry;
extern const uint64_t guest_table[];
void guest_enter(struct Context*) __attribute__((noreturn));

static uint8_t* image;
static size_t image_size;
static const char* image_path;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct Process* processes[MAX_PROCESSES];
static int next_pid = 100;
static int trace;
static uint64_t syscall_count, fork_count, fork_ns;

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

static void* host(struct Process* process, uint64_t address) { return address ? process->base + address : NULL; }

static int host_fd(struct Process* process, long fd) {
    return fd >= 0 && fd < MAX_FDS ? process->fds[fd] : -1;
}

static long install_fd(struct Process* process, int descriptor, int minimum, int cloexec) {
    if (descriptor < 0) return linux_errno(errno);
    for (int fd = minimum; fd < MAX_FDS; ++fd)
        if (process->fds[fd] < 0) {
            process->fds[fd] = descriptor;
            process->cloexec[fd] = (uint8_t)cloexec;
            return fd;
        }
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

// ---------------------------------------------------------------- guest memory

static void map_none(struct Process* process, uint64_t address, uint64_t length) {
    mmap(process->base + address, length, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

static int page_used(const struct Process* process, uint64_t page) { return process->used[page / 8] >> (page % 8) & 1; }

static void mark(struct Process* process, uint64_t address, uint64_t length, int used) {
    for (uint64_t page = address / PAGE; page < (address + length) / PAGE; ++page) {
        if (used) process->used[page / 8] |= (uint8_t)(1 << page % 8);
        else process->used[page / 8] &= (uint8_t)~(1 << page % 8);
    }
}

// First fit from the top of the mapping area, like Linux's top-down layout.
static uint64_t allocate(struct Process* process, uint64_t length) {
    const uint64_t pages = length / PAGE, low = process->map_low / PAGE;
    uint64_t run = 0;
    for (uint64_t page = process->map_high / PAGE; page-- > low;) {
        run = page_used(process, page) ? 0 : run + 1;
        if (run == pages) {
            mark(process, page * PAGE, length, 1);
            return page * PAGE;
        }
    }
    return 0;
}

static void release(struct Process* process, uint64_t address, uint64_t length) {
    map_none(process, address, length);
    mark(process, address, length, 0);
}

// Grows in place when the pages above are free, otherwise moves the mapping without copying it.
static long guest_mremap(struct Process* process, uint64_t address, uint64_t old, uint64_t wanted, int may_move) {
    if (wanted <= old) {
        if (wanted < old) release(process, address + wanted, old - wanted);
        return (long)address;
    }
    int room = address + wanted <= process->map_high;
    for (uint64_t page = (address + old) / PAGE; room && page < (address + wanted) / PAGE; ++page) room = !page_used(process, page);
    uint64_t target = address;
    if (!room) {
        if (!may_move || !(target = allocate(process, wanted))) return -12;
        vm_address_t destination = (vm_address_t)(process->base + target);
        vm_prot_t current, maximum;
        if (vm_remap(mach_task_self(), &destination, old, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                     (vm_address_t)(process->base + address), FALSE, &current, &maximum, VM_INHERIT_COPY) != KERN_SUCCESS)
            return -12;
        release(process, address, old);
    }
    mark(process, target, wanted, 1);
    mmap(process->base + target + old, wanted - old, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return (long)target;
}

static long guest_mmap(struct Process* process, uint64_t address, uint64_t length, int protection, int flags, long fd, uint64_t offset) {
    length = page_up(length);
    if (!length) return -22;
    const int fixed = flags & 0x10, anonymous = flags & 0x20, shared = flags & 1;
    if (fixed) {
        if (address & (PAGE - 1) || address + length > SEGMENT) return -22;
    } else if (!(address = allocate(process, length))) {
        return -12;
    }
    void* result = mmap(process->base + address, length, protection & (PROT_READ | PROT_WRITE),
                        MAP_FIXED | (anonymous ? MAP_ANON : 0) | (shared && !anonymous ? MAP_SHARED : MAP_PRIVATE),
                        anonymous ? -1 : host_fd(process, fd), anonymous ? 0 : (off_t)offset);
    if (result == MAP_FAILED) return linux_errno(errno);
    return (long)address;
}

// Puts the ELF and a fresh stack into the process's segment and points its context at the entry.
static void load_image(struct Process* process, char** argv, char** envp) {
    uint8_t* base = process->base;
    mmap(base, SEGMENT, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    const uint64_t phoff = *(uint64_t*)(image + 32);
    const uint16_t phentsize = *(uint16_t*)(image + 54), phnum = *(uint16_t*)(image + 56);
    uint64_t end = 0, first = ~0ull;
    for (int index = 0; index < phnum; ++index) {
        const uint8_t* header = image + phoff + (uint64_t)index * phentsize;
        if (*(uint32_t*)header != 1) continue;
        const uint64_t address = *(uint64_t*)(header + 16);
        const uint64_t memory_size = *(uint64_t*)(header + 40);
        const uint64_t low = address & ~(PAGE - 1), high = page_up(address + memory_size);
        mmap(base + low, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (address < first) first = address;
        if (high > end) end = high;
    }
    for (int index = 0; index < phnum; ++index) {
        const uint8_t* header = image + phoff + (uint64_t)index * phentsize;
        if (*(uint32_t*)header != 1) continue;
        memcpy(base + *(uint64_t*)(header + 16), image + *(uint64_t*)(header + 8), *(uint64_t*)(header + 32));
    }
    process->brk_start = process->brk = end;
    process->map_low = end + BRK_LIMIT;
    const uint64_t stack_top = SEGMENT - PAGE;
    process->map_high = stack_top - STACK_SIZE - PAGE;
    memset(process->used, 0, sizeof process->used);
    mmap(base + stack_top - STACK_SIZE, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);

    int argc = 0, envc = 0;
    while (argv[argc]) ++argc;
    while (envp[envc]) ++envc;
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
    const uint64_t auxv[] = {3, first + phoff, 4, phentsize, 5, phnum, 6, PAGE, 9, guest_entry, 11, 0, 12, 0, 13, 0,
                             14, 0, 16, 0, 23, 0, 25, random, 0, 0};
    const uint64_t words = 1 + argc + 1 + envc + 1 + sizeof auxv / 8;
    cursor = (cursor - words * 8) & ~15ull;
    uint64_t* stack = (uint64_t*)(base + cursor);
    *stack++ = (uint64_t)argc;
    for (int index = 0; index < argc; ++index) *stack++ = pointers[index];
    *stack++ = 0;
    for (int index = 0; index < envc; ++index) *stack++ = pointers[argc + index];
    *stack++ = 0;
    memcpy(stack, auxv, sizeof auxv);

    struct Context* context = &process->context;
    const uint64_t host_sp = context->host_sp;
    memset(context, 0, sizeof *context);
    context->host_sp = host_sp;
    context->process = process;
    context->base = (uint64_t)base;
    context->table = (uint64_t)guest_table - 2 * guest_text_start;
    context->sp = (uint64_t)base + cursor;
    context->pc = guest_table[(guest_entry - guest_text_start) / 4];
}

// ---------------------------------------------------------------- processes

static struct Process* new_process(int parent) {
    struct Process* process = calloc(1, sizeof *process);
    for (int fd = 0; fd < MAX_FDS; ++fd) process->fds[fd] = -1;
    pthread_mutex_lock(&lock);
    process->pid = next_pid++;
    process->parent = parent;
    processes[process->pid % MAX_PROCESSES] = process;
    pthread_mutex_unlock(&lock);
    return process;
}

static void terminate(struct Process* process, int status) __attribute__((noreturn));
static void terminate(struct Process* process, int status) {
    for (int fd = 0; fd < MAX_FDS; ++fd) close_fd(process, fd);
    munmap(process->base, SEGMENT);
    pthread_mutex_lock(&lock);
    process->exited = 1;
    process->status = status;
    process->vfork_done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    pthread_exit(NULL);
}

static void* run_process(void* raw) {
    guest_enter(&((struct Process*)raw)->context);
}

static void start_thread(struct Process* process) {
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1 << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attributes, run_process, process);
}

static long guest_fork(struct Process* parent, uint64_t flags, uint64_t child_stack) {
    const uint64_t begin = now_ns();
    struct Process* child = new_process(parent->pid);
    vm_address_t address = 0;
    vm_prot_t current, maximum;
    if (vm_remap(mach_task_self(), &address, SEGMENT, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)parent->base, TRUE,
                 &current, &maximum, VM_INHERIT_COPY) != KERN_SUCCESS)
        return -12;
    child->base = (uint8_t*)address;
    child->brk_start = parent->brk_start;
    child->brk = parent->brk;
    child->map_low = parent->map_low;
    child->map_high = parent->map_high;
    child->pipe_handler = parent->pipe_handler;
    memcpy(child->used, parent->used, sizeof child->used);
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if (parent->fds[fd] >= 0) {
            child->fds[fd] = dup(parent->fds[fd]);
            child->cloexec[fd] = parent->cloexec[fd];
        }
    // Registers hold guest addresses, so only the base and the stack pointer change.
    child->context = parent->context;
    child->context.process = child;
    child->context.base = (uint64_t)child->base;
    child->context.sp = child_stack ? (uint64_t)child->base + child_stack : parent->context.sp - (uint64_t)parent->base + (uint64_t)child->base;
    child->context.x[0] = 0;
    __atomic_add_fetch(&fork_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&fork_ns, now_ns() - begin, __ATOMIC_RELAXED);
    start_thread(child);
    if (flags & 0x4000) {  // CLONE_VFORK: the parent waits until the child execs or exits
        pthread_mutex_lock(&lock);
        while (!child->vfork_done) pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
    }
    return child->pid;
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
            const int result = child->pid;
            if (status_address) *(int*)host(process, status_address) = child->status;
            pthread_mutex_unlock(&lock);
            free(child);
            return result;
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

static long guest_exec(struct Process* process, const char* path, uint64_t argv, uint64_t envp) {
    struct stat wanted, ours;
    if (stat(path, &wanted) != 0) return linux_errno(errno);
    if (stat(image_path, &ours) != 0 || wanted.st_ino != ours.st_ino || wanted.st_dev != ours.st_dev) return -8;
    char** arguments = copy_strings(process, argv);
    char** environment = copy_strings(process, envp);
    for (int fd = 0; fd < MAX_FDS; ++fd)
        if (process->cloexec[fd]) close_fd(process, fd);
    load_image(process, arguments, environment);
    pthread_mutex_lock(&lock);
    process->vfork_done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

// ---------------------------------------------------------------- files

static const char* host_path(const char* path) {
    return strcmp(path, "/proc/self/exe") == 0 ? image_path : path;
}

static int host_directory(struct Process* process, long fd) { return fd == -100 ? AT_FDCWD : host_fd(process, fd); }

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
    struct iovec vectors[64];
    if (count < 0 || count > 64) return -22;
    const uint64_t* guest = host(process, vector);
    for (long index = 0; index < count; ++index) {
        vectors[index].iov_base = host(process, guest[index * 2]);
        vectors[index].iov_len = guest[index * 2 + 1];
    }
    return writing ? writev(host_fd(process, fd), vectors, (int)count) : readv(host_fd(process, fd), vectors, (int)count);
}

// A write to a pipe with no reader kills a Linux process unless it handles SIGPIPE.
static long written(struct Process* process, long result) {
    if (result < 0 && errno == EPIPE && process->pipe_handler == 0) terminate(process, 13);
    return RESULT(result);
}

// ---------------------------------------------------------------- system calls

void guest_syscall(struct Context* context) {
    struct Process* p = context->process;
    const uint64_t* a = context->x;
    const long number = (long)a[8];
    long r;
    ++syscall_count;
    switch (number) {
    case 17: r = getcwd(host(p, a[0]), a[1]) ? (long)strlen(host(p, a[0])) + 1 : linux_errno(errno); break;
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
    case 34: r = RESULT(mkdirat(host_directory(p, (long)a[0]), host(p, a[1]), (mode_t)a[2])); break;
    case 35: r = RESULT(unlinkat(host_directory(p, (long)a[0]), host(p, a[1]), a[2] & 0x200 ? AT_REMOVEDIR : 0)); break;
    case 36: r = RESULT(symlinkat(host(p, a[0]), host_directory(p, (long)a[1]), host(p, a[2]))); break;
    case 38: case 276: r = RESULT(renameat(host_directory(p, (long)a[0]), host(p, a[1]), host_directory(p, (long)a[2]), host(p, a[3]))); break;
    case 46: r = RESULT(ftruncate(host_fd(p, (long)a[0]), (off_t)a[1])); break;
    case 48: case 439: {
        // Execute permission comes from the mode bits: the host's answer reflects its own exec policy
        // (iOS denies it for every file an app can write), and the guest never execs through the host.
        struct stat info;
        const int directory = host_directory(p, (long)a[0]);
        const char* path = host_path(host(p, a[1]));
        r = RESULT(faccessat(directory, path, (int)a[2] & ~X_OK, 0));
        if (r == 0 && a[2] & X_OK && (fstatat(directory, path, &info, 0) != 0 || !(info.st_mode & 0111))) r = -13;
        break;
    }
    case 49: r = RESULT(chdir(host(p, a[0]))); break;
    case 52: r = RESULT(fchmod(host_fd(p, (long)a[0]), (mode_t)a[1])); break;
    case 53: r = RESULT(fchmodat(host_directory(p, (long)a[0]), host(p, a[1]), (mode_t)a[2], 0)); break;
    case 56: r = install_fd(p, openat(host_directory(p, (long)a[0]), host_path(host(p, a[1])), open_flags((long)a[2]), (mode_t)a[3]), 0, (a[2] & 02000000) != 0); break;
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
    case 63: r = RESULT(read(host_fd(p, (long)a[0]), host(p, a[1]), a[2])); break;
    case 64: r = written(p, write(host_fd(p, (long)a[0]), host(p, a[1]), a[2])); break;
    case 65: r = RESULT(vector_io(p, (long)a[0], a[1], (long)a[2], 0)); break;
    case 66: r = written(p, vector_io(p, (long)a[0], a[1], (long)a[2], 1)); break;
    case 67: r = RESULT(pread(host_fd(p, (long)a[0]), host(p, a[1]), a[2], (off_t)a[3])); break;
    case 68: r = RESULT(pwrite(host_fd(p, (long)a[0]), host(p, a[1]), a[2], (off_t)a[3])); break;
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
    case 78: {
        const char* path = host(p, a[1]);
        if (strcmp(path, "/proc/self/exe") == 0) {
            r = (long)strlen(image_path);
            if ((uint64_t)r > a[3]) r = (long)a[3];
            memcpy(host(p, a[2]), image_path, (size_t)r);
        } else {
            r = RESULT(readlinkat(host_directory(p, (long)a[0]), path, host(p, a[2]), a[3]));
        }
        break;
    }
    case 79: {
        struct stat info;
        const char* path = host(p, a[1]);
        if (a[3] & 0x1000 && !*path) r = put_stat(fstat(host_fd(p, (long)a[0]), &info), &info, host(p, a[2]));
        else r = put_stat(fstatat(host_directory(p, (long)a[0]), host_path(path), &info, a[3] & 0x100 ? AT_SYMLINK_NOFOLLOW : 0), &info, host(p, a[2]));
        break;
    }
    case 80: {
        struct stat info;
        r = put_stat(fstat(host_fd(p, (long)a[0]), &info), &info, host(p, a[1]));
        break;
    }
    case 82: case 83: r = RESULT(fsync(host_fd(p, (long)a[0]))); break;
    case 88: r = RESULT(utimensat(host_directory(p, (long)a[0]), host(p, a[1]), host(p, a[2]), a[3] & 0x100 ? AT_SYMLINK_NOFOLLOW : 0)); break;
    case 93: case 94: terminate(p, (int)(a[0] & 0xff) << 8);
    case 96: r = p->pid; break;
    case 98: r = (a[1] & 0x7f) == 0 ? -11 : 0; break;
    case 101: r = RESULT(nanosleep(host(p, a[0]), host(p, a[1]))); break;
    case 113: r = RESULT(clock_gettime(a[0] == 0 ? CLOCK_REALTIME : a[0] == 2 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_MONOTONIC, host(p, a[1]))); break;
    case 123: memset(host(p, a[2]), 0, a[1]); *(uint8_t*)host(p, a[2]) = 1; r = 8; break;
    case 124: sched_yield(); r = 0; break;
    case 129: case 130: case 131:
        if (number == 129 && (long)a[0] != p->pid && a[0] != 0) { r = a[1] == 0 ? 0 : -1; break; }
        if (a[number == 131 ? 2 : 1] == 0) { r = 0; break; }
        terminate(p, (int)a[number == 131 ? 2 : 1]);
    case 134:
        if (a[0] == 13) {
            if (a[2]) *(uint64_t*)host(p, a[2]) = p->pipe_handler;
            if (a[1]) p->pipe_handler = *(uint64_t*)host(p, a[1]);
        } else if (a[2]) {
            memset(host(p, a[2]), 0, 32);
        }
        r = 0;
        break;
    case 135: if (a[2]) memset(host(p, a[2]), 0, a[3]); r = 0; break;
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
    case 172: case 178: r = p->pid; break;
    case 173: r = p->parent; break;
    case 174: case 175: case 176: case 177: r = 0; break;  // the guest sees itself as root, like the reference run
    case 179: memset(host(p, a[0]), 0, 112); r = 0; break;
    case 214:
        if (a[0] >= p->brk_start && a[0] <= p->brk_start + BRK_LIMIT) {
            const uint64_t old = page_up(p->brk), wanted = page_up(a[0]);
            if (wanted > old) mmap(p->base + old, wanted - old, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            else if (wanted < old) map_none(p, wanted, old - wanted);
            p->brk = a[0];
        }
        r = (long)p->brk;
        break;
    case 215: release(p, a[0], page_up(a[1])); r = 0; break;
    case 216: r = guest_mremap(p, a[0], page_up(a[1]), page_up(a[2]), a[3] & 1); break;
    case 220:
        if (a[0] & 0x10000) { r = -38; break; }  // CLONE_THREAD
        r = guest_fork(p, a[0], a[1]);
        break;
    case 221:
        r = guest_exec(p, host_path(host(p, a[0])), a[1], a[2]);
        if (r == 0) return;  // the context now describes the new image
        break;
    case 222: r = guest_mmap(p, a[0], a[1], (int)a[2], (int)a[3], (long)a[4], a[5]); break;
    case 226: r = RESULT(mprotect(host(p, a[0]), page_up(a[1]), (int)a[2] & (PROT_READ | PROT_WRITE))); break;
    case 233: r = 0; break;
    case 260: r = guest_wait(p, (long)(int)a[0], a[1], (long)a[2]); break;
    case 278: arc4random_buf(host(p, a[0]), a[1]); r = (long)a[1]; break;
    default:
        if (trace) fprintf(stderr, "[runtime] pid %d: unimplemented system call %ld\n", p->pid, number);
        r = -38;
    }
    if (trace > 1) fprintf(stderr, "[runtime] pid %d: %ld(%llx, %llx, %llx) = %ld\n", p->pid, number, a[0], a[1], a[2], r);
    context->x[0] = (uint64_t)r;
}

#ifndef GUEST_MAIN
#define GUEST_MAIN main  // an embedding app renames the entry point
#endif

int GUEST_MAIN(int argc, char** argv, char** envp) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <guest elf> <guest argv...>\n", argv[0]);
        return 2;
    }
    trace = getenv("G0E3_TRACE") ? atoi(getenv("G0E3_TRACE")) : 0;
    image_path = realpath(argv[1], NULL);
    const int descriptor = image_path ? open(image_path, O_RDONLY) : -1;
    struct stat info;
    if (descriptor < 0 || fstat(descriptor, &info) != 0) { perror(argv[1]); return 2; }
    image_size = (size_t)info.st_size;
    image = mmap(NULL, image_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
    close(descriptor);
    signal(SIGPIPE, SIG_IGN);

    const uint64_t begin = now_ns();
    struct Process* first = new_process(1);
    first->base = mmap(NULL, SEGMENT, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (int fd = 0; fd < 3; ++fd) first->fds[fd] = dup(fd);
    char* environment[] = {"PATH=/nonexistent", "HOME=/", "TERM=dumb", "LANG=C", NULL};
    (void)envp;
    load_image(first, argv + 2, environment);
    const int pid = first->pid;
    start_thread(first);

    pthread_mutex_lock(&lock);
    while (!first->exited) pthread_cond_wait(&changed, &lock);
    const int status = first->status;
    pthread_mutex_unlock(&lock);
    if (getenv("G0E3_STATS"))
        fprintf(stderr, "[runtime] pid=%d status=0x%x elapsed_ms=%.1f system_calls=%llu forks=%llu fork_mean_us=%.1f\n", pid, status,
                (double)(now_ns() - begin) / 1e6, syscall_count, fork_count, fork_count ? (double)fork_ns / 1e3 / (double)fork_count : 0);
    return status & 0x7f ? 128 + (status & 0x7f) : status >> 8;
}
