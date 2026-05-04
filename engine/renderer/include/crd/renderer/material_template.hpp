#pragma once

// Material system v1c — full material foundation (ADR-0048).
//
// Two-tier split:
//   MaterialTemplate  — immutable, loaded from a MATR artifact. Holds the cooked
//                       parameter schema, default values, pass-keyed shader handles,
//                       PSO state, and shader option declarations.
//   MaterialInstance  — caller-owned mutable overrides atop a MaterialTemplate.
//                       set_float / set_vec4 write into values_blob at the UBO
//                       offset recorded in the CookedParameter. variant_for_pass
//                       evaluates ShaderOptions and returns the correct shader pair.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderer/material_domain.hpp>
#include <crd/renderer/pass_type.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/shader/shader_resource_loader.hpp>

#include <cstring>

namespace crd::renderer
{

// ── ParameterType ──────────────────────────────────────────────────────────

// Frozen values — stored on disk in PRMS chunk (ADR-0048).
enum class ParameterType : crd::u8
{
    Float      = 0,
    Float2     = 1,
    Float3     = 2,
    Float4     = 3,
    Color      = 4,
    Bool       = 5,
    Int        = 6,
    Enum       = 7,
    Texture2D  = 8,
    TextureCube = 9,
    Sampler    = 10,
};

// ── CookedParameter ────────────────────────────────────────────────────────

// One entry in the PRMS chunk (24 bytes on disk).
// Emitted by the cooker after spirv-reflect extracts UBO offsets.
// Sorted by name_hash for O(log N) binary search at bind time.
struct CookedParameter
{
    crd::u64      name_hash           = 0; //  0..7
    crd::u64      enables_option_hash = 0; //  8..15  (0 = no inline functor)
    crd::u16      ubo_offset          = 0; // 16..17  byte offset into the UBO
    ParameterType type                = ParameterType::Float; // 18
    crd::u8       binding_slot        = 0; // 19  (for texture/sampler params)
    crd::u8       pad[4]              = {}; // 20..23
};
static_assert(sizeof(CookedParameter) == 24, "CookedParameter must be 24 bytes");

// ── ShaderOptionDecl ───────────────────────────────────────────────────────

// One entry in the OPTS chunk (16 bytes on disk).
struct ShaderOptionDecl
{
    crd::u64 name_hash      = 0; // 0..7
    crd::u8  default_enabled = 0; // 8
    crd::u8  pad[7]         = {}; // 9..15
};
static_assert(sizeof(ShaderOptionDecl) == 16, "ShaderOptionDecl must be 16 bytes");

// ── RasterState size assertion ─────────────────────────────────────────────

static_assert(sizeof(RasterState) == 8, "RasterState must be 8 bytes");

// ── PassShaderPair ─────────────────────────────────────────────────────────

// Vertex + fragment shader handles for one rendering pass.
struct PassShaderPair
{
    crd::resources::ResourceHandle<crd::shader::ShaderResource> vert;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> frag;
};

// ── MaterialTemplate ───────────────────────────────────────────────────────

// Runtime payload for a cooked MATR artifact. Owns all cooked data for one
// material family. Loaded by MaterialResourceLoader, managed by ResourceManager.
struct MaterialTemplate
{
    MaterialDomain domain = MaterialDomain::Surface;
    crd::u8        pad[3] = {};

    // Parameter schema sorted by name_hash (from PRMS chunk).
    crd::containers::Array<CookedParameter>  parameters;

    // Default values blob (raw bytes; indexed by CookedParameter::ubo_offset).
    crd::containers::Array<crd::u8>          defaults_blob;

    // Per-pass vert+frag shader pair (indexed by PassType ordinal).
    // Populated from the PASS chunk; legacy META chunk is synthesized into Forward.
    PassShaderPair pass_shaders[kPassTypeCount];

    // Per-pass PSO raster state (indexed by PassType ordinal, from PSOS chunk).
    RasterState pso_states[kPassTypeCount] = {};

    // Shader option declarations sorted by name_hash (from OPTS chunk).
    crd::containers::Array<ShaderOptionDecl> options;

    explicit MaterialTemplate(crd::memory::IAllocator* alloc)
        : parameters(alloc), defaults_blob(alloc), options(alloc)
    {
    }

    MaterialTemplate(const MaterialTemplate&)            = delete;
    MaterialTemplate& operator=(const MaterialTemplate&) = delete;
    MaterialTemplate(MaterialTemplate&&)                 = delete;
    MaterialTemplate& operator=(MaterialTemplate&&)      = delete;
};

// ── MaterialInstance ───────────────────────────────────────────────────────

// Caller-owned mutable parameter overrides atop a MaterialTemplate.
// Not managed by ResourceManager. One per visible material per frame is typical.
// Holds a strong reference to its template so the template cannot be evicted
// while the instance is alive.
class MaterialInstance
{
public:
    explicit MaterialInstance(crd::resources::ResourceHandle<MaterialTemplate> tmpl,
                              crd::memory::IAllocator* alloc)
        : m_tmpl(std::move(tmpl)), m_values_blob(alloc)
    {
        // Initialize values blob from template defaults.
        const MaterialTemplate* t = m_tmpl.get();
        if (t != nullptr && !t->defaults_blob.empty())
        {
            m_values_blob.resize(t->defaults_blob.size());
            std::memcpy(m_values_blob.data(), t->defaults_blob.data(),
                        t->defaults_blob.size());
        }
    }

    MaterialInstance(const MaterialInstance&)            = delete;
    MaterialInstance& operator=(const MaterialInstance&) = delete;
    MaterialInstance(MaterialInstance&&)                 = default;
    MaterialInstance& operator=(MaterialInstance&&)      = default;

    // Write a scalar float override.
    void set_float(crd::u64 name_hash, float v)
    {
        const auto* p = find_parameter(name_hash);
        if (p == nullptr || p->type != ParameterType::Float)
        {
            return;
        }
        ensure_blob_size(static_cast<crd::usize>(p->ubo_offset) + sizeof(float));
        std::memcpy(m_values_blob.data() + p->ubo_offset, &v, sizeof(float));
    }

    // Write a vec4 override.
    void set_vec4(crd::u64 name_hash, float x, float y, float z, float w)
    {
        const auto* p = find_parameter(name_hash);
        if (p == nullptr || p->type != ParameterType::Float4)
        {
            return;
        }
        ensure_blob_size(static_cast<crd::usize>(p->ubo_offset) + 4U * sizeof(float));
        float v[4] = {x, y, z, w};
        std::memcpy(m_values_blob.data() + p->ubo_offset, v, sizeof(v));
    }

    // Return the vert+frag pair for `pass`.
    // Falls back to the Forward pair if `pass` has no shader.
    [[nodiscard]] const PassShaderPair& variant_for_pass(PassType pass) const noexcept
    {
        const MaterialTemplate* t = m_tmpl.get();
        if (t == nullptr)
        {
            static const PassShaderPair kEmpty{};
            return kEmpty;
        }

        const auto idx = static_cast<crd::u8>(pass);
        if (idx < kPassTypeCount && t->pass_shaders[idx].vert.is_ready())
        {
            return t->pass_shaders[idx];
        }

        return t->pass_shaders[static_cast<crd::u8>(PassType::Forward)];
    }

    [[nodiscard]] const crd::containers::Array<crd::u8>& values_blob() const noexcept
    {
        return m_values_blob;
    }

    [[nodiscard]] const crd::resources::ResourceHandle<MaterialTemplate>& tmpl() const noexcept
    {
        return m_tmpl;
    }

private:
    crd::resources::ResourceHandle<MaterialTemplate> m_tmpl;
    crd::containers::Array<crd::u8>                 m_values_blob;

    [[nodiscard]] const CookedParameter* find_parameter(crd::u64 name_hash) const noexcept
    {
        const MaterialTemplate* t = m_tmpl.get();
        if (t == nullptr || t->parameters.empty())
        {
            return nullptr;
        }

        // Binary search — parameters are sorted by name_hash.
        const CookedParameter* data  = t->parameters.data();
        crd::usize              lo   = 0;
        crd::usize              hi   = t->parameters.size();
        while (lo < hi)
        {
            const crd::usize mid = lo + (hi - lo) / 2U;
            if (data[mid].name_hash < name_hash)
            {
                lo = mid + 1U;
            }
            else if (data[mid].name_hash > name_hash)
            {
                hi = mid;
            }
            else
            {
                return &data[mid];
            }
        }
        return nullptr;
    }

    void ensure_blob_size(crd::usize required)
    {
        if (m_values_blob.size() < required)
        {
            const crd::usize old = m_values_blob.size();
            m_values_blob.resize(required);
            // Zero-initialise the newly extended region.
            std::memset(m_values_blob.data() + old, 0, required - old);
        }
    }
};

} // namespace crd::renderer
