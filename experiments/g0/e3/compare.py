#!/usr/bin/env python3
"""Runs the same toybox scripts through the rewritten binary on this Mac and through the original ELF
on a reference Linux kernel (an Android emulator reached with adb), then compares output and time.

usage: compare.py <build dir> <adb serial> [--runs N]
       compare.py --export <file>     writes the scripts for the iPhone run (experiments/g0/run.sh G0Guest)
The build dir holds toybox.elf, toybox-plain.elf, mono, toybox-hostlr and toybox-guestlr (see build.sh).
"""
import hashlib
import os
import shutil
import subprocess
import sys
import time
import functools

print = functools.partial(print, flush=True)

DEVICE_DIR = "/data/local/tmp/e3"
TIMEOUT = 120

SETUP = """
seq 1 2000000 > nums
i=0; while [ $i -lt 19 ]; do cat nums; i=$((i+1)); done > big
seq 1 60000 | sha512sum > small; seq 1 50000 >> small
"""

FUNCTIONAL = {
    "arithmetic": "echo $((6*7)) $((1<<20)) $((100/7)) $((100%7))",
    "pipeline": "seq 1 20 | grep 7 | wc -l",
    "three_stage": "seq 1 300000 | sort -rn | tail -3",
    "substitution": "x=$(seq 1 5 | tac); echo $x; echo ${#x}",
    "files": "mkdir d && cd d && for i in 1 2 3; do echo line$i > f$i; done; ls; cat f* | wc -c; cd ..; rm -r d; ls d 2>/dev/null; echo $?",
    "exec_self": "./toybox echo exec-ok; ./toybox seq 3 | ./toybox tac; ./toybox false; echo $?",
    "hashes": "sha256sum small; md5sum small; sha1sum small; cksum small",
    "sed": "seq 1 50000 | sed 's/7/x/g' | sha1sum",
    "xargs": "seq 1 20000 | xargs -n 500 ./toybox echo | wc",
    "base64": "base64 small | sha1sum; base64 small | base64 -d | cmp - small && echo same",
    "gzip": "gzip -c small | wc -c; gzip -c small | zcat | sha256sum",
    "printf": "printf '%5.2f %s %x %e\\n' 3.14159 ok 255 12345.678",
    "bc": "echo 'scale=40; 4*a(1)' | bc -l; echo '2^200' | bc",
    "sort_unique": "seq 1 5000 | sed 's/[0-9]$//' | sort | uniq -c | sort -rn | tail -4",
    "loop_forks": "i=0; while [ $i -lt 100 ]; do echo $i | cat > /dev/null; i=$((i+1)); done; echo $i",
    "exit_status": "(exit 3); echo $?; false || echo recovered; true && echo ok",
    "factor": "factor 600851475143 9007199254740881 1000000007",
    "find": "mkdir -p t/a/b t/c && echo x > t/a/b/f && find t | sort && rm -r t",
    "date": "date -u -d @86400 +%Y-%m-%d",
    "od_cut": "od -An -tx1 -N 32 small; cut -c1-3 nums | sort -u | wc -l",
}

BENCHMARKS = {
    "sha256sum 251 MiB": "sha256sum big",
    "wc 251 MiB": "wc big",
    "gzip 14 MiB": "gzip -c nums | wc -c",
    "sort 2M lines": "sort -rn nums | tail -1",
    "sed 2M lines": "sed 's/1\\(2*\\)3/x\\1y/g' nums | md5sum",
    "grep 2M lines": "grep -c '7.*7' nums",
    "factor": "factor 9007199254740881",
    "shell loop 300k": "i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done; echo $i",
    "300 forks": "i=0; while [ $i -lt 300 ]; do echo $i | cat > /dev/null; i=$((i+1)); done; echo $i",
    "bc pi 2000 digits": "echo 'scale=2000; 4*a(1)' | bc -l | md5sum",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()[:12]


class Mac:
    def __init__(self, build, variant):
        self.name = "mac_" + variant
        self.work = os.path.join(build, "work")
        self.program = os.path.join(build, "toybox-" + variant)

    def run(self, script_path):
        begin = time.perf_counter()
        try:
            result = subprocess.run([self.program, "./toybox", "toybox", "sh", script_path], cwd=self.work, capture_output=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return b"", 124, TIMEOUT * 1000
        return result.stdout, result.returncode, (time.perf_counter() - begin) * 1000


class Linux:
    def __init__(self, serial, binary):
        self.name = "linux_" + binary
        self.serial, self.binary = serial, binary

    def run(self, script_path):
        # The binary dispatches on its own name, so each variant lives in its own directory as "toybox".
        command = (f"cd {DEVICE_DIR}/{self.binary} && S=$(../mono); timeout {TIMEOUT} ./toybox sh {script_path} > ../out.bin 2>/dev/null; "
                   f"R=$?; E=$(../mono); echo $R $(( (E - S) / 1000 ))")
        status = subprocess.run(["adb", "-s", self.serial, "shell", command], capture_output=True, text=True).stdout.split()
        output = subprocess.run(["adb", "-s", self.serial, "exec-out", f"cat {DEVICE_DIR}/out.bin"], capture_output=True).stdout
        return output, int(status[0]), int(status[1]) / 1000


def export(path):
    records = [("setup", SETUP)] + [("f " + name, text) for name, text in FUNCTIONAL.items()] + [("b " + name, text) for name, text in BENCHMARKS.items()]
    open(path, "wb").write(b"".join(name.encode() + b"\0" + text.encode() + b"\0" for name, text in records))


def main():
    if sys.argv[1] == "--export":
        return export(sys.argv[2])
    build, serial = os.path.abspath(sys.argv[1]), sys.argv[2]
    runs = int(sys.argv[sys.argv.index("--runs") + 1]) if "--runs" in sys.argv else 5
    work = os.path.join(build, "work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "scripts"))
    shutil.copy(os.path.join(build, "toybox.elf"), os.path.join(work, "toybox"))
    scripts = {"setup": SETUP}
    scripts.update({"f_" + name: text for name, text in FUNCTIONAL.items()})
    scripts.update({"b_%d" % index: text for index, text in enumerate(BENCHMARKS.values())})
    for name, text in scripts.items():
        open(os.path.join(work, "scripts", name), "w").write(text + "\n")

    adb = ["adb", "-s", serial]
    subprocess.run(adb + ["shell", f"rm -rf {DEVICE_DIR}; mkdir -p {DEVICE_DIR}/toybox {DEVICE_DIR}/toybox-plain"], check=True)
    for binary in ("toybox", "toybox-plain"):
        subprocess.run(adb + ["push", os.path.join(build, binary + ".elf"), f"{DEVICE_DIR}/{binary}/toybox"], check=True, capture_output=True)
        subprocess.run(adb + ["push", os.path.join(work, "scripts"), f"{DEVICE_DIR}/{binary}/scripts"], check=True, capture_output=True)
    subprocess.run(adb + ["push", os.path.join(build, "mono"), f"{DEVICE_DIR}/mono"], check=True, capture_output=True)
    subprocess.run(adb + ["shell", f"chmod 755 {DEVICE_DIR}/*/toybox {DEVICE_DIR}/mono"], check=True)

    targets = [Linux(serial, "toybox"), Linux(serial, "toybox-plain"), Mac(build, "hostlr"), Mac(build, "guestlr")]
    reference = targets[0]
    for target in targets:
        if isinstance(target, Linux) or target is targets[2]:  # both Mac variants share one work directory
            print("setup %s exit=%d ms=%.0f" % ((target.name,) + target.run("scripts/setup")[1:]))

    failures = 0
    for name in FUNCTIONAL:
        expected = reference.run("scripts/f_" + name)[:2]
        verdicts = []
        for target in targets[1:]:
            actual = target.run("scripts/f_" + name)[:2]
            same = actual == expected
            failures += not same
            verdicts.append("%s=%s" % (target.name, "same" if same else "DIFFERENT(exit %d, %s)" % (actual[1], digest(actual[0]))))
        print("functional %-13s exit=%d bytes=%-6d sha=%s %s" % (name, expected[1], len(expected[0]), digest(expected[0]), " ".join(verdicts)))
    print("functional_total tests=%d comparisons=%d different=%d" % (len(FUNCTIONAL), len(FUNCTIONAL) * 3, failures))

    print("benchmark runs=%d, best time in ms" % runs)
    for index, name in enumerate(BENCHMARKS):
        best, outputs = {}, set()
        for _ in range(runs):
            for target in targets:
                output, status, elapsed = target.run("scripts/b_%d" % index)
                outputs.add((status, output))
                best[target.name] = min(best.get(target.name, 1e18), elapsed)
        native = best["linux_toybox"]
        print("benchmark %-18s outputs_agree=%d linux_plain=%.0f linux_reserved=%.0f mac_hostlr=%.0f mac_guestlr=%.0f "
              "reserved_over_plain=%.3f hostlr_over_native=%.3f guestlr_over_native=%.3f" % (
                  name, len(outputs) == 1, best["linux_toybox-plain"], native, best["mac_hostlr"], best["mac_guestlr"],
                  native / best["linux_toybox-plain"], best["mac_hostlr"] / native, best["mac_guestlr"] / native))


if __name__ == "__main__":
    main()
