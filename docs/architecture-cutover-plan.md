# Unified architecture cutover

The SSE cutover is already deployed. This update finishes event-driven dispatch
and its isolated verification; deployment status is recorded separately below.

## Required outcomes

- Physical channels own transport parameters, sessions and scheduling. Devices
  reference channels and have protocol addresses unique within their channel.
- Published point models are immutable. Telemetry and commands reference the
  model revision used to interpret or execute them.
- Telemetry has explicit value types, sample and receive times, quality and
  media kind.
- Persistence, latest values, alerts and external delivery have independent
  consumer progress and retry boundaries.
- Business reads are SSE-only: an initial snapshot is followed by complete,
  event-driven replacement snapshots. There is no JSON GET alternative or
  client polling fallback.
- Subscriptions reauthorize before publishing data, close on revocation or
  expiry, reconnect with a fresh snapshot and release resources when
  unobserved.
- Binary firmware and media, WebSocket upgrades, static assets and operational
  probes retain their transport protocols.
- Deployed EdgeNode compatibility remains at the ingress and egress adapter
  boundary. Protocol versions, legacy task messages and legacy token downloads
  remain supported.

## Implementation status

- [x] Durable cross-instance query change events with bounded local fanout.
- [x] All business read controllers use the SSE snapshot contract.
- [x] Web query consumers own live subscription lifetimes; notification-only
      SSE and query refetch timers are removed.
- [x] Configuration events use committed PostgreSQL LISTEN notifications,
      durable pending changes and bounded deadline recovery.
- [x] Collector configuration and lease handling cover all three collectors
      across two service instances. Lease renewal remains timer-driven and
      configuration events remain event-driven.
- [x] Channel/device model, UI and migration are implemented.
- [x] Immutable point-model publication and historical interpretation are
      implemented.
- [x] Typed telemetry and independent consumers are implemented.
- [x] Video and VPN runtime boundaries are implemented.
- [x] Windows release build, frontend checks and isolated integration pass;
      the native test exclusion is recorded below.

## Verification

The current isolated verification passes cross-instance external SQL changes,
SSE reconnects, dual API behavior, slow paused readers with 4 MiB snapshots,
outbox idle, rollback, future-dated, locked-row and listener-reconnect cases,
configuration delivery to all three collectors across two instances, and
bounded recovery after a trimmed wake hint. Architecture checks cover telemetry
and EdgeNode protocol versions 2, 5 and 6. Frontend checks, lint, build and
eight frontend tests pass, with 328 assertions. Command expiry also passes with
a future deadline and no subsequent mutation or result notification.

The native Windows suite excludes the known GB28181 SIP IPv6 environment
failure. This is an environment limitation, not a claim of hardware or Linux
runtime validation. Both Windows test processes stopped after PTY Ctrl-C with
exit code 1; graceful shutdown has not been established by that result.

Historical configurations that were never retained cannot be reconstructed. A
migrated baseline must be identified as such, not represented as a historical
fact.

## Deployment boundary

The production service is still at `1109161`; the earlier Ruvia `83292260`
deployment remains the production baseline. The current cutover changes in
this checkout have not been deployed. No production, hardware, cellular
traffic or field EdgeNode validation is claimed here.
