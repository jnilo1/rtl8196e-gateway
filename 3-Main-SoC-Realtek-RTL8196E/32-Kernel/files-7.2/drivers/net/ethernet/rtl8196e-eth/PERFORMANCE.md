# RTL8196E Ethernet Driver — Performance

All rows: standard release bench (`scripts/bench_release_iperf3.sh`), one image per
row, fresh boot, radio and userland quiesced, no gateway access during a measurement,
host direct-cabled to the gateway. TCP: 11 reps × 30 s, median (min–max). UDP:
3 reps × 20 s, median delivered rate and receiver loss. Every row recorded zero TCP
retransmissions and zero hard-counter failures. Rows marked ⁽¹⁾ predate the standard
bench and were measured with a shorter protocol (see the notes under each table).

Absolute levels move by a few Mbit/s between sessions; a difference between two rows
is only a finding when it was measured paired in the same session; those paired runs
live in the driver's performance archive.

## Stable line (Linux 6.18, production default)

| Release | Kernel | TCP RX host to gateway | TCP TX gateway to host | UDP RX at 100 Mbit/s offered | UDP TX unconstrained |
|---|---|---:|---:|---:|---:|
| v3.10.0 | 6.18.35 | 93.8 Mbit/s (93.2–94.0) | 68.7 Mbit/s (68.3–72.1) | 30.5 Mbit/s, 68% loss | 32.6 Mbit/s, 0% loss |
| v4.0.0 | 6.18.41 | 92.4 Mbit/s (91.8–92.6) | 71.4 Mbit/s (70.3–72.7) | 33.3 Mbit/s, 65% loss | 33.6 Mbit/s, 0% loss |
| v4.2.0 | 6.18.45 | 91.7 Mbit/s (91.1–92.5) | 82.8 Mbit/s (81.2–83.6) | 40.7 Mbit/s, 57% loss | 36.7 Mbit/s, 0% loss |
| v4.5.0 | 6.18.51 | **93.5 Mbit/s (92.5–93.9)** | **82.8 Mbit/s (81.7–84.3)** | **41.5 Mbit/s, 57% loss** | **37.2 Mbit/s, 0% loss** |

Notes:
- v3.10.0, v4.0.0 and v4.2.0 were re-measured on the same day (2026-08-22) on the
  same rig with the final harness, so those three rows are directly comparable.
- From v4.2.0 on, the kernel places its hottest network code in the SoC's on-chip
  instruction memory; that is the TCP TX step between v4.0.0 and v4.2.0.
- TCP RX from v4.0.0 on includes the deliberate driver v2.23 checksum-integrity policy
  (about −2 %); it is not a regression.

## Development line (Linux 7.x, experimental)

| Release | Kernel | TCP RX host to gateway | TCP TX gateway to host | UDP RX at 100 Mbit/s offered | UDP TX unconstrained |
|---|---|---:|---:|---:|---:|
| pre-4.0.0, never tagged ⁽¹⁾ | 7.1.3 | 88.6 Mbit/s (88.2–88.9) | 69.6 Mbit/s (68.1–70.4) | — | — |
| v4.0.0 ⁽¹⁾ | 7.1.7 | 88.8 Mbit/s (87.9–88.9) | 70.0 Mbit/s (69.0–71.2) | 35.5 Mbit/s, 63% loss | 31.4 Mbit/s, 0% loss |
| v4.2.0 | 7.1.9 | 92.8 Mbit/s (91.8–93.4) | 82.8 Mbit/s (81.4–84.2) | 43.2 Mbit/s, 55% loss | 34.3 Mbit/s, 0% loss |
| v4.5.0 | 7.2.5 | **93.7 Mbit/s (91.9–93.8)** | **84.3 Mbit/s (82.3–85.8)** | **41.5 Mbit/s, 57% loss** | **35.1 Mbit/s, 0% loss** |

Notes:
- ⁽¹⁾ 7.1.3 (2026-07-17) and 7.1.7 (2026-08-08): 3 RX reps and 10 TX reps of 30 s,
  measured before the standard bench existed; 7.1.3 had no UDP phase. Their RX
  medians sit lower than the 2026-08 rows partly because of that protocol.
- 7.1.9 is the 2026-08-21 qualification run of the shipped v4.2.0 image; 7.2.5 was
  measured on 2026-09-13.
- The two lines are never compared across rows: each row is one session.

## History

The detailed campaign record that used to follow these tables — per-driver-version
gate runs (v2.9 to v2.24), paired A/B experiments, the I-cache geometry
measurements, the on-chip SRAM work, the TX/RX per-packet CPU decompositions, the
levers explored and the in-driver instrumentation notes — was moved out of the
tree on 2026-09-14 to keep this file readable. It is unchanged in git history:

```sh
git show bb1c874:3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18/drivers/net/ethernet/rtl8196e-eth/PERFORMANCE.md
```

References elsewhere in this directory to a "v2.x entry", "Levers explored",
"In-driver instrumentation" or the "TX path per-packet decomposition" of
`PERFORMANCE.md` point at that archived version.
