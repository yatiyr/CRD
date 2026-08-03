#include <crd/gpu/command_model.hpp>

#include <memory>

// RAF-2b — the TRANSLATING command encoder. Records the backend-neutral command model (command_model.hpp) by
// lowering each RenderingDesc/RasterDrawPacket/DispatchDesc/TransferDesc/TraceDesc through the verb implementations
// already on the IRasterContext. Because those verbs are virtual, one encoder drives BOTH backends' real GPU lowering
// with zero duplicated code and byte-identical output to the legacy path. RAF-12 inlines the verb bodies into
// per-backend encoders and deletes the verbs; until then the encoder and the verbs are the SAME lowering, two ways.
//
// EVERY command-model kind is mapped here — no no-ops. A rendering scope brackets N draws; the FIRST draw honours the
// attachment LoadOp (clear-vs-load), subsequent draws in the scope always LOAD (never re-clear).

namespace crd::gpu
{
namespace
{
IStorageBuffer* first_storage(const ResourceBindingTable& bindings) noexcept
{
    const crd::u32 n = static_cast<crd::u32>(bindings.size());
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bindings[i].kind == BindingKind::StorageBuffer && bindings[i].buffer != nullptr)
        {
            return bindings[i].buffer;
        }
    }
    return nullptr;
}

const ResourceBinding* find_kind(const ResourceBindingTable& bindings, BindingKind kind) noexcept
{
    const crd::u32 n = static_cast<crd::u32>(bindings.size());
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bindings[i].kind == kind)
        {
            return &bindings[i];
        }
    }
    return nullptr;
}

// A depth SampledTexture paired with a ComparisonSampler ⇒ a shadow lookup (the comparison sampler is chosen by the
// verb identity + ITexture::is_depth(), the engine's rule).
ITexture* shadow_atlas_from(const ResourceBindingTable& bindings) noexcept
{
    if (find_kind(bindings, BindingKind::ComparisonSampler) == nullptr)
    {
        return nullptr;
    }
    const crd::u32 n = static_cast<crd::u32>(bindings.size());
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bindings[i].kind == BindingKind::SampledTexture && bindings[i].texture != nullptr &&
            bindings[i].texture->is_depth())
        {
            return bindings[i].texture;
        }
    }
    return nullptr;
}

IRasterContext::BlitFilter to_blit_filter(SamplerFilter f) noexcept
{
    return (f == SamplerFilter::Nearest) ? IRasterContext::BlitFilter::Nearest : IRasterContext::BlitFilter::Linear;
}

class TranslatingCommandEncoder final : public ICommandEncoder
{
public:
    explicit TranslatingCommandEncoder(IRasterContext& ctx) noexcept : m_ctx(ctx) {}

    void begin_rendering(const RenderingDesc& rendering) override
    {
        m_rendering = rendering; // copy: the encoder owns the scope for its duration
        m_in_scope = true;
        m_first = true;
    }

    void end_rendering() override { m_in_scope = false; }

    void draw(const RasterDrawPacket& packet) override
    {
        if (!m_in_scope || packet.program == nullptr)
        {
            return;
        }
        const RenderingDesc& r = m_rendering;
        IRasterProgram& prog = *packet.program;
        const GeometrySource& g = packet.geometry;
        const ClearColor clear = (r.color.size() > 0) ? r.color[0].clear : ClearColor{};
        const float clear_depth = r.depth.clear_depth;
        // When the scope declares no depth attachment, a verb that still takes a depth compare must not impose one:
        // "no depth test" is DepthCompare::Always (every fragment passes), not the struct's LessEqual default.
        const DepthCompare compare = r.depth.enabled ? r.depth.compare : DepthCompare::Always;
        const bool clears = m_first && wants_clear(r);
        const crd::u32 count = g.vertex_or_index_count;
        IStorageBuffer* buf = first_storage(packet.bindings);
        IRasterTarget* color0 = has_color(r) ? r.color[0].target : nullptr;
        m_first = false;

        // Binding-driven specialisations (a bindless array or a shadow atlas selects a specialised verb).
        const ResourceBinding* bindless = find_kind(packet.bindings, BindingKind::BindlessTextureArray);
        ITexture* shadow_atlas = shadow_atlas_from(packet.bindings);

        switch (g.kind)
        {
        case GeometryKind::None:
            if (bindless != nullptr && color0 != nullptr)
            {
                m_ctx.draw_bindless(*color0, prog, clear, bindless->texture_array, bindless->array_count, count);
            }
            else if (color0 != nullptr)
            {
                m_ctx.draw(*color0, prog, clear, count); // attributeless (fullscreen)
            }
            break;

        case GeometryKind::StoragePull:
            if (buf == nullptr)
            {
                break;
            }
            if (bindless != nullptr && color0 != nullptr)
            {
                m_ctx.draw_bindless(*color0, prog, clear, bindless->texture_array, bindless->array_count, count);
            }
            else if (shadow_atlas != nullptr && r.depth.enabled && color0 != nullptr)
            {
                if (clears)
                {
                    m_ctx.draw_storage_shadowed_depth(*color0, prog, clear, clear_depth, compare, *buf, *shadow_atlas,
                                                      count);
                }
                else
                {
                    m_ctx.draw_storage_shadowed_depth_load(*color0, prog, compare, *buf, *shadow_atlas, count);
                }
            }
            else if (r.color.size() == 0 && r.depth.enabled && r.depth.target != nullptr)
            {
                if (clears)
                {
                    m_ctx.draw_storage_depth_only(*r.depth.target, prog, clear_depth, compare, *buf, count);
                }
                else
                {
                    m_ctx.draw_storage_depth_only_load(*r.depth.target, prog, compare, *buf, count);
                }
            }
            else if (r.color.size() >= 2)
            {
                IRasterTarget* targets[kMaxColorAttachments];
                BlendMode blends[kMaxColorAttachments];
                const crd::u32 cc = static_cast<crd::u32>(r.color.size());
                for (crd::u32 i = 0; i < cc; ++i)
                {
                    targets[i] = r.color[i].target;
                    blends[i] = r.color[i].blend;
                }
                m_ctx.draw_storage_mrt(targets, cc, prog, clear, clear_depth, compare, *buf, count, blends);
            }
            else if (r.depth.enabled && r.depth.target != nullptr && color0 != nullptr)
            {
                if (clears)
                {
                    m_ctx.draw_storage_depth(*color0, prog, clear, clear_depth, compare, *buf, count);
                }
                else
                {
                    m_ctx.draw_storage_depth_load(*color0, prog, compare, *buf, count);
                }
            }
            else if (color0 != nullptr)
            {
                m_ctx.draw_storage(*color0, prog, clear, *buf, count);
            }
            break;

        case GeometryKind::Indexed:
            if (buf != nullptr && color0 != nullptr)
            {
                m_ctx.draw_storage_indexed_depth(*color0, prog, clear, clear_depth, compare, *buf,
                                                 static_cast<crd::u32>(g.index_offset), count, g.instance_count,
                                                 !clears);
            }
            break;

        case GeometryKind::Indirect:
        case GeometryKind::IndirectCount:
            // The engine's indirect draws are indexed (Nanite-style). One verb serves both: pass a count buffer for
            // IndirectCount, null for a fixed max_draws.
            if (buf != nullptr && g.args_buffer != nullptr && color0 != nullptr)
            {
                m_ctx.draw_storage_multi_indexed_indirect(
                    *color0, prog, clear, clear_depth, compare, *buf, static_cast<crd::u32>(g.index_offset), nullptr,
                    nullptr, *g.args_buffer, static_cast<crd::u32>(g.args_offset), g.count_buffer,
                    static_cast<crd::u32>(g.count_offset), g.max_draws, !clears, 0U);
            }
            break;

        case GeometryKind::Meshlet:
            if (buf != nullptr && color0 != nullptr)
            {
                if (clears)
                {
                    m_ctx.draw_mesh_storage(*color0, prog, clear, *buf, g.group_count_x);
                }
                else
                {
                    m_ctx.draw_mesh_storage_load(*color0, prog, *buf, g.group_count_x);
                }
            }
            break;

        case GeometryKind::MeshletIndirect:
            if (color0 != nullptr)
            {
                if (g.args_buffer != nullptr)
                {
                    m_ctx.draw_mesh_indirect_buffer(*color0, prog, clear, *g.args_buffer, g.args_offset);
                }
                else if (g.native_args != nullptr)
                {
                    m_ctx.draw_mesh_indirect(*color0, prog, clear, g.native_args, g.args_offset);
                }
            }
            break;

        case GeometryKind::Patches:
            if (buf != nullptr && color0 != nullptr)
            {
                if (clears)
                {
                    m_ctx.draw_tess_storage(*color0, prog, clear, *buf, g.patch_count);
                }
                else
                {
                    m_ctx.draw_tess_storage_load(*color0, prog, *buf, g.patch_count);
                }
            }
            break;
        }
    }

    void dispatch(const DispatchDesc& d) override
    {
        if (d.kernel == nullptr)
        {
            return;
        }
        IStorageBuffer* bufs[kMaxBindings];
        crd::u32 n = 0;
        const crd::u32 bn = static_cast<crd::u32>(d.bindings.size());
        for (crd::u32 i = 0; i < bn && n < kMaxBindings; ++i)
        {
            if (d.bindings[i].kind == BindingKind::StorageBuffer && d.bindings[i].buffer != nullptr)
            {
                bufs[n++] = d.bindings[i].buffer;
            }
        }
        if (d.kind == DispatchKind::Direct)
        {
            m_ctx.dispatch_kernel(*d.kernel, d.groups_x, d.groups_y, d.groups_z, bufs, n);
        }
        else if (d.args_buffer != nullptr)
        {
            m_ctx.dispatch_kernel_indirect(*d.kernel, *d.args_buffer, d.args_offset, bufs, n);
        }
    }

    void transfer(const TransferDesc& t) override
    {
        switch (t.kind)
        {
        case TransferKind::Clear:
            if (t.dst != nullptr)
            {
                m_ctx.clear(*t.dst, t.clear);
            }
            break;
        case TransferKind::Copy:
            if (t.dst != nullptr && t.src != nullptr)
            {
                m_ctx.copy_image(*t.dst, *t.src);
            }
            break;
        case TransferKind::Blit:
            if (t.dst != nullptr && t.src != nullptr)
            {
                m_ctx.blit_image(*t.dst, *t.src, to_blit_filter(t.filter));
            }
            break;
        case TransferKind::Resolve:
            if (t.dst != nullptr && t.src != nullptr)
            {
                m_ctx.resolve_image(*t.dst, *t.src);
            }
            break;
        }
    }

    void trace_rays(const TraceDesc& t) override
    {
        if (t.raygen == nullptr || t.miss == nullptr || t.closest_hit == nullptr ||
            t.acceleration_structure == nullptr)
        {
            return;
        }
        IStorageBuffer* bufs[kMaxBindings];
        crd::u32 n = 0;
        const crd::u32 bn = static_cast<crd::u32>(t.bindings.size());
        for (crd::u32 i = 0; i < bn && n < kMaxBindings; ++i)
        {
            if (t.bindings[i].kind == BindingKind::StorageBuffer && t.bindings[i].buffer != nullptr)
            {
                bufs[n++] = t.bindings[i].buffer;
            }
        }
        // Select the backend verb by which optional SBT stages are present.
        if (t.intersection != nullptr || t.callable != nullptr)
        {
            m_ctx.trace_rays_full(*t.raygen, *t.miss, *t.closest_hit, t.any_hit, t.intersection, t.callable,
                                  *t.acceleration_structure, t.width, t.height, bufs, n);
        }
        else if (t.any_hit != nullptr)
        {
            m_ctx.trace_rays_anyhit(*t.raygen, *t.miss, *t.closest_hit, *t.any_hit, *t.acceleration_structure, t.width,
                                    t.height, bufs, n);
        }
        else
        {
            m_ctx.trace_rays(*t.raygen, *t.miss, *t.closest_hit, *t.acceleration_structure, t.width, t.height, bufs, n);
        }
    }

private:
    static bool has_color(const RenderingDesc& r) noexcept
    {
        return r.color.size() > 0 && r.color[0].target != nullptr;
    }
    static bool wants_clear(const RenderingDesc& r) noexcept
    {
        if (r.color.size() > 0)
        {
            return r.color[0].load == LoadOp::Clear;
        }
        return r.depth.load == LoadOp::Clear;
    }

    IRasterContext& m_ctx;
    RenderingDesc m_rendering{};
    bool m_in_scope = false;
    bool m_first = true;
};
} // namespace

// The base-class default: every backend gains the encoder with no override (it lowers through the backend's own
// virtual verb implementations). Overridden per-backend only when RAF-12 inlines the lowering + deletes the verbs.
std::unique_ptr<ICommandEncoder> IRasterContext::create_command_encoder()
{
    return std::make_unique<TranslatingCommandEncoder>(*this);
}
} // namespace crd::gpu
