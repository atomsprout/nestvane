// What a software MMU would cost the translated code (PLAN.md, "下一步：先量 2.0").
//
// The same small kernels run with every load and store done one of four ways, written out in the
// instructions the translator would emit (inline assembly, so the compiler cannot merge or hoist the
// lookups as the translator, which works one instruction at a time, cannot either):
//
//   native  ldr x0, [x1]
//   chunk   what Nestvane does today: host = guest + offsets[guest >> 20], one table per process,
//           a run of chunks contiguous in host memory (2 dependent loads)
//   mmu     a two-level page table of 16 KiB pages, pages scattered in a shared pool: host =
//           guest + l2[l1[guest >> 25]][guest >> 14 & 2047], and a check that the access does not
//           cross into the next page, which in a pool is somewhere else (3 dependent loads)
//   tlb     a direct-mapped software TLB of 256 entries in front of the same page table, as QEMU
//           does: tag and addend in one ldp, a walk and refill on a miss
//
// Stores look up the same tables here; a real MMU would have a second table for writes (pages that
// are read-only or copy-on-write point to a fault), which costs memory but no more instructions.
// No access in these kernels crosses a page, so the slow path of the cross-page check is not
// written. The check, which must not change the flags, is add, eor, tbnz.
//
//   xcrun clang++ -O2 -std=c++20 -fno-vectorize -fno-slp-vectorize tools/bench-mmu.cpp -o generated/bench-mmu
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <vector>

namespace {

constexpr uint64_t guestBase = 0x40'0000'0000ull;  // inside a 39-bit guest address space
constexpr uint64_t pageShift = 14, page = 1ull << pageShift, chunkShift = 20;
constexpr uint64_t poolBytes = 192ull << 20;

uint8_t* pool;                    // host memory
std::vector<uint64_t> chunkTable;  // 2^19 offsets, as GuestSpace has
std::vector<uint64_t*> l1;         // 2^14 pointers to second-level tables of 2048 offsets
std::vector<std::vector<uint64_t>> l2s;
struct TlbEntry { uint64_t tag, addend; };
TlbEntry tlb[256];

enum class Mode { native, chunk, mmu, tlb };

template <Mode M> inline uint64_t load64(uint64_t guest);
template <Mode M> inline void store64(uint64_t guest, uint64_t value);
template <Mode M> inline uint32_t load8(uint64_t guest);

uint8_t* hostDirect(uint64_t guest) { return pool + (guest - guestBase); }

// native: the guest address is the host address of the contiguous pool.
template <> inline uint64_t load64<Mode::native>(uint64_t g) {
    uint64_t v, h = uint64_t(hostDirect(g));
    asm volatile("ldr %0, [%1]" : "=r"(v) : "r"(h) : "memory");
    return v;
}
template <> inline void store64<Mode::native>(uint64_t g, uint64_t v) {
    uint64_t h = uint64_t(hostDirect(g));
    asm volatile("str %0, [%1]" : : "r"(v), "r"(h) : "memory");
}
template <> inline uint32_t load8<Mode::native>(uint64_t g) {
    uint32_t v; uint64_t h = uint64_t(hostDirect(g));
    asm volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(h) : "memory");
    return v;
}

#define CHUNK_LOOKUP "lsr %[t], %[g], #20\n ldr %[t], [%[tab], %[t], lsl #3]\n"
template <> inline uint64_t load64<Mode::chunk>(uint64_t g) {
    uint64_t v, t;
    asm volatile(CHUNK_LOOKUP "ldr %[v], [%[t], %[g]]"
                 : [v] "=r"(v), [t] "=&r"(t) : [g] "r"(g), [tab] "r"(chunkTable.data()) : "memory");
    return v;
}
template <> inline void store64<Mode::chunk>(uint64_t g, uint64_t v) {
    uint64_t t;
    asm volatile(CHUNK_LOOKUP "str %[v], [%[t], %[g]]"
                 : [t] "=&r"(t) : [v] "r"(v), [g] "r"(g), [tab] "r"(chunkTable.data()) : "memory");
}
template <> inline uint32_t load8<Mode::chunk>(uint64_t g) {
    uint32_t v; uint64_t t;
    asm volatile(CHUNK_LOOKUP "ldrb %w[v], [%[t], %[g]]"
                 : [v] "=r"(v), [t] "=&r"(t) : [g] "r"(g), [tab] "r"(chunkTable.data()) : "memory");
    return v;
}

// Two-level walk, then the cross-page check for an access of `size` bytes (none for one byte).
#define MMU_WALK "lsr %[t], %[g], #25\n ldr %[t], [%[tab], %[t], lsl #3]\n ubfx %[u], %[g], #14, #11\n ldr %[t], [%[t], %[u], lsl #3]\n"
#define CROSS8 "add %[u], %[g], #7\n eor %[u], %[u], %[g]\n tbnz %[u], #14, 9f\n"
template <> inline uint64_t load64<Mode::mmu>(uint64_t g) {
    uint64_t v, t, u;
    asm volatile(MMU_WALK CROSS8 "ldr %[v], [%[t], %[g]]\n9:"
                 : [v] "=r"(v), [t] "=&r"(t), [u] "=&r"(u) : [g] "r"(g), [tab] "r"(l1.data()) : "memory");
    return v;
}
template <> inline void store64<Mode::mmu>(uint64_t g, uint64_t v) {
    uint64_t t, u;
    asm volatile(MMU_WALK CROSS8 "str %[v], [%[t], %[g]]\n9:"
                 : [t] "=&r"(t), [u] "=&r"(u) : [v] "r"(v), [g] "r"(g), [tab] "r"(l1.data()) : "memory");
}
template <> inline uint32_t load8<Mode::mmu>(uint64_t g) {
    uint32_t v; uint64_t t, u;
    asm volatile(MMU_WALK "ldrb %w[v], [%[t], %[g]]"
                 : [v] "=r"(v), [t] "=&r"(t), [u] "=&r"(u) : [g] "r"(g), [tab] "r"(l1.data()) : "memory");
    return v;
}

// Software TLB: index by page number, compare the tag without touching the flags, walk on a miss.
#define TLB_LOOKUP \
    "ubfx %[u], %[g], #14, #8\n add %[e], %[tlb], %[u], lsl #4\n ldp %[u], %[t], [%[e]]\n" \
    "eor %[u], %[u], %[g], lsr #14\n cbz %[u], 8f\n" \
    MMU_WALK "lsr %[u], %[g], #14\n stp %[u], %[t], [%[e]]\n8:\n"
template <> inline uint64_t load64<Mode::tlb>(uint64_t g) {
    uint64_t v, t, u, e;
    asm volatile(TLB_LOOKUP CROSS8 "ldr %[v], [%[t], %[g]]\n9:"
                 : [v] "=r"(v), [t] "=&r"(t), [u] "=&r"(u), [e] "=&r"(e)
                 : [g] "r"(g), [tab] "r"(l1.data()), [tlb] "r"(tlb) : "memory");
    return v;
}
template <> inline void store64<Mode::tlb>(uint64_t g, uint64_t v) {
    uint64_t t, u, e;
    asm volatile(TLB_LOOKUP CROSS8 "str %[v], [%[t], %[g]]\n9:"
                 : [t] "=&r"(t), [u] "=&r"(u), [e] "=&r"(e)
                 : [v] "r"(v), [g] "r"(g), [tab] "r"(l1.data()), [tlb] "r"(tlb) : "memory");
}
template <> inline uint32_t load8<Mode::tlb>(uint64_t g) {
    uint32_t v; uint64_t t, u, e;
    asm volatile(TLB_LOOKUP "ldrb %w[v], [%[t], %[g]]"
                 : [v] "=r"(v), [t] "=&r"(t), [u] "=&r"(u), [e] "=&r"(e)
                 : [g] "r"(g), [tab] "r"(l1.data()), [tlb] "r"(tlb) : "memory");
    return v;
}

// Kernels. Guest memory: a at 0, b at 64 MiB, a byte buffer at 128 MiB, a small table at 160 MiB.
constexpr uint64_t A = guestBase, B = guestBase + (64ull << 20), Bytes = guestBase + (128ull << 20),
                   Small = guestBase + (160ull << 20);
constexpr uint64_t words = (32ull << 20) / 8;

template <Mode M> uint64_t sequential() {  // sum of 32 MiB, in order
    uint64_t s = 0;
    for (uint64_t i = 0; i < words; ++i) s += load64<M>(A + i * 8);
    return s;
}
template <Mode M> uint64_t chase() {  // a random cycle through 32 MiB, one load depending on the last
    uint64_t p = A, s = 0;
    for (uint64_t i = 0; i < words / 4; ++i) { p = load64<M>(p); s += p; }
    return s;
}
template <Mode M> uint64_t copy() {  // 16 MiB from a to b
    for (uint64_t i = 0; i < words / 2; ++i) store64<M>(B + i * 8, load64<M>(A + i * 8));
    return load64<M>(B + 8);
}
template <Mode M> uint64_t histogram() {  // index addressing, as in gzip: counts of 16 MiB of bytes
    for (uint64_t i = 0; i < 256; ++i) store64<M>(Small + i * 8, 0);
    for (uint64_t i = 0; i < (16ull << 20); ++i) {
        const uint64_t slot = Small + load8<M>(Bytes + i) * 8;
        store64<M>(slot, load64<M>(slot) + 1);
    }
    return load64<M>(Small + 8 * 'a');
}
template <Mode M> uint64_t search() {  // binary search of sorted 32 MiB for 2 million keys
    uint64_t found = 0, key = 12345;
    for (int k = 0; k < 2'000'000; ++k) {
        key = key * 6364136223846793005ull + 1442695040888963407ull;
        const uint64_t want = (key >> 20) % (words * 2);
        uint64_t lo = 0, hi = words;
        while (lo < hi) {
            const uint64_t mid = (lo + hi) / 2;
            if (load64<M>(A + mid * 8) < want) lo = mid + 1; else hi = mid;
        }
        found += lo;
    }
    return found;
}
template <Mode M> uint64_t mixed() {  // arithmetic with one load or store in four instructions or so
    uint64_t x = 1, s = 0;
    for (uint64_t i = 0; i < words; ++i) {
        x = x * 2862933555777941757ull + 3037000493ull;
        s ^= (x >> 17) + (s << 3);
        s += load64<M>(A + (i & 4095) * 8);
        if ((i & 7) == 0) store64<M>(Small + (i & 31) * 8, s);
    }
    return s;
}

void layout(bool scatter) {
    // Page i of the guest's 192 MiB lives at pool page perm[i]: in order for chunk and native (a run),
    // shuffled for the MMU modes (a pool shared by every process gives no order).
    const uint64_t pages = poolBytes / page;
    std::vector<uint64_t> perm(pages);
    for (uint64_t i = 0; i < pages; ++i) perm[i] = i;
    if (scatter) std::shuffle(perm.begin(), perm.end(), std::mt19937_64(7));
    for (auto& t : l2s) std::fill(t.begin(), t.end(), 1ull << 47);
    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t g = guestBase + i * page;
        l1[g >> 25][(g >> 14) & 2047] = uint64_t(pool + perm[i] * page) - g;
    }
    for (auto& e : tlb) e = {~0ull, 0};
}

// Fills guest memory through the page table, so every mode sees the same contents.
void fill() {
    auto at = [](uint64_t g) { return reinterpret_cast<uint64_t*>(g + l1[g >> 25][(g >> 14) & 2047]); };
    std::vector<uint64_t> order(words);
    for (uint64_t i = 0; i < words; ++i) order[i] = i;
    std::shuffle(order.begin() + 1, order.end(), std::mt19937_64(11));
    for (uint64_t i = 0; i < words; ++i) *at(A + order[i] * 8) = A + order[(i + 1) % words] * 8;  // the cycle
    std::mt19937_64 random(3);
    for (uint64_t i = 0; i < (16ull << 20); i += 8) {
        uint64_t r = random();
        std::memcpy(at(Bytes + i), &r, 8);
    }
}
void fillSorted() {
    auto at = [](uint64_t g) { return reinterpret_cast<uint64_t*>(g + l1[g >> 25][(g >> 14) & 2047]); };
    for (uint64_t i = 0; i < words; ++i) *at(A + i * 8) = i * 2;
}

template <Mode M, typename F> double best(F kernel, uint64_t& result, int repeats) {
    double fastest = 1e30;
    for (int run = 0; run < 5; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < repeats; ++r) result = kernel();
        fastest = std::min(fastest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return fastest;
}

} // namespace

int main() {
    pool = static_cast<uint8_t*>(mmap(nullptr, poolBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    chunkTable.assign(1ull << 19, 1ull << 47);
    for (uint64_t c = 0; c < (poolBytes >> chunkShift); ++c)
        chunkTable[(guestBase >> chunkShift) + c] = uint64_t(pool) - guestBase;
    l1.assign(1ull << 14, nullptr);
    l2s.resize(1ull << 14);
    for (uint64_t i = 0; i < l1.size(); ++i) { l2s[i].assign(2048, 1ull << 47); l1[i] = l2s[i].data(); }

    const char* names[] = {"sequential 32 MiB", "pointer chase", "copy 16 MiB", "byte histogram 16 MiB",
                           "binary search", "mixed arithmetic"};
    double ms[6][4];
    const int repeatsOf[6] = {50, 2, 100, 8, 1, 15};
    uint64_t check[6][4];
    for (int mode = 0; mode < 4; ++mode) {
        const bool scatter = mode >= 2;
        layout(scatter);
        fill();
        auto run = [&](int k, int repeats, auto native, auto chunk, auto mmu, auto tlbk) {
            switch (mode) {
            case 0: ms[k][0] = best<Mode::native>(native, check[k][0], repeats); break;
            case 1: ms[k][1] = best<Mode::chunk>(chunk, check[k][1], repeats); break;
            case 2: ms[k][2] = best<Mode::mmu>(mmu, check[k][2], repeats); break;
            case 3: ms[k][3] = best<Mode::tlb>(tlbk, check[k][3], repeats); break;
            }
        };
        // native reads the pool directly, so it needs the pages in order: lay out again for it.
        if (mode == 0) { layout(false); fill(); }
#define KERNEL(k, r, f) run(k, r, f<Mode::native>, f<Mode::chunk>, f<Mode::mmu>, f<Mode::tlb>)
        KERNEL(0, 50, sequential);
        KERNEL(1, 2, chase);
        KERNEL(2, 100, copy);
        KERNEL(3, 8, histogram);
        KERNEL(5, 15, mixed);
        fillSorted();
        KERNEL(4, 1, search);
#undef KERNEL
    }
    std::printf("| Kernel | native ms per pass | chunk (today) | mmu | tlb | mmu / chunk | tlb / chunk |\n| --- | --- | --- | --- | --- | --- | --- |\n");
    double g[3] = {1, 1, 1};
    for (int k = 0; k < 6; ++k) {
        const bool same = check[k][0] == check[k][1] && check[k][1] == check[k][2] && check[k][2] == check[k][3];
        std::printf("| %s | %.1f | %.2fx | %.2fx | %.2fx | %.2f | %.2f |%s\n", names[k], ms[k][0] / repeatsOf[k], ms[k][1] / ms[k][0],
                    ms[k][2] / ms[k][0], ms[k][3] / ms[k][0], ms[k][2] / ms[k][1], ms[k][3] / ms[k][1],
                    same ? "" : " (results differ)");
        g[0] *= ms[k][1] / ms[k][0]; g[1] *= ms[k][2] / ms[k][0]; g[2] *= ms[k][3] / ms[k][0];
    }
    std::printf("| geometric mean | | %.2fx | %.2fx | %.2fx | %.2f | %.2f |\n", std::pow(g[0], 1.0 / 6),
                std::pow(g[1], 1.0 / 6), std::pow(g[2], 1.0 / 6), std::pow(g[1] / g[0], 1.0 / 6), std::pow(g[2] / g[0], 1.0 / 6));
}
