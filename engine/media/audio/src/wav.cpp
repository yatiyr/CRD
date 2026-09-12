// wav.cpp — GEO-10: the RIFF/WAVE codec (see wav.hpp).

#include <crd/audio/wav.hpp>

#include <cstring>

namespace crd::audio
{

namespace
{
    constexpr crd::u16 kFormatPcm        = 1;
    constexpr crd::u16 kFormatFloat      = 3;
    constexpr crd::u16 kFormatExtensible = 0xFFFE;

    [[nodiscard]] crd::u32 rd_u32(const crd::u8* p) noexcept
    {
        crd::u32 v = 0;
        std::memcpy(&v, p, 4);
        return v; // WAV is little-endian; so are our targets
    }
    [[nodiscard]] crd::u16 rd_u16(const crd::u8* p) noexcept
    {
        crd::u16 v = 0;
        std::memcpy(&v, p, 2);
        return v;
    }

    void wr_u32(crd::containers::Array<crd::u8>& b, crd::u32 v)
    {
        crd::u8 raw[4];
        std::memcpy(raw, &v, 4);
        for (crd::u8 x : raw) { b.push_back(x); }
    }
    void wr_u16(crd::containers::Array<crd::u8>& b, crd::u16 v)
    {
        crd::u8 raw[2];
        std::memcpy(raw, &v, 2);
        for (crd::u8 x : raw) { b.push_back(x); }
    }
    void wr_tag(crd::containers::Array<crd::u8>& b, const char* t)
    {
        for (int i = 0; i < 4; ++i) { b.push_back(static_cast<crd::u8>(t[i])); }
    }
} // namespace

WavError wav_decode(crd::containers::ConstSpan<crd::u8> bytes, AudioPcm& out)
{
    out.isamples.clear();
    out.fsamples.clear();
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        return WavError::NotRiffWave;
    }

    crd::u16   format_tag = 0;
    crd::u16   bits       = 0;
    bool       have_fmt   = false;
    crd::usize pos        = 12;
    while (pos + 8 <= bytes.size())
    {
        const crd::u8* ch   = bytes.data() + pos;
        const crd::u32 size = rd_u32(ch + 4);
        const crd::u8* body = ch + 8;
        if (pos + 8 + size > bytes.size()) { return WavError::Malformed; }

        if (std::memcmp(ch, "fmt ", 4) == 0)
        {
            if (size < 16) { return WavError::Malformed; }
            format_tag          = rd_u16(body);
            out.channels        = rd_u16(body + 2);
            out.sample_rate     = rd_u32(body + 4);
            bits                = rd_u16(body + 14);
            if (format_tag == kFormatExtensible)
            {
                // cbSize(22): valid bits + channel mask + SubFormat GUID (first u16 = the real tag)
                if (size < 40) { return WavError::Malformed; }
                format_tag = rd_u16(body + 24);
            }
            if (out.channels == 0 || out.sample_rate == 0) { return WavError::Malformed; }
            have_fmt = true;
        }
        else if (std::memcmp(ch, "data", 4) == 0)
        {
            if (!have_fmt) { return WavError::MissingFmt; }
            if (format_tag == kFormatPcm)
            {
                if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
                {
                    return WavError::UnsupportedFormat;
                }
                const crd::u32   bytes_per = bits / 8U;
                const crd::usize count     = size / bytes_per;
                if (count % out.channels != 0) { return WavError::Malformed; }
                out.bits_per_sample = bits;
                out.isamples.reserve(count);
                for (crd::usize i = 0; i < count; ++i)
                {
                    const crd::u8* s = body + i * bytes_per;
                    crd::i32       v = 0;
                    switch (bits)
                    {
                    case 8: v = static_cast<crd::i32>(s[0]) - 128; break; // WAV 8-bit is UNSIGNED
                    case 16: v = static_cast<crd::i16>(rd_u16(s)); break;
                    case 24:
                    {
                        const crd::u32 u = static_cast<crd::u32>(s[0]) | (static_cast<crd::u32>(s[1]) << 8U) |
                                           (static_cast<crd::u32>(s[2]) << 16U);
                        v = static_cast<crd::i32>(u << 8U) >> 8U; // sign-extend 24 → 32
                        break;
                    }
                    case 32:
                    default: v = static_cast<crd::i32>(rd_u32(s)); break;
                    }
                    out.isamples.push_back(v);
                }
                return out.valid() ? WavError::Ok : WavError::Malformed;
            }
            if (format_tag == kFormatFloat)
            {
                if (bits != 32 && bits != 64) { return WavError::UnsupportedFormat; }
                const crd::u32   bytes_per = bits / 8U;
                const crd::usize count     = size / bytes_per;
                if (count % out.channels != 0) { return WavError::Malformed; }
                out.bits_per_sample = 0;
                out.fsamples.reserve(count);
                for (crd::usize i = 0; i < count; ++i)
                {
                    const crd::u8* s = body + i * bytes_per;
                    if (bits == 32)
                    {
                        crd::f32 v = 0.0F;
                        std::memcpy(&v, s, 4);
                        out.fsamples.push_back(v);
                    }
                    else
                    {
                        crd::f64 v = 0.0;
                        std::memcpy(&v, s, 8);
                        out.fsamples.push_back(static_cast<crd::f32>(v));
                    }
                }
                return out.valid() ? WavError::Ok : WavError::Malformed;
            }
            return WavError::UnsupportedFormat;
        }
        pos += 8 + size + (size & 1U); // RIFF chunks are word-aligned (pad byte after odd sizes)
    }
    return have_fmt ? WavError::Malformed : WavError::MissingFmt;
}

crd::containers::Array<crd::u8> wav_encode(const AudioPcm& pcm, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> out(alloc);
    if (!pcm.valid()) { return out; }
    const bool as_float = pcm.is_float();
    if (!as_float && pcm.bits_per_sample != 16 && pcm.bits_per_sample != 24) { return out; }

    const crd::u16 real_bits  = as_float ? 32U : pcm.bits_per_sample; // float masters are 32-bit IEEE
    const crd::u32 bytes_per  = real_bits / 8U;
    const crd::usize count    = as_float ? pcm.fsamples.size() : pcm.isamples.size();
    const crd::u32 data_size  = static_cast<crd::u32>(count * bytes_per);
    const crd::u32 block      = static_cast<crd::u32>(pcm.channels) * bytes_per;

    wr_tag(out, "RIFF");
    wr_u32(out, 4 + (8 + 16) + (8 + data_size) + (data_size & 1U));
    wr_tag(out, "WAVE");
    wr_tag(out, "fmt ");
    wr_u32(out, 16);
    wr_u16(out, as_float ? kFormatFloat : kFormatPcm);
    wr_u16(out, pcm.channels);
    wr_u32(out, pcm.sample_rate);
    wr_u32(out, pcm.sample_rate * block);
    wr_u16(out, static_cast<crd::u16>(block));
    wr_u16(out, real_bits);
    wr_tag(out, "data");
    wr_u32(out, data_size);
    for (crd::usize i = 0; i < count; ++i)
    {
        if (as_float)
        {
            crd::u8 raw[4];
            std::memcpy(raw, &pcm.fsamples[i], 4);
            for (crd::u8 x : raw) { out.push_back(x); }
        }
        else
        {
            const crd::i32 v = pcm.isamples[i];
            out.push_back(static_cast<crd::u8>(v & 0xFF));
            out.push_back(static_cast<crd::u8>((v >> 8) & 0xFF));
            if (bytes_per == 3) { out.push_back(static_cast<crd::u8>((v >> 16) & 0xFF)); }
        }
    }
    if ((data_size & 1U) != 0) { out.push_back(0); } // the RIFF pad byte
    return out;
}

void pcm_to_f32(const AudioPcm& pcm, crd::containers::Array<crd::f32>& out)
{
    out.clear();
    if (pcm.is_float())
    {
        out.reserve(pcm.fsamples.size());
        for (crd::f32 v : pcm.fsamples) { out.push_back(v); }
        return;
    }
    const crd::f32 scale = 1.0F / static_cast<crd::f32>(1U << (pcm.bits_per_sample - 1U));
    out.reserve(pcm.isamples.size());
    for (crd::i32 v : pcm.isamples) { out.push_back(static_cast<crd::f32>(v) * scale); }
}

} // namespace crd::audio
