# RTL8196E I-MEM, D-MEM, and MIPS16e

This is the implementation reference for the on-chip SRAM facilities of the
RTL8196E (Lexra RLX4181).  It describes the code shipped by this tree for both
supported kernel lines, not a proposal for the former 5.10 port.

**Current production status (2026-08-10):** I-MEM is enabled.  The tree places
the historical IRQ/NAPI/Ethernet set, `csum_partial`, and the two shared
assembly copy cores there.  D-MEM integrity and its capacity/conflict advantage
have now been measured on hardware, but no real CPU-only candidate passed the
production-selection gate, so D-MEM remains dormant.  MIPS16e is advertised by
the platform but is not used for the current kernel hot paths.

The two copy-core patches are present in local `main` and the maintainer has
chosen to ship them on the complete evidence.  The formal pre-registered TX
utility gate nevertheless remains unresolved: the gain is established, but the
complete 95% interval does not clear 1%.  Keep the implementation decision and
the narrower statistical claim distinct.

The historical reference is the local Realtek SDK only:

```text
/home/jnilo/Téléchargements/rtl819x-SDK-v3.4.7.3-full-package/rtl819x
```

Do not look for or download that source from SourceForge.

## Hardware and addressing model

| Resource | Size | Physical range | Window alignment |
|---|---:|---|---:|
| I-MEM instruction SRAM | 16 KiB | `0x00c00000`–`0x00c03fff` | 16 KiB |
| D-MEM data SRAM | 8 KiB | `0x00c04000`–`0x00c05fff` | 8 KiB |
| I-cache | 16 KiB, 2-way, 512 sets | CPU-local | 16-byte lines |
| D-cache | 8 KiB, 2-way, 256 sets | CPU-local | 16-byte lines |

The physical addresses above are hardware facts from the historical RTL8196E
platform header.  They are *not* where the modern linker puts `.iram` or
`.dram`.  The linker places those sections in normal kernel SDRAM at aligned
virtual addresses.  Early boot programs Lexra COP3 windows to make fetches or
loads within those SDRAM ranges resolve through the respective on-chip SRAM.

The I-cache geometry in the table is now measured rather than inferred from
the public data sheet.  A KSEG0 generated-code probe found a sequential-walk
plateau from a 16-byte stride upward and exactly half the cost at 8 bytes.  A
matched-control conflict sweep, repeated over four base offsets and in a
permuted visit order, found two 8 KiB ways: 16 KiB total, 512 sets and 16-byte
lines.  The approximately 200 ns/block plateau is an effective penalty in that
synthetic dependent chain, **not** an isolated hardware miss latency.

The D-cache geometry was measured independently by the 2026 D-MEM campaign.
A matched conflict sweep found a stable third-line knee at 4 KiB and 8 KiB
strides, and a fifth-line knee when a 2 KiB stride alternated between two set
groups.  The result repeated at base offsets 0, 256 and 1,024 bytes and is
consistent with 256 two-way sets and 16-byte lines.  The measured
conflict-minus-control penalty was approximately 182--195 ns/load after the
4/8 KiB knee.  That synthetic penalty is not a bare SDRAM latency.

CP0 cannot supply either cache geometry on this core.  An on-demand guarded
probe read `Config0 = 0x00000000`; `Config0.M` was clear, so the MIPS32
`Config1` select-form instruction was deliberately not executed.  The precise
conclusion is that Config1 is not advertised and no cache geometry is available
through CP0, not that physical absence of the register was proved.  The raw
Config0 value was transcribed rather than captured, an evidence weakness that
is retained in the session report.

Consequences:

- Do not add a linker region at `0x80c00000` or directly dereference the
  KSEG0 aliases of the physical SRAM ranges.
- I-MEM is a 16 KiB window, not a general allocator.  Every byte placed in
  `.iram` consumes the same fixed hardware budget.
- The code runs at its ordinary linked virtual address; COP3 changes the
  backing store selected for that address range.
- Filling more of `.iram` does not enlarge the programmed hardware window: it
  is always 16 KiB.  The important side effect is link-layout displacement in
  `.text` and address-bearing metadata, which must be measured separately from
  the SRAM placement itself.
- No measurement in this campaign proves the inherited claims that an I-MEM
  fetch is exactly one cycle or that the window "bypasses the I-cache
  entirely".  Treat those as hardware-model descriptions pending a dedicated
  latency experiment, not as established explanations for the throughput gain.

## Current implementation

Both 6.18 and 7.1 have the same design and configuration:

```text
CONFIG_RTL8196E_IMEM=y
CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT=y
# CONFIG_RTL8196E_IMEM_POC_IRAM is not set
```

`plat_mem_setup()` calls `_imem_dmem_init()` before ordinary platform setup.
The implementation is in the overlay at:

```text
files-<line>/arch/mips/realtek/imem.S
files-<line>/arch/mips/realtek/setup.c
files-<line>/arch/mips/include/asm/mach-realtek/imem.h
```

The linker changes are in `patches-<line>/arch-mips-kernel-vmlinux.lds.S.patch`.
They collect `.iram-gen`, `.iram-fwd`, and `.iram` into a 16 KiB-aligned
`.iram` section and stop the link if it exceeds `0x4000` bytes.  `.dram-*` and
`.dram` are collected similarly, but there is deliberately no production
placement macro for them.

### Measured image footprint

These figures are from the locally built `vmlinux` files, not estimates.

| Build | `.iram` range | Occupied | Free in 16 KiB window | `.dram` |
|---|---|---:|---:|---|
| Historical 6.18 before `csum_partial` | `0x8032c000`–`0x8032e1fc` | 8,700 bytes | 7,684 bytes | empty |
| Shipping baseline A, with `csum_partial` | `0x8032c000`–`0x8032e7a0` | 10,144 bytes | 6,240 bytes | empty |
| Local-main 6.18 production form | `0x8032c000`–`0x8032edc0` | **11,712 bytes** | **4,672 bytes** | empty |

The local 6.18 `vmlinux` values above were read from the linked section and
boundary symbols.  The same production patches exist for 7.1, but section
addresses must be recorded from a fresh 7.1 build rather than copied from the
6.18 image.

The linker reserves and aligns the whole 16 KiB window.  Treat the free area as
headroom, not as a target: cold code gains nothing merely by fitting, and every
move can reshape `.text`.  There is no runtime capacity competition among
resident I-MEM functions while the hard 16 KiB bound is respected.

### What is in I-MEM

The current image places the following groups in `.iram`:

| Group | Main functions |
|---|---|
| MIPS IRQ entry/dispatch | `plat_irq_dispatch`, `do_IRQ`, `generic_handle_irq`, `handle_irq_event*`, `handle_level_irq`, `realtek_soc_irq_handler` |
| Lexra cache maintenance | `rlx_flush_dcache_fast`, `rlx_flush_dcache_range`, `rlx_dma_cache_wback_inv`, `rlx_dma_cache_inv` |
| Ethernet driver | `rtl8196e_isr`, `rtl8196e_poll`, `rtl8196e_start_xmit`, TX submit/reclaim/kick helpers, RX poll and TX free-count |
| NAPI/GRO glue | `__napi_schedule*`, `napi_complete_done`, `gro_receive_skb` |
| Internet checksum | `csum_partial` |
| Shared assembly copy core | one body exported as `memcpy`, `__raw_copy_from_user` and `__raw_copy_to_user` |
| Shared checksum-copy core | one body exported through the `__csum_partial_copy_*` aliases |

The driver placements use `__iram` from `mach-realtek/imem.h`.  A small number
of upstream-core functions use a local `__iram_hotpath` macro in the relevant
patch, because those files cannot include the platform-private header.  Both
expand to `__attribute__((section(".iram")))` only when both I-MEM Kconfig
switches are enabled.

Aliases at one address are one body and one budget item.  Dynamic profiles and
size reports must aggregate by routine address rather than treating alias names
as distinct functions.

## What the 2026 placement campaign established

### Static layout is not a miss model

The first conflict analysis counted complete function extents by cache set.
That model was useful for exposing possible collisions, but it treated every
line of large functions such as `tcp_ack` and `tcp_write_xmit` as equally hot.
It cannot bound dynamic misses or throughput.  Mutually exclusive paths,
execution frequency and temporal order matter.

The measured geometry therefore did **not** imply the former 5.5% maximum-gain
claim.  That number compared two unweighted static excess-line counts.  It was
retracted once the distinction between distinct linked lines and executed
instruction fetches was made explicit.

### Dynamic selection reversed the ranking

Linux already provided the required selector.  A profiling build used
`CONFIG_PROFILING=y` and `profile=4`, giving one PC histogram bin per 16-byte
I-cache line.  Three 180-second profiles were taken for idle, TCP RX and TCP
TX, with the idle histogram subtracted line by line.

Two analysis corrections were material:

- assembly aliases have no useful independent `nm -S` sizes, so symbol ranges
  were bounded by the next address and samples were aggregated by routine
  address;
- `realtek_wait` is the idle handler, not RX work, so its samples were removed
  before reporting work percentages.

The shared `memcpy`/raw-user-copy body (700 bytes) accounted for 41.5% of RX
work and 20.4% of TX work.  The shared checksum-copy body (856 bytes) accounted
for 15.1% of TX.  By contrast, `tcp_ack` touched 128 of its 316 linked lines but
accounted for only 1.7% of TX.  Whole-function placement of the large TCP
functions was rejected; a future hot/cold split remains a separate possibility.

For each cache set, summing samples beyond the two hottest resident lines gave
a weighted pressure score of 18.2% of RX work and 24.8% of TX work.  These are
selection scores, **not miss counts**: tick profiling measures time spent, slow
instructions can be overrepresented, and a timer IRQ cannot sample code while
IRQs are masked.

The profiler also cannot validate the historical I-MEM residents.  In the
profiling image `_etext` ended before the separately linked `.iram` region, and
the IRQ-heavy residents are exactly the code most under-sampled by the tick.
The campaign selected the best visible `.text` candidates; it did not prove
that the pre-existing 10 KiB was the globally optimal use of I-MEM.

### Layout sensitivity and measurement noise

The wider campaign established that merely linking unused code can change
throughput by moving the kernel.  WireGuard's unexecuted link slot and matched
padding reproduced one such loss, but those early builds moved code and data
together.  Later compensated controls preserved active-data addresses and
showed a narrower instruction-side effect.  Accordingly, "link layout matters"
does not by itself identify I-cache conflicts, D-cache conflicts or SDRAM
burst/alignment behaviour.

Noise was measured by reflashing one saved image eight times and, separately,
by repeating it without reboot.  The observed standard deviations were
0.62--0.70 Mbit/s for TX and 0.47--0.50 for RX.  There was no evidence in that
dataset that flash/reboot increased dispersion, although the two series were
sequential in time.  Their observed ranges are descriptive and must never be
used as detection thresholds: paired means and their uncertainty intervals can
resolve effects smaller than an individual-series range.

Future series must flush the host's cached TCP metrics before each point.  The
entry was constant during the profiling campaign, so it did not explain those
results, but leaving it uncontrolled would permit history from an earlier TCP
run to enter a later one.

### Controlled I-MEM results

The copy candidates were tested in stages, always against the same saved
baseline image and with balanced, pre-registered eight-pair sequences:

| Experiment | TX paired mean and 95% CI | RX paired mean and 95% CI | Interpretation |
|---|---:|---:|---|
| shared copy core only, original `.text` slot padded | +0.638 `[+0.14, +1.14]` | +0.387 `[-0.27, +1.05]` | positive TX effect; 1% utility unresolved |
| both shared copy cores, original slots padded | +1.188 `[+0.74, +1.63]` | +0.400 `[-0.19, +0.99]` | compensated result clears the 0.70 Mbit/s utility threshold |
| both cores, production layout without padding | +1.387 `[+0.63, +2.15]` | +1.875 `[+1.54, +2.21]` | production TX gain established; pre-registered utility gate still inconclusive |

The production TX estimate is about +2.0% and seven of eight differences were
positive.  Its lower confidence bound is nevertheless 0.07 Mbit/s below the
pre-registered 0.70 Mbit/s threshold.  Under that rule it is not yet a final
SHIP result.  Local main carries the change by maintainer decision based on the
positive compensated result, the positive production result, the strong RX
result and the completed safety tests.  Formally clearing the TX utility gate
would require a new pre-registered series, not adding points after inspecting
these eight.

The large production-layout RX gain is real within that A/B, but subtracting
the separate compensated-series estimate does not identify an exact layout
contribution.  The series ran at different times, and the padded variant also
carried a linker anchor and different address-bearing metadata.  The defensible
statement is that the production layout is beneficial on RX in the measured
series, not that a precise amount has been causally assigned to `.text`
reshuffling.

None of these tests separates lower SRAM fetch latency from avoided I-cache
conflicts.  They establish an instruction-side sensitivity because the
compensated tests held active-data addresses and other function addresses
fixed.  The mechanism may contain either or both effects.

`csum_partial` has a different evidence status from the two copy cores.  Its
four-pair compensated control gave RX +0.70 Mbit/s (four of four positive) and
TX +0.12, but four pairs did not meet the declared confirmation rule.  Earlier
unbalanced pairs confounded placement with order and link reshuffling.  The
function remains in local main and in baseline A; its dedicated eight-pair
confirmation is still an open historical result, not a prerequisite for the
copy-core experiments.

### Exception-table requirement

The user-copy assembly carries `__ex_table` entries.  Moving its faulting
instructions and fixups into `.iram` required adding `.iram` to modpost's
allowed text sections; this is functional support, not build-system cosmetics.

The production image was checked as follows:

- 477 decoded exception-table entries;
- 105 with instruction and fixup both in `.iram`;
- 372 with instruction and fixup both in `.text`;
- zero entries straddling the two sections;
- invalid read, file write, pipe write and `sendto` user pointers all returned
  `EFAULT` on hardware without an oops; a normal copy still succeeded.

Any future assembly move carrying exception fixups must repeat both the static
table audit and an on-device fault test.  A `/dev/null` write is not such a
test: that driver need not touch the supplied user buffer.

## Boot-time sequence

`_imem_dmem_init()` is deliberately MIPS32 assembly (`.set nomips16`).  It
performs the following operations, derived from the historical Realtek BSP and
corrected for the current one-bank RTL8196E implementation.

1. Enable COP3 access by setting CU3 in COP0 Status (`$12`).
2. Invalidate I-MEM with the required CCTL 0-to-1 transition on bit 5.
3. Invalidate I-cache and D-cache with CCTL bits 9 and 1.
4. If `.iram` is non-empty, mask its SDRAM address with `0x0fffc000`, program
   COP3 `$0`/`$1` to a 16 KiB instruction window, then pulse CCTL bit 4
   (IRAM Fill).  Hardware copies that SDRAM range into I-MEM.
5. If `.dram` is non-empty, initialise D-MEM with the separate copy sequence
   described below.
6. Clear CCTL.  The COP3 window registers remain programmed; normal cache
   operation resumes.

COP3 registers used on RTL8196E:

| Register | Purpose |
|---|---|
| `$0`, `$1` | I-MEM window base and inclusive top |
| `$4`, `$5` | D-MEM window base and inclusive top |
| `$2`, `$3`, `$6`, `$7` | second-bank registers; unused on this SoC |

Relevant CCTL values are `0x20` (I-MEM invalidate/off), `0x202` (I- and
D-cache invalidate), `0x10` (I-MEM fill), `0x400` (D-MEM on), and `0x800`
(D-MEM off).  They are pulse-style operations: preserve the explicit writes
to zero before each operation.

## D-MEM: validated, dormant, and not selected for production

There are no `__dram` or `__dram_poc` annotations in either production overlay.
Therefore `__dram_start == __dram_end`, the D-MEM block branches to
`skip_dram`, and no data is redirected through the 8 KiB window.

### Recovered historical Ethernet experiment

The dangling PoC was recovered as commit
`555896fdcfb4e065d5d502852db00a2d8f74fe39` and preserved on the private branch
`archive/iram-dmem-poc-20260501`.  Its exact `rtl8196e_ring_hot` object contains
seven CPU-only integers, or 28 bytes.  DMA descriptor rings, pools and packet
buffers were not moved.

Against the post-refactor control, its five-repetition D-MEM phase measured
TCP RX 93.0 versus 93.1, TCP TX 65.7 versus 65.3, UDP TX 33.3 versus 34.7 and
UDP storm 1.65 versus 1.70 Mbit/s.  It was therefore neutral on TCP and
negative on both UDP workloads.  The sequential-image method was not a
balanced same-image A/B and supplied no paired uncertainty interval, so it
rejects that 28-byte placement rather than D-MEM as a hardware facility.

### Runtime COP3 requirement and integrity result

The first on-demand D1 read trapped at `mfc3` with `ExcCode 0b`: CU3 had been
enabled by early boot, but was not set in the later process context which ran
the proc handler.  A runtime D-MEM user must therefore not assume that boot's
CU3 setting remains available in later contexts.  The corrected probe disables
preemption, masks local IRQs around each control sequence, enables CU3 locally,
and restores the exact former Status and COP3 Data Window registers before
returning.

With that correction, three complete repetitions passed aligned 8-, 16- and
32-bit reads, writes and read-modify-write operations, fixed and generated
patterns, both window edges, KSEG0/KSEG1 aliases, SDRAM backing checks and
window disarm.  Source and temporary-shadow SDRAM checksums remained unchanged,
and a subsequent reboot was clean.  An intermediate 8/16-bit failure was a
big-endian expectation bug in the probe, not a hardware failure.

This establishes the CPU-visible semantics of the preserved four-stage copy
sequence over a disposable arena.  It does **not** establish that switch DMA
can see D-MEM or that D-MEM writes are coherent with a peripheral.

### Copy protocol and linker invariant

D-MEM cannot be enabled by simply adding a section attribute.  Unlike I-MEM,
it has no hardware fill operation.  The retained assembly implements the BSP
copy protocol:

1. Point the 8 KiB Data Window at `__dram + 8 KiB` and enable D-MEM.
2. Copy the 8 KiB source range backwards from SDRAM
   `[__dram, __dram + 8 KiB)` to the temporarily redirected destination
   `[__dram + 8 KiB, __dram + 16 KiB)`.
3. Disable D-MEM, move the window back to `__dram`, then enable it again.

After that, the complete 8 KiB virtual window at `__dram` is backed by D-MEM.
This means a future experiment must account for **every** object in that
window, not only the annotated object.  It must also preserve a valid SDRAM
source range immediately after `.dram` during the temporary copy.  Any D-MEM
work therefore requires an isolated benchmark, a clean boot test, and explicit
approval before flashing.

The current linker layout is not sufficient for an isolated small candidate:
if `.data` follows a short `.dram` input, the remainder of the fixed 8 KiB Data
Window silently covers those following objects too.  A future D-MEM mode must
reserve and pad the complete aligned 8 KiB window, assert that candidate input
does not exceed `0x2000`, and prove that `_sdata` and every other live section
start outside it.

### Measured latency and capacity effect

D2-R2 used twelve balanced repetitions, five access patterns, working sets
from 16 bytes through 16 KiB, several base offsets and an independent conflict
sweep.  The table shows the full 8 KiB D-MEM working set.  In the fourth
column, a positive difference means that D-MEM completed the same aggregate
access loop faster than hot KSEG0:

| Pattern | Hot KSEG0 | D-MEM | Paired hot minus D-MEM | 95% interval |
|---|---:|---:|---:|---:|
| Dependent chase | 20.751 ns/access | 17.089 ns/access | **+2.442 ns** | +1.832…+3.662 ns |
| Sequential read | 24.414 ns/access | 22.582 ns/access | +2.442 ns | 0…+3.052 ns |
| Sequential write | 21.972 ns/access | 22.582 ns/access | 0 ns | 0…0 ns |
| Read-modify-write | 32.959 ns/access | 30.517 ns/access | **+2.442 ns** | +1.832…+3.662 ns |
| Pseudo-random read | 40.283 ns/access | 38.452 ns/access | **+2.441 ns** | +1.221…+3.052 ns |

At 4 KiB, every paired interval included zero and every median advantage was
between zero and 0.610 ns/access.  D-MEM's demonstrated benefit is therefore a
full-capacity or conflict effect, not a general improvement over a small hot
KSEG0 object.  D-MEM was much faster than deliberately cold KSEG0 and uncached
KSEG1 for read-bearing patterns, which validated the controls but did not by
itself identify a useful kernel candidate.

The RTL819X clocksource runs at 25 kHz, or one 40 microsecond tick.  D2 divided
aggregated 32,768-access timings, making one tick approximately
1.221 ns/access.  The recurring 1.221/2.442 ns steps are consequently
quantised aggregate differences including common loop overhead, not claims of
sub-tick timer resolution or bare load latency.

### Production-candidate selection

The D3 and D3-R2 profilers did not activate D-MEM.  They measured active
16-byte data lines, access rates and actual D-cache set occupancy before any
linker placement was authorised.

| Candidate | Measured result | Decision |
|---|---|---|
| Historical Ethernet control block | 28 CPU-only bytes; neutral TCP and negative UDP in the recovered PoC | Reject that placement |
| `timer_bases` | 7,200 bytes allocated, but at most 864 bytes active in any one-second epoch and at most 1,760 bytes in a complete window | Reject: sparse working set |
| `timer_bases`, TCP TX ceiling | 5,217 estimated read-bearing accesses/s; 12.741 µs/s benefit ceiling, or 0.001274% CPU | Reject: 78.5 times below the preregistered 1 ms/s gate |
| Composite timer/softirq/IRQ/NAPI/network metadata | Across 25 fixed windows, maximum one-second footprint 880 bytes, maximum window union 1,632 bytes and zero epochs above two lines in a two-way set | Reject: no capacity or candidate self-conflict mechanism |

The composite covered timer data, CPU-owned Ethernet cursors and counters,
active `softnet_data`, two `net_hotdata` budget fields, `softirq_vec`, local
`irq_stat` and tasklet heads.  TCP TX had the largest median active footprint
at 744 bytes.  Even the UDP-64 ring cursor, with roughly 236,000 touch calls per
window, occupied only three 16-byte lines.  Heat without footprint or set
pressure is exactly the regime where D2 found no material D-MEM advantage.

Instrumented throughput in D3/D3-R2 labels the generated workload only; the
probe overhead is too large for those values to be compared with production
throughput.

### Campaign decision and reopening rule

The campaign decision is **STOP before D4**: no production `.dram` placement,
same-image placement A/B or boot-time D-MEM enablement is justified.  This is
not a claim that D-MEM is broken or universally useless.  It means that the
modern kernel workloads examined here contain no safe, fixed CPU-only object
which combines a near-8 KiB active/conflicting footprint with enough access
rate to repay the linker, per-CPU and quiescing complexity.

Reopen candidate selection only for newly measured CPU-owned data which either
approaches the 8 KiB capacity boundary or demonstrably exceeds the D-cache's
two ways in relevant sets, and whose conservative D2-based benefit reaches a
predeclared utility threshold.  DMA-visible data remains prohibited until a
separate safe DMA-visibility and coherence experiment succeeds.

The diagnostic options remain default-off measurement scaffolding:

- `CONFIG_RTL8196E_DMEM_PROBE` provides the guarded D1/D2 arena probes;
- `CONFIG_RTL8196E_DMEM_CANDIDATE_PROBE` profiles `timer_bases` for D3;
- `CONFIG_RTL8196E_DMEM_COMPOSITE_PROBE` profiles D3-R2 set occupancy.

They must all be disabled in production.  The brief, protocols, reports and
raw evidence are kept outside the release tree at:

```text
/home/jnilo/Documents/RTL8196E/20260809-campagne-dmem-rtl8196e
```

## MIPS16e: capability is not current placement

The RLX4181 exposes MIPS16e.  The current platform declares
`cpu_has_mips16 = 1`, its Kconfig selects `SYS_SUPPORTS_MIPS16`, and the
historical SDK used per-function `__MIPS16` / `__NOMIPS16` attributes.

That does **not** mean the current kernel hot path is compiled as MIPS16e:

- Neither supported overlay has a C function annotated with
  `__attribute__((mips16))`.
- The production build does not use global `-mips16`.
- The I-MEM initialiser explicitly uses `.set nomips16` because it performs
  MIPS32 COP0/COP3 control operations.

MIPS16e remains a possible size experiment, not an assumed performance
optimisation.  It changes ABI/code generation and call stubs; evaluate its
actual code-size and throughput impact in a narrowly scoped function before
combining it with I-MEM placement.  Do not apply it globally to the kernel.

## Relationship to the historical Realtek code

The SDK provides the hardware protocol and the design precedent, but it is not
copied verbatim into the modern port.

| Historical source | What it establishes | Modern equivalent / decision |
|---|---|---|
| `linux-2.6.30/arch/rlx/mm/imem-dmem.S` | COP3 bring-up, CCTL sequencing, window masks and bank sizes | `files-<line>/arch/mips/realtek/imem.S`, restricted to one 16 KiB I-MEM and one 8 KiB D-MEM bank |
| `linux-2.6.30/arch/rlx/kernel/vmlinux.lds.S` | Priority-ordered `.iram-*` / `.dram-*` aggregation | Modern linker accepts the core subset and adds a hard 16 KiB I-MEM assertion |
| `linux-2.6.30/include/net/rtl/rtl_types.h` | Conditional placement and per-function MIPS16 macros | `imem.h` provides a small, Kconfig-gated `__iram` interface; MIPS16 is not enabled for current C hot paths |
| `linux-2.6.30/drivers/net/rtl819x/{rtl_nic.c,rtl865xc_swNic.c}` | Ethernet/IRQ/forwarding are plausible candidates for instruction locality | The rewritten `rtl8196e-eth` driver, cache routines, IRQ/NAPI and GRO form the historical resident set; the 2026 PC profile did not rank code already outside `_text` |
| SDK `.dram-gen` / `.dram-fwd` composite placement | Timer, IRQ, forwarding and networking data were historically grouped as hot candidates | D3-R2 measured the analogous modern CPU-only composite; its active footprint never exceeded 880 bytes per epoch and produced no greater-than-two-way candidate conflict |
| `linux-3.10/arch/rlx/soc-rtl8196e/` | RTL8196E address definitions and the later `bspinit.h` D-MEM copy sequence | Address constants and the safe D-MEM sequence are retained; the 3.10 `cpu_imem_size = 0` / `cpu_dmem_size = 0` metadata must not be read as a hardware absence |

The old broad macro system contains sections for wireless, crypto, VoIP, L2/L3
forwarding and other SDK-specific subsystems.  This project does not carry
those drivers, so importing their annotations would add complexity without a
measured benefit.

## Change procedure and verification

1. Change the overlay or the corresponding core patch for **both** kernel
   lines when their APIs permit it; never edit `linux-*-rtl8196e/` directly.
2. Keep the I-MEM candidate leaf-like where possible.  Review its callees:
   placing a wrapper in I-MEM does not move its large callees or data accesses.
   Rank aliases by address, not by exported name, and do not treat a complete
   large function extent as dynamically hot without PC evidence.
3. Build both lines and inspect the section before any flash:

   ```bash
   ./build_kernel.sh
   KERNEL=7.1 ./build_kernel.sh

   readelf -SW linux-6.18-rtl8196e/vmlinux | rg '^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]+\.(iram|dram)'
   nm -n linux-6.18-rtl8196e/vmlinux | rg '__iram$|__iram_tail$|__dram_start$|__dram_end$'
   ```

   The difference `__iram_tail - __iram` must stay at or below `0x4000`.
   Local main currently measures 11,712 bytes on 6.18.  An empty `.dram` is
   expected for the production configuration.

4. Check that the desired symbols are between `__iram` and `__iram_tail` with
   `nm -n`.  Do not trust source attributes alone: inlining, LTO settings, and
   section changes can alter the linked result.
5. Before benchmarking a placement change, separate its mechanisms where
   practical:

   - preserve the old `.text` extent with compensating padding for the causal
     SRAM test;
   - compare symbol addresses and section sizes, including `.data`, `.bss` and
     `.rodata`;
   - inventory address-bearing metadata rather than calling linked images
     byte-identical merely because object instruction sequences match;
   - build each variant once, save it and verify its hash at every flash;
   - use balanced randomised boot pairs, paired differences and a
     pre-registered uncertainty interval with no optional stopping;
   - repeat a positive result without padding in the form that would ship.

6. Benchmark with the Ethernet suite; never interpret a single run:

   ```bash
   ./scripts/test_rtl8196e_eth.sh "IRAM change: <description>"
   ```

   Compare TCP RX/TX, retransmits, CPU load, and UDP where relevant against
   `rtl8196e-eth/PERFORMANCE.md`.  I-MEM is not a remedy for DMA/cache
   coherency mistakes; audit those paths independently.

7. For timing work, remember that functions in the custom `.iram` section do
   not receive ftrace `mcount` instrumentation.  The PC histogram used by this
   campaign also covered `_stext` through `_etext`, while `.iram` was linked
   after `_etext`.  Move a function out of I-MEM temporarily or add focused
   counters rather than assuming either profiler observes current residents.
   Tick profiling additionally under-samples IRQ-disabled code.

8. For assembly user-copy changes, decode `__ex_table` and prove that each
   faulting instruction and fixup remain in the same executable section.  Keep
   `.iram` in modpost's allowed text-section list and exercise real invalid
   user pointers on hardware before shipping.

## Non-goals and guardrails

- Do not use D-MEM for descriptor rings, packet buffers or any other
  DMA-shared state without new visibility and coherence evidence.  CPU-only
  ring cursors are not forbidden on DMA grounds, but the recovered 28-byte
  placement and the modern composite profile both reject them on benefit.
- Do not add production early-boot D-MEM activation without a newly selected
  candidate and a separately preregistered same-image placement A/B.  The
  completed on-demand arena probe validates semantics, not a live placement.
- Do not place a short `.dram` section immediately before live `.data`; the
  hardware redirects the complete 8 KiB window.
- Do not enable global `-mips16`.
- Do not add upstream networking code to I-MEM merely because it is on the
  packet path; section budget, dynamic line use and a controlled throughput
  effect must be measured.  An earlier 4,280-byte network-path extension
  measured no gain.
- Do not remove the `.iram` size assertion or the empty-section checks in
  early boot.  An overlapping IW/DW configuration can make the board fail to
  boot.
- Do not flash a test image until the user explicitly requests it.  A build
  and a symbol inspection are safe local checks; flashing is a separate
  operation.

## Primary source locations

| Path | Role |
|---|---|
| `files-<line>/arch/mips/realtek/imem.S` | Current COP3 initialisation and dormant D-MEM sequence |
| `files-<line>/arch/mips/realtek/{dmem_probe.c,dmem_bench.c}` | Default-off D1 integrity and D2 latency/geometry probes |
| `files-<line>/arch/mips/realtek/{dmem_candidate.c,dmem_composite.c}` | Default-off D3 and D3-R2 active-data profilers |
| `files-<line>/arch/mips/include/asm/mach-realtek/imem.h` | Current placement controls |
| `patches-<line>/arch-mips-kernel-vmlinux.lds.S.patch` | `.iram`/`.dram` linker layout and I-MEM size assertion |
| `files-<line>/drivers/net/ethernet/rtl8196e-eth/` | Driver I-MEM candidates and performance baseline |
| `files-<line>/drivers/net/ethernet/rtl8196e-eth/PERFORMANCE.md` | Per-release throughput tables; the noise characterisation, compensated controls, cache geometry and copy-core A/B results are in its archived history (see its "History" section) |
| `POST-MORTEM-driver-perf.md` | Measurement and ftrace limitations |
| `/home/jnilo/Documents/RTL8196E/20260809-campagne-dmem-rtl8196e/` | D0--D3-R2 protocols, reports and raw evidence |
| `<SDK>/linux-2.6.30/arch/rlx/mm/imem-dmem.S` | Historical COP3 sequence |
| `<SDK>/linux-3.10/arch/rlx/soc-rtl8196e/{bspchip.h,bspinit.h,vmlinux.lds.S}` | RTL8196E-specific constants, D-MEM copy sequence and linker layout |
