// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * swCore.c - Switch core and PHY initialization
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2002-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "boot_soc.h"
#include "boot_net.h"
#include <rtl_types.h>
#include "swcore_regs.h"
#include "swcore.h"

#include "main.h"

#define WRITE_MEM32(addr, val) (*(volatile unsigned int *)(addr)) = (val)
#define WRITE_MEM16(addr, val) (*(volatile unsigned short *)(addr)) = (val)
#define READ_MEM32(addr) (*(volatile unsigned int *)(addr))

#define RTL8651_ETHER_AUTO_100FULL 0x00
#define RTL8651_ETHER_AUTO_100HALF 0x01
#define RTL8651_ETHER_AUTO_10FULL 0x02
#define RTL8651_ETHER_AUTO_10HALF 0x03
#define RTL8651_ETHER_AUTO_1000FULL 0x08
#define RTL8651_ETHER_AUTO_1000HALF 0x09
#define GIGA_PHY_ID 0x16
#define tick_Delay10ms(x)                                                      \
	{                                                                      \
		int i = x;                                                     \
		while (i--)                                                    \
			__delay(5000);                                         \
	}

static uint8 fidHashTable[] = {0x00, 0x0f, 0xf0, 0xff};

/*#define rtl8651_asicTableAccessAddrBase(type)
 * (RTL8651_ASICTABLE_BASE_OF_ALL_TABLES + 0x10000 * (type)) */
#define RTL8651_ASICTABLE_BASE_OF_ALL_TABLES 0xBB000000
#define rtl8651_asicTableAccessAddrBase(type)                                  \
	(RTL8651_ASICTABLE_BASE_OF_ALL_TABLES + ((type) << 16))
#define RTL8651_ASICTABLE_ENTRY_LENGTH (8 * sizeof(uint32))
#define RTL865X_TLU_BUG_FIXED 1

static uint32 _rtl8651_asicTableSize[] = {
    2 /*TYPE_L2_SWITCH_TABLE*/,	    1 /*TYPE_ARP_TABLE*/,
    2 /*TYPE_L3_ROUTING_TABLE*/,    3 /*TYPE_MULTICAST_TABLE*/,
    1 /*TYPE_PROTOCOL_TRAP_TABLE*/, 5 /*TYPE_VLAN_TABLE*/,
    3 /*TYPE_EXT_INT_IP_TABLE*/,    1 /*TYPE_ALG_TABLE*/,
    4 /*TYPE_SERVER_PORT_TABLE*/,   3 /*TYPE_L4_TCP_UDP_TABLE*/,
    3 /*TYPE_L4_ICMP_TABLE*/,	    1 /*TYPE_PPPOE_TABLE*/,
    8 /*TYPE_ACL_RULE_TABLE*/,	    1 /*TYPE_NEXT_HOP_TABLE*/,
    3 /*TYPE_RATE_LIMIT_TABLE*/,
};

static int32 _rtl8651_asicTableAccessForward(uint32 tableType, uint32 eidx,
					     void *entryContent_P)
{
	ASSERT_CSP(entryContent_P);
	if (!swcore_wait_mask(SWTACR, ACTION_MASK, ACTION_DONE))
		return FAILED;

	{
		register uint32 index;
		for (index = 0; index < _rtl8651_asicTableSize[tableType];
		     index++) {
			WRITE_MEM32(TCR0 + (index << 2),
				    *((uint32 *)entryContent_P + index));
		}
	}
	WRITE_MEM32(SWTAA,
		    ((uint32)rtl8651_asicTableAccessAddrBase(tableType) +
		     eidx * RTL8651_ASICTABLE_ENTRY_LENGTH)); // Fill address
	return SUCCESS;
}

static int32 _rtl8651_forceAddAsicEntry(uint32 tableType, uint32 eidx,
					void *entryContent_P)
{

	if (RTL865X_TLU_BUG_FIXED) /* No need to stop HW table lookup process */
	{			   /* No need to stop HW table lookup process */
		WRITE_MEM32(SWTCR0, EN_STOP_TLU | READ_MEM32(SWTCR0));
		if (!swcore_wait_mask(SWTCR0, STOP_TLU_READY, STOP_TLU_READY)) {
			WRITE_MEM32(SWTCR0, ~EN_STOP_TLU & READ_MEM32(SWTCR0));
			return FAILED;
		}
	}

	if (_rtl8651_asicTableAccessForward(tableType, eidx, entryContent_P) !=
	    SUCCESS)
		goto out_tlu;

	WRITE_MEM32(SWTACR, ACTION_START | CMD_FORCE); // Activate add command
	if (!swcore_wait_mask(SWTACR, ACTION_MASK, ACTION_DONE))
		goto out_tlu;

	if (RTL865X_TLU_BUG_FIXED) /* No need to stop HW table lookup process */
	{
		WRITE_MEM32(SWTCR0, ~EN_STOP_TLU & READ_MEM32(SWTCR0));
	}

	return SUCCESS;

out_tlu:
	if (RTL865X_TLU_BUG_FIXED)
		WRITE_MEM32(SWTCR0, ~EN_STOP_TLU & READ_MEM32(SWTCR0));
	return FAILED;
}

uint32 rtl8651_filterDbIndex(ether_addr_t *macAddr, uint16 fid)
{
	return (macAddr->octet[0] ^ macAddr->octet[1] ^ macAddr->octet[2] ^
		macAddr->octet[3] ^ macAddr->octet[4] ^ macAddr->octet[5] ^
		fidHashTable[fid]) &
	       0xFF;
}

static int32 rtl8651_setAsicL2Table(ether_addr_t *mac, uint32 column)
{
	rtl865xc_tblAsic_l2Table_t entry;
	uint32 row;

	row = rtl8651_filterDbIndex(mac, 0);
	if ((row >= RTL8651_L2TBL_ROW) || (column >= RTL8651_L2TBL_COLUMN))
		return FAILED;
	if (mac->octet[5] !=
	    ((row ^ (fidHashTable[0]) ^ mac->octet[0] ^ mac->octet[1] ^
	      mac->octet[2] ^ mac->octet[3] ^ mac->octet[4]) &
	     0xff))
		return FAILED;

	memset(&entry, 0, sizeof(entry));
	entry.mac47_40 = mac->octet[0];
	entry.mac39_24 = (mac->octet[1] << 8) | mac->octet[2];
	entry.mac23_8 = (mac->octet[3] << 8) | mac->octet[4];
	entry.memberPort = 7;
	entry.toCPU = 1;
	entry.isStatic = 1;

	/* RTL865xC: modification of age from ( 2 -> 3 -> 1 -> 0 ) to ( 3 -> 2
	 * -> 1 -> 0 ). modification of granularity 100 sec to 150 sec. */
	entry.agingTime = 0x03;
	entry.fid = 0;
	entry.auth = 1;

	return _rtl8651_forceAddAsicEntry(TYPE_L2_SWITCH_TABLE,
					  row << 2 | column, &entry);
}

//------------------------------------------------------------------------
static int32 _rtl8651_clearSpecifiedAsicTable(uint32 type, uint32 count)
{
	struct {
		uint32 _content[8];
	} entry;
	uint32 idx;
	memset(&entry, 0, sizeof(entry));
	for (idx = 0; idx < count; idx++) /* Write into hardware */
		if (swTable_addEntry(type, idx, &entry) != SUCCESS)
			return FAILED;
	return SUCCESS;
}

void FullAndSemiReset(void)
{
	REG32(SYS_CLKMANAGE) &= ~(1 << 11); /* active_swcore=0 */
	__delay(5000);
	REG32(SYS_CLKMANAGE) |= (1 << 11); /* active_swcore=1 */
	__delay(1000);
}

/**
 * swCore_quiesce - Stop the switch before the CPU hands off or resets
 *
 * A live switch can DMA inbound frames into DRAM during early kernel boot
 * (or across a watchdog reset, which preserves DRAM and does not reset the
 * switch) and corrupt it — the intermittent post-flash boot loop.  Turning
 * the PHY interface off stops new ingress but not an already-armed DMA, so:
 * stop the CPU-port DMA engine, hard-reset the switch core (the same
 * active_swcore reset swCore_init() runs on every boot, which aborts any
 * in-flight transfer), THEN hold the PHY interface off — the reset
 * re-defaults PCRP, so PHY-off must come after it.  Used by the kernel
 * hand-off, the post-flash reboot and the `J` command alike.
 */
void swCore_quiesce(void)
{
	WRITE_MEM32(CPUICR, 0); /* stop CPU-port RX/TX DMA (clears TXCMD|RXCMD) */
	FullAndSemiReset();	/* hard-reset switch core — flush in-flight DMA */
	WRITE_MEM32(PCRP0, (READ_MEM32(PCRP0) & (~EnablePHYIf)));
	WRITE_MEM32(PCRP1, (READ_MEM32(PCRP1) & (~EnablePHYIf)));
	WRITE_MEM32(PCRP2, (READ_MEM32(PCRP2) & (~EnablePHYIf)));
	WRITE_MEM32(PCRP3, (READ_MEM32(PCRP3) & (~EnablePHYIf)));
	WRITE_MEM32(PCRP4, (READ_MEM32(PCRP4) & (~EnablePHYIf)));
}

/**
 * rtl8651_getAsicEthernetPHYReg - Read a PHY register via MDIO
 * @phyId: PHY address (0-31)
 * @regId: register number within the PHY
 * @rData: output pointer for the 16-bit register value
 *
 * Return: SUCCESS on completion
 */
int32 rtl8651_getAsicEthernetPHYReg(uint32 phyId, uint32 regId, uint32 *rData)
{
	uint32 status;

	WRITE_MEM32(MDCIOCR, COMMAND_READ | (phyId << PHYADD_OFFSET) |
				 (regId << REGADD_OFFSET));

	/* 8196C test-chip patch inherited from the SDK: MDIO read data
	 * lags one MDC clock.  Needs the timer tick, which download mode
	 * re-arms before the switch is brought up. */
	delay_ms(10);
	if (!swcore_wait_mask(MDCIOSR, STATUS, 0))
		return FAILED;
	status = READ_MEM32(MDCIOSR);

	status &= 0xffff;
	*rData = status;

	return SUCCESS;
}

/**
 * rtl8651_setAsicEthernetPHYReg - Write a PHY register via MDIO
 * @phyId: PHY address (0-31)
 * @regId: register number within the PHY
 * @wData: 16-bit value to write
 *
 * Return: SUCCESS on completion
 */
int32 rtl8651_setAsicEthernetPHYReg(uint32 phyId, uint32 regId, uint32 wData)
{
	WRITE_MEM32(MDCIOCR, COMMAND_WRITE | (phyId << PHYADD_OFFSET) |
				 (regId << REGADD_OFFSET) | wData);

	if (!swcore_wait_mask(MDCIOSR, STATUS, 0))
		return FAILED;

	return SUCCESS;
}

int32 rtl8651_restartAsicEthernetPHYNway(uint32 port, uint32 phyid)
{
	uint32 statCtrlReg0;

	/* read current PHY reg 0 */
	if (rtl8651_getAsicEthernetPHYReg(phyid, 0, &statCtrlReg0) != SUCCESS)
		return FAILED;

	/* enable 'restart Nway' bit */
	statCtrlReg0 |= RESTART_AUTONEGO;

	/* write PHY reg 0 */
	return rtl8651_setAsicEthernetPHYReg(phyid, 0, statCtrlReg0);
}

//====================================================================

static int Set_GPHYWB(unsigned int phyid, unsigned int page, unsigned int reg,
		      unsigned int mask, unsigned int val)
{

	unsigned int data = 0;
	unsigned int wphyid = 0;     // start
	unsigned int wphyid_end = 1; // end
	if (phyid == 999) {
		wphyid = 0;
		wphyid_end = 5; // total phyid=0~4
	} else {
		wphyid = phyid;
		wphyid_end = phyid + 1;
	}

	for (; wphyid < wphyid_end; wphyid++) {
		// change page

		if (page >= 31) {
			if (rtl8651_setAsicEthernetPHYReg(wphyid, 31, 7) != SUCCESS ||
			    rtl8651_setAsicEthernetPHYReg(wphyid, 30, page) != SUCCESS)
				return FAILED;
		} else {
			if (rtl8651_setAsicEthernetPHYReg(wphyid, 31, page) != SUCCESS)
				return FAILED;
		}
		if (mask != 0) {
			if (rtl8651_getAsicEthernetPHYReg(wphyid, reg, &data) != SUCCESS)
				return FAILED;
			data = data & mask;
		}
		if (rtl8651_setAsicEthernetPHYReg(wphyid, reg, data | val) !=
		    SUCCESS)
			return FAILED;

		// switch to page 0
		if (rtl8651_setAsicEthernetPHYReg(wphyid, 31, 0) != SUCCESS)
			return FAILED;
	}
	return SUCCESS;
}

unsigned int Get_P0_PhyMode()
{
	/*
		00: External  phy
		01: embedded phy
		10: olt
		11: deb_sel
	*/
#define GET_BITVAL(v, bitpos, pat)                                             \
	((v & ((unsigned int)pat << bitpos)) >> bitpos)
#define RANG1 1
#define RANG2 3
#define RANG3 7
#define RANG4 0xf

	unsigned int v = REG32(HW_STRAP_REG);
	unsigned int mode =
	    GET_BITVAL(v, 6, RANG1) * 2 + GET_BITVAL(v, 7, RANG1);
	return (mode & 3);
}

unsigned int Get_P0_MiiMode()
{
	/*
		0: MII-PHY
		1: MII-MAC
		2: GMII-MAC
		3: RGMII
	*/
#define GET_BITVAL(v, bitpos, pat)                                             \
	((v & ((unsigned int)pat << bitpos)) >> bitpos)
#define RANG1 1
#define RANG2 3
#define RANG3 7
#define RANG4 0xf

	unsigned int v = REG32(HW_STRAP_REG);
	unsigned int mode = GET_BITVAL(v, 27, RANG2);
	return mode;
}

unsigned int Get_P0_RxDelay()
{
#define GET_BITVAL(v, bitpos, pat)                                             \
	((v & ((unsigned int)pat << bitpos)) >> bitpos)
#define RANG1 1
#define RANG2 3
#define RANG3 7
#define RANG4 0xf

	unsigned int v = REG32(HW_STRAP_REG);
	unsigned int val = GET_BITVAL(v, 29, RANG3);
	return val;
}

unsigned int Get_P0_TxDelay()
{
#define GET_BITVAL(v, bitpos, pat)                                             \
	((v & ((unsigned int)pat << bitpos)) >> bitpos)
#define RANG1 1
#define RANG2 3
#define RANG3 7
#define RANG4 0xf

	unsigned int v = REG32(HW_STRAP_REG);
	unsigned int val = GET_BITVAL(v, 17, RANG1);
	return val;
}

#define SYS_ECO_NO 0xb8000000
/**
 * Setting_RTL8196E_PHY - Apply RTL8196E-specific PHY tuning
 *
 * Adjusts Iq current, power saving, SNR, and half-duplex backpressure
 * settings.  Handles both A-cut and B-cut silicon variants.
 *
 * Return: 0 on success
 */
int Setting_RTL8196E_PHY(void)
{
	int i;

	for (i = 0; i < 5; i++)
		REG32(PCRP0 + i * 4) |= (EnForceMode);

	// write page1, reg16, bit[15:13] Iq Current 110:175uA (default 100:
	// 125uA)
	if (Set_GPHYWB(999, 1, 16, 0xffff - (0x7 << 13), 0x6 << 13) !=
	    SUCCESS)
		return FAILED;

	if (REG32(SYS_ECO_NO) == 0x8196e000) {
		// disable power saving mode in A-cut only
		if (Set_GPHYWB(999, 0, 0x18, 0xffff - (1 << 15), 0 << 15) !=
		    SUCCESS)
			return FAILED;
	}
	/* B-cut and later,
	    just increase a little power in long RJ45 cable case for Green
	   Ethernet feature.
	 */
	else {
		// adtune_lb setting
		if (Set_GPHYWB(999, 0, 22, 0xffff - (0x7 << 4), 0x4 << 4) !=
		    SUCCESS)
			return FAILED;
		// Setting SNR lb and hb
		if (Set_GPHYWB(999, 0, 21, 0xffff - (0xff << 0), 0xc2 << 0) !=
		    SUCCESS)
			return FAILED;
		// auto bais current
		if (Set_GPHYWB(999, 1, 19, 0xffff - (0x1 << 0), 0x0 << 0) !=
		    SUCCESS ||
		    Set_GPHYWB(999, 0, 22, 0xffff - (0x1 << 3), 0x0 << 3) !=
		    SUCCESS)
			return FAILED;
	}

	/* 100M half duplex enhancement */
	/* fix Smartbit Half duplex backpressure IOT issue */
	REG32(MACCR) = (REG32(MACCR) & ~(CF_RXIPG_MASK | SELIPG_MASK)) |
		       (0x05 | SELIPG_11);

	for (i = 0; i < 5; i++)
		REG32(PCRP0 + i * 4) &= ~(EnForceMode);

	return 0;
}

#define REG32_ANDOR(x, y, z) (REG32(x) = (REG32(x) & (y)) | (z))
/**
 * swCore_init - Initialize the RTL8196E switch core
 *
 * Performs a full/semi reset, configures the embedded PHY (or external
 * PHY via MII/RGMII), sets port VIDs, enables L2 lookup, starts
 * TX/RX, programs PHY LEDs, enables flow control, and adds the
 * bootloader's MAC to the L2 table.
 *
 * Return: 0 on success
 */
int32 swCore_init()
{
	int port;
	unsigned int P0phymode;
	unsigned int P0miimode;
	unsigned int P0txdly;
	unsigned int P0rxdly;

	/* Full reset and semreset */
	FullAndSemiReset();

	if (Setting_RTL8196E_PHY() != SUCCESS)
		return FAILED;

	/* rtl8651_clearAsicAllTable */
	REG32(MEMCR) = 0;
	REG32(MEMCR) = 0x7f;
	if (_rtl8651_clearSpecifiedAsicTable(TYPE_MULTICAST_TABLE,
					 RTL8651_IPMULTICASTTBL_SIZE) != SUCCESS ||
	    _rtl8651_clearSpecifiedAsicTable(TYPE_NETINTERFACE_TABLE,
					 RTL865XC_NETINTERFACE_NUMBER) != SUCCESS)
		return FAILED;
	// anson add
	REG32(PIN_MUX_SEL2) = 0;
	REG32(PCRP0) &= (0xFFFFFFFF - (0x00000000 | MacSwReset));
	REG32(PCRP1) &= (0xFFFFFFFF - (0x00000000 | MacSwReset));
	REG32(PCRP2) &= (0xFFFFFFFF - (0x00000000 | MacSwReset));
	REG32(PCRP3) &= (0xFFFFFFFF - (0x00000000 | MacSwReset));
	REG32(PCRP4) &= (0xFFFFFFFF - (0x00000000 | MacSwReset));
	REG32(PCRP1) =
	    REG32(PCRP1) | (1 << ExtPHYID_OFFSET) | EnablePHYIf | MacSwReset;
	REG32(PCRP2) =
	    REG32(PCRP2) | (2 << ExtPHYID_OFFSET) | EnablePHYIf | MacSwReset;
	REG32(PCRP3) =
	    REG32(PCRP3) | (3 << ExtPHYID_OFFSET) | EnablePHYIf | MacSwReset;
	REG32(PCRP4) =
	    REG32(PCRP4) | (4 << ExtPHYID_OFFSET) | EnablePHYIf | MacSwReset;

	/* Port 0 uses the embedded PHY on every supported board. */
	P0phymode = 1;
	P0miimode = 0;

	if (P0phymode == 1) // embedded phy
	{
		REG32(PCRP0) |= (0 << ExtPHYID_OFFSET) | EnablePHYIf |
				MacSwReset; // emabedded
	} else				    // external phy
	{
		REG32(PCRP0) |= (0x06 << ExtPHYID_OFFSET) | MIIcfg_RXER |
				EnablePHYIf | MacSwReset; // external
		{
			uint32 reg;

			// enable flow control ability
			if (rtl8651_getAsicEthernetPHYReg(0x06, 4, &reg) != SUCCESS)
				return FAILED;
			reg |= (BIT(10) | BIT(11));
			if (rtl8651_setAsicEthernetPHYReg(0x06, 4, reg) != SUCCESS)
				return FAILED;
		}

		if ((P0miimode == 2) || (P0miimode == 3)) {
			REG32(MACCR) |= (1 << 12); // giga link
		}

		// unsigned int P0miimode=1;
		const unsigned char *miimodename[] = {"MII-PHY", "MII-MAC",
						      "GMII-MAC", "RGMII"};
		printf("P0miimode=%02x, %s\n", P0miimode,
		       miimodename[P0miimode]);

		if (P0miimode == 0)
			REG32_ANDOR(P0GMIICR, ~(3 << 23), LINK_MII_PHY << 23);
		else if (P0miimode == 1)
			REG32_ANDOR(P0GMIICR, ~(3 << 23), LINK_MII_MAC << 23);
		else if (P0miimode == 2)
			REG32_ANDOR(P0GMIICR, ~(3 << 23),
				    LINK_MII_MAC << 23); // GMII
		else if (P0miimode == 3)
			REG32_ANDOR(P0GMIICR, ~(3 << 23), LINK_RGMII << 23);

		if (P0miimode == 3) {
			P0txdly = 1; // Get_P0_TxDelay();
			P0rxdly = 3; // Get_P0_RxDelay();
			REG32_ANDOR(P0GMIICR, ~((1 << 4) | (3 << 0)),
				    (P0txdly << 4) | (P0rxdly << 0));
		}

		REG32(PITCR) |= (1 << 0); // 00: embedded , 01L GMII/MII/RGMII
		REG32(P0GMIICR) |= (Conf_done);
	}

	if ((REG32(BOND_OPTION) & BOND_ID_MASK) == BOND_8196ES) {
		P0phymode = Get_P0_PhyMode();

		if (P0phymode == 1) // embedded phy
		{
			REG32(PCRP0) |= (0 << ExtPHYID_OFFSET) | EnablePHYIf |
					MacSwReset; // emabedded
		} else				    // external phy
		{
			REG32(PCRP0) |= (0x06 << ExtPHYID_OFFSET) |
					MIIcfg_RXER | EnablePHYIf |
					MacSwReset; // external

			P0miimode = Get_P0_MiiMode();

			if (P0miimode == 0)
				REG32_ANDOR(P0GMIICR, ~(3 << 23),
					    LINK_MII_PHY << 23);
			else if ((P0miimode == 1) || (P0miimode == 2))
				REG32_ANDOR(P0GMIICR, ~(3 << 23),
					    LINK_MII_MAC << 23);
			else if (P0miimode == 3)
				REG32_ANDOR(P0GMIICR, ~(3 << 23),
					    LINK_RGMII << 23);

			if (P0miimode == 3) {
				P0txdly = Get_P0_TxDelay();
				P0rxdly = Get_P0_RxDelay();
				REG32_ANDOR(P0GMIICR, ~((1 << 4) | (3 << 0)),
					    (P0txdly << 4) | (P0rxdly << 0));
			}

			if ((P0miimode == 0) || (P0miimode == 1))
				REG32_ANDOR(PCRP0, ~AutoNegoSts_MASK,
					    EnForceMode | ForceLink |
						ForceSpeed100M | ForceDuplex);
			else if ((P0miimode == 2) || (P0miimode == 3)) {
				REG32_ANDOR(PCRP0, ~AutoNegoSts_MASK,
					    EnForceMode | ForceLink |
						ForceSpeed1000M | ForceDuplex);
				REG32(MACCR) |= (1 << 12); // giga link
			}
			REG32(PITCR) |=
			    (1 << 0); // 00: embedded , 01L GMII/MII/RGMII
			REG32(P0GMIICR) |= (Conf_done);
		}
	}

	/* Set PVID of all ports to 8 */
	REG32(PVCR0) = (0x8 << 16) | 0x8;
	REG32(PVCR1) = (0x8 << 16) | 0x8;
	REG32(PVCR2) = (0x8 << 16) | 0x8;
	REG32(PVCR3) = (0x8 << 16) | 0x8;

	/* Enable L2 lookup engine and spanning tree functionality */
	REG32(MSCR) = EN_L2;
	REG32(QNUMCR) = P0QNum_1 | P1QNum_1 | P2QNum_1 | P3QNum_1 | P4QNum_1;

	/* Start normal TX and RX */
	REG32(SIRR) |= TRXRDY;

	/* Init PHY LED style */
	/*
		#LED = direct mode
		set mode 0x0
		swwb 0xbb804300 21-20 0x2 19-18 $mode 17-16 $mode 15-14 $mode
	   13-12 $mode 11-10 $mode 9-8 $mode
	*/
	REG32(PIN_MUX_SEL) &= ~((3 << 8) | (3 << 10) | (3 << 3) |
				(1 << 15)); // let P0 to mii mode
	REG32(PIN_MUX_SEL2) &= ~((3 << 0) | (3 << 3) | (3 << 6) | (3 << 9) |
				 (3 << 12) | (7 << 15)); // S0-S3, P0-P1
	REG32(LEDCR) = (2 << 20) | (0 << 18) | (0 << 16) | (0 << 14) |
		       (0 << 12) | (0 << 10) | (0 << 8); // P0-P5

	/*PHY FlowControl. Default enable*/
	for (port = 0; port < MAX_PORT_NUMBER; port++) {
		/* Set Flow Control capability. */
		if (rtl8651_restartAsicEthernetPHYNway(port + 1, port) != SUCCESS)
			return FAILED;
	}

	if (rtl8651_setAsicL2Table((ether_addr_t *)(&eth0_mac), 0) != SUCCESS)
		return FAILED;

	REG32(FFCR) = EN_UNUNICAST_TOCPU |
		      EN_UNMCAST_TOCPU; // rx broadcast and unicast packet
	return 0;
}
