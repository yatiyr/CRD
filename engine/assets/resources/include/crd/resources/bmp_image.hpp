#pragma once

// bmp_image.hpp — OUR OWN BMP decoder (Windows-tooling ubiquity). Coverage: BITMAPINFOHEADER (40) + V4 (108) + V5 (124)
// headers · 1/4/8-bit palette · 16/32-bit BI_BITFIELDS (arbitrary contiguous masks) · 24-bit BGR · 32-bit BGRX/BGRA ·
// RLE8 + RLE4 (runs, absolute mode, EOL/EOB/delta escapes) · bottom-up AND top-down rows · 4-byte row padding. OS/2
// core headers (12-byte) are `Unsupported` by name, never mis-decoded.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/ldr_image.hpp>

namespace crd::resources
{

[[nodiscard]] bool     bmp_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;
[[nodiscard]] LdrError bmp_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
