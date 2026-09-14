# Reset Vector / DDR Init Audit (RTL8196E)

Scope: `btcode/start.S` and `btcode/start.h`.
This is **stage‑1** code that runs from the reset vector in SPI flash and
prepares DDR + UART, flushes caches, copies the stage‑1.5 payload into RAM,
and jumps to it.

## Execution Context

- Entry point: `__start` at reset (SPI flash, 0xBFC00000).
- This code runs **before** RAM is guaranteed stable.
- Uses hard‑coded MMIO addresses and CP0 CCTL (Lexra style).
- **Not testable via RAMTEST** (RAMTEST skips reset vector + DDR init).

## Macro Summary (start.h)

- `BOOT_ADDR`: 0x80100000 (destination address for piggy payload).
- `REG32_R/W/ANDOR`: register read/write helpers using `t6/t7`.
- `IF_EQ/IF_NEQ`: branch helpers using `t6/t7`.
- UART register base: `UART_BASE = 0xB8002000`.
- `SYS_CLK_RATE = 200MHz`, `BAUD_RATE = 38400`.

## High‑Level Flow (start.S)

1. **Disable interrupts**
   - `mtc0 t0, $12` with `t0 = 0`.

2. **SoC patch (RTL8196E)**
   - Read `0xb8000000`; if equals `0x8196e000`, set bit 19 in `0xb8000008`.

3. **UART init + banner**
   - Configure UART LCR/FCR/IER, compute divisor from `SYS_CLK_RATE`.
   - Print banner: `"\r\nBooting...\r\n"`.

4. **Power/clock/DDR register setup**
   - `0xb8001040 = 0x3FFFFF80` then `0x7FFFFF80` (MPMR power save).
   - `0xb8001050 = 0x50800000` (DDCR base).
   - `0xb8000010 = 0x00000b08` (CLKMGR).
   - The vendor code then read `0xb800000c` (system status) to select
     `0xb8000010 = 0x00000ac8` for the "RTL8196E MCM DDR1 package" when it
     read 7 or 4; its two `IF_NEQ` guards required both values at once, so
     the write was unreachable while the read and the compares did run.
     The selector was removed (audit C1-a, 2026-09-12): the clock manager
     keeps `CLKMGR_DEFAULT` on every board, which is what always happened.
   - `0xb8001008 = 0x90E36920` (DDR1 32MB @ 193MHz).
   - `0xb8001004 = 0x54480000`.
   - `0xb8000048`: set bit 23, clear bits 22:23 first.
   - `0xb8000088`: clear bits 29:30 (freq=2M).
   - `0xb800008c`: set OCP bits `0x1f << 2`.

5. **DDR auto‑calibration**
   - Jump to `DDR_Auto_Calibration`.
   - Uses test pattern `0x5a5aa5a5` at `0xA0000000`.
   - Reads/writes DDCR at `0xB8001050`.
   - Sweeps DQS0 from 1..32, records L0/R0, computes center C0.
   - Writes DDCR with derived DQS settings, then returns.

6. **Cache flush (Lexra CCTL)**
   - `mtc0 $0, $20` → `mtc0 0x3, $20` → `mtc0 $0, $20`
   - Commented as “flush all cache”.

7. **Copy stage‑1.5 payload into RAM**
   - Copies `[__boot_start .. __boot_end]` to `BOOT_ADDR`.
   - Loop: `lw` from source, `sw` to destination, increments by 4.

8. **Jump to BOOT_ADDR**
   - `jr k0` where `k0 = BOOT_ADDR` (0x80100000).

## DDR_Auto_Calibration Details (start.S)

Registers and constants:
- DRAM test address: `0xA0000000`
- DDCR register: `0xB8001050`
- Pattern: `0x5a5aa5a5`
- Mask: `0x00ff00ff`
- Expected value: `0x005a00a5`

Algorithm (summary):
- For DQS0 = 1..32:
  - Write candidate DDCR value.
  - Read back DRAM and compare masked pattern.
  - Record first (L0) and last (R0) passing DQS0.
- Compute midpoint `(L0 + R0) / 2` and write DDCR.
- (DQS1 path appears bypassed in current code.)

## UART Init (uart_show)

Registers configured (UART0):
- LCR, FCR, IER, DLL/DLM.
- Divisor computed from `SYS_CLK_RATE` and `BAUD_RATE`.
- Prints `boot_msg`.

## Notes / Risks

- Any change to the **order** of MMIO writes, or removal of NOPs,
  can break DDR init on RTL8196E.
- Cache flush sequence here is **not** the same as stage‑2 cache helpers.
- **RAMTEST does not execute this code**, so changes require real flash boot.

## Testability

- **RAMTEST:** NOT applicable to this file.
- **Flash boot:** Required to validate any modifications in `start.S`.
