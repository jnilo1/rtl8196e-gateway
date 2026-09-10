# 8250_rtl819x — design notes

| | |
|---|---|
| **Last updated** | 2026-07-18 |
| **Driver version** | 1.7 |
| **Active release** | v4.4.0 (kernels `6.18.45` and `7.1.9`, `-rtl8196e-v4.4.0`) |

Architecture companion to [`AUDIT.md`](AUDIT.md). This is the glue
driver for **UART1 = ttyS1 = the EFR32 radio link** — the wire under
the uart-bridge, Z2M/cpcd/otbr-agent, and `flash_efr32.sh`. UART0
(console, ttyS0) deliberately stays on the stock `ns16550a` path; this
driver never touches it. The `files-6.18` and `files-7.1` overlays
carry byte-identical copies of the driver and of these docs.

## 1. Why a glue driver at all

A plain `ns16550a` node almost works — and "almost" cost weeks of
debugging across three issues. The SoC needs four things the generic
driver cannot provide:

1. **Pin mux.** Bits 1/3/6 of `PIN_MUX_SEL` (syscon offset 0x40) must be
   set or UART1 runs perfectly *inside* the SoC (THRE fires) while no
   electrical signal reaches the EFR32. Our bootloader, unlike Tuya's,
   does not set them, and the ethernet drivers historically clobbered
   the neighbouring field. Probe sets them via syscon/regmap and treats
   lookup failure as fatal (AUDIT 8250RTL-001). Since v1.7 the initial
   value is saved and the UART1 bits are restored on probe failure and
   on removal (8250RTL-016).
2. **The SoC flow-control gate.** RTS/CTS needs bit 29 of the 32-bit
   word at +0x10 — which is nothing exotic: it is `UART_MCR_AFE` seen
   through the BE bus's byte-lane routing (the core's MCR byte writes
   land in bits 31:24). CRTSCTS in termios alone is not sufficient; the
   driver syncs the flag into the hardware on every set_termios call.
3. **The N+1 divisor quirk.** DLL/DLM are interpreted as (N+1), not N —
   the stock bootloader's `clock/16/baud - 1` is the tell. Harmless
   inside 16550 tolerance at ≤230400, catastrophic (~3% error, ~40%
   framing errors) at 460800. `set_divisor` programs `quot - 1`.
4. **ttyS1 as a platform contract.** The DT keeps a single
   `realtek,rtl8196e-uart` compatible (no `ns16550a` fallback — the
   6.18 of_platform matcher would hand the node to `8250_of.c` and skip
   the pin mux, the exact rx=0 failure the dtsi comment documents), and
   probe fails outright if the core assigns any line but 1
   (AUDIT 8250RTL-002).

Everything else is the stock 8250 core: this driver registers a port
with `serial8250_register_8250_port()` and overrides a small set of
port hooks.

## 2. The hooks

| Hook | Origin | What it does |
|---|---|---|
| `set_termios` | #89 — RX overruns at 460800 | After `serial8250_do_set_termios()`, syncs the bit-29 gate to the *absolute* CRTSCTS state (8250RTL-008), RMW under `port->lock` (the core's competing MCR byte writes hold the same lock). Sets/clears `UPSTAT_AUTOCTS\|AUTORTS` only after the MCR read-back confirms the state (v1.7) |
| `set_divisor` | framing errors at 460800 | N+1 compensation: `quot ? quot - 1 : 0` (v1.7, 8250RTL-017) |
| `startup` / `shutdown` | v1.7, 8250RTL-010 | `startup` pins `up->fcr` to FIFO + trigger 1 *before* `serial8250_do_startup()` programs the FCR, then arms AFE under the real port lock; `shutdown` disarms AFE in the 8250 shadow and the hardware before `serial8250_do_shutdown()`. All port state is armed per-open through the core lifecycle — probe touches neither FCR nor AFE |
| `throttle` / `unthrottle` | #109 lineage; v1.7, 8250RTL-011 | Replaces the former `set_mctrl` RTS guard. With `UPSTAT_AUTORTS` advertised, `uart_throttle()` calls `throttle()` *instead of* clearing RTS in software (the wedge that killed OTBR after days of uptime). `throttle()` stops the RX interrupts so the FIFO fills and **hardware** AFE deasserts RTS — the core's `skip_rx` logic in `serial8250_handle_irq` cooperates by refusing to drain RX while the UPSTAT bits are set and RDI/RLSI are masked. `unthrottle()` re-enables `UART_IER_RLSI\|UART_IER_RDI`. Mainline `8250_omap` pattern |
| `handle_irq` | v1.5 — multi-day field soft-lockups | Stuck RX-timeout IIR recovery: when IIR reports a character timeout while LSR shows an empty FIFO, one dummy RBR read clears the latch (dw8250 precedent). Silent by default; `phantom_count` (module param, atomic64) counts occurrences. Set the boolean `phantom_log` sysfs parameter to `Y` to emit a deferred, workqueue-based log line; `1` is an accepted alias and reads back as `Y` (v1.7, 8250RTL-013 — never from hard IRQ context) |

The flow-control enable path deliberately forces the **full** MCR
pattern (DTR|RTS|OUT2|AFE = 0x2B000000) rather than just setting AFE:
the v3.5.1 incident showed the core's byte writes rebuilding the MCR
from its mctrl shadow, leaving 0x20000000 (AFE alone, RTS low) — same
wedge, different door. The invariant is unchanged since #109 — **while
AFE owns the line, software never leaves MCR-RTS low** — but v1.7
enforces it the mainline way: `serial_core` is *told* the hardware owns
RTS (`UPSTAT_AUTORTS`) and delegates, instead of being overridden after
the fact. Explicit `B0` (and a deliberate root `TIOCMBIC`) now really
drop the lines: bits 24/25 clear, AFE bit 29 untouched — MCR
`0x28000000` on the bench.

## 3. Probe sequence (order matters)

1. Resource lookup; probe fails unless the MMIO window covers the MCR
   word at +0x10 (8250RTL-015); plain `devm_ioremap` — deliberately
   **without** claiming the mem region: the 8250 core claims it itself
   in `config_port`, and claiming it first silently reverts the port to
   FIFO-off char mode (8250RTL-007). `resource_size()` goes to
   `port.mapsize`.
2. Pin mux via syscon — fatal on failure, before anything can appear to
   work; initial value saved for restore (8250RTL-016).
3. Optional clk; IRQ from DT; uartclk from `clock-frequency`, else the
   clock framework, else **probe fails** — no silent 200 MHz fallback
   (8250RTL-015). The dtsi provides 200 MHz = the LX bus clock all baud
   math uses.
4. Hooks installed (`set_termios`, `set_divisor`, `throttle`,
   `unthrottle`, `startup`, `shutdown`, `handle_irq`);
   `UART_CAP_AFE` set when DT says
   `auto-flow-control`/`uart-has-rtscts`.
5. `serial8250_register_8250_port()`, assert line 1.

Probe performs **no** AFE or FCR writes (8250RTL-010): flow control and
the trigger are armed per-open by `startup()`. Operational consequence:
**MCR reads 0x00000000 until the first open** — a `devmem 0x18002110`
right after boot showing zero is expected, not a regression.

`remove()` unregisters the port, cancels the deferred phantom-log work,
drops the clock and restores the saved pin-mux bits.

## 4. Consumers

```
8250_rtl819x (ttyS1)
  └── rtl8196e-uart-bridge (tty_port client_ops, TCP:8888)
        ├── Z2M / ZHA (EZSP)            ├── cpcd → zigbeed (CPC)
        ├── otbr-agent (Spinel)         └── flash_efr32.sh (Xmodem)
```

The bridge owns the tty in production and drives CRTSCTS through its
`flow_control` parameter; `flash_efr32.sh` drops flow control around
Xmodem transfers. Baud changes arrive via the bridge's `baud` knob →
termios → `set_divisor`. The supported ladder (115200 / 460800 /
691200 / 892857) tops out at the N+1 divisor minimum (200 MHz ÷ 16 ÷ 14).

## 5. Invariants

1. **The DT node keeps a single compatible.** Adding an `ns16550a`
   fallback re-creates the silent rx=0 failure on 6.18.
2. **The pin-mux write stays fatal-on-failure and stays in this
   driver** until pinctrl provably owns it — a ttyS1 that exists but
   isn't wired is the worst failure mode this platform has produced.
3. **While AFE owns the line, no software path may leave MCR-RTS
   low.** Enforced by advertising `UPSTAT_AUTOCTS|AUTORTS` (set only
   on confirmed MCR read-back) so `serial_core` delegates throttling
   to the driver instead of clearing RTS itself. Explicit `B0` and a
   deliberate root `TIOCMBIC` are the sanctioned exceptions. Any new
   MCR access goes through the existing helpers under `port->lock`.
4. **`set_divisor` keeps the N+1 compensation.** `quot ? quot - 1 : 0`;
   the zero encoding (divide-by-1) is only reachable at ≥12.5 Mbaud and
   is unvalidated on the wire — logic-analyzer pass required before any
   such experiment.
5. **ttyS1 or fail.** The line number is API for the bridge,
   `S50uart_bridge`, `S70otbr` and `radio.conf`.
6. **Probe never claims the UART mem region.** The 8250 core requests it
   in `config_port`; a prior devm claim makes that fail silently and the
   port runs FIFO-off with `rx_trig_bytes` missing (8250RTL-007 — the
   v1.2 regression this driver lived with from F-08 to v1.3).
7. **The RX FIFO trigger stays pinned at 1 — re-pinned by `startup()`
   on every open.** Silicon erratum: trigger levels above 1 overrun
   erratically and non-monotonically under sustained line-rate load
   (8250RTL-009, full bench table in AUDIT). Trigger 1 + enabled FIFO =
   the full 16-byte latency cushion and the exact wire behaviour every
   v3.x release was validated on. Raising it via `rx_trig_bytes` is for
   experiments only and lasts until the next open.
8. **Port state arms through the core lifecycle only.** `startup()` and
   `shutdown()` are the single place AFE and FCR are armed/disarmed
   outside a termios change; probe-time hardware writes are limited to
   the pin mux (8250RTL-010).
