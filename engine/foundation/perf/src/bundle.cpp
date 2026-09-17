// crd-perf -- cerid-diagnostics/1 post-mortem bundle container + writer (DIAG.5d(b)).
// See bundle.hpp for the format and rationale. This TU packages bytes handed to it; it
// captures no live process state and depends on no logging (crd-perf does not link crd-log).

#include <crd/perf/bundle.hpp>

#include <crd/containers/string.hpp>
#include <crd/perf/diagnostics.hpp> // diagnostic_now_ns(), kDiagnosticSchemaVersion

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>      // _fileno, _get_osfhandle
#include <windows.h> // FlushFileBuffers, MoveFileExA, FindFirstFileA
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h> // fsync
#endif

namespace crd::perf
{
namespace cont = crd::containers;

namespace
{
// ---- crc32 (IEEE 802.3, poly 0xEDB88320) -- self-contained; crd-perf must not depend on
// the codec stack in crd-resources/crd-numerics that carry the other crc32 tables. --------
struct Crc32Table
{
    crd::u32 v[256];
};

constexpr Crc32Table make_crc32_table() noexcept
{
    Crc32Table t{};
    for (crd::u32 i = 0; i < 256U; ++i)
    {
        crd::u32 c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        t.v[i] = c;
    }
    return t;
}

constexpr Crc32Table kCrc32 = make_crc32_table();

crd::u32 crc32_bytes(const crd::u8* p, crd::usize n) noexcept
{
    crd::u32 c = 0xFFFFFFFFU;
    for (crd::usize i = 0; i < n; ++i)
        c = kCrc32.v[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

void append_bytes(cont::Array<crd::u8>& a, const void* src, crd::usize n) noexcept
{
    if (n == 0)
        return;
    const crd::usize old = a.size();
    a.resize_uninitialized(old + n);
    std::memcpy(a.data() + old, src, n);
}

bool str_starts_with(const char* s, const char* pfx) noexcept
{
    if (pfx == nullptr)
        return true;
    for (; *pfx != '\0'; ++s, ++pfx)
        if (*s != *pfx)
            return false;
    return true;
}

bool str_ends_with(const char* s, const char* sfx) noexcept
{
    if (sfx == nullptr || *sfx == '\0')
        return true;
    const crd::usize ls = std::strlen(s);
    const crd::usize lf = std::strlen(sfx);
    if (lf > ls)
        return false;
    return std::memcmp(s + (ls - lf), sfx, lf) == 0;
}
} // namespace

// ---- BundleWriter -------------------------------------------------------

BundleWriter::BundleWriter(crd::memory::IAllocator* alloc, BundleLimits limits) noexcept
    : m_alloc(alloc), m_limits(limits), m_sections(alloc)
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

    m_total += sizeof(BundleSectionHeader) + payload.size();
    ++m_section_count;
}

BundleWriter::AddStatus BundleWriter::add_section(BundleSectionTag tag, cont::ConstSpan<crd::u8> bytes) noexcept
{
    if (m_finished)
        return AddStatus::RejectedNoRoom;
    if (m_section_count >= m_limits.max_sections)
        return AddStatus::RejectedTooMany;

    // Room accounting first: every section (an empty one included) costs at least its header
    // against the total cap, so the empty branch must not bypass this check.
    if (m_total >= m_limits.max_total_bytes ||
        (m_limits.max_total_bytes - m_total) < sizeof(BundleSectionHeader))
        return AddStatus::RejectedNoRoom;

    if (bytes.empty())
    {
        emit(tag, kSectionFlagAbsent, {}, 0U);
        return AddStatus::StoredAbsent;
    }

    const crd::u64 room_after_header = (m_limits.max_total_bytes - m_total) - sizeof(BundleSectionHeader);
    crd::u64       cap               = m_limits.max_section_bytes;
    if (room_after_header < cap)
        cap = room_after_header;

    const crd::u64 original = bytes.size();
    crd::u64       stored   = original;
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
        return AddStatus::RejectedNoRoom;
    if (m_section_count >= m_limits.max_sections)
        return AddStatus::RejectedTooMany;
    if (m_total >= m_limits.max_total_bytes ||
        (m_limits.max_total_bytes - m_total) < sizeof(BundleSectionHeader))
        return AddStatus::RejectedNoRoom;
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
        return out;
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

    out.resize_uninitialized(sizeof(BundleHeader) + m_sections.size());
    std::memcpy(out.data(), &hdr, sizeof(BundleHeader));
    if (!m_sections.empty())
        std::memcpy(out.data() + sizeof(BundleHeader), m_sections.data(), m_sections.size());
    return out;
}

// ---- Reader (round-trip + truncation recovery) --------------------------

BundleReadResult read_bundle(cont::ConstSpan<crd::u8> buf, crd::memory::IAllocator* alloc) noexcept
{
    // Aggregate-init providing the Array explicitly (its default ctor is `explicit`, so a braced
    // value-init of the whole struct would not compile).
    BundleReadResult r{BundleReadStatus::Ok, BundleHeader{}, 0U, 0U, cont::Array<BundleSectionView>(alloc)};

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
    crd::u64 offset          = sizeof(BundleHeader);

    for (crd::u32 i = 0; i < hdr.section_count; ++i)
    {
        if (buf.size() - offset < sizeof(BundleSectionHeader))
        {
            r.status      = BundleReadStatus::RecoveredTruncated; // header cut mid-stream
            r.stop_offset = offset;
            return r;
        }

        BundleSectionHeader sh{};
        std::memcpy(&sh, buf.data() + offset, sizeof(BundleSectionHeader));
        const crd::u64 payload_off = offset + sizeof(BundleSectionHeader);

        if (sh.tag == static_cast<crd::u32>(BundleSectionTag::Invalid))
        {
            // Tag 0 is reserved so a zero-padded tail never reads as a real section (the writer
            // never emits it). Stop and keep the recovered prefix.
            r.status      = BundleReadStatus::RecoveredTruncated;
            r.stop_offset = offset;
            return r;
        }
        if (sh.flags & kSectionFlagCompressed)
        {
            r.status      = BundleReadStatus::UnsupportedFeature; // no codec edge in v1
            r.stop_offset = offset;
            return r;
        }
        if (sh.payload_length > buf.size() - payload_off)
        {
            r.status      = BundleReadStatus::RecoveredTruncated; // payload cut (or bogus length beyond EOF)
            r.stop_offset = offset;
            return r;
        }

        const cont::ConstSpan<crd::u8> payload{buf.data() + payload_off,
                                               static_cast<crd::usize>(sh.payload_length)};
        const crd::u32 expect = payload.empty() ? 0U : crc32_bytes(payload.data(), payload.size());
        if (expect != sh.payload_crc32)
        {
            r.status      = BundleReadStatus::RecoveredTruncated; // corrupt section -> keep the valid prefix
            r.stop_offset = offset;
            return r;
        }

        BundleSectionView view{};
        view.tag             = static_cast<BundleSectionTag>(sh.tag);
        view.flags           = sh.flags;
        view.original_length = sh.original_length;
        view.payload         = payload;
        r.sections.push_back(view);

        offset = payload_off + sh.payload_length;
    }

    r.status      = BundleReadStatus::Ok;
    r.stop_offset = offset;
    return r;
}

// ---- Atomic publish -----------------------------------------------------

bool write_bundle_atomic(const char* path, cont::ConstSpan<crd::u8> bundle) noexcept
{
    if (path == nullptr)
        return false;

    cont::String tmp{path};
    tmp.append(".tmp");

    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, tmp.c_str(), "wb") != 0)
        f = nullptr;
#else
    f = std::fopen(tmp.c_str(), "wb");
#endif
    if (f == nullptr)
        return false;

    bool ok = bundle.empty() || std::fwrite(bundle.data(), 1, bundle.size(), f) == bundle.size();
    if (ok)
        ok = std::fflush(f) == 0;
    if (ok)
    {
        // Flush to stable storage BEFORE the rename, so the published file is durable.
#if defined(_WIN32)
        const int fd = _fileno(f);
        if (fd >= 0)
        {
            HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
            if (h != INVALID_HANDLE_VALUE)
                ok = FlushFileBuffers(h) != 0;
        }
#else
        const int fd = fileno(f);
        if (fd >= 0)
            ok = fsync(fd) == 0;
#endif
    }
    std::fclose(f); // must close before renaming over the destination on Windows

    if (ok)
    {
#if defined(_WIN32)
        ok = MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = std::rename(tmp.c_str(), path) == 0;
#endif
    }

    if (!ok)
        std::remove(tmp.c_str()); // best-effort: never leave a stale .tmp on failure
    return ok;
}

// ---- Retention ----------------------------------------------------------

crd::i64 enforce_bundle_retention(const char* dir, const char* prefix, const char* suffix,
                                  crd::u32 max_bundles) noexcept
{
    if (dir == nullptr)
        return -1;

    struct Entry
    {
        cont::String name;
        crd::u64     mtime; // platform-local units; only relative order within this dir is used
    };
    cont::Array<Entry> entries;

#if defined(_WIN32)
    cont::String pattern{dir};
    pattern.append("\\*");
    WIN32_FIND_DATAA fd{};
    HANDLE           h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!str_starts_with(fd.cFileName, prefix) || !str_ends_with(fd.cFileName, suffix))
            continue;
        const crd::u64 mt = (static_cast<crd::u64>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                            static_cast<crd::u64>(fd.ftLastWriteTime.dwLowDateTime);
        entries.push_back(Entry{cont::String{fd.cFileName}, mt});
    } while (FindNextFileA(h, &fd) != 0);
    FindClose(h);
    const char sep = '\\';
#else
    DIR* d = ::opendir(dir);
    if (d == nullptr)
        return -1;
    for (struct dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d))
    {
        if (!str_starts_with(e->d_name, prefix) || !str_ends_with(e->d_name, suffix))
            continue;
        cont::String full{dir};
        full.append("/");
        full.append(e->d_name);
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            continue;
        const crd::u64 mt = static_cast<crd::u64>(st.st_mtim.tv_sec) * 1000000000ULL +
                            static_cast<crd::u64>(st.st_mtim.tv_nsec);
        entries.push_back(Entry{cont::String{e->d_name}, mt});
    }
    ::closedir(d);
    const char sep = '/';
#endif

    if (entries.size() <= static_cast<crd::usize>(max_bundles))
        return 0;

    // Oldest first (ascending mtime); ties broken by name so eviction is deterministic even
    // when a filesystem's mtime granularity coalesces same-second writes.
    std::sort(entries.data(), entries.data() + entries.size(), [](const Entry& a, const Entry& b) noexcept {
        if (a.mtime != b.mtime)
            return a.mtime < b.mtime;
        return std::strcmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    const crd::usize to_delete = entries.size() - static_cast<crd::usize>(max_bundles);
    crd::i64         deleted   = 0;
    for (crd::usize i = 0; i < to_delete; ++i)
    {
        cont::String full{dir};
        full.push_back(sep);
        full.append(entries[i].name.c_str());
        if (std::remove(full.c_str()) == 0)
            ++deleted;
    }
    return deleted;
}

} // namespace crd::perf
