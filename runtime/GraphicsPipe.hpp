#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace nestvane {

// Renderer-side byte stream. Calls are nonblocking and return byte counts or
// negative Linux errno values. Zero from read denotes end of stream.
class GraphicsPipe {
public:
    virtual ~GraphicsPipe() = default;
    virtual int64_t read(std::span<uint8_t> bytes) = 0;
    virtual int64_t write(std::span<const uint8_t> bytes) = 0;
    virtual uint16_t pollEvents() const = 0; // Linux POLLIN, POLLOUT, POLLERR and POLLHUP.
};

using GraphicsPipeFactory = std::function<std::unique_ptr<GraphicsPipe>(std::string_view service)>;

} // namespace nestvane
