#pragma once

// tga_image.hpp — OUR OWN TGA decoder (the game-art workhorse). Coverage, no gaps within the format's real surface:
// uncompressed + RLE (types 1/2/3/9/10/11) · 8-bit gray · 8-bit palette (16/24/32-bit palette entries) · 16-bit
// A1R5G5B5 · 24-bit BGR · 32-bit BGRA · both vertical origins + the right-to-left flag · footer ignored (v2 extension
// areas carry no pixels). TGA has NO magic — `tga_sniff` is a header-consistency heuristic, so the auto-dispatch tries
// it LAST (ldr_image.hpp).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/ldr_image.hpp>

namespace crd::resources
{

[[nodiscard]] bool     tga_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;
[[nodiscard]] LdrError tga_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
