
#ifndef __REALTEK_MEMMAP__
#define __REALTEK_MEMMAP__

extern __iomem void *_sys_membase;

// System registers
#define REALTEK_SR_REG_ID			0x00
#define REALTEK_SR_REG_BOOTSTRAP	0x08
#define REALTEK_SR_CLKMANAGE		0x10
#define REALTEK_SR_PCIE_PHY0		0x50
#define REALTEK_SR_PCIE_PHY1		0x54
#define REALTEK_SR_BS_40MHZ			BIT(24) // Crystal clock at 40Mhz

#define sr_w32(val, reg) __raw_writel(val, _sys_membase + reg)
#define sr_r32(reg)      __raw_readl(_sys_membase + reg)

#endif