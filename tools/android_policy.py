"""Capture access vectors from the reference Android kernel."""
import re
import subprocess


SEAPP_CONTEXTS = (
    "system/etc/selinux/plat_seapp_contexts",
    "system_ext/etc/selinux/system_ext_seapp_contexts",
    "product/etc/selinux/product_seapp_contexts",
    "vendor/etc/selinux/vendor_seapp_contexts",
    "odm/etc/selinux/odm_seapp_contexts",
)


def query_access(adb, index, requested, queries):
    queries = sorted(set(queries))
    if not queries:
        return {}
    input_rows = "\n".join(f"{source} {target}" for source, target in queries)
    script = f"""while read -r source target; do
  if exec 3<>/sys/fs/selinux/access; then
    if printf '%s %s {index} {requested:x}' "$source" "$target" >&3; then
      decision=
      read -r decision <&3 || true
      printf '%s %s %s\n' "$source" "$target" "$decision"
    fi
    exec 3>&-
  fi
done <<'NESTVANE_LABELS'
{input_rows}
NESTVANE_LABELS
"""
    result = subprocess.run(adb + ["shell", "sh"], input=script, text=True, capture_output=True, check=True)
    rows = {}
    context = r"u:(?:object_r|r):[A-Za-z0-9_]+:s0(?::c\d+(?:[,\.]c\d+)*)?"
    for line in result.stdout.splitlines():
        if re.fullmatch(rf"{context} {context}(?: [0-9a-f]+){{4}} [0-9]+ [0-9a-f]+", line):
            source, target, decision = line.split(maxsplit=2)
            rows[source, target] = decision
    if list(rows) != queries:
        raise SystemExit(f"Incomplete reference SELinux decisions for class {index}")
    return rows


def allowed(decision, mask):
    return int(decision.split()[0], 16) & mask


def capture_framework_app_policy(adb, remote, guest):
    candidates = set()
    for path in SEAPP_CONTEXTS:
        source = guest / path
        if not source.is_file():
            continue
        for line in source.read_text().splitlines():
            fields = line.partition("#")[0].split()
            if not fields or fields[0] == "neverallow" or "isSystemServer=true" in fields or "fromRunAs=true" in fields:
                continue
            attributes = dict(word.split("=", 1) for word in fields if "=" in word)
            if "user" in attributes and re.fullmatch(r"[a-z0-9_]+", attributes.get("domain", "")):
                candidates.add(attributes["domain"])
    if not candidates:
        raise SystemExit("Framework app policy requires the reference seapp contexts")

    process = "/sys/fs/selinux/class/process"
    class_index = int(remote("cat", process + "/index"))
    setcurrent = 1 << (int(remote("cat", process + "/perms/setcurrent")) - 1)
    dyntransition = 1 << (int(remote("cat", process + "/perms/dyntransition")) - 1)
    zygote = "u:r:zygote:s0"
    masks = setcurrent | dyntransition
    # Check each MLS layout used by the stock seapp specialization against the reference.
    ranges = ("s0", "s0:c159,c256", "s0:c512,c768", "s0:c159,c256,c512,c768", "s0:c160,c256,c513,c768")
    labeled = lambda domain, level: f"u:r:{domain}:{level}"
    decisions = query_access(adb, class_index, masks,
                             [(zygote, zygote)] + [(zygote, labeled(d, r)) for d in candidates for r in ranges])
    if not allowed(decisions[zygote, zygote], setcurrent):
        raise SystemExit("Reference policy denies zygote setcurrent")
    admitted = sorted(d for d in candidates if all(allowed(decisions[zygote, labeled(d, r)], dyntransition) for r in ranges))
    if not admitted:
        raise SystemExit("Reference policy permits no zygote app transitions")

    metadata = guest / "nestvane-metadata"
    (metadata / "selinux-app-transition.txt").write_text("\n".join(admitted) + "\n")
    for name in ("service", "hwservice"):
        manifest = metadata / f"selinux-{name}-access.txt"
        header, *entries = manifest.read_text().splitlines()
        index, *positions = map(int, header.split())
        mask = sum(1 << (bit - 1) for bit in positions)
        original = {}
        for line in entries:
            source, target, decision = line.split(maxsplit=2)
            original[source, target] = decision
        targets = {target for source, target in original}
        captured = query_access(adb, index, mask, (
            (labeled(domain, level), target)
            for domain in admitted for level in ranges for target in targets
        ))
        for domain in admitted:
            name_without_categories = labeled(domain, "s0")
            for target in targets:
                decision = captured[name_without_categories, target]
                if any(allowed(captured[labeled(domain, level), target], mask) != allowed(decision, mask)
                       for level in ranges[1:]):
                    raise SystemExit(f"SELinux {name} decision depends on MLS categories for {domain} -> {target}")
                original[name_without_categories, target] = decision
        manifest.write_text(header + "\n" + "".join(
            f"{source} {target} {decision}\n" for (source, target), decision in sorted(original.items())
        ))
    print(f"Captured reference zygote transitions and Binder service decisions for {len(admitted)} app domains", flush=True)
