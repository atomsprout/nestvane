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
guest address A is A + table[A >> 24]. This is the scheme a guest needs when its address space is too
sparse to reserve as one block, and it costs two more instructions and a load per access.

With --host-lr, bl and ret stay native calls and returns and x30 holds a host address. Without it,
x30 holds the guest return address and every return goes through the table.

usage: rewrite.py [--host-lr] [--table] [--symbols] <elf> <output.s>
"""
import struct
import sys

BASE, SCRATCH, TABLE, CONTEXT = 27, 28, 26, 25
TPIDR_OFFSET = 784
BASE_OFFSET, MEMORY_TABLE_OFFSET = 800, 824
CHUNK_SHIFT = 24
CONDITIONS = "eq ne hs lo mi pl vs vc hi ls ge lt gt le al nv".split()


def signed(value, bits):
    return value - (1 << bits) if value >> (bits - 1) else value


class Rewriter:
    def __init__(self, path, host_lr, symbols=False, table=False):
        self.host_lr = host_lr
        self.table = table
        self.symbols = symbols  # keep a symbol per guest instruction, for debugging
        self.data = open(path, "rb").read()
        assert self.data[:4] == b"\x7fELF" and self.data[4] == 2, "need a 64-bit ELF"
        (self.entry, phoff) = struct.unpack_from("<QQ", self.data, 24)
        phentsize, phnum = struct.unpack_from("<HH", self.data, 54)
        self.start = self.end = None
        for index in range(phnum):
            kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", self.data, phoff + index * phentsize)
            if kind == 1 and flags & 1:
                assert self.start is None, "one executable segment expected"
                self.start, self.end, self.offset = vaddr, vaddr + (filesz + 3 & ~3), offset
        # Pad the front so the first instruction is 4-byte aligned in guest address terms.
        assert self.start % 4 == 0
        self.out = []
        self.counts = {}

    def count(self, kind, extra):
        entry = self.counts.setdefault(kind, [0, 0])
        entry[0] += 1
        entry[1] += extra

    def emit(self, text):
        self.out.append("\t" + text)

    def word(self, value):
        self.emit(".long 0x%08x" % (value & 0xFFFFFFFF))

    def label(self, address):
        return ("g_%x" if self.symbols else "Lg_%x") % address

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
        if register == SCRATCH:  # a constant address: borrow x27 for the base, then restore the table pointer
            self.emit("ldr x27, [x25, #%d]" % BASE_OFFSET)
            self.emit("add x28, x27, x28")
            self.emit("ldr x27, [x25, #%d]" % MEMORY_TABLE_OFFSET)
            return 3
        self.emit("lsr x28, x%d, #%d" % (register, CHUNK_SHIFT))
        self.emit("ldr x28, [x27, x28, lsl #3]")
        self.emit("add x28, x28, x%d" % register)
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
            extra += self.add_base(SCRATCH) - 1
            if vector:
                self.emit("ldr %s%d, [x28]" % ("sdq"[opc], rt))
            else:
                self.emit(("ldr w%d, [x28]", "ldr x%d, [x28]", "ldrsw x%d, [x28]", "prfm #%d, [x28]")[opc] % rt)
            return self.count("literal_load", extra + 1)
        rn = w >> 5 & 31
        if rn == 31:
            self.word(w)
            return self.count("sp_relative", 0)
        before = after = 0
        plain = w
        if w & 0xBFA00000 == 0x0C800000:  # AdvSIMD multiple structures, post-index
            rm = w >> 16 & 31
            registers = {0: 4, 2: 4, 4: 3, 6: 3, 7: 1, 8: 2, 10: 2}[w >> 12 & 15]
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
                amount = signed(w >> 15 & 0x7F, 7) << (2 + opc if vector else 2 + (opc >> 1))
                before, after = (amount, 0) if mode == 3 else (0, amount)
                plain = w & ~(3 << 23 | 0x7F << 15) | 2 << 23
        elif w & 0x3B200000 == 0x38000000:  # register with a 9-bit offset
            mode = w >> 10 & 3
            if mode in (1, 3):
                amount = signed(w >> 12 & 0x1FF, 9)
                before, after = (amount, 0) if mode == 3 else (0, amount)
                plain = w & ~(0x1FF << 12 | 3 << 10)
        self.adjust(rn, before)
        added = self.add_base(rn)
        self.word(plain & ~(31 << 5) | SCRATCH << 5)
        if isinstance(after, tuple):
            self.emit("add x%d, x%d, x%d" % (rn, rn, after[1]))
        else:
            self.adjust(rn, after)
        self.count("memory", added + (before != 0) + (after != 0))

    def set_sp(self):
        """sp = host address of the guest address in x28."""
        if self.table:
            self.emit("ldr x27, [x25, #%d]" % BASE_OFFSET)
            self.emit("add sp, x27, x28")
            self.emit("ldr x27, [x25, #%d]" % MEMORY_TABLE_OFFSET)
        else:
            self.emit("add sp, x27, x28")
        self.count("sp_write", 3 if self.table else 1)

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
                return False
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
            self.emit("b.%s %s" % (CONDITIONS[w & 15], self.label(target)))
            self.count("direct_branch", 0)
            return True
        if w & 0x7E000000 == 0x34000000:
            target = pc + signed(w >> 5 & 0x7FFFF, 19) * 4
            if not self.is_code(target):
                return False
            self.emit("%s %s%d, %s" % ("cbnz" if w >> 24 & 1 else "cbz", "x" if w >> 31 else "w", w & 31, self.label(target)))
            self.count("direct_branch", 0)
            return True
        if w & 0x7E000000 == 0x36000000:
            target = pc + signed(w >> 5 & 0x3FFF, 14) * 4
            if not self.is_code(target):
                return False
            bit = (w >> 31) << 5 | w >> 19 & 31
            register = "%s%d" % ("x" if bit >= 32 else "w", w & 31)
            taken = w >> 24 & 1
            if abs(target - pc) < 0x2800:
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
        if self.branch(pc, w):
            return
        if w & 0xFFE0001F == 0xD4000001:  # svc
            self.emit("adr x28, 1f")
            self.emit("b _guest_svc")
            self.out.append("1:")
            return self.count("svc", 1)
        if w & 0xFFFFFFE0 == 0xD53BD040:
            self.emit("ldr x%d, [x25, #%d]" % (w & 31, TPIDR_OFFSET))
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
            if w & 31 == 31:
                return self.word(0xD503201F)
            return self.count("address", self.constant(w & 31, target) - 1)
        if w & 0x0A000000 == 0x08000000:
            return self.memory(pc, w)
        if self.stack_pointer(w):
            return
        self.word(w)
        self.count("unchanged", 0)

    def run(self):
        out = self.out
        out.append(".section __TEXT,__text,regular,pure_instructions")
        out.append(".p2align 4")
        out.append(".globl _guest_code")
        out.append("_guest_code:")
        for pc in range(self.start, self.end, 4):
            out.append(self.label(pc) + ":")
            self.instruction(pc, struct.unpack_from("<I", self.data, self.offset + pc - self.start)[0])
        out.append("\tbrk #0x3ff")
        out.append(".section __DATA,__const")
        out.append(".p2align 3")
        for name, value in (("text_start", self.start), ("text_end", self.end), ("entry", self.entry)):
            out.append(".globl _guest_%s\n_guest_%s:\n\t.quad 0x%x" % (name, name, value))
        out.append(".globl _guest_table\n_guest_table:")
        for pc in range(self.start, self.end, 4):
            out.append("\t.quad " + self.label(pc))
        return "\n".join(out) + "\n"


def main():
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith("--")]
    rewriter = Rewriter(arguments[0], "--host-lr" in sys.argv, "--symbols" in sys.argv, "--table" in sys.argv)
    open(arguments[1], "w").write(rewriter.run())
    guest = (rewriter.end - rewriter.start) // 4
    added = sum(extra for _, extra in rewriter.counts.values())
    print("guest_instructions=%d added_instructions=%d expansion=%.3f host_lr=%d table=%d" % (guest, added, (guest + added) / guest, rewriter.host_lr, rewriter.table))
    for kind, (count, extra) in sorted(rewriter.counts.items(), key=lambda item: -item[1][0]):
        print("  %-16s count=%-7d share=%5.2f%% added=%d" % (kind, count, 100 * count / guest, extra))


if __name__ == "__main__":
    main()
