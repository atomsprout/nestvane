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

        const auto start = reinterpret_cast<int (*)(uint32_t, uint32_t)>(dlsym(module, "gfxstream_guest_renderer_start"));
        shutdown = reinterpret_cast<void (*)()>(dlsym(module, "stream_renderer_teardown"));
        pipes = loadGfxstreamPipeFactory(module);
        if (!start || !shutdown || !pipes) {
            failure = "gfxstream is missing the Nestvane renderer interfaces";
            return;
        }
        if (const int status = start(width, height)) {
            failure = "gfxstream initialization failed with status " + std::to_string(status);
            return;
        }

        started = true;
    }

    ~GfxstreamSession() {
        if (started) shutdown();
        if (module) dlclose(module);
    }

    GfxstreamSession(const GfxstreamSession&) = delete;
    GfxstreamSession& operator=(const GfxstreamSession&) = delete;

    explicit operator bool() const { return started; }
    const std::string& error() const { return failure; }
    GraphicsPipeFactory pipeFactory() const { return started ? pipes : GraphicsPipeFactory{}; }

private:
    void* module = nullptr;
    void (*shutdown)() = nullptr;
    GraphicsPipeFactory pipes;
    std::string failure;
    bool started = false;
};

} // namespace nestvane
