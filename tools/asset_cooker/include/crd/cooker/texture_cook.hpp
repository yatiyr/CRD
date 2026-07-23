#pragma once

// texture_cook.hpp — GEO-3 stage 2b: the SHARED texture cook core. Both texture consumers ride it — the standalone
// image handler (texture.cpp: .png/.jpg/.tga/.bmp sources) and the glTF decompose (mesh_wave1.cpp: embedded/referenced
// images → extra TXTR artifacts) — so there is exactly ONE mip/encode path in the engine.
//
// The color-space contract (the classic silent bug made structural):
//   • sRGB content (albedo/emissive — COLOR) cooks with format RGBA8UnormSrgb and its mips are filtered in LINEAR
//     space (decode sRGB → f32 linear → box filter → re-encode). Filtering sRGB bytes directly is the classic
//     mip-darkening bug: sRGB(avg_linear(black,white)) = 188, avg_byte(black,white) = 127.
//   • Linear DATA (normal/metallicRoughness/occlusion/masks) cooks RGBA8Unorm; bytes ARE the values, box in place.
//   • Normal maps additionally RENORMALIZE the decoded vector after each downsample (an averaged unit vector is not
//     unit — flat-shading artifacts on glancing mips otherwise).
// Alpha is always linear coverage, never transfer-encoded.
//
// Who decides: the SLOT (glTF baseColor/emissive → sRGB; normal/MR/occlusion → linear) or the `.meta` for standalone
// sources. Nothing ever guesses from pixel contents.

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/ldr_image.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::cooker
{

struct TextureCookOptions
{
    bool srgb       = true;  // sRGB-authored COLOR (the overwhelming default for standalone .png/.jpg sources)
    bool normal_map = false; // renormalize after each downsample; implies linear (a normal map is never sRGB)
};

// Parses the [cook] section of a .meta file body (`srgb = false`, `normal_map = true`). Absent keys keep defaults.
// Pure string processing, testable without the filesystem (the MeshCookOptions pattern).
[[nodiscard]] TextureCookOptions parse_texture_cook_options(crd::containers::StringView meta_text) noexcept;

// Decoded RGBA8 image → full-mip TXTR CRDR artifact (ADR-0042 layout: HEAD + MIP0..N down to 1×1).
// Returns empty on an invalid image.
[[nodiscard]] crd::containers::Array<crd::u8> cook_texture_rgba(const crd::resources::LdrImage&   image,
                                                                const TextureCookOptions&         options,
                                                                const crd::resources::ResourceId& id,
                                                                crd::memory::IAllocator*          alloc);

} // namespace crd::cooker
