#!/usr/bin/env python3
"""Propose link-level text pads that put a new kernel's hot functions back on
the I-cache colours of the previous benchmarked release.

A PROPOSAL, never an acceptance: the output is a report and a candidate
``pads.patch``; it becomes admissible only after ``text_layout.py record``,
the static checks it enforces, and a paired bench against the unpadded build
(see scripts/imem/README.md, "Text placement").

Inputs
  --old-vmlinux    the previous release's production vmlinux (benchmarked)
  --new-vmlinux    the new release's production vmlinux (same policy, no pads)
  --new-tree       the new release's build tree (Makefiles are read there)
  --link-map       GNU ld map of the new release (VMLINUX_LINK_MAP=1) — gives
                   the owning object of each .text.<function> section
  --tx / --rx      decoded profile JSONs (profile_decode.py) of the new
                   release, TX and RX workloads; used to pick the hot set and
                   to classify zones
  --report-json    the I-MEM policy report of the new build (residents are
                   excluded — their addresses are fixed by the window)

Method
  1. Hot SDRAM functions: >= --min-share of the mean TX work samples, not an
     I-MEM resident. Each gets delta = addr_new - addr_old.
  2. Zones: consecutive hot functions in link order sharing one delta; a zone
     boundary that falls inside an object is reported but cannot be padded
     (an intra-object shear needs a source-order change, not a pad).
  3. Classification per zone: TX/RX = sum of TX samples / sum of RX samples.
     Zones with TX/RX >= --tx-rx-ratio are restored to the old colours; the
     others are kept EXACTLY on the new release's colours (pads are recomputed
     so no zone lands in between). --restore-all restores every zone.
  4. Pads: before each zone's first object, pad = (wanted - cumulative) mod
     8192, realised as a pad_<object>.S object inserted in that Makefile's
     obj-y/lib-y line ahead of the target (or ahead of the subdirectory
     entry in the parent Makefile). Composite objects (foo-y := a.o b.o) are
     handled by inserting in the composite list.

Alignment residuals (the next section's alignment after a pad) show up as
8/12-byte misses in text_layout.py's colours; adjust the sizes and rebuild —
one iteration was enough on 6.18.51.
"""
import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys


def nm_table(vmlinux, cross):
    out = subprocess.run([cross + "nm", "-S", "-n", vmlinux], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "tTwW":
            table.setdefault(p[3], (int(p[0], 16), int(p[1], 16)))
        elif len(p) == 3 and p[1] in "tTwW":
            table.setdefault(p[2], (int(p[0], 16), 0))
    return table


def link_map_sections(path):
    """[(addr, size, object, section)] for .text.* input sections, two-line entries handled."""
    one = re.compile(r"^\s*(\.text\S*)\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$")
    name_only = re.compile(r"^\s*(\.text\S*)\s*$")
    rest = re.compile(r"^\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$")
    out, pending = [], None
    for line in open(path):
        m = one.match(line)
        if m:
            out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(4), m.group(1)))
            pending = None
            continue
        m = name_only.match(line)
        if m:
            pending = m.group(1)
            continue
        m = rest.match(line)
        if m and pending:
            out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3), pending))
        pending = None
    return sorted(o for o in out if o[1] and o[0] >= 0x80000000)


def load_profiles(paths):
    acc = {}
    for path in paths:
        for f in json.load(open(path))["functions"]:
            acc[f["name"]] = acc.get(f["name"], 0.0) + f["samples"] / len(paths)
    return acc


def makefile_insertion(tree, obj_rel, pad_obj):
    """Return (makefile, old_text, new_text) inserting pad_obj ahead of obj_rel."""
    d, base = os.path.split(obj_rel)
    mk = os.path.join(d, "Makefile")
    text = open(os.path.join(tree, mk)).read()
    token = re.compile(r"(^[^\n#]*-(?:y|objs)\s*[:+]?=[^\n]*?)(?<![\w.-])" + re.escape(base) + r"(?![\w.-])", re.M)
    m = token.search(text)
    if m and m.group(1).lstrip().startswith("lib-"):
        # lib-y objects go into an archive whose members are pulled on demand:
        # their link order is not the Makefile order. Libraries link after every
        # obj-y directory, so a pad appended to the last obj-y directory (net/)
        # shifts the whole library block as one zone.
        return "net/Makefile", None, "obj-y += " + pad_obj
    if m:
        old = m.group(0)
        new = old[:m.end(1)] + old[m.end(1):].replace(base, pad_obj + " " + base, 1)
        return mk, old, new
    # the object may be listed on a continuation line: search a bare token on any line inside an assignment block
    for mm in re.finditer(r"(?<![\w./-])" + re.escape(base) + r"(?![\w.-])", text):
        line_start = text.rfind("\n", 0, mm.start()) + 1
        line = text[line_start:text.find("\n", mm.start())]
        if not line.lstrip().startswith("#"):
            return mk, line, line.replace(base, pad_obj + " " + base, 1)
    # not listed here: the directory is pulled as a subdir entry of the parent Makefile
    parent, sub = os.path.split(d)
    pmk = os.path.join(parent, "Makefile")
    ptext = open(os.path.join(tree, pmk)).read()
    for line in ptext.split("\n"):
        if re.search(r"(?<![\w./-])" + re.escape(sub) + r"/(?![\w])", line) and not line.lstrip().startswith("#"):
            return pmk, line, "obj-y += " + pad_obj + "\n" + line
    raise SystemExit(f"propose_text_pads: no Makefile line lists {obj_rel} nor {sub}/")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old-vmlinux", required=True)
    ap.add_argument("--new-vmlinux", required=True)
    ap.add_argument("--new-tree", required=True)
    ap.add_argument("--link-map", required=True)
    ap.add_argument("--tx", action="append", required=True)
    ap.add_argument("--rx", action="append", default=[])
    ap.add_argument("--report-json", required=True)
    ap.add_argument("--min-share", type=float, default=0.0025)
    ap.add_argument("--tx-rx-ratio", type=float, default=2.0)
    ap.add_argument("--restore-all", action="store_true")
    ap.add_argument("--group-by", choices=("delta", "object"), default="delta")
    ap.add_argument("--delta-rule", choices=("weighted", "first"), default="weighted")
    ap.add_argument("--cross", default="mips-lexra-linux-musl-")
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    old, new = nm_table(args.old_vmlinux, args.cross), nm_table(args.new_vmlinux, args.cross)
    sections = link_map_sections(args.link_map)
    owner = {sn: o for _a, _s, o, sn in sections}
    residents = {e["section"].replace(".text.", "") for e in json.load(open(args.report_json))["entries"]}
    tx, rx = load_profiles(args.tx), load_profiles(args.rx) if args.rx else {}
    work = sum(tx.values())
    iram_new = new.get("__iram", (0x80330000, 0))[0]
    hot = sorted((new[k][0], k) for k, v in tx.items()
                 if v / work >= args.min_share and k not in residents and k in old and k in new
                 and new[k][0] < iram_new)
    rows = []
    for addr, k in hot:
        rows.append({"function": k, "object": owner.get(".text." + k, "?"), "old": old[k][0], "new": addr,
                     "delta": addr - old[k][0], "tx": tx.get(k, 0.0), "rx": rx.get(k, 0.0)})
    # Objects are the unit of padding (a pad can only sit between objects).
    # An object's delta is the one carrying most of its TX samples; a second
    # delta inside the object is an intra-object shear no pad can fix.
    # Consecutive objects sharing a delta form one zone (--group-by delta),
    # classified on their pooled samples; --group-by object decides per object.
    rows = [r for r in rows if r["object"] != "?"]
    objects = []
    for r in rows:
        if objects and objects[-1]["object"] == r["object"]:
            objects[-1]["rows"].append(r)
        else:
            objects.append({"object": r["object"], "rows": [r]})
    for o in objects:
        weight = collections.Counter()
        for r in o["rows"]:
            weight[r["delta"]] += r["tx"]
        o["delta"] = o["rows"][0]["delta"] if args.delta_rule == "first" else weight.most_common(1)[0][0]
        o["others"] = sorted(set(weight) - {o["delta"]})
    zones = []
    for o in objects:
        if args.group_by == "delta" and zones and zones[-1]["delta"] == o["delta"]:
            zones[-1]["rows"].extend(o["rows"]); zones[-1]["others"] += o["others"]
        else:
            zones.append({"first_object": o["object"], "delta": o["delta"], "rows": list(o["rows"]), "others": list(o["others"])})
    for z in zones:
        z["tx"] = sum(r["tx"] for r in z["rows"]); z["rx"] = sum(r["rx"] for r in z["rows"])
        z["ratio"] = z["tx"] / z["rx"] if z["rx"] else float("inf")
        z["restore"] = args.restore_all or z["ratio"] >= args.tx_rx_ratio
        if z["others"]:
            z["note"] = f"intra-object shear: functions at delta {sorted(set(z['others']))} cannot be padded at link level"
        del z["others"]
    # pads
    cumulative, pads = 0, []
    for z in zones:
        wanted = (-z["delta"]) % 8192 if z["restore"] else 0
        pad = (wanted - cumulative) % 8192
        if pad:
            if pad % 4:
                pad += 4 - pad % 4
            pads.append({"before": z["first_object"], "size": pad, "wanted": wanted, "restore": z["restore"]})
            cumulative += pad
    # emit
    os.makedirs(args.out_dir, exist_ok=True)
    report = {"work_samples": work, "min_share": args.min_share, "tx_rx_ratio": args.tx_rx_ratio,
              "zones": [{k: v for k, v in z.items() if k != "rows"} | {"functions": [r["function"] for r in z["rows"]]} for z in zones],
              "pads": pads, "total_pad_bytes": sum(p["size"] for p in pads)}
    json.dump(report, open(os.path.join(args.out_dir, "proposal.json"), "w"), indent=1)
    patch, makefiles = [], {}
    for i, p in enumerate(pads):
        obj = p["before"]; d = os.path.dirname(obj); name = "pad_" + os.path.splitext(os.path.basename(obj))[0]
        src = (f"\t.section .text.__text_pad_{i:04d},\"ax\",@progbits\n\t.globl __text_pad_{i:04d}\n"
               f"\t.type __text_pad_{i:04d},@function\n__text_pad_{i:04d}:\n\t.space {p['size']}\n"
               f"\t.size __text_pad_{i:04d}, .-__text_pad_{i:04d}\n")
        rel = os.path.join(d, name + ".S")
        patch.append(f"--- /dev/null\n+++ b/{rel}\n@@ -0,0 +1,{src.count(chr(10))} @@\n" + "".join("+" + l + "\n" for l in src.rstrip("\n").split("\n")))
        mk, old_text, new_text = makefile_insertion(args.new_tree, obj, name + ".o")
        if old_text is None:                      # library member: pad lands in net/
            d = "net"; rel = os.path.join(d, name + ".S"); patch.pop()
            patch.append(f"--- /dev/null\n+++ b/{rel}\n@@ -0,0 +1,{src.count(chr(10))} @@\n" + "".join("+" + l + "\n" for l in src.rstrip("\n").split("\n")))
        makefiles.setdefault(mk, open(os.path.join(args.new_tree, mk)).read())
        makefiles[mk] = (makefiles[mk].rstrip("\n") + "\n" + new_text + "\n") if old_text is None else makefiles[mk].replace(old_text, new_text, 1)
    for mk, text in makefiles.items():
        orig = os.path.join(args.new_tree, mk)
        tmp = os.path.join(args.out_dir, "mk.tmp"); open(tmp, "w").write(text)
        d = subprocess.run(["diff", "-u", "--label", "a/" + mk, "--label", "b/" + mk, orig, tmp], capture_output=True, text=True).stdout
        patch.append(d); os.unlink(tmp)
    open(os.path.join(args.out_dir, "pads.patch"), "w").write("".join(patch))
    print(f"hot SDRAM functions: {len(rows)}; zones: {len(zones)} "
          f"({sum(z['restore'] for z in zones)} restored, {sum(not z['restore'] for z in zones)} kept, "
          f"{sum('note' in z for z in zones)} with an intra-object shear); pads: {len(pads)}, {report['total_pad_bytes']} bytes")
    for z in zones:
        print(f"  delta {z['delta']:+6d} TX/RX {z['ratio']:6.2f} {'RESTORE' if z['restore'] else 'keep   '} "
              f"{z['first_object']}: {', '.join(r['function'] for r in z['rows'][:4])}{' …' if len(z['rows']) > 4 else ''}"
              + (f"  [{z['note']}]" if z.get('note') else ""))
    print(f"proposal written: {args.out_dir}/proposal.json, pads.patch — a proposal only: record with text_layout.py "
          f"after a clean build, then bench paired against the unpadded build before any use.")


if __name__ == "__main__":
    main()
