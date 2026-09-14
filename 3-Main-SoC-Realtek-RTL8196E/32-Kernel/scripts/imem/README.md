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
`analyze_captures.sh`. The exact knapsack objective is mean net TX samples; the
bootstrap byte-retention gate must pass before a candidate may be built.

Build the production-layout candidate from the resulting manifest:

```sh
scripts/imem/build_optimized_candidate.sh 6.18 \
  imem-work/6.18.45/profile-captures/selection-manifest.json
```

Run the standard release benchmark on the exact candidate. It uses 11 TX and
11 RX repetitions so each reported median is an observed run. A large-margin
candidate may take the bounded fast path when TX is at least 80 Mbit/s, RX at
least 90 Mbit/s, retransmissions and hard counters remain zero, and both
`dmesg` gates pass. The exact policy is then recorded under `policies/` and is
automatically applied by normal production builds of that kernel release.

For a marginal result, or when a precise causal estimate is wanted, compare the
candidate (`C`) with the previous production policy rebuilt on the same kernel
(`I`):

```sh
scripts/imem/confirm_candidates.sh \
  --candidate imem-work/6.18.45/production/candidate/kernel.img \
  --incumbent imem-work/6.18.45/production/incumbent/kernel.img \
  --output imem-work/6.18.45/confirmation-run1 \
  --expect 6.18.45- 192.168.1.88
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
identical I-MEM policy (see the 13/09/2026 memo in the maintainer's archive).

A **text layout** compensates it without touching any function: never-executed
pad objects (`pad_*.S`, one `.space` in `.text.__text_pad_NNNN`, global symbol,
kept alive by `-u`) inserted in `obj-y` order ahead of chosen objects so that
each zone of hot functions lands either on the previous release's colours or
exactly on its own. Layouts live under `layouts/<KERNEL_VERSION>/`:

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
