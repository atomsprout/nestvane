#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

    // What the app asks of Android through its service (text.sh in DynamicProbe.cpp), which opens
    // textDevice and waits here for the next request: three lines, the kind and two fields.
    // "text", sender, text: a text message the person wrote in the app, for Android to show as one
    // it received. Nothing is sent anywhere: the guest has no phone process and no radio.
    // "rate", frames a second, nothing: the most frames a second apps are to draw, lowered while
    // the phone is hot; 0 lifts it again.
    static constexpr std::string_view textDevice = "/dev/nestvane-text";
    static constexpr std::string_view textReader = "/system/bin/sh";
    bool submitText(std::string_view sender, std::string_view text) {
        const auto line = [](std::string_view field) {
            std::string out(field.substr(0, 2000));
            for (auto& c : out) if (c == '\n' || c == '\r' || c == '\0') c = ' ';
            return out;
        };
        std::lock_guard lock(mutex);
        if (stopped || texts.size() >= 16 || sender.empty() || text.empty()) return false;
        const auto message = "text\n" + line(sender) + "\n" + line(text) + "\n";
        texts.emplace_back(message.begin(), message.end());
        return true;
    }
    bool submitDisplayRate(int framesPerSecond) {
        std::lock_guard lock(mutex);
        if (stopped || texts.size() >= 16 || framesPerSecond < 0 || framesPerSecond > 240) return false;
        const auto message = "rate\n" + std::to_string(framesPerSecond) + "\n\n";
        texts.emplace_back(message.begin(), message.end());
        return true;
    }
    bool takeText(std::vector<uint8_t>& message) {
        std::lock_guard lock(mutex);
        if (texts.empty()) return false;
        message = std::move(texts.front());
        texts.erase(texts.begin());
        return true;
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
    std::vector<std::vector<uint8_t>> texts;
    bool stopped = false;
};

} // namespace nestvane
