// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8196e_hw.c - RTL8196E hardware abstraction layer.
 *
 * Covers: MMIO read/write helpers, SoC clock and reset sequence, MDIO/PHY access,
 * hardware table engine (VLAN, NETIF, L2 forwarding tables), and CPU DMA ring
 * register programming.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include "rtl8196e_hw.h"


/* Poll MDCIOSR until the MDC/MDIO bus is idle; return -ETIMEDOUT on failure. */
static int rtl8196e_mdio_wait_ready(void)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if ((rtl8196e_readl(MDCIOSR) & MDC_STATUS) == 0)
			return 0;
		usleep_range(10, 20);
	}

	return -ETIMEDOUT;
}

/* Read PHY register @reg from PHY address @phy via MDIO; store result in *val. */
static int rtl8196e_mdio_read(int phy, int reg, u16 *val)
{
	int ret;

	rtl8196e_writel(COMMAND_READ | (phy << PHYADD_OFFSET) | (reg << REGADD_OFFSET), MDCIOCR);
	ret = rtl8196e_mdio_wait_ready();
	if (ret)
		return ret;

	*val = rtl8196e_readl(MDCIOSR) & 0xffff;
	return 0;
}

/* Write @val to PHY register @reg on PHY address @phy via MDIO. */
static int rtl8196e_mdio_write(int phy, int reg, u16 val)
{
	rtl8196e_writel(COMMAND_WRITE | (phy << PHYADD_OFFSET) | (reg << REGADD_OFFSET) | val, MDCIOCR);
	return rtl8196e_mdio_wait_ready();
}

/* Poll TBL_ACCESS_CTRL until the hardware table access engine is idle. */
static int rtl8196e_table_wait_ready(void)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if ((rtl8196e_readl(TBL_ACCESS_CTRL) & TBL_ACCESS_BUSY) == 0)
			return 0;
		usleep_range(10, 20);
	}

	return -ETIMEDOUT;
}

/* Assert TLU_CTRL_START and wait for TLU_CTRL_READY; return -ETIMEDOUT on failure. */
static int rtl8196e_tlu_start(void)
{
	u32 tlu;
	int i;

	tlu = rtl8196e_readl(TLU_CTRL);
	rtl8196e_writel(tlu | TLU_CTRL_START, TLU_CTRL);
	for (i = 0; i < 1000; i++) {
		if (rtl8196e_readl(TLU_CTRL) & TLU_CTRL_READY)
			return 0;
		usleep_range(10, 20);
	}

	return -ETIMEDOUT;
}

/* Clear TLU_CTRL_START and TLU_CTRL_READY to stop the Table Lookup Unit. */
static void rtl8196e_tlu_stop(void)
{
	u32 tlu;

	tlu = rtl8196e_readl(TLU_CTRL);
	rtl8196e_writel(tlu & ~(TLU_CTRL_START | TLU_CTRL_READY), TLU_CTRL);
}

/* Write @nwords data words to a hardware table entry of @type at @index. */
static int rtl8196e_table_write(u32 type, u32 index, const u32 *words, u32 nwords)
{
	u32 addr = ASIC_TABLE_BASE + (type << 16) + (index << 5);
	u32 swtcr;
	int ret;
	u32 i;

	if (!words || nwords == 0 || nwords > 8)
		return -EINVAL;

	ret = rtl8196e_table_wait_ready();
	if (ret)
		return ret;

	ret = rtl8196e_tlu_start();
	if (ret) {
		pr_warn_ratelimited("rtl8196e-eth: TLU not ready, aborting table write (type=%u idx=%u)\n",
				    type, index);
		rtl8196e_tlu_stop();	/* clear the START tlu_start() left asserted */
		return ret;
	}

	swtcr = rtl8196e_readl(SWTCR0);
	rtl8196e_writel(swtcr | SWTCR0_TLU_START, SWTCR0);
	for (i = 0; i < 1000; i++) {
		if (rtl8196e_readl(SWTCR0) & SWTCR0_TLU_BUSY)
			break;
		udelay(10);
	}
	if (i == 1000) {
		pr_warn_ratelimited("rtl8196e-eth: TLU BUSY handshake timed out (type=%u idx=%u)\n",
				    type, index);
		ret = -ETIMEDOUT;
		goto out_restore;
	}

	for (i = 0; i < nwords; i++)
		rtl8196e_writel(words[i], TBL_ACCESS_DATA + (i * 4));
	for (; i < 8; i++)
		rtl8196e_writel(0, TBL_ACCESS_DATA + (i * 4));

	rtl8196e_writel(addr, TBL_ACCESS_ADDR);
	rtl8196e_writel(TBL_ACCESS_CMD_WRITE, TBL_ACCESS_CTRL);

	ret = rtl8196e_table_wait_ready();
	if (ret)
		goto out_restore;

	if (rtl8196e_readl(TBL_ACCESS_STAT) & 0x1)
		ret = -EIO;

out_restore:
	rtl8196e_writel(swtcr & ~(SWTCR0_TLU_START | SWTCR0_TLU_BUSY), SWTCR0);
	rtl8196e_tlu_stop();

	return ret;
}

/*
 * Write two 32-bit words to L2 forwarding table entry @index via both the table
 * access engine and a direct ASIC RAM mirror write for silicon compatibility.
 */
static int rtl8196e_l2_write_entry(u32 index, u32 word0, u32 word1)
{
	u32 addr = ASIC_TABLE_BASE + (index << 5);
	u32 swtcr;
	int ret;
	int i;

	ret = rtl8196e_table_wait_ready();
	if (ret)
		return ret;

	ret = rtl8196e_tlu_start();
	if (ret) {
		pr_warn_ratelimited("rtl8196e-eth: TLU not ready, aborting L2 write (idx=%u)\n",
				    index);
		rtl8196e_tlu_stop();	/* clear the START tlu_start() left asserted */
		return ret;
	}

	/* Start the SWTCR0 table-write handshake. */
	swtcr = rtl8196e_readl(SWTCR0);
	rtl8196e_writel(swtcr | SWTCR0_TLU_START, SWTCR0);
	for (i = 0; i < 1000; i++) {
		if (rtl8196e_readl(SWTCR0) & SWTCR0_TLU_BUSY)
			break;
		udelay(10);
	}
	if (i == 1000) {
		pr_warn_ratelimited("rtl8196e-eth: TLU BUSY handshake timed out (L2 idx=%u)\n",
				    index);
		ret = -ETIMEDOUT;
		goto out_restore;
	}

	rtl8196e_writel(word0, TBL_ACCESS_DATA);
	rtl8196e_writel(word1, TBL_ACCESS_DATA + 0x04);
	rtl8196e_writel(addr, TBL_ACCESS_ADDR);
	rtl8196e_writel(TBL_ACCESS_CMD_WRITE, TBL_ACCESS_CTRL);

	ret = rtl8196e_table_wait_ready();
	if (ret)
		goto out_restore;

	if (rtl8196e_readl(TBL_ACCESS_STAT) & 0x1)
		ret = -EIO;

out_restore:
	rtl8196e_writel(swtcr & ~(SWTCR0_TLU_START | SWTCR0_TLU_BUSY), SWTCR0);
	rtl8196e_tlu_stop();

	if (!ret) {
		/* Direct mirror write (some silicon revisions only latch from table RAM) */
		rtl8196e_writel(word0, addr + 0x00);
		rtl8196e_writel(word1, addr + 0x04);
		rtl8196e_writel(0, addr + 0x08);
		rtl8196e_writel(0, addr + 0x0c);
		rtl8196e_writel(0, addr + 0x10);
		rtl8196e_writel(0, addr + 0x14);
		rtl8196e_writel(0, addr + 0x18);
		rtl8196e_writel(0, addr + 0x1c);
	}

	return ret;
}

/* Write a single VLAN table entry at @index with @word0 (3-word layout, words 1-2 zeroed). */
static int rtl8196e_vlan_write_entry(u32 index, u32 word0)
{
	u32 words[3] = { word0, 0, 0 };

	return rtl8196e_table_write(RTL8196E_TBL_VLAN, index, words, 3);
}

/* Clear all entries in the VLAN table. */
static int rtl8196e_vlan_clear_table(void)
{
	u32 index;
	int ret;

	for (index = 0; index < RTL8196E_VLAN_TABLE_SIZE; index++) {
		ret = rtl8196e_vlan_write_entry(index, 0);
		if (ret)
			return ret;
	}

	return 0;
}

/* Clear all entries in the NETIF table. */
static int rtl8196e_netif_clear_table(void)
{
	u32 words[4] = { 0, 0, 0, 0 };
	u32 index;
	int ret;

	for (index = 0; index < RTL8196E_NETIF_TABLE_SIZE; index++) {
		ret = rtl8196e_table_write(RTL8196E_TBL_NETIF, index, words, 4);
		if (ret)
			return ret;
	}

	return 0;
}

/* Clear all 1024 entries in the L2 forwarding table. */
static int rtl8196e_l2_clear_table(void)
{
	u32 index;
	int ret;

	for (index = 0; index < 1024; index++) {
		ret = rtl8196e_l2_write_entry(index, 0, 0);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Read L2 entry @index, double-checking by reading twice until both reads agree,
 * to guard against transient update races in the ASIC RAM.
 */
static int rtl8196e_l2_read_entry(u32 index, u32 *word0, u32 *word1)
{
	u32 addr = ASIC_TABLE_BASE + (index << 5);
	u32 a[8];
	u32 b[8];
	int i;
	int j;
	int ret;
	if (!word0 || !word1)
		return -EINVAL;

	ret = rtl8196e_table_wait_ready();
	if (ret)
		return ret;

	for (i = 0; i < 10; i++) {
		for (j = 0; j < 8; j++)
			a[j] = rtl8196e_readl(addr + (j * 4));
		for (j = 0; j < 8; j++)
			b[j] = rtl8196e_readl(addr + (j * 4));
		if (memcmp(a, b, sizeof(a)) == 0) {
			*word0 = a[0];
			*word1 = a[1];
			return 0;
		}
	}
	return -EIO;
}

/* Full hardware initialisation: enable switch clock, MEMCR, full reset, clear L2 table. */
/*
 * Switch-core silicon reset — the vendor FullAndSemiReset sequence for the
 * RTL8196E: cycle the switch-core clock (SYS_CLK_MAG CM_ACTIVE_SWCORE off/on
 * under CM_PROTECT), re-init MEMCR, pulse FULL_RST via SIRR, restore the LED /
 * RX-queue mapping, clear the L2 table and pending interrupts. ~650 ms of
 * sleeps, so callers must be in process context. Run once at probe (inside
 * rtl8196e_hw_init, after pad muxing) and re-run on its own by the
 * deep-reset worker to recover a wedged switch core — the depth
 * of the vendor rtl865x_reinitSwitchCore.
 */
int rtl8196e_hw_swcore_reset(struct rtl8196e_hw *hw)
{
	u32 clk;
	int ret;

	/* Ensure switch core clock is active (vendor sequence) */
	clk = rtl8196e_readl(SYS_CLK_MAG);
	rtl8196e_writel(clk | CM_PROTECT, SYS_CLK_MAG);
	clk = rtl8196e_readl(SYS_CLK_MAG);
	rtl8196e_writel(clk & ~CM_ACTIVE_SWCORE, SYS_CLK_MAG);
	msleep(300);
	clk = rtl8196e_readl(SYS_CLK_MAG);
	rtl8196e_writel(clk | CM_ACTIVE_SWCORE, SYS_CLK_MAG);
	clk = rtl8196e_readl(SYS_CLK_MAG);
	rtl8196e_writel(clk & ~CM_PROTECT, SYS_CLK_MAG);
	msleep(50);

	/* MEMCR init is mandatory */
	rtl8196e_writel(0, MEMCR);
	rtl8196e_writel(0x7f, MEMCR);

	/* Full reset of switch core */
	rtl8196e_writel(FULL_RST, SIRR);
	msleep(300);

	/* Enable ASIC LED controller in direct mode (link/activity).
	 * The LAN LED is hardwired to a switch ASIC LED_PORTn output
	 * (B6/LED_PORT4 on the Lidl, B2/LED_PORT0 on the Sengled G4) —
	 * GPIO control has no physical effect on it.
	 * LEDMODE_DIRECT gives full-brightness link/activity indication. */
	rtl8196e_writel(LEDMODE_DIRECT, LEDCREG);

	/* Map all RX queues to ring 0 (safe default) */
	rtl8196e_writel(0, CPUQDM0);
	rtl8196e_writel(0, CPUQDM2);
	rtl8196e_writel(0, CPUQDM4);

	ret = rtl8196e_l2_clear_table();
	if (ret)
		pr_warn("rtl8196e-eth: L2 table clear failed (%d)\n", ret);

	/* Clear pending interrupts */
	rtl8196e_writel(rtl8196e_readl(CPUIISR), CPUIISR);

	/* Return the L2-clear result. Callers decide severity: hw_init warns and
	 * continues (a stale L2 table must not block bringup), while the deep-reset
	 * worker treats the follow-on hw_reprogram failure as its hold-down trigger. */
	return ret;
}

int rtl8196e_hw_init(struct rtl8196e_hw *hw)
{
	int ret;

	/*
	 * Configure pin mux for MII mode via syscon regmap.
	 * Without this, bootloader defaults leave pins muxed to functions
	 * that drive EFR32 nRST LOW, preventing the Zigbee radio from
	 * operating.
	 *
	 * PIN_MUX_SEL (0xB800_0040):
	 *   Bits [4:3] = 01 (UART1 TXD), not 00, matching vendor BSP.
	 *   Clear bits [9:8], [11:10], [15] for MII mode.
	 *
	 * PIN_MUX_SEL_2 (0xB800_0044) — pads B2..B6, shared GPIO/LED_PORTn
	 * (datasheet Table 36; same field map as gpio-rtl819x). The board
	 * decides each pad's function on the GPIO controller node
	 * (#124/#126):
	 *   - pad named in gpio-line-names -> field = 11 (GPIO): the pad
	 *     belongs to a GPIO consumer (LED driver, s40button,
	 *     uart-bridge nRST/blmode). Muxing it here, not just at claim
	 *     time, gives a deterministic boot state for lines that are
	 *     claimed late or on demand — e.g. nRST muxed to an undriven
	 *     GPIO input floats high through the EFR32's RESETn pull-up
	 *     instead of being driven by a leftover bootloader function.
	 *   - pad listed in realtek,led-pads -> field = 00 (LED_PORTn):
	 *     the ASIC LED controller drives it (link/activity, LEDCREG
	 *     direct mode below). The LED pad is declared, not derived
	 *     from member-ports: it is a wiring fact a port mask cannot
	 *     prove (a member port may have no LED wired), and only a
	 *     visual check does. On both known boards it follows the
	 *     Table 36 1-1 naming — Lidl port 4 -> B6/LED_PORT4, Sengled
	 *     G4 port 0 -> B2/LED_PORT0 (both verified by eye, #126).
	 *   - neither -> field = 11 as unclaimed GPIO: the pad is Hi-Z
	 *     (Table 36 has no disable encoding; 00/10 leave the ASIC
	 *     driving the pin), the safe state for unknown wiring.
	 *   - realtek,led-pads absent -> every unnamed pad gets 00, the
	 *     pre-v2.8 behaviour, so a DTB without the property keeps its
	 *     LAN LED.
	 * Bits [17:15] (MII) are always cleared.
	 * On the Lidl board (led-pads 14; names at 11/status-led,
	 * 12/efr32-nrst) this yields B6=00 (LAN LED), B3=B4=11 GPIO and
	 * B2=B5=11 Hi-Z; on the Sengled G4 (led-pads 10, names at 11..14)
	 * B2=00 (port-0 LAN LED) and B3..B6=11.
	 */
	if (hw->syscon) {
		static const struct {
			u8 line;	/* gpio-rtl819x line number */
			u8 shift;	/* PIN_MUX_SEL_2 field LSB */
		} pads[] = {
			{ 10, 0 },	/* B2 / LED_PORT0 */
			{ 11, 3 },	/* B3 / LED_PORT1 */
			{ 12, 6 },	/* B4 / LED_PORT2 */
			{ 13, 9 },	/* B5 / LED_PORT3 */
			{ 14, 12 },	/* B6 / LED_PORT4 */
		};
		struct device_node *gpio_np;
		u32 mask = 7 << 15, val = 0;	/* [17:15] MII: always 0 */
		bool has_led_pads = false;
		int i;

		gpio_np = of_find_compatible_node(NULL, NULL,
						  "realtek,rtl8196e-gpio");
		if (gpio_np)
			has_led_pads = of_property_present(gpio_np,
							   "realtek,led-pads");
		for (i = 0; i < ARRAY_SIZE(pads); i++) {
			const char *name = NULL;
			bool led = false;
			u32 u;

			if (gpio_np)
				of_property_read_string_index(gpio_np,
							      "gpio-line-names",
							      pads[i].line,
							      &name);
			if (has_led_pads)
				of_property_for_each_u32(gpio_np,
							 "realtek,led-pads", u)
					if (u == pads[i].line)
						led = true;
			mask |= 3 << pads[i].shift;
			if (name && *name) {
				val |= 3 << pads[i].shift;
				if (led)
					pr_warn("rtl8196e-eth: gpio line %u both named and in realtek,led-pads; name wins (GPIO)\n",
						pads[i].line);
			} else if (!led && has_led_pads) {
				val |= 3 << pads[i].shift;
			}
		}
		of_node_put(gpio_np);

		/* PIN_MUX_SEL: bits [4:3]=01 (UART1), clear bits 8-10,15 */
		regmap_update_bits(hw->syscon, 0x40,
				   (3 << 8) | (3 << 10) | (3 << 3) | (1 << 15),
				   (1 << 3));
		regmap_update_bits(hw->syscon, 0x44, mask, val);
	}

	/*
	 * Switch-core silicon reset (clock cycle + MEMCR + FULL_RST + L2
	 * clear, ~650 ms of sleeps). Factored out so the deep-reset
	 * worker can re-run it without re-touching the pad
	 * muxes above.
	 *
	 * A best-effort L2-table clear failure is warned, NOT fatal: probe must
	 * still bring the interface up (the switch clock cycle + FULL_RST is the
	 * reset that matters). Failing probe here leaves the board with no eth0
	 * and, since it is the only interface, no way back in.
	 */
	ret = rtl8196e_hw_swcore_reset(hw);
	if (ret)
		pr_warn("rtl8196e-eth: switch-core reset L2 clear failed at init (%d), continuing\n",
			ret);
	return 0;
}

/* Set the port default VLAN ID (PVID) for @port in the PVCR register pair. */
static int rtl8196e_set_pvid(u32 port, u32 pvid)
{
	u32 reg;
	u32 offset;

	if (port >= 9 || pvid >= 4096)
		return -EINVAL;

	offset = (port * 2) & ~0x3;
	reg = rtl8196e_readl(PVCR0 + offset);
	if (port & 0x1)
		reg = ((pvid & 0xfff) << 16) | (reg & ~0xfff0000);
	else
		reg = (pvid & 0xfff) | (reg & ~0xfff);
	rtl8196e_writel(reg, PVCR0 + offset);

	return 0;
}

/* Assign NETIF table index @netif to @port in the PLITIMR register. */
static int rtl8196e_set_port_netif(u32 port, u32 netif)
{
	u32 reg;
	u32 offset;

	if (port >= 9 || netif > 7)
		return -EINVAL;

	offset = port * 3;
	reg = rtl8196e_readl(PLITIMR);
	reg &= ~(0x7 << offset);
	reg |= (netif & 0x7) << offset;
	rtl8196e_writel(reg, PLITIMR);

	return 0;
}

/* Program the VLAN table entry 0 and set PVIDs for all member ports. */
int rtl8196e_hw_vlan_setup(struct rtl8196e_hw *hw, u16 vid, u8 fid,
			   u32 member_ports, u32 untag_ports)
{
	u32 word0;
	int ret;
	u32 port;

	if (vid == 0 || vid >= 4096)
		return -EINVAL;

	ret = rtl8196e_vlan_clear_table();
	if (ret) {
		pr_warn("rtl8196e-eth: VLAN table clear failed (%d)\n", ret);
		return ret;
	}

	/* Big-endian MSB-first table layout (rtl865xc_tblAsic_vlanTable_t) */
	word0 = ((vid & 0xfff) << 20);
	word0 |= ((fid & 0x3) << 18);
	word0 |= (((untag_ports >> 6) & 0x7) << 15);
	word0 |= ((untag_ports & 0x3f) << 9);
	word0 |= (((member_ports >> 6) & 0x7) << 6);
	word0 |= (member_ports & 0x3f);

	ret = rtl8196e_vlan_write_entry(0, word0);
	if (ret)
		return ret;

	for (port = 0; port < 9; port++) {
		if (!(member_ports & (1 << port)))
			continue;
		ret = rtl8196e_set_pvid(port, vid);
		if (ret)
			pr_warn("rtl8196e-eth: set PVID failed (port=%u ret=%d)\n", port, ret);
	}

	return 0;
}

/* Program NETIF table entry 0 with MAC, VLAN, MTU and member ports, then assign ports. */
int rtl8196e_hw_netif_setup(struct rtl8196e_hw *hw, const u8 *mac, u16 vid,
			    u16 mtu, u32 member_ports)
{
	u32 words[4];
	u64 mac48;
	u32 mac18_0;
	u32 mac47_19;
	u32 word0;
	u32 word1;
	u32 word2;
	u32 word3;
	u32 port;
	int ret;

	if (!mac || vid == 0 || vid >= 4096 || mtu < 576)
		return -EINVAL;

	ret = rtl8196e_netif_clear_table();
	if (ret) {
		pr_warn("rtl8196e-eth: NETIF table clear failed (%d)\n", ret);
		return ret;
	}

	mac48 = ((u64)mac[0] << 40) | ((u64)mac[1] << 32) | ((u64)mac[2] << 24) |
		((u64)mac[3] << 16) | ((u64)mac[4] << 8) | mac[5];
	mac18_0 = (u32)(mac48 & 0x7ffff);
	mac47_19 = (u32)((mac48 >> 19) & 0x1fffffff);

	/* Big-endian MSB-first table layout (rtl865xc_tblAsic_netifTable_t) */
	word0 = (mac18_0 << 13) | ((vid & 0xfff) << 1) | 0x1;
	word1 = mac47_19;
	word2 = (mtu & 0x7) << 29;
	word3 = (mtu >> 3) & 0xfff;

	words[0] = word0;
	words[1] = word1;
	words[2] = word2;
	words[3] = word3;

	ret = rtl8196e_table_write(RTL8196E_TBL_NETIF, 0, words, 4);
	if (ret)
		return ret;

	for (port = 0; port < 9; port++) {
		if (!(member_ports & (1 << port)))
			continue;
		ret = rtl8196e_set_port_netif(port, 0);
		if (ret)
			pr_warn("rtl8196e-eth: set port netif failed (port=%u ret=%d)\n", port, ret);
	}

	return 0;
}

/* Reset the PHY via PCR, then enable auto-negotiation via MDIO BMCR write. */
int rtl8196e_hw_init_phy(struct rtl8196e_hw *hw, int port, int phy_id)
{
	u32 pcr;
	u16 bmcr;
	int ret;

	if (port < 0 || phy_id < 0)
		return -EINVAL;

	pcr = rtl8196e_readl(PCRP0 + (port << 2));
	pcr |= EnablePHYIf | MacSwReset;
	rtl8196e_writel(pcr, PCRP0 + (port << 2));
	udelay(10);
	pcr &= ~MacSwReset;
	rtl8196e_writel(pcr, PCRP0 + (port << 2));

	ret = rtl8196e_mdio_read(phy_id, 0, &bmcr);
	if (ret)
		return ret;

	bmcr |= (1 << 12) | (1 << 9); /* AN enable + restart */
	return rtl8196e_mdio_write(phy_id, 0, bmcr);
}

/* Return true if the link is up on @port, by reading the port status register. */
bool rtl8196e_hw_link_up(struct rtl8196e_hw *hw, int port)
{
	u32 status;

	if (port < 0)
		return false;

	status = rtl8196e_readl(PSRP0 + (port << 2));
	return (status & PortStatusLinkUp) != 0;
}

/*
 * Decode @port's PSRPx register into link/speed/duplex for ethtool.
 * Speed bits 1:0: 0=10M, 1=100M, others reserved on RTL8196E.
 */
void rtl8196e_hw_link_status(struct rtl8196e_hw *hw, int port,
			     bool *link, int *speed_mbps, bool *full_duplex)
{
	u32 status;
	u32 spd;

	if (port < 0) {
		if (link) *link = false;
		if (speed_mbps) *speed_mbps = -1;
		if (full_duplex) *full_duplex = false;
		return;
	}

	status = rtl8196e_readl(PSRP0 + (port << 2));
	if (link)
		*link = !!(status & PortStatusLinkUp);
	if (full_duplex)
		*full_duplex = !!(status & PortStatusDuplex);
	if (speed_mbps) {
		spd = (status & PortStatusLinkSpeed_MASK) >> PortStatusLinkSpeed_OFFSET;
		switch (spd) {
		case 0: *speed_mbps = 10; break;
		case 1: *speed_mbps = 100; break;
		default: *speed_mbps = -1; break;
		}
	}
}

/* Configure L2 forwarding mode, flood control, aging, queue count, and STP state. */
void rtl8196e_hw_l2_setup(struct rtl8196e_hw *hw)
{
	u32 swtcr;
	u32 ffcr;
	u32 cscr;
	u32 mscr;
	u32 teacr;
	u32 swtcr1;
	u32 vcr0;
	u32 qnumcr;
	u32 port;
	u32 pcr;

	swtcr1 = rtl8196e_readl(SWTCR1);
	swtcr1 |= ENNATT2LOG | ENFRAGTOACLPT;
	rtl8196e_writel(swtcr1, SWTCR1);

	mscr = rtl8196e_readl(MSCR);
	mscr |= EN_L2;
	mscr &= ~(EN_L3 | EN_L4);
	rtl8196e_writel(mscr, MSCR);

	teacr = rtl8196e_readl(TEACR);
	teacr &= ~0x3; /* enable L2 aging, disable L4 aging */
	rtl8196e_writel(teacr, TEACR);

	swtcr = rtl8196e_readl(SWTCR0);
	swtcr &= ~LIMDBC_MASK;
	swtcr |= LIMDBC_VLAN;
	swtcr |= NAPTF2CPU;
	swtcr |= (MCAST_PORT_EXT_MODE_MASK << MCAST_PORT_EXT_MODE_OFFSET);
	rtl8196e_writel(swtcr, SWTCR0);

	vcr0 = rtl8196e_readl(VCR0);
	vcr0 &= ~EN_ALL_PORT_VLAN_INGRESS_FILTER;
	rtl8196e_writel(vcr0, VCR0);

	ffcr = rtl8196e_readl(FFCR);
	ffcr |= EN_MCAST | EN_UNMCAST_TOCPU;
	ffcr &= ~EN_UNUNICAST_TOCPU;
	rtl8196e_writel(ffcr, FFCR);

	cscr = rtl8196e_readl(CSCR);
	/*
	 * Clear only the port-to-port L2/L3/L4 checksum-error-allow bits. Do NOT
	 * touch AcceptL2Err (bit 3): the driver sets EXCLUDE_CRC, so the CPU
	 * injects frames without an FCS and the switch appends it on egress;
	 * AcceptL2Err=1 (reset default, vendor-kept) is what lets the CPU port
	 * accept those FCS-less frames. Clearing it makes the switch drop every
	 * CPU TX frame as an L2 error. RX-side FCS integrity is already enforced
	 * at the physical-port ingress, before the CPU port.
	 */
	cscr &= ~(ALLOW_L2_CHKSUM_ERR | ALLOW_L3_CHKSUM_ERR | ALLOW_L4_CHKSUM_ERR);
	rtl8196e_writel(cscr, CSCR);

	/* Set all ports (0-6) to 1 output queue */
	qnumcr = rtl8196e_readl(QNUMCR);
	for (port = 0; port <= 6; port++) {
		qnumcr &= ~(0x7 << (3 * port));
		qnumcr |= (1 << (3 * port));
	}
	rtl8196e_writel(qnumcr, QNUMCR);

	/*
	 * Force STP state to forwarding on physical ports.
	 *
	 * We deliberately do NOT touch the PCR accept-max-length field: the
	 * vendor SDK never programs it, its bit position is chip-variant
	 * dependent (documented for RTL865xB), and the silicon reset default
	 * (1536) already guarantees an accepted frame fits the 1700-byte RX
	 * buffer. Writing an unvalidated field here risks wedging the port for
	 * no real gain.
	 */
	for (port = 0; port < 6; port++) {
		pcr = rtl8196e_readl(PCRP0 + (port << 2));
		pcr &= ~STP_PortST_MASK;
		pcr |= STP_PortST_FORWARDING;
		rtl8196e_writel(pcr, PCRP0 + (port << 2));
	}
}

/* Enable trap-to-CPU for all unknown unicast traffic (debug fallback when L2 entry fails). */
void rtl8196e_hw_l2_trap_enable(struct rtl8196e_hw *hw)
{
	u32 swtcr;
	u32 ffcr;
	u32 cscr;

	swtcr = rtl8196e_readl(SWTCR0);
	swtcr &= ~LIMDBC_MASK;
	swtcr |= LIMDBC_VLAN | NAPTF2CPU;
	rtl8196e_writel(swtcr, SWTCR0);

	ffcr = rtl8196e_readl(FFCR);
	ffcr |= EN_UNUNICAST_TOCPU | EN_UNMCAST_TOCPU | EN_MCAST;
	rtl8196e_writel(ffcr, FFCR);

	cscr = rtl8196e_readl(CSCR);
	/* Do NOT clear AcceptL2Err here either — see rtl8196e_hw_l2_setup(). */
	cscr &= ~(ALLOW_L2_CHKSUM_ERR | ALLOW_L3_CHKSUM_ERR | ALLOW_L4_CHKSUM_ERR);
	rtl8196e_writel(cscr, CSCR);
}

/* Add a static toCPU L2 entry for @mac so frames destined to it are forwarded to the CPU. */
int rtl8196e_hw_l2_add_cpu_entry(struct rtl8196e_hw *hw, const u8 *mac, u8 fid, u32 portmask)
{
	static const u8 fid_hash[] = { 0x00, 0x0f, 0xf0, 0xff };
	u32 row;
	u32 index;
	u32 word0;
	u32 word1;
	u32 member;

	if (!mac)
		return -EINVAL;

	fid &= 0x3;
	row = (mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5] ^ fid_hash[fid]) & 0xff;
	index = (row << 2);

	word0 = ((u32)mac[1] << 24) | ((u32)mac[2] << 16) | ((u32)mac[3] << 8) | mac[4];
	member = ((portmask >> 6) & 0x7) << 14;
	member |= (portmask & 0x3f) << 8;
	word1 = (1 << 25) | /* auth */
		((u32)(fid & 0x3) << 23) |
		(1 << 22) | /* nhFlag */
		(0 << 21) | /* srcBlock */
		(3 << 19) | /* agingTime */
		(1 << 18) | /* isStatic */
		(1 << 17) | /* toCPU */
		member |
		(mac[0] & 0xff);

	return rtl8196e_l2_write_entry(index, word0, word1);
}

/* Add a static broadcast L2 entry to flood frames to all @portmask member ports. */
int rtl8196e_hw_l2_add_bcast_entry(struct rtl8196e_hw *hw, u8 fid, u32 portmask)
{
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	return rtl8196e_hw_l2_add_cpu_entry(hw, bcast, fid, portmask);
}

/* Verify the toCPU L2 entry for @mac is readable and matches the expected content. */
int rtl8196e_hw_l2_check_cpu_entry(struct rtl8196e_hw *hw, const u8 *mac, u8 fid)
{
	static const u8 fid_hash[] = { 0x00, 0x0f, 0xf0, 0xff };
	u32 row;
	u32 index;
	u32 word0;
	u32 word1;
	u32 expected0;
	u32 expected1;
	u32 mask;
	int ret;
	int tries;

	if (!mac)
		return -EINVAL;

	fid &= 0x3;
	row = (mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5] ^ fid_hash[fid]) & 0xff;
	index = (row << 2);

	expected0 = ((u32)mac[1] << 24) | ((u32)mac[2] << 16) | ((u32)mac[3] << 8) | mac[4];
	expected1 = (mac[0] & 0xff) |
		(1 << 17) | /* toCPU */
		(1 << 18) | /* isStatic */
		(1 << 22) | /* nhFlag */
		((u32)(fid & 0x3) << 23);
	mask = 0xff | (1 << 17) | (1 << 18) | (1 << 22) | (3 << 23);

	for (tries = 0; tries < 50; tries++) {
		ret = rtl8196e_l2_read_entry(index, &word0, &word1);
		if (ret)
			return ret;
		if (word0 == expected0 && (word1 & mask) == expected1)
			return 0;
		udelay(10);
	}

	pr_warn("rtl8196e-eth: L2 verify mismatch row=%u idx=%u exp0=0x%08x exp1=0x%08x got0=0x%08x got1=0x%08x\n",
		row, index, expected0, expected1, word0, word1);
	return -EIO;
}

/* Enable TX and RX DMA in CPUICR, then assert TRXRDY to start the switch. */
void rtl8196e_hw_start(struct rtl8196e_hw *hw)
{
	u32 icr = TXCMD | RXCMD | BUSBURST_32WORDS | MBUF_2048BYTES | EXCLUDE_CRC;
	rtl8196e_writel(icr, CPUICR);
	/* Start TX/RX after rings and CPUICR are set */
	rtl8196e_writel(TRXRDY, SIRR);
}

/* Disable TX and RX DMA in CPUICR and deassert TRXRDY. */
void rtl8196e_hw_stop(struct rtl8196e_hw *hw)
{
	u32 icr = rtl8196e_readl(CPUICR);
	icr &= ~(TXCMD | RXCMD);
	rtl8196e_writel(icr, CPUICR);
	rtl8196e_writel(0, SIRR);
}

/* Program all six CPURPDCR registers with the RX pkthdr ring base and CPURMDCR0 with mbuf base. */
void rtl8196e_hw_set_rx_rings(struct rtl8196e_hw *hw, void *pkthdr, void *mbuf)
{
	/*
	 * Callers pass KSEG1 (uncached) pointers from rtl8196e_alloc_uncached().
	 * Do NOT apply rtl8196e_uncached_addr() again — the OR is idempotent
	 * but masks a conceptual error about who owns the address conversion.
	 */
	rtl8196e_writel((u32)pkthdr, CPURPDCR0);
	rtl8196e_writel((u32)pkthdr, CPURPDCR1);
	rtl8196e_writel((u32)pkthdr, CPURPDCR2);
	rtl8196e_writel((u32)pkthdr, CPURPDCR3);
	rtl8196e_writel((u32)pkthdr, CPURPDCR4);
	rtl8196e_writel((u32)pkthdr, CPURPDCR5);
	rtl8196e_writel((u32)mbuf, CPURMDCR0);
}

/* Program CPUTPDCR0 with the TX pkthdr ring base address. */
void rtl8196e_hw_set_tx_ring(struct rtl8196e_hw *hw, void *pkthdr)
{
	/* Caller passes a KSEG1 pointer — do not double-convert */
	rtl8196e_writel((u32)pkthdr, CPUTPDCR0);
}

/*
 * Unmask RX done, link change and descriptor runout interrupts in CPUIIMR.
 * TX_ALL_DONE is intentionally NOT masked — TX reclaim is done in software
 * (in NAPI poll and start_xmit) to avoid one IRQ per TX completion.
 */
void rtl8196e_hw_enable_irqs(struct rtl8196e_hw *hw)
{
	u32 mask = RX_DONE_IE_ALL | LINK_CHANGE_IE | PKTHDR_DESC_RUNOUT_IE_ALL;
	rtl8196e_writel(mask, CPUIIMR);
}

/* Mask all interrupts by writing 0 to CPUIIMR. */
void rtl8196e_hw_disable_irqs(struct rtl8196e_hw *hw)
{
	rtl8196e_writel(0, CPUIIMR);
}
