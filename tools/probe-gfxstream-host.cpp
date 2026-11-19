// macOS probe for the Android Emulator SDK renderer, using upstream Gfxstream's public C ABI.
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"

#include <array>
#include <cstdio>
#include <dlfcn.h>

namespace {
void writeFence(void*, stream_renderer_fence*) {}

template<class Function>
Function load(void* module, const char* name) {
    return reinterpret_cast<Function>(dlsym(module, name));
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s path/to/libgfxstream_backend.dylib\n", argv[0]);
        return 2;
    }

    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 1;
    }

    const auto init = load<decltype(&stream_renderer_init)>(module, "stream_renderer_init");
    const auto destroy = load<decltype(&stream_renderer_teardown)>(module, "stream_renderer_teardown");
    const auto create = load<decltype(&stream_renderer_resource_create)>(module, "stream_renderer_resource_create");
    const auto upload = load<decltype(&stream_renderer_transfer_write_iov)>(module, "stream_renderer_transfer_write_iov");
    const auto readback = load<decltype(&stream_renderer_transfer_read_iov)>(module, "stream_renderer_transfer_read_iov");
    const auto unref = load<decltype(&stream_renderer_resource_unref)>(module, "stream_renderer_resource_unref");
    if (!init || !destroy || !create || !upload || !readback || !unref) {
        std::fprintf(stderr, "The renderer is missing a required Gfxstream C API symbol\n");
        return 1;
    }

    constexpr uint32_t side = 64;
    unsigned cookie = 0;
    stream_renderer_param params[] = {
        {STREAM_RENDERER_PARAM_USER_DATA, reinterpret_cast<uintptr_t>(&cookie)},
        {STREAM_RENDERER_PARAM_FENCE_CALLBACK, reinterpret_cast<uintptr_t>(&writeFence)},
        {STREAM_RENDERER_PARAM_RENDERER_FLAGS,
            STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT | STREAM_RENDERER_FLAGS_USE_GLES_BIT},
        {STREAM_RENDERER_PARAM_WIN0_WIDTH, side},
        {STREAM_RENDERER_PARAM_WIN0_HEIGHT, side},
    };
    const int initialized = init(params, std::size(params));
    if (initialized) {
        std::fprintf(stderr, "Gfxstream initialization failed (%d)\n", initialized);
        return 1;
    }

    constexpr uint32_t texture2D = 2; // PIPE_TEXTURE_2D
    constexpr uint32_t rgba8Unorm = 67; // VIRGL_FORMAT_R8G8B8A8_UNORM
    constexpr uint32_t samplerView = 1u << 3, scanout = 1u << 18;
    stream_renderer_resource_create_args resource{
        .handle = 1, .target = texture2D, .format = rgba8Unorm, .bind = samplerView | scanout,
        .width = side, .height = side, .depth = 1, .array_size = 1,
        .last_level = 0, .nr_samples = 0, .flags = 0,
    };
    constexpr uint32_t stride = side * 4, bytes = side * stride;
    std::array<uint8_t, bytes> expected{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = uint8_t((i * 97 + i / 13) & 255);
    auto backing = expected;
    iovec memory{backing.data(), backing.size()};
    const int created = create(&resource, &memory, 1);
    if (created) {
        std::fprintf(stderr, "Gfxstream color buffer creation failed (%d)\n", created);
        destroy();
        return 1;
    }

    stream_renderer_box region{0, 0, 0, side, side, 1};
    const int wrote = upload(resource.handle, 0, 0, stride, bytes, &region, 0, nullptr, 0);
    backing.fill(0);
    const int read = readback(resource.handle, 0, 0, stride, bytes, &region, 0, nullptr, 0);
    const bool matched = !wrote && !read && backing == expected;
    std::printf("RGBA8 upload: %d, readback: %d, matching bytes: %s (%u bytes)\n",
                wrote, read, matched ? "yes" : "no", bytes);
    unref(resource.handle);
    destroy();
    return matched ? 0 : 1;
}
