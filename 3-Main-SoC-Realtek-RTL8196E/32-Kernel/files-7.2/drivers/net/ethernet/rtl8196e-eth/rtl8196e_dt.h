/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8196e_dt.h - Devicetree interface descriptor and parser API.
 */
#ifndef RTL8196E_DT_H
#define RTL8196E_DT_H

#include <linux/types.h>

struct device;

struct rtl8196e_dt_iface {
	char ifname[16];
	u8 mac[6];
	bool mac_set;
	u32 vlan_id;
	u32 member_ports;
	u32 untag_ports;
	u32 mtu;
	u32 phy_id;
	bool phy_id_set;
	u32 link_poll_ms;
	bool link_poll_ms_set;
};

int rtl8196e_dt_parse(struct device *dev, struct rtl8196e_dt_iface *iface);

#endif /* RTL8196E_DT_H */
