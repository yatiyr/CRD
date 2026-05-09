// Phase 3.0 v1n1 — PresetArtifactBuilder implementation (ADR-0059).
//
// Emits a CRDR blob with the PINF/PDAT/PCHN chunks. CrdrWriter sorts chunks
// by FourCC at finish() time, so the on-disk byte order is deterministic
// (PCHN < PDAT < PINF lexicographically).

#include <crd/core/assert.hpp>
#include <crd/preset/preset_artifact_builder.hpp>
#include <crd/resources/crdr.hpp>

#include <cstring>

namespace crd::preset
{

PresetArtifactBuilder::PresetArtifactBuilder(crd::memory::IAllocator*    alloc,
                                             crd::u32                    type_fourcc,
                                             crd::u32                    schema_version,
                                             crd::resources::ResourceId  id) noexcept
    : m_alloc(alloc)
    , m_fourcc(type_fourcc)
    , m_schema_version(schema_version)
    , m_id(id)
    , m_payload(alloc)
    , m_chain(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr,        "PresetArtifactBuilder: null allocator");
    CRD_ASSERT_MSG(type_fourcc != 0U,       "PresetArtifactBuilder: zero fourcc reserved");
    CRD_ASSERT_MSG(schema_version >= 1U,    "PresetArtifactBuilder: schema_version < 1");
}

void PresetArtifactBuilder::set_payload(crd::containers::ConstSpan<crd::u8> bytes)
{
    m_payload.resize(bytes.size());
    if (!bytes.empty())
    {
        std::memcpy(m_payload.data(), bytes.data(), bytes.size());
    }
}

void PresetArtifactBuilder::add_chain_dependency(crd::u64 path_hash, crd::u64 content_hash)
{
    PresetChainEntry e{};
    e.path_hash    = path_hash;
    e.content_hash = content_hash;
    m_chain.push_back(e);
}

crd::containers::Array<crd::u8> PresetArtifactBuilder::build() const
{
    CRD_ASSERT_MSG(!m_payload.empty(),
                   "PresetArtifactBuilder::build: payload not set (call set_payload first)");

    crd::resources::CrdrWriter writer{m_alloc, m_id, m_fourcc};

    // PINF (16 bytes).
    PresetInfo info{};
    info.schema_version = m_schema_version;
    info.flags          = 0U;
    info.payload_size   = static_cast<crd::u32>(m_payload.size());
    info.reserved       = 0U;

    writer.add_chunk(kFourCC_PINF,
                     crd::containers::ConstSpan<crd::u8>{
                         reinterpret_cast<const crd::u8*>(&info), sizeof(info)});

    // PDAT.
    writer.add_chunk(kFourCC_PDAT,
                     crd::containers::ConstSpan<crd::u8>{m_payload.data(), m_payload.size()});

    // PCHN — only emitted when chain entries exist (keeps no-extends presets minimal).
    if (!m_chain.empty())
    {
        crd::containers::Array<crd::u8> pchn_bytes(m_alloc);
        const crd::u32 entry_count = static_cast<crd::u32>(m_chain.size());
        const crd::u32 reserved    = 0U;

        const crd::usize total =
            sizeof(crd::u32) + sizeof(crd::u32) + entry_count * sizeof(PresetChainEntry);
        pchn_bytes.resize(total);

        std::memcpy(pchn_bytes.data() + 0,                   &entry_count, sizeof(crd::u32));
        std::memcpy(pchn_bytes.data() + sizeof(crd::u32),    &reserved,    sizeof(crd::u32));
        std::memcpy(pchn_bytes.data() + 2U * sizeof(crd::u32),
                    m_chain.data(),
                    entry_count * sizeof(PresetChainEntry));

        writer.add_chunk(kFourCC_PCHN,
                         crd::containers::ConstSpan<crd::u8>{pchn_bytes.data(), pchn_bytes.size()});
    }

    return writer.finish();
}

} // namespace crd::preset
