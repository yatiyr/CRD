# scripts/wsl-build.ps1
#
# Run a Linux CMake preset inside WSL2 from a Windows PowerShell prompt.
# Mirrors the CI .github/workflows/ci.yml configure → build → test sequence
# for the `linux-gcc-*` presets so you can verify CI green locally without
# pushing.
#
# Prereqs (one-time):
#   - WSL2 with Ubuntu 24.04 (verify: `wsl -l -v`)
#   - Inside WSL: `bash /mnt/d/Dev/cerid/scripts/setup-wsl-deps.sh`
#
# Usage:
#   ./scripts/wsl-build.ps1 linux-gcc-debug              # configure + build + test
#   ./scripts/wsl-build.ps1 linux-gcc-asan -SkipTests    # configure + build only
#   ./scripts/wsl-build.ps1 linux-gcc-release -Reconfigure
#
# The Windows path D:\Dev\cerid is accessed via /mnt/d/Dev/cerid from WSL.
# That mount is the 9P bridge, ~2-3× slower than native Linux FS. For
# heavy iteration consider keeping a second checkout in WSL's home.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet(
        'linux-gcc-debug',
        'linux-gcc-release',
        'linux-gcc-relwithdebinfo',
        'linux-gcc-asan',
        'linux-gcc-shipping'
    )]
    [string]$Preset,

    [switch]$SkipTests,
    [switch]$Reconfigure,
    [string]$Distro = 'Ubuntu'
)

$ErrorActionPreference = 'Stop'

# Resolve repo root from this script's location so the path is stable
# regardless of where you invoke it from.
$repoRootWin = (Resolve-Path "$PSScriptRoot/..").Path

# Convert D:\Dev\cerid → /mnt/d/Dev/cerid (lowercase drive, forward slashes).
$drive = $repoRootWin.Substring(0, 1).ToLower()
$tail  = $repoRootWin.Substring(2).Replace('\', '/')
$repoRootWsl = "/mnt/$drive$tail"

# linux-gcc-shipping is configure+build only on CI (no ctest step) — match.
$skipTestsForPreset = $SkipTests -or ($Preset -eq 'linux-gcc-shipping')

# Build the bash command. `set -e` so any failing step exits non-zero.
# We source ~/.bashrc to pick up VULKAN_SDK from setup-wsl-deps.sh.
$bashLines = @(
    'set -euo pipefail'
    # Ubuntu's default ~/.bashrc returns early for non-interactive shells,
    # so the VULKAN_SDK export from setup-wsl-deps.sh is invisible to
    # `bash -s`. Pin the same path the setup script wrote, while still
    # honouring an externally-set VULKAN_SDK if the user has another one.
    ': "${VULKAN_SDK:=$HOME/cerid-deps/vulkan-sdk}"'
    'export VULKAN_SDK'
    # Mirror the CI workflow (.github/workflows/ci.yml) — runs the GLFW null
    # platform path in tests that would otherwise need a real Wayland / X11
    # session. Lets PlatformContext tests pass without a display.
    'export CRD_PLATFORM_HEADLESS=1'
    "cd '$repoRootWsl'"
    'echo "[wsl-build] gcc=$(gcc --version | head -1)"'
    'echo "[wsl-build] cmake=$(cmake --version | head -1)"'
    'echo "[wsl-build] VULKAN_SDK=$VULKAN_SDK"'
    'echo "[wsl-build] CRD_PLATFORM_HEADLESS=$CRD_PLATFORM_HEADLESS"'
    'echo "[wsl-build] ===== configure ====="'
)

if ($Reconfigure) {
    $bashLines += "rm -rf 'build/$Preset'"
}

$bashLines += "cmake --preset $Preset"
$bashLines += 'echo "[wsl-build] ===== build ====="'
$bashLines += "cmake --build --preset $Preset"

if (-not $skipTestsForPreset) {
    $bashLines += 'echo "[wsl-build] ===== ctest ====="'
    $bashLines += "ctest --preset $Preset --output-on-failure"
}

$bashLines += 'echo "[wsl-build] ===== DONE ====="'

$bashScript = ($bashLines -join "`n") + "`n"

Write-Host "[wsl-build.ps1] preset=$Preset distro=$Distro repoWsl=$repoRootWsl" -ForegroundColor Cyan
Write-Host "[wsl-build.ps1] tests=$(if ($skipTestsForPreset) { 'skipped' } else { 'enabled' })" -ForegroundColor Cyan

# Write the bash script to a temp file with LF endings and NO BOM — piping
# strings through `wsl bash -s` from PowerShell injects a UTF-8 BOM that
# bash interprets as part of the first command (`bash: line 1: ﻿set: command
# not found`). Writing bytes directly bypasses that.
$tmpDir = Join-Path $repoRootWin 'build'
if (-not (Test-Path $tmpDir)) { New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null }
$tmpWin = Join-Path $tmpDir '.wsl-build-tmp.sh'
$bashLF = $bashScript -replace "`r`n", "`n"
[System.IO.File]::WriteAllBytes($tmpWin, [System.Text.UTF8Encoding]::new($false).GetBytes($bashLF))
$tmpDriveLower = $tmpWin.Substring(0, 1).ToLower()
$tmpWsl = "/mnt/$tmpDriveLower" + $tmpWin.Substring(2).Replace('\', '/')

try {
    & wsl.exe -d $Distro -- bash "$tmpWsl"
    $code = $LASTEXITCODE
}
finally {
    Remove-Item -Force $tmpWin -ErrorAction SilentlyContinue
}

if ($code -ne 0) {
    Write-Host "[wsl-build.ps1] FAILED (exit code $code)" -ForegroundColor Red
    exit $code
}

Write-Host "[wsl-build.ps1] OK" -ForegroundColor Green
