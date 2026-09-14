# RTL8196E watchdog — production design

| Item | Value |
|---|---|
| Driver | `rtl819x-wdt` v1.12 |
| Hardware register | `WDTCNR`, physical `0x1800311c` |
| Watchdog clock | 25 kHz, shared CDBR-derived timer clock |
| Hardware windows | approximately 1.31 s to 671 s |
| Post-mortem ABI | record v9, 108 bytes |

## Scope

The watchdog is a recovery primitive. Its hot paths perform one MMIO write:
`start`, `stop`, and `ping` do not allocate, trace, inspect other drivers,
or run a periodic sampler. The only diagnostic retained in production is a
fixed-size panic record written after reset recovery has been armed.

The historical flight recorder, printk-tail capture, scheduler/list walks,
interrupt-controller snapshots, Ethernet/NAPI snapshots, and detailed
multi-record decoder were experimental incident tooling. They are removed
from this driver.

## Hardware protocol

`WDTCNR` has an enable byte, write-one clear bit, and overflow selector. A
non-`0xa5` enable byte runs the counter; `0xa5` stops it. The driver uses:

```
start/ping:  ENABLE_PATTERN | WDTCLR | maximum selector
stop:        DISABLE_PATTERN | WDTCLR
restart/panic recovery: DISABLE_PATTERN | WDTCLR; then 0
```

The two writes in the final sequence are intentional. The first clears and
stops the counter; the second starts it from a known cleared state in the
minimum overflow bucket. Do not collapse or reorder them without hardware
validation.

## Panic path

```
panic notifier (INT_MAX priority)
    ├─ arm short hardware reset
    └─ if the DT crash page is available:
          invalidate magic
          write bounded v9 fields
          barrier
          publish magic last
```

The record write is O(1), has no allocation, no locks, no list traversal, and
no dependency on Ethernet, IRQ-controller, or scheduler internals. It
therefore fits inside the short reset grace window even if the rest of the
kernel is compromised.

The next probe accepts only the exact v9 size and version, prints one line,
and clears the magic. Treat the reserved DRAM as untrusted input: all fields
are fixed-offset and the textual reason is NUL-bounded and sanitized before
printing.

## Device-tree contract

`rtl8196e.dts` reserves two independent no-map pages:

| Node | Address | Owner |
|---|---:|---|
| `watchdog-crash@1ffd000` | `0x01ffd000` | watchdog record v9 |
| `boothold@1ffe000` | `0x01ffe000` | bootloader handoff |

The watchdog node references only `watchdog-crash`. A board port must move
both the node and the address used by any board-specific recovery tooling; it
must never reuse `boothold` for the watchdog record.

## Validation

1. Build the watchdog object and the board DTB with warnings enabled.
2. Boot with `CONFIG_RTL819X_WDT=y`; verify the v1.12 probe banner.
3. Confirm normal feeder pings and a clean software reboot.
4. In a controlled test image, trigger a panic; verify a watchdog reset,
   exactly one post-boot record line, and that it is cleared on the following
   boot.
5. Verify bootloader handoff still works independently of the crash page.
