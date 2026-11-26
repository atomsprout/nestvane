#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nestvane {

class GuestApkInbox {
public:
    // Large apps come as packages of several hundred megabytes: the one of Douyin is 271 MB.
    static constexpr size_t maxBytes = 1024 * 1024 * 1024;
    static constexpr std::string_view device = "/dev/nestvane-apk";
    static constexpr std::string_view installer = "/system/bin/nestvane-apk-install";
    static constexpr uint32_t resultIoctl = 0x40044e01;

    enum class Result { installed, rejected, storageError, guestStopped };
    using Completion = std::function<void(Result)>;

    class Request {
    public:
        Request(std::vector<uint8_t> file, Completion complete) : Request(std::make_shared<const std::vector<uint8_t>>(std::move(file)), std::move(complete)) {}
        // Bytes kept alive by their owner, such as a file mapped into memory: a package of several hundred
        // megabytes is not copied, which could fail for want of memory and ended the app.
        Request(std::span<const uint8_t> file, std::shared_ptr<const void> owner, Completion complete)
            : bytes(file), owner(std::move(owner)), completion(std::move(complete)) {}
        ~Request() { finish(Result::guestStopped); }
        const std::span<const uint8_t> bytes;

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
        Request(std::shared_ptr<const std::vector<uint8_t>> file, Completion complete)
            : bytes(*file), owner(file), completion(std::move(complete)) {}
        std::shared_ptr<const void> owner;
        mutable std::mutex mutex;
        Completion completion;
    };

    bool submit(std::vector<uint8_t> file, Completion completion) {
        if (file.empty() || file.size() > maxBytes || !completion) return false;
        return accept([&] { return std::make_shared<Request>(std::move(file), std::move(completion)); });
    }
    bool submit(std::span<const uint8_t> file, std::shared_ptr<const void> owner, Completion completion) {
        if (file.empty() || file.size() > maxBytes || !completion) return false;
        return accept([&] { return std::make_shared<Request>(file, std::move(owner), std::move(completion)); });
    }

    // The request is made only once it is taken: one refused never reports an outcome.
    template<class Make>
    bool accept(Make make) {
        std::shared_ptr<Request> previous;
        std::lock_guard lock(mutex);
        if (stopped || pending) return false;
        previous = active.lock();
        if (previous && !previous->finished()) return false;
        pending = make();
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
    // "launch", package name, nothing: opens that app, as tapping its icon does. For tests.
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
        textKinds.push_back(true);
        if (broadcasts.size() < 16) {
            const auto announced = line(sender) + "\n" + line(text) + "\n";
            broadcasts.emplace_back(announced.begin(), announced.end());
        }
        return true;
    }
    // The same message once more, "sender\ntext\n", for the service that tells Android's apps a
    // message arrived (sms.sh in DynamicProbe.cpp). Only the system may send that broadcast, so it
    // is a service of its own, running as root, with a device of its own.
    static constexpr std::string_view broadcastDevice = "/dev/nestvane-sms";
    bool takeBroadcast(std::vector<uint8_t>& message) {
        std::lock_guard lock(mutex);
        if (!rootRequests.empty()) {
            message = std::move(rootRequests.front());
            rootRequests.erase(rootRequests.begin());
            return true;
        }
        if (broadcasts.empty() || !announceable) return false;
        --announceable;
        message = std::move(broadcasts.front());
        broadcasts.erase(broadcasts.begin());
        return true;
    }
    // Clears the apps in the background. Removing other apps' recent tasks needs more than the shell
    // user has, so this goes to the root service (sms.sh): an empty sender, which a text message
    // never has, and "clean".
    bool submitClean() {
        std::lock_guard lock(mutex);
        if (stopped || rootRequests.size() >= 4) return false;
        static constexpr std::string_view message = "\nclean\n";
        rootRequests.emplace_back(message.begin(), message.end());
        return true;
    }
    // "uninstall", package name, nothing: removes that app. For tests.
    bool submitUninstall(std::string_view package) { return submitLaunch(package, "uninstall"); }
    bool submitLaunch(std::string_view package, std::string_view kind = "launch") {
        std::lock_guard lock(mutex);
        if (stopped || texts.size() >= 16 || package.empty() || package.size() > 200) return false;
        for (const char c : package)
            if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '.' && c != '_') return false;
        const auto message = std::string(kind) + "\n" + std::string(package) + "\n\n";
        texts.emplace_back(message.begin(), message.end());
        textKinds.push_back(false);
        return true;
    }
    // "scan", a directory of the shared storage, nothing: tells Android's media library to look at
    // it again, after files were put there from outside while Android ran (ProbeLinux::rescanShared).
    // "shell", a command, nothing: runs it as Android's shell user, its output going to
    // nestvane-test/shell.txt in the shared storage. Only the app's tests send it (NESTVANE_TEST_TAPS).
    bool submitLine(std::string_view kind, std::string_view field) {
        std::lock_guard lock(mutex);
        if (stopped || texts.size() >= 16 || field.empty() || field.size() > 4000) return false;
        std::string line(field);
        for (auto& c : line) if (c == '\n' || c == '\r' || c == '\0') c = ' ';
        const auto message = std::string(kind) + "\n" + line + "\n\n";
        texts.emplace_back(message.begin(), message.end());
        textKinds.push_back(false);
        return true;
    }
    // "rotate", quarter turns, nothing: which way up Android is to hold its display, 0 as it was made
    // and 1 turned a quarter; an app that asks for one orientation still gets it. Sent when the
    // person turns the device.
    bool submitRotation(int quarterTurns) {
        std::lock_guard lock(mutex);
        if (stopped || texts.size() >= 16 || quarterTurns < 0 || quarterTurns > 3) return false;
        const auto message = "rotate\n" + std::to_string(quarterTurns) + "\n\n";
        texts.emplace_back(message.begin(), message.end());
        textKinds.push_back(false);
        return true;
    }
    // The names of the apps the person installed: Android's label tool (tools/app-labels), run as shell,
    // writes "package<TAB>name" lines to labelsDevice. For the root prompt and Settings.
    static constexpr std::string_view labelsDevice = "/dev/nestvane-labels";
    static void addLabels(std::string_view lines) {
        std::lock_guard lock(labelMutex());
        while (!lines.empty()) {
            const auto end = lines.find('\n');
            const auto line = lines.substr(0, end);
            lines.remove_prefix(end == std::string_view::npos ? lines.size() : end + 1);
            const auto tab = line.find('\t');
            if (tab == std::string_view::npos || !tab || tab > 200 || line.size() - tab > 200) continue;
            labelMap()[std::string(line.substr(0, tab))] = std::string(line.substr(tab + 1));
        }
    }
    static std::string label(const std::string& package) {
        std::lock_guard lock(labelMutex());
        const auto found = labelMap().find(package);
        return found == labelMap().end() ? std::string() : found->second;
    }
    // "labels", "all", nothing: asks Android for the names again (an app may have been installed since).
    bool submitLabels() { return submitLaunch("all", "labels"); }

    // Which way up Android's display is now, in quarter turns: its rotation watcher
    // (tools/rotation-watch) writes a digit to rotationDevice at the start and at each change.
    static constexpr std::string_view rotationDevice = "/dev/nestvane-rotation";
    static constexpr std::string_view rotationWriter = "/system/bin/app_process64";
    std::atomic_int rotation{0};
    bool submitDisplayRate(int framesPerSecond) {
        std::lock_guard lock(mutex);
        if (stopped || texts.size() >= 16 || framesPerSecond < 0 || framesPerSecond > 240) return false;
        const auto message = "rate\n" + std::to_string(framesPerSecond) + "\n\n";
        texts.emplace_back(message.begin(), message.end());
        textKinds.push_back(false);
        return true;
    }
    bool takeText(std::vector<uint8_t>& message) {
        std::lock_guard lock(mutex);
        // The reader is back for more: what it took before has been dealt with, and a text message
        // among it may now be announced. First into Messaging, then to the apps, as on a phone; and
        // the two tools that do it, each a Java program starting up, do not start at the same moment.
        announceable += std::exchange(textsBeingStored, 0);
        if (texts.empty()) return false;
        message = std::move(texts.front());
        texts.erase(texts.begin());
        if (textKinds.front()) ++textsBeingStored;
        textKinds.erase(textKinds.begin());
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
    std::vector<std::vector<uint8_t>> rootRequests;
    static std::mutex& labelMutex() { static std::mutex value; return value; }
    static std::map<std::string, std::string>& labelMap() { static std::map<std::string, std::string> value; return value; }
    std::mutex mutex;
    std::shared_ptr<Request> pending;
    std::weak_ptr<Request> active;
    std::vector<std::vector<uint8_t>> texts;
    std::vector<std::vector<uint8_t>> broadcasts;
    std::vector<bool> textKinds;  // for each of texts: whether it is a text message
    unsigned textsBeingStored = 0, announceable = 0;
    bool stopped = false;
};

} // namespace nestvane
