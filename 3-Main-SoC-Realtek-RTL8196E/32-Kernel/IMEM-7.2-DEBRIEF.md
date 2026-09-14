# Debrief -- Linux 7.2 I-MEM selection investigation

Date: 2026-08-22

## Outcome

The RTL8196E port of Linux 7.2 is sound. The first native 7.2 I-MEM
selection was not: it optimised TX-only PC-sampling activity and removed
important RX/NAPI/GRO code from I-MEM. The resulting RX regression was an
optimizer failure, not an upstream Linux 7.2 regression.

The projected Linux 7.1.9 policy on Linux 7.2 produced the following standard
release-bench results:

| Workload | Result |
|---|---:|
| TCP RX | 93.0 Mbit/s |
| TCP TX | 84.4 Mbit/s |
| UDP RX | 43.3 Mbit/s, 55% loss |
| UDP TX | 35.8 Mbit/s, 0% loss |

This is the best 7.2 result of the session. It should be treated as an
experimental policy until versioned and integrated through the revised
selection procedure.

## Port and build validation

Linux 7.2 was imported in branch `kernel/7.2-imem`. The official tarball was
verified, all 58 RTL8196E patches replayed without fuzz, offset or warning,
and the full build completed successfully. Runtime code-patch scanning found
215 `__jump_table` sites and no site in I-MEM. Other scanned tables were absent
from this image (`__mcount_loc`, static calls, alternatives).

The runtime release was:

```
7.2.0-rtl8196e-v4.2.0-imem-optimized
```

The standard bench stopped `S70otbr`, `S80netwatch`, and `S40button` before
each run and restored them afterwards. A static repository iperf3 binary was
installed in `/userdata/usr/bin/iperf3` because this userdata image did not
contain it. All three 7.2 comparisons used that same binary and path.

## Native 7.2 profiling and selection

An empty-I-MEM profiling image captured two TX and two RX PC-sampling runs.
The native solver selected 86 sections occupying 15,868 of 15,872 bytes. Its
200-replicate bootstrap had 97.3% median byte retention.

That stability result was real but insufficient: the solver objective was
exactly `mean net TX samples`. RX samples were collected but did not influence
selection. The bootstrap therefore showed that the TX-only choice was
reproducible, not that it would preserve RX throughput.

Compared with the 7.1.9 selection, only 7,800 bytes in 51 sections remained
common. 8,068 bytes (about 51% of I-MEM) were replaced. The native 7.2 set
omitted RX-critical functions including:

- `rtl8196e_ring_rx_poll` (1,608 bytes)
- `dev_gro_receive` (1,592 bytes)
- `tcp4_gro_receive` (984 bytes)
- `skb_copy_and_csum_bits` (728 bytes)
- `eth_type_trans` (408 bytes)

It instead selected more generic IPv4/TCP code, including `ip_rcv_finish_core`,
`ip_rcv_core.constprop.0`, `tcp_add_backlog`, and `fib_validate_source`.

## Controlled benchmark results

All values are Mbit/s. TCP uses 11 repetitions; UDP uses three. No TCP errors
or retransmissions occurred in the 7.2 tests.

| Image | TCP RX | TCP TX | UDP RX | UDP TX |
|---|---:|---:|---:|---:|
| v4.2.0, Linux 6.18.45 | 91.7 | 82.8 | 40.7 | 36.7 |
| Linux 7.2, I-MEM empty | 80.7 | 65.3 | 32.3 | 29.7 |
| Linux 7.2, native TX-only selection | 86.9 | 82.5 | 40.3 | 34.8 |
| Linux 7.1.9, native selection | 92.8 | 82.8 | 43.2 | 34.3 |
| Linux 7.2, projected 7.1.9 selection | 93.0 | 84.4 | 43.3 | 35.8 |

The empty-I-MEM control establishes that I-MEM itself works strongly on Linux
7.2: the native policy recovered +6.2 Mbit/s RX and +17.2 Mbit/s TX against an
empty 7.2 window. The projected 7.1.9 policy then improved another +6.1 RX and
+1.9 TX over the native 7.2 selection.

The earlier conclusion that Linux 7.2 regressed RX was therefore incorrect.
It compared `7.2 + native selection` against `6.18 + v4.2 policy` without a
known-good I-MEM policy projected onto 7.2. The projected-policy control closes
that gap.

## 7.1.9 projection onto 7.2

The 7.1.9 policy could not be reproduced byte-for-byte because eight selected
sections totalling 208 bytes no longer exist in 7.2. The test projected the 79
remaining sections by exact `(object, section)` identity. It used 15,676 bytes
after linker alignment and deliberately left the remaining 196 bytes unused;
no post-result backfill was invented.

The image passed local-hole invariants and the runtime patch-site gate. This is
therefore a valid controlled ablation, while still being explicitly an
experimental policy rather than an automatically promotable release policy.

## Required changes to the I-MEM programme

### 1. Replace the TX-only objective

The solver must become bi-objective: maximise TX subject to an explicit RX
coverage constraint, then use RX as the first tie-breaker. A selection must not
be able to exchange important NAPI/GRO code for TX-only sampled activity.

The manifest should record separate TX and RX values per section and summed
values for every pre-registered candidate.

### 2. Make the prior policy a mandatory incumbent

For every new kernel line, construct three candidates before any throughput
measurement:

- `E`: empty I-MEM;
- `P`: prior release policy projected by exact `(object, section)` identity;
- `N`: the new native bi-objective solver output.

Projection omissions, ambiguity, size changes and unused bytes must be
recorded. The procedure must never silently fill unused projected capacity
after seeing a benchmark.

### 3. Select between pre-registered policies

Run a bounded screening tournament on `P` and `N` (with `E` as the mechanism
control when needed). A candidate may replace `P` only if it has a
pre-registered TX advantage and no RX regression. No hybrid assembled from
screening results is permitted.

Suggested bounded sequence:

1. Two TX/RX screening rounds for each candidate.
2. Retain at most two candidates according to the pre-registered rule.
3. Run four additional rounds only when screening cannot distinguish them.
4. Run the full 11-TX/11-RX release bench only for the selected policy.

### 4. Preserve existing safety gates

Keep all existing checks unchanged:

- warning-free patch replay;
- equal-size local holes and structural equivalence;
- post-alignment hard I-MEM budget;
- runtime patch-site exclusion;
- dmesg gate;
- service/userland quiescence after every reboot;
- odd-count 11-run TCP medians;
- no promotion when the independent release bench reports a regression.

### 5. Make every policy visible in `uname`

The empty, native and projected images all used the suffix
`-imem-optimized`, which is not enough for an auditable campaign. Build tags
should identify the policy, for example:

- `-imem-empty`
- `-imem-native-biobjective`
- `-imem-prev-7.1.9`

## Recommended decision

Do not retain the native 7.2 TX-only selection. Retain the projected 7.1.9
selection as the temporary 7.2 candidate while the selection programme is
revised. It restores RX to the expected range and produces the best measured
TCP TX of the session, but it requires normal policy integration and an
independent confirmation before release use.
