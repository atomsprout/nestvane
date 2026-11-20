#pragma once

#include "Runtime.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace nestvane {

// A straight-line block in the scalar SHA-256 compression path of the
// packaged Android 17 BoringSSL. All operations work on W registers.
struct AndroidSha256Block {
    static constexpr uint64_t fileOffset = 0x126014;
    static constexpr std::array<uint32_t, 19> words{
        0x0b1102d6, 0x139a1b50, 0x0b1302b5, 0x4ada3b4c, 0x0a1a0371,
        0x0a3a0293, 0x0b0902b5, 0x2a130231, 0x4a1702d3, 0x4acc2e10,
        0x13960acc, 0x0b1102b5, 0x4ad626d1, 0x0b1002b5, 0x0a13039c,
        0x0b150339, 0x4a17039c, 0x4ad13591, 0x0b1c02b5,
    };

    static const uint8_t* bind(std::span<const uint8_t> module) {
        if (module.size() < fileOffset + sizeof(words)) return nullptr;
        const auto* entry = module.data() + fileOffset;
        return std::memcmp(entry, words.data(), sizeof(words)) == 0 ? entry : nullptr;
    }

    static void executeRegisters(CPU& cpu) {
        auto& x = cpu.x;
        const auto w = [&](unsigned index) { return uint32_t(x[index]); };
        x[22] = w(22) + w(17);
        x[16] = std::rotr(w(26), 6);
        x[21] = w(21) + w(19);
        x[12] = w(26) ^ std::rotr(w(26), 14);
        x[17] = w(27) & w(26);
        x[19] = w(20) & ~w(26);
        x[21] = w(21) + w(9);
        x[17] = w(17) | w(19);
        x[19] = w(22) ^ w(23);
        x[16] = w(16) ^ std::rotr(w(12), 11);
        x[12] = std::rotr(w(22), 2);
        x[21] = w(21) + w(17);
        x[17] = w(22) ^ std::rotr(w(22), 9);
        x[21] = w(21) + w(16);
        x[28] = w(28) & w(19);
        x[25] = w(25) + w(21);
        x[28] = w(28) ^ w(23);
        x[17] = w(12) ^ std::rotr(w(17), 13);
        x[21] = w(21) + w(28);
    }

    static void execute(CPU& cpu) {
        executeRegisters(cpu);
        cpu.pc += sizeof(words);
        cpu.retired += words.size();
    }
};

inline bool androidLoopMatches(const Machine& machine, const uint8_t* bytes,
                               std::span<const uint32_t> words) {
    const auto* contiguous = machine.read(machine.cpu.pc, words.size_bytes(), 1);
    if (contiguous && contiguous == bytes)
        return std::memcmp(contiguous, words.data(), words.size_bytes()) == 0;
    // Some loops straddle a page. Check every instruction in the current mapping.
    for (uint64_t i = 1; i < words.size(); ++i) {
        const auto* word = machine.readInstruction(machine.cpu.pc + i * sizeof(uint32_t));
        if (!word || std::memcmp(word, &words[i], sizeof(uint32_t))) return false;
    }
    return std::memcmp(bytes, words.data(), sizeof(uint32_t)) == 0;
}

// ComputeModifiedUtf8Hash(const char*) in Android 17 libdexfile. Start at
// the loop header, where w9 holds the current byte and x8 the next address.
struct AndroidDexHashLoop {
    static constexpr std::string_view moduleSuffix = "/lib64/libdexfile.so";
    static constexpr uint64_t fileOffset = 0x33ff8;
    static constexpr std::array<uint32_t, 4> words{
        0x4b00012a, 0x38401509, 0x0b001540, 0x35ffffa9,
    };
    static constexpr uint64_t batchSize = 16, maxIterationInstructions = words.size();

    static uint64_t execute(Machine& machine, uint64_t iterations) {
        auto& cpu = machine.cpu;
        const auto address = cpu.x[8];
        const auto pageRemaining = pageSize - (address & (pageSize - 1));
        auto count = std::min(iterations, pageRemaining);
        if (!count) return 0;
        const auto* input = machine.read(address, count, 4);
        if (!input && count > 1) {
            count = 1;
            input = machine.read(address, count, 4);
        }
        if (!input) return 0; // The interpreter delivers the load fault at its precise PC.

        uint32_t hash = cpu.x[0], byte = cpu.x[9], difference;
        uint64_t consumed = 0;
        do {
            difference = byte - hash;
            hash = difference + (hash << 5);
            byte = input[consumed++];
        } while (byte && consumed < count);

        cpu.x[0] = hash;
        cpu.x[8] += consumed;
        cpu.x[9] = byte;
        cpu.x[10] = difference;
        if (!byte) cpu.pc += sizeof(words);
        const auto retired = consumed * words.size();
        cpu.retired += retired;
        return retired;
    }
};

// Skip font families whose cached next codepoint is beyond this page in
// FontCollection::init. The interpreter handles exhaustion and cache misses.
struct AndroidFontFamilyScan {
    static constexpr std::string_view moduleSuffix = "/system/lib64/libminikin.so";
    static constexpr uint64_t fileOffset = 0x185474;
    static constexpr std::array<uint32_t, 12> words{
        0x9100077b, 0xeb08037f, 0x54fffce2, 0x8b1b0a7c,
        0xeb1c02e9, 0xb7f809bc, 0x54000983, 0xf100113f,
        0x54000943, 0xb87b7aa9, 0xeb09031f, 0x54fffea9,
    };
    static constexpr uint64_t batchSize = 16, maxIterationInstructions = words.size();

    static uint64_t execute(Machine& machine, uint64_t iterations) {
        auto& cpu = machine.cpu;
        auto& x = cpu.x;
        uint64_t completed = 0;
        for (; completed < iterations; ++completed) {
            const uint64_t family = x[27] + 1;
            if (family >= x[8]) break;
            const uint64_t position = x[19] + (family << 2);
            if ((position >> 63) || x[23] < position || x[23] - position < sizeof(uint32_t)) break;
            const auto* cached = machine.read(x[21] + (family << 2), sizeof(uint32_t), 4);
            if (!cached) break;
            uint32_t nextCodepoint;
            std::memcpy(&nextCodepoint, cached, sizeof(nextCodepoint));
            if (x[24] > nextCodepoint) break;

            x[27] = family;
            x[28] = position;
            x[9] = nextCodepoint;
            cpu.nzcv = x[24] == nextCodepoint ? 6 : 8; // CMP x24, x9 on the taken B.LS path.
        }
        const auto retired = completed * words.size();
        cpu.retired += retired;
        return retired;
    }
};

// CheckIntraStringDataItem in Android 17 libdexfile. Handle nonzero ASCII
// bytes on the back edge; multibyte and malformed inputs use the interpreter.
struct AndroidDexAsciiVerifierLoop {
    static constexpr std::string_view moduleSuffix = "/lib64/libdexfile.so";
    static constexpr uint64_t fileOffset = 0x28508;
    static constexpr std::array<uint32_t, 53> words{
        0x710005ce, 0xaa0f03ea, 0x540006e0, 0x9100054f, 0xf900440f,
        0x39400148, 0x53047d09, 0x71003d3f, 0x54000568, 0x1ac92190,
        0x7214061f, 0x54000200, 0xb400068b, 0x9100094f, 0xf900440f,
        0x39400549, 0x121a052a, 0x7102015f, 0x540007c1, 0x12001529,
        0xd100056b, 0x331a1109, 0x34fffd49, 0x531a1108, 0x7101fd1f,
        0x54fffce8, 0x1400003a, 0x7100393f, 0x540002a1, 0xf100057f,
        0x54000449, 0x91000949, 0xf9004409, 0x39400549, 0x121a052f,
        0x710201ff, 0x54000581, 0x91000d4f, 0xf900440f, 0x3940094a,
        0x121a0550, 0x7102021f, 0x540005c1, 0x53140d08, 0x331a1528,
        0x711ffd1f, 0x540005c9, 0xd100096b, 0x17ffffd0, 0x6a0d021f,
        0x540002c1, 0x35fff9a9, 0x35fff988,
    };
    static constexpr uint64_t batchSize = 16, maxIterationInstructions = 18;

    static uint64_t execute(Machine& machine, uint64_t iterations) {
        auto& cpu = machine.cpu;
        const auto* code = machine.read(cpu.pc, sizeof(words), 1);
        if (!code || machine.read(cpu.pc, sizeof(uint32_t), 2)) return 0;

        const auto source = cpu.x[15];
        const auto destination = cpu.x[0] + 0x88;
        if (destination % sizeof(uint64_t)) return 0;
        const auto pageRemaining = pageSize - source % pageSize;
        const auto count = std::min(iterations, pageRemaining);
        if (!count) return 0;
        const auto* input = machine.read(source, count, 4);
        const auto* output = machine.read(destination, sizeof(uint64_t), 6);
        if (!input || !output) return 0;

        const auto overlapsOutput = [output](const uint8_t* bytes, size_t size) {
            const auto start = reinterpret_cast<uintptr_t>(bytes);
            const auto end = start + size;
            const auto writeStart = reinterpret_cast<uintptr_t>(output);
            return start < writeStart + sizeof(uint64_t) && writeStart < end;
        };
        // Each guest iteration stores the cursor before reading the byte.
        if (overlapsOutput(input, count) || overlapsOutput(code, sizeof(words))) return 0;

        uint32_t remaining = cpu.x[14], byte = 0, nibble = 0, shifted = 0;
        uint64_t consumed = 0, retired = 0;
        while (consumed < count && remaining != 1) {
            const uint32_t next = input[consumed];
            if (!next || next >= 0x80) break;
            const uint32_t mask = uint32_t(cpu.x[12]) << (next >> 4);
            if (mask & (0x3000 | uint32_t(cpu.x[13]))) break;
            byte = next;
            nibble = next >> 4;
            shifted = mask;
            retired += nibble ? 17 : 18;
            ++consumed;
            --remaining;
        }
        if (!consumed || !machine.store64(destination, source + consumed)) return 0;

        cpu.x[8] = byte;
        cpu.x[9] = nibble;
        cpu.x[10] = source + consumed - 1;
        cpu.x[14] = remaining;
        cpu.x[15] = source + consumed;
        cpu.x[16] = shifted;
        cpu.nzcv = 4; // The final TST found neither of the multibyte cases.
        cpu.retired += retired;
        return retired;
    }
};

// Batch the positive ASCII path of libart's signed-byte scan. A NUL,
// non-ASCII byte, or inaccessible source is handled by the interpreter.
struct AndroidArtAsciiScan {
    static constexpr std::string_view moduleSuffix = "/lib64/libart.so";
    static constexpr uint64_t fileOffset = 0x67c520;
    static constexpr std::array<uint32_t, 7> words{
        0x38c015ac, 0x7100059f, 0x1a9fa7ec, 0x5400048b,
        0xaa0e03ef, 0xd10005ce, 0xb5ffff4f,
    };
    static constexpr uint64_t batchSize = 16, maxIterationInstructions = words.size();

    static uint64_t execute(Machine& machine, uint64_t iterations) {
        if (!iterations) return 0;
        auto& cpu = machine.cpu;
        const auto address = cpu.x[13];
        const auto pageRemaining = pageSize - (address & (pageSize - 1));
        auto count = std::min({iterations - 1, pageRemaining - 1, cpu.x[14]}) + 1;
        const auto* input = machine.read(address, count, 4);
        if (!input && count > 1) {
            count = 1;
            input = machine.read(address, count, 4);
        }
        if (!input) return 0;

        uint64_t consumed = 0;
        while (consumed < count && input[consumed] > 0 && input[consumed] < 0x80) ++consumed;
        if (!consumed) return 0;

        cpu.x[12] = 0; // CSET w12, LT is false for positive ASCII.
        cpu.x[13] += consumed;
        cpu.x[15] = cpu.x[14] - (consumed - 1);
        cpu.x[14] -= consumed;
        cpu.nzcv = input[consumed - 1] == 1 ? 6 : 2; // CMP w12, #1 before CSET.
        if (!cpu.x[15]) cpu.pc += sizeof(words);
        const auto retired = consumed * words.size();
        cpu.retired += retired;
        return retired;
    }
};

// Fast path for an already marked heap bitmap bit in the Android 17
// copying collector. The interpreter handles first-time marks and faults.
struct AndroidArtGcBitmap {
    static constexpr std::string_view moduleSuffix = "/lib64/libart.so";
    static constexpr uint64_t fileOffset = 0x835b7c;
    static constexpr std::array<uint32_t, 9> words{
        0x5280002b, 0xf868792c, 0x9aca216b, 0xea0c016a, 0x54000061,
        0xaa0c016b, 0xf828792b, 0xf100015f, 0x1a9f07e0,
    };
    static constexpr uint64_t batchSize = 1, maxIterationInstructions = words.size() - 2;

    static uint64_t execute(Machine& machine, uint64_t) {
        auto& cpu = machine.cpu;
        const auto address = cpu.x[9] + (cpu.x[8] << 3);
        const uint64_t mask = uint64_t{1} << (cpu.x[10] & 63);
        uint64_t bitmap;
        if (!machine.load64(address, bitmap) || !(bitmap & mask)) return 0;

        cpu.x[0] = 1;
        cpu.x[10] = mask;
        cpu.x[11] = mask;
        cpu.x[12] = bitmap;
        cpu.nzcv = (mask >> 63 ? 8 : 0) | 2;
        cpu.pc += sizeof(words);
        cpu.retired += maxIterationInstructions;
        return maxIterationInstructions;
    }
};

// The canary-checked return block of a frequently sampled Android 17 ART
// collector routine. A bad canary or unreadable stack stays on the guest path.
struct AndroidArtGcReturn {
    static constexpr std::string_view moduleSuffix = "/lib64/libart.so";
    static constexpr uint64_t fileOffset = 0x838dcc;
    static constexpr std::array<uint32_t, 11> words{
        0xf94016c8, 0xf85f83a9, 0xeb09011f, 0x54000aa1, 0xa9474ff4, 0xf9402bf7,
        0xa94657f6, 0xa9447bfd, 0x910203ff, 0xd50323bf, 0xd65f03c0,
    };
    static constexpr uint64_t batchSize = 1, maxIterationInstructions = words.size();

    static uint64_t execute(Machine& machine, uint64_t) {
        auto& cpu = machine.cpu;
        uint64_t expectedCanary, stackCanary;
        if (!machine.load64(cpu.x[22] + 0x28, expectedCanary) ||
            !machine.load64(cpu.x[29] - 8, stackCanary) || expectedCanary != stackCanary) return 0;
        std::array<uint64_t, 8> saved;
        const auto* frame = machine.read(cpu.sp + 0x40, sizeof(saved), 4);
        if (!frame) return 0;
        std::memcpy(saved.data(), frame, sizeof(saved));

        cpu.x[8] = expectedCanary;
        cpu.x[9] = stackCanary;
        cpu.x[29] = saved[0];
        cpu.x[30] = saved[1];
        cpu.x[23] = saved[2];
        cpu.x[22] = saved[4];
        cpu.x[21] = saved[5];
        cpu.x[20] = saved[6];
        cpu.x[19] = saved[7];
        cpu.nzcv = 6;
        cpu.sp += 0x80;
        // Guest PAC is disabled, so AUTIASP leaves the guest return PC unchanged.
        cpu.pc = cpu.x[30];
        cpu.retired += words.size();
        return words.size();
    }
};

inline constexpr uint64_t bionicMemoryStep = 64;

inline uint64_t bionicMemoryIterations(uint64_t remaining) {
    return std::max(uint64_t{1}, remaining / bionicMemoryStep + (remaining % bionicMemoryStep != 0));
}

inline uint64_t finishBionicMemoryLoop(CPU& cpu, uint64_t iterations, uint64_t instructions) {
    const auto previous = cpu.x[2] - (iterations - 1) * bionicMemoryStep;
    const auto remaining = previous - bionicMemoryStep;
    cpu.x[2] = remaining;
    cpu.x[3] += iterations * bionicMemoryStep;
    cpu.nzcv = ((remaining >> 63) << 3) | (remaining == 0 ? 4 : 0) |
               (previous >= bionicMemoryStep ? 2 : 0) | (((previous ^ remaining) & previous) >> 63);
    if (previous <= bionicMemoryStep) cpu.pc += instructions * sizeof(uint32_t);
    const auto retired = iterations * instructions;
    cpu.retired += retired;
    return retired;
}

// Bionic's portable __memset_aarch64 loop with vector pair stores.
struct AndroidBionicFillLoop {
    static constexpr std::string_view moduleSuffix = "/lib64/bionic/libc.so";
    static constexpr uint64_t fileOffset = 0x6f3c0;
    static constexpr std::array<uint32_t, 5> words{
        0xad010060, 0xad020060, 0x91010063, 0xf1010042, 0x54ffff88,
    };
    static constexpr uint64_t batchSize = 16, maxIterationInstructions = words.size();

    static uint64_t execute(Machine& machine, uint64_t iterations) {
        auto& cpu = machine.cpu;
        // Keep the fast write in one guest page. The interpreter handles stores
        // crossing a page or encountering a fault, including partial pair stores.
        if (machine.read(cpu.pc, sizeof(uint32_t), 2)) return 0;
        const auto destination = cpu.x[3] + 32;
        const auto pageRemaining = pageSize - (destination & (pageSize - 1));
        const auto count = std::min({iterations, batchSize, bionicMemoryIterations(cpu.x[2]),
                                     pageRemaining / bionicMemoryStep});
        if (!count) return 0;

        std::array<uint8_t, batchSize * bionicMemoryStep> buffer;
        const auto written = count * bionicMemoryStep;
        const auto& pattern = cpu.v[0];
        for (uint64_t offset = 0; offset < written; offset += pattern.size())
            std::memcpy(buffer.data() + offset, pattern.data(), pattern.size());
        if (!machine.write(destination, std::span(buffer).first(written))) return 0;

        return finishBionicMemoryLoop(cpu, count, words.size());
    }
};

// Bionic's forward __memmove_aarch64 loop pipelines 64 bytes in x6-x13.
struct AndroidBionicCopyLoop {
    static constexpr std::string_view moduleSuffix = "/lib64/bionic/libc.so";
    static constexpr uint64_t fileOffset = 0x6f080;
    static constexpr std::array<uint32_t, 10> words{
        0xa9011c66, 0xa9411c26, 0xa9022468, 0xa9422428, 0xa9032c6a,
        0xa9432c2a, 0xa984346c, 0xa9c4342c, 0xf1010042, 0x54fffee8,
    };
    static constexpr uint64_t batchSize = 16, maxIterationInstructions = words.size();

    static uint64_t execute(Machine& machine, uint64_t iterations) {
        auto& cpu = machine.cpu;
        if (machine.read(cpu.pc, sizeof(uint32_t), 2)) return 0;
        const auto source = cpu.x[1] + 16;
        const auto destination = cpu.x[3] + 16;
        const auto pageRemaining = pageSize - std::max(source % pageSize, destination % pageSize);
        const auto count = std::min({iterations, batchSize, bionicMemoryIterations(cpu.x[2]),
                                     pageRemaining / bionicMemoryStep});
        if (!count) return 0;
        const auto length = count * bionicMemoryStep;
        const auto overlap = [length](uint64_t a, uint64_t b) {
            return std::max(a, b) - std::min(a, b) < length;
        };
        if (overlap(machine.dataAddress(source), machine.dataAddress(destination))) return 0;

        const auto* input = machine.read(source, length, 4);
        const auto* output = machine.read(destination, length, 6);
        // The same shared pages can appear at distinct guest addresses.
        if (!input || !output || overlap(reinterpret_cast<uintptr_t>(input), reinterpret_cast<uintptr_t>(output)))
            return 0;
        std::array<uint8_t, batchSize * bionicMemoryStep> buffer;
        std::array<uint64_t, 8> pending;
        std::memcpy(buffer.data(), &cpu.x[6], bionicMemoryStep);
        std::memcpy(buffer.data() + bionicMemoryStep, input, length - bionicMemoryStep);
        std::memcpy(pending.data(), input + length - bionicMemoryStep, bionicMemoryStep);
        if (!machine.write(destination, std::span(buffer).first(length))) return 0;
        std::ranges::copy(pending, cpu.x.begin() + 6);
        cpu.x[1] += length;
        return finishBionicMemoryLoop(cpu, count, words.size());
    }
};

struct AndroidBootAotBlock {
    uint64_t fileOffset;
    std::span<const uint32_t> words;
    void (*execute)(CPU&);
};

} // namespace nestvane

#if __has_include("../generated/AndroidBootAotBlocks.hpp")
#include "../generated/AndroidBootAotBlocks.hpp"
#else
namespace nestvane {
inline constexpr std::array<AndroidBootAotBlock, 0> androidBootGeneratedBlocks{};
}
#endif

namespace nestvane {

class AndroidBootAot {
public:
    void bind(std::span<const uint8_t> module) {
        if (module.size() <= firstOffset) return;
        for (const auto& block : androidBootGeneratedBlocks)
            bindBlock(module, block);
        if (androidBootGeneratedBlocks.empty()) bindBlock(module, fallbackBlock);
    }

    const AndroidBootAotBlock* lookup(const uint8_t* bytes) const {
        const auto address = reinterpret_cast<uintptr_t>(bytes);
        if (!base || address < base) return nullptr;
        const auto offset = address - base;
        return offset < index.size() * sizeof(uint32_t) && offset % sizeof(uint32_t) == 0
            ? index[offset / sizeof(uint32_t)] : nullptr;
    }

    explicit operator bool() const { return base != 0; }

private:
    static constexpr uint64_t firstOffset = 0x125dc0, endOffset = 0x126be8;
    static constexpr AndroidBootAotBlock fallbackBlock{
        AndroidSha256Block::fileOffset, AndroidSha256Block::words, AndroidSha256Block::executeRegisters};

    void bindBlock(std::span<const uint8_t> module, const AndroidBootAotBlock& block) {
        if (block.fileOffset < firstOffset || block.fileOffset >= endOffset ||
            (block.fileOffset - firstOffset) % sizeof(uint32_t) ||
            block.fileOffset > module.size() || block.words.size_bytes() > module.size() - block.fileOffset)
            return;
        if (std::memcmp(module.data() + block.fileOffset, block.words.data(), block.words.size_bytes())) return;
        base = reinterpret_cast<uintptr_t>(module.data() + firstOffset);
        index[(block.fileOffset - firstOffset) / sizeof(uint32_t)] = &block;
    }

    uintptr_t base = 0;
    std::array<const AndroidBootAotBlock*, (endOffset - firstOffset) / sizeof(uint32_t)> index{};
};

} // namespace nestvane
