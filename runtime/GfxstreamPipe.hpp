#pragma once

#include "GraphicsPipe.hpp"

#include <algorithm>
#include <dlfcn.h>

namespace nestvane {

inline bool guestDisplayHasColorVariation(std::span<const uint8_t> rgba) {
    for (size_t i = 4; i + 3 < rgba.size(); i += 4) {
        if (rgba[i] != rgba[0] || rgba[i + 1] != rgba[1] || rgba[i + 2] != rgba[2]) return true;
    }
    return false;
}

class GfxstreamRefcountPipe final : public GraphicsPipe {
public:
    explicit GfxstreamRefcountPipe(void (*release)(uint32_t)) : release(release) {}
    ~GfxstreamRefcountPipe() override {
        if (received == sizeof(handle) && handle) release(handle);
    }

    int64_t read(std::span<uint8_t>) override { return -5; }
    int64_t write(std::span<const uint8_t> bytes) override {
        if (received == sizeof(handle)) return bytes.empty() ? 0 : -22;
        const auto count = std::min(bytes.size(), sizeof(handle) - received);
        for (const auto byte : bytes.first(count)) {
            handle |= uint32_t(byte) << (8 * received++);
        }
        return count;
    }
    uint16_t pollEvents() const override { return received < sizeof(handle) ? 4 : 0; }

private:
    void (*release)(uint32_t);
    uint32_t handle = 0;
    size_t received = 0;
};

class GfxstreamProcessPipe final : public GraphicsPipe {
public:
    GfxstreamProcessPipe(uint64_t (*start)(), void (*stop)(uint64_t)) : start(start), stop(stop) {}
    ~GfxstreamProcessPipe() override {
        if (id) stop(id);
    }

    int64_t read(std::span<uint8_t> bytes) override {
        if (bytes.empty()) return 0;
        if (failed) return -5;
        if (!id || sent == sizeof(id)) return -11;
        const auto count = std::min(bytes.size(), sizeof(id) - sent);
        for (auto& byte : bytes.first(count)) byte = uint8_t(id >> (8 * sent++));
        return count;
    }

    int64_t write(std::span<const uint8_t> bytes) override {
        if (bytes.empty()) return 0;
        if (failed) return -5;
        if (received == sizeof(confirmation)) return -22;
        const auto count = std::min(bytes.size(), sizeof(confirmation) - received);
        for (const auto byte : bytes.first(count)) confirmation |= uint32_t(byte) << (8 * received++);
        if (received == sizeof(confirmation)) {
            if (confirmation == 100) id = start();
            if (!id) {
                failed = true;
                return confirmation == 100 ? -5 : -22;
            }
        }
        return count;
    }

    uint16_t pollEvents() const override {
        if (failed) return 8;
        if (received < sizeof(confirmation)) return 4;
        return sent < sizeof(id) ? 1 : 0;
    }

private:
    uint64_t (*start)();
    void (*stop)(uint64_t);
    uint64_t id = 0;
    uint32_t confirmation = 0;
    size_t received = 0, sent = 0;
    bool failed = false;
};

// The module must stay loaded, with its renderer initialized, until all
// guest descriptors using the returned factory have been closed.
inline GraphicsPipeFactory loadGfxstreamPipeFactory(void* module) {
    struct API {
        void* (*open)();
        void (*close)(void*);
        int64_t (*read)(void*, uint8_t*, size_t);
        int64_t (*write)(void*, const uint8_t*, size_t);
        uint16_t (*poll)(void*);
    };
    if (!module) return {};
    const API api{
        reinterpret_cast<decltype(API::open)>(dlsym(module, "gfxstream_guest_pipe_open")),
        reinterpret_cast<decltype(API::close)>(dlsym(module, "gfxstream_guest_pipe_close")),
        reinterpret_cast<decltype(API::read)>(dlsym(module, "gfxstream_guest_pipe_read")),
        reinterpret_cast<decltype(API::write)>(dlsym(module, "gfxstream_guest_pipe_write")),
        reinterpret_cast<decltype(API::poll)>(dlsym(module, "gfxstream_guest_pipe_poll")),
    };
    if (!api.open || !api.close || !api.read || !api.write || !api.poll) return {};

    class Channel final : public GraphicsPipe {
    public:
        Channel(API api, void* channel) : api(api), channel(channel) {}
        ~Channel() override { api.close(channel); }
        int64_t read(std::span<uint8_t> bytes) override { return api.read(channel, bytes.data(), bytes.size()); }
        int64_t write(std::span<const uint8_t> bytes) override { return api.write(channel, bytes.data(), bytes.size()); }
        uint16_t pollEvents() const override { return api.poll(channel); }

    private:
        API api;
        void* channel;
    };

    const auto releaseColorBuffer = reinterpret_cast<void (*)(uint32_t)>(
        dlsym(module, "gfxstream_guest_color_buffer_release"));
    const auto startProcess = reinterpret_cast<uint64_t (*)()>(
        dlsym(module, "gfxstream_guest_graphics_process_start"));
    const auto stopProcess = reinterpret_cast<void (*)(uint64_t)>(
        dlsym(module, "gfxstream_guest_graphics_process_stop"));
    return [api, releaseColorBuffer, startProcess, stopProcess](std::string_view service)
        -> std::unique_ptr<GraphicsPipe> {
        if (service == "GLProcessPipe" && startProcess && stopProcess)
            return std::make_unique<GfxstreamProcessPipe>(startProcess, stopProcess);
        if (service == "refcount" && releaseColorBuffer)
            return std::make_unique<GfxstreamRefcountPipe>(releaseColorBuffer);
        if (service != "opengles") return {};
        if (auto* channel = api.open()) return std::make_unique<Channel>(api, channel);
        return {};
    };
}

} // namespace nestvane
