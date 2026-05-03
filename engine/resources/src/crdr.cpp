#include <crd/resources/crdr.hpp>

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <algorithm>
#include <cstring>

namespace crd::resources
{

// ── FourCC helper ──────────────────────────────────────────────────────────

void fourcc_to_str(crd::u32 fourcc, char (&buf)[5]) noexcept
{
    buf[0] = static_cast<char>( fourcc        & 0xFFU);
    buf[1] = static_cast<char>((fourcc >>  8U) & 0xFFU);
    buf[2] = static_cast<char>((fourcc >> 16U) & 0xFFU);
    buf[3] = static_cast<char>((fourcc >> 24U) & 0xFFU);
    buf[4] = '\0';
}

// ── Little-endian read helpers ─────────────────────────────────────────────

namespace
{

crd::u16 read_u16_le(const crd::u8* p) noexcept
{
    crd::u16 v = 0;
    std::memcpy(&v, p, 2);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = static_cast<crd::u16>((v << 8U) | (v >> 8U));
#endif
    return v;
}

crd::u32 read_u32_le(const crd::u8* p) noexcept
{
    crd::u32 v = 0;
    std::memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U)
      | ((v & 0x00FF0000U) >>  8U) | ((v & 0xFF000000U) >> 24U);
#endif
    return v;
}

crd::u64 read_u64_le(const crd::u8* p) noexcept
{
    crd::u64 v = 0;
    std::memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = ((v & 0x00000000000000FFULL) << 56U) | ((v & 0x000000000000FF00ULL) << 40U)
      | ((v & 0x0000000000FF0000ULL) << 24U) | ((v & 0x00000000FF000000ULL) <<  8U)
      | ((v & 0x000000FF00000000ULL) >>  8U) | ((v & 0x0000FF0000000000ULL) >> 24U)
      | ((v & 0x00FF000000000000ULL) >> 40U) | ((v & 0xFF00000000000000ULL) >> 56U);
#endif
    return v;
}

// Little-endian write helpers.
void write_u16_le(crd::u8* p, crd::u16 v) noexcept
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = static_cast<crd::u16>((v << 8U) | (v >> 8U));
#endif
    std::memcpy(p, &v, 2);
}

void write_u32_le(crd::u8* p, crd::u32 v) noexcept
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U)
      | ((v & 0x00FF0000U) >>  8U) | ((v & 0xFF000000U) >> 24U);
#endif
    std::memcpy(p, &v, 4);
}

void write_u64_le(crd::u8* p, crd::u64 v) noexcept
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = ((v & 0x00000000000000FFULL) << 56U) | ((v & 0x000000000000FF00ULL) << 40U)
      | ((v & 0x0000000000FF0000ULL) << 24U) | ((v & 0x00000000FF000000ULL) <<  8U)
      | ((v & 0x000000FF00000000ULL) >>  8U) | ((v & 0x0000FF0000000000ULL) >> 24U)
      | ((v & 0x00FF000000000000ULL) >> 40U) | ((v & 0xFF00000000000000ULL) >> 56U);
#endif
    std::memcpy(p, &v, 8);
}

// Alignment padding helpers.
constexpr crd::usize kChunkPayloadAlign = 16U;

crd::usize payload_padded_size(crd::usize compressed_size) noexcept
{
    const crd::usize remainder = compressed_size % kChunkPayloadAlign;
    if (remainder == 0U)
    {
        return compressed_size;
    }
    return compressed_size + (kChunkPayloadAlign - remainder);
}

} // anonymous namespace

// ── Reader ─────────────────────────────────────────────────────────────────

CrdrError crdr_read(
    crd::containers::ConstSpan<crd::u8> bytes,
    CrdrFile&                           out,
    crd::memory::IAllocator*            a)
{
    constexpr crd::usize kHeaderSize = 32U;
    constexpr crd::usize kChunkHeaderSize = 24U;

    if (bytes.size() < kHeaderSize)
    {
        return CrdrError::Truncated;
    }

    // Parse header.
    const crd::u32 magic = read_u32_le(bytes.data() + 0U);
    if (magic != kFourCC_CRDR)
    {
        return CrdrError::BadMagic;
    }

    const crd::u16 version = read_u16_le(bytes.data() + 4U);
    if (version != kCrdrVersion)
    {
        return CrdrError::BadVersion;
    }

    const crd::u16 file_flags   = read_u16_le(bytes.data() + 6U);
    const crd::u64 uuid_hi      = read_u64_le(bytes.data() + 8U);
    const crd::u64 uuid_lo      = read_u64_le(bytes.data() + 16U);
    const crd::u32 type_fourcc  = read_u32_le(bytes.data() + 24U);
    const crd::u32 chunk_count  = read_u32_le(bytes.data() + 28U);

    out.id.hi        = uuid_hi;
    out.id.lo        = uuid_lo;
    out.type_fourcc  = type_fourcc;
    out.version      = version;
    out.flags        = file_flags;
    out.chunks       = crd::containers::Array<CrdrChunk>(a);

    // Parse chunks.
    crd::usize offset = kHeaderSize;
    for (crd::u32 i = 0U; i < chunk_count; ++i)
    {
        if (offset + kChunkHeaderSize > bytes.size())
        {
            return CrdrError::Truncated;
        }

        const crd::u32 fourcc            = read_u32_le(bytes.data() + offset + 0U);
        const crd::u32 chunk_flags       = read_u32_le(bytes.data() + offset + 4U);
        const crd::u64 uncompressed_size = read_u64_le(bytes.data() + offset + 8U);
        const crd::u64 compressed_size   = read_u64_le(bytes.data() + offset + 16U);

        offset += kChunkHeaderSize;

        if (compressed_size > bytes.size() - offset)
        {
            return CrdrError::Truncated;
        }

        if (compressed_size > 0xFFFFFFFFULL * kChunkPayloadAlign)
        {
            return CrdrError::InvalidChunk;
        }

        CrdrChunk chunk;
        chunk.fourcc            = fourcc;
        chunk.flags             = chunk_flags;
        chunk.uncompressed_size = uncompressed_size;
        chunk.payload           = bytes.subspan(offset, static_cast<crd::usize>(compressed_size));
        out.chunks.push_back(chunk);

        // Advance past payload (including padding).
        offset += payload_padded_size(static_cast<crd::usize>(compressed_size));
    }

    return CrdrError::Ok;
}

const CrdrChunk* crdr_find_chunk(const CrdrFile& file, crd::u32 fourcc) noexcept
{
    for (const CrdrChunk& chunk : file.chunks)
    {
        if (chunk.fourcc == fourcc)
        {
            return &chunk;
        }
    }
    return nullptr;
}

// ── Writer ─────────────────────────────────────────────────────────────────

CrdrWriter::CrdrWriter(crd::memory::IAllocator* a, ResourceId id, crd::u32 type_fourcc)
    : m_alloc(a), m_id(id), m_type_fourcc(type_fourcc), m_chunks(a)
{
}

void CrdrWriter::add_chunk(
    crd::u32                              fourcc,
    crd::containers::ConstSpan<crd::u8>  payload,
    crd::u32                              chunk_flags)
{
    PendingChunk chunk;
    chunk.fourcc = fourcc;
    chunk.flags  = chunk_flags;
    chunk.payload = crd::containers::Array<crd::u8>(m_alloc);
    chunk.payload.reserve(payload.size());
    for (crd::u8 byte : payload)
    {
        chunk.payload.push_back(byte);
    }
    m_chunks.push_back(std::move(chunk));
}

crd::containers::Array<crd::u8> CrdrWriter::finish()
{
    // Sort chunks ascending by fourcc for determinism.
    std::sort(m_chunks.begin(), m_chunks.end(),
              [](const PendingChunk& a, const PendingChunk& b) noexcept
              {
                  return a.fourcc < b.fourcc;
              });

    constexpr crd::usize kHeaderSize      = 32U;
    constexpr crd::usize kChunkHeaderSize = 24U;

    // Compute total size.
    crd::usize total = kHeaderSize;
    for (const PendingChunk& chunk : m_chunks)
    {
        total += kChunkHeaderSize + payload_padded_size(chunk.payload.size());
    }

    crd::containers::Array<crd::u8> out(m_alloc);
    out.resize(total);
    crd::u8* const buf = out.data();
    std::memset(buf, 0, total);

    // Write header.
    write_u32_le(buf + 0U, kFourCC_CRDR);
    write_u16_le(buf + 4U, kCrdrVersion);
    write_u16_le(buf + 6U, 0U);                     // flags
    write_u64_le(buf + 8U, m_id.hi);
    write_u64_le(buf + 16U, m_id.lo);
    write_u32_le(buf + 24U, m_type_fourcc);
    write_u32_le(buf + 28U, static_cast<crd::u32>(m_chunks.size()));

    // Write chunks.
    crd::usize offset = kHeaderSize;
    for (const PendingChunk& chunk : m_chunks)
    {
        const crd::usize payload_size = chunk.payload.size();
        write_u32_le(buf + offset + 0U, chunk.fourcc);
        write_u32_le(buf + offset + 4U, chunk.flags);
        write_u64_le(buf + offset + 8U,  static_cast<crd::u64>(payload_size)); // uncompressed
        write_u64_le(buf + offset + 16U, static_cast<crd::u64>(payload_size)); // compressed == uncompressed (v1a)
        offset += kChunkHeaderSize;

        if (payload_size > 0U)
        {
            std::memcpy(buf + offset, chunk.payload.data(), payload_size);
        }
        // padding bytes already zero from memset.
        offset += payload_padded_size(payload_size);
    }

    return out;
}

// ── Manifest helpers ────────────────────────────────────────────────────────

namespace
{
constexpr crd::usize kMfstEntrySize = 48U; // bytes per ManifestEntry on disk
} // anonymous namespace

void manifest_write(
    CrdrWriter&                                      writer,
    crd::containers::ConstSpan<ManifestEntry>        entries,
    crd::containers::ConstSpan<crd::u8>              string_pool,
    crd::containers::ConstSpan<crd::u8>              deps_payload)
{
    // Build MFST payload: kMfstEntrySize bytes per entry.
    const crd::usize mfst_size = entries.size() * kMfstEntrySize;
    crd::containers::Array<crd::u8> mfst(crd::memory::default_allocator());
    mfst.resize(mfst_size);
    std::memset(mfst.data(), 0, mfst_size);

    for (crd::usize i = 0U; i < entries.size(); ++i)
    {
        const ManifestEntry& e   = entries[i];
        crd::u8* const       dst = mfst.data() + i * kMfstEntrySize;
        write_u64_le(dst + 0U,  e.id.hi);
        write_u64_le(dst + 8U,  e.id.lo);
        write_u32_le(dst + 16U, e.type_fourcc);
        write_u32_le(dst + 20U, e.flags);
        write_u64_le(dst + 24U, e.blob_offset);
        write_u64_le(dst + 32U, e.blob_size);
        write_u32_le(dst + 40U, e.name_strp_idx);
        write_u32_le(dst + 44U, 0U); // reserved
    }

    writer.add_chunk(kFourCC_MFST, crd::containers::as_const_span(mfst));
    writer.add_chunk(kFourCC_STRP, string_pool);
    writer.add_chunk(kFourCC_DEPS, deps_payload);
}

bool manifest_read_entries(
    crd::containers::ConstSpan<crd::u8>    mfst_payload,
    crd::containers::Array<ManifestEntry>& out,
    crd::memory::IAllocator*               a)
{
    if (mfst_payload.size() % kMfstEntrySize != 0U)
    {
        return false;
    }

    const crd::usize count = mfst_payload.size() / kMfstEntrySize;
    out = crd::containers::Array<ManifestEntry>(a);
    out.reserve(count);

    for (crd::usize i = 0U; i < count; ++i)
    {
        const crd::u8* src = mfst_payload.data() + i * kMfstEntrySize;
        ManifestEntry  e;
        e.id.hi          = read_u64_le(src + 0U);
        e.id.lo          = read_u64_le(src + 8U);
        e.type_fourcc    = read_u32_le(src + 16U);
        e.flags          = read_u32_le(src + 20U);
        e.blob_offset    = read_u64_le(src + 24U);
        e.blob_size      = read_u64_le(src + 32U);
        e.name_strp_idx  = read_u32_le(src + 40U);
        // reserved at +44 is ignored
        out.push_back(e);
    }

    return true;
}

} // namespace crd::resources
