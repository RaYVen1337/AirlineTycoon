param([string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-asan'), [int]$MaxSec = 420, [int]$StallSec = 40)
# Watches a running dbgrun pair: reports crash, stall (with main thread stack) or progress.
$o = "$BuildDir\nettest"
$t0 = Get-Date
Start-Sleep 5
$lastLine = ''; $lastChange = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $MaxSec) {
    if (Select-String "$o\client.cdb.txt", "$o\host.cdb.txt" -Pattern '^CRASH_HIT|ERROR: AddressSanitizer|NETTEST (SEH|CRT)' -Quiet -EA SilentlyContinue) { 'RESULT: CRASH'; break }
    $procs = Get-CimInstance Win32_Process -Filter "Name='AT.exe'" | Where-Object { $_.ExecutablePath -like "$BuildDir*" }
    if (-not $procs) { 'RESULT: processes ended'; break }
    $l = (Select-String "$o\host.debug.txt", "$o\client.debug.txt" -Pattern 'NETTEST: day' -EA SilentlyContinue | ForEach-Object Line) -join ' | '
    if ($l -ne $lastLine) { $lastLine = $l; $lastChange = Get-Date }
    elseif (((Get-Date) - $lastChange).TotalSeconds -gt $StallSec -and $l -match 'time=(09|1[0-7])') {
        "RESULT: STALL ($l)"
        foreach ($p in $procs) {
            "--- stack of " + ($(if ($p.CommandLine -match 'host') { 'host' } else { 'client' }))
            cdbX64.exe -pv -p $p.ProcessId -c "~0kn 20;q" 2>&1 | Select-String -Pattern '^[0-9a-f]{2} ' | Select-Object -First 20 | ForEach-Object { $_.Line -replace '^[0-9a-f]{2} [0-9a-f`]+ [0-9a-f`]+ +', '' }
        }
        break
    }
    Start-Sleep 3
}
"elapsed $([int]((Get-Date) - $t0).TotalSeconds)s; progress: $lastLine"
foreach ($r in 'host', 'client') { "$r dropped messages: " + (Select-String "$o\$r.debug.txt" -Pattern 'Dropping|shorter than expected' -EA SilentlyContinue).Count + ", fuzz: " + (Select-String "$o\$r.debug.txt" -Pattern 'NETTEST fuzz' -EA SilentlyContinue | Select-Object -Last 1 | ForEach-Object Line) }
