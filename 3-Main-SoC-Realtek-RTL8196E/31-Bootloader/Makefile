# Top-level Makefile -- RTL8196E bootloader
#
# One build recipe: build_bootloader.sh.  It locates the Lexra toolchain and
# the Realtek tools, validates BOARD, builds the production image into
# boot/build + btcode/build (copied to boot-img/$(BOARD)/boot.bin) and the
# RAM-test variant into boot/build-ramtest + btcode/build-ramtest
# (btcode/build-ramtest/test.bin).  This Makefile only forwards to it, so
# `make` and `./build_bootloader.sh` cannot drift apart.
#
# Targets:
#   all   - build both variants (default)
#   clean - remove all build outputs (boot-img/ is left alone: committed)
#
# Variables (forwarded through the environment):
#   JUMP_ADDR  - kernel entry point passed to stage-2 (default 0x80500000)
#   BOARD      - board directory under boards/ (default lidl)

.PHONY: all clean

all:
	BOARD="$(BOARD)" JUMP_ADDR="$(JUMP_ADDR)" ./build_bootloader.sh

clean:
	./build_bootloader.sh clean
