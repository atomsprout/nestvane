#pragma once

#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nestvane {

class BpfTypes;

struct BpfMap {
    static constexpr uint32_t lruHash = 9, prefixMap = 11, socketStorage = 24, ringBuffer = 27;
    static constexpr size_t minPrefixKey = 5, maxPrefixKey = sizeof(uint32_t) + 256;
    explicit BpfMap(bool prefixes = false) : hash(CompareKeys{prefixes}) {}
    static constexpr uint32_t ringDiscardBit = 1U << 30, ringBusyBit = 1U << 31;
    uint32_t id = 0, type = 0, flags = 0, entryCount = 0;
    uint32_t keySize = 0, valueSize = 0, valueStride = 0, btfKeyType = 0, btfValueType = 0;
    std::array<char, 16> name{};
    size_t budgetBytes = 0;
    bool array = false;
    std::shared_ptr<const BpfTypes> types;
    SharedFileBytes values;
    std::span<uint8_t> arrayElement(uint32_t index) {
        return std::span(values.pages).subspan(uint64_t(index) * valueStride, valueStride);
    }
    uint64_t ringPosition(size_t offset) const {
        uint64_t position;
        std::memcpy(&position, values.pages.data() + offset, sizeof(position));
        return position;
    }
    bool ringReadable() const { return ringPosition(pageSize) != ringPosition(0); }

    struct RingRecord {
        uint64_t position;
        std::span<uint8_t> payload;
    };

    std::optional<RingRecord> reserveRingRecord(uint64_t size) {
        const auto position = ringPosition(pageSize);
        const auto used = position - ringPosition(0);
        if (size >= entryCount) return {};
        const auto length = (size + 15) & ~uint64_t{7};
        if (used >= entryCount || length >= entryCount - used) return {};
        auto* record = ringData(position);
        const auto header = uint32_t(size) | ringBusyBit;
        std::memcpy(record, &header, sizeof(header));
        std::memset(record + 4, 0, 4);
        auto payload = std::span(record + 8, size);
        // Reused payload bytes never become readable to another guest program.
        std::ranges::fill(payload, 0);
        const auto next = position + length;
        std::memcpy(values.pages.data() + pageSize, &next, sizeof(next));
        return RingRecord{position, payload};
    }

    bool finishRingRecord(RingRecord record, bool discard, uint64_t flags) {
        const auto header = uint32_t(record.payload.size()) | (discard ? ringDiscardBit : 0);
        auto* data = ringData(record.position);
        std::memcpy(data, &header, sizeof(header));
        // The backing reserves budget for a second read-only copy so a guest load
        // can read a wrapping record through either view in a single operation.
        const auto offset = record.position & (entryCount - 1);
        const size_t length = (record.payload.size() + 15) & ~size_t{7};
        const size_t first = std::min<size_t>(length, entryCount - offset);
        std::memcpy(data + entryCount, data, first);
        if (first < length) std::memcpy(ringData(0), data + first, length - first);
        return (flags & 2) || (!(flags & 1) && record.position == ringPosition(0));
    }

    uint8_t* ringData(uint64_t position) {
        return values.pages.data() + 2 * pageSize + (position & (entryCount - 1));
    }

    static uint32_t prefixLength(std::span<const uint8_t> key) {
        uint32_t prefix;
        std::memcpy(&prefix, key.data(), sizeof(prefix));
        return prefix;
    }

    static bool validPrefix(std::span<const uint8_t> key) {
        return key.size() >= minPrefixKey && prefixLength(key) <= 8 * (key.size() - sizeof(uint32_t));
    }

    struct CompareKeys {
        using is_transparent = void;
        bool prefixes;
        bool operator()(std::span<const uint8_t> left, std::span<const uint8_t> right) const {
            if (!prefixes) return std::ranges::lexicographical_compare(left, right);
            const auto leftLength = prefixLength(left), rightLength = prefixLength(right);
            const auto common = std::min(leftLength, rightLength);
            const auto bytes = common / 8, bits = common % 8;
            left = left.subspan(sizeof(uint32_t));
            right = right.subspan(sizeof(uint32_t));
            const auto order = std::memcmp(left.data(), right.data(), bytes);
            if (order) return order < 0;
            if (bits) {
                const auto shift = 8 - bits;
                const auto a = left[bytes] >> shift, b = right[bytes] >> shift;
                if (a != b) return a < b;
            }
            // Linux traverses left, then right, then the matching parent prefix.
            return leftLength > rightLength;
        }
    };
    struct Entry {
        std::vector<uint8_t> bytes;
        uint64_t lastUsed = 0;
    };
    using Entries = std::map<std::vector<uint8_t>, Entry, CompareKeys>;
    Entries hash;
    uint64_t recency = 0;

    void markUsed(Entry& entry) {
        if (type == lruHash) entry.lastUsed = ++recency;
    }

    void storeHash(std::vector<uint8_t> key, std::vector<uint8_t> value, Entries::iterator previous,
                   std::vector<uint8_t>* retired = nullptr) {
        Entry item{std::move(value)};
        markUsed(item);
        if (type == prefixMap && previous != hash.end()) {
            // Enumeration reports the entire most recently supplied key, including its suffix.
            auto node = hash.extract(previous);
            std::ranges::copy(key, node.key().begin());
            if (retired) *retired = std::move(node.mapped().bytes);
            node.mapped() = std::move(item);
            hash.insert(std::move(node));
            return;
        }
        if (previous != hash.end()) {
            if (retired) *retired = std::move(previous->second.bytes);
            previous->second = std::move(item);
            return;
        }
        auto victim = hash.end();
        if (type == lruHash && previous == hash.end() && hash.size() == entryCount) {
            victim = std::min_element(hash.begin(), hash.end(), [](const auto& left, const auto& right) {
                return left.second.lastUsed < right.second.lastUsed;
            });
        }
        hash.emplace(std::move(key), std::move(item));
        // Failure to allocate a new entry leaves the existing cache intact.
        if (victim != hash.end()) {
            if (retired) *retired = std::move(victim->second.bytes);
            hash.erase(victim);
        }
    }

    Entries::iterator findValue(std::span<const uint8_t> key) {
        if (type != prefixMap) return hash.find(key);
        if (hash.empty() || !validPrefix(key)) return hash.end();
        std::array<uint8_t, maxPrefixKey> query;
        std::ranges::copy(key, query.begin());
        const auto candidate = std::span(query).first(key.size());
        for (auto prefix = prefixLength(key);; --prefix) {
            std::memcpy(query.data(), &prefix, sizeof(prefix));
            const auto found = hash.find(candidate);
            if (found != hash.end() || !prefix) return found;
        }
    }

    // Descriptor duplicates share the same socket owner. Closing the last socket FD
    // leaves an expired weak key, which is reaped before using the map again.
    std::map<std::weak_ptr<void>, std::vector<uint8_t>, std::owner_less<void>> socketValues;
};

class BpfProgram {
public:
    static constexpr uint32_t socketFilter = 1, classifier = 3, trafficAction = 4, cgroupPacket = 8, socketOperations = 13;
    static constexpr uint32_t cgroupSocketOperations = 3;
    static constexpr size_t maxInstructions = 4096, maxPacketBytes = 65535;
    using MapResolver = std::function<std::shared_ptr<BpfMap>(uint32_t)>;

    struct Identity {
        uint32_t id = 0, uid = 0;
        uint64_t loadTime = 0;
        std::array<char, 16> name{};
        bool gplCompatible = false;
    };

    using FunctionInfo = std::array<uint32_t, 2>;
    using LineInfo = std::array<uint32_t, 4>;
    struct DebugInfo {
        std::shared_ptr<const BpfTypes> types;
        std::vector<FunctionInfo> functions;
        std::vector<LineInfo> lines;
    };

    BpfProgram(Identity identity, DebugInfo debug) : identity(identity), debug(std::move(debug)) {}

    struct Packet {
        std::vector<uint8_t> bytes;
        uint32_t protocol = 0, packetType = 0, ingressIfindex = 0, ifindex = 0, mark = 0;
        size_t networkOffset = 0;
        uint64_t socketCookie = 0;
        uint32_t socketUid = 65534, socketMark = 0;
        bool ringNotification = false;
        bool hasMacHeader = false;
        std::shared_ptr<void> socket;
        // The guest transport resolves exact TCP/UDP tuples in its own namespace.
        std::function<std::shared_ptr<void>(std::span<const uint8_t>, bool)> socketLookup;
    };

    struct SocketOperations {
        static constexpr uint32_t tcpConnect = 3, passiveEstablished = 5, optionLength = 14, writeOption = 15;
        static constexpr uint32_t allCallbackFlags = 0x7f;

        std::shared_ptr<void> socket;
        uint32_t operation, callbackFlags = 0, tcpFlags = 0;
        std::array<uint8_t, 40> options{};
        size_t used = 0, remaining = options.size();

        uint64_t reserveOption(uint32_t length, uint64_t flags) {
            if (flags || !length) return uint64_t(-22);
            if (operation != optionLength) return uint64_t(-1);
            if (length > remaining) return uint64_t(-28);
            remaining -= length;
            return 0;
        }

        uint64_t storeOption(std::span<const uint8_t> data, uint64_t flags) {
            if (flags || data.size() < 2 || data[0] < 2 || data[0] >= 253 ||
                data[1] < 2 || data[1] > data.size()) return uint64_t(-22);
            if (operation != writeOption) return uint64_t(-1);
            const size_t length = data[1];
            if (length > remaining) return uint64_t(-28);
            for (size_t offset = 0; offset < used;) {
                const auto kind = options[offset];
                if (!kind) break;
                if (kind == 1) { ++offset; continue; }
                if (used - offset < 2 || options[offset + 1] < 2 || options[offset + 1] > used - offset)
                    return uint64_t(-14);
                if (kind == data[0]) return uint64_t(-17);
                offset += options[offset + 1];
            }
            std::ranges::copy(data.first(length), options.begin() + used);
            used += length;
            remaining -= length;
            return 0;
        }
    };

    static bool trafficControl(uint32_t type) { return type == classifier || type == trafficAction; }
    static bool packetProgram(uint32_t type) { return trafficControl(type) || type == cgroupPacket; }
    static bool supports(uint32_t type) {
        return type == socketFilter || packetProgram(type) || type == socketOperations;
    }

    bool load(uint32_t type, std::span<const uint8_t> bytes, const MapResolver& resolve) {
        if (!supports(type) || bytes.empty() ||
            bytes.size() % sizeof(Instruction) || bytes.size() / sizeof(Instruction) > maxInstructions)
            return false;
        std::vector<Instruction> decoded(bytes.size() / sizeof(Instruction));
        std::memcpy(decoded.data(), bytes.data(), bytes.size());
        std::map<size_t, std::shared_ptr<BpfMap>> maps;
        std::set<size_t> entries{0};
        for (size_t pc = 0; pc < decoded.size(); ++pc) {
            const auto& i = decoded[pc];
            if (i.destination() > 10 || i.source() > 10) return false;
            if (i.code == 0x18) { // The second slot carries the high half of the immediate.
                if (i.destination() == 10 || i.offset || pc + 1 >= decoded.size()) return false;
                const auto& upper = decoded[pc + 1];
                if (upper.code || upper.registers || upper.offset) return false;
                if (i.source() == 1) { // BPF_PSEUDO_MAP_FD
                    if (upper.immediate) return false;
                    auto map = resolve(uint32_t(i.immediate));
                    if (!map || map->type == 25) return false;
                    maps.emplace(pc, std::move(map));
                    decoded[pc].immediate = 0;
                } else if (i.source() == 4) { // BPF_PSEUDO_FUNC
                    const auto target = int64_t(pc) + 1 + i.immediate;
                    if (!packetProgram(type) || upper.immediate || target <= 0 ||
                        uint64_t(target) >= decoded.size() || !decoded[target].code) return false;
                    entries.emplace(target);
                } else if (i.source()) {
                    return false;
                }
                ++pc;
            } else if (i.code == 0x85 && i.source() == 1) { // BPF_PSEUDO_CALL
                const auto target = int64_t(pc) + 1 + i.immediate;
                if (type == socketFilter || i.destination() || i.offset || target <= 0 ||
                    uint64_t(target) >= decoded.size() || !decoded[target].code) return false;
                entries.emplace(target);
            } else if (i.code == 0x85) {
                if (i.registers || i.offset) return false;
                switch (i.immediate) {
                    case 26: // skb_load_bytes
                        if (type == socketOperations) return false;
                        break;
                    case 1: break; // bpf_map_lookup_elem
                    case 2: case 46: case 47: // Map updates and socket identity.
                        if (type == socketOperations) return false;
                        break;
                    case 107:
                        if (type == socketFilter) return false;
                        break;
                    case 59: case 143: case 144:
                        if (type != socketOperations) return false;
                        break;
                    case 84: case 85: case 86:
                        if (type != cgroupPacket) return false;
                        break;
                    case 68: case 95: case 125: case 130: case 131:
                    case 132: case 133: case 134: case 164:
                        if (!packetProgram(type)) return false;
                        break;
                    case 9: case 10: case 11: case 23: case 31: case 39: case 40: case 43: case 50:
                        if (!trafficControl(type)) return false;
                        break;
                    default: return false;
                }
            } else if (i.code == 0x95) {
                if (i.registers || i.offset || i.immediate) return false;
            } else if (i.code == 0xd4 || i.code == 0xdc) { // BPF_END
                if (i.destination() == 10 || i.source() || i.offset ||
                    (i.immediate != 16 && i.immediate != 32 && i.immediate != 64)) return false;
            } else if (i.kind() == 0 && (i.mode() == 0x20 || i.mode() == 0x40)) {
                // Legacy absolute and indirect packet loads use r6 as their implicit skb.
                if ((type != socketFilter && !trafficControl(type)) || i.destination() || i.offset || i.width() == 8 ||
                    (i.mode() == 0x20 && i.source())) return false;
            } else if (i.kind() == 5) {
                if (!jump(i.operation(), 0, 0).has_value() || (i.operation() == 0 && i.code != 0x05))
                    return false;
                if ((!i.fromRegister() && i.source()) || (i.fromRegister() && i.immediate) ||
                    (i.code == 0x05 && (i.registers || i.immediate))) return false;
                const auto target = int64_t(pc) + 1 + i.offset;
                if (target < 0 || uint64_t(target) >= decoded.size() || !decoded[target].code) return false;
            } else if (i.kind() == 7) {
                if (i.destination() == 10 || i.offset || (!i.fromRegister() && i.source()) ||
                    (i.operation() == 0x80 && (i.fromRegister() || i.immediate)) ||
                    (i.fromRegister() && i.immediate) || !arithmetic(i.operation(), 0, 0).has_value()) return false;
                if (!i.fromRegister() && (i.operation() == 0x60 || i.operation() == 0x70 ||
                    i.operation() == 0xc0) && uint32_t(i.immediate) >= 64) return false;
            } else if (i.kind() == 1 || i.kind() == 3) {
                if (i.kind() == 1 && i.destination() == 10) return false;
                if (i.mode() != 0x60 && (type == socketOperations || i.kind() != 3 || i.mode() != 0xc0 ||
                    (i.width() != 4 && i.width() != 8))) return false;
                if (i.immediate) return false; // The only supported atomic operation is ADD without fetch.
            } else {
                return false;
            }
        }
        if ((decoded.back().code != 0x95 && decoded.back().code != 0x05) ||
            std::none_of(decoded.begin(), decoded.end(), [](const auto& i) { return i.code == 0x95; })) return false;
        if (entries.size() > 1 && debug.functions.empty()) return false;
        if (!debug.functions.empty() &&
            !std::ranges::equal(entries, debug.functions, {}, std::identity{}, [](const auto& info) { return info[0]; }))
            return false;
        instructions = std::move(decoded);
        functionEntries = std::move(entries);
        referencedMaps = std::move(maps);
        programType = type;
        return true;
    }

    uint32_t type() const { return programType; }
    const Identity& info() const { return identity; }
    const DebugInfo& debugInfo() const { return debug; }
    std::span<const uint8_t> bytecode() const {
        return {reinterpret_cast<const uint8_t*>(instructions.data()), instructions.size() * sizeof(Instruction)};
    }
    std::vector<uint32_t> mapIds() const {
        std::vector<uint32_t> ids;
        for (const auto& [slot, map] : referencedMaps)
            if (std::ranges::find(ids, map->id) == ids.end()) ids.push_back(map->id);
        return ids;
    }

    size_t accept(std::span<const uint8_t> packet) const {
        if (programType != socketFilter) return 0;
        return std::min<size_t>(execute(packet, nullptr).value_or(0), packet.size());
    }

    std::optional<uint32_t> classify(Packet& packet) const {
        if ((!packetProgram(programType) && programType != socketFilter) || packet.bytes.size() > maxPacketBytes ||
            packet.networkOffset > packet.bytes.size()) return std::nullopt;
        return execute(packet.bytes, &packet);
    }

    std::optional<uint32_t> runSocketOperations(SocketOperations& socket) const {
        if (programType != socketOperations || socket.used > socket.options.size() ||
            socket.remaining > socket.options.size() - socket.used) return std::nullopt;
        return execute({}, nullptr, &socket);
    }

private:
    struct Instruction {
        uint8_t code, registers;
        int16_t offset;
        int32_t immediate;
        unsigned destination() const { return registers & 15; }
        unsigned source() const { return registers >> 4; }
        unsigned kind() const { return code & 7; }
        unsigned operation() const { return code & 0xf0; }
        bool fromRegister() const { return code & 8; }
        unsigned mode() const { return code & 0xe0; }
        size_t width() const {
            constexpr std::array<size_t, 4> widths{4, 2, 1, 8};
            return widths[(code >> 3) & 3];
        }
    };
    static_assert(sizeof(Instruction) == 8);

    static std::optional<uint64_t> arithmetic(unsigned operation, uint64_t left, uint64_t right) {
        switch (operation) {
            case 0x00: return left + right;
            case 0x10: return left - right;
            case 0x20: return left * right;
            case 0x30: return right ? left / right : 0;
            case 0x40: return left | right;
            case 0x50: return left & right;
            case 0x60: return left << (right & 63);
            case 0x70: return left >> (right & 63);
            case 0x80: return uint64_t{0} - left;
            case 0xa0: return left ^ right;
            case 0xb0: return right;
            case 0xc0: return uint64_t(int64_t(left) >> (right & 63));
            default: return std::nullopt;
        }
    }

    static std::optional<bool> jump(unsigned operation, uint64_t left, uint64_t right) {
        switch (operation) {
            case 0x00: return true;
            case 0x10: return left == right;
            case 0x20: return left > right;
            case 0x30: return left >= right;
            case 0x40: return (left & right) != 0;
            case 0x50: return left != right;
            case 0x60: return int64_t(left) > int64_t(right);
            case 0x70: return int64_t(left) >= int64_t(right);
            case 0xa0: return left < right;
            case 0xb0: return left <= right;
            case 0xc0: return int64_t(left) < int64_t(right);
            case 0xd0: return int64_t(left) <= int64_t(right);
            default: return std::nullopt;
        }
    }

    static uint64_t changeByteOrder(uint64_t value, unsigned bits, bool bigEndian) {
        switch (bits) {
            case 16: return bigEndian ? __builtin_bswap16(uint16_t(value)) : uint16_t(value);
            case 32: return bigEndian ? __builtin_bswap32(uint32_t(value)) : uint32_t(value);
            default: return bigEndian ? __builtin_bswap64(value) : value;
        }
    }

    static uint32_t addChecksum(uint32_t left, uint32_t right) {
        const auto sum = left + right;
        return sum + (sum < right);
    }

    static uint16_t foldChecksum(uint32_t sum) {
        sum = (sum & UINT16_MAX) + (sum >> 16);
        return uint16_t(~(sum + (sum >> 16)));
    }

    static uint64_t replaceChecksum(std::span<uint8_t> packet, uint32_t offset, uint64_t from, uint64_t to,
                                    uint64_t flags, bool transport) {
        constexpr uint64_t sizeMask = 15, markMangledZero = 32, enforce = 64;
        constexpr uint64_t transportFlags = 16 | markMangledZero | enforce;
        if (flags & ~(sizeMask | (transport ? transportFlags : 0))) return uint64_t(-22);
        if (offset > UINT16_MAX || (offset & 1) || offset > packet.size() || packet.size() - offset < 2)
            return uint64_t(-14);

        uint16_t original = 0;
        std::memcpy(&original, packet.data() + offset, sizeof(original));
        if ((flags & markMangledZero) && !(flags & enforce) && !original) return 0;
        uint32_t delta;
        switch (flags & sizeMask) {
            case 0:
                if (from) return uint64_t(-22);
                delta = uint32_t(to);
                break;
            case 2:
                delta = uint16_t(~from) + uint32_t(uint16_t(to));
                break;
            case 4:
                delta = addChecksum(~uint32_t(from), uint32_t(to));
                break;
            default: return uint64_t(-22);
        }
        auto updated = foldChecksum(addChecksum(~uint32_t(original), delta));
        if ((flags & markMangledZero) && !updated) updated = UINT16_MAX;
        std::memcpy(packet.data() + offset, &updated, sizeof(updated));
        return 0;
    }

    // Protocol values follow __sk_buff.protocol, which is in network byte order.
    static constexpr uint16_t ipv4 = 0x0008, ipv6 = 0xdd86;

    static uint64_t changeProtocol(Packet& packet, uint16_t protocol, uint64_t flags) {
        constexpr size_t extension = 20; // sizeof(ipv6hdr) - sizeof(iphdr)
        if (flags) return uint64_t(-22);
        const auto layer3 = packet.networkOffset;
        const auto size = packet.bytes.size();
        if (layer3 > size) return uint64_t(-22);
        if (packet.protocol == ipv4 && protocol == ipv6) {
            if (extension > maxPacketBytes - size) return uint64_t(-12);
            packet.bytes.insert(packet.bytes.begin() + layer3, extension, 0);
        } else if (packet.protocol == ipv6 && protocol == ipv4) {
            if (extension > size - layer3) return uint64_t(-12);
            packet.bytes.erase(packet.bytes.begin() + layer3, packet.bytes.begin() + layer3 + extension);
        } else {
            return uint64_t(-524);
        }
        // eBPF rewrites the actual L2 and L3 headers after this helper has resized the packet.
        packet.protocol = protocol;
        return 0;
    }

    static uint64_t adjustRoom(Packet& packet, int32_t adjustment, uint32_t mode, uint64_t flags) {
        constexpr uint64_t fixedGso = 1, noChecksumReset = 1 << 5;
        if (flags & ~(fixedGso | noChecksumReset)) return uint64_t(-95); // No tunnel or offload metadata.
        if (adjustment < -4095 || adjustment > 4095) return uint64_t(-14);
        if (mode > 1 || (packet.protocol != ipv4 && packet.protocol != ipv6)) return uint64_t(-524);
        const size_t header = packet.protocol == ipv4 ? 20 : 40;
        const size_t size = packet.bytes.size(), layer3 = packet.networkOffset;
        if (layer3 > size) return uint64_t(-22);
        const size_t offset = layer3 + (mode == 0 ? header : 0); // NET or MAC
        if (offset > size) return uint64_t(-12);
        if (adjustment < 0) {
            const size_t remove = -adjustment;
            if (remove > size - layer3 || size - layer3 - remove < header) return uint64_t(-524);
            packet.bytes.erase(packet.bytes.begin() + offset, packet.bytes.begin() + offset + remove);
        } else {
            if (size_t(adjustment) > maxPacketBytes - size) return uint64_t(-524);
            packet.bytes.insert(packet.bytes.begin() + offset, adjustment, 0);
        }
        return 0;
    }

    struct Region {
        std::span<uint8_t> bytes;
        bool writable;
        BpfMap* ring = nullptr;
        uint64_t position = 0;
    };

    std::optional<uint32_t> execute(std::span<const uint8_t> packet, Packet* classifierPacket,
                                    SocketOperations* sockOps = nullptr) const {
        // Interpreter addresses identify bounded packet, stack and map regions. The guest never
        // receives host addresses. Guest execution is serialized, so an atomic ADD needs no lock.
        constexpr size_t maxDepth = 8;
        constexpr uint64_t stackBase = 0x1000, stackStride = 0x1000, context = 0xf000, packetBase = 0x10000;
        constexpr uint64_t socketHandle = 0xe000;
        constexpr uint64_t callbackBase = 1ULL << 29, keyBase = 1ULL << 30;
        constexpr uint64_t mapHandleBase = 1ULL << 31, mapValueBase = 1ULL << 32, mapStride = 1ULL << 26;
        std::array<std::array<uint8_t, 512>, maxDepth> stacks{};
        std::array<std::span<const uint8_t>, maxDepth> callbackKeys{};
        std::vector<Region> values;
        struct DiscardPending {
            std::vector<Region>& regions;
            Packet* packet;
            ~DiscardPending() {
                for (auto& region : regions) {
                    if (region.ring)
                        packet->ringNotification |= region.ring->finishRingRecord({region.position, region.bytes}, true, 0);
                }
            }
        } discardPending{values, classifierPacket};
        std::unordered_map<const uint8_t*, size_t> valueRegions;
        std::vector<std::vector<uint8_t>> retiredValues;
        size_t retiredBytes = 0;
        const auto slice = [](auto memory, uint64_t base, uint64_t address, uint64_t size) -> decltype(memory) {
            if (address < base || address - base > memory.size() || size > memory.size() - (address - base))
                return {};
            return memory.subspan(address - base, size);
        };
        const bool networkView = classifierPacket && (programType == cgroupPacket || programType == socketFilter);
        const auto dataOffset = networkView ? classifierPacket->networkOffset : 0;
        const auto packetData = [&] { return packet.subspan(dataOffset); };
        auto spanAt = [&](uint64_t address, uint64_t size, bool write, size_t depth) -> std::span<uint8_t> {
            if (address >= mapValueBase) {
                const auto index = (address - mapValueBase) / mapStride;
                if (index >= values.size() || (write && !values[index].writable)) return {};
                return slice(values[index].bytes, mapValueBase + index * mapStride, address, size);
            }
            if (address >= packetBase && classifierPacket)
                return slice(std::span<uint8_t>(classifierPacket->bytes).subspan(dataOffset), packetBase, address, size);
            if (address < stackBase) return {};
            const auto frame = (address - stackBase) / stackStride;
            if (frame > depth) return {};
            return slice(std::span<uint8_t>(stacks[frame]), stackBase + frame * stackStride, address, size);
        };
        const auto readAt = [&](uint64_t address, uint64_t size, size_t depth) -> std::span<const uint8_t> {
            if (address >= keyBase && address < keyBase + maxDepth * mapStride) {
                const auto index = (address - keyBase) / mapStride;
                if (index >= depth) return {};
                return slice(callbackKeys[index], keyBase + index * mapStride, address, size);
            }
            return spanAt(address, size, false, depth);
        };
        const auto mapFor = [&](uint64_t handle) -> BpfMap* {
            if (handle < mapHandleBase || handle - mapHandleBase >= instructions.size()) return nullptr;
            const auto entry = referencedMaps.find(handle - mapHandleBase);
            return entry == referencedMaps.end() ? nullptr : entry->second.get();
        };
        const auto valueAddress = [&](const BpfMap& map, std::span<uint8_t> value) {
            const auto [region, inserted] = valueRegions.try_emplace(value.data(), values.size());
            if (inserted) values.push_back({value, !(map.flags & (1U << 7))});
            return mapValueBase + region->second * mapStride;
        };

        auto lookup = [&](uint64_t handle, uint64_t keyAddress, size_t depth) -> uint64_t {
            auto* foundMap = mapFor(handle);
            if (!foundMap || foundMap->type == BpfMap::socketStorage || foundMap->type == BpfMap::ringBuffer) return 0;
            auto& map = *foundMap;
            const auto key = readAt(keyAddress, map.keySize, depth);
            if (key.empty()) return 0;
            std::span<uint8_t> value;
            if (map.array) {
                uint32_t index = 0;
                std::memcpy(&index, key.data(), sizeof(index));
                if (index >= map.entryCount) return 0;
                value = map.arrayElement(index).first(map.valueSize);
            } else {
                const auto found = map.findValue(key);
                if (found == map.hash.end()) return 0;
                map.markUsed(found->second);
                value = std::span(found->second.bytes).first(map.valueSize);
            }
            return valueAddress(map, value);
        };

        const auto update = [&](BpfMap* map, std::span<const uint8_t> key,
                                std::span<const uint8_t> value, uint64_t flags) -> uint64_t {
            if (flags > 2) return uint64_t(-22);
            if (map->flags & (1U << 7)) return uint64_t(-1);
            if (map->array) {
                uint32_t index = 0;
                std::memcpy(&index, key.data(), sizeof(index));
                if (index >= map->entryCount) return uint64_t(-7);
                if (flags == 1) return uint64_t(-17);
                std::memmove(map->arrayElement(index).data(), value.data(), value.size());
                return 0;
            }
            if (map->type == BpfMap::prefixMap && !map->validPrefix(key)) return uint64_t(-22);
            const auto found = map->hash.find(key);
            const bool exists = found != map->hash.end();
            if (exists && flags == 1) return uint64_t(-17);
            if (!exists && flags == 2) return uint64_t(-2);
            const bool full = !exists && map->hash.size() == map->entryCount;
            if (full && map->type != BpfMap::lruHash)
                return map->type == BpfMap::prefixMap ? uint64_t(-28) : uint64_t(-7);
            // A loaded program may retain a pointer obtained before a map update.
            // Keep replaced and evicted values alive until this invocation ends.
            constexpr size_t maxRetiredBytes = 2 * 1024 * 1024;
            if ((exists || full) && map->valueStride > maxRetiredBytes - retiredBytes) return uint64_t(-12);
            try {
                std::vector<uint8_t>* retired = nullptr;
                if (exists || full) {
                    retiredValues.emplace_back();
                    retired = &retiredValues.back();
                }
                std::vector<uint8_t> replacement(map->valueStride);
                std::ranges::copy(value, replacement.begin());
                if (exists && map->valueStride > value.size())
                    std::copy(found->second.bytes.begin() + value.size(), found->second.bytes.end(),
                              replacement.begin() + value.size());
                map->storeHash({key.begin(), key.end()}, std::move(replacement), found, retired);
                if (retired) retiredBytes += retired->size();
                return 0;
            } catch (const std::bad_alloc&) {
                return uint64_t(-12);
            }
        };

        const auto* socket = sockOps ? &sockOps->socket : classifierPacket ? &classifierPacket->socket : nullptr;
        const bool hasSocket = socket && *socket;
        constexpr uint64_t socketStride = 256;
        std::array<std::shared_ptr<void>, 15> socketReferences{};
        size_t nextSocketReference = 0;
        const auto socketReference = [&](uint64_t handle) -> std::shared_ptr<void>* {
            if (handle <= socketHandle || (handle - socketHandle) % socketStride) return nullptr;
            const auto index = (handle - socketHandle) / socketStride - 1;
            return index < socketReferences.size() ? &socketReferences[index] : nullptr;
        };
        auto skbField = [&](uint64_t offset) -> std::optional<uint32_t> {
            if (offset == 0) return uint32_t(packetData().size()); // __sk_buff.len
            if (!classifierPacket) return std::nullopt;
            switch (offset) {
                case 4: return classifierPacket->packetType;
                case 8: return classifierPacket->mark;
                case 16: return classifierPacket->protocol;
                case 36: return classifierPacket->ingressIfindex;
                case 40: return classifierPacket->ifindex;
                case 76: case 80:
                    if (programType == socketFilter) return std::nullopt;
                    return uint32_t(packetBase + (offset == 80 ? packetData().size() : 0));
                case 164: case 176: return 0; // No segmentation offload on the linear test packet.
                default: return std::nullopt;
            }
        };

        size_t fuel = programType == socketFilter ? 16384 : 65536;
        auto run = [&](auto&& self, size_t start, size_t depth,
                       std::array<uint64_t, 11> registers) -> std::optional<uint64_t> {
            if (depth >= maxDepth) return std::nullopt;
            auto& stack = stacks[depth];
            stack.fill(0);
            registers[10] = stackBase + depth * stackStride + stack.size();
            const auto read = [&](uint64_t address, uint64_t size) { return readAt(address, size, depth); };
            const auto write = [&](uint64_t address, uint64_t size) { return spanAt(address, size, true, depth); };
            const auto next = functionEntries.upper_bound(start);
            const auto end = next == functionEntries.end() ? instructions.size() : *next;
            size_t pc = start;
            while (fuel && pc >= start && pc < end) {
                --fuel;
                const auto& i = instructions[pc++];
                auto& dst = registers[i.destination()];
                const auto src = registers[i.source()];
                const auto imm = uint64_t(int64_t(i.immediate));
                const auto operand = i.fromRegister() ? src : imm;
                switch (i.code) {
                    case 0x20: case 0x28: case 0x30: case 0x40: case 0x48: case 0x50: {
                        if (registers[6] != context) return std::nullopt;
                        const auto offset = uint32_t(i.immediate) + (i.mode() == 0x40 ? uint32_t(src) : 0);
                        const auto width = i.width();
                        const auto data = packetData();
                        if (offset > data.size() || width > data.size() - offset) return 0;
                        uint32_t value = 0;
                        for (const auto byte : data.subspan(offset, width)) value = (value << 8) | byte;
                        registers[0] = value;
                        std::fill_n(registers.begin() + 1, 5, 0);
                        continue;
                    }
                    case 0xd4: case 0xdc:
                        dst = changeByteOrder(dst, i.immediate, i.code == 0xdc);
                        continue;
                    case 0x18:
                        if (i.source() == 1) dst = mapHandleBase + pc - 1;
                        else if (i.source() == 4) dst = callbackBase + pc + i.immediate;
                        else dst = uint32_t(imm) | (uint64_t(uint32_t(instructions[pc].immediate)) << 32);
                        ++pc;
                        continue;
                    case 0x85: {
                        if (i.source() == 1) {
                            auto arguments = registers;
                            arguments[0] = 0;
                            const auto called = self(self, int64_t(pc) + i.immediate, depth + 1, arguments);
                            if (!called) return std::nullopt;
                            registers[0] = *called;
                            std::fill_n(registers.begin() + 1, 5, 0);
                            continue;
                        }
                        const auto r1 = registers[1], r2 = registers[2], r3 = registers[3];
                        uint64_t result = 0;
                        switch (i.immediate) {
                            case 1: result = lookup(r1, r2, depth); break;
                            case 2: {
                                auto* map = mapFor(r1);
                                if (!map || map->type == BpfMap::socketStorage || map->type == BpfMap::ringBuffer)
                                    return std::nullopt;
                                const auto key = read(r2, map->keySize), value = read(r3, map->valueSize);
                                if (key.empty() || value.empty()) return std::nullopt;
                                result = update(map, key, value, registers[4]);
                                break;
                            }
                            case 9: { // bpf_skb_store_bytes
                                if (r1 != context) return std::nullopt;
                                const auto offset = uint32_t(r2), length = uint32_t(registers[4]);
                                if (registers[5] & ~3ULL) { result = uint64_t(-22); break; }
                                const auto source = read(r3, length);
                                if (offset > packet.size() || length > packet.size() - offset || (length && source.empty())) {
                                    result = uint64_t(-14);
                                    break;
                                }
                                // This packet has no checksum-offload or cached hash metadata to update.
                                if (length) std::memmove(classifierPacket->bytes.data() + offset, source.data(), length);
                                break;
                            }
                            case 10: case 11: // bpf_l3_csum_replace and bpf_l4_csum_replace
                                if (r1 != context) return std::nullopt;
                                result = replaceChecksum(classifierPacket->bytes, uint32_t(r2), r3, registers[4],
                                                         registers[5], i.immediate == 11);
                                break;
                            case 23: // bpf_redirect returns a TC action; test-run never transmits the frame.
                                result = r2 & ~1ULL ? 2 : 7; // TC_ACT_SHOT or TC_ACT_REDIRECT
                                break;
                            case 26: case 68: { // skb_load_bytes and skb_load_bytes_relative
                                const auto offset = uint32_t(r2), length = uint32_t(registers[4]);
                                const auto output = write(r3, length);
                                if (r1 != context || output.empty()) return std::nullopt;
                                auto data = packetData();
                                if (i.immediate == 68) {
                                    const auto header = uint32_t(registers[5]);
                                    if (header == 1)
                                        data = packet.subspan(classifierPacket->networkOffset);
                                    else if (header == 0 && classifierPacket->hasMacHeader)
                                        data = packet;
                                    else
                                        data = {};
                                }
                                if (offset > data.size() || length > data.size() - offset) {
                                    std::ranges::fill(output, 0);
                                    result = uint64_t(-14);
                                } else {
                                    std::memmove(output.data(), data.data() + offset, length);
                                }
                                break;
                            }
                            case 31: // bpf_skb_change_proto
                                if (r1 != context) return std::nullopt;
                                result = changeProtocol(*classifierPacket, uint16_t(r2), r3);
                                packet = classifierPacket->bytes;
                                break;
                            case 39: // The guest packet is already linear.
                                if (r1 != context) return std::nullopt;
                                if (uint32_t(r2) > packet.size()) result = uint64_t(-12);
                                break;
                            case 40: // Test-run supplies a CHECKSUM_NONE packet.
                                if (r1 != context) return std::nullopt;
                                result = uint64_t(-524); // ENOTSUPP
                                break;
                            case 43: {
                                if (r1 != context) return std::nullopt;
                                const auto headroom = uint32_t(r2);
                                if (r3 || headroom > maxPacketBytes - packet.size()) {
                                    result = uint64_t(-22);
                                } else {
                                    classifierPacket->bytes.insert(classifierPacket->bytes.begin(), headroom, 0);
                                    classifierPacket->networkOffset += headroom;
                                    packet = classifierPacket->bytes;
                                }
                                break;
                            }
                            case 46: // bpf_get_socket_cookie
                                if (r1 != context) return std::nullopt;
                                result = classifierPacket ? classifierPacket->socketCookie : 0;
                                break;
                            case 47: // bpf_get_socket_uid
                                if (r1 != context) return std::nullopt;
                                result = classifierPacket ? classifierPacket->socketUid : 65534;
                                break;
                            case 84: case 85: { // bpf_sk_lookup_tcp / bpf_sk_lookup_udp
                                if (r1 != context) return std::nullopt;
                                if ((r3 != 12 && r3 != 36) || int64_t(registers[4]) >= 0 || registers[5]) break;
                                const auto tuple = read(r2, r3);
                                if (tuple.empty()) return std::nullopt;
                                if (!classifierPacket->socketLookup || nextSocketReference >= socketReferences.size()) break;
                                auto reference = classifierPacket->socketLookup(tuple, i.immediate == 84);
                                if (reference) {
                                    socketReferences[nextSocketReference++] = std::move(reference);
                                    result = socketHandle + nextSocketReference * socketStride;
                                }
                                break;
                            }
                            case 86: { // bpf_sk_release
                                auto* reference = socketReference(r1);
                                if (!reference || !*reference) return std::nullopt;
                                reference->reset();
                                break;
                            }
                            case 95: // bpf_sk_fullsock
                                if (r1 == socketHandle && hasSocket) result = socketHandle;
                                break;
                            case 50: // bpf_skb_adjust_room
                                if (r1 != context) return std::nullopt;
                                result = adjustRoom(*classifierPacket, int32_t(r2), uint32_t(r3), registers[4]);
                                packet = classifierPacket->bytes;
                                break;
                            case 59: // bpf_sock_ops_cb_flags_set
                                if (r1 != context) return std::nullopt;
                                if (!sockOps->socket) { result = uint64_t(-22); break; }
                                sockOps->callbackFlags = uint32_t(r2) & SocketOperations::allCallbackFlags;
                                result = uint32_t(r2) & ~SocketOperations::allCallbackFlags;
                                break;
                            case 107: { // bpf_sk_storage_get for an existing socket entry.
                                auto* map = mapFor(r1);
                                if (!map || map->type != BpfMap::socketStorage || r3 || registers[4]) return std::nullopt;
                                const auto* owner = r2 == socketHandle ? socket : socketReference(r2);
                                if (!owner || !*owner) break;
                                const auto found = map->socketValues.find(*owner);
                                if (found != map->socketValues.end()) result = valueAddress(*map, found->second);
                                break;
                            }
                            case 143: { // bpf_store_hdr_opt
                                if (r1 != context) return std::nullopt;
                                const auto data = read(r2, uint32_t(r3));
                                if (r3 && data.empty()) return std::nullopt;
                                result = sockOps->storeOption(data, registers[4]);
                                break;
                            }
                            case 144: // bpf_reserve_hdr_opt
                                if (r1 != context) return std::nullopt;
                                result = sockOps->reserveOption(uint32_t(r2), r3);
                                break;
                            case 125: // bpf_ktime_get_boot_ns uses the guest's CLOCK_BOOTTIME clock.
                                result = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                                break;
                            case 130: { // bpf_ringbuf_output
                                auto* ring = mapFor(r1);
                                const auto flags = registers[4];
                                if (!ring || ring->type != BpfMap::ringBuffer) return std::nullopt;
                                if (flags & ~3ULL) { result = uint64_t(-22); break; }
                                const auto data = read(r2, r3);
                                if (r3 && data.empty()) return std::nullopt;
                                const auto record = ring->reserveRingRecord(r3);
                                if (!record) { result = uint64_t(-11); break; }
                                std::ranges::copy(data, record->payload.begin());
                                classifierPacket->ringNotification |= ring->finishRingRecord(*record, false, flags);
                                break;
                            }
                            case 131: { // bpf_ringbuf_reserve
                                auto* ring = mapFor(r1);
                                if (!ring || ring->type != BpfMap::ringBuffer) return std::nullopt;
                                if (r3) break;
                                values.push_back({});
                                const auto record = ring->reserveRingRecord(r2);
                                if (!record) { values.pop_back(); break; }
                                values.back() = {record->payload, true, ring, record->position};
                                result = mapValueBase + (values.size() - 1) * mapStride;
                                break;
                            }
                            case 132: case 133: { // bpf_ringbuf_submit and bpf_ringbuf_discard
                                if (r1 < mapValueBase || (r1 - mapValueBase) % mapStride) return std::nullopt;
                                const auto index = (r1 - mapValueBase) / mapStride;
                                if (index >= values.size() || !values[index].ring) return std::nullopt;
                                auto& region = values[index];
                                classifierPacket->ringNotification |= region.ring->finishRingRecord(
                                    {region.position, region.bytes}, i.immediate == 133, r2);
                                region = {};
                                break;
                            }
                            case 134: { // bpf_ringbuf_query
                                auto* ring = mapFor(r1);
                                if (!ring || ring->type != BpfMap::ringBuffer) return std::nullopt;
                                if (r2 == 0) result = ring->ringPosition(pageSize) - ring->ringPosition(0);
                                if (r2 == 1) result = ring->entryCount;
                                if (r2 == 2) result = ring->ringPosition(0);
                                if (r2 == 3) result = ring->ringPosition(pageSize);
                                break;
                            }
                            case 164: { // bpf_for_each_map_elem
                                auto* map = mapFor(r1);
                                if (registers[4]) { result = uint64_t(-22); break; }
                                if (!map || map->type == BpfMap::socketStorage || map->type == BpfMap::ringBuffer ||
                                    r2 <= callbackBase || r2 - callbackBase >= instructions.size() ||
                                    !functionEntries.contains(r2 - callbackBase)) return std::nullopt;
                                auto invoke = [&](std::span<const uint8_t> key, std::span<uint8_t> value) {
                                    callbackKeys[depth] = key;
                                    auto action = self(self, r2 - callbackBase, depth + 1,
                                                       {0, r1, keyBase + depth * mapStride,
                                                        valueAddress(*map, value), r3});
                                    callbackKeys[depth] = {};
                                    ++result;
                                    if (action && *action > 1) action.reset();
                                    return action;
                                };
                                if (map->array) {
                                    for (uint32_t index = 0; index < map->entryCount; ++index) {
                                        const auto key = std::span(reinterpret_cast<const uint8_t*>(&index), sizeof(index));
                                        const auto value = map->arrayElement(index).first(map->valueSize);
                                        const auto action = invoke(key, value);
                                        if (!action) return std::nullopt;
                                        if (*action) break;
                                    }
                                } else {
                                    std::vector<std::vector<uint8_t>> keys;
                                    keys.reserve(map->hash.size());
                                    for (const auto& entry : map->hash) keys.push_back(entry.first);
                                    for (const auto& key : keys) {
                                        const auto entry = map->hash.find(key);
                                        if (entry == map->hash.end()) continue;
                                        const auto action = invoke(key, std::span(entry->second.bytes).first(map->valueSize));
                                        if (!action) return std::nullopt;
                                        if (*action) break;
                                    }
                                }
                                break;
                            }
                            default: return std::nullopt;
                        }
                        registers[0] = result;
                        std::fill_n(registers.begin() + 1, 5, 0);
                        continue;
                    }
                    case 0x95: return registers[0];
                    default: break;
                }

                switch (i.kind()) {
                    case 1: {
                        const auto address = src + i.offset;
                        if (sockOps && address >= context && address < context + 216) {
                            const auto offset = address - context;
                            if (i.width() == 8 && offset == 184) dst = hasSocket ? socketHandle : 0;
                            else if (i.width() != 4) return std::nullopt;
                            else if (offset == 0) dst = sockOps->operation;
                            else if (offset == 84) dst = sockOps->callbackFlags;
                            else if (offset == 212) dst = sockOps->tcpFlags;
                            else return std::nullopt;
                            break;
                        }
                        if (classifierPacket && hasSocket && i.width() == 4 && address == socketHandle + 12) {
                            dst = classifierPacket->socketMark;
                            break;
                        }
                        if (classifierPacket && i.width() == 8 && address == context + 168) {
                            dst = hasSocket ? socketHandle : 0; // __sk_buff.sk
                            break;
                        }
                        if (i.width() == 4 && address >= context && address < context + 180) {
                            const auto field = skbField(address - context);
                            if (!field) return std::nullopt;
                            dst = *field;
                            break;
                        }
                        const auto from = read(address, i.width());
                        if (from.empty()) return std::nullopt;
                        dst = 0;
                        std::memcpy(&dst, from.data(), from.size());
                        break;
                    }
                    case 3: {
                        const auto address = dst + i.offset;
                        if (classifierPacket && i.code == 0x63 && address == context + 8) {
                            classifierPacket->mark = uint32_t(src);
                            break;
                        }
                        const auto to = write(address, i.width());
                        if (to.empty()) return std::nullopt;
                        uint64_t value = src;
                        if (i.mode() == 0xc0) {
                            if (address % to.size() || address < mapValueBase) return std::nullopt;
                            uint64_t previous = 0;
                            std::memcpy(&previous, to.data(), to.size());
                            value += previous;
                        }
                        std::memcpy(to.data(), &value, to.size());
                        break;
                    }
                    case 5: if (*jump(i.operation(), dst, operand)) pc += i.offset; break;
                    case 7: dst = *arithmetic(i.operation(), dst, operand); break;
                    default: return std::nullopt;
                }
            }
            return std::nullopt;
        };
        const auto result = run(run, 0, 0, {0, context});
        return result ? std::optional{uint32_t(*result)} : std::nullopt;
    }

    Identity identity;
    DebugInfo debug;
    uint32_t programType = 0;
    std::vector<Instruction> instructions;
    std::set<size_t> functionEntries;
    std::map<size_t, std::shared_ptr<BpfMap>> referencedMaps;
};

} // namespace nestvane
