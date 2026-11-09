# RTL8196E watchdog (`rtl819x-wdt`)

This is the production driver for the RTL8196E hardware watchdog. It exposes
the normal Linux `/dev/watchdog` interface and resets the SoC when the feeder
stops. Its production objective is recovery, not live forensic tracing.

## Normal operation

The BusyBox feeder opens `/dev/watchdog` and pings it every 30 seconds. The
driver reports a 60-second timeout, but that value only sets the feeder
cadence: the hardware always runs in its longest bucket (OVSEL=9, about
671 seconds at the 25 kHz timer clock), advertised to the watchdog core as
`max_hw_heartbeat_ms`. Because 60 s is below that window, the core does not
enforce 60 s itself. If the feeder stops, the reset comes from the hardware
counter, about 671 seconds (roughly 11 minutes) after the last ping.
`nowayout` remains a read-only boot-time module parameter.

The watchdog also covers kernel panics and machine restarts:

| Event | Driver action |
|---|---|
| feeder stops | nothing refreshes the hardware; reset about 671 s after the last ping |
| `panic()` / soft-lockup panic | arm the shortest hardware reset window |
| restart callback | arm the shortest hardware reset window |

The short hardware window is about 1.31 seconds. The reset sequence writes
the disable/clear pattern and then zero, which avoids reusing a stale hardware
counter state on this silicon.

## Compact post-mortem record

On a panic, the driver first arms recovery and then writes one bounded,
108-byte record. It contains uptime, EPC/RA, CP0 cause/status, pending
softirqs, WDTCNR, flags, and a sanitized panic reason. The magic word is
written last, so a torn write is discarded at the next boot.

The record resides in the dedicated `watchdog-crash@1ffd000` no-map page.
It is deliberately separate from `boothold@1ffe000`: the bootloader handoff
words and the watchdog record cannot overwrite one another. A missing or
invalid `memory-region` disables only this optional record; the watchdog
continues to operate.

At probe, a valid record is emitted as one `previous panic:` dmesg line and
then cleared. Older record formats are discarded rather than decoded.

## Quick checks

```sh
dmesg | grep -E 'rtl819x-wdt|previous panic'
cat /sys/class/watchdog/watchdog0/identity
cat /sys/class/watchdog/watchdog0/timeout
ls -l /dev/watchdog
```

The expected probe line identifies driver v1.12, its configured timeout, and
whether `nowayout` is active.

For implementation and audit rationale, see [`DESIGN.md`](DESIGN.md) and
[`AUDIT.md`](AUDIT.md).
