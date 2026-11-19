#pragma once

#include "GuestNetlink.hpp"

#include <array>
#include <optional>
#include <string>

namespace nestvane {

class GuestRoute {
public:
    enum Message : uint16_t {
        newLink = 16, getLink = 18, newAddress = 20, getAddress = 22, newRoute = 24, getRoute = 26,
        newRule = 32, deleteRule = 33, getRule = 34
    };

    GuestRoute() {
        for (const uint8_t family : {2, 10}) {
            rules.push_back({family, lookupTable, 255, 0});
            rules.push_back({family, lookupTable, 254, 32766});
            if (family == 2) rules.push_back({family, lookupTable, 253, 32767});
        }
    }

    static bool isDump(uint16_t type, uint16_t flags) {
        return (flags & netlinkDump) == netlinkDump &&
               (type == getRule || type == getLink || type == getAddress || type == getRoute);
    }

    int32_t process(const NetlinkHeader& message, std::span<const uint8_t> payload, bool admin, bool up,
                    uint32_t index, uint32_t mtu, auto&& reply) {
        if (message.type == newRule || message.type == deleteRule) {
            if (!admin) return -1;
            return changeRule(message, payload);
        }
        const auto family = payload.empty() ? 0 : payload.front();
        if (family && family != 2 && family != 10 && !(message.type == getLink && family == 17)) return -97;
        const bool dump = isDump(message.type, message.flags);
        if (message.type == getLink) {
            if (!dump) {
                if (payload.size() < 16) return -22;
                const auto wanted = readNetlink<int32_t>(payload, 4);
                if (wanted != int32_t(index) && !namesLoopback(payload.subspan(16))) return -19;
            }
            reply(newLink, dump ? netlinkMulti : 0, link(up, index, mtu));
        } else if (dump && message.type == getRule) {
            for (const auto& rule : rules) {
                if (!family || rule.family == family) reply(newRule, netlinkMulti, encodeRule(rule));
            }
        } else if (dump && (message.type == getAddress || message.type == getRoute)) {
            for (const uint8_t version : {2, 10}) {
                if (family && version != family) continue;
                if (message.type == getAddress) reply(newAddress, netlinkMulti, address(version, index));
                else reply(newRoute, netlinkMulti, localRoute(version, index));
            }
        } else {
            return -95;
        }
        if (dump) reply(netlinkDone, netlinkMulti, std::vector<uint8_t>(sizeof(int32_t)));
        return 0;
    }

    int routeLoopback(uint8_t family, uint32_t mark, uint32_t uid) const {
        for (const auto& rule : rules) {
            if (rule.family != family) continue;
            const bool matched = ((mark ^ rule.mark) & rule.mask) == 0 &&
                (!rule.uids || (uid >= (*rule.uids)[0] && uid <= (*rule.uids)[1])) &&
                (rule.input.empty() || rule.input == "lo") && (rule.output.empty() || rule.output == "lo");
            if (matched == bool(rule.flags & 2)) continue; // FIB_RULE_INVERT
            if (rule.action == lookupTable && rule.table == 255) return 0;
            if (rule.action == blackhole) return -22;
            if (rule.action == unreachable) return -101;
            if (rule.action == prohibit) return -13;
        }
        return -101;
    }

private:
    static constexpr uint8_t lookupTable = 1, blackhole = 6, unreachable = 7, prohibit = 8;
    static constexpr uint16_t inputName = 3, priorityAttribute = 6, fwmark = 10, tableAttribute = 15,
                              fwmask = 16, outputName = 17, uidRange = 20, protocol = 21;
    static constexpr size_t maxRules = 128, headerBytes = 12;
    struct Rule {
        uint8_t family, action;
        uint32_t table, priority;
        uint32_t mark = 0, mask = 0, flags = 0;
        uint8_t origin = 0;
        std::string input{}, output{};
        std::optional<std::array<uint32_t, 2>> uids{};
        bool operator==(const Rule&) const = default;
    };
    std::vector<Rule> rules;

    static int forAttributes(std::span<const uint8_t> bytes, auto&& apply) {
        while (!bytes.empty()) {
            if (bytes.size() < 4) return -22;
            const auto size = readNetlink<uint16_t>(bytes, 0), type = readNetlink<uint16_t>(bytes, 2);
            if (size < 4 || size > bytes.size()) return -22;
            if (const auto result = apply(type, bytes.subspan(4, size - 4))) return result;
            bytes = bytes.subspan(std::min(bytes.size(), netlinkAlign(size)));
        }
        return 0;
    }

    static bool namesLoopback(std::span<const uint8_t> bytes) {
        bool found = false;
        const auto status = forAttributes(bytes, [&](uint16_t type, auto value) {
            if (type == 3) found = std::ranges::equal(value, std::array<uint8_t, 3>{'l', 'o', 0});
            return 0;
        });
        return !status && found;
    }

    int changeRule(const NetlinkHeader& message, std::span<const uint8_t> bytes) {
        if (bytes.size() < headerBytes) return -22;
        if (bytes[0] != 2 && bytes[0] != 10) return -97;
        if (bytes[1] || bytes[2] || bytes[3]) return -95; // Prefix and TOS selectors need a FIB.
        const bool add = message.type == newRule;
        Rule rule{bytes[0], bytes[7], bytes[4], 0};
        rule.flags = readNetlink<uint32_t>(bytes, 8);
        if (rule.flags & ~3U) return -95;
        bool hasPriority = false, hasMark = false, hasMask = false;
        const auto parsed = forAttributes(bytes.subspan(headerBytes), [&](uint16_t type, auto value) -> int {
            if (type == inputName || type == outputName) {
                if (value.empty() || value.size() > 16 || value.back() ||
                    std::ranges::find(value.first(value.size() - 1), uint8_t{0}) != value.begin() + value.size() - 1)
                    return -22;
                auto& name = type == inputName ? rule.input : rule.output;
                name.assign(reinterpret_cast<const char*>(value.data()), value.size() - 1);
                return 0;
            }
            if (type == uidRange) {
                if (value.size() != 8) return -22;
                const auto range = readNetlink<std::array<uint32_t, 2>>(value, 0);
                if (range[0] > range[1]) return -22;
                rule.uids = range;
                return 0;
            }
            if (type == protocol) {
                if (value.size() != 1) return -22;
                rule.origin = value[0];
                return 0;
            }
            if (type != priorityAttribute && type != tableAttribute && type != fwmark && type != fwmask) return -95;
            if (value.size() != 4) return -22;
            const auto number = readNetlink<uint32_t>(value, 0);
            switch (type) {
                case priorityAttribute: rule.priority = number; hasPriority = true; break;
                case tableAttribute: rule.table = number; break;
                case fwmark: rule.mark = number; hasMark = true; break;
                case fwmask: rule.mask = number; hasMask = true; break;
            }
            return 0;
        });
        if (parsed) return parsed;
        if (hasMark && !hasMask) rule.mask = UINT32_MAX;
        if (!add) {
            const auto old = std::ranges::find_if(rules, [&](const Rule& entry) {
                return entry.family == rule.family && (!rule.action || entry.action == rule.action) &&
                    (!hasPriority || entry.priority == rule.priority) && (!rule.table || entry.table == rule.table) &&
                    (!hasMark || entry.mark == rule.mark) && (!hasMask || entry.mask == rule.mask) &&
                    (rule.input.empty() || rule.input == entry.input) &&
                    (rule.output.empty() || rule.output == entry.output) && (!rule.uids || rule.uids == entry.uids);
            });
            if (old == rules.end()) return -2;
            rules.erase(old);
            return 0;
        }
        if (!hasPriority || (rule.action == lookupTable && !rule.table)) return -22;
        if (rule.action != lookupTable && rule.action != blackhole && rule.action != unreachable && rule.action != prohibit)
            return -95;
        if ((message.flags & 0x200) && std::ranges::find(rules, rule) != rules.end()) return -17;
        if (rules.size() >= maxRules) return -28;
        const auto insert = std::ranges::upper_bound(rules, rule, [](const Rule& a, const Rule& b) {
            return std::pair(a.family, a.priority) < std::pair(b.family, b.priority);
        });
        rules.insert(insert, std::move(rule));
        return 0;
    }

    static std::vector<uint8_t> encodeRule(const Rule& rule) {
        std::vector<uint8_t> data(headerBytes);
        data[0] = rule.family;
        data[4] = rule.table <= UINT8_MAX ? rule.table : 0;
        data[7] = rule.action;
        std::memcpy(data.data() + 8, &rule.flags, sizeof(rule.flags));
        appendNetlinkValue(data, priorityAttribute, rule.priority);
        appendNetlinkValue(data, tableAttribute, rule.table);
        if (rule.mask) {
            appendNetlinkValue(data, fwmark, rule.mark);
            appendNetlinkValue(data, fwmask, rule.mask);
        }
        if (rule.origin) appendNetlinkValue(data, protocol, rule.origin);
        if (rule.uids) appendNetlinkValue(data, uidRange, *rule.uids);
        const auto name = [&](uint16_t type, const std::string& value) {
            if (!value.empty()) appendNetlinkAttribute(data, type,
                {reinterpret_cast<const uint8_t*>(value.c_str()), value.size() + 1});
        };
        name(inputName, rule.input);
        name(outputName, rule.output);
        return data;
    }

    static std::vector<uint8_t> link(bool up, uint32_t index, uint32_t mtu) {
        std::vector<uint8_t> data(16); // ifinfomsg
        const uint16_t hardware = 772; // ARPHRD_LOOPBACK
        const uint32_t flags = 8 | (up ? (1 | 64 | 0x10000) : 0);
        std::memcpy(data.data() + 2, &hardware, sizeof(hardware));
        std::memcpy(data.data() + 4, &index, sizeof(index));
        std::memcpy(data.data() + 8, &flags, sizeof(flags));
        appendNetlinkValue(data, 3, "lo");
        appendNetlinkValue(data, 4, mtu);
        appendNetlinkValue(data, 16, uint8_t(0)); // IF_OPER_UNKNOWN
        appendNetlinkValue(data, 33, uint8_t(up));
        return data;
    }

    static std::span<const uint8_t> loopback(uint8_t family) {
        static constexpr std::array<uint8_t, 4> ipv4{127, 0, 0, 1};
        static constexpr auto ipv6 = [] { std::array<uint8_t, 16> ip{}; ip.back() = 1; return ip; }();
        return family == 2 ? std::span<const uint8_t>(ipv4) : std::span<const uint8_t>(ipv6);
    }

    static std::vector<uint8_t> address(uint8_t family, uint32_t index) {
        std::vector<uint8_t> data{family, uint8_t(family == 2 ? 32 : 128), 0x80, 254, 0, 0, 0, 0};
        std::memcpy(data.data() + 4, &index, sizeof(index));
        appendNetlinkAttribute(data, 1, loopback(family));
        appendNetlinkAttribute(data, 2, loopback(family));
        if (family == 2) appendNetlinkValue(data, 3, "lo");
        return data;
    }

    static std::vector<uint8_t> localRoute(uint8_t family, uint32_t index) {
        std::vector<uint8_t> data(headerBytes); // rtmsg
        data[0] = family;
        data[1] = family == 2 ? 32 : 128;
        data[4] = 255; // RT_TABLE_LOCAL
        data[5] = 2; // RTPROT_KERNEL
        data[6] = 254; // RT_SCOPE_HOST
        data[7] = 2; // RTN_LOCAL
        appendNetlinkAttribute(data, 1, loopback(family));
        appendNetlinkValue(data, 4, index);
        appendNetlinkValue(data, 15, uint32_t(255));
        return data;
    }
};

} // namespace nestvane
