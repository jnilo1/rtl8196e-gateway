// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL8196E hardware watchdog.
 *
 * The normal path is deliberately small: start, stop and ping are one MMIO
 * write.  The panic path first arms a short hardware reset, then writes a
 * bounded post-mortem record to a dedicated no-map DRAM page.	It must never
 * allocate, walk kernel lists, sample periodically, or depend on another
 * driver to make recovery happen.
 */

#include <linux/bitops.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/watchdog.h>

#include <asm/irq_regs.h>
#include <asm/mipsregs.h>
#include <asm/ptrace.h>

#define DRIVER_NAME		       "rtl819x-wdt"
#define DRV_VERSION			"1.12"

/* WDTCNR layout, RTL8196E-CG datasheet table 27. */
#define WDTE_SHIFT			24
#define WDTE_MASK			(0xffU << WDTE_SHIFT)
#define WDTE_STOP			(0xa5U << WDTE_SHIFT)
#define WDTCLR				BIT(23)
#define WDIND				BIT(20)
#define WDT_OVSEL(sel)			((((u32)(sel) & 0x3U) << 21) | \
					 ((((u32)(sel) >> 2) & 0x3U) << 17))
#define WDT_OVSEL_MAX			WDT_OVSEL(0x9)
#define WDT_ENABLE_PATTERN		WDT_OVSEL_MAX
#define WDT_DISABLE_PATTERN		(WDTE_STOP | WDT_OVSEL_MAX)

/* Timer clock is 25 kHz; OVSEL=9 is about 671 s. */
#define WDT_TIMEOUT_SECS_DEFAULT	60U
#define WDT_TIMEOUT_SECS_MIN		1U
#define WDT_HW_WINDOW_MS		671000U

/*
 * Record v9: a fixed, self-contained format.  The magic is written last;
 * the boot reader accepts only this exact version and length.	The reason is
 * sanitized before both persistence and logging, avoiding control characters
 * or an unterminated string in the next boot's dmesg.
 */
#define WDT_REC_MAGIC			0x50414e43U /* "PANC" */
#define WDT_REC_VERSION			9U
#define WDT_REC_REASON_LEN		64U
#define WDT_REC_OFF_MAGIC		0U
#define WDT_REC_OFF_VERSION		4U
#define WDT_REC_OFF_LENGTH		8U
#define WDT_REC_OFF_UPTIME		12U
#define WDT_REC_OFF_EPC			16U
#define WDT_REC_OFF_RA			20U
#define WDT_REC_OFF_CAUSE		24U
#define WDT_REC_OFF_STATUS		28U
#define WDT_REC_OFF_SOFTIRQ		32U
#define WDT_REC_OFF_WDTCNR		36U
#define WDT_REC_OFF_FLAGS		40U
#define WDT_REC_OFF_REASON		44U
#define WDT_REC_SIZE			(WDT_REC_OFF_REASON + WDT_REC_REASON_LEN)
#define WDT_REC_FLAG_IRQ_REGS		BIT(0)

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct rtl819x_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
	void __iomem *rec;
	struct notifier_block panic_nb;
};

static inline struct rtl819x_wdt *to_rtl819x_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct rtl819x_wdt, wdd);
}

static void rtl819x_wdt_arm_reset(struct rtl819x_wdt *wdt)
{
	/*
	 * Clear while halted, then enable OVSEL=0.  A single enable against
	 * a counter left running in the long OVSEL=9 bucket can overflow
	 * immediately and lose the post-mortem write (WDT-010).
	 */
	writel(WDT_DISABLE_PATTERN | WDTCLR, wdt->base);
	writel(0, wdt->base);
}

static int rtl819x_wdt_start(struct watchdog_device *wdd)
{
	writel(WDT_ENABLE_PATTERN | WDTCLR, to_rtl819x_wdt(wdd)->base);
	return 0;
}

static int rtl819x_wdt_stop(struct watchdog_device *wdd)
{
	writel(WDT_DISABLE_PATTERN, to_rtl819x_wdt(wdd)->base);
	return 0;
}

static int rtl819x_wdt_ping(struct watchdog_device *wdd)
{
	/* Constant write: no MMIO read and no accidental WDIND W1C. */
	writel(WDT_ENABLE_PATTERN | WDTCLR, to_rtl819x_wdt(wdd)->base);
	return 0;
}

static int rtl819x_wdt_set_timeout(struct watchdog_device *wdd,
				   unsigned int timeout)
{
	/* Hardware always uses OVSEL=9; timeout controls the feeder cadence. */
	wdd->timeout = timeout;
	return 0;
}

static int rtl819x_wdt_restart(struct watchdog_device *wdd,
			       unsigned long action, void *data)
{
	struct rtl819x_wdt *wdt = to_rtl819x_wdt(wdd);

	(void)action;
	(void)data;
	rtl819x_wdt_arm_reset(wdt);
	mdelay(50);
	return 0;
}

static void rtl819x_wdt_sanitize_reason(char reason[WDT_REC_REASON_LEN])
{
	size_t i;

	reason[WDT_REC_REASON_LEN - 1] = '\0';
	for (i = 0; i < WDT_REC_REASON_LEN - 1 && reason[i]; i++)
		if (!isprint((unsigned char)reason[i]))
			reason[i] = ' ';
}

static void rtl819x_wdt_reason(char dst[WDT_REC_REASON_LEN], const char *src)
{
	memset(dst, 0, WDT_REC_REASON_LEN);
	if (!src)
		src = "panic";
	strscpy(dst, src, WDT_REC_REASON_LEN);
	rtl819x_wdt_sanitize_reason(dst);
}

static void rtl819x_wdt_write_record(struct rtl819x_wdt *wdt, const char *why)
{
	struct pt_regs *regs;
	char reason[WDT_REC_REASON_LEN];
	u32 flags = 0;

	if (!wdt->rec)
		return;

	regs = get_irq_regs();
	if (regs)
		flags |= WDT_REC_FLAG_IRQ_REGS;
	rtl819x_wdt_reason(reason, why);

	/* Invalidate an older record before constructing the new one. */
	writel(0, wdt->rec + WDT_REC_OFF_MAGIC);
	writel(WDT_REC_VERSION, wdt->rec + WDT_REC_OFF_VERSION);
	writel(WDT_REC_SIZE, wdt->rec + WDT_REC_OFF_LENGTH);
	writel((u32)div_u64(ktime_get_boot_fast_ns(), NSEC_PER_SEC),
	       wdt->rec + WDT_REC_OFF_UPTIME);
	writel(regs ? (u32)regs->cp0_epc : 0, wdt->rec + WDT_REC_OFF_EPC);
	writel(regs ? (u32)regs->regs[31] : 0, wdt->rec + WDT_REC_OFF_RA);
	writel(read_c0_cause(), wdt->rec + WDT_REC_OFF_CAUSE);
	writel(read_c0_status(), wdt->rec + WDT_REC_OFF_STATUS);
	writel((u32)local_softirq_pending(), wdt->rec + WDT_REC_OFF_SOFTIRQ);
	writel(readl(wdt->base), wdt->rec + WDT_REC_OFF_WDTCNR);
	writel(flags, wdt->rec + WDT_REC_OFF_FLAGS);
	memset_io(wdt->rec + WDT_REC_OFF_REASON, 0, WDT_REC_REASON_LEN);
	memcpy_toio(wdt->rec + WDT_REC_OFF_REASON, reason, sizeof(reason));
	/* Complete all fields before making the record visible to the next boot. */
	wmb();
	writel(WDT_REC_MAGIC, wdt->rec + WDT_REC_OFF_MAGIC);
	/* Order the magic publication before any subsequent panic-path write. */
	wmb();
}

static int rtl819x_wdt_panic_notify(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct rtl819x_wdt *wdt = container_of(nb, struct rtl819x_wdt,
						panic_nb);

	(void)action;
	/* Recovery comes first; the fixed record write fits the 1.31 s grace. */
	rtl819x_wdt_arm_reset(wdt);
	rtl819x_wdt_write_record(wdt, data);
	return NOTIFY_DONE;
}

static void rtl819x_wdt_report_record(struct rtl819x_wdt *wdt)
{
	struct device *dev = wdt->wdd.parent;
	char reason[WDT_REC_REASON_LEN];
	u32 version, length;

	if (!wdt->rec || readl(wdt->rec + WDT_REC_OFF_MAGIC) != WDT_REC_MAGIC)
		return;

	version = readl(wdt->rec + WDT_REC_OFF_VERSION);
	length = readl(wdt->rec + WDT_REC_OFF_LENGTH);
	if (version != WDT_REC_VERSION || length != WDT_REC_SIZE) {
		dev_warn(dev, "discarding unsupported panic record v%u len=%u\n",
			 version, length);
		writel(0, wdt->rec + WDT_REC_OFF_MAGIC);
		return;
	}

	memcpy_fromio(reason, wdt->rec + WDT_REC_OFF_REASON, sizeof(reason));
	reason[WDT_REC_REASON_LEN - 1] = '\0';
	rtl819x_wdt_sanitize_reason(reason);
	dev_warn(dev,
		 "previous panic: uptime=%us epc=%pS ra=%pS cause=0x%08x status=0x%08x softirq=0x%08x wdt=0x%08x flags=0x%x reason=\"%s\"\n",
		 readl(wdt->rec + WDT_REC_OFF_UPTIME),
		 (void *)(uintptr_t)readl(wdt->rec + WDT_REC_OFF_EPC),
		 (void *)(uintptr_t)readl(wdt->rec + WDT_REC_OFF_RA),
		 readl(wdt->rec + WDT_REC_OFF_CAUSE),
		 readl(wdt->rec + WDT_REC_OFF_STATUS),
		 readl(wdt->rec + WDT_REC_OFF_SOFTIRQ),
		 readl(wdt->rec + WDT_REC_OFF_WDTCNR),
		 readl(wdt->rec + WDT_REC_OFF_FLAGS), reason);
	writel(0, wdt->rec + WDT_REC_OFF_MAGIC);
}

static void rtl819x_wdt_panic_unregister(void *data)
{
	struct rtl819x_wdt *wdt = data;

	atomic_notifier_chain_unregister(&panic_notifier_list, &wdt->panic_nb);
}

static const struct watchdog_info rtl819x_wdt_info = {
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
	.identity	= DRIVER_NAME,
};

static const struct watchdog_ops rtl819x_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= rtl819x_wdt_start,
	.stop		= rtl819x_wdt_stop,
	.ping		= rtl819x_wdt_ping,
	.set_timeout	= rtl819x_wdt_set_timeout,
	.restart	= rtl819x_wdt_restart,
};

static int rtl819x_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem = NULL;
	struct device_node *np;
	struct rtl819x_wdt *wdt;
	u32 raw;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;
	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	wdt->wdd.info = &rtl819x_wdt_info;
	wdt->wdd.ops = &rtl819x_wdt_ops;
	wdt->wdd.parent = dev;
	wdt->wdd.min_timeout = WDT_TIMEOUT_SECS_MIN;
	wdt->wdd.max_hw_heartbeat_ms = WDT_HW_WINDOW_MS;
	wdt->wdd.timeout = WDT_TIMEOUT_SECS_DEFAULT;
	watchdog_init_timeout(&wdt->wdd, 0, dev);
	watchdog_set_nowayout(&wdt->wdd, nowayout);
	watchdog_set_restart_priority(&wdt->wdd, 192);

	raw = readl(wdt->base);
	dev_info(dev, "last reset: %s (WDTCNR=0x%08x)\n",
		 (raw & WDIND) ? "watchdog timeout" : "power-on / pin reset",
		 raw);
	if (raw & WDIND)
		writel(raw | WDIND, wdt->base);
	raw = readl(wdt->base);
	if ((raw & WDTE_MASK) != WDTE_STOP)
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (np) {
		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
	}
	if (rmem && rmem->size >= WDT_REC_SIZE)
		wdt->rec = devm_ioremap(dev, rmem->base, WDT_REC_SIZE);
	if (!wdt->rec)
		dev_warn(dev, "no usable crash-record memory-region\n");
	rtl819x_wdt_report_record(wdt);

	ret = devm_watchdog_register_device(dev, &wdt->wdd);
	if (ret)
		return dev_err_probe(dev, ret, "watchdog registration failed\n");

	wdt->panic_nb.notifier_call = rtl819x_wdt_panic_notify;
	wdt->panic_nb.priority = INT_MAX;
	ret = atomic_notifier_chain_register(&panic_notifier_list,
					     &wdt->panic_nb);
	if (ret)
		return dev_err_probe(dev, ret, "panic notifier registration failed\n");
	ret = devm_add_action_or_reset(dev, rtl819x_wdt_panic_unregister, wdt);
	if (ret)
		return ret;

	dev_info(dev, "v" DRV_VERSION " record v%u timeout=%us nowayout=%d\n",
		 WDT_REC_VERSION, wdt->wdd.timeout, nowayout);
	return 0;
}

static const struct of_device_id rtl819x_wdt_of_match[] = {
	{ .compatible = "realtek,rtl8196e-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl819x_wdt_of_match);

static struct platform_driver rtl819x_wdt_driver = {
	.probe = rtl819x_wdt_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = rtl819x_wdt_of_match,
	},
};
module_platform_driver(rtl819x_wdt_driver);

MODULE_AUTHOR("Jacques Nilo");
MODULE_DESCRIPTION("Production watchdog for Realtek RTL8196E SoC");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
