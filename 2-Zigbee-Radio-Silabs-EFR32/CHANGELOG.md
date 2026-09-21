# Changelog — Zigbee Radio / Silabs EFR32 (RTL8196E Gateway)

All notable changes to the EFR32 firmware and tooling are documented here.

---

## [4.5.1] - 2026-09-21

### Experimental same-channel Zigbee + Thread multi-PAN restored (#160, #161)

The Lidl EFR32MG1B RCP can now expose Zigbee and Thread concurrently when both
networks use the same 802.15.4 channel. The RCP project explicitly creates two
static OpenThread instances and reserves IID 0 for CPC broadcast; `zigbeed`
uses IID 1 and the GSDK-matched OTBR host uses IID 2. The host build enables
`OT_MULTIPAN_RCP`, the missing option behind the earlier
`GetIidListFromUrl()` failure, and includes the Silicon Labs CPC transport.

The release adds the reproducible amd64/arm64 OTBR Dockerfile, supervised
entrypoint, native-Linux Compose topology, host setup and operating limits.
GitHub Actions publishes the multi-architecture image as
`ghcr.io/jnilo1/multipan-otbr:4.5.1`. The rebuilt Lidl RCP image is
`rcp-uart-802154-460800-hw.gbl`, SHA-256
`9a5bce1502a6bb2143aad303b10a403f435500c28e373f1f684a55773da967ff`.

Validation covered native amd64 and QEMU arm64 image builds, runtime library
resolution, Compose and firewall lifecycle checks, and a hardware smoke test
on a Lidl gateway at 460800 baud. On channel 11, Zigbee reached network-up
state through EmberZNet 8.2.2 / EZSP 18 while Thread reached and retained the
leader role; repeated Zigbee health checks passed with no container restart.
One first-start Zigbee software reset recovered automatically and did not recur
on the clean simultaneous restart.

This path remains **experimental**. Series 1 requires Zigbee and Thread to use
the same channel; independent-channel Concurrent Listening still requires a
supported Series 2 radio. No real Thread end device or long mixed-traffic soak
was included in the release gate. Contributed by @mbjd05 in PR #161.

---

## [4.5.0] - 2026-09-14

### Docker stacks — image pins refreshed, Home Assistant pinned

The five compose files under `24-`, `25-` and `26-` move to the current upstream
releases: Zigbee2MQTT `2.7.2` → `2.14.1` (`docker-compose.yml`,
`docker-compose-zigbee.yml`, `docker-compose-zoh.yml`) and matter.js server
`1.3.3` → `1.4.0` (`docker-compose-otbr-host.yml`, `docker-compose-otbr-gateway.yml`;
`1.4.0` is the digest `stable` resolves to on 2026-09-14, recorded next to the image
line). Home Assistant, which followed `:stable`, is now pinned like every other
artefact in this tree, to `2026.9.2` — the digest `stable` resolved to on the same
day, recorded in the same way. Nothing on the gateway changes; the pins are the
host-side stacks only. None of the three was re-tested against a gateway in this
change.

---

## [4.3.0] - 2026-09-03

### Documentation — what the first G4 bootloader install actually looks like, and what its version is not (discussion #148, @hlyi)

@hlyi ran the published instructions end to end on a G4 he had first restored to its
original radio firmware, so the procedure is now field-validated over a **factory**
bootloader, with the commands exactly as written, rather than reconstructed from the run
that produced it. Two things came out of his transcript.

`sz` says almost nothing: a connection line, `Give your local XMODEM receive command now.`,
then silence for the whole transfer and no completion message. The prompt is addressed to a
human driving a terminal, while the bootloader on the other end is already receiving — so
the quiet is the normal course, and the installation guide and the component README now say
so instead of leaving the reader to guess whether it hung. The confirmation arrives one step
later, from the application flash reporting the bootloader version it finds.

And a correction of ours: **the factory bootloader's version is unknown, not 2.4.2.** It
drops straight into XMODEM without announcing one. The 2.4.2 seen in the July logs was our
own earlier build, which @hlyi had installed by hand — the two are easy to conflate, and the
version table's "already in the field" invited exactly that. The only defensible statement
about the factory version is negative and inferred: it compares lower than whatever
installed over it, because the upgrade gate let that image through. The README says that now,
and says not to write a number for it.

`docs/troubleshooting.md` also names the one line of a *successful* application flash that
reads like a failure — `Failed to read firmware metadata: KeyError(... GBLTagId.METADATA ...)`,
an optional tag our images do not carry — and records why it is not filtered out: doing so
means routing application flashes through a line-buffered filter, which is what once
swallowed the upload progress bar.

---

## [4.2.0] - 2026-08-22

_No radio firmware or tooling change. This release updates the RTL8196E kernel and retains
the v4.1.0 EFR32 artifacts unchanged._

---

## [4.1.0] - 2026-08-21

_Documentation only on the radio side: the one-time Sengled G4 bootloader step moves from the
component README into the installation guide, where a first-time G4 owner actually passes._

### Documentation — the Sengled G4 bootloader step reaches the installation guide (discussion #148, @hlyi)

v4.0.0 documented the one-time procedure that installs this project's Gecko bootloader over
Sengled's factory one, which has no menu for `universal-silabs-flasher` to drive. It was
documented in `23-Bootloader-UART-Xmodem/README.md`, where someone already looking for the
Gecko bootloader finds it — and a first-time G4 owner following the installation guide never
opens that door. @hlyi asked for it where it belongs, as a prerequisite in the guide's radio
step, and that is where it now is.

`docs/getting-started.md` step 12 opens with a Sengled-only subsection: why the factory
bootloader cannot be driven, the two bridge settings and why both are load-bearing, the `sz`
transfer, the fact that installing a bootloader erases the application so the two flashes are
one sequence, and the warning against repeating the step — a same-version image is declined in
silence, after the erase, and a raw XMODEM transfer has none of the guards `flash_efr32.sh`
grew for exactly that. The component README keeps the reasoning and the memory map behind it,
and is linked rather than duplicated. `docs/radio-options.md` states the prerequisite before
its flash commands, since a reader can arrive there first, and `docs/troubleshooting.md` names
the case under "EFR32 flash fails" for whoever meets it before reading anything.

---

## [4.0.0] - 2026-08-10

_The cycle that made the EFR32 side genuinely multi-board: every firmware now
ships **prebuilt for the Sengled G4** at the bauds that board runs, `make-all-bauds.sh`
builds any board's matrix, and radio-firmware filenames carry their
**flow-control type** (#145). The bootloader work from #148 is the sharp edge:
hardware entry through a **GPIO the board wires**, and the discovery that a
same-version stage-2 upgrade is declined **in silence, after the application has
already been erased** — now refused up front by `flash_efr32.sh`._

### Sengled G4 — the full prebuilt set, at the bauds that board actually runs (discussions #134, #143)

A G4 user could flash the NCP (#130) and, since #148, the bootloader without a
toolchain; RCP, OT-RCP and the Z3 Router were build-it-yourself, on the policy
that no prebuilt ships until the image has run on the hardware. That policy left
the three firmwares a G4 user is most likely to want behind a Silabs SDK install,
so it is relaxed here: all three now ship prebuilt, and the evidence behind each
image is documented per firmware in `boards/README.md` instead of being implied
by its presence. This supersedes the "No G4 prebuilt is committed" notes in the
4.0.0-rc5 entries below.

```text
25-RCP-UART-HW/firmware/rcp-uart-802154-230400-none-sengled-e39-g8c.gbl
26-OT-RCP/firmware/ot-rcp-230400-sw-iostream-sengled-e39-g8c.gbl
27-Router/firmware/z3-router-7.5.1-115200-sw-sengled-e39-g8c.gbl
```

**230400, not the project default.** The 460800 default is the Lidl reference's,
and it holds because that board wires RTS/CTS. The G4 does not: at 460800 the
host's 16-byte RX FIFO must be drained within ~347 µs, a stage no software flow
control protects, which is where @hlyi's measurements found the losses (#134,
#142). At 230400 the budget doubles and the link is clean, so that is the
operating point — and shipping a 460800 G4 prebuilt would have made the flasher's
default resolve to the one variant known not to hold there. The Router keeps
115200, its only baud; the NCP keeps 115200, already below the ceiling.

`flash_efr32.sh` now takes the default baud from the board rather than assuming
the reference. Two optional `board.env` keys — `BOARD_RCP_DEFAULT_BAUD` and
`BOARD_OT_RCP_DEFAULT_BAUD` — override the project defaults for the board that
declares them (the G4 declares 230400 for both); a board that declares neither is
resolved exactly as before, so the Lidl path is unchanged. A malformed value is
refused at board-selection time, next to the existing `BOARD_UART_FLOW` check,
rather than surfacing later as a missing-GBL error. Verified by resolving all four
firmwares for both boards: one match each, and the lidl matches are the same files
as before.

Validation is not uniform across the set, and `boards/README.md` now says so per
firmware: the NCP was validated end-to-end on a real G4 (#130) and the bootloader
on @hlyi's unit (#148), while **the RCP and Router images have never been run on a
G4** — they are builds of the board facts in `board.env`, and the OT-RCP image is
our build of the sources @hlyi measured at 230400, not the binary he ran.

### `make-all-bauds.sh` — builds the selected board's matrix, not just the reference one

The script that rebuilds every committed prebuilt was the last one still hard-wired
to `lidl`: it hard-coded the flow-control and OT-RCP driver fields of each filename
as literals, so a board with software flow would have had its output looked for
under the wrong name. It now takes the same `BOARD=` selector as `build_efr32.sh`
(default `lidl`, exported to the per-firmware scripts) and derives those fields
from `board.env` exactly as the build scripts do — `<flow>`, RCP's clamp of `sw` to
`none`, OT-RCP's `sw`→`iostream` backend, and the `-<board>` suffix.

Which bauds constitute a board's matrix is now a board fact too: `BOARD_NCP_BAUDS`,
`BOARD_RCP_BAUDS`, `BOARD_OT_RCP_BAUDS` and `BOARD_ROUTER_BAUDS` in `board.env`
override the reference row key by key, so a board declares only what differs. The
G4 declares one baud per firmware, which is exactly the four images committed
above; `lidl` declares none and keeps its 10-GBL matrix unchanged. Verified with
`--list` on both boards: the reference matrix is identical to before, and all four
G4 entries resolve to committed files. `flash_efr32.sh`'s no-GBL error now suggests
`BOARD=<board> ./make-all-bauds.sh` instead of suppressing the hint for non-lidl
boards.

### `flash_efr32.sh` — the bootloader pre-flight no longer waves through a `.gbl` it cannot read (discussion #148)

The pre-flight guard added earlier this cycle refuses a bootloader flash whose
image is not strictly newer than the running one, because the chip declines such
an image in silence *after* staging it inside application space: the application
is gone and nothing changed. But the guard compared the two versions only when it
had both of them, and fell through in silence when it did not.

That silence was a hole in the guard itself. A file whose version cannot be read
reached the flasher unchallenged — an application `.gbl` passed with the
bootloader choice (`--firmware-file <app.gbl> bootloader`) carries no bootloader
tag (`0xF50909F5`) and no version word, so it parsed to nothing, skipped the
comparison entirely, and was flashed as a bootloader. That is precisely the
erase-the-app-for-nothing trade this guard exists to refuse, handed back through
the guard.

The two figures fail in opposite directions, so they are now handled apart. An
unreadable **image** version is disqualifying and refuses (`--force` still
overrides): a bootloader flash is destructive before it is validated, so a file
we cannot reason about is not one to write to the chip. An unknown **running**
version is merely unverifiable — it is the normal state of a chip whose
bootloader was installed by hand, which records none — so it warns, names what it
could not establish, and proceeds.

### Bootloader — hardware entry via GPIO activation, on boards that wire the pin (discussion #148, @hlyi)

The Gecko bootloader is entered in exactly one way in our builds: the running
application performs a system-request reset carrying a bootloader reset reason
(`enterBootloader()`, `btl_main.c`). That is what `universal-silabs-flasher`
triggers over EZSP/CPC/Spinel. An `nRST` pin pulse is not such a reset, so when
the application is wedged — or speaks none of those protocols, as the Z3 Router
does — there is nothing left to ask.

The Silabs `bootloader_gpio_activation` component adds the missing door: the
bootloader samples a GPIO at startup and stays in its menu if the pin is held
active, whatever the application is doing. It was never in our project, which
was right for the Lidl reference board — no EFR32 pin is routed to a SoC GPIO
there, and that is a deliberate fact of the hardware, not an oversight (the OEM
engineered no recovery wire either: see `POST-MORTEM-bootloader-recovery.md`).

It is wrong for the Sengled G4, which *does* wire such a line — the one the
kernel bridge already drives as `blmode_pulse` (#123). @hlyi found this the hard
way: flashing our bootloader over Sengled's silently killed his hardware
bootloader-entry path, because Sengled's bootloader samples the pin and ours did
not. `build_bootloader.sh` now adds the component, and points it at the board's
pin (active LOW, matching the host's open-drain active-low line), for any board
whose `board.env` declares `BOARD_BTL_ACTIVATION_PIN="<port-letter> <pin>"`. The
G4 declares **PB15**, traced on the PCB by @hlyi and checked against the part
(`_GPIO_PORT_B_PIN_MASK = 0xF800` for SDID 89 — pins 11-15 of port B exist).

A board that declares no such pin — the Lidl — builds exactly as before: the
component stays out of the project, and both tracked artefacts (`.gbl`,
`-combined.s37`) rebuild byte-for-byte identical (`1db5aca6…`, `3959733b…`,
verified).

**Validated on hardware by @hlyi**: with the component in, `echo 1 >
blmode_pulse` drops a running G4 into the Gecko Bootloader (`Gecko Bootloader
v2.04.03` at the prompt), whatever the application is doing. The G4 bootloader
prebuilt is committed on the strength of that test — it is the exact image he
ran (`772ed9b7…`). `flash_efr32.sh` now uses the pin automatically on boards that
have it: it enters the bootloader directly, skipping the running-app probe and,
for the Z3 Router, the five-baud fallback sweep that used to be the only way in
(the fast path promised in #123).

### Bootloader — a same-version stage-2 upgrade was declined in silence, *after* erasing the application (discussion #148, @hlyi)

Found while shipping the fix above, and it is the more consequential bug of the
two. The Gecko bootloader installs a stage-2 image only when its version is
strictly greater than the running one, and says nothing when it declines —
`btl_comm_xmodem_common.c`:

```c
if (imageProps->contents & BTL_IMAGE_CONTENT_BOOTLOADER) {
    if (imageProps->bootloaderVersion > bootload_getBootloaderVersion()) {
        bootload_commitBootloaderUpgrade(BTL_UPGRADE_LOCATION, ...);
    }
}   // no else branch
```

The check also runs *after* the damage. The incoming image is staged at
`BTL_UPGRADE_LOCATION` — a fixed `0x8000` on Series 1, which lives **inside
application space** — and `bootload_bootloaderCallback()` erases the first
application page as the first bytes arrive. So a same-version reflash reports
success, destroys the radio firmware, and leaves the old bootloader in place.
That is exactly what happened to @hlyi: our G4 bootloader and his were both
2.4.2, so the GPIO-activation build he flashed was thrown away, silently, and
his `blmode` pin stayed inert. The Lidl has the same flash layout and the same
trap; nobody had ever hit it because the only path anyone exercises is a *first*
install over a factory bootloader of a lower version, where the check passes.

The version word is `major<<24 | minor<<16 | customer`. `2.4` is Silicon Labs'
own; the low 16 bits are the customer field, an SDK config option left to the
integrator — so it is ours, and it is now owned per board via
`BOARD_BTL_CUSTOMER` in `board.env`. **Bump it whenever that board's bootloader
binary changes.** The Lidl stays at **2.4.2** (its binary did not change, and it
still rebuilds byte-identical); the G4 ships **2.4.3**, which is what makes the
GPIO-activation build installable over UART on a unit already running 2.4.2 —
confirmed by @hlyi.

`flash_efr32.sh` no longer takes a successful upload as proof of anything:

- **Before flashing**, it compares the version *inside* the `.gbl` (parsed from
  the bootloader tag, with `od`) against the running one, and refuses outright if
  the image cannot install — rather than letting the chip eat the application for
  nothing. `--force` overrides. Suggested by @hlyi.
- **After flashing**, it reads the version back *off the chip* and compares it
  with what it sent, failing loudly on a mismatch instead of printing "Bootloader
  flashed successfully".
- The `NoFirmwareError` traceback USF spills after every bootloader flash is
  collapsed to one line — it is expected (the app slot really is empty) but it is
  the signature of the app being erased, never evidence that the bootloader
  changed, and mistaking one for the other is what hid this bug. Only that
  traceback, only on the bootloader path: `FailedToEnterBootloaderError` and
  everything else still print in full.

### Documentation — the first radio install on a factory Sengled G4 (discussion #148, @hlyi)

This is what #148 originally asked for, and the piece still missing after the two
bootloader fixes above. `flash_efr32.sh` drives `universal-silabs-flasher`, which
speaks the Gecko bootloader's **menu**; Sengled's factory bootloader has none —
once entered it goes straight into an XMODEM receive and emits `C` once a second
— so the script cannot perform the *first* bootloader install on a stock G4.
`23-Bootloader-UART-Xmodem/README.md` now documents the one-time manual path
@hlyi validated on his own unit: park the bridge at 115200 with flow control off,
`blmode_pulse` into the factory bootloader (it samples the pin — which is why his
pin worked before our bootloader, which did not sample it, replaced Sengled's),
and send the `.gbl` with `sz -X`. Ours has a menu, so every flash after that one
is `flash_efr32.sh` again, including the bootloader's own updates.

### New (experimental): `build_rcp_blehci.sh` — multiprotocol RCP with a Bluetooth HCI endpoint (discussion #146)

@hlyi asked whether the Sengled G4 could act as a Home Assistant Bluetooth proxy
while still serving Zigbee. It can, in principle: the Gecko SDK ships
`rcp-uart-802154-blehci` — our 802.15.4 RCP plus FreeRTOS, the Bluetooth
controller and `bluetooth_hci_cpc` — which runs both stacks concurrently
(dynamic multiprotocol) and exposes Bluetooth to the host as **HCI over CPC**
(endpoint 14, `SL_CPC_ENDPOINT_BLUETOOTH_RCP`). Since `cpcd` runs on the machine
hosting Z2M/HA rather than on the gateway, Silabs' `cpc-hci-bridge` +
`hciattach` turn that endpoint into a BlueZ controller *there*, which is the only
shape Home Assistant accepts besides an ESPHome/Shelly proxy. The gateway itself
needs no Bluetooth stack (and has none).

`build_rcp_blehci.sh` builds it with the usual `BOARD=` contract, the same VCOM
routing and CPC flow-control clamp as `build_rcp.sh`, and the #145 filename
scheme. Two measured facts drive its guards:

- On the Lidl's **EFR32MG1B** (256 kB / 31 kB) the link **fails** — `region FLASH
  overflowed by 10036 bytes`, `.heap will not fit in region RAM`. The script
  refuses that part outright. This is the 512 kB DMP floor from discussion #108,
  now measured rather than cited.
- On the G4's **EFR32MG13P732F512IM32** it fits: ~231 kB flash of 512 kB and
  ~44.6 kB RAM of 64 kB. Bluetooth + FreeRTOS costs about 105 kB of flash and
  20 kB of RAM over the plain RCP.

**Experimental, never run on hardware, no prebuilt committed.** Bluetooth
scanning also competes with 802.15.4 for the single radio, and on a board without
RTS/CTS the (unflow-controlled) CPC link would now carry an advertisement stream
as well — the exact traffic that overruns the host's 16-byte RX FIFO.

### Radio-firmware filenames now encode the flow-control type (discussion #145)

Requested by @hlyi (#145): the pre-built `.gbl`/`.s37` names embedded the baud
but not the UART flow-control type, so two board configurations that differ only
in flow control (e.g. the Lidl `hw` build vs a `sw` port) were indistinguishable
by filename. The scheme now carries the **as-built flow** after the baud, and
OT-RCP — the only firmware with a real UART-driver choice — also carries the
driver:

```
ncp-uart-hw-<ver>-<baud>-<flow>[-<board>].gbl
rcp-uart-802154-<baud>-<flow>[-<board>].gbl
ot-rcp-<baud>-<flow>-<driver>[-<board>].gbl      # driver = uartdrv | iostream
z3-router-<ver>-<baud>-<flow>[-<board>].gbl
```

- `<flow>` is `hw | sw | none`, the value actually built. CPC has no software
  flow control, so a `sw` board's **RCP** is built and named `none` (matching
  what `flash_efr32.sh` records as `FIRMWARE_FLOW_CTRL`).
- The OT-RCP `<driver>` is always spelled out (auto: `sw`→`iostream`,
  `hw|none`→`uartdrv`). A forced `UART_DRIVER=` build now differs by its driver
  field (`…-hw-iostream` vs the default `…-hw-uartdrv`) instead of the old bare
  `-iostream` marker.
- The **bootloader** is unchanged (no baud/flow in its name).
- `flash_efr32.sh` resolves the new names by anchoring `<flow>`/`<driver>`
  exactly from the board's `BOARD_UART_FLOW`, so resolution stays a single
  deterministic match and a forced-driver build never shadows the default. The
  filename version-parser was also made independent of field order (it had been
  silently returning an empty version for board-suffixed builds).
- All committed baud-bearing prebuilts were renamed accordingly (contents
  unchanged). Legacy no-baud artefacts (`ot-rcp.gbl`, `ncp-uart-hw-7.5.1.gbl`,
  …) are left as-is.

### `flash_efr32.sh` — allow OT-RCP flashes at 230400 baud (discussion #134)

The OT-RCP allowed-baud set was pinned to the single max-tested value (460800,
the otbr-agent ceiling per CHANGELOG v3.0.0). @hlyi's G4 measurements (#134,
#142) established 230400 as the operating point for boards without RTS/CTS
wiring: at 460800 the host's 16-byte RX FIFO overruns (~347 µs of tolerated
IRQ latency, a stage no software flow control can protect), while at 230400
the budget doubles and the link is clean. The set is now `230400 460800`
(default unchanged at 460800).

No new prebuilts: the committed lidl matrix still carries 460800 only (the
Lidl board wires RTS/CTS, which makes 460800 reliable there). A 230400 GBL is
built with `build_ot_rcp.sh 230400` — the path `flash_efr32.sh`'s no-GBL
error message already points at.

### `build_ot_rcp.sh` — the generated `.slcp` now reports the baud it was built at (discussion #134)

Spotted by @hlyi: after `./build_ot_rcp.sh 230400`, the project file in the build
directory still read `SL_IOSTREAM_USART_VCOM_BAUDRATE: 460800`. The firmware was
correct — the compiled baud comes from the config header the build overwrites in
step [3/4], which is the only baud the compiler sees — but the project file lied
about what it had produced (the iostream backend swap renamed the config key
without rewriting its value, and the uartdrv path never touched it either). The
requested baud is now substituted into the `.slcp` as well, for both backends.
Cosmetic today; a real trap the day that header overwrite regresses.

### Docker stacks — Matter Server switched to `matterjs-server` (matter.js)

The Matter server was rewritten on matter.js and moved to a new image. Home
Assistant now ships that code as its "Matter Server" add-on, while the image both
of our Thread/Matter stacks pulled — `ghcr.io/matter-js/python-matter-server` —
is frozen at 8.1.2 and no longer updated. `docker-compose-otbr-gateway.yml` (use
case 3) and `docker-compose-otbr-host.yml` (use case 2) now pull
`ghcr.io/matter-js/matterjs-server:1.3.3`. It is a drop-in replacement: same
WebSocket API on `:5580/ws`, plus a web dashboard on `:5580/`. Nothing on the
gateway is affected — the Matter server reaches OTBR through Home Assistant's
Thread integration (REST `:8081`) and mDNS, never directly, so the REST contract,
the Thread dataset and `/userdata/thread` are untouched.

Pinned to the release tag rather than `:stable`, for the same reason every other
artefact in this tree is pinned; `1.3.3` and `stable` resolve to the same digest
today, recorded in a comment next to the image line.

The service also drops `privileged: true`, the `apparmor=unconfined` exception and
the `/sys/fs/cgroup` mount. Those were the price of the Python image running as
root; the new one runs as **uid 1000** and creates `/data` already owned by
`1000:1000`, so a fresh **named** volume inherits the right ownership from the
image and needs no `chown`. That only holds for named volumes — a bind-mount would
still need `chown -R 1000:1000`, which the file says inline, because the upstream
Docker guide documents the bind-mount case and it is the one way to get a
container that starts and then cannot write its own fabric.

Both files mount a **new** volume (`matterjs_data`) and leave the legacy
`matter_data` declared but unmounted. The storage format is upgraded on first
start and the migration is one way, so the old volume is the rollback: put the
image and volume names back and the previous fabric is still there. The cost is
that the default path starts with an empty fabric and devices must be
re-commissioned; `docker-compose-otbr-gateway.yml` carries the volume-copy recipe
for carrying the existing fabric over instead, including the compose project-name
prefix the volumes actually have on disk.

BLE commissioning from the container is deliberately left off. It is not how these
stacks commission — the HA Companion app uses the phone's Bluetooth — so the
server needs no radio, and enabling it is documented as a comment
(`NOBLE_BINDINGS=dbus`, `BLUETOOTH_ADAPTER=0`, `/run/dbus:/run/dbus:ro`) rather
than carried as an unused mount.

Both files also set `LISTEN_ADDRESS=127.0.0.1`, which the previous stack had no
equivalent of. `:5580` is the fabric's control plane — list, commission,
decommission, command — and it takes no credentials; the dashboard on `:5580/`
drives the same API. Under `network_mode: host` the server binds every interface,
so that control plane was reachable from any machine on the LAN (verified on the
bench: the socket listened on `*:5580` and both the dashboard and an
unauthenticated WebSocket handshake answered on the host's LAN address). Home
Assistant sits in the same compose file on the same host and reaches the server
over loopback, so the restriction costs nothing in the documented topology, and
the failure mode if someone departs from it is loud and immediate — the Matter
integration cannot connect — rather than a silent open port. The trade is that the
dashboard opens only from the Docker host; `docker/README.md` documents both that
and the `ssh -L` way around it. Matter/Thread traffic is unaffected: devices are
reached over IPv6/mDNS and OTBR, never through this port. Note this was not a
regression of the image swap — `python-matter-server` bound just as widely; the
new server is simply the one that warns about it at startup.

Worth stating because it diverges from upstream: the
[migration FAQ](https://github.com/home-assistant/addons/blob/master/matter_server/MIGRATION_FAQ.md)
has you migrate the existing data directory in place and is explicit that there is
then no way back. Keeping the old volume trades the fabric for a rollback that does
not depend on having taken a backup first — the right default for a stack a reader
is following from a repo, where the fabric is usually small and the appetite for an
unrecoverable one-way step is low. `docker/README.md` documents both trades and
carries the background links (HA's
[announcement](https://www.home-assistant.io/blog/2026/06/23/the-matter-upgrade-youve-been-waiting-for/),
the FAQ, and matterjs-server's
[`docs/docker.md`](https://github.com/matter-js/matterjs-server/blob/main/docs/docker.md));
`THREAD-MATTER-PRIMER.md` was updated to name the current implementation.

## [4.0.0-rc5] - 2026-07-08

### `flash_efr32.sh` — record `FIRMWARE_FLOW_CTRL` in radio.conf at flash time (discussion #141)

Suggested by @hlyi (#141). `radio.conf` is the record of the chip-side truth written
at flash time (`FIRMWARE=`, `FIRMWARE_VERSION=`, `FIRMWARE_BAUD=`, `MODE=`), yet the
flow-control mode — the one chip-side fact the build system already knows via
`boards/<board>/board.env` — was left for the user to add by hand. On a G4 running
OT-RCP that manual step was mandatory: with the key absent, `S70otbr` passes
`uart-flow-control=true` and otbr-agent asserts RTS/CTS on a board that doesn't wire it.

- Every **application flash** (NCP / RCP / OT-RCP / Router) now writes
  `FIRMWARE_FLOW_CTRL=<hw|sw|none>` into `/userdata/etc/radio.conf`, using the same
  strip-then-append pattern as the other keys the script owns. The value is
  `BOARD_UART_FLOW` from the selected board's `board.env` (`hw` for `lidl`, `sw` for
  `sengled-e39-g8c`), validated at board-selection time (fail-fast on a malformed
  third-party `board.env`, before anything touches the gateway).
- Bootloader-only flashes are unchanged — they still touch only `BOOTLOADER_VERSION=`.
- Behaviourally a no-op for Lidl users: `hw` is what the devicetree default and
  `S70otbr`'s absent-key fallback already selected. On the G4, flashing any radio
  firmware now leaves the host side matched to the chip with nothing to edit.
- A manually-set `FIRMWARE_FLOW_CTRL` is now reset to the board default at each app
  flash — re-apply it afterwards if you flash a `--firmware-file` image whose flow
  mode differs from the board's (rare, power-user case).
- Consumers unchanged: `S50uart_bridge` (bridge `flow_control` knob) and `S70otbr`
  (spinel `uart-flow-control`) already read the key.

### `27-Router` — `BOARD=` support (discussion #143)

Requested by @hlyi (#143): a Z3 Router build with software flow control for the
Sengled G4. `build_router.sh` now follows the exact `build_ncp.sh` mechanism —
it sources `boards/<board>/board.env`, re-points the `.slcp` device component at
`BOARD_TARGET_DEVICE`, applies the board's UART routing and flow-control type to
the VCOM header via `lib_uart_config.sh`, and suffixes non-lidl artefacts with
`-<board>` (`.gbl` and `.s37`). For the G4 that means an MG13 target with XON/XOFF
flow, `z3-router-<ver>-115200-sengled-e39-g8c.gbl`.

- `flash_efr32.sh` lifts its router refusal for non-lidl boards: `router` joins
  `ncp`/`otrcp` in the board-parameterised set (suffixed-GBL resolution + the
  existing devicetree board-match guard). RCP and the Gecko bootloader remain
  lidl-only.
- `build_efr32.sh` no longer skips the router for a non-lidl `BOARD=`.
- The lidl router build is byte-for-byte unchanged (the board mechanism resolves
  to the reference values), and the committed lidl prebuilts keep their names.
- No G4 prebuilt is committed: the G4 router build has not been hardware-validated
  yet (same policy as OT-RCP) — build it yourself and validate before trusting it.

### `23-Bootloader-UART-Xmodem` — `BOARD=` support + pinned GCC first stage (discussion #143)

The Gecko bootloader build joins the `BOARD=` mechanism, leaving RCP as the only
lidl-only build. `build_bootloader.sh` re-points the `.slcp` device component at
`BOARD_TARGET_DEVICE` and applies the board's UART routing to `btl_uart_driver_cfg.h`
via a new routing-only helper (`apply_uart_routing` in `boards/lib_uart_config.sh` —
this header has no CTS/RTS or flow-control-type defines; the bootloader's numeric
flow-control knob stays 0 for every board, since the Xmodem path always runs
flow-off). Non-lidl artefacts carry the `-<board>` suffix (`.gbl`, `.s37`,
`-combined.s37`), and the artefact cleanup is now scoped so boards don't wipe each
other's files.

- `flash_efr32.sh` lifts the bootloader refusal for non-lidl boards: lidl keeps its
  pinned exact GBL path (unchanged), a non-lidl board resolves its suffixed GBL.
- **Reproducibility fix:** `slc generate` was invoked without `--toolchain`; the
  resolution is ambiguous and could embed the **IAR-built** first-stage binary in
  `-combined.s37` instead of the GCC one (both official Silabs prebuilts for the
  chip; the `.gbl` — stage 2 only — was never affected). The build now pins
  `--toolchain gcc`; with it, the lidl rebuild is byte-identical to **both** committed
  artefacts.
- G4 build verified structurally: correct GCC `first_stage_btl_efx32xg13` first stage
  and placement in the MG13's dedicated bootloader flash region at `0x0FE10000`
  (application at `0x0` — different layout from the MG1B's bootloader-in-main-flash).
  **Never run on real G4 hardware, and a bad bootloader flash is an SWD-only
  recovery** — no prebuilt committed; a G4 flashed via `flash_efr32.sh` already has a
  working bootloader, so only replace it with a debugger attached.

### `25-RCP-UART-HW` — `BOARD=` support (discussion #143)

The multi-PAN RCP build joins the `BOARD=` mechanism — **every EFR32 firmware is now
board-parameterised**, and `flash_efr32.sh` no longer refuses anything by board (the
`-<board>`-suffixed GBL resolution + devicetree board-match guard apply uniformly).

- Same pattern as the others: `build_rcp.sh` sources `boards/<board>/board.env`,
  targets `BOARD_TARGET_DEVICE` (this slcp pins no device component — `--with` does
  the job), applies the board's UART routing to the CPC VCOM header via
  `apply_uart_config`, and suffixes non-lidl artefacts with `-<board>`.
- **Flow-control clamp:** CPC supports only RTS/CTS or none — the driver is a binary
  `WITH_HWFC`/`WITHOUT_HWFC` and the framing has no XON/XOFF escaping, so software
  flow control does not exist on this path. A `BOARD_UART_FLOW=sw` board (the G4) is
  built with flow control **none**, and `flash_efr32.sh` records
  `FIRMWARE_FLOW_CTRL=none` (not the board's `sw`) for an RCP flash on such a board:
  the chip's flow partner is the gateway's **in-kernel UART bridge** — cpcd connects
  to it over TCP (`bus_type: TCP`), so cpcd's `uart_hardflow` never applies — and a
  bridge armed `sw` against unescaped binary CPC frames would consume in-frame
  `0x11`/`0x13` bytes as flow control.
- Like OT-RCP, the RCP build is OpenThread-based and not byte-reproducible (embedded
  build-id). Lidl innocence was proven the same way: the generated CPC VCOM header
  and the slcp are byte-identical to their `patches/` references; the committed lidl
  prebuilts are untouched.
- G4 build compiles end-to-end (MG13 target, flow none in slcp + header,
  `rcp-uart-802154-460800-sengled-e39-g8c.gbl`/`.s37`). Not hardware-validated — no
  prebuilt committed.
- Cleanup: the `cpcd-zigbeed` container's `UART_BAUDRATE` env var is gone — nothing
  ever consumed it on the native TCP bus (the gateway's in-kernel bridge owns baud
  and flow control, armed from `radio.conf`). An existing compose file that still
  sets it keeps working; the variable was always ignored.

### `26-OT-RCP` — per-board UART backend: `uartdrv` (hw/none) or `iostream` (sw) (discussion #142)

The Gecko SDK's OpenThread platform abstraction has two UART backends, selected purely
by project component: `uartdrv_usart` (DMA-first; Silabs' own docs call its XON/XOFF
support *"partially supported"* / *"partial only"* — reaction quantised by DMA-chunk
cadence, the #134 analysis) and `iostream_usart` (per-byte IRQ; the NCP's driver, whose
software flow control is **complete**: watermark-driven XOFF/XON emission plus inbound
honor in `sl_iostream_uart.c`). `build_ot_rcp.sh` now picks the backend from the
board's flow mode:

- `BOARD_UART_FLOW=hw` or `none` → `uartdrv_usart`, the historical default — the lidl
  build path is untouched (slcp byte-identical, same artefact names).
- `BOARD_UART_FLOW=sw` (the Sengled G4) → `iostream_usart` with
  `uartFlowControlSoftware`: the only backend that actually flow-controls a board
  without RTS/CTS. **This changes what `BOARD=sengled-e39-g8c ./build_ot_rcp.sh`
  produces.**
- `UART_DRIVER=uartdrv|iostream` forces a backend for experiments; a forced
  non-default choice gets a `-<driver>` filename suffix, so it can never shadow the
  canonical artefact and `flash_efr32.sh`'s exact globs never resolve it
  (`--firmware-file` flashes it explicitly).

The historical objection that iostream corrupts the binary Spinel stream (LF→CRLF)
was a configuration default, not a driver property: the new
`patches/sl_iostream_usart_vcom_config.h` (same reference header as the NCP) disables
the conversion. Validation status: the iostream backend ran clean on the Lidl dev
gateway (hw flow, 460800 — spinel intact, network formed, zero agent restarts under
back-to-back spinel load); the sw-flow variant on a real G4 is with @hlyi for
hardware validation (experimental package attached in #142). Pairs with the host-side
`IXON` work under discussion there.

---

## [4.0.0-rc3] - 2026-06-25

_EFR32 deliverables shipped in `v4.0.0-rc3`: the Sengled G4 NCP firmware is validated on
real hardware and committed as a prebuilt, and `flash_efr32.sh` gains a `BOARD=` selector
with a hardware-match guard. The multi-board build groundwork (the `BOARD=` build
mechanism, with the G4 routing still a placeholder) remains under `[4.0.0-pre]` below._

### G4 (Sengled Smart Hub E39-G8C) — NCP validated on hardware, prebuilt committed

@hlyi flashed the `BOARD=sengled-e39-g8c` NCP build to a real G4 and confirmed it
end-to-end — Home Assistant talks to the radio (#130). Two follow-ups from that:

- `boards/sengled-e39-g8c/board.env` is now **validated**, not a placeholder. The
  G4 wires the EFR32 UART on the **same USART/pins as Lidl** (USART0, PA0 = TX,
  PA1 = RX) — confirmed on hardware — so the only board-specific facts are the
  MG13P OPN (#133) and software flow control (#123).
- A **prebuilt G4 NCP `.gbl`** is committed
  (`24-NCP-UART-HW/firmware/ncp-uart-hw-7.5.1-115200-sengled-e39-g8c.gbl`, baud
  115200, reproducible), so a G4 user can flash without building. OT-RCP builds
  for the G4 but its Thread path is not functionally validated there yet — it
  stays build-it-yourself. @hlyi's cosmetic note (the `-hw-` in the filename and
  the `24-NCP-UART-HW` dir should read `sw` for this board) is deferred as
  non-critical.

### `BOARD=` support for `flash_efr32.sh` (flash half of the multi-board work)

`flash_efr32.sh` now takes the same `BOARD=` selector as the builds (env var or
`--board`, default `lidl`). A Lidl user sets nothing and the flash path is
unchanged; a non-lidl board flashes its `-<board>`-suffixed NCP/OT-RCP firmware
from the same flat `firmware/` directory.

- **Per-board resolution.** For a non-lidl `BOARD=`, the firmware glob gains the
  `-<board>` suffix (`ncp-uart-hw-*-<baud>-<board>.gbl`, `ot-rcp-<baud>-<board>.gbl`).
  The lidl globs don't match the suffixed files and vice-versa, so lidl
  resolution is byte-for-byte unchanged. rcp/router/bootloader for a non-lidl
  board are refused with a clear lidl-only message (`--firmware-file` still
  bypasses for power users).
- **Authoritative board-match guardrail.** Because the script always runs against
  a live gateway, it reads `/proc/device-tree/model` (folded into its SSH detect
  block) and refuses to flash before pushing firmware when the selected board
  disagrees with the hardware (`lidl`→"Lidl", `sengled-e39-g8c`→"Sengled"). Since
  the effective board defaults to `lidl`, this also catches the common slip of
  forgetting `BOARD=` on a G4 box. `--force` overrides; an unreadable model skips
  the check.
- Per-board `firmware/<board>/` subdirectories and `BOARD=` for the other three
  firmwares are deferred until there's demand (the flat `firmware/` + `-<board>`
  suffix suffices today).

---

## [4.0.0-pre] - 2026-06-13

### `BOARD=` support for the firmware builds (radio half of the multi-board work)

The NCP and OT-RCP builds are now parameterised by board, mirroring the
`BOARD=` mechanism on the RTL8196E bootloader side. A board contributes a
single `boards/<board>/board.env` file packaging the MCU OPN and the UART
routing to the host; the build scripts read it (default `BOARD=lidl`) and a
shared helper (`boards/lib_uart_config.sh`) applies the routing into the
generated VCOM config header, substituting only the value token on each
`#define` so the reference build stays byte-identical.

- `boards/lidl/board.env` reproduces the historical hard-coded values, so
  `BOARD=lidl` (the default) is unchanged — verified: the rebuilt NCP `.gbl`
  is byte-for-byte identical to the committed firmware, and the generated VCOM
  headers (iostream for NCP, uartdrv for OT-RCP) are byte-identical to their
  `patches/` references.
- Flow control is `hw` / `sw` / `none`. Software XON/XOFF is a first-class SDK
  option (`uartFlowControlSoftware` for NCP, `uartdrvFlowControlSw` for OT-RCP)
  selected through the same `_FLOW_CONTROL_TYPE` token — no patch required.
- `boards/sengled-e39-g8c/board.env` builds NCP and OT-RCP end-to-end for the
  Sengled G4 (MG13 target, software flow). The MG13P OPN
  (`EFR32MG13P732F512IM32`, from the hardware page in #133) and
  `BOARD_UART_FLOW=sw` (the G4 has no RTS/CTS, #123) are set. The NCP `.slcp`
  pinned the lidl MCU as a device component, so the build now re-points it at
  `BOARD_TARGET_DEVICE` before `slc generate` (else two device families link →
  duplicate symbols); lidl is unaffected. The **USART/pin routing** is still a
  Lidl placeholder pending on-hardware validation (#130) — the image is
  structurally correct but electrically wrong until then. RCP, Router and the
  bootloader remain lidl-only and are skipped for non-lidl boards.
- The NCP `.slcp` flow-control config item now tracks the board's
  `BOARD_UART_FLOW` (alongside the existing VCOM header substitution), so the
  generated project file no longer shows `usartHwFlowControlCtsAndRts` on a
  software-flow board. This changed no binary — the firmware compiles against
  the VCOM header, which was already correct — but the stale slcp value was
  misdiagnosed as the cause of a non-working radio (#130); lidl rebuilds
  byte-identical.
- `flash_efr32.sh` no longer pulses the EFR32 nRST when the chip is **already**
  in the Gecko Bootloader. Entering the bootloader manually (`blmode_pulse` or a
  download-mode reboot) and then running the flasher used to reset the chip back
  into the application before the upload, so the flash silently did not take
  (#130). It now probes the bootloader first and skips the pulse if it answers.

---

## [3.8.1] - 2026-06-09

Docker-image-only fix for the `cpcd-zigbeed` container. No EFR32 firmware,
`cpcd`, or `zigbeed` change.

### Health check no longer false-flags the container as `unhealthy`

`socat-zigbeed` serializes the zigbeed port with `tcp-listen,fork,max-children=1`
(added in 3.8.0). Once a client (Z2M) holds the single slot, `socat` stops
calling `accept()`. The old health check probed the port with
`nc -z localhost ${ZIGBEED_PORT}`, so each probe's connection piled up
unaccepted in the listen backlog; once the backlog filled, every subsequent
`connect()` hung until the 10 s timeout. The container then flapped to
`unhealthy` (and orchestrators that restart unhealthy containers would have
churned it) even though the stack was working — observed in the field as a
`nc` process reaped roughly every 40 s with the stack otherwise stable.

The probe no longer opens a connection: it confirms a `LISTEN` socket exists
on `ZIGBEED_PORT` by scanning `/proc/net/tcp` and `/proc/net/tcp6` (state
`0A`), with no extra binaries. Reproduced and verified on the published
`:3.8.0` image: with the slot held, the old `nc -z` probe hangs to timeout
while the new probe returns in ~2 ms.

---

## [3.8.0] - 2026-06-02

Companion entry to the [v3.8.0 RTL8196E
release](../3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md#380---2026-06-02).
**No EFR32 firmware change** — same `.gbl` artefacts as before. The
host-side RCP stack (`cpcd`) gains a native TCP bus, plus two tooling
fixes.

### `cpcd` — native TCP bus (drops the `socat` PTY shim)

`cpcd` only spoke `bus_type: UART`/`SPI`, so the RCP host stack put a
`socat` in front of it to turn the gateway's in-kernel UART↔TCP bridge
(`TCP:8888`) into a PTY (`/tmp/ttyCpcRcp`). On any TCP blip that `socat`
recreated the PTY, `cpcd`'s serial fd went stale, and `cpcd` + `zigbeed`
had to restart in cascade.

A native `bus_type: TCP` lets `cpcd` dial the bridge directly and own its
own reconnection — no `socat` shim, no stale-PTY restart cascade. Because
a bridge drop loses only the host-side TCP (not the EFR32), the CPC
reliability layer (sequence numbers + ACK + RTO retransmit) recovers the
gap when the link returns: the session resumes with sequence continuity,
no secondary reset, no daemon restart.

Validated against a live EFR32MG1B RCP: the full CPC handshake completes
over the TCP bus, and a mid-session link drop (bridge disarm/re-arm) is
recovered transparently with NOOP keep-alives flowing straight through.

* The cpcd source is a gitignored build clone, so the change is carried as
  `25-RCP-UART-HW/cpcd/tcp-bus.patch` (cpcd v4.5.3) and applied
  idempotently by `build_cpcd.sh` (mirrors the existing `cmakeLists.patch`
  flow). New config keys: `bus_type: TCP`, `tcp_server_address`,
  `tcp_server_port`.
* The Docker stack (`25-RCP-UART-HW/docker/cpcd-zigbeed/`) is wired for it:
  `cpcd.conf.template` uses `bus_type: TCP` and `supervisord.conf` no
  longer runs a `socat-cpc` program. The `Dockerfile.multiarch` builder
  now `git apply`s `tcp-bus.patch` onto its cpcd v4.5.3 clone, so the
  image ships the same patched `cpcd` as the host build — without it the
  image would bundle a stock `cpcd` that rejects `bus_type: TCP` and fail
  to start. The build context is widened to `25-RCP-UART-HW/` (with a
  `.dockerignore` keeping it minimal) so the Dockerfile can reach the
  single-source patch; the CI workflow's `context:` is repointed to match.
  Set `bus_type: UART` with `uart_device_file` to fall back to the classic
  socat-PTY path.
* The native `rcp-stack` (systemd `--user`) flow is migrated too: it
  generates a `bus_type: TCP` `cpcd.conf` from `RCP_ENDPOINT` and the
  `socat-cpc-rcp.service` PTY shim (and its `rcp-socat-rcp` helper) are
  removed — `cpcd-bringup.service` now dials the gateway directly. The
  downstream `socat-zigbeed-pty` (zigbeed↔Z2M) is unchanged.

### `flash_efr32.sh` — false-negative on the post-flash `radio.conf` write

For RCP/NCP/Router flashes (where the `MODE=` line is empty), the SSH
command that rewrites `/userdata/etc/radio.conf` ended in a brace group
whose last statement was `[ -n '' ] && echo` — which exits 1. The whole
remote command returned non-zero, so the script printed "failed to write
/userdata/etc/radio.conf" and aborted before rebooting, even though the
`sed` + appends had all succeeded and `radio.conf` was correct.

Fix: the optional-key lines now use `if … fi` (returns 0 when the key is
absent). Genuine SSH transport failures still surface (`ssh` returns 255).

### `cpcd.conf` — drop the unrecognized `socket_folder` key

cpcd v4.5.3 has no config-file parser for `socket_folder`; it is set only
at build time from `CPC_SOCKET_DIR` (`/dev/shm`) and the per-instance
socket path is hardcoded as `<socket_folder>/cpcd/<instance_name>/`. Every
`cpcd.conf` that set `socket_folder` therefore got a "key not recognized"
warning and the value was silently ignored (and would have double-nested
the path if it had been honoured). The dead line is removed from the three
places that emitted it — the Docker template, the `rcp-stack` example, and
`rcp-stack`'s generated conf — with a comment so it is not re-added. Inert:
the `/dev/shm` default already yields the documented
`/dev/shm/cpcd/<instance>/` path.

### Docker `socat-zigbeed` — one Z2M client at a time

The container exports the shared zigbeed PTY to Z2M over TCP with
`socat tcp-listen:9999,…,fork`, which forks a handler per connection. Two
overlapping Z2M connections (e.g. a Z2M restart racing the old session)
both relayed onto the same PTY, interleaving bytes and corrupting the EZSP
stream — the multi-PID-over-shared-PTY failure seen in Discussion #112.
Adding `max-children=1` keeps the listener alive (so reconnects still work)
but stops it accepting while a client is connected; a second connection is
queued and served only once the active one closes.

* `25-RCP-UART-HW/cpcd/tcp-bus.patch`, `25-RCP-UART-HW/cpcd/build_cpcd.sh`
* `25-RCP-UART-HW/docker/cpcd-zigbeed/{cpcd.conf.template,supervisord.conf}`
* `25-RCP-UART-HW/rcp-stack/examples/cpcd.conf.example`,
  `25-RCP-UART-HW/rcp-stack/bin/rcp-stack`
* `25-RCP-UART-HW/README.md`, `25-RCP-UART-HW/EMBERZNET-8.x-GUIDE.md`
* `flash_efr32.sh`

---

## [3.4.1] - 2026-05-02

Companion entry to the [v3.4.1 RTL8196E
release](../3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md#341---2026-05-02).
**No EFR32 firmware change** — same `.gbl` artefacts as v3.4.0. One
`flash_efr32.sh` fix.

### `flash_efr32.sh` — silent abort when USF doesn't log the bootloader version (#96)

Reported by @frtz13 on a fresh `ncp → otrcp` flash. Symptoms: USF
uploaded the GBL cleanly to 100 %, then the script printed "Flash did
not complete successfully" and left `/userdata/etc/radio.conf` with
the previous `FIRMWARE_BAUD` — the gateway booted at the wrong baud
on the next reboot.

Root cause: the post-flash step that persists `BOOTLOADER_VERSION` to
`radio.conf` greps USF's log for `Detected bootloader version 'X.Y.Z'`.
USF only emits that line when the chip is already in the Gecko
Bootloader at probe time (or when `--bootloader-reset` wired up an
external GPIO/RTS-DTR entry). On the common app→bootloader transition
path (running NCP/RCP/OT-RCP → `launchStandaloneBootloader` → upload),
the version is detected internally but never logged at INFO. The grep
returns 1, `set -euo pipefail` propagates that out of the command
substitution, and the script aborts *before* writing `radio.conf`.

Fix: `|| true` on the grep | tail | sed pipeline so a missing version
line is treated as "version unknown" instead of a fatal error. The
`radio.conf` write proceeds regardless, just without the
`BOOTLOADER_VERSION=` key on this path.

The comment block above the grep is also updated — the previous text
claimed USF emits the line "every time it enters the bootloader",
which was wrong and misled the original change.

### `26-OT-RCP/range-testing/` — Thread mesh range-test toolset and field-test report

Reusable scripts plus a written report for users who want to characterise
their own deployment instead of relying on defaults:

* `gateway/range_test.sh` — generic per-cycle CSV sampler driven by
  `ot-ctl neighbor table`; one invocation = one experimental palier.
* `gateway/phase1_tx_sweep.sh` — TX power sweep with abort-on-detach
  safety, restoring TX to a known-good value on exit.
* `gateway/phase2_channel_migration.sh` — Thread channel migration via
  Pending Operational Dataset, including a back-migration control sample.
* `gateway/orientation_runner.sh` — operator-paced orientation runner
  for either gateway-antenna or sensor-body rotation tests.
* `gateway/healthmon.sh` — opt-in host-side health sampler (memory, CPU
  load, UART1 error counters on the OT-RCP link, Thread role and child
  count, Ethernet errors) at 1 Hz/min. Not a permanent service; users
  start it before a long test and stop it after.
* `analysis/ha_matter_map.py` — bridges HA Matter friendly labels with
  Thread `ext_mac` via the HA WebSocket API (Matter rotates the
  `ext_mac` at every commissioning, so labels can't be inferred from
  the mesh alone).
* `analysis/analyze.py` — pure-stdlib stats (n, mean, median, stddev,
  min/max, mean LQI) per palier and per sensor.

`REPORT.md` documents a 16-sensor home deployment across four phases
(TX power, channel, gateway orientation, sensor orientation) plus a
12 h validation soak. Headline takeaways: TX = 3 dBm carries enough
margin for typical homes, channel choice has sub-dB pooled effect, and
sensor orientation alone can move the uplink RSSI by 22 dB on a single
Matter device — the largest single-axis effect observed in the run.

Recipe 3 ("Recovering a stuck Matter sensor") now leads with a
**non-physical, non-destructive first step** that an SSH into the
gateway can do: `ot-ctl srp server disable && sleep 30 && ot-ctl srp
server enable`. Validated on a 16-sensor run: zero attached sensors
lost, all attached sensors re-publish their SRP record within 1–2 min,
and a fraction of stuck sensors recover in the process. Battery pull
and HA delete+re-pair remain documented as Step 2 and Step 3.

Added `gateway/ha_link_publisher.sh` (+ annotated conf template) — an
opt-in BusyBox shell daemon that polls `ot-ctl neighbor table` at a
configurable interval and pushes one HA entity per Thread child via the
HA REST API: `sensor.thread_<slug>_rssi` with state = avg uplink RSSI
(dBm) and attributes for LQI, last-seen age, RLOC, ext_mac and an
attached/detached flag. Detached devices keep their HA entity alive with
a growing `age_s` so dashboards can flag stale links. Resource footprint
at 60 s cadence with 16 sensors: ~1 % CPU, ~500 KB transient RSS,
~24 MB/day HA traffic, **zero JFFS2 wear** (push-only, no local logs) —
lighter than `healthmon.sh`.

An optional auto-start init script
`gateway/examples/S75ha_link_publisher` is shipped alongside (not in
the default rootfs skeleton — install per-gateway). It is gated on
`/userdata/etc/ha_link_publisher.conf` being present, so it stays inert
on a gateway that does not use the publisher.

## [3.4.0] - 2026-05-01

Companion entry to the [v3.4.0 RTL8196E
release](../3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md#340---2026-05-01).
**No EFR32 firmware change** — same `.gbl` artefacts as v3.3.0 for NCP /
RCP / OT-RCP / Router / Bootloader. No tooling change in this directory
either; `flash_efr32.sh` is unchanged.

The release on the gateway side is a kernel-driver hardening pass plus a
perf tuning that lowers the latency tail on the UART1 path used by the
EFR32 radio. Re-flashing the radio is **not** required for the upgrade.

### Why this matters here even with no firmware change

The kernel `irq-rtl819x` driver now routes UART1 (the line carrying the
Spinel / EZSP / CPC traffic to this chip) on MIPS IP4 instead of IP3, so
under simultaneous Ethernet + radio activity the UART RX ISR runs first.
At 460800 baud with the 16-byte RX FIFO that absorbs ~350 µs of latency
budget, the change makes the gateway slightly more tolerant to bursts
during heavy LAN traffic — particularly during Matter commissioning
attestation while the host is doing iperf-class TCP at the same time.
Validated with an overnight OT-RCP soak: zero overruns on `ttyS1` over
8h+, two paired Sleepy End Devices stable.

The `8250_rtl819x` driver fixes from v3.3.0 (`MCR_AFE` plumbing for #89)
are unchanged.

---

## [3.3.0] - 2026-04-30

Companion entry to the [v3.3.0 RTL8196E
release](../3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md#330---2026-04-30).
The EFR32 firmwares are unchanged from v3.2.x — same `.gbl` artefacts —
but tooling and host-side integration changed substantially.

### `flash_efr32.sh` — refactor (TODO-v3.3 #1, #2, #5)

* **Bridge ↔ `radio.conf` reconciliation** — switching modes by
  editing `radio.conf` no longer requires manual sysfs rearm.
* **Symmetric baud-fallback sweep** `{115200, 230400, 460800, 691200,
  892857}` — chip auto-detected even when `radio.conf` is stale, in
  either direction.
* **`--firmware-file PATH`** for explicit GBL selection; refuses
  ambiguous matches (used to silently pick newest by mtime).
* **USF venv version pin sanity check** — abort on drift.
* **`assert_bridge_idle()`** + read-back-verified sysfs writes.

### Documentation

- [`26-OT-RCP/README.md`](26-OT-RCP/README.md) "Hardware flow control"
  note expanded to document the v3.3.0 root-cause fix for
  [#89](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/89):
  the host-side `&uart-flow-control=true` in `S70otbr`'s spinel radio
  URL is required for reliable operation at 460800 — the EFR32 firmware
  always had RTS/CTS enabled, but without the host flag the kernel
  UART would not engage `MCR_AFE` and the RX FIFO could overrun.

---

## [3.1.1] - 2026-04-27

Companion entry to the [v3.1.1 RTL8196E
release](../3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md#311---2026-04-27)
— the heavy lifting (kernel UART-bridge hardening,
`flash_efr32.sh` TCP-client safety check, `radio.conf`
simplification) lives there. EFR32 side has only documentation
cleanups in this cycle.

### Documentation

- `23-Bootloader-UART-Xmodem/firmware/README.md` — drop the dead
  link to the unshipped Stage-2-only
  `bootloader-uart-xmodem-2.4.2.s37` (`*.s37` is gitignored except
  the `-combined.s37` artefact, see commit `7d67772`); replaced with
  a "build it locally" pointer. Restored a green `mkdocs --strict`
  CI build.
- Per-firmware READMEs (`24-NCP`, `25-RCP`, `26-OT-RCP`, `27-Router`)
  + the top-level `README.md` updated for the new single-key
  `radio.conf` model: `FIRMWARE_BAUD` is now the canonical baud
  reference (chip-side = host-side, since both ends of the UART
  link must agree). Legacy `BRIDGE_BAUD` / `OTBR_BAUD` references
  removed from user-facing prose; `flash_efr32.sh` stops emitting
  them and strips them from existing configs on every flash.
- `26-OT-RCP/docker/README.md` — three-case switching recipe
  (ZoH / OTBR-host / OTBR-gateway) collapses to a one-line `sed`
  flipping `MODE=otbr` on/off; no more `BRIDGE_BAUD` ↔ `OTBR_BAUD`
  swap.

---

## [3.1.0] - 2026-04-26

Build matrix and documentation pass. Each per-firmware `build_*.sh`
script now takes the UART baud as a positional argument and emits
baud-aware filenames so multiple bauds can coexist in `firmware/`. A
new top-level `make-all-bauds.sh` builds the full matrix in one run.
Pre-built artefacts ship for every supported baud point so users
without a Silabs toolchain can flash any combination directly. The
companion `flash_efr32.sh` (top-level, see
`../3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md` for the full refactor
notes) resolves the right `.gbl` via a glob — no more EmberZNet SDK
lookup.

### Build matrix

- Per-firmware build scripts (`build_ncp.sh`, `build_rcp.sh`,
  `build_ot_rcp.sh`, `build_router.sh`) take an optional positional
  baud:
  ```
  ./build_ncp.sh                # default per-firmware baud
  ./build_ncp.sh 460800         # explicit override
  ```
- Output `.gbl` / `.s37` filenames embed the baud:
  ```
  ncp-uart-hw-7.5.1-<baud>.gbl
  rcp-uart-802154-<baud>.gbl
  ot-rcp-<baud>.gbl
  z3-router-7.5.1-<baud>.gbl
  ```
- New `make-all-bauds.sh` wrapper builds all variants in one run
  (NCP×5, RCP×3, OT-RCP×1, Router×1), idempotent (skips files that
  already exist), with `--force` and `--list` options.

### Pre-built firmware shipped

| Firmware | Baud points |
|---|---|
| NCP-UART-HW (7.5.1) | 115200 / 230400 / 460800 / 691200 / 892857 |
| RCP-UART-HW | 115200 / 230400 / 460800 *(cpcd POSIX baud ceiling)* |
| OT-RCP | 460800 *(matches OpenThread default)* |
| Z3 Router (7.5.1) | 115200 *(no UART data path)* |

End-to-end validated on hardware against Z2M (NCP & RCP), the ZoH
adapter (OT-RCP bridge mode), `otbr-agent` in Docker, and on-gateway
`otbr-agent`.

### `firmware/` directory cleanup

Two batches of dead weight removed from the per-firmware `firmware/`
directories:

* **Unshipped `.s37` artefacts** — every per-firmware build emits
  both `.gbl` (UART/OTA path) and `.s37` (J-Link/SWD path), but only
  the *combined-bootloader* `.s37` is end-to-end useful for users
  (the one-shot J-Link image to install Stage 1 + Stage 2 on a virgin
  chip). Dropped 15 orphan `.s37` files (~1.4 MiB) from `23-`, `24-`,
  `25-`, `26-`, `27-` and tightened `.gitignore` so future builds
  don't re-introduce them. Kept:
  `23-Bootloader-UART-Xmodem/firmware/bootloader-uart-xmodem-2.4.2-combined.s37`.

* **Pre-v3.0 manual-flash legacy in `24-NCP-UART-HW/firmware/`** —
  the directory still carried `flash_ezsp{7,8,13}.sh` plus a MIPS
  `sx` xmodem-send binary (~166 KiB) from the pre-v3.0 workflow
  (scp the script to the gateway, push the `.gbl` over `sx` on the
  serial line). Replaced end-to-end by the repo-root `flash_efr32.sh`
  driving `universal-silabs-flasher` over the in-kernel UART bridge —
  these scripts have been unreachable from the docs since v3.0.
  Removed.

### `POST-MORTEM-bootloader-recovery.md`

New top-of-tree post-mortem documenting why a hardware `nRST` pulse
on the EFR32 cannot enter the Gecko Bootloader on this gateway, and
what was tried:

- PIN reset always boots the application slot — Gecko Stage-2 only
  enters its UART menu on `SYSREQ`+magic or a `BTL_GPIO_ACTIVATION`
  pin pulled by the host, neither of which is wired on the Lidl PCB.
- `PB11` (the canonical `BTL_GPIO_ACTIVATION` pin in old Gecko
  bootloaders) was checked empirically — not routed to the RTL8196E.
- Tuya stock firmware confirms the limit: zero `/sys/class/gpio`
  references, recovery is software-only via
  `ezspLaunchStandaloneBootloader`.

Lists two untested alternative paths (A: chip-reset on every reboot;
B: `PA5`/CTS as `BTL_GPIO_ACTIVATION`) for future work — Alternative
A landed in v3.1 (see RTL8196E CHANGELOG); B is parked.

### Documentation

- Per-firmware READMEs (`24-NCP`, `25-RCP`, `26-OT-RCP`, `27-Router`)
  rewritten for v3.1: new `flash_efr32.sh` CLI, baud-aware
  filenames, gateway-side `radio.conf` keys (`MODE`, `BRIDGE_BAUD`,
  `OTBR_BAUD`) plus the new chip-identity keys (`FIRMWARE`,
  `FIRMWARE_VERSION`, `FIRMWARE_BAUD`) shown in every "Gateway state
  after flash" snippet.
- `2-Zigbee-Radio-Silabs-EFR32/README.md` adds a "Gateway-side
  runtime configuration" section, a per-firmware supported-baud
  table, and splits the `radio.conf` keys into "chip-identity" vs
  "daemon-routing" so readers see at a glance what's informational
  vs operational.
- `22-Backup-Flash-Restore/README.md` and
  `23-Bootloader-UART-Xmodem/README.md` refreshed (USF probe-methods
  patch reference; chained bootloader+app flash walkthrough).
- `26-OT-RCP/docker/README.md` lays out the three OT-RCP use cases
  side-by-side with their gateway-side configuration; emphasises
  that all three share `FIRMWARE=otrcp` (the chip is the same; only
  the daemon-routing keys differ).
- `25-RCP` and `26-OT-RCP` Z2M `configuration.yaml` examples now
  externalise the device list (`devices: devices.yaml`) like 24-NCP
  does — keeps personal IEEE addresses out of git.

> Canonical full reference for `radio.conf` keys (including the new
> `FIRMWARE` / `FIRMWARE_VERSION` / `FIRMWARE_BAUD`) lives in
> [`../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md`](../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#radioconf-keys-full-reference);
> per-firmware READMEs link to it instead of duplicating.

---

## [3.0.0] - 2026-04-16

### UART baud rates — 230400 ceiling removed

The long-standing 230400 baud limit has been eliminated across all
firmwares. The root cause was an RTL8196E UART divisor N+1 quirk (see
`3-Main-SoC-Realtek-RTL8196E/32-Kernel/POST-MORTEM-6.18.md`), not
userspace latency as previously believed.

**Tested baud rates with zero framing/overrun errors:**

| Firmware | Default baud | Max tested | Transport |
|----------|-------------|------------|-----------|
| NCP-UART-HW | 115200 | 892857 | in-kernel UART↔TCP bridge |
| RCP-UART-HW | **460800** | 460800 | cpcd via in-kernel UART↔TCP bridge (cpcd has no 892857 support) |
| OT-RCP | **460800** | 460800 | otbr-agent (direct UART, on-gateway) |
| Router | 115200 | N/A | No UART data path |

### 26-OT-RCP
- **Default baud raised to 460800** — aligns with OpenThread's own
  default. Firmware, S70otbr init script, docker compose, and all
  documentation updated. OTBR users get 4× throughput with no
  configuration change.
- Pre-built firmware rebuilt at 460800.

### 24-NCP-UART-HW
- **Firmware rebuilt at 460800** for testing (committed earlier on
  `kernel-6.18` branch). Default distribution remains 115200; power
  users can rebuild at up to 892857 and set the in-kernel UART bridge
  baud to match via `/userdata/etc/radio.conf:BRIDGE_BAUD=`.
- Z2M `configuration.yaml` updated with `baudrate: 460800`.

### flash_efr32.sh
- **Flash any firmware from any baud/mode state.** The script now handles
  all transitions (NCP↔OT-RCP↔RCP) regardless of the current firmware
  baud rate (115200–892857).
- **Smart detection via radio.conf**: reads the persistent radio mode
  (`MODE=otbr` → Spinel@460800) and `BRIDGE_BAUD=` to pick the right
  probe speed, instead of relying on `ps | grep` (which missed crashed
  daemons on the old serialgateway-based path).
- **Targeted probing**: OT-RCP probes `spinel:460800` only (~15ms);
  NCP/RCP probes `ezsp`+`cpc` at detected baud. No more 30s full scan.
- **FailedToEnterBootloaderError recovery**: when USF detects the
  firmware and enters the Gecko Bootloader (baud changes to 115200),
  the script automatically switches the in-kernel bridge to 115200
  (flow control off) and flashes via `bootloader:115200`.
- **TCP port readiness**: `wait_for_port` polls TCP:8888 after every
  bridge reconfiguration, replacing fragile `sleep 1`. Prevents USF
  `AssertionError` crashes on unstable connections.
- **USF probe retry**: transient transport errors (TCP not fully ready)
  trigger one automatic retry instead of aborting.
- **radio.conf cleanup**: NCP/RCP/Router flash deletes radio.conf
  (`rm -f`) instead of leaving a 0-byte ghost file.
- **USF probe patch regenerated**: all bauds 115200–892857 for EZSP,
  Spinel, and CPC protocols.

### 25-RCP-UART-HW
- **RCP@460800 validated**: pre-built firmware rebuilt at 460800 baud.
  Tested with cpcd 4.5.3 + zigbeed 8.2.2 (EZSP v18) + Z2M. cpcd
  does not support non-standard bauds (892857), so 460800 is the RCP
  maximum.
- **Simplicity SDK 2025.6.2 → 2025.6.3**: zigbeed build updated to
  latest patch (Feb 2026). End-device move delay config, Green Power
  fixes. EmberZNet stays 8.2.2 (build 436→532), EZSP v18.
- **Removed MEMO-uart-bridge-kernel.md** — kept on `kernel-6.18` branch.

### 25-RCP-UART-HW — multipan POC explored, tested, dropped

A Zigbee + Thread multipan Docker stack (cpcd + zigbeed on IID=1 +
otbr-agent on IID=2) was drafted and added to the tree during v3.0
dev. End-to-end test on hardware: cpcd connects, zigbeed attaches on
IID=1 (EZSP v18), but **otbr-agent fails on IID=2** with
`GetIidListFromUrl: InvalidArgument`. Root cause is a hardware limit —
Silicon Labs' Concurrent Multiprotocol (CMP, concurrent Zigbee + Thread
on one radio) is a **Series 2-only** feature; our EFR32MG1B is
Series 1 and only supports Dynamic Multiprotocol (BLE + one of
Zigbee/Thread, never both 15.4 protocols together). GSDK 4.5.0 has no
multi-PAN RCP sample for MG1B.

Since the gateway's hardware will never change, the whole POC was
dropped: `docker-compose-multipan.yml`,
`cpcd-zigbeed-otbr/Dockerfile.multiarch`,
`z2m/configuration-multipan.yaml`, and the CI workflow that built
`:poc`. A short `cpcd-zigbeed-otbr/README.md` remains as a tombstone
pointing at the working single-protocol paths
(Zigbee via `docker-compose-zigbee.yml`, Matter-over-Thread via
`../../26-OT-RCP/docker/docker-compose-otbr-host.yml`).

**Correction (post-v4.5.0):** the failure above was misdiagnosed. The
`GetIidListFromUrl()` error came from an OpenThread POSIX host build without
`OT_MULTIPAN_RCP=ON`; it occurred in the host URL parser before any RCP query.
With that option enabled, two static RCP instances, and the explicit allocation
broadcast/IID 0, Zigbee/IID 1, Thread/IID 2, the Lidl EFR32MG1B ran both
protocols concurrently in a short channel-11 test. Series 1 supports this only
when the Zigbee and Thread networks share a channel. Independent-channel
operation remains a Series 2 Concurrent Listening requirement. Longer
mixed-traffic testing and validation with a real Thread device are still
pending, so the restored multi-PAN path is experimental.

### Firmware rebuild against v3.0 sources

All five firmwares rebuilt against the current sources (GSDK 4.5.0 +
ARM GCC 12.2). Stage 2 bootloader `.gbl`/`.s37`, NCP and Router
produce **bit-identical binaries** (deterministic build, sources
unchanged). RCP and OT-RCP pick up a +88 B delta coming from the
`.slcp` baudrate cleanup below. The Stage 2+Stage 1 combined `.s37`
sees a small metadata-only delta.

### 25-RCP, 26-OT-RCP — .slcp baudrate realigned with .h override

In `rcp-uart-802154.slcp` and `ot-rcp.slcp`, the `BAUDRATE` config
value was 115200 at the `.slcp` level but 460800 in the `.h` patches
that overlay the generated config. The `.h` wins at compile time, so
runtime was already 460800 — but the two layers disagreeing misled
anyone reading the `.slcp`. Normalised to 460800 on both; the Silabs
Configuration Wizard `<i> Default: 115200` hints are kept since they
legitimately document the upstream SDK default.

### 24-NCP-UART-HW — Z2M device list externalised

- `z2m/configuration.yaml` no longer carries a hard-coded `devices:`
  block; the list lives in a separate `devices.yaml`. Device-roster
  updates no longer churn the main config.
- Unused `baudrate:` dropped from the Z2M config (inherited from the
  serial adapter URL).

### Tooling

- `25-RCP-UART-HW/patches/measure_uart_overruns.sh` — dev helper that
  reads UART framing/overrun counters via sysfs during RCP stress tests.

### Documentation
- All firmware READMEs (24-NCP, 25-RCP, 26-OT-RCP, 27-Router) updated:
  replaced "460800+ not supported" with full baud rate table; removed
  all references to in-kernel UART bridge.
- EMBERZNET-8.x-GUIDE.md: removed "overruns" warning.

---

## [2.1.5] - 2026-04-04

### 26-OT-RCP (OTBR on gateway)
- **HA REST API: PascalCase kept**. `python-otbr-api` 2.9.0 (HA 2026.4)
  still sends PascalCase in PUT requests — upstream camelCase `otbr-agent`
  rejects them. PascalCase `otbr-agent` works with all HA versions.
  See [python-otbr-api#238](https://github.com/home-assistant-libs/python-otbr-api/issues/238).

---

## [2.1.3] - 2026-04-01

### 26-OT-RCP (OTBR on gateway)
- **IPv6 mDNS fix (`accept_ra=2`)**: `S70otbr` enabled IPv6 forwarding
  which silently disabled Router Advertisement processing. The gateway
  never acquired a GUA via SLAAC, so mDNS only announced the IPv4
  address. Fixed: set `accept_ra=2` on eth0 after enabling forwarding. (#77)
- **Channel Manager enabled**: `otbr-agent` now built with
  `OT_CHANNEL_MANAGER` and `OT_CHANNEL_MONITOR` (+14 KB). Enables
  `ot-ctl channel manager` for graceful channel changes across the
  Thread mesh. Channel change also works from the HA Thread UI.
- **HA REST API compatibility**: `build_otbr.sh` patches ot-br-posix
  REST API JSON keys from camelCase back to PascalCase at build time.
  Fixes "Failed to call OTBR API" in Home Assistant's Thread integration
  (`python-otbr-api` < 2.9.0 expects PascalCase).

---

## [2.1.1] - 2026-03-22

### flash_efr32.sh
- **Auto-reinstall USF on patch change**: `flash_efr32.sh` now stores the
  md5 hash of the applied `silabs-flasher-probe-methods.patch` in the venv.
  On next launch, if the patch has changed, the venv is removed and USF is
  reinstalled with the new patch automatically.

---

## [2.1.0] - 2026-03-21

### 24-NCP-UART-HW
- **Docker Compose stack**: new `docker/` directory with Mosquitto + Zigbee2MQTT
  (ember adapter) + Home Assistant. Self-contained for NCP users.

### flash_efr32.sh
- **OTBR support**: stops otbr-agent, cpcd, zigbeed before starting serialgateway
  in flash mode — no longer requires manual daemon management.
- **Remove 460800 baud**: gateway UART unreliable at 460800 (see 25-RCP-UART-HW).
  Removed from baud rate recovery scan and USF probe patch. Saves ~30s on flash.

### silabs-flasher-probe-methods.patch
- Drop all 460800 entries (EZSP, SPINEL, CPC). Add EZSP@230400, SPINEL@115200,
  SPINEL@230400.

### 22-Backup-Flash-Restore
- MEMO-universal-silabs-flasher.md: document 460800 removal rationale.

---

## [2.0.0] - 2026-03-11

### 26-OT-RCP
- **3 use cases, 1 firmware:** ZoH (Zigbee on Host), OTBR on host (Docker),
  OTBR on gateway (native) — all use the same OT-RCP firmware
- 3 Docker Compose stacks: `docker-compose-zoh.yml`, `docker-compose-otbr-host.yml`,
  `docker-compose-otbr-gateway.yml`
- Radio mode switching support in Docker compose files
- Thread network formation guide (use case 3: OTBR on gateway)
- Tested devices: IKEA TIMMERFLOTTE, BILRESA, MYGGSPRAY (all Matter/Thread)
- README restructured around the 3 use cases with architecture diagrams

### 22-Backup-Flash-Restore
- Technical memo: [MEMO-universal-silabs-flasher.md](22-Backup-Flash-Restore/MEMO-universal-silabs-flasher.md)
  — how USF works over TCP, baud rate mismatch problem, recovery mechanism

### flash_efr32.sh
- **Auto-recovery from non-standard baud rates:** when firmware runs at 230400
  or 460800 (e.g., after a custom build), the script scans baud rates, lets USF
  detect and enter the Gecko Bootloader, restarts serialgateway at 115200, and
  flashes. Tested with Spinel (OT-RCP) and EZSP (NCP).
- Patches USF probe methods at install time (`silabs-flasher-probe-methods.patch`)
  to add SPINEL@115200, SPINEL@230400, EZSP@230400
- SSH `-n` flag prevents stdin conflicts when piping firmware selection
- Default gateway IP: 192.168.1.88 (replaces placeholder throughout docs)

### Documentation
- IP placeholders replaced with default 192.168.1.88 across all Docker and Z2M configs

---

## [1.2.1] - 2026-03-05

### 26-OT-RCP
- Docker Compose stack for Thread/Matter: OTBR + Matter Server + Home Assistant
- Docker Compose stack for Zigbee: Zigbee2MQTT with zigbee-on-host (`zoh`) adapter
- Matter commissioning via HA Companion App (replaces chip-tool)
- Documented full setup: IPv6 forwarding, OTBR integration, Thread credentials sync
- Thread/Matter primer for Zigbee users (`THREAD-MATTER-PRIMER.md`)
- Tested: IKEA TIMMERFLOTTE (22.8 °C, 54.69 %, battery 100 %)
- Removed erroneous 460800 baud memo (actual root cause: PCB signal integrity)

### Build environment
- Unified build scripts: `build_rtl8196e.sh` (bootloader + kernel + rootfs + userdata), `build_efr32.sh` (all 5 firmware)
- Fixed Docker builds: GLIBC mismatch, lzma conflict, tool path detection
- All 9 build scripts work both in Docker and natively
- `nano` and `serialgateway` binaries now committed to skeleton for fresh clones

---

## [1.2.0] - 2026-03-02

### flash_efr32.sh (new — repository root)
- OTA flash script for EFR32 via SSH + universal-silabs-flasher
- Firmware selection menu: bootloader, NCP-UART-HW, RCP-UART-HW, OT-RCP, Z3-Router
- Bootloader flash automatically chains application firmware
- SSH retry (3 attempts, ConnectTimeout=10) for unreliable networks
- Progress bar visible for normal firmware flash
- Prerequisite checks: python3, python3-venv

### 23-Bootloader-UART-Xmodem
- Pre-built UART Xmodem firmware v2.4.2

### 26-OT-RCP
- Rebuilt firmware with PTI warning fix and Spinel bootloader reset support
- Removed orphan iostream config; clarified uartdrv vs iostream usage in README

### Documentation
- All firmware READMEs (23, 24, 25, 26, 27) updated: flash instructions now reference `flash_efr32.sh`
- Set `JAVA_TOOL_OPTIONS` in all build scripts so slc finds the trusted SDK

---

## [1.1.0] - 2026-01-25

### Build environment
- Updated Silabs toolchain: slc-cli 5.11, GSDK 4.5.0
- Silabs tools installed in project directory (like x-tools)

### 23-Bootloader-UART-Xmodem
- Aligned with Simplicity Studio standard project structure

### 25-RCP-UART-HW (new)
- Pre-built RCP firmware (CPC Protocol v5, GSDK 4.5.0)
- `rcp-stack` systemd service manager for cpcd + zigbeed chain
- zigbeed build scripts for EmberZNet 7.5.1 and 8.2.2
- Docker stack: cpcd-zigbeed + Zigbee2MQTT (amd64/arm64), based on Nerivec pre-built binaries
- cpcd/zigbeed build: fixed interactive prompts, dropped unused deps, added `--local`/`--deb` flags
- rcp-stack: fixed crash on empty env file, unbound variable on first run, symlink cleanup race

### 26-OT-RCP (new)
- OpenThread RCP firmware for zigbee-on-host (Z2M `zoh` adapter)
- Fixed flow control configuration for serialgateway compatibility

### 27-Router (new)
- Zigbee 3.0 Router SoC firmware with auto-join and network steering
- Mini-CLI: `bootloader reboot`, `network status/leave/steer`, `version`, `info`, `help`
- ZCL Basic Cluster: LidlRouter model, Silvercrest manufacturer, SW Build ID 1.0.0

---

## [1.0.0] - 2025-12-18

Initial release.

### 22-Backup-Flash-Restore
- Documentation for backing up and restoring the EFR32 via universal-silabs-flasher

### 23-Bootloader-UART-Xmodem
- Build script for Gecko UART Xmodem bootloader

### 24-NCP-UART-HW
- Pre-built NCP firmware v7.5.1 (EZSP v13, EmberZNet 7.5.1)
- Build script and patch system for customization
