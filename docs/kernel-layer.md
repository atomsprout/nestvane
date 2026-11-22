# The kernel layer

Android's programs are Linux programs. They ask the kernel for everything: memory, files, threads, timers, sockets, and the Binder driver through which Android's processes call each other.
Inside an iOS app there is no Linux kernel to ask, and the iOS kernel's interface is different and mostly closed to apps.
So Nestvane contains its own implementation of the Linux system call interface, in user space, as ordinary C++.
These documents call it the kernel layer.

It is not a port of Linux. It is written from the observed behaviour of the interface: what each call returns, in what order things happen, what the files under `/proc` and `/sys` contain.
Where Android depends on the exact behaviour, the layer follows Linux closely. Where Android only needs an answer, the layer gives a fixed one.
This document says which is which.

## Source files

| File | Contents |
| --- | --- |
| [`runtime/ProbeLinux.cpp`](../runtime/ProbeLinux.cpp), [`ProbeLinux.hpp`](../runtime/ProbeLinux.hpp) | The system calls, tasks, scheduler, mounts, files, sockets, signals. About 12,000 lines. |
| [`runtime/ProbeBinder.cpp`](../runtime/ProbeBinder.cpp) | The Binder driver. |
| [`runtime/ProbeData.cpp`](../runtime/ProbeData.cpp) | The persistent store behind `/data`. |
| [`runtime/GuestSpace.cpp`](../runtime/GuestSpace.cpp), [`GuestSpace.hpp`](../runtime/GuestSpace.hpp) | A guest process's address space as host memory. |
| [`runtime/Ext4.cpp`](../runtime/Ext4.cpp) | A read-only reader for ext4 images. |
| [`runtime/GuestRoute.hpp`](../runtime/GuestRoute.hpp), [`GuestNetlink.hpp`](../runtime/GuestNetlink.hpp) | The network interface and routing netlink. |
| [`runtime/BpfProgram.hpp`](../runtime/BpfProgram.hpp), [`GuestFirewall.hpp`](../runtime/GuestFirewall.hpp) | The eBPF programs and packet filter rules that Android's network daemons install. |
| [`runtime/DynamicProbe.cpp`](../runtime/DynamicProbe.cpp) | The main loop that runs guest threads and calls the layer. |

The name "Probe" is historical: the layer began as a test harness that ran one Android program at a time and reported the first system call it could not answer.

## Tasks and scheduling

A guest thread is a `Thread` record: saved registers, its process, its mount namespace, its seccomp filters, signal state, and what it is waiting for.
A process is a `Process` record shared by its threads, holding the memory mappings (`Segment`s), the address space, descriptors and credentials.
`clone`, `fork` and `execve` create and replace these as Linux does; `fork` copies the address space copy-on-write.

Scheduling is a single loop on one host thread.
`switchThread` takes the next thread in thread-id order that is not waiting, restores its registers, and lets it run for one turn: a time slice of native code, or a fixed number of interpreted instructions.
Timers and wait deadlines are examined every 200 microseconds rather than at every switch, because a boot switches threads several hundred thousand times.
When no thread can run, the loop sleeps until the earliest deadline or until a host event arrives.

Because all system calls execute on this one thread, the layer needs no locks around its own state.
The price is that system calls never run in parallel.
A thread whose native code has used a whole turn without leaving can be handed to a worker thread on another core (see [static-rewriting.md](static-rewriting.md)); its next system call brings it back.

What is simplified: every thread is scheduled as `SCHED_OTHER` with equal weight, whatever policy or priority the guest sets. Control groups exist as a `cgroup2` tree with no controllers: processes can be moved between groups, which is what Android's process grouping needs, and no resource limit is enforced.

## Memory

Each process has a list of segments kept in address order, each with a range, protection, and a backing: anonymous pages, a private copy of a file, or storage shared between processes (memory files, ashmem, Binder buffers).
`mmap`, `munmap`, `mprotect`, `mremap` and `madvise` operate on this list.

For code to run natively, guest memory must be real host memory at a predictable place.
[`GuestSpace`](../runtime/GuestSpace.hpp) provides that: the guest address space is cut into 1 MiB chunks, each either backed by host memory at some offset or not backed at all.
Backed chunks are grouped into runs with guard pages on both sides, so an access that strays out of the run of its base register faults instead of reaching unrelated memory.
Reservations with no access (`PROT_NONE`), which Android's allocator makes in units of gigabytes, take no host address space until they are made accessible or touched.

- Private segments live directly in the `GuestSpace`.
- Shared storage is kept in a `SharedBuffer`, host pages that are mapped into each process that maps the storage.
- Read-only file content is mapped copy-on-write from one host copy, so that many processes do not each hold their own.

The guest page size is 16 KiB, the same as the host's. This is required: with matching page sizes a guest `mprotect` can be passed straight to the host.
The Android build is the 16 KiB-page variant for the same reason.

Not implemented: `userfaultfd` (ART falls back to a stop-the-world compacting collector) and `io_uring`. Swap does not exist.

## Files, mounts and namespaces

The file system layer is a path-based virtual file system with several kinds of backing.

| What | Backing |
| --- | --- |
| The system partitions (`/system`, `/vendor`, `/product`, …) | The guest root shipped in the app, read-only. |
| APEX modules under `/apex` | Read-only ext4 images, read by `Ext4.cpp` and mounted through emulated loop devices by Android's own `apexd`. |
| `tmpfs` mounts (`/dev`, `/mnt`, `/apex`, …) | Memory. |
| `/data` | The persistent store, described in [devices.md](devices.md). |
| `/proc`, `/sys` | Generated files: process status, maps, command lines, the network interface, power supplies, memory pressure. |
| `binder`, `bpf`, `cgroup2`, `fuse` | Special handling per type. |

`mount` accepts the types `tmpfs`, `ext4`, `binder`, `bpf` and `fuse`, plus bind mounts and remounts; any other type returns "no such device".

**Mount namespaces and propagation.**
Android gives each app process its own mount namespace so that it sees only its own storage.
The layer implements namespaces (`unshare(CLONE_NEWNS)`, `setns`) as separate mount tables.
It also implements propagation, because Android depends on it: the storage daemon `vold` mounts the shared storage in the initial namespace, and apps must see that mount appear in theirs.
A mount belongs to a peer group when it is shared, and remembers the group it came from when it is made a slave.
A new mount is copied to every namespace in which the directory it is placed on is visible through a mount of the same peer group or through a slave of that group ([`bindMount`](../runtime/ProbeLinux.cpp)).
Each namespace is limited to 128 mounts.

**Metadata.**
Files carry owner, group, mode, timestamps, extended attributes (including the SELinux label), POSIX access control lists, record locks and memory-file seals.
Permission checks use them, with one exception described below for the shared storage.

**Simplified: FUSE.**
The one file system Android mounts through FUSE is the shared storage, served by a daemon inside the media provider.
The layer does not implement the FUSE protocol.
A `fuse` mount is a direct view of `/data/media`. The daemon receives the opening `FUSE_INIT` request, answers it, and from then on its reads simply wait until the file system is unmounted.
No file operation passes through the daemon, so the per-app access control the media provider would apply is not applied: for paths under `/data/media` the layer does not check owner or mode.

## Binder

Binder is implemented as a driver model, not by forwarding to anything in the host.
`ProbeBinder.cpp` handles the `ioctl` interface on `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder`, which appear through a `binder` file system mount as on a real device.

Implemented: the context manager registration that `servicemanager` performs; synchronous and one-way transactions with replies; scatter-gather transactions (how the hardware interface layer, HIDL, passes buffers); translation of object references and handles between processes with reference counting; passing file descriptors, singly and as arrays inside a buffer; death notifications; the looper thread protocol including the request to spawn threads; the security context attached to a transaction; and `poll` on a Binder descriptor.
Transaction data is delivered into the receiver's mapped buffer, limited to 4 MiB per mapping, with the accounting of the asynchronous budget that Android relies on.

Android's own `servicemanager` and the services above it run unmodified on this.

## Signals, futexes and event waiting

**Signals** follow the Linux model: per-thread masks, pending sets, handlers with `siginfo`, alternate stacks, `rt_sigreturn`, signals that interrupt a wait, and default actions that end the process.
A handler runs on a signal frame that the layer builds in guest memory (`installSignalFrame`), and `rt_sigreturn` restores the interrupted state from it.
Host faults in guest code become guest `SIGSEGV` when the guest address really is invalid; ART's null-pointer checks depend on this.
The handler's return goes to a trampoline page at a fixed guest address (`0xFFFF0000`) whose code-table entries leave native code.

**Futexes** support wait and wake with bit masks, timeouts, requeueing, and the priority-inheritance operations. A futex in shared memory is identified by its backing storage, so two processes that map the same memory at different addresses meet on the same futex.

**Event waiting** covers `epoll`, `poll`/`ppoll`, `eventfd`, `timerfd`, `signalfd` and `inotify`.
A waiting thread is woken by the event itself, not by polling, except for waits that depend on the host (a host socket, the renderer), which are looked at again after a short delay.

## Sockets and netlink

Unix-domain sockets (stream, datagram and sequenced-packet, with descriptor and credential passing) are implemented in the layer; Android's property service, logging, zygote command socket and many others use them.
Internet sockets to the loopback address stay inside the layer. Sockets to other addresses are carried by host sockets; see [devices.md](devices.md).

Netlink is implemented for the families Android uses at boot: routing (links, addresses, routes, neighbours), kernel object events (`uevent`, which `ueventd` reads to create device nodes), netfilter and XFRM.
Link and address queries describe the loopback and one Ethernet interface. Route and neighbour tables accept writes without keeping them, since the host does the routing.
The eBPF programs and maps that Android's network daemons load are stored and interpreted (`BpfProgram.hpp`), and packet filter rules are modelled (`GuestFirewall.hpp`); they see only the guest's loopback traffic.

## SELinux as captured tables

Android runs with SELinux enforcing and its user space consults the policy constantly: `init` to decide which domain a service runs in, `servicemanager` to decide who may register or find a service, the property service, keystore.
The layer does not contain a policy engine.
Instead, the answers are captured from the reference emulator when the guest root is prepared: [`tools/prepare-init-root.py`](../tools/prepare-init-root.py) and [`tools/android_policy.py`](../tools/android_policy.py) query the real kernel for the decisions the boot needs and write them to text files under `nestvane-metadata/` in the guest root (domain transitions, file labels, and access tables for properties, services, hardware services and keystore).

At run time the layer presents `/sys/fs/selinux` with `enforce` reading 1 and answers queries to its `access`, `create` and `context` files from those tables. Labels are stored as the `security.selinux` attribute of each file.

The consequence: a decision that was not captured is not available.
Some services added later (the media server, the codec service, the media extractor) had no captured rows; their rules were written by hand from those of a similar domain, as recorded in [design-notes.md](design-notes.md).
There is no policy engine that judges every system call; checks happen where Android's user space asks for a decision, and in the few places the layer consults a captured table itself, such as labelling new files.

## Seccomp

Seccomp filters are implemented faithfully, because Android installs them on zygote's children and on media services, and a filter that behaves differently changes what a process is allowed to do.
`installSeccomp` reads and validates a classic BPF filter program. `enforceSeccomp` runs every filter attached to the thread on each system call, over the same data a Linux kernel passes (call number, architecture, instruction pointer, six arguments), and applies the most restrictive verdict: allow, return an error, raise `SIGSYS`, or kill.
Filters are inherited across `fork` and `clone`.

This matters for more than fidelity: an early version of the dynamic linker patch used a private system call, which worked in the kernel layer before it had seccomp and killed `system_server` on a real kernel.

## Device nodes

| Node | Purpose |
| --- | --- |
| `/dev/null`, `/dev/random`, `/dev/urandom`, `/dev/kmsg` | As on Linux. |
| `/dev/ptmx`, `/dev/pts/*` | Pseudo-terminals. |
| `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` | Binder. |
| `/dev/ashmem`, `/dev/dma_heap/system` | Shareable memory; each allocation is a memory file. |
| `/dev/loop-control`, `/dev/block/loop*` | Loop devices, for mounting APEX images. |
| `/dev/fuse` | The simplified FUSE session described above. |
| `/dev/goldfish_pipe_dprctd`, `/dev/goldfish_address_space` | The emulator's graphics pipe and shared graphics memory. |
| `/dev/input/event0` | Touch and keys. |
| `/dev/snd/pcmC0D0p` | One sound playback device. |

Device nodes are created by Android's `ueventd` in response to the kernel object events the layer sends, as on a real device.

## What happens with a call that is not implemented

The layer implements the calls Android was observed to make. An unknown system call number is not silently ignored: the main loop records it in the report and suspends the calling process, or stops the guest if the caller is `init`.
This is deliberate. A wrong answer tends to surface much later as an unrelated failure, while a stop names the missing call.
