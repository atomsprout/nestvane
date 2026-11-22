#pragma once

// The guest's sound output on its way to the host. The kernel layer's sound device writes what
// Android plays into it; the host's audio output takes it out at the rate the hardware plays. With
// nobody taking it out, the kernel layer lets the guest play at the pace of the clock, into nothing.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace nestvane {

struct GuestAudio {
    // The format the guest plays in. `generation` goes up each time the guest sets it.
    std::atomic<uint32_t> rate{0}, channels{0}, bits{16};
    std::atomic<uint64_t> generation{0};
    // Whether a host output is taking the sound out.
    std::atomic<bool> consumer{false};
    // For a report: the bytes the host has taken out, and the loudest 16-bit sample among them.
    std::atomic<uint64_t> taken{0};
    std::atomic<uint32_t> peak{0};

    void configure(uint32_t newRate, uint32_t newChannels, uint32_t newBits) {
        const std::lock_guard guard(lock);
        rate = newRate;
        channels = newChannels;
        bits = newBits;
        // A fifth of a second: enough to ride out the guest being late, short enough not to lag.
        ring.assign(std::max<size_t>(size_t(newRate) * newChannels * (newBits / 8) / 5, 16384), 0);
        head = filled = 0;
        ++generation;
    }
    size_t space() {
        const std::lock_guard guard(lock);
        return ring.size() - filled;
    }
    // All of the bytes or none.
    bool write(const uint8_t* bytes, size_t count) {
        const std::lock_guard guard(lock);
        if (count > ring.size() - filled) return false;
        size_t at = (head + filled) % ring.size();
        const size_t first = std::min(count, ring.size() - at);
        std::memcpy(ring.data() + at, bytes, first);
        std::memcpy(ring.data(), bytes + first, count - first);
        filled += count;
        return true;
    }
    size_t read(uint8_t* out, size_t count) {
        const std::lock_guard guard(lock);
        count = std::min(count, filled);
        if (!count) return 0;
        const size_t first = std::min(count, ring.size() - head);
        std::memcpy(out, ring.data() + head, first);
        std::memcpy(out + first, ring.data(), count - first);
        head = (head + count) % ring.size();
        filled -= count;
        taken += count;
        if (bits.load() == 16) {
            uint32_t loudest = peak.load();
            for (size_t at = 0; at + 1 < count; at += 2) {
                int16_t sample;
                std::memcpy(&sample, out + at, sizeof(sample));
                loudest = std::max<uint32_t>(loudest, uint32_t(sample < 0 ? -int32_t(sample) : sample));
            }
            peak = loudest;
        }
        return count;
    }
    size_t buffered() {
        const std::lock_guard guard(lock);
        return filled;
    }
    static GuestAudio& host() {
        static GuestAudio audio;
        return audio;
    }

private:
    std::mutex lock;
    std::vector<uint8_t> ring;
    size_t head = 0, filled = 0;
};

} // namespace nestvane
