# Stage-2 bootloader Makefile (RTL8196E)
#
# Builds the interactive bootloader that stage-1 (btcode/) decompresses
# into RAM: SPI flash driver, Ethernet/TFTP recovery, monitor console,
# kernel loader.  DRAM is already up when this code runs (stage-1 does the
# DDR bring-up).  All sources live in boot/ (flat layout); objects go to
# $(OUTDIR).  The stage-1 Makefile (btcode/) references boot.out directly
# to produce the compressed flash image.
#
# Targets:
#   boot  - build $(OUTDIR)/boot.out (ELF)
#   clean - remove build outputs
#
# Variables (passed from top-level Makefile):
#   JUMP_ADDR               - kernel entry address (e.g. 0x80500000)
#   RAMTEST_TRACE           - if non-empty, build the RAM-test variant
#   OUTDIR                  - object directory (build, or build-ramtest)
#   BOOT_CODE_TIME_OVERRIDE - override the pinned build timestamp (ver.h)

CROSS   ?= mips-lexra-linux-musl-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
NM       = $(CROSS)nm
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump

OUTDIR  ?= build

# --- Board selection ---------------------------------------------------------
# Per-board constants (DRAM size/top, DDR bring-up) live in
# ../boards/$(BOARD)/board.h — see ../boards/README.md.

BOARD ?= lidl

# --- Compiler / linker flags ------------------------------------------------
#
# The code type-puns freely (unsigned short * over headers, unsigned long *
# over byte buffers): -fno-strict-aliasing makes that defined.
# -ffreestanding: this is not a hosted environment; memcpy & co are ours,
# and the only outside headers are the compiler's own <stdint.h>,
# <stddef.h> and <stdarg.h> — never the sysroot's (build_bootloader.sh
# checks the -MD dependency files for that).
# Warnings: everything -Wall -Wextra reports is either fixed or promoted
# to an error below; the three -Wno- cover what the switch driver and the
# handler signatures do by design (unsigned char * vs char * buffers, int
# indexes against unsigned sizes, unused handler arguments).

WARNFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
            -Wno-pointer-sign \
            -Werror=return-type -Werror=parentheses \
            -Werror=implicit-function-declaration -Werror=uninitialized \
            -Werror=maybe-uninitialized -Werror=implicit-fallthrough \
            -Werror=unused-variable -Werror=unused-function \
            -Werror=array-parameter -Werror=unknown-pragmas

CFLAGS   = -g -O -G 0 \
           -fomit-frame-pointer -fno-pic -mno-abicalls \
           -fno-strict-aliasing -ffreestanding -MD \
           $(WARNFLAGS) \
           -I. -I./include -I../boards/$(BOARD)

ASFLAGS  = -x assembler-with-cpp -G 0

LDFLAGS  = -nostdlib -T./ld.script -EB --static

ifneq ($(BOOT_CODE_TIME_OVERRIDE),)
CFLAGS  += -DBOOT_CODE_TIME='"$(BOOT_CODE_TIME_OVERRIDE)"'
endif

ifneq ($(RAMTEST_TRACE),)
CFLAGS  += -DRAMTEST_TRACE
ASFLAGS += -DRAMTEST_TRACE
endif

ifneq ($(strip $(JUMP_ADDR)),)
CFLAGS  += -DJUMP_ADDR=$(JUMP_ADDR)
endif

# --- Source lists -----------------------------------------------------------

ASM_SRCS = head.S inthandler.S

C_SRCS = \
    arch.c main.c irq.c timer.c fatal.c calloc.c libc.c crc32.c \
    uart.c \
    flash.c net/eth.c net/tftpd.c \
    swCore.c swTable.c swNic.c \
    monitor.c

ASM_OBJS = $(addprefix $(OUTDIR)/,$(ASM_SRCS:.S=.o))
C_OBJS   = $(addprefix $(OUTDIR)/,$(C_SRCS:.c=.o))
OBJS     = $(ASM_OBJS) $(C_OBJS)

# --- Build rules ------------------------------------------------------------

.PHONY: all boot clean

all:
	@echo "Usage: make boot JUMP_ADDR=0x80500000"

boot: $(OUTDIR)/boot.out
	$(NM) $(OUTDIR)/boot.out | sort > $(OUTDIR)/boot.nm
	$(OBJDUMP) -h -S $(OUTDIR)/boot.out > $(OUTDIR)/boot.text

$(OUTDIR)/boot.out: $(OBJS) | $(OUTDIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(OUTDIR)/%.o: %.S | $(OUTDIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(ASFLAGS) $< -o $@

$(OUTDIR)/%.o: %.c | $(OUTDIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(OUTDIR):
	@mkdir -p $@

clean:
	rm -rf build build-ramtest
