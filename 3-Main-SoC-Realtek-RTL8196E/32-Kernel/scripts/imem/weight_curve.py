#!/usr/bin/env python3
"""Offline TX/RX coverage curve of the I-MEM selection versus the RX weight.

Reads any selection manifest written by select_profile.py (eligibility and the
per-section TX/RX sample means do not depend on the weight), re-solves the exact
knapsack for a grid of RX weights and prints the TX and RX coverage of each
selection. No build, no bench: this only chooses which weights are worth
benching.

The curve is a step followed by a slow tail: most of the RX coverage arrives as
soon as a small weight lets the RX-hot sections (csum_partial first) into the
window. The suggested pair is DEFAULT_PAIR (0.1 won on 6.18.54; 0.25 and 0.5
tied within 0.1 Mbit/s on 7.2.8, which shipped 0.5), as long as the step
is already reached at its first weight (STEP_FRACTION of the RX coverage
gain of the largest weight); otherwise it is the first weight that reaches
the step and the next one. Both must still be benched paired against the
incumbent policy: coverage does not predict throughput (on 6.18.54, weight 0.5
matched the incumbent's TX coverage and lost 2.3 Mbit/s of TX).
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from select_profile import BUDGET, solve  # noqa: E402

DEFAULT_GRID = "0,0.05,0.1,0.15,0.25,0.5,1"
DEFAULT_PAIR = (0.1, 0.25)
STEP_FRACTION = 0.5


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("manifest", help="selection manifest (any RX weight)")
    parser.add_argument("--weights", default=DEFAULT_GRID,
                        help=f"comma-separated RX weights (default {DEFAULT_GRID})")
    parser.add_argument("--policy", help="incumbent policy .tsv, reported for comparison")
    args = parser.parse_args()

    weights = sorted({float(w) for w in args.weights.split(",")})
    if weights[0] != 0.0:
        weights.insert(0, 0.0)
    manifest = json.load(open(args.manifest, encoding="utf-8"))
    eligible = manifest["candidates"]
    tx_total = sum(c["tx_mean"] for c in eligible)
    rx_total = sum(max(c["rx_mean"], 0.0) for c in eligible)
    if not tx_total or not rx_total:
        raise SystemExit("manifest has no TX or no RX samples")

    def coverage(chosen):
        tx = sum(c["tx_mean"] for c in chosen) / tx_total
        rx = sum(max(c["rx_mean"], 0.0) for c in chosen) / rx_total
        return tx, rx

    rows = []
    for weight in weights:
        scale = weight * tx_total / rx_total
        values = [c["tx_mean"] + scale * max(c["rx_mean"], 0.0) for c in eligible]
        used, _, indices = solve(eligible, values)
        tx, rx = coverage([eligible[i] for i in indices])
        rows.append((weight, len(indices), used, tx, rx))

    print(f"release {manifest.get('release')}, {len(eligible)} eligible sections, "
          f"budget {BUDGET} bytes")
    print(f"{'weight':>7} {'sections':>8} {'bytes':>6} {'TX cov':>7} {'RX cov':>7}")
    for weight, count, used, tx, rx in rows:
        print(f"{weight:7.2f} {count:8d} {used:6d} {tx:7.1%} {rx:7.1%}")

    if args.policy:
        names = {line.split("\t")[1].strip() for line in open(args.policy, encoding="utf-8")
                 if not line.startswith("#") and "\t" in line}
        tx, rx = coverage([c for c in eligible if c["section"] in names])
        print(f"incumbent {os.path.basename(args.policy)}: TX {tx:.1%}, RX {rx:.1%} "
              "(upper bound: sections matched by name)")

    base_rx, top_rx = rows[0][4], rows[-1][4]
    gain = top_rx - base_rx
    if gain <= 0:
        print("RX coverage does not grow with the weight: bench the TX-only selection only")
        return
    step = next(i for i, row in enumerate(rows)
                if row[4] - base_rx >= STEP_FRACTION * gain)
    if rows[step][0] <= DEFAULT_PAIR[0]:
        pair = list(DEFAULT_PAIR)
        print(f"step: weight {rows[step][0]:g} already reaches {STEP_FRACTION:.0%} of the RX "
              f"coverage gain of weight {rows[-1][0]:g}; default pair applies")
    else:
        pair = [rows[step][0]] + ([rows[step + 1][0]] if step + 1 < len(rows) else [])
        print(f"step: only weight {rows[step][0]:g} reaches {STEP_FRACTION:.0%} of the RX "
              f"coverage gain; the pair moves up")
    print("bench, paired against the incumbent: weights " + " and ".join(f"{w:g}" for w in pair))


if __name__ == "__main__":
    main()
