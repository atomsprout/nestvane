#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nestvane {

inline constexpr uint64_t pageSize = 4096;
inline constexpr uint64_t fuel = 2'000'000;
inline constexpr uint64_t guestStackSize = 8 * 1024 * 1024;
inline constexpr uint64_t maxGuestStackSize = 32 * 1024 * 1024;

enum class Op {
    movz, addImm, subImm, addReg, adr, load, store, cbnz, svc, unsupported
};

struct Instruction {
    Op op = Op::unsupported;
    unsigned rd = 0;
    unsigned rn = 0;
    unsigned rm = 0;
    uint64_t immediate = 0;
};

Instruction decode(uint32_t word);
bool endsBlock(Op op);

struct SharedFileBytes {
    std::vector<uint8_t> pages;
    uint64_t size = 0;
};

struct AnonymousBudget {
    uint64_t reserved = 0, dirty = 0;
    uint64_t reservedLimit, dirtyLimit;
};

class AnonymousPages {
public:
    static std::shared_ptr<AnonymousPages> allocate(uint64_t size, const std::shared_ptr<AnonymousBudget>& budget);
    ~AnonymousPages();
    AnonymousPages(const AnonymousPages&) = delete;
    AnonymousPages& operator=(const AnonymousPages&) = delete;

    uint8_t* data() const { return mapping; }
    uint64_t capacity() const { return size; }
    bool prepareWrite(uint64_t offset, uint64_t length);
    bool resident(uint64_t offset) const;
    void discard(uint64_t offset, uint64_t length);
    bool copyTo(AnonymousPages& target, uint64_t source, uint64_t destination, uint64_t length) const;

private:
    AnonymousPages(uint64_t size, const std::shared_ptr<AnonymousBudget>& budget);
    std::shared_ptr<AnonymousBudget> budget;
    uint64_t hostPage, size, dirtyBytes = 0;
    std::vector<bool> dirty;
    uint8_t* mapping = nullptr;
};

struct Segment {
    uint64_t address;
    std::vector<uint8_t> bytes;
    unsigned flags;
    uint64_t reservedSize = 0;
    bool beyondFileEnd = false;

    std::shared_ptr<SharedFileBytes> shared = {};
    uint64_t fileOffset = 0;
    bool anonymous = false;
    bool sharedReadOnly = false;
    bool initialStack = false;
    bool fileMapping = false;
    std::shared_ptr<AnonymousPages> anonymousPages = {};
    bool omitOnFork = false, wipeOnFork = false, sharedAnonymous = false;
    unsigned maxProtection = 7; // Linux PROT bits permitted for this mapping.

    uint64_t mappedSize() const { return reservedSize ? reservedSize : bytes.size(); }
    uint64_t accessibleSize() const;
    const uint8_t* data() const;
    uint8_t* data();
    bool prepareWrite(uint64_t offset, uint64_t length);

    bool operator==(const Segment&) const = default;
};

struct Startup {
    uint64_t executableEntry;
    uint64_t interpreterBase;
    uint64_t programHeaders;
    uint16_t programHeaderSize;
    uint16_t programHeaderCount;
    std::string filename;
    std::vector<std::string> arguments;
    uint64_t uid;
    std::optional<std::string> argv0{};
    std::vector<std::string> environment{};
    std::optional<std::array<uint64_t, 4>> credentials{};
    uint64_t stackSize = guestStackSize;
};

struct Image {
    uint64_t entry;
    std::vector<Segment> segments;
    std::optional<Startup> startup;
};

Image loadELF(std::span<const uint8_t> elf, uint64_t bias = 0);
Image loadDynamicELF(std::span<const uint8_t> elf, std::span<const uint8_t> linker,
                     std::string_view guestFilename,
                     std::string_view interpreterPath = "/system/bin/linker64",
                     std::vector<std::string> arguments = {}, uint64_t uid = 1000);
std::string elfInterpreter(std::span<const uint8_t> elf);
std::vector<uint8_t> readFile(const std::string& path);

enum class Stop { running, exit, fault, unsupported, fuelExhausted };
enum class Mode { step, blocks, aot };

struct CPU {
    std::array<uint64_t, 31> x{};
    std::array<std::array<uint8_t, 16>, 32> v{};
    uint64_t pc = 0;
    uint64_t sp = 0;
    uint64_t retired = 0;
    uint64_t faultAddress = 0;
    uint64_t threadPointer = 0;
    uint64_t exclusiveAddress = 0;
    unsigned exclusiveSize = 0;
    uint8_t nzcv = 0;
    int exitCode = 0;
    Stop stop = Stop::running;
    std::string output;

    bool operator==(const CPU&) const = default;
};

struct Machine {
    explicit Machine(const Image& image, const std::shared_ptr<AnonymousBudget>& budget = {});

    CPU cpu;
    // Kept in address order. Insert new mappings through insertSegment().
    std::vector<Segment> segments;
    void insertSegment(Segment segment);

    uint64_t dataAddress(uint64_t address) const {
        return topByteIgnore ? address & 0x00ff'ffff'ffff'ffffULL : address;
    }
    const uint8_t* read(uint64_t address, size_t size, unsigned flags) const;
    bool readInto(uint64_t address, std::span<uint8_t> output) const;
    bool write(uint64_t address, std::span<const uint8_t> bytes);
    bool load64(uint64_t address, uint64_t& value) const;
    bool store64(uint64_t address, uint64_t value);
    void fault(uint64_t pc, uint64_t address);

private:
    size_t findSegment(uint64_t address, size_t size, unsigned flags, size_t& hint) const;
    bool mappedRange(uint64_t address, size_t size, unsigned flags) const;
    mutable size_t codeHint = 0, dataHint = 0;
    mutable std::array<size_t, 64> segmentHints{};
    bool topByteIgnore;
};

struct AOTBlock {
    std::string_view fixture;
    uint64_t entryOffset;
    std::span<const uint32_t> words;
    void (*execute)(Machine&, uint64_t pc);
};

std::span<const AOTBlock> compiledBlocks();

struct Counters {
    uint64_t interpreted = 0;
    uint64_t blockDispatches = 0;
    uint64_t aotEntries = 0;
    uint64_t fallbackDispatches = 0;
    uint64_t rejectedMappings = 0;
};

void executeDecoded(Machine& machine, const Instruction& instruction);
Counters run(Machine& machine, Mode mode, uint64_t entry, std::string_view fixture);
const char* modeName(Mode mode);

} // namespace nestvane
