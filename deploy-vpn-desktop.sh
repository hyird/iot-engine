#!/usr/bin/env bash
set -Eeuo pipefail
umask 077
root=/opt/iot
slug=921d681-vpn-desktop
expected_old=b3959e866bcd09c957e086ddb9e65090cfac9d9ed18ed256fad8ffdb288f819a
unit=iot.service
set -a
. "$root/.env"
set +a
export PGPASSWORD="$DB_PASSWORD"
db=(psql -X -v ON_ERROR_STOP=1 -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USERNAME" -d "$DB_DATABASE")
if [ "${1:-}" = preflight ]; then
  "${db[@]}" -Atc "SELECT 'migrations=' || count(*) FROM sys_schema_migrations; SELECT migration_id FROM sys_schema_migrations ORDER BY migration_id DESC LIMIT 5; SELECT to_regclass('vpn_effective_edge_access'), to_regclass('vpn_peer_edge_selection'); SELECT 'active_vpn_peers=' || count(*) FROM vpn_peer WHERE status='active';"
  exit
fi
archive_sha=${1:?archive hash}
binary_sha=${2:?binary hash}
archive="$root/incoming-$slug.tar.gz"
release="$root/releases/$slug"
backup="$root/backups/$(date +%Y%m%dT%H%M%S)-$slug"
test "$(sha256sum "$root/server" | cut -d' ' -f1)" = "$expected_old"
test "$(sha256sum "$archive" | cut -d' ' -f1)" = "$archive_sha"
test ! -e "$release"
mkdir -p "$release" "$backup"
tar -xzf "$archive" -C "$release"
test "$(sha256sum "$release/iot-engine" | cut -d' ' -f1)" = "$binary_sha"
cp -a "$root/server" "$root/.env" "$root/config" /etc/systemd/system/iot.service "$backup/"
pg_dump -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USERNAME" -d "$DB_DATABASE" --format=custom > "$backup/database.dump"
test -s "$backup/database.dump"
pg_restore --list "$backup/database.dump" > "$backup/database.contents"
nft list table inet iot_vpn > "$backup/vpn-before.nft"
ip link show wg > "$backup/wg-before.txt"
sha256sum "$root/.env" /etc/systemd/system/iot.service > "$backup/unchanged.sha256"
find "$root/config" "$root/web" -type f -exec sha256sum {} + >> "$backup/unchanged.sha256"
"${db[@]}" -Atc "SELECT count(*) FROM sys_schema_migrations" > "$backup/migrations-before.txt"
install -m 0755 "$release/iot-engine" "$root/server.$slug.new"
rollback() {
  result=$?
  trap - ERR
  set +e
  systemctl stop "$unit"
  install -m 0755 "$backup/server" "$root/server.$slug.rollback"
  mv -f "$root/server.$slug.rollback" "$root/server"
  systemctl start "$unit"
  echo "ROLLED_BACK backup=$backup status=$result; additive schema retained"
  exit "$result"
}
trap rollback ERR
systemctl stop "$unit"
mv -f "$root/server.$slug.new" "$root/server"
systemctl start "$unit"
ready=0
for attempt in $(seq 1 60); do
  code=$(curl --max-time 3 -s -o "$backup/ready.json" -w '%{http_code}' http://127.0.0.1:3000/internal/health/ready || true)
  if [ "$code" = 200 ]; then ready=1; break; fi
  sleep 1
done
test "$ready" = 1
for path in /v1/vpn/desktop/devices /v1/vpn/desktop/peers/11111111-1111-4111-8111-111111111111/config /v1/vpn/peers; do
  test "$(curl --max-time 5 -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:3000$path")" = 401
done
for method in POST PATCH DELETE; do
  path=/v1/vpn/desktop/peers
  if [ "$method" != POST ]; then path="$path/11111111-1111-4111-8111-111111111111"; fi
  test "$(curl --max-time 5 -s -X "$method" -o /dev/null -w '%{http_code}' "http://127.0.0.1:3000$path")" = 401
done
sha256sum --check --status "$backup/unchanged.sha256"
test "$(sha256sum "$root/server" | cut -d' ' -f1)" = "$binary_sha"
pid=$(systemctl show "$unit" -p MainPID --value)
test "$(sha256sum "/proc/$pid/exe" | cut -d' ' -f1)" = "$binary_sha"
systemctl is-active --quiet "$unit"
test "$(systemctl show "$unit" -p NRestarts --value)" = 0
ip link show wg > "$backup/wg-after.txt"
nft list table inet iot_vpn > "$backup/vpn-after.nft"
ss -H -lun | grep -Eq '[:.]51820[[:space:]]'
"${db[@]}" -Atc "SELECT count(*) FROM sys_schema_migrations; SELECT to_regclass('vpn_effective_edge_access'), to_regclass('vpn_peer_edge_selection');" > "$backup/schema-after.txt"
trap - ERR
echo "DEPLOYED $slug PID=$pid BACKUP=$backup SHA256=$binary_sha"
cat "$backup/schema-after.txt"
