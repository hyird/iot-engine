# Process deployment

Run one native `iot-engine` process. It owns the API, EdgeNode gateway,
GB28181/SIP/media runtime, and VPN control/data-plane integration in the same
process and listens on `127.0.0.1:1102`. `GB28181_ENABLED` controls whether the
GB28181 and media runtime starts (default `false`). `VPN_HUB_ENABLED` controls
the VPN runtime (default `true`); it does not require a second process or port.

All settings belong in `/etc/iot-engine/iot-engine.env`, including the shared
database, Redis, JWT, Edge platform, media, and VPN settings. Keep the file
readable only by the `iot-engine` service user. Create that user and the
writable `/var/lib/iot-engine` state directory. Install the artifact under
`/opt/iot-engine` and `iot-engine.service` under `/etc/systemd/system`.
The unit grants the network capabilities required by the VPN integration.

The nginx example sends every public path to the single 1102 listener while
keeping the public paths unchanged, including legacy EdgeNode WebSocket and
tokenized firmware downloads.
The HTTP listener is retained for deployed HTTP/WebSocket nodes; do not force
them through an HTTPS redirect. Preserve the installation's existing public
hostname, port and Edge platform ID when switching upstream processes.
Terminate browser TLS with HTTP/2: concurrent SSE subscriptions must not be constrained by HTTP/1.1's browser
connection limit. SSE buffering is disabled and each stream reauthorizes on
changes. Use the public reverse-proxy origin for browser integration testing.

Stop the previous deployment before starting the unified unit and run the
artifact with `--migrate-only`. The channel migration deliberately fails on
duplicate physical endpoints or duplicate device addresses; resolve identified
conflicts explicitly rather than silently changing an installed device address.
Verify `/internal/health/ready` on port 1102 directly and verify browser
subscriptions through the HTTP/2 proxy. These files are templates; they do not
claim a production deployment has been performed.
