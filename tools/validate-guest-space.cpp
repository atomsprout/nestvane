// Host checks for GuestSpace: placement, lazy backing, fork, shared mappings and address recovery.
#include "GuestSpace.hpp"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <vector>

using nestvane::GuestSpace;

static int failures;
static void check(const char* name, bool ok) {
    std::printf("%s: %s\n", name, ok ? "ok" : "FAILED");
    failures += !ok;
}

int main() {
    constexpr uint64_t page = GuestSpace::page, chunk = GuestSpace::chunk;
    GuestSpace space;
    const uint64_t base = 0x4000000000 - 64 * chunk;

    check("empty space maps nothing", !space.mapped(base) && !space.backed(base) && space.reservedBytes() == 0);
    check("map anonymous", space.mapAnonymous(base, 4 * page, GuestSpace::read | GuestSpace::write));
    std::memset(space.host(base), 0x5a, 4 * page);
    check("mapped and backed", space.mapped(base + page) && space.backed(base) && space.protection(base) == 3 && space.host(base)[100] == 0x5a);
    uint64_t guest = 0;
    bool inRun = false;
    check("host address back to guest", space.guestAddress(reinterpret_cast<uint64_t>(space.host(base + 12345)), guest, &inRun) && guest == base + 12345 && inRun);
    check("void address back to guest", space.guestAddress(reinterpret_cast<uint64_t>(space.host(0x1234567)), guest, &inRun) && guest == 0x1234567 && !inRun);

    // A large mapping gets host memory chunk by chunk, in the order it is touched.
    const uint64_t large = base + 8 * chunk;
    check("map lazily", space.mapAnonymous(large, 16 * chunk, 3, false) && space.mapped(large + 5 * chunk) && !space.backed(large + 5 * chunk));
    check("touch out of order", space.touch(large + 3 * chunk) && space.touch(large + chunk) && space.touch(large + 2 * chunk) && space.touch(large));
    space.host(large + 3 * chunk)[7] = 1;
    check("untouched chunk stays unbacked", !space.backed(large + 9 * chunk) && space.backed(large + 2 * chunk));
    std::vector<uint8_t> pattern(3 * chunk + 999);
    for (size_t index = 0; index < pattern.size(); ++index) pattern[index] = uint8_t(index * 31 + index / 4096);
    std::vector<uint8_t> copy(pattern.size());
    check("copy across chunks", space.copyIn(large + chunk - 500, pattern.data(), pattern.size()) &&
                                    space.copyOut(copy.data(), large + chunk - 500, copy.size()) && copy == pattern);
    check("copy to unmapped fails", !space.copyIn(base + 40 * chunk, pattern.data(), 100));

    // fork: both sides keep their own memory, and the tables are separate.
    auto child = space.fork();
    check("fork", child && child->mapped(base) && child->host(base) != space.host(base) && child->host(base)[100] == 0x5a);
    child->host(base)[100] = 1;
    space.host(base)[101] = 2;
    check("fork copies on write", space.host(base)[100] == 0x5a && child->host(base)[101] == 0x5a);
    check("fork keeps lazy mappings", child->mapped(large + 9 * chunk) && !child->backed(large + 9 * chunk) && child->touch(large + 9 * chunk) &&
                                        !space.backed(large + 9 * chunk));
    child->unmap(base, 4 * page);
    check("unmap in the child only", !child->mapped(base) && space.mapped(base) && space.host(base)[100] == 0x5a);

    // A buffer shared with host memory elsewhere, as for memory files and binder buffers.
    auto* buffer = static_cast<uint8_t*>(mmap(nullptr, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    std::memset(buffer, 7, 4 * page);
    const uint64_t shared = base + 40 * chunk - 2 * page;  // across a chunk boundary
    check("map shared", space.mapFrom(shared, 4 * page, 3, buffer, true) && space.host(shared)[5] == 7);
    uint8_t byte = 9;
    space.copyIn(shared + 2 * page + 3, &byte, 1);
    buffer[10] = 8;
    check("shared both ways", buffer[2 * page + 3] == 9 && space.host(shared)[10] == 8);
    const uint64_t priv = base + 44 * chunk;
    check("map private copy", space.mapFrom(priv, 4 * page, 3, buffer, false) && space.host(priv)[10] == 8);
    space.host(priv)[10] = 3;
    check("private copy is private", buffer[10] == 8);

    space.protect(base, page, GuestSpace::read);
    check("protect recorded", space.protection(base) == 1 && space.protection(base + page) == 3);
    space.discard(base + page, 2 * page);
    check("discard zeroes", space.host(base + page)[0] == 0 && space.host(base + 3 * page)[0] == 0x5a);

    const uint64_t target = base + 50 * chunk;
    check("move pages", space.mapAnonymous(target, 4 * page, 3) && space.move(base, target, 4 * page) && space.host(target)[3 * page + 1] == 0x5a);

    const uint64_t reserved = space.reservedBytes();
    space.unmap(large, 16 * chunk);
    check("unmap returns host memory", space.reservedBytes() < reserved && !space.mapped(large) && !space.backed(large + chunk));
    std::printf("runs=%zu reserved_mib=%llu failures=%d\n", space.runCount(), static_cast<unsigned long long>(space.reservedBytes() >> 20), failures);
    return failures != 0;
}
