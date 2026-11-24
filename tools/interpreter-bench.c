// The code the interpreter benchmark runs (tools/bench-interpreter.cpp): ordinary compiled C of the
// kind an app's library is made of, a small bytecode machine, hashing, sorting and a linked list.
// It touches nothing outside the buffer it is given and calls no library, so the same bytes run on
// this Mac's processor and in the interpreter. Built with -ffreestanding -fno-builtin; every function
// is in one section so that the code can be copied out whole.
#include <stdint.h>

#define BENCH __attribute__((section("__TEXT,__bench"), noinline))

// A stack machine with a switch, like a script engine's main loop.
BENCH static int64_t run_bytecode(const uint8_t* code, int64_t* stack, int64_t rounds) {
    int64_t total = 0;
    for (int64_t round = 0; round < rounds; ++round) {
        int64_t* top = stack;
        const uint8_t* pc = code;
        for (;;) {
            switch (*pc++) {
                case 0: *top++ = (int8_t)*pc++; break;            // push a small constant
                case 1: top[-2] += top[-1]; --top; break;         // add
                case 2: top[-2] *= top[-1]; --top; break;         // multiply
                case 3: top[-2] ^= top[-1] >> 3; --top; break;    // mix
                case 4: *top = top[-1]; ++top; break;             // duplicate
                case 5: if (top[-1] & 1) pc += 2; break;          // skip when odd
                case 6: total += *--top; goto done;               // return
                default: --top; break;
            }
        }
    done:;
    }
    return total;
}

// FNV-1a over the buffer, a byte at a time.
BENCH static uint64_t hash_bytes(const uint8_t* bytes, int64_t length) {
    uint64_t hash = 1469598103934665603ULL;
    for (int64_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

// Insertion sort of 32-bit numbers.
BENCH static void sort_numbers(int32_t* numbers, int64_t count) {
    for (int64_t i = 1; i < count; ++i) {
        const int32_t value = numbers[i];
        int64_t j = i - 1;
        while (j >= 0 && numbers[j] > value) {
            numbers[j + 1] = numbers[j];
            --j;
        }
        numbers[j + 1] = value;
    }
}

struct node { struct node* next; int64_t value; };

BENCH static int64_t walk_list(struct node* nodes, int64_t count, int64_t rounds) {
    for (int64_t i = 0; i < count; ++i) {
        nodes[i].next = &nodes[(i * 7 + 3) % count];
        nodes[i].value = i * 3;
    }
    int64_t total = 0;
    struct node* at = nodes;
    for (int64_t step = 0; step < count * rounds; ++step) {
        total += at->value;
        at = at->next;
    }
    return total;
}

// Floating point, as in a physics step or a mixer: a few bodies pulled together, in double, and a
// filter over float samples.
BENCH static int64_t step_bodies(double* state, float* samples, int64_t rounds) {
    for (int i = 0; i < 16; ++i) {
        state[i * 4] = i * 0.5;
        state[i * 4 + 1] = (i * 7 % 5) * 0.25;
        state[i * 4 + 2] = state[i * 4 + 3] = 0;
    }
    for (int64_t round = 0; round < rounds; ++round) {
        for (int i = 0; i < 16; ++i) {
            double fx = 0, fy = 0;
            for (int j = 0; j < 16; ++j) {
                const double dx = state[j * 4] - state[i * 4], dy = state[j * 4 + 1] - state[i * 4 + 1];
                const double d = dx * dx + dy * dy + 0.01;
                fx += dx / d;
                fy += dy / d;
            }
            state[i * 4 + 2] += fx * 0.001;
            state[i * 4 + 3] += fy * 0.001;
        }
        for (int i = 0; i < 16; ++i) {
            state[i * 4] += state[i * 4 + 2];
            state[i * 4 + 1] += state[i * 4 + 3];
        }
    }
    float level = 0;
    for (int64_t i = 0; i < 4096 * rounds / 8; ++i) {
        const float sample = (float)((i * 37) % 200 - 100) * 0.01f;
        level = level * 0.95f + sample * 0.05f;
        samples[i & 4095] = level;
    }
    int64_t sum = (int64_t)(level * 1000);
    for (int i = 0; i < 64; ++i) sum += (int64_t)(state[i] * 1000);
    return sum;
}

// The whole benchmark on a buffer of at least 1 MiB. Returns a number that depends on every part.
BENCH int64_t interpreter_bench(uint8_t* buffer, int64_t scale) {
    uint8_t* code = buffer;
    // Written a byte at a time: a constant array would be data outside the copied code.
    uint64_t program = 0x0300020401070005ULL, more = 0x0201090001010005ULL;
    for (unsigned i = 0; i < 8; ++i) code[i] = (uint8_t)(program >> (8 * i));
    code[8] = 3;
    for (unsigned i = 0; i < 8; ++i) code[9 + i] = (uint8_t)(more >> (8 * i));
    code[17] = 1;
    code[18] = 6;
    int64_t* stack = (int64_t*)(buffer + 4096);
    int64_t result = run_bytecode(code, stack, 2000 * scale);

    uint8_t* bytes = buffer + 8192;
    for (int64_t i = 0; i < 65536; ++i) bytes[i] = (uint8_t)(i * 31 + (i >> 7));
    for (int64_t i = 0; i < scale; ++i) result ^= (int64_t)hash_bytes(bytes, 65536);

    int32_t* numbers = (int32_t*)(buffer + 8192 + 65536);
    uint32_t seed = 12345;
    for (int64_t i = 0; i < 600; ++i) {
        seed = seed * 1103515245 + 12345;
        numbers[i] = (int32_t)(seed >> 8);
    }
    for (int64_t i = 0; i < scale; ++i) {
        sort_numbers(numbers, 600);
        result += numbers[i % 600];
        numbers[(i * 13) % 600] ^= (int32_t)i * 977;
    }

    struct node* nodes = (struct node*)(buffer + 262144);
    result += walk_list(nodes, 4096, 4 * scale);
    result += step_bodies((double*)(buffer + 196608), (float*)(buffer + 229376), 4 * scale);
    return result;
}
