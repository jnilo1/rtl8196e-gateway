// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * swTable.c - Switch ASIC table access and VLAN management
 *
 * RTL8196E stage-2 bootloader
 *
 * Copyright (c) 2002-2020 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */

#include "boot_common.h"
#include "boot_soc.h"
#include <rtl_types.h>
#include "swcore_regs.h"
#include "swcore.h"
#include "main.h"

/**
 * swTable_addEntry - Write an entry to a switch ASIC table
 * @tableType: table identifier (TYPE_VLAN_TABLE, etc.)
 * @eidx: entry index within the table
 * @entryContent_P: pointer to the entry data (8 x uint32)
 *
 * Stops the table lookup unit, writes the entry via TCR registers,
 * issues an ADD command, and waits for completion.
 *
 * Return: 0 on success, ECOLLISION on hash collision
 */
int32 swTable_addEntry(uint32 tableType, uint32 eidx, void *entryContent_P)
{
	REG32(SWTCR0) = REG32(SWTCR0) | EN_STOP_TLU;
	if (!swcore_wait_mask(SWTCR0, STOP_TLU_READY, STOP_TLU_READY)) {
		REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;
		return FAILED;
	}

	if (tableAccessForeword(tableType, eidx, entryContent_P) != SUCCESS) {
		REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;
		return FAILED;
	}

	/* Activate add command */
	REG32(SWTACR) = ACTION_START | CMD_ADD;

	/* Wait for command done */
	if (!swcore_wait_mask(SWTACR, ACTION_MASK, ACTION_DONE)) {
		REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;
		return FAILED;
	}

	REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;

	/* Check status */
	if ((REG32(SWTASR) & TABSTS_MASK) != TABSTS_SUCCESS)
		return ECOLLISION;
	else
		return 0;
}

/**
 * swTable_readEntry - Read an entry from a switch ASIC table
 * @tableType: table identifier
 * @eidx: entry index
 * @entryContent_P: output buffer (8 x uint32)
 *
 * Return: 0 on success
 */
int32 swTable_readEntry(uint32 tableType, uint32 eidx, void *entryContent_P)
{
	uint32 *entryAddr;

	REG32(SWTCR0) = REG32(SWTCR0) | EN_STOP_TLU;
	if (!swcore_wait_mask(SWTCR0, STOP_TLU_READY, STOP_TLU_READY)) {
		REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;
		return FAILED;
	}

	ASSERT_CSP(entryContent_P);

	entryAddr = (uint32 *)(table_access_addr_base(tableType) +
			       eidx * TABLE_ENTRY_DISTANCE);

	/* Wait for command ready */
	if (!swcore_wait_mask(SWTACR, ACTION_MASK, ACTION_DONE)) {
		REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;
		return FAILED;
	}

	/* Read registers according to entry width of each table */
	*((uint32 *)entryContent_P + 7) = *(entryAddr + 7);
	*((uint32 *)entryContent_P + 6) = *(entryAddr + 6);
	*((uint32 *)entryContent_P + 5) = *(entryAddr + 5);
	*((uint32 *)entryContent_P + 4) = *(entryAddr + 4);
	*((uint32 *)entryContent_P + 3) = *(entryAddr + 3);
	*((uint32 *)entryContent_P + 2) = *(entryAddr + 2);
	*((uint32 *)entryContent_P + 1) = *(entryAddr + 1);
	*((uint32 *)entryContent_P + 0) = *(entryAddr + 0);

	REG32(SWTCR0) = REG32(SWTCR0) & ~EN_STOP_TLU;

	return 0;
}

int32 tableAccessForeword(uint32 tableType, uint32 eidx, void *entryContent_P)
{
	ASSERT_CSP(entryContent_P);

	/* Wait for command done */
	if (!swcore_wait_mask(SWTACR, ACTION_MASK, ACTION_DONE))
		return FAILED;

	/* Write registers according to entry width of each table */
	REG32(TCR7) = *((uint32 *)entryContent_P + 7);
	REG32(TCR6) = *((uint32 *)entryContent_P + 6);
	REG32(TCR5) = *((uint32 *)entryContent_P + 5);
	REG32(TCR4) = *((uint32 *)entryContent_P + 4);
	REG32(TCR3) = *((uint32 *)entryContent_P + 3);
	REG32(TCR2) = *((uint32 *)entryContent_P + 2);
	REG32(TCR1) = *((uint32 *)entryContent_P + 1);
	REG32(TCR0) = *(uint32 *)entryContent_P;

	/* Fill address */
	REG32(SWTAA) =
	    table_access_addr_base(tableType) + eidx * TABLE_ENTRY_DISTANCE;
	return SUCCESS;
}

/* ===== VLAN table access (vlanTable.c) ===== */

// wei add
int lx4180_ReadStatus()
{
	volatile unsigned int reg;
	reg = read_32bit_cp0_register(CP0_STATUS);
	__asm__ volatile("nop"); // david
	__asm__ volatile("nop");
	return reg;
}
void lx4180_WriteStatus(int s)
{
	volatile unsigned int reg = s;
	write_32bit_cp0_register(CP0_STATUS, reg);
	__asm__ volatile("nop"); // david
	__asm__ volatile("nop");
	return;
}

/**
 * swCore_netifCreate - Create a network interface in the switch ASIC
 * @idx: interface index
 * @param: network interface parameters (VID, MAC, ACL, MTU)
 *
 * Reads the current table entry, verifies it's not already valid,
 * then populates and writes the netif table entry.
 *
 * Return: 0 on success, EEXIST if already valid
 */
int32 swCore_netifCreate(uint32 idx, rtl_netif_param_t *param)
{
	netif_table_t entryContent;
	uint32 temp, temp2;

	ASSERT_CSP(param);

	// disable interrupt
	// I don't know the reason but if you want to use "-O" flag, must
	// disalbe interrupt before swTable_readEntry();
	temp = lx4180_ReadStatus();
	if ((temp & 0x1) != 0) {
		temp2 = temp & 0xfffffffe;
		lx4180_WriteStatus(temp2);
	}

	if (swTable_readEntry(TYPE_NETINTERFACE_TABLE, idx, &entryContent) !=
	    SUCCESS) {
		if ((temp & 0x1) != 0)
			lx4180_WriteStatus(temp);
		return FAILED;
	}

	// restore status register
	if ((temp & 0x1) != 0) {
		lx4180_WriteStatus(temp);
	}

	if (entryContent.valid) {
		return EEXIST;
	}

	memset((void *)&entryContent, 0, sizeof(entryContent));
	entryContent.valid = param->valid;
	entryContent.vid = param->vid;

	entryContent.mac47_19 =
	    ((param->gMac.mac47_32 << 13) | (param->gMac.mac31_16 >> 3)) &
	    0xFFFFFFF;
	entryContent.mac18_0 =
	    ((param->gMac.mac31_16 << 16) | param->gMac.mac15_0) & 0x7FFFF;

	entryContent.inACLStartH = (param->inAclStart >> 2) & 0x1f;
	entryContent.inACLStartL = param->inAclStart & 0x3;
	entryContent.inACLEnd = param->inAclEnd;
	entryContent.outACLStart = param->outAclStart;
	entryContent.outACLEnd = param->outAclEnd;
	entryContent.enHWRoute = param->enableRoute;

	entryContent.macMask = 8 - (param->macAddrNumber & 0x7);

	entryContent.mtuH = param->mtu >> 3;
	entryContent.mtuL = param->mtu & 0x7;

	/* Write into hardware */
	if (swTable_addEntry(TYPE_NETINTERFACE_TABLE, idx, &entryContent) == 0)
		return 0;
	fatal("switch netif table write failed");
}

/**
 * vlanTable_create - Create a VLAN entry in the switch ASIC
 * @vid: VLAN ID
 * @param: VLAN parameters (member ports, egress untag, FID)
 *
 * Return: 0 on success
 */
int32 vlanTable_create(uint32 vid, rtl_vlan_param_t *param)
{
	vlan_table_t entryContent;
	uint32 temp, temp2;

	ASSERT_CSP(param);

	// disable interrupt
	// I don't know the reason but if you want to use "-O" flag, must
	// disalbe interrupt before swTable_readEntry();
	temp = lx4180_ReadStatus();
	if ((temp & 0x1) != 0) {
		temp2 = temp & 0xfffffffe;
		lx4180_WriteStatus(temp2);
	}

	if (swTable_readEntry(TYPE_VLAN_TABLE, vid, &entryContent) != SUCCESS) {
		if ((temp & 0x1) != 0)
			lx4180_WriteStatus(temp);
		return FAILED;
	}

	// restore status register
	if ((temp & 0x1) != 0) {
		lx4180_WriteStatus(temp);
	}

	memset((void *)&entryContent, 0, sizeof(entryContent));
	entryContent.memberPort = param->memberPort & ALL_PORT_MASK;
	entryContent.egressUntag = param->egressUntag;
	entryContent.fid = param->fid;

	entryContent.vid = vid;

	/* Write into hardware */
	if (swTable_addEntry(TYPE_VLAN_TABLE, vid, &entryContent) == 0)
		return 0;
	fatal("switch VLAN table write failed");
}
