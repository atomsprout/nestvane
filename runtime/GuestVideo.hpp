#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace nestvane {

// A decoder of the host's for one video stream of the guest's. The image's own codec service for an
// emulator (c2.goldfish.h264.decoder and c2.goldfish.hevc.decoder, device/generic/goldfish/codecs/c2)
// hands the stream over as it comes, in Annex B, and asks for the pictures in the order they are shown.
class GuestVideoDecoder {
public:
    enum class Codec { h264, hevc };
    struct Picture {
        uint32_t width = 0, height = 0;
        uint64_t pts = 0;
        // ISO/IEC 23001-8 code points; 2 is unspecified.
        uint32_t primaries = 2, range = 0, transfer = 2, matrix = 2;
    };
    using Factory = std::function<std::unique_ptr<GuestVideoDecoder>(Codec codec, uint32_t width, uint32_t height)>;

    virtual ~GuestVideoDecoder() = default;
    // One buffer of the stream and when its picture is shown.
    virtual void decode(std::span<const uint8_t> stream, uint64_t pts) = 0;
    // The next picture in the order they are shown, as I420 without padding, if one is ready and fits.
    virtual std::optional<Picture> picture(std::span<uint8_t> i420) = 0;
    // The end of the stream, or of what is wanted of it: pictures held back for their order are ready.
    virtual void flush() = 0;
};

}
