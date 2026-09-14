# RTL8196E Bootloader Command Reference

## Overview

The bootloader provides an interactive serial console (38400 8N1) at the
`<RealTek>` prompt.  Commands are case-insensitive.  All numeric arguments
are hexadecimal unless noted otherwise.

The console is entered automatically when no valid firmware is found in
flash, or by holding **ESC** while the board boots (the key is sampled
while the kernel image is checksummed and once more before the jump; there
is no timed window).

A built-in TFTP server (IP `192.168.1.6`) listens for firmware uploads
in the background while the console is active.

---

## Memory commands

### DB - Dump bytes

Display memory contents as a hex byte dump.

    DB <address> <length>

- `address` — start address (hex)
- `length` — number of bytes to display (decimal)

```
<RealTek>DB 80000000 64
 [Addr]   .0 .1 .2 .3 .4 .5 .6 .7 .8 .9 .A .B .C .D .E .F
80000000: 08 04 00 14 00 00 00 00 B8 00 10 08 FF FF 05 C0    ................
80000010: B8 00 10 04 54 48 00 00 B8 00 10 50 D2 80 00 00    ....TH.....P....
80000020: B8 00 00 10 01 7F FD 2E B8 00 00 10 01 7F FD 2E    ................
80000030: B8 00 00 10 01 7F FD 2E B8 00 00 10 01 7F FD 2E    ................
```

### DW - Dump words

Display memory contents as 32-bit words (4 words per line).

    DW <address> <count>

- `address` — start address (hex, aligned to 4 bytes)
- `count` — number of lines to display (decimal, 4 words each)

```
<RealTek>DW B8000000 4
B8000000:   04060002    0400A200    00004425    00C00000
B8000010:   017FFD2E    06420804    00000000    00000000
B8000020:   00000000    00000000    00000000    00000000
B8000030:   00000000    00000000    00000000    00000000
```

### EB - Edit bytes

Write one or more byte values to consecutive memory addresses.

    EB <address> <value1> [value2] ...

```
<RealTek>EB 80500000 DE AD BE EF
```

### EW - Edit words

Write one or more 32-bit word values to consecutive addresses.
Address is aligned to a 4-byte boundary.

    EW <address> <value1> [value2] ...

```
<RealTek>EW 80500000 DEADBEEF 12345678
```

### CMP - Compare memory

Compare two memory regions word by word.  Reports any differences
found, or prints "No error found" if the regions match.

    CMP <dst> <src> <length>

- `dst` — first region address (hex)
- `src` — second region address (hex)
- `length` — number of bytes to compare (hex)

```
<RealTek>CMP 80000000 80500000 1000
No error found
```

---

## TFTP commands

The bootloader runs a TFTP server on IP `192.168.1.6`.  Files uploaded
via TFTP are stored at the load address and optionally auto-flashed
based on the image signature.

### LOADADDR - Set/show TFTP load address

Set the RAM address where incoming TFTP uploads are stored.
Without arguments, displays the current load address.

    LOADADDR [address]

```
<RealTek>LOADADDR
TFTP Load Addr: 0x80500000

<RealTek>LOADADDR 80000000
Set TFTP Load Addr 0x80000000
```

### AUTOBURN - Enable/disable auto-flash

When enabled (default), the TFTP server automatically validates and
flashes received images to SPI based on their signature header.
The `reboot` field in the signature table controls whether the board
reboots after a successful flash:

| Signature | Image type             | Auto-reboot (noreboot) | Auto-reboot (reboot) |
|-----------|------------------------|------------------------|----------------------|
| `cs6c`    | Linux kernel           | yes                    | yes                  |
| `cr6c`    | Linux kernel (root-fs) | yes                    | yes                  |
| `r6cr`    | Root filesystem        | no                     | no                   |
| `boot`    | Boot code              | yes                    | yes                  |
| `ALL1`    | Total image            | yes                    | yes                  |
| `ALL2`    | Total image (no check) | yes                    | yes                  |

Every image type auto-reboots after a successful write (`boot` included
since V2.5).  A write is reported as succeeded only after the range has been
read back and compared with the upload.

Without arguments, displays the current setting.

    AUTOBURN [0|1]

```
<RealTek>AUTOBURN
AutoBurning=1

<RealTek>AUTOBURN 0
AutoBurning=0
```

### IPCONFIG - Set/show server IP address

Set the TFTP server IP address of the bootloader.  Without arguments,
displays the current address (default: `192.168.1.6`).

    IPCONFIG [ip_address]

```
<RealTek>IPCONFIG
 Target Address=192.168.1.6

<RealTek>IPCONFIG 192.168.1.100
Now your Target IP is 192.168.1.100
```

The same server-IP value can also be set from Linux without the serial
console: `boothold <A.B.C.D> && reboot` hands the IP to the bootloader
through DRAM (V2.7+), and `IPCONFIG` shares the same internal variable.
See `REBOOT_TO_BOOTLOADER.md` ("Optional TFTP server-IP handoff").

### TFTP download (RRQ / `tftp get`)

The TFTP server also supports read requests (RRQ), allowing you to
download RAM contents to the host.  The server sends whatever data was
last loaded — either by a TFTP upload or by the `FLR` command.

**Workflow — export flash to host:**

```
<RealTek>FLR 80500000 0 10000
Flash Read Succeeded!
```

From the PC:

```bash
tftp -m binary 192.168.1.6 -c get flash_dump.bin
```

Serial console output:

```
**TFTP Server Download: 10000 bytes from 80500000
\
TFTP Download Complete!
<RealTek>
```

**Workflow — echo-back a TFTP upload:**

```bash
# Upload a file
tftp -m binary 192.168.1.6 -c put test_data.bin
# Download it back
tftp -m binary 192.168.1.6 -c get readback.bin
# Compare
cmp test_data.bin readback.bin
```

**Notes:**
- Data must be loaded first (`FLR` or TFTP upload). If no data is
  available, the server prints an error and ignores the RRQ.
- Only one transfer at a time.  A `get` during an active upload or
  download is rejected.

---

## Flash commands

### FLR - Flash read

Read data from SPI flash into RAM.  Prompts for confirmation.

    FLR <dst_ram> <src_flash> <length>

- `dst_ram` — destination RAM address (hex)
- `src_flash` — source flash offset (hex)
- `length` — number of bytes to read (hex)

```
<RealTek>FLR 80500000 0 10000
Flash read from 0 to 80500000 with 10000 bytes   ?
(Y)es , (N)o ? --> Y
Flash Read Succeeded!
```

### FLW - Flash write

Write data from RAM to SPI flash.  Prompts for confirmation.
**This erases the target flash sectors before writing.**

    FLW <dst_flash> <src_ram> <length>

- `dst_flash` — destination flash offset (hex)
- `src_ram` — source RAM address (hex)
- `length` — number of bytes to write (hex)

```
<RealTek>FLW 0 80500000 5600
Write 0x5600 Bytes to SPI flash, offset 0x0<0xbd000000>, from RAM 0x80500000 to 0x80505600
(Y)es, (N)o->Y
```

---

## Execution commands

### J - Jump to address

Transfer execution to the specified address.  Disables all interrupts
and PHY interfaces before jumping.

If the target address is `BFC00000` (flash reset vector), a watchdog
reset is triggered instead of a direct jump.

    J <address>

```
<RealTek>J 80000000
---Jump to address=80000000
```

**Common use:** load test.bin via TFTP, then jump to it:

```
<RealTek>AUTOBURN 0
<RealTek>LOADADDR 80100000
<RealTek>                          (upload test.bin via TFTP)
**TFTP Client Upload File Size = 525C Bytes at 80100000
Success!
<RealTek>J 80100000
```

**Reboot the board:**

```
<RealTek>J BFC00000
```

---

## PHY / MDIO commands

### PHYR - Read PHY register

Read a single register from an Ethernet PHY.

    PHYR <phyid> <register>

- `phyid` — PHY address, 0-4 (hex)
- `register` — register number (hex)

```
<RealTek>PHYR 0 2
PHYID=0x0 regID=0x2 data=0x001c
```

### PHYW - Write PHY register

Write a value to an Ethernet PHY register, then read it back.

    PHYW <phyid> <register> <data>

```
<RealTek>PHYW 0 0 3300
Write PHYID=0x0 regID=0x0 data=0x3300
Readback PHYID=0x0 regID=0x0 data=0x3300
```

### MDIOR - MDIO read (all PHYs)

Read a register from all 32 possible PHY addresses (0x00-0x1F).
Useful for scanning which PHY addresses are populated.

    MDIOR <register>

- `register` — register number (decimal)

```
<RealTek>MDIOR 2
PHYID=0x00 regID=0x02 data=0x001c
PHYID=0x01 regID=0x02 data=0x001c
...
PHYID=0x1f regID=0x02 data=0x0000
```

### MDIOW - MDIO write

Write a value to a specific PHY register via MDIO.

    MDIOW <phyid> <register> <data>

- `phyid` — PHY address (hex)
- `register` — register number (decimal)
- `data` — value to write (hex)

```
<RealTek>MDIOW 0 0 3300
Write PHYID=0x0 regID=0x0 data=0x3300
Readback PHYID=0x0 regID=0x0 data=0x3300
```

---

## Typical workflows

### Flash a new bootloader

```
<RealTek>AUTOBURN 1
<RealTek>                          (upload boot.bin via TFTP to 192.168.1.6)
**TFTP Client Upload, File Name: boot.bin
**TFTP Client Upload File Size = 55E2 Bytes at 80500000
Success!
Boot code upgrade.
checksum Ok !
Flash write: dst=0x0 src=0x80500010 len=0x55d2 (21970 bytes)
Flash Write Succeeded!
<RealTek>
```

### Test a bootloader from RAM before flashing

```
<RealTek>AUTOBURN 0
<RealTek>LOADADDR 80100000
<RealTek>                          (upload test.bin via TFTP)
**TFTP Client Upload File Size = 525C Bytes at 80100000
Success!
<RealTek>J 80100000
---Jump to address=80100000
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q128 (JEDEC c84018)
Bootloader: v2.1 - 2026.02.11-09:49+0100 - J. Nilo
---RAMTEST mode: skipping kernel boot
---Escape booting by user
<RealTek>
```

The test bootloader enters download mode instead of booting the kernel,
allowing you to verify commands and TFTP functionality.

### Flash a firmware image (kernel + rootfs)

```
<RealTek>AUTOBURN 1
<RealTek>                          (upload firmware.bin via TFTP)
**TFTP Client Upload, File Name: firmware.bin
**TFTP Client Upload File Size = F5000 Bytes at 80500000
Success!
Linux kernel upgrade.
checksum Ok !
Flash write: dst=0x30000 src=0x80500000 len=0xF5000 (1003520 bytes)
Flash Write Succeeded!
reboot.......
```

Firmware images with kernel signature auto-reboot after flashing.

### Read and inspect flash contents

```
<RealTek>FLR 80500000 0 100
Flash Read Succeeded!
<RealTek>DB 80500000 64
```

### Export flash contents to host via TFTP

```
<RealTek>FLR 80500000 0 10000
Flash Read Succeeded!
```

From the PC:

```bash
tftp -m binary 192.168.1.6 -c get flash_dump.bin
```

### Manual flash write (without AUTOBURN)

```
<RealTek>AUTOBURN 0
<RealTek>LOADADDR 80500000
<RealTek>                          (upload raw data via TFTP)
<RealTek>FLW 10000 80500000 5000
Write 0x5000 Bytes to SPI flash, offset 0x10000<0xbd010000>, from RAM 0x80500000 to 0x80505000
(Y)es, (N)o->Y
```
