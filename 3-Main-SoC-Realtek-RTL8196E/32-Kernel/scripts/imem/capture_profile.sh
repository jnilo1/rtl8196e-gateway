#!/bin/bash
# Capture the bounded PC-sampling profile used by the simplified I-MEM flow.
#
# The target must already run the empty-I-MEM profiling image.  Workloads are
# driven from the host; no SSH command runs between resetting and reading a
# profile histogram.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_ROOT="$(git -C "$KERNEL_DIR" rev-parse --show-toplevel)"
# shellcheck disable=SC1091
. "$REPO_ROOT/lib/gwconf.sh"

DURATION=180
OUTPUT=""
TARGET=""
EXPECT_RELEASE=""
STABILIZE=45
LOCAL_IPERF="$KERNEL_DIR/../34-Userdata/iperf3/iperf3"
REMOTE_IPERF=/tmp/imem-iperf3

usage() {
	cat <<'EOF'
Usage: capture_profile.sh [options] [gateway-ip]

Options:
  --duration SEC       Duration of each capture (default: 180)
  --output DIR         Output directory
  --expect RELEASE     Required uname -r prefix, for example 6.18.51-
  --stabilize SEC      Delay after SSH first answers (default: 45)
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--duration) DURATION="$2"; shift 2 ;;
		--output) OUTPUT="$2"; shift 2 ;;
		--expect) EXPECT_RELEASE="$2"; shift 2 ;;
		--stabilize) STABILIZE="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		-*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
		*) [ -z "$TARGET" ] || { echo "more than one gateway address" >&2; exit 2; }
			TARGET="$1"; shift ;;
	esac
done

case "$DURATION:$STABILIZE" in
	*[!0-9:]*|:*) echo "duration and stabilization must be integer seconds" >&2; exit 2 ;;
esac

if [ -z "$TARGET" ]; then
	gwconf_resolve_gateway RTL8196E_IP
	TARGET="$GWCONF_ADDR"
fi
OUTPUT="${OUTPUT:-$KERNEL_DIR/imem-work/profile-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUTPUT"

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no "root@$TARGET")

say() { printf '\n=== %s ===\n' "$*"; }
remote() { "${SSH[@]}" "$@"; }

say "waiting for Linux at $TARGET"
ready=0
for _ in $(seq 1 90); do
	if remote 'true' >/dev/null 2>&1; then ready=1; break; fi
	sleep 2
done
[ "$ready" -eq 1 ] || { echo "gateway did not become reachable" >&2; exit 1; }

release="$(remote 'uname -r')"
cmdline="$(remote 'cat /proc/cmdline')"
model="$(remote 'cat /proc/device-tree/model' | xargs -0 printf '%s')"
printf 'target: %s\nmodel: %s\nrelease: %s\ncmdline: %s\n' \
	"$TARGET" "$model" "$release" "$cmdline" | tee "$OUTPUT/identity.txt"

[ -z "$EXPECT_RELEASE" ] || case "$release" in
	"$EXPECT_RELEASE"*) ;;
	*) echo "unexpected kernel release: $release (wanted $EXPECT_RELEASE*)" >&2; exit 1 ;;
esac
case "$cmdline" in
	*profile=4*) ;;
	*) echo "profiling image is missing profile=4" >&2; exit 1 ;;
esac
remote 'test -r /proc/profile && test -w /proc/profile' \
	|| { echo "/proc/profile is not readable and writable" >&2; exit 1; }
if ! remote "$REMOTE_IPERF --version" >/dev/null 2>&1; then
	[ -x "$LOCAL_IPERF" ] || { echo "gateway iperf3 is absent and $LOCAL_IPERF is unavailable" >&2; exit 1; }
	scp -q -O -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
		"$LOCAL_IPERF" "root@$TARGET:$REMOTE_IPERF"
	remote "chmod 755 $REMOTE_IPERF"
fi

say "stabilizing for $STABILIZE seconds"
sleep "$STABILIZE"

say "quiescing services"
# The single-quoted variables below are intentionally expanded by the remote
# shell, not by this host-side script.
# shellcheck disable=SC2016
remote 'for s in S80netwatch S70otbr S50uart_bridge S40button; do
          [ -x /userdata/etc/init.d/$s ] && /userdata/etc/init.d/$s stop >/dev/null 2>&1
        done
        rm -f /userdata/thread/* 2>/dev/null
        sleep 2
        if ps w | grep -E "[o]tbr-agent|[o]tbr-monitor|[s]40button|[s]erialgateway"; then
          echo "FATAL: a benchmark-disturbing userland process survived quiesce" >&2
          exit 42
        fi
        if [ -r /sys/module/rtl8196e_uart_bridge/parameters/armed ] &&
           [ "$(cat /sys/module/rtl8196e_uart_bridge/parameters/armed)" = 1 ]; then
          echo "FATAL: the UART bridge survived quiesce" >&2
          exit 42
        fi
        echo "still running:"
        ps | grep -vE "\[|ps$|sh$|dropbear|grep" | tail -n +2' \
	| tee "$OUTPUT/quiesce.txt"
sleep 15

serve() {
	remote "killall iperf3 2>/dev/null || true; $REMOTE_IPERF -s -p 5201 -D </dev/null >/dev/null 2>&1"
	sleep 1
}

capture() {
	local tag="$1" mode="$2"
	local load="$OUTPUT/$tag-load.txt"
	say "$tag ($mode, $DURATION seconds)"
	if [ "$mode" != idle ]; then serve; fi
	remote 'echo 0 > /proc/profile'
	case "$mode" in
		idle) sleep "$DURATION" ;;
		tx) iperf3 -c "$TARGET" -p 5201 -R -t "$DURATION" >"$load" 2>&1 || true ;;
		rx) iperf3 -c "$TARGET" -p 5201    -t "$DURATION" >"$load" 2>&1 || true ;;
		*) echo "internal error: unknown mode $mode" >&2; exit 2 ;;
	esac
	remote 'cat /proc/profile' >"$OUTPUT/$tag.bin"
	[ -s "$OUTPUT/$tag.bin" ] || { echo "empty profile: $tag" >&2; exit 1; }
	if [ "$mode" != idle ]; then
		if ! grep -qE 'receiver[[:space:]]*$' "$load"; then
			echo "no measured throughput for $tag; refusing an idle capture" >&2
			sed -n '1,8p' "$load" >&2
			exit 1
		fi
		grep -E 'sender|receiver' "$load" | tee -a "$OUTPUT/throughput.txt"
	fi
	sleep 5
}

capture idle1 idle
capture tx1 tx
capture rx1 rx
capture tx2 tx
capture rx2 rx
capture idle2 idle

say "collecting terminal evidence"
remote 'uname -a; cat /proc/cmdline; uptime; dmesg | tail -200' >"$OUTPUT/target-final.txt" 2>&1
sha256sum "$OUTPUT"/*.bin "$OUTPUT/identity.txt" "$OUTPUT/quiesce.txt" \
	>"$OUTPUT/SHA256SUMS"
printf 'profile complete: %s\n' "$OUTPUT"
