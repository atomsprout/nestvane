#!/usr/bin/env python3
"""Sums per-process memory on a rooted reference Android to size the footprint that an iOS host would
charge when every guest process is a copy-on-write mapping inside one task.

iOS charges an anonymous page once per mapping that touched it, which is RssAnon. Linux charges it
once in total, which is Pss_Anon. The difference is the cost of that accounting.

Linux fork copies the parent's page table entries, so RssAnon also counts inherited pages the child
never touched. vm_remap enters a page in the child only when it is touched. The per-mapping Referenced
field separates the two as long as nothing has aged the accessed bits, so run this with swap off and
no reclaim (the script prints the counters that show it).

usage: guest-memory.py <adb serial>
"""
import re
import subprocess
import sys

SCRIPT = r'''
for p in /proc/[0-9]*; do
  n=$(tr "\0" " " < $p/cmdline 2>/dev/null | cut -d" " -f1)
  [ -z "$n" ] && continue
  s=$(grep -E "^(PPid|RssAnon|RssFile|RssShmem)" $p/status | tr -s "\t " " " | tr "\n" " ")
  r=$(grep -E "^(Pss_Anon|Pss_Shmem|Pss_File|Swap|SwapPss):" $p/smaps_rollup 2>/dev/null | tr -s " " | tr "\n" " ")
  echo "${p#/proc/} $n | $s| $r"
done
'''


def shell(serial, command):
    return subprocess.run(["adb", "-s", serial, "shell", command], capture_output=True, text=True, check=True).stdout


def main():
    serial = sys.argv[1]
    subprocess.run(["adb", "-s", serial, "root"], capture_output=True, check=True)
    subprocess.run(["adb", "-s", serial, "wait-for-device"], check=True)
    print("fingerprint=" + shell(serial, "getprop ro.build.fingerprint").strip())
    print("page_size=" + shell(serial, "getconf PAGE_SIZE").strip())
    print("resumed=" + shell(serial, "dumpsys activity activities | grep -m1 ResumedActivity").strip())
    print("uptime_s=" + shell(serial, "cut -d' ' -f1 /proc/uptime").strip())
    meminfo = dict(re.findall(r"(\w+):\s+(\d+) kB", shell(serial, "cat /proc/meminfo")))
    print("meminfo_mib " + " ".join(f"{key}={int(meminfo[key]) / 1024:.0f}" for key in
                                    ("MemTotal", "MemFree", "MemAvailable", "AnonPages", "Shmem", "Mapped", "SwapTotal", "SwapFree")))

    rows = []
    for line in shell(serial, SCRIPT).splitlines():
        row = {key: int(value) for key, value in re.findall(r"(\w+): (\d+) kB", line)}
        if "RssAnon" not in row or not re.match(r"\d+ ", line):
            continue  # kernel thread or shell noise
        row["pid"] = int(line.split(" ", 1)[0])
        row["name"] = line.split(" ", 2)[1]
        parent = re.search(r"PPid: (\d+)", line)
        row["parent"] = int(parent.group(1)) if parent else 0
        rows.append(row)

    zygotes = {row["pid"] for row in rows if row["name"] in ("zygote64", "zygote", "webview_zygote")}
    groups = (
        ("all", rows),
        ("zygote_and_children", [row for row in rows if row["parent"] in zygotes or row["pid"] in zygotes]),
        ("native", [row for row in rows if row["parent"] not in zygotes and row["pid"] not in zygotes]),
    )
    keys = ("RssAnon", "Pss_Anon", "Swap", "SwapPss", "RssShmem", "Pss_Shmem", "RssFile", "Pss_File")
    for label, group in groups:
        totals = " ".join(f"{key}={sum(row.get(key, 0) for row in group) / 1024:.0f}" for key in keys)
        print(f"total_mib group={label} processes={len(group)} {totals}")
    for row in sorted(rows, key=lambda row: -row["RssAnon"]):
        if row["RssAnon"] < 1024:
            break
        print(f"process pid={row['pid']} name={row['name']} zygote_child={int(row['parent'] in zygotes)} "
              + " ".join(f"{key}={row.get(key, 0) / 1024:.1f}" for key in keys))


    touched(serial, {row["pid"]: row for row in rows}, zygotes)


SMAPS = r'''
for p in /proc/[0-9]*; do
  echo "@@ ${p#/proc/}"
  cat $p/smaps 2>/dev/null
done
'''


def touched(serial, rows, zygotes):
    """Per process: anonymous pages resident, shared with another process, and referenced since fork."""
    vmstat = dict(line.split() for line in shell(serial, "cat /proc/vmstat").splitlines())
    print("aging " + " ".join(f"{key}={vmstat.get(key, '?')}" for key in ("pgscan_kswapd", "pgscan_direct", "pgsteal_kswapd"))
          + " SwapTotal_kb=" + re.search(r"SwapTotal:\s+(\d+)", shell(serial, "cat /proc/meminfo")).group(1))
    totals = {}
    pid = None
    header = re.compile(r"^[0-9a-f]+-[0-9a-f]+ \S+ \S+ \S+ \S+\s*(.*)$")
    area = {}

    def close():
        if pid is None or not area or pid not in rows:
            return
        anonymous, resident, referenced = area.get("Anonymous", 0), area.get("Rss", 0), area.get("Referenced", 0)
        if not anonymous:
            return
        total = totals.setdefault(pid, dict.fromkeys(("anon", "low", "high", "shared", "file_private"), 0))
        total["anon"] += anonymous
        # File pages in the same mapping make the split ambiguous, so keep both bounds.
        total["low"] += max(0, referenced - (resident - anonymous))
        total["high"] += min(anonymous, referenced)
        if area["path"].startswith("/"):
            total["file_private"] += anonymous
        else:
            total["shared"] += area.get("Shared_Clean", 0) + area.get("Shared_Dirty", 0)

    for line in shell(serial, SMAPS).splitlines():
        if line.startswith("@@ "):
            close()
            pid, area = int(line[3:]), {}
            continue
        match = header.match(line)
        if match:
            close()
            area = {"path": match.group(1)}
            continue
        field = re.match(r"(\w+):\s+(\d+) kB", line)
        if field and area:
            area[field.group(1)] = int(field.group(2))
    close()

    def report(label, pids):
        group = [totals[pid] for pid in pids if pid in totals]
        if not group:
            return
        print(f"touched_mib group={label} processes={len(group)} "
              + " ".join(f"{key}={sum(total[key] for total in group) / 1024:.0f}" for key in ("anon", "low", "high", "shared", "file_private"))
              + f" Pss_Anon={sum(rows[pid].get('Pss_Anon', 0) for pid in pids if pid in totals) / 1024:.0f}")

    children = [pid for pid, row in rows.items() if row["parent"] in zygotes]
    report("all", list(rows))
    report("zygote_children", children)
    report("zygotes", list(zygotes))
    report("native", [pid for pid, row in rows.items() if row["parent"] not in zygotes and pid not in zygotes])
    for pid in sorted(totals, key=lambda pid: -totals[pid]["anon"])[:40]:
        total = totals[pid]
        print(f"touched pid={pid} name={rows[pid]['name']} " + " ".join(f"{key}={value / 1024:.1f}" for key, value in total.items())
              + f" Pss_Anon={rows[pid].get('Pss_Anon', 0) / 1024:.1f}")


if __name__ == "__main__":
    main()
