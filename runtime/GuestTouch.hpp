#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>

namespace nestvane {

class GuestTouch {
public:
    static constexpr int32_t maximumAxis = 32767;
    static constexpr uint16_t touchKey = 0x14a;

    struct Event {
        int64_t seconds;
        int64_t microseconds;
        uint16_t type, code;
        int32_t value;
    };
    static_assert(sizeof(Event) == 24);

    struct Position {
        int32_t x = 0, y = 0;
        bool down = false;
    };

    void submit(int32_t x, int32_t y, bool down) {
        std::scoped_lock lock(mutex);
        if (!down && !position.down) return;
        x = std::clamp(x, 0, maximumAxis);
        y = std::clamp(y, 0, maximumAxis);
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timestamp);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timestamp - seconds);
        const auto append = [&](uint16_t type, uint16_t code, int32_t value) {
            events.push_back({seconds.count(), micros.count(), type, code, value});
        };
        if (events.size() + 4 > capacity) {
            events.clear();
            append(0, 3, 0); // SYN_DROPPED, followed by the new state and SYN_REPORT.
        }
        append(3, 0, x); // ABS_X
        append(3, 1, y); // ABS_Y
        if (down != position.down) append(1, touchKey, down);
        append(0, 0, 0); // SYN_REPORT
        position = {x, y, down};
    }

    Position state() const {
        std::scoped_lock lock(mutex);
        return position;
    }

    bool ready() const {
        std::scoped_lock lock(mutex);
        return !events.empty();
    }

    template<typename Write>
    uint64_t read(size_t count, Write write) {
        std::scoped_lock lock(mutex);
        count = std::min(count, events.size());
        if (!count) return uint64_t(-11);
        std::array<Event, capacity> batch;
        std::copy_n(events.begin(), count, batch.begin());
        const auto length = count * sizeof(Event);
        if (!write(std::span(reinterpret_cast<const uint8_t*>(batch.data()), length))) return uint64_t(-14);
        events.erase(events.begin(), events.begin() + count);
        return length;
    }

private:
    static constexpr size_t capacity = 256;
    mutable std::mutex mutex;
    std::deque<Event> events;
    Position position;
};

} // namespace nestvane
