# Use and maintain the gateway

This page covers normal administration after installation. Detailed internals
remain in the component references; the commands here are the safe, common
operations.

## First-login checklist

Connect over SSH:

```bash
ssh root@<gateway-ip>
```

On a fresh image, the password is `root`. Complete these steps before relying
on the gateway:

1. Change the password with `passwd`.
2. Confirm the board and kernel with `cat /proc/device-tree/model` and `uname -r`.
3. Confirm `/userdata/etc/radio.conf` matches the EFR32 firmware you flashed.
4. Add an SSH public key.
5. Make a post-install full-flash backup.

## Set up SSH keys

From the administration computer:

```bash
ssh-copy-id root@<gateway-ip>
```

The persistent key file is `/userdata/ssh/authorized_keys`. Dropbear provides
legacy SCP rather than an SFTP server; with recent OpenSSH clients, use `scp -O`
when copying a file:

```bash
scp -O local-file root@<gateway-ip>:/userdata/
```

## Understand persistent storage

The base root filesystem is a read-only SquashFS. Persistent configuration,
SSH keys, applications, Thread credentials, and local changes live under
`/userdata` on JFFS2.

The normal full-system upgrader preserves `/userdata` configuration and files.
A bare `flash_userdata.sh` is a clean partition replacement and can erase those
changes, so it is a developer tool rather than a routine update command.

The full layout and init sequence are documented in
[Userdata](../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md).

## Network configuration

### Static address

Create or edit `/userdata/etc/eth0.conf`:

```text
IPADDR=192.168.1.88
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
DNS=192.168.1.1
DOMAIN=home.lan
```

`DNS` and `DOMAIN` are optional. Reboot to apply a remote address change safely:

```bash
reboot
```

Your SSH session will disconnect. Reconnect at the new address.

Changing the address this way does not reach back to the computer you run the
flash scripts from. That machine remembers the address of the last install in
`.gateway-state`, so commands that take no address argument will keep aiming at
the old one until you tell it otherwise — set `GW_IP` in `gateway.env`, or
delete `.gateway-state`. The scripts print the address and where it came from
before they act, so a stale record shows up as a failed connection rather than
as an action against the wrong host.

### DHCP

DHCP mode is selected by the absence of `/userdata/etc/eth0.conf`. Preserve a
copy elsewhere if you may want the static settings again, remove the file, and
reboot. Find the new address in the DHCP server's lease table.

The gateway keeps a generated local MAC address in
`/userdata/etc/mac_address`, so DHCP identity survives reboots and upgrades —
including a full flash, which re-injects the file. **Reserve a lease for that MAC
on your router**: the gateway then takes its address from DHCP while the address
itself stays fixed, which is what the host-side scripts need to find it. Record
the reserved address as `GW_IP` in `gateway.env` and no script has to guess.

The gateway also sends its hostname in the DHCP request (option 12), so many
routers will resolve `rtl8196e-gw` on the LAN. That is the fallback the scripts
use when no address is recorded and the mode is DHCP; it depends entirely on the
router, as the gateway runs no mDNS responder of its own.

If no lease ever arrives, `udhcpc.script` applies the static configuration in
`/userdata/etc/eth0.bak` so the gateway stays reachable. The flash scripts write
that file with an address in the subnet the gateway was installed on — high in
the range, clear of the address used in static mode. It is regenerated on every
full install, so edit it on the device only for a temporary change; for a
permanent one, set the values in `gateway.env` before installing. The same file
is what you copy over `eth0.conf` to return to a static address.

## Hostname, timezone, and time service

Edit the persistent files:

```bash
nano /userdata/etc/hostname
nano /userdata/etc/TZ
nano /userdata/etc/ntp.conf
```

`TZ` uses a POSIX timezone string, for example:

```text
CET-1CEST,M3.5.0/2,M10.5.0/3
```

Reboot after changing the hostname. Timezone and NTP details are in the
[userdata reference](../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md).

## Inspect the radio state

`flash_efr32.sh` maintains the host-side record of the EFR32 application:

```bash
cat /userdata/etc/radio.conf
```

Typical NCP state:

```text
FIRMWARE=ncp
FIRMWARE_VERSION=7.5.2
FIRMWARE_BAUD=115200
FIRMWARE_FLOW_CTRL=hw
```

Sengled uses board-specific flow control, so do not copy a Lidl
`radio.conf` onto a G4. Re-run `flash_efr32.sh` with the correct `BOARD` rather
than manually guessing the baud or flow mode.

In Zigbee bridge mode, useful checks are:

```bash
cat /sys/module/rtl8196e_uart_bridge/parameters/armed
cat /sys/module/rtl8196e_uart_bridge/parameters/stats
```

Only one client can own TCP port 8888. Stop ZHA or Zigbee2MQTT before running
the EFR32 flasher or connecting a different client.

## Protect the Zigbee TCP bridge

TCP port 8888 transports raw radio protocol and does not authenticate clients.
The default `0.0.0.0` bind is convenient on a trusted home LAN but should not be
exposed to the internet or an untrusted network.

For a stricter setup, add this to `/userdata/etc/radio.conf`:

```text
BRIDGE_BIND=127.0.0.1
```

Restart the bridge and connect through an SSH tunnel. The complete tunnel and
container recipes are in the
[UART bridge security guide](../3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18/drivers/net/rtl8196e-uart-bridge/SECURITY.md).

## LED brightness

The persistent LED configuration is `/userdata/etc/leds.conf`:

```text
MODE=dim
```

Supported values are `bright`, `dim`, and `off`. Apply a change without
rebooting:

```bash
/userdata/etc/init.d/S11leds start
```

## Back up regularly

From the repository on another computer:

```bash
./backup_gateway.sh --linux-ip <gateway-ip> \
  --output /path/outside/the/repository/gateway-backup
```

Keep at least one known-good 16 MiB backup away from the working tree and away
from the gateway itself. Back up before a full upgrade, board/kernel switch, or
manual partition flash.

Restore procedures are in
[Backup and restore](../3-Main-SoC-Realtek-RTL8196E/30-Backup-Restore/README.md).

## Upgrade the firmware

Use the full installer for release upgrades:

```bash
./flash_install_rtl8196e.sh -y <gateway-ip>
```

It prepares the image while Linux is still running and preserves persistent
configuration. Read the [upgrade guide](./upgrading.md) before changing board or
kernel selections.

## Recover an unresponsive radio

If SSH still works but Zigbee or Thread does not, try these in order:

1. Hold the front-panel button for five seconds. The status LED indicates the
   hold; the gateway resets the EFR32 and restarts the radio service.
2. Run `ssh root@<gateway-ip> recover_efr32`.
3. Run `ssh root@<gateway-ip> reboot`.

These actions do not erase the Zigbee network. If they fail, continue with
[The EFR32 radio is unresponsive](./troubleshooting.md#the-efr32-radio-is-unresponsive).

## Logs and incident records

Useful runtime information includes:

```bash
dmesg
cat /var/log/messages 2>/dev/null
ls -l /userdata/panic /userdata/netwatch 2>/dev/null
```

The hardware watchdog is enabled, while `netwatch` — which can reboot a gateway
whose network path is dead — is deliberately disabled by default. Enable it
only after reading the
[netwatch reference](../3-Main-SoC-Realtek-RTL8196E/34-Userdata/README.md#netwatch--network-isolation-watchdog-s80netwatch).

## Advanced customization warning

Every file matching `S??*` in `/userdata/etc/init.d/` runs at boot. Do not leave
copies such as `S50uart_bridge.bak` or `S70otbr.old` in that directory: they also
match the startup glob and can launch duplicate services. Store backups outside
the init directory.
