#!/usr/bin/env python3
"""Map sampled functions to movable input sections and solve the I-MEM set."""

import argparse
import bisect
import hashlib
import importlib.util
import json
import math
import os
import random
import re
import subprocess

BUDGET = 15872
SECTION_RE = re.compile(
    r"\s*\[\s*(\d+)\]\s+(\.text\.\S+)\s+PROGBITS\s+\S+\s+\S+\s+"
    r"(\S+)\s+\S+\s+(\S*)\s+\d+\s+\d+\s+(\d+)")


def run(command):
    return subprocess.run(command, check=True, capture_output=True,
                          text=True).stdout


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def archive_objects(ar, build):
    return [os.path.join(build, name) for name in
            run([ar, "t", os.path.join(build, "vmlinux.a")]).splitlines()
            if name.endswith(".o") and os.path.exists(os.path.join(build, name))]


def object_index(readelf, objects, build):
    """Return symbol definitions and section geometry from linked objects."""
    sections = {}
    symbols = {}
    for start in range(0, len(objects), 100):
        current = None
        by_index = {}
        output = run([readelf, "-SsW"] + objects[start:start + 100])
        for line in output.splitlines():
            if line.startswith("File: "):
                current = os.path.relpath(line[6:].strip(), build)
                by_index = {}
                continue
            match = SECTION_RE.match(line)
            if match and current:
                number, name, size, flags, align = match.groups()
                by_index[number] = name
                sections[(current, name)] = {
                    "size": int(size, 16), "flags": flags,
                    "align": int(align),
                }
                continue
            fields = line.split()
            if (current and len(fields) >= 8 and fields[3] in ("FUNC", "NOTYPE")
                    and fields[6] in by_index and not fields[7].startswith(("$", ".L"))):
                symbols.setdefault(fields[7], []).append({
                    "object": current,
                    "section": by_index[fields[6]],
                    "offset": int(fields[1], 16),
                    "binding": fields[4],
                    "type": fields[3],
                })
    return sections, symbols


def choose_definition(names, symbol_index):
    found = []
    for name in names:
        found.extend((name, item) for item in symbol_index.get(name, []))
    unique = {(item["object"], item["section"], item["offset"]): (name, item)
              for name, item in found}
    definitions = list(unique.values())
    strong = [(name, item) for name, item in definitions
              if item["binding"] == "GLOBAL"]
    if len(strong) == 1:
        return strong[0], None
    if len(definitions) == 1:
        return definitions[0], None
    if not definitions:
        return None, "no unique per-function input section"
    return None, f"ambiguous symbol definition ({len(definitions)} copies)"


def link_map_index(path, build):
    """Map linked address intervals back to their exact input object/section."""
    intervals = []
    pending = None
    for line in open(path, encoding="utf-8", errors="replace"):
        fields = line.split()
        if len(fields) == 1 and fields[0].startswith(".text."):
            pending = fields[0]
            continue
        if (len(fields) >= 4 and fields[0].startswith(".text.")
                and fields[1].startswith("0x") and fields[2].startswith("0x")):
            section, address, size, obj = fields[0], fields[1], fields[2], fields[3]
        elif (pending and len(fields) >= 3 and fields[0].startswith("0x")
              and fields[1].startswith("0x")):
            section, address, size, obj = pending, fields[0], fields[1], fields[2]
        else:
            pending = None
            continue
        pending = None
        if not obj.endswith(".o"):
            continue
        obj = os.path.relpath(os.path.abspath(os.path.join(build, obj)), build)
        start, length = int(address, 16), int(size, 16)
        if length:
            intervals.append((start, start + length, obj, section))
    intervals.sort()
    return intervals, [item[0] for item in intervals]


def map_owner(address, mapped):
    intervals, starts = mapped
    index = bisect.bisect_right(starts, address) - 1
    if index >= 0 and intervals[index][0] <= address < intervals[index][1]:
        return intervals[index]
    return None


def load_profile(path):
    data = json.load(open(path, encoding="utf-8"))
    return data, {int(item["addr"], 16): item for item in data["functions"]}


def poisson(rng, value):
    if value <= 0:
        return 0.0
    if value > 50:
        return max(0.0, rng.gauss(value, math.sqrt(value)))
    limit = math.exp(-value)
    count = 0
    product = 1.0
    while product > limit:
        count += 1
        product *= rng.random()
    return float(count - 1)


def solve(candidates, values):
    """Exact ordered knapsack including the real alignment transition."""
    states = {0: (0.0, ())}
    for index, candidate in enumerate(candidates):
        updated = dict(states)
        for used, (value, chosen) in states.items():
            aligned = (used + candidate["align"] - 1) // candidate["align"] * candidate["align"]
            new_used = aligned + candidate["size"]
            if new_used > BUDGET:
                continue
            new_value = value + values[index]
            previous = updated.get(new_used)
            if previous is None or new_value > previous[0]:
                updated[new_used] = (new_value, chosen + (index,))
        states = updated
    used, (value, chosen) = max(states.items(), key=lambda item: (item[1][0], -item[0]))
    return used, value, chosen


def dynamic_sites(script_dir, cross, reference, config):
    path = os.path.join(script_dir, "scan_dynamic_code.py")
    spec = importlib.util.spec_from_file_location("imem_dynamic", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cfg = module.read_config(config)
    sites, scanned, absent, _ = module.scan(
        cross, os.path.join(reference, "vmlinux"),
        os.path.join(reference, "System.map"), cfg)
    return sites, dict(scanned), absent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--tx", action="append", required=True)
    parser.add_argument("--rx", action="append", default=[])
    parser.add_argument("--out", required=True)
    parser.add_argument("--release", required=True)
    parser.add_argument("--link-map", help="GNU ld map for the profiled reference")
    parser.add_argument("--cross", default="mips-lexra-linux-musl-")
    parser.add_argument("--replicates", type=int, default=200)
    parser.add_argument("--seed", type=int, default=8196)
    args = parser.parse_args()

    build = os.path.abspath(args.build_dir)
    reference = os.path.abspath(args.reference)
    readelf, ar, objcopy = (args.cross + tool for tool in
                            ("readelf", "ar", "objcopy"))
    objects = archive_objects(ar, build)
    print(f"indexing {len(objects)} linked objects")
    section_index, symbol_index = object_index(readelf, objects, build)
    link_map = args.link_map or os.path.join(reference, "link.map")
    if not os.path.exists(link_map):
        raise SystemExit(f"linked-object provenance map is missing: {link_map}")
    mapped = link_map_index(link_map, build)
    print(f"indexed {len(mapped[0])} linked text-section intervals")
    tx_profiles = [load_profile(path) for path in args.tx]
    rx_profiles = [load_profile(path) for path in args.rx]

    aggregate = {}
    dropped = []
    for profile_index, (_, functions) in enumerate(tx_profiles):
        for address, function in functions.items():
            owner = map_owner(address, mapped)
            if owner:
                start, _end, obj, section = owner
                choice = (function["name"], {"object": obj, "section": section,
                                             "offset": address - start})
                reason = None if (obj, section) in section_index else \
                    "link map names an unavailable input section"
            else:
                choice, reason = choose_definition(function["aliases"], symbol_index)
                start = None
            if reason or not choice:
                dropped.append({"name": function["name"],
                                "address": function["addr"],
                                "reason": reason or "no linked input-section owner"})
                continue
            _, definition = choice
            geometry = section_index[(definition["object"], definition["section"])]
            key = (definition["object"], definition["section"])
            if start is None:
                start = address - definition["offset"]
            candidate = aggregate.setdefault(key, {
                "object": definition["object"], "section": definition["section"],
                "size": geometry["size"], "align": geometry["align"],
                "flags": geometry["flags"], "address": start,
                "symbols": set(), "tx": [0.0] * len(tx_profiles),
                "rx": [0.0] * len(rx_profiles),
            })
            if candidate["address"] != start:
                candidate["inconsistent_address"] = True
            candidate["symbols"].update(function["aliases"])
            candidate["tx"][profile_index] += function["samples"]

    for profile_index, (_, functions) in enumerate(rx_profiles):
        for address, function in functions.items():
            owner = map_owner(address, mapped)
            if owner and (owner[2], owner[3]) in section_index:
                choice = (function["name"], {"object": owner[2],
                                              "section": owner[3]})
            else:
                choice, _ = choose_definition(function["aliases"], symbol_index)
            if not choice:
                continue
            _, definition = choice
            key = (definition["object"], definition["section"])
            if key in aggregate:
                aggregate[key]["rx"][profile_index] += function["samples"]

    sites, scanned, absent = dynamic_sites(
        os.path.dirname(os.path.abspath(__file__)), args.cross, reference,
        os.path.join(build, ".config"))
    exclusions = {}
    exclude_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "exclude.tsv")
    if os.path.exists(exclude_path):
        for line in open(exclude_path, encoding="utf-8"):
            if line.strip() and not line.startswith("#") and "\t" in line:
                name, reason = line.rstrip("\n").split("\t", 1)
                exclusions[name] = reason
    eligible = []
    for candidate in aggregate.values():
        reasons = []
        if candidate["section"] in exclusions:
            reasons.append("excluded by scripts/imem/exclude.tsv: " + exclusions[candidate["section"]])
        if candidate["size"] <= 0 or candidate["size"] > BUDGET:
            reasons.append("section does not fit the I-MEM budget")
        if "G" in candidate["flags"]:
            reasons.append("ELF section group is not supported")
        if candidate.get("inconsistent_address"):
            reasons.append("aliases imply inconsistent linked addresses")
        hits = [{"address": f"0x{address:08x}", "table": table, "what": what}
                for address, table, what in sites
                if candidate["address"] <= address < candidate["address"] + candidate["size"]]
        if hits:
            reasons.append("contains runtime-patch site")
            candidate["dynamic_sites"] = hits
        mean_tx = sum(candidate["tx"]) / len(candidate["tx"])
        candidate["tx_mean"] = mean_tx
        candidate["rx_mean"] = (sum(candidate["rx"]) / len(candidate["rx"])
                                if candidate["rx"] else 0.0)
        candidate["symbols"] = sorted(candidate["symbols"])
        if mean_tx <= 0:
            reasons.append("non-positive net TX weight")
        if reasons:
            candidate["eligible"] = False
            candidate["ineligible_reasons"] = reasons
            dropped.append(candidate)
        else:
            candidate["eligible"] = True
            eligible.append(candidate)

    eligible.sort(key=lambda item: (item["address"], item["object"], item["section"]))
    nominal_values = [item["tx_mean"] for item in eligible]
    used, value, selected_indices = solve(eligible, nominal_values)
    selected = set(selected_indices)

    rng = random.Random(args.seed)
    retention = []
    frequency = [0] * len(eligible)
    nominal_bytes = sum(eligible[index]["size"] for index in selected)
    for _ in range(args.replicates):
        blocks = [rng.randrange(len(tx_profiles)) for _ in tx_profiles]
        values = []
        for candidate in eligible:
            sampled = sum(poisson(rng, candidate["tx"][block])
                          for block in blocks) / len(blocks)
            values.append(sampled)
        _, _, indices = solve(eligible, values)
        chosen = set(indices)
        for index in chosen:
            frequency[index] += 1
        kept = sum(eligible[index]["size"] for index in selected & chosen)
        retention.append(kept / nominal_bytes if nominal_bytes else 1.0)
    retention.sort()
    median_retention = retention[len(retention) // 2]

    for index, candidate in enumerate(eligible):
        candidate["selected"] = index in selected
        candidate["bootstrap_frequency"] = frequency[index] / args.replicates
        candidate["address"] = f"0x{candidate['address']:08x}"
    for candidate in dropped:
        if isinstance(candidate, dict) and isinstance(candidate.get("address"), int):
            candidate["address"] = f"0x{candidate['address']:08x}"

    output = {
        "schema": 1,
        "release": args.release,
        "budget": BUDGET,
        "reference": {
            "vmlinux_sha256": sha256(os.path.join(reference, "vmlinux")),
            "config_sha256": sha256(os.path.join(build, ".config")),
        },
        "profiles": {
            "tx": [{"path": path, "sha256": sha256(path)} for path in args.tx],
            "rx": [{"path": path, "sha256": sha256(path)} for path in args.rx],
        },
        "dynamic_code": {"tables_scanned": scanned, "tables_absent": absent},
        "solver": {"objective": "mean net TX samples", "occupation": used,
                   "value": value, "selected_sections": len(selected)},
        "bootstrap": {"replicates": args.replicates, "seed": args.seed,
                      "median_byte_retention": median_retention,
                      "pass": median_retention >= 0.80},
        "candidates": eligible,
        "ineligible": dropped,
    }
    with open(args.out, "w", encoding="utf-8") as stream:
        json.dump(output, stream, indent=2)
        stream.write("\n")
    print(f"eligible: {len(eligible)}, selected: {len(selected)}, "
          f"I-MEM: {used}/{BUDGET} bytes")
    print(f"bootstrap median byte retention: {100 * median_retention:.1f}% "
          f"({'PASS' if median_retention >= .8 else 'NEEDS 2 MORE TX CAPTURES'})")


if __name__ == "__main__":
    main()
