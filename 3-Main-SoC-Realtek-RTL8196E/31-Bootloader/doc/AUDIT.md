# Bootloader code audit — efficiency, clarity, security

Audit of the RTL8196E bootloader sources under `31-Bootloader/` (stage-1 `btcode/`,
stage-2 `boot/`, boards, build and flash scripts, docs). Performed 2026-09-11 against
bootloader **V3.0** (`boot/include/ver.h`) as committed on local `main` (`e726439`,
"make the Esc escape into download mode reliable", shipped in platform release 4.4.0;
`main` tip `32157d9` at the time of writing). Line numbers below refer to that tree.

Provenance note: the first pass was read on the checked-out branch `kernel/7.2-imem`,
which still carries V2.9 because it does not contain `e726439`. The V2.9 → V3.0 diff was
then applied to the audit: it touches only `boot/main.c`, `boot/include/main.h`, a
comment in `boot/uart.c`, the version string, the pinned build timestamp and the two
`boot-img/*/boot.bin`; `btcode/`, `boot/net/`, `flash.c`, `monitor.c`, the switch driver,
the scripts and the docs are byte-identical between the two. Findings that V3.0 already
resolved are marked **[fixed in V3.0]** rather than deleted, so the record stays usable
when reading older field reports.

**No code was changed by this audit.** This document is a brief for a later session that
decides to implement all or part of the recommendations. Every finding carries an ID,
the exact location, the evidence, the recommended change, the validation path and the
brick risk, so that work can be picked up item by item.

---

## 0. Implementation status (2026-09-11, bootloader V3.1 in source)

Rule applied: everything that `test.bin` can exercise from RAM was implemented; anything
whose validation needs a flash write of the bootloader itself (all of `btcode/`) was left
out. Line numbers in the sections below still refer to the V3.0 tree.

| ID | Status | Where / how validated |
|---|---|---|
| S1 | **done** | `checks.h` (`udp_frame_lens_ok`, `tftp_udp_len_ok`, `ram_window_ok`), `kick_tftpd()`, `prepareACK()` sends TFTP ERROR and resets, `LOADADDR` refuses; host unit test + bench (`LOADADDR` refusals, 100 KiB upload). The raw-packet probe `tests/tftp_probe.py` (root) passed against the flashed loader. |
| S2 | **done** | `flash.c`: every sector/block read back after programming, `spi_flw_image()` returns 1/0, callers report `FAIL`; bench: FLW round trips + injected mismatch (`g_flash_verify_poison`, RAM-test build) reported as failed write. |
| S3.1 | **done** | `flash_raw_fullflash()`: kernel `cs6c` checksum verified in the buffer, then `0x20000..end`, then `0..0x20000`. Bench (flashed V3.1, `flash_install_rtl8196e.sh -y 192.168.1.88`, 2026-09-11): `kernel checksum Ok !`, `dst=0x20000 len=0xfe0000` verified, then `dst=0x0 len=0x20000` verified, automatic reboot, clean boot with the preserved configuration. |
| S3.2 | **done** | 16-byte CRC trailer at `0x1FFF0` (last bytes of the bootloader partition: `FFCS`, length, CRC-32 of the image minus the trailer, complement), written by `build_fullflash.sh` / `create_fullflash.sh` and rewritten by `backup_gateway.sh` (`lib/fullflash_crc.sh`); `flash_raw_fullflash()` checks it after the kernel checksum: all-`0xFF` = no trailer, accepted as before; mismatch or foreign bytes = refused before any write. Parser in `checks.h`, `crc32.c`, host tests. Bench (Lidl `.88`, 2026-09-12, from the flashed loader): rootfs byte flipped → `fullflash CRC error: image 68d48fcc, trailer ce8bf192`; trailer byte flipped → `unrecognised data at 0x1fff0: not a CRC trailer`; real fullflash through `flash_install_rtl8196e.sh` → `fullflash CRC Ok ! (a18c9dc0, 770 ms)` then the two verified writes and a normal boot; backup then `restore_gateway.sh` → `CRC trailer OK`, a corrupted copy refused on the host. Design record: `MEMO_S32_FULLFLASH_CRC.md` (kept with the bench archive). |
| S4 | **done** | `autoflash_header_ok()`; chip-1 branch and `0xdeadc0de` block deleted; host unit test. |
| S5 | **done** | `kernel_header_ok()` in `check_system_image()`; negative cases in the host unit test only (never staged on a board); positive path on the bench (kernel found and summed from RAM). |
| S6 | **done** | ISR acks only; `eth_poll()` from the main loop via the `g_uart_idle` hook of `serial_inc()`; timer re-armed by `goToDownMode()`; GIMR hack in the MDIO read and the blind loop in `autoreboot()` removed. Found and fixed on the bench: the RUNOUT ack order in `swNic_receive()` raised a transient line once it ran with interrupts enabled (spurious interrupt); spurious interrupts are now counted (`g_spurious_irq`) and ignored. **Second bench finding (2026-09-13): a descriptor-run-out interrupt storm.** With the ISR acking only, `swNic_receive()` used to arm `PKTHDR_DESC_RUNOUT_IE_ALL` and nothing disarmed it; the run-out status is level, so once the 4-descriptor RX ring emptied faster than the main loop refilled it, the acked line re-asserted at once and the ISR re-entered forever, starving the very loop that returns descriptors — no prompt, no ICMP, no ACK. Only the RAM-test path exposed it (its slow `putchar` plus the client's retransmits made run-out routine; the flashed loader, 16 MiB in 26 s, rarely reached it). Fix, mirroring the kernel driver: never arm the run-out mask, mask it defensively in the ISR, and resync both rings (`swNic_rx_resync()`) from `eth_poll()` when run-out stays asserted across idle polls; a plain run-out is absorbed by the normal receive path. Counters `g_rx_runout`/`g_rx_resync`. Validated: 16 MiB from RAM 4/4 (`AUTOBURN 0`), ~67 run-outs absorbed with no resync; then flashed to the Lidl bench and a real `flash_install_rtl8196e.sh` full-flash booted clean. RAM-test build also gained a main-loop watchdog kick + a breadcrumb page (`boot/include/ramtest_trace.h`, `RAMTEST_TRACE` only) so a starved loop self-resets and leaves a record. |
| S7 | **done** | one buffer per TX descriptor in `swNic.c`, bounded wait for a free descriptor, `swNic_wait_tx_idle()`. |
| S8 | **done** | `read_setting_block()`, `gethwmac()`, `getmacandip()` and the flash offsets deleted; fixed MAC. |
| S9 | **done** | echo copy bounded by `nic.packetlen`; RRQ kept as a feature and documented in `tftpd.c`. |
| S10 | **done** | stray store deleted. |
| S11 | **done** | JEDEC ID printed in the banner (`c84018` on the bench); writes refused unless the capacity byte is `0x18`; agent notes corrected. |
| S12 | **done** | stage-1 failures reported on the console: `DDR calibration: no DQS window` when the DQS sweep finds no passing delay (then the historical centre is applied — print-and-continue, not a halt: a false positive would brick a Lidl), `LZMA: bad stream size / properties / decode failed` then a halt; explicit `nop` in the `jal boot_entry` delay slot + `piggy_halt`.. Bench (Lidl `.88`, 2026-09-11): RAM negative tests on the two LZMA header paths, real image flashed and booted, DRAM message path proven in flash with a one-word test build (`bnez zero`), real image reflashed. The `DDR_TEST_EXPECT` test build was rejected: the board's real DQS centre is 10, not the forced 16. |
| S13 | **done** | `fatal()` (print, pause, clear HOLD, watchdog); used by `do_IRQ`, `do_reserved`, `malloc`, `ASSERT_CSP`, the switch table paths. Found on the bench: `do_reserved()` dereferenced `regs`, which `exception_matrix` never fills (no frame is saved), so any real exception hung the board silently in a nested fault — inherited, never exercised before; it now reads CP0 only. Bench: `CMP 80500001 80500001 4` → `cp0_cause=10, epc, badvaddr` → `FATAL: unhandled exception` → watchdog reset → normal Linux boot. |
| S14 | **done** | build with `-Wall -Wextra` + `-Werror=` promotions at zero warnings; return types, `swTable.c` parentheses, uninitialised `prev`, fall-through comment fixed; `-fno-strict-aliasing -ffreestanding`. |
| E1 | **done** | `flash_write_range()`: 64 KiB block erase when aligned and a whole block remains. Bench: kernel 1.44 MiB erased + programmed + verified in 8.7 s (the audit estimated ~21 s at sector granularity, before verification). |
| E2 | **done** | calibration removed, banner prints the platform constant. |
| E3 | **done** (decided: keep the PIO copy) | RAM-test build prints the copy time: 1,440,114 bytes in 230 ms (PIO fast read, 6.3 MB/s). Measured 2026-09-12 from RAM: the same copy through the memory-mapped window (`0xBD000000`, 32-bit reads, checksum identical) takes **420 ms** — every uncached word is a full SPI command sequence — so the window is not a faster path. The SPI clock is already 100 MHz: `setFSCR(84)` divides the 200 MHz bus by 2 because `CheckDramFreq()` (an RTL8196D table) returns 156 instead of 200; the GD25Q127C is rated 104 MHz for single/dual fast read, and both boards have booted this way since V2.x. Single-IO at that clock tops out at 12.5 MB/s, so the only remaining lever is a dual-output read (`0x3B`), worth at most ~80 ms of a 15 s boot. Not worth a new read mode in the boot path; left as is. |
| E4 | **done** (decided: keep the delay) | Tried on the bench 2026-09-12 from RAM: trusting the `MDCIOSR` busy bit as the write path does (poll, short spin, read) brings `eth_startup()` from 310 ms to 10 ms and the first 38 PHY registers read identical to the reference — then the read of PHY 1 register 6 never completes and the loader hangs in the poll (no console, no ICMP; power-cycle). The SDK's "8196C test chip patch" is a real hardware workaround, not cruft: polling the status register right after a read command can wedge the controller. Shortening the sleep is not possible either, `delay_ms()` resolves to the 10 ms timer tick. 200 ms on the recovery path only; left as is, with this record so nobody retries it blind. |
| E5 | **done** (decided: leave the rest) | `nicreg[]`/`eth.h` deleted in V3.1. Measured 2026-09-12: the last `.text` object ends at `0x804095d0` and the section is padded to `0x8040c000` for the I-MEM fill — 10.8 KiB of zeros in a 90 KiB stage-2, which LZMA compresses to nothing in flash and which cost RAM (32 MiB available) and microseconds of decompression. Removing the `ALIGN(0x4000)` means touching the `head.S` I-MEM fill, whose only validation is the boot itself; no payoff to justify it. Left as is. |
| E6 | **done** | `boot_entry()` refuses a stream whose probability array (`LzmaGetNumProbs × sizeof(CProb)`, 32 KiB for the lc=3/lp=0 the build uses) would not fit in the 1 MiB between `LZMA_STATUS_ADDR` and the decompression target: `LZMA: workspace too large`, then the S12 halt. Bench (Lidl `.88`, 2026-09-12): RAM test nominal, then properties byte forced to `0x86` (lc=8/lp=4) → message, watchdog reset; flashed, booted, `mtd0` read back. |
| E7 | **done** | one static `struct tftp_t` transmit frame. |
| E8 | **done** | `autoreboot()` waits for the TX descriptors, then `delay_ms(20)`. |
| C1 stage-2 | **done** | `SettingCPUClk` & co, `CmdWriteHword`, `loops_per_sec`, watchpoint handler, NULL initialiser, dead flash wrappers, `spi_flash_info[2]`, `ucSFCR2` side effect (now `spi_enable_mmap_read()`), `eth.h`, etherboot leftovers, `ETH_BROADCAST`, `jump`, client IP typo, `arpInput`/`arpResolve`, `#pragma ghs`, `swTable_forceAddEntry`, `rtl8651_setAsicFlowControlRegister`, the `P0phymode` print, `check_rootfs_image` + `SKIP_ROOTFS_SCAN`, `HS/DS/CS_IMAGE_OFFSET`, duplicated `HW_SETTING_OFFSET`, `user_interrupt(time)`, `WAIT_TIME_USER_INTERRUPT`, `strstr`, `SprintF`, `init_arch` arguments. `head.S` CU3/IRAM/`gp` left (see E5). |
| C1 stage-1 | **done** | C1-b **done** (2026-09-12): the dead `regdata` table and the `j 1f` that skipped it deleted from `piggy.S`, `__start` falls straight into the stack setup; the only stage-1 code change is the payload end address of the copy loop. Bench (Lidl `.88`): RAM test from `test.bin`, then flashed and booted (`mtd0` read back). C1-a **done** (2026-09-12): the MCM DDR1 selector deleted from `start.S` with its two constants — its `0x0AC8` write was unreachable, the status read and the compares did run; same register writes in the same order, `piggy.bin`/`test.bin` byte-identical. Flash-only validation on the Lidl bench, taken as an accepted exception (no G4 at hand): flashed through `flash_remote.sh`, then five mains power-cycles with the console captured and `CLKMGR` = `0x0B08`, `DDCR` = `0xD4A00000`, `mtd0` hash read back after each (three further cold boots console-only, all clean). Leftovers **done** (2026-09-12): `start.S` comments (DDR values from `board.h`, no strap read; the `__boot_end + 4` bound; DQS1 never swept), the dead `start.h` macros (`ADD3VAL`, `UART_WRITE`, `UART_PRINT_DELAY`, `UART_BIN2HEX`, `VIR2PHY`, `SRAM_BASE`/`SRAM_TOP`) and the `btcode/Makefile` header — both board images byte-identical before and after, so no flash. |
| C2 | **done** | `J` given the full quiesce (`swCore_quiesce()` shared by three callers); HOLD-page rationale rewritten (no stage-1 stack); `autoreboot()`, `swNic_send()`, flash write comments, `boot/Makefile` header, `uart.c` timeout comment, `.gitignore`. |
| C3 | **done** | warning flags; `build-ramtest/` object directories (the `btcode/build/boot.bin` trap is gone); `ver.h` carries `B_VERSION` and the pinned `BOOT_CODE_TIME` (Makefile override kept). Recipe deduplicated (2026-09-12): the top-level `Makefile` only forwards `all`/`clean` to `build_bootloader.sh`, which is the one recipe (`make` and the script build the same `boot.bin`, md5-checked on both boards). The `$(HOME)` fallbacks in `btcode/Makefile` and the Docker path in the script stay. |
| C4 | **done** | `timer.c` split out; `main.h` no longer defines globals; `fatal.c`; magic numbers `WDTCNR_REG`, `CPUIISR`, `SYS_CLKMANAGE`; `asm/branch.h`, `asm/inst.h`, `asm/rtl8196x.h` removed. 2026-09-12, header pruning: (1) 12 files whose emptying left the stage-2 binary unchanged deleted with their `#include` lines (`asm/bootinfo.h`, `cachectl.h`, `cpu.h`, `current.h`, `hw_irq.h`, `isadep.h`, `softirq.h`, `limits.h`, `linux/byteorder/generic.h`, `linux/config.h`, `rtl_depend.h`, `stddef.h`); (2) inside the `asm/` and `linux/` headers, every paragraph whose removal kept the binary identical removed — 2,344 lines; (3) SoC layering: `asm/rtl_soc.h` (a two-line shim) folded into `boot_soc.h` and `uart.h`, `asm/rtl8181.h` dropped (its only user, `inthandler.S`, takes `asm/rtl8196.h`, which defines the same `GIMR0`/`GISR`), so one SoC header remains (`asm/rtl8196.h`) beside the switch registers (`rtl8196x/asicregs.h`). 84 → 70 files, 11,867 → 8,640 lines. Proof: both board images and both variants byte-identical, zero warnings (the byte criterion alone let `asm/sgidefs.h` be emptied — every `#if _MIPS_ISA` branch then defined the same macros and the last one won; the 100 redefinition warnings caught it, the header stays), host tests, and a review of every preprocessed line that changed form (46 distinct: `htons(x)` → `(x)` where the `__u16` cast was redundant, `ULONG_MAX` re-spelled, generic prototypes replacing unused inlines). Phase 2 (2026-09-12, seven commits): the inherited `asm/` and `linux/` directories are gone — 40 files / 4,151 lines — and the loader's own headers carry what it used: `boot_asm.h` (CPU: CP0 numbers and Status bits, the CCTL operations, the o32 names, entry macros and the exception frame — also included by stage-1's `start.S` and `piggy.S`), `boot_soc.h` (register map, asm-safe, plus the C accessors), `boot_irq.h` (`cli`/`sti`, `irqaction`, `request_IRQ`), `boot_common.h` (gcc's `<stdint.h>`/`<stddef.h>`), `stdlib.h` (what `libc.c`/`calloc.c` export, from `nm --defined-only`), `rtl_types.h` on the `_t` types, byte order as four identities in `etherboot.h`. 70 → 30 files, 8,640 → 5,087 lines, none inherited; `-D__KERNEL__ -Dlinux -D__ASSEMBLY__` dropped (nothing read them), `-Wno-ignored-qualifiers` dropped (its only trigger was a `volatile` return type), `build_bootloader.sh` checks the `-MD` dependency files of all 44 units for a sysroot path (there is none: only `lib/gcc/`'s `stdint.h`, `stdint-gcc.h`, `stddef.h`, `stdarg.h`). Proof at every step: both board images and both RAM-test images byte-identical, zero warnings, host tests, and the preprocessed review (`tests/preproc_diff.py`, cumulative: 12,362 lines removed, 440 moved by include order, 2,194 added in 109 distinct texts — gcc's `stdint`/`stddef` typedefs, the `stdlib.h` prototypes and `rtl_types.h` aliases now reaching every unit, the `inl`/`outl` and `cli`/`sti` inlines under their own names, `LONG_MAX` & co re-spelled from `__LONG_MAX__`, five one-token re-spellings in `head.S`, `inthandler.S` and the CP0 write macro). Three re-spellings were first left because they change the image: `inl`/`outl` as `rtl_inl`/`rtl_outl` (gcc then stores `$zero` directly at three sites), `strcmp` as C in `libc.c` (a call instead of the inline), `NR_IRQS` 64 → 32 (BSS layout). Done the same day once the byte-identity constraint was lifted for stage-2: the four GIMR sites use `REG32(GIMR_REG)` like `timer.c`, `strcmp` is a C function in `libc.c`, `NR_IRQS` is 32 (text unchanged at 53,640 bytes, BSS −128, `_end` 0x80424a40). Bench (Lidl `.88`, RAM, 2026-09-12): `test.bin` loaded and started, command matching (exact, near-miss refused, `HELP`), ping, a 20 KiB TFTP upload read back byte-exact at both ends, GIMR 0x8100 (timer + switch unmasked by the new path), `J BFC00000` back to the flash loader and Linux up. Two bench notes for next time: the upload size is printed in hex (`%X`), and a watchdog armed before `J` fires during the RAM loader's banner — nothing in stage-2 disarms it; arm it only to escape a halt. Network headers (2026-09-12, same criterion): the eleven Realtek/Etherboot files (`rtl8196x/asicregs.h` 2,046 lines, `swCore.h`, `vlanTable.h`, `swNic_poll.h`, `swTable.h`, `loader.h`, `etherboot.h`, `nic.h`, `eth_api.h`, `tftp.h`, `boot_net.h` — 3,516 lines) become three: `swcore_regs.h` (the switch registers actually addressed, field bits under their register), `swcore.h` (driver API and the two table entry layouts), `boot_net.h` (frame formats, `struct nic`, eth/TFTP API) — 504 lines. Method: every `#define` with no reference anywhere in the corpus removed in one go (1,048), then paragraph-level pruning with a build per trial, then the survivors rewritten by hand from the per-file use census; `REG32`, the absolute UART registers and the bond option move to `boot_soc.h`. Images byte-identical, zero warnings, preprocessed review: register bases inlined, prototypes reaching the units that include the merged header. Last pass, the loader's own headers by the same name-by-name census: `rtl_errno.h` down to the four codes returned (`EEXIST`, `EINVAL`, `ERANGE`, `ECOLLISION`), `boot_soc.h` without the twenty registers nothing addresses, the unused o32 names, bit macros, ring-size constants and two stray constants gone; `boot/include` is 22 files / 1,860 lines (8,640 in the morning), every name defined there used, images byte-identical. Then regrouped by subject: the flash layout joins `spi_flash.h` and the internals of `flash.c` (`spi_common.h`) go back into `flash.c` (external linkage kept: `static` changes the inlining and the image); `cache.h`, `crc32.h` and `rtk.h` fold into `main.h` (the `crc32` prototype into `checks.h`, host-testable); `ctype.h` into `stdlib.h`, `rtl_errno.h` into `rtl_types.h`. 15 files / 1,774 lines; `ver.h` stays alone on purpose (bumped at release, read by `btcode/bootload.c`). |
| C5 | **done** | `dispatch_event()` as a switch on the event with the three `bootState` tests where the table's rows differed — cell for cell the former 3 × 8 table, `kick_tftpd()` (lock, 20 s WRQ retransmit window, `SERVER_port++`) untouched; stage-2 16 bytes smaller. Bench regression (Lidl `.88`, 2026-09-12), from RAM: ARP/ICMP; two uploads back to back; client killed mid-transfer then a fresh `put` accepted after the 20 s window (27 s, `Success!`); `FLR` + `tftp get` of 64 KiB identical to the flash; DATA/ERROR/OACK from idle → `Boot state error: 0` each, ACK from idle → nothing (filtered before dispatch), ERROR during an upload → transfer aborted, loader alive — every one of these identical on the previous stage-2 loaded the same way. Then flashed: `flash_remote.sh bootloader`, kernel auto-flash, raw fullflash (`fullflash CRC Ok ! (aa00645b, 740 ms)`), each followed by a normal boot; `mtd0` read back. |
| D1–D6 | **done** | `MEMO_BOOTLOADER.md`, `COMMANDS.md`, `TESTING.md`, `README.md`, `REBOOT_TO_BOOTLOADER.md`, `RESET_VECTOR_AUDIT.md`, parent `CLAUDE.md`/`AGENTS.md`. |

Bench: Lidl `.88`, first with V3.0 in flash and V3.1 `test.bin` loaded at `0x80100000` (harness = HOLD from Linux, TFTP, `J 80100000`, `J BFC00000` to leave); kernel auto-flash twice, the second with a UDP listener: `OK` on port 9999 8.7 s after the upload, then the reboot. **V3.1 (`6acba0aa…`) then flashed on the bench through `flash_remote.sh`** (2026-09-11 16:23, `mtd0` backed up first): HOLD → V3.1 banner from flash, normal kernel boot, and a 28 MiB upload aborted at exactly `0x81FFD000` after 26.99 MiB with TFTP error 3 `no room in RAM`, board still answering. `tests/tftp_probe.py` run by the operator against the flashed loader: all five malformed frames survived (PASS). The raw fullflash was then written from the flashed V3.1 by `flash_install_rtl8196e.sh`: both writes verified, clean boot. Every stage-2 item has now run on the board.

### Follow-up hardening status (2026-09-13, dedicated worktree)

The following follow-up items were found while reviewing the V3.1 result and
implemented on branch `hardening/stage2-only`.  This is not a released version
bump: `B_VERSION` remains V3.1 and the timestamp remains pinned, so the banner
alone cannot distinguish this worktree.  Both board production images and both
RAM-test images build successfully; `tests/run_host_tests.sh lidl` and
`tests/run_host_tests.sh sengled-e39-g8c` pass.  The Lidl image was flashed on
the `.88` bench box on 2026-09-13: the bootloader sent UDP `OK` after its
read-back verification and Linux (`6.18.45-rtl8196e-v4.4.0`) returned over SSH.

| ID | Status | Where / how validated |
|---|---|---|
| S15 | **implemented; peer-injection test pending** | `tftpd.c` binds an active WRQ/RRQ to its source MAC, IPv4 address and UDP port. Packets from a different peer are dropped and the binding is cleared on every completion/abort. Build + host bounds tests pass; the normal TFTP/flash path was exercised on the Lidl bench. A two-host forged-DATA/ERROR test remains the specific negative validation. |
| S16 | **done** | `tftpd.c` auto-flash policy accepts only a complete, known cvimg package and binds each type to its fixed partition. It rejects `ALL2`, unknown signatures, overlapping package members and out-of-layout packages before the first erase. Partition and even-length checks are host-tested; the Lidl boot image was flashed successfully. |
| S17 | **done** | `stage1_checks.h` bounds the LZMA-declared stage-2 output to 1 MiB and the board DRAM top; `bootload.c` rejects zero, truncated or incompletely decoded streams. Both boards' host tests and RAM-test builds pass; the Lidl bootloader was flashed and booted Linux. |
| S18 | **implemented; fault-injection test pending** | SPI-controller, SPI-NOR WIP, switch-table and MDIO polls have finite budgets. Their errors propagate to serial-only recovery or fail the write rather than looping forever. Normal Ethernet bring-up and a verified flash write succeeded on Lidl; a deliberately wedged controller needs hardware fault injection and is not claimed as bench-covered. |
| S19 | **done** | `lib/flash_tftp.sh` creates the retry safety probe in the TFTP client's working directory. The former `/tmp` path could fail locally yet be mistaken for an acknowledged probe. A one-byte rejected WRQ was acknowledged by the Lidl bootloader before the successful real upload. |
| C6 | **open** | Add an automated two-client TFTP regression (second peer DATA/ERROR during an active transfer) and a controllable MMIO fault seam for S18. They would turn the remaining negative hardware/protocol paths into repeatable tests without changing the production recovery path. |

---

## 1. Method and baseline

### 1.1 What was done

- Every file of `btcode/`, `boot/*.c`, `boot/*.S`, `boot/net/`, the project headers in
  `boot/include/` (SoC, flash, TFTP, monitor), `boards/`, the three Makefiles, both shell
  scripts and the six existing docs were read in full.
- A witness build was made in a scratch copy of the tree (never in the repo) with the
  toolchain the project ships (`mips-lexra-linux-musl-gcc` 15.2.0, crosstool-NG 1.28):
  - plain build of the `main` tree (`git archive main`): `btcode/build/boot.bin` md5
    `c89258e28ddccdf2c2fc9583d3280be1` == committed `boot-img/lidl/boot.bin` (22,498 B).
    **The build is reproducible; the shipped Lidl binary matches the V3.0 source.** (The
    G4 image, `f0969e5d…`, was not rebuilt in this pass.)
  - warning build: same sources with `-Wall -Wextra -Wno-unused-parameter` and
    `-fstack-usage` (see §7 for the raw numbers).
- The stage-1 disassembly was used to confirm the one finding that depends on branch
  logic (C1-a). The GD25Q127C datasheet in `0-Hardware/datasheet/` supplied the erase and
  program timings used in E1.
- Cross-references outside the tree were checked where the bootloader has a contract:
  `cvimg.c` (image header), `build_fullflash.sh` (raw fullflash layout),
  `34-Userdata/boothold/src/boothold.c` and the kernel DTS (HOLD page),
  `lib/flash_tftp.sh` and `flash_bootloader.sh` (UDP:9999 handshake).

### 1.2 Baseline facts a future session needs

Sizes (witness build, `lidl`):

| Artifact | text | data | bss | Notes |
|---|---|---|---|---|
| `boot/build/boot.out` (stage-2) | 53,308 | 1,152 | 83,056 | `.text` padded to `0x8040C000` by `ALIGN(0x4000)`; `.bss` ends `0x80421930` |
| `btcode/build/piggy.elf` (decompressor + LZMA payload) | 4,144 | 17,152 | 0 | payload `boot.img.gz` = 17,152 B |
| `btcode/build/boot.bin` (flash image) | | | | 22,498 B incl. 16 B `boot` header; 22,482 B written at flash offset 0 |

RAM map at run time (KSEG0 addresses; KSEG1 alias = +0x20000000):

| Range | Owner | Source |
|---|---|---|
| `0x80000080` (+0x80) | general exception vector, copied from `exception_matrix` | `irq.c:288` |
| `0x80100000`–`0x80105330` | piggy (decompressor + compressed stage-2), copied by stage-1 | `start.S:109-121`, `piggy.script` |
| `0x80105330`+4 KiB | piggy stack (`sp = __bss_end + 4096`) | `piggy.S:42-46` |
| `0x80300000` (+32 KiB) | LZMA probability array (`CProb` is 32-bit here, lc=3/lp=0 → 7,990 entries) | `bootload.c:4,104` |
| `0x80400000`–`0x8040C000` | stage-2 `.text` | `boot/ld.script` |
| `0x8040D4C8` (+64 KiB) | `dl_heap` (malloc arena) | `main.h:41-42` |
| `0x8041D4D0` (+8 KiB) | `init_task_union` = the only stack; `sp` starts at `~0x8041F4A0` | `main.h:17-18`, `head.S:104-107` |
| `0x80500000` | TFTP load address (`JUMP_ADDR` → `FILESTART`) | `tftpd.c:26,46` |
| `0x80560000` | kernel `startAddr` (from the `cs6c` header of the shipped `kernel-6.18.img`, len `0x15F962`) | `check_system_image()` |
| `DRAM_TOP-0x3000` (+4 KiB) | kernel watchdog crash record (`wdtcrash`, DTS only) | `rtl8196e.dts` |
| `DRAM_TOP-0x2000` (+4 KiB) | boothold page: HOLD at `+0xFFC`, IPV4 magic `+0xFF8`, IP `+0xFF4` | `main.c:65-79`, `boards/*/board.h` |

Flash map (16 MiB GD25Q127C, 4 KiB sectors, 64 KiB blocks, 256 B pages):

| Offset | Content on flash | Header on flash? |
|---|---|---|
| `0x000000` | bootloader (raw; first word `0x0BF00004` = `j load_boot`) | no (stripped by `build_fullflash.sh`, by the TFTP `boot` path, and by `cvimg -t boot`) |
| `0x020000` | kernel, `cs6c` header kept | yes — the bootloader scans for it at boot |
| `0x200000` | squashfs rootfs (raw, `hsqs`) | no |
| `0x400000` | JFFS2 userdata (raw) | no |

Execution contexts that matter for the findings:

- **Stage-1 (`start.S`) runs with no stack at all.** `uart_show` and `DDR_Auto_Calibration`
  are leaf routines; the payload copy uses `k0/k1`. The stage-1 stack "at top of DRAM"
  described in `main.c:61-63` and `REBOOT_TO_BOOTLOADER.md` does not exist in this code.
- **All TFTP work, including the entire flash write, runs inside the Ethernet interrupt
  handler with interrupts disabled**: `IRQ_finder` (`inthandler.S:42-43`, `SAVE_ALL` + `CLI`)
  → `irq_dispatch` → `do_IRQ` → `eth_interrupt` (`eth.c:124-137`) → `kick_tftpd` →
  `prepareACK` → `checkAutoFlashing` → `spi_flw_image_mio_8198` → ... → `autoreboot`.
  The console `monitor()` loop is blocked in `GetLine()` the whole time.
- `jiffies` only advances from the timer IRQ (IRQ 8). Anything that calls `delay_ms()`
  with interrupts masked spins forever (`libc.c:596-603`).

---

## 2. Executive summary

The bootloader is small (about 6,900 lines of project C/asm plus 9,000 lines of inherited
2.4-era Linux headers), reproducible, and does its job in the field. The audit found no
remote code execution path and no defect that breaks the normal boot of a healthy image.
What it found is concentrated in three places:

1. **The flash-write path has no verification and no size validation** (S2, S3, S4, S1).
   The UDP:9999 `OK` currently means "the write loop returned", never "the flash holds the
   image". For the bootloader partition that is the difference between a recoverable
   failure and a brick.
2. **The TFTP server and the flash write run in interrupt context** (S6). This explains
   several oddities the code works around by hand (the timer "stopping" after a flash,
   the blind delay loop in `autoreboot()`, the GIMR hack in the MDIO read) and puts a
   4 KiB VLA on an 8 KiB stack shared with the interrupted main loop.
3. **Erase granularity**: everything is erased 4 KiB at a time although the driver already
   implements 64 KiB block erase. On the datasheet typicals a 16 MiB fullflash spends
   ~205 s erasing where ~77 s would do (E1).

Around those, there is a long tail of dead code (some of it actively misleading for DRAM
work, C1), stale comments and documentation (C2, D1-D5), and a build that compiles with
no warnings enabled (129 warnings under `-Wall -Wextra`, 17 of them functions that fall
off the end without returning a value, C3).

Recommended sequencing (per-item validation in §3–§5, doc fixes in §6, checklist in §9):

| Phase | Content | Stage-2 only? | Validation |
|---|---|---|---|
| 0 | build flags + warning triage, dead code removal, comment/doc fixes | yes (except C1-a/b which are stage-1) | `test.bin` from RAM, then one flash on the bench box |
| 1 | input validation and stack safety: S1, S4, S5, S7, S9, S10, S14, E7 | yes | `test.bin` + crafted TFTP packets from the host |
| 2 | flash safety and speed: S2 (verify), S3 (write order), E1 (block erase), S11 (JEDEC) | yes | fullflash timing before/after on the bench box, power-cut test on a scratch partition |
| 3 | architecture: S6 (TFTP out of ISR), E2 (drop CPU-speed calibration), C6 (header consolidation) | yes | full regression of `doc/TESTING.md` §4 |
| stage-1 | S12, C1-b and C1-a (**done**, see status table) | **no** — flash-only validation, RAMTEST cannot exercise `start.S` | done as one flash cycle each on the Lidl bench |

---

## 3. Security and robustness findings (S)

Severity scale: **High** = can brick a device or crash the recovery path from the network;
**Medium** = incorrect behaviour reachable in normal use or a latent memory-safety defect;
**Low** = hygiene with a plausible but unlikely failure mode.

Threat model, stated once: the bootloader's network services (ARP, ICMP echo, TFTP on
UDP/69, UDP/9999 notify) are reachable only while the device sits at the `<RealTek>`
prompt, on the local L2 segment, and the whole point of that mode is to accept an
unauthenticated image from that segment. Authentication of images is therefore **out of
scope by design** (it would defeat recovery). The relevant property is: a malformed or
hostile packet must not corrupt memory, hang the loader, or make it write garbage to
flash — and a bad write must be detected.

### S1 — TFTP upload writes to RAM without any bound (High)

- **Where**: `boot/net/tftpd.c:670-677` (`prepareACK`), `tftpd.c:917-951` (`kick_tftpd`).
- **Evidence**: `tftpdata_length = ntohs(udp->len) - 4 - sizeof(struct udphdr)` is
  computed from the packet's own UDP length field, never compared with `nic.packetlen`
  and never checked against `TFTP_DEFAULTSIZE_PACKET`. A UDP length below 12 underflows
  to ~4 GiB; a UDP length of 65535 copies 64 KiB per block from beyond the 2 KiB RX
  cluster. `address_to_store` grows without limit: an upload of more than
  `DRAM_TOP - image_address` bytes (27 MiB on the Lidl board) runs off the end of DRAM and
  aliases back over the loader. Nothing prevents `LOADADDR` from pointing into the
  running bootloader or the exception vectors either (documented as a trap in
  `TESTING.md` instead of being refused).
- **Impact**: any host on the segment can crash the recovery loader while it is in
  download mode (denial of recovery); no code execution found, but it is memory
  corruption driven by network input.
- **Recommendation**: in `kick_tftpd()` require `nic.packetlen >= 14 + ntohs(ip->len)` and
  `ntohs(ip->len) >= 20 + ntohs(udp->len)`; in `prepareACK()` require
  `12 <= udp->len <= 12 + 512` and `address_to_store + len <= BOARD_DRAM_TOP_KSEG0 - 0x3000`
  (keep both reserved pages), otherwise send a TFTP ERROR (opcode 5, code 3 "disk full")
  and reset the state machine. In `CmdLoad()` refuse addresses below `_end` of the
  running image and above the reserved pages.
- **Validation**: `test.bin` + a 20-line Python/scapy sender producing (a) short UDP
  length, (b) oversized UDP length, (c) an upload longer than free DRAM; the box must stay
  at the prompt and answer `ping`.
- **Brick risk**: none (stage-2, RAM-testable).

### S2 — No read-back verification after a flash write; the FAIL path is unreachable (High, brick)

- **Where**: `boot/flash.c:688-729` (`ComSrlCmd_ComWriteData` returns `uiLen`),
  `flash.c:898-906` (`spi_flw_image_mio_8198` returns that value), `tftpd.c:469-481` and
  `tftpd.c:613-642` (callers treat non-zero as success).
- **Evidence**: no function in the write path reads anything back. The only way
  `spi_flw_image_mio_8198()` returns 0 is `image_size == 0`. `Flash Write Failed!` and the
  UDP `FAIL` after a write are dead paths. `flash_bootloader.sh`, `flash_remote.sh` and
  `flash_install_rtl8196e.sh` all trust the `OK`.
- **Impact**: a page-program failure, a stuck WIP bit, an SPI clock glitch or a partially
  bad sector produce a confident `OK` followed by a reboot. For `burnAddr == 0` this is a
  brick with no software recovery on the Lidl board (no hardware recovery pin per the
  project notes).
- **Recommendation**: after each `ComSrlCmd_ComWriteData()` call, re-read the range
  (either through `pfRead` in 4 KiB chunks into the existing sector buffer, or through the
  memory-mapped window at `0xBD000000` that `check_system_image()` already uses) and
  `memcmp` against the source; return 0 on mismatch. Make `spi_flw_image_mio_8198()`
  return 1/0 as its comment claims. Cost: one extra read of the written size; for 16 MiB
  a few seconds against the ~4 minutes of erase+program.
- **Validation**: on the bench box, `FLR`/`FLW` a 4 KiB scratch sector, then inject a
  mismatch by writing the RAM buffer after the `Y` prompt from a second path (or
  temporarily corrupt one byte in the verify buffer in a test build) and confirm `FAIL`
  is reported on serial and on UDP:9999.
- **Brick risk**: none for the change itself; it reduces existing risk.

### S3 — Raw fullflash: no integrity check, and the bootloader sector is written first (High, brick)

- **Where**: `boot/net/tftpd.c:459-483`.
- **Evidence**: the 16 MiB path is accepted on three magic words (`0x0BF00004` at 0,
  `cs6c` at `0x20000`, `hsqs` at `0x200000`). No checksum covers the 22 KiB of bootloader
  code or the 14 MiB of rootfs/userdata; the kernel's own `cs6c` checksum is present in
  the buffer but not verified on this path. The write is a single linear call starting at
  offset 0, so the bootloader partition is erased in the first ~0.4 s and the device is
  unbootable for the remaining ~4 minutes of the write.
- **Impact**: a truncated or bit-flipped upload (TFTP has no end-to-end checksum beyond
  the Ethernet FCS per frame; the UDP checksum is not verified by this stack) is written
  as-is; a power loss during the write leaves a device with no bootloader.
- **Recommendation**:
  1. Write order: program `0x20000..0x1000000` first, then `0..0x20000` last. Two calls
     instead of one; shrinks the no-bootloader window from ~240 s to the ~1 s it takes to
     erase and program 6 sectors. Combine with S2 so the bootloader sector is only written
     after the rest verified.
  2. Integrity: verify the kernel `cs6c` checksum in the buffer before writing anything
     (the code that does it for the per-partition path is 10 lines away). Consider having
     `build_fullflash.sh` append a 16-byte trailer (magic + CRC32 of the 16 MiB) in the
     erased tail of the userdata partition, and have the loader honour it when present;
     that keeps old images flashable and gives new ones end-to-end coverage. Magic-word
     checks on the bootloader portion are not a substitute: they prove nothing about the
     20 KiB behind them.
- **Validation**: fullflash on the bench box with a deliberately corrupted rootfs byte
  → must be refused; timing of the two-call write vs. the current one (expect equal).
- **Brick risk**: none for the change; it reduces existing risk. Coordinate with
  `build_fullflash.sh` / `flash_install_rtl8196e.sh` if a trailer is added.
- **Done**: S3.1 in V3.1 (2026-09-11), S3.2 in V3.1 (2026-09-12, trailer at `0x1FFF0`;
  see the status table). Found on the way: a 16 MiB upload into a stage-2 loaded from
  `test.bin` hangs the loader (three times, including the committed build), while the
  flashed loader takes it in 26 s — the raw path is validated from flash, not from RAM.

### S4 — Image header fields are trusted in the auto-flash path (Medium)

- **Where**: `boot/net/tftpd.c:485-511` and `:613-628`.
- **Evidence**:
  - `burnLen = Header.len (+16)` is used without checking that
    `head_offset + 16 + Header.len <= len` — a header claiming more than was uploaded makes
    the checksum loop and the flash write read past the received data into stale RAM.
  - `if (Header.burnAddr + burnLen > chip_size)` falls into a dual-chip path that calls
    `spi_flash_info[1].pfWrite`, and `spi_flash_info[1]` is zero-initialised: a NULL
    function call. The dual-chip design has no hardware counterpart here.
  - No check that `burnAddr` is sector-aligned; a mis-aligned `burnAddr` triggers the
    read-modify-write of the neighbouring sector including, for small offsets, the
    bootloader's own.
  - `tftpd.c:599-608`: the OpenWrt `0xdeadc0de` probe reads `*(startAddr + burnLen)`,
    which is the wrong base when `skip_header` is set and can be past the upload.
- **Recommendation**: reject with `FAIL` when `head_offset + 16 + Header.len > len`, when
  `burnAddr + burnLen > chip_size`, when `burnAddr % sector_size != 0`; delete the chip-1
  branch and the `0xdeadc0de` block (no image in this project relies on it).
- **Validation**: `test.bin` + hand-built headers (`cvimg` with `-b 0xFFF000`, and a
  truncated file).
- **Brick risk**: none.

### S5 — Boot-time header from flash is trusted for the kernel copy (Medium)

- **Where**: `boot/main.c:183-187` (`check_system_image`).
- **Evidence**: `startAddr` and `len` are read from flash and passed straight to
  `flashread()` as destination and length. The shipped kernel loads at `0x80560000`
  (1.44 MiB). A corrupted header (one flipped bit in `startAddr`) makes the loader copy
  1.4 MiB over itself, over the exception vectors or over the HOLD page — the board then
  hangs before the ESC check has run once, i.e. before any recovery path.
- **Recommendation**: accept only `0x80000000 + 0x100000 <= startAddr`,
  `startAddr + len <= BOARD_DRAM_TOP_KSEG0 - 0x3000`, `len <= 0x1E0000` (kernel
  partition), and `[startAddr, startAddr+len)` disjoint from `[0x80400000, _end)`;
  otherwise treat the image as absent (the loader then enters download mode, which is the
  designed recovery). Ten lines; the constants already exist in `board.h`.
- **Validation**: the header is read from flash, so the negative case cannot be staged
  from RAM, and **it must never be staged against the current loader** (a kernel header
  with `startAddr` inside the loader hangs every boot before the ESC check, with no
  software way back in). Test the bounds as a pure function on the host (a 20-line C
  test compiled natively), then on the box test only the positive path: both shipped
  kernels (6.18 and 7.1) must still boot. Note that `test.bin` does run `check_image()`
  (RAMTEST only skips `doBooting()`), so the positive path is also visible from RAM.
- **Brick risk**: low for the change (a wrong bound refuses a valid kernel, which lands in
  download mode); high for a careless negative test, see above.

### S6 — TFTP and flash writes run in interrupt context (Medium, architectural)

- **Where**: `boot/net/eth.c:124-137`, `boot/inthandler.S:42-43`, `boot/net/tftpd.c:394-442`.
- **Evidence**: see §1.2. Consequences visible in the code:
  - `autoreboot()` comments that "the SPI flash write can leave the jiffy timer stopped,
    so `delay_ms()` spins forever" and replaces it with a 4,000,000-iteration blind loop.
    The timer is not stopped; the CPU is inside `IRQ_finder` with `IE=0`, so IRQ 8 is
    simply not delivered. The workaround is correct, the diagnosis is not.
  - `rtl8651_getAsicEthernetPHYReg()` (`swCore.c:183`) re-enables IRQ 8 in GIMR before
    calling `delay_ms(10)` because `doBooting()` masks all of GIMR before
    `goToDownMode()`; without that hack `eth_startup()` would hang.
  - Stack: the ISR runs on the single 8 KiB stack on top of whatever `monitor()` had.
    Measured static frames on the deepest path (§7.2): `PT_SIZE` 176 + `irq_dispatch` 40
    + `do_IRQ` 40 + `eth_interrupt` 40 + `kick_tftpd` 56 + `prepareACK` 72 +
    `checkAutoFlashing` 120 + `spi_flw_image_mio_8198` 32 + `ComSrlCmd_ComWriteData` 96 +
    `ComSrlCmd_BufWriteSector` 48 + **4,096 (VLA)** + `ComSrlCmd_ComWriteSector` 48 +
    `ComSrlCmd_ComWrite` 48 ≈ 4.9 KiB, over a main-loop residue of ~0.3 KiB. The ACK
    path is shallower but carries a **1,504**-byte frame (`tftpd_send_ack`). It works
    with ~3 KiB to spare and no guard: an overflow lands silently in `dl_heap`.
  - The UART is not serviced for the whole transfer; the operator cannot abort.
- **Recommendation**: make the ISR do nothing but hand frames to a small ring (or simply
  a "packet pending" flag — the driver already has 4 RX descriptors and `swNic_receive()`
  is a poll), and run `kick_tftpd()` from the main loop. `GetLine()` needs a non-blocking
  variant that polls `uart_data_ready()` and the network between characters. With this in
  place: `delay_ms()` works everywhere, the GIMR hack in the MDIO read and the blind loop
  in `autoreboot()` go away, `sti()`/`cli()` sprinkled through `initHeap()` and
  `goToDownMode()` can be rationalised, and the stack margin doubles.
- **Validation**: full `doc/TESTING.md` §4/§5 pass from `test.bin`, plus a 16 MiB
  fullflash and a `flash_remote.sh` round trip.
- **Brick risk**: none if done from RAM first; medium effort (half a day).

### S7 — One TX bounce buffer shared by four TX descriptors (Medium)

- **Where**: `boot/swNic.c:274,316-318`; comment at `swNic.c:291-292` says the function
  "waits until the packet is successfully sent" — it does not.
- **Evidence**: every `swNic_send()` copies the frame into the same global `pktbuf`,
  hands the descriptor to the switch and returns. Back-to-back sends (the last-block ACK
  immediately followed by the UDP `OK` in `prepareACK()`→`checkAutoFlashing()`, or
  `doARPReply()` racing an ACK) overwrite a buffer the switch may still be reading.
  `eth.c:68` allocates `ETH0_tx_buf[NUM_DESC]` for exactly this purpose and uses only row 0.
- **Impact**: occasional corrupted ACK/notify frames; the host scripts already tolerate a
  missing `OK` with a 30 s timeout, which may be hiding this.
- **Recommendation**: before reusing a descriptor's buffer, spin until that descriptor is
  back in RISC ownership (`swNic_txDone()` logic, bounded by a loop count), or give each
  of the 4 descriptors its own 1,600-byte buffer (6 KiB of BSS). The second also removes
  the need for the blind delay in `autoreboot()` (E8).
- **Validation**: `tcpdump` on the host during 50 `flash_bootloader.sh` runs; count
  malformed frames before/after.
- **Brick risk**: none.

### S8 — Legacy Realtek settings blocks are read from flash and can alter the MAC (Low)

- **Where**: `boot/net/eth.c:85-187` (`read_setting_block`, `gethwmac`, `getmacandip`),
  called from `eth_startup()`; offsets `0x6000`/`0xC000` from `etherboot.h:82-84`.
- **Evidence**: on entry to download mode the loader reads flash offsets `0x6000` and
  `0xC000` (inside the bootloader partition, i.e. leftovers of the Tuya stock firmware
  on a migrated board, or `0xFF` on a fullflashed one), checks a one-byte magic, mallocs up
  to 16 KiB, checksums, and may replace `eth0_mac[0]` and `[5]` from that data. The
  results in `tmp_mac`/`tmp_ip` are discarded; only the side effect on `eth0_mac` remains,
  and `tftpd_entry()` then overwrites bytes 1–4 with the IP anyway.
- **Recommendation**: delete the three functions and the two offsets; keep the fixed
  `eth0_mac` (or derive a locally-administered MAC deterministically). Also fixes the
  malloc-and-hang path if the stale block advertises a length near the 64 KiB heap.
- **Validation**: ARP/ping/TFTP from `test.bin` on a board that still has Tuya data at
  `0x6000` (any migrated Lidl box) — the MAC must be the fixed one.
- **Brick risk**: none.

### S9 — Information leaks over ICMP echo and TFTP RRQ (Low)

- **Where**: `tftpd.c:869-872` (`handle_icmp_echo`), `tftpd.c:273-308` (`handleTFTP_RRQ`).
- **Evidence**: the echo reply copies `ntohs(ip->len) - 20` bytes (bounded to 1,480) from
  the RX cluster regardless of the real frame length, so a 60-byte ping claiming 1,500
  returns 1,440 bytes of previous packet data. RRQ serves `file_length_to_server` bytes
  from `image_address` to any client: after an `FLR` of the whole flash that includes the
  JFFS2 userdata (Thread network key, dropbear host keys). Local segment only, operator
  initiated, but worth a line in the docs and a bound in the code.
- **Recommendation**: bound the echo copy by `nic.packetlen - 14 - 20`; document that RRQ
  exports whatever was last loaded (it is a feature per `COMMANDS.md`), or gate RRQ
  behind an `AUTOBURN`-style toggle defaulting to off.
- **Brick risk**: none.

### S10 — Stray store into DRAM from `console_init()` (Low)

- **Where**: `boot/uart.c:78`: `*(volatile unsigned long *)(0xa1000000) = divisor;`.
- **Evidence**: writes the baud divisor to physical `0x01000000` (16 MiB) on every boot.
  Debug leftover. Harmless on 32/64 MiB boards today (below the kernel load address,
  above the loader), it would hit the last page of a 16 MiB board.
- **Recommendation**: delete the line.
- **Brick risk**: none (`test.bin` prints the banner through this path).

### S11 — The SPI flash driver ignores the JEDEC ID (Low)

- **Where**: `boot/flash.c:207-226` (`spi_regist`): `ComSrlCmd_RDID()` is called twice,
  the result is stored, and `GD25Q128` geometry is applied unconditionally.
- **Evidence**: banner says `Flash: GD25Q128` whatever the chip. A board with a different
  capacity or sector layout (a future port) would be erased/programmed with the wrong
  geometry silently. `3-Main-SoC-Realtek-RTL8196E/CLAUDE.md:15` describes this as "the
  bootloader's JEDEC probe" — it is a label, not a probe.
- **Recommendation**: print the 3-byte ID in the banner; compare manufacturer `0xC8` and
  capacity code `0x18` and, on mismatch, keep reads working but refuse writes with a
  clear message (a wrong-geometry write is worse than no write). Also fix the sentence in
  the agent notes.
- **Brick risk**: none.

### S12 — Stage-1 failures are silent (Low, stage-1)

- **Where**: `btcode/start.S:159-191` (DQS sweep), `btcode/bootload.c:91-110`,
  `btcode/piggy.S:49-50`.
- **Evidence**: if the DQS sweep never finds a passing delay (`L0 == 0`), the code still
  computes a centre (16) and continues into non-working DRAM. If LZMA rejects the stream,
  `boot_entry()` returns to `piggy.S`, whose `jal boot_entry` (`piggy.S:50`) is the last
  instruction of the file under `.set noreorder`: the linked image
  (`btcode/build/piggy.elf.txt`) shows `memcpy` starting at the very next word
  (`0x80100070`), so the first instruction of `memcpy` already executes in the delay slot,
  and a return from `boot_entry()` continues into the body of `memcpy`. Neither failure
  prints anything after `Booting...`.
- **Recommendation**: an explicit `nop` in the delay slot and a `b .` (or a UART
  character per failure class, `D` for DRAM, `L` for LZMA, then a halt — the UART is
  already up). Ten instructions, but **any `start.S`/`piggy.S` change is flash-only
  validated** (RAMTEST loads `piggy.bin` at `0x80100000` and does exercise `piggy.S`, but
  not `start.S`), so bundle with the next stage-1 change rather than shipping alone.
- **Brick risk**: medium for stage-1 edits in general; validate on the G4 first (it has a
  hardware recovery pin) then on a Lidl bench box.
- **Status**: implemented and validated on the Lidl bench box only (no G4 at hand);.

### S13 — Every error path is an infinite loop with no watchdog (Low, design choice)

- **Where**: `irq.c:188-189` (unregistered IRQ), `irq.c:210-211`, `inthandler.S:82-91`
  (spurious interrupt prints `m` forever), `calloc.c:176-178` (malloc failure),
  `rtl_types.h:119-123` (`ASSERT_CSP`), `swTable.c:79,229,274`.
- **Evidence**: the hardware watchdog is only ever used as a reset trigger
  (`0xB800311C = 0` in `autoreboot()` and `J BFC00000`). A hang in download mode is
  permanent until power-cycle; on a headless remote site that means a truck roll.
- **Recommendation**: one `fatal(const char *why)` helper that prints, waits ~5 s, and
  triggers the watchdog reset — used by all the loops above. Do **not** arm a periodic
  watchdog in download mode (a 16 MiB write legitimately takes minutes). Note the reset
  preserves DRAM, so a HOLD flag would still be honoured; decide whether `fatal()` should
  clear it (probably yes, to avoid a reset loop into the same failure).
- **Brick risk**: none.

### S14 — Undefined behaviour and latent bugs surfaced by `-Wall -Wextra` (Low)

- **Where**: full list in §7.1. The ones that are real defects:
  - 17× `-Wreturn-type`: `request_IRQ()` and `free_IRQ()` (`irq.c:158,164`), all `Cmd*`
    handlers in `monitor.c`, `SettingCPUClk()`. `monitor()` stores the garbage return
    value in `retval` and ignores it, so no visible effect today.
  - `irq.c:248-254`: `print_string` used uninitialised when no status bit is set; and
    `prom_printf("ADDR:%x, ENTRY:%x", ...)` is passed three arguments for two conversions.
  - `swTable.c:186,194,250,258`: `if (0 != temp & 0x1)` parses as `(0 != temp) & 1`; the
    intent (test IE) is not what runs. It happens to work because Status is never zero,
    so interrupts are always disabled/restored around the table read.
  - `main.c:538-539`: the `LOCALSTART_MODE` → `DOWN_MODE` fall-through is intentional
    (return from `goToLocalStartMode()` means the user pressed ESC) and should say so.
  - `calloc.c:104`: `prev` may be uninitialised if `free()` is called with `frhd == NULL`;
    guarded by `ASSERT_CSP(frhd)` above, so only a robustness nit.
- **Recommendation**: build with `-Wall -Wextra -Wno-unused-parameter
  -Werror=return-type -Werror=parentheses -fno-strict-aliasing` (the code type-puns
  everywhere: `unsigned short *` over headers, `unsigned long *` over `tmpbuf`), fix the
  list, and keep the warning count at zero in the Makefile.
- **Brick risk**: none; the fixed binary must still be validated from RAM because
  `-fno-strict-aliasing` changes codegen.

### S15 — An active TFTP transfer was not bound to its initiating peer (Medium)

- **Where**: V3.1 `boot/net/tftpd.c`: the global `CLIENT_port`, `client_ip` and
  `one_tftp_lock` tracked transfer state but DATA, ACK and ERROR processing did not
  consistently prove that the incoming Ethernet/IP/UDP tuple was the peer that opened
  the transfer.
- **Evidence**: TFTP has no authenticated session identifier.  On the recovery L2, a
  second host could send a plausible packet while the legitimate client was uploading.
  Depending on the opcode, it could advance a block, abort the state machine or receive
  a response intended for the other host.  The normal single-client flash scripts do not
  expose this race.
- **Impact**: a local peer can turn a legitimate update into a failed or wrong RAM image.
  Header and checksum checks reduce the chance of a flash write, but they are not a
  substitute for keeping one transfer owned by one peer.
- **Recommendation**: capture source MAC, IPv4 address and UDP source port when accepting
  WRQ/RRQ.  Require all subsequent DATA, ACK, ERROR and OACK packets to match it; silently
  drop packets from every other peer and clear the binding on completion, timeout and
  abort.  Preserve the existing ephemeral server-port behaviour.
- **Validation**: two hosts on the same L2: start an upload from A, inject DATA and ERROR
  from B using the correct block and server port, then confirm A completes byte-exactly
  and B receives no state-changing reply.  This is a protocol negative test, not a NOR
  flash test.
- **Brick risk**: none (stage-2, RAM-testable).
- **Status**: implemented in the hardening worktree (`struct tftp_peer`, capture/match/
  clear helpers).  Production and RAM-test builds for both boards pass and the normal
  Lidl `.88` flash path passed; the two-host negative test remains pending.

### S16 — A valid cvimg header still had too much authority over flash layout (High, brick)

- **Where**: V3.1 `boot/net/tftpd.c` after `autoflash_header_ok()`.
- **Evidence**: S4 validates that a header is internally bounded, aligned and inside the
  chip.  It does not establish that a `boot`, kernel, rootfs or userdata container is
  writing its *intended* partition.  A syntactically valid package can therefore select
  another aligned range; broad `ALL2` containers also make package intent ambiguous.
- **Impact**: an operator error or a malformed package can overwrite an unintended
  partition, including the bootloader range.  Read-back verification proves what was
  written, not that the selected destination was appropriate.
- **Recommendation**: preflight the whole package before its first erase; allow only the
  known signatures and require exact partition starts plus a length contained in that
  partition.  Reject `ALL2`, unknown members, overlap and packages larger than the four
  fixed partitions.
- **Validation**: host tests for each partition edge, odd payload lengths and overlap;
  RAM-test malformed-package uploads; then one normal bootloader upload from the flashed
  loader.  Do not use a bootloader-partition negative case on a loader that lacks this
  guard.
- **Brick risk**: none for the guard; it removes an existing source of wrong-partition
  writes.
- **Status**: done in the hardening worktree through `flash_image_policy()` and
  `flash_partition_ok()`.  The host tests pass for Lidl and Sengled; the Lidl image was
  flashed on `.88`, sent UDP `OK` and booted Linux.

### S17 — Stage-1.5 trusted the LZMA output size and a partial decode (Medium, pre-stage-2)

- **Where**: V3.1 `btcode/bootload.c` (`boot_entry()`).
- **Evidence**: the 64-bit LZMA size field was only checked for a non-zero high word and
  representability in `SizeT`.  The low word could still describe an arbitrarily large
  stage-2 output at `0x80400000`; a successful decoder return was accepted without proving
  it produced the declared number of bytes.
- **Impact**: a corrupted bootloader payload can overwrite DRAM outside the intended
  stage-2 region before the jump.  This is not a reset-vector or DDR-init path, but it is
  before the stage-2 monitor and therefore needs the same care as stage-1 changes.
- **Recommendation**: make the output window an explicit per-board policy, reject zero,
  truncated and over-limit streams before decoding, and require `outProcessed == outSize`.
  Keep the bound deliberately small (1 MiB) relative to available RAM, rather than making
  it a second copy of the entire DRAM size.
- **Validation**: native tests for zero, maximum, maximum-plus-one and wrap sizes on both
  boards; rebuild production and RAM-test images; compare a host decompression of the
  packaged payload with `boot.out`; finally flash once and capture a clean cold boot.
- **Brick risk**: low but non-zero because `btcode/bootload.c` is stage-1.5 and is embedded
  in every bootloader image.  `btcode/start.S`, DDR register values, the stage-1 linker
  script and the stage-1 copy loop must remain unchanged.
- **Status**: done in the hardening worktree (`stage1_checks.h`).  Both board builds and
  host tests pass; host LZMA output matched `boot.out`, and the Lidl `.88` flash/reboot
  succeeded.  This is the only follow-up item below stage-2.

### S18 — Peripheral wait loops could make recovery permanent (Medium)

- **Where**: V3.1 `boot/flash.c` (SPI controller ready and NOR WIP), `boot/swCore.c` /
  `boot/swTable.c` (MDIO and switch table engine).
- **Evidence**: these polls had no iteration or time limit.  A stuck SPI controller
  prevents reads and writes indefinitely; a stuck MDIO/table engine freezes download-mode
  Ethernet setup before the prompt can service TFTP.  The generic `fatal()` work does not
  help when execution never leaves a polling loop.
- **Impact**: a transient hardware fault becomes a permanent loss of network recovery;
  an unbounded SPI wait during a write is especially dangerous because it obscures whether
  NOR programming completed.
- **Recommendation**: give short controller/table/MDIO polls a finite iteration budget
  and NOR WIP a larger independent budget.  Latch an SPI failure, refuse later writes,
  propagate switch setup failures and retain the serial monitor rather than continuing
  with a partly initialised network path.
- **Validation**: normal boot, Ethernet entry and flash write on both boards; a controlled
  stuck-status-register test for every timeout path.  The latter needs a hardware fault
  seam or an instrumented RAM-test build, not a destructive NOR test.
- **Brick risk**: none for the normal path; bounded failure handling reduces the chance of
  an operator interrupting an apparently hung flash operation.
- **Status**: implemented in the hardening worktree.  `g_spi_fault` disables flash writes
  after a timeout, `eth_startup()` returns failure and enters serial-only recovery, and
  table/MDIO errors propagate.  Both board builds pass; Lidl `.88` entered Ethernet mode,
  verified the bootloader write and returned to Linux.  Forced-fault coverage is pending.

### S19 — The host retry safety probe could report an idle bootloader without sending a packet (High, host-side brick risk)

- **Where**: `lib/flash_tftp.sh` (`probe_tftp_wrq()`), used by
  `flash_bootloader.sh` and the other TFTP flash scripts.
- **Evidence**: the local TFTP client used on the build host does not accept an absolute
  `/tmp/...` PUT source in this invocation.  The probe treated every local error except a
  timeout as a positive response, so it could declare the loader idle without emitting a
  WRQ at all.  After a timed-out final block, that false positive permits a retry while
  the target may already be erasing or programming NOR.
- **Impact**: a second transfer during an ambiguous in-progress flash can corrupt the
  recovery update.  This is outside the target binary but directly controls the risk of
  writing its bootloader partition.
- **Recommendation**: create the one-byte deliberately invalid probe in the TFTP
  client's current directory and pass the relative name; retain the rule that a probe
  timeout is ambiguous and must never be retried.
- **Validation**: observe an invalid one-byte WRQ on the serial console, then complete a
  normal upload with UDP:9999 `OK`.  The invalid payload must be rejected before any
  erase.
- **Brick risk**: none for the probe itself; high for the pre-fix retry decision.
- **Status**: done in the hardening worktree.  A one-byte probe was acknowledged by the
  Lidl `.88` loader, followed by a single successful 23,186-byte bootloader upload and a
  normal Linux reboot.

---

## 4. Efficiency findings (E)

### E1 — 4 KiB sector erase everywhere; block erase exists but is unused (High value)

- **Where**: `boot/flash.c:219` (`pfErase = ComSrlCmd_SE`), `flash.c:688-729`
  (`ComSrlCmd_ComWriteData`), `flash.c:454-462` (`ComSrlCmd_BE`, 64 KiB, unused).
- **Evidence**: every 4 KiB of any write is erased with command `0x20`. GD25Q127C
  datasheet typicals (3.3 V table): sector erase 50 ms, 64 KiB block erase 300 ms, page
  program 0.5 ms. Per 64 KiB that is 16 × 50 = 800 ms of erase against 300 ms.

  | Image | Erase today (typ) | Erase with 64 KiB blocks (typ) | Program (unchanged) |
  |---|---|---|---|
  | fullflash 16 MiB | 4096 × 50 ms ≈ 205 s | 256 × 300 ms ≈ 77 s | 65,536 × 0.5 ms ≈ 33 s |
  | userdata 12 MiB | ≈ 154 s | ≈ 58 s | ≈ 25 s |
  | kernel 1.44 MiB | ≈ 18 s | ≈ 7 s | ≈ 3 s |

  These are datasheet typicals, not measurements; the observed fullflash time on the
  bench box is the number to compare against.
- **Recommendation**: in `ComSrlCmd_ComWriteData()`, when `uiSectorAddr` is 64 KiB aligned
  and at least 16 whole sectors remain, erase with `ComSrlCmd_BE` and program the 256
  pages; fall back to sector erase for the head/tail. No change to the page-program path.
  Pair with S2 so the faster path is also the verified one.
- **Validation**: fullflash timing before/after; S2 verify must pass; `FLW` of a 4 KiB
  range must still work (tail path).
- **Brick risk**: low — the erase command is standard and the driver already implements
  it; validate on a scratch range (`0xF00000`, inside userdata) before touching partition
  boundaries.

### E2 — CPU-speed calibration on every boot only feeds the banner (Medium)

- **Where**: `boot/monitor.c:182-222` (`check_cpu_speed`), called from
  `showBoardInfo()` (`main.c:139`).
- **Evidence**: the routine calibrates `loops_per_jiffy` against the 10 ms tick: 9
  doublings each waiting for a tick edge plus the delay itself, then 8 refinement steps.
  From the algorithm this is on the order of 150–250 ms per boot (to be measured; the
  serial timestamps around the banner give it directly). The result is used only to print
  `CPU: 400MHz`. The other delay users (`__delay(5000)` in `FullAndSemiReset()`,
  `delay_ms()`) do not depend on it; `loops_per_sec`/`udelay()` are never updated nor used.
- **Recommendation**: keep `timer_init()` (jiffies are needed), drop the calibration, and
  print a constant or a strap-derived frequency. Saves ~0.2 s on every boot for free.
- **Validation**: measure banner-to-banner time before/after with the serial capture host.
- **Brick risk**: none.

### E3 — The kernel is copied through SPI PIO and checksummed on every boot (Medium, measure first)

- **Where**: `boot/main.c:183-194`.
- **Evidence**: 1.44 MiB is read with `mxic_cmd_read_s1()` (4 bytes per FIFO read with a
  ready poll each) into RAM, then summed 16 bits at a time. The kernel's own decompressor
  then runs. The read is the dominant term; the sum is a few ms. The memory-mapped window
  at `0xBD000000` (already configured by the dummy read at the end of `spi_regist()`)
  might be faster for a bulk copy, or might not — the SFCR2 read mode is single-IO too.
- **Recommendation**: measure the copy time first (two `get_timer_jiffies()` reads around
  `flashread()`), then decide between: bulk copy through the memory window, dual-IO read
  (`0x3B`, supported by the chip and by SFCR2's `DATA_IO` field), or leaving it. Do not
  drop the checksum: it is the only integrity check the boot path has.
- **Brick risk**: none if the checksum stays.
- **Done** (2026-09-12): measured and decided, see the status table. The memory window
  is slower than PIO (420 ms vs 230 ms); the clock is already at the chip's ceiling.

### E4 — 10 ms sleep per MDIO read in download-mode entry (Low)

- **Where**: `boot/swCore.c:184`.
- **Evidence**: `Setting_RTL8196E_PHY()` and the per-port N-way restart perform ~20 MDIO
  reads at `eth_startup()`, each with `delay_ms(10)` ("8196C test chip patch") before the
  status poll → ~200 ms added to every download-mode entry. Only on the recovery path, so
  low priority; the fix is to trust the `MDCIOSR` busy bit, which the write path already
  does, and validate on both boards.
- **Done** (2026-09-12): tried and reverted — the busy-bit poll wedges the controller on
  one register (see the status table). The delay stays.

### E5 — Dead bytes in the image (Low, cosmetic)

- `.text` is padded to `0x4000` for an IRAM section nothing populates (`ld.script:24-33`),
  `head.S:42` reserves 1 KiB for exception handlers that live at `0x80000080`, `eth.h`
  emits a 544-byte `nicreg[]` table into `.data` that nothing reads, and `head.S:234-258`
  fills the IRAM with 4 KiB of whatever follows `.rodata`. Together ~6 KiB of the 90 KiB
  uncompressed stage-2; LZMA hides most of it in flash but not in RAM or in decompression
  time.

### E6 — LZMA workspace (informational)

- `CProb` is 32-bit in this SDK copy (`LzmaDecode.h:42`), so the probability array at
  `0x80300000` is 32 KiB for the default lc=3/lp=0/pb=2. It sits 1 MiB below the
  decompression target; there is no check that a different `lzma` configuration keeps it
  there. A one-line static check on the properties byte in `boot_entry()` would make the
  assumption explicit. Decompression of 48 KiB is not a measurable cost.
  **Done** (2026-09-12): the check is in `boot_entry()`, see the status table.

### E7 — 1.5 KiB stack frames in the TFTP send functions (Low, pairs with S6)

- **Where**: `tftpd.c:239` and `tftpd.c:763` put a full `struct tftp_t` (1,432-byte data
  union) on the stack to send a 4-byte ACK; `etherboot.h:183-201` defines `tftpreq_t`
  for exactly this and nothing uses it. Static buffers, or `tftpreq_t`, cut the ISR peak
  by 1.5 KiB.

### E8 — Blind delay before the post-flash reboot (Low, pairs with S7)

- `autoreboot()` spins 4,000,000 iterations so the UDP `OK` leaves the switch before the
  PHY is turned off. Polling the TX descriptor back to RISC ownership is deterministic and
  shorter.

---

## 5. Clarity and maintainability findings (C)

### C1 — Dead and misleading code inventory

The items marked **(!)** are the ones that can mislead a future DRAM or boot debugging
session in the same way the removed `start_c.c` did.

Stage-1 (`btcode/`, flash-only validation):

- **(!) C1-a** `start.S:64-71`: the "RTL8196E MCM DDR1 package" write of `0x0AC8` to
  `CLKMGR_REG` is unreachable. The two `IF_NEQ` guards require `SYS_STATUS == 7` **and**
  `== 4` at once; the disassembly (`bfc00114-bfc00150`) confirms both `bne` jump to
  `rtl_8196E_four`. `RESET_VECTOR_AUDIT.md:39-40` and the source comment describe it as
  "if 0x7 or 0x4". Either the intent was `and t6, t6, 7` first (then it is a real
  behaviour change to restore, to be validated on hardware) or the block should go.
  **Do not "fix" it without a flash test on both boards** — it changes the clock manager
  value on any board whose status register reads 4 or 7.
  **Done** (2026-09-12): deleted, not restored — with a mask the selector would have fired
  on the Lidl (`SYS_STATUS` reads `0xF`), see the status table.
- **(!) C1-b** `piggy.S:21-35`: `regdata` table, never referenced. Labelled "UART
  configuration", it actually lists DDR controller registers with values
  (`0xFFFF05C0` at `0x1008`, `0xD2800000` at `0x1050`) that contradict `board.h`. Delete.
  **Done** (2026-09-12): table and the jump over it removed, see the status table.
- `start.S:142-143`: DQS1 bounds (`t5`, `t7`) initialised and never used; the DQS0 centre
  is applied to both DQS fields (`start.S:196-202`). The `RESET_VECTOR_AUDIT.md` note
  "DQS1 path appears bypassed" is confirmed; say so in the source.
- `start.S:110`: the copy loop ends at `__boot_end + 4`, one word past the payload.
  Harmless (reads flash), inaccurate.
- `start.h:92-94, 120-125, 141-156, 160-172, 174, 176-181`: `ADD3VAL`, `UART_WRITE`,
  `UART_PRINT_DELAY`, `UART_BIN2HEX`, `VIR2PHY`, `SRAM_BASE/SRAM_TOP` (the latter claims
  "32M" and evaluates to `0x88000000`) — all unused.
- `start.S:76`: comment "Set DTR by hw_strap ck_m2x_freq_sel" — the strap is not read
  (see `CLAUDE.md`); `btcode/Makefile:3-6` says `start.S` clears BSS and sets up the
  stack — it does neither (`piggy.S` sets `sp`; nobody clears piggy's BSS, which is empty).
  **Done** (2026-09-12) for the five items above: comments corrected, dead macros deleted,
  the unused DQS1/R0 registers and the `+ 4` bound documented and kept (byte-identical
  images), see the status table.

Stage-2 (`boot/`, RAM-testable):

- `monitor.c:723-869`: `SettingCPUClk()`, `SPEED_isr`, `irq_SPEED` and the 25-line
  `#undef` block — no caller. `monitor.c:445-465` `CmdWriteHword` ("EH") — not in
  `MainCmdTable`. `monitor.c:168-172` `loops_per_sec` — never updated, only reachable via
  the unused `udelay()`.
- `irq.c:215-267` + `inthandler.S:110-140`: Lexra watchpoint handler (`do_watch`,
  `handle_watch`, exception 23). No code ever programs `LX0_WMPCTL`; 800 bytes of debug
  scaffolding with an uninitialised-variable bug inside. Remove, or keep behind a
  `DEBUG_WATCH` macro.
- `irq.c:19,25-31`: `exception_handlers[32]` and `irq_action[NR_IRQS]` with a long
  explicit `NULL` initialiser for a static (already zero) array.
- `flash.c`: `spi_block_erase`, `spi_erase_chip`, `spi_read`, `flashwrite`,
  `spi_flw_image` — declared in `spi_flash.h`, no caller; `spi_flash_info[2]` and every
  `ucChip` parameter (single chip); `CheckDramFreq()` table commented "8196D";
  `spi_flw_image_mio_8198()` is a synonym of `spi_flw_image()` with a misleading name.
  The two-layer Realtek structure (`SeqCmd_*`, `ComSrlCmd_*`, `mxic_*`, `spi_*`) can
  collapse to read / erase-sector / erase-block / program-page / write-range.
- `flash.c:169,517-563,225`: `ucSFCR2` starts at 154 and the first `ComSrlCmd_ComRead()`
  programs SFCR2 for memory-mapped reads as a side effect; the dummy `pfRead(0, 4)` at the
  end of `spi_regist()` exists only to trigger it, and `check_system_image()` depends on
  it through `rtl_inw(FLASH_BASE + ...)`. Make it an explicit `spi_enable_mmap_read()`.
- `eth.c:85-187`: see S8. `eth.c:68`: `ETH0_tx_buf[NUM_DESC]` with only row 0 used.
  `eth.c:126`: `0xb801002c` is `CPUIISR`; use the symbol.
- `eth.h` (539 lines): only `MBUF_LEN` is used by `eth.c`, and it is also defined in
  `rtl8196x/loader.h`. The header defines a non-static global array (`nicreg[]`,
  `eth.h:345`) — data in a header. Delete the file.
- `etherboot.h`: `TICKS_PER_SEC`, `TIMEOUT`, `MAX_*_RETRIES`, `AWAIT_*`, `TFTP_CODE_*`,
  `DEFAULT_*FILE`, `tftpreq_t`, `IP_BROADCAST` — leftovers of the etherboot client.
  `tftpd.c:41` `ETH_BROADCAST` unused; `tftpd.c:913` `jump` unused; `tftpd.c:716` client
  IP `192.162.1.116` (typo of 192.168, immediately overwritten, and `MEMO_BOOTLOADER.md`
  documents the 192.168 value).
- `swNic.c:172-173`: `arpInput`/`arpResolve` declared static, never defined;
  `swNic.c:175,380`: `#pragma ghs section` (Green Hills compiler); `swTable.c:57-80`
  `swTable_forceAddEntry` unused; `swCore.c:231-253` `rtl8651_setAsicFlowControlRegister`
  unused; `swCore.c:460` prints `P0phymode=01, embedded phy` on every download-mode entry
  for a constant.
- `main.c:212-251` `check_rootfs_image()` and the whole `#if !SKIP_ROOTFS_SCAN` block
  (`main.c:295`): compiled out since `SKIP_ROOTFS_SCAN 1`; the `cr6c` (kernel+rootfs)
  signature it serves is not produced by any build script in this repo. `main.h:45-47`
  `HS/DS/CS_IMAGE_OFFSET`, `rtk.h` `SETTING_HEADER_T`, `HW_SETTING_OFFSET` duplicated in
  `rtk.h:23` and `etherboot.h:82`.
- ESC detection — **[fixed in V3.0]** for the two defects behind issue #159: the periodic
  poll inside the kernel checksum loop is now live (`main.c:190-192`, one poll per
  `CHKKEY_POLL_BYTES` = 64 KiB, `main.h:32`; the V2.9 counter needed 32 GiB of image to
  fire once), and `pollingDownModeKeyword()` (`main.c:348-374`) drains the whole receive
  FIFO looking for the key instead of giving up on the first byte. Bench-validated in
  paired runs per the commit message. What remains, and is cosmetic only:
  `user_interrupt(unsigned long time)` (`main.c:382-385`) still ignores `time`,
  `WAIT_TIME_USER_INTERRUPT` (`main.h:34`) is still unused, and the docs still describe a
  "3-second countdown" that never existed (D2). Note for the next reader: the poll runs
  during the checksum pass over RAM, not during the SPI copy that precedes it; an ESC
  typed during the copy waits in the 16-byte UART FIFO and is found at the first poll,
  which is why key-repeat works and why a single tap can still be lost if more than 16
  other bytes arrive first.
- `libc.c:81-95` `strstr`, `libc.c:539-546` `SprintF` — unused.
- `head.S:113-118` enables COP3 (CU3) a second time after `_rom_flush_cache` did; then
  `init_arch()` (`arch.c:35`) clears CU3 again; `head.S:234-258` programs IRAM base/top and
  refills it for a `.iram-rtkwlan` section that is empty ("DDR calibration requires IRAM"
  is a stale comment — calibration is in stage-1). `head.S:104` sets `$28` (`gp`) to the
  stack base, a 2.4-kernel convention with no consumer under `-G 0`.
- `arch.c:30`: `init_arch(int argc, char **argv, char **envp, int *prom_vec)` — no
  caller passes arguments.

### C2 — Comments and behaviours that contradict each other

- `main.c:407-409,428-439`: the hand-off quiesce comment says the `J` command does "the
  same quiesce". `CmdCfn()` (`monitor.c:308-315`) only turns the PHY interface off; it does
  not stop `CPUICR` nor call `FullAndSemiReset()`. Either give `J` the full sequence (it
  is the path `test.bin` and manual kernel boots use) or correct the comment. Since V2.9
  exists because the PHY-off alone was insufficient, aligning `J` is the safer choice.
- `main.c:61-63`, `REBOOT_TO_BOOTLOADER.md:115-118,272-278`: rationale for the HOLD page
  placement cites a stage-1 stack at top of DRAM. There is none (§1.2). The page choice
  remains correct (the kernel DTS reserves it and the loader never touches the top
  pages), but the stated reason is wrong and will send the next person looking for a
  stack that does not exist. The observed false positives at the top page came from
  something else (kernel or stock loader); say "unexplained, empirically excluded".
- `tftpd.c:398-408`: `autoreboot()` diagnosis (see S6).
- `swNic.c:291-292`: "waits until the packet is successfully sent" (see S7).
- `flash.c:875-876,895-896`: "Return: 1 on success, 0 on failure" (see S2).
- `boot/Makefile:3-4`: "hardware init, DDR calibration" — stage-2 does no DDR work.
- `uart.c:19-21`: "TX timeout 6540 iterations ~340 µs @ 200 MHz LexRA" — the CPU runs at
  400 MHz and the loop is compiler-dependent; it is a bound, not a calibrated delay.
- `31-Bootloader/.gitignore`: entries for `src/btcode/*`, `src/config/...` and
  `23-Bootloader/` — paths that no longer exist.

### C3 — Build hygiene

- No warning flags anywhere; 129 warnings appear with `-Wall -Wextra` (§7.1). Stage-2
  builds at `-O` (O1) while stage-1 builds at `-Os`; neither uses
  `-fno-strict-aliasing` although both alias freely. `-ffreestanding` is set for stage-1
  only; stage-2 relies on GCC not turning its `memcpy` loop into a call to itself (it
  does not today, but `-ffreestanding -fno-builtin` makes it a rule).
- The build recipe exists twice (`Makefile:38-50` and `build_bootloader.sh:142-157`) and
  must be kept in sync by hand; both hard-code `$(HOME)/rtl8196e-gateway` fallbacks
  (`Makefile:30`, `btcode/Makefile:48-50`), and the script hard-codes the Docker path
  `/home/builder/realtek-tools/bin`.
- The ramtest variant is built into the same `build/` directories after the production
  one, which is why `btcode/build/boot.bin` on disk is never the shipped image
  (documented as a trap in `CLAUDE.md`). A second `OUTDIR` (`build-ramtest/`) removes the
  trap and the need to `clean` between variants.
- `BOOT_CODE_TIME` is pinned (good) but lives in `boot/Makefile:33` while `B_VERSION` lives
  in `ver.h`; the release checklist has to touch both. One `version.h` with both strings
  is simpler. `ver.h` defines `static char B_VERSION[]` in a header (warns as unused in
  every other includer).

### C4 — Structure and naming

- `monitor.c` hosts the timer and CPU-clock code (`timer_init`, `timer_interrupt`,
  `jiffies`, `check_cpu_speed`); a `timer.c` would make the dependency of `delay_ms()`
  on IRQ 8 visible.
- Shared state is declared ad hoc with `extern` inside `.c` files (`monitor.c:25-35,517`,
  `tftpd.c:30,34`, `eth.c:63-64`) instead of in headers; `main.h` **defines** four globals
  (`init_task_union`, `return_addr`, `kernelsp`, `dl_heap`) and only links because
  `main.c` is its sole includer.
- Three generations of SoC headers coexist (`asm/rtl8181.h`, `asm/rtl8196.h`,
  `asm/rtl8196x.h`, plus `rtl8196x/asicregs.h`) with overlapping and conflicting
  definitions — `FLASH_BASE` alone is `0x02BF0000`, `0x05000000` and `0x06000000`
  depending on the include order. `boot_soc.h` currently resolves to `rtl8196.h`; one
  header should remain.
- `boot/include/linux/` and `boot/include/asm/` are ~9,000 lines of Linux 2.4 headers of
  which a few hundred lines are used (`stackframe.h`, `regdef.h`, `mipsregs.h`,
  `lexraregs.h`, the `cli/sti` inlines, `types.h`). A compile-checked pruning pass would
  leave ~15 files; `BOOTLOADER_TOOLCHAIN_NOTES.md` already recorded the intent.
  Done 2026-09-12 in two passes — a paragraph-level prune, then the re-homing of what
  was used into the loader's own headers; the status
  table has the figures.
- Magic numbers where symbols exist: `0xB800311C` (`WDTCNR_REG`), `0xb801002c`
  (`CPUIISR`), `0xb8000010` in `FullAndSemiReset()` (`CLKMGR_REG` in stage-1's naming),
  `0x80000000 + 0x80` (`KSEG0 + 0x80`).

### C5 — TFTP state machine

`BootStateEvent[3][8]` (`tftpd.c:144-178`) is a full table for a protocol with two
useful states; rows 0 and 1 differ in two cells, and the WRQ/RRQ acceptance logic is
duplicated between `kick_tftpd()` (`one_tftp_lock`, the 20 s retransmit window) and the
table. A `switch` on `(bootState, opcode)` in one function would be a third of the code
and make the S1/S4 checks local. Keep the observable behaviour (ports, retransmit
window, `SERVER_port++` per transfer) — the host scripts depend on it.

---

## 6. Documentation drift (D)

Items where a document states something the code does not do. These are cheap and safe
to fix and should go with phase 0.

| ID | Document | Says | Code |
|---|---|---|---|
| D1 | `MEMO_BOOTLOADER.md:176-186` | timestamp from `$(shell date)`; `B_VERSION "V2.2"` | pinned constant (`boot/Makefile:33`); V3.0 |
| D1 | `MEMO_BOOTLOADER.md:48-52, 87-117` | stage-2 "initializes SDRAM, Ethernet switch, GPIO"; ESC polled "during image copy (configurable timeout)"; quotes the V2.9 body of `pollingDownModeKeyword()` | stage-2 does no DRAM/GPIO init; no timeout exists; the quoted function was rewritten in V3.0 |
| D1 | `MEMO_BOOTLOADER.md:289-295` | mtd3 userdata at `0x420000` | `0x400000` (`build_fullflash.sh:67`, `userdata.bin` header, and the memo's own cvimg example at line 405) |
| D1 | `MEMO_BOOTLOADER.md:127` | expected client IP `192.168.1.116` | `192.162.1.116` (typo in code, irrelevant since overwritten) |
| D2 | `COMMANDS.md:10`, `TESTING.md:49-50` (still on `main`) | "3-second boot countdown", "press ESC within 3 seconds" | no timed window before or after the scan, in V2.9 or V3.0; the key is sampled while the image is scanned (see C1) |
| D2 | `COMMANDS.md:121-131`, `TESTING.md:291`, `README.md:98-101` | `boot` images do not auto-reboot; "reboot manually with `J BFC00000`" | `sign_tbl` has `reboot = 1` for `boot` since V2.5; `flash_bootloader.sh:184` says so |
| D3 | `REBOOT_TO_BOOTLOADER.md:115-118, 156-158, 272-278` | "btcode stack at top of RAM"; "kernel image loaded by btcode"; safety map lists "DDR size detection" | no stage-1 stack; stage-2 loads the kernel; size detection was the removed `start_c.c` |
| D4 | `RESET_VECTOR_AUDIT.md:39-40` | "if `0xb800000c == 0x7` or `0x4`, set MCM DDR1" | unreachable; selector removed with C1-a (2026-09-12) |
| D5 | `3-Main-SoC-Realtek-RTL8196E/CLAUDE.md:15-16` (and `AGENTS.md`) | "the bootloader's JEDEC probe labels it GD25Q128" | hard-coded label (S11) |
| D6 | `TESTING.md:32` | bootloader BSS ends `0x80421600` | `0x80421930` in V3.0 (moves with every build; say "see `boot/build/boot.nm`") |

`BOOTLOADER_TOOLCHAIN_NOTES.md` is a post-mortem and is accurate as history; its claim
that `-fgnu89-inline` was removed matches the Makefiles.

---

## 7. Appendix — measurements from the witness build

### 7.1 Warnings under `-Wall -Wextra -Wno-unused-parameter`

Stage-2: 129 warnings. Stage-1: 2 (`B_VERSION` unused in `bootload.c`; `inStream` unused).

| Category | Count | Where |
|---|---|---|
| `-Wignored-qualifiers` | 43 | inherited `linux/byteorder/swab.h` (33), `kernel.h`, `rtk.h` |
| `-Wpointer-sign` | 27 | `tftpd.c` (15), `monitor.c`, `swCore.c`, `swNic.c`, `main.c` |
| `-Wsign-compare` | 18 | loops with `int` against `unsigned` |
| `-Wreturn-type` | 17 | `irq.c:158,164`; `monitor.c:317,373,400,421,443,465,485,515,588,657,673,688,702,721,869` |
| `-Wunused-variable` | 9 | `irq.c:177`, `monitor.c:406,831`, `eth.c:199,223`, `tftpd.c:913`, `ver.h:3` |
| `-Wparentheses` | 4 | `swTable.c:186,194,250,258` |
| `-Wunused-but-set-variable` | 3 | `flash.c:647`, `monitor.c:238`, `swNic.c:362` |
| `-Wunused-function` | 2 | `swNic.c:172,173` |
| `-Wunknown-pragmas` | 2 | `swNic.c:175,380` |
| `-Wmaybe-uninitialized` | 2 | `calloc.c:104`, `irq.c:254` |
| `-Warray-parameter=` | 2 | `swNic.c:417,419` vs `swNic_poll.h:29-30` |
| `-Wunused-const-variable=` | 1 | `tftpd.c:41` |
| `-Wimplicit-fallthrough=` | 1 | `main.c:538` |

### 7.2 Stack usage (`-fstack-usage`, bytes, static unless noted)

Largest frames: `tftpd_send_data` 1,520; `tftpd_send_ack` 1,504; `monitor` 200;
`eth_startup` 160; `tftpd_send_notify` 120; `checkAutoFlashing` 120; `CmdIp` 104;
`outnum` 96; `ComSrlCmd_ComWriteData` 96; `ComSrlCmd_BufWriteSector` 48 + **dynamic**
(4,096-byte VLA sized by `sector_size`); `boot_entry` 88 (`CLzmaDecoderState` +
properties); `swNic_init`/`swCore_init` 88; `LzmaDecode` 88.

Interrupt path worst case (deepest chain through the VLA, `PT_SIZE` 176 included):
≈ 4.9 KiB on the 8 KiB `init_task_union`, on top of the interrupted `monitor()`/`GetLine()`
residue (≈ 0.3 KiB). No guard page; a stack overflow lands in `dl_heap` (below) silently.

### 7.3 Largest symbols (stage-2)

`dl_heap` 65,536 (bss) · `init_task_union` 8,192 · `ETH0_tx_buf` 3,200 · `pktbuf` 2,048 ·
`eth_packet` 1,518 · `icmp_reply` 1,500 · `swNic_init` 1,992 (text) · `checkAutoFlashing`
1,392 · `swCore_init` 1,304 · `kick_tftpd` 1,152 · `vprintf_internal` 1,100 ·
`nicreg` 544 (data, unused) · `do_watch` 432 + `handle_watch` 444 (unused).

---

## 8. What was deliberately not recommended

- **Image authentication / signing.** The TFTP server is the recovery path for a device
  with no other entry; requiring signatures would turn a wrong key into a brick. The
  integrity work (S2, S3) is where the value is.
- **A watchdog armed during download mode.** A 16 MiB write legitimately takes minutes;
  a periodic watchdog would need servicing from inside the flash loop and adds a new way
  to reset mid-write.
- **Renaming registers or reordering stage-1 MMIO writes.** `RESET_VECTOR_AUDIT.md`'s
  warning stands: stage-1 order and NOPs are hardware-validated, not derived. Only C1-a/b
  and S12 touch stage-1, and each is flagged flash-only.
- **Replacing the Realtek switch driver.** `swCore.c`/`swNic.c`/`swTable.c` are opaque but
  work on both boards; the quiesce sequence added in V2.8/V2.9 depends on their exact
  behaviour. Clean around them, not through them.
- **Changing the on-flash layout, the header format, the UDP:9999 protocol or the
  `boothold` page contract.** All have consumers outside this tree (`cvimg`, the four
  flash scripts, `boothold`, the kernel DTS, `flash_remote.sh`).
- **A/B bootloader slots and a commit record.** The 16 MiB NOR layout has no spare
  bootable partition without a migration that displaces a supported payload.  The fixed
  partition policy, complete preflight and read-back verification in S16 reduce the
  practical write risk without imposing that operational migration.
- **A general TFTP-module rewrite.** S15 makes the existing small state machine
  single-peer and S19 makes its host retry rule conservative.  Replacing it solely for
  abstraction would add recovery-path churn without a direct reliability benefit; keep
  the proposed two-peer regression in C6 instead.

---

## 9. Checklist for the implementing session

1. Branch from local `main` (V3.0, contains `e726439`), not from `kernel/7.2-imem`,
   which is still at V2.9 for this tree. Read `31-Bootloader/CLAUDE.md` (build,
   reproducibility, `btcode/build/boot.bin` trap) and `doc/TESTING.md` (RAMTEST workflow).
   Everything in stage-2 is testable from RAM with `test.bin`; nothing in `start.S` is.
   V3.0 added a `---RAMTEST key check` line to `test.bin` output, which is the hook for
   testing anything that touches the ESC path.
2. Start with phase 0 on a branch: enable the warning flags, fix S14, remove the dead
   code in C1 (stage-2 part), fix the comments in C2 and the docs in §6. Rebuild, confirm
   the new `boot.bin` md5 is stable across two builds, run the `TESTING.md` §4 checklist
   from `test.bin`, then flash once on the bench box (`192.168.1.88` at the time of
   writing; resolve it through `lib/gwconf.sh`, do not hardcode).
3. Phase 1 and 2 items each come with a validation line above; keep the per-item
   validation, do not batch-validate. S2 before E1 (verify first, then speed up).
4. Bump `B_VERSION` and `BOOT_CODE_TIME` together; the per-board `boot-img/*/boot.bin`
   are committed artifacts and must be rebuilt with the matching `BOARD=` (both boards
   share every line of stage-2, so both binaries change on any stage-2 edit).
5. Anything that touches `start.S` or `piggy.S` is validated on the G4 first (hardware
   recovery pin), then on a Lidl board that can be desoldered if needed.
6. For the follow-up hardening items, do not replace the normal flash/reboot check with a
   synthetic one: build production plus RAM-test for **both** boards, run the native bounds
   tests, make the stage-1.5 host decompression equal `boot.out`, then perform one verified
   flash/reboot on the bench box.
7. Before calling S15/S18 fully bench-covered, run the outstanding negative tests: a
   second L2 host must not alter an active transfer, and an instrumented RAM-test build
   must force each SPI/table/MDIO timeout while retaining a serial recovery prompt.

---

## 10. Summary — recommendation by recommendation

| ID | Recommendation | Status |
|---|---|---|
| S1 | Bound TFTP/IP/UDP lengths and the upload RAM window; `LOADADDR` refusals | Fixed in V3.1 |
| S2 | Read back and verify every flash write; make the FAIL path reachable | Fixed in V3.1 |
| S3.1 | Raw fullflash: verify the kernel checksum first, write the bootloader partition last | Fixed in V3.1 |
| S3.2 | CRC trailer appended by `build_fullflash.sh` and honoured by the loader | Fixed in V3.1 (2026-09-12, see the status table) |
| S4 | Validate auto-flash image headers; drop the chip-1 branch and the `0xdeadc0de` probe | Fixed in V3.1 |
| S5 | Bound-check the kernel header read from flash before copying | Fixed in V3.1 |
| S6 | Move the TFTP server and the flash write out of the interrupt handler | Fixed in V3.1 |
| S7 | One transmit buffer per TX descriptor | Fixed in V3.1 |
| S8 | Delete the legacy Tuya settings-block readers | Fixed in V3.1 |
| S9 | Bound the ICMP echo copy; document RRQ | Fixed in V3.1 |
| S10 | Delete the stray store in `console_init()` | Fixed in V3.1 |
| S11 | Print the JEDEC ID; refuse writes on a mismatching capacity | Fixed in V3.1 |
| S12 | Report stage-1 failures (DQS sweep, LZMA) instead of continuing silently | Fixed in V3.1 (2026-09-11, see the status table) |
| S13 | Replace every infinite error loop with a report + watchdog reset | Fixed in V3.1 |
| S14 | Build with warnings; fix the undefined behaviour they surface | Fixed in V3.1 |
| S15 | Bind every active TFTP transfer to its opening MAC/IP/UDP peer | Implemented in the hardening worktree; two-client negative test pending |
| S16 | Preflight a complete package and bind each image type to its fixed partition | Done in the hardening worktree; host tests + Lidl `.88` flash/reboot |
| S17 | Bound the declared stage-2 LZMA output and reject incomplete decoding | Done in the hardening worktree; stage-1.5 only, both builds/tests + Lidl `.88` flash/reboot |
| S18 | Bound SPI, WIP, table-engine and MDIO waits and propagate their failures | Implemented in the hardening worktree; normal-path bench covered, forced-fault test pending |
| S19 | Make the host TFTP retry safety probe prove that a WRQ was sent | Done in the hardening worktree; invalid probe + successful Lidl `.88` upload |
| E1 | Erase by 64 KiB block where aligned | Fixed in V3.1 |
| E2 | Drop the per-boot CPU-speed calibration | Fixed in V3.1 |
| E3 | Measure the kernel copy time, then decide on a faster read mode | Decided in V3.1 (2026-09-12): PIO kept — 230 ms vs 420 ms through the memory window; SPI clock already 100 MHz |
| E4 | Drop the 10 ms delay per MDIO read | Decided in V3.1 (2026-09-12): kept — polling the busy bit instead hangs the controller on PHY 1 reg 6 (reproduced on the Lidl) |
| E5 | Remove dead bytes from the image (IRAM padding, `nicreg[]`, handler reservation) | Decided in V3.1 (2026-09-12): `nicreg[]` gone, the 10.8 KiB I-MEM padding kept — no payoff |
| E6 | Check the LZMA properties byte in the decompressor | Fixed in V3.1 (2026-09-12, RAM test + flash on the Lidl bench) |
| E7 | Static transmit frame instead of 1.5 KiB stack frames | Fixed in V3.1 |
| E8 | Replace the blind delay before the post-flash reboot | Fixed in V3.1 |
| C1-a | Unreachable MCM DDR1 branch in `start.S` | Fixed in V3.1 (2026-09-12, deleted; flash + five cold boots on the Lidl bench) |
| C1-b | Dead `regdata` table in `piggy.S` | Fixed in V3.1 (2026-09-12, RAM test + flash on the Lidl bench) |
| C1 (other stage-1 items) | `start.S`/`start.h` leftovers, `btcode/Makefile` comment | Fixed in V3.1 (2026-09-12, comments and dead macros only; images byte-identical) |
| C1 (stage-2 items) | Dead code inventory of `boot/` | Fixed in V3.1 (except `head.S` CU3/IRAM/`gp`, see E5) |
| C2 | Comments and documents that contradict the code | Fixed in V3.1 |
| C3 | Build hygiene: warning flags, separate ramtest output, one version header | Fixed in V3.1 (recipe deduplicated 2026-09-12) |
| C4 | Structure: `timer.c`, shared state in headers, SoC header consolidation, Linux header pruning, magic numbers | Fixed in V3.1 (2026-09-12: inherited headers gone, 30 owned files / 5,087 lines, images byte-identical at every step) |
| C5 | Rewrite the TFTP state machine as a `switch` | Fixed in V3.1 (2026-09-12, bench regression from RAM and from flash) |
| C6 | Add two-peer TFTP and controllable peripheral-timeout regression tests | Open — test-only work, no production-path change proposed |
| D1 | `MEMO_BOOTLOADER.md` drift | Fixed in V3.1 |
| D2 | `COMMANDS.md` / `TESTING.md` / `README.md`: countdown, manual reboot | Fixed in V3.1 |
| D3 | `REBOOT_TO_BOOTLOADER.md`: non-existent stage-1 stack | Fixed in V3.1 |
| D4 | `RESET_VECTOR_AUDIT.md`: unreachable branch | Fixed in V3.1 (documented as unreachable) |
| D5 | Agent notes: "JEDEC probe" | Fixed in V3.1 |
| D6 | `TESTING.md`: BSS end address | Fixed in V3.1 |

Everything marked fixed was validated on the Lidl bench, first from RAM (`test.bin`), then
from the flashed V3.1 (including the raw fullflash through `flash_install_rtl8196e.sh`).
The stage-1 items (S12, E6, C1) were done last, since they cannot be validated from RAM:
each was flashed on the Lidl bench with the console captured over repeated cold boots and
`mtd0` read back, the accepted exception to the "RAM first" rule while no G4 is at hand.

The 2026-09-13 follow-up items use the same rule.  S15 and S18 are deliberately marked
**implemented**, rather than fully bench-validated, until their hostile-peer and
forced-peripheral-fault tests exist.  S17 changes stage-1.5 only: it adds a pure LZMA
output bound after the unchanged `start.S` DDR bring-up and before the stage-2 jump.
The actual Lidl flash was performed from an existing boot hold; the target's `OK`
notification was received and Linux subsequently answered over SSH.

## Impact — V3.1 against V3.0 (v4.4.0), measured 2026-09-12

Everything above, once shipped, against the loader of v4.4.0. Method: `tests/loc.sh` for the source counts; six runs of `flash_install_rtl8196e.sh -y` on the Lidl bench box, the console captured and timestamped on the host, V3.0 put back with `flash_bootloader.sh` before each of its runs. Sizes from `tests/loc.sh` and the two builds (`c89258e2` for V3.0, `68c30af3` for V3.1, both reproducible).

| | V3.0 (v4.4.0) | V3.1 | |
|---|---:|---:|---|
| **Source, comments and blank lines excluded** | | | |
| sources, C + assembler, stage-1 and stage-2 | 5,383 | 4,705 | −13 %, with the audit's additions in |
| headers | 9,092 | 1,242 | −86 %, 91 → 19 files, none inherited |
| **Binaries** (Lidl) | | | |
| `boot.bin`, the flash image | 22,498 B | 21,690 B | −808 B |
| stage-2 compressed payload (`boot.img.gz`) | 17,309 B | 16,070 B | −7 % |
| stage-2 `.text` | 53,404 B | 53,640 B | +236 B: the bound checks, the CRC trailer, the fatal path |
| stage-2 `.data` | 1,152 B | 312 B | |
| stage-2 `.bss` | 83,056 B | 96,128 B | +13 KiB: the two static 4 KiB sector buffers of the verified write (no VLA any more), the CRC table |
| **`flash_install`, 16 MiB full-flash** (median of 3 runs, bench `.88`) | | | |
| TFTP upload | 21.2 s | 21.7 s | unchanged, spread of the six runs 0.9 s |
| flash write | 107.8 s | 61.2 s | −43 %: 64 KiB block erase instead of 4 KiB sectors, with the trailer CRC (0.74 s), the kernel checksum and the read-back of every unit now included |
| `fullflash.bin` construction and miscellaneous | 66 s | 64 s | host side, independent of the loader: SSH probe, config save, `build_fullflash.sh` (userdata JFFS2 regenerated, 16 MiB assembled), `boothold` — about 55 s — then the Linux shutdown, the reboot into the loader and the TFTP probe, about 9 s |
| whole script | 195 s | 147 s | −48 s, all in the write |

Not in the numbers: V3.1 refuses an image whose trailer CRC or kernel checksum is wrong before the first erase, verifies every flash write and reports the failure over UDP:9999, reports stage-1 failures (DQS window, LZMA) instead of hanging silently, runs nothing network-related in interrupt context, and owns every header it compiles — the build fails on any sysroot include.
