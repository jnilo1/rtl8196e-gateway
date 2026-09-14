/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * swcore.h - Switch core driver API: swCore.c, swNic.c, swTable.c
 *
 * Bring-up of the switch and its one CPU port, the polled descriptor NIC,
 * and the table engine with the two entries the loader writes (one network
 * interface, one VLAN).  Derived from the Realtek loader's swCore.h,
 * swNic_poll.h, swTable.h, vlanTable.h and loader.h.
 *
 * Copyright (c) 2002-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */
#ifndef _SWCORE_H_
#define _SWCORE_H_

#include "rtl_types.h"
#include "stdlib.h"

/* Hardware state changes normally finish immediately.  Bound every poll so
 * a wedged switch leaves recovery available through the serial monitor. */
#define SWCORE_POLL_LIMIT 1000000U
static inline int swcore_wait_mask(unsigned long reg, uint32 mask,
				   uint32 expected)
{
	unsigned int count;

	for (count = 0; count < SWCORE_POLL_LIMIT; count++)
		if ((*(volatile uint32 *)reg & mask) == expected)
			return 1;
	return 0;
}

/* --- swCore.c ------------------------------------------------------------- */

int32 swCore_init(void);
int32 rtl8651_getAsicEthernetPHYReg(uint32 phyId, uint32 regId, uint32 *rData);
int32 rtl8651_setAsicEthernetPHYReg(uint32 phyId, uint32 regId, uint32 wData);

/* --- swNic.c: polled descriptor rings ------------------------------------- */

#define MBUF_LEN 2048
#define UNCACHED_MALLOC(x) (void *)(0xa0000000 | (uint32)malloc(x))

int32 swNic_init(uint32 userNeedRxPkthdrRingCnt[6], uint32 userNeedRxMbufRingCnt,
		 uint32 userNeedTxPkthdrRingCnt[4], uint32 clusterSize);
int32 swNic_receive(void **input, uint32 *pLen);
int32 swNic_send(void *output, uint32 len);
void swNic_txDone(void);
void swNic_wait_tx_idle(void);
uint32 swNic_rx_count(void);
void swNic_rx_resync(void);
extern unsigned int g_rx_runout;
extern unsigned int g_rx_resync;

/* --- swTable.c: table engine, network interface and VLAN entries ---------- */

int32 tableAccessForeword(uint32 tableType, uint32 eidx, void *entryContent_P);
int32 swTable_addEntry(uint32 tableType, uint32 eidx, void *entryContent_P);
int32 swTable_readEntry(uint32 tableType, uint32 eidx, void *entryContent_P);

typedef struct {
	macaddr_t gMac;
	uint16 macAddrNumber;
	uint16 vid;
	uint32 inAclStart, inAclEnd, outAclStart, outAclEnd;
	uint32 mtu;
	uint32 enableRoute : 1, valid : 1;
} rtl_netif_param_t;

typedef struct {
	uint32 memberPort;
	uint32 egressUntag;
	uint32 fid : 2;
	uint32 vid : 12;
} rtl_vlan_param_t;

int32 swCore_netifCreate(uint32 idx, rtl_netif_param_t *param);
int32 vlanTable_create(uint32 vid, rtl_vlan_param_t *param);

/*
 * Hardware layout of the table entries (8 words each, big-endian bit
 * allocation as the ASIC reads them through TCR0..TCR7).
 */
typedef struct {
	/* word 0 */
	uint16 mac39_24;
	uint16 mac23_8;
	/* word 1 */
	uint32 reserv0 : 6;
	uint32 auth : 1;
	uint32 fid : 2;
	uint32 nxtHostFlag : 1;
	uint32 srcBlock : 1;
	uint32 agingTime : 2;
	uint32 isStatic : 1;
	uint32 toCPU : 1;
	uint32 extMemberPort : 3;
	uint32 memberPort : 6;
	uint32 mac47_40 : 8;
	/* words 2..7 */
	uint32 reservw2;
	uint32 reservw3;
	uint32 reservw4;
	uint32 reservw5;
	uint32 reservw6;
	uint32 reservw7;
} rtl865xc_tblAsic_l2Table_t;

typedef struct {
	/* word 0 */
	uint32 vid : 12;
	uint32 fid : 2;
	uint32 extEgressUntag : 3;
	uint32 egressUntag : 6;
	uint32 extMemberPort : 3;
	uint32 memberPort : 6;
	/* words 1..7 */
	uint32 reservw1;
	uint32 reservw2;
	uint32 reservw3;
	uint32 reservw4;
	uint32 reservw5;
	uint32 reservw6;
	uint32 reservw7;
} vlan_table_t;

typedef struct {
	/* word 0 */
	uint32 mac18_0 : 19;
	uint32 vid : 12;
	uint32 valid : 1;
	/* word 1 */
	uint32 inACLStartL : 2;
	uint32 enHWRoute : 1;
	uint32 mac47_19 : 29;
	/* word 2 */
	uint32 mtuL : 3;
	uint32 macMask : 3;
	uint32 outACLEnd : 7;
	uint32 outACLStart : 7;
	uint32 inACLEnd : 7;
	uint32 inACLStartH : 5;
	/* word 3 */
	uint32 reserv10 : 20;
	uint32 mtuH : 12;
	/* words 4..7 */
	uint32 reservw4;
	uint32 reservw5;
	uint32 reservw6;
	uint32 reservw7;
} netif_table_t;

#endif /* _SWCORE_H_ */
