# rtl8196e-uart-bridge — design notes

| | |
|---|---|
| **Last updated** | 2026-06-12 |
| **Driver version** | 1.4 |
| **Active release** | v4.6.0 (kernels `6.18.51` and `7.2.5`, `-rtl8196e-v4.6.0`) |

This document explains what the driver does, why it exists, and the key
choices that shaped the stabilised code in
`rtl8196e_uart_bridge_main.c`. The companion operator reference is
[`README.md`](README.md); the SSH-tunnel hardening recipe lives in
[`SECURITY.md`](SECURITY.md); the code-level security and simplification
review is [`AUDIT.md`](AUDIT.md).

## Scope

The bridge carries bytes between `/dev/ttyS1` (UART1 on the RTL8196E)
and a single TCP client on port 8888 of the gateway. It is the single
shared transport for **every** host ↔ radio conversation:

| Gateway-side firmware | Host-side client | Protocol over the bridge |
|---|---|---|
| NCP-UART-HW (EmberZNet 7.5.1) | Z2M / ZHA | EZSP v13 |
| RCP-UART-HW (802.15.4) | `cpcd` → `zigbeed` | CPC (Zigbee or Thread) |
| OT-RCP (OpenThread) | `otbr-agent` | Spinel-over-CPC |

Anything that talked to the radio over the former userspace
`serialgateway` now talks to the bridge on the same TCP endpoint. The
host-side stack does not care which kernel-side path ships the bytes;
the transport is the same.

## Why the old userspace path hit a wall

Pre-v3.0 the same shovel role was fulfilled by `serialgateway`, a
~520 LOC userspace daemon looping `read(tty)` / `write(tcp)` and
vice-versa. On a 400 MHz Lexra RLX4181 (single core, no SMP, no
hardware integer divide), each byte batch crossed the user/kernel
boundary four times and relied on the scheduler waking the daemon
within a tight window set by the 16-byte UART RX FIFO.

At 460 800 baud the wire delivers a byte every 21.7 µs, leaving
roughly 170 µs to drain eight freshly filled FIFO slots before an
overrun is latched. In practice the daemon started missing that
window intermittently under load — `/proc/tty/driver/serial` showed
non-zero `oe:` counts, and the ASH / CPC stack above it reacted with
retransmits or outright disconnects. Above 460 800 the problem became
reliably reproducible.

The userspace design could be micro-tuned (wake priorities, pinned
memory, larger reads) but any serious fix had to remove the context
switches, not make them faster.

## The kernel-side shovel

The driver forwards bytes entirely in kernel context:

```
 UART1 RX -> 8250 ISR -> tty flip buffer -> bridge receive_buf()
                                             -> kernel_sendmsg(TCP)

 TCP RX   -> bridge worker kthread
                                             -> tty->ops->write(UART1 TX)
```

### Hook point: `tty_port.client_ops` override, not a line discipline

An earlier prototype used a line discipline. It worked, but two things
made us move the hook one level down:

1. **`receive_room` ate bytes.** The default ldisc path consults a
   per-ldisc `receive_room` budget before delivering data from the flip
   buffer. When the TCP sendmsg was slow for any reason (client side
   slow-reader, short burst of congestion) the ldisc would silently
   drop characters that our code would have happily swallowed into the
   `drops_err` counter.
2. **Extra ldisc reference-counting round-trip** on every flip-buffer
   flush with no real ldisc behaviour to justify it.

The bridge instead overrides `tty_port->client_ops` and installs its
own `receive_buf`, bypassing `tty_port_default_receive_buf` and the
ldisc layer entirely. The received bytes are forwarded to the TCP
socket with `MSG_DONTWAIT`; the return value back to the tty core is
always the full `count` (bytes are accounted for in the driver's drop
counters, not fed back as flow-control pressure to the flip buffer).

### Single kthread for TCP accept/recv

The TX direction (TCP → UART) runs in one kernel thread that blocks in
`kernel_accept()` then `kernel_recvmsg()`, and writes the received
bytes into the UART via the existing `tty->ops->write` path. This is
the minimal amount of concurrency the driver needs: one thread per
listen socket, one client at a time.

### Single-client listener

The radio has exactly one consumer at any given moment — Z2M, `cpcd`,
or `otbr-agent`. Supporting multiple simultaneous TCP clients would
require duplicating the byte stream and tracking per-client state in a
path that already runs on a tight CPU budget. The bridge serves one
client at a time; a new connection **replaces** the current one — the
old socket is released and the newcomer takes over (`"replacing
previous client"` in the log). That is what lets a restarted Z2M /
`cpcd` reclaim the bridge without operator action; the flip side is
that any peer able to reach the port can evict the connected client at
any time, which is part of why an untrusted segment calls for the
loopback + SSH-tunnel deployment in `SECURITY.md` (see also
AUDIT.md BRIDGE-002).

### Sysfs-only control interface

All knobs (`tty`, `baud`, `port`, `bind_addr`, `flow_control`,
`enable`, `nrst_pulse`, `nrst_gpio`, `blmode_pulse`, `blmode_gpio`,
`status_led_brightness`) are exposed as module parameters under
`/sys/module/rtl8196e_uart_bridge/parameters/`. Writes are applied
live — the set callbacks teardown and rebuild only the subsystem they
touch (e.g. changing `baud` reconfigures `ktermios` without dropping
the TCP client; changing `bind_addr` rebuilds the listen socket but
keeps the connected client). This avoids shipping a userspace tool
just to control the bridge and makes every setting scriptable from
`/etc/init.d`.

### `enable = 0` at load, armed by init script

The module loads unconditionally with the kernel but does nothing
until an init script — `S50uart_bridge` in the userdata overlay —
writes `enable=1` once `/dev/ttyS1` is known to exist. This avoids
the auto-arm race where the bridge would try to open the tty before
the 8250 driver had created the device node. The init script also
pulls `FIRMWARE_BAUD` and `BRIDGE_BIND` from `/userdata/etc/radio.conf`
before arming, so the operator's persistent choices land on every
boot without a second tool.

### STATUS LED fired from the worker, not from userspace

Pre-v3.0, the userspace `serialgateway` daemon wrote
`/sys/class/leds/status/brightness` directly from its TCP accept and
disconnect paths. Moving the shovel into the kernel lost that hook,
so operators reported the STATUS LED staying off in Zigbee mode even
when Z2M was connected.

The bridge restores it via the Linux LED-trigger subsystem:

- At module init the driver registers a trigger named
  `uart-bridge-client` (`led_trigger_register_simple()`).
- On `kernel_accept()` success the worker fires the trigger at the
  brightness stored in `status_led_brightness` (clamped to 0-255).
- On disconnect and on disarm the worker fires the trigger at 0.

Userspace binds the trigger to the actual LED with
`echo uart-bridge-client > /sys/class/leds/status/trigger`; the init
script `S50uart_bridge` does this at boot and also maps the eth0
`led_mode` (bright/dim/off) to 255/60/0 for the brightness. The
coupling between the bridge and the LED class goes only through the
well-defined trigger API — no direct sysfs access from kernel, no
hard-coded device names beyond the trigger label.

Changing `status_led_brightness` while a client is already connected
does not update the LED live; the new value takes effect on the next
connect. That's intentional: an operator changing the brightness is
usually tuning things up, not trying to flicker the LED on a running
session.

### Runtime flow-control flip for flash mode

The Gecko Bootloader's Xmodem upload uses 115200 baud with hardware
flow control **off**. A flash ends up temporarily re-purposing the
same UART/TCP path, so the bridge exposes `flow_control` as a writable
sysfs knob. `flash_efr32.sh` flips it to `0` for the transfer and
back to `1` afterwards; the bridge stays armed throughout and the TCP
listen socket never drops, so nothing on the host side has to
reconnect.

### Software flow control (v1.2) — XON/XOFF lives in the bridge

Boards without RTS/CTS wiring between the SoC and the radio (e.g. the
Sengled G4 port, discussions #119/#123) pair a software-flow-control
radio firmware (NCP-UART-SW) with `flow_control=sw`. Such firmware
escapes data-plane 0x11/0x13 bytes, so a bare XON/XOFF on the wire is
genuine flow control. From v1.5 a board no longer encodes "sw" directly
in the DT: it declares whether it *can* do hardware flow control
(`realtek,hw-flow-control`, see below), which sets the default and caps
`hw` to `sw` on an unwired board; the firmware-specific mode is then an
optional runtime `FIRMWARE_FLOW_CTRL` key.

Two design points:

1. **The bridge handles XON/XOFF itself, not termios.** The hot path
   bypasses the line discipline (see above), and termios `IXON`/`IXOFF`
   are implemented by the ldisc — setting them would do nothing. In sw
   mode `bridge_receive_sw_locked()` scans each UART chunk, strips bare
   XON/XOFF from the TCP-bound stream and gates the TCP→UART worker
   via `sw_tx_paused`. A welcome side effect of consuming the control
   bytes locally: the remote host (Z2M over TCP) sees a clean ASH
   stream and needs no software-flow-control support of its own.
2. **The flow control is asymmetric by design.** The bridge *honors*
   the radio's XOFF (pauses TX up to a bounded 1 s, then fails open and
   counts `tx_pause_timeouts`) but never *emits* XOFF toward the radio:
   the UART→TCP direction keeps its historical drop-and-count semantics
   (`MSG_DONTWAIT`), and at ≤1 Mbaud the 400 MHz SoC never falls behind
   a 16-byte UART FIFO. The bounded wait also keeps the disarm path's
   synchronous `kthread_stop()` safe — the worker can never be parked
   indefinitely on a lost XON.

hw and none modes are untouched: the RX path branches once on the mode
and takes the exact v1.1 single-send code otherwise.

### DT-seeded defaults (v1.2), module params still in charge

Board ports kept patching the genuinely board-specific knobs (nRST
line, flow-control wiring — and from v1.3 the bootloader-entry line,
`blmode-gpios`), so v1.2 lets an optional `/radio-bridge` DT node seed
their boot defaults (see README "Device tree configuration").
The bridge stays a non-platform driver — converting a field-stable
driver to platform binding for a few values wasn't worth the churn; init
just looks the node up by compatible. Explicit kernel-cmdline or sysfs
writes always win over the DT (the param setters record an explicit
write; built-in modules apply cmdline params before `late_initcall`).

**Capability vs mode (v1.5).** The original DT carried `flow-control =
"hw"|"sw"|"none"`, which conflated two unrelated facts: whether the board
*wires* RTS/CTS (a hardware truth) and which mode a given radio firmware
*wants* (a runtime choice). On the G4, with no RTS/CTS, "sw" really meant
"not hw-capable" — so changing firmware needed a DTB rebuild (#134). v1.5
splits them: the DT boolean `realtek,hw-flow-control` is the board
capability (present on Lidl, absent on the G4), seeding the default and
acting as a ceiling — `param_set_flow_control()` clamps an `hw` request to
`sw` when `!rtl_hw_fc_capable`, so CRTSCTS can never reach an unwired UART
regardless of what userspace writes. The firmware mode is the optional
`FIRMWARE_FLOW_CTRL` radio.conf key (parallel to `FIRMWARE_BAUD`), applied
to the same sysfs `flow_control` knob by the init scripts. The capability
is DT/compiled-only (never a writable param); the mode default it sets is
still overridable by cmdline/sysfs, subject to the clamp. The old DT
string parse was dropped outright — the binding was unreleased, and the
two in-tree DTS were converted in lockstep, so no shim was needed.

### nRST pulse — one open-drain GPIO, not a pin-mux trick

`nrst_pulse` (driver v1.1, discussion #121) resets the EFR32 by claiming
the one GPIO line its nRST pad is wired to (`nrst_gpio`, default 12 =
pad B4 on the Lidl board — isolated per-pad on the bench) through the
gpiod consumer API with `GPIO_OPEN_DRAIN`: assert drives the pad low,
release floats it back to input and the EFR32's internal RESETn pull-up
does the rest. RESETn is never driven high — Silabs wires it as an
open-drain input, so pushing it high would fight the chip's own reset
sources. The pad mux to GPIO mode comes for free from the gpio-rtl819x
`request()` hook, and the line is claimed per pulse, so it stays free
for other consumers between pulses and `nrst_gpio` changes take effect
on the next pulse without driver state.

### blmode pulse (v1.3) — bootloader entry stays a separate trigger

Boards that wire the EFR32 bootloader-entry pin to a SoC GPIO (the
Sengled G4; the Lidl board has none) get `blmode_pulse`
(discussions #123/#126): assert `blmode_gpio` → nRST pulse as above →
hold blmode 5 s across the Gecko bootloader's pin-sampling window →
release. Both lines are claimed per pulse with the same open-drain
semantics; `blmode_gpio` defaults to -1 ("no such pin", the trigger
returns `-ENODEV`) and is DT-seeded from `blmode-gpios`.

The design decision worth recording: this is deliberately **not**
folded into `nrst_pulse`. `nrst_pulse` must keep meaning "reset into
the application" — `flash_efr32.sh` and `recover_efr32` depend on it
after a flash — so a presence-of-blmode-gpios behaviour switch would
leave a blmode-wired board unable to ever start its application. Two
knobs, two meanings. The sequence holds `nrst_pulse_lock` for ~5.1 s;
a concurrent pulse writer just waits, the UART→TCP hot path is
untouched (the lock is never taken there). One system-level nuance
(AUDIT.md BRIDGE-003): the kernel serializes every built-in module's
sysfs parameter access on a single global lock (`kernel/params.c`), so
during the 5.1 s sequence *all* built-in param reads/writes — including
this driver's own `stats` — queue behind it. Only the hot path is
truly unaffected.

Driver v1.0 instead set PIN_MUX_SEL_2 bits {7,10,13} — three separate
mux fields copied wholesale from the chip's reset-default value — which
re-routed two unrelated pads (B5, B6) for the duration of every pulse.
Only the B4 field ever mattered, and the hard-coded mask made the knob
useless on RTL8196E boards with different nRST routing.

## Options considered and dropped

- **Placing the hot path in IRAM.** Early scoping assumed we would
  need the 16 KB on-chip instruction SRAM that the Lexra kernel
  already uses for the Ethernet RX path. Once the plain-text kernel
  build shipped, hardware measurements at 892 857 baud under a
  multi-hour soak showed zero framing/overrun errors and no packet
  drops. The IRAM work was shelved as unnecessary complexity.

- **Multi-client fan-out.** A version that duplicated RX to every
  connected client was prototyped on paper. It would require per-client
  send queues, back-pressure accounting per client, and a policy for
  what happens when a slow client causes a fast one to starve. No
  workflow on the gateway actually benefits from fan-out — one
  supervisor per radio is the norm. Dropped.

- **Netlink control plane.** Considered as an alternative to sysfs.
  Rejected because it would require a userspace client library and
  provide no functionality the sysfs knobs don't already cover. The
  init script is a three-line shell fragment, not a daemon.

- **Keeping the line discipline.** See above — `receive_room`
  behaviour and the extra ldisc round-trip were both net negatives
  for our use case, and we have no consumer of ldisc features
  (canonical mode, signals, echo) since both sides of the wire speak
  framed binary protocols.

## Stability properties

- **Throughput:** 892 857 baud (the N+1 divisor maximum on this SoC)
  sustained with the bridge armed and a Z2M EZSP client loaded with
  a representative Zigbee network. Kernel 8250 stats show `fe=0 oe=0`
  over multi-hour soaks; bridge `drops_*` counters stay at 0.
- **Lower bauds:** 115 200 (baseline NCP/RCP/Router/OT-RCP), 230 400,
  460 800, 691 200 validated on the same setup.
- **Live reconfiguration:** baud, bind address, flow control, listen
  port can all be changed on the running bridge without dropping the
  TCP client (except `bind_addr` and `port`, which rebuild the listen
  socket; an already-connected client is not affected).
- **Boot ordering:** the module is built-in and loads with `enable=0`;
  the init script defers arming until after the 8250 probe has
  published `/dev/ttyS1`. The serialgateway-era race between daemon
  startup and tty availability is structurally gone.
- **Hot path cost (UART -> TCP):** `bridge_port_receive_buf()` takes
  `bridge_lock` and calls `kernel_sendmsg()` while holding it. An
  early audit flagged this mutex as a potential bottleneck at high
  baud; measured cost is negligible. An EZSP `echo` flood at 892 857
  baud drove the path ~3 500 calls/s for 30 s with **0 `drops_err`,
  0 `drops_tx`, 0 UART framing/overrun errors**. The worker kthread
  consumed ~2.5 % CPU, total system load ~19 %, ~81 % idle. Average
  per-call mutex cost ≈ 8 µs, which leaves plenty of headroom even
  if the line ever approached its ~89 kB/s ceiling. The mutex is
  kept as-is: no refactor to a spinlock or a lock-free queue is
  warranted.

## What this driver does not do

- No TLS, no auth. A reachable TCP port is a direct EZSP/CPC/Spinel
  session. For untrusted networks, bind to loopback and tunnel over
  SSH — see `SECURITY.md`.
- No buffering beyond what the UART FIFO, tty flip buffer, and TCP
  socket provide. There is no in-bridge replay/queue if the client
  disconnects mid-frame; the upper-layer protocol (ASH for EZSP, CPC
  for RCP/OT-RCP) is responsible for recovery.
- No multi-PAN / multiplexing. One radio, one UART, one TCP client at
  a time. Multi-PAN concurrency is handled host-side by `zigbeed` +
  `otbr-agent` sharing the same RCP via `cpcd`, which in turn is the
  single TCP client on our end.
