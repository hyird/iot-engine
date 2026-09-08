# Unified architecture cutover

This is one release, not independently deployable partial migrations. Existing
uncommitted work is the starting point. Do not deploy until every gate below passes.

## Required outcomes

- Physical channels own transport parameters, sessions and scheduling. Devices
  reference channels and have protocol addresses unique within their channel.
- Published point models are immutable. Telemetry and commands reference the
  model revision used to interpret or execute them.
- Telemetry has explicit value types, sample/receive times, quality and media kind.
- Persistence, latest values, alerts and external delivery have independent
  consumer progress and retry boundaries.
- Business reads are SSE-only: initial snapshot followed by event-driven complete
  replacement snapshots. No JSON GET alternative and no client polling fallback.
- Subscriptions reauthorize before publishing data, close on revocation/expiry,
  reconnect with a fresh snapshot and release resources when unobserved.
- Binary firmware/media, WebSocket upgrades, static assets and operational probes
  retain their transport protocols. They are not business JSON query alternatives.
- Deployed EdgeNode compatibility is confined to the ingress/egress adapter until
  the repository's tested migration and compatibility-window requirements are met.

## Implementation gates

- [ ] Durable cross-instance query change events and bounded local fanout.
- [x] All business read controllers use the SSE snapshot contract.
- [x] All web query consumers own live subscription lifetimes; old GET clients,
      notification-only SSE and refetch timers are removed.
- [ ] Channel/device model, UI and migration complete.
- [ ] Immutable point-model publication and historical interpretation complete.
- [ ] Typed telemetry and independent consumers complete.
- [ ] Video/VPN runtime deployment boundaries complete.
- [ ] Build, typecheck and existing tests pass.
- [ ] Isolated integration: initial snapshot, external mutation, runtime update,
      reconnect, permission revocation, slow reader, removal, and old-route rejection.
- [ ] Migration validates address collisions, model bindings and preserved history.

Historical configurations that were never retained cannot be reconstructed. A
migrated baseline must be identified as such, not represented as a historical fact.

## Verification update: 2026-09-07

Release compilation and frontend type checking pass with Ruvia pinned to
`50a8ed832f723574761433346a5a956ecf47d683`. The C++ suite reports 25/26 passing;
GB28181 IPv6 UDP catalog delivery fails on this Windows host, where a separate
plain IPv6 UDP loopback check also times out. This gate remains open.

Disposable PostgreSQL/Redis integration passes for immutable revisions, shared
serial channels and address uniqueness, guarded transport changes, independent
latest/alert progress during a history-table lock, history recovery, atomic
fanout retry, command idempotency and ambiguous timeout handling. API/media/VPN
processes each pass readiness and reject routes belonging to other roles.
SSE integration passes initial snapshots, external commits, reconnect, deletion,
permission revocation, and rejection of old JSON/query-notification paths.
Protocol 2/5/6 WebSocket application liveness and legacy protobuf compatibility
checks pass in the isolated fixture. Selected frontend tests report 23/23.

Production cutover is not performed. Remaining release gates include public
HTTP/2 proxy/browser verification, slow-reader and multiple-API-instance tests,
Linux media/VPN runtime verification, and resolving the IPv6 test environment.
Do not interpret local readiness as a complete production acceptance test.
