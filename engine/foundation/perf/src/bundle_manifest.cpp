// crd-perf -- DIAG.5d(e1): the Manifest bundle section payload. See bundle_manifest.hpp.
// No field here can declare the raw dump redacted (ID-8); redaction refers only to the manifest's own strings.

#include <crd/perf/bundle_manifest.hpp>

#include <cstring>

namespace crd::perf
{
namespace cont = crd::containers;

namespace
{
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
} // namespace

cont::Array<crd::u8> serialize_manifest(const BundleManifest& m, crd::memory::IAllocator* alloc) noexcept
{
    cont::Array<crd::u8> out(alloc);

    ManifestHeader hdr{};
    hdr.manifest_version = kManifestVersion;
    hdr.schema_version   = m.schema_version;
    hdr.flags            = m.flags;
    hdr.absent_count     = static_cast<crd::u32>(m.absent_tags.size());

    append_bytes(out, &hdr, sizeof(hdr));
    if (!m.absent_tags.empty())
    {
        append_bytes(out, m.absent_tags.data(), m.absent_tags.size() * sizeof(crd::u32));
    }
    return out;
}

ManifestReadResult read_manifest(cont::ConstSpan<crd::u8> payload, crd::memory::IAllocator* alloc) noexcept
{
    ManifestReadResult r;
    r.absent_tags = cont::Array<crd::u32>(alloc);

    if (payload.size() < sizeof(ManifestHeader))
    {
        return r; // ok stays false
    }
    ManifestHeader hdr{};
    std::memcpy(&hdr, payload.data(), sizeof(hdr));
    if (hdr.manifest_version != kManifestVersion)
    {
        return r;
    }

    // Bound absent_count by the closed cap AND by what the payload can actually hold -- refuse a hostile count
    // rather than trusting it (no allocation sized by an unchecked field).
    const crd::u64 avail = payload.size() - sizeof(ManifestHeader);
    if (hdr.absent_count > kMaxManifestAbsentTags ||
        static_cast<crd::u64>(hdr.absent_count) * sizeof(crd::u32) > avail)
    {
        return r;
    }

    r.manifest_version = hdr.manifest_version;
    r.schema_version   = hdr.schema_version;
    r.flags            = hdr.flags;
    r.unknown_flags    = hdr.flags & ~kManifestKnownFlags; // reported, never interpreted
    if (hdr.absent_count != 0U)
    {
        r.absent_tags.resize_uninitialized(hdr.absent_count);
        std::memcpy(r.absent_tags.data(), payload.data() + sizeof(ManifestHeader),
                    static_cast<crd::usize>(hdr.absent_count) * sizeof(crd::u32));
    }
    r.ok = true;
    return r;
}

} // namespace crd::perf
