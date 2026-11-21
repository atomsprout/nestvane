#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace nestvane {

// The address space of one guest process whose code runs natively (see results/G0.md, "稀疏地址空间的实现").
//
// Guest memory is host memory at an offset that depends on the chunk the guest address is in:
// host = guest + offsets()[guest >> chunkShift]. Rewritten code does this lookup itself, so the
// table's layout is fixed by experiments/g0/e3/rewrite.py (--table --chunk-shift 20).
//
// A chunk has host memory ("is backed") or its offset is voidOffset, which leads above every address
// a process can map: an access there faults, and the fault address still tells the guest address.
// Backed chunks are grouped in runs, one host reservation each with guard pages on both sides, so
// that an access that leaves the run of its base register faults instead of reaching other memory.
// Within a run guest memory is contiguous in host memory.
//
// The class places memory and keeps the per-page record that fork and the fault handler need. What a
// mapping means (file, shared, stack) stays with the caller.
class GuestSpace {
public:
    static constexpr unsigned addressBits = 39, chunkShift = 20;
    static constexpr uint64_t size = uint64_t{1} << addressBits, chunk = uint64_t{1} << chunkShift;
    static constexpr uint64_t chunks = size >> chunkShift, page = 16384;
    static constexpr uint64_t voidOffset = uint64_t{1} << 47;
    static constexpr uint64_t guardBefore = page, guardAfter = 5 * page;
    enum : int { read = 1, write = 2 };

    GuestSpace();
    ~GuestSpace();
    GuestSpace(const GuestSpace&) = delete;
    GuestSpace& operator=(const GuestSpace&) = delete;

    // A copy for a forked process: every run and the tables are copied on write.
    std::unique_ptr<GuestSpace> fork() const;

    // The table rewritten code indexes: one offset per chunk.
    const uint64_t* offsets() const { return offsets_; }
    // The host address of a guest address. In a chunk that is not backed it is an address that faults.
    uint8_t* host(uint64_t address) const { return reinterpret_cast<uint8_t*>(address + offsets_[address >> chunkShift]); }
    bool backed(uint64_t address) const { return offsets_[address >> chunkShift] != voidOffset; }
    bool mapped(uint64_t address) const { return used_[address / page / 8] >> (address / page % 8) & 1; }
    int protection(uint64_t address) const { return protection_[address / page / 4] >> (address / page % 4 * 2) & 3; }
    // How much of [address, address + length) is contiguous in host memory from its start.
    uint64_t contiguous(uint64_t address, uint64_t length) const;
    // Whether all of a range would be in one run if it were mapped now: its chunks are backed alike or
    // not backed at all. A stack needs this, because the stack pointer is a host address.
    bool placeable(uint64_t address, uint64_t length) const;

    // Maps zero-filled private memory. With `now` false, chunks that have no host memory yet get it
    // when the range is first touched (see touch), which keeps large sparse mappings cheap.
    bool mapAnonymous(uint64_t address, uint64_t length, int protection, bool now = true);
    // Maps host memory that exists elsewhere, such as a shared buffer or cached file pages, into the
    // range: shared with the source, or as a private copy made on write.
    bool mapFrom(uint64_t address, uint64_t length, int protection, const uint8_t* source, bool shared);
    void protect(uint64_t address, uint64_t length, int protection);
    // Removes mappings and returns the host memory of chunks that hold nothing any more.
    void unmap(uint64_t address, uint64_t length);
    // Makes private anonymous pages read as zero again (MADV_DONTNEED).
    void discard(uint64_t address, uint64_t length);
    // Moves the pages of one range to another without copying them. Both are mapped.
    bool move(uint64_t from, uint64_t to, uint64_t length);
    // Gives host memory to the chunk of a mapped address that has none. False if it is not mapped.
    bool touch(uint64_t address);

    // The guest address that a host address stands for: in the void, or in a run or its guards.
    // `inRun` tells the two apart from an address in a run's own memory.
    bool guestAddress(uint64_t hostAddress, uint64_t& address, bool* inRun = nullptr) const;
    // Copies between host memory and a guest range that need not be contiguous in host memory.
    // False if part of the range is not mapped.
    bool copyIn(uint64_t address, const void* from, uint64_t length);
    bool copyOut(void* to, uint64_t address, uint64_t length);

    // Where translated code is mapped as it was translated for, so that it can run natively.
    void addCode(uint64_t start, uint64_t end) { code_.push_back({start, end}); }
    bool hasCode(uint64_t pc) const {
        if (pc - lastCode_.start < lastCode_.length) return true;
        for (const auto& range : code_)
            if (pc >= range.start && pc < range.end) {
                lastCode_ = {range.start, range.end - range.start};
                return true;
            }
        return false;
    }
    // The records the dynamic linker reads to load translated files where they were translated for
    // (see experiments/aosp/patch.py): where the process's table is, and which files are in it.
    struct Prelink {
        static constexpr uint64_t capacity = 2048;
        uint64_t table = 0;
        std::vector<std::pair<uint64_t, uint64_t>> files;  // device and inode
    } prelink;

    uint64_t reservedBytes() const { return reserved_; }
    size_t runCount() const { return runs_.size(); }

private:
    struct Run { uint64_t guest, length; uint8_t* memory; };
    explicit GuestSpace(void* tables);
    void attach(void* tables);
    Run* runAt(uint64_t guest);
    bool backRange(uint64_t first, uint64_t last);
    bool back(uint64_t address, uint64_t length);
    void freeChunk(uint64_t index);
    bool chunkEmpty(uint64_t index) const;
    void mark(uint64_t address, uint64_t length, bool used, int protection);
    uint8_t* reserve(uint8_t* hint, uint64_t length);
    void unreserve(uint8_t* memory, uint64_t length);

    void* tables_ = nullptr;        // offsets, mapped bits and protection bits in one block, so that fork copies them lazily
    uint64_t* offsets_ = nullptr;
    uint8_t* used_ = nullptr;       // one bit per guest page that is mapped
    uint8_t* protection_ = nullptr; // two bits per guest page
    std::vector<Run> runs_;
    struct CodeRange { uint64_t start, end; };
    std::vector<CodeRange> code_;
    mutable struct { uint64_t start = 0, length = 0; } lastCode_;
    uint64_t reserved_ = 0;
};

} // namespace nestvane
