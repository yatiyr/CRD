# TextureResource

CPU-side cooked texture payload for `crd-renderer`. Loaded from `type='TXTR'` CRDR artifacts
by `TextureResourceLoader`. No GPU objects — upload is a separate step handled by `GpuUploader`
(Phase 2.7 v1d).

**Phase 2.7 v1a COMPLETE.**

Lives in: `engine/renderer/`
Depends on: `crd-core`, `crd-memory`, `crd-containers`
Does NOT depend on: `crd-rhi`, `crd-shader`, `crd-resources` (keep loaders free of RHI for test headlessness)

## Status

| Slice | Ships | Status |
|-------|-------|--------|
| v1a   | `TextureFormat` enum, `MipLevel`, `TextureResource`, `TextureResourceLoader`, stb_image texture cooker handler | ✅ shipped 2026-05-04 |
| v1d   | `GpuUploader::upload_texture` → `GpuTexture` (staging-buffer pattern) | ✅ shipped 2026-05-05 |

## Types

### `TextureFormat`

```cpp
// engine/renderer/include/crd/renderer/texture_resource.hpp
enum class TextureFormat : crd::u8
{
    RGBA8Unorm   = 0,   // 4 bytes/pixel; always available
    BC7Unorm     = 1,   // 1 byte/pixel; cook-time opt-in (CRD_COOK_BC7)
    BC7UnormSrgb = 2,   // BC7 with sRGB transfer
};
```

On-disk byte values. Never reorder — CRDR format spec, ADR-0042.

### `MipLevel`

```cpp
struct MipLevel
{
    crd::u32                        width;
    crd::u32                        height;
    crd::containers::Array<crd::u8> pixels;   // tightly packed, format-appropriate

    explicit MipLevel(crd::memory::IAllocator* a);
    // move-only
};
```

### `TextureResource`

```cpp
struct TextureResource
{
    TextureFormat                    format;
    crd::u32                         mip_count;
    crd::containers::Array<MipLevel> mips;    // mips[0] = full-res; mips[N-1] = 1×1

    explicit TextureResource(crd::memory::IAllocator* a);
    // move-only
};
```

## CRDR artifact format (TXTR)

```
artifact type: 'TXTR'
loader version: 1

HEAD chunk (16 bytes):
  width     u32
  height    u32
  mip_count u32
  format    u8   (TextureFormat on-disk byte value)
  pad[3]    u8

MIP0..MIP15 chunks:
  raw pixel bytes, tightly packed
  format RGBA8Unorm: width × height × 4 bytes per mip level
```

All mip levels are always present (`mip_count` entries from MIP0 to MIP{N-1}).
Dimensions halve each level, clamped at 1×1.

## Loader registration

```cpp
#include <crd/renderer/texture_resource_loader.hpp>

// At startup, before any ResourceManager mounts:
crd::renderer::register_texture_loader(&rm);

// Load:
auto h = rm.load_sync<crd::renderer::TextureResource>(texture_id);
const crd::renderer::TextureResource* tex = h.get();
// tex->mip_count, tex->format, tex->mips[0].width / height / pixels
```

## Cooker handler

`.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp` → `type='TXTR'` CRDR artifact.

- stb_image for decode (STBI_rgb_alpha → 4 channels; TGA BGRA→RGBA handled by stb).
- Box-filter mip chain down to 1×1 with O(W×H) ping-pong scratch buffers.
- Optional BC7 compression gated by `CRD_COOK_BC7` CMake option (Windows + DirectXTex only).
- Registered via `register_texture_handler()`, called from `register_builtin_handlers()` in `asset_cooker`.

## GPU upload (v1d)

```cpp
#include <crd/renderer/gpu_uploader.hpp>

crd::renderer::GpuTexture gpu_tex = crd::renderer::GpuUploader::upload_texture(tex, device);
// gpu_tex.image — unique_ptr<rhi::Image> (device-local, sampled | transfer_dst)
// Sampler/ImageView wired in Phase 2.8 (per-material descriptor binding).
```

Upload is synchronous (fence + immediate wait). All mip levels are copied in one staging operation.
Only `TextureFormat::RGBA8Unorm` is supported by the uploader in v1d; `BC7` asserts false (Phase 2.8).

## Session logs

- [v1a — TextureResource + stb_image cooker](../sessions/2026-05-05-asset-import-v1d.md)
