/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot_asm.h - CPU-level definitions shared by the entry code and C
 *
 * What the loader relies on from the RLX4181 (MIPS I, o32, big-endian):
 * the CP0 register numbers and Status bits, the CP0 accessors for C, and
 * for the assembler the o32 register names, the function entry macros and
 * the exception frame that inthandler.S saves.  Included by head.S,
 * inthandler.S and boot_soc.h; the stage-1 sources (btcode/start.S,
 * btcode/piggy.S) take it through -I../boot/include.
 *
 * Derived from the Linux 2.4 asm-mips headers asm.h, regdef.h, mipsregs.h,
 * stackframe.h, offset.h and the Realtek lexraregs.h, reduced to what this
 * loader expands.
 *
 * Copyright (C) 1994-2000 Ralf Baechle, Paul M. Antoine
 * Copyright (C) 2000 Silicon Graphics, Inc.
 * Copyright (c) 2009 Realtek Semiconductor Corp.
 * Copyright (c) 2024-2026 J. Nilo
 */
#ifndef _BOOT_ASM_H_
#define _BOOT_ASM_H_

/* --- Coprocessor 0 -------------------------------------------------------- */

#define CP0_BADVADDR $8
#define CP0_STATUS $12
#define CP0_CAUSE $13
#define CP0_EPC $14

/* Status register bits */
#define ST0_SX 0x00000040
#define ST0_KX 0x00000080
#define ST0_IM 0x0000ff00
#define ST0_BEV 0x00400000
#define ST0_CU0 0x10000000
#define ST0_CU1 0x20000000
#define ST0_CU2 0x40000000
#define ST0_CU3 0x80000000

/* Hardware interrupt lines in Status.IM */
#define IE_IRQ0 (1 << 10)
#define IE_IRQ1 (1 << 11)
#define IE_IRQ2 (1 << 12)
#define IE_IRQ3 (1 << 13)
#define IE_IRQ4 (1 << 14)
#define IE_IRQ5 (1 << 15)

/*
 * Lexra CP0 $20 is CCTL (cache control), not XContext.  Each operation is
 * triggered by a 0 -> 1 transition of its bit; 0 leaves the caches active.
 */
#define CCTL_ICACHE_INVAL 0x00000002
#define CCTL_IMEM_FILL 0x00000010
#define CCTL_IMEM_OFF 0x00000020
#define CCTL_DCACHE_WBINVAL 0x00000200

/* Lexra CP3: instruction RAM window */
#define CP3_IWBASE $0
#define CP3_IWTOP $1

/* --- Stage-2 stack -------------------------------------------------------- */

/* init_task_union in main.c; head.S places sp near its top. */
#define SYS_STACK_SIZE (4096 * 2)

#ifdef __ASSEMBLER__

/* --- o32 register names --------------------------------------------------- */

#define zero $0 /* wired zero */
#define v0 $2	/* return value */
#define v1 $3
#define a0 $4 /* argument registers */
#define a1 $5
#define a2 $6
#define a3 $7
#define t0 $8 /* caller saved */
#define t1 $9
#define t2 $10
#define t3 $11
#define t4 $12
#define t5 $13
#define t6 $14
#define t7 $15
#define t8 $24 /* caller saved */
#define t9 $25
#define k0 $26 /* kernel scratch */
#define k1 $27
#define sp $29 /* stack pointer */
#define ra $31 /* return address */

/* Size of a register */
#define SZREG 4

/* --- Function entry and exit ---------------------------------------------- */

/* LEAF - declare leaf routine */
#define LEAF(symbol)                                                           \
	.globl symbol;                                                         \
	.align 2;                                                              \
	.type symbol, @function;                                               \
	.ent symbol, 0;                                                        \
	symbol:                                                                \
	.frame sp, 0, ra

/* NESTED - declare nested routine entry point */
#define NESTED(symbol, framesize, rpc)                                         \
	.globl symbol;                                                         \
	.align 2;                                                              \
	.type symbol, @function;                                               \
	.ent symbol, 0;                                                        \
	symbol:                                                                \
	.frame sp, framesize, rpc

/* END - mark end of function */
#define END(function)                                                          \
	.end function;                                                         \
	.size function, .- function

/* EXPORT - export definition of symbol */
#define EXPORT(symbol)                                                         \
	.globl symbol;                                                         \
	symbol:

/* head.S section marks: the entry code opens .text.init, then .text. */
#define __INIT .section ".text.init", "ax"
#define __FINIT .previous

/* --- Exception frame (inthandler.S) --------------------------------------- */

/*
 * Layout of the frame IRQ_finder builds on the stack: 24 bytes of argument
 * save space, then the general registers (k0/k1 are not saved), LO, HI and
 * the CP0 registers.
 */
#define PT_R0 24
#define PT_R1 28
#define PT_R2 32
#define PT_R3 36
#define PT_R4 40
#define PT_R5 44
#define PT_R6 48
#define PT_R7 52
#define PT_R8 56
#define PT_R9 60
#define PT_R10 64
#define PT_R11 68
#define PT_R12 72
#define PT_R13 76
#define PT_R14 80
#define PT_R15 84
#define PT_R16 88
#define PT_R17 92
#define PT_R18 96
#define PT_R19 100
#define PT_R20 104
#define PT_R21 108
#define PT_R22 112
#define PT_R23 116
#define PT_R24 120
#define PT_R25 124
#define PT_R26 128
#define PT_R27 132
#define PT_R28 136
#define PT_R29 140
#define PT_R30 144
#define PT_R31 148
#define PT_LO 152
#define PT_HI 156
#define PT_EPC 160
#define PT_BVADDR 164
#define PT_STATUS 168
#define PT_CAUSE 172
#define PT_SIZE 176

#define SAVE_AT                                                                \
	.set push;                                                             \
	.set noat;                                                             \
	sw $1, PT_R1(sp);                                                      \
	.set pop

#define SAVE_TEMP                                                              \
	mfhi v1;                                                               \
	sw $8, PT_R8(sp);                                                      \
	sw $9, PT_R9(sp);                                                      \
	sw v1, PT_HI(sp);                                                      \
	mflo v1;                                                               \
	sw $10, PT_R10(sp);                                                    \
	sw $11, PT_R11(sp);                                                    \
	sw v1, PT_LO(sp);                                                      \
	sw $12, PT_R12(sp);                                                    \
	sw $13, PT_R13(sp);                                                    \
	sw $14, PT_R14(sp);                                                    \
	sw $15, PT_R15(sp);                                                    \
	sw $24, PT_R24(sp)

#define SAVE_STATIC                                                            \
	sw $16, PT_R16(sp);                                                    \
	sw $17, PT_R17(sp);                                                    \
	sw $18, PT_R18(sp);                                                    \
	sw $19, PT_R19(sp);                                                    \
	sw $20, PT_R20(sp);                                                    \
	sw $21, PT_R21(sp);                                                    \
	sw $22, PT_R22(sp);                                                    \
	sw $23, PT_R23(sp);                                                    \
	sw $30, PT_R30(sp)

/*
 * Open the frame.  Status.CU0 set means the interrupted code ran on the
 * loader stack (head.S sets CU0 at start): keep sp.  Otherwise take the
 * stack from kernelsp.  In practice the loader never leaves kernel mode.
 */
#define SAVE_SOME                                                              \
	.set push;                                                             \
	.set reorder;                                                          \
	mfc0 k0, CP0_STATUS;                                                   \
	sll k0, 3; /* extract cu0 bit */                                       \
	.set noreorder;                                                        \
	bltz k0, 8f;                                                           \
	move k1, sp;                                                           \
	.set reorder;                                                          \
	/* Called from user mode, new stack. */                                \
	lui k1, %hi(kernelsp);                                                 \
	lw k1, %lo(kernelsp)(k1);                                              \
	8 : move k0, sp;                                                       \
	subu sp, k1, PT_SIZE;                                                  \
	sw k0, PT_R29(sp);                                                     \
	sw $3, PT_R3(sp);                                                      \
	sw $0, PT_R0(sp);                                                      \
	mfc0 v1, CP0_STATUS;                                                   \
	sw $2, PT_R2(sp);                                                      \
	sw v1, PT_STATUS(sp);                                                  \
	sw $4, PT_R4(sp);                                                      \
	mfc0 v1, CP0_CAUSE;                                                    \
	sw $5, PT_R5(sp);                                                      \
	sw v1, PT_CAUSE(sp);                                                   \
	sw $6, PT_R6(sp);                                                      \
	mfc0 v1, CP0_EPC;                                                      \
	sw $7, PT_R7(sp);                                                      \
	sw v1, PT_EPC(sp);                                                     \
	sw $25, PT_R25(sp);                                                    \
	sw $28, PT_R28(sp);                                                    \
	sw $31, PT_R31(sp);                                                    \
	ori $28, sp, 0x1fff;                                                   \
	xori $28, 0x1fff;                                                      \
	.set pop

#define SAVE_ALL                                                               \
	SAVE_SOME;                                                             \
	SAVE_AT;                                                               \
	SAVE_TEMP;                                                             \
	SAVE_STATIC

#define RESTORE_AT                                                             \
	.set push;                                                             \
	.set noat;                                                             \
	lw $1, PT_R1(sp);                                                      \
	.set pop;

#define RESTORE_TEMP                                                           \
	lw $24, PT_LO(sp);                                                     \
	lw $8, PT_R8(sp);                                                      \
	lw $9, PT_R9(sp);                                                      \
	mtlo $24;                                                              \
	lw $24, PT_HI(sp);                                                     \
	lw $10, PT_R10(sp);                                                    \
	lw $11, PT_R11(sp);                                                    \
	mthi $24;                                                              \
	lw $12, PT_R12(sp);                                                    \
	lw $13, PT_R13(sp);                                                    \
	lw $14, PT_R14(sp);                                                    \
	lw $15, PT_R15(sp);                                                    \
	lw $24, PT_R24(sp)

#define RESTORE_STATIC                                                         \
	lw $16, PT_R16(sp);                                                    \
	lw $17, PT_R17(sp);                                                    \
	lw $18, PT_R18(sp);                                                    \
	lw $19, PT_R19(sp);                                                    \
	lw $20, PT_R20(sp);                                                    \
	lw $21, PT_R21(sp);                                                    \
	lw $22, PT_R22(sp);                                                    \
	lw $23, PT_R23(sp);                                                    \
	lw $30, PT_R30(sp)

/* Restore Status.IM from the frame with interrupts kept disabled. */
#define RESTORE_SOME                                                           \
	.set push;                                                             \
	.set reorder;                                                          \
	mfc0 t0, CP0_STATUS;                                                   \
	.set pop;                                                              \
	ori t0, 0x1f;                                                          \
	xori t0, 0x1f;                                                         \
	mtc0 t0, CP0_STATUS;                                                   \
	li v1, 0xff00;                                                         \
	and t0, v1;                                                            \
	lw v0, PT_STATUS(sp);                                                  \
	nor v1, $0, v1;                                                        \
	and v0, v1;                                                            \
	or v0, t0;                                                             \
	mtc0 v0, CP0_STATUS;                                                   \
	lw $31, PT_R31(sp);                                                    \
	lw $28, PT_R28(sp);                                                    \
	lw $25, PT_R25(sp);                                                    \
	lw $7, PT_R7(sp);                                                      \
	lw $6, PT_R6(sp);                                                      \
	lw $5, PT_R5(sp);                                                      \
	lw $4, PT_R4(sp);                                                      \
	lw $3, PT_R3(sp);                                                      \
	lw $2, PT_R2(sp)

#define RESTORE_SP_AND_RET                                                     \
	.set push;                                                             \
	.set noreorder;                                                        \
	lw k0, PT_EPC(sp);                                                     \
	lw sp, PT_R29(sp);                                                     \
	jr k0;                                                                 \
	rfe;                                                                   \
	.set pop

#define RESTORE_ALL_AND_RET                                                    \
	RESTORE_SOME;                                                          \
	RESTORE_AT;                                                            \
	RESTORE_TEMP;                                                          \
	RESTORE_STATIC;                                                        \
	RESTORE_SP_AND_RET

/*
 * Move to kernel mode and disable interrupts.
 * Set cp0 enable bit as sign that we're running on the kernel stack
 */
#define CLI                                                                    \
	mfc0 t0, CP0_STATUS;                                                   \
	li t1, ST0_CU0 | 0x1f;                                                 \
	or t0, t1;                                                             \
	xori t0, 0x1f;                                                         \
	mtc0 t0, CP0_STATUS

#else /* C */

/* --- CP0 accessors -------------------------------------------------------- */

#define __STR(x) #x
#define STR(x) __STR(x)

#define read_32bit_cp0_register(source)                                        \
	({                                                                     \
		int __res;                                                     \
		__asm__ __volatile__(".set\tpush\n\t"                          \
				     ".set\treorder\n\t"                       \
				     "mfc0\t%0," STR(source) "\n\t"            \
							     ".set\tpop"       \
				     : "=r"(__res));                           \
		__res;                                                         \
	})

#define write_32bit_cp0_register(register, value)                              \
	__asm__ __volatile__("mtc0\t%0," STR(register) "\n\t"                  \
						       "nop"                   \
			     :                                                 \
			     : "r"(value))

#endif /* __ASSEMBLER__ */

#endif /* _BOOT_ASM_H_ */
