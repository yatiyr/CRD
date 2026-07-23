// zip_archive.cpp — GEO-5 pt 2 (D-007): the owned ZIP container. See zip_archive.hpp for the scope contract.

#include <crd/resources/zip_archive.hpp>

#include <crd/resources/deflate.hpp>
#include <crd/resources/png_image.hpp> // png_crc32 — the standard CRC-32 (same polynomial ZIP specifies)

#include <cstring>

namespace crd::resources
{
namespace
{

constexpr crd::u32 kSigLocal    = 0x04034B50U;
constexpr crd::u32 kSigCentral  = 0x02014B50U;
constexpr crd::u32 kSigEocd     = 0x06054B50U;
constexpr crd::u32 kSigEocd64   = 0x06064B50U; // the Zip64 end-of-central-directory record
constexpr crd::u32 kSigEocd64Lo = 0x07064B50U; // its locator (sits directly before the classic EOCD)

[[nodiscard]] crd::u16 rd_u16(const crd::u8* p) noexcept
{
    return static_cast<crd::u16>(static_cast<crd::u16>(p[0]) | (static_cast<crd::u16>(p[1]) << 8U));
}
[[nodiscard]] crd::u32 rd_u32(const crd::u8* p) noexcept
{
    return static_cast<crd::u32>(p[0]) | (static_cast<crd::u32>(p[1]) << 8U) | (static_cast<crd::u32>(p[2]) << 16U)
         | (static_cast<crd::u32>(p[3]) << 24U);
}
[[nodiscard]] crd::u64 rd_u64(const crd::u8* p) noexcept
{
    return static_cast<crd::u64>(rd_u32(p)) | (static_cast<crd::u64>(rd_u32(p + 4U)) << 32U);
}

void wr_u16(crd::containers::Array<crd::u8>& out, crd::u16 v)
{
    out.push_back(static_cast<crd::u8>(v & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
}
void wr_u32(crd::containers::Array<crd::u8>& out, crd::u32 v)
{
    out.push_back(static_cast<crd::u8>(v & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<crd::u8>((v >> 24U) & 0xFFU));
}

} // namespace

ZipError ZipReader::open(crd::containers::ConstSpan<crd::u8> bytes)
{
    m_entries.clear();
    m_bytes = bytes;

    const auto fail = [&](ZipError e) {
        m_entries.clear();
        return e;
    };

    // EOCD: back-scan (record is 22 bytes + a comment ≤ 64 KiB)
    if (bytes.size() < 22U) { return fail(ZipError::NotRecognized); }
    const crd::usize scan_floor = bytes.size() > (22U + 65535U) ? bytes.size() - 22U - 65535U : 0U;
    crd::i64         eocd       = -1;
    for (crd::i64 i = static_cast<crd::i64>(bytes.size()) - 22; i >= static_cast<crd::i64>(scan_floor); --i)
    {
        if (rd_u32(bytes.data() + i) == kSigEocd)
        {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) { return fail(ZipError::NotRecognized); }
    const crd::u8* e           = bytes.data() + eocd;
    const crd::u16 disk        = rd_u16(e + 4U);
    crd::u64       total       = rd_u16(e + 10U);
    crd::u64       cdir_size   = rd_u32(e + 12U);
    crd::u64       cdir_offset = rd_u32(e + 16U);
    if (disk != 0U) { return fail(ZipError::Unsupported); } // spanned archives

    // Zip64: sentinel EOCD fields defer to the Zip64 EOCD record, found through the locator directly before the
    // classic EOCD (the reference 3MF toolchain writes these structures unconditionally)
    if (total == 0xFFFFU || cdir_size == 0xFFFFFFFFU || cdir_offset == 0xFFFFFFFFU)
    {
        if (eocd < 20 || rd_u32(bytes.data() + (eocd - 20)) != kSigEocd64Lo) { return fail(ZipError::Malformed); }
        const crd::u8* loc = bytes.data() + (eocd - 20);
        if (rd_u32(loc + 4U) != 0U || rd_u32(loc + 16U) != 1U) { return fail(ZipError::Unsupported); } // spanned
        const crd::u64 e64_off = rd_u64(loc + 8U);
        if (e64_off + 56U > static_cast<crd::u64>(eocd)) { return fail(ZipError::Truncated); }
        const crd::u8* e64 = bytes.data() + e64_off;
        if (rd_u32(e64) != kSigEocd64) { return fail(ZipError::Malformed); }
        if (rd_u32(e64 + 16U) != 0U || rd_u32(e64 + 20U) != 0U) { return fail(ZipError::Unsupported); } // spanned
        total       = rd_u64(e64 + 32U);
        cdir_size   = rd_u64(e64 + 40U);
        cdir_offset = rd_u64(e64 + 48U);
    }
    if (cdir_offset + cdir_size > static_cast<crd::u64>(eocd)) { return fail(ZipError::Truncated); }

    // the central directory walk
    const crd::u8* p   = bytes.data() + cdir_offset;
    const crd::u8* end = p + cdir_size;
    for (crd::u64 n = 0; n < total; ++n)
    {
        if (p + 46 > end) { return fail(ZipError::Truncated); }
        if (rd_u32(p) != kSigCentral) { return fail(ZipError::Malformed); }
        const crd::u16 flags      = rd_u16(p + 8U);
        const crd::u16 method     = rd_u16(p + 10U);
        const crd::u32 crc        = rd_u32(p + 16U);
        crd::u64       csize      = rd_u32(p + 20U);
        crd::u64       raw_size   = rd_u32(p + 24U);
        const crd::u16 name_len   = rd_u16(p + 28U);
        const crd::u16 extra_len  = rd_u16(p + 30U);
        const crd::u16 comm_len   = rd_u16(p + 32U);
        crd::u64       local_off  = rd_u32(p + 42U);
        if ((flags & 0x0001U) != 0U) { return fail(ZipError::Unsupported); } // encrypted
        if ((flags & 0x0008U) != 0U && crc == 0U && csize == 0U) { return fail(ZipError::Unsupported); } // descriptor-only
        if (method != 0U && method != 8U) { return fail(ZipError::Unsupported); }
        if (p + 46 + name_len + extra_len + comm_len > end) { return fail(ZipError::Truncated); }

        // Zip64: sentinel fixed fields resolve through the 0x0001 extra field — ONLY the sentinel ones appear
        // there, in the spec's fixed order (uncompressed, compressed, local offset)
        if (csize == 0xFFFFFFFFU || raw_size == 0xFFFFFFFFU || local_off == 0xFFFFFFFFU)
        {
            const crd::u8* xp   = p + 46 + name_len;
            const crd::u8* xend = xp + extra_len;
            bool           resolved = false;
            while (xp + 4 <= xend)
            {
                const crd::u16 xid   = rd_u16(xp);
                const crd::u16 xsize = rd_u16(xp + 2U);
                if (xp + 4 + xsize > xend) { return fail(ZipError::Malformed); }
                if (xid == 0x0001U)
                {
                    const crd::u8* f    = xp + 4;
                    const crd::u8* fend = f + xsize;
                    if (raw_size == 0xFFFFFFFFU)
                    {
                        if (f + 8 > fend) { return fail(ZipError::Malformed); }
                        raw_size = rd_u64(f);
                        f += 8;
                    }
                    if (csize == 0xFFFFFFFFU)
                    {
                        if (f + 8 > fend) { return fail(ZipError::Malformed); }
                        csize = rd_u64(f);
                        f += 8;
                    }
                    if (local_off == 0xFFFFFFFFU)
                    {
                        if (f + 8 > fend) { return fail(ZipError::Malformed); }
                        local_off = rd_u64(f);
                    }
                    resolved = true;
                    break;
                }
                xp += 4 + xsize;
            }
            if (!resolved) { return fail(ZipError::Malformed); } // a sentinel with no Zip64 extra is a violation
        }

        ZipEntry ze(m_alloc);
        for (crd::u16 i = 0; i < name_len; ++i) { ze.name.push_back(static_cast<char>(p[46U + i])); }
        ze.compressed_size   = csize;
        ze.uncompressed_size = raw_size;
        ze.crc32             = crc;
        ze.method            = method;
        ze.local_offset      = local_off;
        m_entries.push_back(std::move(ze));

        p += 46 + name_len + extra_len + comm_len;
    }
    return ZipError::Ok;
}

crd::i64 ZipReader::find(const char* name) const noexcept
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (std::strcmp(m_entries[i].name.c_str(), name) == 0) { return static_cast<crd::i64>(i); }
    }
    return -1;
}

ZipError ZipReader::extract(crd::usize i, crd::containers::Array<crd::u8>& out) const
{
    out.clear();
    if (i >= m_entries.size()) { return ZipError::Malformed; }
    const ZipEntry& ze = m_entries[i];

    // the LOCAL header's own name/extra lengths govern the data offset (they may differ from the central record's)
    if (ze.local_offset + 30U > m_bytes.size()) { return ZipError::Truncated; }
    const crd::u8* lh = m_bytes.data() + static_cast<crd::usize>(ze.local_offset);
    if (rd_u32(lh) != kSigLocal) { return ZipError::Malformed; }
    const crd::u16 lname  = rd_u16(lh + 26U);
    const crd::u16 lextra = rd_u16(lh + 28U);
    const crd::u64 data   = ze.local_offset + 30U + lname + lextra;
    if (data + ze.compressed_size > m_bytes.size()) { return ZipError::Truncated; }
    const crd::containers::ConstSpan<crd::u8> comp(m_bytes.data() + static_cast<crd::usize>(data),
                                                   static_cast<crd::usize>(ze.compressed_size));

    if (ze.method == 0U) // stored
    {
        if (ze.compressed_size != ze.uncompressed_size) { return ZipError::Malformed; }
        for (crd::usize b = 0; b < comp.size(); ++b) { out.push_back(comp[b]); }
    }
    else // deflate
    {
        if (!inflate_raw(comp, out)) { out.clear(); return ZipError::Corrupt; }
    }
    if (out.size() != ze.uncompressed_size) { out.clear(); return ZipError::Corrupt; }
    if (png_crc32(crd::containers::ConstSpan<crd::u8>(out.data(), out.size())) != ze.crc32)
    {
        out.clear();
        return ZipError::Corrupt;
    }
    return ZipError::Ok;
}

bool ZipWriter::add(const char* name, crd::containers::ConstSpan<crd::u8> bytes)
{
    const crd::usize name_len = std::strlen(name);
    if (name_len == 0U || name_len > 0xFFFFU) { return false; }

    const crd::u32 crc = png_crc32(bytes);
    crd::containers::Array<crd::u8> deflated = deflate_raw(bytes, m_alloc);
    const bool stored = deflated.size() >= bytes.size(); // deflate only when it SHRINKS
    const crd::containers::ConstSpan<crd::u8> payload =
        stored ? bytes : crd::containers::ConstSpan<crd::u8>(deflated.data(), deflated.size());
    const crd::u16 method       = stored ? 0U : 8U;
    const crd::u32 local_offset = static_cast<crd::u32>(m_out.size());

    // local header
    wr_u32(m_out, kSigLocal);
    wr_u16(m_out, 20U); // version needed
    wr_u16(m_out, 0U);  // flags
    wr_u16(m_out, method);
    wr_u16(m_out, 0U); // time
    wr_u16(m_out, 0U); // date (fixed-zero — deterministic archives, the reproducible-build convention)
    wr_u32(m_out, crc);
    wr_u32(m_out, static_cast<crd::u32>(payload.size()));
    wr_u32(m_out, static_cast<crd::u32>(bytes.size()));
    wr_u16(m_out, static_cast<crd::u16>(name_len));
    wr_u16(m_out, 0U); // extra len
    for (crd::usize i = 0; i < name_len; ++i) { m_out.push_back(static_cast<crd::u8>(name[i])); }
    for (crd::usize i = 0; i < payload.size(); ++i) { m_out.push_back(payload[i]); }

    // central record
    wr_u32(m_central, kSigCentral);
    wr_u16(m_central, 20U); // version made by
    wr_u16(m_central, 20U); // version needed
    wr_u16(m_central, 0U);  // flags
    wr_u16(m_central, method);
    wr_u16(m_central, 0U); // time
    wr_u16(m_central, 0U); // date
    wr_u32(m_central, crc);
    wr_u32(m_central, static_cast<crd::u32>(payload.size()));
    wr_u32(m_central, static_cast<crd::u32>(bytes.size()));
    wr_u16(m_central, static_cast<crd::u16>(name_len));
    wr_u16(m_central, 0U); // extra
    wr_u16(m_central, 0U); // comment
    wr_u16(m_central, 0U); // disk
    wr_u16(m_central, 0U); // internal attrs
    wr_u32(m_central, 0U); // external attrs
    wr_u32(m_central, local_offset);
    for (crd::usize i = 0; i < name_len; ++i) { m_central.push_back(static_cast<crd::u8>(name[i])); }

    ++m_count;
    return true;
}

crd::containers::Array<crd::u8> ZipWriter::finish()
{
    const crd::u32 cdir_offset = static_cast<crd::u32>(m_out.size());
    for (crd::usize i = 0; i < m_central.size(); ++i) { m_out.push_back(m_central[i]); }
    const crd::u32 cdir_size = static_cast<crd::u32>(m_out.size()) - cdir_offset;

    wr_u32(m_out, kSigEocd);
    wr_u16(m_out, 0U); // disk
    wr_u16(m_out, 0U); // cdir disk
    wr_u16(m_out, m_count);
    wr_u16(m_out, m_count);
    wr_u32(m_out, cdir_size);
    wr_u32(m_out, cdir_offset);
    wr_u16(m_out, 0U); // comment len

    crd::containers::Array<crd::u8> result(m_alloc);
    for (crd::usize i = 0; i < m_out.size(); ++i) { result.push_back(m_out[i]); }
    m_out.clear();
    m_central.clear();
    m_count = 0;
    return result;
}

} // namespace crd::resources
