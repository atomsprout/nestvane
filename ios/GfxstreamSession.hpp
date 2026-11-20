#pragma once

#include "GfxstreamPipe.hpp"

#include <string>

namespace nestvane {

class GfxstreamSession {
public:
    GfxstreamSession(const char* library, uint32_t width, uint32_t height) {
        module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            const char* reason = dlerror();
            failure = reason ? reason : "cannot load gfxstream";
            return;
        }

        shutdown = reinterpret_cast<void (*)()>(dlsym(module, "stream_renderer_teardown"));
        displayRead = reinterpret_cast<DisplayRead>(dlsym(module, "gfxstream_guest_display_read"));
        pipes = loadGfxstreamPipeFactory(module);
        if (!shutdown || !pipes) {
            failure = "gfxstream is missing the Nestvane renderer interfaces";
            return;
        }
        if (const int status = startGfxstreamGuestRenderer(module, width, height, dma)) {
            failure = "gfxstream initialization failed with status " + std::to_string(status);
            return;
        }

        started = true;
    }

    ~GfxstreamSession() {
        if (dma) dma->stop();
        if (started) shutdown();
        if (module) dlclose(module);
    }

    GfxstreamSession(const GfxstreamSession&) = delete;
    GfxstreamSession& operator=(const GfxstreamSession&) = delete;

    explicit operator bool() const { return started; }
    const std::string& error() const { return failure; }
    GraphicsPipeFactory pipeFactory() const { return started ? pipes : GraphicsPipeFactory{}; }
    std::shared_ptr<GuestColorDma> colorDma() const { return dma; }

    bool hasDisplayReadback() const { return started && displayRead; }
    uint64_t readDisplay(uint64_t after, std::span<uint8_t> pixels, uint32_t (&dimensions)[2]) const {
        return hasDisplayReadback() ? displayRead(after, pixels.data(), pixels.size(), dimensions) : 0;
    }

private:
    using DisplayRead = uint64_t (*)(uint64_t, uint8_t*, size_t, uint32_t*);
    DisplayRead displayRead = nullptr;
    void* module = nullptr;
    void (*shutdown)() = nullptr;
    GraphicsPipeFactory pipes;
    std::shared_ptr<GuestColorDma> dma;
    std::string failure;
    bool started = false;
};

} // namespace nestvane
