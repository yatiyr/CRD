// RAF-5 Gate 5 — the material + technique contract (device-free).
//
// Gates (mission §5 · D-007 RAF-5): many instances share one definition; invalid overrides rejected; missing
// texture/resource diagnosed; material CANNOT access lighting state; technique CANNOT consume an incompatible surface;
// technique surface bindings verified; phase incompatibility rejected; program variants cached + deterministic.
//
// ⛔ named allocator throughout; ASCII-only test names.

#include <crd/rendermaterial/material_contract.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u32;
using crd::u64;
using namespace crd::rendermaterial;

namespace
{
constexpr u64 kBaseColor = 1U;
constexpr u64 kRoughness = 2U;

// A definition: a required (undefaulted) base-colour TEXTURE, a defaulted roughness scalar; produces BaseColor +
// Roughness surface channels.
void fill_def(RuntimeMaterialDefinition& def, DiagnosticList& d)
{
    def.add_param(MaterialParam{kBaseColor, ParamType::Texture, false}); // no default ⇒ must be bound
    def.add_param(MaterialParam{kRoughness, ParamType::Scalar, true});
    REQUIRE(def.add_surface_output(RenderChannel::BaseColor, d));
    REQUIRE(def.add_surface_output(RenderChannel::Roughness, d));
}
} // namespace

TEST_CASE("raf5 many instances share one definition")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-share");
    DiagnosticList d(&alloc);
    RuntimeMaterialDefinition def(crd::renderasset::AssetId{100U}, &alloc);
    fill_def(def, d);
    REQUIRE(def.validate(d));

    // Three instances of the ONE definition, each binding the required texture.
    for (u32 i = 0; i < 3U; ++i)
    {
        RuntimeMaterialInstance inst(crd::renderasset::AssetId{200U + i}, def.id(), &alloc);
        inst.add_override(ParamOverride{kBaseColor, ParamType::Texture, true}); // bind the required texture
        if (i != 0U)
        {
            inst.add_override(ParamOverride{kRoughness, ParamType::Scalar, true}); // some also tweak roughness
        }
        REQUIRE(inst.validate(def, d));
        REQUIRE(inst.definition_id() == def.id());
    }
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("raf5 invalid overrides are rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-override");
    DiagnosticList setup(&alloc);
    RuntimeMaterialDefinition def(crd::renderasset::AssetId{100U}, &alloc);
    fill_def(def, setup);

    SECTION("unknown param")
    {
        DiagnosticList d(&alloc);
        RuntimeMaterialInstance inst(crd::renderasset::AssetId{201U}, def.id(), &alloc);
        inst.add_override(ParamOverride{kBaseColor, ParamType::Texture, true});
        inst.add_override(ParamOverride{999U, ParamType::Scalar, true}); // no such param
        REQUIRE_FALSE(inst.validate(def, d));
        REQUIRE(d.contains(DiagCode::InvalidOverride));
    }
    SECTION("type mismatch")
    {
        DiagnosticList d(&alloc);
        RuntimeMaterialInstance inst(crd::renderasset::AssetId{202U}, def.id(), &alloc);
        inst.add_override(ParamOverride{kBaseColor, ParamType::Texture, true});
        inst.add_override(ParamOverride{kRoughness, ParamType::Vec4, true}); // roughness is Scalar, not Vec4
        REQUIRE_FALSE(inst.validate(def, d));
        REQUIRE(d.contains(DiagCode::InvalidOverride));
    }
}

TEST_CASE("raf5 missing required texture is diagnosed")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-missing");
    DiagnosticList setup(&alloc);
    RuntimeMaterialDefinition def(crd::renderasset::AssetId{100U}, &alloc);
    fill_def(def, setup);

    DiagnosticList d(&alloc);
    RuntimeMaterialInstance inst(crd::renderasset::AssetId{203U}, def.id(), &alloc);
    // Only tweak roughness; the required base-colour texture is left unbound.
    inst.add_override(ParamOverride{kRoughness, ParamType::Scalar, true});
    REQUIRE_FALSE(inst.validate(def, d));
    REQUIRE(d.contains(DiagCode::MissingResource));

    // Binding it makes the instance valid.
    DiagnosticList d2(&alloc);
    inst.add_override(ParamOverride{kBaseColor, ParamType::Texture, true});
    REQUIRE(inst.validate(def, d2));
}

TEST_CASE("raf5 material cannot access lighting state")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-lighting");
    DiagnosticList d(&alloc);
    RuntimeMaterialDefinition def(crd::renderasset::AssetId{100U}, &alloc);
    REQUIRE(def.add_surface_output(RenderChannel::BaseColor, d)); // a surface channel is fine
    REQUIRE_FALSE(def.add_surface_output(RenderChannel::Irradiance, d)); // a lighting channel is rejected
    REQUIRE(d.contains(DiagCode::MaterialLightingAccess));
}

TEST_CASE("raf5 technique cannot consume an incompatible surface")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-surface");
    DiagnosticList d(&alloc);

    RuntimeMaterialDefinition def(crd::renderasset::AssetId{100U}, &alloc);
    REQUIRE(def.add_surface_output(RenderChannel::BaseColor, d)); // produces only BaseColor

    RuntimeTechnique tech(crd::renderasset::AssetId{300U}, &alloc);
    REQUIRE(tech.add_surface_input(RenderChannel::BaseColor, d));
    REQUIRE(tech.add_surface_input(RenderChannel::Normal, d)); // needs Normal too

    DiagnosticList d2(&alloc);
    REQUIRE_FALSE(validate_surface_compat(tech, def, d2)); // material produces no Normal
    REQUIRE(d2.contains(DiagCode::IncompatibleSurface));

    // Add Normal to the material ⇒ compatible.
    DiagnosticList d3(&alloc);
    REQUIRE(def.add_surface_output(RenderChannel::Normal, d3));
    REQUIRE(validate_surface_compat(tech, def, d3));

    // A technique input contract also rejects a lighting channel directly.
    DiagnosticList d4(&alloc);
    REQUIRE_FALSE(tech.add_surface_input(RenderChannel::ShadowFactor, d4));
    REQUIRE(d4.contains(DiagCode::IncompatibleSurface));
}

TEST_CASE("raf5 phase incompatibility is rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-phase");
    DiagnosticList d(&alloc);
    RuntimeTechnique tech(crd::renderasset::AssetId{300U}, &alloc);
    tech.add_supported_phase(0U);
    tech.add_supported_phase(2U);

    REQUIRE(validate_phase(tech, 0U, d));
    REQUIRE(validate_phase(tech, 2U, d));
    REQUIRE_FALSE(d.has_errors());

    DiagnosticList d2(&alloc);
    REQUIRE_FALSE(validate_phase(tech, 1U, d2)); // phase 1 unsupported
    REQUIRE(d2.contains(DiagCode::UnsupportedPhase));
}

TEST_CASE("raf5 program variants are cached and deterministic")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf5-variant");
    DiagnosticList d(&alloc);
    RuntimeMaterialDefinition def(crd::renderasset::AssetId{100U}, &alloc);
    fill_def(def, d);
    RuntimeTechnique tech(crd::renderasset::AssetId{300U}, &alloc);
    tech.add_supported_phase(0U);

    // Two instances of the same definition resolve to the SAME variant (they share it).
    const VariantKey v_inst_a = resolve_variant(def, tech, /*phase*/ 0U, /*caps*/ 1U);
    const VariantKey v_inst_b = resolve_variant(def, tech, 0U, 1U);
    REQUIRE(v_inst_a == v_inst_b);

    VariantCache cache(&alloc);
    const VariantCache::Lookup la = cache.get_or_create(v_inst_a);
    REQUIRE(la.created); // first resolution creates
    const VariantCache::Lookup lb = cache.get_or_create(v_inst_b);
    REQUIRE_FALSE(lb.created);         // the second instance HITS the cached variant
    REQUIRE(lb.handle == la.handle);   // same variant handle
    REQUIRE(cache.size() == 1U);

    // A different phase (or capability tier) is a distinct variant.
    const VariantKey v_other_phase = resolve_variant(def, tech, 1U, 1U);
    REQUIRE(v_other_phase != v_inst_a);
    const VariantCache::Lookup lc = cache.get_or_create(v_other_phase);
    REQUIRE(lc.created);
    REQUIRE(lc.handle != la.handle);
    REQUIRE(cache.size() == 2U);

    // Deterministic: re-resolving anything already cached hits its original handle.
    REQUIRE_FALSE(cache.get_or_create(v_inst_a).created);
    REQUIRE(cache.get_or_create(v_inst_a).handle == la.handle);
}
