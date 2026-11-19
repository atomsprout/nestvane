#pragma once

#include "GraphicsPipe.hpp"

#include <dlfcn.h>

namespace nestvane {

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

    return [api](std::string_view service) -> std::unique_ptr<GraphicsPipe> {
        if (service != "opengles") return {};
        if (auto* channel = api.open()) return std::make_unique<Channel>(api, channel);
        return {};
    };
}

} // namespace nestvane
