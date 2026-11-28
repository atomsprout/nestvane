#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "GuestSpace.hpp"

namespace nestvane {

// The guest's page size. 16384 matches the host, which native execution of guest code needs.
#ifndef NESTVANE_PAGE_SIZE
#define NESTVANE_PAGE_SIZE 4096
#endif
inline constexpr uint64_t pageSize = NESTVANE_PAGE_SIZE;
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

bool hasNativeSha256();
Instruction decode(uint32_t word);
bool endsBlock(Op op);

// The bytes of storage that several mappings share. It behaves like a vector of bytes until it is
// made shareable, which moves it to host pages of its own that can be mapped into guest address
// spaces page for page. Growing it may move it again: `moved` then tells the owner.
//
// The pages can instead be those of a host file (`backWith`, `attach`): then they take no memory of
// their own, the host writes what changes to the file, and a file that nothing reads is not even
// mapped. The file is as long as the reserved pages; the size of the content is the owner's to keep.
class SharedBuffer {
public:
    using value_type = uint8_t;
    SharedBuffer() = default;
    ~SharedBuffer();
    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const std::vector<uint8_t>& content) {
        resize(content.size());
        std::copy(content.begin(), content.end(), data());
        return *this;
    }
    uint8_t* data() { return paged() ? pages() : bytes.data(); }
    const uint8_t* data() const { return paged() ? pages() : bytes.data(); }
    size_t size() const { return paged() ? length : bytes.size(); }
    size_t capacity() const { return paged() ? reserved : bytes.capacity(); }
    bool empty() const { return size() == 0; }
    uint8_t* begin() { return data(); }
    uint8_t* end() { return data() + size(); }
    const uint8_t* begin() const { return data(); }
    const uint8_t* end() const { return data() + size(); }
    void resize(size_t size);
    // Moves the bytes to shareable host pages. False if the host has no memory for it.
    bool makeShareable();
    bool shareable() const { return paged(); }
    // Moves the bytes into a host file, new or emptied, and keeps them there from now on.
    bool backWith(std::string path);
    // Takes a host file that holds `size` bytes of content already as the bytes.
    void attach(std::string path, size_t size);
    bool backed() const { return !file.empty(); }
    // A host file that is the content and nothing else: exactly as long as the content, under a name
    // of its owner's choosing, so that it is of use outside the guest. The owner says how long the
    // content is (`truncateFile`) before it writes past the end, and removes the file itself.
    void attachExact(std::string path, size_t size);
    bool exact() const { return exactFile; }
    void truncateFile(uint64_t size) const;
    // Makes the bytes such a file, from memory or from the host file they are in.
    bool makeExact(std::string path, uint64_t contentSize);
    // The host file has another name now: with `move`, gives it that name.
    bool renameFile(std::string path, bool move);
    // The file is one of the first kind from now on.
    void leaveExact() { exactFile = false; }
    // Takes the bytes back into memory and lets go of the file.
    bool unback();
    // The name is another file's now (it was replaced from outside): lets go of the name. What was
    // mapped of the old file moves to memory; what was not is gone and reads as zeros.
    bool forsake();
    const std::string& backingFile() const { return file; }
    // The files under a directory whose buffers are gone since the last call: their owner decides
    // when the content is no longer needed and removes them.
    static std::vector<std::string> takeRetired(std::string_view directory);
    // Counts the times the bytes have moved in host memory.
    uint64_t moves = 0;
    // The inode of the host file, once its owner has looked: a file of another inode under the same
    // name later was put there from outside. 0 when not known.
    uint64_t hostInode = 0;

private:
    bool paged() const { return mapping || !file.empty(); }
    // The pages, mapped from the file now if they were not yet.
    uint8_t* pages() const;
    std::vector<uint8_t> bytes;
    mutable uint8_t* mapping = nullptr;
    size_t length = 0, reserved = 0;
    std::string file;
    bool exactFile = false;
};

struct SharedFileBytes {
    SharedBuffer pages;
    uint64_t size = 0;
    // Where the storage is mapped into guest address spaces, to map it again when it moves.
    struct Placement { std::weak_ptr<GuestSpace> space; uint64_t address, offset, length; };
    std::vector<Placement> placements;
    uint64_t placedMoves = 0;
    // Maps part of the storage at an address of a guest address space, shared with every other user.
    bool place(const std::shared_ptr<GuestSpace>& space, uint64_t address, uint64_t offset, uint64_t length);
    // Forgets the placements in a range that has been unmapped.
    void unplace(const GuestSpace* space, uint64_t start, uint64_t end);
    // Maps the storage again wherever it is placed, after it moved or grew.
    void replace();
};

struct AnonymousBudget {
    uint64_t reserved = 0, dirty = 0;
    uint64_t reservedLimit, dirtyLimit;
};

class AnonymousPages {
public:
    static std::shared_ptr<AnonymousPages> allocate(uint64_t size, const std::shared_ptr<AnonymousBudget>& budget,
                                                     int* mappingError = nullptr);
    // Pages that are the memory of a guest address space at `base`, where native guest code reaches
    // them directly. With `map` they are mapped there now, readable and writable and zero-filled;
    // without it they describe memory that is already mapped, as after fork.
    static std::shared_ptr<AnonymousPages> allocateIn(const std::shared_ptr<GuestSpace>& space, uint64_t base, uint64_t size,
                                                       bool map = true);
    const std::shared_ptr<GuestSpace>& guestSpace() const { return space; }
    uint64_t guestBase() const { return base; }
    // Gives a range up for good, when its mapping goes away. Pages in a guest address space are
    // unmapped. Other pages only need discard.
    void release(uint64_t offset, uint64_t length);
    ~AnonymousPages();
    AnonymousPages(const AnonymousPages&) = delete;
    AnonymousPages& operator=(const AnonymousPages&) = delete;

    const uint8_t* read(uint64_t offset, size_t length) const;
    void readInto(uint64_t offset, std::span<uint8_t> bytes) const;
    void write(uint64_t offset, std::span<const uint8_t> bytes);
    uint64_t capacity() const { return size; }
    bool prepareWrite(uint64_t offset, uint64_t length, int* mappingError = nullptr);
    bool resident(uint64_t offset) const;
    void discard(uint64_t offset, uint64_t length);
    bool copyTo(AnonymousPages& target, uint64_t source, uint64_t destination, uint64_t length,
                int* mappingError = nullptr) const;

private:
    AnonymousPages(uint64_t size, const std::shared_ptr<AnonymousBudget>& budget);
    AnonymousPages(const std::shared_ptr<GuestSpace>& space, uint64_t base, uint64_t size);
    static constexpr size_t blockSize = 64 * 1024;
    std::shared_ptr<GuestSpace> space;  // set for pages in a guest address space
    uint64_t base = 0;
    struct Block {
        uint8_t* data = nullptr;
        size_t dirtyPages = 0;
    };
    void releaseBlock(size_t index);
    std::shared_ptr<AnonymousBudget> budget;
    uint64_t hostPage, size, dirtyBytes = 0;
    std::vector<bool> dirty;
    std::vector<Block> blocks;
};

class DenseBytes {
    struct Storage;

public:
    class Weak {
    public:
        DenseBytes lock() const;
        void remember(const DenseBytes& bytes) { storage = bytes.storage; }

    private:
        std::weak_ptr<Storage> storage;
    };

    DenseBytes() = default;
    DenseBytes(std::vector<uint8_t> bytes);
    DenseBytes(std::initializer_list<uint8_t> bytes);
    DenseBytes copyRange(size_t offset, size_t length) const;

    size_t size() const;
    size_t capacity() const;
    bool empty() const { return size() == 0; }
    const uint8_t* data() const;
    const uint8_t* begin() const { return data(); }
    const uint8_t* end() const { return empty() ? data() : data() + size(); }
    uint8_t* mutableData();
    void resize(size_t size);
    bool operator==(const DenseBytes& other) const;

private:
    std::shared_ptr<Storage> storage;
};

inline DenseBytes DenseBytes::Weak::lock() const {
    DenseBytes bytes;
    bytes.storage = storage.lock();
    return bytes;
}

struct Segment {
    uint64_t address;
    DenseBytes bytes;
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
    const uint8_t* imageView = nullptr; // Borrowed immutable root bytes, kept alive by the image loader.
    bool placed = false; // Shared storage that is mapped at this address of the guest address space.

    uint64_t mappedSize() const { return reservedSize ? reservedSize : bytes.size(); }
    uint64_t accessibleSize() const {
        if (anonymousPages || imageView) return mappedSize();
        if (!shared) return bytes.size();
        if (fileOffset >= shared->pages.size()) return 0;
        return std::min(mappedSize(), shared->pages.size() - fileOffset);
    }
    bool contains(uint64_t start, size_t count) const {
        if (start < address) return false;
        const auto offset = start - address;
        const auto length = accessibleSize();
        return offset < length && count <= length - offset;
    }
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
    // Text of translated files that this image loads where they were translated for.
    std::vector<std::array<uint64_t, 2>> nativeCode{};
    // The program or its dynamic linker has no translation. Translated libraries must then be
    // interpreted too: native code that returned into the program would not find it.
    bool interpreted = false;
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

// AArch64-only guest status fields: saturation and floating-point exceptions.
inline constexpr uint32_t fpsrSaturation = 1U << 27;
inline constexpr uint32_t fpsrMask = fpsrSaturation | 0x9f;
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
    uint32_t fpsr = 0;
    // The floating-point control register as the guest wrote it. Only the rounding mode has an effect,
    // and only on FRINTX and FRINTI; everything else rounds to nearest, which is all the system's own
    // code ever asks for.
    uint32_t fpcr = 0;
    uint8_t nzcv = 0;
    int exitCode = 0;
    Stop stop = Stop::running;
    std::string output;
    // Return addresses into interpreted code that native code was given interpreterReturnPc in place
    // of, each with the stack pointer at the call (NativeCode.cpp, the worker's loop).
    std::array<std::array<uint64_t, 2>, 16> returns{};
    uint8_t returnCount = 0;

    bool operator==(const CPU&) const = default;
};

// The guest address native code returns to instead of an address in code that is interpreted: its
// entry in the code table leaves native code without a fault (NativeEntry.S,
// nestvane_native_interpreter_return). 0 when there is none.
inline uint64_t interpreterReturnPc = 0;

// Where a branch to `target` goes: to itself, or for interpreterReturnPc to the return address it
// stands for, which x30 then holds again. Entries of calls deeper than the stack is now, left by a
// longjmp, are dropped.
inline uint64_t branchTarget(CPU& cpu, uint64_t target) {
    if (target != interpreterReturnPc || !target) return target;
    while (cpu.returnCount && cpu.returns[cpu.returnCount - 1][1] < cpu.sp) --cpu.returnCount;
    if (!cpu.returnCount) return target;
    const uint64_t real = cpu.returns[--cpu.returnCount][0];
    cpu.x[30] = real;
    return real;
}

struct Machine {
    explicit Machine(const Image& image, const std::shared_ptr<AnonymousBudget>& budget = {},
                     std::shared_ptr<GuestSpace> space = {});

    CPU cpu;
    // Set when guest memory is laid out for native execution: private memory then lives in this space.
    std::shared_ptr<GuestSpace> space;
    // Kept in address order. Insert new mappings through insertSegment().
    std::vector<Segment> segments;
    void insertSegment(Segment segment);
    // With a guest address space: moves a private segment's contents to its place in it, so that it
    // is host memory at the address native guest code uses. Shared storage is not handled here.
    void place(Segment& segment);
    void placeInSpace(Segment& segment);

    uint64_t dataAddress(uint64_t address) const {
        return topByteIgnore ? address & 0x00ff'ffff'ffff'ffffULL : address;
    }
    const uint8_t* read(uint64_t address, size_t size, unsigned flags) const {
        const bool code = flags & 1;
        if (!code) address = dataAddress(address);
        const auto index = findSegment(address, size, flags, code ? codeHint : dataHint);
        if (index == segments.size()) return nullptr;
        const auto& segment = segments[index];
        const auto offset = address - segment.address;
        if (segment.anonymousPages) return segment.anonymousPages->read(segment.fileOffset + offset, size);
        return segment.data() + offset;
    }
    const uint8_t* read(uint64_t address, size_t size, unsigned flags,
                        std::vector<uint8_t>& scratch) const;

    const uint8_t* readInstruction(uint64_t address) const {
        if (address % sizeof(uint32_t)) return nullptr;
        if (codeHint < segments.size()) {
            const auto& segment = segments[codeHint];
            // Most guest instructions are fetched from borrowed, immutable ELF bytes.
            // Check the current mapping even when the cached index survives a remap.
            if (segment.imageView && (segment.flags & 1) && address >= segment.address) {
                const auto offset = address - segment.address;
                const auto length = segment.mappedSize();
                if (offset < length && sizeof(uint32_t) <= length - offset)
                    return segment.imageView + offset;
            }
        }
        return read(address, sizeof(uint32_t), 1);
    }
    // Where the instructions around `address` are in host memory, for fetching many in a row: from
    // `start` to `end` they are at `base` onward. Only code that cannot be written, which nothing
    // short of a change of mappings moves; false otherwise, and the caller fetches one at a time.
    bool codeWindow(uint64_t address, uint64_t& start, uint64_t& end, const uint8_t*& base) const;
    // The code segment around `address` when it is in the guest address space and cannot be written:
    // a worker can then fetch its instructions through the space (FastPath.hpp, SpaceCode).
    bool codeInSpace(uint64_t address, uint64_t& start, uint64_t& end) const;
    // Where the data around `address` is in host memory, for loads and stores straight from there:
    // from `start` to `end` it is at `base` onward. Only private memory of the guest address space
    // that has host pages behind it, and, for reading, data that cannot be written. Valid until the
    // mappings change; false otherwise, and the caller goes through read and write.
    bool dataWindow(uint64_t address, bool forWrite, uint64_t& start, uint64_t& end, uint8_t*& base) const;
    bool readInto(uint64_t address, std::span<uint8_t> output) const;
    bool write(uint64_t address, std::span<const uint8_t> bytes);
    bool load64(uint64_t address, uint64_t& value) const;
    bool store64(uint64_t address, uint64_t value);
    void fault(uint64_t pc, uint64_t address);

private:
    size_t findSegment(uint64_t address, size_t size, unsigned flags, size_t& hint) const {
        // Indices survive as hints only. Check the current mapping and permissions on every access.
        if (hint < segments.size()) {
            const auto& segment = segments[hint];
            if ((segment.flags & flags) == flags && segment.contains(address, size)) return hint;
        }
        return searchSegment(address, size, flags, hint);
    }
    size_t searchSegment(uint64_t address, size_t size, unsigned flags, size_t& hint) const;
    bool mappedRange(uint64_t address, size_t size, unsigned flags) const;
    mutable size_t codeHint = 0, dataHint = 0;
    mutable std::array<size_t, 64> segmentHints{};
    bool topByteIgnore;
};

// How many mappings got their guest memory by a copy of their bytes, and of what kind (Machine::placeInSpace).
std::string placedByCopyReport();

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
