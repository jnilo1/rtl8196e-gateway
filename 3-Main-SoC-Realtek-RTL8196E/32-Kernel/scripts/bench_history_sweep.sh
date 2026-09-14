#!/bin/bash
# bench_history_sweep.sh — objective TX comparison across firmware versions
#
# The problem this solves: a single bench of version A vs a single bench of
# version B is NOT comparable — the box's TX throughput drifts ~3 Mbit/s
# across sessions (thermal/clock), which dwarfs the ~1-2% deltas between
# driver/kernel builds. Comparing the historically-recorded numbers is worse
# (different days, conditions, even iperf2 vs iperf3).
#
# The fix: REPLAY every image on ONE box in ONE session, INTERLEAVED in
# RANDOMIZED ROUNDS, and report each version NORMALIZED to a fixed reference
# measured in the same round. The slow common-mode drift then cancels: within
# a round all versions see ~the same box state, so the per-round ratio
# version/reference is drift-free, and averaging ratios across rounds gives a
# tight confidence interval. Going oldest->newest in order would let the
# monotonic session drift masquerade as a trend — hence the shuffle.
#
# What it measures cleanly: RELATIVE TX (gw->host) per build. RX is line-rate
# bound and already comparable; a 1-rep RX sanity is taken per point.
#
# CONFOUNDER (cannot be removed here): each release usually changed driver +
# kernel + gcc together, so this charts COMBINED release evolution, not the
# driver in isolation. To isolate the driver, rebuild each driver version on a
# fixed kernel+toolchain and feed those images here. Cross the 5.10->6.18
# kernel-line boundary is a further confounder; sweep within one line.
#
# Each image SOURCE is either a git ref (tag/sha — kernel image extracted from
# $KIMG_PATH at that ref) or a path to a .img file.
#
# Usage:
#   ./bench_history_sweep.sh [opts] LABEL=SOURCE LABEL=SOURCE ...
#   ./bench_history_sweep.sh [opts] --manifest sweep.txt      # "LABEL SOURCE" per line
#
# Example (replay three releases, 4 rounds, normalize to rc2):
#   ./bench_history_sweep.sh --rounds 4 --ref rc2 \
#       v3.8.0=v3.8.0 rc1=v4.0.0-rc1 rc2=kernel-img/lidl/kernel-6.18.img
#
# Options:
#   --rounds N    randomized rounds (default 3; >=3 needed for a CI)
#   --reps N      TX reps per (image,round) (default 3)
#   --ref LABEL   normalization reference (default: first LABEL given)
#   --restore SRC image flashed at the very end to return the box to a known
#                 build (default: the reference's source)
#   --manifest F  read "LABEL SOURCE" lines from F
#   --preset NAME built-in build set instead of LABEL=SOURCE args.
#                 'perf-boundaries' = the 7 tags where driver code / kernel
#                 minor / gcc actually change (v3.0.0 v3.4.0 v3.4.1 v3.5.0
#                 v3.8.0 v3.9.0 rc2), skipping perf-identical releases; ref=rc2.
#   --balanced    alternate the order every round (AB, BA, AB, ...) instead of
#                 shuffling it — a balanced design for a two-image comparison
#   --dry-run     no flash / no measure — simulate, to validate the harness
#
# ENVIRONMENT-INVALID ROUNDS: a round where EVERY image measured far below the
# operational band (TX < ENV_TX_FLOOR or RX < ENV_RX_FLOOR, on all images at
# once) is not a kernel difference — it is the path (host NIC, switch, cable,
# concurrent traffic) that failed for those minutes. Such a round is flagged
# "env-invalid" in sweep.tsv, its rows are also archived in env-invalid.tsv,
# it is excluded from the analysis, and it is REPLAYED once more at the end of
# the schedule with the same image order (so a --balanced design stays
# balanced). At most ENV_MAX_REPLAYS replays per sweep; beyond that the sweep
# stops with an error rather than measure on a broken path.
#
# FLASH MECHANISM — why not flash_remote/boothold: the boothold binary reads
# the HOLD-magic page address from a device-tree reserved-memory node, and
# REFUSES to write if that node is absent. Early DTBs (e.g. v3.0.0 and the
# first 6.18 releases) predate the node, so boothold cannot re-enter the
# bootloader over SSH from those kernels — a history sweep would flash INTO an
# old build and then get stuck. So this tool arms the bootloader itself, the
# way manual recovery does: write HOLD ("0x484F4C44") to the board's fixed page
# word (Lidl: 0x01FFEFFC) via devmem, then reboot — works on every version.
# The address is board-specific; set BOOTHOLD_ADDR for a non-Lidl board.
#
# Env: RTL8196E_IP RTL8196E_USER(root) BOOT_IP IPERF_PORT(5201)
#      (addresses default via lib/gwconf.sh: gateway.env, last install, host LAN)
#      IPERF3_BIN(iperf3) IFACE(eth0) DUR_TX(20) GAP(10) ALLOW_WIRELESS(0)
#      KIMG_PATH(3-Main-SoC-Realtek-RTL8196E/32-Kernel/kernel-6.18.img — historical refs)
#      BOOTHOLD_ADDR(0x01FFEFFC) NOTIFY_CMD(desktop notify-send)
#      ENV_TX_FLOOR(40) ENV_RX_FLOOR(60) ENV_MAX_REPLAYS(2)  — see above
#      ENV_SIM_INVALID_ROUND(0) — dry-run only: simulate round N as invalid
#
# Progress: a notification fires after EVERY point — desktop notify-send if
# present; set NOTIFY_CMD="mycmd" to route it anywhere (gets the message as $1).
#
# J. Nilo — June 2026

set -euo pipefail
export LC_ALL=C

# Gateway address: RTL8196E_IP env > gateway.env > the last gateway installed
# or reached > its hostname > the historic 192.168.1.88 (see lib/gwconf.sh).
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/../../../lib/gwconf.sh"
RTL8196E_IP="${RTL8196E_IP:-$(gwconf_gateway_addr)}"
RTL8196E_USER="${RTL8196E_USER:-root}"
BOOT_IP="${BOOT_IP:-$(gwconf_cold_boot_ip)}"
IPERF_PORT="${IPERF_PORT:-5201}"
IPERF3_BIN="${IPERF3_BIN:-iperf3}"
IFACE="${IFACE:-eth0}"
DUR_TX="${DUR_TX:-20}"
GAP="${GAP:-10}"
ALLOW_WIRELESS="${ALLOW_WIRELESS:-0}"
# Historical git refs (v3.x) carry the image at the pre-kernel-img path, so the
# `git show <ref>:$KIMG_PATH` extraction must keep using it. The current image
# (now under kernel-img/<board>/) is fed as a working-tree path SOURCE instead
# (e.g. rc2=kernel-img/lidl/kernel-6.18.img), which bypasses KIMG_PATH.
KIMG_PATH="${KIMG_PATH:-3-Main-SoC-Realtek-RTL8196E/32-Kernel/kernel-6.18.img}"
BOOTHOLD_ADDR="${BOOTHOLD_ADDR:-0x01FFEFFC}"   # board HOLD-magic word (Lidl)
HOLD_MAGIC="0x484F4C44"                          # "HOLD" — bootloader download-mode trigger
NOTIFY_CMD="${NOTIFY_CMD:-}"   # per-point alert: a cmd that gets the message as $1; else desktop notify-send
# "Environment invalid" round rule and floors (ENV_TX_FLOOR, ENV_RX_FLOOR,
# ENV_MAX_REPLAYS): shared with the confirmation harness, see bench_env.sh.
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/bench_env.sh"
ENV_SIM_INVALID_ROUND="${ENV_SIM_INVALID_ROUND:-0}"   # dry-run: make round N read as a path fault

ROUNDS=3; REPS=3; REF=""; RESTORE=""; MANIFEST=""; DRY=0; PRESET=""; BALANCED=0
LABELS=(); SOURCES=()

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; CYAN='\033[0;36m'; NC='\033[0m'
log(){ echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} $1"; }
log_ok(){ echo -e "${GREEN}[$(date +%H:%M:%S)] ✓${NC} $1"; }
log_err(){ echo -e "${RED}[$(date +%H:%M:%S)] ✗${NC} $1"; }
log_warn(){ echo -e "${YELLOW}[$(date +%H:%M:%S)] !${NC} $1"; }
log_info(){ echo -e "${CYAN}[$(date +%H:%M:%S)] ℹ${NC} $1"; }
notify(){  # per-point progress alert: custom NOTIFY_CMD, else desktop notify-send, plus stdout
  local msg="$1"
  if [ -n "$NOTIFY_CMD" ]; then $NOTIFY_CMD "$msg" >/dev/null 2>&1 || true
  elif command -v notify-send >/dev/null 2>&1; then notify-send -t 8000 "history sweep" "$msg" >/dev/null 2>&1 || true; fi
  echo -e "${GREEN}>>> ${msg}${NC}"
}

# ── Arg parse ─────────────────────────────────────────────────────────
add_entry(){ LABELS+=("${1%%=*}"); SOURCES+=("${1#*=}"); }
while [ $# -gt 0 ]; do
  case "$1" in
    --rounds) ROUNDS="$2"; shift 2 ;;
    --balanced) BALANCED=1; shift ;;
    --reps)   REPS="$2"; shift 2 ;;
    --ref)    REF="$2"; shift 2 ;;
    --restore) RESTORE="$2"; shift 2 ;;
    --manifest) MANIFEST="$2"; shift 2 ;;
    --preset) PRESET="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    --*) log_err "unknown option: $1"; exit 1 ;;
    *) add_entry "$1"; shift ;;
  esac
done
if [ -n "$MANIFEST" ]; then
  while read -r lbl src _; do
    [ -z "$lbl" ] && continue; case "$lbl" in \#*) continue ;; esac
    LABELS+=("$lbl"); SOURCES+=("$src")
  done < "$MANIFEST"
fi
# Built-in presets — the TX-relevant transitions (driver code / kernel minor /
# gcc), so the sweep skips perf-identical releases. See the header table.
case "$PRESET" in
  "") : ;;
  perf-boundaries)
    for e in v3.0.0=v3.0.0 v3.4.0=v3.4.0 v3.4.1=v3.4.1 v3.5.0=v3.5.0 v3.8.0=v3.8.0 v3.9.0=v3.9.0 rc2=kernel-img/lidl/kernel-6.18.img; do add_entry "$e"; done
    [ -z "$REF" ] && REF=rc2 ;;
  *) log_err "unknown --preset '$PRESET' (known: perf-boundaries)"; exit 1 ;;
esac
[ "${#LABELS[@]}" -ge 2 ] || { log_err "need >=2 LABEL=SOURCE entries (or --preset; got ${#LABELS[@]})"; exit 1; }
[ -z "$REF" ] && REF="${LABELS[0]}"
# validate ref is among labels
ref_ok=0; for l in "${LABELS[@]}"; do [ "$l" = "$REF" ] && ref_ok=1; done
[ "$ref_ok" = 1 ] || { log_err "--ref '$REF' is not one of: ${LABELS[*]}"; exit 1; }
[ "$ROUNDS" -ge 1 ] || { log_err "--rounds must be >=1"; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RTLDIR="$(cd "${KERNEL_DIR}/.." && pwd)"           # holds flash_remote.sh
REPO_ROOT="$(git -C "$RTLDIR" rev-parse --show-toplevel)"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${KERNEL_DIR}/test_results_history_sweep_${TS}"
CSV="${OUT_DIR}/sweep.tsv"

# ── gw + measurement helpers (mirror bench_release_iperf3.sh) ─────────
gw(){ ssh -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 -o BatchMode=yes "${RTL8196E_USER}@${RTL8196E_IP}" "$@"; }
recv_mbps(){ grep -E "[0-9]+(\.[0-9]+)?[[:space:]]+[KMG]bits/sec.*receiver[[:space:]]*$" 2>/dev/null | \
  awk '{for(i=1;i<=NF;i++) if($i ~ /bits\/sec/){u=$i; v=$(i-1); if(u~/Kbits/)v=v/1000; else if(u~/Gbits/)v=v*1000; print v; exit}}' | tail -1; }
med(){ awk '/^[0-9]/{v[n++]=$1} END{if(n==0){print "NA";exit} for(i=0;i<n;i++)for(j=i+1;j<n;j++)if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t} printf "%.1f",(n%2)?v[int(n/2)]:(v[n/2-1]+v[n/2])/2}'; }
restart_server(){ gw "killall iperf3 2>/dev/null; $IPERF3_BIN -s -p $IPERF_PORT -D </dev/null >/dev/null 2>&1"; sleep 1; }

quiesce_radio(){  # stop every radio init script that exists (harmless if not running)
  local initd="" d svc
  for d in /userdata/etc/init.d /etc/init.d; do gw "[ -d $d ]" 2>/dev/null && { initd="$d"; break; }; done
  [ -z "$initd" ] && return 0
  for svc in S70otbr S50uart_bridge S50serialgateway S60serialgateway; do
    gw "[ -x $initd/$svc ]" 2>/dev/null || continue
    gw "$initd/$svc stop" >/dev/null 2>&1 || true
  done
  sleep 2
}

wait_ssh(){ local i; for i in $(seq 1 45); do gw "true" 2>/dev/null && return 0; ping -c 2 -W 1 "$RTL8196E_IP" >/dev/null 2>&1; done; return 1; }

# Resolve a SOURCE to a flat image basename inside KERNEL_DIR. Echoes basename.
resolve_image(){  # label source -> basename (in KERNEL_DIR) | "" on failure
  local label=$1 src=$2 base dest          # split: a later 'local' RHS can't see an earlier one under set -u
  base="sweepimg_${label}.img"
  dest="${KERNEL_DIR}/${base}"
  if [ -f "$src" ]; then cp -f "$src" "$dest"
  elif [ -f "${KERNEL_DIR}/${src}" ]; then cp -f "${KERNEL_DIR}/${src}" "$dest"
  elif git -C "$REPO_ROOT" cat-file -e "${src}:${KIMG_PATH}" 2>/dev/null; then
    git -C "$REPO_ROOT" show "${src}:${KIMG_PATH}" > "$dest" 2>/dev/null || return 1
  else return 1; fi
  echo "$base"
}

flash_image(){  # basename (in KERNEL_DIR) -> 0 ok (box back up) ; echoes uname -r
  # Arm the bootloader ourselves (see FLASH MECHANISM header): boothold can't
  # exit old kernels, but writing the HOLD magic to the fixed page word works
  # on every version. SSH drops on reboot — ignore it.
  gw "devmem $BOOTHOLD_ADDR 32 $HOLD_MAGIC; sync; reboot" >/dev/null 2>&1 || true
  # wait for the bootloader to come up at BOOT_IP
  local i; for i in $(seq 1 30); do
    ping -c1 -W1 "$BOOT_IP" >/dev/null 2>&1 && break
    ping -c2 -W1 "$RTL8196E_IP" >/dev/null 2>&1
  done
  ping -c1 -W2 "$BOOT_IP" >/dev/null 2>&1 || { log_err "bootloader did not appear at $BOOT_IP after arm+reboot"; return 1; }
  # TFTP the image (box already at the bootloader prompt)
  ( cd "$KERNEL_DIR" && CONFIRM=y ./flash_kernel.sh "$BOOT_IP" --image "$1" ) >/dev/null 2>&1 || return 1
  wait_ssh || return 1
  gw "uname -r" 2>/dev/null || echo "?"
}

SWEEP_IMGS=()   # extracted temp images to clean up
cleanup(){ gw "killall iperf3 2>/dev/null" >/dev/null 2>&1 || true; local f; for f in "${SWEEP_IMGS[@]}"; do rm -f "${KERNEL_DIR}/${f}" 2>/dev/null || true; done; }
trap 'echo; log_warn "interrupted"; cleanup; exit 1' INT TERM

sim_tx(){ awk -v L="$1" -v r="$2" 'BEGIN{h=0; for(i=1;i<=length(L);i++)h=(h*31+index("abcdefghijklmnopqrstuvwxyz0123456789.-_",substr(L,i,1)))%997; srand(h*100+r); printf "%.1f", 68 + (h%450)/100.0 + r*0.5 + (rand()-0.5)*0.6}'; }

# ══ Preconditions ═════════════════════════════════════════════════════
mkdir -p "$OUT_DIR"
printf 'label\tround\ttx\trx\tuname\tstatus\n' > "$CSV"
INVALID_CSV="${OUT_DIR}/env-invalid.tsv"
log "History sweep: ${#LABELS[@]} builds x ${ROUNDS} round(s), ref=${REF}, reps=${REPS}, dry=${DRY}"
log "Builds: $(for i in "${!LABELS[@]}"; do printf '%s=%s ' "${LABELS[$i]}" "${SOURCES[$i]}"; done)"
log "Output: $OUT_DIR"

if [ "$DRY" = 0 ]; then
  command -v iperf3 >/dev/null || { log_err "iperf3 not on host"; exit 1; }
  command -v shuf  >/dev/null || { log_err "shuf (coreutils) required for round randomization"; exit 1; }
  rdev=$(ip route get "$RTL8196E_IP" 2>/dev/null | grep -oE 'dev[[:space:]]+[^ ]+' | awk '{print $2}' | head -1)
  case "$rdev" in
    wlan*|wl*) [ "$ALLOW_WIRELESS" = 1 ] || { log_err "route to $RTL8196E_IP is wireless ($rdev) — set ALLOW_WIRELESS=1 to override"; exit 1; } ;;
    *) log_info "wired link via ${rdev:-?}" ;;
  esac
  # Pre-extract & validate every image up front (fail fast before any flash).
  for i in "${!LABELS[@]}"; do
    b=$(resolve_image "${LABELS[$i]}" "${SOURCES[$i]}") || { log_err "cannot resolve image for ${LABELS[$i]} (${SOURCES[$i]})"; cleanup; exit 1; }
    SWEEP_IMGS+=("$b"); log_info "resolved ${LABELS[$i]} -> ${b} ($(du -h "${KERNEL_DIR}/${b}" | cut -f1))"
  done
fi

# Environment-invalid round: every image measured (tx and rx numeric) sits
# below the floors — TX under ENV_TX_FLOOR or RX under ENV_RX_FLOOR. A round
# with no measured image at all is not "invalid", it is a flash failure and is
# already recorded as such per point.
round_env_invalid(){  # round -> 0 if invalid (all measured images below floors), 1 otherwise
  awk -F'\t' -v OFS='\t' -v r="$1" 'NR>1 && $2==r { print $3, $4 }' "$CSV" | env_all_below
}
tag_round(){  # round status -> rewrite the status column of that round's rows in place
  local tmp="${CSV}.tmp"
  awk -F'\t' -v OFS='\t' -v r="$1" -v st="$2" 'NR>1 && $2==r { $6=st } { print }' "$CSV" > "$tmp" && mv -f "$tmp" "$CSV"
}

# ══ Rounds (randomized order each round) ══════════════════════════════
idx_list="$(seq 0 $((${#LABELS[@]}-1)))"
DONE=0; TOTAL=$(( ROUNDS * ${#LABELS[@]} ))
SCHEDULED="$ROUNDS"      # grows by one per replayed round
REPLAYS=0                # env-invalid rounds replayed so far
REPLAY_ORDER=""          # order to reuse for the next round when replaying
REPLAY_OF=0              # the invalid round a replay stands in for (0 = regular round)
REG=0                    # regular (non-replay) rounds started — drives the --balanced alternation
r=0
while [ "$r" -lt "$SCHEDULED" ]; do
  r=$((r+1))
  if [ -n "$REPLAY_ORDER" ]; then
    order="$REPLAY_ORDER"; REPLAY_ORDER=""
    log "──── round ${r}/${SCHEDULED} — REPLAY of env-invalid round ${REPLAY_OF}, same order: $(for k in $order; do printf '%s ' "${LABELS[$k]}"; done)"
  else
    REPLAY_OF=0; REG=$((REG+1))
    if [ "$BALANCED" = 1 ]; then
      # alternate on the regular-round count, not on r: a replay repeats its
      # round's order, the next regular round must continue the alternation
      order=$( if [ $((REG % 2)) -eq 1 ]; then echo "$idx_list"; else echo "$idx_list" | tac; fi )
    else
      order=$( [ "$DRY" = 0 ] && echo "$idx_list" | shuf || echo "$idx_list" )
    fi
    log "──── round ${r}/${SCHEDULED} — order: $(for k in $order; do printf '%s ' "${LABELS[$k]}"; done)"
  fi
  for k in $order; do
    L="${LABELS[$k]}"; S="${SOURCES[$k]}"; DONE=$((DONE+1))
    if [ "$DRY" = 1 ]; then
      tx=$(sim_tx "$L" "$r"); rx="93.8"; un="dry-${L}"
      [ "$ENV_SIM_INVALID_ROUND" = "$r" ] && { tx="3.1"; rx="84.7"; }   # simulated path fault
    else
      un=$(flash_image "sweepimg_${L}.img") || { log_err "flash/boot FAILED for $L round $r — skipping point"; printf '%s\t%s\tNA\tNA\tflash-fail\tflash-fail\n' "$L" "$r" >> "$CSV"; notify "point ${DONE}/${TOTAL} — ${L} r${r}: FLASH/BOOT FAILED"; continue; }
      quiesce_radio
      txv=()
      for _ in $(seq 1 "$REPS"); do
        restart_server
        v=$(timeout --kill-after=5 $((DUR_TX+15)) iperf3 -c "$RTL8196E_IP" -p "$IPERF_PORT" -R -t "$DUR_TX" 2>/dev/null | recv_mbps || true)
        txv+=("${v:-}"); sleep "$GAP"
      done
      tx=$(printf '%s\n' "${txv[@]}" | med)
      restart_server
      rx=$(timeout --kill-after=5 $((DUR_TX+15)) iperf3 -c "$RTL8196E_IP" -p "$IPERF_PORT" -t "$DUR_TX" 2>/dev/null | recv_mbps || true)
    fi
    printf '%s\t%s\t%s\t%s\t%s\tok\n' "$L" "$r" "${tx:-NA}" "${rx:-NA}" "$un" >> "$CSV"
    log_info "  ${L} r${r}: TX ${tx:-NA}  RX ${rx:-NA}  [${un}]"
    notify "point ${DONE}/${TOTAL} done — ${L} round ${r}: TX ${tx:-NA} / RX ${rx:-NA} Mbit/s"
  done
  # ── environment check on the whole round ──
  if round_env_invalid "$r"; then
    tag_round "$r" "env-invalid"
    { [ -s "$INVALID_CSV" ] || head -1 "$CSV"; awk -F'\t' -v r="$r" 'NR>1 && $2==r' "$CSV"; } >> "$INVALID_CSV"
    log_warn "round ${r}: ENVIRONMENT INVALID — every image below TX ${ENV_TX_FLOOR} / RX ${ENV_RX_FLOOR} Mbit/s at once (path fault, not a kernel difference); rows tagged env-invalid, archived in $(basename "$INVALID_CSV"), excluded from the analysis"
    notify "round ${r}: environment invalid (all images below floors) — archived, will be replayed"
    if [ "$REPLAYS" -lt "$ENV_MAX_REPLAYS" ]; then
      REPLAYS=$((REPLAYS+1)); SCHEDULED=$((SCHEDULED+1)); TOTAL=$(( TOTAL + ${#LABELS[@]} ))
      REPLAY_ORDER="$order"; REPLAY_OF="$r"
      log "round ${r} will be replayed as round $((r+1)) with the same order (replay ${REPLAYS}/${ENV_MAX_REPLAYS})"
      [ "$DRY" = 0 ] && { log_info "settling ${GAP}s before the replay"; sleep "$GAP"; }
    else
      log_err "round ${r}: ${ENV_MAX_REPLAYS} replay(s) already spent — the path is not usable, stopping the sweep (partial results in $CSV)"
      break
    fi
  fi
done

# ══ Restore the box to a known build ══════════════════════════════════
if [ "$DRY" = 0 ]; then
  rsrc="${RESTORE:-${SOURCES[$(for i in "${!LABELS[@]}"; do [ "${LABELS[$i]}" = "$REF" ] && echo "$i"; done)]}}"
  rb=$(resolve_image "restore" "$rsrc" 2>/dev/null) && { SWEEP_IMGS+=("$rb"); log "restoring box to '${rsrc}'"; flash_image "$rb" >/dev/null 2>&1 || log_warn "restore flash failed — box left on last-swept build"; }
fi

# ══ Analysis: per-build abs median TX + TX-vs-ref ratio ± 95% CI ═══════
REPORT="${OUT_DIR}/RESULTS.md"
ORDER_LABELS="$(printf '%s ' "${LABELS[@]}")"
{
  echo "### Driver/release history sweep ($(date +%Y-%m-%d))"
  echo
  echo "Replay on one box (${RTL8196E_IP}), **${ROUNDS} randomized round(s)**, ref = \`${REF}\`, ${REPS} TX reps/point, ${DUR_TX}s, GAP ${GAP}s. TX normalized to the reference **measured in the same round** (cancels session drift); ± is the 95% CI of the per-round ratio. RX is a 1-rep line-rate sanity."
  echo
  if [ -s "$INVALID_CSV" ]; then
    inv_rounds=$(awk -F'\t' 'NR>1{print $2}' "$INVALID_CSV" | sort -un | tr '\n' ' ' | sed 's/ $//')
    echo "**Environment-invalid round(s) ${inv_rounds}** — every image below TX ${ENV_TX_FLOOR} / RX ${ENV_RX_FLOOR} Mbit/s at once (path fault, not a kernel difference): excluded from the table below, rows archived in \`env-invalid.tsv\`, replayed (${REPLAYS} replay(s) of ${ENV_MAX_REPLAYS} allowed)."
    echo
  fi
  awk -F'\t' -v ref="$REF" -v order="$ORDER_LABELS" '
    NR>1 && $3!="NA" && $6=="ok" { tx[$1"|"$2]=$3; rx[$1]=rx[$1]" "$4; seen[$1]=1; rounds[$2]=1; if(uname[$1]=="")uname[$1]=$5 }
    function median(arr,n,  i,j,t){ for(i=0;i<n;i++)for(j=i+1;j<n;j++)if(arr[j]<arr[i]){t=arr[i];arr[i]=arr[j];arr[j]=t} return (n%2)?arr[int(n/2)]:(arr[n/2-1]+arr[n/2])/2 }
    function tval(df){ split("12.706 4.303 3.182 2.776 2.571 2.447 2.365 2.306 2.262 2.228",T," "); return (df>=1&&df<=10)?T[df]:(df>10?2.086:0) }
    END{
      printf "| Build | uname | rounds | TX median (Mbit/s) | TX vs %s | RX |\n", ref
      printf "|---|---|:--:|--:|--:|--:|\n"
      nr=0; for(rr in rounds) nr++
      n=split(order,ord," ")
      for(o=1;o<=n;o++){ L=ord[o]; if(!seen[L]) continue
        # abs TX median over rounds
        m=0; delete ta
        for(rr in rounds){ k=L"|"rr; if(k in tx){ta[m++]=tx[k]} }
        absm=median(ta,m)
        # per-round ratio vs ref
        c=0; sum=0; delete ra
        for(rr in rounds){ a=L"|"rr; b=ref"|"rr; if((a in tx)&&(b in tx)&&tx[b]>0){ ratio=tx[a]/tx[b]; ra[c]=ratio; sum+=ratio; c++ } }
        if(c>0){ mean=sum/c; ss=0; for(i=0;i<c;i++){d=ra[i]-mean; ss+=d*d}; sd=(c>1)?sqrt(ss/(c-1)):0; ci=(c>1)?tval(c-1)*sd/sqrt(c):0 }
        # rx median
        rm=split(rx[L],rl," "); for(i=0;i<rm;i++)rxa[i]=rl[i+1]; rxmed=median(rxa,rm)
        if(c>1) printf "| %s | %s | %d | %.1f | %.3f× ±%.3f | %.1f |\n", L, uname[L], m, absm, mean, ci, rxmed
        else    printf "| %s | %s | %d | %.1f | %.3f× (n<2) | %.1f |\n", L, uname[L], m, absm, (c?mean:1), rxmed
      }
      print ""
      print "Read: a ratio whose ±CI excludes 1.000 is a TX difference distinguishable from drift; otherwise it is noise. **Combined** driver+kernel+gcc evolution (not the driver in isolation — see header)."
    }' "$CSV"
} | tee "$REPORT"

cleanup
echo
log_ok "Done. TSV + RESULTS.md in: $OUT_DIR"
