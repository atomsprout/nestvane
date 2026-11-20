#pragma once

#include "Runtime.hpp"

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
