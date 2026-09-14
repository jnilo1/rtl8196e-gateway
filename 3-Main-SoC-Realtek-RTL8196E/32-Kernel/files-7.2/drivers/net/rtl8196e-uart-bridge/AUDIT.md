# rtl8196e-uart-bridge — cumulative driver audit

> **Cumulative audit ledger.** The opening table is the authoritative coverage
> statement. Pass A2 is the current end-to-end audit. The A1 text is preserved
> as history; where A2 explicitly corrects it (notably BRIDGE-002/009), A2 is
> authoritative.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `rtl8196e-uart-bridge` v1.7 |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | 2026-07-30 — C6, extended live-EZSP validation on Linux 6.18 + 7.1 |
| **Last fully audited baseline** | v1.6 |
| **Post-baseline changes** | v1.7 implements BRIDGE-009…011 and includes the close/flip-buffer self-deadlock correction found during candidate testing |
| **Validation state** | complete `W=1` links plus BRIDGE-009…011 target gates pass; concurrent EZSP replacement and live-traffic teardown pass on both kernels, sustained read-only EZSP teardown passes on 7.1 |
| **Maintained kernels** | Linux 6.18 and 7.1 |
| **Current finding registry** | §4, consolidated through A2 |
| **Scope** | driver, Kconfig/Makefile, tty/socket/GPIO concurrency and exposed TCP/sysfs surfaces |
| **Companions** | `DESIGN.md`, `README.md`, `SECURITY.md` |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | 2026-06-12 | v1.3 → v1.4 | BRIDGE-001…008 reviewed; S01…S03 implemented | build plus .88 connect/disconnect bench |
| C1 | 2026-06-13 | v1.4 → v1.5 | flow-control capability/firmware-mode split | change-local review |
| C2 | 2026-07-16 | v1.5 → v1.6 | truthful `blmode_pulse` sequence logging | change-local review |
| A2 | 2026-07-30 | v1.6 | independent full security/performance re-audit; BRIDGE-002 corrected, BRIDGE-009…011 opened | targeted `W=1` builds on 6.18 + 7.1; static checks |
| C3 | 2026-07-30 | v1.6 → v1.7 | BRIDGE-009…011 implemented | objects + complete `vmlinux` links with `W=1` on 6.18 + 7.1; target regression pending |
| C4 | 2026-07-30 | v1.7 candidate → corrected v1.7 | fix target-discovered disarm/error-unwind self-deadlock | static lock-order review + complete `vmlinux` links with `W=1` on 6.18 + 7.1; target retest was pending (see C5) |
| C5 | 2026-07-30 | corrected v1.7 | target regression on `.88` at 460800 baud | 6.18 stop/start; 7.1 ten-cycle teardown, idle A→B replacement and dormant-client teardown; no critical kernel diagnostic |
| C6 | 2026-07-30 | corrected v1.7 | close remaining target gates with protocol-valid EZSP traffic | BRIDGE-009/010 pass on 6.18 + 7.1; ten live-traffic teardowns per kernel; five sustained read-only EZSP teardowns on 7.1 |

## A2 — independent v1.6 re-audit (2026-07-30)

### Method and coverage

The v1.6 implementation was read and analysed in full **before** opening this
file. The pre-existing audit was frozen only by SHA-256
(`d26e9234ed9a020d5667cd2d0de5fbfba7872f087c0cdc2faa3f575b843ff40a`
in both maintained overlays); its contents were consulted only after the
independent findings below had been established.

Independence limitation: the auditor had previously seen this file's header
and a few excerpts during the repository-wide audit-header harmonization. It
was therefore not a perfectly blind review. No existing finding text was
re-read during A2 source analysis, and the A2 findings were frozen before the
full comparison above.

Coverage:

- complete source, Kconfig and Makefile review for Linux 6.18 and 7.1;
- socket, tty/flip-buffer, kthread, module-parameter, GPIO and LED lifecycles;
- device-tree and `S50uart_bridge` integration;
- comparison with the exact 6.18/7.1 kernel APIs. The two driver variants
  differ only in the 7.1 `kernel_bind()` cast to `struct sockaddr_unsized`;
- targeted object builds with `W=1`, successful and warning-free for both
  kernels;
- `checkpatch.pl --strict`: identical style debt in both variants
  (1 error, 22 warnings, 10 checks), with no security/correctness diagnostic.
  The error is the redundant explicit `false` initializer; the rest is comment,
  alignment and string-layout style;
- `sparse` and `smatch` were not installed. No runtime/network test was run in
  A2, so the three new findings require a target regression test when fixed.

### Current verdict

No memory corruption, UAF or double release was found. The socket shutdown and
ownership protocol, bounded XOFF wait, parameter validation, 64-bit counter
locking and GPIO error unwinds remain sound.

The main security boundary remains deployment: the default
`0.0.0.0:8888` endpoint is plaintext and unauthenticated, and gives a reachable
peer raw access to the radio. A2 additionally finds that the availability
semantics are less robust than A1 claimed: the first accepted client owns the
only worker until it disconnects, so an idle peer can deny service
indefinitely. Binding to loopback and using an authenticated SSH tunnel remains
the recommended mitigation.

### BRIDGE-009 — first client monopolizes the worker; replace-on-connect is unreachable (medium)

Pass A1/BRIDGE-002 misread the presence of the
`if (state.client_sock)` branch as proof of replace-on-connect. There is only
one worker:

1. it calls `kernel_accept()`;
2. after accepting, it remains in blocking `kernel_recvmsg(newsock, ...)`;
3. it returns to `kernel_accept()` only after that client disconnects.

Consequently `state.client_sock` is NULL at every normal accept installation:
the preceding session's cleanup clears it before the outer loop repeats. No
second thread accepts concurrently, so the replacement branch and its log are
dead in the current design.

Impact:

- the first peer can keep the connection idle indefinitely and deny the
  legitimate Z2M/cpcd client access;
- TCP keepalive is merely enabled with system defaults; it does not evict a
  live, deliberately idle peer;
- later connection handshakes may sit in the backlog, but they are not accepted
  and cannot replace the owner;
- `DESIGN.md` and `SECURITY.md` currently claim immediate eviction and must be
  corrected. Their earlier wording before A1 ("one client until disconnect")
  was closer to the implementation.

This does not expand the confidentiality/integrity boundary — any reachable
peer already has raw radio access — but it adds an easy persistent availability
attack. Recommended direction: first choose and document the intended policy.
For replace-on-connect, split accepting from client RX (or poll both sockets)
and transfer socket ownership explicitly. For first-client-wins, remove the
dead replacement path, document the DoS property, and consider configurable
idle/liveness enforcement that does not break legitimately quiet radio
sessions.

### BRIDGE-010 — hot `bind_addr` reconfiguration can retain the old exposure (medium)

`param_set_bind()` writes the new address, then
`bridge_reconfig_listen_locked()` creates and binds the replacement listener
**before** shutting down the old one. With the default wildcard listener
already bound to `0.0.0.0:8888`, creating `127.0.0.1:8888` normally conflicts
without `SO_REUSEPORT` and returns `-EADDRINUSE`. The setter rolls the string
back and leaves the old wildcard listener operational.

This matters because changing from wildcard to loopback is the documented
security-hardening transition. The persistent init-script route remains safe
when it disarms first (`S50uart_bridge restart`), and setting `bind_addr` before
the initial arm is safe. A direct write while armed, however, must not be
assumed to have narrowed exposure; callers must check the write result and
readback.

The same helper always captures, shuts down and releases `client_sock` on a
successful port/bind reconfiguration, contrary to `DESIGN.md`'s statement that
the connected client is preserved.

Recommended direction: create the new listener first only when the old and new
address/port tuples can coexist; otherwise perform a controlled stop-old,
bind-new transition with rollback that recreates the old listener. Add a
target test for `0.0.0.0 -> 127.0.0.1` while armed, verify the listening
address, and explicitly test/document client-disconnect semantics.

### BRIDGE-011 — `client_ops` swaps do not exclude flip-buffer callbacks (low)

Arm, unwind and disarm assign `tty->port->client_ops` under `tty_lock()`.
However, `tty_buffer.c` invokes `port->client_ops->receive_buf()` under the
flip-buffer's own exclusion; it does not take `tty_lock()`. The pointer load
and the driver's stores therefore have no common synchronization.

The current callbacks and their ops tables are static for the kernel lifetime,
so A2 found no UAF: a stale bridge callback after disarm sees
`state.client_sock == NULL`, while a stale default callback during arm can at
worst send transitional bytes to the ldisc. The remaining risks are a formal
data race, boundary byte loss/misdirection, and fragile assumptions if either
callback ever gains non-static state.

Recommended direction: use `tty_buffer_lock_exclusive()` /
`tty_buffer_unlock_exclusive()` around the ownership transition, with careful
lock ordering. Do not simply acquire it while holding `bridge_lock` on an
error/disarm path: an already-dispatched bridge callback may be waiting for
that mutex. A regression test should exercise repeated arm/disarm while UART
RX is active and check KCSAN/lockdep on a capable build.

### A2 performance assessment

- UART→TCP remains intentionally non-blocking (`MSG_DONTWAIT`) and accounts
  partial/error drops. Holding `bridge_lock` over one send is acceptable on
  the measured single-core target, but means stats/config readers share this
  hot-path latency.
- TCP→UART reads are bounded to 512 bytes and write retry count is bounded by
  the chunk length plus four no-progress sleeps. There is no unbounded
  allocation or queue. A fast peer can still drive short-write/drop activity;
  `drops_tx` is the operational signal.
- Software flow control scans each RX chunk linearly. Alternating XON/XOFF
  bytes can fragment it into many small nonblocking sends, but that input is
  radio-side/trusted firmware and remains bounded by the chunk size.
- No optimization change is justified before BRIDGE-009…011 correctness work.
  The prior 892857-baud soak measurements remain useful evidence but were not
  repeated by A2.

## C3 — v1.7 implementation of BRIDGE-009…011 (2026-07-30)

### BRIDGE-009 — fixed: replace-on-connect now executes

The worker roles are split:

- the accept worker remains blocked in `kernel_accept()` even while a client is
  active;
- each accepted client has a dedicated TCP→UART worker blocked in
  `kernel_recvmsg()`;
- on a new accept, the accept worker removes the old client from shared state,
  shuts its socket down, synchronously joins its worker, then publishes the new
  socket and worker as one locked transition.

The client worker exclusively owns and releases its socket. After a natural
EOF it clears only `client_sock`, switches the LED off, and stays dormant until
the accept worker or disarm joins it; it releases the socket immediately before
returning from that joined exit. Keeping both task and socket alive closes the
EOF-versus-replacement window where a captured socket could otherwise be freed
just before `kernel_sock_shutdown()`. At most one dormant client task/socket is
retained. During replacement there is a short intentional no-client window;
UART bytes received in it are counted in `drops_nocli`.

Result: a new client really evicts an idle or active predecessor, matching the
documented policy and closing the persistent first-client DoS. This does not
authenticate the endpoint; loopback plus SSH remains the security boundary.

### BRIDGE-010 — fixed: conflict-safe relisten with client preservation

Relisten now stops and releases the old accept worker/listener before binding
the replacement. The independent client socket/worker is left untouched, so a
successful port or bind-address change no longer drops the active radio
session. The wildcard-to-loopback hardening transition can therefore bind the
same port without colliding with the old wildcard listener.

If new bind or worker creation fails, the setter restores the old parameter and
recreates the old listener. If that rollback also fails, the bridge fully
disarms instead of reporting `armed=1` with no listener.

### BRIDGE-011 — fixed: callback ownership transition is quiesced

Arm takes `tty_buffer_lock_exclusive()`, installs the bridge `client_ops`
**before** opening the UART, and holds the exclusion until tty/socket/acceptor
state is fully published. Error paths close the opened UART before restoring
the saved table.

Disarm first removes socket visibility, stops both workers, then takes the
exclusive flip-buffer lock, closes the UART (stopping RX/write-wakeup activity),
restores the saved `client_ops`, and finally releases the tty. `bridge_lock` is
not held while waiting for flip-buffer exclusion, so an already-dispatched
receive callback can finish rather than deadlock.

### C3 validation and remaining gates

- 6.18 and 7.1 targeted objects and complete `vmlinux` links succeed with
  `W=1`;
- the maintained sources differ only by the required Linux 7.1
  `kernel_bind(..., struct sockaddr_unsized *, ...)` cast;
- `checkpatch.pl --strict` reports the pre-existing style class only
  (1 redundant-static-initializer error, 19 comment/string warnings, 8
  alignment/blank-line checks per variant); no new functional diagnostic;
- at C3 time no gateway had been flashed and no target runtime test had been
  performed; C5 records the later target campaign.

Before release, exercise:

1. client A active, then connect B; verify A is closed promptly, B transfers
   bidirectionally and the LED remains on;
2. idle A, then connect B; verify the same eviction without waiting for
   keepalive;
3. while a client exchanges traffic, change
   `0.0.0.0 -> 127.0.0.1 -> 0.0.0.0` and change the port; verify the client
   survives and the listening tuple changes;
4. force a failed rebind, verify automatic rollback, then reconnect;
5. repeat arm/disarm under UART RX load and verify no hang, oops, lockdep/KCSAN
   report or unexplained counter loss.

## C4 — v1.7 teardown lock-order correction (2026-07-30)

The first v1.7 target disarm blocked `S50uart_bridge` in uninterruptible sleep.
The captured stack was:

`tty_buffer_flush` → `tty_ldisc_flush` → `tty_port_close_start` →
`tty_port_close` → `bridge_disarm_locked` → `param_set_enable`.

v1.7 took `tty_buffer_lock_exclusive()` and then called `uart_close()`.
The close path calls `tty_buffer_flush()`, which tries to take the same
non-recursive `port->buf.lock`; the task therefore deadlocked against itself.
The post-open arm error path had the same latent ordering.

The corrected v1.7 runs close with both flip-buffer exclusion and `bridge_lock`
dropped. This lets an already-dispatched bridge RX callback finish and lets close flush
the buffer before shutting down and synchronizing the UART IRQ. Once close
returns, the corrected v1.7 takes flip-buffer exclusion solely around
restoration of `client_ops`. The partially armed error unwind follows the same ordering;
the pre-open failure path needs no UART close but also drops `bridge_lock`
around `tty_kclose()` so pending tty work cannot invert the locks.

The required security invariant for this deployment is limited to one active
TCP client at a time on port 8888. BRIDGE-009's last-connection-wins policy
meets that contract only if replacement is synchronous; authentication,
encryption and loopback-only binding are not release security requirements.
BRIDGE-010 remains a robustness property of the exposed live reconfiguration
interface.

## C5 — corrected v1.7 target regression (2026-07-30)

Both rebuilt Lidl images were flashed on the same `.88` gateway with an
NCP-UART firmware configured for 460800 baud and hardware flow control.

| Kernel | Image SHA-256 | Runtime result |
|---|---|---|
| 6.18.38 | `7d5e665082515c79f707e9ad74291ec2065e795c332c1776a51601927a7fc8ba` | booted as `6.18.38-rtl8196e-v4.0.0-rc5`; driver v1.7 armed on `ttyS1` at 460800; one `S50uart_bridge stop`/`start` reproducer completed with `armed=0` then `armed=1` |
| 7.1.3 | `8f67697147457cd304adb7cffd544d4692a5f2a5bbe725b36fbbd7a77c70f4c7` | booted as `7.1.3-rtl8196e-v4.0.0-rc5`; driver v1.7 armed at 460800; ten consecutive direct disarm/arm cycles completed, followed by a successful teardown/re-arm with a dormant client worker/socket |

The 7.1 single-active-client check connected idle client A and then client B
without sending any UART payload. A received EOF 3 ms after B connected; the
driver logged `replacing previous client` before publishing B. Closing B left
only the documented dormant socket until the final disarm joined it. After
that cleanup there was exactly one `0.0.0.0:8888` listener and no active
connection (the sole residual TCP table entry was A in `TIME_WAIT`).

Across both kernels the bridge returned to `enable=1`, `armed=1`, baud 460800,
with no `BUG`, oops, hung-task report, blocked-task report, call trace,
deadlock or warning in `dmesg`. These results validate the exact C4 deadlock
reproducer and the idle-client half of BRIDGE-009's one-active-client
invariant.

Still pending after C5 (completed by C6):

1. active A→B replacement while transferring bidirectional UART traffic,
   including LED continuity;
2. BRIDGE-010 live address/port reconfiguration, client preservation and
   failed-rebind rollback;
3. repeated arm/disarm while the UART RX path is carrying sustained traffic,
   with counter-loss and framing/overrun checks.

## C6 — extended live-EZSP target validation (2026-07-30)

The remaining gates were exercised with the NCP-UART 7.5.1 firmware at 460800
baud and hardware flow control. `universal-silabs-flasher`/Bellows generated
protocol-valid, read-only EZSP traffic; no arbitrary bytes were injected and
no radio configuration was changed.

### BRIDGE-009 — active replacement and LED continuity

On each maintained kernel, client A began an EZSP probe and client B connected
350 ms later while A was active. A failed after its socket was evicted; B
completed bidirectional communication and detected
`ApplicationType.EZSP`, version `7.5.1.0 build 0`. On 7.1 the bridge counters
after this scenario were `rx=773 tx=931`, with `drops_nocli=0`,
`drops_err=0` and `drops_tx=0`.

A separate replacement check on each kernel observed STATUS brightness 255
for A, 255 after B replaced A, and 0 after B closed. A received EOF. This
validates synchronous one-active-client replacement, bidirectional use by the
winner and LED continuity.

### BRIDGE-010 — live relisten, preservation and rollback

On both kernels, an open client A survived all of:

1. `0.0.0.0:8888` → `127.0.0.1:8888`;
2. `127.0.0.1:8888` → `0.0.0.0:8888`;
3. port 8888 → 8899;
4. a forced failed change to the already occupied port 22;
5. rollback on `0.0.0.0:8899`, followed by restoration to port 8888.

After the forced conflict the sysfs write returned failure, `armed` remained
1, the parameter read back as 8899 and exactly one 8899 listener existed.
The driver then restored exactly one `0.0.0.0:8888` listener. A received no
EOF or reset during any successful relisten or the rollback.

### BRIDGE-011 — teardown with live and sustained traffic

Each kernel completed ten disarm/arm cycles while an EZSP handshake was
actively exchanging bytes. Every cycle sampled `rx=115 tx=125` before
teardown, observed `armed=0` then `armed=1`, and was followed by successful
NCP recovery. The disarm records reached `tx=161`; the small
`drops_nocli=7` value is expected for radio bytes arriving in the intentional
no-client teardown window. `drops_err` and `drops_tx` stayed zero.

Linux 7.1 received an additional sustained test using one persistent Bellows
session repeatedly issuing read-only `get_board_info` commands:

- baseline: 71 calls in 3 seconds, `rx=5807 tx=4036`, zero drops;
- five teardowns at `rx/tx` samples of 5156/3187, 4454/2750, 4753/2925,
  4990/3087 and 4805/2963;
- every sustained cycle had `drops_err=0`, `drops_tx=0`, completed
  `armed=0→1`, and the final probe redetected NCP 7.5.1.

One attempted fifth load establishment initially timed out at only
`rx=112 tx=114`; the controller correctly performed no teardown, a full EZSP
probe recovered ASH, and the replayed fifth cycle produced the successful
4805/2963 result above.

Neither kernel logged a BUG, oops, hung/blocked task, call trace, deadlock or
warning. Linux 6.18's 8250 report ended with UART1 `tx=3472 rx=2766` and no
framing/overrun flag. Linux 7.1 does not expose that optional proc report in
this build. Lockdep/KCSAN were not enabled; running those sanitizers would
require a separate instrumented kernel and is not a remaining functional
release gate. A long-duration maximum-throughput soak remains optional
performance evidence, distinct from the lifecycle correctness tests above.

The gateway was finally restored to Linux 7.1.3 with `enable=1`, `armed=1`,
baud 460800, hardware flow control, one `0.0.0.0:8888` listener, no active
test connection and a successful final EZSP 7.5.1 probe.

First standalone audit of this driver (the existing `DESIGN.md` / `SECURITY.md`
pre-date it and remain the architecture and deployment references). Pass A1
was based solely on the v1.3 code; every claim below was verified against that
source, including the kernel-side `kernel/params.c` behaviour the driver
implicitly depends on (§1.2, BRIDGE-001).

Method: full read of the single source file, cross-checked against the 6.18
tty (`tty_kopen_exclusive`, `tty_port.client_ops`), socket (`kernel_*msg`,
`kernel_sock_shutdown`) and gpiod consumer APIs, plus `kernel/params.c`
(`param_attr_store` / `param_attr_show`) in `linux-6.18-rtl8196e/`.

---

## 1. Security review

### 1.1 Attack surface

| Surface | Who can reach it | Exposure |
|---|---|---|
| TCP listener (default `0.0.0.0:8888`) | any peer that can route to the gateway | **Unauthenticated, plaintext EZSP/CPC/Spinel session with the radio.** Deliberate, documented, mitigated by `BRIDGE_BIND=127.0.0.1` + SSH tunnel (`SECURITY.md`). A new connection **evicts** the connected client (see BRIDGE-002). |
| UART RX bytes (radio side) | EFR32 firmware (or whoever flashed it) | In `flow_control=sw`, bare 0x11/0x13 gate the TCP→UART direction — bounded to 1 s by the fail-open timer, so a hostile/wedged radio cannot park the worker or hang disarm. In hw/none modes, bytes are forwarded verbatim. |
| Module parameters (sysfs) | root only — all files are root-owned with modes 0600/0644/0444/0200; write bit is owner-only on every knob | Validated setters (§1.3). Side effect: pulse knobs hold the **global** built-in `param_lock` for their duration (BRIDGE-003). |
| Device tree `/radio-bridge` node | build-time (trusted) | Seeds `nrst_gpio` / `blmode_gpio` and the `realtek,hw-flow-control` capability (which sets the `flow_control` default) only; GPIO lines range-checked (`args[0] <= 31`). The capability is also a ceiling: an `hw` request is clamped to `sw` when the board lacks the boolean, so CRTSCTS is never asserted on an unwired UART. |

### 1.2 Verified correct

Concurrency and lifecycle — the historically dangerous part of this driver —
hold up under line-by-line review:

- **`client_sock` ownership discipline.** `bridge_send_to_client_locked()`
  never releases the socket on error; it only `kernel_sock_shutdown()`s it
  (state flip, no free) to wake the worker out of `kernel_recvmsg()`. The
  final `sock_release()` is owned by exactly one party: the worker's phase-2
  cleanup if `state.client_sock == newsock` still holds, otherwise whoever
  NULLed the pointer under `bridge_lock` (disarm, reconfig, or the
  replace-client path). No UAF, no double-free path found.
- **`stopping_worker` handshake.** A connection accepted inside a
  disarm/reconfig tear-down window is released immediately instead of being
  installed as state the outgoing teardown no longer knows about. The flag is
  re-checked after the replace-client path re-acquires `bridge_lock`, closing
  the window opened by the intermediate `sock_release(old)`.
- **`listen_sock` lifetime vs the lockless reader.** The worker reads
  `state.listen_sock` with `READ_ONCE()` outside any lock; disarm keeps the
  pointer populated until the synchronous `kthread_stop()` has returned, and
  only then NULLs it. `bridge_reconfig_listen_locked()` deliberately defers
  installing the new socket until the old worker is gone, for the same reason.
- **Bounded XOFF gate.** The sw-mode TX pause is a 1 s fail-open wait that
  also polls `kthread_should_stop()`, so a lost XON can neither stall the
  host→radio path indefinitely nor hang the disarm path's `kthread_stop()`.
  A network client cannot inject flow control at all: the XON/XOFF scan runs
  only on the UART→TCP direction, and the host→radio stream is never filtered.
- **nRST / blmode pulses.** Open-drain semantics throughout — the pad is
  driven low or floated, never driven high against the EFR32's RESETn
  pull-up. Both knobs are write-only mode 0200 (root), serialized by
  `nrst_pulse_lock`, and claim the lines per-pulse so they stay free between
  pulses. `blmode_pulse` validates `blmode_gpio >= 0` and
  `blmode_gpio != nrst_gpio`, and its error path releases the already-claimed
  blmode line before returning.
- **Input validation.** baud 1200–4 000 000; port 1–65535; gpio lines 0–31
  (blmode −1–31); brightness 0–255; `bind_addr` through `in4_pton()` before
  acceptance; `flow_control` parsed into a closed enum (numeric ABI preserved
  for `flash_efr32.sh` readback); all string params copied with `strscpy`
  into fixed buffers with explicit `-ENAMETOOLONG` on overflow.
- **No torn reads.** All `u64` counters are read and written under
  `bridge_lock` (32-bit MIPS has no atomic 64-bit loads); every getter takes
  the lock; the two lockless reads (`sw_tx_paused`, `listen_sock`,
  `rtl_flow_control` in the worker) are `READ_ONCE`/`WRITE_ONCE` pairs with
  documented staleness tolerance.
- **`kernel_getpeername()` into `struct sockaddr_in`** is safe on an AF_INET
  socket (inet writes exactly `sizeof(struct sockaddr_in)`).
- **Arm error unwind** restores `saved_client_ops` and closes the tty in the
  correct order under `tty_lock`; `tty_kopen_exclusive` guarantees no
  userspace open can race the kernel-side claim.

### 1.3 Standing risk — the unauthenticated TCP endpoint

Unchanged and deliberate: anyone who can reach the listen port owns the
radio. The complete threat model and the loopback+SSH mitigation live in
`SECURITY.md`. This audit's one correction to that model: eviction is
immediate (BRIDGE-002) — the attacker does not have to wait for the
legitimate client to disconnect. `SECURITY.md` has been amended accordingly.

### 1.4 Verdict

No remotely exploitable memory-safety flaw found. The socket/worker/tty
lifecycle is correct under the locking that actually executes (including the
implicit global param lock, see BRIDGE-001). The residual risks are the
documented unauthenticated endpoint (mitigated by deployment, §1.3) and the
low-severity items below.

---

## 2. Findings

### BRIDGE-001 — config-transition atomicity silently depends on the global kernel param lock (low, latent)

`bridge_disarm_locked()` and `bridge_reconfig_listen_locked()` drop
`bridge_lock` mid-operation (around `kernel_sock_shutdown` + synchronous
`kthread_stop()`), then re-acquire it to finish clearing state. Taken in
isolation, that window would let a concurrent `enable=1` (after a `tty`
change, or timed between `tty_kclose()` and the re-lock) arm a fresh
worker/tty/socket that the resuming disarm would then wipe —
`state.listen_sock` NULLed under a live worker means a NULL dereference in
`kernel_accept()`, plus an orphaned kthread and leaked socket/tty refs.

**Why it is not reachable today:** every sysfs write to a built-in module's
parameters runs inside `param_attr_store()` → `kernel_param_lock(NULL)` →
the **global** `param_lock` mutex (`kernel/params.c`; verified in this tree).
Getters serialize the same way. So no two setters of this driver can ever
interleave, and the mid-disarm window is unobservable. Boot-time cmdline
parsing is single-threaded.

**Risk:** the invariant lives entirely outside the driver and is not recorded
in it. Any future non-param entry point that calls arm/disarm — a reboot
notifier, a platform-driver conversion, an ioctl — would reopen the race for
real.

**Fix direction (later):** a one-paragraph comment above
`bridge_disarm_locked()` naming the dependency; optionally an explicit
driver-level config mutex taken at the top of every setter as belt-and-braces
(zero hot-path cost — the hot path never touches it).

### BRIDGE-002 — replace-on-connect vs "refuses additional connects" in the docs (low, doc/threat-model — **docs corrected in this pass**)

The code replaces an existing client on every new accept: the worker releases
the old socket (`"replacing previous client"`) and installs the new one.
`DESIGN.md` ("Single-client listener") claimed the bridge *refuses* additional
connects until the first closes, and `SECURITY.md`'s threat model said an
attacker could impersonate the host "once the legitimate client disconnects".
Both understated the behaviour: any peer that can reach the port can evict
the connected client **at any time**.

The replace policy itself is sound — it is what lets a restarted Z2M/cpcd
reclaim the bridge without operator action, and an attacker who can connect
already owns the radio under this threat model, eviction or not. The defect
was documentation. Both files are corrected as of this audit; no code change
needed.

### BRIDGE-003 — pulse knobs hold the global built-in param_lock for their whole duration (info)

Because of the same `kernel/params.c` serialization, `nrst_pulse` holds the
global `param_lock` for ~100 ms and `blmode_pulse` for ~5.1 s. During that
time **every** sysfs parameter read/write of **every built-in module on the
system** blocks — including this driver's own `stats`/`armed` getters. The
in-code comment above `nrst_pulse_lock` ("it should not stall a concurrent
stats reader") is therefore inaccurate at the sysfs layer; only the UART→TCP
hot path (`bridge_port_receive_buf`, which goes nowhere near the param
machinery) is genuinely unaffected, and that is the claim that matters.

Operationally harmless (both knobs are rare, root-triggered maintenance
actions). Fix is a comment correction when the file is next touched; moving
the sleeps out of the setter is not worth the asynchrony it would buy.

### BRIDGE-004 — connection-lifecycle pr_info is not ratelimited (low)

`"client connected from %pI4"`, `"replacing previous client"` and
`"client disconnected"` are plain `pr_info()`. A LAN peer can flap
connections in a tight loop and churn the kmsg buffer / ramfs
`/var/log/messages` (3 MB rotation) at no cost, washing out diagnostics. The
error paths already use `pr_warn_ratelimited`; the connect/replace/disconnect
trio should follow when next touched.

### BRIDGE-005 — worker kthread name truncated (info)

`DRV_NAME "-worker"` = `rtl8196e-uart-bridge-worker` exceeds
`TASK_COMM_LEN`; `ps` shows `rtl8196e-uart-b`, indistinguishable from a
hypothetical sibling. Cosmetic.

### BRIDGE-006 — stale ancillary text (info)

- `Kconfig` help describes `nrst_pulse`/`nrst_gpio` but predates v1.3's
  `blmode_pulse`/`blmode_gpio`.
- `README.md` said "~1400 lines" for the source file (actual: 1 785 raw,
  1 098 pure LOC) — corrected in this pass.

### BRIDGE-007 — DT gpio phandle controller is ignored (info, accepted)

`bridge_seed_defaults_from_dt()` consumes only the line number of
`nrst-gpios`/`blmode-gpios`; the pulse paths always claim through the
`"gpio-rtl819x"` label regardless of which controller the DT cell named.
Irrelevant on this SoC (single gpiochip) and already documented in
`README.md` ("only the line numbers are consumed"). Recorded here so a
future multi-gpiochip port knows to revisit.

### BRIDGE-008 — tty path→devt TOCTOU (info, negligible)

`resolve_tty_devt()` resolves the path, then `tty_kopen_exclusive()` opens by
`dev_t`; the path could in principle be swapped in between. Requires root,
on a static devtmpfs, to attack a root-only knob. No action.

---

## 3. Simplification / 6.18 alignment

The driver is already shaped for 6.18 (`tty_kopen_exclusive`,
`tty_port.client_ops`, gpiod consumer API, `sock_set_*` helpers, LED trigger
API). Remaining items are small:

| ID | Change | Value |
|---|---|---|
| BRIDGE-S01 | Comment (or explicit config mutex) recording the param-lock dependency | Closes BRIDGE-001's latency before it bites a future refactor |
| BRIDGE-S02 | `pr_info_ratelimited` on the connection-lifecycle messages | Closes BRIDGE-004 |
| BRIDGE-S03 | Refresh `Kconfig` help (blmode knobs) and the inaccurate stats-reader comment | Closes BRIDGE-003/-006 leftovers |

### Considered and rejected (this audit)

- **Platform-driver conversion** to bind the `/radio-bridge` node properly
  (would also fix BRIDGE-007). Rejected — already evaluated in `DESIGN.md`;
  churn on a field-stable driver for zero functional gain, and it would turn
  BRIDGE-001's latent race into a live one unless S01 lands first.
- **Persistent gpiod descriptors** instead of claim-per-pulse. Rejected —
  claim-per-pulse keeps the lines free for other consumers and makes
  `nrst_gpio` changes stateless (`DESIGN.md` rationale stands).
- **`tty_dev_name_to_number()`** instead of `kern_path()`. Rejected — the
  parameter is a path; name-based lookup would silently drop symlink
  semantics for no robustness gain.
- **Spinlock or lock-free hot path.** Rejected — measured mutex cost ≈ 8 µs
  per `receive_buf` at 892 857 baud, ~2.5 % worker CPU (`DESIGN.md`,
  "Stability properties"). No headroom problem exists.
- **`sk->sk_data_ready` callbacks instead of the blocking worker.** Rejected —
  the single blocking kthread is the simplest correct shape for one client at
  a time; callback-driven TX would need its own queue and flush discipline
  for zero measured benefit.
- **Table-driven param boilerplate.** The eleven setters share a
  lock/validate/rollback pattern that could be factored, but each rollback is
  subtly different (re-arm, re-termios, re-listen); a generic helper would
  obscure more than it saves.
- Previously rejected in `DESIGN.md` and still valid: line discipline hook,
  multi-client fan-out, netlink control plane, IRAM placement of the hot
  path.

---

## 4. Finding ID registry

| ID | Severity | Status | Summary |
|---|---|---|---|
| BRIDGE-001 | low (latent) | mitigated (v1.4) | dependency now documented in-code at both sites; still latent by design |
| BRIDGE-002 | low | **superseded by A2 / BRIDGE-009** | A1 incorrectly inferred replace-on-connect from an unreachable branch |
| BRIDGE-003 | info | comment fixed (v1.4) | pulses hold global built-in param_lock 100 ms / ~1.1 s; in-code comment now accurate |
| BRIDGE-004 | low | fixed (v1.4) | connect/replace/disconnect now pr_info_ratelimited |
| BRIDGE-005 | info | open | worker kthread comm truncated to `rtl8196e-uart-b` |
| BRIDGE-006 | info | fixed (v1.4) | Kconfig help covers blmode knobs; README line count fixed earlier |
| BRIDGE-007 | info | accepted | DT gpio controller phandle ignored, line number only |
| BRIDGE-008 | info | accepted | tty path→devt TOCTOU, root-only, negligible |
| BRIDGE-009 | medium | **fixed and target-validated in v1.7 on 6.18 + 7.1** | active EZSP A is synchronously evicted; B completes bidirectional probe; LED remains on across replacement |
| BRIDGE-010 | medium | **fixed and target-validated in v1.7 on 6.18 + 7.1** | live address/port changes preserve A; occupied-port failure rolls back to one valid listener |
| BRIDGE-011 | low | **corrected and target-validated in v1.7 on 6.18 + 7.1** | ten live-traffic cycles per kernel plus five sustained 7.1 cycles pass without hang or data-path error |
| BRIDGE-S01..S03 | — | implemented (v1.4) | see §3 and the note in §5 |

---

## 5. Conclusion

The corrected v1.7 source closes all three findings opened by A2: replacement is now
architecturally reachable, relisten is conflict-safe and preserves the client,
and callback-table transitions are quiesced against the tty data paths without
calling close under the flip-buffer mutex. C5 validates the original teardown
deadlock reproducer on both maintained kernels and validates idle-client
replacement on 7.1. C6 closes the remaining functional gates on both kernels:
active bidirectional replacement, BRIDGE-010 live/rollback scenarios and
teardown under real EZSP traffic; 7.1 additionally passes sustained read-only
EZSP load. The deployment security contract is one active TCP client at a
time; broader transport authentication and confidentiality are explicitly
outside that contract.

**Implementation note (2026-06-12):** S01–S03 implemented as driver
**v1.4** on maintainer request: BRIDGE-001's param_lock dependency is now
documented at the `bridge_lock` definition and at the disarm
drop-and-retake site (the latent race stays latent *and* visible to the
next refactor); the four remote-triggerable connection-lifecycle
messages are `pr_info_ratelimited`; the inaccurate stats-reader claim in
the `nrst_pulse_lock` comment now states the param_lock blocking
behaviour (BRIDGE-003), and the Kconfig help covers the blmode knobs
(BRIDGE-006). Bench-verified on .88: armed at boot, three
connect/disconnect cycles logged normally.
