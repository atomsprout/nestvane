#pragma once

#include "GuestAudio.hpp"
#include "GuestBattery.hpp"
#include "GraphicsPipe.hpp"
#include "GuestApkInbox.hpp"
#include "GuestCamera.hpp"
#include "GuestRoute.hpp"
#include "HostSocket.hpp"
#include "GuestTouch.hpp"
#include "GuestColorDma.hpp"
#include "Runtime.hpp"
#include "NativeCode.hpp"
#include "PosixAcl.hpp"
#include "GuestFirewall.hpp"
#include "Ext4.hpp"

#include <stop_token>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

struct SpaceCode;

class BpfProgram;
struct BpfMap;
class BpfTypes;

using GuestInodeTimes = std::array<int64_t, 6>; // atime, mtime and ctime, as seconds/nanoseconds pairs.

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
    void setCamera(std::shared_ptr<GuestCameraSource> source) { camera = std::move(source); }
    void setGraphicsPipeFactory(GraphicsPipeFactory factory) { graphicsPipeFactory = std::move(factory); }
    void setTetheringLoader(std::span<const uint8_t> executable) { tetheringLoader = executable; }
    void setApkInbox(std::shared_ptr<GuestApkInbox> inbox) { apkInbox = std::move(inbox); }
    void setGuestTouch(std::shared_ptr<GuestTouch> touch);
    void setGraphicsDma(std::shared_ptr<GuestColorDma> dma) {
        graphicsDma = std::move(dma);
        // The main loop waits on the transfers' signal when it has one: a worker's finished run ends that wait.
        if (graphicsDma) native::setFinishedHook([dma = graphicsDma] { dma->poke(); });
        else native::setFinishedHook(nullptr);
    }
    bool syncData();
    bool syncDataSoon();
    std::chrono::steady_clock::time_point lastDataSync{};
    bool dataSyncPending = false;
    // With native memory, every process gets a guest address space and its private lazy memory lives
    // there (NESTVANE_NATIVE_MEMORY in the environment turns it on; it needs 16 KiB guest pages).
    void setNativeMemory(bool enabled) { nativeMemory = enabled; }
    Machine createMachine(const Image& image) const {
        return Machine(image, anonymousBudget, nativeMemory ? std::make_shared<GuestSpace>() : nullptr);
    }
    uint64_t dirtyAnonymousBytes() const { return anonymousBudget->dirty; }
    // Whether the current thread is at translated code that its process has mapped where it belongs.
    bool nativeAt(const Machine& machine) const { return machine.space && machine.space->hasCode(machine.cpu.pc); }

    // Handles only the implemented guest syscalls. Unknown calls remain visible to the probe.
    bool syscall(Machine& machine);
    bool deliverMemoryFault(Machine& machine);
    // The current thread met an instruction the interpreter does not have. Its process gets SIGILL:
    // its handler runs, or the process ends, as on a processor without the instruction. False for
    // the first process, whose end is the end of the run.
    bool deliverIllegalInstruction(Machine& machine);
    bool restoredSignalContext() { return std::exchange(signalContextRestored, false); }
    uint64_t threadId() const { return currentTid; }
    // Whether the current process is an app the person installed (SELinux domain untrusted_app).
    bool currentIsApp() const;
    uint64_t processId() const;
    uint64_t parentProcessId() const { return current->process->parent; }
    bool suspendChild();
    // What a descriptor of the current process is, for a log line: its path, or for a socket its family,
    // type, protocol, addresses and whether it is connected.
    std::string describeDescriptor(uint64_t fd);
    bool blocked() const { return current->wait != Wait::none; }
    const std::string& currentExecutable() const { return current->process->executable; }
    const std::string& currentContext() const { return current->process->context; }
    bool schedule(Machine& machine, std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::time_point::max(), const std::stop_token& stop = {}) {
        if (!(machine.cpu.retired & 4095) && graphicsDma) serviceGraphicsDma();
        return machine.cpu.retired < sliceEnd || switchThread(machine, deadline, stop);
    }
    bool hasSliceBudget(uint64_t retired, uint64_t count) const {
        return retired <= sliceEnd && count <= sliceEnd - retired;
    }
    std::string workerReport(const Machine& machine, uint64_t observedProcess = 0) const;
    std::string memoryReport(const Machine& machine) const;
    // How long the scheduler slept because no guest thread could run, how often, and thread switches.
    std::chrono::steady_clock::time_point lastTimerScan{};
    std::chrono::steady_clock::duration idleTime{};
    uint64_t idleWaits = 0, threadSwitches = 0;
    // Hands the current thread's native code to a worker if it has shown to compute for long. The
    // scheduler goes on with other threads and the thread has its next turn when the run has ended.
    bool offloadNative(Machine& machine);
    // Hands the current thread to a worker that interprets it in `code` (native::offloadInterpreter),
    // when it keeps using whole turns there. False when it stays.
    bool offloadInterpreter(Machine& machine, const SpaceCode& code);

    // How the current thread's run on a worker ended, once, at the thread's next turn.
    std::optional<native::Result> takeNativeResult();
    // The current thread's native code ran until its turn was over.
    void noteNativeTurnUsed() { current->cpuBound = true; }
    uint64_t nativeOffloads = 0;
    // A thread's turn, in interpreted instructions. A turn of native code is one time slice and counts as this many.
    uint64_t sliceLength = 4096;
    std::optional<std::pair<std::string_view, uint64_t>> guestCodeFile(const uint8_t* instruction) const;
    std::string guestCodeFileName(std::string_view backingPath) const;
    void setBootContext(std::string context);
    void setCommandLine(const Startup& startup, const Machine* machine = nullptr);
    bool loadBootTransition(std::string_view manifest);
    bool loadBootFileLabels(std::string_view manifest);
    bool loadCurrentContextPolicy(std::string_view manifest);
    bool loadPropertyPolicy(std::string_view manifest);
    bool loadServicePolicy(std::string_view manifest, bool hardware);
    bool loadKeystorePolicy(std::string_view manifest, bool key = false);
    bool loadFsCreatePolicy(std::string_view manifest);
    bool loadSelinuxPolicyVersion(std::string_view manifest);
    bool loadPackageRelabelPolicy(std::string_view manifest, bool directory);
    bool loadSeappContexts(std::string_view rules);
    bool loadAppTransitionDomains(std::string_view domains);
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
    // For Export Diagnostics (DynamicProbe.hpp): the last of Android's log with the time and the process
    // of each line, in two halves that take turns (the older is dropped when the newer fills), and while
    // a report is being made, everything logged since it was asked for.
    std::string diagnosticLogOlder, diagnosticLog, diagnosticCapture;
    bool diagnosticCapturing = false;
    void keepDiagnosticLine(std::string_view message);
    std::string diagnosticLogText() const { return diagnosticLogOlder + diagnosticLog; }
    // Whether the guest reaches the network through the host's sockets. Without it there is only loopback.
    bool hostNetwork = false;
    // The guest's sound card: a control device with no controls, /dev/snd/controlC0, and one playback
    // device, /dev/snd/pcmC0D0p, of the kind Android's audio service plays through. What is played goes to runtime/GuestAudio.hpp. Nothing or a wait.
    void enableSound();
    std::optional<uint64_t> soundIoctl(Machine& machine, bool nonblocking, bool capture);
    uint64_t soundPending();
    uint64_t soundRoom();
    // /dev/dma_heap/system, where the codecs get the memory of their buffers: each allocation is a
    // file of memory, like memfd_create's, that can be mapped and passed between processes.
    uint64_t allocateDmaBuffer(Machine& machine);
    uint32_t soundFrameBytes = 4, soundRate = 48000, soundBufferFrames = 4096, soundAvailableMinimum = 1024;
    uint64_t soundFrames = 0;
    // With nobody taking the sound: the frame the card had played at a moment, from which the clock plays on.
    uint64_t soundClockFrames = 0;
    std::chrono::steady_clock::time_point soundClockStart{};
    // The card's capture device, /dev/snd/pcmC0D0c: what the host's microphone hears
    // (GuestMicrophone in runtime/GuestAudio.hpp), or silence at the pace of the clock.
    uint64_t microphoneAvailable();
    uint32_t microphoneFrameBytes = 2, microphoneRate = 48000, microphoneBufferFrames = 4096, microphoneAvailableMinimum = 1;
    uint64_t microphoneFrames = 0, microphoneClockFrames = 0;
    bool microphoneFromHost = false;
    std::chrono::steady_clock::time_point microphoneClockStart{};
    // Gives the guest the host's battery (runtime/GuestBattery.hpp) as a power supply of its own, and
    // a charger beside it. Without it Android shows a battery of unknown state.
    void enableBattery();
    void refreshBattery();
    // Makes a host directory the guest's shared storage (see sharedHostRoot). Before mountData.
    void shareStorage(std::string hostDirectory) { sharedHostRoot = std::move(hostDirectory); }
    // Files put into the shared folder from outside while Android runs, from the Files app or a
    // computer, and files removed there: the guest's tables follow the host directory, as when the
    // data is mounted, with inotify events, and the media library is asked to look again. Looked at
    // every few seconds; only directories whose modification time changed are listed again.
    void rescanShared();
    // Gives the guest its Ethernet interface and lets its sockets out through the host's.
    void enableHostNetwork();
    // Makes a Java process print the stacks of its threads into the Android log, the way SIGQUIT does:
    // for finding what a thread that makes no progress is waiting for. `name` is its main thread's name.
    bool requestJavaDump(std::string_view name);
    bool javaDumpsToLog = false;
    // Where a path leads for the calling thread and who may use each directory on the way, for a report.
    std::string pathReport(std::string_view path) const;
    // The contents of the guest's writable files under a directory, for a report: at most `limit` bytes of each.
    std::string guestFiles(std::string_view directory, size_t limit) const;
    // The call that is about to wait does so for a host socket, which cannot wake it: it looks again soon.
    bool hostSocketWait = false;
    std::function<void(std::string_view)> androidLogObserver, watchdogObserver;
    // Built-in root, off unless the person turns it on. Then /system/bin/su and /system/xbin/su are
    // the shell, and starting one gives the app root once rootDecision allows it: 1 allows, 0 refuses,
    // -1 is no answer yet (the start waits for one). Root's shell runs in rootContext, which SELinux
    // allows everything, without the app's seccomp filter.
    void enableRoot();
    // The commands that are toybox under other names (ls, id, cp, ps...): the image has toybox but
    // not its links, and apps that run a command, as root or not, expect them in /system/bin.
    void addToyboxLinks();
    // What the app has set for Android: whether root is on (asked at each start of Android, so the switch
    // in Settings applies from the next start without closing the app), and who decides.
    struct RootPolicy {
        std::function<bool()> enabled;
        std::function<int(uint32_t uid, const std::string& package)> decision;
        // Every answer given so far, by package (or "uid <n>"), and a new one: the KernelSU Manager
        // reads and changes the same answers the person gave (ProbeKernelSu.cpp).
        std::function<std::vector<std::pair<std::string, bool>>()> answers;
        std::function<void(const std::string& package, bool allowed)> remember;
    };
    static void setRootPolicy(RootPolicy policy);
    static RootPolicy rootPolicy();
    bool rootEnabled() const { return rootOn; }
    std::function<int(uint32_t uid, const std::string& package)> rootDecision;
    static constexpr std::string_view rootContext = "u:r:su:s0";
    // KernelSU's interface, for its official Manager (ProbeKernelSu.cpp). The Manager's process is
    // given the driver when zygote makes it the Manager's user; anyone else asks for it with reboot.
    static constexpr std::string_view kernelSuPackage = "me.weishu.kernelsu";
    void kernelSuUserChanged(uint32_t uid);
    uint64_t installKernelSu();
    // Its answer, or none when it waits for the person to answer (the call is made again).
    std::optional<uint64_t> kernelSuIoctl(Machine& machine, uint32_t request, uint64_t argument);
    bool kernelSuManager() const;
    // Root's credentials for the calling process now, as su gives them at its start.
    void becomeRoot();
    // The package that has a uid, from Android's own list; "" for none. And the other way.
    std::string packageOfUid(uint32_t uid) const;
    std::optional<uint32_t> uidOfPackage(std::string_view package) const;
    // Apps the user installed do not run until the home screen is shown (the starter clears this) or
    // half a minute has passed. NESTVANE_HOLD_APPS=0 turns it off.
    std::atomic_bool appsHeld{!std::getenv("NESTVANE_HOLD_APPS") || std::string_view(std::getenv("NESTVANE_HOLD_APPS")) != "0"};
    std::chrono::steady_clock::time_point appsHeldUntil = std::chrono::steady_clock::now() + std::chrono::seconds(30);
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
        std::shared_ptr<const std::map<std::string, MountView, std::less<>>> obscuredMounts{};
        uint64_t peerGroup = 0;
        // The peer group this mount was in before it was made a slave: it still receives what is
        // mounted in that group, and passes nothing on.
        uint64_t masterGroup = 0;
    };
    // A FUSE device once opened. The one filesystem Android mounts through it is the shared storage,
    // which its media provider serves from /data/media. Here the mount is a view of that directory, and
    // the provider's daemon gets the request that opens the session and nothing after it: no file
    // operation goes through the daemon, so what it would check for each app is not checked.
    struct FuseConnection {
        std::deque<std::vector<uint8_t>> requests;
        std::string mountpoint;
        bool mounted = false, unmounted = false;
    };
    std::map<std::string, std::weak_ptr<FuseConnection>, std::less<>> fuseMounts;
    static constexpr std::string_view sharedStorageRoot = "/data/media";
    // The call names a file of the shared storage. Access to those is the media provider's to grant,
    // not a matter of the files' owners and modes: Android leaves the checks of the lower filesystem
    // out of the mount (no default_permissions).
    mutable bool sharedStorageCall = false;
    uint64_t mountFuse(const std::string& path, std::string_view options);
    struct MountNamespace {
        uint64_t id = 1;
        uint64_t rootPropagation = 0;
        uint64_t generation = 0;
        std::map<std::string, MountView, std::less<>> mounts{{"/dev", {6, 0755, 0, 0, "/dev"}}};
    };
    // `native`: the thread's native code runs on a worker, and its registers here are out of date.
    enum class Wait { none, poll, recordLock, futex, futexPi, signal, sleep, exited, suspended, signalSuspend, native };
    struct Process;
    struct WritableFile;
    using SignalWords = std::array<uint32_t, 32>;
    using PendingSignals = std::multimap<uint32_t, SignalWords>;
    struct BinderConnection;
    struct EventState;
    struct SocketState;
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
        std::weak_ptr<BinderConnection> idleBinder;
        std::weak_ptr<BinderConnection> waitingBinder;
        uint32_t waitingBinderFd = 0;
        std::weak_ptr<EventState> waitingEpoll;
        std::weak_ptr<WritableFile> waitingRecordFile;
        std::weak_ptr<uint64_t> waitingRecordDescription;
        uint64_t recordLockBlocker = 0;
        bool noNewPrivileges = false;
        Wait wait = Wait::none, signalInterruptedWait = Wait::none;
        uint64_t clearTid = 0, futexAddress = 0, futexPiOrder = 0;
        std::shared_ptr<SharedFileBytes> futexBacking;
        std::shared_ptr<Process> futexAddressSpace;
        uint32_t futexMask = 0, ioPriority = 0;
        int nice = 0;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        std::optional<std::chrono::nanoseconds> realtimeWaitDeadline;
        std::optional<std::chrono::steady_clock::time_point> hostEventRetry;
        // When this wait for a host event began, and when it was last looked at.
        std::optional<std::chrono::steady_clock::time_point> hostWaitBegan;
        std::chrono::steady_clock::time_point hostWaitLast{};
        std::optional<std::chrono::steady_clock::time_point> nextSliceAfter;
        uint64_t nativeTicket = 0;                  // names the run on a worker while wait is native
        std::optional<native::Result> nativeResult; // how that run ended, until the thread has its next turn
        bool cpuBound = false;                      // its native code used up a whole turn: worth a worker
        unsigned briefRuns = 0;                     // runs on a worker in a row that ended at once
        unsigned briefInterpreted = 0;              // the same for runs of the interpreter on a worker
        uint64_t signalMask = 0;
        uint64_t signalWaitMask = 0;
        std::optional<uint64_t> suspendSignalMask;
        std::string fsCreateContext, socketCreateContext;
        PendingSignals pendingSignals;
        std::array<uint8_t, 16> name{};
        std::array<uint64_t, 3> alternateStack{0, 2, 0};
        std::chrono::nanoseconds cpuTime{};
    };

    bool switchThread(Machine& machine, std::chrono::steady_clock::time_point deadline, const std::stop_token& stop);
    void accountGuestCpu();
    bool cloneThread(Machine& machine);
    bool forkProcess(Machine& machine);
    bool execProcess(Machine& machine);
    bool futex(Machine& machine);
    bool futexPi(Machine& machine, uint64_t op, uint64_t address,
                 std::shared_ptr<SharedFileBytes> backing, uint32_t value);
    unsigned wakeFutex(uint64_t address, unsigned count, uint32_t mask,
                       const std::shared_ptr<SharedFileBytes>& backing = {});
    void wakePollers(const EventState* changedEvent = nullptr, bool binderOnly = false,
                     const SocketState* readableSocket = nullptr);
    void wakeBinderPollers() { wakePollers(nullptr, true); }
    bool waitForEvents();
    bool waitForHostEvents();
    bool poll(Machine& machine);
    void removeSiblingThreads();
    // Takes back the finished runs of threads whose native code ran on workers.
    void collectNative(Machine& machine);
    // Ends the runs on workers of a process's threads and waits for them: before the threads go.
    void recallNativeThreads(const Process* process);
    bool exitThread(Machine& machine);
    void finishProcess(Process& process, uint32_t status, bool killed);
    // When a new process finds no room in the host's address space: ends the cached app that matters
    // least, as Linux's low-memory killer would. False when there is none to end.
    bool endCachedAppForRoom();
    bool waitChild(Machine& machine);
    Process* liveProcess(uint64_t id) const;
    bool processAlive(uint64_t id) const { return liveProcess(id) != nullptr; }
    bool installSignalFrame(Machine& machine, uint32_t signal, const SignalWords& info, uint64_t faultAddress = 0);
    static bool canCatchSignal(const Thread& thread, uint32_t signal);
    void deliverPendingCaughtSignal(Machine& machine);
    bool restoreSignalContext(Machine& machine);
    void terminateOnPendingSignal();
    uint64_t signalRestorer(Machine& machine);
    uint64_t installSeccomp(Machine& machine, uint64_t address);
    bool enforceSeccomp(Machine& machine);

    enum class Node { regular, temporary, directory, symbolicLink, input, output, error, null, entropy, kernelLog, poller, event, inotify, signal, socket, keySocket, mountNamespace, pipeRead, pipeWrite, execLabel, fsCreateLabel, socketCreateLabel, securityInfo, securityCreate, securityAccess, securityValidate, loopControl, loopDevice, timer, ptyMaster, ptySlave, ueventTrigger, kernelSetting, oomScoreAdjustment, binder, processHandle, cgroupProcs, ashmem, graphicsPipe, graphicsAddressSpace, touchDevice, bpfMap, bpfProgram, bpfBtf, pressure, fuse, sound, dmaHeap, kernelSu };
    struct RecordLock {
        uint64_t owner, start, end;
        int16_t type;
    };
    struct WritableFile {
        GuestInodeTimes times{};
        uint64_t inode;
        uint32_t mode;
        uint32_t uid, gid;
        bool persistent = false, memoryFile = false;
        uint32_t seals = 0;
        std::vector<RecordLock> locks;
        std::shared_ptr<SharedFileBytes> storage = std::make_shared<SharedFileBytes>();
        DenseBytes::Weak privateMapping;
        uint64_t privateMappingOffset = 0;
        size_t privateMappingSourceSize = 0;
        std::map<std::string, std::vector<uint8_t>, std::less<>> attributes, userAttributes;
        PosixAcl accessAcl{};
        std::shared_ptr<BpfMap> pinnedMap{};
        std::shared_ptr<const BpfProgram> pinnedProgram{};
        bool pinnedBpfObject() const { return pinnedMap || pinnedProgram; }
    };
    struct Directory;
    struct PollNotifications {
        uint64_t readable = 0;
        uint64_t writable = 0;
        uint64_t stateChange = 0;
    };
    struct Descriptor;
    using SocketRights = std::vector<std::shared_ptr<Descriptor>>;
    struct SocketState {
        uint32_t type;
        uint32_t family = 1, protocol = 0;
        uint64_t cookie = 0;
        uint32_t socketMark = 0;
        int inetError = 0;
        bool tcpConnectFailed = false;
        std::shared_ptr<uint32_t> tcpConnectionMark;
        std::shared_ptr<void> storageIdentity;
        std::string inetCgroup;
        std::vector<std::weak_ptr<Directory>> inetAncestry;
        using InetAddress = std::array<uint8_t, 28>;
        InetAddress localInetAddress{}, peerInetAddress{};
        uint32_t tcpCallbackFlags = 0, tcpSequence = 0;
        bool inetV6Only = false;
        bool inetStream() const { return (family == 2 || family == 10) && protocol == 6; }
        struct Datagram {
            InetAddress source;
            std::vector<uint8_t> payload;
        };
        std::deque<Datagram> datagrams;
        // A host socket that carries this one to the network (HostSocket.hpp), or -1 while the socket
        // only talks to the guest's own loopback.
        int hostFd = -1;
        bool hostConnecting = false, hostConnectReported = false;
        // UDP, or the echo socket ping uses: ICMP over a datagram socket.
        bool inetDatagram() const { return protocol == 17 || icmp(); }
        bool icmp() const { return family == 2 && protocol == 1; }
        uint16_t icmpIdentifier = 0;  // of the last echo request sent: replies to others are not ours
        bool firewallSocket() const { return type == 3 && protocol == 255; }
        bool hasInput() const { return !inbox.empty() || !datagrams.empty(); }
        std::function<void(const SocketState&)> onClose;
        ~SocketState() {
            if (inetStream() && connected && linger[0] && !linger[1]) {
                if (const auto remote = peer.lock()) {
                    remote->inetError = 104; // ECONNRESET after previously delivered data.
                    ++remote->pollNotifications.stateChange;
                }
            }
            if (family == 1 && type != 2) {
                if (const auto remote = peer.lock()) ++remote->pollNotifications.stateChange;
            }
            if (onClose) onClose(*this);
            if (hostFd >= 0) hostnet::closeSocket(hostFd);
        }
        PollNotifications pollNotifications;
        std::array<uint32_t, 3> peerCredentials{0, UINT32_MAX, UINT32_MAX};
        std::string context, peerContext;
        std::string localAddress, peerAddress;
        std::array<uint32_t, 3> credentials{0, UINT32_MAX, UINT32_MAX};
        std::weak_ptr<SocketState> peer;
        struct Packet {
            std::vector<uint8_t> payload;
            std::optional<std::array<uint32_t, 3>> credentials{};
            SocketRights files{};
        };
        std::deque<Packet> inbox;
        struct RightsMessage {
            uint64_t firstByte, endByte;
            SocketRights files;
        };
        std::deque<RightsMessage> rights;
        uint64_t streamOffset = 0;
        std::deque<std::shared_ptr<SocketState>> pending;
        size_t backlog = 0;
        size_t queuedBytes = 0;
        uint32_t sendBuffer = 262144, receiveBuffer = 212992;
        // A send to this socket found its queue too full: the next read has a writer to wake.
        bool writerWaiting = false;
        std::array<int32_t, 2> linger{}; // Linux struct linger: enabled, seconds.
        std::chrono::microseconds receiveTimeout{}, sendTimeout{};
        bool connected = false;
        bool listening = false;
        bool receiveShutdown = false, sendShutdown = false;
        bool passCredentials = false, reuseAddress = false;
        bool logWriter = false;
        struct NetlinkSocket {
            uint32_t pid = 0, groups = 0, receiveBuffer = 212992;
            bool timestamp = false;
        };
        std::optional<NetlinkSocket> netlink;
        std::shared_ptr<const BpfProgram> socketFilter;
        explicit SocketState(uint32_t type) : type(type) {}
    };
    bool waitForSocketTimeout(std::chrono::microseconds timeout);
    struct GraphicsPipeState {
        std::string serviceHeader;
        std::unique_ptr<GraphicsPipe> backend;
        std::vector<uint8_t> readBuffer;
        size_t readOffset = 0, readSize = 0;
        uint64_t received = 0, sent = 0, readAgain = 0, writeAgain = 0;
        int64_t lastRead = 0, lastWrite = 0;
        std::string_view serviceContext;
        bool failed = false, vsock = false, connected = false, renderChannel = false;

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
    std::shared_ptr<SocketState> findInetDestination(uint32_t protocol, const SocketState::InetAddress& address) const;
    bool filterInetAddress(const SocketState& socket, uint32_t type, SocketState::InetAddress& address,
        bool* skipBindCapability = nullptr);
    uint64_t connectInetSocket(Machine& machine, const std::shared_ptr<SocketState>& socket, bool nonblocking);
    // Connects a socket to an address out on the network, through a host socket.
    uint64_t connectHostSocket(const std::shared_ptr<SocketState>& socket, const SocketState::InetAddress& destination, bool nonblocking);
    // What a socket that a host socket carries can do now, in poll's bits; a connection that was being
    // made is finished here.
    int hostSocketEvents(SocketState& socket);
    uint64_t connectInetStream(const std::shared_ptr<SocketState>& socket,
        const SocketState::InetAddress& destination, bool nonblocking);
    uint64_t resolveFirewallBpf(std::string_view path, int32_t fd, std::shared_ptr<const BpfProgram>& program);
    bool matchFirewallBpf(const BpfProgram& program, GuestFirewall::Hook hook, BpfProgram::Packet& packet);
    bool transmitInetStream(SocketState& from, SocketState& to, std::span<const uint8_t> payload,
        uint32_t flags, GuestFirewall::Rejection& rejection);
    std::shared_ptr<void> lookupInetSocket(std::span<const uint8_t> tuple, bool tcp,
        const SocketState* receiving = nullptr) const;
    uint64_t sendInetDatagram(Machine& machine, const std::shared_ptr<SocketState>& socket,
        std::span<const std::array<uint64_t, 2>> vectors, uint64_t address = 0, uint64_t addressLength = 0);
    std::optional<uint64_t> pullHostDatagram(SocketState& socket);
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
    GuestFirewall firewall;
    struct DatagramConnection {
        bool sawReply = false, confirmed = false;
        std::weak_ptr<SocketState> first, second;
        std::chrono::steady_clock::time_point expires;
        uint32_t mark = 0;
    };
    std::map<std::pair<uint64_t, uint64_t>, DatagramConnection> datagramConnections;
    DatagramConnection* trackDatagramConnection(const std::shared_ptr<SocketState>& first,
        const std::shared_ptr<SocketState>& second);
    std::shared_ptr<CgroupSocketHooks> inetReleasePrograms = std::make_shared<CgroupSocketHooks>();
    uint64_t bpfMapCall(Machine& machine);
    uint64_t bpfHashBatch(Machine& machine, std::span<const uint8_t> input, bool remove);
    uint64_t loadBpfProgram(Machine& machine);
    uint64_t testBpfProgram(Machine& machine);
    uint64_t bpfObjectIdCall(Machine& machine);
    uint64_t loadBpfTypes(Machine& machine);
    uint64_t bpfObjectInfo(Machine& machine);
    uint64_t createBpfToken(Machine& machine);
    std::vector<std::weak_ptr<const BpfTypes>> bpfTypes;
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
    std::shared_ptr<GuestColorDma> graphicsDma;
    uint64_t nextNativeTicket = 1;
    std::unordered_map<uint64_t, uint64_t> nativeTickets;  // run on a worker -> thread
    void serviceGraphicsDma();
    bool transferGraphicsDma(GuestColorDma::Transfer& transfer);
    std::shared_ptr<GraphicsRegion> allocateGraphicsRegion(uint64_t requested);
    uint64_t graphicsAddressIoctl(Machine& machine, const Descriptor& fd);
    GraphicsPipeFactory graphicsPipeFactory;
    std::shared_ptr<GuestCameraSource> camera;
    std::shared_ptr<GuestApkInbox> apkInbox;
    std::span<const uint8_t> tetheringLoader;
    bool writeCameraFrame(uint64_t offset, GuestCameraSource::Format format);
    std::shared_ptr<GuestTouch> guestTouch;
    std::weak_ptr<GuestTouch::Reader> guestTouchReader;
    uint64_t touchIoctl(Machine& machine, const Descriptor& fd);
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
        std::optional<PollNotifications> deliveredNotifications{};
        uint64_t pipeWritesSeen = ~uint64_t{0};  // Pipe::writes when this watch last reported, if it reports only changes
        // Edge-triggered on a socket the host carries: what was reported and is still ready, and the socket's
        // counts of reads and writes that found nothing to do when this watch last looked. A direction is
        // reported again once it was drained (a read or write that would block) or stopped being ready.
        uint32_t hostEdgeReported = 0;
        PollNotifications hostEdgeDrains{};
    };
    struct EventState {
        struct Timer {
            std::optional<std::chrono::nanoseconds> next;
            std::chrono::nanoseconds interval{};
            bool wakeAlarm = false, realtime = false, absoluteRealtime = false;
            bool cancelOnSet = false, cancelled = false;
        };
        std::optional<Timer> timer;
        PollNotifications pollNotifications;
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
    // Sends SIGALRM for the interval timers that are due, and brings |next| forward to the earliest one that is not.
    void fireAlarms(std::chrono::steady_clock::time_point now, std::optional<std::chrono::steady_clock::time_point>& next);
    struct Pipe {
        std::deque<uint8_t> data;
        std::weak_ptr<int> reader, writer;
        size_t capacity = 65536;
        uint64_t writes = 0;  // goes up with each write, for watches that report only changes
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
        uint64_t strong = 0, weak = 0;
        bool ownerStrong = false, ownerWeak = false;
        bool pendingStrong = false, pendingWeak = false;
    };
    uint64_t nextBinderCall = 0;
    unsigned binderStaleReplies = 0;
    using BinderReferenceChanges = std::set<std::weak_ptr<BinderNode>, std::owner_less<>>;
    struct BinderReference {
        std::shared_ptr<BinderNode> node;
        uint32_t inBuffer = 0, strong = 0, weak = 0;
    };
    struct BinderAllocation {
        size_t size;
        std::vector<uint32_t> handles;
        std::vector<std::shared_ptr<BinderNode>> localReferences;
        bool delivered = false;
        size_t asyncCharge = 0;
        uint64_t asyncTarget = 0;
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
        std::shared_ptr<const BinderRoute> parent;
        uint64_t call = 0;  // the synchronous call this route answers (PendingCall::call)
    };
    struct BinderFileTransfer {
        size_t offset;
        std::shared_ptr<Descriptor> file;
        // One of an array of descriptors inside a buffer: a 32-bit number, where a file object has 64 bits.
        bool narrow = false;
    };
    struct BinderPacket {
        uint32_t command;
        BinderTransaction transaction{};
        uint64_t securityContext = 0;
        std::optional<BinderRoute> reply{};
        std::array<uint64_t, 2> reference{};
        std::vector<BinderFileTransfer> files{};
        bool completesCall = false;
        uint64_t answers = 0;  // for a reply: the call it answers (PendingCall::call)
        uint64_t sender = 0;   // for a reply: the process it came from, for the log
    };
    struct BinderDeath {
        std::weak_ptr<BinderConnection> target;
        uint64_t cookie, thread;
        bool sent = false, acknowledged = false, cleared = false;
    };
    static constexpr size_t binderMappingLimit = 4 * 1024 * 1024;
    struct BinderConnection {
        uint64_t owner;
        uint32_t maxThreads = 0;
        bool looperRequested = false;
        std::shared_ptr<SharedFileBytes> mapped{};
        uint64_t address = 0, target = 0, cookie = 0;
        bool sendSecurityContext = false, managerAcceptsFiles = true;
        size_t asyncBytes = 0;
        std::map<uint64_t, BinderAllocation> allocations;
        std::map<uint32_t, BinderReference> handles;
        std::map<uint32_t, BinderDeath> deaths;
        std::map<uint64_t, std::shared_ptr<BinderNode>> localNodes;
        uint32_t nextHandle = 1;
        std::map<uint64_t, std::deque<BinderPacket>> directed;
        std::map<uint64_t, std::vector<BinderRoute>> replies;
        struct PendingCall {
            std::weak_ptr<BinderConnection> target;
            uint64_t pid;
            uint32_t code;
            std::string descriptor;
            uint64_t call = 0;  // which call: a reply for another one is not the one this thread waits for
        };
        std::map<uint64_t, std::vector<PendingCall>> pendingCalls;
        std::deque<BinderPacket> incoming;
        std::map<uint64_t, std::deque<BinderPacket>> pendingAsync; // Present while this node has a live transaction.
        std::map<uint64_t, bool> loopers; // Whether the looper was requested by the driver.
        BinderConnection(uint64_t owner, const std::shared_ptr<bool>& deathsDirty,
                         const std::shared_ptr<BinderReferenceChanges>& referenceChanges)
            : owner(owner), deathsDirty(deathsDirty), referenceChanges(referenceChanges) {}
        ~BinderConnection();
        void changeReference(const std::shared_ptr<BinderNode>& node, int64_t strong, int64_t weak);
        std::weak_ptr<bool> deathsDirty;
        std::weak_ptr<BinderReferenceChanges> referenceChanges;
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
        std::shared_ptr<int> pipeOtherEnd{}; // the writing end too, when a named pipe is open for both
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
        std::shared_ptr<GuestApkInbox::Request> apkRequest{};
        uint64_t apkInstaller = 0;
        std::shared_ptr<FuseConnection> fuse{};
        std::span<const uint8_t> contents() const { return ext4File ? ext4File->bytes() : bytes; }
        uint64_t contentSize() const { return ext4File ? ext4File->size : bytes.size(); }
        static constexpr uint32_t mutableStatus = 0xc00; // O_APPEND and O_NONBLOCK
        std::shared_ptr<uint32_t> status = std::make_shared<uint32_t>(flags & mutableStatus);
        uint32_t effectiveFlags() const { return (flags & ~mutableStatus) | *status; }
        bool pathOnly() const { return flags & 0x200000; }
    };
    std::optional<std::string> resolve(std::string_view path) const;
    std::string absolutePath(std::string_view path) const;
    uint64_t resolveAtPath(uint32_t directory, std::string_view path, std::string& result) const;
    uint64_t changeWorkingDirectory(std::string path);
    std::optional<std::string> followLinks(std::string path, bool followFinal = true, uint64_t* lookupError = nullptr) const;
    uint64_t writeDescriptor(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size,
                             std::optional<uint64_t> position = {});
    uint64_t open(const Machine& machine, const std::string& path, uint32_t flags, uint32_t mode);
    bool lockWholeFile(Machine& machine, const Descriptor& file);
    struct WholeFileLock {
        std::weak_ptr<uint64_t> owner;
        bool exclusive;
    };
    std::map<std::pair<uint64_t, std::string>, std::vector<WholeFileLock>> wholeFileLocks;
    uint64_t stat(Machine& machine, uint64_t address, const Descriptor& fd, bool extended = false) const;
    void notePrelink(Machine& machine, const Descriptor& fd, uint64_t status);
    // Page-aligned copies of read-only files whose bytes are not page-aligned where they are kept.
    std::map<const uint8_t*, std::span<const uint8_t>> alignedFiles;
    uint64_t statx(Machine& machine) const;
    uint64_t resize(const WritableFile& file, uint64_t size);
    uint64_t readGraphics(Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size);
    uint64_t writeGraphics(const Machine& machine, GraphicsPipeState& pipe, uint64_t address, uint64_t size);
    uint64_t ashmemIoctl(Machine& machine, const Descriptor& fd);
    bool dataPath(std::string_view path) const;
    std::string newFileContext(std::string_view path) const;
    bool validAppDataContext(std::string_view label) const;
    bool validAppProcessContext(std::string_view label) const;
    bool canSpecializeApp(std::string_view label) const;
    bool ownerMayChown(uint32_t owner, uint32_t uid, uint32_t gid) const;
    bool canAccessParent(std::string_view path, bool modify, bool realIds = false) const;
    const Directory* parentDirectory(std::string_view path) const;
    PosixAcl inheritAcl(const Directory* parent, uint32_t& mode) const;
    bool inodeAllows(const auto& node, unsigned requested, bool realIds = false) const;
    uint64_t changePosixAcl(Machine& machine, const Descriptor& entry, bool defaults, bool removing);
    uint64_t chmodDirectory(Directory& directory, uint32_t mode) const;
    uint64_t changeUserAttribute(Machine& machine, const Descriptor& entry,
                                          const std::string& name, bool removing);
    uint64_t renameFile(Machine& machine);
    uint64_t renameDirectory(const std::string& source, const std::string& destination, bool noReplace);
    sqlite3* dataDatabase = nullptr;
    // Where the content of the guest's large files is kept, one host file per inode, beside the
    // database that has everything else of /data. A large file is mapped from there when something
    // reads it, so it takes no memory of its own and the database does not carry it. The database is
    // a snapshot taken when the guest syncs; the content of a large file is whatever was last written.
    // After the app is killed a large file can so be newer than the snapshot, never older.
    std::string dataFilesDirectory;
    static constexpr uint64_t externalFileBytes = 1024 * 1024;
    std::string externalFile(uint64_t inode) const { return dataFilesDirectory + "/" + std::to_string(inode); }
    // A host directory that is the guest's shared storage (/data/media/0, which apps see as
    // /storage/emulated/0): every file and directory there is one of the host's, under its own name
    // and exactly as long, so that the host's other apps can use them and put files there. The
    // database keeps only who owns each. What the host has when the data is mounted is what the
    // guest gets: files added from outside appear, files removed are gone. Set before mountData.
    std::string sharedHostRoot;
    static constexpr std::string_view sharedGuestRoot = "/data/media/0";
    // The host path of a guest path in the shared storage, or nothing.
    std::string sharedHost(std::string_view backing) const;
    void sharedFileCreated(const std::string& backing, WritableFile& file);
    void sharedFileRemoved(const std::string& backing);
    std::map<std::string, std::pair<int64_t, int64_t>, std::less<>> sharedListedTimes;
    std::chrono::steady_clock::time_point sharedScannedAt{};
    // After a file or a directory tree moved: the host's files follow, into the shared storage, out of it, or within.
    void sharedMoved(const std::string& source, const std::string& destination);
    // The database still has the content of files that have moved out of it: it is packed after the next sync.
    bool compactData = false;
    int64_t dataGeneration = 0;
    static constexpr uint64_t maxDataFileBytes = 1024ULL * 1024 * 1024;
    // Transient filesystem writes (tmpfs and the like, kept in memory) have a fixed budget. Persistent data
    // has the host's: Android sees the volume the app is on, its size and what is free on it for the app
    // (less a reserve, so that Android never fills the device).
    static constexpr uint64_t maxTransientBytes = 8192ULL * 1024 * 1024;
    static constexpr uint64_t hostReserveBytes = 1024ULL * 1024 * 1024;
public:
    struct HostVolume { uint64_t total = 0, available = 0, generation = 0; };
    // Where the volume's numbers come from. By default statfs of $HOME; the iOS app sets the one that
    // asks for the space available for important use, the number the device's Settings shows.
    static void setHostVolumeSource(HostVolume (*source)());
    static HostVolume hostVolume();
private:
    // What the persistent data may grow to: what it uses now and what is free on the host.
    uint64_t persistentLimit(uint64_t used) const;
    mutable uint64_t volumeGeneration = 0, volumeUsed = 0;
    bool rootOn = false;
    // An app the person installed, which waits while Android starts (appsHeld).
    bool heldApp(const Thread& thread) const;
    bool appsHeldNow() const;
    // How many files and directories the guest's data may hold. With some thirty apps installed the
    // earlier limits (4096 files, 2048 directories) were reached, and Android could start no new app:
    // zygote could not make the app's profile directory and aborted.
    static constexpr size_t maxPersistentFiles = 65536;
    static constexpr size_t maxTransientFiles = 8192;
    static constexpr size_t maxGuestDirectories = 32768;
    static constexpr size_t maxGuestLinks = 8192;
    static constexpr size_t maxDataRecords = maxPersistentFiles + maxGuestDirectories + maxGuestLinks;
    std::vector<std::weak_ptr<SharedFileBytes>> memoryFiles;
    std::optional<Descriptor> find(std::string_view path, uint32_t flags, const Machine* machine = nullptr) const;
    uint64_t freeMemory(const Machine& machine) const;
    void markFileModified(WritableFile& file) const;
    uint64_t writeFileBytes(WritableFile& file, uint64_t offset, std::span<const uint8_t> bytes);
    std::string backingPath(std::string_view path) const;
    static std::string backingPath(std::string_view path, const MountNamespace& mountNamespace);
    const MountView* mountFor(std::string_view path) const;
    void setMountPropagation(MountView& mount, uint64_t propagation);
    uint64_t bindMount(std::string_view from, const std::string& to, bool recursive, std::string_view device = {});
    uint64_t mountFilesystem(Machine& machine);
    uint64_t mountBinder(const std::string& path, uint64_t flags, std::string_view options);
    uint64_t binderIoctl(Machine& machine, const Descriptor& fd);
    uint64_t binderCommand(Machine& machine, const Descriptor& fd, uint32_t command,
                           std::span<const uint8_t> data);
    uint64_t binderWriteRead(Machine& machine, const Descriptor& fd);
    static bool binderAvailableForProcessWork(const BinderConnection& connection, uint64_t tid);
    bool binderReadable(const BinderConnection& connection, uint64_t pid, uint64_t tid) const;
    bool binderCallLostPeer(const BinderConnection& connection, uint64_t tid) const;
    bool freeBinderBuffer(BinderConnection& connection, uint64_t address, bool delivered);
    uint64_t storeBinderBuffer(BinderConnection& target, std::span<const uint8_t> data,
                              std::span<const uint8_t> offsets, std::span<const uint8_t> extra,
                              std::string_view context, bool asynchronous);
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
    const Thread* visibleProcThread(uint64_t tid) const;
    uint64_t procThreadId(std::string_view task) const;
    std::string procCommandLine(const Process& process, const Machine* machine) const;
    const Process* procTaskDirectory(std::string_view path) const;
    static std::string mountList(const MountNamespace& group);
    uint64_t writableAllocation(bool persistent) const;
    uint64_t statFilesystem(Machine& machine, uint64_t address, std::string_view path) const;
    static constexpr uint32_t descriptorCapacity = 512 * 1024, defaultDescriptorLimit = 1024;
    static constexpr uint32_t resourceObjectCapacity = 4096;
    uint32_t descriptorLimit() const;
    uint32_t freeDescriptor(uint32_t minimum = 0) const;
    uint64_t recordLock(Machine& machine, const Descriptor& fd);
    void wakeRecordLockWaiters(const WritableFile& file);
    // descriptionUsers: how many descriptors of the open file description go now (close: this one).
    void releaseRecordLocks(const Descriptor& fd, uint64_t owner, long descriptionUsers = 1);
    static constexpr uint64_t descriptionLockBit = uint64_t{1} << 63;
    static uint64_t descriptionLockOwner(const Descriptor& fd) { return descriptionLockBit | reinterpret_cast<uintptr_t>(fd.offset.get()); }
    uint64_t readDirectory(Machine& machine, const Descriptor& fd) const;
    uint64_t socketRead(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size,
                        bool reportPacketSize = false, bool peek = false);
    using SocketVector = std::array<uint64_t, 2>; // Guest iovec: pointer and length.
    uint64_t socketWrite(Machine& machine, const Descriptor& fd, uint64_t address, uint64_t size);
    uint64_t socketWrite(Machine& machine, const Descriptor& fd, std::span<const SocketVector> vectors,
                         SocketRights rights = {});
    // A datagram to a named UNIX socket from one that is not connected to it (sendto or sendmsg with an
    // address): the zygotes tell system_server of a child that ended this way.
    uint64_t sendUnixDatagramTo(Machine& machine, const Descriptor& fd, std::span<const SocketVector> vectors,
                                uint64_t name, uint64_t nameLength, SocketRights rights = {});
    void consumeSocketStream(SocketState& endpoint, size_t count);
    // After the guest read from a socket's queue: wakes waiting threads only if a writer could have
    // been held up by a full queue before (Linux's writability: the queue under the writer's send buffer
    // and fewer than 256 packets). Each read used to wake every waiting thread.
    void socketDrained(SocketState& endpoint, uint64_t queuedBefore, size_t packetsBefore);
    uint64_t readSocketRights(const Machine& machine, uint64_t control, uint64_t length, SocketRights& rights) const;
    bool writeSocketRights(Machine& machine, const SocketRights& rights, uint64_t control,
                           uint64_t capacity, bool closeOnExec, uint64_t& used, uint32_t& flags,
                           std::map<uint32_t, Descriptor>& transferred);
    uint64_t receiveUnixStreamMessage(Machine& machine, SocketState& endpoint);
    bool socketCall(Machine& machine);
    uint64_t receiveKernelNetlink(Machine& machine, SocketState& endpoint);
    uint64_t receiveKernelNetlinkFrom(Machine& machine, SocketState& endpoint);
    uint64_t sendKernelNetlink(Machine& machine, const std::shared_ptr<SocketState>& endpoint,
        std::span<const std::array<uint64_t, 2>> vectors, uint64_t address = 0, uint64_t length = 0);
    uint64_t bindNetlink(const std::shared_ptr<SocketState>& endpoint, uint32_t pid, uint32_t groups);
    void emitNetfilterLog(const GuestFirewall::LogRecord& record, const SocketState& sender);
    void queueNetlink(SocketState& endpoint, std::vector<uint8_t> packet);
    struct NetfilterLogGroup {
        std::weak_ptr<SocketState> owner;
        uint32_t copyRange = 65531;
        uint8_t mode = 2;
    };
    std::map<uint16_t, NetfilterLogGroup> netfilterLogGroups;
    GuestRoute guestRoutes;
    std::map<std::pair<uint32_t, uint32_t>, std::weak_ptr<SocketState>> netlinkPorts;
    uint32_t nextNetlinkPort = 0x80000000;
    void emitLoopUevent(unsigned number);
    std::vector<std::weak_ptr<SocketState>> ueventListeners;
    std::map<std::string, unsigned, std::less<>> ueventTriggers;
    uint64_t ueventSequence = 0;
    bool eventCall(Machine& machine);
    bool fileCall(Machine& machine);
    uint64_t makePipe(Machine& machine);
    bool securityCall(Machine& machine);
    uint64_t updateTimes(Machine& machine);
    uint64_t map(Machine& machine);
    uint64_t randomMappingBase() const;
    struct Setting { unsigned value, minimum, maximum; };
    std::array<int32_t, 2> guestTimezone{};
    // The guest owns its wall clock and starts at the host's current wall time.
    std::chrono::nanoseconds realtimeOffset =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()) -
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
    std::chrono::nanoseconds realtimeAt(std::chrono::steady_clock::time_point now) const;
    void setRealtime(std::chrono::nanoseconds value);
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
    // Each image file's position in `files`, which is its inode number: counting the map's entries up
    // to it for each stat walked tens of thousands of them. Made again after `files` changes.
    mutable std::unordered_map<std::string_view, uint64_t> fileInodes;
    mutable bool fileInodesValid = false;
    uint64_t fileInode(std::map<std::string, std::span<const uint8_t>, std::less<>>::const_iterator file) const;
    struct Link {
        std::string target;
        std::string securityContext;
    };
    std::map<std::string, Link, std::less<>> links;
    struct Process {
        bool expeditedMembarrier = false, dumpable = true;
        uint64_t id;
        uint64_t parent = 0;
        std::string executable;
        std::string workingDirectory = "/";
        std::string programName;
        std::string commandLine;
        uint64_t argvAddress = 0;
        std::string context, pendingContext;
        std::string cgroup;
        uint64_t group = 0, session = 0;
        uint32_t umask = 0022, secureBits = 0;
        uint8_t mdwe = 0;
        // The real-time interval timer (setitimer, which alarm is made of): when SIGALRM is due
        // next, and how long after that again.
        std::optional<std::chrono::steady_clock::time_point> alarmAt;
        std::chrono::microseconds alarmEvery{};
        int oomScoreAdjustment = 0, minimumOomScoreAdjustment = 0;
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
        std::shared_ptr<GuestSpace> space;  // held here while another process runs, like the segments
        std::array<std::array<uint64_t, 4>, 65> signalActions{};
        PendingSignals pendingSignals;
        uint64_t signalTrampoline = 0;
        struct ChildSignal { uint32_t pid, uid, status, code; };
        std::optional<ChildSignal> childSignal;
        std::chrono::nanoseconds cpuTime{}, reapedCpuTime{};
    };
    auto& descriptors() { return current->process->descriptors; }
    const auto& descriptors() const { return current->process->descriptors; }
    struct BoundSocket {
        std::weak_ptr<SocketState> endpoint;
        uint32_t mode, uid, gid;
    };
    std::map<std::string, BoundSocket, std::less<>> socketPaths;
    // Named pipes (mkfifo), by path. They live as long as this run: nothing writes them to storage.
    struct NamedPipe {
        std::shared_ptr<Pipe> pipe;
        uint32_t mode, uid, gid;
    };
    std::map<std::string, NamedPipe, std::less<>> fifoPaths;
    std::map<std::string, std::shared_ptr<WritableFile>, std::less<>> writableFiles;
    struct Directory {
        // Stored with the directory across rename, mount aliases and guest restarts.
        uint32_t mode, uid = 0, gid = 0;
        std::string securityContext{};
        std::optional<uint8_t> restoreconInProgress{};
        GuestInodeTimes times{};
        static constexpr size_t maxUserAttributes = 8, maxUserAttributeValue = 4096;
        std::map<std::string, std::vector<uint8_t>, std::less<>> userAttributes{};
        PosixAcl accessAcl{}, defaultAcl{};
    };
    struct BinderDevice : Directory {
        uint32_t minor;
        std::weak_ptr<BinderConnection> manager{};
    };
    // Connection lifetime and guest process changes are handled by the guest scheduler.
    std::shared_ptr<bool> binderDeathsDirty = std::make_shared<bool>(false);
    std::vector<std::weak_ptr<BinderConnection>> binderDeathSubscribers;
    void refreshBinderDeaths();
    std::shared_ptr<BinderReferenceChanges> binderReferenceChanges = std::make_shared<BinderReferenceChanges>();
    void refreshBinderReferences();
    std::string binderTrace;
    unsigned binderCapacityReports = 0;
    std::map<std::string, std::string, std::less<>> bootFileLabels;
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
    std::set<std::string, std::less<>> appDataLabelTypes, appTransitionDomains;
    struct AppLabelRule {
        std::string user;
        bool appCategories, userCategories;
    };
    std::map<std::string, std::vector<AppLabelRule>, std::less<>> appLabelRules;
    std::map<std::string, std::string, std::less<>> kernelFiles;
    std::string ashmemBootPath;
    struct AccessClass {
        uint32_t mask = 0;
        bool applicationService = false;
        // What an app's process may do with its own keys in the keystore; empty for every other class.
        std::string applicationKeys;
        std::map<std::pair<std::string, std::string>, std::string> decisions;
    };
    std::map<uint32_t, AccessClass> accessClasses;
    uint32_t fsCreateClass = 0, fsCreatePermission = 0, socketCreatePermission = 0;
    struct PackageRelabelClass { uint32_t index = 0, from = 0, to = 0; };
    std::array<PackageRelabelClass, 2> packageRelabelClasses{};
    bool canRelabelPackage(const Descriptor& entry, std::string_view label) const;
    bool loadAccessClass(std::string_view manifest, std::string_view name,
                         std::span<const std::string_view> permissions);
    uint64_t nextNamespace = 2;
    uint64_t nextMountBacking = 1;
    uint64_t nextMountPeerGroup = 1;
    uint64_t nextInode = 65536;
    uint64_t nextFutexPiOrder = 1;
    uint32_t userId() const { return current->process->userIds[1]; }
    uint32_t groupId() const { return current->process->groupIds[1]; }
    unsigned permissionShift(uint32_t uid, uint32_t gid, bool realIds = false) const;
    uint64_t pid, memoryLimit;
    std::shared_ptr<AnonymousBudget> anonymousBudget;
    bool nativeMemory = false;
    struct ChildExit {
        uint64_t parent;
        uint32_t status, uid, code;
        std::chrono::nanoseconds cpuTime;
    };
    std::map<uint64_t, ChildExit> childExits;
    std::map<uint64_t, Thread> threads;
    std::map<uint64_t, Process*> processes; // The live threads own each process.
    std::map<std::string, uint64_t, std::less<>> propertyPollers;
    Thread* current;
    uint64_t currentTid, nextTid;
    bool imageReplaced = false;
    bool signalContextRestored = false;
    uint64_t sliceEnd = UINT64_MAX;
    std::optional<std::chrono::nanoseconds> hostCpuSample;
public:
    // Added after every member 1.11 had, so that those keep their places in memory (1.12's start was
    // slower with them in the middle): the KernelSU state and the answers the Manager reads.
    std::function<std::vector<std::pair<std::string, bool>>()> rootAnswers;
    std::function<void(const std::string& package, bool allowed)> rootRemember;
    int64_t kernelSuManagerUid = -1;  // the uid whose APK carries the Manager's certificate
    std::map<std::string, bool, std::less<>> kernelSuSignatures;  // APK path -> whether it is the Manager's
};

} // namespace nestvane
