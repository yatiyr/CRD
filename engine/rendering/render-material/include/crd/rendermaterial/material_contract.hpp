#pragma once

// crd-render-material — the MATERIAL + TECHNIQUE runtime contract (RAF-5, mission §5 · ADR-0102 / REN-37).
//
// The separation, as a validated type system:
//   • MATERIAL = surface response. A `RuntimeMaterialDefinition` declares surface PARAMETERS + the surface CHANNELS it
//     produces (base colour, normal, roughness…). It structurally CANNOT touch lighting: `RenderChannel` splits into
//     surface vs lighting channels and a material's outputs are rejected if any is a lighting channel.
//   • INSTANCE = a lightweight override of a definition. Many `RuntimeMaterialInstance`s share ONE definition; an
//     override must target an existing param with a matching type, and a required texture must be defaulted or bound.
//   • TECHNIQUE = shading algorithm. A `RuntimeTechnique` declares which surface channels it CONSUMES and which render
//     PHASES it supports. It cannot consume a surface channel the material does not produce; it cannot run in an
//     unsupported phase. It (and only it) reaches lighting state — that is not modelled as a material capability.
//   • The technique produces a program VARIANT keyed on the RAF-4 `VariantKey`; a `VariantCache` makes resolution
//     deterministic (same material+technique+phase+caps ⇒ same cached variant).
//
// Backend-/IR-agnostic; validated and gated with no device. Built on render-asset-core + render-program.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp>
#include <crd/renderprogram/program_contract.hpp>

namespace crd::rendermaterial
{
using crd::containers::Array;
using crd::renderasset::AssetId;
using crd::renderasset::DiagCode;
using crd::renderasset::DiagnosticList;
using crd::renderasset::Severity;
using crd::renderprogram::VariantKey;

// A render channel. Surface channels (< kLightingBase) are the material's vocabulary; lighting channels are the
// TECHNIQUE's — a material may never declare one (the surface/algorithm separation, enforced structurally).
inline constexpr u16 kLightingBase = 0x100;
enum class RenderChannel : u16
{
    // ── surface response (a MATERIAL produces these) ──
    BaseColor = 0,
    Normal = 1,
    Roughness = 2,
    Metalness = 3,
    Specular = 4,
    Emission = 5,
    Opacity = 6,
    Occlusion = 7,
    ClearCoat = 8,
    ClearCoatRoughness = 9,
    Sheen = 10,
    Anisotropy = 11,
    SubsurfaceColor = 12,
    Thickness = 13,
    // ── lighting (only a TECHNIQUE touches these) ──
    Irradiance = kLightingBase,
    Radiance = kLightingBase + 1,
    ShadowFactor = kLightingBase + 2,
    DirectLighting = kLightingBase + 3,
    IndirectLighting = kLightingBase + 4,
    AmbientOcclusionApplied = kLightingBase + 5,
    FinalColor = kLightingBase + 6,
};
[[nodiscard]] constexpr bool is_surface_channel(RenderChannel c) noexcept
{
    return static_cast<u16>(c) < kLightingBase;
}
[[nodiscard]] constexpr bool is_lighting_channel(RenderChannel c) noexcept { return !is_surface_channel(c); }

enum class ParamType : u8
{
    Scalar = 0,
    Vec2,
    Vec3,
    Vec4,
    Texture,
    Sampler,
};

// One material parameter: a stable name + type. Texture params without a default MUST be bound by an instance.
struct MaterialParam
{
    u64 name_hash = 0;
    ParamType type = ParamType::Scalar;
    bool has_default = true;
    friend constexpr bool operator==(const MaterialParam&, const MaterialParam&) noexcept = default;
};

using RenderPhaseId = u32;

// A material DEFINITION — the shared surface response. Immutable; instances reference it by id.
class RuntimeMaterialDefinition
{
public:
    RuntimeMaterialDefinition(AssetId id, memory::IAllocator* alloc) noexcept
        : m_id(id), m_params(alloc), m_surface_outputs(alloc)
    {
    }

    void add_param(const MaterialParam& p) { m_params.push_back(p); }
    // Declare a surface channel this material produces. Rejects a LIGHTING channel (MaterialLightingAccess).
    bool add_surface_output(RenderChannel channel, DiagnosticList& diags);

    [[nodiscard]] AssetId id() const noexcept { return m_id; }
    [[nodiscard]] const Array<MaterialParam>& params() const noexcept { return m_params; }
    [[nodiscard]] const Array<RenderChannel>& surface_outputs() const noexcept { return m_surface_outputs; }
    [[nodiscard]] const MaterialParam* find_param(u64 name_hash) const noexcept;

    // A material may only produce surface channels (checked as they are added, re-checkable here).
    [[nodiscard]] bool validate(DiagnosticList& diags) const;

private:
    AssetId m_id;
    Array<MaterialParam> m_params;
    Array<RenderChannel> m_surface_outputs;
};

// One instance override: which param it supplies a value/texture for.
struct ParamOverride
{
    u64 name_hash = 0;
    ParamType type = ParamType::Scalar;
    bool provides_value = true; // for a texture/sampler override, true ⇒ a real binding is supplied
};

// A material INSTANCE — a lightweight override set over a shared definition.
class RuntimeMaterialInstance
{
public:
    RuntimeMaterialInstance(AssetId id, AssetId definition_id, memory::IAllocator* alloc) noexcept
        : m_id(id), m_definition_id(definition_id), m_overrides(alloc)
    {
    }

    void add_override(const ParamOverride& o) { m_overrides.push_back(o); }

    [[nodiscard]] AssetId id() const noexcept { return m_id; }
    [[nodiscard]] AssetId definition_id() const noexcept { return m_definition_id; }
    [[nodiscard]] const Array<ParamOverride>& overrides() const noexcept { return m_overrides; }

    // Validate against the shared definition: every override targets an existing param of a matching type
    // (InvalidOverride); every undefaulted texture param is bound by an override (MissingResource).
    [[nodiscard]] bool validate(const RuntimeMaterialDefinition& def, DiagnosticList& diags) const;

private:
    AssetId m_id;
    AssetId m_definition_id;
    Array<ParamOverride> m_overrides;
};

// A TECHNIQUE — the shading algorithm. Declares the surface channels it consumes + the phases it supports.
class RuntimeTechnique
{
public:
    RuntimeTechnique(AssetId id, memory::IAllocator* alloc) noexcept
        : m_id(id), m_surface_inputs(alloc), m_supported_phases(alloc)
    {
    }

    // Declare a surface channel this technique reads from the material. Rejects a lighting channel (a technique reads
    // SURFACE from the material; it reaches lighting internally, not as a material input).
    bool add_surface_input(RenderChannel channel, DiagnosticList& diags);
    void add_supported_phase(RenderPhaseId phase) { m_supported_phases.push_back(phase); }

    [[nodiscard]] AssetId id() const noexcept { return m_id; }
    [[nodiscard]] const Array<RenderChannel>& surface_inputs() const noexcept { return m_surface_inputs; }
    [[nodiscard]] bool supports_phase(RenderPhaseId phase) const noexcept;

    // The technique consumes only surface channels (checked on add; re-checkable).
    [[nodiscard]] bool validate(DiagnosticList& diags) const;

private:
    AssetId m_id;
    Array<RenderChannel> m_surface_inputs;
    Array<RenderPhaseId> m_supported_phases;
};

// A technique may only consume surface channels the material actually PRODUCES (IncompatibleSurface).
[[nodiscard]] bool validate_surface_compat(const RuntimeTechnique& technique, const RuntimeMaterialDefinition& material,
                                           DiagnosticList& diags);
// A technique must support the render phase it is asked to run in (UnsupportedPhase).
[[nodiscard]] bool validate_phase(const RuntimeTechnique& technique, RenderPhaseId phase, DiagnosticList& diags);

// The deterministic program-variant key for (material def + technique + phase + capability tier). Note: NO instance —
// all instances of one definition SHARE its variant (they differ only in bound param values), so resolving the variant
// for two instances of the same definition yields the same key. That is the "many instances share one variant" gate.
[[nodiscard]] VariantKey resolve_variant(const RuntimeMaterialDefinition& def, const RuntimeTechnique& technique,
                                         RenderPhaseId phase, u32 capability_tier) noexcept;

// A deterministic variant cache: resolving the same VariantKey returns the same stable handle (a cache HIT); a new
// key gets a fresh handle. Program variants are created once and shared.
class VariantCache
{
public:
    explicit VariantCache(memory::IAllocator* alloc) noexcept : m_entries(alloc) {}

    struct Lookup
    {
        u32 handle = 0;
        bool created = false; // true ⇒ this call created the variant (a cache miss)
    };
    [[nodiscard]] Lookup get_or_create(const VariantKey& key);
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(m_entries.size()); }

private:
    struct Entry
    {
        VariantKey key; // the full key (collision-robust exact match, not just its hash)
        u32 handle;
    };
    Array<Entry> m_entries; // sorted ascending by key.hash()
};
} // namespace crd::rendermaterial
