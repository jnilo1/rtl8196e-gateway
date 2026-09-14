#!/bin/bash
# bench_release_iperf3.sh — portable per-release iperf3 throughput benchmark
#
# Derived from test_rtl8196e_eth_iperf3.sh, but built to be (a) reproducible
# release-to-release and (b) portable across firmwares — including the stock
# Lidl gateway firmware. Differences from the sibling:
#
#   * NO ethtool. All gateway-side counters come from /proc/net/dev and
#     /proc/net/snmp, which exist on every Linux (the ethtool -S kick/#99
#     counters are driver-specific and vary by release).
#   * The gateway-side iperf3 binary path is configurable via IPERF3_BIN
#     (so it can point at /tmp/iperf3 on a stock firmware, etc.).
#   * Multi-rep with MEDIAN reporting and a strict inter-session protocol:
#     each rep restarts a FRESH gateway iperf3 server + fresh client, >=30 s
#     for TCP, spaced by GAP — so the 11 TX reps surface the real run-to-run
#     spread (reusing one warm server back-to-back understates it).
#   * Radio/userland quiesce via /userdata/etc/init.d (restarted after): OTBR,
#     UART bridges, netwatch and the button poller. A surviving workload is a
#     hard failure, not a warning. SSH calls are time-bounded.
#
# Core suite (5 measurements, ~23 min): see the table in the plan / README.
#   1. TCP RX  host->gw  11 reps   (median Mbit/s)
#   2. TCP TX  gw->host  11 reps   (median + spread/sd)
#   3. TCP stress RX     1x 300 s  (sustained + retrans + err/drop)
#   4. UDP TX  gw->host   3 reps   (-b 0,    median delivered + loss%)
#   5. UDP RX  host->gw   3 reps   (-b 100M, median delivered ceiling + loss%)
#
# Output: raw logs in a timestamped dir under 32-Kernel/, plus a markdown
# block (stdout + RESULTS.md) whose medians feed a PERFORMANCE.md release
# row. The script edits no tracked file.
#
# Usage:   ./scripts/bench_release_iperf3.sh "<release label>"
#   stock: IPERF3_BIN=/tmp/iperf3 RTL8196E_IP=192.168.1.x \
#            ./scripts/bench_release_iperf3.sh "lidl-stock"
#   smoke: REPS_TX=2 REPS_RX=1 REPS_UDP=1 DUR_TCP=10 DUR_STRESS=20 \
#            ./scripts/bench_release_iperf3.sh smoke
#
# Requires iperf3 on host AND gateway. J. Nilo — June 2026

set -euo pipefail
export LC_ALL=C

# ── Configuration (all env-overridable) ───────────────────────────────
# Gateway address: RTL8196E_IP env > gateway.env > the last gateway installed
# or reached > its hostname > the historic 192.168.1.88 (see lib/gwconf.sh).
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/../../../lib/gwconf.sh"
RTL8196E_IP="${RTL8196E_IP:-$(gwconf_gateway_addr)}"
RTL8196E_USER="${RTL8196E_USER:-root}"
IPERF_PORT="${IPERF_PORT:-5201}"
IPERF3_BIN="${IPERF3_BIN:-iperf3}"        # path to iperf3 ON THE GATEWAY
IFACE="${IFACE:-eth0}"
REPS_RX="${REPS_RX:-11}"
REPS_TX="${REPS_TX:-11}"
REPS_UDP="${REPS_UDP:-3}"
DUR_TCP="${DUR_TCP:-30}"
DUR_STRESS="${DUR_STRESS:-300}"
DUR_UDP="${DUR_UDP:-20}"
GAP="${GAP:-10}"          # inter-session gap: each rep is an independent session
QUIESCE_SETTLE="${QUIESCE_SETTLE:-15}" # quiet time after stopping radio/userland
UDP_TX_RATE="${UDP_TX_RATE:-0}"           # 0 = unlimited -> gw send ceiling
UDP_RX_RATE="${UDP_RX_RATE:-100M}"        # above the rcvbuf ceiling
QUIESCE_RADIO="${QUIESCE_RADIO:-auto}"    # stop OTBR / uart-bridge / serialgateway during the run
ALLOW_WIRELESS="${ALLOW_WIRELESS:-0}"
RELEASE="${1:-unspecified}"
INITD=""                 # resolved /userdata/etc/init.d (or /etc/init.d) dir
QUIESCED=""              # radio init scripts we stopped (space-separated; restarted after)
RADIO_WARN=0             # 1 if a radio service is still active after quiesce

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${KERNEL_DIR}/test_results_release_iperf3_$(date +%Y%m%d_%H%M%S)"

# Regression thresholds.
#
# THR_RX_FLOOR was 93 until 2026-08-08, from before driver v2.23. That release
# made RX checksums a gated policy (rtl8196e_rx_set_csum): TCP is verified by
# the stack instead of trusting the switch's uncharacterised checksum bits. An
# isolated same-build A/B in July 2026 priced it at ~2% single-stream and ~4.4%
# at 8-stream saturation -- a deliberate integrity/throughput trade, documented
# in the driver's DESIGN.md and SPECIFICATIONS.md (the A/B itself is in the
# archived performance record, PERFORMANCE.md "History").
#
# So the ~93.5-94 line-rate the old floor encoded is simply no longer reachable
# by design. Post-v2.23 the shipping kernel sits at 89.5-91.7, and across every
# kernel tried on 2026-08-07/08 -- 6.18.38/.41/.42/.43, 7.1.3/.7, with and
# without WireGuard, with and without I-MEM -- nothing exceeded 91.7. A floor
# nothing can clear fires on every run and gets ignored, which is how a real
# regression slips past. 88 sits about 1.5 below the lowest normal value, clear
# of the +/-0.9 same-binary repeatability measured by the null control.
THR_TX_FLOOR=69
THR_RX_FLOOR=88

# ── Colors & logging ──────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; CYAN='\033[0;36m'; NC='\033[0m'
log(){ echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} $1"; }
log_ok(){ echo -e "${GREEN}[$(date +%H:%M:%S)] ✓${NC} $1"; }
log_err(){ echo -e "${RED}[$(date +%H:%M:%S)] ✗${NC} $1"; }
log_warn(){ echo -e "${YELLOW}[$(date +%H:%M:%S)] !${NC} $1"; }
log_info(){ echo -e "${CYAN}[$(date +%H:%M:%S)] ℹ${NC} $1"; }

# ── SSH wrapper: every gateway call is time-bounded ───────────────────
gw(){
  ssh -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 \
      -o BatchMode=yes "${RTL8196E_USER}@${RTL8196E_IP}" "$@"
}

# ── Parsing helpers ───────────────────────────────────────────────────
# Last "X.Y Mbits/sec" on a line ending in "receiver", normalized to Mbit/s.
iperf3_receiver_mbps(){
  grep -E "[0-9]+(\.[0-9]+)?[[:space:]]+[KMG]bits/sec.*receiver[[:space:]]*$" "$1" 2>/dev/null | \
    awk '{for(i=1;i<=NF;i++) if($i ~ /bits\/sec/){u=$i; v=$(i-1); if(u~/Kbits/)v=v/1000; else if(u~/Gbits/)v=v*1000; print v; exit}}' | tail -1
}
# UDP loss percentage from the receiver line "(X%)" — "NA" if absent.
iperf3_udp_loss_pct(){
  local p
  p=$(grep -E "receiver" "$1" 2>/dev/null | grep -oE '\(([0-9]+(\.[0-9]+)?)%\)' | tr -d '()%' | tail -1)
  echo "${p:-NA}"
}
# /proc/net/snmp Tcp:/Udp: value by header name (column-index robust).
snmp_value(){  # file proto field
  awk -v pr="$2:" -v key="$3" '
    $1==pr && hdr==0 { for(i=2;i<=NF;i++) idx[$i]=i; hdr=1; next }
    $1==pr && hdr==1 { if(idx[key]>0){print $idx[key]; exit} }
  ' "$1" 2>/dev/null || echo 0
}
# /proc/net/dev counter for IFACE. Columns after ':' collapse:
# 1=rxbytes 2=rxpkts 3=rxerrs 4=rxdrop ... 9=txbytes 10=txpkts 11=txerrs 12=txdrop
procdev_val(){  # file iface col
  awk -v ifc="$2" -v c="$3" '{gsub(/:/," ")} $1==ifc{print $(c+1); exit}' "$1" 2>/dev/null || echo 0
}
delta(){ local d=$(( ${1:-0} - ${2:-0} )); [ "$d" -lt 0 ] && d=$(( (${1:-0}+4294967296) - ${2:-0} )); echo "$d"; }

# stdin: one number per line -> "median mean min max spread sd"
summarize(){
  awk '
    /^[0-9]/{v[n++]=$1; s+=$1; if(mn==""||$1<mn)mn=$1; if($1>mx)mx=$1}
    END{
      if(n==0){print "NA NA NA NA NA NA"; exit}
      for(i=0;i<n;i++)for(j=i+1;j<n;j++)if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t}
      med=(n%2)?v[int(n/2)]:(v[n/2-1]+v[n/2])/2; mean=s/n
      for(i=0;i<n;i++){d=v[i]-mean; ss+=d*d}; sd=(n>1)?sqrt(ss/n):0
      printf "%.1f %.2f %.1f %.1f %.1f %.2f", med, mean, mn, mx, mx-mn, sd
    }'
}

# Restart the gateway iperf3 server. Done before EVERY rep so each rep is a
# genuinely independent session (fresh server + fresh client + the inter-session
# gap) — that is what surfaces the real run-to-run TX spread; reusing one warm
# server back-to-back understates it.
restart_server(){
  gw "killall iperf3 2>/dev/null; $IPERF3_BIN -s -p $IPERF_PORT -D </dev/null >/dev/null 2>&1"
  sleep 1
}

# ── One iperf3 client run (host side) -> prints delivered Mbit/s ──────
run_iperf(){  # label idx duration extra-args...
  local label=$1 idx=$2 dur=$3; shift 3
  restart_server
  local logf="$LOG_DIR/${label}_${idx}.log"
  set +e
  timeout --kill-after=5 $((dur + 15)) \
    iperf3 -c "$RTL8196E_IP" -p "$IPERF_PORT" -t "$dur" "$@" > "$logf" 2>&1
  set -e
  iperf3_receiver_mbps "$logf"
}

# ── Radio-path quiesce: OTBR, in-kernel uart-bridge, legacy serialgateway ──
# All three put traffic on eth0 / contend for CPU and must be off for a clean
# eth bench. Each is launched from an init script under /userdata/etc/init.d;
# detection differs per service (the agent process, the bridge 'armed' sysfs
# flag, the serialgateway daemon). pgrep is unreliable for otbr-agent on this
# BusyBox, so match via ps. Only services found ACTIVE are stopped, and only
# those are restarted afterwards (never start something that was not running).
radio_active(){  # init-script basename -> exit 0 if its service is active
  case "$1" in
    S70otbr) gw "ps w 2>/dev/null | grep '/otbr-agent ' | grep -v keepalive | grep -v grep" >/dev/null 2>&1 ;;
    S50uart_bridge) gw "[ \"\$(cat /sys/module/rtl8196e_uart_bridge/parameters/armed 2>/dev/null)\" = 1 ]" >/dev/null 2>&1 ;;
    S50serialgateway|S60serialgateway) gw "ps w 2>/dev/null | grep '[s]erialgateway'" >/dev/null 2>&1 ;;
    *) return 1 ;;
  esac
}
radio_any_active(){ for s in S70otbr S50uart_bridge S50serialgateway S60serialgateway; do radio_active "$s" && return 0; done; return 1; }
radio_restore(){ [ -z "$QUIESCED" ] && return 0; local s; for s in $QUIESCED; do gw "$INITD/$s start" >/dev/null 2>&1 || true; done; }
cleanup(){ gw "killall iperf3 2>/dev/null" >/dev/null 2>&1 || true; killall iperf3 2>/dev/null || true; }
trap 'echo; log_warn "interrupted"; cleanup; radio_restore; exit 1' INT TERM

# ══ Preconditions ═════════════════════════════════════════════════════
command -v iperf3 >/dev/null || { log_err "iperf3 not installed on host"; exit 1; }

route_dev=$(ip route get "$RTL8196E_IP" 2>/dev/null | grep -oE 'dev[[:space:]]+[^ ]+' | awk '{print $2}' | head -1)
case "$route_dev" in
  wlan*|wl*)
    if [ "$ALLOW_WIRELESS" = "1" ]; then
      log_warn "route to $RTL8196E_IP is wireless ($route_dev) — ALLOW_WIRELESS=1, continuing"
    else
      log_err "route to $RTL8196E_IP is wireless ($route_dev) — Wi-Fi silently invalidates results."
      log_err "Use the wired link, or set ALLOW_WIRELESS=1 to override."; exit 1
    fi ;;
  "") log_warn "could not determine route device to $RTL8196E_IP" ;;
  *)  log_info "wired link via $route_dev" ;;
esac

gw "echo ok" >/dev/null 2>&1 || { log_err "cannot SSH to ${RTL8196E_USER}@${RTL8196E_IP}"; exit 1; }
GW_IPERF_VER=$(gw "$IPERF3_BIN --version" 2>&1 | head -1) || true
case "$GW_IPERF_VER" in
  iperf*) log_info "gateway iperf3: $GW_IPERF_VER (via $IPERF3_BIN)" ;;
  *) log_err "iperf3 not runnable on gateway at '$IPERF3_BIN' (set IPERF3_BIN)"; exit 1 ;;
esac

mkdir -p "$LOG_DIR"
GW_UNAME=$(gw "uname -a" 2>/dev/null || echo "uname: n/a"); echo "$GW_UNAME" > "$LOG_DIR/uname.txt"
dmesg_before="$LOG_DIR/dmesg_before.txt"
gw "dmesg" > "$dmesg_before" 2>&1 \
  || { log_err "cannot read the kernel log before measurement"; exit 1; }
if grep -Eiq 'WARNING:|BUG:|Oops:|Kernel panic|can.t patch jump_label|section mismatch' "$dmesg_before"; then
  log_err "kernel log contains a warning/oops relevant to release qualification"
  grep -Ein 'WARNING:|BUG:|Oops:|Kernel panic|can.t patch jump_label|section mismatch' "$dmesg_before" >&2
  exit 1
fi
log "Release label : $RELEASE"
log "Gateway       : ${RTL8196E_USER}@${RTL8196E_IP}  ($GW_UNAME)"
log "Host rig      : $(hostname) via ${route_dev:-?}"
log "Output        : $LOG_DIR"

# ══ Quiesce the radio path (best-effort; no-op on a firmware without it) ═
if [ "$QUIESCE_RADIO" = "auto" ]; then
  for d in /userdata/etc/init.d /etc/init.d; do
    if gw "[ -d $d ]" 2>/dev/null; then INITD="$d"; break; fi
  done
  if [ -n "$INITD" ]; then
    for svc in S70otbr S50uart_bridge S50serialgateway S60serialgateway; do
      gw "[ -x $INITD/$svc ]" 2>/dev/null || continue
      if radio_active "$svc"; then
        gw "$INITD/$svc stop" >/dev/null 2>&1 || true
        QUIESCED="$QUIESCED $svc"
        log_info "stopped $svc for the run (will restart after)"
      fi
    done
    for svc in S80netwatch S40button; do
      gw "[ -x $INITD/$svc ]" 2>/dev/null || continue
      gw "$INITD/$svc stop" >/dev/null 2>&1 || true
      QUIESCED="$QUIESCED $svc"
      log_info "stopped $svc for the run (will restart after)"
    done
    sleep 2
    if radio_any_active || gw "ps w | grep -E '[o]tbr-agent|[o]tbr-monitor|[s]40button|[s]erialgateway'" >/dev/null 2>&1; then
      log_err "a benchmark-disturbing userland process survived quiesce"
      QUIESCED="${QUIESCED# }"
      radio_restore
      exit 1
    fi
    [ -z "$QUIESCED" ] && [ "$RADIO_WARN" = 0 ] && log_info "no radio service active (nothing to stop)"
  else
    log_info "no init.d found (stock firmware?) — radio quiesce skipped"
  fi
fi
QUIESCED="${QUIESCED# }"   # trim leading space
cleanup
log_info "settling ${QUIESCE_SETTLE}s after radio/userland quiesce"
sleep "$QUIESCE_SETTLE"

# ══ Pre-suite counters (no ethtool) ═══════════════════════════════════
gw "cat /proc/net/dev"  > "$LOG_DIR/proc_dev_before.txt"  2>&1 || true
gw "cat /proc/net/snmp" > "$LOG_DIR/proc_snmp_before.txt" 2>&1 || true
# (the iperf3 server is (re)started fresh before each rep — see run_iperf)

# ══ 1. TCP RX (host -> gw) ════════════════════════════════════════════
log "[1/5] TCP RX  host->gw   ${REPS_RX} reps x ${DUR_TCP}s"
rx_vals=()
for i in $(seq 1 "$REPS_RX"); do
  v=$(run_iperf tcp_rx "$i" "$DUR_TCP" -i 1); rx_vals+=("$v")
  printf '      rep %2d: %s Mbit/s\n' "$i" "${v:-FAIL}"
  sleep "$GAP"
done
RX_STATS=$(printf '%s\n' "${rx_vals[@]}" | summarize)

# ══ 2. TCP TX (gw -> host, -R) ════════════════════════════════════════
log "[2/5] TCP TX  gw->host   ${REPS_TX} reps x ${DUR_TCP}s (-R)"
tx_vals=()
for i in $(seq 1 "$REPS_TX"); do
  v=$(run_iperf tcp_tx "$i" "$DUR_TCP" -R -i 1); tx_vals+=("$v")
  printf '      rep %2d: %s Mbit/s\n' "$i" "${v:-FAIL}"
  sleep "$GAP"
done
TX_STATS=$(printf '%s\n' "${tx_vals[@]}" | summarize)

# ══ 3. TCP stress (host -> gw, sustained) ═════════════════════════════
log "[3/5] TCP stress host->gw  1 x ${DUR_STRESS}s"
STRESS_VAL=$(run_iperf tcp_stress 1 "$DUR_STRESS" -i 10)
printf '      sustained: %s Mbit/s\n' "${STRESS_VAL:-FAIL}"
sleep "$GAP"

# Mid snapshot: TCP phase done, UDP flood not yet started. err/drop must be 0
# across the TCP window; the UDP-RX line-rate flood (-b 100M) that follows
# legitimately overflows the RX ring, so its drops are reported but NOT flagged.
gw "cat /proc/net/dev"  > "$LOG_DIR/proc_dev_mid.txt"  2>&1 || true
gw "cat /proc/net/snmp" > "$LOG_DIR/proc_snmp_mid.txt" 2>&1 || true

# ══ 4. UDP TX (gw -> host, -b 0) ══════════════════════════════════════
log "[4/5] UDP TX  gw->host   ${REPS_UDP} reps x ${DUR_UDP}s (-b ${UDP_TX_RATE})"
utx_vals=(); utx_loss=()
for i in $(seq 1 "$REPS_UDP"); do
  v=$(run_iperf udp_tx "$i" "$DUR_UDP" -u -R -b "$UDP_TX_RATE" -i 1)
  l=$(iperf3_udp_loss_pct "$LOG_DIR/udp_tx_${i}.log")
  utx_vals+=("$v"); utx_loss+=("$l")
  printf '      rep %2d: %s Mbit/s  loss %s%%\n' "$i" "${v:-FAIL}" "$l"
  sleep "$GAP"
done
UTX_STATS=$(printf '%s\n' "${utx_vals[@]}" | summarize)
UTX_LOSS=$(printf '%s\n' "${utx_loss[@]}" | grep -E '^[0-9]' | summarize | awk '{print $1}')

# ══ 5. UDP RX (host -> gw, -b 100M) ═══════════════════════════════════
log "[5/5] UDP RX  host->gw   ${REPS_UDP} reps x ${DUR_UDP}s (-b ${UDP_RX_RATE})"
urx_vals=(); urx_loss=()
for i in $(seq 1 "$REPS_UDP"); do
  v=$(run_iperf udp_rx "$i" "$DUR_UDP" -u -b "$UDP_RX_RATE" -i 1)
  l=$(iperf3_udp_loss_pct "$LOG_DIR/udp_rx_${i}.log")
  urx_vals+=("$v"); urx_loss+=("$l")
  printf '      rep %2d: %s Mbit/s  loss %s%%\n' "$i" "${v:-FAIL}" "$l"
  sleep "$GAP"
done
URX_STATS=$(printf '%s\n' "${urx_vals[@]}" | summarize)
URX_LOSS=$(printf '%s\n' "${urx_loss[@]}" | grep -E '^[0-9]' | summarize | awk '{print $1}')

# ══ Stop server + post-suite counters, then restore the radio path ════
cleanup
gw "cat /proc/net/dev"  > "$LOG_DIR/proc_dev_after.txt"  2>&1 || true
gw "cat /proc/net/snmp" > "$LOG_DIR/proc_snmp_after.txt" 2>&1 || true
if [ -n "$QUIESCED" ]; then radio_restore; log_info "restarted radio service(s): $QUIESCED"; fi

# ── Counter deltas (no ethtool) ──
DB="$LOG_DIR/proc_dev_before.txt"; DM="$LOG_DIR/proc_dev_mid.txt"; DA="$LOG_DIR/proc_dev_after.txt"
SB="$LOG_DIR/proc_snmp_before.txt"; SM="$LOG_DIR/proc_snmp_mid.txt"
# TCP window (before -> mid): err/drop here are real -> flagged.
t_rxerr=$(delta "$(procdev_val "$DM" "$IFACE" 3)"  "$(procdev_val "$DB" "$IFACE" 3)")
t_rxdrp=$(delta "$(procdev_val "$DM" "$IFACE" 4)"  "$(procdev_val "$DB" "$IFACE" 4)")
t_txerr=$(delta "$(procdev_val "$DM" "$IFACE" 11)" "$(procdev_val "$DB" "$IFACE" 11)")
t_txdrp=$(delta "$(procdev_val "$DM" "$IFACE" 12)" "$(procdev_val "$DB" "$IFACE" 12)")
d_out=$(delta "$(snmp_value "$SM" Tcp OutSegs)"     "$(snmp_value "$SB" Tcp OutSegs)")
d_re=$(delta  "$(snmp_value "$SM" Tcp RetransSegs)" "$(snmp_value "$SB" Tcp RetransSegs)")
RETR_PCT=$(awk -v r="$d_re" -v o="$d_out" 'BEGIN{printf "%.4f", (o>0)? 100*r/o : 0}')
# UDP flood window (mid -> after): line-rate drops expected -> informational only.
u_rxdrp=$(delta "$(procdev_val "$DA" "$IFACE" 4)" "$(procdev_val "$DM" "$IFACE" 4)")
u_rxerr=$(delta "$(procdev_val "$DA" "$IFACE" 3)" "$(procdev_val "$DM" "$IFACE" 3)")

# ── Regression flags ──
tx_med=$(echo "$TX_STATS" | awk '{print $1}')
rx_med=$(echo "$RX_STATS" | awk '{print $1}')
flags=""
awk -v v="$tx_med" -v t="$THR_TX_FLOOR" 'BEGIN{exit !(v<t)}' && flags="${flags}TX<${THR_TX_FLOOR} "
awk -v v="$rx_med" -v t="$THR_RX_FLOOR" 'BEGIN{exit !(v<t)}' && flags="${flags}RX<${THR_RX_FLOOR} "
[ "$d_re" -gt 0 ]    && flags="${flags}retrans>0 "
[ "$t_rxerr" -gt 0 ] && flags="${flags}rx_errs "
[ "$t_rxdrp" -gt 0 ] && flags="${flags}rx_drop "
[ "$t_txerr" -gt 0 ] && flags="${flags}tx_errs "
[ "$t_txdrp" -gt 0 ] && flags="${flags}tx_drop "
[ "$RADIO_WARN" = 1 ] && flags="${flags}radio-active(results-perturbed) "
[ -z "$flags" ] && flags="PASS (all within thresholds)"

# ══ Markdown report block (stdout + RESULTS.md) ═══════════════════════
read -r RXM _ RXmn RXmx _ _ <<<"$RX_STATS"
read -r TXM _ TXmn TXmx TXspr TXsd <<<"$TX_STATS"
read -r UTXM _ _ _ _ _ <<<"$UTX_STATS"
read -r URXM _ _ _ _ _ <<<"$URX_STATS"

REPORT="$LOG_DIR/RESULTS.md"
{
  echo "### ${RELEASE} — iperf3 release bench ($(date +%Y-%m-%d))"
  echo
  echo "\`${GW_UNAME}\`"
  echo
  echo "Rig: host \`$(hostname)\` direct via \`${route_dev:-?}\`, radio quiesced: \`${QUIESCED:-none}\`, iface \`${IFACE}\`, iperf3 server on gateway (\`${IPERF3_BIN}\`). Inter-session: **fresh server + fresh client per rep**, ${DUR_TCP}s TCP, ${GAP}s gap, **median** reported."
  echo
  echo "| Workload | Reps | Median (Mbit/s) | Spread / loss |"
  echo "|---|:--:|--:|---|"
  echo "| TCP RX (host→gw)        | ${REPS_RX} | ${RXM} | range ${RXmn}–${RXmx} |"
  echo "| **TCP TX (gw→host)**    | ${REPS_TX} | **${TXM}** | spread ${TXspr}, sd ${TXsd}, range ${TXmn}–${TXmx} |"
  echo "| TCP stress (host→gw, ${DUR_STRESS}s) | 1 | ${STRESS_VAL} | retrans ${RETR_PCT}% |"
  echo "| UDP TX (gw→host, -b ${UDP_TX_RATE}) | ${REPS_UDP} | ${UTXM} | loss ${UTX_LOSS}% |"
  echo "| UDP RX (host→gw, -b ${UDP_RX_RATE}) | ${REPS_UDP} | ${URXM} | loss ${URX_LOSS}% |"
  echo
  echo "Counters (from /proc, no ethtool) — **TCP phase, must be 0**: rx_errs +${t_rxerr}, rx_drop +${t_rxdrp}, tx_errs +${t_txerr}, tx_drop +${t_txdrp}; TCP RetransSegs +${d_re} of +${d_out} (${RETR_PCT}%). UDP flood phase (drops expected at line-rate): rx_drop +${u_rxdrp}, rx_errs +${u_rxerr}."
  echo
  echo "Regression flags: ${flags}"
} | tee "$REPORT"

echo
log_ok "Done. Raw logs + RESULTS.md in: $LOG_DIR"
