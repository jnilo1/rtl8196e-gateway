// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO driver for Realtek RTL8196E SoC
 *
 * The RTL8196E has 4 GPIO ports (A, B, C, D) with 8 pins each = 32 GPIOs
 *   - Port A: GPIO 0-7
 *   - Port B: GPIO 8-15
 *   - Port C: GPIO 16-23
 *   - Port D: GPIO 24-31
 *
 * Register layout (GPIO base 0xB8003500):
 *   0x00: PABCD_CNR  - Port ABCD control (0=GPIO, 1=peripheral)
 *   0x04: PABCD_PTYPE - Port ABCD type
 *   0x08: PABCD_DIR  - Port ABCD direction (0=input, 1=output)
 *   0x0C: PABCD_DAT  - Port ABCD data
 *   0x10: PABCD_ISR  - Port ABCD interrupt status
 *   0x14: PAB_IMR    - Port AB interrupt mask
 *   0x18: PCD_IMR    - Port CD interrupt mask
 *
 * Pin muxing (RTL8196E specific - other chips may differ):
 *   PIN_MUX_SEL_2 (0x18000044) controls GPIO B2-B6 shared with LED_PORT0-4
 *   Bits must be set to 0b11 to enable GPIO mode for these pins.
 *
 * Note: Other RTL819x variants (RTL8196C, RTL8197F) may have different
 * pinmux register layouts. This driver is tested on RTL8196E only.
 *
 * The get/set/direction ops are provided by the generic MMIO GPIO core
 * (GPIO_GENERIC, gpio-mmio.c): DATA at 0x0C and DIR at 0x08 (1=out) are
 * exactly its single read/write register model, and its default
 * direction_output writes the value before flipping the direction —
 * the same glitch-free order the hand-rolled v1.x ops used. Only
 * `.request` is ours: it muxes the pad (PIN_MUX_SEL_2) and drops the
 * pin out of peripheral mode (CNR).
 *
 * Author: Jacques Nilo
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/generic.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/spinlock.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#define RTL819X_GPIO_REG_CNR    0x00    /* Control register */
#define RTL819X_GPIO_REG_PTYPE  0x04    /* Port type */
#define RTL819X_GPIO_REG_DIR    0x08    /* Direction: 0=in, 1=out */
#define RTL819X_GPIO_REG_DATA   0x0C    /* Data register */
#define RTL819X_GPIO_REG_ISR    0x10    /* Interrupt status */
/*
 * Two interrupt-mask registers, 16 bits per port (2 bits per pin) —
 * unused until an irqchip materializes (GPIO-004, deferred), declared
 * to match the hardware so the lone-define/header-comment drift does
 * not recur (GPIO-S07).
 */
#define RTL819X_GPIO_REG_PAB_IMR 0x14   /* Port A/B interrupt mask */
#define RTL819X_GPIO_REG_PCD_IMR 0x18   /* Port C/D interrupt mask */

/*
 * No local line-count constant: the generic MMIO core owns the effective
 * gc.ngpio (derived from the register width), and the banner reports that.
 * A private copy could only drift away from it. The hardware count is stated
 * once, in the port map at the top of this file.
 */

#define DRIVER_NAME             "gpio-rtl819x"
#define DRV_VERSION             "1.3"

struct rtl819x_gpio {
	struct gpio_generic_chip chip;
	void __iomem            *base;
	struct regmap           *syscon;    /* PIN_MUX_SEL_2 for LED/GPIO mux */
	spinlock_t              lock;       /* serializes the CNR RMW in .request */
};

/*
 * Configure PIN_MUX_SEL_2 for GPIO B2-B6 (shared with LED_PORT0-4)
 * RTL8196E datasheet Table 36:
 *   GPIO 10 (B2): bits 1:0  = 11 for GPIO mode
 *   GPIO 11 (B3): bits 4:3  = 11 for GPIO mode
 *   GPIO 12 (B4): bits 7:6  = 11 for GPIO mode
 *   GPIO 13 (B5): bits 10:9 = 11 for GPIO mode
 *   GPIO 14 (B6): bits 13:12 = 11 for GPIO mode
 */
static int rtl819x_gpio_configure_pinmux(struct rtl819x_gpio *rg, unsigned int offset)
{
	u32 mask = 0, bits = 0;

	switch (offset) {
	case 10: /* GPIO B2 - LED_PORT0 */
		mask = 0x3 << 0;
		bits = 0x3 << 0;
		break;
	case 11: /* GPIO B3 - LED_PORT1 */
		mask = 0x3 << 3;
		bits = 0x3 << 3;
		break;
	case 12: /* GPIO B4 - LED_PORT2 */
		mask = 0x3 << 6;
		bits = 0x3 << 6;
		break;
	case 13: /* GPIO B5 - LED_PORT3 */
		mask = 0x3 << 9;
		bits = 0x3 << 9;
		break;
	case 14: /* GPIO B6 - LED_PORT4 */
		mask = 0x3 << 12;
		bits = 0x3 << 12;
		break;
	default:
		return 0; /* No pinmux needed for other GPIOs */
	}

	/*
	 * Fail loudly when the pad cannot be muxed: without the syscon the
	 * request would otherwise succeed while the pin stays electrically
	 * in peripheral mode — a dead LED/button with no trace beyond one
	 * probe-time warning (GPIO-008).
	 */
	if (!rg->syscon) {
		dev_err(rg->chip.gc.parent,
			"GPIO %u needs PIN_MUX_SEL_2 but syscon is missing\n",
			offset);
		return -ENODEV;
	}

	return regmap_update_bits(rg->syscon, 0x44, mask, bits);
}

static int rtl819x_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct rtl819x_gpio *rg = gpiochip_get_data(gc);
	unsigned long flags;
	u32 val;
	int ret;

	/* Configure pinmux for GPIO B2-B6 (shared with LED ports) */
	ret = rtl819x_gpio_configure_pinmux(rg, offset);
	if (ret) {
		dev_err(gc->parent, "pinmux setup failed for GPIO %u: %d\n", offset, ret);
		return ret;
	}

	spin_lock_irqsave(&rg->lock, flags);

	/* Enable GPIO function (clear bit in CNR = GPIO mode) */
	val = readl(rg->base + RTL819X_GPIO_REG_CNR);
	val &= ~BIT(offset);
	writel(val, rg->base + RTL819X_GPIO_REG_CNR);

	spin_unlock_irqrestore(&rg->lock, flags);

	return 0;
}

static int rtl819x_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl819x_gpio *rg;
	int ret;

	rg = devm_kzalloc(dev, sizeof(*rg), GFP_KERNEL);
	if (!rg)
		return -ENOMEM;

	rg->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rg->base))
		return PTR_ERR(rg->base);

	/*
	 * Get syscon regmap for PIN_MUX_SEL_2 pinmux configuration.
	 *
	 * Every supported RTL8196E board inherits realtek,syscon from the shared
	 * rtl819x.dtsi, so a lookup failure is a broken device tree, not an
	 * optional feature: fail the probe instead of continuing with a partial
	 * bank. Collapsing every error to NULL also swallowed -EPROBE_DEFER,
	 * which would have turned a mere probe-ordering issue into permanently
	 * dead LED/button/EFR32-reset lines. dev_err_probe() keeps deferral
	 * quiet and propagates the original error either way.
	 */
	rg->syscon = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,syscon");
	if (IS_ERR(rg->syscon))
		return dev_err_probe(dev, PTR_ERR(rg->syscon),
				     "realtek,syscon lookup failed; GPIOs 10-14 (pads B2-B6) cannot be muxed\n");

	spin_lock_init(&rg->lock);

	{
		const struct gpio_generic_chip_config config = {
			.dev    = dev,
			.sz     = 4,
			.dat    = rg->base + RTL819X_GPIO_REG_DATA,
			.dirout = rg->base + RTL819X_GPIO_REG_DIR,
		};

		ret = gpio_generic_chip_init(&rg->chip, &config);
		if (ret) {
			dev_err(dev, "generic chip init failed: %d\n", ret);
			return ret;
		}
	}

	/*
	 * Post-init overrides: the generic core labels the chip after
	 * dev_name() and leaves base at 0 — keep the v1.x label and the
	 * dynamic base. `.request` stays ours (pinmux + CNR); everything
	 * else (get/set/directions, multiple variants for free) is the
	 * generic implementation.
	 */
	rg->chip.gc.label   = DRIVER_NAME;
	rg->chip.gc.owner   = THIS_MODULE;
	rg->chip.gc.request = rtl819x_gpio_request;
	rg->chip.gc.base    = -1;   /* dynamic — DT consumers use phandles */

	ret = devm_gpiochip_add_data(dev, &rg->chip.gc, rg);
	if (ret) {
		dev_err(dev, "failed to register gpio chip: %d\n", ret);
		return ret;
	}

	dev_info(dev, "v" DRV_VERSION " (J. Nilo) - %u GPIOs registered\n",
			 rg->chip.gc.ngpio);

	return 0;
}

static const struct of_device_id rtl819x_gpio_of_match[] = {
	{ .compatible = "realtek,rtl8196e-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtl819x_gpio_of_match);

static struct platform_driver rtl819x_gpio_driver = {
	.probe  = rtl819x_gpio_probe,
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = rtl819x_gpio_of_match,
	},
};

module_platform_driver(rtl819x_gpio_driver);

MODULE_AUTHOR("Jacques Nilo");
MODULE_DESCRIPTION("GPIO driver for Realtek RTL819x SoCs");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
