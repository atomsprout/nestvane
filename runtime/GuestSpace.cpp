#include "GuestSpace.hpp"

#include <algorithm>
#include <cstring>
#include <mach/mach.h>
#include <mutex>
#include <sys/mman.h>

namespace nestvane {
namespace {

constexpr uint64_t tableBytes = (GuestSpace::chunks * 8 + GuestSpace::size / GuestSpace::page / 8 +
                                 GuestSpace::size / GuestSpace::page / 4 + GuestSpace::page - 1) & ~(GuestSpace::page - 1);

// The tables of an empty space are a copy-on-write copy of one block that is filled in once, so that
// a process only pays for the parts of them it uses.
void* emptyTables() {
    static void* block = [] {
        auto* offsets = static_cast<uint64_t*>(mmap(nullptr, tableBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        std::fill(offsets, offsets + GuestSpace::chunks, GuestSpace::voidOffset);
        return static_cast<void*>(offsets);
    }();
    return block;
}

void* copyOnWrite(const void* source, uint64_t length, void* target = nullptr) {
    vm_address_t address = reinterpret_cast<vm_address_t>(target);
    vm_prot_t current, maximum;
    const int flags = target ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE : VM_FLAGS_ANYWHERE;
    if (vm_remap(mach_task_self(), &address, length, 0, flags, mach_task_self(), reinterpret_cast<vm_address_t>(source), TRUE,
                 &current, &maximum, VM_INHERIT_COPY) != KERN_SUCCESS)
        return nullptr;
    return reinterpret_cast<void*>(address);
}

} // namespace

GuestSpace::GuestSpace() : GuestSpace(copyOnWrite(emptyTables(), tableBytes)) {}

GuestSpace::GuestSpace(void* tables) { attach(tables); }

void GuestSpace::attach(void* tables) {
    tables_ = tables;
    offsets_ = static_cast<uint64_t*>(tables);
    used_ = static_cast<uint8_t*>(tables) + chunks * 8;
    protection_ = used_ + size / page / 8;
}

GuestSpace::~GuestSpace() {
    for (const auto& run : runs_) munmap(run.memory - guardBefore, guardBefore + run.length + guardAfter);
    if (tables_) munmap(tables_, tableBytes);
}

std::unique_ptr<GuestSpace> GuestSpace::fork() const {
    void* tables = copyOnWrite(tables_, tableBytes);
    if (!tables) return nullptr;
    std::unique_ptr<GuestSpace> child(new GuestSpace(tables));
    child->code_ = code_;
    child->interpreted_ = interpreted_;
    child->prelink = prelink;
    for (const auto& run : runs_) {
        uint8_t* memory = child->reserve(nullptr, guardBefore + run.length + guardAfter);
        if (!memory) return nullptr;
        child->runs_.push_back({run.guest, run.length, memory + guardBefore});
        if (!copyOnWrite(run.memory, run.length, memory + guardBefore)) return nullptr;
        for (uint64_t index = run.guest >> chunkShift; index < (run.guest + run.length) >> chunkShift; ++index)
            child->offsets_[index] = reinterpret_cast<uint64_t>(memory) + guardBefore - run.guest;
    }
    return child;
}

uint8_t* GuestSpace::reserve(uint8_t* hint, uint64_t length) {
    void* result = mmap(hint, length, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result == MAP_FAILED) return nullptr;
    reserved_ += length;
    return static_cast<uint8_t*>(result);
}

void GuestSpace::unreserve(uint8_t* memory, uint64_t length) {
    munmap(memory, length);
    reserved_ -= length;
}

GuestSpace::Run* GuestSpace::runAt(uint64_t guest) {
    for (auto& run : runs_)
        if (guest - run.guest < run.length) return &run;
    return nullptr;
}

bool GuestSpace::chunkEmpty(uint64_t index) const {
    const uint8_t* bits = used_ + index * (chunk / page / 8);
    return std::all_of(bits, bits + chunk / page / 8, [](uint8_t byte) { return byte == 0; });
}

// Gives host memory to chunks [first, last), none of which has any. It continues the run on either
// side when the host addresses next to it are free, so that neighbours in the guest stay neighbours
// in the host.
bool GuestSpace::backRange(uint64_t first, uint64_t last) {
    const uint64_t guest = first << chunkShift, length = (last - first) << chunkShift;
    Run* before = first ? runAt(guest - 1) : nullptr;
    Run* after = last < chunks ? runAt(guest + length) : nullptr;
    Run* run = nullptr;
    if (before) {  // the new chunks take over its trailing guard, and end with a new one
        uint8_t* wanted = before->memory + before->length + guardAfter;
        uint8_t* got = reserve(wanted, length);
        if (got == wanted) {
            before->length += length;
            run = before;
        } else if (got) {
            unreserve(got, length);
        }
    }
    if (!run && after) {
        uint8_t* wanted = after->memory - guardBefore - length;
        uint8_t* got = reserve(wanted, length);
        if (got == wanted) {
            after->memory -= length;
            after->guest -= length;
            after->length += length;
            run = after;
        } else if (got) {
            unreserve(got, length);
        }
    }
    if (!run) {
        uint8_t* got = reserve(nullptr, guardBefore + length + guardAfter);
        if (!got) return false;
        runs_.push_back({guest, length, got + guardBefore});
        run = &runs_.back();
    }
    for (uint64_t index = first; index < last; ++index) offsets_[index] = reinterpret_cast<uint64_t>(run->memory) - run->guest;
    // Mapped pages in these chunks were untouched anonymous memory. Now they are host memory.
    for (uint64_t at = guest; at < guest + length;) {
        const int wanted = mapped(at) ? protection(at) : 0;
        uint64_t end = at + page;
        while (end < guest + length && (mapped(end) ? protection(end) : 0) == wanted) end += page;
        if (wanted) mprotect(host(at), end - at, wanted);
        at = end;
    }
    return true;
}

bool GuestSpace::back(uint64_t address, uint64_t length) {
    const uint64_t end = (address + length + chunk - 1) >> chunkShift;
    for (uint64_t index = address >> chunkShift; index < end;) {
        if (offsets_[index] != voidOffset) { ++index; continue; }
        uint64_t last = index + 1;
        while (last < end && offsets_[last] == voidOffset) ++last;
        if (!backRange(index, last)) return false;
        index = last;
    }
    return true;
}

// Returns a chunk's host memory. The guards of what remains of its run are kept.
void GuestSpace::freeChunk(uint64_t index) {
    const uint64_t guest = index << chunkShift;
    Run* run = runAt(guest);
    uint8_t* memory = run->memory + (guest - run->guest);
    const bool first = guest == run->guest, last = guest + chunk == run->guest + run->length;
    const auto guard = [](uint8_t* at, uint64_t length) { mmap(at, length, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0); };
    if (first && last) {
        unreserve(memory - guardBefore, guardBefore + chunk + guardAfter);
        *run = runs_.back();
        runs_.pop_back();
    } else if (first) {
        guard(memory + chunk - guardBefore, guardBefore);
        unreserve(memory - guardBefore, chunk);
        run->memory += chunk;
        run->guest += chunk;
        run->length -= chunk;
    } else if (last) {
        guard(memory, guardAfter);
        unreserve(memory + guardAfter, chunk);
        run->length -= chunk;
    } else {
        const Run right{guest + chunk, run->guest + run->length - guest - chunk, memory + chunk};
        run->length = guest - run->guest;
        guard(memory, guardAfter);
        guard(memory + chunk - guardBefore, guardBefore);
        unreserve(memory + guardAfter, chunk - guardAfter - guardBefore);
        runs_.push_back(right);
    }
    offsets_[index] = voidOffset;
}

uint64_t GuestSpace::contiguous(uint64_t address, uint64_t length) const {
    const uint64_t offset = offsets_[address >> chunkShift];
    uint64_t end = (address | (chunk - 1)) + 1;
    while (end < address + length && offsets_[end >> chunkShift] == offset) end += chunk;
    return std::min(end - address, length);
}

bool GuestSpace::placeable(uint64_t address, uint64_t length) const {
    const uint64_t offset = offsets_[address >> chunkShift];
    for (uint64_t index = address >> chunkShift; index <= (address + length - 1) >> chunkShift; ++index)
        if (offsets_[index] != offset) return false;
    return true;
}

void GuestSpace::mark(uint64_t address, uint64_t length, bool used, int protection) {
    uint64_t at = address / page;
    const uint64_t end = (address + length) / page;
    while (at < end) {
        if (at % 8 == 0 && end - at >= 8) {  // whole bytes of both tables
            const uint64_t pages = (end - at) & ~uint64_t{7};
            std::memset(used_ + at / 8, used ? 0xff : 0, pages / 8);
            std::memset(protection_ + at / 4, protection * 0x55, pages / 4);
            at += pages;
            continue;
        }
        if (used) used_[at / 8] |= uint8_t(1 << at % 8);
        else used_[at / 8] &= uint8_t(~(1 << at % 8));
        protection_[at / 4] = uint8_t((protection_[at / 4] & ~(3 << at % 4 * 2)) | protection << at % 4 * 2);
        ++at;
    }
}

bool GuestSpace::mapAnonymous(uint64_t address, uint64_t length, int protection, bool now) {
    if (address % page || length % page || address + length > size) return false;
    if (now && !back(address, length)) return false;
    mark(address, length, true, protection);
    for (uint64_t done = 0; done < length;) {
        const uint64_t part = contiguous(address + done, length - done);
        if (backed(address + done) &&
            mmap(host(address + done), part, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
            return false;
        done += part;
    }
    return true;
}

bool GuestSpace::mapFrom(uint64_t address, uint64_t length, int protection, const uint8_t* source, bool shared) {
    if (address % page || length % page || address + length > size || !back(address, length)) return false;
    mark(address, length, true, protection);
    for (uint64_t done = 0; done < length;) {
        const uint64_t part = contiguous(address + done, length - done);
        vm_address_t target = reinterpret_cast<vm_address_t>(host(address + done));
        vm_prot_t current, maximum;
        if (vm_remap(mach_task_self(), &target, part, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                     reinterpret_cast<vm_address_t>(source + done), shared ? FALSE : TRUE, &current, &maximum,
                     shared ? VM_INHERIT_SHARE : VM_INHERIT_COPY) != KERN_SUCCESS)
            return false;
        mprotect(reinterpret_cast<void*>(target), part, protection);
        done += part;
    }
    return true;
}

void GuestSpace::protect(uint64_t address, uint64_t length, int protection) {
    for (uint64_t at = address / page; at < (address + length) / page; ++at)
        protection_[at / 4] = uint8_t((protection_[at / 4] & ~(3 << at % 4 * 2)) | protection << at % 4 * 2);
    for (uint64_t done = 0; done < length;) {
        const uint64_t part = contiguous(address + done, length - done);
        if (backed(address + done)) mprotect(host(address + done), part, protection);
        done += part;
    }
}

void GuestSpace::unmap(uint64_t address, uint64_t length) {
    if (!length || address + length > size) return;
    for (uint64_t done = 0; done < length;) {
        const uint64_t part = contiguous(address + done, length - done);
        if (backed(address + done)) mmap(host(address + done), part, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        done += part;
    }
    mark(address, length, false, 0);
    for (uint64_t index = address >> chunkShift; index <= (address + length - 1) >> chunkShift; ++index)
        if (offsets_[index] != voidOffset && chunkEmpty(index)) freeChunk(index);
}

void GuestSpace::discard(uint64_t address, uint64_t length) {
    for (uint64_t at = address; at < address + length;) {
        uint64_t end = at + page;
        const bool present = mapped(at) && backed(at);
        const int wanted = protection(at);
        while (end < address + length && (end & (chunk - 1)) && mapped(end) == mapped(at) && protection(end) == wanted) end += page;
        if (present) mmap(host(at), end - at, wanted, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        at = end;
    }
}

bool GuestSpace::move(uint64_t from, uint64_t to, uint64_t length) {
    for (uint64_t done = 0; done < length;) {
        uint64_t part = contiguous(from + done, length - done);
        if (backed(from + done)) {
            if (!backed(to + done) && !touch(to + done)) return false;
            part = std::min(part, contiguous(to + done, length - done));
            vm_address_t target = reinterpret_cast<vm_address_t>(host(to + done));
            vm_prot_t current, maximum;
            if (vm_remap(mach_task_self(), &target, part, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                         reinterpret_cast<vm_address_t>(host(from + done)), FALSE, &current, &maximum, VM_INHERIT_COPY) != KERN_SUCCESS)
                return false;
        }
        done += part;
    }
    return true;
}

bool GuestSpace::touch(uint64_t address) {
    if (address >= size || !mapped(address)) return false;
    return backed(address) || backRange(address >> chunkShift, (address >> chunkShift) + 1);
}

bool GuestSpace::guestAddress(uint64_t hostAddress, uint64_t& address, bool* inRun) const {
    if (inRun) *inRun = false;
    if (hostAddress - voidOffset < size + chunk) {
        address = hostAddress - voidOffset;
        return true;
    }
    for (const auto& run : runs_) {
        const uint64_t start = reinterpret_cast<uint64_t>(run.memory);
        if (hostAddress - (start - guardBefore) >= guardBefore + run.length + guardAfter) continue;
        address = hostAddress - (start - run.guest);
        if (inRun) *inRun = hostAddress - start < run.length;
        return true;
    }
    return false;
}

bool GuestSpace::copyIn(uint64_t address, const void* from, uint64_t length) {
    for (uint64_t done = 0; done < length;) {
        if (!touch(address + done)) return false;
        const uint64_t part = std::min(contiguous(address + done, length - done), chunk - ((address + done) & (chunk - 1)));
        std::memcpy(host(address + done), static_cast<const uint8_t*>(from) + done, part);
        done += part;
    }
    return true;
}

bool GuestSpace::copyOut(void* to, uint64_t address, uint64_t length) {
    for (uint64_t done = 0; done < length;) {
        if (!touch(address + done)) return false;
        const uint64_t part = std::min(contiguous(address + done, length - done), chunk - ((address + done) & (chunk - 1)));
        std::memcpy(static_cast<uint8_t*>(to) + done, host(address + done), part);
        done += part;
    }
    return true;
}

} // namespace nestvane
