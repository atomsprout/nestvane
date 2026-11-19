#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $# != 1 || ! -r "$1" ]]; then
    echo "Usage: tools/validate-linux.sh /absolute/path/to/ARM64-Linux-kernel" >&2
    exit 2
fi

tools/build-fixtures.sh
xcrun clang --target=aarch64-linux-gnu -ffreestanding -fno-stack-protector -O2     -Wall -Wextra -Werror -c tools/linux-oracle.c -o generated/linux-oracle.o
ld.lld -m aarch64elf -static -e _start generated/linux-oracle.o -o generated/linux-init

python3 - <<'PY'
import gzip
import stat
from pathlib import Path

with gzip.open('generated/linux-initramfs.cpio.gz', 'wb') as archive:
    position = 0
    inode = 0

    def write(data):
        global position
        archive.write(data)
        position += len(data)

    def pad():
        write(b'\0' * (-position % 4))

    def entry(name, mode=0, data=b'', major=0, minor=0):
        global inode
        inode += 1
        filename = name.encode() + b'\0'
        fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(filename), 0)
        write(b'070701' + ''.join(f'{value:08x}' for value in fields).encode())
        write(filename)
        pad()
        write(data)
        pad()

    entry('dev', stat.S_IFDIR | 0o755)
    entry('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
    entry('init', stat.S_IFREG | 0o755, Path('generated/linux-init').read_bytes())
    for name in ('arithmetic.elf', 'memory.elf'):
        entry(name, stat.S_IFREG | 0o755, (Path('generated/guests') / name).read_bytes())
    entry('TRAILER!!!')
PY

python3 - "$1" <<'PY'
import subprocess
import sys

command = [
    'qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a57', '-m', '256M',
    '-net', 'none', '-nodefaults', '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
    '-no-reboot', '-kernel', sys.argv[1], '-initrd', 'generated/linux-initramfs.cpio.gz',
    '-append', 'console=ttyAMA0 rdinit=/init quiet loglevel=3',
]
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40, check=False)
sys.stdout.buffer.write(result.stdout)
if result.returncode or b'NESTVANE_LINUX_ORACLE PASS' not in result.stdout:
    raise SystemExit(1)
PY
