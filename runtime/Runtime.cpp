#include "Runtime.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace nestvane {

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

bool contains(const Segment& segment, uint64_t address, size_t count) {
    if (address < segment.address) return false;
    uint64_t offset = address - segment.address;
    const auto length = segment.accessibleSize();
    return offset < length && count <= length - offset;
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
        auto data = machine.read(cpu.x[1], cpu.x[2], readable);
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
    auto bytes = machine.read(entry + block.entryOffset, block.words.size_bytes(), executable);
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
        return (segment.flags & executable) && contains(segment, image.entry, 4);
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
    const uint64_t randomPointer = pushData(machine, platformPointer, random);

    // Linux/AArch64 auxiliary vector key-value pairs.
    const auto ids = startup.credentials.value_or(std::array<uint64_t, 4>{
        startup.uid, startup.uid, startup.uid, startup.uid});
    initial.insert(initial.end(), {
        3, startup.programHeaders, 4, startup.programHeaderSize, 5, startup.programHeaderCount,
        6, pageSize, 7, startup.interpreterBase, 9, startup.executableEntry,
        11, ids[0], 12, ids[1], 13, ids[2], 14, ids[3],
        15, platformPointer, 16, 0, 17, 100, // AT_CLKTCK, the Linux guest USER_HZ.
        23, 0, 25, randomPointer, 31, name,
        0, 0,
    });
    if (initial.size() % 2) initial.push_back(0);
    machine.cpu.sp = pushData(machine, randomPointer & ~uint64_t{15},
                              bytes(initial.data(), initial.size() * sizeof(initial.front())));
}

} // namespace

Image loadELF(std::span<const uint8_t> elf, uint64_t bias) {
    return readELF(elf, bias, ImageKind::staticExecutable).image;
}

Image loadDynamicELF(std::span<const uint8_t> elf, std::span<const uint8_t> linker,
                     std::string_view guestFilename, std::string_view interpreterPath,
                     std::vector<std::string> arguments, uint64_t uid) {
    const uint64_t executableBias = field<uint16_t>(elf, 16) == 3 ? 0x4000'0000 : 0;
    const uint64_t interpreterBase = 0x6000'0000;
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
    return image;
}

std::string elfInterpreter(std::span<const uint8_t> elf) {
    const auto bias = field<uint16_t>(elf, 16) == 3 ? 0x4000'0000 : 0;
    return readELF(elf, bias, ImageKind::dynamicProgram).interpreter;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

Machine::Machine(const Image& image, const std::shared_ptr<AnonymousBudget>& budget)
    : segments(image.segments), topByteIgnore(image.startup.has_value()) {
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
        stack.anonymousPages = AnonymousPages::allocate(stackSize, budget);
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
    cpu.pc = image.entry;
    if (image.startup) {
        setupStack(*this, *image.startup);
        return;
    }
    cpu.sp = stackTop - 48; // Zero argc, argv/envp terminators and an AT_NULL pair.
}

AnonymousPages::AnonymousPages(uint64_t length, const std::shared_ptr<AnonymousBudget>& limits)
    : budget(limits), hostPage(getpagesize()), size((length + hostPage - 1) & ~(hostPage - 1)) {}

std::shared_ptr<AnonymousPages> AnonymousPages::allocate(uint64_t length, const std::shared_ptr<AnonymousBudget>& budget) {
    auto pages = std::shared_ptr<AnonymousPages>(new AnonymousPages(length, budget));
    if (pages->size > budget->reservedLimit - budget->reserved) return {};
    pages->dirty.resize(pages->size / pages->hostPage);
    // Only the interpreter reads these bytes as guest instructions. Host permissions are always RW.
    auto* region = mmap(nullptr, pages->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) return {};
    pages->mapping = static_cast<uint8_t*>(region);
    budget->reserved += pages->size;
    return pages;
}

AnonymousPages::~AnonymousPages() {
    if (!mapping) return;
    munmap(mapping, size);
    budget->reserved -= size;
    budget->dirty -= dirtyBytes;
}

bool AnonymousPages::resident(uint64_t offset) const {
    return offset < size && dirty[offset / hostPage];
}

bool AnonymousPages::prepareWrite(uint64_t offset, uint64_t length) {
    if (!length) return true;
    const auto first = offset / hostPage, last = (offset + length - 1) / hostPage;
    uint64_t additional = 0;
    for (auto index = first; index <= last; ++index) {
        if (!dirty[index]) additional += hostPage;
    }
    if (!additional) return true;
    if (additional > budget->dirtyLimit - budget->dirty) return false;
    for (auto index = first; index <= last; ++index) dirty[index] = true;
    budget->dirty += additional;
    dirtyBytes += additional;
    return true;
}

void AnonymousPages::discard(uint64_t offset, uint64_t length) {
    const auto end = offset + length;
    for (auto position = offset; position < end;) {
        const auto index = position / hostPage;
        const auto next = std::min(end, (index + 1) * hostPage);
        if (dirty[index]) {
            std::memset(mapping + position, 0, next - position);
            if (position % hostPage == 0 && next - position == hostPage) {
                // Darwin can reclaim the zeroed page with MADV_FREE. On Linux DONTNEED also zeros it.
#ifdef __APPLE__
                constexpr auto release = MADV_FREE;
#else
                constexpr auto release = MADV_DONTNEED;
#endif
                if (madvise(mapping + position, hostPage, release) == 0) {
                    dirty[index] = false;
                    budget->dirty -= hostPage;
                    dirtyBytes -= hostPage;
                }
            }
        }
        position = next;
    }
}

bool AnonymousPages::copyTo(AnonymousPages& target, uint64_t source, uint64_t destination, uint64_t length) const {
    const auto end = source + length;
    for (auto position = source; position < end;) {
        const auto index = position / hostPage;
        const auto next = std::min(end, (index + 1) * hostPage);
        if (dirty[index]) {
            const auto targetOffset = destination + position - source;
            if (!target.prepareWrite(targetOffset, next - position)) return false;
            std::memcpy(target.mapping + targetOffset, mapping + position, next - position);
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
        if (mapping) munmap(mapping, length);
    }

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    std::shared_ptr<Storage> clone() const {
#ifdef __APPLE__
        if (mapping) {
            auto copy = std::make_shared<Storage>(0);
            vm_address_t destination = 0;
            vm_prot_t current = 0, maximum = 0;
            const auto task = mach_task_self();
            if (vm_remap(task, &destination, length, 0, VM_FLAGS_ANYWHERE, task,
                         reinterpret_cast<vm_address_t>(mapping), true, &current, &maximum,
                         VM_INHERIT_NONE) == KERN_SUCCESS) {
                copy->mapping = reinterpret_cast<uint8_t*>(destination);
                copy->length = length;
                return copy;
            }
        }
#endif
        auto copy = std::make_shared<Storage>(length);
        std::memcpy(copy->data(), data(), length);
        return copy;
    }

    uint8_t* data() { return mapping ? mapping : bytes.data(); }
    const uint8_t* data() const { return mapping ? mapping : bytes.data(); }
    size_t capacity() const { return mapping ? length : bytes.capacity(); }

    size_t length;
    std::vector<uint8_t> bytes;
    uint8_t* mapping = nullptr;
};

DenseBytes::DenseBytes(std::vector<uint8_t> bytes) {
    if (!bytes.empty()) storage = std::make_shared<Storage>(bytes.size(), std::move(bytes));
}

DenseBytes::DenseBytes(std::initializer_list<uint8_t> bytes)
    : DenseBytes(std::vector<uint8_t>(bytes)) {}

DenseBytes::DenseBytes(const uint8_t* first, const uint8_t* last) {
    if (first == last) return;
    storage = std::make_shared<Storage>(last - first);
    std::memcpy(storage->data(), first, storage->length);
}

size_t DenseBytes::size() const { return storage ? storage->length : 0; }
size_t DenseBytes::capacity() const { return storage ? storage->capacity() : 0; }
const uint8_t* DenseBytes::data() const { return storage ? storage->data() : nullptr; }

uint8_t* DenseBytes::mutableData() {
    if (!storage) return nullptr;
    if (storage.use_count() != 1) storage = storage->clone();
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

uint64_t Segment::accessibleSize() const {
    if (anonymousPages || imageView) return mappedSize();
    if (!shared) return bytes.size();
    if (fileOffset >= shared->pages.size()) return 0;
    return std::min(mappedSize(), shared->pages.size() - fileOffset);
}

const uint8_t* Segment::data() const {
    if (anonymousPages) return anonymousPages->data() + fileOffset;
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

void Machine::insertSegment(Segment segment) {
    auto position = std::lower_bound(segments.begin(), segments.end(), segment.address,
        [](const Segment& entry, uint64_t address) { return entry.address < address; });
    segments.insert(position, std::move(segment));
}

size_t Machine::findSegment(uint64_t address, size_t size, unsigned flags, size_t& hint) const {
    const auto matches = [&](const Segment& segment) {
        return (segment.flags & flags) == flags && contains(segment, address, size);
    };
    // The hint is an index. Revalidate it against the current mappings on every use.
    if (hint < segments.size() && matches(segments[hint])) return hint;
    const auto page = address / pageSize;
    auto& cached = segmentHints[(page ^ (page >> 6)) & (segmentHints.size() - 1)];
    if (cached < segments.size() && matches(segments[cached])) return hint = cached;
    auto next = std::upper_bound(segments.begin(), segments.end(), address,
        [](uint64_t address, const Segment& entry) { return address < entry.address; });
    if (next == segments.begin() || !matches(*std::prev(next))) return segments.size();
    hint = cached = size_t(std::prev(next) - segments.begin());
    return hint;
}

const uint8_t* Machine::read(uint64_t address, size_t size, unsigned flags) const {
    if (!(flags & executable)) address = dataAddress(address);
    const auto index = findSegment(address, size, flags, flags & executable ? codeHint : dataHint);
    if (index == segments.size()) return nullptr;
    const auto& segment = segments[index];
    return segment.data() + (address - segment.address);
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
        if (!segment.prepareWrite(address - segment.address, bytes.size())) return false;
        std::memmove(segment.data() + (address - segment.address), bytes.data(), bytes.size());
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
