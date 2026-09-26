# irq-rtl819x — design notes

| | |
|---|---|
| **Last updated** | 2026-06-12 |
| **Driver version** | 1.0 |
| **Active release** | v4.6.0 (kernels `6.18.51` and `7.2.5`, `-rtl8196e-v4.6.0`) |

Architecture companion to [`AUDIT.md`](AUDIT.md). Explains what the RTL8196E
interrupt controller actually is, how the driver maps it onto the Linux
irqchip model, and the invariants that must survive any future edit.

## 1. The hardware

The INTC at `0xB8003000` is a thin 32-source controller in front of the
MIPS CPU's interrupt pins:

| Register | Offset | Role |
|---|---|---|
| GIMR | 0x00 | global interrupt mask, one bit per source |
| GISR | 0x04 | global interrupt status, W1C per bit |
| IRR0–IRR3 | 0x08–0x14 | routing: one 4-bit field per source selects the CPU IP line (0–7) |

Sources used on this platform (GIMR/GISR bit numbers): TC0 timer (8),
UART0 console (12), UART1 radio link (13), switch core / Ethernet (15).
IRR1 covers bits 8–15, four bits per source. Everything else (USB, OTG,
TC1, PCIe fields in IRR2) is routed to 0 = disabled.

**Priority is positional, not programmable.** `plat_irq_dispatch()` services
pending CPU IPs in fixed order IP7 > IP4 > IP3 > IP2. The routing therefore
*is* the priority policy:

| Source | IP line | Why |
|---|---|---|
| TC0 timer | IP7 | clocksource/clockevent — must preempt everything |
| UART1 (EFR32 Zigbee link) | IP4 | 16-byte 8250 RX FIFO ≈ 350 µs budget at 460 800 baud; an overrun drops a Zigbee frame and forces Z2M/ZHA to reconnect — user-visible |
| Switch / Ethernet | IP3 | DMA rings + NAPI; a delayed IRQ costs at most a TCP retransmit — invisible |
| UART0 (console) + everything else | IP2 | cascade, latency-insensitive |

This UART1↔switch ordering is the PERF-UART1-IRR swap (v3.4.0), validated
by an 8 h OTBR soak at 460 800 baud with zero overruns.

## 2. Driver shape

~200 pure LOC, three layers:

1. **`irq_chip` callbacks** — `mask`/`unmask` do a GIMR RMW under a raw
   spinlock; `ack` is a single W1C write to GISR. All three bounds-check
   `hwirq < 32`. Children run `handle_level_irq`.
2. **One chained handler for all parents.** `intc_of_init()` walks the DT
   `interrupts` list (`<2>, <3>, <4>` = cascade, switch, uart1) with
   `irq_of_parse_and_map()` and installs the same
   `realtek_soc_irq_handler` on each. The handler reads `GIMR & GISR` and
   dispatches every pending bit (`__ffs` order, lowest first), so whichever
   IP fires first drains all ready sources; the sibling IP then takes the
   documented spurious path (`pending == 0`, enter/exit only). The handler
   is `__iram` (Lexra on-chip instruction RAM, hot path).
3. **Legacy irqdomain**, 32 hwirqs at fixed virq base 16
   (`irq_domain_create_legacy`), `xlate_onecell`. `intc_map()` caches the
   virqs of the three hot sources (12/13/15) for the handler's `switch`;
   everything else goes through `irq_find_mapping()` — which on a legacy
   domain is an O(1) revmap lookup anyway (AUDIT IRQ-010: keep the cache,
   don't extend it).

## 3. Init sequence (order matters)

1. Map registers (`of_address_to_resource` + `ioremap`).
2. Program IRR1/IRR2 routing (and zero IRR0/IRR3) — before anything can
   fire.
3. Create the domain (eagerly maps all 32 hwirqs, populating the virq
   cache).
4. Chain the handler on every DT-declared parent IP.
5. **`GIMR = BIT(8)` — TC0 only.** UART0/UART1/switch bits are set later by
   `.irq_unmask` when their consumer calls `request_irq()` (IRQ-001 fix:
   never expose the chained handler to a source no driver drains yet).

## 4. The TC0 special case (do not "fix")

The timer DT node is parented to `&cpuintc/<7>`, so `timer-rtl819x`
requests CPU IRQ 7 directly and **never traverses this driver's
`.irq_unmask`**. But the only hardware path TC0 → CPU is through INTC
IRR1 + GIMR — there is no bypass (verified against the bootloader:
`31-Bootloader/boot/monitor.c:163,190` routes TC0 via IRR1, `irq.c:39`
arms GIMR bit 8). Hence the unconditional `GIMR = BIT(8)` at init; clearing
it hangs the kernel at clocksource init. No chained handler is installed on
IP7, so there is no double dispatch; GISR bit 8 clears when the timer
driver W1Cs its own `TC_IR`. Full analysis: AUDIT IRQ-002 (rejected).

## 5. Dependencies

| Consumer | hwirq | Path |
|---|---|---|
| `8250_rtl819x` UART0 | 12 | `&intc` → IP2 cascade |
| `8250_rtl819x` UART1 | 13 | `&intc` → IP4 |
| `rtl8196e-eth` | 15 | `&intc` → IP3 |
| `timer-rtl819x` | — | `&cpuintc/<7>` directly; INTC only routes/arms (§4) |

DT node: `intc@3000`, compatible `realtek,rtl819x-intc`,
`#interrupt-cells = <1>` (plain source bit number, `xlate_onecell`).

## 6. Invariants

1. **GIMR bit 8 is armed at init, unconditionally** — and nothing else (§4).
2. **Routing lives in two places that must agree**: the IRR1 nibbles in
   `realtek_soc_irq_init()` and the DT `interrupts`/`interrupt-names` on
   `intc@3000`. Change one, change both.
3. **UART1 outranks the switch** (IP4 > IP3). Re-benching the Zigbee link at
   ≥460 800 baud is mandatory before any routing change.
4. **The chained handler never W1Cs GISR** — ack belongs to `.irq_ack` via
   the level flow (IRQ-004); a parent-side ack would be a redundant MMIO
   write per interrupt.
5. **`pending = GIMR & GISR`**, never GISR alone — masked sources must not
   be dispatched.
6. **The handler stays `__iram`** — IRAM placement is part of the measured
   interrupt-latency budget on this platform (see the exp/no-imem bench
   history before touching any `__iram` annotation).
