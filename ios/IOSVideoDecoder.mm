#include "IOSVideoDecoder.hpp"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace nestvane {
namespace {

// Counted for the rate report.
std::atomic<uint64_t> samplesGiven{0}, samplesRefused{0}, picturesMade{0}, picturesTaken{0}, picturesAsked{0}, unprepared{0};
std::atomic<int32_t> lastRefusal{0};
std::atomic<uint64_t> decodeMicroseconds{0}, pictureMicroseconds{0}, sessionsMade{0};

struct Timed {
    std::atomic<uint64_t>& total;
    CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();
    ~Timed() { total += uint64_t((CFAbsoluteTimeGetCurrent() - began) * 1e6); }
};

// The bits of a parameter set, without the bytes that keep start codes out of it.
class Bits {
public:
    explicit Bits(std::span<const uint8_t> unit) {
        for (size_t index = 0; index < unit.size(); ++index) {
            if (index >= 2 && unit[index] == 3 && !unit[index - 1] && !unit[index - 2]) continue;
            bytes.push_back(unit[index]);
        }
    }
    bool ended() const { return position >= bytes.size() * 8; }
    uint32_t take(unsigned count) {
        uint32_t value = 0;
        for (; count; --count, ++position)
            value = value << 1 | (ended() ? 0 : (bytes[position / 8] >> (7 - position % 8)) & 1);
        return value;
    }
    void skip(size_t count) { position += count; }
    uint32_t golomb() {
        unsigned zeros = 0;
        while (!ended() && !take(1) && zeros < 32) ++zeros;
        return zeros ? ((1u << zeros) - 1) + take(zeros) : 0;
    }

private:
    std::vector<uint8_t> bytes;
    size_t position = 0;
};

// How many pictures an HEVC stream may hold back to put them in order (sps_max_num_reorder_pics of the
// highest sub-layer), or -1 if the sequence parameter set cannot be read.
int hevcReorder(std::span<const uint8_t> sps) {
    if (sps.size() < 16) return -1;
    Bits bits(sps.subspan(2)); // after the two bytes of the unit's header
    bits.skip(4);
    const unsigned layers = bits.take(3);
    bits.skip(1);
    bits.skip(96); // the general profile, tier and level
    bool profile[8] = {}, level[8] = {};
    for (unsigned index = 0; index < layers; ++index) {
        profile[index] = bits.take(1);
        level[index] = bits.take(1);
    }
    if (layers) bits.skip(2 * (8 - layers));
    for (unsigned index = 0; index < layers; ++index) bits.skip((profile[index] ? 88 : 0) + (level[index] ? 8 : 0));
    bits.golomb(); // sps_seq_parameter_set_id
    if (bits.golomb() == 3) bits.skip(1);
    bits.golomb();
    bits.golomb();
    if (bits.take(1)) for (int index = 0; index < 4; ++index) bits.golomb();
    bits.golomb();
    bits.golomb();
    bits.golomb();
    const bool each = bits.take(1);
    uint32_t reorder = 0;
    for (unsigned index = each ? 0 : layers; index <= layers; ++index) {
        bits.golomb();
        reorder = bits.golomb();
        bits.golomb();
    }
    return bits.ended() || reorder > 16 ? -1 : int(reorder);
}

class Decoder final : public GuestVideoDecoder {
public:
    Decoder(Codec codec, uint32_t width, uint32_t height) : codec(codec), width(width), height(height) {}

    ~Decoder() override {
        close();
        std::lock_guard guard(lock);
        for (auto& [pts, image] : ready) CVPixelBufferRelease(image);
    }

    void decode(std::span<const uint8_t> stream, uint64_t pts) override {
        Timed timed{decodeMicroseconds};
        draining = false;
        std::vector<uint8_t> sample;
        // Annex B: each unit follows 00 00 01. The decoder takes them with their length in front instead.
        size_t position = 0, start = SIZE_MAX;
        const auto unit = [&](size_t first, size_t last) {
            while (last > first && !stream[last - 1]) --last; // zero bytes before the next start code
            if (last > first) take(stream.subspan(first, last - first), sample);
        };
        while (position + 3 <= stream.size()) {
            if (!stream[position] && !stream[position + 1] && stream[position + 2] == 1) {
                if (start != SIZE_MAX) unit(start, position);
                start = position += 3;
            } else {
                ++position;
            }
        }
        if (start != SIZE_MAX) unit(start, stream.size());
        if (sample.empty()) return;
        if (lost.exchange(false)) close();
        if (!prepare()) {
            ++unprepared;
            return;
        }

        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, sample.size(), kCFAllocatorDefault, nullptr, 0,
                                               sample.size(), kCMBlockBufferAssureMemoryNowFlag, &block) != noErr) return;
        CMBlockBufferReplaceDataBytes(sample.data(), block, 0, sample.size());
        // The time is carried through as it is: the guest matches pictures to its buffers by it.
        CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake(int64_t(pts), 1000000), kCMTimeInvalid};
        const size_t size = sample.size();
        CMSampleBufferRef buffer = nullptr;
        if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 1, &timing, 1, &size, &buffer) == noErr) {
            const OSStatus status = VTDecompressionSessionDecodeFrameWithOutputHandler(
                session, buffer, 0, nullptr,
                ^(OSStatus result, VTDecodeInfoFlags, CVImageBufferRef image, CMTime shown, CMTime) {
                    if (result != noErr || !image) {
                        ++samplesRefused;
                        lastRefusal = result;
                        // The decoder was taken away (the app went to the background): a new one is made
                        // for the next buffer, and pictures come again from the next key frame.
                        if (result == kVTInvalidSessionErr || result == kVTVideoDecoderMalfunctionErr) lost = true;
                        return;
                    }
                    ++picturesMade;
                    const uint64_t at = uint64_t(shown.value);
                    std::lock_guard guard(lock);
                    // A picture that comes after later ones tells how far the stream reorders, when
                    // its parameter sets did not: the pictures waiting, and those already given out.
                    size_t later = 0;
                    for (auto found = ready.upper_bound(at); found != ready.end(); ++found) ++later;
                    if (given && at < lastGiven) later += 1;
                    reorder = std::min<size_t>(16, std::max(reorder, later));
                    ready.emplace(at, CVPixelBufferRetain(image));
                    while (ready.size() > 32) {
                        CVPixelBufferRelease(ready.begin()->second);
                        ready.erase(ready.begin());
                    }
                });
            ++samplesGiven;
            if (status != noErr) lastRefusal = status;
            if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) lost = true;
            CFRelease(buffer);
        }
        CFRelease(block);
    }

    std::optional<Picture> picture(std::span<uint8_t> i420) override {
        Timed timed{pictureMicroseconds};
        CVPixelBufferRef image = nullptr;
        Picture result{width, height};
        ++picturesAsked;
        {
            std::lock_guard guard(lock);
            if (ready.empty() || (!draining && ready.size() <= reorder)) return std::nullopt;
            if (i420.size() < size_t(width) * height * 3 / 2) return std::nullopt;
            result.pts = ready.begin()->first;
            image = ready.begin()->second;
            ready.erase(ready.begin());
            given = true;
            lastGiven = result.pts;
        }
        ++picturesTaken;
        // The decoder was asked for two planes: brightness, and the two colour differences interleaved.
        // The guest wants three, at the size it gave, with no padding.
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        const size_t columns = std::min<size_t>(width, CVPixelBufferGetWidthOfPlane(image, 0));
        const size_t rows = std::min<size_t>(height, CVPixelBufferGetHeightOfPlane(image, 0));
        const auto* luma = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
        const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
        std::memset(i420.data(), 16, size_t(width) * height);
        std::memset(i420.data() + size_t(width) * height, 128, size_t(width) * height / 2);
        if (luma && CVPixelBufferGetPlaneCount(image) == 2) {
            for (size_t row = 0; row < rows; ++row)
                std::memcpy(i420.data() + row * width, luma + row * lumaStride, columns);
            const auto* chroma = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
            const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
            const size_t halfColumns = std::min<size_t>(width / 2, CVPixelBufferGetWidthOfPlane(image, 1));
            const size_t halfRows = std::min<size_t>(height / 2, CVPixelBufferGetHeightOfPlane(image, 1));
            uint8_t* u = i420.data() + size_t(width) * height;
            uint8_t* v = u + size_t(width / 2) * (height / 2);
            for (size_t row = 0; chroma && row < halfRows; ++row) {
                const uint8_t* source = chroma + row * chromaStride;
                uint8_t* toU = u + row * (width / 2);
                uint8_t* toV = v + row * (width / 2);
                for (size_t column = 0; column < halfColumns; ++column) {
                    toU[column] = source[2 * column];
                    toV[column] = source[2 * column + 1];
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(image);
        return result;
    }

    void flush() override {
        if (session) VTDecompressionSessionWaitForAsynchronousFrames(session);
        draining = true;
    }

private:
    // One unit of the stream: a parameter set is kept for the decoder's format, a unit of a picture
    // goes into the sample with its length in front.
    void take(std::span<const uint8_t> unit, std::vector<uint8_t>& sample) {
        const unsigned type = codec == Codec::h264 ? unit[0] & 0x1f : (unit[0] >> 1) & 0x3f;
        const int set = codec == Codec::h264 ? (type == 7 ? 1 : type == 8 ? 2 : -1)
                                             : (type == 32 ? 0 : type == 33 ? 1 : type == 34 ? 2 : -1);
        if (set >= 0) {
            std::vector<uint8_t> bytes(unit.begin(), unit.end());
            if (sets[set] != bytes) {
                sets[set] = std::move(bytes);
                changed = true;
            }
            return;
        }
        // Access unit delimiters, the end of a sequence and filler are not for the decoder.
        const bool wanted = codec == Codec::h264 ? (type >= 1 && type <= 6) : (type <= 31 || type == 39 || type == 40);
        if (!wanted) return;
        const uint32_t length = uint32_t(unit.size());
        const uint8_t prefix[4] = {uint8_t(length >> 24), uint8_t(length >> 16), uint8_t(length >> 8), uint8_t(length)};
        sample.insert(sample.end(), prefix, prefix + 4);
        sample.insert(sample.end(), unit.begin(), unit.end());
    }

    // A session for the parameter sets seen so far. False until the stream has given them.
    bool prepare() {
        if (session && !changed) return true;
        if (sets[1].empty() || sets[2].empty() || (codec == Codec::hevc && sets[0].empty())) return false;
        CMVideoFormatDescriptionRef described = nullptr;
        if (codec == Codec::h264) {
            const uint8_t* const pointers[] = {sets[1].data(), sets[2].data()};
            const size_t sizes[] = {sets[1].size(), sets[2].size()};
            if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, pointers, sizes, 4, &described) != noErr)
                return false;
        } else {
            const uint8_t* const pointers[] = {sets[0].data(), sets[1].data(), sets[2].data()};
            const size_t sizes[] = {sets[0].size(), sets[1].size(), sets[2].size()};
            if (CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, pointers, sizes, 4, nullptr, &described) != noErr)
                return false;
        }
        changed = false;
        if (session && VTDecompressionSessionCanAcceptFormatDescription(session, described)) {
            if (format) CFRelease(format);
            format = described;
            return true;
        }
        close();
        changed = false;
        format = described;
        NSDictionary* wanted = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        };
        ++sessionsMade;
        if (VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr, (__bridge CFDictionaryRef)wanted, nullptr, &session) != noErr) {
            session = nullptr;
            return false;
        }
        // What the stream says of its reordering, where it can be read; a baseline H.264 stream has none.
        // Otherwise it is learnt from the pictures as they come (see the output handler).
        int known = -1;
        if (codec == Codec::hevc) known = hevcReorder(sets[1]);
        else if (sets[1].size() > 1 && sets[1][1] == 66) known = 0;
        std::lock_guard guard(lock);
        reorder = known >= 0 ? size_t(known) : std::max<size_t>(reorder, 2);
        return true;
    }

    void close() {
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        if (format) {
            CFRelease(format);
            format = nullptr;
        }
        changed = true;
    }

    const Codec codec;
    const uint32_t width, height;
    std::vector<uint8_t> sets[3]; // video, sequence and picture parameter sets
    bool changed = false;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    std::mutex lock;
    std::multimap<uint64_t, CVPixelBufferRef> ready; // decoded pictures by the time they are shown
    size_t reorder = 0;
    bool draining = false, given = false;
    std::atomic<bool> lost{false};
    uint64_t lastGiven = 0;
};

}

std::string IOSVideoDecoderReport() {
    char text[300];
    std::snprintf(text, sizeof(text), "video decoder: %llu samples, %llu refused (last %d), %llu before parameter sets, %llu pictures made, %llu taken of %llu asked; %llu ms decoding, %llu ms copying, %llu sessions",
                  static_cast<unsigned long long>(samplesGiven.exchange(0)), static_cast<unsigned long long>(samplesRefused.exchange(0)),
                  int(lastRefusal.load()), static_cast<unsigned long long>(unprepared.exchange(0)),
                  static_cast<unsigned long long>(picturesMade.exchange(0)), static_cast<unsigned long long>(picturesTaken.exchange(0)),
                  static_cast<unsigned long long>(picturesAsked.exchange(0)),
                  static_cast<unsigned long long>(decodeMicroseconds.exchange(0) / 1000), static_cast<unsigned long long>(pictureMicroseconds.exchange(0) / 1000),
                  static_cast<unsigned long long>(sessionsMade.exchange(0)));
    return text;
}

GuestVideoDecoder::Factory makeIOSVideoDecoders() {
    return [](GuestVideoDecoder::Codec codec, uint32_t width, uint32_t height) -> std::unique_ptr<GuestVideoDecoder> {
        return std::make_unique<Decoder>(codec, width, height);
    };
}

}
