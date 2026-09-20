# Choose a Zigbee or Thread radio mode

The EFR32 is a separate processor from the RTL8196E Linux system. Flashing
Linux does not automatically change the radio application, and changing the
radio does not reinstall Linux.

Choose the radio mode from the service you want to run, not from the firmware
name that sounds newest.

On a Sengled G4 whose radio has never been flashed by this project, none of the
commands below work until the radio's own Gecko bootloader is replaced once —
the factory one has no menu for the flasher to drive. That one-time step is in
the [first installation guide](./getting-started.md#sengled-g4-only-install-the-radios-gecko-bootloader-first).

## Short answer

- Want a reliable Zigbee coordinator for **Zigbee2MQTT or ZHA**? Choose **NCP**.
- Want to experiment with **EmberZNet 8.2 on the host**? Choose **RCP**.
- Want to experiment with **same-channel Zigbee + Thread** on a Lidl gateway?
  Choose the RCP multi-PAN stack.
- Want a **Thread Border Router**? Choose **OT-RCP**.
- Want this box to extend an existing Zigbee mesh, without acting as a
  coordinator? Choose **Router**.

NCP is the recommended default for new users.

## Comparison

| Mode | Main use | Where the protocol stack runs | Client connection | Knowledge level |
| --- | --- | --- | --- | --- |
| **NCP** | Zigbee2MQTT or ZHA coordinator | EFR32, EmberZNet 7.5.1 / EZSP v13 | Directly to gateway TCP:8888 | Beginner |
| **RCP** | Modern host-side Zigbee; experimental same-channel Zigbee + Thread on Lidl | `cpcd` + host stacks, EmberZNet 8.2.2 / EZSP v18 | Client connects to `zigbeed`, not directly to the RCP | Advanced |
| **OT-RCP** | Thread/Matter or Zigbee-on-Host | OpenThread or Zigbee stack on gateway/external host | Depends on selected host stack | Intermediate to advanced |
| **Router** | Extend an existing Zigbee mesh | Entire router application on EFR32 | No coordinator client | Intermediate |

## NCP: recommended Zigbee coordinator

Choose NCP if your goal is to pair and control Zigbee devices with
Zigbee2MQTT or Home Assistant ZHA. It has the fewest moving parts: the Zigbee
stack runs on the EFR32 and the Linux gateway transports EZSP frames over TCP.

Flash it with:

```bash
# Lidl
./flash_efr32.sh -y -g <gateway-ip> ncp

# Sengled Smart Hub G4
BOARD=sengled-e39-g8c ./flash_efr32.sh -y -g <gateway-ip> ncp
```

Zigbee2MQTT configuration:

```yaml
serial:
  port: tcp://<gateway-ip>:8888
  adapter: ember
```

Home Assistant ZHA configuration:

```text
Radio type: EmberZNet
Serial device path: socket://<gateway-ip>:8888
```

The default NCP baud is 115200. A TCP client does not set that baud; the
gateway bridge reads it from `/userdata/etc/radio.conf`. Higher pre-built baud
variants exist for the Lidl board, but the default is the best starting point.

Full reference: [NCP-UART-HW](../2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/README.md).

## RCP: EmberZNet 8.2 on the host

RCP moves the Zigbee stack off the Series-1 radio. The EFR32 handles only the
802.15.4 radio layer, while `cpcd` and `zigbeed` run on the Linux host. This
allows the project to expose EmberZNet 8.2.2 / EZSP v18 even though recent
Silabs SDKs no longer target the EFR32MG1B directly.

This path adds several components and a second network endpoint:

```text
EFR32 RCP -> gateway TCP:8888 -> cpcd -> zigbeed -> EZSP endpoint -> Z2M/ZHA
```

Choose RCP only if you specifically need or want the host-side stack. Flashing
the RCP is one step; deploying and maintaining `cpcd` and `zigbeed` is another.

```bash
./flash_efr32.sh -y -g <gateway-ip> rcp
```

For Sengled, prefix the command with `BOARD=sengled-e39-g8c`; its safe default
baud is selected automatically.

Continue with the [RCP guide](../2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/README.md)
and the [EmberZNet 8.x guide](../2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/EMBERZNET-8.x-GUIDE.md).

The Lidl RCP also has an experimental multi-PAN host stack: Zigbee uses IID 1,
Thread uses IID 2, and IID 0 is broadcast. Both networks must use the same
802.15.4 channel on the Series 1 EFR32MG1B. Independent-channel Zigbee + Thread
requires Series 2 Concurrent Listening. Current validation is limited to a
short same-channel test without a real Thread device; see the
[multi-PAN guide](../2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/docker/cpcd-zigbeed-otbr/README.md).

## OT-RCP: Thread or Zigbee-on-Host

OT-RCP exposes the OpenThread Spinel protocol. The same EFR32 image supports
three arrangements:

1. **OTBR on the gateway** — `otbr-agent` runs natively on the RTL8196E. This is
   the default configuration written by `flash_efr32.sh`.
2. **OTBR on another host** — the UART bridge exposes the RCP to a more powerful
   machine running OTBR.
3. **Zigbee-on-Host** — Zigbee2MQTT's `zoh` adapter runs the Zigbee stack on an
   external host.

For a Thread Border Router running on the gateway:

```bash
./flash_efr32.sh -y -g <gateway-ip> otrcp
```

The script writes `MODE=otbr` and the selected baud to `radio.conf`, so Linux
starts `otbr-agent` instead of the TCP bridge. A Thread Border Router supplies
Thread network connectivity; it is not by itself a Matter controller or Home
Assistant installation.

Read the [Thread and Matter primer](../2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/THREAD-MATTER-PRIMER.md)
before commissioning a production Thread network. Deployment details are in
the [OT-RCP guide](../2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/README.md).

## Router: dedicated Zigbee mesh extender

Router firmware turns the EFR32 into a standalone Zigbee 3.0 router. It joins
an existing coordinator's network and relays mesh traffic. It does not expose a
coordinator to Zigbee2MQTT or ZHA.

```bash
./flash_efr32.sh -y -g <gateway-ip> router
```

After flashing, open permit-join on the existing Zigbee coordinator and follow
the [router joining procedure](../2-Zigbee-Radio-Silabs-EFR32/27-Router/README.md).

## Board and baud selection

`flash_efr32.sh` chooses a pre-built file from the selected board and mode. The
normal defaults are:

| Board | NCP | RCP | OT-RCP | Router |
| --- | --- | --- | --- | --- |
| Lidl | 115200 | 460800 | 460800 | 115200 |
| Sengled G4 | 115200 | 230400 | 230400 | 115200 |

The G4 has no RTS/CTS wiring and therefore uses board-specific flow control and
safer high-speed defaults. Do not copy Lidl baud choices to it without reading
the [per-board validation notes](../2-Zigbee-Radio-Silabs-EFR32/boards/README.md).

To see every supported option without changing the gateway:

```bash
./flash_efr32.sh --help
```

## Switching modes safely

- Stop Zigbee2MQTT, ZHA, `cpcd`, or any other client before flashing. Only one
  client can hold TCP port 8888.
- Let `flash_efr32.sh` update `radio.conf`; do not pre-edit the baud to the
  target value.
- Keep the gateway powered until the script reports success and completes its
  reboot.
- After flashing, verify the recorded state:

  ```bash
  ssh root@<gateway-ip> cat /userdata/etc/radio.conf
  ```

If the new mode cannot communicate with the radio, use the
[radio troubleshooting path](./troubleshooting.md#the-efr32-radio-is-unresponsive).
