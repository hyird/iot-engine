[CmdletBinding()]
param(
    [string]$PostgresBin = 'C:/Program Files/PostgreSQL/18/bin',
    [string]$RedisExe = 'C:/Redis/redis-server.exe',
    [string]$Bun = 'build/bun-1.3.14/bun-windows-x64/bun.exe',
    [switch]$Gb28181
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $repository 'build'
$fixture = Join-Path $build ('rpc-integration-fixture-' + [Guid]::NewGuid().ToString('N'))
$data = Join-Path $fixture 'postgres'
$bunPath = if ([IO.Path]::IsPathRooted($Bun)) { [IO.Path]::GetFullPath($Bun) } else {
    [IO.Path]::GetFullPath((Join-Path $repository $Bun))
}
$backend = Join-Path $build 'Release/iot-engine.exe'
foreach ($file in @($backend, $bunPath, $RedisExe, (Join-Path $PostgresBin 'initdb.exe'),
                    (Join-Path $PostgresBin 'pg_ctl.exe'))) {
    if (!$file -or !(Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing test dependency: $file"
    }
}
foreach ($port in @(55469, 56469, 55132)) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $port)
    try { $listener.Start() } finally { $listener.Stop() }
}
New-Item -ItemType Directory -Path $fixture | Out-Null
$apiProcess = $null
$redisProcess = $null
$oldPath = $env:Path
$oldDatabaseUrl = $env:ARCHITECTURE_DATABASE_URL
$oldRedisUrl = $env:ARCHITECTURE_REDIS_URL
$oldApiBase = $env:TEST_BASE_URL
$oldRpcTestWorkers = $env:RPC_TEST_WORKERS
function Stop-OwnedProcess($Process, [string]$ExpectedPath) {
    if (!$Process) { return }
    try {
        $current = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
        if (!$current) { return }
        $current.Refresh()
        $sameStart = $current.StartTime -eq $Process.StartTime
        $samePath = $false
        try {
            $samePath = [IO.Path]::GetFullPath($current.Path) -eq
                [IO.Path]::GetFullPath($ExpectedPath)
        } catch {
            $samePath = $false
        }
        if (!$sameStart -or !$samePath) { return }
        Stop-Process -Id $current.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $current.Id -Timeout 10 -ErrorAction SilentlyContinue
    } catch {
        Write-Warning "Could not stop owned process $($Process.Id): $($_.Exception.Message)"
    }
}
try {
    & (Join-Path $PostgresBin 'initdb.exe') -D $data -U architecture_test --auth=trust -E UTF8 --locale=C | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Test database initialization failed.' }
    Add-Content -LiteralPath (Join-Path $data 'postgresql.conf') -Value "shared_preload_libraries = 'timescaledb'"
    $pgArguments = '-D "{0}" -l "{1}" -o "-p 55469 -h 127.0.0.1" -w start' -f $data,(Join-Path $fixture 'postgres.log')
    $launcher = Start-Process -FilePath (Join-Path $PostgresBin 'pg_ctl.exe') -ArgumentList $pgArguments -WindowStyle Hidden -PassThru
    if (!$launcher.WaitForExit(30000)) { throw 'Test PostgreSQL launcher timed out.' }
    if ($launcher.ExitCode -ne 0) { throw "Test PostgreSQL startup failed; see $fixture" }
    & (Join-Path $PostgresBin 'createdb.exe') -h 127.0.0.1 -p 55469 -U architecture_test iot_architecture
    if ($LASTEXITCODE -ne 0) { throw 'Test database creation failed.' }
    Set-Content -LiteralPath (Join-Path $fixture 'redis.conf') -Encoding ascii -Value "bind 127.0.0.1`nport 56469`nsave `"`"`nappendonly no"
    $redisProcess = Start-Process -FilePath $RedisExe -ArgumentList 'redis.conf' -WorkingDirectory $fixture -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $fixture 'redis.out') -RedirectStandardError (Join-Path $fixture 'redis.err')
    Copy-Item -LiteralPath $backend -Destination (Join-Path $fixture 'iot-engine.exe')
    @'
JWT_SECRET=architecture-test-only-access-secret-000000000
JWT_REFRESH_SECRET=architecture-test-only-refresh-secret-00000000
DB_HOST=127.0.0.1
DB_PORT=55469
DB_USERNAME=architecture_test
DB_PASSWORD=
DB_DATABASE=iot_architecture
REDIS_HOST=127.0.0.1
REDIS_PORT=56469
HOST=127.0.0.1
PORT=55132
SERVICE_WORKERS=2
COLLECTOR_WORKERS=1
GB28181_ENABLED=false
VPN_HUB_ENABLED=false
EDGE_PUBLIC_BASE_URL=http://127.0.0.1:55132
EDGE_PLATFORM_ID=00000000-0000-7000-8000-000000000001
'@ | Set-Content -LiteralPath (Join-Path $fixture '.env') -Encoding ascii
    if ($Gb28181) {
        $configuration = Get-Content -LiteralPath (Join-Path $fixture '.env') -Raw
        $configuration = $configuration.Replace('COLLECTOR_WORKERS=1', 'COLLECTOR_WORKERS=2').Replace('GB28181_ENABLED=false', 'GB28181_ENABLED=true')
        $configuration += @'

GB28181_SIP_DOMAIN=3402000000
GB28181_SIP_ID=34020000002000000001
GB28181_SIP_HOST=127.0.0.1
GB28181_SIP_PUBLIC_IP=127.0.0.1
GB28181_SIP_PORT=55133
GB28181_SIP_PASSWORD=test
GB28181_SIP_TRANSPORT=both
GB28181_RTP_PUBLIC_IP=127.0.0.1
GB28181_MEDIA_TOKEN_SECRET=gb28181-integration-only-secret
GB28181_RTP_PORT_START=0
GB28181_RTP_PORT_END=0
ZLM_HTTP_PORT=0
ZLM_RTSP_PORT=0
ZLM_RTMP_PORT=0
ZLM_RTC_PORT=0
ZLM_SRT_PORT=0
'@
        Set-Content -LiteralPath (Join-Path $fixture '.env') -Value $configuration -Encoding ascii
    }
    $env:Path = (Join-Path $build 'Release') + ';' + $oldPath
    $env:ARCHITECTURE_DATABASE_URL = 'postgres://architecture_test@127.0.0.1:55469/iot_architecture'
    $env:ARCHITECTURE_REDIS_URL = 'redis://127.0.0.1:56469'
    $env:TEST_BASE_URL = 'http://127.0.0.1:55132'
    $env:RPC_TEST_WORKERS = '2'
    $apiProcess = Start-Process -FilePath (Join-Path $fixture 'iot-engine.exe') -WorkingDirectory $fixture -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $fixture 'api.out') -RedirectStandardError (Join-Path $fixture 'api.err')
    $ready = $false
    $healthClient = [Net.Http.HttpClient]::new()
    try {
        for ($attempt = 0; $attempt -lt 120; $attempt++) {
            $apiProcess.Refresh(); $redisProcess.Refresh()
            if ($apiProcess.HasExited -or $redisProcess.HasExited) {
                throw "Test process exited; inspect logs in $fixture"
            }
            try {
                $probe = [Net.Sockets.TcpClient]::new()
                try { $probe.Connect('127.0.0.1',55132) }
                finally { if ($probe) { $probe.Dispose() } }
                $health = $healthClient.GetAsync('http://127.0.0.1:55132/internal/health/ready').GetAwaiter().GetResult()
                try { $ready = [int]$health.StatusCode -eq 200 }
                finally { $health.Dispose() }
            } catch { }
            if ($ready) { break }
            Start-Sleep -Milliseconds 250
        }
    } finally {
        $healthClient.Dispose()
    }
    if (!$ready) { throw "Test API did not become ready; inspect logs in $fixture" }
    if ($Gb28181) {
        & $bunPath run (Join-Path $repository 'tests/gb28181-worker-integration.ts')
        if ($LASTEXITCODE -ne 0) { throw "GB28181 integration tests failed; logs retained at $fixture" }
        return
    }
    & $bunPath run (Join-Path $repository 'tests/rpc-integration.ts')
    if ($LASTEXITCODE -ne 0) { throw "RPC integration tests failed; logs retained at $fixture" }
    & $bunPath run (Join-Path $repository 'tests/edge-recovery-integration.ts')
    if ($LASTEXITCODE -ne 0) { throw "Edge Redis recovery tests failed; logs retained at $fixture" }
    # This final test intentionally expires an owner and expects readiness to fail.
    & $bunPath run (Join-Path $repository 'tests/edge-recovery-application.ts')
    if ($LASTEXITCODE -ne 0) { throw "Edge application recovery tests failed; logs retained at $fixture" }
} finally {
    Stop-OwnedProcess $apiProcess (Join-Path $fixture 'iot-engine.exe')
    Stop-OwnedProcess $redisProcess $RedisExe
    if (Test-Path -LiteralPath (Join-Path $data 'postmaster.pid')) {
        & (Join-Path $PostgresBin 'pg_ctl.exe') -D $data -m fast -w stop | Out-Null
    }
    $env:Path = $oldPath
    $env:ARCHITECTURE_DATABASE_URL = $oldDatabaseUrl
    $env:ARCHITECTURE_REDIS_URL = $oldRedisUrl
    $env:TEST_BASE_URL = $oldApiBase
    $env:RPC_TEST_WORKERS = $oldRpcTestWorkers
    Write-Output "Disposable fixture logs: $fixture"
}
