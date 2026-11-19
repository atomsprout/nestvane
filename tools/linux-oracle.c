/* A freestanding AArch64 Linux initramfs test. The init process traces only
 * the two fixture processes it starts and reads X19 at their exit syscall. */

typedef unsigned long u64;

static long call(long nr, long a, long b, long c, long d, long e) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x3 asm("x3") = d;
    register long x4 asm("x4") = e;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory", "cc");
    return x0;
}

static void output(const char *text) {
    long length = 0;
    while (text[length]) ++length;
    call(64, 1, (long)text, length, 0, 0);
}

static void print_number(u64 value) {
    char buffer[21];
    int index = sizeof(buffer);
    do {
        buffer[--index] = '0' + value % 10;
    } while ((value /= 10) != 0);
    call(64, 1, (long)&buffer[index], sizeof(buffer) - index, 0, 0);
}

static int wait_status(long child) {
    int status = -1;
    if (call(260, child, (long)&status, 0, 0, 0) != child) return -1;
    return status;
}

static int fixture(const char *path, u64 expected) {
    enum { traceMe = 0, traceContinue = 7, traceSyscall = 24, getRegSet = 0x4204 };
    long child = call(220, 17, 0, 0, 0, 0);
    if (child == 0) {
        const char *argv[] = { path, 0 };
        const char *envp[] = { 0 };
        if (call(117, traceMe, 0, 0, 0, 0) == 0) {
            call(221, (long)path, (long)argv, (long)envp, 0, 0);
        }
        call(93, 1, 0, 0, 0, 0);
        for (;;) {}
    }
    if (child < 0 || wait_status(child) != 0x57f) return 0;
    for (int i = 0; i < 12; ++i) {
        if (call(117, traceSyscall, child, 0, 0, 0) < 0 || wait_status(child) != 0x57f) return 0;
        u64 registers[34];
        struct { u64 *base; u64 len; } iov = { registers, sizeof(registers) };
        if (call(117, getRegSet, child, 1, (long)&iov, 0) < 0) return 0;
        if (registers[8] != 93) continue;
        output(path);
        output(" X19=");
        print_number(registers[19]);
        output("\n");
        if (call(117, traceContinue, child, 0, 0, 0) < 0) return 0;
        return wait_status(child) == 0 && registers[19] == expected;
    }
    return 0;
}

void _start(void) {
    int arithmetic = fixture("/arithmetic.elf", 1250175000UL);
    int memory = fixture("/memory.elf", 1250425000UL);
    output(arithmetic && memory ? "NESTVANE_LINUX_ORACLE PASS\n" : "NESTVANE_LINUX_ORACLE FAIL\n");
    call(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0);
    for (;;) {}
}
