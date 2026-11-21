// G0 feasibility probes for the near-native route in REVIEW.md section 5.3.
// Each probe runs in its own launch (argument e1, e2, e4 or e5), prints to
// stdout and mirrors the report to Documents/<probe>.txt.

#import <UIKit/UIKit.h>

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint64_t KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;

FILE* report = nullptr;

void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    printf("%s\n", line);
    fflush(stdout);
    if (report) {
        fprintf(report, "%s\n", line);
        fflush(report);
        fsync(fileno(report));
    }
}

uint64_t nowNs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }

task_vm_info_data_t vmInfo() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return info;
}
double footprintMiB() { return static_cast<double>(vmInfo().phys_footprint) / MiB; }
double internalMiB() { return static_cast<double>(vmInfo().internal) / MiB; }

// Free and reclaimable memory of the whole device, to tell real copies from per-mapping accounting.
double hostFreeMiB() {
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) != KERN_SUCCESS) return -1;
    return static_cast<double>(statistics.free_count + statistics.inactive_count + statistics.purgeable_count) * vm_kernel_page_size / MiB;
}

bool traced() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info{};
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

void header(const char* probe) {
    struct utsname name{};
    uname(&name);
    uint64_t memory = 0;
    size_t size = sizeof memory;
    sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
    const auto info = vmInfo();
    say("probe=%s bundle=%s", probe, NSBundle.mainBundle.bundleIdentifier.UTF8String);
    say("machine=%s os=%s build_config=%s", name.machine,
        NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String,
#ifdef __OPTIMIZE__
        "optimized"
#else
        "unoptimized"
#endif
    );
    say("traced=%d host_page=%lu kernel_page=%lu physical_memory_mib=%.0f available_mib=%.0f footprint_mib=%.1f",
        traced(), static_cast<unsigned long>(vm_page_size), static_cast<unsigned long>(vm_kernel_page_size),
        static_cast<double>(memory) / MiB, static_cast<double>(os_proc_available_memory()) / MiB, footprintMiB());
    say("vm_map min=0x%llx max=0x%llx span_gib=%.2f virtual_size_gib=%.2f",
        static_cast<unsigned long long>(info.min_address), static_cast<unsigned long long>(info.max_address),
        static_cast<double>(info.max_address - info.min_address) / GiB, static_cast<double>(info.virtual_size) / GiB);
}

// ---------------------------------------------------------------- E1

void reserveUntilFailure(const char* label, int protection, int flags) {
    constexpr size_t limit = 300000;
    static void* addresses[limit];
    static uint64_t sizes[limit];
    size_t count = 0;
    uint64_t total = 0, lowest = UINT64_MAX, highest = 0;
    int lastError = 0;
    const uint64_t begin = nowNs();
    for (uint64_t chunk = GiB; chunk >= 16 * MiB && count < limit; ) {
        void* address = mmap(nullptr, chunk, protection, flags, -1, 0);
        if (address == MAP_FAILED) {
            lastError = errno;
            chunk /= 2;
            continue;
        }
        addresses[count] = address;
        sizes[count++] = chunk;
        total += chunk;
        const auto value = reinterpret_cast<uint64_t>(address);
        if (value < lowest) lowest = value;
        if (value + chunk > highest) highest = value + chunk;
    }
    const uint64_t elapsed = nowNs() - begin;
    say("e1 cumulative %s total_gib=%.3f chunks=%zu lowest=0x%llx highest=0x%llx last_errno=%d reserve_ms=%.1f virtual_size_gib=%.2f footprint_mib=%.1f",
        label, static_cast<double>(total) / GiB, count, static_cast<unsigned long long>(lowest),
        static_cast<unsigned long long>(highest), lastError, ms(elapsed),
        static_cast<double>(vmInfo().virtual_size) / GiB, footprintMiB());
    for (size_t index = 0; index < count; ++index) munmap(addresses[index], sizes[index]);
}

void e1() {
    for (int shift = 46; shift >= 30; --shift) {
        const uint64_t size = 1ull << shift;
        void* address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (address == MAP_FAILED) {
            say("e1 single size_gib=%.0f result=fail errno=%d", static_cast<double>(size) / GiB, errno);
            continue;
        }
        say("e1 single size_gib=%.0f result=ok address=%p", static_cast<double>(size) / GiB, address);
        munmap(address, size);
    }
    reserveUntilFailure("prot_none", PROT_NONE, MAP_PRIVATE | MAP_ANON);
    reserveUntilFailure("read_write_untouched", PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);
    reserveUntilFailure("prot_none_repeat", PROT_NONE, MAP_PRIVATE | MAP_ANON);
}

// ---------------------------------------------------------------- E2

uint64_t pageValue(uint64_t generation, uint64_t offset) { return (generation << 48) ^ offset ^ 0x9e3779b97f4a7c15ull; }

kern_return_t remapCopy(void* source, size_t size, void** target, int flags, vm_prot_t* current = nullptr) {
    vm_address_t address = reinterpret_cast<vm_address_t>(*target);
    vm_prot_t cur = 0, max = 0;
    const kern_return_t result = vm_remap(mach_task_self(), &address, size, 0, flags, mach_task_self(),
                                          reinterpret_cast<vm_address_t>(source), TRUE, &cur, &max, VM_INHERIT_COPY);
    *target = reinterpret_cast<void*>(address);
    if (current) *current = cur;
    return result;
}

void e2Sparse(uint64_t size, uint64_t residentBytes) {
    const uint64_t page = vm_page_size;
    const uint64_t pages = residentBytes / page;
    const uint64_t stride = size / pages / page * page;
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (parent == MAP_FAILED) { say("e2 sparse size_gib=%.1f mmap errno=%d", static_cast<double>(size) / GiB, errno); return; }
    const double before = footprintMiB();
    uint64_t begin = nowNs();
    for (uint64_t index = 0; index < pages; ++index) *reinterpret_cast<uint64_t*>(parent + index * stride) = pageValue(1, index * stride);
    const uint64_t touchNs = nowNs() - begin;
    const double touched = footprintMiB();

    void* target = nullptr;
    vm_prot_t protection = 0;
    begin = nowNs();
    const kern_return_t result = remapCopy(parent, size, &target, VM_FLAGS_ANYWHERE, &protection);
    const uint64_t remapNs = nowNs() - begin;
    if (result != KERN_SUCCESS) {
        say("e2 sparse size_gib=%.1f resident_mib=%.0f remap kr=%d", static_cast<double>(size) / GiB, static_cast<double>(residentBytes) / MiB, result);
        munmap(parent, size);
        return;
    }
    auto* child = static_cast<uint8_t*>(target);
    const double remapped = footprintMiB();
    const double hostBeforeRead = hostFreeMiB(), internalBeforeRead = internalMiB();

    uint64_t mismatches = 0;
    begin = nowNs();
    for (uint64_t index = 0; index < pages; ++index)
        if (*reinterpret_cast<uint64_t*>(child + index * stride) != pageValue(1, index * stride)) ++mismatches;
    const uint64_t readNs = nowNs() - begin;
    const double afterRead = footprintMiB();
    const double hostAfterRead = hostFreeMiB(), internalAfterRead = internalMiB();

    begin = nowNs();
    for (uint64_t index = 0; index < pages; ++index) *reinterpret_cast<uint64_t*>(child + index * stride) = pageValue(2, index * stride);
    const uint64_t childWriteNs = nowNs() - begin;
    const double afterChildWrite = footprintMiB();
    const double hostAfterWrite = hostFreeMiB();
    for (uint64_t index = 0; index < pages; ++index)
        if (*reinterpret_cast<uint64_t*>(parent + index * stride) != pageValue(1, index * stride)) ++mismatches;

    begin = nowNs();
    for (uint64_t index = 0; index < pages; ++index) *reinterpret_cast<uint64_t*>(parent + index * stride) = pageValue(3, index * stride);
    const uint64_t parentWriteNs = nowNs() - begin;
    for (uint64_t index = 0; index < pages; ++index)
        if (*reinterpret_cast<uint64_t*>(child + index * stride) != pageValue(2, index * stride)) ++mismatches;
    // An untouched page written only in the child must stay zero in the parent.
    child[page * 3] = 7;
    if (stride > page * 4 && parent[page * 3] != 0) ++mismatches;

    say("e2 sparse size_gib=%.1f resident_mib=%.0f pages=%llu first_touch_us_per_page=%.2f remap_ms=%.3f cur_prot=%d "
        "child_read_us_per_page=%.2f child_cow_write_us_per_page=%.2f parent_write_after_us_per_page=%.2f mismatches=%llu "
        "footprint_mib before=%.1f touched=%.1f remapped=%.1f child_read=%.1f child_written=%.1f end=%.1f "
        "internal_mib before_read=%.1f after_read=%.1f host_free_mib before_read=%.0f after_read=%.0f after_child_write=%.0f",
        static_cast<double>(size) / GiB, static_cast<double>(residentBytes) / MiB, static_cast<unsigned long long>(pages),
        static_cast<double>(touchNs) / 1e3 / pages, ms(remapNs), protection, static_cast<double>(readNs) / 1e3 / pages,
        static_cast<double>(childWriteNs) / 1e3 / pages, static_cast<double>(parentWriteNs) / 1e3 / pages,
        static_cast<unsigned long long>(mismatches), before, touched, remapped, afterRead, afterChildWrite, footprintMiB(),
        internalBeforeRead, internalAfterRead, hostBeforeRead, hostAfterRead, hostAfterWrite);
    munmap(child, size);
    munmap(parent, size);
}

// Reads the same anonymous pages through a second mapping to see how the footprint ledger counts them.
void e2ReadAccounting(bool copy, bool fileBacked) {
    const uint64_t size = 256 * MiB, page = vm_page_size;
    int descriptor = -1;
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"g0-backing.bin"];
    if (fileBacked) {
        descriptor = open(path.fileSystemRepresentation, O_RDWR | O_CREAT | O_TRUNC, 0600);
        unlink(path.fileSystemRepresentation);
        ftruncate(descriptor, size);
    }
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, fileBacked ? MAP_SHARED : MAP_PRIVATE | MAP_ANON, descriptor, 0));
    if (parent == MAP_FAILED) { say("e2 read_accounting mmap errno=%d", errno); return; }
    const double before = footprintMiB(), hostBefore = hostFreeMiB();
    for (uint64_t offset = 0; offset < size; offset += page) parent[offset] = 1;
    const double touched = footprintMiB(), hostTouched = hostFreeMiB();
    vm_address_t address = 0;
    vm_prot_t cur = 0, max = 0;
    const kern_return_t result = vm_remap(mach_task_self(), &address, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                          reinterpret_cast<vm_address_t>(parent), copy, &cur, &max, copy ? VM_INHERIT_COPY : VM_INHERIT_SHARE);
    if (result != KERN_SUCCESS) { say("e2 read_accounting remap kr=%d", result); munmap(parent, size); return; }
    auto* child = reinterpret_cast<uint8_t*>(address);
    uint64_t sum = 0;
    const uint64_t begin = nowNs();
    for (uint64_t offset = 0; offset < size; offset += page) sum += child[offset];
    const uint64_t readNs = nowNs() - begin;
    const double afterRead = footprintMiB(), hostRead = hostFreeMiB();
    uint64_t writeNs = 0;
    if (copy) {
        const uint64_t start = nowNs();
        for (uint64_t offset = 0; offset < size; offset += page) child[offset] = 2;
        writeNs = nowNs() - start;
    }
    say("e2 read_accounting backing=%s second_mapping=%s resident_mib=%.0f read_us_per_page=%.2f write_us_per_page=%.2f footprint_mib before=%.1f touched=%.1f "
        "second_mapping_read=%.1f second_mapping_written=%.1f host_free_mib before=%.0f touched=%.0f second_mapping_read=%.0f second_mapping_written=%.0f sum=%llu",
        fileBacked ? "unlinked_file" : "anonymous", copy ? "copy_on_write" : "shared", static_cast<double>(size) / MiB,
        static_cast<double>(readNs) / 1e3 / (size / page), static_cast<double>(writeNs) / 1e3 / (size / page), before, touched, afterRead,
        footprintMiB(), hostBefore, hostTouched, hostRead, hostFreeMiB(), static_cast<unsigned long long>(sum));
    munmap(child, size);
    munmap(parent, size);
    if (descriptor >= 0) close(descriptor);
}

void e2Fixed() {
    const uint64_t size = GiB, page = vm_page_size;
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* reserved = static_cast<uint8_t*>(mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (parent == MAP_FAILED || reserved == MAP_FAILED) { say("e2 fixed mmap errno=%d", errno); return; }
    for (uint64_t offset = 0; offset < size; offset += 64 * page) parent[offset] = 1;
    void* target = reserved;
    const uint64_t begin = nowNs();
    const kern_return_t result = remapCopy(parent, size, &target, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    const uint64_t elapsed = nowNs() - begin;
    bool ok = result == KERN_SUCCESS && target == reserved;
    if (ok) {
        reserved[0] = 2;
        ok = parent[0] == 1 && reserved[64 * page] == 1;
    }
    say("e2 fixed_overwrite_into_reserved kr=%d same_address=%d cow_ok=%d remap_ms=%.3f", result, target == reserved, ok, ms(elapsed));
    munmap(parent, size);
    munmap(reserved, size);
}

void e2Fragmented(uint64_t entries) {
    const uint64_t page = vm_page_size, size = entries * page;
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (parent == MAP_FAILED) { say("e2 fragmented mmap errno=%d", errno); return; }
    for (uint64_t index = 0; index < entries; index += 8) parent[index * page] = 1;
    uint64_t begin = nowNs();
    for (uint64_t index = 0; index < entries; index += 2) mprotect(parent + index * page, page, PROT_READ);
    const uint64_t protectNs = nowNs() - begin;
    void* target = nullptr;
    begin = nowNs();
    const kern_return_t result = remapCopy(parent, size, &target, VM_FLAGS_ANYWHERE);
    const uint64_t remapNs = nowNs() - begin;
    bool protectionKept = false;
    if (result == KERN_SUCCESS) {
        // Protection is checked by asking the kernel, so no fault is needed.
        vm_address_t address = reinterpret_cast<vm_address_t>(target);
        vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &address, &regionSize, VM_REGION_BASIC_INFO_64,
                         reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS)
            protectionKept = info.protection == VM_PROT_READ && regionSize == page;
        munmap(target, size);
    }
    say("e2 fragmented entries=%llu size_mib=%.0f mprotect_ms=%.2f remap_kr=%d remap_ms=%.3f remap_us_per_entry=%.3f first_entry_protection_kept=%d",
        static_cast<unsigned long long>(entries), static_cast<double>(size) / MiB, ms(protectNs), result, ms(remapNs),
        static_cast<double>(remapNs) / 1e3 / entries, protectionKept);
    munmap(parent, size);
}

void e2Granularity() {
    const uint64_t page = vm_page_size;
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    memset(parent, 0x5a, 4 * page);
    void* target = nullptr;
    const kern_return_t result = remapCopy(parent + 4096, 4096, &target, VM_FLAGS_ANYWHERE);
    say("e2 granularity request_offset=4096 request_size=4096 kr=%d target=%p target_low_bits=0x%llx", result, target,
        static_cast<unsigned long long>(reinterpret_cast<uint64_t>(target) & (page - 1)));
    if (result == KERN_SUCCESS) {
        auto* child = reinterpret_cast<uint8_t*>(reinterpret_cast<uint64_t>(target) & ~(page - 1));
        child[0] = 1;
        say("e2 granularity whole_host_page_copied=%d parent_unchanged=%d", child[page - 1] == 0x5a, parent[0] == 0x5a);
        munmap(child, page);
    }
    // A 4 KiB protection change inside a 16 KiB host page.
    const int protect = mprotect(parent + 4096, 4096, PROT_READ);
    say("e2 granularity mprotect_4k_inside_page result=%d errno=%d", protect, protect ? errno : 0);
    munmap(parent, 4 * page);
}

void e2SharedFile() {
    const uint64_t page = vm_page_size, size = 16 * page;
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"g0-shared.bin"];
    const int descriptor = open(path.fileSystemRepresentation, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ftruncate(descriptor, size);
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, 3 * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    mmap(parent + size, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, descriptor, 0);
    parent[size] = 1;
    void* target = nullptr;
    const kern_return_t copyResult = remapCopy(parent, 3 * size, &target, VM_FLAGS_ANYWHERE);
    auto* child = static_cast<uint8_t*>(target);
    parent[size] = 2;
    const int seenByCopy = copyResult == KERN_SUCCESS ? child[size] : -1;
    // Linux fork keeps MAP_SHARED mappings shared. Re-share that range over the copy.
    vm_address_t address = reinterpret_cast<vm_address_t>(child + size);
    vm_prot_t cur = 0, max = 0;
    const kern_return_t shareResult = copyResult != KERN_SUCCESS ? copyResult : vm_remap(
        mach_task_self(), &address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
        reinterpret_cast<vm_address_t>(parent + size), FALSE, &cur, &max, VM_INHERIT_SHARE);
    parent[size] = 3;
    const int seenAfterShare = shareResult == KERN_SUCCESS ? child[size] : -1;
    if (shareResult == KERN_SUCCESS) child[size + 1] = 9;
    char fileByte = 0;
    msync(parent + size, size, MS_SYNC);
    pread(descriptor, &fileByte, 1, 1);
    say("e2 shared_file copy_kr=%d child_sees_parent_write_after_copy=%d share_kr=%d child_sees_parent_write_after_share=%d "
        "parent_sees_child_write=%d file_sees_child_write=%d",
        copyResult, seenByCopy == 2, shareResult, seenAfterShare == 3, parent[size + 1] == 9, fileByte == 9);
    if (copyResult == KERN_SUCCESS) munmap(child, 3 * size);
    munmap(parent, 3 * size);
    close(descriptor);
    unlink(path.fileSystemRepresentation);
}

void e2Generations() {
    const uint64_t size = 256 * MiB, page = vm_page_size, stride = 16 * page;
    constexpr int generations = 12;
    uint8_t* maps[generations + 1];
    maps[0] = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    for (uint64_t offset = 0; offset < size; offset += stride) maps[0][offset] = 1;
    uint64_t worst = 0, total = 0;
    int made = 0;
    for (int index = 1; index <= generations; ++index) {
        void* target = nullptr;
        const uint64_t begin = nowNs();
        const kern_return_t result = remapCopy(maps[index - 1], size, &target, VM_FLAGS_ANYWHERE);
        const uint64_t elapsed = nowNs() - begin;
        if (result != KERN_SUCCESS) { say("e2 generations depth=%d kr=%d", index, result); break; }
        maps[index] = static_cast<uint8_t*>(target);
        // Each generation dirties a different sixteenth of the resident pages.
        for (uint64_t offset = static_cast<uint64_t>(index) * stride; offset < size; offset += 16 * stride) maps[index][offset] = static_cast<uint8_t>(index + 1);
        total += elapsed;
        if (elapsed > worst) worst = elapsed;
        made = index;
    }
    uint64_t wrong = 0;
    for (int index = 1; index <= made; ++index)
        for (uint64_t offset = 0; offset < size; offset += stride) {
            uint8_t expected = 1;
            for (int writer = 1; writer <= index; ++writer)
                if (offset >= static_cast<uint64_t>(writer) * stride && (offset - writer * stride) % (16 * stride) == 0) expected = static_cast<uint8_t>(writer + 1);
            if (maps[index][offset] != expected) ++wrong;
        }
    const uint64_t begin = nowNs();
    uint64_t sum = 0;
    for (uint64_t offset = 0; offset < size; offset += stride) sum += maps[made][offset];
    const uint64_t readNs = nowNs() - begin;
    say("e2 generations depth=%d resident_mib_each=%.0f remap_mean_ms=%.3f remap_worst_ms=%.3f wrong_values=%llu deepest_read_us_per_page=%.2f footprint_mib=%.1f checksum=%llu",
        made, static_cast<double>(size / stride * page) / MiB, ms(total) / (made ? made : 1), ms(worst),
        static_cast<unsigned long long>(wrong), static_cast<double>(readNs) / 1e3 / (size / stride), footprintMiB(),
        static_cast<unsigned long long>(sum));
    for (int index = 0; index <= made; ++index) munmap(maps[index], size);
}

extern "C" int proc_pid_rusage(int pid, int flavor, rusage_info_t* buffer);

uint64_t diskBytesWritten() {
    rusage_info_v4 usage{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) != 0) return 0;
    return usage.ri_diskio_byteswritten;
}

// A zygote-like parent with many copy-on-write children that each read part of the inherited pages and
// write a smaller part. Compares anonymous backing with an unlinked file mapped MAP_SHARED in the parent.
void e2Zygote(bool fileBacked, int children, uint64_t parentBytes, int readPercent, int writePercent) {
    const uint64_t page = vm_page_size, pages = parentBytes / page;
    int descriptor = -1;
    if (fileBacked) {
        NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"g0-zygote.bin"];
        descriptor = open(path.fileSystemRepresentation, O_RDWR | O_CREAT | O_TRUNC, 0600);
        unlink(path.fileSystemRepresentation);
        ftruncate(descriptor, parentBytes);
    }
    const double footprintBefore = footprintMiB(), hostBefore = hostFreeMiB();
    const uint64_t diskBefore = diskBytesWritten();
    auto* parent = static_cast<uint8_t*>(mmap(nullptr, parentBytes, PROT_READ | PROT_WRITE, fileBacked ? MAP_SHARED : MAP_PRIVATE | MAP_ANON, descriptor, 0));
    if (parent == MAP_FAILED) { say("e2 zygote mmap errno=%d", errno); return; }
    uint64_t begin = nowNs();
    for (uint64_t index = 0; index < pages; ++index) memset(parent + index * page, static_cast<int>(index | 1), page);
    const uint64_t fillNs = nowNs() - begin;
    const double footprintParent = footprintMiB();

    static uint8_t* maps[256];
    uint64_t remapNs = 0, readNs = 0, writeNs = 0, wrong = 0, readPages = 0, writtenPages = 0;
    int made = 0;
    for (int child = 0; child < children && child < 256; ++child) {
        void* target = nullptr;
        begin = nowNs();
        if (remapCopy(parent, parentBytes, &target, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) break;
        remapNs += nowNs() - begin;
        maps[made++] = static_cast<uint8_t*>(target);
        auto* memory = static_cast<uint8_t*>(target);
        // Each child reads and writes a different window so the union covers the whole parent.
        const uint64_t first = static_cast<uint64_t>(child) * 7919 % pages;
        begin = nowNs();
        for (uint64_t count = 0; count < pages * readPercent / 100; ++count) {
            const uint64_t index = (first + count) % pages;
            if (memory[index * page + 5] != static_cast<uint8_t>(index | 1)) ++wrong;
            ++readPages;
        }
        readNs += nowNs() - begin;
        begin = nowNs();
        for (uint64_t count = 0; count < pages * writePercent / 100; ++count) {
            memory[(first + count) % pages * page + 9] = 0;
            ++writtenPages;
        }
        writeNs += nowNs() - begin;
    }
    for (uint64_t index = 0; index < pages; ++index)
        if (parent[index * page + 9] != static_cast<uint8_t>(index | 1)) ++wrong;
    const double footprintChildren = footprintMiB(), hostChildren = hostFreeMiB();
    const uint64_t diskAfter = diskBytesWritten();
    sleep(3);
    const uint64_t diskLater = diskBytesWritten();
    say("e2 zygote backing=%s children=%d parent_mib=%.0f read_percent=%d write_percent=%d parent_fill_us_per_page=%.2f remap_mean_ms=%.3f "
        "child_read_us_per_page=%.2f child_write_us_per_page=%.2f wrong=%llu footprint_mib start=%.1f parent_filled=%.1f with_children=%.1f "
        "host_free_mib start=%.0f with_children=%.0f disk_written_mib during=%.1f after_3s=%.1f",
        fileBacked ? "unlinked_file" : "anonymous", made, static_cast<double>(parentBytes) / MiB, readPercent, writePercent,
        static_cast<double>(fillNs) / 1e3 / pages, made ? ms(remapNs) / made : 0, readPages ? static_cast<double>(readNs) / 1e3 / readPages : 0,
        writtenPages ? static_cast<double>(writeNs) / 1e3 / writtenPages : 0, static_cast<unsigned long long>(wrong), footprintBefore,
        footprintParent, footprintChildren, hostBefore, hostChildren, static_cast<double>(diskAfter - diskBefore) / MiB,
        static_cast<double>(diskLater - diskBefore) / MiB);
    for (int child = 0; child < made; ++child) munmap(maps[child], parentBytes);
    munmap(parent, parentBytes);
    if (descriptor >= 0) close(descriptor);
}

void e2Zygotes() {
    // Shaped after the 16 KiB reference: about 44 MiB inherited per child, a quarter read, a tenth written.
    e2Zygote(false, 32, 48 * MiB, 25, 10);
    e2Zygote(true, 32, 48 * MiB, 25, 10);
    e2Zygote(false, 64, 48 * MiB, 25, 10);
    e2Zygote(true, 64, 48 * MiB, 25, 10);
    e2Zygote(true, 64, 48 * MiB, 100, 0);
}

void e2() {
    e2Sparse(GiB, 64 * MiB);
    e2Sparse(GiB, 256 * MiB);
    e2Sparse(2 * GiB, 256 * MiB);
    e2Sparse(2 * GiB, 512 * MiB);
    e2ReadAccounting(true, false);
    e2ReadAccounting(false, false);
    e2ReadAccounting(true, true);
    e2ReadAccounting(false, true);
    e2Fixed();
    e2Fragmented(1024);
    e2Fragmented(16384);
    e2Fragmented(65536);
    e2Granularity();
    e2SharedFile();
    e2Generations();
}

// ---------------------------------------------------------------- E4

volatile sig_atomic_t signalCount = 0;
volatile int lastSignal = 0;
volatile uintptr_t lastAddress = 0;
volatile bool redirect = false;
volatile sig_atomic_t landingCount = 0;

__attribute__((noinline)) void landing() { landingCount = landingCount + 1; }

void onFault(int number, siginfo_t* info, void* raw) {
    auto* context = static_cast<ucontext_t*>(raw);
    signalCount = signalCount + 1;
    lastSignal = number;
    lastAddress = reinterpret_cast<uintptr_t>(info->si_addr);
    if (redirect) {
        // The pattern ART uses: resume in a different function with a return address after the fault.
        context->uc_mcontext->__ss.__lr = context->uc_mcontext->__ss.__pc + 4;
        context->uc_mcontext->__ss.__pc = reinterpret_cast<uint64_t>(&landing);
    } else {
        context->uc_mcontext->__ss.__pc += 4;
    }
}

__attribute__((noinline)) uint64_t faultingLoad(volatile uint64_t* pointer) {
    uint64_t value = 0xdead;
    __asm__ volatile("ldr %0, [%1]" : "+r"(value) : "r"(pointer) : "x30", "memory");
    return value;
}

__attribute__((noinline)) void faultingStore(volatile uint64_t* pointer) {
    __asm__ volatile("str xzr, [%0]" : : "r"(pointer) : "x30", "memory");
}

void installSignals() {
    stack_t stack{};
    stack.ss_size = 256 * KiB;
    stack.ss_sp = malloc(stack.ss_size);
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = onFault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}

void signalRound(const char* label, volatile uint64_t* pointer, bool store, bool viaLanding, int iterations) {
    signalCount = 0;
    landingCount = 0;
    redirect = viaLanding;
    uint64_t worst = 0, kept = 0;
    const uint64_t begin = nowNs();
    for (int index = 0; index < iterations; ++index) {
        const uint64_t start = nowNs();
        if (store) faultingStore(pointer);
        else if (faultingLoad(pointer) == 0xdead) ++kept;
        const uint64_t elapsed = nowNs() - start;
        if (elapsed > worst) worst = elapsed;
    }
    const uint64_t total = nowNs() - begin;
    redirect = false;
    say("e4 %s iterations=%d delivered=%d landing=%d value_preserved=%llu signal=%d si_addr=0x%lx mean_us=%.2f worst_us=%.1f",
        label, iterations, static_cast<int>(signalCount), static_cast<int>(landingCount),
        static_cast<unsigned long long>(kept), lastSignal, static_cast<unsigned long>(lastAddress),
        static_cast<double>(total) / 1e3 / iterations, static_cast<double>(worst) / 1e3);
}

void* signalThread(void* raw) {
    stack_t stack{};
    stack.ss_size = 256 * KiB;
    stack.ss_sp = malloc(stack.ss_size);
    sigaltstack(&stack, nullptr);
    signalRound("signal second_thread null_load", nullptr, false, false, 20000);
    signalRound("signal second_thread protected_store", static_cast<volatile uint64_t*>(raw), true, true, 20000);
    return nullptr;
}

#pragma pack(push, 4)
struct ExceptionRequest {
    mach_msg_header_t head;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t codeCount;
    int64_t code[2];
    int flavor;
    mach_msg_type_number_t stateCount;
    natural_t state[1296];
};
struct ExceptionReply {
    mach_msg_header_t head;
    NDR_record_t ndr;
    kern_return_t result;
    int flavor;
    mach_msg_type_number_t stateCount;
    natural_t state[1296];
};
#pragma pack(pop)

mach_port_t exceptionPort = MACH_PORT_NULL;
volatile int machMode = 0;  // 0 forwards to the BSD signal, 1 repairs the thread state.
volatile int machReceived = 0;
volatile int machBadMessages = 0;
volatile int machSendErrors = 0;

void* exceptionServer(void*) {
    static union { ExceptionRequest request; char bytes[sizeof(ExceptionRequest) + 1024]; } incoming;
    static ExceptionReply reply;
    for (;;) {
        memset(&incoming.request.head, 0, sizeof incoming.request.head);
        if (mach_msg(&incoming.request.head, MACH_RCV_MSG, 0, sizeof incoming, exceptionPort, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) continue;
        const ExceptionRequest& request = incoming.request;
        machReceived = machReceived + 1;
        memset(&reply, 0, offsetof(ExceptionReply, state));
        reply.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.head.msgh_bits), 0);
        reply.head.msgh_remote_port = request.head.msgh_remote_port;
        reply.head.msgh_id = request.head.msgh_id + 100;
        reply.ndr = request.ndr;
        const bool usable = request.head.msgh_id == 2406 && request.flavor == ARM_THREAD_STATE64 &&
                            request.stateCount == ARM_THREAD_STATE64_COUNT;
        if (!usable) machBadMessages = machBadMessages + 1;
        if (machMode == 1 && usable) {
            arm_thread_state64_t state;
            memcpy(&state, request.state, sizeof state);
            state.__pc += 4;
            memcpy(reply.state, &state, sizeof state);
            reply.result = KERN_SUCCESS;
            reply.flavor = ARM_THREAD_STATE64;
            reply.stateCount = ARM_THREAD_STATE64_COUNT;
            reply.head.msgh_size = static_cast<mach_msg_size_t>(offsetof(ExceptionReply, state) + sizeof state);
        } else {
            reply.result = KERN_FAILURE;
            reply.head.msgh_size = static_cast<mach_msg_size_t>(offsetof(ExceptionReply, flavor));
        }
        if (mach_msg(&reply.head, MACH_SEND_MSG, reply.head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS)
            machSendErrors = machSendErrors + 1;
    }
}

void* watchdog(void*) {
    sleep(60);
    say("e4 watchdog fired after 60 s: a fault never resumed. mach_received=%d bad_messages=%d send_errors=%d",
        machReceived, machBadMessages, machSendErrors);
    _exit(3);
}

void e4() {
    const uint64_t page = vm_page_size;
    auto* none = static_cast<volatile uint64_t*>(mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* readOnly = static_cast<volatile uint64_t*>(mmap(nullptr, page, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0));
    installSignals();
    pthread_t dog;
    pthread_create(&dog, nullptr, watchdog, nullptr);

    signalRound("signal null_load", nullptr, false, false, 100000);
    signalRound("signal low_address_load", reinterpret_cast<volatile uint64_t*>(0x1238), false, false, 100000);
    signalRound("signal prot_none_load", none, false, false, 100000);
    signalRound("signal read_only_store", readOnly, true, false, 100000);
    signalRound("signal null_load_redirected", nullptr, false, true, 100000);
    pthread_t second;
    pthread_create(&second, nullptr, signalThread, const_cast<uint64_t*>(readOnly));
    pthread_join(second, nullptr);

    // Mixed with a task-level Mach exception port, the case reported as unreliable on iOS 18.3.1.
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t saved = EXC_TYPES_COUNT;
    kern_return_t result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exceptionPort);
    if (result == KERN_SUCCESS) result = mach_port_insert_right(mach_task_self(), exceptionPort, exceptionPort, MACH_MSG_TYPE_MAKE_SEND);
    say("e4 mach port_setup kr=%d", result);
    if (result == KERN_SUCCESS) {
        pthread_t server;
        pthread_create(&server, nullptr, exceptionServer, nullptr);
        result = task_swap_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, exceptionPort,
                                           static_cast<exception_behavior_t>(EXCEPTION_STATE | MACH_EXCEPTION_CODES),
                                           ARM_THREAD_STATE64, masks, &saved, ports, behaviors, flavors);
        say("e4 mach task_swap_exception_ports kr=%d previous_handlers=%u", result, saved);
    }
    if (result == KERN_SUCCESS) {
        machMode = 0;
        machReceived = 0;
        signalRound("mach_forwarded_to_signal null_load", nullptr, false, false, 20000);
        signalRound("mach_forwarded_to_signal read_only_store_redirected", readOnly, true, true, 20000);
        say("e4 mach forwarded received=%d bad_messages=%d send_errors=%d", machReceived, machBadMessages, machSendErrors);
        machMode = 1;
        machReceived = 0;
        signalRound("mach_repairs_state null_load", nullptr, false, false, 20000);
        signalRound("mach_repairs_state prot_none_load", none, false, false, 20000);
        say("e4 mach repaired received=%d bad_messages=%d send_errors=%d", machReceived, machBadMessages, machSendErrors);
        machMode = 0;
        signalRound("mach_forwarded_again null_load", nullptr, false, false, 20000);
        for (mach_msg_type_number_t index = 0; index < saved; ++index)
            task_set_exception_ports(mach_task_self(), masks[index], ports[index], behaviors[index], flavors[index]);
        if (saved == 0) task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
        signalRound("signal after_mach_port_removed null_load", nullptr, false, false, 100000);
    }
    say("e4 traced_at_end=%d", traced());
}

// ---------------------------------------------------------------- E5

void e5() {
    const uint64_t page = vm_page_size;
    NSString* frameworks = NSBundle.mainBundle.privateFrameworksPath;
    NSArray<NSString*>* names = [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:frameworks error:nil]
        filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'G0Code'"]] sortedArrayUsingSelector:@selector(compare:)];
    say("e5 frameworks=%lu", static_cast<unsigned long>(names.count));
    uint64_t totalBytes = 0, totalOpen = 0, totalCold = 0, totalWarm = 0;
    int index = 0;
    for (NSString* name in names) {
        NSString* stem = name.stringByDeletingPathExtension;
        NSString* path = [[frameworks stringByAppendingPathComponent:name] stringByAppendingPathComponent:stem];
        uint64_t begin = nowNs();
        void* handle = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        const uint64_t openNs = nowNs() - begin;
        if (!handle) { say("e5 %s dlopen failed: %s", stem.UTF8String, dlerror()); continue; }
        auto* start = static_cast<const uint8_t*>(dlsym(handle, "g0_code_begin"));
        auto* end = static_cast<const uint8_t*>(dlsym(handle, "g0_code_end"));
        if (!start || !end) { say("e5 %s symbols missing", stem.UTF8String); continue; }
        const uint64_t bytes = static_cast<uint64_t>(end - start), pages = bytes / page;
        // Even images are first touched by reading, odd ones by executing. Every 4 KiB block starts a
        // function that ends in ret, so any block start is a valid entry.
        const bool execute = index % 2 == 1;
        uint64_t sum = 0;
        begin = nowNs();
        for (uint64_t offset = 0; offset < bytes; offset += page) {
            if (execute) reinterpret_cast<void (*)()>(const_cast<uint8_t*>(start + offset))();
            else sum += *reinterpret_cast<const uint32_t*>(start + offset);
        }
        const uint64_t coldNs = nowNs() - begin;
        begin = nowNs();
        for (uint64_t offset = 0; offset < bytes; offset += page) {
            if (execute) reinterpret_cast<void (*)()>(const_cast<uint8_t*>(start + offset))();
            else sum += *reinterpret_cast<const uint32_t*>(start + offset);
        }
        const uint64_t warmNs = nowNs() - begin;
        // Run every block once to measure straight-line execution of signed pages.
        begin = nowNs();
        for (uint64_t offset = 0; offset < bytes; offset += 4096) reinterpret_cast<void (*)()>(const_cast<uint8_t*>(start + offset))();
        const uint64_t runNs = nowNs() - begin;
        say("e5 %s text_mib=%.1f dlopen_ms=%.2f first_touch=%s first_touch_ms=%.1f first_touch_us_per_page=%.2f "
            "second_touch_ms=%.2f run_all_ms=%.1f run_minst_per_s=%.0f footprint_mib=%.1f checksum=%llu",
            stem.UTF8String, static_cast<double>(bytes) / MiB, ms(openNs), execute ? "execute" : "read", ms(coldNs),
            static_cast<double>(coldNs) / 1e3 / pages, ms(warmNs), ms(runNs),
            static_cast<double>(bytes / 4) / (static_cast<double>(runNs) / 1e9) / 1e6, footprintMiB(),
            static_cast<unsigned long long>(sum));
        totalBytes += bytes;
        totalOpen += openNs;
        totalCold += coldNs;
        totalWarm += warmNs;
        ++index;
    }
    say("e5 total text_mib=%.1f dlopen_ms=%.1f first_touch_ms=%.1f second_touch_ms=%.1f first_touch_mib_per_s=%.0f footprint_mib=%.1f",
        static_cast<double>(totalBytes) / MiB, ms(totalOpen), ms(totalCold), ms(totalWarm),
        totalCold ? static_cast<double>(totalBytes) / MiB / (static_cast<double>(totalCold) / 1e9) : 0, footprintMiB());
}

#ifdef G0_GUEST
// ---------------------------------------------------------------- E3 on the device
// Runs the rewritten toybox (experiments/g0/e3) inside this app with the scripts of compare.py.

extern "C" int guest_main(int argc, char** argv, char** envp);

NSData* runGuest(NSString* script, double* milliseconds, int* status) {
    NSString* output = [NSTemporaryDirectory() stringByAppendingPathComponent:@"g0-guest-output"];
    fflush(stdout);
    const int saved = dup(1), file = open(output.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(file, 1);
    close(file);
    char* arguments[] = {(char*)"g0", (char*)"./toybox", (char*)"toybox", (char*)"sh", (char*)"-c", (char*)script.UTF8String, nullptr};
    char* environment[] = {nullptr};
    const uint64_t begin = nowNs();
    *status = guest_main(6, arguments, environment);
    *milliseconds = ms(nowNs() - begin);
    dup2(saved, 1);
    close(saved);
    return [NSData dataWithContentsOfFile:output];
}

NSString* digest(NSData* data) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), hash);
    NSMutableString* text = [NSMutableString string];
    for (int index = 0; index < 6; ++index) [text appendFormat:@"%02x", hash[index]];
    return text;
}

void e3() {
    NSString* work = [NSTemporaryDirectory() stringByAppendingPathComponent:@"g0-guest"];
    [NSFileManager.defaultManager removeItemAtPath:work error:nil];
    [NSFileManager.defaultManager createDirectoryAtPath:work withIntermediateDirectories:YES attributes:nil error:nil];
    [NSFileManager.defaultManager copyItemAtPath:[NSBundle.mainBundle pathForResource:@"toybox" ofType:@"elf"]
                                          toPath:[work stringByAppendingPathComponent:@"toybox"] error:nil];
    chmod([work stringByAppendingPathComponent:@"toybox"].fileSystemRepresentation, 0755);
    chdir(work.fileSystemRepresentation);
    NSData* scripts = [NSData dataWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"scripts" ofType:@"bin"]];
    NSArray<NSString*>* fields = [[[NSString alloc] initWithData:scripts encoding:NSUTF8StringEncoding] componentsSeparatedByString:@"\0"];
    for (NSUInteger index = 0; index + 1 < fields.count; index += 2) {
        NSString* name = fields[index];
        const bool benchmark = [name hasPrefix:@"b "];
        double best = 1e18, elapsed = 0;
        int status = 0;
        NSData* output = nil;
        for (int round = 0; round < (benchmark ? 3 : 1); ++round) {
            output = runGuest(fields[index + 1], &elapsed, &status);
            if (elapsed < best) best = elapsed;
        }
        say("e3 %s exit=%d bytes=%lu sha=%s ms=%.0f", [name stringByReplacingOccurrencesOfString:@" " withString:@"_"].UTF8String, status,
            static_cast<unsigned long>(output.length), digest(output).UTF8String, best);
        if (getenv("G0_SHOW_OUTPUT") && !benchmark && output.length < 600)
            say("e3 output %s", [[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding] stringByReplacingOccurrencesOfString:@"\n" withString:@"|"].UTF8String);
    }
    say("e3 footprint_mib=%.1f", footprintMiB());
}
#endif

void* run(void* raw) {
    NSString* probe = (__bridge_transfer NSString*)raw;
    NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    report = fopen([documents stringByAppendingPathComponent:[probe stringByAppendingString:@".txt"]].fileSystemRepresentation, "w");
    header(probe.UTF8String);
    const uint64_t begin = nowNs();
    if ([probe isEqualToString:@"e1"]) e1();
    else if ([probe isEqualToString:@"e2"]) e2();
    else if ([probe isEqualToString:@"e2zygote"]) e2Zygotes();
    else if ([probe isEqualToString:@"e4"]) e4();
    else if ([probe isEqualToString:@"e5"]) e5();
#ifdef G0_GUEST
    else if ([probe isEqualToString:@"e3"]) e3();
#endif
    else if ([probe isEqualToString:@"e5pressure"]) {
        // Dirty most of the app's memory budget first so the file cache holding the code is likely evicted.
        const uint64_t size = os_proc_available_memory() - 256 * MiB;
        auto* memory = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        const double hostBefore = hostFreeMiB();
        for (uint64_t offset = 0; offset < size; offset += vm_page_size) memory[offset] = 1;
        say("e5 pressure dirtied_mib=%.0f host_free_mib before=%.0f after=%.0f", static_cast<double>(size) / MiB, hostBefore, hostFreeMiB());
        munmap(memory, size);
        e5();
    }
    else say("unknown probe");
    say("done probe=%s elapsed_ms=%.0f", probe.UTF8String, ms(nowNs() - begin));
    fclose(report);
    exit(0);
}

}  // namespace

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController* controller = [UIViewController new];
    UILabel* label = [[UILabel alloc] initWithFrame:controller.view.bounds];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
    [controller.view addSubview:label];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];

    NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
    NSString* probe = arguments.count > 1 ? arguments[1] : nil;
    label.text = probe ? [@"G0 probe " stringByAppendingString:probe] : @"G0 probes\nLaunch with e1, e2, e4 or e5.";
    if (probe) {
        pthread_t thread;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 8 * MiB);
        pthread_create(&thread, &attributes, run, (__bridge_retained void*)probe);
    }
    return YES;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
