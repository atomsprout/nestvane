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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nestvane::prefetch {

struct Image {
    std::string name;
    const uint8_t* text;
    uint64_t size;
    std::string path = {};
};

// How far the reading has got, for the report of a start: pages asked for and read of the code and of
// the system image's files, and when the last thread of each was done (milliseconds from when it
// began, 0 while it is not).
struct Progress {
    std::atomic<uint64_t> codeWanted{0}, codeRead{0}, codeDoneAt{0}, codeThreads{0};
    std::atomic<uint64_t> fileWanted{0}, fileRead{0}, fileDoneAt{0}, fileThreads{0};
    std::atomic<uint64_t> lead{0};  // how long the reading had been going when Android's start began
    std::atomic<uint64_t> skipped{0};  // pages of files that were in memory already and were not read
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    uint64_t now() const {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count());
    }
};
inline Progress progress;

// What the lists this start began with named, by image name and by file path: one byte a page.
// A list is made from the pages that are in the system's memory when the home screen is up, and that
// is more than the start used: what was read ahead is there whether it was needed or not, and so is
// whatever an earlier session left. Left to itself a list only grows (41,000 pages of code became
// 54,000 in a day of starts, and a start with nothing in memory went from 8.5 s to 10 s). So a page
// is only kept in a list if the list the start began with had it, or the start can be shown to have
// used it; a list is made from everything in memory only when there was none before, which is the
// first start after installing, when nothing else is in memory yet.
// NESTVANE_PREFETCH_RESET=1 starts without the lists, so that this start makes them anew.
inline std::vector<std::pair<std::string, std::vector<char>>> previousCode;
inline std::vector<std::pair<std::string, std::vector<char>>> previousFiles;
inline bool resetting() {
    static const bool reset = std::getenv("NESTVANE_PREFETCH_RESET") != nullptr;
    return reset;
}
inline const std::vector<char>* previousOf(const std::vector<std::pair<std::string, std::vector<char>>>& lists, const std::string& name) {
    for (const auto& [listed, pages] : lists)
        if (listed == name) return &pages;
    return nullptr;
}

inline std::string status() {
    char text[240];
    std::snprintf(text, sizeof(text), "read ahead (begun %llu ms before): code %llu of %llu pages%s, files %llu of %llu pages%s",
                  static_cast<unsigned long long>(progress.lead.load()),
                  static_cast<unsigned long long>(progress.codeRead.load()), static_cast<unsigned long long>(progress.codeWanted.load()),
                  progress.codeDoneAt ? (" (done at " + std::to_string(progress.codeDoneAt.load()) + " ms)").c_str() : "",
                  static_cast<unsigned long long>(progress.fileRead.load()), static_cast<unsigned long long>(progress.fileWanted.load()),
                  progress.fileDoneAt ? (" (done at " + std::to_string(progress.fileDoneAt.load()) + " ms)").c_str() : "");
    return std::string(text) + ", " + std::to_string(progress.skipped.load()) + " in memory already";
}

inline std::vector<Image> images() {
    std::vector<Image> found;
    for (uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const char* path = _dyld_get_image_name(index);
        const char* name = path ? std::strrchr(path, '/') : nullptr;
        if (!name || std::strncmp(name + 1, "NestvaneCode", 12) != 0) continue;
        unsigned long size = 0;
        const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(index));
        const uint8_t* text = getsegmentdata(header, "__TEXT", &size);
        if (text && size) found.push_back({name + 1, text, size, path});
    }
    return found;
}

// How many threads read: NESTVANE_PREFETCH_THREADS=<code>,<files> for trying other numbers.
inline unsigned threadCount(unsigned which, unsigned fallback) {
    const char* text = std::getenv("NESTVANE_PREFETCH_THREADS");
    if (!text) return fallback;
    unsigned code = 0, files = 0;
    if (std::sscanf(text, "%u,%u", &code, &files) != 2) return fallback;
    const unsigned asked = which ? files : code;
    return asked >= 1 && asked <= 64 ? asked : fallback;
}

// What is done with a stretch of a file that the system holds in memory already: it is read all the
// same, as before 1.24. NESTVANE_PREFETCH_HELD=skip leaves it alone and =touch reads a byte of each
// of its pages through a mapping. Left alone looked best when the three took turns (median 7465 ms
// against 7534 read and 7757 touched), but with every start leaving them alone the pages a start read
// from storage crept up (32,000 to 37,000) and the starts got slower (7.5-8.1 s): being read again
// at each start is part of what keeps these pages in the system's memory.
inline int heldMode() {
    static const int mode = [] {
        const char* text = std::getenv("NESTVANE_PREFETCH_HELD");
        return !text ? 0 : !std::strcmp(text, "skip") ? 1 : !std::strcmp(text, "touch") ? 2 : 0;
    }();
    return mode;
}

// Whether a stretch of a file is in the system's memory, asked through a mapping made for the question.
inline bool inMemory(int fd, uint64_t from, uint64_t length, std::vector<char>& present) {
    if (heldMode() == 0) return false;
    const uint64_t page = uint64_t(getpagesize()), pages = (length + page - 1) / page;
    void* mapped = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, off_t(from));
    if (mapped == MAP_FAILED) return false;
    if (present.size() < pages) present.resize(pages);
    bool all = mincore(mapped, length, present.data()) == 0;
    for (uint64_t index = 0; all && index < pages; ++index) all = present[index] & 1;
    if (all && heldMode() == 2) {
        unsigned sum = 0;
        for (uint64_t index = 0; index < pages; ++index) sum += static_cast<const volatile uint8_t*>(mapped)[index * page];
        (void)sum;
    }
    munmap(mapped, length);
    return all;
}

// The list of the code's pages: "NVPF2", the images (name, newline, size of the code), then the pages
// that were in memory, each as image and page number, in the order a start comes to them.
// |order| is the translated code in the order the start first looked it up (native::tablePlanCode):
// a page takes its place from the first stretch that touches it, and a page that no stretch touches
// (code reached without a look-up) goes with the nearest page of its image that one does.
// NESTVANE_PREFETCH_UNORDERED=1 at the next start reads the same pages in the order of their
// addresses, as before 1.24, for comparing.
inline void record(const std::string& path, const std::vector<std::pair<uint64_t, uint64_t>>& order = {}) {
    const uint64_t page = uint64_t(getpagesize());
    const auto loaded = images();
    constexpr uint32_t none = UINT32_MAX;
    std::vector<std::vector<char>> present(loaded.size());
    std::vector<std::vector<uint32_t>> place(loaded.size());
    for (size_t index = 0; index < loaded.size(); ++index) {
        const uint64_t pages = (loaded[index].size + page - 1) / page;
        present[index].resize(pages);
        if (mincore(loaded[index].text, loaded[index].size, present[index].data()) != 0) return;
        for (auto& byte : present[index]) byte = byte & 1;  // MINCORE_INCORE
        place[index].assign(pages, none);
    }
    // Kept: what the list this start began with had, and what the plan's stretches touch (the start
    // looked that code up, so it used it). With no list to begin with, everything that is in memory.
    for (size_t index = 0; index < loaded.size(); ++index) {
        const auto* before = previousOf(previousCode, loaded[index].name);
        if (!before || before->size() != present[index].size()) continue;
        std::vector<char> planned(present[index].size(), 0);
        const uint64_t text = reinterpret_cast<uint64_t>(loaded[index].text);
        for (const auto& [from, to] : order) {
            if (from < text || from >= text + loaded[index].size || to <= from) continue;
            for (uint64_t at = (from - text) / page; at <= (std::min(to, text + loaded[index].size) - 1 - text) / page; ++at) planned[at] = 1;
        }
        for (size_t at = 0; at < present[index].size(); ++at)
            if (!(*before)[at] && !planned[at]) present[index][at] = 0;
    }
    uint32_t next = 0;
    for (const auto& [from, to] : order) {
        for (size_t index = 0; index < loaded.size(); ++index) {
            const uint64_t text = reinterpret_cast<uint64_t>(loaded[index].text);
            if (from < text || from >= text + loaded[index].size || to <= from) continue;
            for (uint64_t at = (from - text) / page; at <= (std::min(to, text + loaded[index].size) - 1 - text) / page; ++at)
                if (present[index][at] && place[index][at] == none) place[index][at] = next++;
            break;
        }
    }
    struct Entry { uint32_t place, image, page; };
    std::vector<Entry> entries;
    for (size_t index = 0; index < loaded.size(); ++index) {
        auto& places = place[index];
        uint32_t last = none;
        for (size_t at = 0; at < places.size(); ++at) {
            if (places[at] != none) last = places[at];
            else if (present[index][at]) places[at] = last;
        }
        last = next;  // before the image's first placed page: with that page; none placed: at the end
        for (size_t at = places.size(); at-- > 0;) {
            if (places[at] != none) last = places[at];
            else if (present[index][at]) places[at] = last;
        }
        for (size_t at = 0; at < places.size(); ++at)
            if (present[index][at]) entries.push_back({places[at], uint32_t(index), uint32_t(at)});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.place < b.place; });
    const std::string unfinished = path + ".new";
    FILE* file = std::fopen(unfinished.c_str(), "wb");
    if (!file) return;
    std::fputs("NVPF2\n", file);
    const uint64_t count = loaded.size(), total = entries.size();
    std::fwrite(&count, sizeof(count), 1, file);
    for (const auto& image : loaded) {
        std::fprintf(file, "%s\n", image.name.c_str());
        std::fwrite(&image.size, sizeof(image.size), 1, file);
    }
    std::fwrite(&total, sizeof(total), 1, file);
    for (const auto& entry : entries) {
        const uint32_t pair[2] = {entry.image, entry.page};
        std::fwrite(pair, sizeof(pair), 1, file);
    }
    if (std::fclose(file) == 0) std::rename(unfinished.c_str(), path.c_str());
    else std::remove(unfinished.c_str());
}

// Returns how many pages are being read in. The threads go through the list together, each taking
// every so-many-th page, so that what the start needs first is read first.
inline size_t start(const std::string& path, unsigned threads = threadCount(0, 4)) {
    FILE* file = resetting() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!file) return 0;
    const uint64_t page = uint64_t(getpagesize());
    const auto loaded = images();
    auto wanted = std::make_shared<std::vector<const uint8_t*>>();
    char name[256];
    uint64_t count = 0, total = 0;
    // A list of an earlier version, or of another build's code, is not used: this start writes a new one.
    bool valid = std::fgets(name, sizeof(name), file) && !std::strcmp(name, "NVPF2\n") &&
                 std::fread(&count, sizeof(count), 1, file) == 1 && count == loaded.size();
    std::vector<const Image*> listed;
    for (uint64_t index = 0; valid && index < count; ++index) {
        uint64_t size = 0;
        valid = std::fgets(name, sizeof(name), file) && std::fread(&size, sizeof(size), 1, file) == 1;
        if (!valid) break;
        name[std::strcspn(name, "\n")] = 0;
        const auto image = std::find_if(loaded.begin(), loaded.end(), [&](const Image& each) { return each.name == name && each.size == size; });
        valid = image != loaded.end();
        if (valid) listed.push_back(&*image);
    }
    valid = valid && std::fread(&total, sizeof(total), 1, file) == 1 && total <= (uint64_t{1} << 24);
    for (uint64_t index = 0; valid && index < total; ++index) {
        uint32_t pair[2];
        if (std::fread(pair, sizeof(pair), 1, file) != 1) break;
        if (pair[0] < listed.size() && uint64_t{pair[1]} * page < listed[pair[0]]->size)
            wanted->push_back(listed[pair[0]]->text + uint64_t{pair[1]} * page);
    }
    std::fclose(file);
    // What this list names, for the list that this start will make (record).
    if (!wanted->empty()) {
        previousCode.clear();
        for (const auto& image : loaded) previousCode.emplace_back(image.name, std::vector<char>((image.size + page - 1) / page, 0));
        for (const uint8_t* address : *wanted)
            for (size_t index = 0; index < loaded.size(); ++index)
                if (address >= loaded[index].text && address < loaded[index].text + loaded[index].size) {
                    previousCode[index].second[uint64_t(address - loaded[index].text) / page] = 1;
                    break;
                }
    }
    if (std::getenv("NESTVANE_PREFETCH_UNORDERED")) std::sort(wanted->begin(), wanted->end());
    progress.codeWanted = wanted->size();
    progress.codeThreads = wanted->empty() ? 0 : threads;
    for (unsigned thread = 0; thread < threads && !wanted->empty(); ++thread) {
        std::thread([wanted, thread, threads] {
            unsigned sum = 0;
            size_t read = 0;
            for (size_t index = thread; index < wanted->size(); index += threads) {
                sum += *static_cast<const volatile uint8_t*>((*wanted)[index]);
                if (++read % 64 == 0) progress.codeRead += 64;
            }
            (void)sum;
            progress.codeRead += read % 64;
            if (--progress.codeThreads == 0) progress.codeDoneAt = std::max<uint64_t>(progress.now(), 1);
        }).detach();
    }
    return wanted->size();
}

// The same for the files of the system image, which are mapped one by one as Android starts
// (visitMappedImageFiles). Paths are kept from |root| on, which moves when the app is installed anew.
inline void recordFiles(const std::string& path, const std::string& root) {
    const uint64_t page = uint64_t(getpagesize());
    // In the order Android first opened them (MappedImageFile::order); those it never opened, which the
    // runtime itself reads as it sets up, come first.
    struct Listed { uint32_t order; std::string name; uint64_t size; std::vector<char> present; };
    std::vector<Listed> listed;
    visitMappedImageFiles([&](const MappedImageFile& mapped) {
        if (mapped.path.compare(0, root.size(), root) != 0 || mapped.size < page) return;
        const uint64_t size = mapped.size, pages = (size + page - 1) / page;
        std::vector<char> present(pages);
        if (mincore(mapped.data, size, present.data()) != 0) return;
        // Of a file the list this start began with had: only the pages it had there.
        const auto* before = previousOf(previousFiles, mapped.path);
        if (before && before->size() != pages) before = nullptr;
        uint64_t count = 0;
        for (uint64_t at = 0; at < pages; ++at) count += (present[at] = (present[at] & 1) && (!before || (*before)[at]));
        if (!count) return;
        listed.push_back({mapped.order, mapped.path.substr(root.size()), size, std::move(present)});
    });
    std::stable_sort(listed.begin(), listed.end(), [](const Listed& a, const Listed& b) { return a.order < b.order; });
    const std::string unfinished = path + ".new";
    FILE* file = std::fopen(unfinished.c_str(), "wb");
    if (!file) return;
    for (const auto& each : listed) {
        const uint64_t pages = each.present.size();
        std::fprintf(file, "%s\n", each.name.c_str());
        std::fwrite(&each.size, sizeof(each.size), 1, file);
        std::fwrite(&pages, sizeof(pages), 1, file);
        std::fwrite(each.present.data(), 1, pages, file);
    }
    if (std::fclose(file) == 0) std::rename(unfinished.c_str(), path.c_str());
    else std::remove(unfinished.c_str());
}

// Reads the listed pages of the files, so that they are in memory when Android maps them.
inline size_t startFiles(const std::string& path, const std::string& root, unsigned threads = threadCount(1, 3)) {
    FILE* file = resetting() ? nullptr : std::fopen(path.c_str(), "rb");
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
        previousFiles.emplace_back(root + name, std::move(present));
    }
    std::fclose(file);
    if (std::getenv("NESTVANE_PREFETCH_UNORDERED"))
        std::stable_sort(runs->begin(), runs->end(), [](const Run& a, const Run& b) { return a.path < b.path; });
    progress.fileWanted += total;
    progress.fileThreads += runs->empty() ? 0 : threads;
    for (unsigned thread = 0; thread < threads && !runs->empty(); ++thread) {
        std::thread([runs, thread, threads, page] {
            std::vector<char> scratch(64 * 16384), present(64);
            std::string open;
            int fd = -1;
            // Every so-many-th stretch, as with the code: the files Android opens first are read first.
            for (size_t index = thread; index < runs->size(); index += threads) {
                const auto& run = (*runs)[index];
                if (run.path != open) {
                    if (fd >= 0) ::close(fd);
                    fd = ::open(run.path.c_str(), O_RDONLY | O_CLOEXEC);
                    open = run.path;
                }
                // What the system still holds is not read again: on a start that finds its files in
                // memory that is all of it, and reading it would only copy hundreds of megabytes.
                if (fd >= 0 && !inMemory(fd, run.from, run.length, present))
                    (void)::pread(fd, scratch.data(), std::min<uint64_t>(run.length, scratch.size()), off_t(run.from));
                else if (fd >= 0) progress.skipped += run.length / page;
                progress.fileRead += run.length / page;
            }
            if (fd >= 0) ::close(fd);
            if (--progress.fileThreads == 0) progress.fileDoneAt = std::max<uint64_t>(progress.now(), 1);
        }).detach();
    }
    return total;
}

// The same for a directory of files that are not of the system image: Android's own large files (apps'
// packages and compiled code), which the runtime keeps as files beside its database. Which of their
// pages are in the system's memory is asked through a mapping made for the question.
inline void recordDirectory(const std::string& path, const std::string& directory) {
    const uint64_t page = uint64_t(getpagesize());
    const std::string unfinished = path + ".new";
    FILE* file = std::fopen(unfinished.c_str(), "wb");
    if (!file) return;
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* entry = readdir(listing)) {
            const std::string name = entry->d_name, full = directory + "/" + name;
            struct stat status{};
            if (name[0] == '.' || lstat(full.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || uint64_t(status.st_size) < page) continue;
            const int fd = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            const uint64_t size = uint64_t(status.st_size), pages = (size + page - 1) / page;
            void* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
            ::close(fd);
            if (mapped == MAP_FAILED) continue;
            std::vector<char> present(pages);
            uint64_t count = 0;
            const auto* before = previousOf(previousFiles, full);
            if (before && before->size() != pages) before = nullptr;
            if (mincore(mapped, size, present.data()) == 0)
                for (uint64_t at = 0; at < pages; ++at) count += (present[at] = (present[at] & 1) && (!before || (*before)[at]));
            munmap(mapped, size);
            if (!count) continue;
            std::fprintf(file, "/%s\n", name.c_str());
            std::fwrite(&size, sizeof(size), 1, file);
            std::fwrite(&pages, sizeof(pages), 1, file);
            std::fwrite(present.data(), 1, pages, file);
        }
        closedir(listing);
    }
    if (std::fclose(file) == 0) std::rename(unfinished.c_str(), path.c_str());
    else std::remove(unfinished.c_str());
}

// Reads a whole file through, in large pieces, on a thread of its own: the database of Android's
// data, which the start reads all of before anything else.
inline void startWhole(const std::string& path) {
    std::thread([path] {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        std::vector<char> scratch(1 << 20), present;
        struct stat status{};
        // A megabyte at a time, and only what the system does not hold already.
        for (uint64_t at = 0; fstat(fd, &status) == 0 && at < uint64_t(status.st_size); at += scratch.size()) {
            const uint64_t length = std::min<uint64_t>(scratch.size(), uint64_t(status.st_size) - at);
            if (!inMemory(fd, at, length, present)) (void)::pread(fd, scratch.data(), length, off_t(at));
            else progress.skipped += length / uint64_t(getpagesize());
        }
        ::close(fd);
    }).detach();
}

// For measuring a start that finds nothing in memory, without restarting the device: asks the system
// to let go of the pages it holds of the translated code and of every file under |roots|. What it
// does let go of shows in the pages the start then reads from storage. Returns the files asked about.
inline size_t dropForTest(const std::vector<std::string>& roots) {
    for (const auto& image : images()) msync(const_cast<uint8_t*>(image.text), image.size, MS_INVALIDATE);
    size_t files = 0;
    std::vector<std::string> pending = roots;
    while (!pending.empty()) {
        const std::string directory = std::move(pending.back());
        pending.pop_back();
        DIR* listing = opendir(directory.c_str());
        if (!listing) continue;
        while (const dirent* entry = readdir(listing)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string path = directory + "/" + name;
            struct stat status{};
            if (lstat(path.c_str(), &status) != 0) continue;
            if (S_ISDIR(status.st_mode)) { pending.push_back(path); continue; }
            if (!S_ISREG(status.st_mode) || status.st_size <= 0) continue;
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            void* mapped = mmap(nullptr, size_t(status.st_size), PROT_READ, MAP_SHARED, fd, 0);
            if (mapped != MAP_FAILED) {
                msync(mapped, size_t(status.st_size), MS_INVALIDATE);
                munmap(mapped, size_t(status.st_size));
                ++files;
            }
            ::close(fd);
        }
        closedir(listing);
    }
    return files;
}

} // namespace nestvane::prefetch
