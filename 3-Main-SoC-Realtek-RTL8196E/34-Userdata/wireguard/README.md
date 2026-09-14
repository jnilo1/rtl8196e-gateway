# WireGuard — built here, deliberately not shipped

This directory holds a complete, working WireGuard setup for the gateway:
`wg(8)`, a minimal RTNL link helper, a lifecycle script, an init script and a
config template. **None of it is part of the userdata image**, and the shipped
kernels do not build in the WireGuard driver either.

That is a deliberate choice, not an oversight. This file explains the reason and
how to put it back if you want the tunnel.

## Why it is not shipped

`CONFIG_WIREGUARD=y` costs **3.7 Mbit/s of Ethernet TX** on this SoC — about
5 % — while never executing a single instruction of WireGuard.

The cost has nothing to do with the feature working or not. It comes from
*where* the option puts its code in the link. WireGuard is declared at
`drivers/net/Makefile` line 13, ahead of `ethernet/` and therefore ahead of the
whole network stack. Its ~71 KiB push every hot symbol downstream:
`rtl8196e_poll` moves by 81 920 bytes, `__dev_queue_xmit` by 64 724,
`softnet_data` by 90 368. The RLX4181 has a 16 KiB instruction cache and an
8 KiB data cache, and that relocation is worth several percent of throughput.

This was measured, not assumed:

| build | TX (Mbit/s) |
|---|--:|
| `CONFIG_WIREGUARD=n` | 69.95 |
| `CONFIG_WIREGUARD=y` | 66.3 |
| everything WireGuard *selects*, but `WIREGUARD=n` | 70.1 |
| 71 392 bytes of **inert** padding at WireGuard's link slot | 66.9 |

The third row shows the pulled-in dependencies (`NET_UDP_TUNNEL`, `DST_CACHE`,
`GRO_CELLS`, the `CRYPTO_LIB_*` family) cost nothing. The fourth shows dead
bytes at the same place reproduce the loss exactly. Measurements were
interleaved `n, y, n, y` so that drift could not be read as effect, and no `wg`
interface ever existed in any run.

Charging every gateway 5 % of its TX throughput for a feature that defaults to
off did not seem a good trade.

`CONFIG_WIREGUARD=m` may be the better route for whoever wants the tunnel. The
shipped configs have `CONFIG_MODULES` off, but this release is the one that
makes turning it on possible at all — a modular build used to stop at
`vermagic.h`, which had no arm for the Lexra core. A module is not in the
`vmlinux` link, so it cannot push the network stack downstream, and the
measurement agrees: `MODULES=y WIREGUARD=m` gives 68.0 / 67.9 against a
shipping baseline of 69.6 / 67.4 — 0.55 against a ±0.9 repeatability floor,
so no measurable cost, even though `CONFIG_MODULES=y` adds 381 KiB of `.text`.
What nobody has measured is a module actually **loaded** and carrying traffic:
whether running from vmalloc space penalises the tunnel is an open question.

Full write-up: `20260808_wireguard-cout-de-position-lien.md` in the maintainer's
bench notes.

## What is in here

```
build_wireguard.sh                     builds wg + wg-link (cross, static)
src/wg_link.c                          minimal RTNL "ip link add type wireguard"
wireguard-tools-1.0.20260223/          upstream source
optional/etc/init.d/S60wireguard       boot hook, no-op while ENABLED=0
optional/etc/wireguard.conf            ENABLED flag and explicit route list
optional/etc/wireguard/wg0.conf.example
optional/usr/sbin/wireguardctl         genkey / enable / disable / status
optional/usr/bin/wg                    built by build_wireguard.sh
optional/usr/sbin/wg-link              built by build_wireguard.sh
```

`optional/` mirrors the on-target layout, so installing is a plain copy.

## Installing it on a gateway that wants it

**1. Rebuild a kernel with the driver.** Set `CONFIG_WIREGUARD=y` in
`../../32-Kernel/config-<line>-realtek.txt`, rebuild and flash:

```sh
cd ../../32-Kernel
./build_kernel.sh                       # or KERNEL=7.2, BOARD=sengled-e39-g8c
../flash_remote.sh -y kernel <gateway-ip>
```

Expect the throughput cost described above. It is not a bug and it will not go
away by tuning anything.

**2. Build the userland tools.**

```sh
./build_wireguard.sh                    # from this directory
```

They land in `optional/usr/{bin,sbin}/`.

**3. Install onto the running gateway.** `/userdata` is writable JFFS2, so this
survives a reboot but **not** a userdata reflash:

```sh
GW=<gateway-ip>
scp -O -r optional/etc/* root@$GW:/userdata/etc/
scp -O optional/usr/bin/wg        root@$GW:/userdata/usr/bin/
scp -O optional/usr/sbin/wg-link  root@$GW:/userdata/usr/sbin/
scp -O optional/usr/sbin/wireguardctl root@$GW:/userdata/usr/sbin/
ssh root@$GW 'chmod +x /userdata/usr/bin/wg /userdata/usr/sbin/wg-link \
                       /userdata/usr/sbin/wireguardctl \
                       /userdata/etc/init.d/S60wireguard'
```

Use `scp -O`: dropbear on the gateway ships no sftp-server.

To make it survive reflashes instead, copy `optional/` back over
`../skeleton/` and rebuild the image — but then every gateway you flash carries
it, which is what this split exists to avoid.

**4. Configure and enable.**

```sh
ssh root@$GW '/userdata/usr/sbin/wireguardctl genkey'   # writes wg0.conf, 0600
# edit /userdata/etc/wireguard/wg0.conf with your peer
# set ENABLED=1 in /userdata/etc/wireguard.conf
ssh root@$GW '/userdata/etc/init.d/S60wireguard start'
```

Only the routes listed explicitly in `wireguard.conf` are installed, so enabling
the tunnel cannot silently replace the gateway's default route.

## If you remove it again

Deleting the files under `/userdata` is enough on the target. On the build side,
reverting `CONFIG_WIREGUARD` to `n` and rebuilding the kernel restores the
throughput — the two are independent.
