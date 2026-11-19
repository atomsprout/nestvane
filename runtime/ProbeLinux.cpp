#include "ProbeLinux.hpp"

#include "BpfProgram.hpp"
#include "BpfTypes.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <atomic>
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

constexpr uint64_t maxReservation = 16ULL * 1024 * 1024 * 1024;
constexpr uint64_t mappingFloor = 0x1'0000'0000;
constexpr uint64_t lowMappingFloor = 0x10000;
constexpr uint64_t mappingLimit = 0x20'0000'0000; // 128 GiB guest arena; includes 24 randomized page bits.
constexpr uint64_t maxGuestFileBytes = 2 * 1024 * 1024;
constexpr size_t maxWritableFiles = 1024;
// Leave space for boot-created directories alongside a restored /data tree.
constexpr size_t maxGuestDirectories = 2048;
constexpr size_t maxGuestTasks = 256;
constexpr uint32_t guestLoopbackIndex = 1;
constexpr uint32_t guestLoopbackMtu = 65536;
constexpr size_t bpfMapBudget = 32 * 1024 * 1024;
constexpr std::string_view cgroupRoot = "/sys/fs/cgroup";
constexpr std::string_view cgroupProcs = "/cgroup.procs";
constexpr uint64_t lazyAnonymousThreshold = 4 * 1024 * 1024;
constexpr uint64_t noMemory = uint64_t(-12);
constexpr uint8_t mdweRefuseExecGain = 1, mdweNoInherit = 2;
constexpr uint32_t sealSeal = 1, sealShrink = 2, sealGrow = 4, sealWrite = 8, sealFutureWrite = 16;
constexpr unsigned sigChild = 17;
constexpr uint64_t receiveTimeoutOld = 20, receiveTimeoutNew = 66;
constexpr uint64_t sendTimeoutOld = 21, sendTimeoutNew = 67;
constexpr std::string_view touchDevicePath = "/dev/input/event0";
constexpr std::string_view graphicsDevice = "/dev/goldfish_pipe_dprctd";
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
                   saNoDefer = 0x40000000, saResetHand = 0x80000000;
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
    std::vector<Segment> pages;
    for (auto& segment : machine.segments) {
        const auto segmentEnd = segment.address + segment.mappedSize();
        const auto left = std::max(start, segment.address), right = std::min(end, segmentEnd);
        if (left >= right) {
            pages.push_back(std::move(segment));
            continue;
        }
        if (!retain && segment.anonymousPages)
            segment.anonymousPages->discard(segment.fileOffset + left - segment.address, right - left);
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
                    append({from, {first, first + (to - from)}, permissions, 0, false, {}, 0,
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
            auto first = segment.bytes.begin() + (from - segment.address);
            auto last = segment.bytes.begin() + (to - segment.address);
            append({from, {first, last}, permissions, 0, false, {}, 0, segment.anonymous, false, segment.initialStack, segment.fileMapping});
        };
        part(segment.address, left, segment.flags);
        if (retain) part(left, right, flags.value_or(segment.flags), true);
        part(right, segmentEnd, segment.flags);
    }
    machine.segments = std::move(pages);
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

uint64_t statFile(Machine& machine, uint64_t address, uint64_t size, uint64_t inode, uint32_t mode, uint32_t uid = 0, uint32_t gid = 0, uint64_t device = 0) {
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
    return writeGuest(machine, address, stat) ? 0 : uint64_t(-14);
}

} // namespace

ProbeLinux::ProbeLinux(uint64_t pid, uint64_t uid, uint64_t memoryLimit)
    : pid(pid), memoryLimit(memoryLimit),
      anonymousBudget(std::make_shared<AnonymousBudget>(AnonymousBudget{0, 0, 8 * memoryLimit, memoryLimit})),
      current(&threads[pid]), currentTid(pid), nextTid(pid + 1) {
    current->process = std::make_shared<Process>();
    current->process->id = pid;
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
}

void ProbeLinux::alias(std::string path, std::string target) {
    links.emplace(std::move(path), std::move(target));
}

std::optional<std::string> ProbeLinux::resolve(std::string_view path) const {
    if (path == "/proc/self/exe" && !current->process->executable.empty())
        return current->process->executable;
    if (auto found = links.find(backingPath(path)); found != links.end()) return found->second;
    constexpr std::string_view fdPrefix = "/proc/self/fd/";
    if (!path.starts_with(fdPrefix)) return {};
    path.remove_prefix(fdPrefix.size());
    uint32_t fd = 0;
    const auto result = std::from_chars(path.data(), path.data() + path.size(), fd);
    if (result.ec != std::errc{} || result.ptr != path.data() + path.size()) return {};
    if (const auto found = descriptors().find(fd); found != descriptors().end()) return found->second.visiblePath.empty() ? found->second.path : found->second.visiblePath;
    return {};
}

std::optional<std::string> ProbeLinux::followLinks(std::string path, bool followFinal) const {
    for (unsigned depth = 0; depth <= 32; ++depth) {
        const std::string_view original(path);
        bool replaced = false;
        for (size_t end = path.find('/', 1); ; end = path.find('/', end + 1)) {
            if (end == std::string::npos && !followFinal) break;
            const auto prefix = original.substr(0, end);
            if (const auto link = resolve(prefix)) {
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
    if (guestTouch) addDirectory("/dev/input", 0755);
}

void ProbeLinux::addBootTmpfs(std::string path, uint32_t mode, uint32_t gid, uint64_t flags) {
    addDirectory(path, mode);
    directories.at(path).gid = gid;
    auto& group = *current->mountNamespace;
    group.mounts.emplace(path, MountView{flags, mode, gid, group.rootPropagation, path});
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
    return backingPath(path, *current->mountNamespace);
}

std::string ProbeLinux::backingPath(std::string_view path, const MountNamespace& mountNamespace) {
    if (path.size() > 1 && path[1] == '\0') return std::string(path);
    size_t longest = 0;
    const MountView* view = nullptr;
    for (const auto& [mountpoint, entry] : mountNamespace.mounts) {
        if ((path == mountpoint || inside(path, mountpoint)) && mountpoint.size() > longest) {
            longest = mountpoint.size();
            view = &entry;
        }
    }
    if (!view) return std::string(path);
    return view->backing + std::string(path.substr(longest));
}

bool ProbeLinux::writablePath(std::string_view path) const {
    const auto* view = mountFor(path);
    return view && !(view->flags & 1);
}

uint64_t ProbeLinux::bindMount(std::string_view from, const std::string& to, bool recursive) {
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
    if (const auto mode = directories.find(source); mode != directories.end()) view.mode = mode->second.mode;
    view.propagation = propagation;
    view.covered = {};
    const auto overlay = [&](const std::string& path, MountView entry) {
        if (auto old = mounts.find(path); old != mounts.end())
            entry.covered = std::make_shared<MountView>(old->second);
        mounts.insert_or_assign(path, std::move(entry));
    };
    std::vector<std::pair<std::string, MountView>> children;
    if (recursive) {
        const std::string prefix = std::string(from) + "/";
        for (auto it = mounts.lower_bound(prefix); it != mounts.end() && inside(it->first, from); ++it) {
            auto entry = it->second;
            entry.covered = {};
            children.emplace_back(to + it->first.substr(from.size()), std::move(entry));
        }
    }
    if (mounts.size() + children.size() + 1 > 128) return uint64_t(-28);
    overlay(to, std::move(view));
    for (auto& [path, entry] : children) overlay(path, std::move(entry));
    ++current->mountNamespace->generation;
    wakePollers();
    return 0;
}

uint64_t ProbeLinux::mountFilesystem(Machine& machine) {
    const auto& x = machine.cpu.x;
    if (userId() != 0) return uint64_t(-1);
    const auto target = readPath(machine, x[1]);
    if (!target) return uint64_t(-14);
    const auto path = normalizePath(*target);
    auto& group = *current->mountNamespace;
    auto& mounts = group.mounts;
    if (x[3] & 0x1000) { // MS_BIND
        if (x[3] & ~0x5000ULL) return invalid;
        const auto source = readPath(machine, x[0]);
        if (!source) return uint64_t(-14);
        return bindMount(normalizePath(*source), path, x[3] & 0x4000);
    }
    const uint64_t propagation = x[3] & 0x1e0000; // MS_UNBINDABLE/PRIVATE/SLAVE/SHARED
    if (propagation) {
        if ((propagation & (propagation - 1)) || (x[3] & ~(propagation | 0x4000)))
            return invalid;
        const auto mount = mounts.find(path);
        if (mount == mounts.end() && path != "/") return invalid;
        if (path == "/") group.rootPropagation = propagation;
        else mount->second.propagation = propagation;
        if (x[3] & 0x4000) { // MS_REC
            for (auto& [mountpoint, entry] : mounts) {
                if (path == "/" || inside(mountpoint, path)) entry.propagation = propagation;
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
    const bool bpf = *type == "bpf";
    if (!bpf && *type != "tmpfs") return uint64_t(-19);
    if (bpf && (*source != "bpf" || !options->empty())) return invalid;
    if (x[3] & ~15ULL) return uint64_t(-95);
    if (mounts.contains(path)) return uint64_t(-16);
    if (mounts.size() >= 128) return uint64_t(-28);
    uint32_t mode = bpf ? 0777 : 01777;
    auto remaining = std::string_view(*options);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        auto option = remaining.substr(0, comma);
        if (option.starts_with("mode=")) {
            option.remove_prefix(5);
            uint32_t parsed = 0;
            const auto result = std::from_chars(option.data(), option.data() + option.size(), parsed, 8);
            if (result.ec != std::errc{} || result.ptr != option.data() + option.size() || parsed > 01777)
                return invalid;
            mode = parsed;
        } else if (option != "uid=0" && option != "gid=0") {
            return invalid;
        }
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
    directories.emplace(backing, Directory{mode});
    mounts.emplace(path, MountView{x[3], mode, 0, inherited, std::move(backing), {}, bpf ? "bpf" : ""});
    ++current->mountNamespace->generation;
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
    group.mounts.emplace(path, MountView{flags | 8, 0755, 0, inherited, std::move(backing),
                                         {}, "binder", nullptr, true});
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
    group.mounts.emplace(path, MountView{flags, uint32_t(root.mode & 0777), root.gid,
                                        propagation, volume.backing, {}, device, &volume.image});
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
    const auto visible = normalizePath(*path);
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
    if ((nested != mounts.end() && inside(nested->first, visible)) ||
        (!found->second.ext4 && directory != directories.end() && inside(directory->first, backing)) ||
        (temporary != writableFiles.end() && inside(temporary->first, backing))) return uint64_t(-16);
    if (auto covered = found->second.covered) found->second = *covered;
    else mounts.erase(found);
    ++current->mountNamespace->generation;
    wakePollers();
    return 0;
}

bool ProbeLinux::directoryExists(std::string_view path) const {
    constexpr std::string_view taskPrefix = "/proc/self/task/";
    if (path.starts_with(taskPrefix)) {
        const auto id = path.substr(taskPrefix.size());
        uint64_t tid = 0;
        const auto result = std::from_chars(id.data(), id.data() + id.size(), tid);
        if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) return false;
        const auto thread = threads.find(tid);
        return thread != threads.end() && thread->second.process == current->process &&
               thread->second.wait != Wait::exited;
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
        const auto type = mount.binder ? " binder " : mount.ext4 ? " ext4 " :
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

uint64_t ProbeLinux::writableAllocation() const {
    uint64_t bytes = 0;
    for (const auto& [path, file] : writableFiles) bytes += file->storage->pages.size();
    return bytes;
}

uint64_t ProbeLinux::statFilesystem(Machine& machine, uint64_t address, std::string_view path) const {
    std::array<uint64_t, 15> stat{};
    stat[0] = 0x01021994; // TMPFS_MAGIC
    stat[1] = pageSize;
    for (const auto& [path, bytes] : files) {
        const auto ext4 = ext4Nodes.find(path);
        const auto size = ext4 == ext4Nodes.end() ? bytes.size() : ext4->second->size;
        stat[2] += pageAlignedLength(size) / pageSize;
    }
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
        stat[2] = maxWritableBytes / pageSize;
        stat[3] = stat[4] = (maxWritableBytes - writableAllocation()) / pageSize;
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
        if (auto file = files.find(name); file != files.end()) reader.files.emplace(*file);
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

uint64_t ProbeLinux::stat(Machine& machine, uint64_t address, const Descriptor& fd) const {
    if (fd.node == Node::touchDevice) return statFile(machine, address, 0, 70'265, 0020660, 0, 1004, 0x0d40);
    if (fd.node == Node::graphicsAddressSpace) return statFile(machine, address, 0, 70'266, 0020666, 1000, 1000, 0x100a0a);
    if (fd.node == Node::graphicsPipe) return statFile(machine, address, 0, 70'267, 0020666, 1000, 1000, 0x100a0b);
    if (fd.node == Node::ashmem) return statFile(machine, address, 0, 69'999, 0020666, 0, 0, 0x100a03);
    if (fd.temporary) {
        const auto& file = *fd.temporary;
        return statFile(machine, address, file.storage->size, file.inode, 0100000 | file.mode, file.uid, file.gid);
    }
    if (fd.node == Node::kernelSetting || fd.node == Node::ueventTrigger)
        return statFile(machine, address, 0, std::hash<std::string>{}(fd.path) | 1, 0100644);
    if (fd.cgroupFile)
        return statFile(machine, address, 0, std::hash<std::string>{}(fd.path) | 1,
                        0100000 | fd.cgroupFile->mode, fd.cgroupFile->uid, fd.cgroupFile->gid);
    if (fd.node == Node::ptyMaster) return statFile(machine, address, 0, 3, 0020666, 0, 0, 0x502);
    if (fd.node == Node::ptySlave && fd.pty)
        return statFile(machine, address, 0, 10000 + fd.pty->number, 0020620, fd.pty->owner, 5, 0x8800 | fd.pty->number);
    if (fd.node == Node::binder) {
        const auto& device = binderDevices.at(fd.path);
        constexpr uint64_t guestBinderMajor = 240; // Guest character-device numbers.
        return statFile(machine, address, 0, 70'000 + device.minor, 0020000 | device.mode,
                        device.uid, device.gid, guestBinderMajor << 8 | device.minor);
    }
    if (fd.node == Node::loopControl) return statFile(machine, address, 0, 3, 0020600, 0, 0, 0xaed);
    if (fd.node == Node::loopDevice) {
        const auto& loop = loopDevices.at(fd.path);
        return statFile(machine, address, 0, 65536 + loop.number, 0060000 | loop.mode,
                        loop.uid, loop.gid, 0x700 | loop.number);
    }
    if (const auto node = ext4Nodes.find(fd.path); node != ext4Nodes.end()) {
        const auto& inode = *node->second;
        return statFile(machine, address, inode.size, std::hash<std::string>{}(fd.path) | 1,
                        inode.mode, inode.uid, inode.gid);
    }
    if (fd.node == Node::directory) {
        const auto pos = directories.find(fd.path);
        const auto node = pos != directories.end() ? pos->second : Directory{0755};
        return statFile(machine, address, pageSize, std::hash<std::string>{}(fd.path) | 1,
                        0040000 | node.mode, node.uid, node.gid);
    }
    if (fd.mountNamespace)
        return statFile(machine, address, 0, fd.mountNamespace->id, 0100444);
    if (fd.snapshot) return statFile(machine, address, 0, 4, 0100444);
    if (fd.node == Node::entropy)
        return statFile(machine, address, 0, 5, 0020666, 0, 0, fd.path == "/dev/random" ? 0x108 : 0x109);
    if (kernelFiles.contains(fd.path))
        return statFile(machine, address, fd.bytes.size(), std::hash<std::string>{}(fd.path) | 1, 0100444);
    if (fd.node == Node::socket) {
        const auto bound = socketPaths.find(fd.path);
        if (bound != socketPaths.end()) {
            const auto& node = bound->second;
            return statFile(machine, address, 0, 3, 0140000 | node.mode, node.uid, node.gid);
        }
        return statFile(machine, address, 0, 3, 0140777);
    }
    if (fd.node != Node::regular) return statFile(machine, address, 0, 3, 0020666);
    const auto file = files.find(fd.path);
    const auto mode = fd.path.starts_with("/data/") || fd.path.find("/bin/") != std::string::npos
        ? 0100555 : 0100444;
    return statFile(machine, address, fd.bytes.size(), std::distance(files.begin(), file) + 8, mode);
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
    if (!currentContextTransitions.empty() && path == "/sys/fs/selinux/context")
        return Descriptor{std::string(path), {}, flags, Node::securityValidate};
    if (path == "/proc/self/cmdline") {
        Descriptor entry{std::string(path), {}, flags, Node::regular};
        entry.snapshot = std::make_shared<std::string>(current->process->commandLine);
        entry.bytes = {reinterpret_cast<const uint8_t*>(entry.snapshot->data()), entry.snapshot->size()};
        return entry;
    }
    if (const auto file = kernelFiles.find(path); file != kernelFiles.end()) {
        const auto& content = file->second;
        Descriptor entry{file->first, {}, flags, Node::regular};
        if (path.starts_with("/sys/devices/virtual/block/") && path.ends_with("/loop/backing_file"))
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
    if (kernelSettings.contains(path)) return Descriptor{std::string(path), {}, flags, Node::kernelSetting};
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
    if (graphicsPipeFactory && path == graphicsAddressDevice)
        return Descriptor{std::string(path), {}, flags, Node::graphicsAddressSpace};
    if (graphicsPipeFactory && path == graphicsDevice)
        return Descriptor{std::string(path), {}, flags, Node::graphicsPipe};
    if (path == "/dev/null") return Descriptor{std::string(path), {}, flags, Node::null};
    if (path == "/dev/random" || path == "/dev/urandom")
        return Descriptor{std::string(path), {}, flags, Node::entropy};
    if (path == "/dev/kmsg") return Descriptor{std::string(path), {}, flags, Node::kernelLog};
    if (directoryExists(path)) {
        Descriptor result{std::string(path), {}, flags, Node::directory};
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
            allocated = writableAllocation();
        }
        const auto limit = file.memoryFile ? std::max(maxWritableBytes, memoryLimit / 2) : maxWritableBytes;
        if (pages - file.storage->pages.size() + allocated > limit) return uint64_t(-28);
    }
    file.storage->pages.resize(pages);
    const auto zeroFrom = std::min(size, file.storage->size);
    const auto zeroTo = size < file.storage->size ? pages : size;
    std::fill(file.storage->pages.begin() + zeroFrom, file.storage->pages.begin() + zeroTo, 0);
    file.storage->size = size;
    return 0;
}

uint64_t ProbeLinux::writeFileBytes(const WritableFile& file, uint64_t offset, std::span<const uint8_t> bytes) {
    if (offset + bytes.size() > file.storage->size) {
        // An input mapping may share the output storage, whose allocation can move.
        const std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        if (const auto error = resize(file, offset + bytes.size()); error) return error;
        std::copy(copy.begin(), copy.end(), file.storage->pages.begin() + offset);
    } else {
        std::memmove(file.storage->pages.data() + offset, bytes.data(), bytes.size());
    }
    return 0;
}

bool ProbeLinux::canAccessParent(std::string_view path, bool modify) const {
    if (!userId()) return true;
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
            const auto& metadata = directory->second;
            permission = metadata.mode >> permissionShift(metadata.uid, metadata.gid);
        } else if (view && parent == root) {
            permission = view->mode >> permissionShift(0, view->gid);
        }
        if ((permission & requested) != requested) return false;
        immediate = false;
        parent = parent.substr(0, parent.rfind('/'));
    }
    return !modify || !immediate;
}

unsigned ProbeLinux::permissionShift(uint32_t uid, uint32_t gid) const {
    if (userId() == uid) return 6;
    const auto& groups = current->process->groups;
    return groupId() == gid || std::find(groups.begin(), groups.end(), gid) != groups.end() ? 3 : 0;
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
    const auto* input = machine.read(x[2], sizeof(request), 4);
    if (!input) return uint64_t(-14);
    std::memcpy(&request, input, sizeof(request));
    const bool query = x[1] == 5;
    if (request.type < readLock || request.type > unlock || (query && request.type == unlock) ||
        request.whence < 0 || request.whence > 2) return invalid;
    const auto base = request.whence == 0 ? 0U :
        request.whence == 1 ? *fd.offset : fd.temporary->storage->size;
    int64_t start, otherEnd = 0;
    if (base > INT64_MAX || __builtin_add_overflow(int64_t(base), request.start, &start) ||
        (request.length && __builtin_add_overflow(start, request.length, &otherEnd))) return uint64_t(-75);
    if (start < 0 || (request.length && otherEnd < 0)) return invalid;
    const uint64_t low = request.length < 0 ? otherEnd : start;
    const uint64_t high = request.length < 0 ? uint64_t(start) : request.length > 0 ? uint64_t(otherEnd) : endOfFile;
    auto& locks = fd.temporary->locks;
    const auto owner = processId();
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
                         int32_t(conflict->owner), 0};
        return writeGuest(machine, x[2], request) ? 0 : uint64_t(-14);
    }
    if (request.type != unlock && conflict != locks.end()) return uint64_t(-11);

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
    return 0;
}

void ProbeLinux::releaseRecordLocks(const Descriptor& fd, uint64_t owner) {
    if (fd.temporary) {
        std::erase_if(fd.temporary->locks, [owner](const auto& lock) { return lock.owner == owner; });
    }
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
            containsChild(links) || containsChild(socketPaths)) return uint64_t(-39);
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
        if (input->starts_with('/') || uint32_t(fd) == uint32_t(-100)) {
            path = normalizePath(*input);
            return 0;
        }
        const auto directory = descriptors().find(uint32_t(fd));
        if (directory == descriptors().end()) return uint64_t(-9);
        if (directory->second.node != Node::directory) return uint64_t(-20);
        path = normalizePath(directory->second.path + "/" + *input);
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
    if (sourceMount->device == "bpf") {
        source = backingPath(source);
        destination = backingPath(destination);
    } else if (backingPath(source) != source || backingPath(destination) != destination) {
        return uint64_t(-18);
    }
    if (!writablePath(source) || !writablePath(destination)) return uint64_t(-30);
    auto file = writableFiles.find(source);
    auto link = links.find(source);
    const bool directory = directories.contains(source);
    if (!directory && file == writableFiles.end() && link == links.end()) return uint64_t(-2);
    if (source == destination) return flags ? uint64_t(-17) : 0;
    if (!directoryExists(destination.substr(0, destination.rfind('/')))) return uint64_t(-2);
    if (!canAccessParent(source, true) || !canAccessParent(destination, true)) return uint64_t(-13);
    if (directory) {
        const auto result = renameDirectory(source, destination, flags != 0);
        if (!result) fileMoved(source, destination, true);
        return result;
    }
    if (directories.contains(destination)) return uint64_t(-21);
    if (const auto existing = find(destination, 0); existing && !existing->temporary) return uint64_t(-30);
    if (flags && (writableFiles.contains(destination) || links.contains(destination))) return uint64_t(-17);
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
    if (entry && entry->temporary && userId() != 0 && !(flags & pathOnly)) {
        const auto& file = *entry->temporary;
        const unsigned requested = (flags & 3) == 2 ? 6 : ((flags & 3) || (flags & truncate)) ? 2 : 4;
        if (((file.mode >> permissionShift(file.uid, file.gid)) & requested) != requested) return uint64_t(-13);
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
        if (writableFiles.size() >= maxWritableFiles &&
            size_t(std::count_if(writableFiles.begin(), writableFiles.end(), [persistent](const auto& entry) {
                return entry.second->persistent == persistent;
            })) >= maxWritableFiles) return uint64_t(-28);
        auto file = std::make_shared<WritableFile>();
        file->inode = nextInode++;
        file->mode = mode & 0777 & ~current->process->umask;
        file->uid = userId();
        file->gid = groupId();
        file->persistent = persistent;
        if (const auto& context = current->fsCreateContext; !context.empty())
            file->attributes["security.selinux"].assign(context.c_str(), context.c_str() + context.size() + 1);
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
        entry->binder = std::make_shared<BinderConnection>(processId());
    }
    if (!entry->pathOnly() && (entry->node == Node::ptyMaster || entry->node == Node::ptySlave)) {
        if (const auto error = openPty(*entry); error) return error;
    }
    if (!entry->pathOnly() && entry->node == Node::ashmem) {
        entry->ashmem = std::make_shared<Ashmem>();
        entry->ashmem->file.memoryFile = true;
    }
    if ((flags & truncate) && entry->temporary) resize(*entry->temporary, 0);
    if (entry->node == Node::directory && pathname.find('/', 2) == std::string::npos) {
        // Each BPF mount owns a synthetic backing root. A bind mount can expose a subdirectory.
        const auto* mount = mountFor(*logical);
        entry->bpfFilesystemRoot = mount && mount->device == "bpf" && mount->backing == pathname;
    }
    if (entry->path != *logical) entry->visiblePath = *logical;
    if (!entry->pathOnly() && (entry->node == Node::regular || entry->temporary || entry->node == Node::directory))
        fileEvent(entry->path, fileOpened, entry->node == Node::directory);
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
            const bool wakeAlarm = x[0] == 9; // CLOCK_BOOTTIME_ALARM
            if ((x[0] != 1 && x[0] != 7 && !wakeAlarm) || (x[1] & ~(cloexec | nonblock))) {
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
            entry.event->timer.emplace().wakeAlarm = wakeAlarm;
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
                if (x[1] & ~uint64_t{1}) { x[0] = invalid; break; }
                const auto* input = machine.read(x[2], sizeof previous, 4);
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
                replacement.interval = *interval;
                const auto epoch = x[1] ? steady_clock::time_point{} : now;
                if (value->count()) {
                    replacement.next = *value > steady_clock::time_point::max() - epoch ?
                        steady_clock::time_point::max() : epoch + *value;
                }
            }
            const auto remaining = timer.next ? duration_cast<nanoseconds>(*timer.next - now) : nanoseconds{};
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
                !target->second.pipe && !target->second.touch && (!target->second.event || target->second.node == Node::poller)) {
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
            const auto* data = machine.read(x[3], sizeof(event), 4);
            if (!data) { x[0] = uint64_t(-14); break; }
            std::memcpy(event.data(), data, sizeof(event));
            const auto mask = uint32_t(event[0]);
            // EPOLLWAKEUP requires no power-management action while the guest is scheduled.
            if (mask & ~0x2000205fU) return false;
            watches.insert_or_assign(fd, Watch{mask, event[1], target->second.event, target->second.socket,
                                              target->second.node == Node::event, target->second.binder, target->second.graphics,
                                              target->second.pipe, target->second.pipeEnd, target->second.node == Node::pipeRead,
                                              target->second.touch, target->second.touchReader, ring});
            x[0] = 0;
            break;
        }
        case 22: { // epoll_pwait with level-triggered, immediately ready guest events.
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (descriptor->second.node != Node::poller || !x[2] || x[2] > INT32_MAX) {
                x[0] = invalid;
                break;
            }
            if (x[4]) return false; // Temporary signal-mask swaps need a scheduler.
            if (!timers.empty()) refreshTimers(std::chrono::steady_clock::now());
            std::vector<std::array<uint64_t, 2>> ready;
            std::vector<std::shared_ptr<EventState>> readNotifications;
            bool watchesHostEvents = false;
            for (const auto& [fd, watch] : descriptor->second.event->watches) {
                const auto state = watch.source.lock();
                const auto endpoint = watch.socket.lock();
                const auto peer = endpoint ? endpoint->peer.lock() : nullptr;
                const bool childReady = state && (state->signalMask & sigChildMask) && current->process->childSignal;
                const auto binder = watch.binder.lock();
                const auto graphics = watch.graphics.lock();
                const auto touch = watch.touchReader.expired() ? nullptr : watch.touch.lock();
                watchesHostEvents |= bool(graphics) || bool(touch);
                const auto graphicsEvents = graphics ? graphics->pollEvents() : 0;
                const auto pipe = watch.pipeEnd.expired() ? nullptr : watch.pipe.lock();
                const auto pipeEvents = pipe ? pipe->pollEvents(watch.pipeReader) : 0;
                const auto ring = watch.ring.lock();
                const auto ringEvents = ring && ring->ringReadable() ? 0x41 : 0;
                const bool receiveEnded = endpoint && (endpoint->receiveShutdown ||
                    (!endpoint->inetDatagram() && endpoint->connected && (!peer || peer->sendShutdown)));
                const bool readable = (state && state->counter) || childReady || (binder && binderReadable(*binder)) ||
                    (touch && touch->ready()) ||
                    (endpoint && (endpoint->hasInput() || !endpoint->pending.empty() || (peer && receiveEnded)));
                const bool writable = (state && watch.eventCounter && state->counter < UINT64_MAX - 1) ||
                    (endpoint && endpoint->inetDatagram() && !endpoint->sendShutdown) ||
                    (peer && !endpoint->sendShutdown && !peer->receiveShutdown &&
                     peer->queuedBytes < endpoint->sendBuffer && peer->inbox.size() < 256);
                const bool hungUp = endpoint && !endpoint->inetDatagram() && endpoint->connected &&
                    (!peer || (receiveEnded && endpoint->sendShutdown));
                const bool mountsChanged = state && state->mountNamespace &&
                    state->observedMountGeneration != state->mountNamespace->generation;
                const uint64_t mask = ((graphicsEvents | pipeEvents | ringEvents) & (watch.mask | 0x18U)) | (watch.mask & ((readable ? 1U : 0) | (mountsChanged ? 2U : 0) |
                                                        (writable ? 4U : 0) | (receiveEnded ? 0x2000U : 0))) | (hungUp ? 16U : 0);
                if (mask) {
                    ready.push_back({mask, watch.data});
                    if (mask & 2) readNotifications.push_back(state);
                    if (ready.size() == x[2]) break;
                }
            }
            if (ready.empty()) {
                const auto wait = [&] { return watchesHostEvents ? waitForHostEvents() : waitForEvents(); };
                if (uint32_t(x[3]) == UINT32_MAX) {
                    if (currentTid != pid) {
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
            for (const auto& state : readNotifications) state->observedMountGeneration = state->mountNamespace->generation;
            x[0] = ready.size();
            break;
        }
        default: return false;
    }
    return true;
}

uint64_t ProbeLinux::socketRead(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size) {
    auto& endpoint = *fd.socket;
    if (!size) return 0;
    if (endpoint.inetDatagram()) return receiveInetDatagram(machine, endpoint, address, size);
    if (endpoint.inbox.empty()) {
        const auto peer = endpoint.peer.lock();
        if (endpoint.receiveShutdown || (endpoint.connected && (!peer || peer->sendShutdown))) return 0;
        return uint64_t(-11);
    }
    if (endpoint.type == 1) {
        const auto count = std::min<uint64_t>(size, endpoint.queuedBytes);
        std::vector<uint8_t> bytes;
        bytes.reserve(count);
        for (const auto& packet : endpoint.inbox) {
            const auto chunk = std::min<size_t>(packet.size(), count - bytes.size());
            bytes.insert(bytes.end(), packet.begin(), packet.begin() + chunk);
            if (bytes.size() == count) break;
        }
        if (count && !machine.write(address, bytes)) return uint64_t(-14);
        size_t consumed = count;
        while (consumed) {
            auto& packet = endpoint.inbox.front();
            if (packet.size() <= consumed) {
                consumed -= packet.size();
                endpoint.inbox.pop_front();
            } else {
                packet.erase(packet.begin(), packet.begin() + consumed);
                consumed = 0;
            }
        }
        endpoint.queuedBytes -= count;
        wakePollers();
        return count;
    }
    auto& packet = endpoint.inbox.front();
    const auto count = std::min<uint64_t>(size, packet.size());
    if (count && !machine.write(address, std::span(packet).first(count))) return uint64_t(-14);
    endpoint.queuedBytes -= packet.size();
    endpoint.inbox.pop_front();
    wakePollers();
    return count;
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
        if (memoryFiles.size() >= descriptorCapacity) return {};
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
            if (device.claims.size() >= descriptorCapacity) return uint64_t(-28);
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
            bits[GuestTouch::touchKey / 8] = 1 << (GuestTouch::touchKey % 8);
            break;
        case 0x23: bits[0] = 3; break; // ABS_X, ABS_Y
        default: break;
    }
    const auto count = std::min<size_t>(size, bits.size());
    return machine.write(x[2], std::span(bits).first(count)) ? count : uint64_t(-14);
}

uint16_t ProbeLinux::GraphicsPipeState::pollEvents() const {
    constexpr uint16_t readable = 1, writable = 4, hangup = 16;
    if (failed) return hangup;
    if (!backend) return writable;
    return backend->pollEvents() | (unread.empty() ? 0 : readable);
}

uint64_t ProbeLinux::readGraphics(Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size) {
    if (!size) return 0;
    if (pipe.failed) return uint64_t(-5);
    if (!pipe.backend) return uint64_t(-11);
    if (pipe.unread.empty()) {
        auto& buffer = pipe.unread;
        buffer.resize(std::min<uint64_t>(size, maxGraphicsTransfer));
        const auto received = pipe.backend->read(buffer);
        if (received <= 0 || uint64_t(received) > buffer.size()) {
            buffer.clear();
            return received <= 0 ? uint64_t(received) : uint64_t(-5);
        }
        buffer.resize(size_t(received));
    }
    const auto bytes = std::span(pipe.unread).subspan(pipe.offset);
    const auto count = std::min<uint64_t>(size, bytes.size());
    if (!machine.write(address, bytes.first(count))) return uint64_t(-14);
    pipe.offset += count;
    if (pipe.offset == pipe.unread.size()) {
        pipe.unread.clear();
        pipe.offset = 0;
    }
    return count;
}

uint64_t ProbeLinux::writeGraphics(const Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size) {
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
        if (delimiter != bytes.end() && (service == "pipe:opengles" || service == "pipe:GLProcessPipe" || service == "pipe:refcount"))
            pipe.backend = graphicsPipeFactory(service.substr(5));
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

uint64_t ProbeLinux::socketWrite(Machine& machine, const Descriptor& fd, std::span<const SocketVector> vectors) {
    if (fd.socket->inetDatagram()) return sendInetDatagram(machine, fd.socket, vectors);
    if (fd.socket->inetStream() && !inetLoopbackUp) return uint64_t(-100);
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
    if (peer->queuedBytes >= fd.socket->sendBuffer || peer->inbox.size() >= 256) return uint64_t(-11);
    if (size > fd.socket->sendBuffer - peer->queuedBytes) {
        if (!stream) return uint64_t(-11);
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
            if (!transmitInetStream(*fd.socket, *peer, packet, 0x18)) return uint64_t(-110); // PSH|ACK
        } catch (const std::bad_alloc&) {
            return noMemory;
        }
    }
    peer->inbox.push_back(std::move(packet));
    peer->queuedBytes += size;
    wakePollers();
    return size;
}

uint64_t ProbeLinux::recvUevent(Machine& machine, SocketState& endpoint) {
    const auto& x = machine.cpu.x;
    if (x[2] & ~0x40ULL) return invalid; // MSG_DONTWAIT
    if (endpoint.inbox.empty()) return uint64_t(-11);
    SocketMessage message{};
    const auto* header = machine.read(x[1], sizeof(message), 4);
    if (!header) return uint64_t(-14);
    std::memcpy(&message, header, sizeof(message));
    std::array<uint64_t, 2> vector{};
    if (message.vectorCount != 1) return invalid;
    const auto* iov = machine.read(message.vector, sizeof(vector), 4);
    if (!iov) return uint64_t(-14);
    std::memcpy(vector.data(), iov, sizeof(vector));
    const auto& packet = endpoint.inbox.front();
    const auto copied = std::min<uint64_t>(packet.size(), vector[1]);
    if (copied && !machine.write(vector[0], std::span(packet).first(copied))) return uint64_t(-14);
    message.flags = copied < packet.size() ? 0x20 : 0; // MSG_TRUNC
    if (message.name) {
        const std::array<uint32_t, 3> address{16, 0, 1}; // AF_NETLINK, kernel port, multicast group
        const auto size = std::min<size_t>(sizeof(address), message.nameLength);
        if (size && !machine.write(message.name, {reinterpret_cast<const uint8_t*>(address.data()), size}))
            return uint64_t(-14);
        message.nameLength = sizeof(address);
    } else {
        message.nameLength = 0;
    }
    const std::array<uint64_t, 4> credentials{28, (uint64_t{2} << 32) | 1, 0, 0}; // SCM_CREDENTIALS
    if (endpoint.passCredentials && message.control && message.controlLength >= sizeof(credentials)) {
        if (!writeGuest(machine, message.control, credentials)) return uint64_t(-14);
        message.controlLength = sizeof(credentials);
    } else {
        if (endpoint.passCredentials) message.flags |= 0x8; // MSG_CTRUNC
        message.controlLength = 0;
    }
    if (!writeGuest(machine, x[1], message)) return uint64_t(-14);
    endpoint.queuedBytes -= packet.size();
    endpoint.inbox.pop_front();
    return copied;
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
        if (size && endpoint->queuedBytes + size <= endpoint->uevent->receiveBuffer) {
            endpoint->inbox.emplace_back(packet.begin(), packet.begin() + size);
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
        const auto* len = machine.read(x[2], sizeof(space), 4);
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
        return writeAddress(std::span(address).first(name.empty() ? 2 : 3 + name.size()));
    };
    switch (x[8]) {
        case 198: case 199: { // socket, socketpair
            const bool uevent = x[8] == 198 && x[0] == 16 && x[2] == 15; // NETLINK_KOBJECT_UEVENT
            const bool inet = x[8] == 198 && (x[0] == 2 || x[0] == 10);
            if (x[0] != 1 && !uevent && !inet) { x[0] = uint64_t(-97); break; }
            const uint32_t type = x[1] & 15;
            constexpr uint32_t flags = 0x80000 | 0x800;
            const bool firewallSocket = inet && type == 3 && x[2] == 255;
            if (inet && !firewallSocket && (type != 1 && type != 2)) { x[0] = uint64_t(-94); break; }
            if (firewallSocket && !(current->process->effectiveCaps & (1ULL << 13))) {
                x[0] = uint64_t(-1); break; // CAP_NET_RAW
            }
            const uint32_t inetProtocol = firewallSocket ? 255 : type == 1 ? 6 : 17;
            if (inet && x[2] && x[2] != inetProtocol) { x[0] = uint64_t(-93); break; }
            if (x[1] & ~(uint64_t(flags) | 15) ||
                (!inet && (uevent ? type != 2 : (type != 1 && type != 2 && type != 5) || x[2]))) {
                x[0] = uint64_t(-22);
                break;
            }
            const auto first = freeDescriptor();
            if (first == descriptorCapacity) { x[0] = uint64_t(-24); break; }
            auto endpoint = std::make_shared<SocketState>(type);
            endpoint->context = current->socketCreateContext.empty()
                ? current->process->context : current->socketCreateContext;
            endpoint->credentials = {uint32_t(processId()), userId(), groupId()};
            Descriptor entry{inet ? "socket:[inet]" : uevent ? "socket:[uevent]" : "socket:[unix]",
                             {}, uint32_t(x[1] & flags) | 2, Node::socket};
            if (uevent) endpoint->uevent.emplace();
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
            if (!fd->second.socket) { x[0] = uint64_t(-88); break; }
            if (fd->second.socket->firewallSocket()) { x[0] = uint64_t(-95); break; }
            if (fd->second.socket->family == 2 || fd->second.socket->family == 10) {
                const auto status = x[8] == 200 ? bindInetSocket(machine, fd->second.socket) :
                    connectInetSocket(machine, fd->second.socket);
                if (status == uint64_t(-11) && !(fd->second.effectiveFlags() & 0x800) &&
                    waitForSocketTimeout(fd->second.socket->sendTimeout)) return true;
                current->deadline.reset();
                x[0] = status;
                break;
            }
            if (auto& uevent = fd->second.socket->uevent; uevent) {
                std::array<uint32_t, 3> address{};
                if (x[8] != 200 || x[2] != sizeof(address)) { x[0] = invalid; break; }
                const auto* data = machine.read(x[1], sizeof(address), 4);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(address.data(), data, sizeof(address));
                if (address[0] != 16 || uevent->pid) { x[0] = invalid; break; }
                if (address[2] && userId() != 0) { x[0] = uint64_t(-1); break; }
                uevent->pid = address[1] ? address[1] : uint32_t(processId());
                uevent->groups = address[2];
                if (uevent->groups & 1) ueventListeners.push_back(fd->second.socket);
                x[0] = 0;
                break;
            }
            if (x[2] < 3 || x[2] > 110) { x[0] = invalid; break; }
            const auto* bytes = machine.read(x[1], x[2], 4);
            if (!bytes) { x[0] = uint64_t(-14); break; }
            if (bytes[0] != 1 || bytes[1]) { x[0] = uint64_t(-97); break; }
            if (!bytes[2]) return false; // Abstract Unix socket namespace.
            const auto* nul = static_cast<const uint8_t*>(std::memchr(bytes + 2, 0, x[2] - 2));
            const size_t length = nul ? nul - bytes - 2 : x[2] - 2;
            const std::string_view name(reinterpret_cast<const char*>(bytes + 2), length);
            const auto path = normalizePath(name);
            if (x[8] == 200) {
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
                const auto bound = socketPaths.find(path);
                if (bound == socketPaths.end()) { x[0] = uint64_t(-2); break; }
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
            if (endpoint.uevent) {
                const std::array<uint32_t, 3> address{16, endpoint.uevent->pid, endpoint.uevent->groups};
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
            if (x[4] || x[5] || (x[3] & ~(send ? 0x4040ULL : 0x140ULL))) return false;
            const auto& endpoint = *fd->second.socket;
            const auto peer = endpoint.peer.lock();
            const bool blocking = !(x[3] & 0x40) && !(fd->second.effectiveFlags() & 0x800);
            const bool waitAll = !send && (x[3] & 0x100) && blocking &&
                endpoint.type == 1 && x[2] > endpoint.queuedBytes && peer &&
                !peer->sendShutdown && !endpoint.receiveShutdown;
            if (waitAll && waitForSocketTimeout(endpoint.receiveTimeout)) return true;
            const auto result = send ? socketWrite(machine, fd->second, x[1], x[2])
                                     : socketRead(machine, fd->second, x[1], x[2]);
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
            wakePollers();
            x[0] = 0;
            break;
        }
        case 211: { // sendmsg on a connected UNIX socket.
            if (fd == descriptors().end()) { x[0] = uint64_t(-9); break; }
            const auto& endpoint = fd->second.socket;
            if (!endpoint) { x[0] = uint64_t(-88); break; }
            if (endpoint->uevent || (x[2] & ~0x40c0ULL)) return false; // DONTWAIT, EOR, NOSIGNAL
            SocketMessage message{};
            std::vector<SocketVector> vectors;
            if (const auto error = readSocketMessage(machine, x[1], message, vectors)) {
                x[0] = error;
                break;
            }
            if (message.controlLength || (message.nameLength && !endpoint->inetDatagram())) return false;
            const auto result = endpoint->inetDatagram() ?
                sendInetDatagram(machine, endpoint, vectors, message.name, message.nameLength) :
                socketWrite(machine, fd->second, vectors);
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
            if (endpoint.uevent) {
                if (endpoint.inbox.empty() && !(fd->second.effectiveFlags() & 0x800) && !(x[2] & 0x40) &&
                    waitForSocketTimeout(endpoint.receiveTimeout)) return true;
                current->deadline.reset();
                x[0] = recvUevent(machine, endpoint);
                break;
            }
            constexpr uint64_t peek = 0x2, trunc = 0x20, dontWait = 0x40, cloexec = 0x40000000;
            if (endpoint.type == 1 || endpoint.passCredentials || (x[2] & ~(peek | trunc | dontWait | cloexec)))
                return false;
            SocketMessage message{};
            std::vector<SocketVector> vectors;
            if (const auto error = readSocketMessage(machine, x[1], message, vectors)) {
                x[0] = error;
                break;
            }
            if (!endpoint.hasInput()) {
                const auto peer = endpoint.peer.lock();
                if (endpoint.receiveShutdown || (endpoint.type == 5 && endpoint.connected &&
                    (!peer || peer->sendShutdown))) {
                    x[0] = 0;
                } else {
                    if (!(fd->second.effectiveFlags() & 0x800) && !(x[2] & dontWait) &&
                        waitForSocketTimeout(endpoint.receiveTimeout)) return true;
                    x[0] = uint64_t(-11);
                }
                current->deadline.reset();
                break;
            }
            current->deadline.reset();
            const auto& packet = endpoint.inetDatagram() ? endpoint.datagrams.front().payload : endpoint.inbox.front();
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
            message.flags = remaining.empty() ? 0 : trunc;
            message.controlLength = 0;
            if (!copied || !writeGuest(machine, x[1], message)) { x[0] = uint64_t(-14); break; }
            x[0] = (x[2] & trunc) ? packet.size() : packet.size() - remaining.size();
            if (!(x[2] & peek)) {
                endpoint.queuedBytes -= packet.size();
                if (endpoint.inetDatagram()) endpoint.datagrams.pop_front();
                else endpoint.inbox.pop_front();
                wakePollers();
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
            const auto* pointer = machine.read(x[4], sizeof(size), 4);
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
            if (x[2] == 2 || x[2] == 3 || x[2] == 7 || x[2] == 8 || x[2] == 38 || x[2] == 39) {
                uint32_t value = 0;
                switch (x[2]) {
                    case 2: value = endpoint.reuseAddress; break; // SO_REUSEADDR
                    case 3: value = endpoint.type; break; // SO_TYPE
                    case 7: value = endpoint.sendBuffer; break; // SO_SNDBUF
                    case 8: value = endpoint.uevent ? endpoint.uevent->receiveBuffer : endpoint.receiveBuffer; break;
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
                    x[0] = firewall.set(machine, endpoint.family == 10);
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
            if (level != 1) {
                if (!endpoint.storageIdentity) return false;
                x[0] = uint64_t(-92);
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
            if (x[2] == 50 && endpoint.uevent) { // SO_ATTACH_BPF
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
            if (name != 2 && name != 16 && name != 7 && name != 8 && (name != 33 || !endpoint.uevent)) {
                if (!endpoint.storageIdentity) return false;
                x[0] = uint64_t(-92);
                break;
            }
            if (size < 4) { x[0] = invalid; break; }
            uint32_t enabled = 0;
            if (!readOption({reinterpret_cast<uint8_t*>(&enabled), sizeof(enabled)})) { x[0] = uint64_t(-14); break; }
            if (name == 2) {
                endpoint.reuseAddress = enabled != 0;
            } else if (name == 16) {
                endpoint.passCredentials = enabled != 0;
            } else if (name == 7) {
                endpoint.sendBuffer = 2 * std::clamp<uint32_t>(enabled, 1024, 131072);
                wakePollers();
            } else if (endpoint.uevent) {
                if (x[2] == 33 && userId() != 0) { x[0] = uint64_t(-1); break; }
                endpoint.uevent->receiveBuffer = 2 * std::clamp<uint32_t>(enabled, 1024, 4 * 1024 * 1024);
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
    add(loopDevices, 6);
    add(binderDevices, 2);
    add(ueventTriggers, 8);
    add(kernelSettings, 8);
    add(cgroupFiles, 8);
    if (fd.path == "/proc/self/task") {
        for (const auto& [tid, task] : threads) {
            if (task.process == current->process && task.wait != Wait::exited)
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
            if (path->starts_with('/') || uint32_t(x[1]) == uint32_t(-100)) logical = normalizePath(*path);
            else if (auto directory = descriptors().find(uint32_t(x[1])); directory != descriptors().end()) {
                if (directory->second.node != Node::directory) { x[0] = uint64_t(-20); break; }
                logical = normalizePath(directory->second.path + "/" + *path);
            } else {
                x[0] = uint64_t(-9);
                break;
            }
            const auto backing = backingPath(logical);
            if (!directoryExists(backing.substr(0, backing.rfind('/')))) x[0] = uint64_t(-2);
            else if (links.contains(backing) || find(backing, 0)) x[0] = uint64_t(-17);
            else if (!writablePath(logical)) x[0] = uint64_t(-30);
            else if (userId() != 0) x[0] = uint64_t(-13);
            else if (links.size() - ext4LinkCount >= 512) x[0] = uint64_t(-28);
            else { links.emplace(backing, *target); x[0] = 0; }
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
            x[0] = !persistent || syncData() ? 0 : uint64_t(-5);
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
            if (rawPath->starts_with('/')) path = normalizePath(*rawPath);
            else if (uint32_t(x[0]) == uint32_t(-100)) path = normalizePath("/" + *rawPath);
            else if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            else if (descriptor->second.node != Node::directory) { x[0] = uint64_t(-20); break; }
            else path = normalizePath(descriptor->second.path + "/" + *rawPath);
            if (x[8] == 56) {
                const auto ownProc = "/proc/" + std::to_string(processId());
                const bool procMaps = path == "/proc/self/maps" || path == ownProc + "/maps";
                uint64_t statTid = 0;
                if (path == "/proc/self/stat" || path == ownProc + "/stat") statTid = processId();
                else if (path == "/proc/thread-self/stat") statTid = currentTid;
                else if (path.ends_with("/stat")) {
                    std::string_view task = path;
                    const auto ownTasks = ownProc + "/task/";
                    if (task.starts_with("/proc/self/task/")) task.remove_prefix(sizeof("/proc/self/task/") - 1);
                    else if (task.starts_with(ownTasks)) task.remove_prefix(ownTasks.size());
                    else task = {};
                    if (task.size() > 5) {
                        task.remove_suffix(5);
                        const auto [end, error] = std::from_chars(task.begin(), task.end(), statTid);
                        if (error != std::errc{} || end != task.end()) statTid = 0;
                    }
                }
                if (procMaps || statTid) {
                    if ((x[2] & 3) || (x[2] & 0x240)) { x[0] = uint64_t(-13); break; }
                    const auto fd = freeDescriptor();
                    if (fd == descriptorCapacity) { x[0] = uint64_t(-24); break; }
                    std::ostringstream listing;
                    if (statTid) {
                        const auto& process = *current->process;
                        const auto subject = threads.find(statTid);
                        if (subject == threads.end() || subject->second.process.get() != &process ||
                            subject->second.wait == Wait::exited) { x[0] = uint64_t(-2); break; }
                        const auto& thread = subject->second;
                        // mprotect can split the stack. The exec stack pointer stays near its high end.
                        const auto topStack = std::find_if(machine.segments.rbegin(), machine.segments.rend(),
                            [](const Segment& area) { return area.initialStack; });
                        const uint64_t stackStart = topStack == machine.segments.rend() ? 0 :
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
            auto target = resolve(path);
            if (x[8] == 78 && path == "/proc/self/ns/mnt")
                target = "mnt:[" + std::to_string(current->mountNamespace->id) + "]";
            const auto backing = backingPath(path);
            auto entry = find(x[8] != 35 && target ? backingPath(*target) : backing, 0);
            if (!entry && (x[8] == 48 || x[8] == 79)) {
                const auto resolved = followLinks(path, x[8] != 79 || !(x[3] & 0x100));
                if (!resolved) { x[0] = uint64_t(-40); break; }
                if (*resolved != path) entry = find(backingPath(*resolved), 0);
            }
            if (entry && entry->path != path) entry->visiblePath = path;
            if (x[8] == 33) { // mknodat against the guest's registered loop devices.
                if (userId() != 0) { x[0] = uint64_t(-1); break; }
                if (entry || target) { x[0] = uint64_t(-17); break; }
                if (!directoryExists(backing.substr(0, backing.rfind('/')))) { x[0] = uint64_t(-2); break; }
                x[0] = writablePath(path) ? uint64_t(-95) : uint64_t(-30);
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
                        hasChild(links) || hasChild(socketPaths)) {
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
                    if (!x[0]) fileEvent(backing, fileDeleted, true);
                } else if ((links.contains(backing) || (entry && entry->temporary)) && writablePath(path)) {
                    if (!canAccessParent(backing, true)) { x[0] = uint64_t(-13); break; }
                    writableFiles.erase(backing);
                    links.erase(backing);
                    fileEvent(backing, fileDeleted);
                    x[0] = 0;
                } else if (entry && entry->node == Node::directory) x[0] = uint64_t(-21);
                else if (entry && entry->node == Node::cgroupProcs) x[0] = uint64_t(-1);
                else if (!entry && !links.contains(backing)) x[0] = uint64_t(-2);
                else if (socketPaths.contains(path) && userId() == 0) { socketPaths.erase(path); x[0] = 0; }
                else if (!writablePath(path)) x[0] = uint64_t(-30);
                else return false;
            } else if (x[8] == 53 || x[8] == 54) { // fchmodat, fchownat
                if (!entry) { x[0] = uint64_t(-2); break; }
                const auto changeMetadata = [&](auto& node) {
                    if (userId() != 0 && (x[8] != 53 || userId() != node.uid)) return uint64_t(-1);
                    if (x[8] == 53) node.mode = x[2] & 0777;
                    else {
                        if (uint32_t(x[2]) != UINT32_MAX) node.uid = uint32_t(x[2]);
                        if (uint32_t(x[3]) != UINT32_MAX) node.gid = uint32_t(x[3]);
                    }
                    return uint64_t(0);
                };
                if (entry->temporary) x[0] = changeMetadata(*entry->temporary);
                else if (entry->cgroupFile) x[0] = changeMetadata(*entry->cgroupFile);
                else if (auto socket = socketPaths.find(entry->path); socket != socketPaths.end())
                    x[0] = changeMetadata(socket->second);
                else if (!writablePath(target ? *target : path)) x[0] = uint64_t(-30);
                else if (auto directory = directories.find(entry->path); directory != directories.end())
                    x[0] = changeMetadata(directory->second);
                else if (auto block = loopDevices.find(entry->path); block != loopDevices.end())
                    x[0] = changeMetadata(block->second);
                else if (auto binder = binderDevices.find(entry->path); binder != binderDevices.end())
                    x[0] = changeMetadata(binder->second);
                else return false;
                if (!x[0]) fileEvent(entry->path, fileAttributes, entry->node == Node::directory);
            } else if (x[8] == 34) {
                const auto parent = backing.substr(0, backing.rfind('/'));
                if (entry) x[0] = uint64_t(-17);
                else if (!directoryExists(parent)) x[0] = uint64_t(-2);
                else if (!writablePath(path)) x[0] = uint64_t(-30);
                else if (!canAccessParent(backing, true)) x[0] = uint64_t(-13);
                else if (directories.size() - ext4DirectoryCount >= maxGuestDirectories) x[0] = uint64_t(-28);
                else {
                    directories.emplace(backing, Directory{uint32_t(x[2] & 0777 & ~current->process->umask),
                                                           userId(), groupId(), current->fsCreateContext});
                    if (const auto* view = mountFor(path); view && view->device == "cgroup2") {
                        cgroupFiles.emplace(backing + std::string(cgroupProcs),
                                            std::make_shared<Directory>(Directory{0644, userId(), groupId()}));
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
            } else if (x[8] == 48) {
                x[0] = x[2] > 7 ? invalid : (x[2] & 2) && entry->node == Node::regular ? uint64_t(-13) : 0;
            } else {
                if (x[3] && x[3] != 0x100) return false;
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
            descriptors().erase(descriptor);
            wakePollers();
            x[0] = 0;
            break;
        case 62: { // lseek
            if (descriptor == descriptors().end() || descriptor->second.pathOnly()) {
                x[0] = uint64_t(-9); break;
            }
            auto& fd = descriptor->second;
            if (fd.node != Node::regular && fd.node != Node::temporary && fd.node != Node::directory &&
                fd.node != Node::kernelSetting) {
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
            const auto target = followLinks(normalizePath(*source));
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
            const auto& file = *output.temporary;
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
                if (result == uint64_t(-11) && !(fd.effectiveFlags() & 0x800)) return waitForHostEvents();
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
            if (fd.securityReply || fd.node == Node::kernelSetting) {
                const auto offset = x[8] == 67 ? x[3] : *fd.offset;
                const auto setting = fd.securityReply ? std::string{} : std::to_string(kernelSettings.at(fd.path).value) + "\n";
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
                if (!fd.event->counter) {
                    if (!(fd.effectiveFlags() & 0x800)) return waitForEvents();
                    x[0] = uint64_t(-11);
                    break;
                }
                if (!machine.store64(x[1], fd.event->counter)) { x[0] = uint64_t(-14); break; }
                fd.event->counter = 0;
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
            if (!x[0]) fileEvent(descriptor->second.path, fileModified);
            break;
        }
        case 52: case 55: { // fchmod, fchown
            if (descriptor == descriptors().end() || descriptor->second.pathOnly()) { x[0] = uint64_t(-9); break; }
            const auto& file = descriptor->second.temporary;
            if (!file) { x[0] = uint64_t(-30); break; }
            if (userId() != 0 && userId() != file->uid) { x[0] = uint64_t(-1); break; }
            if (x[8] == 52) file->mode = x[1] & 0777;
            else {
                if (userId() != 0) { x[0] = uint64_t(-1); break; }
                if (uint32_t(x[1]) != UINT32_MAX) file->uid = uint32_t(x[1]);
                if (uint32_t(x[2]) != UINT32_MAX) file->gid = uint32_t(x[2]);
            }
            x[0] = 0;
            fileEvent(descriptor->second.path, fileAttributes);
            break;
        }
        case 5: case 6: case 7: { // setxattr, lsetxattr, fsetxattr on guest-writable nodes.
            std::optional<Descriptor> pathEntry;
            const Descriptor* entry = nullptr;
            if (x[8] == 7) {
                if (descriptor == descriptors().end() || descriptor->second.pathOnly()) {
                    x[0] = uint64_t(-9);
                    break;
                }
                entry = &descriptor->second;
            } else {
                const auto input = readPath(machine, x[0]);
                if (!input) { x[0] = uint64_t(-14); break; }
                if (input->empty()) { x[0] = uint64_t(-2); break; }
                const auto logical = followLinks(normalizePath(*input), x[8] == 5);
                if (!logical) { x[0] = uint64_t(-40); break; }
                pathEntry = find(backingPath(*logical), 0);
                if (!pathEntry) { x[0] = uint64_t(-2); break; }
                pathEntry->visiblePath = *logical;
                entry = &*pathEntry;
            }
            const auto name = readPath(machine, x[1]);
            if (!name) { x[0] = uint64_t(-14); break; }
            if (*name != "security.selinux") { x[0] = uint64_t(-95); break; }
            if (x[3] > 4096 || x[4] > 2) { x[0] = invalid; break; }
            if (userId() != 0) { x[0] = uint64_t(-1); break; }
            const auto* bytes = x[3] ? machine.read(x[2], x[3], 4) : nullptr;
            if (x[3] && !bytes) { x[0] = uint64_t(-14); break; }
            if (entry->temporary) {
                auto& attributes = entry->temporary->attributes;
                const bool present = attributes.contains(*name);
                if (x[4] == 1 && present) { x[0] = uint64_t(-17); break; }
                if (x[4] == 2 && !present) { x[0] = uint64_t(-61); break; }
                auto& value = attributes[*name];
                if (x[3]) value.assign(bytes, bytes + x[3]);
                else value.clear();
            } else if (entry->node == Node::directory) {
                const auto& visible = entry->visiblePath.empty() ? entry->path : entry->visiblePath;
                if (!writablePath(visible)) { x[0] = uint64_t(-30); break; }
                const auto directory = directories.find(entry->path);
                if (directory == directories.end()) { x[0] = uint64_t(-95); break; }
                auto& context = directory->second.securityContext;
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
        case 80: // fstat
            x[0] = descriptor == descriptors().end() ? uint64_t(-9) : stat(machine, x[1], descriptor->second);
            break;
        case 43: { // statfs by guest pathname
            const auto path = readPath(machine, x[0]);
            if (!path) { x[0] = uint64_t(-14); break; }
            const auto target = resolve(*path);
            x[0] = find(backingPath(target ? *target : *path), 0) ? statFilesystem(machine, x[1], target ? *target : *path) : uint64_t(-2);
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
    return mappingFloor + pages(entropy) * pageSize;
}

uint64_t ProbeLinux::map(Machine& machine) {
    const auto& x = machine.cpu.x;
    constexpr uint64_t fixed = 0x10, anonymous = 0x20, noReserve = 0x4000;
    constexpr uint64_t fixedNoReplace = 0x100000;
    const bool atAddress = x[3] & (fixed | fixedNoReplace), anon = x[3] & anonymous;
    const auto sharing = x[3] & 3;
    if (!x[1] || x[1] > maxReservation || x[2] > 7 ||
        (x[3] & ~(fixed | fixedNoReplace | anonymous | noReserve | 3ULL)) || !sharing || sharing == 3 ||
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
    const auto start = atAddress ? x[0] : lowHint ? hint : nextMapping;
    if (start < lowMappingFloor || start > mappingLimit - length) return noMemory;
    const auto end = start + length;
    if ((x[3] & fixedNoReplace) && overlaps(start, false)) return uint64_t(-17);
    if (start < mappingFloor && (end > mappingFloor || (atAddress && overlaps(start, true))))
        return noMemory;
    if (atAddress && !(x[3] & fixedNoReplace) && start >= mappingFloor && end > nextMapping) return noMemory;
    std::span<const uint8_t> bytes;
    uint64_t backedLength = length;
    uint64_t imageLength = 0;
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
                if (memoryFiles.size() >= descriptorCapacity) return uint64_t(-23);
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
            if (atAddress || x[5] || x[2] != 1 || length > 4 * 1024 * 1024) return invalid;
            if (fd.binder->mapped) return uint64_t(-16);
            if (!allowsBacking(machine, start, end, length, memoryLimit)) return noMemory;
            auto mapping = std::make_shared<SharedFileBytes>();
            mapping->pages.resize(length);
            mapping->size = length;
            fd.binder->mapped = mapping;
            fd.binder->address = start;
            nextMapping = end;
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
        backedLength = pageAlignedLength(bytes.size());
        // The root loader and retained read-only APEX mounts own these bytes throughout the boot.
        if (!(x[2] & 2) && !fd.temporary && files.contains(fd.path))
            imageLength = bytes.size() & ~(pageSize - 1);
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
        segment.anonymousPages = AnonymousPages::allocate(length, anonymousBudget);
        if (!segment.anonymousPages) return noMemory;
    } else if (!sparse && copiedLength) {
        segment.bytes.resize(copiedLength);
        std::copy(bytes.begin() + imageLength, bytes.end(), segment.bytes.begin());
    }
    if (atAddress) changeRange(machine, start, end, {});
    if (!atAddress || (x[3] & fixedNoReplace)) nextMapping = std::max(nextMapping, end);
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
    const auto* bytes = machine.read(address, size, 4);
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
    if (fd.node == Node::securityInfo || fd.node == Node::securityValidate) {
        if (!size || size > pageSize) return invalid;
        const auto* data = machine.read(address, size, 4);
        if (!data) return uint64_t(-14);
        std::string_view label(reinterpret_cast<const char*>(data), size);
        if (label.ends_with('\0')) label.remove_suffix(1);
        if (fd.node == Node::securityValidate) {
            const bool known = currentContextTransitions.contains(label) ||
                std::any_of(currentContextTransitions.begin(), currentContextTransitions.end(),
                            [&](const auto& transition) { return transition.second.contains(label); });
            return known ? size : invalid;
        }
        if (fd.targetProcess != processId() || *fd.offset) return uint64_t(-1);
        auto& process = *current->process;
        const auto known = currentContextTransitions.find(process.context);
        if (known == currentContextTransitions.end() || !known->second.contains(label)) return uint64_t(-13);
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
        const auto* data = machine.read(address, size, 4);
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
            const auto decision = access->second.decisions.find({source, object});
            if (decision == access->second.decisions.end()) return uint64_t(-95);
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
        if (decision == access.decisions.end() ||
            !(std::istringstream(decision->second) >> std::hex >> allowed) || !(allowed & (socket ? socketCreatePermission : fsCreatePermission)))
            return uint64_t(-13);
        std::string_view label;
        if (size) {
            const auto* data = machine.read(address, size, 4);
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
        const auto* data = machine.read(address, size, 4);
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
        const auto space = Pipe::capacity - fd.pipe->data.size();
        if (!space || (size <= pageSize && size > space)) return uint64_t(-11);
        const auto count = std::min<uint64_t>(size, space);
        const auto* data = machine.read(address, count, 4);
        if (!data) return uint64_t(-14);
        fd.pipe->data.insert(fd.pipe->data.end(), data, data + count);
        wakePollers();
        return count;
    }
    if (fd.cgroupFile) return moveToCgroup(machine, fd, address, size);
    if (fd.node == Node::kernelSetting) {
        if (userId() != 0) return uint64_t(-1);
        if (*fd.offset || !size || size > 16) return invalid;
        const auto* data = machine.read(address, size, 4);
        if (!data) return uint64_t(-14);
        std::string_view value(reinterpret_cast<const char*>(data), size);
        while (!value.empty() && (value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);
        unsigned number = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        auto& setting = kernelSettings.at(fd.path);
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
        const auto* bytes = machine.read(address, size, 4);
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
        const auto* data = machine.read(address, count, 4);
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
        if (increment) wakePollers();
        return 8;
    }
    if (!size || fd.node == Node::null) return size;
    if (fd.temporary) {
        const auto& file = *fd.temporary;
        if (file.seals & (sealWrite | sealFutureWrite)) return uint64_t(-1);
        const auto offset = fd.effectiveFlags() & 0x400 ? file.storage->size : position.value_or(*fd.offset);
        const auto limit = file.persistent || file.memoryFile ? maxDataFileBytes : maxGuestFileBytes;
        if (offset > limit || size > limit - offset) return uint64_t(-27);
        const auto* data = machine.read(address, size, 4);
        if (!data) return uint64_t(-14);
        if (const auto error = writeFileBytes(file, offset, {data, size}); error) return error;
        if (!position) *fd.offset = offset + size;
        fileEvent(fd.path, fileModified);
        return size;
    }
    if (fd.node == Node::kernelLog) {
        if (size > 65536) return uint64_t(-28);
        const auto* data = machine.read(address, size, 4);
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
    const auto* data = machine.read(address, size, 4);
    if (!data) return uint64_t(-14);
    destination->append(reinterpret_cast<const char*>(data), size);
    return size;
}

std::optional<std::chrono::steady_clock::time_point> ProbeLinux::refreshTimers(
    std::chrono::steady_clock::time_point now) {
    using namespace std::chrono;
    std::optional<steady_clock::time_point> next;
    bool expired = false;
    for (const auto& timer : timers) {
        const auto event = timer.lock();
        if (!event || !event->timer->next) continue;
        auto& state = *event->timer;
        if (*state.next <= now) {
            auto count = uint64_t{1};
            if (state.interval.count()) {
                const auto overdue = duration_cast<nanoseconds>(now - *state.next);
                count += overdue / state.interval;
                const auto delay = state.interval - overdue % state.interval;
                state.next = delay > steady_clock::time_point::max() - now ?
                    steady_clock::time_point::max() : now + delay;
            } else {
                state.next.reset();
            }
            event->counter += std::min(count, UINT64_MAX - event->counter);
            expired = true;
        }
        if (state.next && (!next || *state.next < *next)) next = state.next;
    }
    if (expired) wakePollers();
    return next;
}

bool ProbeLinux::switchThread(Machine& machine, std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    auto now = steady_clock::now();
    std::optional<steady_clock::time_point> nextDeadline;
    bool runnable = false;
    const auto updateTimers = [&]() {
        nextDeadline = refreshTimers(now);
        for (auto& [tid, thread] : threads) {
            const auto wait = thread.wait;
            if (wait == Wait::poll && thread.hostEventRetry) {
                if (*thread.hostEventRetry <= now) {
                    thread.hostEventRetry.reset();
                    thread.wait = Wait::none;
                } else if (!nextDeadline || *thread.hostEventRetry < *nextDeadline) {
                    nextDeadline = thread.hostEventRetry;
                }
            }
            if ((wait == Wait::poll || wait == Wait::sleep || wait == Wait::futex || wait == Wait::signal) && thread.deadline) {
                if (*thread.deadline <= now) {
                    thread.wait = Wait::none;
                    if (wait == Wait::futex || wait == Wait::signal) {
                        auto& state = tid == currentTid ? machine.cpu : thread.saved;
                        state.pc += 4;
                        state.x[0] = uint64_t(wait == Wait::futex ? -110 : -11);
                        thread.signalWaitMask = 0;
                        thread.deadline.reset();
                        thread.futexBacking.reset();
                        thread.futexAddressSpace.reset();
                    }
                } else if (!nextDeadline || *thread.deadline < *nextDeadline) {
                    nextDeadline = thread.deadline;
                }
            }
            runnable |= thread.wait == Wait::none;
        }
    };
    if (!binderDeathSubscribers.empty()) refreshBinderDeaths();
    updateTimers();
    while (!runnable && nextDeadline && now < deadline) {
        std::this_thread::sleep_until(std::min(*nextDeadline, deadline));
        now = steady_clock::now();
        updateTimers();
    }
    if (now >= deadline) return false;
    auto next = threads.upper_bound(currentTid);
    for (size_t tested = 0; tested < threads.size(); ++tested) {
        if (next == threads.end()) next = threads.begin();
        if (next->second.wait != Wait::none) { ++next; continue; }
        if (next->first != currentTid) {
            const auto retired = machine.cpu.retired;
            auto output = std::move(machine.cpu.output);
            machine.cpu.exclusiveSize = 0;
            const auto previous = currentTid;
            const bool exited = current->wait == Wait::exited;
            if (!exited) current->saved = std::move(machine.cpu);
            if (current->process != next->second.process) {
                current->process->segments = std::move(machine.segments);
                machine.segments = std::move(next->second.process->segments);
            }
            currentTid = next->first;
            current = &next->second;
            machine.cpu = std::move(current->saved);
            machine.cpu.output = std::move(output);
            machine.cpu.retired = retired;
            if (exited) threads.erase(previous);
        }
        sliceEnd = machine.cpu.retired + 4096;
        return true;
    }
    return false;
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
    // Bionic fork uses SIGCHLD, CLONE_CHILD_SETTID and CLONE_CHILD_CLEARTID.
    if (x[0] != 0x1200011 || x[1]) return false;
    if (threads.size() >= maxGuestTasks) { x[0] = uint64_t(-11); return true; }
    Thread child;
    child.process = std::make_shared<Process>(*current->process);
    if (child.process->mdwe & mdweNoInherit) child.process->mdwe = 0;
    child.process->childSignal.reset();
    child.process->segments = machine.segments;
    std::erase_if(child.process->segments, [](const Segment& segment) { return segment.omitOnFork; });
    std::map<const AnonymousPages*, std::shared_ptr<AnonymousPages>> privateCopies;
    for (auto& segment : child.process->segments) {
        if (!segment.anonymousPages) {
            if (segment.wipeOnFork) std::fill(segment.bytes.begin(), segment.bytes.end(), 0);
            continue;
        }
        const auto& source = segment.anonymousPages;
        auto& copy = privateCopies[source.get()];
        if (!copy) copy = AnonymousPages::allocate(source->capacity(), anonymousBudget);
        if (!copy || (!segment.wipeOnFork &&
                      !source->copyTo(*copy, segment.fileOffset, segment.fileOffset, segment.mappedSize()))) {
            x[0] = uint64_t(-11);
            return true;
        }
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
    child.clearTid = x[4];
    child.saved = cpu;
    child.saved.output.clear();
    child.saved.pc += 4;
    child.saved.x[0] = 0;
    child.saved.exclusiveSize = 0;
    // The child TID is written only to its private memory view.
    auto parent = std::move(machine.segments);
    machine.segments = std::move(child.process->segments);
    const bool wrote = writeGuest(machine, x[4], uint32_t(nextTid));
    child.process->segments = std::move(machine.segments);
    machine.segments = std::move(parent);
    if (!wrote) { x[0] = uint64_t(-14); return true; }
    threads.emplace(nextTid, std::move(child));
    x[0] = nextTid++;
    sliceEnd = cpu.retired;
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
        return loadAccessClass(manifest, "keystore2_key", permissions);
    }
    constexpr std::string_view permissions[] = {
        "add_auth", "change_password", "change_user", "clear_ns", "clear_uid", "delete_all_keys",
        "early_boot_ended", "get_attestation_key", "get_auth_token", "get_last_auth_time",
        "get_state", "list", "lock", "pull_metrics", "report_off_body", "reset", "unlock",
    };
    return loadAccessClass(manifest, "keystore2", permissions);
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

bool ProbeLinux::securityCall(Machine& machine) {
    auto& x = machine.cpu.x;
    if (x[8] != 8 && x[8] != 9 && x[8] != 10) return false; // getxattr, lgetxattr, fgetxattr
    const auto name = readPath(machine, x[1]);
    if (!name) { x[0] = uint64_t(-14); return true; }
    std::optional<Descriptor> fd;
    std::string logical;
    if (x[8] == 10) {
        const auto found = descriptors().find(uint32_t(x[0]));
        if (found == descriptors().end()) { x[0] = uint64_t(-9); return true; }
        fd = found->second;
        logical = fd->visiblePath.empty() ? fd->path : fd->visiblePath;
    } else {
        const auto path = readPath(machine, x[0]);
        if (!path) { x[0] = uint64_t(-14); return true; }
        logical = normalizePath(*path);
        if (x[8] == 8) {
            if (const auto link = resolve(logical)) logical = *link;
        }
        fd = find(backingPath(logical), 0);
        if (!fd) { x[0] = uint64_t(-2); return true; }
    }
    std::span<const uint8_t> value;
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
        if (const auto transition = bootTransitions.find(logical); transition != bootTransitions.end()) {
            const auto& label = transition->second.file;
            value = {reinterpret_cast<const uint8_t*>(label.c_str()), label.size() + 1};
        }
    }
    if (value.empty()) x[0] = uint64_t(-61);
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

void ProbeLinux::setCommandLine(const Startup& startup) {
    auto& commandLine = current->process->commandLine;
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
    const auto hasSiblings = std::any_of(threads.begin(), threads.end(), [&](const auto& entry) {
        return entry.first != currentTid && entry.second.process == current->process && entry.second.wait != Wait::exited;
    });
    if (hasSiblings) return false;
    const auto resolved = followLinks(normalizePath(*filename));
    if (!resolved) { x[0] = uint64_t(-40); return true; }
    const auto& executablePath = *resolved;
    const auto file = find(backingPath(executablePath), 0);
    if (!file) { x[0] = uint64_t(-2); return true; }
    if (file->node != Node::regular || file->snapshot ||
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
        const auto executableBytes = file->contents();
        const auto interpreterPath = elfInterpreter(executableBytes);
        if (interpreterPath.empty()) return false; // Static exec has a different startup ABI.
        const auto target = followLinks(interpreterPath);
        if (!target) { x[0] = uint64_t(-40); return true; }
        const auto interpreter = find(backingPath(*target), 0);
        if (!interpreter) { x[0] = uint64_t(-2); return true; }
        std::vector<std::string> arguments;
        if (!argv.empty()) arguments.assign(std::next(argv.begin()), argv.end());
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
        setCommandLine(*image.startup);
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

bool ProbeLinux::futex(Machine& machine) {
    auto& x = machine.cpu.x;
    constexpr uint64_t realtime = 256, privateFlag = 128;
    const auto op = x[1] & ~(privateFlag | realtime);
    if ((x[1] & realtime) && op != 9) return false;
    const bool requeue = op == 3 || op == 4;
    if (op != 0 && op != 1 && !requeue && op != 9 && op != 10) return false;
    const uint32_t mask = op >= 9 ? uint32_t(x[5]) : UINT32_MAX;
    if (x[0] % 4 || !mask) { x[0] = invalid; return true; }
    const auto* bytes = machine.read(x[0], 4, 4);
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
    if (requeue) {
        if (int32_t(x[2]) < 0 || int32_t(x[3]) < 0 || x[4] % 4) { x[0] = invalid; return true; }
        if (!machine.read(x[4], 4, 4)) { x[0] = uint64_t(-14); return true; }
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
    if (x[3]) {
        using namespace std::chrono;
        std::array<int64_t, 2> timeout{};
        const auto* data = machine.read(x[3], sizeof(timeout), 4);
        if (!data) { x[0] = uint64_t(-14); return true; }
        std::memcpy(timeout.data(), data, sizeof(timeout));
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1'000'000'000 ||
            timeout[0] > (INT64_MAX - 1'000'000'000) / 1'000'000'000) {
            x[0] = invalid;
            return true;
        }
        auto remaining = duration_cast<nanoseconds>(seconds(timeout[0])) + nanoseconds(timeout[1]);
        const auto now = steady_clock::now();
        if (op == 9) {
            const auto epoch = (x[1] & realtime) ? duration_cast<nanoseconds>(system_clock::now().time_since_epoch())
                                                  : duration_cast<nanoseconds>(now.time_since_epoch());
            if (remaining <= epoch) { x[0] = uint64_t(-110); return true; }
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

void ProbeLinux::finishProcess(Process& process, uint32_t status, bool killed) {
    const uint32_t code = killed ? 2 : 1; // CLD_KILLED, CLD_EXITED
    const auto uid = process.userIds[0];
    const auto parent = process.parent;
    if (parent) {
        childExits.emplace(process.id, ChildExit{parent, status, uid, code});
        for (auto& [tid, task] : threads) {
            if (task.process->id != parent || task.wait == Wait::exited || !(task.signalMask & sigChildMask)) continue;
            if (task.process->signalActions[sigChild][0] != 1 && !task.process->childSignal)
                task.process->childSignal = Process::ChildSignal{uint32_t(process.id), uid, status, code};
            break;
        }
    }
    for (const auto& [fd, entry] : process.descriptors) releaseRecordLocks(entry, process.id);
    process.descriptors.clear();
    wakePollers();
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
    current->wait = Wait::exited;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::processAlive(uint64_t id) const {
    const auto leader = threads.find(id);
    if (leader != threads.end() && leader->second.process->id == id && leader->second.wait != Wait::exited)
        return true;
    // A process remains alive after its leader exits if it still has another thread.
    return std::any_of(threads.begin(), threads.end(), [&](const auto& entry) {
        return entry.second.process->id == id && entry.second.wait != Wait::exited;
    });
}

bool ProbeLinux::waitChild(Machine& machine) {
    auto& x = machine.cpu.x;
    constexpr uint64_t noHang = 1, exited = 4, noWait = 0x1000000;
    const bool wait4 = x[8] == 260;
    int32_t target = wait4 ? int32_t(x[0]) : x[0] == 1 ? int32_t(x[1]) : -1;
    if (wait4 ? ((target <= 0 && target != -1) || x[3] || (x[2] & ~noHang)) :
                (x[0] == 2 || x[0] > 3 || x[4] || (x[3] & ~(noHang | exited | noWait)))) return false;
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
    if (wait4) {
        if (child == childExits.end()) { x[0] = 0; return true; }
        const auto status = child->second.code == 1 ? child->second.status << 8 : child->second.status;
        if (x[1] && !writeGuest(machine, x[1], status)) { x[0] = uint64_t(-14); return true; }
        x[0] = child->first;
        childExits.erase(child);
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
    if (child != childExits.end() && !(x[3] & noWait)) childExits.erase(child);
    x[0] = 0;
    return true;
}

bool ProbeLinux::waitForEvents() {
    current->wait = Wait::poll;
    sliceEnd = 0;
    return true;
}

bool ProbeLinux::waitForHostEvents() {
    current->hostEventRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
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
    const auto logical = followLinks(normalizePath(*path), !(mask & noFollow));
    if (!logical) { x[0] = uint64_t(-40); return true; }
    const auto watched = backingPath(*logical);
    const auto entry = find(watched, 0);
    if (!entry && !((mask & noFollow) && resolve(*logical))) { x[0] = uint64_t(-2); return true; }
    const bool directory = entry && entry->node == Node::directory;
    if ((mask & onlyDir) && !directory) { x[0] = uint64_t(-20); return true; }
    if (!canAccessParent(watched, false)) { x[0] = uint64_t(-13); return true; }
    if (userId()) {
        const auto dir = directories.find(watched);
        if ((dir != directories.end() && !((dir->second.mode >> permissionShift(dir->second.uid, dir->second.gid)) & 4)) ||
            (entry && entry->temporary &&
             !((entry->temporary->mode >> permissionShift(entry->temporary->uid, entry->temporary->gid)) & 4))) {
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

void ProbeLinux::wakePollers() {
    for (auto& [tid, thread] : threads) {
        if (thread.wait == Wait::poll) thread.wait = Wait::none;
    }
}

bool ProbeLinux::poll(Machine& machine) {
    auto& x = machine.cpu.x;
    if (x[3]) return false; // Temporary signal-mask swap.
    if (x[1] > descriptorLimit()) { x[0] = invalid; return true; }
    using namespace std::chrono;
    if (x[2] && !current->deadline) {
        std::array<int64_t, 2> timeout{};
        const auto* data = machine.read(x[2], sizeof(timeout), 4);
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
        const auto* input = machine.read(x[0], length, 4);
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
        } else if (descriptor->second.pty) {
            const auto events = ptyPoll(descriptor->second);
            entry.returned = (events & entry.events) | (events & 16);
        } else if (const auto pipe = descriptor->second.pipe) {
            entry.returned = pipe->pollEvents(descriptor->second.node == Node::pipeRead) & (entry.events | 0x18);
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
            entry.returned = entry.events & (binderReadable(*descriptor->second.binder) ? 1 : 0);
        } else if (const auto endpoint = descriptor->second.socket) {
            const auto peer = endpoint->peer.lock();
            const bool receiveEnded = endpoint->receiveShutdown ||
                (!endpoint->inetDatagram() && endpoint->connected && (!peer || peer->sendShutdown));
            const bool readable = endpoint->hasInput() || !endpoint->pending.empty() || (peer && receiveEnded);
            const bool writable = (endpoint->inetDatagram() && !endpoint->sendShutdown) ||
                (peer && !endpoint->sendShutdown && !peer->receiveShutdown &&
                 peer->queuedBytes < endpoint->sendBuffer && peer->inbox.size() < 256);
            const bool hungUp = !endpoint->inetDatagram() && endpoint->connected &&
                (!peer || (receiveEnded && endpoint->sendShutdown));
            entry.returned = (entry.events & ((readable ? 1 : 0) | (writable ? 4 : 0) |
                                              (receiveEnded ? 0x2000 : 0))) | (hungUp ? 16 : 0);
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

std::string ProbeLinux::workerReport(const Machine& machine, uint64_t observedProcess) const {
    std::string report;
    for (const auto& [path, tid] : propertyPollers) {
        report += "  guest worker " + std::to_string(tid) + " waited in epoll for " + path + "\n";
    }
    std::ostringstream tasks;
    for (const auto& [tid, thread] : threads) {
        const auto& process = *thread.process;
        if (thread.wait == Wait::exited) continue;
        if (process.id != observedProcess && process.executable != "/system/bin/bootanimation" &&
            process.executable != "/system/bin/surfaceflinger" &&
            !process.executable.starts_with("/vendor/bin/hw/android.hardware.graphics.") &&
            process.programName != "system_server") continue;
        const auto& cpu = tid == currentTid ? machine.cpu : thread.saved;
        const auto end = std::find(thread.name.begin(), thread.name.end(), uint8_t{0});
        const std::string_view name(reinterpret_cast<const char*>(thread.name.data()), end - thread.name.begin());
        tasks << "  guest task " << process.id << '/' << tid << ' ' << process.executable << ' ' << name;
        switch (thread.wait) {
            case Wait::none: tasks << " runnable"; break;
            case Wait::poll: {
                tasks << " poll syscall=" << cpu.x[8] << " arg0=" << cpu.x[0];
                const auto descriptorName = [](const Descriptor& descriptor) {
                    if (!descriptor.visiblePath.empty()) return std::string_view(descriptor.visiblePath);
                    if (descriptor.path.find('\0') == std::string::npos) return std::string_view(descriptor.path);
                    return std::string_view("(guest namespace)");
                };
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
                }
                break;
            }
            case Wait::futex: tasks << " futex=0x" << std::hex << thread.futexAddress << std::dec; break;
            case Wait::signal: tasks << " signal"; break;
            case Wait::sleep: tasks << " sleeping"; break;
            case Wait::suspended: tasks << " suspended"; break;
            case Wait::exited: break;
        }
        tasks << " PC=0x" << std::hex << cpu.pc << " LR=0x" << cpu.x[30] << std::dec << '\n';
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
        const auto* bytes = machine.read(x[2], sizeof(value), 4);
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
        const auto* input = machine.read(x[2], sizeof(config), 4);
        if (!input) return uint64_t(-14);
        std::memcpy(&config, input, sizeof(config));
        const auto backing = descriptors().find(config.fd);
        if (backing == descriptors().end() || backing->second.pathOnly()) return uint64_t(-9);
        if (!loop.bytes.empty()) return uint64_t(-16);
        const auto& image = backing->second;
        // Only immutable preinstalled package bytes are valid backing storage here.
        if (image.node != Node::regular || image.snapshot || !image.path.starts_with("/system/apex/") ||
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
    auto& address = process.nextMapping;
    if (address > mappingLimit - pageSize || !allowsBacking(machine, address, address + pageSize, pageSize, memoryLimit)) return 0;
    // Guest Linux trampoline. The leading NOP matches the ARM64 vDSO unwinding convention.
    constexpr std::array<uint32_t, 4> code{0xd503201f, 0x52801168, 0xd4000001, 0xd4200000};
    Segment page{address, std::vector<uint8_t>(pageSize), 5};
    std::memcpy(page.bytes.data(), code.data(), sizeof(code));
    machine.insertSegment(std::move(page));
    process.signalTrampoline = address + sizeof(code[0]);
    address += pageSize;
    return process.signalTrampoline;
}

bool ProbeLinux::deliverMemoryFault(Machine& machine) {
    auto& cpu = machine.cpu;
    auto& action = current->process->signalActions[sigSegv];
    const auto [handler, flags, restorer, mask] = action;
    const uint64_t signalBit = 1ULL << (sigSegv - 1);
    if (cpu.stop != Stop::fault || handler <= 1 || (current->signalMask & signalBit) ||
        handler % 4 || !machine.read(handler, 4, 1)) return false;

    SignalFrame frame{};
    auto& context = frame.context;
    context.stack = current->alternateStack;
    context.mask = current->signalMask;
    const bool alreadyOnStack = onSignalStack(context.stack, cpu.sp);
    if (alreadyOnStack) context.stack[1] = ssOnStack;
    const auto top = (flags & saOnStack) && !(context.stack[1] & ssDisable) && !alreadyOnStack
        ? context.stack[0] + context.stack[2] : cpu.sp;
    if (top < sizeof(frame)) return false;
    const auto address = (top - sizeof(frame)) & ~uint64_t{15};
    if ((alreadyOnStack || top != cpu.sp) && address < context.stack[0]) return false;

    auto& saved = context.machine;
    saved.faultAddress = cpu.faultAddress & 0x00ff'ffff'ffff'ffff;
    const bool mapped = std::any_of(machine.segments.begin(), machine.segments.end(), [&](const Segment& segment) {
        return saved.faultAddress >= segment.address && saved.faultAddress - segment.address < segment.mappedSize();
    });
    frame.info.number = sigSegv;
    frame.info.code = mapped ? 2 : 1; // SEGV_ACCERR or SEGV_MAPERR
    frame.info.address = saved.faultAddress;
    saved.regs = cpu.x;
    saved.sp = cpu.sp;
    saved.pc = cpu.pc;
    saved.pstate = uint64_t(cpu.nzcv) << 28;
    saved.fp.magic = fpsimdMagic;
    saved.fp.size = sizeof(saved.fp);
    saved.fp.regs = cpu.v;
    frame.frameRecord = {cpu.x[29], cpu.x[30]};
    const auto returnAddress = flags & saRestorer ? restorer : signalRestorer(machine);
    if (!returnAddress || !writeGuest(machine, address, frame)) return false;

    if (flags & saResetHand) action.fill(0);
    current->signalMask |= (mask | ((flags & saNoDefer) ? 0 : signalBit)) & ~unblockableSignals;
    cpu.x[0] = sigSegv;
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
    cpu.sp = saved.sp;
    cpu.pc = saved.pc;
    cpu.nzcv = (saved.pstate >> 28) & 15;
    cpu.exclusiveSize = 0;
    cpu.faultAddress = 0;
    signalContextRestored = true;
    return true;
}

uint64_t ProbeLinux::installSeccomp(Machine& machine, uint64_t address) {
    const auto* header = machine.read(address, 16, 4); // AArch64 sock_fprog
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
    const auto* source = machine.read(instructions, bytes, 4);
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
            default: return invalid;
        }
    }
    current->seccomp = std::make_shared<SeccompFilter>(SeccompFilter{std::move(program), previous, length});
    return 0;
}

bool ProbeLinux::enforceSeccomp(Machine& machine) {
    if (!current->seccomp) return false;
    auto& cpu = machine.cpu;
    constexpr uint32_t auditArm64 = 0xc00000b7;
    std::array<uint32_t, 16> data{uint32_t(cpu.x[8]), auditArm64};
    const auto nextPC = cpu.pc + 4;
    std::memcpy(data.data() + 2, &nextPC, sizeof(nextPC));
    std::memcpy(data.data() + 4, cpu.x.data(), 6 * sizeof(uint64_t));
    constexpr uint32_t allow = 0x7fff0000;
    uint32_t result = allow;
    for (auto filter = current->seccomp; filter; filter = filter->previous) {
        uint32_t accumulator = 0;
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
    // The probe supports fatal SIGSYS. A guest SIGSYS handler needs signal-frame delivery.
    const bool wholeProcess = action != 0 && action != 0x00030000;
    const auto process = current->process;
    for (auto& [tid, task] : threads) {
        if (task.process == process && (wholeProcess || tid == currentTid)) task.wait = Wait::exited;
    }
    if (!processAlive(process->id)) finishProcess(*process, 31, true);
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

uint64_t ProbeLinux::connectInetSocket(Machine& machine, const std::shared_ptr<SocketState>& socket) {
    const auto& x = machine.cpu.x;
    if (x[2] >= 2) {
        uint16_t family = 0;
        if (!machine.readInto(x[1], {reinterpret_cast<uint8_t*>(&family), sizeof(family)})) return uint64_t(-14);
        if (!family) {
            if (!socket->inetDatagram()) return uint64_t(-95);
            socket->connected = false;
            socket->peerInetAddress = {};
            return 0;
        }
    }
    InetAddress destination{};
    if (const auto error = readInetDestination(machine, x[1], x[2], socket->family, destination)) return error;
    if (!filterInetAddress(*socket, socket->family == 2 ? BpfProgram::connect4 : BpfProgram::connect6, destination))
        return uint64_t(-1);
    if (const auto error = routeInetDestination(destination)) return error;
    if (!inetLoopbackUp) return uint64_t(-100);
    if (socket->inetStream()) return connectInetStream(socket, destination);
    if (!socket->localInetAddress[0]) {
        InetAddress local{};
        local[0] = socket->family;
        setInetLoopback(local);
        if (const auto error = reserveInetAddress(socket, local)) return error;
    }
    socket->peerInetAddress = destination;
    socket->connected = true;
    return 0;
}

static bool filterInetPacket(const auto& hooks, const auto& socket, BpfProgram::Packet& packet, bool egress) {
    packet.ringNotification = false;
    if (hooks.empty()) return true;
    packet.socket = socket.storageIdentity;
    packet.socketCookie = socket.cookie;
    packet.socketUid = socket.credentials[1];
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
                                    uint32_t flags) {
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
    packet.ifindex = guestLoopbackIndex;
    packet.socketLookup = [&](std::span<const uint8_t> tuple, bool tcp) {
        return lookupInetSocket(tuple, tcp, &to);
    };
    const bool sent = filterInetPacket(inetEgressPrograms, from, packet, true);
    if (packet.ringNotification) wakePollers();
    if (!sent || !firewall.allows(packet.bytes, GuestFirewall::output) ||
        !firewall.allows(packet.bytes, GuestFirewall::postrouting) ||
        !firewall.allows(packet.bytes, GuestFirewall::prerouting) ||
        !firewall.allows(packet.bytes, GuestFirewall::input)) return false;
    packet.ingressIfindex = guestLoopbackIndex;
    const bool received = filterInetPacket(inetIngressPrograms, to, packet, false);
    if (packet.ringNotification) wakePollers();
    if (!received) return false;
    advance();
    return true;
}

uint64_t ProbeLinux::connectInetStream(const std::shared_ptr<SocketState>& socket,
                                       const SocketState::InetAddress& destination) {
    if (socket->connected) return uint64_t(-106);
    if (socket->listening) return invalid;
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
        accepted->context = listener->context;
        const bool mapped = socket->family != accepted->family;
        accepted->localInetAddress = mapped ? asMappedIpv4(destination) : destination;
        accepted->peerInetAddress = mapped ? asMappedIpv4(socket->localInetAddress) : socket->localInetAddress;
        openInetSocket(accepted, listener.get());

        socket->tcpSequence = uint32_t(socket->cookie);
        accepted->tcpSequence = uint32_t(accepted->cookie);
        constexpr uint32_t syn = 2, ack = 16;
        BpfProgram::SocketOperations connecting{socket->storageIdentity,
            BpfProgram::SocketOperations::tcpConnect, socket->tcpCallbackFlags, syn};
        runTcpOperations(inetSocketOperationPrograms, *socket, connecting);
        socket->tcpCallbackFlags = connecting.callbackFlags;
        if (!transmitInetStream(*socket, *accepted, {}, syn) ||
            !transmitInetStream(*accepted, *socket, {}, syn | ack) ||
            !transmitInetStream(*socket, *accepted, {}, ack)) return uint64_t(-110);

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

uint64_t ProbeLinux::sendInetDatagram(Machine& machine, const std::shared_ptr<SocketState>& socket,
    std::span<const SocketVector> vectors, uint64_t address, uint64_t addressLength) {
    InetAddress destination = socket->peerInetAddress;
    if (address) {
        if (const auto error = readInetDestination(machine, address, addressLength, socket->family, destination))
            return error;
        if (!filterInetAddress(*socket, socket->family == 2 ? BpfProgram::send4 : BpfProgram::send6, destination))
            return uint64_t(-1);
        if (const auto error = routeInetDestination(destination)) return error;
    } else if (!socket->connected) {
        return uint64_t(-89);
    }
    if (socket->sendShutdown) return uint64_t(-32);
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
        packet.ifindex = guestLoopbackIndex;
        packet.socketLookup = [&](std::span<const uint8_t> tuple, bool tcp) {
            return lookupInetSocket(tuple, tcp);
        };
        const bool sent = filterInetPacket(inetEgressPrograms, *socket, packet, true);
        if (packet.ringNotification) wakePollers();
        if (!sent || !firewall.allows(packet.bytes, GuestFirewall::output) ||
            !firewall.allows(packet.bytes, GuestFirewall::postrouting)) return uint64_t(-1);

        const auto peer = findInetDestination(socket->protocol, destination);
        if (!peer || peer->receiveShutdown) return size;
        const auto reportedSource = socket->family == peer->family ? source : asMappedIpv4(source);
        if (peer->connected && !matchInetPeer(peer->peerInetAddress, reportedSource)) return size;
        if (!firewall.allows(packet.bytes, GuestFirewall::prerouting) ||
            !firewall.allows(packet.bytes, GuestFirewall::input)) return size;
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

uint64_t ProbeLinux::receiveInetDatagram(Machine& machine, SocketState& socket, uint64_t address, uint64_t size,
    uint64_t flags, uint64_t source, uint64_t sourceLength) {
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
    if (x[1] == 41 && x[2] == 26 && socket.family == 10) { // IPV6_V6ONLY
        store(uint32_t(socket.inetV6Only));
    } else if (x[1] == 1) {
        switch (x[2]) {
            case 2: store(uint32_t(socket.reuseAddress)); break;
            case 3: store(socket.type); break;
            case 4: store(uint32_t(0)); break; // No asynchronous socket error has been queued.
            case 7: store(socket.sendBuffer); break;
            case 8: store(socket.receiveBuffer); break;
            case 38: store(socket.protocol); break;
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
    if (type != BpfProgram::socketFilter && (!(caps & (capBpf | capSysAdmin)) || !(caps & (1ULL << 12))))
        return uint64_t(-1);
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
    auto& x = machine.cpu.x;
    const uint64_t number = x[8];
    current->hostEventRetry.reset();
    if (enforceSeccomp(machine)) return true;
    if (number == 139) return restoreSignalContext(machine); // rt_sigreturn
    if (securityCall(machine) || socketCall(machine) || eventCall(machine) || fileCall(machine)) return true;
    const auto descriptor = descriptors().find(uint32_t(x[0]));
    switch (number) {
        case 280: { // bpf
            if (x[0] <= 4) { x[0] = bpfMapCall(machine); break; }
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
            if (!path.starts_with('/') && (flags & pathFd) && directory != -100) {
                const auto dir = descriptors().find(uint32_t(directory));
                if (dir == descriptors().end()) { x[0] = uint64_t(-9); break; }
                if (dir->second.node != Node::directory) { x[0] = uint64_t(-20); break; }
                path = dir->second.path + "/" + path;
            }
            path = normalizePath(path.starts_with('/') ? path : "/" + path);
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
                if (writableFiles.size() >= 2 * maxWritableFiles) { x[0] = uint64_t(-28); break; }
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
        case 17: { // getcwd; the guest starts at the root directory.
            constexpr std::array<uint8_t, 2> directory{'/', 0};
            if (x[1] < directory.size()) { x[0] = uint64_t(-34); break; }
            x[0] = machine.write(x[0], directory) ? directory.size() : uint64_t(-14);
            break;
        }
        case 270: { // process_vm_readv, restricted to the caller's guest address space.
            if (x[0] != processId()) { x[0] = uint64_t(-3); break; }
            if (x[5] || x[2] > 64 || x[4] > 64) { x[0] = invalid; break; }
            struct Vector { uint64_t address, size; };
            const auto loadVectors = [&](uint64_t address, uint64_t count, std::array<Vector, 64>& vectors) {
                if (!count) return true;
                const auto bytes = count * sizeof(Vector);
                const auto* source = machine.read(address, bytes, 4);
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
                const auto* input = machine.read(source.address, count, 4);
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
            if (descriptor == descriptors().end()) { x[0] = uint64_t(-9); break; }
            if (x[1] == 0x8913 || x[1] == 0x8914 || x[1] == 0x8921 || x[1] == 0x8933) {
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
            if (x[1] == 0x80086601 && (descriptor->second.temporary ||
                                      descriptor->second.node == Node::directory)) { // FS_IOC_GETFLAGS
                x[0] = descriptor->second.pathOnly() ? uint64_t(-9) :
                    writeGuest(machine, x[2], uint32_t{0}) ? 0 : uint64_t(-14);
                break;
            }
            if (x[1] == 0x40086602 && (descriptor->second.temporary ||
                                      descriptor->second.node == Node::directory)) { // FS_IOC_SETFLAGS
                x[0] = descriptor->second.pathOnly() ? uint64_t(-9) : uint64_t(-95);
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
            if (descriptor->second.node == Node::touchDevice) x[0] = touchIoctl(machine, descriptor->second);
            else if (descriptor->second.pty) x[0] = ptyIoctl(machine, descriptor->second);
            else if (descriptor->second.node == Node::ashmem) x[0] = ashmemIoctl(machine, descriptor->second);
            else if (descriptor->second.node == Node::graphicsAddressSpace) x[0] = graphicsAddressIoctl(machine, descriptor->second);
            else if (x[1] == 0x5401) x[0] = uint64_t(-25); // TCGETS on a non-terminal.
            else if (descriptor->second.binder) {
                const auto result = binderIoctl(machine, descriptor->second);
                if (result == uint64_t(-11) && x[1] == 0xc0306201 && !(descriptor->second.effectiveFlags() & 0x800))
                    return waitForEvents();
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
                case 5: case 6: // F_GETLK, F_SETLK
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
        case 220: return cloneThread(machine);
        case 93: return exitThread(machine);
        case 94:
            if (processId() == pid) return false;
            for (auto& [tid, thread] : threads) {
                if (tid != currentTid && thread.process == current->process) thread.wait = Wait::exited;
            }
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
                for (unsigned i = 0; i < 3; ++i) {
                    const auto value = uint32_t(x[triple ? i : 0]);
                    if (triple && value == UINT32_MAX) continue;
                    if (value == UINT32_MAX || (userId() != 0 && std::find(ids.begin(), ids.end(), value) == ids.end())) {
                        allowed = false;
                        break;
                    }
                    if (triple || userId() == 0 || i == 1) updated[i] = value;
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
                ids = updated;
            }
            x[0] = allowed ? 0 : uint64_t(-1);
            break;
        }
        case 158: case 159: { // getgroups, setgroups
            auto& groups = current->process->groups;
            if (number == 159) {
                if (userId() != 0) { x[0] = uint64_t(-1); break; }
                if (x[0] > 64) { x[0] = invalid; break; }
                std::vector<uint32_t> updated(x[0]);
                const auto size = updated.size() * sizeof(uint32_t);
                if (size) {
                    const auto* source = machine.read(x[1], size, 4);
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
            const auto* bytes = machine.read(parameter, sizeof(int32_t), 4);
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
            if (x[0] == 0 || x[0] == 5) {
                now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch());
            } else if (x[0] == 1 || x[0] == 4 || x[0] == 6 || x[0] == 7) {
                now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch());
            } else if (x[0] == 2 || x[0] == 3) {
                timespec ticks{};
                const auto clock = x[0] == 2 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID;
                if (::clock_gettime(clock, &ticks) != 0) { x[0] = invalid; break; }
                now = std::chrono::seconds(ticks.tv_sec) + std::chrono::nanoseconds(ticks.tv_nsec);
            } else {
                x[0] = invalid;
                break;
            }
            const std::array<int64_t, 2> time{now.count() / 1'000'000'000, now.count() % 1'000'000'000};
            x[0] = writeGuest(machine, x[1], time) ? 0 : uint64_t(-14);
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
                : std::array<uint64_t, 2>{UINT64_MAX, UINT64_MAX};
            const auto old = found == limits.end() ? defaultLimit : found->second;
            auto updated = old;
            if (input) {
                const auto* data = machine.read(input, sizeof(updated), 4);
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
        case 101: { // nanosleep
            using namespace std::chrono;
            const auto now = steady_clock::now();
            if (!current->deadline) {
                std::array<int64_t, 2> timeout{};
                const auto* data = machine.read(x[0], sizeof(timeout), 4);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(timeout.data(), data, sizeof(timeout));
                if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1'000'000'000) { x[0] = invalid; break; }
                if (timeout[0] > 3600) return false;
                current->deadline = now + seconds(timeout[0]) + nanoseconds(timeout[1]);
            }
            if (*current->deadline > now) {
                current->wait = Wait::sleep;
                sliceEnd = 0;
                return true;
            }
            current->deadline.reset();
            x[0] = 0;
            break;
        }
        case 169: { // gettimeofday
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            const std::array<int64_t, 2> time{micros / 1'000'000, micros % 1'000'000};
            x[0] = (!x[0] || writeGuest(machine, x[0], time)) &&
                   (!x[1] || writeGuest(machine, x[1], guestTimezone)) ? 0 : uint64_t(-14);
            break;
        }
        case 170: { // settimeofday, with a guest-only timezone.
            if (!(current->process->effectiveCaps & (1ULL << 25))) { x[0] = uint64_t(-1); break; }
            if (x[0]) return false; // Wall-clock offsets are not modeled.
            if (x[1]) {
                std::array<int32_t, 2> update{};
                const auto* source = machine.read(x[1], sizeof(update), 4);
                if (!source) { x[0] = uint64_t(-14); break; }
                std::memcpy(update.data(), source, sizeof(update));
                if (update[0] < -900 || update[0] > 900) { x[0] = invalid; break; }
                guestTimezone = update;
            }
            x[0] = 0;
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
            if (memoryFiles.size() >= descriptorCapacity) { x[0] = uint64_t(-23); break; }
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
                    const auto* region = machine.read(x[1] + i * sizeof(vector), sizeof(vector), 4);
                    if (!region) { readable = false; break; }
                    std::memcpy(vector.data(), region, sizeof(vector));
                    if (!vector[1]) continue;
                    if (vector[1] > 16384 || written > 16384 - vector[1]) { readable = false; break; }
                    const auto* bytes = machine.read(vector[0], vector[1], 4);
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
                    if (watchdogReport.empty() && message.starts_with("Watchdog: *** WATCHDOG KILLING SYSTEM PROCESS"))
                        watchdogReport = workerReport(machine, processId());
                    if (androidLogObserver) androidLogObserver(message);
                    message.push_back('\n');
                    captureLog(androidOutput, recentAndroidOutput, 128 * 1024, message);
                }
                x[0] = readable ? written : uint64_t(-14);
                break;
            }
            uint64_t written = 0;
            uint64_t result = 0;
            for (uint64_t i = 0; i < x[2]; ++i) {
                std::array<uint64_t, 2> iov{};
                const auto address = x[1] + i * sizeof(iov);
                const auto* entry = address >= x[1] ? machine.read(address, sizeof(iov), 4) : nullptr;
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
            const auto result = map(machine);
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
            constexpr uint64_t mayMove = 1, fixed = 2;
            const auto flags = x[3];
            if ((flags & ~(mayMove | fixed)) || ((flags & fixed) && !(flags & mayMove)) ||
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
            } else if (newSize > length) {
                const bool room = source <= mappingLimit - newSize &&
                    std::none_of(machine.segments.begin(), machine.segments.end(), [&](const Segment& candidate) {
                        return source + length < candidate.address + candidate.mappedSize() &&
                               candidate.address < source + newSize;
                    });
                if (!room) {
                    if (!(flags & mayMove)) { x[0] = noMemory; break; }
                    target = next;
                    if (target > mappingLimit - newSize) { x[0] = noMemory; break; }
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
            if (lazy) {
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
                    part->anonymousPages->discard(part->fileOffset, part->mappedSize());
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
                        std::copy_n(part->bytes.begin(), count, moved.bytes.begin() + offset);
                    }
                }
            }
            machine.segments.erase(mapping, after);
            if (flags & fixed) changeRange(machine, target, target + newSize, {});
            machine.insertSegment(std::move(moved));
            next = std::max(next, target + newSize);
            machine.cpu.exclusiveSize = 0;
            x[0] = target;
            break;
        }
        case 226: // mprotect
            x[0] = protect(machine, x[0], x[1], x[2], memoryLimit,
                           current->process->mdwe & mdweRefuseExecGain, anonymousBudget);
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
            if (advice > 4 && advice != 8 && advice != 10 && advice != 11 && advice != 18 && advice != 19) return false;
            if (x[0] % pageSize || x[1] > maxReservation) { x[0] = invalid; break; }
            const auto length = pageAlignedLength(x[1]);
            if (x[0] > UINT64_MAX - length || !fullyMapped(machine, x[0], x[0] + length)) {
                x[0] = noMemory;
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
                x[0] = uint64_t(-13);
                break;
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
                const auto* data = machine.read(x[1], size, 4);
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
        case 167: { // Guest process controls.
            auto& process = *current->process;
            if (x[0] == 21) { x[0] = current->seccomp ? 2 : 0; break; } // PR_GET_SECCOMP
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
            if (x[0] == 3) { x[0] = 1; break; } // PR_GET_DUMPABLE
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
            if (signal && signal != 9 && signal != 15) return false;
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
            if (signal == 15) {
                // The boot probe supports the default and ignored SIGTERM dispositions.
                // A caught or fully masked delivery needs guest signal-frame handling.
                std::erase_if(victims, [](const auto& item) { return item.second->signalActions[15][0] == 1; });
                for (const auto& [id, victim] : victims) {
                    if (victim->signalActions[15][0] || std::none_of(threads.begin(), threads.end(), [&](const auto& item) {
                            const auto& task = item.second;
                            return task.process == victim && task.wait != Wait::exited && !(task.signalMask & (1ULL << 14));
                        })) return false;
                }
            }
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
        case 130: case 131: { // tkill, tgkill for guest threads.
            const bool grouped = number == 131;
            const int32_t tid = x[grouped ? 1 : 0];
            const int32_t group = grouped ? int32_t(x[0]) : 0;
            const uint32_t signal = x[grouped ? 2 : 1];
            if (tid <= 0 || (grouped && group <= 0) || signal > 64) { x[0] = invalid; break; }
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
            if (signal >= 32 || (!waiting && !(target.signalMask & mask))) return false;
            std::array<uint32_t, 32> info{};
            info[0] = signal;
            info[2] = uint32_t(-6); // SI_TKILL
            info[4] = uint32_t(processId());
            info[5] = sender[0];
            target.pendingSignals.try_emplace(signal, info); // Standard signals coalesce.
            x[0] = 0;
            if (waiting) {
                target.wait = Wait::none;
                target.deadline.reset();
            }
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
                const auto* data = machine.read(x[2], sizeof(values), 4);
                if (!data) { x[0] = uint64_t(-14); break; }
                std::memcpy(values.data(), data, sizeof(values));
                if (values[0] < 0 || values[1] < 0 || values[1] >= 1'000'000'000) { x[0] = invalid; break; }
                timeout = values[0] > (INT64_MAX - values[1]) / 1'000'000'000
                    ? nanoseconds::max() : seconds(values[0]) + nanoseconds(values[1]);
            }
            current->signalWaitMask = 0;
            auto& pending = current->pendingSignals;
            const auto found = std::find_if(pending.begin(), pending.end(), [&](const auto& entry) {
                return requested & (1ULL << (entry.first - 1));
            });
            if (found != pending.end()) {
                if (x[1] && !writeGuest(machine, x[1], found->second)) { x[0] = uint64_t(-14); break; }
                x[0] = found->first;
                pending.erase(found);
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
                const auto* data = machine.read(x[1], sizeof(next), 4);
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
                auto data = machine.read(x[0], sizeof(next), 4);
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
