#include <crd/renderprogram/program_contract.hpp>

#include <crd/containers/hash.hpp>
#include <crd/containers/sort.hpp>

namespace crd::renderprogram
{
using crd::containers::hash_u64;

StringView program_stage_name(ProgramStage stage) noexcept
{
    switch (stage)
    {
    case ProgramStage::Vertex:
        return "vertex";
    case ProgramStage::TessControl:
        return "tess-control";
    case ProgramStage::TessEval:
        return "tess-eval";
    case ProgramStage::Geometry:
        return "geometry";
    case ProgramStage::Fragment:
        return "fragment";
    case ProgramStage::Task:
        return "task";
    case ProgramStage::Mesh:
        return "mesh";
    case ProgramStage::Compute:
        return "compute";
    case ProgramStage::RayGen:
        return "raygen";
    case ProgramStage::Miss:
        return "miss";
    case ProgramStage::ClosestHit:
        return "closest-hit";
    case ProgramStage::AnyHit:
        return "any-hit";
    case ProgramStage::Intersection:
        return "intersection";
    case ProgramStage::Callable:
        return "callable";
    }
    return "unknown";
}

u64 VariantKey::hash() const noexcept
{
    u64 h = hash_u64(technique + 1U);
    h = hash_u64(h ^ material_definition);
    h = hash_u64(h ^ material_feature);
    h = hash_u64(h ^ (static_cast<u64>(render_phase) << 1U));
    h = hash_u64(h ^ (static_cast<u64>(vertex_variant) << 2U));
    h = hash_u64(h ^ (static_cast<u64>(skinning_variant) << 3U));
    h = hash_u64(h ^ (static_cast<u64>(attachment_signature) << 4U));
    h = hash_u64(h ^ (static_cast<u64>(capability_tier) << 5U));
    h = hash_u64(h ^ technique_options);
    return h;
}

const ShaderModuleContract* ProgramContract::find_stage(ProgramStage stage) const noexcept
{
    for (u32 i = 0; i < m_modules.size(); ++i)
    {
        if (m_modules[i].stage == stage)
        {
            return &m_modules[i];
        }
    }
    return nullptr;
}

bool ProgramContract::add_module(const ShaderModuleContract& module, DiagnosticList& diags)
{
    if (find_stage(module.stage) != nullptr)
    {
        diags.error(DiagCode::DuplicateStage, "program already declares this shader stage", {},
                    program_stage_name(module.stage));
        return false;
    }
    m_modules.push_back(module);
    return true;
}

namespace
{
constexpr u32 stage_bit(ProgramStage s) noexcept { return 1U << static_cast<u32>(s); }
} // namespace

bool ProgramContract::validate(DiagnosticList& diags) const
{
    if (m_modules.size() == 0)
    {
        diags.error(DiagCode::IllegalStageComposition, "program has no shader stages");
        return false;
    }

    u32 mask = 0;
    for (u32 i = 0; i < m_modules.size(); ++i)
    {
        mask |= stage_bit(m_modules[i].stage);
    }
    const auto has = [&](ProgramStage s) { return (mask & stage_bit(s)) != 0U; };

    const u32 rt_mask = stage_bit(ProgramStage::RayGen) | stage_bit(ProgramStage::Miss) |
                        stage_bit(ProgramStage::ClosestHit) | stage_bit(ProgramStage::AnyHit) |
                        stage_bit(ProgramStage::Intersection) | stage_bit(ProgramStage::Callable);
    const bool any_rt = (mask & rt_mask) != 0U;
    const bool any_raster = (mask & (stage_bit(ProgramStage::Vertex) | stage_bit(ProgramStage::TessControl) |
                                     stage_bit(ProgramStage::TessEval) | stage_bit(ProgramStage::Geometry) |
                                     stage_bit(ProgramStage::Fragment) | stage_bit(ProgramStage::Task) |
                                     stage_bit(ProgramStage::Mesh))) != 0U;

    if (has(ProgramStage::Compute) && mask != stage_bit(ProgramStage::Compute))
    {
        diags.error(DiagCode::IllegalStageComposition, "a compute stage must stand alone");
        return false;
    }
    if (any_rt)
    {
        if (any_raster || has(ProgramStage::Compute))
        {
            diags.error(DiagCode::IllegalStageComposition, "ray-tracing stages cannot mix with raster/compute");
            return false;
        }
        if (!has(ProgramStage::RayGen))
        {
            diags.error(DiagCode::IllegalStageComposition, "a ray-tracing program needs a raygen stage");
            return false;
        }
        return true; // RT set is legal; no vertex I/O to match
    }
    if (has(ProgramStage::Vertex) && has(ProgramStage::Mesh))
    {
        diags.error(DiagCode::IllegalStageComposition, "vertex and mesh geometry paths are mutually exclusive");
        return false;
    }
    if (has(ProgramStage::TessControl) != has(ProgramStage::TessEval))
    {
        diags.error(DiagCode::IllegalStageComposition, "tessellation control and eval stages must both be present");
        return false;
    }
    if ((has(ProgramStage::TessControl) || has(ProgramStage::Geometry)) && !has(ProgramStage::Vertex))
    {
        diags.error(DiagCode::IllegalStageComposition, "tessellation/geometry stages require a vertex stage");
        return false;
    }
    if (has(ProgramStage::Task) && !has(ProgramStage::Mesh))
    {
        diags.error(DiagCode::IllegalStageComposition, "a task stage requires a mesh stage");
        return false;
    }
    if (has(ProgramStage::Fragment) && !has(ProgramStage::Vertex) && !has(ProgramStage::Mesh))
    {
        diags.error(DiagCode::IllegalStageComposition, "a fragment stage requires a vertex or mesh producer");
        return false;
    }
    if (!has(ProgramStage::Vertex) && !has(ProgramStage::Mesh))
    {
        diags.error(DiagCode::IllegalStageComposition, "a raster program needs a vertex or mesh stage");
        return false;
    }

    // Stage-I/O compatibility: every fragment input must be produced by the immediately-preceding stage.
    if (has(ProgramStage::Fragment))
    {
        const ShaderModuleContract* producer = find_stage(ProgramStage::Geometry);
        if (producer == nullptr)
        {
            producer = find_stage(ProgramStage::TessEval);
        }
        if (producer == nullptr)
        {
            producer = find_stage(ProgramStage::Mesh);
        }
        if (producer == nullptr)
        {
            producer = find_stage(ProgramStage::Vertex);
        }
        const ShaderModuleContract* fs = find_stage(ProgramStage::Fragment);
        if (producer != nullptr && fs != nullptr)
        {
            for (u32 i = 0; i < fs->inputs.size(); ++i)
            {
                const StageIoVar& in = fs->inputs[i];
                bool matched = false;
                for (u32 j = 0; j < producer->outputs.size(); ++j)
                {
                    if (producer->outputs[j] == in)
                    {
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    diags.error(DiagCode::StageIoMismatch, "fragment input has no matching producer output");
                    return false;
                }
            }
        }
    }
    return true;
}

bool ProgramContract::resolve_layout(Array<ResolvedBinding>& out, DiagnosticList& diags) const
{
    out.clear();
    // Collect unique resources by name; conflicting decls of the same name are rejected; stage masks merge.
    for (u32 m = 0; m < m_modules.size(); ++m)
    {
        const ShaderModuleContract& mod = m_modules[m];
        for (u32 r = 0; r < mod.resources.size(); ++r)
        {
            const ResourceDecl& d = mod.resources[r];
            ResolvedBinding* existing = nullptr;
            for (u32 k = 0; k < out.size(); ++k)
            {
                if (out[k].name_hash == d.name_hash)
                {
                    existing = &out[k];
                    break;
                }
            }
            if (existing != nullptr)
            {
                if (existing->kind != d.kind || existing->frequency != d.frequency ||
                    existing->array_count != d.array_count)
                {
                    diags.error(DiagCode::BindingConflict, "binding name declared with conflicting kind/frequency");
                    return false;
                }
                existing->stage_mask |= stage_bit(mod.stage);
            }
            else
            {
                out.push_back(ResolvedBinding{d.frequency, 0U, d.kind, d.name_hash, d.array_count, stage_bit(mod.stage)});
            }
        }
    }

    // Deterministic order: sort by (frequency, name_hash), then assign compact slots within each frequency group.
    crd::containers::sort(out.begin(), out.end(),
                          [](const ResolvedBinding& a, const ResolvedBinding& b)
                          {
                              if (a.frequency != b.frequency)
                              {
                                  return static_cast<u8>(a.frequency) < static_cast<u8>(b.frequency);
                              }
                              return a.name_hash < b.name_hash;
                          });
    u32 slot = 0;
    for (u32 i = 0; i < out.size(); ++i)
    {
        if (i > 0 && out[i].frequency != out[i - 1].frequency)
        {
            slot = 0;
        }
        out[i].slot = slot++;
    }
    return true;
}

bool ProgramContract::validate_attachment_compat(const StageIoVar* rt_outputs, u32 rt_count, DiagnosticList& diags) const
{
    const ShaderModuleContract* fs = find_stage(ProgramStage::Fragment);
    const u32 fs_count = (fs != nullptr) ? static_cast<u32>(fs->outputs.size()) : 0U;
    if (fs_count != rt_count)
    {
        diags.emit(Severity::Error, DiagCode::AttachmentMismatch, "fragment output count differs from the render target");
        return false;
    }
    for (u32 i = 0; i < rt_count; ++i)
    {
        bool matched = false;
        for (u32 j = 0; j < fs_count; ++j)
        {
            if (fs->outputs[j].location == rt_outputs[i].location)
            {
                if (fs->outputs[j].components != rt_outputs[i].components)
                {
                    diags.error(DiagCode::AttachmentMismatch, "fragment output components differ from the attachment");
                    return false;
                }
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            diags.error(DiagCode::AttachmentMismatch, "render-target attachment has no matching fragment output");
            return false;
        }
    }
    return true;
}

InterfaceHash ProgramContract::interface_hash() const noexcept
{
    u64 program_h = 0;
    for (u32 m = 0; m < m_modules.size(); ++m)
    {
        const ShaderModuleContract& mod = m_modules[m];
        u64 mh = hash_u64(static_cast<u64>(mod.stage) + 1U);
        for (u32 i = 0; i < mod.inputs.size(); ++i)
        {
            const StageIoVar& v = mod.inputs[i];
            mh ^= hash_u64((static_cast<u64>(v.location) << 20U) ^ (static_cast<u64>(v.scalar) << 8U) ^ v.components ^
                           0x11U);
        }
        for (u32 i = 0; i < mod.outputs.size(); ++i)
        {
            const StageIoVar& v = mod.outputs[i];
            mh ^= hash_u64((static_cast<u64>(v.location) << 20U) ^ (static_cast<u64>(v.scalar) << 8U) ^ v.components ^
                           0x22U);
        }
        for (u32 i = 0; i < mod.resources.size(); ++i)
        {
            const ResourceDecl& d = mod.resources[i];
            mh ^= hash_u64(d.name_hash ^ (static_cast<u64>(d.kind) << 40U) ^ (static_cast<u64>(d.frequency) << 44U) ^
                           (static_cast<u64>(d.array_count) << 48U) ^ 0x33U);
        }
        program_h ^= hash_u64(mh ^ (static_cast<u64>(mod.stage) << 56U));
    }
    return InterfaceHash{program_h};
}

u64 variant_space_size(const VariantAxis* axes, u32 axis_count) noexcept
{
    u64 total = 1;
    for (u32 i = 0; i < axis_count; ++i)
    {
        const u64 n = static_cast<u64>(axes[i].options.size());
        total *= (n == 0 ? 1U : n); // an empty axis contributes no variation
    }
    return total;
}
} // namespace crd::renderprogram
