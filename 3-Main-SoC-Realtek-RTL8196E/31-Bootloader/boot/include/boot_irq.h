/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot_irq.h - Interrupt plumbing: CPU interrupt enable and the IRQ table
 *
 * cli/sti and the save-and-restore pair on Status.IE, and the irqaction a
 * driver registers with request_IRQ (irq.c).  The enable/disable inlines
 * are those of the Linux 2.4 asm-mips system.h.
 *
 * Copyright (C) 1994 - 1999 by Ralf Baechle
 * Copyright (c) 2024-2026 J. Nilo
 */
#ifndef _BOOT_IRQ_H_
#define _BOOT_IRQ_H_

/* One handler slot per GIMR line. */
#define NR_IRQS 32

/*
 * The exception frame IRQ_finder saves (PT_* in boot_asm.h).  Handlers get
 * a pointer to it and never read through it.
 */
struct pt_regs;

struct irqaction {
	void (*handler)(int, void *, struct pt_regs *);
	unsigned long flags;
	unsigned long mask;
	const char *name;
	void *dev_id;
	struct irqaction *next;
};

int request_IRQ(unsigned long irq, struct irqaction *action, void *dev_id);

/* --- CPU interrupt enable (Status.IE) ------------------------------------ */

static __inline__ void sti(void)
{
	__asm__ __volatile__(".set\tpush\n\t"
			     ".set\treorder\n\t"
			     ".set\tnoat\n\t"
			     "mfc0\t$1,$12\n\t"
			     "ori\t$1,0x1f\n\t"
			     "xori\t$1,0x1e\n\t"
			     "mtc0\t$1,$12\n\t"
			     ".set\tpop\n\t"
			     : /* no outputs */
			     : /* no inputs */
			     : "$1", "memory");
}

/*
 * For cli() we have to insert nops to make shure that the new value
 * has actually arrived in the status register before the end of this
 * macro.
 * R4000/R4400 need three nops, the R4600 two nops and the R10000 needs
 * no nops at all.
 */
static __inline__ void cli(void)
{
	__asm__ __volatile__(".set\tpush\n\t"
			     ".set\treorder\n\t"
			     ".set\tnoat\n\t"
			     "mfc0\t$1,$12\n\t"
			     "ori\t$1,1\n\t"
			     "xori\t$1,1\n\t"
			     ".set\tnoreorder\n\t"
			     "mtc0\t$1,$12\n\t"
			     "nop\n\t"
			     "nop\n\t"
			     "nop\n\t"
			     ".set\tpop\n\t"
			     : /* no outputs */
			     : /* no inputs */
			     : "$1", "memory");
}

#define save_and_cli(x)                                                        \
	__asm__ __volatile__(".set\tpush\n\t"                                  \
			     ".set\treorder\n\t"                               \
			     ".set\tnoat\n\t"                                  \
			     "mfc0\t%0,$12\n\t"                                \
			     "ori\t$1,%0,1\n\t"                                \
			     "xori\t$1,1\n\t"                                  \
			     ".set\tnoreorder\n\t"                             \
			     "mtc0\t$1,$12\n\t"                                \
			     "nop\n\t"                                         \
			     "nop\n\t"                                         \
			     "nop\n\t"                                         \
			     ".set\tpop\n\t"                                   \
			     : "=r"(x)                                         \
			     : /* no inputs */                                 \
			     : "$1", "memory")

#define restore_flags(flags)                                                   \
	do {                                                                   \
		unsigned long __tmp1;                                          \
                                                                               \
		__asm__ __volatile__(                                          \
		    ".set\tnoreorder\t\t\t# restore_flags\n\t"                 \
		    ".set\tnoat\n\t"                                           \
		    "mfc0\t$1, $12\n\t"                                        \
		    "andi\t%0, 1\n\t"                                          \
		    "ori\t$1, 1\n\t"                                           \
		    "xori\t$1, 1\n\t"                                          \
		    "or\t%0, $1\n\t"                                           \
		    "mtc0\t%0, $12\n\t"                                        \
		    "nop\n\t"                                                  \
		    "nop\n\t"                                                  \
		    "nop\n\t"                                                  \
		    ".set\tat\n\t"                                             \
		    ".set\treorder"                                            \
		    : "=r"(__tmp1)                                             \
		    : "0"(flags)                                               \
		    : "$1", "memory");                                         \
	} while (0)

#endif /* _BOOT_IRQ_H_ */
