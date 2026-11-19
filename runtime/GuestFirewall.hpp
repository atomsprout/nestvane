#pragma once

#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nestvane {

class GuestFirewall {
public:
    enum Hook : unsigned { prerouting, input, forward, output, postrouting };
    struct PacketState {
        uint32_t* mark = nullptr;
        uint32_t* connectionMark = nullptr;
    };

    uint64_t get(Machine& machine, bool ipv6) {
        const auto& x = machine.cpu.x;
        const uint32_t option = x[2];
        uint32_t length = 0;
        if (!read(machine, x[4], length)) return fault;
        if (option == (ipv6 ? 68 : 66) || option == (ipv6 ? 69 : 67)) {
            std::array<uint8_t, 30> revision{};
            if (length != revision.size()) return invalid;
            if (!read(machine, x[3], revision)) return fault;
            const auto name = string(std::span(revision).first(29));
            const bool match = option == (ipv6 ? 68 : 66);
            const auto version = revision.back();
            if (match && name == "connmark") return version == 1 ? 0 : uint64_t(-93);
            if (!match && name == "CONNMARK") return (version == 1 || version == 2) ? 0 : uint64_t(-93);
            const bool known = match ? name == "comment" || name == "tcp" || name == "udp" : name == "TCPMSS";
            return !known ? missing : version ? uint64_t(-93) : 0;
        }
        if (option != 64 && option != 65) return unsupported;
        std::array<char, 32> name{};
        if (length < name.size()) return invalid;
        if (!read(machine, x[3], name)) return fault;
        auto* table = lookup(ipv6, string(name));
        if (!table) return missing;
        if (option == 64) {
            Info result{name, table->hooks, table->starts, table->policies,
                        uint32_t(table->offsets.size()), uint32_t(table->entries.size())};
            if (length != sizeof(result)) return invalid;
            return write(machine, x[3], result) ? 0 : fault;
        }

        uint32_t size = 0;
        if (length < 40 || !read(machine, x[3] + name.size(), size)) return length < 40 ? invalid : fault;
        if (size != length - 40) return invalid;
        if (size != table->entries.size()) return uint64_t(-11); // Table changed since the information query.
        return machine.write(x[3] + 40, table->entries) ? 0 : fault;
    }

    uint64_t set(Machine& machine, bool ipv6) {
        const auto& x = machine.cpu.x;
        const uint32_t option = x[2];
        if (option != 64 && option != 65) return unsupported;
        if (x[4] > maxTableBytes + sizeof(Replace)) return uint64_t(-7);
        std::array<char, 32> name{};
        if (x[4] < name.size()) return invalid;
        if (!read(machine, x[3], name)) return fault;
        auto* table = lookup(ipv6, string(name));
        if (!table) return missing;
        const Layout layout(ipv6);

        if (option == 65) {
            uint32_t count = 0;
            if (x[4] < 40 || !read(machine, x[3] + name.size(), count)) return x[4] < 40 ? invalid : fault;
            if (count != table->offsets.size() || x[4] != 40 + count * sizeof(Counters)) return invalid;
            std::vector<Counters> incoming(count);
            if (!machine.readInto(x[3] + 40, writableBytes(incoming))) return fault;
            for (size_t i = 0; i < incoming.size(); ++i) {
                auto bytes = std::span(table->entries);
                const auto at = table->offsets[i] + layout.entrySize - sizeof(Counters);
                const auto old = load<Counters>(bytes, at);
                store(bytes, at, Counters{old.packets + incoming[i].packets, old.bytes + incoming[i].bytes});
            }
            return 0;
        }

        Replace request{};
        if (x[4] < sizeof(request)) return invalid;
        if (!read(machine, x[3], request)) return fault;
        if (request.hooks != table->hooks || x[4] != sizeof(request) + uint64_t(request.size) ||
            request.count > maxEntries || !request.previousCount) return invalid;
        if (request.previousCount != table->offsets.size()) return uint64_t(-11);
        Table replacement{request.hooks, request.starts, request.policies, std::vector<uint8_t>(request.size), {}};
        if (!machine.readInto(x[3] + sizeof(request), replacement.entries)) return fault;
        const auto status = validate(replacement, layout, request.count, string(name));
        if (status) return status;

        std::vector<Counters> previous;
        previous.reserve(table->offsets.size());
        for (const auto at : table->offsets)
            previous.push_back(load<Counters>(table->entries, at + layout.entrySize - sizeof(Counters)));
        if (!previous.empty() && !machine.write(request.previous, writableBytes(previous))) return fault;
        *table = std::move(replacement);
        return 0;
    }

    bool empty() const { return tables.empty(); }
    bool tracksMarks() const {
        return std::ranges::any_of(tables, [](const auto& entry) { return entry.second.usesMarks; });
    }

    bool allows(std::vector<uint8_t>& packet, Hook hook, uint32_t mtu, PacketState state) {
        if (tables.empty()) return true;
        if (packet.size() < 20) return false;
        const bool ipv6 = packet[0] >> 4 == 6;
        if (ipv6 && packet.size() < 40) return false;
        const Layout layout(ipv6);
        for (const auto name : {"raw", "mangle", "nat", "filter"}) {
            const auto table = tables.find({ipv6, name});
            if (table == tables.end() || !(table->second.hooks & (1U << hook))) continue;
            auto visible = state;
            if (std::string_view(name) == "raw") visible.connectionMark = nullptr;
            if (!evaluate(table->second, layout, packet, hook, mtu, visible)) return false;
        }
        return true;
    }

private:
    static constexpr uint64_t fault = uint64_t(-14), invalid = uint64_t(-22), missing = uint64_t(-2),
                              unsupported = uint64_t(-92);
    static constexpr size_t maxEntries = 4096, maxTableBytes = 1024 * 1024, targetHeader = 32;
    static constexpr int32_t drop = -1, accept = -2, returnToCaller = -5;

    struct Layout {
        bool ipv6;
        size_t entrySize, ipSize, flags, inversions;
        explicit Layout(bool v6)
            : ipv6(v6), entrySize(v6 ? 168 : 112), ipSize(v6 ? 136 : 84),
              flags(v6 ? 131 : 82), inversions(v6 ? 132 : 83) {}
        size_t targetOffset() const { return ipSize + 4; }
        size_t nextOffset() const { return ipSize + 6; }
    };
    using Positions = std::array<uint32_t, 5>;
    struct Info {
        std::array<char, 32> name;
        uint32_t hooks;
        Positions starts, policies;
        uint32_t count, size;
    };
    struct Replace {
        std::array<char, 32> name;
        uint32_t hooks, count, size;
        Positions starts, policies;
        uint32_t previousCount;
        uint64_t previous;
    };
    struct Counters { uint64_t packets, bytes; };
    static_assert(sizeof(Info) == 84 && sizeof(Replace) == 96 && sizeof(Counters) == 16);

    struct Table {
        uint32_t hooks;
        Positions starts{}, policies{};
        std::vector<uint8_t> entries;
        std::vector<uint32_t> offsets;
        bool usesMarks = false;
    };
    std::map<std::pair<bool, std::string>, Table> tables;

    static std::string_view string(std::span<const char> bytes) {
        const auto end = std::ranges::find(bytes, '\0');
        return end == bytes.end() ? std::string_view{} : std::string_view(bytes.data(), end - bytes.begin());
    }
    static std::string_view string(std::span<const uint8_t> bytes) {
        return string({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    }
    template<class T>
    static T load(std::span<const uint8_t> bytes, size_t offset) {
        T result{};
        std::memcpy(&result, bytes.data() + offset, sizeof(result));
        return result;
    }
    template<class T>
    static void store(std::span<uint8_t> bytes, size_t offset, const T& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    template<class T>
    static bool read(const Machine& machine, uint64_t at, T& value) {
        return machine.readInto(at, {reinterpret_cast<uint8_t*>(&value), sizeof(T)});
    }
    template<class T>
    static bool write(Machine& machine, uint64_t at, const T& value) {
        return machine.write(at, {reinterpret_cast<const uint8_t*>(&value), sizeof(T)});
    }
    template<class T>
    static std::span<uint8_t> writableBytes(std::vector<T>& values) {
        return {reinterpret_cast<uint8_t*>(values.data()), values.size() * sizeof(T)};
    }

    Table* lookup(bool ipv6, std::string_view name) {
        uint32_t hooks = 0;
        if (name == "filter") hooks = 0x0e;
        else if (name == "raw") hooks = 0x09;
        else if (name == "mangle") hooks = 0x1f;
        else if (name == "nat" && !ipv6) hooks = 0x1b;
        else return nullptr;
        auto [node, fresh] = tables.try_emplace({ipv6, std::string(name)}, Table{hooks, {}, {}, {}, {}});
        auto& table = node->second;
        if (!fresh) return &table;
        const Layout layout(ipv6);
        const auto add = [&](bool tail) {
            const auto at = table.entries.size();
            const uint16_t targetSize = tail ? 64 : 40, size = layout.entrySize + targetSize;
            table.offsets.push_back(at);
            table.entries.resize(at + size);
            auto entry = std::span(table.entries).subspan(at);
            store(entry, layout.targetOffset(), uint16_t(layout.entrySize));
            store(entry, layout.nextOffset(), size);
            store(entry, layout.entrySize, targetSize);
            if (tail) {
                std::memcpy(entry.data() + layout.entrySize + 2, "ERROR", 5);
                std::memcpy(entry.data() + layout.entrySize + targetHeader, "ERROR", 5);
            } else {
                store(entry, layout.entrySize + targetHeader, accept);
            }
        };
        for (unsigned h = 0; h < 5; ++h) {
            if (!(hooks & (1U << h))) continue;
            table.starts[h] = table.policies[h] = table.entries.size();
            add(false);
        }
        add(true);
        return &table;
    }

    static uint64_t validate(Table& table, const Layout& layout, uint32_t count, std::string_view name) {
        std::vector<uint32_t> jumps;
        auto bytes = std::span(table.entries);
        for (size_t at = 0; at < bytes.size();) {
            if (table.offsets.size() == count || bytes.size() - at < layout.entrySize) return invalid;
            auto entry = bytes.subspan(at);
            const auto targetAt = load<uint16_t>(entry, layout.targetOffset()), end = load<uint16_t>(entry, layout.nextOffset());
            if (targetAt < layout.entrySize || targetAt % 8 || end % 8 ||
                end < size_t(targetAt) + targetHeader || end > entry.size()) return invalid;
            if ((entry[layout.flags] & ~(layout.ipv6 ? 5 : 2)) || (entry[layout.inversions] & ~0x5b)) return unsupported;
            const auto protocol = load<uint16_t>(entry, layout.ipv6 ? 128 : 80);
            bool synMatch = false;
            for (size_t match = layout.entrySize; match < targetAt;) {
                if (targetAt - match < targetHeader) return invalid;
                const auto extension = entry.subspan(match, targetAt - match);
                const auto length = load<uint16_t>(extension, 0);
                if (length < targetHeader || length % 8 || length > extension.size()) return invalid;
                const auto matchName = string(extension.subspan(2, 29));
                if (matchName == "connmark") {
                    if (extension[31] != 1) return unsupported;
                    if (length != 48 || extension[40] > 1) return invalid;
                    table.usesMarks = true;
                    match += length;
                    continue;
                }
                if (extension[31]) return missing;
                if (matchName == "comment") {
                    if (length != 288) return invalid;
                } else if (matchName == "tcp") {
                    if (length != 48 || protocol != 6 || (entry[layout.inversions] & 0x40) ||
                        (extension[43] & ~15)) return invalid;
                    synMatch |= (extension[41] & 2) && (extension[42] & 2) && !(extension[43] & 4);
                } else if (matchName == "udp") {
                    if (length != 48 || protocol != 17 || (entry[layout.inversions] & 0x40) ||
                        (extension[40] & ~3)) return invalid;
                } else {
                    return missing;
                }
                match += length;
            }

            const auto target = entry.subspan(targetAt, end - targetAt);
            const auto length = load<uint16_t>(target, 0);
            if (length != target.size()) return invalid;
            if (target[2] == 0) {
                if (length != 40 || target[31]) return invalid;
                const auto verdict = load<int32_t>(target, targetHeader);
                if (verdict >= 0) jumps.push_back(verdict);
                else if (verdict != drop && verdict != accept && verdict != returnToCaller) return unsupported;
            } else {
                const auto targetName = string(target.subspan(2, 29));
                if (targetName == "ERROR") {
                    if (length != 64 || target[31] || string(target.subspan(targetHeader, 30)).empty()) return invalid;
                } else if (targetName == "TCPMSS") {
                    if (length != 40 || target[31] || name != "mangle" || protocol != 6 || !synMatch) return invalid;
                } else if (targetName == "CONNMARK") {
                    if (length != 48 || (target[31] != 1 && target[31] != 2)) return invalid;
                    const bool shift = target[31] == 2;
                    if (target[shift ? 46 : 44] > 2 || (shift && (target[44] > 1 || target[45] > 31)))
                        return invalid;
                    table.usesMarks = true;
                } else {
                    return missing;
                }
            }
            table.offsets.push_back(at);
            store(entry, layout.entrySize - sizeof(Counters), Counters{});
            at += end;
        }
        if (!count || table.offsets.size() != count) return invalid;
        for (const auto jump : jumps)
            if (!std::ranges::binary_search(table.offsets, jump)) return invalid;
        for (unsigned h = 0; h < 5; ++h) {
            if (!(table.hooks & (1U << h))) continue;
            if (!std::ranges::binary_search(table.offsets, table.starts[h]) ||
                !std::ranges::binary_search(table.offsets, table.policies[h])) return invalid;
            const auto policy = bytes.subspan(table.policies[h]);
            const auto target = load<uint16_t>(policy, layout.targetOffset());
            if (target != layout.entrySize || policy[target + 2] ||
                std::ranges::any_of(policy.first(layout.ipSize), [](uint8_t byte) { return byte != 0; }))
                return invalid;
            const auto verdict = load<int32_t>(policy, target + targetHeader);
            if (verdict != accept && verdict != drop) return invalid;
        }
        return 0;
    }

    static bool matches(std::span<const uint8_t> entry, const Layout& layout,
                        std::span<const uint8_t> packet, Hook hook, PacketState state) {
        const size_t ipBytes = layout.ipv6 ? 16 : 4;
        const size_t ipOffset = layout.ipv6 ? 8 : 12;
        const auto inversions = entry[layout.inversions];
        const auto equal = [&](std::span<const uint8_t> value, size_t key, size_t mask, unsigned reverse) {
            bool same = true;
            for (size_t b = 0; b < value.size(); ++b)
                same &= ((value[b] ^ entry[key + b]) & entry[mask + b]) == 0;
            return same != bool(inversions & reverse);
        };
        if (!equal(packet.subspan(ipOffset, ipBytes), 0, 2 * ipBytes, 8) ||
            !equal(packet.subspan(ipOffset + ipBytes, ipBytes), ipBytes, 3 * ipBytes, 16)) return false;
        constexpr std::array<uint8_t, 16> loopback{'l', 'o'}, empty{};
        const size_t interfaces = 4 * ipBytes;
        const auto& in = hook == input || hook == prerouting ? loopback : empty;
        const auto& out = hook == output || hook == postrouting ? loopback : empty;
        if (!equal(in, interfaces, interfaces + 32, 1) || !equal(out, interfaces + 16, interfaces + 48, 2)) return false;
        const auto protocol = load<uint16_t>(entry, interfaces + 64);
        if ((!protocol || protocol == packet[layout.ipv6 ? 6 : 9]) == bool(inversions & 0x40)) return false;
        const auto targetAt = load<uint16_t>(entry, layout.targetOffset());
        for (size_t match = layout.entrySize; match < targetAt;) {
            const auto extension = entry.subspan(match);
            const auto type = string(extension.subspan(2, 29));
            const auto body = extension.subspan(targetHeader);
            if (type == "connmark") {
                if (!state.connectionMark) return false;
                const bool same = (*state.connectionMark & load<uint32_t>(body, 4)) == load<uint32_t>(body, 0);
                if (same == bool(body[8])) return false;
            } else if (type != "comment" && !matchTransport(type, body, packet, layout.ipv6)) {
                return false;
            }
            match += load<uint16_t>(extension, 0);
        }
        return true;
    }

    static uint16_t network16(std::span<const uint8_t> bytes, size_t offset) {
        return uint16_t(uint16_t(bytes[offset]) << 8 | bytes[offset + 1]);
    }
    static void setNetwork16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
        bytes[offset] = uint8_t(value >> 8);
        bytes[offset + 1] = uint8_t(value);
    }
    static uint32_t checksum(std::span<const uint8_t> bytes, uint32_t sum = 0) {
        size_t i = 0;
        for (; i + 1 < bytes.size(); i += 2) sum += network16(bytes, i);
        if (i < bytes.size()) sum += uint32_t(bytes[i]) << 8;
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        return sum;
    }
    static std::span<const uint8_t> transport(std::span<const uint8_t> packet, bool ipv6) {
        const size_t offset = ipv6 ? 40 : size_t(packet[0] & 15) * 4;
        return offset <= packet.size() && offset >= (ipv6 ? 40 : 20) ? packet.subspan(offset) :
            std::span<const uint8_t>{};
    }

    static bool matchTransport(std::string_view type, std::span<const uint8_t> match,
                               std::span<const uint8_t> packet, bool ipv6) {
        const bool tcp = type == "tcp";
        const auto segment = transport(packet, ipv6);
        const auto inverse = match[tcp ? 11 : 8];
        if (segment.size() < (tcp ? 20 : 8)) return false;
        for (unsigned field = 0; field < 2; ++field) {
            const auto port = network16(segment, 2 * field);
            const bool inRange = port >= load<uint16_t>(match, 4 * field) &&
                                 port <= load<uint16_t>(match, 4 * field + 2);
            if (inRange == bool(inverse & (1 << field))) return false;
        }
        if (!tcp) return true;
        if (((segment[13] & match[9]) == match[10]) == bool(inverse & 4)) return false;
        if (!match[8]) return true;
        const auto end = size_t(segment[12] >> 4) * 4;
        if (end < 20 || end > segment.size()) return false;
        for (size_t option = 20; option < end;) {
            if (segment[option] == match[8]) return !(inverse & 8);
            if (segment[option] == 0) break;
            if (segment[option] == 1) { ++option; continue; }
            if (end - option < 2 || segment[option + 1] < 2 || segment[option + 1] > end - option) return false;
            option += segment[option + 1];
        }
        return inverse & 8;
    }

    static bool applyMss(std::vector<uint8_t>& packet, bool ipv6, uint16_t requested, uint32_t mtu) {
        const auto segment = transport(packet, ipv6);
        if (segment.size() < 20) return false;
        if (!(segment[13] & 2)) return true;
        const auto end = size_t(segment[12] >> 4) * 4;
        if (end < 20 || end > segment.size()) return false;
        const size_t ipHeader = packet.size() - segment.size();
        if (requested == 0xffff && mtu <= ipHeader + 20) return false;
        uint16_t mss = requested == 0xffff ? std::min<size_t>(mtu - ipHeader - 20, 65535) : requested;
        size_t data = 0;
        for (size_t option = 20; option < end;) {
            if (segment[option] == 0) break;
            if (segment[option] == 1) { ++option; continue; }
            if (end - option < 2 || segment[option + 1] < 2 || segment[option + 1] > end - option) return false;
            if (segment[option] == 2 && segment[option + 1] == 4) {
                if (network16(segment, option + 2) <= mss) return true;
                data = ipHeader + option + 2;
                break;
            }
            option += segment[option + 1];
        }
        if (!data) {
            if (segment.size() != end || end == 60 || packet.size() > 65531) return true;
            mss = std::min<uint16_t>(mss, ipv6 ? 1220 : 536);
            packet.insert(packet.begin() + ipHeader + 20, {2, 4, 0, 0});
            packet[ipHeader + 12] += 0x10;
            data = ipHeader + 22;
        }
        setNetwork16(packet, data, mss);
        auto tcp = std::span(packet).subspan(ipHeader);
        if (ipv6) {
            setNetwork16(packet, 4, tcp.size());
        } else {
            setNetwork16(packet, 2, packet.size());
            setNetwork16(packet, 10, 0);
            setNetwork16(packet, 10, uint16_t(~checksum(std::span(packet).first(ipHeader))));
        }
        setNetwork16(tcp, 16, 0);
        const auto addresses = std::span(packet).subspan(ipv6 ? 8 : 12, ipv6 ? 32 : 8);
        setNetwork16(tcp, 16, uint16_t(~checksum(tcp, checksum(addresses, 6 + tcp.size()))));
        return true;
    }

    static void applyConnectionMark(std::span<const uint8_t> data, uint8_t version, PacketState state) {
        if (!state.connectionMark) return;
        const auto ctmark = load<uint32_t>(data, 0);
        const auto ctmask = load<uint32_t>(data, 4);
        const auto nfmask = load<uint32_t>(data, 8);
        const bool shifted = version == 2;
        const unsigned width = shifted ? data[13] : 0;
        const auto shift = [&](uint32_t value) {
            return shifted && data[12] ? value >> width : value << width;
        };
        const auto mode = data[shifted ? 14 : 12];
        auto& connection = *state.connectionMark;
        switch (mode) {
            case 0: connection = shift((connection & ~ctmask) ^ ctmark); break;
            case 1: connection = (connection & ~ctmask) ^ shift((state.mark ? *state.mark : 0) & nfmask); break;
            case 2:
                if (state.mark) *state.mark = (*state.mark & ~nfmask) ^ shift(connection & ctmask);
                break;
        }
    }

    static bool evaluate(Table& table, const Layout& layout, std::vector<uint8_t>& packet, Hook hook,
                         uint32_t mtu, PacketState state) {
        std::array<uint32_t, 64> stack{};
        size_t depth = 0;
        size_t at = table.starts[hook];
        for (size_t fuel = maxEntries; fuel && at < table.entries.size(); --fuel) {
            auto entry = std::span(table.entries).subspan(at);
            const size_t next = at + load<uint16_t>(entry, layout.nextOffset());
            const auto target = load<uint16_t>(entry, layout.targetOffset());
            if (!matches(entry, layout, packet, hook, state)) { at = next; continue; }
            const size_t counter = layout.entrySize - sizeof(Counters);
            const auto previous = load<Counters>(entry, counter);
            store(entry, counter, Counters{previous.packets + 1, previous.bytes + packet.size()});
            if (entry[target + 2]) {
                const auto type = string(entry.subspan(target + 2, 29));
                const auto parameters = entry.subspan(target + targetHeader);
                if (type == "CONNMARK") {
                    applyConnectionMark(parameters, entry[target + 31], state);
                } else if (type != "TCPMSS" || !applyMss(packet, layout.ipv6, load<uint16_t>(parameters, 0), mtu)) {
                    return false;
                }
                at = next;
                continue;
            }
            const auto verdict = load<int32_t>(entry, target + targetHeader);
            if (verdict == accept || verdict == drop) return verdict == accept;
            if (verdict == returnToCaller) {
                at = depth ? stack[--depth] : table.policies[hook];
                continue;
            }
            const auto goTo = entry[layout.flags] & (layout.ipv6 ? 4 : 2);
            if (!goTo && size_t(verdict) != next) {
                if (depth == stack.size()) return false;
                stack[depth++] = next;
            }
            at = size_t(verdict);
        }
        return false;
    }
};

} // namespace nestvane
