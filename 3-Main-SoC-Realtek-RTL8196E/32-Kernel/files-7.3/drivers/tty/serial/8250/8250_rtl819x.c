// SPDX-License-Identifier: GPL-2.0+
/*
 * Realtek RTL8196E UART1 glue driver for 8250 core.
 *
 * This driver is specifically for UART1 (0x18002100) which requires hardware
 * flow control for communication with the EFR32 Zigbee NCP. UART0 (0x18002000)
 * uses the standard ns16550a driver and serves as the system console.
 *
 * Manages the SoC-specific flow control register (bit 29 @ 0x18002110) needed
 * for reliable RTS/CTS operation - setting CRTSCTS in termios alone is not
 * sufficient on this SoC. Also forces registration as ttyS1 to avoid stealing
 * the console (ttyS0) from UART0.
 *
 * Copyright (C) 2025 Jacques Nilo
 */

#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "8250.h"

#define DRV_NAME    "rtl8196e-uart"
#define DRV_VERSION "1.7"

/* Phantom RX-timeout accounting — see rtl8196e_uart_handle_irq(). */
static atomic64_t phantom_count = ATOMIC64_INIT(0);

static int rtl8196e_uart_phantom_count_get(char *buffer,
					  const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%lld\n", atomic64_read(&phantom_count));
}

static const struct kernel_param_ops rtl8196e_uart_phantom_count_ops = {
	.get = rtl8196e_uart_phantom_count_get,
};

module_param_cb(phantom_count, &rtl8196e_uart_phantom_count_ops, NULL, 0444);
MODULE_PARM_DESC(phantom_count, "Phantom RX-timeout recoveries since boot");

static bool phantom_log;
module_param(phantom_log, bool, 0644);
MODULE_PARM_DESC(phantom_log,
		 "Log deferred phantom RX-timeout summaries (default off; sysfs: Y/N, 1/0 also accepted)");

/*
 * RTL8196E UART Flow Control Register
 * Physical address: 0x18002110 == UART1 base (0x18002100) + 0x10 == MCR
 *                   with regshift=2 (reg 4 << 2).
 *
 * This 32-bit word at 0x18002110 is the MCR register seen through the
 * big-endian byte-lane routing of the SoC bus: writeb() by the 8250
 * core lands in bits 31:24 (the MCR byte), and bit 29 of the 32-bit
 * word is UART_MCR_AFE (bit 5 of the MCR byte). Our readl/writel RMW
 * preserves DTR/RTS/OUT2 (bits 24/25/27) that the core writes.
 *
 * Validated on hardware (2026-04-23) via devmem cycles:
 *   - boot:             0x2B000000 (DTR|RTS|OUT2|AFE)
 *   - stty -crtscts:    0x0B000000 (AFE cleared by core writeb)
 *   - stty crtscts:     0x2B000000 (AFE restored by our set_termios)
 *
 * Bit 29: Hardware Flow Control Enable (MCR_AFE alias)
 *   0 = Disabled (default) - causes UART overruns
 *   1 = Enabled - proper RTS/CTS operation
 */
#define RTL8196E_UART_FLOW_CTRL_OFFSET		0x10	/* reg 4 (MCR) << regshift=2 */
#define RTL8196E_UART_FLOW_CTRL_BIT		BIT(29)

/*
 * Full MCR pattern enforced when CRTSCTS is active: DTR|RTS|OUT2|AFE,
 * shifted into bits 24..31 by the SoC's byte-lane routing (see comment
 * above). This is the boot-time value 0x2B000000 documented at the top
 * of the file; we re-enforce it on every enable_flow_control() call.
 */
#define RTL8196E_UART_MCR_PATTERN	\
	(((u32)(UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2 | UART_MCR_AFE)) << 24)

/*
 * PIN_MUX_SEL register (offset 0x40 in system controller).
 * Bits 1, 3, 6 must be set for UART1 TXD/RXD signals to reach the
 * physical pins.  Without this, the UART peripheral works internally
 * (THRE fires, DMA runs) but no electrical signal reaches the EFR32.
 */
#define RTL8196E_PIN_MUX_UART1_BITS		(BIT(1) | BIT(3) | BIT(6))

/**
 * struct rtl8196e_uart_data - Private data for RTL8196E UART
 * @line: UART line number assigned by serial core
 * @clk: Optional clock for UART
 * @flow_ctrl_base: Virtual address of flow control register
 * @supports_afe: True if auto-flow-control is enabled in DT
 * @dev: Parent device used for logging and managed resources
 * @syscon: System-controller regmap used for UART1 pin muxing
 * @saved_pin_mux: Pin-mux bits to restore on probe failure or driver removal
 * @pin_mux_configured: True after the UART1 pin mux has been changed
 * @stuck_iir_cnt: Per-device count of phantom RX-timeout recoveries
 * @phantom_log_work: Deferred logging for phantom RX timeouts
 * @phantom_iir: Most recent phantom interrupt-identification value
 * @phantom_lsr: Most recent phantom line-status value
 */
struct rtl8196e_uart_data {
	int line;
	struct clk *clk;
	void __iomem *flow_ctrl_base;
	bool supports_afe;
	struct device *dev;
	struct regmap *syscon;
	u32 saved_pin_mux;
	bool pin_mux_configured;
	atomic64_t stuck_iir_cnt;
	struct work_struct phantom_log_work;
	u8 phantom_iir;
	u8 phantom_lsr;
};

/**
 * rtl8196e_uart_enable_flow_control() - Enable hardware flow control
 * @port: UART port (used to take port->lock around the RMW)
 * @data: RTL8196E UART private data
 *
 * Configures the RTL8196E-specific hardware flow control register.
 * This is REQUIRED for proper RTS/CTS operation - setting CRTSCTS
 * in termios alone is not sufficient on this SoC.
 *
 * Locking: takes port->lock with IRQ disabled around the readl/writel.
 * Without this, the 8250 core's byte-level MCR writes (e.g. from
 * serial8250_set_mctrl, serial8250_em485_stop_tx) can clobber bit 29
 * (AFE) between our readl() and writel() — confirmed cause of UART RX
 * FIFO overruns under bursty load at 460800 (issue #89).
 */
static void rtl8196e_uart_enable_flow_control(struct uart_port *port,
					      struct rtl8196e_uart_data *data)
{
	unsigned long flags;
	u32 reg_val;

	if (!data->flow_ctrl_base) {
		dev_warn(data->dev, "flow control register not mapped\n");
		return;
	}

	uart_port_lock_irqsave(port, &flags);
	/*
	 * Force the full MCR pattern (DTR|RTS|OUT2|AFE) on every call: the
	 * 8250 core's byte-wise MCR writes during set_termios preserve AFE
	 * but stomp DTR/RTS/OUT2 to its mctrl shadow, leaving the SoC at
	 * MCR=0x20000000 (AFE only). With RTS clear under AFE the SoC
	 * asserts !RTS to the EFR32 → RCP Spinel responses throttle →
	 * otbr-agent times out. The earlier "skip if AFE already set"
	 * fast-path masked this — we must re-OR every time.
	 */
	reg_val = readl(data->flow_ctrl_base);
	reg_val |= RTL8196E_UART_MCR_PATTERN;
	writel(reg_val, data->flow_ctrl_base);
	/* Read back under lock to verify atomically */
	reg_val = readl(data->flow_ctrl_base);
	if ((reg_val & RTL8196E_UART_MCR_PATTERN) ==
	    RTL8196E_UART_MCR_PATTERN)
		port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;
	else
		port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	uart_port_unlock_irqrestore(port, flags);

	if ((reg_val & RTL8196E_UART_MCR_PATTERN) == RTL8196E_UART_MCR_PATTERN) {
		dev_dbg(data->dev, "HW flow control enforced (reg=0x%08x)\n",
			reg_val);
	} else {
		dev_err(data->dev,
			"MCR pattern incomplete: 0x%08x (want 0x%08x set)\n",
			reg_val, RTL8196E_UART_MCR_PATTERN);
	}
}

/**
 * rtl8196e_uart_disable_flow_control() - Disable hardware flow control
 * @port: UART port (used to take port->lock around the RMW)
 * @data: RTL8196E UART private data
 *
 * Disables the RTL8196E-specific hardware flow control register.
 * Called when CRTSCTS is removed from termios.
 *
 * Locking: same rationale as enable_flow_control — see comment there.
 */
static void rtl8196e_uart_disable_flow_control(struct uart_port *port,
					       struct rtl8196e_uart_data *data)
{
	unsigned long flags;
	u32 reg_val;

	if (!data->flow_ctrl_base) {
		dev_warn(data->dev, "flow control register not mapped\n");
		return;
	}

	uart_port_lock_irqsave(port, &flags);
	reg_val = readl(data->flow_ctrl_base);
	if (!(reg_val & RTL8196E_UART_FLOW_CTRL_BIT)) {
		port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
		uart_port_unlock_irqrestore(port, flags);
		dev_dbg(data->dev, "HW flow control already disabled (0x%08x)\n",
			reg_val);
		return;
	}
	reg_val &= ~RTL8196E_UART_FLOW_CTRL_BIT;
	writel(reg_val, data->flow_ctrl_base);
	/* Read back under lock to verify atomically */
	reg_val = readl(data->flow_ctrl_base);
	if (!(reg_val & RTL8196E_UART_FLOW_CTRL_BIT))
		port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	uart_port_unlock_irqrestore(port, flags);

	if (!(reg_val & RTL8196E_UART_FLOW_CTRL_BIT)) {
		dev_dbg(data->dev, "HW flow control disabled (reg=0x%08x)\n",
			reg_val);
	} else {
		dev_err(data->dev, "Failed to disable HW flow control!\n");
	}
}

/**
 * rtl8196e_uart_set_divisor() - Custom divisor programmer
 * @port: UART port
 * @baud: target baud rate
 * @quot: divisor computed by the 8250 core (clock / (16 * baud))
 * @quot_frac: fractional divisor (unused on this SoC)
 *
 * The RTL8196E UART interprets the value written to DLL/DLM as (N + 1),
 * not N like a textbook 16550A. Evidence:
 *   - The stock Realtek bootloader uses `divisor = clock/16/baud - 1`
 *     (see 31-Bootloader/boot/uart.c).
 *   - Leaving it alone produces usable baud at 115200/230400 (error
 *     <1.4%, within tolerance) but catastrophic ~3% error at 460800,
 *     manifesting as ~40% framing errors on the wire.
 *   - Programming `quot - 1` restores a 0.47% error at 460800 and
 *     matches what the RTL hardware actually emits — verified live by
 *     arming the bridge at a fake baud such that the unpatched core
 *     programmed quot-1 and observing FE=0.
 *
 * Compensate by programming (quot - 1) so the hardware ends up at the
 * requested baud rate.
 */
static void rtl8196e_uart_set_divisor(struct uart_port *port, unsigned int baud,
				      unsigned int quot, unsigned int quot_frac)
{
	unsigned int adjusted = quot ? quot - 1 : 0;

	(void)quot_frac;
	serial8250_do_set_divisor(port, baud, adjusted);
}

/**
 * rtl8196e_uart_set_termios() - Custom set_termios handler
 * @port: UART port
 * @termios: New termios settings
 * @old: Old termios settings
 *
 * This function is called whenever termios settings change (via tcsetattr/stty).
 * It synchronizes the RTL8196E hardware flow control register (bit 29) with
 * the CRTSCTS flag.
 *
 * The sync is on the *absolute* CRTSCTS state, not on transitions. The
 * automatic-flow status bits tell serial_core not to manipulate RTS in
 * software while AFE owns it. Our throttle callbacks instead stop draining
 * RX so the FIFO threshold makes hardware deassert RTS.
 */
static void rtl8196e_uart_set_termios(struct uart_port *port,
				      struct ktermios *termios,
				      const struct ktermios *old)
{
	struct rtl8196e_uart_data *data = port->private_data;

	/*
	 * Let the 8250 core program baud/LCR/AFE; we only mirror the SoC
	 * flow-control gate (bit 29) after this.
	 */
	serial8250_do_set_termios(port, termios, old);

	/* Only manage HW flow control if AFE is supported */
	if (!data || !data->supports_afe)
		return;

	if (termios->c_cflag & CRTSCTS)
		rtl8196e_uart_enable_flow_control(port, data);
	else
		rtl8196e_uart_disable_flow_control(port, data);
}

static void rtl8196e_uart_throttle(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	guard(serial8250_rpm)(up);
	guard(uart_port_lock_irqsave)(port);
	port->ops->stop_rx(port);
}

static void rtl8196e_uart_unthrottle(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	guard(serial8250_rpm)(up);
	guard(uart_port_lock_irqsave)(port);
	up->ier |= UART_IER_RLSI | UART_IER_RDI;
	serial_port_out(port, UART_IER, up->ier);
}

static int rtl8196e_uart_startup(struct uart_port *port)
{
	struct rtl8196e_uart_data *data = port->private_data;
	struct uart_8250_port *up = up_to_u8250p(port);
	int ret;

	/* Apply the erratum-safe trigger before the core programs the FIFO. */
	up->fcr = UART_FCR_ENABLE_FIFO | UART_FCR_R_TRIG_00;

	ret = serial8250_do_startup(port);
	if (ret)
		return ret;

	/* Protect RX between startup and the first set_termios callback. */
	if (data && data->supports_afe)
		rtl8196e_uart_enable_flow_control(port, data);

	return 0;
}

static void rtl8196e_uart_shutdown(struct uart_port *port)
{
	struct rtl8196e_uart_data *data = port->private_data;
	struct uart_8250_port *up = up_to_u8250p(port);

	if (data && data->supports_afe) {
		unsigned long flags;

		uart_port_lock_irqsave(port, &flags);
		up->mcr &= ~UART_MCR_AFE;
		uart_port_unlock_irqrestore(port, flags);
		rtl8196e_uart_disable_flow_control(port, data);
	}
	serial8250_do_shutdown(port);
}

static void rtl8196e_uart_phantom_log_work(struct work_struct *work)
{
	struct rtl8196e_uart_data *data =
		container_of(work, struct rtl8196e_uart_data, phantom_log_work);

	if (!READ_ONCE(phantom_log))
		return;

	dev_info_ratelimited(data->dev,
		"stuck RX-timeout IIR with empty RX FIFO - cleared by dummy read (iir=0x%02x lsr=0x%02x, occurrence #%lld)\n",
		READ_ONCE(data->phantom_iir), READ_ONCE(data->phantom_lsr),
		atomic64_read(&data->stuck_iir_cnt));
}

/*
 * Stuck RX-timeout IIR recovery — the root cause of the multi-day field
 * soft-lockups (captured live by the panic-record flight recorder).
 *
 * Failure mode, captured on hardware: IIR reads 0xCC — FIFOs enabled,
 * interrupt PENDING, ID = Character Timeout ("RX bytes have been waiting
 * longer than the timeout") — while LSR reads 0x60 with Data Ready CLEAR
 * ("RX FIFO empty"). The two registers contradict each other and the state
 * is stable: the 8250 core gates its RX drain on LSR_DR, so it reads
 * nothing, the timeout condition is never cleared, the handler returns
 * IRQ_HANDLED, and the level-triggered line re-asserts after every eret.
 * Each rotation costs ~700 µs of MMIO on this 200 MHz bus (~1360
 * rotations/s = ~95 % of the CPU in hardirq): the timer wheel freezes, NAPI
 * never runs, no schedule point is ever reached, and the softlockup
 * detector panics the box after 20 s.
 *
 * The remedy is the classic 16550-clone workaround (mainline precedent:
 * dw8250_handle_irq, "there are ways to get Designware-based UARTs into a
 * state where they are asserting UART_IIR_RX_TIMEOUT but there is no actual
 * data available ... If we don't do this then the 'RX TIMEOUT' interrupt
 * will fire forever"): when the RX-timeout ID shows with an empty LSR, do
 * one dummy RBR read — it flushes the phantom byte the FIFO timeout counter
 * believes it holds and clears the latch.
 *
 * The recovery is silent by default, like the dw8250 precedent: a log line
 * emitted from here reaches a legacy (non-nbcon) console whose record write
 * runs with interrupts disabled — ~38 ms per line at the 38400-baud console,
 * long enough to overrun the peer's RX path on a board without RTS/CTS
 * (measured in the field: the warning itself caused the very overruns it
 * was suspected of witnessing). phantom_count keeps the per-boot total
 * observable in sysfs; setting phantom_log to Y (the canonical sysfs
 * spelling; 1 is also accepted by the bool module-parameter parser)
 * restores the timestamped per-occurrence line at runtime when a field
 * incident needs time correlation.
 */
static int rtl8196e_uart_handle_irq(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct rtl8196e_uart_data *data = port->private_data;
	unsigned int iir = serial_port_in(port, UART_IIR);

	guard(uart_port_lock_check_sysrq_irqsave)(port);

	if ((iir & 0x3f) == UART_IIR_RX_TIMEOUT) {
		u16 lsr = serial_lsr_in(up);

		if (!(lsr & (UART_LSR_DR | UART_LSR_BI))) {
			(void)serial_port_in(port, UART_RX);
			atomic64_inc(&phantom_count);
			if (data) {
				atomic64_inc(&data->stuck_iir_cnt);
				if (READ_ONCE(phantom_log)) {
					WRITE_ONCE(data->phantom_iir, iir);
					WRITE_ONCE(data->phantom_lsr, lsr);
					schedule_work(&data->phantom_log_work);
				}
			}
		}
	}

	if (iir & UART_IIR_NO_INT)
		return 0;

	serial8250_handle_irq_locked(port, iir);
	return 1;
}

/**
 * rtl8196e_uart_probe() - Probe and initialize RTL8196E UART
 * @pdev: Platform device
 *
 * Initializes the UART port and configures RTL8196E-specific features.
 *
 * Return: 0 on success, negative error code on failure
 */
static int rtl8196e_uart_probe(struct platform_device *pdev)
{
	struct uart_8250_port uart = {};
	struct rtl8196e_uart_data *data;
	struct resource *regs;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = &pdev->dev;
	atomic64_set(&data->stuck_iir_cnt, 0);
	INIT_WORK(&data->phantom_log_work, rtl8196e_uart_phantom_log_work);

	/* Map the UART register window WITHOUT claiming the mem region:
	 * the 8250 core claims it itself in serial8250_config_port()
	 * (serial8250_request_std_resource). A devm request here made that
	 * claim fail with -EBUSY and config_port bail out early, which left
	 * up->fcr at 0 (FIFO genuinely disabled on the wire — confirmed via
	 * IIR bits 7:6 = 00) and the rx_trig_bytes sysfs knob unregistered.
	 * Audit 8250RTL-007; plain devm_ioremap is the standard 8250-glue
	 * idiom (cf. 8250_dw).
	 */
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs) {
		dev_err(&pdev->dev, "no UART register resource\n");
		return -EINVAL;
	}
	if (resource_size(regs) <
	    RTL8196E_UART_FLOW_CTRL_OFFSET + sizeof(u32)) {
		dev_err(&pdev->dev, "UART register resource is too small\n");
		return -EINVAL;
	}
	uart.port.membase = devm_ioremap(&pdev->dev, regs->start,
					 resource_size(regs));
	if (!uart.port.membase) {
		dev_err(&pdev->dev, "Failed to map UART registers\n");
		return -ENOMEM;
	}

	/* flow_ctrl_base is an alias on MCR; assigned after struct init below */

	/* Ensure UART1 pins are muxed to the UART peripheral via syscon.
	 * Audit 8250RTL-001: syscon is required on this SoC — without the
	 * pinmux, UART1 may appear as a usable ttyS1 internally but the
	 * RX/TX signals never reach the EFR32. Treat lookup failure as
	 * fatal (other than -EPROBE_DEFER) rather than warn-and-continue,
	 * so we surface the misconfig clearly instead of producing a
	 * silently-broken serial link.
	 */
	{
		data->syscon = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							       "realtek,syscon");
		if (IS_ERR(data->syscon)) {
			ret = PTR_ERR(data->syscon);
			if (ret != -EPROBE_DEFER)
				dev_err(&pdev->dev,
					"syscon lookup failed (%d), cannot mux UART1 pins\n",
					ret);
			return ret;
		}
		ret = regmap_read(data->syscon, 0x40, &data->saved_pin_mux);
		if (ret) {
			dev_err(&pdev->dev, "pin mux read failed: %d\n", ret);
			return ret;
		}
		ret = regmap_update_bits(data->syscon, 0x40,
					 RTL8196E_PIN_MUX_UART1_BITS,
					 RTL8196E_PIN_MUX_UART1_BITS);
		if (ret) {
			dev_err(&pdev->dev, "pin mux write failed: %d\n", ret);
			return ret;
		}
		data->pin_mux_configured = true;
	}

	/* Optional: Get clock if specified in DT */
	data->clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(data->clk)) {
		ret = PTR_ERR(data->clk);
		goto err_pin_mux_restore;
	}
	if (data->clk) {
		ret = clk_prepare_enable(data->clk);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable clock: %d\n", ret);
			goto err_pin_mux_restore;
		}
	}

	/* Initialize uart_8250_port structure */
	uart.port.dev = &pdev->dev;
	uart.port.type = PORT_16550A;
	uart.port.iotype = UPIO_MEM;
	uart.port.mapbase = regs->start;
	uart.port.mapsize = resource_size(regs);
	uart.port.regshift = 2;  /* 32-bit aligned registers on 8196E */
	uart.port.private_data = data;

	/* Install custom set_termios handler for dynamic flow control */
	uart.port.set_termios = rtl8196e_uart_set_termios;

	/* Install custom divisor programmer (compensates for the RTL's N+1 quirk) */
	uart.port.set_divisor = rtl8196e_uart_set_divisor;

	/* Let hardware AFE throttle by filling the FIFO instead of clearing RTS. */
	uart.port.throttle = rtl8196e_uart_throttle;
	uart.port.unthrottle = rtl8196e_uart_unthrottle;
	uart.port.startup = rtl8196e_uart_startup;
	uart.port.shutdown = rtl8196e_uart_shutdown;

	/* Install the stuck-IIR interrupt handler (storm guard) — see the
	 * rtl8196e_uart_handle_irq() comment block. The template field IS
	 * copied by serial8250_register_8250_port() (8250_core.c checks
	 * up->port.handle_irq explicitly), unlike .fcr (audit 8250RTL-006).
	 */
	uart.port.handle_irq = rtl8196e_uart_handle_irq;

	/* Get IRQ from device tree */
	ret = platform_get_irq(pdev, 0);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get IRQ: %d\n", ret);
		goto err_clk_disable;
	}
	uart.port.irq = ret;

	/* Get clock frequency: DT property, then clock framework. */
	if (of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				 &uart.port.uartclk)) {
		if (data->clk)
			uart.port.uartclk = clk_get_rate(data->clk);
		if (!uart.port.uartclk) {
			dev_err(&pdev->dev, "UART clock rate is unavailable\n");
			ret = -EINVAL;
			goto err_clk_disable;
		} else {
			dev_info(&pdev->dev, "uartclk: %u Hz (clock framework)\n",
				 uart.port.uartclk);
		}
	}

	/* flow_ctrl_base aliases MCR (see header comment). */
	data->flow_ctrl_base = uart.port.membase + RTL8196E_UART_FLOW_CTRL_OFFSET;

	/* Set UART capabilities */
	uart.capabilities = UART_CAP_FIFO;

	/* Enable AFE (Automatic Flow Control) if requested in DT */
	if (of_property_read_bool(pdev->dev.of_node, "auto-flow-control") ||
	    of_property_read_bool(pdev->dev.of_node, "uart-has-rtscts")) {
		uart.capabilities |= UART_CAP_AFE;
		data->supports_afe = true;
	} else {
		data->supports_afe = false;
	}

	/* Configure FIFO. No .fcr on purpose: the template field is never
	 * copied by serial8250_register_8250_port() (audit 8250RTL-006).
	 * The startup callback pins the effective FCR before the core programs
	 * the hardware.
	 */
	uart.port.fifosize = 16;
	uart.tx_loadsz = 16;

	/* Set port flags */
	uart.port.flags = UPF_FIXED_PORT | UPF_FIXED_TYPE;

	/* Force line 1 (ttyS1) to not steal ttyS0 from console uart0 */
	uart.port.line = 1;

	/* Register the port with 8250 subsystem */
	ret = serial8250_register_8250_port(&uart);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register 8250 port: %d\n", ret);
		goto err_clk_disable;
	}

	data->line = ret;
	/* Audit 8250RTL-002: ttyS1 is a platform contract — rtl8196e-uart-bridge,
	 * S50uart_bridge, S70otbr, and radio.conf all assume /dev/ttyS1. If the
	 * core registered us as a different line (e.g. ttyS1 was already taken
	 * by some other registration), fail probe explicitly instead of leaving
	 * a silently mis-wired bridge.
	 */
	if (ret != 1) {
		dev_err(&pdev->dev, "registered as ttyS%d, expected ttyS1\n", ret);
		serial8250_unregister_port(data->line);
		ret = -EBUSY;
		goto err_clk_disable;
	}

	/* The startup callback pins the RX FIFO trigger to 1 (R_TRIG_00),
	 * overriding the PORT_16550A table default (trigger 8) that config_port
	 * just set.
	 * Hardware erratum (audit 8250RTL-009): on this UART clone, RX
	 * trigger levels above 1 produce erratic overruns under sustained
	 * line-rate load, non-monotonic in the trigger value (loopback
	 * bench 2026-06-12, 20 s flood, flow control off:
	 *   892857: trig1 oe=0, trig4 oe=548, trig8 oe=53, trig14 oe=2
	 *   460800: trig1 oe=0, trig8 oe=3,  trig14 oe=2088).
	 * Trigger 1 with the FIFO enabled keeps the full 16-byte cushion
	 * against IRQ latency and is wire-identical to the long-proven
	 * v3.x behaviour (FCR=0 on this clone kept the FIFO alive with an
	 * effective trigger of 1 — see 8250RTL-007). The cost, ~1 IRQ per
	 * 1-2 RX bytes at line rate, is the historical baseline. The
	 * rx_trig_bytes sysfs knob stays writable for experiments.
	 */
	platform_set_drvdata(pdev, data);

	dev_info(&pdev->dev,
		 "v" DRV_VERSION " (J. Nilo) - ttyS%d @ %u baud-clk, IRQ %d, FIFO %d, AFE %s\n",
		 data->line, uart.port.uartclk, uart.port.irq,
		 uart.port.fifosize, data->supports_afe ? "on" : "off");

	return 0;

err_clk_disable:
	cancel_work_sync(&data->phantom_log_work);
	if (data->clk)
		clk_disable_unprepare(data->clk);
err_pin_mux_restore:
	if (data->pin_mux_configured) {
		int restore_ret;

		restore_ret = regmap_update_bits(data->syscon, 0x40,
						 RTL8196E_PIN_MUX_UART1_BITS,
						 data->saved_pin_mux);
		if (restore_ret)
			dev_warn(&pdev->dev, "pin mux restore failed: %d\n",
				 restore_ret);
		data->pin_mux_configured = false;
	}
	return ret;
}

/**
 * rtl8196e_uart_remove() - Remove RTL8196E UART
 * @pdev: Platform device
 *
 * Return: 0 on success
 */
static void rtl8196e_uart_remove(struct platform_device *pdev)
{
	struct rtl8196e_uart_data *data = platform_get_drvdata(pdev);

	serial8250_unregister_port(data->line);
	cancel_work_sync(&data->phantom_log_work);

	if (data->clk)
		clk_disable_unprepare(data->clk);

	if (data->pin_mux_configured) {
		int ret;

		ret = regmap_update_bits(data->syscon, 0x40,
					 RTL8196E_PIN_MUX_UART1_BITS,
					 data->saved_pin_mux);
		if (ret)
			dev_warn(&pdev->dev, "pin mux restore failed: %d\n", ret);
	}
}

/* Device tree match table */
static const struct of_device_id rtl8196e_uart_of_match[] = {
	{ .compatible = "realtek,rtl8196e-uart" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtl8196e_uart_of_match);

/* Platform driver structure */
static struct platform_driver rtl8196e_uart_driver = {
	.probe = rtl8196e_uart_probe,
	.remove = rtl8196e_uart_remove,
	.driver = {
		.name = "rtl8196e-uart",
		.of_match_table = rtl8196e_uart_of_match,
	},
};

module_platform_driver(rtl8196e_uart_driver);

MODULE_AUTHOR("Jacques Nilo");
MODULE_DESCRIPTION("Realtek RTL8196E UART driver with hardware flow control");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
