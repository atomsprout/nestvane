#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace nestvane {

class GuestApkInbox {
public:
    static constexpr size_t maxBytes = 256 * 1024 * 1024;
    static constexpr std::string_view device = "/dev/nestvane-apk";
    static constexpr std::string_view installer = "/system/bin/nestvane-apk-install";
    static constexpr uint32_t resultIoctl = 0x40044e01;

    enum class Result { installed, rejected, storageError, guestStopped };
    using Completion = std::function<void(Result)>;

    class Request {
    public:
        Request(std::vector<uint8_t> file, Completion complete)
            : bytes(std::move(file)), completion(std::move(complete)) {}
        ~Request() { finish(Result::guestStopped); }
        const std::vector<uint8_t> bytes;

        bool finished() const {
            std::lock_guard lock(mutex);
            return !completion;
        }

        void finish(Result result) {
            Completion callback;
            {
                std::lock_guard lock(mutex);
                callback = std::exchange(completion, {});
            }
            if (callback) callback(result);
        }

    private:
        mutable std::mutex mutex;
        Completion completion;
    };

    bool submit(std::vector<uint8_t> file, Completion completion) {
        if (file.empty() || file.size() > maxBytes || !completion) return false;
        std::shared_ptr<Request> previous;
        std::lock_guard lock(mutex);
        if (stopped || pending) return false;
        previous = active.lock();
        if (previous && !previous->finished()) return false;
        pending = std::make_shared<Request>(std::move(file), std::move(completion));
        return true;
    }

    std::shared_ptr<Request> take() {
        std::lock_guard lock(mutex);
        auto request = std::exchange(pending, {});
        if (request) active = request;
        return request;
    }

    void stop() {
        std::shared_ptr<Request> queued, running;
        {
            std::lock_guard lock(mutex);
            stopped = true;
            queued = std::exchange(pending, {});
            running = active.lock();
        }
        if (queued) queued->finish(Result::guestStopped);
        if (running) running->finish(Result::guestStopped);
    }

private:
    std::mutex mutex;
    std::shared_ptr<Request> pending;
    std::weak_ptr<Request> active;
    bool stopped = false;
};

} // namespace nestvane
