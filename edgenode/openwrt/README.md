# edgenode for OpenWrt

This directory contains a small C daemon and an OpenWrt package recipe. It has no C++
runtime, full protobuf runtime, or database dependency.

Implemented foundations:

- the node registers independently with up to four platforms using its 15-digit IMEI;
- an HTTP/HTTPS platform base address is upgraded internally to a binary WebSocket
  session carrying one nanopb `Envelope` per message;
- certificate and hostname validation are intentionally disabled for this deployment;
- every platform has isolated registration, config, reconnect, heartbeat, and outbox state;
- config and outbox files are raw nanopb messages under
  `/tmp/edgenode/<platform_id>/`; process restarts recover them, device reboots do not;
- before every tmpfs write, the daemon preserves 15% free space by rolling the oldest
  outbox message across all platforms; active and staging config are never rolled;
- the DTU runtime reads every second, processes queued writes in that same cadence, and
  reports independently at the configured interval;
- Modbus TCP/RTU and S7 request/response codecs implement reads and writes. A successful
  command requires readback equality;
- an unresponsive S7 PLC closes the TCP socket and repeats TCP, COTP, and S7 Setup
  Communication on the next one-second cycle.
- network and serial capabilities are reported automatically; when `ttyd` is installed,
  the platform exposes an authenticated remote terminal;
- bootstrap-authorized commands can update `br-lan` through UCI, manage additional
  HTTP/HTTPS platforms, download verified firmware, and invoke `sysupgrade`.

The active-config-to-physical-endpoint binding is kept separate from the wire/session
layer. The current code provides the tested protocol codecs and scheduler that binding
uses; actual target hardware is still required before declaring a target deployable.

## Configure

The bootstrap platform base address is compiled as `https://i.a-z.xin` and cannot be
overridden through UCI. The daemon derives the internal upgrade path
`/edge/v1/connect`. The default IMEI and model are empty: the init service reads IMEI
from the modem and model from `/tmp/sysinfo/model` before starting the platform client.
To override either value explicitly, use UCI and restart the service:

```sh
uci set edgenode.node.imei='your-15-digit-imei'
uci set edgenode.node.model='your-model'
uci commit edgenode
/etc/init.d/edgenode restart
```

If enrollment requires a token, store it at
`/etc/edgenode/credentials/bootstrap.token` with mode `0600`. Additional platform
sections are created or removed only by authenticated commands from the bootstrap
platform; the node applies those settings through `uci set/delete` and `uci commit`.

## Build an IPK

`edgenode/openwrt` is a complete OpenWrt package directory. Copy or link this whole
directory into the selected SDK as `package/edgenode`; do not copy only its `Makefile`:

```sh
ln -s /path/to/iot-engine/edgenode/openwrt /path/to/openwrt/package/edgenode
make menuconfig
make package/edgenode/compile V=s
```

The package is self-contained apart from dependencies fetched by the OpenWrt build
system: `Makefile`, `files/`, `proto/`, and `src/` stay together.
`proto/edge.proto` is the single wire-contract source used by the platform and node;
the OpenWrt SDK generates nanopb C sources in its build directory before compiling.
The recipe downloads nanopb `0.4.9.1`, compiles only its three C runtime files, enables
`-Os`, LTO, function sections, and linker garbage collection, and dynamically uses
OpenWrt's mbedTLS-backed libuwsc.

The package, daemon, init service, UCI configuration, and runtime paths are all named
`edgenode`.

Generated nanopb C files are deliberately not committed. The OpenWrt build host needs
the recipe's Python/protobuf host dependencies and generates `edge.pb.c` and `edge.pb.h`
from `proto/edge.proto` in `PKG_BUILD_DIR` on every relevant build.

Hardware paths, interface names, bridge mode, modem USB ID, AT port, status path, and
monitor interval are UCI settings rather than compiled constants. The TAS-682 package
defaults describe its single field serial port and Ethernet port plus the LierdaComm
modem. The service initializes the model from `/tmp/sysinfo/model`, initializes IMEI and
ICCID from the modem, and keeps registration and signal status in tmpfs without
repeatedly writing flash.

The resulting package must be cross-compiled and installed on the actual target. A host
binary is not an OpenWrt deliverable.
