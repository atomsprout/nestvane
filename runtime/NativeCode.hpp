#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace nestvane {

struct Machine;
struct CPU;
struct SpaceCode;

// Guest code that was translated ahead of time into host code (tools/build-native-code.py) and is
// linked into this program. A translation is for one load address, so the kernel layer loads each
// translated file there, and code that is not translated, or not where it belongs, is interpreted.
namespace native {

struct Module {
    uint64_t key, size;    // of the ELF file: contentKey of its bytes, and its length
    const uint64_t* info;  // text start, text end, entry, table of host code per instruction, starts of stubbed instructions,
                           // code start, entry trap, where the stubs begin, their count
    uint64_t base;         // the load address the translation is for
    const char* name;
};

// Whether guest code runs natively: translations are linked in and NESTVANE_NATIVE is set.
bool enabled();
// Why translations that are linked in and wanted cannot be used, or null.
const char* failure();
// The guest address where the kernel layer puts the page of its signal return trampoline in a process
// whose code runs natively, or 0. A handler's return looks its return address up in the table of
// translated code, which has an entry for this address: the trampoline starts 4 bytes into the page.
uint64_t signalReturnPage();
// Guest addresses set aside for code that has no translation, such as the libraries of an app the user
// installs. The table of translated code spans them without any entries, so a jump from native code to
// such code faults at the look-up and goes on in the interpreter, and the rest of the process stays
// native. The kernel layer puts the dynamic linker's reservations here.
inline constexpr uint64_t foreignLow = 0x1'0000'0000, foreignHigh = 0x1'4000'0000;
// Where Android System WebView's library goes. The zygote reserves 1 GiB for it once (WebViewLibraryLoader)
// and every app loads it there. The kernel layer puts that reservation here, just above the addresses
// for code without a translation, and the table of translated code spans it too: the library's code
// runs in place in the interpreter while the rest of the process stays native.
inline constexpr uint64_t webViewBase = foreignHigh, webViewSize = 0x4000'0000;
// Whether the table spans these guest addresses: native code may jump there without harm.
bool covers(uint64_t start, uint64_t end);
uint64_t contentKey(std::span<const uint8_t> file);
// A translation of an ELF file, or null. Files with the same bytes can exist under several paths and
// be loaded together, so there may be several translations, each for an address of its own: `copy`
// counts them from 0.
const Module* moduleFor(std::span<const uint8_t> file, unsigned copy = 0);
// The module, if mapping `file` from `offset` at `address` puts its code where it was translated for.
const Module* mappedCode(std::span<const uint8_t> file, uint64_t address, uint64_t offset);
// Makes a module's code reachable: maps its table into the table of all translated code. Until then
// a jump to it from native code faults in the look-up and is interpreted. False if it cannot be mapped.
bool activate(const Module& module);
// Makes native code that jumps to `pc` leave native execution there, so that the caller sees the
// guest arrive. `pc` is the entry point of an active module that is a program.
void trap(uint64_t pc);

enum class Exit {
    syscall,    // at an svc, which the caller carries out
    fault,      // at an instruction that cannot run natively as things are: the caller interprets it
    preempted,  // the time slice ended, between two instructions
    interpret,  // the instruction at pc is not entered natively
    retry,      // memory that had no host pages yet has them now: the instruction at pc runs natively again
    lost,       // control left translated code: machine.fault has the details
};
// Runs the machine's current thread natively from cpu.pc, which is in translated code that is mapped
// in machine.space. Returns with the registers in machine.cpu and pc at a guest instruction.
Exit run(Machine& machine);
// How native code left, for a run on another host thread whose registers the main loop takes back later.
struct Result {
    int reason = 0, signal = 0;
    uint64_t pc = 0, faultPc = 0, faultAddress = 0, preemptedPc = 0;
    uint64_t ran = 0;  // mach time in guest code
    bool brief = false; // it left again within a fraction of a millisecond
    uint64_t interpreted = 0; // instructions a worker interpreted (offloadInterpreter)
    bool interpreter = false; // a run of the interpreter on a worker
    int faultAccess = 0;      // of a SIGBUS for a page that is not there: 1 a read, 2 a write (repairFilePage)
};
// A fault at a page of a mapped host file that the mapping allows to be touched: the file could not
// give the page, because it was cut short or replaced from outside the app while mapped (a file of
// the shared storage, changed in the Files app). The page becomes zeros, in memory only, and the
// access can be made again. `access` is 1 for a read and 2 for a write of a page that was not there,
// 0 for any other fault, which is never repaired. False when the address is no such page. Safe in
// a signal handler.
bool repairFilePage(uint64_t address, int access) noexcept;
// How many pages so far, and the host address of the last.
uint64_t repairedFilePages(uint64_t* last = nullptr) noexcept;
// Installs a handler that does this for the whole process, where the one of translated code is not there.
void catchFileFaults();
// Host threads that run guest threads' native code beside the main loop, which goes on with other
// threads meanwhile.
void startWorkers(unsigned count);
// Called on a worker when its run has ended, for a main loop that waits for something else. Null for none.
void setFinishedHook(std::function<void()> finished);
// Returns when a run has ended and is not taken back, or at the deadline.
void waitFinished(std::chrono::steady_clock::time_point deadline);
unsigned workers();
// Runs handed over and not taken back yet.
unsigned outstanding();
// Hands the machine's current thread to a free worker, which runs it natively from cpu.pc. False if
// there is none, or the instruction at pc is not entered natively. `ticket` names the run.
bool offload(Machine& machine, uint64_t ticket);
// Hands the machine's current thread to a free worker that interprets it there (FastPath.hpp,
// interpretInSpace) in `code`, as long as it can. False if there is no free worker. Taken back like a
// native run; settle then says Exit::interpret, with pc at the instruction the worker did not run.
bool offloadInterpreter(Machine& machine, uint64_t ticket, const SpaceCode& code);
// A run that has ended and has not been taken back.
bool finished(uint64_t& ticket);
// Takes a finished run back: the thread's registers go to `cpu` unless it is null. False if the run
// has not ended.
bool take(uint64_t ticket, CPU* cpu, Result& result);
// What run() returns, for a result that was taken back: with the thread's registers in machine.cpu
// and its address space in machine.space.
Exit settle(Machine& machine, const Result& result);
// Asks a run to end soon, or with `wait` returns once it has.
void recall(uint64_t ticket, bool wait);
std::string statistics();
// The host address of the access that made the last run end with Exit::fault.
uint64_t lastFaultAddress();

} // namespace native
} // namespace nestvane
