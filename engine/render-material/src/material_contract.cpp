#include <crd/rendermaterial/material_contract.hpp>

#include <crd/containers/hash.hpp>

#include <utility> // std::swap

namespace crd::rendermaterial
{
using crd::containers::hash_u64;

// ── RuntimeMaterialDefinition ──
bool RuntimeMaterialDefinition::add_surface_output(RenderChannel channel, DiagnosticList& diags)
{
    if (is_lighting_channel(channel))
    {
        diags.error(DiagCode::MaterialLightingAccess,
                    "a material may only produce SURFACE channels, never lighting state");
        return false;
    }
    m_surface_outputs.push_back(channel);
    return true;
}

const MaterialParam* RuntimeMaterialDefinition::find_param(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_params.size(); ++i)
    {
        if (m_params[i].name_hash == name_hash)
        {
            return &m_params[i];
        }
    }
    return nullptr;
}

bool RuntimeMaterialDefinition::validate(DiagnosticList& diags) const
{
    for (u32 i = 0; i < m_surface_outputs.size(); ++i)
    {
        if (is_lighting_channel(m_surface_outputs[i]))
        {
            diags.error(DiagCode::MaterialLightingAccess, "material surface contract contains a lighting channel");
            return false;
        }
    }
    return true;
}

// ── RuntimeMaterialInstance ──
bool RuntimeMaterialInstance::validate(const RuntimeMaterialDefinition& def, DiagnosticList& diags) const
{
    // Every override must target an existing param with a matching type.
    for (u32 i = 0; i < m_overrides.size(); ++i)
    {
        const ParamOverride& o = m_overrides[i];
        const MaterialParam* p = def.find_param(o.name_hash);
        if (p == nullptr || p->type != o.type)
        {
            diags.error(DiagCode::InvalidOverride, "material instance overrides an unknown or type-mismatched param");
            return false;
        }
    }
    // Every undefaulted texture/sampler param must be bound by an override that provides a value.
    for (u32 i = 0; i < def.params().size(); ++i)
    {
        const MaterialParam& p = def.params()[i];
        const bool is_resource = (p.type == ParamType::Texture || p.type == ParamType::Sampler);
        if (!is_resource || p.has_default)
        {
            continue;
        }
        bool bound = false;
        for (u32 j = 0; j < m_overrides.size(); ++j)
        {
            if (m_overrides[j].name_hash == p.name_hash && m_overrides[j].provides_value)
            {
                bound = true;
                break;
            }
        }
        if (!bound)
        {
            diags.error(DiagCode::MissingResource, "material instance leaves a required texture/sampler unbound");
            return false;
        }
    }
    return true;
}

// ── RuntimeTechnique ──
bool RuntimeTechnique::add_surface_input(RenderChannel channel, DiagnosticList& diags)
{
    if (is_lighting_channel(channel))
    {
        diags.error(DiagCode::IncompatibleSurface,
                    "a technique consumes SURFACE channels from the material, not lighting channels");
        return false;
    }
    m_surface_inputs.push_back(channel);
    return true;
}

bool RuntimeTechnique::supports_phase(RenderPhaseId phase) const noexcept
{
    for (u32 i = 0; i < m_supported_phases.size(); ++i)
    {
        if (m_supported_phases[i] == phase)
        {
            return true;
        }
    }
    return false;
}

bool RuntimeTechnique::validate(DiagnosticList& diags) const
{
    for (u32 i = 0; i < m_surface_inputs.size(); ++i)
    {
        if (is_lighting_channel(m_surface_inputs[i]))
        {
            diags.error(DiagCode::IncompatibleSurface, "technique surface-input contract contains a lighting channel");
            return false;
        }
    }
    return true;
}

// ── Compatibility ──
bool validate_surface_compat(const RuntimeTechnique& technique, const RuntimeMaterialDefinition& material,
                             DiagnosticList& diags)
{
    for (u32 i = 0; i < technique.surface_inputs().size(); ++i)
    {
        const RenderChannel need = technique.surface_inputs()[i];
        bool produced = false;
        for (u32 j = 0; j < material.surface_outputs().size(); ++j)
        {
            if (material.surface_outputs()[j] == need)
            {
                produced = true;
                break;
            }
        }
        if (!produced)
        {
            diags.error(DiagCode::IncompatibleSurface,
                        "technique consumes a surface channel the material does not produce");
            return false;
        }
    }
    return true;
}

bool validate_phase(const RuntimeTechnique& technique, RenderPhaseId phase, DiagnosticList& diags)
{
    if (!technique.supports_phase(phase))
    {
        diags.error(DiagCode::UnsupportedPhase, "technique does not support this render phase");
        return false;
    }
    return true;
}

// ── Variant resolution ──
VariantKey resolve_variant(const RuntimeMaterialDefinition& def, const RuntimeTechnique& technique,
                           RenderPhaseId phase, u32 capability_tier) noexcept
{
    // The material FEATURE key is a structural hash of the definition (params + produced channels) — instances share
    // it, so two instances of one definition resolve to one variant.
    u64 feature = 0;
    for (u32 i = 0; i < def.params().size(); ++i)
    {
        const MaterialParam& p = def.params()[i];
        feature ^= hash_u64(p.name_hash ^ (static_cast<u64>(p.type) << 40U) ^ (p.has_default ? 1U : 0U));
    }
    for (u32 i = 0; i < def.surface_outputs().size(); ++i)
    {
        feature ^= hash_u64(static_cast<u64>(def.surface_outputs()[i]) + 0x55U);
    }

    VariantKey key;
    key.technique = technique.id().value;
    key.material_definition = def.id().value;
    key.material_feature = feature;
    key.render_phase = phase;
    key.capability_tier = capability_tier;
    return key;
}

// ── VariantCache ──
VariantCache::Lookup VariantCache::get_or_create(const VariantKey& key)
{
    const u64 h = key.hash();
    // lower_bound by key hash
    usize lo = 0;
    usize hi = m_entries.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (m_entries[mid].key.hash() < h)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    // Exact-match scan across the equal-hash run (collision-robust).
    for (usize i = lo; i < m_entries.size() && m_entries[i].key.hash() == h; ++i)
    {
        if (m_entries[i].key == key)
        {
            return Lookup{m_entries[i].handle, false};
        }
    }
    // Miss: create a fresh handle and insert keeping the array sorted by hash.
    const u32 handle = static_cast<u32>(m_entries.size());
    m_entries.push_back(Entry{key, handle});
    for (usize j = m_entries.size() - 1; j > lo; --j)
    {
        std::swap(m_entries[j], m_entries[j - 1]);
    }
    return Lookup{handle, true};
}
} // namespace crd::rendermaterial
