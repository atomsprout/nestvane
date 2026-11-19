#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace nestvane {

class BpfTypes {
public:
    size_t byteSize() const { return size; }

    bool load(std::span<const uint8_t> bytes) {
        constexpr size_t headerBytes = 24;
        if (bytes.size() < headerBytes || read32(bytes, 0) != 0x0001eb9f) return false;
        const auto header = read32(bytes, 4);
        const auto typeOffset = read32(bytes, 8), typeLength = read32(bytes, 12);
        const auto stringOffset = read32(bytes, 16), stringLength = read32(bytes, 20);
        if (header < headerBytes || header > bytes.size() || (typeOffset & 3) || !stringLength) return false;
        const auto payload = bytes.subspan(header);
        if (typeOffset > payload.size() || typeLength > payload.size() - typeOffset ||
            stringOffset != uint64_t(typeOffset) + typeLength ||
            uint64_t(stringOffset) + stringLength != payload.size()) return false;
        const auto strings = payload.subspan(stringOffset, stringLength);
        if (strings.front() || strings.back()) return false;
        const auto validName = [&](uint32_t offset) { return offset < strings.size(); };

        const auto types = payload.subspan(typeOffset, typeLength);
        std::vector<Record> decoded;
        uint32_t highestReference = 0;
        for (size_t pos = 0; pos < types.size();) {
            if (types.size() - pos < 12 || !validName(read32(types, pos))) return false;
            const auto info = read32(types, pos + 4);
            const auto kind = (info >> 24) & 31;
            const auto count = info & 0xffff;
            const auto data = read32(types, pos + 8);
            if (!kind || kind >= widths.size() || (info & 0x60ff0000)) return false;
            const bool repeats = kind == 4 || kind == 5 || kind == 6 || kind == 13 || kind == 15 || kind == 19;
            if (!repeats && count && (kind != 12 || count > 2)) return false;
            const size_t elements = repeats ? count : 1;
            const size_t width = widths[kind];
            pos += 12;
            if (width && elements > (types.size() - pos) / width) return false;
            Record record{kind, data, 0};
            const auto reference = [&](uint32_t id) {
                if (id > highestReference) highestReference = id;
            };
            if (kind == 1) {
                const auto encoding = read32(types, pos);
                if (!data || !(encoding & 0xff) ||
                    ((encoding >> 16) & 0xff) + (encoding & 0xff) > uint64_t(data) * 8) return false;
            } else if (kind == 3) {
                record.data = read32(types, pos);
                record.elements = read32(types, pos + 8);
                reference(record.data);
                reference(read32(types, pos + 4));
            } else if (kind == 2 || (kind >= 8 && kind <= 14) || kind == 17 || kind == 18) {
                reference(data);
            }
            for (size_t entry = 0; repeats && entry < elements; ++entry) {
                const auto offset = pos + entry * width;
                if (kind == 15) {
                    reference(read32(types, offset));
                } else {
                    if (!validName(read32(types, offset))) return false;
                    if (kind == 4 || kind == 5 || kind == 13) reference(read32(types, offset + 4));
                }
            }
            decoded.push_back(record);
            pos += elements * width;
        }
        if (highestReference > decoded.size()) return false;
        records = std::move(decoded);
        size = bytes.size();
        return true;
    }

    std::optional<uint32_t> typeSize(uint32_t id) const {
        uint64_t count = 1;
        for (size_t remaining = records.size(); remaining && id && id <= records.size(); --remaining) {
            const auto& entry = records[id - 1];
            switch (entry.kind) {
                case 1: case 4: case 5: case 6: case 16: case 19:
                    if (entry.data && count > UINT32_MAX / entry.data) return {};
                    return uint32_t(count * entry.data);
                case 2: // Pointer width on the ARM64 target.
                    if (count > UINT32_MAX / 8) return {};
                    return uint32_t(count * 8);
                case 3:
                    if (entry.elements && count > UINT32_MAX / entry.elements) return {};
                    count *= entry.elements;
                    break;
                case 8: case 9: case 10: case 11: case 18:
                    break;
                default: return {};
            }
            id = entry.data;
        }
        return {};
    }

private:
    struct Record { uint32_t kind, data, elements; };
    static constexpr std::array<uint8_t, 20> widths{0, 4, 0, 12, 12, 12, 8, 0, 0, 0, 0, 0, 0, 8, 4, 12, 0, 4, 0, 12};
    static uint32_t read32(std::span<const uint8_t> bytes, size_t offset) {
        uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    std::vector<Record> records;
    size_t size = 0;
};

} // namespace nestvane
