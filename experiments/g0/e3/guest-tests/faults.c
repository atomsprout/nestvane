// Exercises fault handling the way ART uses it: a SIGSEGV handler that reads siginfo and the
// ucontext, then resumes somewhere else by changing pc, on the normal and the alternate stack.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static volatile int handled;
static volatile unsigned long last_address;
static volatile int last_code;
static sigjmp_buf escape;
static volatile int mode;  // 0 skip the instruction, 1 redirect to landing, 2 siglongjmp

static void landing(void) { ++handled; }

static void on_fault(int number, siginfo_t* info, void* raw) {
    ucontext_t* context = raw;
    last_address = (unsigned long)info->si_addr;
    last_code = info->si_code;
    if (number != SIGSEGV) _exit(3);
    if (mode == 0) {
        ++handled;
        context->uc_mcontext.pc += 4;
    } else if (mode == 1) {
        // What ART does for a null check: continue in another function instead of the faulting one.
        // The link register is untouched, so landing() returns to whoever called the faulting function.
        context->uc_mcontext.pc = (unsigned long)&landing;
    } else {
        ++handled;
        siglongjmp(escape, 1);
    }
}

__attribute__((noinline)) static long load(volatile long* pointer) { return *pointer; }
__attribute__((noinline)) static void store(volatile long* pointer) { *pointer = 1; }

static long recurse(long depth) {
    volatile char frame[4096];
    frame[0] = (char)depth;
    return depth == 0 ? frame[0] : recurse(depth - 1) + frame[0];
}

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
    stack_t stack = {malloc(1 << 16), 0, 1 << 16};
    sigaltstack(&stack, NULL);

    volatile long* read_only = mmap(NULL, 16384, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    volatile long* none = mmap(NULL, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    mode = 0;
    for (int index = 0; index < 1000; ++index) load((volatile long*)0x40);
    printf("null load: handled=%d address=0x%lx code=%d\n", handled, last_address, last_code);
    handled = 0;
    for (int index = 0; index < 1000; ++index) store(read_only);
    printf("read-only store: handled=%d address_ok=%d code=%d value=%ld\n", handled, last_address == (unsigned long)read_only, last_code, read_only[0]);
    handled = 0;
    for (int index = 0; index < 1000; ++index) load(none + 3);
    printf("protected load: handled=%d address_ok=%d code=%d\n", handled, last_address == (unsigned long)(none + 3), last_code);

    mode = 1;
    handled = 0;
    for (int index = 0; index < 1000; ++index) load((volatile long*)0x8);
    printf("redirected: landing_calls=%d\n", handled);

    mode = 2;
    handled = 0;
    for (int index = 0; index < 100; ++index)
        if (sigsetjmp(escape, 1) == 0) store((volatile long*)0x10);
    printf("siglongjmp: handled=%d\n", handled);

    // Run out of stack: the handler has to run on the alternate stack.
    handled = 0;
    if (sigsetjmp(escape, 1) == 0) recurse(1 << 20);
    printf("stack overflow: handled=%d\n", handled);
    return 0;
}
