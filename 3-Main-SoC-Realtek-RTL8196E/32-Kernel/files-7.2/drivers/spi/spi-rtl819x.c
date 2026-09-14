// SPDX-License-Identifier: GPL-2.0
/*
 * SPI controller driver for Realtek RTL819x SoCs
 *
 * Adapted from Weijie Gao's out-of-tree RTL819x SPI driver and
 * modernized for 6.x:
 * - devm_spi_alloc_host() to avoid leaks
 * - readl_poll_timeout() to bound every hardware wait
 * - per-transfer clock divisor selection from speed_hz
 * - CS parked ALL_HIGH in every unexpected case
 * - devm-ordered teardown and shutdown quiesce the hardware safely
 *
 * This controller is the only path to the SPI NOR holding all four
 * flash partitions — see AUDIT.md/DESIGN.md next to this file for the
 * storage-gate validation rules before changing anything here.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/spi/spi.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/iopoll.h>
#include <linux/unaligned.h>

#define DRIVER_NAME "realtek-spi"
#define DRV_VERSION "1.2"

/* Registers */
#define RTK_SPI_CONFIG_OFFSET 0x00
#define RTK_SPI_CONTROL_STATUS_OFFSET 0x08
#define RTK_SPI_DATA_OFFSET 0x0c

/* CONFIG bits */
#define RTK_SPI_CLK_DIV_SHIFT 29 /* 3 bits: index 0..7 */
#define RTK_SPI_READ_BYTE_ORDER BIT(28)
#define RTK_SPI_WRITE_BYTE_ORDER BIT(27)
#define RTK_SPI_CS_DESELECT_TIME_SHIFT 22 /* 5 bits, 0..31 */

/* CONTROL/STATUS bits */
#define RTK_SPI_CS_0_HIGH BIT(31)
#define RTK_SPI_CS_1_HIGH BIT(30)
#define RTK_SPI_CS_ALL_HIGH (RTK_SPI_CS_0_HIGH | RTK_SPI_CS_1_HIGH)
#define RTK_SPI_DATA_LENGTH_SHIFT 28 /* 2 bits: (len-1) */
#define RTK_SPI_READY BIT(27)

/* Available divisors: parent_clk / {2,4,6,8,10,12,14,16} */
static const u32 realtek_spi_clk_div_table[] = { 2, 4, 6, 8, 10, 12, 14, 16 };

struct realtek_spi_data {
	struct spi_controller *master;
	void __iomem *base;
	u32 ioc_base;
	struct clk *clk; /* optional */
	u32 parent_rate; /* Hz */
	bool quiesced;
};

static void realtek_spi_quiesce(void *data);

#ifdef CONFIG_CPU_BIG_ENDIAN
static inline u32 realtek_spi_make_data(u32 data, u32 bytes)
{
	return data << ((4 - bytes) << 3);
}
static inline u32 realtek_spi_resolve_data(u32 data, u32 bytes)
{
	return data >> ((4 - bytes) << 3);
}
#else
static inline u32 realtek_spi_make_data(u32 data, u32 bytes)
{
	return data;
}
static inline u32 realtek_spi_resolve_data(u32 data, u32 bytes)
{
	return data;
}
#endif

static inline u32 rtk_rr(struct realtek_spi_data *rsd, unsigned reg)
{
	return ioread32(rsd->base + reg);
}

static inline void rtk_wr(struct realtek_spi_data *rsd, unsigned reg, u32 val)
{
	iowrite32(val, rsd->base + reg);
}

static int rtk_wait_ready(struct realtek_spi_data *rsd)
{
	u32 v;
	/* 10 ms timeout, poll every ~1 µs */
	return readl_poll_timeout(rsd->base + RTK_SPI_CONTROL_STATUS_OFFSET, v,
				  v & RTK_SPI_READY, 1, 10000);
}

static void rtk_set_txrx_size(struct realtek_spi_data *rsd, u32 size)
{
	rtk_wr(rsd, RTK_SPI_CONTROL_STATUS_OFFSET,
	       rsd->ioc_base | ((size - 1) << RTK_SPI_DATA_LENGTH_SHIFT));
}

static void rtk_set_default_config(struct realtek_spi_data *rsd, u32 div_idx)
{
	u32 cfg = (div_idx << RTK_SPI_CLK_DIV_SHIFT) |
		  (31 << RTK_SPI_CS_DESELECT_TIME_SHIFT); /* max deselect */

#ifdef CONFIG_CPU_BIG_ENDIAN
	cfg |= RTK_SPI_READ_BYTE_ORDER | RTK_SPI_WRITE_BYTE_ORDER;
#endif
	rtk_wr(rsd, RTK_SPI_CONFIG_OFFSET, cfg);
}

static u32 rtk_choose_div_idx(struct realtek_spi_data *rsd, u32 hz)
{
	u32 parent = rsd->parent_rate ? rsd->parent_rate : 200000000;
	u32 best_idx =
		ARRAY_SIZE(realtek_spi_clk_div_table) - 1; /* 16 by default */
	u32 i;

	if (!hz) /* fallback */
		return best_idx;

	for (i = 0; i < ARRAY_SIZE(realtek_spi_clk_div_table); i++) {
		u32 div = realtek_spi_clk_div_table[i];
		u32 rate = parent / div;
		if (rate <= hz) {
			best_idx = i;
			break;
		}
	}
	return best_idx;
}

static void realtek_spi_set_cs(struct spi_device *spi, bool cs_high)
{
	struct realtek_spi_data *rsd = spi_controller_get_devdata(spi->controller);

	if (cs_high) {
		/* Deselect: park the whole bus, both lines high. Writing
		 * only the addressed CS bit here would actively drive the
		 * *other* line low (= selected) for as long as the bus
		 * idles — audit SPI-001.
		 */
		rsd->ioc_base = RTK_SPI_CS_ALL_HIGH;
	} else {
		/* Select: drive the addressed line low by raising only the
		 * other one (active-low convention of this block).
		 */
		switch (spi_get_chipselect(spi, 0)) {
		case 0:
			rsd->ioc_base = RTK_SPI_CS_1_HIGH;
			break;
		case 1:
			rsd->ioc_base = RTK_SPI_CS_0_HIGH;
			break;
		default:
			rsd->ioc_base = RTK_SPI_CS_ALL_HIGH;
			break;
		}
	}

	rsd->ioc_base |= RTK_SPI_READY;
	rtk_wr(rsd, RTK_SPI_CONTROL_STATUS_OFFSET, rsd->ioc_base);
}

static int rtk_read(struct realtek_spi_data *rsd, u8 *buf, unsigned len)
{
	int ret;

	if (!IS_ALIGNED((unsigned long)buf, 4)) {
		rtk_set_txrx_size(rsd, 1);
		while (!IS_ALIGNED((unsigned long)buf, 4) && len) {
			ret = rtk_wait_ready(rsd);
			if (ret)
				return ret;
			*buf = realtek_spi_resolve_data(
				rtk_rr(rsd, RTK_SPI_DATA_OFFSET), 1);
			buf++;
			len--;
		}
	}

	rtk_set_txrx_size(rsd, 4);
	while (len >= 4) {
		ret = rtk_wait_ready(rsd);
		if (ret)
			return ret;
		put_unaligned(realtek_spi_resolve_data(
			rtk_rr(rsd, RTK_SPI_DATA_OFFSET), 4), (u32 *)buf);
		buf += 4;
		len -= 4;
	}

	rtk_set_txrx_size(rsd, 1);
	while (len) {
		ret = rtk_wait_ready(rsd);
		if (ret)
			return ret;
		*buf = realtek_spi_resolve_data(
			rtk_rr(rsd, RTK_SPI_DATA_OFFSET), 1);
		buf++;
		len--;
	}

	return 0;
}

static int rtk_write(struct realtek_spi_data *rsd, const u8 *buf, unsigned len)
{
	int ret;

	if (!IS_ALIGNED((unsigned long)buf, 4)) {
		rtk_set_txrx_size(rsd, 1);
		while (!IS_ALIGNED((unsigned long)buf, 4) && len) {
			rtk_wr(rsd, RTK_SPI_DATA_OFFSET,
			       realtek_spi_make_data(*buf, 1));
			ret = rtk_wait_ready(rsd);
			if (ret)
				return ret;
			buf++;
			len--;
		}
	}

	rtk_set_txrx_size(rsd, 4);
	while (len >= 4) {
		rtk_wr(rsd, RTK_SPI_DATA_OFFSET,
		       realtek_spi_make_data(get_unaligned((const u32 *)buf), 4));
		ret = rtk_wait_ready(rsd);
		if (ret)
			return ret;
		buf += 4;
		len -= 4;
	}

	rtk_set_txrx_size(rsd, 1);
	while (len) {
		rtk_wr(rsd, RTK_SPI_DATA_OFFSET,
		       realtek_spi_make_data(*buf, 1));
		ret = rtk_wait_ready(rsd);
		if (ret)
			return ret;
		buf++;
		len--;
	}

	return 0;
}

static int realtek_spi_transfer_one(struct spi_controller *master,
				    struct spi_device *spi,
				    struct spi_transfer *xfer)
{
	struct realtek_spi_data *rsd = spi_controller_get_devdata(master);
	u32 hz = xfer->speed_hz ? xfer->speed_hz :
				  (spi->max_speed_hz ? spi->max_speed_hz :
						       master->max_speed_hz);
	u32 div_idx = rtk_choose_div_idx(rsd, hz);

	/* No divisor reaches a request below parent/16 (12.5 MHz at the
	 * 200 MHz LX clock); the fallback then runs the wire *above* the
	 * requested ceiling — audit SPI-004. No such device exists today
	 * (the flash runs at 25 MHz, divisor 8 exact); warn if one shows up.
	 */
	if (hz && (rsd->parent_rate / realtek_spi_clk_div_table[div_idx]) > hz)
		dev_warn_once(&spi->dev,
			      "%u Hz below divisor range, overclocking to %u Hz\n",
			      hz,
			      rsd->parent_rate / realtek_spi_clk_div_table[div_idx]);

	/* Program the clock and CS deselect timing for this transfer */
	rtk_set_default_config(rsd, div_idx);
	xfer->effective_speed_hz =
		rsd->parent_rate / realtek_spi_clk_div_table[div_idx];

	/* Full-duplex transfers never reach us: the core filters them at
	 * validation time for SPI_CONTROLLER_HALF_DUPLEX controllers.
	 */

	if (xfer->tx_buf)
		return rtk_write(rsd, (const u8 *)xfer->tx_buf, xfer->len);

	if (xfer->rx_buf)
		return rtk_read(rsd, (u8 *)xfer->rx_buf, xfer->len);

	return 0;
}

static int realtek_spi_probe(struct platform_device *pdev)
{
	struct realtek_spi_data *rsd;
	struct spi_controller *master;
	struct resource *res;
	u32 rate = 0;
	int ret;

	master = devm_spi_alloc_host(&pdev->dev, sizeof(*rsd));
	if (!master)
		return -ENOMEM;

	rsd = spi_controller_get_devdata(master);
	platform_set_drvdata(pdev, rsd);
	rsd->master = master;

	/* Bus number from DT alias "spi<N>", fallback to platform id or 0 */
	{
		int id = of_alias_get_id(pdev->dev.of_node, "spi");
		if (id < 0)
			id = (pdev->id >= 0) ? pdev->id : 0;
		master->bus_num = id;
	}

	master->dev.of_node = pdev->dev.of_node;
	master->num_chipselect = 2;
	/* Truth in advertising (audit SPI-002/-003): the block runs mode 0
	 * only (nothing programs CPOL/CPHA) and moves byte streams only —
	 * let the core reject what the hardware can't do.
	 */
	/*
	 * Mode 0, active-low CS only. The SPI core has already converted its
	 * logical select state to the physical level passed to ->set_cs().
	 * Advertising SPI_CS_HIGH would require per-line idle-polarity tracking
	 * so selecting one active-high device cannot select the other CS.
	 */
	master->mode_bits = 0;
	master->flags = SPI_CONTROLLER_HALF_DUPLEX;
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->transfer_one = realtek_spi_transfer_one;
	master->set_cs = realtek_spi_set_cs;

	/* ioremapped regs */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	rsd->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rsd->base))
		return PTR_ERR(rsd->base);

	/* Clock (optional) */
	rsd->clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(rsd->clk))
		return PTR_ERR(rsd->clk);
	if (rsd->clk) {
		ret = clk_prepare_enable(rsd->clk);
		if (ret)
			return ret;
		rate = clk_get_rate(rsd->clk);
	}
	if (!rate)
		of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				     &rate);
	if (!rate)
		rate = 200000000; /* fallback: LX bus clock on RTL8196E */
	rsd->parent_rate = rate;

	master->max_speed_hz = rate / 2; /* div=2 */
	master->min_speed_hz = rate / 16; /* div=16 */

	/* Safe config and CS state before anything talks */
	rtk_set_default_config(rsd, ARRAY_SIZE(realtek_spi_clk_div_table) - 1);
	rtk_wr(rsd, RTK_SPI_CONTROL_STATUS_OFFSET,
	       RTK_SPI_CS_ALL_HIGH | RTK_SPI_READY);

	/*
	 * Added before devm_spi_register_controller(): devres unwinds in LIFO
	 * order, so the SPI core first unregisters the controller and drains its
	 * queue, then this action parks CS and disables the clock.
	 */
	ret = devm_add_action_or_reset(&pdev->dev, realtek_spi_quiesce, rsd);
	if (ret)
		return ret;

	ret = devm_spi_register_controller(&pdev->dev, master);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "v" DRV_VERSION " - bus %d, parent %u Hz, %d CS\n",
		 master->bus_num, rsd->parent_rate, master->num_chipselect);

	return 0;
}

static void realtek_spi_quiesce(void *data)
{
	struct realtek_spi_data *rsd = data;

	if (!rsd || rsd->quiesced)
		return;

	/* Park the hardware only after the SPI core has drained its queue. */
	rtk_set_default_config(
		rsd, ARRAY_SIZE(realtek_spi_clk_div_table) - 1);
	rtk_wr(rsd, RTK_SPI_CONTROL_STATUS_OFFSET,
	       RTK_SPI_CS_ALL_HIGH | RTK_SPI_READY);
	if (rsd->clk) {
		clk_disable_unprepare(rsd->clk);
		rsd->clk = NULL;
	}
	rsd->quiesced = true;
}

static void realtek_spi_shutdown(struct platform_device *pdev)
{
	struct realtek_spi_data *rsd = platform_get_drvdata(pdev);

	/* Child devices are shut down before their controller. */
	realtek_spi_quiesce(rsd);
}

static const struct of_device_id realtek_spi_match[] = {
	{ .compatible = "realtek,rtl819x-spi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, realtek_spi_match);

static struct platform_driver realtek_spi_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = realtek_spi_match,
	},
	.probe    = realtek_spi_probe,
	.shutdown = realtek_spi_shutdown,
};
module_platform_driver(realtek_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Weijie Gao <hackpascal@gmail.com>");
MODULE_DESCRIPTION("Realtek SoC SPI controller driver (RTL819x)");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS("platform:" DRIVER_NAME);
