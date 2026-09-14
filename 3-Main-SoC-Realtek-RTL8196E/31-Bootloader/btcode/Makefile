# Stage-1 bootloader Makefile (btcode)
#
# Builds the first code that runs from flash on the RTL8196E.  start.S
# runs in place from flash: SoC patch, UART0 banner, DDR controller
# bring-up and DQS calibration, cache flush, then it copies the embedded
# decompressor (piggy.S + bootload.c + LzmaDecode.c) to RAM and jumps to
# it; piggy.S sets the stack and the decompressor unpacks the stage-2
# bootloader into RAM and transfers control to it.  No BSS is cleared
# anywhere (the decompressor has none).
#
# Build pipeline:
#
#   boot.out  (ELF from boot/)
#     |  objcopy to raw binary + LZMA compress
#     v
#   boot.img.gz  +  piggy.S  +  bootload.o  +  LzmaDecode.o
#     |  link with piggy.script
#     v
#   piggy.elf  ->  piggy.bin  (raw stage-2 payload)
#     |  embed in start.o via .initrd section
#     v
#   start.o
#     |  link with ld.script
#     v
#   boot.elf  ->  boot  (raw binary)
#     |  cvimg (add Realtek flash header)
#     v
#   boot.bin        (final flash image)
#
# When RAMTEST_TRACE=1, piggy.bin is also copied as test.bin.  The two
# variants build into separate directories (build/ for the flash image,
# build-ramtest/ for the RAM-test image), so btcode/build/boot.bin is
# always the production image.  The top-level Makefile builds both.
#
# Targets:
#   bootload - build boot.bin (and test.bin under RAMTEST_TRACE) (default)
#   clean    - remove build outputs
#
# Variables:
#   RAMTEST_TRACE - set to 1 for the RAM-test image variant
#   OUTDIR        - object directory (build, or build-ramtest)
#   BOOT_OUT      - stage-2 ELF to embed (default ../boot/$(OUTDIR)/boot.out)
#   CVIMG         - path to the Realtek cvimg header tool
#   CVIMG_FLAGS   - flags for cvimg (image type, addresses)
#   LZMA          - path to the LZMA compression tool

CROSS   ?= mips-lexra-linux-musl-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
NM       = $(CROSS)nm

CVIMG       ?= $(HOME)/rtl8196e-gateway/1-Build-Environment/11-realtek-tools/bin/cvimg
CVIMG_FLAGS ?= -t boot -c any -e 0x0 -b 0x0
LZMA        ?= $(HOME)/rtl8196e-gateway/1-Build-Environment/11-realtek-tools/bin/lzma

OUTDIR ?= build

# --- Board selection ---------------------------------------------------------
# Per-board constants (DRAM size/top, DDR bring-up) live in
# ../boards/$(BOARD)/board.h — see ../boards/README.md.

BOARD ?= lidl

# --- Compiler flags ---------------------------------------------------------

CFLAGS = -Os -g -G 0 \
         -fomit-frame-pointer -fno-pic -mno-abicalls \
         -ffreestanding -fno-common -MD \
         -I../boards/$(BOARD)

ifeq ($(RAMTEST_TRACE),1)
CFLAGS += -DRAMTEST_TRACE
endif

BOOT_OUT ?= ../boot/$(OUTDIR)/boot.out

# --- Build rules ------------------------------------------------------------

.PHONY: bootload clean

bootload: $(OUTDIR)/boot.bin

# Stage-2 payload: extract raw binary, LZMA compress, wrap in piggy.elf
# Depends on BOOT_OUT to force recompile when stage-2 changes,
# so the subsequent --add-section in piggy.elf starts with a fresh .o.
$(OUTDIR)/piggy.o: piggy.S $(BOOT_OUT) | $(OUTDIR)
	$(CC) $(CFLAGS) -I../boot/include -O2 -c $< -o $@

$(OUTDIR)/boot.img.gz: $(BOOT_OUT) | $(OUTDIR)
	$(OBJCOPY) -g -Obinary -R .MIPS.abiflags $< $(OUTDIR)/boot.img
	$(LZMA) e $(OUTDIR)/boot.img $@
	rm -f $(OUTDIR)/boot.img

$(OUTDIR)/bootload.o: bootload.c | $(OUTDIR)
	$(CC) $(CFLAGS) -nostdinc -c $< -o $@

$(OUTDIR)/LzmaDecode.o: LzmaDecode.c | $(OUTDIR)
	$(CC) $(CFLAGS) -I../boot/include -fno-inline -O2 -c $< -o $@

$(OUTDIR)/piggy.elf: $(OUTDIR)/piggy.o $(OUTDIR)/bootload.o $(OUTDIR)/LzmaDecode.o $(OUTDIR)/boot.img.gz
	$(OBJCOPY) --add-section .initrd=$(OUTDIR)/boot.img.gz $(OUTDIR)/piggy.o
	$(LD) -X -nostdlib -Tpiggy.script --static $(OUTDIR)/piggy.o $(OUTDIR)/bootload.o $(OUTDIR)/LzmaDecode.o -o $@

$(OUTDIR)/piggy.bin: $(OUTDIR)/piggy.elf
	$(OBJCOPY) -Obinary -R .MIPS.abiflags $< $@
	$(OBJDUMP) -h -S $< > $(OUTDIR)/piggy.elf.txt

# Stage-1 wrapper: start.S + piggy.bin -> final boot image
$(OUTDIR)/start.o: start.S $(OUTDIR)/piggy.bin | $(OUTDIR)
	$(CC) $(CFLAGS) -I../boot/include -O2 -c start.S -o $@
	$(OBJCOPY) --add-section .initrd=$(OUTDIR)/piggy.bin $@

$(OUTDIR)/boot.elf: $(OUTDIR)/start.o
	$(LD) -X -nostdlib -Tld.script $< -o $@

$(OUTDIR)/boot: $(OUTDIR)/boot.elf
	$(OBJCOPY) -Obinary -R .MIPS.abiflags $< $@
	$(OBJDUMP) -h -S $(OUTDIR)/boot.elf > $(OUTDIR)/boot.elf.txt
	$(NM) $(OUTDIR)/boot.elf | sort > $(OUTDIR)/system.map

$(OUTDIR)/boot.bin: $(OUTDIR)/boot
	$(CVIMG) -i $< -o $@ $(CVIMG_FLAGS)
ifeq ($(RAMTEST_TRACE),1)
	cp -f $(OUTDIR)/piggy.bin $(OUTDIR)/test.bin
endif

$(OUTDIR):
	@mkdir -p $@

clean:
	rm -rf build build-ramtest
