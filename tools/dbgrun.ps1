# Runs a headless /nettest host + client pair under cdb (WinDbg) and prints the first crash stack of each side.
#   tools\dbgrun.ps1 [-Days 3] [-TimeoutSec 600] [-Speed 3] [-BuildDir <dir with AT.exe>] [-Port 60011] [-Monkey <seed>] [-Turbo 1] [-Fuzz 0]
param([int]$Days = 3, [int]$TimeoutSec = 600, [int]$Speed = 3, [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-asan'), [int]$Port = 60011, [int]$Monkey = 0, [int]$Turbo = 1, [int]$Fuzz = 0)
$bd = $BuildDir
$out = "$bd\nettest"
New-Item -ItemType Directory -Force $out | Out-Null
$env:SDL_VIDEODRIVER = 'dummy'; $env:SDL_AUDIODRIVER = 'dummy'; $env:ASAN_OPTIONS = 'detect_stack_use_after_return=0'
$cmd = '"sxe -c \".echo CRASH_HIT;.lines -e;.ecxr;kn 60;q\" sov; sxe -c \".echo CRASH_HIT;.lines -e;kn 60;q\" bpe; sxd av; sxd eh; sxe -c \".echo AV2_HIT\" -c2 \".echo CRASH_HIT;.lines -e;kn 60;q\" av; g"'
function Start-Dbg($role, $roleArgs) {
    Remove-Item "$out\$role.*" -EA SilentlyContinue
    $env:AT_LOG_FILE = "$out\$role.debug.txt"
    $a = @('-G', '-y', $bd, '-c', $cmd, "$bd\AT.exe", '/nettest') + $roleArgs + @('/nettestdays', "$Days", '/nettestspeed', "$Speed", '/nettestmonkey', "$Monkey", '/nettestturbo', "$Turbo", '/nettestfuzz', "$Fuzz")
    Start-Process cdbX64.exe -ArgumentList $a -WorkingDirectory $bd -RedirectStandardOutput "$out\$role.cdb.txt" -RedirectStandardError "$out\$role.stderr.txt" -PassThru -WindowStyle Hidden
}
function Stop-Ours {
    Get-CimInstance Win32_Process -Filter "Name='AT.exe'" | Where-Object { $_.ExecutablePath -like "$bd*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -EA SilentlyContinue }
}
Stop-Ours
$h = Start-Dbg 'host' @('host', "$Port")
$t0 = Get-Date
while (-not (Select-String -Path "$out\host.debug.txt" -Pattern 'CREATE SESSION' -Quiet -EA SilentlyContinue)) {
    if (((Get-Date) - $t0).TotalSeconds -gt 90) { break }
    Start-Sleep 2
}
$c = Start-Dbg 'client' @('client', '127.0.0.1', "$Port")
$null = $h.WaitForExit($TimeoutSec * 1000)
$null = $c.WaitForExit(30000)
Stop-Ours
foreach ($p in $h, $c) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -EA SilentlyContinue } }
foreach ($r in 'host', 'client') {
    "===== $r"
    Select-String -Path "$out\$r.debug.txt" -Pattern 'NETTEST: day' | Select-Object -Last 1 | ForEach-Object Line
    $t = Get-Content "$out\$r.cdb.txt"
    $i = [array]::FindIndex([string[]]$t, [Predicate[string]] { param($l) $l -match '^CRASH_HIT' })
    if ($i -ge 0) {
        $t[([math]::Max($i - 3, 0))..([math]::Min($i + 45, $t.Count - 1))] | ForEach-Object { $_ -replace '^[0-9a-f]{2} [0-9a-f`]+ [0-9a-f`]+ +', '' }
    } else {
        'no crash caught'
        Select-String -Path "$out\$r.cdb.txt" -Pattern 'ERROR: AddressSanitizer|NETTEST (SEH|CRT)|Assertion' | Select-Object -First 5 | ForEach-Object Line
    }
}

