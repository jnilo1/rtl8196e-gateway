# Changelog — RTL8196E Platform

All notable changes to the RTL8196E firmware distribution are documented here.
A version covers the complete set of components: bootloader (31-), kernel (32-),
rootfs (33-), and userdata (34-).

---

## [4.4.0] - 2026-09-10

_A bootloader release. Interrupting the boot with `Esc` stops being a knack and becomes
reliable, the guides lead with the procedure that actually works, and the in-RAM syslog ring
grows to hold about two weeks instead of four days. No kernel, rootfs or radio code changed;
the four kernel images are rebuilt only to carry the new localversion._

### Bootloader — V3.0, reliable escape into download mode

`Esc` at boot was examined by a poll that gave up on the first character it found. Anything
that was not `Esc` — a line transient at reset, a stray keystroke, an echo from a terminal
that announces itself on connect — went into the peek slot, and every later poll then returned
immediately, because nothing consumes that slot until the monitor runs. One byte in front of
the user's key hid it for the rest of the boot, which is why the escape worked for some people
every time and for others almost never. `pollingDownModeKeyword()` now drains the receive FIFO
looking for the key, still keeping the first non-matching character for the monitor.

The periodic poll inside the image checksum was inert. Its counter advanced once per 64 KiB
scanned while the threshold was written in bytes, so firing it once would have taken 32 GiB of
image: in practice the whole escape rested on a single poll taken just before the jump to the
kernel. The interval is now expressed directly in bytes and the counter is gone, so the key is
sampled roughly every 64 KiB of image as the original code intended. The rootfs checksum loop
carried the same constant under a different unit and is corrected alongside, although it is
compiled out on this flash layout.

Under `RAMTEST_TRACE` the kernel jump is compiled out, so both outcomes end in download mode
and the key decision left no trace. The RAM-test build now reports it, which is what makes
this path testable without writing to flash. Validated on the bench in paired runs against a
control built from the previous code: with a stray byte ahead of a continuous `Esc` stream the
control never detected the key in two runs while the corrected build caught it in both, and
neither reported a key when nothing was sent. The flashed V3.0 bootloader was then confirmed
on the same board, stopping at the `<RealTek>` prompt on a normal reboot with four stray bytes
interleaved ahead of every `Esc`.

Reported in issue #159, where the escape had become impossible on a gateway whose rootfs
offered no other way back into the bootloader.

### Documentation — the boot-interrupt procedure

Both guides told the reader to press `Esc` repeatedly as soon as serial output began, which is
the fragile way to do it. They now lead with the procedure that is deterministic: open the
terminal, remove power, hold the key down, then restore power. Keyboard auto-repeat feeds the
line from the first moment the bootloader listens, so there is no window left to miss.

The troubleshooting entry also spells out the two power arrangements, since they must not be
mixed: with the official supply connected the serial adapter stays on three wires and pin 1
VCC is left disconnected, while a board with no official supply can be powered from the
adapter's 3.3 V and cycled on that wire. Connecting both drives the same rail from two sources.
Finally, it adds the test that separates a bootloader problem from a dead link: let Linux boot
and try typing at the serial console, because a readable banner only proves the direction from
the gateway to the host.

### Userdata — two weeks of syslog

`syslogd` rotated three files of 32 KB in RAM. On a busy border-router site the `otbr-agent`
warnings alone fill 32 KB in a little over a day, so the supervisor's "exited/died, restarting"
line was rotated out before anyone came to look: a restart on the production gateway left no
trace two and a half days later. The ring is now six files of 64 KB, which holds roughly two
weeks for 384 KB of RAM.

## [4.3.0] - 2026-09-03

_A rootfs and installer release. BusyBox moves to 1.38.0 on a patch stack that upstream has
shrunk from 24 patches to 19, and the host-side scripts stop half-finishing on machines
that cannot complete the job — both reported from the field within a day of each other. No
kernel, bootloader or radio code changed; the four kernel images are rebuilt only to carry
the new localversion._

### Rootfs — BusyBox 1.38.0

The BusyBox patch set tracks Alpine edge, so staleness is measured against aports rather
than against upstream announcements. A review on 2026-09-03 found real drift for the first
time: BusyBox 1.38.0 was released on 2026-05-13 and Alpine moved to it on 2026-05-19, four
weeks after this tree pinned 1.37.0 — which was what Alpine shipped at the time.

The patch stack drops from 24 to 19. Upstream 1.38.0 absorbed all three path-traversal CVE
backports carried with an `800-` prefix (CVE-2023-39810, CVE-2026-26157, CVE-2026-26158):
the `FEATURE_PATH_TRAVERSAL_PROTECTION` framework, `unsafe_prefix.c`, and the
`!S_ISLNK(...)` guard that keeps the strip on hard links only. It also absorbed four of the
seventeen Alpine patches. Alpine contributed two `ash` fixes that exist in no 1.37.0 tree:
a `bb_got_signal` clear for non-interactive shells, and a fix for an out-of-bounds read in
`ifsbreakup()` where `ifsfree()` is skipped because `argstr()` longjmps out of
`expandarg()`, leaving stale IFS split offsets that a later, shorter expansion trusts. The
four Lexra platform patches applied to 1.38.0 unchanged.

The applet set is identical to the 1.37.0 build — 103 applets, none lost, none gained —
and that took a decision rather than luck. `build_busybox.sh` resolves a new Kconfig with
`yes "" | make oldconfig`, which accepts every new symbol at its upstream default and only
writes the result back to `busybox.config` when the word "not set" appears in its output.
Here it did not write back, so the first 1.38.0 build silently gained five applets the
tracked configuration never mentioned: `ssl_server`, `lsblk`, `vmstat`, `uuidgen` and
`sha384sum`. All five are now explicitly off — `ssl_server` most of all, being
network-facing while its `ssl_client` counterpart is deliberately not shipped. Four other
symbols were resolved and recorded rather than inherited: `FEATURE_IP_ROUTE` and
`FEATURE_IP_NEIGH`, which 1.38.0 has the already-enabled `iproute` and `ipneigh` applets
`select`; `FEATURE_VERSION`, a new knob over the previously unconditional
`busybox --version`; and `USE_BB_CRYPT_YES`, which only adds verification of yescrypt
hashes and generates none, the default algorithm here being DES. `busybox.config` now
determines the build on its own: a second `oldconfig` pass changes nothing but a timestamp.

Binary text grows 758,297 to 770,516 bytes (+1.6 %). All 19 patches apply with zero fuzz.
Bench-validated on a Lidl gateway upgraded from v4.0.0: two clean boots, twelve supervised
services, no panic or oops in the kernel log and no error in syslog, the device applet set
matching the flashed skeleton, and the `/userdata` user additions preserved. Field
confirmation still pending; the `ash` over-read fix in particular is exercised but not
proven by a smoke test.

### Installer — refuse an unsupported host instead of failing half-way (#157)

The host-side scripts now check the operating system and the shell before doing anything,
and stop with a single actionable message when the host cannot complete the job. The
requirement is real rather than conservative: the userdata image is built with
`mkfs.jffs2`, from mtd-utils, which exists on Linux only, so a port would end at that wall
whatever else were made portable.

The failure this replaces was not a clean one. On macOS the scripts ran on bash 3.2 until
the first associative array — `lib/gwconf.sh`, in the state-file writer — and died there,
which on the flash path is after the gateway has been sent to its bootloader. Before that,
size checks written as `stat -c%s ... || echo 0` returned 0 for every partition, because
BSD `stat` rejects `-c` and the fallback swallows it: `backup_gateway.sh` reported four
empty partitions, then padded the concatenation with a full 16 MiB, so an unusable backup
looked like a completed one.

The guard lives in `lib/hostcheck.sh`, sourced by `lib/gwconf.sh` and by
`34-Userdata/build_userdata.sh`, which together cover every documented entry point. It is
written in bash 3 syntax on purpose — it has to run on the shell it rejects in order to
report anything — and its message names the reason and the ways out: another Linux machine
on the same network, a virtual machine bridged rather than NATed, or WSL2.
`scripts/test_hostcheck.sh` fakes the operating system through a `uname` stub, so it takes
the same branch a Mac would. The documented requirement is now stated in
`docs/getting-started.md`.

### Installer — find mkfs.jffs2 where the distribution puts it (#158)

`flash_install_rtl8196e.sh` reported `missing build/flash prerequisites: mtd-utils` on a
Debian host where `mtd-utils` was correctly installed. The package puts `mkfs.jffs2` in
`/usr/sbin`, and whether that directory is on a non-root user's PATH is distribution
policy: Ubuntu, the documented and tested build host, ships an `/etc/environment` whose
PATH carries the sbin directories for every user, while Debian leaves that file empty and
grants them to uid 0 only. The same gap opens on any distribution outside a login shell —
cron, systemd units, minimal containers.

The scripts now append `/usr/local/sbin`, `/usr/sbin` and `/sbin` to their own PATH when
those directories exist and are not already listed, rather than assuming the distribution
provided them. Appended and not prepended, so a tool placed earlier on PATH on purpose
keeps its precedence. The helper lives in `lib/hostpath.sh` and is sourced by the two
scripts that need it: `flash_install_rtl8196e.sh`, whose prerequisite check probes
`mkfs.jffs2`, and `34-Userdata/build_userdata.sh`, which runs it. `scripts/test_hostpath.sh`
covers appending, precedence, idempotence and the resolution of `mkfs.jffs2` from a
Debian-style non-root PATH. `mkfs.jffs2` is the only host tool in the flash path that lives
solely in an sbin directory; `ip` is not affected, iproute2 installing the real binary as
`/bin/ip`.

---

## [4.2.0] - 2026-08-22

_A kernel performance release: both supported lines move forward, and each now ships a
versioned I-MEM policy selected from an empty-window profile instead of the historical
hand-picked placement. Local holes preserve the surrounding text layout, production builds
verify the exact policy and reject runtime-patch sites in the boot-time SRAM copy, and the
standard release bench clears 82 Mbit/s TX on both kernels without sacrificing RX._

### Kernel — Linux 6.18.45 and 7.1.9

The production line moves from Linux 6.18.41 to 6.18.45, and the secondary line from 7.1.7
to 7.1.9. All 58 RTL8196E patches apply at zero offset, without fuzz or warnings; the patch
lint now treats any of those conditions as a build failure rather than accepting a plausible
but shifted port.

### I-MEM — measured, versioned policies in the normal build

The optimizer profiles a kernel with an empty 16 KiB I-MEM window, enumerates the complete
safe text-section universe, excludes every runtime-patch site, and solves one exact TX
knapsack. Selected functions move into SRAM while equal-size, equal-alignment local holes
retain their original text slots. This replaces both the old annotation list and the global
layout arena, whose measured TX cost was 3.85 Mbit/s.

Normal builds now detect the policy for the exact point release, restore pristine objects
before an incremental compile, apply the local holes after that compile, relink, and verify
the linked policy. The gate checks the exact entry order, 15,872-byte budget, hole roots and
I-MEM boundaries; a separate scanner rejects jump labels and every other enabled dynamic
text-patching mechanism in the SRAM window.

The qualified policies occupy 15,868 bytes: 93 sections on 6.18.45 and 87 on 7.1.9. The
final standard 11-run-per-direction release bench measured 82.8/91.7 Mbit/s TX/RX on
6.18.45; the 7.1.9 qualification measured 82.8/92.8 Mbit/s. Both had zero TCP
retransmissions, zero hard-counter failures and clean kernel logs. The 7.1.9 run predates
the now-versioned 80/90 fast-path rule and is therefore a retrospective operational
qualification, not a prospective causal estimate.

### Bench — bounded release qualification

The release suite uses eleven TX and eleven RX repetitions, so its medians are observed
runs. The harness now also enforces the documented 15-second quiet period after stopping
radio and userland services instead of waiting only one second. For future campaigns, a
structurally clean candidate reaching at least 80 Mbit/s TX
and 90 Mbit/s RX with all safety gates green completes the bounded fast path. The long
12-pair comparison remains available for marginal results, close policy comparisons or a
precise causal estimate; it is no longer a mandatory four-hour tax on a large-margin release
decision.

### Installer — preserve release-candidate versions (#156)

`flash_install_rtl8196e.sh` no longer truncates the running firmware version at the third
numeric component: a gateway reporting `v4.0.0-rc5` is now displayed as `v4.0.0-rc5`, not
`v4.0.0`. The parser preserves SemVer prerelease and build suffixes and exposes the major
component separately, so the only behavioural consumer — the v2-to-v3 `radio.conf`
migration — remains independent from the display form. Firmware version detection still
does not select or gate the flash path.

### Ethernet performance across releases

The Linux 6.18 production images for v3.10.0, v4.0.0 and v4.2.0 were remeasured on the
same Lidl gateway and host rig with the current standard release bench. Each image was
flashed afresh, followed by 45 seconds of boot settling; `S70otbr`, `S80netwatch` and
`S40button` were then stopped and checked absent, followed by another 15 seconds of quiet
time. No command was run on the gateway during a measurement. TCP figures are medians of
eleven independent 30-second sessions with a fresh client and server and a 10-second gap;
UDP figures are medians of three 20-second sessions.

| Release | Kernel | TCP RX host to gateway | TCP TX gateway to host | UDP RX at 100 Mbit/s offered | UDP TX unconstrained |
|---|---|---:|---:|---:|---:|
| v3.10.0 | 6.18.35 | 93.8 Mbit/s (93.2–94.0) | 68.7 Mbit/s (68.3–72.1) | 30.5 Mbit/s, 68% loss | 32.6 Mbit/s, 0% loss |
| v4.0.0 | 6.18.41 | 92.4 Mbit/s (91.8–92.6) | 71.4 Mbit/s (70.3–72.7) | 33.3 Mbit/s, 65% loss | 33.6 Mbit/s, 0% loss |
| v4.2.0 | 6.18.45 | 91.7 Mbit/s (91.1–92.5) | **82.8 Mbit/s** (81.2–83.6) | **40.7 Mbit/s**, 57% loss | **36.7 Mbit/s**, 0% loss |

From v3.10.0 to v4.2.0, median TCP TX rises by 14.1 Mbit/s (20.5%), UDP TX by
4.1 Mbit/s (12.6%), and delivered UDP RX by 10.2 Mbit/s (33.4%). TCP RX falls by
2.1 Mbit/s (2.2%); this progression includes the deliberate v2.23 checksum-integrity
policy cost documented in the Ethernet performance notes. All three runs recorded zero
TCP retransmissions and zero RX/TX errors. The 629-632 TCP-phase `rx_drop` increments were
effectively identical on all three images and correspond to ambient broadcast traffic,
not a release-dependent regression.

Only the kernel image changed between runs: the installed rootfs and userdata were held
constant, and every boot received the same repository-built static iperf 3.18 binary
(`9001f02e4245e316d08f830424c7e0168efb9b1f3f4b46a047f2217339ec85df`). The kernel-image
SHA-256 prefixes were `2488a974c7512ad6` (v3.10.0), `eda60783c8491256` (v4.0.0), and
`0cb480ec488e81d7` (v4.2.0). The progression is therefore a release-level observation of
the aggregate kernel, driver, layout and I-MEM changes; it is not a causal decomposition of
any single change.

---

## [4.1.0] - 2026-08-21

_A repair release, and its subject is the distance between what a source says and what a
binary does. Issue #155 — @hlyi's Sengled G4 rebooting straight back into Linux instead of
stopping in its bootloader — was a **`boothold` binary two months older than its own
source**: it still had the Lidl page address compiled in, so on a 64 MiB board it wrote the
boot-hold flag 32 MiB below where the bootloader reads it, verified its own write, and
returned success. The binary is rebuilt and validated on both boards. The two flash scripts
that hid the failure for two months, by discarding everything `boothold` printed, now
**report what the arming actually did** and stop the run when it refuses. A third defect
surfaced while testing that fix on genuinely old firmware: the board guard matched a model
string that only became board-specific in v3.10.0, so it refused every upgrade from v3.9.0
and earlier — the documented command, told to re-run with the `BOARD=` it had just been
given. It now **confirms the board from its DRAM bring-up registers**, which every firmware
generation exposes. No kernel change ships here: the kernel images are v4.0.0's, unchanged._

### `boothold` — the shipped binary predated its own source by two months (issue #155, @hlyi)

`boothold` learned to take its target address from the live device tree in June
(`reserved-memory/boothold@*`), so that a board relocating the page — the 64 MiB Sengled
E39-G8C moves it from `0x01FFE000` to `0x03FFE000` — moves the flag with its DTS. The
source did. **The committed binary never got rebuilt**: its last commit is the May IP-handoff
change, two months and one feature earlier. Disassembling what shipped leaves no doubt —
`lui a3,0x1ff` / `ori a3,a3,0xeffc` for the HOLD word and the same pair for the two IP words,
no `/sys/firmware` string anywhere, no directory walk. The address was still compiled in.

On the Lidl board the hardcoded value, the device tree and the bootloader's own constant all
say `0x01FFEFFC`, so nothing showed. On a 64 MiB board the write lands 32 MiB below the page
the bootloader reads: it goes into ordinary kernel RAM, the read-back verification passes,
`boothold` exits 0, the gateway reboots — and the bootloader, finding nothing at
`0x03FFEFFC`, boots Linux again. That is the whole of issue #155, and it made every
`flash_install_rtl8196e.sh` / `flash_remote.sh` upgrade impossible on that board while
looking, from the outside, like a bootloader that ignored the flag.

The binary is rebuilt from the current source with the toolchain that produced it
(crosstool-NG 1.28.0, gcc 15.2.0) and validated on the bench: it now prints the address it
resolved, `Boot hold set at 0x01FFEFFC.`, and the three words verify through `/dev/mem` —
HOLD `0x484F4C44`, marker `0x49505634`, and the packed server IP. No hardcoded page address
remains in it. The Sengled path follows from the same code reading that board's DTS, and is
for its owner to confirm.

The other prebuilt tools were screened for the same drift and are in sync: `linkwatch`,
`otbr-monitor`, `netwatch`, `keepalive` and `s40button` have no non-comment source change
since their binaries were committed, the four kernel images have no non-documentation commit
on their source trees since they were built, and the only bootloader-source commit after
`boot.bin` rewrites two host-tool paths in a Makefile.

### `flash_install_rtl8196e.sh` / `flash_remote.sh` — the boot-hold arming says what it did (issue #155, @hlyi)

Both scripts entered the bootloader with a single remote command,
`boothold "$BOOT_IP" && reboot`, run with its standard error sent to `/dev/null` and its
exit status discarded. That hid the one report that matters. `boothold` refuses to write
when the running kernel declares no `boothold` reserved-memory page, and it verifies its
own write by reading it back — both failures print a reason, and both were swallowed.
Worse, a refusal is not a reboot: the gateway stayed up on its current firmware while the
script went on to wait a minute for a bootloader that was never coming, and then blamed
the bootloader.

The arming and the reboot are now two commands. `boothold` runs on its own, its output —
including the address it wrote, which is per board — is echoed, and its exit status is
read: zero sends the `reboot`, a refusal stops the run there with nothing written and the
gateway untouched. A dropped connection is treated as before, since that is also what a
successful arm looks like on firmware older than v3.2.0, where `boothold` was a shell
script that rebooted the gateway itself.

The failure message at the end of the wait now separates the two ways this fails. If SSH
answers again, the gateway rebooted straight back into Linux: the flag *was* written and
verified, so the bootloader on the flash did not look where the running kernel wrote. That
page is a per-board contract — the kernel DTS reserves it, the bootloader reads a constant
compiled from `BOARD_DRAM_TOP_KSEG1 - 0x2000`, and the two must name the same address
(`0x01FFEFFC` on the 32 MiB Lidl board, `0x03FFEFFC` on the 64 MiB Sengled E39-G8C). A
mismatched pair — a bootloader built for another board — fails exactly this way, silently
on both sides. The script now says so, and points at the bootloader banner, whose `RAM:`
figure identifies the board the installed bootloader was built for.

### Board guard — gateways older than per-board model strings can be upgraded again

The guard that keeps a Lidl image off a Sengled board, and the reverse, matched a substring
of `/proc/device-tree/model`: "Lidl", or "Sengled". The model only became board-specific when
the device tree started describing board wiring — v3.8.0, v3.8.3 and v3.9.0 all report the
generic `Realtek RTL8196E SoC`, and only v3.10.0 onwards names the board. So every gateway
that had not been updated since was refused by the documented upgrade command, and told to
"re-run with the matching `BOARD=`" — which is what it had just done. Only `--force` got
through, under a name that reads like disabling a brick check.

Widening the string was the wrong repair: the one string to add would be the generic SoC one,
and a Sengled G4 running a build from before its own device tree reports exactly that. It
would have reopened the mis-flash it exists to prevent.

The DDR controller answers where the string cannot. `btcode/start.S` writes `0x18001004` and
`0x18001008` at bring-up from `boards/<board>/board.h`, nothing rewrites them afterwards, and
they are readable from Linux with `devmem` on every firmware generation. The guard now falls
back to them whenever the model names no board it knows: matching the selected board's pair
confirms and proceeds, matching another board's pair refuses and names that board, and only
a gateway that answers neither is turned away — with a message that says so instead of
blaming the `BOARD=` the user got right. The pairs are read out of `board.h` at call time,
never copied into the library: adding a board means adding its `board.h`.

The two copies of the check, one per entry point, are now a single `board_guard_check` in
`lib/kernel_image.sh`, which already owned the board list. It returns three states — confirmed,
positive mismatch, inconclusive — so each caller keeps its own policy on the last one.
`flash_remote.sh` changes behaviour in one narrow way: it used to skip the check when the model
could not be read at all, and now asks the registers first, so such a gateway is confirmed
rather than waved through, and refused if they identify nothing either.

Verified on the bench across a real downgrade and upgrade: on v3.5.1 (`Realtek RTL8196E SoC`,
bootloader V2.6) `BOARD=lidl` is confirmed by `0x54480000` / `0x90E36920` and the upgrade to
v4.0.0 runs through with no `--force`, while `BOARD=sengled-e39-g8c` is refused on the same
evidence. The registers read identically under V2.6 and V2.9, so the discriminator does not
depend on the bootloader version.

---

## [4.0.0] - 2026-08-10

_The release the whole 4.0.0-rc series was working towards, and it turned into
more than a driver release. The **independent driver audits** land in full
(Ethernet, UART1, IRQ/GPIO, watchdog, LED-PWM, UART bridge), the **TC0
clockevent** stops being re-armed the way that could wedge it, the LAN LED
finally goes **truly off**, and the UART bridge gains a real
**client-replacement lifecycle** instead of an advertised one that was dead
code. Around them: `otbr-agent` learns **software flow control** for boards
without RTS/CTS (#134), the Thread BorderAgent identifies itself **per board**
(#144), and the third-party stack moves up — **Dropbear 2026.94**, **OTBR
v2026.07.0**, **Linux 6.18.41**. The three code bodies every packet passes
through — the checksum routine and the two assembly copy cores — **move into
on-chip SRAM**, each priced by its own paired series at 1 to 2 Mbit/s in both
directions. The shipped image benches at **70.7 Mbit/s TX and 92.8 Mbit/s RX**
on the 6.18 line, where the same suite read 68.5 / 89.5 before.

One new capability stays off until asked for: **netwatch**, which reboots and —
above all — records a gateway isolated on a live link. The gateway also stops
being addressed by a hardcoded `192.168.1.88`: **addresses are resolved**, from
your own network if nothing else says otherwise. And the project finally says what it is —
**renamed** `rtl8196e-gateway`, with **five new user guides** replacing a
reference that was organised by subsystem rather than by what a reader is
trying to do._

<!-- New capabilities -->

### New: `netwatch` — recover, and above all record, a gateway isolated on a live link

The hardware watchdog armed by `S25watchdog` only catches a stopped CPU: the
feeder is a userspace process, so a kernel hang stops the kicks and the chip
resets the board inside the ~60 s timeout. It is blind to the other way a
gateway disappears — userspace alive, network path dead. The kicks keep coming,
the chip stays quiet, and the box is simply gone from the LAN until a human
power-cycles it.

That blindness costs more than the outage. The power cycle is itself the
evidence-destroying step: it clears the watchdog reset-reason latch and wipes
the reserved DRAM page holding any panic record, so `S26panicrec` — which only
persists a record after a *successful* boot — never gets the chance. A remote
installation therefore yields an outage of unknown duration and no cause at
all. That is exactly what happened to a field gateway on 2026-07-31, and why
this exists.

`netwatch` (34-Userdata/netwatch, C, static, ~138 KB) probes targets with ICMP
echo from the only vantage point that still works in that state — the box
itself. After a long continuous failure window **with the link carrier still
up**, it writes a snapshot into `/userdata/netwatch/incidents.log` and reboots.
The snapshot is the point; the reboot is the by-product. It lands on JFFS2, so
it survives the reboot, a later power cycle, and everything else: reason,
uptime, carrier, `operstate`, `/proc/net/dev` counters, routes, the ARP table,
and the last 8 KB of the kernel ring — which is where the driver's own
`last reset:` line and any TX-timeout or switch-reset messages live.

Three decisions carry the design, each because this box may be physically
unreachable for weeks:

- **Carrier must be up.** Carrier down means the cable is out or the peer
  switch is off — a physical fault no reboot fixes, and one a human may have
  caused deliberately. Carrier up with nothing reachable is the signature
  wanted: the link is electrically fine and the stack is not answering.
- **The reboot goes through `/sbin/reboot`, never `reboot(2)`.** BusyBox init
  runs the `::shutdown` entry, reaching `S10network stop` and its
  `ip link set eth0 down`; that calls `ndo_stop`, which clears `TXCMD|RXCMD` in
  CPUICR and deasserts TRXRDY — the CPU-port DMA engine stops. Worth stating
  precisely, because it is easy to claim more than it does: that is **one** of
  the three steps the bootloader performs before its own resets (V2.9 also
  hard-resets the switch core — the step that aborts an *already-armed*
  transfer — and holds the five PHY interfaces off). The driver does neither of
  those, and `realtek_machine_restart()` adds nothing: it disables IRQs and
  writes the watchdog register, and the platform driver has no `.shutdown` hook
  for `device_shutdown()` to call. So this is not a full quiesce. It is strictly
  more than `reboot(2)` would leave — a board reset with the DMA engine still
  armed — which is exactly the state a watchdog timeout on a hung CPU produces.

  Note also what the V2.9 entry actually attributes the safety of an ordinary
  reboot to: *"a plain reboot never tripped it because the link is idle at that
  point"* — not to a quiesce. The documented boot loop was only ever reproduced
  in the bootloader's own post-TFTP path, under a large transfer. That a
  watchdog timeout can corrupt early boot the same way is a plausible mechanism
  on this platform, not a demonstrated one.
- **A reboot budget, persisted in `/userdata`, that fails closed.** Default 3
  per 24 h. If the budget file cannot be written the daemon refuses to reboot,
  because an unbounded loop on unreachable hardware would be worse than the
  outage it is trying to fix.

Written in C like the other long-lived daemons here, though on its own merits
rather than the old ash-is-unsafe rationale — that one died with the issue #109
root cause (a TLB sweep starting at a hardcoded index instead of the `Wired`
boundary, fixed in `fac8bbf`). The work is a raw ICMP socket, `klogctl()` and a
structured snapshot writer; none of it is a shell operation, and a script would
fork `ping` and `dmesg` every cycle forever on a single 400 MHz core.
Started by `S80netwatch` under `keepalive`, after every service
it might describe and before `S90checkpasswd`, tunable through the optional
`/userdata/etc/netwatch.conf` and **disabled with a single `ENABLED=0`**.

Bench-validated on `.88` across six cases: no false trigger against a live
target; detection and snapshot in dry run; carrier gating (unreadable carrier
never acts); fail-closed when the state directory is unwritable; refusal once
the budget is spent; and one real end-to-end reboot — box down 20 s, incident
and budget both intact afterwards, and the automatic restart at boot correctly
refusing the second reboot, so the anti-loop guard is proven across the very
reboot it bounds.

Two things the bench corrected. The failure window originally counted probe
rounds, which silently stretched it (each round also carries the probe wait, so
a nominal 60 s ran ~90 s); it now measures elapsed time from `/proc/uptime`.
And the reset-reason latch turns out to carry no information at all: the
driver reports `watchdog timeout` only when `WDIND` is set, and that bit does
not survive the reset it is meant to flag. Both overflow paths were checked on
the bench — the short window armed by the panic notifier, and a natural
`OVSEL=9` overflow after the feeder was killed outright — and each came back
reading `power-on / pin reset (WDTCNR=0xa5000000)`. A `netwatch` reboot, a
power cycle and a genuine watchdog bite are therefore indistinguishable from
that line. The panic record is the only positive evidence of a watchdog
recovery; its absence proves nothing either way.

Not enabled anywhere by default yet: shipped as an opt-in awaiting a field
decision.

<!-- Naming and documentation -->

### Documentation — five user guides replace a subsystem-first reference
The tree documented itself the way it is built: everything a reader needed
existed, but in the README of whichever subsystem produced it. That serves
someone who already knows where their problem lives, and fails everyone else —
a first installation spanned four directories, and nothing said in which order
to read them.

`docs/` now carries the path a user actually walks, and all five pages are new
in this release: first installation, choosing a radio mode, upgrading, using
and maintaining the gateway, and troubleshooting. The MkDocs navigation leads
with them. The subsystem READMEs keep their place as the reference layer
behind, no longer as the entry point.

### The project is `rtl8196e-gateway`, and no longer calls itself Zigbee-only
The repository, the documentation site and the build scripts described a
single-board, Zigbee-only firmware for one Lidl product. It has supported a
second board since 4.0.0-pre, and three radio roles — Zigbee, Thread, and a
standalone router — for longer than that. The name was telling new readers
something untrue about what they were about to install.

Renamed throughout: the project, the CI workflows, the build environment, and
the per-subsystem READMEs. The default hostname follows the same move, above.

Two names were deliberately left alone. The `2-Zigbee-Radio-Silabs-EFR32/`
directory and the navigation section it carries are published URLs; breaking
them to satisfy a naming rule would cost readers more than the inconsistency
does. Historical entries further down this file keep the old name too — they
describe what was true when they were written.

### Changed: the default hostname is `rtl8196e-gw`, not `zigbeegw`

The firmware runs three roles — Zigbee coordinator, Thread Border Router,
standalone router — on two boards, and the default hostname claimed only the
first of them. A border router announced itself on the LAN as a Zigbee gateway:
`zigbeegw login:` on the console, `zigbeegw` in `uname -a`, and `zigbeegw` sent
as DHCP option 12, which is the name the router's DNS then serves to everyone
else. The new default says what the box is — an RTL8196E gateway — and leaves
the role to the role.

**Only fresh installs are affected.** `etc/hostname` is in the preserved set of
both `flash_install_rtl8196e.sh` and `flash_remote.sh`, so an existing gateway
keeps whatever name it has across an upgrade, including `zigbeegw`. The new
value ships with a first install or a full-flash — and there, the DNS name of
the box changes, so anything addressing it by name (a Zigbee2MQTT adapter path,
a bookmark, a firewall rule) needs the new one. To keep the old name:

```sh
echo zigbeegw > /userdata/etc/hostname && reboot
```

The hostname has always been a plain writable file on the JFFS2 partition; any
other value works just as well.

### Login banner is now board- and role-neutral (`etc/version`, `etc/motd`)

`etc/version` and `etc/motd` greeted every board as a "Lidl Zigbee Gateway" —
wrong on a ported board (e.g. the Sengled G4), and "Zigbee" is wrong whenever
the box runs as a Thread Border Router or a standalone router. Both now read
"RTL8196E Gateway": the RTL8196E SoC is common to every supported board and the
name carries no role, so the banner is accurate in every configuration. The
board-specific identity a client actually consumes is advertised over mDNS by
`S70otbr` (see below). The project name ("Lidl Silvercrest Gateway", in
docs/repo) is unchanged — this is only the on-device login banner.

<!-- Addressing and first-run configuration -->

### Changed: gateway addresses are resolved from the host's network, not hardcoded

`192.168.1.88` and `192.168.1.6` were the maintainer's addresses. They were
baked into every host-side script as the default, and into the prompts that
build an image — so a user on `192.168.0.0/24` or `10.0.0.0/8` was offered an
address they could not reach, and had to override an environment variable on
every single command.

The addresses now come from `lib/gwconf.sh`, one resolver shared by the flash,
backup, restore and bench scripts. In order: the explicit argument, the script's
environment variable, `gateway.env` at the repository root (gitignored, template
in `gateway.env.example`), `.gateway-state` (gitignored — what the last install
wrote and the last gateway reached), a value derived from the LAN of the machine
running the script (its netmask, its default route, host part 88 for the gateway
and 6 for the bootloader), and only then the historic constants. On a
`192.168.1.0/24` LAN every value is unchanged.

Two consequences worth knowing:

- The address chosen at install time is written back, so a re-install proposes
  the same one and `backup_gateway.sh`, `flash_remote.sh`, `flash_efr32.sh` and
  the bench scripts find the gateway with no argument. `flash_remote.sh` no
  longer requires `LINUX_IP`. Whenever an address was not stated explicitly the
  scripts print it with its origin before acting, and `flash_remote.sh` compares
  the gateway's hostname against the recorded one — a remembered address can be
  handed to another machine by a DHCP server, and the next step reboots the
  target into its bootloader.
- Changing the gateway's address by hand — edit `/userdata/etc/eth0.conf`,
  reboot, reconnect — never reaches back to the machine running the flash
  scripts, so `.gateway-state` keeps pointing at the old address and the next
  argument-less command aims at a host that is no longer there. The failure was
  already safe and already named its source, but the only advice it offered was
  about cables and bootloaders. It now explains that the record may be stale and
  gives the three ways out. `docs/using-the-gateway.md` states the consequence
  where the reader learns the procedure.
- `flash_userdata.sh` no longer rewrites `docker-compose-otbr-host.yml` and
  `z2m/configuration.yaml` in place. Editing tracked files left the working tree
  dirty after every install. The compose file now reads `RCP_HOST` from a
  gitignored `.env` written next to it; the Zigbee2MQTT port is printed for the
  user to paste, as its documentation already instructs.

### Fixed: the bootloader address is only derived where boothold can hand it over

Deriving the bootloader's address from the host's LAN is right for the caller
that then tells the bootloader where to be — `boothold`, used by
`flash_remote.sh` and by the upgrade path of `flash_install_rtl8196e.sh`. It is
wrong everywhere else: a first installation, a restore, or a bare partition
flash finds a gateway already sitting at a bootloader prompt, reached by a cold
boot and a serial ESC. Nothing can move it, so it answers on the address
compiled into it — `192.168.1.6` — whatever LAN the host is on. The derivation
would have sent a first flash from a `192.168.0.0/24` machine looking for a
bootloader at `192.168.0.6`.

`gwconf_cold_boot_ip` now serves those callers and never derives;
`gwconf_boot_ip` keeps the derivation for the two that hand the address over.
`flash_install_rtl8196e.sh` has both paths, so it starts from the compiled
default and switches to the derived address only once it knows it is going
through boothold. A value stated by flag, environment or `gateway.env` still
wins everywhere — the address can be set by hand with `IPCONFIG` at the
bootloader prompt.

Found while auditing the documentation rather than by a test: the first-install
path is the one scenario the bench campaign deliberately skipped, because
rehearsing it wipes `/userdata`.

### Fixed: a failed DHCP lease no longer strands the gateway on the wrong subnet

When DHCP obtained no lease, `udhcpc.script` fell back to a hardcoded
`192.168.1.254/24` with a `192.168.1.1` route. On any other LAN that made the
gateway unreachable at precisely the moment the fallback exists to save it.

The fallback is now read from `/userdata/etc/eth0.bak`, which the flash scripts
generate in the subnet the gateway was actually installed on, high in the range
so it cannot collide with the address used in static mode. The hardcoded values
remain only as a last resort when that file is absent.

Running that branch for the first time also exposed a defect that had been
latent in it since it was written: the fallback never installed a default
route. BusyBox ignores the selector in `ip route show default` and prints the
whole table, so the guard saw the connected route added moments earlier,
concluded a default route already existed, and skipped its own. A gateway that
fell back was reachable from its own subnet but could reach nothing beyond it —
including the 8.8.8.8 the same branch writes into `resolv.conf`. The guard now
matches a real `default via ...` line. Verified on the bench: before the fix,
`ip route get 8.8.8.8` answered `Network unreachable`; after it, the box pings
8.8.8.8 from the fallback address.

The `bound` branch carried the same idiom, where it was harmless in effect but
dishonest in the log: it announced `Removing existing default routes` on every
single lease, because the test matched the connected route rather than a
default one. Fixed the same way, and the unconditional `Checking existing
default routes` line above it is gone — it announced an intention, not an
outcome. Exercising the branch afterwards showed the removal never runs at all:
`ip -4 addr flush` a few lines earlier already drops any default route that
pointed through this interface, so by the time the test executes there is
genuinely nothing to remove. The block is kept as a guard for a default route
reached through some other device; it simply no longer claims to have done
work it did not do. `S15hostname` recognises
the fallback by matching `eth0.bak` instead of comparing against a fixed
address, and `eth0.bak` keeps its second role: the file to copy over
`eth0.conf` to return to a static address.

<!-- Boot and logging behaviour -->

### Changed: no RSA host key — the first boot stops stalling for half a minute

`S30dropbear` generated three host keys on the first boot, and RSA alone
accounted for almost all of the wait. Measured on this SoC: ed25519 0.2 s,
ecdsa 0.3 s, **RSA-2048 32.4 s** — sixty-five times the other two together. The
function runs synchronously in the boot sequence, so that cost also delayed the
button daemon, the UART bridge and OTBR, while the network was already up: the
gateway answered ping but refused SSH for half a minute, which reads as a hang.

Dropbear runs without `-r` and without `-R`, so it serves exactly the keys that
exist and offers no other algorithm. ed25519 has been accepted by OpenSSH since
6.5 and PuTTY since 0.68, ecdsa by OpenSSH since 5.7, so any client of the last
decade connects. This is the host key only — logging in with an RSA *user* key
is unaffected.

Existing gateways are untouched: the function only creates what is missing and
`etc/dropbear` is preserved across upgrades, so a box that already has an RSA
key keeps serving it and nobody gets a host-key-changed warning. Anyone who
needs one for an old client can create it once, and the command is in the
script.

Verified by deleting the host keys on the bench and rebooting: only ecdsa and
ed25519 are generated, and `ssh-keyscan` confirms the server no longer offers
`ssh-rsa`.

### Changed: the boot log no longer goes silent on services that do nothing

`S26panicrec` printed nothing on the path it takes almost
every boot — no panic record to persist — leaving a `Starting S...` line with no
outcome under it. Silence there reads like a service that failed rather than one
with nothing to do. It now reports in the convention the other init scripts use:

```
Starting S26panicrec
  panicrec: skipped (no panic record)
```

`panicrec` distinguishes its two quiet cases, because a record already sitting
in `/userdata/panic/` is worth surfacing at boot: it reports
`skipped (record already saved, delete to re-arm)`.

`S70otbr` was the same defect in a third script, and the one that mattered most:
it exits silently on every gateway that is not a Thread border router — any
Zigbee box, and any fresh install whose radio has not been flashed yet. It only
escaped notice because the bench runs in OTBR mode; a first-boot log from a
virgin install is what exposed it. It now reports which mode won
(`otbr: skipped (radio mode: ncp)`), says `no radio configured` when
`radio.conf` does not exist yet, and `otbr-agent not installed` when the binary
is absent.

`S50uart_bridge` was checked and deliberately left alone: it prints nothing on
its success path because the kernel driver already logs `armed on <tty> @ <baud>
baud, listening on ...`, which is a documented decision rather than an omission.

The same two-voice convention was applied to `wireguardctl` — lifecycle
subcommands speak in the boot-log style, human-typed ones keep the tool's own
voice. That work stays in the tree under `34-Userdata/wireguard/`, which this
release does not ship; see its README.

### Fixed: `/var/log/messages` was timestamped in UTC while everything else was local

Correlating a field incident meant mentally shifting every log line by the UTC
offset — against the login shell, against a home automation history, against a
capture taken on another host. The offset is silent: nothing in the file says
which zone it is in, and the box has no RTC, so there is no boot banner to give
it away either. It cost a wrong reading of a real incident log before being
noticed.

`S05syslog` now exports `TZ` from `/etc/TZ` before starting syslogd, and passes
`-t`. Both are required, and this is the part that is easy to get wrong:
`syslog(3)` embeds its own timestamp in the message, and without `-t` BusyBox
syslogd copies that verbatim instead of stamping the line itself — so setting
`TZ` alone changes nothing for any line a daemon wrote, which is nearly all of
them. `TZ` was never in syslogd's environment to begin with: only `/etc/profile`
exports it, and only for login shells, while init runs each `S*` script in a
separate shell.

Bench-validated across a reboot: with `-t`, a client forced to `TZ=UTC` is
restamped local like every other, and the pre-NTP lines read `Jan 1 01:00:06`
rather than `00:00:06` — the zone offset is already applied to the epoch, so
`TZ` is in effect from the very first line written.

The cost is the one-hour fold at the DST fallback, which the previous comment
in the script cited as the reason for UTC. Kernel lines still carry the
monotonic `[ seconds ]` prefix and disambiguate themselves; for daemon lines,
an hour of ambiguity once a year is a smaller problem than a constant offset
against every other clock in the installation.

### Fixed: every syslog line was stamped `(none)` instead of the gateway's name

`/var/log/messages` never carried the hostname. Every line, for the whole
uptime, read `(none)` — which is not a formatting artefact but the kernel's
literal `CONFIG_DEFAULT_HOSTNAME`, the name the box holds until something sets
one. It stayed invisible for as long as nobody had another gateway's log to
read beside it.

BusyBox syslogd reads the hostname **once**, before forking, and stamps every
subsequent line with that copy. `rcS` runs the init scripts in glob order, so
`S05syslog` starts the daemon well before `S15hostname` gets to call
`hostname -F`. The daemon therefore caches the default and keeps it until it is
restarted — which is why a log could show the real name partway through: those
lines come from a second syslogd, started later from a login shell. That also
explains the mixed timestamps seen while diagnosing a field incident, since a
login shell sources `/etc/profile` and thus exports `TZ` as well: same second
instance, both symptoms.

`S05syslog` now applies `/etc/hostname` before starting the daemon. The
ordering constraint belongs to syslog, so it is expressed there rather than by
renumbering scripts; `S15hostname` keeps its real job, writing `/etc/hosts`,
which needs an address this early script does not have yet. Bench-verified:
zero `(none)` lines in a full boot, the name present from the first line.

<!-- Component versions -->

### Kernel — Linux 6.18.35 → 6.18.41 on the production line, 7.1.3 → 7.1.7 on the alternate one
Both lines move up and the four shipped images
(`{lidl, sengled-e39-g8c}` × `{6.18, 7.1}`) were regenerated, so every board
ships a kernel built from the same overlay as the drivers below. `uname -r`
names the upstream release and the firmware release together
(`6.18.41-rtl8196e-v4.0.0`), which is what identifies a kernel partition after
a kernel-only reflash.

The production line deliberately stops at `.41` rather than the newest `.43`.
Benching every point release of the interval, paired and back to back, puts the
last good release at `.41` and the first bad one at `.42`, which costs about
2.2 Mbit/s of TX:

| version | commits | TX | RX |
|---|--:|--:|--:|
| 6.18.38 | — | 68.2 | 90.6 |
| 6.18.39 | 496 | 68.3 | 89.8 |
| 6.18.40 | 1617 | 68.8 | 89.3 |
| **6.18.41** | 3 | **68.8** | 89.4 |
| 6.18.42 | 672 | 66.0 | 86.6 |
| 6.18.43 | 2 | 66.8 | 86.9 |

Those six rows are a within-campaign comparison on a fixed driver, taken before
the two I-MEM placements below, and they are left as measured — the point of the
table is the step between rows, not the absolute level. The shipped `.41` image,
benched after both placements, reads **TX 70.7** (10 reps, σ 0.80) and **RX
92.8** (3 reps, 91.9–93.2); the 7.1.7 image reads TX 72.2 and RX 89.4. Those two
are single runs taken 45 minutes apart, so the gap between the two lines is not
settled — only the level of what ships is.

The loss is not attributable to any single commit — reverting all of `net/`
restores it, no subgroup does — and whether it is extra work or merely a
different link layout was not settled. Either way, stopping at `.41` takes 2116
upstream commits at no measured cost, where `.43` takes 671 more and pays for
them; nothing in those 671 applies to this configuration. 7.1 takes its newest
point release, which costs nothing there.

All 57 patches per line apply with no reject and no fuzz. Four needed a refresh
for line offsets only, with identical content; four are new this release — the
`csum_partial` placement, the copy-core placement and the `modpost`
authorization it needs, and the `vermagic.h` arm below.

### Kernel — `csum_partial` runs from on-chip SRAM

Driver v2.23 made RX checksums a gated policy: TCP is verified by the stack
rather than trusting the switch's uncharacterised checksum bits. That turned
`csum_partial` into a per-RX-packet function, and it was still being fetched
from SDRAM.

The RTL8196E has 16 KiB of on-chip instruction SRAM (I-MEM), filled at boot
through the Lexra COP3 window. The window is **always** 16 KiB whatever `.iram`
holds, and only 8 700 B of it were in use — the remaining 7.7 KiB were
inter-section padding, SRAM that was simply wasted. `csum_partial` (1 432 B)
now goes there: `.iram` 8 700 → 10 144 B, `.text` down 1 440 B.

Measured on `.88`, four interleaved `base`/`csum` pairs across two sessions,
5×30 s TX and 3×30 s RX medians per point, radio quiesced:

| pair | RX baseline | RX with I-MEM | Δ RX | Δ TX |
|---|--:|--:|--:|--:|
| 1 | 89.1 | 91.1 | +2.0 | +0.3 |
| 2 | 89.9 | 90.8 | +0.9 | +0.9 |
| 3 | 89.5 | 90.7 | +1.2 | +1.5 |
| 4 | 88.9 | 90.2 | +1.3 | +1.5 |
| | | | **+1.35** | **+1.05** |

Eight paired differences, all positive, and the distributions do not overlap —
the best baseline (89.9) stays below the weakest treated run (90.2). Against a
±0.9 same-binary noise floor, no single pair would be a result; the pattern is.

What is **not** established is why. Moving 1 432 B out of `.text` also shifts
everything downstream, and code placement alone is worth ±2–4 % on this SoC.
Separating the two needs a `.space 0x598` left at the original location so every
other `.text` address is preserved; that control has not been run. The gain is
measured, its mechanism is assumed.

Placement is guarded on `CONFIG_RTL8196E_IMEM &&
CONFIG_RTL8196E_IMEM_DEFAULT_PLACEMENT`, so it follows the same two switches as
every C-side annotation and the PoC baseline that turns default placement off
still removes every default placement site.

Ships as a patch on both kernel lines.

### Kernel — the two assembly copy cores run from on-chip SRAM

A dynamic profile of the RX and TX paths — PC sampling in 16-byte buckets, one
bucket per cache line, RX and TX measured separately — ranked two shared
assembly bodies far above everything else per byte:

| body | size | % RX work | % TX work | % per KB |
|---|--:|--:|--:|--:|
| user-copy core (`__raw_copy_{to,from}_user`, `memcpy`) | 700 B | 41.5 | 20.4 | **90.6** |
| checksum-copy core (`__csum_partial_copy_*`) | 856 B | — | 15.1 | 18.1 |
| `tcp_ack` | 5 056 B | 0.4 | 1.7 | 0.4 |

Both move into the on-chip instruction SRAM, taking `.iram` from 10 144 to
11 712 bytes of the fixed 16 KiB window. `tcp_ack` is listed because it headed
every pair of the earlier *static* conflict model and returns 0.4 % of profile
per kilobyte — 220 times less than the copy core. Selecting by static extent had
been picking exactly the functions that matter least per byte.

Eight paired A/B runs of the shipping form, balanced order, one saved image per
variant with its md5 verified at every flash:

| | mean | 95 % CI | positive |
|---|--:|---|---|
| **TCP TX** | **+1.39 Mbit/s** (+2.0 %) | [+0.63, +2.15] | 7/8 |
| **TCP RX** | **+1.88 Mbit/s** (+2.1 %) | [+1.54, +2.21] | 8/8 |

The declared utility gate — a 95 % lower bound above 0.70 Mbit/s — was **not**
formally cleared on TX: the bound is +0.63. The point estimate is twice the bar,
RX is unambiguous, and the change ships with that stated rather than rounded
away. What causes the gain is also open: on-chip SRAM latency, an avoided
I-cache conflict, or both.

Verified before shipping: the exception table holds 477 entries with 105 having
instruction *and* fixup in `.iram` and **none straddling**; 236 of 14 310 `.text`
symbols move while `.data`, `.rodata` and `.bss` do not, `softnet_data` and
`tcp_ack` among them; and user-fault recovery was tested on the board — bad
addresses through `read`, `write` to a file and to a pipe, and `sendto` to
exercise `csum_partial_copy_from_user` — all returning `EFAULT` cleanly with no
oops.

Requires a third patch, `scripts/mod/modpost.c`, to authorize `.iram` as an
`__ex_table` fixup target: the copy-user routines carry exception entries for
faults on user pointers, and modpost refuses a target in an unlisted section.
Checked before authorizing that the faulting instruction and its handler move
into `.iram` together, so fault recovery stays intact.

`.iram` now holds 11 712 bytes of 16 384; **4 672 remain free**.

### Kernel — `CONFIG_MODULES=y` builds again on this port

Nothing is modular in the shipped configuration and nothing becomes modular
here. This removes a build failure that made the option impossible to even
evaluate.

`arch/mips/include/asm/vermagic.h` picks `MODULE_PROC_FAMILY` from a cascade of
upstream CPU types — BMIPS, MIPS32_R1..R6, MIPS64_\*, R3000, R4300, LOONGSON\*,
OCTEON, P5600 — and falls through to an `#error` otherwise. The Lexra core is
not an upstream CPU type, so `CONFIG_CPU_RLX4181` matched nothing and any
`CONFIG_MODULES=y` build stopped at:

```
vermagic.h:54:2: error: MODULE_PROC_FAMILY undefined for your processor configuration
```

An `RLX4181` arm at the head of the cascade fixes it. The string only has to be
unique and consistent between `vmlinux` and its modules. With
`CONFIG_MODULES=n` the macro is unused and the image is byte-identical
(`.text` 3 312 688 either way, verified).

Two facts for whoever picks this up. `CONFIG_MODULES=y` adds **381 KiB** to
`.text` — five times what WireGuard adds — because `--gc-sections` can no longer
drop anything reachable from an `EXPORT_SYMBOL`; `CONFIG_TRIM_UNUSED_KSYMS` was
not tried and would claw some of that back. And that 381 KiB costs **no
measurable throughput**: `MODULES=y WIREGUARD=m` measured 68.0 / 67.9 against a
shipping baseline of 69.6 / 67.4, a gap of 0.55 against ±0.9 repeatability. A
first pass at one run per point suggested modularising cost 1.6 Mbit/s; the
reversed-order repeat showed that was baseline drift.

Not measured: a module actually loaded. We know what its presence costs
(nothing); we do not know what its use costs, nor whether running from vmalloc
space penalises a tunnel against the built-in version.

Ships as a patch on both kernel lines.

### Userdata — OpenThread Border Router pinned to the upstream release `v2026.07.0`

`build_otbr.sh` pinned a main-branch commit (`717abf0d`, 2026-05-01) because
ot-br-posix had no recent tagged release; the project has since started cutting
CalVer releases, so the pin is now the tag `v2026.07.0` (2026-07-01). It contains
the previous pin — 107 ot-br-posix commits further on, carrying an OpenThread
core bump of 195 commits (`fb274efe6` → `c34311ff5`). Upstream's own notes for
this release are dominated by integration tests; the one host fix (D-Bus method
hangs on Spinel set-property failures) does not apply to a build configured with
`OTBR_DBUS=OFF`, and the advertising proxy that #3416 enables by default for all
mDNS backends was already active here through `OTBR_MDNS=openthread`.

What matters on a bump is that the three local patches still land, since two of
them are the reason Home Assistant can talk to this border router at all. All
three re-applied on the new tree and are visible in the artefact: the REST JSON
keys are PascalCase (`ActiveTimestamp`, `NetworkKey`, `PSKc`, `MeshLocalPrefix`
present; no camelCase spelling left), the six `/api/actions` route registrations
are commented out so python-otbr-api 2.10+ falls back to the PascalCase parser,
and the `uart-sw-flow-control` URL parameter (#134, @hlyi) is in the binary for
boards without RTS/CTS.

`otbr-agent` grows from 4 761 288 to 4 828 936 bytes (+66 kB, +1.4 % — the JFFS2
userdata partition has room), and reports
`OPENTHREAD/thread-reference-20250612-1257-gc34311ff5-dirty`, the `-dirty`
suffix being our flow-control patch. `ot-ctl` rebuilds byte-for-byte identical.
**Built, not yet exercised on hardware**: the development gateway currently runs
the NCP firmware (Zigbee), so its Thread path is not in play.

### Rootfs — Dropbear 2025.89 → 2026.94

Five upstream releases, most of them carrying security fixes, and the SSH daemon
is the gateway's only remote entry point. The ones that reach a build like ours:

- **2026.90** — an authenticated user could bypass an `authorized_keys`
  `forced_command` when the server runs with `-t`; `authorized_keys` is now opened
  non-blocking, so a special file in its place can no longer wedge a login; the
  missing OpenSSH patch for **CVE-2019-6111** (a malicious server overwriting
  unexpected files through `scp`) was added, as was clearing setuid/setgid bits on
  files received by `scp` (**CVE-2026-35385**). We ship `scp`, so both client-side
  fixes apply to anyone copying files *from* the gateway.
- **2026.92** — `-B` (blank password) combined with `-t` let a blank-password
  account in without pubkey auth; long `authorized_keys` lines were parsed such
  that the tail of a line became the start of a new one, which can defeat a
  `command=` restriction on keys added by external tooling.
- **2026.93** — use-after-free in X11 forwarding (not built here), out-of-bounds
  read in utmp/wtmp handling, and `permitlisten` entries with an invalid port are
  now ignored rather than misread.
- **2026.94** — fixes an `scp` build regression from 2026.93 and supports longer
  `scp` paths.

Two behaviour changes are visible from the outside. `scp -r` now refuses a target
directory that already exists (2026.90, logic corrected in 2026.91) — it affects
the gateway's own `scp` client, not the `scp -O` a host runs against the gateway.
And log lines now bracket IPv6 addresses that carry a port
(`[2a00:…:1c6]:22`), which matters to anything parsing them, `fail2ban` style.
Dropbear's new default of disabling client-side compression changes nothing here:
the build has always passed `--disable-zlib`, so compression was never available.

No configure flag had to change — the deprecations in 2026.92/93 concern auth
plugins and `--enable-plugin-deprecated`, which this build never enabled. The
result is a static MIPS-I musl binary of 634 608 bytes, 704 more than 2025.89,
and its advertised algorithm set is unchanged: curve25519-sha256,
sntrup761x25519, mlkem768x25519, ssh-ed25519, ecdsa-sha2-nistp256, rsa-sha2-256,
chacha20-poly1305 and aes256-ctr all resolve in the new image exactly as in the
old one.

Validated on the development gateway before shipping, without disturbing the
running daemon: the new binary was copied to `/tmp`, generated an ed25519 host
key with its own `dropbearkey`, and served a second instance on port 2222 while
the production one kept port 22. Through that instance, an interactive session
opened and ran commands, and a 200 kB `scp` transfer arrived with a matching
md5 — which also exercises the gateway-side `scp` applet, the one the CVE
patches above touch. The instance was then killed and the temporary files
removed.

`build_dropbear.sh` also changed its working directory to its own: every path
after the download was relative, so running the script from anywhere but
`33-Rootfs/dropbear/` scattered a `DROPBEAR_<version>/` tree wherever the shell
happened to be — which is exactly how a stray `33-Rootfs/DROPBEAR_2025.89/`
came to exist.

<!-- Kernel drivers -->

### Kernel — Ethernet LAN LED true off (`rtl8196e-eth` v2.23 → v2.24)

The visual LED regression gate found that `led_mode=off` reported success but
left the LAN LED visibly DIM. The old path wrote `LEDCREG=0` and
`DIRECTLCR=0`; the RTL8196E SDK definitions and live register tests show why
that cannot mean off: topology zero is scan mode, while DIRECTLCR is a
duty-scale register whose zero value still leaves a minimum-duty glow.

v2.24 declares the active-low LAN LED pad on the Ethernet node, claims it
through gpiolib and preloads its inactive physical-high state. `bright` and
`dim` program the ASIC and route the pad to LED_PORTn; `off` routes only that
pad to GPIO, where it remains fully dark. The mux field is derived from the
board device tree rather than hard-coded: Lidl uses B6/LED_PORT4 and Sengled
G4 uses B2/LED_PORT0. Failed mux changes are propagated and the remembered
mode is updated only after a successful hardware transition, so switch-core
reset replay cannot advertise an unapplied state.

The true-off GPIO route was visually validated on Lidl. The 6.18 and 7.1
overlays carry identical v2.24 sources, and all four Lidl/Sengled build
combinations compile. The LED test script now detects the board and records
the ASIC, pin-mux and GPIO readbacks needed to distinguish true OFF from the
former false-DIM state.

### Kernel — UART bridge client/relisten lifecycle (`rtl8196e-uart-bridge` v1.6 → v1.7)

The independent A2 audit exposed that the bridge's advertised
replace-on-connect policy was dead code: its only worker blocked in
`kernel_recvmsg()` for the first client and could not return to `accept()` to
see a replacement. v1.7 splits those roles. A permanent accept worker remains
available while a dedicated client worker shovels TCP→UART; a new connection
atomically removes the predecessor, shuts it down, joins its worker and takes
over. The client worker owns its socket through final release, including EOF,
replacement and disarm paths, so the fix does not trade the availability flaw
for a socket/task lifetime race.

Live listener reconfiguration is also transactional. The old listener is
stopped before binding its replacement, allowing the security-sensitive
`0.0.0.0` → `127.0.0.1` transition on the same port without the old wildcard
socket causing `EADDRINUSE`. The independent client worker is preserved across
successful port/address changes. A failed change recreates the old listener;
if rollback itself fails, the bridge disarms rather than retaining a false
`armed=1` state without an acceptor.

Finally, tty callback ownership changes are quiesced. Arm holds the tty
flip-buffer exclusion, installs bridge `client_ops` before opening the UART and
publishes all state before releasing it. The first v1.7 candidate target disarm exposed
an ordering bug: it called `uart_close()` while already holding the
non-recursive flip-buffer mutex, then self-deadlocked when
`tty_buffer_flush()` tried to acquire that mutex. The corrected v1.7 closes the UART with
`bridge_lock` dropped, allowing close/flush and any dispatched RX callback to
finish, and only then takes flip-buffer exclusion to restore the saved
callback table. Partially armed error unwinds use the same ordering.

The 6.18 and 7.1 objects compile and their complete `vmlinux` images link with
`W=1`; their sources differ only by the 7.1 `kernel_bind()` argument type.
The corrected v1.7 was then validated on the development gateway at 460800 baud:
active/idle client replacement, live client teardown and replacement, repeated
arm/disarm, and sustained read-only EZSP traffic teardown passed on both kernel
lines. The target gates are therefore closed for this unreleased change.

### Kernel — TC0 clockevent: do-not-toggle hardening (`timer-rtl819x` v1.2 → v1.3)

The historical saturation wedge is strongly attributed to TC0’s COUNTER-mode
`TC0_EN: 0 → 1` re-arm edge, rather than to Ethernet traffic or lost MMIO
writes.
During the failures the visible enable, DATA0 and interrupt registers remained
correct while COUNT0 stayed zero; TC1 continued at the healthy 25 kHz cadence.
The evidence is consistent with TC0 functional logic missing that edge across
its slow clock domain.

The production remedy is **do-not-toggle (DNT)**. TC0 enters TIMER/auto-reload
mode with enable held high; every one-shot is armed by writing DATA0 while it
runs. The old disable → DATA0 → enable transition is absent. The IRQ sequence
masks TC0, W1C-acks, writes DATA0, W1C-acks again and unmasks, preventing a
stale completion from becoming the next event. During NO_HZ idle, TC0 remains
enabled but IRQ-masked, so `CONFIG_NO_HZ_IDLE` is safe and tickless operation
does not recreate the hazardous edge.

The DATA0 reload semantics were validated with forced minimum, intermediate and
maximum/short-replacement cells. A running maximum (~3 h) deadline was
replaced by a short one deterministically, with zero missed, double or
parasitic IRQs. The cause-level DNT soak completed **26.3 h and 54.8 million
kernel reprogrammings under bidirectional saturation**, with zero wedge, missed
IRQ, double IRQ, parasitic IRQ or unresolved write-ahead record.
`pend_at_unmask` at the registered minimum delta remains telemetry: pending is
preserved and the corresponding IRQ is delivered.

DNT is selected by `CONFIG_RTL819X_TC0_DNT=y`. The verified legacy
disable/DATA0/enable + observe/retry path remains the protected boot and
fallback path. Experimental phase sweeps, busy waits and forensic DRAM
instrumentation are removed from the production hot path. Three
reboot-separated iperf3 suites on 6.18 for each configuration showed no
adverse throughput effect beyond normal run-to-run variation. Three additional
7.1 DNT runs confirmed compatibility on that kernel line. Unlike phase
avoidance, DNT has no per-event busy-wait cost.

### Kernel — watchdog: production recovery record (`rtl819x-wdt` v1.11 → v1.12)

The watchdog is returned to its production role. Start, stop and ping are
constant-time MMIO operations; the 1 Hz flight recorder, printk capture,
scheduler/list walks, Ethernet/NAPI and INTC snapshots, and multi-record
decoder are removed.

On panic it first arms the validated short reset sequence (disable/clear, then
zero), then optionally writes a fixed 108-byte record v9: uptime, EPC/RA, CP0
cause/status, pending softirqs, WDTCNR, flags and a sanitized reason. Magic is
published last. The next boot prints one `previous panic:` line and clears the
DRAM record; `S26panicrec` persists the first such line to
`/userdata/panic/history`, which the operator explicitly deletes to re-arm.

The record has a dedicated no-map page, `watchdog-crash@1ffd000`, separate
from bootloader handoff page `boothold@1ffe000`. Both 6.18 and 7.1 watchdog
objects/DTBs compile cleanly and the driver passes checkpatch.

`S26panicrec` no longer dates its capture line 1970. The board has no RTC, so
the clock starts at the epoch every boot, and since #125 `S20time` only starts
`ntpd` as a daemon rather than blocking on a one-shot sync — the step lands
well after this script, which meant the persisted line always carried a
confidently wrong date. It now marks the stamp `(clock not yet synced)` when
the year is implausible, and points at the record's own `uptime=` field, which
is the trustworthy indication of when the panic happened.

Bench verification of the whole chain on the release image (2026-08-07,
`.88`): a `sysrq-c` panic passed all ten checks — record written, survived the
reset, decoded once, persisted, watchdog and feeder re-armed — and a second
incident with the feeder killed outright confirmed the hardware bite itself,
the board resetting unaided about 681 s after the last kick and coming back
with the DRAM magic already cleared and `history` untouched, so the one-shot
and first-occurrence guards both hold across a later reboot.

### Kernel — software-PWM LED lifecycle hardening (`leds-gpio-pwm` v1.1 → v1.2)

The STATUS LED PWM state machine is now safe for the full atomic LED callback
contract. The old rail transition called `timer_delete_sync()` from
`brightness_set()`, which LED triggers may invoke in hard-IRQ context; if that
IRQ interrupted the PWM softirq, both contexts could wait on each other
forever. Threshold, counter, active state, GPIO writes and timer decisions are
now serialized by one IRQ-safe lock. Rail transitions mark PWM inactive and
use non-blocking `timer_delete()`; synchronous shutdown is confined to an
ordered process-context devres action after LED-class unregister and before
GPIO release.

Probe now rejects sleep-capable GPIO providers, propagates kept-state read and
direction errors, and no longer retains unused private brightness/driver data.
A new `leds-gpio-pwm` DT schema documents the supported gpio-leds-like subset
and rejects the lifecycle properties the driver does not implement. Source,
binding, DESIGN and AUDIT are identical between 6.18 and 7.1; the equivalent
Kconfig changes differ only by their upstream context line.

The established PWM behavior is unchanged: four jiffies per cycle at `HZ=250`,
62.5 Hz output, quantized rails with no recurring timer, and the intentional
`mod_timer(..., jiffies)` rearm that fixes issue #120. Intermediate brightness
still creates 250 timer wakeups/s and limits `NO_HZ_IDLE` residency while
active; this is an explicit design cost, not a hidden regression.

### Kernel — IRQ and GPIO drivers: independent-audit diagnostic-hygiene pass (`irq-rtl819x` v1.1 → v1.2, `gpio-rtl819x` v1.2 → v1.3)

Independent security and performance audits of the interrupt-controller and
GPIO drivers found no vulnerability and no runtime defect in either. The routing
policy was confirmed appropriate for this product and is unchanged: TC0 on IP7,
UART1 on IP4, switch/Ethernet on IP3, UART0 on IP2. What the audits did find is
that three pieces of telemetry meant what their comments did not say — harmless
to execution, but able to send a future investigation down the wrong path.

The headline: the architecture dispatcher named CPU line IP3 "UART1" and IP4
"SWITCH", the exact inverse of the routing the irqchip programs (`IRR1 =
0x30420007`, read back on live silicon). Nothing was ever misrouted — IP*n*
maps to CPU IRQ *n*, and all three cascade lines share one chained handler — but
the inversion did mislead a forensic campaign into reading an IP4-only trace as
an Ethernet interrupt storm when it was ordinary UART1 traffic, and it would
become a real bug the day distinct per-parent handlers are installed. The
constants are now named after the CPU line they carry, above a canonical routing
table that the IRR programming, the device tree and these labels must all agree
with.

Second, `count[8]` was being read as a tick counter and is not one. TC0 is
requested directly on CPU IP7 and never traverses the chained INTC handler, so
its GISR bit is only ever observed incidentally — it read near zero while the
clockevent was plainly running. A dedicated `ip7` counter is now incremented in
`plat_irq_dispatch()`, and the watchdog flight recorder's `d_tc0` reads it.
Two more fields had their documented meaning tightened: `empty` is an ambiguous
empty chained entry rather than proof of a storm (the one-snapshot dispatch
design produces it in normal operation, when one parent drains every pending bit
and the sibling parent then enters with nothing left), and `last_seen_j` is
meaningful only while the clockevent is alive, since jiffies freeze together
with the tick they measure.

On the GPIO side, a `realtek,syscon` lookup failure now fails the probe through
`dev_err_probe()` instead of collapsing every error into "absent". The old path
also swallowed `-EPROBE_DEFER`, which could have turned a mere probe-ordering
issue into permanently dead LED, button and EFR32-reset lines; every supported
board inherits the property from the shared `rtl819x.dtsi`, so its absence is a
broken device tree rather than an optional feature. A device-tree binding was
added for the controller, constraining `realtek,led-pads` to unique offsets
10-14 and writing down the shared-pad ownership contract that the GPIO and
Ethernet drivers both read. A schema cannot catch an electrically wrong but
syntactically valid pad number — that failure reads back perfectly correct and
shows up only as a dead LAN LED — so board bring-up still requires a visual
check. The registration banner now reports the core's effective `gc.ngpio`
instead of a private constant that could only drift away from it.

Both kernel overlays carry the change: the six affected files were byte-identical
between the 6.18 and 7.1 trees beforehand, and are again.

Both audits explicitly recommended leaving several things alone, and they were:
the interrupt routing and priorities, consumer-driven unmasking, the one-snapshot
bounded dispatch, the current I-MEM placement, the always-on INTC counters, the
absence of a GPIO `.free` mux restore, the absence of a hardware valid mask
while the character device stays root-only, and the absence of a GPIO irqchip.

Verified statically so far: clean build, `.iram` at 8 700 of 16 384 bytes after
the added IP7 counter, checkpatch clean on every touched file except the
pre-existing space indentation of the architecture dispatcher. The bench gates
from both reports — INTC counters against `/proc/interrupts`, `d_tc0` against
CPU IRQ 7, LAN LED verified by eye, EFR32 reset pulse — are still to run.

### Kernel — UART1 driver: independent-audit flow-control rework (`8250_rtl819x` v1.6 → v1.7)

An independent security and performance audit of the EFR32-link UART driver was
implemented and bench-validated on both kernel lines. The headline change: the
driver now expresses RTS ownership in the serial core's native vocabulary
instead of overriding it. The `set_mctrl` guard that force-held RTS under
hardware flow control (the issue #109 wedge fix) protected the link but also
intercepted legitimate requests — dropping to `B0` could leave physical RTS
asserted, and a `CRTSCTS` removal could desynchronise the modem-control shadow.
It is replaced by advertising `UPSTAT_AUTOCTS`/`UPSTAT_AUTORTS` (set only after
the MCR read-back confirms the hardware state) plus `throttle`/`unthrottle`
callbacks in the mainline `8250_omap` pattern: under tty backpressure the core
now delegates to the driver, which masks the RX interrupts so the FIFO fills
and the hardware itself deasserts RTS. `B0` genuinely drops DTR and RTS.

The port lifecycle was also moved where the core expects it: the FIFO trigger
and the flow-control gate are armed by a `startup` callback (before the core
programs the FCR) and disarmed by a symmetric `shutdown`, eliminating the
post-registration writes that raced the first open of `ttyS1`. Probe no longer
touches the UART beyond the pin mux — operators reading the MCR via `devmem`
will now see `0x00000000` until the first open, which is expected. Hardening
around it: the optional phantom-timeout log line moved from hard-IRQ context
to a workqueue, the phantom counters became 64-bit atomics, the MMIO window is
size-checked, the silent 200 MHz clock fallback now fails probe explicitly,
the UART1 pin-mux bits are restored on probe failure and removal, and the N+1
divisor now programs zero for a unit quotient (unreachable below 12.5 Mbaud).

Validated on the Lidl gateway on both kernel lines: termios/MCR semantics
including the `B0` transitions, 500 open/configure/close cycles, an OTBR soak,
and a loopback load campaign — 10-minute floods at 460800 and 892857 in both
flow modes with byte-perfect echo, zero hardware overruns and IRQ/CPU cost on
par with the June baseline; the new throttle path engages and recovers cleanly
under a stalled-reader stress. The campaign also characterised a pre-existing
platform envelope, confirmed by an A/B against the previous driver: a
sustained full-rate flood at 691200 overruns the tty buffer layer (the flip
worker starves at ~97 % CPU) while the hardware FIFO stays clean — real radio
workloads sit far below this envelope, 460800 and 892857 floods are lossless,
and the same traffic paced at 80 % is lossless at 691200 too.

The driver's two audit reports were merged into a single English `AUDIT.md`
(findings `8250RTL-010..017`) and `DESIGN.md` was rewritten to the v1.7
architecture, both mirrored in the 6.18 and 7.1 overlays.

### Kernel — Ethernet driver: independent-audit hardening pass (`rtl8196e-eth` v2.21 → v2.23)

Two independent from-scratch audits of the `rtl8196e-eth` driver were reconciled
and their findings implemented, then bench-validated on the Lidl gateway. The
headline correctness fix: `ndo_tx_timeout` was calling `napi_disable()` — which
sleeps in 6.x — from the `dev_watchdog` timer softirq while holding
`tx_global_lock`, a "scheduling while atomic" bug that fired precisely under the
load a TX timeout is most likely to hit. The callback is now atomic (stop the
queue, record a diagnostic fingerprint) and hands the ring/switch recovery to a
workqueue.

Around it: the NAPI kick path masks the device IRQs before publishing NAPI
state, closing a race the ISR and both software timers shared; the switch-core
deep-reset recovery gained an explicit hold-down with bounded 1/2/4 s retries
that never re-opens interrupts or carrier on a half-programmed switch and
requires an administrative down/up once the retry budget is exhausted; each RX
packet-header descriptor now occupies its own cache line, removing a
false-sharing window where a rearm write-back could clobber a neighbouring
switch DMA update (a silent, rate-dependent RX drop); the RX checksum policy now
defaults to software verification except for characterised unfragmented IPv4
UDP, trading a measured ~2–5 % RX at 8-stream saturation for the kernel's
checksum-integrity contract instead of trusting uncharacterised switch bits for
TCP/IPv6; the hardware table engine fails and propagates handshake/setup
timeouts instead of silently proceeding; in-flight TX SKBs freed by a ring reset
are accounted as `tx_dropped`; and packet/byte totals are 64-bit via
`ndo_get_stats64` so they no longer wrap in days at line rate. Descriptor-pointer
validation (exact element stride), the RX length bound (MTU + headers), the
interface MTU floor (576), the `phy-id` range and a `!SMP` build guard were also
tightened, and a coalesced TX kick is now drained at each dequeue-batch boundary
so a descriptor flipped after the TX engine parked is never stranded on an
RX-silent link.

Two "improvements" were investigated on the bench and **rejected** because they
brick the port: explicitly programming the PCR accept-max-length field — the
vendor never writes it, its bit position is chip-variant-dependent, and writing
it wedged the port entirely — and clearing the CSCR `AcceptL2Err` bit, which
silently killed all CPU transmit (the CPU injects FCS-less frames under
`EXCLUDE_CRC`, and that bit is exactly what lets the CPU port accept them). Both
are left at their silicon defaults and the reasoning is documented in the
register header.

Validated on hardware: TCP RX at 100BASE-TX line rate (~94 Mbit/s), TX in the
historical ~70 Mbit/s band, zero TCP retransmissions across the full RX/TX/UDP
suite, all ring-anomaly and switch-core recovery counters at zero, and the
recovery hold-down / bounded-retry / administrative-recovery lifecycle exercised
by fault injection.

### Kernel — the blmode pulse reports its sequence, not an outcome it cannot see (`rtl8196e-uart-bridge` v1.5 → v1.6, discussion #148)

`blmode_pulse` drops the radio into its bootloader by holding the board's
bootloader-entry pin through an nRST pulse. It then announced `blmode released,
EFR32 in bootloader` — a claim the driver is in no position to make. Whether the
radio actually stopped in its bootloader depends entirely on the firmware at the
far end of the pin: a Gecko bootloader built without GPIO activation never
samples it and boots the application instead. Nothing on the host side can tell
those two outcomes apart.

On a board whose pin works the message was merely redundant. In the one case
where an operator actually needs the log — a radio whose bootloader ignores the
pin — it asserted success while the chip quietly went back to running its
application, which is exactly the wrong thing to read at that moment. Both lines
now state what the driver did (`resetting EFR32 with blmode asserted`, then
`blmode released, reset sequence complete`); establishing what the chip is really
running is the caller's probe's job, which is what `flash_efr32.sh` has always
done with the result.

Raised by @hlyi while reviewing the bootloader-entry fast path on a Sengled G4.

### Kernel — the phantom RX-timeout recovery goes silent (`8250_rtl819x` v1.5 → v1.6, issue #99)

The rc5 quirk logged a rate-limited warning on every phantom RX-timeout it
cleared — the field-confirmation instrument of the issue #99 campaign. The
field data then showed the instrument was not free: the warning is emitted
from the interrupt handler, and on a legacy (non-nbcon) console every record
is written with interrupts disabled — ~147 characters at the 38400-baud
console is **~38 ms of interrupts-off time per line**. On a board without
RTS/CTS nothing stops the radio during that blackout, so the warning caused
the very hardware overrun it was suspected of witnessing (the one field
overrun coincident with a phantom landed 42 ms after the warning line — the
console-write duration plus the drain). On RTS/CTS boards the AFE
back-pressure makes the blackout loss-free, but each occurrence still cost a
38 ms interrupt outage — and a fully-handled quirk that fills a healthy box's
log with "stuck ... empty RX FIFO" lines reads like an error to users.

v1.6 adopts full parity with the mainline `dw8250_handle_irq` precedent the
workaround came from: the dummy-read recovery is now **silent**. Observability
moves to two module parameters:

- `/sys/module/8250_rtl819x/parameters/phantom_count` — per-boot total of
  phantom recoveries (read-only; replaces counting `dmesg` lines).
- `/sys/module/8250_rtl819x/parameters/phantom_log` — set this boolean
  parameter to `Y` (for example, `printf 'Y\\n' >
  /sys/module/8250_rtl819x/parameters/phantom_log`) to restore the timestamped
  per-occurrence log line (identical text, now `KERN_INFO`) at runtime, when
  diagnosing a field incident needs time correlation. Reading a boolean module
  parameter returns `Y`/`N`; `1`/`0` are accepted input aliases, so
  `8250_rtl819x.phantom_log=1` remains valid on the boot command line. No
  reflash or reboot is required for a runtime change.

Upgrading soakers, note: the `stuck RX-timeout` dmesg lines disappear at this
version — read the counter instead.

Issue #99 was reported by @olivluca; the field campaign that produced both the
quirk and this refinement ran on gateways volunteered by @olivluca, @frtz13,
@MaxRower and @hlyi.

### `otbr-agent` — real software flow control on the RCP UART (discussion #134, @hlyi)

A board without RTS/CTS wiring (the Sengled G4) runs its OT-RCP on the iostream
UART backend, which **emits** XON/XOFF when its RX buffer hits the watermark.
Upstream OpenThread cannot consume them: `HdlcInterface::OpenFile()` calls
`cfmakeraw()` — which clears `IXON`/`IXOFF` — and its only flow-control radio-URL
parameter, `uart-flow-control`, sets `CRTSCTS` (hardware only). So on such a
board the radio's 0x11/0x13 reached the HDLC decoder as frame data, failed the
FCS, and the frame was dropped. There is no way to get `IXON` from the outside:
`tcsetattr()` at open overrides anything `stty` had set.

`build_otbr.sh` therefore applies a patch to the OpenThread submodule
(`patches/openthread-uart-sw-flow-control.patch`) adding a `uart-sw-flow-control`
parameter that clears `CRTSCTS` and sets `IXON|IXOFF`, and `S70otbr` passes it
when `radio.conf` says `FIRMWARE_FLOW_CTRL=sw` (`hw` still selects
`uart-flow-control`, `none` neither). `IXON` honors the radio's XOFF and consumes
the flow bytes; `IXOFF` lets the tty throttle the radio in return — the iostream
backend honors an inbound XOFF, so the link is now flow-controlled in both
directions. This is safe on a spinel link because OpenThread's HDLC encoder
escapes 0x11/0x13 (`HdlcByteNeedsEscape`), so no payload byte can be mistaken for
a flow character. Software flow control protects the software buffers, not the
16-byte hardware FIFO: **230400 remains the recommended operating point** for a
`sw` board; 460800 without RTS/CTS stays best-effort.

Patch authored and validated by @hlyi on a Sengled G4, at the recommended `sw`
operating point: OT-RCP at 230400, 18 hours of uptime with repeated Matter
pairings, no `oe:` field on the `ttyS1` line of `/proc/tty/driver/serial` and no
overrun warning in `dmesg`. The shipped `otbr-agent` is rebuilt with it.

<!-- Tooling -->

### `S70otbr` — derive the Thread BorderAgent vendor/model from the board (discussion #144)

Requested by @hlyi (#144): `S70otbr` hard-coded `--vendor-name Lidl` /
`--model-name Silvercrest` in the `otbr-agent` command line — the values a
commissioner sees in the mDNS `_meshcop._udp` `vn`/`mn` TXT records. On a ported
board (e.g. the Sengled G4) that mislabels the device. The two flags are now
derived from `/proc/device-tree/model` — the same authoritative board identity
`flash_efr32.sh` already keys off — defaulting to `Lidl` / `Silvercrest`, so the
reference board is unchanged, and advertising `Sengled` / `E39-G8C` on the G4 —
the model number on the unit's own label, @hlyi's call, not the marketing name
("G4"). Matched with a `case` glob on the model file's contents — needs only `cat`
plus a shell builtin (no `grep` binary-file heuristic, no `tr`; the ash command
substitution simply drops the model node's trailing NUL).

### `build_fullflash.sh` / `create_fullflash.sh` — the `xxd` requirement is gone (issue #147, @hlyi)

Both scripts verify the assembled image by reading the four partition magic bytes back
out of it, and did so with `xxd -p`. On Debian `xxd` ships in its own package and is not
installed by default, so on a fresh host the run died — under `set -euo pipefail`,
`xxd: command not found` (exit 127) aborted the script *after* the 16 MiB image had been
assembled. `flash_install_rtl8196e.sh`'s prerequisite check did not catch it: the tool is
used by the script it delegates to, not by the checker.

Rather than add `xxd` to that checklist — which would only have covered that one entry
point, leaving both fullflash scripts to fail the same way when run on their own, as the
documentation invites — the dependency is removed. The magics are now dumped with
`od -An -tx1 -v | tr -d ' \n'`: `od` and `tr` are coreutils, present on every Linux host.
`xxd` is consequently dropped from `install_deps.sh` and the Dockerfile — nothing in the
tree invokes it any more.

## [4.0.0-rc5] - 2026-07-08

_A **UART stuck-interrupt storm** with the exact issue #99 soft-lockup signature caught
in the act on the bench and fixed (via the new **v8 diagnostic instrumentation**: flight
recorder + panic-time kernel-log capture) — **field confirmation pending**; root-cause fix for
the intermittent **RLX4181 TLB fault** (issue #109) behind the busybox
`SIGSEGV`/`SIGILL`/`SIGBUS` crashes, switch-core **PHY-interface watchdog**, driver-comment
cleanup, a **userdata supervision overhaul** (keepalive for every long-lived daemon +
`otbr-monitor` ported from busybox-ash to a C daemon), an **`S70otbr` spinel
flow-control fix** for boards without RTS/CTS (discussion #142), and **per-board
pre-built bootloaders** closing the discussion #140 brick scenario, heading to the
next release._

### Kernel — issue #99: a UART1 stuck-IIR interrupt storm captured and fixed — field confirmation pending (rtl8196e-uart v1.4 → v1.5)

- **The v8 instrumentation caught a soft-lockup in the act — on the bench, the day it
  was built.** The reference unit crashed with the exact issue #99 macro-signature
  while the flight recorder was running. The film of THAT crash is unambiguous: for the
  final 21 s **UART1 (the EFR32 radio link) rotated at ~1360 interrupts/s = 100 % of
  all interrupt-controller dispatches**, starving everything else to zero (timer wheel
  frozen, NAPI never run — the ethernet "culprit" of the earlier theories an innocent
  bystander in this capture). The UART registers at panic show a **contradictory,
  frozen state**: IIR = 0xCC (interrupt pending, ID = RX character timeout, "bytes are
  waiting in the FIFO") while LSR = 0x60 (Data Ready clear, "RX FIFO empty").
- **Mechanism:** the 8250 core gates its RX drain on `LSR_DR`, so it reads nothing and
  the timeout condition is never cleared; the level-triggered line re-asserts after
  every interrupt return. Each rotation costs ~700 µs of MMIO on the 200 MHz bus
  (~95 % of the CPU in hardirq): no schedule point is ever reached, the softlockup
  detector fires at 20 s and the watchdog reboots the box.
- **Fix:** a custom `port->handle_irq` in the UART1 glue driver — the classic
  16550-clone workaround with a direct mainline precedent (`dw8250_handle_irq`: *"there
  are ways to get [these] UARTs into a state where they are asserting
  UART_IIR_RX_TIMEOUT but there is no actual data available … if we don't do this then
  the 'RX TIMEOUT' interrupt will fire forever"*). On an RX-timeout ID with an empty
  LSR, one dummy RBR read flushes the phantom byte and clears the latch. A rate-limited
  `stuck RX-timeout IIR … occurrence #N` warning is the confirmation signal: one log
  line = one averted storm, with unbroken uptime.
- **Honest scope — this issue has been declared fixed twice before and the field
  disagreed both times, so the claims are kept falsifiable.** Established: the bench
  crash was this UART1 storm, measured at the registers. NOT yet established: (a) the
  field crashes match on the macro-signature only — the v7 records structurally could
  not see UART registers or per-line interrupt rates, so whether the reporting units
  die of the same mechanism is unconfirmed until one of them logs the trace line (or a
  v8 record decides otherwise); (b) the quirk has never been exercised against a live
  storm (no false positives ≠ efficacy). Confirmation = the trace line with unbroken
  uptime and no recurrence over several times the historical 3–8 day interval.
- Validated so far: no false positives under normal spinel traffic nor across 10
  otbr-agent restart cycles under a 60 Mbit/s UDP flood; ethernet throughput untouched
  by construction (the change is confined to the UART1 interrupt path).
- The rc4 bounded RX poll and all its detectors remain in place as hardening; the v8
  record (below) stays as the permanent black box precisely in case this diagnosis is
  wrong too.

### Kernel — issue #99 reopened: v8 diagnostic instrumentation (wdt v1.11, eth v2.21, irqchip v1.1)

- **The rc4 theory did not survive the field.** The first crash captured with the full
  v7 record (uptime 7.83 d on v4.0.0-rc4) showed **every** rc4 detector counter at zero:
  the bounded RX poll never saturated its budget once (`pollhit=0`), no stall run, no
  deep reset, RUNOUT clear, TX idle — while the box still soft-locked ~21 s with
  TIMER|NET_RX pending and a frozen timer wheel. The bounded poll remains (correct
  hardening), but the field mechanism is elsewhere: the evidence points at an interrupt
  storm on an **unidentified line**, with the ethernet state at panic consistent with an
  innocent bystander (the normal `napi_defer_hard_irqs` masked window). The storming
  source cannot be named from a final-frame snapshot — hence v8, which records the film:
  - **Panic record v8** (`rtl819x_wdt` v1.10 → v1.11). The reserved 4 KB DRAM page is now
    fully used: v7 core record (byte-compatible) + a v8 scalar block (CP0 Cause/Status,
    GIMR/GISR, `preempt_count`, interrupted task comm, a raw **UART1 8250 register
    snapshot** (IER/IIR/LSR/MSR/MCR), INTC dispatch stats, per-line last-seen jiffies,
    eth activity stamps, RX ring base addresses that make the switch-pointer captures
    decodable into ring indices).
  - **1 Hz flight recorder.** A storm-proof `HRTIMER_MODE_REL_HARD` sampler (the same
    context the softlockup detector provably kept running from during the field hang)
    records per-second deltas of total hardirqs, per-INTC-line dispatches
    (UART0/UART1/switch/TC0), NET_RX/TIMER softirq runs, eth ISR/poll/delivered/rx
    counters, INTC entries and **empty-pending entries** (the signature of an INTC-level
    storm no existing counter could see), plus live CPUIISR/CPUIIMR and NAPI state. The
    newest 31 samples are copied into the DRAM page at panic — the 20 s hang window plus
    a pre-onset baseline.
  - **Panic-time kernel-log capture.** The last ~2.1 KB of the printk buffer are saved
    into the DRAM page at panic (strictly best-effort, torn-safe length marker): it
    contains the softlockup report — including the per-IRQ interrupt-storm utilization
    table the kernel already compiles in (`SOFTLOCKUP_DETECTOR_INTR_STORM`) — and the
    backtrace that headless field units could never show. Decoded and re-emitted at the
    next boot under `flight|` / `panic-log|` prefixes; `S26panicrec` now persists the
    whole multi-line block to `/userdata/panic/history`.
  - **eth v2.20 → v2.21**: ISR/poll invocation counters with last-activity jiffies
    stamps, a per-jiffy ISR burst high-water (an eth-line hardirq storm cannot hide from
    it), a counter on the tx-reclaim timer's NAPI kick (the one `napi_schedule` that sets
    `SCHED` without masking), all exposed via `ethtool -S` and the v8 record.
  - **irqchip v1.0 → v1.1**: INTC dispatch statistics (entries, empty-pending, per-line
    counts and last-seen jiffies) feeding the recorder and the record.
  - Bench-validated end-to-end on the reference unit: sysrq-c panic → next boot decodes
    the v8 scalars, the 31-row flight table (exact 1 s cadence) and the log tail ending
    at the panic banner; live counters advance at the expected idle rates.

### Kernel — MIPS TLB flush (RLX4181)

- **Root-cause fix for the intermittent RLX4181 `SIGSEGV`/`SIGILL`/`SIGBUS` faults
  (issue #109).** Under fork/exec pressure a user process would intermittently read the
  *wrong physical frame* for a correctly-mapped virtual address — a plain load returned a
  stale pointer its own page table never pointed to — corrupting a register mid-sequence
  and crashing (busybox-ash, the fork/exec-heaviest process, was the most exposed).
  The cause was in `local_flush_tlb_all()`: it began its invalidation sweep at a hardcoded
  TLB index of **8**, inheriting the classic-R3000 assumption that the `Random` register
  never allocates entries 0–7. The Lexra RLX4181 does **not** honour that convention — it
  reserves entries through the `Wired` register, which is **0** on this core — so
  `tlb_write_random` installs translations into slots 0–7 and the bulk flush left them
  untouched. A stale mapping parked in a low slot survived every flush and, once the ASID
  generation rolled over, spuriously matched and resolved a user address to the wrong
  frame. The sweep now starts at `read_c0_wired()`, matching the vendor `arch/rlx` TLB
  code. Confirmed by ~53 min of continuous fork/exec churn with **zero** faults, on a load
  that crashed every prior build within 200–600 s. Applies to both kernel lines (6.18 and
  7.1). This is the actual fix behind the earlier ash→C daemon rewrites, which were
  mitigations that moved code off the exposed path rather than closing the bug.

### rtl8196e-eth driver — v2.19 → v2.20

- **Switch-core PHY-interface watchdog.** A periodic 1 s check reads the switch's
  per-port `EnablePHYIf` bit; if it reads back **clear** on an administratively-up
  port across three consecutive checks — the switch core silently severed the
  MAC↔PHY interface, a stall with neither a `PKTHDR_DESC_RUNOUT` nor a TX-done
  hang, invisible to the existing detectors — it escalates to the shared
  `FullAndSemiReset` deep reset. Mirrors the vendor `one_sec_timer()`
  `EnablePHYIf` recovery branch. Bench-validated: detection in ~3 s, full port
  recovery in ~3.2 s, independent of the RUNOUT / stall detectors (they stay 0).

### Cleanup

- Genericized the eth + watchdog driver comments — dropped the project-internal
  issue-tracker / audit-finding tags so the code reads standalone for reuse.
- Removed the `CONFIG_RTL8196E_ETH_DEBUG` fault injectors (`force_dropflood` /
  `force_stall`): bench validation scaffolding, never shipped in any image.

### userdata — unified process supervision + `otbr-monitor` ported to C

- **Unified keepalive supervision.** `s40button` and `linkwatch`, previously started
  fire-and-forget, now run under the `keepalive` supervisor like `otbr-agent` /
  `otbr-monitor`, so any long-lived daemon that crashes is restarted instead of staying
  dead until the next reboot. A shared `etc/init.d/supervise.func` helper
  (`sup_start` / `sup_stop`) — factored from the hardened `S70otbr` stop logic (kill the
  supervisor before the child, then a command-line-matched poll-until-dead with a SIGKILL
  fallback) — replaces the duplicated bookkeeping in `S40button`, `S10network` and
  `S70otbr`.
- **`otbr-monitor` rewritten in C** (`34-Userdata/otbr-monitor/`, v1.0). The OTBR
  housekeeping loop (radio tuning, status LED, dataset sync, once-per-boot SRP recovery)
  was the last long-lived busybox-ash loop in the system — the only process still exposed
  to the intermittent RLX4181 `SIGSEGV`/`SIGILL`/`SIGBUS` fault that retired the s40button
  and inline-monitor shell loops. It is now a foreground C daemon: it reads the REST API
  over a plain TCP socket, tunes the radio and runs the SRP cycle via short-lived `ot-ctl`
  execs, and copies the dataset in-process — no ash is spawned, and its resident set drops
  from ~944 KB to ~288 KB. Behaviour-for-behaviour parity with the script; bench-validated
  across a warm restart and a cold reboot (all services come up supervised, radio/LED/SRP
  correct, no ash faults).

### userdata — `S70otbr` no longer passes `uart-flow-control=false` (discussion #142)

- **Flow-control was force-enabled on boards without RTS/CTS.** OpenThread's radio
  URL parser treats `uart-flow-control` as a **presence flag** (`Url::HasParam()`
  only checks that the name occurs in the query string, per its own usage text:
  "Enable flow control, disabled by default"), so the `uart-flow-control=false`
  that `S70otbr` emitted for `FIRMWARE_FLOW_CTRL=none|sw` still set `CRTSCTS` on
  `/dev/ttyS1`. On a board that does not wire RTS/CTS (the Sengled G4), otbr-agent
  then waited on a dead handshake and exited silently with code 1 before printing
  anything — OT-RCP simply never came up (#142). Root-caused by @hlyi, from the
  shipped rc3 image down to the exact `HasParam` line. `S70otbr` now **omits the
  parameter entirely** for `none|sw` and keeps `&uart-flow-control=true` for `hw`
  (or an absent key), so the Lidl path is byte-for-byte unchanged. The bug never
  shipped in a GA release — it was introduced with the #134 flow-control split and
  only ever affected the v4.0.0 release candidates on non-hw boards.

### Flash tooling — per-board pre-built bootloaders (discussion #140)

- **`boot.bin` is now shipped per board**, in `31-Bootloader/boot-img/<board>/boot.bin` —
  the same layout and selection mechanism as the kernel images. All consumers
  (`build_fullflash.sh`, `create_fullflash.sh`, `flash_install_rtl8196e.sh`,
  `flash_remote.sh bootloader`, `flash_bootloader.sh`) resolve the binary from `BOARD=`
  through the shared `lib/kernel_image.sh` helper, and `build_bootloader.sh` writes only
  into the selected board's slot. This closes the discussion #140 brick scenario both
  ways: a non-lidl full install can no longer bundle the Lidl DRAM bring-up (the old
  single `boot.bin` + stderr warning), and a leftover non-lidl build can no longer land
  on a Lidl gateway, since no shared mutable `boot.bin` exists any more. The Lidl image
  is byte-for-byte the former `31-Bootloader/boot.bin` (V2.9, reproducible build); a
  pre-built `sengled-e39-g8c` image is now committed alongside it. `flash_remote.sh
  bootloader` additionally gained the `/proc/device-tree/model` board-mismatch guard
  that previously covered only the kernel component.

---

## [4.0.0-rc4] - 2026-06-30

_Supersedes `v4.0.0-rc3`. Headline: **issue #99 — the real root cause, fixed.** Two field
units still soft-locked on rc3 (frtz13, olivluca), and their `v6` panic records revised the
diagnosis: not the RUNOUT desync rc1–rc3 targeted (the field signature is `iisr=0x320e` —
RX_DONE set, **RUNOUT clear**), but an **unbounded RX poll loop**. The NAPI poll bounded
itself by packets *delivered*, while every drop/error path re-armed and advanced the ring
cursor without counting — so under a flood of droppable descriptors the poll never returned,
pinning the CPU in softirq (100% softirq / 0% hardirq) until the watchdog rebooted. rc3's
switch-core recovery was structurally blind to it (it gates on RUNOUT, which was clear). rc4
**bounds the poll by descriptors processed**, restoring the vendor SDK's iteration bound the
rewrite had dropped: the poll now provably returns within one NAPI budget, converting the
fatal lockup into a survivable yielding poll. A **RUNOUT-independent stall detector** escalates
a sustained zero-delivery saturating poll to the existing switch-core deep reset. The
post-mortem capture is widened (record **v7**) and now **also logged live at every recovery**,
so a self-heal that never reboots still records what triggered it. Both kernel lines (6.18 and
7.1) in sync. Release candidate, not GA._

### `rtl8196e-eth` v2.18 → v2.19 — issue #99 bounded RX poll + RUNOUT-independent recovery

- **Bounded RX poll (the fix).** `rtl8196e_ring_rx_poll` now counts descriptors *processed*
  (incremented unconditionally every iteration), not packets *delivered*, and returns that to
  NAPI; the packet count is reported separately. The loop terminates in ≤ budget iterations by
  construction, so a droppable-descriptor flood can no longer spin it forever. This restores the
  behaviour of the 2012 vendor driver, which bounds its receive loop by total iterations.
- **RUNOUT-independent stall detector.** A budget-saturating poll that delivered nothing, for
  `rtl8196e_rx_stall_thresh` (default 32) consecutive polls, escalates to the bench-validated
  switch-core deep reset — independent of the RUNOUT status the rc1–rc3 recovery gated on (and
  which the field captures showed clear). Tunable via the module parameter (`0` disables).
- No datapath regression (TCP RX ~94, TX median ~70 Mbit/s, retrans 0). Bench-validated under
  fault injection: the bounded poll absorbs a sustained forced-drop flood (~64 K drops/s) with
  the box staying schedulable (no soft-lockup), the detector escalates to a deep reset, and RX
  returns to line rate afterward.

### Watchdog post-mortem record v6 → v7 + live recovery fingerprint (`rtl819x-wdt` v1.9 → v1.10)

The DRAM panic record is re-focused on the narrowed root cause, and the same fingerprint is now
emitted to the kernel log at each recovery action — so the next field event is diagnosable
whether it self-heals (a log line, no reboot) or rides a hang to the watchdog (the DRAM record):

- **Retired** the RUNOUT-latch in-progress counters (`zero`, `seen`) of the disproved model.
- **Added** the bounded-poll detector state (`poll_budget_hit`, `rx_stall_run`,
  `swcore_deep_reset`); an **A-vs-B discriminator** (the switch-desync counters
  `wild_pkthdr`/`wild_mbuf`/`mbuf_no_shadow`/`skew` vs the runt-flood counter `bad_len`); and
  the switch's own RX descriptor pointers and CPU-port descriptor counter
  (`CPURPDCR0`/`CPURMDCR0`/`P6_DCR0`, real vendor register addresses) cross-checked against the
  driver's ring cursor.
- The eth driver emits the same fingerprint via `netdev_warn` at the four rate-limited deep
  recovery sites (RX-poll stall, TX-done stuck, TX timeout, RUNOUT escalation).

Record size widened to `0x280`; the v5/v6 decoders are retained for the one-boot-after-upgrade
leftover.

## [4.0.0-rc3] - 2026-06-25

_Supersedes `v4.0.0-rc2`. Headline: **issue #99 switch-core self-recovery** (ETHDRV-016,
eth v2.18) — the driver now detects a wedged switch core and performs a full hardware
switch-core reset to recover at runtime instead of riding the lockup to a watchdog reboot,
restoring the deep recovery the vendor SDK shipped and the from-scratch rewrite had
dropped. It pairs with the new **watchdog post-mortem record** (v6): the next field event
either self-heals visibly (a `switch core reset done (#N)` log line) or is captured in the
panic record. rc3 also rolls up the full rc2-development-cycle work that the public
`v4.0.0-rc2` branch predated — **board + kernel-line selection** (four pre-built images),
**bootloader V2.9**, **flash user-file preservation**, **quiet boot**, and the **QA/bench
tooling** (each detailed under `[4.0.0-rc2]` below). Both kernel lines (6.18 and 7.1) are
brought in sync. Release candidate, not GA. (The validated Sengled G4 NCP **radio**
firmware, #130, is tracked in the EFR32 changelog under `2-Zigbee-Radio-Silabs-EFR32/`.)_

### `rtl8196e-eth` v2.15 → v2.18 — issue #99 switch-core self-recovery (ETHDRV-016)

Building on the rc2 detector + ring-resync (ETHDRV-015), three changes reconstruct the
stock firmware's self-healing that our rewrite had omitted:

- **Deep switch-core reset (escalation).** A new process-context worker performs the
  vendor `FullAndSemiReset` sequence (switch-core clock cycle + `FULL_RST`) followed by a
  full re-bring-up, when the cheap in-poll ring resync repeatedly fails to clear a RUNOUT
  storm. It runs from a workqueue (it sleeps ~650 ms), never in the NAPI poll.
- **TX-done hang watchdog.** The 1 s timer now also detects a TX-done descriptor left
  owned by the switch across consecutive checks — a switch-core stall the RX-runout
  detector cannot see — and triggers the deep reset (mirrors the vendor
  `rtl_check_swCore_tx_hang`).
- **Clear-RUNOUT-on-forward-progress.** A budget-saturating poll now clears the RUNOUT
  status as the vendor receive path does, so a transient runout self-clears.

New `ethtool -S` counter `rtl8196e_swcore_deep_reset`. No datapath regression (TCP RX
~94, TX median ~69 Mbit/s, retrans 0). Bench-validated on the lab unit: the deep reset
recovers a live gateway cleanly with no reboot, and both the RUNOUT-storm and TX-hang
paths detect and recover under fault injection.

### Watchdog post-mortem record v4 → v6 — eth #99 state captured at panic

The DRAM-backed panic record (`rtl819x-wdt` v1.7 → v1.9), decoded one boot after a
soft-lockup, now carries an Ethernet `#99` snapshot pulled at panic via a `__weak`
provider in the eth driver, so a recurrence is diagnosable from the field rather than by
guesswork:

- **record v5** — the recovery counters (resyncs / NAPI kicks performed, in-progress
  zero-work and RUNOUT-seen counts) plus the live `CPUIISR`/`CPUIIMR` and the RX ring
  cursor.
- **record v6** — broadened to switch-core / TX-done / ring-progress state
  (`rxdesc`, `txprod`/`txcons`/`txfree`/`txdesc`, `cpuicr`, `sirr`, rx/tx packet counts),
  to tell an RX-runout storm apart from a broader switch-core or TX-done stall.

Persisted to `/userdata/panic/history` by `S26panicrec` (first occurrence after each
clear). rc3 keeps this instrumentation alongside the self-recovery above — so the next
event is either healed (a reset log line) or fully captured.

### Both kernel lines synced (6.18 + 7.1)

The 7.1 overlay (experimental dual-kernel line) had lagged at eth v2.15 / watchdog v1.7;
it is brought to parity — eth **v2.18**, watchdog **v1.9 / record v6** — with identical
drivers across both lines. rc3 ships four images (`{lidl, sengled-e39-g8c} × {6.18,
7.1}`).

---

## [4.0.0-rc2] - 2026-06-19

_Supersedes `v4.0.0-rc1`. Carries the **issue #99 engine fix** (ETHDRV-015,
eth v2.15) after the rc1-line fix (ETHDRV-013) proved insufficient in the field,
the **bootloader fixes (V2.9)** that close the post-flash boot loop on both a
kernel flash (PHY-quiesce) and a 16 MiB full-flash (switch-DMA stop), and the
harmonized init-script output. Field confirmation of #99 is still pending —
release candidate, not GA._

### `rtl8196e-eth` v2.15 — issue #99 engine fix (poll-side RUNOUT-storm recovery)

The v2.14 candidate fix (ETHDRV-013, RX resync inside `tx_timeout`) proved
**insufficient in the field**: a unit running `v3.8.5` (driver v2.7, which already
carries that fix) recurred with the exact #99 signature after ~3.7 days. A full
review (see the driver's `issue99.md`, cross-checked against the original Realtek
SDK) found the real engine: the `PKTHDR_DESC_RUNOUT` storm is **self-sustaining
regardless of how the switch-RX/`rx_idx` desync is entered**, and the NAPI poll has
no escape — a zero-work poll under RUNOUT just re-enables the interrupt against an
unchanged starved ring and the switch re-asserts the next cycle. ETHDRV-013 only
closes one entry (`tx_timeout`); any other entry lands in the same trap. The
original Realtek driver never hits this because it ships a runtime stuck-detector
(`rtl_check_swCore_tx_hang` → `rtl865x_reinitSwitchCore`) that our from-scratch
rewrite dropped.

v2.15 restores that safety net, NAPI-friendly (ETHDRV-015):

* **Poll-side detector (primary).** After 3 consecutive zero-work NAPI polls with
  `PKTHDR_DESC_RUNOUT` asserted, the poll runs a full ring resync
  (`rtl8196e_hw_ring_resync` — the `open()`/`tx_timeout` reset+rearm+TRXRDY
  sequence, now factored out) so the switch RX pointer and `rx_idx` are forced back
  in sync; the `napi_complete` tail then re-enables IRQs against an armed ring and
  the storm cannot restart. Breaks the storm in microseconds. CPUIISR is read only
  on a zero-work poll, so the normal RX path is unaffected.
* **Periodic watchdog (belt-and-suspenders).** A ~1 s timer kicks a poll if RUNOUT
  stays asserted across 3 checks — covering a non-CPU-pinning stall. Off the TX/RX
  datapath (one MMIO read per second), no throughput impact.

Two `ethtool -S` counters expose the recovery firing: `rtl8196e_rx_runout_resync`
and `rtl8196e_rx_runout_kick` (both 0 unless a storm was caught). ETHDRV-013 and
ETHDRV-014 are retained as defence in depth. Candidate pending field confirmation.

### Bootloader V2.9 — stop the switch DMA and quiesce the PHY before kernel handoff

Fixes an intermittent **boot loop after a `flash_remote` kernel flash**: the box
looped in early boot (resetting around the `/sbin/init` handoff, no panic text)
until a physical power cycle, whereas a plain `reboot` or a cold boot was always
fine. The bug is in the bootloader, so it affected both the 6.18 production
kernel and the experimental 7.1 line — not a kernel issue.

Root cause: after a TFTP kernel flash, `autoreboot()` triggered a watchdog reset
without disabling the Ethernet PHY. A watchdog reset preserves DRAM (that is how
the `boothold` flag survives it) and does not fully reset the switch DMA engine,
so right after a ~1.4 MB TFTP transfer the switch could keep DMAing incoming
frames into DRAM across the reset and into early kernel boot — before the
kernel's Ethernet driver resets the MAC — corrupting it. A plain reboot never
tripped it because the link is idle at that point; only a power-on reset cleared
the switch. `autoreboot()` now disables the PHY on all five ports before the
watchdog reset, mirroring the direct-jump-to-kernel path in `monitor.c` that
already did this "to prevent ethernet [from] disturb[ing] Linux kernel booting".

A small timer-independent busy-loop first lets the post-flash UDP `OK`
notification drain out, so the new PHY-disable no longer drops it — otherwise the
flash tools reported a spurious "no notification" on every successful flash. It
is deliberately **not** `delay_ms()`: the preceding SPI flash write can leave the
jiffy timer stopped, which would make `delay_ms()` spin forever and the box never
reboot.

Validated on the bench: 13 consecutive `flash_remote` cycles alternating the 6.18
and 7.1 kernels all booted cleanly with no loop, and the flash tools' "Flash
Write Succeeded" confirmation is restored. The bootloader build stays
reproducible (`B_VERSION` V2.7 → V2.8, pinned `BOOT_CODE_TIME` bumped).

**Follow-up — the same quiesce was missing from the auto-boot handoff.** The loop
recurred in field use because the PHY-disable had been added only to
`autoreboot()` and the manual `J` command, not to `goToLocalStartMode()` — the
path actually taken on every auto-boot. After a `flash_remote` warm reset the
bootloader re-enables the PHY for its own TFTP, then `goToLocalStartMode()` jumped
to the kernel with the PHY still live, reopening the same DMA-corruption window
during early kernel boot. Added the identical five-port `EnablePHYIf` clear before
the kernel jump in `goToLocalStartMode()`, symmetric with `J` and `autoreboot()`.
Re-validated: 10 consecutive `flash_remote` kernel cycles plus 3 manual reboots,
all clean on the serial console (previously reproducible within a couple of
cycles). Folded into V2.8 (still unreleased); `BOOT_CODE_TIME` refreshed.

**Follow-up — a 16 MiB full-flash needed more than the PHY-off (V2.8 → V2.9).**
Disabling the PHY stops *new* ingress but not a CPU-port DMA that is already
armed, and a full `flash_install` (16 MiB over TFTP) leaves far more in flight
than the ~1.4 MB kernel flash V2.8 was validated against — so the switch kept
DMAing into DRAM through the handoff and the box looped until a cold power cycle
(a warm `reboot` and a single-partition `flash_remote` were unaffected). V2.9
adds, before the PHY-off in both `autoreboot()` and `goToLocalStartMode()`, a
`CPUICR = 0` (clears the CPU-port `TXCMD`/`RXCMD` DMA enables) and a
`FullAndSemiReset()` — the same switch-core reset `swCore_init()` runs on every
cold boot, which aborts any in-flight DMA. The reset re-defaults the port
registers, so the PHY-off deliberately stays *after* it. Validated on the bench:
three consecutive 16 MiB `flash_install` full-flash cycles each auto-rebooted
straight to userspace with no manual power cycle (previously reproducible on the
first full-flash). Build stays reproducible (`B_VERSION` V2.8 → V2.9, pinned
`BOOT_CODE_TIME` refreshed). Caveat: a gateway still running a pre-V2.9
bootloader runs its *old* `autoreboot()` for the very first full-flash, so that
one upgrade may still loop once — clear it with a cold power cycle (unplug/replug,
not a warm reboot); every flash after V2.9 is in place boots clean on its own.

### Init-script output — one consistent, sober convention

The per-service init scripts now print a single uniform `<service>: <state>` line
(no emoji, no redundant per-script self-prefix), with warnings and errors as
`<service>: WARNING/ERROR …`. The boot runner prints each script's basename; the
previously silent watchdog now emits concise `armed`/`stopped` lines, while
`S26panicrec` and `S90checkpasswd` stay quiet on a normal boot. Shutdown is now
symmetric with boot: `rcK` frames the stop sequence with a
`===== Stopping userdata services =====` header and a closing
`Userdata services stopped` line, and a leading blank line keeps both sequences
off the login prompt. The dead `34-Userdata/…/init.d/rcS` is removed — it was
never executed (the bootstrap runs the rootfs `rcS` at sysinit and the userdata
`rcK` at shutdown; the `S??*` glob it iterates never matches `rcS`). Cosmetic
only; no service behaviour changes.

### Rootfs — quiet the console at the end of boot

The kernel's `random: crng init done` notice (KERN_NOTICE, level 5) lands ~11 s
into boot on this low-entropy SoC — just after getty prints the `zigbeegw login:`
prompt, so it trailed the prompt on the serial console. At the very end of `rcS`
(after the userdata init loop, so klogd is already draining the kernel ring buffer
into `/var/log/messages`, and right before getty), `console_loglevel` is lowered to
5: level-5 notices no longer reach the console while warnings/errors (level < 5)
still do, and `/var/log/messages` keeps everything. Writing one value to
`/proc/sys/kernel/printk` touches only `console_loglevel` (7 4 1 4 → 5 4 1 4).
Cosmetic; no service behaviour changes.

### Board + kernel selection — pick `BOARD` and `KERNEL`, four pre-built images

Every flash and build script now accepts two environment variables, both defaulting to
the historical Lidl 6.18 build so **nothing changes for a Lidl user**:

* `BOARD` — `lidl` (default) or `sengled-e39-g8c` (Sengled Smart Hub G4).
* `KERNEL` — `6.18` (default) or `7.1`.

The Linux **7.1 line** is now shipped alongside 6.18 (`patches-7.1/`, `files-7.1/`,
`config-7.1-realtek.txt`); `build_kernel.sh` builds it with `KERNEL=7.1`. Pre-built
images moved under `32-Kernel/kernel-img/<board>/kernel-<line>.img` — four shipped
images (`{lidl, sengled-e39-g8c}` × `{6.18, 7.1}`). The flash scripts
(`flash_kernel.sh`, `flash_remote.sh`, `build_fullflash.sh`, `create_fullflash.sh`,
`flash_install_rtl8196e.sh`) resolve the image from `BOARD`/`KERNEL` through a shared
helper (`lib/kernel_image.sh`); an explicit `--image` still wins (flash_remote.sh
first gained a kernel `--image` passthrough, now generalised to `BOARD`/`KERNEL`).
`flash_remote.sh` and `flash_install_rtl8196e.sh` (upgrade path) read the gateway's
`/proc/device-tree/model` and refuse a board-mismatched flash unless `--force`; a
full install additionally warns when `BOARD` is non-default that the bundled
bootloader (`31-Bootloader/boot.bin`) must be built for the same board, since its
DRAM config is per-board (a mismatch bricks the gateway). The 6.18 production line
is byte-for-byte unchanged.

### Flash — preserve user-added files across a userdata reflash

`flash_install_rtl8196e.sh` (upgrade path) and `flash_remote.sh userdata` already re-inject
the saved config into the rebuilt `userdata.bin`; they now also carry over **anything the
user added under `/userdata`** — a custom program in `usr/bin`, a hand-pushed
`iperf3`/`ethtool`, a script, or a whole new directory (subdirectories and empty dirs
included). The rule: every path the gateway has under `/userdata` that the fresh skeleton
does not ship is preserved (files, symlinks and directories, with the executable bit kept);
skeleton-shipped paths come from the new image (its version wins, never shadowed by the
gateway's old copy), and *edits* to shipped files keep the shipped version — curated config
edits are still re-injected by the separate save list. A shared
`lib/ssh.sh:preserve_user_additions` does it best-effort over the existing SSH session
(BusyBox-safe enumeration via `tar`, run after the config save so config is excluded
automatically). First-flash-from-bootloader and a bare `34-Userdata/flash_userdata.sh` are
unchanged — the latter still does a clean wipe.

### Developer & QA tooling

- **Functional test harnesses** (`32-Kernel/scripts/test_{leds,button,watchdog}.sh`):
  host-side scripts that drive each peripheral over SSH and verify it. LEDs walks the
  status/LAN LEDs through ON/DIM/OFF with software read-back; the button harness fakes
  a press via `devmem` (no physical contact) and checks short-press, long-press →
  `recover_efr32`, and the #131 LED restore; the watchdog harness validates the armed
  state and, opt-in, a real panic → record → reboot → re-arm cycle.
- **Portable per-release iperf3 bench** (`bench_release_iperf3.sh`): RX/TX with
  inter-session medians and no `ethtool` dependency, for reproducible release gating.
- **Cross-version TX comparison** (`bench_history_sweep.sh`): flashes a set of releases
  in drift-cancelling randomized rounds (arming the bootloader via `devmem`, so it works
  on early DTBs that predate the boothold node) to compare TCP-TX across versions without
  session/thermal bias.
- Prebuilt `iperf3` and `ethtool` MIPS binaries are committed next to their build scripts
  so benching needs no rebuild (they are not part of the shipped userdata image).

---

## [4.0.0-rc1] - 2026-06-15

_Supersedes `v4.0.0-rc0`: the same issue #99 candidate fix, plus a fix for a TX
throughput regression rc0 introduced. If you're testing #99, this is the build
to use._

### `rtl8196e-eth` — restore TX throughput (TX-reclaim timer arm guard)

The software TX-reclaim timer added for #99 was armed unconditionally from the TX
hot path (`start_xmit` and the NAPI poll re-arm), re-inserting the timer per packet
under load — about 5% off TCP TX on the RLX4181 (≈67.6 vs ≈71 Mbit/s; RX
unaffected). It now arms only when not already pending: it still fires within one
timer window to break a no-RX stall, but is free once armed. TCP TX is back to
≈70.8 Mbit/s and an idle border router still shows zero TX timeouts. Driver version
unchanged (v2.14, same release cycle).

Bench confirmation on the rc1 build (OTBR stopped, direct Cat-6 to a Gigabit
host): TCP RX 93.6 Mbit/s, TCP TX 70.3 Mbit/s (5-rep median, range 69.1–72.3) —
back inside the historical 69.3–72.8 TX spread, no regression. Stress (300 s
single-stream RX): 94.0 Mbit/s sustained, 0.00 % retransmits; eth0 rx/tx errors
and drops all zero. Full per-gate detail in the driver's `PERFORMANCE.md`.

---

## [4.0.0-rc0] - 2026-06-15

_Release candidate over `v4.0.0-pre`: one focused change — the **candidate fix
for issue #99**, the long-running soft-lockup hang reported on units running an
OpenThread Border Router. The hang was reproduced on the bench and traced to the
Ethernet driver's TX-timeout recovery; both the storm and the condition that
triggers it are addressed. `v4.0.0-pre` is left unchanged. Field confirmation
that the hangs stop is still pending — hence a release candidate, not GA._

### `rtl8196e-eth` v2.14 — issue #99 candidate fix (TX-timeout RX resync + software TX reclaim)

Two changes to the Ethernet driver, validated on the bench; field confirmation
pending.

* **RX resync in `ndo_tx_timeout`.** The watchdog recovery rebuilt only the TX
  ring; its `hw_stop()`/`hw_start()` cycles the switch's RX engine back to
  descriptor 0 while the driver's `rx_idx` stays put — a desync that leaves the
  switch with no usable RX descriptors. The switch then asserts
  `PKTHDR_DESC_RUNOUT` continuously; `napi_complete` clears it and the switch
  re-asserts it the next cycle — a ~100 k/s spurious interrupt storm with zero
  forward progress that pins the CPU in `__napi_poll` until the hardware
  watchdog resets the SoC. That is the issue #99 soft-lockup. The recovery now
  also resets and reprograms the RX ring (`ring_rx_reset` + `hw_set_rx_rings`),
  symmetric with `open()` and `stop()`. The bug is old (present unchanged since
  at least v2.6); the BQL work in v4.0.0-pre lowers the queue-stop threshold,
  which makes the triggering TX timeout fire readily and turned the bench into a
  reliable reproducer.
* **Software TX-reclaim timer (the trigger).** TX reclaim runs only in
  `start_xmit` and the RX-driven NAPI poll, so a TX queue that stops (ring-full
  or BQL byte-limit) while no RX arrives has no path to reclaim and waits for the
  10 s netdev watchdog — e.g. a Thread border router idling with no paired
  device, the configuration the soak reporters run. A short timer, armed when the
  queue stops and lapsing once it drains, kicks a NAPI reclaim so the queue
  recovers without RX. This removes the spurious timeouts — and with them the
  path into the storm.

Bench result: a border router idling alone (zero RX) previously fired a TX
timeout every 10 s, then desynced and stormed into the #99 soft-lockup; with
both changes it fires none — no timeout, no storm — and SSH stays responsive.
The storm signature matches the field #99 captures, so this is the strongest
candidate yet; confirmation that it stops the field hangs is pending.

---

## [4.0.0-pre] - 2026-06-13

_Folds the `v3.11.0-pre` beta into v4.0.0. Two headlines: the **Sengled
Smart Hub G4 (E39-G8C)** board port — the firmware's first port to a
second board, contributed and hardware-validated by @hlyi — and a
per-driver hardening pass across the whole Linux 6.18 kernel tree. Two
userdata fixes (#131, #132) ride along. Everything is board-agnostic and
was validated on the Lidl bench; currently on the `v4.0.0-pre` branch for
beta testing._

### Sengled Smart Hub G4 (E39-G8C) — a second supported board

The platform is no longer Lidl-only. Porting to an RTL8196E twin now means
contributing per-board data files instead of patching the tree:

* **Bootloader `BOARD=`** (#126): a board contributes a single
  `31-Bootloader/boards/<board>/board.h` packaging its DRAM bring-up, RAM
  banner and boothold-page constants; `BOARD=<board> ./build_bootloader.sh`
  selects it (default `lidl`, which still rebuilds bit-for-bit identical).
* **The Sengled E39-G8C board** (64 MB DDR2): its `board.h` was contributed
  and validated on real hardware by **@hlyi** (#127, #128), who also
  reduced the front-panel bootloader-mode pin hold from 5 s to 1 s.
* **Hardware teardown page** for the G4 (`0-Hardware/sengled-e39-g8c/`) —
  also **@hlyi** (#133).
* **Board-portable LAN LED** (#126): the Ethernet driver learned a
  `realtek,led-pads` devicetree property (eth v2.8), so the LAN LED maps to
  the correct switch pad on either board (the Lidl and the G4 wire it
  differently).

This pairs with the kernel `BOARD=` devicetree selection (shipped in 3.10.0)
and the EFR32 radio-firmware `BOARD=` support (see the radio changelog,
#130) so all three build stages are board-aware.

An out-of-band audit produced an `AUDIT.md` / `DESIGN.md` pair for every
RTL8196E kernel driver; the findings were implemented and each driver
re-validated on the bench before the kernel image (`kernel-6.18.img`) was
cut. No on-device behaviour regresses and the Ethernet throughput
envelope is unchanged (RX ~94 Mbit/s, TX within the established layout
spread, single-stream retransmits 0).

### `rtl8196e-eth` v2.13 — BQL, a real DT resource model, audit fixes

Five bench-gated steps on top of the v2.8 LAN-LED work:

* **v2.9** — probe/teardown robustness: the uncached ring alias is
  flush-and-discarded before first use (a dirty cached line could
  otherwise evict over a live descriptor); `stop()` disables NAPI before
  masking, so a finishing poll cannot re-arm the interrupt mask; probe
  quiesces the IRQ mask/status the bootloader's TFTP path can leave
  latched before `request_irq()`; `ndo_change_mtu` is `-EBUSY` while up.
* **v2.10** — the first-packet/timer debug scaffolding is retired (~190
  fewer lines), along with dead defines and the unused `tx_submit` flags.
* **v2.11** — the one-time SoC bring-up (pinmux, board pad state, switch
  clock, `FULL_RST`, LED controller, ~650 ms of `msleep`s) is hoisted to
  probe; `ndo_open` now does only the volatile per-open programming and
  measures ~30 ms instead of >1 s.
* **v2.12** — the ethernet node declares its three register windows and
  probe claims and maps all three, failing the probe loudly if a mapped
  base ever diverges from the compile-time KSEG1 constants (kept, for the
  hot path) — DT drift and conflicts can no longer corrupt MMIO silently;
  `/proc/iomem` now shows three named windows.
* **v2.13** — Byte Queue Limits on the TX queue (the one performance idea
  the audit left open), landed without the throughput cost the driver's
  earlier pointer-routing experiments warned about.

### `rtl819x_wdt` v1.7 — panic-safe semantics + panic record v4 (issue #99)

The panic notifier reads the record uptime with the NMI-safe
`ktime_get_boot_fast_ns()` instead of a seqcount-retrying accessor that
could spin forever if the panic interrupted a timekeeping writer — and it
sat before the chip-arm writes, so a spin would have cost both the crash
record and the fast reset. Userspace-visible cleanups: `WDIOC_GETTIMELEFT`
now honestly returns `EOPNOTSUPP` (it used to report the constant timeout)
and the bogus `timeleft` sysfs attribute is gone; the fixed ~671 s
hardware window is declared as `max_hw_heartbeat_ms`, so the core bridges
longer software timeouts with worker pings (the 60 s feeder cadence is
unchanged). The kick is now a constant write, dropping one MMIO read per
kick.

Panic record **v4** (issue #99) extends the post-mortem with the NET_RX side of
the soft-lockup storm, which the timer candidate lists structurally cannot name.
The v3 field captures showed the timer wheel is only a victim (`overdue` saturates
at the detection window while `pending` stays at the normal idle count = frozen,
not flooded), with the co-pending vector being NET_RX. Record v4 adds, read
straight from kernel counters at panic: per-softirq run counts
(`kstat_softirqs_cpu`), the total hardirq count (`kstat_cpu_irqs_sum`), and the
NAPI poll-list (`softnet_data.poll_list`) — the last naming the driver (the
rtl8196e Ethernet `poll`) whose NAPI is scheduled at the hang. All cold-path and
entirely within the watchdog driver (`softnet_data` is a per-CPU export, so no
kernel patch); v2/v3 records still decode across an upgrade, and the probe banner
prints `record v4`.

### `8250_rtl819x` v1.4 — FIFO on, RX trigger pinned to 1

The clone's region-claim quirk had left `ttyS1` running with `FCR=0` (FIFO
off, 16450 char mode) since the early bring-up; the 8250 core now claims
the window itself and the FIFOs are enabled. An A/B loopback bench showed
the clone's trigger levels above 1 overrun non-monotonically, so the RX
FIFO trigger is pinned to 1 — wire-identical to the proven v3.x envelope,
full 16-byte latency cushion (`rx_trig_bytes` stays writable for
experiments). Flow-control gating now tracks the absolute `CRTSCTS` state
instead of edge transitions.

### `timer-rtl819x` (clocksource) v1.2 — timer_of, a DT overlap closed

Converted to the `timer_of` helper (base, refclk, IRQ); the timer's DT
`reg` window is shrunk from 0x20 to 0x1c so it no longer overlaps the
watchdog's `WDTCNR` register. An IRQ-of-parse failure now panics like the
other init-failure paths instead of booting into a clockevent-less system
that hangs later in scheduler bring-up with nothing pointing at the cause.

### `gpio-rtl819x` v1.2 — generic MMIO core, loud failure without syscon

Re-based on the kernel's generic MMIO GPIO core, keeping only the
irreducible custom part (request-time `PIN_MUX_SEL_2` pinmux + CNR); the
glitch-free DATA-before-DIR ordering the nRST open-drain emulation relies
on is preserved and recorded as a design invariant. A request on a
mux-requiring line with the syscon absent now fails with `-ENODEV` and a
clear error instead of silently leaving the pad in peripheral mode (a dead
LED/button whose only trace was one probe-time warning).

### `leds-gpio-pwm` v1.1 — no timer at the rails, keep-state that keeps

The `default-state = "keep"` path now reads the line back and re-drives it
at that level instead of destroying the state it was meant to keep; the
0 % and 100 % duty bands are steady GPIO levels with no timer (a 250 Hz
timer used to run at full brightness); the initial timer arm follows the
#120 timer-wheel rule (`jiffies`, not `jiffies+1`).

### `spi-rtl819x` v1.1 — electrically correct CS, capabilities declared

Chip-select deselect now parks all lines high instead of actively driving
the sibling CS low (dormant on the Lidl board, but wrong electrically);
`mode_bits` / `bits_per_word_mask` declare only what the hardware supports,
so the core rejects the rest; a sub-12.5 MHz clock request that falls back
to the next divisor warns once. Storage gate passed on the bench: squashfs
boot and a 4 MB JFFS2 write/re-read byte-identical before and after.

### `rtl8196e-uart-bridge` v1.4 — documented locking, rate-limited churn

The remote-triggerable connection-lifecycle messages (connect, replace,
disconnect) are rate-limited; the disarm/relisten window's dependency on
the builtin param lock is documented at the lock definition and the unlock
site for any future config entry point; the Kconfig help now covers
`blmode_pulse` / `blmode_gpio`. No data-path change.

### `rtl8196e-uart-bridge` v1.5 — split flow-control capability from firmware mode (discussion #134)

The devicetree `radio-bridge/flow-control = "hw"|"sw"|"none"` conflated two
unrelated facts: whether a board physically wires the EFR32 UART's RTS/CTS
(a hardware truth) and which mode a given radio firmware wants (a runtime
choice). On the Lidl board both are "hw", so the conflation was invisible;
on the Sengled G4, which does not wire RTS/CTS, "sw" really encoded "not
hw-capable", so changing the radio firmware meant rebuilding the DTB. v1.5
splits the two:

* **Board capability → devicetree.** A new boolean `realtek,hw-flow-control`
  on the `/radio-bridge` node declares that RTS/CTS is wired. Present on the
  Lidl board, omitted on the G4. It seeds the `flow_control` default
  (present → `hw`, absent → `sw`) and acts as a **ceiling**: an `hw` request
  from sysfs or the init scripts is clamped to `sw` on a board without the
  boolean, so CRTSCTS is never asserted on an unwired UART. The old DT
  `flow-control` string is dropped entirely (unreleased binding, both
  in-tree DTS converted in lockstep — no shim).
* **Firmware mode → radio.conf.** A new optional `FIRMWARE_FLOW_CTRL=none|sw|hw`
  key (parallel to `FIRMWARE_BAUD`) selects the mode at runtime, applied to
  the sysfs `flow_control` knob by `S50uart_bridge` (Zigbee) and reflected in
  the `otbr-agent` `uart-flow-control` URL by `S70otbr` (Thread). Absent ⇒
  the devicetree per-board default stands, so **no existing `radio.conf`
  needs to change** on either board.
* **`flash_efr32.sh`** now accepts a non-zero `flow_control` readback when
  re-enabling flow control after a flash (1 on a wired board, 2/`sw` on an
  unwired one), instead of asserting exactly `1` — which would have aborted
  the flash on a not-capable board. The off-state checks (`0`, required for
  the Gecko Bootloader Xmodem path) are unchanged.

The runtime sysfs `flow_control` 0/1/2 numeric ABI is unchanged.

### `irq-rtl819x` (irqchip) v1.0 — error-path cleanup

The no-parent error path now removes the IRQ domain and NULLs the base
after `iounmap`; the SPDX header and kernel-style indentation are applied,
and the swapped parent labels in the chained-handler comment (IP3 = Switch,
IP4 = UART1) are corrected. No functional change on the success path.

### `s40button` v2.1 — preserve the status LED across a button press (issue #131)

A button press used to switch the STATUS LED off even when it was lit before
the press. The daemon snapshotted the LED brightness once at startup (when it
defaults to off) and re-applied that stale snapshot on release. It now captures
the brightness at the moment the press is confirmed and restores *that* — so a
press leaves the LED exactly as it found it.

### `linkwatch` — re-acquire DHCP after a link change (issue #132)

In DHCP mode the gateway never asked for a new address after the cable was
moved to a different subnet: busybox `udhcpc` is started once at boot and, once
bound, never re-DISCOVERs on its own, so it sat on the stale lease until the
lease timers expired. A new tiny static-C daemon, `linkwatch`, watches
`/sys/class/net/eth0/carrier` and, on a down→up transition, pokes `udhcpc`
(SIGUSR2 release + SIGUSR1 discover) so it re-acquires on whatever network is
now present. It runs in **DHCP mode only** — static `/userdata/etc/eth0.conf`
configurations are untouched. Written in C like `keepalive`/`s40button` so the
long-lived poll loop never runs busybox ash (issue #109 fault class).

### Kernel build — malformed `drivers-gpio-Kconfig.patch` fixed (issues #136/#137)

A from-clean kernel build aborted at the patch step with `malformed patch at
line 15`. The hunk header in `patches-6.18/drivers-gpio-Kconfig.patch` declared
`@@ -598,6 +598,11 @@` but its body adds six lines (the `GPIO_RTL819X` block
plus a spacer), so the real new-count is twelve — `patch(1)` ran past the
declared count and bailed. The off-by-one was introduced when `select
GPIO_GENERIC` was added to the patch during the gpio v1.2 audit without bumping
the header count; it stayed invisible because incremental builds reuse the
already-patched kernel tree and never re-run `patch(1)`, so only a build from
clean re-exercises the file. Reported and fixed by **@hlyi** (#137): header
corrected to `-599,6 +599,12` and a stray trailing-whitespace spacer dropped.
**No change to `kernel-6.18.img`** — the shipped image was built from the
already-correct tree, and the fix only affects the patch text (the resulting
`drivers/gpio/Kconfig` is identical bar one ignored blank-line whitespace).

### Kernel build — from-clean patch lint guard

To keep that class of bug from reaching anyone again, `32-Kernel/lint_patches.sh`
replays the build's patch step against a freshly downloaded pristine kernel with
`--dry-run` (same flags and order as `build_kernel.sh`, kernel version read from
it as the single source of truth), and a `kernel-patches.yml` GitHub Actions
workflow runs it on every push/PR touching the patch set — including PRs against
the `v*-pre` branches, where contributors build from clean. It catches malformed
hunks, rejects, and context drift before they merge.

### Flash tooling — safe TFTP upload retry (discussion #135)

A 16 MiB `fullflash` TFTP upload — and the per-partition kernel/rootfs/userdata/
bootloader uploads — could give up on a single stalled block mid-transfer
(tftp-hpa's own per-block timeout) even when the bootloader's TFTP server was
healthy, dropping the user at the bootloader prompt (reported by @MaxRower). The
upload now retries automatically, but safely: after a timeout it re-probes the
bootloader's TFTP server and re-sends only if it is still idle (nothing landed).
If the server has gone quiet — busy writing flash, i.e. a lost final ACK that
merely looked like a timeout — it does **not** re-send, so a retry can never
collide with an in-progress auto-flash; it falls through to the existing write
confirmation instead. The logic lives once in `lib/flash_tftp.sh`
(`probe_tftp_wrq` + `tftp_put_safe`), shared by `flash_install_rtl8196e.sh` and
the four per-partition `flash_*.sh` scripts.

---

## [3.10.0] - 2026-06-11

_Kernel + userdata + host-tooling changes. No bootloader or rootfs change —
existing installs upgrade with `flash_install_rtl8196e.sh` (your config is
preserved); the flashing fixes arrive with a plain `git pull`. Issue #99
soak boxes should stay on v3.8.3 (frozen baseline)._

### Device tree now describes the board wiring (discussions #122/#123/#124)

Board facts move out of code and into the DTS, so porting to an RTL8196E
twin (e.g. Sengled G4, discussion #119) no longer means patching drivers:

* `gpio-line-names` on the gpio controller node names the SoC lines
  (`reset-button` at line 9, `status-led` at 11, `efr32-nrst` at 12 on the
  Lidl board).
* New optional `/radio-bridge` node (`compatible =
  "realtek,rtl8196e-uart-bridge"`) seeds the bridge defaults at boot:
  `nrst-gpios` and `flow-control = "hw" | "sw" | "none"`. Module
  parameters and runtime sysfs still override (DT < cmdline < sysfs).
* One dtb per board: the devicetree Kconfig choice gains an add-a-board
  recipe, the dts Makefile moves from a patch to the `files-6.18/`
  overlay, and `BOARD=<board> ./build_kernel.sh` selects the board
  (default `lidl`).

### `rtl8196e-uart-bridge` v1.2 — software flow control (discussion #123)

`flow_control` is now tri-state: `0`/`none`, `1`/`hw` (RTS/CTS, default),
`2`/`sw` (XON/XOFF). In sw mode the bridge itself scans the radio→host
stream for bare XON/XOFF, strips them from the TCP stream, and gates the
TCP→UART direction (pause on XOFF, resume on XON, bounded 1 s fail-open) —
the hot path bypasses the tty line discipline, so termios IXON/IXOFF would
be inert. Remote hosts (Z2M over TCP) see a clean ASH stream and need no
software-flow support of their own. New `xoff` / `xon` /
`tx_pause_timeouts` counters in `parameters/stats`. Sysfs readback stays
numeric, so existing tooling (`flash_efr32.sh`) is unaffected. During an
EFR32 flash, flow control must be `0` in all modes (Xmodem payloads
contain raw XON/XOFF bytes).

### `s40button` v2 — button read through the GPIO cdev (discussion #122)

The front-panel button daemon no longer touches GPIO registers via
`/dev/mem`: it claims its line through `/dev/gpiochip0` (uAPI v2 ioctls,
no libgpiod, still a small static binary). The line is found by DTS name
(`reset-button`), with a fallback to line 9 and a `-p <line>` override for
bring-up. Claiming through the kernel also applies the PIN_MUX_SEL_2
pad-mux for pads B2–B6 in the gpio driver's request hook — which is what
kept a button on pad B6 (the G4) dead under v1. Press logic unchanged
(100 ms poll, 5 s long-press → `recover_efr32 -q`).

### `S20time` — ntpd now runs as a daemon (issue #125)

The old script ran a one-shot `ntpd -q` behind a ping gate and exited for
good if the network was not up at boot; with no RTC the clock then stayed
at the 1970 epoch for the whole uptime. `ntpd` now starts unconditionally
as a daemon: it retries forever (DNS failures included, backoff capped at
~4 min), steps the clock whenever internet appears, and keeps disciplining
it afterwards — no more long-term drift on month-long uptimes. Measured
cost on the gateway: ~100–200 kB private RSS, ~0.03 % CPU in its busiest
phase, one 48-byte UDP exchange per poll interval (32 s → ~1.1 h).

### `flash_install_rtl8196e.sh` — no more false "manual flash required" (discussion #115)

After `boothold`, the script accepted a bare ARP reply as "bootloader
detected" — but a shutting-down Linux keeps answering ARP for a few
seconds after SSH dies, so everything downstream could run against a
rebooting box: the ICMP classification called a V2.7 bootloader "Tuya /
pre-v2" (false "manual flash required" on a flash that succeeded), or the
16 MiB upload sat in a 5-minute timeout against nothing.

* Detection now requires the bootloader's TFTP server to ACK a 1-byte
  write probe, not just ARP.
* The boothold path skips ICMP classification entirely — it reached the
  bootloader through `boothold`, so "Tuya" is impossible by construction.
* Auto-flash confirmation is dual-channel: the bootloader's UDP:9999
  notification (lost to host firewalls or netcat variants) OR the gateway
  coming back up on SSH at its known address. A firewall can no longer
  turn a successful flash into a scary message.
* Failure paths now state explicitly that nothing was written, and that
  pre-V2.7 bootloaders ignore the `--boot-ip` handoff (they always come
  up at 192.168.1.6).

### Documentation

READMEs synced with the changes above: the uart-bridge pages (tri-state
flow control, `/radio-bridge` device-tree seeding, new stats counters),
the kernel build page (`BOARD=` board selection and the add-a-board
recipe), the userdata page (bridge sysfs table, ntpd daemon), and the
EFR32 flashing page (three-mode flow-control table). The root README now
presents the project as a platform portable to other RTL8196E-based
gateways, and leads the RCP/OT-RCP firmware choices with what they
actually buy you: EmberZNet 8.2 host-side (cpcd + zigbeed) and
Zigbee2MQTT's ZigBee-on-Host (`zoh`) adapter.

---

## [3.9.0] - 2026-06-10

_Kernel-only changes: Linux 6.18.35 rebase, release-stamped `uname -r`, and
the EFR32 nRST pulse rework. No bootloader, rootfs or userdata change —
existing installs upgrade with a kernel reflash. Issue #99 soak boxes should
stay on v3.8.3 (frozen baseline — the capture instrumentation is identical,
and the rebase changes the timer core under test)._

### Linux 6.18.24 → 6.18.35

The SysRq dispatch series we submitted upstream (serial core guard +
8250/8250_dw IRQ-path dispatch, Reviewed-by Ilpo Järvinen) is part of
6.18.35, so the three provisional patches are dropped from
`patches-6.18/`. Two context-drifted patches refreshed to offset 0; the
remaining 51 apply unchanged. `build_kernel.sh` now aborts loudly on a
rejected or fuzzed hunk instead of swallowing it, and iperf3 confirms
no throughput regression on the rebase (RX 93.8 / TX 69.9 Mbit/s).

### Kernel self-identifies its firmware release (issue #120)

`build_kernel.sh` appends the firmware release from `VERSION` to the
kernel localversion: `uname -r` now reads `6.18.35-rtl8196e-v3.9.0`.
After a kernel-only reflash the running kernel names its release even
though `/userdata/etc/version` still describes the (unchanged) userdata
partition — the mixed state issue #120 found confusing is now visible
and accurate.

### `rtl8196e-uart-bridge` v1.1 — nRST pulse reworked to a single open-drain GPIO (discussion #121)

`nrst_pulse` used to set PIN_MUX_SEL_2 bits {7,10,13} — three separate
pin-mux fields copied from the chip's reset-default value — although the
EFR32 nRST is wired to a single pad. Per-pad bench isolation showed only
bit 7 (pad B4 = gpio-rtl819x line 12) resets the radio; the other two
fields just re-routed unrelated pads during every pulse.

* The pulse now claims that one line through the gpiod consumer API with
  open-drain semantics: assert drives the pad low, release floats it back
  to input and the EFR32's internal RESETn pull-up releases the chip.
* New `nrst_gpio` parameter (default 12) so ports to RTL8196E twins with
  different nRST routing select their line at runtime instead of patching
  the driver.
* Sysfs interface unchanged (`echo 1 > .../nrst_pulse`); `recover_efr32`
  and `flash_efr32.sh` work as before.

---

## [3.8.3] - 2026-06-10

_Kernel-only changes: status-LED dimming fix and watchdog post-mortem v3
(issue #99 instrumentation). No bootloader, rootfs or userdata change._

### `leds-gpio-pwm` — status LED flickered at 31 Hz instead of dimming (issue #120)

Since the Linux 4.8 timer-wheel rework the kernel rounds every timer expiry
up by one jiffy so a timer can never fire early: the driver's per-jiffy
re-arm at `jiffies + 1` actually fired every 2 ticks, doubling the PWM step
to 8 ms and halving the PWM frequency to the 31 Hz flicker scoped in issue
#120. Re-arming at `jiffies` (expire ASAP — bucketed at the next tick by
construction) restores the designed 62.5 Hz.

* Bench (GPIO 11 sampler, brightness 60/128/192): 31.3–31.5 Hz before,
  62.5 Hz after (4/12, 8/8, 12/4 ms).
* The sysfs scale stays 0–255; the 4-level duty quantization (25/50/75/100 %)
  and the frequency-vs-resolution trade-off are now documented in the driver
  header.

### `rtl819x-wdt` v1.4 — panic-path arm race fixed, panic record v3 (issue #99)

* **v1.3 regression fixed — candidate lists were silently lost.** v1.3 armed
  the recovery reset before the best-effort wheel walks with a single
  `WDTCNR=0` write, assuming a ~1.31 s grace window. With the userspace
  kicker active the up-counter sits far above the OVSEL=0 threshold and the
  chip resets *instantly* — DRAM breadcrumbs on the bench showed not one
  instruction executing after the arm write, so every v3.8.1 field capture
  would have come back `timers=[none]`. The arm is now two writes: clear the
  counter while the watchdog is halted (no race), then enable.
* **`delayed_work` candidates resolved to their work function.** The first
  issue #99 record-v2 field capture (2026-06-09) returned four
  indistinguishable `delayed_work_timer_fn` wrappers;
  `timer_collect_pending_fns()` now resolves them one level deeper (e.g.
  `neigh_managed_work`).
* **Wheel backlog captured — record v3.** New fields `overdue=` (jiffies the
  earliest queued timer is past expiry) and `pending=` (total queued timers)
  discriminate the two storm shapes behind #99: a wheel that never catches
  up (death spiral — large overdue) vs a softirq re-raised over a caught-up
  wheel (overdue ≈ 0). Bench: `overdue=0j` on a healthy crash,
  `overdue=6269j` (25.1 s of wheel starvation) under an injected soft-lockup.
* A leftover v2 record from a pre-upgrade crash still decodes on the one
  upgrade boot.

## [3.8.2] - 2026-06-09

_Host-side flashing-tooling and UX release. No on-device firmware behavior
change — the kernel and bootloader binaries are identical to v3.8.1; only the
flashing scripts and the version banner differ._

### `flash_install_rtl8196e.sh` — build the image before boothold on the upgrade path

On the auto (boothold) upgrade path the 16 MiB `fullflash.bin` was assembled
only *after* the gateway had warm-rebooted into the bootloader, so on slow
hosts (e.g. a Raspberry Pi 4) the gateway sat idle in download mode for the
whole multi-minute build. The image depends only on the config snapshotted
over SSH into `SKELETON_DIR` *before* boothold — no bootloader state — so the
build can run while Linux is still up.

The build + size check + final WARNING/Proceed confirmation are factored into
`build_image_and_confirm()` (guarded by `IMAGE_READY`). On the auto path it is
called right after `require_boot_l2` and **before** boothold; at the
convergence point it runs only if not already done, so the first-flash /
manual path (gateway already in the bootloader, interactive IP/radio prompts)
is unchanged.

* **Auto path:** only the TFTP upload + flash write now block the gateway, not
  the build.
* **Safer abort:** declining the confirmation (or a build failure) leaves Linux
  intact instead of stranding the gateway in the bootloader.
* **First-flash / Tuya / manual paths:** behaviour identical.
* **ICMP capability probe now polls (~10 s) instead of a single ping.** With the
  build no longer sitting between bootloader detection and the probe, the custom
  V2.5 bootloader's ICMP responder may not be up yet for the first second or two;
  a single ping flaked to "no auto-flash" and printed manual FLW guidance even
  though the auto-flash succeeded. A genuine Tuya / pre-v2 bootloader still never
  answers and correctly falls through to the manual path.

* `flash_install_rtl8196e.sh` — new `build_image_and_confirm()` helper; build
  relocated ahead of boothold on the upgrade path; pre-upload ICMP probe polls
  for the bootloader to settle.

### `flash_install_rtl8196e.sh` — clearer guidance when auto-flash is not detected

When the script falls back to the manual-FLW path (e.g. the ICMP probe never
classified the bootloader as auto-flashing), a custom V2.x bootloader may still
have auto-flashed the uploaded image on its own. The fallback wording now says
so explicitly and tells the user to `ping`/`ssh` the gateway (waiting ~2 min for
the reboot) **before** re-flashing — instead of the previous, misleading
"nothing was changed", which contradicted a flash that had in fact succeeded.

* `flash_install_rtl8196e.sh` — `manual_flw_guidance()` adds an auto-flash
  heads-up before the FLW steps and a check-first message after a declined
  confirmation. Behaviour is unchanged; only the on-screen guidance differs.

### `--boot-ip` / `BOOT_IP` accept a hostname

The bootloader-mode IP can now be given as a hostname; it is resolved host-side
to a dotted-quad before use (the on-device `boothold` and the bootloader only
understand a literal IPv4). A dotted-quad still passes through unchanged.

* `lib/ssh.sh` — new `resolve_ipv4()` (passthrough for a valid IPv4, otherwise
  `getent ahostsv4` + re-validate).
* `flash_install_rtl8196e.sh`, `flash_remote.sh` — `--boot-ip` / `BOOT_IP` run
  through `resolve_ipv4()`; an error is printed if the value is neither a valid
  IPv4 nor a name that resolves to one.

---

## [3.8.1] - 2026-06-09

### Kernel — Hardware watchdog (`rtl819x_wdt` 1.2 → 1.3) — panic notifier ordering hardened

Defense-in-depth for the v3.8.0 post-mortem. v3.8.0 added timer-wheel and
hrtimer *wheel walks* to the panic notifier (the candidate-callback lists),
but ran them **before** writing the record's magic and **before** arming the
watchdog reset. A diagnostic walk must never be able to lose the core
post-mortem or delay recovery, so the notifier is reordered:

1. write the **core** record (uptime, reason, running fn, `pc`/`ra`,
   softirq mask), candidate counts zeroed, then magic last with a barrier;
2. **arm the ~1.31 s reset** (`WDTCNR=0`);
3. only then do the best-effort timer/hrtimer walks, within the grace
   window, each list's count written *after* its entries.

If a walk ever stalled on a corrupt list during a real storm, the chip now
still resets at ~1.31 s and the core record (including `pc`/`ra`/softirq) is
already committed — the candidate lists are a bonus, never a dependency. No
record-format change (still v2). Validated on the bench: `sysrq-c` still
yields a complete record (core + populated `timers[]`/`hrtimers[]`) and the
box recovers.

* `32-Kernel/files-6.18/drivers/watchdog/rtl819x_wdt.c` — driver 1.2 → 1.3.

---

## [3.8.0] - 2026-06-02

### Kernel — Ethernet driver (`rtl8196e-eth` v2.6) + D-cache flush bounding

RX shadow-skb association hardened and the descriptor paths gained
bounds validation, at no throughput cost (TCP RX 93.9 / TX 71.5 Mbit/s,
zero interface errors or drops across the full iperf3 suite).

* **RX correctness under ring saturation.** The RX poll now indexes the
  shadow skb by the hardware mbuf index (guarded by `mbuf_index <
  rx_cnt`) instead of `rx_idx`. Under RX ring saturation the switch can
  link `pkthdr[rx_idx]` to a mbuf at a different ring index, so the old
  code could hand the stack the wrong shadow skb; flow-controlled TCP
  (the nominal case) is unaffected.
* **Descriptor pool validation.** `rtl8196e_ptr_in_pool()` range- and
  alignment-checks the pkthdr/mbuf pointers read back from the rings; a
  corrupt TX descriptor now returns `-EIO` instead of being
  dereferenced as a wild pointer. New ring anomaly counters are exposed
  via `ethtool -S eth0` (24 stats) and stay at zero in nominal flow.
* **`mm/c-lexra`: D-cache range-flush bounding.** The fast D-cache
  flush/wback now rounds to 16-byte lines and stays within the rounded
  range. Previously a 20/32-byte descriptor flush issued a full 128-byte
  unrolled op and spilled onto adjacent buffers — needless blast radius
  on this non-coherent platform. Zero-length / underflow guards added on
  the range entry points.

* `32-Kernel/files-6.18/drivers/net/ethernet/rtl8196e-eth/` — driver
  2.5 → 2.6 (`rtl8196e_ring.c/.h`, `rtl8196e_main.c`, `SPECIFICATIONS.md`,
  `PERFORMANCE.md`).
* `32-Kernel/files-6.18/arch/mips/mm/c-lexra.c` — range-flush bounding.

### Kernel — Hardware watchdog driver (`rtl819x_wdt` v1.2) — stuck-CPU PC in the panic record

The v3.7.0 post-mortem records the running timer callback
(`running=<fn>`), but a soft-lockup storm sits *between* timer callbacks,
where the kernel has already cleared `running_timer` — so that field reads
`running=0x0` and cannot name the culprit (confirmed by the first field
captures of issue #99 on v3.7.0 units). The record now also captures the
**program counter, return address, and pending-softirq mask of the stuck
context**, which pinpoint the offending frame *and* the storming softirq
even on a console-less gateway.

```
rtl819x-wdt ...: previous boot ended in panic: uptime=<sec>s pc=<fn> ra=<fn> running=<fn> softirq=0x<mask>[NAMES] reason="<msg>"
```

* `32-Kernel/files-6.18/drivers/watchdog/rtl819x_wdt.c` — driver 1.1 → 1.2.
  A soft-lockup panic is raised by the watchdog hrtimer off the local
  timer IRQ; the panic notifier still runs nested in that IRQ (sole, UP
  CPU), so `get_irq_regs()` yields the interrupted (stuck) context. Its
  `cp0_epc` (PC) and `regs[31]` (return address) are stored raw at record
  offsets `+0xF0` / `+0xF4`, resolved with `%pS` only at next-boot read
  (no kallsyms in the atomic panic path). The `ra` is kept because the
  stuck PC often lands on a leaf helper — issue #99's is
  `arch_local_irq_enable+0x14`, whose caller `handle_softirqs` is the
  frame that names the softirq storm. A panic taken from non-IRQ context
  (e.g. `sysrq-c`) stores `pc=0x0 ra=0x0`.
* `local_softirq_pending()` is captured at `+0xF8` and decoded to vector
  names at next-boot read (e.g. `softirq=0x102[TIMER|HRTIMER]`). When
  `epc`/`ra` only say "stuck in the softirq dispatcher", this names *which*
  softirq is storming; read mid-storm it shows the vectors the stuck
  handler keeps re-raising — the perpetuators. This is the datum that
  moves issue #99 from "softirq storm" to a specific subsystem.
* **Candidate callback lists.** When the storming softirq is TIMER or
  HRTIMER, the notifier walks the timer wheel / hrtimer bases on the
  panicking CPU and records up to 6 queued `.function` pointers each
  (`timers=[...]`, `hrtimers=[...]`), resolved via `%pS` at next boot. At
  panic time `running_timer` is NULL (we are between callbacks), but a
  self-rearming culprit is sitting in a near bucket — so this names it,
  the one address recurring across captures. Crucially the walks run
  **only in the panic path** (cold), so normal operation pays nothing —
  unlike the v3.4.2-era storm-2/3 hot-path instrumentation rings that
  risked perturbing the very timing they measured. Two new small exported
  accessors back this: `timer_collect_pending_fns()` and
  `hrtimer_collect_pending_fns()`.
* `32-Kernel/patches-6.18/kernel-time-timer.c.patch`,
  `include-linux-timer.h.patch`,
  `kernel-time-hrtimer.c.patch`,
  `include-linux-hrtimer.h.patch` — the cold-path accessors and their
  declarations.
* Record format bumped to **v2** and the mapped window to 512 B (adds the
  `epc` + `ra` + `softirq` fields and the two candidate lists). A leftover
  v1 record from a v3.7.0 boot is reported as `unknown record v1` on the
  one upgrade boot — no torn read (magic is still written last).

---

## [3.7.0] - 2026-05-30

### Kernel — Hardware watchdog driver (`rtl819x_wdt` v1.1) — persistent panic post-mortem

A gateway that auto-recovers from a soft-lockup hang (WDT-008) used to
reboot with no surviving trace of why: the soft-lockup report lived only
in the volatile ramfs kernel log and was wiped by the watchdog reset.

The watchdog panic notifier now leaves a compact post-mortem record in
the `boothold` reserved-memory `no-map` page before arming the chip, and
the driver decodes and clears it (one-shot) on the next boot:

```
rtl819x-wdt ...: previous boot ended in panic: uptime=<sec>s running=<fn> reason="<msg>"
```

* `32-Kernel/files-6.18/drivers/watchdog/rtl819x_wdt.c` — record (version,
  boot uptime via `ktime_get_boottime_seconds()`, the timer callback
  running on the UP CPU, and the panic reason) is written payload-first /
  magic-last with a barrier into the base of the page (`0x01FFE000`),
  ~3.8 KB clear of `boothold`'s HOLD/TFTP-IP fields at the top. All writes
  are plain MMIO into the uncached mapping — no sleeping in the atomic
  panic path. The culprit pointer is stored raw and resolved with `%pS`
  only at next-boot read (process context, no kallsyms in the panic path).
* `32-Kernel/patches-6.18/kernel-time-timer.c.patch` — tiny exported
  accessor `timer_get_running_fn()`; `base->running_timer` is otherwise
  static and unexported. Cold path only, no hot-path change. Returns NULL
  (printed as `running=0x0`) when the panic lands between callbacks.
* `34-Userdata/skeleton/etc/init.d/S26panicrec` — copies the one dmesg
  line into `/userdata/panic/history` on the **first occurrence only** and
  blocks until the file is removed by hand, so a reboot loop cannot fill
  the JFFS2 partition. Re-arm with `rm /userdata/panic/history`. This means
  at most a single one-line write to NOR flash per re-arm, for an already
  rare event — no flash-wear concern.

The page already survives the same `WDTCNR=0` reset (proven by
`boothold`), and a panic reboot does not set HOLD, so the bootloader boots
straight through without touching the record. Validated on bench:
`sysrq-c` → WDT reset → next boot reports the record and persists it.

### OTBR — drop accidental WAIT_TRACER instrumentation

`otbr-agent` / `ot-ctl` were shipped in v3.5.0–v3.6.0 with a leftover
`WAIT_TRACER` debug shim (OTBR-WAIT-TRACE kmsg spam). Rebuilt clean.

### Bootloader V2.7 + `boothold` v1.1 — configurable download-mode TFTP IP

The bootloader's download-mode IP — the address the PC's `flash_*.sh`
scripts connect to — was hard-wired to `192.168.1.6` in `tftpd_entry()`.
A user whose LAN is not on `192.168.1.x` had to recompile the bootloader
or type `IPCONFIG` at the serial console on every flash. It is now
configurable at warm reboot, with `192.168.1.6` kept as the compiled
cold-boot fallback.

The IP travels through the existing `boothold` DRAM page rather than
flash: the only automated path into download mode is `flash_remote.sh` /
`flash_install_rtl8196e.sh`, which always go SSH → `boothold && reboot`
→ **warm reboot**. DRAM survives a warm reset on the RTL8196E, so the
running Linux hands the IP to the bootloader — no flash writes, no new
MTD partition, no partition-map change. The PC is authoritative: it
already holds `BOOT_IP` (the address it will connect to), so passing it
as a parameter makes the gateway's listen-IP and the PC's connect-IP the
same value by construction. Cold-boot paths (serial recovery,
auto-download after a corrupt image, raw power-cycle) have garbage DRAM,
fail the marker check, and fall back to `192.168.1.6`; the serial
`IPCONFIG` command still overrides at the prompt there.

The IP record lives in the same reserved page as the HOLD magic
(`0x01FFE000`–`0x01FFEFFF`, `no-map`), so the device tree is unchanged:

* `34-Userdata/boothold/src/boothold.c` — `boothold [A.B.C.D]` now also
  writes an IP marker (`0x49505634` "IPV4" @ `0x01FFEFF8`) and the packed
  IPv4 (@ `0x01FFEFF4`), value-first/marker-last, with read-back verify.
  HOLD is still written unconditionally, so an older bootloader that
  ignores the argument keeps working. Helper bumps to **v1.1**.
* `31-Bootloader/boot/main.c` — on a valid HOLD, read the IP record (only
  trusted alongside HOLD, i.e. a deliberate warm reboot), apply it, and
  wipe all three words (one-shot).
* `31-Bootloader/boot/net/tftpd.c` — new `g_tftp_server_ip` global
  (default `192.168.1.6`); `tftpd_entry()` uses it and derives the
  matching `eth0_mac[1..4]`, mirroring the IP→MAC coupling `IPCONFIG`
  already performs, and prints the active server IP.
* `31-Bootloader/boot/monitor.c` — `IPCONFIG` updates the same global, so
  the serial and DRAM paths share one variable. Bootloader bumps to
  **V2.7** (was V2.6).
* `3-Main-SoC-Realtek-RTL8196E/flash_remote.sh` — passes `$BOOT_IP` to
  `boothold`, so a non-default `BOOT_IP` configures the bootloader's
  download-mode IP with no serial console.

Also fixes a long-standing build warning: `format(printf, ...)` in
`boot/include/linux/kernel.h` was macro-expanded to the unrecognized
`format(dprintf, ...)` by `#define printf dprintf`; switched to the
reserved `__printf__` archetype.

### `flash_install_rtl8196e.sh` — capability tests over version heuristics

The first-install script used to cross five detection axes (gateway state,
firmware type via a `devmem` proxy + port-2333 sniff, `FW_VERSION`,
bootloader type via ping, an inferred auto-flash flag) before the real
outcome was settled a sixth way by the runtime UDP:9999 notification.
Brittle proxying that conflated two independent concerns. Collapsed to two
direct signals:

* **Entry** is decided from Linux by `command -v boothold` over SSH:
  present → automated (`boothold "$BOOT_IP" && reboot`, tying in the V2.7
  IP handoff); absent → serial-guided. More correct than the old `devmem`
  proxy — custom v1.0.0 had `devmem` but no `boothold`, so it used to
  boothold-fail then time out; it now routes straight to manual.
* **Flash** is one shared, version-free routine: always upload the image,
  then decide behaviourally. Pre-upload ICMP silent → Tuya/pre-v2, guided
  FLW. ICMP up then going silent post-upload → the auto-flash bootloader
  is writing 16 MiB → wait for UDP:9999 OK. Staying up → custom bootloader
  without auto-flash → guided FLW. Decided in seconds; the 180 s dead wait
  is gone (survives only as the UDP listener ceiling, which breaks early on
  OK).

Removed the `BOOTLOADER_TYPE`, auto-flash, `devmem`/port-2333 type proxy,
and `FW_VERSION` auto-flash gate (`FW_VERSION` survives only for the v2→v3
`radio.conf` pre-seed). Bench-verified on Tuya stock, custom v1.2.1, and
v3.6.0/V2.7.

CLI harmonised with `flash_remote.sh` so the two flash entry points share
one interface:

* `--boot-ip <IP>` flag added to both scripts as an alternative to the
  `BOOT_IP` env var (precedence: flag > env > default `192.168.1.6`). The
  value is validated as a dotted-quad IPv4 before any network action, via a
  shared `valid_ipv4` helper now in `lib/ssh.sh`.
* The gateway-IP positional is `LINUX_IP` in both scripts (paired with
  `BOOT_IP` for the two gateway states). `flash_remote.sh` drops its unused
  `SSH_USER` override — both now connect as `root` — and the `--help` text
  for the shared options/environment is identical between the two.

---

## [3.6.0] - 2026-05-26

Closes the second half of the OTBR/RCP-link failure first addressed in
3.5.1 (#109), and adds process supervision so the border router
self-heals from an agent crash. Also rolls up the build/tooling fixes
that landed on `main` after 3.5.1.

### Kernel — UART driver (`8250_rtl819x` v1.2)

3.5.1 forced the full `DTR|RTS|OUT2|AFE` MCR pattern in
`enable_flow_control()`, but only re-applied it at probe and on a
`CRTSCTS` off→on transition. Steady-state OTBR never toggles `CRTSCTS`,
so a runtime `serial8250_set_mctrl()` — e.g. the serial core's
`uart_throttle()` clearing RTS when the tty RX buffer backs up (the
driver never advertised `UPSTAT_AUTORTS`) — re-clobbered the MCR to
`0x20000000` (AFE only, RTS deasserted) and nothing restored it.
Field-confirmed on 3.5.1 (#109): `devmem 0x18002110` read `0x20000000`
at the wedge, `0x2B000000` after an `otbr-agent` restart.

The driver now installs a custom `port->set_mctrl` that re-ORs
`TIOCM_RTS` while flow control is engaged, so no runtime modem-control
write can deassert RTS under AFE. Hardware AFE still provides the real
backpressure by gating the RTS line on the RX FIFO level. Touches only
the rtl8196e glue driver — no `serial_core` / 8250-core change.
`MODULE_VERSION` 1.1 → 1.2.

### Userdata — OTBR process supervision (`keepalive`)

`otbr-agent` previously ran unsupervised, and the housekeeping loop
(status LED, dataset sync, SRP recovery) was an inline busybox-ash
sub-shell. So an `otbr-agent` exit (e.g. an RCP fault) left OTBR down
until manual intervention, and the long-lived ash loop took intermittent
SIGSEGV/SIGILL — the fault class that retired the s40button shell loop
in 3.3.1.

A new static-C `keepalive` supervisor (fork/exec/waitpid, capped
exponential backoff, SIGTERM forwarding for a clean stop) now runs both
`otbr-agent` and a standalone `otbr-monitor`, each self-healing from a
crash within ~1 s. Post-start radio tuning (log level, TX power) moved
into `otbr-monitor` so it re-applies on every `otbr-agent` restart (the
OT stack resets TX power on each init); SRP recovery stays once-per-boot
via a tmpfs flag. `S70otbr stop` terminates the supervisors so they do
not fight the shutdown. `ncp-uart` is unaffected — `S70otbr` is gated on
`MODE=otbr`, so `keepalive` ships unused in that mode.

### Build / tooling

- Docker build image gains `iproute2` and `xxd` for in-container flashing.
- Fixed a self-referential `bin/` symlink that broke Docker image rebuilds.
- Hardened the release squash recipe against `ugrep`-aliased `grep`.

---

## [3.5.1] - 2026-05-19

Single-fix patch release: closes a partial MCR clobber in the RTL8196E
UART driver that could throttle the RCP Spinel link between the kernel
and the EFR32 radio after a `CRTSCTS` termios cycle, eventually causing
`otbr-agent` to time out with `RadioSpinelNoResponse`.

### Kernel — UART driver (`8250_rtl819x` v1.1)

`rtl8196e_uart_enable_flow_control()` no longer fast-paths when the AFE
bit (bit 29) is already set in MCR. The 8250 core's byte-wise MCR writes
during `set_termios()` preserve AFE but stomp DTR/RTS/OUT2 back to its
mctrl shadow. With the previous fast-path, the post-termios re-enable
saw AFE=1 and returned, leaving the SoC at `MCR = 0x20000000` (AFE only)
instead of the boot-time `0x2B000000` (DTR|RTS|OUT2|AFE). RTS clear
under AFE = SoC asserts !RTS to the EFR32 = Spinel throttle = RCP
timeout.

The fix re-ORs the full `DTR|RTS|OUT2|AFE` pattern on every call, so the
post-termios re-enable always restores the boot-time MCR even when AFE
is already set. Observable signal: `devmem 0x18002110 32` reads
`0x2B000000` after every `S70otbr restart`; symptomatic gateways read
`0x20000000`.

Driver `MODULE_VERSION` bumped 1.0 → 1.1.

---

## [3.5.0] - 2026-05-17

Two robustness improvements aimed at the same failure mode — the
gateway losing critical state across a reboot or power cut. The
**hardware watchdog driver** lets the gateway auto-recover from a
kernel hang in ~23 s instead of needing a manual power cycle. The
**SRP server auto-recovery in `S70otbr`** closes the user-visible
gap where Matter-over-Thread sensors disappeared from Home
Assistant for up to ~1 hour after every reboot.

### Kernel — Hardware watchdog driver (`rtl819x_wdt` v1.0)

New kernel driver `drivers/watchdog/rtl819x_wdt.c` drives the
RTL8196E on-chip watchdog at sysc + 0x311C. WDTCNR field semantics
verified against the RTL8196E-CG datasheet (table 27): WDTE
[31:24], WDTCLR [23], OVSEL[1:0] [22:21], WDIND [20], OVSEL[3:2]
[18:17].

The driver lands with the full recovery story wired:

* `/dev/watchdog` registered with the kernel framework. Restart
  handler at priority 192 supersedes `arch_reset` so `reboot` and
  `sysrq-b` flow through the same chip path (~1.3 s OVSEL=0
  bucket). `WDOG_HW_RUNNING` adoption keeps a pre-armed chip
  kicked across the probe-to-userspace window.
* **Slowclk CDBR rework** (WDT-005 closed). The watchdog and
  Timer0/Timer1 used to share a 25 MHz CDBR tick, capping
  watchdog overflow at ~671 ms even at OVSEL=1001 — too tight
  for any userspace feeder. `timer-rtl819x` now runs from a
  dedicated 25 kHz `slowclk` DT fixed-clock; CDBR DivFactor=8000
  matches the SDK BSP, and OVSEL=1001 overflows at ~671 s. DT
  `timeout-sec=60` exposed to userspace; BusyBox feeder pings
  every 30 s for ~22× margin. Validated under iperf3 soak
  (cross-driver impact captured in
  `drivers/clocksource/AUDIT.md` TMR-005).
* **Userspace feeder activated** (WDT-007 closed).
  `34-Userdata/skeleton/etc/init.d/S25watchdog` shipped
  executable; BusyBox `watchdog` applet (`CONFIG_WATCHDOG=y`)
  in the rootfs feeds `/dev/watchdog -t 30` from boot slot S25.
* **Soft-lockup blind spot plugged** (WDT-008 closed). On
  UP+PREEMPT_NONE, a userspace busy-syscall loop (e.g. the
  `otbr-agent __do_wait` hang in GitHub issue #99) used to ride
  out the watchdog forever — the framework auto-kicker fires
  from softirq context which drains on every syscall return,
  keeping the chip petted while the soft-lockup detector
  screamed unheard. The driver now registers on
  `panic_notifier_list` and writes `WDTCNR=0` (OVSEL=0,
  ~1.31 s bucket) from the notifier. Wired against
  `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y`, end-to-end hang
  recovery drops from "never" to ~23 s (22 s detection +
  ~1.31 s chip overflow). Validated on hardware via `sysrq-c`:
  reboot in ~5 s wall (well under the 10 s `CONFIG_PANIC_TIMEOUT`
  fallback).
* **Notifier priority pinned to INT_MAX** (WDT-009). Defence in
  depth: the panic notifier chain dispatches in descending
  priority order; without an explicit priority, a future
  higher-priority notifier that wedged on a console flush or
  flash write would defeat our chip-arming write. `NOTIFY_DONE`
  unchanged so crashlog dumpers still run inside the ~1.31 s
  grace window.
* **Operator diagnostics via sysfs** (`CONFIG_WATCHDOG_SYSFS=y`).
  `/sys/class/watchdog/watchdog0/` exposes 15 attributes —
  `identity`, `timeout`, `min_timeout`, `max_timeout`,
  `nowayout`, `bootstatus`, `state`, `status`, `timeleft`,
  `options`, `fw_version`, ... — so ops can confirm the chip
  is armed without going through `devmem` or dmesg.
* **User-facing documentation**.
  `files-6.18/drivers/watchdog/README.md` covers the four
  recovery scenarios, sysfs verification, configuration knobs
  (DT, module param, Kconfig), three ways to disable for debug,
  and a six-symptom troubleshooting table. Internal design log
  stays at `AUDIT.md` alongside (nine WDT-### findings closed).
* DT match-table restricted to `realtek,rtl8196e-wdt` — same
  RTL8196E-specific tightening as the v3.4.0 GPIO driver pass.

### Kernel — Hardening enablers for autonomous recovery

The watchdog can only fire on what the kernel knows about. The
Kconfig flips below turn "could fire" into "actually fires" for the
classes of failure that v3.4.x silently sat on:

* `CONFIG_LOCKUP_DETECTOR=y` + `CONFIG_SOFTLOCKUP_DETECTOR=y`
  + `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y` — soft lockup at 22 s
  → `panic()` → watchdog notifier path.
* `CONFIG_SOFTLOCKUP_DETECTOR_INTR_STORM=y` +
  `CONFIG_IRQ_TIME_ACCOUNTING=y` — when a soft lockup fires, the
  detector now also auto-prints the storming IRQ (top offender by
  CPU time) directly in the panic banner. Added specifically to
  cut the diagnostic round-trip on classes of hangs that look like
  user-space spin but turn out to be IRQ-driven (e.g. GitHub
  issue #99). Negligible runtime cost — accounting is read at
  panic time.
* `CONFIG_PANIC_ON_OOPS=y` + `CONFIG_PANIC_TIMEOUT=10` — any oops
  becomes a panic, same watchdog path; the 10 s timeout is the
  fallback if the watchdog notifier itself wedges (chip overflow
  still fires ~1.3 s after `panic()` writes `WDTCNR=0`, so the
  10 s is rarely consumed). v3.4.1 had `PANIC_TIMEOUT=0` and
  `PANIC_ON_OOPS` unset → oops logged, box limped on forever.
* `CONFIG_DETECT_HUNG_TASK=y` +
  `CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=60` +
  `CONFIG_DETECT_HUNG_TASK_BLOCKER=y` — blocked-task detection
  with the blocker reported alongside (still warn-only;
  `BOOTPARAM_HUNG_TASK_PANIC` deliberately not set yet).
* `CONFIG_KALLSYMS=y` + `CONFIG_KALLSYMS_ALL=y` — softlockup /
  panic / oops traces now show symbol names instead of raw
  addresses, including data symbols (helps when reading slab /
  per-CPU state in a post-mortem). Modest kernel-size cost
  (+184 KB), pays for itself the first time anyone reads a
  post-mortem.
* `CONFIG_PRINTK_TIME=y` — every dmesg line gets a relative
  timestamp; required to correlate soft-lockup spam with the
  watchdog overflow window.
* `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y` — required for the
  `WDOG_HW_RUNNING` adoption path mentioned above; without it
  the framework wouldn't recognise a chip armed by an earlier
  boot stage.

### Kernel — Serial-console diagnostics surface

`CONFIG_MAGIC_SYSRQ=y` + `CONFIG_MAGIC_SYSRQ_SERIAL=y` +
`CONFIG_MAGIC_SYSRQ_DEFAULT_ENABLE=0x1` enable BREAK→SysRq
dispatch from the serial console (e.g. `sysrq-b` for force-reboot,
`sysrq-c` to crash and exercise the watchdog notifier path,
`sysrq-t` to dump every task's stack). v3.4.1 had `MAGIC_SYSRQ`
unset, so the 8250 SysRq dispatch fix below would have had nothing
to dispatch to. This is also the validation path used for the
WDT-008 acceptance test (`echo c > /proc/sysrq-trigger` → reboot
in ~5 s wall).

### Kernel — SysRq dispatch fix on the 8250 serial driver

Linux 6.18 vanilla broke BREAK→SysRq dispatch on `8250_port` and
`8250_dw` (upstream commit `8324a54f604d` replaced
`uart_unlock_and_check_sysrq_irqrestore()` with a plain
`guard(uart_port_lock_irqsave)`; the new guard destructor drops
the captured `sysrq_ch` instead of dispatching it). Restored via
three patches under `patches-6.18/` mirroring the upstream RFC v2
posted to linux-serial (`uart_port_lock_check_sysrq_irqsave`
helper + per-driver updates), with Ilpo Järvinen's `Reviewed-by`
on 2/3 + 3/3. Without this fix, serial-console `sysrq-c` and
`sysrq-b` would be silently swallowed — which would also have
silently broken the watchdog-via-sysrq validation path above.

### Userdata — SRP server auto-recovery in `S70otbr`

`otbr-agent` keeps its SRP server host/service registry in RAM
only. OpenThread persists only the UDP port across reboots
(`kKeySrpServerInfo` in `Settings`); the host/service entries in
`mHosts` are deliberately not persisted — the protocol assumes
clients refresh on their own lease. After every gateway reboot
or power cut, attached Matter SEDs stayed silent in Home
Assistant until each one's lease refresh fired — up to ~1 hour
with the default 7200 s lease. The same gap hit every firmware
upgrade.

`S70otbr` now runs a one-shot recovery cycle in its background
loop: once the Thread state has been `up` for 120 s, if
`ot-ctl child table` shows ≥ 1 attached child, the script runs
`disable && sleep 30 && enable` on the SRP server. The cycle
bumps the SRP UDP port via
`OPENTHREAD_CONFIG_SRP_SERVER_PORT_SWITCH_ENABLE`, the BR
republishes the new port in Thread Network Data, and every
attached child re-registers within seconds at its next data-poll.
Validated end-to-end on the gateway: registry repopulated in
15-17 s, no manual intervention, no re-pair, HA collection
resumed immediately.

The cycle fires unconditionally (once per boot) whenever any
child is attached — partial natural recovery on a multi-sensor
deployment would otherwise leave most SEDs silent until each
own lease/2 fires (~1 h). Cost: 30 s of SRP downtime once at
boot. On a freshly flashed gateway with no commissioned device,
`child table` is empty so the cycle does not run. The manual
recipe (`REPORT.md` Recipe 3, Step 1) still applies for edge
cases — e.g., a sensor that re-attaches only after the
watchdog window has closed.

A proper architectural fix (persisting the SRP server registry to
flash) would require an OpenThread fork, ~500–800 lines in
`srp_server.cpp` + new `Settings` keys; tracked as a separate
item.

---

## [3.4.1] - 2026-05-02

Point release on top of v3.4.0.  No breaking change — MOTD, sysfs
keys and init scripts unchanged.  EFR32 firmware unchanged; see the
[EFR32 v3.4.1 entry](../2-Zigbee-Radio-Silabs-EFR32/CHANGELOG.md#341---2026-05-02)
for the matching `flash_efr32.sh` fix.

### Kernel — `rtl8196e-eth` v2.4 → v2.5

* **TX-kick coalescing.**  The switch ASIC's "kick TX" pulse is now
  batched (up to 4 packets per pulse) instead of fired on every
  submit, with an immediate flush on cold-start and at the end of
  each NAPI poll.  Small TCP TX gain on this single-core CPU.
* **Mandatory `realtek,syscon` in `rtl8196e_probe()`.**
  `-EPROBE_DEFER` is propagated and any other lookup error fails the
  probe loudly, so a missing or not-yet-ready syscon node no longer
  produces a partially-muxed `eth0`.
* **Canonical RX descriptor rearm.**  The drop and bad-length exits
  in `rtl8196e_ring_rx_poll()` now reset descriptor metadata before
  flipping ownership back to the switch, like the nominal exit
  already did.  Removes a class of intermittent RX-stall failure
  modes under SKB-allocation pressure.
* **Compile-time big-endian guard** in `rtl8196e_desc.h` — the
  `rtl_pktHdr` / `rtl_mBuf` bitfield layout only matches the wire
  format on big-endian builds, and an `#error` now stops a
  little-endian build before it produces a binary that compiles
  cleanly but plants the wrong bits in TX/RX descriptors.

### Kernel — `rtl8196e-eth` observability

* **`ethtool eth0`** decodes link state, speed and duplex from the
  switch `PSRPx` register; **`ethtool -i eth0`** reports driver name,
  version and bus info.
* **Per-cause TX-kick counters** added to the stats list:
  `rtl8196e_tx_kicks_total`, `_cold`, `_threshold`, `_drain`.
* **`/sys/class/net/eth0/kick_threshold`** is RW (1..64) so the TX
  coalescing batch size can be tuned without a reboot.

### Kernel — `arch/mips/realtek/imem.S` (latent IRAM/DMEM init fixes)

Both bugs are inert under the v3.4.0 defconfig but would trip the
boot the moment `.iram` empties or any data is placed in DMEM.

* **IRAM Window programming skipped when `.iram` is empty**, removing
  an overlap with the DRAM Window when both sections share the same
  physical address.
* **DMEM bring-up now copies SDRAM→DMEM** in 4 stages before flipping
  "DMEM On"; reads from the DMEM virtual range no longer return
  uninitialised SRAM garbage.

Two Kconfig switches — `RTL8196E_IMEM_DEFAULT_PLACEMENT` (default y)
and `RTL8196E_IMEM_POC_IRAM` (default n) — gate the existing
`__iram*` annotations and an extra `__iram_poc` macro respectively.
Default v3.4.0 behaviour is preserved bit-for-bit.

### Userdata — OTBR REST API compatible with HA 2026.4 and 2026.5+

`build_otbr.sh` patches `ot-br-posix` so that `otbr-agent` serves
PascalCase JSON keys (the form expected by `python-otbr-api` 2.9.x
shipped in HA 2026.4) and so that the schema-detection probe added
in `python-otbr-api` 2.10.0 (HA 2026.5+) selects the matching
PascalCase parser.  Without these patches HA 2026.5+ would silently
fail to read the Thread dataset.

### Userdata — radio TX power persisted across reboots

`S70otbr` now sets the OT-RCP TX power at every boot, with a verify-and-retry
loop because the OT stack drops `ot-ctl txpower` commands during early init.
The target is **+3 dBm** — validated overnight on a 16-sensor home deployment
(see `2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/range-testing/REPORT.md`) as
enough margin for typical homes without unnecessarily polluting 2.4 GHz.

The previous behaviour was to leave the radio at the firmware default
(0 dBm) on every boot, costing ~3–7 dB of margin on weak links until an
operator manually issued `ot-ctl txpower`. A power-cycle (or a physical
gateway move that briefly disconnects power) was enough to silently
revert to the worst-case TX. Now persisted in `/userdata/etc/init.d/S70otbr`
itself, so a kernel/rootfs upgrade does not lose the setting and a
`flash_install_rtl8196e.sh` redeploys it.

### Userdata — optional debug tooling

* `34-Userdata/ethtool/build_ethtool.sh` — builds **ethtool 6.10**
  (≈ 189 KB static).
* `34-Userdata/iperf3/build_iperf3.sh` — builds **iperf3 3.18**
  (≈ 303 KB static).

Both binaries are intentionally absent from the default JFFS2 image
to keep the install lean.  Build locally and `scp -O` to
`/userdata/usr/bin/` if you want them — they are PATH-resolved and
JFFS2-persistent across reboots.

### Tooling

* `flash_efr32.sh` — fix for issue #96: the script no longer aborts
  silently when the bootloader version line is missing from
  `commander`'s USF log on the common app→bootloader transition path.

### Documentation

* `files-6.18/drivers/net/ethernet/rtl8196e-eth/PERFORMANCE.md`
  rewritten against the current 6.18 / driver 2.5 code (the previous
  file targeted the 5.10 / 200 MHz era and was inaccurate).
* `files-6.18/drivers/net/ethernet/rtl8196e-eth/SPECIFICATIONS.md`
  aligned with the actual ring sizes, kick thresholds, module
  parameters and sysfs attributes.

---

## [3.4.0] - 2026-05-01

Hardening release: four independent driver audits applied as bounded
patch sets across the custom kernel drivers (timer, IRQ controller,
GPIO bank, Ethernet), one user-visible perf tuning of the IRQ routing
on the Zigbee path, and the front-panel button daemon rewrite that
fixes the v3.2.x/v3.3.0 intermittent SIGSEGV. No EFR32 firmware change,
no breaking change to `radio.conf` or sysfs interfaces; no measured
regression on the iperf full suite (TCP RX 93.9 / TX 70.2 Mbit/s vs
93.9 / 71 baseline, 5-min stress retrans 0.00 %, soak OTBR 460800 baud
8h+ stable). Validated end-to-end on real hardware.

### Kernel — `timer-rtl819x` v1.0 (4 audit fixes + version banner)

`drivers/clocksource/timer-rtl819x.c`:

* **Quiesce Timer0 + `request_irq()` before `clockevents_config_and_register()`.**
  Reordered the bring-up so the IRQ handler is installed before the
  clockevent core can drive `set_next_event()`. On this platform CPU
  IP7 is level-triggered and dedicated to Timer0, so an unhandled
  assertion would turn into an interrupt storm rather than a single
  lost edge. (audit RTL819X-TMR-002)
* **Validate clock divider and busclk enable.** `clk_prepare_enable(busclk)`
  return is now checked, `bus_rate >= timer_rate` is enforced, and the
  computed `div_fac` is bounded to `[1, 0xffff]` (the CLOCK_DIV[31:16]
  field width). In practice the DT pins busclk=200 MHz / refclk=25 MHz
  so `div_fac = 8` and the bounds never trigger, but a misconfigured
  DT would otherwise silently program a bogus divider. (RTL819X-TMR-001)
* **Propagate `clocksource_register_hz()` errors.** `rtl819x_clocksource_init()`
  now returns `int` and the caller stops the timer init on failure
  instead of silently continuing with `sched_clock_register()` against
  a clocksource the kernel rejected. (RTL819X-TMR-004)
* **Return `IRQ_NONE` when `TC0_PENDING` is not set.** Lets the kernel
  spurious-IRQ machinery catch a misrouted IP7 instead of the driver
  silently absorbing it. (RTL819X-TMR-003)
* `DRV_VERSION "1.0"` and `pr_info` boot banner aligned with the other
  custom drivers (`8250_rtl819x`, `rtl8196e-eth`, `rtl8196e-uart-bridge`).

### Kernel — `irq-rtl819x` v1.0 (3 audit fixes + perf tuning + version banner)

`drivers/irqchip/irq-rtl819x.c`:

* **Only arm TC0 in GIMR at init.** Other child sources (UART0/UART1/
  Switch) are now activated through their `.irq_unmask` callback when
  the consumer driver calls `request_irq()` / `enable_irq()`, instead
  of being globally unmasked at irqchip init regardless of probe state.
  TC0 stays unconditional because the timer driver requests CPU IRQ 7
  directly via `&cpuintc` and never traverses this irqdomain — the only
  hardware path TC0→IP7 is via INTC IRR1 + GIMR (verified against the
  bootloader source). (RTL819X-IRQ-001)
* **Describe and parse IP2/IP3/IP4 parent IRQs from DT.** The intc@3000
  node previously declared a single parent IRQ but the driver hardcoded
  three, hiding an implicit dependency on cpuintc legacy domain
  numbering. The DT now lists the three parents with `interrupt-names`,
  the driver resolves them with `irq_of_parse_and_map()`, and the
  CPU-IP constants are gone from the C side. (RTL819X-IRQ-003)
* **Drop redundant GISR ack in chained handler.** Acknowledgement was
  done twice per IRQ (parent-side W1C + `realtek_soc_irq_ack` via
  `handle_level_irq`). The level flow handler covers it, so one MMIO
  write per IRQ saved on every UART/Switch interrupt. (RTL819X-IRQ-004)
* **Swap UART1/Switch IRR routing for Zigbee gateway latency.**
  `plat_irq_dispatch()` services the MIPS IP lines in fixed order
  IP7 > IP4 > IP3 > IP2. UART1 (Zigbee link to the EFR32 radio, 16-byte
  RX FIFO, ~350 µs of latency budget at 460800 — overrun = lost frame =
  Z2M/ZHA reconnect, user-visible) is now on IP4; the Ethernet switch
  (DMA rings + NAPI, missed IRQ = TCP retransmit, invisible) is on IP3.
  Asymmetric benefit for the actual workload of this gateway. Validated
  by the overnight OTBR 460800 soak: zero overruns on `ttyS1` over 8h+.
* `DRV_VERSION "1.0"` and probe banner.

### Kernel — `gpio-rtl819x` v1.0 (3 audit fixes + version banner)

`drivers/gpio/gpio-rtl819x.c`:

* **Use dynamic GPIO base (-1) instead of hardcoded 0.** Aligns with
  the modern gpiolib convention; all in-tree DT consumers go through
  phandles so no consumer breaks. (RTL819X-GPIO-001)
* **Propagate `regmap_update_bits` error from pinmux setup.**
  `rtl819x_gpio_configure_pinmux()` now returns `int` and a syscon
  write failure surfaces as `dev_err` + `.request` failure, instead of
  gpiolib silently handing out a line whose physical pin is still
  driving the shared LED function. (RTL819X-GPIO-002)
* **Match only `realtek,rtl8196e-gpio` compatible.** The driver header
  states the PIN_MUX_SEL_2 layout is RTL8196E-specific; the previous
  generic `realtek,realtek-gpio` and `realtek,rtl819x-gpio` entries are
  removed and the DT updated accordingly. (RTL819X-GPIO-003)
* `DRV_VERSION "1.0"` and probe banner.

### Kernel — `rtl8196e-eth` v2.3 → v2.4 (4 audit fixes + version bump)

`drivers/net/ethernet/rtl8196e-eth/`:

* **Zero-pad short TX frames before DMA.** `rtl8196e_start_xmit()` now
  calls `skb_put_padto(skb, ETH_ZLEN)` before flushing data cache. The
  previous flush of `max(skb->len, ETH_ZLEN)` exposed slab tailroom
  to the switch DMA — a low-impact information leak on the wire on
  short frames (ARP, IPv4 minimal). (RTL8196E-ETH-001)
* **Reset RX/TX rings on `.ndo_stop`.** `ip link set eth0 down/up` under
  live traffic now starts each cycle with both rings rebuilt from the
  shadow SKB pool — the previous stop() left descriptors in indeterminate
  ownership, and open() only reprogrammed the ring base addresses. New
  `rtl8196e_ring_rx_reset()` mirrors the RX init in `ring_create()`
  exactly (same flags, flush span, ownership flip order); SKBs are
  reused in place to avoid OOM in tight loops. (RTL8196E-ETH-002)
* **`BUILD_BUG_ON` the descriptor layout vs ASIC ABI.** Hardware writes
  `ph_len`/`ph_flags`/`ph_reason` and reads `m_data`/`m_extbuf` at
  fixed byte offsets, so the GCC layout of `struct rtl_pktHdr` /
  `rtl_mBuf` is part of the contract. A static-inline check pinned at
  `ring_create()` fails the build on any silent shift, instead of
  corrupting RX/TX at runtime. (RTL8196E-ETH-004)
* **Validate DT port masks against the 9-port hardware.** `member-ports`
  must fit in the 9-bit window (`0x1ff`); `untag-ports` must be a
  subset of `member-ports`. The HW iterates over `port < 9` so out-of-
  range bits previously cycled through table writes on imaginary ports
  without error. (RTL8196E-ETH-006)
* `DRV_VERSION` bumped to **2.4**, banner updated, local
  `AUDIT.md` extended with a "Second-pass audit (2026-05-01)" section
  cross-referencing the new finding IDs against the existing F1-F17
  history (in particular: ETH-005 == F17 "KSEG1 documented intentional",
  and ETH-008 == F13 "RX rearm `wback_inv → inv` — tested HW 2026-04-23
  in F11+F13+F15 bundle, **rejected** with -47 Mb/s RX regression").

### Userdata — `s40button` static C daemon

`34-Userdata/s40button/`:

* The v3.2.x / v3.3.0 BusyBox shell loop polling GPIO 9 via `devmem`
  had an intermittent SIGSEGV in the `ash` interpreter after some hours
  of idle polling. Rewritten as a static C daemon (~112 KB, Lexra musl
  toolchain): `mmap(/dev/mem)` on the GPIO bank page, 100 ms poll loop
  on bit 9, debouncing identical to the shell version, 5 s long-press
  → `recover_efr32 -q`. Same observable behaviour, no more crash. Built
  by `build_s40button.sh` and installed into `skeleton/usr/sbin/`.

### Audits behind this release

The four kernel audits were independent and out-of-band. Each driver
now ships its own `AUDIT.md` next to the source:

* `files-6.18/drivers/clocksource/AUDIT.md` — timer (TMR-001..004)
* `files-6.18/drivers/irqchip/AUDIT.md` — INTC (IRQ-001..007 + perf swap)
* `files-6.18/drivers/gpio/AUDIT.md` — GPIO bank (GPIO-001..006)
* `files-6.18/drivers/net/ethernet/rtl8196e-eth/AUDIT.md` — Ethernet
  (F1..F17 from the April pass + ETH-001..008 from the May pass-2)

Each file maps every finding ID to its commit SHA, status (fixed /
deferred / rejected), and reasoning — including the rejected ones.
Convention going forward: an audit pass landing as a coherent commit
batch gets a `## Post-audit pass N (date) — driver M.N` section
appended to the local `AUDIT.md`.

### Upgrade

```sh
./flash_install_rtl8196e.sh -y <gateway-IP>
```

No `radio.conf` migration needed; sysfs interface unchanged.

---

## [3.3.0] - 2026-04-30

Critical reliability release: closes
[#89](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/89) —
otbr-agent timeouts at 460800 baud — by enabling hardware UART flow control
(RTS/CTS) at the operating-system layer, which was previously not configured.
The release also bundles two out-of-band 8250 driver audit hardenings, a
major refactor of `flash_efr32.sh` that resolves long-standing UX issues
around mode switching, and defensive robustness in `radio.conf` parsing.

### Hardware UART flow control — root cause for #89

For Thread / OT-RCP installations at 460800 baud, `otbr-agent` would lose
Spinel sync after ~1h of operation, ending with `HandleRcpTimeout()` and
the agent abandoning. Reported by @olivluca in
[#89](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/89).
Not observable at 115200, where the FIFO fill window is large enough to
absorb burst latency without flow control.

Root cause: `S70otbr` opened `/dev/ttyS1` with default termios — no
`CRTSCTS` — so the 8250 core never set `MCR_AFE`, and the gateway ran
without hardware RTS/CTS. At 460800 the 16-byte RX FIFO fills in ~280 µs;
under bursty Spinel traffic (Matter commissioning attestation in
particular), kernel IRQ latency could exceed the drain budget and the
FIFO would overrun, dropping bytes → HDLC corruption → Spinel timeout.

Fix: `S70otbr` now passes `&uart-flow-control=true` in the spinel radio
URL. `otbr-agent` sets `CRTSCTS` on the tty, the 8250 core sets `MCR_AFE`
(bit 29 of the 32-bit MCR alias on this SoC), and the hardware
auto-asserts RTS when the FIFO approaches full, throttling the EFR32.
Validated by reproducing the failure locally, applying the fix, and
running a 3h+ continuous soak with two Matter sleepy devices and
back-to-back commissioning bursts: zero overruns, `MCR=0x2B000000` (AFE
on), `otbr-agent` PID stable.

### Kernel 8250 driver fixes (#89 defensive layers + audit hardening)

Four changes in `8250_rtl819x.c` bundling the #89 defensive layers with
two orthogonal findings from the out-of-band 8250 audit:

* **RX FIFO trigger** lowered from 8 to 4 bytes (`UART_FCR_R_TRIG_01`).
  Gives 12 bytes of headroom before overflow at 460800 (~210 µs of IRQ
  latency budget) instead of 8 bytes (~140 µs). With AFE engaged this
  is belt-and-suspenders, but it absorbs latency spikes on the
  single-core 200 MHz Lexra without affecting throughput.

* **AFE bit RMW under `port->lock`** in `enable/disable_flow_control`.
  Closes the race where the 8250 core's byte-level MCR writes
  (`serial8250_set_mctrl`, `em485_stop_tx`) could clobber AFE between
  our `readl()` and `writel()`. The helpers accept `port=NULL` for
  probe-time calls (no concurrency yet). Audit finding **8250RTL-003**.

* **`realtek,syscon` DT phandle now mandatory** (audit **8250RTL-001**).
  Probe fails explicitly if the syscon is missing instead of warning
  and continuing. Without the syscon the UART pinmux is not configured,
  and `ttyS1` looks usable internally but has no signal on the physical
  pins toward the EFR32.

* **Refuse to register on a line other than `ttyS1`** (audit
  **8250RTL-002**). The kernel UART bridge, `S50uart_bridge`, `S70otbr`,
  and `radio.conf` all assume `/dev/ttyS1`; accepting an opportunistic
  line would silently mis-wire the bridge. Probe now unregisters and
  returns `-EBUSY` if the core assigned a different line.

### `flash_efr32.sh` — switch-mode UX, baud sweep, hardening

The EFR32 over-the-air flash script gained a substantial robustness pass.
Resolves three of the five items tracked in the prior `TODO-v3.3.md`
(items #1, #2, #5):

* **Bridge ↔ `radio.conf` reconciliation** *(TODO #1)*. Previously,
  after editing `/userdata/etc/radio.conf` to switch modes (Zigbee ↔
  OTBR), the user had to manually rearm the bridge sysfs at the new
  baud + `flow_control` before `flash_efr32.sh` would work. Now: if
  `radio.conf` says one baud and the bridge sysfs says another, the
  script disarms + rearms the bridge at the config baud, with
  `flow_control` aligned to `MODE`.

* **Symmetric baud-fallback sweep** *(TODO #2)*. The old fallback
  tried only 115200, missing the inverse case (radio.conf=115200 but
  chip really at 460800 from a prior test). Replaced with a parametric
  `try_flash_at_baud()` that sweeps
  `{115200, 230400, 460800, 691200, 892857}`, skipping the baud already
  attempted from `radio.conf`, and exits on the first success.
  `radio.conf` is now treated as a hint, not ground truth.

* **Switch-mode UX** *(TODO #5)*: combination of the above with the
  existing post-flash `radio.conf` write-back means
  `./flash_efr32.sh -g IP otrcp` on a Zigbee-installed gateway now
  Just Works, without manual `radio.conf` edits or sysfs gymnastics.

Additional hardening, beyond TODO-v3.3, bundled here while the script
was being touched:

* **`--firmware-file PATH`** option to bypass GBL glob resolution.
  Useful for testing custom builds outside the repo's `firmware/`
  tree.

* **Refuse ambiguous GBL match**. The old `ls -t … | head -1` silently
  picked the most recent file by mtime, which could hide a stale or
  wrong image. Now: error + force `--firmware-file PATH` if multiple
  matches.

* **USF venv version pin sanity check**. If the installed
  `universal-silabs-flasher` is not 1.0.3, reinstall before use; abort
  if reinstall didn't take. Prevents probe-method CLI drift bugs that
  motivated the venv pin in the first place (#92).

* **`assert_bridge_idle()`** race protection: rechecks that no TCP
  client has grabbed `:8888` between detection and the actual flash.

* **`set_bridge_baud()` / `set_bridge_flow_control()`** helpers with
  read-back verification — catches silent sysfs write failures.

* **`tail -1`** on `FIRMWARE_BAUD` lookup so duplicate keys
  (manual edits, stale migrations) resolve to the last value rather
  than the first.

### Init scripts — defensive `radio.conf` parsing

`S70otbr` and `S50uart_bridge` now apply `tail -1` to `radio.conf`
lookups (`FIRMWARE_BAUD`, `BRIDGE_BAUD`, `BRIDGE_BIND`, `OTBR_BAUD`).
Same defensive pattern as `flash_efr32.sh`. No functional change for
clean configs; only matters when `radio.conf` has duplicate keys.

### Upgrade

```sh
./flash_install_rtl8196e.sh -y <gateway-IP>
```

In-place upgrade. Existing `radio.conf` is preserved across the upgrade,
so v3.2.x → v3.3.0 introduces no migration friction. The
`uart-flow-control=true` flag in the new `S70otbr` activates on the
first reboot after userdata is updated.

For users running v3.1.x or v3.2.x who want to verify the fix landed:

```sh
ssh root@<gw> "stty -F /dev/ttyS1 -a | grep -o '\\bcrtscts\\b.\\?'"
# expected: crtscts        (no leading dash)

ssh root@<gw> "devmem 0x18002110 32"
# expected: 0x2B000000     (AFE bit 29 is set)

ssh root@<gw> "cat /proc/tty/driver/serial | grep ttyS1"
# expected: no 'oe:' field after sustained 460800 traffic
```

### Known issues

* `S40button` may receive a SIGSEGV from busybox after some hours of
  idle polling. One occurrence observed in ~1h21 of dev-box uptime; not
  reproducible on demand. The shell process dies; the long-press →
  `recover_efr32` recovery surface is silently lost until the next
  reboot. Does **not** affect the radio path (`otbr-agent`,
  `S50uart_bridge`, `S70otbr` are independent). Pre-existing — not
  introduced by v3.3.0. Tracked for a follow-up release: a supervisor
  that respawns `S40button` if it exits unexpectedly.

### Acknowledgments

* @olivluca — the 14h baseline at 115200 vs the failure at 460800 was
  the data point that pointed the investigation in the right direction.
* @skinkie — earlier `radio.conf` mandatory work (#93, v3.2.1) made
  this release simpler to ship.

---

## [3.2.1] - 2026-04-29

Patch release on top of v3.2.0. Two related fixes around
`/userdata/etc/radio.conf`, both reported by @skinkie in
[#93](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/93):
the file is now always present after a fresh install (was previously
deleted on Zigbee installs), and `flash_efr32.sh` no longer silently
swallows the SSH error when the post-flash radio.conf write fails.

### `radio.conf` is now mandatory after install (#93)

`build_fullflash.sh` previously deleted `radio.conf` whenever
`RADIO_MODE=zigbee` (or the user picked "Zigbee" interactively) — the
Zigbee/OTBR mode was signalled purely by the file's *presence*. That
left the in-kernel UART bridge with no reference for the chip-side
baud (defaulted to 460800 in the driver), and a fresh-from-Tuya
gateway whose chip is at 115200 was mismatched out of the box.
Worse, `flash_efr32.sh` couldn't read the current `FIRMWARE_BAUD`
either — every probe started blind.

Fix: `radio.conf` is now always written, with at minimum `FIRMWARE`
and `FIRMWARE_BAUD`:

| Mode | `radio.conf` contents |
|---|---|
| Zigbee (default) | `FIRMWARE=ncp` + `FIRMWARE_BAUD=115200` |
| Thread | `FIRMWARE=otrcp` + `FIRMWARE_BAUD=460800` + `MODE=otbr` |

The Zigbee defaults match Tuya stock and v2.x first-flash state, so a
freshly-installed gateway talks to its EFR32 out of the box without
needing `flash_efr32.sh`. Any later flash to a non-default baud (or
firmware) rewrites these keys to match.

### `flash_efr32.sh` — hard-fail on radio.conf write errors (#93)

The post-flash block that writes `radio.conf` over SSH used to
trail with `2>/dev/null || true`, which silently masked any SSH
failure (auth, transport, gateway rebooting). The script then
printed `Flash complete.` while the file on the gateway was stale
or empty — exactly what @skinkie observed in
[#93](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/93):
chip flashed at a new baud, gateway-side bridge still on the old
one, link broken on next boot.

Fix: the write is now `if ! ssh_gw "..."; then exit 1; fi` with an
actionable error message — exact `echo … > /userdata/etc/radio.conf`
recovery commands so the user can fix it by hand, plus a hint that
re-running the script will pick up where it left off (the chip is
already on the new firmware; the second run only updates
radio.conf).

### Upgrade

```sh
./flash_install_rtl8196e.sh -y <gateway-IP>
```

In-place upgrade. Existing `radio.conf` content is preserved, so
v3.2.0 → v3.2.1 introduces no migration friction. The new defaults
only affect *fresh* installs (no LINUX_IP / no saved config to
preserve).

---

## [3.2.0] - 2026-04-28

Two reliability fixes that touch core boot infrastructure: the
`boothold && reboot` mechanism becomes 100 % reliable on Linux 6.18,
and the front-panel button handler is hardened against spurious
long-press detection. Because the bootloader's HOLD address changed,
**this release requires a fullflash** when upgrading from v3.1.x or
earlier — per-partition upgrade across this boundary is unsupported.

### Bootloader V2.6 + `boothold` — move HOLD flag to high DRAM (0x01FFEFFC)

The boot-hold mechanism (Linux writes a magic word to a fixed DRAM
address; bootloader reads it after watchdog reset and stops at the
`<RealTek>` prompt) regressed on Linux 6.18: at the v2.x address
`0x003FFFFC` it hit ~73-87 % reliability instead of the 100 % we got
on Linux 5.10. The kernel was scribbling low DRAM during early init
or shutdown, before the `reserved-memory no-map` declaration is
enforced — independent of `boothold` itself (the helper binary is
bit-identical between v3.0.1 and v3.1.1).

Bisection across v2.1.6 / v3.0.0 / v3.1.1 with mixed kernel/userland
combinations pinned the regression to the 5.10 → 6.18 transition (and
its toolchain refresh). Mitigations attempted at the original address
(`mmap` + `O_SYNC`, `__sync_synchronize`, `usleep` between write and
reboot) did not break 80 % reliability — the conflict is in the
kernel's memory access patterns, not the cache path.

The fix moves HOLD to the page just below the btcode stack
(`0x01FFEFFC` — high DRAM, 32 MB − 8 KB) and reserves it in the device
tree as `reserved-memory` with `no-map`:

* `31-Bootloader/boot/main.c` — read `BOOTHOLD_RAM = 0xA1FFEFFC` (KSEG1
  uncached), clear it before `goToDownMode()`. Bootloader bumps to
  **V2.6** (was V2.5).
* `34-Userdata/boothold/boothold.c` — `pwrite(/dev/mem, ..., 0x01FFEFFC)`.
  Helper itself is simpler than v3.1.1 — no cache flushes, no
  `O_SYNC` dance: the new address is in a `no-map` page, the kernel
  never touches it, and KSEG1 from the bootloader bypasses cache.
* `32-Kernel/files-6.18/arch/mips/boot/dts/realtek/rtl8196e.dts` —
  new `boothold@1ffe000` reserved-memory node (4 KB, `no-map`).
* `31-Bootloader/doc/REBOOT_TO_BOOTLOADER.md` — full rewrite with
  the regression analysis, address-safety map, and upgrade path.

Validated at the new address: **30 / 30 = 100 %** of `boothold &&
reboot` cycles enter download mode on v3.2.0 (vs 22 / 30 = 73 % on
v3.1.1 at the old address).

**Upgrade path: fullflash required.** A bootloader V2.5 + boothold
v3.2.0 mismatch (or vice versa) leaves `boothold && reboot`
non-functional, since they read/write different addresses. Use:

```
./flash_install_rtl8196e.sh -y <gateway-IP>
```

(`flash_remote.sh` per-partition upgrades across the v3.1.1 →
v3.2.0 boundary will not work.)

### `S40button` — defense-in-depth against spurious long-press

`S40button` polls GPIO 9 every 100 ms and fires `recover_efr32` on a
5 s sustained press. The poll loop trusted the GPIO data register
without checking that the line had ever been observed HIGH, that a
single LOW reading could be a noise sample, or that the pin mux was
still in GPIO mode. Any of those could in principle synthesise a
phantom long-press and reset the EFR32 from thin air.

Four mitigations land together as defense-in-depth, none of them
trusting the others:

1. **Edge detection** — start disarmed; only count LOWs after observing
   a HIGH on GPIO 9. Guards against a stuck-low pin or wrong pin mux at
   startup.
2. **Debounce** — require 3 consecutive LOW polls (~300 ms) before
   treating it as a real press. Filters single-sample noise.
3. **Mux re-verification** — before counting a press, re-read CNR. If
   GPIO 9 was flipped back to peripheral mode at runtime, restore GPIO
   mode, log it, and require re-arming via HIGH.
4. **Logging** — every state transition (armed, press, release, mux
   flip, long-press fire) goes through `logger -t S40button` for
   post-mortem diagnostics.

Behaviour on a healthy board is unchanged — a real 5 s long-press
still triggers `recover_efr32 -q`. Edge cases that previously could
fire spuriously now stay silent and leave a syslog trace.

> **Note on [discussion #89](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/discussions/89).**
> An early hypothesis suggested `S40button` misfiring might be
> behind the EFR32 dropping out after ~1 h on @olivluca's OT-RCP
> gateway (`Reset info: 0x301 (PIN)` traces). The A/B test
> (`/userdata/etc/init.d/S40button stop` for several hours) showed
> no `S40button` or `nrst_pulse` log entries before the failure,
> and the `PIN` reset was confirmed to be the normal pulse from
> `recover_efr32` during `S70otbr` startup. **`S40button` is
> exonerated** for #89; the actual failure (HDLC parse errors →
> Spinel timeout) is still being investigated. The hardening above
> ships anyway as defensive cleanup of a path that was relying on
> too many implicit assumptions.

### Flash scripts — SSH auth UX (issue #90)

`lib/ssh.sh` carried `BatchMode=yes` since v3.1.0 to defuse a 17-min
hang seen during the 5-baud NCP loop test. Side effect: silently
fails on encrypted private keys not loaded in `ssh-agent`, and
forbids password prompts entirely — leaving users without a key with
no path forward (reported by @skinkie).

The original hang was at the transport layer (`ssh root@gw 'true'`
stuck on a half-broken TCP), not at auth. `ConnectTimeout=5` +
`ServerAliveInterval=3` + `ServerAliveCountMax=2` already cover that.
`BatchMode=yes` was a redundant belt-and-braces that turned out to
bite users.

* **Drop `BatchMode=yes`** — auth-time prompts now go through (once).
* **Centralise SSH ControlMaster in `lib/ssh.sh`** — was previously
  duplicated in `flash_install_rtl8196e.sh` and `flash_remote.sh`,
  absent from `flash_efr32.sh`. Per-script `mktemp -d` socket dir
  (mode 0700, `${UID}-$$`-keyed) avoids collisions between concurrent
  runs and between users on a shared host.
* **`ssh_cleanup_multiplex` helper** — sends `-O exit` to every live
  master, drops the tmpdir. Idempotent; callers chain it into their
  existing EXIT trap.

User-visible result:

| Setup | Before | After |
|---|---|---|
| Key in agent (or no passphrase) | works | works (unchanged) |
| Encrypted key, no agent | silent "Permission denied" | passphrase prompted **once** |
| Root password only | silent "Permission denied" | password prompted **once** |
| stdin closed (CI) | silent fail | fails immediately after timeout |

The "once" comes from ControlMaster — every script does N back-to-back
SSH calls, all of which now ride a single multiplexed channel.

For full **non-interactive automation with password only** (CI, scripted
runs without a tty), a new `SSH_PASSWORD` env var feeds the password to
ssh via `sshpass`. The new `ssh_prime_with_password` helper in
`lib/ssh.sh` opens the ControlMaster up-front using `sshpass -e`;
subsequent ssh calls ride the multiplexed channel as usual. `sshpass`
is checked as a hard dependency only when `SSH_PASSWORD` is set;
interactive runs without it remain prompt-driven and don't need
`sshpass` installed.

```sh
# Interactive (default): ssh prompts, you type once
./flash_efr32.sh -y -g 192.168.1.88 ncp

# Non-interactive (no tty): SSH_PASSWORD is fed via sshpass
SSH_PASSWORD=root ./flash_efr32.sh -y -g 192.168.1.88 ncp
```

README's "What You Need" gains an explicit SSH access section
documenting the four modes (key in agent / encrypted key / interactive
password / non-interactive `SSH_PASSWORD`).

### `flash_install_rtl8196e.sh` — show user-chosen IP in post-install hints (issue #90)

The IP prompted at first-flash (default `192.168.1.88`, but @skinkie
typed `192.168.5.252`) was set inside `build_fullflash.sh`'s subshell
and never propagated back to the parent. The four post-install hint
lines fell through to the hardcoded default — telling users to
`ssh root@192.168.1.88` for a gateway that lives at `.5.252`. The
final `Flash with: ./flash_efr32.sh <IP>` hint additionally used the
v3.0 positional syntax dropped in v3.1.0 — that command now errors.

* Pre-create `SKEL_WORK` in the parent on first-flash too, so
  `build_fullflash.sh` writes `eth0.conf` into a dir the parent owns.
* Read `IPADDR` back from the generated `eth0.conf` after the build
  returns; introduce `GW_HINT_IP` resolved from `LINUX_IP` (upgrade)
  or `IPADDR` (first-flash) or the default.
* Rewrite the final hint to the current CLI shape:
  `./flash_efr32.sh -g <IP> ncp` (or `otrcp` when MODE=otbr).

### `flash_efr32.sh` — always use the pinned venv USF (issue #92)

Reported by @skinkie: `universal-silabs-flasher` exits with
`Error: No such option: --probe-methods Did you mean
--probe-method?` on a host that already has USF installed in
`~/.local/bin`.

The script's USF resolution preferred `command -v
universal-silabs-flasher` over the venv install — so any USF in
`$PATH` short-circuited the install path. Older USF releases use
`--probe-method` (singular, no comma-list) and none of them carry
our `DEFAULT_PROBE_METHODS` patch (extra bauds for EZSP / SPINEL /
CPC at 230400 / 691200 / 892857). The script then called the system
binary with the new CLI shape and patched bauds it didn't have.

Fix:

* Always use the pinned venv (USF 1.0.3 + our probe-methods patch).
  The system USF is ignored even when present.
* New `USF_ALLOW_SYSTEM=1` env var as an explicit escape hatch for
  operators who really want to point at a system install — with a
  warning that probe results may differ.

### Documentation

- `31-Bootloader/doc/REBOOT_TO_BOOTLOADER.md` — full rewrite
  (mechanism, address rationale, regression analysis table,
  upgrade-path note for the v3.1.1 → v3.2.0 fullflash boundary).
- `boothold.c` header — English narrative of the regression and
  why the new address is reliable.
- README "What You Need" — three SSH access modes (key / encrypted
  key / password), default password reminder.
- Per-version banners (`/etc/version`, `/etc/motd`, bootloader
  print) updated to v3.2.0 / V2.6.

---

## [3.1.1] - 2026-04-27

Hardening pass on the kernel UART bridge and a few operator-facing
rough edges sanded off the flash tooling. No new features; one config
simplification (`radio.conf` collapses two redundant baud keys into
one). Upgrade is in-place — no user action needed for legacy configs.

### Kernel — `rtl8196e-uart-bridge` hardening

Four correctness fixes landed together; one image rebuild covers
them all (`kernel-6.18.img` refreshed once at the end of the batch).
No operator-visible behaviour change in the normal path.

* **Drop `bridge_lock` across `nrst_pulse`'s `msleep(100)`.**
  `param_set_nrst_pulse()` used to hold the global mutex from the
  lazy syscon init through the full assert/sleep/release sequence,
  so the UART → TCP hot path stalled for ~100 ms on every EFR32
  reset. Real-world impact is minimal (the protocol asks operators
  to stop the radio daemon before pulsing), but holding a global
  mutex across `msleep` on a single-core CPU is sloppy. Narrowed
  `bridge_lock` to the syscon lookup; the assert/sleep/release runs
  under a new dedicated `nrst_pulse_lock` so concurrent pulses still
  serialize on the syscon bits.
* **Drop the stale IRAM promise from Kconfig and the
  `late_initcall` comment.** Both claimed the hot path lives in
  IRAM; in practice the IRAM placement was shelved after measurement
  showed plain `.text` was sufficient at 892857 baud (see
  `DESIGN.md` "Options considered and dropped"). Doc-only — keeps
  the built-in requirement, justifies it via `late_initcall`
  ordering and the no-unload contract instead.
* **Tighten `status_led_brightness` lock discipline.** The
  brightness param was wired through `module_param_named` with a
  direct write to the int variable; the worker thread read the same
  variable unlocked when firing the LED trigger on client connect.
  Real-world impact minimal (32-bit aligned int, RLX4181 won't
  tear), but it was the only live-tunable param bypassing the lock
  and KCSAN would flag the data race. Converted to `module_param_cb`
  with locked setter/getter (same pattern as `baud`, `port`,
  `bind_addr`, `flow_control`); the worker now snapshots the
  clamped value into a local under `bridge_lock` so
  `led_trigger_event` runs lockless without racing the setter.
* **Save and restore tty `client_ops` verbatim across arm/disarm.**
  `bridge_arm_locked()` replaced `tty->port->client_ops` with our
  hook, but disarm/err paths always wrote
  `tty_port_default_client_ops` back. Today that's the same pointer
  (ttyS1 carries the default at boot) so the bug is purely latent —
  but if the port ever gets a `serdev` or other layered consumer
  ahead of us, disarm would silently overwrite it. Now stashes the
  previous pointer in `state.saved_client_ops` at arm time and
  restores it from there.

### `flash_efr32.sh` — refuse to flash when the bridge is in use

Reads `/proc/net/tcp` on the gateway in the existing detection
heredoc; emits `PEER=IP:PORT` (or empty) alongside the other
`KEY=VALUE` lines. If non-empty, abort with the peer's IP so the
operator knows which host to silence. Otherwise the kernel bridge
would silently replace the existing client (`"replacing previous
client"` in `dmesg`) and let Z2M / ZHA / `otbr-agent` fight USF for
the socket. Covers Z2M wherever it lives — gateway, the host
running the script, or any third box (Pi/NAS) — `/proc/net/tcp` on
the gateway sees them all uniformly. No `--force` escape hatch:
stop the right process on the right machine.

### `backup_gateway.sh` + `flash_install_rtl8196e.sh` — defer the `BOOT_IP` L2 check (issue #88)

Both scripts ran an unconditional pre-flight check that `BOOT_IP`
(default `192.168.1.6`, the bootloader's hard-coded TFTP IP) was on
the same L2 segment. SSH backup and SSH-based firmware detection
don't need that — they only need `LINUX_IP` — so users on a routed
network were blocked before the script could do anything useful.

* `backup_gateway.sh` — drop the unconditional check and the
  now-unused `check_tftp` / `resolve_iface` helpers. SSH backup
  runs from any routable subnet.
* `flash_install_rtl8196e.sh` — new `require_boot_l2` helper
  (resolves `IFACE`, enforces L2, prints a `sudo ip addr add
  192.168.1.10/24 dev <iface>` hint). Called lazily — just before
  `boothold && reboot` on the upgrade path, immediately on the
  bootloader-only path. Backup proposal and config save still work
  from a routed network; failure now surfaces *before* the gateway
  is tipped into bootloader mode rather than after.

Reported by @skinkie on a v2.x → v3.1 migration with the gateway at
`192.168.5.252` and the host on the same routed subnet.

### `radio.conf` — single baud key

Pre-v3.1.1 `radio.conf` carried two redundant baud keys: a chip-
side `FIRMWARE_BAUD` (set at flash time, "what's on the chip") and
a host-side `BRIDGE_BAUD` (Zigbee path) or `OTBR_BAUD` (OTBR path)
read by the init scripts. Both ends of a UART link must agree, so
in practice the keys were always written and read at the same
value — duplication that only created room for divergence and
confusion.

Single source of truth: `FIRMWARE_BAUD`. Both `S50uart_bridge` and
`S70otbr` read this same key now.

* `flash_efr32.sh` stops emitting `BRIDGE_BAUD` / `OTBR_BAUD`
  entirely; the existing `sed` cleanup in the persist path strips
  legacy keys so old configs converge on every flash.
* `flash_install_rtl8196e.sh`'s v2 → v3 migration heredoc only
  writes `FIRMWARE_BAUD` now.
* Backwards compat: `S50uart_bridge` / `S70otbr` fall back to the
  legacy host-side keys when `FIRMWARE_BAUD` is absent, so a v3.0.x
  install upgrading to v3.1.1+ keeps working until the next
  `flash_efr32.sh` run strips them. **No user action needed.**

OT-RCP three-case switching (1/2 ↔ 3) becomes simpler too: just
add or remove `MODE=otbr` — no baud key to flip — see
[`26-OT-RCP/docker/README.md`](../2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/docker/README.md#switching-radio-mode-no-efr32-reflash-needed).

### Documentation

- ~14 README files swept to drop `BRIDGE_BAUD` / `OTBR_BAUD` from
  user-facing reference (canonical `radio.conf` reference in
  [`34-Userdata/README.md`](34-Userdata/README.md#radioconf-keys-full-reference);
  per-firmware READMEs in `24-NCP`, `25-RCP`, `26-OT-RCP`,
  `27-Router`; kernel-driver `README.md` / `DESIGN.md` /
  `SECURITY.md`; migration guide).
- `23-Bootloader-UART-Xmodem/firmware/README.md` — drop the dead
  link to the unshipped Stage-2-only `.s37` (`*.s37` is gitignored
  except the `-combined.s37` artefact); restored a green
  `mkdocs --strict` CI build.

---

## [3.1.0] - 2026-04-26

EFR32 radio recovery and a rock-solid `flash_efr32.sh`. The kernel
gains a write-only sysfs knob to chip-reset the EFR32 without touching
the SoC; userland gets a mode-aware recovery helper and a long-press
front-panel button handler that wires it all together. The companion
flash script is rewritten with proper CLI ergonomics, hardened SSH,
and ~120 lines of scan-baud heuristics removed (the kernel pulse
makes them obsolete). The `etc/version` component table is also
refreshed to reflect what v3.0 actually shipped.

### Kernel — `nrst_pulse` sysfs knob in `rtl8196e-uart-bridge`

- Write-only kernel param at
  `/sys/module/rtl8196e_uart_bridge/parameters/nrst_pulse`: writing
  `1` asserts the EFR32 `nRST` line via `PIN_MUX_SEL_2` bits {7,10,13}
  (sysc reg 0x44, mask 0x2480), holds 100 ms, releases. Recovers a
  stuck EFR32 (crashed app, J-Link halt, `pc==0xFFFFFFFF`) without
  rebooting the SoC.
- Implementation uses `syscon_regmap_lookup_by_compatible()` against
  `"realtek,rtl819x-sysc"` (same syscon used by `rtl8196e-eth`),
  looked up lazily on first pulse. Pulse is taken under `bridge_lock`,
  in-flight UART bytes during the reset are lost (expected).

### Userdata — front-panel button + recovery helper

- `skeleton/usr/sbin/recover_efr32` (new) — mode-aware. Reads
  `/userdata/etc/radio.conf`, stops the matching radio daemon
  (`S70otbr` for `MODE=otbr`, otherwise leaves `S50uart_bridge` up),
  pulses `nrst_pulse`, restarts the daemon. `-q` for quiet mode.
- `skeleton/etc/init.d/S40button` (new) — long-press handler on the
  front-panel button, GPIO 9 (port B bit 1, active LOW) — confirmed
  empirically on the v3.0 board. Holding ≥ 5 s invokes
  `recover_efr32 -q`; shorter presses ignored.
- `skeleton/etc/init.d/S70otbr` — `RCP_URL` now reads `OTBR_BAUD=`
  from `/userdata/etc/radio.conf` (default 460800). Lets
  `flash_efr32.sh otrcp` install a non-default-baud OT-RCP image and
  have `S70otbr` pick the matching baud on next boot.

### `flash_efr32.sh` — top-to-bottom rewrite

Motivated by a 5-baud NCP loop test that pinned the script for ~17
min on a stuck `ssh root@gw 'true'` (no `ConnectTimeout`, no
`ServerAliveInterval`). Parallel goal: real CLI ergonomics — flags,
`--help`, validation before any SSH.

- **CLI** —
  `flash_efr32.sh [OPTIONS] FIRMWARE [BAUD]`
  with positional `FIRMWARE` (`bootloader|ncp|rcp|otrcp|router`,
  numeric `1..5` also accepted) and optional `BAUD` (defaults
  per-firmware). Options: `-g/--gateway IP`, `-y/--yes`,
  `--no-reboot`, `-h/--help`. Env vars `FW_CHOICE` / `BAUD_CHOICE` /
  `CONFIRM` still honoured for one release with a deprecation warning.
- **`ssh_gw` wrapper** — every SSH call uses `ConnectTimeout=5`,
  `ServerAliveInterval=3`, `ServerAliveCountMax=2`, `BatchMode=yes`,
  three retries with 2 s/4 s backoff on transport failure (`rc=255`).
  No more silent infinite hangs after a gateway reboot.
- **Detection rewrite** — remote probe step returns a structured
  `KEY=VALUE` block (`STATUS`, `MODE`, `ARMED`, `BRIDGE_BAUD`) parsed
  with `awk`. Probes ALL four protocols (`ezsp+cpc+spinel+bootloader`)
  at the bridge baud regardless of `RADIO_MODE` — needed for
  bridge-mode OT-RCP that speaks Spinel under `MODE=zigbee`.
- **Pre-flight bootloader check** — first probe at bridge=115200 /
  `flow=0` with `bootloader:115200` covers the "previous flash left
  the chip in the Gecko Bootloader" case (empty app slot after a
  fresh bootloader install). Case-insensitive `grep -qi` catches
  `Detected.*BOOTLOADER` / `bootloader` USF variants.
- **Drop scan-baud (~120 LOC)** — the kernel `nrst_pulse` knob means
  the chip is always at the `radio.conf` baud after a pulse. The
  "stale `radio.conf`" failure path now prints a clear
  "power-cycle and retry" message instead of looping.
- **GBL resolution by glob** — `resolve_firmware()` `ls -1t` the
  `firmware/` dir for the right pattern and picks the most recent
  match. Removes the EmberZNet SDK lookup — the script now runs on a
  host without any Silabs tools.
- **Cleanup is non-rebooting** — `cleanup()` restores bridge state
  and prints "reboot manually if needed". The gateway reboot only
  happens at the end of the success path (suppressible with
  `--no-reboot` for chained flashes).
- **Z3-Router CLI fallback symmetric** — earlier draft fired the
  router-specific `bootloader reboot` CLI hack unconditionally when
  the target was the router, breaking every "non-router → router"
  path (CLI bytes hit a chip that doesn't speak CLI, bridge ends up
  at 115200, downstream probe fails). Now the standard probe runs
  first (so NCP/RCP/OT-RCP/bootloader paths are unaffected) and the
  CLI fallback fires whenever the standard probe fails — handles
  both `router → router` AND `router → ncp/rcp/otrcp` migrations
  (the symmetric case where the chip is already running the router
  firmware and doesn't speak EZSP/CPC/Spinel/Gecko-BTL at all).
- **115200 baud fallback** — when the standard probe at the
  `radio.conf` baud fails, try `ezsp+cpc+spinel+bootloader@115200`
  before giving up. Covers three real-world cases that the v3.1
  nrst_pulse-only design didn't handle on its own (because nrst_pulse
  resets the chip but doesn't change its firmware-baked-in baud):
  * **Tuya stock NCP** — the original Tuya factory firmware runs at
    115200; `flash_efr32.sh` can now upgrade straight from a fresh
    Tuya → custom-Linux install without manually editing `radio.conf`
    first. Validated end-to-end on hardware: chip detected as `EZSP
    6.5.0.0 build 188` running on Tuya bootloader `1.8.0`, then
    cleanly upgraded to NCP 7.5.1 @ 460800 + the new
    `BOOTLOADER_VERSION=1.8.0` (the Tuya bootloader survives — no
    Stage 2 reflash needed) recorded in `radio.conf`.
  * **Stale `radio.conf`** — user manually flashed the EFR32 outside
    this script (Simplicity Commander / J-Link) or the file carries
    a value from a previous firmware no longer on the chip.
  * **Factory state / cold-boot at 115200** — any chip booting at
    the Gecko default after a reset.

  Skipped when `radio.conf` already says 115200 (the standard probe
  just tried that, no point re-trying). The 115200 fallback runs
  before the Z3-Router CLI fallback, so the worst-case slow path is:
  standard probe (~23 s) → 115200 fallback (~23 s if no match) →
  router CLI (~8 s) → final error.

End-to-end validated on hardware (Z2M+cpcd-zigbeed back online with
EmberZNet 8.2.2 / EZSP v18 between every transition):

| Source firmware | Target | Path |
|---|---|---|
| RCP @ 460800 | router | standard probe (CPC → BTL) |
| router | router | CLI fallback |
| router | NCP @ 460800 | CLI fallback |
| OT-RCP @ 460800 | router | standard probe (Spinel → BTL) |
| router | RCP @ 460800 | CLI fallback |
| NCP @ 460800 | RCP @ 460800 | standard probe (EZSP → BTL) |

Plus the original 5-baud NCP sweep (115200..892857), 3 OT-RCP use
cases (ZoH, OTBR-host, OTBR-gateway), and chained bootloader→RCP
flash.

### SSH helpers — `lib/ssh.sh`

The v3.1 ssh-hang fix (the one that turned a 17-min loop test into a
2-min one) was originally inline in `flash_efr32.sh`. Promoted to a
sourceable lib so the same hardening protects the other flash
scripts that talk to the gateway over SSH.

* `lib/ssh.sh` (new) exports:
  - `SSH_HARDEN_OPTS` (bash array) — `ConnectTimeout=5`,
    `ServerAliveInterval=3`, `ServerAliveCountMax=2`, `BatchMode=yes`.
  - `ssh_retry` — `ssh` wrapper that retries only on `rc=255`
    (transport failure) up to 3 times with 2 s/4 s backoff. Real
    remote-command exit codes pass through unchanged.
  - `wait_for_port` — TCP-port-ready poll helper.
* `flash_efr32.sh`, `flash_remote.sh`, and `flash_install_rtl8196e.sh`
  all source `lib/ssh.sh`. The latter two had been re-implementing the
  v3.0-era SSH option string (no `ServerAliveInterval`, no retry on
  `rc=255`) and shared the v3.1 ssh-hang failure mode unfixed —
  validated on a real v2.1.2 → v3.1.0 production migration that
  involved several back-to-back SSH commands plus a mid-session
  reboot, all of which used to be hang candidates.

### `flash_install_rtl8196e.sh` — v2 → v3 migration

Pre-v3.0 firmware (which ran `serialgateway`) had no
`/userdata/etc/radio.conf` — the EFR32 baud was hard-coded to 115200
(NCP-UART-HW @ 115200, the v2.x default). The v3.x in-kernel UART
bridge defaults to 460800 when `radio.conf` is missing, which left
v2 → v3 upgrades with the host bridge mismatched against a chip still
running at 115200. Z2M / ZHA could not reach the coordinator until
the user manually wrote `radio.conf` or reflashed the EFR32.

`flash_install_rtl8196e.sh` now detects the v2 → v3 case (saved
`FW_VERSION` major < 3 AND no `radio.conf` in the restored skeleton)
and pre-seeds the new `userdata.bin` with the known v2.x state:

```
FIRMWARE=ncp
FIRMWARE_BAUD=115200
BRIDGE_BAUD=115200
```

so the gateway boots into a working state out of the box. Caught on
the maintainer's prod gateway during a real v2.1.2 → v3.1.0 upgrade.

### `radio.conf` — chip-identity keys

Until now `radio.conf` told you the bridge baud and the daemon stack
to use, but not which app firmware was on the EFR32. A
`BRIDGE_BAUD=460800` could mean NCP@460800, RCP@460800, or OT-RCP in
bridge mode @460800 — only a `universal-silabs-flasher probe` could
disambiguate. `flash_efr32.sh` now also writes four informational
keys at every successful flash:

```
FIRMWARE=ncp             # ncp | rcp | otrcp | router  (CLI alias vocab)
FIRMWARE_VERSION=7.5.1   # only when GBL filename embeds it (NCP, Router)
FIRMWARE_BAUD=460800     # the chip's UART baud at flash time
BOOTLOADER_VERSION=2.4.2 # Gecko Bootloader Stage 2 — refreshed on every flash
```

There is no `FIRMWARE=bootloader` value: the Gecko Bootloader is a
runtime mode (chip stuck on empty/corrupt app slot), not an
application. A bootloader-only flash leaves `FIRMWARE`/`FIRMWARE_BAUD`
untouched (the existing app stays valid) but DOES update
`BOOTLOADER_VERSION` (since that's the one piece of state it changed).
For app flashes, USF transits the bootloader on its way to upload the
GBL and logs the bootloader version in passing — `flash_efr32.sh`
parses that line and persists it.

The `flash_install_rtl8196e.sh` v2 → v3 pre-seed (above) writes the
chip-identity keys describing the known v2.x state; `BOOTLOADER_VERSION`
is left absent until the next `flash_efr32.sh` invocation populates it.

### `etc/version` — component table refreshed

The component list had been frozen at the v2.x line since the v3.0
bump. Updated to reflect what v3.0 actually shipped:

| Component | was | now |
|---|---|---|
| crosstool-NG | 1.28.0.3_a3fef85 (gcc 8.5.0 + binutils 2.34) | 1.28.0 (gcc 15.2.0 + binutils 2.45.1) |
| musl | 1.2.5 | 1.2.6 |
| linux | 5.10.252 | 6.18.24 |
| busybox | 1.37 | 1.37.0 |
| dropbear | 2025.89 (incorrect) | 2025.88 |
| serialgateway | 3.0 | *(removed — replaced by in-kernel uart-bridge)* |
| otbr-agent | *(absent)* | thread-reference-20250612+ commit 111e78d0 |

### Documentation

- ~24 README files swept for v3.1 reality across two passes: first
  the new `flash_efr32.sh` CLI / baud-aware GBL filenames / `OTBR_BAUD`
  key / three OT-RCP deployment patterns (ZoH host, OTBR-in-docker,
  OTBR-on-gateway); second the new `radio.conf` chip-identity keys
  (`FIRMWARE`, `FIRMWARE_VERSION`, `FIRMWARE_BAUD`) — canonical full
  reference in [`34-Userdata/README.md`](34-Userdata/README.md#radioconf-keys-full-reference),
  per-firmware READMEs link to it instead of duplicating.
- `2-Zigbee-Radio-Silabs-EFR32/POST-MORTEM-bootloader-recovery.md`
  (new) — documents why a hardware `nRST` pulse can't enter the Gecko
  Bootloader on this board (PIN reset always boots the app slot;
  `BTL_GPIO_ACTIVATION` pin not wired; Tuya stock confirms no
  hardware recovery), and traces the empirical evidence behind the
  `nrst_pulse` design.
- `25-RCP` and `26-OT-RCP` Z2M `configuration.yaml` examples now
  externalise the device list (`devices: devices.yaml`) like 24-NCP
  does — keeps personal IEEE addresses out of git.

---

## [3.0.1] - 2026-04-25

Point release fixing `flash_efr32.sh` recovery paths reported on day 1
of v3.0.0 (GH discussion #86, @olivluca). No kernel/rootfs/bootloader
change.

### `flash_efr32.sh`

- **Self-arm bridge in `MODE=otbr`** (commit `ebd4199`): script now
  stops `S70otbr`, arms the bridge at `BRIDGE_BAUD=` (default 460800),
  keeps `flow_control=1` for the Spinel probe, drops it to 0 only when
  USF enters the Gecko Bootloader.
- **Targeted probe uses `RADIO_MODE` only**, not baud-AND-mode — Spinel
  is picked whenever `MODE=otbr`, at whatever `CURRENT_BAUD` was
  self-armed. Fixes 2.1.6 → 3.0 migration where `BRIDGE_BAUD=` is
  missing.
- **Scan-baud no longer skips `CURRENT_BAUD`** — handles the case where
  the targeted probe failed for protocol reasons (wrong `MODE`), not
  baud reasons. End-to-end validated on a deliberate RCP→OT-RCP flash
  with `MODE=otbr` mismatch.

### Documentation

- `34-Userdata/README.md` § 8 documents `/userdata/etc/radio.conf`
  (`MODE=`, `BRIDGE_BAUD=`, `BRIDGE_BIND=`).

---

## [3.0.0]

Platform-level overhaul: single kernel line, UART↔TCP bridge moves
in-kernel, **the rewritten `rtl8196e-eth` driver becomes the only ethernet
path and delivers +47 % TCP TX / +8.5 % TCP RX over the legacy Realtek SDK
driver** (see the perf table below), all native binaries rebuilt against
the Alpine-rebased Lexra cross-toolchain (GCC 15.2 / binutils 2.45 /
musl 1.2.6). See `../1-Build-Environment/CHANGELOG.md` for the toolchain
side.

### Kernel — Linux 5.10 dropped, 6.18.24 becomes mainstream

- 5.10 tree, patches, config and pre-built `kernel.img` removed. 6.18
  is the single supported line, vanilla 6.18.24 + `patches-6.18/` +
  `files-6.18/`.
- `build_kernel.sh`: `KERNEL_VERSION` (6.18.24) decoupled from
  `KERNEL_MAJOR_MINOR` (6.18) so future point-release bumps are a
  one-line edit; `-v`/`--version` flag dropped. Overlay re-synced every
  run via `rsync -a`, closing the "edited files-6.18/X but build was a
  no-op" footgun.
- Output `kernel.img` renamed to `kernel-6.18.img` across scripts,
  docs and `.gitignore` exception.
- `build_rtl8196e.sh kernel` passes `clean` so the tree is always built
  from scratch against the current toolchain (make alone is
  toolchain-unaware).

### Kernel — in-kernel UART↔TCP bridge replaces userspace `serialgateway`

New kernel driver `rtl8196e-uart-bridge` (built-in,
`CONFIG_RTL8196E_UART_BRIDGE=y`) shuttles bytes between UART1 (Zigbee
radio) and TCP:8888.
- Module parameters for live reconfig (`baud`, `port`, `bind_addr`,
  `flow_control`, `enable`), mirrored by `/userdata/etc/radio.conf`
  (`BRIDGE_BAUD=`, `BRIDGE_BIND=`).
- STATUS LED tied to the `uart-bridge-client` LED trigger (on when a
  TCP client is connected, off on disarm).
- Security + robustness audit pass (batch F1–F9): accept/disarm race,
  short-write retry, sendmsg-shutdown, enable-lock, license tag,
  disarm-path UAF, lock-scope, IRAM hot-path mutex cost review.
  Hardening recipes co-located with the source in `SECURITY.md`.
- Userspace `serialgateway` daemon and `S50serialgateway` removed;
  `S50uart_bridge` replaces them.

### Kernel — rtl8196e-eth becomes the sole ethernet driver (v2.3)

The from-scratch `rtl8196e-eth` replaces the legacy Realtek SDK driver
(`rtl819x`, ~7000 LOC, dropped with Linux 5.10) and is now the only
ethernet path shipped. Benchmarked on the gateway's ~380 MHz single-core
Lexra against the legacy SDK driver it replaces:

| Test | `rtl819x` (legacy SDK) | **`rtl8196e-eth` v2.3** | Delta |
|---|:---:|:---:|:---:|
| TCP RX (host → gw) | 86.6 Mbit/s | **94.0 Mbit/s** | **+8.5 %** |
| TCP TX (gw → host) | 48.1 Mbit/s | **70.6 Mbit/s** | **+47 %** |
| TCP parallel 4/8 streams | — | 95.1 / 95.9 Mbit/s | line-saturating |
| TCP 5-min stress | — | 94.1 Mbit/s, 11 retrans on 2.46 M (0.00 %) | — |
| UDP 10/50 Mbit RX | — | 0 % loss | — |

Code size: **~1 900 LOC total** across the rewritten driver, a **5.2×
reduction** from the legacy SDK blob, with modern Linux networking
idioms (NAPI, phylib, devicetree, regmap/syscon, DMA coherency handled
explicitly for the non-coherent L1 cache).

Hardening since v2.2 (2026-04-16):

- RX: double-reserve on the initial pool fixed, `NET_IP_ALIGN` reserved
  on pool init, `wback_inv` ordered before handover to `SWCORE_OWNED`.
- `led_mode` sysfs migrated to `attribute_group` (F8 refactor).
- NAPI deferral tuned for slow Lexra CPU.
- Audit findings F6 and F11+F13+F15 tested on hardware and **rejected**
  — each introduced measurable perf regressions; see
  `POST-MORTEM-driver-perf.md`.

### Kernel — UART baud ceiling raised from 230400 to 892857 (N+1 divisor fix)

Root cause was not userspace latency (as long believed) but an
off-by-one in the RTL8196E UART divisor: the hardware interprets
DLL/DLM as (N+1), not N. The fix is a `port->set_divisor` hook that
programs `quot - 1`. Max achievable is **892857 baud**
(200 MHz / (16 × 14), 0 % error); 921600 is unreachable on this silicon.

| Baud | Divisor | Wire baud | Error | Status |
|------|---------|-----------|-------|--------|
| 115200 | 108 | 115741 | +0.47% | Default |
| 460800 | 27 | 462963 | +0.47% | 8 h soak OK |
| 691200 | 18 | 694444 | +0.47% | Tested |
| 892857 | 14 | 892857 | 0.00% | 2 h soak OK |

Also under `8250_rtl819x`: batch-1 audit (IRQ errno, flow-ctrl MCR
alias, probe defer), `devm_platform_get_and_ioremap_resource`
migration, hardened probe observability.

### Kernel — driver metadata + GCC 15 hygiene

- `rtl8196e-eth` v2.2 → v2.3; first explicit version 1.0 for
  `8250_rtl819x` and `rtl8196e-uart-bridge`. `MODULE_VERSION` set,
  `<driver> v<version> (J. Nilo)` boot banner on probe, visible via
  `/sys/module/*/version`.
- `-Warray-compare` silenced in `plat_mem_setup`
  (`__dtb_start != __dtb_end` → `&__dtb_start[0] != &__dtb_end[0]`).

### Rootfs — BusyBox on Alpine edge patches, binaries rebuilt

- BusyBox 1.37.0 adopts Alpine-edge's downstream patch set
  (see `33-Rootfs/busybox/ALPINE-PORT.md`). Same version, same applet
  set, fewer project-local patches.
- `busybox` and `dropbearmulti` rebuilt against the Alpine-rebased
  Lexra toolchain.

### Userdata — bridge-aware, image-first flow

- `build_userdata.sh` packages the JFFS2 image from the
  already-committed skeleton binaries by default — a fresh clone no
  longer rebuilds nano (~2 min saved, no binary churn in git). Opt into
  full source rebuild with `--rebuild-components` (boothold + nano +
  otbr-agent + ot-ctl); `--components-only` used by
  `build_rtl8196e.sh`. `--jffs2-only` kept as alias for backward compat.
- Init-script echoes no longer interleave with the kernel log.
- LED fixes: residual glow on boot, STATUS LED off until a service
  lights it.
- `S50uart_bridge` + `S70otbr` aligned on the in-kernel bridge path;
  otbr-agent uses `spinel+hdlc+uart:///dev/ttyS1` — `CONFIG_IEEE802154`
  is explicitly NOT required (wpan0 is a TUN; 802.15.4 stack lives in
  userspace + the EFR32 RCP firmware).

### Bootloader

- `boot.bin` rebuilt with the new toolchain (22 946 → 22 362 B).
- README's "Modern toolchain" blurb made version-agnostic.

### Flash helpers

- `flash_install_rtl8196e.sh` prereq check: capture `tftp --help`
  output before grepping, so `set -o pipefail` + tftp-hpa's exit=64
  don't falsely flag tftp-hpa as missing.
- `flash_remote.sh kernel` no longer needs `-v` or `KERNEL_VERSION=…`;
  `flash_kernel.sh` targets `kernel-6.18.img` directly.
- `build_fullflash.sh` / `create_fullflash.sh` updated to the new image
  name.

### Documentation

- `POST-MORTEM-6.18.md`: 5.10 → 6.18 arch port (CP0, atomics, cache,
  clocksource), UART bridge hardening, N+1 divisor investigation.
- `POST-MORTEM-driver-perf.md`: rtl8196e-eth RX regression hunt.
- UART bridge source ships with `DESIGN.md`, `README.md`, `SECURITY.md`.
- `PORT-6.18-STATUS.md` removed — port done.
- Top-level + 3-Main-SoC + 32-Kernel + ot-br-posix READMEs/CLAUDE.md:
  single kernel line, `kernel-6.18.img` everywhere, legacy rtl819x
  reference driver note corrected (dropped with 5.10).

---

## [2.2.0] - 2026-04-10

### Kernel — upgrade to 5.10.252

- **Linux 5.10.246 → 5.10.252**: 6 stable point releases with minor fixes.
  All 47 custom patches regenerated cleanly against 5.10.252 (4 with trivial
  line offset changes). Binary size unchanged (+80 bytes on vmlinuz).
  No relevant CVE for RTL8196E hardware in this range.

### Kernel — RLX4181 patch set cleanup

Audit triggered by the experimental Linux 6.18 port (branch `kernel-6.18`)
revealed that 3 of our long-standing Lexra patches were either no-ops or
hitting the wrong file. The 5.10 patch set is reduced from 47 to 45 patches
with **zero functional change**, and the remaining patches are now
structurally aligned with what is needed in 6.18.

- **`arch-mips-include-asm-pgtable-32.h.patch` removed**: the wrapper
  `#if CPU_R3000 || CPU_TX39XX || CPU_RLX4181` it added was a no-op for our
  build (always-true with `CONFIG_CPU_RLX4181=y`, and the inner branches were
  identical to the vanilla `#if CONFIG_CPU_R3K_TLB` path). The patch had been
  carried since the original 3.10 SDK without ever being functionally needed.
- **`arch-mips-mm-tlbex.c.patch` removed**: it replaced the vanilla
  `if (cpu_has_3kex)` branch with a `switch (current_cpu_type())` that added
  an explicit `CPU_RLX4181` case. Because `cpu_has_3kex` is defined as
  `!cpu_has_4kex` and our `cpu-feature-overrides.h` already forces
  `cpu_has_4kex=0`, the vanilla path automatically routes RLX4181 to
  `build_r3000_tlb_refill_handler()`. Same end result, less code.
- **`arch-mips-kernel-cpu-probe.c.patch` removed and replaced by
  `arch-mips-kernel-cpu-r3k-probe.c.patch`**: with `select CPU_R3K_TLB`
  added to the `CPU_RLX4181` Kconfig block (see below), the build now
  compiles `arch/mips/kernel/cpu-r3k-probe.c` (151 lines, R3K-class CPUs)
  instead of the much larger `cpu-probe.c` (~1900 lines, all CPUs). The
  `case PRID_IMP_LX4380` initializing `cputype`, `tlbsize`, `options`, and
  calling `lexra_cache_init()` moved to `cpu-r3k-probe.c`.
- **`arch-mips-Kconfig.patch` updated**: `config CPU_RLX4181` now includes
  `select CPU_R3K_TLB`. This activates the R3K TLB code paths in mainline
  (TLB exception generator, swap entry format, dump_tlb), which is what we
  want for the Lexra and which removes the need for the two patches above.

**Result**: 45 patches (was 47), `kernel.img` shrinks by ~4 KiB
(1 060 864 → 1 056 768 bytes), all vendor drivers and userland behavior
identical, boot tested on hardware (login prompt, eth0, OTBR).

### Userdata — component upgrades

- **nano 8.3 → 9.0**: text editor update. Binary grows 542 KB → 549 KB (+7 KB)
  with ncurses 6.6 (was 6.5). No functional impact — nano is a convenience tool
  for on-device config editing.
- **ncurses 6.5 → 6.6**: robustness fixes (null pointer checks, bounds checking).
  No security CVEs.
- **`build_otbr.sh`**: pinned default to commit `111e78d0` (thread-reference-20250612
  +327 commits, 2026-04-09) for reproducible builds. Previously defaulted to
  `main` branch. Script now installs binaries to skeleton automatically,
  restores working directory on exit, and uses `--single-branch` for faster clone.

### Build — BusyBox build script improvements

- **`build_busybox.sh`**: rewrote argument parsing with proper `case` statement.
  Added `clean` (remove build tree) and `--help` options. Version argument now
  validated with regex instead of being treated as default fallback.

### Security — BusyBox hardening

- **Compiler hardening**: added `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`,
  and full RELRO (`-Wl,-z,relro,-z,now`) to BusyBox build. Binary grows +20 KB
  (714 KB -> 734 KB).
- **CVE-2023-39810**: enabled `FEATURE_PATH_TRAVERSAL_PROTECTION` to prevent
  archive extraction outside the target directory (cpio, ar, rpm).
- **CVE-2025-46394**: sanitize terminal escape sequences in `tar -t` output to
  prevent filename concealment attacks.
- **CVE-2026-26157 / CVE-2026-26158**: fix tar hardlink path traversal and
  incomplete prefix sanitization. Hardlink targets are now stripped like regular
  filenames (matching GNU tar 1.34 behavior).
- **CONFIG_LFS=y**: enable Large File Support to match musl's 64-bit `off_t`,
  fixing 7 format-string warnings and potential truncation of file sizes > 2 GB.

---

## [2.1.6] - 2026-04-10

### Fixes
- **DHCP resilience**: udhcpc now uses `-b` instead of `-n`, so if the DHCP
  server is unavailable at boot the client keeps retrying in the background
  and configures the gateway automatically once a lease is obtained. Previously
  the gateway stayed without an IP until rebooted. (S10network — [#82])
- **`/var/hosts` updated on late DHCP lease**: `udhcpc.script` now regenerates
  `/var/hosts` in the `bound` handler, so the hostname→IP mapping is corrected
  when a lease arrives after initial failure. (udhcpc.script — [#82])
- **Hostname fallback uses `127.0.1.1`**: S15hostname now falls back to
  `127.0.1.1` (Debian convention) instead of `192.168.1.254` when no IP is
  assigned at boot. (S15hostname — [#82])

[#82]: https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/discussions/82

---

## [2.1.5] - 2026-04-04

### OTBR REST API — PascalCase kept

The PascalCase REST API patch is **kept**. `python-otbr-api` 2.9.0
(HA 2026.4) accepts camelCase in GET responses but still sends PascalCase
in PUT requests — upstream `otbr-agent` (camelCase) rejects them.
PascalCase `otbr-agent` works with all HA versions.
See [python-otbr-api#238](https://github.com/home-assistant-libs/python-otbr-api/issues/238).

### New features
- **Dropbear SCP & SSH client**: `scp` and `dbclient` (SSH client) added to
  the dropbear multi-call binary. Enables `scp` file transfers to/from the
  gateway and outbound SSH connections via `dbclient`. Progress bar included
  (`SCPPROGRESS`). Binary size: 473 KB → 555 KB (+82 KB).

### Fixes
- **rootfs.bin always rebuilt**: `build_fullflash.sh` and `flash_rootfs.sh`
  now always rebuild `rootfs.bin` from the skeleton, like `userdata.bin`.
  Prevents stale images from being flashed after an upgrade.
- **version/motd bumped** to v2.1.5.

---

## [2.1.4] - 2026-04-02

### New features
- **LED off mode**: `MODE=off` in `leds.conf` completely disables both LEDs
  (LAN + STATUS). The LAN LED is turned off via the `DIRECTLCR` register
  (0xBB804314) which controls the switch ASIC LED output scale — setting it
  to 0 fully disables the LED output with no residual glow.
- **`led_mode` sysfs**: now supports `bright`, `dim`, and `off`.
  `serialgateway` v2.2 and `S70otbr` respect `off` mode (STATUS LED stays
  at 0 even when the radio is connected).

### Changes
- **rtl8196e-eth v2.2**: added `DIRECTLCR` register support for true LED off.
- **serialgateway v2.2**: respects `led_mode=off` (keeps STATUS LED at 0).

---

## [2.1.3] - 2026-04-01

### New features
- **LED dual brightness mode**: new `led_mode` sysfs attribute
  (`/sys/class/net/eth0/led_mode`) allows switching between `bright`
  (default) and `dim` modes. In `dim` mode, both LEDs run at reduced
  intensity for nighttime use.
- **`leds-gpio-pwm` driver**: new GPIO LED driver with software PWM
  brightness control (0-255) via kernel timer_list (250 Hz). Replaces
  `gpio-leds` for the STATUS LED. At brightness 0 or 255 the timer is
  stopped (zero CPU overhead). Designed for SoCs without hardware PWM.
- **`S11leds` init script**: persistent LED mode via `/userdata/etc/leds.conf`.
  Set `MODE=dim` or `MODE=bright` (default). Applied at boot right after
  network init, before serialgateway/otbr-agent start.
- **`flash_efr32.sh` PWM guard**: disables status LED PWM before Xmodem
  transfer to avoid bus contention between GPIO writes and UART on the
  shared LX bus. Brightness is restored on reboot via S11leds.
- **Config preservation**: `leds.conf` added to `SAVE_FILES` in
  `flash_install_rtl8196e.sh` and `flash_remote.sh` — LED preference
  survives firmware upgrades.

### Bug fixes
- **LAN LED dim after Linux 5.10 port**: the LAN LED is hardwired to
  the switch ASIC LED_PORT0 output, not to the GPIO pad. GPIO control
  had no physical effect. Fixed: Ethernet driver now configures LEDCREG
  in LEDMODE_DIRECT after FULL_RST, restoring full-brightness
  link/activity indication as in the stock firmware.
- **STATUS LED invisible with serialgateway**: `_set_status_led()` wrote
  `"1\n"` to brightness. With `gpio-leds` (max=1) this was full-on, but
  with `leds-gpio-pwm` (max=255) it was 0.4% duty cycle — invisible.
  Fixed: serialgateway v2.1 reads `led_mode` and writes 255 (bright) or
  60 (dim). Same fix applied to S70otbr LED daemon.

### Technical notes
- **Hardware discovery**: PIN_MUX_SEL_2 bits [1:0] = 11 (GPIO mode)
  has no effect on the LAN LED — the PCB routes it directly to the
  switch ASIC LED output, bypassing the pin mux. Confirmed by register
  analysis: GPIO DATA register toggles correctly but LED does not
  respond; LEDCREG changes immediately affect the LED.
- Ethernet driver bumped to v2.1, serialgateway bumped to v2.1.

---

## [2.1.2] - 2026-03-23

### Bug fixes
- **Skeleton pollution / cross-contamination after flash**: `flash_remote.sh`,
  `flash_install_rtl8196e.sh`, `build_fullflash.sh`, and `create_fullflash.sh`
  injected gateway config into the skeleton directory, leaving residual files
  (dropbear keys, radio.conf, thread/) between runs. Flashing one device in
  OTBR mode then another in Zigbee mode leaked radio.conf. Refactored: all
  scripts now work on a temporary copy of the skeleton via `SKELETON_DIR`;
  the original is never modified. Credit: olivluca (#73).
- **S70otbr redundant flash writes**: sync daemon wrote to flash on first poll
  (seeded with empty `last_dataset`) and unconditionally on shutdown. Fixed:
  `last_dataset` seeded from REST API before entering the loop; trap and stop
  no longer copy — the daemon syncs on dataset change only. Frame counters
  are ephemeral (OpenThread recovers by jumping ahead). Credit: olivluca (#66).
- **Serial console backspace**: replaced `askfirst` + `login` with
  `getty -L 38400 ttyS0 vt100` in rootfs inittab — backspace now works
  at the login prompt.

---

## [2.1.1] - 2026-03-22

### Bug fixes
- **`boothold` unreliable via SSH**: BusyBox `devmem` writes through KSEG0
  (cached, write-back) — the HOLD flag could stay in L1 D-cache and be lost
  on watchdog reset. Replaced with a C binary (`boothold`) that uses
  `pwrite()` + `O_SYNC` on `/dev/mem` to force the write to DRAM.
- **JFFS2 decompression errors on fresh userdata flash**: `mkfs.jffs2 -X zlib`
  enables zlib but does not disable rtime (enabled by default). Added
  `-x rtime -x lzo` to force zlib-only compression — matches the kernel
  config (`CONFIG_JFFS2_ZLIB=y`, no rtime/lzo).
- **Skeleton pollution after flash**: `flash_remote.sh` and
  `flash_install_rtl8196e.sh` injected gateway config (passwd, eth0.conf,
  etc.) into the skeleton without cleanup. Added `rsync --delete` restore
  via EXIT trap — skeleton is always restored to its original state.

### Improvements
- **Bootloader V2.5**: auto-reboot after flashing all partition types
  (rootfs, bootloader — was only kernel).
- **`flash_remote.sh`**: two-phase bootloader detection (wait SSH down,
  then ARP) prevents false positives during shutdown. ControlMaster socket
  closed after boothold. Skip redundant boot mode check via
  `BOOTLOADER_CONFIRMED`. Quiet build mode via `BUILD_QUIET`.
- **`flash_install_rtl8196e.sh`**: firmware version displayed early (v2.1.0
  format). EFR32 compatible firmware list shown at end (depends on radio mode).
  Same two-phase detection and ControlMaster fix.
- **`build_rootfs.sh`**: quiet mode (`-q`) for auto-build from flash scripts.
- **S70otbr**: sync daemon uses REST API instead of `ot-ctl` (eliminates
  broken pipe warnings). Fast poll (5s) until Thread is up, then 30s.

---

## [2.1.0] - 2026-03-21

### Bug fixes
- **`boothold` fails on running system**: the kernel's page allocator actively
  uses the page at physical `0x003FFFFC` (KSEG0 cached), overwriting the HOLD
  magic written by `devmem` (KSEG1 uncached) within milliseconds. Fixed by
  declaring the page as `reserved-memory` with `no-map` in the device tree —
  the kernel never allocates it, eliminating the cache coherency conflict.
  Address kept at `0x003FFFFC` (top of DRAM is unsafe: btcode stack).
  Bootloader V2.4: BOOTHOLD_RAM uses KSEG1 (`0xA03FFFFC`) so the clear
  bypasses the write-back cache and reaches DRAM — prevents false boot-hold
  after power cycle.
- **Thread dataset lost on reboot**: S70otbr sync loop only ran 60s after boot —
  networks created later were never persisted. Replaced with a persistent daemon
  that polls `ot-ctl dataset active -x` every 30s and syncs to flash only when
  the dataset changes. Traps SIGTERM for a final sync on shutdown.
- **No shutdown hooks**: added `::shutdown:` entry to rootfs inittab, calling a
  new `rcK` script that stops all services in reverse order on reboot — ensures
  clean `stop` for otbr-agent and all other init scripts.

### New features
- **OTBR status LED**: S70otbr sync daemon polls `ot-ctl state` every 30s —
  LED on when Thread network is formed (child/router/leader), off otherwise.
  Replaces netdev trigger on wpan0 which did not reflect Thread network state.

### Improvements
- **Auto-flash on first flash**: `flash_install_rtl8196e.sh` now attempts auto-flash
  when `BOOTLOADER_TYPE=v2` even without SSH (first flash, `FW_VERSION` unknown).
  Worst case (old V2.3 without auto-flash): 3-min timeout then fallback to manual FLW.
- **EFR32 flash prompt**: at the end of `flash_install_rtl8196e.sh`, interactive mode
  now offers to launch `flash_efr32.sh` to flash the Zigbee/Thread radio firmware.
- **`userdata.bin` and `rootfs.bin` removed from git**: both binaries are now
  rebuilt on the fly by the build/flash scripts (skeletons and build tools are
  in git). `build_fullflash.sh` and `create_fullflash.sh` auto-rebuild
  `rootfs.bin` if missing. Skeleton backup/restore traps simplified.
- **`create_fullflash.sh` aligned**: now prompts for network/radio configuration
  and rebuilds userdata via `build_userdata.sh --jffs2-only` before assembly,
  matching `build_fullflash.sh` behavior.
- **Dropbear 2025.89**: updated from 2025.88.
- **EFR32 build scripts**: all firmware build scripts (bootloader, NCP, RCP,
  OT-RCP, Router) now consistently output exactly two files in `firmware/`:
  `.gbl` (for UART/Xmodem flashing) and `.s37` (for J-Link). Removed `.hex`,
  `.bin`, and intermediate `.s37` variants.

---

## [2.0.1] - 2026-03-17

### Bug fixes
- **DHCP wipes IPv6**: `udhcpc.script` uses `ip -4 addr flush` to preserve IPv6 link-local
- **Thread dataset not persisted**: S70otbr syncs to flash once Thread is up (was daily)
- **`tr: not found` in S70otbr**: replaced with shell parameter expansion
- **`/root` permissions**: fixed to 750 in rootfs skeleton (read-only squashfs)
- **SSH probe timeout**: `SSH_TIMEOUT` env var (default 2s) for slow networks
- **Auto-flash timeout**: 10s → 180s (flash write takes ~2 min)
- **`resolv.conf` overwritten by S15hostname**: removed, handled by S10network
- **Kernel .config warnings**: removed duplicate config entries
- **motd/version sync**: motd now shows the same version and date as `/etc/version`

### Improvements
- **`flash_install_rtl8196e.sh` refactored**: two distinct modes of operation:
  - **First flash** (no argument): gateway must be in bootloader mode, prompts
    for network/radio config, TFTP probe confirms bootloader presence
  - **Upgrade** (`LINUX_IP`): connects via SSH, saves user config (eth0.conf,
    mac_address, radio.conf, passwd, TZ, hostname, dropbear keys, SSH keys,
    Thread credentials), boothold + reboot, then flash. Prompts skipped
  - **`-y` / `--yes` flag**: non-interactive mode for fully automated upgrades
    (firmware >= v2.0.0 with auto-flash support)
- **Firmware detection via `devmem`**: distinguishes custom firmware from Tuya
  (even if Tuya SSH port was changed to 22) by checking `devmem` presence
- **TFTP bootloader probe**: ARP + TFTP PUT distinguishes bootloader from Linux
  running on `BOOT_IP` — prevents false positive detection
- **Auto-flash skip for firmware < v2.0.0**: reads `/etc/version` before boothold
  to skip the 3-minute nc listener on older bootloaders that lack UDP notification
- **Quiet build mode** (`-q`): `build_fullflash.sh` and `build_userdata.sh`
  suppress non-essential output (banners, cvimg details, image sizes) when
  called from `flash_install_rtl8196e.sh`
- **Removed `--boot-ip` parameter**: `BOOT_IP` is env-var only (always 192.168.1.6)
- **Config preservation on reflash**: prompts skipped when config is preserved
- **DNS/domain in eth0.conf**: S10network reads optional `DNS` and `DOMAIN` fields
- **SSH ControlMaster**: single password prompt instead of two
- **SSH auth check**: fail fast on bad password
- **Clean git checkout after flash**: `build_fullflash.sh` and `flash_userdata.sh`
  restore skeleton after build so `git pull` is not blocked
- **`flash_remote.sh` refactored** (renamed from `remote_flash.sh`):
  - `LINUX_IP` is now required (no more hardcoded default)
  - Dual-port SSH probe: port 2333 → Tuya error with redirect to `flash_install_rtl8196e.sh`
  - `devmem` check after SSH: absent = Tuya/v1.0 → same error
  - Boothold via `devmem` directly (no dependency on `boothold` binary)
  - Bootloader wait confirms SSH is down before declaring ready
  - Removed bootloader-already-up path (use individual flash scripts directly)
  - Added `-y`/`--yes` flag, `SSH_TIMEOUT` env var, `StrictHostKeyChecking=no`
  - Renamed to `flash_remote.sh` to match `flash_*.sh` naming convention

---

## [2.0.0] - 2026-03-13

### 30-Backup-Restore
- **`backup_gateway.sh`** (new, at repository root): unified backup script that auto-detects
  gateway state (custom Linux SSH:22, Tuya Linux SSH:2333, or bootloader) and chooses the
  best backup method. Outputs `fullflash.bin` + individual partition files + `backup.log`
  to `backups/YYYYMMDD-HHMM/`. Replaces `backup_mtd_via_ssh.sh` and `backup_rtl8196e.sh`.
- **`restore_gateway.sh`** (new, at repository root): restore a `fullflash.bin` backup to the
  gateway. Verifies 16 MiB size, detects bootloader type (V2 auto-flash vs V1.2/Tuya guided
  LOADADDR + FLW on serial console). Symmetric counterpart to `backup_gateway.sh`.
- Removed `backup_mtd_via_ssh.sh` (superseded by unified script SSH path)
- Removed `backup_rtl8196e.sh` (superseded by unified script bootloader path)

### 31-Bootloader
- **UDP notification after flash** (port 9999): bootloader sends "OK" or "FAIL" to the
  TFTP client after `checkAutoFlashing()` completes, enabling fully automated remote
  flashing without serial console confirmation
- **Raw fullflash auto-flash**: V2.3+ bootloader detects raw 16 MiB images by verifying
  magic bytes at partition offsets (bootloader at 0x0, cs6c at 0x20000, hsqs at 0x200000)
  and writes the entire image to flash — enables fully automatic install via TFTP
- Notification sent before `autoreboot()` so it arrives even for kernel images

### 32-Kernel
- **GPIO 11 / status LED fix**: Ethernet driver no longer clears PIN_MUX_SEL2 bits [4:3]
  that control GPIO 11 (Port B3), fixing the gpio-leds regression introduced in the
  procfs-to-gpio-leds migration
- **Ethernet driver v2.0** — optimized for OTBR/NCP-UART workloads:
  - TX IRQ mitigation: TX_ALL_DONE interrupt disabled, descriptors reclaimed in start_xmit and NAPI poll (eliminates 1 IRQ per TX packet)
  - Ring buffers reduced from 600/500 to 128/128 — saves ~780 KB RAM (3.8% of free memory)
  - TX stop/wake thresholds scaled proportionally (16→4, 64→16)
  - UDP TX throughput +28% vs conditional reclaim approach
  - OTBR use case validated: CoAP/mDNS traffic has 88× headroom vs UART bottleneck
  - 29 hot-path functions placed in 16 KB on-chip I-MEM (SRAM) via `__iram` section
- Syscon/regmap: PIN_MUX_SEL/SEL2 access coordinated via shared regmap (GPIO, UART, Ethernet)
- Interrupt controller: chained_irq_enter/exit, GIMR enabled after handler install, raw_spinlock on GIMR
- Ethernet: IRQ_NONE on spurious, tx_dropped accounting, napi_enable after HW init
- CONFIG_MIPS_L1_CACHE_SHIFT corrected from 5 (32 B) to 4 (16 B) to match actual RLX4181 cache line size
- Timer: bus clock from DT (busclk fixed-clock), max_delta_ticks capped to 28-bit, clk_prepare_enable
- SPI: unaligned access safety (get/put_unaligned), devm_clk_get_optional, double-disable prevention
- UART1: devm_clk_get_optional, dev_warn/err/dbg, PIN_MUX via syscon
- GPIO: spinlock on get_direction, pinmux via syscon/regmap
- LED: replaced custom /proc/led1 driver with standard gpio-leds DT binding (/sys/class/leds/status/)
- DT: syscon on system-controller, busclk fixed-clock, gpio-leds node
- Kconfig: CONFIG_MFD_SYSCON=y, CONFIG_LEDS_GPIO=y, CONFIG_LEDS_TRIGGERS=y
- IPv6 stack integrated into base config (+135 KB kernel, zero overhead when unused)
- CONFIG_FILE_LOCKING=y (required by otbr-agent flock())
- CONFIG_TUN=y (required for wpan0 Thread interface)
- Kconfig size reduction (-298 KB text, -106 KB compressed = -9.2%):
  - Stripped unused subsystems: MTD_CFI/JEDECPROBE, PHYLIB/MDIO, MSDOS/EFI_PARTITION,
    NLS, IKCONFIG, INET_DIAG, IPV6_SIT/TUNNEL, MIPS_FP_SUPPORT, IEEE802154, SHMEM
  - CRC32_SLICEBY8 → SLICEBY4 (-4 KB tables, better D-cache fit)
  - Disabled SYN_COOKIES (unnecessary behind NAT), NETFILTER (incompatible with
    RTL8196E Ethernet driver)
- Kernel size: 1.0 MB → 1.03 MB (net, after IPv6 addition and kconfig stripping)

### 33-Rootfs
- BusyBox: IPv6 support (ping6, traceroute6, ip route)
- Migrated all scripts from ifconfig/route to ip commands
- Removed ifconfig, route, microcom applets (replaced by ip, no longer needed)

### 34-Userdata
- serialgateway: LED control migrated from /proc/led1 to /sys/class/leds/status/brightness
- otbr-agent and ot-ctl binaries in /userdata/usr/bin/
- S70otbr init script: IPv6 forwarding, UART 115200, REST on :8081
- Build script: `ot-br-posix/build_otbr.sh` for cross-compilation

### Flash scripts
- **`flash_install_rtl8196e.sh`** (new): unified firmware installation script —
  builds `fullflash.bin`, auto-detects gateway state (custom Linux → boothold,
  V2 bootloader → auto-flash, old bootloader → guided FLW), handles Tuya and
  custom firmware. Replaces `flash_rtl8196e.sh` as the recommended install method.
- **`build_fullflash.sh`** (new): assembles bootloader + kernel + rootfs + userdata
  into a verified 16 MiB flash image with correct header stripping per partition
- **`flash_remote.sh`** (new): fully automated remote flash via SSH — connects to the
  gateway, sends `boothold`, waits for bootloader, runs the appropriate flash script.
  Supports all 4 components: `./flash_remote.sh <bootloader|kernel|rootfs|userdata>`
- All individual flash scripts (`flash_bootloader.sh`, `flash_kernel.sh`, `flash_rootfs.sh`,
  `flash_userdata.sh`) now wait for bootloader UDP notification ("OK"/"FAIL") instead of
  returning immediately after TFTP upload
- All build scripts (`build_kernel.sh`, `build_rootfs.sh`, `build_userdata.sh`) check
  for gcc before attempting to compile cvimg
- Non-interactive mode via environment variables: `CONFIRM=y` skips "Proceed?" prompt,
  `NET_MODE=static|dhcp` and `RADIO_MODE=zigbee|thread` skip userdata config prompts.
- Removed `flash_rtl8196e.sh` (superseded by `flash_install_rtl8196e.sh` and `flash_remote.sh`)

### Thread Border Router — OTBR on-device
- OpenThread Border Router runs natively on the RTL8196E gateway (no Docker, no PC)
- otbr-agent 0.3.0 (Thread 1.4) cross-compiled for MIPS Lexra, static binary (4.3 MB)
- ot-ctl CLI for Thread network management (57 KB)
- REST API on port 8081 — compatible with Home Assistant OTBR integration
- mDNS/DNS-SD (OpenThread built-in), SRP Advertising Proxy, Border Routing
- Tested: IKEA TIMMERFLOTTE commissioned via HA Companion App, 20 MB RAM free

### Unified Zigbee/Thread distribution
- Single kernel, rootfs, and userdata image for both Zigbee and Thread modes
- `flash_userdata.sh`: new "Radio mode" prompt selects Zigbee or Thread at flash time
- `/userdata/etc/radio.conf` (MODE=otbr) gates init scripts at boot
- S60serialgateway: skips when radio mode is OTBR
- S70otbr: starts only when radio mode is OTBR

### Build fixes (ot-br-posix)
- `-Wno-error=maybe-uninitialized` for GCC 8.5 false positive
- Socket path redirected to /tmp (rootfs is read-only, no /run)
- `--vendor-name` / `--model-name` required by latest ot-br-posix

---

## [1.2.0] - 2026-03-02

### 30-Backup-Restore
- Added custom firmware partition map (4 partitions) alongside the original Lidl/Tuya map
- Added FLR/FLW quick reference table for the custom layout (mtd3 = 12 MiB JFFS2 userdata)

### 32-Kernel
- Updated README: build process now describes zboot (in-tree `arch/mips/boot/compressed/`), corrected image size (~1 MB)
- PIN_MUX_SEL fix: UART1 TX/RX pins correctly muxed in both `rtl8196e-eth` and legacy `rtl819x` drivers — fixes EFR32 communication after Ethernet init
- PIN_MUX_SEL2: nRST clearing preserved for EFR32 reset control

### 33-Rootfs
- Fixed Dropbear pubkey auth by correcting `/root` permissions at startup

### 34-Userdata
- NTP: retry connectivity check in `S20time` for reliable time sync
- Dropbear: unified stop emoji, fixed restart logic
- `flash_userdata.sh`: network configuration (static IP or DHCP) asked at flash time

### Flash scripts (root level)
- New `flash_rtl8196e.sh` at repository root — flashes all RTL8196E partitions in one command
- New `flash_efr32.sh` at repository root — OTA flash of EFR32 via SSH + universal-silabs-flasher
  - Firmware selection menu (bootloader, NCP, RCP, OT-RCP, Z3-Router)
  - SSH retry (3 attempts, ConnectTimeout=10) for unreliable networks
  - Progress bar visible for normal firmware flash
  - Bootloader flash chains application firmware automatically
- Prerequisite checks: tftp-hpa (flash_rtl8196e.sh), python3 + venv (flash_efr32.sh)
- Deleted unused `clean_part1.sh`, `clean_part2.sh`, `clean_part3.sh`

### Documentation
- All EFR32 firmware READMEs (23-Bootloader, 24-NCP, 25-RCP, 26-OT-RCP, 27-Router) updated to reference `flash_efr32.sh`
- Root README rewritten: user-oriented intro, single quick start flow, firmware selection table
- `35-Migration` README rewritten to describe the two root-level flash scripts
- Fixed Z2M port syntax to `tcp://` across all READMEs
- `3-Main-SoC-Realtek-RTL8196E/README.md`: clarified flash script paths (root vs subdirectory)

---

## [1.1.0] - 2026-02-24

### 30-Backup-Restore (new)
- `backup_mtd_via_ssh.sh` / `restore_mtd_via_ssh.sh`: per-partition SSH backup and restore, original firmware only (5 partitions, port 2333); 4-partition layout rejected with FLR/FLW guidance
- README: method comparison table (SSH / FLR/FLW / SPI programmer), full FLR/FLW backup and restore procedure

### 31-Bootloader
- Fully rewritten from [Sourceforge V3.4.7.3 SDK](https://sourceforge.net/projects/rtl819x/files/) source code and adapted to the new lexra toolchain
- V2.3: ICMP ping support — `ping 192.168.1.6` works from download mode
- Boothold mechanism: reboot-to-bootloader from Linux via DRAM magic flag
  (`devmem 0x003FFFFC 32 0x484F4C44 && reboot`), RAM flag at 0x803FFFFC
- Download progress shown as percentage instead of endless `.` / `#`
- `flash_bootloader.sh`: ARP-based boot mode detection (rootless, no arping),
  `set -euo pipefail`, helper functions, background UDP ARP trigger with proper
  cleanup, `ip neigh del` flush before probing, `timeout 15` on tftp,
  `TRIES`/`PORT`/`SLEEP_BETWEEN` env-var overrides, clean error reporting

### 32-Kernel
- **New driver**: `rtl8196e-eth` — clean-room Ethernet driver (1 855 pure LOC
  vs 9 664 for legacy rtl819x, 5.2× reduction)
  - TCP RX: **91.2 Mbps** (+6.4% vs legacy 85.7 Mbps)
  - TCP TX: **46.9 Mbps** (+8.1% vs legacy 43.4 Mbps)
  - TCP stress 300s: 92.0 Mbps, 0 errors, 0 retransmissions (SoC side)
  - Architecture: NAPI, zero-copy RX (`napi_alloc_skb`), no spinlock, no BQL,
    devicetree-based configuration
- **New build system**: unified `build_kernel.sh` supporting both drivers
  (`./build_kernel.sh` for rtl8196e-eth, `./build_kernel.sh legacy` for rtl819x)
- **New decompressor**: zboot (`arch/mips/boot/compressed/`) replaces the
  external lzma-loader from the Realtek SDK — no external tool dependency
- Legacy `rtl819x` driver from initial release 1.0.0 remains available as a reference build

### 34-Userdata
- `/etc/version` updated to include firmware version
- `boothold` script installed in `usr/bin/`: one-command reboot-to-bootloader
  from Linux SSH, wraps `devmem 0x003FFFFC 32 0x484F4C44 && reboot` with a root check
- `flash_userdata.sh`: asks for network configuration (static IP or DHCP) before
  flashing; generates `skeleton/etc/eth0.conf` temporarily, rebuilds JFFS2, then
  flashes — `eth0.conf` is removed after flash (trap EXIT), skeleton stays clean

### Flash scripts
- Fixed invalid `-timeout` tftp flag in all scripts — replaced by `timeout N tftp` wrapper
- `flash_rtl8196e.sh`: fixed `set -e` silent exit, improved UX messages
- `flash_rtl8196e.sh`: optional FLR full flash backup before flashing, saved as `YYMMJJ-HH.MM-Gw-Backup.bin`
- `flash_rtl8196e.sh`: asks for network configuration (static IP or DHCP) before
  flashing, rebuilds userdata with the chosen config

---

## [1.0.0] - 2025-12-18

Initial release.

### 31-Bootloader
- Lidl/Tuya/Realtek original bootloader

### 32-Kernel
- Linux 5.10.246, legacy `rtl819x` Ethernet driver developed from original 2.6 code, lzma-loader decompressor

### 33-Rootfs
- musl 1.2.5, busybox 1.37, dropbear 2025.88

### 34-Userdata
- Init scripts: S20time (NTP), S30dropbear (SSH), hostname, eth0 config
