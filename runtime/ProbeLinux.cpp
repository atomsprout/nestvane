#include "ProbeLinux.hpp"
#include "NativeCode.hpp"
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>

#include "BpfProgram.hpp"
#include "BpfTypes.hpp"
#include "GuestSensors.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iterator>
#include <new>
#include <random>
#include <sstream>
#include <thread>
#include <type_traits>

namespace nestvane {

namespace {

// The largest single mapping. In a WebView renderer Chromium's allocator reserves 16 GiB aligned to 16 GiB
// (it asks for 32 and gives back the ends), and V8's sandbox asks for about 196 GiB, or 68 if that fails.
// A reservation costs addresses, not memory.
constexpr uint64_t maxReservation = 256ULL * 1024 * 1024 * 1024;
constexpr uint64_t mappingFloor = 0x1'0000'0000;
// The settings of the guest's Ethernet interface under /proc/sys/net, which the network daemon and
// the address configuration write as they bring the interface up. The host's network takes no notice.
static bool isInterfaceSetting(std::string_view path) {
    for (const std::string_view prefix : {"/proc/sys/net/ipv4/conf/eth0/", "/proc/sys/net/ipv6/conf/eth0/",
                                          "/proc/sys/net/ipv4/neigh/eth0/", "/proc/sys/net/ipv6/neigh/eth0/"})
        if (path.starts_with(prefix) && path.find('/', prefix.size()) == std::string_view::npos) return true;
    return false;
}

constexpr uint64_t lowMappingFloor = 0x10000;
// The guest arena: 384 GiB of the 512 GiB a guest address space has, with 24 randomized page bits.
// A WebView renderer reserves tens of GiB up front (Chromium's allocator 32 GiB to get 16 aligned,
// V8 and Blink's heap cages more); with 128 GiB, Blink's heap found no room and the renderer stopped.
constexpr uint64_t mappingLimit = 0x60'0000'0000;
constexpr uint64_t maxGuestFileBytes = 2 * 1024 * 1024;
constexpr size_t maxBpfPins = 2048;
// Even the reference system_server alone exceeds 256 threads after boot.
// How many threads the guest may have at once. Android with a dozen ordinary apps comes to about 700;
// one large app brings several hundred of its own, and at 1024 it could start no more (Douyin beside
// Toutiao: "pthread_create failed", and the system came apart after it).
constexpr size_t maxGuestTasks = 4096;
// /proc/config.gz reports this guest's ABI, including the absence of VMAP_STACK.
// Contents of the fixed gzip stream
// CONFIG_ARM64=y
// CONFIG_64BIT=y
// CONFIG_IKCONFIG=y
// CONFIG_IKCONFIG_PROC=y
// # CONFIG_VMAP_STACK is not set
constexpr uint8_t guestKernelConfig[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x73, 0xf6,
    0xf7, 0x73, 0xf3, 0x74, 0x8f, 0x77, 0x0c, 0xf2, 0x35, 0x33, 0xb1, 0xad,
    0xe4, 0x72, 0x86, 0x70, 0xcd, 0x4c, 0x9c, 0x3c, 0x43, 0x10, 0x5c, 0x4f,
    0x6f, 0x08, 0x03, 0x53, 0x24, 0x3e, 0x20, 0xc8, 0xdf, 0x19, 0x28, 0xac,
    0xac, 0x00, 0xe5, 0x87, 0xf9, 0x3a, 0x06, 0xc4, 0x07, 0x87, 0x38, 0x3a,
    0x7b, 0x2b, 0x64, 0x16, 0x2b, 0xe4, 0xe5, 0x97, 0x28, 0x14, 0xa7, 0x96,
    0x70, 0x01, 0x00, 0x3d, 0x2c, 0xce, 0x1f, 0x66, 0x00, 0x00, 0x00,
};
constexpr uint32_t guestLoopbackIndex = 1;
constexpr uint32_t guestLoopbackMtu = 65536;
constexpr uint32_t niceLimitResource = 13; // RLIMIT_NICE
constexpr size_t bpfMapBudget = 32 * 1024 * 1024;
constexpr std::string_view cgroupRoot = "/sys/fs/cgroup";
constexpr std::string_view cgroupProcs = "/cgroup.procs";
constexpr uint64_t lazyAnonymousThreshold = 4 * 1024 * 1024;
constexpr uint64_t noMemory = uint64_t(-12);
constexpr char unlabeledFile[] = "u:object_r:unlabeled:s0";
constexpr std::string_view restoreconMarker = "user.restorecon_in_progress";
constexpr uint32_t setgidMode = 02000;
constexpr uint8_t mdweRefuseExecGain = 1, mdweNoInherit = 2;
constexpr uint32_t sealSeal = 1, sealShrink = 2, sealGrow = 4, sealWrite = 8, sealFutureWrite = 16;
constexpr unsigned sigChild = 17;
constexpr uint64_t receiveTimeoutOld = 20, receiveTimeoutNew = 66;
constexpr uint64_t sendTimeoutOld = 21, sendTimeoutNew = 67;
constexpr std::string_view touchDevicePath = "/dev/input/event0";
constexpr std::string_view graphicsDevice = "/dev/goldfish_pipe_dprctd";
constexpr std::string_view cameraHalContext = "u:r:hal_camera_default:s0";
constexpr std::string_view sensorsHalContext = "u:r:hal_sensors_default:s0";
constexpr uint16_t addressFamilyVsock = 40;
constexpr uint32_t emulatorHostCid = 2, emulatorDataPort = 5000;
constexpr std::string_view graphicsAddressDevice = "/dev/goldfish_address_space";
constexpr uint64_t graphicsAddressBase = 0x800000000ULL, graphicsAlignment = 16 * 1024;
constexpr uint64_t goldfishHostMemory = 5, goldfishSharedSlots = 6;
constexpr size_t maxGraphicsTransfer = 64 * 1024;
constexpr uint32_t fileAccessed = 1, fileModified = 2, fileAttributes = 4, fileClosedWrite = 8,
                   fileClosedRead = 16, fileOpened = 32, fileMovedFrom = 64, fileMovedTo = 128,
                   fileCreated = 256, fileDeleted = 512, fileDeletedSelf = 1024, fileMovedSelf = 2048;
constexpr uint32_t fileWatchOverflow = 0x4000, fileWatchIgnored = 0x8000, fileWatchDirectory = 0x40000000,
                   fileWatchOnce = 0x80000000;
constexpr uint32_t secureNoRoot = 1, secureNoSetuidFixup = 4, secureKeepCaps = 16,
                   secureKeepCapsLocked = 32, secureNoAmbientRaise = 64;
constexpr uint64_t sigChildMask = uint64_t{1} << (sigChild - 1);
constexpr uint64_t invalid = uint64_t(-22);
constexpr unsigned sigSegv = 11;
constexpr uint64_t unblockableSignals = (1ULL << 8) | (1ULL << 18);
constexpr uint32_t saSigInfo = 4, saRestorer = 0x04000000, saOnStack = 0x08000000,
                   saRestart = 0x10000000, saNoDefer = 0x40000000, saResetHand = 0x80000000;
constexpr uint32_t ssOnStack = 1, ssDisable = 2;
constexpr uint32_t fpsimdMagic = 0x46508001;

struct SignalFrame {
    struct Info {
        int32_t number, error, code, padding;
        uint64_t address;
        std::array<uint8_t, 104> reserved;
    } info;
    struct Context {
        uint64_t flags, link;
        std::array<uint64_t, 3> stack;
        uint64_t mask;
        std::array<uint8_t, 120> maskPadding;
        struct alignas(16) MachineContext {
            uint64_t faultAddress;
            std::array<uint64_t, 31> regs;
            uint64_t sp, pc, pstate;
            struct alignas(16) FpSimd {
                uint32_t magic, size, fpsr, fpcr;
                std::array<std::array<uint8_t, 16>, 32> regs;
            } fp;
            std::array<uint8_t, 4096 - sizeof(FpSimd)> recordPadding;
        } machine;
    } context;
    std::array<uint64_t, 2> frameRecord;
};

static_assert(sizeof(SignalFrame::Info) == 128 && sizeof(SignalFrame::Context::MachineContext::FpSimd) == 0x210);
static_assert(offsetof(SignalFrame, context) == 128 && offsetof(SignalFrame::Context, machine) == 176);
static_assert(offsetof(SignalFrame::Context::MachineContext, fp) == 288 && offsetof(SignalFrame, frameRecord) == 4688);

bool onSignalStack(const std::array<uint64_t, 3>& stack, uint64_t sp) {
    return !(stack[1] & ssDisable) && sp > stack[0] && sp - stack[0] <= stack[2];
}

uint64_t writeEntropy(Machine& machine, uint64_t address, size_t size) {
    if (!size) return 0;
    std::array<uint8_t, pageSize> bytes;
    const auto output = std::span(bytes).first(size);
    std::random_device source;
    for (auto& byte : output) byte = static_cast<uint8_t>(source());
    return machine.write(address, output) ? size : uint64_t(-14);
}

void captureLog(std::string& initial, std::string& recent, size_t initialLimit, std::string_view message) {
    constexpr size_t recentLimit = 64 * 1024;
    if (recent.empty() && initial.size() + message.size() <= initialLimit) {
        initial.append(message);
        return;
    }
    if (recent.size() + message.size() > recentLimit) {
        const auto remove = std::max(recentLimit / 2, recent.size() + message.size() - recentLimit);
        const auto end = recent.find('\n', remove - 1);
        recent.erase(0, end == std::string::npos ? recent.size() : end + 1);
    }
    recent.append(message);
}


unsigned guestFlags(uint64_t prot) {
    // AArch64 page permissions allow reads on writable mappings.
    return ((prot & 3) ? 4 : 0) | (prot & 2) | ((prot & 4) ? 1 : 0);
}

uint64_t pageAlignedLength(uint64_t length) {
    return (length + pageSize - 1) & ~(pageSize - 1);
}

bool fullyMapped(const Machine& machine, uint64_t address, uint64_t end) {
    while (address < end) {
        auto match = std::find_if(machine.segments.begin(), machine.segments.end(), [=](const Segment& segment) {
            return address >= segment.address && address - segment.address < segment.mappedSize();
        });
        if (match == machine.segments.end()) return false;
        address = match->address + match->mappedSize();
    }
    return true;
}

enum class ForkAdvice { omit, inherit, wipe, keep };
using SparseBacking = std::map<uint64_t, std::shared_ptr<AnonymousPages>>;

void changeRange(Machine& machine, uint64_t start, uint64_t end, std::optional<unsigned> flags,
                 std::optional<ForkAdvice> advice = {}, const SparseBacking& sparseBacking = {}) {
    const bool retain = flags || advice;
    // The segments are in address order and do not overlap: only those that reach into the range are
    // looked at and replaced. A process has thousands, and a boot changes ranges a hundred thousand times.
    auto& all = machine.segments;
    if (static const bool check = std::getenv("NESTVANE_CHECK_SEGMENTS"); check)
        for (size_t index = 1; index < all.size(); ++index)
            if (all[index].address < all[index - 1].address + all[index - 1].mappedSize()) std::abort();
    const auto first = std::partition_point(all.begin(), all.end(), [&](const Segment& segment) {
        return segment.address + segment.mappedSize() <= start;
    });
    auto last = first;
    while (last != all.end() && last->address < end) ++last;
    std::vector<Segment> pages;
    pages.reserve(size_t(last - first) + 2);
    for (auto each = first; each != last; ++each) {
        auto& segment = *each;
        const auto segmentEnd = segment.address + segment.mappedSize();
        const auto left = std::max(start, segment.address), right = std::min(end, segmentEnd);
        if (left >= right) {
            pages.push_back(std::move(segment));
            continue;
        }
        if (!retain && segment.anonymousPages) {
            segment.anonymousPages->release(segment.fileOffset + left - segment.address, right - left);
        } else if (!retain && machine.space && segment.shared && segment.placed) {
            // Only what this segment mapped: a segment that is not placed may share a page with one that is.
            segment.shared->unplace(machine.space.get(), left, right);
            machine.space->unmap(left, right - left);
        }
        const auto applyAdvice = [&](Segment& part) {
            if (!advice) return;
            switch (*advice) {
                case ForkAdvice::omit: part.omitOnFork = true; break;
                case ForkAdvice::inherit: part.omitOnFork = false; break;
                case ForkAdvice::wipe: part.wipeOnFork = true; break;
                case ForkAdvice::keep: part.wipeOnFork = false; break;
            }
        };
        const bool privatize = segment.imageView && flags && (*flags & 2);
        if (retain && !privatize && left == segment.address && right == segmentEnd &&
            (advice || !segment.reservedSize || segment.anonymousPages || segment.imageView)) {
            if (flags) segment.flags = *flags;
            applyAdvice(segment);
            pages.push_back(std::move(segment));
            continue;
        }
        const auto part = [&](uint64_t from, uint64_t to, unsigned permissions, bool changed = false) {
            if (from == to) return;
            const auto append = [&](Segment fragment) {
                fragment.omitOnFork = segment.omitOnFork;
                fragment.wipeOnFork = segment.wipeOnFork;
                fragment.sharedAnonymous = segment.sharedAnonymous;
                fragment.sharedReadOnly = segment.sharedReadOnly;
                fragment.maxProtection = segment.maxProtection;
                if (changed) applyAdvice(fragment);
                pages.push_back(std::move(fragment));
            };
            if (segment.anonymousPages) {
                auto fragment = segment;
                fragment.address = from;
                fragment.reservedSize = to - from;
                fragment.fileOffset += from - segment.address;
                fragment.flags = permissions;
                append(std::move(fragment));
                return;
            }
            if (segment.imageView) {
                const auto* first = segment.imageView + (from - segment.address);
                if (permissions & 2) {
                    append({from, std::vector<uint8_t>(first, first + (to - from)),
                            permissions, 0, false, {}, 0,
                            false, false, false, true});
                } else {
                    auto fragment = segment;
                    fragment.address = from;
                    fragment.reservedSize = to - from;
                    fragment.imageView = first;
                    fragment.flags = permissions;
                    append(std::move(fragment));
                }
                return;
            }
            if (segment.shared) {
                append({from, {}, permissions, to - from, false, segment.shared,
                                 segment.fileOffset + from - segment.address, false, segment.sharedReadOnly, false, segment.fileMapping});
                return;
            }
            if (segment.reservedSize) {
                if (const auto planned = sparseBacking.find(from); changed && planned != sparseBacking.end()) {
                    Segment fragment{from, {}, permissions, to - from, false, {}, 0, true};
                    fragment.anonymousPages = planned->second;
                    append(std::move(fragment));
                } else if (permissions && !segment.beyondFileEnd) {
                    append({from, std::vector<uint8_t>(to - from), permissions, 0, false, {}, 0, segment.anonymous, false, false, segment.fileMapping});
                } else {
                    append({from, {}, 0, to - from, segment.beyondFileEnd, {}, 0, segment.anonymous, false, false, segment.fileMapping});
                }
                return;
            }
            auto bytes = segment.bytes.copyRange(from - segment.address, to - from);
            append({from, std::move(bytes), permissions, 0, false, {}, 0,
                    segment.anonymous, false, segment.initialStack, segment.fileMapping});
        };
        part(segment.address, left, segment.flags);
        if (retain) part(left, right, flags.value_or(segment.flags), true);
        part(right, segmentEnd, segment.flags);
    }
    const size_t index = size_t(first - all.begin());
    if (pages.size() == size_t(last - first)) {
        std::move(pages.begin(), pages.end(), first);
    } else {
        all.erase(first, last);
        all.insert(all.begin() + index, std::make_move_iterator(pages.begin()), std::make_move_iterator(pages.end()));
    }
    if (machine.space)
        for (size_t each = index; each < index + pages.size(); ++each) machine.place(all[each]);  // fragments that got buffers of their own
    machine.cpu.exclusiveSize = 0;
}

bool allowsBacking(const Machine& machine, uint64_t start, uint64_t end, uint64_t size, uint64_t memoryLimit) {
    for (const auto& segment : machine.segments) {
        if (segment.reservedSize) continue;
        const auto left = std::max(start, segment.address);
        const auto right = std::min(end, segment.address + segment.bytes.size());
        size += segment.bytes.size() - (left < right ? right - left : 0);
    }
    return size <= memoryLimit;
}

uint64_t protect(Machine& machine, uint64_t start, uint64_t count, uint64_t prot,
                 uint64_t memoryLimit, bool refuseExecGain, const std::shared_ptr<AnonymousBudget>& anonymousBudget) {
    if (start % pageSize || count > maxReservation || prot > 7) return invalid;
    if ((prot & 6) == 6) return refuseExecGain ? uint64_t(-13) : invalid;
    const auto length = pageAlignedLength(count);
    if (!length) return 0;
    if (start > UINT64_MAX - length || !fullyMapped(machine, start, start + length)) return noMemory;
    const auto end = start + length;
    if (refuseExecGain && (prot & 4) && std::any_of(machine.segments.begin(), machine.segments.end(),
        [=](const Segment& area) {
            return !(area.flags & 1) && area.address < end && start < area.address + area.mappedSize();
        })) return uint64_t(-13);
    if (std::any_of(machine.segments.begin(), machine.segments.end(), [=](const Segment& segment) {
            const bool denied = (prot & ~segment.maxProtection) || ((prot & 2) && segment.sharedReadOnly);
            return denied && segment.address < end && start < segment.address + segment.mappedSize();
        })) return uint64_t(-13);
    SparseBacking sparseBacking;
    if (prot) {
        constexpr uint64_t sparseProtectThreshold = 1024 * 1024;
        uint64_t needed = 0;
        for (const auto& segment : machine.segments) {
            if (segment.shared || segment.anonymousPages || segment.beyondFileEnd ||
                (segment.imageView && !(prot & 2))) continue;
            const auto left = std::max(start, segment.address);
            const auto right = std::min(end, segment.address + segment.mappedSize());
            if (left >= right) continue;
            const auto span = right - left;
            if (segment.anonymous && !segment.sharedAnonymous && segment.reservedSize &&
                span >= sparseProtectThreshold) {
                // Protecting a reserved allocator arena must not need resident pages up front.
                auto pages = AnonymousPages::allocate(span, anonymousBudget);
                if (!pages) return noMemory;
                sparseBacking.emplace(left, std::move(pages));
            } else {
                needed += span;
            }
        }
        if (!allowsBacking(machine, start, end, needed, memoryLimit)) return noMemory;
    }
    changeRange(machine, start, end, guestFlags(prot), {}, sparseBacking);
    return 0;
}

std::optional<std::string> readPath(const Machine& machine, uint64_t address) {
    std::string path;
    for (size_t pos = 0; pos < 4096 && address <= UINT64_MAX - pos; ++pos) {
        const auto* byte = machine.read(address + pos, 1, 4);
        if (!byte) return {};
        if (!*byte) return path;
        path.push_back(static_cast<char>(*byte));
    }
    return {};
}

std::string normalizePath(std::string_view path) {
    std::string result;
    while (!path.empty()) {
        const auto separator = path.find('/');
        const auto component = path.substr(0, separator);
        if (component == "..") {
            if (!result.empty()) result.resize(result.rfind('/'));
        } else if (!component.empty() && component != ".") {
            result += '/';
            result.append(component);
        }
        if (separator == std::string_view::npos) break;
        path.remove_prefix(separator + 1);
    }
    return result.empty() ? "/" : result;
}

template<class T>
bool writeGuest(Machine& machine, uint64_t address, const T& data) {
    return machine.write(address, {reinterpret_cast<const uint8_t*>(&data), sizeof(data)});
}

struct GuestStatx {
    struct Timestamp { int64_t seconds; uint32_t nanos; uint32_t reserved; };
    uint32_t mask, blockSize;
    uint64_t attributes;
    uint32_t links, uid, gid;
    uint16_t mode, padding;
    uint64_t inode, size, blocks, attributesMask;
    Timestamp accessed, created, changed, modified;
    uint32_t deviceTypeMajor, deviceTypeMinor, deviceMajor, deviceMinor;
    std::array<uint64_t, 14> optional{};
};
static_assert(sizeof(GuestStatx) == 256 && offsetof(GuestStatx, accessed) == 64
              && offsetof(GuestStatx, deviceTypeMajor) == 128);

uint64_t statFile(Machine& machine, uint64_t address, bool extended, uint64_t size, uint64_t inode,
                  uint32_t mode, uint32_t uid, uint32_t gid, uint64_t device, const GuestInodeTimes& times) {
    if (extended) {
        const auto major = [](uint64_t number) {
            return uint32_t((number >> 8) & 0xfff) | (uint32_t(number >> 32) & 0xfffff000);
        };
        const auto minor = [](uint64_t number) {
            return uint32_t(number & 0xff) | (uint32_t(number >> 12) & 0xffffff00);
        };
        const auto timestamp = [&](size_t index) -> GuestStatx::Timestamp {
            return {times[index], uint32_t(times[index + 1]), 0};
        };
        // Report basic metadata only. The guest file model has no birth times or extended attributes here.
        const GuestStatx result{0x7ff, pageSize, 0, 1, uid, gid, uint16_t(mode), 0,
                               inode, size, (size + 511) / 512, 0, timestamp(0), {},
                               timestamp(4), timestamp(2), major(device), minor(device), 0, 1};
        return writeGuest(machine, address, result) ? 0 : uint64_t(-14);
    }
    // Linux asm-generic stat layout, shared by Android arm64.
    std::array<uint64_t, 16> stat{};
    stat[0] = 1;
    stat[1] = inode;
    stat[2] = (1ULL << 32) | mode;
    stat[3] = (uint64_t(gid) << 32) | uid;
    stat[4] = device;
    stat[6] = size;
    stat[7] = pageSize;
    stat[8] = (size + 511) / 512;
    std::copy(times.begin(), times.end(), stat.begin() + 9);
    return writeGuest(machine, address, stat) ? 0 : uint64_t(-14);
}

} // namespace

ProbeLinux::ProbeLinux(uint64_t pid, uint64_t uid, uint64_t memoryLimit)
    : pid(pid), memoryLimit(memoryLimit),
      // ART reserves large sparse address ranges per process. Only touched blocks need host mappings.
      anonymousBudget(std::make_shared<AnonymousBudget>(AnonymousBudget{0, 0, 32 * memoryLimit, memoryLimit})),
      current(&threads[pid]), currentTid(pid), nextTid(pid + 1) {
    current->process = std::make_shared<Process>();
    current->process->id = pid;
    nativeMemory = pageSize == GuestSpace::page && (std::getenv("NESTVANE_NATIVE_MEMORY") || native::enabled());
    // With native execution, what is interpreted is an app's own library in a process whose other
    // threads are native: a turn of a few thousand instructions would be spent on switching.
    if (native::enabled()) sliceLength = 32768;
    // NESTVANE_NATIVE_WORKERS=<n> sets how many host threads run native code beside this one; 0 for none.
    if (const char* count = std::getenv("NESTVANE_NATIVE_WORKERS"); !count || std::atoi(count) > 0)
        native::startWorkers(count ? unsigned(std::atoi(count)) : 3);
    processes.emplace(pid, current->process.get());
    current->process->userIds.fill(uint32_t(uid));
    current->process->groupIds.fill(uint32_t(uid));
    if (!uid) current->process->effectiveCaps = current->process->permittedCaps = Process::allCapabilities;
    current->process->group = current->process->session = pid;
    current->process->nextMapping = randomMappingBase();
    current->mountNamespace = std::make_shared<MountNamespace>();
    addDirectory("/proc/self/ns", 0555);
    addDirectory("/proc/self/task", 0555);
    addDirectory("/proc/self/fd", 0555);
    addDirectory("/proc/sys/vm", 0555);
    addDirectory("/proc/pressure", 0555);
    addDirectory("/proc/sys/kernel", 0555);
    addDirectory("/proc/sys/kernel/random", 0555);
    std::array<uint8_t, 16> id{};
    std::random_device entropy;
    for (auto& byte : id) byte = static_cast<uint8_t>(entropy());
    id[6] = (id[6] & 0x0f) | 0x40;
    id[8] = (id[8] & 0x3f) | 0x80;
    constexpr std::string_view hex = "0123456789abcdef";
    std::string bootId;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) bootId.push_back('-');
        bootId.push_back(hex[id[i] >> 4]);
        bootId.push_back(hex[id[i] & 15]);
    }
    ashmemBootPath = "/dev/ashmem" + bootId;
    bootId.push_back('\n');
    kernelFiles.emplace("/proc/sys/kernel/random/boot_id", std::move(bootId));
    kernelFiles.emplace("/proc/config.gz", std::string(
        reinterpret_cast<const char*>(guestKernelConfig), sizeof guestKernelConfig));
    kernelFiles.emplace("/proc/filesystems", "nodev\ttmpfs\nnodev\tproc\nnodev\tbinder\nnodev\tbpf\next4\n");
    addDirectory("/dev/pts", 0755);
    addDirectory("/sys/devices", 0555);
    addDirectory("/sys/devices/system/cpu", 0555);
    for (const auto name : {"possible", "present", "online"})
        kernelFiles.emplace(std::string("/sys/devices/system/cpu/") + name, "0\n");
    addDirectory("/sys/fs/bpf", 0755);
    addDirectory("/sys/class", 0555);
    addDirectory("/sys/class/net/lo", 0555);
    kernelFiles.emplace("/sys/class/net/lo/ifindex", std::to_string(guestLoopbackIndex) + "\n");
    addDirectory("/sys/block", 0555);
    descriptors().emplace(0, Descriptor{"/dev/stdin", {}, 0, Node::input});
    descriptors().emplace(1, Descriptor{"/dev/stdout", {}, 1, Node::output});
    descriptors().emplace(2, Descriptor{"/dev/stderr", {}, 1, Node::error});
}

void ProbeLinux::mount(std::string path, std::span<const uint8_t> bytes) {
    files.emplace(std::move(path), bytes);
    fileInodesValid = false;
}

uint64_t ProbeLinux::fileInode(std::map<std::string, std::span<const uint8_t>, std::less<>>::const_iterator file) const {
    if (file == files.end()) return files.size() + 8;
    if (!fileInodesValid) {
        fileInodes.clear();
        fileInodes.reserve(files.size());
        uint64_t index = 0;
        for (const auto& [path, bytes] : files) fileInodes.emplace(path, index++);
        fileInodesValid = true;
    }
    return fileInodes.at(file->first) + 8;
}

void ProbeLinux::alias(std::string path, std::string target) {
    links.emplace(std::move(path), Link{std::move(target), {}});
}

namespace {
std::mutex rootPolicyLock;
ProbeLinux::RootPolicy configuredRootPolicy;
}

void ProbeLinux::setRootPolicy(RootPolicy policy) {
    std::lock_guard guard(rootPolicyLock);
    configuredRootPolicy = std::move(policy);
}

ProbeLinux::RootPolicy ProbeLinux::rootPolicy() {
    std::lock_guard guard(rootPolicyLock);
    return configuredRootPolicy;
}

void ProbeLinux::addToyboxLinks() {
    // Called while the root is being set up, before the system image is there to look in: a link
    // to a command the image does have is the same program.
    static constexpr std::string_view commands[] = {
        "basename", "cat", "chgrp", "chmod", "chown", "cmp", "cp", "cut", "date", "dd", "df", "diff", "dirname",
        "dmesg", "du", "echo", "env", "file", "find", "free", "getenforce", "getprop", "grep", "head",
        "hostname", "id", "ifconfig", "kill", "killall", "ln", "log", "ls", "lsof", "md5sum", "mkdir", "mount",
        "mv", "netstat", "nproc", "pgrep", "pidof", "pkill", "printf", "ps", "pwd", "readlink", "realpath",
        "rm", "rmdir", "sed", "setenforce", "setprop", "sha256sum", "sleep", "sort", "stat", "tail", "tar",
        "tee", "timeout", "top", "touch", "tr", "umount", "uname", "uptime", "wc", "which", "whoami", "xargs"};
    for (const auto command : commands) {
        auto path = "/system/bin/" + std::string(command);
        if (!links.contains(path)) alias(std::move(path), "/system/bin/toybox");
    }
}

void ProbeLinux::enableRoot() {
    rootOn = true;
    alias("/system/bin/su", "/system/bin/sh");
    if (!find("/system/xbin", 0)) addDirectory("/system/xbin", 0755);
    alias("/system/xbin/su", "/system/bin/sh");
}

std::string ProbeLinux::packageOfUid(uint32_t uid) const {
    // Android writes the list to /data, whose files are the kernel layer's own.
    const auto list = writableFiles.find(backingPath("/data/system/packages.list"));
    if (list == writableFiles.end()) return {};
    const auto& storage = *list->second->storage;
    const auto bytes = std::span(storage.pages).first(storage.size);
    std::istringstream lines(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string package;
        uint64_t owner = 0;
        if ((fields >> package >> owner) && owner == uid % 100000) return package;
    }
    return {};
}

std::string ProbeLinux::absolutePath(std::string_view path) const {
    if (path.starts_with('/')) return normalizePath(path);
    auto absolute = current->process->workingDirectory;
    absolute += '/';
    absolute.append(path);
    return normalizePath(absolute);
}

uint64_t ProbeLinux::resolveAtPath(uint32_t directory, std::string_view path, std::string& result) const {
    if (path.starts_with('/') || directory == uint32_t(-100)) {
        result = absolutePath(path);
        return 0;
    }
    const auto parent = descriptors().find(directory);
    if (parent == descriptors().end()) return uint64_t(-9);
    const auto& descriptor = parent->second;
    if (descriptor.node != Node::directory) return uint64_t(-20);
    result = descriptor.visiblePath.empty() ? descriptor.path : descriptor.visiblePath;
    result += '/';
    result.append(path);
    result = normalizePath(result);
    return 0;
}

uint64_t ProbeLinux::changeWorkingDirectory(std::string path) {
    const auto logical = followLinks(std::move(path));
    if (!logical) return uint64_t(-40);
    const auto backing = backingPath(*logical);
    if (!canAccessParent(backing, false)) return uint64_t(-13);
    const auto entry = find(backing, 0);
    if (!entry) return uint64_t(-2);
    if (entry->node != Node::directory) return uint64_t(-20);
    if (userId()) {
        if (const auto found = directories.find(backing); found != directories.end()) {
            const auto& node = found->second;
            if (!inodeAllows(node, 1)) return uint64_t(-13);
        }
    }
    current->process->workingDirectory = *logical;
    return 0;
}

std::optional<std::string> ProbeLinux::resolve(std::string_view path) const {
    if (path == "/proc/self/cwd") return current->process->workingDirectory;
    if (path == "/proc/self/exe" && !current->process->executable.empty())
        return current->process->executable;
    if (auto found = links.find(backingPath(path)); found != links.end()) return found->second.target;
    constexpr std::string_view fdPrefix = "/proc/self/fd/";
    if (!path.starts_with(fdPrefix)) return {};
    path.remove_prefix(fdPrefix.size());
    uint32_t fd = 0;
    const auto result = std::from_chars(path.data(), path.data() + path.size(), fd);
    if (result.ec != std::errc{} || result.ptr != path.data() + path.size()) return {};
    if (const auto found = descriptors().find(fd); found != descriptors().end()) return found->second.visiblePath.empty() ? found->second.path : found->second.visiblePath;
    return {};
}

std::optional<std::string> ProbeLinux::followLinks(std::string path, bool followFinal, uint64_t* lookupError) const {
    for (unsigned depth = 0; depth <= 32; ++depth) {
        const std::string_view original(path);
        bool replaced = false;
        for (size_t end = path.find('/', 1); ; end = path.find('/', end + 1)) {
            if (end == std::string::npos && !followFinal) break;
            const auto prefix = original.substr(0, end);
            if (const auto link = resolve(prefix)) {
                if (lookupError && !canAccessParent(backingPath(prefix), false)) {
                    *lookupError = uint64_t(-13);
                    return {};
                }
                if (depth == 32) return {};
                auto target = link->starts_with('/') ? *link :
                    std::string(prefix.substr(0, prefix.rfind('/'))) + "/" + *link;
                if (end != std::string::npos) target.append(original.substr(end));
                path = normalizePath(target);
                replaced = true;
                break;
            }
            if (end == std::string::npos) break;
        }
        if (!replaced) return path;
    }
    return {};
}

void ProbeLinux::addDirectory(std::string path, uint32_t mode) {
    directories.emplace(std::move(path), Directory{mode & 0777});
}

void ProbeLinux::setGuestTouch(std::shared_ptr<GuestTouch> touch) {
    guestTouch = std::move(touch);
    constexpr auto configPath = "/system/usr/idc/Nestvane_Touchscreen.idc";
    constexpr auto keyLayoutPath = "/system/usr/keylayout/Nestvane_Touchscreen.kl";
    if (!guestTouch) {
        files.erase(configPath);
        files.erase(keyLayoutPath);
        fileInodesValid = false;
        return;
    }
    addDirectory("/dev/input", 0755);
    // Let Android keep this virtual touchscreen available while its display sleeps.
    static constexpr std::string_view config =
        "touch.enableForInactiveViewport = 1\n"
        "touch.wake = 1\n";
    mount(configPath, {reinterpret_cast<const uint8_t*>(config.data()), config.size()});
    static constexpr std::string_view keyLayout = "key 143 WAKEUP\nkey 158 BACK\nkey 172 HOME\nkey 580 APP_SWITCH\n";
    mount(keyLayoutPath, {reinterpret_cast<const uint8_t*>(keyLayout.data()), keyLayout.size()});
}

void ProbeLinux::addBootTmpfs(std::string path, uint32_t mode, uint32_t gid, uint64_t flags) {
    addDirectory(path, mode);
    directories.at(path).gid = gid;
    auto& group = *current->mountNamespace;
    auto& view = group.mounts.emplace(path, MountView{flags, mode, gid, 0, path}).first->second;
    setMountPropagation(view, group.rootPropagation);
}

void ProbeLinux::addBootCgroups() {
    const std::string root(cgroupRoot);
    addBootTmpfs(root, 0775, 1000, 14);
    current->mountNamespace->mounts.at(root).device = "cgroup2";
    for (const auto& path : {root, root + "/apps", root + "/system"}) {
        directories.insert_or_assign(path, Directory{0775, 1000, 1000});
        cgroupFiles.emplace(path + std::string(cgroupProcs), std::make_shared<Directory>(Directory{0664, 1000, 1000}));
    }
    kernelFiles.emplace(root + "/cgroup.controllers", "\n");
    current->process->cgroup = root;
}

bool inside(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
}

const ProbeLinux::MountView* ProbeLinux::mountFor(std::string_view path) const {
    const MountView* view = nullptr;
    size_t longest = 0;
    const bool backing = path.size() > 1 && path[1] == '\0';
    for (const auto& [mountpoint, entry] : current->mountNamespace->mounts) {
        const auto& key = backing ? entry.backing : mountpoint;
        if ((path == key || inside(path, key)) && key.size() > longest) {
            longest = key.size();
            view = &entry;
        }
    }
    return view;
}

std::string ProbeLinux::backingPath(std::string_view path) const {
    auto backing = backingPath(path, *current->mountNamespace);
    if (backing == sharedStorageRoot || inside(backing, sharedStorageRoot)) sharedStorageCall = true;
    return backing;
}

std::string ProbeLinux::backingPath(std::string_view path, const MountNamespace& mountNamespace) {
    if (path.size() > 1 && path[1] == '\0') return std::string(path);
    // The longest mount point that is the path or a directory above it: its prefixes are looked up from
    // the longest, a few look-ups where going through every mount point (up to 128) took one per mount.
    for (size_t end = path.size(); end > 0; end = path.rfind('/', end - 1)) {
        if (const auto mount = mountNamespace.mounts.find(path.substr(0, end)); mount != mountNamespace.mounts.end())
            return mount->second.backing + std::string(path.substr(end));
        if (end == 0 || path.rfind('/', end - 1) == std::string_view::npos) break;
    }
    return std::string(path);
}

bool ProbeLinux::writablePath(std::string_view path) const {
    const auto* view = mountFor(path);
    return view && !(view->flags & 1);
}

void ProbeLinux::setMountPropagation(MountView& mount, uint64_t propagation) {
    constexpr uint64_t shared = 0x100000; // MS_SHARED
    constexpr uint64_t slave = 0x80000; // MS_SLAVE
    mount.propagation = propagation;
    if (propagation == shared) {
        if (!mount.peerGroup) mount.peerGroup = nextMountPeerGroup++;
        mount.masterGroup = 0;
    } else {
        if (propagation != slave) mount.masterGroup = 0;
        else if (mount.peerGroup) mount.masterGroup = mount.peerGroup;
        mount.peerGroup = 0;
    }
}

uint64_t ProbeLinux::bindMount(std::string_view from, const std::string& to, bool recursive, std::string_view device) {
    const auto source = backingPath(from);
    const auto dest = backingPath(to);
    if (!directoryExists(source) || !directoryExists(dest)) return uint64_t(-2);
    if (from == "/" || to == "/" || inside(to, from)) return invalid;
    auto& mounts = current->mountNamespace->mounts;
    if (mounts.size() >= 128) return uint64_t(-28);
    const auto* sourceMount = mountFor(from);
    const auto* targetMount = mountFor(to);
    const auto propagation = targetMount ? targetMount->propagation : current->mountNamespace->rootPropagation;
    auto view = sourceMount ? *sourceMount : MountView{1, 0755};
    view.backing = source;
    if (!device.empty()) view.device = device;
    if (const auto mode = directories.find(source); mode != directories.end()) view.mode = mode->second.mode;
    setMountPropagation(view, view.peerGroup ? view.propagation : propagation);
    view.covered.reset();
    view.obscuredMounts.reset();
    const auto overlay = [](MountNamespace& group, const std::string& path, MountView entry) {
        if (group.mounts.size() >= 128) return;
        if (auto old = group.mounts.find(path); old != group.mounts.end())
            entry.covered = std::make_shared<MountView>(old->second);
        group.mounts.insert_or_assign(path, std::move(entry));
    };
    // The mount is seen wherever the directory it is on is seen through a mount of the same peer
    // group, or through a slave of the group: in this namespace and in the others. That is how what
    // vold mounts reaches the namespaces of the apps, which are slaves of the one they came from.
    std::vector<std::pair<MountNamespace*, std::string>> destinations{{current->mountNamespace.get(), to}};
    if (targetMount && targetMount->peerGroup) {
        std::set<MountNamespace*> groups{current->mountNamespace.get()};
        for (const auto& [tid, thread] : threads)
            if (thread.mountNamespace) groups.insert(thread.mountNamespace.get());
        for (auto* group : groups) {
            for (const auto& [mountpoint, peer] : group->mounts) {
                if (&peer == targetMount || (peer.peerGroup != targetMount->peerGroup && peer.masterGroup != targetMount->peerGroup) ||
                    (dest != peer.backing && !inside(dest, peer.backing))) continue;
                auto path = mountpoint + dest.substr(peer.backing.size());
                if (group == current->mountNamespace.get() && path == to) continue;
                // A private overmount can hide the receiving directory in an otherwise shared tree.
                const MountView* shown = nullptr;
                size_t longest = 0;
                for (const auto& [other, entry] : group->mounts)
                    if ((path == other || inside(path, other)) && other.size() > longest) {
                        longest = other.size();
                        shown = &entry;
                    }
                if (shown == &peer) destinations.emplace_back(group, std::move(path));
            }
        }
    }
    std::vector<std::pair<std::string, MountView>> children;
    if (recursive) {
        const std::string prefix = std::string(from) + "/";
        for (auto it = mounts.lower_bound(prefix); it != mounts.end() && inside(it->first, from); ++it) {
            auto entry = it->second;
            entry.covered.reset();
            entry.obscuredMounts.reset();
            children.emplace_back(it->first.substr(from.size()), std::move(entry));
        }
    }
    if (mounts.size() + children.size() + 1 > 128) return uint64_t(-28);
    for (const auto& [group, path] : destinations) {
        overlay(*group, path, view);
        for (const auto& [relative, entry] : children) overlay(*group, path + relative, entry);
        ++group->generation;
    }
    ++current->mountNamespace->generation;
    wakePollers();
    return 0;
}

uint64_t ProbeLinux::mountFilesystem(Machine& machine) {
    const auto& x = machine.cpu.x;
    if (userId() != 0) return uint64_t(-1);
    const auto target = readPath(machine, x[1]);
    if (!target) return uint64_t(-14);
    const auto path = absolutePath(*target);
    auto& group = *current->mountNamespace;
    auto& mounts = group.mounts;
    if (x[3] & 0x1000) { // MS_BIND
        if (x[3] & ~0x5000ULL) return invalid;
        const auto source = readPath(machine, x[0]);
        if (!source) return uint64_t(-14);
        return bindMount(absolutePath(*source), path, x[3] & 0x4000);
    }
    const uint64_t propagation = x[3] & 0x1e0000; // MS_UNBINDABLE/PRIVATE/SLAVE/SHARED
    if (propagation) {
        if ((propagation & (propagation - 1)) || (x[3] & ~(propagation | 0x4000)))
            return invalid;
        const auto mount = mounts.find(path);
        if (mount == mounts.end() && path != "/") return invalid;
        if (path == "/") group.rootPropagation = propagation;
        else setMountPropagation(mount->second, propagation);
        if (x[3] & 0x4000) { // MS_REC
            for (auto& [mountpoint, entry] : mounts) {
                if (path == "/" || inside(mountpoint, path)) setMountPropagation(entry, propagation);
            }
        }
        ++current->mountNamespace->generation;
        wakePollers();
        return 0;
    }
    const auto source = x[0] ? readPath(machine, x[0]) : std::optional<std::string>{""};
    const auto type = readPath(machine, x[2]);
    const auto options = x[4] ? readPath(machine, x[4]) : std::optional<std::string>{""};
    if (!source || !type || !options) return uint64_t(-14);
    if (!directoryExists(backingPath(path))) return uint64_t(-2);
    if (*type == "binder" && *source == "binder") return mountBinder(path, x[3], *options);
    if (*type == "ext4" && options->empty()) return mountExt4(*source, path, x[3]);
    if (*type == "fuse") return mountFuse(path, *options);
    const bool bpf = *type == "bpf";
    if (!bpf && *type != "tmpfs") return uint64_t(-19);
    if (bpf && (*source != "bpf" || !options->empty())) return invalid;
    if (x[3] & ~15ULL) return uint64_t(-95);
    if (mounts.contains(path)) return uint64_t(-16);
    if (mounts.size() >= 128) return uint64_t(-28);
    uint32_t mode = 01777, uid = 0, gid = 0;
    auto remaining = std::string_view(*options);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto option = remaining.substr(0, comma);
        const auto separator = option.find('=');
        if (separator == std::string_view::npos) return invalid;
        const auto key = option.substr(0, separator);
        const auto value = option.substr(separator + 1);
        uint32_t parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, key == "mode" ? 8 : 10);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return invalid;
        if (key == "mode" && parsed <= 01777) mode = parsed;
        else if (key == "uid" && parsed != UINT32_MAX) uid = parsed;
        else if (key == "gid" && parsed != UINT32_MAX) gid = parsed;
        else return invalid;
        if (comma == std::string_view::npos) break;
        remaining.remove_prefix(comma + 1);
    }
    uint64_t inherited = group.rootPropagation;
    size_t best = 0;
    for (const auto& [parent, entry] : mounts) {
        if (inside(path, parent) && parent.size() > best) {
            best = parent.size();
            inherited = entry.propagation;
        }
    }
    std::string backing("/\0", 2);
    backing += std::to_string(nextMountBacking++);
    directories.emplace(backing, Directory{mode, uid, gid});
    MountView view{x[3], mode, gid, 0, std::move(backing), {}, bpf ? "bpf" : ""};
    setMountPropagation(view, inherited);
    auto child = mounts.lower_bound(path + "/");
    if (child != mounts.end() && inside(child->first, path)) {
        // A new filesystem hides mounts attached to the previous directory tree.
        auto obscured = std::make_shared<decltype(group.mounts)>();
        while (child != mounts.end() && inside(child->first, path))
            obscured->insert(mounts.extract(child++));
        view.obscuredMounts = std::move(obscured);
    }
    mounts.emplace(path, std::move(view));
    ++current->mountNamespace->generation;
    wakePollers();
    return 0;
}

uint64_t ProbeLinux::mountFuse(const std::string& path, std::string_view options) {
    uint32_t number = UINT32_MAX;
    for (auto remaining = options; !remaining.empty();) {
        const auto option = remaining.substr(0, remaining.find(','));
        if (option.starts_with("fd=")) std::from_chars(option.data() + 3, option.data() + option.size(), number);
        remaining.remove_prefix(std::min(remaining.size(), option.size() + 1));
    }
    const auto device = descriptors().find(number);
    if (device == descriptors().end() || !device->second.fuse || device->second.fuse->mounted) return invalid;
    if (const auto status = bindMount(sharedStorageRoot, path, false, "/dev/fuse")) return status;
    auto& connection = *device->second.fuse;
    connection.mounted = true;
    connection.mountpoint = path;
    fuseMounts[path] = device->second.fuse;
    // FUSE_INIT: the header, then the protocol version, the read-ahead and what this side can do.
    constexpr uint32_t asyncRead = 1, atomicTruncate = 1 << 3, bigWrites = 1 << 5, dontMask = 1 << 6,
                       autoInvalidate = 1 << 12, readDirPlus = 1 << 13, readDirPlusAuto = 1 << 14,
                       parallelDirectories = 1 << 18, maxPages = 1 << 22;
    std::array<uint32_t, 26> init{};
    init[0] = sizeof(init);
    init[1] = 26;  // FUSE_INIT
    init[2] = 2;   // the first request's number
    init[10] = 7;
    init[11] = 36;
    init[12] = 128 * 1024;
    init[13] = asyncRead | atomicTruncate | bigWrites | dontMask | autoInvalidate | readDirPlus | readDirPlusAuto |
               parallelDirectories | maxPages;
    const auto* bytes = reinterpret_cast<const uint8_t*>(init.data());
    connection.requests.emplace_back(bytes, bytes + sizeof(init));
    wakePollers();
    return 0;
}

uint64_t ProbeLinux::mountBinder(const std::string& path, uint64_t flags, std::string_view options) {
    if (flags & ~uint64_t{15} || (!options.empty() && options != "stats=global")) return invalid;
    auto& group = *current->mountNamespace;
    if (group.mounts.contains(path)) return uint64_t(-16);
    if (group.mounts.size() >= 128 || binderDevices.size() > 60) return uint64_t(-28);
    const auto* parent = mountFor(path);
    const auto inherited = parent ? parent->propagation : group.rootPropagation;
    std::string backing("/\0", 2);
    backing += std::to_string(nextMountBacking++);
    addDirectory(backing, 0755);
    addDirectory(backing + "/features", 0555);
    for (const auto name : {"binder", "hwbinder", "vndbinder"}) {
        binderDevices.emplace(backing + "/" + name, BinderDevice{{0600}, nextBinderMinor++});
    }
    auto& view = group.mounts.emplace(path, MountView{flags | 8, 0755, 0, 0, std::move(backing),
                                                       {}, "binder", nullptr, true}).first->second;
    setMountPropagation(view, inherited);
    ++group.generation;
    wakePollers();
    return 0;
}

uint64_t ProbeLinux::mountExt4(const std::string& device, const std::string& path, uint64_t flags) {
    constexpr uint64_t allowedFlags = 0x400 | 0x80 | 15; // noatime, dirsync, standard flags
    if (!(flags & 1) || (flags & ~allowedFlags)) return invalid;
    const auto block = loopDevices.find(device);
    if (block == loopDevices.end() || block->second.bytes.empty()) return uint64_t(-19);
    auto& group = *current->mountNamespace;
    if (group.mounts.contains(path)) return uint64_t(-16);
    if (group.mounts.size() >= 128) return uint64_t(-28);
    const auto& loop = block->second;
    uint64_t offset = 0;
    std::memcpy(&offset, loop.info.data() + 24, sizeof(offset));
    const auto key = std::tuple{loop.file, offset, uint64_t(loop.bytes.size())};
    auto cached = mountedExt4.find(key);
    if (cached == mountedExt4.end()) {
        uint64_t imageBytes = loop.bytes.size();
        for (const auto& [source, mount] : mountedExt4) imageBytes += std::get<2>(source);
        if (imageBytes > 448 * 1024 * 1024 || mountedExt4.size() >= 48) return noMemory;
        try {
            auto parsed = readExt4(loop.bytes);
            if (!tetheringLoader.empty() && loop.file == "/system/apex/com.android.tethering.apex") {
                // Use the explicitly staged executable with the system APEX's own linker namespace and metadata.
                const auto program = parsed.nodes.find("/bin/netbpfload");
                if (program == parsed.nodes.end() || (program->second.mode & 0170000) != 0100000 ||
                    !(program->second.mode & 0111)) return invalid;
                auto& node = program->second;
                node.size = tetheringLoader.size();
                node.view = tetheringLoader;
                node.owned.clear();
                node.extents.clear();
            }
            std::string backing("/\0", 2);
            backing += std::to_string(nextMountBacking++);
            cached = mountedExt4.emplace(key, MountedExt4{std::move(backing), std::move(parsed)}).first;
            for (const auto& [relative, inode] : cached->second.image.nodes) {
                const auto name = cached->second.backing + relative;
                ext4Nodes.emplace(name, &inode);
                switch (inode.mode & 0170000) {
                    case 0040000:
                        addDirectory(name, inode.mode);
                        ++ext4DirectoryCount;
                        break;
                    case 0100000: mount(name, {}); break; // Name index. The ext4 node owns file contents.
                    case 0120000: {
                        const auto bytes = inode.bytes();
                        alias(name, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
                        ++ext4LinkCount;
                        break;
                    }
                }
            }
        } catch (const std::runtime_error&) {
            return invalid;
        }
    }
    const auto& volume = cached->second;
    const auto& root = volume.image.nodes.at("");
    const auto* parent = mountFor(path);
    const auto propagation = parent ? parent->propagation : group.rootPropagation;
    auto& view = group.mounts.emplace(path, MountView{flags, uint32_t(root.mode & 0777), root.gid,
                                                       0, volume.backing, {}, device, &volume.image}).first->second;
    setMountPropagation(view, propagation);
    ++group.generation;
    wakePollers();
    return 0;
}

uint64_t ProbeLinux::unmountFilesystem(Machine& machine) {
    const auto& x = machine.cpu.x;
    const auto path = readPath(machine, x[0]);
    if (!path) return uint64_t(-14);
    if (x[1] & ~15ULL) return invalid;
    if (userId() != 0) return uint64_t(-1);
    const auto visible = absolutePath(*path);
    const auto backing = backingPath(visible);
    if (!directoryExists(backing)) return uint64_t(-2);
    auto& mounts = current->mountNamespace->mounts;
    const auto found = mounts.find(visible);
    if (found == mounts.end()) return invalid;
    if ((x[1] & ~8ULL) || *path == "/dev") return uint64_t(-16);
    const auto prefix = backing + "/";
    const auto directory = directories.lower_bound(prefix);
    const auto temporary = writableFiles.lower_bound(prefix);
    const auto nested = mounts.lower_bound(visible + "/");
    const bool fuse = found->second.device == "/dev/fuse";
    if ((nested != mounts.end() && inside(nested->first, visible)) ||
        (!fuse && !found->second.ext4 && directory != directories.end() && inside(directory->first, backing)) ||
        (!fuse && temporary != writableFiles.end() && inside(temporary->first, backing))) return uint64_t(-16);
    if (fuse) {
        // The daemon's read finds the filesystem gone, and its session ends.
        if (const auto mount = fuseMounts.find(visible); mount != fuseMounts.end()) {
            if (const auto connection = mount->second.lock()) connection->unmounted = true;
            fuseMounts.erase(mount);
        }
    }
    const auto obscured = found->second.obscuredMounts;
    if (auto covered = found->second.covered) found->second = *covered;
    else mounts.erase(found);
    if (obscured) mounts.insert(obscured->begin(), obscured->end());
    ++current->mountNamespace->generation;
    wakePollers();
    return 0;
}

static uint64_t procId(std::string_view text) {
    if (text.empty()) return 0;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.end() ? value : 0;
}

const ProbeLinux::Thread* ProbeLinux::visibleProcThread(uint64_t tid) const {
    const auto found = threads.find(tid);
    if (found == threads.end() || found->second.wait == Wait::exited) return nullptr;
    const auto& target = *found->second.process;
    const auto& reader = *current->process;
    // Android mounts procfs with hidepid=invisible and grants the readproc group access.
    constexpr uint32_t readproc = 3009;
    constexpr uint64_t capSysPtrace = 1ULL << 19;
    const bool mayRead = &target == &reader || userId() == 0 || (reader.effectiveCaps & capSysPtrace) ||
        groupId() == readproc || std::ranges::find(reader.groups, readproc) != reader.groups.end() ||
        std::ranges::all_of(target.userIds, [&](uint32_t uid) { return uid == userId(); });
    return mayRead ? &found->second : nullptr;
}

const ProbeLinux::Process* ProbeLinux::procTaskDirectory(std::string_view path) const {
    constexpr std::string_view prefix = "/proc/", suffix = "/task";
    if (!path.starts_with(prefix) || !path.ends_with(suffix) || path.size() <= prefix.size() + suffix.size())
        return nullptr;
    const auto id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (id == "self") return current->process.get();
    const auto tid = procId(id);
    const auto* subject = visibleProcThread(tid);
    return subject && subject->process->id == tid ? subject->process.get() : nullptr;
}

uint64_t ProbeLinux::procThreadId(std::string_view task) const {
    if (task == "self") return processId();
    if (task == "thread-self") return currentTid;
    if (const auto tid = procId(task)) return tid;
    const auto slash = task.rfind('/');
    if (slash == std::string_view::npos) return 0;
    const auto* owner = procTaskDirectory("/proc/" + std::string(task.substr(0, slash)));
    const auto tid = procId(task.substr(slash + 1));
    const auto* subject = visibleProcThread(tid);
    return owner && subject && subject->process.get() == owner ? tid : 0;
}

std::string ProbeLinux::procCommandLine(const Process& process, const Machine* machine) const {
    if (!process.argvAddress || !machine) return process.commandLine;
    const auto& segments = process.id == processId() ? machine->segments : process.segments;
    std::string result(process.commandLine.size(), '\0');
    uint64_t address = process.argvAddress;
    size_t copied = 0;
    while (copied < result.size()) {
        const auto next = std::upper_bound(segments.begin(), segments.end(), address,
            [](uint64_t address, const Segment& segment) { return address < segment.address; });
        if (next == segments.begin()) break;
        const auto& area = *std::prev(next);
        const auto offset = address - area.address;
        if (!(area.flags & 4) || offset >= area.accessibleSize()) break;
        const auto length = std::min<uint64_t>(result.size() - copied, area.accessibleSize() - offset);
        if (area.anonymousPages)
            area.anonymousPages->readInto(area.fileOffset + offset,
                {reinterpret_cast<uint8_t*>(result.data() + copied), length});
        else
            std::memcpy(result.data() + copied, area.data() + offset, length);
        copied += length;
        address += length;
    }
    result.resize(copied);
    return result;
}

bool ProbeLinux::directoryExists(std::string_view path) const {
    if (path.starts_with("/proc/")) {
        const auto id = path.substr(sizeof("/proc/") - 1);
        if (const auto tid = procId(id)) return visibleProcThread(tid) != nullptr;
        if (procTaskDirectory(path)) return true;
        const auto slash = path.rfind('/');
        if (const auto tid = procId(path.substr(slash + 1))) {
            if (const auto* owner = procTaskDirectory(path.substr(0, slash))) {
                const auto* subject = visibleProcThread(tid);
                return subject && subject->process.get() == owner;
            }
        }
    }
    if (path == "/" || directories.contains(path)) return true;
    const std::string prefix = std::string(path) + "/";
    const auto file = files.lower_bound(prefix);
    const auto link = links.lower_bound(prefix);
    const auto directory = directories.lower_bound(prefix);
    return (directory != directories.end() && directory->first.starts_with(prefix)) ||
           (file != files.end() && file->first.starts_with(prefix)) ||
           (link != links.end() && link->first.starts_with(prefix));
}

std::string ProbeLinux::mountList(const MountNamespace& group) {
    std::string list = "rootfs / rootfs ro 0 0\n";
    for (const auto& [path, mount] : group.mounts) {
        std::string escaped;
        for (const char c : path) {
            if (c == ' ') escaped += "\\040";
            else if (c == '\t') escaped += "\\011";
            else if (c == '\n') escaped += "\\012";
            else if (c == '\\') escaped += "\\134";
            else escaped += c;
        }
        list += !mount.device.empty() ? mount.device : mount.hostData ? "nestvane-data" : "tmpfs";
        const auto type = mount.binder ? " binder " : mount.ext4 ? " ext4 " : mount.device == "/dev/fuse" ? " fuse " :
                          mount.device == "bpf" ? " bpf " : mount.device == "cgroup2" ? " cgroup2 " :
                          mount.hostData ? " nestvane-data " : " tmpfs ";
        list += " " + escaped + type + ((mount.flags & 1) ? "ro" : "rw");
        if (mount.flags & 2) list += ",nosuid";
        if (mount.flags & 4) list += ",nodev";
        if (mount.flags & 8) list += ",noexec";
        if (mount.flags & 0x80) list += ",dirsync";
        if (mount.flags & 0x400) list += ",noatime";
        list += " 0 0\n";
    }
    return list;
}

namespace {
ProbeLinux::HostVolume statfsVolume() {
    struct statfs status{};
    const char* home = std::getenv("HOME");
    if (::statfs(home && *home ? home : "/", &status) != 0) return {};
    return {uint64_t(status.f_blocks) * status.f_bsize, uint64_t(status.f_bavail) * status.f_bsize};
}
std::atomic<ProbeLinux::HostVolume (*)()> hostVolumeSource{statfsVolume};
}

void ProbeLinux::setHostVolumeSource(HostVolume (*source)()) {
    hostVolumeSource = source ? source : statfsVolume;
}

// Asked at most once every few seconds: statfs asks for every library the linker opens, and the
// question to iOS is not free. Each new answer has a new generation.
ProbeLinux::HostVolume ProbeLinux::hostVolume() {
    static std::mutex lock;
    static HostVolume cached;
    static std::chrono::steady_clock::time_point asked;
    static HostVolume (*askedSource)() = nullptr;
    static uint64_t generation = 0;
    std::lock_guard guard(lock);
    const auto now = std::chrono::steady_clock::now();
    const auto source = hostVolumeSource.load();
    if (source != askedSource || now - asked > std::chrono::seconds(5) || !cached.total) {
        cached = source();
        cached.generation = ++generation;
        asked = now;
        askedSource = source;
    }
    return cached;
}

// What was free when the host was last asked, less what /data has grown by since: the host's number
// only catches up with the guest's writes at the next ask.
// A volume holds a little more than the size it is sold as, and Android rounds a total up to the next
// power of two: 128 GB and a bit showed as 256 GB. Within 5% above a size, the size is what is shown.
static uint64_t nominalCapacity(uint64_t bytes) {
    for (uint64_t size = 16'000'000'000ULL; size <= 64'000'000'000'000ULL; size *= 2)
        if (bytes > size && bytes <= size + size / 20) return size;
    return bytes;
}

uint64_t ProbeLinux::persistentLimit(uint64_t used) const {
    const auto volume = hostVolume();
    // No answer from the host: the old fixed budget.
    if (!volume.total) return maxTransientBytes;
    if (volume.generation != volumeGeneration) {
        volumeGeneration = volume.generation;
        volumeUsed = used;
    }
    return volumeUsed + (volume.available > hostReserveBytes ? volume.available - hostReserveBytes : 0);
}

uint64_t ProbeLinux::writableAllocation(bool persistent) const {
    uint64_t bytes = 0;
    for (const auto& [path, file] : writableFiles) {
        if (file->persistent == persistent) bytes += file->storage->pages.size();
    }
    return bytes;
}

uint64_t ProbeLinux::statFilesystem(Machine& machine, uint64_t address, std::string_view path) const {
    std::array<uint64_t, 15> stat{};
    stat[0] = 0x01021994; // TMPFS_MAGIC
    stat[1] = pageSize;
    // The linker asks for every library it opens, and the image has tens of thousands of files.
    static thread_local struct { const void* owner = nullptr; size_t count = 0; uint64_t blocks = 0; } total;
    if (total.owner != this || total.count != files.size()) {
        total = {this, files.size(), 0};
        for (const auto& [path, bytes] : files) {
            const auto ext4 = ext4Nodes.find(path);
            const auto size = ext4 == ext4Nodes.end() ? bytes.size() : ext4->second->size;
            total.blocks += pageAlignedLength(size) / pageSize;
        }
    }
    stat[2] = total.blocks;
    stat[5] = files.size();
    stat[8] = 255;
    stat[9] = pageSize;
    const auto* view = mountFor(path);
    stat[10] = 0x20 | (view ? view->flags & 15 : 1); // ST_VALID and basic per-mount flags.
    if (view && view->device == "bpf") stat[0] = 0xcafe4a11; // BPF_FS_MAGIC
    if (view && view->device == "cgroup2") stat[0] = 0x63677270; // CGROUP2_SUPER_MAGIC
    if (view && view->binder) stat[0] = 0x6c6f6f70; // BINDERFS_SUPER_MAGIC
    if (view && view->ext4) {
        stat[0] = 0xef53;
        stat[1] = stat[9] = view->ext4->blockSize;
        stat[2] = view->ext4->blockCount;
        stat[5] = view->ext4->nodes.size();
    }
    if (dataPath(backingPath(path))) {
        // The host's volume: Settings shows its size and what is free, as the device's own Settings does.
        // Android takes the total as /data's and /system's together, rounded up to a power of two, so
        // /data is the volume less what statfs says /system holds: a 256 GB phone showed 512 GB.
        const auto volume = hostVolume();
        const auto used = writableAllocation(true);
        const auto limit = persistentLimit(used);
        uint64_t systemBytes = total.blocks * pageSize;
        if (const auto* system = mountFor("/system"); system && system->ext4)
            systemBytes = system->ext4->blockCount * system->ext4->blockSize;
        const auto shown = volume.total ? nominalCapacity(volume.total) : limit;
        const auto dataTotal = shown > systemBytes ? shown - systemBytes : shown;
        const auto room = std::min(limit, dataTotal);
        stat[2] = dataTotal / pageSize;
        stat[3] = stat[4] = (room - std::min(room, used)) / pageSize;
        const auto filesUsed = std::ranges::count_if(writableFiles, [](const auto& entry) {
            return entry.second->persistent;
        });
        stat[5] = maxPersistentFiles;
        stat[6] = maxPersistentFiles - std::min(maxPersistentFiles, size_t(filesUsed));
    }
    if (!bootTransitions.empty() && (path == "/sys/fs/selinux" || inside(path, "/sys/fs/selinux"))) {
        stat[0] = 0xf97cff8c; // SELINUX_MAGIC
        stat[10] = 0x2e;
    }
    return writeGuest(machine, address, stat) ? 0 : uint64_t(-14);
}

size_t ProbeLinux::exportFiles(ProbeLinux& reader, std::string_view prefix) const {
    size_t count = 0;
    for (const auto& [path, file] : writableFiles) {
        if (!path.starts_with(prefix)) continue;
        reader.mount(path, std::span(file->storage->pages).first(file->storage->size));
        // Static mounts can imply a path exists, but traversal under /dev checks directory metadata.
        const std::string_view filename = path;
        for (auto separator = filename.rfind('/'); separator != std::string_view::npos && separator > 0;
             separator = filename.rfind('/', separator - 1)) {
            const auto directory = directories.find(filename.substr(0, separator));
            if (directory != directories.end())
                reader.directories.try_emplace(directory->first, directory->second);
        }
        ++count;
    }
    return count;
}

std::span<const uint8_t> ProbeLinux::bootGuestFile(std::string_view path) const {
    const auto init = threads.find(pid);
    if (init == threads.end()) return {};
    const auto file = writableFiles.find(backingPath(path, *init->second.mountNamespace));
    if (file == writableFiles.end()) return {};
    const auto& storage = *file->second->storage;
    return std::span(storage.pages).first(storage.size);
}

bool ProbeLinux::exportExt4Mount(ProbeLinux& reader, std::string_view path) const {
    // The boot probe exits with whichever runnable task was last selected.
    // Inspect retained namespaces to find the active mount created by apexd.
    const auto findMount = [&](const MountNamespace& group) -> const MountView* {
        const auto found = group.mounts.find(path);
        return found != group.mounts.end() && found->second.ext4 ? &found->second : nullptr;
    };
    const MountView* view = nullptr;
    for (const auto& [tid, thread] : threads) {
        view = findMount(*thread.mountNamespace);
        if (view) break;
        for (const auto& [fd, entry] : thread.process->descriptors) {
            if (entry.mountNamespace) view = findMount(*entry.mountNamespace);
            if (view) break;
        }
        if (view) break;
    }
    if (!view) return false;
    const auto import = [&](const auto& node) {
        const auto& [name, inode] = node;
        reader.ext4Nodes.emplace(name, inode);
        if (auto file = files.find(name); file != files.end()) {
            reader.files.emplace(*file);
            reader.fileInodesValid = false;
        }
        if (auto directory = directories.find(name); directory != directories.end() &&
            reader.directories.emplace(*directory).second) ++reader.ext4DirectoryCount;
        if (auto link = links.find(name); link != links.end() &&
            reader.links.emplace(*link).second) ++reader.ext4LinkCount;
    };
    import(*ext4Nodes.find(view->backing));
    for (auto it = ext4Nodes.lower_bound(view->backing + "/");
         it != ext4Nodes.end() && inside(it->first, view->backing); ++it)
        import(*it);
    auto exported = *view;
    exported.covered.reset();
    reader.current->mountNamespace->mounts.emplace(path, std::move(exported));
    return true;
}

// Tells the dynamic linker where a translated file belongs, once per process and file: a record of
// the file's device and inode, as fstat has just reported them at `status`, and the address.
void ProbeLinux::notePrelink(Machine& machine, const Descriptor& fd, uint64_t status) {
    if (fd.node != Node::regular || fd.temporary) return;
    std::array<uint64_t, 3> record{};
    if (!machine.readInto(status, {reinterpret_cast<uint8_t*>(record.data()), 16})) return;
    const std::pair<uint64_t, uint64_t> identity{record[0], record[1]};
    // Inode numbers are not kept for what a file is: the same number can come to stand for another file.
    auto& prelink = machine.space->prelink;
    // Only an ELF file can have a translation. Every file that is opened comes by here, and the
    // bytes of a file in a mounted image may first have to be put together from its extents.
    const auto head = fd.ext4File ? fd.ext4File->head(4) : fd.bytes.first(std::min<size_t>(4, fd.bytes.size()));
    const bool elf = head.size() == 4 && !std::memcmp(head.data(), "\x7f" "ELF", 4);
    const auto contents = elf ? fd.contents() : std::span<const uint8_t>{};
    const auto* first = elf ? native::moduleFor(contents) : nullptr;
    const auto known = std::find(prelink.files.begin(), prelink.files.end(), identity);
    if (known != prelink.files.end()) {
        // The numbers are in the table already. If they were another file's when they went in, the
        // record would send this file to that file's address: it comes out, and this file gets its own.
        const size_t slot = known - prelink.files.begin();
        if (first && prelink.keys[slot] == first->key) return;
        const std::array<uint64_t, 3> none{};
        machine.write(prelink.table + slot * 24, {reinterpret_cast<const uint8_t*>(none.data()), 24});
        *known = {0, 0};
    }
    if (!first || prelink.files.size() >= GuestSpace::Prelink::capacity) return;
    // The same bytes may be several files, and each needs a translation of its own in one process.
    const native::Module* module = nullptr;
    for (unsigned copy = 0; (module = native::moduleFor(contents, copy)); ++copy)
        if (std::find(prelink.used.begin(), prelink.used.end(), module->base) == prelink.used.end()) break;
    if (!module) {
        if (static const bool report = std::getenv("NESTVANE_NATIVE_TRACE"); report)
            std::fprintf(stderr, "[native] no translation left for %s (device %llu inode %llu) in pid %llu\n", fd.path.c_str(),
                         static_cast<unsigned long long>(record[0]), static_cast<unsigned long long>(record[1]),
                         static_cast<unsigned long long>(processId()));
        return;
    }
    if (static const bool report = std::getenv("NESTVANE_NATIVE_TRACE"); report && fd.path.find(std::getenv("NESTVANE_NATIVE_TRACE")) != std::string::npos)
        std::fprintf(stderr, "[native] prelink %s (device %llu inode %llu) at 0x%llx in pid %llu\n", fd.path.c_str(),
                     static_cast<unsigned long long>(record[0]), static_cast<unsigned long long>(record[1]),
                     static_cast<unsigned long long>(module->base), static_cast<unsigned long long>(processId()));
    record[2] = module->base;
    if (machine.write(prelink.table + prelink.files.size() * 24, {reinterpret_cast<const uint8_t*>(record.data()), 24})) {
        prelink.files.push_back(identity);
        prelink.keys.push_back(module->key);
        prelink.addresses.push_back(module->base);
        prelink.used.push_back(module->base);
    }
}

uint64_t ProbeLinux::stat(Machine& machine, uint64_t address, const Descriptor& fd, bool extended) const {
    const auto result = [&](uint64_t size, uint64_t inode, uint32_t mode, uint32_t uid = 0,
                            uint32_t gid = 0, uint64_t device = 0, const GuestInodeTimes& times = {}) {
        return statFile(machine, address, extended, size, inode, mode, uid, gid, device, times);
    };
    if (fd.node == Node::touchDevice) return result(0, 70'265, 0020660, 0, 1004, 0x0d40);
    if (fd.node == Node::graphicsAddressSpace) return result(0, 70'266, 0020666, 1000, 1000, 0x100a0a);
    if (fd.graphics && fd.graphics->vsock) return result(0, 70'269, 0140777);
    if (fd.node == Node::graphicsPipe) return result(0, 70'267, 0020666, 1000, 1000, 0x100a0b);
    if (fd.node == Node::ashmem) return result(0, 69'999, 0020666, 0, 0, 0x100a03);
    if (fd.temporary) {
        const auto& file = *fd.temporary;
        return result(file.storage->size, file.inode, 0100000 | file.mode, file.uid, file.gid, 0, file.times);
    }
    if (fd.node == Node::oomScoreAdjustment) {
        const auto* subject = liveProcess(fd.targetProcess);
        return subject ? result(0, std::hash<std::string>{}(fd.path) | 1, 0100644,
                                subject->userIds[1], subject->groupIds[1]) : uint64_t(-3);
    }
    if (fd.node == Node::kernelSetting || fd.node == Node::ueventTrigger || fd.node == Node::pressure)
        return result(0, std::hash<std::string>{}(fd.path) | 1, 0100644);
    if (fd.cgroupFile)
        return result(0, std::hash<std::string>{}(fd.path) | 1,
                        0100000 | fd.cgroupFile->mode, fd.cgroupFile->uid, fd.cgroupFile->gid);
    if (fd.node == Node::ptyMaster) return result(0, 3, 0020666, 0, 0, 0x502);
    if (fd.node == Node::ptySlave && fd.pty)
        return result(0, 10000 + fd.pty->number, 0020620, fd.pty->owner, 5, 0x8800 | fd.pty->number);
    if (fd.node == Node::binder) {
        const auto& device = binderDevices.at(fd.path);
        constexpr uint64_t guestBinderMajor = 240; // Guest character-device numbers.
        return result(0, 70'000 + device.minor, 0020000 | device.mode,
                        device.uid, device.gid, guestBinderMajor << 8 | device.minor);
    }
    if (fd.node == Node::loopControl) return result(0, 3, 0020600, 0, 0, 0xaed);
    if (fd.node == Node::loopDevice) {
        const auto& loop = loopDevices.at(fd.path);
        return result(0, 65536 + loop.number, 0060000 | loop.mode,
                        loop.uid, loop.gid, 0x700 | loop.number);
    }
    if (const auto node = ext4Nodes.find(fd.path); node != ext4Nodes.end()) {
        const auto& inode = *node->second;
        return result(inode.size, std::hash<std::string>{}(fd.path) | 1,
                        inode.mode, inode.uid, inode.gid);
    }
    if (fd.node == Node::directory) {
        const auto pos = directories.find(fd.path);
        const auto node = pos != directories.end() ? pos->second : Directory{0755};
        return result(pageSize, std::hash<std::string>{}(fd.path) | 1,
                        0040000 | node.mode, node.uid, node.gid, 0, node.times);
    }
    if (fd.mountNamespace)
        return result(0, fd.mountNamespace->id, 0100444);
    if (fd.snapshot) return result(0, 4, 0100444);
    if (fd.node == Node::entropy)
        return result(0, 5, 0020666, 0, 0, fd.path == "/dev/random" ? 0x108 : 0x109);
    if (kernelFiles.contains(fd.path))
        return result(fd.bytes.size(), std::hash<std::string>{}(fd.path) | 1, 0100444);
    if (fd.node == Node::keySocket) return result(0, 3, 0140777);
    if (fd.node == Node::socket) {
        const auto bound = socketPaths.find(fd.path);
        if (bound != socketPaths.end()) {
            const auto& node = bound->second;
            return result(0, 3, 0140000 | node.mode, node.uid, node.gid);
        }
        return result(0, 3, 0140777);
    }
    if (fd.pipe) {
        if (const auto fifo = fifoPaths.find(fd.path); fifo != fifoPaths.end())
            return result(0, std::hash<std::string>{}(fd.path) | 1, 0010000 | fifo->second.mode, fifo->second.uid, fifo->second.gid);
        return result(0, 3, 0010600);
    }
    if (fd.node != Node::regular) return result(0, 3, 0020666);
    const auto file = files.find(fd.path);
    const auto mode = fd.path.starts_with("/data/") || fd.path.find("/bin/") != std::string::npos
        ? 0100555 : 0100444;
    return result(fd.bytes.size(), fileInode(file), mode);
}

uint64_t ProbeLinux::statx(Machine& machine) const {
    constexpr uint32_t atCwd = uint32_t(-100), noFollow = 0x100, noAutomount = 0x800, emptyPath = 0x1000;
    constexpr uint32_t syncType = 0x6000, reservedMask = 0x80000000;
    const auto& x = machine.cpu.x;
    const uint32_t directory = x[0], flags = x[2], mask = x[3];
    if (flags & ~(noFollow | noAutomount | emptyPath | syncType) ||
        (flags & syncType) == syncType || (mask & reservedMask)) return invalid;
    const auto path = !x[1] && (flags & emptyPath) ? std::optional<std::string>{""} : readPath(machine, x[1]);
    if (!path) return uint64_t(-14);
    if (path->empty() && !(flags & emptyPath)) return uint64_t(-2);
    if (path->empty() && directory != atCwd) {
        const auto fd = descriptors().find(directory);
        return fd == descriptors().end() ? uint64_t(-9) : stat(machine, x[4], fd->second, true);
    }
    std::string absolute;
    if (const auto error = resolveAtPath(directory, *path, absolute)) return error;
    const auto visible = followLinks(std::move(absolute), !(flags & noFollow));
    if (!visible) return uint64_t(-40);
    const auto backing = backingPath(*visible);
    if (!canAccessParent(backing, false)) return uint64_t(-13);
    if (flags & noFollow) {
        if (const auto link = resolve(*visible)) {
            return statFile(machine, x[4], true, link->size(), std::hash<std::string>{}(backing) | 1,
                            0120777, 0, 0, 0, {});
        }
    }
    const auto fd = find(backing, 0, &machine);
    return fd ? stat(machine, x[4], *fd, true) : uint64_t(-2);
}

uint64_t ProbeLinux::freeMemory(const Machine& machine) const {
    uint64_t resident = anonymousBudget->dirty;
    for (const auto& segment : machine.segments) resident += segment.bytes.size();
    return memoryLimit - std::min(memoryLimit, resident);
}

std::optional<ProbeLinux::Descriptor> ProbeLinux::find(std::string_view path, uint32_t flags,
                                                       const Machine* machine) const {
    if (path == "/proc/meminfo") {
        Descriptor entry{std::string(path), {}, flags, Node::regular};
        entry.snapshot = std::make_shared<std::string>();
        if (machine) {
            constexpr uint64_t bytesPerKiB = 1024;
            const auto available = freeMemory(*machine) / bytesPerKiB;
            std::ostringstream info;
            info << "MemTotal: " << memoryLimit / bytesPerKiB << " kB\n"
                 << "MemFree: " << available << " kB\n"
                 << "MemAvailable: " << available << " kB\n"
                 << "AnonPages: " << anonymousBudget->dirty / bytesPerKiB << " kB\n"
                 << "SwapTotal: 0 kB\n"
                 << "SwapFree: 0 kB\n";
            *entry.snapshot = std::move(info).str();
            entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
        }
        return entry;
    }
    const auto taskPath = "/proc/self/task/" + std::to_string(currentTid);
    if (const auto file = cgroupFiles.find(path); file != cgroupFiles.end()) {
        Descriptor entry{file->first, {}, flags, Node::cgroupProcs};
        entry.cgroupFile = file->second;
        return entry;
    }
    if (!current->process->cgroup.empty() &&
        (path == "/proc/self/cgroup" || path == "/proc/thread-self/cgroup" ||
         path == "/proc/" + std::to_string(processId()) + "/cgroup" || path == taskPath + "/cgroup")) {
        const auto relative = current->process->cgroup.substr(cgroupRoot.size());
        Descriptor entry{std::string(path), {}, flags, Node::regular};
        entry.snapshot = std::make_shared<std::string>("0::" + (relative.empty() ? "/" : relative) + "\n");
        entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
        return entry;
    }
    if (path.starts_with("/proc/") && path.ends_with("/attr/current")) {
        const Process* subject = nullptr;
        if (path == "/proc/thread-self/attr/current" || path == "/proc/self/attr/current" ||
            path == taskPath + "/attr/current") {
            subject = current->process.get();
        } else if (path.size() > std::string_view("/proc//attr/current").size()) {
            const auto pidText = path.substr(6, path.size() - std::string_view("/proc//attr/current").size());
            uint64_t target = 0;
            const auto [end, error] = std::from_chars(pidText.begin(), pidText.end(), target);
            if (error == std::errc{} && end == pidText.end() && target) {
                const auto thread = threads.find(target);
                if (thread != threads.end() && thread->second.wait != Wait::exited)
                    subject = thread->second.process.get();
            }
        }
        if (!subject) return {};
        Descriptor entry{std::string(path), {}, flags, Node::securityInfo};
        entry.targetProcess = subject->id;
        entry.securityReply = std::make_shared<std::string>(subject->context);
        entry.securityReply->push_back('\0');
        return entry;
    }
    if ((!bootTransitions.empty() && path == "/sys/fs/selinux/create") ||
        (!accessClasses.empty() && path == "/sys/fs/selinux/access")) {
        const auto node = path.ends_with("/create") ? Node::securityCreate : Node::securityAccess;
        Descriptor entry{std::string(path), {}, flags, node};
        entry.securityReply = std::make_shared<std::string>();
        return entry;
    }
    if (path == "/sys/fs/selinux/context" && (!currentContextTransitions.empty() || !appDataLabelTypes.empty() || !appTransitionDomains.empty()))
        return Descriptor{std::string(path), {}, flags, Node::securityValidate};
    const bool isOomScore = path.ends_with("/oom_score_adj");
    const bool isCmdline = path.ends_with("/cmdline");
    const std::string_view procSuffix = isOomScore ? "/oom_score_adj" : isCmdline ? "/cmdline" : "/status";
    if (path.starts_with("/proc/") && path.size() > 6 + procSuffix.size() && path.ends_with(procSuffix)) {
        auto task = path.substr(6);
        task.remove_suffix(procSuffix.size());
        const auto tid = procThreadId(task);
        const auto* subject = visibleProcThread(tid);
        if (!subject) return {};
        const auto& thread = *subject;
        const auto& process = *thread.process;
        if (isCmdline) {
            Descriptor entry{std::string(path), {}, flags, Node::regular};
            entry.snapshot = std::make_shared<std::string>(procCommandLine(process, machine));
            entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
            return entry;
        }
        if (isOomScore) {
            Descriptor entry{std::string(path), {}, flags, Node::oomScoreAdjustment};
            entry.targetProcess = process.id;
            return entry;
        }
        const auto state = thread.wait == Wait::none ? "R (running)" :
            thread.wait == Wait::suspended ? "T (stopped)" : "S (sleeping)";
        std::ostringstream listing;
        listing << "Name:\t" << reinterpret_cast<const char*>(thread.name.data()) << '\n'
                << "State:\t" << state << '\n'
                << "Tgid:\t" << process.id << '\n'
                << "Pid:\t" << tid << '\n'
                << "PPid:\t" << process.parent << '\n'
                << "TracerPid:\t0\n" // Guest ptrace attachments are unsupported.
                << "Uid:\t" << process.userIds[0] << '\t' << process.userIds[1] << '\t'
                << process.userIds[2] << '\t' << process.userIds[1] << '\n'
                << "Gid:\t" << process.groupIds[0] << '\t' << process.groupIds[1] << '\t'
                << process.groupIds[2] << '\t' << process.groupIds[1] << '\n'
                << "Groups:\t";
        for (const auto group : process.groups) listing << group << ' ';
        listing << '\n';
        Descriptor entry{std::string(path), {}, flags, Node::regular};
        entry.snapshot = std::make_shared<std::string>(std::move(listing).str());
        entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
        return entry;
    }
    if (path.starts_with("/sys/class/power_supply/")) const_cast<ProbeLinux*>(this)->refreshBattery();
    if (const auto file = kernelFiles.find(path); file != kernelFiles.end()) {
        const auto& content = file->second;
        Descriptor entry{file->first, {}, flags, Node::regular};
        // What changes while it is open is read from a copy.
        if ((path.starts_with("/sys/devices/virtual/block/") && path.ends_with("/loop/backing_file")) ||
            path.starts_with("/sys/class/power_supply/"))
            entry.snapshot = std::make_shared<std::string>(content);
        const auto& bytes = entry.snapshot ? *entry.snapshot : content;
        entry.bytes = {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        return entry;
    }
    const auto taskAttribute = [&](std::string_view attribute) {
        return path.ends_with(attribute) && (path == "/proc/thread-self/attr" + std::string(attribute) ||
            path == "/proc/self/attr" + std::string(attribute) || path == taskPath + "/attr" + std::string(attribute));
    };
    if ((fsCreatePermission && taskAttribute("/fscreate")) ||
        (socketCreatePermission && taskAttribute("/sockcreate"))) {
        const bool socket = path.ends_with("/sockcreate");
        Descriptor entry{std::string(path), {}, flags, socket ? Node::socketCreateLabel : Node::fsCreateLabel};
        entry.securityReply = std::make_shared<std::string>(socket ? current->socketCreateContext : current->fsCreateContext);
        if (!entry.securityReply->empty()) entry.securityReply->push_back('\0');
        return entry;
    }
    if (path == "/proc/thread-self/attr/exec" || path == "/proc/self/attr/exec" ||
        path == taskPath + "/attr/exec")
        return Descriptor{std::string(path), {}, flags, Node::execLabel};
    if (path == "/proc/self/ns/mnt") {
        Descriptor entry{std::string(path), {}, flags, Node::mountNamespace};
        entry.mountNamespace = current->mountNamespace;
        return entry;
    }
    if (path == "/proc/mounts" || path == "/proc/self/mounts") {
        Descriptor entry{std::string(path), {}, flags, Node::regular};
        entry.snapshot = std::make_shared<std::string>(mountList(*current->mountNamespace));
        entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
        entry.event = std::make_shared<EventState>();
        entry.event->mountNamespace = current->mountNamespace;
        entry.event->observedMountGeneration = current->mountNamespace->generation;
        return entry;
    }
    if (const auto file = writableFiles.find(path); file != writableFiles.end()) {
        Descriptor fd{file->first, {}, flags, Node::temporary};
        fd.temporary = file->second;
        return fd;
    }
    if (const auto file = files.find(path); file != files.end()) {
        Descriptor entry{file->first, file->second, flags, Node::regular};
        if (const auto node = ext4Nodes.find(path); node != ext4Nodes.end()) entry.ext4File = node->second;
        return entry;
    }
    if (socketPaths.contains(path)) return Descriptor{std::string(path), {}, flags, Node::socket};
    if (const auto fifo = fifoPaths.find(path); fifo != fifoPaths.end()) {
        Descriptor entry{std::string(path), {}, flags, (flags & 3) == 1 ? Node::pipeWrite : Node::pipeRead};
        entry.pipe = fifo->second.pipe; // open() gives it its ends
        return entry;
    }
    if (hostNetwork && isInterfaceSetting(path))
        const_cast<ProbeLinux*>(this)->kernelSettings.try_emplace(std::string(path), Setting{0, 0, UINT32_MAX});
    if (kernelSettings.contains(path)) return Descriptor{std::string(path), {}, flags, Node::kernelSetting};
    if (path == "/proc/pressure/memory" || path == "/proc/pressure/io" || path == "/proc/pressure/cpu") {
        // Pressure stall information. lmkd will not run without it. The guest's memory is the host's
        // to manage, so the figures are zero and a registered trigger never fires.
        Descriptor entry{std::string(path), {}, flags, Node::pressure};
        entry.securityReply = std::make_shared<std::string>(
            "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        return entry;
    }
    if (ueventTriggers.contains(path)) return Descriptor{std::string(path), {}, flags, Node::ueventTrigger};
    if (path == "/dev/ptmx") return Descriptor{std::string(path), {}, flags, Node::ptyMaster};
    if (path == "/dev/ashmem" || path == ashmemBootPath) return Descriptor{std::string(path), {}, flags, Node::ashmem};
    if (const auto device = ptys.find(path); device != ptys.end()) {
        if (auto terminal = device->second.lock(); terminal && !terminal->ends[0].expired()) {
            Descriptor entry{std::string(path), {}, flags, Node::ptySlave};
            entry.pty = std::move(terminal);
            return entry;
        }
    }
    if (path == "/dev/loop-control") return Descriptor{std::string(path), {}, flags, Node::loopControl};
    if (binderDevices.contains(path)) return Descriptor{std::string(path), {}, flags, Node::binder};
    if (loopDevices.contains(path)) return Descriptor{std::string(path), {}, flags, Node::loopDevice};
    if (guestTouch && path == touchDevicePath)
        return Descriptor{std::string(path), {}, flags, Node::touchDevice};
    if ((graphicsPipeFactory || camera) && path == graphicsAddressDevice)
        return Descriptor{std::string(path), {}, flags, Node::graphicsAddressSpace};
    if (path == graphicsDevice && (graphicsPipeFactory || camera || current->process->context == sensorsHalContext))
        return Descriptor{std::string(path), {}, flags, Node::graphicsPipe};
    if (path == "/dev/null") return Descriptor{std::string(path), {}, flags, Node::null};
    if (path.starts_with("/dev/snd/")) {
        if (static const bool traced = std::getenv("NESTVANE_SOUND_TRACE"); traced && androidLogObserver)
            androidLogObserver("native execution: sound open " + std::string(path));
    }
    if ((path == "/dev/snd/pcmC0D0p" || path == "/dev/snd/pcmC0D0c" || path == "/dev/snd/controlC0") && directoryExists("/dev/snd"))
        return Descriptor{std::string(path), {}, flags, Node::sound};
    if (path.starts_with("/dev/dma_heap/system") && directoryExists("/dev/dma_heap"))
        return Descriptor{std::string(path), {}, flags, Node::dmaHeap};
    if (path == "/dev/fuse") {
        Descriptor entry{std::string(path), {}, flags, Node::fuse};
        entry.fuse = std::make_shared<FuseConnection>();
        return entry;
    }
    if (path == "/dev/random" || path == "/dev/urandom")
        return Descriptor{std::string(path), {}, flags, Node::entropy};
    if (path == "/dev/kmsg") return Descriptor{std::string(path), {}, flags, Node::kernelLog};
    if (directoryExists(path)) {
        Descriptor result{std::string(path), {}, flags, Node::directory};
        if (const auto* subject = procTaskDirectory(path)) result.targetProcess = subject->id;
        if (path == cgroupRoot || inside(path, cgroupRoot)) {
            const auto node = cgroupFiles.find(result.path + std::string(cgroupProcs));
            if (node != cgroupFiles.end()) result.cgroupDirectory = node->second;
        }
        return result;
    }
    return {};
}

uint64_t ProbeLinux::resize(const WritableFile& file, uint64_t size) {
    if ((size < file.storage->size && (file.seals & sealShrink)) ||
        (size > file.storage->size && (file.seals & sealGrow))) return uint64_t(-1);
    if (size > (file.persistent || file.memoryFile ? maxDataFileBytes : maxGuestFileBytes)) return uint64_t(-27);
    const auto pages = pageAlignedLength(size);
    if (pages > file.storage->pages.size()) {
        uint64_t allocated = 0;
        if (file.memoryFile) {
            std::erase_if(memoryFiles, [](const auto& file) { return file.expired(); });
            for (const auto& weakFile : memoryFiles) {
                if (const auto storage = weakFile.lock()) allocated += storage->pages.size();
            }
        } else {
            allocated = writableAllocation(file.persistent);
        }
        const auto limit = file.memoryFile ? std::max(maxTransientBytes, memoryLimit / 2)
                         : file.persistent ? persistentLimit(allocated) : maxTransientBytes;
        if (pages - file.storage->pages.size() + allocated > limit) return uint64_t(-28);
    }
    const uint64_t before = file.storage->pages.size();
    // A file of /data that becomes large moves to a host file of its own. If that fails it stays in memory.
    if (file.persistent && !dataFilesDirectory.empty() && size >= externalFileBytes && !file.storage->pages.backed())
        file.storage->pages.backWith(externalFile(file.inode));
    file.storage->pages.resize(pages);
    if (file.storage->placedMoves != file.storage->pages.moves) file.storage->replace();
    const auto zeroFrom = std::min(size, file.storage->size);
    // What the storage gains reads as zero already, and writing zeros there would give every page of
    // a sparse file host memory: a property area is 128 KiB of which a few pages are used.
    const auto zeroTo = size < file.storage->size ? pages : std::min<uint64_t>(size, before);
    if (zeroFrom < zeroTo) std::fill(file.storage->pages.begin() + zeroFrom, file.storage->pages.begin() + zeroTo, 0);
    file.storage->size = size;
    file.storage->pages.truncateFile(size);
    return 0;
}

void ProbeLinux::markFileModified(WritableFile& file) const {
    using namespace std::chrono;
    const auto time = realtimeAt(steady_clock::now());
    const auto seconds = floor<std::chrono::seconds>(time);
    const auto nanoseconds = (time - seconds).count();
    file.times[2] = file.times[4] = seconds.count();
    file.times[3] = file.times[5] = nanoseconds;
}

uint64_t ProbeLinux::writeFileBytes(WritableFile& file, uint64_t offset, std::span<const uint8_t> bytes) {
    if (offset + bytes.size() > file.storage->size) {
        // An input mapping may share the output storage, whose allocation can move.
        const std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        if (const auto error = resize(file, offset + bytes.size()); error) return error;
        std::copy(copy.begin(), copy.end(), file.storage->pages.begin() + offset);
    } else {
        std::memmove(file.storage->pages.data() + offset, bytes.data(), bytes.size());
    }
    markFileModified(file);
    return 0;
}

const ProbeLinux::Directory* ProbeLinux::parentDirectory(std::string_view path) const {
    const auto parent = directories.find(path.substr(0, path.rfind('/')));
    return parent != directories.end() ? &parent->second : nullptr;
}

PosixAcl ProbeLinux::inheritAcl(const Directory* parent, uint32_t& mode) const {
    if (!parent || parent->defaultAcl.empty()) {
        mode &= ~current->process->umask;
        return {};
    }
    auto acl = parent->defaultAcl;
    acl.updateMode(mode, true);
    mode = (mode & ~0777U) | acl.modeBits();
    return acl.extended() ? std::move(acl) : PosixAcl{};
}

bool ProbeLinux::inodeAllows(const auto& node, unsigned requested, bool realIds) const {
    if (sharedStorageCall) return true;
    const auto& process = *current->process;
    const auto uid = process.userIds[realIds ? 0 : 1];
    if (!uid) return true;
    // access(2) with a non-root real UID ignores the process's capabilities.
    if (!realIds) {
        constexpr uint64_t capDacOverride = 1ULL << 1, capDacReadSearch = 1ULL << 2;
        constexpr bool directory = std::is_same_v<std::remove_cvref_t<decltype(node)>, Directory>;
        if ((process.effectiveCaps & capDacOverride) &&
            (directory || !(requested & 1) || (node.mode & 0111))) return true;
        if ((process.effectiveCaps & capDacReadSearch) && !(requested & (directory ? 2U : 3U)))
            return true;
    }
    if (node.accessAcl.empty())
        return ((node.mode >> permissionShift(node.uid, node.gid, realIds)) & requested) == requested;
    const auto inGroup = [&](uint32_t group) {
        return process.groupIds[realIds ? 0 : 1] == group ||
               std::ranges::find(process.groups, group) != process.groups.end();
    };
    return node.accessAcl.allows(requested, uid == node.uid, uid, node.gid, inGroup);
}

uint64_t ProbeLinux::chmodDirectory(Directory& directory, uint32_t mode) const {
    if (userId() != 0 && userId() != directory.uid) return uint64_t(-1);
    constexpr uint64_t capFsetid = 1ULL << 4;
    const auto& process = *current->process;
    if (userId() != 0 && !(process.effectiveCaps & capFsetid) && groupId() != directory.gid &&
        std::ranges::find(process.groups, directory.gid) == process.groups.end()) mode &= ~setgidMode;
    directory.mode = mode & 03777;
    directory.accessAcl.updateMode(directory.mode, false);
    return 0;
}

bool ProbeLinux::canAccessParent(std::string_view path, bool modify, bool realIds) const {
    if (!current->process->userIds[realIds ? 0 : 1]) return true;
    if (path == sharedStorageRoot || inside(path, sharedStorageRoot)) return true;
    const MountView* view = nullptr;
    std::string_view root = "/data";
    if (!dataPath(path)) {
        view = mountFor(path);
        if (!view || (view->flags & 1)) return !modify;
        root = view->backing;
    }
    auto parent = path.substr(0, path.rfind('/'));
    bool immediate = true;
    while (parent == root || inside(parent, root)) {
        const auto directory = directories.find(parent);
        const auto requested = immediate && modify ? 3U : 1U;
        unsigned permission = 0;
        if (directory != directories.end()) {
            permission = inodeAllows(directory->second, requested, realIds) ? requested : 0;
        } else if (view && parent == root) {
            permission = view->mode >> permissionShift(0, view->gid, realIds);
        }
        if ((permission & requested) != requested) return false;
        immediate = false;
        parent = parent.substr(0, parent.rfind('/'));
    }
    return !modify || !immediate;
}

unsigned ProbeLinux::permissionShift(uint32_t uid, uint32_t gid, bool realIds) const {
    const auto& process = *current->process;
    if (process.userIds[realIds ? 0 : 1] == uid) return 6;
    return process.groupIds[realIds ? 0 : 1] == gid ||
           std::ranges::find(process.groups, gid) != process.groups.end() ? 3 : 0;
}

uint64_t ProbeLinux::recordLock(Machine& machine, const Descriptor& fd) {
    constexpr int16_t readLock = 0, writeLock = 1, unlock = 2;
    constexpr uint64_t endOfFile = uint64_t{1} << 63;
    const auto& x = machine.cpu.x;
    if (fd.pathOnly()) return uint64_t(-9);
    if (!fd.temporary) return uint64_t(-22);
    struct Flock {
        int16_t type, whence;
        int32_t padding;
        int64_t start, length;
        int32_t owner, tailPadding;
    };
    static_assert(sizeof(Flock) == 32);
    Flock request;
    if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&request), sizeof(request)}))
        return uint64_t(-14);
    // F_GETLK, F_SETLK, F_SETLKW (5, 6, 7) lock for the process; F_OFD_GETLK, F_OFD_SETLK, F_OFD_SETLKW
    // (36, 37, 38) for the open file description, which MediaProvider asks about before it opens a file.
    const bool description = x[1] >= 36;
    const bool query = x[1] == 5 || x[1] == 36, wait = x[1] == 7 || x[1] == 38;
    if (request.type < readLock || request.type > unlock || (query && request.type == unlock) ||
        request.whence < 0 || request.whence > 2 || (description && request.owner)) return invalid;
    const auto base = request.whence == 0 ? 0U :
        request.whence == 1 ? *fd.offset : fd.temporary->storage->size;
    int64_t start, otherEnd = 0;
    if (base > INT64_MAX || __builtin_add_overflow(int64_t(base), request.start, &start) ||
        (request.length && __builtin_add_overflow(start, request.length, &otherEnd))) return uint64_t(-75);
    if (start < 0 || (request.length && otherEnd < 0)) return invalid;
    const uint64_t low = request.length < 0 ? otherEnd : start;
    const uint64_t high = request.length < 0 ? uint64_t(start) : request.length > 0 ? uint64_t(otherEnd) : endOfFile;
    auto& locks = fd.temporary->locks;
    const auto owner = description ? descriptionLockOwner(fd) : processId();
    const auto overlap = [&](const RecordLock& lock) { return low < lock.end && lock.start < high; };
    if (!query && ((request.type == readLock && (fd.flags & 3) == 1) ||
                   (request.type == writeLock && !(fd.flags & 3)))) return uint64_t(-9);
    const auto conflict = std::find_if(locks.begin(), locks.end(), [&](const RecordLock& lock) {
        return lock.owner != owner && overlap(lock) && (lock.type == writeLock || request.type == writeLock);
    });
    if (query) {
        if (conflict == locks.end()) request.type = unlock;
        else request = {conflict->type, 0, 0, int64_t(conflict->start),
                         conflict->end == endOfFile ? 0 : int64_t(conflict->end - conflict->start),
                         conflict->owner & descriptionLockBit ? -1 : int32_t(conflict->owner), 0};
        return writeGuest(machine, x[2], request) ? 0 : uint64_t(-14);
    }
    if (request.type != unlock && conflict != locks.end()) {
        if (!wait) return uint64_t(-11);
        std::set<uint64_t> visited;
        std::vector<uint64_t> blockers{conflict->owner};
        while (!blockers.empty()) {
            const auto blocker = blockers.back();
            blockers.pop_back();
            if (blocker == owner && !description) return uint64_t(-35); // EDEADLK
            if (blocker & descriptionLockBit) continue;  // Linux finds no deadlocks through these
            if (!visited.insert(blocker).second) continue;
            for (const auto& [tid, thread] : threads) {
                if (thread.wait == Wait::recordLock && thread.process->id == blocker)
                    blockers.push_back(thread.recordLockBlocker);
            }
        }
        current->waitingRecordFile = fd.temporary;
        current->waitingRecordDescription = fd.offset;
        current->recordLockBlocker = conflict->owner;
        current->wait = Wait::recordLock;
        sliceEnd = 0;
        return x[0]; // The scheduler will retry fcntl with its original descriptor.
    }

    std::vector<RecordLock> updated;
    updated.reserve(locks.size() + 2);
    for (const auto& lock : locks) {
        if (lock.owner != owner || !overlap(lock)) updated.push_back(lock);
        else {
            if (lock.start < low) updated.push_back({owner, lock.start, low, lock.type});
            if (lock.end > high) updated.push_back({owner, high, lock.end, lock.type});
        }
    }
    if (request.type != unlock) updated.push_back({owner, low, high, request.type});
    std::sort(updated.begin(), updated.end(), [](const RecordLock& lhs, const RecordLock& rhs) {
        return std::tie(lhs.owner, lhs.start) < std::tie(rhs.owner, rhs.start);
    });
    size_t used = 0;
    for (const auto& lock : updated) {
        if (used && updated[used - 1].owner == lock.owner && updated[used - 1].type == lock.type &&
            updated[used - 1].end == lock.start) updated[used - 1].end = lock.end;
        else updated[used++] = lock;
    }
    updated.resize(used);
    locks = std::move(updated);
    wakeRecordLockWaiters(*fd.temporary);
    return 0;
}

void ProbeLinux::wakeRecordLockWaiters(const WritableFile& file) {
    for (auto& [tid, thread] : threads) {
        if (thread.wait != Wait::recordLock || thread.waitingRecordFile.lock().get() != &file) continue;
        thread.wait = Wait::none;
    }
}

void ProbeLinux::releaseRecordLocks(const Descriptor& fd, uint64_t owner, long descriptionUsers) {
    // A lock of the open file description goes when its last descriptor does.
    const auto description = fd.offset.use_count() <= descriptionUsers ? descriptionLockOwner(fd) : 0;
    if (fd.temporary && std::erase_if(fd.temporary->locks, [owner, description](const auto& lock) {
            return lock.owner == owner || (description && lock.owner == description);
        }))
        wakeRecordLockWaiters(*fd.temporary);
}

uint64_t ProbeLinux::renameDirectory(const std::string& source, const std::string& destination, bool noReplace) {
    if (inside(source, cgroupRoot) || inside(destination, cgroupRoot)) return uint64_t(-95);
    if (inside(destination, source)) return invalid;
    if (inside(source, destination)) return uint64_t(-39);
    const auto sameOrChild = [](std::string_view path, std::string_view root) {
        return path == root || inside(path, root);
    };
    const auto blocksMove = [&](const MountNamespace& view) {
        return std::any_of(view.mounts.begin(), view.mounts.end(), [&](const auto& mount) {
            return sameOrChild(mount.first, source) || sameOrChild(mount.first, destination) ||
                   sameOrChild(mount.second.backing, source) || sameOrChild(mount.second.backing, destination);
        });
    };
    std::set<const Process*> seen;
    std::vector<std::pair<std::string*, std::string>> descriptorMoves;
    for (const auto& [tid, thread] : threads) {
        if (blocksMove(*thread.mountNamespace)) return uint64_t(-16);
        if (!seen.insert(thread.process.get()).second) continue;
        auto& cwd = thread.process->workingDirectory;
        if (cwd == destination) return uint64_t(-16);
        if (sameOrChild(cwd, source)) descriptorMoves.emplace_back(&cwd, destination + cwd.substr(source.size()));
        for (auto& [number, fd] : thread.process->descriptors) {
            if (fd.mountNamespace && blocksMove(*fd.mountNamespace)) return uint64_t(-16);
            if (fd.node == Node::directory && fd.path == destination) return uint64_t(-16);
            for (auto* path : {&fd.path, &fd.visiblePath}) {
                if (sameOrChild(*path, source))
                    descriptorMoves.emplace_back(path, destination + path->substr(source.size()));
            }
        }
    }
    const auto containsChild = [&](const auto& nodes) {
        const auto child = nodes.lower_bound(destination + "/");
        return child != nodes.end() && inside(child->first, destination);
    };
    if (directories.contains(destination)) {
        if (noReplace) return uint64_t(-17);
        if (containsChild(directories) || containsChild(writableFiles) ||
            containsChild(links) || containsChild(socketPaths) || containsChild(fifoPaths)) return uint64_t(-39);
    } else if (find(destination, 0) || links.contains(destination)) {
        return noReplace ? uint64_t(-17) : uint64_t(-20);
    }
    const auto collect = [&](auto& nodes) {
        using Iterator = decltype(nodes.begin());
        std::vector<std::pair<Iterator, std::string>> moves;
        if (auto root = nodes.find(source); root != nodes.end()) moves.emplace_back(root, destination);
        for (auto it = nodes.lower_bound(source + "/");
             it != nodes.end() && inside(it->first, source); ++it)
            moves.emplace_back(it, destination + it->first.substr(source.size()));
        return moves;
    };
    auto movedDirectories = collect(directories);
    auto movedFiles = collect(writableFiles);
    auto movedLinks = collect(links);
    auto movedSockets = collect(socketPaths);
    auto movedFifos = collect(fifoPaths);
    const auto apply = [](auto& nodes, auto& moves) {
        for (auto& [position, target] : moves) {
            auto node = nodes.extract(position);
            node.key().swap(target);
            nodes.insert(std::move(node));
        }
    };
    directories.erase(destination);
    apply(directories, movedDirectories);
    apply(writableFiles, movedFiles);
    apply(links, movedLinks);
    apply(socketPaths, movedSockets);
    apply(fifoPaths, movedFifos);
    for (auto& [path, target] : descriptorMoves) path->swap(target);
    return 0;
}

uint64_t ProbeLinux::renameFile(Machine& machine) {
    auto& x = machine.cpu.x;
    const auto flags = x[8] == 276 ? x[4] : 0;
    if (flags & ~uint64_t{1}) return invalid;
    const auto getPath = [&](uint64_t fd, uint64_t pointer, std::string& path) -> uint64_t {
        const auto input = readPath(machine, pointer);
        if (!input) return uint64_t(-14);
        if (input->empty()) return uint64_t(-2);
        if (const auto error = resolveAtPath(uint32_t(fd), *input, path)) return error;
        uint64_t lookupError = uint64_t(-40);
        auto visible = followLinks(std::move(path), false, &lookupError);
        if (!visible) return lookupError;
        path = std::move(*visible);
        return 0;
    };
    std::string source, destination;
    if (const auto error = getPath(x[0], x[1], source); error) return error;
    if (const auto error = getPath(x[2], x[3], destination); error) return error;
    const auto* sourceMount = mountFor(source);
    if (!sourceMount || sourceMount != mountFor(destination) ||
        (!sourceMount->hostData && !sourceMount->device.empty() && sourceMount->device != "bpf"))
        return uint64_t(-18);
    if (source == "/data" || destination == "/data" || current->mountNamespace->mounts.contains(source) ||
        current->mountNamespace->mounts.contains(destination)) return uint64_t(-16);
    if (!writablePath(source) || !writablePath(destination)) return uint64_t(-30);
    source = backingPath(source);
    destination = backingPath(destination);
    auto file = writableFiles.find(source);
    auto link = links.find(source);
    const bool directory = directories.contains(source);
    if (!directory && file == writableFiles.end() && link == links.end()) return uint64_t(-2);
    if (source == destination) return flags ? uint64_t(-17) : 0;
    if (!directoryExists(destination.substr(0, destination.rfind('/')))) return uint64_t(-2);
    if (!canAccessParent(source, true) || !canAccessParent(destination, true)) return uint64_t(-13);
    if (directory) {
        const auto result = renameDirectory(source, destination, flags != 0);
        if (!result) {
            fileMoved(source, destination, true);
            sharedMoved(source, destination);
        }
        return result;
    }
    if (directories.contains(destination)) return uint64_t(-21);
    if (const auto existing = find(destination, 0); existing && !existing->temporary) return uint64_t(-30);
    if (flags && (writableFiles.contains(destination) || links.contains(destination))) return uint64_t(-17);
    sharedFileRemoved(destination);
    writableFiles.erase(destination);
    links.erase(destination);
    const auto move = [&](auto& items, auto item) {
        auto entry = items.extract(item);
        entry.key() = destination;
        items.insert(std::move(entry));
    };
    if (file != writableFiles.end()) move(writableFiles, file);
    else move(links, link);
    fileMoved(source, destination, false);
    sharedMoved(source, destination);
    return 0;
}

uint64_t ProbeLinux::open(const Machine& machine, const std::string& path, uint32_t flags, uint32_t mode) {
    constexpr uint32_t create = 0x40, exclusive = 0x80, truncate = 0x200;
    // The arm64 UAPI uses architecture-specific fcntl flag values.
    constexpr uint32_t directory = 0x4000, noFollow = 0x8000, direct = 0x10000, pathOnly = 0x200000;
    constexpr uint32_t supported = 0x80000 | noFollow | direct | pathOnly | 0x20000 | 0x800 | 0x400 | 0x100 |
                                   directory | truncate | create | exclusive | 3;
    if ((flags & ~supported) || (flags & 3) == 3) return invalid;
    const auto logical = followLinks(path, !(flags & noFollow));
    if (!logical || ((flags & noFollow) && resolve(*logical))) return uint64_t(-40);
    const auto pathname = backingPath(*logical);
    if (!canAccessParent(pathname, false)) return uint64_t(-13);
    auto entry = find(pathname, flags, &machine);
    if ((flags & (create | exclusive)) == (create | exclusive) && entry) return uint64_t(-17);
    if (!entry && !(flags & create)) return uint64_t(-2);
    // Immutable image data is already memory backed, with no guest page cache to bypass.
    if ((flags & direct) && (!entry || entry->node != Node::regular)) return invalid;
    if ((flags & directory) && (!entry || entry->node != Node::directory)) return uint64_t(-20);
    if (entry && (entry->node == Node::loopControl || entry->node == Node::loopDevice) && userId() != 0)
        return uint64_t(-13);
    if (entry && entry->node == Node::socket && !(flags & pathOnly)) return uint64_t(-6);
    if (entry && entry->temporary && entry->temporary->pinnedBpfObject() && !(flags & pathOnly))
        return uint64_t(-5); // Ordinary open does not create a BPF object descriptor.
    if (entry && entry->node == Node::directory && (flags & 3)) return uint64_t(-21);
    if (entry && entry->node == Node::regular && ((flags & 3) || (flags & truncate))) return uint64_t(-30);
    if (entry && entry->cgroupFile && (flags & 3) && !canWriteCgroup(*entry->cgroupFile)) return uint64_t(-13);
    if (entry && entry->node == Node::oomScoreAdjustment && !(flags & pathOnly) && (flags & (3 | truncate))) {
        constexpr uint64_t capDacOverride = 1ULL << 1;
        const auto* subject = liveProcess(entry->targetProcess);
        if (!subject) return uint64_t(-3);
        if (userId() != subject->userIds[1] && !(current->process->effectiveCaps & capDacOverride))
            return uint64_t(-13);
    }
    if (entry && entry->temporary && userId() != 0 && !(flags & pathOnly)) {
        const auto& file = *entry->temporary;
        const unsigned requested = (flags & 3) == 2 ? 6 : ((flags & 3) || (flags & truncate)) ? 2 : 4;
        if (!inodeAllows(file, requested)) return uint64_t(-13);
    }
    if (entry && entry->node == Node::directory && !(flags & pathOnly)) {
        const auto directory = directories.find(pathname);
        if (directory != directories.end() && !directory->second.accessAcl.empty() &&
            !inodeAllows(directory->second, 4)) return uint64_t(-13);
    }
    const auto fd = freeDescriptor();
    if (fd == descriptorCapacity) return uint64_t(-24);
    if (!entry) {
        if (const auto* view = mountFor(path); view && view->device == "cgroup2") return uint64_t(-13);
        const auto parent = pathname.substr(0, pathname.rfind('/'));
        if (!directoryExists(parent)) return uint64_t(-2);
        if (!writablePath(path)) return uint64_t(-30);
        if (const auto* mount = mountFor(path); mount && mount->device == "bpf") return uint64_t(-13);
        if (!canAccessParent(pathname, true)) return uint64_t(-13);
        const bool persistent = dataPath(pathname);
        const auto fileLimit = persistent ? maxPersistentFiles : maxTransientFiles;
        if (writableFiles.size() >= fileLimit &&
            size_t(std::count_if(writableFiles.begin(), writableFiles.end(), [persistent](const auto& entry) {
                return !entry.second->pinnedBpfObject() && entry.second->persistent == persistent;
            })) >= fileLimit) return uint64_t(-28);
        auto file = std::make_shared<WritableFile>();
        file->inode = nextInode++;
        file->mode = mode & 0777;
        file->uid = userId();
        const auto* parentMetadata = parentDirectory(pathname);
        file->gid = parentMetadata && (parentMetadata->mode & setgidMode) ? parentMetadata->gid : groupId();
        file->accessAcl = inheritAcl(parentMetadata, file->mode);
        file->persistent = persistent;
        markFileModified(*file);
        file->times[0] = file->times[2];
        file->times[1] = file->times[3];
        if (const auto context = newFileContext(pathname); !context.empty())
            file->attributes["security.selinux"].assign(context.c_str(), context.c_str() + context.size() + 1);
        if (persistent) sharedFileCreated(pathname, *file);
        writableFiles.emplace(pathname, std::move(file));
        fileEvent(pathname, fileCreated);
        entry = find(pathname, flags);
    }
    if (!entry->pathOnly() && entry->node == Node::touchDevice) {
        if (userId() && permissionShift(0, 1004) == 0) return uint64_t(-13);
        if (!guestTouchReader.expired()) return uint64_t(-16);
        entry->touchReader = std::make_shared<GuestTouch::Reader>(guestTouch);
        guestTouchReader = entry->touchReader;
        entry->touch = guestTouch;
    }
    if (!entry->pathOnly() && entry->node == Node::graphicsAddressSpace)
        entry->graphicsAddress = std::make_shared<GraphicsAddressSpace>();
    if (!entry->pathOnly() && entry->node == Node::graphicsPipe) entry->graphics = std::make_shared<GraphicsPipeState>();
    if (!entry->pathOnly() && entry->node == Node::binder) {
        const auto& node = binderDevices.at(entry->path);
        if (userId() != 0) {
            const unsigned shift = userId() == node.uid ? 6 : groupId() == node.gid ? 3 : 0;
            const uint32_t requested = (flags & 3) == 2 ? 6 : (flags & 3) == 1 ? 2 : 4;
            if (((node.mode >> shift) & requested) != requested) return uint64_t(-13);
        }
        entry->binder = std::make_shared<BinderConnection>(processId(), binderDeathsDirty, binderReferenceChanges);
    }
    if (!entry->pathOnly() && (entry->node == Node::ptyMaster || entry->node == Node::ptySlave)) {
        if (const auto error = openPty(*entry); error) return error;
    }
    if (!entry->pathOnly() && entry->node == Node::ashmem) {
        entry->ashmem = std::make_shared<Ashmem>();
        entry->ashmem->file.memoryFile = true;
    }
    if ((flags & truncate) && entry->temporary) {
        if (const auto error = resize(*entry->temporary, 0)) return error;
        markFileModified(*entry->temporary);
        fileEvent(entry->path, fileModified);
    }
    if (entry->node == Node::directory && pathname.find('/', 2) == std::string::npos) {
        // Each BPF mount owns a synthetic backing root. A bind mount can expose a subdirectory.
        const auto* mount = mountFor(*logical);
        entry->bpfFilesystemRoot = mount && mount->device == "bpf" && mount->backing == pathname;
    }
    if (entry->path != *logical) entry->visiblePath = *logical;
    if (!entry->pathOnly() && (entry->node == Node::regular || entry->temporary || entry->node == Node::directory))
        fileEvent(entry->path, fileOpened, entry->node == Node::directory);
    if (entry->pipe && !entry->pathOnly()) {
        // A named pipe: every reader shares one end and every writer the other, so that the pipe sees
        // its last reader or writer go. Opening does not wait for the other side as Linux would,
        // except that a writer who will not wait is told there is no reader.
        const auto end = [](std::weak_ptr<int>& slot) {
            auto held = slot.lock();
            if (!held) slot = held = std::make_shared<int>(0);
            return held;
        };
        const unsigned access = flags & 3;
        if (access == 1 && (flags & 0x800) && entry->pipe->reader.expired()) return uint64_t(-6);
        if (access != 1) entry->pipeEnd = end(entry->pipe->reader);
        if (access != 0) (access == 1 ? entry->pipeEnd : entry->pipeOtherEnd) = end(entry->pipe->writer);
    }
    descriptors().emplace(fd, std::move(*entry));
    return fd;
}

bool ProbeLinux::lockWholeFile(Machine& machine, const Descriptor& file) {
    auto& x = machine.cpu.x;
    constexpr uint32_t shared = 1, exclusive = 2, nonblocking = 4, unlock = 8;
    const auto operation = uint32_t(x[1]);
    if (file.pathOnly()) { x[0] = uint64_t(-9); return true; }
    const auto action = operation & ~nonblocking;
    if (action != shared && action != exclusive && action != unlock) { x[0] = invalid; return true; }
    if (file.node != Node::regular && file.node != Node::temporary && file.node != Node::directory) {
        x[0] = uint64_t(-95);
        return true;
    }

    for (auto it = wholeFileLocks.begin(); it != wholeFileLocks.end();) {
        auto& holders = it->second;
        std::erase_if(holders, [](const auto& lock) { return lock.owner.expired(); });
        if (holders.empty()) it = wholeFileLocks.erase(it);
        else ++it;
    }
    const auto key = file.temporary ? std::pair{file.temporary->inode, std::string{}} :
        std::pair{uint64_t{0}, file.path};
    auto& holders = wholeFileLocks[key];
    const auto sameOwner = [&](const auto& lock) { return lock.owner.lock() == file.offset; };
    const auto previous = std::ranges::find_if(holders, sameOwner);
    if (previous != holders.end()) {
        if (action != unlock && previous->exclusive == (action == exclusive)) { x[0] = 0; return true; }
        holders.erase(previous);
        wakePollers();
    }
    if (action == unlock) {
        if (holders.empty()) wholeFileLocks.erase(key);
        x[0] = 0;
        return true;
    }
    if (std::ranges::any_of(holders, [action](const auto& lock) { return action == exclusive || lock.exclusive; })) {
        if (operation & nonblocking) { x[0] = uint64_t(-11); return true; }
        return waitForEvents();
    }
    holders.push_back({file.offset, action == exclusive});
    x[0] = 0;
    return true;
}

uint32_t ProbeLinux::descriptorLimit() const {
    const auto resource = current->process->limits.find(7); // RLIMIT_NOFILE
    return resource == current->process->limits.end()
        ? defaultDescriptorLimit : std::min<uint64_t>(descriptorCapacity, resource->second[0]);
}

uint32_t ProbeLinux::freeDescriptor(uint32_t minimum) const {
    auto fd = minimum;
    const auto limit = descriptorLimit();
    while (fd < limit && descriptors().contains(fd)) ++fd;
    return fd < limit ? fd : descriptorCapacity;
}

bool ProbeLinux::eventCall(Machine& machine) {
    constexpr uint64_t capWakeAlarm = 1ULL << 35;
    constexpr uint32_t epollIn = 1, epollOut = 4, epollError = 8, epollHangup = 16, epollNormalRead = 0x40;
    constexpr uint32_t epollReadHangup = 0x2000, epollWakeup = 0x20000000, epollEdge = 0x80000000;
    constexpr auto unixReadEdge = epollIn | epollNormalRead | epollReadHangup | epollError | epollHangup |
        epollWakeup | epollEdge;
    auto& x = machine.cpu.x;
    constexpr uint32_t cloexec = 0x80000, nonblock = 0x800;
    const auto descriptor = descriptors().find(uint32_t(x[0]));
    switch (x[8]) {
        case 19: case 20: case 74: { // eventfd2, epoll_create1, signalfd4
            const bool signal = x[8] == 74, poller = x[8] == 20;
            const auto flags = signal ? x[3] : poller ? x[0] : x[1];
            const auto supported = poller ? cloexec : cloexec | nonblock;
            if (flags & ~uint64_t(supported)) { x[0] = invalid; break; }
            uint64_t mask = 0;
            if (signal) {
                if (x[2] != sizeof(mask)) { x[0] = invalid; break; }
                if (!machine.load64(x[1], mask)) { x[0] = uint64_t(-14); break; }
                mask &= ~((1ULL << 8) | (1ULL << 18)); // SIGKILL/SIGSTOP cannot be blocked.
                if (uint32_t(x[0]) != UINT32_MAX) {
                    if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
                    if (descriptor->second.node != Node::signal) { x[0] = invalid; break; }
                    descriptor->second.event->signalMask = mask;
                    wakePollers();
                    break;
                }
            }
            const auto fd = freeDescriptor();
            if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            const auto node = signal ? Node::signal : poller ? Node::poller : Node::event;
            const auto path = signal ? "anon_inode:[signalfd]" : poller ? "anon_inode:[eventpoll]" : "anon_inode:[eventfd]";
            Descriptor entry{path, {}, uint32_t(flags) | 2, node};
            entry.event = std::make_shared<EventState>();
            entry.event->signalMask = mask;
            if (node == Node::event) entry.event->counter = uint32_t(x[0]);
            descriptors().emplace(fd, std::move(entry));
            x[0] = fd;
            break;
        }
        case 85: { // timerfd_create
            const bool wakeAlarm = x[0] == 8 || x[0] == 9; // CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM
            const bool realtime = x[0] == 0 || x[0] == 8;
            if ((x[0] != 1 && x[0] != 7 && !wakeAlarm && !realtime) || (x[1] & ~(cloexec | nonblock))) {
                x[0] = invalid;
                break;
            }
            if (wakeAlarm && !(current->process->effectiveCaps & capWakeAlarm)) { x[0] = uint64_t(-1); break; }
            const auto fd = freeDescriptor();
            if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            std::erase_if(timers, [](const auto& timer) { return timer.expired(); });
            Descriptor entry{"anon_inode:[timerfd]", {}, uint32_t(x[1]), Node::timer};
            entry.event = std::make_shared<EventState>();
            // Wake alarm timers use the guest scheduler while the iOS app is running.
            auto& timer = entry.event->timer.emplace();
            timer.wakeAlarm = wakeAlarm;
            timer.realtime = realtime;
            timers.emplace_back(entry.event);
            descriptors().emplace(fd, std::move(entry));
            x[0] = fd;
            break;
        }
        case 86: case 87: { // timerfd_settime / timerfd_gettime
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!descriptor->second.event || !descriptor->second.event->timer) { x[0] = invalid; break; }
            using namespace std::chrono;
            const bool setting = x[8] == 86;
            const auto now = steady_clock::now();
            refreshTimers(now);
            auto& event = *descriptor->second.event;
            auto& timer = *event.timer;
            std::array<int64_t, 4> previous{};
            const auto duration = [](int64_t sec, int64_t ns) -> std::optional<nanoseconds> {
                if (sec < 0 || ns < 0 || ns >= 1'000'000'000 || sec > (INT64_MAX - ns) / 1'000'000'000)
                    return {};
                return seconds(sec) + nanoseconds(ns);
            };
            EventState::Timer replacement;
            if (setting) {
                constexpr uint64_t absolute = 1, cancelOnSet = 2;
                if (x[1] & ~(absolute | cancelOnSet)) { x[0] = invalid; break; }
                std::vector<uint8_t> inputScratch;
                const auto* input = machine.read(x[2], sizeof previous, 4, inputScratch);
                if (!input) { x[0] = uint64_t(-14); break; }
                std::array<int64_t, 4> values{};
                std::memcpy(values.data(), input, sizeof values);
                const auto interval = duration(values[0], values[1]), value = duration(values[2], values[3]);
                if (!interval || !value) { x[0] = invalid; break; }
                if (timer.wakeAlarm && !(current->process->effectiveCaps & capWakeAlarm)) {
                    x[0] = uint64_t(-1);
                    break;
                }
                replacement.wakeAlarm = timer.wakeAlarm;
                replacement.realtime = timer.realtime;
                replacement.absoluteRealtime = timer.realtime && (x[1] & absolute);
                replacement.cancelOnSet = replacement.absoluteRealtime && (x[1] & cancelOnSet);
                replacement.interval = *interval;
                if (value->count()) {
                    const auto epoch = x[1] & absolute ? nanoseconds{} : duration_cast<nanoseconds>(now.time_since_epoch());
                    replacement.next = epoch + std::min(*value, nanoseconds::max() - epoch);
                }
            }
            const auto timerNow = timer.absoluteRealtime ? realtimeAt(now) : duration_cast<nanoseconds>(now.time_since_epoch());
            const auto remaining = timer.next ? *timer.next - timerNow : nanoseconds{};
            previous = {duration_cast<seconds>(timer.interval).count(), timer.interval.count() % 1'000'000'000,
                        duration_cast<seconds>(remaining).count(), remaining.count() % 1'000'000'000};
            if ((!setting || x[3]) && !writeGuest(machine, setting ? x[3] : x[1], previous)) {
                x[0] = uint64_t(-14);
                break;
            }
            if (setting) {
                timer = replacement;
                event.counter = 0;
                refreshTimers(now);
            }
            x[0] = 0;
            break;
        }
        case 21: { // epoll_ctl, AArch64 epoll_event with 64-bit data at offset 8.
            const auto target = descriptors().find(uint32_t(x[2]));
            if (descriptor == descriptors().end() || target == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (descriptor->second.node != Node::poller) { x[0] = invalid; break; }
            const auto& targetMap = target->second.bpfMap;
            const auto ring = targetMap && targetMap->type == BpfMap::ringBuffer ? targetMap : nullptr;
            if (!ring && !target->second.socket && !target->second.binder && !target->second.graphics &&
                !target->second.pipe && !target->second.touch && target->second.node != Node::pressure &&
                (!target->second.event || target->second.node == Node::poller)) {
                x[0] = uint64_t(-1);
                break;
            }
            auto& watches = descriptor->second.event->watches;
            const auto fd = uint32_t(x[2]);
            if (const auto stale = watches.find(fd); stale != watches.end() && stale->second.source.expired() &&
                stale->second.socket.expired() && stale->second.binder.expired() && stale->second.graphics.expired() &&
                stale->second.pipeEnd.expired() && stale->second.touchReader.expired() && stale->second.ring.expired()) {
                watches.erase(stale);
            }
            const bool registered = watches.contains(fd);
            if (x[1] == 2 && registered) { watches.erase(fd); x[0] = 0; break; }
            if (x[1] < 1 || x[1] > 3) { x[0] = invalid; break; }
            if (x[1] == 1 && registered) { x[0] = uint64_t(-17); break; }
            if (x[1] != 1 && !registered) { x[0] = uint64_t(-2); break; }
            std::array<uint64_t, 2> event{};
            std::vector<uint8_t> dataScratch;
            const auto* data = machine.read(x[3], sizeof(event), 4, dataScratch);
            if (!data) { x[0] = uint64_t(-14); break; }
            std::memcpy(event.data(), data, sizeof(event));
            const auto mask = uint32_t(event[0]);
            // EPOLLWAKEUP requires no power-management action while the guest is scheduled.
            const auto& socket = target->second.socket;
            // A pipe may be watched for changes only: it is reported once for each time something was
            // written to it. Realm watches its named pipes this way and never reads them, so reporting
            // them for as long as they can be read keeps its thread running without end.
            const bool edgeSupported = target->second.node == Node::event || target->second.pipe ||
                (socket && socket->family == 1 && !(mask & ~unixReadEdge));
            const auto supported = 0x2000205fU | (edgeSupported ? epollEdge : 0);
            if (mask & ~supported) return false;
            watches.insert_or_assign(fd, Watch{mask, event[1], target->second.event, target->second.socket,
                                              target->second.node == Node::event, target->second.binder, target->second.graphics,
                                              target->second.pipe, target->second.pipeEnd, target->second.node == Node::pipeRead,
                                              target->second.touch, target->second.touchReader, ring});
            wakePollers(descriptor->second.event.get());
            x[0] = 0;
            break;
        }
        case 22: { // epoll_pwait
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (descriptor->second.node != Node::poller || !x[2] || x[2] > INT32_MAX) {
                x[0] = invalid;
                break;
            }
            if (x[4]) return false; // Temporary signal-mask swaps need a scheduler.
            if (!timers.empty()) refreshTimers(std::chrono::steady_clock::now());
            std::vector<std::array<uint64_t, 2>> ready;
            std::vector<std::shared_ptr<EventState>> readNotifications;
            std::vector<std::pair<Watch*, PollNotifications>> readyEdges;
            bool watchesHostEvents = false, watchesListener = false;
            for (auto& [fd, watch] : descriptor->second.event->watches) {
                const auto state = watch.source.lock();
                const auto endpoint = watch.socket.lock();
                watchesListener |= endpoint && endpoint->listening && (watch.mask & epollIn);
                const bool pipeEdge = (watch.mask & epollEdge) && watch.pipe.lock();
                const bool edge = (watch.mask & epollEdge) && !pipeEdge;
                const PollNotifications* notifications = nullptr;
                if (edge) {
                    if (!state && !endpoint) continue;
                    notifications = state ? &state->pollNotifications : &endpoint->pollNotifications;
                    const auto& delivered = watch.deliveredNotifications;
                    const auto reads = endpoint ? epollIn | epollNormalRead : epollIn;
                    if (delivered &&
                        (!(watch.mask & reads) || delivered->readable == notifications->readable) &&
                        (!(watch.mask & epollOut) || delivered->writable == notifications->writable) &&
                        delivered->stateChange == notifications->stateChange) continue;
                }
                const auto peer = endpoint ? endpoint->peer.lock() : nullptr;
                const bool childReady = state && (state->signalMask & sigChildMask) && current->process->childSignal;
                const auto binder = watch.binder.lock();
                const auto graphics = watch.graphics.lock();
                const auto touch = watch.touchReader.expired() ? nullptr : watch.touch.lock();
                watchesHostEvents |= bool(graphics) || bool(touch);
                const auto graphicsEvents = graphics ? graphics->pollEvents() : 0;
                const auto pipe = watch.pipeEnd.expired() ? nullptr : watch.pipe.lock();
                const auto pipeEvents = !pipe || (pipeEdge && watch.pipeWritesSeen == pipe->writes) ? 0
                    : pipe->pollEvents(watch.pipeReader);
                const auto ring = watch.ring.lock();
                const auto ringEvents = ring && ring->ringReadable() ? 0x41 : 0;
                const bool guestReceiveEnded = endpoint && (endpoint->receiveShutdown ||
                    (!endpoint->netlink && !endpoint->inetDatagram() && endpoint->connected && (!peer || peer->sendShutdown)));
                const bool guestReadable = (state && state->counter) || childReady || (binder && binderReadable(*binder, processId(), currentTid)) ||
                    (touch && touch->ready()) ||
                    (endpoint && (endpoint->hasInput() || !endpoint->pending.empty() ||
                                  ((peer || endpoint->family == 1) && guestReceiveEnded)));
                const bool guestWritable = (state && watch.eventCounter && state->counter < UINT64_MAX - 1) ||
                    (endpoint && (endpoint->tcpConnectFailed ||
                        (endpoint->netlink && endpoint->protocol != 15) ||
                        (endpoint->inetDatagram() && !endpoint->sendShutdown))) ||
                    (peer && !endpoint->sendShutdown && !peer->receiveShutdown &&
                     peer->queuedBytes < endpoint->sendBuffer && peer->inbox.size() < 256);
                const bool guestHungUp = endpoint && (endpoint->tcpConnectFailed ||
                    (!endpoint->netlink && !endpoint->inetDatagram() && endpoint->connected &&
                     (!peer || (guestReceiveEnded && endpoint->sendShutdown))));
                const bool guestSocketError = endpoint && endpoint->inetError;
                // A socket that the host carries is ready when the host's is.
                const int host = endpoint && endpoint->hostFd >= 0 ? hostSocketEvents(*endpoint) : -1;
                watchesHostEvents |= host >= 0;
                const bool receiveEnded = host < 0 && guestReceiveEnded;
                const bool readable = host >= 0 ? bool(host & 1) : guestReadable;
                const bool writable = host >= 0 ? bool(host & 4) : guestWritable;
                const bool hungUp = host >= 0 ? bool(host & 16) : guestHungUp;
                const bool socketError = host >= 0 ? bool(host & 8) : guestSocketError;
                const bool mountsChanged = state && state->mountNamespace &&
                    state->observedMountGeneration != state->mountNamespace->generation;
                const auto readEvents = readable ? epollIn | (endpoint && endpoint->family == 1 ? epollNormalRead : 0) : 0;
                const auto taskEvents = readEvents | (mountsChanged ? 2U : 0) | (writable ? epollOut : 0) |
                    (receiveEnded ? epollReadHangup : 0);
                const auto errors = (hungUp ? epollHangup : 0) | (socketError ? epollError : 0);
                const uint64_t mask = (taskEvents & watch.mask) | errors |
                    ((graphicsEvents | pipeEvents | ringEvents) & (watch.mask | epollError | epollHangup));
                if (mask) {
                    if (pipeEdge && pipe) watch.pipeWritesSeen = pipe->writes;
                    ready.push_back({mask, watch.data});
                    if (notifications) readyEdges.emplace_back(&watch, *notifications);
                    if (mask & 2) readNotifications.push_back(state);
                    if (ready.size() == x[2]) break;
                } else if (notifications) {
                    watch.deliveredNotifications = *notifications;
                }
            }
            if (ready.empty()) {
                const auto wait = [&] {
                    const auto waiting = watchesHostEvents ? waitForHostEvents() : waitForEvents();
                    current->waitingEpoll = descriptor->second.event;
                    return waiting;
                };
                if (uint32_t(x[3]) == UINT32_MAX) {
                    // Only listener waits can contribute a socket path to the worker report.
                    if (watchesListener && currentTid != pid) {
                        for (const auto& [path, bound] : socketPaths) {
                            const auto endpoint = bound.endpoint.lock();
                            if (!endpoint || !endpoint->listening) continue;
                            for (const auto& [fd, watch] : descriptor->second.event->watches) {
                                if ((watch.mask & 1) && watch.socket.lock() == endpoint) {
                                    propertyPollers.insert_or_assign(path, currentTid);
                                }
                            }
                        }
                    }
                    return wait();
                }
                const auto timeout = int32_t(x[3]);
                if (timeout < 0) return wait();
                if (timeout > 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!current->deadline) current->deadline = now + std::chrono::milliseconds(timeout);
                    if (*current->deadline > now) return wait();
                }
                current->deadline.reset();
                x[0] = 0;
                break;
            }
            current->deadline.reset();
            const auto bytes = std::span(reinterpret_cast<const uint8_t*>(ready.data()), ready.size() * sizeof(ready[0]));
            if (!machine.write(x[1], bytes)) { x[0] = uint64_t(-14); break; }
            for (const auto& [watch, notifications] : readyEdges) watch->deliveredNotifications = notifications;
            for (const auto& state : readNotifications) state->observedMountGeneration = state->mountNamespace->generation;
            x[0] = ready.size();
            break;
        }
        default: return false;
    }
    return true;
}

uint64_t ProbeLinux::socketRead(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size,
                                bool reportPacketSize, bool peek) {
    auto& endpoint = *fd.socket;
    if (!size && (!reportPacketSize || endpoint.type == 1)) return 0;
    if (endpoint.inetDatagram()) return receiveInetDatagram(machine, endpoint, address, size);
    if (endpoint.hostFd >= 0) {
        if (endpoint.hostConnecting) hostSocketEvents(endpoint);
        if (endpoint.inetError) return uint64_t(-std::exchange(endpoint.inetError, 0));
        if (!endpoint.connected) return uint64_t(-107);
        if (endpoint.receiveShutdown) return 0;
        std::vector<uint8_t> bytes(std::min<uint64_t>(size, 256 * 1024));
        const auto received = hostnet::receive(endpoint.hostFd, bytes, peek);
        if (received == -11) hostSocketWait = true;
        if (received <= 0) return uint64_t(received);
        return machine.write(address, std::span(bytes).first(size_t(received))) ? uint64_t(received) : uint64_t(-14);
    }
    if (endpoint.inetError && endpoint.inbox.empty()) return uint64_t(-std::exchange(endpoint.inetError, 0));
    if (endpoint.inbox.empty()) {
        if (endpoint.netlink) return uint64_t(-11);
        const auto peer = endpoint.peer.lock();
        if (endpoint.receiveShutdown || (endpoint.connected && (!peer || peer->sendShutdown))) return 0;
        return uint64_t(-11);
    }
    if (endpoint.type == 1) {
        const auto count = std::min<uint64_t>(size, endpoint.queuedBytes);
        std::vector<uint8_t> bytes;
        bytes.reserve(count);
        for (const auto& queued : endpoint.inbox) {
            const auto& packet = queued.payload;
            const auto chunk = std::min<size_t>(packet.size(), count - bytes.size());
            bytes.insert(bytes.end(), packet.begin(), packet.begin() + chunk);
            if (bytes.size() == count) break;
        }
        if (count && !machine.write(address, bytes)) return uint64_t(-14);
        consumeSocketStream(endpoint, count);
        return count;
    }
    const auto& packet = endpoint.inbox.front().payload;
    const size_t packetSize = packet.size();
    const auto count = std::min<uint64_t>(size, packetSize);
    if (count && !machine.write(address, std::span(packet).first(count))) return uint64_t(-14);
    const auto queuedBefore = endpoint.queuedBytes;
    const auto packetsBefore = endpoint.inbox.size();
    endpoint.queuedBytes -= packetSize;
    endpoint.inbox.pop_front();
    socketDrained(endpoint, queuedBefore, packetsBefore);
    return reportPacketSize ? packetSize : count;
}

void ProbeLinux::socketDrained(SocketState& endpoint, uint64_t queuedBefore, size_t packetsBefore) {
    // A writer that was refused, or a connected writer that may be polling for room it did not have.
    const auto writer = endpoint.peer.lock();
    if (std::exchange(endpoint.writerWaiting, false) || packetsBefore >= 256 ||
        (writer && queuedBefore >= writer->sendBuffer)) wakePollers();
}

void ProbeLinux::consumeSocketStream(SocketState& endpoint, size_t count) {
    if (!count) return;
    const auto queuedBefore = endpoint.queuedBytes;
    const auto packetsBefore = endpoint.inbox.size();
    endpoint.queuedBytes -= count;
    endpoint.streamOffset += count;
    while (!endpoint.rights.empty() && endpoint.rights.front().firstByte < endpoint.streamOffset)
        endpoint.rights.pop_front();
    while (count) {
        auto& packet = endpoint.inbox.front().payload;
        if (packet.size() > count) {
            packet.erase(packet.begin(), packet.begin() + count);
            break;
        }
        count -= packet.size();
        endpoint.inbox.pop_front();
    }
    socketDrained(endpoint, queuedBefore, packetsBefore);
}

std::shared_ptr<ProbeLinux::GraphicsRegion> ProbeLinux::GraphicsAddressSpace::mapping(uint64_t offset, uint64_t size) const {
    const auto includes = [=](uint64_t start, uint64_t length) {
        return offset >= start && offset - start <= length && size <= length - (offset - start);
    };
    if (type != goldfishSharedSlots) {
        const auto found = owned.upper_bound(offset);
        if (found != owned.begin()) {
            const auto& [base, region] = *std::prev(found);
            if (includes(base, region->storage.size)) return region;
        }
    }
    for (auto found = claims.upper_bound(offset); found != claims.begin();) {
        const auto& [base, claim] = *--found;
        if (includes(base, claim.size)) return claim.region;
        if (offset - claim.region->offset >= claim.region->storage.size) break;
    }
    return {};
}

void ProbeLinux::serviceGraphicsDma() {
    if (graphicsDma) graphicsDma->service([this](auto& transfer) { return transferGraphicsDma(transfer); });
}

bool ProbeLinux::transferGraphicsDma(GuestColorDma::Transfer& transfer) {
    if (transfer.address < graphicsAddressBase) return false;
    const auto offset = transfer.address - graphicsAddressBase;
    const auto end = graphicsRegions.upper_bound(offset);
    if (end == graphicsRegions.begin()) return false;
    const auto region = std::prev(end)->second.lock();
    if (!region || region->released) return false;
    const auto within = offset - region->offset;
    const auto size = region->storage.size;
    if (within >= size || transfer.pixels.size() > size - within) return false;

    auto* pixels = region->storage.pages.data() + within;
    switch (transfer.phase) {
        case GuestColorDma::Phase::upload:
            std::memcpy(transfer.pixels.data(), pixels, transfer.pixels.size());
            return true;
        case GuestColorDma::Phase::readback:
            transfer.backing = std::shared_ptr<SharedFileBytes>(region, &region->storage);
            return true;
        case GuestColorDma::Phase::publish:
            if (transfer.backing.get() != &region->storage) return false;
            std::memcpy(pixels, transfer.pixels.data(), transfer.pixels.size());
            return true;
    }
    return false;
}

std::shared_ptr<ProbeLinux::GraphicsRegion> ProbeLinux::allocateGraphicsRegion(uint64_t requested) {
    if (!requested || requested > maxDataFileBytes) return {};
    const auto size = (requested + graphicsAlignment - 1) & ~(graphicsAlignment - 1);
    uint64_t offset = graphicsAlignment;
    for (auto it = graphicsRegions.begin(); it != graphicsRegions.end();) {
        if (const auto region = it->second.lock()) {
            if (it->first - offset >= size) break;
            offset = it->first + region->storage.size;
            ++it;
        } else {
            it = graphicsRegions.erase(it);
        }
    }
    if (offset > graphicsAddressBase - size) return {};
    try {
        auto region = std::make_shared<GraphicsRegion>();
        region->offset = offset;
        WritableFile backing{};
        backing.memoryFile = true;
        backing.storage = {region, &region->storage};
        if (resize(backing, size)) return {};
        if (memoryFiles.size() >= resourceObjectCapacity) return {};
        memoryFiles.push_back(backing.storage);
        graphicsRegions.insert_or_assign(offset, region);
        return region;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

uint64_t ProbeLinux::graphicsAddressIoctl(Machine& machine, const Descriptor& fd) {
    if (!fd.graphicsAddress) return uint64_t(-9);
    auto& device = *fd.graphicsAddress;
    const auto& x = machine.cpu.x;
    const auto load = [&]<typename Request>(Request& request) {
        return machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&request), sizeof(request)});
    };
    const auto release = [&](uint64_t offset) {
        const auto found = device.owned.find(offset);
        if (found == device.owned.end()) return false;
        found->second->released = true;
        device.owned.erase(found);
        return true;
    };
    switch (x[1]) {
        case 0xc018470a: { // ALLOCATE_BLOCK
            struct Block { uint64_t size, offset, physical; } request{};
            if (!load(request)) return uint64_t(-14);
            const auto allocation = allocateGraphicsRegion(request.size);
            if (!allocation) return noMemory;
            request = {allocation->storage.size, allocation->offset, graphicsAddressBase + allocation->offset};
            if (!writeGuest(machine, x[2], request)) return uint64_t(-14);
            device.owned.emplace(allocation->offset, allocation);
            return 0;
        }
        case 0xc008470b: case 0xc008470e: { // DEALLOCATE_BLOCK, UNCLAIM_SHARED
            uint64_t offset = 0;
            if (!load(offset)) return uint64_t(-14);
            if (x[1] == 0xc008470b) return release(offset) ? 0 : uint64_t(-6);
            const auto claim = device.claims.find(offset);
            if (claim == device.claims.end()) return uint64_t(-6);
            device.claims.erase(claim);
            return 0;
        }
        case 0xc010470d: { // CLAIM_SHARED
            struct Claim { uint64_t offset, size; } request{};
            if (!load(request)) return uint64_t(-14);
            const auto end = graphicsRegions.upper_bound(request.offset);
            if (!request.size || request.offset % pageSize || end == graphicsRegions.begin()) return invalid;
            const auto candidate = std::prev(end)->second.lock();
            if (!candidate || candidate->released) return uint64_t(-6);
            const auto position = request.offset - candidate->offset;
            if (position > candidate->storage.size || request.size > candidate->storage.size - position) return invalid;
            if (device.claims.size() >= resourceObjectCapacity) return uint64_t(-28);
            if (!writeGuest(machine, x[2], request)) return uint64_t(-14);
            device.claims.emplace(request.offset, GraphicsAddressSpace::Claim{candidate, request.size});
            return 0;
        }
        case 0xc028470c: { // PING
            struct Ping {
                uint64_t offset, size, metadata;
                uint32_t version, fence, flags, direction;
            } request{};
            if (!load(request)) return uint64_t(-14);
            if (request.offset > UINT64_MAX - graphicsAddressBase || request.flags) return invalid;
            const auto offset = request.offset;
            const auto command = request.metadata;
            request.offset += graphicsAddressBase;
            request.metadata = 0;
            if (!device.type) {
                if (command != goldfishHostMemory && command != goldfishSharedSlots) request.metadata = uint64_t(-19);
                if (!writeGuest(machine, x[2], request)) return uint64_t(-14);
                if (!request.metadata) device.type = command;
                return 0;
            }

            if (command == 1 && device.type == goldfishSharedSlots) {
                const auto allocation = allocateGraphicsRegion(request.size);
                if (allocation) {
                    request.offset = allocation->offset;
                    request.size = allocation->storage.size;
                } else {
                    request.metadata = noMemory;
                }
                if (!writeGuest(machine, x[2], request)) return uint64_t(-14);
                if (allocation) device.owned.emplace(allocation->offset, allocation);
                return 0;
            }

            const auto found = device.owned.find(offset);
            if (command != 1 && command != 2) request.metadata = invalid;
            else if (found == device.owned.end()) request.metadata = uint64_t(-6);
            else if (command == 1 && (!request.size || request.size > found->second->storage.size)) request.metadata = invalid;
            if (!writeGuest(machine, x[2], request)) return uint64_t(-14);
            if (!request.metadata && command == 2 && device.type == goldfishSharedSlots) release(offset);
            return 0;
        }
        default: return uint64_t(-25);
    }
}

uint64_t ProbeLinux::touchIoctl(Machine& machine, const Descriptor& fd) {
    if (!fd.touch) return uint64_t(-9);
    const auto& x = machine.cpu.x;
    const auto code = uint32_t(x[1]);
    const auto output = [&](const auto& value) {
        return writeGuest(machine, x[2], value) ? uint64_t{0} : uint64_t(-14);
    };
    if (code == 0x80044501) return output(uint32_t{0x010001}); // EVIOCGVERSION
    if (code == 0x80084502) return output(std::array<uint16_t, 4>{0x19, 0, 0, 1}); // EVIOCGID, BUS_HOST
    if (code == 0x400445a0) { // EVIOCSCLOCKID
        uint32_t clock = 0;
        if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&clock), sizeof(clock)})) return uint64_t(-14);
        return clock == 1 || clock == 7 ? 0 : invalid;
    }
    const auto size = (code >> 16) & 0x3fffU;
    const auto op = code & 0xffU;
    if ((code & 0xc000ff00U) != 0x80004500U) return uint64_t(-25);
    if (op == 0x40 || op == 0x41) { // EVIOCGABS
        if (size != 24) return invalid;
        const auto position = fd.touch->state();
        return output(std::array<int32_t, 6>{op == 0x40 ? position.x : position.y, 0,
                                           GuestTouch::maximumAxis, 0, 0, 0});
    }
    if (op == 0x06) { // EVIOCGNAME
        constexpr std::string_view name = "Nestvane Touchscreen";
        const auto count = std::min<uint64_t>(size, name.size() + 1);
        return machine.write(x[2], {reinterpret_cast<const uint8_t*>(name.data()), count}) ? count : uint64_t(-14);
    }
    if (op == 0x07 || op == 0x08) return uint64_t(-2); // No hardware physical path or serial.
    if (op != 0x09 && op != 0x18 && op != 0x1b && (op < 0x20 || op > 0x3f))
        return uint64_t(-25);
    std::array<uint8_t, 96> bits{};
    switch (op) {
        case 0x09: bits[0] = 1 << 1; break; // INPUT_PROP_DIRECT
        case 0x18:
            if (fd.touch->state().down) bits[GuestTouch::touchKey / 8] = 1 << (GuestTouch::touchKey % 8);
            break;
        case 0x20: bits[0] = (1 << 0) | (1 << 1) | (1 << 3); break; // EV_SYN, EV_KEY, EV_ABS
        case 0x21:
            for (const auto key : {GuestTouch::Key::wake, GuestTouch::Key::back, GuestTouch::Key::home, GuestTouch::Key::recents}) {
                const auto scanCode = static_cast<uint16_t>(key);
                bits[scanCode / 8] |= 1 << (scanCode % 8);
            }
            bits[GuestTouch::touchKey / 8] |= 1 << (GuestTouch::touchKey % 8);
            break;
        case 0x23: bits[0] = 3; break; // ABS_X, ABS_Y
        default: break;
    }
    const auto count = std::min<size_t>(size, bits.size());
    return machine.write(x[2], std::span(bits).first(count)) ? count : uint64_t(-14);
}

uint16_t ProbeLinux::GraphicsPipeState::pollEvents() const {
    if (vsock && !connected) return 0x10;
    constexpr uint16_t readable = 1, writable = 4, hangup = 16;
    if (failed) return hangup;
    if (!backend) return writable;
    return backend->pollEvents() | (readOffset < readSize ? readable : 0);
}

uint64_t ProbeLinux::readGraphics(Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size) {
    if (!pipe.serviceContext.empty() && current->process->context != pipe.serviceContext)
        return uint64_t(-13);
    if (pipe.vsock && !pipe.connected) return uint64_t(-107);
    if (!size) return 0;
    if (pipe.failed) return uint64_t(-5);
    if (!pipe.backend) return uint64_t(-11);
    if (pipe.readOffset == pipe.readSize) {
        const auto request = std::min<uint64_t>(size, maxGraphicsTransfer);
        if (pipe.readBuffer.size() < request) pipe.readBuffer.resize(request);
        const auto received = pipe.backend->read(std::span(pipe.readBuffer).first(request));
        if (pipe.renderChannel) {
            pipe.lastRead = received;
            if (received > 0) pipe.received += received;
            if (received == -11) ++pipe.readAgain;
        }
        if (received <= 0 || uint64_t(received) > request)
            return received <= 0 ? uint64_t(received) : uint64_t(-5);
        pipe.readOffset = 0;
        pipe.readSize = size_t(received);
    }
    const auto bytes = std::span(pipe.readBuffer).first(pipe.readSize).subspan(pipe.readOffset);
    const auto count = std::min<uint64_t>(size, bytes.size());
    if (!machine.write(address, bytes.first(count))) return uint64_t(-14);
    pipe.readOffset += count;
    return count;
}

bool ProbeLinux::writeCameraFrame(uint64_t offset, GuestCameraSource::Format format) {
    const auto bytes = GuestCameraSource::frameBytes(format);
    for (const auto& [_, descriptor] : descriptors()) {
        if (!descriptor.graphicsAddress) continue;
        const auto region = descriptor.graphicsAddress->mapping(offset, bytes);
        if (!region || region->released) continue;
        return camera->copyFrame(format, std::span(region->storage.pages).subspan(offset - region->offset, bytes));
    }
    return false;
}

uint64_t ProbeLinux::writeGraphics(const Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size) {
    if (!pipe.serviceContext.empty() && current->process->context != pipe.serviceContext)
        return uint64_t(-13);
    if (pipe.vsock && !pipe.connected) return uint64_t(-107);
    if (!size) return 0;
    if (pipe.failed) return uint64_t(-5);
    if (!pipe.backend) {
        // Goldfish opens a service by writing a NUL-terminated name to the device.
        // Return a short write for bytes after the name so retries cannot duplicate the handshake.
        constexpr size_t maxHeader = 256;
        const auto count = std::min<uint64_t>(size, maxHeader - pipe.serviceHeader.size());
        std::array<uint8_t, maxHeader> input{};
        const auto bytes = std::span(input).first(count);
        if (!machine.readInto(address, bytes)) return uint64_t(-14);
        const auto delimiter = std::ranges::find(bytes, uint8_t{0});
        pipe.serviceHeader.append(reinterpret_cast<const char*>(bytes.data()), delimiter - bytes.begin());
        if (delimiter == bytes.end() && pipe.serviceHeader.size() < maxHeader) return count;
        const auto service = std::string_view(pipe.serviceHeader);
        if (delimiter != bytes.end() && service.starts_with("pipe:")) {
            const auto name = service.substr(5);
            const auto cameraService = GuestCameraPipe::service(name);
            if (camera && current->process->context == cameraHalContext && cameraService) {
                pipe.serviceContext = cameraHalContext;
                pipe.backend = std::make_unique<GuestCameraPipe>(camera, cameraService->first, cameraService->second,
                    [this](auto offset, auto format) { return writeCameraFrame(offset, format); });
            } else if (name == GuestSensorsPipe::service && current->process->context == sensorsHalContext) {
                pipe.serviceContext = sensorsHalContext;
                pipe.backend = std::make_unique<GuestSensorsPipe>();
            } else if (!pipe.vsock && graphicsPipeFactory &&
                       (name == "opengles" || name == "GLProcessPipe" || name == "refcount")) {
                pipe.backend = graphicsPipeFactory(name);
                pipe.renderChannel = name == "opengles";
            }
        }
        pipe.failed = !pipe.backend;
        pipe.serviceHeader.clear();
        wakePollers();
        if (pipe.failed) return uint64_t(-2);
        return delimiter - bytes.begin() + 1;
    }
    const auto count = std::min<uint64_t>(size, maxGraphicsTransfer);
    std::vector<uint8_t> input(count);
    if (!machine.readInto(address, input)) return uint64_t(-14);
    const auto sent = pipe.backend->write(input);
    if (pipe.renderChannel) {
        pipe.lastWrite = sent;
        if (sent > 0) pipe.sent += sent;
        if (sent == -11) ++pipe.writeAgain;
    }
    return sent <= int64_t(input.size()) ? uint64_t(sent) : uint64_t(-5);
}

namespace {
struct SocketMessage {
    uint64_t name;
    uint32_t nameLength, namePadding;
    uint64_t vector, vectorCount, control, controlLength;
    uint32_t flags, tailPadding;
};
static_assert(sizeof(SocketMessage) == 56);

struct SocketControl {
    uint64_t length;
    int32_t level, type;
};
static_assert(sizeof(SocketControl) == 16);
constexpr int32_t socketControlLevel = 1, socketControlRights = 1;
constexpr size_t maxRightsPerMessage = 253;
constexpr size_t socketControlAlign(size_t size) { return (size + 7) & ~size_t{7}; }

uint64_t readSocketMessage(const Machine& machine, uint64_t address, SocketMessage& message,
                           std::vector<std::array<uint64_t, 2>>& vectors) {
    if (!machine.readInto(address, {reinterpret_cast<uint8_t*>(&message), sizeof(message)})) return uint64_t(-14);
    if (message.vectorCount > 1024) return uint64_t(-90);
    vectors.resize(message.vectorCount);
    if (!machine.readInto(message.vector, {reinterpret_cast<uint8_t*>(vectors.data()),
                                          vectors.size() * sizeof(vectors.front())})) return uint64_t(-14);
    return 0;
}
}

uint64_t ProbeLinux::socketWrite(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size) {
    const SocketVector vector{address, size};
    return socketWrite(machine, fd, std::span(&vector, 1));
}

uint64_t ProbeLinux::readSocketRights(const Machine& machine, uint64_t address, uint64_t length,
                                      SocketRights& rights) const {
    if (length > pageSize) return uint64_t(-105);
    if (length < sizeof(SocketControl)) return invalid;
    std::array<uint8_t, pageSize> buffer{};
    if (!machine.readInto(address, std::span(buffer).first(length))) return uint64_t(-14);
    for (size_t offset = 0; length - offset >= sizeof(SocketControl);) {
        SocketControl header{};
        std::memcpy(&header, buffer.data() + offset, sizeof(header));
        if (header.length < sizeof(header) || header.length > length - offset ||
            header.level != socketControlLevel || header.type != socketControlRights)
            return invalid;
        const auto bytes = header.length - sizeof(header);
        if (bytes % sizeof(int32_t) || rights.size() + bytes / sizeof(int32_t) > maxRightsPerMessage)
            return invalid;
        for (size_t i = 0; i < bytes; i += sizeof(int32_t)) {
            int32_t number;
            std::memcpy(&number, buffer.data() + offset + sizeof(header) + i, sizeof(number));
            const auto descriptor = descriptors().find(uint32_t(number));
            if (descriptor == descriptors().end()) return uint64_t(-9);
            const auto& fd = descriptor->second;
            // Only ordinary files, pipes and INET sockets are transferable here.
            // Queuing a UNIX endpoint could create an ownership cycle between sockets.
            const bool transferable = fd.node == Node::regular || fd.node == Node::temporary ||
                fd.pipe || (fd.socket && (fd.socket->family == 2 || fd.socket->family == 10));
            if (!transferable) return uint64_t(-95);
            rights.push_back(std::make_shared<Descriptor>(fd));
        }
        offset += std::min<uint64_t>(socketControlAlign(header.length), length - offset);
    }
    return 0;
}

uint64_t ProbeLinux::socketWrite(Machine& machine, const Descriptor& fd, std::span<const SocketVector> vectors,
                                SocketRights rights) {
    if (fd.socket->inetDatagram()) return sendInetDatagram(machine, fd.socket, vectors);
    if (fd.socket->netlink && fd.socket->protocol != 15) return sendKernelNetlink(machine, fd.socket, vectors);
    if (fd.socket->hostFd >= 0) {
        auto& endpoint = *fd.socket;
        if (endpoint.hostConnecting) hostSocketEvents(endpoint);
        if (endpoint.inetError) return uint64_t(-std::exchange(endpoint.inetError, 0));
        if (!endpoint.connected) return uint64_t(-107);
        if (endpoint.sendShutdown) return uint64_t(-32);
        std::vector<uint8_t> bytes;
        for (const auto& vector : vectors) {
            const uint64_t length = std::min<uint64_t>(vector[1], 256 * 1024 - bytes.size());
            const size_t offset = bytes.size();
            bytes.resize(offset + length);
            if (length && !machine.readInto(vector[0], std::span(bytes).subspan(offset, length))) return uint64_t(-14);
            if (length < vector[1]) break;
        }
        const auto sent = hostnet::send(endpoint.hostFd, bytes);
        if (sent == -11) hostSocketWait = true;
        return uint64_t(sent);
    }
    if (fd.socket->inetStream() && !inetLoopbackUp) return uint64_t(-100);
    if (fd.socket->inetError) return uint64_t(-std::exchange(fd.socket->inetError, 0));
    constexpr uint64_t maxPacketBytes = 65536;
    auto peer = fd.socket->peer.lock();
    if (!fd.socket->connected) return uint64_t(-107);
    if (!peer || fd.socket->sendShutdown || peer->receiveShutdown) return uint64_t(-32);
    uint64_t size = 0;
    for (const auto& vector : vectors) {
        if (vector[1] > maxPacketBytes - size) return uint64_t(-90);
        size += vector[1];
    }
    const bool stream = fd.socket->type == 1;
    if (!stream && size > fd.socket->sendBuffer) return uint64_t(-90);
    if (peer->queuedBytes >= fd.socket->sendBuffer || peer->inbox.size() >= 256) {
        peer->writerWaiting = true;
        return uint64_t(-11);
    }
    if (size > fd.socket->sendBuffer - peer->queuedBytes) {
        if (!stream) {
            peer->writerWaiting = true;
            return uint64_t(-11);
        }
        size = fd.socket->sendBuffer - peer->queuedBytes;
    }
    if (fd.socket->inetStream())
        size = std::min<uint64_t>(size, BpfProgram::maxPacketBytes - 100); // IPv6 + TCP with options.
    if (!size && stream) return 0;
    std::vector<uint8_t> packet(size);
    auto remaining = std::span(packet);
    for (const auto& [address, length] : vectors) {
        if (remaining.empty()) break;
        const auto count = std::min<uint64_t>(remaining.size(), length);
        if (!machine.readInto(address, remaining.first(count))) return uint64_t(-14);
        remaining = remaining.subspan(count);
    }
    if (fd.socket->inetStream()) {
        try {
            GuestFirewall::Rejection rejection;
            if (!transmitInetStream(*fd.socket, *peer, packet, 0x18, rejection)) { // PSH|ACK
                if (!rejection.reset) return uint64_t(-110);
                fd.socket->inetError = 104; // ECONNRESET on an established stream.
                fd.socket->receiveShutdown = fd.socket->sendShutdown = true;
                wakePollers();
                return size;
            }
        } catch (const std::bad_alloc&) {
            return noMemory;
        }
    }
    const bool streamRights = stream && !rights.empty();
    if (streamRights && peer->rights.size() >= 64) return uint64_t(-11);
    if (!stream && !rights.empty() && std::ranges::count_if(peer->inbox, [](const auto& queued) {
            return !queued.files.empty();
        }) >= 64) return uint64_t(-11);
    try {
        if (streamRights) {
            const auto firstByte = peer->streamOffset + peer->queuedBytes;
            peer->rights.push_back({firstByte, firstByte + size, std::move(rights)});
        }
        try {
            std::optional<std::array<uint32_t, 3>> sender;
            if (peer->family == 1 && !stream && (fd.socket->passCredentials || peer->passCredentials))
                sender = {uint32_t(processId()), current->process->userIds[0], current->process->groupIds[0]};
            peer->inbox.push_back({std::move(packet), sender, std::move(rights)});
        } catch (const std::bad_alloc&) {
            if (streamRights) peer->rights.pop_back();
            return noMemory;
        }
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
    peer->queuedBytes += size;
    ++peer->pollNotifications.readable;
    wakePollers(nullptr, false, fd.socket->family == 1 ? peer.get() : nullptr);
    return size;
}

bool ProbeLinux::writeSocketRights(Machine& machine, const SocketRights& rights, uint64_t control,
                                  uint64_t capacity, bool closeOnExec, uint64_t& used, uint32_t& flags,
                                  std::map<uint32_t, Descriptor>& transferred) {
    const auto fits = capacity > sizeof(SocketControl) ? (capacity - sizeof(SocketControl)) / sizeof(uint32_t) : 0;
    std::vector<uint32_t> numbers;
    numbers.reserve(std::min<uint64_t>(fits, rights.size()));
    for (size_t i = 0; i < fits && i < rights.size(); ++i) {
        const auto number = freeDescriptor(numbers.empty() ? 0 : numbers.back() + 1);
        if (number == descriptorCapacity) break;
        numbers.push_back(number);
    }
    if (numbers.size() < rights.size()) flags |= 0x8; // MSG_CTRUNC
    if (numbers.empty()) return true;
    const SocketControl header{sizeof(SocketControl) + numbers.size() * sizeof(numbers[0]),
                               socketControlLevel, socketControlRights};
    if (header.length > UINT64_MAX - control || !writeGuest(machine, control, header) ||
        !machine.write(control + sizeof(header), {reinterpret_cast<const uint8_t*>(numbers.data()),
                                                  numbers.size() * sizeof(numbers[0])})) return false;
    used = std::min<uint64_t>(capacity, socketControlAlign(header.length));
    for (size_t i = 0; i < numbers.size(); ++i) {
        auto copy = *rights[i];
        copy.flags = (copy.flags & ~0x80000U) | (closeOnExec ? 0x80000U : 0);
        transferred.emplace(numbers[i], std::move(copy));
    }
    return true;
}

uint64_t ProbeLinux::receiveUnixStreamMessage(Machine& machine, SocketState& endpoint) {
    const auto& x = machine.cpu.x;
    constexpr uint64_t peek = 0x2, dontWait = 0x40, closeOnExec = 0x40000000;
    // Linux ignores MSG_CTRUNC, MSG_TRUNC and MSG_NOSIGNAL on UNIX stream reads.
    constexpr uint64_t ignoredReceiveFlags = 0x8 | 0x20 | 0x4000;
    if (endpoint.passCredentials || (x[2] & ~(peek | dontWait | closeOnExec | ignoredReceiveFlags)))
        return uint64_t(-95);
    SocketMessage message{};
    std::vector<SocketVector> vectors;
    if (const auto error = readSocketMessage(machine, x[1], message, vectors)) return error;
    uint64_t count = 0;
    for (const auto& vector : vectors) {
        if (vector[1] > uint64_t(INT64_MAX) - count) return invalid;
        count += vector[1];
    }
    if (count && endpoint.inbox.empty()) {
        const auto peer = endpoint.peer.lock();
        if (!endpoint.receiveShutdown && (!endpoint.connected || (peer && !peer->sendShutdown)))
            return uint64_t(endpoint.connected ? -11 : -107);
    }
    count = std::min<uint64_t>(count, endpoint.queuedBytes);
    const auto* rights = endpoint.rights.empty() ? nullptr : &endpoint.rights.front();
    if (rights && rights->firstByte - endpoint.streamOffset < count)
        count = std::min(count, rights->endByte - endpoint.streamOffset);
    else
        rights = nullptr;

    std::vector<uint8_t> payload;
    payload.reserve(count);
    for (const auto& queued : endpoint.inbox) {
        const auto& packet = queued.payload;
        const auto chunk = std::min<size_t>(packet.size(), count - payload.size());
        payload.insert(payload.end(), packet.begin(), packet.begin() + chunk);
        if (payload.size() == count) break;
    }
    auto remaining = std::span(payload);
    for (const auto& [address, size] : vectors) {
        if (remaining.empty()) break;
        const auto copied = std::min<uint64_t>(size, remaining.size());
        if (!machine.write(address, remaining.first(copied))) return uint64_t(-14);
        remaining = remaining.subspan(copied);
    }
    message.flags = uint32_t(x[2] & closeOnExec);
    message.nameLength = 0; // Connected stream; no datagram source address.
    const auto capacity = message.control ? message.controlLength : 0;
    message.controlLength = 0;
    std::map<uint32_t, Descriptor> transferred;
    if (rights && !writeSocketRights(machine, rights->files, message.control, capacity, x[2] & closeOnExec,
                                     message.controlLength, message.flags, transferred)) return uint64_t(-14);
    if (!writeGuest(machine, x[1], message)) return uint64_t(-14);
    descriptors().merge(transferred);
    if (!(x[2] & peek)) consumeSocketStream(endpoint, count);
    return count;
}

uint64_t ProbeLinux::receiveKernelNetlink(Machine& machine, SocketState& endpoint) {
    const auto& x = machine.cpu.x;
    if (x[2] & ~0x40ULL) return invalid; // MSG_DONTWAIT
    if (endpoint.inbox.empty()) return uint64_t(-11);
    SocketMessage message{};
    std::vector<uint8_t> headerScratch;
    const auto* header = machine.read(x[1], sizeof(message), 4, headerScratch);
    if (!header) return uint64_t(-14);
    std::memcpy(&message, header, sizeof(message));
    std::array<uint64_t, 2> vector{};
    if (message.vectorCount != 1) return invalid;
    std::vector<uint8_t> iovScratch;
    const auto* iov = machine.read(message.vector, sizeof(vector), 4, iovScratch);
    if (!iov) return uint64_t(-14);
    std::memcpy(vector.data(), iov, sizeof(vector));
    const auto& packet = endpoint.inbox.front().payload;
    const auto copied = std::min<uint64_t>(packet.size(), vector[1]);
    if (copied && !machine.write(vector[0], std::span(packet).first(copied))) return uint64_t(-14);
    message.flags = copied < packet.size() ? 0x20 : 0; // MSG_TRUNC
    if (message.name) {
        const std::array<uint32_t, 3> address{16, 0, endpoint.protocol == 15 ? 1U : 0U};
        const auto size = std::min<size_t>(sizeof(address), message.nameLength);
        if (size && !machine.write(message.name, {reinterpret_cast<const uint8_t*>(address.data()), size}))
            return uint64_t(-14);
        message.nameLength = sizeof(address);
    } else {
        message.nameLength = 0;
    }
    const std::array<uint64_t, 4> credentials{28, (uint64_t{2} << 32) | 1, 0, 0}; // SCM_CREDENTIALS
    const auto available = message.control ? message.controlLength : 0;
    message.controlLength = 0;
    const auto control = [&](const auto& value) {
        if (sizeof(value) > available - message.controlLength) {
            message.flags |= 0x8; // MSG_CTRUNC
            return true;
        }
        if (!writeGuest(machine, message.control + message.controlLength, value)) return false;
        message.controlLength += sizeof(value);
        return true;
    };
    if (endpoint.passCredentials && !control(credentials)) return uint64_t(-14);
    if (endpoint.netlink->timestamp) {
        const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
            realtimeAt(std::chrono::steady_clock::now())).count();
        const std::array<uint64_t, 4> timestamp{32, (uint64_t{29} << 32) | 1,
                                            uint64_t(usec / 1000000), uint64_t(usec % 1000000)};
        if (!control(timestamp)) return uint64_t(-14);
    }
    if (!writeGuest(machine, x[1], message)) return uint64_t(-14);
    endpoint.queuedBytes -= packet.size();
    endpoint.inbox.pop_front();
    return copied;
}

uint64_t ProbeLinux::receiveKernelNetlinkFrom(Machine& machine, SocketState& endpoint) {
    const auto& x = machine.cpu.x;
    constexpr uint64_t peek = 2, trunc = 0x20, dontwait = 0x40;
    if (x[3] & ~(peek | trunc | dontwait)) return invalid;
    if (endpoint.inbox.empty()) return uint64_t(-11);
    const auto& packet = endpoint.inbox.front().payload;
    const auto bytes = std::min<uint64_t>(packet.size(), x[2]);
    if (bytes && !machine.write(x[1], std::span(packet).first(bytes))) return uint64_t(-14);
    if (x[4]) {
        uint32_t capacity = 0;
        if (!machine.readInto(x[5], {reinterpret_cast<uint8_t*>(&capacity), sizeof(capacity)}))
            return uint64_t(-14);
        if (capacity > INT32_MAX) return invalid;
        const std::array<uint32_t, 3> address{16, 0, endpoint.protocol == 15 ? 1U : 0U};
        const size_t written = std::min<size_t>(capacity, sizeof(address));
        if ((written && !machine.write(x[4], {reinterpret_cast<const uint8_t*>(address.data()), written})) ||
            !writeGuest(machine, x[5], uint32_t(sizeof(address)))) return uint64_t(-14);
    }
    const auto result = x[3] & trunc ? packet.size() : bytes;
    if (!(x[3] & peek)) {
        endpoint.queuedBytes -= packet.size();
        endpoint.inbox.pop_front();
        wakePollers();
    }
    return result;
}

namespace {
constexpr uint16_t netfilterLogPacket = 4 << 8, netfilterLogConfig = netfilterLogPacket | 1;
constexpr size_t netfilterHeader = sizeof(NetlinkHeader) + 4;
constexpr uint16_t xfrmGetState = 0x12, xfrmGetPolicy = 0x15, xfrmFlushState = 0x1c, xfrmFlushPolicy = 0x1d;

bool isXfrmDump(const NetlinkHeader& request) {
    return (request.flags & netlinkDump) == netlinkDump &&
           (request.type == xfrmGetState || request.type == xfrmGetPolicy);
}

int32_t handleGuestXfrm(const NetlinkHeader& request, std::span<const uint8_t> payload, bool admin, auto&& reply) {
    if (!admin) return -1;
    // The guest has no IPsec transform data plane. Its policy and SA tables are always empty.
    if (isXfrmDump(request)) {
        if (!payload.empty()) return -95;
        reply(netlinkDone, netlinkMulti, std::vector<uint8_t>(sizeof(int32_t)));
        return 0;
    }
    if (request.type == xfrmFlushState) {
        if (payload.empty()) return -22;
        return payload.size() <= 4 ? 0 : -95;
    }
    if (request.type == xfrmFlushPolicy) return payload.empty() ? 0 : -95;
    return -95;
}

std::array<uint8_t, 4> netlinkNetwork32(uint32_t value) {
    return {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
}
} // namespace

void ProbeLinux::keepDiagnosticLine(std::string_view message) {
    static const auto began = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    std::string line = std::to_string(ms / 1000) + "." + std::to_string(ms % 1000 + 1000).substr(1) + " " +
        std::to_string(processId()) + " " + std::string(message.substr(0, 8192));
    line.push_back('\n');
    // A quarter of a megabyte at most: some minutes of a quiet system, the last seconds of a busy one.
    // Each line is only appended: the boot logs megabytes, and moving the kept text up for each line
    // cost the start most of a second.
    constexpr size_t half = 128 * 1024;
    if (diagnosticLog.size() + line.size() > half) {
        diagnosticLogOlder.swap(diagnosticLog);
        diagnosticLog.clear();
    }
    diagnosticLog += line;
    if (diagnosticCapturing && diagnosticCapture.size() < 4 * 1024 * 1024) diagnosticCapture += line;
}

bool ProbeLinux::requestJavaDump(std::string_view name) {
    const auto named = [](const Thread& thread) {
        return std::string_view(reinterpret_cast<const char*>(thread.name.data()),
                                strnlen(reinterpret_cast<const char*>(thread.name.data()), thread.name.size()));
    };
    const Process* process = nullptr;
    for (const auto& [tid, thread] : threads)
        if (tid == thread.process->id && named(thread) == name) process = thread.process.get();
    if (!process) return false;
    for (auto& [tid, thread] : threads) {
        if (thread.process.get() != process || named(thread) != "Signal Catcher") continue;
        SignalWords info{};
        info[0] = 3;  // SIGQUIT
        // With no tombstoned to take the dump, the runtime writes it into the log line by line.
        javaDumpsToLog = true;
        thread.pendingSignals.emplace(3, info);
        if (thread.wait == Wait::signal) thread.wait = Wait::none;
        sliceEnd = 0;
        return true;
    }
    return false;
}

std::string ProbeLinux::guestFiles(std::string_view directory, size_t limit) const {
    std::string text;
    for (const auto& [path, file] : writableFiles) {
        if (!std::string_view(path).starts_with(directory)) continue;
        const auto size = std::min<size_t>(file->storage->size, limit);
        text += "---- " + path + " (" + std::to_string(file->storage->size) + " bytes)\n";
        text.append(reinterpret_cast<const char*>(file->storage->pages.data()), size);
        text += '\n';
    }
    return text;
}

std::string ProbeLinux::pathReport(std::string_view path) const {
    if (path.empty() || path[0] != '/') return {};
    const auto& process = *current->process;
    std::string text = "uid=" + std::to_string(process.userIds[1]) + " gid=" + std::to_string(process.groupIds[1]) + " groups=";
    for (const auto group : process.groups) text += std::to_string(group) + ",";
    for (size_t end = path.find('/', 1);; end = path.find('/', end + 1)) {
        const auto part = path.substr(0, end);
        auto backing = backingPath(part, *current->mountNamespace);
        std::string shown = backing;
        for (auto& c : shown) if (!c) c = '?';
        text += " | " + std::string(part) + " -> " + shown;
        if (const auto found = directories.find(backing); found != directories.end()) {
            char mode[48];
            std::snprintf(mode, sizeof(mode), " %o %u:%u", found->second.mode, found->second.uid, found->second.gid);
            text += mode;
        } else {
            text += " (no directory)";
        }
        if (end == std::string_view::npos) break;
    }
    return text;
}

void ProbeLinux::enableSound() {
    addDirectory("/dev/snd", 0755);
    addDirectory("/dev/dma_heap", 0755);
}

uint64_t ProbeLinux::allocateDmaBuffer(Machine& machine) {
    auto& x = machine.cpu.x;
    struct Allocation {
        uint64_t length;
        uint32_t fd, fdFlags;
        uint64_t heapFlags;
    } request;
    static_assert(sizeof(Allocation) == 24);
    if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&request), sizeof(request)})) return uint64_t(-14);
    if (!request.length || request.heapFlags || (request.fdFlags & ~0x80003U)) return invalid;
    const auto fd = freeDescriptor();
    if (fd == descriptorCapacity) return uint64_t(-24);
    std::erase_if(memoryFiles, [](const auto& file) { return file.expired(); });
    if (memoryFiles.size() >= resourceObjectCapacity) return uint64_t(-23);
    auto file = std::make_shared<WritableFile>();
    file->inode = nextInode++;
    file->mode = 0666;
    file->uid = userId();
    file->gid = groupId();
    file->memoryFile = true;
    memoryFiles.push_back(file->storage);
    if (const auto status = resize(*file, request.length)) {
        memoryFiles.pop_back();
        return status;
    }
    file->seals = sealSeal | sealShrink | sealGrow;
    Descriptor entry{"/dmabuf:", {}, (request.fdFlags & 0x80000U) | 2U, Node::temporary};
    entry.temporary = std::move(file);
    descriptors().emplace(fd, std::move(entry));
    request.fd = fd;
    return machine.write(x[2], {reinterpret_cast<const uint8_t*>(&request), sizeof(request)}) ? 0 : uint64_t(-14);
}

// The frames the guest has written that the card has not played yet. With the phone's output taking the
// sound, that is what waits for it beyond the cushion it keeps (runtime/GuestAudio.hpp); with nobody
// taking it, the sound is played by the clock, into nothing.
uint64_t ProbeLinux::soundPending() {
    auto& audio = GuestAudio::host();
    if (audio.consumer.load() && audio.rate.load()) return audio.pending() / std::max<uint32_t>(soundFrameBytes, 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - soundClockStart).count();
    const uint64_t played = soundClockFrames + uint64_t(std::max<int64_t>(elapsed, 0)) * soundRate / 1'000'000;
    return played >= soundFrames ? 0 : soundFrames - played;
}

// The card's buffer is as long as the guest was told, like a real card's: Android's audio service works
// out when to write, and when to fill in silence, from how full it is.
uint64_t ProbeLinux::soundRoom() {
    const uint64_t pending = soundPending();
    return pending >= soundBufferFrames ? 0 : soundBufferFrames - pending;
}

// The frames the capture device has for the guest: what the host's microphone has put in, or, with
// nobody recording, what the clock says a card would have heard since the guest last took any.
uint64_t ProbeLinux::microphoneAvailable() {
    auto& microphone = GuestMicrophone::host();
    const bool fromHost = microphone.producer.load();
    const auto now = std::chrono::steady_clock::now();
    if (fromHost != microphoneFromHost) {
        microphoneFromHost = fromHost;
        microphoneClockStart = now;
        microphoneClockFrames = microphoneFrames;
    }
    if (fromHost) return microphone.buffered() / std::max<uint32_t>(microphoneFrameBytes, 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - microphoneClockStart).count();
    const uint64_t heard = microphoneClockFrames + uint64_t(std::max<int64_t>(elapsed, 0)) * microphoneRate / 1'000'000;
    if (heard > microphoneFrames + microphoneBufferFrames) { // nobody read for a while: the card overran, and starts afresh
        microphoneClockStart = now;
        microphoneClockFrames = microphoneFrames;
        return 0;
    }
    return heard > microphoneFrames ? heard - microphoneFrames : 0;
}

std::optional<uint64_t> ProbeLinux::soundIoctl(Machine& machine, bool nonblocking, bool capture) {
    auto& x = machine.cpu.x;
    auto& audio = GuestAudio::host();
    auto& microphone = GuestMicrophone::host();
    if (capture) switch (uint32_t(x[1])) {
        case 0x81204101: { // SNDRV_PCM_IOCTL_INFO: card 0, device 0, capture
            std::array<uint8_t, 288> info{};
            info[8] = 1;
            return machine.write(x[2], info) ? 0 : uint64_t(-14);
        }
        case 0xc0884113: { // SNDRV_PCM_IOCTL_SW_PARAMS: how much there must be before a wait for sound ends
            uint64_t minimum;
            if (!machine.readInto(x[2] + 16, {reinterpret_cast<uint8_t*>(&minimum), sizeof(minimum)})) return uint64_t(-14);
            microphoneAvailableMinimum = uint32_t(std::clamp<uint64_t>(minimum, 1, microphoneBufferFrames));
            return 0;
        }
        case 0x80184151: { // SNDRV_PCM_IOCTL_READI_FRAMES
            std::array<uint64_t, 3> transfer;
            if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(transfer.data()), sizeof(transfer)})) return uint64_t(-14);
            const auto frameBytes = std::max<uint32_t>(microphoneFrameBytes, 1);
            if (transfer[2] > (1 << 20) / frameBytes) return invalid;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            microphone.wanted = uint64_t(now);
            const uint64_t frames = std::min<uint64_t>(transfer[2], microphoneAvailable());
            if (!frames && transfer[2]) {
                if (nonblocking) return uint64_t(-11);
                return std::nullopt;
            }
            std::vector<uint8_t> samples(size_t(frames) * frameBytes);
            if (microphoneFromHost) microphone.read(samples.data(), samples.size());
            if (!machine.write(transfer[1], samples)) return uint64_t(-14);
            microphoneFrames += frames;
            transfer[0] = frames;
            return machine.write(x[2], {reinterpret_cast<const uint8_t*>(transfer.data()), sizeof(transfer)}) ? 0 : uint64_t(-14);
        }
        case 0xc0884123: { // SNDRV_PCM_IOCTL_SYNC_PTR: running, and how far the recording has come
            std::array<uint64_t, 17> pointers;
            if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(pointers.data()), sizeof(pointers)})) return uint64_t(-14);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            pointers[1] = 3;  // SNDRV_PCM_STATE_RUNNING
            pointers[2] = microphoneFrames + microphoneAvailable();
            pointers[3] = uint64_t(now / 1'000'000'000);
            pointers[4] = uint64_t(now % 1'000'000'000);
            pointers[9] = microphoneFrames;
            pointers[10] = microphoneAvailableMinimum;
            return machine.write(x[2], {reinterpret_cast<const uint8_t*>(pointers.data()), sizeof(pointers)}) ? 0 : uint64_t(-14);
        }
        case 0x80084121: // SNDRV_PCM_IOCTL_DELAY
            return machine.store64(x[2], microphoneAvailable()) ? 0 : uint64_t(-14);
        case 0xc2604111: break; // the parameters, below
        default: return 0;
    }
    const auto frameBytes = std::max<uint32_t>(soundFrameBytes, 1);
    // NESTVANE_SOUND_TRACE=1: what the guest asks of its sound device, in the Android log.
    if (static const bool traced = std::getenv("NESTVANE_SOUND_TRACE"); traced && androidLogObserver) {
        static uint32_t last = 0;
        static uint64_t repeats = 0;
        if (uint32_t(x[1]) != last || ++repeats % 200 == 0) {
            char line[160];
            std::snprintf(line, sizeof(line), "native execution: sound ioctl %#x, %llu frames so far, consumer %d, buffered %zu",
                          uint32_t(x[1]), static_cast<unsigned long long>(soundFrames), int(audio.consumer.load()), audio.buffered());
            androidLogObserver(line);
            last = uint32_t(x[1]);
        }
    }
    switch (uint32_t(x[1])) {
        // The card's control device: a card with a name and no controls. Android's audio service will not use a
        // playback device whose card has no mixer to open.
        case 0x80045500: // SNDRV_CTL_IOCTL_PVERSION: 2.0.7
        {
            const uint32_t version = 0x20007;
            return machine.write(x[2], {reinterpret_cast<const uint8_t*>(&version), sizeof(version)}) ? 0 : uint64_t(-14);
        }
        case 0x81785501: { // SNDRV_CTL_IOCTL_CARD_INFO
            std::array<uint8_t, 376> card{};
            const auto name = [&](size_t offset, std::string_view text) { std::copy(text.begin(), text.end(), card.begin() + offset); };
            name(8, "Nestvane");     // id
            name(24, "nestvane");    // driver
            name(40, "Nestvane");    // name
            name(72, "Nestvane sound");  // long name
            return machine.write(x[2], card) ? 0 : uint64_t(-14);
        }
        case 0xc0505510: { // SNDRV_CTL_IOCTL_ELEM_LIST: none used, none there
            const std::array<uint32_t, 2> none{};
            return machine.write(x[2] + 8, {reinterpret_cast<const uint8_t*>(none.data()), sizeof(none)}) ? 0 : uint64_t(-14);
        }
        case 0x81204101: { // SNDRV_PCM_IOCTL_INFO: card 0, device 0, playback
            const std::array<uint8_t, 288> info{};
            return machine.write(x[2], info) ? 0 : uint64_t(-14);
        }
        case 0xc2604111: { // SNDRV_PCM_IOCTL_HW_PARAMS: whatever is asked for is what there is
            std::array<uint32_t, 152> parameters;
            if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(parameters.data()), sizeof(parameters)})) return uint64_t(-14);
            // The intervals begin after the flags and eight masks; each is a minimum, a maximum and flags.
            const auto interval = [&](unsigned index) { return parameters[65 + index * 3]; };
            if (static const bool traced = std::getenv("NESTVANE_SOUND_TRACE"); traced && androidLogObserver) {
                std::string line = "native execution: sound parameters";
                for (unsigned index = 0; index < 12; ++index)
                    line += " " + std::to_string(interval(index)) + "-" + std::to_string(parameters[66 + index * 3]);
                line += " masks " + std::to_string(parameters[1]) + " " + std::to_string(parameters[9]) + " " + std::to_string(parameters[17]);
                androidLogObserver(line);
            }
            // The sample format comes as a set of formats: signed 16 bits (2) or signed 32 bits (10), little-endian.
            const uint32_t formats = parameters[9];
            const uint32_t format = formats & (1u << 2) ? 2 : formats & (1u << 10) ? 10 : 0;
            const uint32_t bits = format == 2 ? 16 : 32, channels = interval(2), rate = interval(3);
            if (!format || !channels || channels > 8 || rate < 8000 || rate > 192000) return invalid;
            const uint32_t period = std::clamp<uint32_t>(interval(5), 64, 1 << 16), periods = std::clamp<uint32_t>(interval(7), 2, 64);
            // The answer is what the device settled on: every interval one whole number, each set one member.
            const auto settle = [&](unsigned index, uint64_t value) {
                parameters[65 + index * 3] = parameters[66 + index * 3] = uint32_t(value);
                parameters[67 + index * 3] = 4;  // an integer
            };
            const auto only = [&](unsigned mask, uint32_t member) {
                std::fill_n(parameters.begin() + 1 + mask * 8, 8, 0);
                parameters[1 + mask * 8] = 1u << member;
            };
            only(0, 3);  // read and write calls, interleaved
            only(1, format);
            only(2, 0);
            const uint64_t frame = uint64_t(channels) * bits / 8, buffer = uint64_t(period) * periods;
            settle(0, bits);
            settle(1, frame * 8);
            settle(2, channels);
            settle(3, rate);
            settle(4, uint64_t(period) * 1'000'000 / rate);
            settle(5, period);
            settle(6, period * frame);
            settle(7, periods);
            settle(8, buffer * 1'000'000 / rate);
            settle(9, buffer);
            settle(10, buffer * frame);
            settle(11, 0);
            parameters[131] = bits;  // significant bits
            parameters[132] = rate;  // the rate as a fraction
            parameters[133] = 1;
            if (!machine.write(x[2], {reinterpret_cast<const uint8_t*>(parameters.data()), sizeof(parameters)})) return uint64_t(-14);
            if (capture) {
                microphoneFrameBytes = channels * bits / 8;
                microphoneBufferFrames = buffer;
                microphoneAvailableMinimum = period;
                microphoneRate = rate;
                microphoneFrames = microphoneClockFrames = 0;
                microphoneClockStart = std::chrono::steady_clock::now();
                microphoneFromHost = false;
                microphone.configure(rate, channels, bits);
                return 0;
            }
            soundFrameBytes = channels * bits / 8;
            soundBufferFrames = buffer;
            soundAvailableMinimum = period;
            soundRate = rate;
            soundFrames = soundClockFrames = 0;
            soundClockStart = std::chrono::steady_clock::now();
            audio.configure(rate, channels, bits);
            return 0;
        }
        case 0xc0884113: { // SNDRV_PCM_IOCTL_SW_PARAMS: how much room there must be before a wait for room ends
            uint64_t minimum;
            if (!machine.readInto(x[2] + 16, {reinterpret_cast<uint8_t*>(&minimum), sizeof(minimum)})) return uint64_t(-14);
            soundAvailableMinimum = uint32_t(std::clamp<uint64_t>(minimum, 1, soundBufferFrames));
            return 0;
        }
        case 0x40184150: { // SNDRV_PCM_IOCTL_WRITEI_FRAMES
            std::array<uint64_t, 3> transfer;
            if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(transfer.data()), sizeof(transfer)})) return uint64_t(-14);
            if (transfer[2] > (1 << 20) / frameBytes) return invalid;
            // As much as there is room for, as a real card takes it: Android's audio service writes whatever it
            // has whenever the card has any room, and counts on the rest being refused, not waited for. A wait
            // for the whole of a long write would let the card run empty while what comes next piles up.
            const uint64_t frames = std::min<uint64_t>(transfer[2], soundRoom());
            if (!frames && transfer[2]) {
                if (nonblocking) return uint64_t(-11);
                return std::nullopt;
            }
            const size_t bytes = size_t(frames) * frameBytes;
            if (audio.consumer.load() && audio.rate.load()) {
                std::vector<uint8_t> samples(bytes);
                if (!machine.readInto(transfer[1], samples)) return uint64_t(-14);
                audio.write(samples.data(), bytes);
            } else if (!soundPending()) {
                // Nothing was playing: the clock that plays into nothing starts again from here.
                soundClockStart = std::chrono::steady_clock::now();
                soundClockFrames = soundFrames;
            }
            soundFrames += frames;
            audio.played += bytes;
            transfer[0] = frames;
            return machine.write(x[2], {reinterpret_cast<const uint8_t*>(transfer.data()), sizeof(transfer)}) ? 0 : uint64_t(-14);
        }
        case 0xc0884123: { // SNDRV_PCM_IOCTL_SYNC_PTR: running, and how far the playing has come
            std::array<uint64_t, 17> pointers;
            if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(pointers.data()), sizeof(pointers)})) return uint64_t(-14);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            pointers[1] = 3;  // SNDRV_PCM_STATE_RUNNING
            pointers[2] = soundFrames - std::min(soundFrames, soundPending());
            pointers[3] = uint64_t(now / 1'000'000'000);
            pointers[4] = uint64_t(now % 1'000'000'000);
            pointers[9] = soundFrames;
            pointers[10] = soundAvailableMinimum;
            return machine.write(x[2], {reinterpret_cast<const uint8_t*>(pointers.data()), sizeof(pointers)}) ? 0 : uint64_t(-14);
        }
        case 0x80084121: // SNDRV_PCM_IOCTL_DELAY
            return machine.store64(x[2], soundPending()) ? 0 : uint64_t(-14);
        default: // parameters that need no answer, and prepare, start, drop, drain
            return 0;
    }
}

void ProbeLinux::enableBattery() {
    if (GuestBattery::host().level.load() < 0) return;
    addDirectory("/sys/class/power_supply", 0555);
    addDirectory("/sys/class/power_supply/battery", 0555);
    addDirectory("/sys/class/power_supply/ac", 0555);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"battery/type", "Battery\n"}, {"battery/present", "1\n"}, {"battery/health", "Good\n"},
             {"battery/technology", "Li-ion\n"}, {"battery/voltage_now", "4000000\n"}, {"battery/temp", "250\n"},
             {"battery/status", "Unknown\n"}, {"battery/capacity", "50\n"}, {"ac/type", "Mains\n"}, {"ac/online", "0\n"}})
        kernelFiles.insert_or_assign(std::string("/sys/class/power_supply/") + name, value);
    refreshBattery();
}

void ProbeLinux::refreshBattery() {
    const auto& battery = GuestBattery::host();
    const int level = battery.level.load(), state = battery.state.load();
    if (level < 0) return;
    kernelFiles["/sys/class/power_supply/battery/capacity"] = std::to_string(std::clamp(level, 0, 100)) + "\n";
    kernelFiles["/sys/class/power_supply/battery/status"] =
        state == GuestBattery::charging ? "Charging\n" : state == GuestBattery::full ? "Full\n" :
        state == GuestBattery::unplugged ? "Discharging\n" : "Unknown\n";
    kernelFiles["/sys/class/power_supply/ac/online"] = state == GuestBattery::charging || state == GuestBattery::full ? "1\n" : "0\n";
}

void ProbeLinux::enableHostNetwork() {
    hostNetwork = true;
    if (std::getenv("NESTVANE_NETWORK_TRACE")) {
        // Whether the host lets this program out at all: a datagram and a connection to a public address.
        sockaddr_in target{};
        target.sin_len = sizeof(target);
        target.sin_family = AF_INET;
        target.sin_port = htons(53);
        inet_pton(AF_INET, "223.5.5.5", &target.sin_addr);
        const int datagram = ::socket(AF_INET, SOCK_DGRAM, 0);
        const char probe[] = "x";
        const auto sent = ::sendto(datagram, probe, 1, 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        const int sendError = sent < 0 ? errno : 0;
        ::close(datagram);
        const int stream = ::socket(AF_INET, SOCK_STREAM, 0);
        const int connected = ::connect(stream, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        const int connectError = connected ? errno : 0;
        ::close(stream);
        std::fprintf(stderr, "[network] host self-test: datagram to 223.5.5.5:53 errno %d, connection errno %d\n", sendError, connectError);
    }
    auto& ethernet = guestRoutes.ethernet;
    ethernet.present = true;
    ethernet.hardware = hostnet::hardwareAddress;
    char hardware[32];
    std::snprintf(hardware, sizeof(hardware), "%02x:%02x:%02x:%02x:%02x:%02x\n", ethernet.hardware[0], ethernet.hardware[1],
                  ethernet.hardware[2], ethernet.hardware[3], ethernet.hardware[4], ethernet.hardware[5]);
    addDirectory("/sys/class/net/eth0", 0555);
    kernelFiles.emplace("/sys/class/net/eth0/ifindex", std::to_string(ethernet.index) + "\n");
    kernelFiles.emplace("/sys/class/net/eth0/address", hardware);
    kernelFiles.emplace("/sys/class/net/eth0/mtu", std::to_string(ethernet.mtu) + "\n");
    kernelFiles.emplace("/sys/class/net/eth0/type", "1\n");
    kernelFiles.emplace("/sys/class/net/eth0/operstate", "up\n");
    kernelFiles.emplace("/sys/class/net/eth0/carrier", "1\n");
    kernelFiles.emplace("/sys/class/net/eth0/flags", "0x1043\n");
    // The IPv6 addresses of all interfaces: none, the guest's network is IPv4.
    addDirectory("/proc/net", 0555);
    kernelFiles.emplace("/proc/net/if_inet6", "");
    // Changes of links and addresses go to the route sockets that asked for their group.
    guestRoutes.notify = [this](uint32_t groups, uint16_t type, std::vector<uint8_t> payload) {
        const NetlinkHeader header{uint32_t(sizeof(NetlinkHeader) + payload.size()), type, 0, 0, 0};
        std::vector<uint8_t> packet(sizeof(header) + payload.size());
        std::memcpy(packet.data(), &header, sizeof(header));
        std::ranges::copy(payload, packet.begin() + sizeof(header));
        for (const auto& [port, listener] : netlinkPorts) {
            const auto endpoint = listener.lock();
            if (endpoint && port.first == 0 && endpoint->netlink && (endpoint->netlink->groups & groups))
                queueNetlink(*endpoint, packet);
        }
    };
}

uint64_t ProbeLinux::bindNetlink(const std::shared_ptr<SocketState>& endpoint, uint32_t port, uint32_t groups) {
    auto& socket = *endpoint->netlink;
    if (socket.pid) return invalid;
    if (endpoint->protocol == 15) {
        if (groups && userId() != 0) return uint64_t(-1);
        socket.pid = port ? port : uint32_t(processId());
        socket.groups = groups;
        if (groups & 1) ueventListeners.push_back(endpoint);
        return 0;
    }
    if (groups && endpoint->protocol != 0) return uint64_t(-95);
    std::erase_if(netlinkPorts, [](const auto& item) { return item.second.expired(); });
    if (netlinkPorts.size() >= resourceObjectCapacity) return uint64_t(-28);
    const auto used = [&](uint32_t id) { return netlinkPorts.contains({endpoint->protocol, id}); };
    if (port && used(port)) return uint64_t(-98);
    if (!port) {
        port = processId();
        while (!port || used(port)) port = nextNetlinkPort++;
    }
    netlinkPorts.emplace(std::pair(endpoint->protocol, port), endpoint);
    socket.pid = port;
    socket.groups = groups;
    return 0;
}

void ProbeLinux::queueNetlink(SocketState& endpoint, std::vector<uint8_t> packet) {
    if (endpoint.inbox.size() >= 256 || endpoint.queuedBytes + packet.size() > endpoint.netlink->receiveBuffer) return;
    const auto size = packet.size();
    endpoint.inbox.push_back({std::move(packet)});
    endpoint.queuedBytes += size;
    wakePollers();
}

uint64_t ProbeLinux::sendKernelNetlink(Machine& machine, const std::shared_ptr<SocketState>& endpoint,
    std::span<const SocketVector> vectors, uint64_t address, uint64_t length) {
    const bool route = endpoint->protocol == 0, xfrm = endpoint->protocol == 6;
    const bool admin = current->process->effectiveCaps & (1ULL << 12);
    if (!route && !xfrm && !admin) return uint64_t(-1);
    if (address) {
        std::array<uint32_t, 3> destination{};
        if (length != sizeof(destination)) return invalid;
        if (!machine.readInto(address, {reinterpret_cast<uint8_t*>(&destination), sizeof(destination)}))
            return uint64_t(-14);
        if (destination != std::array<uint32_t, 3>{16, 0, 0}) return uint64_t(-111);
    }
    constexpr size_t maximum = 65536;
    size_t size = 0;
    for (const auto& [_, bytes] : vectors) {
        if (bytes > maximum - size) return uint64_t(-90);
        size += bytes;
    }
    if (size < sizeof(NetlinkHeader)) return invalid;
    try {
        std::vector<uint8_t> data(size);
        size_t offset = 0;
        for (const auto& [source, bytes] : vectors) {
            if (bytes && !machine.readInto(source, std::span(data).subspan(offset, bytes))) return uint64_t(-14);
            offset += bytes;
        }
        if (!endpoint->netlink->pid)
            if (const auto error = bindNetlink(endpoint, 0, 0)) return error;

        const auto configure = [&](std::span<const uint8_t> message) -> int32_t {
            if (message.size() < netfilterHeader || message[17] ||
                (message[16] != 0 && message[16] != 2 && message[16] != 10)) return -22;
            const uint16_t group = uint16_t(message[18]) << 8 | message[19];
            uint8_t command = 0;
            std::optional<std::array<uint32_t, 2>> mode;
            for (auto attributes = message.subspan(netfilterHeader); !attributes.empty();) {
                if (attributes.size() < 4) return -22;
                const auto length = readNetlink<uint16_t>(attributes, 0);
                const auto type = readNetlink<uint16_t>(attributes, 2);
                if (length < 4 || length > attributes.size()) return -22;
                const auto value = attributes.subspan(4, length - 4);
                if (type == 1) {
                    if (value.size() != 1 || command || value[0] < 1 || value[0] > 4) return -22;
                    command = value[0];
                } else if (type == 2) {
                    if (value.size() != 6 || mode || value[4] > 2) return -22;
                    const uint32_t range = uint32_t(value[0]) << 24 | uint32_t(value[1]) << 16 |
                                           uint32_t(value[2]) << 8 | value[3];
                    mode = std::array<uint32_t, 2>{value[4], range};
                } else {
                    return -95;
                }
                attributes = attributes.subspan(std::min(netlinkAlign(length), attributes.size()));
            }
            if (command == 3 || command == 4) return mode ? -22 : 0; // Family logger selection.
            auto found = netfilterLogGroups.find(group);
            auto owner = found == netfilterLogGroups.end() ? nullptr : found->second.owner.lock();
            if (command == 1) {
                if (owner) return owner == endpoint ? -16 : -1;
                std::erase_if(netfilterLogGroups, [](const auto& item) { return item.second.owner.expired(); });
                if (netfilterLogGroups.size() >= 256) return -28;
                found = netfilterLogGroups.emplace(group, NetfilterLogGroup{endpoint}).first;
            } else {
                if (!owner) return -19;
                if (owner != endpoint) return -1;
                if (command == 2) {
                    netfilterLogGroups.erase(found);
                    return 0;
                }
                if (!mode) return -22;
            }
            if (mode) {
                const auto [kind, range] = *mode;
                found->second.mode = kind;
                found->second.copyRange = range ? std::min(range, 65531U) : 65531;
            }
            return 0;
        };
        for (auto remaining = std::span<const uint8_t>(data); remaining.size() >= sizeof(NetlinkHeader);) {
            const auto request = readNetlink<NetlinkHeader>(remaining, 0);
            if (request.length < sizeof(request) || request.length > remaining.size()) break;
            if (request.flags & netlinkRequest) {
                const auto reply = [&](uint16_t type, uint16_t flags, std::vector<uint8_t> payload) {
                    const NetlinkHeader header{uint32_t(sizeof(NetlinkHeader) + payload.size()), type, flags,
                                               request.sequence, endpoint->netlink->pid};
                    std::vector<uint8_t> packet(sizeof(header) + payload.size());
                    std::memcpy(packet.data(), &header, sizeof(header));
                    std::ranges::copy(payload, packet.begin() + sizeof(header));
                    queueNetlink(*endpoint, std::move(packet));
                };
                const auto payload = remaining.subspan(sizeof(request), request.length - sizeof(request));
                int32_t status = -95;
                if (route)
                    status = guestRoutes.process(request, payload, admin, inetLoopbackUp,
                                                 guestLoopbackIndex, guestLoopbackMtu, reply);
                // NESTVANE_NETWORK_TRACE=1 puts every route request and its answer into the Android log.
                if (static const bool trace = std::getenv("NESTVANE_NETWORK_TRACE"); trace && route && androidLogObserver)
                    androidLogObserver("native execution: netlink route type " + std::to_string(request.type) + " flags " +
                                       std::to_string(request.flags) + " family " + std::to_string(payload.empty() ? 0 : payload[0]) +
                                       " from " + current->process->executable + " (" + std::string(reinterpret_cast<const char*>(current->name.data()), strnlen(reinterpret_cast<const char*>(current->name.data()), current->name.size())) + ") -> " + std::to_string(status));
                else if (xfrm)
                    status = handleGuestXfrm(request, payload, admin, reply);
                else if (request.type == netfilterLogConfig)
                    status = configure(remaining.first(request.length));
                const bool dump = (route && GuestRoute::isDump(request.type, request.flags)) ||
                                  (xfrm && isXfrmDump(request));
                if (status || (!dump && (request.flags & netlinkAck))) {
                    const auto echoed = status ? request.length : sizeof(request);
                    std::vector<uint8_t> ack(sizeof(NetlinkHeader) + sizeof(status) + echoed);
                    const NetlinkHeader header{uint32_t(ack.size()), netlinkError, 0, request.sequence, endpoint->netlink->pid};
                    std::memcpy(ack.data(), &header, sizeof(header));
                    std::memcpy(ack.data() + sizeof(header), &status, sizeof(status));
                    std::ranges::copy(remaining.first(echoed), ack.begin() + sizeof(header) + sizeof(status));
                    queueNetlink(*endpoint, std::move(ack));
                }
            }
            remaining = remaining.subspan(std::min(netlinkAlign(request.length), remaining.size()));
        }
        return size;
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

void ProbeLinux::emitNetfilterLog(const GuestFirewall::LogRecord& record, const SocketState& sender) {
    const auto found = netfilterLogGroups.find(record.group);
    if (found == netfilterLogGroups.end()) return;
    const auto endpoint = found->second.owner.lock();
    if (!endpoint || endpoint->inbox.size() >= 256) return;
    try {
        const bool ipv6 = record.packet.front() >> 4 == 6;
        std::vector<uint8_t> packet(netfilterHeader);
        packet[16] = ipv6 ? 10 : 2;
        packet[18] = record.group >> 8;
        packet[19] = record.group & 255;
        const std::array<uint8_t, 4> link{uint8_t(ipv6 ? 0x86 : 8), uint8_t(ipv6 ? 0xdd : 0),
                                          uint8_t(record.hook), 0};
        appendNetlinkAttribute(packet, 1, link); // NFULA_PACKET_HDR
        if (record.mark) appendNetlinkAttribute(packet, 2, netlinkNetwork32(record.mark));
        const auto ingress = record.hook == GuestFirewall::input || record.hook == GuestFirewall::prerouting;
        appendNetlinkAttribute(packet, ingress ? 4 : 5, netlinkNetwork32(guestLoopbackIndex));
        if (!record.prefix.empty())
            appendNetlinkAttribute(packet, 10, {reinterpret_cast<const uint8_t*>(record.prefix.data()),
                                                record.prefix.size() + 1});
        appendNetlinkAttribute(packet, 11, netlinkNetwork32(sender.credentials[1]));
        appendNetlinkAttribute(packet, 14, netlinkNetwork32(sender.credentials[2]));
        if (found->second.mode == 2) {
            const auto limit = std::min(found->second.copyRange, record.captureLimit.value_or(UINT32_MAX));
            const auto size = std::min<size_t>(record.packet.size(), limit);
            if (size) appendNetlinkAttribute(packet, 9, record.packet.first(size));
        }
        const NetlinkHeader header{uint32_t(packet.size()), netfilterLogPacket, 0, 0, 0};
        std::memcpy(packet.data(), &header, sizeof(header));
        queueNetlink(*endpoint, std::move(packet));
    } catch (const std::bad_alloc&) {
        // Logging is best effort and must not change the packet verdict.
    }
}

void ProbeLinux::emitLoopUevent(unsigned number) {
    const auto name = "loop" + std::to_string(number);
    const auto dev = "/devices/virtual/block/" + name;
    std::string packet;
    for (const auto& entry : {"add@" + dev, std::string("ACTION=add"), "DEVPATH=" + dev,
                              std::string("SUBSYSTEM=block"), std::string("MAJOR=7"),
                              "MINOR=" + std::to_string(number), "DEVNAME=" + name,
                              std::string("DEVTYPE=disk"), "SEQNUM=" + std::to_string(++ueventSequence)}) {
        packet += entry;
        packet.push_back('\0');
    }
    std::erase_if(ueventListeners, [](const auto& endpoint) { return endpoint.expired(); });
    for (const auto& listener : ueventListeners) {
        const auto endpoint = listener.lock();
        if (!endpoint || endpoint->inbox.size() >= 256) continue;
        const auto size = endpoint->socketFilter ?
            endpoint->socketFilter->accept({reinterpret_cast<const uint8_t*>(packet.data()), packet.size()}) : packet.size();
        if (size && endpoint->queuedBytes + size <= endpoint->netlink->receiveBuffer) {
            endpoint->inbox.push_back({std::vector<uint8_t>(packet.begin(), packet.begin() + size)});
            endpoint->queuedBytes += size;
        }
    }
    wakePollers();
}

bool ProbeLinux::socketCall(Machine& machine) {
    auto& x = machine.cpu.x;
    const auto fd = descriptors().find(uint32_t(x[0]));
    const auto writeAddress = [&](std::span<const uint8_t> address) -> uint64_t {
        uint32_t space = 0;
        std::vector<uint8_t> lenScratch;
        const auto* len = machine.read(x[2], sizeof(space), 4, lenScratch);
        if (!len) return uint64_t(-14);
        std::memcpy(&space, len, sizeof(space));
        if (space > INT32_MAX) return invalid;
        const auto count = std::min<size_t>(space, address.size());
        return (!count || machine.write(x[1], address.first(count))) &&
               writeGuest(machine, x[2], uint32_t(address.size())) ? 0 : uint64_t(-14);
    };
    const auto writeUnixAddress = [&](std::string_view name) {
        std::array<uint8_t, 111> address{1, 0};
        std::copy(name.begin(), name.end(), address.begin() + 2);
        // An abstract name (a zero byte first) has no terminating zero: its length is the address's.
        return writeAddress(std::span(address).first(name.empty() ? 2 : (name[0] ? 3 : 2) + name.size()));
    };
    switch (x[8]) {
        case 198: case 199: { // socket, socketpair
            if (x[8] == 198 && x[0] == 15) { // AF_KEY
                constexpr uint64_t closeOnExec = 0x80000, nonblock = 0x800, typeMask = 15;
                constexpr uint64_t capNetAdmin = 1ULL << 12;
                const std::string_view context = current->process->context;
                if (x[1] & ~(closeOnExec | nonblock | typeMask)) { x[0] = invalid; break; }
                if (!(current->process->effectiveCaps & capNetAdmin)) { x[0] = uint64_t(-1); break; }
                if (context != "u:r:system_server:s0" && context != "u:r:network_stack:s0" &&
                    context != "u:r:netd:s0") { x[0] = uint64_t(-13); break; }
                if ((x[1] & typeMask) != 3) { x[0] = uint64_t(-94); break; } // SOCK_RAW
                if (x[2] != 2) { x[0] = uint64_t(-93); break; } // PF_KEY_V2
                const auto key = freeDescriptor();
                if (key == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                // Android closes this socket for an RCU grace period after switching BPF maps.
                // Guest BPF readers run synchronously on this worker, so close is already ordered after them.
                Descriptor entry{"socket:[key]", {}, uint32_t(x[1] & (closeOnExec | nonblock)) | 2, Node::keySocket};
                descriptors().emplace(key, std::move(entry));
                x[0] = key;
                break;
            }
            const bool kernelNetlink = x[8] == 198 && x[0] == 16;
            const bool uevent = kernelNetlink && x[2] == 15; // NETLINK_KOBJECT_UEVENT
            const bool netfilter = kernelNetlink && x[2] == 12; // NETLINK_NETFILTER
            const bool route = kernelNetlink && x[2] == 0; // NETLINK_ROUTE
            const bool xfrm = kernelNetlink && x[2] == 6; // NETLINK_XFRM
            const bool inet = x[8] == 198 && (x[0] == 2 || x[0] == 10);
            const auto context = std::string_view(current->process->context);
            const bool vsock = x[8] == 198 && x[0] == addressFamilyVsock &&
                (context == sensorsHalContext || (camera && context == cameraHalContext));
            if (x[0] != 1 && !uevent && !netfilter && !route && !xfrm && !inet && !vsock) { x[0] = uint64_t(-97); break; }
            const uint32_t type = x[1] & 15;
            constexpr uint32_t flags = 0x80000 | 0x800;
            if (vsock && (type != 1 || x[2])) { x[0] = uint64_t(-94); break; }
            const bool firewallSocket = inet && type == 3 && x[2] == 255;
            if (inet && !firewallSocket && (type != 1 && type != 2)) { x[0] = uint64_t(-94); break; }
            if (firewallSocket && !(current->process->effectiveCaps & (1ULL << 13))) {
                x[0] = uint64_t(-1); break; // CAP_NET_RAW
            }
            // A datagram socket of ICMP is an echo socket: what ping uses when it has no privileges.
            const bool echoSocket = inet && x[0] == 2 && type == 2 && x[2] == 1;
            const uint32_t inetProtocol = firewallSocket ? 255 : type == 1 ? 6 : echoSocket ? 1 : 17;
            if (inet && x[2] && x[2] != inetProtocol) { x[0] = uint64_t(-93); break; }
            if (x[1] & ~(uint64_t(flags) | 15) ||
                (!inet && (uevent ? type != 2 : kernelNetlink ? (type != 2 && type != 3) :
                           (type != 1 && type != 2 && type != 5) || x[2]))) {
                x[0] = uint64_t(-22);
                break;
            }
            const auto first = freeDescriptor();
            if (first == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            if (vsock) {
                Descriptor entry{"socket:[vsock]", {}, uint32_t(x[1] & flags) | 2, Node::graphicsPipe};
                entry.graphics = std::make_shared<GraphicsPipeState>();
                entry.graphics->vsock = true;
                entry.graphics->serviceContext = context == sensorsHalContext ? sensorsHalContext : cameraHalContext;
                descriptors().emplace(first, std::move(entry));
                x[0] = first;
                break;
            }
            auto endpoint = std::make_shared<SocketState>(type);
            endpoint->context = current->socketCreateContext.empty()
                ? current->process->context : current->socketCreateContext;
            endpoint->credentials = {uint32_t(processId()), userId(), groupId()};
            Descriptor entry{inet ? "socket:[inet]" : kernelNetlink ? "socket:[netlink]" : "socket:[unix]",
                             {}, uint32_t(x[1] & flags) | 2, Node::socket};
            if (kernelNetlink) endpoint->netlink.emplace();
            endpoint->family = x[0];
            endpoint->protocol = inet ? inetProtocol : x[2];
            endpoint->cookie = nextSocketCookie++;
            if (inet && !openInetSocket(endpoint)) { x[0] = uint64_t(-1); break; }
            entry.socket = endpoint;
            descriptors().emplace(first, entry);
            if (x[8] == 198) { x[0] = first; break; }
            const auto second = freeDescriptor();
            if (second == descriptorCapacity) { descriptors().erase(first); x[0] = uint64_t(-24); break; }
            const std::array<uint32_t, 2> pair{first, second};
            if (!writeGuest(machine, x[3], pair)) { descriptors().erase(first); x[0] = uint64_t(-14); break; }
            entry.socket = std::make_shared<SocketState>(type);
            entry.socket->cookie = nextSocketCookie++;
            entry.socket->peer = endpoint;
            endpoint->peer = entry.socket;
            entry.socket->connected = endpoint->connected = true;
            entry.socket->credentials = endpoint->credentials;
            entry.socket->context = endpoint->context;
            entry.socket->peerCredentials = endpoint->peerCredentials = endpoint->credentials;
            entry.socket->peerContext = endpoint->peerContext = endpoint->context;
            descriptors().emplace(second, std::move(entry));
            x[0] = 0;
            break;
        }
        case 200: case 203: { // bind, connect
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (auto pipe = fd->second.graphics; pipe && pipe->vsock) {
                if (current->process->context != pipe->serviceContext) { x[0] = uint64_t(-13); break; }
                if (x[8] != 203) { x[0] = uint64_t(-95); break; }
                if (pipe->connected) { x[0] = uint64_t(-106); break; }
                // Linux sockaddr_vm is 16 bytes and stores its port and CID in native byte order.
                struct Address {
                    uint16_t family, reserved;
                    uint32_t port, cid;
                    std::array<uint8_t, 4> flagsAndPadding;
                } address{};
                if (x[2] != sizeof(address)) { x[0] = invalid; break; }
                if (!machine.readInto(x[1], {reinterpret_cast<uint8_t*>(&address), sizeof(address)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                if (address.family != addressFamilyVsock) { x[0] = uint64_t(-97); break; }
                if (address.reserved || !std::ranges::all_of(address.flagsAndPadding, [](auto byte) { return !byte; })) {
                    x[0] = invalid;
                    break;
                }
                if (address.cid != emulatorHostCid || address.port != emulatorDataPort) {
                    x[0] = uint64_t(-111);
                    break;
                }
                pipe->connected = true;
                wakePollers();
                x[0] = 0;
                break;
            }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            if (fd->second.socket->firewallSocket()) { x[0] = uint64_t(-95); break; }
            if (fd->second.socket->family == 2 || fd->second.socket->family == 10) {
                const auto status = x[8] == 200 ? bindInetSocket(machine, fd->second.socket) :
                    connectInetSocket(machine, fd->second.socket, fd->second.effectiveFlags() & 0x800);
                if (status == uint64_t(-11) && !(fd->second.effectiveFlags() & 0x800) &&
                    waitForSocketTimeout(fd->second.socket->sendTimeout)) return true;
                current->deadline.reset();
                x[0] = status;
                break;
            }
            if (fd->second.socket->netlink) {
                std::array<uint32_t, 3> address{};
                if (x[2] != sizeof(address)) { x[0] = invalid; break; }
                if (!machine.readInto(x[1], {reinterpret_cast<uint8_t*>(&address), sizeof(address)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                if (address[0] != 16) { x[0] = invalid; break; }
                if (x[8] == 200) {
                    x[0] = bindNetlink(fd->second.socket, address[1], address[2]);
                } else if (fd->second.socket->protocol != 15 && !address[1] && !address[2]) {
                    if (!fd->second.socket->netlink->pid && (x[0] = bindNetlink(fd->second.socket, 0, 0))) break;
                    fd->second.socket->connected = true;
                    x[0] = 0;
                } else {
                    x[0] = uint64_t(-95);
                }
                break;
            }
            if (x[2] < 3 || x[2] > 110) { x[0] = invalid; break; }
            std::vector<uint8_t> bytesScratch;
            const auto* bytes = machine.read(x[1], x[2], 4, bytesScratch);
            if (!bytes) { x[0] = uint64_t(-14); break; }
            if (bytes[0] != 1 || bytes[1]) { x[0] = uint64_t(-97); break; }
            // An abstract name starts with a zero byte and is all the bytes of the address, with no file
            // behind it. It is kept with the paths, which it cannot be mistaken for, and is free again
            // once its socket is closed. WebView's zygote listens on one.
            const bool abstract = !bytes[2];
            const auto* nul = abstract ? nullptr : static_cast<const uint8_t*>(std::memchr(bytes + 2, 0, x[2] - 2));
            const size_t length = nul ? nul - bytes - 2 : x[2] - 2;
            const std::string_view name(reinterpret_cast<const char*>(bytes + 2), length);
            const auto path = abstract ? std::string(name) : absolutePath(name);
            if (x[8] == 200 && abstract) {
                if (!fd->second.socket->localAddress.empty()) { x[0] = invalid; break; }
                if (const auto taken = socketPaths.find(path); taken != socketPaths.end()) {
                    if (!taken->second.endpoint.expired()) { x[0] = uint64_t(-98); break; }
                    socketPaths.erase(taken);
                }
                std::erase_if(socketPaths, [](const auto& entry) {
                    return entry.first.starts_with('\0') && entry.second.endpoint.expired();
                });
                if (socketPaths.size() >= 256) { x[0] = uint64_t(-28); break; }
                socketPaths.emplace(path, BoundSocket{fd->second.socket, 0777, userId(), groupId()});
                fd->second.socket->localAddress = name;
                x[0] = 0;
            } else if (x[8] == 200) {
                if (!fd->second.socket->localAddress.empty()) { x[0] = invalid; break; }
                if (!directoryExists(path.substr(0, path.rfind('/')))) { x[0] = uint64_t(-2); break; }
                if (!writablePath(path)) { x[0] = uint64_t(-30); break; }
                if (socketPaths.contains(path) || find(path, 0)) { x[0] = uint64_t(-98); break; }
                if (socketPaths.size() >= 256) { x[0] = uint64_t(-28); break; }
                socketPaths.emplace(path, BoundSocket{fd->second.socket, 0777, userId(), groupId()});
                fd->second.socket->localAddress = name;
                x[0] = 0;
            } else {
                if (captureAndroidLogs && path == "/dev/socket/logdw" && fd->second.socket->type == 2) {
                    fd->second.socket->connected = true;
                    fd->second.socket->logWriter = true;
                    fd->second.socket->peerAddress = name;
                    x[0] = 0;
                    break;
                }
                if (javaDumpsToLog && path == "/dev/socket/tombstoned_java_trace") { x[0] = uint64_t(-111); break; }
                const auto bound = socketPaths.find(path);
                if (bound == socketPaths.end()) { x[0] = abstract ? uint64_t(-111) : uint64_t(-2); break; }
                auto endpoint = bound->second.endpoint.lock();
                if (!endpoint) { x[0] = uint64_t(-111); break; }
                if (endpoint->type != fd->second.socket->type) { x[0] = uint64_t(-91); break; }
                auto client = fd->second.socket;
                if (client->connected) { x[0] = uint64_t(-106); break; }
                if (endpoint->type == 2) {
                    client->peer = endpoint;
                } else {
                    if (!endpoint->listening) { x[0] = uint64_t(-111); break; }
                    if (endpoint->pending.size() >= endpoint->backlog) {
                        if (!(fd->second.effectiveFlags() & 0x800) &&
                            waitForSocketTimeout(client->sendTimeout)) return true;
                        x[0] = uint64_t(-11);
                        break;
                    }
                    auto server = std::make_shared<SocketState>(client->type);
                    if (client->type == 5) server->passCredentials = endpoint->passCredentials;
                    server->connected = true;
                    server->credentials = endpoint->credentials;
                    server->context = endpoint->context;
                    server->peerCredentials = {uint32_t(processId()), userId(), groupId()};
                    server->peerContext = client->context;
                    server->localAddress = endpoint->localAddress;
                    server->peerAddress = client->localAddress;
                    server->peer = client;
                    client->peer = server;
                    endpoint->pending.push_back(std::move(server));
                    ++endpoint->pollNotifications.readable;
                }
                client->peerCredentials = endpoint->credentials;
                client->peerContext = endpoint->context;
                client->peerAddress = endpoint->localAddress;
                client->connected = true;
                wakePollers();
                x[0] = 0;
            }
            break;
        }
        case 201: { // listen
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            if (fd->second.socket->type == 2) { x[0] = uint64_t(-95); break; }
            auto& socket = fd->second.socket;
            if (socket->inetStream()) {
                if (socket->connected) { x[0] = invalid; break; }
                if (!socket->localInetAddress[0]) {
                    SocketState::InetAddress local{};
                    local[0] = socket->family;
                    if (const auto error = reserveInetAddress(socket, local)) { x[0] = error; break; }
                }
            } else if (!std::any_of(socketPaths.begin(), socketPaths.end(), [&](const auto& entry) {
                    return entry.second.endpoint.lock() == socket;
                })) { x[0] = invalid; break; }
            fd->second.socket->listening = true;
            fd->second.socket->credentials = {uint32_t(processId()), userId(), groupId()};
            fd->second.socket->backlog = std::clamp<int32_t>(int32_t(x[1]), 1, 128);
            x[0] = 0;
            break;
        }
        case 202: case 242: { // accept, accept4
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            const auto endpoint = fd->second.socket;
            if (!endpoint) { x[0] = uint64_t(-88); break; }
            const uint64_t flags = x[8] == 242 ? x[3] : 0;
            if (!endpoint->listening || (flags & ~0x80800ULL)) { x[0] = invalid; break; }
            if (endpoint->pending.empty()) {
                if (!(fd->second.effectiveFlags() & 0x800) && waitForSocketTimeout(endpoint->receiveTimeout)) return true;
                current->deadline.reset();
                x[0] = uint64_t(-11);
                break;
            }
            current->deadline.reset();
            const auto number = freeDescriptor();
            if (number == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            if (x[1]) {
                const auto& peer = *endpoint->pending.front();
                const auto result = peer.inetStream() ?
                    writeAddress(std::span(peer.peerInetAddress).first(peer.family == 2 ? 16 : 28)) :
                    writeUnixAddress(peer.peerAddress);
                if (result) { x[0] = result; break; }
            }
            Descriptor entry{endpoint->inetStream() ? "socket:[inet]" : "socket:[unix]", {},
                             uint32_t(flags) | 2, Node::socket};
            entry.socket = std::move(endpoint->pending.front());
            endpoint->pending.pop_front();
            descriptors().emplace(number, std::move(entry));
            wakePollers();
            x[0] = number;
            break;
        }
        case 204: case 205: { // getsockname, getpeername
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            const auto& endpoint = *fd->second.socket;
            if (x[8] == 205 && !endpoint.connected) { x[0] = uint64_t(-107); break; }
            if (endpoint.netlink) {
                const auto address = x[8] == 205 ? std::array<uint32_t, 3>{16, 0, 0} :
                    std::array<uint32_t, 3>{16, endpoint.netlink->pid, endpoint.netlink->groups};
                x[0] = writeAddress({reinterpret_cast<const uint8_t*>(address.data()), sizeof(address)});
            } else if (endpoint.family == 2 || endpoint.family == 10) {
                auto address = x[8] == 204 ? endpoint.localInetAddress : endpoint.peerInetAddress;
                address[0] = endpoint.family;
                x[0] = writeAddress(std::span(address).first(endpoint.family == 2 ? 16 : 28));
            } else {
                x[0] = writeUnixAddress(x[8] == 204 ? endpoint.localAddress : endpoint.peerAddress);
            }
            break;
        }
        case 206: case 207: { // sendto/recvfrom on a connected UNIX socket
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            const bool send = x[8] == 206;
            const auto& socket = fd->second.socket;
            if (send && socket->netlink && socket->protocol != 15) {
                if (x[3] & ~0x4040ULL) { x[0] = invalid; break; }
                const SocketVector vector{x[1], x[2]};
                x[0] = sendKernelNetlink(machine, socket, std::span(&vector, 1), x[4], x[5]);
                break;
            }
            if (!send && socket->netlink) {
                const auto result = receiveKernelNetlinkFrom(machine, *socket);
                if (result == uint64_t(-11) && !(x[3] & 0x40) && !(fd->second.effectiveFlags() & 0x800) &&
                    waitForSocketTimeout(socket->receiveTimeout)) return true;
                current->deadline.reset();
                x[0] = result;
                break;
            }
            if (socket->inetDatagram()) {
                if (x[3] & ~(send ? 0x4040ULL : 0x162ULL)) { x[0] = invalid; break; }
                const bool blocking = !(x[3] & 0x40) && !(fd->second.effectiveFlags() & 0x800);
                const SocketVector vector{x[1], x[2]};
                const auto result = send ? sendInetDatagram(machine, socket, std::span(&vector, 1), x[4], x[5])
                    : receiveInetDatagram(machine, *socket, x[1], x[2], x[3], x[4], x[5]);
                if (!send && result == uint64_t(-11) && blocking && waitForSocketTimeout(socket->receiveTimeout))
                    return true;
                current->deadline.reset();
                x[0] = result;
                break;
            }
            constexpr uint64_t truncate = 0x20; // MSG_TRUNC, ignored for UNIX streams.
            // MSG_PEEK on a TCP connection: Chromium looks at one byte to learn whether the connection is
            // still open before it uses it again.
            constexpr uint64_t peek = 0x2;
            const bool hostStream = socket->hostFd >= 0 && !socket->inetDatagram();
            const auto receiveFlags = 0x140ULL | (socket->family == 1 ? truncate : 0) | (hostStream ? peek : 0);
            if (x[4] || x[5] || (x[3] & ~(send ? 0x4040ULL : receiveFlags))) return false;
            const auto& endpoint = *socket;
            const auto peer = endpoint.peer.lock();
            const bool blocking = !(x[3] & 0x40) && !(fd->second.effectiveFlags() & 0x800);
            const bool waitAll = !send && (x[3] & 0x100) && blocking &&
                endpoint.type == 1 && x[2] > endpoint.queuedBytes && peer &&
                !peer->sendShutdown && !endpoint.receiveShutdown;
            if (waitAll && waitForSocketTimeout(endpoint.receiveTimeout)) return true;
            const auto result = send ? socketWrite(machine, fd->second, x[1], x[2])
                                     : socketRead(machine, fd->second, x[1], x[2], x[3] & truncate, x[3] & peek);
            if (result == uint64_t(-11) && blocking &&
                (!waitAll && waitForSocketTimeout(send ? endpoint.sendTimeout : endpoint.receiveTimeout))) return true;
            current->deadline.reset();
            x[0] = result;
            break;
        }
        case 210: { // shutdown, including AF_UNIX half-close.
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            const auto endpoint = fd->second.socket;
            if (!endpoint) { x[0] = uint64_t(-88); break; }
            if (x[1] > 2) { x[0] = invalid; break; }
            if (!endpoint->connected) { x[0] = uint64_t(-107); break; }
            if (x[1] != 1) endpoint->receiveShutdown = true;
            if (x[1] != 0) endpoint->sendShutdown = true;
            if (endpoint->hostFd >= 0) ::shutdown(endpoint->hostFd, int(x[1]));
            if (endpoint->family == 1) {
                ++endpoint->pollNotifications.stateChange;
                if (endpoint->type != 2) {
                    if (const auto peer = endpoint->peer.lock()) ++peer->pollNotifications.stateChange;
                }
            }
            wakePollers();
            x[0] = 0;
            break;
        }
        case 211: { // sendmsg on a connected UNIX socket.
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            const auto& endpoint = fd->second.socket;
            if (!endpoint) { x[0] = uint64_t(-88); break; }
            if (endpoint->protocol == 15 || (x[2] & ~0x40c0ULL)) return false; // DONTWAIT, EOR, NOSIGNAL
            SocketMessage message{};
            std::vector<SocketVector> vectors;
            if (const auto error = readSocketMessage(machine, x[1], message, vectors)) {
                x[0] = error;
                break;
            }
            SocketRights rights;
            if (message.controlLength) {
                if (endpoint->family != 1) { x[0] = uint64_t(-95); break; }
                try {
                    if (const auto error = readSocketRights(machine, message.control, message.controlLength, rights)) {
                        x[0] = error;
                        break;
                    }
                } catch (const std::bad_alloc&) {
                    x[0] = noMemory;
                    break;
                }
                // Android's LocalSocket emits SCM_RIGHTS even when it has no files to pass.
                if (!rights.empty() && endpoint->type != 1 && endpoint->type != 5) { x[0] = uint64_t(-95); break; }
            }
            if (endpoint->netlink) {
                x[0] = sendKernelNetlink(machine, endpoint, vectors, message.name, message.nameLength);
                break;
            }
            if (message.nameLength && !endpoint->inetDatagram()) return false;
            const auto result = endpoint->inetDatagram() ?
                sendInetDatagram(machine, endpoint, vectors, message.name, message.nameLength) :
                socketWrite(machine, fd->second, vectors, std::move(rights));
            const bool blocking = !(x[2] & 0x40) && !(fd->second.effectiveFlags() & 0x800);
            if (result == uint64_t(-11) && blocking && waitForSocketTimeout(endpoint->sendTimeout)) return true;
            current->deadline.reset();
            x[0] = result;
            break;
        }
        case 212: { // recvmsg on the guest kernel multicast channel or a connected packet socket.
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            auto& endpoint = *fd->second.socket;
            if (endpoint.netlink) {
                if (endpoint.inbox.empty() && !(fd->second.effectiveFlags() & 0x800) && !(x[2] & 0x40) &&
                    waitForSocketTimeout(endpoint.receiveTimeout)) return true;
                current->deadline.reset();
                x[0] = receiveKernelNetlink(machine, endpoint);
                break;
            }
            if (endpoint.family == 1 && endpoint.type == 1) {
                uint64_t result;
                try {
                    result = receiveUnixStreamMessage(machine, endpoint);
                } catch (const std::bad_alloc&) {
                    result = noMemory;
                }
                if (result == uint64_t(-11) && !(fd->second.effectiveFlags() & 0x800) && !(x[2] & 0x40) &&
                    waitForSocketTimeout(endpoint.receiveTimeout)) return true;
                current->deadline.reset();
                x[0] = result;
                break;
            }
            constexpr uint64_t peek = 0x2, trunc = 0x20, dontWait = 0x40, cloexec = 0x40000000;
            const bool unixCredentials = endpoint.family == 1 && endpoint.passCredentials;
            constexpr uint64_t ignoredUnixReceiveFlags = 0x8 | 0x4000; // MSG_CTRUNC and MSG_NOSIGNAL
            const auto ignored = endpoint.family == 1 ? ignoredUnixReceiveFlags : 0;
            if (endpoint.type == 1 || (endpoint.passCredentials && !unixCredentials) ||
                (x[2] & ~(peek | trunc | dontWait | cloexec | ignored))) return false;
            SocketMessage message{};
            std::vector<SocketVector> vectors;
            if (const auto error = readSocketMessage(machine, x[1], message, vectors)) {
                x[0] = error;
                break;
            }
            if (endpoint.inetError) {
                current->deadline.reset();
                x[0] = uint64_t(-std::exchange(endpoint.inetError, 0));
                break;
            }
            // A datagram socket that the host carries: what the host has for it comes first.
            if (endpoint.inetDatagram() && endpoint.hostFd >= 0 && endpoint.datagrams.empty()) {
                if (const auto pulled = pullHostDatagram(endpoint); pulled && *pulled != uint64_t(-11)) {
                    current->deadline.reset();
                    x[0] = *pulled;
                    break;
                }
                if (endpoint.datagrams.empty() && !endpoint.receiveShutdown &&
                    !(fd->second.effectiveFlags() & 0x800) && !(x[2] & dontWait)) return waitForHostEvents();
            }
            if (!endpoint.hasInput()) {
                const auto peer = endpoint.peer.lock();
                if (endpoint.receiveShutdown || (endpoint.type == 5 && endpoint.connected &&
                    (!peer || peer->sendShutdown))) {
                    message.nameLength = 0;
                    message.controlLength = 0;
                    message.flags = endpoint.family == 1 ? uint32_t(x[2] & cloexec) : 0;
                    x[0] = writeGuest(machine, x[1], message) ? 0 : uint64_t(-14);
                } else {
                    if (!(fd->second.effectiveFlags() & 0x800) && !(x[2] & dontWait) &&
                        waitForSocketTimeout(endpoint.receiveTimeout)) return true;
                    x[0] = uint64_t(-11);
                }
                current->deadline.reset();
                break;
            }
            current->deadline.reset();
            const auto& packet = endpoint.inetDatagram() ? endpoint.datagrams.front().payload : endpoint.inbox.front().payload;
            auto remaining = std::span(packet);
            bool copied = true;
            for (const auto& [address, size] : vectors) {
                if (remaining.empty()) break;
                const auto count = std::min<uint64_t>(size, remaining.size());
                if (count && !machine.write(address, remaining.first(count))) { copied = false; break; }
                remaining = remaining.subspan(count);
            }
            if (copied && message.name && endpoint.inetDatagram()) {
                const auto length = uint32_t(endpoint.family == 2 ? 16 : 28);
                const auto count = std::min(message.nameLength, length);
                auto source = endpoint.datagrams.front().source;
                filterInetAddress(endpoint, endpoint.family == 2 ? BpfProgram::receive4 : BpfProgram::receive6, source);
                copied = !count || machine.write(message.name, std::span(source).first(count));
                message.nameLength = length;
            } else if (copied && message.name && !endpoint.peerAddress.empty()) {
                std::array<uint8_t, 111> address{1, 0};
                std::ranges::copy(endpoint.peerAddress, address.begin() + 2);
                const auto length = uint32_t(endpoint.peerAddress.size() + 3);
                const auto count = std::min(message.nameLength, length);
                copied = !count || machine.write(message.name, std::span(address).first(count));
                message.nameLength = length;
            } else {
                message.nameLength = 0;
            }
            message.flags = (remaining.empty() ? 0 : trunc) | (endpoint.family == 1 ? uint32_t(x[2] & cloexec) : 0);
            const auto controlCapacity = message.control ? message.controlLength : 0;
            message.controlLength = 0;
            if (copied && unixCredentials) {
                const std::array<uint32_t, 3> absent{0, 65534, 65534};
                const auto sender = endpoint.inbox.front().credentials.value_or(absent);
                constexpr auto length = sizeof(SocketControl) + sizeof(sender);
                if (controlCapacity < length) message.flags |= 0x8; // MSG_CTRUNC
                if (controlCapacity >= sizeof(SocketControl)) {
                    const auto used = std::min<uint64_t>(controlCapacity, length);
                    const SocketControl control{used, socketControlLevel, 2}; // SCM_CREDENTIALS
                    const auto payload = std::span(reinterpret_cast<const uint8_t*>(sender.data()), sizeof(sender));
                    copied = message.control <= UINT64_MAX - used && writeGuest(machine, message.control, control) &&
                        machine.write(message.control + sizeof(control), payload.first(used - sizeof(control)));
                    message.controlLength = std::min<uint64_t>(controlCapacity, socketControlAlign(length));
                }
            }
            std::map<uint32_t, Descriptor> transferred;
            if (copied && endpoint.family == 1 && !endpoint.inbox.front().files.empty()) {
                const auto& rights = endpoint.inbox.front().files;
                uint64_t used = 0;
                try {
                    copied = message.control <= UINT64_MAX - message.controlLength &&
                        writeSocketRights(machine, rights, message.control + message.controlLength,
                                          controlCapacity - message.controlLength, x[2] & cloexec,
                                          used, message.flags, transferred);
                } catch (const std::bad_alloc&) {
                    x[0] = noMemory;
                    break;
                }
                message.controlLength += used;
            }
            if (!copied || !writeGuest(machine, x[1], message)) { x[0] = uint64_t(-14); break; }
            descriptors().merge(transferred);
            x[0] = (x[2] & trunc) ? packet.size() : packet.size() - remaining.size();
            if (!(x[2] & peek)) {
                const auto queuedBefore = endpoint.queuedBytes;
                const auto packetsBefore = endpoint.inetDatagram() ? endpoint.datagrams.size() : endpoint.inbox.size();
                endpoint.queuedBytes -= packet.size();
                if (endpoint.inetDatagram()) endpoint.datagrams.pop_front();
                else endpoint.inbox.pop_front();
                if (endpoint.inetDatagram()) wakePollers();
                else socketDrained(endpoint, queuedBefore, packetsBefore);
            }
            break;
        }
        case 209: { // getsockopt on AF_UNIX.
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            const auto& endpoint = *fd->second.socket;
            if (endpoint.firewallSocket() && x[1] == (endpoint.family == 10 ? 41 : 0)) {
                if (!(current->process->effectiveCaps & (1ULL << 12))) { x[0] = uint64_t(-1); break; }
                try {
                    x[0] = firewall.get(machine, endpoint.family == 10);
                } catch (const std::bad_alloc&) {
                    x[0] = noMemory;
                }
                break;
            }
            if (fd->second.socket->storageIdentity) {
                x[0] = getInetSocketOption(machine, *fd->second.socket);
                break;
            }
            if (x[1] != 1) return false;
            uint32_t size = 0;
            std::vector<uint8_t> pointerScratch;
            const auto* pointer = machine.read(x[4], sizeof(size), 4, pointerScratch);
            if (!pointer) { x[0] = uint64_t(-14); break; }
            std::memcpy(&size, pointer, sizeof(size));
            const bool sendTimeout = x[2] == sendTimeoutOld || x[2] == sendTimeoutNew;
            if (sendTimeout || x[2] == receiveTimeoutOld || x[2] == receiveTimeoutNew) {
                const auto delay = sendTimeout ? endpoint.sendTimeout : endpoint.receiveTimeout;
                const auto usec = std::max<int64_t>(0, delay.count());
                const std::array<int64_t, 2> timeout{usec / 1'000'000, usec % 1'000'000};
                size = std::min<uint32_t>(size, sizeof timeout);
                x[0] = (!size || machine.write(x[3], {reinterpret_cast<const uint8_t*>(timeout.data()), size})) &&
                    writeGuest(machine, x[4], size) ? 0 : uint64_t(-14);
                break;
            }
            std::span<const uint8_t> bytes;
            if (x[2] == 13) { // SO_LINGER
                if (size > INT32_MAX) { x[0] = invalid; break; }
                size = std::min<uint32_t>(size, sizeof(endpoint.linger));
                const auto* setting = reinterpret_cast<const uint8_t*>(endpoint.linger.data());
                x[0] = (!size || machine.write(x[3], {setting, size})) && writeGuest(machine, x[4], size)
                    ? 0 : uint64_t(-14);
                break;
            }
            if (x[2] == 2 || x[2] == 3 || x[2] == 7 || x[2] == 8 || x[2] == 16 || x[2] == 38 || x[2] == 39) {
                uint32_t value = 0;
                switch (x[2]) {
                    case 2: value = endpoint.reuseAddress; break; // SO_REUSEADDR
                    case 3: value = endpoint.type; break; // SO_TYPE
                    case 7: value = endpoint.sendBuffer; break; // SO_SNDBUF
                    case 8: value = endpoint.netlink ? endpoint.netlink->receiveBuffer : endpoint.receiveBuffer; break;
                    case 16: value = endpoint.passCredentials; break; // SO_PASSCRED
                    case 38: value = endpoint.protocol; break; // SO_PROTOCOL
                    case 39: value = endpoint.family; break; // SO_DOMAIN
                }
                size = std::min<uint32_t>(size, sizeof(value));
                x[0] = (!size || machine.write(x[3], {reinterpret_cast<const uint8_t*>(&value), size})) &&
                       writeGuest(machine, x[4], size) ? 0 : uint64_t(-14);
                break;
            }
            if (x[2] == 57) { // SO_COOKIE
                if (size < sizeof(endpoint.cookie)) { x[0] = invalid; break; }
                const uint64_t cookie = fd->second.socket->cookie
                    ? fd->second.socket->cookie : (fd->second.socket->cookie = nextSocketCookie++);
                x[0] = writeGuest(machine, x[3], cookie) && writeGuest(machine, x[4], uint32_t(sizeof(cookie))) ? 0 : uint64_t(-14);
                break;
            }
            if (x[2] != 17 && x[2] != 31) return false; // SO_PEERCRED or SO_PEERSEC
            if (x[2] == 31) {
                if (endpoint.peerContext.empty()) { x[0] = uint64_t(-92); break; }
                const uint32_t required = endpoint.peerContext.size() + 1;
                if (size < required) {
                    x[0] = writeGuest(machine, x[4], required) ? uint64_t(-34) : uint64_t(-14);
                    break;
                }
                size = required;
                bytes = {reinterpret_cast<const uint8_t*>(endpoint.peerContext.c_str()), size};
            } else {
                size = std::min<uint32_t>(size, sizeof(endpoint.peerCredentials));
                bytes = {reinterpret_cast<const uint8_t*>(endpoint.peerCredentials.data()), size};
            }
            x[0] = ((!size || machine.write(x[3], bytes)) && writeGuest(machine, x[4], size))
                ? 0 : uint64_t(-14);
            break;
        }
        case 208: { // SOL_SOCKET options
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            auto& endpoint = *fd->second.socket;
            if (endpoint.firewallSocket() && x[1] == (endpoint.family == 10 ? 41 : 0)) {
                if (!(current->process->effectiveCaps & (1ULL << 12))) { x[0] = uint64_t(-1); break; }
                try {
                    x[0] = firewall.set(machine, endpoint.family == 10, [&](auto path, auto fd, auto& program) {
                        return resolveFirewallBpf(path, fd, program);
                    });
                } catch (const std::bad_alloc&) {
                    x[0] = noMemory;
                }
                break;
            }
            int32_t level = x[1], name = x[2];
            uint32_t size = x[4];
            std::vector<uint8_t> buffer;
            if (endpoint.storageIdentity) {
                if (const auto result = setInetSocketOption(machine, endpoint, level, name, size, buffer)) {
                    x[0] = *result;
                    break;
                }
            }
            const auto readOption = [&](std::span<uint8_t> output) {
                if (buffer.empty()) return machine.readInto(x[3], output);
                if (output.size() > buffer.size()) return false;
                std::ranges::copy(std::span(buffer).first(output.size()), output.begin());
                return true;
            };
            if (level == 41 && name == 26 && endpoint.family == 10) { // IPV6_V6ONLY
                int32_t value = 0;
                if (size < sizeof(value) || endpoint.localInetAddress[0]) { x[0] = invalid; break; }
                if (!readOption({reinterpret_cast<uint8_t*>(&value), sizeof(value)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                endpoint.inetV6Only = value != 0;
                x[0] = 0;
                break;
            }
            if (level == 270 && endpoint.netlink) { // SOL_NETLINK
                // Joining and leaving a group by its number, and options about how errors are
                // reported, which change nothing in what this layer sends.
                if (name == 1 || name == 2) {
                    uint32_t group = 0;
                    if (x[4] < 4 || !machine.readInto(x[3], {reinterpret_cast<uint8_t*>(&group), sizeof(group)})) { x[0] = uint64_t(-14); break; }
                    if (!group || group > 32) { x[0] = invalid; break; }
                    if (name == 1) endpoint.netlink->groups |= 1u << (group - 1);
                    else endpoint.netlink->groups &= ~(1u << (group - 1));
                }
                x[0] = 0;
                break;
            }
            if (level != 1) {
                if (!endpoint.storageIdentity) return false;
                // Options of TCP and IP: with the network there, programs set them as a matter of
                // course (no delay, keep-alive times, the type of service). The host's own socket keeps
                // its defaults, but for the one that changes what the program sees.
                if (hostNetwork && (level == 6 || level == 0 || level == 41)) {
                    if (level == 6 && name == 1 && endpoint.hostFd >= 0 && x[4] >= 4) {
                        int value = 0;
                        if (machine.readInto(x[3], {reinterpret_cast<uint8_t*>(&value), sizeof(value)}))
                            ::setsockopt(endpoint.hostFd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
                    }
                    x[0] = 0;
                    break;
                }
                x[0] = uint64_t(-92);
                break;
            }
            if (name == 13) { // SO_LINGER
                std::array<int32_t, 2> setting{};
                if (size < sizeof(setting)) { x[0] = invalid; break; }
                if (!readOption({reinterpret_cast<uint8_t*>(setting.data()), sizeof(setting)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                endpoint.linger[0] = setting[0] != 0;
                if (setting[0]) endpoint.linger[1] = setting[1];
                x[0] = 0;
                break;
            }
            if (name == 36 && endpoint.storageIdentity) { // SO_MARK
                constexpr uint64_t capNet = (1ULL << 12) | (1ULL << 13);
                if (!(current->process->effectiveCaps & capNet)) { x[0] = uint64_t(-1); break; }
                uint32_t mark = 0;
                if (size < sizeof(mark)) { x[0] = invalid; break; }
                if (!readOption({reinterpret_cast<uint8_t*>(&mark), sizeof(mark)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                endpoint.socketMark = mark;
                x[0] = 0;
                break;
            }
            const bool sendTimeout = name == sendTimeoutOld || name == sendTimeoutNew;
            if (sendTimeout || name == receiveTimeoutOld || name == receiveTimeoutNew) {
                std::array<int64_t, 2> timeout{};
                if (size < sizeof timeout) { x[0] = invalid; break; }
                auto bytes = std::span(reinterpret_cast<uint8_t*>(timeout.data()), sizeof timeout);
                if (!readOption(bytes)) { x[0] = uint64_t(-14); break; }
                const auto [sec, usec] = timeout;
                if (usec < 0 || usec >= 1'000'000) { x[0] = uint64_t(-33); break; }
                constexpr auto largest = std::chrono::microseconds::max().count();
                const auto delay = sec < 0 ? -1 : sec > (largest - usec) / 1'000'000 ?
                    largest : sec * 1'000'000 + usec;
                (sendTimeout ? endpoint.sendTimeout : endpoint.receiveTimeout) = std::chrono::microseconds(delay);
                x[0] = 0;
                break;
            }
            if (x[2] == 50 && endpoint.protocol == 15) { // SO_ATTACH_BPF
                uint32_t programFd = 0;
                if (x[4] != sizeof(programFd)) { x[0] = invalid; break; }
                if (!machine.readInto(x[3], {reinterpret_cast<uint8_t*>(&programFd), sizeof(programFd)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                const auto program = descriptors().find(programFd);
                if (program == descriptors().end()) { x[0] = uint64_t(-9); break; }
                if (!program->second.bpfProgram || program->second.bpfProgram->type() != BpfProgram::socketFilter) {
                    x[0] = invalid;
                    break;
                }
                endpoint.socketFilter = program->second.bpfProgram;
                x[0] = 0;
                break;
            }
            const bool timestamp = name == 29 && endpoint.netlink;
            // SO_BROADCAST: Java's DatagramSocket sets it on every socket it makes (Jellyfin looks for
            // servers that way). Nothing here is broadcast, so it changes nothing.
            if (name == 6 && endpoint.storageIdentity) {
                uint32_t enabled = 0;
                x[0] = size < 4 ? invalid : readOption({reinterpret_cast<uint8_t*>(&enabled), sizeof(enabled)}) ? 0 : uint64_t(-14);
                break;
            }
            if (name != 2 && name != 16 && name != 7 && name != 8 && (name != 33 || !endpoint.netlink) && !timestamp) {
                if (!endpoint.storageIdentity) return false;
                x[0] = uint64_t(-92);
                break;
            }
            if (size < 4) { x[0] = invalid; break; }
            uint32_t enabled = 0;
            if (!readOption({reinterpret_cast<uint8_t*>(&enabled), sizeof(enabled)})) { x[0] = uint64_t(-14); break; }
            if (timestamp) {
                endpoint.netlink->timestamp = enabled != 0;
            } else if (name == 2) {
                endpoint.reuseAddress = enabled != 0;
            } else if (name == 16) {
                endpoint.passCredentials = enabled != 0;
            } else if (name == 7) {
                endpoint.sendBuffer = 2 * std::clamp<uint32_t>(enabled, 1024, 131072);
                wakePollers();
            } else if (endpoint.netlink) {
                const bool privileged = endpoint.protocol == 15 ? userId() == 0 :
                    (current->process->effectiveCaps & (1ULL << 12)) != 0;
                if (name == 33 && !privileged) { x[0] = uint64_t(-1); break; }
                endpoint.netlink->receiveBuffer = 2 * std::clamp<uint32_t>(enabled, 1024, 4 * 1024 * 1024);
            } else {
                endpoint.receiveBuffer = 2 * std::clamp<uint32_t>(enabled, 1024, 131072);
            }
            x[0] = 0;
            break;
        }
        default: return false;
    }
    if (x[8] == 203) current->deadline.reset();
    return true;
}

uint64_t ProbeLinux::readDirectory(Machine& machine, const Descriptor& fd) const {
    if (fd.pathOnly()) return uint64_t(-9);
    if (fd.node != Node::directory) return uint64_t(-20);
    const auto& x = machine.cpu.x;
    const auto prefix = fd.path == "/" ? fd.path : fd.path + "/";
    std::map<std::string, uint8_t, std::less<>> entries{{".", 4}, {"..", 4}};
    const auto add = [&](const auto& nodes, uint8_t type) {
        for (auto it = nodes.lower_bound(prefix); it != nodes.end() && it->first.starts_with(prefix); ++it) {
            const auto relative = std::string_view(it->first).substr(prefix.size());
            const auto slash = relative.find('/');
            if (!relative.empty() && relative.front() != '\0') {
                entries.try_emplace(std::string(relative.substr(0, slash)), slash == std::string_view::npos ? type : 4);
            }
        }
    };
    add(directories, 4);
    add(files, 8);
    add(kernelFiles, 8);
    add(writableFiles, 8);
    add(links, 10);
    add(socketPaths, 12);
    add(fifoPaths, 1);
    add(loopDevices, 6);
    add(binderDevices, 2);
    add(ueventTriggers, 8);
    add(kernelSettings, 8);
    add(cgroupFiles, 8);
    if (fd.path == "/proc") {
        for (const auto& [pid, process] : processes)
            if (visibleProcThread(pid)) entries.emplace(std::to_string(pid), 4);
    }
    if (fd.targetProcess && fd.path.starts_with("/proc/") && fd.path.ends_with("/task")) {
        for (const auto& [tid, task] : threads) {
            if (task.process->id == fd.targetProcess && task.wait != Wait::exited)
                entries.emplace(std::to_string(tid), 4);
        }
    }
    if (fd.path == "/proc/self/fd") {
        for (const auto& [number, entry] : descriptors())
            entries.emplace(std::to_string(number), 10); // DT_LNK
    }
    if (fd.path == "/dev") {
        entries.emplace("null", 2);
        entries.emplace("loop-control", 2);
        entries.emplace("ptmx", 2);
        entries.emplace("ashmem", 2);
        entries.emplace(ashmemBootPath.substr(sizeof("/dev/") - 1), 2);
    }
    if (fd.path == "/dev/pts") {
        for (const auto& [name, device] : ptys)
            if (auto pty = device.lock(); pty && !pty->ends[0].expired())
                entries.emplace(std::to_string(pty->number), 2);
        entries.emplace("kmsg", 2);
    }
    if (fd.path == "/dev/input" && guestTouch) entries.emplace("event0", 2); // DT_CHR
    uint64_t index = *fd.offset;
    if (index >= entries.size()) return 0;
    auto item = std::next(entries.begin(), index);
    std::vector<uint8_t> buffer;
    for (; item != entries.end(); ++item) {
        const auto& [name, type] = *item;
        const uint16_t recordSize = (20 + name.size() + 7) & ~uint16_t{7};
        if (buffer.size() + recordSize > x[2]) break;
        const auto start = buffer.size();
        buffer.resize(start + recordSize);
        auto* record = buffer.data() + start;
        const uint64_t inode = std::hash<std::string>{}(prefix + name) | 1;
        const uint64_t next = ++index;
        std::memcpy(record, &inode, 8);
        std::memcpy(record + 8, &next, 8);
        std::memcpy(record + 16, &recordSize, 2);
        record[18] = type;
        std::memcpy(record + 19, name.data(), name.size());
    }
    if (buffer.empty()) return invalid;
    if (!machine.write(x[1], buffer)) return uint64_t(-14);
    *fd.offset = index;
    return buffer.size();
}

uint16_t ProbeLinux::Pipe::pollEvents(bool readEnd) const {
    if (readEnd) return (data.empty() ? 0 : 1) | (writer.expired() ? 16 : 0);
    return (data.size() < capacity ? 4 : 0) | (reader.expired() ? 8 : 0);
}

uint64_t ProbeLinux::makePipe(Machine& machine) {
    const auto& x = machine.cpu.x;
    if (x[1] & ~0x80800ULL) return invalid;
    const auto readFd = freeDescriptor();
    if (readFd == descriptorCapacity) return uint64_t(-24);
    auto pipe = std::make_shared<Pipe>();
    Descriptor reader{"pipe:[read]", {}, uint32_t(x[1]), Node::pipeRead};
    reader.pipe = pipe;
    reader.pipeEnd = std::make_shared<int>(0);
    pipe->reader = reader.pipeEnd;
    descriptors().emplace(readFd, std::move(reader));
    const auto writeFd = freeDescriptor();
    if (writeFd == descriptorCapacity || !writeGuest(machine, x[0], std::array<uint32_t, 2>{readFd, writeFd})) {
        descriptors().erase(readFd);
        return writeFd == descriptorCapacity ? uint64_t(-24) : uint64_t(-14);
    }
    Descriptor writer{"pipe:[write]", {}, uint32_t(x[1]) | 1, Node::pipeWrite};
    writer.pipe = pipe;
    writer.pipeEnd = std::make_shared<int>(0);
    pipe->writer = writer.pipeEnd;
    descriptors().emplace(writeFd, std::move(writer));
    return 0;
}

bool ProbeLinux::fileCall(Machine& machine) {
    auto& x = machine.cpu.x;
    const auto descriptor = descriptors().find(uint32_t(x[0]));
    switch (x[8]) {
        case 36: { // symlinkat
            const auto target = readPath(machine, x[0]);
            const auto path = readPath(machine, x[2]);
            if (!target || !path) { x[0] = uint64_t(-14); break; }
            if (target->empty() || path->empty()) { x[0] = uint64_t(-2); break; }
            std::string logical;
            if (const auto error = resolveAtPath(uint32_t(x[1]), *path, logical)) {
                x[0] = error;
                break;
            }
            const auto backing = backingPath(logical);
            if (!directoryExists(backing.substr(0, backing.rfind('/')))) x[0] = uint64_t(-2);
            else if (links.contains(backing) || find(backing, 0)) x[0] = uint64_t(-17);
            else if (!writablePath(logical)) x[0] = uint64_t(-30);
            else if (userId() != 0) x[0] = uint64_t(-13);
            else if (links.size() - ext4LinkCount >= maxGuestLinks) x[0] = uint64_t(-28);
            else {
                links.emplace(backing, Link{*target, current->fsCreateContext});
                x[0] = 0;
            }
            break;
        }
        case 38: case 276: // renameat / renameat2
            x[0] = renameFile(machine);
            break;
        case 81: // sync
            x[0] = syncData() ? 0 : uint64_t(-5);
            break;
        case 82: case 83: case 267: { // fsync / fdatasync / syncfs
            if (descriptor == descriptors().end() || descriptor->second.pathOnly()) {
                x[0] = uint64_t(-9);
                break;
            }
            const auto& fd = descriptor->second;
            if (fd.node != Node::regular && fd.node != Node::directory && !fd.temporary) {
                x[0] = invalid;
                break;
            }
            const bool persistent = (fd.temporary && fd.temporary->persistent) || dataPath(fd.path);
            x[0] = !persistent || syncDataSoon() ? 0 : uint64_t(-5);
            break;
        }
        case 39: // umount2
            x[0] = unmountFilesystem(machine);
            break;
        case 40: // mount
            x[0] = mountFilesystem(machine);
            break;
        case 33: case 34: case 35: case 48: case 53: case 54: case 56: case 78: case 79: { // *at path operations
            const auto rawPath = readPath(machine, x[1]);
            if (!rawPath) { x[0] = uint64_t(-14); break; }
            if (rawPath->empty()) {
                x[0] = x[8] == 79 && x[3] == 0x1000 && descriptor != descriptors().end()
                    ? stat(machine, x[2], descriptor->second) : uint64_t(-2);
                break;
            }
            std::string path;
            if (const auto error = resolveAtPath(uint32_t(x[0]), *rawPath, path)) { x[0] = error; break; }
            if (x[8] == 56) {
                if (path == GuestApkInbox::device && apkInbox) {
                    if (userId() != 2000 || current->process->context != "u:r:shell:s0" ||
                        current->process->executable != GuestApkInbox::installer) {
                        x[0] = uint64_t(-13);
                        break;
                    }
                    if (x[2] & ~0x80800ULL) { x[0] = invalid; break; }
                    const auto fd = freeDescriptor();
                    if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                    auto request = apkInbox->take();
                    if (!request) {
                        if (!(x[2] & 0x800)) return waitForHostEvents();
                        x[0] = uint64_t(-11);
                        break;
                    }
                    Descriptor entry{path, request->bytes, uint32_t(x[2]), Node::regular};
                    entry.apkRequest = std::move(request);
                    entry.apkInstaller = processId();
                    descriptors().emplace(fd, std::move(entry));
                    x[0] = fd;
                    break;
                }
                if ((path == GuestApkInbox::rotationDevice || path == GuestApkInbox::labelsDevice) && apkInbox) {
                    if (userId() != 2000 || current->process->context != "u:r:shell:s0" ||
                        current->process->executable != GuestApkInbox::rotationWriter || (x[2] & 3) != 1) {
                        x[0] = uint64_t(-13);
                        break;
                    }
                    const auto fd = freeDescriptor();
                    if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                    descriptors().emplace(fd, Descriptor{path, {}, uint32_t(x[2]), Node::null});
                    x[0] = fd;
                    break;
                }
                if ((path == GuestApkInbox::textDevice || path == GuestApkInbox::broadcastDevice) && apkInbox) {
                    const bool broadcast = path == GuestApkInbox::broadcastDevice;
                    if (userId() != (broadcast ? 0u : 2000u) || current->process->context != "u:r:shell:s0" ||
                        current->process->executable != GuestApkInbox::textReader) {
                        x[0] = uint64_t(-13);
                        break;
                    }
                    if (x[2] & ~0x80800ULL) { x[0] = invalid; break; }
                    const auto fd = freeDescriptor();
                    if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                    std::vector<uint8_t> message;
                    if (!(broadcast ? apkInbox->takeBroadcast(message) : apkInbox->takeText(message))) {
                        if (!(x[2] & 0x800)) return waitForHostEvents();
                        x[0] = uint64_t(-11);
                        break;
                    }
                    const auto owned = std::make_shared<std::string>(message.begin(), message.end());
                    Descriptor entry{path, {reinterpret_cast<const uint8_t*>(owned->data()), owned->size()}, uint32_t(x[2]), Node::regular};
                    entry.snapshot = owned;
                    descriptors().emplace(fd, std::move(entry));
                    x[0] = fd;
                    break;
                }
                const auto ownProc = "/proc/" + std::to_string(processId());
                const bool procMaps = path == "/proc/self/maps" || path == ownProc + "/maps";
                const std::string_view procPath = path;
                const auto statTid = procPath.starts_with("/proc/") && procPath.ends_with("/stat") && procPath.size() > 11
                    ? procThreadId(procPath.substr(6, procPath.size() - 11)) : 0;
                if (procMaps || statTid) {
                    if ((x[2] & 3) || (x[2] & 0x240)) { x[0] = uint64_t(-13); break; }
                    const auto fd = freeDescriptor();
                    if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                    std::ostringstream listing;
                    if (statTid) {
                        const auto* subject = visibleProcThread(statTid);
                        if (!subject) { x[0] = uint64_t(-2); break; }
                        const auto& thread = *subject;
                        const auto& process = *thread.process;
                        const auto& segments = process.id == processId() ? machine.segments : process.segments;
                        // mprotect can split the stack. The exec stack pointer stays near its high end.
                        const auto topStack = std::find_if(segments.rbegin(), segments.rend(),
                            [](const Segment& area) { return area.initialStack; });
                        const uint64_t stackStart = topStack == segments.rend() ? 0 :
                            topStack->address + topStack->mappedSize() - 16;
                        const auto taskCount = std::count_if(threads.begin(), threads.end(), [&](const auto& task) {
                            return task.second.process.get() == &process && task.second.wait != Wait::exited;
                        });
                        const char state = thread.wait == Wait::none ? 'R' : thread.wait == Wait::suspended ? 'T' : 'S';
                        listing << statTid << " (" << reinterpret_cast<const char*>(thread.name.data()) << ") " << state
                                << ' ' << process.parent << ' ' << process.group << ' ' << process.session;
                        for (unsigned field = 7; field <= 52; ++field) {
                            listing << ' ';
                            switch (field) {
                                case 18: listing << 20 + thread.nice; break;
                                case 19: listing << thread.nice; break;
                                case 20: listing << taskCount; break;
                                case 28: listing << stackStart; break;
                                default: listing << '0'; break;
                            }
                        }
                        listing << '\n';
                    } else {
                        std::vector<const Segment*> areas;
                        for (const auto& area : machine.segments) areas.push_back(&area);
                        std::sort(areas.begin(), areas.end(), [](const auto* lhs, const auto* rhs) {
                            return lhs->address < rhs->address;
                        });
                        for (const auto* area : areas) {
                            listing << std::hex << area->address << '-' << area->address + area->mappedSize() << ' '
                                    << (area->flags & 4 ? 'r' : '-') << (area->flags & 2 ? 'w' : '-')
                                    << (area->flags & 1 ? 'x' : '-') << (area->shared ? 's' : 'p')
                                    << " 00000000 00:00 0" << (area->initialStack ? " [stack]" : "") << '\n';
                        }
                    }
                    Descriptor entry{path, {}, uint32_t(x[2]), Node::regular};
                    entry.snapshot = std::make_shared<std::string>(std::move(listing).str());
                    entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
                    descriptors().emplace(fd, std::move(entry));
                    x[0] = fd;
                    break;
                }
                x[0] = open(machine, path, uint32_t(x[2]), uint32_t(x[3]));
                break;
            }
            if (x[8] == 79 && x[3] == 0x100) { // AT_SYMLINK_NOFOLLOW
                const auto visible = followLinks(path, false);
                if (!visible) { x[0] = uint64_t(-40); break; }
                const auto backing = backingPath(*visible);
                if (!canAccessParent(backing, false)) { x[0] = uint64_t(-13); break; }
                if (const auto link = resolve(*visible))
                    x[0] = statFile(machine, x[2], false, link->size(),
                                    std::hash<std::string>{}(backing) | 1, 0120777, 0, 0, 0, {});
                else if (const auto node = find(backing, 0))
                    x[0] = stat(machine, x[2], *node);
                else
                    x[0] = uint64_t(-2);
                break;
            }
            if (x[8] == 33 || x[8] == 34 || x[8] == 35) { // mknodat, mkdirat and unlinkat follow only ancestor links.
                uint64_t lookupError = uint64_t(-40);
                auto visible = followLinks(std::move(path), false, &lookupError);
                if (!visible) { x[0] = lookupError; break; }
                path = std::move(*visible);
            }
            auto target = resolve(path);
            if (x[8] == 78 && path == "/proc/self/ns/mnt")
                target = "mnt:[" + std::to_string(current->mountNamespace->id) + "]";
            const auto backing = backingPath(path);
            auto entry = find(x[8] != 35 && target ? backingPath(*target) : backing, 0);
            if (!entry && (x[8] == 48 || x[8] == 79)) {
                const auto resolved = followLinks(path);
                if (!resolved) { x[0] = uint64_t(-40); break; }
                if (*resolved != path) entry = find(backingPath(*resolved), 0);
            }
            if (entry && entry->path != path) entry->visiblePath = path;
            if (x[8] == 33) { // mknodat: named pipes, and the refusal the guest's loop devices expect.
                const bool fifo = (x[2] & 0170000) == 0010000;
                if (!fifo && userId() != 0) { x[0] = uint64_t(-1); break; }
                if (entry || target) { x[0] = uint64_t(-17); break; }
                if (!directoryExists(backing.substr(0, backing.rfind('/')))) { x[0] = uint64_t(-2); break; }
                if (!fifo) { x[0] = writablePath(path) ? uint64_t(-95) : uint64_t(-30); break; }
                if (!writablePath(path)) { x[0] = uint64_t(-30); break; }
                if (!canAccessParent(backing, true)) { x[0] = uint64_t(-13); break; }
                if (fifoPaths.size() >= 1024) { x[0] = uint64_t(-28); break; }
                fifoPaths.emplace(backing, NamedPipe{std::make_shared<Pipe>(), uint32_t(x[2] & 0777), userId(), groupId()});
                x[0] = 0;
            } else if (x[8] == 35) { // unlinkat and rmdir
                constexpr uint64_t removeDirectory = 0x200;
                if (x[2] && x[2] != removeDirectory) { x[0] = invalid; break; }
                if (x[2] == removeDirectory) {
                    if (!entry && !target) { x[0] = uint64_t(-2); break; }
                    if (!entry || entry->node != Node::directory || target) { x[0] = uint64_t(-20); break; }
                    if (path == "/" || current->mountNamespace->mounts.contains(path)) {
                        x[0] = uint64_t(-16); break;
                    }
                    if (!writablePath(path)) { x[0] = uint64_t(-30); break; }
                    if (!canAccessParent(backing, true)) { x[0] = uint64_t(-13); break; }
                    const auto prefix = backing + "/";
                    const auto hasChild = [&](const auto& entries) {
                        const auto next = entries.lower_bound(prefix);
                        return next != entries.end() && next->first.starts_with(prefix);
                    };
                    if (hasChild(directories) || hasChild(files) || hasChild(writableFiles) ||
                        hasChild(links) || hasChild(socketPaths) || hasChild(fifoPaths)) {
                        x[0] = uint64_t(-39);
                    } else if (const auto file = cgroupFiles.find(backing + std::string(cgroupProcs)); file != cgroupFiles.end()) {
                        if (std::any_of(threads.begin(), threads.end(), [&](const auto& task) {
                                return task.second.wait != Wait::exited && task.second.process->cgroup == backing;
                            })) { x[0] = uint64_t(-16); break; }
                        cgroupFiles.erase(file);
                        inetCreatePrograms.erase(backing);
                        inetReleasePrograms->erase(backing);
                        for (auto& [type, hooks] : inetAddressPrograms) hooks.erase(backing);
                        inetGetOptionPrograms.erase(backing);
                        inetSetOptionPrograms.erase(backing);
                        inetIngressPrograms.erase(backing);
                        inetEgressPrograms.erase(backing);
                        inetSocketOperationPrograms.erase(backing);
                        x[0] = directories.erase(backing) ? 0 : uint64_t(-2);
                    } else {
                        x[0] = directories.erase(backing) ? 0 : uint64_t(-2);
                    }
                    if (!x[0]) {
                        fileEvent(backing, fileDeleted, true);
                        if (const auto host = sharedHost(backing); !host.empty()) {
                            std::error_code ignored;
                            std::filesystem::remove_all(host, ignored);  // with what the host put there that the guest never saw
                        }
                    }
                } else if ((links.contains(backing) || (entry && entry->temporary)) && writablePath(path)) {
                    if (!canAccessParent(backing, true)) { x[0] = uint64_t(-13); break; }
                    sharedFileRemoved(backing);
                    writableFiles.erase(backing);
                    links.erase(backing);
                    fileEvent(backing, fileDeleted);
                    x[0] = 0;
                } else if (entry && entry->node == Node::directory) x[0] = uint64_t(-21);
                else if (entry && entry->node == Node::cgroupProcs) x[0] = uint64_t(-1);
                else if (!entry && !links.contains(backing)) x[0] = uint64_t(-2);
                else if (socketPaths.contains(path) && userId() == 0) { socketPaths.erase(path); x[0] = 0; }
                else if (fifoPaths.contains(backing)) {
                    if (!canAccessParent(backing, true)) { x[0] = uint64_t(-13); break; }
                    fifoPaths.erase(backing); // descriptors that have it open keep the pipe
                    x[0] = 0;
                }
                else if (!writablePath(path)) x[0] = uint64_t(-30);
                else return false;
            } else if (x[8] == 53 || x[8] == 54) { // fchmodat, fchownat
                if (!entry) { x[0] = uint64_t(-2); break; }
                const auto changeMetadata = [&](auto& node) {
                    // CAP_CHOWN may give a file away, CAP_FOWNER change the mode of one it does not own:
                    // system_server hands each new process its cgroup with the first.
                    const auto caps = current->process->effectiveCaps;
                    const bool may = userId() == 0 || (x[8] == 53 ? userId() == node.uid || (caps & (1u << 3)) :
                                                       (caps & 1u) || ownerMayChown(node.uid, uint32_t(x[2]), uint32_t(x[3])));
                    if (!may) return uint64_t(-1);
                    if (x[8] == 53) node.mode = x[2] & 0777;
                    else {
                        if (uint32_t(x[2]) != UINT32_MAX) node.uid = uint32_t(x[2]);
                        if (uint32_t(x[3]) != UINT32_MAX) node.gid = uint32_t(x[3]);
                    }
                    return uint64_t(0);
                };
                if (entry->temporary) {
                    x[0] = changeMetadata(*entry->temporary);
                    if (!x[0] && x[8] == 53) entry->temporary->accessAcl.updateMode(entry->temporary->mode, false);
                } else if (entry->cgroupFile) x[0] = changeMetadata(*entry->cgroupFile);
                else if (auto socket = socketPaths.find(entry->path); socket != socketPaths.end())
                    x[0] = changeMetadata(socket->second);
                else if (!writablePath(target ? *target : path)) x[0] = uint64_t(-30);
                else if (auto directory = directories.find(entry->path); directory != directories.end())
                    x[0] = x[8] == 53 ? chmodDirectory(directory->second, uint32_t(x[2])) :
                        changeMetadata(directory->second);
                else if (auto block = loopDevices.find(entry->path); block != loopDevices.end())
                    x[0] = changeMetadata(block->second);
                else if (auto binder = binderDevices.find(entry->path); binder != binderDevices.end())
                    x[0] = changeMetadata(binder->second);
                else return false;
                if (!x[0]) fileEvent(entry->path, fileAttributes, entry->node == Node::directory);
            } else if (x[8] == 34) {
                const auto parent = backing.substr(0, backing.rfind('/'));
                if (entry || target) x[0] = uint64_t(-17);
                else if (!directoryExists(parent)) x[0] = uint64_t(-2);
                else if (!writablePath(path)) x[0] = uint64_t(-30);
                else if (!canAccessParent(backing, true)) x[0] = uint64_t(-13);
                else if (directories.size() - ext4DirectoryCount >= maxGuestDirectories) x[0] = uint64_t(-28);
                else {
                    const auto* parentMetadata = parentDirectory(backing);
                    const bool setgid = parentMetadata && (parentMetadata->mode & setgidMode);
                    const auto gid = setgid ? parentMetadata->gid : groupId();
                    Directory directory{uint32_t(x[2] & 01777), userId(), gid, newFileContext(backing)};
                    if (setgid) directory.mode |= setgidMode;
                    directory.accessAcl = inheritAcl(parentMetadata, directory.mode);
                    if (parentMetadata) directory.defaultAcl = parentMetadata->defaultAcl;
                    directories.emplace(backing, std::move(directory));
                    if (const auto host = sharedHost(backing); !host.empty()) ::mkdir(host.c_str(), 0755);
                    if (const auto* view = mountFor(path); view && view->device == "cgroup2") {
                        cgroupFiles.emplace(backing + std::string(cgroupProcs),
                                            std::make_shared<Directory>(Directory{0644, userId(), gid}));
                    }
                    fileEvent(backing, fileCreated, true);
                    x[0] = 0;
                }
            } else if (x[8] == 78) {
                if (!x[3]) { x[0] = invalid; break; }
                if (!target) { x[0] = entry ? invalid : uint64_t(-2); break; }
                const auto size = std::min<uint64_t>(x[3], target->size());
                x[0] = machine.write(x[2], {reinterpret_cast<const uint8_t*>(target->data()), size})
                    ? size : uint64_t(-14);
            } else if (!entry) {
                x[0] = uint64_t(-2);
            } else if (x[8] == 48) { // faccessat uses real credentials.
                const auto directory = directories.find(entry->path);
                if (x[2] > 7) x[0] = invalid;
                else if (!canAccessParent(entry->path, false, true)) x[0] = uint64_t(-13);
                else if (entry->temporary)
                    x[0] = inodeAllows(*entry->temporary, x[2], true) ? 0 : uint64_t(-13);
                else if (directory != directories.end())
                    x[0] = inodeAllows(directory->second, x[2], true) ? 0 : uint64_t(-13);
                else x[0] = (x[2] & 2) && entry->node == Node::regular ? uint64_t(-13) : 0;
            } else {
                if (x[3]) return false;
                x[0] = stat(machine, x[2], *entry);
            }
            break;
        }
        case 23: { // dup
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            const auto fd = freeDescriptor();
            if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            auto copy = descriptor->second;
            copy.flags &= ~0x80000U;
            descriptors().emplace(fd, std::move(copy));
            x[0] = fd;
            break;
        }
        case 24: { // dup3
            if (uint32_t(x[0]) == uint32_t(x[1]) || (x[2] & ~0x80000ULL)) { x[0] = invalid; break; }
            if (descriptor == descriptors().end() || x[1] >= descriptorLimit()) { x[0] = uint64_t(-9); break; }
            auto duplicate = descriptor->second;
            duplicate.flags = (duplicate.flags & ~0x80000U) | uint32_t(x[2]);
            if (const auto replaced = descriptors().find(uint32_t(x[1])); replaced != descriptors().end())
                releaseRecordLocks(replaced->second, processId());
            descriptors().insert_or_assign(uint32_t(x[1]), std::move(duplicate));
            x[0] = x[1];
            break;
        }
        case 26: case 27: case 28:
            return inotify(machine);
        case 61: // getdents64
            x[0] = descriptor == descriptors().end() ? uint64_t(-9) : readDirectory(machine, descriptor->second);
            break;
        case 57: // close
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!descriptor->second.pathOnly() && descriptor->second.offset.use_count() == 1 &&
                (descriptor->second.node == Node::regular || descriptor->second.temporary ||
                 descriptor->second.node == Node::directory)) {
                const auto& fd = descriptor->second;
                fileEvent(fd.path, fd.flags & 3 ? fileClosedWrite : fileClosedRead, fd.node == Node::directory);
            }
            releaseRecordLocks(descriptor->second, processId());
            {
                // Only closing something a poll or an epoll can wait on changes what they wait for: a
                // pipe's other end, a socket's peer, an event. Files, directories and kernel settings do
                // not (epoll does not even take them). Waking every waiting thread for each of the
                // start's tens of thousands of closes made them all ask again, for nothing: 26,000
                // wakes of some ten threads each before the home screen.
                const auto& fd = descriptor->second;
                static constexpr Node pollable[] = {Node::poller, Node::event, Node::inotify, Node::signal, Node::socket,
                    Node::keySocket, Node::pipeRead, Node::pipeWrite, Node::timer, Node::ptyMaster, Node::ptySlave,
                    Node::binder, Node::processHandle, Node::touchDevice, Node::pressure, Node::fuse, Node::sound,
                    Node::input, Node::kernelLog};
                const bool watched = std::ranges::find(pollable, fd.node) != std::end(pollable) || fd.socket || fd.pipe ||
                    fd.event || fd.binder;
                // A whole-file lock (flock) held through it goes with its last descriptor, and waiters
                // for that lock wait as pollers do.
                const bool locks = std::ranges::any_of(wholeFileLocks, [&](const auto& entry) {
                    return std::ranges::any_of(entry.second, [&](const auto& lock) { return lock.owner.lock() == fd.offset; });
                });
                // Nor does closing one of several descriptors of the same open description, as a child
                // that zygote forks does with what it inherited: the pipe or socket is still open.
                const bool shared = fd.offset && fd.offset.use_count() > 1;
                const bool quiet = !locks && (fd.pathOnly() || shared || !watched);
                descriptors().erase(descriptor);
                if (!quiet) wakePollers();
            }
            x[0] = 0;
            break;
        case 62: { // lseek
            if (descriptor == descriptors().end() || descriptor->second.pathOnly()) {
                x[0] = uint64_t(-9); break;
            }
            auto& fd = descriptor->second;
            if (fd.node != Node::regular && fd.node != Node::temporary && fd.node != Node::directory &&
                fd.node != Node::kernelSetting && fd.node != Node::oomScoreAdjustment) {
                x[0] = uint64_t(-29); break;
            }
            if (x[2] > 2 || (fd.node == Node::directory && x[2] != 0)) { x[0] = invalid; break; }
            const auto length = fd.temporary ? fd.temporary->storage->size :
                fd.node == Node::kernelSetting ? std::to_string(kernelSettings.at(fd.path).value).size() + 1 :
                fd.snapshot ? fd.snapshot->size() : fd.contentSize();
            const auto origin = x[2] == 1 ? *fd.offset : x[2] == 2 ? length : 0;
            const auto delta = static_cast<int64_t>(x[1]);
            if ((delta < 0 && uint64_t(-x[1]) > origin) ||
                (delta >= 0 && uint64_t(delta) > uint64_t(INT64_MAX) - origin)) {
                x[0] = invalid; break;
            }
            *fd.offset = origin + x[1];
            if (fd.snapshot && fd.event && fd.event->mountNamespace && !*fd.offset)
                *fd.snapshot = mountList(*fd.event->mountNamespace);
            x[0] = *fd.offset;
            break;
        }
        case 59: // pipe2
            x[0] = makePipe(machine);
            break;
        case 60: { // quotactl: query quota availability on a guest block filesystem.
            constexpr uint32_t getQuota = 0x800007;
            if (uint32_t(x[0]) >> 8 != getQuota || (x[0] & 255) > 2) { x[0] = invalid; break; }
            const auto source = readPath(machine, x[1]);
            if (!source) { x[0] = uint64_t(-14); break; }
            if (source->empty()) { x[0] = uint64_t(-2); break; }
            const auto target = followLinks(absolutePath(*source));
            if (!target) { x[0] = uint64_t(-40); break; }
            const auto device = backingPath(*target);
            if (!loopDevices.contains(device)) { x[0] = uint64_t(-2); break; }
            const bool mounted = std::any_of(current->mountNamespace->mounts.begin(),
                current->mountNamespace->mounts.end(), [&](const auto& entry) {
                    return entry.second.ext4 && entry.second.device == device;
                });
            // The read-only guest Ext4 implementation has no quota operations.
            x[0] = uint64_t(mounted ? -38 : -19);
            break;
        }
        case 285: { // copy_file_range, when guest filesystems require buffered copying.
            if (x[5]) { x[0] = invalid; break; }
            const auto target = descriptors().find(uint32_t(x[2]));
            if (descriptor == descriptors().end() || target == descriptors().end() ||
                descriptor->second.pathOnly() || target->second.pathOnly() ||
                (descriptor->second.flags & 3) == 1 || !(target->second.flags & 3) ||
                (target->second.effectiveFlags() & 0x400)) { x[0] = uint64_t(-9); break; }
            const auto regular = [](const Descriptor& file) { return file.node == Node::regular || file.temporary; };
            if (!regular(descriptor->second) || !regular(target->second)) { x[0] = invalid; break; }
            uint64_t position = 0;
            if ((x[1] && !machine.load64(x[1], position)) || (x[3] && !machine.load64(x[3], position))) {
                x[0] = uint64_t(-14);
                break;
            }
            const auto& input = descriptor->second;
            const auto& output = target->second;
            x[0] = uint64_t(mountFor(input.path) != mountFor(output.path) ? -18 : -95);
            break;
        }
        case 76: { // splice, which the FUSE daemon reads its device with: there is never a second request.
            if (descriptor == descriptors().end() || !descriptor->second.fuse) return false;
            if (descriptor->second.fuse->unmounted) { x[0] = uint64_t(-19); break; }
            if (descriptor->second.fuse->requests.empty()) return waitForEvents();
            x[0] = invalid;
            break;
        }
        case 71: { // sendfile between guest regular files.
            const auto input = descriptors().find(uint32_t(x[1]));
            if (descriptor == descriptors().end() || input == descriptors().end() ||
                descriptor->second.pathOnly() || input->second.pathOnly() ||
                !(descriptor->second.flags & 3) || (input->second.flags & 3) == 1) {
                x[0] = uint64_t(-9);
                break;
            }
            const auto& output = descriptor->second;
            const auto& source = input->second;
            if (!output.temporary || (!source.temporary && source.node != Node::regular) ||
                (output.effectiveFlags() & 0x400)) { x[0] = invalid; break; }
            uint64_t position = *source.offset;
            if (x[2] && (!machine.load64(x[2], position) || !machine.store64(x[2], position))) {
                x[0] = uint64_t(-14);
                break;
            }
            if (position > INT64_MAX) { x[0] = invalid; break; }
            const auto data = source.temporary
                ? std::span<const uint8_t>(source.temporary->storage->pages).first(source.temporary->storage->size)
                : source.contents();
            if (!x[3] || position >= data.size()) { x[0] = 0; break; }
            auto& file = *output.temporary;
            if (file.seals & (sealWrite | sealFutureWrite)) { x[0] = uint64_t(-1); break; }
            const auto count = std::min<uint64_t>(x[3], data.size() - position);
            const auto offset = *output.offset;
            const auto limit = file.persistent || file.memoryFile ? maxDataFileBytes : maxGuestFileBytes;
            if (offset > limit || count > limit - offset) { x[0] = uint64_t(-27); break; }
            if (const auto error = writeFileBytes(file, offset, data.subspan(position, count)); error) {
                x[0] = error;
                break;
            }
            *output.offset = offset + count;
            if (!x[2]) *source.offset = position + count;
            fileEvent(source.path, fileAccessed);
            fileEvent(output.path, fileModified);
            x[0] = !x[2] || machine.store64(x[2], position + count) ? count : uint64_t(-14);
            break;
        }
        case 63: case 67: { // read, pread64
            if (descriptor == descriptors().end() || descriptor->second.pathOnly()) { x[0] = uint64_t(-9); break; }
            auto& fd = descriptor->second;
            if ((fd.flags & 3) == 1) { x[0] = uint64_t(-9); break; }
            if (fd.node == Node::inotify) return readInotify(machine, fd);
            if (fd.fuse) {
                auto& requests = fd.fuse->requests;
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                if (fd.fuse->unmounted) { x[0] = uint64_t(-19); break; }
                if (requests.empty()) {
                    if (!(fd.effectiveFlags() & 0x800)) return waitForEvents();
                    x[0] = uint64_t(-11);
                    break;
                }
                if (x[2] < requests.front().size()) { x[0] = invalid; break; }
                if (!machine.write(x[1], requests.front())) { x[0] = uint64_t(-14); break; }
                x[0] = requests.front().size();
                requests.pop_front();
                break;
            }
            if (fd.touch) {
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                if (x[2] < sizeof(GuestTouch::Event)) { x[0] = x[2] ? invalid : 0; break; }
                const auto count = fd.touch->read(x[2] / sizeof(GuestTouch::Event),
                    [&](auto bytes) { return machine.write(x[1], bytes); });
                if (count == uint64_t(-11) && !(fd.effectiveFlags() & 0x800)) return waitForHostEvents();
                x[0] = count;
                break;
            }
            if (fd.graphics) {
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                const auto result = readGraphics(machine, *fd.graphics, x[1], x[2]);
                if (result == uint64_t(-11)) {
                    if (!(fd.effectiveFlags() & 0x800)) return waitForHostEvents();
                    if (fd.graphics->renderChannel) {
                        // Let the renderer and other guest tasks run between empty polls.
                        // The nonblocking read still completes with EAGAIN at this PC.
                        current->nextSliceAfter = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
                        sliceEnd = 0;
                    }
                }
                x[0] = result;
                break;
            }
            if (fd.cgroupFile) {
                const auto found = cgroupFiles.find(fd.path);
                if (found == cgroupFiles.end() || found->second != fd.cgroupFile) { x[0] = uint64_t(-19); break; }
                const auto group = std::string_view(fd.path).substr(0, fd.path.size() - cgroupProcs.size());
                std::set<uint64_t> members;
                for (const auto& [tid, task] : threads) {
                    if (task.wait != Wait::exited && task.process->cgroup == group) members.insert(task.process->id);
                }
                std::string contents;
                for (const auto member : members) contents += std::to_string(member) + '\n';
                const auto offset = x[8] == 67 ? x[3] : *fd.offset;
                const auto bytes = std::string_view(contents).substr(std::min<uint64_t>(offset, contents.size()));
                const auto size = std::min<uint64_t>(bytes.size(), x[2]);
                const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
                if (size && !machine.write(x[1], {data, size})) { x[0] = uint64_t(-14); break; }
                if (x[8] == 63) *fd.offset += size;
                x[0] = size;
                break;
            }
            if (fd.pty) {
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                if (!x[2]) { x[0] = 0; break; }
                auto& queue = fd.pty->input[fd.node == Node::ptySlave];
                if (queue.empty()) {
                    if (ptyPoll(fd) & 16) { x[0] = uint64_t(-5); break; }
                    if (fd.effectiveFlags() & 0x800) { x[0] = uint64_t(-11); break; }
                    return waitForEvents();
                }
                const auto size = std::min<uint64_t>(x[2], queue.size());
                const auto end = queue.begin() + size;
                const std::vector<uint8_t> data(queue.begin(), end);
                if (!machine.write(x[1], data)) { x[0] = uint64_t(-14); break; }
                queue.erase(queue.begin(), end);
                wakePollers();
                x[0] = size;
                break;
            }
            if (fd.securityReply || fd.node == Node::kernelSetting || fd.node == Node::oomScoreAdjustment) {
                const auto offset = x[8] == 67 ? x[3] : *fd.offset;
                const auto* subject = fd.node == Node::oomScoreAdjustment ? liveProcess(fd.targetProcess) : nullptr;
                if (fd.node == Node::oomScoreAdjustment && !subject) { x[0] = uint64_t(-3); break; }
                const auto setting = subject ? std::to_string(subject->oomScoreAdjustment) + "\n" :
                    fd.securityReply ? std::string{} : std::to_string(kernelSettings.at(fd.path).value) + "\n";
                const std::string_view reply = fd.securityReply ? *fd.securityReply : setting;
                if (!x[2] || offset >= reply.size()) { x[0] = 0; break; }
                const auto size = std::min<uint64_t>(reply.size() - offset, x[2]);
                const auto* data = reinterpret_cast<const uint8_t*>(reply.data()) + offset;
                if (!machine.write(x[1], {data, size})) { x[0] = uint64_t(-14); break; }
                if (x[8] == 63) *fd.offset += size;
                x[0] = size;
                break;
            }
            if (fd.pipe) {
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                if (!x[2] || (fd.pipe->data.empty() && fd.pipe->writer.expired())) { x[0] = 0; break; }
                if (fd.pipe->data.empty()) {
                    if (!(fd.effectiveFlags() & 0x800)) return waitForEvents();
                    x[0] = uint64_t(-11);
                    break;
                }
                const auto count = std::min<uint64_t>(x[2], fd.pipe->data.size());
                const auto end = fd.pipe->data.begin() + count;
                const std::vector<uint8_t> bytes(fd.pipe->data.begin(), end);
                if (!machine.write(x[1], bytes)) { x[0] = uint64_t(-14); break; }
                fd.pipe->data.erase(fd.pipe->data.begin(), end);
                wakePollers();
                x[0] = count;
                break;
            }
            if (fd.socket) {
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                const auto result = socketRead(machine, fd, x[1], x[2]);
                if (result == uint64_t(-11) && !(fd.effectiveFlags() & 0x800) &&
                    waitForSocketTimeout(fd.socket->receiveTimeout)) return true;
                current->deadline.reset();
                x[0] = result;
                break;
            }
            if (fd.node == Node::signal) {
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                std::array<uint32_t, 32> info{}; // Linux signalfd_siginfo.
                if (x[2] < sizeof(info)) { x[0] = invalid; break; }
                auto& child = current->process->childSignal;
                if (!(fd.event->signalMask & sigChildMask) || !child) {
                    if (fd.effectiveFlags() & 0x800) { x[0] = uint64_t(-11); break; }
                    return waitForEvents();
                }
                info[0] = sigChild;
                info[2] = child->code;
                info[3] = child->pid;
                info[4] = child->uid;
                info[10] = child->status;
                if (!writeGuest(machine, x[1], info)) { x[0] = uint64_t(-14); break; }
                child.reset();
                x[0] = sizeof(info);
                break;
            }
            if (fd.node == Node::event || fd.node == Node::timer) {
                if (fd.node == Node::timer) refreshTimers(std::chrono::steady_clock::now());
                if (x[8] == 67) { x[0] = uint64_t(-29); break; }
                if (x[2] < 8) { x[0] = invalid; break; }
                if (fd.node == Node::timer && std::exchange(fd.event->timer->cancelled, false)) {
                    fd.event->counter = 0;
                    x[0] = uint64_t(-125); // ECANCELED follows a guest wall-clock change.
                    break;
                }
                if (!fd.event->counter) {
                    if (!(fd.effectiveFlags() & 0x800)) return waitForEvents();
                    x[0] = uint64_t(-11);
                    break;
                }
                if (!machine.store64(x[1], fd.event->counter)) { x[0] = uint64_t(-14); break; }
                fd.event->counter = 0;
                if (fd.node == Node::event) {
                    ++fd.event->pollNotifications.writable;
                    wakePollers(fd.event.get());
                }
                x[0] = 8;
                break;
            }
            if (fd.node == Node::null || fd.node == Node::input) { x[0] = 0; break; }
            if (fd.node == Node::entropy) {
                x[0] = x[8] == 67 ? uint64_t(-29) : writeEntropy(machine, x[1], std::min<uint64_t>(x[2], pageSize));
                break;
            }
            if (fd.node == Node::directory) { x[0] = uint64_t(-21); break; }
            if (fd.node != Node::regular && fd.node != Node::loopDevice && !fd.temporary) return false;
            auto bytes = fd.contents();
            if (fd.node == Node::loopDevice) {
                bytes = loopDevices.at(fd.path).bytes;
                if (bytes.empty()) { x[0] = uint64_t(-6); break; }
            }
            if (fd.temporary) bytes = std::span<const uint8_t>(fd.temporary->storage->pages).first(fd.temporary->storage->size);
            if (fd.snapshot) bytes = {reinterpret_cast<const uint8_t*>(fd.snapshot->data()), fd.snapshot->size()};
            const auto offset = x[8] == 63 ? *fd.offset : x[3];
            if (offset > INT64_MAX) { x[0] = invalid; break; }
            if (!x[2] || offset >= bytes.size()) { x[0] = 0; break; }
            const auto size = std::min<uint64_t>(x[2], bytes.size() - offset);
            if (!machine.write(x[1], bytes.subspan(offset, size))) { x[0] = uint64_t(-14); break; }
            if (x[8] == 63) *fd.offset += size;
            if (fd.node == Node::regular || fd.temporary) fileEvent(fd.path, fileAccessed);
            x[0] = size;
            break;
        }
        case 46: { // ftruncate
            if (descriptor == descriptors().end() || !(descriptor->second.flags & 3)) { x[0] = uint64_t(-9); break; }
            const auto& file = descriptor->second.temporary;
            x[0] = !file || x[1] > INT64_MAX ? invalid : resize(*file, x[1]);
            if (!x[0]) {
                markFileModified(*file);
                fileEvent(descriptor->second.path, fileModified);
            }
            break;
        }
        case 47: { // fallocate
            if (x[2] > INT64_MAX || !x[3] || x[3] > INT64_MAX) { x[0] = invalid; break; }
            if (x[1]) { x[0] = uint64_t(-95); break; }
            if (descriptor == descriptors().end() || descriptor->second.pathOnly() ||
                !(descriptor->second.effectiveFlags() & 3)) { x[0] = uint64_t(-9); break; }
            const auto& entry = descriptor->second;
            if (!entry.temporary) { x[0] = uint64_t(entry.pipe ? -29 : -95); break; }
            if (x[3] > uint64_t(INT64_MAX) - x[2]) { x[0] = uint64_t(-27); break; }
            auto& file = *entry.temporary;
            const auto end = x[2] + x[3];
            // Regular guest files use dense pages. An in-range extent is already allocated.
            x[0] = end > file.storage->size ? resize(file, end) : 0;
            if (!x[0]) {
                markFileModified(file);
                fileEvent(entry.path, fileModified);
            }
            break;
        }
        case 52: case 55: { // fchmod, fchown
            if (descriptor == descriptors().end() || descriptor->second.pathOnly()) { x[0] = uint64_t(-9); break; }
            const auto& entry = descriptor->second;
            if (x[8] == 52 && entry.node == Node::directory) {
                const auto& visible = entry.visiblePath.empty() ? entry.path : entry.visiblePath;
                const auto directory = directories.find(entry.path);
                x[0] = directory != directories.end() && writablePath(visible) ?
                    chmodDirectory(directory->second, uint32_t(x[1])) : uint64_t(-30);
                if (!x[0]) fileEvent(entry.path, fileAttributes, true);
                break;
            }
            // The resolver gives each socket it probes routes with to the app it asks for, so that the
            // traffic counts as the app's. Nothing here counts traffic by owner: root may, and that is all.
            if (x[8] == 55 && entry.socket) { x[0] = userId() == 0 ? 0 : uint64_t(-1); break; }
            const auto& file = entry.temporary;
            if (!file) { x[0] = uint64_t(-30); break; }
            const auto caps = current->process->effectiveCaps;
            if (userId() != 0 && userId() != file->uid && !(caps & (x[8] == 52 ? 1u << 3 : 1u))) { x[0] = uint64_t(-1); break; }
            if (x[8] == 52) {
                file->mode = x[1] & 0777;
                file->accessAcl.updateMode(file->mode, false);
            }
            else {
                if (userId() != 0 && !(caps & 1u) && !ownerMayChown(file->uid, uint32_t(x[1]), uint32_t(x[2]))) { x[0] = uint64_t(-1); break; }
                if (uint32_t(x[1]) != UINT32_MAX) file->uid = uint32_t(x[1]);
                if (uint32_t(x[2]) != UINT32_MAX) file->gid = uint32_t(x[2]);
            }
            x[0] = 0;
            fileEvent(descriptor->second.path, fileAttributes);
            break;
        }
        case 5: case 6: case 7: case 14: case 15: case 16: {
            // setxattr and removexattr, including their link and descriptor variants.
            const bool removing = x[8] >= 14;
            std::optional<Descriptor> pathEntry;
            const Descriptor* entry = nullptr;
            Link* link = nullptr;
            if (x[8] == 7 || x[8] == 16) {
                if (descriptor == descriptors().end() || descriptor->second.pathOnly()) {
                    x[0] = uint64_t(-9);
                    break;
                }
                entry = &descriptor->second;
            } else {
                const auto input = readPath(machine, x[0]);
                if (!input) { x[0] = uint64_t(-14); break; }
                if (input->empty()) { x[0] = uint64_t(-2); break; }
                const auto logical = followLinks(absolutePath(*input), x[8] == 5 || x[8] == 14);
                if (!logical) { x[0] = uint64_t(-40); break; }
                const auto backing = backingPath(*logical);
                if (dataPath(backing) && !canAccessParent(backing, false)) {
                    x[0] = uint64_t(-13);
                    break;
                }
                if (const auto found = links.find(backing);
                    (x[8] == 6 || x[8] == 15) && found != links.end()) {
                    link = &found->second;
                    pathEntry.emplace(Descriptor{backing, {}, 0, Node::symbolicLink});
                } else {
                    pathEntry = find(backing, 0);
                }
                if (!pathEntry) { x[0] = uint64_t(-2); break; }
                pathEntry->visiblePath = *logical;
                entry = &*pathEntry;
            }
            const auto name = readPath(machine, x[1]);
            if (!name) { x[0] = uint64_t(-14); break; }
            if (*name == restoreconMarker) {
                const auto directory = directories.find(entry->path);
                if (entry->node != Node::directory || directory == directories.end() || !dataPath(entry->path)) {
                    x[0] = uint64_t(-95);
                    break;
                }
                const auto& visible = entry->visiblePath.empty() ? entry->path : entry->visiblePath;
                if (!writablePath(visible)) { x[0] = uint64_t(-30); break; }
                auto& node = directory->second;
                if (!inodeAllows(node, 2)) {
                    x[0] = uint64_t(-13);
                    break;
                }
                auto& marker = node.restoreconInProgress;
                if (removing) {
                    if (!marker) { x[0] = uint64_t(-61); break; }
                    marker.reset();
                } else {
                    if (x[3] != 1 || x[4] > 2) { x[0] = invalid; break; }
                    if (x[4] == 1 && marker) { x[0] = uint64_t(-17); break; }
                    if (x[4] == 2 && !marker) { x[0] = uint64_t(-61); break; }
                    const auto* value = machine.read(x[2], 1, 4);
                    if (!value) { x[0] = uint64_t(-14); break; }
                    marker = *value;
                }
                fileEvent(entry->path, fileAttributes);
                x[0] = 0;
                break;
            }
            if (*name == accessAclName || *name == defaultAclName) {
                x[0] = changePosixAcl(machine, *entry, *name == defaultAclName, removing);
                break;
            }
            if (name->starts_with("user.")) {
                x[0] = changeUserAttribute(machine, *entry, *name, removing);
                break;
            }
            if (*name != "security.selinux") { x[0] = uint64_t(-95); break; }
            if (removing) { x[0] = uint64_t(-13); break; }
            if (x[3] > 4096 || x[4] > 2) { x[0] = invalid; break; }
            std::vector<uint8_t> bytesScratch;
            const auto* bytes = x[3] ? machine.read(x[2], x[3], 4, bytesScratch) : nullptr;
            if (x[3] && !bytes) { x[0] = uint64_t(-14); break; }
            if (userId() != 0) {
                const auto label = bytes
                    ? std::string_view(reinterpret_cast<const char*>(bytes), x[3]) : std::string_view{};
                if (!canRelabelPackage(*entry, label)) { x[0] = uint64_t(-1); break; }
            }
            if (entry->temporary) {
                auto& attributes = entry->temporary->attributes;
                const bool present = attributes.contains(*name);
                if (x[4] == 1 && present) { x[0] = uint64_t(-17); break; }
                if (x[4] == 2 && !present) { x[0] = uint64_t(-61); break; }
                auto& value = attributes[*name];
                if (x[3]) value.assign(bytes, bytes + x[3]);
                else value.clear();
            } else if (link || entry->node == Node::directory) {
                const auto& visible = entry->visiblePath.empty() ? entry->path : entry->visiblePath;
                if (!writablePath(visible)) { x[0] = uint64_t(-30); break; }
                const auto directory = directories.find(entry->path);
                if (!link && directory == directories.end()) { x[0] = uint64_t(-95); break; }
                auto& context = link ? link->securityContext : directory->second.securityContext;
                if (x[4] == 1 && !context.empty()) { x[0] = uint64_t(-17); break; }
                if (x[4] == 2 && context.empty()) { x[0] = uint64_t(-61); break; }
                std::string_view label = bytes
                    ? std::string_view(reinterpret_cast<const char*>(bytes), x[3]) : std::string_view{};
                if (label.ends_with('\0')) label.remove_suffix(1);
                if (label.find('\0') != std::string_view::npos) { x[0] = invalid; break; }
                context = label;
            } else {
                x[0] = uint64_t(-95);
                break;
            }
            x[0] = 0;
            break;
        }
        case 88: // utimensat
            x[0] = updateTimes(machine);
            break;
        case 80: // fstat
            x[0] = descriptor == descriptors().end() ? uint64_t(-9) : stat(machine, x[1], descriptor->second);
            if (!x[0] && machine.space && machine.space->prelink.table) notePrelink(machine, descriptor->second, x[1]);
            break;
        case 291: // statx
            x[0] = statx(machine);
            break;
        case 43: { // statfs by guest pathname
            const auto path = readPath(machine, x[0]);
            if (!path) { x[0] = uint64_t(-14); break; }
            if (path->empty()) { x[0] = uint64_t(-2); break; }
            const auto target = followLinks(absolutePath(*path));
            if (!target) { x[0] = uint64_t(-40); break; }
            const auto backing = backingPath(*target);
            if (!canAccessParent(backing, false)) { x[0] = uint64_t(-13); break; }
            x[0] = find(backing, 0) ? statFilesystem(machine, x[1], *target) : uint64_t(-2);
            break;
        }
        case 44: // fstatfs by guest descriptor
            x[0] = descriptor == descriptors().end() ? uint64_t(-9) : statFilesystem(machine, x[1], descriptor->second.visiblePath.empty() ?
                                                                            descriptor->second.path : descriptor->second.visiblePath);
            break;
        default:
            return false;
    }
    return true;
}

uint64_t ProbeLinux::randomMappingBase() const {
    const auto bits = kernelSettings.at("/proc/sys/vm/mmap_rnd_bits").value;
    std::random_device entropy;
    std::uniform_int_distribution<uint64_t> pages(0, (uint64_t{1} << bits) - 1);
    // With native execution the first addresses are for code without a translation (native::foreignLow).
    // The random part stays in the lower half of the arena whatever the page size is: a base beyond the
    // arena would make every mapping of the process a search for free addresses from the floor.
    const uint64_t floor = native::enabled() ? native::webViewBase + native::webViewSize : mappingFloor;
    return floor + pages(entropy) * pageSize % ((mappingLimit - floor) / 2);
}

// Keep the randomized bump allocator on the fast path. Long-lived processes
// must still be able to reuse unmapped addresses after reaching the arena end.
static uint64_t freeMappingAddress(const Machine& machine, uint64_t next, uint64_t length) {
    // A mapping of at most a chunk of the guest address space that would reach into the next chunk
    // starts there instead: its two parts could get host memory in different runs (GuestSpace), and
    // translated code that reached across faulted on every access.
    constexpr uint64_t chunk = GuestSpace::chunk;
    if (length <= chunk && (next & (chunk - 1)) + length > chunk) next = (next + chunk - 1) & ~(chunk - 1);
    if (next <= mappingLimit - length) return next;
    auto candidate = native::enabled() ? native::webViewBase + native::webViewSize : mappingFloor;
    for (const auto& segment : machine.segments) {
        if (candidate > mappingLimit - length) return 0;
        if (segment.address >= candidate + length) return candidate;
        candidate = std::max(candidate, pageAlignedLength(segment.address + segment.mappedSize()));
    }
    return candidate <= mappingLimit - length ? candidate : 0;
}

// The lowest free addresses among those set aside for code without a translation, or 0.
static uint64_t freeForeignAddress(const Machine& machine, uint64_t length) {
    auto candidate = native::foreignLow;
    for (const auto& segment : machine.segments) {
        if (segment.address + segment.mappedSize() <= candidate) continue;
        if (segment.address >= candidate + length) break;
        candidate = pageAlignedLength(segment.address + segment.mappedSize());
    }
    return candidate <= native::foreignHigh && length <= native::foreignHigh - candidate ? candidate : 0;
}

uint64_t ProbeLinux::map(Machine& machine) {
    const auto& x = machine.cpu.x;
    constexpr uint64_t fixed = 0x10, anonymous = 0x20, noReserve = 0x4000;
    // Prefaulting is advisory. Existing guest backing and protection rules still apply.
    constexpr uint64_t populate = 0x8000;
    constexpr uint64_t fixedNoReplace = 0x100000;
    const bool atAddress = x[3] & (fixed | fixedNoReplace), anon = x[3] & anonymous;
    const auto sharing = x[3] & 3;
    if (!x[1] || x[1] > maxReservation || x[2] > 7 ||
        (x[3] & ~(fixed | fixedNoReplace | anonymous | noReserve | populate | 3ULL)) || !sharing || sharing == 3 ||
        x[5] % pageSize || (atAddress && x[0] % pageSize)) return invalid;
    if ((x[2] & 6) == 6) return (current->process->mdwe & mdweRefuseExecGain) ? uint64_t(-13) : invalid;
    const auto length = pageAlignedLength(x[1]);
    auto& nextMapping = current->process->nextMapping;
    const auto hint = x[0] & ~(pageSize - 1);
    const auto overlaps = [&](uint64_t start, bool preserveGuestImages) {
        return std::any_of(machine.segments.begin(), machine.segments.end(), [&](const Segment& segment) {
            return start < segment.address + segment.mappedSize() && segment.address < start + length &&
                   (!preserveGuestImages || segment.initialStack || (!segment.anonymous && !segment.fileMapping));
        });
    };
    const bool lowHint = !atAddress && hint >= lowMappingFloor && hint < mappingFloor &&
                         length <= mappingFloor - hint && !overlaps(hint, false);
    // What the dynamic linker reserves is for a library. It goes where native code can jump to it even
    // if it has no translation: a prelinked library is mapped at an address of its own instead.
    const uint64_t forLibrary = !atAddress && !lowHint && anon && !(x[2] & 7) && machine.space && !machine.space->interpreted() &&
        machine.space->inLinker(machine.cpu.pc) ? freeForeignAddress(machine, length) : 0;
    // A hint in the arena is taken when the range is free, as Linux takes it. V8 places its code range
    // near its own code this way, and gives up when every mapping lands somewhere else.
    const uint64_t arenaFloor = native::enabled() ? native::webViewBase + native::webViewSize : mappingFloor;
    const bool highHint = !atAddress && !lowHint && !forLibrary && hint >= arenaFloor && length <= mappingLimit - hint &&
                          !overlaps(hint, false);
    const bool webView = !atAddress && anon && !(x[2] & 7) && length == native::webViewSize && machine.space &&
                         native::covers(native::webViewBase, native::webViewBase + length) && !overlaps(native::webViewBase, false);
    const auto start = atAddress ? x[0] : webView ? native::webViewBase : lowHint || highHint ? hint :
                       forLibrary ? forLibrary : freeMappingAddress(machine, nextMapping, length);
    if (static const bool trace = std::getenv("NESTVANE_NATIVE_TRACE"); trace && machine.space && !(x[2] & 7) && anon)
        std::fprintf(stderr, "[native] pid %llu reserves 0x%llx bytes hint 0x%llx flags 0x%llx pc 0x%llx: linker %d interpreted %d -> 0x%llx\n",
                     static_cast<unsigned long long>(processId()), static_cast<unsigned long long>(length),
                     static_cast<unsigned long long>(x[0]), static_cast<unsigned long long>(x[3]),
                     static_cast<unsigned long long>(machine.cpu.pc), machine.space->inLinker(machine.cpu.pc),
                     machine.space->interpreted(), static_cast<unsigned long long>(start));
    if (start < lowMappingFloor || start > mappingLimit - length) return noMemory;
    const auto end = start + length;
    if ((x[3] & fixedNoReplace) && overlaps(start, false)) {
        if (std::getenv("NESTVANE_NATIVE_TRACE") && start >= 0x8000'0000 && start < mappingFloor) {
            std::fprintf(stderr, "[native] pid %llu: 0x%llx+0x%llx is taken:", static_cast<unsigned long long>(processId()),
                         static_cast<unsigned long long>(start), static_cast<unsigned long long>(length));
            for (const auto& segment : machine.segments)
                if (start < segment.address + segment.mappedSize() && segment.address < start + length)
                    std::fprintf(stderr, " 0x%llx+0x%llx", static_cast<unsigned long long>(segment.address),
                                 static_cast<unsigned long long>(segment.mappedSize()));
            std::fprintf(stderr, "\n");
        }
        return uint64_t(-17);
    }
    if (start < mappingFloor && (end > mappingFloor || (atAddress && overlaps(start, true))))
        return noMemory;
    if (atAddress && !(x[3] & fixedNoReplace) && start >= mappingFloor && end > nextMapping) return noMemory;
    std::span<const uint8_t> bytes;
    uint64_t backedLength = length;
    uint64_t imageLength = 0;
    WritableFile* snapshotFile = nullptr;
    if (anon) {
        if (x[5]) return invalid;
    } else {
        const auto file = descriptors().find(uint32_t(x[4]));
        if (file == descriptors().end() || file->second.pathOnly()) return uint64_t(-9);
        const auto& fd = file->second;
        if ((fd.flags & 3) == 1) return uint64_t(-13);
        if (const auto& map = fd.bpfMap) {
            constexpr uint32_t mappable = 1 << 10, programReadOnly = 1 << 7;
            const bool ring = map->type == BpfMap::ringBuffer;
            if (!ring && map->type != 2) return uint64_t(-524);
            if (sharing != 1 || (!ring && !(map->flags & mappable))) return invalid;
            const bool writable = x[2] & 2;
            if (ring && writable && (x[5] || length != pageSize)) return uint64_t(-1);
            if ((x[2] & 4) || (writable && (!(fd.flags & 3) || (map->flags & programReadOnly))))
                return uint64_t(-13);
            if (x[5] > map->values.size || length > map->values.size - x[5]) return invalid;
            // Keep the BPF object alive as long as the guest mapping, even after close().
            Segment segment{start, {}, guestFlags(x[2]), length, false, {map, &map->values},
                            x[5], false, !writable, false, true};
            segment.maxProtection = writable ? 3 : 1;
            if (atAddress) changeRange(machine, start, end, {});
            if (!atAddress || (x[3] & fixedNoReplace)) nextMapping = std::max(nextMapping, end);
            machine.insertSegment(std::move(segment));
            return start;
        }
        if (fd.graphicsAddress) {
            if (sharing != 1) return uint64_t(-95);
            if ((x[2] & 4) || ((x[2] & 2) && !(fd.flags & 3))) return uint64_t(-13);
            const auto region = fd.graphicsAddress->mapping(x[5], length);
            if (!region) return uint64_t(-1);
            Segment segment{start, {}, guestFlags(x[2]), length, false, {region, &region->storage},
                            x[5] - region->offset, false, !(fd.flags & 3), false, true};
            segment.maxProtection = 3;
            if (atAddress) changeRange(machine, start, end, {});
            if (!atAddress || (x[3] & fixedNoReplace)) nextMapping = std::max(nextMapping, end);
            machine.insertSegment(std::move(segment));
            return start;
        }
        if (fd.ashmem) {
            auto& region = *fd.ashmem;
            const auto backed = region.size <= maxDataFileBytes ? pageAlignedLength(region.size) : 0;
            if (!backed || x[5] > backed || length > backed - x[5]) return invalid;
            if (x[2] & ~region.protection) return uint64_t(-1);
            if (sharing != 1) return uint64_t(-95);
            if ((x[2] & 2) && !(fd.flags & 3)) return uint64_t(-13);
            if (!region.allocated) {
                std::erase_if(memoryFiles, [](const auto& file) { return file.expired(); });
                if (memoryFiles.size() >= resourceObjectCapacity) return uint64_t(-23);
                if (const auto error = resize(region.file, region.size); error) return error;
                memoryFiles.push_back(region.file.storage);
                region.allocated = true;
            }
            Segment segment{start, {}, guestFlags(x[2]), length, false, region.file.storage, x[5], false,
                            !(fd.flags & 3), false, true};
            segment.maxProtection = region.protection;
            if (atAddress) changeRange(machine, start, end, {});
            if (!atAddress || (x[3] & fixedNoReplace)) nextMapping = std::max(nextMapping, end);
            machine.insertSegment(std::move(segment));
            return start;
        }
        if (fd.binder) {
            if (fd.binder->owner != processId()) return uint64_t(-1);
            if (atAddress || x[5] || x[2] != 1 || length > binderMappingLimit) return invalid;
            if (fd.binder->mapped) return uint64_t(-16);
            if (!allowsBacking(machine, start, end, length, memoryLimit)) return noMemory;
            auto mapping = std::make_shared<SharedFileBytes>();
            mapping->pages.resize(length);
            mapping->size = length;
            fd.binder->mapped = mapping;
            fd.binder->address = start;
            nextMapping = std::max(nextMapping, end);
            machine.insertSegment({start, {}, 4, length, false, std::move(mapping), 0, false, true});
            return start;
        }
        if ((fd.node != Node::regular && !fd.temporary) || fd.snapshot) return uint64_t(-19);
        if (x[5] > INT64_MAX - length) return invalid;
        if (sharing == 1 && (x[2] & 2) && !(fd.flags & 3)) return uint64_t(-13);
        if (sharing == 1 && fd.temporary) {
            const bool sealed = fd.temporary->seals & (sealWrite | sealFutureWrite);
            if (sealed && (x[2] & 2)) return uint64_t(-1);
            if (atAddress) changeRange(machine, start, end, {});
            else nextMapping = std::max(nextMapping, end);
            machine.insertSegment({start, {}, guestFlags(x[2]), length, false,
                                        fd.temporary->storage, x[5], false, sealed || !(fd.flags & 3), false, true});
            return start;
        }
        const auto raw = fd.temporary
            ? std::span<const uint8_t>(fd.temporary->storage->pages).first(fd.temporary->storage->size) : fd.contents();
        if (x[5] < raw.size()) bytes = raw.subspan(x[5], std::min<uint64_t>(length, raw.size() - x[5]));
        // Large private maps can share their existing copy-on-write backing.
        if (sharing == 2 && bytes.size() >= 256 * 1024) snapshotFile = fd.temporary.get();
        backedLength = pageAlignedLength(bytes.size());
        // The root loader and retained read-only APEX mounts own these bytes throughout the boot.
        // In a guest address space a private writable mapping can start as a view too: it is mapped
        // copy-on-write there, so only the pages the guest writes take memory of their own.
        if ((!(x[2] & 2) || (machine.space && sharing == 2)) && !fd.temporary && !fd.apkRequest && files.contains(fd.path))
            imageLength = bytes.size() & ~(pageSize - 1);
        if (machine.space && imageLength && reinterpret_cast<uintptr_t>(bytes.data()) % GuestSpace::page) {
            // A file inside a mounted image starts wherever its blocks are. Guest address spaces map
            // whole host pages, so such a file gets one page-aligned copy that every process maps,
            // instead of a copy per process.
            auto& aligned = alignedFiles[raw.data()];
            if (aligned.size() != raw.size()) {
                const auto capacity = (raw.size() + GuestSpace::page - 1) & ~(GuestSpace::page - 1);
                void* memory = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (memory != MAP_FAILED) {
                    std::memcpy(memory, raw.data(), raw.size());
                    aligned = {static_cast<const uint8_t*>(memory), raw.size()};
                }
            }
            if (aligned.size() == raw.size()) bytes = aligned.subspan(x[5], bytes.size());
        }
    }
    const bool sparse = anon && !x[2];
    const bool lazy = anon && !sparse && sharing == 2 && length >= lazyAnonymousThreshold;
    const auto copiedLength = backedLength - imageLength;
    if (!sparse && !lazy && copiedLength && (copiedLength > memoryLimit / 2 ||
        !allowsBacking(machine, start, end, copiedLength, memoryLimit))) return noMemory;
    const auto flags = guestFlags(x[2]);
    Segment segment{start + imageLength, {}, flags, sparse || lazy ? length : 0,
                    false, {}, 0, anon, !anon && sharing == 1, false, !anon};
    segment.sharedAnonymous = anon && sharing == 1;
    if (lazy) {
        if (machine.space) {
            // The range may still hold pages of mappings this one replaces: they go first.
            if (atAddress) changeRange(machine, start, end, {});
            segment.anonymousPages = AnonymousPages::allocateIn(machine.space, start, length);
        } else {
            segment.anonymousPages = AnonymousPages::allocate(length, anonymousBudget);
        }
        if (!segment.anonymousPages) return noMemory;
    } else if (!sparse && copiedLength) {
        if (snapshotFile && snapshotFile->privateMappingOffset == x[5] &&
            snapshotFile->privateMappingSourceSize == bytes.size()) {
            auto existing = snapshotFile->privateMapping.lock();
            // MAP_SHARED may change the inode without a file syscall, so verify the visible bytes.
            if (existing.size() == copiedLength &&
                std::memcmp(existing.data(), bytes.data(), bytes.size()) == 0)
                segment.bytes = std::move(existing);
        }
        if (segment.bytes.empty()) {
            segment.bytes.resize(copiedLength);
            std::copy(bytes.begin() + imageLength, bytes.end(), segment.bytes.mutableData());
            if (snapshotFile) {
                snapshotFile->privateMappingOffset = x[5];
                snapshotFile->privateMappingSourceSize = bytes.size();
                snapshotFile->privateMapping.remember(segment.bytes);
            }
        }
    }
    if (atAddress) changeRange(machine, start, end, {});
    if (!atAddress || (x[3] & fixedNoReplace)) nextMapping = std::max(nextMapping, end);
    if (!anon && (x[2] & 4) && machine.space && native::enabled()) {
        // Code of a translated file, mapped where it was translated for, runs natively from now on.
        const auto& fd = descriptors().find(uint32_t(x[4]))->second;
        if (const auto* module = fd.temporary ? nullptr : native::mappedCode(fd.contents(), start, x[5]);
            module && native::activate(*module)) {
            machine.space->addCode(module->info[0], module->info[1]);
            // Linker namespaces can load one file more than once. The next load of this file goes to
            // another translation of the same bytes, if there is one that the process does not use.
            auto& prelink = machine.space->prelink;
            const auto record = std::find(prelink.addresses.begin(), prelink.addresses.end(), module->base);
            if (record != prelink.addresses.end()) {
                const auto contents = fd.contents();
                const native::Module* next = nullptr;
                for (unsigned copy = 0; (next = native::moduleFor(contents, copy)); ++copy)
                    if (std::find(prelink.used.begin(), prelink.used.end(), next->base) == prelink.used.end()) break;
                if (next && machine.store64(prelink.table + (record - prelink.addresses.begin()) * 24 + 16, next->base)) {
                    *record = next->base;
                    prelink.used.push_back(next->base);
                }
            }
        } else {
            // Code that can only be interpreted. Native code that calls it looks for it in the table of
            // translated code. Where the table spans the address, the look-up faults and the interpreter
            // takes over until the code returns; anywhere else the look-up would read memory that is no
            // table, so the whole process is interpreted from here on.
            const bool inPlace = native::covers(start, end);
            if (!machine.space->interpreted() && androidLogObserver) {
                std::string name = fd.path.substr(0, 200);
                for (char& character : name)
                    if (character < 0x20 || character > 0x7e) character = '?';
                char where[64];
                std::snprintf(where, sizeof(where), " at 0x%llx", static_cast<unsigned long long>(start));
                const bool known = !fd.temporary && native::moduleFor(fd.contents());
                androidLogObserver("native execution: " + current->process->executable + " (pid " + std::to_string(processId()) +
                                   (inPlace ? ") interprets code without a translation in place: " :
                                              ") is interpreted from here on, it maps code without a translation in place: ") +
                                   name + where + (known ? ", translated for another address" : ""));
            }
            if (!inPlace) machine.space->dropCode();
            if (static const bool report = std::getenv("NESTVANE_NATIVE_TRACE"); report) {
                const auto* known = fd.temporary ? nullptr : native::moduleFor(fd.contents());
                std::fprintf(stderr, "[native] interpreted code: %s at 0x%llx offset 0x%llx (%s) in pid %llu %s, %zu prelinked\n", fd.path.c_str(),
                             static_cast<unsigned long long>(start), static_cast<unsigned long long>(x[5]),
                             known ? "translated for another address" : "no translation",
                             static_cast<unsigned long long>(processId()), current->process->executable.c_str(),
                             machine.space->prelink.files.size());
            }
        }
    }
    if (imageLength) {
        Segment view{start, {}, flags, imageLength, false, {}, 0, false, sharing == 1, false, true};
        view.imageView = bytes.data();
        machine.insertSegment(std::move(view));
    }
    if (sparse || copiedLength) machine.insertSegment(std::move(segment));
    if (backedLength < length) {
        // Linux accepts the mapping. Access to pages entirely beyond EOF faults.
        machine.insertSegment({start + backedLength, {}, 0, length - backedLength, true, {}, 0, false, false, false, true});
    }
    return start;
}

bool ProbeLinux::canWriteCgroup(const Directory& node) const {
    return userId() == 0 || ((node.mode >> permissionShift(node.uid, node.gid)) & 2);
}

uint64_t ProbeLinux::moveToCgroup(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size) {
    const auto found = cgroupFiles.find(fd.path);
    if (found == cgroupFiles.end() || found->second != fd.cgroupFile) return uint64_t(-19);
    if (!canWriteCgroup(*fd.cgroupFile)) return uint64_t(-13);
    if (!size) return 0;
    if (size > 32) return invalid;
    std::vector<uint8_t> bytesScratch;
    const auto* bytes = machine.read(address, size, 4, bytesScratch);
    if (!bytes) return uint64_t(-14);
    std::string_view input(reinterpret_cast<const char*>(bytes), size);
    while (!input.empty() && (input.back() == '\n' || input.back() == ' ' || input.back() == '\t')) input.remove_suffix(1);
    uint32_t tid = 0;
    const auto [end, error] = std::from_chars(input.begin(), input.end(), tid);
    if (error != std::errc{} || end != input.end() || tid > INT32_MAX) return invalid;
    const auto target = threads.find(tid ? tid : currentTid);
    if (target == threads.end() || target->second.wait == Wait::exited) return uint64_t(-3);
    auto& group = target->second.process->cgroup;
    const auto destination = std::string_view(fd.path).substr(0, fd.path.size() - cgroupProcs.size());
    if (group.empty()) return uint64_t(-2);
    auto ancestor = destination;
    while (ancestor != group && !inside(group, ancestor))
        ancestor = ancestor.substr(0, ancestor.rfind('/'));
    const auto boundary = cgroupFiles.find(std::string(ancestor) + std::string(cgroupProcs));
    if (boundary == cgroupFiles.end() || !canWriteCgroup(*boundary->second)) return uint64_t(-13);
    group = destination;
    return size;
}

uint64_t ProbeLinux::writeDescriptor(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size,
                                     std::optional<uint64_t> position) {
    if (fd.pathOnly() || !(fd.flags & 3)) return uint64_t(-9);
    if (position && !fd.temporary) return uint64_t(-29);
    if (fd.graphics) return writeGraphics(machine, *fd.graphics, address, size);
    if (fd.path == GuestApkInbox::labelsDevice && apkInbox) { // "package<TAB>name" lines from the label tool
        if (size > 64 * 1024) return invalid;
        std::vector<uint8_t> dataScratch;
        const auto* data = size ? machine.read(address, size, 4, dataScratch) : nullptr;
        if (!data) return size ? uint64_t(-14) : 0;
        GuestApkInbox::addLabels({reinterpret_cast<const char*>(data), size});
        return size;
    }
    if (fd.path == GuestApkInbox::rotationDevice && apkInbox) { // one digit from Android's rotation watcher
        std::vector<uint8_t> dataScratch;
        const auto* data = size ? machine.read(address, 1, 4, dataScratch) : nullptr;
        if (!data) return size ? uint64_t(-14) : 0;
        if (*data < '0' || *data > '3') return invalid;
        apkInbox->rotation = *data - '0';
        return size;
    }
    if (fd.node == Node::securityInfo || fd.node == Node::securityValidate) {
        if (!size || size > pageSize) return invalid;
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, size, 4, dataScratch);
        if (!data) return uint64_t(-14);
        std::string_view label(reinterpret_cast<const char*>(data), size);
        if (label.ends_with('\0')) label.remove_suffix(1);
        if (fd.node == Node::securityValidate) {
            const bool known = currentContextTransitions.contains(label) ||
                std::any_of(currentContextTransitions.begin(), currentContextTransitions.end(),
                            [&](const auto& transition) { return transition.second.contains(label); });
            return known || validAppDataContext(label) || validAppProcessContext(label) ? size : invalid;
        }
        if (fd.targetProcess != processId() || *fd.offset) return uint64_t(-1);
        auto& process = *current->process;
        const auto known = currentContextTransitions.find(process.context);
        if ((known == currentContextTransitions.end() || !known->second.contains(label)) && !canSpecializeApp(label))
            return uint64_t(-13);
        if (std::any_of(threads.begin(), threads.end(), [&](const auto& task) {
                return task.first != currentTid && task.second.process == current->process && task.second.wait != Wait::exited;
            })) return uint64_t(-13);
        process.context = label;
        *fd.securityReply = process.context;
        fd.securityReply->push_back('\0');
        return size;
    }
    if (fd.node == Node::securityCreate || fd.node == Node::securityAccess) {
        if (!size || size > pageSize) return invalid;
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, size, 4, dataScratch);
        if (!data) return uint64_t(-14);
        fd.securityReply->clear();
        *fd.offset = 0;
        std::string request(reinterpret_cast<const char*>(data), size);
        if (request.ends_with('\0')) request.pop_back();
        std::istringstream query(std::move(request));
        std::string source, object, trailing;
        uint32_t type = 0;
        if (!(query >> source >> object >> type)) return invalid;
        if (fd.node == Node::securityCreate) {
            if (query >> trailing) return invalid;
            if (source != current->process->context) return uint64_t(-95);
            const auto rule = std::find_if(bootTransitions.begin(), bootTransitions.end(), [&](const auto& entry) {
                const auto& transition = entry.second;
                return source == transition.source && object == transition.file && type == transition.processClass;
            });
            if (rule == bootTransitions.end()) return uint64_t(-95);
            *fd.securityReply = rule->second.destination;
            fd.securityReply->push_back('\0');
        } else {
            uint32_t mask = 0;
            if (!(query >> std::hex >> mask) || (query >> trailing)) return invalid;
            const auto access = accessClasses.find(type);
            if (access == accessClasses.end() || !mask || (mask & ~access->second.mask)) return uint64_t(-95);
            // Root's shell may do anything.
            if (source == rootContext) {
                std::ostringstream reply;
                reply << std::hex << access->second.mask << " ffffffff 0 0 1 0";
                *fd.securityReply = reply.str();
                return size;
            }
            auto decision = access->second.decisions.find({source, object});
            if (decision == access->second.decisions.end() && access->second.applicationService &&
                validAppProcessContext(source)) {
                const auto end = source.find(':', 4);
                decision = access->second.decisions.find({source.substr(0, end) + ":s0", object});
            }
            if (decision == access->second.decisions.end()) {
                const auto appKeys = [&] {
                    if (access->second.applicationKeys.empty() || object != "u:r:keystore:s0" ||
                        !validAppProcessContext(source)) return false;
                    const std::string_view domain = std::string_view(source).substr(4);
                    return !domain.starts_with("isolated_app") && !domain.starts_with("ephemeral_app") &&
                        !domain.starts_with("sdk_sandbox");
                };
                if (!appKeys()) return uint64_t(-95);
                *fd.securityReply = access->second.applicationKeys;
                return size;
            }
            *fd.securityReply = decision->second;
        }
        return size;
    }
    if (fd.node == Node::fsCreateLabel || fd.node == Node::socketCreateLabel) {
        if (*fd.offset || size > 4096) return invalid;
        const bool socket = fd.node == Node::socketCreateLabel;
        const auto& access = accessClasses.at(fsCreateClass);
        const auto& subject = current->process->context;
        const auto decision = access.decisions.find({subject, subject});
        uint32_t allowed = 0;
        if (subject != rootContext && (decision == access.decisions.end() ||
            !(std::istringstream(decision->second) >> std::hex >> allowed) || !(allowed & (socket ? socketCreatePermission : fsCreatePermission))))
            return uint64_t(-13);
        std::string_view label;
        if (size) {
            std::vector<uint8_t> dataScratch;
            const auto* data = machine.read(address, size, 4, dataScratch);
            if (!data) return uint64_t(-14);
            label = {reinterpret_cast<const char*>(data), size};
            if (label.ends_with('\0') || label.ends_with('\n')) label.remove_suffix(1);
        }
        if (!label.empty() && (!(label.starts_with("u:object_r:") || (socket && label.starts_with("u:r:"))) ||
            !std::all_of(label.begin(), label.end(), [](char byte) {
                return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                       byte == ':' || byte == '_' || byte == ',' || byte == '.' || byte == '-';
            }))) return invalid;
        (socket ? current->socketCreateContext : current->fsCreateContext) = label;
        *fd.securityReply = label;
        if (!label.empty()) fd.securityReply->push_back('\0');
        *fd.offset = size;
        return size;
    }
    if (fd.node == Node::execLabel) {
        if (userId() != 0 && current->process->context != "u:r:init:s0") return uint64_t(-1);
        if (!size || size > 256) return invalid;
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, size, 4, dataScratch);
        if (!data) return uint64_t(-14);
        std::string_view label(reinterpret_cast<const char*>(data), size);
        if (label.ends_with('\0')) label.remove_suffix(1);
        if (!label.starts_with("u:r:") || !label.ends_with(":s0") || label.size() < 8) return invalid;
        const auto type = label.substr(4, label.size() - 7);
        if (!std::all_of(type.begin(), type.end(), [](char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
        })) return invalid;
        current->process->pendingContext = label;
        return size;
    }
    if (fd.pipe) {
        if (!size) return 0;
        if (fd.pipe->reader.expired()) return uint64_t(-32);
        const auto space = fd.pipe->capacity - fd.pipe->data.size();
        if (!space || (size <= pageSize && size > space)) return uint64_t(-11);
        const auto count = std::min<uint64_t>(size, space);
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, count, 4, dataScratch);
        if (!data) return uint64_t(-14);
        fd.pipe->data.insert(fd.pipe->data.end(), data, data + count);
        ++fd.pipe->writes;
        wakePollers();
        return count;
    }
    if (fd.cgroupFile) return moveToCgroup(machine, fd, address, size);
    if (fd.node == Node::oomScoreAdjustment) {
        if (!size || size > 16) return invalid;
        auto* subject = liveProcess(fd.targetProcess);
        if (!subject) return uint64_t(-3);
        std::array<uint8_t, 16> bytes{};
        if (!machine.readInto(address, std::span(bytes).first(size))) return uint64_t(-14);
        std::string_view input(reinterpret_cast<const char*>(bytes.data()), size);
        constexpr std::string_view space = " \t\n\r\v\f";
        const auto first = input.find_first_not_of(space);
        if (first == std::string_view::npos) return invalid;
        input = input.substr(first, input.find_last_not_of(space) - first + 1);
        const std::string text(input);
        char* end = nullptr;
        const auto value = std::strtol(text.c_str(), &end, 0);
        if (end == text.c_str() || end != text.c_str() + text.size() || value < -1000 || value > 1000)
            return invalid;
        constexpr uint64_t capSysResource = 1ULL << 24;
        if (current->process->effectiveCaps & capSysResource) subject->minimumOomScoreAdjustment = int(value);
        else if (value < subject->minimumOomScoreAdjustment) return uint64_t(-13);
        subject->oomScoreAdjustment = int(value);
        return size;
    }
    if (fd.node == Node::pressure) return size <= 256 ? size : invalid;  // a trigger: accepted, never fires
    if (fd.fuse) return size;  // the daemon's answer to the one request it gets
    if (fd.node == Node::kernelSetting) {
        if (userId() != 0) return uint64_t(-1);
        if (*fd.offset || !size || size > (isInterfaceSetting(fd.path) ? 128u : 16u)) return invalid;
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, size, 4, dataScratch);
        if (!data) return uint64_t(-14);
        std::string_view value(reinterpret_cast<const char*>(data), size);
        while (!value.empty() && (value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);
        unsigned number = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        auto& setting = kernelSettings.at(fd.path);
        if (isInterfaceSetting(fd.path) && (result.ec != std::errc{} || result.ptr != value.data() + value.size())) {
            *fd.offset += size;  // a setting that is not a number, such as the secret for stable addresses
            return size;
        }
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            number < setting.minimum || number > setting.maximum) return invalid;
        if (fd.path == "/proc/sys/kernel/unprivileged_bpf_disabled" && setting.value == 1 && number != 1)
            return uint64_t(-1);
        setting.value = number;
        *fd.offset += size;
        return size;
    }
    if (fd.node == Node::ueventTrigger) {
        if (userId() != 0) return uint64_t(-1);
        if (size < 3 || size > 4) return invalid;
        std::vector<uint8_t> bytesScratch;
        const auto* bytes = machine.read(address, size, 4, bytesScratch);
        if (!bytes) return uint64_t(-14);
        const std::string_view action(reinterpret_cast<const char*>(bytes), size);
        if (action != "add" && action != "add\n") return invalid;
        emitLoopUevent(ueventTriggers.at(fd.path));
        return size;
    }
    if (fd.pty) {
        if (!size) return 0;
        const bool slave = fd.node == Node::ptySlave;
        if (ptyPoll(fd) & 16) return uint64_t(-5);
        auto& peer = fd.pty->input[!slave];
        const auto count = std::min<uint64_t>(size, 65536 - peer.size());
        if (!count) return uint64_t(-11);
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, count, 4, dataScratch);
        if (!data) return uint64_t(-14);
        const bool crlf = slave && (fd.pty->termios[1] & 5) == 5;
        uint64_t written = 0;
        for (; written < count; ++written) {
            if (crlf && data[written] == '\n') {
                if (65536 - peer.size() < 2) break;
                peer.push_back('\r');
            }
            if (peer.size() == 65536) break;
            peer.push_back(data[written]);
        }
        if (!written) return uint64_t(-11);
        wakePollers();
        return written;
    }
    if (fd.socket) return socketWrite(machine, fd, address, size);
    if (fd.node == Node::event) {
        if (size != 8) return invalid;
        uint64_t increment = 0;
        if (!machine.load64(address, increment)) return uint64_t(-14);
        if (increment == UINT64_MAX) return invalid;
        if (UINT64_MAX - fd.event->counter <= increment) return uint64_t(-11);
        fd.event->counter += increment;
        ++fd.event->pollNotifications.readable;
        wakePollers(fd.event.get());
        return 8;
    }
    if (!size || fd.node == Node::null) return size;
    if (fd.temporary) {
        auto& file = *fd.temporary;
        if (file.seals & (sealWrite | sealFutureWrite)) return uint64_t(-1);
        const auto offset = fd.effectiveFlags() & 0x400 ? file.storage->size : position.value_or(*fd.offset);
        const auto limit = file.persistent || file.memoryFile ? maxDataFileBytes : maxGuestFileBytes;
        if (offset > limit || size > limit - offset) return uint64_t(-27);
        std::vector<uint8_t> splitBuffer;
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, size, 4, dataScratch);
        if (!data) {
            splitBuffer.resize(size);
            if (!machine.readInto(address, splitBuffer)) return uint64_t(-14);
            data = splitBuffer.data();
        }
        if (const auto error = writeFileBytes(file, offset, {data, size}); error) return error;
        if (!position) *fd.offset = offset + size;
        fileEvent(fd.path, fileModified);
        return size;
    }
    if (fd.node == Node::kernelLog) {
        if (size > 65536) return uint64_t(-28);
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(address, size, 4, dataScratch);
        if (!data) return uint64_t(-14);
        captureLog(kernelOutput, recentKernelOutput, 64 * 1024,
                   {reinterpret_cast<const char*>(data), size});
        return size;
    }
    std::string* destination = nullptr;
    switch (fd.node) {
        case Node::output: destination = &machine.cpu.output; break;
        case Node::error: destination = &stderrOutput; break;
        default: return uint64_t(-9);
    }
    if (size > 65536 || destination->size() > 65536 - size) return uint64_t(-28);
    std::vector<uint8_t> dataScratch;
    const auto* data = machine.read(address, size, 4, dataScratch);
    if (!data) return uint64_t(-14);
    destination->append(reinterpret_cast<const char*>(data), size);
    return size;
}

std::chrono::nanoseconds ProbeLinux::realtimeAt(std::chrono::steady_clock::time_point now) const {
    using namespace std::chrono;
    const auto elapsed = duration_cast<nanoseconds>(now.time_since_epoch());
    return elapsed + std::min(realtimeOffset, nanoseconds::max() - elapsed);
}

void ProbeLinux::setRealtime(std::chrono::nanoseconds value) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const auto offset = value - duration_cast<nanoseconds>(now.time_since_epoch());
    if (offset == realtimeOffset) return;
    realtimeOffset = offset;
    for (auto& [_, task] : threads) {
        if (!task.realtimeWaitDeadline ||
            (task.wait != Wait::futex && task.wait != Wait::futexPi && task.wait != Wait::sleep)) continue;
        const auto delay = std::max(nanoseconds{}, *task.realtimeWaitDeadline - value);
        task.deadline = now + std::min(delay, duration_cast<nanoseconds>(steady_clock::time_point::max() - now));
    }
    bool notify = false;
    for (const auto& weak : timers) {
        const auto event = weak.lock();
        if (!event || !event->timer->cancelOnSet) continue;
        event->timer->cancelled = true;
        event->counter = std::max(event->counter, uint64_t{1});
        notify = true;
    }
    if (notify) wakePollers();
    refreshTimers(now);
}

std::optional<std::chrono::steady_clock::time_point> ProbeLinux::refreshTimers(
    std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    std::optional<steady_clock::time_point> next;
    for (const auto& timer : timers) {
        const auto event = timer.lock();
        if (!event || !event->timer->next) continue;
        auto& state = *event->timer;
        const auto clock = state.absoluteRealtime ? realtimeAt(now) : duration_cast<nanoseconds>(now.time_since_epoch());
        if (*state.next <= clock) {
            auto count = uint64_t{1};
            if (state.interval.count()) {
                const auto overdue = clock - *state.next;
                count += overdue / state.interval;
                const auto delay = state.interval - overdue % state.interval;
                state.next = clock + std::min(delay, nanoseconds::max() - clock);
            } else {
                state.next.reset();
            }
            event->counter += std::min(count, UINT64_MAX - event->counter);
            wakePollers(event.get());
        }
        if (state.next) {
            const auto remaining = *state.next - clock;
            const auto deadline = now + std::min(remaining, duration_cast<nanoseconds>(steady_clock::time_point::max() - now));
            if (!next || deadline < *next) next = deadline;
        }
    }
    return next;
}

void ProbeLinux::accountGuestCpu() {
    using namespace std::chrono;
    timespec time{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
        hostCpuSample.reset();
        return;
    }
    const auto sample = seconds(time.tv_sec) + nanoseconds(time.tv_nsec);
    if (hostCpuSample && sample >= *hostCpuSample && current->wait != Wait::exited) {
        const auto elapsed = sample - *hostCpuSample;
        current->cpuTime += elapsed;
        current->process->cpuTime += elapsed;
    }
    hostCpuSample = sample;
}

bool ProbeLinux::switchThread(Machine& machine, std::chrono::steady_clock::time_point deadline, const std::stop_token& stop) {
    using namespace std::chrono;
    serviceGraphicsDma();
    collectNative(machine);
    auto now = steady_clock::now();
    // The shared folder, for files put there from outside: every three seconds once the home screen is up.
    if (!sharedHostRoot.empty() && !appsHeld.load(std::memory_order_relaxed) && now - sharedScannedAt > seconds(3)) {
        sharedScannedAt = now;
        rescanShared();
        now = steady_clock::now();
    }
    std::optional<steady_clock::time_point> nextDeadline;
    bool runnable = false;
    // Until the home screen is shown, apps the user installed do not run: their start at BOOT_COMPLETED
    // (a chat app's sync, say) took the CPU from Android's start, on the main loop and on the workers.
    // Run only when nothing else could, a held app still kept the main loop for whole turns. After
    // half a minute they run anyway, in case the home screen waits for one of them (and before Android
    // gives up on an app process that does not start).
    bool holdApps = false;
    const auto refreshHold = [&] {
        holdApps = appsHeld.load(std::memory_order_relaxed) && now < appsHeldUntil;
        if (!holdApps && appsHeld.load(std::memory_order_relaxed)) appsHeld = false;
    };
    refreshHold();
    const auto held = [&](const Thread& thread) { return holdApps && heldApp(thread); };
    const auto updateTimers = [&]() {
        refreshHold();
        nextDeadline = refreshTimers(now);
        fireAlarms(now, nextDeadline);
        for (auto& [tid, thread] : threads) {
            const auto wait = thread.wait;
            if (wait == Wait::native) {
                // A signal for it is delivered at its next turn, which has to come soon then.
                if (!thread.pendingSignals.empty() || !thread.process->pendingSignals.empty())
                    native::recall(thread.nativeTicket, false);
                continue;
            }
            if (wait == Wait::signalSuspend) {
                auto& child = thread.process->childSignal;
                if (child && canCatchSignal(thread, sigChild) && !thread.pendingSignals.contains(sigChild)) {
                    SignalWords info{};
                    info[0] = sigChild;
                    info[2] = child->code;
                    info[4] = child->pid;
                    info[5] = child->uid;
                    info[6] = child->status;
                    thread.pendingSignals.emplace(sigChild, info);
                    child.reset();
                }
                if (std::ranges::any_of(thread.pendingSignals, [&](const auto& entry) {
                        return canCatchSignal(thread, entry.first);
                    })) {
                    thread.signalInterruptedWait = wait;
                    thread.wait = Wait::none;
                    thread.nextSliceAfter.reset();
                }
            }
            // A child has ended and its parent, waiting in poll, has a handler for that: the handler
            // runs. A program that waits for a child with a time limit, as toybox's timeout does,
            // learns of the end from its handler and not from the wait running out. Zygote is left as
            // it was: its handler reaps every child, and Android was brought up without it running.
            if (wait == Wait::poll && thread.process->context != "u:r:zygote:s0") {
                auto& child = thread.process->childSignal;
                if (child && canCatchSignal(thread, sigChild) && !thread.pendingSignals.contains(sigChild)) {
                    SignalWords info{};
                    info[0] = sigChild;
                    info[2] = child->code;
                    info[4] = child->pid;
                    info[5] = child->uid;
                    info[6] = child->status;
                    thread.pendingSignals.emplace(sigChild, info);
                    child.reset();
                    thread.signalInterruptedWait = wait;
                    thread.wait = Wait::none;
                    thread.nextSliceAfter.reset();
                }
            }
            if (wait == Wait::poll && thread.hostEventRetry) {
                if (*thread.hostEventRetry <= now) {
                    thread.hostEventRetry.reset();
                    thread.wait = Wait::none;
                } else if (!nextDeadline || *thread.hostEventRetry < *nextDeadline) {
                    nextDeadline = thread.hostEventRetry;
                }
            }
            const bool futexWait = wait == Wait::futex || wait == Wait::futexPi;
            if ((wait == Wait::poll || wait == Wait::sleep || futexWait || wait == Wait::signal) && thread.deadline) {
                if (*thread.deadline <= now) {
                    thread.wait = Wait::none;
                    if (futexWait || wait == Wait::signal) {
                        auto& state = tid == currentTid ? machine.cpu : thread.saved;
                        state.pc += 4;
                        state.x[0] = uint64_t(futexWait ? -110 : -11);
                        thread.signalWaitMask = 0;
                        thread.deadline.reset();
                        thread.realtimeWaitDeadline.reset();
                        thread.futexBacking.reset();
                        thread.futexAddressSpace.reset();
                    }
                } else if (!nextDeadline || *thread.deadline < *nextDeadline) {
                    nextDeadline = thread.deadline;
                }
            }
            if (thread.wait != Wait::none) continue;
            if (thread.nextSliceAfter && *thread.nextSliceAfter > now) {
                if (!nextDeadline || *thread.nextSliceAfter < *nextDeadline)
                    nextDeadline = thread.nextSliceAfter;
                continue;
            }
            thread.nextSliceAfter.reset();
            // A held app is not one that can run: the wait is until the hold ends at the latest.
            if (held(thread)) {
                if (!nextDeadline || appsHeldUntil < *nextDeadline) nextDeadline = appsHeldUntil;
                continue;
            }
            runnable = true;
        }
    };
    const auto pickNext = [&]() -> bool {
        auto next = threads.upper_bound(currentTid);
        for (size_t tested = 0; tested < threads.size(); ++tested) {
            if (next == threads.end()) next = threads.begin();
            if (next->second.wait != Wait::none || next->second.nextSliceAfter || held(next->second)) { ++next; continue; }
            if (next->first != currentTid) {
                ++threadSwitches;
                accountGuestCpu();
                const auto retired = machine.cpu.retired;
                auto output = std::move(machine.cpu.output);
                machine.cpu.exclusiveSize = 0;
                const auto previous = currentTid;
                const bool exited = current->wait == Wait::exited;
                if (!exited) current->saved = std::move(machine.cpu);
                if (current->process != next->second.process) {
                    current->process->segments = std::move(machine.segments);
                    current->process->space = std::move(machine.space);
                    machine.segments = std::move(next->second.process->segments);
                    machine.space = std::move(next->second.process->space);
                }
                currentTid = next->first;
                current = &next->second;
                machine.cpu = std::move(current->saved);
                machine.cpu.output = std::move(output);
                machine.cpu.retired = retired;
                if (exited) threads.erase(previous);
            }
            sliceEnd = machine.cpu.retired + sliceLength;
            if (!current->pendingSignals.empty()) deliverPendingCaughtSignal(machine);
            return true;
        }
        return false;
    };
    if (*binderDeathsDirty) refreshBinderDeaths();
    if (!binderReferenceChanges->empty()) refreshBinderReferences();
    // Looking at every thread's timers costs more than the switch itself once there are hundreds of
    // threads, and a boot switches threads hundreds of thousands of times. Timers are looked at every
    // 200 microseconds, and in between the next thread that can run is taken as it is.
    if (now - lastTimerScan < microseconds(200) && pickNext()) return true;
    lastTimerScan = now;
    updateTimers();
    // Without a timer to wait for, there is still the end of a run on a worker.
    while (!runnable && (nextDeadline || native::outstanding()) && now < deadline && !stop.stop_requested()) {
        if (dataSyncPending && now - lastDataSync >= seconds(2)) syncDataSoon();
        auto wake = nextDeadline ? std::min(*nextDeadline, deadline) : deadline;
        if (stop.stop_possible()) wake = std::min(wake, now + milliseconds(100));
        if (native::outstanding()) wake = std::min(wake, now + milliseconds(20));
        uint64_t ended = 0;
        if (wake - now < microseconds(150)) {
            // Too short for the system's timers to be worth it: the wait is for another host thread.
            while (steady_clock::now() < wake && !native::finished(ended)) std::this_thread::yield();
            serviceGraphicsDma();
        } else if (graphicsDma) {
            graphicsDma->waitUntil(wake);
            serviceGraphicsDma();
        } else if (native::outstanding()) {
            native::waitFinished(wake);
        } else {
            std::this_thread::sleep_until(wake);
        }
        collectNative(machine);
        const auto woke = steady_clock::now();
        idleTime += woke - now;
        ++idleWaits;
        now = woke;
        updateTimers();
    }
    if (now >= deadline || stop.stop_requested()) return false;
    return pickNext();
}

bool ProbeLinux::cloneThread(Machine& machine) {
    auto& cpu = machine.cpu;
    auto& x = cpu.x;
    // Bionic pthread_create uses CLONE_VM, FS, FILES, SIGHAND, THREAD,
    // SYSVSEM, SETTLS, PARENT_SETTID and CHILD_CLEARTID.
    constexpr uint64_t pthreadFlags = 0x3d0f00;
    if (x[0] != pthreadFlags) return forkProcess(machine);
    if (!x[1] || x[1] % 16) { x[0] = invalid; return true; }
    if (threads.size() >= maxGuestTasks) { x[0] = uint64_t(-11); return true; }
    const auto tid = static_cast<uint32_t>(nextTid);
    if (!writeGuest(machine, x[2], tid)) { x[0] = uint64_t(-14); return true; }
    Thread child;
    child.process = current->process;
    child.saved = cpu;
    child.saved.output.clear();
    child.saved.pc += 4;
    child.saved.x[0] = 0;
    child.saved.sp = x[1];
    child.saved.threadPointer = x[3];
    child.saved.exclusiveSize = 0;
    child.clearTid = x[4];
    child.mountNamespace = current->mountNamespace;
    child.seccomp = current->seccomp;
    child.noNewPrivileges = current->noNewPrivileges;
    child.signalMask = current->signalMask;
    child.fsCreateContext = current->fsCreateContext;
    child.socketCreateContext = current->socketCreateContext;
    child.ioPriority = current->ioPriority;
    child.nice = current->nice;
    child.name = current->name;
    threads.emplace(nextTid, std::move(child));
    x[0] = nextTid++;
    sliceEnd = cpu.retired; // Give the new runnable task a turn.
    return true;
}

uint64_t ProbeLinux::processId() const {
    return current->process->id;
}

bool ProbeLinux::suspendChild() {
    if (processId() == pid) return false;
    current->wait = Wait::suspended;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::forkProcess(Machine& machine) {
    auto& cpu = machine.cpu;
    auto& x = cpu.x;
    // Bionic fork uses SIGCHLD, CLONE_CHILD_SETTID and CLONE_CHILD_CLEARTID. vfork uses CLONE_VM and
    // CLONE_VFORK, and Java's Runtime.exec and ProcessBuilder start every program with it: here the
    // child gets a copy-on-write copy as from fork, which is all a child that only sets up and execs
    // needs, and the parent goes on at once.
    constexpr uint64_t cloneVm = 0x100, cloneVfork = 0x4000, childSetTid = 0x1000000, childClearTid = 0x200000;
    const bool vfork = (x[0] & ~uint64_t(0xff)) == (cloneVm | cloneVfork) && (x[0] & 0xff) == 17;
    if (!vfork && (x[0] != 0x1200011 || x[1])) return false;
    const auto retry = [&](std::string_view reason, uint64_t requested, int mappingError = 0) {
        if (androidLogObserver) {
            std::ostringstream event;
            event << "guest fork EAGAIN: pid=" << processId() << " reason=" << reason
                  << " requested=" << requested << " tasks=" << threads.size() << '/' << maxGuestTasks
                  << " sparse reserved=" << anonymousBudget->reserved << '/' << anonymousBudget->reservedLimit
                  << " dirty=" << anonymousBudget->dirty << '/' << anonymousBudget->dirtyLimit;
            if (mappingError) event << " host memory errno=" << mappingError;
            androidLogObserver(event.str());
            keepDiagnosticLine(event.str());
        }
        x[0] = uint64_t(-11);
        return true;
    };
    if (threads.size() >= maxGuestTasks) return retry("task limit", 0);
    Thread child;
    child.process = std::make_shared<Process>(*current->process);
    if (child.process->mdwe & mdweNoInherit) child.process->mdwe = 0;
    // A timer belongs to the process that set it.
    child.process->alarmAt.reset();
    child.process->alarmEvery = {};
    child.process->childSignal.reset();
    child.process->pendingSignals.clear();
    child.process->cpuTime = child.process->reapedCpuTime = {};
    child.process->segments = machine.segments;
    // The child's address space is a copy-on-write copy. Its page stores below only point into it.
    std::shared_ptr<GuestSpace> childSpace;
    // The host's address space is shared by every guest process, and a few large apps can fill it. Then
    // a cached app goes, the one Android ranks lowest, and the copy is tried again: otherwise zygote could
    // start no app and the one tapped never opened.
    // NESTVANE_TEST_ROOM=<seconds>: zygote's first fork that many seconds after the home screen is up
    // finds no room, for testing this.
    static const char* const roomTest = std::getenv("NESTVANE_TEST_ROOM");
    static bool roomTested = !roomTest;
    static std::optional<std::chrono::steady_clock::time_point> homeAt;
    if (!roomTested && !homeAt && !appsHeld.load(std::memory_order_relaxed)) homeAt = std::chrono::steady_clock::now();
    if (!roomTested && homeAt && std::chrono::steady_clock::now() - *homeAt > std::chrono::seconds(std::atoi(roomTest)) &&
        current->process->context == "u:r:zygote:s0") {
        roomTested = true;
        if (!endCachedAppForRoom()) return retry("guest address space (test)", 0);
    }
    for (int attempt = 0; machine.space && !(childSpace = machine.space->fork()); ++attempt)
        if (attempt == 4 || !endCachedAppForRoom()) return retry("guest address space", 0);
    child.process->space = childSpace;
    if (childSpace) {
        for (const auto& segment : machine.segments)
            if (segment.omitOnFork && segment.anonymousPages && segment.anonymousPages->guestSpace())
                childSpace->unmap(segment.address, segment.mappedSize());
    }
    std::erase_if(child.process->segments, [](const Segment& segment) { return segment.omitOnFork; });
    // The copy of the address space made shared mappings private. Map the storage into it again.
    for (auto& segment : child.process->segments)
        if (childSpace && segment.shared && segment.placed)
            segment.shared->place(childSpace, segment.address, segment.fileOffset, segment.mappedSize());
    std::map<const AnonymousPages*, std::shared_ptr<AnonymousPages>> privateCopies;
    for (auto& segment : child.process->segments) {
        if (!segment.anonymousPages) {
            if (segment.wipeOnFork && !segment.bytes.empty())
                segment.bytes = std::vector<uint8_t>(segment.bytes.size());
            continue;
        }
        const auto& source = segment.anonymousPages;
        auto& copy = privateCopies[source.get()];
        if (!copy && source->guestSpace()) {
            copy = AnonymousPages::allocateIn(childSpace, source->guestBase(), source->capacity(), false);
            if (!copy) return retry("guest address space", source->capacity());
        }
        if (source->guestSpace()) {
            if (segment.wipeOnFork) copy->discard(segment.fileOffset, segment.mappedSize());
            segment.anonymousPages = copy;
            continue;
        }
        if (!copy) {
            int mappingError = 0;
            copy = AnonymousPages::allocate(source->capacity(), anonymousBudget, &mappingError);
            if (!copy) {
                return retry(mappingError ? "host bookkeeping allocation" : "sparse reservation limit",
                             source->capacity(), mappingError);
            }
        }
        int mappingError = 0;
        if (!segment.wipeOnFork &&
            !source->copyTo(*copy, segment.fileOffset, segment.fileOffset, segment.mappedSize(), &mappingError))
            return retry(mappingError ? "host block mapping" : "dirty page limit",
                         segment.mappedSize(), mappingError);
        segment.anonymousPages = copy;
    }
    child.process->parent = processId();
    child.process->id = nextTid;
    child.mountNamespace = current->mountNamespace;
    child.seccomp = current->seccomp;
    child.noNewPrivileges = current->noNewPrivileges;
    child.signalMask = current->signalMask;
    child.fsCreateContext = current->fsCreateContext;
    child.socketCreateContext = current->socketCreateContext;
    child.ioPriority = current->ioPriority;
    child.nice = current->nice;
    child.name = current->name;
    child.alternateStack = current->alternateStack;
    child.clearTid = x[0] & childClearTid ? x[4] : 0;
    child.saved = cpu;
    child.saved.output.clear();
    child.saved.pc += 4;
    child.saved.x[0] = 0;
    child.saved.exclusiveSize = 0;
    if (vfork && x[1]) child.saved.sp = x[1];
    // The child TID is written only to its private memory view.
    auto parent = std::move(machine.segments);
    auto parentSpace = std::move(machine.space);
    machine.segments = std::move(child.process->segments);
    machine.space = childSpace;
    const bool wrote = !(x[0] & childSetTid) || writeGuest(machine, x[4], uint32_t(nextTid));
    child.process->segments = std::move(machine.segments);
    machine.segments = std::move(parent);
    machine.space = std::move(parentSpace);
    if (!wrote) { x[0] = uint64_t(-14); return true; }
    const auto task = threads.emplace(nextTid, std::move(child)).first;
    processes.emplace(nextTid, task->second.process.get());
    x[0] = nextTid++;
    sliceEnd = cpu.retired;
    return true;
}

bool ProbeLinux::loadBootFileLabels(std::string_view manifest) {
    if (manifest.empty() || manifest.size() > 8192 || !bootFileLabels.empty()) return false;
    std::istringstream lines{std::string(manifest)};
    decltype(bootFileLabels) labels;
    for (std::string line; std::getline(lines, line);) {
        std::istringstream fields(line);
        std::string path, label, trailing;
        if (!(fields >> path >> label) || (fields >> trailing) ||
            (!path.starts_with("/apex/com.android.") && path != "/sys/fs/bpf" &&
             !inside(path, "/sys/fs/bpf")) || normalizePath(path) != path ||
            label.size() <= 14 || !label.starts_with("u:object_r:") || !label.ends_with(":s0")) return false;
        const auto type = std::string_view(label).substr(11, label.size() - 14);
        if (type.empty() || !std::ranges::all_of(type, [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
            }) || !labels.emplace(std::move(path), std::move(label)).second) return false;
    }
    if (labels.empty()) return false;
    bootFileLabels = std::move(labels);
    return true;
}

bool ProbeLinux::loadBootTransition(std::string_view manifest) {
    if (manifest.size() > 8192 || !bootTransitions.empty()) return false;
    std::istringstream lines{std::string(manifest)};
    decltype(bootTransitions) transitions;
    uint32_t processClass = 0;
    for (std::string line; std::getline(lines, line);) {
        BootTransition transition{};
        std::string path, trailing;
        std::istringstream fields(line);
        if (!(fields >> path >> transition.source >> transition.file >> transition.processClass >>
              transition.destination) || (fields >> trailing) || !path.starts_with('/') ||
            !transition.processClass || transition.processClass > UINT16_MAX ||
            (processClass && processClass != transition.processClass) ||
            !transition.source.starts_with("u:r:") || !transition.file.starts_with("u:object_r:") ||
            !transition.destination.starts_with("u:r:") ||
            (!files.contains(path) && !path.starts_with("/apex/com.android."))) return false;
        processClass = transition.processClass;
        if (!transitions.emplace(std::move(path), std::move(transition)).second) return false;
    }
    if (transitions.empty()) return false;
    bootTransitions = std::move(transitions);
    addDirectory("/sys/fs/selinux/class/process/perms", 0555);
    kernelFiles.emplace("/sys/fs/selinux/class/process/index", std::to_string(processClass));
    return true;
}

bool ProbeLinux::loadPropertyPolicy(std::string_view manifest) {
    constexpr std::string_view permissions[] = {"set"};
    return loadAccessClass(manifest, "property_service", permissions);
}

bool ProbeLinux::loadCurrentContextPolicy(std::string_view manifest) {
    if (manifest.size() > 8192 || !currentContextTransitions.empty()) return false;
    const auto valid = [](std::string_view label) {
        if (!label.starts_with("u:r:") || !label.ends_with(":s0") || label.size() <= 7) return false;
        label.remove_prefix(4);
        label.remove_suffix(3);
        return label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string_view::npos;
    };
    decltype(currentContextTransitions) transitions;
    std::istringstream lines{std::string(manifest)};
    for (std::string line; std::getline(lines, line);) {
        std::istringstream fields(line);
        std::string source, target, trailing;
        if (!(fields >> source >> target) || (fields >> trailing) || !valid(source) || !valid(target) ||
            !transitions[source].insert(target).second) return false;
    }
    if (transitions.empty()) return false;
    currentContextTransitions = std::move(transitions);
    return true;
}

bool ProbeLinux::loadServicePolicy(std::string_view manifest, bool hardware) {
    constexpr std::string_view permissions[] = {"add", "find", "list"};
    return loadAccessClass(manifest, hardware ? "hwservice_manager" : "service_manager", permissions);
}

bool ProbeLinux::loadKeystorePolicy(std::string_view manifest, bool key) {
    if (key) {
        constexpr std::string_view permissions[] = {
            "convert_storage_key_to_ephemeral", "delete", "gen_unique_id", "get_info", "grant", "manage_blob",
            "rebind", "req_forced_op", "update", "use", "use_dev_id",
        };
        if (!loadAccessClass(manifest, "keystore2_key", permissions)) return false;
        // The captured decisions cover the system's own processes, which were all there was to ask about
        // when they were taken. Apps get what AOSP's policy gives every app domain except the isolated,
        // instant and SDK sandbox ones (system/sepolicy/private/app.te):
        //   allow { appdomain ... } keystore:keystore2_key { delete use get_info grant rebind update };
        // Without it no app can make a key: EncryptedSharedPreferences and the like fail at once.
        const std::string directory = "/sys/fs/selinux/class/keystore2_key";
        uint32_t allowed = 0;
        for (const std::string_view permission : {"delete", "use", "get_info", "grant", "rebind", "update"})
            allowed |= uint32_t{1} << (std::stoul(kernelFiles.at(directory + "/perms/" + std::string(permission))) - 1);
        std::ostringstream reply;
        reply << std::hex << allowed << " ffffffff 0 ffffffff 1 0";
        accessClasses.at(uint32_t(std::stoul(kernelFiles.at(directory + "/index")))).applicationKeys = reply.str();
        return true;
    }
    constexpr std::string_view permissions[] = {
        "add_auth", "change_password", "change_user", "clear_ns", "clear_uid", "delete_all_keys",
        "early_boot_ended", "get_attestation_key", "get_auth_token", "get_last_auth_time",
        "get_state", "list", "lock", "pull_metrics", "report_off_body", "reset", "unlock",
    };
    return loadAccessClass(manifest, "keystore2", permissions);
}

namespace {

bool validPolicyName(std::string_view name) {
    return !name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string_view::npos;
}

bool validAppMlsRange(std::string_view label) {
    if (label == "s0") return true;
    if (!label.starts_with("s0:")) return false;
    label.remove_prefix(3);

    // Android seapp_contexts computes MLS categories from the package app ID and user ID.
    const auto category = [](std::string_view text) {
        if (!text.starts_with('c') || text.size() == 1) return -1;
        text.remove_prefix(1);
        unsigned value;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end == text.data() + text.size() && value < 1024 ? int(value) : -1;
    };
    int previous = -1;
    while (!label.empty()) {
        const auto comma = label.find(',');
        const auto field = label.substr(0, comma);
        const auto dot = field.find('.');
        const auto first = category(field.substr(0, dot));
        const auto last = dot == std::string_view::npos ? first : category(field.substr(dot + 1));
        if (first <= previous || last < first) return false;
        if (comma == std::string_view::npos) return true;
        previous = last;
        label.remove_prefix(comma + 1);
    }
    return false;
}

} // namespace

bool ProbeLinux::loadSeappContexts(std::string_view rules) {
    if (rules.size() > 1024 * 1024) return false;
    std::istringstream lines{std::string(rules)};
    for (std::string line; std::getline(lines, line);) {
        if (const auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line);
        bool separateLauncher = false;
        std::string typeName, domainName, userName, levelName = "none";
        std::string explicitLevel;
        for (std::string field; fields >> field;) {
            if (field == "neverallow") { separateLauncher = true; break; }
            if (field.starts_with("type=")) typeName = field.substr(5);
            else if (field.starts_with("domain=")) domainName = field.substr(7);
            else if (field.starts_with("user=")) userName = field.substr(5);
            else if (field.starts_with("levelFrom=")) levelName = field.substr(10);
            else if (field.starts_with("level=")) explicitLevel = field.substr(6);
            else if (field == "isSystemServer=true" || field == "fromRunAs=true") separateLauncher = true;
        }
        if (!typeName.empty()) {
            if (!validPolicyName(typeName)) return false;
            appDataLabelTypes.insert(std::move(typeName));
        }
        if (separateLauncher || domainName.empty() || userName.empty()) continue;
        if (!validPolicyName(domainName) || !validPolicyName(userName)) return false;
        if (!explicitLevel.empty() && explicitLevel != "s0") continue;
        if (levelName != "none" && levelName != "app" && levelName != "user" && levelName != "all") return false;
        appLabelRules[domainName].push_back({std::move(userName), levelName == "app" || levelName == "all",
                                            levelName == "user" || levelName == "all"});
    }
    return true;
}

bool ProbeLinux::loadAppTransitionDomains(std::string_view domains) {
    if (domains.size() > 8192 || !appTransitionDomains.empty()) return false;
    decltype(appTransitionDomains) allowed;
    std::istringstream lines{std::string(domains)};
    for (std::string domain; std::getline(lines, domain);) {
        if (!validPolicyName(domain) || !appLabelRules.contains(domain) || !allowed.insert(domain).second)
            return false;
    }
    if (allowed.empty()) return false;
    appTransitionDomains = std::move(allowed);
    return true;
}

bool ProbeLinux::validAppDataContext(std::string_view label) const {
    constexpr std::string_view prefix = "u:object_r:";
    if (!label.starts_with(prefix)) return false;
    label.remove_prefix(prefix.size());
    const auto end = label.find(':');
    return end != std::string_view::npos && appDataLabelTypes.contains(label.substr(0, end)) &&
        validAppMlsRange(label.substr(end + 1));
}

bool ProbeLinux::validAppProcessContext(std::string_view label) const {
    constexpr std::string_view prefix = "u:r:";
    if (!label.starts_with(prefix)) return false;
    label.remove_prefix(prefix.size());
    const auto end = label.find(':');
    return end != std::string_view::npos && appTransitionDomains.contains(label.substr(0, end)) &&
        validAppMlsRange(label.substr(end + 1));
}

// Without CAP_CHOWN the owner of a file may still "change" its owner to itself and its group to one of
// its own groups. libprocessgroup does that for each new process's cgroup, from system_server.
bool ProbeLinux::ownerMayChown(uint32_t owner, uint32_t uid, uint32_t gid) const {
    if (userId() != owner || (uid != UINT32_MAX && uid != owner)) return false;
    if (gid == UINT32_MAX || gid == groupId()) return true;
    const auto& groups = current->process->groups;
    return std::find(groups.begin(), groups.end(), gid) != groups.end();
}

bool ProbeLinux::canSpecializeApp(std::string_view label) const {
    constexpr std::string_view zygote = "u:r:zygote:s0", webViewZygote = "u:r:webview_zygote:s0";
    const auto& process = *current->process;
    if (!validAppProcessContext(label)) return false;
    const auto parent = threads.find(process.parent);
    if (parent == threads.end()) return false;
    const auto uid = userId();
    if (process.context == webViewZygote) {
        // WebView's zygote starts renderers, each an isolated app with an isolated uid.
        if (parent->second.process->context != webViewZygote || !label.starts_with("u:r:isolated_app:") ||
            uid % 100000 < 90000) return false;
    } else if (process.context != zygote || parent->second.process->context != zygote ||
               parent->second.process->userIds[0] != 0) {
        return false;
    }
    if (!uid || !std::ranges::all_of(process.userIds, [uid](auto id) { return id == uid; })) return false;

    constexpr unsigned userOffset = 100000, appStart = 10000, sdkStart = 20000, pccStart = 30000,
                       pccEnd = 39999, isolatedStart = 90000;
    const auto user = uid / userOffset;
    auto app = uid % userOffset;
    std::string_view androidUser;
    if (app >= isolatedStart) {
        androidUser = "_isolated";
        app -= isolatedStart;
    } else if (app >= pccStart && app <= pccEnd) {
        androidUser = "_pcc_component";
    } else if (app >= sdkStart && app < pccStart) {
        androidUser = "_sdksandbox";
        app -= sdkStart;
    } else if (app >= appStart && app < sdkStart) {
        androidUser = "_app";
        app -= appStart;
    } else {
        constexpr std::pair<unsigned, std::string_view> users[] = {
            {1000, "system"}, {1001, "radio"}, {1002, "bluetooth"}, {1027, "nfc"}, {1037, "shared_relro"},
            {1053, "webview_zygote"}, {1068, "secure_element"}, {1073, "network_stack"}, {2000, "shell"},
        };
        for (const auto& [id, name] : users) {
            if (app == id) { androidUser = name; break; }
        }
        if (androidUser.empty()) return false;
    }
    label.remove_prefix(4);
    const auto typeEnd = label.find(':');
    const auto rules = appLabelRules.find(label.substr(0, typeEnd));
    return std::ranges::any_of(rules->second, [&](const auto& rule) {
        if (rule.user != androidUser) return false;
        std::string expected = "s0";
        const auto append = [&](unsigned low, unsigned high, unsigned value) {
            expected += expected.size() == 2 ? ":c" : ",c";
            expected += std::to_string(low + (value & 255)) + ",c" + std::to_string(high + ((value >> 8) & 255));
        };
        if (rule.appCategories) append(0, 256, app);
        if (rule.userCategories) append(512, 768, user);
        return label.substr(typeEnd + 1) == expected;
    });
}

std::string ProbeLinux::newFileContext(std::string_view path) const {
    if (!current->fsCreateContext.empty()) return current->fsCreateContext;
    if (!dataPath(path)) return {};
    const auto parent = directories.find(path.substr(0, path.rfind('/')));
    if (parent != directories.end() && !parent->second.securityContext.empty()) return parent->second.securityContext;
    return unlabeledFile;
}

bool ProbeLinux::loadSelinuxPolicyVersion(std::string_view manifest) {
    if (manifest.ends_with('\n')) manifest.remove_suffix(1);
    if (accessClasses.empty() || manifest.empty()) return false;
    unsigned version = 0;
    const auto [end, error] = std::from_chars(manifest.data(), manifest.data() + manifest.size(), version);
    if (error != std::errc{} || end != manifest.data() + manifest.size() || !version || version > 255)
        return false;
    return kernelFiles.emplace("/sys/fs/selinux/policyvers", std::to_string(version)).second;
}

bool ProbeLinux::loadFsCreatePolicy(std::string_view manifest) {
    constexpr std::string_view permissions[] = {"setfscreate", "setsockcreate"};
    std::istringstream header{std::string(manifest.substr(0, manifest.find('\n')))};
    uint32_t index = 0, fileBit = 0, socketBit = 0;
    if (!(header >> index >> fileBit) || !fileBit || fileBit > 32) return false;
    const bool hasSocket = bool(header >> socketBit);
    if (hasSocket && (!socketBit || socketBit > 32)) return false;
    if (!loadAccessClass(manifest, "process", std::span(permissions).first(hasSocket ? 2 : 1))) return false;
    fsCreateClass = index;
    fsCreatePermission = uint32_t{1} << (fileBit - 1);
    socketCreatePermission = hasSocket ? uint32_t{1} << (socketBit - 1) : 0;
    return true;
}

bool ProbeLinux::loadPackageRelabelPolicy(std::string_view manifest, bool directory) {
    constexpr std::string_view permissions[] = {"relabelfrom", "relabelto"};
    std::istringstream header{std::string(manifest.substr(0, manifest.find('\n')))};
    uint32_t index = 0, from = 0, to = 0;
    if (!(header >> index >> from >> to) || !from || from > 32 || !to || to > 32) return false;
    auto& relabel = packageRelabelClasses[directory];
    if (relabel.index || !loadAccessClass(manifest, directory ? "dir" : "file", permissions)) return false;
    relabel = {index, uint32_t{1} << (from - 1), uint32_t{1} << (to - 1)};
    return true;
}

bool ProbeLinux::canRelabelPackage(const Descriptor& entry, std::string_view label) const {
    const auto& source = current->process->context;
    if (userId() != 1000 || source != "u:r:system_server:s0" || !entry.path.starts_with("/data/app/"))
        return false;
    const auto& visible = entry.visiblePath.empty() ? entry.path : entry.visiblePath;
    if (!writablePath(visible)) return false;

    const bool directory = entry.node == Node::directory;
    const auto& relabel = packageRelabelClasses[directory];
    if (!relabel.index) return false;
    std::string_view previous;
    if (directory) {
        const auto found = directories.find(entry.path);
        if (found == directories.end() || found->second.uid != userId()) return false;
        previous = found->second.securityContext;
    } else if (entry.node == Node::temporary && entry.temporary && entry.temporary->uid == userId()) {
        const auto found = entry.temporary->attributes.find("security.selinux");
        if (found == entry.temporary->attributes.end() || found->second.empty()) return false;
        previous = {reinterpret_cast<const char*>(found->second.data()), found->second.size()};
    } else {
        return false;
    }
    if (label.ends_with('\0')) label.remove_suffix(1);
    if (previous.ends_with('\0')) previous.remove_suffix(1);
    const auto& policy = accessClasses.at(relabel.index);
    const auto permits = [&](std::string_view context, uint32_t permission) {
        constexpr std::string_view installed = "u:object_r:apk_data_file:s0";
        constexpr std::string_view staging = "u:object_r:apk_tmp_file:s0";
        if (context != installed && context != staging) return false;
        const auto rule = policy.decisions.find({source, std::string(context)});
        uint32_t allowed = 0;
        return rule != policy.decisions.end() && (std::istringstream(rule->second) >> std::hex >> allowed) &&
            (allowed & permission);
    };
    return permits(previous, relabel.from) && permits(label, relabel.to);
}

bool ProbeLinux::loadAccessClass(std::string_view manifest, std::string_view name,
                                 std::span<const std::string_view> permissions) {
    const auto path = "/sys/fs/selinux/class/" + std::string(name);
    // Reference policy decisions include every captured subject and service label.
    // Leave room for framework HALs while keeping startup memory bounded.
    if (manifest.size() > 4 * 1024 * 1024) return false;
    std::istringstream stream{std::string(manifest)};
    uint32_t type = 0;
    if (!(stream >> type) || !type || type > UINT16_MAX || accessClasses.contains(type)) return false;
    if (const auto declared = kernelFiles.find(path + "/index");
        declared != kernelFiles.end() && declared->second != std::to_string(type)) return false;
    std::vector<uint32_t> positions;
    AccessClass access;
    access.applicationService = name == "service_manager" || name == "hwservice_manager";
    for (size_t i = 0; i < permissions.size(); ++i) {
        uint32_t position = 0;
        if (!(stream >> position) || !position || position > 32) return false;
        const auto bit = uint32_t{1} << (position - 1);
        if (access.mask & bit) return false;
        access.mask |= bit;
        positions.push_back(position);
    }
    std::string line;
    std::getline(stream, line);
    if (line.find_first_not_of(" \t") != std::string::npos) return false;
    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        std::string source, target, trailing;
        uint32_t allowed, decided, auditAllow, auditDeny, sequence, flags;
        if (!(fields >> source >> target >> std::hex >> allowed >> decided >> auditAllow >> auditDeny >>
              std::dec >> sequence >> std::hex >> flags) || (fields >> trailing) ||
            !source.starts_with("u:r:") ||
            !(target.starts_with("u:object_r:") || (name != "property_service" && target.starts_with("u:r:"))))
            return false;
        std::ostringstream reply;
        reply << std::hex << allowed << ' ' << decided << ' ' << auditAllow << ' ' << auditDeny
              << ' ' << std::dec << sequence << ' ' << std::hex << flags;
        if (!access.decisions.emplace(std::pair{source, target}, reply.str()).second) return false;
    }
    if (access.decisions.empty()) return false;
    accessClasses.emplace(type, std::move(access));
    addDirectory(path + "/perms", 0555);
    kernelFiles.emplace(path + "/index", std::to_string(type));
    for (size_t i = 0; i < permissions.size(); ++i)
        kernelFiles.emplace(path + "/perms/" + std::string(permissions[i]), std::to_string(positions[i]));
    if (accessClasses.size() != 1) return true;
    kernelFiles.emplace("/sys/fs/selinux/enforce", "1");
    kernelFiles.emplace("/sys/fs/selinux/deny_unknown", "1");
    // Stable status page for the captured policy. The guest cannot reload this policy.
    std::string status(pageSize, '\0');
    const uint32_t header[] = {1, 0, 1, 0, 1}; // version, sequence, enforcing, policyload, deny_unknown
    std::memcpy(status.data(), header, sizeof(header));
    kernelFiles.emplace("/sys/fs/selinux/status", std::move(status));
    return true;
}

uint64_t ProbeLinux::updateTimes(Machine& machine) {
    using namespace std::chrono;
    const auto& x = machine.cpu.x;
    constexpr int64_t now = (1 << 30) - 1, omit = now - 1;
    constexpr uint64_t atEmptyPath = 0x1000, noFollow = 0x100;
    std::array<int64_t, 4> requested{0, now, 0, now};
    if (x[2]) {
        std::vector<uint8_t> bytesScratch;
        const auto* bytes = machine.read(x[2], sizeof(requested), 4, bytesScratch);
        if (!bytes) return uint64_t(-14);
        std::memcpy(requested.data(), bytes, sizeof(requested));
        if (requested[1] == omit && requested[3] == omit) return 0;
    }
    if (x[3] & ~(atEmptyPath | noFollow)) return invalid;
    if (!x[1] && (x[3] || uint32_t(x[0]) == uint32_t(-100))) return x[3] ? invalid : uint64_t(-14);
    const auto descriptor = descriptors().find(uint32_t(x[0]));
    std::optional<Descriptor> entry;
    if (!x[1]) {
        if (descriptor == descriptors().end()) return uint64_t(-9);
        entry = descriptor->second;
    } else {
        const auto path = readPath(machine, x[1]);
        if (!path) return uint64_t(-14);
        if (path->empty()) {
            if (!(x[3] & atEmptyPath)) return uint64_t(-2);
            if (descriptor != descriptors().end()) entry = descriptor->second;
            else if (uint32_t(x[0]) == uint32_t(-100)) entry = find(backingPath(current->process->workingDirectory), 0);
            else return uint64_t(-9);
        } else {
            std::string absolute;
            if (const auto error = resolveAtPath(uint32_t(x[0]), *path, absolute)) return error;
            const auto logical = followLinks(std::move(absolute), !(x[3] & noFollow));
            if (!logical) return uint64_t(-40);
            const auto backing = backingPath(*logical);
            if (dataPath(backing) && !canAccessParent(backing, false)) return uint64_t(-13);
            entry = find(backing, 0);
            if (entry) entry->visiblePath = *logical;
            else if (links.contains(backing)) return uint64_t(-95);
        }
    }
    if (!entry) return uint64_t(-2);
    for (const auto ns : {requested[1], requested[3]})
        if (ns < 0 || (ns >= 1'000'000'000 && ns != now && ns != omit)) return invalid;

    const auto& visible = entry->visiblePath.empty() ? entry->path : entry->visiblePath;
    if (!writablePath(visible)) return uint64_t(-30);
    const auto directory = directories.find(entry->path);
    const auto update = [&](auto& node) -> uint64_t {
        const bool touch = requested[1] == now && requested[3] == now;
        if (userId() != 0 && userId() != node.uid) {
            if (!touch) return uint64_t(-1);
            if (!inodeAllows(node, 2)) return uint64_t(-13);
        }
        const auto currentTime = realtimeAt(steady_clock::now());
        const auto whole = floor<seconds>(currentTime);
        for (unsigned i = 0; i < requested.size(); i += 2) {
            if (requested[i + 1] == omit) continue;
            node.times[i] = requested[i + 1] == now ? whole.count() : requested[i];
            node.times[i + 1] = requested[i + 1] == now ? (currentTime - whole).count() : requested[i + 1];
        }
        node.times[4] = whole.count();
        node.times[5] = (currentTime - whole).count();
        fileEvent(entry->path, fileAttributes);
        return 0;
    };
    if (entry->temporary) return update(*entry->temporary);
    if (entry->node == Node::directory && directory != directories.end()) return update(directory->second);
    return uint64_t(-95);
}

uint64_t ProbeLinux::changeUserAttribute(Machine& machine, const Descriptor& entry,
                                        const std::string& name, bool removing) {
    if (name.size() <= 5 || name.size() > 255) return uint64_t(-34);
    if (!dataPath(entry.path)) return uint64_t(-95);
    const auto& visible = entry.visiblePath.empty() ? entry.path : entry.visiblePath;
    if (!writablePath(visible)) return uint64_t(-30);
    const auto update = [&](auto& node, bool directory) -> uint64_t {
        constexpr uint64_t capFowner = 1ULL << 3;
        if (userId() != 0) {
            if (directory && (node.mode & 01000) && userId() != node.uid &&
                !(current->process->effectiveCaps & capFowner)) return uint64_t(-1);
            if (!inodeAllows(node, 2)) return uint64_t(-13);
        }
        auto& attributes = node.userAttributes;
        const auto existing = attributes.find(name);
        const bool present = existing != attributes.end();
        if (removing) {
            if (!present) return uint64_t(-61);
            attributes.erase(existing);
        } else {
            const auto& x = machine.cpu.x;
            if (x[4] > 2) return invalid;
            if (x[3] > Directory::maxUserAttributeValue) return uint64_t(-7);
            if (x[4] == 1 && present) return uint64_t(-17);
            if (x[4] == 2 && !present) return uint64_t(-61);
            if (!present && attributes.size() >= Directory::maxUserAttributes) return uint64_t(-28);
            std::vector<uint8_t> value(x[3]);
            if (!value.empty() && !machine.readInto(x[2], value)) return uint64_t(-14);
            attributes.insert_or_assign(name, std::move(value));
        }
        fileEvent(entry.path, fileAttributes, directory);
        return 0;
    };
    if (entry.temporary && entry.temporary->persistent) return update(*entry.temporary, false);
    const auto directory = directories.find(entry.path);
    if (entry.node == Node::directory && directory != directories.end()) return update(directory->second, true);
    return uint64_t(-95);
}

uint64_t ProbeLinux::changePosixAcl(Machine& machine, const Descriptor& entry, bool defaults, bool removing) {
    if (!dataPath(entry.path)) return uint64_t(-95);
    const auto& visible = entry.visiblePath.empty() ? entry.path : entry.visiblePath;
    if (!writablePath(visible)) return uint64_t(-30);
    const auto& x = machine.cpu.x;
    constexpr uint64_t capFowner = 1ULL << 3;
    const auto update = [&](PosixAcl& acl, uint32_t uid, uint32_t& mode) -> uint64_t {
        if (userId() && userId() != uid && !(current->process->effectiveCaps & capFowner))
            return uint64_t(-1);
        if (removing) {
            if (acl.empty()) return uint64_t(-61);
            acl = {};
        } else {
            if (x[4] > 2) return invalid;
            if (x[4] == 1 && !acl.empty()) return uint64_t(-17);
            if (x[4] == 2 && acl.empty()) return uint64_t(-61);
            if (x[3] > PosixAcl::maxBytes) return uint64_t(-7);
            std::vector<uint8_t> bytes(x[3]);
            if (!bytes.empty() && !machine.readInto(x[2], bytes)) return uint64_t(-14);
            auto decoded = bytes.empty() ? std::optional<PosixAcl>(PosixAcl{}) : PosixAcl::decode(bytes);
            if (!decoded) return invalid;
            if (!defaults && !decoded->empty()) {
                mode = (mode & ~0777U) | decoded->modeBits();
                if (!decoded->extended()) decoded = PosixAcl{};
            }
            acl = std::move(*decoded);
        }
        fileEvent(entry.path, fileAttributes, entry.node == Node::directory);
        return 0;
    };
    if (entry.node == Node::directory) {
        const auto directory = directories.find(entry.path);
        if (directory == directories.end()) return uint64_t(-95);
        auto& node = directory->second;
        return update(defaults ? node.defaultAcl : node.accessAcl, node.uid, node.mode);
    }
    if (entry.temporary && !defaults) {
        auto& node = *entry.temporary;
        return update(node.accessAcl, node.uid, node.mode);
    }
    return defaults && entry.temporary ? uint64_t(-13) : uint64_t(-95);
}

bool ProbeLinux::securityCall(Machine& machine) {
    auto& x = machine.cpu.x;
    if (x[8] != 8 && x[8] != 9 && x[8] != 10) return false; // getxattr, lgetxattr, fgetxattr
    const auto name = readPath(machine, x[1]);
    if (!name) { x[0] = uint64_t(-14); return true; }
    std::optional<Descriptor> fd;
    const Link* link = nullptr;
    std::string logical;
    if (x[8] == 10) {
        const auto found = descriptors().find(uint32_t(x[0]));
        if (found == descriptors().end()) { x[0] = uint64_t(-9); return true; }
        fd = found->second;
        logical = fd->visiblePath.empty() ? fd->path : fd->visiblePath;
    } else {
        const auto path = readPath(machine, x[0]);
        if (!path) { x[0] = uint64_t(-14); return true; }
        const auto target = followLinks(absolutePath(*path), x[8] == 8);
        if (!target) { x[0] = uint64_t(-40); return true; }
        logical = *target;
        const auto backing = backingPath(logical);
        if (dataPath(backing) && !canAccessParent(backing, false)) { x[0] = uint64_t(-13); return true; }
        if (const auto found = links.find(backing); x[8] == 9 && found != links.end()) {
            link = &found->second;
            fd.emplace(Descriptor{backing, {}, 0, Node::symbolicLink});
        } else {
            fd = find(backing, 0);
        }
        if (!fd) { x[0] = uint64_t(-2); return true; }
    }
    std::span<const uint8_t> value;
    std::vector<uint8_t> aclBytes;
    bool present = false;
    if (*name == accessAclName || *name == defaultAclName) {
        if (fd->pathOnly()) { x[0] = uint64_t(-9); return true; }
        if (fd->node == Node::directory && dataPath(fd->path)) {
            const auto directory = directories.find(fd->path);
            if (directory != directories.end())
                aclBytes = (*name == defaultAclName ? directory->second.defaultAcl : directory->second.accessAcl).encode();
        } else if (fd->temporary && *name == accessAclName && dataPath(fd->path)) {
            aclBytes = fd->temporary->accessAcl.encode();
        }
        value = aclBytes;
    }
    if (name->starts_with("user.") && *name != restoreconMarker && dataPath(fd->path)) {
        if (fd->pathOnly()) { x[0] = uint64_t(-9); return true; }
        const auto lookup = [&](const auto& node) {
            if (!inodeAllows(node, 4)) return false;
            if (const auto attribute = node.userAttributes.find(*name); attribute != node.userAttributes.end()) {
                value = attribute->second;
                present = true;
            }
            return true;
        };
        const auto directory = directories.find(fd->path);
        if (fd->temporary && fd->temporary->persistent) {
            if (!lookup(*fd->temporary)) { x[0] = uint64_t(-13); return true; }
        } else if (fd->node == Node::directory && directory != directories.end()) {
            if (!lookup(directory->second)) { x[0] = uint64_t(-13); return true; }
        }
    }
    if (*name == restoreconMarker) {
        const auto directory = directories.find(fd->path);
        if (fd->node != Node::directory || directory == directories.end() || !dataPath(fd->path)) {
            x[0] = uint64_t(-61);
            return true;
        }
        const auto& node = directory->second;
        if (!inodeAllows(node, 4)) {
            x[0] = uint64_t(-13);
            return true;
        }
        if (node.restoreconInProgress) value = {&*node.restoreconInProgress, 1};
    }
    if (fd->temporary) {
        const auto attribute = fd->temporary->attributes.find(*name);
        if (attribute != fd->temporary->attributes.end()) value = attribute->second;
    }
    if (*name == "security.selinux") {
        if (const auto directory = directories.find(fd->path);
            directory != directories.end() && !directory->second.securityContext.empty()) {
            const auto& context = directory->second.securityContext;
            value = {reinterpret_cast<const uint8_t*>(context.c_str()), context.size() + 1};
        }
        // Older guest data predates initial inode labels. Expose the filesystem's unlabeled
        // context so libselinux can compute and apply the real restorecon label.
        if (value.empty() && dataPath(fd->path) && (fd->temporary || directories.contains(fd->path)))
            value = {reinterpret_cast<const uint8_t*>(unlabeledFile), sizeof unlabeledFile};
        if (const auto captured = bootFileLabels.find(logical); captured != bootFileLabels.end()) {
            const auto& label = captured->second;
            value = {reinterpret_cast<const uint8_t*>(label.c_str()), label.size() + 1};
        }
        if (const auto transition = bootTransitions.find(logical); transition != bootTransitions.end()) {
            const auto& label = transition->second.file;
            value = {reinterpret_cast<const uint8_t*>(label.c_str()), label.size() + 1};
        }
        if (link && !link->securityContext.empty()) {
            const auto& context = link->securityContext;
            value = {reinterpret_cast<const uint8_t*>(context.c_str()), context.size() + 1};
        }
    }
    if (value.empty()) x[0] = present ? 0 : uint64_t(-61);
    else if (!x[3]) x[0] = value.size();
    else if (value.size() > x[3]) x[0] = uint64_t(-34);
    else x[0] = machine.write(x[2], value) ? value.size() : uint64_t(-14);
    return true;
}

void ProbeLinux::setBootContext(std::string context) {
    current->process->context = std::move(context);
}

std::string ProbeLinux::observeEntry(uint64_t pc) {
    auto& process = *current->process;
    if (!process.pendingEntry || pc != *process.pendingEntry) return {};
    process.pendingEntry.reset();
    sliceEnd = 0; // Restore normal scheduling once the linker transfers control.
    return "  guest PID " + std::to_string(process.id) + " entered " + process.programName +
           (process.context.empty() ? "" : " (guest context " + process.context + ")") + "\n";
}

void ProbeLinux::setCommandLine(const Startup& startup, const Machine* machine) {
    auto& process = *current->process;
    process.argvAddress = 0;
    if (machine) machine->load64(machine->cpu.sp + sizeof(uint64_t), process.argvAddress);
    auto& commandLine = process.commandLine;
    commandLine = startup.argv0.value_or(startup.filename);
    commandLine.push_back('\0');
    for (const auto& argument : startup.arguments) {
        commandLine.append(argument);
        commandLine.push_back('\0');
    }
}

bool ProbeLinux::execProcess(Machine& machine) {
    auto& x = machine.cpu.x;
    const auto filename = readPath(machine, x[0]);
    if (!filename) { x[0] = uint64_t(-14); return true; }
    if (filename->empty()) { x[0] = uint64_t(-2); return true; }
    // Built-in root: su is the shell, started with root's credentials for an app the person allowed.
    bool becomesRoot = false;
    if (rootOn) {
        const auto requested = absolutePath(*filename);
        if (requested == "/system/bin/su" || requested == "/system/xbin/su") {
            const auto uid = current->process->userIds[0];
            const int decision = uid == 0 || kernelSuManager() ? 1 : rootDecision ? rootDecision(uid, packageOfUid(uid)) : 0;
            if (decision < 0) {
                // The person has not answered yet: the start is tried again in a moment.
                current->hostEventRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                return waitForEvents();
            }
            if (!decision) { x[0] = uint64_t(-13); return true; }
            becomesRoot = true;
        }
    }
    const auto hasSiblings = std::any_of(threads.begin(), threads.end(), [&](const auto& entry) {
        return entry.first != currentTid && entry.second.process == current->process && entry.second.wait != Wait::exited;
    });
    if (hasSiblings) return false;
    const auto resolved = followLinks(absolutePath(*filename));
    if (!resolved) { x[0] = uint64_t(-40); return true; }
    const auto& executablePath = *resolved;
    const auto file = find(backingPath(executablePath), 0);
    if (!file) { x[0] = uint64_t(-2); return true; }
    // A program in /data is one of the files the kernel layer writes (an app's native libraries
    // directory has some, which apps such as KernelSU's Manager run): it runs when it may be executed.
    const bool dataProgram = file->node == Node::temporary && file->temporary && file->path.starts_with("/data/");
    if ((file->node != Node::regular && !dataProgram) || file->snapshot ||
        (dataProgram && !(file->temporary->mode & 0111)) ||
        (!file->path.starts_with("/data/") && file->path.find("/bin/") == std::string::npos)) {
        x[0] = uint64_t(-13);
        return true;
    }
    if (const auto* mount = mountFor(executablePath); mount && (mount->flags & 8)) {
        x[0] = uint64_t(-13); return true;
    }
    const auto strings = [&](uint64_t vector, std::vector<std::string>& output) -> uint64_t {
        if (!vector) return 0;
        for (unsigned i = 0; i < 256 && vector <= UINT64_MAX - i * 8; ++i) {
            uint64_t address = 0;
            if (!machine.load64(vector + i * 8, address)) return uint64_t(-14);
            if (!address) return 0;
            auto value = readPath(machine, address);
            if (!value) return uint64_t(-14);
            output.push_back(std::move(*value));
        }
        return uint64_t(-7);
    };
    std::vector<std::string> argv, environment;
    if (auto error = strings(x[1], argv)) { x[0] = error; return true; }
    if (auto error = strings(x[2], environment)) { x[0] = error; return true; }
    try {
        const auto executableBytes = dataProgram
            ? std::span<const uint8_t>(file->temporary->storage->pages).first(file->temporary->storage->size)
            : file->contents();
        const auto interpreterPath = elfInterpreter(executableBytes);
        if (interpreterPath.empty()) return false; // Static exec has a different startup ABI.
        const auto target = followLinks(interpreterPath);
        if (!target) { x[0] = uint64_t(-40); return true; }
        const auto interpreter = find(backingPath(*target), 0);
        if (!interpreter) { x[0] = uint64_t(-2); return true; }
        std::vector<std::string> arguments;
        if (!argv.empty()) arguments.assign(std::next(argv.begin()), argv.end());
        if (becomesRoot) {
            // What su takes and the shell does not: the user to become (root is the only one here)
            // and the options of the common su programs. "-c <command>" is the shell's own.
            std::vector<std::string> shell;
            bool command = false;
            for (auto& argument : arguments) {
                if (command) { shell.push_back(std::move(argument)); continue; }
                if (argument == "-c" || argument == "--command") { shell.push_back("-c"); command = true; continue; }
                if (argument == "-" || argument == "-l" || argument == "--login" || argument == "-m" ||
                    argument == "-p" || argument == "--preserve-environment" || argument == "-mm" ||
                    argument == "--mount-master" || argument == "root" || argument == "0") continue;
                if (argument.starts_with("--")) continue;
                shell.push_back(std::move(argument));
            }
            arguments = std::move(shell);
            becomeRoot();
            if (androidLogObserver) androidLogObserver("Nestvane: root granted to uid " + std::to_string(current->process->userIds[1]));
        }
        auto image = loadDynamicELF(executableBytes, interpreter->contents(), executablePath, interpreterPath,
                                    std::move(arguments), userId());
        image.startup->argv0 = argv.empty() ? std::string{} : argv.front();
        image.startup->environment = std::move(environment);
        image.startup->credentials = std::array<uint64_t, 4>{current->process->userIds[0], userId(),
                                                             current->process->groupIds[0], groupId()};
        const auto stackLimit = current->process->limits.find(3); // RLIMIT_STACK
        if (stackLimit != current->process->limits.end()) {
            image.startup->stackSize = std::max(pageSize, pageAlignedLength(stackLimit->second[0]));
        }
        auto replacement = createMachine(image);
        replacement.cpu.retired = machine.cpu.retired;
        replacement.cpu.output = std::move(machine.cpu.output);
        machine = std::move(replacement);
        auto& process = *current->process;
        process.expeditedMembarrier = false;
        process.executable = executablePath;
        process.programName = *filename;
        setCommandLine(*image.startup, &machine);
        const auto basename = std::string_view(*filename).substr(filename->find_last_of('/') + 1);
        current->name.fill(0);
        std::copy_n(basename.begin(), std::min(basename.size(), current->name.size() - 1), current->name.begin());
        if (!process.pendingContext.empty()) {
            process.context = std::move(process.pendingContext);
            process.pendingContext.clear();
        } else if (const auto rule = bootTransitions.find(executablePath);
                   rule != bootTransitions.end() && process.context == rule->second.source) {
            process.context = rule->second.destination;
        }
        // Guest binaries in this root have no file capabilities or setuid metadata.
        const auto oldPermitted = process.permittedCaps, oldEffective = process.effectiveCaps;
        process.permittedCaps = process.ambientCaps;
        const bool useRootCapabilities = !(process.secureBits & secureNoRoot);
        if (useRootCapabilities && (process.userIds[0] == 0 || userId() == 0))
            process.permittedCaps |= process.capabilityBound | process.inheritableCaps;
        process.effectiveCaps = useRootCapabilities && userId() == 0 ? process.permittedCaps : process.ambientCaps;
        if (current->noNewPrivileges) {
            process.permittedCaps &= oldPermitted;
            process.effectiveCaps &= oldEffective & process.permittedCaps;
        }
        process.secureBits &= ~secureKeepCaps;
        process.dumpable = process.userIds[0] == process.userIds[1] && process.groupIds[0] == process.groupIds[1];
        process.pendingEntry = image.startup->executableEntry;
        process.nextMapping = randomMappingBase();
        for (auto& action : process.signalActions) if (action[0] != 1) action.fill(0);
        process.signalTrampoline = 0;
        std::erase_if(process.descriptors, [&](const auto& fd) {
            if (!(fd.second.flags & 0x80000)) return false;
            releaseRecordLocks(fd.second, processId());
            return true;
        });
        current->clearTid = 0;
        current->fsCreateContext.clear();
        current->socketCreateContext.clear();
        current->alternateStack = {0, 2, 0};
        imageReplaced = true;
        // Let a fresh image make progress through its loader without thrashing address spaces.
        // Any blocking syscall still yields immediately.
        sliceEnd = machine.cpu.retired + 32'000'000;
        wakePollers();
    } catch (const std::bad_alloc&) {
        x[0] = noMemory;
    } catch (const std::runtime_error&) {
        x[0] = uint64_t(-8);
    }
    return true;
}

unsigned ProbeLinux::wakeFutex(uint64_t address, unsigned count, uint32_t mask,
                              const std::shared_ptr<SharedFileBytes>& backing) {
    unsigned woken = 0;
    for (auto& [tid, thread] : threads) {
        if (woken == count) break;
        if (thread.wait != Wait::futex || thread.futexAddress != address || !(thread.futexMask & mask) ||
            thread.futexBacking != backing || (!backing && thread.futexAddressSpace != current->process)) continue;
        thread.wait = Wait::none;
        thread.futexBacking.reset();
        thread.futexAddressSpace.reset();
        thread.deadline.reset();
        thread.saved.pc += 4;
        thread.saved.x[0] = 0;
        ++woken;
    }
    return woken;
}

bool ProbeLinux::futexPi(Machine& machine, uint64_t op, uint64_t address,
                         std::shared_ptr<SharedFileBytes> backing, uint32_t value) {
    using namespace std::chrono;
    constexpr uint32_t waiters = 0x80000000, ownerDied = 0x40000000, tidMask = 0x3fffffff;
    auto& x = machine.cpu.x;
    const auto owner = value & tidMask;
    const bool unlock = op == 7;
    const bool tryLock = op == 8;
    const bool wallClock = op == 6 || (x[1] & 256);
    std::optional<nanoseconds> timeout;
    if (!unlock && !tryLock && x[3]) {
        std::array<int64_t, 2> fields{};
        std::vector<uint8_t> bytesScratch;
        const auto* bytes = machine.read(x[3], sizeof(fields), 4, bytesScratch);
        if (!bytes) { x[0] = uint64_t(-14); return true; }
        std::memcpy(fields.data(), bytes, sizeof(fields));
        if (fields[0] < 0 || fields[1] < 0 || fields[1] >= 1'000'000'000) {
            x[0] = invalid;
            return true;
        }
        timeout = fields[0] > (INT64_MAX - fields[1]) / 1'000'000'000
            ? nanoseconds::max() : nanoseconds(fields[0] * 1'000'000'000 + fields[1]);
    }
    if (unlock ? owner != currentTid : owner == currentTid) {
        x[0] = uint64_t(unlock ? -1 : -35);
        return true;
    }
    Thread* next = nullptr;
    uint32_t nextOwner = 0;
    for (auto& [tid, thread] : threads) {
        if ((thread.wait != Wait::futexPi && thread.wait != Wait::futex) ||
            thread.futexAddress != address || thread.futexBacking != backing ||
            (!backing && thread.futexAddressSpace != current->process)) continue;
        if (thread.wait == Wait::futex) { x[0] = invalid; return true; }
        if (!next || thread.futexPiOrder < next->futexPiOrder) {
            next = &thread;
            nextOwner = uint32_t(tid);
        }
    }
    if (unlock || !owner) {
        const uint32_t replacement = unlock ? (next ? waiters | nextOwner : 0)
                                            : (value & ownerDied) | uint32_t(currentTid);
        if (!writeGuest(machine, x[0], replacement)) { x[0] = uint64_t(-14); return true; }
        if (unlock && next) {
            next->wait = Wait::none;
            next->futexBacking.reset();
            next->futexAddressSpace.reset();
            next->deadline.reset();
            next->realtimeWaitDeadline.reset();
            next->saved.pc += 4;
            next->saved.x[0] = 0;
        }
        x[0] = 0;
        return true;
    }
    if (!writeGuest(machine, x[0], value | waiters)) { x[0] = uint64_t(-14); return true; }
    const auto task = threads.find(owner);
    if (task == threads.end() || task->second.wait == Wait::exited) {
        x[0] = uint64_t(-3);
        return true;
    }
    if (tryLock) { x[0] = uint64_t(-11); return true; }
    current->deadline.reset();
    current->realtimeWaitDeadline.reset();
    if (timeout) {
        const auto now = steady_clock::now();
        const auto epoch = wallClock ? realtimeAt(now) : duration_cast<nanoseconds>(now.time_since_epoch());
        if (*timeout <= epoch) { x[0] = uint64_t(-110); return true; }
        const auto delay = std::min(*timeout - epoch, duration_cast<nanoseconds>(steady_clock::time_point::max() - now));
        current->deadline = now + duration_cast<steady_clock::duration>(delay);
        if (wallClock) current->realtimeWaitDeadline = *timeout;
    }
    current->wait = Wait::futexPi;
    current->futexAddress = address;
    current->futexAddressSpace = backing ? nullptr : current->process;
    current->futexBacking = std::move(backing);
    current->futexPiOrder = nextFutexPiOrder++;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::futex(Machine& machine) {
    auto& x = machine.cpu.x;
    constexpr uint64_t realtime = 256, privateFlag = 128;
    const auto op = x[1] & ~(privateFlag | realtime);
    if ((x[1] & realtime) && op != 9 && op != 13) return false;
    const bool requeue = op == 3 || op == 4;
    const bool pi = op == 6 || op == 7 || op == 8 || op == 13;
    if (op != 0 && op != 1 && !requeue && !pi && op != 9 && op != 10) return false;
    const uint32_t mask = op == 9 || op == 10 ? uint32_t(x[5]) : UINT32_MAX;
    if (x[0] % 4 || !mask) { x[0] = invalid; return true; }
    std::vector<uint8_t> bytesScratch;
    const auto* bytes = machine.read(x[0], 4, 4, bytesScratch);
    if (!bytes) { x[0] = uint64_t(-14); return true; }
    const auto keyFor = [&](uint64_t pointer) {
        auto address = pointer & 0x00ff'ffff'ffff'ffff;
        std::shared_ptr<SharedFileBytes> backing;
        if (!(x[1] & privateFlag)) {
            for (const auto& segment : machine.segments) {
                if (!segment.shared || address < segment.address || address - segment.address >= segment.mappedSize())
                    continue;
                backing = segment.shared;
                address = segment.fileOffset + address - segment.address;
                break;
            }
        }
        return std::pair{address, std::move(backing)};
    };
    auto [address, backing] = keyFor(x[0]);
    if (pi) {
        uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return futexPi(machine, op, address, std::move(backing), value);
    }
    if (requeue) {
        if (int32_t(x[2]) < 0 || int32_t(x[3]) < 0 || x[4] % 4) { x[0] = invalid; return true; }
        std::array<uint8_t, 4> bitset{};
        if (!machine.readInto(x[4], bitset)) { x[0] = uint64_t(-14); return true; }
        uint32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        if (op == 4 && value != uint32_t(x[5])) { x[0] = uint64_t(-11); return true; }
        const auto [target, targetBacking] = keyFor(x[4]);
        const auto woken = wakeFutex(address, uint32_t(x[2]), UINT32_MAX, backing);
        unsigned moved = 0;
        for (auto& [tid, thread] : threads) {
            if (moved == uint32_t(x[3])) break;
            if (thread.wait != Wait::futex || thread.futexAddress != address ||
                thread.futexBacking != backing || (!backing && thread.futexAddressSpace != current->process)) continue;
            thread.futexAddress = target;
            thread.futexBacking = targetBacking;
            thread.futexAddressSpace = targetBacking ? nullptr : current->process;
            ++moved;
        }
        x[0] = woken + moved;
        return true;
    }
    if (op == 1 || op == 10) {
        if (int32_t(x[2]) < 0) { x[0] = invalid; return true; }
        x[0] = wakeFutex(address, uint32_t(x[2]), mask, backing);
        return true;
    }
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    if (value != uint32_t(x[2])) { x[0] = uint64_t(-11); return true; }
    current->realtimeWaitDeadline.reset();
    if (x[3]) {
        using namespace std::chrono;
        std::array<int64_t, 2> timeout{};
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(x[3], sizeof(timeout), 4, dataScratch);
        if (!data) { x[0] = uint64_t(-14); return true; }
        std::memcpy(timeout.data(), data, sizeof(timeout));
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1'000'000'000) {
            x[0] = invalid;
            return true;
        }
        auto remaining = timeout[0] > (INT64_MAX - timeout[1]) / 1'000'000'000
            ? nanoseconds::max() : nanoseconds(timeout[0] * 1'000'000'000 + timeout[1]);
        const auto now = steady_clock::now();
        if (op == 9) {
            const bool wallClock = x[1] & realtime;
            const auto epoch = wallClock ? realtimeAt(now) : duration_cast<nanoseconds>(now.time_since_epoch());
            if (remaining <= epoch) { x[0] = uint64_t(-110); return true; }
            if (wallClock) current->realtimeWaitDeadline = remaining;
            remaining -= epoch;
        }
        if (!remaining.count()) { x[0] = uint64_t(-110); return true; }
        current->deadline = remaining >= steady_clock::time_point::max() - now ?
            steady_clock::time_point::max() : now + duration_cast<steady_clock::duration>(remaining);
    }
    current->wait = Wait::futex;
    current->futexAddress = address;
    current->futexAddressSpace = backing ? nullptr : current->process;
    current->futexBacking = std::move(backing);
    current->futexMask = mask;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::endCachedAppForRoom() {
    // Android's cached apps have 900 and above (ProcessList.CACHED_APP_MIN_ADJ); the higher, the less needed.
    Process* victim = nullptr;
    for (const auto& [id, process] : processes)
        if (process != current->process.get() && process->oomScoreAdjustment >= 900 &&
            (!victim || process->oomScoreAdjustment > victim->oomScoreAdjustment)) victim = process;
    if (!victim) return false;
    const auto id = victim->id;
    std::string name;
    if (const auto main = threads.find(id); main != threads.end())
        name.assign(reinterpret_cast<const char*>(main->second.name.data()),
                    strnlen(reinterpret_cast<const char*>(main->second.name.data()), main->second.name.size()));
    const std::string event = "Nestvane: ended cached app " + name + " (pid " + std::to_string(id) + ", adj " +
        std::to_string(victim->oomScoreAdjustment) + ") to make room for a new process";
    if (androidLogObserver) androidLogObserver(event);
    keepDiagnosticLine(event);
    recallNativeThreads(victim);
    finishProcess(*victim, 9, true);
    std::erase_if(threads, [&](const auto& task) { return task.first != currentTid && task.second.process->id == id; });
    return true;
}

void ProbeLinux::finishProcess(Process& process, uint32_t status, bool killed) {
    if (current->process.get() == &process) accountGuestCpu();
    processes.erase(process.id);
    *binderDeathsDirty = true;
    const uint32_t code = killed ? 2 : 1; // CLD_KILLED, CLD_EXITED
    const auto uid = process.userIds[0];
    const auto parent = process.parent;
    if (parent) {
        childExits.emplace(process.id, ChildExit{parent, status, uid, code, process.cpuTime + process.reapedCpuTime});
        for (auto& [tid, task] : threads) {
            if (task.process->id != parent || task.wait == Wait::exited) continue;
            const auto handler = task.process->signalActions[sigChild][0];
            if (!(task.signalMask & sigChildMask) && handler <= 1) continue;
            if (handler != 1 && !task.process->childSignal)
                task.process->childSignal = Process::ChildSignal{uint32_t(process.id), uid, status, code};
            // A thread waiting in rt_sigtimedwait for this signal asks again, and now finds it.
            for (auto& [waitingTid, waiting] : threads) {
                if (waiting.process->id == parent && waiting.wait == Wait::signal && (waiting.signalWaitMask & sigChildMask)) {
                    waiting.wait = Wait::none;
                    waiting.deadline.reset();
                }
            }
            break;
        }
    }
    for (const auto& [fd, entry] : process.descriptors) {
        // The process's own descriptors that share the description go with it.
        const auto sharing = std::ranges::count_if(process.descriptors, [&](const auto& other) { return other.second.offset == entry.offset; });
        releaseRecordLocks(entry, process.id, sharing);
    }
    process.descriptors.clear();
    wakePollers();
}

void ProbeLinux::collectNative(Machine& machine) {
    uint64_t ticket = 0;
    while (native::finished(ticket)) {
        native::Result result;
        const auto known = nativeTickets.find(ticket);
        const auto found = known == nativeTickets.end() ? threads.end() : threads.find(known->second);
        if (known != nativeTickets.end()) nativeTickets.erase(known);
        if (found == threads.end() || found->second.nativeTicket != ticket) {
            native::take(ticket, nullptr, result);  // the thread is gone
            continue;
        }
        auto& thread = found->second;
        native::take(ticket, found->first == currentTid ? &machine.cpu : &thread.saved, result);
        thread.nativeResult = result;
        thread.nativeTicket = 0;
        thread.wait = Wait::none;
    }
}

void ProbeLinux::recallNativeThreads(const Process* process) {
    for (auto& [tid, thread] : threads) {
        if (thread.wait != Wait::native || (process && thread.process.get() != process)) continue;
        native::recall(thread.nativeTicket, true);
        // No thread that is on a worker is the current one here: the current one is in a system call.
        native::Result result;
        if (native::take(thread.nativeTicket, &thread.saved, result)) thread.nativeResult = result;
        nativeTickets.erase(thread.nativeTicket);
        thread.nativeTicket = 0;
        thread.wait = Wait::none;
    }
}

bool ProbeLinux::heldApp(const Thread& thread) const {
    return thread.process->context.starts_with("u:r:untrusted_app");
}

bool ProbeLinux::currentIsApp() const { return heldApp(*current); }

bool ProbeLinux::appsHeldNow() const {
    return appsHeld.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < appsHeldUntil;
}

bool ProbeLinux::offloadNative(Machine& machine) {
    if (!current->cpuBound || !native::workers() || !current->pendingSignals.empty() ||
        !current->process->pendingSignals.empty()) return false;
    // A held app does not get a core of its own either: on workers it took them from Android's start.
    if (heldApp(*current) && appsHeldNow()) return false;
    const uint64_t ticket = nextNativeTicket++;
    if (!native::offload(machine, ticket)) return false;
    current->nativeTicket = ticket;
    nativeTickets[ticket] = currentTid;
    current->wait = Wait::native;
    ++nativeOffloads;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::offloadInterpreter(Machine& machine, const SpaceCode& code) {
    // NESTVANE_NO_INTERPRET_WORKERS=1 keeps interpreting on the main loop, for comparison.
    static const bool allowed = !std::getenv("NESTVANE_NO_INTERPRET_WORKERS");
    if (!allowed || !native::workers() || !current->pendingSignals.empty() || !current->process->pendingSignals.empty() ||
        (heldApp(*current) && appsHeldNow()))
        return false;
    // A thread whose runs there keep ending at once (its code calls out all the time) stays, mostly.
    if (current->briefInterpreted >= 4) {
        --current->briefInterpreted;
        return false;
    }
    const uint64_t ticket = nextNativeTicket++;
    if (!native::offloadInterpreter(machine, ticket, code)) return false;
    current->nativeTicket = ticket;
    nativeTickets[ticket] = currentTid;
    current->wait = Wait::native;
    ++nativeOffloads;
    sliceEnd = 0;
    return true;
}

std::optional<native::Result> ProbeLinux::takeNativeResult() {
    auto result = std::exchange(current->nativeResult, std::nullopt);
    if (result && result->interpreter) {
        current->briefInterpreted = result->interpreted < 20000 ? current->briefInterpreted + 1 : 0;
        return result;
    }
    if (result) {
        // A thread that keeps coming back at once for a system call is better off without the detour.
        current->briefRuns = result->brief ? current->briefRuns + 1 : 0;
        if (current->briefRuns >= 4) {
            current->cpuBound = false;
            current->briefRuns = 0;
        }
    }
    return result;
}

void ProbeLinux::removeSiblingThreads() {
    recallNativeThreads(current->process.get());
    // Keep the current task until the scheduler saves its address space and advances.
    std::erase_if(threads, [&](const auto& entry) {
        return entry.first != currentTid && entry.second.process == current->process;
    });
}

bool ProbeLinux::exitThread(Machine& machine) {
    const bool others = std::any_of(threads.begin(), threads.end(), [&](const auto& task) {
        return task.first != currentTid && task.second.wait != Wait::exited;
    });
    if (current->clearTid) {
        const uint32_t zero = 0;
        if (writeGuest(machine, current->clearTid, zero))
            wakeFutex(current->clearTid & 0x00ff'ffff'ffff'ffff, 1, UINT32_MAX);
    }
    if (!others) return false; // The runner handles exit of the last task.
    if (std::none_of(threads.begin(), threads.end(), [&](const auto& task) {
            return task.first != currentTid && task.second.process == current->process &&
                   task.second.wait != Wait::exited;
        }))
        finishProcess(*current->process, uint32_t(machine.cpu.x[0]) & 255, false);
    else
        accountGuestCpu();
    current->wait = Wait::exited;
    sliceEnd = 0;
    return true;
}

ProbeLinux::Process* ProbeLinux::liveProcess(uint64_t id) const {
    const auto found = processes.find(id);
    return found == processes.end() ? nullptr : found->second;
}

static bool writeGuestRusage(Machine& machine, uint64_t address, std::chrono::nanoseconds cpuTime) {
    // Guest work runs on one host CPU thread. Charge its work as user CPU time.
    // Guest system CPU time and the other Linux rusage counters are unaccounted.
    std::array<int64_t, 18> usage{};
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(cpuTime).count();
    usage[0] = microseconds / 1'000'000;
    usage[1] = microseconds % 1'000'000;
    return writeGuest(machine, address, usage);
}

bool ProbeLinux::waitChild(Machine& machine) {
    auto& x = machine.cpu.x;
    constexpr uint64_t noHang = 1, untraced = 2, exited = 4, continued = 8, noWait = 0x1000000;
    const bool wait4 = x[8] == 260;
    int32_t target = wait4 ? int32_t(x[0]) : x[0] == 1 ? int32_t(x[1]) : -1;
    if (wait4 ? ((target <= 0 && target != -1) || (x[2] & ~(noHang | untraced | continued))) :
                (x[0] == 2 || x[0] > 3 || (x[3] & ~(noHang | exited | noWait)))) return false;
    if (!wait4 && x[0] == 3) { // P_PIDFD
        const auto fd = descriptors().find(uint32_t(x[1]));
        if (fd == descriptors().end() || fd->second.node != Node::processHandle) {
            x[0] = uint64_t(-9);
            return true;
        }
        target = fd->second.targetProcess;
    }
    if (!wait4 && !(x[3] & exited)) { x[0] = invalid; return true; }
    const auto matches = [&](uint64_t child, uint64_t parent) {
        return parent == processId() && (target == -1 || child == uint32_t(target));
    };
    const auto child = std::find_if(childExits.begin(), childExits.end(), [&](const auto& entry) {
        return matches(entry.first, entry.second.parent);
    });
    if (child == childExits.end()) {
        const bool hasChild = std::any_of(threads.begin(), threads.end(), [&](const auto& task) {
            return task.second.wait != Wait::exited &&
                   matches(task.second.process->id, task.second.process->parent);
        });
        if (!hasChild) { x[0] = uint64_t(-10); return true; }
        if (!((wait4 ? x[2] : x[3]) & noHang)) return waitForEvents();
    }
    const auto usage = wait4 ? x[3] : x[4];
    if (child != childExits.end() && usage && !writeGuestRusage(machine, usage, child->second.cpuTime)) {
        x[0] = uint64_t(-14);
        return true;
    }
    const auto reap = [&] {
        current->process->reapedCpuTime += child->second.cpuTime;
        childExits.erase(child);
    };
    if (wait4) {
        if (child == childExits.end()) { x[0] = 0; return true; }
        const auto status = child->second.code == 1 ? child->second.status << 8 : child->second.status;
        if (x[1] && !writeGuest(machine, x[1], status)) { x[0] = uint64_t(-14); return true; }
        x[0] = child->first;
        reap();
        return true;
    }
    std::array<uint32_t, 32> info{}; // Linux AArch64 siginfo_t, 128 bytes.
    if (child != childExits.end()) {
        info[0] = sigChild;
        info[2] = child->second.code;
        info[4] = uint32_t(child->first);
        info[5] = child->second.uid;
        info[6] = child->second.status;
    }
    if (x[2] && !writeGuest(machine, x[2], info)) { x[0] = uint64_t(-14); return true; }
    if (child != childExits.end() && !(x[3] & noWait)) reap();
    x[0] = 0;
    return true;
}

// A guest fsync commits the whole data volume to the host's store, about 10 ms each, and a boot
// asks for hundreds. Commits are at least two seconds apart: one that comes sooner is left to the next
// request or to the scheduler's next idle moment. The store always holds one whole earlier state of
// the volume, so what a stop in between loses is the last two seconds, as after a power cut.
bool ProbeLinux::syncDataSoon() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastDataSync < std::chrono::seconds(2)) {
        dataSyncPending = true;
        return true;
    }
    dataSyncPending = false;
    const bool done = syncData();
    lastDataSync = std::chrono::steady_clock::now();
    return done;
}

bool ProbeLinux::waitForEvents() {
    if (std::exchange(hostSocketWait, false)) return waitForHostEvents();
    current->waitingEpoll.reset();
    current->wait = Wait::poll;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::waitForHostEvents() {
    // The renderer answers from a thread of its own, usually within tens of microseconds, and there is
    // no notification for it: look again soon, and less often the longer the answer takes. A fixed
    // 2 ms here made every round trip to the renderer cost that much.
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (!current->hostWaitBegan || now - current->hostWaitLast > milliseconds(4)) current->hostWaitBegan = now;
    current->hostWaitLast = now;
    const auto waited = duration_cast<microseconds>(now - *current->hostWaitBegan);
    current->hostEventRetry = now + std::clamp(waited / 4, microseconds(20), microseconds(2000));
    return waitForEvents();
}

bool ProbeLinux::waitForSocketTimeout(std::chrono::microseconds timeout) {
    using namespace std::chrono;
    if (timeout.count() < 0) return false;
    if (timeout.count()) {
        const auto now = steady_clock::now();
        if (!current->deadline) {
            const auto maximum = steady_clock::time_point::max();
            current->deadline = timeout >= duration_cast<microseconds>(maximum - now) ? maximum : now + timeout;
        }
        if (*current->deadline <= now) return false;
    }
    return waitForEvents();
}

void ProbeLinux::EventState::queueFileEvent(FileEvent event) {
    constexpr size_t capacity = 256;
    if (fileEvents.size() > capacity || (!fileEvents.empty() && fileEvents.back() == event)) return;
    if (fileEvents.size() == capacity) event = {-1, fileWatchOverflow, 0};
    fileEvents.push_back(std::move(event));
    counter = fileEvents.size();
}

void ProbeLinux::fileEvent(std::string_view path, uint32_t mask, bool directory, uint32_t cookie) {
    if (inotifyInstances.empty() || !path.starts_with('/')) return;
    const auto separator = path.rfind('/');
    const auto parent = path.substr(0, separator ? separator : 1);
    const auto name = path.substr(separator + 1);
    bool notify = false;
    for (const auto& weak : inotifyInstances) {
        const auto instance = weak.lock();
        if (!instance) continue;
        for (auto watch = instance->fileWatches.begin(); watch != instance->fileWatches.end();) {
            const auto& config = watch->second;
            const bool same = config.path == path;
            const bool child = config.directory && config.path == parent && !same && mask != fileMovedSelf;
            const auto event = same && mask == fileDeleted ? fileDeletedSelf : mask;
            const bool parentOnly = mask == fileCreated || mask == fileMovedFrom || mask == fileMovedTo;
            const bool triggered = (child || (same && !parentOnly)) && (config.mask & event);
            if (triggered) {
                instance->queueFileEvent({watch->first, event | (directory ? fileWatchDirectory : 0),
                                           same ? 0 : cookie, child ? std::string(name) : std::string{}});
                notify = true;
            }
            if ((same && mask == fileDeleted) || (triggered && (config.mask & fileWatchOnce))) {
                instance->queueFileEvent({watch->first, fileWatchIgnored, 0});
                watch = instance->fileWatches.erase(watch);
                notify = true;
            } else {
                ++watch;
            }
        }
    }
    if (notify) wakePollers();
}

std::string ProbeLinux::sharedHost(std::string_view backing) const {
    if (sharedHostRoot.empty()) return {};
    if (backing == sharedGuestRoot) return sharedHostRoot;
    return inside(backing, sharedGuestRoot) ? sharedHostRoot + std::string(backing.substr(sharedGuestRoot.size())) : std::string{};
}

void ProbeLinux::sharedFileCreated(const std::string& backing, WritableFile& file) {
    const auto host = sharedHost(backing);
    if (host.empty()) return;
    const int fd = ::open(host.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;  // it stays a file of the database, and moves here when the data is next mounted
    ::close(fd);
    file.storage->pages.attachExact(host, 0);
}

void ProbeLinux::sharedFileRemoved(const std::string& backing) {
    const auto found = writableFiles.find(backing);
    if (found == writableFiles.end() || !found->second->storage->pages.exact()) return;
    auto& storage = *found->second->storage;
    const std::string host = storage.pages.backingFile();
    // Whoever still has the file open or mapped keeps its content, in memory.
    if (found->second.use_count() > 1 || found->second->storage.use_count() > 1) {
        storage.pages.unback();
        if (storage.placedMoves != storage.pages.moves) storage.replace();
    }
    ::unlink(host.c_str());
}

void ProbeLinux::sharedMoved(const std::string& source, const std::string& destination) {
    const auto from = sharedHost(source), to = sharedHost(destination);
    if (from.empty() && to.empty()) return;
    const auto each = [&](auto& nodes, const auto& visit) {
        if (const auto root = nodes.find(destination); root != nodes.end()) visit(root->first, root->second);
        for (auto it = nodes.lower_bound(destination + "/"); it != nodes.end() && inside(it->first, destination); ++it)
            visit(it->first, it->second);
    };
    const auto placedAgain = [](SharedFileBytes& storage) {
        if (storage.placedMoves != storage.pages.moves) storage.replace();
    };
    if (!from.empty() && !to.empty()) {
        ::rename(from.c_str(), to.c_str());
        each(writableFiles, [&](const std::string& path, const std::shared_ptr<WritableFile>& file) {
            if (file->storage->pages.exact()) file->storage->pages.renameFile(sharedHost(path), false);
        });
    } else if (!to.empty()) {
        each(directories, [&](const std::string& path, const Directory&) { ::mkdir(sharedHost(path).c_str(), 0755); });
        each(writableFiles, [&](const std::string& path, const std::shared_ptr<WritableFile>& file) {
            if (!file->persistent) return;
            file->storage->pages.makeExact(sharedHost(path), file->storage->size);
            placedAgain(*file->storage);
        });
    } else {
        each(writableFiles, [&](const std::string&, const std::shared_ptr<WritableFile>& file) {
            auto& pages = file->storage->pages;
            if (!pages.exact()) return;
            if (!dataFilesDirectory.empty() && pages.renameFile(externalFile(file->inode), true)) pages.leaveExact();
            else pages.unback();
            placedAgain(*file->storage);
        });
        std::error_code ignored;
        std::filesystem::remove_all(from, ignored);
    }
}

void ProbeLinux::fileMoved(std::string_view source, std::string_view destination, bool directory) {
    if (inotifyInstances.empty()) return;
    if (!nextMoveCookie) ++nextMoveCookie;
    const auto cookie = nextMoveCookie++;
    fileEvent(source, fileMovedFrom, directory, cookie);
    fileEvent(destination, fileMovedTo, directory, cookie);
    fileEvent(source, fileMovedSelf, directory);
    for (const auto& weak : inotifyInstances) {
        if (const auto instance = weak.lock()) {
            for (auto& [id, watch] : instance->fileWatches) {
                if (watch.path == source || (directory && inside(watch.path, source)))
                    watch.path = std::string(destination) + watch.path.substr(source.size());
            }
        }
    }
}

bool ProbeLinux::inotify(Machine& machine) {
    auto& x = machine.cpu.x;
    if (x[8] == 26) {
        if (x[0] & ~uint64_t{0x80800}) { x[0] = invalid; return true; }
        const auto fd = freeDescriptor();
        if (fd == descriptorCapacity) { x[0] = uint64_t(-24); return true; }
        std::erase_if(inotifyInstances, [](const auto& instance) { return instance.expired(); });
        if (inotifyInstances.size() >= 64) { x[0] = uint64_t(-24); return true; }
        Descriptor entry{"anon_inode:inotify", {}, uint32_t(x[0]), Node::inotify};
        entry.event = std::make_shared<EventState>();
        inotifyInstances.push_back(entry.event);
        descriptors().emplace(fd, std::move(entry));
        x[0] = fd;
        return true;
    }
    const auto fd = descriptors().find(uint32_t(x[0]));
    if (fd == descriptors().end()) { x[0] = uint64_t(-9); return true; }
    if (fd->second.node != Node::inotify) { x[0] = invalid; return true; }
    auto& instance = *fd->second.event;
    if (x[8] == 28) {
        const auto watch = int32_t(x[1]);
        if (!instance.fileWatches.erase(watch)) { x[0] = invalid; return true; }
        instance.queueFileEvent({watch, fileWatchIgnored, 0});
        wakePollers();
        x[0] = 0;
        return true;
    }
    constexpr uint32_t events = 0xfff, onlyDir = 0x01000000, noFollow = 0x02000000,
                       excludeUnlinked = 0x04000000, createWatch = 0x10000000, addMask = 0x20000000;
    const auto mask = uint32_t(x[2]);
    if (!(mask & events) || (mask & ~(events | onlyDir | noFollow | excludeUnlinked |
                                     createWatch | addMask | fileWatchOnce)) ||
        ((mask & createWatch) && (mask & addMask))) { x[0] = invalid; return true; }
    const auto path = readPath(machine, x[1]);
    if (!path) { x[0] = uint64_t(-14); return true; }
    if (path->empty()) { x[0] = uint64_t(-2); return true; }
    const auto logical = followLinks(absolutePath(*path), !(mask & noFollow));
    if (!logical) { x[0] = uint64_t(-40); return true; }
    const auto watched = backingPath(*logical);
    const auto entry = find(watched, 0);
    if (!entry && !((mask & noFollow) && resolve(*logical))) { x[0] = uint64_t(-2); return true; }
    const bool directory = entry && entry->node == Node::directory;
    if ((mask & onlyDir) && !directory) { x[0] = uint64_t(-20); return true; }
    if (!canAccessParent(watched, false)) { x[0] = uint64_t(-13); return true; }
    if (userId()) {
        const auto dir = directories.find(watched);
        if ((dir != directories.end() && !inodeAllows(dir->second, 4)) ||
            (entry && entry->temporary &&
             !inodeAllows(*entry->temporary, 4))) {
            x[0] = uint64_t(-13);
            return true;
        }
    }
    const auto existing = std::find_if(instance.fileWatches.begin(), instance.fileWatches.end(),
        [&](const auto& watch) { return watch.second.path == watched; });
    const auto requested = mask & (events | excludeUnlinked | fileWatchOnce);
    if (existing == instance.fileWatches.end()) {
        if (instance.fileWatches.size() >= 256 || instance.nextFileWatch == INT32_MAX) {
            x[0] = uint64_t(-28);
            return true;
        }
        x[0] = instance.nextFileWatch++;
        instance.fileWatches.emplace(int32_t(x[0]), EventState::FileWatch{watched, requested, directory});
    } else {
        if (mask & createWatch) { x[0] = uint64_t(-17); return true; }
        existing->second.mask = (mask & addMask ? existing->second.mask : 0) | requested;
        x[0] = existing->first;
    }
    return true;
}

bool ProbeLinux::readInotify(Machine& machine, const Descriptor& fd) {
    auto& x = machine.cpu.x;
    if (x[8] == 67) { x[0] = uint64_t(-29); return true; }
    auto& instance = *fd.event;
    if (instance.fileEvents.empty()) {
        if (!(fd.effectiveFlags() & 0x800)) return waitForEvents();
        x[0] = uint64_t(-11);
        return true;
    }
    std::vector<uint8_t> bytes;
    size_t count = 0;
    for (const auto& event : instance.fileEvents) {
        const auto length = event.name.empty() ? 0 : (event.name.size() + 16) & ~size_t{15};
        const std::array<uint32_t, 4> header{uint32_t(event.watch), event.mask, event.cookie, uint32_t(length)};
        if (sizeof header + length > x[2] - bytes.size()) break;
        const auto start = bytes.size();
        bytes.resize(start + sizeof header + length);
        std::memcpy(bytes.data() + start, header.data(), sizeof header);
        if (length) std::memcpy(bytes.data() + start + sizeof header, event.name.data(), event.name.size());
        ++count;
    }
    if (!count) { x[0] = invalid; return true; }
    if (!machine.write(x[1], bytes)) { x[0] = uint64_t(-14); return true; }
    instance.fileEvents.erase(instance.fileEvents.begin(), instance.fileEvents.begin() + count);
    instance.counter = instance.fileEvents.size();
    x[0] = bytes.size();
    return true;
}

void ProbeLinux::wakePollers(const EventState* changedEvent, bool binderOnly, const SocketState* readableSocket) {
    for (auto& [tid, thread] : threads) {
        if (thread.wait != Wait::poll) continue;
        if (readableSocket) {
            const auto poller = thread.waitingEpoll.lock();
            // A Unix socket send makes only its peer readable.
            if (poller && std::ranges::none_of(poller->watches, [readableSocket](const auto& entry) {
                    return entry.second.socket.lock().get() == readableSocket;
                })) continue;
        }
        if (changedEvent) {
            const auto poller = thread.waitingEpoll.lock();
            // Epoll waiters only need to recheck their own watch list when an
            // individual timer or eventfd changes. Other wait APIs can share sources.
            if (poller && poller.get() != changedEvent &&
                std::ranges::none_of(poller->watches, [changedEvent](const auto& entry) {
                    return entry.second.source.lock().get() == changedEvent;
                })) continue;
        }
        if (binderOnly) {
            const auto poller = thread.waitingEpoll.lock();
            if (poller && std::ranges::none_of(poller->watches, [&](const auto& entry) {
                    if (!(entry.second.mask & 1)) return false;
                    const auto binder = entry.second.binder.lock();
                    return binder && binderReadable(*binder, thread.process->id, tid);
                })) continue;
        }
        if (auto binder = thread.waitingBinder.lock()) {
            const auto& files = thread.process->descriptors;
            const auto fd = files.find(thread.waitingBinderFd);
            // A direct Binder ioctl needs queued work or a change to its descriptor.
            if (fd != files.end() && fd->second.binder == binder &&
                !binderReadable(*binder, thread.process->id, tid)) continue;
        }
        thread.wait = Wait::none;
    }
}

bool ProbeLinux::poll(Machine& machine) {
    auto& x = machine.cpu.x;
    if (x[3]) return false; // Temporary signal-mask swap.
    if (x[1] > descriptorLimit()) { x[0] = invalid; return true; }
    using namespace std::chrono;
    if (x[2] && !current->deadline) {
        std::array<int64_t, 2> timeout{};
        std::vector<uint8_t> dataScratch;
        const auto* data = machine.read(x[2], sizeof(timeout), 4, dataScratch);
        if (!data) { x[0] = uint64_t(-14); return true; }
        std::memcpy(timeout.data(), data, sizeof(timeout));
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1'000'000'000) { x[0] = invalid; return true; }
        const auto now = steady_clock::now();
        const auto available = duration_cast<nanoseconds>(steady_clock::time_point::max() - now);
        if (timeout[0] > (available.count() - timeout[1]) / 1'000'000'000) {
            current->deadline = steady_clock::time_point::max();
        } else {
            current->deadline = now + duration_cast<steady_clock::duration>(
                seconds(timeout[0]) + nanoseconds(timeout[1]));
        }
    }
    const auto complete = [&](uint64_t result) {
        x[0] = result;
        current->deadline.reset();
        return true;
    };
    struct PollFD {
        int32_t fd;
        int16_t events, returned;
    };
    static_assert(sizeof(PollFD) == 8);
    std::vector<PollFD> entries(x[1]);
    const size_t length = entries.size() * sizeof(PollFD);
    if (length) {
        std::vector<uint8_t> inputScratch;
        const auto* input = machine.read(x[0], length, 4, inputScratch);
        if (!input) return complete(uint64_t(-14));
        std::memcpy(entries.data(), input, length);
    }
    unsigned ready = 0;
    bool watchesHostEvents = false;
    for (auto& entry : entries) {
        entry.returned = 0;
        if (entry.fd < 0) continue;
        const auto descriptor = descriptors().find(entry.fd);
        if (descriptor == descriptors().end()) {
            entry.returned = 0x20;
        } else if (descriptor->second.node == Node::processHandle) {
            const auto target = descriptor->second.targetProcess;
            if (!processAlive(target))
                entry.returned = (entry.events & 0x41) | (childExits.contains(target) ? 0 : 16);
        } else if (const auto& graphics = descriptor->second.graphics) {
            watchesHostEvents = true;
            entry.returned = graphics->pollEvents() & (entry.events | 0x18);
        } else if (const auto& touch = descriptor->second.touch) {
            watchesHostEvents = true;
            entry.returned = touch->ready() ? entry.events & 1 : 0;
        } else if (descriptor->second.node == Node::sound) {
            // The playback device takes more when there is the room its user asked to wait for; the card's control device
            // never has anything to report.
            watchesHostEvents = true;
            entry.returned = descriptor->second.path.ends_with("p") && soundRoom() >= soundAvailableMinimum ? entry.events & 0x104 :
                descriptor->second.path.ends_with("D0c") && microphoneAvailable() >= microphoneAvailableMinimum ? entry.events & 0x41 : 0;
        } else if (descriptor->second.pty) {
            const auto events = ptyPoll(descriptor->second);
            entry.returned = (events & entry.events) | (events & 16);
        } else if (const auto pipe = descriptor->second.pipe) {
            entry.returned = descriptor->second.pipeOtherEnd
                ? (pipe->pollEvents(true) | pipe->pollEvents(false)) & entry.events
                : pipe->pollEvents(descriptor->second.node == Node::pipeRead) & (entry.events | 0x18);
        } else if (const auto& map = descriptor->second.bpfMap) {
            entry.returned = map->type == BpfMap::ringBuffer && map->ringReadable() ? entry.events & 0x41 : 0;
        } else if (descriptor->second.node == Node::timer) {
            refreshTimers(steady_clock::now());
            entry.returned = entry.events & (descriptor->second.event->counter ? 1 : 0);
        } else if (descriptor->second.node == Node::inotify) {
            entry.returned = entry.events & (descriptor->second.event->counter ? 1 : 0);
        } else if (descriptor->second.node == Node::event) {
            const auto count = descriptor->second.event->counter;
            entry.returned = entry.events & ((count ? 1 : 0) | (count < UINT64_MAX - 1 ? 4 : 0));
        } else if (descriptor->second.binder) {
            entry.returned = entry.events & (binderReadable(*descriptor->second.binder, processId(), currentTid) ? 1 : 0);
        } else if (const auto carried = descriptor->second.socket; carried && carried->hostFd >= 0) {
            watchesHostEvents = true;
            const int host = hostSocketEvents(*carried);
            entry.returned = (entry.events & host & 5) | (host & 0x18);
        } else if (const auto endpoint = descriptor->second.socket) {
            const auto peer = endpoint->peer.lock();
            const bool receiveEnded = endpoint->receiveShutdown ||
                (!endpoint->netlink && !endpoint->inetDatagram() && endpoint->connected && (!peer || peer->sendShutdown));
            const bool readable = endpoint->hasInput() || !endpoint->pending.empty() ||
                ((peer || endpoint->family == 1) && receiveEnded);
            const bool writable = endpoint->tcpConnectFailed ||
                (endpoint->netlink && endpoint->protocol != 15) ||
                (endpoint->inetDatagram() && !endpoint->sendShutdown) ||
                (peer && !endpoint->sendShutdown && !peer->receiveShutdown &&
                 peer->queuedBytes < endpoint->sendBuffer && peer->inbox.size() < 256);
            const bool hungUp = endpoint->tcpConnectFailed ||
                (!endpoint->netlink && !endpoint->inetDatagram() && endpoint->connected &&
                 (!peer || (receiveEnded && endpoint->sendShutdown)));
            entry.returned = (entry.events & ((readable ? (endpoint->family == 1 ? 0x41 : 1) : 0) | (writable ? 4 : 0) |
                                              (receiveEnded ? 0x2000 : 0))) | (hungUp ? 16 : 0) | (endpoint->inetError ? 8 : 0);
        } else {
            switch (descriptor->second.node) {
                case Node::input: case Node::output: case Node::error: case Node::null:
                case Node::regular: case Node::temporary: case Node::directory: case Node::entropy:
                    entry.returned = entry.events & 0x145; // Linux DEFAULT_POLLMASK.
                    break;
                default: return false;
            }
        }
        ready += entry.returned != 0;
    }
    if (!ready && (!current->deadline || *current->deadline > steady_clock::now()))
        return watchesHostEvents ? waitForHostEvents() : waitForEvents();
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(entries.data()), length);
    return complete(!length || machine.write(x[0], bytes) ? ready : uint64_t(-14));
}

std::optional<std::pair<std::string_view, uint64_t>> ProbeLinux::guestCodeFile(const uint8_t* instruction) const {
    const auto pointer = reinterpret_cast<uintptr_t>(instruction);
    const auto offsetIn = [pointer](std::span<const uint8_t> bytes) -> std::optional<uint64_t> {
        const auto base = reinterpret_cast<uintptr_t>(bytes.data());
        if (pointer < base || pointer - base >= bytes.size()) return {};
        return pointer - base;
    };
    // An APEX file view can overlap its containing image. Prefer the file inside the APEX.
    for (const auto& [path, node] : ext4Nodes) {
        const auto bytes = node->view.empty() ? std::span<const uint8_t>(node->owned) : node->view;
        if (const auto offset = offsetIn(bytes)) return std::pair{std::string_view(path), *offset};
    }
    for (const auto& [path, bytes] : files)
        if (const auto offset = offsetIn(bytes)) return std::pair{std::string_view(path), *offset};
    return {};
}

std::string ProbeLinux::guestCodeFileName(std::string_view path) const {
    if (path.size() < 2 || path[1] != '\0') return std::string(path);
    const auto separator = path.find('/', 2);
    const auto backing = path.substr(0, separator);
    const auto relative = separator == std::string_view::npos ? std::string{} : std::string(path.substr(separator));
    for (const auto& [source, mount] : mountedExt4)
        if (mount.backing == backing) return std::get<0>(source) + ':' + relative;
    return relative;
}

// The host memory behind each guest process's address space, largest first.
std::string ProbeLinux::memoryReport(const Machine& machine) const {
    struct Row { uint64_t pid; std::string name; GuestSpace::Usage usage; size_t runs; };
    std::vector<Row> rows;
    GuestSpace::Usage total;
    for (const auto& [id, process] : processes) {
        const auto* space = process == current->process.get() ? machine.space.get() : process->space.get();
        if (!space) continue;
        rows.push_back({id, process->executable, space->usage(), space->runCount()});
        total.resident += rows.back().usage.resident;
        total.dirty += rows.back().usage.dirty;
        total.swapped += rows.back().usage.swapped;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.usage.dirty + a.usage.swapped > b.usage.dirty + b.usage.swapped;
    });
    std::ostringstream out;
    constexpr uint64_t MiB = 1024 * 1024;
    out << "  guest address spaces: " << rows.size() << ", MiB resident " << total.resident / MiB << ", dirty "
        << total.dirty / MiB << ", compressed or swapped " << total.swapped / MiB << "\n";
    for (size_t index = 0; index < rows.size() && index < 16; ++index)
        out << "    pid " << rows[index].pid << " " << rows[index].name << ": resident " << rows[index].usage.resident / MiB
            << ", dirty " << rows[index].usage.dirty / MiB << ", swapped " << rows[index].usage.swapped / MiB
            << " MiB in " << rows[index].runs << " runs\n";
    // NESTVANE_MEMORY_DETAIL=<part of a program's path>: the mappings of the first such process.
    if (const char* wanted = std::getenv("NESTVANE_MEMORY_DETAIL")) {
        for (const auto& [id, process] : processes) {
            if (std::string_view(wanted).starts_with("pid:") ? std::to_string(id) != wanted + 4
                                                             : process->executable.find(wanted) == std::string::npos) continue;
            const bool running = process == current->process.get();
            const auto* space = running ? machine.space.get() : process->space.get();
            if (!space) continue;
            out << "    mappings of pid " << id << " " << process->executable << " (KiB dirty of KiB mapped):\n";
            for (const auto& segment : running ? machine.segments : process->segments) {
                const auto usage = space->usage(segment.address, (segment.mappedSize() + GuestSpace::page - 1) & ~(GuestSpace::page - 1));
                if (!usage.dirty) continue;
                out << "      0x" << std::hex << segment.address << std::dec << " " << usage.dirty / 1024 << " of " << segment.mappedSize() / 1024
                    << " flags=" << segment.flags << (segment.anonymous ? " anonymous" : " file") << (segment.shared ? " shared" : "")
                    << (segment.initialStack ? " stack" : "") << "\n";
            }
            break;
        }
    }
    return out.str();
}

std::string ProbeLinux::workerReport(const Machine& machine, uint64_t observedProcess) const {
    std::string report;
    for (const auto& [path, tid] : propertyPollers) {
        // An abstract socket's name starts with a zero byte: shown with "@", as Linux tools show it.
        const std::string shown = path.starts_with('\0') ? "@" + std::string(path).substr(1) : std::string(path);
        report += "  guest worker " + std::to_string(tid) + " waited in epoll for " + shown + "\n";
    }
    std::ostringstream tasks;
    const auto snapshotTime = std::chrono::steady_clock::now();
    const auto segmentAt = [&](const Process& process, uint64_t address) -> const Segment* {
        const auto& segments = process.id == processId() ? machine.segments : process.segments;
        const auto next = std::upper_bound(segments.begin(), segments.end(), address,
            [](uint64_t pc, const Segment& segment) { return pc < segment.address; });
        return next == segments.begin() ? nullptr : &*std::prev(next);
    };
    const auto describeCode = [&](const Process& process, uint64_t address, const char* field) {
        const auto* segment = segmentAt(process, address);
        if (!segment || !segment->imageView || address - segment->address >= segment->mappedSize()) return;
        const auto location = guestCodeFile(segment->imageView + (address - segment->address));
        if (!location) return;
        const auto [path, offset] = *location;
        tasks << ' ' << field << '=' << guestCodeFileName(path) << "+0x" << std::hex << offset << std::dec;
    };
    const auto active = std::ranges::count_if(threads, [](const auto& entry) {
        return entry.second.wait != Wait::exited;
    });
    tasks << "  guest tasks: " << active << " active, " << threads.size()
          << " tracked, limit " << maxGuestTasks << '\n';
    std::set<const SharedFileBytes*> countedShared;
    std::set<const uint8_t*> countedDense;
    size_t privateBytes = 0, sharedBytes = 0;
    size_t anonymousDenseBytes = 0, fileDenseBytes = 0;
    const auto countShared = [&](const SharedFileBytes* storage) {
        if (storage && countedShared.insert(storage).second) sharedBytes += storage->pages.capacity();
    };
    std::set<uint64_t> countedProcesses;
    std::vector<const Process*> activeProcesses;
    for (const auto& [_, thread] : threads) {
        const auto& process = *thread.process;
        if (thread.wait == Wait::exited || !countedProcesses.insert(process.id).second) continue;
        activeProcesses.push_back(&process);
        for (const auto& area : process.id == processId() ? machine.segments : process.segments) {
            if (!area.bytes.empty() && countedDense.insert(area.bytes.data()).second) {
                const auto capacity = area.bytes.capacity();
                privateBytes += capacity;
                if (area.anonymous) anonymousDenseBytes += capacity;
                else if (area.fileMapping) fileDenseBytes += capacity;
            }
            countShared(area.shared.get());
        }
        for (const auto& [_, fd] : process.descriptors) {
            if (fd.temporary) countShared(fd.temporary->storage.get());
            if (fd.ashmem) countShared(fd.ashmem->file.storage.get());
            if (fd.binder) countShared(fd.binder->mapped.get());
        }
    }
    size_t persistentFiles = 0, transientFiles = 0, pinnedObjects = 0;
    for (const auto& [_, file] : writableFiles) {
        countShared(file->storage.get());
        if (file->pinnedBpfObject()) ++pinnedObjects;
        else if (file->persistent) ++persistentFiles;
        else ++transientFiles;
    }
    for (const auto& file : memoryFiles) countShared(file.lock().get());
    tasks << "  guest memory bytes: anonymous dirty=" << anonymousBudget->dirty
          << " dense private capacity=" << privateBytes << " shared capacity=" << sharedBytes
          << " anonymous reserved=" << anonymousBudget->reserved << '/' << anonymousBudget->reservedLimit << '\n';
    tasks << "  guest dense mapping bytes: anonymous=" << anonymousDenseBytes
          << " file backed=" << fileDenseBytes
          << " other=" << privateBytes - anonymousDenseBytes - fileDenseBytes << '\n';
    tasks << "  guest filesystem entries: persistent files=" << persistentFiles << '/' << maxPersistentFiles
          << " transient files=" << transientFiles << '/' << maxTransientFiles
          << " bpffs pins=" << pinnedObjects << '/' << maxBpfPins
          << " directories=" << directories.size() - ext4DirectoryCount << '/' << maxGuestDirectories << '\n';
    // NESTVANE_INIT_PROCESS_REPORT=<n> lists that many processes instead of the eight largest.
    const char* wanted = std::getenv("NESTVANE_INIT_PROCESS_REPORT");
    const auto top = activeProcesses.begin() + std::min<size_t>(activeProcesses.size(), wanted ? std::max(1, std::atoi(wanted)) : 8);
    std::partial_sort(activeProcesses.begin(), top, activeProcesses.end(),
                      [](const Process* a, const Process* b) { return a->cpuTime > b->cpuTime; });
    for (auto entry = activeProcesses.begin(); entry != top; ++entry) {
        const auto& process = **entry;
        const auto hostCPU = std::chrono::duration_cast<std::chrono::milliseconds>(process.cpuTime).count();
        tasks << "  guest process host CPU: " << hostCPU << " ms pid=" << process.id
              << ' ' << (process.programName.empty() ? process.executable : process.programName);
        // A Java process is known by the name its main thread was given, not by app_process.
        if (const auto main = threads.find(process.id); main != threads.end()) {
            const auto end = std::find(main->second.name.begin(), main->second.name.end(), uint8_t{0});
            tasks << " (" << std::string_view(reinterpret_cast<const char*>(main->second.name.data()),
                                              size_t(end - main->second.name.begin())) << ')';
        }
        tasks << '\n';
    }
    const auto threadName = [](const Thread& thread) -> std::string_view {
        const auto end = std::find(thread.name.begin(), thread.name.end(), uint8_t{0});
        return {reinterpret_cast<const char*>(thread.name.data()), size_t(end - thread.name.begin())};
    };
    const auto descriptorName = [](const Descriptor& descriptor) {
        if (!descriptor.visiblePath.empty()) return std::string_view(descriptor.visiblePath);
        if (descriptor.path.find('\0') == std::string::npos) return std::string_view(descriptor.path);
        return std::string_view("(guest namespace)");
    };
    for (const auto& [tid, thread] : threads) {
        const auto& process = *thread.process;
        if (thread.wait == Wait::exited) continue;
        const auto name = threadName(thread);
        const auto leader = threads.find(process.id);
        const bool launcherThread = (tid == process.id || name == "RenderThread") &&
            leader != threads.end() && threadName(leader->second) == "droid.launcher3";
        // NESTVANE_TASK_MATCH=<part of a program's path> lists that program's threads and no others.
        static const char* const matched = std::getenv("NESTVANE_TASK_MATCH");
        if (matched) {
            if (process.executable.find(matched) == std::string::npos) continue;
        } else
        if (thread.wait != Wait::suspended && process.id != observedProcess && !launcherThread &&
            process.executable != "/system/bin/bootanimation" &&
            process.executable != "/system/bin/surfaceflinger" &&
            !process.executable.starts_with("/vendor/bin/hw/android.hardware.graphics.") &&
            process.programName != "system_server" && process.context != "u:r:system_server:s0") continue;
        const auto& cpu = tid == currentTid ? machine.cpu : thread.saved;
        tasks << "  guest task " << process.id << '/' << tid << ' ' << process.executable << ' ' << name;
        switch (thread.wait) {
            case Wait::none: tasks << " runnable"; break;
            case Wait::poll: {
                tasks << " poll syscall=" << cpu.x[8] << " arg0=" << cpu.x[0];
                const auto fd = process.descriptors.find(uint32_t(cpu.x[0]));
                if (fd == process.descriptors.end()) break;
                if (cpu.x[8] == 22 && fd->second.node == Node::poller) {
                    tasks << " timeout=" << int32_t(cpu.x[3]) << " watches=[";
                    unsigned count = 0;
                    for (const auto& [watchedFd, watch] : fd->second.event->watches) {
                        if (count++) tasks << ", ";
                        if (count > 12) { tasks << "..."; break; }
                        const auto watched = process.descriptors.find(watchedFd);
                        tasks << watchedFd;
                        if (watched != process.descriptors.end()) {
                            tasks << ':' << descriptorName(watched->second);
                            if (auto endpoint = watch.socket.lock()) tasks << ":inbox=" << endpoint->inbox.size();
                        }
                        tasks << "/0x" << std::hex << watch.mask << std::dec;
                    }
                    tasks << ']';
                } else if (cpu.x[8] == 29 || cpu.x[8] == 63 || cpu.x[8] == 64) {
                    tasks << " fd=" << descriptorName(fd->second);
                    if (const auto& binder = fd->second.binder; cpu.x[8] == 29 && binder) {
                        const auto pending = binder->pendingCalls.find(tid);
                        if (pending != binder->pendingCalls.end()) {
                            const auto& call = pending->second.back();
                            tasks << " binder-target=" << call.pid << " code=" << call.code;
                            if (const auto target = call.target.lock()) {
                                for (const auto& [serverTid, stack] : target->replies) {
                                    if (std::ranges::any_of(stack, [&](const BinderRoute& route) {
                                            return route.thread == tid && route.connection.lock() == binder;
                                        })) tasks << " binder-server-tid=" << serverTid;
                                }
                                tasks << " target-queued=" << target->incoming.size();
                                if (!call.descriptor.empty()) tasks << " interface=" << call.descriptor;
                            } else {
                                tasks << " binder-target-closed";
                            }
                        }
                    }
                }
                break;
            }
            case Wait::futexPi:
            case Wait::futex:
                tasks << " futex=0x" << std::hex << thread.futexAddress << std::dec
                      << " op=" << cpu.x[1];
                if (thread.wait == Wait::futex) tasks << " expected=" << uint32_t(cpu.x[2]);
                if (thread.deadline) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        *thread.deadline - snapshotTime).count();
                    tasks << " deadline-ms=" << remaining;
                }
                break;
            case Wait::recordLock: tasks << " record lock held by PID " << thread.recordLockBlocker; break;
            case Wait::signal: tasks << " signal"; break;
            case Wait::signalSuspend: tasks << " sigsuspend"; break;
            case Wait::sleep: tasks << " sleeping"; break;
            case Wait::suspended:
                tasks << " suspended syscall=" << cpu.x[8] << std::hex
                      << " arg0=0x" << cpu.x[0] << " arg1=0x" << cpu.x[1]
                      << " arg2=0x" << cpu.x[2] << " arg3=0x" << cpu.x[3] << std::dec;
                if (cpu.x[8] == 29) {
                    const auto fd = process.descriptors.find(uint32_t(cpu.x[0]));
                    if (fd != process.descriptors.end()) tasks << " fd=" << descriptorName(fd->second);
                }
                break;
            case Wait::exited: break;
            case Wait::native: tasks << " native code on a worker"; break;
        }
        tasks << " PC=0x" << std::hex << cpu.pc << " LR=0x" << cpu.x[30] << std::dec;
        const bool handlingBinderCall = process.context == "u:r:system_server:s0" &&
            std::ranges::any_of(process.descriptors, [&](const auto& entry) {
                const auto& binder = entry.second.binder;
                if (!binder) return false;
                const auto outstanding = binder->replies.find(tid);
                return outstanding != binder->replies.end() && !outstanding->second.empty();
            });
        const bool stoppingThread = tid == currentTid && process.id == observedProcess && cpu.stop != Stop::running;
        if (stoppingThread || launcherThread || handlingBinderCall || (tid == process.id &&
            (process.id == observedProcess || process.context == "u:r:system_server:s0"))) {
            describeCode(process, cpu.pc, "pc-file");
            describeCode(process, cpu.x[30], "lr-file");
            // Native AArch64 frame records contain the previous FP and the saved LR.
            auto frame = cpu.x[29];
            for (unsigned depth = 0; depth < 12 && frame >= cpu.sp && frame - cpu.sp < 8 * 1024 * 1024; ++depth) {
                if (!frame || (frame & 15)) break;
                const auto* segment = segmentAt(process, frame);
                if (!segment || !(segment->flags & 4)) break;
                const auto offset = frame - segment->address;
                const auto size = segment->accessibleSize();
                std::array<uint64_t, 2> record;
                if (offset >= size || sizeof(record) > size - offset) break;
                if (segment->anonymousPages)
                    segment->anonymousPages->readInto(segment->fileOffset + offset,
                        {reinterpret_cast<uint8_t*>(record.data()), sizeof(record)});
                else
                    std::memcpy(record.data(), segment->data() + offset, sizeof(record));
                const auto [previous, returnAddress] = record;
                if (returnAddress < 4 || (returnAddress & 3)) break;
                const auto caller = returnAddress - 4;
                tasks << "\n    caller " << depth << " PC=0x" << std::hex << caller << std::dec;
                describeCode(process, caller, "pc-file");
                if (previous <= frame) break;
                frame = previous;
            }
        }
        if (tid == process.id && (launcherThread || process.id == observedProcess)) {
            std::set<const GraphicsPipeState*> reported;
            for (const auto& [fd, desc] : process.descriptors) {
                const auto* pipe = desc.graphics.get();
                if (!pipe || !pipe->renderChannel || !reported.insert(pipe).second) continue;
                tasks << "\n    OpenGL pipe fd=" << fd << " received=" << pipe->received << " sent=" << pipe->sent
                      << " read EAGAIN=" << pipe->readAgain << " write EAGAIN=" << pipe->writeAgain
                      << " last read=" << pipe->lastRead << " last write=" << pipe->lastWrite;
            }
        }
        tasks << '\n';
    }
    return report + tasks.str() + binderTrace;
}

uint64_t ProbeLinux::openPty(Descriptor& fd) {
    const bool slave = fd.node == Node::ptySlave;
    if (slave) {
        if (fd.pty->locked) return uint64_t(-5);
        if (userId() != 0 && userId() != fd.pty->owner) return uint64_t(-13);
        fd.pty->slaveOpened = true;
    } else {
        std::erase_if(ptys, [](const auto& entry) {
            const auto pty = entry.second.lock();
            return !pty || (pty->ends[0].expired() && pty->ends[1].expired());
        });
        if (ptys.size() >= 64) return uint64_t(-28);
        uint32_t number = 0;
        while (ptys.contains("/dev/pts/" + std::to_string(number))) ++number;
        fd.pty = std::make_shared<Pty>();
        fd.pty->number = number;
        fd.pty->owner = userId();
        ptys.emplace("/dev/pts/" + std::to_string(number), fd.pty);
    }
    fd.ptyEnd = fd.pty->ends[slave].lock();
    if (!fd.ptyEnd) {
        fd.ptyEnd = std::make_shared<int>(0);
        fd.pty->ends[slave] = fd.ptyEnd;
    }
    wakePollers();
    return 0;
}

uint16_t ProbeLinux::ptyPoll(const Descriptor& fd) {
    const bool slave = fd.node == Node::ptySlave;
    const auto& pty = *fd.pty;
    const bool hungUp = (slave || pty.slaveOpened) && pty.ends[!slave].expired();
    return (!pty.input[slave].empty() ? 1 : 0) |
        (hungUp ? 16 : pty.input[!slave].size() < 65536 ? 4 : 0);
}

uint64_t ProbeLinux::ptyIoctl(Machine& machine, const Descriptor& fd) {
    if (fd.pathOnly()) return uint64_t(-9);
    const auto& x = machine.cpu.x;
    auto& terminal = *fd.pty;
    const auto write = [&](const auto& value) { return writeGuest(machine, x[2], value) ? 0 : uint64_t(-14); };
    const auto read = [&](auto& value) -> uint64_t {
        std::vector<uint8_t> bytesScratch;
        const auto* bytes = machine.read(x[2], sizeof(value), 4, bytesScratch);
        if (!bytes) return uint64_t(-14);
        std::memcpy(&value, bytes, sizeof(value));
        return 0;
    };
    switch (x[1]) {
        case 0x80045430: // TIOCGPTN
            return fd.node == Node::ptyMaster ? write(terminal.number) : uint64_t(-25);
        case 0x40045431: { // TIOCSPTLCK
            if (fd.node != Node::ptyMaster) return uint64_t(-25);
            uint32_t lock = 0;
            if (auto error = read(lock); error) return error;
            terminal.locked = lock != 0;
            return 0;
        }
        case 0x5401: return write(terminal.termios); // TCGETS
        case 0x5402: case 0x5403: case 0x5404: return read(terminal.termios); // TCSETS*
        case 0x5413: return write(terminal.window); // TIOCGWINSZ
        case 0x5414: return read(terminal.window); // TIOCSWINSZ
        default: return uint64_t(-25);
    }
}

void ProbeLinux::addLoopDevice(unsigned number) {
    const auto name = "loop" + std::to_string(number);
    addDirectory("/dev/block", 0755);
    loopDevices.try_emplace("/dev/block/" + name).first->second.number = number;
    const auto sysPath = "/sys/devices/virtual/block/" + name;
    addDirectory(sysPath, 0555);
    ueventTriggers.emplace(sysPath + "/uevent", number);
    alias("/sys/block/" + name, sysPath);
    emitLoopUevent(number);
}

uint64_t ProbeLinux::ashmemIoctl(Machine& machine, const Descriptor& fd) {
    if (!fd.ashmem) return uint64_t(-9);
    auto& region = *fd.ashmem;
    const auto& x = machine.cpu.x;
    switch (x[1]) {
        case 0x41007701: { // ASHMEM_SET_NAME
            if (region.allocated) return invalid;
            std::string name;
            for (unsigned i = 0; i < 256; ++i) {
                if (i > UINT64_MAX - x[2]) return uint64_t(-14);
                const auto* byte = machine.read(x[2] + i, 1, 4);
                if (!byte) return uint64_t(-14);
                if (!*byte || i == 255) break;
                name.push_back(char(*byte));
            }
            region.name = std::move(name);
            return 0;
        }
        case 0x81007702: { // ASHMEM_GET_NAME
            const auto* name = region.name.empty() ? "dev/ashmem" : region.name.c_str();
            return machine.write(x[2], {reinterpret_cast<const uint8_t*>(name), std::strlen(name) + 1}) ? 0 : uint64_t(-14);
        }
        case 0x40087703: // ASHMEM_SET_SIZE
            if (region.allocated) return invalid;
            region.size = x[2];
            return 0;
        case 0x7704: // ASHMEM_GET_SIZE
            return region.size;
        case 0x40087705: // ASHMEM_SET_PROT_MASK
            if (x[2] & ~uint64_t(region.protection)) return invalid;
            region.protection = x[2];
            return 0;
        case 0x7706: // ASHMEM_GET_PROT_MASK
            return region.protection;
        default:
            return uint64_t(-25);
    }
}

uint64_t ProbeLinux::loopIoctl(Machine& machine, const Descriptor& fd) {
    const auto& x = machine.cpu.x;
    if (fd.pathOnly()) return uint64_t(-9);
    if (userId() != 0) return uint64_t(-1);
    const auto code = x[1];
    constexpr unsigned maxLoopDevices = 64;
    if (fd.node == Node::loopControl) {
        if (code == 0x4c80) { // LOOP_CTL_ADD
            if (x[2] >= maxLoopDevices) return invalid;
            if (loopDevices.contains("/dev/block/loop" + std::to_string(x[2]))) return uint64_t(-17);
            addLoopDevice(x[2]);
            return x[2];
        }
        if (code != 0x4c82) return uint64_t(-25); // LOOP_CTL_GET_FREE
        for (unsigned i = 0; i < maxLoopDevices; ++i) {
            const auto found = loopDevices.find("/dev/block/loop" + std::to_string(i));
            if (found == loopDevices.end()) { addLoopDevice(i); return i; }
            if (found->second.bytes.empty()) return i;
        }
        return uint64_t(-28);
    }
    auto& loop = loopDevices.at(fd.path);
    const auto backingDirectory = "/sys/devices/virtual/block/loop" + std::to_string(loop.number) + "/loop";
    if (code == 0x4c0a) { // LOOP_CONFIGURE
        struct Config {
            uint32_t fd, blockSize;
            std::array<uint64_t, 29> info;
            std::array<uint64_t, 8> reserved;
        };
        static_assert(sizeof(Config) == 304);
        Config config;
        std::vector<uint8_t> inputScratch;
        const auto* input = machine.read(x[2], sizeof(config), 4, inputScratch);
        if (!input) return uint64_t(-14);
        std::memcpy(&config, input, sizeof(config));
        const auto backing = descriptors().find(config.fd);
        if (backing == descriptors().end() || backing->second.pathOnly()) return uint64_t(-9);
        if (!loop.bytes.empty()) return uint64_t(-16);
        const auto& image = backing->second;
        // Only immutable preinstalled package bytes are valid backing storage here.
        const bool factoryPartition = image.path.starts_with("/system/apex/") || image.path.starts_with("/vendor/apex/");
        if (image.node != Node::regular || image.snapshot || !factoryPartition ||
            !image.path.ends_with(".apex") || (image.flags & 3)) return uint64_t(-95);
        const auto offset = config.info[3], size = config.info[4];
        const auto settings = static_cast<uint32_t>(config.info[6] >> 32);
        const auto blockSize = config.blockSize ? config.blockSize : 512;
        if (!size || offset > image.bytes.size() || size > image.bytes.size() - offset ||
            !std::has_single_bit(blockSize) || blockSize < 512 || blockSize > pageSize ||
            offset % blockSize || size % blockSize || (settings & ~13U) ||
            config.info[5] >> 32 || static_cast<uint32_t>(config.info[6]) ||
            std::any_of(config.reserved.begin(), config.reserved.end(), [](auto word) { return word != 0; }))
            return invalid;
        config.info[6] |= uint64_t{1} << 32; // LO_FLAGS_READ_ONLY from the read-only backing fd.
        loop.file = image.path;
        loop.bytes = image.bytes.subspan(offset, size);
        addDirectory(backingDirectory, 0555);
        kernelFiles.insert_or_assign(backingDirectory + "/backing_file", loop.file + '\n');
        loop.blockSize = blockSize;
        std::memcpy(loop.info.data(), config.info.data(), loop.info.size());
        return 0;
    }
    if (loop.bytes.empty()) return uint64_t(-6);
    switch (code) {
        case 0x4c05: // LOOP_GET_STATUS64
            return machine.write(x[2], loop.info) ? 0 : uint64_t(-14);
        case 0x4c01: // LOOP_CLR_FD
            for (const auto& [tid, thread] : threads) {
                if (std::any_of(thread.mountNamespace->mounts.begin(), thread.mountNamespace->mounts.end(),
                                [&](const auto& entry) { return entry.second.device == fd.path; }))
                    return uint64_t(-16);
            }
            kernelFiles.erase(backingDirectory + "/backing_file");
            directories.erase(backingDirectory);
            loop.file.clear();
            loop.bytes = {};
            loop.info = {};
            loop.blockSize = 512;
            loop.readAhead = 256;
            return 0;
        case 0x80081272: // BLKGETSIZE64
            return writeGuest(machine, x[2], uint64_t(loop.bytes.size())) ? 0 : uint64_t(-14);
        case 0x1268: // BLKSSZGET
            return writeGuest(machine, x[2], loop.blockSize) ? 0 : uint64_t(-14);
        case 0x1262: // BLKRASET (512-byte sectors)
            if (x[2] > 32768) return invalid;
            loop.readAhead = x[2];
            return 0;
        case 0x1263: // BLKRAGET
            return writeGuest(machine, x[2], loop.readAhead) ? 0 : uint64_t(-14);
        case 0x125e: // BLKROGET
            return writeGuest(machine, x[2], uint32_t{1}) ? 0 : uint64_t(-14);
        default:
            return uint64_t(-25);
    }
}

uint64_t ProbeLinux::signalRestorer(Machine& machine) {
    auto& process = *current->process;
    if (process.signalTrampoline) return process.signalTrampoline;
    const bool fixed = machine.space && native::signalReturnPage();
    const auto address = fixed ? native::signalReturnPage() : freeMappingAddress(machine, process.nextMapping, pageSize);
    if (!address || !allowsBacking(machine, address, address + pageSize, pageSize, memoryLimit)) return 0;
    // Guest Linux trampoline. The leading NOP matches the ARM64 vDSO unwinding convention.
    constexpr std::array<uint32_t, 4> code{0xd503201f, 0x52801168, 0xd4000001, 0xd4200000};
    Segment page{address, std::vector<uint8_t>(pageSize), 5};
    std::memcpy(page.bytes.mutableData(), code.data(), sizeof(code));
    machine.insertSegment(std::move(page));
    process.signalTrampoline = address + sizeof(code[0]);
    if (!fixed) process.nextMapping = std::max(process.nextMapping, address + pageSize);
    return process.signalTrampoline;
}

bool ProbeLinux::deliverMemoryFault(Machine& machine) {
    if (machine.cpu.stop != Stop::fault) return false;
    const auto address = machine.cpu.faultAddress & 0x00ff'ffff'ffff'ffff;
    const bool mapped = std::ranges::any_of(machine.segments, [&](const Segment& segment) {
        return address >= segment.address && address - segment.address < segment.mappedSize();
    });
    SignalFrame::Info info{};
    info.number = sigSegv;
    info.code = mapped ? 2 : 1; // SEGV_ACCERR or SEGV_MAPERR
    info.address = address;
    return installSignalFrame(machine, sigSegv, std::bit_cast<SignalWords>(info), address);
}

bool ProbeLinux::deliverIllegalInstruction(Machine& machine) {
    if (current->process->id == pid) return false;
    // BRK is not an instruction that is missing: it is a breakpoint, and Linux answers it with SIGTRAP.
    // Programs use it to end themselves where they must never arrive.
    uint32_t word = 0;
    const bool breakpoint = machine.readInto(machine.cpu.pc, {reinterpret_cast<uint8_t*>(&word), sizeof(word)}) &&
        ((word & 0xffe0001f) == 0xd4200000 || (word & 0xffe0001f) == 0xd4400000);  // BRK, HLT
    const uint32_t sigIll = breakpoint ? 5 : 4;
    SignalFrame::Info info{};
    info.number = sigIll;
    info.code = 1; // ILL_ILLOPC, or TRAP_BRKPT
    info.address = machine.cpu.pc;
    if (installSignalFrame(machine, sigIll, std::bit_cast<SignalWords>(info), machine.cpu.pc)) return true;
    finishProcess(*current->process, sigIll, true);
    removeSiblingThreads();
    current->wait = Wait::exited;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::installSignalFrame(Machine& machine, uint32_t signal, const SignalWords& info,
                                    uint64_t faultAddress) {
    auto& cpu = machine.cpu;
    auto& action = current->process->signalActions[signal];
    const auto [handler, flags, restorer, mask] = action;
    const uint64_t signalBit = 1ULL << (signal - 1);
    if (handler <= 1 || (current->signalMask & signalBit) || handler % 4 ||
        !machine.read(handler, 4, 1)) return false;

    SignalFrame frame{};
    frame.info = std::bit_cast<SignalFrame::Info>(info);
    auto& context = frame.context;
    context.stack = current->alternateStack;
    context.mask = current->suspendSignalMask.value_or(current->signalMask);
    const bool alreadyOnStack = onSignalStack(context.stack, cpu.sp);
    if (alreadyOnStack) context.stack[1] = ssOnStack;
    const auto top = (flags & saOnStack) && !(context.stack[1] & ssDisable) && !alreadyOnStack
        ? context.stack[0] + context.stack[2] : cpu.sp;
    if (top < sizeof(frame)) return false;
    const auto address = (top - sizeof(frame)) & ~uint64_t{15};
    if ((alreadyOnStack || top != cpu.sp) && address < context.stack[0]) return false;

    auto& saved = context.machine;
    saved.faultAddress = faultAddress;
    saved.regs = cpu.x;
    saved.sp = cpu.sp;
    saved.pc = cpu.pc;
    saved.pstate = uint64_t(cpu.nzcv) << 28;
    saved.fp.magic = fpsimdMagic;
    saved.fp.size = sizeof(saved.fp);
    saved.fp.fpsr = cpu.fpsr;
    saved.fp.fpcr = cpu.fpcr;
    saved.fp.regs = cpu.v;
    frame.frameRecord = {cpu.x[29], cpu.x[30]};
    const auto returnAddress = flags & saRestorer ? restorer : signalRestorer(machine);
    if (!returnAddress || !writeGuest(machine, address, frame)) return false;

    current->suspendSignalMask.reset();
    if (flags & saResetHand) action.fill(0);
    current->signalMask |= (mask | ((flags & saNoDefer) ? 0 : signalBit)) & ~unblockableSignals;
    cpu.x[0] = signal;
    if (flags & saSigInfo) {
        cpu.x[1] = address;
        cpu.x[2] = address + offsetof(SignalFrame, context);
    }
    cpu.x[29] = address + offsetof(SignalFrame, frameRecord);
    cpu.x[30] = returnAddress;
    cpu.sp = address;
    cpu.pc = handler;
    cpu.exclusiveSize = 0;
    cpu.stop = Stop::running;
    return true;
}

bool ProbeLinux::canCatchSignal(const Thread& thread, uint32_t signal) {
    const auto bit = 1ULL << (signal - 1);
    return !((thread.signalMask | thread.signalWaitMask) & bit) &&
           thread.process->signalActions[signal][0] > 1;
}

void ProbeLinux::deliverPendingCaughtSignal(Machine& machine) {
    auto& thread = *current;
    auto& cpu = machine.cpu;
    auto& pending = thread.pendingSignals;
    const auto found = std::ranges::find_if(pending, [&](const auto& entry) {
        return canCatchSignal(thread, entry.first);
    });
    if (found == pending.end()) return;
    const auto signal = found->first;
    const auto info = found->second;
    pending.erase(found);

    const auto interrupted = std::exchange(thread.signalInterruptedWait, Wait::none);
    // A diagnostic stop leaves the guest instruction unexecuted. Let the handler
    // inspect that context and let the runtime retry the instruction on return.
    if (interrupted != Wait::none && interrupted != Wait::suspended) {
        const auto flags = thread.process->signalActions[signal][1];
        const auto syscall = cpu.x[8], futexOp = cpu.x[1] & 127;
        const bool restartable = interrupted == Wait::futex
            ? (!cpu.x[3] || futexOp == 9)
            // Reads and writes, and an open that was waiting, as of one of this layer's own devices: with
            // SA_RESTART Linux starts each again after the handler.
            : interrupted == Wait::poll && (syscall == 63 || syscall == 64 || syscall == 206 || syscall == 207 || syscall == 56);
        if (!(flags & saRestart) || !restartable) {
            uint64_t error = uint64_t(-4); // EINTR
            if (interrupted == Wait::sleep && thread.deadline) {
                const auto remainingAddress = syscall == 101 ? cpu.x[1] : (cpu.x[1] & 1) ? 0 : cpu.x[3];
                if (remainingAddress) {
                    using namespace std::chrono;
                    const auto time = std::max(nanoseconds{}, duration_cast<nanoseconds>(
                        *thread.deadline - steady_clock::now()));
                    const std::array<int64_t, 2> remaining{time.count() / 1'000'000'000, time.count() % 1'000'000'000};
                    if (!writeGuest(machine, remainingAddress, remaining)) error = uint64_t(-14);
                }
            }
            cpu.pc += 4;
            cpu.x[0] = error;
        }
        thread.deadline.reset();
        if (interrupted == Wait::signal) thread.signalWaitMask = 0;
        thread.hostEventRetry.reset();
        thread.realtimeWaitDeadline.reset();
        thread.futexBacking.reset();
        thread.futexAddressSpace.reset();
        thread.waitingBinder.reset();
    }
    if (!installSignalFrame(machine, signal, info)) machine.fault(cpu.pc, cpu.sp);
}

// These default dispositions terminate the thread group. Catchable signals
// with guest handlers use the separate signal-frame delivery path.
static bool defaultFatalSignal(uint32_t signal) {
    switch (signal) {
        case 4: case 5: case 6: case 7: case 8: case 9: case 11: case 14: case 31:
            return true;
        default:
            return false;
    }
}

void ProbeLinux::fireAlarms(std::chrono::steady_clock::time_point now,
                            std::optional<std::chrono::steady_clock::time_point>& next) {
    constexpr uint32_t sigAlrm = 14;
    std::vector<Process*> ended;
    for (const auto& [id, process] : processes) {
        if (!process->alarmAt) continue;
        if (*process->alarmAt > now) {
            if (!next || *process->alarmAt < *next) next = process->alarmAt;
            continue;
        }
        if (process->alarmEvery.count() > 0) {
            while (*process->alarmAt <= now) *process->alarmAt += process->alarmEvery;
            if (!next || *process->alarmAt < *next) next = process->alarmAt;
        } else {
            process->alarmAt.reset();
        }
        const auto handler = process->signalActions[sigAlrm][0];
        if (handler == 1) continue;  // ignored
        if (handler == 0) {          // the default: the process ends
            if (process->id != pid) ended.push_back(process);
            continue;
        }
        // To a thread that does not block it; the scheduler's pass that follows wakes it for the handler.
        for (auto& [tid, thread] : threads) {
            if (thread.process.get() != process || thread.wait == Wait::exited || thread.wait == Wait::native ||
                (thread.signalMask & (1ULL << (sigAlrm - 1)))) continue;
            if (!thread.pendingSignals.contains(sigAlrm)) {
                SignalWords info{};
                info[0] = sigAlrm;
                info[2] = 0x80;  // SI_KERNEL
                thread.pendingSignals.emplace(sigAlrm, info);
            }
            break;
        }
    }
    for (Process* process : ended) {
        recallNativeThreads(process);
        finishProcess(*process, sigAlrm, true);
        const auto id = process->id;
        if (current->process.get() == process) {
            current->wait = Wait::exited;
            sliceEnd = 0;
        }
        std::erase_if(threads, [&](const auto& task) {
            return task.first != currentTid && task.second.process->id == id;
        });
    }
}

void ProbeLinux::terminateOnPendingSignal() {
    if (current->pendingSignals.empty() || current->process->id == pid) return;
    const auto signal = std::ranges::find_if(current->pendingSignals, [&](const auto& entry) {
        const auto number = entry.first;
        return defaultFatalSignal(number) && !current->process->signalActions[number][0] &&
               !(current->signalMask & (1ULL << (number - 1)));
    });
    if (signal == current->pendingSignals.end()) return;
    finishProcess(*current->process, signal->first, true);
    removeSiblingThreads();
    current->wait = Wait::exited;
    sliceEnd = 0;
}

bool ProbeLinux::restoreSignalContext(Machine& machine) {
    auto& cpu = machine.cpu;
    SignalFrame frame;
    auto bytes = std::span(reinterpret_cast<uint8_t*>(&frame), offsetof(SignalFrame, frameRecord));
    const auto& context = frame.context;
    const auto& saved = context.machine;
    if ((cpu.sp & 15) || !machine.readInto(cpu.sp, bytes) || (saved.pc & 3) || (saved.pstate & 0xf) ||
        saved.fp.magic != fpsimdMagic || saved.fp.size != sizeof(saved.fp) ||
        (context.stack[1] & ~uint64_t(ssOnStack | ssDisable)) || context.stack[2] > UINT64_MAX - context.stack[0]) {
        machine.fault(cpu.pc, cpu.sp);
        return true;
    }
    current->signalMask = context.mask & ~unblockableSignals;
    current->alternateStack = context.stack;
    current->alternateStack[1] &= ~ssOnStack;
    cpu.x = saved.regs;
    cpu.v = saved.fp.regs;
    cpu.fpsr = saved.fp.fpsr & fpsrMask;
    cpu.fpcr = saved.fp.fpcr & 0x07c00000;
    cpu.sp = saved.sp;
    cpu.pc = saved.pc;
    cpu.nzcv = (saved.pstate >> 28) & 15;
    cpu.exclusiveSize = 0;
    cpu.faultAddress = 0;
    signalContextRestored = true;
    terminateOnPendingSignal();
    if (!current->pendingSignals.empty()) sliceEnd = 0;
    return true;
}

uint64_t ProbeLinux::installSeccomp(Machine& machine, uint64_t address) {
    std::vector<uint8_t> headerScratch;
    const auto* header = machine.read(address, 16, 4, headerScratch); // AArch64 sock_fprog
    if (!header) return uint64_t(-14);
    uint16_t count;
    uint64_t instructions;
    std::memcpy(&count, header, sizeof(count));
    std::memcpy(&instructions, header + 8, sizeof(instructions));
    if (!count || count > 4096) return invalid;
    if (!current->noNewPrivileges && !(current->process->effectiveCaps & (1ULL << 21))) return uint64_t(-13);
    const auto previous = current->seccomp;
    const size_t length = count + (previous ? previous->pathLength + 4 : 0);
    if (length > 32768) return noMemory;
    static_assert(sizeof(SeccompFilter::Instruction) == 8);
    std::vector<SeccompFilter::Instruction> program(count);
    const auto bytes = program.size() * sizeof(program[0]);
    std::vector<uint8_t> sourceScratch;
    const auto* source = machine.read(instructions, bytes, 4, sourceScratch);
    if (!source) return uint64_t(-14);
    std::memcpy(program.data(), source, bytes);
    if (program.back().code != 0x06) return invalid;
    for (size_t i = 0; i < program.size(); ++i) {
        const auto& [code, yes, no, value] = program[i];
        const auto remaining = program.size() - i - 1;
        switch (code) {
            case 0x20: // LD W ABS, within struct seccomp_data.
                if (value % 4 || value >= 64) return invalid;
                break;
            case 0x05: // JA
                if (value >= remaining) return invalid;
                break;
            case 0x15: case 0x25: case 0x35: case 0x45: // JEQ, JGT, JGE, JSET K
                if (yes >= remaining || no >= remaining) return invalid;
                break;
            case 0x06: // RET K
                break;
            case 0x00: case 0x01: case 0x07: case 0x87: // LD/LDX IMM, TAX, TXA
            case 0x04: case 0x14: case 0x24: case 0x44: case 0x54: case 0xa4: case 0x84: // ADD, SUB, MUL, OR, AND, XOR K; NEG
            case 0x0c: case 0x1c: case 0x2c: case 0x4c: case 0x5c: case 0xac: // the same with X
                break;
            case 0x64: case 0x74: // LSH, RSH K
                if (value >= 32) return invalid;
                break;
            case 0x60: case 0x61: case 0x02: case 0x03: // LD/LDX MEM, ST, STX
                if (value >= 16) return invalid;
                break;
            case 0x1d: case 0x2d: case 0x3d: case 0x4d: // JEQ, JGT, JGE, JSET X
                if (yes >= remaining || no >= remaining) return invalid;
                break;
            default: return invalid;
        }
    }
    current->seccomp = std::make_shared<SeccompFilter>(SeccompFilter{std::move(program), previous, length});
    return 0;
}

bool ProbeLinux::enforceSeccomp(Machine& machine) {
    if (!current->seccomp) return false;
    auto& cpu = machine.cpu;
    // KernelSU's request for its driver goes past an app's filter, as KernelSU lets it (reboot is
    // not among the calls an app may make).
    if (rootOn && cpu.x[8] == 142 && uint32_t(cpu.x[0]) == 0xDEADBEEF && uint32_t(cpu.x[1]) == 0xCAFEBABE) return false;
    constexpr uint32_t auditArm64 = 0xc00000b7;
    std::array<uint32_t, 16> data{uint32_t(cpu.x[8]), auditArm64};
    const auto nextPC = cpu.pc + 4;
    std::memcpy(data.data() + 2, &nextPC, sizeof(nextPC));
    std::memcpy(data.data() + 4, cpu.x.data(), 6 * sizeof(uint64_t));
    constexpr uint32_t allow = 0x7fff0000;
    uint32_t result = allow;
    for (auto filter = current->seccomp; filter; filter = filter->previous) {
        uint32_t accumulator = 0, index = 0;
        std::array<uint32_t, 16> scratch{};
        for (size_t pc = 0; pc < filter->program.size(); ++pc) {
            const auto& [code, yes, no, value] = filter->program[pc];
            if (code == 0x06) {
                const auto action = value & 0xffff0000;
                const auto verdict = action == 0 || action == 0x00030000 || action == 0x00050000 ||
                    action == 0x7fc00000 || action == 0x7ff00000 || action == 0x7ffc0000 || action == allow
                    ? value : 0x80000000U;
                // Signed comparison puts KILL_PROCESS ahead of the remaining actions.
                if (int32_t(verdict & 0xffff0000) < int32_t(result & 0xffff0000)) result = verdict;
                break;
            }
            switch (code) {
                case 0x20: accumulator = data[value / 4]; break;
                case 0x05: pc += value; break;
                case 0x15: pc += accumulator == value ? yes : no; break;
                case 0x25: pc += accumulator > value ? yes : no; break;
                case 0x35: pc += accumulator >= value ? yes : no; break;
                case 0x45: pc += (accumulator & value) ? yes : no; break;
                case 0x1d: pc += accumulator == index ? yes : no; break;
                case 0x2d: pc += accumulator > index ? yes : no; break;
                case 0x3d: pc += accumulator >= index ? yes : no; break;
                case 0x4d: pc += (accumulator & index) ? yes : no; break;
                case 0x00: accumulator = value; break;
                case 0x01: index = value; break;
                case 0x07: index = accumulator; break;
                case 0x87: accumulator = index; break;
                case 0x60: accumulator = scratch[value]; break;
                case 0x61: index = scratch[value]; break;
                case 0x02: scratch[value] = accumulator; break;
                case 0x03: scratch[value] = index; break;
                case 0x04: accumulator += value; break;
                case 0x14: accumulator -= value; break;
                case 0x24: accumulator *= value; break;
                case 0x44: accumulator |= value; break;
                case 0x54: accumulator &= value; break;
                case 0xa4: accumulator ^= value; break;
                case 0x64: accumulator <<= value; break;
                case 0x74: accumulator >>= value; break;
                case 0x84: accumulator = 0 - accumulator; break;
                case 0x0c: accumulator += index; break;
                case 0x1c: accumulator -= index; break;
                case 0x2c: accumulator *= index; break;
                case 0x4c: accumulator |= index; break;
                case 0x5c: accumulator &= index; break;
                case 0xac: accumulator ^= index; break;
            }
        }
    }
    const auto action = result & 0xffff0000;
    if (action == allow || action == 0x7ffc0000) return false; // LOG permits the call.
    if (action == 0x00050000) { // ERRNO
        cpu.x[0] = uint64_t(-int64_t(std::min(result & 0xffff, 4095U)));
        return true;
    }
    if (action == 0x7fc00000 || action == 0x7ff00000) { // Notifications and ptrace have no listener.
        cpu.x[0] = uint64_t(-38);
        return true;
    }
    if (action == 0x00030000) { // TRAP: SIGSYS to the thread, before its next instruction
        // The call is not made. The handler reads which call it was from the signal's information and
        // can give the call a result through the saved registers: Chromium's sandbox works that way.
        // Without a handler, SIGSYS ends the process.
        SignalWords info{};
        info[0] = 31;                     // SIGSYS
        info[1] = result & 0xffff;        // SECCOMP_RET_DATA, in si_errno
        info[2] = 1;                      // SYS_SECCOMP
        const auto callAddress = cpu.pc + 4;
        std::memcpy(&info[4], &callAddress, sizeof(callAddress));
        info[6] = uint32_t(cpu.x[8]);     // si_syscall
        info[7] = 0xc00000b7;             // si_arch: AUDIT_ARCH_AARCH64
        current->pendingSignals.emplace(31, info);
        current->signalMask &= ~(1ULL << 30); // a seccomp SIGSYS is not held back by the mask
        sliceEnd = 0;
        return true;
    }
    const bool wholeProcess = action != 0;
    const auto process = current->process;
    if (wholeProcess) removeSiblingThreads();
    current->wait = Wait::exited;
    const bool memberAlive = !wholeProcess && std::ranges::any_of(threads, [&](const auto& task) {
        return task.second.process == process && task.second.wait != Wait::exited;
    });
    if (!memberAlive) finishProcess(*process, 31, true);
    sliceEnd = 0;
    return true;
}

static bool visitInetPrograms(const auto& hooks, std::string_view group, const auto& isGroupCurrent, const auto& run) {
    constexpr uint32_t multi = 2;
    bool evaluated = false;
    size_t depth = 0;
    while (group == cgroupRoot || inside(group, cgroupRoot)) {
        const auto attachment = hooks.find(group);
        if (attachment != hooks.end() && (!evaluated || (attachment->second.flags & multi)) &&
            isGroupCurrent(attachment->second.group, depth)) {
            for (const auto& program : attachment->second.programs) if (!run(*program)) return false;
            evaluated = true;
        }
        if (group == cgroupRoot) break;
        group = group.substr(0, group.rfind('/'));
        ++depth;
    }
    return true;
}

static bool runInetPrograms(const auto& hooks, std::string_view group, const BpfProgram::SocketContext& context,
                            const auto& isGroupCurrent, bool* skipBindCapability = nullptr) {
    return visitInetPrograms(hooks, group, isGroupCurrent, [&](const BpfProgram& program) {
        const auto result = program.runSocketContext(context);
        if (!result || (*result != 1 && (!skipBindCapability || *result != 3))) return false;
        if (skipBindCapability && *result == 3) *skipBindCapability = true;
        return true;
    });
}

static bool sameSocketGroup(const auto& socket, const auto& attached, size_t depth) {
    constexpr std::owner_less<void> order{};
    const auto& ancestry = socket.inetAncestry;
    return depth < ancestry.size() && !order(attached, ancestry[depth]) && !order(ancestry[depth], attached);
}

bool ProbeLinux::openInetSocket(const std::shared_ptr<SocketState>& socket, const SocketState* listener) {
    socket->storageIdentity = std::make_shared<char>();
    const auto& group = listener ? listener->inetCgroup : current->process->cgroup;
    const BpfProgram::SocketContext context{socket->storageIdentity, socket->cookie, socket->credentials[1],
        socket->credentials[2], socket->family, socket->type, socket->protocol,
        [&](size_t bytes) { return canAllocateSocketStorage(bytes); }, uint32_t(processId()), uint32_t(currentTid)};
    if (!listener && !runInetPrograms(inetCreatePrograms, group, context, [](const auto&, size_t) { return true; }))
        return false;
    if (group.empty()) return true;
    socket->inetCgroup = group;
    if (listener) socket->inetAncestry = listener->inetAncestry;
    for (auto path = std::string_view(group); !listener && (path == cgroupRoot || inside(path, cgroupRoot));
         path = path.substr(0, path.rfind('/'))) {
        const auto identity = cgroupFiles.find(std::string(path) + std::string(cgroupProcs));
        socket->inetAncestry.emplace_back(identity == cgroupFiles.end() ? nullptr : identity->second);
        if (path == cgroupRoot) break;
    }
    socket->onClose = [hooks = std::weak_ptr(inetReleasePrograms)](const SocketState& endpoint) {
        if (const auto programs = hooks.lock(); programs && !programs->empty()) {
            const auto isCurrent = [&](const auto& attached, size_t depth) {
                return sameSocketGroup(endpoint, attached, depth);
            };
            try {
                const BpfProgram::SocketContext release{endpoint.storageIdentity, endpoint.cookie, endpoint.credentials[1],
                    endpoint.credentials[2], endpoint.family, endpoint.type, endpoint.protocol, {}};
                runInetPrograms(*programs, endpoint.inetCgroup, release, isCurrent);
            } catch (const std::bad_alloc&) {
                // Socket teardown still releases the descriptor and its storage identity.
            }
        }
    };
    return true;
}

bool ProbeLinux::filterInetAddress(const SocketState& socket, uint32_t type,
    SocketState::InetAddress& address, bool* skipBindCapability) {
    const auto hooks = inetAddressPrograms.find(type);
    if (hooks == inetAddressPrograms.end() || hooks->second.empty()) return true;
    const bool ipv4 = socket.family == 2;
    const size_t offset = ipv4 ? 4 : 8, ipSize = ipv4 ? 4 : 16;
    BpfProgram::SocketAddress target;
    std::memcpy(&target.port, address.data() + 2, 2);
    std::memcpy(target.ip.data(), address.data() + offset, ipSize);
    BpfProgram::SocketContext context{socket.storageIdentity, socket.cookie, userId(), groupId(),
        socket.family, socket.type, socket.protocol, {}, uint32_t(processId()), uint32_t(currentTid)};
    context.address = &target;
    const auto isCurrent = [&](const auto& attached, size_t depth) {
        return sameSocketGroup(socket, attached, depth);
    };
    if (!runInetPrograms(hooks->second, socket.inetCgroup, context, isCurrent, skipBindCapability)) return false;
    std::memcpy(address.data() + 2, &target.port, 2);
    std::memcpy(address.data() + offset, target.ip.data(), ipSize);
    return true;
}

uint64_t ProbeLinux::bindInetSocket(Machine& machine, const std::shared_ptr<SocketState>& socket) {
    const auto& x = machine.cpu.x;
    const bool ipv4 = socket->family == 2;
    const size_t size = ipv4 ? 16 : 28;
    if (x[2] < size || x[2] > 128 || socket->localInetAddress[0]) return invalid;
    std::array<uint8_t, 28> address{};
    if (!machine.readInto(x[1], std::span(address).first(size))) return uint64_t(-14);
    if (address[0] != socket->family || address[1]) return uint64_t(-97);
    bool skipBindCapability = false;
    if (!filterInetAddress(*socket, ipv4 ? BpfProgram::bind4 : BpfProgram::bind6, address, &skipBindCapability))
        return uint64_t(-1);
    // The guest loopback owns exactly the wildcard and standard loopback addresses.
    const auto ip = std::span(address).subspan(ipv4 ? 4 : 8, ipv4 ? 4 : 16);
    constexpr std::array<uint8_t, 4> loopback4{127, 0, 0, 1};
    constexpr std::array<uint8_t, 16> loopback6{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto loopback = ipv4 ? std::span<const uint8_t>(loopback4) : std::span<const uint8_t>(loopback6);
    if (!std::ranges::equal(ip, loopback) && std::ranges::any_of(ip, [](uint8_t byte) { return byte != 0; }))
        return uint64_t(-99);
    const auto port = uint16_t(uint16_t(address[2]) << 8 | address[3]);
    if (port && port < 1024 && !skipBindCapability && !(current->process->effectiveCaps & (1ULL << 10))) return uint64_t(-13);
    return reserveInetAddress(socket, address);
}

namespace {
using InetAddress = std::array<uint8_t, 28>;

uint16_t inetPort(const InetAddress& address) {
    return uint16_t(uint16_t(address[2]) << 8 | address[3]);
}

std::span<const uint8_t> inetHost(const InetAddress& address) {
    return address[0] == 2 ? std::span(address).subspan(4, 4) : std::span(address).subspan(8, 16);
}

bool inetWildcard(const InetAddress& address) {
    return std::ranges::all_of(inetHost(address), [](auto byte) { return byte == 0; });
}

void setInetLoopback(InetAddress& address) {
    if (address[0] == 2) address[4] = 127;
    address[address[0] == 2 ? 7 : 23] = 1;
}

InetAddress asMappedIpv4(const InetAddress& address) {
    InetAddress mapped{};
    mapped[0] = 10;
    std::copy_n(address.begin() + 2, 2, mapped.begin() + 2);
    mapped[18] = mapped[19] = 0xff;
    std::copy_n(address.begin() + 4, 4, mapped.begin() + 20);
    return mapped;
}

InetAddress asWireAddress(const InetAddress& address) {
    const auto ip = inetHost(address);
    if (address[0] != 10 || ip[10] != 0xff || ip[11] != 0xff ||
        std::ranges::any_of(ip.first(10), [](uint8_t byte) { return byte != 0; })) return address;
    InetAddress wire{};
    wire[0] = 2;
    std::copy_n(address.begin() + 2, 2, wire.begin() + 2);
    std::copy(ip.end() - 4, ip.end(), wire.begin() + 4);
    return wire;
}

bool matchInetPeer(const InetAddress& expected, const InetAddress& actual) {
    return inetPort(expected) == inetPort(actual) && std::ranges::equal(inetHost(expected), inetHost(actual));
}

uint64_t readInetDestination(const Machine& machine, uint64_t pointer, uint64_t length,
                             uint32_t family, InetAddress& address) {
    const uint64_t size = family == 2 ? 16 : 28;
    if (length < size || length > 128) return uint64_t(-22);
    if (!machine.readInto(pointer, std::span(address).first(size))) return uint64_t(-14);
    return address[0] == family && !address[1] ? 0 : uint64_t(-97);
}

uint64_t routeInetDestination(InetAddress& address) {
    InetAddress loopback{};
    loopback[0] = address[0];
    setInetLoopback(loopback);
    if (inetWildcard(address)) setInetLoopback(address);
    if (!std::ranges::equal(inetHost(address), inetHost(loopback))) return uint64_t(-101);
    return inetPort(address) ? 0 : uint64_t(-22);
}

uint32_t internetSum(std::span<const uint8_t> bytes, uint32_t sum = 0) {
    for (size_t i = 0; i < bytes.size(); i += 2)
        sum += (uint32_t(bytes[i]) << 8) | (i + 1 < bytes.size() ? bytes[i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return sum;
}

void writeNetwork16(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    bytes[offset] = uint8_t(value >> 8);
    bytes[offset + 1] = uint8_t(value);
}

void encodeLoopbackIp(std::span<uint8_t> bytes, const InetAddress& source, const InetAddress& destination,
                      uint8_t protocol) {
    const bool ipv4 = source[0] == 2;
    const auto headerSize = ipv4 ? 20 : 40;
    bytes[0] = ipv4 ? 0x45 : 0x60;
    if (ipv4) {
        writeNetwork16(bytes, 2, bytes.size());
        bytes[8] = 64;
        bytes[9] = protocol;
    } else {
        writeNetwork16(bytes, 4, bytes.size() - headerSize);
        bytes[6] = protocol;
        bytes[7] = 64;
    }
    std::ranges::copy(inetHost(source), bytes.begin() + (ipv4 ? 12 : 8));
    std::ranges::copy(inetHost(destination), bytes.begin() + (ipv4 ? 16 : 24));
    if (ipv4) writeNetwork16(bytes, 10, ~internetSum(bytes.first(headerSize)));
}

uint16_t transportChecksum(std::span<const uint8_t> packet, const InetAddress& source,
                           const InetAddress& destination, uint8_t protocol) {
    const auto payload = packet.subspan(source[0] == 2 ? 20 : 40);
    const auto pseudo = internetSum(inetHost(source), internetSum(inetHost(destination), protocol + payload.size()));
    return uint16_t(~internetSum(payload, pseudo));
}

void encodeLoopbackUdp(std::span<uint8_t> bytes, const InetAddress& source, const InetAddress& destination) {
    encodeLoopbackIp(bytes, source, destination, 17);
    auto udp = bytes.subspan(source[0] == 2 ? 20 : 40);
    writeNetwork16(udp, 0, inetPort(source));
    writeNetwork16(udp, 2, inetPort(destination));
    writeNetwork16(udp, 4, udp.size());
    const auto checksum = transportChecksum(bytes, source, destination, 17);
    writeNetwork16(udp, 6, checksum ? checksum : 0xffff);
}
} // namespace

uint64_t ProbeLinux::reserveInetAddress(const std::shared_ptr<SocketState>& socket, SocketState::InetAddress address) {
    const bool ipv4 = socket->family == 2;
    auto port = uint16_t(uint16_t(address[2]) << 8 | address[3]);
    std::erase_if(inetBindings, [](const auto& entry) { return entry.second.expired(); });
    if (inetBindings.size() >= 256) return uint64_t(-98);
    const auto key = [&](uint16_t value) { return std::tuple(socket->family, socket->protocol, value); };
    const auto conflicts = [&](uint16_t value) {
        if (inetBindings.contains(key(value))) return true;
        const auto other = inetBindings.find({ipv4 ? 10U : 2U, socket->protocol, value});
        if (other == inetBindings.end()) return false;
        if (!ipv4) return !socket->inetV6Only && inetWildcard(address);
        const auto bound = other->second.lock();
        return bound && !bound->inetV6Only && inetWildcard(bound->localInetAddress);
    };
    if (!port) {
        constexpr uint16_t end = 60999, start = 32768;
        do {
            port = nextEphemeralPort;
            nextEphemeralPort = port == end ? start : port + 1;
        } while (conflicts(port));
    }
    if (conflicts(port)) return uint64_t(-98);
    inetBindings.emplace(key(port), socket);
    address[2] = port >> 8;
    address[3] = uint8_t(port);
    if (ipv4) std::ranges::fill(std::span(address).subspan(8), 0);
    socket->localInetAddress = address;
    return 0;
}

std::shared_ptr<ProbeLinux::SocketState> ProbeLinux::findInetDestination(
    uint32_t protocol, const SocketState::InetAddress& address) const {
    const auto port = inetPort(address);
    if (const auto exact = inetBindings.find({address[0], protocol, port}); exact != inetBindings.end()) {
        if (const auto endpoint = exact->second.lock(); endpoint &&
            (inetWildcard(endpoint->localInetAddress) || matchInetPeer(endpoint->localInetAddress, address)))
            return endpoint;
    }
    if (address[0] == 2) {
        if (const auto dual = inetBindings.find({10, protocol, port}); dual != inetBindings.end()) {
            if (const auto endpoint = dual->second.lock(); endpoint && !endpoint->inetV6Only &&
                inetWildcard(endpoint->localInetAddress)) return endpoint;
        }
    }
    return nullptr;
}

int ProbeLinux::hostSocketEvents(SocketState& socket) {
    if (socket.hostConnecting) {
        const int state = hostnet::connectState(socket.hostFd);
        if (state == -115) return 0;
        socket.hostConnecting = false;
        if (state < 0) {
            socket.inetError = -state;
            socket.tcpConnectFailed = true;
            return 1 | 4 | 8 | 16;
        }
        socket.connected = true;
    }
    return hostnet::events(socket.hostFd);
}

// NESTVANE_NETWORK_TRACE=1: what the guest's sockets do out on the network, in the Android log.
#define NETWORK_TRACE(...) do { \
        if (static const bool traced = std::getenv("NESTVANE_NETWORK_TRACE"); traced && androidLogObserver) { \
            char line[200]; \
            std::snprintf(line, sizeof(line), "native execution: network " __VA_ARGS__); \
            androidLogObserver(line); \
        } \
    } while (false)

uint64_t ProbeLinux::connectHostSocket(const std::shared_ptr<SocketState>& socket, const InetAddress& destination,
                                       bool nonblocking) {
    NETWORK_TRACE("connect %s to %u.%u.%u.%u:%u family %u fd %d", socket->inetDatagram() ? "udp" : "tcp",
                  hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[0], hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[1],
                  hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[2], hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[3],
                  unsigned(destination[2]) << 8 | destination[3], unsigned(destination[0]), socket->hostFd);
    const bool ipv6 = hostnet::isIpv6(destination);
    const auto setLocal = [&] {
        if (socket->localInetAddress[0] && inetPort(socket->localInetAddress)) return;
        socket->localInetAddress = hostnet::localAddress(socket->family, nextEphemeralPort, ipv6);
        nextEphemeralPort = nextEphemeralPort >= 60999 ? 32768 : nextEphemeralPort + 1;
    };
    // IPv6 destinations go out where the phone's network has IPv6; ping has only an IPv4 socket here.
    if (hostnet::unreachable(destination) || (ipv6 && socket->icmp())) return uint64_t(-101);
    if (socket->inetDatagram()) {
        // Connecting a datagram socket to port 0 only asks whether there is a route, which is how the
        // resolver finds out which address families the network has. The host refuses such a connect,
        // so for IPv6 the host is asked about the route another way.
        if (!inetPort(destination) && ipv6 && !hostnet::hostRoutes(destination)) return uint64_t(-101);
        if (inetPort(destination) || socket->icmp()) {
            if (socket->hostFd < 0) socket->hostFd = hostnet::openDatagram(destination, socket->icmp());
            if (socket->hostFd < 0) return uint64_t(int64_t(std::exchange(socket->hostFd, -1)));
            if (const int error = hostnet::connectDatagram(socket->hostFd, destination)) return uint64_t(int64_t(error));
        }
        setLocal();
        socket->peerInetAddress = destination;
        socket->connected = true;
        socket->inetError = 0;
        return 0;
    }
    if (socket->hostFd < 0) {
        if (socket->connected || socket->listening) return uint64_t(-106);
        const int fd = hostnet::openStream(destination);
        if (fd < 0) return uint64_t(int64_t(fd));
        socket->hostFd = fd;
        socket->hostConnecting = true;
        socket->hostConnectReported = false;
        socket->peerInetAddress = destination;
        setLocal();
    }
    if (!socket->hostConnecting) return socket->connected ? uint64_t(-106) : uint64_t(-std::exchange(socket->inetError, 111));
    hostSocketEvents(*socket);
    if (!socket->hostConnecting) {
        if (socket->connected) return std::exchange(socket->hostConnectReported, true) && nonblocking ? uint64_t(-106) : 0;
        return uint64_t(-std::exchange(socket->inetError, 0));
    }
    if (nonblocking) return std::exchange(socket->hostConnectReported, true) ? uint64_t(-114) : uint64_t(-115);
    hostSocketWait = true;
    return uint64_t(-11);
}

uint64_t ProbeLinux::connectInetSocket(Machine& machine, const std::shared_ptr<SocketState>& socket,
                                       bool nonblocking) {
    const auto& x = machine.cpu.x;
    if (x[2] >= 2) {
        uint16_t family = 0;
        if (!machine.readInto(x[1], {reinterpret_cast<uint8_t*>(&family), sizeof(family)})) return uint64_t(-14);
        if (!family) {
            if (!socket->inetDatagram()) return uint64_t(-95);
            socket->connected = false;
            socket->peerInetAddress = {};
            socket->inetError = 0;
            return 0;
        }
    }
    InetAddress destination{};
    if (const auto error = readInetDestination(machine, x[1], x[2], socket->family, destination)) return error;
    if (!filterInetAddress(*socket, socket->family == 2 ? BpfProgram::connect4 : BpfProgram::connect6, destination))
        return uint64_t(-1);
    if (hostNetwork && (socket->hostFd >= 0 || hostnet::external(destination)))
        return connectHostSocket(socket, destination, nonblocking);
    if (const auto error = routeInetDestination(destination)) return error;
    if (const auto error = guestRoutes.routeLoopback(asWireAddress(destination)[0], socket->socketMark,
                                                    socket->credentials[1])) return uint64_t(error);
    if (!inetLoopbackUp) return uint64_t(-100);
    if (socket->inetStream()) return connectInetStream(socket, destination, nonblocking);
    if (!socket->localInetAddress[0]) {
        InetAddress local{};
        local[0] = socket->family;
        setInetLoopback(local);
        if (const auto error = reserveInetAddress(socket, local)) return error;
    }
    socket->peerInetAddress = destination;
    socket->connected = true;
    socket->inetError = 0;
    return 0;
}

uint64_t ProbeLinux::resolveFirewallBpf(std::string_view path, int32_t fd,
                                        std::shared_ptr<const BpfProgram>& program) {
    if (path.empty()) {
        const auto descriptor = descriptors().find(uint32_t(fd));
        if (descriptor == descriptors().end()) return uint64_t(-9);
        program = descriptor->second.bpfProgram;
        return program ? 0 : invalid;
    }
    const auto target = followLinks(absolutePath(path), true);
    if (!target) return uint64_t(-40);
    const auto backing = backingPath(*target);
    const auto entry = find(backing, 0);
    if (!canAccessParent(backing, false)) return uint64_t(-13);
    if (!entry) return uint64_t(-2);
    if (!entry->temporary || !entry->temporary->pinnedProgram) return invalid;
    const auto& inode = *entry->temporary;
    if (userId() && !(inode.mode >> permissionShift(inode.uid, inode.gid) & 4)) return uint64_t(-13);
    program = inode.pinnedProgram;
    return 0;
}

bool ProbeLinux::matchFirewallBpf(const BpfProgram& program, GuestFirewall::Hook hook, BpfProgram::Packet& packet) {
    packet.ingressIfindex = (hook == GuestFirewall::prerouting || hook == GuestFirewall::input) ? guestLoopbackIndex : 0;
    const auto matched = program.classify(packet).value_or(0) != 0;
    if (packet.ringNotification) {
        wakePollers();
        packet.ringNotification = false;
    }
    return matched;
}

static bool filterInetPacket(const auto& hooks, const auto& socket, BpfProgram::Packet& packet, bool egress) {
    packet.ringNotification = false;
    packet.socket = socket.storageIdentity;
    packet.socketCookie = socket.cookie;
    packet.socketUid = socket.credentials[1];
    packet.socketMark = socket.socketMark;
    if (hooks.empty()) return true;
    const auto isCurrent = [&](const auto& attached, size_t depth) {
        return sameSocketGroup(socket, attached, depth);
    };
    bool accepted = true;
    visitInetPrograms(hooks, socket.inetCgroup, isCurrent, [&](const BpfProgram& program) {
        const auto result = program.classify(packet);
        // Egress bit zero is the verdict. Loopback has no congestion notification transport.
        accepted &= result && (egress ? (*result & 1) != 0 : *result == 1);
        return true; // Later programs still observe the packet and update accounting.
    });
    return accepted;
}

std::shared_ptr<void> ProbeLinux::lookupInetSocket(std::span<const uint8_t> tuple, bool tcp,
                                                    const SocketState* receiving) const {
    if (tuple.size() != 12 && tuple.size() != 36) return nullptr;
    const size_t ipBytes = (tuple.size() - 4) / 2;
    const uint32_t family = ipBytes == 4 ? 2 : 10;
    const size_t offset = family == 2 ? 4 : 8;
    InetAddress from{}, to{};
    from[0] = to[0] = family;
    std::ranges::copy(tuple.first(ipBytes), from.begin() + offset);
    std::ranges::copy(tuple.subspan(ipBytes, ipBytes), to.begin() + offset);
    std::ranges::copy(tuple.subspan(2 * ipBytes, 2), from.begin() + 2);
    std::ranges::copy(tuple.last(2), to.begin() + 2);
    const uint32_t protocol = tcp ? 6 : 17;
    const auto addressMatches = [&](const SocketState& socket) {
        const auto local = asWireAddress(socket.localInetAddress);
        const bool wildcard = inetWildcard(local);
        const bool dual = family == 2 && local[0] == 10 && !socket.inetV6Only && wildcard;
        return socket.protocol == protocol && (local[0] == family || dual) &&
            (wildcard || matchInetPeer(local, to)) && inetPort(local) == inetPort(to) &&
            (!socket.connected || matchInetPeer(asWireAddress(socket.peerInetAddress), from));
    };
    if (receiving && addressMatches(*receiving)) return receiving->storageIdentity;
    const auto socket = findInetDestination(protocol, to);
    if (!socket || !addressMatches(*socket) || (tcp && !socket->listening && !socket->connected)) return nullptr;
    if (tcp && socket->listening) {
        const auto remote = inetBindings.find({family, protocol, inetPort(from)});
        const auto sender = remote == inetBindings.end() ? nullptr : remote->second.lock();
        const auto accepted = sender ? sender->peer.lock() : nullptr;
        if (accepted && addressMatches(*accepted)) return accepted->storageIdentity;
    }
    return socket->storageIdentity;
}

static void runTcpOperations(const auto& hooks, const auto& socket, BpfProgram::SocketOperations& operation) {
    const auto isCurrent = [&](const auto& attached, size_t depth) {
        return sameSocketGroup(socket, attached, depth);
    };
    visitInetPrograms(hooks, socket.inetCgroup, isCurrent, [&](const BpfProgram& program) {
        program.runSocketOperations(operation);
        return true;
    });
}

bool ProbeLinux::transmitInetStream(SocketState& from, SocketState& to, std::span<const uint8_t> payload,
                                    uint32_t flags, GuestFirewall::Rejection& rejection) {
    constexpr uint32_t syn = 2, fin = 1, ack = 16;
    const auto advance = [&] { from.tcpSequence += uint32_t(payload.size()) + bool(flags & (syn | fin)); };
    const bool optionsEnabled = (from.tcpCallbackFlags & 0x40) && !inetSocketOperationPrograms.empty();
    if (!optionsEnabled && inetEgressPrograms.empty() && inetIngressPrograms.empty() && firewall.empty()) {
        advance();
        return true;
    }

    BpfProgram::SocketOperations output{from.storageIdentity, BpfProgram::SocketOperations::writeOption,
                                         from.tcpCallbackFlags, flags};
    if (optionsEnabled) {
        BpfProgram::SocketOperations length{from.storageIdentity, BpfProgram::SocketOperations::optionLength,
                                            from.tcpCallbackFlags, flags};
        runTcpOperations(inetSocketOperationPrograms, from, length);
        output.callbackFlags = length.callbackFlags;
        output.remaining = length.options.size() - length.remaining;
        runTcpOperations(inetSocketOperationPrograms, from, output);
        from.tcpCallbackFlags = output.callbackFlags;
    }
    const auto paddedOptions = (output.used + 3) & ~size_t(3);
    const auto source = asWireAddress(from.localInetAddress), destination = asWireAddress(to.localInetAddress);
    const auto networkSize = source[0] == 2 ? 20 : 40;
    const auto tcpSize = 20 + paddedOptions;
    BpfProgram::Packet packet;
    packet.bytes.resize(networkSize + tcpSize + payload.size());
    encodeLoopbackIp(packet.bytes, source, destination, 6);
    auto tcp = std::span(packet.bytes).subspan(networkSize);
    writeNetwork16(tcp, 0, inetPort(from.localInetAddress));
    writeNetwork16(tcp, 2, inetPort(to.localInetAddress));
    writeNetwork16(tcp, 4, from.tcpSequence >> 16);
    writeNetwork16(tcp, 6, from.tcpSequence);
    if (flags & ack) {
        writeNetwork16(tcp, 8, to.tcpSequence >> 16);
        writeNetwork16(tcp, 10, to.tcpSequence);
    }
    tcp[12] = uint8_t(tcpSize / 4 << 4);
    tcp[13] = uint8_t(flags);
    writeNetwork16(tcp, 14, 65535);
    std::ranges::copy(std::span(output.options).first(output.used), tcp.begin() + 20);
    std::ranges::copy(payload, tcp.begin() + tcpSize);
    writeNetwork16(tcp, 16, transportChecksum(packet.bytes, source, destination, 6));

    packet.protocol = source[0] == 2 ? 0x0008 : 0xdd86;
    packet.mark = from.socketMark;
    const auto connection = (flags & (syn | ack)) == syn ? GuestFirewall::newConnection :
                                                          GuestFirewall::establishedConnection;
    const GuestFirewall::LogPacket logger = [&](const auto& record) { emitNetfilterLog(record, from); };
    const GuestFirewall::RunBpf classify = [&](const auto& program, auto hook) {
        return matchFirewallBpf(program, hook, packet);
    };
    const GuestFirewall::PacketState state{&packet.mark, from.tcpConnectionMark.get(), connection,
                                            &rejection, &logger, &classify};
    packet.ifindex = guestLoopbackIndex;
    packet.socketLookup = [&](std::span<const uint8_t> tuple, bool tcp) {
        return lookupInetSocket(tuple, tcp, &to);
    };
    const bool sent = filterInetPacket(inetEgressPrograms, from, packet, true);
    if (packet.ringNotification) wakePollers();
    if (!sent || !firewall.allows(packet.bytes, GuestFirewall::output, guestLoopbackMtu, state) ||
        !firewall.allows(packet.bytes, GuestFirewall::postrouting, guestLoopbackMtu, state) ||
        !firewall.allows(packet.bytes, GuestFirewall::prerouting, guestLoopbackMtu, state) ||
        !firewall.allows(packet.bytes, GuestFirewall::input, guestLoopbackMtu, state)) return false;
    packet.ingressIfindex = guestLoopbackIndex;
    const bool received = filterInetPacket(inetIngressPrograms, to, packet, false);
    if (packet.ringNotification) wakePollers();
    if (!received) return false;
    advance();
    return true;
}

uint64_t ProbeLinux::connectInetStream(const std::shared_ptr<SocketState>& socket,
                                       const SocketState::InetAddress& destination, bool nonblocking) {
    if (socket->connected) return uint64_t(-106);
    if (socket->listening) return invalid;
    socket->tcpConnectFailed = false;
    socket->inetError = 0;
    const auto listener = findInetDestination(socket->protocol, destination);
    if (!listener || !listener->listening) return uint64_t(-111);
    if (listener->pending.size() >= listener->backlog) return uint64_t(-11);
    if (!socket->localInetAddress[0]) {
        InetAddress local{};
        local[0] = socket->family;
        setInetLoopback(local);
        if (const auto error = reserveInetAddress(socket, local)) return error;
    }
    if (inetWildcard(socket->localInetAddress)) setInetLoopback(socket->localInetAddress);
    try {
        auto accepted = std::make_shared<SocketState>(socket->type);
        accepted->protocol = socket->protocol;
        accepted->family = listener->family;
        accepted->inetV6Only = listener->inetV6Only;
        accepted->cookie = nextSocketCookie++;
        accepted->credentials = listener->credentials;
        accepted->socketMark = listener->socketMark;
        accepted->linger = listener->linger;
        accepted->context = listener->context;
        const bool mapped = socket->family != accepted->family;
        accepted->localInetAddress = mapped ? asMappedIpv4(destination) : destination;
        accepted->peerInetAddress = mapped ? asMappedIpv4(socket->localInetAddress) : socket->localInetAddress;
        openInetSocket(accepted, listener.get());
        socket->tcpConnectionMark = accepted->tcpConnectionMark = std::make_shared<uint32_t>(0);

        socket->tcpSequence = uint32_t(socket->cookie);
        accepted->tcpSequence = uint32_t(accepted->cookie);
        constexpr uint32_t syn = 2, ack = 16;
        BpfProgram::SocketOperations connecting{socket->storageIdentity,
            BpfProgram::SocketOperations::tcpConnect, socket->tcpCallbackFlags, syn};
        runTcpOperations(inetSocketOperationPrograms, *socket, connecting);
        socket->tcpCallbackFlags = connecting.callbackFlags;
        GuestFirewall::Rejection rejection;
        if (!transmitInetStream(*socket, *accepted, {}, syn, rejection) ||
            !transmitInetStream(*accepted, *socket, {}, syn | ack, rejection) ||
            !transmitInetStream(*socket, *accepted, {}, ack, rejection)) {
            if (!rejection.error) return uint64_t(-110);
            socket->tcpConnectFailed = true;
            if (!nonblocking) return uint64_t(-rejection.error);
            socket->inetError = rejection.error;
            wakePollers();
            return uint64_t(-115); // EINPROGRESS; poll/SO_ERROR report the refusal.
        }

        BpfProgram::SocketOperations active{socket->storageIdentity,
            BpfProgram::SocketOperations::activeEstablished, socket->tcpCallbackFlags, ack};
        runTcpOperations(inetSocketOperationPrograms, *socket, active);
        socket->tcpCallbackFlags = active.callbackFlags;
        BpfProgram::SocketOperations passive{accepted->storageIdentity,
            BpfProgram::SocketOperations::passiveEstablished, accepted->tcpCallbackFlags, ack};
        runTcpOperations(inetSocketOperationPrograms, *accepted, passive);
        accepted->tcpCallbackFlags = passive.callbackFlags;
        listener->pending.push_back(accepted);
        socket->peerInetAddress = destination;
        socket->peer = accepted;
        accepted->peer = socket;
        socket->connected = accepted->connected = true;
        socket->peerCredentials = accepted->credentials;
        accepted->peerCredentials = socket->credentials;
        socket->peerContext = accepted->context;
        accepted->peerContext = socket->context;
        wakePollers();
        return 0;
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

ProbeLinux::DatagramConnection* ProbeLinux::trackDatagramConnection(const std::shared_ptr<SocketState>& first,
    const std::shared_ptr<SocketState>& second) {
    const auto now = std::chrono::steady_clock::now();
    const auto key = std::minmax(first->cookie, second->cookie);
    auto existing = datagramConnections.find(key);
    if (existing != datagramConnections.end() && existing->second.expires > now) {
        existing->second.expires = now + std::chrono::seconds(30);
        return &existing->second;
    }
    std::erase_if(datagramConnections, [&](const auto& entry) {
        const auto& connection = entry.second;
        return connection.expires <= now || connection.first.expired() || connection.second.expired();
    });
    if (datagramConnections.size() >= 1024) return nullptr;
    const auto at = datagramConnections.try_emplace(key,
        DatagramConnection{false, false, first, second, now + std::chrono::seconds(30)}).first;
    return &at->second;
}

uint64_t ProbeLinux::sendInetDatagram(Machine& machine, const std::shared_ptr<SocketState>& socket,
    std::span<const SocketVector> vectors, uint64_t address, uint64_t addressLength) {
    if (socket->inetError) return uint64_t(-std::exchange(socket->inetError, 0));
    InetAddress destination = socket->peerInetAddress;
    if (address) {
        if (const auto error = readInetDestination(machine, address, addressLength, socket->family, destination))
            return error;
        if (!filterInetAddress(*socket, socket->family == 2 ? BpfProgram::send4 : BpfProgram::send6, destination))
            return uint64_t(-1);
        if (hostNetwork && hostnet::external(destination)) {
            NETWORK_TRACE("udp to %u.%u.%u.%u:%u%s", hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[0],
                          hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[1],
                          hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[2],
                          hostnet::ipv4(destination).empty() ? 0 : hostnet::ipv4(destination)[3],
                          unsigned(destination[2]) << 8 | destination[3],
                          hostnet::unreachable(destination) ? " (not carried)" : "");
            if (hostnet::unreachable(destination) || (hostnet::isIpv6(destination) && socket->icmp())) return uint64_t(-101);
            if (socket->hostFd < 0) socket->hostFd = hostnet::openDatagram(destination, socket->icmp());
            if (socket->hostFd < 0) return uint64_t(int64_t(std::exchange(socket->hostFd, -1)));
        } else if (const auto error = routeInetDestination(destination)) {
            return error;
        }
    } else if (!socket->connected) {
        return uint64_t(-89);
    }
    if (socket->sendShutdown) return uint64_t(-32);
    if (socket->hostFd >= 0) {
        // Out on the network: the host sends it.
        std::vector<uint8_t> payload;
        for (const auto& [base, length] : vectors) {
            if (length > 65507 - payload.size()) return uint64_t(-90);
            const size_t offset = payload.size();
            payload.resize(offset + length);
            if (length && !machine.readInto(base, std::span(payload).subspan(offset, length))) return uint64_t(-14);
        }
        if (!socket->localInetAddress[0] || !inetPort(socket->localInetAddress)) {
            socket->localInetAddress = hostnet::localAddress(socket->family, nextEphemeralPort, hostnet::isIpv6(destination));
            nextEphemeralPort = nextEphemeralPort >= 60999 ? 32768 : nextEphemeralPort + 1;
        }
        if (socket->icmp()) {
            // Only an echo request goes out of an echo socket.
            if (payload.size() < 8 || payload[0] != 8 || payload[1] != 0) return invalid;
            socket->icmpIdentifier = uint16_t(payload[4] << 8 | payload[5]);
            hostnet::sealIcmp(payload);
        }
        const auto transmit = [&] {
            return address || !socket->connected ? hostnet::sendTo(socket->hostFd, payload, destination)
                                                 : hostnet::send(socket->hostFd, payload);
        };
        const auto sent = transmit();
        NETWORK_TRACE("udp send %zu bytes -> %lld", payload.size(), static_cast<long long>(sent));
        return uint64_t(sent);
    }
    if (const auto error = guestRoutes.routeLoopback(asWireAddress(destination)[0], socket->socketMark,
                                                    socket->credentials[1])) return uint64_t(error);
    if (!inetLoopbackUp) return uint64_t(-100);
    const bool ipv4 = socket->family == 2;
    const size_t headerSize = ipv4 ? 28 : 48;
    size_t size = 0;
    for (const auto& [base, length] : vectors) {
        if (length > BpfProgram::maxPacketBytes - headerSize - size) return uint64_t(-90);
        size += length;
    }
    try {
        BpfProgram::Packet packet;
        packet.bytes.resize(headerSize + size);
        size_t offset = headerSize;
        for (const auto& [base, length] : vectors) {
            if (length && !machine.readInto(base, std::span(packet.bytes).subspan(offset, length))) return uint64_t(-14);
            offset += length;
        }
        if (!socket->localInetAddress[0]) {
            InetAddress local{};
            local[0] = socket->family;
            if (const auto error = reserveInetAddress(socket, local)) return error;
        }
        auto source = socket->localInetAddress;
        if (inetWildcard(source)) setInetLoopback(source);
        encodeLoopbackUdp(packet.bytes, source, destination);
        packet.protocol = ipv4 ? 0x0008 : 0xdd86;
        packet.mark = socket->socketMark;
        const auto peer = findInetDestination(socket->protocol, destination);
        packet.ifindex = guestLoopbackIndex;
        packet.socketLookup = [&](std::span<const uint8_t> tuple, bool tcp) {
            return lookupInetSocket(tuple, tcp);
        };
        const bool sent = filterInetPacket(inetEgressPrograms, *socket, packet, true);
        if (packet.ringNotification) wakePollers();
        if (!sent) return uint64_t(-1);
        const bool tracked = peer && firewall.tracksConnections();
        auto* connection = tracked ? trackDatagramConnection(socket, peer) : nullptr;
        if (tracked && !connection) return uint64_t(-105);
        if (connection && connection->first.lock() != socket) connection->sawReply = true;
        const auto status = connection && connection->sawReply ? GuestFirewall::establishedConnection :
                                                                GuestFirewall::newConnection;
        GuestFirewall::Rejection rejection;
        const GuestFirewall::LogPacket logger = [&](const auto& record) { emitNetfilterLog(record, *socket); };
        const GuestFirewall::RunBpf classify = [&](const auto& program, auto hook) {
            return matchFirewallBpf(program, hook, packet);
        };
        const GuestFirewall::PacketState state{&packet.mark, connection ? &connection->mark : nullptr,
                                                status, &rejection, &logger, &classify};
        const auto deliverRejection = [&] {
            if (!rejection.hard || !socket->connected || !matchInetPeer(socket->peerInetAddress, destination)) return;
            socket->inetError = rejection.error;
            wakePollers();
        };
        if (!firewall.allows(packet.bytes, GuestFirewall::output, guestLoopbackMtu, state) ||
            !firewall.allows(packet.bytes, GuestFirewall::postrouting, guestLoopbackMtu, state)) {
            if (connection && !connection->confirmed)
                datagramConnections.erase(std::minmax(socket->cookie, peer->cookie));
            deliverRejection();
            return uint64_t(-1);
        }

        if (connection) connection->confirmed = true;
        if (!peer || peer->receiveShutdown) return size;
        const auto reportedSource = socket->family == peer->family ? source : asMappedIpv4(source);
        if (peer->connected && !matchInetPeer(peer->peerInetAddress, reportedSource)) return size;
        if (!firewall.allows(packet.bytes, GuestFirewall::prerouting, guestLoopbackMtu, state) ||
            !firewall.allows(packet.bytes, GuestFirewall::input, guestLoopbackMtu, state)) {
            deliverRejection();
            return size;
        }
        packet.ingressIfindex = guestLoopbackIndex;
        const bool received = filterInetPacket(inetIngressPrograms, *peer, packet, false);
        if (packet.ringNotification) wakePollers();
        if (!received) return size;
        if (peer->datagrams.size() < 256 && peer->queuedBytes + size <= peer->receiveBuffer) {
            peer->datagrams.push_back({reportedSource, {packet.bytes.begin() + headerSize, packet.bytes.end()}});
            peer->queuedBytes += size;
            wakePollers();
        }
        return size;
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

// What the host has received for a datagram socket joins its queue, and is handed on like any other
// datagram. Nothing when one was added; otherwise what the caller is to return.
std::optional<uint64_t> ProbeLinux::pullHostDatagram(SocketState& socket) {
    SocketState::Datagram arrived;
    arrived.payload.resize(65536);
    const auto received = hostnet::receiveFrom(socket.hostFd, arrived.payload, socket.family, arrived.source);
    if (received == -11) {
        if (socket.receiveShutdown) return uint64_t{0};
        hostSocketWait = true;
        return uint64_t(-11);
    }
    NETWORK_TRACE("udp receive -> %lld", static_cast<long long>(received));
    if (received < 0) return uint64_t(received);
    arrived.payload.resize(size_t(received));
    if (socket.icmp()) {
        hostnet::stripIpHeader(arrived.payload);
        // The host hands an echo socket whatever ICMP arrives. Only the answers to this socket's own
        // requests are passed on: an echo reply with the identifier it last sent.
        if (arrived.payload.size() < 8 || arrived.payload[0] != 0 ||
            uint16_t(arrived.payload[4] << 8 | arrived.payload[5]) != socket.icmpIdentifier) {
            NETWORK_TRACE("icmp of type %u dropped", arrived.payload.empty() ? 255u : unsigned(arrived.payload[0]));
            hostSocketWait = true;
            return uint64_t(-11);
        }
        NETWORK_TRACE("icmp echo reply, %zu bytes", arrived.payload.size());
    }
    socket.queuedBytes += arrived.payload.size();
    socket.datagrams.push_back(std::move(arrived));
    return std::nullopt;
}

uint64_t ProbeLinux::receiveInetDatagram(Machine& machine, SocketState& socket, uint64_t address, uint64_t size,
    uint64_t flags, uint64_t source, uint64_t sourceLength) {
    if (socket.inetError) return uint64_t(-std::exchange(socket.inetError, 0));
    if (socket.hostFd >= 0 && socket.datagrams.empty()) {
        if (const auto pulled = pullHostDatagram(socket); pulled) return *pulled;
    }
    if (socket.datagrams.empty()) return socket.receiveShutdown ? 0 : uint64_t(-11);
    const auto& packet = socket.datagrams.front();
    const auto count = std::min<uint64_t>(size, packet.payload.size());
    if (source) {
        uint32_t capacity = 0;
        const auto length = uint32_t(socket.family == 2 ? 16 : 28);
        if (!machine.readInto(sourceLength, {reinterpret_cast<uint8_t*>(&capacity), sizeof(capacity)}))
            return uint64_t(-14);
        if (capacity > INT32_MAX) return invalid;
        auto reported = packet.source;
        filterInetAddress(socket, socket.family == 2 ? BpfProgram::receive4 : BpfProgram::receive6, reported);
        if ((capacity && !machine.write(source, std::span(reported).first(std::min(capacity, length)))) ||
            !writeGuest(machine, sourceLength, length)) return uint64_t(-14);
    }
    if (count && !machine.write(address, std::span(packet.payload).first(count))) return uint64_t(-14);
    const auto result = flags & 0x20 ? packet.payload.size() : count; // MSG_TRUNC
    if (!(flags & 2)) { // MSG_PEEK
        socket.queuedBytes -= packet.payload.size();
        socket.datagrams.pop_front();
    }
    return result;
}

static bool runInetOption(const auto& programs, const auto& socket, uint32_t uid, uint32_t gid,
                          uint32_t pid, uint32_t tid, BpfProgram::SocketOption& option) {
    BpfProgram::SocketContext context{socket.storageIdentity, socket.cookie, uid, gid,
        socket.family, socket.type, socket.protocol, {}, pid, tid};
    context.option = &option;
    const auto isCurrent = [&](const auto& attached, size_t depth) {
        return sameSocketGroup(socket, attached, depth);
    };
    return runInetPrograms(programs, socket.inetCgroup, context, isCurrent);
}

uint64_t ProbeLinux::getInetSocketOption(Machine& machine, SocketState& socket) {
    const auto& x = machine.cpu.x;
    uint32_t capacity = 0;
    if (!machine.readInto(x[4], {reinterpret_cast<uint8_t*>(&capacity), sizeof(capacity)})) return uint64_t(-14);
    if (capacity > INT32_MAX) return invalid;
    std::array<uint8_t, 16> original{};
    uint32_t size = 0;
    int32_t status = -92; // ENOPROTOOPT
    const auto store = [&](auto value) {
        size = std::min<uint32_t>(capacity, sizeof(value));
        std::memcpy(original.data(), &value, size);
        status = 0;
    };
    if (x[1] == 1 && x[2] == 4 && socket.hostFd >= 0) { // SO_ERROR, of a connection the host makes
        if (socket.hostConnecting) hostSocketEvents(socket);
        store(uint32_t(std::exchange(socket.inetError, 0)));
    } else if (x[1] == 41 && x[2] == 26 && socket.family == 10) { // IPV6_V6ONLY
        store(uint32_t(socket.inetV6Only));
    } else if (x[1] == 1) {
        switch (x[2]) {
            case 2: store(uint32_t(socket.reuseAddress)); break;
            case 3: store(socket.type); break;
            case 4: store(uint32_t(std::exchange(socket.inetError, 0))); break;
            case 7: store(socket.sendBuffer); break;
            case 8: store(socket.receiveBuffer); break;
            case 13: store(socket.linger); break;
            case 38: store(socket.protocol); break;
            case 36: store(socket.socketMark); break;
            case 39: store(socket.family); break;
            case 57:
                if (capacity < sizeof(socket.cookie)) status = -22;
                else store(socket.cookie);
                break;
            case sendTimeoutOld: case sendTimeoutNew: case receiveTimeoutOld: case receiveTimeoutNew: {
                const auto delay = x[2] == sendTimeoutOld || x[2] == sendTimeoutNew
                    ? socket.sendTimeout : socket.receiveTimeout;
                const auto usec = std::max<int64_t>(0, delay.count());
                store(std::array<int64_t, 2>{usec / 1'000'000, usec % 1'000'000});
                break;
            }
        }
    }
    const auto result = [&](int32_t error, std::span<const uint8_t> data) -> uint64_t {
        if (error) return uint64_t(error);
        return (data.empty() || machine.write(x[3], data)) && writeGuest(machine, x[4], uint32_t(data.size()))
            ? 0 : uint64_t(-14);
    };
    if (!inetGetOptionPrograms.empty()) {
        std::array<uint8_t, pageSize> bytes{};
        auto optionData = std::span(bytes).first(std::min<size_t>(capacity, bytes.size()));
        std::ranges::copy(std::span(original).first(size), optionData.begin());
        const auto originalLength = int32_t(status ? capacity : size);
        BpfProgram::SocketOption option{false, int32_t(x[1]), int32_t(x[2]), originalLength, status, optionData};
        if (!runInetOption(inetGetOptionPrograms, socket, userId(), groupId(), processId(), currentTid, option))
            return uint64_t(option.error < 0 && option.error >= -4095 ? option.error : -1);
        if (option.length < 0) return uint64_t(-14);
        if (size_t(option.length) > optionData.size()) {
            if (option.length != originalLength) return uint64_t(-14);
        } else if (option.length) {
            return result(option.retval, optionData.first(option.length));
        }
    }
    return result(status, std::span(original).first(size));
}

std::optional<uint64_t> ProbeLinux::setInetSocketOption(Machine& machine, SocketState& socket,
    int32_t& level, int32_t& name, uint32_t& size, std::vector<uint8_t>& buffer) {
    if (size > INT32_MAX) return invalid;
    if (inetSetOptionPrograms.empty()) return {};
    buffer.resize(std::min<size_t>(size, pageSize));
    if (!machine.readInto(machine.cpu.x[3], buffer)) return uint64_t(-14);
    BpfProgram::SocketOption option{true, level, name, int32_t(size), 0, buffer};
    if (!runInetOption(inetSetOptionPrograms, socket, userId(), groupId(), processId(), currentTid, option))
        return uint64_t(option.error < 0 && option.error >= -4095 ? option.error : -1);
    if (option.length == -1) return 0;
    if (option.length < -1 || (option.length != int32_t(size) && size_t(option.length) > buffer.size()))
        return uint64_t(-14);
    if (!option.length || size_t(option.length) > buffer.size()) {
        buffer.clear();
        return {};
    }
    level = option.level;
    name = option.name;
    size = option.length;
    buffer.resize(size);
    return {};
}

ProbeLinux::CgroupSocketHooks* ProbeLinux::bpfCgroupHooks(uint32_t type) {
    switch (type) {
        case BpfProgram::cgroupIngress: return &inetIngressPrograms;
        case BpfProgram::cgroupEgress: return &inetEgressPrograms;
        case BpfProgram::cgroupSocketOperations: return &inetSocketOperationPrograms;
        case BpfProgram::cgroupInetCreate: return &inetCreatePrograms;
        case BpfProgram::cgroupInetRelease: return inetReleasePrograms.get();
        case BpfProgram::getOption: return &inetGetOptionPrograms;
        case BpfProgram::setOption: return &inetSetOptionPrograms;
        default: return BpfProgram::inetAddressAttach(type) ? &inetAddressPrograms[type] : nullptr;
    }
}

uint64_t ProbeLinux::changeBpfCgroupAttachment(Machine& machine) {
    constexpr uint32_t allowOverride = 1, multi = 2;
    const auto& x = machine.cpu.x;
    if (!(current->process->effectiveCaps & (1ULL << 12))) return uint64_t(-1); // CAP_NET_ADMIN
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return uint64_t(-14);
    std::array<uint32_t, 4> fields{};
    std::memcpy(fields.data(), input.data(), sizeof(fields));
    const auto [targetFd, programFd, type, flags] = fields;
    const bool attach = x[0] == 8;
    const bool packet = type == BpfProgram::cgroupIngress || type == BpfProgram::cgroupEgress;
    const bool socketOperations = type == BpfProgram::cgroupSocketOperations;
    const bool address = BpfProgram::inetAddressAttach(type);
    const bool option = type == BpfProgram::getOption || type == BpfProgram::setOption;
    auto* selected = bpfCgroupHooks(type);
    if (!selected) return uint64_t(-95);
    auto& hooks = *selected;
    if (flags > multi || (!attach && flags) ||
        std::ranges::any_of(std::span(input).subspan(sizeof(fields)), [](auto byte) { return byte != 0; }))
        return invalid;
    const auto fd = descriptors().find(targetFd);
    if (fd == descriptors().end()) return uint64_t(-9);
    const auto& target = fd->second;
    if (!target.cgroupDirectory) return invalid;
    const auto live = cgroupFiles.find(target.path + std::string(cgroupProcs));
    if (live == cgroupFiles.end() || live->second != target.cgroupDirectory) return uint64_t(-19);
    const auto attachment = hooks.find(target.path);
    const auto source = descriptors().find(programFd);
    const auto program = source == descriptors().end() ? nullptr : source->second.bpfProgram;
    if (!attach) {
        if (attachment == hooks.end()) return uint64_t(-2);
        auto& programs = attachment->second.programs;
        if (attachment->second.flags & multi) {
            if (!program) return source == descriptors().end() ? uint64_t(-9) : invalid;
            if (!std::erase(programs, program)) return uint64_t(-2);
        } else {
            programs.clear();
        }
        if (programs.empty()) hooks.erase(attachment);
        return 0;
    }
    if (!program) return source == descriptors().end() ? uint64_t(-9) : invalid;
    const auto programType = packet ? BpfProgram::cgroupPacket :
        socketOperations ? BpfProgram::socketOperations : address ? BpfProgram::socketAddress :
        option ? BpfProgram::socketOption : BpfProgram::cgroupSocket;
    const auto expected = program->info().attachType;
    const bool legacyPacket = packet && expected == BpfProgram::cgroupIngress;
    if (program->type() != programType || (!legacyPacket && expected != type)) return invalid;
    for (const auto& [path, item] : hooks) {
        if (inside(target.path, path) && !(item.flags & (multi | allowOverride))) return uint64_t(-1);
        if (inside(path, target.path) && !flags) return uint64_t(-1);
    }
    if (attachment != hooks.end() && attachment->second.flags != flags) return uint64_t(-1);
    if (attachment == hooks.end()) {
        hooks.emplace(target.path, CgroupSocketPrograms{flags, target.cgroupDirectory, {program}});
    } else if (flags & multi) {
        auto& programs = attachment->second.programs;
        if (std::ranges::find(programs, program) != programs.end()) return uint64_t(-17);
        if (programs.size() == 64) return uint64_t(-7);
        programs.push_back(program);
    } else {
        attachment->second.programs.front() = program;
    }
    return 0;
}

uint64_t ProbeLinux::queryBpfCgroupPrograms(Machine& machine) {
    const auto& x = machine.cpu.x;
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return uint64_t(-14);
    constexpr uint64_t capNetAdmin = 1ULL << 12, capSysAdmin = 1ULL << 21;
    if (!(current->process->effectiveCaps & (capNetAdmin | capSysAdmin))) return uint64_t(-1);
    struct Request {
        uint32_t fd, type, flags, attachFlags;
        uint64_t ids;
        uint32_t count, padding;
        uint64_t programFlags;
    };
    static_assert(sizeof(Request) == 40 && offsetof(Request, programFlags) == 32);
    Request req{};
    std::memcpy(&req, input.data(), sizeof(req));
    const bool effective = req.flags & 1;
    if ((req.flags & ~1U) || (effective && req.programFlags) || req.padding ||
        std::any_of(input.begin() + sizeof(Request), input.end(), [](uint8_t byte) { return byte != 0; }))
        return invalid;
    const auto* hooks = bpfCgroupHooks(req.type);
    if (!hooks) return uint64_t(-95);
    const auto fd = descriptors().find(req.fd);
    if (fd == descriptors().end()) return uint64_t(-9);
    const auto& target = fd->second;
    if (!target.cgroupDirectory) return invalid;
    const auto live = cgroupFiles.find(target.path + std::string(cgroupProcs));
    if (live == cgroupFiles.end() || live->second != target.cgroupDirectory) return uint64_t(-19);

    std::vector<uint32_t> ids;
    uint32_t flags = 0;
    if (effective) {
        visitInetPrograms(*hooks, target.path, [](const auto&, size_t) { return true; },
            [&](const BpfProgram& program) { ids.push_back(program.info().id); return true; });
    } else if (const auto local = hooks->find(target.path); local != hooks->end()) {
        flags = local->second.flags;
        for (const auto& program : local->second.programs) ids.push_back(program->info().id);
    }
    if (!writeGuest(machine, x[1] + offsetof(Request, attachFlags), flags) ||
        !writeGuest(machine, x[1] + offsetof(Request, count), uint32_t(ids.size())))
        return uint64_t(-14);
    if (!req.ids || !req.count || ids.empty()) return 0;
    const auto copied = std::min<size_t>(ids.size(), req.count);
    if (!machine.write(req.ids, {reinterpret_cast<const uint8_t*>(ids.data()), copied * sizeof(ids.front())}))
        return uint64_t(-14);
    if (req.programFlags) {
        std::fill_n(ids.begin(), copied, flags);
        if (!machine.write(req.programFlags, {reinterpret_cast<const uint8_t*>(ids.data()), copied * sizeof(flags)}))
            return uint64_t(-14);
    }
    return copied < ids.size() ? uint64_t(-28) : 0;
}

size_t ProbeLinux::bpfMapBytes() {
    size_t bytes = 0;
    for (const auto& [id, weak] : bpfMaps) {
        if (const auto map = weak.lock()) {
            if (map->type == BpfMap::socketStorage) {
                std::erase_if(map->socketValues, [](const auto& entry) { return entry.first.expired(); });
                bytes += map->socketValues.size() * (map->valueSize + 128);
            }
            bytes += map->budgetBytes;
        }
    }
    return bytes;
}

bool ProbeLinux::canAllocateSocketStorage(size_t valueSize) {
    return bpfMapBytes() <= bpfMapBudget - (valueSize + 128);
}

uint64_t ProbeLinux::bpfHashBatch(Machine& machine, std::span<const uint8_t> input, bool remove) {
    struct Batch {
        uint64_t inputCursor, outputCursor, keys, values;
        uint32_t count, mapFd;
        uint64_t entryFlags, flags;
    };
    static_assert(sizeof(Batch) == 56);
    constexpr uint64_t fault = uint64_t(-14), end = uint64_t(-2);
    Batch batch{};
    std::memcpy(&batch, input.data(), sizeof(batch));
    if (batch.entryFlags || batch.flags ||
        std::any_of(input.begin() + sizeof(batch), input.end(), [](uint8_t byte) { return byte != 0; }))
        return invalid;
    const auto fd = descriptors().find(batch.mapFd);
    if (fd == descriptors().end()) return uint64_t(-9);
    if (!fd->second.bpfMap) return invalid;
    const auto access = fd->second.flags & 3;
    if (access == 1 || (remove && !access)) return uint64_t(-1);
    auto& map = *fd->second.bpfMap;
    if (map.type != 1 && map.type != 5 && map.type != BpfMap::lruHash) return uint64_t(-95);
    if (!batch.count) return 0;
    const auto countAddress = machine.dataAddress(machine.cpu.x[1]) + offsetof(Batch, count);
    if (!writeGuest(machine, countAddress, uint32_t{0})) return fault;

    uint32_t cursor = 0;
    if (batch.inputCursor &&
        !machine.readInto(batch.inputCursor, {reinterpret_cast<uint8_t*>(&cursor), sizeof(cursor)})) return fault;
    const auto bucketCount = std::bit_ceil(map.entryCount);
    if (cursor >= bucketCount) return end;
    try {
        // The cursor is an opaque bucket number and stays valid when a prior batch erased entries.
        std::vector<std::pair<uint32_t, BpfMap::Entries::iterator>> entries;
        entries.reserve(map.hash.size());
        for (auto entry = map.hash.begin(); entry != map.hash.end(); ++entry) {
            uint32_t hash = 2166136261U;
            for (const auto byte : entry->first) hash = (hash ^ byte) * 16777619U;
            const uint32_t bucket = hash & (bucketCount - 1);
            if (bucket >= cursor) entries.emplace_back(bucket, entry);
        }
        std::ranges::sort(entries, {}, [](const auto& entry) { return entry.first; });
        size_t count = 0;
        while (count < entries.size()) {
            const auto first = entries.begin() + count;
            const auto next = std::upper_bound(first, entries.end(), first->first,
                [](uint32_t bucket, const auto& entry) { return bucket < entry.first; });
            const auto throughBucket = size_t(next - entries.begin());
            if (throughBucket > batch.count) break;
            count = throughBucket;
        }
        const bool finished = count == entries.size();
        const auto nextCursor = finished ? bucketCount : entries[count].first;
        const auto keys = machine.dataAddress(batch.keys);
        const auto values = machine.dataAddress(batch.values);
        if (keys > UINT64_MAX - count * map.keySize || values > UINT64_MAX - count * map.valueStride) return fault;
        for (size_t i = 0; i < count; ++i) {
            const auto& [key, value] = *entries[i].second;
            if (!machine.write(keys + i * map.keySize, key) ||
                !machine.write(values + i * map.valueStride, value.bytes)) return fault;
        }
        if (!writeGuest(machine, batch.outputCursor, nextCursor) ||
            !writeGuest(machine, countAddress, uint32_t(count))) return fault;
        if (remove) {
            for (size_t i = 0; i < count; ++i) map.hash.erase(entries[i].second);
        }
        if (finished) return end;
        return count ? 0 : uint64_t(-28);
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

uint64_t ProbeLinux::bpfMapCall(Machine& machine) {
    constexpr uint64_t fault = uint64_t(-14), tooBig = uint64_t(-7);
    constexpr uint64_t create = 0, lookup = 1, update = 2, erase = 3, nextKey = 4;
    constexpr uint32_t hashType = 1, arrayType = 2, perCpuHash = 5, perCpuArray = 6, deviceHash = 25;
    constexpr uint32_t noPrealloc = 1, readOnly = 1 << 3, writeOnly = 1 << 4, programReadOnly = 1 << 7;
    constexpr uint32_t mappable = 1 << 10;
    constexpr uint64_t capBpf = 1ULL << 39, capSysAdmin = 1ULL << 21;
    const auto& x = machine.cpu.x;
    const auto command = x[0];
    if (kernelSettings.at("/proc/sys/kernel/unprivileged_bpf_disabled").value &&
        !(current->process->effectiveCaps & (capBpf | capSysAdmin)))
        return uint64_t(-1);

    if (x[2] > pageSize) return tooBig;
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return fault;
    const auto nonzero = [&](size_t first, size_t last) {
        const auto bytes = std::span(input).subspan(first, last - first);
        return std::any_of(bytes.begin(), bytes.end(), [](auto b) { return b != 0; });
    };

    if (command == 24 || command == 25)
        return bpfHashBatch(machine, input, command == 25);

    if (command == create) {
        // BPF_MAP_CREATE has five leading u32 fields and an optional 16-byte object name.
        std::array<uint32_t, 5> createFields{};
        std::memcpy(createFields.data(), input.data(), sizeof(createFields));
        const auto [type, keySize, valueSize, entries, flags] = createFields;
        const bool array = type == arrayType || type == perCpuArray;
        const bool perCpu = type == perCpuArray || type == perCpuHash;
        const bool device = type == deviceHash;
        const bool socket = type == BpfMap::socketStorage;
        const bool ring = type == BpfMap::ringBuffer;
        const bool prefix = type == BpfMap::prefixMap;
        const bool lru = type == BpfMap::lruHash;
        if (!array && !device && !socket && !ring && !prefix && !lru && type != hashType && type != perCpuHash)
            return uint64_t(-95);
        if ((socket || ring || prefix || lru) && !(current->process->effectiveCaps & (capBpf | capSysAdmin))) return uint64_t(-1);
        if (device && !(current->process->effectiveCaps & (1ULL << 12))) return uint64_t(-1); // CAP_NET_ADMIN
        const uint32_t arrayFlags = type == arrayType ? mappable | programReadOnly : 0;
        const uint32_t supportedFlags = readOnly | writeOnly | (device ? programReadOnly : array ? arrayFlags : noPrealloc);
        if (ring) {
            if (keySize || valueSize || entries < pageSize || (entries & (entries - 1)) || flags) return invalid;
        } else if (((array || device || socket) && keySize != sizeof(uint32_t)) || !keySize || !valueSize ||
                   (socket ? entries || !(flags & noPrealloc) : !entries) ||
                   (device && valueSize != 4 && valueSize != 8) || (flags & ~supportedFlags)) {
            return invalid;
        }
        if (lru && (flags & noPrealloc)) return uint64_t(-524);
        if (prefix && (keySize < BpfMap::minPrefixKey || keySize > BpfMap::maxPrefixKey || !(flags & noPrealloc)))
            return invalid;
        if ((flags & (readOnly | writeOnly)) == (readOnly | writeOnly)) return invalid;
        if (nonzero(20, 28) || nonzero(44, 48) || nonzero(60, input.size())) return uint64_t(-95);
        std::array<uint32_t, 3> btf{};
        std::memcpy(btf.data(), input.data() + 48, sizeof(btf));
        const auto [btfFd, keyType, valueType] = btf;
        std::shared_ptr<const BpfTypes> types;
        // libbpf can supply the ELF's BTF FD even when the ring has no typed elements.
        if (keyType || valueType || (btfFd && !ring)) {
            if (ring) return invalid;
            const auto found = descriptors().find(btfFd);
            if (found == descriptors().end()) return uint64_t(-9);
            types = found->second.btf;
            if (!types || types->typeSize(keyType) != keySize || types->typeSize(valueType) != valueSize)
                return invalid;
        }
        if (socket && (!types || !types->isInt32(keyType))) return invalid;
        if (prefix && types && !types->isStruct(keyType)) return invalid;
        if (socket && valueSize > 65535 - 128) return tooBig;
        // The mmap array ABI aligns array elements to eight bytes. Per-CPU syscalls copy
        // one aligned value for each possible guest CPU, of which this guest has one.
        const uint64_t valueStride = array || perCpu ? (uint64_t(valueSize) + 7) & ~uint64_t{7} : valueSize;
        const uint64_t entryBytes = valueStride + (array ? 0 : uint64_t(keySize) + 128);
        if (entries && !ring && entryBytes > bpfMapBudget / entries) return noMemory;
        const uint64_t valueBytes = ring ? 2 * (uint64_t(entries) + pageSize) : socket ? 128 : entryBytes * entries;
        const auto bytes = flags & mappable ? pageAlignedLength(valueBytes) : valueBytes;
        if (bytes > bpfMapBudget) return noMemory;
        const auto newFd = freeDescriptor();
        if (newFd == descriptorCapacity) return uint64_t(-24);
        std::erase_if(bpfMaps, [](const auto& entry) { return entry.second.expired(); });
        if (bpfMapBytes() > bpfMapBudget - bytes || bpfMaps.size() >= 256) return noMemory;
        if (nextBpfMapId >= INT32_MAX) return uint64_t(-28);
        try {
            auto map = std::make_shared<BpfMap>(prefix);
            map->id = nextBpfMapId;
            map->type = type;
            map->flags = (flags & ~(readOnly | writeOnly)) | (device ? programReadOnly : 0);
            map->entryCount = entries;
            map->valueSize = valueSize;
            map->btfKeyType = keyType;
            map->btfValueType = valueType;
            std::memcpy(map->name.data(), input.data() + 28, map->name.size() - 1);
            map->keySize = keySize;
            map->valueStride = valueStride;
            map->budgetBytes = bytes;
            map->array = array;
            map->types = std::move(types);
            if (array || ring) {
                map->values.pages.resize(bytes);
                if (map->values.placedMoves != map->values.pages.moves) map->values.replace();
                map->values.size = bytes;
            }
            const uint32_t access = flags & readOnly ? 0 : flags & writeOnly ? 1 : 2;
            Descriptor result{"anon_inode:bpf-map", {}, 0x80000 | access, Node::bpfMap};
            result.bpfMap = map;
            bpfMaps.emplace(nextBpfMapId++, map);
            descriptors().emplace(newFd, std::move(result));
            return newFd;
        } catch (const std::bad_alloc&) {
            return uint64_t(-12);
        }
    }

    const size_t last = command == erase ? 16 : command == nextKey ? 24 : 32;
    if (nonzero(last, input.size())) return invalid;
    uint32_t mapFd = 0;
    uint64_t key = 0, value = 0, flags = 0;
    std::memcpy(&mapFd, input.data(), sizeof(mapFd));
    std::memcpy(&key, input.data() + 8, sizeof(key));
    std::memcpy(&value, input.data() + 16, sizeof(value));
    std::memcpy(&flags, input.data() + 24, sizeof(flags));
    const auto fd = descriptors().find(mapFd);
    if (fd == descriptors().end()) return uint64_t(-9);
    if (!fd->second.bpfMap) return invalid;
    auto& map = *fd->second.bpfMap;
    const auto access = fd->second.flags & 3;
    if ((command == update || command == erase) ? access == 0 : access == 1) return uint64_t(-1);
    if (command == lookup && flags) return invalid;
    if (command == update && flags > 2) return invalid;
    if (map.type == BpfMap::ringBuffer) return uint64_t(-524);

    if (map.type == BpfMap::socketStorage) {
        if (command == nextKey) return uint64_t(-524);
        uint32_t socketFd = 0;
        if (!machine.readInto(key, {reinterpret_cast<uint8_t*>(&socketFd), sizeof(socketFd)})) return fault;
        const auto descriptor = descriptors().find(socketFd);
        if (descriptor == descriptors().end()) return uint64_t(-9);
        if (!descriptor->second.socket) return uint64_t(-88); // ENOTSOCK
        const auto& endpoint = descriptor->second.socket;
        const auto socket = endpoint->storageIdentity ? endpoint->storageIdentity : std::shared_ptr<void>(endpoint);
        const auto storage = map.socketValues.find(socket);
        if (command == lookup || command == erase) {
            if (storage == map.socketValues.end()) return uint64_t(-2);
            if (command == lookup) return machine.write(value, storage->second) ? 0 : fault;
            map.socketValues.erase(storage);
            return 0;
        }
        if (storage == map.socketValues.end() && flags == 2) return uint64_t(-2);
        if (storage != map.socketValues.end() && flags == 1) return uint64_t(-17);
        if (storage == map.socketValues.end() && !canAllocateSocketStorage(map.valueSize))
            return noMemory;
        try {
            std::vector<uint8_t> replacement(map.valueSize);
            if (!machine.readInto(value, replacement)) return fault;
            map.socketValues.insert_or_assign(socket, std::move(replacement));
            return 0;
        } catch (const std::bad_alloc&) {
            return noMemory;
        }
    }

    if (!map.array) {
        try {
            const bool hasKey = key || command != nextKey;
            std::vector<uint8_t> bytes(hasKey ? map.keySize : 0);
            if (hasKey && !machine.readInto(key, bytes)) return fault;
            const bool prefix = map.type == BpfMap::prefixMap;
            const bool validKey = hasKey && (!prefix || map.validPrefix(bytes));
            if (hasKey && !validKey && (command == update || command == erase)) return invalid;
            const auto entry = !validKey ? map.hash.end() : command == lookup ? map.findValue(bytes) : map.hash.find(bytes);
            if (command == nextKey) {
                const auto next = entry == map.hash.end() ? map.hash.begin() : std::next(entry);
                if (next == map.hash.end()) return uint64_t(-2);
                return machine.write(value, next->first) ? 0 : fault;
            }
            if (command == lookup || command == erase) {
                if (entry == map.hash.end()) return uint64_t(-2);
                if (command == lookup) return machine.write(value, entry->second.bytes) ? 0 : fault;
                map.hash.erase(entry);
                return 0;
            }
            const bool device = map.type == deviceHash;
            if (!device && entry == map.hash.end() && flags == 2) return uint64_t(-2); // BPF_EXIST
            if (entry != map.hash.end() && flags == 1) return uint64_t(-17); // BPF_NOEXIST
            if (entry == map.hash.end() && map.hash.size() == map.entryCount && map.type != BpfMap::lruHash)
                return prefix ? uint64_t(-28) : tooBig;
            std::vector<uint8_t> replacement(map.valueStride);
            if (!machine.readInto(value, replacement)) return fault;
            if (device) {
                uint32_t index = 0;
                std::memcpy(&index, replacement.data(), sizeof(index));
                if (index != guestLoopbackIndex) return invalid;
                if (replacement.size() == 8) {
                    int32_t program = 0;
                    std::memcpy(&program, replacement.data() + 4, sizeof(program));
                    if (program > 0) return uint64_t(-95); // XDP programs require a device backend.
                    std::fill(replacement.begin() + 4, replacement.end(), 0);
                }
            }
            map.storeHash(std::move(bytes), std::move(replacement), entry);
            return 0;
        } catch (const std::bad_alloc&) {
            return noMemory;
        }
    }

    uint32_t index = UINT32_MAX;
    if (key || command != nextKey) {
        if (!machine.readInto(key, {reinterpret_cast<uint8_t*>(&index), sizeof(index)})) return fault;
    }
    if (command == nextKey) {
        if (index == map.entryCount - 1) return uint64_t(-2);
        const uint32_t next = index < map.entryCount ? index + 1 : 0;
        return writeGuest(machine, value, next) ? 0 : fault;
    }
    if (command == erase) return invalid; // Array entries are allocated at creation.
    if (index >= map.entryCount) return command == update ? tooBig : uint64_t(-2);
    auto bytes = map.arrayElement(index);
    if (map.type != perCpuArray) bytes = bytes.first(map.valueSize);
    if (command == lookup) return machine.write(value, bytes) ? 0 : fault;
    if (flags == 1) return uint64_t(-17); // BPF_NOEXIST on a preallocated slot.
    // Read into a temporary buffer so a fault on a later guest page leaves the map unchanged.
    try {
        std::vector<uint8_t> replacement(bytes.size());
        if (!machine.readInto(value, replacement)) return fault;
        std::ranges::copy(replacement, bytes.begin());
        return 0;
    } catch (const std::bad_alloc&) {
        return uint64_t(-12);
    }
}

uint64_t ProbeLinux::loadBpfProgram(Machine& machine) {
    constexpr uint64_t capBpf = 1ULL << 39, capSysAdmin = 1ULL << 21;
    const auto& x = machine.cpu.x;
    if (kernelSettings.at("/proc/sys/kernel/unprivileged_bpf_disabled").value &&
        !(current->process->effectiveCaps & (capBpf | capSysAdmin)))
        return uint64_t(-1);
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return uint64_t(-14);
    uint32_t type = 0, count = 0, flags = 0;
    uint64_t source = 0, license = 0;
    std::memcpy(&type, input.data(), sizeof(type));
    std::memcpy(&count, input.data() + 4, sizeof(count));
    std::memcpy(&source, input.data() + 8, sizeof(source));
    std::memcpy(&license, input.data() + 16, sizeof(license));
    std::memcpy(&flags, input.data() + 44, sizeof(flags));
    if (!BpfProgram::supports(type)) return uint64_t(-95);
    const auto caps = current->process->effectiveCaps;
    constexpr uint64_t capNetAdmin = 1ULL << 12, capPerfmon = 1ULL << 38;
    if (type == BpfProgram::tracepoint) {
        if (!(caps & capSysAdmin) && (!(caps & capBpf) || !(caps & capPerfmon))) return uint64_t(-1);
    } else if (type != BpfProgram::socketFilter && (!(caps & (capBpf | capSysAdmin)) || !(caps & capNetAdmin))) {
        return uint64_t(-1);
    }
    uint32_t ifindex = 0, attachType = 0;
    std::memcpy(&ifindex, input.data() + 64, sizeof(ifindex));
    std::memcpy(&attachType, input.data() + 68, sizeof(attachType));
    const auto expected = type == BpfProgram::socketOperations ? BpfProgram::cgroupSocketOperations :
        type == BpfProgram::cgroupSocket ? BpfProgram::cgroupInetCreate : 0;
    const bool validAttach = type == BpfProgram::cgroupPacket ? attachType <= 1 :
        type == BpfProgram::socketAddress ? BpfProgram::inetAddressAttach(attachType) :
        type == BpfProgram::socketOption ? attachType == BpfProgram::getOption || attachType == BpfProgram::setOption :
        !attachType || attachType == expected ||
        (type == BpfProgram::cgroupSocket && attachType == BpfProgram::cgroupInetRelease);
    if ((flags & ~3U) || flags == 3 || ifindex || !validAttach ||
        std::any_of(input.begin() + 108, input.end(), [](auto b) { return b != 0; }))
        return uint64_t(-95);
    if (!count || count > BpfProgram::maxInstructions) return uint64_t(-7);
    const auto licenseText = readPath(machine, license);
    if (!licenseText) return uint64_t(-14);
    const auto fd = freeDescriptor();
    if (fd == descriptorCapacity) return uint64_t(-24);
    std::erase_if(bpfPrograms, [](const auto& filter) { return filter.second.expired(); });
    if (bpfPrograms.size() >= 256) return noMemory;
    if (nextBpfProgramId >= INT32_MAX) return uint64_t(-28);
    try {
        std::vector<uint8_t> bytecode(count * 8);
        if (!machine.readInto(source, bytecode)) return uint64_t(-14);
        constexpr std::array<std::string_view, 6> gplLicenses{
            "GPL", "GPL v2", "GPL and additional rights", "Dual BSD/GPL", "Dual MIT/GPL", "Dual MPL/GPL"};
        BpfProgram::Identity identity{};
        identity.id = nextBpfProgramId;
        identity.uid = current->process->userIds[1];
        identity.attachType = attachType ? attachType : expected;
        identity.loadTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        identity.gplCompatible = std::ranges::find(gplLicenses, *licenseText) != gplLicenses.end();
        std::memcpy(identity.name.data(), input.data() + 48, identity.name.size() - 1);
        BpfProgram::DebugInfo debug;
        struct DebugAttributes {
            uint32_t btfFd, functionSize;
            uint64_t functions;
            uint32_t functionCount, lineSize;
            uint64_t lines;
            uint32_t lineCount;
        };
        static_assert(sizeof(DebugAttributes) == 40 && offsetof(DebugAttributes, lineCount) == 32);
        DebugAttributes metadata{};
        std::memcpy(&metadata, input.data() + 72, 36);
        if (metadata.functionCount || metadata.lineCount) {
            if (!(caps & (capBpf | capSysAdmin))) return uint64_t(-1);
            const auto btf = descriptors().find(metadata.btfFd);
            if (btf == descriptors().end()) return uint64_t(-9);
            if (!btf->second.btf) return invalid;
            debug.types = btf->second.btf;
            if (metadata.functionCount) {
                if (metadata.functionCount > count || metadata.functionSize != sizeof(BpfProgram::FunctionInfo))
                    return invalid;
                debug.functions.resize(metadata.functionCount);
                auto bytes = std::span(reinterpret_cast<uint8_t*>(debug.functions.data()),
                                       metadata.functionCount * metadata.functionSize);
                if (!machine.readInto(metadata.functions, bytes)) return uint64_t(-14);
                if (debug.functions.front()[0]) return invalid;
                uint32_t minimum = 0;
                for (const auto& [slot, function] : debug.functions) {
                    if (slot >= count || slot < minimum || !bytecode[size_t(slot) * 8] ||
                        !debug.types->isFunction(function)) return invalid;
                    minimum = slot + 1;
                }
            }
            if (metadata.lineCount) {
                if (metadata.lineCount > count || metadata.lineSize != sizeof(BpfProgram::LineInfo)) return invalid;
                debug.lines.resize(metadata.lineCount);
                auto bytes = std::span(reinterpret_cast<uint8_t*>(debug.lines.data()), metadata.lineCount * metadata.lineSize);
                if (!machine.readInto(metadata.lines, bytes)) return uint64_t(-14);
                if (debug.lines.front()[0]) return invalid;
                uint32_t minimum = 0;
                for (const auto& record : debug.lines) {
                    const auto slot = record[0];
                    if (slot >= count || slot < minimum || !bytecode[size_t(slot) * 8] ||
                        !debug.types->hasString(record[1]) || !debug.types->hasString(record[2])) return invalid;
                    minimum = slot + 1;
                }
            }
        }
        auto program = std::make_shared<BpfProgram>(identity, std::move(debug));
        if (!program->load(type, bytecode, [&](uint32_t mapFd) -> std::shared_ptr<BpfMap> {
                const auto found = descriptors().find(mapFd);
                return found == descriptors().end() ? nullptr : found->second.bpfMap;
            })) return invalid;
        Descriptor result{"anon_inode:bpf-prog", {}, 0x80002, Node::bpfProgram};
        result.bpfProgram = program;
        bpfPrograms.emplace(nextBpfProgramId++, program);
        descriptors().emplace(fd, std::move(result));
        return fd;
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

uint64_t ProbeLinux::testBpfProgram(Machine& machine) {
    constexpr uint64_t fault = uint64_t(-14);
    const auto& x = machine.cpu.x;
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return fault;
    struct Test {
        uint32_t fd, action, inputSize, outputSize;
        uint64_t input, output;
        uint32_t repeat, duration;
    };
    static_assert(sizeof(Test) == 40);
    Test test{};
    std::memcpy(&test, input.data(), sizeof(test));
    if (std::any_of(input.begin() + sizeof(Test), input.end(), [](auto byte) { return byte != 0; }) ||
        (!test.output && test.outputSize) || test.inputSize < 14 ||
        test.inputSize > BpfProgram::maxPacketBytes || test.repeat > 64) return invalid;

    const auto fd = descriptors().find(test.fd);
    if (fd == descriptors().end()) return uint64_t(-9);
    const auto& program = fd->second.bpfProgram;
    if (!program || (!BpfProgram::packetProgram(program->type()) && program->type() != BpfProgram::socketFilter))
        return invalid;
    try {
        BpfProgram::Packet packet;
        packet.bytes.resize(test.inputSize);
        if (!machine.readInto(test.input, packet.bytes)) return fault;
        // The Linux test constructs an Ethernet skb on the loopback device.
        const auto type = (uint32_t(packet.bytes[12]) << 8) | packet.bytes[13];
        if (type >= 0x600) {
            packet.protocol = packet.bytes[12] | (uint32_t(packet.bytes[13]) << 8);
        } else {
            const bool ipx = packet.bytes.size() >= 16 && packet.bytes[14] == 255 && packet.bytes[15] == 255;
            packet.protocol = ipx ? 0x0100 : 0x0400; // ETH_P_802_3 or ETH_P_802_2, in network order.
        }
        if (packet.bytes[0] & 1) {
            packet.packetType = std::all_of(packet.bytes.begin(), packet.bytes.begin() + 6,
                [](auto b) { return b == 255; }) ? 1 : 2;
        } else if (std::any_of(packet.bytes.begin(), packet.bytes.begin() + 6, [](auto b) { return b != 0; })) {
            packet.packetType = 3; // PACKET_OTHERHOST
        }
        packet.ifindex = guestLoopbackIndex;
        packet.networkOffset = 14;
        packet.hasMacHeader = true;
        packet.socketCookie = nextSocketCookie++;
        const auto repetitions = std::max(1U, test.repeat);
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t run = 0; run < repetitions; ++run) {
            packet.ringNotification = false;
            const auto result = program->classify(packet);
            if (packet.ringNotification) wakePollers();
            if (!result) return invalid;
            test.action = *result;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count() / repetitions;
        test.duration = std::clamp<int64_t>(elapsed, 0, UINT32_MAX);
        const size_t capacity = test.outputSize;
        test.outputSize = packet.bytes.size();
        if (test.output) {
            const auto copySize = capacity ? std::min<size_t>(capacity, packet.bytes.size()) : packet.bytes.size();
            if (!machine.write(test.output, std::span(packet.bytes).first(copySize))) return fault;
        }
        if (!writeGuest(machine, x[1] + 4, test.action) || !writeGuest(machine, x[1] + 12, test.outputSize) ||
            !writeGuest(machine, x[1] + 36, test.duration)) return fault;
        return test.output && capacity && capacity < test.outputSize ? uint64_t(-28) : 0;
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

uint64_t ProbeLinux::bpfObjectIdCall(Machine& machine) {
    constexpr uint64_t nextProgram = 11, nextMap = 12, openProgram = 13, openMap = 14;
    constexpr uint32_t readOnly = 1 << 3, writeOnly = 1 << 4;
    const auto& x = machine.cpu.x;
    const bool enumerate = x[0] == nextProgram || x[0] == nextMap;
    const bool program = x[0] == nextProgram || x[0] == openProgram;
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> attr{};
    if (!machine.readInto(x[1], std::span(attr).first(x[2]))) return uint64_t(-14);
    const size_t used = enumerate ? 8 : program ? 4 : 12;
    if (std::any_of(attr.begin() + used, attr.end(), [](auto byte) { return byte != 0; })) return invalid;

    uint32_t id = 0, flags = 0;
    std::memcpy(&id, attr.data(), sizeof(id));
    if (x[0] == openMap) std::memcpy(&flags, attr.data() + 8, sizeof(flags));
    if ((enumerate && id >= INT32_MAX) || (flags & ~(readOnly | writeOnly)) || flags == (readOnly | writeOnly))
        return invalid;
    if (!(current->process->effectiveCaps & (1ULL << 21))) return uint64_t(-1); // CAP_SYS_ADMIN

    if (enumerate) {
        const auto nextId = [id](const auto& objects) -> uint32_t {
            for (auto it = objects.upper_bound(id); it != objects.end(); ++it)
                if (!it->second.expired()) return it->first;
            return 0;
        };
        const auto next = program ? nextId(bpfPrograms) : nextId(bpfMaps);
        if (!next) return uint64_t(-2);
        return writeGuest(machine, x[1] + 4, next) ? 0 : uint64_t(-14);
    }

    std::shared_ptr<const BpfProgram> filter;
    std::shared_ptr<BpfMap> map;
    if (program) {
        const auto found = bpfPrograms.find(id);
        if (found != bpfPrograms.end()) filter = found->second.lock();
    } else {
        const auto found = bpfMaps.find(id);
        if (found != bpfMaps.end()) map = found->second.lock();
    }
    if (!filter && !map) return uint64_t(-2);
    const auto fd = freeDescriptor();
    if (fd == descriptorCapacity) return uint64_t(-24);
    const uint32_t access = flags & readOnly ? 0 : flags & writeOnly ? 1 : 2;
    Descriptor entry{program ? "anon_inode:bpf-prog" : "anon_inode:bpf-map", {},
        0x80000 | access, program ? Node::bpfProgram : Node::bpfMap};
    entry.bpfMap = std::move(map);
    entry.bpfProgram = std::move(filter);
    descriptors().emplace(fd, std::move(entry));
    return fd;
}

uint64_t ProbeLinux::loadBpfTypes(Machine& machine) {
    constexpr size_t budget = 8 * 1024 * 1024, maxObject = 1024 * 1024;
    constexpr uint64_t capBpf = 1ULL << 39, capSysAdmin = 1ULL << 21;
    const auto& x = machine.cpu.x;
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return uint64_t(-14);
    if (std::any_of(input.begin() + 32, input.end(), [](auto byte) { return byte != 0; })) return invalid;
    if (!(current->process->effectiveCaps & (capBpf | capSysAdmin))) return uint64_t(-1);

    struct Attributes {
        uint64_t address, logAddress;
        uint32_t size, logSize, logLevel, logTrueSize;
    };
    Attributes attr{};
    std::memcpy(&attr, input.data(), sizeof(attr));
    if (attr.size > maxObject) return uint64_t(-7);
    if (!attr.size || (attr.logLevel & ~7U) || (attr.logLevel && (!attr.logAddress || !attr.logSize)))
        return invalid;
    std::erase_if(bpfTypes, [](const auto& entry) { return entry.expired(); });
    size_t used = 0;
    for (const auto& entry : bpfTypes)
        if (const auto types = entry.lock()) used += types->bytes().size();
    if (used > budget - attr.size || bpfTypes.size() >= 64) return noMemory;
    if (nextBpfTypeId >= INT32_MAX) return uint64_t(-28);
    const auto fd = freeDescriptor();
    if (fd == descriptorCapacity) return uint64_t(-24);
    try {
        std::vector<uint8_t> bytes(attr.size);
        if (!machine.readInto(attr.address, bytes)) return uint64_t(-14);
        auto types = std::make_shared<BpfTypes>(nextBpfTypeId);
        const bool valid = types->load(std::move(bytes));
        if (attr.logLevel) {
            const std::string_view message = valid ? "" : "invalid BPF type metadata";
            const auto count = uint32_t(message.size() + 1);
            const auto data = std::span(reinterpret_cast<const uint8_t*>(message.data()), count);
            if (!machine.write(attr.logAddress, data.first(std::min(count, attr.logSize)))) return uint64_t(-14);
            if (x[2] >= sizeof(attr) && !writeGuest(machine, x[1] + 28, count)) return uint64_t(-14);
        }
        if (!valid) return invalid;
        Descriptor entry{"anon_inode:bpf-btf", {}, 0x80000, Node::bpfBtf};
        entry.btf = types;
        bpfTypes.emplace_back(types);
        ++nextBpfTypeId;
        descriptors().emplace(fd, std::move(entry));
        return fd;
    } catch (const std::bad_alloc&) {
        return noMemory;
    }
}

uint64_t ProbeLinux::bpfObjectInfo(Machine& machine) {
    const auto& x = machine.cpu.x;
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> attr{};
    if (!machine.readInto(x[1], std::span(attr).first(x[2]))) return uint64_t(-14);
    if (std::any_of(attr.begin() + 16, attr.end(), [](auto b) { return b != 0; })) return invalid;
    uint32_t fd = 0, length = 0;
    uint64_t address = 0;
    std::memcpy(&fd, attr.data(), sizeof(fd));
    std::memcpy(&length, attr.data() + 4, sizeof(length));
    std::memcpy(&address, attr.data() + 8, sizeof(address));
    if (length > pageSize) return uint64_t(-7);
    const auto descriptor = descriptors().find(fd);
    if (descriptor == descriptors().end()) return uint64_t(-77); // EBADFD

    std::array<uint8_t, pageSize> request{};
    if (!machine.readInto(address, std::span(request).first(length))) return uint64_t(-14);
    const auto validTail = [&](size_t used) {
        return length <= used || std::none_of(request.begin() + used, request.begin() + length,
                                              [](auto byte) { return byte != 0; });
    };
    const auto writeInfo = [&](const auto& info) {
        const auto size = uint32_t(std::min<size_t>(length, sizeof(info)));
        const auto output = std::span(reinterpret_cast<const uint8_t*>(&info), size);
        return machine.write(address, output) && writeGuest(machine, x[1] + 4, size) ? 0 : uint64_t(-14);
    };

    if (const auto program = descriptor->second.bpfProgram) {
        struct Info {
            uint32_t type, id;
            std::array<uint8_t, 8> tag;
            uint32_t jitLength, translatedLength;
            uint64_t jitBytes, translatedBytes, loadTime;
            uint32_t uid, mapCount;
            uint64_t mapIds;
            std::array<char, 16> name;
            uint32_t ifindex, gplCompatible;
            uint64_t namespaceDevice, namespaceInode;
            uint32_t jitSymbolCount, jitFunctionCount;
            uint64_t jitSymbols, jitFunctions;
            uint32_t btf, functionSize;
            uint64_t functions;
            uint32_t functionCount, lineCount;
            uint64_t lines, jitLines;
            uint32_t jitLineCount, lineSize;
        };
        static_assert(sizeof(Info) == 176 && offsetof(Info, mapIds) == 56 && offsetof(Info, btf) == 128);
        if (!validTail(sizeof(Info))) return uint64_t(-95);
        Info info{};
        std::memcpy(&info, request.data(), std::min<size_t>(length, sizeof(info)));
        if (info.jitBytes || info.translatedBytes || info.jitSymbols || info.jitFunctions || info.jitLines)
            return uint64_t(-95);
        try {
            const auto ids = program->mapIds();
            const auto size = std::min<size_t>(info.mapCount, ids.size()) * sizeof(uint32_t);
            if (size && !machine.write(info.mapIds, {reinterpret_cast<const uint8_t*>(ids.data()), size}))
                return uint64_t(-14);
            const auto& identity = program->info();
            info.type = program->type();
            info.id = identity.id;
            const auto code = program->bytecode();
            std::array<uint8_t, CC_SHA1_DIGEST_LENGTH> digest{};
            // This is the Linux 6.12 program tag format, a bytecode fingerprint.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            CC_SHA1(code.data(), static_cast<CC_LONG>(code.size()), digest.data());
#pragma clang diagnostic pop
            std::copy_n(digest.begin(), info.tag.size(), info.tag.begin());
            info.jitLength = 0;
            constexpr uint64_t bpfCapabilities = (1ULL << 39) | (1ULL << 21);
            const bool inspect = current->process->effectiveCaps & bpfCapabilities;
            info.translatedLength = inspect ? code.size() : 0;
            info.loadTime = identity.loadTime;
            info.uid = identity.uid;
            info.mapCount = ids.size();
            info.name = identity.name;
            info.ifindex = 0;
            info.gplCompatible = identity.gplCompatible;
            info.namespaceDevice = info.namespaceInode = 0;
            info.jitSymbolCount = info.jitFunctionCount = info.jitLineCount = 0;
            const auto& debug = program->debugInfo();
            info.btf = debug.types ? debug.types->id() : 0;
            const auto copyRecords = [&](const auto& records, uint32_t& count, uint32_t& stride, uint64_t where) {
                const auto requested = inspect ? std::min<size_t>(count, records.size()) : 0;
                const auto width = sizeof(typename std::decay_t<decltype(records)>::value_type);
                if (count && stride != width) return invalid;
                if (requested && !machine.write(where,
                        {reinterpret_cast<const uint8_t*>(records.data()), requested * width})) return uint64_t(-14);
                count = inspect ? records.size() : 0;
                stride = width;
                return uint64_t{0};
            };
            if (const auto result = copyRecords(debug.functions, info.functionCount, info.functionSize, info.functions))
                return result;
            if (const auto result = copyRecords(debug.lines, info.lineCount, info.lineSize, info.lines)) return result;
            return writeInfo(info);
        } catch (const std::bad_alloc&) {
            return noMemory;
        }
    }
    if (const auto map = descriptor->second.bpfMap) {
        struct Info {
            uint32_t type, id, key, value, entries, flags;
            std::array<char, 16> name;
            uint32_t ifindex, vmlinuxValueType;
            uint64_t namespaceDevice, namespaceInode;
            uint32_t btf, keyType, valueType, vmlinuxBtf;
            uint64_t extra;
        };
        static_assert(sizeof(Info) == 88);
        if (!validTail(sizeof(Info))) return uint64_t(-7);
        const Info info{map->type, map->id, map->keySize, map->valueSize, map->entryCount, map->flags,
            map->name, 0, 0, 0, 0, map->types ? map->types->id() : 0, map->btfKeyType, map->btfValueType, 0, 0};
        return writeInfo(info);
    }
    if (const auto types = descriptor->second.btf) {
        struct Info {
            uint64_t address;
            uint32_t size, id;
            uint64_t nameAddress;
            uint32_t nameSize, kernel;
        };
        static_assert(sizeof(Info) == 32);
        if (!validTail(sizeof(Info))) return uint64_t(-7);
        Info info{};
        std::memcpy(&info, request.data(), std::min<size_t>(length, sizeof(info)));
        const auto bytes = types->bytes();
        const auto copy = std::min<size_t>(info.size, bytes.size());
        if (copy && !machine.write(info.address, bytes.first(copy))) return uint64_t(-14);
        info.size = bytes.size();
        info.id = types->id();
        info.nameSize = 0;
        info.kernel = 0;
        return writeInfo(info);
    }
    return invalid;
}

uint64_t ProbeLinux::createBpfToken(Machine& machine) {
    const auto& x = machine.cpu.x;
    if (x[2] > pageSize) return uint64_t(-7);
    std::array<uint8_t, pageSize> input{};
    if (!machine.readInto(x[1], std::span(input).first(x[2]))) return uint64_t(-14);
    uint32_t flags = 0, rootFd = 0;
    std::memcpy(&flags, input.data(), sizeof(flags));
    std::memcpy(&rootFd, input.data() + sizeof(flags), sizeof(rootFd));
    if (flags || std::any_of(input.begin() + 8, input.end(), [](auto byte) { return byte != 0; }))
        return invalid;
    const auto fd = descriptors().find(rootFd);
    if (fd == descriptors().end() || fd->second.pathOnly()) return uint64_t(-9);
    if (!fd->second.bpfFilesystemRoot) return invalid;

    constexpr uint64_t capBpf = 1ULL << 39;
    if (!(current->process->effectiveCaps & capBpf)) return uint64_t(-1);
    // The guest has only the initial user namespace, where Linux rejects token creation.
    return uint64_t(-95);
}

bool ProbeLinux::syscall(Machine& machine) {
    if (!hostCpuSample) accountGuestCpu();
    auto& x = machine.cpu.x;
    const uint64_t number = x[8];
    current->hostEventRetry.reset();
    current->idleBinder.reset();
    current->waitingBinder.reset();
    sharedStorageCall = false;
    if (enforceSeccomp(machine)) return true;
    if (number == 139) return restoreSignalContext(machine); // rt_sigreturn
    if (securityCall(machine) || socketCall(machine) || eventCall(machine) || fileCall(machine)) return true;
    const auto descriptor = descriptors().find(uint32_t(x[0]));
    switch (number) {
        case 425: case 426: case 427: // io_uring is not available in the guest.
            x[0] = uint64_t(-38);
            return true;
        case 280: { // bpf
            if (x[0] <= 4 || x[0] == 24 || x[0] == 25) { x[0] = bpfMapCall(machine); break; }
            if (x[0] == 5) { x[0] = loadBpfProgram(machine); break; }
            if (x[0] == 8 || x[0] == 9) { x[0] = changeBpfCgroupAttachment(machine); break; }
            if (x[0] == 10) { x[0] = testBpfProgram(machine); break; }
            if (x[0] >= 11 && x[0] <= 14) { x[0] = bpfObjectIdCall(machine); break; }
            if (x[0] == 15) { x[0] = bpfObjectInfo(machine); break; }
            if (x[0] == 16) { x[0] = queryBpfCgroupPrograms(machine); break; }
            if (x[0] == 18) { x[0] = loadBpfTypes(machine); break; }
            if (x[0] == 36) { x[0] = createBpfToken(machine); break; }
            const bool pin = x[0] == 6; // BPF_OBJ_PIN
            if (!pin && x[0] != 7) return false; // BPF_OBJ_GET
            if (x[2] > pageSize) { x[0] = uint64_t(-7); break; }
            std::array<uint8_t, pageSize> input{};
            if (!machine.readInto(x[1], std::span(input).first(x[2]))) { x[0] = uint64_t(-14); break; }
            uint64_t pathname = 0;
            uint32_t object = 0, flags = 0;
            int32_t directory = 0;
            std::memcpy(&pathname, input.data(), 8);
            std::memcpy(&object, input.data() + 8, 4);
            std::memcpy(&flags, input.data() + 12, 4);
            std::memcpy(&directory, input.data() + 16, 4);
            constexpr uint32_t readOnly = 8, writeOnly = 16, pathFd = 1 << 14;
            const auto validFlags = pin ? pathFd : readOnly | writeOnly | pathFd;
            if ((!pin && object) || (flags & ~validFlags) ||
                ((flags & (readOnly | writeOnly)) == (readOnly | writeOnly)) || (!(flags & pathFd) && directory) ||
                std::any_of(input.begin() + 20, input.begin() + std::max<uint64_t>(20, x[2]),
                            [](auto byte) { return byte != 0; })) {
                x[0] = invalid;
                break;
            }
            const auto rawPath = readPath(machine, pathname);
            if (!rawPath) { x[0] = uint64_t(-14); break; }
            if (rawPath->empty()) { x[0] = uint64_t(-2); break; }
            auto path = *rawPath;
            std::string absolute;
            if (const auto error = resolveAtPath(flags & pathFd ? uint32_t(directory) : uint32_t(-100), path, absolute)) {
                x[0] = error;
                break;
            }
            path = std::move(absolute);
            const auto target = followLinks(std::move(path), !pin);
            if (!target) { x[0] = uint64_t(-40); break; }
            const auto backing = backingPath(*target);
            const auto existing = find(backing, 0);
            if (!canAccessParent(backing, pin)) { x[0] = uint64_t(-13); break; }
            if (pin) {
                const auto fd = descriptors().find(object);
                if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
                if (!fd->second.bpfMap && !fd->second.bpfProgram) { x[0] = invalid; break; }
                const auto* mount = mountFor(*target);
                if (!mount || mount->device != "bpf") { x[0] = invalid; break; }
                if (!writablePath(*target)) { x[0] = uint64_t(-30); break; }
                if (existing || resolve(*target)) { x[0] = uint64_t(-17); break; }
                if (!directoryExists(backing.substr(0, backing.rfind('/')))) { x[0] = uint64_t(-2); break; }
                // bpffs stores references to kernel objects, independently of regular file quotas.
                if (writableFiles.size() >= maxBpfPins &&
                    size_t(std::count_if(writableFiles.begin(), writableFiles.end(), [](const auto& entry) {
                        return entry.second->pinnedBpfObject();
                    })) >= maxBpfPins) {
                    x[0] = uint64_t(-28);
                    break;
                }
                auto inode = std::make_shared<WritableFile>();
                inode->inode = nextInode++;
                inode->mode = 0600 & ~current->process->umask;
                inode->uid = userId();
                inode->gid = groupId();
                inode->pinnedMap = fd->second.bpfMap;
                inode->pinnedProgram = fd->second.bpfProgram;
                if (const auto& context = current->fsCreateContext; !context.empty())
                    inode->attributes["security.selinux"].assign(context.c_str(), context.c_str() + context.size() + 1);
                writableFiles.emplace(backing, std::move(inode));
                fileEvent(backing, fileCreated);
                x[0] = 0;
                break;
            }
            if (!existing) { x[0] = uint64_t(-2); break; }
            if (!existing->temporary || !existing->temporary->pinnedBpfObject()) { x[0] = uint64_t(-13); break; }
            const auto& inode = *existing->temporary;
            const auto access = flags & readOnly ? 0 : flags & writeOnly ? 1 : 2;
            const unsigned required = access == 0 ? 4 : access == 1 ? 2 : 6;
            if (userId() != 0 && ((inode.mode >> permissionShift(inode.uid, inode.gid)) & required) != required) {
                x[0] = uint64_t(-13);
                break;
            }
            const auto newFd = freeDescriptor();
            if (newFd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            const bool program = bool(inode.pinnedProgram);
            Descriptor result{program ? "anon_inode:bpf-prog" : "anon_inode:bpf-map", {},
                uint32_t(0x80000 | (program ? 2 : access)), program ? Node::bpfProgram : Node::bpfMap};
            result.bpfMap = inode.pinnedMap;
            result.bpfProgram = inode.pinnedProgram;
            descriptors().emplace(newFd, std::move(result));
            x[0] = newFd;
            break;
        }
        case 17: { // getcwd
            const auto& directory = current->process->workingDirectory;
            if (!directoryExists(backingPath(directory))) { x[0] = uint64_t(-2); break; }
            const auto size = directory.size() + 1;
            if (x[1] < size) { x[0] = uint64_t(-34); break; }
            const auto* bytes = reinterpret_cast<const uint8_t*>(directory.c_str());
            x[0] = machine.write(x[0], {bytes, size}) ? size : uint64_t(-14);
            break;
        }
        case 49: { // chdir
            const auto path = readPath(machine, x[0]);
            x[0] = !path ? uint64_t(-14) : path->empty() ? uint64_t(-2) :
                changeWorkingDirectory(absolutePath(*path));
            break;
        }
        case 50: { // fchdir
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            const auto& entry = descriptor->second;
            x[0] = entry.node != Node::directory ? uint64_t(-20) :
                changeWorkingDirectory(entry.visiblePath.empty() ? entry.path : entry.visiblePath);
            break;
        }
        case 270: { // process_vm_readv, restricted to the caller's guest address space.
            if (x[0] != processId()) { x[0] = uint64_t(-3); break; }
            if (x[5] || x[2] > 64 || x[4] > 64) { x[0] = invalid; break; }
            struct Vector { uint64_t address, size; };
            const auto loadVectors = [&](uint64_t address, uint64_t count, std::array<Vector, 64>& vectors) {
                if (!count) return true;
                const auto bytes = count * sizeof(Vector);
                std::vector<uint8_t> sourceScratch;
                const auto* source = machine.read(address, bytes, 4, sourceScratch);
                if (!source) return false;
                std::memcpy(vectors.data(), source, bytes);
                uint64_t total = 0;
                for (size_t i = 0; i < count; ++i) {
                    if (vectors[i].size > INT64_MAX - total ||
                        vectors[i].size > UINT64_MAX - vectors[i].address) return false;
                    total += vectors[i].size;
                }
                return true;
            };
            std::array<Vector, 64> local{}, remote{};
            if (!loadVectors(x[1], x[2], local) || !loadVectors(x[3], x[4], remote)) {
                x[0] = uint64_t(-14); break;
            }
            size_t from = 0, to = 0;
            uint64_t copied = 0;
            bool failed = false;
            std::array<uint8_t, pageSize> buffer;
            while (from < x[4] && to < x[2]) {
                auto& source = remote[from];
                auto& dest = local[to];
                if (!source.size) { ++from; continue; }
                if (!dest.size) { ++to; continue; }
                const auto count = std::min({source.size, dest.size,
                    pageSize - source.address % pageSize, pageSize - dest.address % pageSize});
                std::vector<uint8_t> inputScratch;
                const auto* input = machine.read(source.address, count, 4, inputScratch);
                if (!input) { failed = true; break; }
                std::memcpy(buffer.data(), input, count);
                if (!machine.write(dest.address, std::span(buffer).first(count))) { failed = true; break; }
                source.address += count;
                source.size -= count;
                dest.address += count;
                dest.size -= count;
                copied += count;
            }
            x[0] = failed && !copied ? uint64_t(-14) : copied;
            break;
        }
        case 283: { // membarrier, private expedited mode.
            constexpr uint64_t expedited = 8, registerExpedited = 16;
            if (x[1]) { x[0] = invalid; break; }
            switch (x[0]) {
                case 0: x[0] = expedited | registerExpedited; break;
                case registerExpedited: current->process->expeditedMembarrier = true; x[0] = 0; break;
                case expedited:
                    if (!current->process->expeditedMembarrier) { x[0] = uint64_t(-1); break; }
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    x[0] = 0;
                    break;
                default: x[0] = invalid; break;
            }
            break;
        }
        case 282: // userfaultfd is unavailable in this virtual Linux kernel.
            x[0] = uint64_t(-38);
            break;
        case 32: // flock
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            return lockWholeFile(machine, descriptor->second);
        case 29: // ioctl
            if (descriptor != descriptors().end() && descriptor->second.node == Node::kernelSu) {
                const auto answer = kernelSuIoctl(machine, uint32_t(x[1]), x[2]);
                if (!answer) return waitForEvents();
                x[0] = *answer;
                break;
            }
            if (descriptor != descriptors().end() && descriptor->second.node == Node::dmaHeap) {
                x[0] = uint32_t(x[1]) == 0xc0184800 ? allocateDmaBuffer(machine) : uint64_t(-25);
                break;
            }
            // DMA_BUF_IOCTL_SYNC and the naming of a buffer: nothing to do for memory that is only memory.
            if (descriptor != descriptors().end() && descriptor->second.temporary && descriptor->second.temporary->memoryFile &&
                (uint32_t(x[1]) & 0xff00) == 0x6200) {
                x[0] = 0;
                break;
            }
            if (descriptor != descriptors().end() && descriptor->second.node == Node::sound) {
                const auto answer = soundIoctl(machine, descriptor->second.flags & 0x800, descriptor->second.path.ends_with("D0c"));
                if (!answer) return waitForHostEvents();
                x[0] = *answer;
                break;
            }
            if (descriptor != descriptors().end() && descriptor->second.fuse && uint32_t(x[1]) == 0x8004e500) {
                // FUSE_DEV_IOC_CLONE: this device becomes another way to the session of the one named.
                uint32_t number;
                if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&number), sizeof(number)})) { x[0] = uint64_t(-14); break; }
                const auto session = descriptors().find(number);
                if (session == descriptors().end() || !session->second.fuse) { x[0] = invalid; break; }
                descriptor->second.fuse = session->second.fuse;
                x[0] = 0;
                break;
            }
            if (descriptor != descriptors().end() && descriptor->second.apkRequest &&
                x[1] == GuestApkInbox::resultIoctl) {
                const auto& file = descriptor->second;
                if (file.apkInstaller != processId()) { x[0] = uint64_t(-1); break; }
                if (file.apkRequest->finished()) { x[0] = uint64_t(-114); break; }
                std::vector<uint8_t> statusScratch;
                const auto* status = machine.read(x[2], sizeof(uint32_t), 4, statusScratch);
                if (!status) { x[0] = uint64_t(-14); break; }
                uint32_t waitStatus;
                std::memcpy(&waitStatus, status, sizeof waitStatus);
                const auto outcome = waitStatus ? GuestApkInbox::Result::rejected :
                    (!dataDatabase || syncData()) ? GuestApkInbox::Result::installed : GuestApkInbox::Result::storageError;
                file.apkRequest->finish(outcome);
                x[0] = 0;
                break;
            }
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (static const bool trace = std::getenv("NESTVANE_NETWORK_TRACE"); trace && androidLogObserver &&
                descriptor->second.socket && (x[1] >> 8) == 0x89) {
                char text[96];
                std::snprintf(text, sizeof(text), "native execution: socket ioctl 0x%llx from %.16s", static_cast<unsigned long long>(x[1]),
                              reinterpret_cast<const char*>(current->name.data()));
                androidLogObserver(text);
            }
            if (guestRoutes.ethernet.present && descriptor->second.socket && x[1] == 0x8946) { // SIOCETHTOOL
                // What the Ethernet service asks of a link before it takes it: its driver, whether it
                // has a carrier, and its permanent hardware address.
                std::array<char, 16> name{};
                uint64_t data = 0;
                uint32_t command = 0;
                if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(name.data()), name.size()}) ||
                    !machine.readInto(x[2] + 16, {reinterpret_cast<uint8_t*>(&data), sizeof(data)}) ||
                    !machine.readInto(data, {reinterpret_cast<uint8_t*>(&command), sizeof(command)})) { x[0] = uint64_t(-14); break; }
                if (std::string_view(name.data(), strnlen(name.data(), name.size())) != "eth0") { x[0] = uint64_t(-19); break; }
                if (command == 3) { // ETHTOOL_GDRVINFO
                    std::array<uint8_t, 196> info{};
                    std::memcpy(info.data(), &command, 4);
                    std::memcpy(info.data() + 4, "virtio_net", 10);
                    std::memcpy(info.data() + 36, "1.0.0", 5);
                    std::memcpy(info.data() + 100, "nestvane", 8);
                    x[0] = machine.write(data, info) ? 0 : uint64_t(-14);
                } else if (command == 10) { // ETHTOOL_GLINK
                    const std::array<uint32_t, 2> value{command, guestRoutes.ethernet.up};
                    x[0] = writeGuest(machine, data, value) ? 0 : uint64_t(-14);
                } else if (command == 0x20) { // ETHTOOL_GPERMADDR
                    std::array<uint8_t, 14> value{};
                    const uint32_t size = 6;
                    std::memcpy(value.data(), &command, 4);
                    std::memcpy(value.data() + 4, &size, 4);
                    std::copy(guestRoutes.ethernet.hardware.begin(), guestRoutes.ethernet.hardware.end(), value.begin() + 8);
                    x[0] = machine.write(data, value) ? 0 : uint64_t(-14);
                } else {
                    if (static const bool trace = std::getenv("NESTVANE_NETWORK_TRACE"); trace && androidLogObserver)
                        androidLogObserver("native execution: ethtool command " + std::to_string(command));
                    x[0] = uint64_t(-95);
                }
                break;
            }
            if (guestRoutes.ethernet.present && descriptor->second.socket &&
                (x[1] == 0x8910 || (x[1] >= 0x8913 && x[1] <= 0x8922) || x[1] == 0x8927 || x[1] == 0x8933)) {
                // The interface requests that name the Ethernet interface; the others go on below.
                struct Request {
                    std::array<char, 16> name;
                    std::array<uint8_t, 24> value;
                };
                Request input{};
                if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&input), sizeof(input)})) { x[0] = uint64_t(-14); break; }
                auto& ethernet = guestRoutes.ethernet;
                auto name = std::string_view(input.name.data(), input.name.size());
                name = name.substr(0, name.find('\0'));
                name = name.substr(0, name.find(':'));
                uint32_t wanted = 0;
                std::memcpy(&wanted, input.value.data(), sizeof(wanted));
                if (x[1] == 0x8910 ? wanted == ethernet.index : name == "eth0") {
                    const bool admin = current->process->effectiveCaps & (1ULL << 12);
                    const auto address = [&](std::array<uint8_t, 4> ip) {
                        input.value = {};
                        input.value[0] = 2;  // AF_INET
                        std::copy(ip.begin(), ip.end(), input.value.begin() + 4);
                    };
                    const auto mask = [&] {
                        const uint32_t bits = ethernet.prefix ? ~uint32_t{0} << (32 - ethernet.prefix) : 0;
                        return std::array<uint8_t, 4>{uint8_t(bits >> 24), uint8_t(bits >> 16), uint8_t(bits >> 8), uint8_t(bits)};
                    };
                    int64_t status = 0;
                    switch (x[1]) {
                        case 0x8910: input.name = {'e', 't', 'h', '0'}; break;                    // SIOCGIFNAME
                        case 0x8913: {                                                              // SIOCGIFFLAGS
                            const uint16_t flags = 2 | 0x1000 | (ethernet.up ? 1 | 0x40 : 0);
                            input.value = {};
                            std::memcpy(input.value.data(), &flags, sizeof(flags));
                            break;
                        }
                        case 0x8914:                                                                // SIOCSIFFLAGS
                            if (!admin) status = -1;
                            else guestRoutes.setEthernetUp(input.value[0] & 1);
                            break;
                        case 0x8915:                                                                // SIOCGIFADDR
                            if (!ethernet.address) status = -99;
                            else address(*ethernet.address);
                            break;
                        case 0x8916:                                                                // SIOCSIFADDR
                            if (!admin) { status = -1; break; }
                            if (input.value[4] | input.value[5] | input.value[6] | input.value[7])
                                guestRoutes.setEthernetAddress(std::array<uint8_t, 4>{input.value[4], input.value[5], input.value[6], input.value[7]},
                                                               ethernet.prefix);
                            else guestRoutes.setEthernetAddress({}, ethernet.prefix);
                            break;
                        case 0x8919: {                                                              // SIOCGIFBRDADDR
                            auto broadcast = ethernet.address.value_or(std::array<uint8_t, 4>{});
                            const auto bits = mask();
                            for (size_t each = 0; each < 4; ++each) broadcast[each] |= uint8_t(~bits[each]);
                            address(broadcast);
                            break;
                        }
                        case 0x891b: address(mask()); break;                                       // SIOCGIFNETMASK
                        case 0x891c: {                                                              // SIOCSIFNETMASK
                            if (!admin) { status = -1; break; }
                            const uint32_t bits = uint32_t(input.value[4]) << 24 | uint32_t(input.value[5]) << 16 |
                                                  uint32_t(input.value[6]) << 8 | input.value[7];
                            ethernet.prefix = uint8_t(std::popcount(bits));
                            break;
                        }
                        case 0x8921: input.value = {}; std::memcpy(input.value.data(), &ethernet.mtu, 4); break;   // SIOCGIFMTU
                        case 0x8922:                                                                // SIOCSIFMTU
                            if (!admin) status = -1;
                            else std::memcpy(&ethernet.mtu, input.value.data(), 4);
                            break;
                        case 0x8927:                                                                // SIOCGIFHWADDR
                            input.value = {};
                            input.value[0] = 1;  // ARPHRD_ETHER
                            std::copy(ethernet.hardware.begin(), ethernet.hardware.end(), input.value.begin() + 2);
                            break;
                        case 0x8933: input.value = {}; std::memcpy(input.value.data(), &ethernet.index, 4); break; // SIOCGIFINDEX
                        default: status = -95; break;
                    }
                    x[0] = status ? uint64_t(status) : writeGuest(machine, x[2], input) ? 0 : uint64_t(-14);
                    break;
                }
            }
            if (x[1] == 0x8910 || x[1] == 0x8913 || x[1] == 0x8914 || x[1] == 0x8921 || x[1] == 0x8933) {
                if (!descriptor->second.socket) { x[0] = uint64_t(-25); break; }
                struct Request {
                    std::array<char, 16> name;
                    std::array<uint8_t, 24> value;
                };
                static_assert(sizeof(Request) == 40);
                Request input{};
                if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&input), sizeof(input)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                if (x[1] == 0x8910) { // SIOCGIFNAME
                    uint32_t index = 0;
                    std::memcpy(&index, input.value.data(), sizeof(index));
                    if (index != guestLoopbackIndex) { x[0] = uint64_t(-19); break; }
                    constexpr std::array<char, 16> loopbackName{'l', 'o'};
                    x[0] = writeGuest(machine, x[2], loopbackName) ? 0 : uint64_t(-14);
                    break;
                }
                auto name = std::string_view(input.name.data(), input.name.size());
                name = name.substr(0, name.find('\0'));
                name = name.substr(0, name.find(':'));
                if (name != "lo") { x[0] = uint64_t(-19); break; }
                const auto value = x[2] + offsetof(Request, value);
                constexpr uint16_t up = 1, loopback = 8, running = 0x40;
                if (x[1] == 0x8913) { // SIOCGIFFLAGS
                    const auto flags = uint16_t(loopback | (inetLoopbackUp ? up | running : 0));
                    x[0] = writeGuest(machine, value, flags) ? 0 : uint64_t(-14);
                } else if (x[1] == 0x8914) { // SIOCSIFFLAGS
                    if (!(current->process->effectiveCaps & (1ULL << 12))) { x[0] = uint64_t(-1); break; }
                    inetLoopbackUp = input.value[0] & up;
                    x[0] = 0;
                } else { // SIOCGIFMTU, SIOCGIFINDEX
                    x[0] = writeGuest(machine, value, x[1] == 0x8921 ? guestLoopbackMtu : guestLoopbackIndex)
                        ? 0 : uint64_t(-14);
                }
                break;
            }
            if (x[1] == 0x8004f50c) { // F2FS_IOC_GET_FEATURES, no F2FS mount in the guest.
                x[0] = uint64_t(-25);
                break;
            }
            if (x[1] == 0x40806685 || x[1] == 0xc0046686 || x[1] == 0xc0286687) {
                // FS_IOC_{ENABLE,MEASURE,READ}_VERITY. The guest filesystems do not implement fs-verity.
                x[0] = descriptor->second.pathOnly() ? uint64_t(-9) : uint64_t(-25);
                break;
            }
            if (x[1] == 0x80086601 || x[1] == 0x40086602) { // FS_IOC_GETFLAGS / FS_IOC_SETFLAGS
                const auto& fd = descriptor->second;
                if (fd.pathOnly()) { x[0] = uint64_t(-9); break; }
                if (!fd.temporary && fd.node != Node::directory) { x[0] = uint64_t(-25); break; }
                if (x[1] == 0x40086602) { x[0] = uint64_t(-95); break; }
                x[0] = writeGuest(machine, x[2], uint32_t{0}) ? 0 : uint64_t(-14);
                break;
            }
            if (x[1] == 0x801c581f || x[1] == 0x401c5820) { // FS_IOC_FSGETXATTR / FS_IOC_FSSETXATTR
                // vold gives each app's directory in shared storage a project id for disk quotas. There are
                // no quotas here: every file is in project 0, and naming another one is accepted and forgotten.
                const auto& fd = descriptor->second;
                if (fd.pathOnly()) { x[0] = uint64_t(-9); break; }
                if (!fd.temporary && fd.node != Node::directory) { x[0] = uint64_t(-25); break; }
                const std::array<uint8_t, 28> attributes{};
                x[0] = x[1] == 0x401c5820 || machine.write(x[2], attributes) ? 0 : uint64_t(-14);
                break;
            }
            if (x[1] == 0x541b && (descriptor->second.temporary ||
                                   descriptor->second.node == Node::regular)) { // FIONREAD
                const auto& fd = descriptor->second;
                const auto size = fd.temporary ? fd.temporary->storage->size : fd.contentSize();
                x[0] = fd.pathOnly() ? uint64_t(-9) :
                    writeGuest(machine, x[2], uint32_t(size - *fd.offset)) ? 0 : uint64_t(-14);
                break;
            }
            // How much a pipe holds: Java's Process streams ask (available()) before they read a child's
            // output. Without it the asking thread was suspended, and Runtime.exec never returned.
            if (x[1] == 0x541b && descriptor->second.pipe && descriptor->second.node == Node::pipeRead) {
                x[0] = writeGuest(machine, x[2], uint32_t(std::min<size_t>(descriptor->second.pipe->data.size(), INT32_MAX)))
                     ? 0 : uint64_t(-14);
                break;
            }
            if (descriptor->second.node == Node::touchDevice) x[0] = touchIoctl(machine, descriptor->second);
            else if (descriptor->second.pty) x[0] = ptyIoctl(machine, descriptor->second);
            else if (descriptor->second.node == Node::ashmem) x[0] = ashmemIoctl(machine, descriptor->second);
            else if (descriptor->second.node == Node::graphicsAddressSpace) x[0] = graphicsAddressIoctl(machine, descriptor->second);
            else if (x[1] == 0x5401) x[0] = uint64_t(-25); // TCGETS on a non-terminal.
            else if (descriptor->second.binder) {
                const auto result = binderIoctl(machine, descriptor->second);
                if (result == uint64_t(-11) && x[1] == 0xc0306201 && !(descriptor->second.effectiveFlags() & 0x800)) {
                    const auto& connection = descriptor->second.binder;
                    current->waitingBinder = connection;
                    current->waitingBinderFd = uint32_t(x[0]);
                    if (binderAvailableForProcessWork(*connection, currentTid))
                        current->idleBinder = connection;
                    return waitForEvents();
                }
                x[0] = result;
            }
            else if (descriptor->second.node == Node::loopControl || descriptor->second.node == Node::loopDevice)
                x[0] = loopIoctl(machine, descriptor->second);
            else return false;
            break;
        case 30: case 31: { // ioprio_set/get for guest tasks and in-memory I/O.
            if (x[0] != 1) return false; // IOPRIO_WHO_PROCESS
            const auto tid = x[1] ? x[1] : currentTid;
            const auto task = threads.find(tid);
            if (task == threads.end() || task->second.wait == Wait::exited) {
                x[0] = uint64_t(-3); break;
            }
            if (number == 31) { x[0] = task->second.ioPriority; break; }
            const auto value = x[2], ioClass = value >> 13, level = value & 0x1fff;
            if (ioClass > 3 || (!ioClass && level) || (ioClass && ioClass < 3 && level > 7)) {
                x[0] = invalid; break;
            }
            const auto& owner = task->second.process->userIds;
            const bool admin = current->process->effectiveCaps & (1ULL << 21);
            if ((ioClass == 1 && !admin) ||
                (!admin && userId() != owner[0] && userId() != owner[1])) {
                x[0] = uint64_t(-1); break;
            }
            task->second.ioPriority = value;
            x[0] = 0;
            break;
        }
        case 25: { // fcntl file-descriptor flags and open-file status.
            if (x[1] == 7 && current->recordLockBlocker) {
                const auto original = current->waitingRecordDescription.lock();
                current->waitingRecordFile.reset();
                current->waitingRecordDescription.reset();
                current->recordLockBlocker = 0;
                if (descriptor == descriptors().end() || descriptor->second.offset != original) {
                    x[0] = uint64_t(-9);
                    break;
                }
            }
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            auto& fd = descriptor->second;
            constexpr uint32_t cloexec = 0x80000;
            switch (x[1]) {
                case 0: case 1030: { // F_DUPFD, F_DUPFD_CLOEXEC
                    if (x[2] >= descriptorLimit()) { x[0] = invalid; break; }
                    const auto number = freeDescriptor(uint32_t(x[2]));
                    if (number == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                    auto copy = fd;
                    copy.flags = (copy.flags & ~cloexec) | (x[1] == 1030 ? cloexec : 0);
                    descriptors().emplace(number, std::move(copy));
                    x[0] = number;
                    break;
                }
                case 1031: { // F_SETPIPE_SZ
                    if (!fd.pipe) { x[0] = uint64_t(-9); break; }
                    constexpr uint64_t maxPipeCapacity = 1024 * 1024;
                    if (x[2] > INT32_MAX) { x[0] = invalid; break; }
                    if (x[2] > maxPipeCapacity) { x[0] = uint64_t(-1); break; }
                    const auto capacity = std::bit_ceil(std::max(x[2], pageSize));
                    if (capacity < fd.pipe->data.size()) { x[0] = uint64_t(-16); break; }
                    fd.pipe->capacity = capacity;
                    x[0] = capacity;
                    wakePollers();
                    break;
                }
                case 1032: // F_GETPIPE_SZ
                    x[0] = fd.pipe ? fd.pipe->capacity : uint64_t(-9);
                    break;
                case 1033: { // F_ADD_SEALS
                    if (!fd.temporary || !fd.temporary->memoryFile ||
                        (x[2] & ~uint64_t(sealSeal | sealShrink | sealGrow | sealWrite | sealFutureWrite))) {
                        x[0] = invalid;
                        break;
                    }
                    auto& file = *fd.temporary;
                    if (!(fd.flags & 3) || (file.seals & sealSeal)) { x[0] = uint64_t(-1); break; }
                    if ((x[2] & sealWrite) && !(file.seals & sealWrite)) {
                        const auto anyWritable = [&](const std::vector<Segment>& segments) {
                            return std::any_of(segments.begin(), segments.end(), [&](const Segment& area) {
                                return area.shared == file.storage && !area.sharedReadOnly;
                            });
                        };
                        if (anyWritable(machine.segments) || std::any_of(threads.begin(), threads.end(),
                            [&](const auto& item) { return anyWritable(item.second.process->segments); })) {
                            x[0] = uint64_t(-16);
                            break;
                        }
                    }
                    file.seals |= uint32_t(x[2]);
                    x[0] = 0;
                    break;
                }
                case 1034: // F_GET_SEALS
                    x[0] = fd.temporary && fd.temporary->memoryFile ? fd.temporary->seals : invalid;
                    break;
                case 1: x[0] = bool(fd.flags & cloexec); break; // F_GETFD
                case 2: // F_SETFD
                    fd.flags = (fd.flags & ~cloexec) | (x[2] & 1 ? cloexec : 0);
                    x[0] = 0;
                    break;
                case 3: // F_GETFL
                    x[0] = fd.effectiveFlags() & ~(cloexec | 0x8000U | 0x40U | 0x80U | 0x200U);
                    break;
                case 4: // F_SETFL
                    if (fd.pathOnly()) { x[0] = uint64_t(-9); break; }
                    *fd.status = x[2] & Descriptor::mutableStatus;
                    x[0] = 0;
                    break;
                case 5: case 6: case 7: // F_GETLK, F_SETLK, F_SETLKW
                case 36: case 37: case 38: // F_OFD_GETLK, F_OFD_SETLK, F_OFD_SETLKW
                    x[0] = recordLock(machine, fd);
                    break;
                default: return false;
            }
            break;
        }
        case 172: // getpid
            x[0] = processId();
            break;
        case 173: // getppid
            x[0] = current->process->parent;
            break;
        case 221: return execProcess(machine); // execve
        case 95: case 260: return waitChild(machine); // waitid, wait4
        case 142: { // reboot: only KernelSU's request for its driver, whose descriptor goes to *arg4
            if (!rootOn || uint32_t(x[0]) != 0xDEADBEEF || uint32_t(x[1]) != 0xCAFEBABE) return false;
            const auto fd = installKernelSu();
            const int32_t number = fd >= descriptorCapacity ? -1 : int32_t(fd);
            if (number >= 0 && !machine.write(x[3], {reinterpret_cast<const uint8_t*>(&number), sizeof number}))
                descriptors().erase(uint32_t(number));
            x[0] = uint64_t(-22);  // the reboot itself does not happen
            break;
        }
        case 434: { // pidfd_open for guest process leaders.
            const auto target = int32_t(x[0]);
            if (target <= 0 || x[1]) { x[0] = invalid; break; }
            if (!processAlive(target) && !childExits.contains(target)) { x[0] = uint64_t(-3); break; }
            const auto fd = freeDescriptor();
            if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            Descriptor handle{"anon_inode:[pidfd]", {}, 0x80000, Node::processHandle};
            handle.targetProcess = target;
            descriptors().emplace(fd, std::move(handle));
            x[0] = fd;
            break;
        }
        case 96: // set_tid_address
            current->clearTid = x[0];
            x[0] = currentTid;
            break;
        case 179: { // sysinfo, ARM64 Linux layout.
            struct SystemInfo {
                int64_t uptime;
                uint64_t loads[3], totalRam, freeRam, sharedRam, bufferRam, totalSwap, freeSwap;
                uint16_t processes;
                uint16_t padding;
                uint64_t totalHigh, freeHigh;
                uint32_t memoryUnit;
            } info{};
            static_assert(sizeof(SystemInfo) == 112);
            info.uptime = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            info.totalRam = memoryLimit;
            info.freeRam = freeMemory(machine);
            info.processes = threads.size() - std::count_if(threads.begin(), threads.end(), [](const auto& task) {
                return task.second.wait == Wait::exited;
            });
            info.memoryUnit = 1;
            x[0] = writeGuest(machine, x[0], info) ? 0 : uint64_t(-14);
            break;
        }
        case 178: // gettid
            x[0] = currentTid;
            break;
        case 97: { // unshare(CLONE_NEWNS) implicitly unshares pathname state.
            if (x[0] != 0x20000) return false;
            if (userId() != 0) { x[0] = uint64_t(-1); break; }
            if (nextNamespace > 64) { x[0] = uint64_t(-28); break; }
            auto next = std::make_shared<MountNamespace>(*current->mountNamespace);
            next->id = nextNamespace++;
            current->mountNamespace = std::move(next);
            x[0] = 0;
            break;
        }
        case 268: // setns to a retained guest mount namespace handle
            if (x[1] && x[1] != 0x20000) return false;
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (!descriptor->second.mountNamespace) { x[0] = invalid; break; }
            if (userId() != 0) { x[0] = uint64_t(-1); break; }
            current->mountNamespace = descriptor->second.mountNamespace;
            x[0] = 0;
            break;
        case 124: // sched_yield
            sliceEnd = 0;
            x[0] = 0;
            break;
        case 220: // clone
            // debuggerd asks for a thread with its own file table. This profile
            // shares files across threads, so let Android take its exit fallback.
            if (x[0] == 0x01210900) { x[0] = uint64_t(-38); break; }
            return cloneThread(machine);
        case 93: return exitThread(machine);
        case 94:
            if (processId() == pid) return false;
            removeSiblingThreads();
            return exitThread(machine);
        case 174: case 175: case 176: case 177:
            x[0] = (number < 176 ? current->process->userIds : current->process->groupIds)[number & 1];
            break;
        case 143: case 144: case 145: case 146: case 147: case 149: {
            auto& process = *current->process;
            const bool users = number == 145 || number == 146 || number == 147;
            auto& ids = users ? process.userIds : process.groupIds;
            const bool pair = number == 143 || number == 145; // setregid, setreuid
            const bool triple = number == 147 || number == 149;
            auto updated = ids;
            bool allowed = true;
            if (pair) {
                const auto real = uint32_t(x[0]), effective = uint32_t(x[1]);
                const auto capability = uint64_t{1} << (users ? 7 : 6); // CAP_SETUID, CAP_SETGID
                const bool canSet = process.effectiveCaps & capability;
                allowed = canSet ||
                    ((real == UINT32_MAX || real == ids[0] || real == ids[1]) &&
                     (effective == UINT32_MAX || std::find(ids.begin(), ids.end(), effective) != ids.end()));
                if (real != UINT32_MAX) updated[0] = real;
                if (effective != UINT32_MAX) updated[1] = effective;
                if (real != UINT32_MAX || (effective != UINT32_MAX && effective != ids[0]))
                    updated[2] = updated[1];
            } else {
                // CAP_SETUID or CAP_SETGID, not only root, may set any id. WebView's zygote runs as its own
                // user with both, and gives each renderer it starts an id of its range.
                const bool privileged = userId() == 0 || (process.effectiveCaps & (uint64_t{1} << (users ? 7 : 6)));
                for (unsigned i = 0; i < 3; ++i) {
                    const auto value = uint32_t(x[triple ? i : 0]);
                    if (triple && value == UINT32_MAX) continue;
                    if (value == UINT32_MAX || (!privileged && std::find(ids.begin(), ids.end(), value) == ids.end())) {
                        allowed = false;
                        break;
                    }
                    if (triple || privileged || i == 1) updated[i] = value;
                }
            }
            if (allowed) {
                if (users && !(process.secureBits & secureNoSetuidFixup)) {
                    if (std::count(ids.begin(), ids.end(), 0) && !std::count(updated.begin(), updated.end(), 0)) {
                        process.effectiveCaps = process.ambientCaps = 0;
                        if (!(process.secureBits & secureKeepCaps)) process.permittedCaps = 0;
                    } else if (ids[1] == 0 && updated[1] != 0) {
                        process.effectiveCaps = 0;
                    } else if (ids[1] != 0 && updated[1] == 0) {
                        process.effectiveCaps = process.permittedCaps;
                    }
                }
                if (ids[1] != updated[1]) process.dumpable = false;
                const bool userChanged = users && ids[0] != updated[0];
                ids = updated;
                // KernelSU: the Manager's process gets its driver as it becomes the Manager's user.
                if (userChanged && rootOn) kernelSuUserChanged(updated[0]);
            }
            x[0] = allowed ? 0 : uint64_t(-1);
            break;
        }
        case 158: case 159: { // getgroups, setgroups
            auto& groups = current->process->groups;
            if (number == 159) {
                if (userId() != 0 && !(current->process->effectiveCaps & (uint64_t{1} << 6))) { x[0] = uint64_t(-1); break; }
                if (x[0] > 64) { x[0] = invalid; break; }
                std::vector<uint32_t> updated(x[0]);
                const auto size = updated.size() * sizeof(uint32_t);
                if (size) {
                    std::vector<uint8_t> sourceScratch;
                    const auto* source = machine.read(x[1], size, 4, sourceScratch);
                    if (!source) { x[0] = uint64_t(-14); break; }
                    std::memcpy(updated.data(), source, size);
                }
                groups = std::move(updated);
                x[0] = 0;
            } else {
                if (!x[0]) { x[0] = groups.size(); break; }
                if (x[0] < groups.size()) { x[0] = invalid; break; }
                const auto bytes = std::as_bytes(std::span(groups));
                x[0] = bytes.empty() || machine.write(x[1], {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()})
                    ? groups.size() : uint64_t(-14);
            }
            break;
        }
        case 125: case 126: { // sched_get_priority_max/min, Linux policy numbers.
            switch (int32_t(x[0])) {
                case 1: case 2: x[0] = number == 125 ? 99 : 1; break; // FIFO, RR
                case 0: case 3: case 5: case 6: x[0] = 0; break; // OTHER, BATCH, IDLE, DEADLINE
                default: x[0] = invalid; break;
            }
            break;
        }
        case 118: case 119: { // Guest scheduler remains SCHED_OTHER with priority zero.
            const auto task = threads.find(x[0] ? x[0] : currentTid);
            if (task == threads.end() || task->second.wait == Wait::exited) { x[0] = uint64_t(-3); break; }
            const auto parameter = x[number == 119 ? 2 : 1];
            std::vector<uint8_t> bytesScratch;
            const auto* bytes = machine.read(parameter, sizeof(int32_t), 4, bytesScratch);
            if (!bytes) { x[0] = uint64_t(-14); break; }
            int32_t priority = 0;
            std::memcpy(&priority, bytes, sizeof(priority));
            const auto policy = number == 119 ? uint32_t(x[1]) : 0;
            if (policy == 1 || policy == 2) {
                x[0] = priority < 1 || priority > 99 ? invalid : uint64_t(-95);
            } else if (policy == 0) {
                x[0] = priority == 0 ? 0 : invalid;
            } else {
                x[0] = invalid;
            }
            break;
        }
        case 120: case 121: { // sched_getscheduler / sched_getparam. Guest tasks use SCHED_OTHER.
            const auto task = threads.find(x[0] ? x[0] : currentTid);
            if (task == threads.end() || task->second.wait == Wait::exited) { x[0] = uint64_t(-3); break; }
            x[0] = number == 120 || writeGuest(machine, x[1], int32_t{0}) ? 0 : uint64_t(-14);
            break;
        }
        case 113: { // clock_gettime
            std::chrono::nanoseconds now;
            if (x[0] == 0 || x[0] == 5 || x[0] == 8) {
                now = realtimeAt(std::chrono::steady_clock::now());
            } else if (x[0] == 1 || x[0] == 4 || x[0] == 6 || x[0] == 7 || x[0] == 9) {
                now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch());
            } else if (x[0] == 2 || x[0] == 3) {
                accountGuestCpu();
                now = x[0] == 2 ? current->process->cpuTime : current->cpuTime;
            } else {
                x[0] = invalid;
                break;
            }
            const std::array<int64_t, 2> time{now.count() / 1'000'000'000, now.count() % 1'000'000'000};
            x[0] = writeGuest(machine, x[1], time) ? 0 : uint64_t(-14);
            break;
        }
        case 165: { // getrusage
            const auto who = int32_t(x[0]);
            if (who != -1 && who != 0 && who != 1) { x[0] = invalid; break; }
            accountGuestCpu();
            const auto& process = *current->process;
            const auto usage = who == -1 ? process.reapedCpuTime : who == 0 ? process.cpuTime : current->cpuTime;
            x[0] = writeGuestRusage(machine, x[1], usage) ? 0 : uint64_t(-14);
            break;
        }
        case 163: case 164: case 261: { // getrlimit, setrlimit, prlimit64
            const bool both = number == 261;
            if (both && x[0] && x[0] != processId()) { x[0] = uint64_t(-3); break; }
            const auto resource = uint32_t(x[both ? 1 : 0]);
            if (resource >= 16) { x[0] = invalid; break; }
            const auto input = both ? x[2] : number == 164 ? x[1] : 0;
            const auto output = both ? x[3] : number == 163 ? x[1] : 0;
            if ((number == 164 && !input) || (number == 163 && !output)) { x[0] = uint64_t(-14); break; }
            auto& limits = current->process->limits;
            const auto found = limits.find(resource);
            const auto defaultLimit = resource == 7
                ? std::array<uint64_t, 2>{defaultDescriptorLimit, descriptorCapacity}
                : resource == 3 ? std::array<uint64_t, 2>{guestStackSize, maxGuestStackSize}
                : resource == niceLimitResource ? std::array<uint64_t, 2>{0, 0}
                : std::array<uint64_t, 2>{UINT64_MAX, UINT64_MAX};
            const auto old = found == limits.end() ? defaultLimit : found->second;
            auto updated = old;
            if (input) {
                std::vector<uint8_t> dataScratch;
                const auto* data = machine.read(input, sizeof(updated), 4, dataScratch);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(updated.data(), data, sizeof(updated));
                if (updated[0] > updated[1]) { x[0] = invalid; break; }
                if (resource == 7 && updated[1] > descriptorCapacity) { x[0] = uint64_t(-1); break; }
                if (resource == 3 && updated[1] > maxGuestStackSize) { x[0] = uint64_t(-1); break; }
                if (updated[1] > old[1] && userId() != 0) { x[0] = uint64_t(-1); break; }
            }
            if (output && !writeGuest(machine, output, old)) { x[0] = uint64_t(-14); break; }
            if (input) limits.insert_or_assign(resource, updated);
            x[0] = 0;
            break;
        }
        case 154: case 155: case 156: case 157: { // setpgid, getpgid, getsid, setsid
            if (number == 157) {
                auto& process = *current->process;
                if (process.group == process.id) { x[0] = uint64_t(-1); break; }
                process.group = process.session = process.id;
                x[0] = process.id;
                break;
            }
            const auto id = x[0] ? x[0] : processId();
            const auto target = std::find_if(threads.begin(), threads.end(), [=](const auto& task) {
                return task.second.process->id == id && task.second.wait != Wait::exited;
            });
            if (target == threads.end()) { x[0] = uint64_t(-3); break; }
            auto& process = *target->second.process;
            if (number == 155 || number == 156) {
                x[0] = number == 155 ? process.group : process.session;
                break;
            }
            if (process.id != processId() && process.parent != processId()) { x[0] = uint64_t(-3); break; }
            if (process.session != current->process->session || process.session == process.id) {
                x[0] = uint64_t(-1); break;
            }
            const auto group = x[1] ? x[1] : id;
            if (group != id && std::none_of(threads.begin(), threads.end(), [&](const auto& task) {
                    return task.second.process->group == group && task.second.process->session == process.session;
                })) { x[0] = uint64_t(-1); break; }
            process.group = group;
            x[0] = 0;
            break;
        }
        case 166: // umask
            x[0] = std::exchange(current->process->umask, uint32_t(x[0]) & 0777);
            break;
        case 160: { // uname for the virtual Linux kernel.
            const std::array<std::string_view, 6> fields{"Linux", "localhost", "6.12.0-nestvane",
                                                        "#1 SMP", "aarch64", "(none)"};
            std::array<char, 6 * 65> info{};
            for (size_t i = 0; i < fields.size(); ++i)
                std::copy(fields[i].begin(), fields[i].end(), info.begin() + i * 65);
            x[0] = writeGuest(machine, x[0], info) ? 0 : uint64_t(-14);
            break;
        }
        case 123: { // sched_getaffinity, one virtual CPU.
            if (x[0] && x[0] != processId()) { x[0] = uint64_t(-3); break; }
            const uint64_t mask = 1;
            if (x[1] < sizeof(mask) || x[1] % sizeof(mask)) { x[0] = invalid; break; }
            x[0] = writeGuest(machine, x[2], mask) ? sizeof(mask) : uint64_t(-14);
            break;
        }
        case 73: return poll(machine); // ppoll
        case 98: return futex(machine);
        case 102: case 103: { // getitimer, setitimer
            // The real-time timer, which alarm and the timeouts of command-line tools are made of.
            // The two that count a process's own running time are taken and never go off.
            using namespace std::chrono;
            const bool set = number == 103;
            if (x[0] > 2) { x[0] = invalid; break; }
            std::array<int64_t, 4> wanted{};  // the interval, then the time until the first, each as seconds and microseconds
            if (set && x[1] && !machine.readInto(x[1], {reinterpret_cast<uint8_t*>(wanted.data()), sizeof(wanted)})) {
                x[0] = uint64_t(-14);
                break;
            }
            if (wanted[0] < 0 || wanted[2] < 0 || wanted[1] < 0 || wanted[1] >= 1'000'000 || wanted[3] < 0 || wanted[3] >= 1'000'000) {
                x[0] = invalid;
                break;
            }
            auto& process = *current->process;
            const uint64_t report = set ? x[2] : x[1];
            if (report) {
                std::array<int64_t, 4> before{};
                if (x[0] == 0 && process.alarmAt) {
                    const auto left = std::max(microseconds{1}, duration_cast<microseconds>(*process.alarmAt - steady_clock::now()));
                    before = {process.alarmEvery.count() / 1'000'000, process.alarmEvery.count() % 1'000'000,
                              left.count() / 1'000'000, left.count() % 1'000'000};
                }
                if (!machine.write(report, {reinterpret_cast<const uint8_t*>(before.data()), sizeof(before)})) {
                    x[0] = uint64_t(-14);
                    break;
                }
            }
            if (set && x[0] == 0) {
                const auto first = seconds(wanted[2]) + microseconds(wanted[3]);
                process.alarmEvery = seconds(wanted[0]) + microseconds(wanted[1]);
                if (x[1] && first.count() > 0) process.alarmAt = steady_clock::now() + first;
                else process.alarmAt.reset();
                sliceEnd = 0;  // the scheduler has a new time to wake for
            }
            x[0] = 0;
            break;
        }
        case 101: case 115: { // nanosleep / clock_nanosleep
            using namespace std::chrono;
            const auto now = steady_clock::now();
            const bool explicitClock = number == 115;
            constexpr uint32_t realtime = 0, monotonic = 1, boottime = 7, absoluteFlag = 1;
            if (!current->deadline) {
                const auto clock = explicitClock ? uint32_t(x[0]) : monotonic;
                const auto flags = explicitClock ? uint32_t(x[1]) : 0;
                if (flags & ~absoluteFlag || clock == 3 || clock > 11) { x[0] = invalid; break; }
                if (clock != realtime && clock != monotonic && clock != boottime) {
                    x[0] = uint64_t(-95);
                    break;
                }
                std::array<int64_t, 2> timeout{};
                if (!machine.readInto(x[explicitClock ? 2 : 0],
                        {reinterpret_cast<uint8_t*>(timeout.data()), sizeof timeout})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1'000'000'000) { x[0] = invalid; break; }
                auto delay = timeout[0] > (INT64_MAX - timeout[1]) / 1'000'000'000
                    ? nanoseconds::max() : seconds(timeout[0]) + nanoseconds(timeout[1]);
                if (flags & absoluteFlag) {
                    const auto epoch = clock == realtime ? realtimeAt(now) : duration_cast<nanoseconds>(now.time_since_epoch());
                    if (clock == realtime) current->realtimeWaitDeadline = delay;
                    delay = std::max(nanoseconds{}, delay - epoch);
                }
                current->deadline = now + std::min(delay, duration_cast<nanoseconds>(steady_clock::time_point::max() - now));
            }
            if (*current->deadline > now) {
                current->wait = Wait::sleep;
                sliceEnd = 0;
                return true;
            }
            current->deadline.reset();
            current->realtimeWaitDeadline.reset();
            x[0] = 0;
            break;
        }
        case 112: case 170: { // clock_settime / settimeofday, confined to the guest.
            const bool timeOfDay = number == 170;
            if (!timeOfDay && x[0] != 0) { x[0] = invalid; break; }
            if (!(current->process->effectiveCaps & (1ULL << 25))) { x[0] = uint64_t(-1); break; }
            const auto address = x[timeOfDay ? 0 : 1];
            std::array<int64_t, 2> time{};
            std::array<int32_t, 2> timezone{};
            if (address || !timeOfDay) {
                std::vector<uint8_t> inputScratch;
                const auto* input = machine.read(address, sizeof time, 4, inputScratch);
                if (!input) { x[0] = uint64_t(-14); break; }
                std::memcpy(time.data(), input, sizeof time);
                const auto ticksPerSecond = timeOfDay ? 1'000'000 : 1'000'000'000;
                const auto fraction = time[1] >= 0 && time[1] < ticksPerSecond
                    ? time[1] * (1'000'000'000 / ticksPerSecond) : -1;
                if (fraction < 0 || time[0] < 0 || time[0] > (INT64_MAX - fraction) / 1'000'000'000) {
                    x[0] = invalid;
                    break;
                }
                time[1] = fraction;
            }
            if (timeOfDay && x[1]) {
                std::vector<uint8_t> inputScratch;
                const auto* input = machine.read(x[1], sizeof timezone, 4, inputScratch);
                if (!input) { x[0] = uint64_t(-14); break; }
                std::memcpy(timezone.data(), input, sizeof timezone);
                if (timezone[0] < -900 || timezone[0] > 900) { x[0] = invalid; break; }
                guestTimezone = timezone;
            }
            if (address) setRealtime(std::chrono::seconds(time[0]) + std::chrono::nanoseconds(time[1]));
            x[0] = 0;
            break;
        }
        case 169: { // gettimeofday
            const auto now = realtimeAt(std::chrono::steady_clock::now());
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            const std::array<int64_t, 2> time{micros / 1'000'000, micros % 1'000'000};
            x[0] = (!x[0] || writeGuest(machine, x[0], time)) &&
                   (!x[1] || writeGuest(machine, x[1], guestTimezone)) ? 0 : uint64_t(-14);
            break;
        }
        case 279: { // memfd_create, backed by guest-only memory.
            constexpr uint32_t cloexec = 1, allowSealing = 2;
            if (x[1] & ~uint64_t(cloexec | allowSealing)) { x[0] = invalid; break; }
            const auto name = readPath(machine, x[0]);
            if (!name) { x[0] = uint64_t(-14); break; }
            if (name->size() > 249) { x[0] = invalid; break; }
            const auto fd = freeDescriptor();
            if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            std::erase_if(memoryFiles, [](const auto& file) { return file.expired(); });
            if (memoryFiles.size() >= resourceObjectCapacity) { x[0] = uint64_t(-23); break; }
            auto file = std::make_shared<WritableFile>();
            file->inode = nextInode++;
            file->mode = 0777;
            file->uid = userId();
            file->gid = groupId();
            file->memoryFile = true;
            file->seals = x[1] & allowSealing ? 0 : sealSeal;
            memoryFiles.push_back(file->storage);
            Descriptor entry{"/memfd:" + *name + " (deleted)", {}, x[1] & cloexec ? 0x80002U : 2U, Node::temporary};
            entry.temporary = std::move(file);
            descriptors().emplace(fd, std::move(entry));
            x[0] = fd;
            break;
        }
        case 278: { // getrandom
            constexpr uint64_t nonblock = 1, random = 2, insecure = 4;
            if ((x[2] & ~(nonblock | random | insecure)) || (x[2] & (random | insecure)) == (random | insecure)) {
                x[0] = invalid;
                break;
            }
            x[0] = writeEntropy(machine, x[0], std::min(x[1], pageSize));
            break;
        }
        case 64: case 68: { // write, pwrite64
            const auto position = number == 68 ? std::optional(x[3]) : std::nullopt;
            if (position && *position > INT64_MAX) { x[0] = invalid; break; }
            const auto result = descriptor == descriptors().end() ? uint64_t(-9) :
                writeDescriptor(machine, descriptor->second, x[1], x[2], position);
            if (result == uint64_t(-11) && descriptor->second.graphics &&
                !(descriptor->second.effectiveFlags() & 0x800)) return waitForHostEvents();
            if (descriptor != descriptors().end() && descriptor->second.socket) {
                const auto& fd = descriptor->second;
                if (result == uint64_t(-11) && !(fd.effectiveFlags() & 0x800) &&
                    waitForSocketTimeout(fd.socket->sendTimeout)) return true;
                current->deadline.reset();
            } else if (result == uint64_t(-11) && (descriptor->second.pipe || descriptor->second.pty) &&
                       !(descriptor->second.effectiveFlags() & 0x800)) {
                return waitForEvents();
            }
            x[0] = result;
            break;
        }
        case 66: { // writev
            if (descriptor == descriptors().end() || descriptor->second.pathOnly() ||
                !(descriptor->second.flags & 3)) { x[0] = uint64_t(-9); break; }
            if (x[2] > 64) { x[0] = invalid; break; }
            if (descriptor->second.socket && descriptor->second.socket->logWriter) {
                uint64_t written = 0;
                std::string message;
                bool readable = true;
                for (uint64_t i = 0; i < x[2]; ++i) {
                    std::array<uint64_t, 2> vector{};
                    if (x[1] > UINT64_MAX - i * sizeof(vector)) { readable = false; break; }
                    std::vector<uint8_t> regionScratch;
                    const auto* region = machine.read(x[1] + i * sizeof(vector), sizeof(vector), 4, regionScratch);
                    if (!region) { readable = false; break; }
                    std::memcpy(vector.data(), region, sizeof(vector));
                    if (!vector[1]) continue;
                    if (vector[1] > 16384 || written > 16384 - vector[1]) { readable = false; break; }
                    std::vector<uint8_t> bytesScratch;
                    const auto* bytes = machine.read(vector[0], vector[1], 4, bytesScratch);
                    if (!bytes) { readable = false; break; }
                    written += vector[1];
                    // liblog writes a packed header. Bionic's async-safe writer splits it into three vectors.
                    const auto tag = x[2] == 4 ? 2 : x[2] == 6 ? 4 : x[2];
                    if (i < tag) continue;
                    std::string_view part(reinterpret_cast<const char*>(bytes), vector[1]);
                    part = part.substr(0, part.find('\0'));
                    if (i == tag + 1) message += ": ";
                    for (const char character : part)
                        if (character >= 32 || character == '\n' || character == '\t') message.push_back(character);
                }
                if (readable && !message.empty()) {
                    if (watchdogReport.empty() && message.starts_with("Watchdog: *** WATCHDOG KILLING SYSTEM PROCESS")) {
                        watchdogReport = workerReport(machine, processId());
                        if (watchdogObserver) watchdogObserver(watchdogReport);
                    }
                    if (androidLogObserver) androidLogObserver(message);
                    keepDiagnosticLine(message);
                    message.push_back('\n');
                    captureLog(androidOutput, recentAndroidOutput, 128 * 1024, message);
                }
                x[0] = readable ? written : uint64_t(-14);
                break;
            }
            if (const auto& socket = descriptor->second.socket; socket && socket->netlink && socket->protocol != 15) {
                std::array<SocketVector, 64> vectors{};
                const auto parts = std::span(vectors).first(x[2]);
                if (parts.empty()) { x[0] = 0; break; }
                if (!machine.readInto(x[1], {reinterpret_cast<uint8_t*>(parts.data()), parts.size_bytes()})) {
                    x[0] = uint64_t(-14);
                    break;
                }
                x[0] = sendKernelNetlink(machine, socket, parts);
                break;
            }
            uint64_t written = 0;
            uint64_t result = 0;
            for (uint64_t i = 0; i < x[2]; ++i) {
                std::array<uint64_t, 2> iov{};
                const auto address = x[1] + i * sizeof(iov);
                std::vector<uint8_t> entryScratch;
                const auto* entry = address >= x[1] ? machine.read(address, sizeof(iov), 4, entryScratch) : nullptr;
                if (!entry) { result = uint64_t(-14); break; }
                std::memcpy(iov.data(), entry, sizeof(iov));
                result = writeDescriptor(machine, descriptor->second, iov[0], iov[1]);
                if (result <= iov[1]) written += result;
                if (result != iov[1]) break;
            }
            const auto& fd = descriptor->second;
            if (!written && result == uint64_t(-11) && fd.graphics &&
                !(fd.effectiveFlags() & 0x800)) return waitForHostEvents();
            if (fd.socket) {
                if (!written && result == uint64_t(-11) && !(fd.effectiveFlags() & 0x800) &&
                    waitForSocketTimeout(fd.socket->sendTimeout)) return true;
                current->deadline.reset();
            }
            x[0] = written ? written : result;
            break;
        }
        case 222: { // mmap
            const std::array<uint64_t, 6> asked{x[0], x[1], x[2], x[3], x[4], x[5]};
            const auto result = map(machine);
            // A large reservation that fails ends a program that cannot do without it (Chromium's
            // allocator, V8's code range): say what was asked.
            if (androidLogObserver && result >= uint64_t(-4095) && asked[1] >= 32 * 1024 * 1024) {
                std::ostringstream event;
                event << "native execution: mmap of 0x" << std::hex << asked[1] << " bytes failed with errno=" << std::dec
                      << -int64_t(result) << std::hex << " hint=0x" << asked[0] << " prot=0x" << asked[2]
                      << " flags=0x" << asked[3] << " pid " << std::dec << processId();
                androidLogObserver(event.str());
            }
            if (androidLogObserver && result >= uint64_t(-4095) && !(x[3] & 0x20)) {
                const auto file = descriptors().find(uint32_t(x[4]));
                if (file != descriptors().end() && file->second.path.ends_with(".apk")) {
                    std::ostringstream event;
                    const auto& path = file->second.visiblePath.empty() ? file->second.path : file->second.visiblePath;
                    event << "mmap APK: " << path << " failed with errno=" << -int64_t(result)
                          << std::hex << " length=0x" << x[1] << " prot=0x" << x[2]
                          << " flags=0x" << x[3] << " offset=0x" << x[5];
                    const auto message = event.str();
                    androidLogObserver(message);
                }
            }
            x[0] = result;
            break;
        }
        case 215: { // munmap
            if (x[0] % pageSize || x[0] < lowMappingFloor || !x[1] || x[1] > maxReservation ||
                x[0] > mappingLimit - pageAlignedLength(x[1])) { x[0] = invalid; break; }
            const auto end = x[0] + pageAlignedLength(x[1]);
            if (x[0] < mappingFloor && std::any_of(machine.segments.begin(), machine.segments.end(),
                [&](const Segment& segment) {
                    return (segment.initialStack || (!segment.anonymous && !segment.fileMapping)) &&
                           x[0] < segment.address + segment.mappedSize() &&
                           segment.address < end;
                })) { x[0] = invalid; break; }
            changeRange(machine, x[0], end, {});
            x[0] = 0;
            break;
        }
        case 216: { // mremap of an anonymous range, including fragments left by mprotect.
            // MREMAP_DONTUNMAP leaves the old range mapped and empty. ART's compacting collector moves
            // its heap aside this way.
            constexpr uint64_t mayMove = 1, fixed = 2, dontUnmap = 4;
            const auto flags = x[3];
            if ((flags & ~(mayMove | fixed | dontUnmap)) || ((flags & (fixed | dontUnmap)) && !(flags & mayMove)) ||
                ((flags & dontUnmap) && x[1] != x[2]) ||
                !x[1] || !x[2] || x[1] > maxReservation || x[2] > maxReservation ||
                x[0] % pageSize || ((flags & fixed) && x[4] % pageSize)) { x[0] = invalid; break; }
            const auto length = pageAlignedLength(x[1]), newSize = pageAlignedLength(x[2]);
            const auto source = x[0];
            if (source < lowMappingFloor || source > mappingLimit - length) { x[0] = invalid; break; }
            auto mapping = std::lower_bound(machine.segments.begin(), machine.segments.end(), source,
                [](const Segment& segment, uint64_t address) { return segment.address < address; });
            if (mapping == machine.segments.end()) { x[0] = uint64_t(-14); break; }
            const bool lazy = bool(mapping->anonymousPages);
            const bool sparse = mapping->bytes.empty() && !lazy;
            auto after = mapping;
            auto covered = source;
            const auto end = source + length;
            while (covered < end && after != machine.segments.end() && after->address == covered &&
                   after->mappedSize() <= end - covered && after->anonymous && !after->initialStack &&
                   !after->shared && !after->beyondFileEnd && after->flags == mapping->flags &&
                   after->omitOnFork == mapping->omitOnFork && after->wipeOnFork == mapping->wipeOnFork &&
                   after->sharedAnonymous == mapping->sharedAnonymous &&
                   bool(after->anonymousPages) == lazy && (lazy || after->bytes.empty() == sparse)) {
                covered += after->mappedSize();
                ++after;
            }
            if (covered != end) { x[0] = uint64_t(-14); break; }
            auto& next = current->process->nextMapping;
            auto target = source;
            if (flags & fixed) {
                target = x[4];
                if (target < mappingFloor || target > mappingLimit - newSize ||
                    (source < target + newSize && target < source + length)) { x[0] = invalid; break; }
            } else if (flags & dontUnmap) {
                target = freeMappingAddress(machine, next, newSize);
                if (!target) { x[0] = noMemory; break; }
            } else if (newSize > length) {
                const bool room = source <= mappingLimit - newSize &&
                    std::none_of(machine.segments.begin(), machine.segments.end(), [&](const Segment& candidate) {
                        return source + length < candidate.address + candidate.mappedSize() &&
                               candidate.address < source + newSize;
                    });
                if (!room) {
                    if (!(flags & mayMove)) { x[0] = noMemory; break; }
                    target = freeMappingAddress(machine, next, newSize);
                    if (!target) { x[0] = noMemory; break; }
                }
            }
            if (!sparse && !lazy && (newSize > memoryLimit / 2 ||
                                     !allowsBacking(machine, target, target + newSize, newSize, memoryLimit))) {
                x[0] = noMemory;
                break;
            }
            Segment moved{target, {}, mapping->flags, sparse || lazy ? newSize : 0,
                          false, {}, 0, true};
            moved.omitOnFork = mapping->omitOnFork;
            moved.wipeOnFork = mapping->wipeOnFork;
            moved.sharedAnonymous = mapping->sharedAnonymous;
            // In a guest address space the pages themselves move, unless the two ranges overlap.
            const bool movePages = lazy && machine.space && target != source &&
                                   !(source < target + newSize && target < source + length);
            if (movePages) {
            } else if (lazy) {
                moved.anonymousPages = AnonymousPages::allocate(newSize, anonymousBudget);
                if (!moved.anonymousPages) { x[0] = noMemory; break; }
                bool copied = true;
                for (auto part = mapping; part != after; ++part) {
                    const auto offset = part->address - source;
                    if (offset >= newSize) break;
                    const auto count = std::min(newSize - offset, part->mappedSize());
                    if (!part->anonymousPages->copyTo(*moved.anonymousPages, part->fileOffset, offset, count)) {
                        copied = false;
                        break;
                    }
                }
                if (!copied) { x[0] = noMemory; break; }
                for (auto part = mapping; part != after; ++part)
                    part->anonymousPages->release(part->fileOffset, part->mappedSize());
            } else if (!sparse) {
                if (std::next(mapping) == after) {
                    moved.bytes = std::move(mapping->bytes);
                    moved.bytes.resize(newSize);
                } else {
                    moved.bytes.resize(newSize);
                    for (auto part = mapping; part != after; ++part) {
                        const auto offset = part->address - source;
                        if (offset >= newSize) break;
                        const auto count = std::min<uint64_t>(part->bytes.size(), newSize - offset);
                        std::copy_n(part->bytes.begin(), count, moved.bytes.mutableData() + offset);
                    }
                }
            }
            machine.segments.erase(mapping, after);
            if (flags & fixed) changeRange(machine, target, target + newSize, {});
            if (movePages) {
                moved.anonymousPages = AnonymousPages::allocateIn(machine.space, target, newSize);
                if (!moved.anonymousPages || !machine.space->move(source, target, std::min(length, newSize))) { x[0] = noMemory; break; }
                // The old range goes, or with MREMAP_DONTUNMAP is mapped afresh below.
                if (!(flags & dontUnmap)) machine.space->unmap(source, length);
            } else if (lazy && machine.space) {
                // The contents were gathered outside the address space, because the old and the new
                // range overlap. Now that the target is free they go to their place in it.
                auto placed = AnonymousPages::allocateIn(machine.space, target, newSize);
                if (!placed || !moved.anonymousPages->copyTo(*placed, 0, 0, newSize)) { x[0] = noMemory; break; }
                moved.anonymousPages = std::move(placed);
            }
            const auto movedFlags = moved.flags;
            const bool omitOnFork = moved.omitOnFork, wipeOnFork = moved.wipeOnFork;
            machine.insertSegment(std::move(moved));
            if (flags & dontUnmap) {
                Segment fresh{source, {}, movedFlags, sparse || lazy ? length : 0, false, {}, 0, true};
                fresh.omitOnFork = omitOnFork;
                fresh.wipeOnFork = wipeOnFork;
                if (lazy) {
                    fresh.anonymousPages = machine.space ? AnonymousPages::allocateIn(machine.space, source, length)
                                                         : AnonymousPages::allocate(length, anonymousBudget);
                    if (!fresh.anonymousPages) { x[0] = noMemory; break; }
                } else if (!sparse) {
                    fresh.bytes.resize(length);
                }
                machine.insertSegment(std::move(fresh));
            }
            next = std::max(next, target + newSize);
            machine.cpu.exclusiveSize = 0;
            x[0] = target;
            break;
        }
        case 226: // mprotect
            // A process that asked for memory never to gain execute permission (PR_SET_MDWE) is not
            // held to it here. Zygote asks for every app, and Android lifts it for an app whose own
            // libraries are laid out for 4 KiB pages: the linker reads such a library into memory
            // it then has to make executable. Here every app is loaded that way when it has to be
            // (bionic.linker.16kb.app_compat.enabled), and zygote does not know. Guest code that
            // gains execute permission is interpreted, never run by the processor directly.
            x[0] = protect(machine, x[0], x[1], x[2], memoryLimit, false, anonymousBudget);
            break;
        case 227: { // msync
            const auto length = x[1] <= maxReservation ? pageAlignedLength(x[1]) : UINT64_MAX;
            if (x[0] % pageSize || (x[2] & ~uint64_t{7}) || (x[2] & 5) == 5 ||
                length > UINT64_MAX - x[0]) { x[0] = invalid; break; }
            const auto end = x[0] + length;
            if (!fullyMapped(machine, x[0], end)) { x[0] = noMemory; break; }
            const bool guestData = dataDatabase && std::any_of(machine.segments.begin(), machine.segments.end(),
                [&](const Segment& segment) {
                    return segment.shared && x[0] < segment.address + segment.mappedSize() && segment.address < end &&
                        std::any_of(writableFiles.begin(), writableFiles.end(), [&](const auto& entry) {
                            return entry.second->persistent && entry.second->storage == segment.shared;
                        });
                });
            x[0] = !guestData || syncData() ? 0 : uint64_t(-5);
            break;
        }
        case 228: case 229: { // mlock, munlock. Resident guest mappings have no guest swap.
            const auto start = x[0] & ~(pageSize - 1);
            if (x[1] > maxReservation || x[1] > UINT64_MAX - x[0]) { x[0] = invalid; break; }
            const auto size = pageAlignedLength(x[1] + x[0] - start);
            x[0] = size > UINT64_MAX - start || !fullyMapped(machine, start, start + size) ? noMemory : 0;
            break;
        }
        case 230: { // mlockall: lmkd's first call. It stopped there, and Android had no low-memory killer.
            // NESTVANE_TEST_NO_LMKD=1 leaves it stopped there, as before 1.11, for comparing starts.
            if (static const bool parked = std::getenv("NESTVANE_TEST_NO_LMKD"); parked) return false;
            // Until the home screen is up it waits here, where it always stopped before: once lmkd listens,
            // the activity manager talks to it, and that cost the start a few hundred milliseconds.
            if (appsHeldNow()) {
                current->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                return waitForEvents();
            }
            constexpr uint64_t current = 1, future = 2, onFault = 4;
            x[0] = !x[0] || (x[0] & ~(current | future | onFault)) || x[0] == onFault ? invalid : 0;
            break;
        }
        case 231: x[0] = 0; break; // munlockall
        // process_mrelease: lmkd asks at its start whether there is one, with no process. A process
        // that is ended gives its memory back at once here, so there is nothing more to release.
        case 448: {
            const auto handle = descriptors().find(uint32_t(x[0]));
            x[0] = x[1] ? invalid : handle == descriptors().end() || handle->second.node != Node::processHandle
                ? uint64_t(-9) : 0;
            break;
        }
        // pidfd_send_signal: not here (lmkd's kills would use it). Saying so lets the caller go on, where
        // an unknown call would stop it.
        case 424: x[0] = uint64_t(-38); break;
        case 232: { // mincore: residency of the guest backing for each virtual page.
            if (x[0] % pageSize || x[1] > maxReservation) { x[0] = invalid; break; }
            const auto length = pageAlignedLength(x[1]);
            if (x[0] > UINT64_MAX - length || !fullyMapped(machine, x[0], x[0] + length)) { x[0] = noMemory; break; }
            std::vector<uint8_t> pages(length / pageSize);
            auto segment = machine.segments.begin();
            for (uint64_t i = 0; i < pages.size(); ++i) {
                const auto address = x[0] + i * pageSize;
                while (segment->address + segment->mappedSize() <= address) ++segment;
                const auto offset = address - segment->address;
                pages[i] = segment->anonymousPages ? segment->anonymousPages->resident(segment->fileOffset + offset) :
                           offset < segment->accessibleSize();
            }
            x[0] = machine.write(x[2], pages) ? 0 : uint64_t(-14);
            break;
        }
        case 233: { // madvise
            const auto advice = int32_t(x[2]);
            if (advice < 0) { x[0] = invalid; break; }
            if (advice > 4 && advice != 8 && advice != 9 && advice != 10 && advice != 11 && advice != 18 && advice != 19) return false;
            if (x[0] % pageSize || x[1] > maxReservation) { x[0] = invalid; break; }
            const auto length = pageAlignedLength(x[1]);
            if (x[0] > UINT64_MAX - length || !fullyMapped(machine, x[0], x[0] + length)) {
                x[0] = noMemory;
                break;
            }
            if (advice == 9) { // MADV_REMOVE: the range of a shared memory file reads as zero afterwards.
                static const std::vector<uint8_t> zeros(64 * 1024);
                for (uint64_t done = 0; done < length;) {
                    const auto count = std::min<uint64_t>(length - done, zeros.size());
                    machine.write(x[0] + done, std::span(zeros).first(count));  // what cannot be written stays
                    done += count;
                }
                x[0] = 0;
                break;
            }
            if (advice == 8) { // MADV_FREE: private anonymous pages are eligible for lazy reclaim.
                const auto start = x[0], end = start + length;
                const bool unsuitable = std::any_of(machine.segments.begin(), machine.segments.end(), [&](const Segment& area) {
                    return area.address < end && start < area.address + area.mappedSize() &&
                           (!area.anonymous || area.sharedAnonymous || area.shared);
                });
                x[0] = unsuitable ? invalid : 0;
                break;
            }
            if (advice >= 10) {
                const auto start = x[0], end = start + length;
                if (advice == 18 && std::any_of(machine.segments.begin(), machine.segments.end(), [&](const Segment& area) {
                    return area.address < end && start < area.address + area.mappedSize() &&
                           (!area.anonymous || area.sharedAnonymous || area.shared);
                })) { x[0] = invalid; break; }
                if (length) {
                    const auto mode = advice == 10 ? ForkAdvice::omit : advice == 11 ? ForkAdvice::inherit :
                                      advice == 18 ? ForkAdvice::wipe : ForkAdvice::keep;
                    changeRange(machine, start, end, {}, mode);
                }
            }
            if (advice == 4) {
                const auto start = x[0], end = start + length;
                for (const auto& segment : machine.segments) {
                    if (start < segment.address + segment.mappedSize() && segment.address < end &&
                        !segment.anonymous) return false;
                }
                for (auto& segment : machine.segments) {
                    const auto left = std::max(start, segment.address);
                    const auto right = std::min(end, segment.address + segment.accessibleSize());
                    if (left >= right) continue;
                    if (segment.anonymousPages) {
                        segment.anonymousPages->discard(segment.fileOffset + left - segment.address, right - left);
                    } else {
                        std::fill(segment.data() + (left - segment.address), segment.data() + (right - segment.address), 0);
                    }
                }
                machine.cpu.exclusiveSize = 0;
            }
            x[0] = 0;
            break;
        }
        case 140: case 141: { // PRIO_PROCESS addresses a Linux task by TID.
            if (x[0] != 0) return false;
            const auto task = threads.find(x[1] ? uint32_t(x[1]) : currentTid);
            if (task == threads.end() || task->second.wait == Wait::exited) {
                x[0] = uint64_t(-3);
                break;
            }
            if (number == 141) {
                x[0] = 20 - task->second.nice;
                break;
            }
            constexpr uint64_t capSysNice = 1ULL << 23;
            const auto& owner = task->second.process->userIds;
            const bool privileged = current->process->effectiveCaps & capSysNice;
            if (!privileged && userId() != owner[0] && userId() != owner[1]) {
                x[0] = uint64_t(-1);
                break;
            }
            const auto priority = std::clamp(static_cast<int32_t>(x[2]), -20, 19);
            if (priority < task->second.nice && !privileged) {
                const auto& limits = task->second.process->limits;
                const auto limit = limits.find(niceLimitResource);
                const auto soft = limit == limits.end() ? 0 : limit->second[0];
                if (uint64_t(20 - priority) > soft) { x[0] = uint64_t(-13); break; }
            }
            task->second.nice = priority;
            x[0] = 0;
            break;
        }
        case 90: case 91: { // capget and capset for the current guest process.
            constexpr uint32_t version = 0x20080522, legacy = 0x19980330;
            const bool setting = number == 91;
            auto& process = *current->process;
            uint64_t header = 0;
            if (!machine.load64(x[0], header)) { x[0] = uint64_t(-14); break; }
            const auto requested = uint32_t(header);
            if (requested != version && requested != 0x20071026 && requested != legacy) {
                if (!writeGuest(machine, x[0], version)) x[0] = uint64_t(-14);
                else x[0] = setting || x[1] ? invalid : 0;
                break;
            }
            if (!setting && !x[1]) { x[0] = 0; break; }
            const auto subject = int32_t(header >> 32);
            if (subject < 0) { x[0] = invalid; break; }
            if (subject && uint32_t(subject) != processId()) {
                x[0] = setting ? uint64_t(-1) : uint64_t(-3);
                break;
            }
            const auto size = requested == legacy ? 12 : 24;
            std::array<uint32_t, 6> caps{};
            if (setting) {
                std::vector<uint8_t> dataScratch;
                const auto* data = machine.read(x[1], size, 4, dataScratch);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(caps.data(), data, size);
                const auto mask = [&](unsigned i) {
                    return (caps[i] | (uint64_t(caps[i + 3]) << 32)) & Process::allCapabilities;
                };
                const auto effective = mask(0), permitted = mask(1), inheritable = mask(2);
                constexpr uint64_t setpcap = 1ULL << 8;
                if ((effective & ~permitted) || (permitted & ~process.permittedCaps) ||
                    (inheritable & ~(process.inheritableCaps | process.capabilityBound)) ||
                    (!(process.effectiveCaps & setpcap) &&
                     (inheritable & ~(process.inheritableCaps | process.permittedCaps)))) {
                    x[0] = uint64_t(-1);
                    break;
                }
                process.effectiveCaps = effective;
                process.permittedCaps = permitted;
                process.inheritableCaps = inheritable;
                process.ambientCaps &= permitted & inheritable;
                x[0] = 0;
                break;
            }
            const auto effective = process.effectiveCaps, permitted = process.permittedCaps;
            const auto inheritable = process.inheritableCaps;
            caps = {uint32_t(effective), uint32_t(permitted), uint32_t(inheritable),
                    uint32_t(effective >> 32), uint32_t(permitted >> 32), uint32_t(inheritable >> 32)};
            x[0] = machine.write(x[1], {reinterpret_cast<const uint8_t*>(caps.data()), size_t(size)})
                ? 0 : uint64_t(-14);
            break;
        }
        case 277: { // seccomp. Chromium's sandbox uses it rather than prctl.
            constexpr uint64_t threadSync = 1, logged = 2, speculation = 4;
            if (x[0] == 1) { // SECCOMP_SET_MODE_FILTER
                if (x[1] & ~(threadSync | logged | speculation)) { x[0] = invalid; break; }
                const auto result = installSeccomp(machine, x[2]);
                // TSYNC: every thread of the process gets the filter just installed.
                if (!result && (x[1] & threadSync)) {
                    for (auto& [tid, thread] : threads) {
                        if (thread.process != current->process || &thread == current || thread.wait == Wait::exited) continue;
                        thread.seccomp = current->seccomp;
                        thread.noNewPrivileges = thread.noNewPrivileges || current->noNewPrivileges;
                    }
                }
                x[0] = result;
            } else if (x[0] == 2) { // SECCOMP_GET_ACTION_AVAIL
                if (x[1]) { x[0] = invalid; break; }
                uint32_t action = 0;
                if (!machine.readInto(x[2], {reinterpret_cast<uint8_t*>(&action), sizeof(action)})) { x[0] = uint64_t(-14); break; }
                // The actions enforceSeccomp carries out (KILL_PROCESS, KILL_THREAD, TRAP, ERRNO, LOG, ALLOW);
                // with no listener and no tracer, USER_NOTIF and TRACE are not offered.
                constexpr uint32_t known[] = {0x80000000, 0, 0x00030000, 0x00050000, 0x7ffc0000, 0x7fff0000};
                x[0] = std::ranges::find(known, action) != std::end(known) ? 0 : uint64_t(-95);
            } else {
                x[0] = invalid; // STRICT mode and notification sizes are not offered.
            }
            break;
        }
        case 223: // fadvise64: advice about how a file will be read, which there is no cache here to act on.
            x[0] = descriptors().contains(uint32_t(x[0])) ? (x[3] > 5 ? invalid : 0) : uint64_t(-9);
            break;
        case 167: { // Guest process controls.
            auto& process = *current->process;
            if (x[0] == 21) { x[0] = current->seccomp ? 2 : 0; break; } // PR_GET_SECCOMP
            // PR_PAC_SET/GET_ENABLED_KEYS: there is no pointer authentication here, as on a kernel without it.
            if (x[0] == 60 || x[0] == 61) { x[0] = invalid; break; }
            if (x[0] == 22) { x[0] = x[1] == 2 ? installSeccomp(machine, x[2]) : invalid; break; }
            if (x[0] == 38 || x[0] == 39) { // PR_SET/GET_NO_NEW_PRIVS
                const bool set = x[0] == 38;
                if (x[1] != uint64_t(set) || x[2] || x[3] || x[4]) { x[0] = invalid; break; }
                if (set) current->noNewPrivileges = true;
                x[0] = set ? 0 : current->noNewPrivileges;
                break;
            }
            if (x[0] == 55 || x[0] == 56) { // PR_SET_MDWE, PR_GET_MDWE
                if (x[2] || x[3] || x[4]) { x[0] = invalid; break; }
                if (x[0] == 56) { x[0] = x[1] ? invalid : process.mdwe; break; }
                const auto flags = x[1];
                if (flags > (mdweRefuseExecGain | mdweNoInherit) || flags == mdweNoInherit) {
                    x[0] = invalid;
                    break;
                }
                if (process.mdwe && process.mdwe != flags) { x[0] = uint64_t(-1); break; }
                process.mdwe = flags;
                x[0] = 0;
                break;
            }
            if (x[0] == 0x59616d61) { x[0] = invalid; break; } // PR_SET_PTRACER without Yama.
            if (x[0] == 3) { x[0] = process.dumpable; break; } // PR_GET_DUMPABLE
            if (x[0] == 4) { // PR_SET_DUMPABLE
                if (x[1] > 1) { x[0] = invalid; break; }
                process.dumpable = x[1];
                x[0] = 0;
                if (!process.dumpable || !androidLogObserver) break;

                // Bionic places this record at the beginning of its abort-message mapping.
                constexpr uint64_t magic1 = 0xb18e40886ac388f0ULL;
                constexpr uint64_t magic2 = 0xc6dfba755a1de0b5ULL;
                for (const auto& area : machine.segments) {
                    std::array<uint64_t, 3> header{};
                    if (!(area.flags & 4) || area.accessibleSize() < sizeof(header)) continue;
                    if (!machine.readInto(area.address,
                            {reinterpret_cast<uint8_t*>(header.data()), sizeof(header)})) continue;
                    if (header[0] != magic1 || header[1] != magic2 || header[2] <= sizeof(header)) continue;
                    std::array<uint8_t, 512> buffer{};
                    const auto length = std::min<uint64_t>(buffer.size(), header[2] - sizeof(header));
                    if (!machine.readInto(area.address + sizeof(header), std::span(buffer).first(length))) continue;
                    std::string message = "Guest native abort: PID=" + std::to_string(processId()) +
                        " TID=" + std::to_string(currentTid) + " " + process.context + ": ";
                    for (const auto ch : std::span(buffer).first(length)) {
                        if (!ch) break;
                        message += ch >= 32 && ch < 127 ? char(ch) : ' ';
                    }
                    androidLogObserver(message);
                    break;
                }
                break;
            }
            if (x[0] == 7) { x[0] = bool(process.secureBits & secureKeepCaps); break; }
            if (x[0] == 8) { // PR_SET_KEEPCAPS
                if (x[1] > 1) { x[0] = invalid; break; }
                if (process.secureBits & secureKeepCapsLocked) { x[0] = uint64_t(-1); break; }
                process.secureBits = (process.secureBits & ~secureKeepCaps) | (x[1] ? secureKeepCaps : 0);
                x[0] = 0;
                break;
            }
            if (x[0] == 27) { x[0] = process.secureBits; break; } // PR_GET_SECUREBITS
            if (x[0] == 28) { // PR_SET_SECUREBITS
                const auto locks = process.secureBits & 0xaa;
                if (x[1] > 0xff || ((process.secureBits ^ x[1]) & (locks | (locks >> 1))) ||
                    !(process.effectiveCaps & (1ULL << 8))) { x[0] = uint64_t(-1); break; }
                process.secureBits = x[1];
                x[0] = 0;
                break;
            }
            if (x[0] == 15) { // PR_SET_NAME
                std::array<uint8_t, 16> name{};
                bool readable = true;
                for (size_t i = 0; i + 1 < name.size(); ++i) {
                    if (x[1] > UINT64_MAX - i) { readable = false; break; }
                    const auto* value = machine.read(x[1] + i, 1, 4);
                    if (!value) { readable = false; break; }
                    if (!*value) break;
                    name[i] = *value;
                }
                if (readable) current->name = name;
                x[0] = readable ? 0 : uint64_t(-14);
                break;
            }
            if (x[0] == 16) { // PR_GET_NAME
                x[0] = machine.write(x[1], current->name) ? 0 : uint64_t(-14);
                break;
            }
            if (x[0] == 23) { // PR_CAPBSET_READ
                x[0] = x[1] <= 40 ? (process.capabilityBound >> x[1]) & 1 : invalid;
                break;
            }
            if (x[0] == 24) { // PR_CAPBSET_DROP
                if (x[1] > 40) { x[0] = invalid; break; }
                if (!(process.effectiveCaps & (1ULL << 8))) { x[0] = uint64_t(-1); break; }
                process.capabilityBound &= ~(1ULL << x[1]);
                x[0] = 0;
                break;
            }
            if (x[0] == 47 && !x[3] && !x[4]) { // PR_CAP_AMBIENT
                if (x[1] == 4 && !x[2]) { process.ambientCaps = 0; x[0] = 0; break; }
                if (x[2] > 40 || x[1] < 1 || x[1] > 3) { x[0] = invalid; break; }
                const auto bit = 1ULL << x[2];
                if (x[1] == 1) { x[0] = bool(process.ambientCaps & bit); break; }
                if (x[1] == 3) { process.ambientCaps &= ~bit; x[0] = 0; break; }
                if ((process.secureBits & secureNoAmbientRaise) ||
                    !(process.permittedCaps & process.inheritableCaps & bit)) { x[0] = uint64_t(-1); break; }
                process.ambientCaps |= bit;
                x[0] = 0;
                break;
            }
            if (x[0] == 50 || x[0] == 51 || x[0] == 63 || x[0] == 64) { x[0] = invalid; break; } // SVE and SME are unavailable.

            if (x[0] == 55) { x[0] = invalid; break; } // Tagged address ABI disabled.
            // PR_SET_VMA_ANON_NAME is advisory in this probe.
            if (x[0] != 0x53564d41 || x[1] != 0) return false;
            x[0] = x[2] % pageSize || x[3] > maxReservation || x[2] > UINT64_MAX - x[3]
                ? uint64_t(-22)
                : fullyMapped(machine, x[2], x[2] + x[3]) ? 0 : uint64_t(-12);
            break;
        }
        case 129: { // kill: guest process and process-group signaling.
            const int32_t target = x[0];
            const uint32_t signal = x[1];
            if (signal > 64) { x[0] = invalid; break; }
            const auto matches = [&](const Thread& task) {
                if (task.wait == Wait::exited) return false;
                const auto id = task.process->id;
                if (target > 0) return id == uint32_t(target);
                if (target == -1) return id > 1 && id != processId();
                const auto group = target ? uint64_t(-int64_t(target)) : current->process->group;
                return task.process->group == group;
            };
            const auto found = std::find_if(threads.begin(), threads.end(),
                                           [&](const auto& task) { return matches(task.second); });
            if (found == threads.end()) { x[0] = uint64_t(-3); break; }
            // ART blocks SIGQUIT in its thread group and consumes it on Signal Catcher.
            constexpr uint32_t quit = 3;
            if (signal && signal != quit && signal != 9 && signal != 15) return false;
            const auto allowed = [&](const auto& task) {
                const auto& sender = current->process->userIds;
                const auto& receiver = task.process->userIds;
                constexpr uint64_t capKill = 1ULL << 5;
                return matches(task) && ((current->process->effectiveCaps & capKill) ||
                    sender[0] == receiver[0] || sender[0] == receiver[2] ||
                    sender[1] == receiver[0] || sender[1] == receiver[2]);
            };
            bool permitted = false;
            std::map<uint64_t, std::shared_ptr<Process>> victims;
            for (const auto& [tid, task] : threads) {
                if (!allowed(task)) continue;
                permitted = true;
                if (signal && task.process->id != pid)
                    victims.try_emplace(task.process->id, task.process);
            }
            if (signal == quit) {
                constexpr uint64_t mask = 1ULL << (quit - 1);
                const auto waiting = [&](const Thread& task) {
                    return task.wait == Wait::signal && (task.signalWaitMask & mask);
                };
                // An unmasked recipient would need an asynchronous signal delivery.
                for (const auto& [id, victim] : victims) {
                    if (std::ranges::any_of(threads, [&](const auto& item) {
                            const auto& task = item.second;
                            return task.process == victim && task.wait != Wait::exited &&
                                !(task.signalMask & mask) && !waiting(task);
                        })) return false;
                }
                std::array<uint32_t, 32> info{};
                info[0] = quit;
                info[4] = uint32_t(processId()); // SI_USER
                info[5] = current->process->userIds[0];
                for (const auto& [id, victim] : victims) {
                    if (victim->pendingSignals.contains(quit)) continue;
                    victim->pendingSignals.emplace(quit, info);
                    for (auto& [tid, task] : threads) {
                        if (task.process != victim || !waiting(task)) continue;
                        task.wait = Wait::none;
                        task.deadline.reset();
                        break;
                    }
                }
                x[0] = permitted ? 0 : uint64_t(-1);
                break;
            }
            if (signal == 15) {
                // Ignored, it does nothing. Caught, one of the process's threads that does not hold it
                // back runs the handler, as tgkill has it run (a shell catches it, and libsu ends its
                // root shell with it). Otherwise it ends the process; held back by every thread, that
                // would need it kept until one lets it through.
                std::erase_if(victims, [](const auto& item) { return item.second->signalActions[15][0] == 1; });
                constexpr uint64_t mask = 1ULL << 14;
                std::vector<uint64_t> caught;
                for (const auto& [id, victim] : victims) {
                    const auto open = [&](const Thread& task) {
                        return task.process == victim && task.wait != Wait::exited && !(task.signalMask & mask);
                    };
                    if (!victim->signalActions[15][0]) {
                        if (std::none_of(threads.begin(), threads.end(), [&](const auto& item) { return open(item.second); }))
                            return false;
                        continue;
                    }
                    const auto chosen = std::find_if(threads.begin(), threads.end(), [&](const auto& item) {
                        const auto& task = item.second;
                        return open(task) && (task.wait == Wait::none || task.wait == Wait::poll || task.wait == Wait::futex ||
                                              task.wait == Wait::signal || task.wait == Wait::signalSuspend ||
                                              task.wait == Wait::sleep);
                    });
                    if (chosen == threads.end()) return false;
                    auto& target = chosen->second;
                    std::array<uint32_t, 32> info{};
                    info[0] = 15;
                    info[4] = uint32_t(processId()); // SI_USER
                    info[5] = current->process->userIds[0];
                    if (!target.pendingSignals.contains(15)) target.pendingSignals.emplace(15, info);
                    if (target.wait == Wait::signal && (target.signalWaitMask & mask)) {
                        target.wait = Wait::none;
                        target.deadline.reset();
                    } else if (target.wait != Wait::none) {
                        target.signalInterruptedWait = target.wait;
                        target.wait = Wait::none;
                        target.nextSliceAfter.reset();
                    }
                    sliceEnd = 0;
                    caught.push_back(id);
                }
                for (const auto id : caught) victims.erase(id);
            }
            for (const auto& [id, victim] : victims) recallNativeThreads(victim.get());
            for (const auto& [id, victim] : victims) finishProcess(*victim, signal, true);
            std::erase_if(threads, [&](const auto& task) {
                return task.first != currentTid && victims.contains(task.second.process->id);
            });
            if (victims.contains(processId())) {
                current->wait = Wait::exited;
                sliceEnd = 0;
            }
            x[0] = permitted ? 0 : uint64_t(-1);
            break;
        }
        case 130: case 131: case 240: { // tkill, tgkill, rt_tgsigqueueinfo.
            const bool queued = number == 240;
            const bool grouped = number != 130;
            const int32_t tid = x[grouped ? 1 : 0];
            const int32_t group = grouped ? int32_t(x[0]) : 0;
            const uint32_t signal = x[grouped ? 2 : 1];
            std::array<uint32_t, 32> info{};
            const auto infoBytes = std::span(reinterpret_cast<uint8_t*>(info.data()), sizeof(info));
            if (queued && !machine.readInto(x[3], infoBytes)) {
                x[0] = uint64_t(-14);
                break;
            }
            if (tid <= 0 || (grouped && group <= 0) || signal > 64) { x[0] = invalid; break; }
            if (queued && uint32_t(tid) != currentTid && (int32_t(info[2]) >= 0 || int32_t(info[2]) == -6)) {
                x[0] = uint64_t(-1);
                break;
            }
            const auto task = threads.find(tid);
            if (task == threads.end() || task->second.wait == Wait::exited ||
                (grouped && task->second.process->id != uint32_t(group))) { x[0] = uint64_t(-3); break; }
            auto& target = task->second;
            const auto& sender = current->process->userIds;
            const auto& receiver = target.process->userIds;
            constexpr uint64_t capKill = 1ULL << 5;
            if (!(current->process->effectiveCaps & capKill) &&
                sender[0] != receiver[0] && sender[0] != receiver[2] &&
                sender[1] != receiver[0] && sender[1] != receiver[2]) { x[0] = uint64_t(-1); break; }
            if (!signal) { x[0] = 0; break; }
            const uint64_t mask = 1ULL << (signal - 1);
            const bool waiting = target.wait == Wait::signal && (target.signalWaitMask & mask);
            const auto handler = target.process->signalActions[signal][0];
            if (handler == 1 && !waiting && !(target.signalMask & mask)) { x[0] = 0; break; }
            const bool fatalResend = queued && &target == current && !handler &&
                defaultFatalSignal(signal) && processId() != pid;
            const bool masked = target.signalMask & mask;
            const bool caught = handler > 1;
            const bool scheduleHandler = caught && !masked && !waiting;
            if ((!caught && signal >= 32 && !waiting) ||
                (!waiting && !masked && !fatalResend && !caught)) return false;
            if (scheduleHandler && target.wait != Wait::none && target.wait != Wait::poll &&
                target.wait != Wait::futex && target.wait != Wait::signal && target.wait != Wait::signalSuspend &&
                target.wait != Wait::sleep && target.wait != Wait::suspended) return false;
            if (signal >= 32 && target.pendingSignals.size() >= 1024) { x[0] = uint64_t(-11); break; }
            info[0] = signal;
            if (!queued) {
                info[2] = uint32_t(-6); // SI_TKILL
                info[4] = uint32_t(processId());
                info[5] = sender[0];
            }
            if (signal >= 32 || !target.pendingSignals.contains(signal))
                target.pendingSignals.emplace(signal, info);
            x[0] = 0;
            if (waiting) {
                target.wait = Wait::none;
                target.deadline.reset();
            } else if (scheduleHandler) {
                if (target.wait != Wait::none) target.signalInterruptedWait = target.wait;
                target.wait = Wait::none;
                target.nextSliceAfter.reset();
                sliceEnd = 0;
            } else if (fatalResend) {
                terminateOnPendingSignal();
            }
            break;
        }
        case 133: { // rt_sigsuspend waits for a handler under a temporary mask.
            if (x[1] != sizeof(uint64_t)) { x[0] = invalid; break; }
            uint64_t mask = 0;
            if (!machine.load64(x[0], mask)) { x[0] = uint64_t(-14); break; }
            current->suspendSignalMask = current->signalMask;
            current->signalMask = mask & ~unblockableSignals;
            current->deadline.reset();
            current->wait = Wait::signalSuspend;
            sliceEnd = 0;
            terminateOnPendingSignal();
            break;
        }
        case 137: { // rt_sigtimedwait synchronously consumes a pending guest signal.
            if (x[3] != 8) { x[0] = invalid; break; }
            uint64_t requested = 0;
            if (!machine.load64(x[0], requested)) { x[0] = uint64_t(-14); break; }
            requested &= ~((1ULL << 8) | (1ULL << 18)); // SIGKILL and SIGSTOP cannot be waited on.
            using namespace std::chrono;
            std::optional<nanoseconds> timeout;
            if (x[2]) {
                std::array<int64_t, 2> values{};
                std::vector<uint8_t> dataScratch;
                const auto* data = machine.read(x[2], sizeof(values), 4, dataScratch);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(values.data(), data, sizeof(values));
                if (values[0] < 0 || values[1] < 0 || values[1] >= 1'000'000'000) { x[0] = invalid; break; }
                timeout = values[0] > (INT64_MAX - values[1]) / 1'000'000'000
                    ? nanoseconds::max() : seconds(values[0]) + nanoseconds(values[1]);
            }
            current->signalWaitMask = 0;
            const auto consume = [&](PendingSignals& pending) {
                const auto found = std::ranges::find_if(pending, [&](const auto& entry) {
                    return requested & (1ULL << (entry.first - 1));
                });
                if (found == pending.end()) return false;
                if (x[1] && !writeGuest(machine, x[1], found->second)) {
                    x[0] = uint64_t(-14);
                } else {
                    x[0] = found->first;
                    pending.erase(found);
                }
                return true;
            };
            if (consume(current->pendingSignals) || consume(current->process->pendingSignals)) break;
            // A child that has ended is kept apart from the other pending signals. A program that
            // blocks SIGCHLD and waits for it here, as toybox's timeout does, is told of it too.
            if (auto& child = current->process->childSignal; (requested & sigChildMask) && child) {
                SignalWords info{};
                info[0] = sigChild;
                info[2] = child->code;
                info[4] = child->pid;
                info[5] = child->uid;
                info[6] = child->status;
                if (x[1] && !writeGuest(machine, x[1], info)) {
                    x[0] = uint64_t(-14);
                } else {
                    x[0] = sigChild;
                    child.reset();
                }
                break;
            }
            if (timeout && timeout->count() == 0) { x[0] = uint64_t(-11); break; }
            if (timeout) {
                const auto now = steady_clock::now();
                current->deadline = *timeout >= steady_clock::time_point::max() - now
                    ? steady_clock::time_point::max() : now + duration_cast<steady_clock::duration>(*timeout);
            }
            current->signalWaitMask = requested;
            current->wait = Wait::signal;
            sliceEnd = 0;
            break;
        }
        case 134: { // rt_sigaction
            if (!x[0] || x[0] >= current->process->signalActions.size() || x[3] != 8 ||
                (x[1] && (x[0] == 9 || x[0] == 19))) { x[0] = invalid; break; }
            auto& action = current->process->signalActions[x[0]];
            auto next = action;
            if (x[1]) {
                std::vector<uint8_t> dataScratch;
                const auto* data = machine.read(x[1], sizeof(next), 4, dataScratch);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(next.data(), data, sizeof(next));
            }
            if (x[2] && !writeGuest(machine, x[2], action)) { x[0] = uint64_t(-14); break; }
            action = next;
            x[0] = 0;
            break;
        }
        case 135: { // rt_sigprocmask
            if (x[3] != 8 || (x[1] && x[0] > 2)) { x[0] = invalid; break; }
            auto next = current->signalMask;
            if (x[1]) {
                uint64_t mask = 0;
                if (!machine.load64(x[1], mask)) { x[0] = uint64_t(-14); break; }
                mask &= ~unblockableSignals;
                next = x[0] == 0 ? next | mask : x[0] == 1 ? next & ~mask : mask;
            }
            if (x[2] && !writeGuest(machine, x[2], current->signalMask)) { x[0] = uint64_t(-14); break; }
            current->signalMask = next;
            x[0] = 0;
            if (x[1]) {
                terminateOnPendingSignal();
                if (!current->pendingSignals.empty()) sliceEnd = 0;
            }
            break;
        }
        case 132: { // sigaltstack
            const auto bytes = [](const auto& array) {
                return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(array.data()), sizeof(array));
            };
            auto next = current->alternateStack;
            const bool active = onSignalStack(next, machine.cpu.sp);
            if (x[0]) {
                if (active) { x[0] = uint64_t(-1); break; }
                std::vector<uint8_t> dataScratch;
                auto data = machine.read(x[0], sizeof(next), 4, dataScratch);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(next.data(), data, sizeof(next));
                if (uint32_t(next[1]) == ssDisable) next = {0, ssDisable, 0};
                else if (uint32_t(next[1]) || next[2] > UINT64_MAX - next[0]) { x[0] = invalid; break; }
                else if (next[2] < 2048) { x[0] = noMemory; break; }
                else next[1] = 0;
            }
            auto previous = current->alternateStack;
            if (active) previous[1] = ssOnStack;
            if (x[1] && !machine.write(x[1], bytes(previous))) { x[0] = uint64_t(-14); break; }
            current->alternateStack = next;
            x[0] = 0;
            break;
        }
        default:
            return false;
    }
    return true;
}

} // namespace nestvane
