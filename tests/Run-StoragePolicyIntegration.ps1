[CmdletBinding()]
param([string]$PostgresBin = 'C:/Program Files/PostgreSQL/18/bin')
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $repository ('build/storage-policy-fixture-' + [Guid]::NewGuid().ToString('N'))
$data = Join-Path $fixture 'postgres'
$backend = Join-Path $repository 'build/Release/iot-engine.exe'
$psql = Join-Path $PostgresBin 'psql.exe'
$pgctl = Join-Path $PostgresBin 'pg_ctl.exe'
$port = 55479
foreach ($file in @($backend, $psql, $pgctl, (Join-Path $PostgresBin 'initdb.exe'))) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing dependency: $file" }
}
$probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $port)
try { $probe.Start() } finally { $probe.Stop() }
New-Item -ItemType Directory -Path $fixture | Out-Null
$envFile = Join-Path $fixture '.env'
$oldPath = $env:Path
$migrationProcess = $null
$fixtureEnvironment = @(
    'DB_HOST', 'DB_PORT', 'DB_USERNAME', 'DB_PASSWORD', 'DB_DATABASE',
    'GB28181_ENABLED', 'VPN_HUB_ENABLED', 'EDGE_PUBLIC_BASE_URL', 'EDGE_PLATFORM_ID',
    'DEVICE_DATA_COMPRESSION_POLICY_ENABLED', 'DEVICE_DATA_CHUNK_INTERVAL_HOURS',
    'DEVICE_DATA_COMPRESSION_AFTER_HOURS', 'DEVICE_DATA_MUTABLE_WINDOW_HOURS'
)
$savedEnvironment = @{}
foreach ($name in $fixtureEnvironment) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Invoke-Sql([string]$Sql) {
    $sqlFile = Join-Path $fixture 'query.sql'
    Set-Content -LiteralPath $sqlFile -Value $Sql -Encoding utf8
    $result = & $psql -X -q -A -t -v ON_ERROR_STOP=1 -h 127.0.0.1 -p $port -U storage_test -d iot_storage -f $sqlFile
    if ($LASTEXITCODE -ne 0) { throw "Fixture SQL failed; see $fixture" }
    return ($result -join "`n").Trim()
}
function Assert-Sql([string]$Sql, [string]$Message) {
    if ((Invoke-Sql $Sql) -ne 't') { throw $Message }
}
function Set-Policy([bool]$Enabled, [int]$ChunkHours = 168) {
    @"
DB_HOST=127.0.0.1
DB_PORT=$port
DB_USERNAME=storage_test
DB_PASSWORD=
DB_DATABASE=iot_storage
GB28181_ENABLED=false
VPN_HUB_ENABLED=false
EDGE_PUBLIC_BASE_URL=http://127.0.0.1:55179
EDGE_PLATFORM_ID=00000000-0000-7000-8000-000000000001
DEVICE_DATA_COMPRESSION_POLICY_ENABLED=$($Enabled.ToString().ToLowerInvariant())
DEVICE_DATA_CHUNK_INTERVAL_HOURS=$ChunkHours
DEVICE_DATA_COMPRESSION_AFTER_HOURS=168
DEVICE_DATA_MUTABLE_WINDOW_HOURS=48
"@ | Set-Content -LiteralPath $envFile -Encoding ascii
}
function Invoke-Migration([string]$Name, [bool]$ExpectedSuccess = $true) {
    $script:migrationProcess = Start-Process -FilePath (Join-Path $fixture 'iot-engine.exe') -ArgumentList '--migrate-only' -WorkingDirectory $fixture -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $fixture "$Name.out") -RedirectStandardError (Join-Path $fixture "$Name.err")
    if (!$migrationProcess.WaitForExit(60000)) { throw "Migration timed out: $Name" }
    if (($migrationProcess.ExitCode -eq 0) -ne $ExpectedSuccess) { throw "Unexpected migration exit for $Name; see $fixture" }
}
function Assert-Policy([bool]$Enabled, [int]$ChunkHours = 168) {
    $mode = if ($Enabled) { 'on' } else { 'off' }
    $expectedId = "runtime_device_data_storage_policy_v2_${mode}_chunk_${ChunkHours}_after_168_mutable_48"
    Assert-Sql "SELECT count(*) = 1 AND min(migration_id) = '$expectedId' FROM sys_schema_migrations WHERE starts_with(migration_id,'runtime_device_data_storage_policy_');" 'Unexpected policy migration records'
    $jobCount = if ($Enabled) { 1 } else { 0 }
    Assert-Sql "SELECT count(*) = $jobCount FROM timescaledb_information.jobs WHERE hypertable_name='device_data' AND proc_name='policy_compression';" 'Unexpected compression policy jobs'
    Assert-Sql "SELECT time_interval = make_interval(hours => $ChunkHours) FROM timescaledb_information.dimensions WHERE hypertable_name='device_data' AND column_name='report_time';" 'Unexpected chunk interval'
    if ($Enabled) {
        Assert-Sql "SELECT (config->>'compress_after')::interval = interval '168 hours' FROM timescaledb_information.jobs WHERE hypertable_name='device_data' AND proc_name='policy_compression';" 'Unexpected compression age'
    }
}
try {
    # loadDotenv preserves existing process values; force migration inputs to the fixture.
    foreach ($name in $fixtureEnvironment) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    & (Join-Path $PostgresBin 'initdb.exe') -D $data -U storage_test --auth=trust -E UTF8 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'initdb failed' }
    Add-Content -LiteralPath (Join-Path $data 'postgresql.conf') -Value "shared_preload_libraries = 'timescaledb'"
    $arguments = '-D "{0}" -l "{1}" -o "-p {2} -h 127.0.0.1" -w start' -f $data,(Join-Path $fixture 'postgres.log'),$port
    $launcher = Start-Process -FilePath $pgctl -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (!$launcher.WaitForExit(30000) -or $launcher.ExitCode -ne 0) { throw 'PostgreSQL startup failed' }
    & (Join-Path $PostgresBin 'createdb.exe') -h 127.0.0.1 -p $port -U storage_test iot_storage
    if ($LASTEXITCODE -ne 0) { throw 'createdb failed' }
    Copy-Item -LiteralPath $backend -Destination (Join-Path $fixture 'iot-engine.exe')
    $env:Path = (Join-Path $repository 'build/Release') + ';' + $oldPath
    Set-Policy $true
    Invoke-Migration 'initial'
    Assert-Policy $true
    $before = Invoke-Sql "SELECT checksum || ':' || applied_at::text FROM sys_schema_migrations WHERE starts_with(migration_id,'runtime_device_data_storage_policy_');"
    Invoke-Migration 'repeat'
    if ($before -ne (Invoke-Sql "SELECT checksum || ':' || applied_at::text FROM sys_schema_migrations WHERE starts_with(migration_id,'runtime_device_data_storage_policy_');")) { throw 'Repeated migration was not skipped' }
    Write-Output 'PASS fresh schema and repeated migration'

    Invoke-Sql "UPDATE sys_schema_migrations SET migration_id=replace(migration_id,'_v2_','_v1_'), checksum=repeat('0',64) WHERE starts_with(migration_id,'runtime_device_data_storage_policy_');" | Out-Null
    Invoke-Migration 'upgrade-v1'
    Assert-Policy $true
    Write-Output 'PASS existing v1 record upgrades without checksum collision'

    Set-Policy $false 24
    Invoke-Migration 'disable'
    Assert-Policy $false 24
    Set-Policy $true
    Invoke-Migration 'reenable'
    Assert-Policy $true
    Write-Output 'PASS on/off/on restores compression and chunk settings'

    $before = Invoke-Sql "SELECT checksum || ':' || applied_at::text FROM sys_schema_migrations WHERE starts_with(migration_id,'runtime_device_data_storage_policy_');"
    $jobBefore = Invoke-Sql "SELECT job_id FROM timescaledb_information.jobs WHERE hypertable_name='device_data' AND proc_name='policy_compression';"
    Invoke-Sql @'
CREATE FUNCTION reject_storage_policy() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF starts_with(NEW.migration_id,'runtime_device_data_storage_policy_v2_off_') THEN
    RAISE EXCEPTION 'storage policy test injected failure';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER reject_storage_policy BEFORE INSERT ON sys_schema_migrations
FOR EACH ROW EXECUTE FUNCTION reject_storage_policy();
'@ | Out-Null
    Set-Policy $false 24
    Invoke-Migration 'rollback' $false
    if (!(Select-String -LiteralPath (Join-Path $fixture 'rollback.err') -SimpleMatch 'storage policy test injected failure' -Quiet)) { throw 'Expected injected failure was not reached' }
    Assert-Policy $true
    if ($before -ne (Invoke-Sql "SELECT checksum || ':' || applied_at::text FROM sys_schema_migrations WHERE starts_with(migration_id,'runtime_device_data_storage_policy_');")) { throw 'Failure changed migration bookkeeping' }
    if ($jobBefore -ne (Invoke-Sql "SELECT job_id FROM timescaledb_information.jobs WHERE hypertable_name='device_data' AND proc_name='policy_compression';")) { throw 'Failure replaced compression job' }
    Invoke-Sql 'DROP TRIGGER reject_storage_policy ON sys_schema_migrations; DROP FUNCTION reject_storage_policy();' | Out-Null
    Invoke-Migration 'retry'
    Assert-Policy $false 24
    Write-Output 'PASS failure rolls back policy, chunk settings and bookkeeping; retry succeeds'
} finally {
    if ($migrationProcess -and !$migrationProcess.HasExited) {
        $current = Get-Process -Id $migrationProcess.Id -ErrorAction SilentlyContinue
        if ($current -and $current.StartTime -eq $migrationProcess.StartTime -and $current.Path -eq (Join-Path $fixture 'iot-engine.exe')) { Stop-Process -Id $current.Id -Force }
    }
    if (Test-Path -LiteralPath (Join-Path $data 'postmaster.pid')) { & $pgctl -D $data -m fast -w stop | Out-Null }
    $env:Path = $oldPath
    foreach ($name in $fixtureEnvironment) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Write-Output "Disposable fixture logs: $fixture"
}
