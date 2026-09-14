/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot_common.h - The C environment of the stage-2 loader
 *
 * The build is freestanding: the only headers taken from outside the tree
 * are the compiler's own <stdint.h> and <stddef.h> (gcc's, not the
 * sysroot's, which -ffreestanding guarantees and build_bootloader.sh
 * checks on the include trace), plus <stdarg.h> where libc.c needs it.
 */
#ifndef _BOOT_COMMON_H_
#define _BOOT_COMMON_H_

#include <stdint.h>
#include <stddef.h>

/* Marks functions called from the assembler entry code. */
#define asmlinkage

#include "stdlib.h"

#endif /* _BOOT_COMMON_H_ */
