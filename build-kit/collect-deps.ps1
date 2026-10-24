# Collect the dependency prefix for a portable kit from a CMake build tree.
#
# This is the maintainer/CI side of the kit. End users do not run it: the kit
# zip they download already has deps/ populated.
#
# Usage (from a build tree that used FetchContent):
#   powershell -File build-kit\collect-deps.ps1 `
#     -Target vs2015-x86 `
#     -BuildDir build-win7-vs2015-x86 `
#     -DepsRoot kit-deps `
#     -OpenSSLRoot deps\openssl-x86 `
#     -CmakeRoot deps\cmake

param(
  [Parameter(Mandatory = $true)][string]$Target,
  [Parameter(Mandatory = $true)][string]$BuildDir,
  [Parameter(Mandatory = $true)][string]$DepsRoot,
  [Parameter(Mandatory = $true)][string]$OpenSSLRoot,
  [string]$CmakeRoot = "",
  [string]$MingwRoot = ""
)

$ErrorActionPreference = "Stop"

function Copy-Dir([string]$from, [string]$to) {
  if (-not (Test-Path $from)) { throw "missing: $from" }
  New-Item -ItemType Directory -Force -Path $to | Out-Null
  Copy-Item -Path (Join-Path $from "*") -Destination $to -Recurse -Force
}

function Find-First([string]$root, [string]$pattern) {
  $f = Get-ChildItem -Path $root -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
       Sort-Object FullName | Select-Object -First 1
  if (-not $f) { throw "no $pattern under $root" }
  return $f
}

$src = Join-Path $BuildDir "_deps"
if (-not (Test-Path $src)) { throw "no _deps under $BuildDir; was libuv built via FetchContent?" }

# libuv: headers + static library
Copy-Dir (Join-Path $src "libuv-src\include") (Join-Path $DepsRoot "libuv\include")
$isMingw = $Target -like "mingw*"
if ($isMingw) {
  $uvPattern = "libuv_a.a"; $uvName = "libuv_a.a"
} else {
  $uvPattern = "uv_a.lib";  $uvName = "uv_a.lib"
}
$uvLib = Find-First $src $uvPattern
$uvLibDir = Join-Path $DepsRoot "libuv\lib"
New-Item -ItemType Directory -Force -Path $uvLibDir | Out-Null
Copy-Item $uvLib.FullName (Join-Path $uvLibDir $uvName) -Force

# zlib: only when the profile built it (web-lite/VS2013 does not).
$zsrc = Join-Path $src "zlib-src"
if (Test-Path $zsrc) {
  Copy-Dir $zsrc (Join-Path $DepsRoot "zlib\include")
  Copy-Item (Find-First $src "zconf.h").FullName (Join-Path $DepsRoot "zlib\include\zconf.h") -Force
  if ($isMingw) {
    $zLib = Get-ChildItem -Path $src -Recurse -File -Include "libz.a","libzlibstatic.a" -ErrorAction SilentlyContinue |
            Sort-Object FullName | Select-Object -First 1
    if (-not $zLib) { throw "no MinGW zlib static library under $src" }
  } else {
    $zLib = Find-First $src "zlibstatic.lib"
  }
  $zLibDir = Join-Path $DepsRoot "zlib\lib"
  New-Item -ItemType Directory -Force -Path $zLibDir | Out-Null
  Copy-Item $zLib.FullName (Join-Path $zLibDir $zLib.Name) -Force
} else {
  Write-Host "no zlib in this build tree (profile does not use gzip); skipping"
}

# llhttp: source only (CMake builds it from this tree)
Copy-Dir (Join-Path $src "llhttp-src") (Join-Path $DepsRoot "llhttp")

# OpenSSL: the CI built it into a separate prefix
Copy-Dir $OpenSSLRoot (Join-Path $DepsRoot "openssl")

# Portable CMake (optional, but makes the kit self-contained)
if ($CmakeRoot -and (Test-Path $CmakeRoot)) {
  Copy-Dir $CmakeRoot (Join-Path $DepsRoot "cmake")
}

# MinGW toolchain: keep it in the kit.
if ($MingwRoot -and (Test-Path $MingwRoot)) {
  Copy-Dir $MingwRoot (Join-Path $DepsRoot (Split-Path $MingwRoot -Leaf))
} else {
  foreach ($tc in @("mingw32", "mingw64")) {
    $p = Join-Path (Split-Path $DepsRoot -Parent) $tc
    if (Test-Path $p) { Copy-Dir $p (Join-Path $DepsRoot $tc) }
  }
}

Write-Host "dependency prefix collected in $DepsRoot"
