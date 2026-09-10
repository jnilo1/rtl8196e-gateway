# Stage-2 bootloader Makefile (RTL8196E)
#
# Builds the main bootloader: hardware init, DDR calibration, SPI flash
# driver, Ethernet/TFTP recovery, monitor console, and kernel loader.
# All sources live in boot/ (flat layout); objects go to build/.
# The stage-1 Makefile (btcode/) references boot.out directly to
# produce the compressed flash image.
#
# Targets:
#   boot  - build boot.out (ELF)
#   clean - remove build outputs
#
# Variables (passed from top-level Makefile):
#   JUMP_ADDR               - kernel entry address (e.g. 0x80500000)
#   RAMTEST_TRACE           - if non-empty, build the RAM-test variant
#   BOOT_CODE_TIME_OVERRIDE - override the build timestamp in the banner

CROSS   ?= mips-lexra-linux-musl-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
NM       = $(CROSS)nm
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump

OUTDIR   = build

# --- Build timestamp -------------------------------------------------------
# Pinned build timestamp of this bootloader version (NOT a live `date` call),
# so the build is reproducible: same source => same boot.bin. This is the
# actual build moment of the current B_VERSION; update it together with
# B_VERSION (include/ver.h) whenever the bootloader is rebuilt for a release.

BOOT_CODE_TIME ?= 2026.09.10-18:57+0200
ifneq ($(BOOT_CODE_TIME_OVERRIDE),)
BOOT_CODE_TIME := $(BOOT_CODE_TIME_OVERRIDE)
endif

# --- Board selection ---------------------------------------------------------
# Per-board constants (DRAM size/top, DDR bring-up) live in
# ../boards/$(BOARD)/board.h — see ../boards/README.md.

BOARD ?= lidl

# --- Compiler / linker flags ------------------------------------------------

CFLAGS   = -g -O -G 0 \
           -fomit-frame-pointer -fno-pic -mno-abicalls \
           -I. -I./include -I../boards/$(BOARD) \
           -D__KERNEL__ -Dlinux \
           -DBOOT_CODE_TIME='"$(BOOT_CODE_TIME)"'

ASFLAGS  = -D__ASSEMBLY__ -x assembler-with-cpp -G 0

LDFLAGS  = -nostdlib -T./ld.script -EB --static

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
    arch.c main.c irq.c calloc.c libc.c \
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
	rm -rf $(OUTDIR)
