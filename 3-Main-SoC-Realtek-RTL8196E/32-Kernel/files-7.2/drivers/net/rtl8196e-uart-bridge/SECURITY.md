# Hardening the UART bridge with an SSH tunnel

The in-kernel `rtl8196e-uart-bridge` driver exposes the UART link to the
EFR32 Zigbee NCP as a raw TCP listener (default: `0.0.0.0:8888`, no
authentication, no encryption). On a trusted home LAN this is usually
acceptable; on any segment shared with untrusted peers it is not. This
note describes the recommended hardening path: keep the bridge bound to
loopback on the gateway and reach it from the host through an SSH tunnel.

All instructions assume the current release tree: gateway running the
custom firmware with Dropbear 2026.94 and the `rtl8196e-uart-bridge`
kernel driver; host running a modern OpenSSH (≥ 9.x) and Docker for
Z2M / HA.

---

## Threat model

Without any mitigation, a peer able to reach TCP:8888 on the gateway
can:

- Send arbitrary bytes to the EFR32 NCP, hijacking the Zigbee radio.
- Read every byte the NCP emits, recovering network keys, device join
  events, and sensor payloads.
- Evict the legitimate client and take over the session at any time:
  the bridge replaces the connected client on every new accept
  (replace-on-connect), so the attacker does not even have to wait for
  a disconnect.

IP-based allowlisting (without cryptographic authentication) does not
stop a motivated local attacker: ARP spoofing, DHCP rebinding, or
simply hotplugging a device with the "authorized" IP defeats it. Only
end-to-end cryptographic authentication and encryption provide real
protection — which is exactly what SSH offers for free on this
platform.

One protocol-level note on `flow_control=sw` (v1.2): in that mode the
bridge consumes bare 0x11/0x13 bytes from the *radio→host* stream as
XON/XOFF instead of forwarding them. This is byte-transparent for a
software-flow-control radio firmware (it escapes data-plane 0x11/0x13
by design), but it is one more reason `flash_efr32.sh` must drop
`flow_control` to 0 during a flash — Xmodem payloads carry raw
0x11/0x13. The host→radio direction is never filtered.

Hardware-flow-control capability (v1.5): the `flow_control` knob is
writable by root, but the *hardware* mode is gated by the board itself,
not by whoever writes the knob. The DT boolean `realtek,hw-flow-control`
declares whether RTS/CTS is physically wired; on a board that lacks it,
`param_set_flow_control()` clamps any `hw` request down to `sw`. So a
`flow_control=hw` write (from a bad radio.conf, a typo, or a confused
operator) can never assert CRTSCTS on a UART that has no RTS/CTS lines —
which on such a board would wedge the link rather than protect it. The
clamp is a capability ceiling, enforced in the kernel regardless of the
init scripts.

---

## Architecture

```
  ┌──────────── host machine ────────────┐         ┌──────── gateway ────────┐
  │                                      │         │                         │
  │  Docker   ┌──────────────────────┐   │   SSH   │   ┌─────────────────┐   │
  │  z2m ───→ │ 127.0.0.1:8888       │───┼─tunnel──┼──→│ 127.0.0.1:8888  │   │
  │   (ember  │ (tunnel endpoint)    │   │ ChaCha20│   │ rtl8196e-uart-  │   │
  │    adap.) │                      │   │ Poly1305│   │ bridge          │   │
  │           └──────────────────────┘   │         │   └─────┬───────────┘   │
  │                                      │         │         │ /dev/ttyS1    │
  │   autossh  ┌────────────────┐        │         │         ▼               │
  │  (keeps    │ ssh -L 8888:   │        │         │   ┌──────────────┐      │
  │   tunnel   │   127.0.0.1:   │────────┼─port 22─┼──→│ Dropbear     │      │
  │   alive)   │   8888         │        │         │   │ (pubkey only)│      │
  │            └────────────────┘        │         │   └──────────────┘      │
  └──────────────────────────────────────┘         └─────────────────────────┘
```

Key property: port 8888 is **not reachable from any network interface**
on the gateway. Only port 22 (Dropbear) is exposed, and it only accepts
pubkey authentication. The Z2M ⇄ NCP path crosses the network
exclusively inside the SSH-encrypted channel.

---

## Gateway-side setup

### 1. Bind the bridge to loopback

Edit `/userdata/etc/radio.conf` on the gateway:

```
BRIDGE_BIND=127.0.0.1
FIRMWARE_BAUD=115200    # or whatever baud your NCP firmware is built at
```

Then restart the bridge init script so it re-reads `radio.conf` and
writes the bind address to sysfs before arming:

```sh
/userdata/etc/init.d/S50uart_bridge restart
```

Verify:

```sh
netstat -tln | grep 8888
# Expected:
# tcp  0  0  127.0.0.1:8888  0.0.0.0:*  LISTEN
# (NOT 0.0.0.0:8888)
```

### 2. Harden Dropbear

Edit `/userdata/etc/init.d/S30dropbear` and add the `-s` flag to
disable password logins (pubkey is already enabled by default):

```diff
-DROPBEAR_OPTS="-p 22 -K 300"
+DROPBEAR_OPTS="-p 22 -K 300 -s"
```

Then restart Dropbear:

```sh
/userdata/etc/init.d/S30dropbear restart
```

Before committing this change, make sure at least one host public key
is already deployed:

```sh
ls -la /root/.ssh/authorized_keys
wc -l /root/.ssh/authorized_keys    # should be ≥ 1
```

If the file is empty or missing, copy the host key first (see
host-side section below).

### 3. Sanity-check the host keys

Dropbear on the gateway presents three host keys (ed25519, ecdsa, rsa).
Host clients will pin whichever key they see first. Prefer ed25519 and
keep the others for compatibility:

```sh
ls -la /userdata/etc/dropbear/
```

The `.pub` fingerprints can be printed with:

```sh
dropbearkey -y -f /userdata/etc/dropbear/dropbear_ed25519_host_key | grep -i fingerprint
```

Record the fingerprint and verify it out-of-band on first connection
from the host — it ends up in `~/.ssh/known_hosts` and protects you
against MITM on subsequent connections.

### 4. Verify negotiated crypto

From the host, run:

```sh
ssh -v root@gateway exit 2>&1 | grep 'kex: .* cipher:'
```

Expected lines:

```
kex: server->client cipher: chacha20-poly1305@openssh.com MAC: <implicit>
kex: client->server cipher: chacha20-poly1305@openssh.com MAC: <implicit>
```

ChaCha20-Poly1305 is negotiated by default (it is both sides'
preferred cipher). `MAC: <implicit>` means the AEAD mode is used — one
pass, integrated authentication, ideal on the Lexra CPU (no AES
hardware on RLX4181, ChaCha20's ARX operations map natively to 32-bit
MIPS).

The KEX will be `sntrup761x25519-sha512` (post-quantum hybrid) on
modern OpenSSH ≥ 9.0. You get post-quantum-resistant session keys for
free.

---

## Host-side setup

### 1. Ensure a pubkey is deployed

```sh
# On host (one time, only if not already done):
ssh-copy-id -i ~/.ssh/id_ed25519.pub root@gateway

# Verify:
ssh root@gateway 'cat /root/.ssh/authorized_keys'
```

Prefer an Ed25519 key; it's small, fast to verify on Lexra, and modern.

### 2. Basic one-shot tunnel (for testing)

```sh
ssh -N -L 127.0.0.1:8888:127.0.0.1:8888 root@gateway
```

Flags:

- `-N` : no remote command, just the tunnel.
- `-L ADDR:PORT:HOST:PORT` : forward local `127.0.0.1:8888` to
  `127.0.0.1:8888` on the gateway (where the bridge listens).

From a second terminal on the host, Z2M can now be pointed at
`tcp://127.0.0.1:8888` (see Z2M section below).

### 3. Robust always-on tunnel with autossh

One-shot `ssh -L` dies whenever the underlying TCP connection drops
(gateway reboot, Wi-Fi hiccup, etc.). Use [autossh][autossh] — it
restarts the tunnel automatically.

[autossh]: https://www.harding.motd.ca/autossh/

Install:

```sh
# Ubuntu / Debian
sudo apt install autossh

# Arch
sudo pacman -S autossh
```

Invocation:

```sh
AUTOSSH_POLL=30 \
AUTOSSH_FIRST_POLL=30 \
AUTOSSH_GATETIME=0 \
autossh -M 0 -N \
  -o ServerAliveInterval=15 \
  -o ServerAliveCountMax=2 \
  -o ExitOnForwardFailure=yes \
  -o StrictHostKeyChecking=yes \
  -L 127.0.0.1:8888:127.0.0.1:8888 \
  root@gateway
```

Why each option:

- `AUTOSSH_GATETIME=0` : start monitoring immediately, no "success
  period" grace (useful on flaky networks).
- `-M 0` : disable autossh's own monitor port; rely on SSH's
  `ServerAliveInterval` to detect dead tunnels. Simpler and avoids
  opening another port.
- `ServerAliveInterval=15` + `ServerAliveCountMax=2` : send a
  keepalive every 15 s; tear down after 2 missed replies (30 s).
- `ExitOnForwardFailure=yes` : if the remote `127.0.0.1:8888` can't be
  bound (bridge not armed yet), fail fast so autossh retries.
- `StrictHostKeyChecking=yes` : refuse to proceed if the gateway host
  key changes unexpectedly (post-reflash, after `/userdata` wipe, etc.
  — you want to see this and verify fingerprint manually).

### 4. systemd unit (recommended for daemons)

Save as `/etc/systemd/system/zigbee-tunnel.service`:

```ini
[Unit]
Description=SSH tunnel to RTL8196E UART bridge
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=<your-user>
Environment="AUTOSSH_GATETIME=0"
ExecStart=/usr/bin/autossh -M 0 -N \
  -o ServerAliveInterval=15 \
  -o ServerAliveCountMax=2 \
  -o ExitOnForwardFailure=yes \
  -o StrictHostKeyChecking=yes \
  -i /home/<your-user>/.ssh/id_ed25519 \
  -L 127.0.0.1:8888:127.0.0.1:8888 \
  root@gateway
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now zigbee-tunnel.service
sudo systemctl status zigbee-tunnel.service
```

Expected: `active (running)`; `journalctl -u zigbee-tunnel -f` will
show reconnection attempts if the gateway reboots.

### 5. Point Z2M at the tunnel

Edit `2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/docker/z2m/configuration.yaml`:

```diff
 serial:
-  port: tcp://192.168.1.88:8888
+  port: tcp://127.0.0.1:8888
   adapter: ember
```

Then restart the Z2M stack:

```sh
cd 2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/docker
docker compose up -d zigbee2mqtt
```

Important: if Z2M runs inside Docker with the default bridge network,
`127.0.0.1` inside the container is **not** the host's loopback.
Either:

- Run Z2M with `network_mode: host` (recommended for this setup —
  same network namespace as the host's loopback). Home Assistant
  already uses `network_mode: host` in the bundled compose; Z2M
  does not by default. Add `network_mode: host` under the
  `zigbee2mqtt` service definition in `docker-compose.yml`.

- Or bind the tunnel's local endpoint on the docker bridge IP
  (typically `172.17.0.1`), e.g. `-L 172.17.0.1:8888:127.0.0.1:8888`,
  and have Z2M point at `tcp://172.17.0.1:8888`. Less clean.

### 6. Verify end to end

```sh
# On host:
nc -zv 127.0.0.1 8888        # tunnel endpoint is listening

ssh root@gateway 'netstat -tn | grep 8888'
# Expected: a single ESTABLISHED connection from 127.0.0.1 to 127.0.0.1
# (the other side of the SSH-forwarded local port), NOT one from your
# host's LAN IP.

docker logs zigbee2mqtt | grep -E 'ASH started|Network up'
# Expected: ASH and Zigbee network come up normally via the tunnel.
```

---

## Failure modes and recovery

| Symptom | Cause | Fix |
|---|---|---|
| `autossh` dies immediately with "ssh_exchange_identification" | Dropbear not yet up on gateway | autossh retries in 5 s (systemd Restart=always); no action needed |
| `Z2M: Error: connect ECONNREFUSED 127.0.0.1:8888` | Tunnel down at the moment Z2M connected | autossh will re-establish; Z2M will retry on its own (docker restart policy unless-stopped) |
| `autossh: WARNING: unknown host key` | Gateway host key changed (reflash, JFFS2 wipe) | Verify gateway fingerprint out of band, then `ssh-keygen -R gateway` on host; restart tunnel service |
| High CPU on the gateway while traffic flows | Expected overhead of ChaCha20-Poly1305 | Typically < 1% on Lexra 400 MHz at NCP 115200 baud; <5% at 892857 baud |
| Tunnel stays up but Z2M times out | SSH keepalive masks a broken bridge | Reduce `ServerAliveInterval` to 5, `ServerAliveCountMax=1`; still prefer loosing the tunnel over silently forwarding a dead socket |

For deep diagnostics, run the tunnel with `-vv` on the host and watch
`journalctl -u dropbear` (not supported on BusyBox — tail
`/var/log/messages` instead) on the gateway.

---

## What this does not protect against

- **Compromise of the host running Z2M.** If an attacker controls the
  host, they have the SSH key and can open the tunnel themselves. Use
  a passphrase-protected key and `ssh-agent`; never store a plaintext
  key in a Docker image layer.
- **Compromise of the gateway itself.** If an attacker has root on the
  gateway, the tunnel is irrelevant — they read `/dev/ttyS1` directly.
  Keep the gateway patched, keep Dropbear modern, disable password
  auth as described, and minimise network surface.
- **Metadata disclosure.** An observer on the LAN still sees TCP
  connections between host and gateway on port 22. Only the bytes
  flowing are confidential.

This document complements `DESIGN.md` (driver rationale) and lives
alongside the driver source for easy discovery by future maintainers.
