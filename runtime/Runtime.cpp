#include "Runtime.hpp"
#include "NativeCode.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <utility>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace nestvane {

bool hasNativeSha256() {
#if defined(__APPLE__) && defined(__aarch64__)
    static const bool available = [] {
        int supported = 0;
        size_t size = sizeof(supported);
        return sysctlbyname("hw.optional.arm.FEAT_SHA256", &supported, &size, nullptr, 0) == 0 &&
               size == sizeof(supported) && supported == 1;
    }();
    return available;
#else
    return false;
#endif
}

namespace {

constexpr unsigned readable = 4;
constexpr unsigned writable = 2;
constexpr unsigned executable = 1;
constexpr uint64_t stackTop = 0x8000'0000;
constexpr size_t staticStackSize = 4 * pageSize;
constexpr size_t maxSegment = 8 * 1024 * 1024;
constexpr size_t maxBlockLength = 16;

template<class T>
T field(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw std::runtime_error("truncated ELF");
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

uint64_t signExtend(unsigned value, unsigned bits) {
    uint64_t sign = uint64_t{1} << (bits - 1);
    return (uint64_t{value} ^ sign) - sign;
}

uint32_t fetch(const uint8_t* bytes) {
    uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void systemCall(Machine& machine) {
    auto& cpu = machine.cpu;
    if (cpu.x[8] == 93 || cpu.x[8] == 94) {
        cpu.exitCode = static_cast<int>(cpu.x[0] & 255);
        cpu.stop = Stop::exit;
    } else if (cpu.x[8] == 64 && (cpu.x[0] == 1 || cpu.x[0] == 2) && cpu.x[2] <= 4096) {
        std::vector<uint8_t> scratch;
        auto data = machine.read(cpu.x[1], cpu.x[2], readable, scratch);
        if (!data) {
            cpu.x[0] = uint64_t(-14); // EFAULT
            return;
        }
        cpu.output.append(reinterpret_cast<const char*>(data), cpu.x[2]);
        cpu.x[0] = cpu.x[2];
    } else {
        cpu.x[0] = uint64_t(-38); // ENOSYS
    }
}

} // namespace

void executeDecoded(Machine& machine, const Instruction& ins) {
    auto& cpu = machine.cpu;
    auto& x = cpu.x;
    uint64_t next = cpu.pc + 4;
    switch (ins.op) {
        case Op::movz: x[ins.rd] = ins.immediate; break;
        case Op::addImm: x[ins.rd] = x[ins.rn] + ins.immediate; break;
        case Op::subImm: x[ins.rd] = x[ins.rn] - ins.immediate; break;
        case Op::addReg: x[ins.rd] = x[ins.rn] + x[ins.rm]; break;
        case Op::adr: x[ins.rd] = cpu.pc + ins.immediate; break;
        case Op::load:
            if (!machine.load64(x[ins.rn] + ins.immediate, x[ins.rd])) {
                machine.fault(cpu.pc, x[ins.rn] + ins.immediate);
                return;
            }
            break;
        case Op::store:
            if (!machine.store64(x[ins.rn] + ins.immediate, x[ins.rd])) {
                machine.fault(cpu.pc, x[ins.rn] + ins.immediate);
                return;
            }
            break;
        case Op::cbnz:
            if (x[ins.rd]) next = cpu.pc + ins.immediate;
            break;
        case Op::svc: systemCall(machine); break;
        case Op::unsupported: cpu.stop = Stop::unsupported; return;
    }
    cpu.pc = next;
    ++cpu.retired;
}

namespace {

const AOTBlock* checkedBlock(Machine& machine, const AOTBlock& block,
                             uint64_t entry, Counters& counters) {
    if (block.entryOffset > UINT64_MAX - entry) return nullptr;
    std::vector<uint8_t> scratch;
    auto bytes = machine.read(entry + block.entryOffset, block.words.size_bytes(), executable, scratch);
    if (bytes && std::memcmp(bytes, block.words.data(), block.words.size_bytes()) == 0) return &block;
    ++counters.rejectedMappings;
    return nullptr;
}

} // namespace

Instruction decode(uint32_t word) {
    unsigned rd = word & 31;
    unsigned rn = (word >> 5) & 31;
    unsigned rm = (word >> 16) & 31;
    if (word == 0xd4000001) return {Op::svc};
    if (rd == 31) return {}; // The fixture subset does not address XZR or SP.
    if ((word & 0xff800000) == 0xd2800000) {
        return {Op::movz, rd, 0, 0, uint64_t((word >> 5) & 0xffff) << (((word >> 21) & 3) * 16)};
    }
    if ((word & 0x9f000000) == 0x10000000) {
        unsigned immediate = (((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3);
        return {Op::adr, rd, 0, 0, signExtend(immediate, 21)};
    }
    if ((word & 0xff000000) == 0xb5000000) {
        return {Op::cbnz, rd, 0, 0, signExtend((word >> 5) & 0x7ffff, 19) * 4};
    }
    if (rn == 31) return {};
    if ((word & 0xffc00000) == 0x91000000 || (word & 0xffc00000) == 0xd1000000) {
        auto op = (word & 0x40000000) ? Op::subImm : Op::addImm;
        return {op, rd, rn, 0, (word >> 10) & 0xfff};
    }
    if ((word & 0xffe0fc00) == 0x8b000000 && rm != 31) {
        return {Op::addReg, rd, rn, rm};
    }
    if ((word & 0xffc00000) == 0xf9400000 || (word & 0xffc00000) == 0xf9000000) {
        auto op = (word & 0x00400000) ? Op::load : Op::store;
        return {op, rd, rn, 0, ((word >> 10) & 0xfff) * 8};
    }
    return {};
}

bool endsBlock(Op op) {
    return op == Op::cbnz || op == Op::svc || op == Op::unsupported;
}

namespace {

enum class ImageKind { staticExecutable, dynamicProgram, interpreter };

struct ELFImage {
    Image image;
    uint64_t programHeaders = 0;
    uint16_t programHeaderSize = 0;
    uint16_t programHeaderCount = 0;
    std::string interpreter;
};

ELFImage readELF(std::span<const uint8_t> elf, uint64_t bias, ImageKind kind) {
    if (elf.size() < 64 || field<uint32_t>(elf, 0) != 0x464c457f || elf[4] != 2 ||
        elf[5] != 1 || elf[6] != 1 ||
        field<uint16_t>(elf, 18) != 183 || field<uint32_t>(elf, 20) != 1) {
        throw std::runtime_error("expected a little-endian AArch64 ELF executable");
    }
    unsigned elfType = field<uint16_t>(elf, 16);
    if ((kind == ImageKind::staticExecutable && elfType != 2) ||
        (kind == ImageKind::dynamicProgram && elfType != 2 && elfType != 3) ||
        (kind == ImageKind::interpreter && elfType != 3)) {
        throw std::runtime_error("unexpected ELF type for requested launch");
    }
    uint64_t entry = field<uint64_t>(elf, 24);
    uint64_t phoff = field<uint64_t>(elf, 32);
    uint16_t phsize = field<uint16_t>(elf, 54);
    uint16_t phnum = field<uint16_t>(elf, 56);
    if (phsize != 56 || phnum > 32 || phoff > elf.size() ||
        phnum * size_t(phsize) > elf.size() - phoff || entry > UINT64_MAX - bias ||
        bias % pageSize) {
        throw std::runtime_error("invalid ELF layout");
    }
    ELFImage object{{entry + bias, {}, std::nullopt}, 0, phsize, phnum, {}};
    auto& image = object.image;
    for (unsigned i = 0; i < phnum; ++i) {
        size_t header = phoff + i * phsize;
        unsigned type = field<uint32_t>(elf, header);
        if (type == 2 && kind == ImageKind::staticExecutable) {
            throw std::runtime_error("ELF requires a dynamic loader");
        }
        if (type == 3) {
            uint64_t offset = field<uint64_t>(elf, header + 8);
            uint64_t size = field<uint64_t>(elf, header + 32);
            if (kind != ImageKind::dynamicProgram || !object.interpreter.empty() ||
                size < 2 || size > 256 || offset > elf.size() || size > elf.size() - offset) {
                throw std::runtime_error("invalid PT_INTERP");
            }
            auto path = reinterpret_cast<const char*>(elf.data() + offset);
            if (std::memchr(path, 0, size) != path + size - 1) {
                throw std::runtime_error("invalid interpreter path");
            }
            object.interpreter.assign(path, size - 1);
        }
        if (type != 1) continue;
        unsigned flags = field<uint32_t>(elf, header + 4);
        uint64_t offset = field<uint64_t>(elf, header + 8);
        uint64_t address = field<uint64_t>(elf, header + 16);
        uint64_t filesz = field<uint64_t>(elf, header + 32);
        uint64_t memsz = field<uint64_t>(elf, header + 40);
        if (filesz > memsz || memsz > maxSegment || offset > elf.size() ||
            filesz > elf.size() - offset || address > UINT64_MAX - bias ||
            memsz > UINT64_MAX - (address + bias) ||
            ((flags & (writable | executable)) == (writable | executable))) {
            throw std::runtime_error("invalid ELF load segment");
        }
        if (!memsz) continue;
        address += bias;
        const size_t phbytes = phnum * size_t(phsize);
        if ((flags & readable) && phoff >= offset && phoff - offset <= filesz &&
            phbytes <= filesz - (phoff - offset)) {
            object.programHeaders = address + (phoff - offset);
        }
        for (const auto& segment : image.segments) {
            if (address < segment.address + segment.bytes.size() && segment.address < address + memsz) {
                throw std::runtime_error("overlapping ELF segments");
            }
        }
        if (kind != ImageKind::staticExecutable) {
            const uint64_t prefix = address % pageSize;
            if (offset % pageSize != prefix || offset < prefix ||
                address + memsz > UINT64_MAX - (pageSize - 1)) {
                throw std::runtime_error("invalid dynamic ELF page alignment");
            }
            address -= prefix;
            offset -= prefix;
            filesz += prefix;
            memsz = (memsz + prefix + pageSize - 1) & ~(pageSize - 1);
            for (const auto& segment : image.segments) {
                if (address < segment.address + segment.bytes.size() && segment.address < address + memsz) {
                    throw std::runtime_error("overlapping ELF page mappings");
                }
            }
        }
        auto& segment = image.segments.emplace_back(Segment{address, std::vector<uint8_t>(memsz), flags});
        std::copy_n(elf.data() + offset, filesz, segment.bytes.mutableData());
    }
    if (image.entry % 4 || std::none_of(image.segments.begin(), image.segments.end(), [&](const Segment& segment) {
        return (segment.flags & executable) && segment.contains(image.entry, 4);
    })) throw std::runtime_error("ELF entry is not mapped for execution");
    return object;
}

uint64_t pushData(Machine& machine, uint64_t stack, std::span<const uint8_t> data) {
    if (stack < data.size() || !machine.write(stack - data.size(), data)) {
        throw std::runtime_error("initial stack overflow");
    }
    return stack - data.size();
}

void setupStack(Machine& machine, const Startup& startup) {
    auto bytes = [](const auto* value, size_t length) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value), length);
    };
    const uint64_t name = pushData(machine, stackTop, bytes(startup.filename.c_str(), startup.filename.size() + 1));
    uint64_t cursor = name;
    const auto argc = startup.arguments.size() + 1;
    const auto environmentIndex = argc + 2;
    std::vector<uint64_t> initial(environmentIndex + startup.environment.size() + 1);
    initial[0] = argc;
    const auto pushString = [&](const std::string& value) {
        cursor = pushData(machine, cursor, bytes(value.c_str(), value.size() + 1));
        return cursor;
    };
    // Linux stores argv and then envp strings consecutively in increasing address order.
    for (auto i = startup.environment.size(); i > 0; --i)
        initial[environmentIndex + i - 1] = pushString(startup.environment[i - 1]);
    for (auto i = startup.arguments.size(); i > 0; --i)
        initial[i + 1] = pushString(startup.arguments[i - 1]);
    initial[1] = pushString(startup.argv0 ? *startup.argv0 : startup.filename);
    constexpr char platform[] = "aarch64";
    const uint64_t platformPointer = pushData(machine, cursor, bytes(platform, sizeof(platform)));
    std::array<uint8_t, 16> random;
    std::random_device source;
    for (auto& value : random) value = static_cast<uint8_t>(source());
    uint64_t randomPointer = pushData(machine, platformPointer, random);
    // Room for the records that tell the dynamic linker where translated files belong. The kernel
    // layer adds a file's record when the process first looks at the file.
    uint64_t prelink = 0;
    if (machine.space && native::enabled()) {
        const std::vector<uint8_t> empty(GuestSpace::Prelink::capacity * 24);
        prelink = pushData(machine, randomPointer & ~uint64_t{15}, empty);
        machine.space->prelink = {prelink, {}, {}, {}, {}};
    }

    // Linux/AArch64 auxiliary vector key-value pairs.
    constexpr uint64_t hwcapCrc32 = uint64_t{1} << 7; // CRC32 and CRC32C at all operand widths.
    constexpr uint64_t hwcapAsimd = uint64_t{1} << 1;
    constexpr uint64_t hwcapSha2 = uint64_t{1} << 6;
    const auto* enableSha2 = std::getenv("NESTVANE_GUEST_SHA2");
    // BoringSSL checks ASIMD before selecting its SHA-256 implementation.
    const uint64_t hwcap = hwcapCrc32 | (enableSha2 && std::string_view(enableSha2) == "1" && hasNativeSha256()
                                       ? hwcapAsimd | hwcapSha2 : 0);
    const auto ids = startup.credentials.value_or(std::array<uint64_t, 4>{
        startup.uid, startup.uid, startup.uid, startup.uid});
    initial.insert(initial.end(), {
        3, startup.programHeaders, 4, startup.programHeaderSize, 5, startup.programHeaderCount,
        6, pageSize, 7, startup.interpreterBase, 9, startup.executableEntry,
        11, ids[0], 12, ids[1], 13, ids[2], 14, ids[3],
        15, platformPointer, 16, hwcap, 17, 100, // AT_CLKTCK, the Linux guest USER_HZ.
        23, 0, 25, randomPointer, 31, name,
    });
    if (prelink) initial.insert(initial.end(), {0x4e56, prelink, 0x4e57, GuestSpace::Prelink::capacity});
    initial.insert(initial.end(), {0, 0});
    if (initial.size() % 2) initial.push_back(0);
    machine.cpu.sp = pushData(machine, (prelink ? prelink : randomPointer) & ~uint64_t{15},
                              bytes(initial.data(), initial.size() * sizeof(initial.front())));
}

} // namespace

Image loadELF(std::span<const uint8_t> elf, uint64_t bias) {
    return readELF(elf, bias, ImageKind::staticExecutable).image;
}

Image loadDynamicELF(std::span<const uint8_t> elf, std::span<const uint8_t> linker,
                     std::string_view guestFilename, std::string_view interpreterPath,
                     std::vector<std::string> arguments, uint64_t uid) {
    // A translated file is loaded where it was translated for, and then its code can run natively.
    const auto* nativeProgram = field<uint16_t>(elf, 16) == 3 ? native::moduleFor(elf) : nullptr;
    const auto* nativeLinker = native::moduleFor(linker);
    const uint64_t executableBias = nativeProgram ? nativeProgram->base : field<uint16_t>(elf, 16) == 3 ? 0x4000'0000 : 0;
    const uint64_t interpreterBase = nativeLinker ? nativeLinker->base : 0x6000'0000;
    auto program = readELF(elf, executableBias, ImageKind::dynamicProgram);
    if (program.interpreter != interpreterPath || !program.programHeaders ||
        guestFilename.empty() || guestFilename.front() != '/') {
        throw std::runtime_error("Android executable interpreter or mapped headers do not match the launch profile");
    }
    auto interpreter = readELF(linker, interpreterBase, ImageKind::interpreter);
    auto& image = interpreter.image;
    for (auto& segment : program.image.segments) {
        for (const auto& existing : image.segments) {
            if (segment.address < existing.address + existing.bytes.size() &&
                existing.address < segment.address + segment.bytes.size()) {
                throw std::runtime_error("executable and interpreter mappings overlap");
            }
        }
        image.segments.push_back(std::move(segment));
    }
    image.startup = Startup{program.image.entry, interpreterBase, program.programHeaders,
                            program.programHeaderSize, program.programHeaderCount, std::string(guestFilename),
                            std::move(arguments), uid};
    image.interpreted = !nativeProgram || !nativeLinker || !native::activate(*nativeProgram) || !native::activate(*nativeLinker);
    for (const auto* module : {nativeProgram, nativeLinker})
        if (!image.interpreted) image.nativeCode.push_back({module->info[0], module->info[1]});
    // The kernel layer watches for a new program reaching its entry point.
    if (!image.interpreted) native::trap(program.image.entry);
    return image;
}

std::string elfInterpreter(std::span<const uint8_t> elf) {
    const uint64_t bias = field<uint16_t>(elf, 16) == 3 ? 0x4000'0000 : 0;
    return readELF(elf, bias, ImageKind::dynamicProgram).interpreter;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

Machine::Machine(const Image& image, const std::shared_ptr<AnonymousBudget>& budget, std::shared_ptr<GuestSpace> guestSpace)
    : space(std::move(guestSpace)), segments(image.segments), topByteIgnore(image.startup.has_value()) {
    const auto stackSize = image.startup ? image.startup->stackSize : staticStackSize;
    if (!stackSize || stackSize % pageSize || stackSize > maxGuestStackSize)
        throw std::runtime_error("invalid guest stack size");
    for (const auto& segment : segments) {
        if (segment.address < stackTop && stackTop - stackSize < segment.address + segment.bytes.size()) {
            throw std::runtime_error("ELF overlaps the guest stack");
        }
    }
    Segment stack{stackTop - stackSize, {}, readable | writable};
    if (image.startup && budget) {
        stack.reservedSize = stackSize;
        stack.anonymousPages = space ? AnonymousPages::allocateIn(space, stack.address, stackSize)
                                     : AnonymousPages::allocate(stackSize, budget);
        if (!stack.anonymousPages) throw std::bad_alloc();
    } else {
        stack.bytes.resize(stackSize);
    }
    stack.anonymous = true;
    stack.initialStack = true;
    segments.push_back(std::move(stack));
    std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) {
        return a.address < b.address;
    });
    for (auto& segment : segments) place(segment);
    if (space && image.interpreted) space->dropCode();
    if (space)
        for (const auto& range : image.nativeCode) space->addCode(range[0], range[1]);
    if (space && image.nativeCode.size() == 2) space->setLinker(image.nativeCode[1][0], image.nativeCode[1][1]);
    cpu.pc = image.entry;
    if (image.startup) {
        setupStack(*this, *image.startup);
        return;
    }
    cpu.sp = stackTop - 48; // Zero argc, argv/envp terminators and an AT_NULL pair.
}

AnonymousPages::AnonymousPages(uint64_t length, const std::shared_ptr<AnonymousBudget>& limits)
    : budget(limits), hostPage(getpagesize()), size((length + hostPage - 1) & ~(hostPage - 1)),
      dirty(size / hostPage), blocks(size / blockSize + (size % blockSize != 0)) {
    budget->reserved += size;
}

std::shared_ptr<AnonymousPages> AnonymousPages::allocate(uint64_t length, const std::shared_ptr<AnonymousBudget>& budget,
                                                        int* allocationError) {
    if (allocationError) *allocationError = 0;
    const auto hostPage = uint64_t(getpagesize());
    const auto remaining = budget->reservedLimit - budget->reserved;
    if (!length || length > (remaining & ~(hostPage - 1)))
        return {};
    try {
        return std::shared_ptr<AnonymousPages>(new AnonymousPages(length, budget));
    } catch (const std::bad_alloc&) {
        if (allocationError) *allocationError = ENOMEM;
        return {};
    }
}

AnonymousPages::AnonymousPages(const std::shared_ptr<GuestSpace>& guestSpace, uint64_t guestBase, uint64_t length)
    : space(guestSpace), base(guestBase), hostPage(GuestSpace::page), size(length) {}

std::shared_ptr<AnonymousPages> AnonymousPages::allocateIn(const std::shared_ptr<GuestSpace>& space, uint64_t base, uint64_t length,
                                                          bool map) {
    if (!length || base % GuestSpace::page || length % GuestSpace::page || base + length > GuestSpace::size) return {};
    // Host memory comes when a chunk is first touched, so a large sparse mapping costs nothing.
    if (map && !space->mapAnonymous(base, length, GuestSpace::read | GuestSpace::write, false)) return {};
    return std::shared_ptr<AnonymousPages>(new AnonymousPages(space, base, length));
}

void AnonymousPages::release(uint64_t offset, uint64_t length) {
    if (space) space->unmap(base + offset, length);
    else discard(offset, length);
}

AnonymousPages::~AnonymousPages() {
    if (space) return;  // the address space owns the memory, and release has returned what was unmapped
    for (size_t index = 0; index < blocks.size(); ++index) releaseBlock(index);
    budget->reserved -= size;
    budget->dirty -= dirtyBytes;
}

void AnonymousPages::releaseBlock(size_t index) {
    auto& block = blocks[index];
    if (!block.data) return;
    munmap(block.data, std::min<uint64_t>(blockSize, size - index * blockSize));
    block = {};
}

const uint8_t* AnonymousPages::read(uint64_t offset, size_t length) const {
    if (offset >= size || length > size - offset || length > blockSize - offset % blockSize) return nullptr;
    static constexpr std::array<uint8_t, blockSize> zero{};
    if (space) {
        // Host memory is contiguous within a chunk only: the next chunk may be in another run, behind
        // guard pages. Blocks count from the start of the store, so one may cross a chunk boundary.
        if (length > GuestSpace::chunk - (base + offset) % GuestSpace::chunk) return nullptr;
        // Untouched memory reads as zero.
        return space->backed(base + offset) ? space->host(base + offset) : zero.data() + offset % blockSize;
    }
    const auto* data = blocks[offset / blockSize].data;
    return (data ? data : zero.data()) + offset % blockSize;
}

void AnonymousPages::readInto(uint64_t offset, std::span<uint8_t> bytes) const {
    while (!bytes.empty()) {
        auto count = std::min(bytes.size(), blockSize - size_t(offset % blockSize));
        if (space) count = std::min<size_t>(count, GuestSpace::chunk - (base + offset) % GuestSpace::chunk);
        std::memcpy(bytes.data(), read(offset, count), count);
        offset += count;
        bytes = bytes.subspan(count);
    }
}

void AnonymousPages::write(uint64_t offset, std::span<const uint8_t> bytes) {
    if (space) {
        space->copyIn(base + offset, bytes.data(), bytes.size());
        return;
    }
    while (!bytes.empty()) {
        const auto within = offset % blockSize;
        const auto count = std::min(bytes.size(), blockSize - size_t(within));
        std::memmove(blocks[offset / blockSize].data + within, bytes.data(), count);
        offset += count;
        bytes = bytes.subspan(count);
    }
}

bool AnonymousPages::resident(uint64_t offset) const {
    if (space) return offset < size && space->backed(base + offset);
    return offset < size && dirty[offset / hostPage];
}

bool AnonymousPages::prepareWrite(uint64_t offset, uint64_t length, int* mappingError) {
    if (mappingError) *mappingError = 0;
    if (!length) return true;
    if (offset >= size || length > size - offset) return false;
    if (space) {
        // What is shared with another process (GuestSpace::fork) becomes this one's own before it is written.
        space->makePrivate(base + offset, length);
        for (uint64_t at = (base + offset) & ~(GuestSpace::chunk - 1); at < base + offset + length; at += GuestSpace::chunk)
            if (!space->touch(std::max(at, base + offset))) return false;
        return true;
    }
    const auto first = offset / hostPage, last = (offset + length - 1) / hostPage;
    uint64_t additional = 0;
    for (auto index = first; index <= last; ++index) {
        if (!dirty[index]) additional += hostPage;
    }
    if (!additional) return true;
    if (additional > budget->dirtyLimit - budget->dirty) return false;

    const auto firstBlock = offset / blockSize, lastBlock = (offset + length - 1) / blockSize;
    for (auto index = firstBlock; index <= lastBlock; ++index) {
        auto& block = blocks[index];
        if (block.data) continue;
        void* data = mmap(nullptr, std::min<uint64_t>(blockSize, size - index * blockSize),
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data == MAP_FAILED) {
            if (mappingError) *mappingError = errno;
            for (auto pending = firstBlock; pending < index; ++pending) {
                if (!blocks[pending].dirtyPages) releaseBlock(pending);
            }
            return false;
        }
        block.data = static_cast<uint8_t*>(data);
    }
    for (auto index = first; index <= last; ++index) {
        if (dirty[index]) continue;
        dirty[index] = true;
        ++blocks[index * hostPage / blockSize].dirtyPages;
    }
    budget->dirty += additional;
    dirtyBytes += additional;
    return true;
}

void AnonymousPages::discard(uint64_t offset, uint64_t length) {
    const auto end = offset + length;
    if (space) {
        // Whole pages are replaced by fresh ones. The partial pages at the ends are cleared in place.
        const auto first = (offset + hostPage - 1) & ~(hostPage - 1), last = end & ~(hostPage - 1);
        const auto clear = [&](uint64_t from, uint64_t to) {
            if (from >= to || !space->backed(base + from)) return;
            space->makePrivate(base + from, to - from);
            std::memset(space->host(base + from), 0, to - from);
        };
        if (first >= last) return clear(offset, end);
        clear(offset, first);
        space->discard(base + first, last - first);
        clear(last, end);
        return;
    }
    for (auto position = offset; position < end;) {
        const auto index = position / hostPage;
        const auto next = std::min(end, (index + 1) * hostPage);
        if (dirty[index]) {
            const auto blockIndex = position / blockSize;
            auto& block = blocks[blockIndex];
            auto* data = block.data + position % blockSize;
            std::memset(data, 0, next - position);
            if (position % hostPage == 0 && next - position == hostPage) {
#ifdef __APPLE__
                constexpr auto release = MADV_FREE;
#else
                constexpr auto release = MADV_DONTNEED;
#endif
                if (block.dirtyPages == 1 || madvise(data, hostPage, release) == 0) {
                    dirty[index] = false;
                    budget->dirty -= hostPage;
                    dirtyBytes -= hostPage;
                    if (!--block.dirtyPages) releaseBlock(blockIndex);
                }
            }
        }
        position = next;
    }
}

bool AnonymousPages::copyTo(AnonymousPages& target, uint64_t source, uint64_t destination,
                            uint64_t length, int* mappingError) const {
    if (mappingError) *mappingError = 0;
    const auto end = source + length;
    if (space) {
        // After fork the child's pages are already a copy in its own address space.
        if (target.space && target.space != space && target.base + destination == base + source) return true;
        for (auto position = source; position < end;) {
            const auto next = std::min(end, (position / blockSize + 1) * blockSize);
            if (space->backed(base + position)) {
                const auto targetOffset = destination + position - source;
                if (!target.prepareWrite(targetOffset, next - position, mappingError)) return false;
                target.write(targetOffset, {read(position, next - position), size_t(next - position)});
            }
            position = next;
        }
        return true;
    }
    for (auto position = source; position < end;) {
        const auto index = position / hostPage;
        const auto next = std::min(end, (index + 1) * hostPage);
        if (dirty[index]) {
            const auto targetOffset = destination + position - source;
            if (!target.prepareWrite(targetOffset, next - position, mappingError)) return false;
            target.write(targetOffset, {read(position, next - position), size_t(next - position)});
        }
        position = next;
    }
    return true;
}

struct DenseBytes::Storage {
    explicit Storage(size_t size, std::vector<uint8_t> initial = {}) : length(size) {
#ifdef __APPLE__
        // Give large guest mappings their own host pages so the kernel can fork them lazily.
        constexpr size_t minimumMapping = 256 * 1024;
        if (length >= minimumMapping) {
            void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (address != MAP_FAILED) {
                mapping = static_cast<uint8_t*>(address);
                if (!initial.empty()) std::memcpy(mapping, initial.data(), initial.size());
                return;
            }
        }
#endif
        bytes = std::move(initial);
        bytes.resize(length);
    }

    ~Storage() {
        if (mapping) munmap(mapping, mappingOffset + length);
    }

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    std::shared_ptr<Storage> clone(size_t offset, size_t size) const {
#ifdef __APPLE__
        static const auto hostPage = static_cast<size_t>(getpagesize());
        if (mapping && size >= hostPage) {
            // The guest can split inside a host page. Copy the enclosing host
            // pages and expose only the requested bytes of the new view.
            const auto sourceOffset = mappingOffset + offset;
            const auto leading = sourceOffset % hostPage;
            auto copy = std::make_shared<Storage>(0);
            vm_address_t destination = 0;
            vm_prot_t current = 0, maximum = 0;
            const auto task = mach_task_self();
            if (vm_remap(task, &destination, leading + size, 0, VM_FLAGS_ANYWHERE, task,
                         reinterpret_cast<vm_address_t>(mapping + sourceOffset - leading), true, &current, &maximum,
                         VM_INHERIT_NONE) == KERN_SUCCESS) {
                copy->mapping = reinterpret_cast<uint8_t*>(destination);
                copy->mappingOffset = leading;
                copy->length = size;
                return copy;
            }
        }
#endif
        auto copy = std::make_shared<Storage>(size);
        std::memcpy(copy->data(), data() + offset, size);
        return copy;
    }

    uint8_t* data() { return mapping ? mapping + mappingOffset : bytes.data(); }
    const uint8_t* data() const { return mapping ? mapping + mappingOffset : bytes.data(); }
    size_t capacity() const { return mapping ? length : bytes.capacity(); }

    size_t length;
    std::vector<uint8_t> bytes;
    uint8_t* mapping = nullptr;
    size_t mappingOffset = 0;
};

DenseBytes::DenseBytes(std::vector<uint8_t> bytes) {
    if (!bytes.empty()) storage = std::make_shared<Storage>(bytes.size(), std::move(bytes));
}

DenseBytes::DenseBytes(std::initializer_list<uint8_t> bytes)
    : DenseBytes(std::vector<uint8_t>(bytes)) {}

DenseBytes DenseBytes::copyRange(size_t offset, size_t length) const {
    DenseBytes copy;
    if (length) copy.storage = storage->clone(offset, length);
    return copy;
}

size_t DenseBytes::size() const { return storage ? storage->length : 0; }
size_t DenseBytes::capacity() const { return storage ? storage->capacity() : 0; }
const uint8_t* DenseBytes::data() const { return storage ? storage->data() : nullptr; }

uint8_t* DenseBytes::mutableData() {
    if (!storage) return nullptr;
    if (storage.use_count() != 1) storage = storage->clone(0, storage->length);
    return storage->data();
}

void DenseBytes::resize(size_t length) {
    if (length == size()) return;
    if (!length) {
        storage.reset();
        return;
    }
    auto resized = std::make_shared<Storage>(length);
    if (storage) std::memcpy(resized->data(), storage->data(), std::min(length, storage->length));
    storage = std::move(resized);
}

bool DenseBytes::operator==(const DenseBytes& other) const {
    return storage == other.storage ||
        (size() == other.size() && std::memcmp(data(), other.data(), size()) == 0);
}

const uint8_t* Segment::data() const {
    if (anonymousPages) return anonymousPages->read(fileOffset, mappedSize());
    if (imageView) return imageView;
    return shared ? shared->pages.data() + fileOffset : bytes.data();
}

uint8_t* Segment::data() {
    if (!anonymousPages && !imageView && !shared) return bytes.mutableData();
    return const_cast<uint8_t*>(static_cast<const Segment&>(*this).data());
}

bool Segment::prepareWrite(uint64_t offset, uint64_t length) {
    if (imageView) return false;
    if (anonymousPages) return anonymousPages->prepareWrite(fileOffset + offset, length);
    if (!shared && length) {
        try {
            bytes.mutableData();
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    return true;
}

namespace {
std::mutex retiredLock;
std::vector<std::string> retiredFiles;

size_t hostPages(size_t size) {
    return std::max<size_t>((size + GuestSpace::page - 1) & ~(GuestSpace::page - 1), GuestSpace::page);
}
} // namespace

SharedBuffer::~SharedBuffer() {
    if (mapping) munmap(mapping, reserved);
    if (!file.empty() && !exactFile) {
        const std::lock_guard lock(retiredLock);
        retiredFiles.push_back(std::move(file));
    }
}

std::vector<std::string> SharedBuffer::takeRetired(std::string_view directory) {
    const std::lock_guard lock(retiredLock);
    std::vector<std::string> taken;
    std::erase_if(retiredFiles, [&](std::string& path) {
        if (!path.starts_with(directory)) return false;
        taken.push_back(std::move(path));
        return true;
    });
    return taken;
}

uint8_t* SharedBuffer::pages() const {
    if (mapping || !reserved) return mapping;
    const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    struct stat status{};
    // Of a file that is exactly its content the pages past the end are never touched.
    if (fd < 0 || fstat(fd, &status) || (!exactFile && size_t(status.st_size) < reserved && ftruncate(fd, off_t(reserved)))) {
        if (fd >= 0) ::close(fd);
        throw std::bad_alloc();
    }
    void* memory = mmap(nullptr, reserved, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (memory == MAP_FAILED) throw std::bad_alloc();
    mapping = static_cast<uint8_t*>(memory);
    return mapping;
}

void SharedBuffer::attach(std::string path, size_t size) {
    if (mapping) munmap(mapping, reserved);
    mapping = nullptr;
    bytes = {};
    file = std::move(path);
    length = size;
    reserved = hostPages(size);
    hostInode = 0;
    ++moves;
}

void SharedBuffer::attachExact(std::string path, size_t size) {
    attach(std::move(path), size);
    exactFile = true;
}

void SharedBuffer::truncateFile(uint64_t size) const {
    if (exactFile) ::truncate(file.c_str(), off_t(size));
}

bool SharedBuffer::renameFile(std::string path, bool move) {
    if (file.empty() || (move && ::rename(file.c_str(), path.c_str()))) return false;
    file = std::move(path);
    return true;
}

bool SharedBuffer::makeExact(std::string path, uint64_t contentSize) {
    if (!file.empty()) {
        if (!renameFile(std::move(path), true)) return false;
        exactFile = true;
        truncateFile(contentSize);
        return true;
    }
    const size_t held = size();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const uint8_t* from = data();
    for (uint64_t written = 0; written < contentSize;) {
        const auto count = ::write(fd, from + written, size_t(contentSize - written));
        if (count <= 0) {
            ::close(fd);
            ::unlink(path.c_str());
            return false;
        }
        written += uint64_t(count);
    }
    ::close(fd);
    if (mapping) munmap(mapping, reserved);
    mapping = nullptr;
    bytes = {};
    length = held;
    reserved = hostPages(held);
    file = std::move(path);
    exactFile = true;
    hostInode = 0;
    ++moves;
    return true;
}

bool SharedBuffer::unback() {
    if (file.empty()) return true;
    void* memory = mmap(nullptr, reserved, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return false;
    struct stat status{};
    // Only what the file has: the pages past its end cannot be read.
    const size_t content = ::stat(file.c_str(), &status) ? 0 : std::min<size_t>(length, size_t(status.st_size));
    if (content) std::memcpy(memory, pages(), content);
    if (mapping) munmap(mapping, reserved);
    mapping = static_cast<uint8_t*>(memory);
    file.clear();
    exactFile = false;
    hostInode = 0;
    ++moves;
    return true;
}

bool SharedBuffer::forsake() {
    if (file.empty()) return true;
    void* memory = mmap(nullptr, std::max<size_t>(reserved, 1), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return false;
    if (mapping) {
        std::memcpy(memory, mapping, length);
        munmap(mapping, reserved);
    }
    mapping = static_cast<uint8_t*>(memory);
    file.clear();
    exactFile = false;
    hostInode = 0;
    ++moves;
    return true;
}

bool SharedBuffer::backWith(std::string path) {
    if (!file.empty()) return true;
    const size_t content = size();
    const size_t wanted = std::max(hostPages(content), reserved);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    void* memory = ftruncate(fd, off_t(wanted)) ? MAP_FAILED : mmap(nullptr, wanted, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (memory == MAP_FAILED) {
        ::unlink(path.c_str());
        return false;
    }
    if (content) std::memcpy(memory, data(), content);
    if (mapping) munmap(mapping, reserved);
    bytes = {};
    mapping = static_cast<uint8_t*>(memory);
    length = content;
    reserved = wanted;
    file = std::move(path);
    hostInode = 0;
    ++moves;
    return true;
}

bool SharedBuffer::makeShareable() {
    if (!file.empty()) return true;
    if (mapping) return true;
    const size_t wanted = std::max<size_t>((bytes.size() + GuestSpace::page - 1) & ~(GuestSpace::page - 1), GuestSpace::page);
    void* memory = mmap(nullptr, wanted, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return false;
    std::memcpy(memory, bytes.data(), bytes.size());
    mapping = static_cast<uint8_t*>(memory);
    length = bytes.size();
    reserved = wanted;
    bytes = {};
    ++moves;
    return true;
}

void SharedBuffer::resize(size_t size) {
    // Large storage lives in pages of its own from the start: the parts nothing has written take no
    // memory, as in a file system that keeps files sparse.
    constexpr size_t sparseFrom = 64 * 1024;
    if (!paged() && (size < sparseFrom || !makeShareable())) return bytes.resize(size);
    if (size <= reserved) {
        if (size < length) std::memset(pages() + size, 0, length - size);  // what is cut off reads as zero if it comes back
        length = size;
        return;
    }
    if (!file.empty()) {
        // The file grows, and is mapped again where there is room for all of it.
        if (mapping) munmap(mapping, reserved);
        mapping = nullptr;
        reserved = std::max<size_t>(hostPages(size), reserved + std::min<size_t>(reserved, 64 * 1024 * 1024));
        length = size;
        ++moves;
        pages();
        return;
    }
    // Room to grow into costs only address space. The pages move to the new place as they are: a copy
    // would give memory to every page of the file that nothing has written.
    const size_t wanted = std::max<size_t>((size + GuestSpace::page - 1) & ~(GuestSpace::page - 1),
                                           reserved + std::min<size_t>(reserved, 64 * 1024 * 1024));
    void* memory = mmap(nullptr, wanted, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::bad_alloc();
    vm_address_t target = reinterpret_cast<vm_address_t>(memory);
    vm_prot_t current, maximum;
    if (vm_remap(mach_task_self(), &target, reserved, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                 reinterpret_cast<vm_address_t>(mapping), FALSE, &current, &maximum, VM_INHERIT_SHARE) != KERN_SUCCESS)
        std::memcpy(memory, mapping, length);
    munmap(mapping, reserved);
    mapping = static_cast<uint8_t*>(memory);
    length = size;
    reserved = wanted;
    ++moves;
}

bool SharedFileBytes::place(const std::shared_ptr<GuestSpace>& space, uint64_t address, uint64_t offset, uint64_t length) {
    if (address % GuestSpace::page || offset % GuestSpace::page || length % GuestSpace::page || !pages.makeShareable()) return false;
    if (placedMoves != pages.moves) replace();
    unplace(space.get(), address, address + length);
    placements.push_back({space, address, offset, length});
    // Only what the storage holds now can be mapped. The rest follows when it grows.
    const uint64_t available = offset < pages.capacity() ? std::min<uint64_t>(length, pages.capacity() - offset) : 0;
    return !available || space->mapFrom(address, available, GuestSpace::read | GuestSpace::write, pages.data() + offset, true);
}

void SharedFileBytes::unplace(const GuestSpace* space, uint64_t start, uint64_t end) {
    std::vector<Placement> kept;
    for (const auto& placement : placements) {
        const auto owner = placement.space.lock();
        if (!owner) continue;
        const uint64_t last = placement.address + placement.length;
        if (owner.get() != space || last <= start || end <= placement.address) {
            kept.push_back(placement);
            continue;
        }
        if (placement.address < start) kept.push_back({placement.space, placement.address, placement.offset, start - placement.address});
        if (end < last) kept.push_back({placement.space, end, placement.offset + (end - placement.address), last - end});
    }
    placements = std::move(kept);
}

void SharedFileBytes::replace() {
    placedMoves = pages.moves;
    for (const auto& placement : placements) {
        const auto space = placement.space.lock();
        if (!space || placement.offset >= pages.capacity()) continue;
        space->mapFrom(placement.address, std::min<uint64_t>(placement.length, pages.capacity() - placement.offset),
                       GuestSpace::read | GuestSpace::write, pages.data() + placement.offset, true);
    }
}

void Machine::place(Segment& segment) {
    placeInSpace(segment);
    // A view of file contents that stayed a view must not be written through.
    if (segment.imageView && (segment.flags & writable)) {
        const auto length = segment.mappedSize();
        std::vector<uint8_t> copy(segment.imageView, segment.imageView + length);
        segment.imageView = nullptr;
        segment.bytes = DenseBytes(std::move(copy));
    }
}

void Machine::placeInSpace(Segment& segment) {
    if (space && segment.shared && !segment.beyondFileEnd && segment.address % GuestSpace::page == 0 &&
        segment.fileOffset % GuestSpace::page == 0 && segment.mappedSize() % GuestSpace::page == 0) {
        if (!segment.placed) segment.placed = segment.shared->place(space, segment.address, segment.fileOffset, segment.mappedSize());
        return;
    }
    if (!space || segment.shared || segment.beyondFileEnd || (segment.anonymousPages && segment.anonymousPages->guestSpace())) return;
    const auto size = (segment.mappedSize() + GuestSpace::page - 1) & ~(GuestSpace::page - 1);
    if (!size || segment.address % GuestSpace::page || segment.address + size > GuestSpace::size) return;
    // An ELF segment may end inside a page. The rest of that page is the segment's, unless the next
    // segment starts in it, which a 16 KiB image does not do.
    for (const auto& other : segments)
        if (&other != &segment && other.address < segment.address + size && segment.address + segment.mappedSize() <= other.address) return;
    // Read-only file contents that the loader keeps for the whole boot are mapped copy-on-write from
    // that one copy, so that processes share the pages until one of them writes.
    if (segment.imageView && !segment.anonymousPages && segment.mappedSize() == size &&
        reinterpret_cast<uintptr_t>(segment.imageView) % GuestSpace::page == 0 &&
        space->mapFrom(segment.address, size, GuestSpace::read | GuestSpace::write, segment.imageView, false)) {
        if (auto mapped = AnonymousPages::allocateIn(space, segment.address, size, false)) {
            segment.imageView = nullptr;
            segment.anonymousPages = std::move(mapped);
            segment.fileOffset = 0;
            segment.reservedSize = size;
            return;
        }
    }
    auto pages = AnonymousPages::allocateIn(space, segment.address, size);
    if (!pages) return;
    const auto length = segment.accessibleSize();
    if (segment.anonymousPages) {
        if (!segment.anonymousPages->copyTo(*pages, segment.fileOffset, 0, std::min<uint64_t>(length, size))) return;
    } else if (length && (segment.imageView || !segment.bytes.empty())) {
        if (!pages->prepareWrite(0, length)) return;
        pages->write(0, {segment.data(), size_t(length)});
    }
    segment.bytes = {};
    segment.imageView = nullptr;
    segment.anonymousPages = std::move(pages);
    segment.fileOffset = 0;
    segment.reservedSize = size;
}

void Machine::insertSegment(Segment segment) {
    place(segment);
    auto position = std::lower_bound(segments.begin(), segments.end(), segment.address,
        [](const Segment& entry, uint64_t address) { return entry.address < address; });
    segments.insert(position, std::move(segment));
}

size_t Machine::searchSegment(uint64_t address, size_t size, unsigned flags, size_t& hint) const {
    const auto matches = [&](const Segment& segment) {
        return (segment.flags & flags) == flags && segment.contains(address, size);
    };
    const auto page = address / pageSize;
    auto& cached = segmentHints[(page ^ (page >> 6)) & (segmentHints.size() - 1)];
    if (cached < segments.size() && matches(segments[cached])) return hint = cached;
    auto next = std::upper_bound(segments.begin(), segments.end(), address,
        [](uint64_t address, const Segment& entry) { return address < entry.address; });
    if (next == segments.begin() || !matches(*std::prev(next))) return segments.size();
    hint = cached = size_t(std::prev(next) - segments.begin());
    return hint;
}

bool Machine::codeInSpace(uint64_t address, uint64_t& start, uint64_t& end) const {
    size_t hint = codeHint;
    const auto index = findSegment(address, sizeof(uint32_t), executable, hint);
    if (index == segments.size()) return false;
    const auto& segment = segments[index];
    if ((segment.flags & writable) || !segment.anonymousPages || !segment.anonymousPages->guestSpace() || segment.shared ||
        segment.anonymousPages->guestBase() + segment.fileOffset != segment.address)
        return false;
    start = segment.address;
    end = segment.address + (segment.accessibleSize() & ~uint64_t{3});
    return end > start;
}

bool Machine::codeWindow(uint64_t address, uint64_t& start, uint64_t& end, const uint8_t*& base) const {
    constexpr unsigned writableFlag = 2;
    size_t hint = codeHint;
    const auto index = findSegment(address, sizeof(uint32_t), executable, hint);
    if (index == segments.size()) return false;
    const auto& segment = segments[index];
    if (segment.flags & writableFlag) return false;
    if (segment.imageView || (!segment.shared && !segment.anonymousPages)) {
        start = segment.address;
        end = segment.address + (segment.accessibleSize() & ~uint64_t{3});
        base = segment.imageView ? segment.imageView : segment.data();
        return end > start;
    }
    // Code placed in the guest address space (a library's text, copied there): as far as the run of host
    // memory it is in goes, within the segment.
    if (segment.anonymousPages && segment.anonymousPages->guestSpace() && !segment.shared) {
        const auto& guestSpace = *segment.anonymousPages->guestSpace();
        const auto toStore = segment.anonymousPages->guestBase() + segment.fileOffset - segment.address;
        const auto stored = address + toStore;
        if (!guestSpace.backed(stored)) return false;
        const auto offset = guestSpace.offsets()[stored >> GuestSpace::chunkShift];
        uint64_t low = stored & ~(GuestSpace::chunk - 1), high = low + GuestSpace::chunk;
        while (low && guestSpace.offsets()[(low - 1) >> GuestSpace::chunkShift] == offset) low -= GuestSpace::chunk;
        while (high < GuestSpace::size && guestSpace.offsets()[high >> GuestSpace::chunkShift] == offset) high += GuestSpace::chunk;
        const auto limit = segment.address + segment.accessibleSize();
        start = low >= segment.address + toStore ? low - toStore : segment.address;
        end = (high - toStore < limit ? high - toStore : limit) & ~uint64_t{3};
        base = guestSpace.host(start + toStore);
        return end > start;
    }
    // Pages of their own: the page at `address`, which is contiguous.
    const uint64_t page = address & ~(pageSize - 1);
    if (!segment.contains(page, pageSize)) return false;
    const auto* bytes = read(page, pageSize, executable);
    if (!bytes) return false;
    start = page;
    end = page + pageSize;
    base = bytes;
    return true;
}

bool Machine::dataWindow(uint64_t address, bool forWrite, uint64_t& start, uint64_t& end, uint8_t*& base) const {
    if (dataAddress(address) != address) return false;
    size_t hint = dataHint;
    const auto index = findSegment(address, 1, forWrite ? writable : readable, hint);
    if (index == segments.size()) return false;
    const auto& segment = segments[index];
    const auto limit = segment.address + segment.accessibleSize();
    if (segment.anonymousPages && segment.anonymousPages->guestSpace() && !segment.shared) {
        const auto& guestSpace = *segment.anonymousPages->guestSpace();
        // Guest address of the store at `address`, and the 1 MiB chunk it is in: host memory is
        // contiguous within a chunk, and a chunk has host pages behind it as a whole.
        const auto toStore = segment.anonymousPages->guestBase() + segment.fileOffset - segment.address;
        const auto stored = address + toStore;
        if (!guestSpace.backed(stored)) return false;
        const auto chunkStart = stored & ~(GuestSpace::chunk - 1);
        // A window for writing is this process's own memory: what it shares with another becomes its own first.
        if (forWrite && guestSpace.shared(stored)) segment.anonymousPages->guestSpace()->makePrivate(chunkStart, GuestSpace::chunk);
        start = std::max(segment.address, chunkStart - toStore);
        end = std::min(limit, chunkStart + GuestSpace::chunk - toStore);
        base = guestSpace.host(start + toStore);
        return base && end > start;
    }
    if (!forWrite && !(segment.flags & writable) && (segment.imageView || (!segment.shared && !segment.anonymousPages))) {
        start = segment.address;
        end = limit;
        base = const_cast<uint8_t*>(segment.imageView ? segment.imageView : segment.data());
        return end > start;
    }
    return false;
}

const uint8_t* Machine::read(uint64_t address, size_t size, unsigned flags,
                             std::vector<uint8_t>& scratch) const {
    if (const auto* bytes = read(address, size, flags)) return bytes;
    if (!(flags & executable)) address = dataAddress(address);
    const auto index = findSegment(address, size, flags, flags & executable ? codeHint : dataHint);
    if (index == segments.size()) {
        // No one mapping holds all of it. Bytes that lie across two or more mappings, each of which may
        // be read, are gathered from them: a buffer on the heap straddles two of the allocator's
        // mappings now and then, and a write call given such a buffer failed with EFAULT.
        if ((flags & executable) || !size || !mappedRange(address, size, flags)) return nullptr;
        try {
            scratch.resize(size);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        return readInto(address, scratch) ? scratch.data() : nullptr;
    }
    if (!segments[index].anonymousPages) return nullptr;
    const auto& segment = segments[index];
    try {
        scratch.resize(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    segment.anonymousPages->readInto(segment.fileOffset + address - segment.address, scratch);
    return scratch.data();
}

bool Machine::mappedRange(uint64_t address, size_t size, unsigned flags) const {
    if (size > UINT64_MAX - address) return false;
    const auto end = address + size;
    while (address < end) {
        const auto index = findSegment(address, 1, flags, dataHint);
        if (index == segments.size()) return false;
        const auto& segment = segments[index];
        address = segment.address + segment.accessibleSize();
    }
    return true;
}

bool Machine::readInto(uint64_t address, std::span<uint8_t> output) const {
    if (output.empty()) return true;
    if (const auto* source = read(address, output.size(), readable)) {
        std::memcpy(output.data(), source, output.size());
        return true;
    }
    address = dataAddress(address);
    if (!mappedRange(address, output.size(), readable)) return false;
    while (!output.empty()) {
        const auto& segment = segments[findSegment(address, 1, readable, dataHint)];
        const auto offset = address - segment.address;
        const auto count = std::min<uint64_t>(output.size(), segment.accessibleSize() - offset);
        if (segment.anonymousPages)
            segment.anonymousPages->readInto(segment.fileOffset + offset, output.first(count));
        else
            std::memcpy(output.data(), segment.data() + offset, count);
        output = output.subspan(count);
        address += count;
    }
    return true;
}

bool Machine::load64(uint64_t address, uint64_t& value) const {
    return readInto(address, {reinterpret_cast<uint8_t*>(&value), sizeof(value)});
}

bool Machine::write(uint64_t address, std::span<const uint8_t> bytes) {
    address = dataAddress(address);
    const auto index = findSegment(address, bytes.size(), writable, dataHint);
    if (index != segments.size()) {
        auto& segment = segments[index];
        const auto offset = address - segment.address;
        std::vector<uint8_t> copy;
        if (segment.anonymousPages &&
            !segment.anonymousPages->read(segment.fileOffset + offset, bytes.size())) {
            try {
                copy.assign(bytes.begin(), bytes.end());
            } catch (const std::bad_alloc&) {
                return false;
            }
            bytes = copy;
        }
        if (!segment.prepareWrite(offset, bytes.size())) return false;
        if (segment.anonymousPages) {
            segment.anonymousPages->write(segment.fileOffset + offset, bytes);
        } else {
            std::memmove(segment.data() + offset, bytes.data(), bytes.size());
        }
    } else {
        if (!mappedRange(address, bytes.size(), writable)) return false;
        for (auto position = address, end = address + bytes.size(); position < end;) {
            auto& segment = segments[findSegment(position, 1, writable, dataHint)];
            const auto next = std::min(end, segment.address + segment.accessibleSize());
            if (!segment.prepareWrite(position - segment.address, next - position)) return false;
            position = next;
        }
        // The input may refer to one of the segments being written.
        const std::vector<uint8_t> source(bytes.begin(), bytes.end());
        auto position = address;
        size_t copied = 0;
        while (copied < source.size()) {
            auto& segment = segments[findSegment(position, 1, writable, dataHint)];
            const auto offset = position - segment.address;
            const auto count = std::min<uint64_t>(source.size() - copied, segment.accessibleSize() - offset);
            if (segment.anonymousPages)
                segment.anonymousPages->write(segment.fileOffset + offset, std::span(source).subspan(copied, count));
            else
                std::memcpy(segment.data() + offset, source.data() + copied, count);
            position += count;
            copied += count;
        }
    }
    if (cpu.exclusiveSize && address < cpu.exclusiveAddress + cpu.exclusiveSize &&
        cpu.exclusiveAddress < address + bytes.size()) cpu.exclusiveSize = 0;
    return true;
}

bool Machine::store64(uint64_t address, uint64_t value) {
    return write(address, {reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
}

void Machine::fault(uint64_t pc, uint64_t address) {
    cpu.pc = pc;
    cpu.stop = Stop::fault;
    cpu.faultAddress = address;
}

Counters run(Machine& machine, Mode mode, uint64_t entry, std::string_view fixture) {
    Counters counters;
    std::unordered_map<uint64_t, std::vector<Instruction>> blocks;
    std::unordered_map<uint64_t, const AOTBlock*> native;
    if (mode == Mode::aot) {
        for (const auto& block : compiledBlocks()) {
            if (block.fixture != fixture) continue;
            if (auto bound = checkedBlock(machine, block, entry, counters)) {
                native.emplace(entry + block.entryOffset, bound);
            }
        }
    }
    auto& cpu = machine.cpu;
    while (cpu.stop == Stop::running && cpu.retired < fuel) {
        if (auto target = native.find(cpu.pc);
            target != native.end() && fuel - cpu.retired >= target->second->words.size()) {
            ++counters.aotEntries;
            target->second->execute(machine, cpu.pc);
            continue;
        }
        auto bytes = machine.read(cpu.pc, 4, executable);
        if (!bytes) {
            machine.fault(cpu.pc, cpu.pc);
            break;
        }
        if (mode == Mode::step) {
            auto before = cpu.retired;
            executeDecoded(machine, decode(fetch(bytes)));
            counters.interpreted += cpu.retired - before;
            continue;
        }
        if (mode == Mode::aot) ++counters.fallbackDispatches;
        ++counters.blockDispatches;
        auto [slot, inserted] = blocks.try_emplace(cpu.pc);
        auto& block = slot->second;
        if (inserted) {
            for (size_t i = 0; i < maxBlockLength; ++i) {
                uint64_t address = cpu.pc + 4 * i;
                if (i && native.contains(address)) break;
                auto word = machine.read(address, 4, executable);
                if (!word) break;
                block.push_back(decode(fetch(word)));
                if (endsBlock(block.back().op)) break;
            }
        }
        auto before = cpu.retired;
        for (const auto& instruction : block) {
            if (cpu.stop != Stop::running || cpu.retired == fuel) break;
            executeDecoded(machine, instruction);
        }
        counters.interpreted += cpu.retired - before;
    }
    if (cpu.stop == Stop::running) cpu.stop = Stop::fuelExhausted;
    return counters;
}

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::step: return "step";
        case Mode::blocks: return "blocks";
        case Mode::aot: return "aot";
    }
    return "unknown";
}

} // namespace nestvane
