<#
.SYNOPSIS
  One-step verification of OpenCDC on Windows x64 / Visual Studio 2022:
  configure, build (Debug + Release, warnings as errors), run all tests,
  install, package (ZIP, and NSIS when makensis is available).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\ci\verify-windows.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64

  Everything is logged to build\verify-windows.log; the packages land in dist\.
  Exit code 0 means every step passed.
#>
param(
    [Parameter(Mandatory = $true)][string]$QtDir,
    [switch]$ReleaseOnly
)

# "Continue": native tools write warnings to stderr; in Windows PowerShell 5.1
# "Stop" would turn those into terminating errors. Success is decided by
# exit codes only.
$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $root
New-Item -ItemType Directory -Force build | Out-Null
$log = Join-Path $root "build\verify-windows.log"
"OpenCDC Windows verification $(Get-Date -Format s)" | Out-File $log -Encoding utf8

function Step([string]$name, [scriptblock]$cmd) {
    Write-Host "==> $name" -ForegroundColor Cyan
    "==> $name" | Out-File $log -Append -Encoding utf8
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $cmd 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $name (exit $LASTEXITCODE) - see $log" -ForegroundColor Red
        exit 1
    }
    Write-Host ("    ok ({0:N0} s)" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}

if (-not (Test-Path (Join-Path $QtDir "bin\windeployqt.exe"))) {
    Write-Host "Qt not found: $QtDir\bin\windeployqt.exe" -ForegroundColor Red
    exit 2
}
$env:CMAKE_PREFIX_PATH = $QtDir
$env:PATH = "$QtDir\bin;$env:PATH"
$nsis = "${env:ProgramFiles(x86)}\NSIS"
if (Test-Path "$nsis\makensis.exe") { $env:PATH = "$nsis;$env:PATH" }

Step "tool versions" { cmake --version; ctest --version | Select-Object -First 1 }
Step "configure (windows-msvc-ci, /W4 /WX)" { cmake --preset windows-msvc-ci }

$configs = if ($ReleaseOnly) { @("release") } else { @("debug", "release") }
foreach ($c in $configs) {
    Step "build $c" { cmake --build --preset "windows-msvc-ci-$c" --parallel }
    Step "test $c" { ctest --preset "windows-msvc-ci-$c" --parallel 4 }
}

Step "package (cpack)" { cpack --config build\windows-msvc-ci\CPackConfig.cmake -C Release -B dist }

Write-Host ""
Write-Host "All steps passed. Packages:" -ForegroundColor Green
Get-ChildItem dist -File | Where-Object { $_.Extension -in ".zip", ".exe" } | ForEach-Object { "  " + $_.FullName }
Write-Host "Log: $log"
exit 0
