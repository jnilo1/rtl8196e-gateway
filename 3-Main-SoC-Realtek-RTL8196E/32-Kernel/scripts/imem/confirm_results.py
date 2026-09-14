#!/usr/bin/env python3
"""One fixed-size paired analysis for the rev3 I-MEM confirmation."""
import argparse, csv, json, math, os, statistics

T95_DF11 = 2.201

def interval(values):
    mean = statistics.mean(values)
    sd = statistics.stdev(values)
    half = T95_DF11 * sd / math.sqrt(len(values))
    return {"n": len(values), "values": values, "mean": mean, "sd": sd,
            "lower": mean - half, "upper": mean + half}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--candidate", default="C")
    ap.add_argument("--incumbent", default="I")
    args = ap.parse_args()
    rows = list(csv.DictReader(open(os.path.join(args.dir, "sweep.tsv")), delimiter="\t"))
    # Floors of the harness's environment rule (protocol.txt); a point under
    # them is a path fault that the harness must have archived and replayed,
    # never a measurement to judge. Absent (older runs): the defaults.
    floors = {"env_tx_floor": 40.0, "env_rx_floor": 60.0}
    try:
        for line in open(os.path.join(args.dir, "protocol.txt")):
            k, _, v = line.strip().partition("=")
            if k in floors: floors[k] = float(v)
    except FileNotFoundError:
        pass
    failures = []
    points = {}
    for row in rows:
        key = (row["label"], int(row["round"]))
        if key in points: failures.append(f"duplicate point {key}")
        points[key] = row
        if float(row["tx"]) < floors["env_tx_floor"] or float(row["rx"]) < floors["env_rx_floor"]:
            failures.append(f"point below environment floors at {key}: tx {row['tx']} rx {row['rx']}")
        if row["tcpflush"] != "yes": failures.append(f"tcpflush failed at {key}")
        if row["retr_unparsed"] != "0" or row["retr"] != "0": failures.append(f"retransmission defect at {key}")
        if not row["err_delta"].startswith("hard=0 "): failures.append(f"hard counter defect at {key}: {row['err_delta']}")
    if len(rows) != 24: failures.append(f"expected 24 points, got {len(rows)}")
    for round_no in range(1, 13):
        for label in (args.candidate, args.incumbent):
            if (label, round_no) not in points: failures.append(f"missing {(label, round_no)}")
    for odd in range(1, 13, 2):
        if any((label, r) not in points for label in (args.candidate, args.incumbent) for r in (odd, odd + 1)):
            continue  # already reported as missing: a partial run fails the protocol, it must not crash the judge
        a = sorted(((int(points[(label, odd)]["pos"]), label) for label in (args.candidate, args.incumbent)))
        b = sorted(((int(points[(label, odd + 1)]["pos"]), label) for label in (args.candidate, args.incumbent)))
        if [label for _, label in a] != list(reversed([label for _, label in b])):
            failures.append(f"rounds {odd}/{odd+1} are not exact reverses")
    running = {row["still_running"] for row in rows}
    if len(running) != 1: failures.append(f"quiesce differs across points: {len(running)} sets")
    result = {"protocol_pass": not failures, "protocol_failures": failures,
              "points": len(rows), "reversed_order_pairs": 6,
              "quiesce_uniform": len(running) == 1,
              "env_floors": floors}
    if failures:
        result["decision"] = "INCONCLUSIVE"
    else:
        tx = interval([float(points[(args.candidate, r)]["tx"]) - float(points[(args.incumbent, r)]["tx"]) for r in range(1, 13)])
        rx = interval([float(points[(args.candidate, r)]["rx"]) - float(points[(args.incumbent, r)]["rx"]) for r in range(1, 13)])
        result["tx"] = tx; result["rx"] = rx
        confirmed = tx["lower"] > 0 and tx["mean"] >= 0.7 and rx["lower"] > -0.5
        refuted = tx["mean"] < 0.7 or tx["upper"] <= 0 or rx["upper"] <= -0.5
        result["criteria"] = {"tx_lower_gt_zero": tx["lower"] > 0,
                              "tx_mean_ge_0_7": tx["mean"] >= 0.7,
                              "rx_lower_gt_minus_0_5": rx["lower"] > -0.5}
        result["decision"] = "CONFIRMED" if confirmed else ("REJECTED" if refuted else "INCONCLUSIVE")
    with open(os.path.join(args.dir, "confirmation.json"), "w") as stream:
        json.dump(result, stream, indent=2); stream.write("\n")
    print("protocol:", "PASS" if result["protocol_pass"] else "FAIL")
    if "tx" in result:
        print(f"TX C-I: {result['tx']['mean']:+.2f} Mbit/s, 95% [{result['tx']['lower']:+.2f}, {result['tx']['upper']:+.2f}]")
        print(f"RX C-I: {result['rx']['mean']:+.2f} Mbit/s, 95% [{result['rx']['lower']:+.2f}, {result['rx']['upper']:+.2f}]")
    print("decision:", result["decision"])
    if failures: raise SystemExit(3)

if __name__ == "__main__": main()
