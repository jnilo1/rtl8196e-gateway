# RTL8196E Bootloader Memo

## Overview

Bootloader for Realtek RTL8196E SoC (Lexra MIPS 4181 big-endian architecture).
Used on routers/gateways with SPI flash and SDRAM.

---

## Code Architecture

### Two-stage structure

```
├── btcode/          # Stage 1: Initial boot code (LZMA wrapper)
│   ├── start.S      # MIPS assembly entry point (DDR init + payload copy)
│   ├── piggy.S      # Compressed payload wrapper
│   ├── bootload.c   # LZMA decompression and jump to stage 2
│   └── LzmaDecode.* # LZMA decompressor
│
└── boot/            # Stage 2: Main bootloader
    ├── head.S       # MIPS entry point (CPU init, DDR calibration)
    ├── inthandler.S # Exception/interrupt vectors
    ├── arch.c       # Architecture init (CP0, cache, exceptions)
    ├── main.c       # Boot logic: image validation, kernel loading
    ├── irq.c        # Interrupt controller setup
    ├── monitor.c    # Interactive console + CPU speed calibration
    ├── uart.c       # UART driver
    ├── libc.c       # Minimal libc (printf, string, strtol...)
    ├── calloc.c     # Heap allocator
    ├── flash.c      # SPI flash driver
    ├── swCore.c     # Ethernet switch core
    ├── swTable.c    # Ethernet switch table management
    ├── swNic.c      # Ethernet NIC polling driver
    └── net/
        ├── eth.c    # Ethernet frame TX/RX
        └── tftpd.c  # TFTP server for firmware recovery
```

### Boot process

1. **Stage 1** (`btcode/`): Loaded from SPI flash at `0x80100000`
   - Initializes UART, minimal memory
   - Decompresses stage 2 (LZMA) to `0x80400000`
   - Jumps to stage 2

2. **Stage 2** (`boot/`): Full bootloader (DRAM is already up — stage 1 did it)
   - Initializes console, heap, interrupts, timer tick, SPI flash
   - Displays banner
   - Scans flash for a valid firmware image and copies it to RAM
   - While checksumming the copy, polls the console for ESC every 64 KiB
     (no timed window: hold the key, or use `boothold` from Linux)
   - Normal mode -> jumps to the kernel (switch quiesced first)
   - Download mode -> Ethernet switch up, TFTP server + console

---

## Toolchain

**Required**: Custom Lexra toolchain built with crosstool-ng

```bash
export PATH=$HOME/rtl8196e-gateway/x-tools/mips-lexra-linux-musl/bin:$PATH
```

Compiler: `mips-lexra-linux-musl-gcc` (MIPS big-endian, Lexra LX4380, musl libc)

The toolchain is built using crosstool-ng with Lexra patches. See `1-Build-Environment/` for build instructions.

---

## Boot Modes

### 1. Normal mode (LOCALSTART_MODE)

- Automatic boot after image scan
- Loads and executes kernel from flash

### 2. Download mode (DOWN_MODE)

Triggered by:
- **ESC key** (0x1B) on UART during image copy/checksum verification

Available services:
- TFTP server (port 2098)
- Interactive console `<RealTek>`

### ESC key detection: `main.c`

```c
static int pollingDownModeKeyword(int key)
{
    int ch;

    /* Drain the whole FIFO looking for the key (V3.0): one stray byte
     * ahead of ESC used to hide it for the rest of the boot. */
    while (uart_data_ready()) {
        ch = uart_getc_nowait();
        if (ch == key) {       // key = 0x1B (ESC)
            gCHKKEY_HIT = 1;
            return 1;
        }
        /* Keep the first non-matching character for the monitor */
        if (g_uart_peek < 0)
            g_uart_peek = ch;
    }
    return 0;
}

int user_interrupt(void)
{
    return pollingDownModeKeyword(ESC);
}
```

Note: The reset button / GPIO detection has been removed in this simplified version. Only ESC key over UART triggers download mode. The first character received that is not the escape key is stashed in `g_uart_peek` so it is not lost.

---

## TFTP Server

### Configuration: `net/tftpd.c`

| Parameter | Value |
|-----------|-------|
| Server IP (bootloader) | 192.168.1.6 (default; `IPCONFIG` or `boothold <ip>` override it) |
| Client IP | learned from the first request |
| TFTP port | **2098** (non-standard) |
| Standard TFTP port | 69 (used for WRQ reception) |

### Usage

```bash
# From PC (192.168.1.116)
tftp 192.168.1.6 2098
> binary
> put firmware.bin
```

**Note**: The bootloader acts as TFTP server, not client. It waits for incoming connections.

---

## Console Output at Boot

### Banner: `main.c:61-70`

```c
void showBoardInfo(void)
{
    cpu_speed = check_cpu_speed();
    prom_printf("Realtek RTL8196E  CPU: %dMHz  RAM: 32MB  Flash: %s\n",
                cpu_speed, g_flash_chip_name);
    prom_printf("Bootloader: %s - %s - J. Nilo", B_VERSION, BOOT_CODE_TIME);
}
```

### Typical output

```
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q128 (JEDEC c84018)
Bootloader: V2.3 - 2026.03.11-19:34+0100 - J. Nilo
```

In download mode:
```
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q128 (JEDEC c84018)
Bootloader: V2.3 - 2026.03.11-19:34+0100 - J. Nilo
---Escape booting by user
---Ethernet init Okay!
<RealTek>
```

### Version and build timestamp

Both live in `boot/include/ver.h` and are bumped together when the
bootloader is rebuilt for a release.  The timestamp is a pinned constant,
not a live `date` call, so the build is reproducible (same source, same
`boot.bin`; `BOOT_CODE_TIME_OVERRIDE=...` on the make line overrides it for
experiments):
```c
#define B_VERSION "V3.1"
#define BOOT_CODE_TIME "2026.09.11-18:00+0200"
```

---

## Key Memory Addresses

| Address | Usage |
|---------|-------|
| `0x80000000` | SDRAM base (kseg0) |
| `0x80100000` | Compressed btcode load address |
| `0x80300000` | LZMA state buffer |
| `0x80400000` | Bootloader decompression target |
| `0x80500000` | Kernel entry point (JUMP_ADDR) |
| `0xB8000000` | I/O registers (kseg1) |
| `0xB8002000` | UART base |
| `0xB8003500` | GPIO registers |
| `0xBFC00000` | SPI flash (boot ROM) |

---

## Building

### Using the build script

```bash
./build_bootloader.sh          # build all variants
./build_bootloader.sh clean    # clean all build outputs
```

### Using make directly

```bash
make            # build all variants
make clean      # clean all build outputs
```

### Build variants

Two variants are built:

| Variant | Output file | Description |
|---------|-------------|-------------|
| boot | `boot-img/<board>/boot.bin` | Production flash image, per-board slot (stays in download mode after boot TFTP) |
| ramtest | `btcode/build-ramtest/test.bin` | RAM-test image (RAMTEST_TRACE), own object directories |

### Build pipeline

```
boot.out  (ELF from boot/)
  |  objcopy to raw binary + LZMA compress
  v
boot.img.gz  +  piggy.S  +  bootload.o  +  LzmaDecode.o
  |  link with piggy.script
  v
piggy.elf  ->  piggy.bin  (raw stage-2 payload)
  |  embed in start.o via .initrd section
  v
start.o
  |  link with ld.script
  v
boot.elf  ->  boot  (raw binary)
  |  cvimg (add Realtek flash header)
  v
boot-img/<board>/boot.bin   (final flash image, per-board slot)
```

### Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `JUMP_ADDR` | `0x80500000` | Kernel entry point passed to stage-2 |
| `CROSS` | `mips-lexra-linux-musl-` | Toolchain prefix |

---

## Configuration Files

| File | Purpose |
|------|---------|
| `Makefile` | Top-level build: orchestrates three variants |
| `boot/Makefile` | Stage-2 build: compiler flags, source list |
| `btcode/Makefile` | Stage-1 build: LZMA compression, flash header |
| `boot/include/spi_flash.h` | Flash partition offsets, scan ranges and the flash.c API |
| `boot/include/ver.h` | Bootloader version string |

---

## Summary of Changes Made

1. **Toolchain migration**: From RSDK to crosstool-ng (mips-lexra-linux-musl)
2. **Flat source layout**: Removed `src/` wrapper; `boot/` and `btcode/` are now top-level
3. **Flat boot/ structure**: Removed subdirectories (`arch/`, `init/`, `io/`, `monitor/`, `flash/`, `rtl8196x/`); all C sources at top level with `net/` for networking
4. **Code consolidation**: `utility.c` merged into `main.c`; `power_on_led.c` removed; `string.c`/`strtol.c`/`strtoul.c`/`misc.c`/`ctool.c`/`init.c` merged into `libc.c`; `spi_common.c`/`spi_flash.c` merged into `flash.c`; `setup.c`/`traps.c`/`head.S` simplified into `arch.c`/`head.S`
5. **Reset button removed**: GPIO-based reset button detection removed; ESC key only
6. **Boot LED removed**: `power_on_led.c` and all LED-related code removed
7. **Build system simplification**: Removed menuconfig, hardcoded RTL8196E config; three-variant build (noreboot/reboot/ramtest)
8. **Header cleanup**: Replaced Linux 2.4 headers with minimal standalone headers
9. **UART driver extracted**: Dedicated `uart.c` for serial I/O

---

## Flash Partitions

| Partition | Offset | Size | Description |
|-----------|--------|------|-------------|
| mtd0 | 0x000000 | 0x020000 (128 KB) | Bootloader + config |
| mtd1 | 0x020000 | 0x1E0000 (1.9 MB) | Linux kernel |
| mtd2 | 0x200000 | 0x200000 (2 MB) | Root filesystem |
| mtd3 | 0x400000 | 0xC00000 (12 MB) | User data (JFFS2) |

---

## Image Format

The bootloader only accepts images with a valid **Realtek header**. Raw binary files are rejected.

### Header Structure (16 bytes)

```c
typedef struct {
    unsigned char signature[4];   // Image type identifier
    unsigned long startAddr;      // RAM load address (big-endian)
    unsigned long burnAddr;       // Flash destination address (big-endian)
    unsigned long len;            // Payload length (big-endian)
} IMG_HEADER_T;
```

### Complete Image Layout

```
+------------------+
|  Header (16 B)   |  signature + startAddr + burnAddr + len
+------------------+
|                  |
|  Payload         |  Raw binary data (kernel, rootfs, etc.)
|  (len bytes)     |
|                  |
+------------------+
|  Checksum (2 B)  |  16-bit one's complement
+------------------+
|  Padding         |  Alignment to block size (optional)
+------------------+
```

### Known Signatures

#### TFTP auto-flash signatures (sign_tbl in tftpd.c)

These signatures are recognized by the TFTP server for automatic flashing:

| Signature | Type | Description |
|-----------|------|-------------|
| `cs6c` | Kernel | Linux kernel for RTL8196E |
| `cr6c` | Firmware | Kernel + rootfs combined |
| `r6cr` | Rootfs/Data | SquashFS rootfs or JFFS2 userdata |
| `boot` | Bootloader | Bootloader image |
| `ALL1` | Total image | Full flash image (multi-section, checksum per section) |
| `ALL2` | Total image | Full flash image (no per-section check) |

#### Rootfs validation signatures (main.c)

These signatures are used only during boot-time rootfs checksum verification, not for TFTP auto-flash:

| Signature | Type | Description |
|-----------|------|-------------|
| `sqsh` | Rootfs | SquashFS filesystem (big-endian magic) |
| `hsqs` | Rootfs | SquashFS filesystem (little-endian magic) |

### Checksum Algorithm

16-bit one's complement checksum:

1. Pad payload to 2-byte alignment
2. Sum all 16-bit words (big-endian) of the payload
3. Append checksum = `~sum + 1` (two's complement of sum)
4. Verification: sum of all words including checksum must equal **0**

```c
uint16_t calculate_checksum(const uint8_t *buf, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
        sum += (buf[i] << 8) | buf[i + 1];  // Big-endian
    }
    return ~sum + 1;  // Two's complement
}
```

### Image Validation Process

When receiving a file via TFTP, the bootloader:

1. **Reads header** — Extracts signature, addresses, and length
2. **Validates signature** — Checks against known signatures table
3. **Verifies checksum** — Sums all 16-bit words of payload + checksum
4. **Rejects if invalid** — Displays `checksum error` message
5. **Flashes to burnAddr** — Writes to the address specified in header

### Creating Valid Images

Use the `cvimg` tool to create properly formatted images:

```bash
# Kernel (from 32-Kernel/build_kernel.sh)
cvimg -i loader.bin -o kernel.img \
      -s cs6c \
      -e 0x80c00000 \
      -b 0x00020000 \
      -a 4k

# Rootfs (from 33-Rootfs/build_rootfs.sh)
cvimg -i rootfs.sqfs -o rootfs.bin \
      -s r6cr \
      -e 0x80c00000 \
      -b 0x200000

# Userdata (from 34-Userdata/build_userdata.sh)
cvimg -i userdata.jffs2 -o userdata.bin \
      -s r6cr \
      -e 0x80c00000 \
      -b 0x400000
```

| Option | Description |
|--------|-------------|
| `-i` | Input file (raw binary) |
| `-o` | Output file (with Realtek header) |
| `-s` | 4-char signature (`cs6c`, `r6cr`, etc.) |
| `-e` | Start/entry address in RAM |
| `-b` | Burn address in flash |
| `-a` | Output alignment (optional) |

See [1-Build-Environment/11-realtek-tools/cvimg](../../1-Build-Environment/11-realtek-tools/cvimg/) for the tool source.

---

## Technical Notes

- **Lexra architecture**: Modified MIPS without unaligned memory access
- **Big-endian**: Watch out for conversions (`le32_to_cpu` macros)
- **SPI flash**: No NAND in this configuration
- **No ASCII logo**: Text-only boot display
- **No rootfs scan**: `SKIP_ROOTFS_SCAN` is set to 1; Linux locates rootfs itself
