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

// A single SampledTexture with NO comparison sampler ⇒ the ordinary textured fullscreen draw (draw_textured). The
// comparison sampler — not the format — is what selects a SHADOW lookup (shadow_atlas_from, checked FIRST): a DEPTH
// texture bound WITHOUT one is a RAW-float read (the HZB / TAA `depth_as_float` shape), sampled through the ordinary
// sampler, so it must be accepted here rather than falling through to a procedural no-op.
ITexture* plain_sampled_texture(const ResourceBindingTable& bindings) noexcept
{
    if (find_kind(bindings, BindingKind::ComparisonSampler) != nullptr)
    {
        return nullptr;
    }
    const crd::u32 n = static_cast<crd::u32>(bindings.size());
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bindings[i].kind == BindingKind::SampledTexture && bindings[i].texture != nullptr)
        {
            return bindings[i].texture;
        }
    }
    return nullptr;
}

// The albedo MAP — a non-depth SampledTexture — REGARDLESS of a comparison sampler being present. Unlike
// plain_sampled_texture, this survives the COMBINED textured+shadowed shape (map@1/2 + atlas@4/5, one scene draw).
ITexture* map_texture(const ResourceBindingTable& bindings) noexcept
{
    const crd::u32 n = static_cast<crd::u32>(bindings.size());
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bindings[i].kind == BindingKind::SampledTexture && bindings[i].texture != nullptr &&
            !bindings[i].texture->is_depth())
        {
            return bindings[i].texture;
        }
    }
    return nullptr;
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
        // ⭐ RAF-8: the DEPTH-PREPASS shape — a colour+depth scope that CLEARS colour but LOADS depth (the depth was
        // written by a prior pass). The clearing scene verbs clear BOTH attachments, so signal the backend to load
        // depth for this next clearing draw (the live set_next_draw_load_depth path, now driven by the per-attachment
        // LoadOp). No-op unless this draw actually clears + depth is a LOAD.
        if (clears && r.depth.enabled && r.depth.load == LoadOp::Load)
        {
            m_ctx.set_next_draw_load_depth(true);
        }
        m_first = false;

        // Binding-driven specialisations (a bindless array or a shadow atlas selects a specialised verb).
        const ResourceBinding* bindless = find_kind(packet.bindings, BindingKind::BindlessTextureArray);
        ITexture* shadow_atlas = shadow_atlas_from(packet.bindings);

        switch (g.kind)
        {
        case GeometryKind::None:
        {
            // The fullscreen family (the live RasterFullscreen precedence): VRS / conservative are draw ATTRIBUTES;
            // then 0 reads → procedural draw, 1 read → textured/shadow, N reads → bindless (+ optional constants).
            if (color0 == nullptr)
            {
                break;
            }
            // ⭐ RAF-8: the VISIBILITY-BUFFER draw (an R32_UINT id target). The FIRST draw of the scope clears to the
            // integer `clear_id`; every later one LOADS (draw_visbuffer_load) so the one image keeps EVERY draw's ids
            // — a per-draw clear would leave only the last mesh's, the visibility scar in its most silent form.
            if (r.visbuffer)
            {
                if (clears)
                {
                    m_ctx.draw_visbuffer(*color0, prog, r.clear_id, count);
                }
                else
                {
                    m_ctx.draw_visbuffer_load(*color0, prog, count);
                }
                break;
            }
            const RasterState& st = packet.state;
            if (st.vrs_pipeline_rate != ShadingRate::Rate1x1)
            {
                m_ctx.draw_vrs(*color0, prog, clear, st.vrs_pipeline_rate, st.vrs_primitive_combiner, count);
                break;
            }
            if (st.conservative != ConservativeMode::Off)
            {
                m_ctx.draw_conservative(*color0, prog, clear, st.conservative, count);
                break;
            }
            if (bindless != nullptr)
            {
                // ⭐ RAF-8: the COMPOSITE draw — a bindless fullscreen whose attachment LOADS (not clears) BLENDS over
                // the existing target (WBOIT's `rgb·(1-reveal) + background·reveal` resolve). draw_bindless_blend_load
                // reads what is there and blends; a clearing bindless would wipe the background the OIT resolve needs.
                const bool loads_blend = r.color.size() > 0 && r.color[0].load == LoadOp::Load;
                if (loads_blend)
                {
                    m_ctx.draw_bindless_blend_load(*color0, prog, bindless->texture_array, bindless->array_count, count,
                                                   r.color[0].blend);
                    break;
                }
                // ⭐⭐ a fullscreen bindless pass that ALSO binds a constants buffer takes the storage variant (b0) —
                // the only path a fullscreen FS gets per-frame matrices (the live TAA resolve shape).
                IStorageBuffer* cbuf = first_storage(packet.bindings);
                if (cbuf != nullptr)
                {
                    m_ctx.draw_bindless_storage(*color0, prog, clear, bindless->texture_array, bindless->array_count,
                                                *cbuf, count);
                }
                else
                {
                    m_ctx.draw_bindless(*color0, prog, clear, bindless->texture_array, bindless->array_count, count);
                }
                break;
            }
            if (shadow_atlas != nullptr)
            {
                m_ctx.draw_shadow(*color0, prog, clear, *shadow_atlas, count);
                break;
            }
            if (ITexture* tex = plain_sampled_texture(packet.bindings); tex != nullptr)
            {
                m_ctx.draw_textured(*color0, prog, clear, *tex, count);
                break;
            }
            m_ctx.draw(*color0, prog, clear, count); // attributeless / procedural
            break;
        }

        case GeometryKind::StoragePull:
            if (buf == nullptr)
            {
                break;
            }
            if (bindless != nullptr && color0 != nullptr)
            {
                m_ctx.draw_bindless(*color0, prog, clear, bindless->texture_array, bindless->array_count, count);
            }
            // ⭐⭐ RAF-8: the COMBINED textured+shadowed scene draw — an albedo MAP (non-depth SampledTexture @1/2)
            // AND a shadow ATLAS (depth SampledTexture + ComparisonSampler @4/5), ONE draw. Checked FIRST (most
            // specific) so a group carrying a base-colour map does not lose its shadow the moment shadows turn on.
            else if (map_texture(packet.bindings) != nullptr && shadow_atlas != nullptr && r.depth.enabled &&
                     color0 != nullptr)
            {
                ITexture* map = map_texture(packet.bindings);
                if (clears)
                {
                    m_ctx.draw_storage_textured_shadowed_depth(*color0, prog, clear, clear_depth, compare, *buf, *map,
                                                               *shadow_atlas, count);
                }
                else
                {
                    m_ctx.draw_storage_textured_shadowed_depth_load(*color0, prog, compare, *buf, *map, *shadow_atlas,
                                                                    count);
                }
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
            // ⭐ RAF-8: the TEXTURED scene draw — a base-colour map, no atlas (draw_storage_textured_depth).
            else if (map_texture(packet.bindings) != nullptr && r.depth.enabled && color0 != nullptr)
            {
                ITexture* map = map_texture(packet.bindings);
                if (clears)
                {
                    m_ctx.draw_storage_textured_depth(*color0, prog, clear, clear_depth, compare, *buf, *map, count);
                }
                else
                {
                    m_ctx.draw_storage_textured_depth_load(*color0, prog, compare, *buf, *map, count);
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
                // ⭐⭐ RAF-8 / REN-39-C1: an INDEXED-PULL item carrying per-draw texture state (a base-colour MAP @1/2
                // and/or a shadow ATLAS @4/5) takes the indexed SAMPLED verb — ONE verb, the shape chosen by which
                // textures the bindings resolve, and the DrawIndex ROW pushed so a rebased program reads its region.
                // With NO textures it is the plain indexed depth draw.
                ITexture* map = map_texture(packet.bindings);
                ITexture* atlas = shadow_atlas_from(packet.bindings);
                if (map != nullptr || atlas != nullptr)
                {
                    m_ctx.draw_storage_indexed_sampled_depth(*color0, prog, clear, clear_depth, compare, *buf,
                                                             static_cast<crd::u32>(g.index_offset), count,
                                                             g.instance_count, g.first_index, map, atlas, !clears,
                                                             g.first_draw_index);
                }
                else
                {
                    m_ctx.draw_storage_indexed_depth(*color0, prog, clear, clear_depth, compare, *buf,
                                                     static_cast<crd::u32>(g.index_offset), count, g.instance_count,
                                                     !clears);
                }
            }
            break;

        case GeometryKind::Indirect:
        case GeometryKind::IndirectCount:
            // The engine's indirect draws are indexed (Nanite-style). One verb serves both: pass a count buffer for
            // IndirectCount, null for a fixed max_draws. ⭐⭐ RAF-8 / REN-40-A: a GPU-driven scene item carries the
            // same per-draw MAP/ATLAS the CPU-count arms do (bound → passed; velocity binds neither → both null) and
            // the DrawIndex ROW (`first_draw_index`) so the rebased program reads its region base from device memory.
            if (buf != nullptr && g.args_buffer != nullptr && color0 == nullptr && r.depth.enabled &&
                r.depth.target != nullptr)
            {
                // ⭐ RAF-8 gap (d): a DEPTH-ONLY GPU-driven pass (a shadow cascade under device cull) — no colour
                // attachment, the count from device memory, the DrawIndex row rebasing each command's region.
                m_ctx.draw_storage_multi_indexed_depth_only_indirect(
                    *r.depth.target, prog, clear_depth, compare, *buf, static_cast<crd::u32>(g.index_offset),
                    *g.args_buffer, static_cast<crd::u32>(g.args_offset), g.count_buffer,
                    static_cast<crd::u32>(g.count_offset), g.max_draws, !clears, g.first_draw_index);
            }
            else if (buf != nullptr && g.args_buffer != nullptr && color0 != nullptr)
            {
                ITexture* map = map_texture(packet.bindings);
                ITexture* atlas = shadow_atlas_from(packet.bindings);
                m_ctx.draw_storage_multi_indexed_indirect(
                    *color0, prog, clear, clear_depth, compare, *buf, static_cast<crd::u32>(g.index_offset), map, atlas,
                    *g.args_buffer, static_cast<crd::u32>(g.args_offset), g.count_buffer,
                    static_cast<crd::u32>(g.count_offset), g.max_draws, !clears, g.first_draw_index);
            }
            break;

        case GeometryKind::Meshlet:
            if (color0 != nullptr)
            {
                // ⭐ RAF-8: a mesh-shader amplification draw. Carrying a STORAGE buffer pulls its meshlets from that
                // buffer (the GEO-1 seam); with none it is a PROCEDURAL mesh dispatch (draw_mesh). Either way the FIRST
                // draw of the scope clears, every later one LOADS — a per-draw clear renders only the last meshlet.
                if (buf != nullptr)
                {
                    if (clears) { m_ctx.draw_mesh_storage(*color0, prog, clear, *buf, g.group_count_x); }
                    else        { m_ctx.draw_mesh_storage_load(*color0, prog, *buf, g.group_count_x); }
                }
                else
                {
                    if (clears) { m_ctx.draw_mesh(*color0, prog, clear, g.group_count_x); }
                    else        { m_ctx.draw_mesh_load(*color0, prog, g.group_count_x); }
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
            if (color0 != nullptr)
            {
                // ⭐ RAF-8: a tessellation amplification draw — storage-pull control points (GEO-1) or a PROCEDURAL
                // patch grid (draw_tess). FIRST clears, later ones LOAD (a per-draw clear renders only the last patch).
                if (buf != nullptr)
                {
                    if (clears) { m_ctx.draw_tess_storage(*color0, prog, clear, *buf, g.patch_count); }
                    else        { m_ctx.draw_tess_storage_load(*color0, prog, *buf, g.patch_count); }
                }
                else
                {
                    if (clears) { m_ctx.draw_tess(*color0, prog, clear, g.patch_count); }
                    else        { m_ctx.draw_tess_load(*color0, prog, g.patch_count); }
                }
            }
            break;

        case GeometryKind::MultiStoragePull:
            // ⭐ RAF-8: the CPU multi-draw BATCH (the scene run of plain items) — ONE verb, N vertex counts, the
            // DrawIndex row base rebased per command. `!clears` is the load flag (the first draw of the scope clears).
            if (buf != nullptr && color0 != nullptr && g.multi_counts != nullptr && g.draw_count > 0U)
            {
                m_ctx.draw_storage_multi_depth(*color0, prog, clear, clear_depth, compare, *buf, g.multi_counts,
                                               g.draw_count, g.first_draw_index, !clears);
            }
            break;

        case GeometryKind::MultiIndexed:
            if (buf != nullptr && g.multi_indexed != nullptr && g.draw_count > 0U && color0 == nullptr &&
                r.depth.enabled && r.depth.target != nullptr)
            {
                // ⭐ RAF-8 gap (d): the DEPTH-ONLY CPU indexed batch — a run of shadow-cascade items into depth alone.
                m_ctx.draw_storage_multi_indexed_depth_only(*r.depth.target, prog, clear_depth, compare, *buf,
                                                            static_cast<crd::u32>(g.index_offset), g.multi_indexed,
                                                            g.draw_count, !clears);
            }
            else if (buf != nullptr && color0 != nullptr && g.multi_indexed != nullptr && g.draw_count > 0U)
            {
                m_ctx.draw_storage_multi_indexed_depth(*color0, prog, clear, clear_depth, compare, *buf,
                                                       static_cast<crd::u32>(g.index_offset), g.multi_indexed,
                                                       g.draw_count, g.first_draw_index, !clears);
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
        ITexture* sampled = nullptr; // ⭐ RAF-8 gap (b): a kernel that reads a SAMPLED texture (the HZB occlusion cull)
        const crd::u32 bn = static_cast<crd::u32>(d.bindings.size());
        for (crd::u32 i = 0; i < bn && n < kMaxBindings; ++i)
        {
            if (d.bindings[i].kind == BindingKind::StorageBuffer && d.bindings[i].buffer != nullptr)
            {
                bufs[n++] = d.bindings[i].buffer;
            }
            else if (d.bindings[i].kind == BindingKind::SampledTexture && d.bindings[i].texture != nullptr)
            {
                sampled = d.bindings[i].texture;
            }
        }
        if (d.kind == DispatchKind::Direct)
        {
            // ⭐ RAF-8: an INLINE RAY-QUERY dispatch — a Direct dispatch carrying an acceleration structure binds the
            // TLAS at set 0/binding 0 and the storage buffers at 1..N, exactly as the offline `trace_dispatch` rig, so
            // a ray-query kernel runs unchanged inside the frame's one submission (dispatch_kernel_rt). Checked FIRST:
            // an accel-bearing dispatch is never an ordinary buffer-only kernel.
            if (d.acceleration_structure != nullptr)
            {
                m_ctx.dispatch_kernel_rt(*d.kernel, *d.acceleration_structure, d.groups_x, d.groups_y, d.groups_z, bufs,
                                         n);
            }
            // a sampled texture selects the sampled-dispatch verb (kernel binds the texture + a nearest-clamp sampler
            // at the fixed post-buffer positions); no texture ⇒ the ordinary buffer-only dispatch.
            else if (sampled != nullptr)
            {
                m_ctx.dispatch_kernel_sampled(*d.kernel, d.groups_x, d.groups_y, d.groups_z, bufs, n, *sampled);
            }
            else
            {
                m_ctx.dispatch_kernel(*d.kernel, d.groups_x, d.groups_y, d.groups_z, bufs, n);
            }
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
