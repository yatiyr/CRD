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
# That mount is the 9P bridge, ~2-3× slower than native Linux FS for the
# many small file ops CMake/Ninja do (configure, _deps clones, .o writes,
# stat checks). To dodge the bottleneck WITHOUT cloning a second source
# tree, this script overrides the preset's binaryDir to a path under
# WSL's native ext4 (~/cerid-build/<preset>) so all generated artefacts
# live on native FS. Source files still live on /mnt/d but get cached
# after the first read.
#
# CI on GitHub keeps using the in-tree build/<preset> path — that's
# decided by the CMakePresets.json `binaryDir` and our -B override only
# applies to local invocations.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet(
        'linux-gcc-debug',
        'linux-gcc-release',
        'linux-gcc-relwithdebinfo',
        'linux-gcc-asan',
        'linux-gcc-shipping',
        'linux-gcc-debug-scalar'
    )]
    [string]$Preset,

    [switch]$SkipTests,
    # Reconfigure wipes the build dir before re-running cmake, forcing
    # every CPM dep + engine source to rebuild. Default OFF — the cached
    # build dir on $HOME/cerid-build/<preset> drives down repeat-sweep
    # time from ~5min/preset to ~30s/preset (only changed sources
    # recompile). Enable explicitly for first-run after a CMakePresets
    # change, or when you want a known-clean state.
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

# linux-gcc-shipping ran tests + sandbox starting 2026-05-11 (shipping
# config hardening). All Linux configs run ctest unless -SkipTests.
$skipTestsForPreset = $SkipTests.IsPresent

# Build the bash command. `set -e` so any failing step exits non-zero.
# We source ~/.bashrc to pick up VULKAN_SDK from setup-wsl-deps.sh.
# Native build dir on WSL ext4 — drives the speed-up. The path is
# preset-specific so multiple presets coexist.
$nativeBuildDir = "`$HOME/cerid-build/$Preset"

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
    "BUILD_DIR=$nativeBuildDir"
    'mkdir -p "$BUILD_DIR"'
    'echo "[wsl-build] gcc=$(gcc --version | head -1)"'
    'echo "[wsl-build] cmake=$(cmake --version | head -1)"'
    'echo "[wsl-build] VULKAN_SDK=$VULKAN_SDK"'
    'echo "[wsl-build] CRD_PLATFORM_HEADLESS=$CRD_PLATFORM_HEADLESS"'
    'echo "[wsl-build] BUILD_DIR=$BUILD_DIR (native ext4)"'
    'echo "[wsl-build] ===== configure ====="'
)

if ($Reconfigure) {
    $bashLines += 'rm -rf "$BUILD_DIR"'
    $bashLines += 'mkdir -p "$BUILD_DIR"'
}

# -B overrides the preset's binaryDir; CTest --test-dir does the same.
$bashLines += "cmake --preset $Preset -B `"`$BUILD_DIR`""
$bashLines += 'echo "[wsl-build] ===== build ====="'
$bashLines += "cmake --build `"`$BUILD_DIR`""

if (-not $skipTestsForPreset) {
    $bashLines += 'echo "[wsl-build] ===== ctest ====="'
    # Note: do NOT use --preset here. CMake test-preset machinery honours the
    # preset's binaryDir (`${sourceDir}/build/${presetName}` from base preset)
    # over --test-dir for test enumeration, so a stale leftover build dir on
    # the 9p mount silently shadows the fresh native ext4 build dir we just
    # populated. Passing --test-dir without --preset makes ctest read the
    # CTestTestfile.cmake from exactly where we built it.
    # (Phase 3.1 v1a-sandbox-smoke debugging surfaced this — Linux ctest was
    # running yesterday's enumeration, missing the new eylem tests.)
    $bashLines += "ctest --test-dir `"`$BUILD_DIR`" --output-on-failure"
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
# Per-preset temp file so parallel invocations of this script (one per
# preset) don't race on the same path. The earlier shared-name version
# silently swapped each other's bash scripts when run concurrently.
$tmpWin = Join-Path $tmpDir ".wsl-build-tmp-$Preset.sh"
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
