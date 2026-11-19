#include "Ext4.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace nestvane {
namespace {

using Bytes = std::span<const uint8_t>;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
constexpr unsigned maxEntries = 4096;

void require(bool valid) {
    if (!valid) throw std::runtime_error("invalid or unsupported read-only ext4 image");
}

Bytes slice(Bytes bytes, uint64_t offset, uint64_t size) {
    require(offset <= bytes.size() && size <= bytes.size() - offset);
    return bytes.subspan(offset, size);
}

uint16_t u16(Bytes bytes, size_t offset) {
    const auto value = slice(bytes, offset, 2);
    return uint16_t(value[0]) | uint16_t(value[1]) << 8;
}

uint32_t u32(Bytes bytes, size_t offset) {
    return u16(bytes, offset) | uint32_t(u16(bytes, offset + 2)) << 16;
}

class Reader {
public:
    explicit Reader(Bytes bytes) : image(bytes) {
        const auto sb = slice(bytes, 1024, 1024);
        require(u16(sb, 56) == 0xef53 && (u16(sb, 58) & 1) && u32(sb, 76) == 1);
        const auto incompatible = u32(sb, 96);
        require((incompatible & 0x42) == 0x42 && !(incompatible & ~uint32_t{0xc2}));
        const auto readOnly = u32(sb, 100);
        // These flags describe superblock backups, file sizes, allocator metadata or checksums.
        require(!(readOnly & ~uint32_t{0x447f}));
        const auto shift = u32(sb, 24);
        require(shift <= 2);
        result.blockSize = 1024U << shift;
        result.blockCount = u32(sb, 4);
        if (incompatible & 0x80) result.blockCount |= uint64_t(u32(sb, 336)) << 32;
        require(result.blockCount && result.blockCount <= bytes.size() / result.blockSize);
        image = slice(image, 0, result.blockCount * result.blockSize);
        inodeCount = u32(sb, 0);
        inodesPerGroup = u32(sb, 40);
        const auto blocksPerGroup = u32(sb, 32);
        const auto firstBlock = u32(sb, 20);
        inodeSize = u16(sb, 88);
        descriptorSize = incompatible & 0x80 ? u16(sb, 254) : 32;
        require(inodeCount >= 2 && inodesPerGroup && blocksPerGroup &&
                firstBlock == (result.blockSize == 1024 ? 1U : 0U) &&
                firstBlock < result.blockCount && inodeSize >= 128 && inodeSize <= result.blockSize &&
                descriptorSize >= ((incompatible & 0x80) ? 64 : 32) && descriptorSize <= result.blockSize);
        groups = (result.blockCount - firstBlock + blocksPerGroup - 1) / blocksPerGroup;
        require(inodeCount <= groups * inodesPerGroup);
        descriptors = slice(image, (firstBlock + 1) * result.blockSize, groups * descriptorSize);
    }

    Ext4Image read() {
        visit(2, "", 0);
        require((result.nodes.at("").mode & 0170000) == 0040000);
        return std::move(result);
    }

private:
    Bytes image, descriptors;
    Ext4Image result{};
    std::set<uint32_t> seenDirectories;
    uint64_t remaining = maxBytes, groups = 0;
    uint32_t inodeCount = 0, inodesPerGroup = 0, inodeSize = 0, descriptorSize = 0;

    Bytes block(uint64_t index, uint32_t count = 1) const {
        require(index && index < result.blockCount && count <= result.blockCount - index);
        return slice(image, index * result.blockSize, uint64_t(count) * result.blockSize);
    }

    Bytes inode(uint32_t number) const {
        require(number && number <= inodeCount);
        const auto index = number - 1;
        const auto desc = slice(descriptors, uint64_t(index / inodesPerGroup) * descriptorSize, descriptorSize);
        auto table = uint64_t(u32(desc, 8));
        if (descriptorSize >= 64) table |= uint64_t(u32(desc, 40)) << 32;
        require(table && table < result.blockCount);
        return slice(image, table * result.blockSize + uint64_t(index % inodesPerGroup) * inodeSize, inodeSize);
    }

    void readExtents(Bytes tree, uint64_t fileSize, uint16_t depth, uint64_t& logicalEnd,
                     std::vector<Ext4Node::Extent>& extents) const {
        require(depth <= 3 && u16(tree, 0) == 0xf30a && u16(tree, 6) == depth);
        const auto count = u16(tree, 2);
        require(count <= u16(tree, 4) && size_t(count) <= (tree.size() - 12) / 12);
        uint64_t previous = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto entry = slice(tree, 12 + 12 * i, 12);
            const auto logical = uint64_t(u32(entry, 0));
            require(logical >= logicalEnd && (!i || logical > previous));
            previous = logical;
            const auto offset = logical * result.blockSize;
            require(offset < fileSize);
            if (depth) {
                const auto child = u32(entry, 4) | (uint64_t(u16(entry, 8)) << 32);
                readExtents(block(child), fileSize, depth - 1, logicalEnd, extents);
                continue;
            }
            const auto length = u16(entry, 4);
            require(length && length <= 32768);
            const auto physical = u32(entry, 8) | (uint64_t(u16(entry, 6)) << 32);
            const auto contents = block(physical, length);
            extents.push_back({offset, contents.first(std::min<uint64_t>(contents.size(), fileSize - offset))});
            logicalEnd = logical + length;
        }
    }

    void readFileData(Ext4Node& node, Bytes tree, uint64_t size) const {
        std::vector<Ext4Node::Extent> extents;
        uint64_t logicalEnd = 0;
        readExtents(tree, size, u16(tree, 6), logicalEnd, extents);
        if (!extents.empty()) {
            const auto start = uint64_t(extents.front().bytes.data() - image.data());
            uint64_t end = 0;
            for (const auto& extent : extents) {
                const auto physical = uint64_t(extent.bytes.data() - image.data());
                if (extent.offset != end || physical != start + end) break;
                end += extent.bytes.size();
            }
            if (end == size) {
                node.view = slice(image, start, size);
                return;
            }
        }
        node.extents = std::move(extents);
    }

    void visit(uint32_t number, const std::string& path, unsigned level) {
        require(level <= 32 && path.size() <= 4096 && result.nodes.size() < maxEntries);
        const auto item = inode(number);
        const auto mode = u16(item, 0);
        const auto type = mode & 0170000;
        require(type == 0100000 || type == 0040000 || type == 0120000);
        require(u16(item, 26) && !u32(item, 20));
        const auto size = u32(item, 4) | uint64_t(u32(item, 108)) << 32;
        require(size <= remaining && size <= image.size());
        remaining -= size;
        Ext4Node node{mode, u16(item, 2) | (uint32_t(u16(item, 120)) << 16),
                     u16(item, 24) | (uint32_t(u16(item, 122)) << 16), size};
        const auto tree = slice(item, 40, 60);
        if (type == 0120000 && size <= tree.size() && !(u32(item, 32) & 0x80000)) {
            node.view = tree.first(size);
        } else if (size) {
            require(u32(item, 32) & 0x80000);
            readFileData(node, tree, size);
        }
        const auto [it, inserted] = result.nodes.emplace(path, std::move(node));
        require(inserted);
        if (type == 0120000) {
            const auto contents = it->second.bytes();
            require(size && size <= 4096 &&
                    std::find(contents.begin(), contents.end(), uint8_t{0}) == contents.end());
        }
        if (type != 0040000) return;
        require(seenDirectories.insert(number).second && size && size % result.blockSize == 0);
        const Bytes children = it->second.bytes();
        for (size_t offset = 0; offset < children.size();) {
            const auto recordSize = u16(children, offset + 4);
            require(recordSize >= 8 && recordSize % 4 == 0 &&
                    recordSize <= result.blockSize - offset % result.blockSize);
            const auto entry = slice(children, offset, recordSize);
            const auto child = u32(entry, 0);
            const auto nameSize = entry[6];
            offset += recordSize;
            if (!child) continue; // Includes directory checksum records.
            require(nameSize && nameSize <= recordSize - 8);
            const std::string name(reinterpret_cast<const char*>(entry.data() + 8), nameSize);
            require(name.find('/') == std::string::npos && name.find('\0') == std::string::npos);
            if (name != "." && name != "..") visit(child, path + "/" + name, level + 1);
        }
        it->second.view = {};
        it->second.size = 0;
        std::vector<Ext4Node::Extent>().swap(it->second.extents);
        std::vector<uint8_t>().swap(it->second.owned);
    }
};

} // namespace

std::span<const uint8_t> Ext4Node::bytes() const {
    if (!view.empty()) return view;
    if (owned.empty() && size) {
        owned.resize(size);
        for (const auto& extent : extents)
            std::copy(extent.bytes.begin(), extent.bytes.end(), owned.begin() + extent.offset);
    }
    return owned;
}

Ext4Image readExt4(std::span<const uint8_t> bytes) {
    return Reader(bytes).read();
}

}
