#pragma once

#include "GraphicsPipe.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>

namespace nestvane {

// Ranchu's qemud sensors service frames messages with four hexadecimal length bytes.
// This device has an empty motion-sensor catalog, so there are no periodic readings.
class GuestSensorsPipe final : public GraphicsPipe {
public:
    static constexpr std::string_view service = "qemud:sensors";

    int64_t read(std::span<uint8_t> bytes) override {
        if (bytes.empty()) return 0;
        if (reply.empty()) return failed ? 0 : -11;
        const auto count = std::min(bytes.size(), reply.size());
        std::copy_n(reply.begin(), count, bytes.begin());
        reply.erase(0, count);
        return count;
    }

    int64_t write(std::span<const uint8_t> bytes) override {
        if (failed) return -5;
        size_t consumed = 0;
        while (!bytes.empty() && reply.size() + maxReply <= replyLimit) {
            const auto count = std::min(bytes.size(), frameSize - filled);
            std::copy_n(bytes.begin(), count, frame.begin() + filled);
            filled += count;
            consumed += count;
            bytes = bytes.subspan(count);
            if (filled < frameSize) continue;
            if (frameSize == headerSize) {
                unsigned length = 0;
                const auto end = frame.data() + headerSize;
                const auto result = std::from_chars(frame.data(), end, length, 16);
                if (result.ec != std::errc{} || result.ptr != end || !length || length > frame.size() - headerSize) {
                    failed = true;
                    return -22;
                }
                frameSize = headerSize + length;
                continue;
            }
            const std::string_view command(frame.data() + headerSize, frameSize - headerSize);
            if (command == "list-sensors") reply += "00010";
            else if (command == "wake") reply += "0004wake";
            // Timing and activation messages have no effect when no sensors are attached.
            filled = 0;
            frameSize = headerSize;
        }
        return consumed || bytes.empty() ? int64_t(consumed) : -11;
    }

    uint16_t pollEvents() const override {
        constexpr uint16_t readable = 1, writable = 4, hangup = 16;
        return (reply.empty() ? 0 : readable) |
            (failed ? hangup : reply.size() + maxReply <= replyLimit ? writable : 0);
    }

private:
    static constexpr size_t headerSize = 4, maxReply = 8, replyLimit = 512;
    std::array<char, headerSize + 256> frame{};
    size_t filled = 0, frameSize = headerSize;
    std::string reply;
    bool failed = false;
};

} // namespace nestvane
