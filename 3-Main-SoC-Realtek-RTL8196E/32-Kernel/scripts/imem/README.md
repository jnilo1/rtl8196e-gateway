# RTL8196E I-MEM optimization tools

These tools implement the bounded I-MEM procedure for a new kernel release.
Generated references, raw profiles, candidates and benchmark results live below
`imem-work/` and are deliberately ignored by git.

The procedure starts from an empty 16 KiB I-MEM window. It does not inherit a
previous release's function list. The previous production policy is rebuilt on
the new kernel only as the independent confirmation baseline.

## Safety invariants

- Every patch must apply without warning, fuzz or offset.
- Profiling uses an empty I-MEM window and the in-kernel 250 Hz PC sampler.
- Runtime-patched text (`__jump_table`, `__mcount_loc`, static calls and
  alternatives when present) is excluded from I-MEM.
- Moving a selected input section leaves an equal-size local hole in `.text`.
  The invariant checker compares the candidate against the exact empty-window
  production link map.
- Every reboot used for performance testing is followed by a `dmesg` gate and
  by stopping OTBR, netwatch, button and UART-bridge userland. A surviving
  process or an armed in-kernel UART bridge aborts the point.
- Raw selection output becomes a production policy only after structural
  checks and the standard 11-run release benchmark pass. A 12-round paired
  comparison is reserved for marginal candidates or causal measurement.

## Campaign outline

Build the empty profiling reference:

```sh
scripts/imem/build_profile_reference.sh 6.18
scripts/imem/build_profile_reference.sh 7.2
```

Capture two idle, two TX and two RX profiles with
`capture_profile.sh`, then decode and solve them with
`analyze_captures.sh`. By default (`IMEM_RX_WEIGHT=0`) the exact knapsack
objective is mean net TX samples, but both shipped policies were selected with
an RX weight (6.18.54: `0.1`, 7.2.8: `0.5`; see the policy headers). The
bootstrap byte-retention gate must pass before a candidate may be built.

`IMEM_RX_WEIGHT=<w>` adds RX coverage to that objective, with the RX samples
rescaled to the TX total, so `w` values one percent of RX coverage at `w`
percent of TX coverage; `0` (the default) is the TX-only selection.
`IMEM_MANIFEST=<path>` keeps one manifest per weight when several are compared.
A TX-only selection leaves the RX-hot code (`csum_partial`, the driver RX poll,
GRO) out of the window: on 7.2.8 it covered 69 % of TX samples but only 56 % of
RX samples and cost 3 Mbit/s of TCP RX. The deployed gateways receive far more
than they send, so RX is not traded away for TX.

### Choosing the RX weight

The weight is chosen by a fixed rule, not by scanning. Each benched weight costs
a build and a paired bench, the TX median moves by about 1 Mbit/s from bench to
bench, and the best of many noisy weights overstates its own gain.

1. Offline curve, no bench: `weight_curve.py <manifest> [--policy <incumbent.tsv>]`
   re-solves the exact knapsack over a grid of weights and prints the TX and RX
   coverage of each selection. The curve is a step then a slow tail: most of the
   RX coverage arrives with the first small weight (`csum_partial` enters).
2. Two candidates only: the tool suggests `0.1` and `0.25` when the step is
   already reached at `0.1`, otherwise the first weight that reaches it and the
   next one. Produce each manifest with `IMEM_RX_WEIGHT` and `IMEM_MANIFEST`.
3. Paired bench in one session, each candidate between two runs of the
   incumbent policy, criterion fixed before the run: TX median no more than
   1 Mbit/s below the incumbent's mean, and at least +1 Mbit/s on TX or RX.
   Otherwise the incumbent stays.
4. A marginal winner (a gain just above 1 Mbit/s) may be confirmed with
   `confirm_candidates.sh` before a release.

Coverage does not predict throughput, especially on 6.18: on 6.18.54, `w = 0.5`
matched the incumbent's TX coverage and still lost 2.3 Mbit/s of TCP TX against
it. Results so far:

| Line | Weight | TCP TX | TCP RX | Verdict |
|---|---:|---:|---:|---|
| 7.2.8 | 0 | 86.2 | 90.7 | RX regression |
| 7.2.8 | 0.25 | 86.3 | 93.6 | tie with 0.5 |
| 7.2.8 | 0.5 | 86.4 | 93.7 | shipped |
| 6.18.54 | incumbent (6.18.45 selection) | 82.8 | 92.6 | mean of three runs |
| 6.18.54 | 0.1 | 82.8 | 93.7 | shipped |
| 6.18.54 | 0.5 | 80.5 | 93.4 | TX regression |

Build the production-layout candidate from the resulting manifest:

```sh
scripts/imem/build_optimized_candidate.sh 6.18 \
  imem-work/6.18.54/profile-captures/selection-manifest.json
```

Run the standard release benchmark on the exact candidate. It uses 11 TX and
11 RX repetitions so each reported median is an observed run. A large-margin
candidate may take the bounded fast path (a manual rule, not enforced by any
script) when TX is at least 80 Mbit/s, RX at
least 90 Mbit/s, retransmissions and hard counters remain zero, and both
`dmesg` gates pass. The exact policy is then recorded under `policies/` and is
automatically applied by normal production builds of that kernel release.

For a marginal result, or when a precise causal estimate is wanted, compare the
candidate (`C`) with the previous production policy rebuilt on the same kernel
(`I`):

```sh
scripts/imem/confirm_candidates.sh \
  --candidate imem-work/6.18.54/production/candidate/kernel.img \
  --incumbent imem-work/6.18.54/production/incumbent/kernel.img \
  --output imem-work/6.18.54/confirmation-run1 \
  --expect 6.18.54- 192.168.1.88
```

This optional confirmation freezes six randomized order draws and their six exact
reverses before the first point. Each of the 24 points reboots and flashes the
gateway, flushes host TCP metrics, then records the median of three TX and three
RX measurements. Aggregate results remain sealed until all points are valid.

Confirmation requires a narrowly scoped host sudoers rule for:

```text
/bin/ip tcp_metrics flush all
```

The harness proves this permission before creating the output directory or
touching the gateway. It never falls back to a run without the flush.

`confirm_results.py` performs the one pre-registered paired analysis. A
candidate is confirmed only when the 95% TX interval excludes zero, mean TX is
at least +0.7 Mbit/s, and the RX lower bound remains above -0.5 Mbit/s.

## Structural equivalence

Structural equivalence is decided by the tools, not by visual review. The
local-hole invariant checker verifies selected section sizes, preserved holes,
code identity and linked I-MEM occupation against the exact reference map. The
dynamic-code scanner independently rejects any runtime patch site in the I-MEM
window. A failed check reopens the campaign; it cannot be waived by the
optimizer.

## Text placement (link-level pads)

Between two point releases the text of the SDRAM-resident hot path shears: a
few dozen upstream size changes ahead of `net/` in link order move every hot
function by a different amount, so their I-cache colours (address modulo
8 KiB on this 2-way, 512-set, 16-byte-line cache) all change at once. On
6.18.45 → 6.18.51 that alone cost 2.1 Mbit/s TX with identical hot code and an
identical I-MEM policy (see the 6.18.51 entry of the
[CHANGELOG](../../../CHANGELOG.md)).

A **text layout** compensates it without touching any function: never-executed
pad objects (`pad_*.S`, one `.space` in `.text.__text_pad_NNNN`, global symbol,
kept alive by `-u`) inserted in `obj-y` order ahead of chosen objects so that
each zone of hot functions lands either on the previous release's colours or
exactly on its own. Layouts live under `layouts/<KERNEL_VERSION>/`. The
mechanism is dormant for the pinned releases: only `layouts/6.18.51/` exists,
so 6.18.54 and 7.2.8 build unpadded. A layout holds:

- `pads.patch` — the pad objects and their Makefile insertions; applied by
  `build_kernel.sh` after `patches-<line>/` to **production builds only**
  (it follows the release I-MEM policy: profiling and empty-window builds have
  another layout and are never padded); roots derived from the patch;
- `tracked.tsv` — the hot functions whose colours the layout is about, with
  the intended reference (`6.18.45` restored, `6.18.51` kept);
- `layout.json` — recorded by `text_layout.py record` from the accepted build:
  toolchain strings, sha256 of the built `.config` and of `pads.patch`, every
  pad's address and size, every tracked function's address, a sha256 of the
  whole address-ordered text sequence (aliases grouped, pads excluded), and
  the proof that nothing references a pad.

Every production build then runs `text_layout.py verify` after the I-MEM
relink and **fails** on any drift of those facts — the guard is the build's,
not CI's: a drift means the benchmarked layout is not the one being shipped.
`TEXT_LAYOUT_DISABLE=1` builds the unpadded kernel (the comparison baseline);
`TEXT_LAYOUT_RECORD=1` skips the guard for the build that `record` will read.
A tree prepared with or without a layout must be rebuilt with `clean` to switch.

Proposing a layout for a new release is `propose_text_pads.py` — a proposal,
never an acceptance: it compares the two production `vmlinux`, reads the link
map for object ownership, classifies each zone by its TX/RX sample ratio
(default: restore the old colours when TX/RX ≥ 2, keep the new ones
otherwise — the 6.18.51 lesson is that recolouring `csum_partial`, GRO and
`eth_type_trans` buys TX with RX), and writes `proposal.json` + a candidate
`pads.patch`. Then: clean build with the patch under `patches-<line>/zz-*.patch`
and `TEXT_PAD_ROOTS`, `text_layout.py record` (which refuses references into
pads), colour check, and a paired bench against the unpadded build
(`bench_history_sweep.sh`, then `confirm_candidates.sh` before shipping).
The sweep flags a round where *every* image reads far below the band at once
(TX < `ENV_TX_FLOOR`, default 40, or RX < `ENV_RX_FLOOR`, default 60) as
`env-invalid`: a path fault, not a kernel difference. Its rows are tagged in
`sweep.tsv`, archived in `env-invalid.tsv`, excluded from the analysis, and
the round is replayed with the same order (at most `ENV_MAX_REPLAYS`, 2).
`confirm_candidates.sh` applies the same rule (shared `scripts/bench_env.sh`)
per round of candidate + incumbent: an invalid round is archived under
`env-invalid/` with its raw logs and dmesg, re-measured in the same order,
and only the 24 valid points reach `confirm_results.py`, which itself refuses
any point under the floors recorded in `protocol.txt`. The boundary cases
(one replay, replays exhausted, a single image under the floor) are covered
host-only by `test_confirm_env_invalid.sh`, which runs the harness's round
loop verbatim with a stubbed measurement.
Intra-object shears (upstream code changed between two hot functions of one
file) are reported and cannot be padded; leave them.

Known knobs: `--delta-rule weighted|first` (which delta an object with two
takes), `--group-by delta|object`, `--tx-rx-ratio`, `--min-share`.
