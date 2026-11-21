#!/usr/bin/env python3
"""Writes the C table of translated modules that runtime.c links against.

usage: modules.py <output.c> <name>[=<guest path>@<base>] ...
A bare name is the single static module whose ELF is named on the runtime's command line.
"""
import sys

output, entries = sys.argv[1], []
for argument in sys.argv[2:]:
    name, _, rest = argument.partition("=")
    path, _, base = rest.rpartition("@")  # HIDL library names have an @ of their own
    entries.append((name, path, int(base, 0) if base else 0))

lines = ["#include <stdint.h>", "struct Module { const char* guest_path; const uint64_t* info; uint64_t base;",
         "                const char* host_path; const uint8_t* image; uint64_t end, headers; int32_t device; uint64_t inode; };"]
lines += ["extern const uint64_t guest_module_%s[];" % name for name, _, _ in entries]
lines.append("struct Module guest_modules[] = {")
for name, path, base in entries:
    lines.append("    {%s, guest_module_%s, 0x%xull}," % ('"%s"' % path if path else "0", name, base))
lines += ["};", "const int guest_module_count = %d;" % len(entries)]
open(output, "w").write("\n".join(lines) + "\n")
