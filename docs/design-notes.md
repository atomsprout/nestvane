# Design notes

This document collects decisions and pitfalls.
The first part explains why the design is what it is. The second is a selection of problems met while building it, each given as problem, cause and fix.
All of them are taken from the lab record, [`results/G0.md`](../results/G0.md), which has more detail and the measurements.

Many of the problems share a shape: a symptom appears far from its cause, and the obvious suspect is innocent.
That is the main reason they are written down.

## Decisions

### Rebuild Android instead of rewriting a stock image

A stock image would avoid building AOSP at all. It was ruled out by measurement.
In the stock Android 17 SDK image about 6% of instructions use `x25` to `x28` (9,377 of 150,837 in `libc.so`), so the rewriter would have to find a free register at every rewritten instruction.
The one register the image leaves almost unused is `x18`, and iOS does not preserve it: a value written to `x18` was lost on every system call, sleep and signal, and occasionally during pure computation (941 of 55,117 trials on an iPhone 15 Pro) when the thread was preempted. It cannot hold a value even across two instructions.

### A chunk table instead of one block of memory per process

The first experimental runtime gave each guest process one contiguous block and kept its base in a register, which costs one added instruction per memory access.
Android does not fit. In the reference image at the launcher, 117 processes had reserved 1,510.7 GiB of addresses in total; the smallest native process reserves about 10 GiB, mostly the allocator's 8 GiB reservation.
An iOS app gets about 53 GiB of address space with the extended addressing entitlement and about 7 GiB without, on both devices tested.
So guest addresses are translated per chunk through a table, and only chunks that are actually touched take host addresses. The table costs two more instructions and a load per access.

### Guest return addresses

Leaving `bl` and `ret` as native calls and returns would be fastest, but then the link register and the stack would hold host addresses.
ART walks the stack and looks return addresses up in its own tables.
Keeping guest return addresses and sending every return through the code table cost about one percentage point on the command-line benchmarks, so ART's stack walking needs no change.

### Compile everything ahead of time

On a phone, Android's boot image is compiled by profile and the JIT compiles the rest when it turns out to be hot.
Here there is no JIT, so a method that was not compiled is interpreted by ART for good: sorting four million integers with `Arrays.sort` took 2.1 seconds that way.
The build therefore compiles the boot image, the system server and the apps in full. The guest's own package manager only verifies an installed app, since code it compiled on the phone would have no translation.

### One scheduler thread first

The kernel layer was written as single-threaded code and stays that way: every system call runs on the main loop.
Only stretches of pure computation move to worker threads.
Measured at boot on an iPhone 15 Pro this changed little (10.6 to 10.7 seconds with workers, 11.2 without, within the noise), because during boot few threads compute for a whole millisecond without a system call.

## Pitfalls

### The storage volume never mounted

- **Problem.** Android's shared storage stayed unmounted. Settings crashed on its storage page and apps that asked for external storage failed.
- **Cause.** Nothing to do with storage. The thread of `StorageManagerService` that would send the mount request first asked whether an HEVC decoder exists; that query waited forever for the `media.player` service, and the guest root had no `mediaserver`. The mount message sat in the queue behind it.
- **Fix.** Add `mediaserver` and its libraries to the guest root, and remove the declaration of a codec service that was declared but not present.
- **How it was found.** By making the Java process print its thread stacks: a debugging switch sends `SIGQUIT` to the process and keeps it from connecting to the crash daemon, so ART writes the stacks to the log.

### Host names did not resolve

- **Problem.** With the network interface up and DNS answers arriving, apps still got "unable to resolve host".
- **Cause.** Before each query Android's resolver probes for an IPv4 route with a UDP socket and hands that socket to the querying app with `fchown`. The kernel layer answered `fchown` on a socket with "read-only file system", and the resolver concluded there was no IPv4 at all and sent no query.
- **Fix.** `fchown` on a socket by root succeeds.
- **Before that**, three other causes produced the same symptom in turn: the Ethernet configuration's capability field left empty meant "none" rather than "default"; the resolver's route probe connects a UDP socket to port 0, which the host refuses; and a thread of the connectivity service called an unimplemented `setsockopt` and was suspended for good, leaving the network in "connecting".

### The phone refused every packet

- **Problem.** Every datagram the host sent failed with "no route to host", including a direct test to a public address.
- **Cause.** On this phone each app has to be allowed onto the network, and iOS asks the user only when the app uses the system's own loader. Plain sockets never trigger the question.
- **Fix.** One request through `NSURLSession` at start.

### Assumptions about FUSE

- **Problem.** Shared storage on Android is a FUSE file system served by the media provider. Implementing the FUSE protocol in the kernel layer looked like a requirement.
- **Cause of the simplification.** FUSE is used for exactly one mount, and the media provider only waits for the session to be opened before it reports the volume as ready.
- **Fix.** A `fuse` mount is a view of `/data/media`. The daemon gets the `FUSE_INIT` request and nothing after it.
- **What this gives up.** The provider's per-app access control. The code states it where the decision is made ([`runtime/ProbeLinux.hpp`](../runtime/ProbeLinux.hpp), `FuseConnection`).

### Mount propagation stopped at the namespace boundary

- **Problem.** The volume was mounted, and the media provider and apps still saw an empty `/storage/emulated`.
- **Cause.** `vold` mounts in the initial mount namespace, and each app has a namespace of its own whose mounts are slaves of the original. The kernel layer propagated a new mount only to peer mounts within the same namespace.
- **Fix.** Propagate to mounts of the same peer group in every namespace, and to mounts that were made slaves of that group.

### The interpreter stopped the guest on an instruction it did not know

- **Problem.** Opening a video in a gallery app made the whole app appear to crash.
- **Cause.** Translated code runs every instruction the processor has, natively. The interpreter knows only the instructions it was taught. When a fault sent execution to the interpreter in the middle of code that reads the timer registers (`CNTVCT_EL0`, `CNTFRQ_EL0`) or zeroes a cache block (`DC ZVA`), the interpreter did not know them, and an unknown instruction stops the guest.
- **Fix.** Add those instructions to the interpreter.
- **Lesson.** With mixed execution the interpreter must cover whatever translated code may be executing when a fault occurs, not only what untranslated libraries contain.

### A jump to a bad address cannot be caught on iOS

- **Problem.** The first design marked special places (a program's entry point, the signal return trampoline) with an invalid address in the code table and caught the fault of jumping there. On the Mac this worked. On iOS the process was killed.
- **Cause.** iOS treats a jump to an address that is not signed code as a code-signing violation and sends `SIGKILL`, which cannot be handled.
- **Fix.** Every table entry that native code may jump through is real code: a small stub that hands the guest address to the runtime. Only the table look-up itself, a data access, is allowed to fault.

### A private system call killed the system server

- **Problem.** The rebuilt image did not boot in the reference emulator: zygote restarted every five seconds.
- **Cause.** The patched dynamic linker asked the host for a file's load address with a private system call number. On a real kernel `system_server` runs under a seccomp filter, which kills a process that makes a call it does not list. The kernel layer had no seccomp at the time, so it never showed.
- **Fix.** Pass the address table in the auxiliary vector instead.

### Time slices never ended

- **Problem.** A guest thread in a native loop was never preempted on the phone.
- **Cause.** The guest ran on a thread of a dispatch queue, and Darwin does not deliver `pthread_kill` signals to such threads.
- **Fix.** The guest runs on a dedicated thread.

### Fifty seconds of sleep during boot

- **Problem.** On an iPhone 11 the boot took 110 seconds, and for 50 of them no guest thread was runnable.
- **Cause.** The guest root had the init script of `lmkd`, the low-memory killer daemon, but not the program. Each time the activity manager adjusted a process's priority it waited three seconds for the daemon's socket while holding a lock.
- **Fix.** Add `lmkd` and the `/proc/pressure` files it opens. Boot went to 34 seconds on the same phone.

### A file is not identified by its inode, and one file is not one mapping

- **Problem.** A translation is for one file at one address, and the first scheme matched files to translations by inode number and assumed one address per file. Neither holds.
- **Causes.** The kernel layer reuses inode numbers. The same bytes exist under many paths (one C++ library had more than twenty copies). One process can load the same file twice.
- **Fix.** Identify files by a hash of their content, translate one copy per path, and keep spare translations at other addresses.
- **Related.** Load addresses had to be aligned to 256 KiB rather than 64 KiB, because Bionic's control-flow-integrity shadow has one entry per 256 KiB.

### Resource overlays that did nothing

- **Problem.** A resource overlay meant to change a framework setting had no effect.
- **Cause.** The overlay daemon, `idmap2d`, had been trimmed from the guest root. Without it overlays are scanned and never applied.
- **Fix.** Add the daemon. This also explained an earlier overlay, tried to shorten the boot, that had seemed to make no difference.

### The same instruction faulted ten million times

- **Problem.** During the first boots on an iPhone 11, one instruction faulted more than ten million times.
- **Cause.** Memory that is mapped but never written has no host pages. The interpreter reads it as zeros without allocating, so native code faulted, the interpreter executed the instruction, and the next access faulted again.
- **Fix.** The first native access allocates the pages and retries natively.

### Hand-written assembly inside a prebuilt-looking library

- **Problem.** Zygote died while preloading the HTTP engine library.
- **Cause.** Cronet carries its own copy of BoringSSL's assembly, which uses `x25` to `x28`. Its self-test at load time overwrote the runtime's registers.
- **Fix.** Build that copy without assembly as well, and audit the built tree for the reserved registers.

### Copying the code table

- **Problem.** At boot on the Mac, guest code used 1.4 seconds and copying 642 MiB of code tables used 2.2.
- **Fix, in two steps.** First the tables were laid out so that they could be mapped into the global table with `vm_remap` instead of copied. Later the 8-byte tables were dropped from the app altogether in favour of 2-byte lengths, and the runtime now generates table pages on first use into file-backed memory. The translated code shrank from 2.59 GB to 1.70 GB.

### Boot waited for shaders

- **Problem.** The launcher appeared 1.6 seconds later than it could.
- **Cause.** SurfaceFlinger compiled 97 shaders at boot in 3.9 seconds; SystemUI waited for it, the wallpaper was not drawn, and the boot animation did not end.
- **Fix.** Switch off the priming and compile shaders when first used.
