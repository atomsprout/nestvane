#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace nestvane {

struct Ext4Node {
    struct Extent { uint64_t offset; std::span<const uint8_t> bytes; };
    uint16_t mode;
    uint32_t uid, gid;
    uint64_t size;
    std::span<const uint8_t> view{};
    std::vector<Extent> extents{};
    mutable std::vector<uint8_t> owned{};
    std::span<const uint8_t> bytes() const;
};

struct Ext4Image {
    uint32_t blockSize;
    uint64_t blockCount;
    std::map<std::string, Ext4Node, std::less<>> nodes;
};

// Reads a small, clean, extent-based image with regular files, directories and links.
// Throws on malformed images and filesystem features this reader cannot interpret.
// The immutable input must outlive the returned image and all file views.
Ext4Image readExt4(std::span<const uint8_t> bytes);

}
