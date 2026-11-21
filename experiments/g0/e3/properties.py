#!/usr/bin/env python3
"""Writes Android system properties as one property area file, the form Bionic reads when
/dev/__properties__ is a file rather than the directory that init and its property service keep.

usage: properties.py <output file> [<build.prop file> ...] [name=value ...]
Later arguments win. Values of 92 bytes or more are left out: they need the long-value form.
"""
import struct
import sys

MAGIC, VERSION, HEADER = 0x504F5250, 0xFC6ED0AB, 128
VALUE_MAX = 92


class Area:
    """The trie of prop_area.cpp: one node per dot-separated part of a name, siblings in a binary tree."""

    def __init__(self):
        self.data = bytearray(20)  # the root node, with an empty name

    def allocate(self, size):
        offset = len(self.data)
        self.data += bytes((size + 3) & ~3)
        return offset

    def node(self, name):
        offset = self.allocate(20 + len(name) + 1)
        struct.pack_into("<I", self.data, offset, len(name))
        self.data[offset + 20:offset + 20 + len(name)] = name
        return offset

    def child(self, parent, name):
        """The node for `name` among the children of `parent`, created if it is not there."""
        current, = struct.unpack_from("<I", self.data, parent + 16)
        if not current:
            current = self.node(name)
            struct.pack_into("<I", self.data, parent + 16, current)
        while True:
            length, = struct.unpack_from("<I", self.data, current)
            other = bytes(self.data[current + 20:current + 20 + length])
            if other == name:
                return current
            # Shorter names sort first, then byte order, as cmp_prop_name does.
            field = current + (8 if (len(name), name) < (length, other) else 12)
            following, = struct.unpack_from("<I", self.data, field)
            if not following:
                following = self.node(name)
                struct.pack_into("<I", self.data, field, following)
            current = following

    def set(self, name, value):
        node = 0
        for part in name.split(b"."):
            if not part:
                return
            node = self.child(node, part)
        info = self.allocate(4 + VALUE_MAX + len(name) + 1)
        struct.pack_into("<I", self.data, info, len(value) << 24)
        self.data[info + 4:info + 4 + len(value)] = value
        self.data[info + 96:info + 96 + len(name)] = name
        struct.pack_into("<I", self.data, node + 4, info)

    def image(self):
        return struct.pack("<IIII", len(self.data), 0, MAGIC, VERSION) + bytes(HEADER - 16) + bytes(self.data)


def main():
    properties = {}
    for argument in sys.argv[2:]:
        lines = [argument] if "=" in argument else open(argument, errors="replace").read().splitlines()
        for line in lines:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line and not line.startswith("import "):
                name, _, value = line.partition("=")
                properties[name.strip()] = value.strip()
    area = Area()
    skipped = 0
    for name, value in sorted(properties.items()):
        if len(value.encode()) >= VALUE_MAX:
            skipped += 1
            continue
        area.set(name.encode(), value.encode())
    open(sys.argv[1], "wb").write(area.image())
    print("properties=%d skipped_long=%d bytes=%d" % (len(properties) - skipped, skipped, HEADER + len(area.data)))


if __name__ == "__main__":
    main()
