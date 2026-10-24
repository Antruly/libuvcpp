# Build libuvcpp from a portable kit.
#
# The kit zip already contains the pinned dependency prefix under deps/
# (OpenSSL 1.1.1w, libuv 1.33/1.40 with the CVE-2024-24806 backport, zlib,
# llhttp). This script only runs CMake with the user's compiler; it does not
# download or build dependencies.
#
# Usage:
#   build-kit\build.cmd vs2015-x86
#   build-kit\build.cmd vs2015-x64 -Profile web
#   build-kit\build.cmd vs2013-x86 -Profile web-lite
#   build-kit\build.cmd mingw32
#
# Output:
#   out\<target>\  (uvcpp.dll / .lib / headers / CMake config)

param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("vs2013-x86", "vs2015-x86", "vs2015-x64", "mingw32", "mingw64")]
  [string]$Target,

  [ValidateSet("net", "net-ssl", "web-lite", "web", "full")]
  [string]$Profile = "",

  [string]$DepsRoot = "",
  [string]$OutRoot = ""
)

$ErrorActionPreference = "Stop"

$KitRoot = Split-Path -Parent $PSScriptRoot
if (-not $DepsRoot) { $DepsRoot = Join-Path $KitRoot "deps" }
if (-not $OutRoot)  { $OutRoot  = Join-Path $KitRoot "out" }

$targets = @{
  "vs2013-x86" = @{
    Kind = "msvc"; Generator = "NMake Makefiles"
    VcVars = "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
    VcArch = "x86"; DefaultProfile = "web-lite"
  }
  "vs2015-x86" = @{
    Kind = "msvc"; Generator = "NMake Makefiles"
    VcVars = "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat"
    VcArch = "x86"; DefaultProfile = "web"
  }
  "vs2015-x64" = @{
    Kind = "msvc"; Generator = "NMake Makefiles"
    VcVars = "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat"
    VcArch = "x86_amd64"; DefaultProfile = "web"
  }
  "mingw32" = @{
    Kind = "mingw"; Generator = "MinGW Makefiles"
    Bin = (Join-Path $DepsRoot "mingw32\bin"); DefaultProfile = "web"
  }
  "mingw64" = @{
    Kind = "mingw"; Generator = "MinGW Makefiles"
    Bin = (Join-Path $DepsRoot "mingw64\bin"); DefaultProfile = "web"
  }
}

$cfg = $targets[$Target]
if (-not $Profile) { $Profile = $cfg.DefaultProfile }

$required = @(
  (Join-Path $DepsRoot "libuv\include\uv.h"),
  (Join-Path $DepsRoot "openssl\include\openssl\ssl.h"),
  (Join-Path $DepsRoot "llhttp\CMakeLists.txt")
)
foreach ($p in $required) {
  if (-not (Test-Path $p)) {
    throw "dependency kit is incomplete: missing $p. Download the kit zip for '$Target', or build deps/ with packaging/collect-deps.ps1."
  }
}
if ($cfg.Kind -eq "mingw" -and -not (Test-Path (Join-Path $cfg.Bin "gcc.exe"))) {
  throw "MinGW GCC was not found at $($cfg.Bin). The kit must carry the pinned GCC 8.1 toolchain under deps/$([IO.Path]::GetFileName((Split-Path $cfg.Bin -Parent)))."
}

$libuvLib = Get-ChildItem (Join-Path $DepsRoot "libuv\lib") -File -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match '^(uv_a|libuv_a|uv)\.(lib|a)$' } |
  Sort-Object Name | Select-Object -First 1
if (-not $libuvLib) {
  throw "no libuv static library under $DepsRoot\libuv\lib (expected uv_a.lib or libuv_a.a)"
}

$zlibLib = Get-ChildItem (Join-Path $DepsRoot "zlib\lib") -File -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match '(zlibstatic|^libz\.a$|^z\.lib$)' } |
  Sort-Object Name | Select-Object -First 1

$DepsF = $DepsRoot -replace '\\', '/'
$OutF  = (Join-Path $OutRoot $Target) -replace '\\', '/'
$buildDir = Join-Path $KitRoot "build-kit-$Target"
$outDir   = Join-Path $OutRoot $Target
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cmake = "cmake"
$kitCmake = Join-Path $DepsRoot "cmake\bin\cmake.exe"
if (Test-Path $kitCmake) {
  $env:PATH = "$(Split-Path $kitCmake -Parent);$env:PATH"
  $cmake = $kitCmake
}
$cmakeArgs = @(
  "-S", $KitRoot,
  "-B", $buildDir,
  "-G", $cfg.Generator
)
if ($cfg.Arch) { $cmakeArgs += @("-A", $cfg.Arch) }
$cmakeArgs += @(
  "-DCMAKE_BUILD_TYPE=Release",
  "-DUVCPP_FEATURES=$Profile",
  "-DUVCPP_WINVER=0x0601",
  "-DUVCPP_BUILD_TESTS=OFF",
  "-DBUILD_SHARED_LIBS=OFF",
  "-DUVCPP_BUILD_SHARED=ON",
  "-DUVCPP_BUILD_STATIC=OFF",
  "-DUVCPP_USE_SYSTEM_LIBUV=ON",
  "-DUVCPP_BUILD_LIBUV_FROM_SOURCE=OFF",
  "-DLIBUV_INCLUDE_DIR=$DepsF/libuv/include",
  "-DLIBUV_LIB=$($libuvLib.FullName -replace '\\', '/')",
  "-DOPENSSL_ROOT_DIR=$DepsF/openssl",
  "-DOPENSSL_USE_STATIC_LIBS=ON",
  "-DZLIB_ROOT=$DepsF/zlib",
  "-DFETCHCONTENT_SOURCE_DIR_LLHTTP=$DepsF/llhttp",
  "-DCMAKE_PREFIX_PATH=$DepsF;$DepsF/openssl;$DepsF/zlib",
  "-DCMAKE_INSTALL_PREFIX=$OutF"
)
if ($zlibLib) {
  $cmakeArgs += @("-DZLIB_LIBRARY=$($zlibLib.FullName -replace '\\', '/')", "-DZLIB_INCLUDE_DIR=$DepsF/zlib/include")
}
if ($cfg.Kind -eq "msvc") {
  # The kit's OpenSSL/zlib/libuv are static /MT; keep uvcpp on the same CRT.
  $cmakeArgs += "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
} else {
  # Do not make the kit depend on libstdc++/libgcc/winpthread DLLs.
  $cmakeArgs += "-DUVCPP_STATIC_RUNTIME=ON"
}
if ($Profile -eq "full") {
  $nlohmann = Join-Path $DepsRoot "nlohmann"
  if (Test-Path $nlohmann) {
    $cmakeArgs += "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$nlohmann"
  }
}

if ($cfg.Kind -eq "msvc") {
  $bits = if ($Target -like "*-x64") { "64" } else { "32" }
  $osslStatic = Join-Path $DepsRoot "openssl\lib\VC\static"
  $sslMT = Join-Path $osslStatic "libssl${bits}MT.lib"
  $cryptoMT = Join-Path $osslStatic "libcrypto${bits}MT.lib"
  if ((Test-Path $sslMT) -and (Test-Path $cryptoMT)) {
    $cmakeArgs += @(
      "-DSSL_EAY_RELEASE=$($sslMT -replace '\\','/')",
      "-DLIB_EAY_RELEASE=$($cryptoMT -replace '\\','/')",
      "-DSSL_EAY_DEBUG=$((Join-Path $osslStatic "libssl${bits}MTd.lib") -replace '\\','/')",
      "-DLIB_EAY_DEBUG=$((Join-Path $osslStatic "libcrypto${bits}MTd.lib") -replace '\\','/')"
    )
  }
  # VS2015 + a newer Windows SDK UCRT needs _mm_loadu_si64 (added after 19.00).
  #
  # Pass the shim through the CL environment variable, not through
  # -DCMAKE_CXX_FLAGS: assigning that cache entry replaces CMake's MSVC
  # defaults (/DWIN32 /D_WINDOWS /W3 /GR /EHsc) instead of extending them.
  # Without /EHsc the library still compiles, but every C++ translation unit
  # warns C4530/C4577 and is built without unwind semantics, so destructors
  # are not run when an exception propagates. The batch file below appends
  # the switch to CL after vcvarsall, which is what the manual Win7 build
  # (and a consumer of this kit) has to do as well.
  $shim = Join-Path $PSScriptRoot "msvc2015-sdk-compat.h"
  if (Test-Path $shim) {
    $shimPath = ($shim -replace '\\','/')
  }
  # If the only installed Windows 10 SDK carries rc.exe/mt.exe, point CMake at it.
  $sdkArch = if ($bits -eq "64") { "x64" } else { "x86" }
  $sdkDir = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName $sdkArch } |
            Where-Object { Test-Path (Join-Path $_ "rc.exe") } |
            Select-Object -First 1
  if ($sdkDir) {
    $cmakeArgs += @("-DCMAKE_RC_COMPILER=$((Join-Path $sdkDir 'rc.exe') -replace '\\','/')",
                    "-DCMAKE_MT=$((Join-Path $sdkDir 'mt.exe') -replace '\\','/')")
  }
}

function Quote-Arg([string]$a) {
  if ($a -match '[\s";]') { return '"' + $a.Replace('"', '\"') + '"' }
  return $a
}

if ($cfg.Kind -eq "msvc") {
  if (-not (Test-Path $cfg.VcVars)) {
    throw "Visual Studio environment not found: $($cfg.VcVars). Install the matching VS/VC++ toolset or edit build-kit\build.ps1."
  }
  $bat = Join-Path $env:TEMP ("uvcpp-build-" + [guid]::NewGuid().ToString("N") + ".bat")
  $lines = @(
    "@echo off",
    "call `"$($cfg.VcVars)`" $($cfg.VcArch)",
    "if errorlevel 1 exit /b 1"
  )
  if ($shimPath) {
    # Keep CMake's own MSVC flags; just force-include the SDK shim.
    $lines += "set `"CL=%CL% /FI`"$shimPath`"`""
  }
  $lines += @(
    "$(Quote-Arg $cmake) $((@($cmakeArgs) | ForEach-Object { Quote-Arg $_ }) -join ' ')",
    "if errorlevel 1 exit /b 1",
    "$(Quote-Arg $cmake) --build `"$buildDir`"",
    "if errorlevel 1 exit /b 1",
    "$(Quote-Arg $cmake) --install `"$buildDir`"",
    "if errorlevel 1 exit /b 1"
  )
  Set-Content -Path $bat -Value $lines -Encoding ASCII
  & cmd.exe /c "`"$bat`""
  if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
  Remove-Item $bat -Force
  Write-Host "built: $outDir"
} else {
  $env:PATH = "$($cfg.Bin);$env:PATH"
  & $cmake @cmakeArgs
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
  & $cmake --build $buildDir --parallel 4
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  & $cmake --install $buildDir
  if ($LASTEXITCODE -ne 0) { throw "install failed" }
  Write-Host "built: $outDir"
}

# The public headers include <uv.h>, <openssl/ssl.h> and (when gzip is on)
# <zlib.h>. FetchContent installs some of them into the uvcpp prefix, but a
# prebuilt dependency prefix does not, so make out/<target>/include complete.
$inc = Join-Path $outDir "include"
foreach ($sub in @("libuv\include", "openssl\include", "zlib\include")) {
  $p = Join-Path $DepsRoot $sub
  if (Test-Path $p) {
    Copy-Item (Join-Path $p "*") $inc -Recurse -Force
  }
}

# A consumer built with VS2015 against a post-2015 Windows SDK hits the same
# _mm_loadu_si64 problem (the SDK's wchar.h) that we just worked around for
# the library. Ship the shim next to the output and say so once.
if ($cfg.Kind -eq "msvc" -and $shimPath) {
  Copy-Item $shimPath (Join-Path $outDir "msvc2015-sdk-compat.h") -Force
  Write-Host "note: a VS2015 consumer on a newer Windows SDK must compile with"
  Write-Host "      /FI`"$(Join-Path $outDir 'msvc2015-sdk-compat.h')`" (or set CL the same way)."
}
