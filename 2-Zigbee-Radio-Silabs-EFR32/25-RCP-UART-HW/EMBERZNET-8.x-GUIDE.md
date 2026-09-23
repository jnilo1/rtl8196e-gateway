# Running EmberZNet 8.x on a Series 1 radio

This page explains *why* the RCP path works. To set it up, follow the
[RCP README](./README.md).

## The Series 1 deprecation problem

Every board this firmware supports carries a **Silicon Labs Series 1** radio —
an **EFR32MG1B** on the Lidl Silvercrest Gateway, an **EFR32MG13P** on the
Sengled Smart Hub G4.

Silicon Labs stopped supporting Series 1 chips in its newer Zigbee stacks:

| SDK | EmberZNet | Series 1 support |
|-----|-----------|------------------|
| Gecko SDK 4.x | 7.5.x | ✅ Last supported |
| Simplicity SDK 2025.x | 8.x | ❌ Series 2 only |

A stack that runs *on* the chip — the NCP (Network Co-Processor) architecture —
is therefore stuck at EmberZNet 7.5.x / EZSP 13 on these gateways.

## The way around it: move the stack off the chip

```
NCP: the Zigbee stack runs ON the EFR32
RCP: the Zigbee stack runs on a Linux host (x86 PC, Raspberry Pi, ...)
```

In RCP mode the EFR32 only handles the 802.15.4 PHY/MAC layer and speaks the
**CPC** protocol. The Zigbee stack runs on the host as **zigbeed**, which
reaches the radio through the CPC daemon **cpcd**.

The key fact: the RCP firmware built from **Gecko SDK 4.5.0** (CPC protocol v5)
is accepted by **zigbeed from Simplicity SDK 2025.6.3**, which runs
**EmberZNet 8.2.2**. The radio image contains no Zigbee stack at all — only
the 802.15.4 layer — so the Series 1 cut-off, which concerns stacks running on
the chip, never reaches it. This pairing is an observed compatibility, checked
on the Lidl gateway (EZSP 18 network up), not one Silicon Labs documents for
Series 1.

```
 EFR32 (RCP, GSDK 4.5.0)   Gateway (RTL8196E)         Host
+------------------+ UART +-------------------+ TCP  +----------------------------------------+
| 802.15.4 PHY/MAC |<---->| in-kernel         |<---->| cpcd 4.5.3                             |
| CPC protocol v5  |      | UART<->TCP bridge | 8888 | '- zigbeed (EmberZNet 8.2.2)           |
|                  |      |                   |      |      '- TCP 9999 -> Zigbee2MQTT / ZHA  |
+------------------+      +-------------------+      +----------------------------------------+
```

Zigbee2MQTT and ZHA then see an **EZSP 18** coordinator:

```
zh:ember: Adapter version info: {"ezsp":18,"revision":"8.2.2 [GA]",...}
```

## Consequences

- **Updating Zigbee means swapping zigbeed on the host.** The EFR32 image stays
  the same.
- **The host needs resources the chip does not have** (network tables, stack
  RAM): a Raspberry Pi is plenty.
- **The link carries more traffic** than with an NCP: cpcd must reach the
  gateway over a stable, low-latency path (wired Ethernet).
- **The same radio can serve a second protocol.** Because the RCP is
  OpenThread-based and multi-instance, one EFR32 can carry Zigbee (IID 1) and
  Thread (IID 2) at the same time — on the **same channel** only, since Series 1
  lacks Concurrent Listening. See the
  [multi-PAN guide](./docker/cpcd-zigbeed-otbr/README.md).
- **Migrating an existing 7.x host token to 8.x:** delete zigbeed's
  `host_token.nvm` (with rcp-stack:
  `~/.local/state/rcp-stack/zigbeed/host_token.nvm`); the formats differ.
