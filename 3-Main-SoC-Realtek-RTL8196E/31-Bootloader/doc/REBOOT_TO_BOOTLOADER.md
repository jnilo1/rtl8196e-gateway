# Reboot-to-bootloader: enter `<RealTek>` prompt from Linux

## Overview

A single command from Linux SSH reboots the gateway into the `<RealTek>`
bootloader prompt, ready for TFTP firmware updates — no need to press
ESC on the serial console.

```sh
boothold && reboot
```

The `boothold` binary writes the HOLD magic word to the top of the
board's `boothold` reserved-memory page and exits.  The flag is
**one-shot**: the bootloader clears it before entering download mode, so
the next reboot boots Linux normally.

Since boothold v1.2 the page is **discovered at runtime from the live
device tree** (`/sys/firmware/devicetree/base/reserved-memory/boothold@*`,
fields addressed relative to the page top), so a board that relocates
the reservation in its DTS (different DRAM size, e.g. the 64 MiB Sengled
G4) moves the flag without rebuilding the tool.  The bootloader read
side, by contrast, is a **compile-time constant per board**
(`BOOTHOLD_RAM` in `boot/main.c`) — a board's DTS and its bootloader
build must agree on the page address.  All concrete addresses below are
the **Lidl board's instantiation** (page `0x01FFE000`, size `0x1000`,
top word `0x01FFEFFC`).

---

## How it works

The mechanism uses a **magic word in DRAM** that survives the
watchdog reset triggered by `reboot`.  No flash writes are involved.

1. Linux writes `0x484F4C44` ("HOLD") to physical address
   `0x01FFEFFC` via `/dev/mem`.
2. Linux triggers `reboot`, which causes a watchdog reset.
3. The CPU restarts at `BFC00000` (flash reset vector).  The btcode
   re-initialises the DDR controller, but DRAM cell contents survive
   because the DDR2 retention time (~64-256 ms) exceeds the re-init
   delay (~1-2 ms).
4. The stage-2 bootloader checks `0xA1FFEFFC` (KSEG1 uncached alias of
   physical `0x01FFEFFC`) for the magic word.
5. If it matches, the bootloader **clears it** and enters
   download mode (`goToDownMode()`).
6. If it doesn't match (normal boot, cold power-on), the bootloader
   proceeds to load and boot the kernel as usual.

A full power cycle (disconnect all cables) clears DRAM and restores
normal boot.

### Optional TFTP server-IP handoff (bootloader V2.7+)

`boothold <A.B.C.D>` writes an additional **IP record** into the same
reserved page, just below the HOLD magic:

| Phys / KSEG1 addr          | Contents                                |
|----------------------------|-----------------------------------------|
| `0x01FFEFFC` / `0xA1FFEFFC` | HOLD magic `0x484F4C44` ("HOLD")        |
| `0x01FFEFF8` / `0xA1FFEFF8` | IP marker magic `0x49505634` ("IPV4")   |
| `0x01FFEFF4` / `0xA1FFEFF4` | IPv4 packed as `(a<<24)|(b<<16)|(c<<8)|d` |

When the bootloader sees a valid HOLD, it also checks the IP marker; if
present it uses the packed IPv4 as its download-mode TFTP server address
(see `tftpd_entry()` / `g_tftp_server_ip`), then clears all three words
(one-shot).  The IP is honoured **only alongside a valid HOLD** — i.e. a
deliberate warm reboot from a running Linux.  On a cold boot the page
holds garbage, the marker will not match, and the bootloader keeps its
compiled default (`192.168.1.6`).  `flash_remote.sh` passes its `BOOT_IP`
this way so the gateway comes up on the expected address with no serial
`IPCONFIG`.  Words are written value-first, marker-last, so the marker is
never valid over a stale value.  Endianness: the SoC is big-endian and
`boothold` writes via `htonl()`, so the bootloader reads the words as
plain `unsigned long`.

### Boot flow with boot-hold

```
setClkInitConsole()
initHeap()
initInterrupt()
initFlash()
showBoardInfo()
                    ← check BOOTHOLD_RAM[0]
                       if match → clear, goToDownMode(), return
check_image()
doBooting()
```

---

## DRAM address selection

### Why 0x01FFEFFC (high DRAM, one page below the btcode stack)

The flag must satisfy three constraints:

1. The bootloader (btcode + stage-2) must not write to it during early
   boot — otherwise it gets clobbered before the HOLD check runs.
2. The Linux kernel must not write to it while running, nor during
   shutdown / reboot — otherwise the magic written by `boothold` is
   lost before the watchdog reset takes effect.
3. The bootloader's HOLD check (KSEG1 uncached read) must agree with
   whatever path Linux uses to write it — otherwise cache coherency
   bites.

The page `0x01FFE000–0x01FFEFFF` (4 KB at 32 MB − 8 KB) sits in a
narrow but reliable window:

- **Above** the kernel image, kernel BSS/data, and any low-DRAM
  scratch the kernel touches during early init.  The kernel is loaded
  at phys `0x00500000` and never grows past ~24 MB even with maximum
  rootfs/userdata cache pressure.
- **Below** the top page, which produced false HOLD detections on cold
  boots when it was tried (see "Design alternatives").  The loader itself
  never writes either page: stage-1 runs without a stack and stage-2's
  stack lives inside its own BSS at `0x8041xxxx`.
- **Reserved** in the device tree as `reserved-memory` with `no-map`,
  so the kernel page allocator skips it and there's no MMU mapping —
  no KSEG0/KSEG1 coherency conflict, no cache aliasing risk.

### Why not 0x003FFFFC (the v2.x location)?

v2.x firmware (Linux 5.10) put the flag at `0x003FFFFC`, near the
*bottom* of DRAM.  This was reliable on the 5.10 kernel.  Starting
with Linux 6.18 (v3.0.0+) it became unreliable:

| Stack | HOLD address | `boothold && reboot` reliability |
|---|---|---|
| v2.1.6 (Linux 5.10) | 0x003FFFFC | 30/30 = 100% |
| v3.0.0 (Linux 6.18) | 0x003FFFFC | 26/30 = 87% |
| v3.1.1 (Linux 6.18) | 0x003FFFFC | 22/30 = 73% |
| **v3.2.0 (Linux 6.18)** | **0x01FFEFFC** | **30/30 = 100%** |

On failed boots, the bootloader read either zero or kernel-code-like
values (e.g. `0xFFFFFFFC`, `0x00001000`) — symptoms of the kernel
scribbling low DRAM before the `reserved-memory no-map` declaration
takes effect.  The 5.10 → 6.18 jump (with its accompanying toolchain
refresh) changed the early-init memory access patterns; the bisection
in the v3.2.0 fix narrowed the regression to that kernel transition.

Moving the flag to high DRAM eliminates the conflict regardless of
which exact path the kernel takes through low memory during boot or
shutdown.

### Address safety map

| Region                                  | Address range                    | Status    |
|-----------------------------------------|----------------------------------|-----------|
| Exception vectors                       | `0x80000000 – 0x800001FF`       | Avoid     |
| DDR calibration (stage-1 DQS sweep)    | low DRAM scratch                | Avoid     |
| Stage-1.5 piggy decompressor + payload  | `0x80100000+`                   | Avoid     |
| LZMA workspace                          | `0x80300000`                    | Avoid     |
| Stage-2 loader (code, BSS, stack, heap) | `0x80400000 – _end`             | Avoid     |
| Kernel image (loaded by stage-2)        | `0x80560000 – ~0x806C0000`      | Avoid     |
| Watchdog crash record (kernel DTS)      | `0x81FFD000 – 0x81FFDFFF`       | Avoid     |
| **Boot-hold flag**                      | **`0x81FFEFFC`** (KSEG0)        | **Used**  |
| Top page (false HOLDs on cold boot)     | `0x81FFF000 – 0x81FFFFFF`       | Avoid     |

---

## Bootloader implementation

In `boot/main.c`, at file scope:

```c
#define BOOTHOLD_MAGIC  0x484F4C44  /* "HOLD" */
#define BOOTHOLD_RAM    ((volatile unsigned long *)0xA1FFEFFC)
```

In `start_kernel()`, after `showBoardInfo()`:

```c
if (BOOTHOLD_RAM[0] == BOOTHOLD_MAGIC) {
    BOOTHOLD_RAM[0] = 0;
    prom_printf("---Boot hold requested\n");
    goToDownMode();
    return;
}
```

The pointer uses KSEG1 (`0xA1FFEFFC`), not KSEG0 (`0x81FFEFFC`), so
both the read and the clear bypass the L1 D-cache and go directly to
DRAM.  Without this, the clear (write 0) could stay in the cache and
be lost on power cycle, producing a false boot-hold on every cold
boot.

### Upgrading from v3.1.1 or earlier

The HOLD address changed in v3.2.0 (bootloader V2.6).  Bootloader and
`boothold` binary must be upgraded together; a mismatch (old bootloader
+ new boothold, or vice versa) leaves `boothold && reboot`
non-functional.  Always upgrade via fullflash
(`./flash_install_rtl8196e.sh -y <gateway-IP>`) when crossing the
v3.1.1 → v3.2.0 boundary.  Per-partition upgrade across this boundary
is unsupported.

---

## Kernel device tree reservation

In `arch/mips/boot/dts/realtek/rtl8196e.dts`:

```dts
memory@0 {
    device_type = "memory";
    reg = <0x00000000 0x02000000>;  /* 32MB */
};

reserved-memory {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges;

    boothold@1ffe000 {
        reg = <0x01FFE000 0x1000>;  /* 4KB reserved for boothold flag */
        no-map;
    };
};
```

The `no-map` property removes this page from `memblock` before the page
allocator starts — no runtime cost, no exception handling.  On MIPS,
KSEG0/KSEG1 are hardware-mapped (no TLB), so `/dev/mem` access still
works.  The 4 KB cost (0.01% of 32 MB) is negligible.

The kernel boot log confirms the reservation:

```
OF: reserved mem: 0x01ffe000..0x01ffefff (4 KiB) nomap non-reusable boothold@1ffe000
```

---

## Linux-side usage

### With boothold binary (recommended)

```sh
boothold && reboot
```

The `boothold` binary (installed in `/usr/bin/`) uses `pwrite()` on
`/dev/mem` to write the HOLD magic to physical address `0x01FFEFFC`.

### Manual / one-liner alternative

```sh
devmem 0x01FFEFFC 32 0x484F4C44 && reboot
```

This works because the address is in a `reserved-memory no-map` page —
the kernel never caches or touches it from the page allocator side.

---

## Experimental results

Tested on the Lidl Silvercrest gateway (RTL8196E, 32 MB DDR2):

| Test | Result |
|------|--------|
| DRAM retention across watchdog reset | **Survives** — magic at `0x01FFEFFC` preserved |
| Boot-hold from Linux SSH on Linux 6.18 | **30/30 = 100%** at the new address |
| One-shot behavior (subsequent reboot) | **Works** — flag is cleared, Linux boots normally |
| Full power cycle (disconnect all cables) | **Flag cleared** — DRAM lost, normal boot |

---

## Design alternatives considered

### Top of DRAM (last page, `0x01FFF000–0x01FFFFFF`)

Rejected: it produced false HOLD detections on cold power-on.  The
writer was never identified — stage-1 has no stack and stage-2's stack is
inside its own BSS, so it is the kernel or the stock loader, not our
bootloader.  The page one below (the current location) was found clean by
experiment; do not move it without re-running that experiment.

### Low DRAM (`0x003FFFFC`, the v2.x location)

Rejected on v3.x: see the regression analysis above.  Unreliable on
Linux 6.18 due to the kernel scribbling low DRAM during early init or
shutdown, before the `no-map` reservation is enforced.

### Flash-based flag

Write a 4-byte magic to flash (e.g. last sector of mtd0), bootloader
reads and clears it via sector read-modify-write.  Guaranteed to
survive any DRAM corruption but causes one flash erase+write cycle per
use — needless wear when DRAM works reliably at the new address.

Not implemented.
