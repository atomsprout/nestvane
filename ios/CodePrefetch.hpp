#pragma once

// Reads in ahead of time the pages of translated code that a start of Android uses.
//
// The translated code is 2.6 GB in the app's frameworks, and a start runs through a few hundred
// megabytes of it. When the app has not run for a while those pages are no longer in memory, and the
// one thread that runs Android waits for each as it gets there: a start took 12 s instead of 7.5 s on
// an iPhone 17 Pro. After the home screen is up, record() notes which pages are in memory; at the next
// start, start() touches those pages from other threads, which the phone's other cores run while
// Android begins.

#include "DynamicProbe.hpp"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace nestvane::prefetch {

struct Image {
    std::string name;
    const uint8_t* text;
    uint64_t size;
};

inline std::vector<Image> images() {
    std::vector<Image> found;
    for (uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const char* path = _dyld_get_image_name(index);
        const char* name = path ? std::strrchr(path, '/') : nullptr;
        if (!name || std::strncmp(name + 1, "NestvaneCode", 12) != 0) continue;
        unsigned long size = 0;
        const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(index));
        const uint8_t* text = getsegmentdata(header, "__TEXT", &size);
        if (text && size) found.push_back({name + 1, text, size});
    }
    return found;
}

// The list is, for each image: its name and a newline, the size of its code, the number of pages, and
// one byte a page, 1 for a page that was in memory.
inline void record(const std::string& path) {
    const uint64_t page = uint64_t(getpagesize());
    const std::string unfinished = path + ".new";
    FILE* file = std::fopen(unfinished.c_str(), "wb");
    if (!file) return;
    bool complete = true;
    for (const auto& image : images()) {
        const uint64_t pages = (image.size + page - 1) / page;
        std::vector<char> present(pages);
        if (mincore(image.text, image.size, present.data()) != 0) { complete = false; break; }
        for (auto& byte : present) byte = byte & 1;  // MINCORE_INCORE
        std::fprintf(file, "%s\n", image.name.c_str());
        std::fwrite(&image.size, sizeof(image.size), 1, file);
        std::fwrite(&pages, sizeof(pages), 1, file);
        std::fwrite(present.data(), 1, pages, file);
    }
    complete = std::fclose(file) == 0 && complete;
    if (complete) std::rename(unfinished.c_str(), path.c_str());
    else std::remove(unfinished.c_str());
}

// Returns how many pages are being read in.
inline size_t start(const std::string& path, unsigned threads = 4) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return 0;
    const uint64_t page = uint64_t(getpagesize());
    const auto loaded = images();
    auto wanted = std::make_shared<std::vector<const uint8_t*>>();
    char name[256];
    while (std::fgets(name, sizeof(name), file)) {
        name[std::strcspn(name, "\n")] = 0;
        uint64_t size = 0, pages = 0;
        if (std::fread(&size, sizeof(size), 1, file) != 1 || std::fread(&pages, sizeof(pages), 1, file) != 1 ||
            pages > (uint64_t{1} << 24)) break;
        std::vector<char> present(pages);
        if (std::fread(present.data(), 1, pages, file) != pages) break;
        for (const auto& image : loaded) {
            // The same name with another size is another build's code: its pages mean nothing here.
            if (image.name != name || image.size != size) continue;
            for (uint64_t index = 0; index < pages; ++index)
                if (present[index]) wanted->push_back(image.text + index * page);
        }
    }
    std::fclose(file);
    for (unsigned thread = 0; thread < threads && !wanted->empty(); ++thread) {
        std::thread([wanted, thread, threads] {
            const size_t from = wanted->size() * thread / threads, to = wanted->size() * (thread + 1) / threads;
            unsigned sum = 0;
            for (size_t index = from; index < to; ++index) sum += *static_cast<const volatile uint8_t*>((*wanted)[index]);
            (void)sum;
        }).detach();
    }
    return wanted->size();
}

// The same for the files of the system image, which are mapped one by one as Android starts
// (visitMappedImageFiles). Paths are kept from |root| on, which moves when the app is installed anew.
inline void recordFiles(const std::string& path, const std::string& root) {
    const uint64_t page = uint64_t(getpagesize());
    const std::string unfinished = path + ".new";
    FILE* file = std::fopen(unfinished.c_str(), "wb");
    if (!file) return;
    visitMappedImageFiles([&](const MappedImageFile& mapped) {
        if (mapped.path.compare(0, root.size(), root) != 0 || mapped.size < page) return;
        const uint64_t size = mapped.size, pages = (size + page - 1) / page;
        std::vector<char> present(pages);
        if (mincore(mapped.data, size, present.data()) != 0) return;
        uint64_t count = 0;
        for (auto& byte : present) count += (byte = byte & 1);
        if (!count) return;
        std::fprintf(file, "%s\n", mapped.path.c_str() + root.size());
        std::fwrite(&size, sizeof(size), 1, file);
        std::fwrite(&pages, sizeof(pages), 1, file);
        std::fwrite(present.data(), 1, pages, file);
    });
    if (std::fclose(file) == 0) std::rename(unfinished.c_str(), path.c_str());
    else std::remove(unfinished.c_str());
}

// Reads the listed pages of the files, so that they are in memory when Android maps them.
inline size_t startFiles(const std::string& path, const std::string& root, unsigned threads = 3) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return 0;
    struct Run { std::string path; uint64_t from, length; };
    auto runs = std::make_shared<std::vector<Run>>();
    const uint64_t page = uint64_t(getpagesize());
    size_t total = 0;
    char name[2048];
    while (std::fgets(name, sizeof(name), file)) {
        name[std::strcspn(name, "\n")] = 0;
        uint64_t size = 0, pages = 0;
        if (std::fread(&size, sizeof(size), 1, file) != 1 || std::fread(&pages, sizeof(pages), 1, file) != 1 ||
            pages > (uint64_t{1} << 24)) break;
        std::vector<char> present(pages);
        if (std::fread(present.data(), 1, pages, file) != pages) break;
        for (uint64_t index = 0; index < pages;) {
            if (!present[index]) { ++index; continue; }
            uint64_t end = index;
            while (end < pages && present[end] && end - index < 64) ++end;
            runs->push_back({root + name, index * page, (end - index) * page});
            total += end - index;
            index = end;
        }
    }
    std::fclose(file);
    for (unsigned thread = 0; thread < threads && !runs->empty(); ++thread) {
        std::thread([runs, thread, threads] {
            std::vector<char> scratch(64 * 16384);
            const size_t from = runs->size() * thread / threads, to = runs->size() * (thread + 1) / threads;
            std::string open;
            int fd = -1;
            for (size_t index = from; index < to; ++index) {
                const auto& run = (*runs)[index];
                if (run.path != open) {
                    if (fd >= 0) ::close(fd);
                    fd = ::open(run.path.c_str(), O_RDONLY | O_CLOEXEC);
                    open = run.path;
                }
                if (fd >= 0) (void)::pread(fd, scratch.data(), std::min<uint64_t>(run.length, scratch.size()), off_t(run.from));
            }
            if (fd >= 0) ::close(fd);
        }).detach();
    }
    return total;
}

} // namespace nestvane::prefetch
