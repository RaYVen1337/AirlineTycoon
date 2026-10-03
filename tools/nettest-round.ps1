# Builds the ASan build, runs a host/client pair (tools\dbgrun.ps1) and prints crash/ASan stacks.
#   powershell -ExecutionPolicy Bypass -File tools\nettest-round.ps1 [-Fuzz 50] [-Turbo 4] [-Days 1] [-Monkey -1] [-NoBuild]
# Fuzz > 0 damages messages on the wire (the checksum must drop them), Fuzz < 0 damages them behind the checksum.
# Needs cdbX64.exe (WinDbg) in PATH.
param([string]$Root = (Split-Path -Parent $PSScriptRoot), [int]$Fuzz = 50, [int]$Turbo = 4, [int]$Days = 1, [int]$MaxSec = 400, [int]$Monkey = 0, [switch]$NoBuild)
$bd = "$Root\build-asan"
if (-not $NoBuild) {
    $b = powershell -ExecutionPolicy Bypass -File "$Root\tools\build.ps1" -Asan 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { ($b -split "`n") | Where-Object { $_ -match 'error|FAILED' } | Select-Object -First 10; 'BUILD FAILED'; exit 1 }
}
Start-Process powershell -ArgumentList '-ExecutionPolicy', 'Bypass', '-File', "$Root\tools\dbgrun.ps1", '-BuildDir', $bd, '-Days', "$Days", '-TimeoutSec', "$MaxSec", '-Turbo', "$Turbo", '-Fuzz', "$Fuzz", '-Monkey', "$Monkey" -WindowStyle Hidden
Start-Sleep 3
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\nettest-watch.ps1" -BuildDir $bd -MaxSec ($MaxSec + 20)
foreach ($r in 'host', 'client') {
    $t = Get-Content "$bd\nettest\$r.cdb.txt" -EA SilentlyContinue
    if (-not $t) { continue }
    $i = [array]::FindIndex([string[]]$t, [Predicate[string]] { param($l) $l -match '^CRASH_HIT' })
    if ($i -ge 0) {
        "=== $r stack"
        $t[$i..([math]::Min($i + 24, $t.Count - 1))] | Where-Object { $_ -match '!' } | ForEach-Object { $_ -replace '^[0-9a-f]{2} [0-9a-f`]+ [0-9a-f`]+ +', '' } | Select-Object -First 14
    }
    $j = [array]::FindIndex([string[]]$t, [Predicate[string]] { param($l) $l -match 'ERROR: AddressSanitizer' })
    if ($j -ge 0) { "=== $r ASan"; $t[$j..([math]::Min($j + 16, $t.Count - 1))] }
}
