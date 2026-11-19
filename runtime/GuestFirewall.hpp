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
            if (option != (ipv6 ? 68 : 66) || name != "comment" || revision.back()) return missing;
            return 0;
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
        const auto status = validate(replacement, layout, request.count);
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

    bool allows(std::span<const uint8_t> packet, Hook hook) {
        if (tables.empty()) return true;
        if (packet.size() < 20) return false;
        const bool ipv6 = packet[0] >> 4 == 6;
        if (ipv6 && packet.size() < 40) return false;
        const Layout layout(ipv6);
        for (const auto name : {"raw", "mangle", "nat", "filter"}) {
            const auto table = tables.find({ipv6, name});
            if (table == tables.end() || !(table->second.hooks & (1U << hook))) continue;
            if (!evaluate(table->second, layout, packet, hook)) return false;
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

    static uint64_t validate(Table& table, const Layout& layout, uint32_t count) {
        std::vector<uint32_t> jumps;
        auto bytes = std::span(table.entries);
        for (size_t at = 0; at < bytes.size();) {
            if (table.offsets.size() == count || bytes.size() - at < layout.entrySize) return invalid;
            auto entry = bytes.subspan(at);
            const auto targetAt = load<uint16_t>(entry, layout.targetOffset()), end = load<uint16_t>(entry, layout.nextOffset());
            if (targetAt < layout.entrySize || targetAt % 8 || end % 8 ||
                end < size_t(targetAt) + targetHeader || end > entry.size()) return invalid;
            if ((entry[layout.flags] & ~(layout.ipv6 ? 5 : 2)) || (entry[layout.inversions] & ~0x5b)) return unsupported;
            for (size_t match = layout.entrySize; match < targetAt;) {
                if (targetAt - match < targetHeader) return invalid;
                const auto extension = entry.subspan(match, targetAt - match);
                const auto length = load<uint16_t>(extension, 0);
                if (length < targetHeader || length % 8 || length > extension.size()) return invalid;
                if (string(extension.subspan(2, 29)) != "comment" || extension[31] || length != 288)
                    return missing;
                match += length;
            }

            const auto target = entry.subspan(targetAt, end - targetAt);
            const auto length = load<uint16_t>(target, 0);
            if (length != target.size() || target[31]) return invalid;
            if (target[2] == 0) {
                if (length != 40) return invalid;
                const auto verdict = load<int32_t>(target, targetHeader);
                if (verdict >= 0) jumps.push_back(verdict);
                else if (verdict != drop && verdict != accept && verdict != returnToCaller) return unsupported;
            } else {
                if (string(target.subspan(2, 29)) != "ERROR") return missing;
                if (length != 64 || !target[targetHeader] || string(target.subspan(targetHeader, 30)).empty()) return invalid;
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
                        std::span<const uint8_t> packet, Hook hook) {
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
        return (!protocol || protocol == packet[layout.ipv6 ? 6 : 9]) != bool(inversions & 0x40);
    }

    static bool evaluate(Table& table, const Layout& layout, std::span<const uint8_t> packet, Hook hook) {
        std::array<uint32_t, 64> stack{};
        size_t depth = 0;
        size_t at = table.starts[hook];
        for (size_t fuel = maxEntries; fuel && at < table.entries.size(); --fuel) {
            auto entry = std::span(table.entries).subspan(at);
            const size_t next = at + load<uint16_t>(entry, layout.nextOffset());
            const auto target = load<uint16_t>(entry, layout.targetOffset());
            if (!matches(entry, layout, packet, hook)) { at = next; continue; }
            const size_t counter = layout.entrySize - sizeof(Counters);
            const auto previous = load<Counters>(entry, counter);
            store(entry, counter, Counters{previous.packets + 1, previous.bytes + packet.size()});
            if (entry[target + 2]) return false; // Chain label or end-of-table marker.
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
