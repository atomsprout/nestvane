#include "AArch64.hpp"
#include "AndroidBootAot.hpp"
#include "BpfProgram.hpp"
#include "GfxstreamPipe.hpp"
#include "ProbeLinux.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <sstream>
#include <sqlite3.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#include <random>
#endif


using namespace nestvane;

#if defined(__APPLE__) && defined(__aarch64__)
__attribute__((target("sha2")))
static uint32x4_t nativeSha256Instruction(unsigned instruction, uint32x4_t dest, uint32x4_t first, uint32x4_t second) {
    switch (instruction) {
        case 0: asm("sha256h %q0, %q1, %2.4s" : "+w"(dest) : "w"(first), "w"(second)); break;
        case 1: asm("sha256h2 %q0, %q1, %2.4s" : "+w"(dest) : "w"(first), "w"(second)); break;
        case 2: asm("sha256su0 %0.4s, %1.4s" : "+w"(dest) : "w"(first)); break;
        case 3: asm("sha256su1 %0.4s, %1.4s, %2.4s" : "+w"(dest) : "w"(first), "w"(second)); break;
    }
    return dest;
}

#endif

int main() {
#if defined(__APPLE__) && defined(__aarch64__)
    if (hasNativeSha256()) {
        constexpr std::array<uint32_t, 4> opcodes{0x5e004000, 0x5e005000, 0x5e282800, 0x5e006000};
        std::mt19937_64 random(92485);
        Machine machine(Image{0x4000, {}, {}});
        for (unsigned i = 0; i < 2048; ++i) {
            for (unsigned op = 0; op < opcodes.size(); ++op) {
                CPU initial;
                initial.pc = 0x4000;
                initial.retired = i;
                initial.nzcv = i & 15;
                initial.fpsr = random();
                initial.exclusiveAddress = 0x6000;
                initial.exclusiveSize = 8;
                for (auto& vector : initial.v)
                    for (auto& value : vector) value = random();
                const auto rd = unsigned(i & 31);
                const auto rn = (i & 64) ? rd : unsigned(random() & 31);
                const auto rm = (i & 128) ? rd : unsigned(random() & 31);
                const auto lane = [&](unsigned reg) {
                    uint32x4_t value;
                    std::memcpy(&value, initial.v[reg].data(), sizeof(value));
                    return value;
                };
                const auto value = nativeSha256Instruction(op, lane(rd), lane(rn), lane(rm));
                const auto word = opcodes[op] | (rn << 5) | rd | (op == 2 ? 0 : rm << 16);
                machine.cpu = initial;
                assert(executeStartupInstruction(machine, word));
                std::memcpy(initial.v[rd].data(), &value, sizeof(value));
                initial.pc += 4;
                ++initial.retired;
                assert(machine.cpu == initial);
            }
        }
    }
#endif
    {
        // The guest may split its mapping at either a host page or a smaller guest page.
        const size_t hostPage = getpagesize();
        DenseBytes source(std::vector<uint8_t>(64 * hostPage, 0x6a));
        for (const auto offset : {hostPage, hostPage + size_t(pageSize / 2)}) {
            auto copy = source.copyRange(offset, 32 * hostPage);
            assert(copy.size() == 32 * hostPage && copy.data()[0] == 0x6a && copy.data()[copy.size() - 1] == 0x6a);
            source.mutableData()[offset] = 0xcb;
            assert(copy.data()[0] == 0x6a);
            copy.mutableData()[copy.size() - 1] = 0xb5;
            assert(source.data()[offset + copy.size() - 1] == 0x6a);
            auto nested = copy.copyRange(3, copy.size() - 3);
            assert(nested.data()[0] == 0x6a && nested.data()[nested.size() - 1] == 0xb5);
            auto nestedFork = nested;
            nestedFork.mutableData()[0] = 0x35;
            assert(copy.data()[3] == 0x6a && nested.data()[0] == 0x6a);
            source.mutableData()[offset] = 0x6a;
        }
        auto shortCopy = source.copyRange(hostPage - 2, 5);
        assert(shortCopy.size() == 5 && shortCopy.data()[4] == 0x6a);
    }
    {
        const Startup startup{0x4000, 0, 0, 0, 0, "/bin/crc", {}, 0};
        Machine machine(Image{0x4000, {}, startup});
        // One argv entry, no environment, followed by key-value pairs.
        uint64_t hwcap = 0;
        for (uint64_t entry = machine.cpu.sp + 4 * sizeof(uint64_t); ; entry += 16) {
            uint64_t key;
            assert(machine.load64(entry, key));
            if (key == 0) break;
            if (key == 16) {
                assert(machine.load64(entry + sizeof(key), hwcap));
                break;
            }
        }
        const auto* sha2 = std::getenv("NESTVANE_GUEST_SHA2");
        const bool accelerated = sha2 && std::string_view(sha2) == "1" && hasNativeSha256();
        assert(hwcap == (accelerated ? 0xc2 : 0x80));
        for (const bool castagnoli : {false, true}) {
            const uint32_t polynomial = castagnoli ? 0x82f63b78 : 0xedb88320;
            for (unsigned width = 0; width < 4; ++width) {
                const uint32_t word = 0x1ac24020 | (width << 10) |
                    (castagnoli ? 0x1000U : 0U) | (width == 3 ? 0x80000000U : 0U);
                for (uint64_t i = 0; i < 64; ++i) {
                    CPU initial;
                    initial.pc = 0x4000;
                    initial.nzcv = i & 15;
                    initial.x[0] = UINT64_MAX;
                    initial.x[1] = 0xffff0000d3456789ULL ^ (i * 0x1000007);
                    initial.x[2] = 0x0fedcba987654321ULL * i;
                    uint32_t checksum = initial.x[1];
                    for (unsigned bit = 0; bit < (8U << width); ++bit) {
                        const bool feedback = (checksum ^ (initial.x[2] >> bit)) & 1;
                        checksum = (checksum >> 1) ^ (feedback ? polynomial : 0);
                    }
                    machine.cpu = initial;
                    assert(executeStartupInstruction(machine, word));
                    initial.x[0] = checksum;
                    initial.pc += sizeof(word);
                    ++initial.retired;
                    assert(machine.cpu == initial);
                }
            }
        }
    }
    {
        std::vector<uint8_t> module(AndroidSha256Block::fileOffset + sizeof(AndroidSha256Block::words));
        auto* entry = module.data() + AndroidSha256Block::fileOffset;
        std::memcpy(entry, AndroidSha256Block::words.data(), sizeof(AndroidSha256Block::words));
        assert(AndroidSha256Block::bind(module) == entry);
        entry[5] ^= 1;
        assert(!AndroidSha256Block::bind(module));
        uint64_t state = 0x8123456789abcdefULL;
        Machine baseline(Image{0x8000, {}, {}});
        const auto compare = [&](const AndroidBootAotBlock& block) {
            for (unsigned example = 0; example < 64; ++example) {
                CPU native;
                native.pc = 0x8000;
                native.retired = example * 4;
                native.nzcv = example & 15;
                native.fpsr = example & fpsrMask;
                for (auto& value : native.x) {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    value = state;
                }
                baseline.cpu = native;
                for (auto word : block.words) {
                    const auto instruction = decode(word);
                    if (instruction.op == Op::unsupported) {
                        assert(executeStartupInstruction(baseline, word));
                    } else {
                        executeDecoded(baseline, instruction);
                    }
                }
                block.execute(native);
                native.pc += block.words.size_bytes();
                native.retired += block.words.size();
                assert(baseline.cpu == native);
            }
        };
        compare({AndroidSha256Block::fileOffset, AndroidSha256Block::words, AndroidSha256Block::executeRegisters});
        for (const auto& block : androidBootGeneratedBlocks) compare(block);
    }
    {
        constexpr uint64_t entry = 0x4ff8, data = 0x7000;
        constexpr auto words = AndroidDexHashLoop::words;
        std::vector<uint8_t> code(sizeof(words)), input(pageSize);
        std::memcpy(code.data(), words.data(), code.size());
        const Image image{entry, {
            {entry, std::vector<uint8_t>(code.begin(), code.begin() + 8), 5},
            {entry + 8, std::vector<uint8_t>(code.begin() + 8, code.end()), 5},
            {data, input, 6},
        }, {}};
        Machine native(image), reference(image);
        const auto* instruction = native.readInstruction(entry);
        assert(androidLoopMatches(native, instruction, words));
        native.segments[1].flags = 4;
        assert(!androidLoopMatches(native, instruction, words));
        native.segments[1].flags = 5;
        for (unsigned example = 0; example < 256; ++example) {
            CPU initial;
            initial.pc = entry;
            initial.retired = 4 * example;
            initial.nzcv = example & 15;
            initial.x[0] = 0xffff000000000000ULL | (0xffffffffU - example * 31);
            initial.x[8] = example % 2 ? data + pageSize - 2 : data;
            initial.x[9] = 0xabcd000000000000ULL | example;
            for (size_t i = 0; i < 32; ++i)
                input[i] = i == example % 32 ? 0 : 1 + ((example * 31 + i * 17) % 255);
            input[pageSize - 2] = input[0];
            input[pageSize - 1] = input[1];
            assert(native.write(data, input) && reference.write(data, input));
            native.cpu = reference.cpu = initial;
            const auto retired = AndroidDexHashLoop::execute(native, 1 + example % 16);
            assert(retired && retired <= (1 + example % 16) * words.size());
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
        }
        native.cpu.pc = entry;
        native.cpu.x[8] = data + pageSize;
        const CPU beforeFault = native.cpu;
        assert(!AndroidDexHashLoop::execute(native, 16) && native.cpu == beforeFault);
    }
    {
        using Loop = AndroidFontFamilyScan;
        constexpr uint64_t entry = 0x4474, cache = 0x8000;
        std::vector<uint8_t> code(sizeof(Loop::words)), bytes(pageSize);
        std::memcpy(code.data(), Loop::words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {cache, bytes, 6}}, {}};
        Machine native(image), reference(image);
        for (unsigned example = 0; example < 256; ++example) {
            CPU cpu;
            cpu.pc = entry;
            cpu.retired = example;
            cpu.nzcv = example & 15;
            cpu.x[8] = 30;
            cpu.x[19] = 0x3000;
            cpu.x[21] = cache;
            cpu.x[23] = 0x3100;
            cpu.x[24] = 0x400;
            cpu.x[27] = example % 10;
            const auto stop = cpu.x[27] + 1 + example % 20;
            for (size_t i = 0; i < 32; ++i) {
                const uint32_t value = i == stop ? 0x3ff : 0x400 + (i + example) % 2;
                std::memcpy(bytes.data() + i * sizeof(value), &value, sizeof(value));
            }
            assert(native.write(cache, bytes) && reference.write(cache, bytes));
            native.cpu = reference.cpu = cpu;
            const auto iterations = 1 + example % Loop::batchSize;
            const auto expected = std::min({iterations, stop - cpu.x[27] - 1, cpu.x[8] - cpu.x[27] - 1});
            const auto retired = Loop::execute(native, iterations);
            assert(retired == expected * Loop::words.size());
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = Loop::words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
        }
        const auto rejects = [&](CPU cpu) {
            native.cpu = cpu;
            assert(!Loop::execute(native, Loop::batchSize) && native.cpu == cpu);
        };
        auto cpu = native.cpu;
        cpu.x[27] = 0;
        cpu.x[21] = cache - 8;
        rejects(cpu);
        cpu.x[21] = cache;
        cpu.x[23] = cpu.x[19] + 7;
        rejects(cpu);
        cpu.x[19] = uint64_t{1} << 63;
        cpu.x[23] = UINT64_MAX;
        rejects(cpu);
    }
    {
        using Loop = AndroidDexAsciiVerifierLoop;
        constexpr uint64_t entry = 0x4508, input = 0x6000, object = 0x8000, cursor = object + 0x88;
        std::vector<uint8_t> code(sizeof(Loop::words)), bytes(pageSize, 0x41), state(pageSize);
        std::memcpy(code.data(), Loop::words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {input, bytes, 6}, {object, state, 6}}, {}};
        Machine native(image), reference(image);
        const auto* instruction = native.readInstruction(entry);
        assert(androidLoopMatches(native, instruction, Loop::words));
        for (unsigned example = 0; example < 256; ++example) {
            CPU initial;
            initial.pc = entry;
            initial.retired = example;
            initial.nzcv = example & 15;
            initial.x[0] = object;
            initial.x[12] = 1;
            initial.x[13] = 0x8f00;
            initial.x[14] = 2 + example % 23;
            initial.x[15] = input + (example % 2 ? pageSize - 3 : 0);
            initial.exclusiveAddress = cursor;
            initial.exclusiveSize = 8;
            const auto length = std::min<size_t>(32, pageSize - (initial.x[15] - input));
            for (size_t i = 0; i < length; ++i)
                bytes[i] = 1 + (i * 17 + example * 31) % 127;
            if (example % 4 == 0) bytes[1] = 0;
            if (example % 4 == 1) bytes[2] = 0xc3;
            assert(native.write(initial.x[15], std::span(bytes).first(length)));
            assert(reference.write(initial.x[15], std::span(bytes).first(length)));
            native.cpu = reference.cpu = initial;
            const auto iterations = 1 + example % Loop::batchSize;
            const auto retired = Loop::execute(native, iterations);
            assert(retired >= 17 && retired <= iterations * Loop::maxIterationInstructions);
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = Loop::words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
            uint64_t observed, expected;
            assert(native.load64(cursor, observed) && reference.load64(cursor, expected) && observed == expected);
        }
        native.cpu.x[14] = 1;
        const auto lastCharacter = native.cpu;
        assert(!Loop::execute(native, 16) && native.cpu == lastCharacter);
        native.cpu.x[14] = 20;
        native.cpu.x[15] = cursor;
        const auto aliased = native.cpu;
        assert(!Loop::execute(native, 16) && native.cpu == aliased);
        native.cpu.x[15] = input;
        const auto objectSegment = std::ranges::find(native.segments, object, &Segment::address);
        assert(objectSegment != native.segments.end());
        objectSegment->flags = 4;
        const auto writeFault = native.cpu;
        assert(!Loop::execute(native, 16) && native.cpu == writeFault);
        objectSegment->flags = 6;
        native.cpu.x[15] = input + pageSize;
        const auto readFault = native.cpu;
        assert(!Loop::execute(native, 16) && native.cpu == readFault);
        const auto codeSegment = std::ranges::find(native.segments, entry, &Segment::address);
        assert(codeSegment != native.segments.end());
        codeSegment->flags = 7;
        native.cpu.x[15] = input;
        const auto mutableCode = native.cpu;
        assert(!Loop::execute(native, 16) && native.cpu == mutableCode);
    }
    {
        using Loop = AndroidArtAsciiScan;
        constexpr uint64_t entry = 0x4520, input = 0x6000;
        std::vector<uint8_t> code(sizeof(Loop::words)), bytes(pageSize, 0x41);
        std::memcpy(code.data(), Loop::words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {input, bytes, 6}}, {}};
        Machine native(image), reference(image);
        assert(androidLoopMatches(native, native.readInstruction(entry), Loop::words));
        for (unsigned example = 0; example < 256; ++example) {
            CPU initial;
            initial.pc = entry;
            initial.retired = example;
            initial.nzcv = example & 15;
            initial.x[12] = UINT64_MAX;
            initial.x[13] = input + (example % 2 ? pageSize - 3 : 0);
            initial.x[14] = example % 23;
            initial.x[15] = UINT64_MAX;
            initial.exclusiveAddress = input;
            initial.exclusiveSize = 8;
            const auto length = std::min<size_t>(32, pageSize - (initial.x[13] - input));
            for (size_t i = 0; i < length; ++i) bytes[i] = 1 + (i * 17 + example * 31) % 127;
            if (example % 4 == 0) bytes[0] = 0;
            if (example % 4 == 1) bytes[1] = 0xc3;
            assert(native.write(initial.x[13], std::span(bytes).first(length)));
            assert(reference.write(initial.x[13], std::span(bytes).first(length)));
            native.cpu = reference.cpu = initial;
            const auto iterations = 1 + example % Loop::batchSize;
            const auto retired = Loop::execute(native, iterations);
            const uint64_t maximum = std::min({iterations, uint64_t(length), initial.x[14] + 1});
            uint64_t expected = 0;
            while (expected < maximum && bytes[expected] > 0 && bytes[expected] < 0x80) ++expected;
            assert(retired == expected * Loop::words.size());
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = Loop::words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
        }
        native.cpu.x[13] = input + pageSize;
        const auto fault = native.cpu;
        assert(!Loop::execute(native, Loop::batchSize) && native.cpu == fault);
    }
    {
        using Block = AndroidArtGcBitmap;
        constexpr uint64_t entry = 0x5b7c, data = 0x9000;
        std::vector<uint8_t> code(sizeof(Block::words)), bytes(pageSize);
        std::memcpy(code.data(), Block::words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {data, bytes, 6}}, {}};
        Machine native(image), reference(image);
        for (unsigned example = 0; example < 256; ++example) {
            CPU cpu;
            cpu.pc = entry;
            cpu.retired = example;
            cpu.nzcv = example & 15;
            cpu.x[8] = example % 5;
            cpu.x[9] = data + 64;
            cpu.x[10] = example;
            cpu.x[11] = UINT64_MAX;
            cpu.x[12] = UINT64_MAX - 1;
            const auto address = cpu.x[9] + cpu.x[8] * 8;
            const auto mask = uint64_t{1} << (example & 63);
            const uint64_t bitmap = (example & 64) ? 0x55aa55aa55aa55aaULL | mask : 0x55aa55aa55aa55aaULL & ~mask;
            assert(native.store64(address, bitmap) && reference.store64(address, bitmap));
            cpu.exclusiveAddress = address;
            cpu.exclusiveSize = 8;
            native.cpu = reference.cpu = cpu;
            const auto retired = Block::execute(native, 1);
            assert(retired == ((bitmap & mask) ? Block::maxIterationInstructions : 0));
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = Block::words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
            uint64_t nativeBitmap, referenceBitmap;
            assert(native.load64(address, nativeBitmap) && reference.load64(address, referenceBitmap));
            assert(nativeBitmap == referenceBitmap && nativeBitmap == bitmap);
        }
        native.cpu.pc = entry;
        native.cpu.x[8] = 0;
        native.cpu.x[9] = data;
        native.cpu.x[10] = 1;
        native.segments[1].flags = 0;
        const auto initial = native.cpu;
        assert(!Block::execute(native, 1) && native.cpu == initial);
    }
    {
        using Block = AndroidArtGcReturn;
        constexpr uint64_t entry = 0x4dcc, stack = 0x6000, tls = 0x8000;
        std::vector<uint8_t> code(sizeof(Block::words)), data(pageSize);
        std::memcpy(code.data(), Block::words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {stack, data, 6}, {tls, data, 6}}, {}};
        Machine native(image), reference(image);
        assert(androidLoopMatches(native, native.readInstruction(entry), Block::words));
        for (unsigned example = 0; example < 256; ++example) {
            CPU initial;
            initial.pc = entry;
            initial.sp = stack + example * 8;
            initial.x[22] = tls;
            initial.x[29] = initial.sp + 0x40;
            initial.retired = example * 37;
            initial.nzcv = example & 15;
            initial.exclusiveAddress = stack;
            initial.exclusiveSize = 8;
            const auto canary = example * 0xba95'a55a'ef03'4951ULL;
            for (auto* machine : {&native, &reference}) {
                for (uint64_t slot = 0; slot < 8; ++slot)
                    assert(machine->store64(initial.sp + 0x40 + slot * 8, (canary + slot) ^ 0x7654'3210ULL));
                assert(machine->store64(initial.x[29] - 8, canary));
                assert(machine->store64(tls + 0x28, canary));
                machine->cpu = initial;
            }
            assert(Block::execute(native, 1) == Block::words.size());
            for (const auto word : Block::words) {
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
            native.cpu = initial;
            assert(native.store64(tls + 0x28, ~canary));
            assert(Block::execute(native, 1) == 0 && native.cpu == initial);
        }
        native.cpu.x[22] = 0;
        const auto unreadableCanary = native.cpu;
        assert(Block::execute(native, 1) == 0 && native.cpu == unreadableCanary);
        native.cpu.x[22] = tls;
        native.cpu.x[29] = stack + 8;
        native.cpu.sp = stack + pageSize - 0x50;
        assert(native.store64(tls + 0x28, 0) && native.store64(stack, 0));
        const auto unreadableFrame = native.cpu;
        assert(Block::execute(native, 1) == 0 && native.cpu == unreadableFrame);
    }
    {
        constexpr uint64_t entry = 0x53c0, data = 0x7000;
        constexpr auto words = AndroidBionicFillLoop::words;
        std::vector<uint8_t> code(sizeof(words)), input(pageSize, 0x55);
        std::memcpy(code.data(), words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {data, input, 6}}, {}};
        Machine native(image), reference(image);
        assert(androidLoopMatches(native, native.readInstruction(entry), words));
        const std::array<uint64_t, 8> lengths{0, 1, 63, 64, 65, 1024, INT64_MAX, uint64_t{1} << 63};
        for (size_t example = 0; example < 256; ++example) {
            CPU initial;
            initial.pc = entry;
            initial.retired = 5 * example;
            initial.nzcv = example % 16;
            initial.x[2] = lengths[example % lengths.size()];
            initial.x[3] = example % 2 ? data + pageSize - 128 : data;
            initial.exclusiveAddress = initial.x[3] + (example % 4) * 32;
            initial.exclusiveSize = 16;
            for (size_t index = 0; index < initial.v[0].size(); ++index) initial.v[0][index] = example * 31 + index;
            assert(native.write(data, input) && reference.write(data, input));
            native.cpu = reference.cpu = initial;
            const auto retired = AndroidBionicFillLoop::execute(native, 1 + example % 16);
            assert(retired && retired <= (1 + example % 16) * words.size());
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
            assert(std::memcmp(native.read(data, pageSize, 4), reference.read(data, pageSize, 4), pageSize) == 0);
        }
        native.cpu.pc = entry;
        native.cpu.x[3] = data + pageSize - 64;
        const CPU beforeFault = native.cpu;
        assert(!AndroidBionicFillLoop::execute(native, 16) && native.cpu == beforeFault);
        native.cpu.x[3] = data;
        native.segments[1].flags = 4;
        const CPU readOnly = native.cpu;
        assert(!AndroidBionicFillLoop::execute(native, 16) && native.cpu == readOnly);
        native.segments[0].flags = 7;
        native.segments[1].flags = 6;
        assert(!AndroidBionicFillLoop::execute(native, 16) && native.cpu == readOnly);
    }
    {
        constexpr uint64_t entry = 0x5080, source = 0x7000, target = 0x9000;
        constexpr auto words = AndroidBionicCopyLoop::words;
        std::vector<uint8_t> code(sizeof(words)), input(pageSize), blank(pageSize, 0x55);
        std::memcpy(code.data(), words.data(), code.size());
        const Image image{entry, {{entry, code, 5}, {source, input, 6}, {target, blank, 6}}, {}};
        Machine native(image), reference(image);
        assert(androidLoopMatches(native, native.readInstruction(entry), words));
        const std::array<uint64_t, 8> lengths{0, 1, 63, 64, 65, 1024, INT64_MAX, uint64_t{1} << 63};
        for (size_t example = 0; example < 256; ++example) {
            CPU initial;
            initial.pc = entry;
            initial.retired = 10 * example;
            initial.nzcv = example % 16;
            initial.x[1] = source + (example % 2 ? pageSize - 96 : 3);
            initial.x[2] = lengths[example % lengths.size()];
            initial.x[3] = target + (example % 4 ? 7 : pageSize - 96);
            initial.exclusiveSize = 16;
            initial.exclusiveAddress = initial.x[3] + (example % 8) * 16;
            for (size_t i = 6; i <= 13; ++i) initial.x[i] = example * 7 + i * 0x112233445566ULL;
            for (size_t i = 0; i < input.size(); ++i) input[i] = example * 19 + i * 7 + (i >> 8);
            assert(native.write(source, input) && reference.write(source, input));
            assert(native.write(target, blank) && reference.write(target, blank));
            native.cpu = reference.cpu = initial;
            const auto retired = AndroidBionicCopyLoop::execute(native, 1 + example % 16);
            assert(retired && retired <= (1 + example % 16) * words.size());
            for (uint64_t i = 0; i < retired; ++i) {
                const auto word = words[(reference.cpu.pc - entry) / sizeof(uint32_t)];
                const auto decoded = decode(word);
                if (decoded.op == Op::unsupported) assert(executeStartupInstruction(reference, word));
                else executeDecoded(reference, decoded);
            }
            assert(native.cpu == reference.cpu);
            assert(std::memcmp(native.read(target, pageSize, 4), reference.read(target, pageSize, 4), pageSize) == 0);
        }
        native.cpu.pc = entry;
        native.cpu.x[1] = source;
        native.cpu.x[2] = 1024;
        native.cpu.x[3] = source + 16;
        const auto overlap = native.cpu;
        assert(!AndroidBionicCopyLoop::execute(native, 16) && native.cpu == overlap);
        native.cpu.x[3] = target;
        native.segments[2].flags = 4;
        const auto protectedTarget = native.cpu;
        assert(!AndroidBionicCopyLoop::execute(native, 16) && native.cpu == protectedTarget);
        native.segments[2].flags = 6;
        native.cpu.x[1] = source + pageSize - 32;
        const auto crossesPage = native.cpu;
        assert(!AndroidBionicCopyLoop::execute(native, 16) && native.cpu == crossesPage);

        auto shared = std::make_shared<SharedFileBytes>();
        shared->pages = input;
        shared->size = input.size();
        native.segments[1].shared = native.segments[2].shared = shared;
        native.cpu.x[1] = source;
        const auto mappedAlias = native.cpu;
        assert(!AndroidBionicCopyLoop::execute(native, 16) && native.cpu == mappedAlias);
    }
    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t base = 0x2000, path = base + 0x20, payload = base + 0x140, reply = base + 0x220;
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        guest.setBootContext("u:r:zygote:s0");
        assert(guest.loadSeappContexts(
            "user=_app domain=untrusted_app_34 type=app_data_file levelFrom=all\n"
            "user=_app domain=platform_app type=app_data_file levelFrom=user\n"
            "user=system domain=system_app type=system_app_data_file\n"));
        assert(!guest.loadAppTransitionDomains("untrusted_app_34\nunknown_app\n"));
        assert(guest.loadAppTransitionDomains("untrusted_app_34\nplatform_app\nsystem_app\n"));
        assert(guest.loadServicePolicy(
            "7 1 2 3\n"
            "u:r:untrusted_app_34:s0 u:object_r:activity_service:s0 2 ffffffff 0 ffffffff 1 0\n"
            "u:r:untrusted_app_34:s0 u:object_r:vold_service:s0 0 ffffffff 0 ffffffff 1 0\n", false));
        const auto call = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto copy = [&](uint64_t destination, std::string_view value) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
            assert(task.write(destination, {bytes, value.size()}));
            const uint8_t nul = 0;
            assert(task.write(destination + value.size(), {&nul, 1}));
        };
        const auto open = [&](std::string_view file) {
            copy(path, file);
            const auto fd = call(56, {uint32_t(-100), path, 2});
            assert(fd < 256);
            return fd;
        };
        const auto write = [&](uint64_t fd, std::string_view value) {
            copy(payload, value);
            return call(64, {fd, payload, value.size() + 1});
        };
        constexpr std::string_view target = "u:r:untrusted_app_34:s0:c159,c256,c512,c768";
        auto current = open("/proc/self/attr/current");
        assert(write(current, target) == uint64_t(-13)); // The zygote must remain unchanged.
        const auto child = call(220, {0x1200011, 0, 0, 0, base + 0x310});
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        assert(call(147, {10159, 10159, 10159}) == 0);
        assert(write(current, target) == uint64_t(-1)); // The inherited FD names the parent's task.
        current = open("/proc/self/attr/current");
        const auto context = open("/sys/fs/selinux/context");
        assert(write(context, target) == target.size() + 1);
        assert(write(context, "u:r:missing_app:s0") == uint64_t(-22));
        assert(write(current, "u:r:untrusted_app_34:s0:c160,c256,c512,c768") == uint64_t(-13));
        assert(write(current, "u:r:system_app:s0") == uint64_t(-13));
        assert(write(current, target) == target.size() + 1);
        copy(path, "/proc/self/status");
        const auto status = call(56, {uint32_t(-100), path, 0});
        assert(status < 256);
        const auto statusLength = call(63, {status, reply, 512});
        assert(statusLength < 512);
        const std::string_view statusText(reinterpret_cast<const char*>(task.read(reply, statusLength, 4)), statusLength);
        assert(statusText.find("Uid:\t10159\t10159\t10159\t10159\n") != std::string_view::npos);
        assert(statusText.find("PPid:\t1\n") != std::string_view::npos);
        assert(call(57, {status}) == 0);
        copy(path, "/proc/1/status");
        assert(call(56, {uint32_t(-100), path, 0}) == uint64_t(-2));
        copy(path, "/proc/99999/status");
        assert(call(56, {uint32_t(-100), path, 0}) == uint64_t(-2));
        copy(path, "/proc/1/task");
        assert(call(56, {uint32_t(-100), path, 0x4000}) == uint64_t(-2));
        copy(path, "/proc/1/cmdline");
        assert(call(56, {uint32_t(-100), path, 0}) == uint64_t(-2));
        assert(write(current, "u:r:platform_app:s0:c512,c768") == uint64_t(-13));
        assert(call(63, {current, reply, 128}) == target.size() + 1);
        assert(std::memcmp(task.read(reply, target.size(), 4), target.data(), target.size()) == 0);
        const auto access = open("/sys/fs/selinux/access");
        const auto decision = [&](std::string_view service, uint8_t allow) {
            const auto query = std::string(target) + " u:object_r:" + std::string(service) + ":s0 7 2";
            assert(write(access, query) == query.size() + 1);
            assert(call(63, {access, reply, 128}) > 6);
            assert(*task.read(reply, 1, 4) == allow);
        };
        decision("activity_service", '2');
        decision("vold_service", '0');
    }
    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t base = 0x2000, limits = base + 0x20;
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto call = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        assert(task.store64(limits, 32768) && task.store64(limits + 8, 524288));
        assert(call(164, {7, limits}) == 0); // Android init's early-init nofile limits.
        const auto child = call(220, {0x1200011, 0, 0, 0, base + 0x40});
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        assert(call(163, {7, limits}) == 0);
        uint64_t soft = 0, hard = 0;
        assert(task.load64(limits, soft) && task.load64(limits + 8, hard) && soft == 32768 && hard == 524288);
        assert(call(25, {0, 1030, 32767}) == 32767);
        assert(call(24, {0, 32768, 0}) == uint64_t(-9));
        assert(call(57, {32767}) == 0);
        assert(task.store64(limits, hard));
        assert(call(164, {7, limits}) == 0);
        assert(call(25, {0, 1030, 524287}) == 524287);
        assert(call(24, {0, 524288, 0}) == uint64_t(-9));
        assert(task.store64(limits + 8, hard + 1));
        assert(call(164, {7, limits}) == uint64_t(-1));
    }
    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t base = 0x2000, text = base + 0x100, output = base + 0x180;
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto call = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto copy = [&](std::string_view value) {
            assert(task.write(text, {reinterpret_cast<const uint8_t*>(value.data()), value.size()}));
            const uint8_t zero = 0;
            assert(task.write(text + value.size(), {&zero, 1}));
        };
        const auto open = [&](std::string_view path, uint64_t flags) {
            copy(path);
            return call(56, {uint32_t(-100), text, flags});
        };
        const auto set = [&](uint64_t descriptor, std::string_view value) {
            copy(value);
            return call(64, {descriptor, text, value.size()});
        };
        const auto get = [&](uint64_t descriptor, std::string_view expected) {
            assert(call(67, {descriptor, output, 16, 0}) == expected.size());
            assert(std::memcmp(task.read(output, expected.size(), 4), expected.data(), expected.size()) == 0);
        };
        const auto parentScore = open("/proc/self/oom_score_adj", 2);
        assert(parentScore < 256);
        get(parentScore, "0\n");
        assert(set(parentScore, "-1001") == uint64_t(-22));
        assert(set(parentScore, "+0x64\n") == 6);
        const auto child = call(220, {0x1200011, 0, 0, 0, base + 0x40});
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        assert(call(147, {1000, 1000, 1000}) == 0);
        assert(open("/proc/1/oom_score_adj", 2) == uint64_t(-2));
        const auto ownScore = open("/proc/thread-self/oom_score_adj", 2);
        assert(ownScore < 256);
        get(ownScore, "100\n");
        assert(set(ownScore, "300") == 3 && set(ownScore, "99") == uint64_t(-13));
        assert(set(ownScore, "100") == 3);
        get(ownScore, "100\n");
        assert(set(ownScore, "400") == 3);
        get(parentScore, "100\n");
        task.cpu.retired += 4096;
        assert(guest.schedule(task) && guest.processId() == 1);
        const auto childScore = open("/proc/" + std::to_string(child) + "/oom_score_adj", 2);
        assert(childScore < 256);
        get(childScore, "400\n");
        assert(set(childScore, "-1000") == 5);
        get(childScore, "-1000\n");
        assert(call(129, {child, 9}) == 0);
        assert(call(67, {childScore, output, 16, 0}) == uint64_t(-3));
    }
    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t base = 0x2000, data = base + pageSize;
        constexpr uint64_t size = 256 * 1024 + pageSize;
        const uint64_t tail = data + size - sizeof(uint64_t);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6},
                                  {data, std::vector<uint8_t>(size), 6}}, {}});
        assert(task.store64(data, 31));
        assert(task.store64(tail, 57));
        const auto* parentBytes = task.read(data, sizeof(uint64_t), 4);
        task.cpu.x[0] = 0x1200011;
        task.cpu.x[4] = base + 0x20;
        task.cpu.x[8] = 220;
        assert(guest.syscall(task));
        const auto child = task.cpu.x[0];
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        assert(task.read(data, sizeof(uint64_t), 4) == parentBytes);
        assert(task.store64(data, 42));
        uint64_t value = 0;
        assert(task.load64(data, value) && value == 42);
        assert(task.read(data, sizeof(uint64_t), 4) != parentBytes);
        assert(task.load64(tail, value) && value == 57);
        assert(task.store64(tail, 63));
        task.cpu.retired += 4096;
        assert(guest.schedule(task) && guest.processId() == 1);
        assert(task.read(data, sizeof(uint64_t), 4) == parentBytes);
        assert(task.load64(data, value) && value == 31);
        assert(task.load64(tail, value) && value == 57);
    }
    {
        using Result = GuestApkInbox::Result;
        GuestApkInbox inbox;
        std::vector<Result> outcomes;
        const auto finished = [&](Result result) { outcomes.push_back(result); };
        assert(!inbox.take() && !inbox.submit({}, finished));
        assert(inbox.submit({0x50, 0x4b}, finished));
        assert(!inbox.submit({1}, finished));
        const auto first = inbox.take();
        assert(first && first->bytes == std::vector<uint8_t>({0x50, 0x4b}));
        assert(!inbox.take() && !inbox.submit({2}, finished));
        first->finish(Result::installed);
        first->finish(Result::rejected);
        assert(outcomes == std::vector{Result::installed});
        assert(inbox.submit({3}, finished));
        {
            const auto abandoned = inbox.take();
            assert(abandoned && !abandoned->finished());
        }
        assert(outcomes.back() == Result::guestStopped && outcomes.size() == 2);
        assert(inbox.submit({4}, finished));
        inbox.stop();
        inbox.stop();
        assert(!inbox.take() && !inbox.submit({5}, finished));
        assert(outcomes.size() == 3 && outcomes.back() == Result::guestStopped);
    }
    {
        constexpr uint64_t address = 0x100000;
        Machine lanes(Image{address, {{address, std::vector<uint8_t>(pageSize), 6}}, {}});
        lanes.cpu.x[1] = address;
        for (unsigned i = 0; i < 4; ++i) lanes.cpu.v[i].fill(uint8_t(0x10 + i));
        assert(executeStartupInstruction(lanes, 0x0dbfa020)); // st4 {v0.s-v3.s}[0], [x1], #16
        const std::array<uint32_t, 4> expected{0x10101010, 0x11111111, 0x12121212, 0x13131313};
        std::array<uint32_t, 4> values{};
        assert(lanes.readInto(address, {reinterpret_cast<uint8_t*>(values.data()), sizeof(values)}));
        assert(values == expected && lanes.cpu.x[1] == address + 16);

        lanes.cpu.x[3] = address;
        lanes.cpu.x[4] = 11;
        lanes.cpu.v[31].fill(0xff);
        assert(executeStartupInstruction(lanes, 0x4dc4787f)); // ld3 {v31.h, v0.h, v1.h}[7], [x3], x4
        assert(lanes.cpu.x[3] == address + 11);
        assert(lanes.cpu.v[31][13] == 0xff && lanes.cpu.v[31][14] == 0x10 && lanes.cpu.v[31][15] == 0x10);
        assert(lanes.cpu.v[0][13] == 0x10 && lanes.cpu.v[0][14] == 0x10 && lanes.cpu.v[0][15] == 0x10);
        assert(lanes.cpu.v[1][13] == 0x11 && lanes.cpu.v[1][14] == 0x11 && lanes.cpu.v[1][15] == 0x11);

        lanes.cpu.sp = address + 32;
        lanes.cpu.v[30].fill(0x33);
        assert(executeStartupInstruction(lanes, 0x4d2087fe)); // st2 {v30.d, v31.d}[1], [sp]
        assert(lanes.cpu.sp == address + 32);
        uint64_t first, second;
        assert(lanes.load64(address + 32, first) && first == 0x3333333333333333ULL);
        assert(lanes.load64(address + 40, second) && second == 0x1010ffffffffffffULL);

        lanes.cpu.x[3] = address + pageSize - 24;
        const auto before = lanes.cpu.v;
        assert(executeStartupInstruction(lanes, 0x4dffa47d)); // ld4 {v29.d-v0.d}[1], [x3], #32
        assert(lanes.cpu.stop == Stop::fault && lanes.cpu.x[3] == address + pageSize - 24);
        assert(lanes.cpu.v == before);
    }
    {
        auto map = std::make_shared<BpfMap>();
        map->id = 1;
        map->array = true;
        map->type = 2;
        map->entryCount = 3;
        map->keySize = 4;
        map->valueSize = map->valueStride = 8;
        const std::array<uint64_t, 3> values{2, 3, 100};
        const auto initial = std::as_bytes(std::span(values));
        map->values.pages.resize(initial.size());
        std::memcpy(map->values.pages.data(), initial.data(), initial.size());
        const auto encode = [](uint8_t code, uint8_t registers, int16_t offset = 0, int32_t immediate = 0) {
            return uint64_t(code) | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        const std::array<uint64_t, 20> code{
            encode(0xb7, 0, 0, 0), encode(0x7b, 0x0a, -8),
            encode(0xbf, 0xa3), encode(0x07, 3, 0, -8), // Parent callback context points to the sum.
            encode(0x18, 0x11, 0, 12), 0,              // r1 = map
            encode(0x18, 0x42, 0, 4), 0,               // r2 = callback at slot 11
            encode(0x85, 0, 0, 164),
            encode(0x79, 0xa0, -8), encode(0x95, 0),
            encode(0x61, 0x25), encode(0xb7, 0, 0, 1),
            encode(0x25, 5, 5, 1),                    // Stop at the third key.
            encode(0x79, 0x35), encode(0x79, 0x46),
            encode(0x0f, 0x56), encode(0x7b, 0x64),
            encode(0xb7, 0, 0, 0), encode(0x95, 0),
        };
        auto bytecode = std::span(reinterpret_cast<const uint8_t*>(code.data()), sizeof(code));
        BpfProgram unsupported({}, {});
        const auto fromFd = [&](uint32_t fd) { return fd == 12 ? map : nullptr; };
        assert(!unsupported.load(BpfProgram::classifier, bytecode, fromFd));
        // The callback stores into its parent's stack while leaving its own return register independent.
        BpfProgram::DebugInfo debug{};
        debug.functions = {{0, 1}, {11, 2}};
        BpfProgram callback({}, std::move(debug));
        assert(callback.load(BpfProgram::classifier, bytecode, fromFd));
        BpfProgram::Packet packet;
        assert(callback.classify(packet) == 5);
    }
    {
        auto cache = std::make_shared<BpfMap>();
        cache->type = BpfMap::lruHash;
        cache->entryCount = 2;
        cache->keySize = cache->valueSize = cache->valueStride = 4;
        const std::vector<uint8_t> first{1, 0, 0, 0}, second{2, 0, 0, 0}, third{3, 0, 0, 0};
        cache->storeHash(first, {10, 0, 0, 0}, cache->hash.end());
        cache->storeHash(second, {20, 0, 0, 0}, cache->hash.end());
        const auto encode = [](uint8_t op, uint8_t registers, int16_t offset = 0, int32_t immediate = 0) {
            return op | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        const std::array<uint64_t, 10> code{
            encode(0x18, 0x11, 0, 12), 0, encode(0xb7, 3, 0, 1), encode(0x63, 0x3a, -4),
            encode(0xbf, 0xa2), encode(0x07, 2, 0, -4), encode(0x85, 0, 0, 1),
            encode(0x15, 0, 1), encode(0x61, 0), encode(0x95, 0)};
        BpfProgram program({}, {});
        const auto bytes = std::span(reinterpret_cast<const uint8_t*>(code.data()), sizeof(code));
        assert(program.load(BpfProgram::classifier, bytes, [&](uint32_t fd) { return fd == 12 ? cache : nullptr; }));
        BpfProgram::Packet packet;
        assert(program.classify(packet) == 10);
        assert(cache->findValue(second) != cache->hash.end()); // A userspace lookup leaves recency unchanged.
        cache->storeHash(third, {30, 0, 0, 0}, cache->hash.end());
        assert(cache->hash.size() == 2 && cache->hash.contains(first) && !cache->hash.contains(second));
    }
    {
        const std::array<uint8_t, 24> record{0x42, 0, 0, 0, 0, 0, 0, 0, 0x37};
        BpfProgram program({}, {});
        const auto load = [&](uint64_t instruction) {
            const std::array<uint64_t, 2> code{instruction, 0x95};
            return program.load(BpfProgram::tracepoint,
                                {reinterpret_cast<const uint8_t*>(code.data()), sizeof(code)}, {});
        };
        assert(load(0x1061)); // LDXW r0, [r1]
        assert(program.runTracepoint(record) == 0x42);
        assert(load(0x81079)); // LDXDW r0, [r1 + 8]
        assert(program.runTracepoint(record) == 0x37);
        assert(!program.runTracepoint(std::span(record).first(15)));
        assert(load(0x017b)); // STXDW [r1], r0
        assert(!program.runTracepoint(record));
        assert(!load((uint64_t{46} << 32) | 0x85)); // Socket helpers are unavailable.
    }
    {
        using Callback = BpfProgram::SocketOperations;
        const auto socket = std::make_shared<int>(0), other = std::make_shared<int>(1);
        auto storage = std::make_shared<BpfMap>();
        storage->type = BpfMap::socketStorage;
        storage->valueSize = 4;
        storage->socketValues[socket] = {42, 0, 0, 0};
        const auto encode = [](uint8_t op, uint8_t registers, int16_t offset = 0, int32_t immediate = 0) {
            return op | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        const auto run = [&](Callback& callback, const auto& code) {
            BpfProgram program({}, {});
            const auto bytes = std::span(reinterpret_cast<const uint8_t*>(code.data()), sizeof(code));
            assert(program.load(BpfProgram::socketOperations, bytes, [&](uint32_t fd) {
                return fd == 12 ? storage : nullptr;
            }));
            return program.runSocketOperations(callback);
        };
        const std::array lookup{
            encode(0x79, 0x12, 184), encode(0x18, 0x11, 0, 12), uint64_t{0},
            encode(0xb7, 3), encode(0xb7, 4), encode(0x85, 0, 0, 107),
            encode(0x15, 0, 1), encode(0x61, 0), encode(0x95, 0)};
        Callback connect{socket, Callback::tcpConnect}, unrelated{other, Callback::tcpConnect};
        assert(run(connect, lookup) == 42 && run(unrelated, lookup) == 0);
        const std::array setFlags{encode(0xb7, 2, 0, 0x40), encode(0x85, 0, 0, 59), encode(0x95, 0)};
        assert(run(connect, setFlags) == 0 && connect.callbackFlags == 0x40);
        const std::array reserve{
            encode(0xb7, 2, 0, 3), encode(0xb7, 3), encode(0x85, 0, 0, 144), encode(0x95, 0)};
        assert(run(connect, reserve) == uint32_t(-1));
        Callback length{socket, Callback::optionLength};
        assert(run(length, reserve) == 0 && length.remaining == 37);
        const std::array write{
            encode(0xb7, 3, 0, 0x6503ae), encode(0x63, 0x3a, -4), encode(0xbf, 0xa2), encode(0x07, 2, 0, -4),
            encode(0xb7, 3, 0, 3), encode(0xb7, 4), encode(0x85, 0, 0, 143), encode(0x95, 0)};
        Callback option{socket, Callback::writeOption};
        option.remaining = 3;
        assert(run(option, write) == 0 && option.used == 3 && !option.remaining);
        assert(option.options[0] == 174 && option.options[1] == 3 && option.options[2] == 101);
        assert(run(option, write) == uint32_t(-28));
    }
    {
        const auto encode = [](uint8_t op, uint8_t registers = 0, int32_t immediate = 0) {
            return op | (uint64_t(registers) << 8) | (uint64_t(uint32_t(immediate)) << 32);
        };
        // r6 is the implicit skb. The first absolute read selects an index for the indirect read.
        const std::array code{encode(0xbf, 0x16), encode(0x30, 0, 1), encode(0xbf, 0x07),
                              encode(0x48, 0x70, 1), encode(0x95)};
        const auto bytes = std::span(reinterpret_cast<const uint8_t*>(code.data()), sizeof(code));
        BpfProgram program({}, {});
        assert(program.load(BpfProgram::classifier, bytes, {}));
        BpfProgram::Packet packet;
        packet.bytes = {0, 2, 0, 0x12, 0x34};
        assert(program.classify(packet) == 0x1234);
        packet.bytes.pop_back();
        assert(program.classify(packet) == 0); // Truncated read terminates the program immediately.
        assert(!program.load(BpfProgram::socketOperations, bytes, {}));
    }
    {
        auto map = std::make_shared<BpfMap>();
        map->type = BpfMap::lruHash;
        map->entryCount = 1;
        map->keySize = map->valueSize = map->valueStride = 4;
        const std::vector<uint8_t> key{0, 0, 0, 0};
        map->storeHash(key, {42, 0, 0, 0}, map->hash.end());
        const auto encode = [](uint8_t op, uint8_t registers, int16_t offset = 0, int32_t immediate = 0) {
            return op | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        const std::array code{
            encode(0x85, 0x10, 0, 1), encode(0x95, 0), // Local call to slot 2.
            encode(0x18, 0x17, 0, 12), uint64_t{0}, encode(0xb7, 1), encode(0x63, 0x1a, -4),
            encode(0xbf, 0x71), encode(0xbf, 0xa2), encode(0x07, 2, 0, -4), encode(0x85, 0, 0, 1),
            encode(0x15, 0, 11), encode(0xbf, 0x06), // Keep the old value across an update.
            encode(0xb7, 3, 0, 77), encode(0x63, 0x3a, -8),
            encode(0xbf, 0x71), encode(0xbf, 0xa2), encode(0x07, 2, 0, -4),
            encode(0xbf, 0xa3), encode(0x07, 3, 0, -8), encode(0xb7, 4), encode(0x85, 0, 0, 2),
            encode(0x61, 0x60), encode(0x95, 0)};
        const auto bytes = std::span(reinterpret_cast<const uint8_t*>(code.data()), sizeof(code));
        BpfProgram::DebugInfo debug{};
        debug.functions = {{0, 1}, {2, 2}};
        BpfProgram program({}, std::move(debug));
        assert(program.load(BpfProgram::cgroupPacket, bytes, [&](uint32_t fd) { return fd == 12 ? map : nullptr; }));
        BpfProgram::Packet packet;
        assert(program.classify(packet) == 42 && map->hash.find(key)->second.bytes[0] == 77);
    }
    {
        const auto encode = [](uint8_t op, uint8_t registers, int16_t offset = 0, int32_t immediate = 0) {
            return op | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        const std::array code{encode(0x79, 0x12, 8), encode(0xb7, 3, 0, 42), encode(0x73, 0x32),
            encode(0xb7, 3, 0, 1), encode(0x63, 0x31, 32), encode(0xb7, 0, 0, 1), encode(0x95, 0)};
        const auto bytes = std::span(reinterpret_cast<const uint8_t*>(code.data()), sizeof(code));
        BpfProgram program({}, {});
        assert(program.load(BpfProgram::socketOption, bytes, {}));
        std::array<uint8_t, 2> output{};
        BpfProgram::SocketOption request{false, 1, 2, 2, 0, output};
        BpfProgram::SocketContext socket{std::make_shared<int>(0), 1, 0, 0, 2, 1, 6, {}};
        socket.option = &request;
        assert(program.runSocketContext(socket) == 1 && request.length == 1 && output[0] == 42 && !output[1]);
        request.bytes = {};
        assert(!program.runSocketContext(socket)); // The zero-length buffer cannot be written.
    }
    constexpr uint64_t base = 0x2000;
    Machine machine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
    {
        Machine pairs(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        constexpr uint64_t address = base + 0x80;
        pairs.cpu.x[8] = address;
        assert(pairs.store64(address, 0x1122334455667788) && pairs.store64(address + 8, 0x99aabbccddeeff00));
        for (const uint32_t hint : {0xd503211fU, 0xd503215fU, 0xd503219fU, 0xd50321dfU, 0xd503251fU}) {
            auto expected = pairs.cpu;
            expected.pc += 4;
            ++expected.retired;
            assert(executeStartupInstruction(pairs, hint) && pairs.cpu == expected);
        }
        assert(executeStartupInstruction(pairs, 0xc87fd115)); // ldaxp x21, x20, [x8]
        assert(pairs.cpu.x[21] == 0x1122334455667788 && pairs.cpu.x[20] == 0x99aabbccddeeff00);
        pairs.cpu.x[21] = 1;
        pairs.cpu.x[20] = 2;
        assert(executeStartupInstruction(pairs, 0xc8245115)); // stxp w4, x21, x20, [x8]
        uint64_t first = 0, second = 0;
        assert(pairs.cpu.x[4] == 0 && pairs.load64(address, first) && pairs.load64(address + 8, second));
        assert(first == 1 && second == 2);
        assert(executeStartupInstruction(pairs, 0xc87fd115));
        assert(pairs.store64(address + 8, 3));
        assert(executeStartupInstruction(pairs, 0xc8245115));
        assert(pairs.cpu.x[4] == 1 && pairs.load64(address + 8, second) && second == 3);
        assert(executeStartupInstruction(pairs, 0x887f5115)); // ldxp w21, w20, [x8]
        assert(pairs.cpu.x[21] == 1 && pairs.cpu.x[20] == 0);
        pairs.cpu.x[20] = 0xbadcafe512345678ULL;
        assert(executeStartupInstruction(pairs, 0x8824d115)); // stlxp w4, w21, w20, [x8]
        assert(pairs.cpu.x[4] == 0 && pairs.load64(address, first) && first == 0x1234567800000001ULL);
        pairs.cpu.x[8] = address + 8;
        assert(executeStartupInstruction(pairs, 0xc87fd115));
        assert(pairs.cpu.stop == Stop::fault && pairs.cpu.faultAddress == address + 8);
    }
    {
        constexpr uint64_t location = 0x4000;
        Machine literal(Image{location, {{location, std::vector<uint8_t>(pageSize), 7}}, {}});
        const std::array<uint8_t, 16> bytes{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        assert(literal.write(location + 0xcc, bytes));
        assert(executeStartupInstruction(literal, 0xd53b0029)); // mrs x9, ctr_el0
        assert(literal.cpu.x[9] == 0xb004c004 && literal.cpu.pc == location + 4);
        assert(executeStartupInstruction(literal, 0xd5033b9f)); // dsb ish
        assert(literal.cpu.pc == location + 8);
        for (unsigned scale = 0; scale < 3; ++scale) {
            literal.cpu.pc = location;
            literal.cpu.v[8].fill(0xff);
            assert(executeStartupInstruction(literal, 0x1c000668 | (scale << 30)));
            assert(literal.cpu.pc == location + 4);
            const unsigned size = 4U << scale;
            for (unsigned i = 0; i < bytes.size(); ++i)
                assert(literal.cpu.v[8][i] == (i < size ? bytes[i] : 0));
        }
        literal.cpu.pc = location + 0xdc;
        assert(executeStartupInstruction(literal, 0x9c000008 | (0x7fffc << 5)));
        assert(literal.cpu.v[8] == bytes);
        literal.cpu.pc = location + pageSize - 4;
        assert(executeStartupInstruction(literal, 0x1c000668));
        assert(literal.cpu.stop == Stop::fault && literal.cpu.faultAddress == location + pageSize + 0xc8);
        assert(literal.cpu.v[8] == bytes);
    }
    {
        constexpr uint64_t location = 0x4000;
        Machine cache(Image{location, {{location, std::vector<uint8_t>(pageSize, 0x35), 4},
                                      {location + pageSize, std::vector<uint8_t>(pageSize), 0}}, {}});
        cache.cpu.x[22] = location + 3;
        assert(executeStartupInstruction(cache, 0xd50b7a36)); // dc cvac, x22
        assert(cache.cpu.pc == location + 4 && cache.cpu.retired == 1);
        assert(cache.cpu.x[22] == location + 3 && *cache.read(location + 3, 1, 4) == 0x35);
        for (const auto inaccessible : {location + pageSize, location + 2 * pageSize}) {
            cache.cpu.x[22] = inaccessible;
            assert(executeStartupInstruction(cache, 0xd50b7a36));
            assert(cache.cpu.stop == Stop::fault && cache.cpu.faultAddress == inaccessible);
            assert(cache.cpu.pc == location + 4 && cache.cpu.retired == 1);
            cache.cpu.stop = Stop::running;
        }
    }
    {
        Machine permute(Image{base, {}, {}});
        const std::array<uint32_t, 4> left{10, 20, 30, 40}, right{1, 2, 3, 4}, expected{20, 2, 40, 4};
        std::memcpy(permute.cpu.v[2].data(), left.data(), sizeof(left));
        std::memcpy(permute.cpu.v[1].data(), right.data(), sizeof(right));
        assert(executeStartupInstruction(permute, 0x4e816841)); // trn2 v1.4s, v2.4s, v1.4s
        assert(std::memcmp(permute.cpu.v[1].data(), expected.data(), sizeof(expected)) == 0);
        const std::array<uint16_t, 4> lowerLeft{4, 5, 6, 7}, lowerRight{8, 9, 10, 11};
        const std::array<uint16_t, 8> lowerExpected{4, 8, 6, 10, 0, 0, 0, 0};
        std::memcpy(permute.cpu.v[4].data(), lowerLeft.data(), sizeof(lowerLeft));
        std::memcpy(permute.cpu.v[5].data(), lowerRight.data(), sizeof(lowerRight));
        permute.cpu.v[3].fill(0xff);
        assert(executeStartupInstruction(permute, 0x0e452883)); // trn1 v3.4h, v4.4h, v5.4h
        assert(std::memcmp(permute.cpu.v[3].data(), lowerExpected.data(), sizeof(lowerExpected)) == 0);
        const std::array<uint32_t, 4> compare{0xbf800000, 0x80000000, 0x3f800000, 0x7fc00000};
        const std::array<uint32_t, 4> compareExpected{0, UINT32_MAX, UINT32_MAX, 0};
        std::memcpy(permute.cpu.v[3].data(), compare.data(), sizeof(compare));
        assert(executeStartupInstruction(permute, 0x6ea0c863)); // fcmge v3.4s, v3.4s, #0.0
        assert(std::memcmp(permute.cpu.v[3].data(), compareExpected.data(), sizeof(compareExpected)) == 0);
        const std::array<uint32_t, 4> inverted{UINT32_MAX, 0, 0, UINT32_MAX};
        assert(executeStartupInstruction(permute, 0x6e205863)); // mvn v3.16b, v3.16b
        assert(std::memcmp(permute.cpu.v[3].data(), inverted.data(), sizeof(inverted)) == 0);
        const float one = 1.0f, four = 4.0f;
        std::memcpy(permute.cpu.v[1].data(), &one, sizeof(one));
        std::memcpy(permute.cpu.v[3].data(), &four, sizeof(four));
        permute.cpu.v[2].fill(0xff);
        assert(executeStartupInstruction(permute, 0x7ea3d422)); // fabd s2, s1, s3
        const std::array<float, 4> absoluteDifference{3.0f, 0, 0, 0};
        assert(std::memcmp(permute.cpu.v[2].data(), absoluteDifference.data(), sizeof(absoluteDifference)) == 0);
        const float positiveZero = 0.0f;
        std::memcpy(permute.cpu.v[4].data(), &positiveZero, sizeof(positiveZero));
        permute.cpu.v[6].fill(0xff);
        assert(executeStartupInstruction(permute, 0x1e248866)); // fnmul s6, s3, s4
        const std::array<float, 4> negativeZero{-0.0f, 0, 0, 0};
        assert(std::memcmp(permute.cpu.v[6].data(), negativeZero.data(), sizeof(negativeZero)) == 0);
        const double factor = -3.25;
        std::memcpy(permute.cpu.v[7].data(), &factor, sizeof(factor));
        permute.cpu.v[17].fill(0xff);
        assert(executeStartupInstruction(permute, 0x1e6788f1)); // fnmul d17, d7, d7
        const std::array<double, 2> negativeProduct{-10.5625, 0};
        assert(std::memcmp(permute.cpu.v[17].data(), negativeProduct.data(), sizeof(negativeProduct)) == 0);
        const std::array<uint64_t, 2> compareLeft{0x8000000000000000, 3};
        const std::array<uint64_t, 2> compareRight{0x8000000000000000, 5};
        const std::array<uint64_t, 2> compareEqual{UINT64_MAX, 0}, compareUnequal{};
        std::memcpy(permute.cpu.v[1].data(), compareLeft.data(), sizeof(compareLeft));
        std::memcpy(permute.cpu.v[0].data(), compareRight.data(), sizeof(compareRight));
        assert(executeStartupInstruction(permute, 0x7ee08c20)); // cmeq d0, d1, d0
        assert(std::memcmp(permute.cpu.v[0].data(), compareEqual.data(), sizeof(compareEqual)) == 0);
        assert(executeStartupInstruction(permute, 0x7ee08c20));
        assert(std::memcmp(permute.cpu.v[0].data(), compareUnequal.data(), sizeof(compareUnequal)) == 0);
        const std::array<uint64_t, 2> positive{1, UINT64_MAX}, negative{UINT64_MAX, 9};
        std::memcpy(permute.cpu.v[4].data(), positive.data(), sizeof(positive));
        std::memcpy(permute.cpu.v[0].data(), negative.data(), sizeof(negative));
        permute.cpu.v[16].fill(0xff);
        assert(executeStartupInstruction(permute, 0x5ee03490)); // cmgt d16, d4, d0
        assert(std::memcmp(permute.cpu.v[16].data(), compareEqual.data(), sizeof(compareEqual)) == 0);
        std::memcpy(permute.cpu.v[4].data(), negative.data(), sizeof(negative));
        assert(executeStartupInstruction(permute, 0x5ee03490));
        assert((permute.cpu.v[16] == std::array<uint8_t, 16>{}));
        assert(executeStartupInstruction(permute, 0x5ee03c90)); // cmge d16, d4, d0
        assert(std::memcmp(permute.cpu.v[16].data(), compareEqual.data(), sizeof(compareEqual)) == 0);
        std::memcpy(permute.cpu.v[4].data(), positive.data(), sizeof(positive));
        assert(executeStartupInstruction(permute, 0x7ee03490)); // cmhi d16, d4, d0
        assert((permute.cpu.v[16] == std::array<uint8_t, 16>{}));
        assert(executeStartupInstruction(permute, 0x5ee08c90)); // cmtst d16, d4, d0
        assert(std::memcmp(permute.cpu.v[16].data(), compareEqual.data(), sizeof(compareEqual)) == 0);
        assert(!executeStartupInstruction(permute, 0x5ea03490)); // reserved scalar size

        const std::array<float, 4> floorSingles{-2.5f, -0.0f, 17, 42}, floorSinglesResult{-3, -0.0f, 0, 0};
        std::memcpy(permute.cpu.v[1].data(), floorSingles.data(), sizeof(floorSingles));
        assert(executeStartupInstruction(permute, 0x0e219821)); // frintm v1.2s, v1.2s
        assert(std::memcmp(permute.cpu.v[1].data(), floorSinglesResult.data(), sizeof(floorSinglesResult)) == 0);
        const std::array<double, 2> floorDoubles{3.9, -0.2}, floorDoublesResult{3, -1};
        std::memcpy(permute.cpu.v[5].data(), floorDoubles.data(), sizeof(floorDoubles));
        assert(executeStartupInstruction(permute, 0x4e6198a4)); // frintm v4.2d, v5.2d
        assert(std::memcmp(permute.cpu.v[4].data(), floorDoublesResult.data(), sizeof(floorDoublesResult)) == 0);
        const std::array<float, 4> roundSingles{-0.5f, 1.5f, 17, 42}, roundSinglesResult{-1, 2, 0, 0};
        std::memcpy(permute.cpu.v[0].data(), roundSingles.data(), sizeof(roundSingles));
        assert(executeStartupInstruction(permute, 0x2e218800)); // frinta v0.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), roundSinglesResult.data(), sizeof(roundSinglesResult)) == 0);
        const std::array<float, 4> nearestSingles{-0.5f, 2.5f, 1, 2}, nearestSinglesResult{-0.0f, 2, 0, 0};
        std::memcpy(permute.cpu.v[16].data(), nearestSingles.data(), sizeof(nearestSingles));
        assert(executeStartupInstruction(permute, 0x0e218a10)); // frintn v16.2s, v16.2s
        assert(std::memcmp(permute.cpu.v[16].data(), nearestSinglesResult.data(), sizeof(nearestSinglesResult)) == 0);
        const std::array<double, 2> nearestDoubles{-2.5, 3.5}, nearestDoublesResult{-2, 4};
        std::memcpy(permute.cpu.v[16].data(), nearestDoubles.data(), sizeof(nearestDoubles));
        assert(executeStartupInstruction(permute, 0x4e618a10)); // frintn v16.2d, v16.2d
        assert(std::memcmp(permute.cpu.v[16].data(), nearestDoublesResult.data(), sizeof(nearestDoublesResult)) == 0);
        assert(!executeStartupInstruction(permute, 0x0e618a10)); // unallocated 1D vector form
        const std::array<uint32_t, 4> roundingNaNs{0x7f812345, 0xffc01234, 0x7f800000, 0xff800000};
        const std::array<uint32_t, 4> quietedNaNs{0x7fc12345, 0xffc01234, 0x7f800000, 0xff800000};
        permute.cpu.fpsr = fpsrSaturation;
        std::memcpy(permute.cpu.v[16].data(), roundingNaNs.data(), sizeof(roundingNaNs));
        assert(executeStartupInstruction(permute, 0x4e218a10)); // frintn v16.4s, v16.4s
        assert(std::memcmp(permute.cpu.v[16].data(), quietedNaNs.data(), sizeof(quietedNaNs)) == 0 &&
               permute.cpu.fpsr == (fpsrSaturation | 1));
        permute.cpu.fpsr = 0;
        const std::array<float, 4> scalarInput{-0.5f, 2.5f, 3.5f, 4.5f}, scalarRounded{-0.0f, 0, 0, 0};
        std::memcpy(permute.cpu.v[17].data(), scalarInput.data(), sizeof(scalarInput));
        assert(executeStartupInstruction(permute, 0x1e244231)); // frintn s17, s17
        assert(std::memcmp(permute.cpu.v[17].data(), scalarRounded.data(), sizeof(scalarRounded)) == 0);
        const std::array<double, 2> scalarDoubleInput{3.5, 2.5}, scalarDoubleRounded{4, 0};
        std::memcpy(permute.cpu.v[17].data(), scalarDoubleInput.data(), sizeof(scalarDoubleInput));
        assert(executeStartupInstruction(permute, 0x1e644231)); // frintn d17, d17
        assert(std::memcmp(permute.cpu.v[17].data(), scalarDoubleRounded.data(), sizeof(scalarDoubleRounded)) == 0);
        const std::array<uint64_t, 2> negateDoubles{0, 0xfff0000000000000ULL};
        const std::array<uint64_t, 2> negateDoublesResult{0x8000000000000000ULL, 0x7ff0000000000000ULL};
        std::memcpy(permute.cpu.v[0].data(), negateDoubles.data(), sizeof(negateDoubles));
        assert(executeStartupInstruction(permute, 0x6ee0f800)); // fneg v0.2d, v0.2d
        assert(std::memcmp(permute.cpu.v[0].data(), negateDoublesResult.data(), sizeof(negateDoublesResult)) == 0);
        const std::array<float, 4> negateSingles{-0.0f, 2.5f, 17, 42}, negateSinglesResult{0.0f, -2.5f, 0, 0};
        std::memcpy(permute.cpu.v[4].data(), negateSingles.data(), sizeof(negateSingles));
        assert(executeStartupInstruction(permute, 0x2ea0f883)); // fneg v3.2s, v4.2s
        assert(std::memcmp(permute.cpu.v[3].data(), negateSinglesResult.data(), sizeof(negateSinglesResult)) == 0);
        const std::array<float, 4> absSingles{-3.25f, -0.0f, -17.0f, 42.0f};
        const std::array<float, 4> absLow{3.25f, 0.0f, 0.0f, 0.0f}, absAll{3.25f, 0.0f, 17.0f, 42.0f};
        std::memcpy(permute.cpu.v[0].data(), absSingles.data(), sizeof(absSingles));
        permute.cpu.v[2].fill(0xff);
        assert(executeStartupInstruction(permute, 0x0ea0f802)); // fabs v2.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[2].data(), absLow.data(), sizeof(absLow)) == 0);
        assert(executeStartupInstruction(permute, 0x4ea0f800)); // fabs v0.4s, v0.4s
        assert(std::memcmp(permute.cpu.v[0].data(), absAll.data(), sizeof(absAll)) == 0);
        const std::array<uint64_t, 2> absDoubles{0xfff8000000000012ULL, 0xfff0000000000000ULL};
        const std::array<uint64_t, 2> absDoubleResult{0x7ff8000000000012ULL, 0x7ff0000000000000ULL};
        std::memcpy(permute.cpu.v[1].data(), absDoubles.data(), sizeof(absDoubles));
        assert(executeStartupInstruction(permute, 0x4ee0f821)); // fabs v1.2d, v1.2d
        assert(std::memcmp(permute.cpu.v[1].data(), absDoubleResult.data(), sizeof(absDoubleResult)) == 0);
        assert(!executeStartupInstruction(permute, 0x0ee0f821)); // unallocated 1D vector form
        const std::array<float, 4> widenInput{1.75f, -0.0f, -11.5f, 23.25f};
        const std::array<double, 2> widenedLow{1.75, -0.0}, widenedHigh{-11.5, 23.25};
        std::memcpy(permute.cpu.v[2].data(), widenInput.data(), sizeof(widenInput));
        std::memcpy(permute.cpu.v[4].data(), widenInput.data(), sizeof(widenInput));
        assert(executeStartupInstruction(permute, 0x0e617842)); // fcvtl v2.2d, v2.2s
        assert(std::memcmp(permute.cpu.v[2].data(), widenedLow.data(), sizeof(widenedLow)) == 0);
        assert(executeStartupInstruction(permute, 0x4e617883)); // fcvtl2 v3.2d, v4.4s
        assert(std::memcmp(permute.cpu.v[3].data(), widenedHigh.data(), sizeof(widenedHigh)) == 0);
        const std::array<float, 4> narrowedLow{1.75f, -0.0f, 0, 0}, narrowedHigh{42, 12, 1.75f, -0.0f};
        const std::array<float, 4> narrowDestination{42, 12, 79, 22};
        assert(executeStartupInstruction(permute, 0x0e616842)); // fcvtn v2.2s, v2.2d
        assert(std::memcmp(permute.cpu.v[2].data(), narrowedLow.data(), sizeof(narrowedLow)) == 0);
        std::memcpy(permute.cpu.v[1].data(), narrowDestination.data(), sizeof(narrowDestination));
        std::memcpy(permute.cpu.v[2].data(), widenedLow.data(), sizeof(widenedLow));
        assert(executeStartupInstruction(permute, 0x4e616841)); // fcvtn2 v1.4s, v2.2d
        assert(std::memcmp(permute.cpu.v[1].data(), narrowedHigh.data(), sizeof(narrowedHigh)) == 0);
        const std::array<uint32_t, 4> minLeft{0, 0x7fc00042, 14, 19}, minRight{0x80000000, 0x3f800000, 0, 0};
        const std::array<uint32_t, 4> minOutput{0x80000000, 0x3f800000, 0, 0};
        const std::array<uint32_t, 4> maxOutput{0, 0x3f800000, 0, 0};
        std::memcpy(permute.cpu.v[2].data(), minLeft.data(), sizeof(minLeft));
        std::memcpy(permute.cpu.v[3].data(), minRight.data(), sizeof(minRight));
        assert(executeStartupInstruction(permute, 0x0ea3c442)); // fminnm v2.2s, v2.2s, v3.2s
        assert(std::memcmp(permute.cpu.v[2].data(), minOutput.data(), sizeof(minOutput)) == 0);
        std::memcpy(permute.cpu.v[0].data(), minLeft.data(), sizeof(minLeft));
        assert(executeStartupInstruction(permute, 0x0e20c440)); // fmaxnm v0.2s, v2.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), maxOutput.data(), sizeof(maxOutput)) == 0);
        const std::array<uint32_t, 4> leftMax{0x80000000, 0x7fc00042, 0x7f800017, 0xc0000000};
        const std::array<uint32_t, 4> rightMax{0, 0x3f800000, 0x7fc00051, 0x40400000};
        const std::array<uint32_t, 4> expectedMax{0, 0x7fc00042, 0x7fc00017, 0x40400000};
        std::memcpy(permute.cpu.v[16].data(), leftMax.data(), sizeof(leftMax));
        std::memcpy(permute.cpu.v[21].data(), rightMax.data(), sizeof(rightMax));
        permute.cpu.fpsr = fpsrSaturation;
        assert(executeStartupInstruction(permute, 0x4e35f610)); // fmax v16.4s, v16.4s, v21.4s
        assert(std::memcmp(permute.cpu.v[16].data(), expectedMax.data(), sizeof(expectedMax)) == 0);
        assert(permute.cpu.fpsr == (fpsrSaturation | 1));
        const std::array<uint32_t, 4> expectedMin{0x80000000, 0x7fc00042, 0, 0};
        std::memcpy(permute.cpu.v[2].data(), minLeft.data(), sizeof(minLeft));
        std::memcpy(permute.cpu.v[3].data(), minRight.data(), sizeof(minRight));
        permute.cpu.fpsr = 0;
        assert(executeStartupInstruction(permute, 0x0ea3f442)); // fmin v2.2s, v2.2s, v3.2s
        assert(std::memcmp(permute.cpu.v[2].data(), expectedMin.data(), sizeof(expectedMin)) == 0);
        assert(permute.cpu.fpsr == 0);
        const std::array<double, 2> leftDoubleMax{-3.0, 0.5}, rightDoubleMax{-1.0, 0.125};
        const std::array<double, 2> expectedDoubleMax{-1.0, 0.5};
        std::memcpy(permute.cpu.v[2].data(), leftDoubleMax.data(), sizeof(leftDoubleMax));
        std::memcpy(permute.cpu.v[1].data(), rightDoubleMax.data(), sizeof(rightDoubleMax));
        assert(executeStartupInstruction(permute, 0x4e61f441)); // fmax v1.2d, v2.2d, v1.2d
        assert(std::memcmp(permute.cpu.v[1].data(), expectedDoubleMax.data(), sizeof(expectedDoubleMax)) == 0);
        assert(!executeStartupInstruction(permute, 0x0e61f441)); // unallocated 1D vector form
        const std::array<float, 4> nearestInput{0.5f, 1.5f, 2.5f, -0.5f};
        const std::array<uint32_t, 4> nearestResult{0, 2, 2, 0};
        std::memcpy(permute.cpu.v[16].data(), nearestInput.data(), sizeof(nearestInput));
        permute.cpu.fpsr = fpsrSaturation;
        assert(executeStartupInstruction(permute, 0x6e21aa10)); // fcvtnu v16.4s, v16.4s
        assert(std::memcmp(permute.cpu.v[16].data(), nearestResult.data(), sizeof(nearestResult)) == 0);
        assert(permute.cpu.fpsr == (fpsrSaturation | 16));
        const std::array<double, 2> clippedInput{-0.75, 0x1p64};
        const std::array<uint64_t, 2> clippedResult{0, UINT64_MAX};
        std::memcpy(permute.cpu.v[2].data(), clippedInput.data(), sizeof(clippedInput));
        permute.cpu.fpsr = 0;
        assert(executeStartupInstruction(permute, 0x6e61a841)); // fcvtnu v1.2d, v2.2d
        assert(std::memcmp(permute.cpu.v[1].data(), clippedResult.data(), sizeof(clippedResult)) == 0);
        assert(permute.cpu.fpsr == 1);
        assert(!executeStartupInstruction(permute, 0x2e61a841)); // unallocated 1D vector form
        const std::array<float, 2> signedInput{-1.5f, -2.5f};
        const std::array<int32_t, 4> signedResult{-2, -2, 0, 0};
        std::memcpy(permute.cpu.v[2].data(), signedInput.data(), sizeof(signedInput));
        permute.cpu.fpsr = 0;
        assert(executeStartupInstruction(permute, 0x0e21a841)); // fcvtns v1.2s, v2.2s
        assert(std::memcmp(permute.cpu.v[1].data(), signedResult.data(), sizeof(signedResult)) == 0);
        assert(permute.cpu.fpsr == 16);
        permute.cpu.fpsr = 0;
        const std::array<int64_t, 2> positiveAndNegative{1, INT64_MIN}, zeroAndPositive{0, INT64_MAX};
        const std::array<uint64_t, 2> firstGreater{UINT64_MAX, 0}, secondGreater{0, UINT64_MAX};
        std::memcpy(permute.cpu.v[2].data(), positiveAndNegative.data(), sizeof(positiveAndNegative));
        assert(executeStartupInstruction(permute, 0x4ee08843)); // cmgt v3.2d, v2.2d, #0
        assert(std::memcmp(permute.cpu.v[3].data(), firstGreater.data(), sizeof(firstGreater)) == 0);
        std::memcpy(permute.cpu.v[2].data(), zeroAndPositive.data(), sizeof(zeroAndPositive));
        assert(executeStartupInstruction(permute, 0x4ee08843));
        assert(std::memcmp(permute.cpu.v[3].data(), secondGreater.data(), sizeof(secondGreater)) == 0);
        std::memcpy(permute.cpu.v[1].data(), positiveAndNegative.data(), sizeof(positiveAndNegative));
        assert(executeStartupInstruction(permute, 0x6ee09823)); // cmle v3.2d, v1.2d, #0
        assert(std::memcmp(permute.cpu.v[3].data(), secondGreater.data(), sizeof(secondGreater)) == 0);
        std::memcpy(permute.cpu.v[1].data(), zeroAndPositive.data(), sizeof(zeroAndPositive));
        assert(executeStartupInstruction(permute, 0x6ee09823));
        assert(std::memcmp(permute.cpu.v[3].data(), firstGreater.data(), sizeof(firstGreater)) == 0);
        const std::array<uint16_t, 8> reductionInput{0xffff, 0x7fff, 3, 0xfffe, 0, 1, 2, 0};
        const std::array<uint16_t, 8> reductionOutput{3};
        std::memcpy(permute.cpu.v[2].data(), reductionInput.data(), sizeof(reductionInput));
        assert(executeStartupInstruction(permute, 0x2e71a842)); // uminv h2, v2.4h
        assert(std::memcmp(permute.cpu.v[2].data(), reductionOutput.data(), sizeof(reductionOutput)) == 0);
        const std::array<int16_t, 8> signedHalfwords{-301, -9, -32768, -120, 302, 303, 304, 305};
        const std::array<int16_t, 8> largestHalfword{-9};
        std::memcpy(permute.cpu.v[0].data(), signedHalfwords.data(), sizeof(signedHalfwords));
        assert(executeStartupInstruction(permute, 0x0e70a800)); // smaxv h0, v0.4h
        assert(std::memcmp(permute.cpu.v[0].data(), largestHalfword.data(), sizeof(largestHalfword)) == 0);
        const std::array<int8_t, 16> signedBytes{6, 7, 8, -1, -4, 127, -128, 25};
        const std::array<int8_t, 16> smallestByte{-128};
        std::memcpy(permute.cpu.v[2].data(), signedBytes.data(), sizeof(signedBytes));
        assert(executeStartupInstruction(permute, 0x4e31a841)); // sminv b1, v2.16b
        assert(std::memcmp(permute.cpu.v[1].data(), smallestByte.data(), sizeof(smallestByte)) == 0);
        const std::array<int32_t, 4> signedWords{INT32_MIN, -2, 25, 951};
        const std::array<int32_t, 4> largestWord{951};
        std::memcpy(permute.cpu.v[4].data(), signedWords.data(), sizeof(signedWords));
        assert(executeStartupInstruction(permute, 0x4eb0a883)); // smaxv s3, v4.4s
        assert(std::memcmp(permute.cpu.v[3].data(), largestWord.data(), sizeof(largestWord)) == 0);
        const int32_t negativeLane = -123;
        std::memcpy(permute.cpu.v[0].data(), &negativeLane, sizeof(negativeLane));
        assert(executeStartupInstruction(permute, 0x4e042c0b)); // smov x11, v0.s[0]
        assert(permute.cpu.x[11] == uint64_t(-123));
        permute.cpu.v[2][15] = 0x80;
        assert(executeStartupInstruction(permute, 0x0e1f2c49)); // smov w9, v2.b[15]
        assert(permute.cpu.x[9] == 0xffffff80);
        const float fixedSingle = 1.625125f;
        const double fixedDouble = -1.753;
        const double unsignedOverflow = 1.0;
        const float signedOverflow = 1.0f;
        std::memcpy(permute.cpu.v[0].data(), &fixedSingle, sizeof(fixedSingle));
        std::memcpy(permute.cpu.v[1].data(), &fixedDouble, sizeof(fixedDouble));
        std::memcpy(permute.cpu.v[2].data(), &unsignedOverflow, sizeof(unsignedOverflow));
        std::memcpy(permute.cpu.v[3].data(), &signedOverflow, sizeof(signedOverflow));
        assert(executeStartupInstruction(permute, 0x9e19d019)); // fcvtzu x25, s0, #12
        assert(permute.cpu.x[25] == 6656);
        assert(executeStartupInstruction(permute, 0x1e58e439)); // fcvtzs w25, d1, #7
        assert(permute.cpu.x[25] == uint32_t(-224));
        assert(executeStartupInstruction(permute, 0x1e59805b)); // fcvtzu w27, d2, #32
        assert(permute.cpu.x[27] == UINT32_MAX);
        assert(executeStartupInstruction(permute, 0x9e18007c)); // fcvtzs x28, s3, #64
        assert(permute.cpu.x[28] == INT64_MAX);
        assert(!executeStartupInstruction(permute, 0x1e18007c)); // 64 fractional bits in a W result.
        const std::array<float, 4> ceilSingles{-0.5f, 2.1f, 17, 42}, ceilSinglesResult{-0.0f, 3, 0, 0};
        std::memcpy(permute.cpu.v[1].data(), ceilSingles.data(), sizeof(ceilSingles));
        assert(executeStartupInstruction(permute, 0x0ea18821)); // frintp v1.2s, v1.2s
        assert(std::memcmp(permute.cpu.v[1].data(), ceilSinglesResult.data(), sizeof(ceilSinglesResult)) == 0);
        const std::array<uint16_t, 8> roundedInput{0, 127, 128, 255, 256, 383, 384, 65535};
        const std::array<uint16_t, 8> roundedShift{0, 0, 1, 1, 1, 1, 2, 256};
        std::memcpy(permute.cpu.v[4].data(), roundedInput.data(), sizeof(roundedInput));
        assert(executeStartupInstruction(permute, 0x6f182485)); // urshr v5.8h, v4.8h, #8
        assert(std::memcmp(permute.cpu.v[5].data(), roundedShift.data(), sizeof(roundedShift)) == 0);
        const std::array<uint16_t, 8> roundedSum{0, 127, 129, 256, 257, 384, 386, 255};
        std::memcpy(permute.cpu.v[0].data(), roundedInput.data(), sizeof(roundedInput));
        assert(executeStartupInstruction(permute, 0x6f183400)); // ursra v0.8h, v0.8h, #8
        assert(std::memcmp(permute.cpu.v[0].data(), roundedSum.data(), sizeof(roundedSum)) == 0);
        const std::array<uint64_t, 2> wideInput{UINT64_MAX, uint64_t{1} << 63};
        const std::array<uint64_t, 2> wideAccumulator{UINT64_MAX, 17}, wideRoundedSum{0, 18};
        std::memcpy(permute.cpu.v[2].data(), wideInput.data(), sizeof(wideInput));
        std::memcpy(permute.cpu.v[1].data(), wideAccumulator.data(), sizeof(wideAccumulator));
        assert(executeStartupInstruction(permute, 0x4f403441)); // srsra v1.2d, v2.2d, #64
        assert(std::memcmp(permute.cpu.v[1].data(), wideAccumulator.data(), sizeof(wideAccumulator)) == 0);
        assert(executeStartupInstruction(permute, 0x6f403441)); // ursra v1.2d, v2.2d, #64
        assert(std::memcmp(permute.cpu.v[1].data(), wideRoundedSum.data(), sizeof(wideRoundedSum)) == 0);
        assert(!executeStartupInstruction(permute, 0x2f403441)); // unallocated 1D vector form
        assert(executeStartupInstruction(permute, 0x2e254082)); // raddhn v2.8b, v4.8h, v5.8h
        const std::array<uint8_t, 16> narrowedSum{0, 0, 1, 1, 1, 2, 2, 1};
        assert(permute.cpu.v[2] == narrowedSum);
        assert(executeStartupInstruction(permute, 0x6e254082)); // raddhn2 v2.16b, v4.8h, v5.8h
        const std::array<uint8_t, 16> duplicatedSum{0, 0, 1, 1, 1, 2, 2, 1, 0, 0, 1, 1, 1, 2, 2, 1};
        assert(permute.cpu.v[2] == duplicatedSum);
        const std::array<float, 4> squareInput{2.25f, -0.0f, 6.0f, 8.0f};
        const std::array<float, 4> squareRoot{1.5f, -0.0f, 0.0f, 0.0f};
        std::memcpy(permute.cpu.v[0].data(), squareInput.data(), sizeof(squareInput));
        assert(executeStartupInstruction(permute, 0x2ea1f800)); // fsqrt v0.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), squareRoot.data(), sizeof(squareRoot)) == 0);
        const float toCeil = -4.75f, outOfRange = 0x1p31f;
        std::memcpy(permute.cpu.v[1].data(), &toCeil, sizeof(toCeil));
        assert(executeStartupInstruction(permute, 0x1e280028)); // fcvtps w8, s1
        assert(permute.cpu.x[8] == uint32_t(-4));
        assert(executeStartupInstruction(permute, 0x1e300034)); // fcvtms w20, s1
        assert(permute.cpu.x[20] == uint32_t(-5));
        std::memcpy(permute.cpu.v[1].data(), &outOfRange, sizeof(outOfRange));
        assert(executeStartupInstruction(permute, 0x1e280028));
        assert(permute.cpu.x[8] == INT32_MAX);
        const double half = -2.5, beyondSignedMax = 0x1p63;
        std::memcpy(permute.cpu.v[5].data(), &half, sizeof(half));
        assert(executeStartupInstruction(permute, 0x9e6400a9)); // fcvtas x9, d5
        assert(permute.cpu.x[9] == uint64_t(-3));
        assert(executeStartupInstruction(permute, 0x9e7000aa)); // fcvtms x10, d5
        assert(permute.cpu.x[10] == uint64_t(-3));
        std::memcpy(permute.cpu.v[5].data(), &beyondSignedMax, sizeof(beyondSignedMax));
        assert(executeStartupInstruction(permute, 0x9e6400a9));
        assert(permute.cpu.x[9] == INT64_MAX);
        permute.cpu.v[1] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const std::array<uint16_t, 8> wideLower{0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800};
        const std::array<uint16_t, 8> wideUpper{0x900, 0xa00, 0xb00, 0xc00, 0xd00, 0xe00, 0xf00, 0x1000};
        assert(executeStartupInstruction(permute, 0x2e213820)); // shll v0.8h, v1.8b, #8
        assert(std::memcmp(permute.cpu.v[0].data(), wideLower.data(), sizeof(wideLower)) == 0);
        assert(executeStartupInstruction(permute, 0x6e213820)); // shll2 v0.8h, v1.16b, #8
        assert(std::memcmp(permute.cpu.v[0].data(), wideUpper.data(), sizeof(wideUpper)) == 0);
        const std::array<int16_t, 8> shiftInput{-1, 3, -32768, 32767, -5, 4, -300, 1024};
        const std::array<int32_t, 4> shiftLower{-1, 3, -32768, 32767}, shiftUpper{-40, 32, -2400, 8192};
        std::memcpy(permute.cpu.v[3].data(), shiftInput.data(), sizeof(shiftInput));
        std::memcpy(permute.cpu.v[8].data(), shiftInput.data(), sizeof(shiftInput));
        assert(executeStartupInstruction(permute, 0x0f10a462)); // sshll v2.4s, v3.4h, #0
        assert(std::memcmp(permute.cpu.v[2].data(), shiftLower.data(), sizeof(shiftLower)) == 0);
        assert(executeStartupInstruction(permute, 0x4f13a507)); // sshll2 v7.4s, v8.8h, #3
        assert(std::memcmp(permute.cpu.v[7].data(), shiftUpper.data(), sizeof(shiftUpper)) == 0);
        const auto minNumber = [&](uint32_t a, uint32_t b, uint32_t expectedBits) {
            std::memcpy(permute.cpu.v[6].data(), &a, sizeof(a));
            std::memcpy(permute.cpu.v[0].data(), &b, sizeof(b));
            assert(executeStartupInstruction(permute, 0x1e2078c0)); // fminnm s0, s6, s0
            assert(std::memcmp(permute.cpu.v[0].data(), &expectedBits, sizeof(expectedBits)) == 0);
        };
        minNumber(0, 0x80000000, 0x80000000);
        minNumber(0x7fc00042, 0x3f800000, 0x3f800000);
        minNumber(0x7f800042, 0x3f800000, 0x7fc00042);
        const std::array<uint32_t, 4> vectorFloat{0xbff00000, 0x4f000000, UINT32_MAX, UINT32_MAX};
        const std::array<uint32_t, 4> vectorInteger{UINT32_MAX, INT32_MAX, 0, 0};
        std::memcpy(permute.cpu.v[0].data(), vectorFloat.data(), sizeof(vectorFloat));
        assert(executeStartupInstruction(permute, 0x0ea1b800)); // fcvtzs v0.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), vectorInteger.data(), sizeof(vectorInteger)) == 0);
        const std::array<uint32_t, 4> unsignedLanes{0, 17, 0x80000000, UINT32_MAX};
        const std::array<float, 4> convertedUnsigned{0, 17, 0x1p31f, 0x1p32f};
        std::memcpy(permute.cpu.v[0].data(), unsignedLanes.data(), sizeof(unsignedLanes));
        assert(executeStartupInstruction(permute, 0x6e21d800)); // ucvtf v0.4s, v0.4s
        assert(std::memcmp(permute.cpu.v[0].data(), convertedUnsigned.data(), sizeof(convertedUnsigned)) == 0);
        const std::array<int64_t, 2> signedLanes{-9, 100};
        const std::array<double, 2> convertedSigned{-9, 100};
        std::memcpy(permute.cpu.v[4].data(), signedLanes.data(), sizeof(signedLanes));
        assert(executeStartupInstruction(permute, 0x4e61d883)); // scvtf v3.2d, v4.2d
        assert(std::memcmp(permute.cpu.v[3].data(), convertedSigned.data(), sizeof(convertedSigned)) == 0);
        const std::array<float, 4> twoSingles{1, 1, 0, 0}, fourSingles{-2, -2, -2, -2};
        const std::array<double, 2> twoDoubles{0.75, 0.75};
        assert(executeStartupInstruction(permute, 0x0f03f600)); // fmov v0.2s, #1.0
        assert(std::memcmp(permute.cpu.v[0].data(), twoSingles.data(), sizeof(twoSingles)) == 0);
        assert(executeStartupInstruction(permute, 0x4f04f401)); // fmov v1.4s, #-2.0
        assert(std::memcmp(permute.cpu.v[1].data(), fourSingles.data(), sizeof(fourSingles)) == 0);
        assert(executeStartupInstruction(permute, 0x6f03f502)); // fmov v2.2d, #0.75
        assert(std::memcmp(permute.cpu.v[2].data(), twoDoubles.data(), sizeof(twoDoubles)) == 0);
        const std::array<uint32_t, 4> mslByte{0xf2ff, 0xf2ff, 0xf2ff, 0xf2ff};
        const std::array<uint32_t, 4> mslHalfword{0x12ffff, 0x12ffff, 0x12ffff, 0x12ffff};
        const std::array<uint32_t, 4> mslInverted{0xfffffe00, 0xfffffe00, 0, 0};
        assert(executeStartupInstruction(permute, 0x4f07c640)); // movi v0.4s, #242, msl #8
        assert(std::memcmp(permute.cpu.v[0].data(), mslByte.data(), sizeof(mslByte)) == 0);
        assert(executeStartupInstruction(permute, 0x4f00d642)); // movi v2.4s, #18, msl #16
        assert(std::memcmp(permute.cpu.v[2].data(), mslHalfword.data(), sizeof(mslHalfword)) == 0);
        assert(executeStartupInstruction(permute, 0x2f00c423)); // mvni v3.2s, #1, msl #8
        assert(std::memcmp(permute.cpu.v[3].data(), mslInverted.data(), sizeof(mslInverted)) == 0);
        const std::array<int16_t, 8> firstSigned{-5, 2, -11, 32767, -5, -7, 0, -32768};
        const std::array<int16_t, 8> secondSigned{3, -2, -9, -32768, -6, 11, 0, -4};
        const std::array<int16_t, 8> maxSigned{3, 2, -9, 32767, -5, 11, 0, -4};
        const std::array<int16_t, 8> minSigned{-5, -2, -11, -32768};
        std::memcpy(permute.cpu.v[7].data(), firstSigned.data(), sizeof(firstSigned));
        std::memcpy(permute.cpu.v[1].data(), secondSigned.data(), sizeof(secondSigned));
        assert(executeStartupInstruction(permute, 0x4e6164e7)); // smax v7.8h, v7.8h, v1.8h
        assert(std::memcmp(permute.cpu.v[7].data(), maxSigned.data(), sizeof(maxSigned)) == 0);
        std::memcpy(permute.cpu.v[2].data(), firstSigned.data(), sizeof(firstSigned));
        std::memcpy(permute.cpu.v[3].data(), secondSigned.data(), sizeof(secondSigned));
        assert(executeStartupInstruction(permute, 0x0e636c44)); // smin v4.4h, v2.4h, v3.4h
        assert(std::memcmp(permute.cpu.v[4].data(), minSigned.data(), sizeof(minSigned)) == 0);
        const std::array<uint16_t, 8> unsignedLeft{0, 256, 128, 65535}, unsignedRight{1, 2, 512, 65535};
        const std::array<int32_t, 4> unsignedDifference{-1, 254, -384, 0}, highDifference{1, -18, 0, -32764};
        std::memcpy(permute.cpu.v[7].data(), unsignedLeft.data(), sizeof(unsignedLeft));
        std::memcpy(permute.cpu.v[6].data(), unsignedRight.data(), sizeof(unsignedRight));
        assert(executeStartupInstruction(permute, 0x2e6620f0)); // usubl v16.4s, v7.4h, v6.4h
        assert(std::memcmp(permute.cpu.v[16].data(), unsignedDifference.data(), sizeof(unsignedDifference)) == 0);
        assert(executeStartupInstruction(permute, 0x4e632041)); // ssubl2 v1.4s, v2.8h, v3.8h
        assert(std::memcmp(permute.cpu.v[1].data(), highDifference.data(), sizeof(highDifference)) == 0);
        const std::array<uint32_t, 4> words{UINT32_MAX, 0x80000000, 3, 7};
        const std::array<uint32_t, 4> factors{3, 8, UINT32_MAX, 5};
        const std::array<uint32_t, 4> indexedProduct{UINT32_MAX - 2, 0x80000000, 9, 21};
        std::memcpy(permute.cpu.v[16].data(), words.data(), sizeof(words));
        std::memcpy(permute.cpu.v[5].data(), factors.data(), sizeof(factors));
        assert(executeStartupInstruction(permute, 0x4f858210)); // mul v16.4s, v16.4s, v5.s[0]
        assert(std::memcmp(permute.cpu.v[16].data(), indexedProduct.data(), sizeof(indexedProduct)) == 0);
        std::memcpy(permute.cpu.v[2].data(), words.data(), sizeof(words));
        std::memcpy(permute.cpu.v[31].data(), factors.data(), sizeof(factors));
        const std::array<uint32_t, 4> highWordProduct{UINT32_MAX - 4, 0x80000000, 15, 35};
        assert(executeStartupInstruction(permute, 0x4fbf8841)); // mul v1.4s, v2.4s, v31.s[3]
        assert(std::memcmp(permute.cpu.v[1].data(), highWordProduct.data(), sizeof(highWordProduct)) == 0);
        const std::array<uint32_t, 4> indexedSum{UINT32_MAX - 3, 0, 12, 28};
        assert(executeStartupInstruction(permute, 0x6f850841)); // mla v1.4s, v2.4s, v5.s[2]
        assert(std::memcmp(permute.cpu.v[1].data(), indexedSum.data(), sizeof(indexedSum)) == 0);
        const std::array<uint16_t, 8> halfwords{2, 3, 32768, 65535, 1, 2, 3, 4};
        const std::array<uint16_t, 8> halfwordFactors{1, 2, 4, 5, 6, 8, 9, 3};
        const std::array<uint16_t, 8> halfwordProduct{6, 9, 32768, 65533};
        const std::array<uint16_t, 8> indexedDifference{65532, 65530, 0, 2, 65531, 65526, 65521, 65516};
        std::memcpy(permute.cpu.v[2].data(), halfwords.data(), sizeof(halfwords));
        std::memcpy(permute.cpu.v[3].data(), halfwordFactors.data(), sizeof(halfwordFactors));
        std::memcpy(permute.cpu.v[5].data(), halfwordFactors.data(), sizeof(halfwordFactors));
        assert(executeStartupInstruction(permute, 0x0f738841)); // mul v1.4h, v2.4h, v3.h[7]
        assert(std::memcmp(permute.cpu.v[1].data(), halfwordProduct.data(), sizeof(halfwordProduct)) == 0);
        assert(executeStartupInstruction(permute, 0x6f754041)); // mls v1.8h, v2.8h, v5.h[3]
        assert(std::memcmp(permute.cpu.v[1].data(), indexedDifference.data(), sizeof(indexedDifference)) == 0);
        const std::array<int16_t, 8> signedProductsLeft{-2, 32767, -32768, 5, 2, -7, 5, -1};
        const std::array<int16_t, 8> signedProductsRight{3, -2, -32768, -5, -2, -3, 4, -32768};
        const std::array<int32_t, 4> signedAccumulators{-1, 2, -1, 0};
        const std::array<int32_t, 4> signedProductSum{-7, -65532, 1073741823, -25};
        const std::array<int32_t, 4> signedHighProduct{-4, 21, 20, 32768};
        std::memcpy(permute.cpu.v[17].data(), signedProductsLeft.data(), sizeof(signedProductsLeft));
        std::memcpy(permute.cpu.v[2].data(), signedProductsRight.data(), sizeof(signedProductsRight));
        std::memcpy(permute.cpu.v[5].data(), signedAccumulators.data(), sizeof(signedAccumulators));
        assert(executeStartupInstruction(permute, 0x0e628225)); // smlal v5.4s, v17.4h, v2.4h
        assert(std::memcmp(permute.cpu.v[5].data(), signedProductSum.data(), sizeof(signedProductSum)) == 0);
        assert(executeStartupInstruction(permute, 0x4e62c225)); // smull2 v5.4s, v17.8h, v2.8h
        assert(std::memcmp(permute.cpu.v[5].data(), signedHighProduct.data(), sizeof(signedHighProduct)) == 0);
        const std::array<uint32_t, 4> highProductsLeft{10, 20, UINT32_MAX, 0x80000000};
        const std::array<uint32_t, 4> highProductsRight{30, 40, UINT32_MAX, 0x80000001};
        const std::array<uint64_t, 2> unsignedAccumulators{10, 0x100000000};
        const std::array<uint64_t, 2> unsignedProductSum{0xfffffffe0000000b, 0x4000000180000000};
        std::memcpy(permute.cpu.v[3].data(), highProductsLeft.data(), sizeof(highProductsLeft));
        std::memcpy(permute.cpu.v[4].data(), highProductsRight.data(), sizeof(highProductsRight));
        std::memcpy(permute.cpu.v[8].data(), unsignedAccumulators.data(), sizeof(unsignedAccumulators));
        assert(executeStartupInstruction(permute, 0x6ea48068)); // umlal2 v8.2d, v3.4s, v4.4s
        assert(std::memcmp(permute.cpu.v[8].data(), unsignedProductSum.data(), sizeof(unsignedProductSum)) == 0);
        assert(executeStartupInstruction(permute, 0x6ea4a068)); // umlsl2 v8.2d, v3.4s, v4.4s
        assert(std::memcmp(permute.cpu.v[8].data(), unsignedAccumulators.data(), sizeof(unsignedAccumulators)) == 0);
        const std::array<uint32_t, 4> insertSource{0x05060789, 0xaabbccab, 1, 2};
        const std::array<uint32_t, 4> insertDestination{0xdeadbeef, 0x7845789a, 3, 4};
        const std::array<uint32_t, 4> inserted{0x89adbeef, 0xab45789a, 0, 0};
        std::memcpy(permute.cpu.v[7].data(), insertSource.data(), sizeof(insertSource));
        std::memcpy(permute.cpu.v[13].data(), insertDestination.data(), sizeof(insertDestination));
        assert(executeStartupInstruction(permute, 0x2f3854ed)); // sli v13.2s, v7.2s, #24
        assert(std::memcmp(permute.cpu.v[13].data(), inserted.data(), sizeof(inserted)) == 0);
        const std::array<uint64_t, 2> aliasedInsert{3, 0x8000000000000002ULL};
        const std::array<uint64_t, 2> aliasedResult{0x8000000000000003ULL, 2};
        std::memcpy(permute.cpu.v[1].data(), aliasedInsert.data(), sizeof(aliasedInsert));
        assert(executeStartupInstruction(permute, 0x6f7f5421)); // sli v1.2d, v1.2d, #63
        assert(std::memcmp(permute.cpu.v[1].data(), aliasedResult.data(), sizeof(aliasedResult)) == 0);
        permute.cpu.v[3].fill(0x17);
        permute.cpu.v[2].fill(0xff);
        assert(executeStartupInstruction(permute, 0x2f085462)); // sli v2.8b, v3.8b, #0
        const std::array<uint8_t, 16> insertedBytes{0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17};
        assert(permute.cpu.v[2] == insertedBytes);
        assert(!executeStartupInstruction(permute, 0x2f7f5421)); // unallocated 1D vector form
        permute.cpu.v[6] = {1, 254, 0, 255, 2, 100, 128, 9, 255, 128, 254, 127, 12, 3, 4, 1};
        permute.cpu.v[3] = {255, 0, 255, 0, 2, 123, 1, 9, 0, 127, 1, 128, 4, 3, 1, 2};
        const std::array<uint16_t, 8> distance{254, 254, 255, 255, 0, 23, 127, 0};
        assert(executeStartupInstruction(permute, 0x2e2370c7)); // uabdl v7.8h, v6.8b, v3.8b
        assert(std::memcmp(permute.cpu.v[7].data(), distance.data(), sizeof(distance)) == 0);
        const std::array<uint16_t, 8> upperDistance{255, 1, 253, 1, 8, 0, 3, 1};
        assert(executeStartupInstruction(permute, 0x6e2370c7)); // uabdl2 v7.8h, v6.16b, v3.16b
        assert(std::memcmp(permute.cpu.v[7].data(), upperDistance.data(), sizeof(upperDistance)) == 0);
        const std::array<uint8_t, 16> signedDistance{2, 2, 1, 1, 0, 23, 129, 0};
        assert(executeStartupInstruction(permute, 0x0e2374c6)); // sabd v6.8b, v6.8b, v3.8b
        assert(permute.cpu.v[6] == signedDistance);
        permute.cpu.v[2] = {255, 255, 0, 1, 7, 11, 128, 2, 1, 2, 3, 4, 5, 6, 7, 8};
        permute.cpu.v[3] = {255, 254, 1, 0, 11, 14, 129, 4, 9, 10, 11, 12, 13, 14, 15, 16};
        assert(executeStartupInstruction(permute, 0x2e230442)); // uhadd v2.8b, v2.8b, v3.8b
        const std::array<uint8_t, 16> halfBytes{255, 254, 0, 0, 9, 12, 128, 3};
        assert(permute.cpu.v[2] == halfBytes);
        const std::array<uint32_t, 4> wideLanes{UINT32_MAX, 1, 8, 0x80000000};
        const std::array<uint32_t, 4> widePeer{UINT32_MAX - 1, UINT32_MAX, 7, 1};
        const std::array<uint32_t, 4> halfWords{UINT32_MAX - 1, 0x80000000, 7, 0x40000000};
        std::memcpy(permute.cpu.v[2].data(), wideLanes.data(), sizeof(wideLanes));
        std::memcpy(permute.cpu.v[3].data(), widePeer.data(), sizeof(widePeer));
        assert(executeStartupInstruction(permute, 0x6ea30462)); // uhadd v2.4s, v3.4s, v3.4s
        assert(permute.cpu.v[2] == permute.cpu.v[3]);
        std::memcpy(permute.cpu.v[2].data(), wideLanes.data(), sizeof(wideLanes));
        assert(executeStartupInstruction(permute, 0x6ea30442)); // uhadd v2.4s, v2.4s, v3.4s
        assert(std::memcmp(permute.cpu.v[2].data(), halfWords.data(), sizeof(halfWords)) == 0);
        assert(!executeStartupInstruction(permute, 0x6ee30442)); // reserved 64-bit lanes
        permute.cpu.fpsr = 1U << 4;
        permute.cpu.v[4] = {255, 254, 150, 0, 10, 30, 127, 255};
        permute.cpu.v[1] = {1, 1, 106, 0, 20, 0, 128, 0};
        permute.cpu.v[16].fill(0xaa);
        assert(executeStartupInstruction(permute, 0x2e210c90)); // uqadd v16.8b, v4.8b, v1.8b
        const std::array<uint8_t, 16> saturatedBytes{255, 255, 255, 0, 30, 30, 255, 255};
        assert(permute.cpu.v[16] == saturatedBytes && permute.cpu.fpsr == (fpsrSaturation | 16));
        const std::array<uint64_t, 2> saturatedLanes{UINT64_MAX, 5}, addition{1, 3};
        const std::array<uint64_t, 2> saturatedSums{UINT64_MAX, 8};
        std::memcpy(permute.cpu.v[4].data(), saturatedLanes.data(), sizeof(saturatedLanes));
        std::memcpy(permute.cpu.v[1].data(), addition.data(), sizeof(addition));
        assert(executeStartupInstruction(permute, 0x6ee10c84)); // uqadd v4.2d, v4.2d, v1.2d
        assert(std::memcmp(permute.cpu.v[4].data(), saturatedSums.data(), sizeof(saturatedSums)) == 0);
        assert(!executeStartupInstruction(permute, 0x2ee10c84)); // unallocated 1D vector form
        const std::array<int16_t, 8> mulLeft{-32768, -32768, 16384, -16384, 32767, 1, -1, 12345};
        const std::array<int16_t, 8> mulRight{-32768, 32767, 1, 1, 32767, 16384, 16384, -23456};
        const std::array<int16_t, 8> mulHigh{32767, -32767, 1, 0, 32766, 1, 0, -8837};
        std::memcpy(permute.cpu.v[28].data(), mulLeft.data(), sizeof(mulLeft));
        std::memcpy(permute.cpu.v[31].data(), mulRight.data(), sizeof(mulRight));
        assert(executeStartupInstruction(permute, 0xd51b443f)); // msr fpsr, xzr
        assert(executeStartupInstruction(permute, 0x6e7fb795)); // sqrdmulh v21.8h, v28.8h, v31.8h
        assert(std::memcmp(permute.cpu.v[21].data(), mulHigh.data(), sizeof(mulHigh)) == 0);
        assert(executeStartupInstruction(permute, 0xd53b4425) && permute.cpu.x[5] == fpsrSaturation);
        const std::array<int32_t, 4> wideMulLeft{INT32_MIN, INT32_MAX, -1, 1};
        const std::array<int32_t, 4> wideMulRight{INT32_MIN, INT32_MAX, 1 << 30, 1 << 30};
        const std::array<int32_t, 4> wideMulHigh{INT32_MAX, INT32_MAX - 1, 0, 1};
        std::memcpy(permute.cpu.v[28].data(), wideMulLeft.data(), sizeof(wideMulLeft));
        std::memcpy(permute.cpu.v[31].data(), wideMulRight.data(), sizeof(wideMulRight));
        assert(executeStartupInstruction(permute, 0x6ebfb79c)); // sqrdmulh v28.4s, v28.4s, v31.4s
        assert(std::memcmp(permute.cpu.v[28].data(), wideMulHigh.data(), sizeof(wideMulHigh)) == 0);
        assert(executeStartupInstruction(permute, 0xd51b443f) && !permute.cpu.fpsr);
        assert(!executeStartupInstruction(permute, 0x6e3fb795)); // reserved byte lanes

        const std::array<int16_t, 8> indexedHalfMultiplier{1, 2, 3, 16384};
        const std::array<int16_t, 8> indexedHalfHigh{-16384, -16384, 8192, -8192, 16383, 0, -1, 6172};
        std::memcpy(permute.cpu.v[17].data(), mulLeft.data(), sizeof(mulLeft));
        std::memcpy(permute.cpu.v[0].data(), indexedHalfMultiplier.data(), sizeof(indexedHalfMultiplier));
        assert(executeStartupInstruction(permute, 0x4f70c234)); // libjpeg: sqdmulh v20.8h, v17.8h, v0.h[3]
        assert(std::memcmp(permute.cpu.v[20].data(), indexedHalfHigh.data(), sizeof(indexedHalfHigh)) == 0);
        assert(!permute.cpu.fpsr && !executeStartupInstruction(permute, 0x4f30c234)); // reserved byte lanes

        const std::array<int16_t, 8> indexedSaturatingMultiplier{1, 2, 3, 4, 5, 6, 7, INT16_MIN};
        const std::array<int16_t, 8> indexedSaturatingHigh{32767, 32767, -16384, 16384};
        std::memcpy(permute.cpu.v[15].data(), indexedSaturatingMultiplier.data(), sizeof(indexedSaturatingMultiplier));
        std::memcpy(permute.cpu.v[10].data(), mulLeft.data(), sizeof(mulLeft));
        permute.cpu.v[9].fill(0xaa);
        assert(executeStartupInstruction(permute, 0x0f7fc949)); // sqdmulh v9.4h, v10.4h, v15.h[7]
        assert(std::memcmp(permute.cpu.v[9].data(), indexedSaturatingHigh.data(), sizeof(indexedSaturatingHigh)) == 0);
        assert(permute.cpu.fpsr == fpsrSaturation);

        const std::array<int32_t, 4> indexedWideHigh{INT32_MIN / 2, INT32_MAX / 2, -1, 0};
        const std::array<int32_t, 4> indexedWideRounded{INT32_MIN / 2, INT32_MAX / 2 + 1, 0, 1};
        std::memcpy(permute.cpu.v[27].data(), wideMulLeft.data(), sizeof(wideMulLeft));
        assert(executeStartupInstruction(permute, 0x4fbfcb72)); // sqdmulh v18.4s, v27.4s, v31.s[3]
        assert(executeStartupInstruction(permute, 0x4fbfdb73)); // sqrdmulh v19.4s, v27.4s, v31.s[3]
        assert(std::memcmp(permute.cpu.v[18].data(), indexedWideHigh.data(), sizeof(indexedWideHigh)) == 0);
        assert(std::memcmp(permute.cpu.v[19].data(), indexedWideRounded.data(), sizeof(indexedWideRounded)) == 0);
        assert(executeStartupInstruction(permute, 0xd51b443f));
        // Exact low-half, high-half and accumulating opcodes from Android libjpeg.
        const std::array<int16_t, 8> longInput{-1, 128, -32768, 32767, 1, -2, 3, -4};
        const std::array<int16_t, 8> longMultiplier{123, -3, 0, 0, 0, 0, 0, 0};
        std::memcpy(permute.cpu.v[16].data(), longInput.data(), sizeof(longInput));
        std::memcpy(permute.cpu.v[17].data(), longInput.data(), sizeof(longInput));
        std::memcpy(permute.cpu.v[1].data(), longMultiplier.data(), sizeof(longMultiplier));
        assert(executeStartupInstruction(permute, 0x0f51a218)); // smull v24.4s, v16.4h, v1.h[1]
        assert(executeStartupInstruction(permute, 0x4f51a212)); // smull2 v18.4s, v16.8h, v1.h[1]
        const std::array<int32_t, 4> longLow{3, -384, 98304, -98301};
        const std::array<int32_t, 4> longHigh{-3, 6, -9, 12};
        assert(std::memcmp(permute.cpu.v[24].data(), longLow.data(), sizeof(longLow)) == 0);
        assert(std::memcmp(permute.cpu.v[18].data(), longHigh.data(), sizeof(longHigh)) == 0);
        assert(executeStartupInstruction(permute, 0x0f512238)); // smlal v24.4s, v17.4h, v1.h[1]
        const std::array<int32_t, 4> longAccumulated{6, -768, 196608, -196602};
        assert(std::memcmp(permute.cpu.v[24].data(), longAccumulated.data(), sizeof(longAccumulated)) == 0);
        const std::array<int32_t, 4> roundedNarrowInput{-1025, -1024, 1023, 1024};
        const std::array<int16_t, 8> roundedNarrowOutput{-1, 0, 0, 1, -1, 0, 0, 1};
        std::memcpy(permute.cpu.v[20].data(), roundedNarrowInput.data(), sizeof(roundedNarrowInput));
        std::memcpy(permute.cpu.v[7].data(), roundedNarrowInput.data(), sizeof(roundedNarrowInput));
        assert(executeStartupInstruction(permute, 0x0f158e91)); // rshrn v17.4h, v20.4s, #11
        assert(executeStartupInstruction(permute, 0x4f158cf1)); // rshrn2 v17.8h, v7.4s, #11
        assert(std::memcmp(permute.cpu.v[17].data(), roundedNarrowOutput.data(), sizeof(roundedNarrowOutput)) == 0);
        // Android libjpeg's inverse transform combines wide products and keeps their high halves.
        const std::array<uint32_t, 4> highNarrowLeft{0xffffffff, 0, 0x12345678, 0x80000000};
        const std::array<uint32_t, 4> highNarrowRight{1, 1, 0x1111aaaa, 0x80000000};
        const std::array<uint16_t, 8> highNarrowSum{0, 0, 0x2346, 0};
        const std::array<uint16_t, 8> highNarrowBoth{0, 0, 0x2346, 0, 0xffff, 0xffff, 0x0122, 0};
        std::memcpy(permute.cpu.v[4].data(), highNarrowLeft.data(), sizeof(highNarrowLeft));
        std::memcpy(permute.cpu.v[18].data(), highNarrowRight.data(), sizeof(highNarrowRight));
        permute.cpu.v[7].fill(0x55);
        assert(executeStartupInstruction(permute, 0x0e724087)); // addhn v7.4h, v4.4s, v18.4s
        assert(std::memcmp(permute.cpu.v[7].data(), highNarrowSum.data(), sizeof(highNarrowSum)) == 0);
        assert(executeStartupInstruction(permute, 0x4e726087)); // subhn2 v7.8h, v4.4s, v18.4s
        assert(std::memcmp(permute.cpu.v[7].data(), highNarrowBoth.data(), sizeof(highNarrowBoth)) == 0);
        const std::array<uint64_t, 2> highNarrowWiderLeft{UINT64_MAX, 0x11223344'80000000};
        const std::array<uint64_t, 2> highNarrowWiderRight{1, 0x00000000'00000001};
        const std::array<uint32_t, 4> highNarrowRounded{0, 0x11223345, 0, 0};
        std::memcpy(permute.cpu.v[4].data(), highNarrowWiderLeft.data(), sizeof(highNarrowWiderLeft));
        std::memcpy(permute.cpu.v[18].data(), highNarrowWiderRight.data(), sizeof(highNarrowWiderRight));
        assert(executeStartupInstruction(permute, 0x2eb24084)); // raddhn v4.2s, v4.2d, v18.2d
        assert(std::memcmp(permute.cpu.v[4].data(), highNarrowRounded.data(), sizeof(highNarrowRounded)) == 0);
        assert(!executeStartupInstruction(permute, 0x0ef24087)); // reserved lane size
        const std::array<int16_t, 8> jpegNarrowInput{INT16_MIN, -514, -512, -2, 2, 505, 510, INT16_MAX};
        const std::array<int8_t, 16> jpegNarrowOutput{-128, -128, -128, 0, 1, 126, 127, 127};
        std::memcpy(permute.cpu.v[7].data(), jpegNarrowInput.data(), sizeof(jpegNarrowInput));
        permute.cpu.v[5].fill(0x44);
        assert(executeStartupInstruction(permute, 0x0f0e9ce5)); // sqrshrn v5.8b, v7.8h, #2
        assert(std::memcmp(permute.cpu.v[5].data(), jpegNarrowOutput.data(), sizeof(jpegNarrowOutput)) == 0);
        assert(permute.cpu.fpsr == fpsrSaturation);
        const std::array<uint16_t, 8> narrowBytesSource{0, 254, 255, 256, 257, 1, 32768, 65535};
        const std::array<uint8_t, 16> narrowBytesResult{0, 254, 255, 255, 255, 1, 255, 255};
        std::memcpy(permute.cpu.v[0].data(), narrowBytesSource.data(), sizeof(narrowBytesSource));
        permute.cpu.v[16].fill(0x51);
        assert(executeStartupInstruction(permute, 0x2e214810)); // uqxtn v16.8b, v0.8h
        assert(permute.cpu.v[16] == narrowBytesResult && permute.cpu.fpsr == fpsrSaturation);
        const std::array<uint64_t, 2> narrowWordsSource{0x1'0000'0000, 0x8765'4321};
        const std::array<uint32_t, 4> narrowWordsResult{0, 1, UINT32_MAX, 0x8765'4321};
        std::memcpy(permute.cpu.v[7].data(), narrowWordsSource.data(), sizeof(narrowWordsSource));
        assert(executeStartupInstruction(permute, 0x6ea148e7)); // uqxtn2 v7.4s, v7.2d
        assert(std::memcmp(permute.cpu.v[7].data(), narrowWordsResult.data(), sizeof(narrowWordsResult)) == 0);
        const std::array<uint32_t, 4> narrowHalvesSource{1, 65535, 42, 0};
        const std::array<uint16_t, 8> narrowHalvesResult{1, 65535, 42, 0};
        std::memcpy(permute.cpu.v[0].data(), narrowHalvesSource.data(), sizeof(narrowHalvesSource));
        assert(executeStartupInstruction(permute, 0x2e614810)); // uqxtn v16.4h, v0.4s
        assert(std::memcmp(permute.cpu.v[16].data(), narrowHalvesResult.data(), sizeof(narrowHalvesResult)) == 0);
        assert(permute.cpu.fpsr == fpsrSaturation); // An in-range conversion preserves the sticky flag.
        assert(executeStartupInstruction(permute, 0xd51b443f) && !permute.cpu.fpsr);
        assert(executeStartupInstruction(permute, 0x2e614810) && !permute.cpu.fpsr);
        {
            const std::array<int16_t, 8> signedBytes{INT16_MIN, -1, 0, 1, 254, 255, 256, INT16_MAX};
            const std::array<uint8_t, 16> saturatedBytes{0, 0, 0, 1, 254, 255, 255, 255};
            std::memcpy(permute.cpu.v[20].data(), signedBytes.data(), sizeof(signedBytes));
            permute.cpu.v[1].fill(0x77);
            assert(executeStartupInstruction(permute, 0x2e212a81)); // sqxtun v1.8b, v20.8h
            assert(permute.cpu.v[1] == saturatedBytes && permute.cpu.fpsr == fpsrSaturation);
            assert(executeStartupInstruction(permute, 0xd51b443f) && !permute.cpu.fpsr);
            const std::array<int16_t, 8> inRange{255, 0, 3, 5, 254, 11, 17, 200};
            const std::array<uint8_t, 16> bothBytes{0, 0, 0, 1, 254, 255, 255, 255,
                                                   255, 0, 3, 5, 254, 11, 17, 200};
            std::memcpy(permute.cpu.v[20].data(), inRange.data(), sizeof(inRange));
            assert(executeStartupInstruction(permute, 0x6e212a81)); // sqxtun2 v1.16b, v20.8h
            assert(permute.cpu.v[1] == bothBytes && !permute.cpu.fpsr);
            const std::array<int32_t, 4> signedHalves{INT32_MIN, 65535, 65536, -1};
            const std::array<uint16_t, 8> saturatedHalves{0, 65535, 65535, 0};
            std::memcpy(permute.cpu.v[1].data(), signedHalves.data(), sizeof(signedHalves));
            assert(executeStartupInstruction(permute, 0x2e612821)); // sqxtun v1.4h, v1.4s
            assert(std::memcmp(permute.cpu.v[1].data(), saturatedHalves.data(), sizeof(saturatedHalves)) == 0);
            assert(permute.cpu.fpsr == fpsrSaturation);
            const std::array<int64_t, 2> signedWords{INT64_MIN, 0x1'0000'0000};
            const std::array<uint32_t, 4> saturatedWords{0, 0x80000000, 0, UINT32_MAX};
            std::memcpy(permute.cpu.v[1].data(), signedWords.data(), sizeof(signedWords));
            assert(executeStartupInstruction(permute, 0x6ea12821)); // sqxtun2 v1.4s, v1.2d
            assert(std::memcmp(permute.cpu.v[1].data(), saturatedWords.data(), sizeof(saturatedWords)) == 0);
            assert(!executeStartupInstruction(permute, 0x2ee12a81)); // reserved lane width
        }

        assert(!executeStartupInstruction(permute, 0x2ee14810)); // reserved 64-bit destinations
        const std::array<float, 4> signedFixedSource{1.78125f, -1.7890625f, 8, 9};
        const std::array<int32_t, 4> signedFixedResult{114, -114, 0, 0};
        std::memcpy(permute.cpu.v[0].data(), signedFixedSource.data(), sizeof(signedFixedSource));
        permute.cpu.v[2].fill(0x51);
        assert(executeStartupInstruction(permute, 0x0f3afc02)); // fcvtzs v2.2s, v0.2s, #6
        assert(std::memcmp(permute.cpu.v[2].data(), signedFixedResult.data(), sizeof(signedFixedResult)) == 0);
        const std::array<double, 2> wideFixedSource{0.125, -1};
        const std::array<int64_t, 2> wideFixedResult{int64_t{1} << 61, INT64_MIN};
        std::memcpy(permute.cpu.v[4].data(), wideFixedSource.data(), sizeof(wideFixedSource));
        assert(executeStartupInstruction(permute, 0x4f40fc84)); // fcvtzs v4.2d, v4.2d, #64
        assert(std::memcmp(permute.cpu.v[4].data(), wideFixedResult.data(), sizeof(wideFixedResult)) == 0);
        const std::array<float, 4> unsignedFixedSource{0.5f, -1, 1, std::numeric_limits<float>::quiet_NaN()};
        const std::array<uint32_t, 4> unsignedFixedResult{uint32_t{1} << 31, 0, UINT32_MAX, 0};
        std::memcpy(permute.cpu.v[3].data(), unsignedFixedSource.data(), sizeof(unsignedFixedSource));
        assert(executeStartupInstruction(permute, 0x6f20fc63)); // fcvtzu v3.4s, v3.4s, #32
        assert(std::memcmp(permute.cpu.v[3].data(), unsignedFixedResult.data(), sizeof(unsignedFixedResult)) == 0);
        const std::array<double, 2> scalarFixedSource{-2.875, 3};
        const std::array<int64_t, 2> scalarFixedResult{-5, 0};
        std::memcpy(permute.cpu.v[6].data(), scalarFixedSource.data(), sizeof(scalarFixedSource));
        assert(executeStartupInstruction(permute, 0x5f7ffcc6)); // fcvtzs d6, d6, #1
        assert(std::memcmp(permute.cpu.v[6].data(), scalarFixedResult.data(), sizeof(scalarFixedResult)) == 0);
        assert(!executeStartupInstruction(permute, 0x0f40fc84)); // reserved 1D vector form
        assert(!executeStartupInstruction(permute, 0x0f1afc02)); // reserved element width
        const std::array<int32_t, 4> shiftSource{-3, INT32_MIN, 3, 5};
        const std::array<uint32_t, 4> shiftCounts{0x12ff, 0x80, 2, 32};
        const std::array<int32_t, 4> shortShiftResult{-2, -1, 0, 0};
        const std::array<int32_t, 4> longShiftResult{-2, -1, 12, 0};
        const std::array<int32_t, 4> unsignedShiftResult{INT32_MAX - 1, 0, 12, 0};
        std::memcpy(permute.cpu.v[2].data(), shiftSource.data(), sizeof(shiftSource));
        std::memcpy(permute.cpu.v[3].data(), shiftCounts.data(), sizeof(shiftCounts));
        assert(executeStartupInstruction(permute, 0x0ea34443)); // sshl v3.2s, v2.2s, v3.2s
        assert(std::memcmp(permute.cpu.v[3].data(), shortShiftResult.data(), sizeof(shortShiftResult)) == 0);
        std::memcpy(permute.cpu.v[3].data(), shiftCounts.data(), sizeof(shiftCounts));
        assert(executeStartupInstruction(permute, 0x4ea34443)); // sshl v3.4s, v2.4s, v3.4s
        assert(std::memcmp(permute.cpu.v[3].data(), longShiftResult.data(), sizeof(longShiftResult)) == 0);
        std::memcpy(permute.cpu.v[3].data(), shiftCounts.data(), sizeof(shiftCounts));
        assert(executeStartupInstruction(permute, 0x6ea34443)); // ushl v3.4s, v2.4s, v3.4s
        assert(std::memcmp(permute.cpu.v[3].data(), unsignedShiftResult.data(), sizeof(unsignedShiftResult)) == 0);
        assert(!executeStartupInstruction(permute, 0x0ee34443)); // reserved 1D vector form
        const std::array<float, 4> fmaLeft{1 + 0x1p-23f, -2, 3, 4};
        const std::array<float, 4> fmaRight{1 - 0x1p-23f, 4, -5, 6};
        const std::array<float, 4> fmaAccumulator{-1, 2, 3, 4};
        const std::array<float, 4> fmaSum{-0x1p-46f, -6, -12, 28};
        std::memcpy(permute.cpu.v[17].data(), fmaLeft.data(), sizeof(fmaLeft));
        std::memcpy(permute.cpu.v[1].data(), fmaRight.data(), sizeof(fmaRight));
        std::memcpy(permute.cpu.v[16].data(), fmaAccumulator.data(), sizeof(fmaAccumulator));
        assert(executeStartupInstruction(permute, 0x4e21ce30)); // fmla v16.4s, v17.4s, v1.4s
        assert(std::memcmp(permute.cpu.v[16].data(), fmaSum.data(), sizeof(fmaSum)) == 0);
        const std::array<float, 4> fmsAliased{4, 8, 73, 82}, fmsLeft{3, 0.5, 1, 2};
        const std::array<float, 4> fmsDifference{-8, 4, 0, 0};
        std::memcpy(permute.cpu.v[0].data(), fmsAliased.data(), sizeof(fmsAliased));
        std::memcpy(permute.cpu.v[2].data(), fmsLeft.data(), sizeof(fmsLeft));
        assert(executeStartupInstruction(permute, 0x0ea0cc40)); // fmls v0.2s, v2.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), fmsDifference.data(), sizeof(fmsDifference)) == 0);
        const std::array<double, 2> doubleFmaLeft{1 + 0x1p-52, -2}, doubleFmaRight{1 - 0x1p-52, 3};
        const std::array<double, 2> doubleAddend{-1, 0.5}, doubleFmaSum{-0x1p-104, -5.5};
        std::memcpy(permute.cpu.v[4].data(), doubleFmaLeft.data(), sizeof(doubleFmaLeft));
        std::memcpy(permute.cpu.v[5].data(), doubleFmaRight.data(), sizeof(doubleFmaRight));
        std::memcpy(permute.cpu.v[6].data(), doubleAddend.data(), sizeof(doubleAddend));
        assert(executeStartupInstruction(permute, 0x4e65cc86)); // fmla v6.2d, v4.2d, v5.2d
        assert(std::memcmp(permute.cpu.v[6].data(), doubleFmaSum.data(), sizeof(doubleFmaSum)) == 0);
        assert(!executeStartupInstruction(permute, 0x0e65cc86)); // reserved 1D vector form
        const std::array<float, 4> leftAdd{1, -2, 3, 4}, rightAdd{3, 1, 6, -4}, expectedSum{4, -1, 9, 0};
        std::memcpy(permute.cpu.v[0].data(), leftAdd.data(), sizeof(leftAdd));
        std::memcpy(permute.cpu.v[2].data(), rightAdd.data(), sizeof(rightAdd));
        assert(executeStartupInstruction(permute, 0x4e22d400)); // fadd v0.4s, v0.4s, v2.4s
        assert(std::memcmp(permute.cpu.v[0].data(), expectedSum.data(), sizeof(expectedSum)) == 0);
        const std::array<float, 4> pairFloat{-2, 5.5f, 7, 8}, sumFloat{3.5f};
        std::memcpy(permute.cpu.v[7].data(), pairFloat.data(), sizeof(pairFloat));
        assert(executeStartupInstruction(permute, 0x7e30d8e7)); // faddp s7, v7.2s
        assert(std::memcmp(permute.cpu.v[7].data(), sumFloat.data(), sizeof(sumFloat)) == 0);
        const std::array<double, 2> pairDouble{-5.25, 4}, sumDouble{-1.25};
        std::memcpy(permute.cpu.v[1].data(), pairDouble.data(), sizeof(pairDouble));
        assert(executeStartupInstruction(permute, 0x7e70d822)); // faddp d2, v1.2d
        assert(std::memcmp(permute.cpu.v[2].data(), sumDouble.data(), sizeof(sumDouble)) == 0);
        const std::array<uint32_t, 4> comparison{UINT32_MAX, 0, 0, 0};
        const std::array<uint32_t, 4> against{0x40400000, 0, 0x41100000, 0x7fc00000};
        std::memcpy(permute.cpu.v[1].data(), against.data(), sizeof(against));
        assert(executeStartupInstruction(permute, 0x6ea1e402)); // fcmgt v2.4s, v0.4s, v1.4s
        assert(std::memcmp(permute.cpu.v[2].data(), comparison.data(), sizeof(comparison)) == 0);
        permute.cpu.v[3] = {0, 1, 2, 0, 0, 8, 0, 16, 0, 0, 0, 0, 16, 0, 0, 2};
        const std::array<uint8_t, 16> nonzeroBytes{0, 255, 255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 0, 0, 255};
        assert(executeStartupInstruction(permute, 0x4e238c67)); // cmtst v7.16b, v3.16b, v3.16b
        assert(permute.cpu.v[7] == nonzeroBytes);
        permute.cpu.nzcv = 0xa;
        assert(executeStartupInstruction(permute, 0xd53b4213)); // mrs x19, NZCV
        assert(permute.cpu.x[19] == 0xa0000000 && permute.cpu.nzcv == 0xa);
        permute.cpu.x[4] = 0xffffffff5000000f;
        assert(executeStartupInstruction(permute, 0xd51b4204)); // msr NZCV, x4
        assert(permute.cpu.nzcv == 5);
        const auto stack = permute.cpu.sp;
        assert(executeStartupInstruction(permute, 0xd53b421f)); // mrs xzr, NZCV
        assert(permute.cpu.sp == stack && permute.cpu.nzcv == 5);
        assert(executeStartupInstruction(permute, 0xd51b421f)); // msr NZCV, xzr
        assert(permute.cpu.nzcv == 0);
    }
    {
        constexpr uint64_t boundary = base + pageSize;
        Machine adjacent(Image{base, {{base, std::vector<uint8_t>(pageSize), 6},
                                     {boundary, std::vector<uint8_t>(pageSize), 6}}, {}});
        constexpr uint64_t expected = 0x1122334455667788;
        uint64_t loaded = 0;
        assert(adjacent.store64(boundary - 1, expected));
        assert(adjacent.load64(boundary - 1, loaded) && loaded == expected);
        auto nextPage = std::find_if(adjacent.segments.begin(), adjacent.segments.end(),
                                     [&](const auto& segment) { return segment.address == boundary; });
        assert(nextPage != adjacent.segments.end());
        nextPage->flags = 4;
        assert(!adjacent.store64(boundary - 1, UINT64_MAX));
        assert(adjacent.load64(boundary - 1, loaded) && loaded == expected);
        nextPage->flags = 0;
        assert(!adjacent.load64(boundary - 1, loaded) && loaded == expected);
    }
    {
        const std::array<uint8_t, 8> instructions{4, 3, 2, 1, 8, 7, 6, 5};
        Machine mapped(Image{base, {}, {}});
        Segment view{base, {}, 5, instructions.size()};
        view.imageView = instructions.data();
        mapped.insertSegment(std::move(view));
        assert(mapped.readInstruction(base) == instructions.data());
        assert(mapped.readInstruction(base + 4) == instructions.data() + 4);
        assert(!mapped.readInstruction(base + 1) && !mapped.readInstruction(base + 8));
        mapped.segments.front().flags = 4;
        assert(!mapped.readInstruction(base));
        mapped.segments.clear();
        mapped.insertSegment(Segment{base, {9, 8, 7, 6}, 7});
        const auto* writableCode = mapped.readInstruction(base);
        assert(writableCode && writableCode[0] == 9);
        const std::array<uint8_t, 1> changed{10};
        assert(mapped.write(base, changed));
        assert(mapped.readInstruction(base)[0] == 10);
    }
    ProbeLinux linux(1, 0);
    linux.addDirectory("/test", 0755);
    linux.mount("/system/etc/xtables.lock", {});
    const auto call = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
        std::copy(args.begin(), args.end(), machine.cpu.x.begin());
        machine.cpu.x[8] = number;
        assert(linux.syscall(machine));
        return machine.cpu.x[0];
    };
    {
        constexpr uint64_t cwd = uint32_t(-100);
        constexpr uint64_t path = base + 256, output = base + 512;
        const auto writePath = [&](uint64_t address, const char* name) {
            assert(machine.write(address, {reinterpret_cast<const uint8_t*>(name), std::strlen(name) + 1}));
        };
        const auto expectCwd = [&](std::string_view expected) {
            assert(call(17, {output, expected.size()}) == uint64_t(-34));
            assert(call(17, {output, expected.size() + 1}) == expected.size() + 1);
            const auto* actual = machine.read(output, expected.size() + 1, 4);
            assert(actual && std::memcmp(actual, expected.data(), expected.size()) == 0);
            assert(actual[expected.size()] == 0);
        };
        expectCwd("/");
        writePath(path, "/");
        const auto root = call(56, {cwd, path, 0x4000}); // O_DIRECTORY
        assert(root < 256);
        writePath(path, "/test");
        assert(call(49, {path}) == 0);
        expectCwd("/test");
        writePath(path, "../system/etc/xtables.lock");
        const auto file = call(56, {cwd, path, 0});
        assert(file < 256);
        assert(call(49, {path}) == uint64_t(-20));
        assert(call(50, {file}) == uint64_t(-20));
        writePath(path, "/proc/self/cwd");
        assert(call(78, {cwd, path, output, 64}) == 5);
        const auto* link = machine.read(output, 5, 4);
        assert(link && std::memcmp(link, "/test", 5) == 0);
        assert(call(50, {root}) == 0);
        expectCwd("/");
        assert(call(57, {file}) == 0 && call(57, {root}) == 0);
    }
    {
        constexpr uint64_t cwd = uint32_t(-100), input = base + 32, output = base + 512;
        const auto putPath = [&](const char* path) {
            assert(machine.write(input, {reinterpret_cast<const uint8_t*>(path), std::strlen(path) + 1}));
        };
        linux.alias("/test/statx-link", "/system/etc/xtables.lock");
        putPath("/test/statx-link");
        const auto fd = call(56, {cwd, input, 0});
        assert(fd < 256);
        for (uint64_t flags : {0, 0x100}) {
            assert(call(291, {cwd, input, flags, 0xfff, output}) == 0);
            const auto* bytes = machine.read(output, 256, 4);
            assert(bytes);
            uint32_t mask;
            uint16_t mode;
            std::memcpy(&mask, bytes, sizeof(mask));
            std::memcpy(&mode, bytes + 28, sizeof(mode));
            assert(mask == 0x7ff);
            assert(mode == (flags ? 0120777 : 0100444));
        }
        assert(call(291, {cwd, input, 0x6000, 0xfff, output}) == uint64_t(-22));
        assert(call(291, {cwd, input, 0, 0x80000000, output}) == uint64_t(-22));
        assert(call(291, {fd, 0, 0x1000, 0xfff, output}) == 0);
        uint64_t inode = 0, legacyInode = 0;
        assert(machine.load64(output + 32, inode));
        assert(call(80, {fd, output}) == 0 && machine.load64(output + 8, legacyInode));
        assert(inode == legacyInode);
        assert(call(291, {cwd, input, 0, 0xfff, 0}) == uint64_t(-14));
        putPath("/system/etc/no-such-font");
        assert(call(291, {cwd, input, 0, 0xfff, output}) == uint64_t(-2));
        assert(call(57, {fd}) == 0);
    }
    {
        const std::string_view filename = "/system/etc/xtables.lock";
        assert(machine.write(base, {reinterpret_cast<const uint8_t*>(filename.data()), filename.size() + 1}));
        constexpr uint64_t cwd = uint32_t(-100);
        const auto first = call(56, {cwd, base, 0x80040, 0600}); // O_RDONLY | O_CREAT | O_CLOEXEC
        const auto second = call(56, {cwd, base, 0});
        assert(first < 256 && second < 256);
        assert(call(56, {cwd, base, 1}) == uint64_t(-30));
        const auto duplicate = call(23, {first});
        assert(duplicate < 256);
        assert(call(32, {first, 6}) == 0); // LOCK_EX | LOCK_NB
        assert(call(32, {second, 5}) == uint64_t(-11));
        assert(call(57, {first}) == 0 && call(32, {second, 6}) == uint64_t(-11));
        assert(call(32, {duplicate, 8}) == 0 && call(32, {second, 5}) == 0);
        assert(call(32, {duplicate, 5}) == 0);
        assert(call(32, {second, 6}) == uint64_t(-11));
        assert(call(57, {duplicate}) == 0 && call(32, {second, 6}) == 0);
        assert(call(32, {second, 3}) == uint64_t(-22));
        const auto pathOnly = call(56, {cwd, base, 0x200000});
        assert(pathOnly < 256 && call(32, {pathOnly, 6}) == uint64_t(-9));
        assert(call(57, {second}) == 0 && call(57, {pathOnly}) == 0);
    }
    {
        struct Info {
            std::array<char, 32> name;
            uint32_t hooks, starts[5], policies[5], count, bytes;
        };
        struct Replace {
            std::array<char, 32> name;
            uint32_t hooks, count, bytes, starts[5], policies[5], previousCount;
            uint64_t previous;
        };
        static_assert(sizeof(Info) == 84 && sizeof(Replace) == 96);
        constexpr auto input = base + 256, oldCounters = base + 1792, length = base + 1952;
        constexpr auto address = base + 1984, payload = base + 2024;
        const auto write = [&](uint64_t offset, const auto& data) {
            return machine.write(offset, {reinterpret_cast<const uint8_t*>(&data), sizeof(data)});
        };
        for (uint32_t family : {2, 10}) {
            const bool ipv6 = family == 10;
            const uint32_t level = ipv6 ? 41 : 0;
            const size_t entrySize = ipv6 ? 168 : 112;
            const auto control = call(198, {family, 3, 255}), reader = call(198, {family, 0x802, 0});
            const auto writer = call(198, {family, 0x802, 0});
            assert(control < 256 && reader < 256 && writer < 256);
            if (ipv6) {
                const Info nat{{'n','a','t'}, 0, {}, {}, 0, 0};
                assert(write(input, nat) && write(length, uint32_t(sizeof(Info))));
                assert(call(209, {control, level, 64, input, length}) == uint64_t(-2));
            }
            const Info blank{{'f','i','l','t','e','r'}, 0, {}, {}, 0, 0};
            assert(write(input, blank) && write(length, uint32_t(sizeof(Info))));
            assert(call(209, {control, level, 64, input, length}) == 0);
            Info actual{};
            assert(machine.readInto(input, {reinterpret_cast<uint8_t*>(&actual), sizeof(actual)}));
            assert(actual.hooks == 0x0e && actual.count == 4 && actual.starts[3] == 2 * (entrySize + 40));
            assert(actual.policies[3] == actual.starts[3]);
            assert(write(input + 32, actual.bytes) && write(length, actual.bytes + 40));
            assert(call(209, {control, level, 65, input, length}) == 0);
            std::vector<uint8_t> rules(actual.bytes);
            assert(machine.readInto(input + 40, rules));
            Replace request{actual.name, actual.hooks, actual.count, actual.bytes, {}, {}, actual.count, oldCounters};
            std::ranges::copy(actual.starts, request.starts);
            std::ranges::copy(actual.policies, request.policies);
            assert(write(input, request));
            const uint32_t action = actual.starts[3] + entrySize + 32;
            auto setRules = [&](int32_t verdict) {
                std::memcpy(rules.data() + action, &verdict, sizeof(verdict));
                assert(machine.write(input + sizeof(request), rules));
                return call(208, {control, level, 64, input, sizeof(request) + rules.size()});
            };
            std::array<uint8_t, 28> local{};
            local[0] = family;
            if (!ipv6) local[4] = 127;
            local[ipv6 ? 23 : 7] = 1;
            const uint32_t addressSize = ipv6 ? 28 : 16;
            assert(write(address, local) && call(200, {reader, address, addressSize}) == 0);
            assert(write(length, addressSize) && call(204, {reader, address, length}) == 0);
            assert(write(payload, uint64_t(0x10203040)));
            assert(setRules(-1) == 0);
            const auto send = [&] { return call(206, {writer, payload, 8, 0, address, addressSize}); };
            assert(send() == uint64_t(-1) && call(207, {reader, payload, 8}) == uint64_t(-11));
            assert(setRules(12) == uint64_t(-22)); // Invalid jump leaves the previous policy active.
            assert(send() == uint64_t(-1));
            assert(setRules(-2) == 0);
            uint64_t dropped = 0;
            assert(machine.load64(oldCounters + 2 * 16, dropped) && dropped == 2);
            assert(send() == 8 && call(207, {reader, payload, 8}) == 8);

            // Once the reply is seen, the state match applies in both directions.
            const auto store = [](auto& bytes, size_t offset, const auto& value) {
                std::memcpy(bytes.data() + offset, &value, sizeof(value));
            };
            std::vector<uint8_t> stateRule(entrySize + 80);
            const auto targetOffset = ipv6 ? 140 : 88;
            store(stateRule, targetOffset, uint16_t(entrySize + 40));
            store(stateRule, targetOffset + 2, uint16_t(stateRule.size()));
            store(stateRule, entrySize, uint16_t(40));
            std::memcpy(stateRule.data() + entrySize + 2, "state", 5);
            store(stateRule, entrySize + 32, uint32_t(2)); // ESTABLISHED
            store(stateRule, entrySize + 40, uint16_t(40));
            store(stateRule, entrySize + 72, int32_t(-1)); // DROP
            rules.insert(rules.begin() + actual.starts[3], stateRule.begin(), stateRule.end());
            request.bytes = rules.size();
            ++request.count;
            request.policies[3] += stateRule.size();
            assert(write(input, request) && machine.write(input + sizeof(request), rules));
            assert(call(208, {control, level, 64, input, sizeof(request) + rules.size()}) == 0);
            assert(send() == 8 && call(207, {reader, payload, 8}) == 8);
            assert(send() == 8 && call(207, {reader, payload, 8}) == 8);
            constexpr auto replyAddress = base + 2064;
            assert(write(length, addressSize) && call(204, {writer, replyAddress, length}) == 0);
            assert(call(206, {reader, payload, 8, 0, replyAddress, addressSize}) == uint64_t(-1));
            assert(send() == uint64_t(-1) && call(207, {reader, payload, 8}) == uint64_t(-11));

            rules.erase(rules.begin() + actual.starts[3], rules.begin() + actual.starts[3] + stateRule.size());
            const auto installTarget = [&](unsigned hook, uint16_t protocol, std::string_view name,
                                           std::span<const uint8_t> parameters) {
                const auto targetSize = (32 + parameters.size() + 7) & ~size_t(7);
                std::vector<uint8_t> rule(entrySize + targetSize);
                store(rule, ipv6 ? 128 : 80, protocol);
                if (ipv6) rule[131] = 1;
                store(rule, targetOffset, uint16_t(entrySize));
                store(rule, targetOffset + 2, uint16_t(rule.size()));
                store(rule, entrySize, uint16_t(targetSize));
                std::ranges::copy(name, rule.begin() + entrySize + 2);
                std::ranges::copy(parameters, rule.begin() + entrySize + 32);
                auto next = rules;
                next.insert(next.begin() + actual.starts[hook], rule.begin(), rule.end());
                request.bytes = next.size();
                request.previousCount = request.count;
                for (unsigned i = 1; i <= 3; ++i) {
                    request.starts[i] = actual.starts[i] + (i > hook ? rule.size() : 0);
                    request.policies[i] = actual.policies[i] + (i >= hook ? rule.size() : 0);
                }
                assert(write(input, request) && machine.write(input + sizeof(request), next));
                return call(208, {control, level, 64, input, sizeof(request) + next.size()});
            };
            const auto installReject = [&](unsigned hook, uint16_t protocol, uint32_t reason) {
                return installTarget(hook, protocol, "REJECT",
                    {reinterpret_cast<const uint8_t*>(&reason), sizeof(reason)});
            };
            const auto readError = [&](uint64_t socket) {
                uint32_t error = 0;
                assert(write(length, uint32_t(sizeof(error))));
                assert(call(209, {socket, 1, 4, replyAddress, length}) == 0);
                assert(machine.readInto(replyAddress, {reinterpret_cast<uint8_t*>(&error), sizeof(error)}));
                return error;
            };
            const auto pollSocket = [&](uint64_t socket, uint16_t events) {
                constexpr auto timeout = base + 2112, pollfd = base + 2136;
                assert(write(timeout, std::array<uint64_t, 2>{}));
                assert(write(pollfd, uint64_t(socket) | (uint64_t(events) << 32)));
                assert(call(73, {pollfd, 1, timeout}) == 1);
                uint64_t value = 0;
                assert(machine.load64(pollfd, value));
                return value >> 48;
            };
            assert(call(203, {writer, address, addressSize}) == 0);
            const uint32_t portUnreachable = ipv6 ? 4 : 3, tcpReset = ipv6 ? 6 : 7;
            assert(installReject(1, 17, tcpReset) == uint64_t(-22)); // Reset requires TCP.
            assert(installReject(1, 17, portUnreachable) == 0);
            assert(send() == 8 && pollSocket(writer, 1) == 8 && readError(writer) == 111 && readError(writer) == 0);
            assert(call(207, {reader, payload, 8}) == uint64_t(-11));
            assert(installReject(3, 17, portUnreachable) == 0);
            assert(send() == uint64_t(-1) && pollSocket(writer, 1) == 8 && readError(writer) == 111);
            assert(call(207, {reader, payload, 8}) == uint64_t(-11));

            const auto tcpServer = call(198, {family, 0x801, 0});
            assert(tcpServer < 256 && call(200, {tcpServer, address, addressSize}) == 0);
            assert(call(201, {tcpServer, 2}) == 0);
            for (const unsigned hook : {1U, 3U}) {
                assert(installReject(hook, 6, tcpReset) == 0);
                const auto tcpClient = call(198, {family, 0x801, 0});
                assert(tcpClient < 256 && call(203, {tcpClient, address, addressSize}) == uint64_t(-115));
                assert(pollSocket(tcpClient, 4) == 0x1c && readError(tcpClient) == 111 && readError(tcpClient) == 0);
                assert(call(202, {tcpServer, 0, 0}) == uint64_t(-11));
                assert(call(57, {tcpClient}) == 0);
            }
            assert(call(57, {tcpServer}) == 0);

            const auto log = call(198, {16, 0x802, 12});
            assert(log < 256);
            constexpr auto nlAddress = base + 2200, config = base + 2260, packet = base + 2516;
            const std::array<uint32_t, 3> kernel{16, 0, 0};
            assert(write(nlAddress, kernel));
            assert(call(200, {log, nlAddress, sizeof(kernel)}) == 0);
            const auto requestLog = [&](uint64_t fd, uint16_t type, std::span<const uint8_t> value, int status) {
                std::vector<uint8_t> message(24 + value.size());
                store(message, 0, uint32_t(message.size()));
                store(message, 4, uint16_t(0x401)); // NFULNL_MSG_CONFIG
                store(message, 6, uint16_t(5)); // REQUEST | ACK
                store(message, 8, uint32_t(42));
                message[19] = 3;
                store(message, 20, uint16_t(4 + value.size()));
                store(message, 22, type);
                std::ranges::copy(value, message.begin() + 24);
                assert(machine.write(config, message));
                assert(call(206, {fd, config, message.size(), 0, nlAddress, sizeof(kernel)}) == message.size());
                assert(pollSocket(fd, 1) == 1);
                assert(call(207, {fd, packet, 192}) >= 36);
                uint64_t reply = 0;
                assert(machine.load64(packet + 4, reply) && reply == (uint64_t(42) << 32 | 2)); // NLMSG_ERROR
                assert(machine.load64(packet + 16, reply) && int32_t(reply) == status);
            };
            const std::array<uint8_t, 1> bindLog{1}, unbindLog{2};
            requestLog(log, 1, bindLog, 0);
            requestLog(log, 1, bindLog, -16);
            const auto other = call(198, {16, 0x802, 12});
            assert(other < 256);
            requestLog(other, 1, bindLog, -1);
            requestLog(other, 1, unbindLog, -1);
            assert(call(57, {other}) == 0);
            requestLog(log, 2, std::array<uint8_t, 6>{0, 0, 0, 80, 2, 0}, 0);

            std::array<uint8_t, 80> parameters{};
            const auto wireSize = uint32_t(ipv6 ? 48 : 28);
            store(parameters, 0, wireSize + 2);
            store(parameters, 4, uint16_t(3));
            store(parameters, 8, uint16_t(1)); // Explicit payload copy limit.
            std::memcpy(parameters.data() + 12, "loop", 4);
            assert(installTarget(3, 17, "NFLOG", parameters) == 0);
            assert(write(payload, uint64_t(0x12345678)));
            assert(send() == 8 && call(207, {reader, payload, 8}) == 8);
            assert(pollSocket(log, 1) == 1);
            const auto logBytes = call(207, {log, packet, 192});
            assert(logBytes <= 192 && logBytes > wireSize);
            std::vector<uint8_t> record(logBytes);
            assert(machine.readInto(packet, record));
            assert(record[4] == 0 && record[5] == 4 && record[16] == family && record[19] == 3);
            bool sawHeader = false, sawPayload = false, sawPrefix = false;
            for (size_t pos = 20; pos + 4 <= record.size();) {
                const uint16_t len = uint16_t(record[pos]) | uint16_t(record[pos + 1]) << 8;
                const uint16_t kind = uint16_t(record[pos + 2]) | uint16_t(record[pos + 3]) << 8;
                assert(len >= 4 && len <= record.size() - pos);
                if (kind == 1) sawHeader = len == 8 && record[pos + 6] == 3; // NF_INET_LOCAL_OUT
                if (kind == 9) sawPayload = len == wireSize + 6 && record[pos + 4 + wireSize] == 0x78;
                if (kind == 10) sawPrefix = len == 9 && std::memcmp(record.data() + pos + 4, "loop", 5) == 0;
                pos += (len + 3) & ~size_t(3);
            }
            assert(sawHeader && sawPayload && sawPrefix);
            requestLog(log, 1, unbindLog, 0);
            assert(send() == 8 && call(207, {log, packet, 192}) == uint64_t(-11));
            assert(call(57, {log}) == 0);
            assert(call(57, {reader}) == 0 && call(57, {writer}) == 0 && call(57, {control}) == 0);
        }
    }
    {
        Machine data(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        for (const bool ipv6 : {false, true}) {
            GuestFirewall firewall;
            const auto ipSize = ipv6 ? 40U : 20U;
            const auto header = ipv6 ? 168U : 112U;
            const auto level = ipv6 ? 41U : 0U;
            const auto offsets = ipv6 ? 140U : 88U;
            const auto protocol = ipv6 ? 128U : 80U;
            constexpr auto address = base + 64, length = base + 3800, counters = base + 3840;
            const auto store = [](auto& buffer, size_t at, const auto& value) {
                std::memcpy(buffer.data() + at, &value, sizeof(value));
            };
            std::array<uint8_t, 84> info{};
            std::memcpy(info.data(), "mangle", 6);
            assert(data.write(address, info) && data.store64(length, info.size()));
            data.cpu.x = {0, level, 64, address, length};
            assert(firewall.get(data, ipv6) == 0 && data.readInto(address, info));
            std::array<uint32_t, 13> metadata{};
            std::memcpy(metadata.data(), info.data() + 32, sizeof(metadata));
            const auto start = metadata[4];
            const auto size = metadata[12];
            assert(data.store64(address + 32, size) && data.store64(length, size + 40));
            data.cpu.x[2] = 65;
            assert(firewall.get(data, ipv6) == 0);
            std::vector<uint8_t> old(size);
            assert(data.readInto(address + 40, old));
            const auto initial = old;
            std::vector<uint8_t> rule(header + 48 + 40);
            store(rule, protocol, uint16_t(6));
            if (ipv6) rule[131] = 1; // IP6T_F_PROTO
            store(rule, offsets, uint16_t(header + 48));
            store(rule, offsets + 2, uint16_t(rule.size()));
            store(rule, header, uint16_t(48));
            std::memcpy(rule.data() + header + 2, "tcp", 3);
            store(rule, header + 34, uint16_t(65535));
            store(rule, header + 36, uint16_t(443));
            store(rule, header + 38, uint16_t(443));
            rule[header + 41] = rule[header + 42] = 2; // Test only SYN.
            store(rule, header + 48, uint16_t(40));
            std::memcpy(rule.data() + header + 50, "TCPMSS", 6);
            store(rule, header + 80, uint16_t(420));
            old.insert(old.begin() + start, rule.begin(), rule.end());
            std::array<uint8_t, 96> update{};
            std::memcpy(update.data(), "mangle", 6);
            store(update, 32, metadata[0]);
            store(update, 36, metadata[11] + 1);
            store(update, 40, uint32_t(old.size()));
            for (unsigned hook = 0; hook < 5; ++hook) {
                const auto entry = metadata[1 + hook], policy = metadata[6 + hook];
                store(update, 44 + hook * 4, uint32_t(entry + (entry > start ? rule.size() : 0)));
                store(update, 64 + hook * 4, uint32_t(policy + (policy >= start ? rule.size() : 0)));
            }
            store(update, 84, metadata[11]);
            store(update, 88, counters);
            assert(data.write(address, update) && data.write(address + update.size(), old));
            data.cpu.x = {0, level, 64, address, update.size() + old.size()};
            assert(firewall.set(data, ipv6) == 0);
            auto packet = std::vector<uint8_t>(ipSize + 20);
            packet[0] = ipv6 ? 0x60 : 0x45;
            packet[ipv6 ? 6 : 9] = 6;
            packet[ipSize + 2] = 1;
            packet[ipSize + 3] = 187;
            packet[ipSize + 12] = 0x50;
            packet[ipSize + 13] = 2;
            auto differentPort = packet;
            differentPort[ipSize + 3] = 188;
            assert(firewall.allows(differentPort, GuestFirewall::output, 65536, {}) && differentPort.size() == packet.size());
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {}) && packet.size() == ipSize + 24);
            assert(packet[ipSize + 12] == 0x60 && packet[ipSize + 20] == 2 && packet[ipSize + 21] == 4);
            assert(packet[ipSize + 22] == 1 && packet[ipSize + 23] == 164);
            const auto checkSum = [](std::span<const uint8_t> bytes, uint32_t sum = 0) {
                for (size_t i = 0; i < bytes.size(); i += 2)
                    sum += (uint32_t(bytes[i]) << 8) | bytes[i + 1];
                while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
                return sum;
            };
            if (!ipv6) assert(checkSum(std::span(packet).first(ipSize)) == 0xffff);
            const auto addresses = std::span(packet).subspan(ipv6 ? 8 : 12, ipv6 ? 32 : 8);
            assert(checkSum(std::span(packet).subspan(ipSize), checkSum(addresses, 30)) == 0xffff);
            packet[ipSize + 23] = 64;
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {}));
            assert(packet[ipSize + 23] == 64); // A smaller existing MSS is preserved.

            const auto markRule = [&](uint32_t expected, uint8_t mode) {
                std::vector<uint8_t> ruleBytes(header + 48 + 48);
                store(ruleBytes, offsets, uint16_t(header + 48));
                store(ruleBytes, offsets + 2, uint16_t(ruleBytes.size()));
                store(ruleBytes, header, uint16_t(48));
                std::memcpy(ruleBytes.data() + header + 2, "connmark", 8);
                ruleBytes[header + 31] = 1;
                store(ruleBytes, header + 32, expected);
                store(ruleBytes, header + 36, uint32_t(0xff));
                const auto target = header + 48;
                store(ruleBytes, target, uint16_t(48));
                std::memcpy(ruleBytes.data() + target + 2, "CONNMARK", 8);
                ruleBytes[target + 31] = mode == 1 ? 1 : 2;
                store(ruleBytes, target + 36, uint32_t(0xff));
                store(ruleBytes, target + 40, uint32_t(mode == 1 ? 0xff : 0xff00));
                if (mode == 1) {
                    ruleBytes[target + 44] = 1; // Save the packet's low byte to the connection.
                } else {
                    ruleBytes[target + 45] = 8; // Restore the connection mark eight bits to the left.
                    ruleBytes[target + 46] = 2;
                }
                return ruleBytes;
            };
            const auto save = markRule(0, 1), restore = markRule(0x34, 2);
            old = initial;
            old.insert(old.begin() + start, save.begin(), save.end());
            old.insert(old.begin() + start + save.size(), restore.begin(), restore.end());
            store(update, 36, metadata[11] + 2);
            store(update, 40, uint32_t(old.size()));
            const auto markBytes = save.size() + restore.size();
            for (unsigned hook = 0; hook < 5; ++hook) {
                const auto entry = metadata[1 + hook], policy = metadata[6 + hook];
                store(update, 44 + hook * 4, uint32_t(entry + (entry > start ? markBytes : 0)));
                store(update, 64 + hook * 4, uint32_t(policy + (policy >= start ? markBytes : 0)));
            }
            store(update, 84, metadata[11] + 1);
            assert(data.write(address, update) && data.write(address + update.size(), old));
            data.cpu.x = {0, level, 64, address, update.size() + old.size()};
            assert(firewall.set(data, ipv6) == 0 && firewall.tracksConnections());
            uint32_t packetMark = 0x1234, connectionMark = 0;
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {&packetMark, &connectionMark}));
            assert(packetMark == 0x3434 && connectionMark == 0x34);
            packetMark = 0;
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {&packetMark, &connectionMark}));
            assert(packetMark == 0x3400 && connectionMark == 0x34);

            std::array<uint8_t, 30> version{};
            std::memcpy(version.data(), "connmark", 8);
            assert(data.write(address, version) && data.store64(length, version.size()));
            data.cpu.x = {0, level, ipv6 ? 68U : 66U, address, length};
            assert(firewall.get(data, ipv6) == uint64_t(-93));
            version.back() = 1;
            assert(data.write(address, version) && firewall.get(data, ipv6) == 0);

            const auto testMarkVersion = [&](uint32_t option, const char* name, uint8_t revision) {
                std::array<uint8_t, 30> request{};
                std::memcpy(request.data(), name, std::strlen(name));
                data.cpu.x[2] = option;
                assert(data.write(address, request) && firewall.get(data, ipv6) == uint64_t(-93));
                request.back() = revision;
                assert(data.write(address, request) && firewall.get(data, ipv6) == 0);
            };
            testMarkVersion(ipv6 ? 69 : 67, "MARK", 2);
            testMarkVersion(ipv6 ? 68 : 66, "mark", 1);

            version.fill(0);
            std::memcpy(version.data(), "u32", 3);
            assert(data.write(address, version) && firewall.get(data, ipv6) == 0);

            // Match both the TCP header's dynamic length and two independent payload words.
            constexpr auto matchSize = 32 + 1984;
            std::vector<uint8_t> payloadRule(header + matchSize + 40);
            store(payloadRule, offsets, uint16_t(header + matchSize));
            store(payloadRule, offsets + 2, uint16_t(payloadRule.size()));
            store(payloadRule, header, uint16_t(matchSize));
            std::memcpy(payloadRule.data() + header + 2, "u32", 3);
            const auto body = header + 32;
            for (unsigned test = 0; test < 2; ++test) {
                const auto expression = body + test * 180;
                store(payloadRule, expression, uint32_t(ipSize + 12));
                store(payloadRule, expression + 8, uint32_t(26));
                payloadRule[expression + 12] = 2; // >>
                store(payloadRule, expression + 16, uint32_t(0x3c));
                payloadRule[expression + 20] = 0; // &
                store(payloadRule, expression + 24, uint32_t(ipSize + test * 4));
                payloadRule[expression + 28] = 3; // @
                const uint32_t word = test == 0 ? 0x16030000 : 0x12345678;
                store(payloadRule, expression + 88, word);
                store(payloadRule, expression + 92, word + (test == 0 ? 0xffff : 0));
                payloadRule[expression + 176] = 4;
                payloadRule[expression + 177] = 1;
            }
            payloadRule[body + 1980] = 2;
            store(payloadRule, header + matchSize, uint16_t(40));
            store(payloadRule, header + matchSize + 32, int32_t(-1));
            store(update, 36, metadata[11] + 1);
            store(update, 40, uint32_t(initial.size() + payloadRule.size()));
            for (unsigned hook = 0; hook < 5; ++hook) {
                const auto entry = metadata[1 + hook], policy = metadata[6 + hook];
                store(update, 44 + hook * 4, uint32_t(entry + (entry > start ? payloadRule.size() : 0)));
                store(update, 64 + hook * 4, uint32_t(policy + (policy >= start ? payloadRule.size() : 0)));
            }
            const auto installPayloadRule = [&](uint32_t previousCount) {
                store(update, 84, previousCount);
                old = initial;
                old.insert(old.begin() + start, payloadRule.begin(), payloadRule.end());
                assert(data.write(address, update) && data.write(address + update.size(), old));
                data.cpu.x = {0, level, 64, address, update.size() + old.size()};
                return firewall.set(data, ipv6);
            };
            assert(installPayloadRule(metadata[11] + 2) == 0);
            packet.resize(ipSize + 32);
            constexpr std::array<uint8_t, 8> payload{0x16, 3, 3, 1, 0x12, 0x34, 0x56, 0x78};
            std::ranges::copy(payload, packet.begin() + ipSize + 24);
            assert(!firewall.allows(packet, GuestFirewall::output, 65536, {}));
            auto truncatedPacket = packet;
            truncatedPacket.pop_back();
            assert(firewall.allows(truncatedPacket, GuestFirewall::output, 65536, {}));
            packet.back() = 0;
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {}));
            payloadRule[body + 1981] = 1;
            assert(installPayloadRule(metadata[11] + 1) == 0);
            assert(!firewall.allows(truncatedPacket, GuestFirewall::output, 65536, {}));
            store(payloadRule, body + 8, uint32_t(32));
            assert(installPayloadRule(metadata[11] + 1) == uint64_t(-22));
            assert(!firewall.allows(packet, GuestFirewall::output, 65536, {}));

            // Only the matching packet's selected mark bits change.
            payloadRule.assign(header + 48 + 40, 0);
            store(payloadRule, offsets, uint16_t(header + 48));
            store(payloadRule, offsets + 2, uint16_t(payloadRule.size()));
            store(payloadRule, header, uint16_t(48));
            std::memcpy(payloadRule.data() + header + 2, "mark", 4);
            payloadRule[header + 31] = 1;
            store(payloadRule, header + 32, uint32_t(0x34));
            store(payloadRule, header + 36, uint32_t(0xff));
            store(payloadRule, header + 48, uint16_t(40));
            std::memcpy(payloadRule.data() + header + 50, "MARK", 4);
            payloadRule[header + 79] = 2;
            store(payloadRule, header + 80, uint32_t(0x11));
            store(payloadRule, header + 84, uint32_t(0xff));
            store(update, 40, uint32_t(initial.size() + payloadRule.size()));
            for (unsigned hook = 0; hook < 5; ++hook) {
                const auto entry = metadata[1 + hook], policy = metadata[6 + hook];
                store(update, 44 + hook * 4, uint32_t(entry + (entry > start ? payloadRule.size() : 0)));
                store(update, 64 + hook * 4, uint32_t(policy + (policy >= start ? payloadRule.size() : 0)));
            }
            assert(installPayloadRule(metadata[11] + 1) == 0 && !firewall.tracksConnections());
            packetMark = 0x6f34;
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {&packetMark}));
            assert(packetMark == 0x6f11);
            packetMark = 0x6f35;
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {&packetMark}));
            assert(packetMark == 0x6f35);

            constexpr uint16_t policySize = 344, policyInfo = 32 + 304;
            payloadRule.assign(header + policySize + 40, 0);
            store(payloadRule, offsets, uint16_t(header + policySize));
            store(payloadRule, offsets + 2, uint16_t(payloadRule.size()));
            store(payloadRule, header, policySize);
            std::memcpy(payloadRule.data() + header + 2, "policy", 6);
            store(payloadRule, header + policyInfo, uint16_t(2)); // --dir out --pol ipsec
            store(payloadRule, header + policyInfo + 2, uint16_t(1));
            store(payloadRule, header + policySize, uint16_t(40));
            store(payloadRule, header + policySize + 32, int32_t(-1));
            store(update, 40, uint32_t(initial.size() + payloadRule.size()));
            for (unsigned hook = 0; hook < 5; ++hook) {
                const auto entry = metadata[1 + hook], policy = metadata[6 + hook];
                store(update, 44 + hook * 4, uint32_t(entry + (entry > start ? payloadRule.size() : 0)));
                store(update, 64 + hook * 4, uint32_t(policy + (policy >= start ? payloadRule.size() : 0)));
            }
            assert(installPayloadRule(metadata[11] + 1) == 0);
            assert(firewall.allows(packet, GuestFirewall::output, 65536, {}));
            store(payloadRule, header + policyInfo, uint16_t(2 | 4)); // --dir out --pol none
            store(payloadRule, header + policyInfo + 2, uint16_t(0));
            assert(installPayloadRule(metadata[11] + 1) == 0);
            assert(!firewall.allows(packet, GuestFirewall::output, 65536, {}));
            store(payloadRule, header + policyInfo, uint16_t(4)); // Missing direction.
            assert(installPayloadRule(metadata[11] + 1) == uint64_t(-22));
            assert(!firewall.allows(packet, GuestFirewall::output, 65536, {}));
        }
    }
    const auto put = [&](uint64_t address, const char* data) {
        assert(machine.write(address, {reinterpret_cast<const uint8_t*>(data), std::strlen(data) + 1}));
    };
    const auto load = [&](uint64_t address) {
        uint64_t value;
        assert(machine.load64(address, value));
        return value;
    };
    {
        constexpr uint64_t committedLength = 1024 * 1024;
        const auto reservation = call(222, {0, committedLength + 2 * pageSize, 0, 0x4022, UINT32_MAX, 0});
        const auto committed = reservation + pageSize;
        const auto dirty = linux.dirtyAnonymousBytes();
        assert(!machine.read(committed, 1, 4));
        assert(call(226, {committed, committedLength, 3}) == 0);
        assert(!machine.read(reservation, 1, 4) && !machine.read(committed + committedLength, 1, 4));
        assert(load(committed) == 0 && linux.dirtyAnonymousBytes() == dirty);
        assert(machine.store64(committed, 42) && load(committed) == 42);
        assert(linux.dirtyAnonymousBytes() > dirty && linux.dirtyAnonymousBytes() < dirty + committedLength);
        assert(call(215, {reservation, committedLength + 2 * pageSize}) == 0);
        assert(linux.dirtyAnonymousBytes() == dirty);
    }
    {
        constexpr uint64_t physicalBudget = 20 * 1024 * 1024;
        constexpr uint64_t sparseRange = 96 * 1024 * 1024;
        ProbeLinux forkGuest(1, 0, physicalBudget);
        auto parent = forkGuest.createMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::initializer_list<uint64_t> args) {
            parent.cpu.x.fill(0);
            std::ranges::copy(args, parent.cpu.x.begin());
            parent.cpu.x[8] = number;
            assert(forkGuest.syscall(parent));
            return parent.cpu.x[0];
        };
        const auto mapping = invoke(222, {0, sparseRange, 3, 0x22, UINT64_MAX, 0});
        assert(mapping < uint64_t(-4095));
        // Forking both ranges exceeds the old logical limit. The second stays untouched.
        const auto untouched = invoke(222, {0, sparseRange, 3, 0x22, UINT32_MAX, 0});
        assert(untouched < uint64_t(-4095));
        const uint64_t boundary = mapping + 64 * 1024;
        const std::array<uint8_t, 8> across{11, 18, 29, 47, 76, 123, 199, 1};
        std::array<uint8_t, 8> restored{};
        assert(parent.readInto(boundary - 4, restored) && restored == (std::array<uint8_t, 8>{}));
        assert(parent.write(boundary - 4, across));
        std::vector<uint8_t> scratch;
        const auto* bytes = parent.read(boundary - 4, across.size(), 4, scratch);
        assert(bytes && std::ranges::equal(std::span(bytes, across.size()), across));
        const auto dirty = forkGuest.dirtyAnonymousBytes();
        const auto hostPage = uint64_t(getpagesize());
        assert(invoke(233, {boundary - hostPage, 2 * hostPage, 4}) == 0); // MADV_DONTNEED
        assert(parent.readInto(boundary - 4, restored) && restored == (std::array<uint8_t, 8>{}));
        assert(forkGuest.dirtyAnonymousBytes() < dirty);
        assert(parent.write(boundary - 4, across));
        assert(parent.store64(mapping + sparseRange - pageSize, 42));
        const auto child = invoke(220, {0x1200011, 0, 0, 0, base + 8});
        assert(child == 2 && forkGuest.dirtyAnonymousBytes() < physicalBudget);
        assert(forkGuest.schedule(parent) && forkGuest.processId() == child);
        uint64_t value = 0;
        assert(parent.load64(mapping + sparseRange - pageSize, value) && value == 42);
        assert(parent.readInto(boundary - 4, restored) && restored == across);
        assert(parent.write(boundary - 4, {}));
        assert(parent.write(boundary - 4, std::array<uint8_t, 8>{}));
        assert(parent.store64(mapping + sparseRange - pageSize, 99));
        parent.cpu.retired += 4096;
        assert(forkGuest.schedule(parent) && forkGuest.processId() == 1);
        assert(parent.load64(mapping + sparseRange - pageSize, value) && value == 42);
        assert(parent.readInto(boundary - 4, restored) && restored == across);
    }
    {
        ProbeLinux apkGuest(1, 0);
        apkGuest.addBootTmpfs("/data", 0771, 1000, 6);
        apkGuest.addDirectory("/data/app", 0755);
        apkGuest.addDirectory("/data/private", 0700);
        auto task = apkGuest.createMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::initializer_list<uint64_t> args) {
            task.cpu.x.fill(0);
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(apkGuest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr char apk[] = "/data/app/base.apk";
        constexpr char hidden[] = "/data/private/base.apk";
        constexpr auto apkPath = base + 64, hiddenPath = base + 128;
        assert(task.write(apkPath, {reinterpret_cast<const uint8_t*>(apk), sizeof apk}));
        assert(task.write(hiddenPath, {reinterpret_cast<const uint8_t*>(hidden), sizeof hidden}));
        constexpr auto atCwd = uint32_t(-100);
        assert(invoke(56, {atCwd, apkPath, 0x42, 0644}) < 256);
        assert(invoke(56, {atCwd, hiddenPath, 0x42, 0644}) < 256);
        assert(invoke(54, {atCwd, apkPath, 1000, 1000, 0}) == 0);
        assert(invoke(147, {10032, 1000, 1000}) == 0);
        assert(invoke(48, {atCwd, apkPath, 4}) == 0);
        assert(invoke(48, {atCwd, apkPath, 2}) == uint64_t(-13));
        assert(invoke(48, {atCwd, hiddenPath, 0}) == uint64_t(-13));
        assert(invoke(56, {atCwd, apkPath, 2}) < 256);
    }
    {
        ProbeLinux guest(1, 0);
        guest.addBootTmpfs("/data", 0771, 1000, 6);
        guest.addDirectory("/data/dalvik-cache", 0771);
        guest.addDirectory("/data/dalvik-cache/arm64", 0711);
        auto task = guest.createMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr auto atCwd = uint32_t(-100);
        constexpr auto header = base + 64, caps = base + 80, path = base + 128;
        constexpr char artifact[] = "/data/dalvik-cache/arm64/artd-test.tmp";
        constexpr uint32_t create = 0x42, dacOverride = 1U << 1, dacReadSearch = 1U << 2;
        assert(task.write(path, {reinterpret_cast<const uint8_t*>(artifact), sizeof artifact}));
        assert(task.store64(header, 0x20080522));
        assert(invoke(167, {8, 1}) == 0); // PR_SET_KEEPCAPS
        assert(invoke(144, {1082, 1082, 1082}) == 0);
        assert(invoke(147, {1082, 1082, 1082}) == 0);
        const auto setCapabilities = [&](uint32_t effective, uint32_t permitted) {
            const std::array<uint32_t, 6> values{effective, permitted};
            assert(task.write(caps, {reinterpret_cast<const uint8_t*>(values.data()), sizeof values}));
            assert(invoke(91, {header, caps}) == 0);
        };
        assert(invoke(56, {atCwd, path, create, 0400}) == uint64_t(-13));
        setCapabilities(dacReadSearch, dacOverride | dacReadSearch);
        assert(invoke(56, {atCwd, path, create, 0400}) == uint64_t(-13));
        setCapabilities(dacOverride, dacOverride | dacReadSearch);
        const auto fd = invoke(56, {atCwd, path, create, 0400});
        assert(fd < 256 && invoke(57, {fd}) == 0);
        assert(invoke(48, {atCwd, path, 2}) == uint64_t(-13));
        assert(invoke(56, {atCwd, path, 2}) < 256);
    }
    {
        static std::array<uint8_t, pageSize * 3 + 32> contents{};
        contents[pageSize] = 0x37;
        contents[2 * pageSize] = 0x72;
        contents[3 * pageSize] = 0x2a;
        linux.mount("/test/resources.apk", contents);
        put(base + 512, "/test/resources.apk");
        const auto file = call(56, {uint32_t(-100), base + 512, 0});
        assert(file < 256);
        const auto view = call(222, {0, pageSize * 2, 1, 1, file, pageSize});
        assert(machine.read(view, 1, 4) == contents.data() + pageSize);
        assert(call(226, {view + pageSize, pageSize, 0}) == 0);
        assert(!machine.read(view + pageSize, 1, 4));
        assert(call(226, {view + pageSize, pageSize, 1}) == 0);
        assert(machine.read(view + pageSize, 1, 4) == contents.data() + pageSize * 2);
        assert(call(226, {view, pageSize, 3}) == uint64_t(-13));
        assert(!machine.store64(view, 0));
        const auto privateView = call(222, {0, pageSize * 2, 1, 2, file, pageSize});
        assert(machine.read(privateView, 1, 4) == contents.data() + pageSize);
        assert(call(226, {privateView, pageSize, 3}) == 0);
        assert(*machine.read(privateView, 1, 4) == 0x37 && machine.store64(privateView, 0x99));
        assert(contents[pageSize] == 0x37 && *machine.read(view, 1, 4) == 0x37);
        assert(machine.read(privateView + pageSize, 1, 4) == contents.data() + pageSize * 2);
        assert(call(215, {privateView, pageSize * 2}) == 0);
        const auto tail = call(222, {0, pageSize * 3, 1, 1, file, pageSize * 2});
        assert(machine.read(tail, 1, 4) == contents.data() + pageSize * 2);
        assert(*machine.read(tail + pageSize, 1, 4) == 0x2a && *machine.read(tail + pageSize + 32, 1, 4) == 0);
        assert(!machine.read(tail + pageSize * 2, 1, 4));
        assert(call(57, {file}) == 0 && machine.read(view, 1, 4) == contents.data() + pageSize);
        assert(call(215, {view, pageSize * 2}) == 0 && call(215, {tail, pageSize * 3}) == 0);
    }
    {
        ProbeLinux routing(1, 0);
        auto guest = routing.createMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto syscall = [&](uint64_t number, std::initializer_list<uint64_t> args) {
            guest.cpu.x.fill(0);
            std::ranges::copy(args, guest.cpu.x.begin());
            guest.cpu.x[8] = number;
            assert(routing.syscall(guest));
            return guest.cpu.x[0];
        };
        constexpr auto kernel = base + 64, input = base + 128, vector = base + 512, output = base + 1024;
        constexpr auto address = base + 1536;
        const auto write = [&](uint64_t where, const auto& data) {
            return guest.write(where, {reinterpret_cast<const uint8_t*>(&data), sizeof(data)});
        };
        auto route = syscall(198, {16, 0x802, 0});
        assert(route < 256 && write(kernel, std::array<uint32_t, 3>{16, 0, 0}));
        assert(syscall(203, {route, kernel, 12}) == 0);
        assert(syscall(207, {route, output, 256}) == uint64_t(-11));
        constexpr auto timeout = base + 1792, pollfd = base + 1816;
        assert(write(timeout, std::array<uint64_t, 2>{}));
        assert(write(pollfd, uint64_t(route) | (1ULL << 32)));
        assert(syscall(73, {pollfd, 1, timeout}) == 0);
        uint32_t sequence = 0;
        const auto request = [&](uint16_t type, uint16_t flags, std::span<const uint8_t> payload) {
            const NetlinkHeader header{uint32_t(sizeof(NetlinkHeader) + payload.size()), type, flags,
                                       ++sequence, 0};
            assert(write(input, header) && guest.write(input + 64, payload));
            const std::array<uint64_t, 4> parts{input, sizeof(header), input + 64, payload.size()};
            assert(write(vector, parts));
            assert(syscall(66, {route, vector, 2}) == header.length);
        };
        const auto receive = [&](uint16_t expected, uint16_t flags) {
            const auto count = syscall(207, {route, output, 256});
            assert(count >= sizeof(NetlinkHeader) && count <= 256);
            std::vector<uint8_t> reply(count);
            assert(guest.readInto(output, reply));
            const auto header = readNetlink<NetlinkHeader>(reply, 0);
            assert(header.length == count && header.type == expected && header.flags == flags &&
                   header.sequence == sequence && header.port == 1);
            return reply;
        };
        const auto rule = [](uint8_t family, uint8_t action, uint32_t priority, uint32_t table) {
            std::vector<uint8_t> payload(12);
            payload[0] = family;
            payload[7] = action;
            appendNetlinkValue(payload, 6, priority); // FRA_PRIORITY
            if (table) appendNetlinkValue(payload, 15, table); // FRA_TABLE
            return payload;
        };
        const auto acknowledge = [&](uint16_t type, uint16_t flags, std::span<const uint8_t> payload, int status) {
            request(type, netlinkRequest | netlinkAck | flags, payload);
            const auto reply = receive(netlinkError, 0);
            assert(readNetlink<int32_t>(reply, 16) == status);
            assert(readNetlink<NetlinkHeader>(reply, 20).type == type);
        };
        const auto dumpRules = [&](uint8_t family, std::span<const uint32_t> priorities) {
            request(GuestRoute::getRule, netlinkRequest | netlinkDump, std::array<uint8_t, 12>{family});
            for (const auto priority : priorities) {
                const auto message = receive(GuestRoute::newRule, netlinkMulti);
                assert(message.size() >= 36 && message[16] == family);
                assert(readNetlink<uint16_t>(message, 30) == 6 &&
                       readNetlink<uint32_t>(message, 32) == priority);
            }
            assert(readNetlink<int32_t>(receive(netlinkDone, netlinkMulti), 16) == 0);
        };
        dumpRules(2, std::array<uint32_t, 3>{0, 32766, 32767});
        dumpRules(10, std::array<uint32_t, 2>{0, 32766});
        request(GuestRoute::getLink, netlinkRequest | netlinkDump, std::array<uint8_t, 16>{});
        constexpr auto source = base + 1840, sourceLength = base + 1856;
        assert(write(sourceLength, uint32_t(8)) && write(source, std::array<uint32_t, 3>{99, 99, 99}));
        const auto peeked = syscall(207, {route, output, 16, 0x22, source, sourceLength});
        assert(peeked > 16 && peeked <= 256);
        const auto read32 = [&](uint64_t where) {
            uint32_t value = 0;
            assert(guest.readInto(where, {reinterpret_cast<uint8_t*>(&value), sizeof(value)}));
            return value;
        };
        assert(read32(sourceLength) == 12 && read32(source) == 16 &&
               read32(source + 4) == 0 && read32(source + 8) == 99);
        const auto link = receive(GuestRoute::newLink, netlinkMulti);
        assert(peeked == link.size());
        assert(readNetlink<uint32_t>(link, 20) == 1 && readNetlink<uint32_t>(link, 24) & 9);
        assert(std::memcmp(link.data() + 36, "lo", 3) == 0);
        receive(netlinkDone, netlinkMulti);
        request(GuestRoute::getAddress, netlinkRequest | netlinkDump, std::array<uint8_t, 8>{10});
        const auto ip6 = receive(GuestRoute::newAddress, netlinkMulti);
        assert(ip6[16] == 10 && ip6[17] == 128 && ip6[43] == 1);
        receive(netlinkDone, netlinkMulti);

        const auto denial = rule(2, 8, 100, 0); // FR_ACT_PROHIBIT
        const auto local = rule(2, 1, 0, 255);
        acknowledge(GuestRoute::newRule, 0x600, denial, 0);
        acknowledge(GuestRoute::newRule, 0x600, denial, -17);
        dumpRules(2, std::array<uint32_t, 4>{0, 100, 32766, 32767});
        acknowledge(GuestRoute::deleteRule, 0, local, 0);
        const auto client = syscall(198, {2, 0x802, 0});
        assert(client < 256 && write(address, std::array<uint8_t, 16>{2, 0, 0x42, 0, 127, 0, 0, 1}));
        assert(syscall(203, {client, address, 16}) == uint64_t(-13));
        acknowledge(GuestRoute::deleteRule, 0, denial, 0);
        acknowledge(GuestRoute::deleteRule, 0, denial, -2);
        assert(syscall(203, {client, address, 16}) == uint64_t(-101));
        acknowledge(GuestRoute::newRule, 0x600, local, 0);
        assert(syscall(203, {client, address, 16}) == 0);
        assert(syscall(57, {client}) == 0 && syscall(57, {route}) == 0);

        route = syscall(198, {16, 0x802, 6}); // NETLINK_XFRM
        assert(route < 256 && syscall(203, {route, kernel, 12}) == 0);
        acknowledge(0x1c, 0, std::array<uint8_t, 4>{255}, 0); // Flush all SAs.
        acknowledge(0x1d, 0, {}, 0); // Flush policies.
        acknowledge(0x1c, 0, {}, -22);
        acknowledge(0x10, 0, {}, -95); // Installing a transform requires the data plane.
        for (const uint16_t type : {0x12, 0x15}) {
            request(type, netlinkRequest | netlinkDump, {});
            assert(readNetlink<int32_t>(receive(netlinkDone, netlinkMulti), 16) == 0);
        }
        constexpr uint64_t keyFamily = 15, keyProtocol = 2, raw = 3, closeOnExec = 0x80000;
        routing.setBootContext("u:r:shell:s0");
        assert(syscall(198, {keyFamily, raw, keyProtocol}) == uint64_t(-13));
        routing.setBootContext("u:r:system_server:s0");
        assert(syscall(198, {keyFamily, 2, keyProtocol}) == uint64_t(-94));
        assert(syscall(198, {keyFamily, raw, 0}) == uint64_t(-93));
        const auto key = syscall(198, {keyFamily, raw | closeOnExec, keyProtocol});
        assert(key < 256 && syscall(25, {key, 1}) == 1);
        assert(syscall(57, {key}) == 0 && syscall(57, {key}) == uint64_t(-9));

        assert(syscall(146, {1000}) == 0);
        assert(syscall(198, {keyFamily, raw, keyProtocol}) == uint64_t(-1));
        acknowledge(0x1d, 0, {}, -1);
        assert(syscall(57, {route}) == 0);
    }
    {
        struct CameraFixture final : GuestCameraSource {
            Status state = Status::unavailable;
            unsigned stops = 0;
            bool available() const override { return true; }
            bool start() override { state = Status::starting; return true; }
            Status status() const override { return state; }
            bool copyFrame(Format format, std::span<uint8_t> bytes) override {
                assert(bytes.size() == frameBytes(format));
                std::ranges::fill(bytes, format == Format::rgba ? 0x92 : 0x31);
                return true;
            }
            void stop() override { state = Status::unavailable; ++stops; }
        };
        auto camera = std::make_shared<CameraFixture>();
        ProbeLinux guest(1, 0);
        guest.setCamera(camera);
        auto device = guest.createMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto syscall = [&](uint64_t number, std::initializer_list<uint64_t> args) {
            device.cpu.x.fill(0);
            std::ranges::copy(args, device.cpu.x.begin());
            device.cpu.x[8] = number;
            assert(guest.syscall(device));
            return device.cpu.x[0];
        };
        const auto word = [&](uint64_t address) {
            uint64_t value = 0;
            assert(device.load64(address, value));
            return value;
        };
        constexpr auto path = base + 64, wire = base + 256;
        const auto put = [&](uint64_t address, const std::string& text) {
            assert(device.write(address, {reinterpret_cast<const uint8_t*>(text.c_str()), text.size() + 1}));
        };
        const auto open = [&](const std::string& name) {
            put(path, name);
            return syscall(56, {uint32_t(-100), path, 0x802});
        };
        const auto send = [&](uint64_t fd, const std::string& command) {
            put(wire, command);
            return syscall(64, {fd, wire, command.size() + 1});
        };
        const auto reply = [&](uint64_t fd) {
            assert(syscall(63, {fd, wire, 8}) == 8);
            const auto* hex = reinterpret_cast<const char*>(device.read(wire, 8, 4));
            assert(hex);
            size_t length = 0;
            assert(std::from_chars(hex, hex + 8, length, 16).ec == std::errc{});
            assert(length > 0 && length <= 256 && syscall(63, {fd, wire, length}) == length);
            const auto* body = reinterpret_cast<const char*>(device.read(wire, length, 4));
            assert(body && body[length - 1] == '\0');
            return std::string(body, length - 1);
        };
        const auto command = [&](uint64_t fd, const std::string& text) {
            assert(send(fd, text) == text.size() + 1);
            return reply(fd);
        };
        guest.setBootContext("u:r:shell:s0");
        const auto denied = open("/dev/goldfish_pipe_dprctd");
        assert(denied < 256 && send(denied, "pipe:qemud:camera") == uint64_t(-2));
        assert(syscall(57, {denied}) == 0);
        assert(syscall(198, {40, 1, 0}) == uint64_t(-97));
        guest.setBootContext("u:r:hal_camera_default:s0");
        const auto vsock = syscall(198, {40, 1, 0});
        assert(vsock < 256 && syscall(63, {vsock, wire, 8}) == uint64_t(-107));
        assert(device.store64(wire, uint64_t(5001) << 32 | 40) && device.store64(wire + 8, 2));
        assert(syscall(203, {vsock, wire, 16}) == uint64_t(-111));
        assert(device.store64(wire, uint64_t(5000) << 32 | 40));
        const auto duplicate = syscall(23, {vsock});
        assert(duplicate < 256 && syscall(203, {duplicate, wire, 16}) == 0);
        assert(syscall(203, {vsock, wire, 16}) == uint64_t(-106));
        assert(send(vsock, "pipe:qemud:camera") == 18);
        assert(command(duplicate, "list").starts_with("ok:name=nestvane-rear "));
        assert(syscall(57, {duplicate}) == 0 && syscall(57, {vsock}) == 0);
        const auto catalog = open("/dev/goldfish_pipe_dprctd");
        assert(catalog < 256);
        put(wire, "pipe:qemud:camera");
        assert(syscall(64, {catalog, wire, 5}) == 5);
        assert(syscall(64, {catalog, wire + 5, 13}) == 13);
        guest.setBootContext("u:r:shell:s0");
        assert(send(catalog, "list") == uint64_t(-13));
        guest.setBootContext("u:r:hal_camera_default:s0");
        assert(command(catalog, "list").starts_with("ok:name=nestvane-rear "));
        assert(syscall(57, {catalog}) == 0);

        guest.setBootContext("u:r:hal_sensors_default:s0");
        const auto sensors = syscall(198, {40, 0x801, 0});
        assert(sensors < 256 && syscall(63, {sensors, wire, 4}) == uint64_t(-107));
        assert(device.store64(wire, uint64_t(5000) << 32 | 40) && device.store64(wire + 8, 2));
        assert(syscall(203, {sensors, wire, 16}) == 0);
        assert(send(sensors, "pipe:qemud:sensors") == 19);
        const auto sendFrame = [&](std::string_view bytes) {
            assert(device.write(wire, {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}));
            assert(syscall(64, {sensors, wire, bytes.size()}) == bytes.size());
        };
        sendFrame("0006time:0");
        sendFrame("00");
        assert(syscall(63, {sensors, wire, 5}) == uint64_t(-11));
        sendFrame("0clist-sensors0004wake");
        guest.setBootContext("u:r:hal_camera_default:s0");
        assert(syscall(63, {sensors, wire, 5}) == uint64_t(-13));
        guest.setBootContext("u:r:hal_sensors_default:s0");
        assert(syscall(63, {sensors, wire, 4}) == 4);
        assert(std::memcmp(device.read(wire, 4, 4), "0001", 4) == 0);
        assert(syscall(63, {sensors, wire, 9}) == 9);
        assert(std::memcmp(device.read(wire, 9, 4), "00004wake", 9) == 0);
        assert(syscall(63, {sensors, wire, 1}) == uint64_t(-11));
        assert(syscall(57, {sensors}) == 0);

        guest.setBootContext("u:r:hal_camera_default:s0");
        const auto rear = open("/dev/goldfish_pipe_dprctd");
        const std::string rearService = "pipe:qemud:camera:name=nestvane-rear";
        assert(rear < 256 && send(rear, rearService) == rearService.size() + 1);
        assert(command(rear, "connect") == "ok");
        assert(send(rear, "start") == 6 && syscall(63, {rear, wire, 8}) == uint64_t(-11));
        camera->state = GuestCameraSource::Status::ready;
        assert(reply(rear) == "ok");
        assert(command(rear, "frame dim=640x480 pix=876758866 offset=0").starts_with("ko:"));
        const auto address = open("/dev/goldfish_address_space");
        assert(address < 256);
        const auto ping = [&](uint64_t size, uint64_t metadata) {
            assert(device.store64(wire, 0) && device.store64(wire + 8, size) &&
                   device.store64(wire + 16, metadata) && device.store64(wire + 24, 40) &&
                   device.store64(wire + 32, 0));
            assert(syscall(29, {address, 0xc028470c, wire}) == 0 && word(wire + 16) == 0);
        };
        ping(0, 6);
        ping(GuestCameraSource::frameBytes(GuestCameraSource::Format::rgba), 1);
        const auto offset = word(wire), length = word(wire + 8);
        assert(device.store64(wire, offset) && device.store64(wire + 8, length));
        assert(syscall(29, {address, 0xc010470d, wire}) == 0);
        const auto mapping = syscall(222, {0, length, 3, 1, address, offset});
        assert(mapping < (1ULL << 45));
        assert(command(rear, "frame dim=640x480 pix=876758866 offset=" + std::to_string(offset)) == "ok");
        assert(word(mapping) == 0x9292929292929292ULL);
        auto dma = std::make_shared<GuestColorDma>();
        guest.setGraphicsDma(dma);
        const auto waitOnRenderer = [&](auto request) {
            using namespace std::chrono;
            auto worker = std::async(std::launch::async, std::move(request));
            const auto deadline = steady_clock::now() + seconds(3);
            while (worker.wait_for(milliseconds(1)) != std::future_status::ready) {
                assert(steady_clock::now() < deadline);
                device.cpu.retired += 4096;
                guest.schedule(device, steady_clock::now() + milliseconds(2));
            }
            return worker.get();
        };
        const auto physical = 0x800000000ULL + offset;
        assert(waitOnRenderer([&] {
            void* pixels = nullptr;
            auto* token = GuestColorDma::acquire(dma.get(), physical, 8, false, &pixels);
            if (!token) return false;
            const auto view = std::span(static_cast<const uint8_t*>(pixels), 8);
            const auto correct = std::ranges::all_of(view, [](uint8_t color) { return color == 0x92; });
            return GuestColorDma::release(dma.get(), token, false) && correct;
        }));
        assert(waitOnRenderer([&] {
            void* pixels = nullptr;
            auto* token = GuestColorDma::acquire(dma.get(), physical + 2, 4, true, &pixels);
            if (!token) return false;
            std::memset(pixels, 0x4d, 4);
            return GuestColorDma::release(dma.get(), token, true);
        }));
        assert(word(mapping) == 0x92924d4d4d4d9292ULL);
        assert(waitOnRenderer([&] {
            void* pixels = nullptr;
            return !GuestColorDma::acquire(dma.get(), physical + length - 1, 2, false, &pixels);
        }));
        assert(command(rear, "frame dim=640x480 pix=842093913 offset=" + std::to_string(offset)) == "ok");
        assert(word(mapping) == 0x3131313131313131ULL);
        assert(command(rear, "frame dim=640x480 pix=876758866 offset=" + std::to_string(offset + length)).starts_with("ko:"));
        assert(command(rear, "stop") == "ok" && camera->stops == 1);
        assert(send(rear, "start") == 6 && syscall(57, {rear}) == 0 && camera->stops == 2);
        assert(syscall(215, {mapping, length}) == 0 && syscall(57, {address}) == 0);
    }
    {
        using namespace std::chrono;
        struct EchoGraphicsPipe : GraphicsPipe {
            std::vector<uint8_t> queued;
            int64_t read(std::span<uint8_t> bytes) override {
                if (queued.empty()) return -11;
                const auto count = std::min(queued.size(), bytes.size());
                std::copy_n(queued.begin(), count, bytes.begin());
                queued.erase(queued.begin(), queued.begin() + count);
                return count;
            }
            int64_t write(std::span<const uint8_t> bytes) override {
                queued.insert(queued.end(), bytes.begin(), bytes.end());
                return bytes.size();
            }
            uint16_t pollEvents() const override { return 4 | (queued.empty() ? 0 : 1); }
        };
        constexpr uint64_t cwd = uint32_t(-100), path = base + 512, buffer = base + 1024, event = base + 768;
        put(path, "/dev/goldfish_pipe_dprctd");
        assert(call(56, {cwd, path, 0x802}) == uint64_t(-2));
        unsigned connected = 0;
        linux.setGraphicsPipeFactory([&](std::string_view service) -> std::unique_ptr<GraphicsPipe> {
            assert(service == "opengles");
            ++connected;
            return std::make_unique<EchoGraphicsPipe>();
        });
        const auto channel = call(56, {cwd, path, 0x802});
        const auto duplicate = call(23, {channel});
        const auto poll = call(20, {0});
        assert(channel < 256 && duplicate < 256 && poll < 256 && connected == 0);
        assert(call(63, {channel, buffer, 2}) == uint64_t(-11) && !linux.blocked());
        put(buffer, "pipe:");
        assert(call(64, {channel, buffer, 5}) == 5 && connected == 0);
        put(buffer, "opengles");
        assert(machine.store64(buffer + 9, 0x0201));
        assert(call(64, {duplicate, buffer, 11}) == 9 && connected == 1);
        assert(machine.store64(event, 1) && machine.store64(event + 8, 77));
        assert(call(21, {poll, 1, duplicate, event}) == 0);
        assert(call(22, {poll, event, 1, 0}) == 0);
        assert(call(22, {poll, event, 1, 100}) == poll && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(64, {channel, buffer + 9, 2}) == 2);
        assert(call(22, {poll, event, 1, 100}) == 1 && load(event) == 1 && load(event + 8) == 77);
        assert(call(63, {duplicate, 0, 2}) == uint64_t(-14));
        assert(call(63, {duplicate, buffer, 1}) == 1 && *machine.read(buffer, 1, 4) == 1);
        assert(call(63, {channel, buffer, 1}) == 1 && *machine.read(buffer, 1, 4) == 2);
        assert(call(63, {duplicate, buffer, 524288}) == uint64_t(-11));
        assert(call(63, {duplicate, buffer, 524288}) == uint64_t(-11) && !linux.blocked());
        assert(call(22, {poll, event, 1, 0}) == 0);
        assert(call(64, {channel, buffer + 9, 2}) == 2);
        assert(call(63, {channel, buffer, 524288}) == 2 && uint16_t(load(buffer)) == 0x0201);
        assert(call(63, {duplicate, buffer, 1}) == uint64_t(-11));
        const auto graphicsReport = linux.workerReport(machine, linux.processId());
        const auto pipeReport = graphicsReport.find("OpenGL pipe fd=");
        assert(pipeReport != std::string::npos);
        assert(graphicsReport.find("OpenGL pipe fd=", pipeReport + 1) == std::string::npos);
        assert(graphicsReport.find("received=4 sent=4 read EAGAIN=3 write EAGAIN=0 last read=-11 last write=2",
                                   pipeReport) != std::string::npos);
        assert(call(57, {duplicate}) == 0 && call(57, {channel}) == 0 && call(57, {poll}) == 0);
        const auto other = call(56, {cwd, path, 0x802});
        put(buffer, "pipe:qemud");
        assert(call(64, {other, buffer, 11}) == uint64_t(-2) && connected == 1);
        assert(call(64, {other, buffer, 1}) == uint64_t(-5));
        assert(call(57, {other}) == 0);
        static std::vector<uint32_t> released;
        linux.setGraphicsPipeFactory([](std::string_view service) -> std::unique_ptr<GraphicsPipe> {
            assert(service == "refcount");
            return std::make_unique<GfxstreamRefcountPipe>(+[](uint32_t handle) { released.push_back(handle); });
        });
        const auto ownerPipe = call(56, {cwd, path, 0x802});
        const auto copiedPipe = call(23, {ownerPipe});
        put(buffer, "pipe:refcount");
        assert(call(64, {copiedPipe, buffer, 14}) == 14);
        assert(machine.store64(buffer, 0x67452301));
        assert(call(63, {ownerPipe, buffer, 4}) == uint64_t(-5));
        assert(call(64, {ownerPipe, buffer, 2}) == 2);
        assert(call(64, {copiedPipe, buffer + 2, 4}) == 2);
        assert(call(64, {ownerPipe, buffer, 4}) == uint64_t(-22));
        assert(call(57, {ownerPipe}) == 0 && released.empty());
        assert(call(57, {copiedPipe}) == 0 && released == std::vector<uint32_t>{0x67452301});

        const auto partialPipe = call(56, {cwd, path, 0x802});
        put(buffer, "pipe:refcount");
        assert(call(64, {partialPipe, buffer, 14}) == 14);
        assert(call(64, {partialPipe, buffer, 2}) == 2);
        assert(call(57, {partialPipe}) == 0 && released.size() == 1);

        put(path, "/dev/goldfish_address_space");
        const auto owner = call(56, {cwd, path, 2}), importer = call(56, {cwd, path, 2});
        assert(owner < 256 && importer < 256 && owner != importer);
        constexpr uint64_t wireBase = 0x800000000ULL;
        const auto ping = [&](uint64_t fd, uint64_t offset, uint64_t size, uint64_t metadata) {
            assert(machine.store64(buffer, offset) && machine.store64(buffer + 8, size) &&
                   machine.store64(buffer + 16, metadata) && machine.store64(buffer + 24, 40) &&
                   machine.store64(buffer + 32, 0));
            return call(29, {fd, 0xc028470c, buffer});
        };
        assert(ping(owner, 0, 0, 6) == 0 && load(buffer) == wireBase && load(buffer + 16) == 0);
        assert(ping(owner, 0, 4097, 1) == 0 && load(buffer + 16) == 0);
        const auto offset = load(buffer), length = load(buffer + 8);
        assert(offset % 16384 == 0 && length == 16384);
        assert(call(222, {0, length, 3, 1, importer, offset}) == uint64_t(-1));
        assert(call(222, {0, length, 3, 1, owner, offset}) == uint64_t(-1));
        const auto claim = [&](uint64_t fd, uint64_t size) {
            assert(machine.store64(buffer, offset) && machine.store64(buffer + 8, size));
            return call(29, {fd, 0xc010470d, buffer});
        };
        assert(claim(importer, length + 1) == uint64_t(-22));
        assert(claim(owner, length) == 0 && claim(importer, length) == 0 && claim(importer, length) == 0);
        const auto left = call(222, {0, length, 2, 1, owner, offset});
        const auto right = call(222, {0, length, 2, 1, importer, offset});
        assert(left < (uint64_t{1} << 45) && right < (uint64_t{1} << 45) && left != right);
        assert(machine.store64(left + pageSize, 0x09375674a1b2c3d4) && load(right + pageSize) == 0x09375674a1b2c3d4);
        assert(call(226, {right + pageSize, pageSize, 0}) == 0);
        uint64_t protectedValue = 0;
        assert(!machine.load64(right + pageSize, protectedValue));
        assert(call(226, {right + pageSize, pageSize, 2}) == 0);
        assert(load(right + pageSize) == 0x09375674a1b2c3d4);
        assert(machine.store64(buffer, offset) && call(29, {importer, 0xc008470e, buffer}) == 0);
        const auto third = call(222, {0, pageSize, 3, 1, importer, offset + pageSize});
        assert(third < (uint64_t{1} << 45) && load(third) == load(right + pageSize));
        assert(ping(owner, offset, 0, 2) == 0 && load(buffer) == wireBase + offset && load(buffer + 16) == 0);
        assert(claim(importer, length) == uint64_t(-6));
        assert(call(57, {owner}) == 0 && call(57, {importer}) == 0);
        assert(machine.store64(third, 99) && load(left + pageSize) == 99);
        assert(call(215, {left, length}) == 0 && call(215, {right, length}) == 0 && call(215, {third, pageSize}) == 0);

        const auto normal = call(56, {cwd, path, 2});
        assert(normal < 256 && ping(normal, 0, 0, 5) == 0 && load(buffer + 16) == 0);
        assert(machine.store64(buffer, pageSize));
        assert(call(29, {normal, 0xc018470a, buffer}) == 0);
        const auto directSize = load(buffer), directOffset = load(buffer + 8);
        assert(directSize >= pageSize && load(buffer + 16) == wireBase + directOffset);
        assert(ping(normal, directOffset, directSize, 1) == 0 && load(buffer + 16) == 0);
        const auto direct = call(222, {0, pageSize, 3, 1, normal, directOffset});
        assert(direct < (uint64_t{1} << 45) && load(direct) == 0);
        assert(ping(normal, directOffset, 0, 2) == 0 && load(buffer + 16) == 0);
        assert(machine.store64(buffer, directOffset) && call(29, {normal, 0xc008470b, buffer}) == 0);
        assert(call(222, {0, pageSize, 3, 1, normal, directOffset}) == uint64_t(-1));
        assert(call(215, {direct, pageSize}) == 0 && call(57, {normal}) == 0);
        linux.setGraphicsPipeFactory({});
    }
    {
        using namespace std::chrono;
        constexpr uint64_t pair = base + 512, timeout = base + 544, length = base + 576;
        assert(call(199, {1, 1, 0, pair}) == 0);
        const auto sender = uint32_t(load(pair)), receiver = uint32_t(load(pair) >> 32);
        assert(machine.store64(timeout, 0) && machine.store64(timeout + 8, 1'000'000));
        assert(call(208, {receiver, 1, 20, timeout, 16}) == uint64_t(-33));
        assert(machine.store64(timeout + 8, 20'000));
        assert(call(208, {receiver, 1, 20, timeout, 8}) == uint64_t(-22));
        assert(call(208, {receiver, 1, 20, timeout, 16}) == 0);
        assert(machine.store64(length, 16) && machine.store64(timeout + 8, 0));
        assert(call(209, {receiver, 1, 66, timeout, length}) == 0);
        assert(load(length) == 16 && load(timeout) == 0 && load(timeout + 8) == 20'000);
        assert(call(207, {receiver, base, 1, 0x40}) == uint64_t(-11) && !linux.blocked());
        assert(call(63, {receiver, base, 1}) == receiver && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(63, {receiver, base, 1}) == uint64_t(-11) && !linux.blocked());
        assert(call(63, {receiver, base, 1}) == receiver && linux.blocked());
        put(base + 16, "x");
        assert(call(64, {sender, base + 16, 1}) == 1 && !linux.blocked());
        assert(call(63, {receiver, base, 1}) == 1 && *machine.read(base, 1, 4) == 'x');
        constexpr uint64_t buffer = base + 1024, vector = base + 640;
        assert(machine.store64(timeout, 0) && machine.store64(timeout + 8, 30'000));
        assert(call(208, {sender, 1, 21, timeout, 16}) == 0);
        assert(machine.store64(length, 16) && machine.store64(timeout + 8, 0));
        assert(call(209, {sender, 1, 67, timeout, length}) == 0);
        assert(load(length) == 16 && load(timeout) == 0 && load(timeout + 8) == 30'000);
        assert(machine.store64(length, 16) && call(209, {sender, 1, 20, timeout, length}) == 0);
        assert(load(timeout) == 0 && load(timeout + 8) == 0);
        assert(machine.store64(length, 1024) && call(208, {sender, 1, 7, length, 4}) == 0);
        assert(call(64, {sender, buffer, 2048}) == 2048);
        assert(call(206, {sender, buffer, 1, 0x40}) == uint64_t(-11) && !linux.blocked());
        assert(call(206, {sender, buffer, 1}) == sender && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(206, {sender, buffer, 1}) == uint64_t(-11) && !linux.blocked());
        assert(call(64, {sender, buffer, 1}) == sender && linux.blocked());
        assert(call(63, {receiver, buffer, 1}) == 1 && !linux.blocked());
        assert(call(64, {sender, buffer, 1}) == 1);
        assert(machine.store64(vector, buffer) && machine.store64(vector + 8, 1));
        assert(call(66, {sender, vector, 1}) == sender && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(66, {sender, vector, 1}) == uint64_t(-11) && !linux.blocked());
        assert(call(57, {sender}) == 0 && call(57, {receiver}) == 0);
    }
    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t floor = 0x1'0000'0000, limit = 0x20'0000'0000;
        constexpr uint64_t fixedPrivate = 0x100022;
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto reserve = [&](uint64_t address, uint64_t length, uint64_t protection) {
            return invoke(222, {address, length, protection,
                                address ? fixedPrivate : 0x22, UINT64_MAX, 0});
        };
        assert(reserve(floor, pageSize, 3) == floor && task.store64(floor, 0x1234));
        assert(reserve(limit - pageSize, pageSize, 3) == limit - pageSize);
        assert(task.store64(limit - pageSize, 0x5678));
        constexpr uint64_t guardBytes = 16 * 1024 * 1024;
        for (unsigned i = 0; i < 32; ++i) {
            const auto guard = reserve(0, guardBytes, 0);
            assert(guard == floor + pageSize);
            assert(invoke(226, {guard + pageSize, pageSize, 3}) == 0);
            uint64_t value = 1;
            assert(task.load64(guard + pageSize, value) && value == 0);
            assert(task.store64(guard + pageSize, i));
            assert(invoke(215, {guard, guardBytes}) == 0);
        }
        constexpr uint64_t source = floor + 8 * pageSize;
        assert(reserve(source, pageSize, 3) == source && task.store64(source, 42));
        assert(reserve(source + pageSize, pageSize, 3) == source + pageSize);
        const auto grown = invoke(216, {source, pageSize, 2 * pageSize, 1});
        assert(grown == floor + pageSize && !task.read(source, 1, 4));
        uint64_t value = 0;
        assert(task.load64(grown, value) && value == 42);
        assert(task.load64(floor, value) && value == 0x1234);
        assert(task.load64(limit - pageSize, value) && value == 0x5678);
    }
    {
        using namespace std::chrono;
        assert(machine.store64(base, 1) && machine.store64(base + 8, 50'000'000));
        assert(call(101, {base, 0}) == base && linux.blocked());
        assert(!linux.schedule(machine, steady_clock::now() + milliseconds(10)) && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(2)) && !linux.blocked());
        assert(call(101, {base, 0}) == 0);
        assert(machine.store64(base, 0) && machine.store64(base + 8, 1'000'000'000));
        assert(call(115, {1, 1, base, 0}) == uint64_t(-22) && !linux.blocked());
        assert(call(115, {1, 1, 1, 0}) == uint64_t(-14) && !linux.blocked());
        assert(machine.store64(base, 0) && machine.store64(base + 8, 0));
        assert(call(115, {1, 1, base, 1}) == 0 && !linux.blocked());
        assert(call(115, {1, 2, base, 0}) == uint64_t(-22) && !linux.blocked());
        assert(call(113, {1, base}) == 0);
        const auto future = load(base + 8) + 100'000'000;
        assert(machine.store64(base, load(base) + future / 1'000'000'000));
        assert(machine.store64(base + 8, future % 1'000'000'000));
        assert(call(115, {1, 1, base, 1}) == 1 && linux.blocked());
        assert(!linux.schedule(machine, steady_clock::now() + milliseconds(5)) && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(2)) && !linux.blocked());
        assert(call(115, {1, 1, base, 1}) == 0);
        machine.cpu.retired += 4096;
    }
    {
        constexpr uint64_t cwd = uint32_t(-100), event = base + 512, buffer = base + 1024;
        linux.addBootTmpfs("/watch", 0755, 0, 6);
        put(base, "/watch");
        assert(call(26, {0x1}) == uint64_t(-22));
        const auto monitor = call(26, {0x80800});
        const auto watch = call(27, {monitor, base, 0x3ca});
        assert(monitor < 256 && watch == 1);
        assert(call(27, {monitor, base, 0x100003ca}) == uint64_t(-17));
        assert(call(63, {monitor, buffer, 128}) == uint64_t(-11));
        const auto poll = call(20, {0x80000});
        assert(machine.store64(event, 1) && machine.store64(event + 8, 123));
        assert(call(21, {poll, 1, monitor, event}) == 0);
        assert(call(22, {poll, buffer, 1, 0}) == 0);
        put(base, "/watch/one");
        const auto file = call(56, {cwd, base, 0x42, 0600});
        assert(file < 256 && call(64, {file, base, 2}) == 2 && call(57, {file}) == 0);
        assert(call(22, {poll, buffer, 1, 0}) == 1 && load(buffer) == 1 && load(buffer + 8) == 123);
        assert(call(63, {monitor, buffer, 16}) == uint64_t(-22));
        assert(call(63, {monitor, buffer, 32}) == 32);
        assert(load(buffer) == (uint64_t{0x100} << 32 | watch));
        assert(std::memcmp(machine.read(buffer + 16, 4, 4), "one", 4) == 0);
        assert(call(63, {monitor, buffer, 128}) == 64);
        assert(load(buffer) == (uint64_t{2} << 32 | watch));
        assert(load(buffer + 32) == (uint64_t{8} << 32 | watch));
        put(base + 128, "/watch/two");
        assert(call(38, {cwd, base, cwd, base + 128}) == 0);
        assert(call(63, {monitor, buffer, 128}) == 64);
        assert(load(buffer) == (uint64_t{64} << 32 | watch));
        assert(load(buffer + 32) == (uint64_t{128} << 32 | watch));
        assert(uint32_t(load(buffer + 8)) && load(buffer + 8) == load(buffer + 40));
        assert(call(35, {cwd, base + 128, 0}) == 0 && call(28, {monitor, watch}) == 0);
        assert(call(63, {monitor, buffer, 128}) == 48);
        assert(load(buffer) == (uint64_t{512} << 32 | watch));
        assert(load(buffer + 32) == (uint64_t{0x8000} << 32 | watch));
        assert(call(63, {monitor, buffer, 128}) == uint64_t(-11));
        assert(call(57, {monitor}) == 0 && call(57, {poll}) == 0);
    }
    {
        put(base, "/proc/meminfo");
        assert(call(48, {uint32_t(-100), base, 4}) == 0);
        assert(call(79, {uint32_t(-100), base, base + 512, 0}) == 0);
        assert(load(base + 512 + 48) == 0);
        assert(call(56, {uint32_t(-100), base, 1}) == uint64_t(-30));
        const auto readMemInfo = [&] {
            const auto fd = call(56, {uint32_t(-100), base, 0x80000});
            assert(fd < 256);
            const auto length = call(63, {fd, base + 512, 512});
            assert(length > 0 && length < 512);
            const auto* bytes = machine.read(base + 512, length, 4);
            assert(bytes && call(63, {fd, base + 512, 512}) == 0);
            assert(call(57, {fd}) == 0);
            return std::string(reinterpret_cast<const char*>(bytes), length);
        };
        assert(call(179, {base + 512}) == 0);
        const auto total = load(base + 512 + 32);
        const auto freeBefore = load(base + 512 + 40);
        const auto initial = readMemInfo();
        assert(initial.starts_with("MemTotal: " + std::to_string(total / 1024) + " kB\n"));
        assert(initial.find("MemFree: " + std::to_string(freeBefore / 1024) + " kB\n") != std::string::npos);
        const auto mapped = call(222, {0, pageSize, 3, 0x22, uint32_t(-1), 0});
        assert(mapped != 0 && mapped < uint64_t(-4095) && machine.store64(mapped, 1));
        assert(call(179, {base + 512}) == 0);
        const auto freeAfter = load(base + 512 + 40);
        assert(freeAfter < freeBefore);
        const auto after = readMemInfo();
        assert(after.find("MemFree: " + std::to_string(freeAfter / 1024) + " kB\n") != std::string::npos);
        assert(after.find("SwapTotal: 0 kB\n") != std::string::npos);
        assert(call(215, {mapped, pageSize}) == 0);
    }
    {
        static constexpr std::array<uint8_t, 2> input{'o', 'k'};
        linux.mount("/test/canonical/input", input);
        linux.alias("/test/linked", "/test/canonical");
        put(base, "/test/linked/input");
        assert(call(48, {uint32_t(-100), base, 4}) == 0);
        assert(call(79, {uint32_t(-100), base, base + 128, 0}) == 0);
        assert(load(base + 128 + 48) == input.size());
    }
    {
        constexpr auto attr = base + 96, key = base + 384, data = base + 392, output = base + 400;
        const auto create = [&](uint32_t entries, uint32_t flags = 0) {
            const std::array<uint32_t, 5> args{2, 4, 8, entries, flags};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(args.data()), sizeof(args)}));
            return call(280, {0, attr, sizeof(args)});
        };
        const auto fd = create(2);
        assert(fd < 256 && call(25, {fd, 1}) == 1);
        assert(machine.store64(attr, 0) && machine.store64(attr + 8, 0));
        assert(call(280, {12, attr, 12}) == 0);
        const uint32_t mapId = load(attr) >> 32;
        assert(mapId && machine.store64(attr, mapId) && machine.store64(attr + 8, 8));
        const auto reopenedById = call(280, {14, attr, 12});
        assert(reopenedById < 256 && reopenedById != fd);
        const auto invoke = [&](uint64_t command, uint64_t descriptor, uint64_t index,
                                uint64_t value, uint64_t flags = 0) {
            assert(machine.store64(attr, descriptor) && machine.store64(attr + 8, index));
            assert(machine.store64(attr + 16, value) && machine.store64(attr + 24, flags));
            return call(280, {command, attr, 32});
        };
        assert(machine.store64(key, 1) && machine.store64(output, UINT64_MAX));
        assert(invoke(1, fd, key, output) == 0 && load(output) == 0);
        constexpr uint64_t expected = 0x1122334455667788ULL;
        assert(machine.store64(data, expected));
        assert(invoke(2, fd, key, data) == 0 && invoke(2, fd, key, data, 1) == uint64_t(-17));
        assert(invoke(2, fd, key, UINT64_MAX, 2) == uint64_t(-14));
        const auto dup = call(25, {fd, 0, 0});
        assert(dup < 256 && dup != fd && call(57, {fd}) == 0);
        assert(invoke(1, fd, key, output) == uint64_t(-9));
        assert(invoke(1, dup, key, output) == 0 && load(output) == expected);
        assert(invoke(3, dup, key, 0) == uint64_t(-22));
        assert(invoke(4, dup, 0, output) == 0 && (load(output) & UINT32_MAX) == 0);
        assert(invoke(4, dup, key, output) == uint64_t(-2));
        assert(machine.store64(key, 2));
        assert(invoke(1, dup, key, output) == uint64_t(-2));
        assert(invoke(2, dup, key, data, 2) == uint64_t(-7));
        assert(invoke(4, dup, key, output) == 0 && (load(output) & UINT32_MAX) == 0);
        assert(call(57, {dup}) == 0);
        assert(machine.store64(key, 1));
        assert(invoke(1, reopenedById, key, output) == 0 && load(output) == expected);
        assert(invoke(2, reopenedById, key, data) == uint64_t(-1));
        assert(call(57, {reopenedById}) == 0);
        assert(machine.store64(attr, mapId) && machine.store64(attr + 8, 0));
        assert(call(280, {14, attr, 12}) == uint64_t(-2));
        assert(machine.store64(attr, 0));
        assert(call(280, {12, attr, 12}) == uint64_t(-2));
        assert(machine.store64(key, 0));
        const auto readOnly = create(2, 8);
        assert(readOnly < 256 && invoke(1, readOnly, key, output) == 0);
        assert(invoke(2, readOnly, key, data) == uint64_t(-1));
        assert(call(57, {readOnly}) == 0);
        assert(create(0) == uint64_t(-22) && create(UINT32_MAX) == uint64_t(-12));
        const std::array<uint32_t, 5> hashArgs{1, 16, 8, 2, 1}; // BPF_F_NO_PREALLOC
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(hashArgs.data()), sizeof(hashArgs)}));
        const auto hash = call(280, {0, attr, sizeof(hashArgs)});
        assert(hash < 256);
        constexpr auto hashKey = base + 512, hashOut = base + 544;
        assert(machine.store64(hashKey, 1) && machine.store64(hashKey + 8, 7));
        assert(invoke(1, hash, hashKey, output) == uint64_t(-2));
        assert(invoke(2, hash, hashKey, data, 2) == uint64_t(-2));
        assert(invoke(2, hash, hashKey, data, 1) == 0);
        assert(invoke(2, hash, hashKey, data, 1) == uint64_t(-17));
        assert(machine.store64(hashKey + 8, 8) && invoke(2, hash, hashKey, data, 1) == 0);
        assert(machine.store64(hashKey + 8, 9) && invoke(2, hash, hashKey, data, 1) == uint64_t(-7));
        assert(invoke(4, hash, 0, hashOut) == 0 && load(hashOut) == 1 && load(hashOut + 8) == 7);
        assert(invoke(4, hash, hashOut, hashOut) == 0 && load(hashOut + 8) == 8);
        assert(invoke(4, hash, hashOut, hashOut) == uint64_t(-2));
        assert(invoke(2, hash, hashOut, UINT64_MAX, 2) == uint64_t(-14));
        assert(invoke(1, hash, hashOut, output) == 0 && load(output) == expected);
        assert(invoke(3, hash, hashOut, 0) == 0);
        assert(invoke(1, hash, hashOut, output) == uint64_t(-2));
        assert(invoke(3, hash, hashOut, 0) == uint64_t(-2));
        assert(invoke(2, hash, hashOut, data) == 0);
        constexpr auto cursor = base + 576, keys = base + 592, values = base + 640;
        const auto batch = [&](uint64_t command, uint64_t in, uint32_t count, uint64_t result) {
            const std::array<uint64_t, 7> args{in, cursor, keys, result, hash << 32 | count, 0, 0};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(args.data()), sizeof(args)}));
            return call(280, {command, attr, sizeof(args)});
        };
        assert(batch(24, 0, 2, values) == uint64_t(-2) && uint32_t(load(attr + 32)) == 2);
        assert(load(keys) == 1 && load(keys + 16) == 1);
        assert(load(keys + 8) + load(keys + 24) == 15);
        assert(load(values) == expected && load(values + 8) == expected);
        assert(batch(25, 0, 2, UINT64_MAX) == uint64_t(-14));
        assert(invoke(1, hash, hashOut, output) == 0);
        assert(batch(25, 0, 1, values) == 0 && uint32_t(load(attr + 32)) == 1);
        assert(invoke(1, hash, keys, output) == uint64_t(-2));
        assert(batch(25, cursor, 1, values) == uint64_t(-2) && uint32_t(load(attr + 32)) == 1);
        assert(invoke(4, hash, 0, hashOut) == uint64_t(-2));
        assert(batch(25, 0, 2, values) == uint64_t(-2) && uint32_t(load(attr + 32)) == 0);
        assert(call(57, {hash}) == 0);
        const std::array<uint32_t, 5> devmapArgs{25, 4, 4, 2, 0};
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(devmapArgs.data()), sizeof(devmapArgs)}));
        const auto devmap = call(280, {0, attr, sizeof(devmapArgs)});
        assert(devmap < 256 && machine.store64(key, 77) && machine.store64(data, 1));
        assert(invoke(2, devmap, key, data, 2) == 0);
        assert(machine.store64(data, UINT32_MAX) && invoke(2, devmap, key, data) == uint64_t(-22));
        assert(invoke(1, devmap, key, output) == 0 && (load(output) & UINT32_MAX) == 1);
        assert(invoke(3, devmap, key, 0) == 0 && invoke(1, devmap, key, output) == uint64_t(-2));
        assert(call(57, {devmap}) == 0);
        put(base, "/sys/class/net/lo/ifindex");
        const auto loopback = call(56, {uint32_t(-100), base, 0});
        assert(loopback < 256 && call(63, {loopback, output, 8}) == 2);
        assert((load(output) & 0xffff) == 0x0a31 && call(57, {loopback}) == 0);
        assert(machine.store64(data, expected));
        for (const uint32_t type : {5, 6}) {
            const std::array<uint32_t, 5> percpuArgs{type, 4, 5, 2, 0};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(percpuArgs.data()), sizeof(percpuArgs)}));
            const auto perCpu = call(280, {0, attr, sizeof(percpuArgs)});
            assert(perCpu < 256 && machine.store64(key, 1));
            assert(invoke(2, perCpu, key, data) == 0);
            assert(machine.store64(output + 8, 0xfeed));
            assert(invoke(1, perCpu, key, output) == 0 && load(output) == expected && load(output + 8) == 0xfeed);
            assert(call(57, {perCpu}) == 0);
        }
        put(base, "/sys/devices/system/cpu/possible");
        const auto possible = call(56, {uint32_t(-100), base, 0});
        assert(possible < 256 && call(63, {possible, output, 8}) == 2);
        assert((load(output) & 0xffff) == 0x0a30);
        assert(call(57, {possible}) == 0);
    }
    {
        constexpr auto attr = base + 0x180, key = base + 0x300, value = base + 0x320;
        const auto write = [&](uint64_t address, const auto& record) {
            return machine.write(address, {reinterpret_cast<const uint8_t*>(&record), sizeof(record)});
        };
        const std::array<uint32_t, 5> creation{2, 4, 3, 513, 1U << 10};
        assert(write(attr, creation));
        const auto map = call(280, {0, attr, sizeof(creation)});
        assert(map < 256);
        assert(call(222, {0, pageSize, 1, 2, map, 0}) == uint64_t(-22)); // MAP_PRIVATE
        assert(call(222, {0, 2 * pageSize, 1, 1, map, pageSize}) == uint64_t(-22));
        const auto rw = call(222, {0, 2 * pageSize, 3, 1, map, 0});
        const auto ro = call(222, {0, pageSize, 1, 1, map, pageSize});
        assert(rw < ro && ro < 0x20'0000'0000);
        assert(call(226, {ro, pageSize, 3}) == uint64_t(-13)); // A read-only VMA cannot gain write permission.
        const std::array<uint8_t, 3> initial{12, 34, 56}, updated{7, 8, 9};
        const std::array<uint64_t, 4> element{map, key, value, 0};
        assert(write(attr, element) && machine.store64(key, 1) && write(value, initial));
        assert(call(280, {2, attr, sizeof(element)}) == 0);
        assert(!std::memcmp(machine.read(rw + 8, initial.size(), 4), initial.data(), initial.size()));
        assert(write(rw + pageSize, updated) && machine.store64(key, 512));
        assert(call(280, {1, attr, sizeof(element)}) == 0);
        assert(!std::memcmp(machine.read(value, updated.size(), 4), updated.data(), updated.size()));
        const std::array<uint64_t, 2> identity{map | (8ULL << 32), value};
        assert(write(attr, identity) && call(280, {15, attr, sizeof(identity)}) == 0);
        const uint64_t id = load(value) >> 32;
        assert(id && call(57, {map}) == 0 && machine.store64(attr, id));
        assert(!std::memcmp(machine.read(ro, updated.size(), 4), updated.data(), updated.size()));
        const auto reopened = call(280, {14, attr, 8});
        assert(reopened < 256 && call(57, {reopened}) == 0);
        assert(call(215, {ro, pageSize}) == 0 && call(215, {rw, 2 * pageSize}) == 0);
        assert(call(280, {14, attr, 8}) == uint64_t(-2)); // The last VMA releases the map.
    }
    {
        constexpr auto from = base + 0x600, to = base + 0x680, attr = base + 0x700;
        constexpr auto key = base + 0x740, value = base + 0x748, output = base + 0x750;
        constexpr auto fs = base + 32, mountpoint = base + 64;
        put(fs, "bpf");
        put(mountpoint, "/sys/fs/bpf");
        assert(call(40, {fs, mountpoint, fs, 14}) == 0);
        const auto tokenRoot = call(56, {uint32_t(-100), mountpoint, 0x4000});
        const auto tokenPath = call(56, {uint32_t(-100), mountpoint, 0x204000});
        assert(tokenRoot < 256 && tokenPath < 256);
        assert(machine.store64(attr, tokenRoot << 32));
        assert(call(280, {36, attr, 8}) == uint64_t(-95));
        assert(machine.store64(attr, (tokenRoot << 32) | 1));
        assert(call(280, {36, attr, 8}) == uint64_t(-22));
        assert(machine.store64(attr, tokenPath << 32));
        assert(call(280, {36, attr, 8}) == uint64_t(-9));
        assert(machine.store64(attr, 0xffffffffULL << 32));
        assert(call(280, {36, attr, 8}) == uint64_t(-9));
        assert(call(57, {tokenPath}) == 0);
        put(from, "/sys/fs/bpf/nestvane_map");
        put(to, "/sys/fs/bpf/nestvane_moved");
        const std::array<uint32_t, 5> create{2, 4, 8, 2, 0};
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(create.data()), sizeof(create)}));
        const auto map = call(280, {0, attr, sizeof(create)});
        assert(map < 256 && machine.store64(key, 1) && machine.store64(value, 42));
        const auto elem = [&](uint64_t command, uint64_t fd, uint64_t slot) {
            assert(machine.store64(attr, fd) && machine.store64(attr + 8, key));
            assert(machine.store64(attr + 16, slot) && machine.store64(attr + 24, 0));
            return call(280, {command, attr, 32});
        };
        const auto object = [&](uint64_t command, uint64_t path, uint64_t fdAndFlags) {
            assert(machine.store64(attr, path) && machine.store64(attr + 8, fdAndFlags));
            assert(machine.store64(attr + 16, 0));
            return call(280, {command, attr, 24});
        };
        assert(elem(2, map, value) == 0);
        assert(object(6, from, map) == 0 && object(6, from, map) == uint64_t(-17));
        assert(call(56, {uint32_t(-100), from, 0}) == uint64_t(-5));
        assert(call(79, {uint32_t(-100), from, base + 0x780, 0}) == 0);
        assert((load(base + 0x790) & 0777) == 0600);
        assert(call(57, {map}) == 0);
        assert(call(38, {uint32_t(-100), from, uint32_t(-100), to}) == 0);
        assert(object(7, from, 0) == uint64_t(-2));
        const auto reopened = object(7, to, 8ULL << 32);
        assert(reopened < 256 && call(25, {reopened, 1}) == 1);
        assert(call(35, {uint32_t(-100), to, 0}) == 0 && object(7, to, 0) == uint64_t(-2));
        assert(elem(1, reopened, output) == 0 && load(output) == 42);
        assert(elem(2, reopened, value) == uint64_t(-1));
        assert(call(57, {reopened}) == 0);
        assert(call(39, {mountpoint, 0}) == 0);
        assert(machine.store64(attr, tokenRoot << 32));
        assert(call(280, {36, attr, 8}) == uint64_t(-95)); // The open FD still names the BPF root.
        assert(call(57, {tokenRoot}) == 0);
    }
    {
        // A restored /data tree and boot tmpfs entries must leave room for Android to pin BPF objects.
        char database[] = "/tmp/nestvane-file-capacity.XXXXXX";
        const int databaseFd = mkstemp(database);
        assert(databaseFd >= 0 && close(databaseFd) == 0);
        ProbeLinux storage(1, 0);
        storage.mountData(database);
        storage.addDirectory("/test", 0755);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(storage.syscall(task));
            return task.cpu.x[0];
        };
        const auto writePath = [&](uint64_t address, const std::string& path) {
            assert(task.write(address, {reinterpret_cast<const uint8_t*>(path.c_str()), path.size() + 1}));
        };
        constexpr auto type = base + 128, target = base + 256, attr = base + 512;
        writePath(type, "tmpfs");
        writePath(target, "/test");
        assert(invoke(40, {type, target, type, 14}) == 0);
        writePath(type, "bpf");
        writePath(target, "/sys/fs/bpf");
        assert(invoke(40, {type, target, type, 14}) == 0);
        const auto createFile = [&](const std::string& path) {
            writePath(base, path);
            const auto fd = invoke(56, {uint32_t(-100), base, 0xc1, 0600});
            if (fd < 256) assert(invoke(57, {fd}) == 0);
            return fd;
        };
        for (unsigned i = 0; i < 4096; ++i) assert(createFile("/data/f" + std::to_string(i)) < 256);
        for (unsigned i = 0; i < 2048; ++i) assert(createFile("/test/f" + std::to_string(i)) < 256);
        assert(createFile("/data/overflow") == uint64_t(-28));
        assert(createFile("/test/overflow") == uint64_t(-28));

        constexpr std::array<uint32_t, 5> createMap{2, 4, 8, 2, 0};
        assert(task.write(attr, {reinterpret_cast<const uint8_t*>(createMap.data()), sizeof(createMap)}));
        const auto map = invoke(280, {0, attr, sizeof(createMap)});
        assert(map < 256);
        const auto pin = [&](const std::string& name) {
            writePath(target, "/sys/fs/bpf/" + name);
            assert(task.store64(attr, target) && task.store64(attr + 8, map));
            return invoke(280, {6, attr, 16});
        };
        for (unsigned i = 0; i < 2048; ++i) assert(pin("map" + std::to_string(i)) == 0);
        assert(pin("overflow") == uint64_t(-28));
        writePath(target, "/sys/fs/bpf/map0");
        assert(invoke(35, {uint32_t(-100), target, 0}) == 0);
        assert(pin("overflow") == 0);

        writePath(base, "/test/f0");
        assert(invoke(35, {uint32_t(-100), base, 0}) == 0);
        assert(createFile("/test/replacement") < 256);
        assert(createFile("/test/overflow") == uint64_t(-28));
        const auto report = storage.workerReport(task);
        assert(report.find("persistent files=4096/4096 transient files=2048/2048 bpffs pins=2048/2048") !=
               std::string::npos);
        writePath(base, "/data");
        assert(invoke(43, {base, attr}) == 0);
        uint64_t count = 0;
        assert(task.load64(attr + 5 * sizeof(uint64_t), count) && count == 4096);
        assert(task.load64(attr + 6 * sizeof(uint64_t), count) && count == 0);

        writePath(base, "/data/f4095");
        const auto last = invoke(56, {uint32_t(-100), base, 1});
        assert(last < 256 && task.store64(attr, 0x0123456789abcdef));
        assert(invoke(64, {last, attr, sizeof(uint64_t)}) == sizeof(uint64_t));
        assert(invoke(82, {last}) == 0 && invoke(57, {last}) == 0);
        ProbeLinux reopened(1, 0);
        reopened.mountData(database);
        const auto path = reopened.bootGuestFile("/data/f4095");
        const uint64_t expected = 0x0123456789abcdef;
        assert(path.size() == sizeof(expected) && !std::memcmp(path.data(), &expected, sizeof(expected)));
        assert(reopened.workerReport(task).find("persistent files=4096/4096 transient files=0/2048") !=
               std::string::npos);
        assert(unlink(database) == 0);
    }
    {
        constexpr auto attr = base + 96, code = base + 256, license = base + 280, address = base + 384;
        constexpr auto message = base + 512, buffers = base + 600, packet = base + 640;
        put(license, "Apache 2.0");
        const auto program = [&](uint64_t first, uint32_t type = BpfProgram::socketFilter) {
            assert(machine.store64(code, first) && machine.store64(code + 8, 0x95)); // EXIT
            const std::array<uint64_t, 8> request{(2ULL << 32) | type, code, license, 0, 0, 0, 0, 0};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(request.data()), sizeof(request)}));
            return call(280, {5, attr, sizeof(request)}); // BPF_PROG_LOAD
        };
        assert(program(0xff) == uint64_t(-22));
        assert(program(0x05 | (uint64_t{3} << 16)) == uint64_t(-22)); // Out-of-range jump.
        constexpr auto fs = base + 32, mountpoint = base + 64, pinned = base + 880;
        put(fs, "bpf");
        put(mountpoint, "/sys/fs/bpf");
        put(pinned, "/sys/fs/bpf/prog_nestvane_filter");
        assert(call(40, {fs, mountpoint, fs, 14}) == 0);
        const auto uevent = call(198, {16, 2 | 0x80800, 15});
        assert(uevent < 256);
        assert(machine.store64(address, 16) && machine.store64(address + 8, 1));
        assert(call(200, {uevent, address, 12}) == 0);
        put(base, "/dev/loop-control");
        const auto control = call(56, {uint32_t(-100), base, 2});
        assert(control < 256);
        uint32_t previousProgramId = 0;
        for (const uint64_t limit : {0, 16}) {
            const auto loaded = program(0xb7 | (limit << 32)); // MOV64 r0, immediate
            assert(loaded < 256 && machine.store64(attr, previousProgramId) && machine.store64(attr + 8, 0));
            assert(call(280, {11, attr, 12}) == 0);
            const uint32_t programId = load(attr) >> 32;
            assert(programId > previousProgramId && machine.store64(attr, programId));
            previousProgramId = programId;
            const auto reopenedById = call(280, {13, attr, 12});
            assert(reopenedById < 256 && reopenedById != loaded && call(25, {reopenedById, 1}) == 1);
            assert(call(57, {reopenedById}) == 0 && machine.store64(attr, pinned));
            assert(machine.store64(attr + 8, loaded) && machine.store64(attr + 16, 0));
            assert(call(280, {6, attr, 24}) == 0 && call(57, {loaded}) == 0); // BPF_OBJ_PIN
            assert(machine.store64(attr + 8, 8ULL << 32)); // BPF_F_RDONLY
            const auto reopened = call(280, {7, attr, 24}); // BPF_OBJ_GET
            assert(reopened < 256 && call(25, {reopened, 1}) == 1);
            assert(call(35, {uint32_t(-100), pinned, 0}) == 0);
            assert(machine.store64(address, reopened));
            assert(call(208, {uevent, 1, 50, address, 4}) == 0 && call(57, {reopened}) == 0);
            assert(call(29, {control, 0x4c80, 31 + limit}) == 31 + limit); // LOOP_CTL_ADD
            const std::array<uint64_t, 7> header{0, 0, buffers, 1, 0, 0, 0};
            assert(machine.store64(buffers, packet) && machine.store64(buffers + 8, 256));
            assert(machine.write(message, {reinterpret_cast<const uint8_t*>(header.data()), sizeof(header)}));
            assert(call(212, {uevent, message, 0}) == (limit ? limit : uint64_t(-11)));
        }
        assert(call(57, {control}) == 0 && call(57, {uevent}) == 0);
        assert(machine.store64(attr, 0) && machine.store64(attr + 8, 0));
        assert(call(280, {11, attr, 12}) == uint64_t(-2));

        const auto tracepoint = program(0xb7, BpfProgram::tracepoint);
        assert(tracepoint < 256);
        constexpr auto tracePath = base + 920;
        put(tracePath, "/sys/fs/bpf/prog_nestvane_tracepoint");
        assert(machine.store64(attr, tracePath) && machine.store64(attr + 8, tracepoint) &&
               machine.store64(attr + 16, 0));
        assert(call(280, {6, attr, 24}) == 0 && call(57, {tracepoint}) == 0);
        assert(machine.store64(attr + 8, 8ULL << 32));
        const auto reopenedTracepoint = call(280, {7, attr, 24});
        assert(reopenedTracepoint < 256 && call(57, {reopenedTracepoint}) == 0);
        assert(call(35, {uint32_t(-100), tracePath, 0}) == 0);

        // Install a real pinned socket filter through the same xtables ABI netd uses.
        std::vector<uint8_t> savedScratch(pageSize);
        assert(machine.readInto(base, savedScratch));
        constexpr auto table = base + 0x760, optLength = base + 0xe40, counters = base + 0xe60;
        const auto firewallFd = call(198, {2, 3, 255});
        const auto receiver = call(198, {2, 0x802, 0}), sender = call(198, {2, 0x802, 0});
        assert(firewallFd < 256 && receiver < 256 && sender < 256);
        const auto write = [&](uint64_t guest, const auto& value) {
            return machine.write(guest, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
        };
        const auto store = [](auto& bytes, size_t offset, const auto& value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        struct Info { char name[32]; uint32_t hooks, starts[5], policies[5], count, size; };
        static_assert(sizeof(Info) == 84);
        const Info blank{"mangle", 0, {}, {}, 0, 0};
        assert(write(table, blank) && write(optLength, uint32_t(sizeof(blank))));
        assert(call(209, {firewallFd, 0, 64, table, optLength}) == 0);
        Info info{};
        assert(machine.readInto(table, {reinterpret_cast<uint8_t*>(&info), sizeof(info)}));
        assert(write(table + 32, info.size) && write(optLength, info.size + 40));
        assert(call(209, {firewallFd, 0, 65, table, optLength}) == 0);
        std::vector<uint8_t> original(info.size);
        assert(machine.readInto(table + 40, original));
        std::vector<uint8_t> rule(112 + 560 + 40);
        store(rule, 88, uint16_t(112 + 560));
        store(rule, 90, uint16_t(rule.size()));
        store(rule, 112, uint16_t(560));
        std::memcpy(rule.data() + 114, "bpf", 3);
        rule[143] = 1;
        store(rule, 144, uint16_t(1)); // Pinned object mode.
        constexpr std::string_view pinnedName = "/sys/fs/bpf/prog_nestvane_filter";
        std::ranges::copy(pinnedName, rule.begin() + 152);
        store(rule, 672, uint16_t(40));
        store(rule, 704, int32_t(-1));
        const auto install = [&](uint32_t oldCount, bool enabled = true) {
            auto entries = original;
            const auto length = enabled ? rule.size() : 0;
            if (enabled) entries.insert(entries.begin() + info.starts[3], rule.begin(), rule.end());
            std::array<uint8_t, 96> replace{};
            std::memcpy(replace.data(), info.name, sizeof(info.name));
            store(replace, 32, info.hooks);
            store(replace, 36, info.count + uint32_t(enabled));
            store(replace, 40, uint32_t(entries.size()));
            for (unsigned h = 0; h < 5; ++h) {
                store(replace, 44 + 4 * h, info.starts[h] + uint32_t(h > 3 ? length : 0));
                store(replace, 64 + 4 * h, info.policies[h] + uint32_t(h >= 3 ? length : 0));
            }
            store(replace, 84, oldCount);
            store(replace, 88, counters);
            assert(machine.write(table, replace) && machine.write(table + replace.size(), entries));
            return call(208, {firewallFd, 0, 64, table, replace.size() + entries.size()});
        };
        std::array<uint8_t, 16> local{2, 0, 0, 0, 127, 0, 0, 1};
        assert(write(address, local) && call(200, {receiver, address, local.size()}) == 0);
        assert(write(optLength, uint32_t(local.size())) && call(204, {receiver, address, optLength}) == 0);
        const auto send = [&] { return call(206, {sender, packet, 8, 0, address, local.size()}); };
        const auto match = program(0xb7 | (1ULL << 32));
        assert(match < 256 && write(attr, std::array<uint64_t, 3>{pinned, match, 0}));
        assert(install(info.count) == uint64_t(-2));
        assert(call(280, {6, attr, 24}) == 0 && install(info.count) == 0);
        assert(call(35, {uint32_t(-100), pinned, 0}) == 0 && call(57, {match}) == 0);
        assert(install(info.count + 1) == uint64_t(-2)); // The old rules retain their program.
        assert(send() == uint64_t(-1) && call(207, {receiver, packet, 8}) == uint64_t(-11));
        const auto quiet = program(0xb7); // Return 0, so the rule does not match.
        assert(quiet < 256);
        store(rule, 144, uint16_t(2));
        store(rule, 148, uint32_t(quiet));
        assert(install(info.count + 1) == 0 && call(57, {quiet}) == 0);
        assert(send() == 8 && call(207, {receiver, packet, 8}) == 8);
        assert(install(info.count + 1, false) == 0);
        assert(call(57, {firewallFd}) == 0 && call(57, {receiver}) == 0 && call(57, {sender}) == 0);
        assert(call(39, {mountpoint, 0}) == 0);
        assert(machine.write(base, savedScratch));
    }
    {
        constexpr auto code = base + 0x100, attr = base + 0x200, license = base + 0x280;
        constexpr auto packet = base + 0x300, output = base + 0x380, key = base + 0x400;
        const auto write = [&](uint64_t address, const auto& record) {
            return machine.write(address, {reinterpret_cast<const uint8_t*>(&record), sizeof(record)});
        };
        const std::array<uint32_t, 5> create{2, 4, 8, 1, 0}; // ARRAY
        assert(write(attr, create));
        const auto counter = call(280, {0, attr, sizeof(create)});
        assert(counter < 256);
        auto encode = [](uint8_t op, uint8_t regs, int16_t off = 0, int32_t imm = 0) {
            return uint64_t(op) | (uint64_t(regs) << 8) | (uint64_t(uint16_t(off)) << 16) |
                (uint64_t(uint32_t(imm)) << 32);
        };
        const std::array<uint64_t, 18> instructions{
            encode(0xb7, 0x00, 0, 0),                     // default TC action
            encode(0x61, 0x16, 76),                        // r6 = skb.data
            encode(0x61, 0x17, 80),                        // r7 = skb.data_end
            encode(0xbf, 0x68), encode(0x07, 0x08, 0, 15), // r8 = r6 + 15
            encode(0x2d, 0x78, 10),                       // if r8 > r7, exit
            encode(0xb7, 0x00, 0, 0),                     // zero key on the stack
            encode(0x63, 0x0a, -4),
            encode(0xbf, 0xa2), encode(0x07, 0x02, 0, -4), // r2 = &key
            encode(0x18, 0x11, 0, counter), 0,             // r1 = relocated array map
            encode(0x85, 0x00, 0, 1),                      // r0 = map_lookup_elem(r1, r2)
            encode(0x15, 0x00, 2),                         // if r0 == nullptr, exit
            encode(0x71, 0x61, 14),                        // r1 = frame[14]
            encode(0xdb, 0x10),                            // atomic *(u64 *)r0 += r1
            encode(0xb7, 0x00, 0, 0),
            encode(0x95, 0x00),
        };
        put(license, "GPL");
        const auto loadProgram = [&](uint32_t type) {
            const std::array<uint64_t, 8> request{(uint64_t(instructions.size()) << 32) | type, code, license};
            assert(write(attr, request));
            return call(280, {5, attr, sizeof(request)});
        };
        assert(write(code, instructions));
        const auto filter = loadProgram(1);
        assert(filter < 256);
        const std::array<uint8_t, 18> filterPacket{};
        const std::array<uint64_t, 5> filterRun{filter, filterPacket.size(), packet};
        assert(write(packet, filterPacket) && write(attr, filterRun));
        // Filters can perform map operations, but cannot access skb.data directly.
        assert(call(280, {10, attr, sizeof(filterRun)}) == uint64_t(-22));
        assert(call(57, {filter}) == 0);
        const auto program = loadProgram(3);
        assert(program < 256);
        const auto info = base + 0x500, ids = base + 0x580;
        std::array<uint64_t, 11> programInfo{};
        const std::array<uint64_t, 2> shortInfo{program | (8ULL << 32), info};
        assert(write(attr, shortInfo) && call(280, {15, attr, sizeof(shortInfo)}) == 0);
        const auto identity = load(info);
        assert((identity & UINT32_MAX) == 3 && identity >> 32);
        programInfo[6] = 1ULL << 32; // Request one map ID.
        programInfo[7] = ids;
        const std::array<uint64_t, 2> fullInfo{program | (sizeof(programInfo) << 32), info};
        assert(write(info, programInfo) && write(attr, fullInfo));
        assert(call(280, {15, attr, sizeof(fullInfo)}) == 0 && load(info) == identity);
        assert(load(info + 8) && load(info + 16) == (uint64_t(sizeof(instructions)) << 32));
        assert(load(info + 48) >> 32 == 1 && load(info + 80) == (1ULL << 32));
        const std::array<uint64_t, 2> mapInfo{counter | (8ULL << 32), info};
        assert(write(attr, mapInfo) && call(280, {15, attr, sizeof(mapInfo)}) == 0);
        assert(uint32_t(load(ids)) == load(info) >> 32);
        const auto socket = call(198, {16, 2 | 0x80800, 15});
        assert(socket < 256 && machine.store64(output, program));
        assert(call(208, {socket, 1, 50, output, 4}) == uint64_t(-22));
        assert(call(57, {socket}) == 0);
        const std::array<uint8_t, 16> frame{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x86, 0xdd, 7};
        assert(write(packet, frame));
        const auto test = [&](uint32_t length, uint32_t capacity, uint32_t repeat) {
            const std::array<uint64_t, 5> request{program, uint64_t(capacity) << 32 | length, packet, output, repeat};
            assert(write(attr, request));
            return call(280, {10, attr, sizeof(request)});
        };
        assert(test(14, 14, 1) == 0 && load(attr) >> 32 == 0); // Bounds branch bypasses the counter.
        assert(test(16, 16, 2) == 0 && load(attr + 8) >> 32 == frame.size());
        assert(!std::memcmp(machine.read(output, frame.size(), 4), frame.data(), frame.size()));
        assert(test(16, 15, 1) == uint64_t(-28) && load(attr + 8) >> 32 == frame.size());
        const std::array<uint64_t, 4> lookup{counter, key, output, 0};
        assert(machine.store64(key, 0) && write(attr, lookup));
        assert(call(280, {1, attr, sizeof(lookup)}) == 0 && load(output) == 21);
        assert(call(57, {counter}) == 0 && call(57, {program}) == 0);

        const auto replaceChecksum = [&](int32_t helper, uint32_t offset, uint32_t flags) {
            const std::array<uint64_t, 7> checksumProgram{
                encode(0xb7, 0x02, 0, offset),             // r2 = checksum field offset
                encode(0xb7, 0x03, 0, 0x3412),             // old field, in network order
                encode(0xb7, 0x04, 0, 0x5678),
                encode(0xdc, 0x04, 0, 16),                 // be16 r4
                encode(0xb7, 0x05, 0, flags),
                encode(0x85, 0, 0, helper),
                encode(0x95, 0),
            };
            const std::array<uint64_t, 8> request{(uint64_t(checksumProgram.size()) << 32) | 3, code, license};
            assert(write(code, checksumProgram) && write(attr, request));
            const auto checksumFd = call(280, {5, attr, sizeof(request)});
            assert(checksumFd < 256);
            const std::array<uint64_t, 5> run{checksumFd, 16ULL << 32 | 16, packet, output, 1};
            assert(write(attr, run) && call(280, {10, attr, sizeof(run)}) == 0);
            assert(call(57, {checksumFd}) == 0);
            return uint32_t(load(attr) >> 32);
        };
        auto checksumFrame = frame;
        checksumFrame[14] = 0xed; checksumFrame[15] = 0xcb; // Wire checksum for 0x1234.
        assert(write(packet, checksumFrame) && replaceChecksum(10, 14, 2) == 0);
        const auto* transformed = machine.read(output, 16, 4);
        assert(transformed && transformed[14] == 0xa9 && transformed[15] == 0x87);
        checksumFrame[14] = 0; checksumFrame[15] = 0;
        assert(write(packet, checksumFrame) && replaceChecksum(11, 14, 0x22) == 0);
        assert((load(output + 8) >> 48) == 0); // UDP zero checksum remains disabled.
        assert(replaceChecksum(10, 15, 2) == uint32_t(-14)); // Misaligned checksum offset.
        constexpr auto address = base + 0x600;
        const std::array<uint64_t, 10> translate{
            encode(0xbf, 0x16),                         // Preserve the skb context in r6.
            encode(0xb7, 0x02, 0, 0x0008),              // ETH_P_IP in network byte order.
            encode(0xb7, 0x03, 0, 0),
            encode(0x85, 0, 0, 31),                     // skb_change_proto
            encode(0xbf, 0x61),
            encode(0xb7, 0x02, 0, -8),
            encode(0xb7, 0x03, 0, 0),                   // BPF_ADJ_ROOM_NET
            encode(0xb7, 0x04, 0, 0),
            encode(0x85, 0, 0, 50),                     // skb_adjust_room
            encode(0x95, 0),
        };
        const std::array<uint64_t, 8> loadTranslation{3 | (uint64_t(translate.size()) << 32), code, license};
        assert(write(code, translate) && write(attr, loadTranslation));
        const auto translation = call(280, {5, attr, sizeof(loadTranslation)});
        assert(translation < 256);
        std::array<uint8_t, 66> ipv6{};
        ipv6[12] = 0x86; ipv6[13] = 0xdd;
        ipv6[34] = 0x23; ipv6[53] = 0x45; // Remaining header bytes after the first resize.
        ipv6[54] = 0x56; // Removed 8-byte fragment header.
        ipv6[62] = 0x67; ipv6[65] = 0x78; // Payload.
        assert(write(packet, ipv6));
        const std::array<uint64_t, 5> invokeTranslation{translation, 128ULL << 32 | ipv6.size(), packet, address, 1};
        assert(write(attr, invokeTranslation) && call(280, {10, attr, sizeof(invokeTranslation)}) == 0);
        assert(load(attr) >> 32 == 0 && load(attr + 8) >> 32 == 38);
        auto* translated = machine.read(address, 38, 4);
        assert(translated && translated[14] == 0x23 && translated[33] == 0x45 &&
               translated[34] == 0x67 && translated[37] == 0x78);
        assert(call(57, {translation}) == 0);

    }
    {
        constexpr auto object = base + 0x100, code = base + 0x200, license = base + 0x240;
        constexpr auto debug = base + 0x260, attr = base + 0x300, info = base + 0x400, output = base + 0x500;
        const auto write = [&](uint64_t address, const auto& record) {
            return machine.write(address, {reinterpret_cast<const uint8_t*>(&record), sizeof(record)});
        };
        constexpr char names[] = "\0int\0ctx\0entry\0file.c\0line";
        const std::array<uint32_t, 21> btf{0x0001eb9f, 24, 0, 60, 60, sizeof(names),
            1, 1U << 24, 4, 0x01000020, 0, 2U << 24, 0,
            0, (13U << 24) | 1, 1, 5, 2, 9, (12U << 24) | 1, 3};
        const std::array<uint64_t, 4> btfAttr{object, 0, sizeof(btf) + sizeof(names), 0};
        assert(write(object, btf) && write(object + sizeof(btf), names) && write(attr, btfAttr));
        const auto types = call(280, {18, attr, sizeof(btfAttr)});
        assert(types < 256);
        const std::array<uint64_t, 2> instructions{0xb7, 0x95};
        assert(write(code, instructions));
        put(license, "GPL");
        const std::array<uint32_t, 2> function{0, 4};
        std::array<uint32_t, 8> lines{0, 15, 22, 1024, 0, 15, 22, 2048};
        const std::array<uint64_t, 14> request{3 | (2ULL << 32), code, license, 0, 0, 0, 0, 0, 0,
            types | (8ULL << 32), debug, 1 | (16ULL << 32), debug + 8, 2};
        assert(write(attr, request) && write(debug, function) && write(debug + 8, lines));
        assert(call(280, {5, attr, sizeof(request)}) == uint64_t(-22)); // Duplicate source-line slot.
        lines[4] = 1;
        assert(write(debug + 8, lines));
        const auto program = call(280, {5, attr, sizeof(request)});
        assert(program < 256 && call(57, {types}) == 0);
        std::array<uint64_t, 22> query{};
        query[16] = 8ULL << 32; // func_info_rec_size
        query[17] = output;
        query[18] = 1 | (2ULL << 32); // nr_func_info, nr_line_info
        query[19] = output + 8;
        query[21] = 16ULL << 32; // line_info_rec_size
        const std::array<uint64_t, 2> requestInfo{program | (sizeof(query) << 32), info};
        assert(write(info, query) && write(attr, requestInfo));
        assert(call(280, {15, attr, sizeof(requestInfo)}) == 0 && uint32_t(load(info + 128)));
        assert(load(info + 144) == query[18]);
        assert(std::memcmp(machine.read(output, sizeof(function), 4), function.data(), sizeof(function)) == 0);
        assert(std::memcmp(machine.read(output + 8, sizeof(lines), 4), lines.data(), sizeof(lines)) == 0);
        assert(call(57, {program}) == 0);
    }
    {
        constexpr auto source = base + 256, attr = base + 400, key = base + 512, value = base + 520;
        constexpr char names[] = "\0u32\0u64";
        const std::array<uint32_t, 17> input{0x0001eb9f, 24, 0, 44, 44, sizeof(names),
            1, 1U << 24, 4, 32, 5, 1U << 24, 8, 64, 0, 10U << 24, 1};
        assert(machine.write(source, {reinterpret_cast<const uint8_t*>(input.data()), sizeof(input)}));
        assert(machine.write(source + sizeof(input), {reinterpret_cast<const uint8_t*>(names), sizeof(names)}));
        assert(machine.store64(attr, source) && machine.store64(attr + 8, 0));
        const uint32_t bytes = sizeof(input) + sizeof(names);
        assert(machine.store64(attr + 16, bytes - 1) && machine.store64(attr + 24, 0));
        assert(call(280, {18, attr, 32}) == uint64_t(-22));
        assert(machine.store64(attr + 16, bytes));
        const auto btf = call(280, {18, attr, 32});
        assert(btf < 256 && call(25, {btf, 1}) == 1);
        constexpr auto info = base + 600, raw = base + 760;
        const auto objectInfo = [&](uint64_t fd, uint64_t size) {
            assert(machine.store64(attr, fd | (size << 32)) && machine.store64(attr + 8, info));
            return call(280, {15, attr, 16});
        };
        const std::array<uint8_t, 88> zero{};
        assert(machine.write(info, zero) && machine.store64(info, raw) && machine.store64(info + 8, bytes));
        assert(objectInfo(btf, 32) == 0 && (load(info + 8) & UINT32_MAX) == bytes);
        const auto btfId = load(info + 8) >> 32;
        assert(btfId && std::memcmp(machine.read(raw, bytes, 4), machine.read(source, bytes, 4), bytes) == 0);
        std::array<uint32_t, 15> map{2, 4, 8, 2};
        std::memcpy(reinterpret_cast<uint8_t*>(map.data()) + 28, "typed_array", sizeof("typed_array"));
        map[12] = btf;
        map[13] = 2;
        map[14] = 1;
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(map.data()), sizeof(map)}));
        assert(call(280, {0, attr, sizeof(map)}) == uint64_t(-22));
        map[13] = 1;
        map[14] = 2;
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(map.data()), sizeof(map)}));
        const auto fd = call(280, {0, attr, sizeof(map)});
        assert(fd < 256);
        map[0] = 24; // BPF_MAP_TYPE_SK_STORAGE
        map[3] = 0;
        map[4] = 1; // BPF_F_NO_PREALLOC
        map[13] = 3; // A const-qualified integer key.
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(map.data()), sizeof(map)}));
        const auto storage = call(280, {0, attr, sizeof(map)});
        assert(storage < 256 && call(57, {btf}) == 0);
        assert(machine.write(info, zero) && objectInfo(fd, 88) == 0 && (load(attr) >> 32) == 88);
        assert((load(info) & UINT32_MAX) == 2 && (load(info) >> 32) != 0);
        assert(load(info + 8) == ((8ULL << 32) | 4) && (load(info + 16) & UINT32_MAX) == 2);
        assert(std::memcmp(machine.read(info + 24, sizeof("typed_array"), 4), "typed_array", sizeof("typed_array")) == 0);
        assert(load(info + 64) == ((1ULL << 32) | btfId) && (load(info + 72) & UINT32_MAX) == 2);
        assert(objectInfo(fd, 8) == 0 && (load(attr) >> 32) == 8);
        assert(machine.store64(key, 0) && machine.store64(value, 99));
        assert(machine.store64(attr, fd) && machine.store64(attr + 8, key));
        assert(machine.store64(attr + 16, value) && machine.store64(attr + 24, 0));
        assert(call(280, {2, attr, 32}) == 0 && machine.store64(value, 0));
        assert(call(280, {1, attr, 32}) == 0 && load(value) == 99);
        assert(call(57, {fd}) == 0);

        assert(call(199, {1, 1, 0, info}) == 0);
        const auto sockets = load(info);
        const auto first = uint32_t(sockets), second = uint32_t(sockets >> 32);
        const auto alias = call(23, {first});
        assert(alias < 256 && machine.store64(attr, storage) && machine.store64(key, first));
        assert(call(280, {2, attr, 32}) == 0);
        assert(call(57, {first}) == 0 && machine.store64(value, 0) && machine.store64(key, alias));
        assert(call(280, {1, attr, 32}) == 0 && load(value) == 99);
        assert(machine.store64(key, second) && call(280, {1, attr, 32}) == uint64_t(-2));
        assert(machine.store64(key, alias) && call(280, {3, attr, 16}) == 0);
        assert(call(280, {1, attr, 32}) == uint64_t(-2));
        assert(call(57, {alias}) == 0 && call(57, {second}) == 0 && call(57, {storage}) == 0);
    }
    {
        constexpr uint64_t epollIn = 1, epollOut = 4, epollEdge = 0x80000000, epollReadHangup = 0x2000;
        constexpr auto input = base + 0x100, result = base + 0x140, value = base + 0x180;
        const auto fd = call(19, {0, 0x800});
        const auto alias = call(23, {fd});
        const auto firstPoller = call(20, {0}), secondPoller = call(20, {0});
        assert(fd < 256 && alias < 256 && firstPoller < 256 && secondPoller < 256);
        const auto watch = [&](uint64_t poller, uint64_t op, uint64_t mask, uint64_t tag) {
            assert(machine.store64(input, mask) && machine.store64(input + 8, tag));
            assert(call(21, {poller, op, fd, input}) == 0);
        };
        const auto wait = [&](uint64_t poller, uint64_t mask, uint64_t tag) {
            assert(call(22, {poller, result, 1, 0}) == (mask ? 1 : 0));
            if (mask) assert(load(result) == mask && load(result + 8) == tag);
        };
        const auto notify = [&](uint64_t count) {
            assert(machine.store64(value, count) && call(64, {alias, value, 8}) == 8);
        };
        watch(firstPoller, 1, epollEdge | epollIn | epollReadHangup, 17);
        watch(secondPoller, 1, epollEdge | epollIn, 23);
        wait(firstPoller, 0, 0);
        notify(1);
        notify(2);
        assert(call(22, {firstPoller, 0, 1, 0}) == uint64_t(-14));
        wait(firstPoller, epollIn, 17);
        wait(firstPoller, 0, 0);
        wait(secondPoller, epollIn, 23);
        wait(secondPoller, 0, 0);
        notify(0); // A successful eventfd write notifies even if the counter stays unchanged.
        wait(firstPoller, epollIn, 17);
        notify(4);
        wait(firstPoller, epollIn, 17);
        watch(firstPoller, 3, epollEdge | epollIn, 29);
        wait(firstPoller, epollIn, 29);
        assert(call(63, {fd, value, 8}) == 8 && load(value) == 7);
        wait(firstPoller, 0, 0);
        wait(secondPoller, 0, 0);
        watch(firstPoller, 3, epollEdge | epollOut, 31);
        wait(firstPoller, epollOut, 31);
        notify(UINT64_MAX - 1);
        wait(secondPoller, epollIn, 23);
        wait(firstPoller, 0, 0);
        assert(call(63, {fd, value, 8}) == 8 && load(value) == UINT64_MAX - 1);
        wait(firstPoller, epollOut, 31);
        wait(firstPoller, 0, 0);
        for (const auto descriptor : {alias, fd, firstPoller, secondPoller}) assert(call(57, {descriptor}) == 0);
    }

    {
        constexpr auto attr = base + 0x100, code = base + 0x200, packet = base + 0x300, license = base + 0x380;
        constexpr auto event = base + 0x400;
        const auto write = [&](uint64_t address, const auto& value) {
            return machine.write(address, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
        };
        std::array<uint32_t, 15> create{27, 0, 0, pageSize, 0};
        create[12] = UINT32_MAX; // Irrelevant BTF FD without key or value type IDs.
        assert(write(attr, create));
        const auto ring = call(280, {0, attr, sizeof(create)});
        assert(ring < 256);
        const auto reader = call(222, {0, pageSize, 3, 1, ring, 0});
        const auto writer = call(222, {0, 3 * pageSize, 1, 1, ring, pageSize});
        assert(reader < (1ULL << 37) && writer < (1ULL << 37));
        assert(call(222, {0, pageSize, 3, 1, ring, pageSize}) == uint64_t(-1));
        assert(call(226, {writer, pageSize, 3}) == uint64_t(-13));
        const auto epoll = call(20, {0});
        const std::array<uint64_t, 2> subscription{0x41, 123};
        assert(epoll < 256 && write(event, subscription) && call(21, {epoll, 1, ring, event}) == 0);
        assert(call(22, {epoll, event, 1, 0}) == 0);

        const auto encode = [](uint8_t op, uint8_t registers, int16_t offset = 0, int32_t immediate = 0) {
            return op | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        put(license, "GPL");
        const auto loadProgram = [&](uint32_t type, uint32_t length, uint32_t finish) {
            const std::array instructions{
                encode(0x18, 0x11, 0, ring), uint64_t{0},
                encode(0xb7, 0x02, 0, length), encode(0xb7, 0x03), encode(0x85, 0, 0, 131),
                encode(0x15, 0, 6), encode(0xb7, 0x01, 0, 123), encode(0x7b, 0x10, length - 8),
                encode(0xbf, 0x01), encode(0xb7, 0x02), encode(0x85, 0, 0, finish), encode(0xb7, 0), encode(0x95, 0)};
            const std::array<uint64_t, 8> request{type | (uint64_t(instructions.size()) << 32), code, license};
            assert(write(code, instructions) && write(attr, request));
            return call(280, {5, attr, sizeof(request)});
        };
        const auto small = loadProgram(1, 8, 132); // Socket filter as used by Android's platform self-test.
        const auto wrapping = loadProgram(3, 16, 132), discarded = loadProgram(3, 8, 133);
        assert(small < 256 && wrapping < 256 && discarded < 256);
        assert(write(packet, std::array<uint8_t, 64>{}));
        const auto run = [&](uint64_t program, uint64_t repeat) {
            const std::array<uint64_t, 5> request{program, 64, packet, 0, repeat};
            assert(write(attr, request) && call(280, {10, attr, sizeof(request)}) == 0);
        };
        for (int i = 0; i < 3; ++i) run(small, 64);
        run(small, 63);
        assert(load(writer) == 4080 && call(22, {epoll, event, 1, 0}) == 1);
        assert(load(event) == 0x41 && load(event + 8) == 123);
        run(small, 1);
        assert(load(writer) == 4080 && machine.store64(reader, 4080));
        assert(call(22, {epoll, event, 1, 0}) == 0);
        run(wrapping, 1);
        assert(load(writer) == 4104);
        const auto data = writer + pageSize;
        assert(uint32_t(load(data + 4080)) == 16 && load(data + 4096) == 123 && load(data) == 123);
        assert(machine.store64(reader, 4104));
        run(discarded, 1);
        assert(load(writer) == 4120 && uint32_t(load(data + 8)) == ((1U << 30) | 8));
        for (const auto fd : {ring, epoll, small, wrapping, discarded}) assert(call(57, {fd}) == 0);
        assert(call(215, {reader, pageSize}) == 0 && call(215, {writer, 3 * pageSize}) == 0);
    }
    {
        constexpr auto attr = base + 0x100, key = base + 0x200, value = key + 8;
        const auto write = [&](uint64_t address, const auto& record) {
            return machine.write(address, {reinterpret_cast<const uint8_t*>(&record), sizeof(record)});
        };
        const std::array<uint32_t, 5> request{11, 8, 4, 2, 1}; // LPM_TRIE, BPF_F_NO_PREALLOC
        assert(write(attr, request));
        const auto map = call(280, {0, attr, sizeof(request)});
        assert(map < 256);
        const auto elem = [&](uint32_t command, uint64_t address, uint64_t flags = 0) {
            const std::array<uint64_t, 4> args{map, address, command == 3 ? 0 : value, flags};
            assert(write(attr, args));
            return call(280, {command, attr, sizeof(args)});
        };
        assert(machine.store64(key, 0x0000640a00000008) && machine.store64(value, 10));
        assert(elem(2, key) == 0);
        assert(machine.store64(key, 0x0000180a0000000d) && machine.store64(value, 20));
        assert(elem(2, key) == 0);
        assert(machine.store64(key, 0x04031f0a0000000d) && machine.store64(value, 30));
        assert(elem(2, key, 1) == uint64_t(-17) && elem(2, key, 2) == 0);
        assert(elem(4, 0) == 0 && load(value) == 0x04031f0a0000000d); // Child first, original key bytes.
        assert(machine.store64(key, 0x01001e0a00000020));
        assert(elem(1, key) == 0 && uint32_t(load(value)) == 30);
        assert(elem(3, key) == uint64_t(-2)); // Deletion requires an exact prefix.
        assert(machine.store64(key, 0x0000180a0000000d) && elem(3, key) == 0);
        assert(machine.store64(key, 0x01001e0a00000020));
        assert(elem(1, key) == 0 && uint32_t(load(value)) == 10);
        assert(call(57, {map}) == 0);
    }
    {
        constexpr auto attr = base + 96;
        put(base, "/sys/fs/bpf/map_missing");
        assert(machine.store64(attr, base) && machine.store64(attr + 8, 0));
        assert(machine.store64(attr + 16, 0));
        assert(call(280, {7, attr, 24}) == uint64_t(-2));
        put(base, "/sys/fs/bpf");
        assert(call(280, {7, attr, 24}) == uint64_t(-13));
        assert(machine.store64(attr + 8, 1));
        assert(call(280, {7, attr, 24}) == uint64_t(-22));
        assert(call(280, {7, UINT64_MAX, 24}) == uint64_t(-14));
    }
    {
        constexpr auto buffer = base + 96;
        put(base, "/proc/config.gz");
        const auto config = call(56, {uint32_t(-100), base, 0});
        assert(config < 256);
        assert(call(80, {config, buffer}) == 0 && uint32_t(load(buffer + 16)) == 0100444);
        const auto size = load(buffer + 48);
        assert(size > 18 && size <= 128);
        assert(call(63, {config, buffer, 128}) == size);
        assert((load(buffer) & 0xffffff) == 0x088b1f); // gzip with deflate
        assert(call(63, {config, buffer, 128}) == 0);
        assert(call(57, {config}) == 0);
        assert(call(56, {uint32_t(-100), base, 1}) == uint64_t(-30));
        put(base, "/proc/sys/kernel/random/boot_id");
        const auto bootId = call(56, {uint32_t(-100), base, 0});
        assert(bootId < 256 && call(63, {bootId, buffer, 37}) == 37);
        const auto* uuid = machine.read(buffer, 36, 4);
        assert(uuid);
        const auto path = "/dev/ashmem" + std::string(reinterpret_cast<const char*>(uuid), 36);
        assert(call(57, {bootId}) == 0);
        put(base, path.c_str());
        assert(call(79, {uint32_t(-100), base, buffer, 0}) == 0 && load(buffer + 32) == 0x100a03);
        const auto fd = call(56, {uint32_t(-100), base, 2});
        assert(fd < 256);
        assert(call(80, {fd, buffer}) == 0 && load(buffer + 32) == 0x100a03);
        put(base, "shared-memory-regression");
        assert(call(29, {fd, 0x41007701, base}) == 0);
        assert(call(29, {fd, 0x81007702, buffer}) == 0);
        assert(std::memcmp(machine.read(buffer, 25, 4), "shared-memory-regression", 25) == 0);
        assert(call(29, {fd, 0x40087703, pageSize}) == 0);
        assert(call(29, {fd, 0x7704}) == pageSize);
        const auto writable = call(222, {0, pageSize, 3, 1, fd});
        assert(writable < (1ULL << 37));
        assert(call(29, {fd, 0x40087703, pageSize * 2}) == uint64_t(-22));
        assert(call(29, {fd, 0x40087705, 1}) == 0);
        assert(call(222, {0, pageSize, 3, 1, fd}) == uint64_t(-1));
        const auto shared = call(222, {0, pageSize, 1, 1, fd});
        assert(shared < (1ULL << 37));
        assert(machine.store64(writable, 0x1234) && load(shared) == 0x1234);
        assert(call(226, {shared, pageSize, 3}) == uint64_t(-13));
        assert(call(233, {shared, pageSize, 8}) == uint64_t(-22));
        assert(call(57, {fd}) == 0 && load(shared) == 0x1234);
        assert(call(215, {writable, pageSize}) == 0 && call(215, {shared, pageSize}) == 0);
    }
    {
        constexpr uint64_t size = 4 * 1024 * 1024, residency = base + 96;
        const auto mapping = call(222, {0, size, 3, 0x22, UINT64_MAX});
        assert(mapping < (1ULL << 37));
        assert(call(232, {mapping, pageSize, residency}) == 0 && *machine.read(residency, 1, 4) == 0);
        assert(machine.store64(mapping, 42));
        assert(call(233, {mapping, pageSize, 8}) == 0);
        assert(call(232, {mapping, pageSize, residency}) == 0 && *machine.read(residency, 1, 4) == 1);
        assert(call(215, {mapping, size}) == 0);
        assert(call(232, {mapping, pageSize, residency}) == uint64_t(-12));
    }
    {
        ProbeLinux guest(1, 0);
        guest.addBootTmpfs("/metadata", 0755, 0, 6);
        guest.addBootTmpfs("/scratch", 0777, 0, 6);
        constexpr std::array<uint8_t, 2> defaults{'o', 'k'};
        guest.mount("/system/etc/example", defaults);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](std::string_view value) {
            const std::string text(value);
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(text.c_str()), text.size() + 1}));
        };
        constexpr uint64_t cwd = uint32_t(-100), scratch = base + 256;
        path("/metadata/maps");
        assert(invoke(34, {cwd, base, 0775}) == 0);
        assert(invoke(54, {cwd, base, 0, 1000, 0}) == 0);
        assert(invoke(53, {cwd, base, 0775, 0}) == 0); // Restore group write removed by umask.
        path("/metadata/private");
        assert(invoke(34, {cwd, base, 0700}) == 0);
        assert(task.store64(scratch, 1000));
        assert(invoke(159, {1, scratch}) == 0);
        assert(invoke(149, {2000, 2000, 2000}) == 0 && invoke(147, {2000, 2000, 2000}) == 0);
        path("/metadata/denied");
        assert(invoke(56, {cwd, base, 0x41, 0600}) == uint64_t(-13));
        path("/metadata/private/denied");
        assert(invoke(56, {cwd, base, 0x41, 0600}) == uint64_t(-13));
        path("/metadata/maps/test");
        const auto file = invoke(56, {cwd, base, 0x42, 0600});
        path("/system/etc/example");
        const auto input = invoke(56, {cwd, base, 0});
        assert(input < 256 && file < 256);
        assert(invoke(285, {input, 0, file, 0, 2}) == uint64_t(-18));
        assert(invoke(285, {file, 0, file, 0, 2}) == uint64_t(-95));
        assert(invoke(285, {input, 0xdead, file, 0, 2}) == uint64_t(-14));
        assert(invoke(285, {input, 0, input, 0, 2}) == uint64_t(-9));
        assert(invoke(285, {input, 0, file, 0, 2, 1}) == uint64_t(-22));
        uint64_t contents = 0;
        assert(task.store64(scratch + 8, 1));
        assert(invoke(71, {file, input, 0xdead, 2}) == uint64_t(-14));
        assert(invoke(71, {file, input, scratch + 8, 2}) == 1);
        assert(invoke(62, {input, 0, 1}) == 0);
        assert(task.load64(scratch + 8, contents) && contents == 2);
        assert(invoke(71, {file, input, 0, 2}) == 2);
        assert(invoke(71, {file, input, 0, 2}) == 0);
        assert(invoke(67, {file, scratch + 8, 3, 0}) == 3);
        assert(std::memcmp(task.read(scratch + 8, 3, 4), "kok", 3) == 0);
        assert(invoke(62, {file, 0, 0}) == 0);
        assert(invoke(64, {file, scratch, 8}) == 8 && invoke(57, {file}) == 0);
        path("/metadata/maps/test");
        const auto reopened = invoke(56, {cwd, base, 0});
        assert(reopened < 256 && invoke(63, {reopened, scratch, 8}) == 8);
        assert(task.load64(scratch, contents) && contents == 1000);
        constexpr char across[] = "/scratch/test";
        constexpr char moved[] = "/metadata/maps/moved";
        assert(task.write(scratch, {reinterpret_cast<const uint8_t*>(across), sizeof(across)}));
        assert(invoke(38, {cwd, base, cwd, scratch}) == uint64_t(-18));
        assert(task.write(scratch, {reinterpret_cast<const uint8_t*>(moved), sizeof(moved)}));
        assert(invoke(38, {cwd, base, cwd, scratch}) == 0);
        assert(invoke(56, {cwd, base, 0}) == uint64_t(-2));
        path(moved);
        assert(invoke(35, {cwd, base, 0}) == 0);
    }
    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto clone = [&](uint64_t flags, uint64_t stack) {
            task.cpu.x[0] = flags;
            task.cpu.x[1] = stack;
            task.cpu.x[2] = task.cpu.x[4] = base;
            task.cpu.x[3] = 0;
            task.cpu.x[8] = 220;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        for (uint64_t batch = 0; batch < 2; ++batch) {
            const auto child = clone(0x1200011, 0);
            assert(child >= 2 && child < 4096);
            assert(guest.schedule(task) && guest.processId() == child);
            for (uint64_t created = 1; created <= 1022; ++created)
                assert(clone(0x3d0f00, base + 256) == child + created);
            assert(clone(0x3d0f00, base + 256) == uint64_t(-11));
            assert(clone(0x1200011, 0) == uint64_t(-11));
            task.cpu.x[0] = 0;
            task.cpu.x[8] = 94;
            assert(guest.syscall(task) && guest.blocked());
            assert(guest.schedule(task) && guest.processId() == 1);
            const auto report = guest.workerReport(task, 1);
            assert(report.find("1 active, 1 tracked, limit 1024") != std::string::npos);
        }
    }
    {
        ProbeLinux source(1, 0), reader(2, 1000);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        source.addDirectory("/dev/__properties__", 0711);
        source.addDirectory("/dev/__properties__/private", 0700);
        const auto call = [&](ProbeLinux& guest, uint64_t syscall, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = syscall;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto open = [&](ProbeLinux& guest, std::string_view filename, uint64_t flags) {
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(filename.data()), filename.size()}));
            const uint8_t terminator = 0;
            assert(task.write(base + filename.size(), {&terminator, 1}));
            return call(guest, 56, {uint32_t(-100), base, flags, 0444});
        };
        constexpr std::string_view propertyInfo = "/dev/__properties__/property_info";
        constexpr std::string_view privateInfo = "/dev/__properties__/private/property_info";
        for (const auto file : {propertyInfo, privateInfo}) {
            const auto fd = open(source, file, 0x41);
            assert(fd < 256 && task.store64(base + 0x100, 37));
            assert(call(source, 64, {fd, base + 0x100, 8}) == 8);
        }
        assert(source.exportFiles(reader, "/dev/__properties__/") == 2);
        const auto readable = open(reader, propertyInfo, 0x88000);
        assert(readable < 256);
        uint64_t value = 0;
        assert(call(reader, 63, {readable, base + 0x180, 8}) == 8);
        assert(task.load64(base + 0x180, value) && value == 37);
        assert(open(reader, privateInfo, 0x88000) == uint64_t(-13));
    }
    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const std::array<uint8_t, 2 * pageSize> code{};
        constexpr char codePath[] = "/system/boot-trace-probe.so";
        guest.mount(codePath, code);
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(codePath), sizeof(codePath)}));
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto fd = invoke(56, {uint32_t(-100), base, 0});
        assert(fd < 256);
        const auto mapping = invoke(222, {0, pageSize, 5, 2, fd, pageSize});
        assert(mapping >= 0x100000000 && mapping < 0x2000000000);
        task.cpu.pc = mapping + 0x10;
        task.cpu.x[30] = mapping + 0x20;
        task.cpu.sp = base + 0x80;
        task.cpu.x[29] = base + 0x100;
        assert(task.store64(base + 0x100, base + 0x110) && task.store64(base + 0x108, mapping + 0x34));
        assert(task.store64(base + 0x110, base + 0x100) && task.store64(base + 0x118, mapping + 0x44));
        const auto report = guest.workerReport(task, 1);
        assert(report.find("pc-file=/system/boot-trace-probe.so+0x1030") != std::string::npos);
        assert(report.find("pc-file=/system/boot-trace-probe.so+0x1040") != std::string::npos);
        assert(report.find("caller 2 PC") == std::string::npos);
        assert(report.find("pc-file=/system/boot-trace-probe.so+0x1010") != std::string::npos);
        assert(report.find("lr-file=/system/boot-trace-probe.so+0x1020") != std::string::npos);

        const auto worker = invoke(220, {0x3d0f00, base + 0x800, base + 0x88, 0, base + 0x90});
        assert(worker == 2 && guest.schedule(task) && guest.threadId() == worker);
        task.cpu.pc = mapping + 0x50;
        task.cpu.x[30] = mapping + 0x60;
        task.cpu.x[29] = 0;
        task.cpu.stop = Stop::unsupported;
        const auto stopped = guest.workerReport(task, 1);
        const auto line = stopped.find("guest task 1/2 ");
        assert(line != std::string::npos);
        const auto workerReport = std::string_view(stopped).substr(line, stopped.find('\n', line) - line);
        assert(workerReport.find("pc-file=/system/boot-trace-probe.so+0x1050") != std::string::npos);
        assert(workerReport.find("lr-file=/system/boot-trace-probe.so+0x1060") != std::string::npos);
    }
    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto call = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto open = [&](const std::string& path, uint64_t flags = 0) {
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(path.c_str()), path.size() + 1}));
            return call(56, {uint32_t(-100), base, flags});
        };
        const auto thread = call(220, {0x3d0f00, base + 0x800, base + 0x88, 0, base + 0x90});
        assert(thread > 1);
        const auto directory = open("/proc/1/task", 0x4000);
        assert(directory < 256);
        const auto size = call(61, {directory, base + 256, 256});
        assert(size > 0 && size <= 256);
        const auto* bytes = task.read(base + 256, size, 4);
        assert(bytes);
        std::set<std::string> names;
        for (size_t offset = 0; offset < size;) {
            uint16_t recordSize;
            std::memcpy(&recordSize, bytes + offset + 16, sizeof(recordSize));
            assert(recordSize >= 20 && recordSize <= size - offset);
            names.emplace(reinterpret_cast<const char*>(bytes + offset + 19));
            offset += recordSize;
        }
        assert(names.contains("1") && names.contains(std::to_string(thread)));
        const auto threadPath = "/proc/1/task/" + std::to_string(thread);
        const auto status = open(threadPath + "/status");
        assert(status < 256 && open("/proc/1/cmdline") < 256);
        const auto length = call(63, {status, base + 256, 256});
        assert(length > 0 && length <= 256);
        const std::string_view text(reinterpret_cast<const char*>(task.read(base + 256, length, 4)), length);
        assert(text.find("Tgid:\t1\n") != text.npos && text.find("Pid:\t" + std::to_string(thread) + "\n") != text.npos);
        assert(text.find("\nTracerPid:\t0\n") != text.npos);
        assert(open("/proc/99999/task", 0x4000) == uint64_t(-2));
    }
    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        assert(task.store64(base, 42) && task.store64(base + 16, 3600));
        task.cpu.x[0] = base;
        task.cpu.x[1] = 128; // FUTEX_WAIT_PRIVATE.
        task.cpu.x[2] = 42;
        task.cpu.x[3] = base + 16;
        task.cpu.x[8] = 98;
        assert(guest.syscall(task));
        const auto report = guest.workerReport(task, 1);
        assert(report.find("op=128 expected=42 deadline-ms=") != std::string::npos);
    }
    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        std::string report;
        guest.androidLogObserver = [&](std::string_view message) { report = message; };
        const auto dumpable = [&](uint64_t operation, uint64_t flag = 0) {
            task.cpu.x[0] = operation;
            task.cpu.x[1] = flag;
            task.cpu.x[8] = 167;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        assert(dumpable(3) == 1);
        assert(dumpable(4, 0) == 0 && dumpable(3) == 0);
        assert(dumpable(4, 2) == uint64_t(-22) && dumpable(3) == 0);
        const auto abort = [&] { assert(dumpable(4, 1) == 0 && dumpable(3) == 1); };
        task.cpu.x.fill(0);
        task.cpu.x[1] = 96 * 1024 * 1024;
        task.cpu.x[2] = 3;
        task.cpu.x[3] = 0x22;
        task.cpu.x[4] = UINT32_MAX;
        task.cpu.x[8] = 222; // mmap
        assert(guest.syscall(task));
        const auto abortMapping = task.cpu.x[0];
        assert(abortMapping < uint64_t(-4095));
        abort();
        assert(report.empty());
        constexpr std::string_view crash = "input initialization\nfailed";
        const std::array<uint64_t, 3> header{0xb18e40886ac388f0ULL, 0xc6dfba755a1de0b5ULL,
                                             3 * sizeof(uint64_t) + crash.size() + 1};
        assert(task.write(abortMapping, {reinterpret_cast<const uint8_t*>(header.data()), sizeof(header)}));
        assert(task.write(abortMapping + sizeof(header),
                          {reinterpret_cast<const uint8_t*>(crash.data()), crash.size()}));
        abort();
        assert(report.starts_with("Guest native abort: PID=1 TID=1 "));
        assert(report.ends_with("input initialization failed"));
        assert(dumpable(0x59616d61, UINT64_MAX) == uint64_t(-22));
        task.cpu.x[0] = 0x01210900;
        task.cpu.x[8] = 220; // debuggerd's clone without CLONE_FILES
        assert(guest.syscall(task) && task.cpu.x[0] == uint64_t(-38));

        task.cpu.x[0] = 1000;
        task.cpu.x[8] = 146; // setuid
        assert(guest.syscall(task) && task.cpu.x[0] == 0 && dumpable(3) == 0);
    }
    for (const auto [wait, restart] : {std::pair{98, 0ULL}, std::pair{98, 0x10000000ULL},
                                      std::pair{137, 0x10000000ULL}, std::pair{133, 0x10000000ULL},
                                      std::pair{115, 0x10000000ULL}}) {
        ProbeLinux guest(1, 0);
        constexpr auto handler = base + 0x200, blockedPc = base + 0x300;
        constexpr auto word = base + 0x400, action = base + 0x440, parentTid = base + 0x480;
        constexpr auto signal = 33;
        Machine task(Image{base, {{base, std::vector<uint8_t>(8 * pageSize), 7}}, {}});
        task.cpu.sp = base + 4 * pageSize;
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        assert(task.store64(action, handler) && task.store64(action + 8, restart | 4));
        assert(invoke(134, {signal, action, 0, 8}) == 0);
        const auto thread = invoke(220, {0x3d0f00, base + 8 * pageSize, parentTid, 0, parentTid + 8});
        assert(thread == 2 && guest.schedule(task) && guest.threadId() == thread);
        task.cpu.pc = blockedPc;
        task.cpu.fpsr = fpsrSaturation;
        if (wait == 133) {
            constexpr uint64_t blockedSignal = 1ULL << (signal - 1);
            assert(task.store64(word, blockedSignal));
            assert(invoke(135, {0, word, 0, 8}) == 0);
            assert(task.store64(word, 0));
            assert(invoke(133, {word, 4}) == uint64_t(-22));
            assert(invoke(133, {1, 8}) == uint64_t(-14));
            assert(invoke(133, {word, 8}) == word);
        } else if (wait == 137) {
            // An unrelated handler interrupts rt_sigtimedwait even with SA_RESTART.
            assert(task.store64(word, 1ULL << (12 - 1)));
            assert(invoke(137, {word, 0, 0, 8}) == word);
        } else if (wait == 115) {
            assert(task.store64(word, 5) && task.store64(word + 8, 0));
            assert(invoke(115, {1, 0, word, word + 16}) == 1);
        } else {
            assert(invoke(98, {word, 0x80, 0}) == word);
        }
        assert(guest.blocked());
        assert(guest.schedule(task) && guest.threadId() == 1);
        assert(invoke(131, {1, thread, signal}) == 0 && guest.schedule(task));
        assert(guest.threadId() == thread && task.cpu.pc == handler && task.cpu.x[0] == signal);
        uint64_t signalInfo = 0;
        assert(task.load64(task.cpu.x[1] + 8, signalInfo) && uint32_t(signalInfo) == uint32_t(-6));
        task.cpu.fpsr = 0;
        invoke(139);
        assert(guest.restoredSignalContext() && !guest.blocked());
        assert(task.cpu.fpsr == fpsrSaturation);
        const bool restarts = wait == 98 && restart;
        assert(task.cpu.pc == blockedPc + (restarts ? 0 : 4));
        assert(task.cpu.x[0] == (restarts ? word : uint64_t(-4)));
        if (wait == 115) {
            uint64_t seconds = 0, nanos = 0;
            assert(task.load64(word + 16, seconds) && task.load64(word + 24, nanos));
            assert(seconds > 0 && seconds < 5 && nanos < 1'000'000'000);
        }
        if (wait == 133) {
            assert(invoke(135, {0, 0, word, 8}) == 0);
            uint64_t mask = 0;
            assert(task.load64(word, mask) && mask == (1ULL << (signal - 1)));
        }
    }
    for (const bool exitsBeforeSuspend : {false, true}) {
        ProbeLinux guest(1, 0);
        constexpr auto handler = base + 0x200, blockedPc = base + 0x300;
        constexpr auto word = base + 0x400, action = base + 0x440, childTid = base + 0x480;
        constexpr uint64_t signal = 17, signalMask = 1ULL << (signal - 1);
        Machine task(Image{base, {{base, std::vector<uint8_t>(8 * pageSize), 7}}, {}});
        task.cpu.sp = base + 4 * pageSize;
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        assert(task.store64(action, handler) && task.store64(action + 8, 0x10000004));
        assert(invoke(134, {signal, action, 0, 8}) == 0);
        assert(task.store64(word, signalMask) && invoke(135, {0, word, 0, 8}) == 0);
        assert(task.store64(word, 0));
        const auto child = invoke(220, {0x1200011, 0, 0, 0, childTid});
        assert(child == 2);
        if (!exitsBeforeSuspend) {
            task.cpu.pc = blockedPc;
            assert(invoke(133, {word, 8}) == word && guest.blocked());
        }
        assert(guest.schedule(task) && guest.processId() == child);
        invoke(94, {42});
        assert(guest.schedule(task) && guest.processId() == 1);
        if (exitsBeforeSuspend) {
            task.cpu.pc = blockedPc;
            assert(invoke(133, {word, 8}) == word && guest.blocked());
            assert(guest.schedule(task));
        }
        assert(task.cpu.pc == handler && task.cpu.x[0] == signal);
        uint64_t info = 0;
        assert(task.load64(task.cpu.x[1] + 8, info) && uint32_t(info) == 1);
        assert(task.load64(task.cpu.x[1] + 16, info) && uint32_t(info) == child);
        assert(task.load64(task.cpu.x[1] + 24, info) && uint32_t(info) == 42);
        invoke(139);
        assert(task.cpu.pc == blockedPc + 4 && task.cpu.x[0] == uint64_t(-4));
        assert(invoke(135, {0, 0, word, 8}) == 0 && task.load64(word, info) && info == signalMask);
        constexpr uint64_t childUsage = base + 0x580, allUsage = base + 0x640;
        assert(invoke(165, {uint32_t(-1), allUsage}) == 0);
        assert(task.load64(allUsage + 8, info) && info == 0);
        assert(invoke(260, {child, word, 0, 1}) == uint64_t(-14));
        assert(invoke(260, {child, word, 0xb, childUsage}) == child);
        assert(task.load64(word, info) && uint32_t(info) == 42U << 8);
        assert(invoke(165, {uint32_t(-1), allUsage}) == 0);
        assert(std::memcmp(task.read(childUsage, 144, 4), task.read(allUsage, 144, 4), 144) == 0);
        assert(invoke(165, {0, allUsage}) == 0);
        assert(invoke(165, {1, allUsage}) == 0 && task.load64(allUsage + 8, info) && info < 1'000'000);
        assert(invoke(165, {2, allUsage}) == uint64_t(-22));
        assert(invoke(165, {0, 1}) == uint64_t(-14));
    }
    {
        ProbeLinux guest(1, 0);
        constexpr auto handler = base + 0x200, stopped = base + 0x300, action = base + 0x440;
        constexpr uint64_t forkTid = base + 0x480, signal = 33;
        constexpr uint64_t unsupported = 65535, argument = 0x1234;
        Machine task(Image{base, {{base, std::vector<uint8_t>(8 * pageSize), 7}}, {}});
        task.cpu.sp = base + 4 * pageSize;
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        assert(task.store64(action, handler) && task.store64(action + 8, 4));
        assert(invoke(134, {signal, action, 0, 8}) == 0);
        const auto child = invoke(220, {0x1200011, 0, 0, 0, forkTid});
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        task.cpu.pc = stopped;
        task.cpu.x[0] = argument;
        task.cpu.x[8] = unsupported;
        assert(!guest.syscall(task) && guest.suspendChild());
        assert(guest.schedule(task) && guest.processId() == 1);
        assert(invoke(131, {child, child, signal}) == 0);
        assert(guest.schedule(task) && guest.processId() == child);
        assert(task.cpu.pc == handler && task.cpu.x[0] == signal);
        uint64_t signalInfo = 0;
        assert(task.load64(task.cpu.x[1] + 8, signalInfo) && uint32_t(signalInfo) == uint32_t(-6));
        invoke(139);
        assert(guest.restoredSignalContext() && task.cpu.pc == stopped);
        assert(task.cpu.x[8] == unsupported && task.cpu.x[0] == argument);
        assert(!guest.syscall(task) && guest.suspendChild());
        assert(guest.schedule(task) && guest.processId() == 1);
    }
    {
        ProbeLinux guest(1, 0);
        guest.addBootCgroups();
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](std::string_view text) {
            const std::string value(text);
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(value.c_str()), value.size() + 1}));
        };
        constexpr uint64_t cwd = uint32_t(-100), output = base + 256;
        path("/sys/fs/cgroup/system");
        assert(invoke(43, {base, output}) == 0);
        uint64_t magic = 0;
        assert(task.load64(output, magic) && magic == 0x63677270);
        path("/sys/fs/cgroup/system/uid_1000");
        assert(invoke(34, {cwd, base, 0775}) == 0);
        path("/sys/fs/cgroup/system/uid_1000/cgroup.procs");
        const auto procs = invoke(56, {cwd, base, 2});
        assert(procs < 256);
        path("0\n");
        assert(invoke(64, {procs, base, 2}) == 2);
        path("/proc/self/cgroup");
        const auto proc = invoke(56, {cwd, base, 0});
        constexpr std::string_view expected = "0::/system/uid_1000\n";
        assert(invoke(63, {proc, output, 128}) == expected.size());
        assert(std::memcmp(task.read(output, expected.size(), 4), expected.data(), expected.size()) == 0);
        path("/sys/fs/cgroup");
        const auto root = invoke(56, {cwd, base, 0x4000});
        assert(root < 256);
        constexpr auto license = base + 384, code = base + 400, attr = base + 440;
        const auto write = [&](uint64_t address, const auto& value) {
            return task.write(address, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
        };
        std::array<uint64_t, 9> load{9 | (2ULL << 32), code, license};
        load[8] = 2ULL << 32; // BPF_CGROUP_INET_SOCK_CREATE
        assert(write(license, "GPL") && write(code, std::array<uint64_t, 2>{0xb7, 0x95}) && write(attr, load));
        const auto deny = invoke(280, {5, attr, sizeof(load)});
        assert(deny < 256);
        const std::array<uint32_t, 4> attach{uint32_t(root), uint32_t(deny), 2, 0};
        assert(write(attr, attach) && invoke(280, {8, attr, sizeof(attach)}) == 0);
        assert(invoke(57, {deny}) == 0); // The cgroup owns a reference beyond the program FD.
        assert(invoke(198, {2, 1, 0}) == uint64_t(-1)); // Attached parent applies inside the child.
        assert(invoke(280, {9, attr, sizeof(attach)}) == 0);
        const auto stream = invoke(198, {2, 1, 0});
        assert(stream < 256);
        std::array<uint8_t, 40> ifreq{'l', 'o'};
        assert(write(attr, ifreq) && invoke(29, {stream, 0x8913, attr}) == 0);
        uint64_t field = 0;
        assert(task.load64(attr + 16, field) && field == 0x49);
        assert(invoke(29, {stream, 0x8933, attr}) == 0);
        assert(task.load64(attr + 16, field) && field == 1);
        assert(write(attr, uint64_t{0}) && invoke(29, {stream, 0x8910, attr}) == 0);
        constexpr std::array<char, 16> loopbackName{'l', 'o'};
        assert(std::memcmp(task.read(attr, loopbackName.size(), 4), loopbackName.data(), loopbackName.size()) == 0);
        assert(write(attr + 16, uint32_t{0}) && invoke(29, {stream, 0x8910, attr}) == uint64_t(-19));
        assert(invoke(29, {stream, 0x8910, attr + pageSize}) == uint64_t(-14));
        assert(invoke(29, {stream, 0x8921, attr}) == 0);
        assert(task.load64(attr + 16, field) && field == 65536);
        assert(write(attr + 16, uint16_t(8)) && invoke(29, {stream, 0x8914, attr}) == 0);
        assert(invoke(29, {stream, 0x8913, attr}) == 0 && task.load64(attr + 16, field) && uint16_t(field) == 8);
        const std::array<uint8_t, 16> withoutRoute{2, 0, 0x80, 0x80, 127, 0, 0, 1};
        assert(write(output, withoutRoute) && invoke(203, {stream, output, withoutRoute.size()}) == uint64_t(-100));
        assert(write(attr + 16, uint16_t(9)) && invoke(29, {stream, 0x8914, attr}) == 0);
        assert(invoke(203, {stream, output, withoutRoute.size()}) == uint64_t(-111));
        ifreq[0] = 'x';
        assert(write(attr, ifreq) && invoke(29, {stream, 0x8913, attr}) == uint64_t(-19));
        assert(invoke(29, {stream, 0x8913, attr + pageSize}) == uint64_t(-14));
        const std::array<uint32_t, 5> createTags{1, 8, 8, 2, 0};
        assert(write(attr, createTags));
        const auto tags = invoke(280, {0, attr, sizeof(createTags)});
        assert(tags < 256);
        // A release hook is attached after the socket was opened.
        const std::array<uint64_t, 9> releaseCode{
            0x2e00000085, 0xfff80a7b, 0xa2bf, 0xfffffff800000207,
            (tags << 32) | 0x1118, 0, 0x300000085, 0x1000000b7, 0x95};
        load[0] = 9 | (uint64_t(releaseCode.size()) << 32);
        load[8] = 34ULL << 32; // BPF_CGROUP_INET_SOCK_RELEASE
        const auto releaseEntry = base + 544;
        load[1] = releaseEntry;
        assert(write(releaseEntry, releaseCode) && write(attr, load));
        const auto release = invoke(280, {5, attr, sizeof(load)});
        assert(release < 256);
        const std::array<uint32_t, 4> releaseAttach{uint32_t(root), uint32_t(release), 34, 0};
        assert(write(attr, releaseAttach) && invoke(280, {8, attr, sizeof(releaseAttach)}) == 0);
        const auto cookie = output + 8, length = output + 16, value = output + 24;
        assert(write(length, uint32_t(8)) && invoke(209, {stream, 1, 57, cookie, length}) == 0);
        const std::array<uint64_t, 4> tag{tags, cookie, value, 0};
        assert(write(value, uint64_t(99)) && write(attr, tag) && invoke(280, {2, attr, sizeof(tag)}) == 0);
        const auto alias = invoke(23, {stream});
        assert(alias < 256 && invoke(57, {stream}) == 0);
        assert(invoke(280, {1, attr, sizeof(tag)}) == 0);
        assert(invoke(57, {alias}) == 0 && invoke(280, {1, attr, sizeof(tag)}) == uint64_t(-2));

        const auto first = invoke(198, {2, 1, 0}), second = invoke(198, {2, 1, 0});
        assert(first < 256 && second < 256);
        const std::array<uint8_t, 16> loopback{2, 0, 0, 0, 127, 0, 0, 1};
        assert(write(output, loopback) && invoke(200, {first, output, loopback.size()}) == 0);
        const auto addrSize = output + 32;
        assert(write(addrSize, uint32_t(loopback.size())) && invoke(204, {first, output, addrSize}) == 0);
        const auto* assigned = task.read(output, loopback.size(), 4);
        assert(assigned && assigned[2] >= 0x80 && assigned[4] == 127 && assigned[7] == 1);
        assert(invoke(200, {second, output, loopback.size()}) == uint64_t(-98));
        assert(invoke(57, {first}) == 0 && invoke(200, {second, output, loopback.size()}) == 0);
        assert(invoke(57, {second}) == 0);

        // Socket-created cgroup context drives address-family packet hooks.
        const auto program = [&](uint32_t direction, uint32_t verdict) {
            const std::array<uint64_t, 2> packetCode{0xb7 | (uint64_t(verdict) << 32), 0x95};
            load[0] = 8 | (uint64_t(packetCode.size()) << 32); // BPF_PROG_TYPE_CGROUP_SKB
            load[8] = uint64_t(direction) << 32;
            assert(write(releaseEntry, packetCode) && write(attr, load));
            const auto id = invoke(280, {5, attr, sizeof(load)});
            assert(id < 256);
            return id;
        };
        const auto attachPacket = [&](uint64_t command, uint32_t direction, uint64_t id, uint32_t flags = 0) {
            const std::array<uint32_t, 4> request{uint32_t(root), uint32_t(id), direction, flags};
            assert(write(attr, request));
            return invoke(280, {command, attr, sizeof(request)});
        };
        const auto blockedIn = program(0, 0), blockedOut = program(1, 0), allowedOut = program(1, 3);
        assert(attachPacket(8, 0, allowedOut) == uint64_t(-22));
        assert(attachPacket(8, 1, blockedIn) == 0 && attachPacket(9, 1, blockedIn) == 0);
        for (uint32_t family : {2, 10}) {
            const auto receiver = invoke(198, {family, 0x802, 0}), sender = invoke(198, {family, 0x802, 0});
            assert(receiver < 256 && sender < 256);
            const auto socketAddress = base + 640, payload = base + 680, from = base + 704;
            const auto fromLength = base + 736;
            std::array<uint8_t, 28> address{};
            address[0] = family;
            address[family == 2 ? 4 : 23] = family == 2 ? 127 : 1;
            if (family == 2) address[7] = 1;
            const uint32_t addressSize = family == 2 ? 16 : 28;
            assert(write(socketAddress, address) && write(fromLength, addressSize));
            assert(invoke(200, {receiver, socketAddress, addressSize}) == 0);
            assert(invoke(204, {receiver, socketAddress, fromLength}) == 0);
            constexpr uint64_t data = 0x1020304050607080ULL;
            assert(write(payload, data));
            const auto send = [&] { return invoke(206, {sender, payload, 8, 0, socketAddress, addressSize}); };
            const auto recv = [&] { return invoke(207, {receiver, payload, 8, 0, from, fromLength}); };
            assert(attachPacket(8, 1, blockedOut) == 0);
            assert(send() == uint64_t(-1) && recv() == uint64_t(-11));
            assert(attachPacket(9, 1, blockedOut) == 0);
            assert(attachPacket(8, 1, allowedOut) == 0 && attachPacket(8, 0, blockedIn) == 0);
            assert(send() == 8 && recv() == uint64_t(-11));
            assert(attachPacket(9, 0, blockedIn) == 0);
            assert(send() == 8 && write(payload, uint64_t(0)));
            assert(invoke(207, {receiver, payload, 4, 0x22, 0, 0}) == 8); // Peek and report full length.
            assert(recv() == 8 && recv() == uint64_t(-11));
            uint64_t observed = 0;
            assert(task.load64(payload, observed) && observed == data);
            const auto* origin = task.read(from, addressSize, 4);
            assert(origin && origin[0] == family && origin[family == 2 ? 7 : 23] == 1 &&
                   (origin[2] || origin[3]));
            assert(invoke(203, {sender, socketAddress, addressSize}) == 0);
            assert(invoke(205, {sender, from, fromLength}) == 0);
            assert(invoke(64, {sender, payload, 8}) == 8 && recv() == 8);
            assert(attachPacket(9, 1, allowedOut) == 0);

            const auto addressProgram = [&](uint32_t direction, uint32_t verdict, std::optional<uint16_t> port) {
                std::vector<uint64_t> instructions;
                if (port) {
                    instructions.push_back(0x02b7 | (uint64_t(*port) << 32)); // r2 = new network-order port
                    instructions.push_back(0x182163); // *(u32 *)(r1 + 24) = r2
                }
                instructions.push_back(0xb7 | (uint64_t(verdict) << 32));
                instructions.push_back(0x95);
                load[0] = 18 | (uint64_t(instructions.size()) << 32); // BPF_PROG_TYPE_CGROUP_SOCK_ADDR
                load[8] = uint64_t(direction) << 32;
                assert(task.write(releaseEntry, {reinterpret_cast<const uint8_t*>(instructions.data()),
                                                instructions.size() * sizeof(instructions.front())}));
                assert(write(attr, load));
                const auto id = invoke(280, {5, attr, sizeof(load)});
                assert(id < 256);
                return id;
            };
            const uint32_t ip = family == 2 ? 0 : 1;
            const auto denyConnect = addressProgram(10 + ip, 0, {});
            assert(attachPacket(8, 10 + ip, denyConnect) == 0);
            assert(invoke(203, {sender, socketAddress, addressSize}) == uint64_t(-1));
            assert(attachPacket(9, 10 + ip, denyConnect) == 0);
            const auto* destination = task.read(socketAddress, addressSize, 4);
            assert(destination);
            uint16_t correctPort = 0;
            std::memcpy(&correctPort, destination + 2, 2);
            std::memcpy(address.data(), destination, addressSize);
            address[2] = 0x10;
            address[3] = 0x20;
            assert(write(socketAddress, address));
            const auto redirectConnect = addressProgram(10 + ip, 1, correctPort);
            assert(attachPacket(8, 10 + ip, redirectConnect) == 0);
            assert(invoke(203, {sender, socketAddress, addressSize}) == 0);
            assert(invoke(205, {sender, from, fromLength}) == 0);
            const auto* connected = task.read(from, addressSize, 4);
            assert(connected && !std::memcmp(connected + 2, &correctPort, 2));
            assert(attachPacket(9, 10 + ip, redirectConnect) == 0);
            const auto redirectSend = addressProgram(14 + ip, 1, correctPort);
            const auto rewriteFrom = addressProgram(19 + ip, 1, 0x3412);
            assert(attachPacket(8, 14 + ip, redirectSend) == 0 && attachPacket(8, 19 + ip, rewriteFrom) == 0);
            assert(send() == 8 && recv() == 8);
            const auto* rewritten = task.read(from, addressSize, 4);
            assert(rewritten && rewritten[2] == 0x12 && rewritten[3] == 0x34);
            assert(attachPacket(9, 14 + ip, redirectSend) == 0 && attachPacket(9, 19 + ip, rewriteFrom) == 0);
            for (const auto id : {denyConnect, redirectConnect, redirectSend, rewriteFrom}) assert(invoke(57, {id}) == 0);
            assert(invoke(57, {sender}) == 0 && invoke(57, {receiver}) == 0);
        }
        assert(invoke(57, {blockedIn}) == 0 && invoke(57, {blockedOut}) == 0 &&
               invoke(57, {allowedOut}) == 0);

        const auto insn = [](uint8_t op, uint8_t registers = 0, int16_t offset = 0, int32_t immediate = 0) {
            return uint64_t(op) | (uint64_t(registers) << 8) | (uint64_t(uint16_t(offset)) << 16) |
                   (uint64_t(uint32_t(immediate)) << 32);
        };
        const auto loadTcpProgram = [&](uint32_t type, uint32_t direction, const auto& instructions) {
            load[0] = type | (uint64_t(instructions.size()) << 32);
            load[8] = uint64_t(direction) << 32;
            assert(write(releaseEntry, instructions) && write(attr, load));
            const auto id = invoke(280, {5, attr, sizeof(load)});
            assert(id < 256);
            return id;
        };
        const std::array sockOps{
            insn(0xbf, 0x16), insn(0x61, 0x64), insn(0x15, 4, 5, 14), insn(0x15, 4, 9, 15),
            insn(0xb7, 2, 0, 0x40), insn(0x85, 0, 0, 59), insn(0xb7, 0, 0, 1), insn(0x95),
            insn(0xb7, 2, 0, 4), insn(0xb7, 3), insn(0x85, 0, 0, 144), insn(0xb7, 0, 0, 1), insn(0x95),
            insn(0xb7, 2, 0, 0x0101041e), insn(0x63, 0x2a, -4), insn(0xbf, 0xa2), insn(0x07, 2, 0, -4),
            insn(0xb7, 3, 0, 4), insn(0xb7, 4), insn(0x85, 0, 0, 143), insn(0xb7, 0, 0, 1), insn(0x95)};
        const auto operations = loadTcpProgram(13, 3, sockOps);
        assert(attachPacket(8, 3, operations, 2) == 0);
        const auto additional = loadTcpProgram(13, 3, sockOps);
        assert(attachPacket(8, 3, additional, 2) == 0);
        constexpr auto ids = base + 1040, flags = base + 1060;
        const auto idOf = [&](uint64_t programFd) {
            const std::array<uint64_t, 2> request{programFd | (8ULL << 32), ids};
            assert(write(attr, request) && invoke(280, {15, attr, sizeof(request)}) == 0);
            uint64_t identity = 0;
            assert(task.load64(ids, identity));
            return uint32_t(identity >> 32);
        };
        const auto firstId = idOf(operations), secondId = idOf(additional);
        path("/sys/fs/cgroup/system/uid_1000");
        const auto childGroup = invoke(56, {cwd, base, 0x4000});
        assert(childGroup < 256 && firstId && secondId && firstId != secondId);
        const auto query = [&](uint64_t group, uint64_t mode, uint64_t capacity, uint64_t programFlags,
                               size_t attributeSize = 64) {
            const std::array<uint64_t, 8> request{group | (3ULL << 32), mode, ids, capacity, programFlags};
            assert(write(attr, request));
            return invoke(280, {16, attr, attributeSize});
        };
        uint64_t value64 = 0;
        assert(query(root, 0, 1, flags) == uint64_t(-28));
        assert(task.load64(attr + 24, value64) && value64 == 2);
        assert(task.load64(attr + 56, value64) && value64 == 0);
        assert(task.load64(ids, value64) && uint32_t(value64) == firstId);
        assert(task.load64(flags, value64) && uint32_t(value64) == 2);
        assert(query(childGroup, 0, 0, 0, 32) == 0);
        assert(task.load64(attr + 24, value64) && value64 == 0);
        assert(query(childGroup, 1, 2, flags) == uint64_t(-22));
        assert(query(childGroup, 1, 2, 0) == 0);
        assert(task.load64(ids, value64) && uint32_t(value64) == firstId && uint32_t(value64 >> 32) == secondId);
        assert(task.load64(attr + 56, value64) && value64 == 0);
        assert(attachPacket(9, 3, additional) == 0 && invoke(57, {additional}) == 0);
        assert(query(childGroup, 1, 1, 0) == 0);
        assert(task.load64(attr + 24, value64) && value64 == 1 && invoke(57, {childGroup}) == 0);
        for (uint32_t family : {2, 10}) {
            const uint32_t addressSize = family == 2 ? 16 : 28;
            const uint32_t ipSize = family == 2 ? 20 : 40;
            // Require the option in real data segments, irrespective of direction.
            const std::array inspect{
                insn(0x61, 0x12, 76), insn(0xb7, 0, 0, 1), insn(0x71, 0x23, ipSize + 13),
                insn(0x55, 3, 4, 0x18), insn(0xb7, 0), insn(0x71, 0x23, ipSize + 20),
                insn(0x55, 3, 1, 30), insn(0xb7, 0, 0, 1), insn(0x95)};
            const auto verifyOptions = loadTcpProgram(8, 1, inspect);
            assert(attachPacket(8, 1, verifyOptions) == 0);
            const auto listener = invoke(198, {family, 0x801, 0}), client = invoke(198, {family, 0x801, 0});
            assert(listener < 256 && client < 256);
            const auto address = base + 800, buffer = base + 840, fromLength = base + 880;
            std::array<uint8_t, 28> sockaddr{};
            sockaddr[0] = family;
            assert(write(address, sockaddr) && write(fromLength, addressSize));
            assert(invoke(200, {listener, address, addressSize}) == 0 && invoke(201, {listener, 4}) == 0);
            assert(invoke(204, {listener, address, fromLength}) == 0);
            assert(invoke(203, {client, address, addressSize}) == 0);
            const auto server = invoke(242, {listener, address, fromLength, 0x800});
            assert(server < 256);
            const auto* named = task.read(address, addressSize, 4);
            assert(named && named[0] == family && named[family == 2 ? 7 : 23] == 1);
            for (const auto& [writer, reader] : {std::pair{client, server}, std::pair{server, client}}) {
                constexpr uint64_t word = 0xf1e2d3c4b5a69788ULL;
                assert(write(buffer, word) && invoke(64, {writer, buffer, 8}) == 8);
                assert(write(buffer, uint64_t{0}) && invoke(63, {reader, buffer, 8}) == 8);
                uint64_t observed = 0;
                assert(task.load64(buffer, observed) && observed == word);
            }
            assert(invoke(210, {client, 1}) == 0 && invoke(63, {server, buffer, 8}) == 0);
            assert(invoke(57, {server}) == 0 && invoke(57, {client}) == 0);

            const auto abortClient = invoke(198, {family, 0x801, 0});
            assert(abortClient < 256 && write(buffer, uint64_t{1}) && invoke(208, {abortClient, 1, 13, buffer, 8}) == 0);
            assert(write(buffer, 1 | (uint64_t{4} << 32)) && invoke(208, {listener, 1, 13, buffer, 8}) == 0);
            assert(write(fromLength, addressSize) && invoke(204, {listener, address, fromLength}) == 0);
            assert(invoke(203, {abortClient, address, addressSize}) == 0);
            const auto abortServer = invoke(242, {listener, 0, 0, 0x800});
            assert(abortServer < 256 && write(fromLength, uint32_t{8}));
            assert(invoke(209, {abortServer, 1, 13, buffer, fromLength}) == 0);
            assert(task.load64(buffer, value64) && value64 == (1 | (uint64_t{4} << 32)));
            assert(write(buffer, uint64_t{0x1234}) && invoke(64, {abortClient, buffer, 8}) == 8);
            assert(invoke(57, {abortClient}) == 0);
            assert(invoke(63, {abortServer, buffer, 8}) == 8 && task.load64(buffer, value64) && value64 == 0x1234);
            assert(invoke(63, {abortServer, buffer, 8}) == uint64_t(-104));
            assert(invoke(57, {abortServer}) == 0 && invoke(57, {listener}) == 0);
            assert(attachPacket(9, 1, verifyOptions) == 0 && invoke(57, {verifyOptions}) == 0);
        }
        for (uint32_t type : {1, 2}) {
            constexpr auto name = base + 800, remote = base + 832, length = base + 866, buffer = base + 880;
            std::array<uint8_t, 28> dualAddress{10};
            const auto dual = invoke(198, {10, type | 0x800, 0}), ipv4 = invoke(198, {2, type | 0x800, 0});
            assert(dual < 256 && ipv4 < 256);
            assert(write(length, uint32_t(4)) && invoke(209, {dual, 41, 26, buffer, length}) == 0);
            uint64_t value = 0;
            assert(task.load64(buffer, value) && uint32_t(value) == 0); // Default listener is dual stack.
            assert(write(name, dualAddress) && invoke(200, {dual, name, dualAddress.size()}) == 0);
            assert(write(length, uint32_t(dualAddress.size())) && invoke(204, {dual, name, length}) == 0);
            const auto* bound = task.read(name, dualAddress.size(), 4);
            assert(bound);
            const std::array<uint8_t, 16> fromV4{2, 0, bound[2], bound[3], 127, 0, 0, 1};
            assert(write(remote, fromV4));
            assert(invoke(200, {ipv4, remote, fromV4.size()}) == uint64_t(-98));
            if (type == 1) assert(invoke(201, {dual, 4}) == 0);
            assert(invoke(203, {ipv4, remote, fromV4.size()}) == 0);
            const auto server = type == 1 ? invoke(242, {dual, name, length, 0x800}) : dual;
            assert(server < 256 && write(buffer, uint64_t(0xabcd)));
            assert(invoke(64, {ipv4, buffer, 8}) == 8);
            if (type == 1) assert(invoke(63, {server, buffer, 8}) == 8);
            else assert(invoke(207, {server, buffer, 8, 0, name, length}) == 8);
            const auto* source = task.read(name, dualAddress.size(), 4);
            assert(source && source[0] == 10 && source[18] == 0xff && source[19] == 0xff &&
                   source[20] == 127 && source[23] == 1);
            if (type == 1) assert(invoke(64, {server, buffer, 8}) == 8 && invoke(63, {ipv4, buffer, 8}) == 8);
            if (type == 1) assert(invoke(57, {server}) == 0);
            assert(invoke(57, {ipv4}) == 0 && invoke(57, {dual}) == 0);
        }
        {
            const auto v6 = invoke(198, {10, 1, 0}), v4 = invoke(198, {2, 1, 0});
            constexpr auto name = base + 996, value = base + 1024, length = base + 1032;
            assert(v6 < 256 && v4 < 256 && write(value, uint32_t(1)));
            assert(invoke(208, {v6, 41, 26, value, 4}) == 0);
            const std::array<uint8_t, 28> localAddress{10};
            assert(write(name, localAddress) && invoke(200, {v6, name, localAddress.size()}) == 0);
            assert(write(length, uint32_t(localAddress.size())) && invoke(204, {v6, name, length}) == 0);
            const auto* host = task.read(name, localAddress.size(), 4);
            assert(host);
            const std::array<uint8_t, 16> v4Address{2, 0, host[2], host[3], 127, 0, 0, 1};
            assert(write(name, v4Address));
            assert(invoke(201, {v6, 4}) == 0 && invoke(203, {v4, name, v4Address.size()}) == uint64_t(-111));
            assert(invoke(200, {v4, name, v4Address.size()}) == 0);
            assert(invoke(208, {v6, 41, 26, value, 4}) == uint64_t(-22));
            assert(invoke(57, {v6}) == 0 && invoke(57, {v4}) == 0);
        }
        assert(attachPacket(9, 3, operations) == 0);
        assert(invoke(57, {operations}) == 0 && invoke(57, {root}) == 0);
        assert(invoke(220, {0x1200011, 0, 0, 0, base + 128}) == 2);
        assert(invoke(63, {procs, output, 128}) == 4);
        assert(std::memcmp(task.read(output, 4, 4), "1\n2\n", 4) == 0);
        path("/sys/fs/cgroup/system/uid_1000");
        assert(invoke(35, {cwd, base, 0x200}) == uint64_t(-16));
    }
    {
        ProbeLinux guest(1, 1000);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        // Deny getuid only on AArch64. Allow other calls, including fork.
        const std::array<uint32_t, 12> filter{
            0x20, 4, 0x03000015, 0xc00000b7, 0x20, 0,
            0x01000015, 174, 0x06, 0x00050001, 0x06, 0x7fff0000,
        };
        constexpr auto instructions = base + 64;
        assert(task.store64(base, filter.size() / 2));
        assert(task.store64(base + 8, instructions));
        assert(task.write(instructions, {reinterpret_cast<const uint8_t*>(filter.data()), sizeof(filter)}));
        assert(invoke(167, {22, 2, base}) == uint64_t(-13));
        assert(invoke(167, {38, 1}) == 0 && invoke(167, {39}) == 1);
        assert(invoke(167, {22, 2, base + 4096}) == uint64_t(-14));
        assert(task.store64(instructions, filter.size() * (uint64_t{1} << 32) + 0x05));
        assert(invoke(167, {22, 2, base}) == uint64_t(-22));
        assert(task.write(instructions, {reinterpret_cast<const uint8_t*>(filter.data()), sizeof(filter)}));
        assert(invoke(167, {22, 2, base}) == 0 && invoke(167, {21}) == 2);
        assert(task.store64(instructions + 32, 0x7fff000000000006)); // Installed code is a snapshot.
        assert(invoke(174) == uint64_t(-1) && invoke(172) == 1);
        assert(invoke(220, {0x1200011, 0, 0, 0, base + 128}) == 2);
        assert(guest.schedule(task) && guest.threadId() == 2);
        assert(invoke(167, {39}) == 1 && invoke(174) == uint64_t(-1));
    }
    {
        ProbeLinux guest(1, 1000);
        guest.setBootContext("u:r:zygote:s0");
        assert(guest.loadCurrentContextPolicy("u:r:zygote:s0 u:r:system_server_startup:s0\n"));
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr char current[] = "/proc/thread-self/attr/current";
        constexpr char validator[] = "/sys/fs/selinux/context";
        constexpr char denied[] = "u:r:init:s0";
        constexpr char allowed[] = "u:r:system_server_startup:s0";
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(current), sizeof(current)}));
        const auto attribute = invoke(56, {uint32_t(-100), base, 2});
        assert(attribute < 256);
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(validator), sizeof(validator)}));
        const auto validate = invoke(56, {uint32_t(-100), base, 2});
        assert(validate < 256);
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(denied), sizeof(denied)}));
        assert(invoke(64, {validate, base, sizeof(denied)}) == uint64_t(-22));
        assert(invoke(64, {attribute, base, sizeof(denied)}) == uint64_t(-13));
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(allowed), sizeof(allowed)}));
        assert(invoke(64, {validate, base, sizeof(allowed)}) == sizeof(allowed));
        assert(invoke(64, {attribute, base, sizeof(allowed)}) == sizeof(allowed));
        assert(invoke(63, {attribute, base, sizeof(allowed)}) == sizeof(allowed));
        assert(std::memcmp(task.read(base, sizeof(allowed), 4), allowed, sizeof(allowed)) == 0);
    }
    {
        constexpr auto alt = base + 2 * pageSize;
        const std::array<uint32_t, 2> instructions{0xb9400022, 0xd65f03c0}; // ldr w2,[x1]; ret
        std::vector<uint8_t> code(sizeof(instructions));
        std::memcpy(code.data(), instructions.data(), code.size());
        Machine task(Image{base, {{base, std::move(code), 5}, {base + pageSize, std::vector<uint8_t>(3 * pageSize), 6}}, {}});
        ProbeLinux guest(1, 1000);
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr auto parameters = base + pageSize;
        const std::array<uint64_t, 4> action{base + 4, 0x08000004, 0, 1ULL << (12 - 1)};
        assert(task.write(parameters, {reinterpret_cast<const uint8_t*>(action.data()), sizeof(action)}));
        assert(invoke(134, {11, parameters, 0, 8}) == 0);
        const std::array<uint64_t, 3> stack{alt, 0, 2 * pageSize};
        assert(task.write(parameters, {reinterpret_cast<const uint8_t*>(stack.data()), sizeof(stack)}));
        assert(invoke(132, {parameters}) == 0);
        task.cpu.sp = alt - 16;
        task.cpu.nzcv = 9;
        task.cpu.v[4][7] = 168;
        task.cpu.x[17] = 0x1234;
        task.cpu.x[2] = 0x5678;
        task.cpu.x[1] = 0;
        const auto before = task.cpu;
        assert(executeStartupInstruction(task, instructions[0]));
        assert(task.cpu.stop == Stop::fault && guest.deliverMemoryFault(task));
        assert(task.cpu.pc == base + 4 && task.cpu.x[0] == 11);
        assert(task.cpu.sp >= alt && task.cpu.sp < alt + stack[2] && !(task.cpu.sp & 15));
        assert(task.cpu.x[1] == task.cpu.sp && task.cpu.x[2] == task.cpu.sp + 128);
        uint64_t value = 0;
        assert(task.load64(task.cpu.x[1] + 16, value) && value == 0); // si_addr
        assert(task.load64(task.cpu.x[1] + 8, value) && value == 1); // SEGV_MAPERR
        assert(task.load64(task.cpu.x[2] + 176 + 264, value) && value == base);
        assert(task.store64(task.cpu.x[2] + 176 + 264, base + 4)); // Change resume PC in ucontext.
        const auto trampoline = task.cpu.x[30];
        assert(executeStartupInstruction(task, instructions[1]));
        assert(task.cpu.pc == trampoline);
        uint32_t mov;
        const auto* entry = task.read(trampoline, sizeof(mov), 1);
        assert(entry != nullptr);
        std::memcpy(&mov, entry, sizeof(mov));
        const auto decoded = decode(mov);
        if (decoded.op == Op::unsupported) assert(executeStartupInstruction(task, mov));
        else executeDecoded(task, decoded);
        assert(task.cpu.x[8] == 139 && guest.syscall(task) && guest.restoredSignalContext());
        assert(task.cpu.pc == base + 4 && task.cpu.sp == before.sp && task.cpu.nzcv == before.nzcv);
        assert(task.cpu.x == before.x && task.cpu.v == before.v);
        assert(invoke(135, {0, 0, parameters, 8}) == 0);
        assert(task.load64(parameters, value) && value == 0);
    }
    assert(call(143, {0, 9999}) == 0 && call(176) == 0 && call(177) == 9999);
    assert(call(145, {UINT32_MAX, 9999}) == 0 && call(174) == 0 && call(175) == 9999);
    assert(call(145, {123, UINT32_MAX}) == uint64_t(-1));
    assert(call(143, {UINT32_MAX, 123}) == uint64_t(-1));
    assert(call(143, {UINT32_MAX, 0}) == 0 && call(177) == 0);
    assert(call(145, {UINT32_MAX, 0}) == 0 && call(175) == 0);
    assert(call(143, {0, 0}) == 0 && call(145, {0, 0}) == 0);
    {
        ProbeLinux guarded(1, 0);
        Machine guardedMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), guardedMachine.cpu.x.begin());
            guardedMachine.cpu.x[8] = number;
            assert(guarded.syscall(guardedMachine));
            return guardedMachine.cpu.x[0];
        };
        const auto area = invoke(222, {0, pageSize, 1, 0x22, uint64_t(-1), 0});
        assert(area < (uint64_t{1} << 45));
        assert(invoke(167, {56}) == 0 && invoke(167, {55, 2}) == uint64_t(-22));
        assert(invoke(167, {55, 3}) == 0 && invoke(167, {56}) == 3);
        assert(invoke(167, {55, 0}) == uint64_t(-1));
        assert(invoke(226, {area, pageSize, 5}) == uint64_t(-13));
        assert(invoke(222, {0, pageSize, 7, 0x22, uint64_t(-1), 0}) == uint64_t(-13));
        assert(invoke(220, {0x1200011, 0, 0, 0, base}) == 2);
        assert(guarded.schedule(guardedMachine) && guarded.processId() == 2);
        assert(invoke(167, {56}) == 0 && invoke(226, {area, pageSize, 5}) == 0);
    }
    assert(machine.store64(base + 0x680, 0) && machine.store64(base + 0x688, 0));
    assert(machine.store64(base + 0x690, 0));
    assert(call(73, {base + 0x690, 1, base + 0x680}) == 0);
    assert(machine.store64(base + 0x690, 1ULL << 32));
    assert(call(73, {base + 0x690, 1, base + 0x680}) == 1);
    assert(load(base + 0x690) >> 48 == 1);
    assert(machine.store64(base + 512, 120));
    assert(call(170, {0, base + 512}) == 0);
    assert(call(169, {0, base + 520}) == 0 && load(base + 520) == 120);
    assert(machine.store64(base + 512, 901));
    assert(call(170, {0, base + 512}) == uint64_t(-22));
    assert(call(169, {0, base + 520}) == 0 && load(base + 520) == 120);

    assert(call(30, {1, 0, 0x4002}) == 0);
    assert(call(31, {1, 1}) == 0x4002);
    assert(call(30, {1, 1, 0x4008}) == uint64_t(-22));
    assert(call(31, {1, 0}) == 0x4002);

    put(base, "tmpfs");
    put(base + 16, "/test");
    put(base + 40, "mode=0750,uid=0,gid=0");
    put(base + 72, "/test/sample");
    assert(call(56, {uint32_t(-100), base + 72, 0x42, 0600}) == uint64_t(-30));
    assert(call(40, {base, base + 16, base, 14, base + 40}) == 0);
    assert(call(43, {base + 16, base + 512}) == 0);
    assert(load(base + 512 + 80) == 0x2e);
    put(base + 390, ".");
    assert(call(49, {base + 16}) == 0);
    assert(call(43, {base + 390, base + 512}) == 0 && load(base + 512) == 0x01021994);
    put(base + 390, "");
    assert(call(43, {base + 390, base + 512}) == uint64_t(-2));
    put(base + 390, "/");
    assert(call(49, {base + 390}) == 0);
    auto file = call(56, {uint32_t(-100), base + 72, 0x42, 0600});
    assert(file == 3);
    put(base + 340, "disk");
    assert(call(68, {file, base + 340, 4, 4}) == 4);
    assert(call(62, {file, 0, 1}) == 0);
    assert(call(67, {file, base + 512, 8, 0}) == 8 && (load(base + 512) & UINT32_MAX) == 0);
    assert(std::memcmp(machine.read(base + 516, 4, 4), "disk", 4) == 0);
    assert(call(68, {file, base + 340, 4, UINT64_MAX}) == uint64_t(-22));
    assert(machine.store64(base + 520, 0x40000000));
    constexpr uint64_t getInodeFlags = 0x80086601, setInodeFlags = 0x40086602;
    assert(call(29, {file, getInodeFlags, base + 520}) == 0 && load(base + 520) == 0);
    assert(call(29, {file, setInodeFlags, base + 520}) == uint64_t(-95));
    assert(call(29, {file, getInodeFlags, 0}) == uint64_t(-14));
    const auto pathOnlyFile = call(56, {uint32_t(-100), base + 72, 0x200000});
    assert(pathOnlyFile < 256 && call(29, {pathOnlyFile, getInodeFlags, base + 520}) == uint64_t(-9));
    assert(call(57, {pathOnlyFile}) == 0);
    assert(call(59, {base + 512, 0}) == 0);
    const auto pipeFlags = load(base + 512);
    assert(call(29, {uint32_t(pipeFlags), getInodeFlags, base + 520}) == uint64_t(-25));
    assert(call(29, {uint32_t(pipeFlags), setInodeFlags, base + 520}) == uint64_t(-25));
    assert(call(57, {uint32_t(pipeFlags)}) == 0 && call(57, {pipeFlags >> 32}) == 0);
    assert(call(29, {file, 0x40806685, base + 520}) == uint64_t(-25)); // FS_IOC_ENABLE_VERITY
    assert(call(29, {file, 0xc0046686, base + 520}) == uint64_t(-25)); // FS_IOC_MEASURE_VERITY
    assert(call(29, {file, 0xc0286687, base + 520}) == uint64_t(-25)); // FS_IOC_READ_VERITY_METADATA
    assert(load(base + 520) == 0);
    assert(call(233, {base, pageSize, UINT32_MAX}) == uint64_t(-22));
    assert(call(57, {file}) == 0);
    assert(call(39, {base + 16}) == uint64_t(-16));

    {
        constexpr auto pair = base + 0x300, data = base + 0x320, vector = base + 0x340;
        constexpr auto message = base + 0x360, control = base + 0x3a0;
        assert(call(199, {1, 2, 0, pair}) == 0); // AF_UNIX, SOCK_DGRAM
        const auto sender = uint32_t(load(pair)), receiver = uint32_t(load(pair) >> 32);
        assert(machine.store64(data, 'a'));
        assert(call(208, {receiver, 1, 16, data, 4}) == 0); // SO_PASSCRED
        assert(call(206, {sender, data, 1, 0}) == 1);
        assert(machine.store64(vector, data) && machine.store64(vector + 8, 1));
        const auto receive = [&](uint64_t capacity, uint64_t flags) {
            const std::array<uint64_t, 7> header{0, 0, vector, 1, control, capacity, 0};
            assert(machine.write(message, {reinterpret_cast<const uint8_t*>(header.data()), sizeof(header)}));
            return call(212, {receiver, message, flags});
        };
        assert(receive(20, 0x42) == 1 && load(message + 48) == 8); // PEEK and CTRUNC.
        assert(load(control) == 20);
        assert(receive(32, 0x40) == 1 && load(message + 48) == 0);
        assert(load(control) == 28 && load(control + 8) == 0x200000001);
        assert(load(control + 16) == 1 && uint32_t(load(control + 24)) == 0);
        assert(receive(32, 0x40) == uint64_t(-11));
        assert(call(57, {sender}) == 0 && call(57, {receiver}) == 0);
        assert(call(425) == uint64_t(-38)); // Linux reports an unavailable io_uring subsystem.
    }

    {
        constexpr auto address = base + 0x300, data = base + 0x320, vector = base + 0x340;
        constexpr auto message = base + 0x360, control = base + 0x3a0, optionLength = base + 0x3c0;
        constexpr char socketAddress[] = "\x01\0/dev/nestvane-passcred";
        assert(machine.write(address, {reinterpret_cast<const uint8_t*>(socketAddress), sizeof(socketAddress)}));
        const auto listener = call(198, {1, 5, 0});
        const auto sender = call(198, {1, 5, 0});
        assert(listener < 256 && sender < 256);
        assert(machine.store64(data, 1) && call(208, {listener, 1, 16, data, 4}) == 0);
        assert(call(200, {listener, address, sizeof(socketAddress)}) == 0);
        assert(call(201, {listener, 4}) == 0);
        assert(call(203, {sender, address, sizeof(socketAddress)}) == 0);
        assert(machine.store64(data, 'k'));
        assert(machine.store64(vector, data) && machine.store64(vector + 8, 1));
        const std::array<uint64_t, 7> outbound{0, 0, vector, 1, control, 16, 0};
        assert(machine.write(message, {reinterpret_cast<const uint8_t*>(outbound.data()), sizeof(outbound)}));
        assert(machine.store64(control, 16) && machine.store64(control + 8, 0x100000001));
        assert(call(211, {sender, message, 0x4000}) == 1); // Android libbase's empty SCM_RIGHTS.
        const auto receiver = call(202, {listener, 0, 0});
        assert(receiver < 256);
        assert(machine.store64(optionLength, 4));
        assert(call(209, {receiver, 1, 16, data, optionLength}) == 0 && load(data) == 1);
        assert(machine.store64(vector, data) && machine.store64(vector + 8, 1));
        const std::array<uint64_t, 7> header{0, 0, vector, 1, control, 32, 0};
        assert(machine.write(message, {reinterpret_cast<const uint8_t*>(header.data()), sizeof(header)}));
        assert(call(212, {receiver, message, 0x40004028}) == 1 && load(data) == 'k');
        assert(load(message + 40) == 32 && load(message + 48) == 0x40000000);
        assert(load(control) == 28 && load(control + 8) == 0x200000001);
        assert(load(control + 16) == 1 && uint32_t(load(control + 24)) == 0);
        assert(machine.write(message, {reinterpret_cast<const uint8_t*>(outbound.data()), sizeof(outbound)}));
        assert(machine.store64(control, 12) && machine.store64(control + 8, 0x100000001));
        assert(call(211, {sender, message, 0x4000}) == uint64_t(-22)); // A malformed header still fails.
        for (const auto descriptor : {receiver, sender, listener}) assert(call(57, {descriptor}) == 0);
    }

    {
        constexpr auto pair = base + 0x300, pipe = base + 0x310, data = base + 0x320, vector = base + 0x340;
        constexpr auto message = base + 0x360, control = base + 0x3a0;
        constexpr uint64_t closeOnExec = 0x40000000;
        assert(call(199, {1, 5, 0, pair}) == 0); // AF_UNIX, SOCK_SEQPACKET.
        assert(call(59, {pipe, 0}) == 0);
        const auto sender = uint32_t(load(pair)), receiver = uint32_t(load(pair) >> 32);
        const auto readEnd = uint32_t(load(pipe)), writeEnd = uint32_t(load(pipe) >> 32);
        assert(machine.store64(data, 'k'));
        assert(machine.store64(vector, data) && machine.store64(vector + 8, 1));
        const auto header = [&](uint64_t buffer, uint64_t capacity) {
            const std::array<uint64_t, 7> bytes{0, 0, vector, 1, buffer, capacity, 0};
            assert(machine.write(message, {reinterpret_cast<const uint8_t*>(bytes.data()), sizeof(bytes)}));
        };
        header(control, 24);
        assert(machine.store64(control, 20) && machine.store64(control + 8, 0x100000001));
        assert(machine.store64(control + 16, writeEnd));
        assert(call(211, {sender, message, 0}) == 1);
        assert(call(57, {writeEnd}) == 0); // The queued packet retains the pipe.
        header(control, 0);
        assert(call(212, {receiver, message, 2}) == 1); // PEEK without space for rights.
        assert(load(message + 40) == 0 && load(message + 48) == 8);
        header(UINT64_MAX, 24);
        assert(call(212, {receiver, message, 0}) == uint64_t(-14)); // A bad control buffer keeps the packet.
        header(control, 24);
        assert(call(212, {receiver, message, closeOnExec}) == 1);
        assert(load(message + 40) == 24 && load(message + 48) == closeOnExec);
        assert(load(control) == 20 && load(control + 8) == 0x100000001);
        const auto received = uint32_t(load(control + 16));
        assert(call(25, {received, 1}) == 1); // F_GETFD.
        assert(call(64, {received, data, 1}) == 1 && call(63, {readEnd, data, 1}) == 1);
        assert(load(data) == 'k');
        assert(call(57, {sender}) == 0);
        header(control, 24);
        assert(call(212, {receiver, message, closeOnExec}) == 0);
        assert(load(message + 40) == 0 && load(message + 48) == closeOnExec);
        for (const auto descriptor : {received, readEnd, receiver}) assert(call(57, {descriptor}) == 0);
    }

    {
        constexpr auto pair = base + 0x300, incoming = base + 0x320, outgoing = base + 0x340;
        constexpr uint64_t truncate = 0x20, nonblock = 0x40;
        assert(machine.store64(outgoing, 0x07060504030201ULL));
        for (uint64_t type : {2, 5, 1}) { // SOCK_DGRAM, SOCK_SEQPACKET, SOCK_STREAM
            assert(call(199, {1, type, 0, pair}) == 0);
            const auto sender = uint32_t(load(pair)), receiver = uint32_t(load(pair) >> 32);
            assert(call(206, {sender, outgoing, 7, 0}) == 7);
            assert(call(207, {receiver, UINT64_MAX, 3, truncate}) == uint64_t(-14));
            assert(machine.store64(incoming, 0));
            assert(call(207, {receiver, incoming, 3, truncate}) == (type == 1 ? 3 : 7));
            assert(load(incoming) == 0x030201);
            if (type == 1) assert(call(207, {receiver, incoming, 4, nonblock}) == 4);
            assert(call(207, {receiver, incoming, 3, truncate | nonblock}) == uint64_t(-11));
            assert(call(206, {sender, outgoing, 4, 0}) == 4);
            assert(call(207, {receiver, 0, 0, truncate}) == (type == 1 ? 0 : 4));
            if (type == 1) assert(call(207, {receiver, incoming, 4, nonblock}) == 4);
            assert(call(207, {receiver, incoming, 3, nonblock}) == uint64_t(-11));
            assert(call(57, {sender}) == 0 && call(57, {receiver}) == 0);
        }
    }

    {
        linux.captureAndroidLogs = true;
        unsigned snapshots = 0;
        linux.watchdogObserver = [&](std::string_view report) {
            assert(report.find("guest task 1/1 ") != std::string_view::npos);
            ++snapshots;
        };
        const auto logSocket = call(198, {1, 2, 0}); // AF_UNIX SOCK_DGRAM
        assert(logSocket < 256);
        constexpr char address[] = "\x01\0/dev/socket/logdw";
        constexpr char tag[] = "Watchdog";
        constexpr char message[] = "*** WATCHDOG KILLING SYSTEM PROCESS: test";
        const auto write = [&](uint64_t destination, const auto& data) {
            assert(machine.write(destination, {reinterpret_cast<const uint8_t*>(&data), sizeof(data)}));
        };
        write(base + 256, address);
        assert(call(203, {logSocket, base + 256, sizeof(address)}) == 0);
        write(base + 288, tag);
        write(base + 320, message);
        const std::array<std::array<uint64_t, 2>, 4> vectors{{
            {base, 1}, {base, 1}, {base + 288, sizeof(tag)}, {base + 320, sizeof(message)},
        }};
        write(base + 384, vectors);
        assert(call(66, {logSocket, base + 384, vectors.size()}) == 2 + sizeof(tag) + sizeof(message));
        assert(linux.watchdogReport.find("guest task 1/1 ") != std::string::npos);
        write(base + 384, vectors);
        assert(call(66, {logSocket, base + 384, vectors.size()}) == 2 + sizeof(tag) + sizeof(message));
        assert(snapshots == 1);
        linux.watchdogObserver = {};
        assert(call(57, {logSocket}) == 0);
        linux.captureAndroidLogs = false;
    }
    {
        constexpr uint64_t in = 1, hangup = 16, readHangup = 0x2000, edge = 0x80000000;
        constexpr auto registration = base + 0x100, output = base + 0x140, data = base + 0x180, addr = base + 0x200;
        constexpr char address[] = "\x01\0/dev/nestvane-epoll-unix";
        assert(machine.write(addr, {reinterpret_cast<const uint8_t*>(address), sizeof address}));
        const auto listener = call(198, {1, 0x801, 0});
        const auto first = call(198, {1, 0x801, 0}), second = call(198, {1, 0x801, 0});
        const auto poller = call(20, {0});
        assert(listener < 256 && first < 256 && second < 256 && poller < 256);
        assert(call(200, {listener, addr, sizeof address}) == 0 && call(201, {listener, 4}) == 0);
        assert(machine.store64(registration, edge | in | readHangup));
        const auto watch = [&](uint64_t fd, uint64_t tag) {
            assert(machine.store64(registration + 8, tag) && call(21, {poller, 1, fd, registration}) == 0);
        };
        const auto wait = [&](uint64_t flags, uint64_t tag) {
            assert(call(22, {poller, output, 1, 0}) == (flags ? 1 : 0));
            if (flags) assert(load(output) == flags && load(output + 8) == tag);
        };
        watch(listener, 15);
        wait(0, 0);
        assert(call(203, {first, addr, sizeof address}) == 0);
        wait(in, 15);
        wait(0, 0); // The pending connection remains queued.
        assert(call(203, {second, addr, sizeof address}) == 0);
        wait(in, 15);
        const auto accepted = call(242, {listener, 0, 0, 0x800});
        const auto other = call(242, {listener, 0, 0, 0x800});
        assert(accepted < 256 && other < 256);
        watch(accepted, 19);
        wait(0, 0);
        assert(machine.store64(data, 'a') && call(64, {first, data, 1}) == 1);
        wait(in, 19);
        wait(0, 0); // The unread byte is insufficient to generate another notification.
        assert(machine.store64(data, 'b') && call(64, {first, data, 1}) == 1);
        wait(in, 19);
        assert(machine.store64(data, 0) && call(63, {accepted, data, 2}) == 2 && load(data) == 0x6261);
        assert(call(210, {first, 1}) == 0);
        wait(in | readHangup, 19);
        wait(0, 0);
        assert(call(63, {accepted, data, 8}) == 0);
        assert(call(57, {first}) == 0);
        wait(in | hangup | readHangup, 19);
        wait(0, 0);
        for (const auto descriptor : {poller, listener, accepted, other, second}) assert(call(57, {descriptor}) == 0);
    }
    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t page = 0x2000, watch = page + 0x80, events = page + 0x100;
        constexpr uint64_t payload = page + 0x200, timerSpec = page + 0x280;
        Machine task(Image{page, {{page, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto poller = invoke(20, {0});
        const auto event = invoke(19, {0, 0x800});
        const auto otherEvent = invoke(19, {0, 0x800});
        const auto timer = invoke(85, {1, 0x800});
        const auto otherTimer = invoke(85, {1, 0x800});
        assert(poller < 256 && event < 256 && otherEvent < 256 && timer < 256 && otherTimer < 256);
        assert(task.store64(watch, 1) && task.store64(watch + 8, 0xa1));
        assert(invoke(21, {poller, 1, event, watch}) == 0);
        assert(task.store64(watch + 8, 0xa2));
        assert(invoke(21, {poller, 1, timer, watch}) == 0);
        assert(invoke(199, {1, 1, 0, payload + 8}) == 0);
        assert(invoke(199, {1, 1, 0, payload + 16}) == 0);
        uint64_t watchedSockets = 0, otherSockets = 0;
        assert(task.load64(payload + 8, watchedSockets) && task.load64(payload + 16, otherSockets));
        assert(task.store64(watch + 8, 0xa4));
        assert(invoke(21, {poller, 1, watchedSockets >> 32, watch}) == 0);
        assert(task.store64(payload, 1) && task.store64(timerSpec + 24, 1));
        const auto writer = invoke(220, {0x3d0f00, page + 0x900, page + 0x300, page + 0x800, page + 0x300});
        assert(writer > 1);
        const auto block = [&] {
            invoke(22, {poller, events, 1, UINT64_MAX});
            assert(guest.blocked() && guest.schedule(task) && guest.threadId() == writer);
        };
        const auto waiting = [&] {
            return guest.workerReport(task, 1).find("  guest task 1/1   poll syscall=22") != std::string::npos;
        };
        const auto wake = [&](uint64_t data) {
            assert(!waiting());
            task.cpu.retired += 4096;
            assert(guest.schedule(task) && guest.threadId() == 1);
            assert(invoke(22, {poller, events, 1, UINT64_MAX}) == 1);
            uint64_t observed = 0;
            assert(task.load64(events + 8, observed) && observed == data);
        };
        block();
        assert(waiting());
        assert(invoke(64, {otherEvent, payload, 8}) == 8 && waiting());
        assert(invoke(86, {otherTimer, 1, timerSpec}) == 0 && waiting());
        assert(invoke(64, {event, payload, 8}) == 8);
        wake(0xa1);
        assert(invoke(63, {event, payload, 8}) == 8);
        block();
        assert(invoke(86, {timer, 1, timerSpec}) == 0);
        wake(0xa2);
        assert(invoke(63, {timer, payload, 8}) == 8);
        block();
        assert(invoke(64, {uint32_t(otherSockets), payload, 1}) == 1 && waiting());
        assert(invoke(64, {uint32_t(watchedSockets), payload, 1}) == 1);
        wake(0xa4);
        assert(invoke(63, {watchedSockets >> 32, payload, 1}) == 1);
        block();
        // A ready level-triggered source registered after epoll_wait must wake it.
        assert(task.store64(watch + 8, 0xa3));
        assert(invoke(21, {poller, 1, otherEvent, watch}) == 0);
        wake(0xa3);
    }
    const auto poll = call(20, {0x80000});
    const auto event = call(19, {0, 0x80800});
    assert(poll == 3 && event == 4);
    assert(machine.store64(base + 256, 1));
    assert(machine.store64(base + 264, 0xdecafe));
    assert(call(21, {poll, 1, event, base + 256}) == 0);
    assert(call(22, {poll, base + 512, 1}) == 0);
    assert(call(63, {event, base + 520, 8}) == uint64_t(-11));
    assert(machine.store64(base + 280, 3));
    assert(call(64, {event, base + 280, 8}) == 8);
    assert(call(22, {poll, base + 512, 1}) == 1);
    assert(load(base + 512) == 1 && load(base + 520) == 0xdecafe);
    assert(call(63, {event, base + 512, 8}) == 8 && load(base + 512) == 3);
    assert(call(22, {poll, base + 512, 1}) == 0);

    for (const uint64_t clock : {0, 1, 8, 9}) {
        const auto timer = call(85, {clock, 0x80800});
        assert(timer < 256 && call(21, {poll, 1, timer, base + 256}) == 0);
        assert(call(63, {timer, base + 512, 8}) == uint64_t(-11));
        const std::array<uint64_t, 4> expiredTimer{0, 0, 0, 1};
        assert(machine.write(base + 280, {reinterpret_cast<const uint8_t*>(expiredTimer.data()), sizeof expiredTimer}));
        assert(call(86, {timer, 1, base + 280, base + 0x380}) == 0);
        assert(load(base + 0x390) == 0 && load(base + 0x398) == 0);
        assert(call(22, {poll, base + 512, 1}) == 1);
        assert(call(63, {timer, base + 512, 8}) == 8 && load(base + 512) == 1);
        assert(call(22, {poll, base + 512, 1}) == 0);
        assert(call(57, {timer}) == 0);
    }
    {
        constexpr auto now = base + 0x3a0, spec = base + 0x3c0, result = base + 0x3e0;
        assert(call(113, {0, now}) == 0);
        const auto originalTime = load(now), originalFraction = load(now + 8);
        const auto setClock = [&](uint64_t seconds, uint64_t fraction = 0) {
            assert(machine.store64(now, seconds) && machine.store64(now + 8, fraction));
            assert(call(112, {0, now}) == 0);
        };
        const auto arm = [&](uint64_t fd, uint64_t flags, uint64_t seconds) {
            const std::array<uint64_t, 4> times{0, 0, seconds, 0};
            assert(machine.write(spec, {reinterpret_cast<const uint8_t*>(times.data()), sizeof times}));
            assert(call(86, {fd, flags, spec, 0}) == 0);
        };
        setClock(10'000);
        const auto absolute = call(85, {8, 0x80800});
        const auto relative = call(85, {0, 0x80800});
        const auto changes = call(85, {0, 0x80800});
        assert(absolute < 256 && relative < 256 && changes < 256);
        assert(call(21, {poll, 1, changes, base + 256}) == 0);
        arm(absolute, 1, 10'005);
        arm(relative, 0, 60);
        arm(changes, 3, 0); // Android listens for time changes on a disarmed CLOCK_REALTIME fd.
        assert(call(22, {poll, base + 512, 1}) == 0);
        setClock(10'010);
        assert(call(113, {8, now}) == 0 && load(now) == 10'010);
        assert(call(169, {now, 0}) == 0 && load(now) == 10'010);
        assert(call(22, {poll, base + 512, 1}) == 1);
        assert(call(63, {changes, result, 8}) == uint64_t(-125));
        assert(call(22, {poll, base + 512, 1}) == 0);
        assert(call(63, {absolute, result, 8}) == 8 && load(result) == 1);
        assert(call(63, {relative, result, 8}) == uint64_t(-11));
        assert(call(57, {absolute}) == 0 && call(57, {relative}) == 0 && call(57, {changes}) == 0);
        setClock(3'000'000'000);
        assert(machine.store64(now, 2'999'999'999) && machine.store64(now + 8, 0));
        assert(machine.store64(result, 0));
        assert(call(98, {result, 0x109, 0, now, 0, UINT32_MAX}) == uint64_t(-110));
        setClock(originalTime, originalFraction);
    }
    {
        ProbeLinux unprivileged(1, 1000);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        task.cpu.x[0] = 9;
        task.cpu.x[8] = 85;
        assert(unprivileged.syscall(task) && task.cpu.x[0] == uint64_t(-1));
    }
    {
        const auto touch = std::make_shared<GuestTouch>();
        linux.setGuestTouch(touch);
        put(base + 704, "/system/usr/idc/Nestvane_Touchscreen.idc");
        const auto config = call(56, {uint32_t(-100), base + 704, 0});
        assert(config < 256);
        constexpr std::string_view touchConfig =
            "touch.enableForInactiveViewport = 1\n"
            "touch.wake = 1\n";
        assert(call(63, {config, base + 768, 192}) == touchConfig.size());
        const auto* configBytes = machine.read(base + 768, touchConfig.size(), 4);
        assert(configBytes && std::string_view(reinterpret_cast<const char*>(configBytes), touchConfig.size()) == touchConfig);
        assert(call(57, {config}) == 0);
        put(base + 704, "/system/usr/keylayout/Nestvane_Touchscreen.kl");
        const auto layout = call(56, {uint32_t(-100), base + 704, 0});
        assert(layout < 256);
        constexpr std::string_view keyLayout = "key 143 WAKEUP\nkey 158 BACK\nkey 172 HOME\n";
        assert(call(63, {layout, base + 768, 96}) == keyLayout.size());
        const auto* layoutBytes = machine.read(base + 768, keyLayout.size(), 4);
        assert(layoutBytes && std::string_view(reinterpret_cast<const char*>(layoutBytes), keyLayout.size()) == keyLayout);
        assert(call(57, {layout}) == 0);
        assert(!touch->press(GuestTouch::Key::wake));
        touch->submit(100, 100, true); // A tap on the boot screen has no guest reader.
        put(base + 704, "/dev/input");
        const auto directory = call(56, {uint32_t(-100), base + 704, 0x4000});
        assert(directory < 256);
        const auto listing = call(61, {directory, base + 768, 192});
        const auto* entries = machine.read(base + 768, listing, 4);
        assert(entries && std::string_view(reinterpret_cast<const char*>(entries), listing).find("event0") != std::string_view::npos);
        assert(call(57, {directory}) == 0);
        put(base + 704, "/dev/input/event0");
        const auto input = call(56, {uint32_t(-100), base + 704, 0x80802});
        assert(input < 256 && call(56, {uint32_t(-100), base + 704, 0x802}) == uint64_t(-16));
        assert(call(29, {input, 0x80044501, base + 760}) == 0 && (load(base + 760) & UINT32_MAX) == 0x010001);
        assert(call(29, {input, 0x80604521, base + 768}) == 96);
        assert(*machine.read(base + 768 + GuestTouch::touchKey / 8, 1, 4) == 1 << (GuestTouch::touchKey % 8));
        constexpr std::array keys{GuestTouch::Key::wake, GuestTouch::Key::back, GuestTouch::Key::home};
        for (const auto key : keys) {
            const auto code = uint16_t(key);
            assert(*machine.read(base + 768 + code / 8, 1, 4) & (1 << (code % 8)));
        }
        assert(machine.store64(base + 688, 0x20000001) && machine.store64(base + 696, 42));
        assert(call(21, {poll, 1, input, base + 688}) == 0);
        assert(call(63, {input, base + 768, 96}) == uint64_t(-11));
        assert(call(22, {poll, base + 512, 1, UINT32_MAX}) == poll && linux.blocked());
        touch->submit(123, 456, true);
        assert(linux.schedule(machine, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
        assert(!linux.blocked());
        assert(call(22, {poll, base + 512, 1}) == 1 && load(base + 512) == 1 && load(base + 520) == 42);
        assert(call(63, {input, 0, 96}) == uint64_t(-14));
        assert(call(63, {input, base + 768, 96}) == 96);
        const auto event = [&](size_t index) {
            GuestTouch::Event data{};
            const auto* bytes = machine.read(base + 768 + index * sizeof(data), sizeof(data), 4);
            assert(bytes);
            std::memcpy(&data, bytes, sizeof(data));
            return data;
        };
        assert(event(0).type == 3 && event(0).code == 0 && event(0).value == 123);
        assert(event(1).type == 3 && event(1).code == 1 && event(1).value == 456);
        assert(event(2).type == 1 && event(2).code == GuestTouch::touchKey && event(2).value == 1);
        assert(event(3).type == 0 && event(3).code == 0);
        assert(call(22, {poll, base + 512, 1}) == 0);
        touch->submit(800, 920, false);
        assert(call(63, {input, base + 768, 96}) == 96);
        assert(event(2).code == GuestTouch::touchKey && event(2).value == 0);
        for (const auto key : keys) {
            assert(call(22, {poll, base + 512, 1, UINT32_MAX}) == poll && linux.blocked());
            assert(touch->press(key));
            assert(linux.schedule(machine, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
            assert(!linux.blocked());
            assert(call(22, {poll, base + 512, 1}) == 1 && load(base + 512) == 1);
            assert(call(63, {input, base + 768, 96}) == 96);
            assert(event(0).type == 1 && event(0).code == uint16_t(key) && event(0).value == 1);
            assert(event(1).type == 0 && event(1).code == 0);
            assert(event(2).type == 1 && event(2).code == uint16_t(key) && event(2).value == 0);
            assert(event(3).type == 0 && event(3).code == 0);
            assert(!touch->state().down && !touch->ready());
        }
        assert(call(57, {input}) == 0);
        linux.setGuestTouch({});
    }

    assert(call(199, {1, 5 | 0x800, 0, base + 512}) == 0);
    auto sockets = load(base + 512);
    auto first = uint32_t(sockets);
    auto second = uint32_t(sockets >> 32);
    assert(first == 5 && second == 6);
    assert(call(21, {poll, 1, second, base + 256}) == 0);
    put(base + 296, "guest packet");
    assert(call(64, {first, base + 296, 12}) == 12);
    assert(call(22, {poll, base + 512, 1}) == 1);
    assert(load(base + 512) == 1);
    assert(call(63, {second, base + 512, 32}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    const std::array<uint64_t, 7> sendHeader{0, 0, base + 640, 2, 0, 0, 0};
    std::array<uint64_t, 4> sendVectors{base + 296, 6, 0, 6};
    assert(machine.write(base + 576, {reinterpret_cast<const uint8_t*>(sendHeader.data()), sizeof(sendHeader)}));
    assert(machine.write(base + 640, {reinterpret_cast<const uint8_t*>(sendVectors.data()), sizeof(sendVectors)}));
    assert(call(211, {first, base + 576, 0x4040}) == uint64_t(-14));
    assert(call(22, {poll, base + 512, 1}) == 0);
    sendVectors[2] = base + 302;
    assert(machine.write(base + 640, {reinterpret_cast<const uint8_t*>(sendVectors.data()), sizeof(sendVectors)}));
    assert(call(211, {first, base + 576, 0x4040}) == 12);
    assert(call(22, {poll, base + 512, 1}) == 1);
    assert(call(207, {second, base + 512, 32, 0x40}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    assert(call(207, {second, base + 512, 32, 0x40}) == uint64_t(-11));
    assert(call(211, {first, base + 576, 0x4040}) == 12);
    const std::array<uint64_t, 7> receiveHeader{0, 0, base + 736, 2, base + 832, 24, 0};
    std::array<uint64_t, 4> receiveVectors{base + 800, 4, 0, 4};
    assert(machine.write(base + 672, {reinterpret_cast<const uint8_t*>(receiveHeader.data()), sizeof(receiveHeader)}));
    assert(machine.write(base + 736, {reinterpret_cast<const uint8_t*>(receiveVectors.data()), sizeof(receiveVectors)}));
    assert(call(212, {second, base + 672, 0x40}) == uint64_t(-14));
    receiveVectors[2] = base + 808;
    assert(machine.write(base + 736, {reinterpret_cast<const uint8_t*>(receiveVectors.data()), sizeof(receiveVectors)}));
    assert(call(212, {second, base + 672, 0x62}) == 12); // Peek with full datagram length.
    assert(load(base + 712) == 0 && load(base + 720) == 0x20);
    assert(call(212, {second, base + 672, 0x40}) == 8);
    assert(std::memcmp(machine.read(base + 800, 4, 4), "gues", 4) == 0);
    assert(std::memcmp(machine.read(base + 808, 4, 4), "t pa", 4) == 0);
    assert(call(212, {second, base + 672, 0x40}) == uint64_t(-11));
    assert(call(57, {first}) == 0);
    assert(call(22, {poll, base + 512, 1}) == 1 && load(base + 512) == (1 | 16)); // EOF is readable.
    assert(call(63, {second, base + 512, 32}) == 0);
    linux.addDirectory("/dev/socket", 0755);
    const auto listener = call(198, {1, 1 | 0x800});
    assert(machine.store64(base + 512, 4));
    assert(call(209, {listener, 1, 39, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 1);
    assert(call(209, {listener, 1, 38, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 0);

    put(base + 770, "/dev/socket/test");
    const std::array<uint8_t, 2> unixFamily{1, 0};
    assert(machine.write(base + 768, unixFamily));
    assert(call(200, {listener, base + 768, 19}) == 0);
    assert(machine.store64(base + 512, 2));
    assert(call(204, {listener, base + 800, base + 512}) == 0 && load(base + 512) == 19);
    assert(std::memcmp(machine.read(base + 800, 2, 4), unixFamily.data(), 2) == 0);
    assert(call(201, {listener, 8}) == 0);
    assert(call(21, {poll, 1, listener, base + 256}) == 0);
    const auto client = call(198, {1, 1});
    assert(call(205, {client, base + 800, base + 512}) == uint64_t(-107));
    assert(call(203, {client, base + 768, 19}) == 0);
    assert(call(205, {client, base + 800, base + 512}) == 0 && load(base + 512) == 19);
    assert(std::memcmp(machine.read(base + 800, 19, 4), machine.read(base + 768, 19, 4), 19) == 0);
    assert(call(22, {poll, base + 512, 2}) == 2); // pending accept and the old socket hangup
    const auto accepted = call(242, {listener, 0, 0, 0x80000});
    assert(accepted < 256);
    assert(call(204, {accepted, base + 800, base + 512}) == 0 && load(base + 512) == 19);
    assert(call(205, {accepted, base + 800, base + 512}) == 0 && load(base + 512) == 2);
    assert(call(206, {client, base + 296, 5}) == 5);
    assert(call(206, {client, base + 301, 7}) == 7);
    assert(call(207, {accepted, base + 512, 12, 0x100}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    assert(call(206, {accepted, base + 296, 3}) == 3);
    assert(call(207, {client, base + 512, 3, 0x100}) == 3);
    assert(call(206, {client, base + 296, 3}) == 3);
    assert(call(210, {client, 1}) == 0);
    assert(call(206, {client, base + 296, 1}) == uint64_t(-32));
    assert(call(207, {accepted, base + 512, 6, 0x100}) == 3);
    assert(call(63, {accepted, base + 512, 3}) == 0);
    assert(call(206, {accepted, base + 296, 3}) == 3);
    assert(call(63, {client, base + 512, 3}) == 3);


    {
        using namespace std::chrono;
        const auto pendingClient = call(198, {1, 1});
        const auto blockedClient = call(198, {1, 1});
        assert(call(201, {listener, 1}) == 0);
        assert(call(203, {pendingClient, base + 768, 19}) == 0);
        assert(machine.store64(base + 832, 0) && machine.store64(base + 840, 30'000));
        assert(call(208, {blockedClient, 1, 67, base + 832, 16}) == 0);
        assert(call(203, {blockedClient, base + 768, 19}) == blockedClient && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(203, {blockedClient, base + 768, 19}) == uint64_t(-11) && !linux.blocked());
        assert(call(203, {blockedClient, base + 768, 19}) == blockedClient && linux.blocked());
        const auto firstConnection = call(202, {listener, 0});
        assert(firstConnection < 256 && !linux.blocked());
        assert(call(203, {blockedClient, base + 768, 19}) == 0);
        const auto secondConnection = call(202, {listener, 0});
        assert(secondConnection < 256);
        for (const auto fd : {pendingClient, blockedClient, firstConnection, secondConnection})
            assert(call(57, {fd}) == 0);
    }

    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr auto limit = base + 0x180;
        uint64_t soft = UINT64_MAX;
        assert(invoke(163, {13, limit}) == 0 && task.load64(limit, soft) && soft == 0);
        assert(task.store64(limit, 24) && task.store64(limit + 8, 40));
        assert(invoke(164, {13, limit}) == 0);
        const auto child = invoke(220, {0x1200011, 0, 0, 0, base + 0x1a0});
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        assert(invoke(147, {1000, 1000, 1000}) == 0);
        assert(invoke(140, {0, 0, uint64_t(-4)}) == 0 && invoke(141, {0, 0}) == 24);
        assert(invoke(140, {0, 0, uint64_t(-5)}) == uint64_t(-13));
        assert(invoke(140, {0, 1, 10}) == uint64_t(-1));
    }

    // Test the clone ABI with the same registers Bionic supplies to the kernel.
    assert(call(140, {0, 0, 3}) == 0);
    machine.cpu.pc = base;
    const auto statScratch = call(222, {0, pageSize, 3, 0x22, uint64_t(-1), 0});
    assert(statScratch < (uint64_t{1} << 45));
    const auto childTid = call(220, {0x3d0f00, base + 0x900, base + 0x810, base + 0xa00, base + 0x810});
    assert(childTid == 2 && load(base + 0x810) == childTid);
    const auto statThreads = [&](const char* path, std::string_view prefix) {
        put(statScratch, path);
        const auto fd = call(56, {uint32_t(-100), statScratch, 0});
        assert(fd < 256);
        const auto size = call(63, {fd, statScratch + 64, 500});
        assert(size && size <= 500 && call(57, {fd}) == 0);
        const auto* content = reinterpret_cast<const char*>(machine.read(statScratch + 64, size, 4));
        const std::string line(content, size);
        assert(line.starts_with(prefix));
        const auto commEnd = line.rfind(')');
        assert(commEnd != std::string::npos);
        std::istringstream fields(line.substr(commEnd + 2));
        std::string field;
        for (unsigned number = 3; number <= 20; ++number) assert(fields >> field);
        return std::stoi(field);
    };
    assert(statThreads("/proc/1/task/1/stat", "1 (") == 2);
    assert(statThreads("/proc/self/task/2/stat", "2 (") == 2);
    put(statScratch, "/proc/self/fd");
    const auto fdDirectory = call(56, {uint32_t(-100), statScratch, 0x4000});
    assert(fdDirectory < 256);
    const auto dirBytes = call(61, {fdDirectory, statScratch + 64, 512});
    assert(dirBytes && dirBytes <= 512);
    const auto* records = machine.read(statScratch + 64, dirBytes, 4);
    assert(records);
    bool sawDirectory = false;
    for (uint64_t position = 0; position < dirBytes;) {
        const auto* record = records + position;
        uint16_t size = 0;
        std::memcpy(&size, record + 16, sizeof(size));
        assert(size >= 24 && size <= dirBytes - position);
        if (std::string_view(reinterpret_cast<const char*>(record + 19)) == std::to_string(fdDirectory))
            sawDirectory = record[18] == 10;
        position += size;
    }
    assert(sawDirectory && call(57, {fdDirectory}) == 0);
    machine.cpu.pc += 4;
    assert(linux.schedule(machine));
    assert(linux.threadId() == 2 && machine.cpu.x[0] == 0 && machine.cpu.pc == base + 4);
    assert(machine.cpu.sp == base + 0x900 && machine.cpu.threadPointer == base + 0xa00);
    assert(call(172) == 1 && call(178) == 2);
    assert(call(141, {0, childTid}) == 17); // Inherits nice 3.
    assert(call(140, {0, childTid, 5}) == 0);
    assert(call(141, {0, 0}) == 15 && call(141, {0, 1}) == 17);
    assert(machine.store64(base + 0x800, 5));
    assert(call(98, {base + 0x800, 128, 4}) == uint64_t(-11) && !linux.blocked());
    call(98, {base + 0x800, 128, 5});
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(call(98, {base + 0x800, 132, 0, 1, base + 0x804, 4}) == uint64_t(-11));
    assert(call(98, {base + 0x800, 132, 0, 1, base + 0x804, 5}) == 1);
    assert(call(98, {base + 0x800, 129, 1}) == 0);
    assert(call(98, {base + 0x804, 129, 1}) == 1);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == 2 && machine.cpu.x[0] == 0);
    assert(machine.cpu.pc == base + 8);

    constexpr uint64_t farTimeout = base + pageSize - 16, timedFutex = base + 0x800;
    assert(machine.store64(farTimeout, 0) && machine.store64(farTimeout + 8, 1'000'000'000));
    assert(call(98, {timedFutex, 128, 5, farTimeout}) == uint64_t(-22));
    for (const auto [op, seconds, nanos] : {
             std::array<uint64_t, 3>{128, INT64_MAX / 1'000'000'000, INT64_MAX % 1'000'000'000},
             std::array<uint64_t, 3>{128 | 256 | 9, INT64_MAX, 0}}) {
        assert(machine.store64(farTimeout, seconds) && machine.store64(farTimeout + 8, nanos));
        call(98, {timedFutex, op, 5, farTimeout, 0, UINT32_MAX});
        assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
        assert(call(98, {timedFutex, 129, 1}) == 1);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.threadId() == 2 && machine.cpu.x[0] == 0);
    }

    constexpr uint64_t piWord = base + 0x838, piTimeout = base + 0x848;
    constexpr uint64_t privateFutex = 128, lockPi2 = 13, unlockPi = 7, trylockPi = 8;
    constexpr uint64_t piWaiters = 0x80000000, piOwnerDied = 0x40000000;
    assert(machine.store64(piWord, 0));
    assert(machine.store64(piTimeout, 0) && machine.store64(piTimeout + 8, 0));
    assert(call(98, {piWord, privateFutex | lockPi2, 0, piTimeout}) == 0);
    assert(load(piWord) == childTid);
    assert(call(98, {piWord, privateFutex | lockPi2}) == uint64_t(-35));
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == 1);
    assert(call(98, {piWord, privateFutex | unlockPi}) == uint64_t(-1));
    assert(call(98, {piWord, privateFutex | trylockPi}) == uint64_t(-11));
    assert(load(piWord) == (piWaiters | childTid));
    assert(call(98, {piWord, privateFutex | lockPi2, 0, piTimeout}) == uint64_t(-110));

    machine.cpu.pc = base + 0x24;
    call(98, {piWord, privateFutex | lockPi2});
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == childTid);
    assert(call(98, {piWord, privateFutex | unlockPi}) == 0 && load(piWord) == (piWaiters | 1));
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == 1);
    assert(machine.cpu.pc == base + 0x28 && machine.cpu.x[0] == 0);
    assert(call(98, {piWord, privateFutex | unlockPi}) == 0 && load(piWord) == 0);
    assert(machine.store64(piWord, piWaiters | piOwnerDied));
    assert(call(98, {piWord, privateFutex | trylockPi}) == 0 && load(piWord) == (piOwnerDied | 1));
    assert(call(98, {piWord, privateFutex | unlockPi}) == 0 && load(piWord) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == childTid);

    constexpr uint64_t userSignal = 10, signalMask = 1ULL << (userSignal - 1);
    assert(machine.store64(base + 0x820, signalMask));
    assert(machine.store64(base + 0x8c0, 0) && machine.store64(base + 0x8c8, 0));
    assert(call(137, {base + 0x820, 0, base + 0x8c0, 8}) == uint64_t(-11));
    call(137, {base + 0x820, base + 0xe00, 0, 8});
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(call(131, {2, childTid, userSignal}) == uint64_t(-3));
    assert(call(131, {1, childTid, userSignal}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == childTid);
    assert(call(137, {base + 0x820, base + 0xe00, 0, 8}) == userSignal);
    assert(load(base + 0xe00) == userSignal && uint32_t(load(base + 0xe08)) == uint32_t(-6));

    assert(call(135, {0, base + 0x820, 0, 8}) == 0);
    std::array<uint32_t, 32> queuedInfo{0, 0, uint32_t(-1), 0, 1, 123, 0x123456};
    constexpr auto signalInfo = base + 0xe80;
    assert(machine.write(signalInfo, {reinterpret_cast<const uint8_t*>(queuedInfo.data()), sizeof(queuedInfo)}));
    call(137, {base + 0x820, base + 0xe00, 0, 8});
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(call(240, {1, childTid, userSignal, 0xdead}) == uint64_t(-14));
    assert(call(240, {2, childTid, userSignal, signalInfo}) == uint64_t(-3));
    assert(machine.store64(signalInfo + 8, uint32_t(-6)));
    assert(call(240, {1, childTid, userSignal, signalInfo}) == uint64_t(-1));
    assert(machine.store64(signalInfo + 8, uint32_t(-1)));
    assert(call(240, {1, childTid, userSignal, signalInfo}) == 0);
    assert(machine.store64(signalInfo + 24, 0x654321));
    assert(call(240, {1, childTid, userSignal, signalInfo}) == 0); // Coalesces with the first entry.
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == childTid);
    assert(call(137, {base + 0x820, base + 0xe00, 0, 8}) == userSignal);
    assert(load(base + 0xe00) == userSignal && uint32_t(load(base + 0xe08)) == uint32_t(-1));
    assert(load(base + 0xe18) == 0x123456);
    call(93);
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(load(base + 0x810) == 0);
    assert(call(141, {0, childTid}) == uint64_t(-3));
    assert(statThreads("/proc/1/task/1/stat", "1 (") == 1);
    put(statScratch, "/proc/1/task/2/stat");
    assert(call(56, {uint32_t(-100), statScratch, 0}) == uint64_t(-2));
    assert(call(215, {statScratch, pageSize}) == 0);

    linux.addDirectory("/view", 0755);
    linux.addDirectory("/other", 0755);
    put(base + 100, "/view");
    put(base + 120, "/view/sample");
    put(base + 144, "/other");
    put(base + 160, "/other/example");
    put(base + 184, "/proc/self/ns/mnt");
    assert(call(40, {base + 16, base + 100, 0, 0x5000}) == 0);
    const auto viaBind = call(56, {uint32_t(-100), base + 120, 2});
    assert(viaBind < 256 && call(64, {viaBind, base + 296, 12}) == 12);
    const auto original = call(56, {uint32_t(-100), base + 72, 0});
    assert(original < 256 && call(63, {original, base + 512, 12}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    const auto firstNamespace = call(56, {uint32_t(-100), base + 184, 0});
    assert(firstNamespace < 256);
    assert(call(97, {0x20000}) == 0);
    const auto secondNamespace = call(56, {uint32_t(-100), base + 184, 0});
    assert(secondNamespace < 256 && secondNamespace != firstNamespace);
    assert(call(80, {firstNamespace, base + 512}) == 0 && load(base + 520) == 1);
    assert(call(80, {secondNamespace, base + 512}) == 0 && load(base + 520) == 2);
    assert(call(40, {base, base + 144, base, 14, base + 40}) == 0);
    assert(call(56, {uint32_t(-100), base + 160, 0x42, 0600}) < 256);
    assert(call(268, {firstNamespace, 0x20000}) == 0);
    assert(call(56, {uint32_t(-100), base + 160, 0}) == uint64_t(-2));
    assert(call(268, {secondNamespace, 0x20000}) == 0);
    assert(call(56, {uint32_t(-100), base + 160, 0}) < 256);

    {
        ProbeLinux guest(1, 0);
        guest.addBootTmpfs("/data", 0771, 1000, 6);
        guest.addDirectory("/data_mirror", 0755);
        guest.addDirectory("/data/user_de", 0751);
        guest.addDirectory("/data/user_de/0", 0771);
        guest.addDirectory("/data/user_de/0/controller", 0700);
        constexpr std::array<uint8_t, 1> marker{42};
        guest.mount("/data/user_de/0/controller/marker", marker);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](uint64_t offset, const auto& text) {
            assert(task.write(base + offset, {reinterpret_cast<const uint8_t*>(text), sizeof(text)}));
            return base + offset;
        };
        const auto tmpfs = path(0, "tmpfs");
        const auto mirror = path(32, "/data_mirror");
        const auto mountOptions = path(64, "mode=0700,uid=0,gid=1000");
        const auto userDe = path(96, "/data/user_de");
        const auto mirroredDirectory = path(128, "/data_mirror/data_de");
        const auto mirroredRoot = path(160, "/data_mirror/data_de/null");
        const auto user = path(192, "/data/user_de/0");
        const auto app = path(224, "/data/user_de/0/controller");
        const auto mirroredApp = path(256, "/data_mirror/data_de/null/0/controller");
        const auto data = path(320, "/data/user_de/0/controller/marker");
        constexpr auto atCwd = uint32_t(-100);
        constexpr auto stat = base + 512;
        assert(invoke(40, {tmpfs, mirror, tmpfs, 14, mountOptions}) == 0);
        assert(invoke(79, {atCwd, mirror, stat, 0}) == 0);
        uint64_t metadata;
        assert(task.load64(stat + 24, metadata) && metadata == (uint64_t{1000} << 32));
        assert(task.load64(stat + 16, metadata) && (metadata & 07777) == 0700);
        assert(invoke(34, {atCwd, mirroredDirectory, 0700}) == 0);
        assert(invoke(34, {atCwd, mirroredRoot, 0700}) == 0);
        assert(invoke(40, {userDe, mirroredRoot, 0, 0x5000}) == 0);
        assert(invoke(97, {0x20000}) == 0 && invoke(40, {tmpfs, userDe, tmpfs, 14}) == 0);
        assert(invoke(56, {atCwd, data, 0}) == uint64_t(-2));
        assert(invoke(34, {atCwd, user, 0751}) == 0);
        assert(invoke(34, {atCwd, app, 0700}) == 0);
        assert(invoke(40, {mirroredApp, app, 0, 0x5000}) == 0);
        const auto original = invoke(56, {atCwd, data, 0});
        assert(original < 256 && invoke(63, {original, base + 800, 1}) == 1);
        assert(*task.read(base + 800, 1, 4) == marker.front());
    }

    {
        ProbeLinux guest(1, 0);
        guest.addBootTmpfs("/data", 0771, 1000, 6);
        guest.addBootTmpfs("/mirror", 0700, 0, 14);
        guest.addDirectory("/data/data", 0771);
        guest.addDirectory("/data/user", 0771);
        guest.addDirectory("/data/user/0", 0771);
        guest.addDirectory("/data/user/1", 0771);
        guest.addDirectory("/mirror/ce", 0700);
        constexpr std::array<uint8_t, 1> marker{42};
        guest.mount("/data/data/marker", marker);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 5> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](uint64_t offset, const auto& text) {
            assert(task.write(base + offset, {reinterpret_cast<const uint8_t*>(text), sizeof(text)}));
            return base + offset;
        };
        const auto dataMount = path(0, "/data");
        const auto data = path(32, "/data/data");
        const auto users = path(64, "/data/user");
        const auto userZero = path(96, "/data/user/0");
        const auto mirror = path(128, "/mirror/ce");
        const auto zeroMarker = path(160, "/mirror/ce/0/marker");
        const auto userOne = path(192, "/data/user/1");
        const auto oneMarker = path(224, "/mirror/ce/1/marker");
        constexpr auto atCwd = uint32_t(-100);
        assert(invoke(40, {0, dataMount, 0, 0x100000}) == 0); // MS_SHARED
        assert(invoke(40, {users, mirror, 0, 0x5000}) == 0);
        assert(invoke(56, {atCwd, zeroMarker, 0}) == uint64_t(-2));
        assert(invoke(40, {data, userZero, 0, 0x1000}) == 0);
        const auto mirrored = invoke(56, {atCwd, zeroMarker, 0});
        assert(mirrored < 256 && invoke(63, {mirrored, base + 512, 1}) == 1);
        assert(*task.read(base + 512, 1, 4) == marker.front());
        assert(invoke(40, {0, mirror, 0, 0x40000}) == 0); // MS_PRIVATE
        assert(invoke(40, {data, userOne, 0, 0x1000}) == 0);
        assert(invoke(56, {atCwd, oneMarker, 0}) == uint64_t(-2));
    }

    {
        ProbeLinux mounts(1, 0);
        mounts.addBootTmpfs("/data", 0771, 1000, 6);
        mounts.addDirectory("/data/data", 0771);
        mounts.addDirectory("/data/user", 0751);
        mounts.addDirectory("/data/user/0", 0751);
        constexpr std::array<uint8_t, 1> content{42};
        mounts.mount("/data/data/marker", content);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(mounts.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](uint64_t offset, const auto& text) {
            assert(task.write(base + offset, {reinterpret_cast<const uint8_t*>(text), sizeof(text)}));
            return base + offset;
        };
        const auto source = path(0, "/data/data");
        const auto user = path(32, "/data/user");
        const auto userZero = path(64, "/data/user/0");
        const auto markerPath = path(96, "/data/user/0/marker");
        const auto tmpfs = path(128, "tmpfs");
        const auto namespacePath = path(160, "/proc/self/ns/mnt");
        constexpr auto atCwd = uint32_t(-100);
        assert(invoke(40, {source, userZero, 0, 0x5000}) == 0);
        assert(invoke(56, {atCwd, markerPath, 0}) < 256);
        const auto originalNamespace = invoke(56, {atCwd, namespacePath, 0});
        assert(originalNamespace < 256 && invoke(97, {0x20000}) == 0);
        const auto isolatedNamespace = invoke(56, {atCwd, namespacePath, 0});
        assert(isolatedNamespace < 256 && invoke(40, {tmpfs, user, tmpfs, 14}) == 0);
        assert(invoke(56, {atCwd, markerPath, 0}) == uint64_t(-2));
        assert(invoke(36, {source, atCwd, userZero}) == 0);
        assert(invoke(79, {atCwd, userZero, base + 256, 0x100}) == 0);
        uint64_t metadata = 0;
        assert(task.load64(base + 272, metadata) && (metadata & 0170000) == 0120000);
        assert(invoke(79, {atCwd, userZero, base + 256, 0}) == 0);
        assert(task.load64(base + 272, metadata) && (metadata & 0170000) == 0040000);
        const auto labelName = path(192, "security.selinux");
        constexpr char linkLabel[] = "u:object_r:system_data_file:s0";
        constexpr char targetLabel[] = "u:object_r:app_data_file:s0";
        const auto linkLabelAddress = path(320, linkLabel);
        const auto targetLabelAddress = path(384, targetLabel);
        constexpr auto labelResult = base + 448;
        assert(invoke(5, {source, labelName, targetLabelAddress, sizeof(targetLabel), 0}) == 0);
        assert(invoke(6, {userZero, labelName, linkLabelAddress, sizeof(linkLabel), 1}) == 0);
        assert(invoke(6, {userZero, labelName, linkLabelAddress, sizeof(linkLabel), 1}) == uint64_t(-17));
        assert(invoke(9, {userZero, labelName, labelResult, 1}) == uint64_t(-34));
        assert(invoke(9, {userZero, labelName, labelResult, 64}) == sizeof(linkLabel));
        assert(std::memcmp(task.read(labelResult, sizeof(linkLabel), 4), linkLabel, sizeof(linkLabel)) == 0);
        assert(invoke(8, {userZero, labelName, labelResult, 64}) == sizeof(targetLabel));
        assert(std::memcmp(task.read(labelResult, sizeof(targetLabel), 4), targetLabel, sizeof(targetLabel)) == 0);
        assert(invoke(56, {atCwd, markerPath, 0}) < 256);
        assert(invoke(268, {originalNamespace, 0x20000}) == 0);
        assert(invoke(78, {atCwd, userZero, base + 256, 32}) == uint64_t(-22));
        assert(invoke(268, {isolatedNamespace, 0x20000}) == 0);
        assert(invoke(39, {user, 0}) == 0);
        assert(invoke(78, {atCwd, userZero, base + 256, 32}) == uint64_t(-22));
        assert(invoke(56, {atCwd, markerPath, 0}) < 256);
    }

    {
        ProbeLinux mounts(1, 0);
        mounts.addBootTmpfs("/data", 0771, 1000, 6);
        mounts.addDirectory("/data/data", 0771);
        mounts.addDirectory("/data/data/own.app", 0700);
        mounts.addDirectory("/data/user", 0751);
        mounts.addDirectory("/data/user/0", 0751);
        mounts.addDirectory("/data/private", 0700);
        mounts.addDirectory("/data/mirror", 0771);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(mounts.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](uint64_t offset, const auto& text) {
            assert(task.write(base + offset, {reinterpret_cast<const uint8_t*>(text), sizeof(text)}));
            return base + offset;
        };
        const auto source = path(0, "/data/data");
        const auto user = path(48, "/data/user");
        const auto userZero = path(96, "/data/user/0");
        const auto package = path(144, "/data/data/own.app");
        const auto isolatedDatabases = path(192, "/data/user/0/own.app/databases");
        const auto canonicalDatabases = path(240, "/data/data/own.app/databases");
        const auto database = path(288, "/data/user/0/own.app/databases/test.db");
        const auto tmpfs = path(336, "tmpfs");
        const auto options = path(384, "mode=751");
        const auto hiddenAlias = path(432, "/data/private/alias");
        const auto hiddenDatabases = path(480, "/data/private/alias/own.app/other");
        const auto mirror = path(528, "/data/mirror");
        const auto backingPackage = path(576, "/data/mirror/own.app");
        const auto backup = path(624, "/data/user/0/own.app/databases/test.db.bak");
        const auto crossMount = path(672, "/data/mirror/own.app/databases/test.db.bak");
        constexpr auto atCwd = uint32_t(-100);
        assert(invoke(54, {atCwd, package, 10068, 10068}) == 0);
        assert(invoke(40, {source, userZero, 0, 0x5000}) == 0);
        assert(invoke(40, {source, mirror, 0, 0x1000}) == 0);
        assert(invoke(97, {0x20000}) == 0);
        assert(invoke(40, {tmpfs, user, tmpfs, 14, options}) == 0);
        assert(invoke(40, {tmpfs, source, tmpfs, 14, options}) == 0);
        assert(invoke(34, {atCwd, package, 0700}) == 0);
        assert(invoke(40, {backingPackage, package, 0, 0x1000}) == 0);
        assert(invoke(36, {source, atCwd, userZero}) == 0);
        assert(invoke(36, {source, atCwd, hiddenAlias}) == 0);
        assert(invoke(149, {10068, 10068, 10068}) == 0);
        assert(invoke(147, {10068, 10068, 10068}) == 0);
        assert(invoke(34, {atCwd, isolatedDatabases, 0700}) == 0);
        assert(invoke(34, {atCwd, canonicalDatabases, 0700}) == uint64_t(-17));
        assert(invoke(34, {atCwd, userZero, 0700}) == uint64_t(-17));
        assert(invoke(34, {atCwd, hiddenDatabases, 0700}) == uint64_t(-13));
        assert(invoke(56, {atCwd, database, 0x42, 0600}) < 256);
        assert(invoke(38, {atCwd, database, atCwd, crossMount}) == uint64_t(-18));
        assert(invoke(38, {atCwd, database, atCwd, backup}) == 0);
        assert(invoke(56, {atCwd, database, 0}) == uint64_t(-2));
        assert(invoke(56, {atCwd, crossMount, 0}) < 256);
        assert(invoke(35, {atCwd, backup, 0}) == 0);
        assert(invoke(56, {atCwd, crossMount, 0}) == uint64_t(-2));
    }

    assert(machine.store64(base + 0x840, 17));
    assert(machine.store64(base + 0x850, 1ULL << 16));
    assert(call(135, {0, base + 0x850, 0, 8}) == 0);
    const auto signal = call(74, {UINT32_MAX, base + 0x850, 8, 0x800});
    assert(signal < 256);
    const auto childPoll = call(20);
    assert(machine.store64(base + 256, 1));
    assert(call(21, {childPoll, 1, signal, base + 256}) == 0);
    assert(call(63, {signal, base + 0x900, 128}) == uint64_t(-11));
    const auto sharedFutex = call(222, {0, pageSize, 3, 1, viaBind, 0});
    assert(sharedFutex > 0x100000000 && sharedFutex < 0x2000000000);
    assert(machine.store64(sharedFutex, 55));
    constexpr uint64_t lockAddress = base + 0xa40;
    const auto prepareLock = [&](uint16_t type, int64_t start, int64_t length) {
        assert(machine.store64(lockAddress, type));
        assert(machine.store64(lockAddress + 8, uint64_t(start)));
        assert(machine.store64(lockAddress + 16, uint64_t(length)));
        assert(machine.store64(lockAddress + 24, 0));
    };
    prepareLock(1, 10, 20);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);
    prepareLock(2, 15, 5);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);
    constexpr uint64_t heapSize = 96 * 1024 * 1024;
    const auto heap = call(222, {0, heapSize, 3, 0x22, uint64_t(-1), 0});
    const auto heapTail = heap + heapSize - pageSize;
    assert(heap < (uint64_t{1} << 45) && load(heap) == 0 && load(heapTail) == 0);
    assert(machine.store64(heap, 31) && machine.store64(heapTail, 79));
    assert(machine.store64(heap + pageSize, 44) && machine.store64(heap + 2 * pageSize, 55));
    assert(call(233, {heap + pageSize, pageSize, 10}) == 0);
    assert(call(233, {heap + 2 * pageSize, pageSize, 18}) == 0);
    assert(call(226, {heapTail, pageSize, 0}) == 0);
    assert(!machine.read(heapTail, 8, 4));
    const auto child = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(child == 3 && load(base + 0x848) == 0);
    machine.cpu.pc += 4;
    assert(linux.schedule(machine) && linux.processId() == child);
    assert(load(base + 0x848) == child && call(173) == 1);
    assert(load(heap) == 31 && machine.store64(heap, 55));
    assert(!machine.read(heap + pageSize, 8, 4) && load(heap + 2 * pageSize) == 0);
    assert(call(226, {heapTail, pageSize, 3}) == 0 && load(heapTail) == 79);
    assert(call(233, {heapTail, pageSize, 4}) == 0 && load(heapTail) == 0);
    prepareLock(1, 8, 4);
    assert(call(25, {viaBind, 6, lockAddress}) == uint64_t(-11));
    assert(call(25, {viaBind, 5, lockAddress}) == 0);
    assert(load(lockAddress) == 1 && load(lockAddress + 8) == 10);
    assert(load(lockAddress + 16) == 5 && load(lockAddress + 24) == 1);
    prepareLock(1, 20, -5);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);
    prepareLock(1, 8, 4);
    assert(call(25, {viaBind, 7, lockAddress}) == viaBind && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == 1);
    prepareLock(1, 15, 5);
    assert(call(25, {viaBind, 7, lockAddress}) == uint64_t(-35) && !linux.blocked());
    const auto aliasForLock = call(23, {viaBind});
    assert(aliasForLock < 256 && call(57, {aliasForLock}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == child && machine.cpu.x[0] == viaBind);
    assert(call(25, {viaBind, 7, lockAddress}) == 0 && !linux.blocked());
    const auto childFutex = call(222, {0, pageSize, 3, 1, viaBind, 0});
    assert(childFutex != sharedFutex && load(childFutex) == 55);
    call(98, {childFutex, 0, 55});
    assert(linux.blocked() && linux.schedule(machine) && linux.processId() == 1);
    assert(call(98, {sharedFutex, 0x81, 1}) == 0);
    assert(call(98, {sharedFutex, 1, 1}) == 1);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == child && machine.cpu.x[0] == 0);
    prepareLock(1, 8, 4);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);

    constexpr uint64_t quit = 3, quitMask = 1ULL << (quit - 1);
    assert(machine.store64(base + 0x820, quitMask));
    assert(call(135, {0, base + 0x820, 0, 8}) == 0);
    call(137, {base + 0x820, base + 0xe00, 0, 8});
    assert(linux.blocked() && linux.schedule(machine) && linux.processId() == 1);
    assert(call(129, {child, quit}) == 0 && call(129, {child, quit}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == child);
    assert(call(137, {base + 0x820, base + 0xe00, 0, 8}) == quit);
    assert(load(base + 0xe00) == quit && load(base + 0xe08) == 0 && load(base + 0xe10) == 1);
    assert(call(137, {base + 0x820, 0, base + 0x8c0, 8}) == uint64_t(-11));

    assert(machine.store64(base + 0x840, 99));
    assert(call(57, {original}) == 0);
    call(94, {23});
    assert(linux.blocked() && linux.schedule(machine) && linux.processId() == 1);
    assert(load(base + 0x840) == 17 && load(base + 0x848) == 0);
    assert(load(heap) == 31 && !machine.read(heapTail, 8, 4));
    assert(load(heap + pageSize) == 44 && load(heap + 2 * pageSize) == 55);
    assert(call(233, {heap + pageSize, pageSize, 11}) == 0);
    assert(call(233, {heap + 2 * pageSize, pageSize, 19}) == 0);
    assert(call(226, {heapTail, pageSize, 3}) == 0 && load(heapTail) == 79);
    const auto heapMoved = call(216, {heap, heapSize, heapSize + pageSize, 1});
    assert(heapMoved < (uint64_t{1} << 45) && load(heapMoved) == 31);
    assert(load(heapMoved + heapSize - pageSize) == 79 && load(heapMoved + heapSize) == 0);
    assert(call(215, {heapMoved, heapSize + pageSize}) == 0 && !machine.read(heapMoved, 8, 4));
    assert(call(80, {original, base + 512}) == 0);
    assert(call(22, {childPoll, base + 512, 1, 0}) == 1 && load(base + 512) == 1);
    assert(call(63, {signal, base + 0x900, 128}) == 128);
    assert(load(base + 0x900) == 17);
    assert(load(base + 0x908) == (child << 32 | 1));
    assert(load(base + 0x928) == 23);
    assert(call(63, {signal, base + 0x900, 128}) == uint64_t(-11));
    assert(call(95, {1, child, base + 512, 0x1000005}) == 0);
    assert(load(base + 528) == child && load(base + 536) == 23);
    assert(call(260, {child, 0xdead, 1}) == uint64_t(-14));
    assert(call(260, {child, base + 512, 1}) == child && load(base + 512) == 23 * 256);
    assert(call(95, {1, child, base + 512, 5}) == uint64_t(-10));
    assert(call(129, {child, 9}) == uint64_t(-3));
    assert(call(129, {1, 0}) == 0);

    // An early-init action mounts a subdirectory over its parent mount.
    put(base + 100, "/test/sub");
    put(base + 120, "/test/sub/marker");
    put(base + 144, "/test/marker");
    assert(call(34, {uint32_t(-100), base + 100, 0755}) == 0);
    assert(call(56, {uint32_t(-100), base + 120, 0x42, 0600}) < 256);
    assert(call(40, {base + 100, base + 16, 0, 0x5000}) == 0);
    assert(call(56, {uint32_t(-100), base + 144, 0}) < 256);
    assert(call(56, {uint32_t(-100), base + 72, 0}) == uint64_t(-2));

    assert(call(59, {0xdead, 0}) == uint64_t(-14));
    assert(call(59, {base + 512, 0x80800}) == 0);
    const auto pipe = load(base + 512);
    const auto receiver = uint32_t(pipe), sender = uint32_t(pipe >> 32);
    const auto pipePoller = call(20, {0x80000});
    constexpr uint64_t pipeEvent = base + 0x740;
    assert(machine.store64(pipeEvent, 1) && machine.store64(pipeEvent + 8, 0x54));
    assert(pipePoller < 256 && call(21, {pipePoller, 1, receiver, pipeEvent}) == 0);
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 0);
    assert(call(63, {receiver, base + 512, 12}) == uint64_t(-11));
    assert(call(64, {sender, base + 296, 12}) == 12);
    const auto duplicate = call(23, {sender});
    assert(duplicate < 256 && call(57, {sender}) == 0);
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 1 && load(pipeEvent) == 1 && load(pipeEvent + 8) == 0x54);
    assert(call(63, {receiver, base + 512, 12}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    assert(call(63, {receiver, base + 512, 12}) == uint64_t(-11));
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 0);
    assert(call(57, {duplicate}) == 0);
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 1 && load(pipeEvent) == 16);
    assert(call(63, {receiver, base + 512, 12}) == 0);
    assert(call(57, {receiver}) == 0 && call(57, {pipePoller}) == 0);

    {
        constexpr uint64_t maxPipeCapacity = 1024 * 1024;
        constexpr uint64_t payload = 65536 + pageSize;
        const auto buffer = call(222, {0, payload, 3, 0x22, uint64_t(-1), 0});
        assert(int64_t(buffer) > 0);
        assert(call(59, {base + 512, 0x800}) == 0);
        const auto ends = load(base + 512);
        const auto reader = uint32_t(ends), writer = uint32_t(ends >> 32);
        assert(call(25, {reader, 1032}) == 65536);
        assert(call(25, {writer, 1031, maxPipeCapacity}) == maxPipeCapacity);
        assert(call(25, {reader, 1032}) == maxPipeCapacity);
        assert(call(25, {reader, 1031, maxPipeCapacity + 1}) == uint64_t(-1));
        assert(call(25, {reader, 1031, UINT64_MAX}) == uint64_t(-22));
        assert(machine.store64(buffer + payload - 8, 0x12345678));
        assert(call(64, {writer, buffer, payload}) == payload);
        assert(call(25, {reader, 1031, 65536}) == uint64_t(-16));
        assert(call(25, {reader, 1032}) == maxPipeCapacity);
        assert(machine.store64(buffer + payload - 8, 0));
        assert(call(63, {reader, buffer, payload}) == payload && load(buffer + payload - 8) == 0x12345678);
        assert(call(25, {reader, 1031, pageSize + 1}) == 2 * pageSize);
        assert(call(25, {writer, 1031, 1}) == pageSize);
        assert(call(64, {writer, buffer, pageSize}) == pageSize);
        assert(call(64, {writer, buffer, 1}) == uint64_t(-11));
        assert(call(63, {reader, buffer, pageSize}) == pageSize);
        assert(call(57, {writer}) == 0 && call(57, {reader}) == 0);
        assert(call(215, {buffer, payload}) == 0);
    }

    put(base + 120, "/system/bin/absent");
    const auto beforeExec = machine.cpu.pc;
    assert(call(221, {base + 120, 0, 0}) == uint64_t(-2));
    assert(!linux.replacedImage() && machine.cpu.pc == beforeExec);
    assert(call(166, {0077}) == 0022 && call(166, {0022}) == 0077);
    assert(machine.store64(base + 512, 1001));
    assert(call(159, {1, base + 512}) == 0 && call(158, {0}) == 1);
    assert(call(158, {1, base + 512}) == 1 && load(base + 512) == 1001);
    assert(call(159, {0, 0}) == 0 && call(158, {0}) == 0);

    put(base + 180, "/test/empty");
    assert(call(35, {uint32_t(-100), base + 180, 0x200}) == uint64_t(-2));
    assert(call(34, {uint32_t(-100), base + 180, 0755}) == 0);
    assert(call(35, {uint32_t(-100), base + 180, 0x200}) == 0);
    assert(call(35, {uint32_t(-100), base + 16, 0x200}) == uint64_t(-16));
    assert(call(35, {uint32_t(-100), base + 144, 0x200}) == uint64_t(-20));

    const auto moveSource = call(222, {0, pageSize, 3, 0x22, uint64_t(-1), 0});
    const auto moveTarget = call(222, {0, pageSize, 3, 0x22, uint64_t(-1), 0});
    assert(machine.store64(moveSource, 0xdecaf));
    assert(call(216, {moveSource, pageSize, pageSize, 3, moveSource}) == uint64_t(-22));
    assert(call(216, {moveSource, pageSize, pageSize, 3, moveTarget}) == moveTarget);
    assert(load(moveTarget) == 0xdecaf && !machine.read(moveSource, 8, 4));

    constexpr uint64_t low = 0x2000000;
    assert(call(222, {low, pageSize, 3, 0x100022, uint64_t(-1), 0}) == low);
    assert(call(222, {low, pageSize, 0, 0x100022, uint64_t(-1), 0}) == uint64_t(-17));
    assert(machine.store64(low, 42));
    const auto grown = call(216, {low, pageSize, pageSize * 2, 1});
    assert(grown == low && load(grown) == 42 && load(grown + pageSize) == 0);
    assert(call(215, {low, pageSize * 2}) == 0 && !machine.read(low, 8, 4));
    assert(call(96, {0}) == 1);
    assert(call(283, {8, 0}) == uint64_t(-1));
    assert(call(283, {16, 0}) == 0 && call(283, {8, 0}) == 0);

    const auto wakeTimer = call(85, {9, 0x80800});
    assert(wakeTimer < 256);
    constexpr auto caps = base + 600;
    assert(machine.store64(base + 512, 0x20080522));
    assert(call(90, {base + 512, caps}) == 0 && load(caps) == UINT64_MAX);
    assert(machine.store64(caps, 0x100'0000'0100ULL));
    assert(machine.store64(caps + 8, 0x100));
    assert(machine.store64(caps + 16, 0));
    assert(call(91, {base + 512, caps}) == 0);
    const std::array<uint64_t, 4> disarmTimer{};
    assert(machine.write(base + 768, {reinterpret_cast<const uint8_t*>(disarmTimer.data()), sizeof(disarmTimer)}));
    assert(call(86, {wakeTimer, 0, base + 768}) == uint64_t(-1));
    assert(call(57, {wakeTimer}) == 0);
    assert(call(167, {47, 2, 8}) == 0 && call(167, {47, 1, 8}) == 1);
    assert(machine.store64(caps, 0x300'0000'0100ULL));
    assert(call(91, {base + 512, caps}) == uint64_t(-1));
    assert(call(90, {base + 512, caps}) == 0 && load(caps) == 0x100'0000'0100ULL);

    const std::array<uint8_t, 1> apexExecutable{0};
    linux.mount("/system/bin/apexd", apexExecutable);
    linux.setBootContext("u:r:init:s0");
    const auto attr = "/proc/" + std::to_string(call(172, {})) + "/attr/current";
    put(base, attr.c_str());
    const auto labelFd = call(56, {uint32_t(-100), base, 0});
    assert(call(63, {labelFd, base + 512, 128}) == sizeof("u:r:init:s0"));
    assert(std::memcmp(machine.read(base + 512, sizeof("u:r:init:s0"), 4),
                       "u:r:init:s0", sizeof("u:r:init:s0")) == 0);
    assert(call(57, {labelFd}) == 0);
    put(base, "/proc/4294967295/attr/current");
    assert(call(56, {uint32_t(-100), base, 0}) == uint64_t(-2));
    put(base, "servicemanager-worker");
    assert(call(167, {15, base}) == 0 && call(167, {16, base + 512}) == 0);
    assert(std::memcmp(machine.read(base + 512, 16, 4), "servicemanager-", 16) == 0);
    assert(linux.loadBootTransition("/system/bin/apexd u:r:init:s0 u:object_r:apexd_exec:s0 2 u:r:apexd:s0"));
    assert(linux.loadPropertyPolicy(
        "100 1\nu:r:init:s0 u:object_r:build_prop:s0 1 ffffffff 0 ffffffff 1 0\n"
        "u:r:apexd:s0 u:object_r:build_prop:s0 0 ffffffff 0 ffffffff 1 0\n"));
    put(base, "/sys/fs/selinux/create");
    const auto create = call(56, {uint32_t(-100), base, 2});
    const char transition[] = "u:r:init:s0 u:object_r:apexd_exec:s0 2";
    put(base + 300, transition);
    assert(call(64, {create, base + 300, sizeof(transition)}) == sizeof(transition));
    assert(call(63, {create, base + 512, 128}) == sizeof("u:r:apexd:s0"));
    assert(std::memcmp(machine.read(base + 512, sizeof("u:r:apexd:s0"), 4),
                       "u:r:apexd:s0", sizeof("u:r:apexd:s0")) == 0);
    assert(call(57, {create}) == 0);
    put(base, "/sys/fs/selinux/access");
    const auto access = call(56, {uint32_t(-100), base, 2});
    const char denial[] = "u:r:apexd:s0 u:object_r:build_prop:s0 100 1";
    put(base + 300, denial);
    assert(call(64, {access, base + 300, sizeof(denial) - 1}) == sizeof(denial) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "0 ffffffff 0 ffffffff 1 0", 25) == 0);
    const char unknown[] = "u:r:unknown:s0 u:object_r:build_prop:s0 100 1";
    put(base + 300, unknown);
    assert(call(64, {access, base + 300, sizeof(unknown) - 1}) == uint64_t(-95));

    assert(linux.loadServicePolicy("101 1 2 3\nu:r:servicemanager:s0 "
                                   "u:object_r:service_manager_service:s0 3 ffffffff 0 ffffffff 1 0\n", false));
    const char permitted[] = "u:r:servicemanager:s0 u:object_r:service_manager_service:s0 101 1";
    put(base + 300, permitted);
    assert(call(64, {access, base + 300, sizeof(permitted) - 1}) == sizeof(permitted) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "3 ffffffff 0 ffffffff 1 0", 25) == 0);

    assert(linux.loadKeystorePolicy(
        "104 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17\nu:r:vold:s0 u:r:keystore:s0 60 ffffffff 0 ffffffff 1 0\n"
        "u:r:shell:s0 u:r:keystore:s0 0 ffffffff 0 ffffffff 1 0\n"));
    put(base, "/sys/fs/selinux/class/keystore2/index");
    const auto keyClass = call(56, {uint32_t(-100), base, 0});
    assert(keyClass < 256 && call(63, {keyClass, base + 512, 8}) == 3);
    assert(std::memcmp(machine.read(base + 512, 3, 4), "104", 3) == 0);
    assert(call(57, {keyClass}) == 0);
    const char maintenance[] = "u:r:vold:s0 u:r:keystore:s0 104 40";
    put(base + 300, maintenance);
    assert(call(64, {access, base + 300, sizeof(maintenance) - 1}) == sizeof(maintenance) - 1);
    assert(call(63, {access, base + 512, 128}) == 26);
    assert(std::memcmp(machine.read(base + 512, 26, 4), "60 ffffffff 0 ffffffff 1 0", 26) == 0);
    const char shellMaintenance[] = "u:r:shell:s0 u:r:keystore:s0 104 40";
    put(base + 300, shellMaintenance);
    assert(call(64, {access, base + 300, sizeof(shellMaintenance) - 1}) == sizeof(shellMaintenance) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "0 ffffffff 0 ffffffff 1 0", 25) == 0);
    const char unrelated[] = "u:r:vold:s0 u:r:keystore:s0 104 20000";
    put(base + 300, unrelated);
    assert(call(64, {access, base + 300, sizeof(unrelated) - 1}) == uint64_t(-95));

    assert(linux.loadKeystorePolicy(
        "105 1 2 3 4 5 6 7 8 9 10 11\nu:r:odsign:s0 u:object_r:odsign_key:s0 24a ffffffff 0 ffffffff 1 0\n"
        "u:r:shell:s0 u:object_r:odsign_key:s0 0 ffffffff 0 ffffffff 1 0\n", true));
    const char signingKey[] = "u:r:odsign:s0 u:object_r:odsign_key:s0 105 40";
    put(base + 300, signingKey);
    assert(call(64, {access, base + 300, sizeof(signingKey) - 1}) == sizeof(signingKey) - 1);
    assert(call(63, {access, base + 512, 128}) == 27);
    assert(std::memcmp(machine.read(base + 512, 27, 4), "24a ffffffff 0 ffffffff 1 0", 27) == 0);
    const char otherKey[] = "u:r:shell:s0 u:object_r:odsign_key:s0 105 40";
    put(base + 300, otherKey);
    assert(call(64, {access, base + 300, sizeof(otherKey) - 1}) == sizeof(otherKey) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "0 ffffffff 0 ffffffff 1 0", 25) == 0);

    constexpr uint64_t loopConfig = base + 0xb00;
    std::array<uint8_t, 8192> package{};
    package[4096] = 0x53;
    package[4097] = 0xef;
    linux.mount("/system/apex/test.apex", package);
    put(base + 200, "/dev/loop-control");
    put(base + 240, "/dev/block/loop0");
    put(base + 280, "/system/apex/test.apex");
    const auto control = call(56, {uint32_t(-100), base + 200, 2});
    constexpr uint64_t getUserQuota = 0x80000700;
    assert(call(60, {getUserQuota, base + 240, 0, base + 512}) == uint64_t(-2));
    assert(call(60, {getUserQuota, 0xdead, 0, base + 512}) == uint64_t(-14));
    assert(call(60, {getUserQuota | 3, base + 240, 0, base + 512}) == uint64_t(-22));
    assert(call(29, {control, 0x4c82}) == 0);
    assert(call(60, {getUserQuota, base + 240, 0, base + 512}) == uint64_t(-19));
    const auto block = call(56, {uint32_t(-100), base + 240, 2});
    const auto backing = call(56, {uint32_t(-100), base + 280, 0x10000});
    assert(control < 256 && block < 256 && backing < 256);
    assert(machine.store64(loopConfig, (uint64_t{4096} << 32) | backing));
    assert(machine.store64(loopConfig + 32, 4096)); // lo_offset
    assert(machine.store64(loopConfig + 40, 8192)); // size would exceed backing file
    assert(call(29, {block, 0x4c0a, loopConfig}) == uint64_t(-22));
    assert(machine.store64(loopConfig + 40, 4096));
    assert(call(29, {block, 0x4c0a, loopConfig}) == 0);
    put(base + 400, "/sys/block/loop0/loop/backing_file");
    const auto backingName = call(56, {uint32_t(-100), base + 400, 0});
    assert(backingName < 256);

    assert(call(57, {backing}) == 0);
    assert(call(29, {block, 0x80081272, base + 512}) == 0 && load(base + 512) == 4096);
    assert(call(29, {block, 0x1262, 256}) == 0);
    assert(call(67, {block, base + 512, 2, 0}) == 2 && (load(base + 512) & 0xffff) == 0xef53);
    assert(call(67, {block, base + 512, 2, 4096}) == 0);
    put(base + 340, "/test/invalid-apex");
    put(base + 370, "ext4");
    assert(call(34, {uint32_t(-100), base + 340, 0755}) == 0);
    assert(call(40, {base + 240, base + 340, base + 370, 1, 0}) == uint64_t(-22));
    assert(call(29, {block, 0x4c01}) == 0);
    assert(call(56, {uint32_t(-100), base + 400, 0}) == uint64_t(-2));
    constexpr std::string_view expectedBacking = "/system/apex/test.apex\n";
    assert(call(63, {backingName, base + 512, 128}) == expectedBacking.size());
    assert(std::memcmp(machine.read(base + 512, expectedBacking.size(), 4),
                       expectedBacking.data(), expectedBacking.size()) == 0);
    assert(call(57, {backingName}) == 0);

    assert(call(67, {block, base + 512, 2, 0}) == uint64_t(-6));

    linux.mount("/vendor/apex/test.apex", package);
    linux.mount("/vendor/other/test.apex", package);
    for (const auto& [path, expected] : {std::pair{"/vendor/other/test.apex", -95},
                                         std::pair{"/vendor/apex/test.apex", 0}}) {
        put(base + 280, path);
        const auto image = call(56, {uint32_t(-100), base + 280, 0x10000});
        assert(image < 256 && machine.store64(loopConfig, (uint64_t{4096} << 32) | image));
        assert(call(29, {block, 0x4c0a, loopConfig}) == uint64_t(expected));
        assert(call(57, {image}) == 0);
    }
    assert(call(67, {block, base + 512, 2, 0}) == 2 && (load(base + 512) & 0xffff) == 0xef53);
    assert(call(29, {block, 0x4c01}) == 0);

    put(base + 200, "/dev/ptmx");
    put(base + 230, "/dev/pts/0");
    const auto master = call(56, {uint32_t(-100), base + 200, 0x80802});
    assert(master < 256);
    assert(call(29, {master, 0x80045430, base + 512}) == 0);
    assert(call(56, {uint32_t(-100), base + 230, 2}) == uint64_t(-5));
    assert(machine.store64(base + 512, 0));
    assert(call(29, {master, 0x40045431, base + 512}) == 0);
    const auto slave = call(56, {uint32_t(-100), base + 230, 2});
    assert(slave < 256 && call(63, {master, base + 512, 20}) == uint64_t(-11));
    put(base + 260, "line\n");
    assert(call(64, {slave, base + 260, 5}) == 5);
    assert(machine.store64(base + 512, (uint64_t{1} << 32) | master));
    assert(machine.store64(base + 600, 0) && machine.store64(base + 608, 0));
    assert(call(73, {base + 512, 1, base + 600}) == 1 && load(base + 512) >> 48 == 1);
    assert(machine.store64(base + 600, 2'147'483));
    assert(call(73, {base + 512, 1, base + 600}) == 1 && load(base + 512) >> 48 == 1);
    assert(machine.store64(base + 600, 0));
    assert(call(63, {master, base + 512, 20}) == 6);
    assert(std::memcmp(machine.read(base + 512, 6, 4), "line\r\n", 6) == 0);
    assert(call(57, {slave}) == 0);
    assert(machine.store64(base + 512, (uint64_t{1} << 32) | master));
    assert(call(73, {base + 512, 1, base + 600}) == 1 && load(base + 512) >> 48 == 16);
    assert(call(63, {master, base + 512, 20}) == uint64_t(-5));

    put(base + 200, "/test/owner");
    assert(call(34, {uint32_t(-100), base + 200, 0755}) == 0);
    assert(call(54, {uint32_t(-100), base + 200, 1000, 1001, 0x100}) == 0);
    assert(call(53, {uint32_t(-100), base + 200, 0710}) == 0);
    assert(call(79, {uint32_t(-100), base + 200, base + 512, 0}) == 0);
    assert(load(base + 528) == (uint64_t{1} << 32 | 0040710));
    assert(load(base + 536) == (uint64_t{1001} << 32 | 1000));

    const auto uevent = call(198, {16, 2 | 0x80800, 15});
    assert(uevent < 256);
    assert(machine.store64(base + 512, 4));
    assert(call(209, {uevent, 1, 39, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 16);
    assert(call(209, {uevent, 1, 38, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 15);

    assert(machine.store64(base + 0xc00, 16));
    assert(machine.store64(base + 0xc08, 1));
    assert(call(200, {uevent, base + 0xc00, 12}) == 0);
    assert(call(208, {uevent, 1, 16, base + 0xc08, 4}) == 0);
    assert(call(212, {uevent, base + 0xc80, 0}) == uint64_t(-11));
    assert(call(29, {control, 0x4c80, 1}) == 1);
    assert(machine.store64(base + 0xc60, base + 0xd00));
    assert(machine.store64(base + 0xc68, 256));
    const std::array<uint64_t, 7> message{base + 0xc10, 12, base + 0xc60, 1, base + 0xc30, 32, 0};
    assert(machine.write(base + 0xc80, {reinterpret_cast<const uint8_t*>(message.data()), sizeof(message)}));
    const auto received = call(212, {uevent, base + 0xc80, 0});
    assert(received > 100 && received < 256);
    assert(load(base + 0xc10) == 16 && (load(base + 0xc18) & 0xffffffff) == 1);
    assert(load(base + 0xc30) == 28 && load(base + 0xc38) == (uint64_t{2} << 32 | 1));
    assert(load(base + 0xc40) == 0 && load(base + 0xc48) == 0);
    const std::string_view packet(reinterpret_cast<const char*>(machine.read(base + 0xd00, received, 4)), received);
    assert(packet.starts_with("add@/devices/virtual/block/loop1"));
    assert(packet.find("DEVNAME=loop1") != std::string_view::npos);
    assert(call(212, {uevent, base + 0xc80, 0}) == uint64_t(-11));

    put(base + 200, "/proc/sys/vm/mmap_rnd_bits");
    const auto bits = call(56, {uint32_t(-100), base + 200, 2});
    assert(bits < 256);
    put(base + 260, "33\n");
    assert(call(64, {bits, base + 260, 3}) == uint64_t(-22));
    put(base + 260, "24\n");
    assert(call(64, {bits, base + 260, 3}) == 3);
    assert(call(62, {bits, 0, 0}) == 0);
    assert(call(63, {bits, base + 512, 16}) == 3);
    assert(std::memcmp(machine.read(base + 512, 3, 4), "24\n", 3) == 0);

    assert(call(167, {27}) == 0);
    assert(call(167, {28, 48}) == 0);
    assert(call(167, {27}) == 48 && call(167, {7}) == 1);
    assert(call(167, {8, 0}) == uint64_t(-1));
    assert(call(167, {28, 0}) == uint64_t(-1));
    assert(call(167, {27}) == 48);

    {
        const auto endpoint = call(198, {1, 1, 0});
        constexpr auto value = base + 0x300, length = base + 0x310;
        assert(endpoint < 256 && machine.store64(length, 8));
        assert(call(209, {endpoint, 1, 13, value, length}) == 0 && load(value) == 0 && load(length) == 8);
        assert(machine.store64(value, 2 | (uint64_t{7} << 32)));
        assert(call(208, {endpoint, 1, 13, value, 4}) == uint64_t(-22));
        assert(call(208, {endpoint, 1, 13, value, 8}) == 0);
        assert(call(209, {endpoint, 1, 13, value, length}) == 0 && load(value) == (1 | (uint64_t{7} << 32)));
        assert(machine.store64(length, 4));
        assert(call(209, {endpoint, 1, 13, value, length}) == 0 && load(length) == 4);
        assert(call(57, {endpoint}) == 0);
    }

    {
        ProbeLinux guest(1, 0);
        constexpr uint64_t directory = base + 0x40, fsName = base + 0x80, device = base + 0xc0;
        constexpr uint64_t watch = base + 0x100, events = base + 0x120;
        constexpr uint64_t commands = base + 0x180, args = base + 0x240, data = base + 0x300;
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> values) {
            std::ranges::copy(values, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto write = [&](uint64_t address, const auto& value) {
            assert(task.write(address, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)}));
        };
        write(directory, "/dev/binder-watch");
        write(fsName, "binder");
        write(device, "/dev/binder-watch/binder");
        assert(invoke(34, {uint32_t(-100), directory, 0755}) == 0);
        assert(invoke(40, {fsName, directory, fsName}) == 0);
        guest.setBootContext("u:r:servicemanager:s0");
        const auto manager = invoke(56, {uint32_t(-100), device, 2});
        assert(manager < 256 && invoke(29, {manager, 0x40046207}) == 0);
        assert(invoke(222, {0, 8 * pageSize, 1, 0x4002, manager}) < uint64_t(-4095));
        write(commands, uint32_t{0x630c}); // BC_ENTER_LOOPER.
        write(args, std::array<uint64_t, 6>{4, 0, commands});
        assert(invoke(29, {manager, 0xc0306201, args}) == 0);

        const auto binderPoller = invoke(20, {0});
        const auto eventPoller = invoke(20, {0});
        const auto event = invoke(19, {0, 0});
        assert(binderPoller < 256 && eventPoller < 256 && event < 256);
        write(watch, std::array<uint64_t, 2>{1, 0xb1});
        assert(invoke(21, {binderPoller, 1, manager, watch}) == 0);
        assert(invoke(21, {eventPoller, 1, event, watch}) == 0);

        const auto waiter = invoke(220, {0x3d0f00, base + 0x900, base + 0x400, 0, base + 0x408});
        const auto client = invoke(220, {0x1200011, 0, 0, 0, base + 0x410});
        assert(waiter == 2 && client == 3);
        invoke(22, {binderPoller, events, 1, UINT64_MAX});
        assert(guest.blocked() && guest.schedule(task) && guest.threadId() == waiter);
        invoke(22, {eventPoller, events, 1, UINT64_MAX});
        assert(guest.blocked() && guest.schedule(task) && guest.processId() == client);

        guest.setBootContext("u:r:vold:s0");
        const auto channel = invoke(56, {uint32_t(-100), device, 2});
        assert(channel < 256);
        write(commands, uint32_t{0x40406300}); // BC_TRANSACTION.
        write(commands + 4, std::array<uint64_t, 8>{0, 0, (uint64_t{1} << 32) | 77, 0, 1, 0, data, 0});
        write(data, uint8_t{1});
        write(args, std::array<uint64_t, 6>{68, 0, commands});
        assert(invoke(29, {channel, 0xc0306201, args}) == 0);
        const auto report = guest.workerReport(task, 1);
        assert(report.find("  guest task 1/1   runnable") != std::string::npos);
        assert(report.find("  guest task 1/2   poll syscall=22") != std::string::npos);
        task.cpu.retired += 4096;
        assert(guest.schedule(task) && guest.threadId() == 1);
        assert(invoke(22, {binderPoller, events, 1, 0}) == 1);
        uint64_t returned = 0;
        assert(task.load64(events + 8, returned) && returned == 0xb1);
    }

    put(base + 200, "/dev/binder-abi");
    put(base + 230, "binder");
    put(base + 260, "/dev/binder-abi/binder");
    assert(call(34, {uint32_t(-100), base + 200, 0755}) == 0);
    assert(call(40, {base + 230, base + 200, base + 230, 0, 0}) == 0);
    assert(call(43, {base + 260, base + 512}) == 0 && load(base + 512) == 0x6c6f6f70);
    const auto manager = call(56, {uint32_t(-100), base + 260, 2});
    assert(manager < 256);
    assert(call(29, {manager, 0xc0046209, base + 512}) == 0 && uint32_t(load(base + 512)) == 8);
    assert(call(29, {manager, 0x40046207}) == uint64_t(-1));
    linux.setBootContext("u:r:servicemanager:s0");
    assert(call(29, {manager, 0x40046207}) == 0);
    assert(call(29, {manager, 0x40046207}) == uint64_t(-16));
    const auto managerMap = call(222, {0, 8 * pageSize, 1, 0x4002, manager, 0});
    assert(managerMap >= 0x100000000 && managerMap < 0x2000000000);
    assert(!machine.store64(managerMap, 0));
    assert(call(226, {managerMap, pageSize, 3}) == uint64_t(-13));

    const auto bindRequest = [&](uint64_t channel, uint32_t operation, uint64_t tag,
                                 std::string_view message, bool receive,
                                 std::span<const uint64_t> offsets = {}, uint64_t handle = 0,
                                 uint64_t extraSize = 0, uint64_t cookie = 0) {
        constexpr auto command = base + 0x600, arguments = base + 0x690, output = base + 0x720;
        constexpr auto payload = base + 0x800, offsetList = base + 0x850;
        const std::array<uint64_t, 9> callData{handle, cookie, tag, UINT64_MAX,
                                               message.size(), offsets.size_bytes(), payload, offsetList, extraSize};
        assert(machine.write(command, {reinterpret_cast<const uint8_t*>(&operation), 4}));
        const auto size = (operation >> 16) & 0x3fff;
        if (size) assert(machine.write(command + 4, {reinterpret_cast<const uint8_t*>(callData.data()), size}));
        if (!message.empty())
            assert(machine.write(payload, {reinterpret_cast<const uint8_t*>(message.data()), message.size()}));
        if (!offsets.empty())
            assert(machine.write(offsetList, {reinterpret_cast<const uint8_t*>(offsets.data()), offsets.size_bytes()}));
        const std::array<uint64_t, 6> request{operation ? size + 4 : 0, 0, command,
                                              receive ? 128U : 0U, 0, output};
        assert(machine.write(arguments, {reinterpret_cast<const uint8_t*>(request.data()), sizeof request}));
        return call(29, {channel, 0xc0306201, arguments});
    };
    machine.cpu.pc = base;
    const auto binderChild = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    machine.cpu.pc += 4;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    linux.setBootContext("u:r:vold:s0");
    assert(call(29, {manager, 0xc0046209, base + 512}) == uint64_t(-1));
    const auto channel = call(56, {uint32_t(-100), base + 260, 2});
    constexpr uint64_t clientMapSize = 128 * 1024;
    const auto clientMap = call(222, {0, clientMapSize, 1, 0x4002, channel, 0});
    assert(channel < 256 && clientMap != managerMap);
    constexpr std::u16string_view interfaceName = u"test.binder.IExample";
    constexpr std::array<uint32_t, 4> interfaceHeader{0, UINT32_MAX, 0x53595354, interfaceName.size()};
    std::string request(reinterpret_cast<const char*>(interfaceHeader.data()), sizeof interfaceHeader);
    request.append(reinterpret_cast<const char*>(interfaceName.data()),
                   (interfaceName.size() + 1) * sizeof(char16_t));
    assert(bindRequest(channel, 0x40406300, 42, request, true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    constexpr uint64_t oneway = uint64_t{1} << 32;
    assert(bindRequest(channel, 0x40406300, oneway | 41, "unrelated", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
    assert(linux.workerReport(machine, binderChild).find("binder-target=1 code=42") != std::string::npos);
    assert(linux.schedule(machine) && linux.processId() == 1);
    const auto unrelatedEvent = call(19, {0, 0});
    assert(unrelatedEvent < 256 && machine.store64(base + 512, 1));
    assert(call(64, {unrelatedEvent, base + 512, 8}) == 8);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(call(57, {unrelatedEvent}) == 0);
    assert(machine.store64(base + 512, 1) && call(29, {manager, 0x40046205, base + 512}) == 0);
    assert(bindRequest(manager, 0x630c, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x720d);
    assert(uint32_t(load(base + 0x724)) == 0x80407202);
    const auto transaction = machine.read(base + 0x728, 64, 4);
    assert(transaction);
    std::array<uint64_t, 8> fields{};
    std::memcpy(fields.data(), transaction, sizeof fields);
    assert(fields[2] == 42 && fields[3] == binderChild && fields[4] == request.size() && fields[6] == managerMap);
    assert(std::memcmp(machine.read(managerMap, request.size(), 4), request.data(), request.size()) == 0);
    const auto report = linux.workerReport(machine, binderChild);
    assert(report.find("binder-target=1 code=42 binder-server-tid=1") != std::string::npos);
    assert(report.find("interface=test.binder.IExample") != std::string::npos);
    assert(call(25, {manager, 4, 0x800}) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == uint64_t(-11));
    assert(bindRequest(manager, 0x40406301, 0, "reply", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(manager, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407202 && uint32_t(load(base + 0x734)) == 41);
    const auto unrelatedBuffer = load(base + 0x724 + 6 * 8);
    assert(std::memcmp(machine.read(unrelatedBuffer, 9, 4), "unrelated", 9) == 0);
    assert(bindRequest(manager, 0x40086303, 0, "", false, {}, unrelatedBuffer) == 0);
    assert(call(25, {manager, 4, 0}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407203);
    assert(std::memcmp(machine.read(clientMap, 5, 4), "reply", 5) == 0);
    {
        constexpr uint32_t transactionSg = 0x40486311, replySg = 0x40486312;
        constexpr uint64_t bufferType = 0x70742a85, childFlag = uint64_t{1} << 32;
        constexpr uint64_t parentSource = base + 0xa00, childSource = base + 0xa10;
        constexpr std::array<uint64_t, 2> offsets{0, 40};
        std::array<uint64_t, 10> buffers{bufferType, parentSource, 16, 0, 0,
                                        bufferType | childFlag, childSource, 8, 0, 8};
        const std::string_view bytes(reinterpret_cast<const char*>(buffers.data()), sizeof buffers);
        assert(machine.store64(parentSource, 0xabc) && machine.store64(parentSource + 8, childSource));
        assert(machine.store64(childSource, 0x12345678));
        assert(bindRequest(channel, transactionSg, 45, bytes, true, offsets, 0, 16) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7211);
        buffers[9] = 12;
        assert(bindRequest(channel, transactionSg, 45, bytes, true, offsets, 0, 24) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7211);
        buffers[9] = 8;
        assert(bindRequest(channel, transactionSg, 45, bytes, true, offsets, 0, 24) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        const auto packet = load(base + 0x724 + 6 * 8);
        const auto parent = load(packet + 8), child = load(packet + 48);
        assert(parent == packet + sizeof(buffers) + sizeof(offsets) && child == parent + 16);
        assert(parent != parentSource && child != childSource);
        assert(load(parent) == 0xabc && load(parent + 8) == child && load(child) == 0x12345678);
        assert(bindRequest(manager, replySg, 0, "sg reply", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, packet) == 0);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == binderChild);
        assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
        const auto response = load(base + 0x724 + 6 * 8);
        assert(std::memcmp(machine.read(response, 8, 4), "sg reply", 8) == 0);
        assert(load(parentSource + 8) == childSource);
    }
    constexpr uint32_t flatBinder = 0x73622a85, flatHandle = 0x73682a85;
    constexpr uint64_t objectPtr = base + 0x990, objectCookie = base + 0x9b0;
    constexpr std::array<uint64_t, 3> object{flatBinder, objectPtr, objectCookie};
    constexpr std::array<uint64_t, 1> objectOffset{0}, invalidOffset{4};
    const std::string_view objectBytes(reinterpret_cast<const char*>(object.data()), sizeof object);
    assert(bindRequest(channel, 0x40406300, 43, objectBytes, true, invalidOffset) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7211);
    assert(bindRequest(channel, 0x40406300, 44, objectBytes, true, objectOffset) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80107207 && load(base + 0x724) == objectPtr);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80107208);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407202);
    const auto receivedBinderBuffer = load(base + 0x724 + 6 * 8);
    const auto handle = load(receivedBinderBuffer + 8);
    assert(uint32_t(load(receivedBinderBuffer)) == flatHandle && handle && load(receivedBinderBuffer + 16) == 0);
    assert(load(load(base + 0x724 + 7 * 8)) == 0);
    assert(bindRequest(manager, 0x40046305, 0, "", false, {}, handle) == 0);
    // binder_handle_cookie is packed: the cookie follows a 32-bit handle.
    assert(bindRequest(manager, 0x400c630e, 0, "", false, {}, handle) == 0);
    assert(bindRequest(manager, 0x40406300, 46, "callback", true, {}, handle) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(manager, 0, 0, "", true) == manager && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407202);
    assert(load(base + 0x724) == objectPtr && uint32_t(load(base + 0x734)) == 46);
    const auto callback = load(base + 0x724 + 6 * 8);
    assert(std::memcmp(machine.read(callback, 8, 4), "callback", 8) == 0);
    assert(bindRequest(channel, 0x40406301, 0, "callback reply", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
    const auto callbackReply = load(base + 0x724 + 6 * 8);
    assert(std::memcmp(machine.read(callbackReply, 14, 4), "callback reply", 14) == 0);
    assert(bindRequest(manager, 0x40086303, 0, "", false, {}, callbackReply) == 0);
    const std::array<uint64_t, 3> proxy{flatHandle, handle, 0};
    const std::string_view proxyBytes(reinterpret_cast<const char*>(proxy.data()), sizeof proxy);
    assert(bindRequest(manager, 0x40406301, 0, proxyBytes, true, objectOffset) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(manager, 0x40086303, 0, "", false, {}, receivedBinderBuffer) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
    const auto roundTrip = load(base + 0x724 + 6 * 8);
    assert(std::memcmp(machine.read(roundTrip, sizeof object, 4), object.data(), sizeof object) == 0);
    assert(bindRequest(channel, 0x630c, 0, "", false) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0x40406300, 47, "outbound", true, {}, handle) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0x40406300, oneway | 48, "new work", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(channel, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407202 && uint32_t(load(base + 0x734)) == 47);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(call(25, {manager, 4, 0x800}) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == uint64_t(-11));
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0x40406301, 0, "outbound reply", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
    const auto outboundReply = load(base + 0x724 + 6 * 8);
    assert(bindRequest(manager, 0x40086303, 0, "", false, {}, outboundReply) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407202 && uint32_t(load(base + 0x734)) == 48);
    const auto newWork = load(base + 0x724 + 6 * 8);
    assert(bindRequest(manager, 0x40086303, 0, "", false, {}, newWork) == 0);
    assert(call(25, {manager, 4, 0}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    {
        constexpr uint64_t fixedFd = 100;
        constexpr uint64_t fileType = 0x66642a85, acceptFiles = uint64_t{0x10} << 32;
        const std::array<uint64_t, 3> file{fileType, fixedFd, 1};
        const std::string_view fileBytes(reinterpret_cast<const char*>(file.data()), sizeof file);
        assert(call(24, {channel, fixedFd, 0}) == fixedFd);
        for (const bool accepts : {false, true}) {
            const auto tag = uint64_t{46} | (accepts ? acceptFiles : 0);
            assert(bindRequest(channel, 0x40406300, tag, "request a file", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x7206);
            assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
            assert(linux.schedule(machine) && linux.processId() == 1);
            assert(bindRequest(manager, 0, 0, "", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x80407202);
            const auto requestBuffer = load(base + 0x724 + 6 * 8);
            const auto origin = call(19, {29, 0}); // eventfd2
            assert(origin < fixedFd && call(24, {origin, fixedFd, 0}) == fixedFd);
            assert(bindRequest(manager, 0x40406301, 0, fileBytes, true, objectOffset) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x7206);
            assert(call(57, {fixedFd}) == 0 && call(57, {origin}) == 0);
            assert(bindRequest(manager, 0x40086303, 0, "", false, {}, requestBuffer) == 0);
            machine.cpu.retired += 4096;
            assert(linux.schedule(machine) && linux.processId() == binderChild);
            assert(bindRequest(channel, 0, 0, "", true) == 0);
            if (accepts) {
                assert(uint32_t(load(base + 0x720)) == 0x80407203);
                const auto response = load(base + 0x724 + 6 * 8);
                const auto received = load(response + 8);
                assert(load(response) == fileType && received < fixedFd);
                assert(call(25, {received, 1}) == 1); // F_GETFD, FD_CLOEXEC
                assert(bindRequest(channel, 0x40086303, 0, "", false, {}, response) == 0);
                assert(call(63, {received, base + 0x900, 8}) == 8 && load(base + 0x900) == 29);
                assert(call(57, {received}) == 0);
            } else {
                assert(uint32_t(load(base + 0x720)) == 0x7211);
            }
        }
        assert(call(57, {fixedFd}) == 0);

        assert(bindRequest(channel, 0x40406300, acceptFiles | 46, "input channel", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        const auto requestBuffer = load(base + 0x724 + 6 * 8);
        constexpr uint64_t io = base + 0x900;
        assert(call(199, {1, 5 | 0x800, 0, io}) == 0);
        const auto server = uint32_t(load(io)), client = uint32_t(load(io) >> 32);
        assert(call(24, {client, fixedFd, 0}) == fixedFd);
        assert(bindRequest(manager, 0x40406301, 0, fileBytes, true, objectOffset) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(call(57, {client}) == 0 && call(57, {fixedFd}) == 0);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, requestBuffer) == 0);
        assert(machine.store64(io, 0x12345678));
        assert(call(206, {server, io, 8, 0x4040}) == 8);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == binderChild);
        assert(bindRequest(channel, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407203);
        const auto response = load(base + 0x724 + 6 * 8);
        const auto received = load(response + 8);
        assert(load(response) == fileType);
        assert(call(25, {received, 1}) == 1 && (call(25, {received, 3}) & 0x800));
        assert(bindRequest(channel, 0x40086303, 0, "", false, {}, response) == 0);
        assert(call(207, {received, io, 8, 0x40}) == 8 && load(io) == 0x12345678);
        assert(call(207, {received, io, 8, 0x40}) == uint64_t(-11));
        assert(machine.store64(io, 0xaabbccdd) && call(206, {received, io, 8, 0x4040}) == 8);
        assert(call(57, {received}) == 0);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(call(207, {server, io, 8, 0x40}) == 8 && load(io) == 0xaabbccdd);
        assert(call(207, {server, io, 8, 0x40}) == 0 && call(57, {server}) == 0);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == binderChild);
    }
    {
        constexpr size_t payloadLength = 72 * 1024, objectCount = 80;
        std::vector<uint8_t> payload(payloadLength, 0xa5);
        std::array<uint64_t, objectCount> offsets{};
        const std::array<uint64_t, 3> nullObject{flatBinder, 0, 0};
        for (size_t i = 0; i < offsets.size(); ++i) {
            offsets[i] = i * sizeof(nullObject);
            std::memcpy(payload.data() + offsets[i], nullObject.data(), sizeof(nullObject));
        }
        for (const auto length : {uint64_t(payloadLength), clientMapSize}) {
            assert(bindRequest(channel, 0x40406300, 50, "large reply", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x7206);
            assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
            assert(linux.schedule(machine) && linux.processId() == 1);
            assert(bindRequest(manager, 0, 0, "", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x80407202);
            const auto requestBuffer = load(base + 0x724 + 6 * 8);
            constexpr uint64_t regionSize = 2 * clientMapSize;
            const auto region = call(222, {0, regionSize, 3, 0x22, UINT32_MAX});
            assert(region < INT64_MAX);
            const auto payloadAt = region + pageSize - 40;
            const auto offsetsAt = region + clientMapSize - 32;
            const auto command = region + clientMapSize + pageSize - 8;
            const std::array<uint64_t, 8> transaction{0, 0, 0, 0, length, sizeof offsets, payloadAt, offsetsAt};
            constexpr uint32_t reply = 0x40406301;
            assert(machine.write(payloadAt, payload));
            assert(machine.write(offsetsAt, {reinterpret_cast<const uint8_t*>(offsets.data()), sizeof offsets}));
            assert(machine.write(command, {reinterpret_cast<const uint8_t*>(&reply), sizeof reply}));
            assert(machine.write(command + 4, {reinterpret_cast<const uint8_t*>(transaction.data()), sizeof transaction}));
            assert(call(226, {region + pageSize, pageSize, 1}) == 0);
            assert(call(226, {region + clientMapSize, pageSize, 1}) == 0);
            assert(!machine.read(payloadAt, payloadLength, 4) && !machine.read(offsetsAt, sizeof offsets, 4));
            assert(!machine.read(command + 4, sizeof transaction, 4));
            constexpr uint64_t arguments = base + 0x690, output = base + 0x720;
            const std::array<uint64_t, 6> exchange{sizeof reply + sizeof transaction, 0, command, 128, 0, output};
            assert(machine.write(arguments, {reinterpret_cast<const uint8_t*>(exchange.data()), sizeof exchange}));
            assert(call(29, {manager, 0xc0306201, arguments}) == 0 && uint32_t(load(output)) == 0x7206);
            assert(bindRequest(manager, 0x40086303, 0, "", false, {}, requestBuffer) == 0);
            assert(call(215, {region, regionSize}) == 0);
            machine.cpu.retired += 4096;
            assert(linux.schedule(machine) && linux.processId() == binderChild);
            assert(bindRequest(channel, 0, 0, "", true) == 0);
            if (length == payloadLength) {
                assert(uint32_t(load(output)) == 0x80407203);
                assert(load(output + 4 + 4 * 8) == length);
                const auto buffer = load(output + 4 + 6 * 8);
                const auto list = load(output + 4 + 7 * 8);
                assert(std::memcmp(machine.read(buffer, payload.size(), 4), payload.data(), payload.size()) == 0);
                assert(std::memcmp(machine.read(list, sizeof offsets, 4), offsets.data(), sizeof offsets) == 0);
                assert(bindRequest(channel, 0x40086303, 0, "", false, {}, buffer) == 0);
            } else {
                assert(uint32_t(load(output)) == 0x7211);
            }
        }
    }
    {
        const std::string payload(64, 'a');
        const auto queued = 8 * pageSize / 2 / payload.size();
        constexpr uint32_t synchronousCode = 4000;
        constexpr uint32_t reuseCode = 4001;
        for (size_t index = 0; index < queued; ++index) {
            assert(bindRequest(channel, 0x40406300, oneway | (100 + index), payload, true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x7206);
        }
        std::string capacityReport;
        linux.androidLogObserver = [&](std::string_view message) { capacityReport = message; };
        assert(bindRequest(channel, 0x40406300, oneway | (100 + queued), payload, true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7211);
        linux.androidLogObserver = {};
        assert(capacityReport.find("allocation-failure=async-quota") != std::string::npos);
        assert(capacityReport.find("largest-oneway-queue=" + std::to_string(queued - 1)) != std::string::npos);
        assert(capacityReport.find("queued-code=101") != std::string::npos);
        assert(bindRequest(channel, 0x40406300, synchronousCode, "synchronous", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
        assert(linux.schedule(machine) && linux.processId() == 1);
        const auto takeOneWay = [&](size_t index) {
            assert(bindRequest(manager, 0, 0, "", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x80407202);
            assert(uint32_t(load(base + 0x724 + 2 * 8)) == 100 + index);
            const auto buffer = load(base + 0x724 + 6 * 8);
            assert(std::memcmp(machine.read(buffer, payload.size(), 4), payload.data(), payload.size()) == 0);
            return buffer;
        };
        const auto firstBuffer = takeOneWay(0);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        assert(uint32_t(load(base + 0x724 + 2 * 8)) == synchronousCode); // Same-node oneways wait for the first buffer.
        const auto requestBuffer = load(base + 0x724 + 6 * 8);
        assert(bindRequest(manager, 0x40406301, 0, "response", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, requestBuffer) == 0);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, firstBuffer) == 0);
        for (size_t index = 1; index < queued; ++index) {
            const auto buffer = takeOneWay(index);
            assert(bindRequest(manager, 0x40086303, 0, "", false, {}, buffer) == 0);
        }
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == binderChild);
        assert(bindRequest(channel, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407203);
        assert(bindRequest(channel, 0x40086303, 0, "", false, {}, load(base + 0x724 + 6 * 8)) == 0);
        assert(bindRequest(channel, 0x40406300, oneway | reuseCode, payload, true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        assert(uint32_t(load(base + 0x724 + 2 * 8)) == reuseCode);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, load(base + 0x724 + 6 * 8)) == 0);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == binderChild);
    }
    const auto binderWorker = call(220, {0x3d0f00, base + 0x900, base + 0x810, base + 0xa00, base + 0x810});
    assert(binderWorker > binderChild);
    call(93, {0}); // The leader exits, leaving its Binder connection alive in the worker.
    assert(linux.schedule(machine) && linux.processId() == binderChild && linux.threadId() == binderWorker);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0x40406300, 78, "pending at exit", true, {}, handle) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(manager, 0, 0, "", true) == manager && linux.blocked());
    assert(linux.schedule(machine) && linux.threadId() == binderWorker);
    assert(bindRequest(channel, 0x630c, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407202 && uint32_t(load(base + 0x734)) == 78);
    call(94, {0});
    assert(linux.schedule(machine) && linux.processId() == 1);

    assert(bindRequest(manager, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x8008720f && load(base + 0x724) == 0);
    assert(bindRequest(manager, 0x40086310, 0, "", false) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x7205);
    assert(bindRequest(manager, 0x40046306, 0, "", false, {}, handle) == 0);

    put(base + 260, "/dev/binder-abi/binder");
    const auto registeringPeer = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(registeringPeer > binderChild);
    assert(linux.schedule(machine) && linux.processId() == registeringPeer);
    linux.setBootContext("u:r:vold:s0");
    const auto registeringBinder = call(56, {uint32_t(-100), base + 260, 2});
    assert(registeringBinder < 256);
    assert(call(222, {0, 8 * pageSize, 1, 0x4002, registeringBinder, 0}) > 0x100000000);
    uint64_t closingHandle = 0;
    uint64_t weakHandle = 0;
    for (uint64_t index = 0; index < 320; ++index) {
        const auto pointer = index == 1 ? objectPtr : objectPtr + index * 8;
        const auto cookie = objectCookie + index * 8;
        const std::array<uint64_t, 3> service{flatBinder, pointer, cookie};
        const std::string_view bytes(reinterpret_cast<const char*>(service.data()), sizeof service);
        constexpr uint64_t registerOneWay = (uint64_t{1} << 32) | 48;
        assert(bindRequest(registeringBinder, 0x40406300, registerOneWay, bytes, true, objectOffset) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80107207); // BR_INCREFS
        assert(bindRequest(registeringBinder, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80107208); // BR_ACQUIRE
        if (index == 0) {
            assert(bindRequest(registeringBinder, 0x40106308, 0, "", false, {}, pointer, 0, cookie) == 0);
            assert(bindRequest(registeringBinder, 0x40106309, 0, "", false, {}, pointer, 0, cookie) == 0);
            assert(bindRequest(registeringBinder, 0x630c, 0, "", false) == 0);
        }
        assert(bindRequest(registeringBinder, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206); // BR_TRANSACTION_COMPLETE
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        const auto serviceBuffer = load(base + 0x724 + 6 * 8);
        const auto serviceHandle = load(serviceBuffer + 8);
        assert(serviceHandle && uint32_t(load(serviceBuffer)) == flatHandle);
        assert(bindRequest(manager, 0x40046305, 0, "", false, {}, serviceHandle) == 0);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, serviceBuffer) == 0);
        if (index == 0) {
            weakHandle = serviceHandle;
            assert(bindRequest(manager, 0x40046304, 0, "", false, {}, weakHandle) == 0);
            assert(bindRequest(manager, 0x40046306, 0, "", false, {}, weakHandle) == 0);
            machine.cpu.retired += 4096;
            assert(linux.schedule(machine) && linux.processId() == registeringPeer);
            assert(bindRequest(registeringBinder, 0, 0, "", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x80107209); // BR_RELEASE
            assert(load(base + 0x724) == pointer && load(base + 0x72c) == cookie);
            machine.cpu.retired += 4096;
            assert(linux.schedule(machine) && linux.processId() == 1);
            assert(bindRequest(manager, 0x40046307, 0, "", false, {}, weakHandle) == 0);
        }
        if (index + 1 < 320) {
            machine.cpu.retired += 4096;
            assert(linux.schedule(machine) && linux.processId() == registeringPeer);
            if (index == 0) {
                assert(bindRequest(registeringBinder, 0, 0, "", true) == 0);
                assert(uint32_t(load(base + 0x720)) == 0x8010720a); // BR_DECREFS
                assert(load(base + 0x724) == pointer && load(base + 0x72c) == cookie);
            }
        } else {
            closingHandle = serviceHandle;
        }
    }
    assert(bindRequest(manager, 0x400c630e, 0, "", false, {}, closingHandle) == 0);
    assert(bindRequest(manager, 0x40406300, 77, "pending reply", true, {}, closingHandle) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == registeringPeer);
    assert(bindRequest(registeringBinder, 0x630c, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407202 && uint32_t(load(base + 0x734)) == 77);
    const auto duplicatedBinder = call(25, {registeringBinder, 0, 0});
    assert(duplicatedBinder != registeringBinder && duplicatedBinder < 256);
    assert(call(57, {registeringBinder}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(call(25, {manager, 4, 0x800}) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == uint64_t(-11));
    assert(call(25, {manager, 4, 0}) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == manager && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == registeringPeer);
    assert(call(57, {duplicatedBinder}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(call(129, {registeringPeer, 0}) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x8008720f);
    assert(bindRequest(manager, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7205); // The held Binder call lost its peer.
    assert(call(25, {manager, 4, 0x800}) == 0);
    assert(bindRequest(manager, 0, 0, "", true) == uint64_t(-11));
    assert(call(25, {manager, 4, 0}) == 0);
    assert(call(129, {registeringPeer, 9}) == 0);

    {
        // The owning service must also be notified when an importer exits with a live proxy.
        const auto importer = call(220, {0x1200011, 0, 0, 0, base + 0x848});
        assert(importer > registeringPeer);
        assert(linux.schedule(machine) && linux.processId() == importer);
        const auto client = call(56, {uint32_t(-100), base + 260, 2});
        assert(client < 256 && call(222, {0, 8 * pageSize, 1, 0x4002, client, 0}) > 0x100000000);
        assert(bindRequest(client, 0x40406300, 82, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(client, 0, 0, "", true) == client && linux.blocked());
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, load(base + 0x724 + 6 * 8)) == 0);
        assert(bindRequest(manager, 0x40406301, 0, objectBytes, true, objectOffset) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80107207);
        assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80107208);
        assert(bindRequest(manager, 0x40106308, 0, "", false, {}, objectPtr, 0, objectCookie) == 0);
        assert(bindRequest(manager, 0x40106309, 0, "", false, {}, objectPtr, 0, objectCookie) == 0);
        assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x7206);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == importer);
        assert(bindRequest(client, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
        const auto importedBuffer = load(base + 0x724 + 6 * 8);
        const auto importedHandle = load(importedBuffer + 8);
        assert(bindRequest(client, 0x40046305, 0, "", false, {}, importedHandle) == 0);
        assert(bindRequest(client, 0x40086303, 0, "", false, {}, importedBuffer) == 0);
        assert(call(94, {0}) == 0);
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80107209);
        assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x8010720a);
        assert(load(base + 0x724) == objectPtr && load(base + 0x72c) == objectCookie);
    }

    {
        ProbeLinux guest(1, 0);
        Startup startup{};
        startup.filename = "/system/bin/app_process64";
        startup.argv0 = "app_process64";
        startup.arguments = {"--zygote", "--start-system-server"};
        auto task = guest.createMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, startup});
        guest.setCommandLine(startup, &task);
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args) {
            std::ranges::copy(args, task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        uint64_t argv;
        assert(task.load64(task.cpu.sp + sizeof(uint64_t), argv));
        const auto child = invoke(220, {0x1200011, 0, 0, 0, base + 0x500});
        assert(child > 1 && guest.schedule(task) && guest.processId() == child);
        constexpr char name[] = "com.android.launcher3";
        const std::array<uint8_t, 45> empty{};
        assert(task.write(argv, empty));
        assert(task.write(argv, {reinterpret_cast<const uint8_t*>(name), sizeof(name)}));
        assert(invoke(124, {}) == 0); // Yield to the parent before its procfs lookup.
        assert(guest.schedule(task) && guest.processId() == 1);
        const auto open = [&](std::string path) {
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(path.c_str()), path.size() + 1}));
            return invoke(56, {uint32_t(-100), base, 0});
        };
        const auto childPath = "/proc/" + std::to_string(child);
        const auto childArgs = open(childPath + "/cmdline");
        const auto parentArgs = open("/proc/self/cmdline");
        assert(childArgs < 256 && parentArgs < 256 && open(childPath + "/stat") < 256);
        assert(invoke(63, {childArgs, base + 256, 64}) >= sizeof(name));
        assert(std::memcmp(task.read(base + 256, sizeof(name), 4), name, sizeof(name)) == 0);
        assert(invoke(63, {parentArgs, base + 256, 64}) > 13);
        assert(std::memcmp(task.read(base + 256, 13, 4), "app_process64", 13) == 0);
    }

    Startup commandLine{};
    commandLine.filename = "/system/bin/app_process64";
    commandLine.argv0 = "app_process";
    commandLine.arguments = {"--zygote"};
    linux.setCommandLine(commandLine);
    put(base + 0x280, "/proc/self/cmdline");
    const auto commandLineFd = call(56, {uint32_t(-100), base + 0x280, 0});
    constexpr char expectedCommandLine[] = "app_process\0--zygote";
    assert(call(63, {commandLineFd, base + 0x300, 64}) == sizeof expectedCommandLine);
    assert(std::memcmp(machine.read(base + 0x300, sizeof expectedCommandLine, 4),
                       expectedCommandLine, sizeof expectedCommandLine) == 0);
    assert(call(62, {commandLineFd, 0, 0}) == 0);
    assert(call(57, {commandLineFd}) == 0);

    const auto killedChild = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(killedChild > binderChild);
    const auto processHandle = call(434, {killedChild, 0});
    assert(processHandle < 256 && call(25, {processHandle, 1}) == 1);
    const auto duplicateHandle = call(25, {processHandle, 1030, 100});
    assert(duplicateHandle == 100 && call(25, {duplicateHandle, 1}) == 1);
    constexpr auto processPoll = base + 0x910;
    constexpr auto noWaitTimeout = base + 0x930;
    assert(machine.store64(processPoll, duplicateHandle | uint64_t(1) << 32));
    assert(machine.store64(noWaitTimeout, 0) && machine.store64(noWaitTimeout + 8, 0));
    assert(call(73, {processPoll, 1, noWaitTimeout}) == 0);
    assert(call(129, {killedChild, 9}) == 0);
    assert(call(129, {killedChild, 0}) == uint64_t(-3));
    assert(call(73, {processPoll, 1, noWaitTimeout}) == 1 && load(processPoll) >> 48 == 1);
    assert(call(95, {3, processHandle, base + 512, 0x1000005}) == 0);
    assert((load(base + 520) & 0xffffffff) == 2);
    assert(load(base + 528) == killedChild && load(base + 536) == 9);
    assert(call(260, {killedChild, base + 512, 1}) == killedChild && load(base + 512) == 9);
    assert(call(73, {processPoll, 1, noWaitTimeout}) == 1 && load(processPoll) >> 48 == 17);
    assert(call(57, {processHandle}) == 0 && call(57, {duplicateHandle}) == 0);

    const auto abortingChild = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(abortingChild > killedChild);
    assert(linux.schedule(machine) && linux.processId() == abortingChild);
    constexpr uint64_t abortSignal = 6;
    assert(machine.store64(base + 0x820, 1ULL << (abortSignal - 1)));
    assert(call(135, {0, base + 0x820, 0, 8}) == 0);
    assert(machine.store64(signalInfo, abortSignal));
    assert(machine.store64(signalInfo + 8, uint32_t(-6)));
    assert(call(240, {abortingChild, abortingChild, abortSignal, signalInfo}) == 0 && !linux.blocked());
    assert(call(135, {1, base + 0x820, 0, 8}) == 0 && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(call(260, {abortingChild, base + 512, 1}) == abortingChild && load(base + 512) == abortSignal);
    const auto groupLeader = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    const auto groupMember = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(groupLeader > killedChild && groupMember > groupLeader);
    assert(call(154, {groupLeader, groupLeader}) == 0);
    assert(call(154, {groupMember, groupLeader}) == 0);
    assert(call(129, {0 - groupLeader, 15}) == 0);
    assert(call(260, {groupLeader, base + 512, 1}) == groupLeader && load(base + 512) == 15);
    assert(call(260, {groupMember, base + 512, 1}) == groupMember && load(base + 512) == 15);



    {
        namespace fs = std::filesystem;
        auto temporary = (fs::temp_directory_path() / "nestvane-data-XXXXXX").string();
        assert(mkdtemp(temporary.data()));
        const struct Cleanup {
            const fs::path directory;
            ~Cleanup() { std::error_code ignored; fs::remove_all(directory, ignored); }
        } cleanup{temporary};
        const auto database = (cleanup.directory / "guest.sqlite3").string();
        ProbeLinux writer(1, 0), stale(1, 0);
        writer.mountData(database);
        stale.mountData(database);
        const auto invoke = [&](ProbeLinux& guest, uint64_t number, std::array<uint64_t, 6> args) {
            std::copy(args.begin(), args.end(), machine.cpu.x.begin());
            machine.cpu.x[8] = number;
            assert(guest.syscall(machine));
            return machine.cpu.x[0];
        };
        {
            ProbeLinux quota(1, 0);
            quota.mountData((cleanup.directory / "quota.sqlite3").string());
            quota.addBootTmpfs("/metadata", 0755, 0, 6);
            constexpr uint64_t cwd = uint32_t(-100);
            constexpr auto fsStat = base + 0x200;
            const auto dataFreeBlocks = [&] {
                put(base, "/data");
                assert(invoke(quota, 43, {base, fsStat}) == 0);
                const auto freeBlocks = load(fsStat + 3 * sizeof(uint64_t));
                assert(freeBlocks == load(fsStat + 4 * sizeof(uint64_t)));
                return freeBlocks;
            };
            const auto initialFree = dataFreeBlocks();
            for (unsigned i = 0; i < 2048; ++i) {
                const auto path = "/metadata/" + std::to_string(i);
                put(base, path.c_str());
                const auto file = invoke(quota, 56, {cwd, base, 0xc1, 0600});
                assert(file < 256);
                if (i == 0) assert(invoke(quota, 46, {file, 2 * 1024 * 1024}) == 0);
                assert(invoke(quota, 57, {file}) == 0);
            }
            assert(dataFreeBlocks() == initialFree);
            put(base, "/metadata/overflow");
            assert(invoke(quota, 56, {cwd, base, 0xc1, 0600}) == uint64_t(-28));
            put(base, "/data/package");
            const auto persistent = invoke(quota, 56, {cwd, base, 0xc1, 0600});
            assert(persistent < 256 && invoke(quota, 46, {persistent, pageSize}) == 0);
            assert(dataFreeBlocks() + 1 == initialFree);
            assert(invoke(quota, 57, {persistent}) == 0 && quota.syncData());
        }
        {
            ProbeLinux maps(1, 0);
            maps.mountData(":memory:");
            put(base, "/data/large-private-map");
            const auto file = invoke(maps, 56, {uint32_t(-100), base, 0xc2, 0600});
            constexpr uint64_t length = 512 * 1024;
            assert(file < 256 && invoke(maps, 46, {file, length}) == 0);
            assert(machine.store64(base + 64, 42) && invoke(maps, 68, {file, base + 64, 8, 0}) == 8);
            const auto first = invoke(maps, 222, {0, length, 1, 2, file, 0});
            const auto second = invoke(maps, 222, {0, length, 1, 2, file, 0});
            const auto* backing = machine.read(first, 8, 4);
            assert(backing && load(first) == 42 && machine.read(second, 8, 4) == backing);
            assert(invoke(maps, 226, {second, length, 3}) == 0);
            assert(machine.store64(second, 99) && load(first) == 42 && load(second) == 99);

            const auto shared = invoke(maps, 222, {0, length, 3, 1, file, 0});
            assert(machine.store64(shared, 71));
            const auto third = invoke(maps, 222, {0, length, 1, 2, file, 0});
            assert(load(third) == 71 && load(first) == 42 && load(second) == 99);
            for (const auto mapping : {first, second, shared, third})
                assert(invoke(maps, 215, {mapping, length}) == 0);
            assert(invoke(maps, 57, {file}) == 0);
        }
        {
            ProbeLinux installer(1, 0);
            installer.mountData((cleanup.directory / "apk-labels.sqlite3").string());
            constexpr auto cwd = uint32_t(-100);
            constexpr auto path = base, name = base + 0x100, label = base + 0x180;
            constexpr char installed[] = "u:object_r:apk_data_file:s0";
            constexpr char staging[] = "u:object_r:apk_tmp_file:s0";
            put(name, "security.selinux");
            put(label, installed);
            for (const char* directory : {"/data/app", "/data/outside"}) {
                put(path, directory);
                assert(invoke(installer, 34, {cwd, path, 0771}) == 0);
                assert(invoke(installer, 5, {path, name, label, sizeof(installed), 0}) == 0);
                assert(invoke(installer, 54, {cwd, path, 1000, 1000}) == 0);
            }
            put(path, "/data/app/foreign");
            assert(invoke(installer, 34, {cwd, path, 0755}) == 0);
            assert(invoke(installer, 146, {1000}) == 0);
            installer.setBootContext("u:r:system_server:s0");
            put(path, "/data/app/vmdl123.tmp");
            assert(invoke(installer, 34, {cwd, path, 0775}) == 0);
            put(label, staging);
            assert(invoke(installer, 6, {path, name, label, sizeof(staging), 0}) == uint64_t(-1));
            const std::string decisions =
                "u:r:system_server:s0 u:object_r:apk_data_file:s0 180 ffffffff 0 ffffffff 1 0\n"
                "u:r:system_server:s0 u:object_r:apk_tmp_file:s0 180 ffffffff 0 ffffffff 1 0\n";
            assert(installer.loadPackageRelabelPolicy("8 8 9\n" + decisions, true));
            assert(installer.loadPackageRelabelPolicy("6 8 9\n" + decisions, false));
            installer.setBootContext("u:r:shell:s0");
            assert(invoke(installer, 6, {path, name, label, sizeof(staging), 0}) == uint64_t(-1));
            installer.setBootContext("u:r:system_server:s0");
            assert(invoke(installer, 6, {path, name, label, sizeof(staging), 0}) == 0);
            assert(invoke(installer, 9, {path, name, label, 128}) == sizeof(staging));
            assert(std::memcmp(machine.read(label, sizeof(staging), 4), staging, sizeof(staging)) == 0);
            put(path, "/data/app/vmdl123.tmp/base.apk");
            const auto apk = invoke(installer, 56, {cwd, path, 0xc2, 0644});
            assert(apk < 256);
            put(label, installed);
            assert(invoke(installer, 7, {apk, name, label, sizeof(installed), 0}) == 0);
            assert(invoke(installer, 10, {apk, name, label, 128}) == sizeof(installed));
            assert(std::memcmp(machine.read(label, sizeof(installed), 4), installed, sizeof(installed)) == 0);
            constexpr char unrelated[] = "u:object_r:system_file:s0";
            put(label, unrelated);
            assert(invoke(installer, 7, {apk, name, label, sizeof(unrelated), 0}) == uint64_t(-1));
            put(label, staging);
            for (const char* directory : {"/data/app/foreign", "/data/outside"}) {
                put(path, directory);
                assert(invoke(installer, 6, {path, name, label, sizeof(staging), 0}) == uint64_t(-1));
            }
        }
        {
            const auto directoryDatabase = (cleanup.directory / "boot-dirs.sqlite3").string();
            ProbeLinux seed(1, 0);
            seed.mountData(directoryDatabase);
            constexpr uint64_t cwd = uint32_t(-100);
            for (unsigned i = 0; i < 540; ++i) {
                const auto path = "/data/package-" + std::to_string(i);
                put(base, path.c_str());
                assert(invoke(seed, 34, {cwd, base, 0755}) == 0);
            }
            assert(seed.syncData());

            ProbeLinux reboot(1, 0);
            reboot.mountData(directoryDatabase);
            put(base, "bpf");
            put(base + 32, "/sys/fs/bpf");
            assert(invoke(reboot, 40, {base, base + 32, base, 14}) == 0);
            put(base, "/sys/fs/bpf/netd_readonly");
            assert(invoke(reboot, 34, {cwd, base, 0755}) == 0);
        }
        constexpr auto atCwd = uint32_t(-100);
        constexpr auto firstPath = base + 0x280, secondPath = base + 0x300, payload = base + 0x380;

        {
            const auto databasePath = (cleanup.directory / "incremental-data.sqlite3").string();
            ProbeLinux writer(1, 0);
            writer.mountData(databasePath);
            put(firstPath, "/data/unchanged");
            put(secondPath, "/data/mapped");
            assert(machine.store64(payload, 42));
            const auto unchanged = invoke(writer, 56, {atCwd, firstPath, 0xc2, 0600});
            const auto changed = invoke(writer, 56, {atCwd, secondPath, 0xc2, 0600});
            assert(unchanged < 256 && changed < 256);
            assert(invoke(writer, 64, {unchanged, payload, 8}) == 8);
            assert(invoke(writer, 64, {changed, payload, 8}) == 8);
            assert(writer.syncData());

            sqlite3* raw = nullptr;
            assert(sqlite3_open(databasePath.c_str(), &raw) == SQLITE_OK);
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
            assert(sqlite3_exec(raw, "CREATE TABLE file_writes (path TEXT);"
                "CREATE TRIGGER data_update AFTER UPDATE ON entries BEGIN INSERT INTO file_writes VALUES(new.path); END;"
                "CREATE TRIGGER data_delete AFTER DELETE ON entries BEGIN INSERT INTO file_writes VALUES(old.path); END;"
                "CREATE TRIGGER data_insert AFTER INSERT ON entries BEGIN INSERT INTO file_writes VALUES(new.path); END;",
                nullptr, nullptr, nullptr) == SQLITE_OK);

            const auto writes = [&] {
                sqlite3_stmt* query = nullptr;
                assert(sqlite3_prepare_v2(raw, "SELECT path FROM file_writes ORDER BY path",
                                          -1, &query, nullptr) == SQLITE_OK);
                std::vector<std::string> paths;
                int status;
                while ((status = sqlite3_step(query)) == SQLITE_ROW)
                    paths.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(query, 0)));
                assert(status == SQLITE_DONE && sqlite3_finalize(query) == SQLITE_OK);
                return paths;
            };
            assert(writer.syncData() && writes().empty());
            const auto shared = invoke(writer, 222, {0, pageSize, 3, 1, changed, 0});
            assert(machine.store64(shared, 99) && writer.syncData());
            assert(writes() == std::vector<std::string>{"/data/mapped"});

            ProbeLinux reopened(1, 0);
            reopened.mountData(databasePath);
            const auto file = invoke(reopened, 56, {atCwd, secondPath, 0});
            assert(file < 256 && invoke(reopened, 63, {file, payload, 8}) == 8 && load(payload) == 99);
            assert(invoke(writer, 215, {shared, pageSize}) == 0);
        }

        {
            const auto attrDatabase = (cleanup.directory / "file-xattrs.sqlite3").string();
            ProbeLinux owner(1, 0);
            owner.mountData(attrDatabase);
            constexpr auto key = base + 0x440, copied = base + 0x480;
            constexpr char uuid[] = "13af50b6-5dc1-4c75-85f5-6607f157dd27";
            put(firstPath, "/data/media.db.tmp");
            put(secondPath, "/data/media.db");
            put(payload, uuid);
            put(key, "user.uuid");
            const auto descriptor = invoke(owner, 56, {atCwd, firstPath, 0xc2, 0600});
            assert(descriptor < 256 && invoke(owner, 55, {descriptor, 10032, 10032}) == 0);
            assert(invoke(owner, 146, {10032}) == 0);
            assert(invoke(owner, 5, {firstPath, key, payload, sizeof(uuid) - 1, 2}) == uint64_t(-61));
            assert(invoke(owner, 5, {firstPath, key, payload, sizeof(uuid) - 1, 1}) == 0);
            assert(invoke(owner, 7, {descriptor, key, payload, sizeof(uuid) - 1, 1}) == uint64_t(-17));
            assert(invoke(owner, 10, {descriptor, key, copied, 0}) == sizeof(uuid) - 1);
            assert(invoke(owner, 38, {atCwd, firstPath, atCwd, secondPath}) == uint64_t(-13));
            assert(owner.syncData());

            // Rename as the directory owner, then reopen the same file as its application UID.
            ProbeLinux rename(1, 0);
            rename.mountData(attrDatabase);
            assert(invoke(rename, 38, {atCwd, firstPath, atCwd, secondPath}) == 0 && rename.syncData());
            ProbeLinux reader(1, 10032), otherApp(1, 10033);
            reader.mountData(attrDatabase);
            otherApp.mountData(attrDatabase);
            assert(invoke(reader, 8, {secondPath, key, copied, sizeof(uuid)}) == sizeof(uuid) - 1);
            assert(std::memcmp(machine.read(copied, sizeof(uuid) - 1, 4), uuid, sizeof(uuid) - 1) == 0);
            assert(invoke(otherApp, 8, {secondPath, key, copied, sizeof(uuid)}) == uint64_t(-13));
            assert(invoke(otherApp, 5, {secondPath, key, payload, 1, 0}) == uint64_t(-13));
            assert(invoke(reader, 5, {secondPath, key, 0, 0, 2}) == 0);
            assert(invoke(reader, 8, {secondPath, key, copied, sizeof(uuid)}) == 0);
            assert(invoke(reader, 14, {secondPath, key}) == 0);
            assert(invoke(reader, 8, {secondPath, key, copied, sizeof(uuid)}) == uint64_t(-61));
        }
        {
            const auto cacheDatabase = (cleanup.directory / "package-cache.sqlite3").string();
            ProbeLinux cache(1, 0);
            cache.mountData(cacheDatabase);
            put(firstPath, "/data/package-cache");
            put(payload, "parsed package");
            const auto entry = invoke(cache, 56, {atCwd, firstPath, 0x42, 0600});
            constexpr auto stat = base + 0x480;
            assert(entry < 256 && invoke(cache, 80, {entry, stat}) == 0);
            assert(load(stat + 88) > 0 && load(stat + 104) == load(stat + 88));
            assert(invoke(cache, 64, {entry, payload, 14}) == 14);
            assert(invoke(cache, 80, {entry, stat}) == 0 && load(stat + 48) == 14);
            const auto modified = load(stat + 88), modifiedNanos = load(stat + 96);
            assert(modified > 0 && modifiedNanos < 1000000000);
            assert(invoke(cache, 82, {entry}) == 0);
            ProbeLinux reopened(1, 0);
            reopened.mountData(cacheDatabase);
            assert(invoke(reopened, 79, {atCwd, firstPath, stat, 0}) == 0);
            assert(load(stat + 88) == modified && load(stat + 96) == modifiedNanos);

            // Explicitly make the file old and verify a truncate refreshes its modification time.
            constexpr auto times = base + 0x600;
            assert(machine.store64(times, 100) && machine.store64(times + 8, 0));
            assert(machine.store64(times + 16, 100) && machine.store64(times + 24, 0));
            assert(invoke(reopened, 88, {atCwd, firstPath, times, 0}) == 0);
            const auto truncated = invoke(reopened, 56, {atCwd, firstPath, 0x202, 0});
            assert(truncated < 256 && invoke(reopened, 80, {truncated, stat}) == 0);
            assert(load(stat + 48) == 0 && load(stat + 88) > 100);

            assert(machine.store64(payload, 0x12345678));
            assert(invoke(reopened, 64, {truncated, payload, 8}) == 8);
            assert(invoke(reopened, 47, {truncated, 0, 2, 1}) == 0);
            assert(invoke(reopened, 80, {truncated, stat}) == 0 && load(stat + 48) == 8);
            assert(invoke(reopened, 47, {truncated, 0, 4096, 32}) == 0);
            assert(invoke(reopened, 80, {truncated, stat}) == 0 && load(stat + 48) == 4128);
            assert(invoke(reopened, 62, {truncated, 0, 1}) == 8);
            assert(invoke(reopened, 67, {truncated, payload, 16, 0}) == 16);
            assert(load(payload) == 0x12345678 && load(payload + 8) == 0);
            assert(invoke(reopened, 47, {truncated, 0, INT64_MAX, 1}) == uint64_t(-27));
            assert(invoke(reopened, 47, {truncated, 0, 0, 0}) == uint64_t(-22));
            assert(invoke(reopened, 47, {truncated, 1, 0, 8}) == uint64_t(-95));
            const auto readOnly = invoke(reopened, 56, {atCwd, firstPath, 0});
            assert(readOnly < 256 && invoke(reopened, 47, {readOnly, 0, 0, 8}) == uint64_t(-9));
            constexpr uint64_t sharedPopulated = 0x8001;
            assert(invoke(reopened, 222, {0, pageSize, 3, sharedPopulated, readOnly, 0}) == uint64_t(-13));
            const auto mapped = invoke(reopened, 222, {0, pageSize, 1, sharedPopulated, readOnly, 0});
            assert(mapped < uint64_t(-4095) && load(mapped) == 0x12345678 && load(mapped + 8) == 0);
            assert(invoke(reopened, 226, {mapped, pageSize, 3}) == uint64_t(-13));
            assert(invoke(reopened, 215, {mapped, pageSize}) == 0);
        }
        {
            const auto journalDatabase = (cleanup.directory / "journal.sqlite3").string();
            ProbeLinux guest(1, 0);
            guest.mountData(journalDatabase);
            put(firstPath, "/data/rollback-journal");
            const auto file = invoke(guest, 56, {atCwd, firstPath, 0x42, 0600});
            const auto mapping = invoke(guest, 222, {0, 2 * pageSize, 3, 0x22, UINT32_MAX});
            assert(file < 256 && mapping < INT64_MAX);
            const auto buffer = mapping + pageSize - 128;
            constexpr size_t length = 4096;
            std::array<uint8_t, length> data{};
            for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i % 251);
            assert(machine.write(buffer, data));
            assert(invoke(guest, 226, {mapping + pageSize, pageSize, 0}) == 0);
            assert(invoke(guest, 68, {file, buffer, length, 25140}) == uint64_t(-14));
            assert(invoke(guest, 226, {mapping + pageSize, pageSize, 1}) == 0);
            assert(!machine.read(buffer, length, 4));
            assert(invoke(guest, 68, {file, buffer, length, 25140}) == length);
            assert(invoke(guest, 82, {file}) == 0);
            ProbeLinux reopened(1, 0);
            reopened.mountData(journalDatabase);
            const auto persisted = invoke(reopened, 56, {atCwd, firstPath, 0});
            assert(persisted < 256 && invoke(reopened, 67, {persisted, mapping, length, 25140}) == length);
            std::array<uint8_t, length> saved{};
            assert(machine.readInto(mapping, saved) && saved == data);
            assert(invoke(guest, 215, {mapping, 2 * pageSize}) == 0);
        }
        {
            const auto aclDatabase = (cleanup.directory / "acl.sqlite3").string();
            const auto name = base + 0x710, value = base + 0x750, output = base + 0x7a0;
            // AOSP vold writes owner/group IDs even though Linux ignores them on these ACL entries.
            const std::array<uint32_t, 11> defaults{
                2, 0x70001, 1023, 4, 1023, 0x70008, 1023, 0x70010, 0, 32, 0};
            const auto aclBytes = std::as_bytes(std::span(defaults));
            assert(machine.write(value, {reinterpret_cast<const uint8_t*>(aclBytes.data()), aclBytes.size()}));
            {
                ProbeLinux guest(1, 0);
                guest.mountData(aclDatabase);
                put(firstPath, "/data/acl-media");
                put(name, "system.posix_acl_default");
                assert(invoke(guest, 34, {atCwd, firstPath, 0755}) == 0);
                assert(invoke(guest, 5, {firstPath, name, value, sizeof(defaults) - 1, 0}) == uint64_t(-22));
                assert(invoke(guest, 5, {firstPath, name, value, sizeof(defaults), 1}) == 0);
                assert(invoke(guest, 5, {firstPath, name, value, sizeof(defaults), 1}) == uint64_t(-17));
                assert(invoke(guest, 166, {0077}) <= 0777);
                put(firstPath, "/data/acl-media/child");
                assert(invoke(guest, 34, {atCwd, firstPath, 0770}) == 0);
                assert(invoke(guest, 8, {firstPath, name, output, sizeof(defaults) - 1}) == uint64_t(-34));
                assert(invoke(guest, 8, {firstPath, name, output, sizeof(defaults)}) == sizeof(defaults));
                assert(uint32_t(load(output + 8)) == UINT32_MAX);
                put(firstPath, "/data/acl-media/child/file");
                const auto fd = invoke(guest, 56, {atCwd, firstPath, 0x42, 0660});
                assert(fd < 256 && invoke(guest, 52, {fd, 0640}) == 0);
                assert(guest.syncData());
            }
            for (const auto gid : {1023U, 1024U}) {
                ProbeLinux guest(1, 0);
                guest.mountData(aclDatabase);
                put(firstPath, "/data/acl-media/child/file");
                put(name, "system.posix_acl_access");
                assert(invoke(guest, 8, {firstPath, name, output, sizeof(defaults)}) == sizeof(defaults));
                assert(invoke(guest, 149, {gid, gid, gid}) == 0);
                assert(invoke(guest, 147, {12000, 12000, 12000}) == 0);
                assert(invoke(guest, 56, {atCwd, firstPath, 2}) == uint64_t(-13));
                const auto readOnly = invoke(guest, 56, {atCwd, firstPath, 0});
                if (gid == 1023) {
                    assert(readOnly < 256);
                    assert(invoke(guest, 7, {readOnly, name, value, sizeof(defaults), 0}) == uint64_t(-1));
                } else {
                    assert(readOnly == uint64_t(-13));
                }
            }
        }
        assert(writer.loadFsCreatePolicy(
            "2 19\nu:r:init:s0 u:r:init:s0 40000 ffffffff 0 ffffffff 1 0\n"
            "u:r:shell:s0 u:r:shell:s0 0 ffffffff 0 ffffffff 1 0\n"));
        assert(writer.loadSeappContexts(
            "# reference seapp policy\nuser=_app domain=untrusted_app type=app_data_file levelFrom=all\n"));
        put(firstPath, "/sys/fs/selinux/context");
        const auto validate = invoke(writer, 56, {atCwd, firstPath, 2});
        assert(validate < 256);
        const auto contextCheck = [&](const char* value, bool allowed) {
            put(payload, value);
            const auto length = std::strlen(value) + 1;
            assert(invoke(writer, 64, {validate, payload, length}) == (allowed ? length : uint64_t(-22)));
        };
        contextCheck("u:object_r:app_data_file:s0:c123,c256,c512,c768", true);
        contextCheck("u:object_r:app_data_file:s0:c0.c3,c768", true);
        contextCheck("u:object_r:app_data_file:s0:c4,c4", false);
        contextCheck("u:object_r:app_data_file:s0:c1024", false);
        contextCheck("u:object_r:unknown_data_file:s0:c512", false);
        assert(invoke(writer, 57, {validate}) == 0);

        put(firstPath, "/data");
        put(payload, "security.selinux");
        constexpr char rootLabel[] = "u:object_r:system_data_root_file:s0";
        assert(invoke(writer, 9, {firstPath, payload, base + 0x480, 128}) == sizeof(rootLabel));
        assert(std::memcmp(machine.read(base + 0x480, sizeof(rootLabel), 4), rootLabel, sizeof(rootLabel)) == 0);
        assert(invoke(writer, 43, {firstPath, base + 0x450}) == 0);
        const auto quotaBlocks = load(base + 0x460);
        assert(quotaBlocks == (128 * 1024 * 1024) / pageSize);
        assert(load(base + 0x468) == quotaBlocks && load(base + 0x470) == quotaBlocks);
        constexpr auto markerName = base + 0x530, markerValue = base + 0x590;
        put(markerName, "user.restorecon_in_progress");
        assert(machine.store64(markerValue, 1));
        constexpr auto legacyPath = base + 0x510;
        put(legacyPath, "/data/legacy");
        assert(invoke(writer, 34, {atCwd, legacyPath, 0755}) == 0);
        assert(invoke(writer, 6, {legacyPath, payload, 0, 0, 0}) == 0);
        assert(invoke(writer, 14, {legacyPath, markerName}) == uint64_t(-61));
        assert(invoke(writer, 5, {legacyPath, markerName, markerValue, 1, 1}) == 0);
        assert(invoke(writer, 5, {legacyPath, markerName, markerValue, 1, 1}) == uint64_t(-17));
        assert(invoke(writer, 8, {legacyPath, markerName, 0, 0}) == 1);
        const auto markerDirectory = invoke(writer, 56, {atCwd, legacyPath, 0x4000});
        assert(markerDirectory < 256);
        assert(invoke(writer, 16, {markerDirectory, markerName}) == 0);
        assert(invoke(writer, 10, {markerDirectory, markerName, 0, 0}) == uint64_t(-61));
        assert(invoke(writer, 7, {markerDirectory, markerName, markerValue, 1, 0}) == 0);
        assert(invoke(writer, 57, {markerDirectory}) == 0);
        put(firstPath, "/proc/thread-self/attr/fscreate");
        const auto labelFd = invoke(writer, 56, {atCwd, firstPath, 2});
        assert(labelFd < 256);
        constexpr char context[] = "u:object_r:system_data_file:s0:c512,c768";
        put(payload, context);
        writer.setBootContext("u:r:shell:s0");
        assert(invoke(writer, 64, {labelFd, payload, sizeof(context)}) == uint64_t(-13));
        writer.setBootContext("u:r:init:s0");
        assert(invoke(writer, 64, {labelFd, payload, sizeof(context)}) == sizeof(context));
        assert(invoke(writer, 57, {labelFd}) == 0);
        put(firstPath, "/data/private");
        assert(invoke(writer, 34, {atCwd, firstPath, 0700}) == 0);
        constexpr auto labelName = base + 0x450;
        put(labelName, "security.selinux");
        put(payload, context);
        assert(invoke(writer, 6, {firstPath, labelName, payload, sizeof(context), 1}) == uint64_t(-17));
        assert(invoke(writer, 6, {firstPath, labelName, payload, sizeof(context), 2}) == 0);
        put(payload, "/proc/thread-self/attr/fscreate");
        const auto clearLabel = invoke(writer, 56, {atCwd, payload, 2});
        assert(clearLabel < 256 && invoke(writer, 64, {clearLabel, 0, 0}) == 0);
        assert(invoke(writer, 57, {clearLabel}) == 0);
        constexpr auto cachePath = base + 0x6c0, cacheChildPath = base + 0x700, cacheFilePath = base + 0x740;
        constexpr auto cacheStat = base + 0x780;
        put(cachePath, "/data/cache-gid");
        put(cacheChildPath, "/data/cache-gid/child");
        put(cacheFilePath, "/data/cache-gid/child/file");
        assert(invoke(writer, 34, {atCwd, cachePath, 0771}) == 0);
        assert(invoke(writer, 54, {atCwd, cachePath, 1000, 20014}) == 0);
        assert(invoke(writer, 53, {atCwd, cachePath, 02771}) == 0);
        const auto cache = invoke(writer, 56, {atCwd, cachePath, 0x4000});
        assert(cache < 256 && invoke(writer, 52, {cache, 03771}) == 0);
        assert(invoke(writer, 80, {cache, cacheStat}) == 0 && (load(cacheStat + 16) & 07777) == 03771);
        assert(invoke(writer, 57, {cache}) == 0);
        assert(invoke(writer, 34, {atCwd, cacheChildPath, 0700}) == 0);
        assert(invoke(writer, 79, {atCwd, cacheChildPath, cacheStat, 0}) == 0);
        assert((load(cacheStat + 16) & 02777) == 02700 && (load(cacheStat + 24) >> 32) == 20014);
        const auto inherited = invoke(writer, 56, {atCwd, cacheFilePath, 0x42, 0600});
        assert(inherited < 256 && invoke(writer, 80, {inherited, cacheStat}) == 0);
        assert((load(cacheStat + 24) >> 32) == 20014);
        assert(invoke(writer, 57, {inherited}) == 0);
        constexpr auto storageName = base + 0x820, inodeName = base + 0x850, attrBytes = base + 0x880;
        put(storageName, "user.default");
        put(inodeName, "user.inode_cache");
        assert(invoke(writer, 8, {cachePath, storageName, 0, 0}) == uint64_t(-61));
        assert(invoke(writer, 5, {cachePath, storageName, 0, 0, 1}) == 0);
        assert(invoke(writer, 5, {cachePath, storageName, 0, 0, 1}) == uint64_t(-17));
        assert(invoke(writer, 8, {cachePath, storageName, 0, 0}) == 0);
        const auto attrDir = invoke(writer, 56, {atCwd, cachePath, 0x4000});
        assert(attrDir < 256 && machine.store64(attrBytes, 0x4321));
        assert(invoke(writer, 7, {attrDir, inodeName, attrBytes, 8, 2}) == uint64_t(-61));
        assert(invoke(writer, 7, {attrDir, inodeName, attrBytes, 8, 1}) == 0);
        assert(invoke(writer, 10, {attrDir, inodeName, attrBytes + 8, 4}) == uint64_t(-34));
        assert(invoke(writer, 10, {attrDir, inodeName, attrBytes + 8, 8}) == 8);
        assert(load(attrBytes + 8) == 0x4321 && invoke(writer, 57, {attrDir}) == 0);
        constexpr auto aliasPath = base + 0x690;
        put(aliasPath, "/data/legacy-link");
        put(payload, context);
        assert(invoke(writer, 36, {legacyPath, atCwd, aliasPath}) == 0);
        assert(invoke(writer, 6, {aliasPath, labelName, payload, sizeof(context), 0}) == 0);
        assert(invoke(writer, 54, {atCwd, firstPath, 1000, 1000}) == 0);
        assert(invoke(writer, 146, {1000}) == 0);
        put(firstPath, "/data/private/draft");
        put(secondPath, "/data/private/current");
        const auto saved = invoke(writer, 56, {atCwd, firstPath, 0xc2, 0600});
        assert(saved < 256 && machine.store64(payload, 0xdecafbad12345678ULL));
        assert(invoke(writer, 64, {saved, payload, 8}) == 8);
        constexpr auto fileTimes = base + 0x5a0, fileStat = base + 0x600;
        assert(machine.store64(fileTimes, 1'700'000'000) && machine.store64(fileTimes + 8, 234));
        assert(machine.store64(fileTimes + 16, 1'700'001'000) && machine.store64(fileTimes + 24, 545));
        assert(invoke(writer, 88, {saved, 0, fileTimes, 0}) == 0);
        assert(invoke(writer, 80, {saved, fileStat}) == 0);
        assert(load(fileStat + 72) == 1'700'000'000 && load(fileStat + 80) == 234);
        assert(load(fileStat + 88) == 1'700'001'000 && load(fileStat + 96) == 545);
        assert(machine.store64(fileTimes + 8, 0x3ffffffe));
        assert(machine.store64(fileTimes + 24, 555));
        assert(invoke(writer, 88, {atCwd, firstPath, fileTimes, 0}) == 0);
        assert(machine.store64(fileTimes + 24, 0x3ffffffe));
        assert(invoke(writer, 88, {atCwd, 0xbad, fileTimes, 0}) == 0);
        assert(invoke(writer, 38, {atCwd, firstPath, atCwd, secondPath}) == 0);
        put(firstPath, "/data/private");
        const auto activeDirectory = invoke(writer, 56, {atCwd, firstPath, 0x4000});
        assert(activeDirectory < 256);
        put(secondPath, "/data/moved");
        assert(invoke(writer, 38, {atCwd, firstPath, atCwd, secondPath}) == 0);
        put(payload, "current");
        const auto moved = invoke(writer, 56, {activeDirectory, payload, 0});
        assert(moved < 256 && invoke(writer, 57, {moved}) == 0);
        assert(invoke(writer, 57, {activeDirectory}) == 0);
        assert(invoke(writer, 82, {saved}) == 0);
        assert(!stale.syncData());
        ProbeLinux reopened(1, 1000), other(1, 2000);
        reopened.mountData(database);
        other.mountData(database);
        assert(invoke(reopened, 79, {atCwd, cachePath, cacheStat, 0}) == 0);
        assert((load(cacheStat + 16) & 07777) == 03771 && (load(cacheStat + 24) >> 32) == 20014);
        assert(invoke(reopened, 8, {cachePath, storageName, 0, 0}) == 0);
        assert(invoke(reopened, 8, {cachePath, inodeName, attrBytes + 8, 8}) == 8);
        assert(load(attrBytes + 8) == 0x4321);
        assert(invoke(other, 14, {cachePath, storageName}) == uint64_t(-1));
        assert(invoke(reopened, 8, {cachePath, storageName, 0, 0}) == 0);
        put(payload, "security.selinux");
        assert(invoke(reopened, 8, {legacyPath, markerName, markerValue, 1}) == 1);
        assert(*machine.read(markerValue, 1, 4) == 1);
        assert(invoke(reopened, 14, {legacyPath, markerName}) == uint64_t(-13));
        constexpr char legacyLabel[] = "u:object_r:unlabeled:s0";
        assert(invoke(reopened, 9, {legacyPath, payload, base + 0x480, 128}) == sizeof(legacyLabel));
        assert(std::memcmp(machine.read(base + 0x480, sizeof(legacyLabel), 4), legacyLabel, sizeof(legacyLabel)) == 0);
        assert(invoke(reopened, 9, {aliasPath, payload, base + 0x480, 128}) == sizeof(context));
        assert(std::memcmp(machine.read(base + 0x480, sizeof(context), 4), context, sizeof(context)) == 0);
        assert(invoke(reopened, 8, {secondPath, payload, base + 0x480, 128}) == sizeof(context));
        assert(std::memcmp(machine.read(base + 0x480, sizeof(context), 4), context, sizeof(context)) == 0);
        put(secondPath, "/data/moved/current");
        assert(invoke(reopened, 9, {secondPath, payload, base + 0x480, 128}) == sizeof(context));
        assert(std::memcmp(machine.read(base + 0x480, sizeof(context), 4), context, sizeof(context)) == 0);
        assert(invoke(other, 9, {secondPath, payload, base + 0x480, 128}) == uint64_t(-13));
        assert(invoke(other, 56, {atCwd, secondPath, 0}) == uint64_t(-13));
        assert(invoke(reopened, 56, {atCwd, firstPath, 0}) == uint64_t(-2));
        const auto restored = invoke(reopened, 56, {atCwd, secondPath, 0});
        assert(restored < 256 && invoke(reopened, 63, {restored, payload, 8}) == 8);
        assert(invoke(reopened, 80, {restored, fileStat}) == 0);
        assert(load(fileStat + 72) == 1'700'000'000 && load(fileStat + 80) == 234);
        assert(load(fileStat + 88) == 1'700'001'000 && load(fileStat + 96) == 555);
        assert(load(payload) == 0xdecafbad12345678ULL);
        assert(invoke(reopened, 35, {atCwd, secondPath, 0}) == 0);
        assert(invoke(reopened, 82, {restored}) == 0);
        ProbeLinux deleted(1, 1000);
        deleted.mountData(database);
        assert(invoke(deleted, 56, {atCwd, secondPath, 0}) == uint64_t(-2));
    }

    put(base + 0x220, "bpf");
    put(base + 0x240, "/sys/fs/bpf");
    put(base + 0x260, "/sys/fs/bpf/netd");
    put(base + 0x280, "/sys/fs/bpf/netd/regular-file");
    assert(call(40, {base + 0x220, base + 0x240, base + 0x220, 14}) == 0);
    assert(call(43, {base + 0x240, base + 0x2a0}) == 0);
    assert(load(base + 0x2a0) == 0xcafe4a11);
    assert(call(34, {uint32_t(-100), base + 0x260, 0700}) == 0);
    assert(call(56, {uint32_t(-100), base + 0x280, 0x42, 0600}) == uint64_t(-13));

    put(base + 0x320, "/proc/sys/kernel/unprivileged_bpf_disabled");
    const auto bpfSetting = call(56, {uint32_t(-100), base + 0x320, 2});
    assert(bpfSetting < 256);
    assert(call(63, {bpfSetting, base + 0x360, 2}) == 2);
    assert(std::memcmp(machine.read(base + 0x360, 2, 4), "2\n", 2) == 0);
    assert(call(62, {bpfSetting, 0, 0}) == 0);
    put(base + 0x360, "0\n");
    assert(call(64, {bpfSetting, base + 0x360, 2}) == 2);
    assert(call(62, {bpfSetting, 0, 0}) == 0);
    put(base + 0x360, "1\n");
    assert(call(64, {bpfSetting, base + 0x360, 2}) == 2);
    assert(call(62, {bpfSetting, 0, 0}) == 0);
    put(base + 0x360, "0\n");
    assert(call(64, {bpfSetting, base + 0x360, 2}) == uint64_t(-1));
    assert(call(57, {bpfSetting}) == 0);


    assert(linux.loadFsCreatePolicy(
        "2 19 30\n"
        "u:r:init:s0 u:r:init:s0 20040000 ffffffff 0 ffffffff 1 0\n"
        "u:r:shell:s0 u:r:shell:s0 0 ffffffff 0 ffffffff 1 0\n"));
    assert(!linux.loadSelinuxPolicyVersion("33 32\n"));
    assert(!linux.loadSelinuxPolicyVersion("0\n"));
    assert(linux.loadSelinuxPolicyVersion("33\n"));
    assert(!linux.loadSelinuxPolicyVersion("34\n"));
    put(base + 0x320, "/sys/fs/selinux/policyvers");
    const auto policyVersion = call(56, {uint32_t(-100), base + 0x320, 0});
    assert(policyVersion < 256 && call(63, {policyVersion, base + 0x360, 4}) == 2);
    assert(std::memcmp(machine.read(base + 0x360, 2, 4), "33", 2) == 0);
    assert(call(57, {policyVersion}) == 0);
    assert(call(56, {uint32_t(-100), base + 0x320, 1}) == uint64_t(-30));

    put(base + 0x320, "/proc/thread-self/attr/sockcreate");
    constexpr char zygoteContext[] = "u:r:zygote:s0";
    put(base + 0x360, zygoteContext);
    linux.setBootContext("u:r:shell:s0");
    const auto deniedSocketLabel = call(56, {uint32_t(-100), base + 0x320, 1});
    assert(call(64, {deniedSocketLabel, base + 0x360, sizeof zygoteContext}) == uint64_t(-13));
    assert(call(57, {deniedSocketLabel}) == 0);
    linux.setBootContext("u:r:init:s0");
    const auto socketLabel = call(56, {uint32_t(-100), base + 0x320, 1});
    assert(call(64, {socketLabel, base + 0x360, sizeof zygoteContext}) == sizeof zygoteContext);
    assert(call(57, {socketLabel}) == 0);
    assert(call(199, {1, 1, 0, base + 0x3a0}) == 0);
    const auto labeledPair = load(base + 0x3a0);
    assert(machine.store64(base + 0x390, 32));
    assert(call(209, {uint32_t(labeledPair), 1, 31, base + 0x3c0, base + 0x390}) == 0);
    assert(load(base + 0x390) == sizeof zygoteContext);
    assert(std::memcmp(machine.read(base + 0x3c0, sizeof zygoteContext, 4), zygoteContext, sizeof zygoteContext) == 0);
    const auto socketSender = uint32_t(labeledPair), socketReceiver = uint32_t(labeledPair >> 32);
    assert(machine.store64(base + 0x390, 4) && machine.store64(base + 0x3c0, 3));
    assert(call(208, {socketSender, 1, 2, base + 0x3c0, 3}) == uint64_t(-22));
    assert(call(208, {socketSender, 1, 2, 0xdead, 4}) == uint64_t(-14));
    assert(call(208, {socketSender, 1, 2, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketSender, 1, 2, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 1);
    assert(call(209, {socketReceiver, 1, 2, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 0);
    assert(call(208, {socketSender, 1, 2, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketSender, 1, 2, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 0);
    assert(machine.store64(base + 0x390, 4) && machine.store64(base + 0x3c0, 4096));
    assert(call(208, {socketSender, 1, 7, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketSender, 1, 7, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 8192);
    assert(call(208, {socketReceiver, 1, 8, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketReceiver, 1, 8, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 16384);
    for (unsigned i = 0; i < 8; ++i) assert(call(64, {socketSender, base, 1024}) == 1024);
    assert(call(25, {socketSender, 4, 0x800}) == 0); // O_NONBLOCK
    assert(call(64, {socketSender, base, 1}) == uint64_t(-11));
    assert(call(63, {socketReceiver, base, 512}) == 512);
    assert(call(64, {socketSender, base, 1024}) == 512);
    assert(call(57, {socketSender}) == 0 && call(57, {socketReceiver}) == 0);

    {
        constexpr auto pair = base + 0x480, msg = base + 0x500, vector = base + 0x540;
        constexpr auto control = base + 0x580, payload = base + 0x5c0;
        assert(call(199, {1, 0x801, 0, pair}) == 0);
        const auto sender = uint32_t(load(pair)), receiver = uint32_t(load(pair) >> 32);
        const auto original = call(198, {2, 2, 0});
        assert(original < 256);
        const auto write = [&](auto addr, const auto& value) {
            assert(machine.write(addr, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)}));
        };
        const auto prepare = [&](uint64_t size, uint64_t capacity, uint64_t address = control) {
            write(vector, std::array<uint64_t, 2>{payload, size});
            write(msg, std::array<uint64_t, 7>{0, 0, vector, 1, address, capacity, 0});
        };
        const auto send = [&] {
            prepare(2, 24);
            write(control, std::array<uint64_t, 3>{20, 0x100000001ULL, original});
            assert(machine.store64(payload, 0x5958));
            assert(call(211, {sender, msg, 0}) == 2);
        };
        assert(machine.store64(payload, 0x6261) && call(64, {sender, payload, 2}) == 2);
        send();
        assert(machine.store64(payload, 'z') && call(64, {sender, payload, 1}) == 1);
        prepare(8, 24, 0xdead);
        assert(call(212, {receiver, msg, 0}) == uint64_t(-14));
        prepare(8, 24);
        assert(call(212, {receiver, msg, 0x4000402a}) == 4); // CLOEXEC, PEEK and three ignored flags.
        const auto peeked = uint32_t(load(control + 16));
        assert((load(payload) & 0xffffffff) == 0x59586261 && load(control) == 20 && load(msg + 40) == 24);
        assert(uint32_t(load(msg + 48)) == 0x40000000);
        assert(peeked != original && call(25, {peeked, 1}) == 1);
        prepare(8, 24);
        assert(call(212, {receiver, msg, 0}) == 4);
        const auto transferred = uint32_t(load(control + 16));
        assert(transferred != peeked && transferred != original && call(25, {transferred, 1}) == 0);
        assert(call(63, {receiver, payload, 8}) == 1 && uint8_t(load(payload)) == 'z');
        assert(machine.store64(payload, 1));
        assert(call(208, {transferred, 1, 2, payload, 4}) == 0);
        assert(machine.store64(vector, 4));
        assert(call(209, {original, 1, 2, payload, vector}) == 0 && uint32_t(load(payload)) == 1);
        send();
        prepare(8, 16);
        assert(call(212, {receiver, msg, 0}) == 2 && load(msg + 40) == 0 && uint32_t(load(msg + 48)) == 8);
        for (const auto fd : {sender, receiver, uint32_t(original), peeked, transferred})
            assert(call(57, {fd}) == 0);
    }

    put(base + 0x320, "/dev/urandom");
    const auto entropyFd = call(56, {uint32_t(-100), base + 0x320, 0});
    assert(entropyFd < 256);
    assert(call(63, {entropyFd, base + 0x350, 32}) == 32);
    assert(call(63, {entropyFd, 0xdead, 32}) == uint64_t(-14));
    assert(call(63, {entropyFd, 0, 0}) == 0);
    assert(call(57, {entropyFd}) == 0);
    assert(call(278, {base + 0x350, 16, 4}) == 16); // GRND_INSECURE is valid after initialization too.
    assert(call(278, {base + 0x350, 16, 6}) == uint64_t(-22));

    assert(call(126, {1}) == 1 && call(125, {2}) == 99);
    assert(call(126, {0}) == 0 && call(125, {4}) == uint64_t(-22));
    assert(machine.store64(base + 0x390, 2));
    assert(call(119, {0, 1, base + 0x390}) == uint64_t(-95));
    assert(call(120, {0}) == 0);

    assert(call(163, {7, base + 0x400}) == 0);
    const auto softDescriptorLimit = load(base + 0x400);
    assert(softDescriptorLimit > 256);
    const auto highDescriptor = call(25, {0, 1030, 256});
    assert(highDescriptor == 256 && call(25, {highDescriptor, 1}) == 1);
    assert(call(24, {highDescriptor, softDescriptorLimit, 0}) == uint64_t(-9));
    assert(call(57, {highDescriptor}) == 0);

    const auto split = call(222, {0, pageSize * 2, 3, 0x22, uint64_t(-1), 0});
    assert(split > base && split < (uint64_t{1} << 45));
    assert(machine.store64(split + pageSize, 42));
    assert(call(226, {split + pageSize, pageSize, 1}) == 0);
    assert(call(216, {split, pageSize * 2, pageSize * 3, 1}) == uint64_t(-14));
    assert(call(226, {split, pageSize * 2, 3}) == 0);
    const auto resized = call(216, {split, pageSize * 2, pageSize * 3, 1});
    assert(resized > base && resized < (uint64_t{1} << 45));
    assert(load(resized + pageSize) == 42 && load(resized + pageSize * 2) == 0);
    assert(call(215, {resized, pageSize * 3}) == 0);

    assert(call(163, {3, base + 0x400}) == 0);
    assert(load(base + 0x400) == guestStackSize && load(base + 0x408) == maxGuestStackSize);
    Startup startup{};
    startup.filename = "/system/bin/app_process64";
    startup.arguments = {"--zygote", "--start-system-server"};
    startup.environment = {"ANDROID_ROOT=/system", "ANDROID_DATA=/data"};
    auto initialStack = linux.createMachine(Image{base, {}, startup});
    constexpr uint64_t stackBottom = 0x8000'0000 - guestStackSize;
    assert(!initialStack.read(stackBottom - 1, 1, 4));
    assert(initialStack.store64(stackBottom, 42));
    const auto stackWord = [&](size_t index) {
        uint64_t value = 0;
        assert(initialStack.load64(initialStack.cpu.sp + index * sizeof(value), value));
        return value;
    };
    assert(stackWord(0) == 3 && stackWord(4) == 0 && stackWord(7) == 0);
    assert(stackWord(1) + startup.filename.size() + 1 == stackWord(2));
    assert(stackWord(2) + startup.arguments[0].size() + 1 == stackWord(3));
    assert(stackWord(3) + startup.arguments[1].size() + 1 == stackWord(5));
    assert(stackWord(5) + startup.environment[0].size() + 1 == stackWord(6));
    uint64_t ticksPerSecond = 0;
    for (size_t index = 8; const auto tag = stackWord(index); index += 2) {
        if (tag == 17) ticksPerSecond = stackWord(index + 1);
    }
    assert(ticksPerSecond == 100);

    put(base + 0x320, "art-heap-test");
    assert(call(279, {base + 0x320, 4}) == uint64_t(-22));
    const auto memoryFile = call(279, {base + 0x320, 1});
    assert(memoryFile < 256 && call(25, {memoryFile, 1}) == 1);
    assert(call(25, {memoryFile, 1034}) == 1);
    assert(call(46, {memoryFile, 3 * 1024 * 1024}) == 0);
    assert(machine.store64(base + 0x400, 0));
    assert(call(62, {memoryFile, pageSize, 0}) == pageSize);
    assert(call(29, {memoryFile, 0x541b, base + 0x400}) == 0 && load(base + 0x400) == 3 * 1024 * 1024 - pageSize);
    assert(call(29, {memoryFile, 0x541b, 0xdead}) == uint64_t(-14));
    const auto mappedMemoryFile = call(222, {0, pageSize, 3, 1, memoryFile, 0});
    assert(mappedMemoryFile > base && mappedMemoryFile < (uint64_t{1} << 45));
    assert(load(mappedMemoryFile) == 0 && machine.store64(mappedMemoryFile, 42));
    assert(call(67, {memoryFile, base + 0x400, 8, 0}) == 8 && load(base + 0x400) == 42);
    constexpr uint64_t lowFileMap = 0x4100'0000;
    assert(call(222, {lowFileMap, pageSize, 0, 0x100022, uint64_t(-1), 0}) == lowFileMap);
    assert(call(222, {lowFileMap, pageSize, 1, 0x12, memoryFile, 0}) == lowFileMap);
    assert(load(lowFileMap) == 42);
    assert(call(226, {lowFileMap, pageSize, 3}) == 0);
    assert(machine.store64(lowFileMap, 19) && load(mappedMemoryFile) == 42);
    assert(call(215, {lowFileMap, pageSize}) == 0);
    assert(call(57, {memoryFile}) == 0 && load(mappedMemoryFile) == 42);
    assert(call(215, {mappedMemoryFile, pageSize}) == 0);

    const auto sealable = call(279, {base + 0x320, 3});
    assert(sealable < 256 && call(25, {sealable, 1034}) == 0);
    assert(call(46, {sealable, pageSize * 2}) == 0);
    const auto writer = call(222, {0, pageSize, 3, 1, sealable, 0});
    assert(writer < (uint64_t{1} << 45));
    assert(call(25, {sealable, 1033, 8}) == uint64_t(-16));
    assert(call(25, {sealable, 1033, 2 | 4 | 16}) == 0);
    assert(call(46, {sealable, pageSize}) == uint64_t(-1));
    assert(call(46, {sealable, pageSize * 3}) == uint64_t(-1));
    assert(call(68, {sealable, base, 8, 0}) == uint64_t(-1));
    assert(call(222, {0, pageSize, 3, 1, sealable, 0}) == uint64_t(-1));
    const auto reader = call(222, {0, pageSize, 1, 1, sealable, 0});
    assert(reader < (uint64_t{1} << 45) && machine.store64(writer, 123) && load(reader) == 123);
    assert(call(226, {reader, pageSize, 3}) == uint64_t(-13));
    assert(call(215, {writer, pageSize}) == 0);
    assert(call(25, {sealable, 1033, 8 | 1}) == 0);
    assert(call(25, {sealable, 1034}) == 31);
    assert(call(25, {sealable, 1033, 0}) == uint64_t(-1));
    assert(call(57, {sealable}) == 0 && call(215, {reader, pageSize}) == 0);

    std::cout << "guest kernel ABI checks passed, including bind views, fork, SIGCHLD, pipe, boot policy, loop bounds, PTY, uevents, guest sysctls, securebits, I/O priority, Binder transactions and persisted guest data\n";
}
