#!/usr/bin/env python3
"""Record and verify a shipped text-placement layout (link-level pads).

A text layout is a set of never-executed pad objects (``pad_*.S``, one
``.space`` in a ``.text.__text_pad_NNNN`` section, kept alive by ``-u``)
inserted in ``obj-y`` order ahead of chosen objects, so that a version's hot
functions land on the I-cache colours (address modulo 8 KiB, 2-way 512-set
16-byte-line cache) the previous release was benchmarked with. The pads are
carried as ``layouts/<KERNEL_VERSION>/pads.patch`` and applied by
``build_kernel.sh`` to production builds only.

``record`` writes ``layout.json`` from an accepted build; ``verify`` recomputes
everything from a fresh build and fails on any drift:

- compiler and linker identification strings;
- sha256 of the built ``.config`` (minus the board's DTB selection) and of ``pads.patch``;
- address and size of every pad symbol;
- absolute address (hence colour) of every tracked function;
- the address-ordered sequence of text symbols below the I-MEM window
  (aliases grouped per address, pads excluded), as a sha256;
- no branch or jump into a pad, no object referencing a pad symbol, and no code
  that can fall through into a pad (the instruction before each pad, nops
  skipped, is the delay slot of jr/j/b/eret — never a call with link).

The guard is the build's, not CI's: a drift here means the benchmarked layout
is not the one being shipped, and the build must stop.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

PAD_RE = re.compile(r"^__text_pad_[0-9]{4}$")


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def config_sha256(path):
    """sha256 of the built .config minus the board's built-in DTB selection:
    a layout is a property of the text, and every board links the same text."""
    h = hashlib.sha256()
    for line in open(path, "rb"):
        if re.match(rb"^(# )?CONFIG_DTB_RTL8196E_", line):
            continue
        h.update(line)
    return h.hexdigest()


def toolchain_id(cross):
    gcc = run([cross + "gcc", "--version"]).splitlines()[0].strip()
    ld = run([cross + "ld", "--version"]).splitlines()[0].strip()
    return {"gcc": gcc, "ld": ld}


def symbols(vmlinux, cross):
    """{name: [(addr, size, type)...]} for text symbols, plus __iram."""
    out = run([cross + "nm", "-S", "-n", vmlinux])
    table = collections.defaultdict(list)
    iram = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in "tTwW":
            table[parts[3]].append((int(parts[0], 16), int(parts[1], 16), parts[2]))
        elif len(parts) == 3 and parts[1] in "tTwW":
            # assembly symbols without a .size directive (memset aliases, etc.)
            table[parts[2]].append((int(parts[0], 16), 0, parts[1]))
        if parts and parts[-1] == "__iram" and len(parts) in (3, 4):
            iram = int(parts[0], 16)
    if iram is None:
        raise SystemExit("verify_text_layout: __iram not found in vmlinux")
    return table, iram


def sequence_hash(table, iram):
    """sha256 of the address-ordered sequence of symbol-name groups below __iram."""
    groups = collections.defaultdict(set)
    for name, entries in table.items():
        if PAD_RE.match(name):
            continue
        for addr, _size, _t in entries:
            if 0x80000000 <= addr < iram:
                groups[addr].add(name)
    text = "\n".join(",".join(sorted(groups[a])) for a in sorted(groups))
    return hashlib.sha256(text.encode()).hexdigest(), len(groups)


def pads(table):
    return sorted(
        ({"symbol": n, "addr": f"0x{e[0][0]:08x}", "size": e[0][1]}
         for n, e in table.items() if PAD_RE.match(n)),
        key=lambda p: p["symbol"])


def tracked_entries(table, names):
    out = []
    for name in names:
        if name not in table:
            raise SystemExit(f"text layout: tracked function not in vmlinux: {name}")
        entries = table[name]
        if len(entries) != 1:
            raise SystemExit(f"text layout: tracked function is ambiguous in nm: {name}")
        addr, size, _t = entries[0]
        out.append({"function": name, "addr": f"0x{addr:08x}", "colour": addr % 8192, "size": size})
    return out


def references_into_pads(vmlinux, cross, pad_list, build_dir):
    """Count (a) branch/jump targets inside a pad in the linked kernel and
    (b) objects holding an undefined reference to a pad symbol — the only way
    code or data can point at a pad, since a pad object contains nothing else
    and its section can only be reached through its global symbol."""
    if not pad_list:
        return 0, 0
    branches = 0
    label = re.compile(r">:\s*$")
    target = re.compile(r"<__text_pad_[0-9]{4}(\+0x[0-9a-f]+)?>")
    dis = subprocess.Popen([cross + "objdump", "-d", "--no-show-raw-insn", vmlinux],
                           stdout=subprocess.PIPE, text=True)
    for line in dis.stdout:
        if target.search(line) and not label.search(line):
            branches += 1
    dis.wait()
    objects = []
    for root, _dirs, files in os.walk(build_dir):
        objects.extend(os.path.join(root, f) for f in files
                       if f.endswith(".o") and not f.startswith("pad_") and not f.endswith(".imem-pristine"))
    referrers = set()
    for i in range(0, len(objects), 200):
        out = subprocess.run([cross + "nm", "-A", "-u"] + objects[i:i + 200],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "__text_pad_" in line:
                referrers.add(line.split(":")[0])
    return branches, len(referrers)


# Transfers with no fall-through: jr/j/b/eret. Calls with link (jal, jalr,
# bal) are NOT acceptable — the callee returns to the instruction after the
# delay slot, i.e. into the pad.
UNCONDITIONAL = re.compile(r"^\s*[0-9a-f]+:\s+(jr|j|b|eret)\b")


def fallthrough_into_pads(vmlinux, cross, pad_list):
    """Pads whose preceding code could run into them sequentially.

    A pad is entered only if execution falls off the end of whatever precedes
    it. The linker fills the alignment gap before a section with zeros (MIPS
    nops), so we skip trailing nops and require the last real instruction of
    the preceding code to be the delay slot of a transfer that never falls
    through (jr/j/b/eret) — the normal end of a function is `jr ra` + slot.
    Calls with link (jal/jalr/bal) return after their slot and are refused."""
    bad = []
    for pad in pad_list:
        start = int(pad["addr"], 16)
        lo = start - 64
        out = run([cross + "objdump", "-d", "--no-show-raw-insn",
                   f"--start-address=0x{lo:x}", f"--stop-address=0x{start:x}", vmlinux])
        insns = [l for l in out.splitlines() if re.match(r"^\s*[0-9a-f]+:", l)]
        while insns and re.search(r"\bnop\s*$", insns[-1]):
            insns.pop()
        # insns[-1] is the delay slot, insns[-2] the transfer that owns it
        ok = len(insns) >= 2 and (UNCONDITIONAL.match(insns[-2]) or UNCONDITIONAL.match(insns[-1]))
        if not ok:
            bad.append(f"{pad['symbol']} at {pad['addr']}: preceded by " +
                       " | ".join(i.split(':', 1)[1].strip() for i in insns[-2:]))
    return bad


def snapshot(args, names):
    vmlinux = os.path.join(args.build_dir, "vmlinux")
    table, iram = symbols(vmlinux, args.cross)
    seq_hash, groups = sequence_hash(table, iram)
    snap = {
        "schema": "rtl8196e-text-layout-v1",
        "kernel_version": args.kernel_version,
        "toolchain": toolchain_id(args.cross),
        "config_sha256": config_sha256(os.path.join(args.build_dir, ".config")),
        "pads_patch_sha256": sha256_file(args.pads_patch) if args.pads_patch else None,
        "iram": f"0x{iram:08x}",
        "pads": pads(table),
        "tracked": tracked_entries(table, names),
        "sequence_sha256": seq_hash,
        "sequence_groups": groups,
    }
    return snap, vmlinux


def cmd_record(args):
    names = [l.split("\t")[0].strip() for l in open(args.tracked)
             if l.strip() and not l.startswith("#")]
    snap, vmlinux = snapshot(args, names)
    branches, words = references_into_pads(vmlinux, args.cross, snap["pads"], args.build_dir)
    if branches or words:
        raise SystemExit(f"text layout: refusing to record a layout with references into pads "
                         f"(branches {branches}, referring objects {words})")
    fall = fallthrough_into_pads(vmlinux, args.cross, snap["pads"])
    if fall:
        raise SystemExit("text layout: refusing to record a layout that code can fall into:\n  " + "\n  ".join(fall))
    snap["vmlinux_sha256"] = sha256_file(vmlinux)
    snap["intent"] = {l.split("\t")[0].strip(): l.split("\t")[1].strip()
                      for l in open(args.tracked) if l.strip() and not l.startswith("#") and "\t" in l}
    with open(args.out, "w") as stream:
        json.dump(snap, stream, indent=1)
    print(f"text layout recorded: {args.out} — {len(snap['pads'])} pads, "
          f"{len(snap['tracked'])} tracked functions, {snap['sequence_groups']} address groups")


def cmd_verify(args):
    expected = json.load(open(args.layout))
    names = [t["function"] for t in expected["tracked"]]
    args.kernel_version = expected["kernel_version"]
    snap, vmlinux = snapshot(args, names)
    failures = []
    if snap["toolchain"] != expected["toolchain"]:
        failures.append(f"toolchain: built with {snap['toolchain']}, layout recorded with {expected['toolchain']}")
    if snap["config_sha256"] != expected["config_sha256"]:
        failures.append("kernel .config differs from the recorded layout")
    if expected.get("pads_patch_sha256") and snap["pads_patch_sha256"] != expected["pads_patch_sha256"]:
        failures.append("pads.patch differs from the recorded layout")
    if snap["pads"] != expected["pads"]:
        failures.append("pad symbols differ (address or size): " +
                        "; ".join(f"{a['symbol']} {a['addr']}/{a['size']} vs {b['addr']}/{b['size']}"
                                  for a, b in zip(snap["pads"], expected["pads"]) if a != b) or
                        f"{len(snap['pads'])} pads built, {len(expected['pads'])} recorded")
    for got, want in zip(snap["tracked"], expected["tracked"]):
        if got["addr"] != want["addr"]:
            failures.append(f"{want['function']}: at {got['addr']} (colour {got['colour']}), "
                            f"recorded {want['addr']} (colour {want['colour']})")
    if snap["sequence_sha256"] != expected["sequence_sha256"]:
        failures.append(f"text symbol sequence differs ({snap['sequence_groups']} groups vs "
                        f"{expected['sequence_groups']} recorded)")
    branches, words = references_into_pads(vmlinux, args.cross, snap["pads"], args.build_dir)
    if branches or words:
        failures.append(f"references into pads: {branches} branch/jump targets, {words} objects with an undefined reference")
    for f in fallthrough_into_pads(vmlinux, args.cross, snap["pads"]):
        failures.append("code can fall through into a pad: " + f)
    if failures:
        print("TEXT LAYOUT GUARD FAILED — the built kernel is not the benchmarked layout:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        print("  Re-run the placement campaign (scripts/imem/propose_text_pads.py) or build with "
              "TEXT_LAYOUT_DISABLE=1 for an unpadded kernel.", file=sys.stderr)
        sys.exit(1)
    print(f"text layout OK: {len(snap['pads'])} pads, {len(snap['tracked'])} tracked functions on "
          f"their recorded colours, sequence {snap['sequence_sha256'][:12]}, no reference into pads, "
          f"no fall-through into a pad")


def cmd_diff(args):
    """Compare two builds' text: every symbol below __iram must sit at the same
    address in both, except the sections named in --allow (a swap's entering
    and leaving sections) and their local holes; pads must be identical."""
    ta, ia = symbols(os.path.join(args.reference, "vmlinux"), args.cross)
    tb, ib = symbols(os.path.join(args.candidate, "vmlinux"), args.cross)
    allow = set(n.replace(".text.", "") for n in (args.allow.split(",") if args.allow else []))
    # SDRAM text only: residents inside the I-MEM window are repacked by any
    # policy change, and the hole root symbols are numbered by policy order.
    top = min(ia, ib)
    hole = re.compile(r"^__imem_hole_[0-9]{4}$")
    moved, missing = [], []
    for name, entries in ta.items():
        if PAD_RE.match(name) or hole.match(name) or name in allow:
            continue
        if name not in tb:
            missing.append(name); continue
        aa = sorted(e[0] for e in entries if e[0] < top); bb = sorted(e[0] for e in tb[name] if e[0] < top)
        if aa != bb:
            moved.append((name, aa, bb))
    pa, pb = pads(ta), pads(tb)
    print(f"reference __iram {ia:08x}, candidate __iram {ib:08x}; pads {'identical' if pa == pb else 'DIFFER'}")
    print(f"symbols compared: {len(ta) - len(allow)}; moved outside the allowed set: {len(moved)}; missing: {len(missing)}")
    for name, aa, bb in moved[:40]:
        print(f"  MOVED {name}: {['%08x' % a for a in aa]} -> {['%08x' % b for b in bb]}")
    for name in allow:
        a = sorted(e[0] for e in ta.get(name, [])); b = sorted(e[0] for e in tb.get(name, []))
        print(f"  swapped {name}: {['%08x' % x for x in a]} -> {['%08x' % x for x in b]}")
    if moved or missing or pa != pb:
        sys.exit(1)
    print("layout diff OK: no SDRAM text moved except the swapped sections")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    rec = sub.add_parser("record")
    rec.add_argument("--build-dir", required=True)
    rec.add_argument("--pads-patch", required=True)
    rec.add_argument("--tracked", required=True, help="TSV: function<TAB>intent")
    rec.add_argument("--kernel-version", required=True)
    rec.add_argument("--cross", default="mips-lexra-linux-musl-")
    rec.add_argument("--out", required=True)
    rec.set_defaults(func=cmd_record)
    ver = sub.add_parser("verify")
    ver.add_argument("--build-dir", required=True)
    ver.add_argument("--layout", required=True)
    ver.add_argument("--pads-patch")
    ver.add_argument("--cross", default="mips-lexra-linux-musl-")
    ver.set_defaults(func=cmd_verify)
    dif = sub.add_parser("diff")
    dif.add_argument("--reference", required=True, help="production build dir")
    dif.add_argument("--candidate", required=True, help="candidate build dir")
    dif.add_argument("--allow", default="", help="comma-separated sections allowed to move")
    dif.add_argument("--cross", default="mips-lexra-linux-musl-")
    dif.set_defaults(func=cmd_diff)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
