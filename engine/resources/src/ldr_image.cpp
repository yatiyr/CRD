// ldr_image.cpp — the LDR-codec auto-dispatch. Magic-carrying formats sniff first; magic-less TGA claims only what its
// header-consistency heuristic accepts, and only LAST.

#include <crd/resources/ldr_image.hpp>

#include <crd/resources/bmp_image.hpp>
#include <crd/resources/jpeg_image.hpp>
#include <crd/resources/png_image.hpp>
#include <crd/resources/tga_image.hpp>

namespace crd::resources
{

LdrCodec ldr_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    if (png_sniff(bytes)) { return LdrCodec::Png; }
    if (jpeg_sniff(bytes)) { return LdrCodec::Jpeg; }
    if (bmp_sniff(bytes)) { return LdrCodec::Bmp; }
    if (tga_sniff(bytes)) { return LdrCodec::Tga; } // heuristic — LAST
    return LdrCodec::Unknown;
}

LdrError ldr_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a)
{
    switch (ldr_sniff(bytes))
    {
    case LdrCodec::Png: return png_decode(bytes, out, a);
    case LdrCodec::Jpeg: return jpeg_decode(bytes, out, a);
    case LdrCodec::Bmp: return bmp_decode(bytes, out, a);
    case LdrCodec::Tga: return tga_decode(bytes, out, a);
    default: return LdrError::BadMagic;
    }
}

} // namespace crd::resources
