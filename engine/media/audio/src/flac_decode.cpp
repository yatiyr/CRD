// flac_decode.cpp — GEO-10: the FLAC decoder (see flac.hpp for the surface). Structure follows the spec's
// grammar: STREAMINFO → frames (header · subframes · footer). CRCs run over the RAW frame bytes; MD5 over the
// decoded little-endian PCM. Every reserved code and size contradiction refuses — never a garbage sample.

#include <crd/audio/flac.hpp>

#include "flac_detail.hpp"


namespace crd::audio
{

namespace
{
    using detail::BitReader;

    struct StreamInfo
    {
        crd::u32 sample_rate  = 0;
        crd::u32 channels     = 0;
        crd::u32 bps          = 0;
        crd::u64 total        = 0;
        crd::u8  md5[16]      = {};
        bool     has_md5      = false;
    };

    // fixed-predictor reconstruction: x[i] = residual[i] + prediction from already-decoded x
    void undo_fixed(crd::containers::Array<crd::i64>& x, crd::u32 order)
    {
        for (crd::usize i = order; i < x.size(); ++i)
        {
            switch (order)
            {
            case 0: break;
            case 1: x[i] += x[i - 1]; break;
            case 2: x[i] += 2 * x[i - 1] - x[i - 2]; break;
            case 3: x[i] += 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; break;
            case 4:
            default: x[i] += 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; break;
            }
        }
    }

    // rice-coded residual into x[order..block) — methods 0 (4-bit params) and 1 (5-bit), escape = raw
    [[nodiscard]] bool read_residual(BitReader& br, crd::containers::Array<crd::i64>& x, crd::u32 order,
                                     crd::u32 block)
    {
        const crd::u32 method = static_cast<crd::u32>(br.read_bits(2));
        if (method > 1) { return false; }
        const crd::u32 param_bits = method == 0 ? 4U : 5U;
        const crd::u32 escape     = method == 0 ? 0xFU : 0x1FU;
        const crd::u32 porder     = static_cast<crd::u32>(br.read_bits(4));
        const crd::u32 partitions = 1U << porder;
        if (block % partitions != 0) { return false; }
        const crd::u32 per = block >> porder;
        if (per == 0 || per < order) { return false; } // the first partition sheds the warmup samples

        crd::usize idx = order;
        for (crd::u32 p = 0; p < partitions; ++p)
        {
            const crd::u32 count = p == 0 ? per - order : per;
            const crd::u32 param = static_cast<crd::u32>(br.read_bits(param_bits));
            if (param == escape)
            {
                const crd::u32 raw_bits = static_cast<crd::u32>(br.read_bits(5));
                for (crd::u32 i = 0; i < count; ++i) { x[idx++] = br.read_signed(raw_bits); }
            }
            else
            {
                for (crd::u32 i = 0; i < count; ++i)
                {
                    const crd::u64 q = br.read_unary();
                    const crd::u64 u = (q << static_cast<crd::u64>(param)) | br.read_bits(param);
                    x[idx++]         = static_cast<crd::i64>(u >> 1U) ^ -static_cast<crd::i64>(u & 1U);
                }
            }
            if (!br.ok) { return false; }
        }
        return idx == block;
    }

    [[nodiscard]] bool read_subframe(BitReader& br, crd::containers::Array<crd::i64>& x, crd::u32 block,
                                     crd::u32 bps)
    {
        if (br.read_bits(1) != 0) { return false; } // the mandatory zero pad bit
        const crd::u32 type   = static_cast<crd::u32>(br.read_bits(6));
        crd::u32       wasted = 0;
        if (br.read_bits(1) != 0) { wasted = br.read_unary() + 1; }
        if (!br.ok || wasted >= bps) { return false; }
        const crd::u32 eff = bps - wasted;

        x.resize(block);
        if (type == 0) // CONSTANT
        {
            const crd::i64 v = br.read_signed(eff);
            for (crd::usize i = 0; i < block; ++i) { x[i] = v; }
        }
        else if (type == 1) // VERBATIM
        {
            for (crd::usize i = 0; i < block; ++i) { x[i] = br.read_signed(eff); }
        }
        else if (type >= 8 && type <= 12) // FIXED order 0-4
        {
            const crd::u32 order = type - 8;
            if (order > block) { return false; }
            for (crd::u32 i = 0; i < order; ++i) { x[i] = br.read_signed(eff); }
            if (!read_residual(br, x, order, block)) { return false; }
            undo_fixed(x, order);
        }
        else if (type >= 32) // LPC order 1-32
        {
            const crd::u32 order = type - 31;
            if (order > block) { return false; }
            for (crd::u32 i = 0; i < order; ++i) { x[i] = br.read_signed(eff); }
            const crd::u32 precision = static_cast<crd::u32>(br.read_bits(4)) + 1;
            if (precision == 16) { return false; } // 1111 is invalid per spec
            const crd::i64 shift = br.read_signed(5);
            if (shift < 0) { return false; }
            crd::i64 coef[32];
            for (crd::u32 i = 0; i < order; ++i) { coef[i] = br.read_signed(precision); }
            if (!read_residual(br, x, order, block)) { return false; }
            for (crd::usize i = order; i < block; ++i)
            {
                crd::i64 acc = 0;
                for (crd::u32 j = 0; j < order; ++j) { acc += coef[j] * x[i - 1 - j]; }
                x[i] += acc >> static_cast<crd::u32>(shift);
            }
        }
        else
        {
            return false; // reserved subframe types
        }
        if (!br.ok) { return false; }
        if (wasted > 0)
        {
            for (crd::usize i = 0; i < block; ++i)
            {
                x[i] = static_cast<crd::i64>(static_cast<crd::u64>(x[i]) << wasted);
            }
        }
        return true;
    }
} // namespace

FlacError flac_decode(crd::containers::ConstSpan<crd::u8> bytes, AudioPcm& out)
{
    out.isamples.clear();
    out.fsamples.clear();
    if (bytes.size() < 42 || std::memcmp(bytes.data(), "fLaC", 4) != 0) { return FlacError::NotFlac; }

    // metadata blocks — STREAMINFO must be first
    StreamInfo info;
    crd::usize pos     = 4;
    bool       last    = false;
    bool       first   = true;
    while (!last)
    {
        if (pos + 4 > bytes.size()) { return FlacError::Malformed; }
        const crd::u8* h = bytes.data() + pos;
        last             = (h[0] & 0x80U) != 0;
        const crd::u32 type = h[0] & 0x7FU;
        const crd::u32 len  = (static_cast<crd::u32>(h[1]) << 16U) | (static_cast<crd::u32>(h[2]) << 8U) | h[3];
        pos += 4;
        if (pos + len > bytes.size()) { return FlacError::Malformed; }
        if (first)
        {
            if (type != 0 || len != 34) { return FlacError::NotFlac; }
            const crd::u8* si = bytes.data() + pos;
            info.sample_rate  = (static_cast<crd::u32>(si[10]) << 12U) | (static_cast<crd::u32>(si[11]) << 4U) |
                               (si[12] >> 4U);
            info.channels = ((si[12] >> 1U) & 0x7U) + 1;
            info.bps      = (((si[12] & 0x1U) << 4U) | (si[13] >> 4U)) + 1;
            info.total    = (static_cast<crd::u64>(si[13] & 0x0FU) << 32U) |
                         (static_cast<crd::u64>(si[14]) << 24U) | (static_cast<crd::u64>(si[15]) << 16U) |
                         (static_cast<crd::u64>(si[16]) << 8U) | si[17];
            std::memcpy(info.md5, si + 18, 16);
            for (int i = 0; i < 16; ++i)
            {
                if (info.md5[i] != 0) { info.has_md5 = true; }
            }
            first = false;
        }
        pos += len;
    }
    if (info.sample_rate == 0 || info.channels == 0 || info.bps == 0) { return FlacError::Malformed; }
    if (info.bps != 8 && info.bps != 16 && info.bps != 24 && info.bps != 32)
    {
        return FlacError::UnsupportedFormat;
    }

    out.sample_rate     = info.sample_rate;
    out.channels        = static_cast<crd::u16>(info.channels);
    out.bits_per_sample = static_cast<crd::u16>(info.bps);

    detail::Md5 md5;
    crd::memory::IAllocator*         alloc = out.isamples.allocator();
    crd::containers::Array<crd::i64> ch[8] = {
        crd::containers::Array<crd::i64>(alloc), crd::containers::Array<crd::i64>(alloc),
        crd::containers::Array<crd::i64>(alloc), crd::containers::Array<crd::i64>(alloc),
        crd::containers::Array<crd::i64>(alloc), crd::containers::Array<crd::i64>(alloc),
        crd::containers::Array<crd::i64>(alloc), crd::containers::Array<crd::i64>(alloc)};

    // frames until the bytes run out
    while (pos < bytes.size())
    {
        const crd::usize frame_start = pos;
        BitReader        br{bytes.data(), bytes.size(), pos, 0, true};

        const crd::u32 sync = static_cast<crd::u32>(br.read_bits(14));
        if (sync != 0x3FFE) { return FlacError::Malformed; }
        if (br.read_bits(1) != 0) { return FlacError::Malformed; } // reserved
        (void)br.read_bits(1);                                     // blocking strategy
        const crd::u32 bs_code   = static_cast<crd::u32>(br.read_bits(4));
        const crd::u32 rate_code = static_cast<crd::u32>(br.read_bits(4));
        const crd::u32 ch_code   = static_cast<crd::u32>(br.read_bits(4));
        const crd::u32 bps_code  = static_cast<crd::u32>(br.read_bits(3));
        if (br.read_bits(1) != 0) { return FlacError::Malformed; } // reserved
        (void)br.read_utf8();                                      // frame/sample number

        crd::u32 block = 0;
        switch (bs_code)
        {
        case 0: return FlacError::Malformed; // reserved
        case 1: block = 192; break;
        case 2:
        case 3:
        case 4:
        case 5: block = 576U << (bs_code - 2U); break;
        case 6: block = static_cast<crd::u32>(br.read_bits(8)) + 1; break;
        case 7: block = static_cast<crd::u32>(br.read_bits(16)) + 1; break;
        default: block = 256U << (bs_code - 8U); break;
        }
        switch (rate_code) // only the get-from-end codes consume header bits here
        {
        case 12: (void)br.read_bits(8); break;
        case 13:
        case 14: (void)br.read_bits(16); break;
        case 15: return FlacError::Malformed;
        default: break;
        }
        crd::u32 bps = info.bps;
        switch (bps_code)
        {
        case 0: break;
        case 1: bps = 8; break;
        case 2: bps = 12; break;
        case 3: return FlacError::Malformed;
        case 4: bps = 16; break;
        case 5: bps = 20; break;
        case 6: bps = 24; break;
        case 7:
        default: bps = 32; break;
        }

        // CRC-8 over the header bytes read so far
        {
            crd::u8 crc = 0;
            for (crd::usize i = frame_start; i < br.byte + (br.bit != 0 ? 1U : 0U); ++i)
            {
                crc = detail::crc8_update(crc, bytes[i]);
            }
            // header CRC sits at the next byte boundary — the spec aligns it (header is byte-aligned here)
            if (br.bit != 0) { return FlacError::Malformed; }
            const crd::u8 stored = static_cast<crd::u8>(br.read_bits(8));
            if (!br.ok || crc != stored) { return FlacError::BadCrc; }
        }

        crd::u32 nch        = 0;
        crd::u32 assignment = ch_code;
        if (ch_code <= 7) { nch = ch_code + 1; }
        else if (ch_code <= 10) { nch = 2; }
        else
        {
            return FlacError::Malformed;
        }
        if (nch != info.channels) { return FlacError::Malformed; }

        for (crd::u32 c = 0; c < nch; ++c)
        {
            crd::u32 sub_bps = bps;
            if (assignment == 8 && c == 1) { ++sub_bps; }  // L/S: side is one bit wider
            if (assignment == 9 && c == 0) { ++sub_bps; }  // R/S
            if (assignment == 10 && c == 1) { ++sub_bps; } // M/S
            if (!read_subframe(br, ch[c], block, sub_bps)) { return FlacError::Malformed; }
        }
        br.align();

        // CRC-16 over the whole frame incl. header CRC
        {
            crd::u16 crc = 0;
            for (crd::usize i = frame_start; i < br.byte; ++i)
            {
                crc = detail::crc16_update(crc, bytes[i]);
            }
            const crd::u16 stored = static_cast<crd::u16>(br.read_bits(16));
            if (!br.ok || crc != stored) { return FlacError::BadCrc; }
        }

        // undo decorrelation
        if (assignment == 8) // L/S: R = L - side
        {
            for (crd::usize i = 0; i < block; ++i) { ch[1][i] = ch[0][i] - ch[1][i]; }
        }
        else if (assignment == 9) // R/S: L = R + side (subframe 0 = side, 1 = R)
        {
            for (crd::usize i = 0; i < block; ++i) { ch[0][i] = ch[1][i] + ch[0][i]; }
        }
        else if (assignment == 10) // M/S
        {
            for (crd::usize i = 0; i < block; ++i)
            {
                const crd::i64 side = ch[1][i];
                const crd::i64 mid  = (ch[0][i] << 1U) | (side & 1);
                ch[0][i]            = (mid + side) >> 1U;
                ch[1][i]            = (mid - side) >> 1U;
            }
        }

        // interleave + MD5 (little-endian bytes at the stream bps)
        const crd::u32 bytes_per = info.bps / 8U;
        for (crd::usize i = 0; i < block; ++i)
        {
            for (crd::u32 c = 0; c < nch; ++c)
            {
                const crd::i64 v = ch[c][i];
                out.isamples.push_back(static_cast<crd::i32>(v));
                crd::u8 le[4];
                for (crd::u32 b = 0; b < bytes_per; ++b)
                {
                    le[b] = static_cast<crd::u8>((static_cast<crd::u64>(v) >> (8U * b)) & 0xFFU);
                }
                md5.update(le, bytes_per);
            }
        }
        pos = br.byte;
    }

    if (info.total != 0 && out.frame_count() != info.total) { return FlacError::Malformed; }
    if (info.has_md5)
    {
        crd::u8 digest[16];
        md5.final(digest);
        if (std::memcmp(digest, info.md5, 16) != 0) { return FlacError::Md5Mismatch; }
    }
    return out.valid() ? FlacError::Ok : FlacError::Malformed;
}

} // namespace crd::audio
