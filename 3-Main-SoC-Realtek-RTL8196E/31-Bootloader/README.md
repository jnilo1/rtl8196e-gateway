# Open-Source Bootloader for RTL8196E

Replacement bootloader for RTL8196E gateways — the Lidl Silvercrest Zigbee gateway (`BOARD=lidl`, the default) and the Sengled Smart Hub G4 (`BOARD=sengled-e39-g8c`), each with its own prebuilt `boot.bin`.

## Why use this bootloader

This is the **last missing piece** that makes the entire gateway firmware stack fully open-source — from bootloader to kernel, rootfs, and Zigbee radio firmware.

**Modern toolchain** — Built with the project's Lexra cross-toolchain (GCC 15 / binutils 2.45 / musl 1.2, crosstool-NG), replacing the legacy Realtek RSDK (GCC 4.6). The code has been simplified and made portable to standard toolchains.

**Clean boot header** — The stock bootloader prints verbose, cluttered output. This version shows only what matters:

```
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q128
Bootloader: V3.0 - 2026.09.10 - J. Nilo
```

**Download progress in %** — The stock bootloader prints endless `.` or `#` characters that flood the serial console during TFTP transfers. This version shows a clean percentage indicator:

```
Flashing: 76%
```

**Reboot to bootloader from Linux** — No need to press ESC on the serial console. A single command from Linux SSH writes a magic flag to RAM and reboots; the bootloader detects it and stops at the `<RealTek>` prompt, ready for TFTP. See [Reboot to Bootloader](doc/REBOOT_TO_BOOTLOADER.md) for details.

**Configurable download-mode IP** — The TFTP server IP defaults to `192.168.1.6` but is no longer hard-wired. `boothold <A.B.C.D> && reboot` hands the desired IP to the bootloader through DRAM, so `flash_remote.sh` makes the gateway come up on your `BOOT_IP` with no serial console — and `IPCONFIG` still sets it at the prompt. The compiled default remains the cold-boot fallback. See [Reboot to Bootloader](doc/REBOOT_TO_BOOTLOADER.md#optional-tftp-server-ip-handoff-bootloader-v27).

**Ping support** — The bootloader responds to ICMP Echo Requests. A simple `ping 192.168.1.6` confirms the board is alive and reachable before attempting a TFTP transfer.

**Post-flash notification** — After flashing, the bootloader sends a UDP packet (port 9999) to the TFTP client with `OK` or `FAIL`. This enables fully automated flashing without serial console confirmation — `flash_install_rtl8196e.sh` and `flash_remote.sh` use this.

**Risk-free testing** — The build generates a `test.bin` image that runs entirely from RAM without touching flash. Load it via TFTP, jump to it, and test your bootloader changes live — no risk of bricking. See the [Testing Guide](doc/TESTING.md) for the full workflow.

## Building

```bash
./build_bootloader.sh                              # Lidl (default)
BOARD=sengled-e39-g8c ./build_bootloader.sh        # Sengled Smart Hub G4
./build_bootloader.sh clean                        # clean
```

Outputs:
- `boot-img/<board>/boot.bin` — flash image (stays in download mode after
  boot-code flash). One pre-built image per board is committed; the build
  writes only into the slot of the selected `BOARD`, so building for one
  board never touches another board's binary.
- `btcode/build/test.bin` — RAM-test image (test without flashing)

Per-board constants (DRAM size and DDR bring-up values, boothold page
placement) live under `boards/` — see `boards/README.md` for the
contract and how to add a board. The build is reproducible: it regenerates
the committed `boot-img/<board>/boot.bin` bit-for-bit.

## Flashing

### Prerequisites

- Serial adapter connected (38400 8N1)
- Ethernet cable between PC and gateway
- PC on `192.168.1.x` (e.g. `192.168.1.1`)

### Step 1 — Enter download mode

**From Linux (recommended):**

```bash
boothold && reboot
```

The gateway reboots and stops at the `<RealTek>` prompt automatically. The flag is one-shot — the next reboot will boot Linux normally.

**From serial console:**

Power on the gateway and press **ESC** repeatedly until the `<RealTek>` prompt appears.

### Step 2 — Send the bootloader via TFTP

```bash
./flash_bootloader.sh                        # Lidl (default board)
BOARD=sengled-e39-g8c ./flash_bootloader.sh  # Sengled G4 pre-built image
```

The script checks ARP reachability, then uploads the pre-built
`boot-img/<board>/boot.bin` matching `BOARD` (default `lidl`). The
bootloader carries the board's DRAM bring-up — flashing another board's
image bricks the gateway, so double-check `BOARD` here.

Or manually:

```bash
# Lidl
tftp -m binary 192.168.1.6 -c put boot-img/lidl/boot.bin

# Sengled G4 — never substitute the Lidl image here
tftp -m binary 192.168.1.6 -c put boot-img/sengled-e39-g8c/boot.bin
```

The bootloader auto-detects the image type and flashes it. After flashing, reboot manually:
```
<RealTek>J BFC00000
```

### Flashing individual partitions

Same workflow — just send the image with the right Realtek header:

```bash
tftp -m binary 192.168.1.6 -c put ../33-Rootfs/rootfs.bin       # Will not reboot
tftp -m binary 192.168.1.6 -c put ../34-Userdata/userdata.bin   # Will not reboot
tftp -m binary 192.168.1.6 -c put ../32-Kernel/kernel-img/lidl/kernel-6.18.img
tftp -m binary 192.168.1.6 -c put ../32-Kernel/kernel-img/sengled-e39-g8c/kernel-6.18.img
```

The kernel causes a reboot after flashing. Rootfs and userdata do not. The
kernel image embeds the board's devicetree, so select the Sengled path on a G4.

The bootloader identifies each image by its header signature and writes it to the correct flash partition.

### Flashing a complete image (fullflash.bin)

The V2.5+ bootloader also auto-detects raw 16 MiB flash images (produced by
`build_fullflash.sh`). It verifies magic bytes at known partition offsets and
writes the entire image to flash:

```bash
tftp -m binary 192.168.1.6 -c put fullflash.bin  # Auto-flashes + reboots
```

This is what `flash_install_rtl8196e.sh` uses for automated installation.

## Safety

- **Never flash mtd0** without a backup and SPI programmer on hand
- The bootloader is the only recovery path if the device bricks (short of desoldering the flash chip)
- Always verify TFTP transfers completed before rebooting
- Use `test.bin` for testing — it runs from RAM without touching flash

## Documentation

| Document | Contents |
|----------|----------|
| [Command Reference](doc/COMMANDS.md) | All bootloader console commands (memory, TFTP, flash, PHY) |
| [Technical Memo](https://github.com/jnilo1/rtl8196e-gateway/blob/main/3-Main-SoC-Realtek-RTL8196E/31-Bootloader/doc/MEMO_BOOTLOADER.md) | Architecture, boot process, image format, flash layout, build system |
| [Toolchain Notes](https://github.com/jnilo1/rtl8196e-gateway/blob/main/3-Main-SoC-Realtek-RTL8196E/31-Bootloader/doc/BOOTLOADER_TOOLCHAIN_NOTES.md) | Porting post-mortem: RSDK to GCC 8.5 / musl |
| [Testing Guide](doc/TESTING.md) | RAM-test workflow, command validation checklist |
| [Reboot to Bootloader](doc/REBOOT_TO_BOOTLOADER.md) | Enter `<RealTek>` prompt from Linux without pressing ESC |
| [Reset Vector Audit](https://github.com/jnilo1/rtl8196e-gateway/blob/main/3-Main-SoC-Realtek-RTL8196E/31-Bootloader/doc/RESET_VECTOR_AUDIT.md) | Stage-1 DDR init analysis |
