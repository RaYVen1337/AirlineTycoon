# Headless two-process multiplayer repro (host + client on 127.0.0.1), both humans auto-played.
#   powershell -ExecutionPolicy Bypass -File tools\nettest.ps1 [-Days 3] [-TimeoutSec 900] [-Speed 5] [-BuildDir build-asan]
# Logs: <BuildDir>\nettest\host.log|client.log (stdout/stderr), host.debug.txt|client.debug.txt (game debug log),
#       asan_host.<pid>|asan_client.<pid> (ASan reports)
param(
    [int]$Days = 3,
    [int]$TimeoutSec = 900,
    [int]$Speed = 3,
    [string]$BuildDir = "build-asan",
    [int]$Port = 60011
)

$root = Split-Path -Parent $PSScriptRoot
$bd = Join-Path $root $BuildDir
$exe = Join-Path $bd "AT.exe"
$out = Join-Path $bd "nettest"
if (-not (Test-Path $exe)) { Write-Error "missing $exe (run tools\build.ps1 first)"; exit 10 }

New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*" -Force -ErrorAction SilentlyContinue

# make sure no stale instance keeps the port
Get-Process AT -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force

function Start-Role($role, $extraArgs) {
    $env:AT_LOG_FILE = "$out\$role.debug.txt"
    $env:ASAN_OPTIONS = "log_path='$out\asan_$role' print_stacktrace=1 symbolize=1 handle_segv=1 handle_abort=1 handle_sigfpe=1 detect_stack_use_after_return=0"
    $env:SDL_VIDEODRIVER = "dummy"
    $env:SDL_AUDIODRIVER = "dummy"
    $args = @("/nettest") + $extraArgs + @("/nettestdays", "$Days", "/nettestspeed", "$Speed")
    $p = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $bd -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput "$out\$role.log" -RedirectStandardError "$out\$role.err"
    $null = $p.Handle # keep the handle so ExitCode is available
    return $p
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$host_p = Start-Role "host" @("host", "$Port")
Write-Host "host pid $($host_p.Id) started"

# wait for the host to listen before the client connects (RakNet connect is a one shot in this code base)
$ready = $false
while ($sw.Elapsed.TotalSeconds -lt 180 -and -not $host_p.HasExited) {
    if ((Test-Path "$out\host.debug.txt") -and (Select-String -Path "$out\host.debug.txt" -Pattern "CREATE SESSION: DIRECT" -Quiet -ErrorAction SilentlyContinue)) { $ready = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $ready) { Write-Host "host never created a session (exited=$($host_p.HasExited))" }
Start-Sleep -Seconds 1

$client_p = $null
if ($ready) {
    $client_p = Start-Role "client" @("client", "127.0.0.1", "$Port")
    Write-Host "client pid $($client_p.Id) started"
}

$procs = @($host_p) + @($client_p | Where-Object { $_ })
$timedOut = $false
while ($true) {
    $alive = @($procs | Where-Object { -not $_.HasExited })
    if ($alive.Count -eq 0) { break }
    if ($sw.Elapsed.TotalSeconds -gt $TimeoutSec) { $timedOut = $true; break }
    # if one side died abnormally, give the peer a few seconds then stop waiting
    $dead = @($procs | Where-Object { $_.HasExited -and $_.ExitCode -ne 0 })
    if ($dead.Count -gt 0) { Start-Sleep -Seconds 5; break }
    Start-Sleep -Seconds 1
}
if ($timedOut) { Write-Host "TIMEOUT after $TimeoutSec s" }
foreach ($p in $procs) { if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() } }

Write-Host "`n=== result (elapsed $([int]$sw.Elapsed.TotalSeconds)s) ==="
Write-Host "host   exit code: $($host_p.ExitCode)"
if ($client_p) { Write-Host "client exit code: $($client_p.ExitCode)" }

foreach ($role in @("host", "client")) {
    Write-Host "`n--- $role : last NETTEST lines ---"
    if (Test-Path "$out\$role.debug.txt") {
        Select-String -Path "$out\$role.debug.txt" -Pattern "NETTEST" | Select-Object -Last 6 | ForEach-Object { $_.Line }
        Write-Host "--- $role : sync/route lines (last 5) ---"
        Select-String -Path "$out\$role.debug.txt" -Pattern "SYNC_ROUTES|SYNC_MONEY|SYNC_IMAGE" | Select-Object -Last 5 | ForEach-Object { $_.Line }
    }
    Write-Host "--- $role : tail of debug log ---"
    if (Test-Path "$out\$role.debug.txt") { Get-Content "$out\$role.debug.txt" -Tail 8 }
    Write-Host "--- $role : tail of stderr ---"
    if (Test-Path "$out\$role.err") { Get-Content "$out\$role.err" -Tail 8 }
    $asan = Get-ChildItem "$out" -Filter "asan_$role.*" -ErrorAction SilentlyContinue
    foreach ($a in $asan) {
        Write-Host "--- $role : ASan report $($a.Name) (first 60 lines) ---"
        Get-Content $a.FullName -TotalCount 60
    }
}

$rc = 0
if ($timedOut) { $rc = 20 }
elseif ($host_p.ExitCode -ne 0 -or ($client_p -and $client_p.ExitCode -ne 0)) { $rc = 1 }
elseif (-not $client_p) { $rc = 21 }
exit $rc
