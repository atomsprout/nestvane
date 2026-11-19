#pragma once

#include "GraphicsPipe.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>

namespace nestvane {

class GuestCameraSource {
public:
    static constexpr unsigned width = 640, height = 480;
    enum class Status { starting, ready, unavailable };
    enum class Format { rgba, yuv420 };
    static constexpr size_t frameBytes(Format format) {
        constexpr size_t pixels = size_t(width) * height;
        return format == Format::rgba ? pixels * 4 : pixels * 3 / 2;
    }

    virtual ~GuestCameraSource() = default;
    virtual bool available() const = 0;
    virtual bool start() = 0; // Acquire the single rear camera lease.
    virtual Status status() const = 0;
    virtual bool copyFrame(Format format, std::span<uint8_t> destination) = 0;
    virtual void stop() = 0;
};

// Goldfish qemud camera commands terminate in NUL. Replies begin with eight
// ASCII hexadecimal digits counting the body, including its final NUL.
class GuestCameraPipe final : public GraphicsPipe {
public:
    static constexpr std::string_view catalog = "qemud:camera";
    static constexpr std::string_view rear = "qemud:camera:name=nestvane-rear";
    using WriteFrame = std::function<bool(uint64_t, GuestCameraSource::Format)>;

    GuestCameraPipe(std::shared_ptr<GuestCameraSource> camera, bool list, WriteFrame writeFrame)
        : camera(std::move(camera)), listOnly(list), writeFrame(std::move(writeFrame)) {}
    ~GuestCameraPipe() override {
        if (leased) camera->stop();
    }

    int64_t write(std::span<const uint8_t> bytes) override {
        if (bytes.empty()) return 0;
        if (!reply.empty() || pendingStart) return -11;
        constexpr size_t limit = 256;
        const auto input = bytes.first(std::min(bytes.size(), limit - request.size()));
        const auto end = std::ranges::find(input, uint8_t{});
        request.append(reinterpret_cast<const char*>(input.data()), end - input.begin());
        if (end == input.end()) {
            if (request.size() < limit) return input.size();
            request.clear();
            return -7;
        }
        handle(request);
        request.clear();
        return end - input.begin() + 1;
    }

    int64_t read(std::span<uint8_t> bytes) override {
        if (bytes.empty()) return 0;
        if (pendingStart) {
            const auto state = camera->status();
            if (state == GuestCameraSource::Status::starting) return -11;
            pendingStart = false;
            respond(state == GuestCameraSource::Status::ready ? "ok" : "ko:camera unavailable");
        }
        if (reply.empty()) return -11;
        const auto length = std::min(bytes.size(), reply.size() - offset);
        std::memcpy(bytes.data(), reply.data() + offset, length);
        offset += length;
        if (offset == reply.size()) {
            reply.clear();
            offset = 0;
        }
        return length;
    }

    uint16_t pollEvents() const override {
        if (pendingStart) return camera->status() == GuestCameraSource::Status::starting ? 0 : 1;
        return reply.empty() ? 4 : 1;
    }

private:
    std::shared_ptr<GuestCameraSource> camera;
    bool listOnly, connected = false, leased = false, pendingStart = false;
    WriteFrame writeFrame;
    std::string request, reply;
    size_t offset = 0;

    void respond(std::string_view body) {
        std::array<char, 9> header{};
        std::snprintf(header.data(), header.size(), "%08x", unsigned(body.size() + 1));
        reply.assign(header.data(), 8);
        reply.append(body);
        reply.push_back('\0');
    }

    static std::string_view token(std::string_view tokens, std::string_view key) {
        while (!tokens.empty()) {
            const auto end = tokens.find(' ');
            const auto field = tokens.substr(0, end);
            if (field.starts_with(key)) return field.substr(key.size());
            if (end == std::string_view::npos) break;
            tokens.remove_prefix(end + 1);
        }
        return {};
    }

    template<class T>
    static bool parse(std::string_view field, T& value) {
        if (field.empty()) return false;
        const auto end = field.data() + field.size();
        const auto parsed = std::from_chars(field.data(), end, value);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    bool frame(std::string_view fields) {
        if (!leased || camera->status() != GuestCameraSource::Status::ready ||
            token(fields, "dim=") != "640x480") return false;
        constexpr uint32_t rgba = 876758866, yuv420 = 842093913; // V4L2 RGB32 and YUV420.
        uint32_t format = 0;
        uint64_t destination = 0;
        if (!parse(token(fields, "pix="), format) || !parse(token(fields, "offset="), destination) ||
            (format != rgba && format != yuv420)) return false;
        return writeFrame(destination, format == rgba ? GuestCameraSource::Format::rgba : GuestCameraSource::Format::yuv420);
    }

    void handle(std::string_view command) {
        if (listOnly) {
            if (command != "list") return respond("ko:invalid catalog command");
            return respond(camera->available()
                ? "ok:name=nestvane-rear channel=0 pix=876758866 dir=back framedims=640x480 sensor_orientation=90\n"
                : "ok:");
        }
        if (command == "connect" && camera->available()) {
            connected = true;
            return respond("ok");
        }
        if (command == "start" && connected && !leased) {
            if (!(leased = camera->start())) return respond("ko:camera unavailable");
            pendingStart = true;
            return;
        }
        if ((command == "stop" || command == "disconnect") && connected) {
            if (leased) camera->stop();
            leased = false;
            connected = command == "stop";
            return respond("ok");
        }
        if (command.starts_with("frame ") && frame(command.substr(6))) return respond("ok");
        respond("ko:camera command failed");
    }
};

} // namespace nestvane
