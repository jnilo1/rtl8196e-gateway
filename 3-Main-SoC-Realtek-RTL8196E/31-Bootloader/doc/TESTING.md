# RTL8196E Bootloader Testing Guide

## Overview

The bootloader build produces two binaries:

| File                | Description                              | Build flag      |
|---------------------|------------------------------------------|-----------------|
| `boot.bin`          | Flash image (no reboot after boot TFTP)  | —               |
| `test.bin`          | RAM-testable bootloader (raw binary)     | `RAMTEST_TRACE` |

`test.bin` mirrors the bootloader behavior except it **skips kernel boot**
and enters download mode directly.  This allows testing all bootloader
functionality from RAM before committing to flash.

---

## Prerequisites

- Serial console: 38400 8N1
- Ethernet cable between PC and board (any port)
- PC network interface configured to `192.168.1.x` (e.g. `192.168.1.1`)
- TFTP client installed (`sudo apt install tftp-hpa`)

---

## Memory map constraints

```
0x80000000 - 0x80001000   MIPS exception vectors (refused as a load target)
0x80100000 - 0x80200000   Safe area for loading test.bin
0x80400000 - _end         Bootloader code/data/BSS (refused as a load target;
                          _end is in boot/build/boot.nm, 0x80424a40 for V3.1)
0x80500000 - ...          Default TFTP load address (AUTOBURN images)
top - 0x3000 .. top       Two reserved pages (boothold, watchdog record)
```

Since V3.1 `LOADADDR` refuses an address outside free RAM (below the first
page, inside the running loader, in the two reserved pages at the top of
DRAM), and an upload that would run into one of those regions is aborted
with a TFTP error instead of overwriting it.

---

## 1. Loading test.bin into RAM

This is the primary testing workflow.  It runs the new bootloader from
RAM without modifying flash.

### Step 1 — Enter download mode

Hold **ESC** while the board powers on (there is no timed window: the key
is sampled while the kernel image is checksummed and once more just before
the jump, so the key must already be on the line).  From a running Linux,
`boothold && reboot` does the same without a keyboard:

```
Booting...
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q128 (JEDEC c84018)
Bootloader: V3.1 - 2026.09.11-18:00+0200 - J. Nilo
---Escape booting by user

---Ethernet init Okay!
TFTP server IP: 192.168.1.6
<RealTek>
```

### Step 2 — Upload test.bin

```
<RealTek>AUTOBURN 0
AutoBurning=0
<RealTek>LOADADDR 80100000
Set TFTP Load Addr 0x80100000
```

From the PC (the `tftp` client sends the file by name without path — you must `cd` into the directory containing `test.bin`):

```bash
cd /path/to/31-Bootloader/btcode/build-ramtest
tftp -m binary 192.168.1.6 -c put test.bin
```

Expected output on serial console:

```
**TFTP Client Upload, File Name: test.bin
\
**TFTP Client Upload File Size = 5220 Bytes at 80100000

Success!
<RealTek>
```

### Step 3 — Execute test.bin

```
<RealTek>J 80100000
---Jump to address=80100000
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q128 (JEDEC c84018)
Bootloader: V3.1 - 2026.09.11-18:00+0200 - J. Nilo

---RAMTEST kernel copy: 1440082 bytes in 230 ms

---RAMTEST key check: during scan=0, at decision=0

---RAMTEST mode: skipping kernel boot

---Escape booting by user

---Ethernet init Okay!
TFTP server IP: 192.168.1.6
<RealTek>
```

Verify:
- Version and timestamp match the build (not the flash bootloader's)
- `RAMTEST kernel copy` and `RAMTEST mode: skipping kernel boot` are
  displayed: the image scan found the kernel through the memory-mapped
  flash window, copied it and validated its checksum — the read path works
- `RAMTEST key check` reports what the ESC poll saw (see §1 of the V3.0 notes
  in CHANGELOG.md for the paired-run procedure)
- Board enters `<RealTek>` prompt without booting the kernel

### Step 4 — Run tests

You are now running the new bootloader from RAM.  All commands are
available.  See sections below for specific test procedures.

**Note:** The serial console has no hardware flow control.  When
copy-pasting multiple commands, enter them one at a time and wait for
each prompt before sending the next.

---

## 2. Flashing the bootloader

Once test.bin is validated, flash the production bootloader.

### From the flash bootloader (ESC at boot)

```
<RealTek>AUTOBURN 1
AutoBurning=1
```

From the PC:

```bash
tftp -m binary 192.168.1.6 -c put boot.bin
```

Expected output:

```
**TFTP Client Upload, File Name: boot.bin
**TFTP Client Upload File Size = 55E2 Bytes at 80500000
Success!

Boot code upgrade.
checksum Ok !
Flash write: dst=0x0 src=0x80500010 len=0x55d2 (21970 bytes)
Flash Write Succeeded!
<RealTek>
reboot.......
```

Every image type auto-reboots after a successful write (the `reboot` field
of the signature table has been 1 for `boot` since V2.5).  `Flash Write
Succeeded!` means the range was read back and compared with the upload;
a mismatch prints `Flash verify FAILED at 0x...` and `Flash Write Failed!`,
and the UDP notification carries `FAIL`.

### From test.bin (running in RAM)

The same procedure works when running test.bin.  This is useful to
flash a bootloader while still having a safety net (the flash
bootloader is untouched until you explicitly flash).

---

## 3. Flashing a firmware image (kernel)

```
<RealTek>AUTOBURN 1
AutoBurning=1
```

From the PC:

```bash
tftp -m binary 192.168.1.6 -c put firmware.bin
```

Expected output:

```
**TFTP Client Upload, File Name: firmware.bin
**TFTP Client Upload File Size = F5000 Bytes at 80500000
Success!

Linux kernel upgrade.
checksum Ok !
Flash write: dst=0x30000 src=0x80500000 len=0xF5000 (1003520 bytes)
Flash Write Succeeded!
reboot.......
```

Kernel images (`cs6c`/`cr6c` signature) have `reboot=1` — the board
reboots automatically after flashing.

---

## 4. Command validation checklist

Test each command after code changes.  Commands are grouped by risk
level.

### Read-only commands (safe, no side effects)

| Command | Test | Expected |
|---------|------|----------|
| `?` | `?` | Help text listing all commands |
| `DB` | `DB 80000000 64` | Hex byte dump, 4 lines |
| `DW` | `DW B8000000 4` | Word dump, 4 lines of 4 words |
| `CMP` | `CMP 80000000 80000000 100` | `No error found` |
| `PHYR` | `PHYR 0 0` | `PHYID=0x0 regID=0x0 data=0x1100` |
| `MDIOR` | `MDIOR 0` | 32 lines, PHY 0-4 show data, rest 0x0000 |
| `LOADADDR` | `LOADADDR` | Shows current load address |
| `AUTOBURN` | `AUTOBURN` | Shows current setting |
| `IPCONFIG` | `IPCONFIG` | `Target Address=192.168.1.6` |

### Write commands (safe, reversible)

| Command | Test | Expected |
|---------|------|----------|
| `EW` | `EW 80500000 DEADBEEF` then `DW 80500000 1` | First word = `DEADBEEF` |
| `EB` | `EB 80500000 41 42 43 44` then `DB 80500000 4` | Bytes `41 42 43 44` |
| `PHYW` | `PHYW 0 0 1100` | Write + Readback, data=0x1100 |
| `MDIOW` | `MDIOW 0 0 1100` | Write + Readback, data=0x1100 |
| `IPCONFIG` | `IPCONFIG 192.168.1.100` then `IPCONFIG` | Shows new address |
| `AUTOBURN` | `AUTOBURN 0` then `AUTOBURN` | `AutoBurning=0` |
| `LOADADDR` | `LOADADDR 80200000` then `LOADADDR` | Shows `0x80200000` |

#### `boothold` DRAM IP handoff (V2.7+)

Unlike the console tests above, this exercises the warm-reboot path, so it
needs a running Linux on the gateway. From an SSH session:

```sh
boothold 192.168.0.6 && reboot
```

On the serial console, the bootloader should print `TFTP server IP:
192.168.0.6` as it enters download mode, and `IPCONFIG` should report
`Target Address=192.168.0.6` — confirming the IP crossed from Linux to the
bootloader through DRAM. Negative checks: a bare `boothold && reboot` (no
argument), or a **cold** power-cycle into download mode, must fall back to
`192.168.1.6`. See `REBOOT_TO_BOOTLOADER.md` for the handoff layout.

### Flash commands (destructive, use with caution)

| Command | Test | Expected |
|---------|------|----------|
| `FLR` | `FLR 80500000 0 100` | `Flash Read Succeeded!` |
| `FLW` | see note below | Prompts (Y)es/(N)o, writes to SPI |

**FLW test procedure** (safe round-trip, on a scratch range inside the
userdata partition rather than next to the bootloader):

```
FLR 80500000 F00000 10000     Read 64 KiB from flash offset 0xF00000
FLW F00000 80500000 10000     Write the same data back: block erase + program + verify
FLR 80600000 F00000 10000     Read it again
CMP 80500000 80600000 10000   Must print "No error found"
```

`FLW` reports `Flash Write Succeeded!` only after the read-back compare
passed.  On the RAM-test build the verify can be made to fail on purpose:
`EW <g_flash_verify_poison> 1` (address in `boot/build-ramtest/boot.nm`)
corrupts one byte of the next read-back, which must produce
`Flash verify FAILED at 0x...` followed by `Flash Write Failed!`.

### Execution commands

| Command | Test | Expected |
|---------|------|----------|
| `J` | `J BFC00000` | Board reboots (watchdog reset, switch quiesced first) |

### Fault paths (RAM-test build)

| Trigger | Expected |
|---------|----------|
| `CMP 80500001 80500001 4` | unaligned word read → `cp0_cause=...` then `FATAL: unhandled exception`, `Resetting...`, board resets into the flash bootloader within ~5 s |
| `DW <g_spurious_irq> 1` | counts interrupts taken with nothing pending; must stay 0 across a TFTP upload (address in `boot.nm`) |

### Stage-1 fault paths (decompressor, RAM-test build)

The decompressor in `test.bin` (`piggy.S` + `bootload.c`) reports a bad
embedded stream on the console and halts instead of returning.  Test it
from RAM with a deliberately corrupted copy of `test.bin` — never flash one:

```bash
OFF=$(printf '%d' 0x$(mips-lexra-linux-musl-nm btcode/build-ramtest/piggy.elf \
      | awk '$3=="__boot_start"{print $1}'))
OFF=$((OFF - 0x80100000))                      # 0x1100 for V3.1 (S12 + C1-b)
cp test.bin test-badprops.bin
printf '\xff' | dd of=test-badprops.bin bs=1 seek=$OFF conv=notrunc
cp test.bin test-badsize.bin
printf '\x01' | dd of=test-badsize.bin bs=1 seek=$((OFF + 9)) conv=notrunc
cp test.bin test-badlc.bin
printf '\x86' | dd of=test-badlc.bin bs=1 seek=$OFF conv=notrunc   # lc=8, lp=4: valid, 12 MiB of probabilities
```

| Image | Expected after `J 80100000` |
|-------|-----------------------------|
| `test-badprops.bin` | `LZMA: bad stream properties`, then nothing (halt) |
| `test-badsize.bin` | `LZMA: bad stream size`, then nothing (halt) |
| `test-badlc.bin` | `LZMA: workspace too large`, then nothing (halt) |

The halt is permanent: the hardware watchdog is not running in stage-1.
**Arm it from the prompt before the jump** so the board resets by itself
instead of needing a power-cycle:

```
<RealTek>EW B800311C 00A20000
<RealTek>J 80100000
```

`0xB800311C` is `WDTCNR`; the value enables the watchdog with `WDTCLR` set
(`WDTE` field 0 = running).  Measured on the bench: the reset follows within
about two seconds, which is plenty — the message is printed microseconds
after the jump.  Use this only for a hang that happens **before** download
mode is reached (a stage-1 fault, the LZMA halts above, an early stage-2
crash): nothing has armed the watchdog yet at that point.

Once the RAM-test build reaches download mode it **arms the watchdog itself
and kicks it from the main loop** (`boot/include/ramtest_trace.h`, compiled
in only under `RAMTEST_TRACE`; OVSEL 9, armed in `goToDownMode()`, kicked in
`eth_poll()` at entry and after every packet, never from the interrupt
handler).  A hang of the main loop after that point — a wedged receive path,
an interrupt storm — therefore self-resets in a couple of seconds without the
manual arm above; a healthy transfer, however long, is kicked continuously
and never reset.  The breadcrumb page at `0xA1FFD800` (the upper half of the
watchdog crash-record page, magic `RTMB`) survives the reset: read it back
from the flashed loader with `DW A1FFD800 20` to see how far the hung run got
(ISR count, run-out count, TFTP block and phase, any exception cause).

The stage-1 DRAM message (`DDR calibration: no DQS window`) lives in
`start.S` and cannot be exercised from RAM.  Do **not** provoke it with a
build that alters `DDR_TEST_EXPECT`: that build applies DQS centre 16, and
the bench box's real centre is 10 (`devmem 0x18001050` under Linux) — a
centre outside the board's window bricks a Lidl.  The safe test build turns
`bnez a2, DDCR_SHIFT_EXIT` into `bnez zero, DDCR_SHIFT_EXIT` (message printed,
real centre applied).

### Raw fullflash fault paths (flashed loader, nothing written)

A raw 16 MiB image carries a CRC trailer at `0x1FFF0` (`lib/fullflash_crc.sh`);
the loader refuses a mismatching or damaged trailer after the kernel checksum
and before the first erase, so a refused upload is a safe test.  Build the
images on the host, upload them at the `<RealTek>` prompt of the **flashed**
loader with `AUTOBURN 1` and `LOADADDR 80500000`:

```bash
NET_MODE=static IPADDR=192.168.1.88 NETMASK=255.255.255.0 GATEWAY=192.168.1.1 \
    ./build_fullflash.sh          # writes the trailer last
cp fullflash.bin ff-badrootfs.bin
printf '\x01' | dd of=ff-badrootfs.bin bs=1 seek=$((0x300000)) conv=notrunc   # flips one rootfs bit
cp fullflash.bin ff-badtrailer.bin
printf '\x00' | dd of=ff-badtrailer.bin bs=1 seek=$((0x1FFF3)) conv=notrunc   # damages the trailer
```

| Image | Expected after the upload |
|-------|---------------------------|
| `ff-badrootfs.bin` | `kernel checksum Ok !` then `fullflash CRC error: image …, trailer …`, `FAIL` on UDP:9999, prompt back |
| `ff-badtrailer.bin` | `unrecognised data at 0x1fff0: not a CRC trailer`, `FAIL`, prompt back |
| `fullflash.bin` | `fullflash CRC Ok ! (…, 770 ms)` then the two verified writes — this one flashes the box |

These run from RAM too: a 16 MiB upload into a stage-2 loaded at
`0x80100000` and jumped to now completes like any other (validated four
times on the bench, `AUTOBURN 0`).  An earlier build hung on it — a
descriptor run-out storm the RAM-test path exposed through its slow
`putchar` and the client's retransmits, which starved the main loop; the
flashed loader took the same upload in 26 s only because it rarely reached
that state.  The loader no longer arms the run-out interrupt, masks it
defensively in the ISR, and resyncs the rings from the main loop when the
run-out stays asserted, so the storm cannot form; a run-out is now absorbed
by the normal receive path (`swNic_receive` / `eth_poll`, counters
`g_rx_runout` and `g_rx_resync`).

---

## 5. TFTP server validation

The TFTP server runs in the background while the console is active.

### Upload test

```bash
# From PC
tftp -m binary 192.168.1.6 -c put test.bin
```

Verify on serial console:
- File name displayed
- File size matches
- `Success!` message

### AUTOBURN test

Upload images with known signatures and verify:

| Image | Signature | Expected behavior |
|-------|-----------|-------------------|
| `boot.bin` | `boot` | Flash to offset 0, no reboot |
| `firmware.bin` | `cs6c` | Flash to kernel offset, auto-reboot |

### Malformed packets (V3.1 length checks)

`tests/tftp_probe.py` sends frames whose UDP/IP length fields lie about the
payload (needs raw sockets, so `sudo`; run it from a real terminal, the
password prompt needs one).  Type `AUTOBURN 0` on the console first.  After
each probe the board must still answer `ping`, and the two probes sent
inside an open upload must get the expected answer: one byte over the block
is refused with TFTP error 4 and the upload aborted, a UDP length of 11 is
dropped and the upload then completes.  A normal 4-block upload ends the
run, and no transfer is left open.  The IP-length probe is sent as a raw
Ethernet frame, since the kernel rewrites the IP length of a raw IP socket.

```bash
sudo ./tests/tftp_probe.py 192.168.1.6
```

Bench (Lidl `.88`, flashed V3.2, host on Wi-Fi, 2026-09-26): all six lines
`board alive` with the expected outcome, `PASS`; the console printed
`TFTP DATA with bad length 525, aborting`, then the 3-byte and 0x604-byte
uploads with `Success!`.

### Oversized upload

With `LOADADDR 80500000`, uploading a file larger than the free RAM above
it (27 MiB on the Lidl board) must stop with a TFTP "disk full" error on
the client and `Upload does not fit at ...` on the console, and the board
must still answer `ping`.

### Abandoned transfer recovery (V3.2)

A transfer belongs to the client that opened it (MAC, IPv4 address, UDP
port).  A client that goes silent (a stalled or interrupted `tftp`, a
Ctrl-C) used to leave the server ignoring every other port until a power
cycle.  Since V3.2 a request that arrives while the transfer has been idle
for 15 s, with no DATA or ACK, drops it: the console prints
`TFTP: idle transfer dropped` and the request is served as from idle.  A
dropped download keeps serving the data still in RAM; a dropped upload is
partial and is forgotten.

`tests/tftp_idle.py` checks it with raw TFTP over an ordinary UDP socket
(no root).  It sends one request per second while it waits, so a lost
packet on a Wi-Fi host costs a second, not the test.  Run it from
`test.bin` after `AUTOBURN 0`: every upload stays in RAM, nothing is
written to flash.

```
<RealTek>AUTOBURN 0
```

```bash
./tests/tftp_idle.py 192.168.1.6 upload-stall 50   # client dies after 50 blocks
./tests/tftp_idle.py 192.168.1.6 upload-stall 0    # client dies right after its WRQ
./tests/tftp_idle.py 192.168.1.6 live-slow 2 30    # alive, one block every 2 s
./tests/tftp_idle.py 192.168.1.6 live-slow 10 45   # alive, one block every 10 s
./tests/tftp_idle.py 192.168.1.6 download-stall    # needs data loaded (last upload)
./tests/tftp_idle.py 192.168.1.6 peer-retx         # WRQ retransmit before block 1
```

Each line prints `PASS` or `FAIL`.  In the stall tests the new client stays
silent for 16 s, then its **first** request must be served: the request
that makes the loader drop the stale transfer is the one a single-shot
client such as `tftp` (one WRQ per 5 s) depends on.  A client retrying every
second would hide a loader that drops the transfer but loses that request.

Bench reference (Lidl `.88`, host on Wi-Fi, 2026-09-26, from `test.bin` and
again from the flashed V3.2): both stalled uploads answered on the first
WRQ 16.0 s after the stall, the stalled download on the first RRQ 16.0 s
after it and re-served in full (0xA00 bytes, as the console reported), the
live uploads were never taken over by an intruder sending a WRQ every
0.5 s, and the retransmitting client was served.  `upload-stall 0` is the
case a WRQ from another port used to keep alive: those WRQs now leave the
idle clock alone.

### Post-flash UDP notification test

After each flash with `AUTOBURN 1`, the bootloader sends a UDP packet
(port 9999) to the TFTP client with `OK` or `FAIL`.

```bash
# Terminal 1: start listener
nc -u -l -p 9999

# Terminal 2: send firmware
tftp -m binary 192.168.1.6 -c put kernel-img/lidl/kernel-6.18.img
```

Terminal 1 should display `OK` after the flash completes.

**Error case** — send a random file to trigger FAIL:

```bash
# Terminal 1: start listener
nc -u -l -p 9999

# Terminal 2: send garbage
dd if=/dev/urandom of=/tmp/bad.bin bs=1024 count=4
tftp -m binary 192.168.1.6 -c put /tmp/bad.bin
```

Terminal 1 should display `FAIL` (no valid signature found).

---

## 6. Troubleshooting

### TFTP transfer hangs

- Check IP address: default is `192.168.1.6`, reset after every reboot
- Verify the board is at the `<RealTek>` prompt (not booting kernel)
- Check PC network: `ping 192.168.1.6` should succeed

### Board reboots unexpectedly after TFTP upload

- Verify `AUTOBURN 0` if you don't want auto-flashing
- Never use `LOADADDR 80000000` — overwrites exception vectors

### test.bin shows wrong timestamp

- Verify you uploaded the freshly-built `test.bin`, not a stale copy
- Check build output: `make` prints the payload size — compare with
  the TFTP upload size on the serial console

### Checksum error on flash

- All images generated by `cvimg` use 16-bit checksums
- If checksum fails, the image file may be corrupted — rebuild and
  re-upload

### No boot log after J command

- Ensure `LOADADDR` was set to `80100000` (not `80000000`)
- Ensure test.bin was built with `RAMTEST_TRACE` (check for
  `---RAMTEST mode` in output); it lives in `btcode/build-ramtest/`,
  `btcode/build/boot.bin` is the flash image

---

## 7. Quick reference

```bash
# === Load and run test.bin ===
# On serial console:
AUTOBURN 0
LOADADDR 80100000
# On PC (must cd into the directory first):
cd /path/to/31-Bootloader/btcode/build-ramtest
tftp -m binary 192.168.1.6 -c put test.bin
# On serial console:
J 80100000

# === Flash bootloader ===
# On serial console:
AUTOBURN 1
# On PC:
tftp -m binary 192.168.1.6 -c put boot.bin
# (board reboots automatically after "Flash Write Succeeded!")

# === Flash firmware ===
# On serial console:
AUTOBURN 1
# On PC:
tftp -m binary 192.168.1.6 -c put firmware.bin
# (board reboots automatically)

```

---

## 8. Host-side unit tests

The bound checks shared by the boot path, the TFTP receiver and the
auto-flash path (`boot/include/checks.h`) are pure functions and are tested
natively, including the negative cases that must never be staged on a board
(a kernel header pointing into the loader, an upload wrapping over DRAM):

```bash
./tests/run_host_tests.sh
```
