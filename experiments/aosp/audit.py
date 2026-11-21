#!/usr/bin/env python3
"""Lists the ELF files of a tree whose code names x25-x28, the registers that the host runtime keeps.

A build patched by patch.py should leave none outside hand-written assembly and the routines that
save and restore every register. What is left is where the next patch goes.

usage: audit.py <tree> [--top N]
"""
import concurrent.futures
import os
import re
import subprocess
import sys

PATTERN = re.compile(rb"\b[xw]2[5-8]\b")


def objdump():
    root = os.path.expanduser("~/Library/Android/sdk/ndk")
    return os.path.join(root, sorted(os.listdir(root))[-1], "toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objdump")


def count(path):
    with open(path, "rb") as file:
        if file.read(4) != b"\x7fELF":
            return None
    output = subprocess.run([objdump(), "-d", "--no-show-raw-insn", path], capture_output=True).stdout
    lines = output.splitlines()
    uses = [line for line in lines if PATTERN.search(line)]
    # The symbol each use is in, for the report: the last label line above it.
    symbols, current = {}, b"?"
    for line in lines:
        if line.endswith(b">:"):
            current = line.split(b"<")[-1][:-2]
        elif PATTERN.search(line):
            symbols[current] = symbols.get(current, 0) + 1
    return len(uses), len(lines), sorted(symbols.items(), key=lambda item: -item[1])[:4]


def main():
    tree = os.path.abspath(sys.argv[1])
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 40
    paths = [os.path.join(directory, name) for directory, _, files in os.walk(tree) for name in files
             if not os.path.islink(os.path.join(directory, name)) and (name.endswith(".so") or "/bin/" in directory + "/" or name.endswith(".oat") or name.endswith(".odex"))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(4, (os.cpu_count() or 8) - 2)) as pool:
        results = [(path, result) for path, result in zip(paths, pool.map(count, paths)) if result]
    with_uses = sorted((item for item in results if item[1][0]), key=lambda item: -item[1][0])
    print("files=%d with_uses=%d instructions_with_uses=%d" % (len(results), len(with_uses), sum(item[1][0] for item in with_uses)))
    for path, (uses, _, symbols) in with_uses[:top]:
        print("%7d %s  %s" % (uses, path[len(tree):], " ".join("%s:%d" % (name.decode(errors="replace")[:40], number) for name, number in symbols)))


if __name__ == "__main__":
    main()
