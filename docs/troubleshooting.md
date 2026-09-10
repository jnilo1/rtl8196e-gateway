# Troubleshooting

Start with the symptom, then collect evidence before reflashing. The gateway has
two processors and two distinct failure domains:

- if SSH and Ethernet work but Zigbee/Thread does not, investigate the EFR32
  radio path;
- if the whole gateway disappears, investigate Linux, Ethernet, power, or the
  RTL8196E boot path;
- if this is the first installation, begin with UART and bootloader networking.

## Find the right section

| Symptom | Start here |
| --- | --- |
| No serial text, or unreadable characters | [No readable serial output](#no-readable-serial-output) |
| Linux boots before you see `<RealTek>` | [Cannot enter the bootloader](#cannot-enter-the-bootloader) |
| An upgrade reboots back into Linux | [The upgrade reboots straight back into Linux](#the-upgrade-reboots-straight-back-into-linux) |
| Installer cannot find `192.168.1.6` | [Bootloader not detected](#bootloader-not-detected) |
| TFTP times out or upload fails | [TFTP transfer fails](#tftp-transfer-fails) |
| First boot loops after a full flash | [Boot loop after the first full flash](#boot-loop-after-the-first-full-flash) |
| Gateway boots but SSH is unavailable | [SSH unavailable after boot](#ssh-unavailable-after-boot) |
| Zigbee2MQTT/ZHA cannot open port 8888 | [Radio client cannot connect](#radio-client-cannot-connect) |
| Zigbee/Thread stops, but SSH still works | [EFR32 radio unresponsive](#the-efr32-radio-is-unresponsive) |
| Radio flash fails on a never-flashed Sengled G4 | [EFR32 flash fails](#efr32-flash-fails) |
| The entire gateway vanishes from the LAN | [Gateway disappears from the network](#the-gateway-disappears-from-the-network) |
| RX `drop` counter is unexpectedly large | [Large RX drop count](#eth0-reports-a-large-rx-drop-count) |

## First-install and bootloader problems

### No readable serial output

The RTL8196E console must use **38400 baud, 8N1, no flow control**. For the Lidl
J1 header, verify all three connections:

| J1 | Adapter |
| --- | --- |
| Pin 2 GND | GND |
| Pin 3 gateway TX | RX |
| Pin 4 gateway RX | TX |

Leave pin 1 VCC disconnected and power the gateway normally.

Then check, in order:

1. the host opened the correct `/dev/ttyUSB*` device;
2. the adapter uses 3.3 V TTL logic, not RS-232 or 5 V logic;
3. ground is common;
4. TX and RX are crossed;
5. the temporary contacts or solder joints are reliable;
6. another program is not already holding the serial device.

Garbled but changing text almost always indicates the wrong baud. No text at all
usually indicates wiring, device selection, or power.

### Cannot enter the bootloader

The bootloader looks at the serial line only briefly, and it acts on the first
character it finds waiting there. Tapping `Esc` after the board is already
running often misses. Hold the key down before power reaches the board instead,
and entry becomes reliable:

1. open the terminal at 38400 8N1, no flow control;
2. remove power from the gateway;
3. press `Esc` and keep it held down;
4. restore power with the key still held.

Keyboard auto-repeat then keeps `Esc` arriving from the first moment the
bootloader starts listening, so there is no window left to miss. Release the key
once `<RealTek>` appears.

How you cut and restore power depends on your wiring, and the two arrangements
must not be mixed:

- with the official supply connected, keep the serial adapter on three wires
  only (GND, gateway TX, gateway RX), pin 1 VCC disconnected, and power-cycle at
  the official supply;
- with no official supply, powering the board from the adapter's 3.3 V on pin 1
  is convenient: unplug that wire to remove power, plug it back to restore it.

Never connect the adapter's 3.3 V while the official supply is plugged in, or
two sources end up driving the same rail.

If `Esc` still has no effect, suspect the host-to-gateway direction of the link
rather than the bootloader. Let Linux boot and try typing at the serial console.
If output arrives but nothing you type has any effect, check the pin 4 contact:
it carries only what you transmit, so it can be bad while the banner still reads
perfectly.

On a gateway already running this project's firmware, serial entry is usually
unnecessary: use the [upgrade guide](./upgrading.md), which invokes `boothold`
over SSH.

### The upgrade reboots straight back into Linux

`flash_install_rtl8196e.sh <IP>` and `flash_remote.sh` do not use a reset pin: they
run `boothold` over SSH, which writes a magic word to a page of DRAM the running
kernel reserves for it, then reboot. The bootloader finds the word on the next
reset, prints `---Boot hold requested` and stops in download mode.

If the gateway instead comes back on Linux, and the script reports that no
bootloader was detected, read the address `boothold` printed:

```
Arming boot hold...
  Boot hold set at 0x01FFEFFC (TFTP server IP 192.168.1.6).
```

That address comes from the running kernel's device tree. The bootloader reads a
constant compiled into it, one per board — the top of DRAM minus 0x2000, so
`0x01FFEFFC` on the 32 MiB Lidl board and `0x03FFEFFC` on the 64 MiB Sengled
E39-G8C. The two must be the same page; a bootloader built for another board
looks elsewhere, finds nothing and boots normally, with no message on either
side.

The bootloader banner on the serial console names the board its image was built
for:

```
Realtek RTL8196E  CPU: 400MHz  RAM: 32MB  Flash: GD25Q127C
```

If that RAM figure is not the memory your board really has, reflash the
bootloader for your board (`BOARD=<board>`) — entering the bootloader with `Esc`
on the serial console, since the SSH route is the one that is broken. If the
figure is right, capture the console log of the reboot and open an issue.

A kernel too old to declare the page is the other case: `boothold` then refuses
to write and the script stops immediately, saying so, with the gateway still
running its current firmware.

### Bootloader not detected

The stock bootloader normally listens at `192.168.1.6`. The computer needs an
address on the same direct Ethernet segment:

```bash
ip route get 192.168.1.6
```

If the output contains `via <router>`, add a temporary address to the interface
that faces the gateway:

```bash
sudo ip addr add 192.168.1.10/24 dev <interface>
```

Check that:

- the gateway is still at the `<RealTek>` prompt;
- Ethernet link is up;
- no other device uses `192.168.1.6`;
- the computer does not itself own `192.168.1.6`;
- a host firewall is not blocking TFTP/UDP.

The installer requires an actual TFTP response, not only ping or an ARP entry.
With a non-default bootloader address, pass the same address to the script with
`--boot-ip` and configure the stock bootloader with `IPCONFIG` when required.

### Host prerequisites are missing

The full installer checks its required commands before modifying the gateway.
For a normal pre-built-image installation on Ubuntu:

```bash
sudo apt install fakeroot gcc mtd-utils squashfs-tools tftp-hpa \
  netcat-openbsd iproute2 iputils-ping openssh-client
```

Ubuntu's `tftp-hpa` is required; another program named `tftp` may not support
the command-line interface used by the scripts. The complete cross-toolchain is
only required when rebuilding firmware. `xxd` is not required; the image and
radio scripts use `od` from the base `coreutils` package. EFR32 flashing also
needs `python3`, `python3-venv`, and `patch`.

### TFTP transfer fails

Check:

1. computer and bootloader are on the same L2 subnet;
2. UDP port 69 is not blocked;
3. no host TFTP server conflicts with the bootloader service;
4. the Ethernet interface and temporary address are still up;
5. the serial console remains at the bootloader prompt.

If the transfer never began, nothing has been written. If a flash write began,
do not remove power merely because the operation is slow. The userdata region
can take one or two minutes on the stock loader; wait for the serial success or
failure message and the installer's result.

### Boot loop after the first full flash

This can occur only on the first handoff from an older, pre-V2.9 bootloader. The
old bootloader can leave switch DMA active while the newly flashed kernel starts.

Unplug the gateway for a few seconds, then power it on again. A warm `reboot`
does not clear the same hardware state. The replacement V2.9 bootloader stops
the DMA engine, so later full flashes boot normally.

### SSH unavailable after boot

Wait at least 30 seconds, then check the serial console for the assigned address:

```bash
ip addr show dev eth0
```

Confirm whether the install selected DHCP or a static address. For DHCP, inspect
the router's lease table. For static networking, the address is the one accepted
at the installer's prompt; when the prompt was left at its default, that is an
address in the subnet of the machine that ran the installer, with host part 88
(`192.168.0.88` on a `192.168.0.0/24` LAN). The value is also recorded as `GW_IP`
in `.gateway-state` at the repository root.

If DHCP was selected and no lease was ever obtained, the gateway falls back to
the static configuration in `/userdata/etc/eth0.bak` — check that file for the
address to try.

If the serial console shows Linux but no address:

```bash
cat /userdata/etc/eth0.conf 2>/dev/null
dmesg | tail -n 50
```

An `eth0.conf` file selects static mode; its absence selects DHCP. Correct the
file from the serial console and reboot. If the gateway never reaches Linux,
keep the serial boot log and use the bootloader restore path rather than
repeating blind flashes.

## Radio and client problems

### Radio client cannot connect

Only one process can own TCP port 8888. Stop all other candidates —
Zigbee2MQTT, ZHA, `cpcd`, a flasher, or a test socket — then retry.

On the gateway, inspect the persisted mode and bridge:

```bash
cat /userdata/etc/radio.conf
cat /sys/module/rtl8196e_uart_bridge/parameters/armed
cat /sys/module/rtl8196e_uart_bridge/parameters/stats
```

For an NCP Zigbee setup, `radio.conf` should contain `FIRMWARE=ncp` and should
not contain `MODE=otbr`. Zigbee2MQTT uses:

```yaml
serial:
  port: tcp://<gateway-ip>:8888
  adapter: ember
```

Home Assistant ZHA uses an EmberZNet radio with
`socket://<gateway-ip>:8888`. Do not set a client-side UART baud for the TCP
connection; Linux gets the physical baud from `radio.conf`.

If `BRIDGE_BIND=127.0.0.1` is set, remote connections are intentionally refused.
Use the configured SSH tunnel or restore a trusted-LAN bind.

### The EFR32 radio is unresponsive

This condition means Linux and SSH still work, but Z2M, ZHA, or OTBR cannot
communicate with the EFR32.

Try these recovery surfaces in order:

1. Hold the front-panel button for five seconds. The status LED gives hold
   feedback; the service pulses EFR32 reset and restarts the radio daemon.
2. Run `ssh root@<gateway-ip> recover_efr32`.
3. Run `ssh root@<gateway-ip> reboot`.

If none works, stop the normal radio client, power-cycle once, and rerun the
flasher with the intended firmware:

```bash
./flash_efr32.sh -y -g <gateway-ip> ncp
```

Use the correct `BOARD` for Sengled. The script probes the running application
and Gecko bootloader and includes protocol-specific fallbacks.

Repeated `HandleRcpTimeout()` or `Failed to communicate with RCP` errors on an
old v3.1.x/v3.2.x OT-RCP installation at 460800 baud were fixed in v3.3.0 by
enabling UART hardware flow control in `otbr-agent`. Upgrade the Linux firmware
before treating that historical failure as a damaged radio.

The recovery architecture and limits are documented in the
[EFR32 bootloader recovery post-mortem](../2-Zigbee-Radio-Silabs-EFR32/POST-MORTEM-bootloader-recovery.md).

### UART errors or intermittent radio timeouts

If radio failures correlate with traffic bursts, measure the RTL8196E UART1
counters instead of relying on an old cumulative `oe:` value:

```bash
scp -O 3-Main-SoC-Realtek-RTL8196E/32-Kernel/tools/uart-overrun-monitor \
  root@<gateway-ip>:/tmp/
ssh root@<gateway-ip> 'chmod +x /tmp/uart-overrun-monitor'
ssh root@<gateway-ip> \
  '/tmp/uart-overrun-monitor -i 2 -d 120 -o /tmp/uart-errors.csv'
scp -O root@<gateway-ip>:/tmp/uart-errors.csv .
```

Reproduce the workload during the two-minute capture. Positive `d_oe` values
mean the UART hardware dropped bytes in that interval; positive `d_fe` or
`d_pe` values indicate framing or parity errors. Verify that the EFR32 firmware,
`FIRMWARE_BAUD`, and `FIRMWARE_FLOW_CTRL` in `/userdata/etc/radio.conf` agree.
Do not simply increase the baud. The Sengled G4 has no RTS/CTS wiring and uses
lower board defaults for this reason.

See the [UART overrun monitor reference](../3-Main-SoC-Realtek-RTL8196E/32-Kernel/tools/README.md)
for every CSV column and interpretation guidance.

### EFR32 flash fails

Before retrying:

1. stop Zigbee2MQTT, ZHA, `cpcd`, or OTBR so the flasher has exclusive access;
2. confirm SSH reaches the correct gateway;
3. confirm the selected board matches `cat /proc/device-tree/model`;
4. keep the gateway powered and rerun without `--force`;
5. save the complete flasher output.

`flash_efr32.sh` temporarily changes the bridge to the Gecko bootloader's
115200/no-flow-control mode and restores runtime configuration afterwards. Do
not manually pre-set bridge parameters unless following a specific recovery
procedure.

On a Sengled G4 whose radio has never been flashed by this project, the failure
is expected and no amount of retrying helps: the factory Gecko bootloader has no
menu, so `universal-silabs-flasher` cannot drive it. Replace that bootloader once
with a plain XMODEM client, as described in
[step 12 of the first installation guide](./getting-started.md#sengled-g4-only-install-the-radios-gecko-bootloader-first),
then flash the application normally.

One line in a *successful* flash reads like a failure and is not one:

```text
INFO Failed to read firmware metadata: KeyError('No tag with id <GBLTagId.METADATA: ...> exists')
```

The flasher looks for an optional metadata tag that this project's application
images do not carry, says so at INFO level, and flashes them anyway. It is left
in place deliberately: filtering it means routing application flashes through a
line-buffered filter, and that is what once swallowed the upload progress bar —
a live bar is worth more than one silenced INFO line. Judge an application flash
by `Flash complete.` and by the version read back afterwards.

## Whole-gateway and network problems

### The gateway disappears from the network

This is distinct from a radio failure: SSH, radio services, and every network
response stop together, and a power cycle restores the box.

After recovery, collect persistent evidence before another power cycle:

```bash
dmesg
cat /userdata/netwatch/incidents.log 2>/dev/null
ls -l /userdata/panic 2>/dev/null
cat /proc/net/dev
ip route
```

The hardware watchdog detects a stopped CPU, but it cannot detect a live
userspace whose network path is dead because its userspace feeder continues to
kick the watchdog. The optional `netwatch` service covers that case by writing a
persistent incident snapshot and rebooting after a long failure with carrier
still up.

`netwatch` is shipped disabled because it can reboot the gateway. For a remote
installation, read the
[netwatch reference](../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#netwatch--network-isolation-watchdog-s80netwatch)
before enabling it. `DRY_RUN=1` records evidence without rebooting.

### eth0 reports a large RX drop count

`ifconfig`, `ip -s link`, or `/proc/net/dev` can show a steadily growing RX
`drop` value while `errs` and `fifo` remain zero. This does not necessarily mean
the Ethernet driver lost useful traffic.

If `ethtool` is present, check the driver's own allocation/drop counters:

```bash
ethtool -S eth0 | grep -E 'rx_no_skb|rx_alloc_fail|rx_mbuf_no_shadow'
```

When those counters are zero, the higher RX `drop` total is commonly the kernel
counting intact Ethernet frames for protocols the gateway does not consume. A
frequent source is Realtek loop-detection traffic with ethertype `0x8899`,
broadcast by other equipment about once every two seconds.

Watch `errs`, `fifo`, the driver-specific counters, TCP retransmissions, and
actual application symptoms. The detailed mechanism and measurements are in
the [Ethernet driver design](../3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18/drivers/net/ethernet/rtl8196e-eth/DESIGN.md).

## Ask for help effectively

For setup questions, open a
[GitHub Discussion](https://github.com/jnilo1/rtl8196e-gateway/discussions).
For a reproducible defect, open an
[Issue](https://github.com/jnilo1/rtl8196e-gateway/issues).

Include:

- exact board model;
- project version and `uname -r`;
- whether the gateway still answers SSH;
- `/userdata/etc/radio.conf` for radio problems;
- complete command output, not only the final line;
- relevant serial boot log or `dmesg`;
- what changed immediately before the failure.

Remove passwords, SSH private keys, Thread credentials, serial numbers, and
user backups before posting logs.
