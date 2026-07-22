# iot-engine Edge Node

`edgenode/` contains the protocol contract and deployable edge-node implementations.

- `proto/`: the shared WebSocket + nanopb wire contract.
- `openwrt/`: the OpenWrt daemon, UCI/procd integration, tests, and `.ipk` recipe.

The platform and the node exchange exactly one `iot.edge.v1.Envelope` in each WebSocket
binary message. Text WebSocket messages and JSON/base64 envelopes are not part of the v1
protocol.

See [`../docs/edge-node.md`](../docs/edge-node.md) for feature parity and multi-platform
routing rules.
