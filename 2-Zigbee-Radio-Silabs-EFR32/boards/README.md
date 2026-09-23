# EFR32 firmware — per-board configuration (`BOARD=`)

The EFR32 firmware builds are parameterised by board, mirroring the `BOARD=`
mechanism on the RTL8196E bootloader side (`3-Main-SoC-Realtek-RTL8196E/31-Bootloader/boards/`).
A board contributes one file — `boards/<board>/board.env` — and the build
scripts read it; no script edits are needed to add a board.

```
boards/
├── README.md                 this file
├── lib_uart_config.sh        shared helper: applies BOARD_UART_* to a VCOM header
├── lidl/board.env            reference board (default)
└── sengled-e39-g8c/board.env contributed, every firmware prebuilt (#130, #148)
```

## Usage

```bash
# Build
./build_efr32.sh ncp                 # BOARD=lidl (default)
BOARD=sengled-e39-g8c ./build_efr32.sh ncp ot-rcp router
BOARD=lidl ./24-NCP-UART-HW/build_ncp.sh   # per-firmware scripts honour BOARD too
BOARD=sengled-e39-g8c ./make-all-bauds.sh  # that board's committed baud matrix

# Flash (repo root) — same BOARD= selector
./flash_efr32.sh -y ncp                          # lidl (default), nothing to set
BOARD=sengled-e39-g8c ./flash_efr32.sh -y ncp    # flashes the -<board>-suffixed GBL
```

Non-lidl artefacts keep the historical flat `firmware/` directory but carry a
`-<board>` filename suffix (e.g. `ncp-uart-hw-7.5.1-115200-sw-sengled-e39-g8c.gbl`,
the `-sw-` being the board's flow-control type, #145),
so the lidl reference firmware is never shadowed. `flash_efr32.sh` resolves that
suffixed file for a non-lidl `BOARD=` and, because it always runs against a live
gateway, **guards on `/proc/device-tree/model`**: it refuses to push a board's
radio firmware to a different board (`lidl`→"Lidl", `sengled-e39-g8c`→"Sengled"),
which also catches forgetting `BOARD=` on a non-lidl box. `--force` overrides.

Scope today: **every firmware build is board-parameterised** (build *and*
flash) — NCP and OT-RCP since #130, the Z3 Router, the Gecko bootloader and
RCP since #143. Two flow-control specifics: the bootloader consumes only the
routing subset of `board.env` (`apply_uart_routing` — its flow control is a
separate numeric knob kept at 0 for every board, because the Xmodem path
always runs flow-off), and CPC (RCP) supports only RTS/CTS or none, so a
`BOARD_UART_FLOW=sw` board is built with flow control **none** — recorded as
such in `radio.conf` at flash time, so the in-kernel bridge (the chip's flow
partner; cpcd connects to it over TCP) arms to match.

## What `board.env` defines

| Variable | Meaning |
|---|---|
| `BOARD_NAME` | Human-readable board name (banners only) |
| `BOARD_TARGET_DEVICE` | Exact MCU OPN passed to `slc generate --with` |
| `BOARD_UART_PERIPHERAL` / `_NO` | USART instance feeding the RTL8196E (e.g. `USART0` / `0`) |
| `BOARD_UART_TX` / `_RX` | `"<port-letter> <pin> <location>"` for each data line |
| `BOARD_UART_FLOW` | `hw` (RTS/CTS handshake), `sw` (software XON/XOFF), or `none`. Also recorded by `flash_efr32.sh` into `radio.conf` as `FIRMWARE_FLOW_CTRL` on every app flash (#141), so the host side follows automatically. For OT-RCP it additionally selects the UART backend: `sw` → `iostream_usart` (complete XON/XOFF), `hw`/`none` → `uartdrv_usart` (DMA — #142) |
| `BOARD_UART_CTS` / `_RTS` | `"<port-letter> <pin> <location>"`; ignored when flow ≠ `hw` |
| `BOARD_RCP_DEFAULT_BAUD` / `BOARD_OT_RCP_DEFAULT_BAUD` | **Optional.** The baud `flash_efr32.sh` offers by default for that firmware on this board, overriding the project default (460800, which assumes the reference board's RTS/CTS wiring). A board without it has a lower ceiling — the G4 sets both to `230400` (#134, #142). Refused at board-selection time if outside the firmware's supported set |
| `BOARD_NCP_BAUDS` / `BOARD_RCP_BAUDS` / `BOARD_OT_RCP_BAUDS` / `BOARD_ROUTER_BAUDS` | **Optional.** The bauds this board commits prebuilts for, i.e. what `BOARD=<board> ./make-all-bauds.sh` builds. Each key falls back to the reference matrix when absent, so a board declares only the rows it changes (the G4 declares all four: one baud per firmware, and an empty `BOARD_RCP_BAUDS=""` for "no committed RCP") |
| `BOARD_BTL_ACTIVATION_PIN` | **Optional.** `"<port-letter> <pin>"` — the EFR32 pin the host can pull to force the radio into its bootloader. Set it only if the board actually wires such a line to a SoC GPIO (the Sengled G4 does: `blmode-gpios` in its devicetree; the Lidl does not). `build_bootloader.sh` then adds the `bootloader_gpio_activation` component and points it at this pin, active LOW (#148). Omit the key and the bootloader is built exactly as before |
| `BOARD_BTL_CUSTOMER` | Bootloader revision — the low 16 bits of the Gecko version word (`major<<24 \| minor<<16 \| customer`). `2.4` is Silicon Labs'; this third number is the customer field, which the SDK leaves to the integrator. **Bump it whenever this board's bootloader binary changes**: the chip installs a stage-2 image over UART only if its version is *strictly greater* than the running one, and declines in silence otherwise — after staging the image inside application space, which erases the app (#148). Default `2`. Lidl stays `2` (its bootloader has not changed); the G4 is `3` (it gained GPIO activation) |

The build copies the firmware's reference VCOM header from its `patches/` tree,
then `lib_uart_config.sh` substitutes the board's values **in place**, changing
only the value token on each `#define` and preserving the file's formatting.
For the `lidl` board the values equal the reference, so the header — and the
resulting firmware — is byte-for-byte unchanged. The same helper drives both
the iostream header (NCP) and the uartdrv header (OT-RCP); the SDK enum names
for flow control differ between them and are supplied by each build script
(`hw`→`usartHwFlowControlCtsAndRts`/`uartdrvFlowControlHw`,
`sw`→`uartFlowControlSoftware`/`uartdrvFlowControlSw`,
`none`→the respective `…None`). Software flow is a first-class SDK option, not
a patch — the generated init code keys off the same `_FLOW_CONTROL_TYPE` token.

> **G4 status (#130):** `BOARD=sengled-e39-g8c` builds every firmware for the
> board (MG13 target, software flow). The NCP `.slcp` pinned the lidl MCU as
> a device component, so the build re-points it at `BOARD_TARGET_DEVICE` before
> `slc generate` (otherwise two device families link → duplicate symbols); for
> lidl that is the same string, so its build is unchanged. The G4 wires the EFR32
> UART on the **same USART/pins as Lidl** (USART0, PA0/PA1), confirmed on hardware
> by @hlyi — so the firmware is electrically correct, not just structurally.

Every G4 firmware except the RCP ships prebuilt. The
images were built from the board facts above; **what differs between them is
how much hardware evidence stands behind each one**, and that is worth reading
before you flash:

| Firmware | Committed prebuilt | Baud | Hardware evidence |
|---|---|---|---|
| NCP | `ncp-uart-hw-7.5.1-115200-sw-sengled-e39-g8c.gbl` | 115200 | Flashed to a real G4 and validated end-to-end — Home Assistant talks to the radio (#130). |
| Gecko bootloader | `bootloader-uart-xmodem-2.4.3-sengled-e39-g8c.gbl` | — | Run on @hlyi's G4: GPIO activation on PB15 drops the chip into the Gecko Bootloader (#148). |
| OT-RCP | `ot-rcp-230400-sw-iostream-sengled-e39-g8c.gbl` | 230400 | 230400 is @hlyi's measured operating point on this board (#134, #142); at 460800 the host's 16-byte RX FIFO overruns. Our build of those sources, not the binary he ran. |
| RCP | *not shipped* — build it with `BOARD=sengled-e39-g8c ./25-RCP-UART-HW/build_rcp.sh 230400` | 230400 | **Never run on a G4**, so no prebuilt is committed. Flow clamped to none (CPC has no XON/XOFF; recorded as none in `radio.conf` so the bridge matches). |
| Z3 Router | `z3-router-7.5.1-115200-sw-sengled-e39-g8c.gbl` | 115200 | **Never run on a G4** (#143). |

The bauds above are what `flash_efr32.sh` picks by itself on this board — the
230400 defaults come from `BOARD_RCP_DEFAULT_BAUD` / `BOARD_OT_RCP_DEFAULT_BAUD`
in `board.env`, since the project defaults (460800) assume the Lidl's RTS/CTS
wiring. For the bootloader, note that a bad flash is an SWD-only recovery: a G4
flashed via `flash_efr32.sh` already has a working bootloader, so only replace
it with a debugger attached. Its build is structurally verified too — correct
GCC xG13 first stage, placed in the MG13's dedicated bootloader region at
`0x0FE10000` (#143).

Rebuild the three committed application images in one shot — `make-all-bauds.sh` takes the
same `BOARD=` selector and reads the board's committed matrix from its
`board.env` (`BOARD_NCP_BAUDS` and friends):

```bash
BOARD=sengled-e39-g8c ./make-all-bauds.sh --list   # what it would build
BOARD=sengled-e39-g8c ./make-all-bauds.sh          # build what's missing
BOARD=sengled-e39-g8c ./make-all-bauds.sh --force  # rebuild all three
```

Or one at a time, with the exact commands that produced the committed images
(the bootloader carries no baud and is not part of that matrix):

```bash
BOARD=sengled-e39-g8c ./24-NCP-UART-HW/build_ncp.sh 115200
BOARD=sengled-e39-g8c ./25-RCP-UART-HW/build_rcp.sh 230400
BOARD=sengled-e39-g8c ./26-OT-RCP/build_ot_rcp.sh 230400
BOARD=sengled-e39-g8c ./27-Router/build_router.sh 115200
BOARD=sengled-e39-g8c ./23-Bootloader-UART-Xmodem/build_bootloader.sh
```

## Porting contract

Mechanism and the `lidl` reference come from upstream; a contributor supplies a
`board.env` with **values validated on real hardware** and PRs it (the model
used for the bootloader `board.h` in #128). The OPN alone is not enough — the
USART/pin routing and the flow-control wiring are board facts that only a
hardware check confirms, and untested radio firmware is an SWD-rescue risk.
Validate the built NCP/OT-RCP `.gbl` over the device before trusting it.

`BOARD_BTL_ACTIVATION_PIN` deserves that warning twice over: it goes into the
**bootloader**, and it must be the pin the board really routes — traced on the
PCB, or read out of the stock bootloader. A wrong pin does not brick the board
(worst case the bootloader sees the line as permanently active and stops
handing over to the application, which is still recoverable over UART through
the bootloader's own menu), but it is the one value here that ships inside the
component of last resort. If the board wires no such line, leave the key out:
that is the reference board's case, and the correct one.
