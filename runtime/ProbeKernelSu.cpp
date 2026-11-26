// KernelSU's interface to its Manager, on top of the built-in root (ProbeLinux::enableRoot).
//
// KernelSU decides root in the kernel, and its Manager talks to the kernel through a driver: an
// anonymous file named [ksu_driver] that takes ioctls. The kernel gives the driver to the Manager's
// process when zygote makes it the Manager's user, and to anyone who calls reboot with two magic
// numbers (the Manager's own ksud does, to get a root shell). Who may do what is the kernel's rule:
// the Manager reads and changes every app's answer; an app it allowed may become root.
//
// The answers are the ones the person gives in the iOS prompt and Nestvane's settings: the Manager
// shows them and changes them, and both see the same. The Manager is the official one only: the
// first certificate of its APK's v2 signature is KernelSU's (as KernelSU's kernel checks it).
// Written from the interface as KernelSU 3.3.1 defines it (uapi/supercall.h, uapi/app_profile.h,
// UAPI version 5); no KernelSU code is used.
#include "ProbeLinux.hpp"

#include "ApkSignature.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>

namespace nestvane {
namespace {

// What the driver says it is: KernelSU 3.3.1's version (its Manager release is 32680), built into
// the kernel rather than a loadable module, with features up to KSU_FEATURE_MAX and UAPI 5.
constexpr uint32_t kernelSuVersion = 32680, uapiVersion = 5, featureCount = 7;
constexpr uint32_t infoFlagManager = 1u << 1;

// The ioctls (KSU_IOCTL_*, 'K').
enum : uint32_t {
    grantRoot = 0x00004b01, getInfo = 0x80104b02, getInfoLegacy = 0x80004b02, reportEvent = 0x40004b03,
    setSepolicy = 0xc0004b04, checkSafeMode = 0x80004b05, getAllowListLegacy = 0xc0004b06,
    getDenyListLegacy = 0xc0004b07, getAllowList = 0xc0044b06, getDenyList = 0xc0044b07,
    uidGrantedRoot = 0xc0004b08, uidShouldUnmount = 0xc0004b09, getManagerAppId = 0x80004b0a,
    getAppProfile = 0xc0004b0b, setAppProfile = 0x40004b0c, getFeature = 0xc0004b0d, setFeature = 0x40004b0e,
};

// struct app_profile, 784 bytes on arm64.
constexpr size_t profileBytes = 784, keyOffset = 4, keyBytes = 256, uidOffset = 260, allowOffset = 264,
                 useDefaultOffset = 272, rootProfileOffset = 536;
constexpr uint32_t profileVersion = 4;

} // namespace

std::optional<uint32_t> ProbeLinux::uidOfPackage(std::string_view wanted) const {
    // The owner of its data directory; else Android's own list.
    if (wanted.find('/') == std::string_view::npos && !wanted.empty() && wanted != "." && wanted != "..")
        if (const auto home = directories.find(backingPath("/data/data/" + std::string(wanted))); home != directories.end())
            return home->second.uid;
    const auto list = writableFiles.find(backingPath("/data/system/packages.list"));
    if (list == writableFiles.end()) return {};
    const auto& storage = *list->second->storage;
    const auto bytes = std::span(storage.pages).first(storage.size);
    std::istringstream lines(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string package;
        uint64_t uid = 0;
        if ((fields >> package >> uid) && package == wanted && uid <= UINT32_MAX) return uint32_t(uid);
    }
    return {};
}

// Zygote made a process this user: when it is the official Manager's, the process gets the driver.
void ProbeLinux::kernelSuUserChanged(uint32_t uid) {
    // Whose it is: the owner of the Manager's data directory, which Android made as it installed it.
    if (!rootOn || uid % 100000 < 10000) return;
    const auto home = directories.find(backingPath("/data/data/" + std::string(kernelSuPackage)));
    if (home == directories.end() || home->second.uid != uid % 100000) return;
    // Its APK: /data/app/<random>/me.weishu.kernelsu-<random>/base.apk.
    const std::string marker = "/" + std::string(kernelSuPackage) + "-";
    bool official = false;
    for (const auto& [path, file] : writableFiles) {
        if (!path.starts_with("/data/app/") || !path.ends_with("/base.apk") || path.find(marker) == std::string::npos)
            continue;
        auto checked = kernelSuSignatures.find(path);
        if (checked == kernelSuSignatures.end()) {
            const auto& storage = *file->storage;
            const bool matches = apk::kernelSuManagerCertificate(apk::firstV2Certificate(std::span(storage.pages).first(storage.size)));
            checked = kernelSuSignatures.emplace(path, matches).first;
            if (androidLogObserver)
                androidLogObserver(std::string("Nestvane: KernelSU Manager ") + (matches ? "recognized" : "not signed by KernelSU") +
                                   " at " + path);
        }
        official = official || checked->second;
    }
    if (!official) return;
    kernelSuManagerUid = uid;
    installKernelSu();
}

uint64_t ProbeLinux::installKernelSu() {
    const auto fd = freeDescriptor();
    if (fd == descriptorCapacity) return uint64_t(-24);
    descriptors().emplace(fd, Descriptor{"anon_inode:[ksu_driver]", {}, 0x80002, Node::kernelSu});
    return fd;
}

bool ProbeLinux::kernelSuManager() const {
    return kernelSuManagerUid >= 0 && current->process->userIds[0] == uint64_t(kernelSuManagerUid);
}

std::optional<ProbeLinux::RootCredentials> ProbeLinux::customRoot(uint32_t uid) const {
    if (!rootProfile) return {};
    const auto package = packageOfUid(uid);
    if (package.empty()) return {};
    const auto profile = rootProfile(package);
    // Only an allowed app's own profile; the default one is root with everything.
    if (profile.size() != profileBytes || !profile[allowOffset] || profile[useDefaultOffset]) return {};
    int32_t ids[3] = {};
    std::memcpy(ids, profile.data() + rootProfileOffset, sizeof ids);
    if (ids[0] < 0 || ids[1] < 0) return {};
    RootCredentials credentials;
    credentials.uid = uint32_t(ids[0]);
    credentials.gid = uint32_t(ids[1]);
    const int32_t count = std::clamp(ids[2], 0, 32);
    for (int32_t group = 0; group < count; ++group) {
        int32_t id = 0;
        std::memcpy(&id, profile.data() + rootProfileOffset + 12 + 4 * group, 4);
        if (id >= 0) credentials.groups.push_back(uint32_t(id));
    }
    uint64_t capabilities[3] = {};
    std::memcpy(capabilities, profile.data() + rootProfileOffset + 144, sizeof capabilities);
    credentials.effective = capabilities[0] & Process::allCapabilities;
    credentials.permitted = capabilities[1] & Process::allCapabilities;
    credentials.inheritable = capabilities[2] & Process::allCapabilities;
    const auto* domain = reinterpret_cast<const char*>(profile.data() + rootProfileOffset + 168);
    credentials.context = std::string(domain, strnlen(domain, 64));
    if (credentials.context.empty()) credentials.context = std::string(rootContext);
    return credentials;
}

void ProbeLinux::becomeRoot() {
    auto& process = *current->process;
    // A custom profile the KernelSU Manager set for the app: its user, groups, capabilities and domain.
    const auto custom = customRoot(uint32_t(process.userIds[0]));
    process.userIds.fill(custom ? custom->uid : 0);
    process.groupIds.fill(custom ? custom->gid : 0);
    process.groups = custom ? custom->groups : std::vector<uint32_t>{0};
    // An app's bounding set is empty (zygote drops it): root gets every capability back, as su gives
    // them, so that it can read and change any app's files.
    process.capabilityBound = Process::allCapabilities;
    process.inheritableCaps = custom ? custom->inheritable : Process::allCapabilities;
    process.secureBits = 0;
    process.pendingContext = custom ? custom->context : std::string(rootContext);
    current->seccomp.reset();
    current->noNewPrivileges = false;
}

std::optional<uint64_t> ProbeLinux::kernelSuIoctl(Machine& machine, uint32_t request, uint64_t argument) {
    const auto uid = uint32_t(current->process->userIds[0]);
    const bool manager = kernelSuManager(), root = uid == 0;
    const auto write = [&](uint64_t at, const void* bytes, size_t size) {
        return machine.write(at, {static_cast<const uint8_t*>(bytes), size});
    };
    const auto read = [&](uint64_t at, void* bytes, size_t size) {
        return machine.readInto(at, {static_cast<uint8_t*>(bytes), size});
    };
    // The answers as uids: 1 allowed, 0 refused.
    const auto answers = [&] {
        std::vector<std::pair<uint32_t, bool>> result;
        if (!rootAnswers) return result;
        for (const auto& [name, allowed] : rootAnswers()) {
            std::optional<uint32_t> owner;
            if (name.starts_with("uid ")) {
                uint32_t number = 0;
                if (std::from_chars(name.data() + 4, name.data() + name.size(), number).ec == std::errc{}) owner = number;
            } else {
                owner = uidOfPackage(name);
            }
            if (owner) result.emplace_back(*owner, allowed);
        }
        return result;
    };
    const auto answerFor = [&](uint32_t subject) -> std::optional<bool> {
        if (int64_t(subject) == kernelSuManagerUid) return true;
        for (const auto& [owner, allowed] : answers())
            if (owner % 100000 == subject % 100000) return allowed;
        return {};
    };
    switch (request) {
        case getInfo: case getInfoLegacy: {
            const uint32_t info[4] = {kernelSuVersion, manager ? infoFlagManager : 0, featureCount, uapiVersion};
            return write(argument, info, request == getInfo ? 16 : 12) ? 0 : uint64_t(-14);
        }
        case checkSafeMode: {
            const uint8_t safe = 0;
            return write(argument, &safe, 1) ? 0 : uint64_t(-14);
        }
        case grantRoot: {
            // The Manager, and the apps the person or the Manager allowed; one never asked is asked now.
            if (!manager && !root) {
                const int decision = rootDecision ? rootDecision(uid, packageOfUid(uid)) : 0;
                if (decision < 0) {
                    current->hostEventRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                    return std::nullopt;
                }
                if (!decision) return uint64_t(-1);
            }
            const auto custom = customRoot(uid);
            becomeRoot();
            auto& process = *current->process;
            process.context = std::exchange(process.pendingContext, {});
            process.permittedCaps = custom ? custom->permitted : Process::allCapabilities;
            process.effectiveCaps = custom ? custom->effective : Process::allCapabilities;
            if (androidLogObserver) androidLogObserver("Nestvane: root granted through KernelSU to uid " + std::to_string(uid));
            return 0;
        }
        default: break;
    }
    if (!manager && !root) return uint64_t(-1);
    switch (request) {
        case getAllowList: case getDenyList: {
            uint16_t capacity = 0;
            if (!read(argument, &capacity, sizeof capacity)) return uint64_t(-14);
            std::vector<uint32_t> uids;
            for (const auto& [owner, allowed] : answers())
                if (allowed == (request == getAllowList)) uids.push_back(owner);
            const uint16_t total = uint16_t(std::min<size_t>(uids.size(), UINT16_MAX)),
                           count = std::min(capacity, total);
            const uint16_t header[2] = {count, total};
            if (!write(argument, header, sizeof header) || (count && !write(argument + 4, uids.data(), count * 4u)))
                return uint64_t(-14);
            return 0;
        }
        case getAllowListLegacy: case getDenyListLegacy: {
            std::vector<uint32_t> uids;
            for (const auto& [owner, allowed] : answers())
                if (allowed == (request == getAllowListLegacy) && uids.size() < 128) uids.push_back(owner);
            const uint32_t count = uint32_t(uids.size());
            if ((count && !write(argument, uids.data(), count * 4u)) || !write(argument + 512, &count, 4)) return uint64_t(-14);
            return 0;
        }
        case uidGrantedRoot: case uidShouldUnmount: {
            uint32_t subject = 0;
            if (!read(argument, &subject, 4)) return uint64_t(-14);
            // No modules are mounted: nothing is for an app to have taken away.
            const uint8_t yes = request == uidGrantedRoot && answerFor(subject).value_or(false);
            return write(argument + 4, &yes, 1) ? 0 : uint64_t(-14);
        }
        case getManagerAppId: {
            const uint32_t id = kernelSuManagerUid < 0 ? uint32_t(-1) : uint32_t(kernelSuManagerUid % 100000);
            return write(argument, &id, 4) ? 0 : uint64_t(-14);
        }
        case getAppProfile: {
            if (!manager) return uint64_t(-1);
            std::array<uint8_t, profileBytes> profile{};
            if (!read(argument, profile.data(), profile.size())) return uint64_t(-14);
            int32_t subject = 0;
            std::memcpy(&subject, profile.data() + uidOffset, 4);
            const auto answer = subject >= 0 ? answerFor(uint32_t(subject)) : std::nullopt;
            if (!answer) return uint64_t(-2);  // no profile: the Manager shows its default
            // The profile the Manager set last, with the answer as it is now (the iOS prompt and
            // Nestvane's settings change it too).
            if (const auto package = packageOfUid(uint32_t(subject)); !package.empty() && rootProfile) {
                if (const auto stored = rootProfile(package); stored.size() == profileBytes) {
                    std::copy(stored.begin(), stored.end(), profile.begin());
                    std::memcpy(profile.data() + uidOffset, &subject, 4);
                    profile[allowOffset] = *answer;
                    return write(argument, profile.data(), profile.size()) ? 0 : uint64_t(-14);
                }
            }
            std::memcpy(profile.data(), &profileVersion, 4);
            profile[allowOffset] = *answer;
            profile[useDefaultOffset] = 1;
            if (*answer) {
                // The default root profile: root, its group, every capability, su's SELinux domain.
                std::fill(profile.begin() + useDefaultOffset + 1, profile.end(), 0);
                const int32_t ids[3] = {0, 0, 1};  // uid, gid, groups_count with groups[0] = 0
                std::memcpy(profile.data() + rootProfileOffset, ids, sizeof ids);
                const uint64_t capabilities[3] = {Process::allCapabilities, Process::allCapabilities, Process::allCapabilities};
                std::memcpy(profile.data() + rootProfileOffset + 144, capabilities, sizeof capabilities);
                std::copy(rootContext.begin(), rootContext.end(), profile.begin() + rootProfileOffset + 168);
            }
            return write(argument, profile.data(), profile.size()) ? 0 : uint64_t(-14);
        }
        case setAppProfile: {
            if (!manager) return uint64_t(-1);
            std::array<uint8_t, profileBytes> profile{};
            if (!read(argument, profile.data(), profile.size())) return uint64_t(-14);
            const auto* key = reinterpret_cast<const char*>(profile.data() + keyOffset);
            const std::string name(key, strnlen(key, keyBytes));
            int32_t subject = 0;
            std::memcpy(&subject, profile.data() + uidOffset, 4);
            // "$" and "#" are the defaults for apps without root and with it: those stay as they are.
            if (name.empty() || name == "$" || name == "#") return 0;
            if (rootRemember) rootRemember(name, profile[allowOffset] != 0);
            if (rootRememberProfile) rootRememberProfile(name, profile);
            if (androidLogObserver)
                androidLogObserver("Nestvane: KernelSU Manager " + std::string(profile[allowOffset] ? "allowed" : "refused") +
                                   " root for " + name);
            return 0;
        }
        case getFeature: {
            uint32_t feature = 0;
            if (!read(argument, &feature, 4)) return uint64_t(-14);
            // su is the only feature: it is on (Nestvane's settings turn it off).
            const uint64_t value = feature == 0;
            const uint8_t supported = feature == 0;
            if (!write(argument + 8, &value, 8) || !write(argument + 16, &supported, 1)) return uint64_t(-14);
            return 0;
        }
        case setFeature: return uint64_t(-95);  // the settings are Nestvane's
        case reportEvent: case setSepolicy: return root ? 0 : uint64_t(-1);
        default: return uint64_t(-25);
    }
}

} // namespace nestvane
