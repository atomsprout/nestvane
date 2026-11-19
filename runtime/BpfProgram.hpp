#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nestvane {

class BpfTypes;

struct BpfMap {
    uint32_t id = 0, type = 0, flags = 0, entryCount = 0;
    uint32_t keySize = 0, valueSize = 0, valueStride = 0, btfKeyType = 0, btfValueType = 0;
    std::array<char, 16> name{};
    size_t budgetBytes = 0;
    bool array = false;
    std::shared_ptr<const BpfTypes> types;
    std::vector<uint8_t> values;
    struct CompareKeys {
        using is_transparent = void;
        bool operator()(std::span<const uint8_t> left, std::span<const uint8_t> right) const {
            return std::ranges::lexicographical_compare(left, right);
        }
    };
    std::map<std::vector<uint8_t>, std::vector<uint8_t>, CompareKeys> hash;
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
    };

    bool load(uint32_t type, std::span<const uint8_t> bytes, const MapResolver& resolve) {
        if ((type != socketFilter && type != classifier) || bytes.empty() ||
            bytes.size() % sizeof(Instruction) || bytes.size() / sizeof(Instruction) > maxInstructions)
            return false;
        std::vector<Instruction> decoded(bytes.size() / sizeof(Instruction));
        std::memcpy(decoded.data(), bytes.data(), bytes.size());
        std::map<size_t, std::shared_ptr<BpfMap>> maps;
        for (size_t pc = 0; pc < decoded.size(); ++pc) {
            const auto& i = decoded[pc];
            if (i.destination() > 10 || i.source() > 10) return false;
            if (i.code == 0x18) { // The second slot carries the high half of the immediate.
                if (i.destination() == 10 || i.offset || pc + 1 >= decoded.size()) return false;
                const auto& upper = decoded[pc + 1];
                if (upper.code || upper.registers || upper.offset) return false;
                if (i.source()) {
                    if (type != classifier || i.source() != 1 || upper.immediate) return false;
                    auto map = resolve(uint32_t(i.immediate)); // BPF_PSEUDO_MAP_FD
                    if (!map || map->type == 25) return false; // Device maps have a distinct helper ABI.
                    maps.emplace(pc, std::move(map));
                    decoded[pc].immediate = 0; // Descriptor numbers have no meaning after relocation.
                }
                ++pc;
            } else if (i.code == 0x85) {
                if (i.registers || i.offset) return false;
                switch (i.immediate) {
                    case 26: break; // skb_load_bytes
                    case 1: case 9: case 10: case 11: case 23: case 31: case 39: case 40: case 43: case 50: case 125:
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
        instructions = std::move(decoded);
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
    };

    std::optional<uint32_t> execute(std::span<const uint8_t> packet, Packet* classifierPacket) const {
        // Interpreter addresses identify bounded packet, stack and map regions. The guest never
        // receives host addresses. Guest execution is serialized, so an atomic ADD needs no lock.
        constexpr uint64_t stackBase = 0x1000, context = 0x2000, packetBase = 0x10000;
        constexpr uint64_t mapHandleBase = 1ULL << 31, mapValueBase = 1ULL << 32, mapStride = 1ULL << 26;
        std::array<uint8_t, 512> stack{};
        std::array<uint64_t, 11> registers{};
        std::vector<Region> values;
        std::unordered_map<const uint8_t*, size_t> valueRegions;
        registers[1] = context;
        registers[10] = stackBase + stack.size();
        auto bytes = [&](uint64_t address, uint64_t size, bool write) -> std::span<uint8_t> {
            const auto region = [&](uint64_t base, std::span<uint8_t> memory) -> std::span<uint8_t> {
                if (address < base || address - base > memory.size() || size > memory.size() - (address - base))
                    return {};
                return memory.subspan(address - base, size);
            };
            if (address >= mapValueBase) {
                const auto index = (address - mapValueBase) / mapStride;
                if (index >= values.size() || (write && !values[index].writable)) return {};
                return region(mapValueBase + index * mapStride, values[index].bytes);
            }
            if (address >= packetBase && classifierPacket) return region(packetBase, classifierPacket->bytes);
            return region(stackBase, stack);
        };

        auto lookup = [&](uint64_t handle, uint64_t keyAddress) -> uint64_t {
            if (handle < mapHandleBase || handle - mapHandleBase >= instructions.size()) return 0;
            const auto entry = referencedMaps.find(handle - mapHandleBase);
            if (entry == referencedMaps.end()) return 0;
            auto& map = *entry->second;
            const auto key = bytes(keyAddress, map.keySize, false);
            if (key.empty()) return 0;
            std::span<uint8_t> value;
            if (map.array) {
                uint32_t index = 0;
                std::memcpy(&index, key.data(), sizeof(index));
                if (index >= map.entryCount) return 0;
                value = std::span(map.values).subspan(uint64_t(index) * map.valueStride, map.valueSize);
            } else {
                const auto found = map.hash.find(key);
                if (found == map.hash.end()) return 0;
                value = std::span(found->second).first(map.valueSize);
            }
            const auto [region, inserted] = valueRegions.try_emplace(value.data(), values.size());
            if (inserted) values.push_back({value, !(map.flags & (1U << 7))});
            return mapValueBase + region->second * mapStride;
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

        size_t pc = 0;
        for (size_t fuel = 0; fuel < 16384 && pc < instructions.size(); ++fuel) {
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
                    dst = i.source() ? mapHandleBase + pc - 1 :
                        uint32_t(imm) | (uint64_t(uint32_t(instructions[pc].immediate)) << 32);
                    ++pc;
                    continue;
                case 0x85: {
                    const auto r1 = registers[1], r2 = registers[2], r3 = registers[3];
                    uint64_t result = 0;
                    switch (i.immediate) {
                        case 1: result = lookup(r1, r2); break;
                        case 9: { // bpf_skb_store_bytes
                            if (r1 != context) return std::nullopt;
                            const auto offset = uint32_t(r2), length = uint32_t(registers[4]);
                            if (registers[5] & ~3ULL) { result = uint64_t(-22); break; }
                            const auto source = bytes(r3, length, false);
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
                            const auto to = bytes(r3, length, true);
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
                        case 50: // bpf_skb_adjust_room
                            if (r1 != context) return std::nullopt;
                            result = adjustRoom(*classifierPacket, int32_t(r2), uint32_t(r3), registers[4]);
                            packet = classifierPacket->bytes;
                            break;
                        case 125: // bpf_ktime_get_boot_ns uses the guest's CLOCK_BOOTTIME clock.
                            result = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            break;
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
                    const auto from = bytes(address, i.width(), false);
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
                    const auto to = bytes(address, i.width(), true);
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
    }

    Identity identity;
    DebugInfo debug;
    uint32_t programType = 0;
    std::vector<Instruction> instructions;
    std::map<size_t, std::shared_ptr<BpfMap>> referencedMaps;
};

} // namespace nestvane
