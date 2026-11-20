#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nestvane {

inline constexpr std::string_view accessAclName = "system.posix_acl_access";
inline constexpr std::string_view defaultAclName = "system.posix_acl_default";

class PosixAcl {
public:
    static constexpr uint32_t version = 2;
    static constexpr size_t maxBytes = 4096;

    static std::optional<PosixAcl> decode(std::span<const uint8_t> bytes) {
        static_assert(std::endian::native == std::endian::little);
        if (bytes.size() < sizeof(version) || bytes.size() > maxBytes ||
            (bytes.size() - sizeof(version)) % sizeof(Entry)) return {};
        uint32_t encodedVersion;
        std::memcpy(&encodedVersion, bytes.data(), sizeof(encodedVersion));
        if (encodedVersion != version) return {};
        bytes = bytes.subspan(sizeof(version));
        PosixAcl acl;
        acl.entries.resize(bytes.size() / sizeof(Entry));
        if (acl.entries.empty()) return acl;
        std::memcpy(acl.entries.data(), bytes.data(), bytes.size());
        bool owner = false, group = false, other = false, named = false, mask = false;
        uint16_t previous = 0;
        for (auto& entry : acl.entries) {
            const bool repeatable = entry.tag == namedUser || entry.tag == namedGroup;
            if (entry.permission > 7 || previous > entry.tag ||
                (previous == entry.tag && !repeatable)) return {};
            previous = entry.tag;
            if (repeatable) {
                if (entry.id == UINT32_MAX) return {};
                named = true;
                continue;
            }
            entry.id = UINT32_MAX; // Linux ignores the identifier on non-named entries.
            switch (entry.tag) {
                case ownerUser: owner = true; break;
                case ownerGroup: group = true; break;
                case permissionMask: mask = true; break;
                case others: other = true; break;
                default: return {};
            }
        }
        if (!owner || !group || !other || (named && !mask)) return {};
        return acl;
    }

    std::vector<uint8_t> encode() const {
        if (empty()) return {};
        std::vector<uint8_t> bytes(sizeof(version) + entries.size() * sizeof(Entry));
        std::memcpy(bytes.data(), &version, sizeof(version));
        std::memcpy(bytes.data() + sizeof(version), entries.data(), entries.size() * sizeof(Entry));
        return bytes;
    }

    bool empty() const { return entries.empty(); }
    bool extended() const { return entries.size() > 3; }

    uint32_t modeBits() const {
        if (empty()) return 0;
        return uint32_t(entries.front().permission) << 6 |
               uint32_t(entries[effectiveGroupIndex()].permission) << 3 | entries.back().permission;
    }

    void updateMode(uint32_t mode, bool limit) {
        if (empty()) return;
        const auto update = [limit](Entry& entry, uint32_t bits) {
            entry.permission = limit ? (bits & entry.permission & 7) : (bits & 7);
        };
        update(entries.front(), mode >> 6);
        update(entries[effectiveGroupIndex()], mode >> 3);
        update(entries.back(), mode);
    }

    template<class InGroup>
    bool allows(unsigned request, bool isOwner, uint32_t user, uint32_t group, InGroup inGroup) const {
        const auto test = [request](unsigned bits) { return (bits & request) == request; };
        if (isOwner) return test(entries.front().permission);
        const bool masked = entries[entries.size() - 2].tag == permissionMask;
        const auto mask = masked ? entries[entries.size() - 2].permission : 7;
        bool groupMatched = false;
        for (const auto& entry : entries) {
            if (entry.tag == namedUser && entry.id == user) return test(entry.permission & mask);
            if ((entry.tag == ownerGroup && inGroup(group)) ||
                (entry.tag == namedGroup && inGroup(entry.id))) {
                groupMatched = true;
                if (test(entry.permission & mask)) return true;
            }
        }
        return !groupMatched && test(entries.back().permission);
    }

private:
    enum Tag : uint16_t { ownerUser = 1, namedUser = 2, ownerGroup = 4,
                          namedGroup = 8, permissionMask = 16, others = 32 };
    struct Entry { uint16_t tag, permission; uint32_t id; };
    static_assert(sizeof(Entry) == 8);
    std::vector<Entry> entries;

    size_t effectiveGroupIndex() const {
        const auto mask = entries.end() - 2;
        if (mask->tag == permissionMask) return size_t(mask - entries.begin());
        return size_t(std::find_if(entries.begin(), mask + 1,
            [](const Entry& entry) { return entry.tag == ownerGroup; }) - entries.begin());
    }
};

} // namespace nestvane
