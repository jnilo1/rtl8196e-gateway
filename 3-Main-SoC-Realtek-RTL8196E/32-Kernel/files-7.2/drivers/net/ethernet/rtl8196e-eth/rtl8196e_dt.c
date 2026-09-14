// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8196e_dt.c - Devicetree parsing for the RTL8196E Ethernet interface node.
 *
 * Reads properties from the &ethernet / interface@0 DT node and fills
 * struct rtl8196e_dt_iface for use by the driver core.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/string.h>
#include <linux/if_ether.h>
#include "rtl8196e_dt.h"

/*
 * RTL8196E switch exposes 9 ports (0..8) — the HW layer iterates port < 9
 * and packs the member-port mask as bits[5:0] | (bits[8:6] << 6) into the
 * VLAN word0 register. Bit 5 is the internal CPU port; users normally do
 * not list it in member-ports for an external interface but the driver
 * does not forbid it (operator-level concern, not a driver invariant).
 */
#define RTL8196E_VALID_PORT_MASK 0x1ff

/* Populate @iface with safe default values (port 4, VLAN 1, MTU 1500, eth0). */
static void rtl8196e_dt_defaults(struct rtl8196e_dt_iface *iface)
{
	strscpy(iface->ifname, "eth0", sizeof(iface->ifname));
	iface->vlan_id = 1;
	iface->member_ports = 0x10; /* port 4 */
	iface->untag_ports = 0x10;
	iface->mtu = 1500;
	iface->mac_set = false;
	iface->phy_id = 4;
	iface->phy_id_set = false;
	iface->link_poll_ms = 0;
	iface->link_poll_ms_set = false;
}

/* Find the interface@0 child node under @np, first by reg=<0>, then by node name. */
static struct device_node *rtl8196e_dt_find_iface(struct device_node *np)
{
	struct device_node *child;
	u32 reg;

	for_each_child_of_node(np, child) {
		if (!of_property_read_u32(child, "reg", &reg) && reg == 0)
			return child;
	}

	for_each_child_of_node(np, child) {
		if (of_node_name_eq(child, "interface@0"))
			return child;
	}

	return NULL;
}

/* Parse the &ethernet DT node and its interface@0 child; fill @iface with the result. */
int rtl8196e_dt_parse(struct device *dev, struct rtl8196e_dt_iface *iface)
{
	struct device_node *np = dev->of_node;
	struct device_node *if_np;
	const char *ifname;

	rtl8196e_dt_defaults(iface);

	if (!np)
		return -EINVAL;

	if (!of_property_read_u32(np, "link-poll-ms", &iface->link_poll_ms))
		iface->link_poll_ms_set = true;

	if_np = rtl8196e_dt_find_iface(np);
	if (!if_np) {
		dev_warn(dev, "no interface@0 node found, using defaults\n");
		return 0;
	}

	if (!of_property_read_string(if_np, "ifname", &ifname))
		strscpy(iface->ifname, ifname, sizeof(iface->ifname));

	/* of_get_mac_address signature changed in 6.x: now returns int, takes output buffer */
	if (of_get_mac_address(if_np, iface->mac) == 0)
		iface->mac_set = true;

	of_property_read_u32(if_np, "vlan-id", &iface->vlan_id);
	of_property_read_u32(if_np, "member-ports", &iface->member_ports);
	of_property_read_u32(if_np, "untag-ports", &iface->untag_ports);
	of_property_read_u32(if_np, "mtu", &iface->mtu);
	if (!of_property_read_u32(if_np, "phy-id", &iface->phy_id))
		iface->phy_id_set = true;
	if (!of_property_read_u32(if_np, "link-poll-ms", &iface->link_poll_ms))
		iface->link_poll_ms_set = true;

	if (iface->vlan_id == 0 || iface->vlan_id >= 4096) {
		dev_err(dev, "invalid vlan-id %u (must be 1-4095)\n", iface->vlan_id);
		of_node_put(if_np);
		return -EINVAL;
	}
	if (iface->member_ports == 0) {
		dev_err(dev, "member-ports cannot be 0\n");
		of_node_put(if_np);
		return -EINVAL;
	}
	if (iface->member_ports & ~RTL8196E_VALID_PORT_MASK) {
		dev_err(dev, "member-ports 0x%x has bits outside the 9-port range (0x%x)\n",
			iface->member_ports, RTL8196E_VALID_PORT_MASK);
		of_node_put(if_np);
		return -EINVAL;
	}
	if (iface->untag_ports & ~iface->member_ports) {
		dev_err(dev, "untag-ports 0x%x is not a subset of member-ports 0x%x\n",
			iface->untag_ports, iface->member_ports);
		of_node_put(if_np);
		return -EINVAL;
	}
	if (iface->mtu < 576 || iface->mtu > 1500) {
		dev_err(dev, "invalid mtu %u (must be 576-1500)\n", iface->mtu);
		of_node_put(if_np);
		return -EINVAL;
	}
	if (iface->phy_id_set && iface->phy_id > 31) {
		dev_err(dev, "invalid phy-id %u (must be 0-31, MDIO address)\n",
			iface->phy_id);
		of_node_put(if_np);
		return -EINVAL;
	}

	of_node_put(if_np);
	return 0;
}
