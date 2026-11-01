// crd-perf -- cerid-diagnostics/1 post-mortem bundle container + writer (DIAG.5d(b)).
// See bundle.hpp for the format and rationale. This TU packages bytes handed to it; it
// captures no live process state and logs nothing. Its files go through crd-platform (OWN.1).

#include <crd/perf/bundle.hpp>

#include <crd/compression/deflate.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diagnostics.hpp> // diagnostic_now_ns(), kDiagnosticSchemaVersion
#include <crd/platform/durable_file.hpp>
#include <crd/platform/filesystem.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace crd::perf
{
namespace cont = crd::containers;
namespace fs = crd::platform::fs;

namespace
{
// The CRC-32 of the bundle's header and payloads (crd/containers/checksum.hpp).
crd::u32 crc32_bytes(const crd::u8* p, crd::usize n) noexcept
{
    return cont::crc32(cont::ConstSpan<crd::u8>(p, n));
}

void append_bytes(cont::Array<crd::u8>& a, const void* src, crd::usize n) noexcept
{
    if (n == 0)
    {
        return;
    }
    const crd::usize old = a.size();
    a.resize_uninitialized(old + n);
    std::memcpy(a.data() + old, src, n);
}

// The last segment of a generic (forward-slash) path.
cont::StringView file_name(cont::StringView path) noexcept
{
    const crd::usize slash = path.rfind('/');
    return slash == cont::StringView::npos ? path : path.substr(slash + 1U);
}

// Whether `name` begins with `prefix` and ends with `suffix` (a null one matches anything).
bool name_matches(cont::StringView name, const char* prefix, const char* suffix) noexcept
{
    return (prefix == nullptr || name.starts_with(prefix)) && (suffix == nullptr || name.ends_with(suffix));
}
} // namespace

// ---- BundleWriter -------------------------------------------------------

BundleWriter::BundleWriter(crd::memory::IAllocator* alloc, BundleLimits limits, BundleCompression compression) noexcept
    : m_alloc(alloc), m_limits(limits), m_compression(compression), m_sections(alloc)
{
}

void BundleWriter::emit(BundleSectionTag tag, crd::u32 flags, cont::ConstSpan<crd::u8> payload,
                        crd::u64 original_length) noexcept
{
    BundleSectionHeader sh{};
    sh.tag             = static_cast<crd::u32>(tag);
    sh.flags           = flags;
    sh.payload_length  = payload.size();
    sh.original_length = original_length;
    sh.payload_crc32   = payload.empty() ? 0U : crc32_bytes(payload.data(), payload.size());
    sh._pad_a          = 0U;

    append_bytes(m_sections, &sh, sizeof(sh));
    append_bytes(m_sections, payload.data(), payload.size());

    const crd::u64 decoded = (flags & kSectionFlagCompressed) != 0U ? original_length : payload.size();
    m_total += sizeof(BundleSectionHeader) + payload.size();
    m_decoded += sizeof(BundleSectionHeader) + decoded;
    ++m_section_count;
}

BundleWriter::AddStatus BundleWriter::add_section(BundleSectionTag tag, cont::ConstSpan<crd::u8> bytes) noexcept
{
    if (m_finished)
    {
        return AddStatus::RejectedNoRoom;
    }
    if (m_section_count >= m_limits.max_sections)
    {
        return AddStatus::RejectedTooMany;
    }

    // Room accounting first: every section (an empty one included) costs at least its header
    // against the total cap, so the empty branch must not bypass this check. The caps bound the
    // decoded bundle, which a reader holds; the stored one is never larger.
    if (m_decoded >= m_limits.max_total_bytes || (m_limits.max_total_bytes - m_decoded) < sizeof(BundleSectionHeader))
    {
        return AddStatus::RejectedNoRoom;
    }

    if (bytes.empty())
    {
        emit(tag, kSectionFlagAbsent, {}, 0U);
        return AddStatus::StoredAbsent;
    }

    const crd::u64 room_after_header = (m_limits.max_total_bytes - m_decoded) - sizeof(BundleSectionHeader);
    crd::u64       cap               = m_limits.max_section_bytes;
    if (room_after_header < cap)
    {
        cap = room_after_header;
    }

    const crd::u64 original = bytes.size();
    if (original <= cap && m_compression == BundleCompression::Deflate)
    {
        // Whole sections only: a cut one stays raw, so Compressed and Truncated never meet. DEFLATE that does not
        // shrink the section is dropped (the ZIP container's rule).
        const cont::Array<crd::u8> packed = crd::compression::deflate_raw(bytes, m_alloc);
        if (packed.size() < original)
        {
            emit(tag, kSectionFlagCompressed, cont::ConstSpan<crd::u8>{packed.data(), packed.size()}, original);
            return AddStatus::Ok;
        }
    }

    crd::u64 stored = original;
    crd::u32       flags    = 0U;
    if (stored > cap)
    {
        stored = cap;
        flags  = kSectionFlagTruncated; // honest marker; original_length carries the pre-cut size
    }

    emit(tag, flags, cont::ConstSpan<crd::u8>{bytes.data(), static_cast<crd::usize>(stored)}, original);
    return (flags & kSectionFlagTruncated) ? AddStatus::StoredTruncated : AddStatus::Ok;
}

BundleWriter::AddStatus BundleWriter::add_absent(BundleSectionTag tag) noexcept
{
    if (m_finished)
    {
        return AddStatus::RejectedNoRoom;
    }
    if (m_section_count >= m_limits.max_sections)
    {
        return AddStatus::RejectedTooMany;
    }
    if (m_decoded >= m_limits.max_total_bytes || (m_limits.max_total_bytes - m_decoded) < sizeof(BundleSectionHeader))
    {
        return AddStatus::RejectedNoRoom;
    }
    emit(tag, kSectionFlagAbsent, {}, 0U);
    return AddStatus::StoredAbsent;
}

cont::Array<crd::u8> BundleWriter::finish() noexcept
{
    return finish(diagnostic_now_ns());
}

cont::Array<crd::u8> BundleWriter::finish(crd::u64 created_at_ns) noexcept
{
    cont::Array<crd::u8> out(m_alloc);
    if (m_finished)
    {
        return out;
    }
    m_finished = true;

    BundleHeader hdr{};
    hdr.magic          = kBundleMagic;
    hdr.schema_version = kDiagnosticSchemaVersion;
    hdr.format_version = kBundleFormatVersion;
    hdr.flags          = 0U;
    hdr.created_at_ns   = created_at_ns;
    hdr.total_len       = sizeof(BundleHeader) + m_sections.size();
    hdr.section_count   = m_section_count;
    hdr.header_crc32    = crc32_bytes(reinterpret_cast<const crd::u8*>(&hdr), offsetof(BundleHeader, header_crc32));

    // One buffer, filled through the same appends as the sections: the header write never sees the empty buffer a
    // wrapped total length would leave.
    out.reserve(sizeof(BundleHeader) + m_sections.size());
    append_bytes(out, &hdr, sizeof(BundleHeader));
    append_bytes(out, m_sections.data(), m_sections.size());
    return out;
}

// ---- Reader (round-trip + truncation recovery) --------------------------

namespace
{
// Walk the section stream after a valid header into r.sections, compressed sections still viewing their stored bytes.
// Sets r.status and r.stop_offset where the walk stopped, and returns the decoded bytes the compressed sections claim.
// A compressed section's claim is bounded here, before anything is allocated for it: by what DEFLATE can reach from
// its stored bytes, by the section cap, and with every section before it by the total cap.
crd::u64 walk_sections(cont::ConstSpan<crd::u8> buf, const BundleLimits& limits, BundleReadResult& r) noexcept
{
    crd::u64 offset = sizeof(BundleHeader);
    crd::u64 decoded = sizeof(BundleHeader); // the decoded bundle so far
    crd::u64 claimed = 0U;                   // the compressed sections' decoded bytes
    for (crd::u32 i = 0; i < r.header.section_count; ++i)
    {
        r.stop_offset = offset;
        if (buf.size() - offset < sizeof(BundleSectionHeader))
        {
            r.status = BundleReadStatus::RecoveredTruncated; // header cut mid-stream
            return claimed;
        }

        BundleSectionHeader sh{};
        std::memcpy(&sh, buf.data() + offset, sizeof(BundleSectionHeader));
        const crd::u64 payload_off = offset + sizeof(BundleSectionHeader);

        // Tag 0 is reserved so a zero-padded tail never reads as a real section (the writer never emits it); a
        // payload past the end is cut (or a bogus length); a crc mismatch is a corrupt section. Each stops the walk
        // with the recovered prefix kept.
        if (sh.tag == static_cast<crd::u32>(BundleSectionTag::Invalid) || sh.payload_length > buf.size() - payload_off)
        {
            r.status = BundleReadStatus::RecoveredTruncated;
            return claimed;
        }
        const cont::ConstSpan<crd::u8> payload{buf.data() + payload_off, static_cast<crd::usize>(sh.payload_length)};
        const crd::u32 expect = payload.empty() ? 0U : crc32_bytes(payload.data(), payload.size());
        if (expect != sh.payload_crc32)
        {
            r.status = BundleReadStatus::RecoveredTruncated;
            return claimed;
        }

        BundleSectionView view{};
        view.tag = static_cast<BundleSectionTag>(sh.tag);
        view.flags = sh.flags;
        view.original_length = sh.original_length;
        view.stored_length = sh.payload_length;
        view.payload = payload;
        crd::u64 held = sh.payload_length; // what the reader holds for this section
        if ((sh.flags & kSectionFlagCompressed) != 0U)
        {
            // A claim DEFLATE cannot reach from the stored bytes is a lying header, and a cut section is never
            // compressed: either stops the walk. A claim past a cap is the bomb defence: the section is listed
            // undecoded with an empty payload, as the importer lists an oversized section, and the walk goes on.
            if ((sh.flags & kSectionFlagTruncated) != 0U ||
                sh.original_length > crd::compression::deflate_max_inflated_size(sh.payload_length))
            {
                r.status = BundleReadStatus::RefusedCompressed;
                return claimed;
            }
            const crd::u64 room = limits.max_total_bytes - std::min(decoded, limits.max_total_bytes);
            held = 0U;
            view.payload = {};
            if (sh.original_length <= limits.max_section_bytes && room >= sizeof(BundleSectionHeader) &&
                sh.original_length <= room - sizeof(BundleSectionHeader))
            {
                held = sh.original_length;
                view.payload = payload; // decoded after the walk, once every claim is known
                claimed += held;
            }
        }
        decoded += sizeof(BundleSectionHeader) + held;
        r.sections.push_back(view);

        offset = payload_off + sh.payload_length;
    }
    r.status = BundleReadStatus::Ok;
    r.stop_offset = offset;
    return claimed;
}
} // namespace

BundleReadResult read_bundle(cont::ConstSpan<crd::u8> buf, BundleLimits limits, crd::memory::IAllocator* alloc) noexcept
{
    BundleReadResult r(alloc);

    if (buf.size() < sizeof(BundleHeader))
    {
        r.status = BundleReadStatus::HeaderTooSmall;
        return r;
    }

    BundleHeader hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(BundleHeader));
    r.header = hdr;

    if (hdr.magic != kBundleMagic)
    {
        r.status = BundleReadStatus::BadMagic;
        return r;
    }
    if (crc32_bytes(buf.data(), offsetof(BundleHeader, header_crc32)) != hdr.header_crc32)
    {
        r.status = BundleReadStatus::HeaderTooSmall; // header unusable -> section_count untrustworthy
        return r;
    }
    if (hdr.format_version != kBundleFormatVersion)
    {
        r.status                 = BundleReadStatus::UnsupportedVersion;
        r.declared_section_count = hdr.section_count;
        return r;
    }

    r.declared_section_count = hdr.section_count;
    const crd::u64 claimed = walk_sections(buf, limits, r);

    // Decode every compressed section of the recovered prefix into one allocation of exactly what passed the bounds;
    // the views move onto it. The first that cannot be decoded ends the prefix there.
    const bool reserved = r.decoded.try_reserve(static_cast<crd::usize>(claimed));
    for (crd::usize i = 0; i < r.sections.size(); ++i)
    {
        BundleSectionView& view = r.sections[i];
        const bool undecoded = view.payload.empty() && view.original_length != 0U; // its claim passed a cap
        if ((view.flags & kSectionFlagCompressed) == 0U || undecoded)
        {
            continue;
        }
        const crd::usize at = r.decoded.size();
        bool ok = reserved;
        if (ok)
        {
            r.decoded.resize_uninitialized(at + static_cast<crd::usize>(view.original_length));
            ok = crd::compression::inflate_raw(view.payload,
                                               cont::Span<crd::u8>{r.decoded.data() + at, r.decoded.size() - at});
        }
        if (!ok)
        {
            r.status = BundleReadStatus::RefusedCompressed;
            r.stop_offset = static_cast<crd::u64>(view.payload.data() - buf.data()) - sizeof(BundleSectionHeader);
            r.sections.resize(i);
            r.decoded.resize_uninitialized(at);
            return r;
        }
        view.payload = cont::ConstSpan<crd::u8>{r.decoded.data() + at, r.decoded.size() - at};
    }
    return r;
}

// ---- Publish, retention and existence: crd-platform's files (OWN.1, io.durable-saves and io.files) -----------

bool write_bundle_atomic(const char* path, cont::ConstSpan<crd::u8> bundle) noexcept
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }
    return fs::write_file_atomic(fs::Path{path}, bundle);
}

crd::i64 enforce_bundle_retention(const char* dir, const char* prefix, const char* suffix,
                                  crd::u32 max_bundles) noexcept
{
    if (dir == nullptr || !fs::is_directory(fs::Path{dir}))
    {
        return -1;
    }

    struct Entry
    {
        fs::Path path;
        crd::i64 mtime_ns;
    };
    cont::Array<fs::Path> listed;
    fs::list_directory(fs::Path{dir}, listed);
    cont::Array<Entry> entries;
    for (fs::Path& path : listed)
    {
        const cont::StringView name = file_name(path.generic());
        if (name_matches(name, prefix, suffix) && fs::is_file(path))
        {
            const crd::i64 mtime_ns = fs::last_modified_unix_ns(path);
            entries.push_back(Entry{std::move(path), mtime_ns});
        }
    }
    if (entries.size() <= static_cast<crd::usize>(max_bundles))
    {
        return 0;
    }

    // Oldest first, ties broken by name, so eviction is deterministic when a file system's clock gives two writes
    // one time.
    std::sort(entries.data(), entries.data() + entries.size(), [](const Entry& a, const Entry& b) noexcept {
        if (a.mtime_ns != b.mtime_ns)
        {
            return a.mtime_ns < b.mtime_ns;
        }
        return file_name(a.path.generic()) < file_name(b.path.generic());
    });

    const crd::usize to_delete = entries.size() - static_cast<crd::usize>(max_bundles);
    crd::i64         deleted   = 0;
    for (crd::usize i = 0; i < to_delete; ++i)
    {
        if (fs::remove_file(entries[i].path))
        {
            ++deleted;
        }
    }
    return deleted;
}

bool bundle_directory_exists(const char* path) noexcept
{
    return path != nullptr && path[0] != '\0' && fs::is_directory(fs::Path{path});
}

bool bundle_path_exists(const char* path) noexcept
{
    return path != nullptr && path[0] != '\0' && fs::exists(fs::Path{path});
}

} // namespace crd::perf
