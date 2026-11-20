/* The init service runs as Android's shell UID. The APK FD stays read-only. */

static long call5(long number, long a, long b, long c, long d, long e) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x3 asm("x3") = d;
    register long x4 asm("x4") = e;
    register long x8 asm("x8") = number;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory", "cc");
    return x0;
}

static long system_call(long n, long a, long b, long c, long d) {
    return call5(n, a, b, c, d, 0);
}

enum {
    dup3 = 24, fcntl = 25, ioctl = 29, openat = 56, close = 57, lseek = 62,
    exit_group = 94, clone = 220, execve = 221, wait4 = 260,
    at_cwd = -100, close_on_exec = 0x80000, set_fd_flags = 2, fork_flags = 0x1200011,
    seek_start = 0, seek_end = 2, interrupted = -4, report_install = 0x40044e01,
    failed = 127 << 8,
};

static unsigned install(long apk) {
    const long length = system_call(lseek, apk, 0, seek_end, 0);
    if (length <= 0 || system_call(lseek, apk, 0, seek_start, 0) != 0) return failed;

    char buffer[24];
    char *size = buffer + sizeof buffer;
    *--size = '\0';
    unsigned long bytes = length;
    do {
        *--size = '0' + bytes % 10;
        bytes /= 10;
    } while (bytes);
    const char *program = "/system/bin/cmd";
    const char *argv[] = {program, "package", "install", "-r", "-S", size, 0};
    const char *envp[] = {"PATH=/system/bin", "ANDROID_ROOT=/system", 0};
    unsigned child_tid;
    const long child = call5(clone, fork_flags, 0, 0, 0, (long)&child_tid);
    if (child == 0) {
        // dup3 leaves the child's stdin open across exec. The original FD has CLOEXEC.
        const long input = apk == 0 ? system_call(fcntl, 0, set_fd_flags, 0, 0) :
            system_call(dup3, apk, 0, 0, 0);
        if (input == 0) system_call(execve, (long)program, (long)argv, (long)envp, 0);
        system_call(exit_group, failed >> 8, 0, 0, 0);
        __builtin_unreachable();
    }
    if (child < 0) return failed;
    unsigned status = failed;
    long result;
    do {
        result = system_call(wait4, child, (long)&status, 0, 0);
    } while (result == interrupted);
    return result == child ? status : failed;
}

/* Android starts this fixture at the ELF entry with argc and argv on the stack. */
__asm__(".text\n"
        ".global _start\n"
        ".type _start, %function\n"
        "_start:\n"
        "ldr x0, [sp]\n"
        "add x1, sp, #8\n"
        "b run_installer\n"
        ".size _start, .-_start\n");

__attribute__((noreturn))
static void finish(unsigned code) {
    system_call(exit_group, code, 0, 0, 0);
    __builtin_unreachable();
}

__attribute__((used, noreturn))
static void run_installer(long argc, const char *const *argv) {
    if (argc == 2) {
        const long fd = system_call(openat, at_cwd, (long)argv[1], close_on_exec, 0);
        if (fd < 0) finish(1);
        const unsigned status = install(fd);
        const unsigned signal = status & 0x7f;
        finish(signal ? 128 + signal : (status >> 8) & 0xff);
    }
    if (argc != 1) finish(1);
    for (;;) {
        const long fd = system_call(openat, at_cwd, (long)"/dev/nestvane-apk", close_on_exec, 0);
        if (fd == interrupted) continue;
        if (fd < 0) break;
        const unsigned status = install(fd);
        const long reported = system_call(ioctl, fd, report_install, (long)&status, 0);
        system_call(close, fd, 0, 0, 0);
        if (reported != 0) break;
    }
    finish(1);
}
