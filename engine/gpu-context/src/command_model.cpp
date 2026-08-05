#include <crd/gpu/command_model.hpp>

namespace crd::gpu
{
StringView command_error_name(CommandError err) noexcept
{
    switch (err)
    {
    case CommandError::None:
        return "none";
    case CommandError::NoAttachments:
        return "no-attachments";
    case CommandError::TooManyColorAttachments:
        return "too-many-color-attachments";
    case CommandError::MismatchedAttachmentSize:
        return "mismatched-attachment-size";
    case CommandError::ZeroRenderArea:
        return "zero-render-area";
    case CommandError::NullProgram:
        return "null-program";
    case CommandError::DuplicateBinding:
        return "duplicate-binding";
    case CommandError::ComparisonSamplerWithoutTexture:
        return "comparison-sampler-without-texture";
    case CommandError::BindlessCountExceeded:
        return "bindless-count-exceeded";
    case CommandError::GeometryCommandMismatch:
        return "geometry-command-mismatch";
    case CommandError::MissingIndexBuffer:
        return "missing-index-buffer";
    case CommandError::MissingIndirectArgs:
        return "missing-indirect-args";
    case CommandError::MissingCountBuffer:
        return "missing-count-buffer";
    case CommandError::MissingAccelerationStructure:
        return "missing-acceleration-structure";
    case CommandError::ZeroDraw:
        return "zero-draw";
    }
    return "unknown";
}

namespace
{
// Bindings shared check: (frequency, slot) uniqueness, comparison-sampler pairing, bindless element count.
CommandError validate_bindings(const ResourceBindingTable& bindings) noexcept
{
    bool has_comparison_sampler = false;
    bool has_sampled_texture = false;
    for (crd::u32 i = 0; i < bindings.size(); ++i)
    {
        const ResourceBinding& bi = bindings[i];
        for (crd::u32 j = i + 1; j < bindings.size(); ++j)
        {
            const ResourceBinding& bj = bindings[j];
            if (bi.frequency == bj.frequency && bi.slot == bj.slot)
            {
                return CommandError::DuplicateBinding;
            }
        }
        if (bi.kind == BindingKind::ComparisonSampler)
        {
            has_comparison_sampler = true;
        }
        if (bi.kind == BindingKind::SampledTexture)
        {
            has_sampled_texture = true;
        }
        if (bi.kind == BindingKind::BindlessTextureArray &&
            (bi.array_count == 0 || bi.array_count > kMaxBindlessTextures))
        {
            return CommandError::BindlessCountExceeded;
        }
    }
    if (has_comparison_sampler && !has_sampled_texture)
    {
        return CommandError::ComparisonSamplerWithoutTexture;
    }
    return CommandError::None;
}
} // namespace

CommandError validate_rendering(const RenderingDesc& rendering) noexcept
{
    if (rendering.width == 0 || rendering.height == 0)
    {
        return CommandError::ZeroRenderArea;
    }
    if (rendering.color.size() > kMaxColorAttachments)
    {
        return CommandError::TooManyColorAttachments;
    }
    if (rendering.color.size() == 0 && !rendering.depth.enabled)
    {
        return CommandError::NoAttachments;
    }
    for (crd::u32 i = 0; i < rendering.color.size(); ++i)
    {
        const IRasterTarget* t = rendering.color[i].target;
        if (t != nullptr && (t->width() != rendering.width || t->height() != rendering.height))
        {
            return CommandError::MismatchedAttachmentSize;
        }
    }
    if (rendering.depth.enabled && rendering.depth.target != nullptr)
    {
        const IRasterTarget* d = rendering.depth.target;
        if (d->width() != rendering.width || d->height() != rendering.height)
        {
            return CommandError::MismatchedAttachmentSize;
        }
    }
    return CommandError::None;
}

CommandError validate_packet(const RasterDrawPacket& packet) noexcept
{
    if (packet.program == nullptr)
    {
        return CommandError::NullProgram;
    }

    const GeometrySource& g = packet.geometry;
    switch (packet.command)
    {
    case RasterCommandKind::Draw:
        if (g.kind != GeometryKind::None && g.kind != GeometryKind::StoragePull)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.vertex_or_index_count == 0 || g.instance_count == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    case RasterCommandKind::DrawIndexed:
        if (g.kind != GeometryKind::Indexed)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.index_buffer == nullptr)
        {
            return CommandError::MissingIndexBuffer;
        }
        if (g.vertex_or_index_count == 0 || g.instance_count == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    case RasterCommandKind::DrawIndirect:
        if (g.kind != GeometryKind::Indirect)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.args_buffer == nullptr)
        {
            return CommandError::MissingIndirectArgs;
        }
        if (g.max_draws == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    case RasterCommandKind::DrawIndexedIndirect:
        if (g.kind != GeometryKind::Indirect)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.args_buffer == nullptr)
        {
            return CommandError::MissingIndirectArgs;
        }
        if (g.index_buffer == nullptr)
        {
            return CommandError::MissingIndexBuffer;
        }
        break;
    case RasterCommandKind::DrawIndexedIndirectCount:
        if (g.kind != GeometryKind::IndirectCount)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.args_buffer == nullptr)
        {
            return CommandError::MissingIndirectArgs;
        }
        if (g.count_buffer == nullptr)
        {
            return CommandError::MissingCountBuffer;
        }
        if (g.index_buffer == nullptr)
        {
            return CommandError::MissingIndexBuffer;
        }
        break;
    case RasterCommandKind::DispatchMesh:
        if (g.kind != GeometryKind::Meshlet)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.group_count_x == 0 || g.group_count_y == 0 || g.group_count_z == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    case RasterCommandKind::DispatchMeshIndirect:
        if (g.kind != GeometryKind::MeshletIndirect)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.args_buffer == nullptr && g.native_args == nullptr)
        {
            return CommandError::MissingIndirectArgs;
        }
        break;
    case RasterCommandKind::DrawPatches:
        if (g.kind != GeometryKind::Patches)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.patch_count == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    case RasterCommandKind::DrawMulti: // ⭐ RAF-8: CPU multi-draw batch — N vertex counts over one storage buffer
        if (g.kind != GeometryKind::MultiStoragePull)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.multi_counts == nullptr || g.draw_count == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    case RasterCommandKind::DrawMultiIndexed: // ⭐ RAF-8: CPU multi-draw batch of N IndexedDraw records
        if (g.kind != GeometryKind::MultiIndexed)
        {
            return CommandError::GeometryCommandMismatch;
        }
        if (g.multi_indexed == nullptr || g.draw_count == 0)
        {
            return CommandError::ZeroDraw;
        }
        break;
    }

    return validate_bindings(packet.bindings);
}

CommandError validate_dispatch(const DispatchDesc& dispatch) noexcept
{
    if (dispatch.kernel == nullptr)
    {
        return CommandError::NullProgram;
    }
    if (dispatch.kind == DispatchKind::Direct)
    {
        if (dispatch.groups_x == 0 || dispatch.groups_y == 0 || dispatch.groups_z == 0)
        {
            return CommandError::ZeroDraw;
        }
    }
    else if (dispatch.args_buffer == nullptr)
    {
        return CommandError::MissingIndirectArgs;
    }
    return validate_bindings(dispatch.bindings);
}

CommandError validate_trace(const TraceDesc& trace) noexcept
{
    if (trace.raygen == nullptr || trace.miss == nullptr || trace.closest_hit == nullptr)
    {
        return CommandError::NullProgram;
    }
    if (trace.acceleration_structure == nullptr)
    {
        return CommandError::MissingAccelerationStructure;
    }
    if (trace.width == 0 || trace.height == 0 || trace.depth == 0)
    {
        return CommandError::ZeroDraw;
    }
    return validate_bindings(trace.bindings);
}
} // namespace crd::gpu
