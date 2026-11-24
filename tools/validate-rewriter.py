#!/usr/bin/env python3
"""Checks the rewriter on instructions that use x25-x28 as ordinary registers, as code built without
them set aside does (WebView's library). Each test is run twice on this Mac's processor: as written, and
as experiments/g0/e3/rewrite.py translates it, with x25 pointing at a context that holds the guest's
x25-x28 and x27 at a table of memory offsets that are all zero (guest addresses are host addresses).
The general registers, the flags, a block of memory and a block of the stack must come out the same.

usage: validate-rewriter.py <work directory>
"""
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv.extend(["--table", "--chunk-shift", "20", "--reuse", "--split-index"])  # as tools/build-native-code.py
sys.path.insert(0, os.path.join(HERE, "../experiments/g0/e3"))
import rewrite  # noqa: E402

# Each test: instructions, and registers to set before it: "M+n" is the test memory plus n.
TESTS = [
    ("add x25, x26, x27", {}),
    ("add w25, w9, w26", {}),
    ("sub x9, x25, #12", {}),
    ("adds x26, x27, x28", {}),
    ("cmp x25, x26\ncset x27, lo", {}),
    ("and x27, x25, #0xff00", {}),
    ("orr x9, x28, x25, lsl #3", {}),
    ("madd x25, x26, x27, x28", {}),
    ("udiv x26, x25, x9", {}),
    ("csel x25, x26, x9, ne", {}),
    ("csinc w27, w25, w28, eq", {}),
    ("ccmp x25, x26, #4, ne\ncset x9, eq", {}),
    ("ccmp x25, #7, #0, eq\ncset x9, eq", {}),
    ("lsl x28, x25, x26", {}),
    ("rev x25, x26", {}),
    ("clz x27, x25", {}),
    ("ubfx x26, x25, #3, #9", {}),
    ("bfi x25, x26, #8, #16", {}),
    ("extr x25, x26, x27, #13", {}),
    ("movz x25, #0x1234, lsl #16", {}),
    ("movk x26, #0xbeef", {}),
    ("adc x25, x26, x27", {}),
    ("mov x25, x9", {}),
    ("mov x9, x26", {}),
    ("mov w27, w25", {}),
    ("mov x26, x25", {}),
    ("add x25, x25, x25, lsl #1", {}),
    ("ldr x25, [x9, #8]", {"x9": "M+0"}),
    ("ldr x9, [x25, #16]", {"x25": "M+0"}),
    ("ldr x26, [x26, #24]", {"x26": "M+0"}),
    ("ldr x25, [x25]", {"x25": "M+8"}),
    ("str x25, [x9, #32]", {"x9": "M+0"}),
    ("str x9, [x27, #40]", {"x27": "M+0"}),
    ("ldr x25, [x26, x27, lsl #3]", {"x26": "M+0", "x27": 3}),
    ("ldr w28, [x9, x25, lsl #2]", {"x9": "M+0", "x25": 5}),
    ("ldrb w25, [x26], #1", {"x26": "M+3"}),
    ("str x27, [x25, #8]!", {"x25": "M+16"}),
    ("ldp x25, x26, [x9, #16]", {"x9": "M+0"}),
    ("stp x27, x28, [x25, #48]", {"x25": "M+0"}),
    ("ldp x9, x10, [x28], #16", {"x28": "M+32"}),
    ("ldp w25, w26, [x27], #8", {"x27": "M+0"}),
    ("stnp x25, x26, [x9, #64]", {"x9": "M+0"}),
    ("ldr x9, [x28, #-8]!", {"x28": "M+64"}),
    ("stur x26, [x25, #-8]", {"x25": "M+64"}),
    ("ldrsw x25, [x26, #4]", {"x26": "M+0"}),
    ("prfm pldl1keep, [x25]", {"x25": "M+0"}),
    ("stp x25, x26, [sp, #16]\nldp x9, x10, [sp, #16]", {}),
    ("ldp x27, x28, [sp, #32]", {}),
    ("stp x25, x26, [sp, #-16]\nldp x9, x10, [sp, #-16]", {}),
    ("str x25, [sp, #8]", {}),
    ("ldr x26, [sp, #24]", {}),
    ("add x25, sp, #16", {}),
    ("mov x26, sp", {}),
    ("ldxr x25, [x9]", {"x9": "M+0"}),
    ("stxr w9, x25, [x26]", {"x26": "M+0"}),
    ("ldadd x25, x26, [x27]", {"x27": "M+8"}),
    ("swp x26, x25, [x9]", {"x9": "M+16"}),
    ("cas x25, x26, [x27]", {"x27": "M+24"}),
    ("ldar x25, [x26]", {"x26": "M+0"}),
    ("stlr x27, [x9]", {"x9": "M+40"}),
    ("fmov d1, x25\nfmov x26, d1", {}),
    ("dup v2.4s, w9\numov w25, v2.s[1]", {}),
    ("dup v3.2d, x26\numov x9, v3.d[1]", {}),
    ("scvtf d0, x25\nfcvtzs x26, d0", {}),
    ("ins v4.d[0], x27\numov x9, v4.d[0]", {}),
    ("ld1 {v0.16b}, [x25]\numov x9, v0.d[1]", {"x25": "M+0"}),
    ("dup v1.2d, x9\nst1 {v1.2d}, [x26], x27", {"x26": "M+0", "x27": 16}),
    ("ldr q0, [x25, #16]\numov x9, v0.d[0]", {"x25": "M+0"}),
    ("fmov d1, x9\nstr d1, [x27, #8]", {"x27": "M+0"}),
    ("cmp w26, #5\ncset x27, gt", {}),
    ("tst x25, #1\ncsel x26, x27, x28, ne", {}),
    ("neg x25, x26", {}),
    ("mul x27, x25, x25", {}),
    ("umulh x28, x25, x26", {}),
    ("sxtw x25, w26", {}),
]

# x0 holds the state while x1-x28 are the test's; the state's slots are x[31], nzcv, the stack block,
# q0-q31 and the memory. The stack block is copied to sp+0..255 before the test and back after it.
STATE_NZCV, STATE_STACK, STATE_Q, STATE_MEMORY = 31 * 8, 256, 512, 1024
MEMORY = 81920
FRAME = 512  # test area at sp+0..255, saved registers above it


def prologue(name, translated):
    lines = [".globl _%s" % name, ".p2align 2", "_%s:" % name,
             "\tsub sp, sp, #%d" % FRAME,
             "\tstp x19, x20, [sp, #256]", "\tstp x21, x22, [sp, #272]", "\tstp x23, x24, [sp, #288]",
             "\tstp x25, x26, [sp, #304]", "\tstp x27, x28, [sp, #320]", "\tstp x29, x30, [sp, #336]",
             "\tstr x0, [sp, #352]", "\tstp d8, d9, [sp, #368]", "\tstp d10, d11, [sp, #384]",
             "\tstp d12, d13, [sp, #400]", "\tstp d14, d15, [sp, #416]", "\tadd x2, x0, #%d" % STATE_Q]
    lines += ["\tldp q%d, q%d, [x2, #%d]" % (r, r + 1, r * 16) for r in range(0, 32, 2)]
    lines += ["\tldr x2, [x0, #%d]\n\tstr x2, [sp, #%d]" % (STATE_STACK + i, i) for i in range(0, 256, 8)]
    lines += ["\tldr x2, [x0, #%d]" % STATE_NZCV, "\tmsr nzcv, x2"]
    if translated:
        lines += ["\tmov x25, x1", "\tldr x27, [x25, #%d]" % rewrite.MEMORY_TABLE_OFFSET, "\tmov x26, xzr"]
    last = 24 if translated else 28
    lines += ["\tldp x%d, x%d, [x0, #%d]" % (r, r + 1, r * 8) for r in range(1, last, 2)]
    if last % 2 == 0:
        lines += ["\tldr x%d, [x0, #%d]" % (last, last * 8)]
    lines += ["\tldr x0, [x0]"]
    return lines


def epilogue(translated):
    lines = ["\tstr x0, [sp, #360]", "\tldr x0, [sp, #352]"]
    last = 24 if translated else 28
    lines += ["\tstp x%d, x%d, [x0, #%d]" % (r, r + 1, r * 8) for r in range(1, last, 2)]
    if last % 2 == 0:
        lines += ["\tstr x%d, [x0, #%d]" % (last, last * 8)]
    lines += ["\tldr x1, [sp, #360]", "\tstr x1, [x0]", "\tmrs x1, nzcv", "\tstr x1, [x0, #%d]" % STATE_NZCV,
              "\tadd x1, x0, #%d" % STATE_Q]
    lines += ["\tstp q%d, q%d, [x1, #%d]" % (r, r + 1, r * 16) for r in range(0, 32, 2)]
    lines += ["\tldp d8, d9, [sp, #368]", "\tldp d10, d11, [sp, #384]", "\tldp d12, d13, [sp, #400]", "\tldp d14, d15, [sp, #416]"]
    lines += ["\tldr x2, [sp, #%d]\n\tstr x2, [x0, #%d]" % (i, STATE_STACK + i) for i in range(0, 256, 8)]
    lines += ["\tldp x19, x20, [sp, #256]", "\tldp x21, x22, [sp, #272]", "\tldp x23, x24, [sp, #288]",
              "\tldp x25, x26, [sp, #304]", "\tldp x27, x28, [sp, #320]", "\tldp x29, x30, [sp, #336]",
              "\tadd sp, sp, #%d" % FRAME, "\tret"]
    return lines


def encodings(work, sources):
    """Assembles each test's instructions and returns their words."""
    path = os.path.join(work, "encode.s")
    with open(path, "w") as out:
        out.write(".text\n")
        for index, source in enumerate(sources):
            out.write(".globl _t%d\n_t%d:\n%s\n" % (index, index, source))
    subprocess.run(["xcrun", "clang", "-c", "-arch", "arm64", path, "-o", path + ".o"], check=True)
    text = subprocess.run(["xcrun", "otool", "-t", path + ".o"], capture_output=True, text=True, check=True).stdout
    words = []
    for line in text.splitlines()[2:]:
        words += [int(part, 16) for part in line.split()[1:]]
    result, at = [], 0
    for source in sources:
        count = len(source.splitlines())
        result.append(words[at:at + count])
        at += count
    return result


def translated_lines(index, words):
    r = rewrite.Rewriter.__new__(rewrite.Rewriter)
    r.base, r.name, r.host_lr, r.table, r.symbols = 0, "t%d" % index, False, True, False
    r.out, r.counts, r.reuse, r.split, r.far = [], {}, True, True, []
    r.cache, r.pending, r.kept, r.stubs, r.leaders = None, [], False, {}, set()
    pc = 0x100000 + index * 0x1000
    r.start, r.end, r.offset, r.entry = pc, pc + 4 * len(words), 0, pc
    for n, w in enumerate(words):
        r.out.append(r.label(pc + 4 * n) + "_%d:" % index)
        r.instruction(pc + 4 * n, w)
    end = r.label(pc + 4 * len(words)) + "_%d" % index
    r.out.append(end + ":")
    body = [line.replace("Lb%x" % (pc + 4 * n), "Lb%x_%d" % (pc + 4 * n, index)).replace("Lf%x" % (pc + 4 * n), "Lf%x_%d" % (pc + 4 * n, index))
            for line in r.out for n in [0]]
    far = []
    if r.far:
        saved = r.out
        r.out = []
        r.flush_far()
        far = r.out
        r.out = saved
    fix = lambda line: _rename(line, index, pc, len(words))
    return [fix(line) for line in r.out], [fix(line) for line in far], r.counts


def _rename(line, index, pc, count):
    for n in range(count + 1):
        for prefix in ("Lb", "Lf", "Li"):
            label = "%s%x" % (prefix, pc + 4 * n)
            if label in line and label + "_" not in line:
                line = line.replace(label, "%s_%d" % (label, index))
    return line


def sampled(path, count):
    """Instructions of a library that name x18 or x25-x28, chosen at random, with registers set so that
    their accesses land in the test memory. Left out are the ones that differ by design (pc-relative,
    system registers, exclusive access), branches, and ones that change sp, x29 or x30."""
    data = open(path, "rb").read()
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    for index in range(phnum):
        kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        if kind == 1 and flags & 1:
            break
    words = set()
    for at in range(offset, offset + filesz - 3, 4):
        w, = struct.unpack_from("<I", data, at)
        fields = rewrite.register_fields(w)
        if not fields or not any(w >> field & 31 in rewrite.GUEST_ONLY for field in fields):
            continue
        if w & 0x1C000000 == 0x14000000 or w & 0x3F000000 == 0x08000000:  # branches, system; exclusive
            continue
        if w & 0x1C000000 == 0x10000000 and (w >> 23 & 7 < 2 or 31 in (w & 31, w >> 5 & 31)):  # adr; sp
            continue
        if w & 0x0A000000 == 0x08000000 and (w & 0x3B000000 == 0x18000000 or w >> 5 & 31 == 31):  # literal; sp
            continue
        if w & 0x7FC00000 in (0x68800000, 0x69000000, 0x69800000):  # stgp: tags, which nothing here uses
            continue
        if w & 0xFF200400 == 0xF8200400:  # ldraa, ldrab: left to the interpreter
            continue
        if w & 0x0A000000 == 0x08000000 and w & 0x3A000000 == 0x28000000 and w >> 5 & 31 in (w & 31, w >> 10 & 31):
            continue  # a pair that stores its own base: an address, which differs between the two runs
        if any(w >> field & 31 in (18, 29, 30) for field in fields):  # x18 is the system's on macOS too
            continue
        if w & 0x0E000000 == 0x0A000000 and 31 in (w & 31, w >> 5 & 31) and w & 0x1F200000 == 0x0B200000:
            continue  # add and subtract of an extended register: 31 is sp
        words.add(w)
    chosen = random.Random(int(os.environ.get("VALIDATE_REWRITER_SEED", "1"))).sample(sorted(words), min(count, len(words)))
    tests = []
    for w in chosen:
        setup = {}
        if w & 0x0A000000 == 0x08000000:
            setup["x%d" % (w >> 5 & 31)] = "M+4096"
            if w & 0x3B200C00 == 0x38200800 or w & 0xBF000000 in (0x0C000000, 0x0D000000):
                setup["x%d" % (w >> 16 & 31)] = 16
        tests.append((".long 0x%08x" % w, setup))
    print("sampled %d of %d instructions that name a guest register" % (len(tests), len(words)))
    return tests


def main():
    work = sys.argv[1]
    if len(sys.argv) > 3 and not sys.argv[2].startswith("--"):
        TESTS[:] = sampled(sys.argv[2], int(sys.argv[3]))
    os.makedirs(work, exist_ok=True)
    sources = [source for source, _ in TESTS]
    words = encodings(work, sources)
    asm = [".text", ".globl _guest_trap", "_guest_trap:", "\tbrk #1"]
    used = {}
    for index, (source, _) in enumerate(TESTS):
        asm += prologue("reference%d" % index, False) + ["\t" + line for line in source.splitlines()] + epilogue(False)
        body, far, counts = translated_lines(index, words[index])
        for key, value in counts.items():
            used[key] = used.get(key, 0) + value[0]
        asm += prologue("translated%d" % index, True) + body + epilogue(True) + far
    with open(os.path.join(work, "tests.s"), "w") as out:
        out.write("\n".join(asm) + "\n")
    setups = []
    for index, (_, setup) in enumerate(TESTS):
        entries = []
        for register, value in setup.items():
            number = int(register[1:])
            if isinstance(value, str):
                entries.append("{%d, true, %d}" % (number, int(value[2:])))
            else:
                entries.append("{%d, false, %d}" % (number, value))
        setups.append("{%s}" % ", ".join(entries))
    harness = """#include <cstdint>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
struct State { uint64_t x[31]; uint64_t nzcv; uint8_t stack[256]; uint8_t q[512]; uint8_t memory[81920]; };
struct Setup { int reg; bool memory; uint64_t value; };
extern "C" {
%s
}
using Reference = void (*)(State*);
using Translated = void (*)(State*, uint64_t*);
static const Reference references[] = {%s};
static const Translated translations[] = {%s};
static const std::vector<Setup> setups[] = {%s};
static const char* const names[] = {%s};
static sigjmp_buf trapped;
static void onTrap(int) { siglongjmp(trapped, 1); }
int main() {
    struct sigaction action {};
    action.sa_handler = onTrap;
    for (int number : {SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGTRAP}) sigaction(number, &action, nullptr);
    int skipped = 0;
    std::mt19937_64 random(1);
    std::vector<uint64_t> table(1 << 19);
    int failures = 0;
    for (size_t test = 0; test < sizeof(references) / sizeof(references[0]); ++test) {
        if (std::getenv("VALIDATE_REWRITER_VERBOSE")) { std::printf("%%s\\n", names[test]); std::fflush(stdout); }
        for (int round = 0; round < 8; ++round) {
            static State first, second;
            first = State{};
            for (auto& value : first.x) value = random();
            for (auto& value : first.memory) value = uint8_t(random());
            for (auto& value : first.q) value = uint8_t(random());
            for (auto& value : first.stack) value = uint8_t(random());
            first.nzcv = (random() & 15) << 28;
            second = first;
            for (const auto& setup : setups[test]) {
                first.x[setup.reg] = setup.memory ? uint64_t(first.memory + setup.value) : setup.value;
                second.x[setup.reg] = setup.memory ? uint64_t(second.memory + setup.value) : setup.value;
            }
            const State before = second;
            // Data in the code that is no instruction here: as written it traps, so it is no test.
            if (sigsetjmp(trapped, 1)) { ++skipped; break; }
            references[test](&first);
            std::vector<uint64_t> context(128);
            for (int r : {18, 25, 26, 27, 28}) context[r] = second.x[r];
            context[824 / 8] = uint64_t(table.data());
            translations[test](&second, context.data());
            for (int r : {18, 25, 26, 27, 28}) second.x[r] = context[r];
            if (context[904 / 8]) { std::printf("%%s: borrowed left set\\n", names[test]); ++failures; }
            for (int r = 0; r < 29; ++r) {
                uint64_t a = first.x[r], b = second.x[r];
                // Addresses into each state's own memory compare as offsets.
                auto relative = [](uint64_t value, const State& state) {
                    const uint64_t start = uint64_t(state.memory);
                    return value >= start && value <= start + 81920 ? value - start + 0x5a5a000000000000ULL : value;
                };
                if (relative(a, first) != relative(b, second) && r != 18) {
                    std::printf("%%s: x%%d %%llx != %%llx (was %%llx)\\n", names[test], r, (unsigned long long)a, (unsigned long long)b,
                                (unsigned long long)before.x[r]);
                    ++failures;
                }
            }
            if (first.nzcv != second.nzcv) { std::printf("%%s: flags differ\\n", names[test]); ++failures; }
            if (std::memcmp(first.memory, second.memory, 81920)) { std::printf("%%s: memory differs\\n", names[test]); ++failures; }
            if (std::memcmp(first.q, second.q, 512)) { std::printf("%%s: vector registers differ\\n", names[test]); ++failures; }
            if (std::memcmp(first.stack, second.stack, 256)) { std::printf("%%s: stack differs\\n", names[test]); ++failures; }
            if (failures > 20) return 1;
        }
    }
    std::printf("rewriter: %%zu tests, %%d failures, %%d not instructions here\\n", sizeof(references) / sizeof(references[0]), failures, skipped);
    return failures != 0;
}
""" % ("\n".join("void reference%d(State*); void translated%d(State*, uint64_t*);" % (i, i) for i in range(len(TESTS))),
       ", ".join("reference%d" % i for i in range(len(TESTS))),
       ", ".join("translated%d" % i for i in range(len(TESTS))),
       ", ".join(setups),
       ", ".join('"%s"' % source.replace("\n", "; ") for source in sources))
    with open(os.path.join(work, "harness.cpp"), "w") as out:
        out.write(harness)
    subprocess.run(["xcrun", "clang++", "-std=c++20", "-O1", "-arch", "arm64", os.path.join(work, "harness.cpp"),
                    os.path.join(work, "tests.s"), "-o", os.path.join(work, "validate-rewriter")], check=True)
    result = subprocess.run([os.path.join(work, "validate-rewriter")])
    print("lent: %d instructions, interpreted: %d" % (used.get("lent_register", 0), used.get("interpreted", 0)))
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
