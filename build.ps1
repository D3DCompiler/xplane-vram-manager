# Build VRAM Manager for X-Plane 12 (Windows x64).
#   powershell -ExecutionPolicy Bypass -File build.ps1
# Needs MinGW-w64 (g++ 9 or newer).  Tested with WinLibs GCC 16.1 UCRT:
#   winget install BrechtSanders.WinLibs.POSIX.UCRT
param(
    [string]$Gpp = "",
    [string]$XPlaneRoot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $root "src"
$out  = Join-Path $root "64"
$sdk  = Join-Path $root "SDK"

if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }

if (-not $Gpp) {
    $cands = @()
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { $cands += $cmd.Source }
    $pkgs = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if (Test-Path $pkgs) {
        Get-ChildItem $pkgs -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "*WinLibs*" } |
            ForEach-Object {
                Get-ChildItem $_.FullName -Recurse -Filter "g++.exe" -ErrorAction SilentlyContinue |
                    ForEach-Object { $cands += $_.FullName }
            }
    }
    $cands += "C:\mingw64\bin\g++.exe"
    $cands += "C:\msys64\mingw64\bin\g++.exe"
    $Gpp = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $Gpp) {
    throw "g++ not found. Install MinGW-w64 (winget install BrechtSanders.WinLibs.POSIX.UCRT) or pass -Gpp <path to g++.exe>."
}
Write-Host "Compiler: $Gpp"

$defs = @(
    "-DIBM=1", "-DAPL=0", "-DLIN=0",
    "-DXPLM200", "-DXPLM210", "-DXPLM300", "-DXPLM301", "-DXPLM302",
    "-DXPLM303", "-DXPLM400", "-DXPLM410", "-DXPLM420",
    "-D_CRT_SECURE_NO_WARNINGS", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN"
)
$srcs = Get-ChildItem $src -Filter *.cpp | ForEach-Object { $_.FullName }
$target = Join-Path $out "win.xpl"

$gargs = @(
    "-std=c++17", "-O2", "-shared", "-m64",
    "-Wall", "-Wno-unused-function", "-Wno-format-security",
    "-o", $target
) + $srcs + @(
    "-I", (Join-Path $sdk "CHeaders\XPLM"),
    "-I", (Join-Path $sdk "CHeaders\Widgets"),
    "-L", (Join-Path $sdk "Libraries\Win")
) + $defs + @(
    "-lXPLM_64", "-lXPWidgets_64", "-ldxgi", "-lpdh", "-lole32",
    "-lopengl32", "-lgdi32",
    "-static", "-static-libgcc", "-static-libstdc++"
)

Write-Host "Compiling $(($srcs | Measure-Object).Count) files -> $target"
& $Gpp @gargs
if ($LASTEXITCODE -ne 0) { throw "build failed with code $LASTEXITCODE" }

$fi = Get-Item $target
Write-Host ("Built {0} ({1:N0} bytes)" -f $fi.FullName, $fi.Length)

# --- distribution folder ---------------------------------------------------
# dist\VRAM-Manager\ is a drop-in copy for users: just the plugin DLL plus a
# short readme, no sources.
$dist = Join-Path $root "dist\VRAM-Manager\64"
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }
Copy-Item $target (Join-Path $dist "win.xpl") -Force
Write-Host "Staged for distribution: $(Join-Path $dist 'win.xpl')"

if ($XPlaneRoot) {
    $dest = Join-Path $XPlaneRoot "Resources\plugins\VRAMManager\64"
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
    Copy-Item $target (Join-Path $dest "win.xpl") -Force
    Write-Host "Installed to $dest"
}

