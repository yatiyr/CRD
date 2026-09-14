# Dependency and license manifest (GENERATED -- do not edit)

> Emitted by `scripts/gen_license_manifest.py` from [cmake/pins.json](../../cmake/pins.json), the registry
> that names every external input once with its version, source, SHA-256 and license
> ([pinned inputs](../design/pinned-inputs.md)). Regenerate with `python scripts/gen_license_manifest.py`;
> `--check` fails on drift. A manifest supplements review, it does not replace it: a new dependency is a
> registry edit, a license reading and a reviewed commit ([contribution routes](../CONTRIBUTING.md)).

- packages: **9** · tools: **11** · workflow actions: **7** · runner images: **3** · unpinned apt packages: **12**
- Demo assets keep their own terms: [assets/source/LICENSES.md](../../assets/source/LICENSES.md).
- `external/` is git-ignored and never shipped: locally built peer oracles for benchmarks, each under its
  upstream license, outside this registry and outside every build of the engine.

## Packages (CPM, commit-addressed archives, verified before extraction)

| Package | Version | License | Source | Archive |
|---|---|---|---|---|
| Catch2 | 3.7.1 | BSL-1.0 | catchorg/Catch2 @ `v3.7.1` (fa43b77429ba) | `Catch2-fa43b774.tar.gz` |
| glfw | 3.4 | Zlib | glfw/glfw @ `3.4` (7b6aead9fb88) | `glfw-7b6aead9.tar.gz` |
| zstd | 1.5.5 | BSD-3-Clause OR GPL-2.0-only | facebook/zstd @ `v1.5.5` (63779c798237) | `zstd-63779c79.tar.gz` |
| stb | 2c980bb | MIT OR Unlicense | nothings/stb @ `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2c980bb59875) | `stb-2c980bb5.tar.gz` |
| cgltf | 1.14 | MIT | jkuhlmann/cgltf @ `v1.14` (52c23814dbb6) | `cgltf-52c23814.tar.gz` |
| MikkTSpace | 3e895b4 | Zlib | mmikk/MikkTSpace @ `3e895b49d05ea07e4c2133156cfa94369e19e409` (3e895b49d05e) | `MikkTSpace-3e895b49.tar.gz` |
| imgui | 1.92.0-docking | MIT | ocornut/imgui @ `v1.92.0-docking` (adfa5364cd84) | `imgui-adfa5364.tar.gz` |
| eigen | 3.4.0 | MPL-2.0 | gitlab.com/libeigen/eigen @ `3.4.0` | `eigen-3.4.0.tar.gz` |
| OpenBLAS | 0.3.27 | BSD-3-Clause | OpenMathLib/OpenBLAS @ `v0.3.27` (ce3f668c992c) | `OpenBLAS-ce3f668c.tar.gz` |

## Tools (installers, SDK members and helpers, verified by digest)

| Tool | Version | License | Source | File |
|---|---|---|---|---|
| cpm | 0.40.2 | MIT | github.com | `CPM_0.40.2.cmake` |
| vulkan-sdk-windows | 1.4.341.1 | LunarG Vulkan SDK License Agreement | sdk.lunarg.com | `vulkansdk-windows-X64-1.4.341.1.exe` |
| vulkan-sdk-linux | 1.4.341.1 | LunarG Vulkan SDK License Agreement | sdk.lunarg.com | `vulkansdk-linux-x86_64-1.4.341.1.tar.xz` |
| vulkan-headers | vulkan-sdk-1.4.341.0 | Apache-2.0 OR MIT | KhronosGroup/Vulkan-Headers (b5c8f996196b) | `Vulkan-Headers-b5c8f996.tar.gz` |
| spirv-reflect | vulkan-sdk-1.4.341.0 | Apache-2.0 | KhronosGroup/SPIRV-Reflect (62ca825c6923) | `spirv_reflect.h`, `spirv_reflect.c` |
| spirv-headers | vulkan-sdk-1.4.341.0 | MIT | KhronosGroup/SPIRV-Headers (04f10f650d51) | `include/spirv/unified1/spirv.h` |
| warp | 1.0.20 | Microsoft Software License Terms (Microsoft.Direct3D.WARP) | api.nuget.org | `microsoft.direct3d.warp.1.0.20.nupkg` |
| actionlint | 1.7.12 | MIT | github.com | `actionlint_1.7.12_linux_amd64.tar.gz` |
| sccache-windows | 0.17.0 | Apache-2.0 | github.com | `sccache-v0.17.0-x86_64-pc-windows-msvc.zip` |
| sccache-linux | 0.17.0 | Apache-2.0 | github.com | `sccache-v0.17.0-x86_64-unknown-linux-musl.tar.gz` |
| llvm | 20.1.8 | Apache-2.0 WITH LLVM-exception | pinned action KyleMayes/install-llvm-action | installed by the action |

## Workflow actions (pinned to a reviewed commit)

| Action | Tag | Commit |
|---|---|---|
| actions/checkout | `v4` | `11d5960a326750d5838078e36cf38b85af677262` |
| actions/setup-python | `v5` | `a26af69be951a213d495a4c3e4e4022e16d87065` |
| actions/cache | `v4` | `0057852bfaa89a56745cba8c7296529d2fc39830` |
| actions/upload-artifact | `v4` | `ea165f8d65b6e75b540449e92b4886f43607fa02` |
| seanmiddleditch/gha-setup-ninja | `v5` | `96bed6edff20d1dd61ecff9b75cc519d516e6401` |
| ilammy/msvc-dev-cmd | `v1` | `0b201ec74fa43914dc39ae48a89fd1d8cb592756` |
| KyleMayes/install-llvm-action | `v2.0.9` | `ebc0426251bc40c7cd31162802432c68818ab8f0` |

## Runner images

| Lane | Image |
|---|---|
| windows | `windows-2025` |
| linux | `ubuntu-24.04` |
| windows-native | `windows-2025-vs2026` |

## Unpinned inputs

OS packages of the Linux image, recorded with their installed versions in every Linux lane census: `libvulkan-dev`, `glslang-tools`, `libshaderc-dev`, `libwayland-dev`, `wayland-protocols`, `libxkbcommon-dev`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `mesa-common-dev`.

OS-provided by the pinned runner image; the Linux lanes record the installed versions with dpkg-query into the lane census. The MSVC toolset comes from the pinned Windows image and is recorded by the evidence bundle.
