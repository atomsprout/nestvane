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
    static constexpr uint64_t batchSize = 16;

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
    static constexpr uint64_t batchSize = 16;

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
    static constexpr uint64_t batchSize = 16;

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
