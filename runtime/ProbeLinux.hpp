#pragma once

#include "GraphicsPipe.hpp"
#include "GuestTouch.hpp"
#include "Runtime.hpp"
#include "Ext4.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <deque>
#include <memory>
#include <functional>
#include <utility>

struct sqlite3;

namespace nestvane {

class BpfProgram;
struct BpfMap;
class BpfTypes;

struct ProbeLinux {
    explicit ProbeLinux(uint64_t pid = 1000, uint64_t uid = 1000, uint64_t memoryLimit = 64 * 1024 * 1024);
    ~ProbeLinux();
    ProbeLinux(const ProbeLinux&) = delete;
    ProbeLinux& operator=(const ProbeLinux&) = delete;
    void mount(std::string path, std::span<const uint8_t> bytes);
    void addDirectory(std::string path, uint32_t mode);
    void addBootTmpfs(std::string path, uint32_t mode, uint32_t gid, uint64_t flags);
    void addBootCgroups();
    void alias(std::string path, std::string target);
    void mountData(const std::string& databasePath);
    void setGraphicsPipeFactory(GraphicsPipeFactory factory) { graphicsPipeFactory = std::move(factory); }
    void setGuestTouch(std::shared_ptr<GuestTouch> touch);
    bool syncData();
    Machine createMachine(const Image& image) const { return Machine(image, anonymousBudget); }
    uint64_t dirtyAnonymousBytes() const { return anonymousBudget->dirty; }

    // Handles only the implemented guest syscalls. Unknown calls remain visible to the probe.
    bool syscall(Machine& machine);
    bool deliverMemoryFault(Machine& machine);
    bool restoredSignalContext() { return std::exchange(signalContextRestored, false); }
    uint64_t threadId() const { return currentTid; }
    uint64_t processId() const;
    uint64_t parentProcessId() const { return current->process->parent; }
    bool suspendChild();
    bool blocked() const { return current->wait != Wait::none; }
    bool schedule(Machine& machine, std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::time_point::max()) {
        return machine.cpu.retired < sliceEnd || switchThread(machine, deadline);
    }
    std::string workerReport(const Machine& machine, uint64_t observedProcess = 0) const;
    void setBootContext(std::string context);
    void setCommandLine(const Startup& startup);
    bool loadBootTransition(std::string_view manifest);
    bool loadCurrentContextPolicy(std::string_view manifest);
    bool loadPropertyPolicy(std::string_view manifest);
    bool loadServicePolicy(std::string_view manifest, bool hardware);
    bool loadKeystorePolicy(std::string_view manifest, bool key = false);
    bool loadFsCreatePolicy(std::string_view manifest);
    bool hasChildEntryAt(uint64_t pc) const {
        const auto& process = *current->process;
        return process.pendingEntry == pc && process.id != pid;
    }
    std::string observeEntry(uint64_t pc);
    bool replacedImage() { return std::exchange(imageReplaced, false); }
    std::string stderrOutput;
    std::string kernelOutput;
    std::string recentKernelOutput;
    std::string androidOutput;
    std::string recentAndroidOutput;
    std::string watchdogReport;
    bool captureAndroidLogs = false;
    std::function<void(std::string_view)> androidLogObserver;
    size_t exportFiles(ProbeLinux& reader, std::string_view prefix) const;
    bool exportExt4Mount(ProbeLinux& reader, std::string_view path) const;
    std::span<const uint8_t> bootGuestFile(std::string_view path) const;

private:
    struct MountView {
        uint64_t flags;
        uint32_t mode;
        uint32_t gid = 0;
        uint64_t propagation = 0;
        std::string backing{};
        std::shared_ptr<MountView> covered{};
        std::string device{};
        const Ext4Image* ext4 = nullptr;
        bool binder = false;
        bool hostData = false;
    };
    struct MountNamespace {
        uint64_t id = 1;
        uint64_t rootPropagation = 0;
        uint64_t generation = 0;
        std::map<std::string, MountView, std::less<>> mounts{{"/dev", {6, 0755, 0, 0, "/dev"}}};
    };
    enum class Wait { none, poll, futex, signal, sleep, exited, suspended };
    struct Process;
    struct SeccompFilter {
        struct Instruction { uint16_t code; uint8_t yes, no; uint32_t value; };
        std::vector<Instruction> program;
        std::shared_ptr<const SeccompFilter> previous;
        size_t pathLength;
    };
    struct Thread {
        CPU saved;
        std::shared_ptr<Process> process;
        std::shared_ptr<MountNamespace> mountNamespace = {};
        std::shared_ptr<const SeccompFilter> seccomp;
        bool noNewPrivileges = false;
        Wait wait = Wait::none;
        uint64_t clearTid = 0, futexAddress = 0;
        std::shared_ptr<SharedFileBytes> futexBacking;
        std::shared_ptr<Process> futexAddressSpace;
        uint32_t futexMask = 0, ioPriority = 0;
        int nice = 0;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        std::optional<std::chrono::steady_clock::time_point> hostEventRetry;
        uint64_t signalMask = 0;
        uint64_t signalWaitMask = 0;
        std::string fsCreateContext, socketCreateContext;
        std::map<uint32_t, std::array<uint32_t, 32>> pendingSignals;
        std::array<uint8_t, 16> name{};
        std::array<uint64_t, 3> alternateStack{0, 2, 0};
    };

    bool switchThread(Machine& machine, std::chrono::steady_clock::time_point deadline);
    bool cloneThread(Machine& machine);
    bool forkProcess(Machine& machine);
    bool execProcess(Machine& machine);
    bool futex(Machine& machine);
    unsigned wakeFutex(uint64_t address, unsigned count, uint32_t mask,
                       const std::shared_ptr<SharedFileBytes>& backing = {});
    void wakePollers();
    bool waitForEvents();
    bool waitForHostEvents();
    bool poll(Machine& machine);
    bool exitThread(Machine& machine);
    void finishProcess(Process& process, uint32_t status, bool killed);
    bool waitChild(Machine& machine);
    bool processAlive(uint64_t id) const;
    bool restoreSignalContext(Machine& machine);
    uint64_t signalRestorer(Machine& machine);
    uint64_t installSeccomp(Machine& machine, uint64_t address);
    bool enforceSeccomp(Machine& machine);

    enum class Node { regular, temporary, directory, input, output, error, null, entropy, kernelLog, poller, event, inotify, signal, socket, mountNamespace, pipeRead, pipeWrite, execLabel, fsCreateLabel, socketCreateLabel, securityInfo, securityCreate, securityAccess, securityValidate, loopControl, loopDevice, timer, ptyMaster, ptySlave, ueventTrigger, kernelSetting, binder, processHandle, cgroupProcs, ashmem, graphicsPipe, graphicsAddressSpace, touchDevice, bpfMap, bpfProgram, bpfBtf };
    struct RecordLock {
        uint64_t owner, start, end;
        int16_t type;
    };
    struct WritableFile {
        uint64_t inode;
        uint32_t mode;
        uint32_t uid, gid;
        bool persistent = false, memoryFile = false;
        uint32_t seals = 0;
        std::vector<RecordLock> locks;
        std::shared_ptr<SharedFileBytes> storage = std::make_shared<SharedFileBytes>();
        std::map<std::string, std::vector<uint8_t>, std::less<>> attributes;
        std::shared_ptr<BpfMap> pinnedMap{};
        std::shared_ptr<const BpfProgram> pinnedProgram{};
        bool pinnedBpfObject() const { return pinnedMap || pinnedProgram; }
    };
    struct Directory;
    struct SocketState {
        uint32_t type;
        uint32_t family = 1, protocol = 0;
        uint64_t cookie = 0;
        std::shared_ptr<void> storageIdentity;
        std::string inetCgroup;
        std::vector<std::weak_ptr<Directory>> inetAncestry;
        using InetAddress = std::array<uint8_t, 28>;
        InetAddress localInetAddress{}, peerInetAddress{};
        uint32_t tcpCallbackFlags = 0, tcpSequence = 0;
        bool inetStream() const { return protocol == 6; }
        struct Datagram {
            InetAddress source;
            std::vector<uint8_t> payload;
        };
        std::deque<Datagram> datagrams;
        bool inetDatagram() const { return protocol == 17; }
        bool hasInput() const { return !inbox.empty() || !datagrams.empty(); }
        std::function<void(const SocketState&)> onClose;
        ~SocketState() { if (onClose) onClose(*this); }
        std::array<uint32_t, 3> peerCredentials{0, UINT32_MAX, UINT32_MAX};
        std::string context, peerContext;
        std::string localAddress, peerAddress;
        std::array<uint32_t, 3> credentials{0, UINT32_MAX, UINT32_MAX};
        std::weak_ptr<SocketState> peer;
        std::deque<std::vector<uint8_t>> inbox;
        std::deque<std::shared_ptr<SocketState>> pending;
        size_t backlog = 0;
        size_t queuedBytes = 0;
        uint32_t sendBuffer = 262144, receiveBuffer = 212992;
        std::chrono::microseconds receiveTimeout{}, sendTimeout{};
        bool connected = false;
        bool listening = false;
        bool receiveShutdown = false, sendShutdown = false;
        bool passCredentials = false, reuseAddress = false;
        bool logWriter = false;
        struct UeventSocket { uint32_t pid = 0, groups = 0, receiveBuffer = 212992; };
        std::optional<UeventSocket> uevent;
        std::shared_ptr<const BpfProgram> socketFilter;
        explicit SocketState(uint32_t type) : type(type) {}
    };
    bool waitForSocketTimeout(std::chrono::microseconds timeout);
    struct BinderConnection;
    struct GraphicsPipeState {
        std::string serviceHeader;
        std::unique_ptr<GraphicsPipe> backend;
        std::vector<uint8_t> unread;
        size_t offset = 0;
        bool failed = false;

        uint16_t pollEvents() const;
    };
    std::map<uint32_t, std::weak_ptr<BpfMap>> bpfMaps;
    std::map<uint32_t, std::weak_ptr<const BpfProgram>> bpfPrograms;
    uint32_t nextBpfMapId = 1, nextBpfProgramId = 1, nextBpfTypeId = 1;
    uint64_t nextSocketCookie = 1;
    size_t bpfMapBytes();
    bool canAllocateSocketStorage(size_t valueSize);
    uint64_t changeBpfCgroupAttachment(Machine& machine);
    uint64_t queryBpfCgroupPrograms(Machine& machine);
    bool openInetSocket(const std::shared_ptr<SocketState>& socket, const SocketState* listener = nullptr);
    uint64_t bindInetSocket(Machine& machine, const std::shared_ptr<SocketState>& socket);
    uint64_t reserveInetAddress(const std::shared_ptr<SocketState>& socket, SocketState::InetAddress address);
    bool filterInetAddress(const SocketState& socket, uint32_t type, SocketState::InetAddress& address,
        bool* skipBindCapability = nullptr);
    uint64_t connectInetSocket(Machine& machine, const std::shared_ptr<SocketState>& socket);
    uint64_t connectInetStream(const std::shared_ptr<SocketState>& socket, const SocketState::InetAddress& destination);
    bool transmitInetStream(SocketState& from, SocketState& to, std::span<const uint8_t> payload, uint32_t flags);
    std::shared_ptr<void> lookupInetSocket(std::span<const uint8_t> tuple, bool tcp,
        const SocketState* receiving = nullptr) const;
    uint64_t sendInetDatagram(Machine& machine, const std::shared_ptr<SocketState>& socket,
        std::span<const std::array<uint64_t, 2>> vectors, uint64_t address = 0, uint64_t addressLength = 0);
    uint64_t receiveInetDatagram(Machine& machine, SocketState& socket, uint64_t address, uint64_t size,
        uint64_t flags = 0, uint64_t source = 0, uint64_t sourceLength = 0);
    uint64_t getInetSocketOption(Machine& machine, SocketState& socket);
    std::optional<uint64_t> setInetSocketOption(Machine& machine, SocketState& socket,
        int32_t& level, int32_t& name, uint32_t& size, std::vector<uint8_t>& buffer);
    struct CgroupSocketPrograms {
        uint32_t flags;
        std::weak_ptr<Directory> group;
        std::vector<std::shared_ptr<const BpfProgram>> programs;
    };
    using CgroupSocketHooks = std::map<std::string, CgroupSocketPrograms, std::less<>>;
    CgroupSocketHooks* bpfCgroupHooks(uint32_t type);
    CgroupSocketHooks inetCreatePrograms, inetGetOptionPrograms, inetSetOptionPrograms;
    std::map<uint32_t, CgroupSocketHooks> inetAddressPrograms;
    CgroupSocketHooks inetIngressPrograms, inetEgressPrograms, inetSocketOperationPrograms;
    std::map<std::tuple<uint32_t, uint32_t, uint16_t>, std::weak_ptr<SocketState>> inetBindings;
    uint16_t nextEphemeralPort = 32768;
    bool inetLoopbackUp = true;
    std::shared_ptr<CgroupSocketHooks> inetReleasePrograms = std::make_shared<CgroupSocketHooks>();
    uint64_t bpfMapCall(Machine& machine);
    uint64_t loadBpfProgram(Machine& machine);
    uint64_t testBpfProgram(Machine& machine);
    uint64_t bpfObjectIdCall(Machine& machine);
    uint64_t loadBpfTypes(Machine& machine);
    uint64_t bpfObjectInfo(Machine& machine);
    uint64_t createBpfToken(Machine& machine);
    std::vector<std::weak_ptr<const BpfTypes>> bpfTypes;
    struct Descriptor;
    struct GraphicsRegion {
        uint64_t offset = 0;
        SharedFileBytes storage;
        bool released = false;
    };
    struct GraphicsAddressSpace {
        struct Claim {
            std::shared_ptr<GraphicsRegion> region;
            uint64_t size;
        };
        uint64_t type = 0;
        std::map<uint64_t, std::shared_ptr<GraphicsRegion>> owned;
        std::multimap<uint64_t, Claim> claims;

        ~GraphicsAddressSpace() {
            for (const auto& [offset, region] : owned) region->released = true;
        }
        std::shared_ptr<GraphicsRegion> mapping(uint64_t offset, uint64_t size) const;
    };
    std::map<uint64_t, std::weak_ptr<GraphicsRegion>> graphicsRegions;
    std::shared_ptr<GraphicsRegion> allocateGraphicsRegion(uint64_t requested);
    uint64_t graphicsAddressIoctl(Machine& machine, const Descriptor& fd);
    GraphicsPipeFactory graphicsPipeFactory;
    std::shared_ptr<GuestTouch> guestTouch;
    std::weak_ptr<GuestTouch::Reader> guestTouchReader;
    uint64_t touchIoctl(Machine& machine, const Descriptor& fd);
    struct EventState;
    struct Pipe;
    struct Watch {
        uint32_t mask;
        uint64_t data;
        std::weak_ptr<EventState> source;
        std::weak_ptr<SocketState> socket;
        bool eventCounter;
        std::weak_ptr<BinderConnection> binder{};
        std::weak_ptr<GraphicsPipeState> graphics{};
        std::weak_ptr<Pipe> pipe{};
        std::weak_ptr<int> pipeEnd{};
        bool pipeReader = false;
        std::weak_ptr<GuestTouch> touch{};
        std::weak_ptr<GuestTouch::Reader> touchReader{};
        std::weak_ptr<BpfMap> ring{};
    };
    struct EventState {
        struct Timer {
            std::optional<std::chrono::steady_clock::time_point> next;
            std::chrono::nanoseconds interval{};
            bool wakeAlarm = false;
        };
        std::optional<Timer> timer;
        uint64_t counter = 0;
        uint64_t signalMask = 0;
        std::shared_ptr<MountNamespace> mountNamespace{};
        uint64_t observedMountGeneration = 0;
        std::map<uint32_t, Watch> watches;
        struct FileWatch { std::string path; uint32_t mask; bool directory; };
        struct FileEvent {
            int32_t watch;
            uint32_t mask, cookie;
            std::string name{};
            bool operator==(const FileEvent&) const = default;
        };
        std::map<int32_t, FileWatch> fileWatches;
        std::deque<FileEvent> fileEvents;
        int32_t nextFileWatch = 1;
        void queueFileEvent(FileEvent event);
    };
    std::vector<std::weak_ptr<EventState>> timers;
    std::vector<std::weak_ptr<EventState>> inotifyInstances;
    uint32_t nextMoveCookie = 1;
    bool inotify(Machine& machine);
    bool readInotify(Machine& machine, const Descriptor& fd);
    void fileEvent(std::string_view path, uint32_t mask, bool directory = false, uint32_t cookie = 0);
    void fileMoved(std::string_view source, std::string_view destination, bool directory);
    std::optional<std::chrono::steady_clock::time_point> refreshTimers(std::chrono::steady_clock::time_point now);
    struct Pipe {
        std::deque<uint8_t> data;
        std::weak_ptr<int> reader, writer;
        static constexpr size_t capacity = 65536;
        uint16_t pollEvents(bool readEnd) const;
    };
    struct Pty {
        uint32_t number, owner;
        bool locked = true, slaveOpened = false;
        std::array<std::deque<uint8_t>, 2> input;
        std::array<std::weak_ptr<int>, 2> ends;
        std::array<uint32_t, 9> termios{0x500, 5, 0xbf, 0x8a3b};
        std::array<uint16_t, 4> window{};
    };
    struct BinderObject {
        uint32_t type, flags;
        uint64_t value, cookie;
    };
    struct BinderNode {
        std::weak_ptr<BinderConnection> owner;
        BinderObject object;
    };
    struct BinderReference {
        std::shared_ptr<BinderNode> node;
        uint32_t inBuffer = 0, strong = 0, weak = 0;
    };
    struct BinderAllocation {
        size_t size;
        std::vector<uint32_t> handles;
        bool delivered = false;
    };
    struct BinderTransaction {
        uint64_t target, cookie;
        uint32_t code, flags, senderPid, senderUid;
        uint64_t dataSize, offsetsSize, buffer, offsets;
    };
    struct BinderRoute {
        std::weak_ptr<BinderConnection> connection;
        uint64_t thread;
        bool acceptsFiles = false;
    };
    struct BinderFileTransfer {
        size_t offset;
        std::shared_ptr<Descriptor> file;
    };
    struct BinderPacket {
        uint32_t command;
        BinderTransaction transaction{};
        uint64_t securityContext = 0;
        std::optional<BinderRoute> reply{};
        std::array<uint64_t, 2> reference{};
        std::vector<BinderFileTransfer> files{};
    };
    struct BinderDeath {
        std::weak_ptr<BinderConnection> target;
        uint64_t cookie, thread;
        bool sent = false, acknowledged = false, cleared = false;
    };
    struct BinderConnection {
        uint64_t owner;
        uint32_t maxThreads = 15;
        std::shared_ptr<SharedFileBytes> mapped{};
        uint64_t address = 0, target = 0, cookie = 0;
        bool sendSecurityContext = false, managerAcceptsFiles = true;
        std::map<uint64_t, BinderAllocation> allocations;
        std::map<uint32_t, BinderReference> handles;
        std::map<uint32_t, BinderDeath> deaths;
        std::map<uint64_t, std::shared_ptr<BinderNode>> localNodes;
        uint32_t nextHandle = 1;
        std::map<uint64_t, std::deque<BinderPacket>> directed;
        std::map<uint64_t, std::vector<BinderRoute>> replies;
        std::deque<BinderPacket> incoming;
        std::set<uint64_t> loopers;
        explicit BinderConnection(uint64_t owner) : owner(owner) {}
    };
    struct Ashmem {
        WritableFile file{};
        uint64_t size = 0;
        unsigned protection = 7;
        std::string name;
        bool allocated = false;
    };
    struct Descriptor {
        std::string path;
        std::span<const uint8_t> bytes;
        uint32_t flags;
        Node node;
        std::shared_ptr<uint64_t> offset = std::make_shared<uint64_t>(0);
        std::shared_ptr<WritableFile> temporary = {};
        std::shared_ptr<EventState> event = {};
        std::shared_ptr<SocketState> socket = {};
        std::shared_ptr<MountNamespace> mountNamespace = {};
        std::string visiblePath{};
        std::shared_ptr<std::string> snapshot = {};
        std::shared_ptr<Pipe> pipe{};
        std::shared_ptr<int> pipeEnd{};
        std::shared_ptr<std::string> securityReply{};
        uint64_t targetProcess = 0;
        std::shared_ptr<Pty> pty{};
        std::shared_ptr<int> ptyEnd{};
        std::shared_ptr<BinderConnection> binder{};
        const Ext4Node* ext4File = nullptr;
        std::shared_ptr<Directory> cgroupFile{}, cgroupDirectory{};
        std::shared_ptr<Ashmem> ashmem{};
        std::shared_ptr<GraphicsPipeState> graphics{};
        std::shared_ptr<GraphicsAddressSpace> graphicsAddress{};
        std::shared_ptr<GuestTouch> touch{};
        std::shared_ptr<GuestTouch::Reader> touchReader{};
        std::shared_ptr<BpfMap> bpfMap{};
        std::shared_ptr<const BpfTypes> btf{};
        std::shared_ptr<const BpfProgram> bpfProgram{};
        bool bpfFilesystemRoot = false;
        std::span<const uint8_t> contents() const { return ext4File ? ext4File->bytes() : bytes; }
        uint64_t contentSize() const { return ext4File ? ext4File->size : bytes.size(); }
        static constexpr uint32_t mutableStatus = 0xc00; // O_APPEND and O_NONBLOCK
        std::shared_ptr<uint32_t> status = std::make_shared<uint32_t>(flags & mutableStatus);
        uint32_t effectiveFlags() const { return (flags & ~mutableStatus) | *status; }
        bool pathOnly() const { return flags & 0x200000; }
    };
    std::optional<std::string> resolve(std::string_view path) const;
    std::optional<std::string> followLinks(std::string path, bool followFinal = true) const;
    uint64_t writeDescriptor(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size,
                             std::optional<uint64_t> position = {});
    uint64_t open(const Machine& machine, const std::string& path, uint32_t flags, uint32_t mode);
    uint64_t stat(Machine& machine, uint64_t address, const Descriptor& fd) const;
    uint64_t resize(const WritableFile& file, uint64_t size);
    uint64_t readGraphics(Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size);
    uint64_t writeGraphics(const Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size);
    uint64_t ashmemIoctl(Machine& machine, const Descriptor& fd);
    bool dataPath(std::string_view path) const;
    bool canAccessParent(std::string_view path, bool modify) const;
    uint64_t renameFile(Machine& machine);
    uint64_t renameDirectory(const std::string& source, const std::string& destination, bool noReplace);
    sqlite3* dataDatabase = nullptr;
    int64_t dataGeneration = 0;
    static constexpr uint64_t maxDataFileBytes = 64 * 1024 * 1024;
    static constexpr uint64_t maxWritableBytes = 128 * 1024 * 1024;
    std::vector<std::weak_ptr<SharedFileBytes>> memoryFiles;
    std::optional<Descriptor> find(std::string_view path, uint32_t flags, const Machine* machine = nullptr) const;
    uint64_t freeMemory(const Machine& machine) const;
    uint64_t writeFileBytes(const WritableFile& file, uint64_t offset, std::span<const uint8_t> bytes);
    std::string backingPath(std::string_view path) const;
    static std::string backingPath(std::string_view path, const MountNamespace& mountNamespace);
    const MountView* mountFor(std::string_view path) const;
    uint64_t bindMount(std::string_view from, const std::string& to, bool recursive);
    uint64_t mountFilesystem(Machine& machine);
    uint64_t mountBinder(const std::string& path, uint64_t flags, std::string_view options);
    uint64_t binderIoctl(Machine& machine, const Descriptor& fd);
    uint64_t binderCommand(Machine& machine, const Descriptor& fd, uint32_t command,
                           std::span<const uint8_t> data);
    uint64_t binderWriteRead(Machine& machine, const Descriptor& fd);
    bool binderReadable(const BinderConnection& connection) const;
    bool freeBinderBuffer(BinderConnection& connection, uint64_t address, bool delivered);
    uint64_t storeBinderBuffer(BinderConnection& target, std::span<const uint8_t> data,
                              std::span<const uint8_t> offsets, std::span<const uint8_t> extra, std::string_view context);
    uint64_t mountExt4(const std::string& device, const std::string& path, uint64_t flags);
    uint64_t loopIoctl(Machine& machine, const Descriptor& fd);
    uint64_t ptyIoctl(Machine& machine, const Descriptor& fd);
    static uint16_t ptyPoll(const Descriptor& fd);
    uint64_t openPty(Descriptor& fd);
    std::map<std::string, std::weak_ptr<Pty>, std::less<>> ptys;
    void addLoopDevice(unsigned number);
    uint64_t unmountFilesystem(Machine& machine);
    bool writablePath(std::string_view path) const;
    bool directoryExists(std::string_view path) const;
    static std::string mountList(const MountNamespace& group);
    uint64_t writableAllocation() const;
    uint64_t statFilesystem(Machine& machine, uint64_t address, std::string_view path) const;
    static constexpr uint32_t descriptorCapacity = 4096, defaultDescriptorLimit = 1024;
    uint32_t descriptorLimit() const;
    uint32_t freeDescriptor(uint32_t minimum = 0) const;
    uint64_t recordLock(Machine& machine, const Descriptor& fd);
    static void releaseRecordLocks(const Descriptor& fd, uint64_t owner);
    uint64_t readDirectory(Machine& machine, const Descriptor& fd) const;
    uint64_t socketRead(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size);
    using SocketVector = std::array<uint64_t, 2>; // Guest iovec: pointer and length.
    uint64_t socketWrite(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size);
    uint64_t socketWrite(Machine& machine, const Descriptor& fd, std::span<const SocketVector> vectors);
    bool socketCall(Machine& machine);
    uint64_t recvUevent(Machine& machine, SocketState& endpoint);
    void emitLoopUevent(unsigned number);
    std::vector<std::weak_ptr<SocketState>> ueventListeners;
    std::map<std::string, unsigned, std::less<>> ueventTriggers;
    uint64_t ueventSequence = 0;
    bool eventCall(Machine& machine);
    bool fileCall(Machine& machine);
    uint64_t makePipe(Machine& machine);
    bool securityCall(Machine& machine);
    uint64_t map(Machine& machine);
    uint64_t randomMappingBase() const;
    struct Setting { unsigned value, minimum, maximum; };
    std::array<int32_t, 2> guestTimezone{};
    std::map<std::string, Setting, std::less<>> kernelSettings{
        {"/proc/sys/vm/mmap_rnd_bits", {24, 14, 24}},
        {"/proc/sys/kernel/kptr_restrict", {2, 0, 2}},
        {"/proc/sys/kernel/unprivileged_bpf_disabled", {2, 0, 2}},
    };

    struct LoopDevice {
        std::string file;
        std::span<const uint8_t> bytes{};
        std::array<uint8_t, 232> info{};
        uint32_t blockSize = 512, number = 0, mode = 0600, uid = 0, gid = 0;
        uint64_t readAhead = 256;
    };
    struct MountedExt4 {
        std::string backing;
        Ext4Image image;
    };
    std::map<std::string, LoopDevice, std::less<>> loopDevices;
    std::map<std::tuple<std::string, uint64_t, uint64_t>, MountedExt4> mountedExt4;
    std::map<std::string, const Ext4Node*, std::less<>> ext4Nodes;
    size_t ext4DirectoryCount = 0, ext4LinkCount = 0;
    std::map<std::string, std::span<const uint8_t>, std::less<>> files;
    std::map<std::string, std::string, std::less<>> links;
    struct Process {
        bool expeditedMembarrier = false;
        uint64_t id;
        uint64_t parent = 0;
        std::string executable;
        std::string programName;
        std::string commandLine;
        std::string context, pendingContext;
        std::string cgroup;
        uint64_t group = 0, session = 0;
        uint32_t umask = 0022, secureBits = 0;
        uint8_t mdwe = 0;
        std::array<uint32_t, 3> userIds{}, groupIds{}; // real, effective, saved
        std::vector<uint32_t> groups;
        static constexpr uint64_t allCapabilities = (uint64_t{1} << 41) - 1;
        uint64_t effectiveCaps = 0, permittedCaps = 0, inheritableCaps = 0, ambientCaps = 0;
        uint64_t capabilityBound = allCapabilities;
        std::optional<uint64_t> pendingEntry;
        uint64_t nextMapping = 0x1'0000'0000;
        std::map<uint32_t, std::array<uint64_t, 2>> limits;
        std::map<uint32_t, Descriptor> descriptors;
        std::vector<Segment> segments;
        std::array<std::array<uint64_t, 4>, 65> signalActions{};
        uint64_t signalTrampoline = 0;
        struct ChildSignal { uint32_t pid, uid, status, code; };
        std::optional<ChildSignal> childSignal;
    };
    auto& descriptors() { return current->process->descriptors; }
    const auto& descriptors() const { return current->process->descriptors; }
    struct BoundSocket {
        std::weak_ptr<SocketState> endpoint;
        uint32_t mode, uid, gid;
    };
    std::map<std::string, BoundSocket, std::less<>> socketPaths;
    std::map<std::string, std::shared_ptr<WritableFile>, std::less<>> writableFiles;
    struct Directory {
        uint32_t mode, uid = 0, gid = 0;
        std::string securityContext{};
    };
    struct BinderDevice : Directory {
        uint32_t minor;
        std::weak_ptr<BinderConnection> manager{};
    };
    std::vector<std::weak_ptr<BinderConnection>> binderDeathSubscribers;
    bool refreshBinderDeaths();
    std::string binderTrace;
    std::map<std::string, BinderDevice, std::less<>> binderDevices;
    uint32_t nextBinderMinor = 0;
    std::map<std::string, Directory, std::less<>> directories{{"/dev", {0755}}};
    std::map<std::string, std::shared_ptr<Directory>, std::less<>> cgroupFiles;
    bool canWriteCgroup(const Directory& node) const;
    uint64_t moveToCgroup(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size);
    struct BootTransition {
        std::string source, file, destination;
        uint32_t processClass;
    };
    std::map<std::string, BootTransition, std::less<>> bootTransitions;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> currentContextTransitions;
    std::map<std::string, std::string, std::less<>> kernelFiles;
    std::string ashmemBootPath;
    struct AccessClass {
        uint32_t mask = 0;
        std::map<std::pair<std::string, std::string>, std::string> decisions;
    };
    std::map<uint32_t, AccessClass> accessClasses;
    uint32_t fsCreateClass = 0, fsCreatePermission = 0, socketCreatePermission = 0;
    bool loadAccessClass(std::string_view manifest, std::string_view name,
                         std::span<const std::string_view> permissions);
    uint64_t nextNamespace = 2;
    uint64_t nextMountBacking = 1;
    uint64_t nextInode = 65536;
    uint32_t userId() const { return current->process->userIds[1]; }
    uint32_t groupId() const { return current->process->groupIds[1]; }
    unsigned permissionShift(uint32_t uid, uint32_t gid) const;
    uint64_t pid, memoryLimit;
    std::shared_ptr<AnonymousBudget> anonymousBudget;
    struct ChildExit { uint64_t parent; uint32_t status, uid, code; };
    std::map<uint64_t, ChildExit> childExits;
    std::map<uint64_t, Thread> threads;
    std::map<std::string, uint64_t, std::less<>> propertyPollers;
    Thread* current;
    uint64_t currentTid, nextTid;
    bool imageReplaced = false;
    bool signalContextRestored = false;
    uint64_t sliceEnd = UINT64_MAX;
};

} // namespace nestvane
