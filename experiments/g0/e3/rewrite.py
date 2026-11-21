#!/usr/bin/env python3
"""Rewrites a static AArch64 Linux ELF into Mach-O assembly that runs its code natively.

The guest must be built with x18 and x25-x28 reserved. The rewritten code uses
  x25  context of the running guest thread
  x26  code table, biased so that the entry for guest pc is at x26 + 2 * pc
  x27  host address of guest address zero
  x28  scratch
Registers hold guest addresses, except sp, which holds a host address so that sp-relative accesses
need no rewriting. Only these instructions change:
  loads and stores through a register other than sp   add the base first
  copies between sp and another register              convert between host and guest
  adr, adrp, literal loads                            use the guest address as a constant
  svc                                                 enter the runtime
  mrs/msr tpidr_el0                                   use the context
  br, blr, ret                                        translate the guest pc through the table

With --table, x27 points at a per-process table instead of holding the base: the host address of
guest address A is A + table[A >> 24], for a 39-bit guest address space in 16 MiB chunks. This is the scheme a guest needs when its address space is too
sparse to reserve as one block, and it costs two more instructions and a load per access.

With --reuse, an access through the same base register as the access before it uses the address
that is still in x28, when nothing in between changed either. The instructions from after the first
access to the second can also be reached by an indirect branch or a signal return, with x28 holding
something else, so each of them gets an entry stub that works the address out again: the code table
points at the stub, and straight-line execution does not pass through it.

With --table --split-index, an access with a register offset first uses the chunk of its base
register, which does not have to wait for the index, and checks that the address is in the same
chunk. If it is not, a path placed out of line looks the address's chunk up.

With --host-lr, bl and ret stay native calls and returns and x30 holds a host address. Without it,
x30 holds the guest return address and every return goes through the table.

A position-independent ELF (an Android executable or shared library) is translated for one load
address, given with --base. --name sets the module name used in symbols, so that several modules
can be linked into one program. Each module exports _guest_module_<name>: its text start, text end,
entry point and code table.

usage: rewrite.py [--host-lr] [--table] [--chunk-shift 24] [--reuse] [--split-index] [--final-table] [--symbols] [--base 0x...] [--name module] <elf> <output.s>
"""
import struct
import sys

BASE, SCRATCH, TABLE, CONTEXT = 27, 28, 26, 25
TPIDR_OFFSET = 784
BASE_OFFSET, MEMORY_TABLE_OFFSET, SPARE_OFFSET = 800, 824, 848
ADDRESS_BITS = 39  # of a guest address, with --table
CHUNK_SHIFT = int(sys.argv[sys.argv.index("--chunk-shift") + 1]) if "--chunk-shift" in sys.argv else 24  # 16 MiB chunks
CHUNK_BITS = ADDRESS_BITS - CHUNK_SHIFT
CONDITIONS = "eq ne hs lo mi pl vs vc hi ls ge lt gt le al nv".split()
# Conditional branches reach 1 MiB and test-and-branch 32 KiB of host code. Translated code is up to
# about 2.5 times larger overall and more in places, so guest distances beyond these get a long form.
FAR_CONDITIONAL, FAR_TEST = 0x30000, 0x1000
# xpaclri, pacia1716 ... autibsp: the hint-space pointer authentication instructions.
PAC_HINTS = {0xD50320FF, 0xD503211F, 0xD503215F, 0xD503219F, 0xD50321DF, 0xD503231F, 0xD503233F, 0xD503235F,
             0xD503237F, 0xD503239F, 0xD50323BF, 0xD50323DF, 0xD50323FF}


def signed(value, bits):
    return value - (1 << bits) if value >> (bits - 1) else value


class Rewriter:
    def __init__(self, path, host_lr, symbols=False, table=False, base=0, name="main"):
        self.base, self.name = base, name
        self.host_lr = host_lr
        self.table = table
        self.symbols = symbols  # keep a symbol per guest instruction, for debugging
        self.data = open(path, "rb").read()
        assert self.data[:4] == b"\x7fELF" and self.data[4] == 2, "need a 64-bit ELF"
        (self.entry, phoff) = struct.unpack_from("<QQ", self.data, 24)
        self.entry += base
        phentsize, phnum = struct.unpack_from("<HH", self.data, 54)
        self.start = self.end = None
        for index in range(phnum):
            kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", self.data, phoff + index * phentsize)
            if kind == 1 and flags & 1:
                assert self.start is None, "one executable segment expected"
                self.start, self.end, self.offset = base + vaddr, base + vaddr + (filesz + 3 & ~3), offset
        # Pad the front so the first instruction is 4-byte aligned in guest address terms.
        assert self.start % 4 == 0
        self.out = []
        self.counts = {}
        # --reuse: the base register whose host address x28 holds, the instructions emitted since
        # that have not disturbed it, and the entry stubs: guest pc -> base register.
        self.reuse = "--reuse" in sys.argv
        # --split-index: see memory(). The accesses whose address is in another chunk than their base.
        self.split, self.far = "--split-index" in sys.argv, []
        self.cache, self.pending, self.kept, self.stubs, self.leaders = None, [], False, {}, set()
        if self.reuse:
            for pc in range(self.start, self.end, 4):
                w, = struct.unpack_from("<I", self.data, self.offset + pc - self.start)
                if w & 0x7C000000 == 0x14000000:
                    self.leaders.add(pc + signed(w & 0x3FFFFFF, 26) * 4)
                elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:
                    self.leaders.add(pc + signed(w >> 5 & 0x7FFFF, 19) * 4)
                elif w & 0x7E000000 == 0x36000000:
                    self.leaders.add(pc + signed(w >> 5 & 0x3FFF, 14) * 4)

    def flush_far(self):
        for pc, address, register in self.far:
            self.out.append("Lf%x:" % pc)
            self.emit(address)
            self.emit("ubfx x28, x28, #%d, #%d" % (CHUNK_SHIFT, CHUNK_BITS))
            self.emit("ldr x28, [x27, x28, lsl #3]")
            self.emit("add x28, x28, x%d" % register)
            self.emit("b Lb%x" % pc)
        self.far = []

    def keep(self, written=()):
        """This instruction leaves x28 alone and writes only these registers."""
        if self.cache is not None and self.cache not in written:
            self.kept = True

    @staticmethod
    def writes(w):
        """The general registers a load or store changes, or None for forms that are not reused."""
        rt, rt2, rn, vector = w & 31, w >> 10 & 31, w >> 5 & 31, w >> 26 & 1
        if w & 0x3A000000 == 0x28000000:  # pair
            result = {rt, rt2} if w >> 22 & 1 and not vector else set()
            return result | {rn} if w >> 23 & 1 else result
        if w & 0x3B000000 == 0x39000000 or w & 0x3B200000 == 0x38000000:  # immediate offsets
            result = {rt} if w >> 22 & 3 and not vector else set()
            return result | {rn} if not w >> 24 & 1 and w >> 10 & 1 else result
        return None

    def count(self, kind, extra):
        entry = self.counts.setdefault(kind, [0, 0])
        entry[0] += 1
        entry[1] += extra

    def emit(self, text):
        self.out.append("\t" + text)

    def word(self, value):
        self.emit(".long 0x%08x" % (value & 0xFFFFFFFF))

    def label(self, address):
        # Local to the file unless symbols are wanted, so the module's name need not be in it: a large
        # module has millions of them.
        return "g_%s_%x" % (self.name, address) if self.symbols else "Li%x" % address

    def is_code(self, address):
        return self.start <= address < self.end and address % 4 == 0

    def constant(self, register, value):
        """Materialises a guest address. Returns the number of instructions."""
        assert 0 <= value < 1 << 48
        self.emit("movz x%d, #0x%x" % (register, value & 0xFFFF))
        count = 1
        for shift in (16, 32):
            part = value >> shift & 0xFFFF
            if part:
                self.emit("movk x%d, #0x%x, lsl #%d" % (register, part, shift))
                count += 1
        return count

    def add_base(self, register):
        """Leaves the host address of the guest address in a register in x28. Returns the instructions added."""
        if not self.table:
            self.word(0x8B000000 | register << 16 | BASE << 5 | SCRATCH)  # add x28, x27, xN
            return 1
        # Only the address bits pick the chunk: Bionic keeps a tag in the top byte of heap pointers.
        self.emit("ubfx x28, x%d, #%d, #%d" % (register, CHUNK_SHIFT, CHUNK_BITS))
        self.emit("ldr x28, [x27, x28, lsl #3]")
        self.emit("add x28, x28, x%d" % register)
        return 3

    def add_base_constant(self, address):
        """The same for a constant guest address, which is in x28. x27 is borrowed for the chunk's offset."""
        if not self.table:
            return self.add_base(SCRATCH)
        assert address >> CHUNK_SHIFT < 4096
        self.emit("ldr x27, [x27, #%d]" % ((address >> CHUNK_SHIFT) * 8))
        self.emit("add x28, x27, x28")
        self.emit("ldr x27, [x25, #%d]" % MEMORY_TABLE_OFFSET)
        return 3

    def adjust(self, register, amount):
        if amount > 0:
            self.emit("add x%d, x%d, #%d" % (register, register, amount))
        elif amount < 0:
            self.emit("sub x%d, x%d, #%d" % (register, register, -amount))

    def lookup(self, register):
        self.emit("add x28, x26, x%d, lsl #1" % register)
        self.emit("ldr x28, [x28]")

    def memory(self, pc, w):
        """Loads and stores: the encoding group with bit 27 set and bit 25 clear."""
        if w & 0x3B000000 == 0x18000000:  # pc-relative literal
            target = pc + signed(w >> 5 & 0x7FFFF, 19) * 4
            opc, vector, rt = w >> 30, w >> 26 & 1, w & 31
            extra = self.constant(SCRATCH, target)
            extra += self.add_base_constant(target) - 1
            if vector:
                self.emit("ldr %s%d, [x28]" % ("sdq"[opc], rt))
            else:
                self.emit(("ldr w%d, [x28]", "ldr x%d, [x28]", "ldrsw x%d, [x28]", "prfm #%d, [x28]")[opc] % rt)
            return self.count("literal_load", extra + 1)
        rn = w >> 5 & 31
        if rn == 31:
            self.word(w)
            if self.writes(w) is not None:
                self.keep(self.writes(w))
            return self.count("sp_relative", 0)
        # `after` is the write-back. It is applied after the access, so that a faulting access has
        # changed nothing and can be reported or retried as one guest instruction.
        after = 0
        plain = w
        if w & 0xBFA00000 == 0x0C800000:  # AdvSIMD multiple structures, post-index
            rm = w >> 16 & 31
            registers = {0: 4, 2: 4, 4: 3, 6: 3, 7: 1, 8: 2, 10: 2}.get(w >> 12 & 15)
            if registers is None:  # not an instruction: data that sits in the code
                self.word(w)
                return self.count("unchanged", 0)
            after = registers * (16 if w >> 30 & 1 else 8) if rm == 31 else ("register", rm)
            plain = w & ~(1 << 23 | 31 << 16)
        elif w & 0xBF800000 == 0x0D800000:  # AdvSIMD single structure, post-index
            rm, opcode, size = w >> 16 & 31, w >> 13 & 7, w >> 10 & 3
            elements = ((opcode & 1) << 1 | w >> 21 & 1) + 1
            scale = opcode >> 1
            width = 1 << size if scale == 3 else (1, 2, 8 if size & 1 else 4)[scale]
            after = elements * width if rm == 31 else ("register", rm)
            plain = w & ~(1 << 23 | 31 << 16)
        elif w & 0x3A000000 == 0x28000000:  # pair
            mode = w >> 23 & 3
            if mode in (1, 3):
                opc, vector = w >> 30, w >> 26 & 1
                after = signed(w >> 15 & 0x7F, 7) << (2 + opc if vector else 2 + (opc >> 1))
                plain = w & ~(3 << 23) | 2 << 23  # the signed-offset form
                if mode == 1:
                    plain &= ~(0x7F << 15)  # post-index: access at the base
        elif w & 0x3B200000 == 0x38000000:  # register with a 9-bit offset
            mode = w >> 10 & 3
            if mode in (1, 3):
                after = signed(w >> 12 & 0x1FF, 9)
                plain = w & ~(3 << 10)  # the unscaled-offset form
                if mode == 1:
                    plain &= ~(0x1FF << 12)  # post-index: access at the base
        written = self.writes(w) if self.reuse else None
        if written is not None and self.cache == rn:
            # x28 still holds this base register's host address.
            for address in self.pending + [pc]:
                self.stubs[address] = rn
            added = -1
        elif self.table and w & 0x3B200C00 == 0x38200800:
            # A register offset can reach any chunk, so the chunk is that of the whole address. The
            # access keeps its index, and x28 becomes the base plus the chunk's offset.
            rm, option, vector = w >> 16 & 31, w >> 13 & 7, w >> 26 & 1
            amount = (4 if vector and w >> 23 & 1 else w >> 30) if w >> 12 & 1 else 0
            if option == 3:
                address = "add x28, x%d, x%d, lsl #%d" % (rn, rm, amount)
            else:
                address = "add x28, x%d, %s%d, %s #%d" % (rn, "x" if option & 1 else "w", rm, {2: "uxtw", 6: "sxtw", 7: "sxtx"}[option], amount)
            self.emit(address)
            if self.split:
                # Nearly always the address is in the base register's chunk. Looking that chunk up does
                # not wait for the index, and a check that leaves the flags alone covers the rest.
                self.emit("eor x28, x28, x%d" % rn)
                self.emit("ubfx x28, x28, #%d, #%d" % (CHUNK_SHIFT, CHUNK_BITS))
                self.emit("cbnz x28, Lf%x" % pc)
                self.add_base(rn)
                self.out.append("Lb%x:" % pc)
                if not self.far:
                    self.far_since = len(self.out)
                self.far.append((pc, address, rn))
                added = 7
            else:
                self.emit("ubfx x28, x28, #%d, #%d" % (CHUNK_SHIFT, CHUNK_BITS))
                self.emit("ldr x28, [x27, x28, lsl #3]")
                self.emit("add x28, x28, x%d" % rn)
                added = 4
        else:
            added = self.add_base(rn)
        self.word(plain & ~(31 << 5) | SCRATCH << 5)
        if isinstance(after, tuple):
            self.emit("add x%d, x%d, x%d" % (rn, rn, after[1]))
        else:
            self.adjust(rn, after)
        if written is not None and rn not in written:  # the next access through rn can use x28 as it is
            self.cache, self.pending, self.kept = rn, [], True
        if added < 0:
            return self.count("memory_reused", after != 0)
        self.count("memory", added + (after != 0))

    def set_sp(self):
        """sp = host address of the guest address in x28."""
        if self.table:
            # The offset of the stack's chunk is kept in the context, for reading sp back as a guest
            # address. A stack lies in chunks that share one offset, so sp arithmetic stays native.
            self.emit("str x28, [x25, #%d]" % SPARE_OFFSET)
            self.emit("ubfx x28, x28, #%d, #%d" % (CHUNK_SHIFT, CHUNK_BITS))
            self.emit("ldr x28, [x27, x28, lsl #3]")
            self.emit("str x28, [x25, #%d]" % BASE_OFFSET)
            self.emit("ldr x27, [x25, #%d]" % SPARE_OFFSET)
            self.emit("add sp, x27, x28")
            self.emit("ldr x27, [x25, #%d]" % MEMORY_TABLE_OFFSET)
        else:
            self.emit("add sp, x27, x28")
        self.count("sp_write", 7 if self.table else 1)

    def stack_pointer(self, w):
        """Add/sub with sp as a source or destination. Returns True when handled."""
        immediate = w & 0x1F000000 == 0x11000000
        extended = w & 0x1F200000 == 0x0B200000
        logical = w & 0x1F800000 == 0x12000000
        rd, rn, flags = w & 31, w >> 5 & 31, w >> 29 & 1
        if immediate or extended:
            reads, writes = rn == 31, rd == 31 and not flags
            if reads and writes or not reads and not writes:
                return False
            if reads:
                if self.table:
                    self.emit("ldr x28, [x25, #%d]" % BASE_OFFSET)
                    self.emit("sub x28, sp, x28")
                else:
                    self.emit("sub x28, sp, x27")
                self.word(w & ~(31 << 5) | SCRATCH << 5)
                self.count("sp_read", 1 + self.table)
            else:
                self.word(w & ~31 | SCRATCH)
                self.set_sp()
            return True
        if logical and rd == 31 and w >> 29 & 3 != 3:
            self.word(w & ~31 | SCRATCH)
            self.set_sp()
            return True
        return False

    def branch(self, pc, w):
        """Returns True when the instruction was a branch and has been emitted."""
        if w & 0x7C000000 == 0x14000000:
            target = pc + signed(w & 0x3FFFFFF, 26) * 4
            if not self.is_code(target):
                # Into another module: compiled boot class path code calls thunks in the first oat file
                # of its image. Where it leads is only known once both are mapped, so go through the table.
                if not 0 <= target < 1 << 48 or target % 4:
                    return False
                extra = self.constant(SCRATCH, target)
                self.lookup(SCRATCH)
                if w >> 31:
                    extra += self.constant(30, pc + 4) if not self.host_lr else 0
                self.emit("blr x28" if w >> 31 and self.host_lr else "br x28")
                self.count("external_branch", extra + 2)
                return True
            if not w >> 31:
                self.emit("b " + self.label(target))
                self.count("direct_branch", 0)
            elif self.host_lr:
                self.emit("bl " + self.label(target))
                self.count("call", 0)
            else:
                extra = self.constant(30, pc + 4)
                self.emit("b " + self.label(target))
                self.count("call", extra)
            return True
        if w & 0xFF000010 == 0x54000000:
            target = pc + signed(w >> 5 & 0x7FFFF, 19) * 4
            if not self.is_code(target):
                return False
            self.keep()
            if abs(target - pc) < FAR_CONDITIONAL:
                self.emit("b.%s %s" % (CONDITIONS[w & 15], self.label(target)))
                self.count("direct_branch", 0)
            else:  # the rewritten code is larger, so a far conditional branch may no longer reach
                self.emit("b.%s 1f" % CONDITIONS[w & 15 ^ 1])
                self.emit("b " + self.label(target))
                self.out.append("1:")
                self.count("direct_branch", 1)
            return True
        if w & 0x7E000000 == 0x34000000:
            target = pc + signed(w >> 5 & 0x7FFFF, 19) * 4
            if not self.is_code(target):
                return False
            self.keep()
            register = "%s%d" % ("x" if w >> 31 else "w", w & 31)
            if abs(target - pc) < FAR_CONDITIONAL:
                self.emit("%s %s, %s" % ("cbnz" if w >> 24 & 1 else "cbz", register, self.label(target)))
                self.count("direct_branch", 0)
            else:
                self.emit("%s %s, 1f" % ("cbz" if w >> 24 & 1 else "cbnz", register))
                self.emit("b " + self.label(target))
                self.out.append("1:")
                self.count("direct_branch", 1)
            return True
        if w & 0x7E000000 == 0x36000000:
            target = pc + signed(w >> 5 & 0x3FFF, 14) * 4
            if not self.is_code(target):
                return False
            self.keep()
            bit = (w >> 31) << 5 | w >> 19 & 31
            register = "%s%d" % ("x" if bit >= 32 else "w", w & 31)
            taken = w >> 24 & 1
            if abs(target - pc) < FAR_TEST:
                self.emit("%s %s, #%d, %s" % ("tbnz" if taken else "tbz", register, bit, self.label(target)))
                self.count("direct_branch", 0)
            else:  # the rewritten code is larger, so a far test-and-branch may no longer reach
                self.emit("%s %s, #%d, 1f" % ("tbz" if taken else "tbnz", register, bit))
                self.emit("b " + self.label(target))
                self.out.append("1:")
                self.count("direct_branch", 1)
            return True
        if w & 0xFF9FFC1F == 0xD61F0000:
            kind, rn = w >> 21 & 3, w >> 5 & 31
            if kind > 2:
                return False
            if self.host_lr and rn == 30 and kind != 1:  # ret, or br x30 as in longjmp
                self.word(w)
                self.count("return", 0)
            elif kind == 1:
                self.lookup(rn)
                if self.host_lr:
                    self.emit("blr x28")
                    self.count("indirect_call", 2)
                else:
                    extra = self.constant(30, pc + 4)
                    self.emit("br x28")
                    self.count("indirect_call", 2 + extra)
            else:
                self.lookup(rn)
                self.emit("br x28")
                self.count("return" if kind == 2 else "indirect_branch", 2)
            return True
        return False

    def instruction(self, pc, w):
        if pc in self.leaders:
            self.cache = None
        self.kept, before = False, self.cache
        mark = len(self.out)
        try:
            self.translate(pc, w)
        except (KeyError, IndexError, ValueError, AssertionError):
            # Not an instruction but data that sits in the code, in a form no instruction takes.
            del self.out[mark:]
            self.far = [entry for entry in self.far if entry[0] != pc]
            self.word(w)
            self.count("undecodable", 0)
        if not self.kept:
            self.cache = None
        elif self.cache == before and (not self.pending or self.pending[-1] != pc) and pc not in self.stubs:
            self.pending.append(pc)  # not an access through the cached register: something in between
            if len(self.pending) > 8:
                self.cache = None

    def translate(self, pc, w):
        if self.branch(pc, w):
            return
        if w in PAC_HINTS:
            self.word(0xD503201F)
            self.keep()
            return self.count("pac_hint", 0)
        if w & 0xFFE0001F == 0xD4000001:  # svc
            self.emit("adr x28, 1f")
            self.emit("b _guest_svc")
            self.out.append("1:")
            return self.count("svc", 1)
        if w & 0xFFFFFFE0 == 0xD53BD040:
            self.emit("ldr x%d, [x25, #%d]" % (w & 31, TPIDR_OFFSET))
            self.keep({w & 31})
            return self.count("tls", 0)
        if w & 0xFFFFFFE0 == 0xD51BD040:
            self.emit("str x%d, [x25, #%d]" % (w & 31, TPIDR_OFFSET))
            return self.count("tls", 0)
        if w & 0xFFFFFFE0 == 0xD50B7420:  # dc zva
            added = self.add_base(w & 31)
            self.word(w & ~31 | SCRATCH)
            return self.count("memory", added)
        if w & 0x1F000000 == 0x10000000:  # adr, adrp
            offset = signed((w >> 5 & 0x7FFFF) << 2 | w >> 29 & 3, 21)
            target = (pc & ~0xFFF) + (offset << 12) if w >> 31 else pc + offset
            self.keep({w & 31})
            if w & 31 == 31:
                return self.word(0xD503201F)
            return self.count("address", self.constant(w & 31, target) - 1)
        if w & 0x0A000000 == 0x08000000:
            return self.memory(pc, w)
        if self.stack_pointer(w):
            return
        self.word(w)
        self.keep({w & 31})  # where every other instruction names the general register it writes
        self.count("unchanged", 0)

    def run(self):
        out = self.out
        out.append(".section __TEXT,__text,regular,pure_instructions")
        out.append(".p2align 4")
        out.append(".globl _guest_code_%s" % self.name)
        out.append("_guest_code_%s:" % self.name)
        for pc in range(self.start, self.end, 4):
            out.append(self.label(pc) + ":")
            w, = struct.unpack_from("<I", self.data, self.offset + pc - self.start)
            self.instruction(pc, w)
            # The far paths of --split-index go where execution does not fall through, within reach of
            # the checks that branch to them: after an unconditional branch, or else behind a jump.
            if self.far and (w & 0xFC000000 == 0x14000000 or w & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000)):
                self.flush_far()
            elif self.far and len(out) - self.far_since > 40000:
                self.emit("b 1f")
                self.flush_far()
                out.append("1:")
        out.append("\tbrk #0x3ff")
        # Entry stubs come after the code, so that host addresses in the table below stay in order.
        for pc, register in sorted(self.stubs.items()):
            out.append("Ls%x:" % pc)
            self.add_base(register)
            self.emit("b " + self.label(pc))
        self.flush_far()
        if "--final-table" in sys.argv:
            return self.finish_mapped()
        out.append(".section __DATA,__const")
        out.append(".p2align 3")
        table = "Lguest_table_%s" % self.name
        out.append(".globl _guest_module_%s\n_guest_module_%s:" % (self.name, self.name))
        out.append("\t.quad 0x%x, 0x%x, 0x%x, %s, Lguest_stubs_%s, %d" % (self.start, self.end, self.entry, table, self.name, len(self.stubs)))
        out.append(table + ":")
        for pc in range(self.start, self.end, 4):
            out.append("\t.quad " + self.label(pc))
        # Where the code table differs from the table above: instruction index, entry stub.
        out.append("Lguest_stubs_%s:" % self.name)
        for pc in sorted(self.stubs):
            out.append("\t.quad %d, Ls%x" % ((pc - self.start) // 4, pc))
        return "\n".join(out) + "\n"


    def finish_mapped(self):
        """--final-table: what runtime/NativeCode.cpp links against.

        The table of host code per guest instruction is complete, with the entry stubs in place, and
        laid out so that the runtime can map its pages into the table of all translated code instead
        of copying it: that table has the entry for guest pc at 2 * pc from a page boundary, and so
        has this one. A second table gives where each instruction's translation starts, as an offset
        from the start of the code; it needs no pointers fixed up and is only read when a fault or
        the end of a time slice has to be traced back to a guest instruction.
        """
        out, name = self.out, self.name
        # Where a jump to the program's entry point leaves native code, when the runtime asks for it.
        out.append("Lguest_trap_%s:" % name)
        self.constant(SCRATCH, self.entry)
        self.emit("b _guest_trap")
        out.append(".section __TEXT,__const")
        out.append(".p2align 2")
        out.append("Lguest_starts_%s:" % name)
        for pc in range(self.start, self.end, 4):
            out.append("\t.long %s - _guest_code_%s" % (self.label(pc), name))
        out.append(".section __DATA,__const")
        out.append(".p2align 3")
        out.append(".globl _guest_module_%s\n_guest_module_%s:" % (name, name))
        out.append("\t.quad 0x%x, 0x%x, 0x%x, Lguest_table_%s, Lguest_starts_%s, _guest_code_%s, Lguest_trap_%s"
                   % (self.start, self.end, self.entry, name, name, name, name))
        out.append(".p2align 14")
        if self.start * 2 % 16384:
            out.append("\t.space %d" % (self.start * 2 % 16384))
        out.append("Lguest_table_%s:" % name)
        for pc in range(self.start, self.end, 4):
            out.append("\t.quad " + ("Ls%x" % pc if pc in self.stubs else self.label(pc)))
        out.append(".p2align 14")
        return "\n".join(out) + "\n"


def main():
    arguments, options = [], {}
    values = iter(sys.argv[1:])
    for argument in values:
        if argument in ("--base", "--name", "--chunk-shift"):
            options[argument] = next(values)
        elif not argument.startswith("--"):
            arguments.append(argument)
    rewriter = Rewriter(arguments[0], "--host-lr" in sys.argv, "--symbols" in sys.argv, "--table" in sys.argv,
                        int(options.get("--base", "0"), 0), options.get("--name", "main"))
    open(arguments[1], "w").write(rewriter.run())
    guest = (rewriter.end - rewriter.start) // 4
    added = sum(extra for _, extra in rewriter.counts.values())
    print("guest_instructions=%d added_instructions=%d expansion=%.3f host_lr=%d table=%d" % (guest, added, (guest + added) / guest, rewriter.host_lr, rewriter.table))
    for kind, (count, extra) in sorted(rewriter.counts.items(), key=lambda item: -item[1][0]):
        print("  %-16s count=%-7d share=%5.2f%% added=%d" % (kind, count, 100 * count / guest, extra))


if __name__ == "__main__":
    main()
