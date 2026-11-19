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
    static constexpr uint32_t socketStorage = 24, ringBuffer = 27;
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

    struct CompareKeys {
        using is_transparent = void;
        bool operator()(std::span<const uint8_t> left, std::span<const uint8_t> right) const {
            return std::ranges::lexicographical_compare(left, right);
        }
    };
    std::map<std::vector<uint8_t>, std::vector<uint8_t>, CompareKeys> hash;
    // Descriptor duplicates share the same socket owner. Closing the last socket FD
    // leaves an expired weak key, which is reaped before using the map again.
    std::map<std::weak_ptr<void>, std::vector<uint8_t>, std::owner_less<void>> socketValues;
};

class BpfProgram {
public:
    static constexpr uint32_t socketFilter = 1, classifier = 3;
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
        bool ringNotification = false;
    };

    bool load(uint32_t type, std::span<const uint8_t> bytes, const MapResolver& resolve) {
        if ((type != socketFilter && type != classifier) || bytes.empty() ||
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
                    if (type != classifier || upper.immediate) return false;
                    auto map = resolve(uint32_t(i.immediate));
                    if (!map || map->type == 25) return false;
                    maps.emplace(pc, std::move(map));
                    decoded[pc].immediate = 0;
                } else if (i.source() == 4) { // BPF_PSEUDO_FUNC
                    const auto target = int64_t(pc) + 1 + i.immediate;
                    if (type != classifier || upper.immediate || target <= 0 ||
                        uint64_t(target) >= decoded.size() || !decoded[target].code) return false;
                    entries.emplace(target);
                } else if (i.source()) {
                    return false;
                }
                ++pc;
            } else if (i.code == 0x85) {
                if (i.registers || i.offset) return false;
                switch (i.immediate) {
                    case 26: break; // skb_load_bytes
                    case 1: case 9: case 10: case 11: case 23: case 31: case 39: case 40: case 43: case 46: case 50: case 125: case 130: case 131:
                    case 132: case 133: case 134: case 164:
                        if (type != classifier) return false;
                        break;
                    default: return false;
                }
            } else if (i.code == 0x95) {
                if (i.registers || i.offset || i.immediate) return false;
            } else if (i.code == 0xd4 || i.code == 0xdc) { // BPF_END
                if (i.destination() == 10 || i.source() || i.offset ||
                    (i.immediate != 16 && i.immediate != 32 && i.immediate != 64)) return false;
            } else if (i.kind() == 5) {
                if (!jump(i.operation(), 0, 0).has_value() || (i.operation() == 0 && i.code != 0x05))
                    return false;
                if ((!i.fromRegister() && i.source()) || (i.fromRegister() && i.immediate) ||
                    (i.code == 0x05 && (i.registers || i.immediate))) return false;
                const auto target = int64_t(pc) + 1 + i.offset;
                if (target < 0 || uint64_t(target) >= decoded.size() || !decoded[target].code) return false;
            } else if (i.kind() == 7) {
                if (i.destination() == 10 || i.offset || (!i.fromRegister() && i.source()) ||
                    (i.fromRegister() && i.immediate) || !arithmetic(i.operation(), 0, 0).has_value()) return false;
                if (!i.fromRegister() && (i.operation() == 0x60 || i.operation() == 0x70 ||
                    i.operation() == 0xc0) && uint32_t(i.immediate) >= 64) return false;
            } else if (i.kind() == 1 || i.kind() == 3) {
                if (i.kind() == 1 && i.destination() == 10) return false;
                if (i.mode() != 0x60 && (type != classifier || i.kind() != 3 || i.mode() != 0xc0 ||
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
        if (programType != classifier || packet.bytes.size() > maxPacketBytes) return std::nullopt;
        return execute(packet.bytes, &packet);
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
            case 0x40: return left | right;
            case 0x50: return left & right;
            case 0x60: return left << (right & 63);
            case 0x70: return left >> (right & 63);
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

    std::optional<uint32_t> execute(std::span<const uint8_t> packet, Packet* classifierPacket) const {
        // Interpreter addresses identify bounded packet, stack and map regions. The guest never
        // receives host addresses. Guest execution is serialized, so an atomic ADD needs no lock.
        constexpr size_t maxDepth = 8;
        constexpr uint64_t stackBase = 0x1000, stackStride = 0x1000, context = 0xf000, packetBase = 0x10000;
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
        const auto slice = [](auto memory, uint64_t base, uint64_t address, uint64_t size) -> decltype(memory) {
            if (address < base || address - base > memory.size() || size > memory.size() - (address - base))
                return {};
            return memory.subspan(address - base, size);
        };
        auto spanAt = [&](uint64_t address, uint64_t size, bool write, size_t depth) -> std::span<uint8_t> {
            if (address >= mapValueBase) {
                const auto index = (address - mapValueBase) / mapStride;
                if (index >= values.size() || (write && !values[index].writable)) return {};
                return slice(values[index].bytes, mapValueBase + index * mapStride, address, size);
            }
            if (address >= packetBase && classifierPacket)
                return slice(std::span<uint8_t>(classifierPacket->bytes), packetBase, address, size);
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
                const auto found = map.hash.find(key);
                if (found == map.hash.end()) return 0;
                value = std::span(found->second).first(map.valueSize);
            }
            return valueAddress(map, value);
        };

        auto skbField = [&](uint64_t offset) -> std::optional<uint32_t> {
            if (offset == 0) return uint32_t(packet.size()); // __sk_buff.len
            if (!classifierPacket) return std::nullopt;
            switch (offset) {
                case 4: return classifierPacket->packetType;
                case 8: return classifierPacket->mark;
                case 16: return classifierPacket->protocol;
                case 36: return classifierPacket->ingressIfindex;
                case 40: return classifierPacket->ifindex;
                case 76: return uint32_t(packetBase);
                case 80: return uint32_t(packetBase + packet.size());
                case 164: case 176: return 0; // No segmentation offload on the linear test packet.
                default: return std::nullopt;
            }
        };

        size_t fuel = classifierPacket ? 65536 : 16384;
        auto run = [&](auto&& self, size_t start, size_t depth,
                       std::array<uint64_t, 11> registers) -> std::optional<uint32_t> {
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
                        const auto r1 = registers[1], r2 = registers[2], r3 = registers[3];
                        uint64_t result = 0;
                        switch (i.immediate) {
                            case 1: result = lookup(r1, r2, depth); break;
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
                            case 26: {
                                const auto offset = uint32_t(r2), length = uint32_t(registers[4]);
                                const auto to = write(r3, length);
                                if (r1 != context || to.empty()) return std::nullopt;
                                if (offset > packet.size() || length > packet.size() - offset) {
                                    std::ranges::fill(to, 0);
                                    result = uint64_t(-14);
                                } else {
                                    std::memmove(to.data(), packet.data() + offset, length);
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
                                result = classifierPacket->socketCookie;
                                break;
                            case 50: // bpf_skb_adjust_room
                                if (r1 != context) return std::nullopt;
                                result = adjustRoom(*classifierPacket, int32_t(r2), uint32_t(r3), registers[4]);
                                packet = classifierPacket->bytes;
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
                                    for (auto& [key, value] : map->hash) {
                                        const auto action = invoke(key, std::span(value).first(map->valueSize));
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
                    case 0x95: return uint32_t(registers[0]);
                    default: break;
                }

                switch (i.kind()) {
                    case 1: {
                        const auto address = src + i.offset;
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
        return run(run, 0, 0, {0, context});
    }

    Identity identity;
    DebugInfo debug;
    uint32_t programType = 0;
    std::vector<Instruction> instructions;
    std::set<size_t> functionEntries;
    std::map<size_t, std::shared_ptr<BpfMap>> referencedMaps;
};

} // namespace nestvane
