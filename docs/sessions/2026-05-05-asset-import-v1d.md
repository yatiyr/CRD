# Session: Phase 2.7 v1d — GPU upload + smoke_asset_import + crd-sandbox bootstrap

**Date:** 2026-05-05
**Branch:** main
**Status:** ✅ shipped — all six configurations green

---

## What was built

### 1. RHI interface extensions

Three new abstract methods added to the RHI interface, with Vulkan implementations:

**`CommandBuffer`** (`engine/rhi/include/crd/rhi/command_buffer.hpp`):
- `copy_buffer(src, dst, src_offset, dst_offset, size_bytes)` — maps to `vkCmdCopyBuffer`
- `copy_buffer_to_image(src, dst, ConstSpan<BufferImageCopy>)` — maps to `vkCmdCopyBufferToImage`

**`Queue`** (`engine/rhi/include/crd/rhi/queue.hpp`):
- `submit_and_wait(CommandBuffer&)` — headless submit + `vkQueueWaitIdle`; used by GpuUploader for synchronous one-shot uploads

**`BufferImageCopy`** struct added to `engine/rhi/include/crd/rhi/types.hpp`:
```cpp
struct BufferImageCopy
{
    crd::u64 buffer_offset = 0;
    crd::u32 mip_level     = 0;
    Extent2D extent{};
};
```

**`transition_image` fix** in `vulkan_backend.cpp`: subresource range now uses `VK_REMAINING_MIP_LEVELS` and `VK_REMAINING_ARRAY_LAYERS` instead of hardcoded `1`. This was a latent bug: multi-mip textures would only transition mip 0, causing validation errors on read.

All five fake RHI implementations kept in sync with the new abstract interface:
- `tests/renderer/test_renderer.cpp` (`FakeCommandBuffer`, `FakeQueue`)
- `tests/rhi/test_rhi.cpp` (`FakeCommandBuffer`, `FakeQueue`)
- `runtime/examples/smoke_renderer.cpp` (nested `FakeCommandBuffer` + `FakeQueue` inside `FakeDevice`)
- `runtime/examples/smoke_rhi_api.cpp` (`SmokeCommandBuffer`, `SmokeQueue`)

### 2. GpuUploader (`engine/renderer/`)

New header `include/crd/renderer/gpu_uploader.hpp` and implementation `src/gpu_uploader.cpp`.

**`GpuTexture`** — `{ std::unique_ptr<rhi::Image> image }`. Device-local image, `Sampled | TransferDst` usage. No sampler or image view (Phase 2.8).

**`GpuMesh`** — `{ std::unique_ptr<rhi::Buffer> vertex_buffer; std::unique_ptr<rhi::Buffer> index_buffer; }`. Device-local buffers, `GpuOnly` memory.

**`GpuUploader::upload_texture(TextureResource&, Device&)`**:
1. Sum all mip pixel data sizes.
2. Allocate host-visible staging buffer (`CpuToGpu`, `TransferSrc` usage).
3. `memcpy` all mip levels into staging buffer.
4. Create device-local `rhi::Image` (`GpuOnly`, `Sampled | TransferDst` usage).
5. Record one-shot command buffer: `transition Undefined → TransferDst`, `copy_buffer_to_image` for each mip, `transition TransferDst → ShaderRead`.
6. `queue.submit_and_wait()` — synchronous; staging buffer then destructs.

Only `RGBA8Unorm` format supported; `BC7` asserts false (deferred to Phase 2.8 when GPU texture compression lands).

**`GpuUploader::upload_mesh(MeshResource&, Device&)`**:
1. Allocate two staging buffers (vertex + index, `CpuToGpu`).
2. `memcpy` vertex and index data.
3. Create two device-local buffers (`GpuOnly`, `Vertex|TransferDst` and `Index|TransferDst`).
4. Record one-shot command buffer: `copy_buffer` × 2.
5. `queue.submit_and_wait()`.

### 3. smoke_asset_import.exe

New GPU/window smoke at `runtime/examples/smoke_asset_import.cpp`.

- Creates Vulkan instance (validation disabled for speed) + device. Skips gracefully (exit 0) if no Vulkan physical device is found — allows the smoke to pass in headless CI slots.
- Builds an inline 4×4 RGBA8 checkerboard `TextureResource` (2 mips: 4×4 + 2×2).
- Builds an inline quad `MeshResource` (4 vertices × 48 bytes, 6 u32 indices, 1 primitive).
- Calls `GpuUploader::upload_texture` + `GpuUploader::upload_mesh`.
- `CRD_ASSERT`s that both result handles are non-null.
- Logs success and exits 0.

Registered in `runtime/CMakeLists.txt` as a standalone executable target (not in the headless smoke list).

### 4. crd-sandbox

New executable at `sandbox/` (gated by `CRD_BUILD_SANDBOX=ON`, default ON).

**`main.cpp`**:
- Parses `--headless` flag from argv.
- Creates `Application` with `window.size = {1280, 720}`.
- Creates Vulkan instance + device + swapchain + 2 command buffers.
- Loads `configs/imgui_layer.toml` (graceful skip if absent).
- `push_layer<SandboxLayer>`, `push_overlay<ImGuiLayer>`.
- Explicit render loop: `app.tick()` → `acquire_next_image()` → record (clear + imgui) → `submit` + `present`.
- In headless mode: runs one frame then calls `app.close()`.
- ESC key closes the app.

**`SandboxLayer`** (regular `ILayer`):
- Holds `OrbitCamera` struct and refs to `Application&` + `Swapchain&`.
- `on_update(f64 delta_seconds)`: reads input state, applies orbit/pan/zoom (guarded by `ImGui::GetIO().WantCaptureMouse`), exponential-lerps smoothed camera state.
- `on_render()`: draws fixed-position ImGui window (top-left, 320×160) showing viewport extent, smoothed yaw/pitch/distance/target, and help text.

**`OrbitCamera`** struct:
```
struct OrbitCamera {
    float yaw, pitch, distance;        // target state
    float s_yaw, s_pitch, s_dist;      // smoothed state (rendered from)
    math::Vec3f target, s_target;
};
```
Constants: `kOrbitSpeed=8.0`, `kPanSpeed=0.005`, `kZoomSpeed=0.5`, `kMinDistance=0.1`, `kMaxDistance=500.0`, `kPitchClamp=89° in radians` (uses `std::numbers::pi_v<float>`).

**`sandbox/CMakeLists.txt`**:
- Links: `crd-app`, `crd-config`, `crd-imgui`, `crd-log`, `crd-math`, `crd-rhi`, `crd-rhi-vulkan`.
- `target_include_directories`: `src/` (private) + `imgui_SOURCE_DIR` (SYSTEM private — needed because `crd-imgui` exposes imgui headers as PRIVATE, not PUBLIC/INTERFACE).
- Custom command copies `runtime/configs/imgui_layer.toml` to `${CMAKE_CURRENT_BINARY_DIR}/configs/imgui_layer.toml` at build time.

**Root `CMakeLists.txt`**:
- Added `option(CRD_BUILD_SANDBOX "Build crd-sandbox interactive app" ON)`.
- Added conditional `add_subdirectory(sandbox)` block.

---

## Decisions made

**GpuTexture scope (v1d = image only):** The original plan said `GpuTexture = {Image*, ImageView*, Sampler*}`. Narrowed to `{unique_ptr<Image>}` only. Sampler and image view require per-material descriptor set binding (Phase 2.8); adding them now without a consumer would be dead code.

**smoke_asset_import is GPU-only, not headless:** The smoke exercises the actual GPU upload path. Graceful skip (exit 0 if no Vulkan device) ensures CI headless slots still pass without false negatives.

**sandbox render loop is explicit:** Used `app.tick()` + manual acquire/record/submit/present instead of `app.run()` to give the sandbox direct control over the frame. This pattern matches what a real game loop looks like and makes the sandbox a realistic reference consumer.

**`imgui_SOURCE_DIR` as SYSTEM include:** `crd-imgui` adds imgui as `PRIVATE SYSTEM`. Consumers (sandbox) must add it themselves. This is intentional — ImGui is a debug-only dep and should not leak into non-debug targets.

**`std::numbers::pi_v<float>` for pitch clamp:** win-tidy (`modernize-use-std-numbers`) flagged the raw `3.14159265F` literal. Replaced with the C++20 constant inlined in the expression to avoid a local `constexpr` variable that would trigger naming convention warnings.

---

## Problems encountered and fixed

| Problem | Fix |
|---------|-----|
| `vk_ok()` has `[[nodiscard]]` — return value ignored in `submit_and_wait` | Wrapped in `static_cast<void>(vk_ok(...))` |
| `smoke_renderer.cpp` nested `FakeQueue` inside `FakeDevice` also needed `submit_and_wait` | Added stub to the nested struct (missed in initial grep — different class structure) |
| `smoke_rhi_api.cpp` uses `SmokeCommandBuffer`/`SmokeQueue` naming — not found by grep for `FakeCommandBuffer` | Added stubs on second build pass |
| `sandbox/src/sandbox_layer.cpp` needed `imgui.h` — not found | Added `SYSTEM "${imgui_SOURCE_DIR}"` to sandbox `target_include_directories` |
| `WindowDesc` uses `size = {1280, 720}` not `width`/`height` | Fixed to `app_desc.window.size = {1280, 720}` |
| `Array<BufferImageCopy> regions(nullptr)` — wrong constructor | Changed to default-constructed `Array<BufferImageCopy> regions;` |
| `make_span(regions)` wrong overload for `Array<T>` | Changed to `as_const_span(regions)` |

---

## Six-configuration quality pass

| Config | Build | CTest | Headless Smokes (17) |
|--------|-------|-------|----------------------|
| win-debug | ✅ | 457/457 | ✅ all pass |
| win-relwithdebinfo | ✅ | 457/457 | ✅ all pass |
| win-release | ✅ | 454/454 | ✅ all pass |
| win-asan | ✅ | 457/457 | ✅ all pass |
| win-clang-cl | ✅ | 457/457 | ✅ all pass |
| win-tidy | ✅ | — | — |

win-release is 3 fewer than debug: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`.

Headless smokes verified: `smoke_config`, `smoke_containers`, `smoke_filesystem`, `smoke_frame_clock`, `smoke_jobs`, `smoke_log`, `smoke_math`, `smoke_memory`, `smoke_shader`, `smoke_resources`, `smoke_resources_async`, `smoke_resources_reload`, `smoke_resources_stream`, `smoke_resources_render`, `smoke_texture`, `smoke_mesh`, `smoke_material`.

GPU/window smokes (`smoke_asset_import`, `smoke_rhi_vulkan_bootstrap`, `smoke_renderer`, `smoke_app`, `smoke_imgui_overlay`): verified manually on a Vulkan-capable machine (RTX GPU, Windows 11). All exit 0.

---

## Files changed

**New files:**
- `engine/renderer/include/crd/renderer/gpu_uploader.hpp`
- `engine/renderer/src/gpu_uploader.cpp`
- `runtime/examples/smoke_asset_import.cpp`
- `sandbox/src/main.cpp`
- `sandbox/src/sandbox_layer.hpp`
- `sandbox/src/sandbox_layer.cpp`
- `sandbox/CMakeLists.txt`

**Modified files:**
- `engine/rhi/include/crd/rhi/command_buffer.hpp` — `copy_buffer`, `copy_buffer_to_image` pure virtuals
- `engine/rhi/include/crd/rhi/queue.hpp` — `submit_and_wait` pure virtual
- `engine/rhi/include/crd/rhi/types.hpp` — `BufferImageCopy` struct
- `engine/rhi-vulkan/src/vulkan_backend.cpp` — Vulkan implementations + `VK_REMAINING_MIP_LEVELS` fix
- `tests/renderer/test_renderer.cpp` — `FakeCommandBuffer`/`FakeQueue` stubs
- `tests/rhi/test_rhi.cpp` — `FakeCommandBuffer`/`FakeQueue` stubs
- `runtime/examples/smoke_renderer.cpp` — nested `FakeCommandBuffer`/`FakeQueue` stubs
- `runtime/examples/smoke_rhi_api.cpp` — `SmokeCommandBuffer`/`SmokeQueue` stubs
- `runtime/CMakeLists.txt` — `smoke_asset_import` target
- `CMakeLists.txt` — `CRD_BUILD_SANDBOX` option + sandbox subdirectory

---

## Next session

**Phase 2.7 v1e — `crd-meshgen`:**
- New module `engine/meshgen/` with procedural geometry generators: sphere, icosphere, box, capsule, cylinder, cone, plane, torus
- `smoke_meshgen.exe` (headless): geometry invariants (vertex count, normal direction, UV range)
- Expand `crd-sandbox` `SandboxLayer` with asset browser panel (click-to-switch meshgen shapes + glTF assets)
- Material inspector placeholder panel
