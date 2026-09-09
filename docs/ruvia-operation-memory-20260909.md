# Ruvia operation memory update — 2026-09-09

Ruvia is pinned to `d7fa8d6dd14be4de204172de9bca0b76f7a3643f`, replacing
`50a8ed832f723574761433346a5a956ecf47d683`.

## Problem and change

A WebSocket handler retains its HTTP request Context for the lifetime of the
connection. Previously, `Context::redis()`, `db()` and `httpClient()` allocated
operation parameters/results from that request's monotonic arena. Destruction
of individual temporary objects did not reclaim arena space. Repeated operations
could therefore grow memory until the connection ended.

The new Ruvia revision routes these clients through `operationResource()`, the
worker resource that supports individual deallocation. The request/handshake
resource remains available for objects that actually need request lifetime.

IoT also uses the operation resource explicitly for its two EdgeNode WebSocket
task scopes and JWT signing/verification temporaries. Protocol messages,
firmware transport, endpoint paths and authentication contracts are unchanged.

## Production evidence collected before this update

Read-only allocator metadata inspection found 13 monotonic arenas with internally
consistent block chains, totaling 266,958,336 bytes (254.6 MiB) of reserved blocks.
The largest held 61,279,808 bytes. These are allocator block sizes, not a separate
RSS measurement. The same objects continued consuming space between snapshots.

A separate 30-second malloc/calloc/realloc/free sample observed roughly 11.6 MiB
of allocation requests and 108 KiB of newly allocated storage still outstanding
at the end. Such a sample cannot account for allocations made before attachment,
or for suballocations inside previously allocated arenas. It therefore does not
contradict the arena accumulation finding.

No production process was restarted or replaced to obtain this evidence.

## Regression coverage and limits

`context-operation-memory-test` keeps one Context alive while constructing 2,000
rounds of Redis and database operations and checks that request arena position
does not advance due to their parameters. It also checks that temporary result
storage is individually deallocated without invalidating a retained result.
These are isolated allocator tests; they do not execute network queries.

Local Release compilation, frontend lint/type checking, and ten SSE/resource
frontend tests pass. CTest reports 26/27 passing, including the new memory test;
the pre-existing GB28181 IPv6 UDP loopback failure remains on this Windows host.
Disposable PostgreSQL/Redis integration passes SSE updates/reconnect/revocation,
command idempotency and result handling, and telemetry recovery/backpressure.
The 65-second real WebSocket test passes for legacy protocol versions 2, 5 and 6:
responsive sessions renew their leases and a silent application session expires.

Explicit DTO allocation from `Context::resource()` in repeated SSE snapshots is
not automatically changed by the dependency update. Those allocations remain
bounded in lifetime by the existing five-minute subscription renewal policy.
This update is not a claim that every request-arena allocation or every source
of process RSS has been eliminated. Production memory reduction requires a
separately deployed build and measurement under comparable traffic.
