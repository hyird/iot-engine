[CmdletBinding()]
param(
    [string]$PostgresBin = 'C:/Program Files/PostgreSQL/18/bin',
    [string]$RedisExe = 'C:/Redis/redis-server.exe',
    [string]$Bun = 'bun'
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $repository 'build'
$fixture = Join-Path $build ('vpn-desktop-fixture-' + [Guid]::NewGuid().ToString('N'))
$data = Join-Path $fixture 'postgres'
$backend = Join-Path $build 'Debug/iot-engine.exe'
foreach ($file in @($backend, $RedisExe, (Join-Path $PostgresBin 'initdb.exe'), (Join-Path $PostgresBin 'pg_ctl.exe'))) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing test dependency: $file" }
}
foreach ($port in @(55449,56449,55112)) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $port)
    try { $listener.Start() } finally { $listener.Stop() }
}
New-Item -ItemType Directory -Path $fixture | Out-Null
$apiProcess = $null
$redisProcess = $null
$oldPath = $env:Path
$oldFixture = $env:VPN_DESKTOP_FIXTURE
function Stop-OwnedProcess($Process, [string]$ExpectedPath) {
    if (!$Process) { return }
    $current = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if ($current -and $current.StartTime -eq $Process.StartTime -and
        [IO.Path]::GetFullPath($current.Path) -eq [IO.Path]::GetFullPath($ExpectedPath)) {
        Stop-Process -Id $Process.Id -Force
        $current.WaitForExit(10000) | Out-Null
    }
}
try {
    & (Join-Path $PostgresBin 'initdb.exe') -D $data -U architecture_test --auth=trust -E UTF8 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Test database initialization failed.' }
    Add-Content -LiteralPath (Join-Path $data 'postgresql.conf') -Value "shared_preload_libraries = 'timescaledb'"
    $pgArguments = '-D "{0}" -l "{1}" -o "-p 55449 -h 127.0.0.1" -w start' -f $data,(Join-Path $fixture 'postgres.log')
    $launcher = Start-Process -FilePath (Join-Path $PostgresBin 'pg_ctl.exe') -ArgumentList $pgArguments -WindowStyle Hidden -PassThru
    if (!$launcher.WaitForExit(30000)) { throw 'Test PostgreSQL launcher timed out.' }
    if ($launcher.ExitCode -ne 0) { throw "Test PostgreSQL startup failed; see $fixture" }
    & (Join-Path $PostgresBin 'createdb.exe') -h 127.0.0.1 -p 55449 -U architecture_test iot_architecture
    if ($LASTEXITCODE -ne 0) { throw 'Test database creation failed.' }
    Set-Content -LiteralPath (Join-Path $fixture 'redis.conf') -Encoding ascii -Value "bind 127.0.0.1`nport 56449`nsave `"`"`nappendonly no"
    # A relative config path also works with the Cygwin Redis distribution.
    $redisProcess = Start-Process -FilePath $RedisExe -ArgumentList 'redis.conf' -WorkingDirectory $fixture -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $fixture 'redis.out') -RedirectStandardError (Join-Path $fixture 'redis.err')
    Copy-Item -LiteralPath $backend -Destination (Join-Path $fixture 'iot-engine.exe')
    @'
JWT_SECRET=architecture-test-only-access-secret-000000000
JWT_REFRESH_SECRET=architecture-test-only-refresh-secret-00000000
DB_HOST=127.0.0.1
DB_PORT=55449
DB_USERNAME=architecture_test
DB_PASSWORD=
DB_DATABASE=iot_architecture
REDIS_HOST=127.0.0.1
REDIS_PORT=56449
HOST=127.0.0.1
PORT=55112
SERVICE_WORKERS=2
COLLECTOR_WORKERS=1
GB28181_ENABLED=false
VPN_HUB_ENABLED=false
EDGE_PUBLIC_BASE_URL=http://127.0.0.1:55112
EDGE_PLATFORM_ID=00000000-0000-7000-8000-000000000001
'@ | Set-Content -LiteralPath (Join-Path $fixture '.env') -Encoding ascii
    $env:Path = (Join-Path $build 'Debug') + ';' + $oldPath
    $env:VPN_DESKTOP_FIXTURE = $fixture
    $apiProcess = Start-Process -FilePath (Join-Path $fixture 'iot-engine.exe') -WorkingDirectory $fixture -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $fixture 'api.out') -RedirectStandardError (Join-Path $fixture 'api.err')
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $apiProcess.Refresh(); $redisProcess.Refresh()
        if ($apiProcess.HasExited -or $redisProcess.HasExited) { throw "Test process exited; inspect logs in $fixture" }
        try {
            $probe = [Net.Sockets.TcpClient]::new()
            $probe.Connect('127.0.0.1',55112)
            $ready = $true
        } catch { } finally { if ($probe) { $probe.Dispose() } }
        if ($ready) { break }
        Start-Sleep -Milliseconds 250
    }
    if (!$ready) { throw "Test API did not start; inspect logs in $fixture" }
    & $Bun run (Join-Path $PSScriptRoot 'vpn-desktop-integration.ts')
    if ($LASTEXITCODE -ne 0) { throw "VPN integration tests failed; logs retained at $fixture" }
} finally {
    Stop-OwnedProcess $apiProcess (Join-Path $fixture 'iot-engine.exe')
    Stop-OwnedProcess $redisProcess $RedisExe
    if (Test-Path -LiteralPath (Join-Path $data 'postmaster.pid')) {
        & (Join-Path $PostgresBin 'pg_ctl.exe') -D $data -m fast -w stop | Out-Null
    }
    $env:Path = $oldPath
    $env:VPN_DESKTOP_FIXTURE = $oldFixture
    Write-Output "Disposable fixture logs: $fixture"
}
