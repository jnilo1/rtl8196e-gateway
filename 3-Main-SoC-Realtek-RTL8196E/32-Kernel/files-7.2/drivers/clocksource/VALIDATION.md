# RTL8196E timer v1.3 — production validation

## Static gate

- both 6.18 and 7.1 Kconfig patches apply from clean;
- build `timer-rtl819x.o` with `W=1` for `RTL819X_TC0_DNT=n` and `=y`;
- run checkpatch and compile the RTL8196E DTB;
- inspect DNT disassembly: DATA0 store precedes IR unmask; only T1 writes TC0
  CTRL after boot; ISR and shutdown write IR only.

## Required configuration

`TICK_ONESHOT`, `NO_HZ_COMMON`, `NO_HZ_IDLE` and high-resolution timers must
be enabled. `RTL819X_TC0_DNT` defaults to `n`; canary images set it to `y` at
build time only. HZ remains the branch's configured value.

## Hardware canary

For both protected and DNT images, record build identity, board, config and
host time. Verify cold and warm boot, Timer1 clocksource selection, Timer0 IRQ
progress, watchdog probe, 20 s/300 s elapsed time, minimum/maximum timerfd
deadlines, TC1 wrap, and repeated idle/wakeup transitions.

For DNT additionally verify that NO_HZ shutdown leaves TC0 running with IRQ
masked; the next deadline is delivered exactly once; no stale IRQ occurs at
unmask; and no callback writes TC0 CTRL after entry.

## Load acceptance

Run wired bidirectional saturation plus ordinary gateway services. Capture
throughput, Timer0 IRQ rate, deadline latency, `nr_hangs`, UART health and
watchdog activity. The DNT cause-level reference is 26.3 h and 54.8 million
valid reprograms with zero wedge/WDT, missed, double or parasitic IRQ. A new
release passes only after at least 24 h **and** 20 million valid reprograms in
the exact release configuration, with zero such failures.

`pend_at_unmask` at the registered 8-tick floor is telemetry: it is acceptable
only when the pending event is delivered exactly once and no latency or load
regression correlates with it. Preserve raw post-mortem data on any reboot or
IRQ anomaly. `irqtime=1` remains a separate diagnostic A/B, never a silent
production default.

## Ethernet A/B reference — 2026-07-29

This is a board-specific regression reference, not a throughput guarantee. On
one RTL8196E board, timer v1.3 was tested three times with each build-time
choice of `RTL819X_TC0_DNT`. The board was rebooted and OTBR stopped before
each run. The wired full iperf3 suite was otherwise identical.

| Metric | DNT=n (mean) | DNT=y (mean) | Change |
| --- | ---: | ---: | ---: |
| TCP host to board | 89.2 Mbit/s | 91.4 Mbit/s | +2.5% |
| TCP board to host | 68.6 Mbit/s | 69.7 Mbit/s | +1.7% |
| TCP 300 s stress | 90.5 Mbit/s | 92.4 Mbit/s | +2.1% |
| UDP 50 Mbit/s receive | 43.1 Mbit/s | 47.1 Mbit/s | +9.4% |
| UDP 50 Mbit/s receive loss | 14.0% | 5.7% | -8.3 pp |
| UDP 100 Mbit/s receive | 31.4 Mbit/s | 33.7 Mbit/s | +7.4% |
| UDP bidirectional, host to board | 19.6 Mbit/s | 21.5 Mbit/s | +9.9% |

Four-stream TCP was effectively unchanged (92.4 versus 93.2 Mbit/s); the
eight-stream result varied in the opposite direction (91.9 versus 90.3
Mbit/s) and is not evidence of a regression with three samples. Ethernet
hardware errors remained zero and interface drops stayed approximately
constant (301--302). The UDP loss is predominantly socket receive-buffer
pressure (`RcvbufErrors`), not a new Ethernet fault.

The result is consistent with DNT removing the protected COUNTER-mode
start-observation work from the steady-state arm path. It does not establish
that every workload or board gains the same amount. Re-run this paired A/B
when changing the timer, IRQ, Ethernet RX, CPU-frequency, or NO_HZ
configuration.

## Kernel 7.1 reference with DNT — 2026-07-29

The same three-run wired suite was then run with `RTL819X_TC0_DNT=y` in both
branches and the same operational conditions (reboot and OTBR stopped before
each run). This is an inter-branch reference: it does **not** isolate DNT,
because DNT is enabled in both images and the kernel, compiler and other
branch configuration differ.

| Metric | 6.18 DNT=y (mean) | 7.1 DNT=y (mean) | Change |
| --- | ---: | ---: | ---: |
| TCP host to board | 91.4 Mbit/s | 89.8 Mbit/s | -1.8% |
| TCP board to host | 69.7 Mbit/s | 70.4 Mbit/s | +1.0% |
| TCP 300 s stress | 92.4 Mbit/s | 90.1 Mbit/s | -2.5% |
| UDP 50 Mbit/s receive | 47.1 Mbit/s | 49.7 Mbit/s | +5.6% |
| UDP 50 Mbit/s receive loss | 5.7% | 0.5% | -5.2 pp |
| UDP 100 Mbit/s receive | 33.7 Mbit/s | 39.9 Mbit/s | +18.3% |
| UDP 100 Mbit/s receive loss | 64.7% | 58.0% | -6.7 pp |
| UDP bidirectional, host to board | 21.5 Mbit/s | 22.6 Mbit/s | +5.3% |

The 7.1 image reached the requested 50 Mbit/s UDP receive rate with zero loss
in two of three runs (the third lost 1.5%). Interface hardware errors remained
zero and interface drops remained approximately 302. TCP is comparable across
the branches: the small opposing changes require more repetitions before any
branch-wide TCP performance claim. For this board and workload, 7.1 with DNT
is the preferred candidate for follow-up UDP and bidirectional testing.
