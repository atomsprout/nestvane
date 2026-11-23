#!/usr/bin/env python3
"""Lists the instructions in native libraries that the interpreter has no implementation for.

    xcrun clang++ -O2 -std=c++20 -Iruntime runtime/Runtime.cpp runtime/GuestSpace.cpp runtime/NativeCode.cpp \
        runtime/NativeEntry.S runtime/NoNativeCode.cpp runtime/AArch64.cpp tools/no-aot.cpp \
        tools/scan-interpreter-coverage.cpp -o generated/scan-interpreter-coverage
    tools/scan-interpreter-coverage.py lib1.so lib2.so ...

Each library is disassembled with llvm-objdump and every instruction word is given to the interpreter
once. Data that sits among the code is disassembled too, so a few of the rarer entries are not real.
Instructions a library only uses after asking AT_HWCAP (AES, SHA, the LSE atomics, SVE) are listed
all the same: the guest is told it has none of them.
"""
import subprocess, sys, collections, re
libs = sys.argv[1:]
total = collections.Counter(); examples = {}; perlib = collections.defaultdict(collections.Counter)
for lib in libs:
    dis = subprocess.run(['xcrun','llvm-objdump','-d','--no-show-raw-insn',lib], capture_output=True, text=True).stdout
    raw = subprocess.run(['xcrun','llvm-objdump','-d',lib], capture_output=True, text=True).stdout
    pairs = []
    for line in raw.splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(\S+)\s*(.*)', line)
        if not m: continue
        word = m.group(1)
        mnem = m.group(2)
        if mnem in ('.word','udf','<unknown>'): continue
        pairs.append((word, mnem, m.group(3)))
    words = sorted(set(w for w,_,_ in pairs))
    out = subprocess.run([__import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'generated', 'scan-interpreter-coverage')], input='\n'.join(words)+'\n', capture_output=True, text=True).stdout.split()
    bad = set(out)
    for w, mnem, ops in pairs:
        if w in bad:
            total[mnem] += 1; perlib[lib.split('/')[-1]][mnem] += 1
            examples.setdefault(mnem, (w, ops))
    print(lib.split('/')[-1], 'instructions', len(pairs), 'distinct words', len(words), 'unsupported words', len(bad), file=sys.stderr)
for mnem, n in total.most_common():
    w, ops = examples[mnem]
    print(f'{n:7d} {mnem:10s} e.g. {w} {ops[:40]:40s}', ' '.join(f'{k}:{v[mnem]}' for k,v in perlib.items() if v[mnem]))
