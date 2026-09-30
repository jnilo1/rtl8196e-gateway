# spi-rtl819x — cumulative security and performance audit

> **Cumulative audit ledger.** The current-state table and the §4 registry are
> authoritative. Closed passes are summarized; their full text is in the git
> history of this file.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `spi-rtl819x` v1.2 |
| **Audited code** | driver 1.2 (`DRV_VERSION` in `spi-rtl819x.c`) |
| **Last audit pass** | 2026-07-30 — A2, independent static security/performance audit |
| **Last fully audited baseline** | v1.2 |
| **Post-baseline changes** | none |
| **Validation state** | full builds and `W=1` passed; target smoke passed (V1); the full storage gate was passed by v1.1 (A1); v1.2's storage gate was not run as a separate gate; the driver has shipped since v4.0.0 |
| **Maintained kernels** | Linux 6.18 and 7.2, identical source |
| **Current finding registry** | §4 |
| **Scope** | driver, SPI-core contracts, Kconfig/Makefile, DT controller/NOR consumer and storage integrity |
| **Companion** | `DESIGN.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-06-12 | unversioned → v1.1 | SPI-001…008 found; SPI-S01…S04 implemented | build plus full storage gate on the bench gateway: passed |
| A2 | 2026-07-30 | v1.1 → v1.2 | SPI-006/009 fixed (SPI-S05/S06); effective speed reporting added (SPI-P01, partial); SPI-010 deferred | static/core review and build |
| V1 | 2026-07-30 | v1.2 | no code change; boot, stable MTD hashes and small JFFS2 readback confirmed | target smoke |
| P1 | 2026-07-30 | v1.2 | v1.2 ported unchanged to the second kernel line (then 7.1) | full image and targeted `W=1` object builds |

The driver is adapted from Weijie Gao's out-of-tree RTL819x SPI driver
(`MODULE_AUTHOR` preserved). Its register semantics (READY written alongside
CS, DATA-register-triggered transfers) are vendor lore validated empirically —
every JFFS2 mount and flash backup exercises them — not datasheet facts.

This is a **storage-path driver**: it carries the 16 MB GD25Q127C NOR holding
all four partitions. A behavioural change is validated with a full boot +
JFFS2 stress + `backup_gateway.sh` byte-compare, not just a smoke test.

---

## 1. Security review

### 1.1 Attack surface

None direct or remote. The controller is reached through the SPI core from
`jedec,spi-nor`/mtd; raw MTD access is governed by device-node permissions.
The DT marks `boot+cfg` `read-only`, enforced by the mtd core. Filesystem and
MTD requests influence addresses and lengths, but buffers and SPI messages are
built by kernel callers. There is no spidev consumer (`CONFIG_SPI_SPIDEV=n`),
DMA or interrupt handler, so the review is chiefly about transfer correctness
and storage integrity.

### 1.2 Verified properties

- **Bounded waits.** Every transfer polls `READY` through
  `readl_poll_timeout` (1 µs poll, 10 ms cap); a wedged controller yields
  `-ETIMEDOUT` up the mtd stack instead of a hang.
- **Big-endian data path.** The `READ/WRITE_BYTE_ORDER` config bits are set
  under `CONFIG_CPU_BIG_ENDIAN`, with partial-word shift helpers for 1-byte
  accesses; every boot (squashfs rootfs, JFFS2 userdata) exercises it.
- **Alignment.** `rtk_read`/`rtk_write` drain leading unaligned bytes, then move
  4-byte words, then the tail — correct on a core without `lwl`/`lwr`.
- **Serialization.** The SPI core's message queue guarantees one in-flight
  transfer per controller; `ioc_base` is only touched inside that pipeline, so
  `set_cs`/`transfer_one` need no lock.
- **Lifecycle.** The only non-devm state transition (`clk_prepare_enable`) is
  undone by the quiesce devm action registered before the controller; see
  SPI-006 for the ordering.

**Verdict:** no remotely exploitable or memory-safety flaw. The byte loops are
bounded by kernel-owned lengths and all MMIO waits time out. A2 found one
medium storage-integrity lifecycle issue (SPI-006) and one dormant CS-polarity
contract bug (SPI-009), both fixed in v1.2.

---

## 2. Findings

### A1 findings (fixed in v1.1)

- **SPI-001 (low, dormant).** Deselecting CS0 wrote `RTK_SPI_CS_0_HIGH` only,
  actively driving CS1 low (asserted) while the bus idled. Harmless on the
  Lidl board (nothing listens on CS1), wrong for a second device. Fixed by
  SPI-S01: deselect parks `RTK_SPI_CS_ALL_HIGH`.
- **SPI-002 (low, dormant).** `SPI_CPOL | SPI_CPHA` were advertised though no
  register depends on `spi->mode`; the hardware runs mode 0. Fixed by SPI-S02.
- **SPI-003 (low, dormant).** 16/24/32 bpw were advertised though
  `transfer_one` is a byte stream. Fixed by SPI-S02
  (`bits_per_word_mask = SPI_BPW_MASK(8)`).
- **SPI-004 (info).** A request below parent/16 (12.5 MHz at 200 MHz) fell back
  to ÷16, above the requested ceiling, silently. v1.1 publishes
  `min_speed_hz = parent/16`, so the SPI core's `__spi_validate()` rejects any
  lower `xfer->speed_hz` before `transfer_one`; the driver's `dev_warn_once`
  branch is only a defensive fallback. The flash runs at 25 MHz (÷8, exact).
- **SPI-005 (hygiene).** French comments; translated (SPI-S03).
- **SPI-007 (info).** No version identity; `MODULE_VERSION` and probe banner
  added (SPI-S03).
- **SPI-008 (info).** An in-driver full-duplex check returned `-EPERM` although
  the core already rejects full duplex for `SPI_CONTROLLER_HALF_DUPLEX`
  controllers; dropped (SPI-S04).

A1 storage gate (2026-06-12, bench gateway, v1.1): passed — squashfs boot
through the new driver; `boot+cfg` and `rootfs` mtdblock md5s byte-identical
to the pre-change baseline before flash, after flash and after stress; 4 MB of
random data written to JFFS2, page cache dropped, re-read byte-identical. This
covers normal runtime I/O; it does not exercise unbind or active-high CS.

### A2 findings (2026-07-30, independent static re-audit of v1.1)

The pass audited the source, Kconfig/Makefile, DT consumer and SPI-core
contracts before reading this document; no kernel was built or flashed for it.

- **SPI-006 (medium, storage integrity; fixed v1.2).** v1.1 parked CS and
  disabled the clock in its remove callback, but the devm controller
  unregister — which removes children and stops the message pump — runs after
  that callback. An administrative unbind during MTD activity could therefore
  force CS high mid-frame or gate the clock mid program/erase: I/O errors or a
  corrupted flash operation. Exposure was limited (`CONFIG_SPI_RTL819X=y`, no
  normal unbind), but the device is the sole boot/rootfs store.
  **Resolution (SPI-S05):** `realtek_spi_quiesce()` is registered with
  `devm_add_action_or_reset()` immediately before
  `devm_spi_register_controller()`. Devres LIFO therefore unregisters the
  controller and drains its queue first, then the quiesce action parks CS and
  disables the clock. The driver no longer has a remove callback; the quiesce
  body is idempotent (`quiesced` flag); `shutdown()` calls it directly, which
  is safe because device shutdown orders children before the controller.
- **SPI-009 (low, dormant; fixed v1.2).** `SPI_CS_HIGH` was double-inverted:
  the SPI core's `spi_set_cs()` already applies the mode polarity before
  calling the controller callback, and `realtek_spi_set_cs()` inverted again,
  so an active-high device would be driven with the active-low pattern.
  **Resolution (SPI-S06):** only mode 0 with active-low CS is advertised
  (`mode_bits = 0`) and the extra inversion was removed; active-high devices now
  fail setup. Supporting them would require per-line idle-polarity tracking and
  electrical validation of both CS lines.
- **SPI-010 (info, open / platform-dormant).** No `dev_pm_ops`: CONFIG/CONTROL
  and the optional clock are assumed to survive the bound lifetime, which holds
  on this gateway (no supported suspend). Add suspend/resume only together with
  a real platform power-state test.
- **SPI-P01 (info, partial v1.2).** `xfer->effective_speed_hz` now reports
  `parent_rate / divisor`, correcting the SPI-core contract without changing
  the programmed clock. Caching the CONFIG write for consecutive same-speed
  transfers stays deferred: it changes MMIO behaviour for negligible expected
  gain and needs an on-target measurement.

Performance: the block is a polled 4-byte shift register with no documented
FIFO, DMA or IRQ; at 25 MHz the wire ceiling is 3.125 MB/s and every four data
bytes cost one DATA MMIO plus at least one READY read. Buffer loops are linear
and each wait is capped at 10 ms.

---

## 3. Simplification items and rejections

| ID | Change | Outcome |
|---|---|---|
| SPI-S01 | deselect → `CS_ALL_HIGH` in both `set_cs` branches | implemented v1.1 (SPI-001) |
| SPI-S02 | narrow `mode_bits` and `bits_per_word_mask = SPI_BPW_MASK(8)` | implemented v1.1 (SPI-002/003); v1.1 still advertised `SPI_CS_HIGH`, withdrawn by SPI-S06 |
| SPI-S03 | English comments, `MODULE_VERSION`, `IS_ALIGNED()` | implemented v1.1 (SPI-005/007) |
| SPI-S04 | drop the dead full-duplex check | implemented v1.1 (SPI-008) |
| SPI-S05 | devm-ordered quiesce after controller unregister | implemented v1.2 (SPI-006) |
| SPI-S06 | stop advertising `SPI_CS_HIGH`, remove the second inversion | implemented v1.2 (SPI-009) |

Considered and rejected:

- **Direct word access in the aligned loops.** `get/put_unaligned` after the
  alignment prologue is pessimistic, but tens of ns per word vanish against
  ~1.3 µs of 25 MHz wire time; not worth a storage re-validation.
- **`spi-mem` conversion.** Nothing to accelerate on a plain shift register;
  large churn on the most corruption-sensitive path for unmeasured benefit.
- **Interrupt-driven transfers.** No evidence of a usable IRQ; a 1 µs poll suits
  a single-purpose flash bus.
- **Clock fallback cleanup.** The clk → DT property → 200 MHz fallback chain is
  reachable depending on DT generation; harmless, kept.

---

## 4. Finding ID registry

| ID | Severity | Status | Summary |
|---|---|---|---|
| SPI-001 | low (dormant) | fixed (v1.1) | CS deselect drove the other CS line low |
| SPI-002 | low (dormant) | fixed (v1.1) | CPOL/CPHA advertised, never programmed |
| SPI-003 | low (dormant) | fixed (v1.1) | 16/24/32 bpw advertised, byte-stream only |
| SPI-004 | info | fixed (v1.1) | sub-12.5 MHz requests rejected by the core via `min_speed_hz`; `dev_warn_once` fallback |
| SPI-005 | hygiene | fixed (v1.1) | comments translated to English |
| SPI-006 | **medium** | fixed (v1.2) | devm ordering unregisters/drains before CS parking and clock disable |
| SPI-007 | info | fixed (v1.1) | `MODULE_VERSION` + probe banner added |
| SPI-008 | info | fixed (v1.1) | dead full-duplex check dropped |
| SPI-009 | low (dormant) | fixed (v1.2) | active-high CS no longer advertised; duplicate inversion removed |
| SPI-010 | info | open / platform-dormant | no suspend/resume clock/register restoration |
| SPI-P01 | info | partial (v1.2) | `effective_speed_hz` reported; CONFIG caching deferred pending measurement |
| SPI-S01…S04 | — | implemented (v1.1) | §3 |
| SPI-S05, SPI-S06 | — | implemented (v1.2) | §3 |

---

## 5. Conclusion

The transfer engine that carries all 16 MB of persistent storage is sound for
the current mode-0, 8-bit, active-low CS0 NOR: bounded polling, correct
big-endian handling, correct alignment discipline and SPI-core serialization.
No remote or memory-safety issue was found. v1.2 closed the SPI-006 teardown
ordering issue and the dormant SPI-009 polarity bug.

The A2 recommendation to rerun the full storage gate (boot + JFFS2 stress +
backup byte-compare) on v1.2 was not run as a separate gate; the driver has
shipped since v4.0.0. The latest recorded on-target evidence is the A1 storage
gate (v1.1) and the V1 smoke test (v1.2). Neither exercises unbind, the path
SPI-006 changed.
