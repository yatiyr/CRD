// crd-perf -- DIAG.5d(e1): the trustworthy bundle importer. See bundle_import.hpp.

#include <crd/perf/bundle_import.hpp>

#include <crd/perf/diagnostics.hpp> // kDiagnosticSchemaVersion

#include <cstring>

namespace crd::perf
{
namespace cont = crd::containers;

namespace
{
bool name_is_traversal(const ModuleIdentity& m) noexcept
{
    if (m.id_kind == SymbolIdKind::Rsds)
        return !is_safe_lookup_name(cont::StringView{m.debug_file.c_str(), m.debug_file.size()});
    if (m.id_kind == SymbolIdKind::PeImage)
        return !is_safe_lookup_name(cont::StringView{m.name.c_str(), m.name.size()});
    return false; // GnuBuildId is hex-only; None has no path
}
} // namespace

BundleImport import_bundle(cont::ConstSpan<crd::u8> buf, BundleLimits limits, crd::memory::IAllocator* alloc) noexcept
{
    BundleImport r;
    r.symbols  = cont::Array<ModuleIdentity>(alloc);
    r.sections = cont::Array<ImportedSection>(alloc);
    r.status   = ImportStatus::Rejected;

    // 1. Oversized -- refuse before reading a single byte.
    if (buf.size() > limits.max_total_bytes)
    {
        r.reject = ImportReject::Oversized;
        return r;
    }

    // 2. Peek the declared section_count BEFORE the structural walk so a hostile count cannot make read_bundle allocate
    //    a view per declared section; a crc-corrupt header with a huge count is rejected here as TooManySections rather
    //    than BadHeader (rejected either way). The post-read_bundle check at step 4 stays as belt-and-braces.
    if (buf.size() >= sizeof(BundleHeader))
    {
        BundleHeader peek{};
        std::memcpy(&peek, buf.data(), sizeof(peek));
        if (peek.magic == kBundleMagic && peek.section_count > limits.max_sections)
        {
            r.reject = ImportReject::TooManySections;
            return r;
        }
    }

    // 3. Structural read.
    BundleReadResult rb = read_bundle(buf, alloc);
    if (rb.status == BundleReadStatus::BadMagic)
    {
        r.reject = ImportReject::BadMagic;
        return r;
    }
    if (rb.status == BundleReadStatus::HeaderTooSmall)
    {
        r.reject = ImportReject::BadHeader; // header unusable -> do not trust any field
        return r;
    }
    if (rb.status == BundleReadStatus::UnsupportedVersion)
    {
        r.header                 = rb.header;
        r.declared_section_count = rb.declared_section_count;
        r.reject                 = ImportReject::UnsupportedVersion;
        return r;
    }

    // Header is valid from here (Ok / RecoveredTruncated / UnsupportedFeature).
    r.header                 = rb.header;
    r.declared_section_count = rb.declared_section_count;
    r.stop_offset            = rb.stop_offset;

    // 4. Schema mismatch -- read_bundle checked only the container format_version; the cerid-diagnostics schema is the
    //    importer's responsibility.
    if (rb.header.schema_version != kDiagnosticSchemaVersion)
    {
        r.reject = ImportReject::SchemaMismatch;
        return r;
    }

    // 5. Too many sections -- belt-and-braces: step 2 already rejected a hostile declared count before the walk; this
    //    re-checks the validated header in case a future read_bundle path reports a different section_count.
    if (rb.header.section_count > limits.max_sections)
    {
        r.reject = ImportReject::TooManySections;
        return r;
    }

    if (rb.status == BundleReadStatus::UnsupportedFeature)
        r.compressed_unsupported = true; // a compressed section was refused; the prefix is kept (v1 bomb defence)

    r.total_len_mismatch = (rb.header.total_len != buf.size());

    // 6. Per-section: cap, duplicate detection, decode Manifest/SymbolIndex.
    for (crd::usize i = 0; i < rb.sections.size(); ++i)
    {
        const BundleSectionView& sv = rb.sections[i];

        ImportedSection is{};
        is.tag             = sv.tag;
        is.section_flags   = sv.flags;
        is.import_flags    = 0U;
        is.original_length = sv.original_length;
        is.payload         = sv.payload;

        if (sv.payload.size() > limits.max_section_bytes)
        {
            is.import_flags |= kImportSectionOversized;
            is.payload = cont::ConstSpan<crd::u8>{}; // never decode an oversized section
        }

        bool dup = false;
        for (crd::usize j = 0; j < r.sections.size(); ++j)
            if (r.sections[j].tag == sv.tag)
            {
                dup = true;
                break;
            }
        if (dup)
            is.import_flags |= kImportSectionDuplicate;

        if (!dup && (is.import_flags & kImportSectionOversized) == 0U)
        {
            if (sv.tag == BundleSectionTag::Manifest && !r.has_manifest)
            {
                r.manifest     = read_manifest(sv.payload, alloc);
                r.has_manifest = r.manifest.ok;
            }
            else if (sv.tag == BundleSectionTag::SymbolIndex && !r.has_symbols)
            {
                r.symbols     = read_symbol_index(sv.payload, alloc);
                r.has_symbols = true;
                for (crd::usize k = 0; k < r.symbols.size(); ++k)
                    if (name_is_traversal(r.symbols[k]))
                        ++r.unsafe_names;
            }
        }

        r.sections.push_back(is);
    }

    // Cross-check the manifest's declared-absent tags against the actual sections (report, never reject: the data is
    // still readable; a mismatch is a manifest/section disagreement worth surfacing).
    if (r.has_manifest)
        for (crd::usize a = 0; a < r.manifest.absent_tags.size(); ++a)
        {
            const crd::u32 tag     = r.manifest.absent_tags[a];
            bool           matched = false;
            for (crd::usize s = 0; s < r.sections.size(); ++s)
                if (static_cast<crd::u32>(r.sections[s].tag) == tag &&
                    (r.sections[s].section_flags & kSectionFlagAbsent) != 0U)
                {
                    matched = true;
                    break;
                }
            if (!matched)
                ++r.manifest_absent_mismatches;
        }

    r.status = (rb.status == BundleReadStatus::Ok) ? ImportStatus::Ok : ImportStatus::RecoveredTruncated;
    return r;
}

} // namespace crd::perf
