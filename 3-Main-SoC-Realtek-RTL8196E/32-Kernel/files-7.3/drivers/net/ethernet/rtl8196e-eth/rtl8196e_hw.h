/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8196e_hw.h - Public API for the RTL8196E hardware abstraction layer.
 */
#ifndef RTL8196E_HW_H
#define RTL8196E_HW_H

#include <linux/types.h>
#include <linux/io.h>
#include <linux/regmap.h>
#include "rtl8196e_regs.h"

struct rtl8196e_hw {
	struct regmap *syscon;
};

int rtl8196e_hw_init(struct rtl8196e_hw *hw);
int rtl8196e_hw_swcore_reset(struct rtl8196e_hw *hw);
void rtl8196e_hw_start(struct rtl8196e_hw *hw);
void rtl8196e_hw_stop(struct rtl8196e_hw *hw);
int rtl8196e_hw_init_phy(struct rtl8196e_hw *hw, int port, int phy_id);
bool rtl8196e_hw_link_up(struct rtl8196e_hw *hw, int port);
void rtl8196e_hw_link_status(struct rtl8196e_hw *hw, int port,
			     bool *link, int *speed_mbps, bool *full_duplex);
void rtl8196e_hw_l2_setup(struct rtl8196e_hw *hw);
void rtl8196e_hw_l2_trap_enable(struct rtl8196e_hw *hw);
int rtl8196e_hw_l2_add_cpu_entry(struct rtl8196e_hw *hw, const u8 *mac, u8 fid, u32 portmask);
int rtl8196e_hw_l2_check_cpu_entry(struct rtl8196e_hw *hw, const u8 *mac, u8 fid);
int rtl8196e_hw_l2_add_bcast_entry(struct rtl8196e_hw *hw, u8 fid, u32 portmask);
int rtl8196e_hw_vlan_setup(struct rtl8196e_hw *hw, u16 vid, u8 fid,
			   u32 member_ports, u32 untag_ports);
int rtl8196e_hw_netif_setup(struct rtl8196e_hw *hw, const u8 *mac, u16 vid,
			    u16 mtu, u32 member_ports);

void rtl8196e_hw_set_rx_rings(struct rtl8196e_hw *hw, void *pkthdr, void *mbuf);
void rtl8196e_hw_set_tx_ring(struct rtl8196e_hw *hw, void *pkthdr);

void rtl8196e_hw_enable_irqs(struct rtl8196e_hw *hw);
void rtl8196e_hw_disable_irqs(struct rtl8196e_hw *hw);

#endif /* RTL8196E_HW_H */
