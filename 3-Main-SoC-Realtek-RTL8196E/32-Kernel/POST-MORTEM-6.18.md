# Post-mortem: Linux 5.10.252 → 6.18 port for RTL8196E (Lexra RLX4181)

**Date:** 2026-04-13
**Scope:** experimental port of the Linux kernel on the Lidl Silvercrest
Zigbee gateway (Realtek RTL8196E SoC, Lexra RLX4181 MIPS-I CPU,
big-endian, 32 MB RAM, no FPU, no ll/sc, non-coherent DMA).

**Result:** kernel boots to userland, all vendor drivers work
(irqchip, timer, gpio, LEDs, SPI flash, 8250 serial, rtl8196e-eth
NAPI driver), clean reproducible build, zero warnings.

---

## TL;DR

A port originally estimated at 2–3 weeks landed end-to-end in one
afternoon of actual porting work plus several hours spent chasing a
single silent-boot failure whose root cause turned out to be trivial:
I was patching the wrong file. Most of the "hard" porting (Lexra
atomics, TLB, cache init) was effectively a no-op because the
5.10 patch set carried over almost verbatim. The sting was in
accumulated driver API drift and one misdirection about which
`cpu-probe` source file the Makefile selects.

---

## Original plan vs. reality

The porting plan was built around
seven steps and warned loudly that **step 2 (Lexra CPU atomics)**
was the high-risk part and could be a hard wall.

| Step | Planned effort | Actual effort | Notes |
|---|---|---|---|
| 0 Scaffolding | 0.5d | ~15 min | trivial |
| 1 Diagnostic map | 2–3d | 30 min | 25/45 patches applied cleanly; the rest collapsed to three mechanical categories |
| 2 Lexra CPU port | 4–7d (critical) | ~30 min | arch/mips had barely moved between 5.10 and 6.18 |
| 3 Vendor drivers | 2–3d | ~1h | rename storm (SPI master→controller, timer API, etc.) but mechanical |
| 4 rtl8196e-eth | 1–2d | ~15 min | pre-existing driver already used stable 6.x APIs |
| 5 First boot | 2–4d | **several hours** | one silent-boot bug ate the entire day |
| Clean-up + patch regeneration | — | ~1h | script already existed, just needed a parameterisable rewrite |

The plan's fear (in its own words, translated: "the MIPS atomics
rework in 6.x prevents clean coexistence with a CPU without ll/sc")
turned out to be
unfounded. The actual wall was something the plan did not
anticipate at all.

---

## What went smoothly

### Mainline structural stability

Everything the plan flagged as risky — the atomics refactor, the
`pgtable` folio churn, the TLB generator — was untouched or
trivially rebaseable:

- **25 of 45 existing 5.10 patches applied cleanly** on vanilla 6.18
  (with fuzz/offset only).
- Of the 20 with failed hunks, **12 hunks** were a single mechanical
  substitution: `CONFIG_CPU_TX39XX` had been dropped from mainline,
  so `#if R3000 || TX39XX` had become `#if R3000`, and our original
  patches that added `|| RLX4181` no longer matched context. One sed
  turned `#if defined(CONFIG_CPU_R3000)$` into `#if defined(CONFIG_CPU_R3000) || defined(CONFIG_CPU_RLX4181)`
  across four files. Verified safe by checking that 5.10 vanilla had
  **zero** isolated `#if R3000` sites in those files — all pre-existing
  ones were `R3000 || TX39XX`, so the substitution could not pollute
  anything.
- Nine more failed hunks were pure line-offset drift (no semantic change).
- The `tlbex.c` patch was dropped entirely: in 6.18, `cpu_has_3kex`
  is defined as `!cpu_has_4kex`, and our `cpu-feature-overrides.h`
  already sets `cpu_has_4kex=0`, so the existing mainline `if
  (cpu_has_3kex)` branch automatically routes RLX4181 to
  `build_r3000_tlb_refill_handler()`. No custom switch needed.
- The `pgtable-32.h` patch was dropped: once `CPU_RLX4181` selects
  `CPU_R3K_TLB` in Kconfig, the existing `#if defined(CONFIG_CPU_R3K_TLB)`
  guard in mainline activates the correct swap-entry format for
  RLX4181 with no extra code.
- The `scripts/Makefile.lib` patch was dropped because mainline
  6.18 already uses the exact LZMA options our patch intended to
  set (`--lzma1=lc=1,lp=2,pb=2,dict=8MiB`).

### Driver overlay was clean

The vendor files under `files/drivers/` (gpio-rtl819x, timer-rtl819x,
irq-rtl819x, leds-gpio-pwm, 8250_rtl819x, spi-rtl819x, c-lexra) and
the modern `rtl8196e-eth` driver all ported with mechanical API
adjustments and no structural rewrites.

### Build machinery

The `extract_patches.sh` utility — originally hard-coded for
5.10.252 — was refactored into a parameterisable script with
proper safeguards (git-tracked directory protection, stdin-is-tty
check, explicit --force). One incident during refactor where env
vars were lost through a `yes y | ...` pipe and the script
overwrote the 5.10 production directories; recovered cleanly via
`git checkout`. That incident directly motivated the safeguard
work.

---

## What went wrong

### Day was consumed by one silent-boot failure

After roughly four hours of clean rebasing and API fixes, the
first test image booted the bootloader, ran zboot, decompressed
LZMA, printed "Now, booting the kernel...", and then went silent.
Debugging this silent failure took longer than the rest of the
port combined.

The root cause was obvious in retrospect:

```
Makefile: obj-y += cpu-probe.o            (used when CPU_R3K_TLB=n)
Makefile: obj-y += cpu-r3k-probe.o        (used when CPU_R3K_TLB=y)
```

The 5.10 carry-over patch set contained `arch-mips-kernel-cpu-probe.c.patch`
which added a `case PRID_IMP_LX4380` to `cpu_probe_legacy()` in
`cpu-probe.c`. That worked on 5.10. But when we added
`select CPU_R3K_TLB` to the RLX4181 Kconfig block (to pick up the
new mainline `CPU_R3K_TLB` plumbing that simplified pgtable-32.h),
the Makefile started selecting `cpu-r3k-probe.c` instead — a
151-line file that only knows R2000 and R3000.

On boot:

1. `cpu_probe()` reads PRID = `0xcd00` (LX4380).
2. `cpu-r3k-probe.c`'s switch has no case for it.
3. `__cpu_name[cpu]` stays NULL.
4. `BUG_ON(!__cpu_name[cpu])` fires.
5. The kernel panics via an exception.
6. The exception vector is still the boot exception vector (BEV=1
   at `0xbfc00200` in ROM) because `setup_c0_status_pri` does not
   touch BEV.
7. The ROM exception handler apparently jumps back to the beginning
   of low RAM.
8. The CPU re-executes the kernel from `_text` at `0x80000000`,
   which is exception fill (zeros, running as nops), reaches
   `__kernel_entry` at `0x80000400`, and `j kernel_entry` brings
   it back to `0x80323258`.
9. Loop, indefinitely.

The one-line fix was to add a mirror `case PRID_IMP_LX4380` block
to `cpu-r3k-probe.c`. Once that was in place, the kernel booted
straight to a login prompt on the first try.

### The false trails

Before reaching that one-line fix, I burned time on multiple wrong
theories, each of which felt plausible in isolation:

#### 1. Cache coherency

The FIXME at line 127 of `arch/mips/boot/compressed/decompress.c`
("should we flush cache here?") was so tempting I pursued it
twice. On a non-coherent Lexra with a writeback D-cache, it *was*
the kind of thing that ought to matter. I wrote a CCTL sequence
in inline asm to flush D-cache then invalidate I-cache between
decompression and the jump to `kernel_entry`. When the first
attempt did nothing I realised the sequence was backwards (the
Lexra CCTL op triggers on a 0→1 transition, not 1→0), fixed it,
rebuilt, reflashed — still silent.

The user's instinct — "wait, 5.10 works fine on the same hardware
with the same FIXME" — was right and saved more wasted time.
Cache coherency was never the problem; the same `_text` layout
and same bootloader handoff worked for 5.10.

**Lesson:** a FIXME in mainline that has stood for years and
that works on a near-identical system is almost certainly not
the bug I am chasing.

#### 2. Wrong byte order in my own instrumentation

When I added `DBG_PUTC` markers (`A..F`) in assembly at the start
of `kernel_entry` to pinpoint where the boot was silently dying,
the markers also produced nothing. For a long time this looked
like confirmation that the jump to `kernel_entry` was failing
altogether — which in turn supported the cache theory.

The real reason the markers did not print was much dumber:

- The 8250 UART on the RTL8196E has `reg-shift = 2` (registers
  at byte offsets `×4`): TX at `0xb8002000`, LSR at `0xb8002014`.
- The CPU is MIPS big-endian. An 8-bit register exposed at byte
  offset `0x14` is read by `lbu` at `0xb8002014` — the bootloader,
  and zboot's `uart-16550.c`, do exactly this and it works.
- My `DBG_PUTC` was using `lw`/`sw` (32-bit word access). On big
  endian, `lw` at `0xb8002014` gives LSR in the **high** byte of
  the loaded word (bits 24-31), not the low byte. Masking with
  `0x20` always returned 0, so the THRE-wait loop never exited
  and spun forever (there was no timeout in the first iteration).
  Adding a timeout still did not help, because `sw` of the byte
  to `0xb8002000` wrote `0x00 0x00 0x00 'X'` across addresses
  `0x00..0x03` and the hardware TX register only listens at
  `0x00`, so the transmitted byte was always `0x00` (NUL,
  invisible).
- A small `[Z]` probe in C using `volatile unsigned char *`
  generated `lbu`/`sb` and worked immediately. That was the
  smoking gun.

**Lesson:** on MIPS BE, never use word access for 8250 registers
that are conceptually 8-bit. Always use `lbu`/`sb`, matching
what the hardware expects. The word-access path happens to work
on little-endian MIPS and that is probably how I got used to it.

#### 3. Following the ELF entry bytes

Once the byte-order issue was fixed, `[Z]ABCDEF...` printed but
looped. That finally pointed at the real problem (exception loop
after `j start_kernel`). But even then I initially added markers
to `cpu-probe.c` and was confused when they did not produce
output. The reason — the file was not being compiled — should
have been an early check: a five-second look at `arch/mips/kernel/Makefile`
would have shown `ifdef CONFIG_CPU_R3K_TLB / obj-y += cpu-r3k-probe.o`.

**Lesson:** when instrumented code appears to "not run", the very
first check is: **is this file actually in the build?** `ar t
built-in.a` or `find -name cpu-probe.o` takes 1 second and rules
out entire categories of confusion.

---

## Concrete API-drift inventory (5.10 → 6.18)

For reference, these are the changes that actually landed in the
driver and overlay code, in rough order of how much thought each
required:

**Mechanical renames (sed-friendly):**

- `CONFIG_CPU_TX39XX` removed; replaced by `CONFIG_CPU_RLX4181`
  in 12 `#if` hunks.
- `from_timer()` → `timer_container_of()`.
- `del_timer_sync()` → `timer_delete_sync()`.
- `spi_master_*` → `spi_controller_*` (`get_devdata`, `alloc_host`,
  `register_controller`, `SPI_CONTROLLER_HALF_DUPLEX`, struct
  `spi_controller`).
- `<asm/unaligned.h>` → `<linux/unaligned.h>`.

**Signature changes:**

- `platform_driver.remove` returns `void` (since 6.11).
- `gpio_chip.set` returns `int` (was `void`).
- `set_termios` takes `const struct ktermios *old`.
- `netif_napi_add(ndev, napi, poll, weight)` lost its `weight`
  parameter in 6.1.
- `of_get_mac_address(np)` returning `u8 *` → `of_get_mac_address(np, u8 *)`
  returning `int`.
- `net_device.dev_addr` became `const unsigned char *`, so
  `ether_addr_copy(ndev->dev_addr, ...)` must become
  `eth_hw_addr_set(ndev, ...)`.

**Structural moves:**

- `napi_gro_receive()` moved from `net/core/dev.c` to
  `net/core/gro.c` and became an inline wrapper in
  `include/linux/netdevice.h` that calls `gro_receive_skb(&napi->gro, skb)`.
  Any `__iram_hotpath` annotation needs to live on
  `gro_receive_skb` now.
- `irq_domain_add_legacy()` replaced by
  `irq_domain_create_legacy(of_fwnode_handle(np), ...)`.
- `local_flush_data_cache_page` removed; only the non-local
  `flush_data_cache_page` remains.
- `fw_passed_dtb` removed from `arch/mips`; boards must fall back
  to `__dtb_start` or another mechanism.
- `struct spi_device.chip_select` is now an array; use
  `spi_get_chipselect(spi, 0)`.

**Warning fixes needed to hit 0-warning builds:**

- Missing prototypes for callback/weak functions (`prom_init`,
  `prom_free_prom_memory`, `realtek_machine_restart/_wait/_halt`,
  `plat_time_init`, `lexra_cache_init`, `rtl819x_clocksource_init`).
  Fixed by the appropriate `#include <asm/bootinfo.h>`,
  `#include <asm/time.h>`, and by making the platform callbacks
  `static`.
- `CONFIG_BASE_SMALL=1` in the inherited 5.10 config was invalid
  for 6.18 (BASE_SMALL is now `bool`, accepting only `y`/`n`);
  changed to `CONFIG_BASE_SMALL=y`.

**One category deliberately dropped:**

- The legacy `drivers/net/ethernet/rtl819x/` Realtek SDK driver
  (11.7 KLOC) was not ported. The modern `rtl8196e-eth` driver
  (2.6 KLOC) replaces it. Two of the original 5.10 patches
  (`net-core-skbuff.c.patch`, `net-ethernet-eth.c.patch`) only
  existed to support the legacy driver and were dropped from
  `patches-6.18/`.

---

## The actual 6.18-only additions

After removing debug scaffolding, the delta from the 5.10 patch
set to the 6.18 patch set is:

- **Patches removed (4):** `arch-mips-include-asm-pgtable-32.h`
  (obsoleted by automatic `CPU_R3K_TLB` selection),
  `arch-mips-mm-tlbex.c` (`cpu_has_3kex` auto-routes), `scripts-Makefile.lib`
  (upstream now uses the same lzma options), `arch-mips-kernel-cpu-probe.c`
  (the *wrong* file, 151-line `cpu-r3k-probe.c` is what gets
  compiled on this config).
- **Patches added (3):** `arch-mips-kernel-cpu-r3k-probe.c`
  (the actual fix — `case PRID_IMP_LX4380`),
  `net-core-gro.c` (`__iram_hotpath` now belongs on
  `gro_receive_skb`), `arch-mips-boot-compressed-uart-16550.c`
  (adds a `CONFIG_REALTEK` case for zboot UART — dead code
  unless `DEBUG_ZBOOT` is enabled, kept in tree so that future
  early-boot diagnostics are a config flip away).

---

## Lessons for the next port

1. **Check which file is actually compiled before debugging it.**
   A one-second `ar t built-in.a | grep cpu-probe` would have
   surfaced the wrong-file problem immediately.
2. **Instrument early, and make the instrumentation correct
   before the thing it instruments.** A broken `DBG_PUTC` cost
   several false conclusions. A single trusted probe is worth
   more than five suggestive ones.
3. **Trust "it works on the old version".** If 5.10 boots on
   the exact same hardware with the exact same bootloader, any
   theory that blames hardware behaviour (caches, MMIO layout,
   endian hazards the bootloader somehow lives with) is suspect
   by construction.
4. **Pessimistic plans are cheap insurance, not a schedule.**
   The plan estimated the Lexra atomics port as 4–7 days and
   warned of a possible hard wall. Reality was ~30 minutes. A
   plan that assumes things will be easy will be demoralising
   when they are not; a plan that assumes things will be hard
   wastes nothing if they turn out easy.
5. **Scripts that overwrite production data need safeguards
   you do not think you need.** One command with mispositioned
   env vars blew away `patches/` on the 5.10 production tree.
   Everything was recoverable via git, but the next scripting
   pass got `--force`, stdin-tty check, and git-tracked-path
   protection — bolted on *after* the scare, which is the
   normal order.
6. **MIPS big-endian MMIO: always byte-accessor.** `lbu`/`sb`
   at the register byte address always does the right thing
   regardless of CPU endianness. `lw`/`sw` requires you to
   think about where the 8 bits are inside the 32-bit loaded
   value, and different platforms disagree.

---

## The UART1 silent-RX regression (2026-04-15)

**Symptom:** on kernel 6.18, ttyS1 was functionally dead in one
direction. TX worked (bytes left the chip, we could see them on
the wire), but RX was silent — `/proc/tty/driver/serial` reported
`rx:0` regardless of load, and IRQ 29 (UART1) incremented only on
THRE (TX holding register empty) events, never on RDR. Result:
`serialgateway` would start, Z2M would connect, send an EZSP
ASH RST frame, get nothing back, time out, and retry forever.
5.10.252 with the same bootloader, same EFR32 firmware, same
serialgateway binary, same `config-5.10.252-realtek.txt`-derived
`rootfs` — zero issue. Network up, devices joined.

**Time cost:** about an hour of wild goose chasing before I
realised I had not committed my first instinct (*"it's my new
in-kernel uart-bridge driver leaking state across attach/detach
cycles"*) to empirical falsification. Ten minutes of actually
running the bisection — move the bridge driver out of the tree,
clean-rebuild, reflash — showed the same failure. **The bug was
in the committed 6.18 port, not in the WIP.**

**Diagnosis path, useful bits only:**

1. `dmesg` confirmed the 8250 core registered the port:
   `18002100.serial: ttyS1 at MMIO 0x18002100 (irq = 29,
   base_baud = 12500000) is a 16550A`
   — so the platform device was bound to *something* that
   called `serial8250_register_8250_port()`.
2. Live register dump through `devmem` (physical addresses, as
   always on this platform):

   ```
   ttyS1 @ 0x18002100, reg-shift 2, big-endian, byte in MSB
     IER = 0x0D → ERDI=1 ELSI=1 EDSSI=1  (RX interrupt enabled)
     LCR = 0x13 → 8N1
     MCR = 0x2B → DTR=1 RTS=1 AFE=1       (auto flow control on)
     LSR = 0x60 → DR=0  THRE=1 TEMT=1     (RX FIFO empty)
     MSR = 0x10 → CTS=1                   (EFR32 ready to receive)
   ```

   The controller was configured correctly. No line error bits,
   no framing, no break. The RX FIFO was simply never filling.

3. The real shift: read PIN_MUX_SEL at syscon offset 0x40 =
   physical `0x18000040`. It was `0x00000008`. It should have
   been `0x0000004A` (BIT(1) | BIT(3) | BIT(6)). BIT(3) alone —
   the power-on default — is enough to route TXD to the physical
   pin, so bytes leave the chip; but RXD needs BIT(1) and BIT(6)
   added. Without them the EFR32's serial output is wired to a
   function that the UART block does not see, so rx stays at 0
   while everything else looks healthy.

4. `devmem 0x18000040 32 0x4a` at runtime → `rx` counter jumps
   from 0 to 449 within seconds, Z2M negotiates EZSP v13, Network
   up, 2 devices joined. So the *fix* works; the question is why
   the probe wasn't setting it.

5. **Root cause, uncovered by `ls /sys/devices/platform/soc/18002100.serial/driver`:**

   ```
   /sys/devices/platform/soc/18002100.serial/driver
       -> ../../../../bus/platform/drivers/of_serial
   ```

   Our vendor driver `rtl8196e-uart` (`8250_rtl819x.c`) was
   registered, but `of_serial` (`drivers/tty/serial/8250/8250_of.c`)
   had won the match and bound to `18002100.serial`. Our probe
   — the only code in the tree that writes PIN_MUX_SEL via
   `syscon_regmap_lookup_by_phandle("realtek,syscon")` —
   *never ran*. Which also means all the earlier time spent
   instrumenting the syscon path and suspecting a regmap API
   drift in 6.18 was misdirected: the regmap call was not
   silently failing; it was never invoked.

**Why did `of_serial` win on 6.18 and not on 5.10?** The uart1
DT node had two compatibles:

```
compatible = "realtek,rtl8196e-uart", "ns16550a";
```

`8250_of.c` matches `"ns16550a"`. `8250_rtl819x.c` matches
`"realtek,rtl8196e-uart"`. Both are in-tree, both platform
drivers, both register against the same node. On 5.10 the
vendor driver got bound first. On 6.18 the of_platform matcher
selects `of_serial` before us — I didn't dig into exactly which
commit in the core changed the ordering because the fix sidesteps
the ambiguity entirely. **Never list a generic fallback compatible
on a DT node whose correct driver is a vendor one; some future
kernel will happily pick the generic match and your probe will
go silent.**

**Fix** (`ecaa671`, `fix(kernel/dts): force rtl8196e-uart driver
binding for UART1`): drop the `"ns16550a"` fallback from the
uart1 node in both `files/arch/mips/boot/dts/realtek/rtl819x.dtsi`
(the 5.10 overlay, removed since in v3.0.0) and `files-6.18/.../rtl819x.dtsi` (6.18). `rtl8196e-uart`
is the only candidate left; it wins the match deterministically
on both kernels, its probe runs, `syscon_regmap_lookup_by_phandle`
returns the real regmap, `regmap_update_bits()` sets the UART1
pin mux correctly, and RX comes up. No driver code change. The
`8250_rtl819x.c` file is identical to the pre-session committed
version.

**Post-fix end-to-end on 6.18:**

```
dmesg: rtl8196e-uart 18002100.serial: (probe ran)
       18002100.serial: ttyS1 at MMIO 0x18002100 is a 16550A
devmem 0x18000040 → 0x0000004A
/sys/devices/platform/soc/18002100.serial/driver
       → .../bus/platform/drivers/rtl8196e-uart
/proc/tty/driver/serial: 1: tx:1019 rx:682 RTS|CTS|DTR
z2m: EZSP started, [STACK STATUS] Network up,
     "Currently 2 devices are joined"
```

**Lessons worth remembering next time something is "silent":**

1. **Bisect the blame before staring at the source.** I burned an
   hour on my own in-tree WIP driver before testing the hypothesis
   with the WIP sources physically removed from the tree. The
   bisection takes ten minutes and would have pointed at 6.18 port
   code immediately.
2. **`/sys/devices/.../driver` is always the right first question**
   when a probe *seems* to run but your instrumentation doesn't
   fire. If the symlink points somewhere unexpected, you already
   know nothing in your vendor `.c` file ran — no register trace
   or printk debug can tell you otherwise.
3. **DT multi-compatible is a platform-matcher footgun.** Adding a
   `"ns16550a"` fallback *feels* defensive ("fall back to the
   generic 8250 core if my custom driver isn't built in"). It
   turns out that the core will then happily match a generic
   driver *ahead* of yours on some kernels and hide everything
   your vendor driver was meant to set up. Prefer a single exact
   compatible and let the driver internally delegate to the
   8250 core via `serial8250_register_8250_port()`, which is what
   we were already doing.
4. **"A probe printk doesn't fire" ≠ "the driver is loaded but
   broken."** It almost always means "something else grabbed the
   device." Verify that before trying to fix the inside of the
   function that never ran.
5. **`devmem` as a bringing-it-back-from-the-dead primitive is
   hard to beat.** A single write to the right physical address
   restored RX end-to-end in seconds, and proved the hardware path
   was fine before a single kernel rebuild. Remember to pass
   *physical* addresses (not KSEG1 virtual) — `devmem 0xB8000040`
   goes to a different mapping and returns zero.

---

## The UART baud-rate N+1 divisor quirk (2026-04-16)

### Symptom

With the in-kernel UART↔TCP bridge (`rtl8196e-uart-bridge`) armed at
460 800 baud and the EFR32 NCP firmware rebuilt at the same baud, the
link showed **~40 % framing errors** (`fe:28 / rx:70` after 30 s) and
Z2M was stuck in an ASH reset loop.  115 200 and 230 400 worked
perfectly with `fe=0`.  Overrun counter (`oe:`) stayed at zero at all
speeds, so the bridge hot-path was fast enough — this was not a
software-latency problem.

### Diagnostic path

1. **Ruled out firmware mismatch** — rebuilt NCP at 230 400, bridge at
   230 400 → clean.  Rebuilt NCP at 460 800 → FE returned.
2. **Computed expected baud error** with `clock-frequency = 200 MHz`,
   standard 8250 divisor math `quot = clk / (16 × baud)`:

   | Baud    | Quot (programmed) | Actual wire (std) | Error (std) |
   |---------|------------------:|------------------:|------------:|
   | 115 200 |               108 |          115 740  |     +0.47 % |
   | 230 400 |                54 |          231 481  |     +0.47 % |
   | 460 800 |                27 |          462 963  |     +0.47 % |

   All ≤ 0.5 % — well within UART tolerance.  So standard math predicts
   460 800 should work.  It didn't.

3. **Found the clue in the bootloader.**
   `31-Bootloader/boot/uart.c` (line 77 at the time) programs:

   ```c
   divisor = (cpu_clock / 16) / BAUD_RATE - 1;
   ```

   That **`- 1`** means the RTL8196E UART interprets the DLL/DLM value
   as **(N + 1)**, not N.  The real baud-rate table with N+1 convention
   becomes:

   | Baud    | Quot (programmed) | RTL interprets as | Actual wire | Error   |
   |---------|------------------:|------------------:|------------:|--------:|
   | 115 200 |               108 |               109 |    114 679  | −0.45 % |
   | 230 400 |                54 |                55 |    227 272  | −1.36 % |
   | 460 800 |                27 |                28 |    446 428  | **−3.12 %** |

   −3.12 % is well beyond the ±2 % tolerance.  ~40 % FE on back-to-back
   ASH bytes is consistent with this magnitude of sampling drift.

4. **Proved it experimentally** — no kernel rebuild needed.  With
   the EFR32 at 460 800 and the **unpatched** kernel, arming the bridge
   at `fake_baud = 480 769` forces the 8250 core to compute `quot = 26`.
   Under N+1 convention: RTL interprets as 27 → wire = 462 963 (+0.47 %
   vs 460 800).

   Result: **`tx:680 rx:513, fe=0`**.  Under the standard convention,
   the wire would have been 480 769 (+4.3 %) → heavy FE.
   This is a clean binary discrimination — the N+1 hypothesis is the
   only one consistent with `fe=0`.

### Fix

Added `rtl8196e_uart_set_divisor()` in `8250_rtl819x.c` via the
`port->set_divisor` hook.  It programs `quot - 1` before calling
`serial8250_do_set_divisor()`, so the hardware lands on the intended
baud.  Mirrored identically to the 5.10 production tree.

```c
static void rtl8196e_uart_set_divisor(struct uart_port *port,
                    unsigned int baud, unsigned int quot,
                    unsigned int quot_frac)
{
    unsigned int adjusted = quot > 1 ? quot - 1 : quot;
    serial8250_do_set_divisor(port, baud, adjusted);
}
```

### Validation

- **460 800 baud, patched kernel, NCP@460 800**: bridge armed →
  Z2M handshake + EZSP bringup + 2 devices reconnected.
  `tx:757 rx:1194, fe=0, oe=0` after 8 h soak test (overnight).
- **115 200 / 230 400**: no regression (now +0.47 % error instead of
  −0.45 % / −1.36 % — closer to true baud, not further).
- Console `ttyS0` at 38 400 unaffected (same clock, divisor large
  enough that ±1 is negligible).

### Lessons

1. **Read the bootloader source before assuming your peripheral is
   standard.**  The RTL8196E 16550A looks textbook but its divisor
   register has an off-by-one convention.  The vendor bootloader knew;
   the Linux kernel's generic 8250 driver could not.
2. **Low-baud success hides high-baud bugs.**  At 115 200 the N+1
   error is −0.45 %, invisible.  At 460 800 it's −3.12 %, fatal.
   The bug was always there but only bites above ~300 kbaud.
3. **You can prove divisor hypotheses without rebuilding anything.**
   By arming at a "fake" baud that forces a specific `quot`, you can
   verify what the hardware actually produces on the wire.  One sysfs
   write discriminated the two hypotheses in 20 seconds.
4. **Incremental kernel builds didn't re-copy `files-6.18/`** (at the
   time).  The build script skipped the file overlay when the build
   tree already existed.  Fixed since: `build_kernel.sh` now rsyncs
   the overlay into the build tree on every run.

---

## UART bridge hardening pass (2026-04-16)

A security and robustness pass on the in-kernel `rtl8196e-uart-bridge` added the
`bind_addr` parameter and root-only permissions on the connection parameters, TCP
keepalive on the client socket, transactional reconfiguration (a failed change rolls
back instead of leaving a half-armed bridge), and the read-only `armed` and `stats`
parameters. It also stopped arming the bridge at `late_initcall`, when `/dev/ttyS1`
does not exist yet: the module now loads with `enable=0` and the `S50uart_bridge`
init script arms it.

The bridge has changed a great deal since (a worker per client and client
replacement in v1.7, more counters, flow control). Its current behaviour is
described next to the source, in `files-6.18/drivers/net/rtl8196e-uart-bridge/`
(`README.md`, `DESIGN.md`, `SECURITY.md`).

---

## Outcome

The port completed, and 6.18 became the single production kernel line
in v3.0.0 (see `../CHANGELOG.md`).

---

## Reproducibility

The build script, patch count and image name quoted at the time of
this port have all changed since. See `README.md` in this directory for
the current build procedure.
