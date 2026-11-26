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
    // And the bytes the guest has played, whether or not anybody took them.
    std::atomic<uint64_t> played{0};
    std::atomic<uint32_t> peak{0};
    // How often the output wanted sound while playing and the guest had not given enough: a gap, heard
    // as a click or a crackle.
    std::atomic<uint64_t> shortfalls{0};

    void configure(uint32_t newRate, uint32_t newChannels, uint32_t newBits) {
        const std::lock_guard guard(lock);
        rate = newRate;
        channels = newChannels;
        bits = newBits;
        // A fifth of a second: enough to ride out the guest being late, short enough not to lag.
        const size_t frame = size_t(newChannels) * (newBits / 8);
        ring.assign(std::max<size_t>(size_t(newRate) * frame / 5, 16384), 0);
        // What the output keeps in hand before it starts taking: about 43 ms, two of its own requests. The
        // guest's audio service fills the card by its own timing, a buffer at a time and never early.
        cushion = std::min<size_t>(size_t(newRate) * frame * 43 / 1000 / frame * frame, ring.size() / 4);
        primed = false;
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
        // Nothing is taken until the cushion is there, and again after the sound ran out.
        if (!primed && filled < cushion) return 0;
        if (primed && filled < count) ++shortfalls;
        primed = filled != 0;
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
    // What the guest's card holds, as the guest sees it: what waits beyond the cushion.
    size_t pending() {
        const std::lock_guard guard(lock);
        return filled > cushion ? filled - cushion : 0;
    }
    static GuestAudio& host() {
        static GuestAudio audio;
        return audio;
    }

private:
    std::mutex lock;
    std::vector<uint8_t> ring;
    size_t head = 0, filled = 0, cushion = 0;
    bool primed = false;
};

// The other way: what the host's microphone hears, on its way to the guest's capture device. The host
// starts recording when the guest reads (`wanted` says when it last did) and stops when it has not
// for a while; with nobody recording, the kernel layer gives the guest silence at the pace of the clock.
struct GuestMicrophone {
    // The format the guest records in. `generation` goes up each time the guest sets it.
    std::atomic<uint32_t> rate{0}, channels{0}, bits{16};
    std::atomic<uint64_t> generation{0};
    // Whether the host is putting sound in.
    std::atomic<bool> producer{false};
    // When the guest last read, in milliseconds of the steady clock; 0 before the first read.
    std::atomic<uint64_t> wanted{0};
    // For a report: the bytes the guest has taken, and the loudest 16-bit sample among them.
    std::atomic<uint64_t> given{0};
    std::atomic<uint32_t> peak{0};

    void configure(uint32_t newRate, uint32_t newChannels, uint32_t newBits) {
        const std::lock_guard guard(lock);
        rate = newRate;
        channels = newChannels;
        bits = newBits;
        // Half a second. More than that waiting is old sound: the oldest is dropped.
        ring.assign(std::max<size_t>(size_t(newRate) * newChannels * (newBits / 8) / 2, 16384), 0);
        head = filled = 0;
        ++generation;
    }
    void write(const uint8_t* bytes, size_t count) {
        const std::lock_guard guard(lock);
        if (ring.empty()) return;
        if (count > ring.size()) { bytes += count - ring.size(); count = ring.size(); }
        if (const size_t over = filled + count > ring.size() ? filled + count - ring.size() : 0) {
            head = (head + over) % ring.size();
            filled -= over;
        }
        const size_t at = (head + filled) % ring.size();
        const size_t first = std::min(count, ring.size() - at);
        std::memcpy(ring.data() + at, bytes, first);
        std::memcpy(ring.data(), bytes + first, count - first);
        filled += count;
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
        given += count;
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
    static GuestMicrophone& host() {
        static GuestMicrophone microphone;
        return microphone;
    }

private:
    std::mutex lock;
    std::vector<uint8_t> ring;
    size_t head = 0, filled = 0;
};

} // namespace nestvane
