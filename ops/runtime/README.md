# Process deployment

Run three instances of the same artifact. `SERVICE_ROLE` accepts only `api`,
`media`, or `vpn`; it defaults to `api`. There is no combined runtime mode.

| Role | HTTP port | Responsibilities |
|---|---|---|
| api | 1102 | Web, authentication, EdgeNode gateway, acquisition, telemetry consumers, outbox |
| media | 1103 | GB28181 APIs, SIP, media workers and media proxy |
| vpn | 1104 | VPN APIs, WireGuard and firewall reconciliation |

All instances require the same database, Redis, JWT secrets and Edge platform ID.
Keep those in `/etc/iot-engine/shared.env`, readable only by the service users.
Put `HOST=127.0.0.1`, the corresponding `PORT`, and role-specific media/VPN
configuration in `/etc/iot-engine/{api,media,vpn}.env`. Media configuration is
required when running the media role. Select each process using `SERVICE_ROLE`.

Create separate `iot-api`, `iot-media`, and `iot-vpn` system users and writable
`/var/lib/iot-engine/{api,media,vpn}` directories. Install the artifact under
`/opt/iot-engine`, the service template in `/etc/systemd/system`, and the VPN
capability drop-in at the path noted in that file. Set log, recording and other
writable paths inside the corresponding state directory. Enable only the roles
actually used by the installation.

The nginx example keeps the public paths unchanged, including legacy EdgeNode
WebSocket and tokenized firmware downloads.
The HTTP listener is retained for deployed HTTP/WebSocket nodes; do not force
them through an HTTPS redirect. Preserve the installation's existing public
hostname, port and Edge platform ID when switching upstream processes.
Terminate browser TLS with HTTP/2: concurrent SSE subscriptions must not be constrained by HTTP/1.1's browser
connection limit. SSE buffering is disabled and each stream reauthorizes on
changes. Use the public reverse-proxy origin for browser integration testing.

Before starting any new-role process, stop the old combined process and run the
artifact with `--migrate-only`. The channel migration deliberately fails on
duplicate physical endpoints or duplicate device addresses; resolve identified
conflicts explicitly rather than silently changing an installed device address.
Start API first, then media/VPN. Verify each role's `/internal/health/ready` directly and
verify browser subscriptions through the HTTP/2 proxy. These files are templates;
they do not claim a production deployment has been performed.
