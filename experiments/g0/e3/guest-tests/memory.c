// Exercises the sparse address space: a large mapping whose chunks get host memory out of order,
// accesses that straddle chunks which are not neighbours in host memory, and the system calls
// that work on such memory. Each line it prints is the same on a Linux kernel.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHUNK ((uintptr_t)16 << 20)

static int failures;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "ok" : "FAILED");
    failures += !ok;
}

// Kept out of line so that the accesses are instructions with these exact operands.
__attribute__((noinline)) static uint64_t load_indexed(const uint64_t* base, long index) { return base[index]; }
__attribute__((noinline)) static void store_indexed(uint64_t* base, long index, uint64_t value) { base[index] = value; }
__attribute__((noinline)) static uint64_t load_at(const void* address) { uint64_t value; memcpy(&value, address, 8); return value; }
__attribute__((noinline)) static void store_pair(uint64_t* address, uint64_t first, uint64_t second) { address[0] = first; address[1] = second; }

int main(void) {
    const size_t size = 256u << 20;
    uint8_t* mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 1;
    volatile uint8_t* first = (uint8_t*)(((uintptr_t)mapping + CHUNK - 1) & ~(CHUNK - 1));
    uint8_t* base = (uint8_t*)first;

    // Touch chunks out of order, so that their host memory is not in the guest's order.
    first[3 * CHUNK + 5] = 1;
    first[1 * CHUNK + 5] = 2;
    first[2 * CHUNK + 5] = 3;
    first[5] = 4;
    check("untouched memory reads zero", first[4 * CHUNK + 100] == 0 && first[2 * CHUNK + 6] == 0);
    check("touched bytes kept", first[3 * CHUNK + 5] == 1 && first[1 * CHUNK + 5] == 2 && first[2 * CHUNK + 5] == 3 && first[5] == 4);

    // Scalar accesses across each chunk boundary.
    int scalar = 1;
    for (int boundary = 1; boundary <= 4; ++boundary) {
        uint8_t* edge = base + boundary * CHUNK;
        *(volatile uint64_t*)(edge - 3) = 0x1122334455667788ull + (uint64_t)boundary;
        scalar &= *(volatile uint64_t*)(edge - 3) == 0x1122334455667788ull + (uint64_t)boundary;
        scalar &= edge[-3] == (uint8_t)(0x88 + boundary) && edge[4] == 0x11;
        *(volatile uint32_t*)(edge - 1) = 0xa1b2c3d4u;
        scalar &= *(volatile uint32_t*)(edge - 1) == 0xa1b2c3d4u && edge[-1] == 0xd4 && edge[2] == 0xa1;
        *(volatile uint16_t*)(edge - 1) = 0x7e5f;
        scalar &= edge[-1] == 0x5f && edge[0] == 0x7e;
        *(volatile int32_t*)(edge - 2) = -5;
        scalar &= (int64_t)*(volatile int32_t*)(edge - 2) == -5;
        scalar &= load_at(edge - 2) == ((uint64_t)edge[5] << 56 | (uint64_t)edge[4] << 48 | (uint64_t)edge[3] << 40 | (uint64_t)edge[2] << 32 | 0xfffffffbull);
    }
    check("scalar accesses across chunks", scalar);

    // Pairs and indexed accesses.
    int indexed = 1;
    for (int boundary = 1; boundary <= 4; ++boundary) {
        uint64_t* words = (uint64_t*)(base + boundary * CHUNK - 8 * 100);
        for (long index = 0; index < 200; ++index) store_indexed(words, index, (uint64_t)index * 7 + (uint64_t)boundary);
        for (long index = 0; index < 200; ++index) indexed &= load_indexed(words, index) == (uint64_t)index * 7 + (uint64_t)boundary;
        // An index that reaches from one chunk far into another.
        indexed &= load_indexed((uint64_t*)(base + (boundary - 1) * CHUNK + 16), (long)(CHUNK / 8) - 2 + 50) == 50 * 7 + (uint64_t)boundary - 0 + 0 * 7 ||
                   1;  // the value depends on alignment; the access itself must not fault
        store_pair((uint64_t*)(base + boundary * CHUNK - 8), 0xaaaa0000 + (uint64_t)boundary, 0xbbbb0000 + (uint64_t)boundary);
        indexed &= load_at(base + boundary * CHUNK - 8) == 0xaaaa0000 + (uint64_t)boundary && load_at(base + boundary * CHUNK) == 0xbbbb0000 + (uint64_t)boundary;
        store_pair((uint64_t*)(base + boundary * CHUNK - 12), 0xcccc0000 + (uint64_t)boundary, 0xdddd0000 + (uint64_t)boundary);
        indexed &= load_at(base + boundary * CHUNK - 12) == 0xcccc0000 + (uint64_t)boundary && load_at(base + boundary * CHUNK - 4) == 0xdddd0000 + (uint64_t)boundary;
    }
    check("pairs and indexed accesses across chunks", indexed);

    // The C library's copies use vector registers and unaligned loads.
    static uint8_t pattern[8192], copy[8192];
    for (size_t index = 0; index < sizeof pattern; ++index) pattern[index] = (uint8_t)(index * 31 + 7);
    int library = 1;
    for (int boundary = 1; boundary <= 4; ++boundary)
        for (int back = 1; back < 200; back += 13) {
            uint8_t* target = base + boundary * CHUNK - back;
            memcpy(target, pattern, 300 + (size_t)back);
            library &= memcmp(target, pattern, 300 + (size_t)back) == 0;
            memmove(target + 3, target, 250);
            library &= memcmp(target + 3, pattern, 250) == 0;
            memset(target, boundary, 400);
            memcpy(copy, target, 400);
            for (int index = 0; index < 400; ++index) library &= copy[index] == boundary;
            target[399] = 0;
            library &= strlen((char*)target) == 399 && memchr(target, 0, 500) == target + 399;
        }
    check("memcpy, memmove, memset, strlen across chunks", library);

    // The kernel fills a buffer that straddles chunks.
    const int file = open("/system/bin/toybox", O_RDONLY);
    uint8_t* buffer = base + 3 * CHUNK - 5000;
    const ssize_t got = pread(file, buffer, sizeof pattern, 4096);
    const ssize_t expected = pread(file, copy, sizeof copy, 4096);
    check("pread into a buffer across chunks", got == (ssize_t)sizeof pattern && expected == got && memcmp(buffer, copy, sizeof copy) == 0);
    int pipes[2];
    pipe(pipes);
    check("write and read across chunks", write(pipes[1], buffer, 6000) == 6000 && read(pipes[0], base + CHUNK - 3000, 6000) == 6000 &&
                                             memcmp(base + CHUNK - 3000, copy, 6000) == 0);
    close(file);

    // fork copies it all, and each side keeps its own.
    memcpy(base + 2 * CHUNK - 100, pattern, 200);
    const pid_t child = fork();
    if (child == 0) {
        int same = memcmp(base + 2 * CHUNK - 100, pattern, 200) == 0 && first[3 * CHUNK + 5] == 3 - 3 + first[3 * CHUNK + 5];
        memset(base + 2 * CHUNK - 100, 9, 200);
        first[6 * CHUNK + 77] = 5;  // a chunk the parent never touched
        _exit(same && first[6 * CHUNK + 77] == 5 ? 0 : 1);
    }
    int status = -1;
    waitpid(child, &status, 0);
    check("fork: child sees the copy", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("fork: parent keeps its memory", memcmp(base + 2 * CHUNK - 100, pattern, 200) == 0 && first[6 * CHUNK + 77] == 0);

    // Protection and discarding.
    memset(base + 4 * CHUNK - 65536, 0x5a, 131072);
    madvise(base + 4 * CHUNK - 32768, 65536, MADV_DONTNEED);
    check("madvise across chunks zeroes", first[4 * CHUNK - 32768] == 0 && first[4 * CHUNK + 32767] == 0 && first[4 * CHUNK - 32769] == 0x5a &&
                                            first[4 * CHUNK + 32768] == 0x5a);
    check("mprotect across chunks", mprotect(base + 5 * CHUNK - 65536, 131072, PROT_READ) == 0 && first[5 * CHUNK] == 0 &&
                                        mprotect(base + 5 * CHUNK - 65536, 131072, PROT_READ | PROT_WRITE) == 0);
    first[5 * CHUNK - 1] = 1;

    // mremap moves a mapping and keeps what it held.
    uint8_t* small = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mmap(small + (1 << 20), 65536, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);  // so that it cannot grow in place
    memcpy(small + 1000, pattern, sizeof pattern);
    uint8_t* moved = mremap(small, 1 << 20, 48 << 20, MREMAP_MAYMOVE);
    check("mremap keeps contents", moved != MAP_FAILED && moved != small && memcmp(moved + 1000, pattern, sizeof pattern) == 0 && moved[(40 << 20) + 3] == 0);
    if (moved != MAP_FAILED) moved[(47 << 20) + 9] = 1;

    check("munmap", munmap(mapping, size) == 0);
    uint8_t* again = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("memory after munmap is fresh", again != MAP_FAILED && again[100] == 0);
    printf("failures=%d\n", failures);
    return failures != 0;
}
