#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::renderer
{

// On-disk byte values — never reorder (CRDR format spec, ADR-0042).
enum class TextureFormat : crd::u8
{
    RGBA8Unorm   = 0,
    BC7Unorm     = 1,
    BC7UnormSrgb = 2,
};

// One mip level of a texture. Owns its pixel bytes.
// No default constructor: Array<MipLevel> cannot use resize() — use push_back.
struct MipLevel
{
    crd::u32                        width  = 0;
    crd::u32                        height = 0;
    crd::containers::Array<crd::u8> pixels;

    explicit MipLevel(crd::memory::IAllocator* a) : pixels(a) {}

    MipLevel(const MipLevel&)            = delete;
    MipLevel& operator=(const MipLevel&) = delete;
    MipLevel(MipLevel&&)                 = default;
    MipLevel& operator=(MipLevel&&)      = default;
};

// CPU-side cooked texture. mips[0] = full-resolution, mips[N-1] = 1×1.
struct TextureResource
{
    TextureFormat                    format    = TextureFormat::RGBA8Unorm;
    crd::u32                         mip_count = 0;
    crd::containers::Array<MipLevel> mips;

    explicit TextureResource(crd::memory::IAllocator* a) : mips(a) {}

    TextureResource(const TextureResource&)            = delete;
    TextureResource& operator=(const TextureResource&) = delete;
    TextureResource(TextureResource&&)                 = default;
    TextureResource& operator=(TextureResource&&)      = default;
};

} // namespace crd::renderer
