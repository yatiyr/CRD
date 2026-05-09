// Phase 3.0 v1n1 — PresetRegistry implementation (ADR-0059).

#include <crd/core/assert.hpp>
#include <crd/preset/preset_registry.hpp>

namespace crd::preset
{

PresetRegistry::PresetRegistry(crd::memory::IAllocator* alloc)
    : m_alloc(alloc)
    , m_types(alloc)
    , m_by_fourcc(alloc)
    , m_owned_loaders(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr, "PresetRegistry: null allocator");
}

PresetTypeInfo& PresetRegistry::register_raw(crd::u32 fourcc,
                                             crd::u32 schema_version,
                                             crd::u32 size_bytes,
                                             crd::u32 alignment,
                                             crd::containers::StringView name)
{
    CRD_ASSERT_MSG(fourcc != 0U,        "PresetRegistry::register_raw: zero fourcc reserved");
    CRD_ASSERT_MSG(size_bytes > 0U,     "PresetRegistry::register_raw: zero-byte schema");
    CRD_ASSERT_MSG(schema_version >= 1U,"PresetRegistry::register_raw: schema_version < 1");

    // Idempotent re-registration (matches ComponentRegistry, ADR-0050 §1).
    if (const crd::u32* idx_ptr = m_by_fourcc.find(fourcc); idx_ptr != nullptr)
    {
        return m_types[*idx_ptr];
    }

    auto loader_owner = std::make_unique<PresetLoader>(fourcc, schema_version, size_bytes, m_alloc);
    PresetLoader* loader_view = loader_owner.get();

    // Aggregate brace-init avoids requiring a default ctor for the String
    // member (whose default ctor is `explicit`, which trips MSVC's value-init
    // path under PresetTypeInfo info{};).
    const crd::u32 idx = static_cast<crd::u32>(m_types.size());
    m_types.push_back(PresetTypeInfo{
        fourcc,
        schema_version,
        size_bytes,
        alignment,
        crd::containers::String(name, m_alloc),
        loader_view,
    });
    m_by_fourcc.emplace(fourcc, idx);
    m_owned_loaders.push_back(std::move(loader_owner));

    return m_types[idx];
}

const PresetTypeInfo* PresetRegistry::find(crd::u32 fourcc) const noexcept
{
    const crd::u32* idx_ptr = m_by_fourcc.find(fourcc);
    if (idx_ptr == nullptr)
    {
        return nullptr;
    }
    return &m_types[*idx_ptr];
}

const PresetTypeInfo* PresetRegistry::find(crd::containers::StringView name) const noexcept
{
    for (const auto& info : m_types)
    {
        if (info.name == name)
        {
            return &info;
        }
    }
    return nullptr;
}

} // namespace crd::preset
