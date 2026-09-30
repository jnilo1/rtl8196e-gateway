# rtl8196e-uart-bridge — cumulative driver audit

> **Cumulative audit ledger.** The opening table and the finding registry (§5)
> are authoritative. §1 records the passes; §2–§4 keep the technical
> conclusions that the code comments and `DESIGN.md` / `SECURITY.md` rely on.

| Current state | Authoritative value |
|---|---|
| **Audited code** | driver v1.7 |
| **Last audit pass** | 2026-07-30 — C6, live-EZSP target validation of v1.7 |
| **Last fully audited baseline** | v1.6 (pass A2) |
| **Post-baseline changes** | v1.7 implements BRIDGE-009…011, including the teardown lock-order correction (C4); each change was reviewed and target-validated in passes C3–C6 |
| **Validation state** | `W=1` builds and complete `vmlinux` links; BRIDGE-009…011 target gates passed on Linux 6.18 and 7.1 (C5, C6) |
| **Maintained kernels** | Linux 6.18 and 7.2; the two sources differ only by the `kernel_bind()` cast to `struct sockaddr_unsized *` required since Linux 7.1 |
| **Scope** | driver, Kconfig/Makefile, tty/socket/GPIO concurrency and exposed TCP/sysfs surfaces |
| **Companions** | `DESIGN.md`, `README.md`, `SECURITY.md` |

## 1. Audit-pass ledger

| Pass | Date | Baseline | Scope and outcome | IDs |
|---|---|---|---|---|
| A1 | 2026-06-12 | v1.3 → v1.4 | first standalone audit: full read of the source against the 6.18 tty, socket, gpiod and `kernel/params.c` APIs. No memory-safety flaw; S01…S03 implemented as v1.4, bench-checked on the bench gateway (armed at boot, three connect/disconnect cycles) | BRIDGE-001…008, S01…S03 |
| C1 | 2026-06-13 | v1.4 → v1.5 | flow-control capability / firmware-mode split; change-local review | — |
| C2 | 2026-07-16 | v1.5 → v1.6 | `blmode_pulse` logs the sequence it performed, not an outcome it cannot observe; change-local review | — |
| A2 | 2026-07-30 | v1.6 | independent full security/performance re-audit (source read before this ledger); corrected A1's BRIDGE-002 conclusion; targeted `W=1` builds on both kernels, `checkpatch.pl --strict` style debt only, no runtime test | BRIDGE-009…011 opened |
| C3 | 2026-07-30 | v1.6 → v1.7 | BRIDGE-009…011 implemented (§3); objects and complete `vmlinux` links with `W=1` on both kernels | BRIDGE-009…011 |
| C4 | 2026-07-30 | v1.7 candidate | target-discovered teardown self-deadlock fixed (§3); static lock-order review and `W=1` links | BRIDGE-011 |
| C5 | 2026-07-30 | v1.7 | target regression at 460800 baud, hardware flow control: 6.18 stop/start; 7.1 ten direct disarm/arm cycles, idle A→B replacement, teardown with a dormant client; no BUG, oops, hung task, call trace or warning | BRIDGE-009, 011 |
| C6 | 2026-07-30 | v1.7 | remaining gates closed with protocol-valid, read-only EZSP traffic (§4) | BRIDGE-009…011 |

Passes C5 and C6 ran on Linux 6.18.38 and 7.1.3. The 7.2 line carries the
same driver source with the `kernel_bind()` cast noted above.

## 2. Security review

### 2.1 Attack surface

| Surface | Who can reach it | Exposure |
|---|---|---|
| TCP listener (default `0.0.0.0:8888`) | any peer that can route to the gateway | **Unauthenticated, plaintext EZSP/CPC/Spinel session with the radio.** Deliberate, documented, mitigated by `BRIDGE_BIND=127.0.0.1` + SSH tunnel (`SECURITY.md`). Since v1.7 a new connection synchronously evicts the connected client (BRIDGE-009). |
| UART RX bytes (radio side) | EFR32 firmware (or whoever flashed it) | In `flow_control=sw`, bare 0x11/0x13 gate the TCP→UART direction — bounded to 1 s by the fail-open timer, so a hostile/wedged radio cannot park the worker or hang disarm. In hw/none modes, bytes are forwarded verbatim. |
| Module parameters (sysfs) | root only — modes 0600/0644/0444/0200, write bit owner-only on every knob | Validated setters (§2.2). Pulse knobs hold the **global** built-in `param_lock` for their duration (BRIDGE-003). |
| Device tree `/radio-bridge` node | build-time (trusted) | Seeds `nrst_gpio` / `blmode_gpio` and the `realtek,hw-flow-control` capability (the `flow_control` default) only; GPIO lines range-checked (`args[0] <= 31`). The capability is also a ceiling: an `hw` request is clamped to `sw` when the board lacks it, so CRTSCTS is never asserted on an unwired UART. |

### 2.2 Verified correct

Established by A1 on v1.3 and re-confirmed by A2 on v1.6 (no memory
corruption, UAF or double release found):

- **Socket ownership.** `bridge_send_to_client_locked()` never releases the
  client socket on error; it only `kernel_sock_shutdown()`s it to wake the
  reader. Exactly one party releases each socket. Since v1.7 the per-client
  worker owns and releases its socket (§3).
- **`stopping_worker` handshake.** A connection accepted inside a
  disarm/reconfig teardown window is released immediately instead of being
  installed as state the teardown no longer knows about; the flag is re-checked
  after every re-acquisition of `bridge_lock`.
- **`listen_sock` lifetime vs the lockless reader.** The accept worker reads
  `state.listen_sock` with `READ_ONCE()`; teardown keeps the pointer populated
  until the synchronous `kthread_stop()` has returned.
- **Bounded XOFF gate.** The sw-mode TX pause is a 1 s fail-open wait that also
  polls `kthread_should_stop()`. A network client cannot inject flow control:
  the XON/XOFF scan runs only on the UART→TCP direction.
- **nRST / blmode pulses.** Open-drain throughout (driven low or floated, never
  driven high against the EFR32's RESETn pull-up); write-only mode 0200,
  serialized by `nrst_pulse_lock`, lines claimed per pulse. `blmode_pulse`
  validates `blmode_gpio >= 0` and `blmode_gpio != nrst_gpio` and releases the
  already-claimed line on error.
- **Input validation.** baud 1200–4 000 000; port 1–65535; gpio lines 0–31
  (blmode −1–31); brightness 0–255; `bind_addr` through `in4_pton()`;
  `flow_control` parsed into a closed enum (numeric ABI kept for
  `flash_efr32.sh`); string params copied with `strscpy` into fixed buffers with
  `-ENAMETOOLONG` on overflow.
- **No torn reads.** All `u64` counters are read and written under `bridge_lock`
  (32-bit MIPS has no atomic 64-bit loads); the lockless reads are
  `READ_ONCE`/`WRITE_ONCE` pairs with documented staleness tolerance.
- **`kernel_getpeername()` into `struct sockaddr_in`** is safe on an AF_INET
  socket.
- **`tty_kopen_exclusive`** guarantees no userspace open can race the
  kernel-side claim.

### 2.3 Standing risk and deployment contract

Anyone who can reach the listen port owns the radio; the threat model and the
loopback + SSH mitigation live in `SECURITY.md`. The deployment security
contract is **one active TCP client at a time** on the bridge port, met by
BRIDGE-009's synchronous replacement. Authentication, encryption and
loopback-only binding are deployment choices, outside that contract.

## 3. Findings

### BRIDGE-001 — config-transition atomicity depends on the global param lock (low, latent)

`bridge_disarm_locked()` and the relisten path drop `bridge_lock` around
`kernel_sock_shutdown()` + synchronous `kthread_stop()`. In isolation, that
window would let a concurrent `enable=1` arm fresh state that the resuming
teardown then wipes (NULL `listen_sock` under a live worker, orphaned kthread,
leaked refs). It is unreachable today because every sysfs write to a built-in
module's parameters runs under the **global** `param_lock`
(`param_attr_store()` → `kernel_param_lock(NULL)` in `kernel/params.c`), and
boot-time parsing is single-threaded. The risk is that the invariant lives
outside the driver: a future non-param entry point (reboot notifier,
platform-driver conversion, ioctl) would reopen the race. **S01 (v1.4):** the
dependency is documented at the `bridge_lock` definition and at the disarm
drop-and-retake site.

### BRIDGE-002 — replace-on-connect claim (low; superseded by BRIDGE-009)

A1 inferred from the `if (state.client_sock)` branch that a new connection
replaced the current one, and amended `DESIGN.md` / `SECURITY.md` accordingly.
A2 showed that branch was unreachable in v1.6 (§BRIDGE-009). v1.7 implements
replacement for real, so the amended documents now describe the code.

### BRIDGE-003 — pulse knobs hold the global param lock (info)

Because of the same `kernel/params.c` serialization, `nrst_pulse` holds the
global `param_lock` for ~100 ms and `blmode_pulse` for ~1.1 s
(`msleep(100)` + `msleep(1000)`; the v1.3 sequence audited by A1 held the
blmode pin for 5 s, ~5.1 s in total). During that time every sysfs parameter
read/write of every built-in module blocks, including this driver's
`stats`/`armed` getters. Only the UART→TCP hot path
(`bridge_port_receive_buf`) is genuinely unaffected. Operationally harmless
(rare, root-triggered maintenance actions). **S03 (v1.4):** the
`nrst_pulse_lock` comment now states this behaviour.

### BRIDGE-004 — connection-lifecycle messages not ratelimited (low)

A LAN peer flapping connections could churn the kernel log and the ramfs
`/var/log/messages`. **S02 (v1.4):** the connect/replace/disconnect messages
are `pr_info_ratelimited`.

### BRIDGE-005 — kthread names truncated (info)

`DRV_NAME "-worker"` (and, since v1.7, `DRV_NAME "-client"`) exceed
`TASK_COMM_LEN`; `ps` shows `rtl8196e-uart-b`. Cosmetic.

### BRIDGE-006 — stale ancillary text (info)

The `Kconfig` help predated the v1.3 `blmode_pulse`/`blmode_gpio` knobs, and
`README.md` misstated the source size. **S03 (v1.4)** refreshed the Kconfig
help; the README figure was corrected by A1.

### BRIDGE-007 — DT gpio phandle controller is ignored (info, accepted)

`bridge_seed_defaults_from_dt()` consumes only the line number of
`nrst-gpios`/`blmode-gpios`; the pulse paths claim through the
`"gpio-rtl819x"` label. Irrelevant on this single-gpiochip SoC and documented
in `README.md`; a multi-gpiochip port must revisit it.

### BRIDGE-008 — tty path→devt TOCTOU (info, accepted)

`resolve_tty_devt()` resolves the path, then `tty_kopen_exclusive()` opens by
`dev_t`. Exploiting the gap requires root, on a static devtmpfs, against a
root-only knob. No action.

### BRIDGE-009 — first client monopolized the worker (medium, fixed v1.7)

**Defect (v1.6).** A single worker called `kernel_accept()`, then stayed in
blocking `kernel_recvmsg()` on the accepted socket until that client
disconnected. `state.client_sock` was therefore always NULL at accept time,
the replacement branch was dead, and an idle peer could hold the radio
indefinitely (TCP keepalive does not evict a live, idle peer).

**Fix (v1.7).** Accepting and client RX are split. The accept worker stays in
`kernel_accept()` while a client is active; each client gets its own TCP→UART
worker. On a new accept, the accept worker detaches the old client from shared
state, shuts its socket down, joins its worker synchronously (`kthread_stop`),
then publishes the new socket and worker. The client worker exclusively owns
and releases its socket; after a natural EOF it clears `client_sock`, switches
the LED off and stays dormant until joined, which closes the EOF-versus-
replacement window. At most one dormant client is retained. UART bytes
arriving during the short no-client window of a replacement are counted in
`drops_nocli`.

**Validated:** idle A→B on 7.1 (C5: A saw EOF 3 ms after B connected); active
A→B during EZSP traffic on 6.18 and 7.1 (C6), LED continuity (§4).

### BRIDGE-010 — hot `bind_addr` reconfiguration could keep the old exposure (medium, fixed v1.7)

**Defect (v1.6).** Relisten bound the new listener before shutting the old
one down. Moving from the default `0.0.0.0:8888` to `127.0.0.1:8888` — the
documented hardening transition — normally failed with `-EADDRINUSE`, the
setter rolled back, and the wildcard listener stayed up. A successful
reconfiguration also dropped the connected client, contrary to `DESIGN.md`.

**Fix (v1.7).** Relisten stops and releases the old accept worker/listener
before binding the replacement; the client socket/worker is left untouched. If
the new bind or worker creation fails, the old parameter is restored and the
old listener recreated; if that rollback fails too, the bridge fully disarms
rather than reporting `armed=1` without a listener.

**Validated:** C6 (§4).

### BRIDGE-011 — `client_ops` swaps did not exclude flip-buffer callbacks (low, fixed v1.7)

**Defect (v1.6).** Arm, unwind and disarm assigned `tty->port->client_ops`
under `tty_lock()`, but `tty_buffer.c` calls `client_ops->receive_buf()` under
the flip buffer's own exclusion. Both ops tables are static, so there was no
UAF; the risks were a formal data race and boundary byte loss or
misdirection.

**Fix (v1.7).** Arm takes `tty_buffer_lock_exclusive()`, installs the bridge
`client_ops` before opening the UART, and holds the exclusion until tty,
socket and acceptor state are published. Disarm removes socket visibility,
stops both workers, closes the UART, then restores the saved `client_ops`
under flip-buffer exclusion.

**Lock-order correction (C4).** The first v1.7 candidate took the exclusion
and then called `uart_close()`; the close path's `tty_buffer_flush()` takes the
same non-recursive `port->buf.lock`, and disarm deadlocked against itself
(`tty_buffer_flush` → `tty_ldisc_flush` → `tty_port_close_start` →
`tty_port_close` → `bridge_disarm_locked` → `param_set_enable`). The corrected
v1.7 runs close with both flip-buffer exclusion and `bridge_lock` dropped, and
takes the exclusion solely around the `client_ops` restore. The partially-armed
error unwind uses the same order; the pre-open failure path also drops
`bridge_lock` around `tty_kclose()`.

**Validated:** C5 and C6 teardown cycles (§4). Lockdep/KCSAN were not enabled;
they would need a separate instrumented kernel and are not a functional gate.

### Simplification items

| ID | Change | Status |
|---|---|---|
| BRIDGE-S01 | Document the param-lock dependency (BRIDGE-001) | implemented (v1.4) |
| BRIDGE-S02 | `pr_info_ratelimited` on connection-lifecycle messages (BRIDGE-004) | implemented (v1.4) |
| BRIDGE-S03 | Refresh `Kconfig` help and the `nrst_pulse_lock` comment (BRIDGE-003, BRIDGE-006) | implemented (v1.4) |

### Considered and rejected

- **Platform-driver conversion** to bind `/radio-bridge` properly (would also
  fix BRIDGE-007): churn for no functional gain, and it would turn BRIDGE-001's
  latent race into a live one.
- **Persistent gpiod descriptors**: claim-per-pulse keeps the lines free for
  other consumers and makes `nrst_gpio` changes stateless.
- **`tty_dev_name_to_number()`** instead of `kern_path()`: the parameter is a
  path; name lookup would drop symlink semantics.
- **Spinlock or lock-free hot path**: measured mutex cost ≈ 8 µs per
  `receive_buf` at 892 857 baud, ~2.5 % worker CPU (`DESIGN.md`).
- **`sk_data_ready` callbacks** instead of blocking workers: would need its own
  queue and flush discipline for no measured benefit.
- **Table-driven param boilerplate**: each setter's rollback differs (re-arm,
  re-termios, re-listen); a generic helper would obscure more than it saves.
- Previously rejected in `DESIGN.md` and still valid: line discipline hook,
  multi-client fan-out, netlink control plane, IRAM placement of the hot path.

### Performance (A2)

- UART→TCP is non-blocking (`MSG_DONTWAIT`) and accounts partial/error drops.
  Holding `bridge_lock` over one send is acceptable on this single-core target;
  stats/config readers share that latency.
- TCP→UART reads are bounded to 512 bytes; write retries are bounded by the
  chunk length plus four no-progress sleeps. No unbounded allocation or queue;
  `drops_tx` is the operational signal for a fast peer.
- The sw flow-control scan is linear per RX chunk; alternating XON/XOFF can
  fragment sends, but that input comes from the trusted radio firmware and is
  bounded by the chunk size.

## 4. Target validation of v1.7 (C5, C6)

Bench gateway, NCP-UART 7.5.1 firmware at 460800 baud with hardware flow
control. C6 traffic was generated by `universal-silabs-flasher`/Bellows:
protocol-valid, read-only EZSP; no radio configuration was changed.

- **BRIDGE-009.** On each kernel, client A started an EZSP probe and B
  connected 350 ms later: A was evicted, B completed and detected
  `ApplicationType.EZSP` 7.5.1.0 (7.1 counters `rx=773 tx=931`, all drop
  counters 0). STATUS LED brightness read 255 with A, 255 after B replaced A,
  0 after B closed.
- **BRIDGE-010.** With client A open on each kernel: `0.0.0.0:8888` →
  `127.0.0.1:8888` → `0.0.0.0:8888`, port 8888 → 8899, a forced failed change to
  the occupied port 22, then restoration to 8888. After the forced conflict the
  write failed, `armed` stayed 1, the parameter read back 8899 with exactly one
  8899 listener; the driver then restored exactly one `0.0.0.0:8888` listener.
  A saw no EOF or reset throughout.
- **BRIDGE-011.** Ten disarm/arm cycles per kernel during an active EZSP
  handshake (samples `rx=115 tx=125` before teardown), each `armed=0` → `1`
  with NCP recovery; `drops_nocli=7` (expected teardown window),
  `drops_err=0`, `drops_tx=0`. Linux 7.1 additionally ran five teardowns under
  a sustained Bellows `get_board_info` loop (baseline 71 calls in 3 s,
  `rx=5807 tx=4036`, zero drops; teardown samples 5156/3187, 4454/2750,
  4753/2925, 4990/3087, 4805/2963), each with zero error/TX drops and a final
  NCP 7.5.1 redetection. One load establishment timed out at `rx=112 tx=114`;
  no teardown was performed, a full probe recovered ASH and the replayed cycle
  gave the last sample above.
- Neither kernel logged a BUG, oops, hung/blocked task, call trace, deadlock or
  warning. On 6.18 the 8250 report ended with UART1 `tx=3472 rx=2766` and no
  framing/overrun flag (7.1 does not expose that proc report in this build).

A long-duration maximum-throughput soak was not repeated for v1.7; it is
optional performance evidence, distinct from these lifecycle tests.

## 5. Finding ID registry

| ID | Severity | Status | Summary |
|---|---|---|---|
| BRIDGE-001 | low (latent) | mitigated (v1.4) | transition atomicity relies on the global param lock; documented in-code at both sites |
| BRIDGE-002 | low | superseded by BRIDGE-009 | A1 inferred replace-on-connect from a branch that was unreachable until v1.7 |
| BRIDGE-003 | info | comment fixed (v1.4) | pulses hold the global param lock ~100 ms (nrst) / ~1.1 s (blmode) |
| BRIDGE-004 | low | fixed (v1.4) | connection-lifecycle messages ratelimited |
| BRIDGE-005 | info | open | kthread comm truncated to `rtl8196e-uart-b` |
| BRIDGE-006 | info | fixed (v1.4) | Kconfig help covers the blmode knobs; README size corrected |
| BRIDGE-007 | info | accepted | DT gpio controller phandle ignored, line number only |
| BRIDGE-008 | info | accepted | tty path→devt TOCTOU, root-only, negligible |
| BRIDGE-009 | medium | fixed (v1.7), target-validated | new client synchronously replaces the old one; one active client at a time |
| BRIDGE-010 | medium | fixed (v1.7), target-validated | conflict-safe relisten, client preserved, rollback to one valid listener |
| BRIDGE-011 | low | fixed (v1.7), target-validated | `client_ops` swaps quiesced against flip-buffer callbacks; teardown lock order corrected |
| BRIDGE-S01..S03 | — | implemented (v1.4) | see §3, simplification items |
