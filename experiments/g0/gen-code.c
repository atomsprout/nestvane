// Writes a blob of valid, position-independent ARM64 code for the E5 load probe.
// Every 4 KiB block is 1,023 register-only instructions on x0-x15 followed by ret.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: gen-code <output> <mebibytes> <seed>\n");
        return 2;
    }
    FILE* output = fopen(argv[1], "wb");
    if (!output) return 1;
    const uint64_t blocks = strtoull(argv[2], NULL, 10) * 256;
    uint64_t state = strtoull(argv[3], NULL, 10) * 0x9e3779b97f4a7c15ull + 1;
    static const uint32_t opcodes[] = {0x8b000000u /* add */, 0xcb000000u /* sub */, 0xca000000u /* eor */, 0xaa000000u /* orr */};
    uint32_t block[1024];
    for (uint64_t index = 0; index < blocks; ++index) {
        for (int slot = 0; slot < 1023; ++slot) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            const uint32_t bits = (uint32_t)(state >> 20);
            block[slot] = opcodes[bits & 3] | ((bits >> 2 & 15) << 16) | ((bits >> 6 & 63) << 10) |
                          ((bits >> 12 & 15) << 5) | (bits >> 16 & 15);
        }
        block[1023] = 0xd65f03c0u;
        if (fwrite(block, sizeof block, 1, output) != 1) return 1;
    }
    return fclose(output) != 0;
}
