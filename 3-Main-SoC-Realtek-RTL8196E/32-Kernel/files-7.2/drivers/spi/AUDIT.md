# spi-rtl819x — cumulative security and performance audit

> **Cumulative audit ledger.** The current-state table and §4 registry are
> authoritative. Sections describing v1.0/v1.1 are historical evidence for
> their audit pass, even when written in the present tense.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `spi-rtl819x` v1.2 |
| **Release target** | firmware v4.0.0 |
| **Documented tree** | Linux 7.1.3 |
| **Last audit pass** | 2026-07-30 — P1, 7.1 port/build revalidation after A2 |
| **Last fully audited baseline** | v1.2 |
| **Post-baseline changes** | none |
| **Validation state** | Linux 7.1.3 full image and targeted `W=1` builds passed; no 7.1 target flash yet; 6.18 target smoke is supporting hardware evidence only |
| **Maintained kernels** | Linux 6.18 and 7.1, identical v1.2 source |
| **Current finding registry** | §4 |
| **Scope** | driver, SPI-core contracts, Kconfig/Makefile, DT controller/NOR consumer and storage integrity |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-06-12 | unversioned → v1.1 | SPI-001…008 reviewed; S01…S04 implemented | build plus full 6.18/.88 storage gate |
| A2 | 2026-07-30 | v1.1 → v1.2 | SPI-006/009 fixed; effective speed reporting added; SPI-010 deferred | static/core review and build |
| V1 | 2026-07-30 | v1.2 on 6.18 | no code change; boot, stable MTD hashes and small JFFS2 readback confirmed | supporting 6.18 target smoke; not a 7.1 qualification |
| P1 | 2026-07-30 | v1.2 on 7.1 | v1.2 ported unchanged from Linux 6.18 | Linux 7.1.3 full image and targeted `W=1` object builds passed; target gate pending |

Pass A1 was the first standalone audit of this driver and was based solely on
its then-current code.
Provenance matters here: the driver is adapted from Weijie Gao's
out-of-tree RTL819x SPI driver (`MODULE_AUTHOR` preserved); the register
semantics (READY bit written alongside CS, DATA-register-triggered
transfers) are vendor lore validated empirically — every JFFS2 mount and
flash backup exercises them — rather than from a datasheet.

This is a **storage-path driver**: it carries the 16 MB GD25Q127C NOR
holding all four partitions. Any behavioural change must be validated with
a full boot + JFFS2 stress + `backup_gateway.sh` byte-compare, not just a
smoke test.

---

## 1. Security review

### 1.1 Attack surface

None direct or remote. The controller is reachable through the SPI core
from `jedec,spi-nor`/mtd; local access to raw MTD devices is governed by
device-node permissions. DT describes fixed partitions with `boot+cfg`
marked `read-only` — enforced by the mtd core, not here. Filesystem and
MTD requests influence addresses and transfer lengths, but buffers and
SPI messages are constructed by kernel callers. There is no spidev
consumer (`CONFIG_SPI_SPIDEV=n`), DMA, or interrupt handler. The review
is therefore chiefly about transfer correctness and storage integrity.

### 1.2 Verified correct

- **Bounded waits.** Every transfer polls `READY` through
  `readl_poll_timeout` (1 µs poll, 10 ms cap); a wedged controller yields
  `-ETIMEDOUT` up the mtd stack instead of a hang. No unbounded loop
  exists in the driver.
- **Big-endian data path.** `READ/WRITE_BYTE_ORDER` config bits are set
  under `CONFIG_CPU_BIG_ENDIAN`, with the partial-word shift helpers
  compensating for 1-byte accesses. Proven in production by every boot
  (squashfs rootfs and JFFS2 userdata both ride this path).
- **Alignment handling.** Both `rtk_read`/`rtk_write` drain leading
  unaligned bytes one at a time, then switch to 4-byte words, then mop up
  the tail — correct on a core with no `lwl`/`lwr` (the word loop even
  uses `get/put_unaligned` on top of the now-aligned pointer; pessimistic
  but safe, see §3 rejections).
- **Serialization.** No locking needed in `set_cs`/`transfer_one`: the SPI
  core's message queue guarantees a single in-flight transfer per
  controller, and `ioc_base` is only touched within that pipeline.
- **Probe unwind.** The one non-devm state transition
  (`clk_prepare_enable`) is undone when controller registration fails.
  Normal removal ordering was fixed in v1.2 (SPI-006).

### 1.3 Verdict

No remotely exploitable memory-safety flaw was found. The byte loops are
bounded by kernel-owned transfer lengths and all MMIO waits time out.
The audit identified one **medium storage-integrity lifecycle issue**
(SPI-006) reachable through administrative unbind/module removal, plus a
dormant active-high-CS contract bug (SPI-009). Both are fixed in v1.2,
pending the target storage gate.

---

## 2. Findings

### SPI-001 — CS deselect drives the *other* CS line low (low, dormant)

`realtek_spi_set_cs()` deselecting CS0 writes `RTK_SPI_CS_0_HIGH` only —
which clears the CS1 bit, actively driving CS1 low (= asserted) for as
long as the bus idles. Deselect should park the bus at
`RTK_SPI_CS_ALL_HIGH`, exactly as probe/remove/shutdown already do. (The
assert path proves the polarity convention: selecting CS0 writes
`CS_1_HIGH`, i.e. "the other line high, mine low".) Dormant on the Lidl
board — only the flash sits on CS0 and the CS1 pad isn't wired to anything
that listens — but a second device on CS1 would see itself selected
whenever the flash is deselected, and vice versa. Fix is one line per
branch (SPI-S01), **flash-soak gated** like everything in this file.

### SPI-002 — `mode_bits` advertises CPOL/CPHA the driver never programs (low, dormant)

`SPI_CPOL | SPI_CPHA` are accepted at setup, but no register write anywhere
depends on `spi->mode` — the controller runs whatever mode the hardware
implements (mode 0, per the flash's requirements). A mode-3 client would
be silently driven in mode 0. Truth in advertising: `mode_bits` should be
`SPI_CS_HIGH` only (SPI-S02).

### SPI-003 — `bits_per_word_mask` advertises 16/24/32 bpw (low, dormant)

`transfer_one` treats every buffer as a byte stream; word sizes above 8
would need per-word endianness handling the driver doesn't do. spi-nor
only uses 8 bpw, so nothing breaks today. Narrow the mask to
`SPI_BPW_MASK(8)` (SPI-S02).

### SPI-004 — sub-12.5 MHz requests are silently overclocked (info)

`rtk_choose_div_idx()` scans div 2→16 for the first rate ≤ `speed_hz`; if
none fits (request below parent/16 = 12.5 MHz at 200 MHz LX), it falls
back to div 16 — *above* the requested ceiling. A hypothetical slow device
would be overclocked without a warning. The flash runs at 25 MHz (div 8,
exact); add a `dev_warn_once` or clamp when touched.

### SPI-005 — French comments throughout (hygiene)

The file header bullets and most inline comments are in French, violating
the repo rule that committed files are English-only. Oldest offender in
the tree (predates the rule's enforcement). Translate at next functional
touch (SPI-S03) — not worth a standalone kernel-partition release.

### SPI-006 — remove disables hardware before devm stops the controller (medium)

`remove()` parks the hardware and disables the clock, but the devm
unregister of the controller runs *after* `remove()` returns. Linux 6.18 and
7.1 `devm_spi_register_controller()` install a devres action which later
calls `spi_unregister_controller()`; that unregister is what removes
children and stops/destroys the queued message pump. The driver's
`remove()` therefore writes `CS_ALL_HIGH` and gates the clock while an
already queued or in-flight flash transfer can still be using the
registers.

This is not safely reducible to “the transfer will time out”: forcing CS
high can terminate a command frame, and gating the clock can interrupt a
program/erase command sequence. An administrative driver unbind during
MTD activity can therefore return I/O errors **or corrupt the operation
being committed to flash**. Exposure is limited on the current image
(`CONFIG_SPI_RTL819X=y`, no normal unbind), but the consequence is storage
integrity on the sole boot/rootfs device.

Recommended fix: register one devm cleanup action immediately after
`clk_prepare_enable()` and before `devm_spi_register_controller()`. Devres
LIFO ordering will then run the SPI-core unregister first; the earlier
cleanup action can park CS and disable the clock only after the queue and
children are quiesced. The registration-failure path must use the same
action without double-disabling. `shutdown()` is distinct: device shutdown
orders children before the controller and may keep its explicit quiesce,
but it should be checked in the storage gate.

**Resolution (v1.2).** Implemented with `realtek_spi_quiesce()` registered
before the managed controller registration. Devres LIFO now unregisters
the controller first and runs the quiesce action second. The explicit
`remove()` callback was deleted; probe failure and shutdown share the same
idempotent cleanup body. Linux 6.18.38 and 7.1.3 full builds plus targeted
`W=1` object builds pass; the 7.1 storage gate remains required before
release.

### SPI-007 — no version identity (info)

Same as LED-004 for the LED driver: no `MODULE_VERSION`, no banner.
Add at next touch (SPI-S03).

### SPI-008 — dead full-duplex check, wrong errno (info)

`transfer_one` rejects `tx_buf && rx_buf` with `-EPERM`, but the core
already filters full-duplex transfers for `SPI_CONTROLLER_HALF_DUPLEX`
controllers at validation time (`-EINVAL`). The in-driver check is
unreachable defensive code with a misleading errno. Drop or fix the errno
when touched.

---

## 2b. Independent re-audit (2026-07-30, driver 1.1)

This pass deliberately audited `spi-rtl819x.c`, its Kconfig/Makefile
integration, DT consumer and Linux 6.18/7.1 SPI-core contracts before reading
this document. The 6.18 and 7.1 copies of the v1.1 driver were byte-identical.
It was a static pass: no kernel was built or flashed and the 2026-06-12
storage gate remains the latest on-target evidence.

| ID | Type | Severity | Status | Summary |
|---|---|---|---|---|
| SPI-006 | storage integrity / teardown | **medium** | fixed v1.2; target gate pending | SPI unregister now drains users before the shared quiesce action disables hardware |
| SPI-009 | API / electrical correctness | low, dormant | fixed v1.2; target gate pending | active-high CS is no longer advertised and the extra inversion was removed |
| SPI-010 | power management | info | open / platform-dormant | no suspend/resume callbacks restore CONFIG/CS if a future platform gates or loses the controller state |
| SPI-P01 | performance/API | info | partial v1.2 | effective speed is reported; CONFIG caching remains bench-gated |

### SPI-009 — `SPI_CS_HIGH` is double-inverted (low, dormant)

The controller advertised `SPI_CS_HIGH` in `mode_bits`. In Linux 6.18 and 7.1,
`spi_set_cs()` already accounts for the mode polarity (`enable = !enable`
for `SPI_CS_HIGH`) and calls the controller callback with `!enable`
(`drivers/spi/spi.c:1073-1122` in Linux 7.1.3).
`realtek_spi_set_cs()` then inverts its
argument a second time at line 143. An active-high device is consequently
driven with the active-low selection pattern.

The current NOR is active-low on CS0, so this branch is dormant. There are
two coherent fixes:

1. stop advertising `SPI_CS_HIGH`, which is the safe recommendation until
   an active-high device exists and the idle polarity of both physical
   lines has been characterized; or
2. implement the callback in terms of the physical level already supplied
   by the SPI core, while also tracking a safe inactive level for the
   *other* CS. Removing only the second inversion is insufficient for a
   mixed active-high/active-low two-device bus; test both polarities and
   both lines.

The first option has no electrical effect on the current flash and makes
the core reject an unverified topology.

**Resolution (v1.2).** The controller now advertises mode 0 with
active-low CS only (`mode_bits = 0`), and the redundant polarity inversion
was removed from `set_cs`. This leaves the current CS0 NOR behavior
unchanged while making active-high devices fail setup instead of being
driven with the wrong polarity.

### SPI-010 — no system-sleep state restoration (info)

The driver has no `dev_pm_ops`. It leaves the optional clock enabled and
assumes CONFIG/CONTROL survive for the controller's entire bound lifetime.
That is true for the current gateway, which has no supported suspend
workflow. A future platform that gates `busclk` or loses the SPI register
block during suspend would resume with stale divisor/byte-order/CS state
and no explicit clock re-enable. This is a compatibility gap, not a
current-board defect. Add suspend/resume only together with a real platform
power-state test; the root filesystem lives behind this controller.

### Performance revalidation

The implementation is a polled 4-byte shift register with no documented
FIFO, DMA or IRQ. At the board's 25 MHz clock the wire ceiling is
3.125 MB/s, and every four data bytes require one DATA MMIO plus at least
one READY read. No algorithmic runaway was found: buffer loops are linear,
each wait is capped at 10 ms, and SPI-core serialization protects
`ioc_base`.

Two small improvements are technically possible but are not justified
without the storage gate:

- cache the programmed CONFIG word/divisor and skip its MMIO write when
  consecutive transfers use the same speed (the spi-nor common case);
- set `xfer->effective_speed_hz` to
  `parent_rate / realtek_spi_clk_div_table[div_idx]`. This makes SPI-core
  clock-cycle delays and observability use the real clock rather than its
  conservative fallback.

Neither changes the dominant DATA/READY loop. `spi-mem`, DMA and
interrupt-driven redesigns remain unjustified without documented hardware
support and measured gain.

**v1.2 decision.** `effective_speed_hz` is implemented because it corrects
the SPI-core contract without changing the programmed clock. CONFIG-write
caching remains deferred: it changes MMIO behavior for negligible expected
gain and needs an on-target measurement.

### Revalidation corrections

- SPI-004 no longer silently overclocks a core-validated transfer:
  v1.1 publishes `master->min_speed_hz = parent/16`, and Linux 6.18/7.1 reject
  any lower `xfer->speed_hz` before `transfer_one`
  (`drivers/spi/spi.c:4310-4312` in Linux 7.1.3). The driver's
  `dev_warn_once` branch is
  therefore defensive/dead for normal SPI-core callers; the effective fix
  is the advertised minimum, not the warning.
- The former “lifecycle verified correct” verdict conflicted with
  SPI-006. Probe failure unwind was correct, but normal removal ordering
  required the v1.2 fix.
- `DESIGN.md` §3 described the pre-v1.1 addressed-only deselect behavior
  even though the code parks `CS_ALL_HIGH`; it is refreshed with the v1.2
  implementation.

---

## 3. Simplification / 6.18 and 7.1 alignment

API level is current: `devm_spi_alloc_host`, `devm_spi_register_controller`,
`readl_poll_timeout`, `spi_get_chipselect`, void `remove`. Candidates:

| ID | Change | Value |
|---|---|---|
| SPI-S01 | deselect → `CS_ALL_HIGH` in both `set_cs` branches | Closes SPI-001; flash-soak gated |
| SPI-S02 | `mode_bits = SPI_CS_HIGH`; `bits_per_word_mask = SPI_BPW_MASK(8)` | Closes SPI-002/-003; makes the core reject what the driver can't do |
| SPI-S03 | English comments, `MODULE_VERSION`, `IS_ALIGNED()` instead of `% 4` casts | Closes SPI-005/-007 |
| SPI-S04 | drop the dead full-duplex check (or `-EINVAL`) | Closes SPI-008 |
| SPI-S05 | order devm cleanup so SPI unregister precedes CS parking/clock disable | Closes SPI-006; **storage gate mandatory** |
| SPI-S06 | stop advertising `SPI_CS_HIGH`; alternatively implement and test per-line idle polarity without a second inversion | Closes SPI-009; active-high support needs both-CS electrical validation |

### Considered and rejected (this audit)

- **Direct word access in the aligned loops.** After the alignment
  prologue the pointer *is* word-aligned, so `get/put_unaligned` (4 byte
  loads + shifts on a no-`lwl`/`lwr` core) is pessimistic. But the CPU
  cost (~tens of ns/word) vanishes against the 25 MHz wire time
  (~1.3 µs/word); zero measurable gain on a storage path that would still
  need a full re-validation. Not worth it.
- **Conversion to `spi-mem` ops.** Would let spi-nor use `exec_op` and
  shave per-message overhead, but this controller is a plain shift
  register with no acceleration to expose; large churn on the most
  corruption-sensitive path in the system for unmeasured benefit.
- **Interrupt-driven transfers.** No evidence the block has a usable IRQ
  (none described in the DT, none in the vendor lore); the 1 µs poll on a
  single-purpose flash bus is the right shape for this platform.
- **`clock-frequency`/fallback cleanup.** The triple fallback (clk →
  DT property → hardcoded 200 MHz) looks redundant but each layer is
  reachable depending on DT generation; harmless, keep.

---

## 4. Finding ID registry

| ID | Severity | Status | Summary |
|---|---|---|---|
| SPI-001 | low (dormant) | fixed (v1.1) | CS deselect drives the other CS line low |
| SPI-002 | low (dormant) | fixed (v1.1) | CPOL/CPHA advertised, never programmed |
| SPI-003 | low (dormant) | fixed (v1.1) | 16/24/32 bpw advertised, byte-stream only |
| SPI-004 | info | fixed (v1.1) | sub-12.5 MHz fallback now dev_warn_once |
| SPI-005 | hygiene | fixed (v1.1) | comments translated to English |
| SPI-006 | **medium** | fixed in v1.2; storage gate pending | devm ordering now unregisters/drains before CS parking and clock disable |
| SPI-007 | info | fixed (v1.1) | MODULE_VERSION + probe banner added |
| SPI-008 | info | fixed (v1.1) | dead full-duplex check dropped |
| SPI-009 | low (dormant) | fixed in v1.2; storage gate pending | active-high no longer advertised; duplicate inversion removed |
| SPI-010 | info | open / platform-dormant | no suspend/resume clock/register restoration |
| SPI-P01 | info | partial v1.2 | `effective_speed_hz` reported; CONFIG caching remains bench-gated |
| SPI-S01..S04 | — | implemented (v1.1) | see §3 and the note in §5 |
| SPI-S05/S06 | — | implemented (v1.2); storage gate pending | ordered teardown and truthful active-low-only CS contract |

---

## 5. Conclusion

The normal transfer engine — the part that carries all 16 MB of persistent
storage — remains sound for the current mode-0, 8-bit, active-low CS0 NOR:
bounded polling, correct BE handling, correct alignment discipline and
SPI-core serialization. No remote or memory-safety issue was found.

The independent re-audit changed the lifecycle verdict: SPI-006 was a
**medium storage-integrity issue**, not an accepted informational wart.
Driver v1.2 implements its ordered devm cleanup and closes the dormant
SPI-009 active-high-CS contract bug. Both changes still require the full
boot + JFFS2 stress + backup byte-compare gate before release.

**Implementation note (2026-06-12):** S01–S04 implemented as driver
**v1.1** on maintainer request, plus the SPI-004 `dev_warn_once` on
divisor-range fallback. **Storage gate passed on the .88 gateway**:
boot from squashfs through the new driver; `boot+cfg` and `rootfs`
mtdblock md5s byte-identical to the pre-change baseline (before flash,
after flash, and again after stress); 4 MB of random data written to
JFFS2, page cache dropped, re-read from flash byte-identical. SPI-001's
deselect now parks `CS_ALL_HIGH`; the advertised-capability layer
was narrowed to mode 0 plus `SPI_CS_HIGH`, and to 8 bpw; all comments are
English; `MODULE_VERSION`/banner present. The later independent pass found
that the remaining `SPI_CS_HIGH` claim is still incorrect (SPI-009).

**Independent re-audit note (2026-07-30):** static source/core-contract
review only; no new target run. It found SPI-006 (medium, teardown/storage
integrity), SPI-009 (low/dormant active-high-CS polarity), SPI-010
(platform-dormant PM gap) and SPI-P01 (bench-gated micro-optimization/API
reporting). The June storage-gate result remains valid for normal runtime
I/O and does not exercise unbind or active-high CS.

**Implementation note (v1.2, 2026-07-30):** SPI-S05/S06 implemented,
along with `effective_speed_hz`: managed teardown now drains/unregisters
before hardware quiesce, and only the verified active-low CS contract is
advertised. SPI-010 stays deferred because this platform has no supported
suspend workflow; CONFIG caching stays bench-gated. No new on-target test
has yet fully qualified v1.2. Linux 6.18 target smoke passed, but does not
replace the complete storage gate. Complete Linux 6.18.38 and 7.1.3 image
builds plus targeted `W=1 drivers/spi/spi-rtl819x.o` builds pass; both linked
trees contain the v1.2 probe banner.

**Linux 7.1 port note (P1, 2026-07-30):** the v1.2 source was copied unchanged
from the audited 6.18 implementation after confirming the required 7.1 SPI
and devres APIs. Source and `DESIGN.md` are byte-identical across the two
lines. `kernel-img/lidl/kernel-7.1.img` was rebuilt successfully; it has not
been flashed, so boot, repeated MTD hashes, JFFS2 stress and backup
byte-compare remain the 7.1 release gate.
