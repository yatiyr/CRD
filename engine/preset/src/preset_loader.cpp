// Phase 3.0 v1n1 — PresetLoader implementation (ADR-0059).
//
// Validates the CRDR header (type_fourcc + payload-size invariants), parses
// the three known chunks (PINF, PDAT, PCHN), and constructs an
// allocator-owned PresetResource. Hard-fails (returns nullptr) on any
// inconsistency; v1n1 has no soft fallback.

#include <crd/core/assert.hpp>
#include <crd/preset/preset_loader.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/resources/crdr.hpp>

#include <cstring>

namespace crd::preset
{

PresetLoader::PresetLoader(crd::u32 fourcc,
                           crd::u32 schema_version,
                           crd::u32 payload_size,
                           crd::memory::IAllocator* alloc) noexcept
    : m_fourcc(fourcc)
    , m_schema_version(schema_version)
    , m_payload_size(payload_size)
    , m_alloc(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr, "PresetLoader: null allocator");
    CRD_ASSERT_MSG(fourcc != 0U,     "PresetLoader: zero fourcc is reserved");
}

void* PresetLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file{m_alloc};
    if (crd::resources::crdr_read(ctx.bytes, file, m_alloc) != crd::resources::CrdrError::Ok)
    {
        return nullptr;
    }
    if (file.type_fourcc != m_fourcc)
    {
        return nullptr;
    }

    const crd::resources::CrdrChunk* pinf = crd::resources::crdr_find_chunk(file, kFourCC_PINF);
    const crd::resources::CrdrChunk* pdat = crd::resources::crdr_find_chunk(file, kFourCC_PDAT);
    if (pinf == nullptr || pdat == nullptr)
    {
        return nullptr;
    }
    if (pinf->payload.size() != sizeof(PresetInfo))
    {
        return nullptr;
    }

    PresetInfo info{};
    std::memcpy(&info, pinf->payload.data(), sizeof(PresetInfo));

    if (info.payload_size != m_payload_size)
    {
        return nullptr;
    }
    if (pdat->payload.size() != m_payload_size)
    {
        return nullptr;
    }

    auto* res = static_cast<PresetResource*>(m_alloc->allocate(sizeof(PresetResource), alignof(PresetResource)));
    if (res == nullptr)
    {
        return nullptr;
    }
    new (res) PresetResource(m_alloc);
    res->set_fourcc(m_fourcc);
    res->set_schema_version(info.schema_version);

    res->mutable_bytes().resize(static_cast<crd::usize>(m_payload_size));
    std::memcpy(res->mutable_bytes().data(), pdat->payload.data(), m_payload_size);

    if (const auto* pchn = crd::resources::crdr_find_chunk(file, kFourCC_PCHN); pchn != nullptr)
    {
        // PCHN layout: u32 entry_count + u32 reserved + PresetChainEntry[entry_count].
        constexpr crd::usize k_header = 8U;
        if (pchn->payload.size() < k_header)
        {
            res->~PresetResource();
            m_alloc->deallocate(res);
            return nullptr;
        }

        crd::u32 entry_count = 0U;
        std::memcpy(&entry_count, pchn->payload.data(), sizeof(entry_count));

        const crd::usize expected = k_header + static_cast<crd::usize>(entry_count) * sizeof(PresetChainEntry);
        if (pchn->payload.size() != expected)
        {
            res->~PresetResource();
            m_alloc->deallocate(res);
            return nullptr;
        }

        res->mutable_chain().resize(static_cast<crd::usize>(entry_count));
        if (entry_count > 0U)
        {
            std::memcpy(res->mutable_chain().data(),
                        pchn->payload.data() + k_header,
                        static_cast<crd::usize>(entry_count) * sizeof(PresetChainEntry));
        }
    }

    return res;
}

void PresetLoader::unload(void* payload) noexcept
{
    if (payload == nullptr)
    {
        return;
    }
    auto* res = static_cast<PresetResource*>(payload);
    res->~PresetResource();
    m_alloc->deallocate(res);
}

} // namespace crd::preset
