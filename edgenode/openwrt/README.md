# edgenode for OpenWrt

This directory contains a small C daemon and an OpenWrt package recipe. It has no C++
runtime, full protobuf runtime, or database dependency.

Implemented foundations:

- the node registers independently with up to four platforms using its 15-digit IMEI;
- WSS carries one nanopb `Envelope` per binary WebSocket message;
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

The active-config-to-physical-endpoint binding is kept separate from the wire/session
layer. The current code provides the tested protocol codecs and scheduler that binding
uses; actual target hardware is still required before declaring a target deployable.

## Configure

Install the package, then edit `/etc/config/edgenode`:

```text
config node 'node'
        option imei '490154203237518'
        option model 'OpenWrt Edge'

config platform 'primary'
        option enabled '1'
        option id '018f6f5e-93d8-7d31-9f70-123456789abc'
        option url 'wss://platform.example/edge/v1/connect'
        option enrollment_token_file '/etc/edgenode/credentials/primary.token'
        option network_owner '1'
        option outbox_max_bytes '262144'
```

The enrollment token file must be readable only by its owner (`0600`). Add another
`platform` section with a different UUID for each additional platform. Only one enabled
platform may set `network_owner=1`.

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

The generated nanopb C files are committed under `generated/`. OpenWrt 18.06 therefore
does not need a host Python, protobuf, or `protoc` package to build `edgenode`; changes to
`proto/edge.proto` must regenerate and commit `generated/edge.pb.c` and `edge.pb.h` with
nanopb 0.4.9.1.

Hardware paths, interface names, bridge mode, modem USB ID, AT port, status path, and
monitor interval are UCI settings rather than compiled constants. The TAS-682 package
defaults describe its single field serial port and Ethernet port plus the LierdaComm
modem. The service initializes IMEI and ICCID once in UCI, and keeps registration and
signal status in tmpfs without repeatedly writing flash.

The resulting package must be cross-compiled and installed on the actual target. A host
binary is not an OpenWrt deliverable.
