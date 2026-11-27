// A mapped file that is cut short from outside while mapped: touching what it no longer has must
// read zeros and go on, not end the process (runtime/NativeCode.hpp, repairFilePage).
#include "NativeCode.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

int main() {
    using namespace nestvane::native;
    const size_t page = vm_page_size, size = 8 * page;
    std::string path = "/tmp/nestvane-file-fault.XXXXXX";
    const int fd = mkstemp(path.data());
    if (fd < 0) return 2;
    unlink(path.c_str());
    const std::vector<uint8_t> content(size, 0x5a);
    if (write(fd, content.data(), size) != ssize_t(size)) return 2;
    auto* shared = static_cast<volatile uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    auto* readOnly = static_cast<volatile uint8_t*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (shared == MAP_FAILED || readOnly == MAP_FAILED) return 2;
    unsigned failures = 0;
    const auto expect = [&](bool condition, const char* what) {
        if (!condition) { std::printf("file fault: FAILED: %s\n", what); ++failures; }
    };
    expect(shared[page] == 0x5a && readOnly[3 * page] == 0x5a, "content before the file is cut");
    // Not a page of a file, and an access the mapping does not allow: neither is repaired.
    void* anonymous = mmap(nullptr, page, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    expect(!repairFilePage(reinterpret_cast<uint64_t>(anonymous), 1), "anonymous memory is left alone");
    expect(!repairFilePage(reinterpret_cast<uint64_t>(readOnly), 2), "a write to a read-only mapping is left alone");
    expect(!repairFilePage(reinterpret_cast<uint64_t>(shared), 0), "a fault of another kind is left alone");
    expect(repairedFilePages() == 0, "nothing repaired yet");

    catchFileFaults();
    if (ftruncate(fd, off_t(page))) return 2;
    expect(shared[0] == 0x5a, "the page the file still has");
    expect(shared[2 * page + 5] == 0, "a read past the new end reads zero");
    shared[4 * page] = 7;
    expect(shared[4 * page] == 7, "a write past the new end stays in memory");
    // A private mapping may still be given the page from the host's cache: whichever, the read returns.
    const uint8_t late = readOnly[5 * page];
    expect(late == 0 || late == 0x5a, "a read through the read-only mapping returns");
    expect(repairedFilePages() >= 2, "the pages touched were repaired");
    struct stat status{};
    expect(fstat(fd, &status) == 0 && size_t(status.st_size) == page, "the file itself is not written to");
    std::printf("file fault: %s (%llu pages repaired)\n", failures ? "FAILED" : "passed", (unsigned long long)repairedFilePages());
    return failures ? 1 : 0;
}
