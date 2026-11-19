#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace nestvane {

struct NetlinkHeader {
    uint32_t length;
    uint16_t type, flags;
    uint32_t sequence, port;
};
static_assert(sizeof(NetlinkHeader) == 16);

inline constexpr uint16_t netlinkError = 2, netlinkDone = 3;
inline constexpr uint16_t netlinkRequest = 1, netlinkMulti = 2, netlinkAck = 4, netlinkDump = 0x300;

template<class T>
T readNetlink(std::span<const uint8_t> bytes, size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

inline size_t netlinkAlign(size_t bytes) { return (bytes + 3) & ~size_t(3); }

inline void appendNetlinkAttribute(std::vector<uint8_t>& packet, uint16_t type, std::span<const uint8_t> data) {
    const size_t start = packet.size();
    const uint16_t length = data.size() + 4;
    packet.resize(start + netlinkAlign(length));
    std::memcpy(packet.data() + start, &length, sizeof(length));
    std::memcpy(packet.data() + start + 2, &type, sizeof(type));
    std::ranges::copy(data, packet.begin() + start + 4);
}

template<class T>
void appendNetlinkValue(std::vector<uint8_t>& packet, uint16_t type, const T& value) {
    appendNetlinkAttribute(packet, type, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
}

} // namespace nestvane
