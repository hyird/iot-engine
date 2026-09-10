# Desktop VPN server deployment

- Target: `i.a-z.xin`, `103.236.69.112`, `/opt/iot/server`, `iot.service`, port 3000.
- Runtime source: `921d68135ee64ff9ea9b73c32951db9540aa4bc4` on `codex/vpn-desktop-server`.
- Follow-up `d888957` changes only the existing firewall source-contract test to follow the effective-access view.
- CI build: https://github.com/hyird/iot-engine/actions/runs/34428552956
- Previous binary SHA-256: `b3959e866bcd09c957e086ddb9e65090cfac9d9ed18ed256fad8ffdb288f819a`.
- Preflight: readiness 200, desktop route 404, 42 migrations, 6 active VPN peers, `wg` interface and `iot_vpn` firewall present.
- Local validation: rebuilt Debug server, five disposable-database desktop API integration groups passed; VPN CIDR/firewall CTest passed.
- Web CI initially failed one stale SQL text assertion. After correcting it, all 35 local web/contract tests passed (430 assertions). No production source changed for that correction.
- Deployment script backs up the binary, configuration, unit and database; verifies file hashes and all desktop routes; retains additive schema if binary rollback is necessary.
- Only the backend is replaced. Existing web assets and firmware are not included in this deployment.

## Deployed 2026-09-10 10:25 Asia/Taipei

- Linux build and all 26 enabled CTests passed (CI excludes the IPv6 UDP loopback test).
- Archive SHA-256: `70077b246ef194d0457b1a5bb2f46fee557daebb2dc3dab344ed5d3dea3d257c`.
- Running binary SHA-256: `a4992ca0cbc139daed79efb50056c0c148a6333f6f7e856291fb519f1acceb7e`, verified via `/proc/2286131/exe`.
- Backup: `/opt/iot/backups/20260910T102506-921d681-vpn-desktop`; full PostgreSQL custom dump and readable archive contents verified. pg_dump emitted the TimescaleDB continuous_agg circular foreign-key warning; restoration must account for TimescaleDB requirements.
- PID 2286131; NRestarts 0; all readiness components ready.
- Schema migrations increased from 42 to 45: 0041 notifications, 0042 desktop VPN, 0043 nonempty notifications.
- All five desktop routes and the legacy peer GET route return the expected unauthenticated 401 instead of 404.
- Public HTTPS desktop devices route verified: HTTP 401, code 11004, message 未登录.
- Six active VPN peers retained; wg interface, UDP 51820, and refreshed iot_vpn rules verified.
- Environment, configuration, systemd unit and existing web files unchanged by hash.
- No authenticated production enrollment was performed; Windows client login/connection remains a user-side acceptance check.
