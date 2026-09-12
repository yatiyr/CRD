// aiff.cpp — GEO-10: the IFF/AIFF codec (see aiff.hpp). Everything is BIG-endian; the sample rate is the
// 80-bit IEEE 754 extended float (sign 1 · exponent 15 · explicit-integer mantissa 64) — parsed exactly.

#include <crd/audio/aiff.hpp>

#include <cstring>

namespace crd::audio
{

namespace
{
    [[nodiscard]] crd::u32 rd_be32(const crd::u8* p) noexcept
    {
        return (static_cast<crd::u32>(p[0]) << 24U) | (static_cast<crd::u32>(p[1]) << 16U) |
               (static_cast<crd::u32>(p[2]) << 8U) | p[3];
    }
    [[nodiscard]] crd::u16 rd_be16(const crd::u8* p) noexcept
    {
        return static_cast<crd::u16>((static_cast<crd::u32>(p[0]) << 8U) | p[1]);
    }

    // 80-bit extended → u32 sample rate (audio rates are small integers; the exact general case still lands
    // on the nearest integer — the field's job here is a RATE, not analysis math)
    [[nodiscard]] crd::u32 rd_extended_rate(const crd::u8* p) noexcept
    {
        const crd::u16 se       = rd_be16(p);
        const bool     negative = (se & 0x8000U) != 0;
        const crd::i32 exponent = static_cast<crd::i32>(se & 0x7FFFU);
        crd::u64       mantissa = 0;
        for (int i = 0; i < 8; ++i) { mantissa = (mantissa << 8U) | p[2 + i]; }
        if (negative || exponent == 0 || mantissa == 0) { return 0; }
        const crd::i32 shift = exponent - 16383 - 63; // value = mantissa × 2^shift
        if (shift > 0) { return shift >= 32 ? 0 : static_cast<crd::u32>(mantissa << static_cast<crd::u32>(shift)); }
        const crd::u32 down = static_cast<crd::u32>(-shift);
        if (down >= 64) { return 0; }
        const crd::u64 v    = mantissa >> down;
        const crd::u64 rem  = mantissa & ((1ULL << down) - 1ULL);
        const crd::u64 half = 1ULL << (down - 1U);
        return static_cast<crd::u32>(v + (rem >= half ? 1U : 0U)); // round to nearest rate
    }

    void wr_be32(crd::containers::Array<crd::u8>& b, crd::u32 v)
    {
        b.push_back(static_cast<crd::u8>(v >> 24U));
        b.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU));
        b.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
        b.push_back(static_cast<crd::u8>(v & 0xFFU));
    }
    void wr_be16(crd::containers::Array<crd::u8>& b, crd::u16 v)
    {
        b.push_back(static_cast<crd::u8>(v >> 8U));
        b.push_back(static_cast<crd::u8>(v & 0xFFU));
    }
    void wr_tag(crd::containers::Array<crd::u8>& b, const char* t)
    {
        for (int i = 0; i < 4; ++i) { b.push_back(static_cast<crd::u8>(t[i])); }
    }

    // u32 rate → 80-bit extended (exact for every integer rate)
    void wr_extended_rate(crd::containers::Array<crd::u8>& b, crd::u32 rate)
    {
        if (rate == 0)
        {
            for (int i = 0; i < 10; ++i) { b.push_back(0); }
            return;
        }
        crd::i32 exponent = 16383 + 63;
        crd::u64 mantissa = static_cast<crd::u64>(rate);
        while ((mantissa & 0x8000000000000000ULL) == 0)
        {
            mantissa <<= 1U;
            --exponent;
        }
        wr_be16(b, static_cast<crd::u16>(exponent));
        for (int i = 7; i >= 0; --i) { b.push_back(static_cast<crd::u8>((mantissa >> (8U * i)) & 0xFFU)); }
    }
} // namespace

AiffError aiff_decode(crd::containers::ConstSpan<crd::u8> bytes, AudioPcm& out)
{
    out.isamples.clear();
    out.fsamples.clear();
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "FORM", 4) != 0) { return AiffError::NotAiff; }
    const bool aifc = std::memcmp(bytes.data() + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(bytes.data() + 8, "AIFF", 4) != 0) { return AiffError::NotAiff; }

    bool       have_comm  = false;
    crd::u16   bits       = 0;
    crd::u32   num_frames = 0;
    crd::usize pos        = 12;
    while (pos + 8 <= bytes.size())
    {
        const crd::u8* ch   = bytes.data() + pos;
        const crd::u32 size = rd_be32(ch + 4);
        const crd::u8* body = ch + 8;
        if (pos + 8 + size > bytes.size()) { return AiffError::Malformed; }

        if (std::memcmp(ch, "COMM", 4) == 0)
        {
            if (size < 18) { return AiffError::Malformed; }
            out.channels    = rd_be16(body);
            num_frames      = rd_be32(body + 2);
            bits            = rd_be16(body + 6);
            out.sample_rate = rd_extended_rate(body + 8);
            if (aifc && size >= 22)
            {
                // AIFC compression type: only NONE / sowt (byte-swapped little-endian... refuse) survive
                if (std::memcmp(body + 18, "NONE", 4) != 0) { return AiffError::UnsupportedFormat; }
            }
            if (out.channels == 0 || out.sample_rate == 0) { return AiffError::Malformed; }
            if (bits != 8 && bits != 16 && bits != 24 && bits != 32) { return AiffError::UnsupportedFormat; }
            have_comm = true;
        }
        else if (std::memcmp(ch, "SSND", 4) == 0)
        {
            if (!have_comm) { return AiffError::MissingComm; }
            if (size < 8) { return AiffError::Malformed; }
            const crd::u32 offset = rd_be32(body);
            const crd::u8* data   = body + 8 + offset;
            const crd::u32 avail  = size - 8 - offset;
            const crd::u32 bytes_per = bits / 8U;
            const crd::usize count   = static_cast<crd::usize>(num_frames) * out.channels;
            if (offset > size - 8 || count * bytes_per > avail) { return AiffError::Malformed; }
            out.bits_per_sample = bits;
            out.isamples.reserve(count);
            for (crd::usize i = 0; i < count; ++i)
            {
                const crd::u8* s = data + i * bytes_per;
                crd::i32       v = 0;
                switch (bits)
                {
                case 8: // AIFF 8-bit is SIGNED (unlike WAV) — two's-complement widen without a char detour
                    v = s[0] >= 128 ? static_cast<crd::i32>(s[0]) - 256 : static_cast<crd::i32>(s[0]);
                    break;
                case 16: v = static_cast<crd::i16>(rd_be16(s)); break;
                case 24:
                {
                    const crd::u32 u = (static_cast<crd::u32>(s[0]) << 16U) |
                                       (static_cast<crd::u32>(s[1]) << 8U) | s[2];
                    v = static_cast<crd::i32>(u << 8U) >> 8U;
                    break;
                }
                case 32:
                default: v = static_cast<crd::i32>(rd_be32(s)); break;
                }
                out.isamples.push_back(v);
            }
            return out.valid() ? AiffError::Ok : AiffError::Malformed;
        }
        pos += 8 + size + (size & 1U); // IFF pads odd chunks
    }
    return have_comm ? AiffError::Malformed : AiffError::MissingComm;
}

crd::containers::Array<crd::u8> aiff_encode(const AudioPcm& pcm, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> out(alloc);
    if (!pcm.valid() || pcm.is_float()) { return out; }
    if (pcm.bits_per_sample != 16 && pcm.bits_per_sample != 24) { return out; }

    const crd::u32 bytes_per = pcm.bits_per_sample / 8U;
    const crd::u32 data_size = static_cast<crd::u32>(pcm.isamples.size() * bytes_per);
    const crd::u32 ssnd_size = 8 + data_size;
    const crd::u32 form_size = 4 + (8 + 18) + (8 + ssnd_size) + (ssnd_size & 1U);

    wr_tag(out, "FORM");
    wr_be32(out, form_size);
    wr_tag(out, "AIFF");
    wr_tag(out, "COMM");
    wr_be32(out, 18);
    wr_be16(out, pcm.channels);
    wr_be32(out, static_cast<crd::u32>(pcm.frame_count()));
    wr_be16(out, pcm.bits_per_sample);
    wr_extended_rate(out, pcm.sample_rate);
    wr_tag(out, "SSND");
    wr_be32(out, ssnd_size);
    wr_be32(out, 0); // offset
    wr_be32(out, 0); // block size
    for (crd::i32 v : pcm.isamples)
    {
        if (bytes_per == 3) { out.push_back(static_cast<crd::u8>((v >> 16) & 0xFF)); }
        out.push_back(static_cast<crd::u8>((v >> 8) & 0xFF));
        out.push_back(static_cast<crd::u8>(v & 0xFF));
    }
    if ((ssnd_size & 1U) != 0) { out.push_back(0); }
    return out;
}

} // namespace crd::audio
