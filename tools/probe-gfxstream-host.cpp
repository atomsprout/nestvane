// macOS probe for the Android Emulator SDK renderer, using upstream Gfxstream's public C ABI.
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "AArch64.hpp"
#include "DynamicProbe.hpp"
#include "GfxstreamPipe.hpp"
#include "ProbeLinux.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace {
void writeFence(void*, stream_renderer_fence*) {}

template<class Function>
Function load(void* module, const char* name) {
    return reinterpret_cast<Function>(dlsym(module, name));
}

struct RendererTransfer {
    decltype(&stream_renderer_resource_create) create;
    decltype(&stream_renderer_transfer_write_iov) write;
    decltype(&stream_renderer_transfer_read_iov) read;
    decltype(&stream_renderer_resource_unref) release;
};

class VersionProbePipe final : public nestvane::GraphicsPipe {
public:
    explicit VersionProbePipe(const RendererTransfer& transfer) : transfer(transfer) {}
    ~VersionProbePipe() override { if (created) transfer.release(id); }

    bool attach(decltype(&stream_renderer_ctx_attach_resource) attachResource) {
        constexpr uint32_t pipeBuffer = 0, r8Unorm = 64, customBind = 1u << 17;
        stream_renderer_resource_create_args args{id, pipeBuffer, r8Unorm, customBind, uint32_t(backing.size()), 1, 1, 1, 0, 0, 0};
        iovec memory{backing.data(), backing.size()};
        if (transfer.create(&args, &memory, 1)) return false;
        created = true;
        attachResource(id, id);
        constexpr char service[] = "pipe:opengles";
        return send(std::span(reinterpret_cast<const uint8_t*>(service), sizeof(service)));
    }

    int64_t write(std::span<const uint8_t> bytes) override {
        // Only the known round trip is exercised here. The SDK transfer API
        // waits synchronously for its reply, so never send other commands.
        constexpr std::array<uint8_t, 12> query{0, 0, 0, 0, 0x10, 0x27, 0, 0, 8, 0, 0, 0};
        if (sent || !std::ranges::equal(bytes, query)) return -22;
        if (!send(bytes)) return -5;
        sent = true;
        stream_renderer_box area{0, 0, 0, responseBytes, 1, 1};
        if (transfer.read(id, id, 0, 0, 0, &area, 0, nullptr, 0)) return -5;
        remaining = responseBytes;
        return bytes.size();
    }

    int64_t read(std::span<uint8_t> bytes) override {
        if (!remaining) return -11;
        const auto count = std::min<size_t>(bytes.size(), remaining);
        std::copy_n(backing.begin() + responseBytes - remaining, count, bytes.begin());
        remaining -= count;
        return count;
    }

    uint16_t pollEvents() const override { return (remaining ? 1 : 0) | (sent ? 0 : 4); }

private:
    bool send(std::span<const uint8_t> bytes) {
        std::copy(bytes.begin(), bytes.end(), backing.begin());
        stream_renderer_box area{0, 0, 0, uint32_t(bytes.size()), 1, 1};
        return !transfer.write(id, id, 0, 0, 0, &area, 0, nullptr, 0);
    }

    static constexpr uint32_t id = 2, responseBytes = 4;
    const RendererTransfer& transfer;
    std::array<uint8_t, 64> backing{};
    size_t remaining = 0;
    bool created = false, sent = false;
};

bool runGuestVersionProbe(const char* elf, nestvane::GraphicsPipeFactory factory) {
    auto guest = nestvane::Machine(nestvane::loadELF(nestvane::readFile(elf)));
    nestvane::ProbeLinux linux(1, 0);
    linux.setGraphicsPipeFactory(std::move(factory));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (unsigned step = 0; step < 256 && guest.cpu.stop == nestvane::Stop::running; ++step) {
        if (!linux.schedule(guest, deadline)) break;
        uint32_t word = 0;
        const auto* bytes = guest.read(guest.cpu.pc, sizeof(word), 1);
        if (!bytes) break;
        std::memcpy(&word, bytes, sizeof word);
        const auto instruction = nestvane::decode(word);
        if (instruction.op == nestvane::Op::svc && guest.cpu.x[8] != 93) {
            if (!linux.syscall(guest)) break;
            if (!linux.blocked()) guest.cpu.pc += 4;
        } else if (instruction.op != nestvane::Op::unsupported) {
            nestvane::executeDecoded(guest, instruction);
        } else if (!nestvane::executeStartupInstruction(guest, word)) {
            break;
        }
    }
    const bool passed = guest.cpu.stop == nestvane::Stop::exit && guest.cpu.exitCode == 0;
    if (!passed)
        std::fprintf(stderr, "Guest stopped at PC %#llx, x8=%llu, exit=%d\n",
                     static_cast<unsigned long long>(guest.cpu.pc),
                     static_cast<unsigned long long>(guest.cpu.x[8]), guest.cpu.exitCode);
    return passed;
}

bool runSdkVersionProbe(void* module, const RendererTransfer& transfer, const char* elf) {
    const auto create = load<decltype(&stream_renderer_context_create)>(module, "stream_renderer_context_create");
    const auto attach = load<decltype(&stream_renderer_ctx_attach_resource)>(module, "stream_renderer_ctx_attach_resource");
    const auto destroy = load<decltype(&stream_renderer_context_destroy)>(module, "stream_renderer_context_destroy");
    constexpr uint32_t contextId = 2, glesCapset = 8;
    constexpr char contextName[] = "nestvane";
    if (!create || !attach || !destroy || create(contextId, sizeof(contextName) - 1, contextName, glesCapset))
        return false;
    const bool passed = runGuestVersionProbe(elf, [&](std::string_view service) -> std::unique_ptr<nestvane::GraphicsPipe> {
        if (service != "opengles") return {};
        auto pipe = std::make_unique<VersionProbePipe>(transfer);
        if (!pipe->attach(attach)) return {};
        return pipe;
    });
    destroy(contextId);
    return passed;
}
}

namespace {
std::jthread watchGuestDisplay(void* module, uint32_t width, uint32_t height) {
    const auto read = load<uint64_t (*)(uint64_t, uint8_t*, size_t, uint32_t*)>(
        module, "gfxstream_guest_display_read");
    if (!read) return {};
    return std::jthread([=](std::stop_token stop) {
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        while (!stop.stop_requested()) {
            uint32_t dimensions[2]{};
            if (const auto sequence = read(0, pixels.data(), pixels.size(), dimensions)) {
                std::fprintf(stderr, "[Android boot] Primary display posted a %ux%u frame (sequence %llu)\n",
                             dimensions[0], dimensions[1], static_cast<unsigned long long>(sequence));
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
}

bool validatePostedDisplay(void* module, uint32_t handle, uint32_t side, std::span<const uint8_t> expected) {
    const auto read = load<uint64_t (*)(uint64_t, uint8_t*, size_t, uint32_t*)>(
        module, "gfxstream_guest_display_read");
    if (!read) return true; // The unmodified SDK library has no Nestvane display ABI.
    const auto flush = load<void (*)(uint32_t)>(module, "stream_renderer_flush");
    if (!flush) return false;

    std::vector<uint8_t> pixels(expected.size());
    uint32_t dimensions[2]{};
    if (read(0, pixels.data(), pixels.size(), dimensions)) return false;

    flush(handle);
    if (read(0, pixels.data(), pixels.size() - 1, dimensions)) return false;
    const uint64_t frame = read(0, pixels.data(), pixels.size(), dimensions);
    const bool matched = frame && dimensions[0] == side && dimensions[1] == side &&
        std::ranges::equal(pixels, expected);
    const bool consumed = !read(frame, pixels.data(), pixels.size(), dimensions);
    flush(handle);
    const bool reused = read(frame, pixels.data(), pixels.size(), dimensions) > frame;
    std::printf("Primary display post: frame=%llu, %ux%u, matching RGBA8=%s, new post to same buffer=%s\n",
                static_cast<unsigned long long>(frame), dimensions[0], dimensions[1],
                matched ? "yes" : "no", reused ? "yes" : "no");
    return matched && consumed && reused;
}
} // namespace

int main(int argc, char** argv) {
    const bool boot = (argc == 4 || argc == 5) && std::string_view(argv[2]) == "--init";
    if (argc != 2 && argc != 3 && !boot) {
        std::fprintf(stderr, "Usage: %s renderer.dylib [guest.elf | --init guest-root [database]]\n", argv[0]);
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
        {STREAM_RENDERER_PARAM_WIN0_WIDTH, boot ? 640u : side},
        {STREAM_RENDERER_PARAM_WIN0_HEIGHT, boot ? 960u : side},
    };
    const auto startGuestRenderer = load<int (*)(uint32_t, uint32_t)>(module, "gfxstream_guest_renderer_start");
    const int initialized = boot && startGuestRenderer ? startGuestRenderer(640, 960)
                                                       : init(params, std::size(params));
    if (initialized) {
        std::fprintf(stderr, "Gfxstream initialization failed (%d)\n", initialized);
        return 1;
    }

    if (boot) {
        auto factory = nestvane::loadGfxstreamPipeFactory(module);
        if (!factory) {
            std::fprintf(stderr, "A renderer with the nonblocking guest pipe patch is required for Android boot\n");
            destroy();
            return 1;
        }
        const auto report = [&] {
            auto display = watchGuestDisplay(module, 640, 960);
            return nestvane::probeInit(argv[3], argc == 5 ? argv[4] : "", [](std::string_view event) {
                std::fprintf(stderr, "[Android boot] %.*s\n", int(event.size()), event.data());
            }, std::move(factory));
        }();
        std::fputs(report.report.c_str(), stdout);
        destroy();
        return report.passed ? 0 : 1;
    }
    if (argc == 3) {
        bool passed = false;
        try {
            if (auto factory = nestvane::loadGfxstreamPipeFactory(module)) {
                std::puts("Using the nonblocking Gfxstream guest pipe");
                passed = runGuestVersionProbe(argv[2], std::move(factory));
            } else {
                passed = runSdkVersionProbe(module, {create, upload, readback, unref}, argv[2]);
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
        }
        std::printf("ARM64 guest Goldfish renderer version query: %s\n", passed ? "PASS" : "FAIL");
        destroy();
        return passed ? 0 : 1;
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
    const bool displayed = matched && validatePostedDisplay(module, resource.handle, side, expected);
    unref(resource.handle);
    destroy();
    return displayed ? 0 : 1;
}
