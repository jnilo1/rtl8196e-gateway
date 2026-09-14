#include "boot_asm.h"

	.text
	.set noreorder
	.globl __start

/*---------------------------------------------------------------------------*/
/* Stage-1.5 entry point                                                     */
/* This code runs from RAM after being copied by the stage-1 bootloader.     */
/* Nothing but code lives here: the stage-2 payload is the .initrd section  */
/* appended by the Makefile, and the DDR controller was configured by       */
/* stage-1 (start.S) before this code was even copied to RAM.               */
/*---------------------------------------------------------------------------*/
__start:
	/* Setup stack pointer */
	la	t0, __bss_end
	nop
	addi	t0, t0, 4096			/* Reserve 4KB stack */
	nop
	move	sp, t0				/* Set stack pointer */
	nop

test:
	jal	boot_entry			/* Jump to C entry point */
	nop					/* delay slot (was memcpy's first word) */

	/*
	 * boot_entry() does not return: on a bad stream it reports and
	 * stops by itself.  Should it ever return, stop here rather than
	 * fall into whatever the linker placed next (memcpy today).
	 */
piggy_halt:
	b	piggy_halt
	nop
