// zip.cpp — the ZIP container. See zip.hpp for the read and write scope.

#include <crd/compression/deflate.hpp>
#include <crd/compression/zip.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/core/assert.hpp>

#include <cstring>
#include <initializer_list>

namespace crd::compression
{
namespace
{
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;

constexpr crd::u32 kSigLocal = 0x04034B50U;
constexpr crd::u32 kSigCentral = 0x02014B50U;
constexpr crd::u32 kSigEnd = 0x06054B50U;
constexpr crd::u32 kSigEnd64 = 0x06064B50U;   // the Zip64 end-of-central-directory record
constexpr crd::u32 kSigLocator = 0x07064B50U; // its locator, directly before the classic end record
constexpr crd::u64 kLocalSize = 30U;
constexpr crd::u64 kCentralSize = 46U;
constexpr crd::u64 kEndSize = 22U;
constexpr crd::u64 kEnd64Size = 56U; // the record's fixed part; its size field counts from byte 12
constexpr crd::u64 kLocatorSize = 20U;
constexpr crd::u64 kMaxComment = 0xFFFFU;
constexpr crd::u16 kSentinel16 = 0xFFFFU;
constexpr crd::u32 kSentinel32 = 0xFFFFFFFFU;
constexpr crd::u16 kZip64Extra = 0x0001U;
constexpr crd::u16 kEncrypted = 0x0001U; // general-purpose flag bit 0 (bit 6, strong encryption, sets it too)
constexpr crd::u16 kVersion20 = 20U;     // 2.0: DEFLATE, folders; the version a classic archive needs
constexpr crd::u16 kDosDate1980 = 0x21U; // 1980-01-01: (year - 1980) << 9 | month << 5 | day

[[nodiscard]] crd::u16 rd_u16(const crd::u8* p) noexcept
{
    return static_cast<crd::u16>(static_cast<crd::u16>(p[0]) | (static_cast<crd::u16>(p[1]) << 8U));
}
[[nodiscard]] crd::u32 rd_u32(const crd::u8* p) noexcept
{
    return static_cast<crd::u32>(p[0]) | (static_cast<crd::u32>(p[1]) << 8U) | (static_cast<crd::u32>(p[2]) << 16U) |
           (static_cast<crd::u32>(p[3]) << 24U);
}
[[nodiscard]] crd::u64 rd_u64(const crd::u8* p) noexcept
{
    return static_cast<crd::u64>(rd_u32(p)) | (static_cast<crd::u64>(rd_u32(p + 4U)) << 32U);
}

void wr_u16(crd::u8* p, crd::u16 v) noexcept
{
    p[0] = static_cast<crd::u8>(v & 0xFFU);
    p[1] = static_cast<crd::u8>(v >> 8U);
}
void wr_u32(crd::u8* p, crd::u32 v) noexcept
{
    wr_u16(p, static_cast<crd::u16>(v & 0xFFFFU));
    wr_u16(p + 2U, static_cast<crd::u16>(v >> 16U));
}

// base plus every term into `out`; false when the sum overflows.
[[nodiscard]] bool sum(crd::usize base, std::initializer_list<crd::usize> terms, crd::usize& out) noexcept
{
    out = base;
    for (const crd::usize term : terms)
    {
        if (term > ~crd::usize{0} - out)
        {
            return false;
        }
        out += term;
    }
    return true;
}

// [offset, offset + size) lies within [0, limit), without overflow.
[[nodiscard]] constexpr bool fits(crd::u64 offset, crd::u64 size, crd::u64 limit) noexcept
{
    return offset <= limit && size <= limit - offset;
}

struct Directory
{
    crd::u64 count = 0;
    crd::u64 offset = 0;
    crd::u64 size = 0;
    crd::u64 limit = 0; // where the end records start: the directory lies before it
};

// The Zip64 end record through the locator that sits directly before the classic end record at `end`.
[[nodiscard]] ZipError read_end64(ConstSpan<crd::u8> bytes, crd::u64 end, Directory& dir) noexcept
{
    if (end < kLocatorSize || rd_u32(bytes.data() + (end - kLocatorSize)) != kSigLocator)
    {
        return ZipError::Malformed;
    }
    const crd::u64 locator = end - kLocatorSize;
    const crd::u8* l = bytes.data() + locator;
    if (rd_u32(l + 4U) != 0U || rd_u32(l + 16U) != 1U) // the record's disk, the disk count
    {
        return ZipError::Unsupported;
    }
    const crd::u64 at = rd_u64(l + 8U);
    if (!fits(at, kEnd64Size, locator))
    {
        return ZipError::Truncated;
    }
    // the record's size counts from byte 12: at least its fixed part, and no further than the locator
    const crd::u8* e = bytes.data() + at;
    const crd::u64 record = rd_u64(e + 4U);
    if (rd_u32(e) != kSigEnd64 || record < kEnd64Size - 12U || record > locator - at - 12U)
    {
        return ZipError::Malformed;
    }
    if (rd_u32(e + 16U) != 0U || rd_u32(e + 20U) != 0U || rd_u64(e + 24U) != rd_u64(e + 32U)) // spanned
    {
        return ZipError::Unsupported;
    }
    dir.count = rd_u64(e + 32U);
    dir.size = rd_u64(e + 40U);
    dir.offset = rd_u64(e + 48U);
    dir.limit = at;
    return ZipError::Ok;
}

// The end-of-central-directory record, found by a back-scan over the largest comment, and the directory it names.
[[nodiscard]] ZipError read_end(ConstSpan<crd::u8> bytes, Directory& dir) noexcept
{
    if (bytes.size() < kEndSize)
    {
        return ZipError::NotRecognized;
    }
    const crd::u64 last = bytes.size() - kEndSize;
    const crd::u64 floor = last > kMaxComment ? last - kMaxComment : 0U;
    crd::u64 end = last;
    while (rd_u32(bytes.data() + end) != kSigEnd)
    {
        if (end == floor)
        {
            return ZipError::NotRecognized;
        }
        --end;
    }
    const crd::u8* e = bytes.data() + end;
    const crd::u16 disk = rd_u16(e + 4U);
    const crd::u16 dir_disk = rd_u16(e + 6U);
    const crd::u16 on_disk = rd_u16(e + 8U);
    const crd::u16 total = rd_u16(e + 10U);
    const crd::u32 size = rd_u32(e + 12U);
    const crd::u32 offset = rd_u32(e + 16U);
    // a field too small for its value holds its sentinel and the Zip64 record holds the value (APPNOTE 4.4.1.4)
    if (disk == kSentinel16 || dir_disk == kSentinel16 || on_disk == kSentinel16 || total == kSentinel16 ||
        size == kSentinel32 || offset == kSentinel32)
    {
        return read_end64(bytes, end, dir);
    }
    if (disk != 0U || dir_disk != 0U || on_disk != total)
    {
        return ZipError::Unsupported;
    }
    dir.count = total;
    dir.size = size;
    dir.offset = offset;
    dir.limit = end;
    return ZipError::Ok;
}

// A central record's sentinel fields from its Zip64 extra field, which holds exactly the sentinel ones in the order
// uncompressed size, compressed size, local header offset. A sentinel with no such field is a violation.
[[nodiscard]] ZipError read_zip64_extra(ConstSpan<crd::u8> extra, crd::u64& uncompressed, crd::u64& compressed,
                                        crd::u64& local) noexcept
{
    crd::usize at = 0U;
    while (extra.size() - at >= 4U)
    {
        const crd::u16 id = rd_u16(extra.data() + at);
        const crd::u16 size = rd_u16(extra.data() + at + 2U);
        if (extra.size() - at - 4U < size)
        {
            return ZipError::Malformed;
        }
        if (id == kZip64Extra)
        {
            const crd::u8* field = extra.data() + at + 4U;
            crd::u16 left = size;
            for (crd::u64* value : {&uncompressed, &compressed, &local})
            {
                if (*value != kSentinel32)
                {
                    continue;
                }
                if (left < 8U)
                {
                    return ZipError::Malformed;
                }
                *value = rd_u64(field);
                field += 8U;
                left = static_cast<crd::u16>(left - 8U);
            }
            return ZipError::Ok;
        }
        at += 4U + size;
    }
    return ZipError::Malformed;
}

// Every central record into `entries`, each member's data resolved through its local header.
[[nodiscard]] ZipError read_directory(ConstSpan<crd::u8> bytes, Array<ZipEntry>& entries) noexcept
{
    Directory dir;
    const ZipError end = read_end(bytes, dir);
    if (end != ZipError::Ok)
    {
        return end;
    }
    if (!fits(dir.offset, dir.size, dir.limit) || dir.count > dir.size / kCentralSize)
    {
        return ZipError::Truncated;
    }
    if (!entries.try_reserve(static_cast<crd::usize>(dir.count)))
    {
        return ZipError::OutOfMemory;
    }
    const crd::u64 dir_end = dir.offset + dir.size;
    crd::u64 at = dir.offset;
    for (crd::u64 n = 0; n < dir.count; ++n)
    {
        if (!fits(at, kCentralSize, dir_end))
        {
            return ZipError::Truncated;
        }
        const crd::u8* c = bytes.data() + at;
        if (rd_u32(c) != kSigCentral)
        {
            return ZipError::Malformed;
        }
        const crd::u16 flags = rd_u16(c + 8U);
        const crd::u16 method = rd_u16(c + 10U);
        crd::u64 compressed = rd_u32(c + 20U);
        crd::u64 uncompressed = rd_u32(c + 24U);
        const crd::u16 name_size = rd_u16(c + 28U);
        const crd::u16 extra_size = rd_u16(c + 30U);
        const crd::u16 comment_size = rd_u16(c + 32U);
        crd::u64 local = rd_u32(c + 42U);
        const crd::u64 record = kCentralSize + name_size + extra_size + comment_size;
        if (!fits(at, record, dir_end))
        {
            return ZipError::Truncated;
        }
        if ((flags & kEncrypted) != 0U ||
            (method != static_cast<crd::u16>(ZipMethod::Stored) && method != static_cast<crd::u16>(ZipMethod::Deflate)))
        {
            return ZipError::Unsupported;
        }
        if (compressed == kSentinel32 || uncompressed == kSentinel32 || local == kSentinel32)
        {
            const ZipError extra =
                read_zip64_extra(bytes.subspan(static_cast<crd::usize>(at + kCentralSize + name_size), extra_size),
                                 uncompressed, compressed, local);
            if (extra != ZipError::Ok)
            {
                return extra;
            }
        }

        if (!fits(local, kLocalSize, bytes.size()))
        {
            return ZipError::Truncated;
        }
        const crd::u8* lh = bytes.data() + local;
        if (rd_u32(lh) != kSigLocal)
        {
            return ZipError::Malformed;
        }
        // the local header's own name and extra sizes place the data (they may differ from the central record's)
        const crd::u64 data = local + kLocalSize + rd_u16(lh + 26U) + rd_u16(lh + 28U);
        if (!fits(data, compressed, bytes.size()))
        {
            return ZipError::Truncated;
        }
        ZipEntry entry;
        entry.method = static_cast<ZipMethod>(method);
        if (entry.method == ZipMethod::Stored && compressed != uncompressed)
        {
            return ZipError::Malformed;
        }
        if (entry.method == ZipMethod::Deflate && uncompressed > deflate_max_inflated_size(compressed))
        {
            return ZipError::Corrupt;
        }
        entry.name = StringView(reinterpret_cast<const char*>(c + kCentralSize), name_size);
        entry.compressed_size = compressed;
        entry.uncompressed_size = uncompressed;
        entry.data_offset = data;
        entry.crc32 = rd_u32(c + 16U);
        entries.push_back(entry); // within the reserve
        at += record;
    }
    return ZipError::Ok;
}

// Append `n` bytes to `out` and return where they start, or nullptr when the allocator refuses.
[[nodiscard]] crd::u8* grow(Array<crd::u8>& out, crd::usize n) noexcept
{
    const crd::usize at = out.size();
    if (!out.try_reserve(at + n))
    {
        return nullptr;
    }
    out.resize_uninitialized(at + n); // within the reserve
    return out.data() + at;
}

} // namespace

ZipError ZipReader::open(ConstSpan<crd::u8> bytes) noexcept
{
    m_entries.clear();
    m_bytes = bytes;
    const ZipError opened = read_directory(bytes, m_entries);
    if (opened != ZipError::Ok)
    {
        m_entries.clear();
        m_bytes = {};
    }
    return opened;
}

const ZipEntry& ZipReader::entry(crd::usize i) const noexcept
{
    CRD_ASSERT_MSG(i < m_entries.size(), "ZipReader::entry: index out of range");
    return m_entries[i];
}

crd::i64 ZipReader::find(StringView name) const noexcept
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].name == name)
        {
            return static_cast<crd::i64>(i);
        }
    }
    return -1;
}

ZipError ZipReader::view(crd::usize i, ConstSpan<crd::u8>& out) const noexcept
{
    const ZipEntry& member = entry(i);
    if (member.method != ZipMethod::Stored)
    {
        return ZipError::Unsupported;
    }
    const ConstSpan<crd::u8> data =
        m_bytes.subspan(static_cast<crd::usize>(member.data_offset), static_cast<crd::usize>(member.compressed_size));
    if (crd::containers::crc32(data) != member.crc32)
    {
        return ZipError::Corrupt;
    }
    out = data;
    return ZipError::Ok;
}

ZipError ZipReader::extract(crd::usize i, Span<crd::u8> out) const noexcept
{
    const ZipEntry& member = entry(i);
    if (out.size() != member.uncompressed_size)
    {
        return ZipError::Unsupported;
    }
    const ConstSpan<crd::u8> data =
        m_bytes.subspan(static_cast<crd::usize>(member.data_offset), static_cast<crd::usize>(member.compressed_size));
    if (member.method == ZipMethod::Stored)
    {
        if (!data.empty())
        {
            std::memcpy(out.data(), data.data(), data.size());
        }
    }
    else if (!inflate_raw(data, out))
    {
        return ZipError::Corrupt;
    }
    return crd::containers::crc32(ConstSpan<crd::u8>(out.data(), out.size())) == member.crc32 ? ZipError::Ok
                                                                                              : ZipError::Corrupt;
}

ZipError ZipReader::extract(crd::usize i, Array<crd::u8>& out) const noexcept
{
    out.clear();
    const ZipEntry& member = entry(i);
    if (member.uncompressed_size > static_cast<crd::u64>(~crd::usize{0}) ||
        !out.try_reserve(static_cast<crd::usize>(member.uncompressed_size)))
    {
        return ZipError::OutOfMemory;
    }
    out.resize_uninitialized(static_cast<crd::usize>(member.uncompressed_size)); // within the reserve
    const ZipError extracted = extract(i, Span<crd::u8>(out.data(), out.size()));
    if (extracted != ZipError::Ok)
    {
        out.clear();
    }
    return extracted;
}

ZipError ZipWriter::add(StringView name, ConstSpan<crd::u8> bytes, ZipMethod method) noexcept
{
    if (name.empty())
    {
        return ZipError::Malformed;
    }
    if (name.size() > kSentinel16 || m_count + 1U >= kSentinel16) // a count of 0xFFFF is the Zip64 sentinel
    {
        return ZipError::Unsupported;
    }

    Array<crd::u8> deflated(m_alloc);
    if (method == ZipMethod::Deflate)
    {
        deflated = deflate_raw(bytes, m_alloc);
        if (deflated.size() >= bytes.size()) // DEFLATE only when it shrinks
        {
            method = ZipMethod::Stored;
        }
    }
    const ConstSpan<crd::u8> payload =
        method == ZipMethod::Stored ? bytes : ConstSpan<crd::u8>(deflated.data(), deflated.size());
    const crd::u64 local = m_out.size();
    const crd::u64 local_size = kLocalSize + name.size() + payload.size();
    // classic fields hold every size and offset below their sentinel: the member's sizes, the directory's offset
    // past this member and the directory's size with this member's record
    if (bytes.size() >= kSentinel32 || local + local_size >= kSentinel32 ||
        m_central.size() + kCentralSize + name.size() >= kSentinel32)
    {
        return ZipError::Unsupported;
    }

    const crd::u32 crc = crd::containers::crc32(bytes);
    const auto header_into = [&](crd::u8* p)
    {
        wr_u16(p, kVersion20); // version needed
        wr_u16(p + 2U, 0U);    // flags
        wr_u16(p + 4U, static_cast<crd::u16>(method));
        wr_u16(p + 6U, 0U); // time 00:00:00
        wr_u16(p + 8U, kDosDate1980);
        wr_u32(p + 10U, crc);
        wr_u32(p + 14U, static_cast<crd::u32>(payload.size()));
        wr_u32(p + 18U, static_cast<crd::u32>(bytes.size()));
        wr_u16(p + 22U, static_cast<crd::u16>(name.size()));
        wr_u16(p + 24U, 0U); // extra size
    };

    const crd::usize central_at = m_central.size();
    crd::u8* c = grow(m_central, static_cast<crd::usize>(kCentralSize) + name.size());
    crd::u8* l = c == nullptr ? nullptr : grow(m_out, static_cast<crd::usize>(local_size));
    if (l == nullptr)
    {
        m_central.resize_uninitialized(central_at); // the archive so far is unchanged
        return ZipError::OutOfMemory;
    }

    wr_u32(l, kSigLocal);
    header_into(l + 4U);
    std::memcpy(l + kLocalSize, name.data(), name.size());
    if (!payload.empty())
    {
        std::memcpy(l + kLocalSize + name.size(), payload.data(), payload.size());
    }

    wr_u32(c, kSigCentral);
    wr_u16(c + 4U, kVersion20); // version made by: MS-DOS attributes, 2.0
    header_into(c + 6U);
    wr_u16(c + 32U, 0U); // comment size
    wr_u16(c + 34U, 0U); // disk
    wr_u16(c + 36U, 0U); // internal attributes
    wr_u32(c + 38U, 0U); // external attributes
    wr_u32(c + 42U, static_cast<crd::u32>(local));
    std::memcpy(c + kCentralSize, name.data(), name.size());

    ++m_count;
    return ZipError::Ok;
}

ZipError ZipWriter::reserve(crd::usize members, crd::usize name_bytes, crd::usize data_bytes) noexcept
{
    // the directory's records, and the local section with the directory and the end record finish appends to it;
    // a size past the address space is one no allocator holds
    crd::usize central = 0;
    crd::usize archive = 0;
    if (members > (~crd::usize{0} - name_bytes) / kCentralSize ||
        !sum(m_central.size(), {members * static_cast<crd::usize>(kCentralSize), name_bytes}, central) ||
        !sum(m_out.size(),
             {members * static_cast<crd::usize>(kLocalSize), name_bytes, data_bytes, central,
              static_cast<crd::usize>(kEndSize)},
             archive))
    {
        return ZipError::OutOfMemory;
    }
    if (!m_central.try_reserve(central) || !m_out.try_reserve(archive))
    {
        return ZipError::OutOfMemory;
    }
    return ZipError::Ok;
}

ZipError ZipWriter::finish(Array<crd::u8>& out) noexcept
{
    const crd::u64 offset = m_out.size(); // add held both below their sentinels
    const crd::u64 size = m_central.size();
    crd::u8* p = grow(m_out, m_central.size() + static_cast<crd::usize>(kEndSize));
    if (p == nullptr)
    {
        return ZipError::OutOfMemory;
    }
    if (!m_central.empty())
    {
        std::memcpy(p, m_central.data(), m_central.size());
    }
    crd::u8* e = p + m_central.size();
    wr_u32(e, kSigEnd);
    wr_u16(e + 4U, 0U); // this disk
    wr_u16(e + 6U, 0U); // the directory's disk
    wr_u16(e + 8U, static_cast<crd::u16>(m_count));
    wr_u16(e + 10U, static_cast<crd::u16>(m_count));
    wr_u32(e + 12U, static_cast<crd::u32>(size));
    wr_u32(e + 16U, static_cast<crd::u32>(offset));
    wr_u16(e + 20U, 0U); // comment size

    out = static_cast<Array<crd::u8>&&>(m_out); // m_out is left empty on its allocator
    m_central.clear();
    m_count = 0U;
    return ZipError::Ok;
}

} // namespace crd::compression
