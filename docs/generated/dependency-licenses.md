# Dependency and license manifest (GENERATED -- do not edit)

> Emitted by `scripts/gen_license_manifest.py` from [cmake/pins.json](../../cmake/pins.json), the registry
> that names every external input once with its version, source, SHA-256 and license
> ([pinned inputs](../design/pinned-inputs.md)). Regenerate with `python scripts/gen_license_manifest.py`;
> `--check` fails on drift. A manifest supplements review, it does not replace it: a new dependency is a
> registry edit, a license reading and a reviewed commit ([contribution routes](../CONTRIBUTING.md)).

- packages: **11** · tools: **21** · workflow actions: **5** · runner images: **5** · unpinned apt packages: **21**
- Demo assets keep their own terms: [assets/source/LICENSES.md](../../assets/source/LICENSES.md).
- `external/` is git-ignored and never shipped: locally built peer oracles for benchmarks, each under its
  upstream license, outside this registry and outside every build of the engine.

## Packages (CPM, commit-addressed archives, verified before extraction)

| Package | Version | License | Source | Archive |
|---|---|---|---|---|
| Catch2 | 3.7.1 | BSL-1.0 | catchorg/Catch2 @ `v3.7.1` (fa43b77429ba) | `Catch2-fa43b774.tar.gz` |
| glfw | 3.4 | Zlib | glfw/glfw @ `3.4` (7b6aead9fb88) | `glfw-7b6aead9.tar.gz` |
| zstd | 1.5.7 | BSD-3-Clause OR GPL-2.0-only | facebook/zstd @ `v1.5.7` (f8745da6ff1a) | `zstd-f8745da6.tar.gz` |
| stb | 2c980bb | MIT OR Unlicense | nothings/stb @ `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2c980bb59875) | `stb-2c980bb5.tar.gz` |
| cgltf | 1.14 | MIT | jkuhlmann/cgltf @ `v1.14` (52c23814dbb6) | `cgltf-52c23814.tar.gz` |
| MikkTSpace | 3e895b4 | Zlib | mmikk/MikkTSpace @ `3e895b49d05ea07e4c2133156cfa94369e19e409` (3e895b49d05e) | `MikkTSpace-3e895b49.tar.gz` |
| imgui | 1.92.0-docking | MIT | ocornut/imgui @ `v1.92.0-docking` (adfa5364cd84) | `imgui-adfa5364.tar.gz` |
| eigen | 3.4.0 | MPL-2.0 | gitlab.com/libeigen/eigen @ `3.4.0` | `eigen-3.4.0.tar.gz` |
| OpenBLAS | 0.3.27 | BSD-3-Clause | OpenMathLib/OpenBLAS @ `v0.3.27` (ce3f668c992c) | `OpenBLAS-ce3f668c.tar.gz` |
| boost_math | 1.92.0 | BSL-1.0 | boostorg/math @ `boost-1.92.0` (f12be767c04d) | `math-f12be767.tar.gz` |
| tracy | 0.14.1 | BSD-3-Clause | wolfpld/tracy @ `v0.14.1` (30997d5ca6bb) | `tracy-30997d5c.tar.gz` |

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
| sde-windows | 10.13.1 | Intel Simplified Software License (Licenses/; third-party programs in Licenses/third-party-programs.txt) | downloadmirror.intel.com | `sde-external-10.13.1-2026-07-28-win.tar.xz` |
| sde-linux | 10.13.1 | Intel Simplified Software License (Licenses/; third-party programs in Licenses/third-party-programs.txt) | downloadmirror.intel.com | `sde-external-10.13.1-2026-07-28-lin.tar.xz` |
| mold-linux | 3.0.0 | MIT | github.com | `mold-3.0.0-x86_64-linux.tar.gz` |
| clang-build-analyzer | 1.6.0 | Unlicense | aras-p/ClangBuildAnalyzer (bae0cb488cce) | `ClangBuildAnalyzer-bae0cb48.tar.gz` |
| clang-tidy-linux | 20.1.0 | Apache-2.0 WITH LLVM-exception AND Apache-2.0 | files.pythonhosted.org | `clang_tidy-20.1.0-py2.py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl` |
| tracy-capture-windows | 0.14.1 | BSD-3-Clause | github.com | `windows-0.14.1.zip` |
| tracy-csvexport-windows | 0.14.1 | BSD-3-Clause | github.com | `windows-0.14.1.zip` |
| ninja-windows | 1.11.1 | Apache-2.0 | github.com | `ninja-win.zip` |
| ninja-linux | 1.11.1 | Apache-2.0 | github.com | `ninja-linux.zip` |
| llvm | 20.1.8 | Apache-2.0 WITH LLVM-exception | github.com | `clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz` |
| genmc | 0.19.0 | Apache-2.0 OR MIT | MPI-SWS/genmc (9f6c4c0772d0) | `genmc-9f6c4c07.tar.gz` |

## Workflow actions (pinned to a reviewed commit)

| Action | Tag | Commit |
|---|---|---|
| actions/checkout | `v7.0.1` | `3d3c42e5aac5ba805825da76410c181273ba90b1` |
| actions/setup-python | `v7.1.0` | `6d4fd22b20465ccc0f7354476588e8df7c9ca0e0` |
| actions/cache | `v6.1.0` | `55cc8345863c7cc4c66a329aec7e433d2d1c52a9` |
| actions/upload-artifact | `v7.0.2` | `cf430e030ddbb5b0abf93d22962f4752f3646cd9` |
| actions/github-script | `v8` | `ed597411d8f924073f98dfc5c65a23a2325f34cd` |

## Runner images

| Lane | Image |
|---|---|
| windows | `windows-2025` |
| linux | `ubuntu-24.04` |
| windows-native | `windows-2025-vs2026` |
| linux-arm64 | `ubuntu-24.04-arm` |
| windows-arm64 | `windows-11-arm` |

## Unpinned inputs

OS packages of the Linux image, recorded with their installed versions in every Linux lane census: `libvulkan-dev`, `glslang-tools`, `libshaderc-dev`, `libwayland-dev`, `wayland-protocols`, `libxkbcommon-dev`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `mesa-common-dev`, `mesa-vulkan-drivers`, `gdb`, `llvm-20-dev`, `clang-20`, `g++-14`, `libedit-dev`, `ninja-build`, `clang`, `libclang-rt-18-dev`.

OS-provided by the pinned runner image; the Linux lanes record the installed versions with dpkg-query into the lane census (llvm-20-dev, clang-20, g++-14 and libedit-dev only on the linux-clang-tsan entry, for the GenMC build; ninja-build, clang and libclang-rt-18-dev only on the linux-arm64 lanes, whose image runs no x86-64 pinned Ninja). The MSVC toolset comes from the pinned Windows image and is recorded by the evidence bundle.
