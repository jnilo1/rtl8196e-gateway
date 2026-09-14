#!/bin/bash
# bench_env.sh — shared "environment invalid" rule for the paired bench harnesses
# (bench_history_sweep.sh, imem/confirm_candidates.sh). Sourced, not executed.
#
# A measurement group (a sweep round, a confirmation round: every image
# measured back to back on the same box) where EVERY image reads far below
# the operational band at once is not a kernel difference — it is the shared
# path (host NIC, switch, cable, concurrent traffic) that failed for those
# minutes. The floors sit far under the band (TX 80-83 / RX 93-94 Mbit/s on
# the shipping kernels) and far under any kernel-attributable delta, so the
# rule can only remove path faults, never a losing candidate: it needs ALL
# images of the group below a floor, and a group is replayed in the same
# order, so it cannot bias one image against the other.
#
# Env: ENV_TX_FLOOR(40) ENV_RX_FLOOR(60) Mbit/s, ENV_MAX_REPLAYS(2) per run.

ENV_TX_FLOOR="${ENV_TX_FLOOR:-40}"
ENV_RX_FLOOR="${ENV_RX_FLOOR:-60}"
ENV_MAX_REPLAYS="${ENV_MAX_REPLAYS:-2}"

# env_point_below TX RX -> 0 when the point is below either floor
env_point_below() {
	awk -v tx="$1" -v rx="$2" -v txf="$ENV_TX_FLOOR" -v rxf="$ENV_RX_FLOOR" \
		'BEGIN { exit (tx+0 < txf || rx+0 < rxf) ? 0 : 1 }'
}

# env_all_below < "TX<TAB>RX" lines -> 0 when at least one line was read and
# every line is below a floor (the whole group failed together). Lines with a
# non-numeric field (NA, flash failure) are ignored: a group with no measured
# point is not "environment invalid", it is a flash failure recorded per point.
env_all_below() {
	awk -F'\t' -v txf="$ENV_TX_FLOOR" -v rxf="$ENV_RX_FLOOR" '
		$1 ~ /^[0-9.]+$/ && $2 ~ /^[0-9.]+$/ { n++; if ($1+0 < txf || $2+0 < rxf) low++ }
		END { exit (n > 0 && low == n) ? 0 : 1 }'
}
