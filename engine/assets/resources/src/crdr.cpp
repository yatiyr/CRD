#include <crd/resources/crdr.hpp>

#include <crd/compression/zstd.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <crd/containers/sort.hpp>

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
#if CRD_BIG_ENDIAN
    v = static_cast<crd::u16>((v << 8U) | (v >> 8U));
#endif
    return v;
}

crd::u32 read_u32_le(const crd::u8* p) noexcept
{
    crd::u32 v = 0;
    std::memcpy(&v, p, 4);
#if CRD_BIG_ENDIAN
    v = ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U)
      | ((v & 0x00FF0000U) >>  8U) | ((v & 0xFF000000U) >> 24U);
#endif
    return v;
}

crd::u64 read_u64_le(const crd::u8* p) noexcept
{
    crd::u64 v = 0;
    std::memcpy(&v, p, 8);
#if CRD_BIG_ENDIAN
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
#if CRD_BIG_ENDIAN
    v = static_cast<crd::u16>((v << 8U) | (v >> 8U));
#endif
    std::memcpy(p, &v, 2);
}

void write_u32_le(crd::u8* p, crd::u32 v) noexcept
{
#if CRD_BIG_ENDIAN
    v = ((v & 0x000000FFU) << 24U) | ((v & 0x0000FF00U) << 8U)
      | ((v & 0x00FF0000U) >>  8U) | ((v & 0xFF000000U) >> 24U);
#endif
    std::memcpy(p, &v, 4);
}

void write_u64_le(crd::u8* p, crd::u64 v) noexcept
{
#if CRD_BIG_ENDIAN
    v = ((v & 0x00000000000000FFULL) << 56U) | ((v & 0x000000000000FF00ULL) << 40U)
      | ((v & 0x0000000000FF0000ULL) << 24U) | ((v & 0x00000000FF000000ULL) <<  8U)
      | ((v & 0x000000FF00000000ULL) >>  8U) | ((v & 0x0000FF0000000000ULL) >> 24U)
      | ((v & 0x00FF000000000000ULL) >> 40U) | ((v & 0xFF00000000000000ULL) >> 56U);
#endif
    std::memcpy(p, &v, 8);
}

// The container header and each chunk header are 40 bytes; the last four hold the CRC-32C of the 36 before them.
constexpr crd::usize kHeaderSize = 40U;
constexpr crd::usize kChunkHeaderSize = 40U;
constexpr crd::usize kCheckedBytes = 36U;

[[nodiscard]] crd::u32 header_crc(const crd::u8* header) noexcept
{
    return crd::containers::crc32c(crd::containers::ConstSpan<crd::u8>(header, kCheckedBytes));
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

const char* crdr_error_text(CrdrError error) noexcept
{
    switch (error)
    {
        case CrdrError::Ok:
            return "ok";
        case CrdrError::BadMagic:
            return "not a CRDR container (bad magic)";
        case CrdrError::BadVersion:
            return "unsupported CRDR version";
        case CrdrError::Truncated:
            return "truncated";
        case CrdrError::InvalidChunk:
            return "invalid chunk";
        case CrdrError::DecompressFailed:
            return "decompression failed";
        case CrdrError::HeaderCorrupt:
            return "the container header failed its checksum";
        case CrdrError::ChunkCorrupt:
            return "a chunk failed its checksum after the read";
        case CrdrError::DecodedCorrupt:
            return "a chunk failed its checksum after decompression";
    }
    return "unknown CRDR error";
}

namespace
{

thread_local CrdrFaultScope* tl_fault_scope = nullptr;

CrdrError parse_container(
    crd::containers::ConstSpan<crd::u8> bytes,
    CrdrFile&                           out,
    crd::memory::IAllocator*            a)
{
    out.chunks = crd::containers::Array<CrdrChunk>(a);
    out.decompressed_backing = crd::containers::Array<crd::u8>(a);
    out.fault_chunk = 0U;
    out.fault_fourcc = 0U;

    if (bytes.size() < kHeaderSize)
    {
        return CrdrError::Truncated;
    }

    // The header's checksum comes first, so no field of a flipped header is trusted. A header that fails it is named
    // by what it holds: not a container, another version (an older container, or a flipped version field), or corrupt.
    const crd::u32 magic = read_u32_le(bytes.data() + 0U);
    const crd::u16 version = read_u16_le(bytes.data() + 4U);
    if (header_crc(bytes.data()) != read_u32_le(bytes.data() + kCheckedBytes) || magic != kFourCC_CRDR ||
        version != kCrdrVersion)
    {
        if (magic != kFourCC_CRDR)
        {
            return CrdrError::BadMagic;
        }
        return version != kCrdrVersion ? CrdrError::BadVersion : CrdrError::HeaderCorrupt;
    }

    out.id.hi = read_u64_le(bytes.data() + 8U);
    out.id.lo = read_u64_le(bytes.data() + 16U);
    out.type_fourcc = read_u32_le(bytes.data() + 24U);
    out.version = version;
    out.flags = read_u16_le(bytes.data() + 6U);
    const crd::u32 chunk_count = read_u32_le(bytes.data() + 28U);

    const auto fault = [&out](CrdrError error, crd::u32 index, const crd::u8* chunk_header) noexcept
    {
        out.fault_chunk = index;
        out.fault_fourcc = read_u32_le(chunk_header);
        return error;
    };

    // Pass 1: check every chunk header, then scan the sizes to compute total decompressed bytes needed,
    // so we can pre-allocate decompressed_backing in one shot (no reallocations
    // that would invalidate any ConstSpan views into it). A compressed chunk's claimed size is checked against the
    // most its stored bytes can decode to (zstd_max_decompressed_size) before anything is reserved for it, so the
    // backing is never sized by a claim the file cannot back, and the total cannot wrap.
    {
        crd::usize scan_offset = kHeaderSize;
        crd::usize total_decompressed = 0U;
        for (crd::u32 i = 0U; i < chunk_count; ++i)
        {
            if (scan_offset + kChunkHeaderSize > bytes.size())
            {
                return CrdrError::Truncated;
            }
            const crd::u8* const header = bytes.data() + scan_offset;
            if (header_crc(header) != read_u32_le(header + kCheckedBytes))
            {
                return fault(CrdrError::ChunkCorrupt, i, header);
            }
            const crd::u32 chunk_flags = read_u32_le(header + 4U);
            const crd::u64 uncomp_size = read_u64_le(header + 8U);
            const crd::u64 compressed_size = read_u64_le(header + 16U);
            scan_offset += kChunkHeaderSize;

            if (compressed_size > bytes.size() - scan_offset)
            {
                return CrdrError::Truncated;
            }
            if ((chunk_flags & 0x1U) != 0U)
            {
                if (uncomp_size > crd::compression::zstd_max_decompressed_size(compressed_size))
                {
                    return fault(CrdrError::InvalidChunk, i, header);
                }
                total_decompressed += static_cast<crd::usize>(uncomp_size);
            }
            else if (uncomp_size != compressed_size || read_u64_le(header + 24U) != 0U)
            {
                return fault(CrdrError::InvalidChunk, i, header);
            }
            scan_offset += payload_padded_size(static_cast<crd::usize>(compressed_size));
        }
        if (total_decompressed > 0U)
        {
            out.decompressed_backing.resize(total_decompressed);
        }
    }

    // Pass 2: check each stored payload, then decompress into the backing buffer and check what decoded.
    crd::usize offset = kHeaderSize;
    crd::usize backing_write = 0U;
    for (crd::u32 i = 0U; i < chunk_count; ++i)
    {
        const crd::u8* const header = bytes.data() + offset;
        const crd::u32 chunk_flags = read_u32_le(header + 4U);
        const crd::u64 uncompressed_size = read_u64_le(header + 8U);
        const crd::u64 compressed_size = read_u64_le(header + 16U);

        offset += kChunkHeaderSize;

        if (compressed_size > 0xFFFFFFFFULL * kChunkPayloadAlign)
        {
            return fault(CrdrError::InvalidChunk, i, header);
        }

        const crd::usize comp_size = static_cast<crd::usize>(compressed_size);
        const crd::containers::ConstSpan<crd::u8> stored = bytes.subspan(offset, comp_size);
        if (crd::containers::crc32c(stored) != read_u32_le(header + 32U))
        {
            return fault(CrdrError::ChunkCorrupt, i, header);
        }

        CrdrChunk chunk;
        chunk.fourcc            = read_u32_le(header);
        chunk.flags             = chunk_flags;
        chunk.uncompressed_size = uncompressed_size;

        if (chunk_flags & 0x1U)
        {
            // Compressed chunk: decompress into pre-allocated backing buffer.
            const crd::usize uncomp_size = static_cast<crd::usize>(uncompressed_size);
            crd::u8* dst = out.decompressed_backing.data() + backing_write;
            // A frame that decodes to fewer bytes than the chunk claims would leave the payload's tail as zero fill
            // the file never held; the claim and the decoded size must agree exactly.
            if (!crd::compression::zstd_decompress_exact(stored, crd::containers::Span<crd::u8>(dst, uncomp_size)))
            {
                return fault(CrdrError::DecompressFailed, i, header);
            }
            chunk.payload = crd::containers::ConstSpan<crd::u8>(dst, uncomp_size);
            if (crd::containers::xxh3_64(chunk.payload) != read_u64_le(header + 24U))
            {
                return fault(CrdrError::DecodedCorrupt, i, header);
            }
            backing_write += uncomp_size;
        }
        else
        {
            // Uncompressed: payload is a view directly into `bytes`.
            chunk.payload = stored;
        }

        out.chunks.push_back(chunk);

        // Advance past payload (including padding).
        offset += payload_padded_size(comp_size);
    }

    return CrdrError::Ok;
}

} // namespace

bool crdr_is_current(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    return bytes.size() >= kHeaderSize && read_u32_le(bytes.data()) == kFourCC_CRDR &&
           read_u16_le(bytes.data() + 4U) == kCrdrVersion &&
           header_crc(bytes.data()) == read_u32_le(bytes.data() + kCheckedBytes);
}

CrdrFaultScope::CrdrFaultScope() noexcept : m_outer(tl_fault_scope)
{
    tl_fault_scope = this;
}

CrdrFaultScope::~CrdrFaultScope()
{
    tl_fault_scope = m_outer;
}

CrdrError crdr_read(crd::containers::ConstSpan<crd::u8> bytes, CrdrFile& out, crd::memory::IAllocator* a)
{
    const CrdrError error = parse_container(bytes, out, a);
    CrdrFaultScope* const scope = tl_fault_scope;
    if (error != CrdrError::Ok && scope != nullptr && scope->m_error == CrdrError::Ok)
    {
        scope->m_error = error;
        scope->m_fault_fourcc = out.fault_fourcc;
    }
    return error;
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
    crd::containers::ConstSpan<crd::u8>  payload)
{
    PendingChunk chunk;
    chunk.fourcc            = fourcc;
    chunk.flags             = 0U;
    chunk.uncompressed_size = static_cast<crd::u64>(payload.size());
    chunk.decoded_hash      = 0U;
    chunk.payload           = crd::containers::Array<crd::u8>(m_alloc);
    chunk.payload.reserve(payload.size());
    for (crd::u8 byte : payload)
    {
        chunk.payload.push_back(byte);
    }
    m_chunks.push_back(std::move(chunk));
}

void CrdrWriter::add_chunk_compressed(
    crd::u32                             fourcc,
    crd::containers::ConstSpan<crd::u8> payload,
    int                                  zstd_level)
{
    const crd::usize src_size = payload.size();
    crd::containers::Array<crd::u8> compressed(m_alloc);

    // Fall back to uncompressed if zstd failed or didn't help.
    if (!crd::compression::zstd_compress(payload, zstd_level, compressed) || compressed.size() >= src_size)
    {
        add_chunk(fourcc, payload);
        return;
    }

    PendingChunk chunk;
    chunk.fourcc            = fourcc;
    chunk.flags             = 0x1U; // bit 0: compressed
    chunk.uncompressed_size = static_cast<crd::u64>(src_size);
    chunk.decoded_hash      = crd::containers::xxh3_64(payload);
    chunk.payload           = std::move(compressed);
    m_chunks.push_back(std::move(chunk));
}

crd::containers::Array<crd::u8> CrdrWriter::finish()
{
    // Sort chunks ascending by fourcc for determinism — crd sort (bit-exact
    // cross-platform, and the 14.51 STL xutility pattern trips clang-tidy
    // through std::sort's innards).
    crd::containers::sort(m_chunks.begin(), m_chunks.end(),
                          [](const PendingChunk& a, const PendingChunk& b) noexcept
                          {
                              return a.fourcc < b.fourcc;
                          });

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
    write_u32_le(buf + kCheckedBytes, header_crc(buf)); // the reserved word at +32 stays 0

    // Write chunks.
    crd::usize offset = kHeaderSize;
    for (const PendingChunk& chunk : m_chunks)
    {
        const crd::usize compressed_size = chunk.payload.size();
        crd::u8* const header = buf + offset;
        write_u32_le(header + 0U, chunk.fourcc);
        write_u32_le(header + 4U, chunk.flags);
        write_u64_le(header + 8U, chunk.uncompressed_size);
        write_u64_le(header + 16U, static_cast<crd::u64>(compressed_size));
        write_u64_le(header + 24U, chunk.decoded_hash);
        write_u32_le(header + 32U, crd::containers::crc32c(crd::containers::as_const_span(chunk.payload)));
        write_u32_le(header + kCheckedBytes, header_crc(header));
        offset += kChunkHeaderSize;

        if (compressed_size > 0U)
        {
            std::memcpy(buf + offset, chunk.payload.data(), compressed_size);
        }
        // padding bytes already zero from memset.
        offset += payload_padded_size(compressed_size);
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
