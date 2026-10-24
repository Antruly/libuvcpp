# Collect deps/ and assemble the kit zip for one AppVeyor matrix leg.

param(
  [Parameter(Mandatory = $true)][string]$Target,
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [Parameter(Mandatory = $true)][string]$Arch,
  [Parameter(Mandatory = $true)][string]$Toolchain,
  [string]$MingwDir = ""
)

$ErrorActionPreference = "Stop"
Set-Location $env:APPVEYOR_BUILD_FOLDER

$collectArgs = @(
  "-Target", $Target,
  "-BuildDir", $BuildDir,
  "-DepsRoot", "kit-deps",
  "-OpenSSLRoot", "deps\openssl-$Arch",
  "-CmakeRoot", "deps\cmake"
)
if ($Toolchain -eq "mingw") {
  $collectArgs += @("-MingwRoot", "deps\$MingwDir")
}

& powershell -NoProfile -ExecutionPolicy Bypass -File build-kit\collect-deps.ps1 @collectArgs
if ($LASTEXITCODE -ne 0) { throw "collect-deps.ps1 failed" }

# Make prebuilt/include self-contained for consumers: uv.h / openssl/*.h / zlib.h
foreach ($sub in @("libuv\include", "openssl\include", "zlib\include")) {
  $p = Join-Path "kit-deps" $sub
  if (Test-Path $p) {
    Copy-Item (Join-Path $p "*") "stage\include" -Recurse -Force
  }
}

$out = "uvcpp-kit-$Target.zip"
if (Get-Command py -ErrorAction SilentlyContinue) {
  & py -3 packaging\make_build_kit.py --deps kit-deps --target $Target --out $out --prebuilt stage
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
  & python packaging\make_build_kit.py --deps kit-deps --target $Target --out $out --prebuilt stage
} else {
  throw "Python 3 is required to package the kit"
}
if ($LASTEXITCODE -ne 0) { throw "make_build_kit.py failed" }
Write-Host "kit: $out"
