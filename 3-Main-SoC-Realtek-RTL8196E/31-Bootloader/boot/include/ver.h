/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ver.h - Bootloader version and pinned build timestamp
 *
 * Both strings go into the stage-2 banner and are bumped together when the
 * bootloader is rebuilt for a release.  BOOT_CODE_TIME is a constant, not a
 * live `date` call, so the build is reproducible: same source, same boot.bin.
 * The Makefile may override it (BOOT_CODE_TIME_OVERRIDE) for experiments.
 */
#ifndef _VER_H_
#define _VER_H_

#define B_VERSION "V3.2"

#ifndef BOOT_CODE_TIME
#define BOOT_CODE_TIME "2026.09.26-09:06+0200"
#endif

#endif /* _VER_H_ */
