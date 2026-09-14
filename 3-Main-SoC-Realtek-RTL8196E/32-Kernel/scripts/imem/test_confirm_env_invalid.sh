#!/bin/bash
# Host-only tests of the "environment invalid" round rule in the confirmation
# harness: the round loop of confirm_candidates.sh is run verbatim (between
# its >>> / <<< markers) with a stubbed measure_round, no gateway, no flash.
#
#   1. one invalid round then a valid re-measurement: archived, replayed in the
#      same order, 24 valid points, judge runs;
#   2. ENV_MAX_REPLAYS+1 invalid attempts in a row: the harness stops without
#      a verdict and without a confirmation.json, and the partial sweep.tsv
#      cannot be judged;
#   3. one image alone under a floor: NOT env-invalid — the round is written
#      as measured (a real regression or a bad flash), and the judge refuses
#      that point as a protocol failure.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
harness="$here/confirm_candidates.sh"
judge="$here/confirm_results.py"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
loop="$work/loop.sh"
awk '/^# >>> round loop/{p=1} p{print} /^# <<< round loop/{exit}' "$harness" >"$loop"
grep -q 'env_replays=' "$loop" || { echo "round loop not found in $harness" >&2; exit 1; }

# Full 12-round mirrored plan (as the harness draws it) and the sweep header.
make_plan() {
	printf 'round\tposition1\tposition2\n'
	for pair in $(seq 1 6); do
		if [ $((pair % 2)) = 1 ]; then a=C; b=I; else a=I; b=C; fi
		printf '%s\t%s\t%s\n%s\t%s\t%s\n' $((pair*2-1)) "$a" "$b" $((pair*2)) "$b" "$a"
	done
}
header='label\tround\ttx\trx\tuname\tpos\tretr\ttcpflush\tretr_unparsed\terr_delta\tstill_running'

# run_case NAME SCENARIO -> runs the loop; SCENARIO is a bash function name
# that prints "tx rx" for (round, label, attempt-of-that-round).
run_case() {
	local name="$1" scenario="$2" out rc=0
	out="$work/$name"
	mkdir -p "$out/raw" "$out/dmesg"
	make_plan >"$out/plan.tsv"; printf "$header\\n" >"$out/sweep.tsv"
	printf 'env_tx_floor=%s\nenv_rx_floor=%s\n' "${ENV_TX_FLOOR:-40}" "${ENV_RX_FLOOR:-60}" >"$out/protocol.txt"
	(
		set -euo pipefail
		# shellcheck disable=SC1091
		. "$here/../bench_env.sh"
		# read by the sourced loop
		export OUTPUT="$out" GAP=0 CANDIDATE=C.img INCUMBENT=I.img
		say() { :; }
		declare -A attempts
		measure_round() {
			local round="$1" label pos v
			attempts[$round]=$(( ${attempts[$round]:-0} + 1 ))
			ROUND_ROWS=(); ROUND_VALS=()
			for label in "$2" "$3"; do
				point=$((point+1)); pos=1; [ "$label" = "$3" ] && pos=2
				v="$($scenario "$round" "$label" "${attempts[$round]}")"
				touch "$out/raw/${round}-${label}-tx1.log" "$out/dmesg/${round}-${label}-before.txt"
				ROUND_ROWS+=("$(printf '%s\t%s\t%s\t%s\tk\t%s\t0\tyes\t0\thard=0 soft=0,0\tx' "$label" "$round" ${v} "$pos")")
				ROUND_VALS+=("$(printf '%s\t%s' ${v})")
			done
		}
		# shellcheck disable=SC1090
		. "$loop"
	) >"$out/log" 2>&1 || rc=$?
	echo "$rc" >"$out/status"
}
fail() { echo "FAIL: $*" >&2; exit 1; }

# ── case 1: round 5 fails once, then measures fine ──
s1() { if [ "$1" = 5 ] && [ "$3" = 1 ]; then echo "3.0 84.7"; elif [ "$2" = C ]; then echo "82.5 93.5"; else echo "80.7 93.6"; fi; }
run_case one_replay s1
[ "$(cat "$work/one_replay/status")" = 0 ] || fail "case 1: loop exited $(cat "$work/one_replay/status")"
[ "$(tail -n +2 "$work/one_replay/sweep.tsv" | wc -l)" = 24 ] || fail "case 1: expected 24 valid points"
grep -q 'env_replays=1' "$work/one_replay/protocol.txt" || fail "case 1: one replay expected"
[ "$(tail -n +2 "$work/one_replay/env-invalid/sweep.tsv" | wc -l)" = 2 ] || fail "case 1: 2 archived rows expected"
[ -f "$work/one_replay/env-invalid/round5-attempt1/raw/5-C-tx1.log" ] || fail "case 1: raw log not archived"
# same order on the replay: positions of round 5 equal the plan's
plan5="$(awk -F'\t' '$1==5{print $2 $3}' "$work/one_replay/plan.tsv")"
got5="$(awk -F'\t' '$2==5{print $6, $1}' "$work/one_replay/sweep.tsv" | sort | awk '{printf "%s",$2}')"
[ "$plan5" = "$got5" ] || fail "case 1: replay order $got5 differs from plan $plan5"
python3 "$judge" --dir "$work/one_replay" >"$work/one_replay/judge" || fail "case 1: judge refused a clean run"
grep -q 'decision: CONFIRMED' "$work/one_replay/judge" || fail "case 1: expected CONFIRMED on the synthetic +1.8"
echo "ok  1. one env-invalid round: archived, replayed in plan order, 24 valid points, judged"

# ── case 2: round 3 is invalid on every attempt ──
s2() { if [ "$1" = 3 ]; then echo "3.0 84.7"; elif [ "$2" = C ]; then echo "82.5 93.5"; else echo "80.7 93.6"; fi; }
ENV_MAX_REPLAYS=2 run_case exhausted s2
[ "$(cat "$work/exhausted/status")" != 0 ] || fail "case 2: loop should stop with an error"
grep -q 'replay(s) already spent' "$work/exhausted/log" || fail "case 2: stop reason not reported"
[ ! -e "$work/exhausted/confirmation.json" ] || fail "case 2: a verdict file exists"
grep -q 'env_replays=' "$work/exhausted/protocol.txt" && fail "case 2: protocol closed as if complete"
[ "$(tail -n +2 "$work/exhausted/sweep.tsv" | wc -l)" = 4 ] || fail "case 2: only rounds 1-2 should be in sweep.tsv"
[ "$(tail -n +2 "$work/exhausted/env-invalid/sweep.tsv" | wc -l)" = 6 ] || fail "case 2: 3 attempts x 2 rows expected in the archive"
ls -d "$work/exhausted/env-invalid/round3-attempt"{1,2,3} >/dev/null || fail "case 2: per-attempt archives missing"
if python3 "$judge" --dir "$work/exhausted" >"$work/exhausted/judge" 2>&1; then fail "case 2: judge accepted a partial run"; fi
grep -q 'protocol: FAIL' "$work/exhausted/judge" || fail "case 2: judge did not report a protocol failure"
echo "ok  2. replays exhausted: stop without verdict, partial sweep refused by the judge"

# ── case 3: only the candidate collapses in round 7 ──
s3() { if [ "$1" = 7 ] && [ "$2" = C ]; then echo "3.0 93.5"; elif [ "$2" = C ]; then echo "82.5 93.5"; else echo "80.7 93.6"; fi; }
run_case one_image s3
[ "$(cat "$work/one_image/status")" = 0 ] || fail "case 3: loop exited $(cat "$work/one_image/status")"
grep -q 'ENVIRONMENT INVALID' "$work/one_image/log" && fail "case 3: a single low image was treated as env-invalid"
[ ! -e "$work/one_image/env-invalid" ] || fail "case 3: nothing should be archived"
grep -q 'env_replays=0' "$work/one_image/protocol.txt" || fail "case 3: no replay expected"
awk -F'\t' '$1=="C" && $2==7 && $3=="3.0"' "$work/one_image/sweep.tsv" | grep -q . || fail "case 3: the low point must be recorded as measured"
if python3 "$judge" --dir "$work/one_image" >"$work/one_image/judge" 2>&1; then fail "case 3: judge accepted a point under the floor"; fi
grep -q "point below environment floors at ('C', 7)" "$work/one_image/confirmation.json" || fail "case 3: judge did not name the point"
grep -q 'decision: INCONCLUSIVE' "$work/one_image/judge" || fail "case 3: expected INCONCLUSIVE"
echo "ok  3. one image under the floor: not env-invalid, recorded, refused by the judge as a point failure"
echo "all env-invalid confirmation tests passed"
