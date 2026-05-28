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
#   ./scripts/wsl-build.ps1 linux-gcc-debug -BuildJobs 8   # cap Ninja to 8 threads
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
        'linux-gcc-debug-scalar',
        'linux-gcc-debug-sse2'
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
    # Cap Ninja threads (exports CMAKE_BUILD_PARALLEL_LEVEL inside WSL). WSL builds
    # run on the same physical CPU as Windows, so the i9-14900K Raptor Lake
    # stability cap applies here too. 0 = uncapped. full-sweep.ps1 plumbs this in;
    # see CLAUDE.md Troubleshooting "Host instability".
    [int]$BuildJobs = 0,
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
    # Exit-code propagation fix (2026-05-16): wsl.exe in current WSL2 builds
    # does NOT reliably propagate the inner bash script's exit code to its
    # parent (PowerShell $LASTEXITCODE). Verified empirically on 2026-05-16
    # by the v5-close full-sweep: every Linux preset (gcc-debug/asan/release/
    # …) failed mid-build with cc1plus -Werror, yet wsl-build.ps1 reported
    # "OK". To get a trustworthy signal, we write the bash exit code to a
    # sentinel file inside WSL after the script runs; PowerShell reads it
    # back via `wsl.exe cat`. The wsl.exe exit code is used only as a
    # secondary signal (catches "WSL boot failed" / "bash not found").
    "RC_FILE=`"/tmp/cerid-wsl-rc-$Preset`""
    'rm -f "$RC_FILE"'
    '('
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
    # Perf budgets SOFT under the sweep (like CI) — over-budget logs a warning + the
    # measured number instead of hard-aborting a config on host/VM timing variance.
    'export CRD_PERF_BUDGET_SOFT=1'
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

# Cap Ninja threads on the WSL side (same physical CPU as the Windows host).
# `cmake --build` honours CMAKE_BUILD_PARALLEL_LEVEL when no --parallel is passed.
if ($BuildJobs -gt 0) {
    $bashLines += "export CMAKE_BUILD_PARALLEL_LEVEL=$BuildJobs"
    $bashLines += "echo `"[wsl-build] CMAKE_BUILD_PARALLEL_LEVEL=$BuildJobs (Raptor Lake stability cap)`""
}

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
# Close the exit-code-capture subshell (opened above with `(`).
# Outer shell has NO set -e, so it continues after the subshell exits
# non-zero; we capture the subshell's exit code from $?, write it to the
# sentinel file, then propagate it as our own exit code.
$bashLines += ')'
$bashLines += 'BASH_RC=$?'
$bashLines += 'echo "$BASH_RC" > "$RC_FILE"'
$bashLines += 'exit $BASH_RC'

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

$prevEAP = $ErrorActionPreference
try {
    # wsl.exe relays the inner bash's stderr verbatim — and CMake writes its
    # warnings (e.g. zstd's "CMake Deprecation Warning" the first time _deps is
    # configured) to stderr. PowerShell 5.1 surfaces a native command's stderr
    # lines as error records, and under $ErrorActionPreference='Stop' (set above)
    # the first one terminates the script — a spurious failure that has nothing
    # to do with the build. The real failure signal is the wsl.exe exit code, so
    # run the call under 'Continue' and trust $LASTEXITCODE.
    $ErrorActionPreference = 'Continue'
    & wsl.exe -d $Distro -- bash "$tmpWsl"
    $wslCode = $LASTEXITCODE
    # See the comment block at $bashLines: wsl.exe in current WSL2 builds
    # does NOT reliably propagate the inner bash exit code. Sentinel file
    # is authoritative; fall back to wsl.exe code only if the file is
    # missing (e.g. WSL itself failed to boot).
    $rcOut = & wsl.exe -d $Distro -- cat "/tmp/cerid-wsl-rc-$Preset" 2>$null
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($rcOut)) {
        $code = [int]($rcOut.Trim())
    } else {
        Write-Host "[wsl-build.ps1] WARN: sentinel /tmp/cerid-wsl-rc-$Preset missing; falling back to wsl.exe rc=$wslCode" -ForegroundColor Yellow
        $code = $wslCode
    }
}
finally {
    $ErrorActionPreference = $prevEAP
    Remove-Item -Force $tmpWin -ErrorAction SilentlyContinue
}

# Windows-side status sentinel — full-sweep.ps1 reads this directly. Belt-
# and-braces: `exit $code` SHOULD propagate to caller's $LASTEXITCODE for
# .ps1 invoked via `&`, but the 2026-05-16 v5-close sweep proved it does
# NOT propagate reliably when the caller's foreach loop runs many child
# .ps1 calls back-to-back — full-sweep saw $LASTEXITCODE=0 even though
# wsl-build.ps1 had printed "[wsl-build.ps1] FAILED". The status file is
# the authoritative signal callers should trust.
$statusFile = Join-Path $repoRootWin "build/.wsl-build-status-$Preset"
[System.IO.File]::WriteAllText($statusFile, "$code`n")

if ($code -ne 0) {
    Write-Host "[wsl-build.ps1] FAILED (exit code $code)" -ForegroundColor Red
    $global:LASTEXITCODE = $code
    exit $code
}

Write-Host "[wsl-build.ps1] OK" -ForegroundColor Green
$global:LASTEXITCODE = 0
