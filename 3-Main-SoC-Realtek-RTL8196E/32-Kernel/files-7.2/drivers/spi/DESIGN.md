# spi-rtl819x — design notes

| | |
|---|---|
| **Last updated** | 2026-07-30 |
| **Driver version** | 1.2 |
| **Release target** | firmware v4.0.0 |
| **Maintained kernels** | Linux 6.18 and 7.1, identical source |

Architecture companion to [`AUDIT.md`](AUDIT.md). This driver is the sole
path to the 16 MB GD25Q127C SPI NOR — bootloader, kernel, squashfs rootfs
and JFFS2 userdata all live behind it. Treat it as storage-critical: the
bar for touching it is a full boot + JFFS2 stress + `backup_gateway.sh`
byte-compare, not a smoke test.

## 1. Provenance

Adapted from Weijie Gao's out-of-tree RTL819x SPI controller driver
(`MODULE_AUTHOR` preserved), modernized for 6.x during the port:
`devm_spi_alloc_host`, `readl_poll_timeout`-bounded waits, per-transfer
divisor selection, parked-CS defaults, and a remove/shutdown path that
quiesces hardware without unregister churn. The register semantics are
vendor lore (no public datasheet for this block): the READY bit is written
back alongside the CS bits, and DATA-register accesses trigger the
transfer clocks. Empirically validated by every boot since v3.0.

## 2. Hardware model

Three registers at `spi@1200` (LX bus, 200 MHz parent clock):

| Register | Offset | Contents |
|---|---|---|
| CONFIG | 0x00 | clock divisor index [31:29] (÷{2,4,…,16}), read/write byte-order swap bits [28:27] (set on BE), CS deselect time [26:22] (driver always writes max=31) |
| CONTROL/STATUS | 0x08 | CS0/CS1 level bits [31:30] (1 = line high = deselected), transfer length−1 [29:28] (1–4 bytes), READY [27] |
| DATA | 0x0c | read = clock in N bytes, write = clock out N bytes |

No IRQ, no FIFO beyond the 4-byte data register, no DMA: a pure polled
shift register. The flash runs at `spi-max-frequency = 25 MHz` → divisor 8
(exact at 200 MHz).

## 3. Transfer model

`transfer_one` (half-duplex, enforced by `SPI_CONTROLLER_HALF_DUPLEX`):

1. Pick the divisor for this transfer's `speed_hz` (first ÷ giving
   rate ≤ request; the core rejects requests below ÷16) and rewrite
   CONFIG. The driver reports that selected rate through
   `effective_speed_hz`.
2. TX or RX in three phases: leading bytes until the buffer is
   word-aligned (1-byte transfers), 4-byte words, trailing bytes. Each
   beat is `READY`-polled with a 10 ms `readl_poll_timeout` cap — write
   beats poll *after* pushing DATA, read beats poll *before* pulling DATA.
3. On big-endian (this platform) the CONFIG byte-order bits handle the
   word lanes; `realtek_spi_make/resolve_data` shift partial words into
   the right lanes for 1-byte beats.

`set_cs` keeps a shadow (`ioc_base`) of the CS-bits + READY pattern that
`rtk_set_txrx_size` re-writes with each length change — CS state and
length share the CONTROL/STATUS register, so the shadow is what keeps a
multi-beat transfer from glitching CS. Deselect parks both physical lines
high. Only active-low CS is advertised: supporting active-high safely
would require per-line idle-polarity tracking for a two-device bus.

## 4. Concurrency

None inside the driver, by design: the SPI core's message queue serializes
all transfers per controller, and `ioc_base` is only mutated inside that
pipeline (`set_cs` → `transfer_one`). No spinlock, no atomic, nothing to
get wrong on the UP Lexra. On detach, devres LIFO ordering unregisters the
controller and drains that queue before the earlier quiesce action parks CS
and disables the optional clock.

## 5. Consumers

```
spi-rtl819x ← spi core ← jedec,spi-nor (flash@0, 25 MHz)
                            └── mtd fixed-partitions:
                                boot+cfg (128 KB, read-only)
                                linux    (1.9 MB)
                                rootfs   (squashfs)
                                userdata (JFFS2)
```

The `read-only` flag on `boot+cfg` is enforced by the mtd core. Everything
the gateway is survives behind this one polled controller — which is also
why the flashing tools (`flash_remote.sh`, boothold+TFTP) deliberately
bypass Linux for partition writes where possible.

## 6. Invariants

1. **Storage gate.** Any change — even "obviously correct" ones — ships
   only with boot + JFFS2 stress + backup byte-compare validation.
2. **The BE byte-order bits and the shift helpers move together.** The
   CONFIG swap bits handle 4-byte beats, the helpers handle 1-byte beats;
   changing one without the other corrupts every odd-length transfer.
3. **`ioc_base` must contain the current CS pattern whenever a transfer is
   in flight** — `rtk_set_txrx_size` rewrites the whole CONTROL/STATUS
   register from it on every length switch.
4. **Polling stays bounded.** 10 ms per beat via `readl_poll_timeout`; no
   raw `while (!READY)` loops, ever — a wedged flash must surface as
   `-ETIMEDOUT`, not a hung kernel.
5. **Max CS deselect time (31) stays** unless a measured reason appears;
   it costs nothing at our transfer sizes and is part of the
   empirically-validated envelope.
6. **Teardown ordering is storage integrity.** The controller unregister
   action must run before CS is parked or the clock is disabled. Keep the
   quiesce devm action registered before `devm_spi_register_controller()`.
