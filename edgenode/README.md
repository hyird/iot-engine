# iot-engine Edge Node

`edgenode/` contains the protocol contract and deployable edge-node implementations.

- `openwrt/proto/`: the shared WebSocket + nanopb wire contract used by both platform and node.
- `openwrt/`: the self-contained OpenWrt daemon, UCI/procd integration, build-time
  nanopb generation, tests, and `.ipk` recipe.

The platform and the node exchange exactly one `iot.edge.v1.Envelope` in each WebSocket
binary message. Text WebSocket messages and JSON/base64 envelopes are not part of the v1
protocol.

See [`../docs/edge-node.md`](../docs/edge-node.md) for feature parity and multi-platform
routing rules.
