#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace nestvane {

class GuestTouch {
public:
    static constexpr int32_t maximumAxis = 32767;
    static constexpr uint16_t touchKey = 0x14a;
    static constexpr uint16_t wakeKey = 143;

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

    class Reader {
    public:
        explicit Reader(std::shared_ptr<GuestTouch> touch) : touch(std::move(touch)) {
            this->touch->setReaderAvailable(true);
        }
        ~Reader() { touch->setReaderAvailable(false); }
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

    private:
        std::shared_ptr<GuestTouch> touch;
    };

    bool submit(int32_t x, int32_t y, bool down) {
        std::scoped_lock lock(mutex);
        if (!readerAvailable || (!down && !position.down)) return false;
        x = std::clamp(x, 0, maximumAxis);
        y = std::clamp(y, 0, maximumAxis);
        const auto [seconds, micros] = eventTime();
        const auto append = [&](uint16_t type, uint16_t code, int32_t value) {
            events.push_back({seconds, micros, type, code, value});
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
        return true;
    }

    bool wake() {
        std::scoped_lock lock(mutex);
        if (!readerAvailable || events.size() + 4 > capacity) return false;
        const auto [seconds, micros] = eventTime();
        events.insert(events.end(), {
            {seconds, micros, 1, wakeKey, 1}, {seconds, micros, 0, 0, 0},
            {seconds, micros, 1, wakeKey, 0}, {seconds, micros, 0, 0, 0},
        });
        return true;
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
    static std::pair<int64_t, int64_t> eventTime() {
        using namespace std::chrono;
        const auto now = steady_clock::now().time_since_epoch();
        const auto whole = duration_cast<seconds>(now);
        return {whole.count(), duration_cast<microseconds>(now - whole).count()};
    }

    void setReaderAvailable(bool available) {
        std::scoped_lock lock(mutex);
        readerAvailable = available;
        events.clear();
        position = {};
    }

    static constexpr size_t capacity = 256;
    mutable std::mutex mutex;
    bool readerAvailable = false;
    std::deque<Event> events;
    Position position;
};

} // namespace nestvane
