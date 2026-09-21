# Experimental multi-PAN Zigbee + Thread host

The Lidl gateway's Series 1 EFR32MG1B can run Zigbee and Thread concurrently
through this multi-PAN RCP configuration **when both networks use the same
802.15.4 channel**. It cannot run them on independent channels. Independent-
channel Zigbee + Thread uses Silicon Labs Concurrent Listening and requires a
supported Series 2 radio.

This distinction is documented by Silicon Labs in
[Concurrent Multiprotocol: An In-Depth Exploration](https://www.silabs.com/blog/concurrent-multiprotocol-an-in-depth-exploration):
same-channel Zigbee + Thread is supported for Series 1 RCP designs, whereas
separate-channel Concurrent Listening is a Series 2 capability. The firmware
layout follows Silicon Labs' [OpenThread multi-instance
configuration](https://docs.silabs.com/openthread/latest/openthread-multi-instance/).

## Instance allocation

The allocation is deliberately explicit at every layer:

| IID | Consumer | Configuration |
|---:|---|---|
| 0 | CPC broadcast | `OPENTHREAD_SPINEL_CONFIG_BROADCAST_IID=0`, `iid-list=0` |
| 1 | `zigbeed` | `spinel+cpc://cpcd_0?iid=1&iid-list=0` |
| 2 | `otbr-agent` | `spinel+cpc://cpcd_0?iid=2&iid-list=0` |

The RCP project enables multi-PAN, enables static multiple instances, and sets
`OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_NUM=2`. In the generated RCP, the two
data instances therefore occupy IIDs 1 and 2 while IID 0 remains broadcast.

The host OpenThread build must also set `OT_MULTIPAN_RCP=ON`. Without that
host-side option, OpenThread rejects `iid=2&iid-list=0` in
`GetIidListFromUrl()` before it queries the RCP. That parser error does not
show that the radio lacks IID 2.

## Reproduce the tested topology

1. Build and flash the RCP after generating it from the updated `.slcp`:

   ```bash
   cd 2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW
   ./build_rcp.sh 460800
   cd ../..
   ./flash_efr32.sh -y -g <gateway-ip> rcp
   ```

2. Set the gateway address and physical Linux LAN interface. For example:

   ```bash
   export RCP_HOST=192.168.1.88
   export OTBR_INFRA_INTERFACE=eth0
   ```

   Find the interface carrying the default route with `ip route show default`.
   Do not use a Docker bridge such as `docker0`. Leave the IID allocation in
   the Compose file unchanged.

3. Prepare the native Linux host for border routing. This is the same host
   setup required by OTBR's own Docker deployment:

   ```bash
   test -c /dev/net/tun
   sudo sysctl -w net.ipv6.conf.all.forwarding=1
   sudo sysctl -w net.ipv4.ip_forward=1
   sudo sysctl -w net.ipv6.conf.${OTBR_INFRA_INTERFACE}.accept_ra=2
   sudo sysctl -w net.ipv6.conf.${OTBR_INFRA_INTERFACE}.accept_ra_rt_info_max_plen=64
   ```

   Make those sysctls persistent using your distribution's normal
   `/etc/sysctl.d/` configuration before relying on the border router after a
   reboot. The Docker engine must be rootful: this stack uses host networking,
   `/dev/net/tun`, `NET_ADMIN`, `NET_RAW`, ipsets, and host firewall rules.

4. Build and start the stack from the `docker` directory:

   ```bash
   docker compose -f docker-compose-multipan.yml pull otbr-agent
   docker compose -f docker-compose-multipan.yml up -d
   docker compose -f docker-compose-multipan.yml logs -f cpcd-zigbeed otbr-agent
   ```

   The Compose default pins the published multi-architecture image to release
   `4.5.1`. To reproduce it locally from the shipped sources instead:

   ```bash
   docker compose -f docker-compose-multipan.yml build otbr-agent
   ```

The `ghcr.io/jnilo1/multipan-otbr:4.5.1` image builds the OpenThread and OTBR
sources vendored by GSDK 4.5.0, adds the Silicon Labs CPC vendor transport, and
passes `OT_MULTIPAN_RCP=ON`.
The containers share only cpcd's Unix-socket directory. `cpcd` itself runs
once, in the `cpcd-zigbeed` container. The Compose default pins that image to
the tested `cpcd4.5.3-ezsp18` tag rather than following `latest`.

OTBR uses host networking because Thread border routing needs real IPv6 router
advertisements and mDNS on the LAN. On native Linux this places `wpan0`, OTBR's
IPv6 routes, mDNS, and its iptables/ipset rules in the host network namespace.
Stop the stack cleanly with `docker compose down` so the entrypoint can remove
its OTBR firewall chain. Docker Desktop's host network is still a private VM on
some platforms; native Linux Docker is the supported deployment for this
compose file.

## Channel requirement and present validation limit

Create or attach the Thread network on the **same channel as Zigbee**. This is
not optional on Series 1. Do not interpret a brief successful test as support
for separate channels.

The present hardware validation is intentionally narrow:

- Lidl Silvercrest gateway, EFR32MG1B, GSDK 4.5.0/CPC v5 at 460800 RTS/CTS;
- tested RCP GBL SHA-256
  `9a5bce1502a6bb2143aad303b10a403f435500c28e373f1f684a55773da967ff`;
- `cpcd` 4.5.3, `zigbeed` EmberZNet 8.2.2/EZSP 18 on IID 1;
- OTBR/OpenThread Thread 1.4 on IID 2;
- a Thread network reached leader state on channel 11 while a Hue Zigbee lamp
  on channel 11 was switched off and on through Zigbee2MQTT;
- both protocol stacks remained attached during this short test.

Long mixed-traffic soak testing and commissioning/controlling a real Thread
device have **not** yet been completed. Treat this path as experimental until
those tests exist. For a production single-protocol Thread deployment, the
standalone [`26-OT-RCP`](../../../26-OT-RCP/README.md) path remains the safer
choice.
