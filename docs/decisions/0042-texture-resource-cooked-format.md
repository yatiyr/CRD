# ADR-0042 — Texture cooked format + GPU upload strategy

**Status:** Accepted  
**Phase:** 2.7 v1a  
**Tags:** `[resources]` `[renderer]` `[cooker]`

---

## Context

Phase 2.7 introduces `TextureResource` as the first image-data asset type. Two design decisions
are entangled: what byte format the cooker writes into the CRDR artifact, and when/how the CPU
data reaches GPU memory.

### Format options considered

| Option | Pros | Cons |
|--------|------|------|
| Always RGBA8 | Simple decoder (stb_image native output). Always correct. | 4× larger than BC7 on GPU; wastes VRAM and bandwidth. |
| Always BC7 | Optimal GPU footprint. | DirectXTex is Windows-only; Linux CI can't cook BC7. BC7 encode is slow (seconds per texture). |
| RGBA8 default, BC7 opt-in | Both CIs work. Opt-in on platforms that have it. | Two codepaths in the loader; artifact format must carry format tag. |

### Upload options considered

| Option | Pros | Cons |
|--------|------|------|
| Synchronous upload in loader (Device* in LoadContext) | Simple call site. One step from artifact to GPU. | Couples ILoader to crd-rhi. Violates loader-registry isolation (ADR-0036). Every loader test needs a Vulkan instance. |
| CPU-only loader; render path uploads on first use | ILoader stays RHI-free. Loader tests need no GPU. Consistent with ShaderResource (SPIRV lives CPU-side until the pipeline uses it). | Extra indirection; render path must track "uploaded vs not". |
| Explicit upload step (GpuUploader helper) | RHI-free loader; upload is explicit and debuggable. One-shot fence+wait keeps v1 simple. | Caller responsible for calling the uploader before first render use. |

---

## Decision

**Texture cooked format:** RGBA8 by default; BC7 opt-in gated by the CMake option
`CRD_COOK_BC7` (Windows + DirectXTex only, graceful no-op on Linux/macOS). The artifact's
`HEAD` chunk carries a `TextureFormat` byte so the loader always knows what it's reading.
Mip chain is always generated at cook time (box filter, down to 1×1) and stored as separate
`MIPn` chunks.

**GPU upload strategy:** The `ILoader::load()` implementation returns CPU data only
(`TextureResource` holds pixel bytes, no RHI handles). A separate `GpuTextureUploader`
helper (in `crd-renderer`, not `crd-resources`) is called by the render path (which owns
a `Device*`) when a `TextureResource` is first needed for rendering. Upload is synchronous
in v1d (staging buffer + one-shot command buffer + fence wait). Async streaming upload is
deferred to Phase 2.6 v1g's `load_streamed<T>` path.

**Why not Device* in LoadContext:**
- Violates ADR-0036 (crd-resources must not depend on crd-rhi).
- Makes loader unit tests require a Vulkan instance.
- Inconsistent with how ShaderResource works (SPIRV lives CPU-side; the PSO resolver
  creates GPU pipeline objects separately).

---

## Consequences

- `TextureFormat` enum lives in `engine/renderer/include/crd/renderer/texture_resource.hpp`.
- `TextureResource` is a plain data struct, no GPU handles, no RHI types.
- `GpuTextureUploader` + `GpuMeshUploader` (ADR-0043 companion) are helper utilities inside
  `crd-renderer/src/` — not public API.
- Loader unit tests run without a GPU.
- `smoke_asset_import.exe` (v1d) exercises the full CPU→GPU path.
- BC7 is never produced in CI (Linux CI uses stb_image only); BC7 artifacts are an opt-in
  developer workflow, never a hard requirement. All release paths must handle RGBA8.
- A future `DeviceResourceCache` that holds GPU image handles keyed by `ResourceId` is the
  natural extension once hot-reload (v1f) needs to swap GPU resources. That cache lives in
  the render path, not in `crd-resources`.

---

## CRDR artifact layout (`type='TXTR'`)

```
HEAD chunk (12 bytes):
  +0  u32  width
  +4  u32  height
  +8  u32  mip_count
  +12 u8   format  (0=RGBA8Unorm, 1=BC7Unorm, 2=BC7UnormSrgb)
  +13 u8[3] padding

MIP0 chunk: raw pixel data for mip level 0 (full resolution)
MIP1 chunk: raw pixel data for mip level 1
...
MIPn chunk: 1×1 pixel (final mip)
```

FourCCs: `kFourCC_TXTR`, `kFourCC_HEAD`, `kFourCC_MIP0` through `kFourCC_MIP15`.

---

## Alternatives not taken

- **Basis Universal / KTX2** — hardware-agnostic compressed formats. Excellent long-term
  choice. Deferred because the Basis transcoder is a significant dependency and Phase 2.7
  just needs "texture on screen," not optimal compression. Revisit in Phase 3.5 (PBR + IBL).
- **DDS container** — Windows-native. Rejected: non-portable and redundant with CRDR.
- **Upload in a background job** — Desirable for streaming. Out of scope for v1d; the
  `load_streamed<T>` path in v1g is the right place for this.
