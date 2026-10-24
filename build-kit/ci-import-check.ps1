# Check that the produced DLL does not import Win8+/Win10-only APIs.
#
# Called by AppVeyor after the build. This is an early warning, not a
# substitute for running on a Windows 7 VM.

param(
  [Parameter(Mandatory = $true)][string]$Toolchain,
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [Parameter(Mandatory = $true)][string]$Arch,
  [string]$Vcvars = "",
  [string]$VcArch = "x86",
  [string]$MingwDir = ""
)

$ErrorActionPreference = "Stop"
$forbidden = "GetSystemTimePreciseAsFileTime|SetThreadDescription|GetDpiForWindow|CreateFile2"

if ($Toolchain -eq "msvc") {
  $dll = Join-Path $BuildDir "Release\uvcpp.dll"
  if (-not (Test-Path $dll)) { throw "missing $dll" }
  $bat = Join-Path $env:TEMP ("uvcpp-imports-" + [guid]::NewGuid().ToString("N") + ".bat")
  @(
    "@echo off",
    "call `"$Vcvars`" $VcArch",
    "if errorlevel 1 exit /b 1",
    "dumpbin /imports `"$dll`""
  ) | Set-Content -Path $bat -Encoding ASCII
  $out = & cmd.exe /c "`"$bat`"" 2>&1
  Remove-Item $bat -Force
  if ($out -match $forbidden) {
    throw "Win8+/Win10-only import found:`n$($out | Select-String $forbidden)"
  }
} else {
  $dll = Get-ChildItem -Path $BuildDir -Recurse -File -Filter "uvcpp.dll" |
         Select-Object -First 1
  if (-not $dll) { throw "no uvcpp.dll under $BuildDir" }
  $objdump = Join-Path $MingwDir "bin\objdump.exe"
  if (-not (Test-Path $objdump)) {
    $objdump = Get-ChildItem -Path $MingwDir -Recurse -File -Filter "objdump.exe" |
               Select-Object -First 1
    if (-not $objdump) { throw "no objdump.exe under $MingwDir" }
    $objdump = $objdump.FullName
  }
  $out = & $objdump -p $dll.FullName 2>&1
  if ($out -match $forbidden) {
    throw "Win8+/Win10-only import found:`n$($out | Select-String $forbidden)"
  }
}

Write-Host "import check: no Win8+/Win10-only APIs"
