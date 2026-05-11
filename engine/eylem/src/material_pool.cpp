// MaterialPool impl. Phase 3.1 v1a-material-b (ADR-0069 §3 + §11).

#include <crd/eylem/material_pool.hpp>

namespace crd::eylem
{
MaterialPool::MaterialPool(crd::memory::IAllocator* alloc)
    : m_materials(alloc != nullptr ? alloc : crd::memory::default_allocator())
{
    m_materials.reserve(2U); // slot 0 + slot 1 minimum

    // Slot 0 — null sentinel. The bytes don't matter (never read on the
    // happy path); contains() short-circuits on `id.is_null()` /
    // `id.index() == 0`. Push a default-constructed Material so the
    // underlying Array still owns valid memory at the slot.
    m_materials.push_back(Material{});

    // Slot 1 — the shipped default material. `MaterialId::default_material()`
    // statically resolves to (index=1, generation=1) per types.hpp; this
    // slot must always exist for the read API's fallback-to-default
    // behaviour to work.
    m_materials.push_back(default_material_value());
}

MaterialId MaterialPool::insert(const Material& material)
{
    const crd::usize current = m_materials.size();
    if (current >= static_cast<crd::usize>(kIndexMax))
    {
        return MaterialId::null();
    }

    const crd::u32 idx = static_cast<crd::u32>(current);
    m_materials.push_back(material);
    // v1: generation always 1 for live slots (no remove → no reuse).
    // v9+ may add remove with generation-bump; the API stays stable.
    return MaterialId::make(idx, /*generation=*/1U);
}

void MaterialPool::update(MaterialId id, const Material& material) noexcept
{
    if (!contains(id))
    {
        return;
    }
    m_materials[id.index()] = material;
}

const Material& MaterialPool::get(MaterialId id) const noexcept
{
    if (!contains(id))
    {
        // Fallback: callers do not need to null-check on the read path.
        // Slot 1 is the shipped default and always exists per the ctor.
        return m_materials[1];
    }
    return m_materials[id.index()];
}

bool MaterialPool::contains(MaterialId id) const noexcept
{
    if (id.is_null())                     return false;
    if (id.index() == 0U)                 return false; // slot 0 is reserved null
    if (id.index() >= m_materials.size()) return false;
    // v1: every live slot has generation == 1. v9+ remove path bumps.
    return id.generation() == 1U;
}

crd::usize MaterialPool::size() const noexcept
{
    // Excludes the slot-0 null sentinel; includes the default at slot 1.
    return m_materials.size() > 0U ? (m_materials.size() - 1U) : 0U;
}

crd::containers::ConstSpan<Material> MaterialPool::all() const noexcept
{
    return crd::containers::ConstSpan<Material>(m_materials.data(), m_materials.size());
}

} // namespace crd::eylem
