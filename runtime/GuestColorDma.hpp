#pragma once

#include "Runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace nestvane {

// The renderer works on a staging buffer. Only the guest scheduler touches the
// backing memory, including when it publishes DMA readback to the guest.
class GuestColorDma {
public:
    enum class Phase { upload, readback, publish };

    struct Transfer {
        uint64_t address;
        std::vector<uint8_t> pixels;
        std::shared_ptr<SharedFileBytes> backing;
        Phase phase;
        bool done = false;
        bool succeeded = false;
    };

    static void* acquire(void* context, uint64_t address, uint32_t size, bool readback, void** pixels) {
        return static_cast<GuestColorDma*>(context)->begin(address, size, readback, pixels);
    }

    static bool release(void* context, void* token, bool publish) {
        return static_cast<GuestColorDma*>(context)->finish(
            std::unique_ptr<Transfer>(static_cast<Transfer*>(token)), publish);
    }

    template <typename Service>
    void service(Service copy) {
        std::unique_lock lock(mutex);
        if (pending.empty()) return;
        for (auto* transfer : pending) {
            transfer->succeeded = copy(*transfer);
            transfer->done = true;
        }
        pending.clear();
        lock.unlock();
        changed.notify_all();
    }

    void waitUntil(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex);
        changed.wait_until(lock, deadline, [&] { return stopped || !pending.empty(); });
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            stopped = true;
            pending.clear();
        }
        changed.notify_all();
    }

private:
    static constexpr size_t maxBytes = 64 * 1024 * 1024;
    static constexpr unsigned maxTransfers = 16;

    void* begin(uint64_t address, uint32_t size, bool readback, void** pixels) {
        std::unique_lock lock(mutex);
        if (stopped || !size || size > maxBytes - bytesInUse || transfers == maxTransfers)
            return nullptr;
        std::unique_ptr<Transfer> transfer;
        try {
            transfer = std::make_unique<Transfer>(
                Transfer{address, std::vector<uint8_t>(size), {}, readback ? Phase::readback : Phase::upload});
            pending.push_back(transfer.get());
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        bytesInUse += size;
        ++transfers;
        changed.notify_all();
        changed.wait(lock, [&] { return stopped || transfer->done; });
        if (!stopped && transfer->succeeded) {
            *pixels = transfer->pixels.data();
            return transfer.release();
        }
        bytesInUse -= size;
        --transfers;
        return nullptr;
    }

    bool finish(std::unique_ptr<Transfer> transfer, bool publish) {
        std::unique_lock lock(mutex);
        bool success = !publish;
        if (publish && !stopped && transfer->phase == Phase::readback) {
            transfer->phase = Phase::publish;
            transfer->done = false;
            try {
                pending.push_back(transfer.get());
                changed.notify_all();
                changed.wait(lock, [&] { return stopped || transfer->done; });
                success = !stopped && transfer->succeeded;
            } catch (const std::bad_alloc&) {
                success = false;
            }
        }
        bytesInUse -= transfer->pixels.size();
        --transfers;
        return success;
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Transfer*> pending;
    size_t bytesInUse = 0;
    unsigned transfers = 0;
    bool stopped = false;
};

} // namespace nestvane
