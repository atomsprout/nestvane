# Static rewriting

Static rewriting is how Android's machine code becomes code that iOS will load and run.
"Static" means it happens on the build machine, before the app is signed; nothing is translated on the phone.

The rewriter is [`experiments/g0/e3/rewrite.py`](../experiments/g0/e3/rewrite.py), about 580 lines of Python.
The driver that applies it to a whole Android image is [`tools/build-native-code.py`](../tools/build-native-code.py).
The run-time side is [`runtime/NativeCode.cpp`](../runtime/NativeCode.cpp) and [`runtime/NativeEntry.S`](../runtime/NativeEntry.S).

## Why registers are reserved

Guest and host are both ARM64, so most guest instructions can run as they are.
What cannot stay the same is anything that depends on where things are: a guest pointer is not a host pointer, a guest code address is not where the translation sits, and `svc` must not reach the iOS kernel.
Each of those instructions is replaced by a short sequence that needs working storage: at least a pointer to per-thread state, a pointer to the memory table, a pointer to the code table, and one scratch register.

If the guest's own code used every register, the rewriter would have to save and restore registers around each sequence, or analyse each function to find a free one.
Instead the guest is compiled so that it never uses the registers the runtime needs.
Android is built with

```
-ffixed-x18 -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28
```

for C, C++ and assembly, and with the matching `+reserve-x…` target features for Rust ([`experiments/aosp/patch.py`](../experiments/aosp/patch.py)).
The cost of losing the registers was measured on a set of command-line benchmarks and was not detectable (a ratio of 1.003 on the reference kernel).

A prebuilt Android image cannot be used: in the stock Android 17 SDK image about 6% of instructions use `x25` to `x28`.
`x18` is hardly used there, but it cannot serve the runtime either, because iOS sets it to zero on every entry to the kernel and even during plain computation when the thread is preempted (see [design-notes.md](design-notes.md)).
It is reserved only so that guest code does not depend on it.

The compiler flag is not enough by itself. The patch script also changes:

- Bionic's `setjmp`/`longjmp`, so they neither save nor restore `x25` to `x28` (after a `fork`, a saved value would be the parent's);
- ART, so that its compiler does not allocate those registers, its calling conventions do not list them, and its hand-written entry code does not restore them; ART's assembly interpreter (nterp) is switched off and the portable one is used;
- libraries with hand-written assembly that uses the registers (BoringSSL, the two copies of it inside Cronet, parts of TensorFlow Lite, ruy), which are built with their C or C++ paths instead;
- the toolchain's prebuilt runtime libraries (compiler-rt builtins, libunwind, libc++, the minimal UBSan runtime), which are rebuilt with the same flags by [`experiments/aosp/build-runtimes.sh`](../experiments/aosp/build-runtimes.sh).

[`experiments/aosp/audit.py`](../experiments/aosp/audit.py) disassembles a built tree and lists the files that still name the reserved registers.

## Register roles

| Register | Role in rewritten code |
| --- | --- |
| `x25` | The context of the running guest thread (`NativeContext` in `NativeCode.cpp`): saved registers, the thread pointer, entry points of the runtime. |
| `x26` | The code table, biased so that the entry for guest address `pc` is at `x26 + 2 * pc`. Each entry is the host address of that instruction's translation. |
| `x27` | The memory table of the process: one 8-byte offset per chunk of guest address space. |
| `x28` | Scratch. Holds the host address being computed, or the target of an indirect branch. |
| `x18` | Unused by guest and runtime. |
| `sp` | A **host** address. |
| all others | Guest values, exactly as the original program would have them. Pointers in them are guest addresses. |

Keeping `sp` as a host address means that loads and stores relative to the stack pointer, about a tenth of all instructions, need no rewriting.
A thread's stack always lies in chunks that share one offset, so arithmetic on `sp` stays valid.
When the program copies `sp` to another register or back, the rewriter converts between host and guest.

## What the rewriter does to an ELF file

The rewriter reads the one executable segment of the ELF file and walks it four bytes at a time.
For each guest instruction it emits a label and zero or more host instructions.
From the rewriter's own header:

```
loads and stores through a register other than sp   add the base first
copies between sp and another register              convert between host and guest
adr, adrp, literal loads                            use the guest address as a constant
svc                                                 enter the runtime
mrs/msr tpidr_el0                                   use the context
br, blr, ret                                        translate the guest pc through the table
```

Everything else is copied unchanged; pointer-authentication hint instructions become `nop`.
Data that sits inside the code segment and does not decode as an instruction is copied as a raw word.

**Memory accesses.**
A load or store through register `xN` first computes the host address in `x28` and then performs the original access through `x28`:

```python
self.emit("ubfx x28, x%d, #%d, #%d" % (register, CHUNK_SHIFT, CHUNK_BITS))
self.emit("ldr x28, [x27, x28, lsl #3]")
self.emit("add x28, x28, x%d" % register)
```

The first instruction extracts the chunk number from the guest address (ignoring the top byte, where Bionic keeps a tag), the second reads that chunk's offset, the third adds it.
Guest addresses have 39 bits. The shipped build uses 1 MiB chunks (`--chunk-shift 20`).
Write-back forms (pre- and post-index) are split so that the register update happens after the access; a faulting access has then changed nothing and can be retried or reported as one guest instruction.

Two optimisations are switched on.
With `--reuse`, an access through the same base register as the previous one uses the host address still in `x28`.
With `--split-index`, an access with a register offset looks up the chunk of the base register, which does not have to wait for the index, and checks that the full address is in the same chunk; if not, an out-of-line path does the full look-up.

**Addresses.**
`adr` and `adrp` compute an address relative to the program counter. In the translation the program counter is somewhere else, so the rewriter emits the guest address as a constant (`movz`/`movk`).
This is the reason a translation is tied to one load address.

**Direct branches.**
A branch to an address inside the same module becomes a branch to the label of the target instruction.
Translated code is larger than the original, so a conditional branch whose target may no longer be in range is emitted as an inverted condition over an unconditional branch.

## Indirect branches and returns

`br`, `blr` and `ret` jump to an address held in a register, which is a guest code address.
The translation looks it up:

```python
def lookup(self, register):
    self.emit("add x28, x26, x%d, lsl #1" % register)
    self.emit("ldr x28, [x28]")
```

followed by `br x28`.
The code table has one 8-byte entry per 4-byte guest instruction, which is why the index is `2 * pc`.

The link register `x30` holds the **guest** return address, so every return goes through the table as well.
ART reads return addresses from the stack to walk frames, and with guest return addresses it needs no change for that.
Measured on command-line benchmarks, looking up returns cost about one percentage point over leaving them native.

A direct branch that leaves the module (compiled Java code calls helper routines in another file this way) also goes through the table, because where it leads is known only once both files are mapped.

The table is where control flow is contained.
The kernel layer maps a module's part of the table only when the process has mapped that module at its address.
Elsewhere the table is reserved address space with nothing behind it, so a jump to an address without a translation faults on the look-up, before control goes anywhere, and the runtime takes over.
Addresses inside a module that are not instructions lead to a stub that leaves native code.

## System calls

```python
if w & 0xFFE0001F == 0xD4000001:  # svc
    self.emit("adr x28, 1f")
    self.emit("b _guest_svc")
    self.out.append("1:")
```

`_guest_svc` is a two-instruction routine linked into each framework. It loads the runtime's entry point from the context and jumps to it:

```
_guest_svc:
	ldr x27, [x25, #864]
	br x27
```

It goes through the context because a call between two Mach-O images would otherwise pass through a linker stub that uses registers belonging to the guest.
`x27` is free to use here because the runtime reloads it whenever the guest is entered.
The entry point, `nestvane_native_svc` in `NativeEntry.S`, stores every guest register in the context, with `x28` as the place to resume, and returns to the main loop.
Reads and writes of the thread pointer (`tpidr_el0`) become loads and stores in the context.

## Why a translation is tied to a load address, and how addresses are assigned

Because guest addresses are baked into the translation as constants and into the code table as positions, a module must be loaded exactly where it was translated for.
Android normally loads libraries at random addresses.
Three things make the addresses fixed.

1. `build-native-code.py` gives each file an address in the range `0x8000_0000` to `0xFFF0_0000` and records it in `generated/native-code/addresses.txt`. A file keeps its address across builds, so adding or removing files does not force everything to be translated again. Addresses are aligned to 256 KiB, because Bionic's control-flow-integrity shadow has one entry per 256 KiB and two libraries must not share one.
2. Files are identified by content, not by path or inode: the key is an FNV-1a hash of the first 16 KiB mixed with the file size. The runtime computes the same key for a file the guest maps.
3. The dynamic linker is patched to ask where a file belongs. The kernel layer passes a table of (device, inode, address) records in two extra entries of the auxiliary vector (`0x4e56`, `0x4e57`); the linker reserves that address if the file is listed. On a real Linux kernel the entries are absent and the linker behaves as usual, which is why the same image also boots in the reference emulator.

Compiled boot class path code (`boot*.oat`) is translated for the address recorded in its image header, and ART is started with `-Xnorelocate` so that it maps the files there.

One process can load the same bytes twice: a library that exists under several paths, a platform library loaded again in an app's linker namespace, or compiled code that `system_server` loads after zygote already has.
One translation serves one address, so such files get a spare translation at a second address, and the kernel layer hands out whichever copy the process has not used yet.

## Packaging

Each translation is assembled into an object file with the Apple toolchain.
One Mach-O image cannot hold them all, so the objects are divided by size into 12 dynamic libraries; for iOS each is wrapped as a framework, `NestvaneCode<n>.framework`, since an app may not carry bare dynamic libraries.
`modules.c` lists every module with its key, size, address and name.

A code table with 8 bytes per guest instruction would be as large as the code.
The frameworks therefore carry a compact form (`--final-table`): the length of each instruction's translation in 2 bytes, the start of every 64th, and the list of instructions entered through a stub.
The runtime builds the real table from this, one page at a time, when a page is first looked up in.
The pages belong to a file, so iOS can write them out and reclaim them, and they do not count as the app's own memory.

With `--used <list>`, the driver leaves out spare copies that a recorded session never mapped, and modules for hardware the guest does not have (Bluetooth, Wi-Fi, NFC and others).
A file without a translation still runs: it is interpreted.

## Code without a translation: mixed execution

Not all code can be translated ahead of time: the native libraries inside an APK the user installs are not known when the app is built.
Such code is interpreted in place by [`runtime/AArch64.cpp`](../runtime/AArch64.cpp), and it lives in the same process as translated code.

The kernel layer places the dynamic linker's reservations for untranslated libraries in a fixed 1 GiB range of guest addresses (`foreignLow` to `foreignHigh` in [`runtime/NativeCode.hpp`](../runtime/NativeCode.hpp)).
The code table spans that range without any entries.
When translated code calls into such a library, the table look-up faults; the runtime catches the fault and the interpreter runs from the target address until control returns to an address that has a translation, where native execution resumes.

The same mechanism handles single instructions.
If a translated instruction faults in the host (its memory has no host pages yet, or the access crosses out of a run of chunks), the guest's registers are still those from before the instruction, so the interpreter can execute just that instruction and native execution continues with the next.

The reasons native code can stop are the `Exit` values in `NativeCode.hpp`: `syscall`, `fault`, `preempted`, `interpret`, `retry` and `lost`.
If a program itself, or its dynamic linker, has no translation, the whole process is interpreted, because translated libraries returning into it would find no table entries.

Interpreted code is much slower than translated code; [status.md](status.md) gives an example.

## Measured cost

All figures are from [`results/G0.md`](../results/G0.md).

| Measurement | Result |
| --- | --- |
| Rewritten Bionic tools compared with the same binaries on a Linux kernel, on a Mac | 1.07 to 1.6 times the native time |
| The same with the chunk table instead of a single base, estimated | about 1.4 to 1.5 times native |
| Size of translated compiled Java code relative to the original | 1.57 to 1.77 times |
| Boot to the launcher on an iPhone 15 Pro, interpreter only | 606 s |
| The same with translated code | about 8 s |
