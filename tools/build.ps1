# Configure + build with MSVC (x64) + Ninja.
#   tools\build.ps1 -Asan   -> build-asan (AddressSanitizer)
#   tools\build.ps1         -> build
param(
    [switch]$Asan,
    [string]$BuildType = "Debug",
    [switch]$ConfigureOnly
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dir = if ($Asan) { "build-asan" } else { "build" }
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$asanOpt = if ($Asan) { "-DAT_ASAN=ON" } else { "" }
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaDir = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

$bat = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=$ninjaDir;%PATH%
cd /d $root
"$cmake" -S . -B $dir -G Ninja -DCMAKE_BUILD_TYPE=$BuildType $asanOpt -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
if "$($ConfigureOnly.IsPresent)"=="True" exit /b 0
"$cmake" --build $dir -j 16 -- -k 0 || exit /b 1
"@
$tmp = Join-Path $env:TEMP "at_build_$dir.bat"
Set-Content -Path $tmp -Value $bat -Encoding ASCII
cmd /c $tmp
exit $LASTEXITCODE
