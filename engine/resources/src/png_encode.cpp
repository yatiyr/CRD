// png_encode.cpp — GEO-4 pt 2 (D-007): the baseline PNG encoder. See png_encode.hpp for the contract.

#include <crd/resources/png_encode.hpp>

#include <crd/resources/deflate.hpp>
#include <crd/resources/png_image.hpp> // png_crc32

namespace crd::resources
{
namespace
{

constexpr crd::u32 kMaxDim = 16384U; // the decoder's own cap — what we encode, we must decode

void put_be32(crd::containers::Array<crd::u8>& out, crd::u32 v)
{
    out.push_back(static_cast<crd::u8>((v >> 24U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<crd::u8>(v & 0xFFU));
}

// one chunk: length + type + payload + CRC(type||payload)
void put_chunk(crd::containers::Array<crd::u8>& out, const char type[4], crd::containers::ConstSpan<crd::u8> payload,
               crd::memory::IAllocator* alloc)
{
    put_be32(out, static_cast<crd::u32>(payload.size()));
    crd::containers::Array<crd::u8> crc_input(alloc);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        out.push_back(static_cast<crd::u8>(type[i]));
        crc_input.push_back(static_cast<crd::u8>(type[i]));
    }
    for (crd::usize i = 0; i < payload.size(); ++i)
    {
        out.push_back(payload[i]);
        crc_input.push_back(payload[i]);
    }
    put_be32(out, png_crc32(crd::containers::ConstSpan<crd::u8>(crc_input.data(), crc_input.size())));
}

} // namespace

crd::containers::Array<crd::u8> png_encode_rgba(crd::containers::ConstSpan<crd::u8> rgba, crd::u32 width,
                                                crd::u32 height, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> out(alloc);
    if (width == 0U || height == 0U || width > kMaxDim || height > kMaxDim) { return out; }
    if (rgba.size() != static_cast<crd::usize>(width) * height * 4U) { return out; }

    // signature
    const crd::u8 sig[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
    for (crd::u32 i = 0; i < 8U; ++i) { out.push_back(sig[i]); }

    // IHDR: 8-bit truecolor+alpha, no interlace
    crd::containers::Array<crd::u8> ihdr(alloc);
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.push_back(8U); // bit depth
    ihdr.push_back(6U); // color type: RGBA
    ihdr.push_back(0U); // compression
    ihdr.push_back(0U); // filter method
    ihdr.push_back(0U); // no interlace
    put_chunk(out, "IHDR", crd::containers::ConstSpan<crd::u8>(ihdr.data(), ihdr.size()), alloc);

    // IDAT: filter byte 0 (None) + the raw scanline, per row, zlib-deflated
    crd::containers::Array<crd::u8> raw(alloc);
    raw.reserve(static_cast<crd::usize>(height) * (1U + static_cast<crd::usize>(width) * 4U));
    for (crd::u32 y = 0; y < height; ++y)
    {
        raw.push_back(0U); // filter: None
        const crd::usize row = static_cast<crd::usize>(y) * width * 4U;
        for (crd::usize i = 0; i < static_cast<crd::usize>(width) * 4U; ++i) { raw.push_back(rgba[row + i]); }
    }
    const crd::containers::Array<crd::u8> idat =
        zlib_deflate(crd::containers::ConstSpan<crd::u8>(raw.data(), raw.size()), alloc);
    put_chunk(out, "IDAT", crd::containers::ConstSpan<crd::u8>(idat.data(), idat.size()), alloc);

    put_chunk(out, "IEND", crd::containers::ConstSpan<crd::u8>{}, alloc);
    return out;
}

} // namespace crd::resources
