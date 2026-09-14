# RTL819x timer driver — v1.3 design

## Platform and hardware contract

The RLX4181 has no usable CP0 Count. TC1 is the continuous 28-bit clocksource
and `sched_clock`; TC0 is the one-shot clockevent. DATA and COUNT values use
bits [31:4]. `busclk / DivFactor` must equal the DT `refclk` exactly; the
driver owns the 0x1c timer resource while WDTCNR remains a watchdog resource.

Initialization is deliberately fail-fast:

1. `timer_of_init()` claims MMIO, enables refclk and requests the IRQ.
2. TC0/TC1 and both timer IRQ sources are stopped/cleared.
3. mandatory busclk is enabled; CDBR is range-checked, programmed and read
   back.
4. TC1 is started in TIMER mode with its IRQ disabled, then registered.
5. TC0 is registered as a one-shot clockevent (`min_delta=8`).

At 25 kHz the resolution is 40 us, the minimum deadline is 320 us, and the
28-bit counter wraps after about 2 h 59 min. CDBR also clocks the watchdog;
timer-rate changes require joint timer/watchdog validation.

## Protected default

With `CONFIG_RTL819X_TC0_DNT=n`, Timer0 uses COUNTER mode. Every arm performs
`disable -> DATA0 -> enable`, then observes COUNT0 against TC1. A zero COUNT0
after six TC1 edges means the functional timer did not start, even if CTRL
echoes TC0_EN; the full sequence is retried up to four times. This path is
safe but adds bounded busy-wait time while clockevent callbacks hold local IRQs
off.

## Do-not-toggle canary

With `CONFIG_RTL819X_TC0_DNT=y`, the first clockevent performs T1:

```text
IR: mask TC0 + W1C pending
CTRL: set TC0_TIMER_MODE | TC0_EN
```

Thereafter TC0_EN never changes until reboot. A deadline is:

```text
DATA0 = encode(delta)
IR: enable TC0 IRQ, write zero to W1C pending bits
```

The zero W1C write preserves an expiry that races the minimum delta. In the
ISR, TC0 IRQ is masked and W1C-acknowledged before invoking the clockevent
handler. In NO_HZ shutdown and later oneshot state callbacks it is only
masked/acknowledged; CTRL is not rewritten. Consequently the DNT steady state
has no TC0 enable edge, no polling, and no runtime reversion.

The suspected cause is a slow-domain capture failure of the enable edge: field
register evidence and the DNT experiment support it, but do not establish a
vendor erratum. DNT is build-time opt-in and has no runtime control interface.

## Security and performance invariants

1. Use ordered `readl()`/`writel()` only; no raw or physical-pointer MMIO.
2. Keep TC1 IRQ masked and reads side-effect-free.
3. Never round or assume a clock rate.
4. In DNT steady state, only T1 may write TC0 CTRL; ISR/shutdown do not.
5. Program DATA0 before unmasking DNT IRQ, preserving pending races.
6. Keep `NO_HZ_IDLE=y`; it reduces idle arms but is not a substitute for DNT
   under load.
7. Never add a runtime fallback that changes timer mode after an anomaly;
   recovery begins from protected state at reboot.
