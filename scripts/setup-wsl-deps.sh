#!/usr/bin/env bash
# scripts/setup-wsl-deps.sh
#
# One-time setup inside WSL2 (Ubuntu 24.04+) so `linux-gcc-*` CMake presets
# build identically to GitHub Actions CI. Runs apt-get for the same package
# list as `.github/workflows/ci.yml` and fetches SPIRV-Reflect (which Ubuntu's
# libvulkan-dev does not ship). Idempotent — safe to re-run.
#
# Usage (from inside WSL):
#     bash /mnt/d/Dev/cerid/scripts/setup-wsl-deps.sh
#
# After it finishes, drive builds from PowerShell on Windows via:
#     ./scripts/wsl-build.ps1 linux-gcc-debug
set -euo pipefail

# --- 1. apt packages -------------------------------------------------------
# Mirror of .github/workflows/ci.yml `Install system dependencies` step,
# plus build-essential / cmake / ninja-build (CI gets ninja from the
# seanmiddleditch/gha-setup-ninja action; we install via apt).
PACKAGES=(
    build-essential
    cmake
    ninja-build
    libvulkan-dev
    glslang-tools
    libshaderc-dev
    libwayland-dev
    wayland-protocols
    libxkbcommon-dev
    libx11-dev
    libxrandr-dev
    libxinerama-dev
    libxcursor-dev
    libxi-dev
    mesa-common-dev
)

echo "[setup-wsl-deps] apt update + install ${#PACKAGES[@]} packages"
sudo apt-get update -q
sudo apt-get install -y "${PACKAGES[@]}"

# --- 2. SPIRV-Reflect headers ---------------------------------------------
# Ubuntu's libvulkan-dev does NOT include SPIRV-Reflect. CI fetches it
# manually under /tmp/vulkan-sdk; we use a persistent path so it survives
# WSL restarts. Layout matches what CMake's FindVulkan + the project's
# spirv-reflect handling expect.
SPIRV_VERSION="vulkan-sdk-1.3.290.0"
SDK_ROOT="$HOME/cerid-deps/vulkan-sdk"
SDK_SRC="$SDK_ROOT/Source/SPIRV-Reflect"

if [[ ! -f "$SDK_SRC/spirv_reflect.c" ]]; then
    echo "[setup-wsl-deps] fetching SPIRV-Reflect ${SPIRV_VERSION} -> $SDK_SRC"
    mkdir -p "$SDK_SRC/include/spirv/unified1"
    curl -fsSL \
        "https://raw.githubusercontent.com/KhronosGroup/SPIRV-Reflect/${SPIRV_VERSION}/spirv_reflect.h" \
        -o "$SDK_SRC/spirv_reflect.h"
    curl -fsSL \
        "https://raw.githubusercontent.com/KhronosGroup/SPIRV-Reflect/${SPIRV_VERSION}/spirv_reflect.c" \
        -o "$SDK_SRC/spirv_reflect.c"
    curl -fsSL \
        "https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/${SPIRV_VERSION}/include/spirv/unified1/spirv.h" \
        -o "$SDK_SRC/include/spirv/unified1/spirv.h"
else
    echo "[setup-wsl-deps] SPIRV-Reflect already present at $SDK_SRC"
fi

# --- 3. shell rc export ----------------------------------------------------
# Make VULKAN_SDK available in every interactive shell so `cmake --preset
# linux-gcc-debug` works without manual env wrangling. Idempotent: checked
# by content match, not by line count.
RC_FILE="$HOME/.bashrc"
RC_LINE="export VULKAN_SDK=\"$SDK_ROOT\"  # added by cerid scripts/setup-wsl-deps.sh"
if ! grep -qF "$RC_LINE" "$RC_FILE" 2>/dev/null; then
    echo "[setup-wsl-deps] appending VULKAN_SDK export to $RC_FILE"
    {
        echo ""
        echo "$RC_LINE"
    } >> "$RC_FILE"
else
    echo "[setup-wsl-deps] VULKAN_SDK export already in $RC_FILE"
fi

# --- 4. summary ------------------------------------------------------------
echo ""
echo "[setup-wsl-deps] DONE."
echo "    GCC:        $(gcc --version | head -1)"
echo "    CMake:      $(cmake --version | head -1)"
echo "    Ninja:      $(ninja --version)"
echo "    VULKAN_SDK: $SDK_ROOT"
echo ""
echo "Next: from PowerShell on Windows, run"
echo "    ./scripts/wsl-build.ps1 linux-gcc-debug"
