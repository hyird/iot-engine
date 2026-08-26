# Transactional Outbox runbook

The service reports Outbox state through `/internal/health/ready` and
`/internal/metrics`. An active Outbox alert changes health status to `degraded` but does
not fail readiness, because restarting healthy instances would normally increase the
backlog.

## First checks

1. Check `iot_engine_outbox_pending`, `iot_engine_outbox_oldest_age_ms`,
   `iot_engine_outbox_dead_lettered`, and
   `increase(iot_engine_outbox_dispatch_failures_total[5m])`.
2. Check PostgreSQL and Redis connectivity from the service environment.
3. Search service logs for `outbox dispatch failed`, `outbox receipt cleanup failed`, or
   `operational alert active`.
4. Do not replay a dead letter until the underlying Redis, schema, or contract problem is
   resolved.

## Dead-lettered events

Use a superadmin account or an account with `system:outbox:manage` permission.

```text
GET /v1/system/outbox/dead-letters
Authorization: Bearer <access-token>
```

Inspect `last_error`, `event_type`, `aggregate_type`, and `aggregate_id`. After fixing the
cause, requeue one event at a time:

```text
POST /v1/system/outbox/dead-letters/<event-id>/replay
Authorization: Bearer <access-token>
```

Success resets the retry state and makes the event immediately available. Repeating the
request after a successful requeue returns `404`, so operator retries are safe. Confirm the
dead-letter gauge falls and the corresponding alert clears within the next collection
interval.

## Pending backlog

- If dispatcher failures are rising, fix PostgreSQL or Redis connectivity first.
- If failures are flat but pending count grows, check service worker saturation and Redis
  latency.
- Compare `iot_engine_outbox_last_batch_size` with the arrival rate. Sustained batches of
  100 indicate the dispatcher is continuously at its batch limit.

## Old events

Inspect the oldest unpublished rows and their retry state:

```sql
SELECT id, event_type, aggregate_type, aggregate_id, attempts, available_at, last_error
FROM outbox_event
WHERE published_at IS NULL AND dead_lettered_at IS NULL
ORDER BY occurred_at, id
LIMIT 20;
```

A repeatedly failing event is retried with exponential backoff and eventually becomes a
dead letter. Resolve the reported error before replaying it.

## Dispatcher failures

Verify both dependencies independently. A Redis publish failure leaves the PostgreSQL row
unpublished and eligible for retry. Consumers use `(consumer_name, event_id)` receipts and
perform their idempotent projection before recording the receipt and acknowledging the
Redis entry.

## Metrics missing

Confirm `/internal/health/live` responds, then check the Prometheus scrape target and
`/internal/metrics`. If the process is live but Outbox metrics remain absent for more than
one collection interval, inspect `iot_engine_metrics_collection_failures_total` and the
service logs.

## Recovery verification

Recovery is complete when all of the following are true:

- `iot_engine_outbox_dead_lettered` is zero or contains only intentionally quarantined
  events.
- `iot_engine_outbox_pending` and `iot_engine_outbox_oldest_age_ms` are below their configured
  thresholds.
- All `iot_engine_alert_active{alert="outbox_*"}` series are zero.
- Runtime configuration and webhook catalogs reflect the replayed change.
