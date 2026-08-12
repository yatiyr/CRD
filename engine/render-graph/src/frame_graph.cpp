#include <crd/rendergraph/frame_graph.hpp>

#include <crd/ceir/gpu/render_materialize.hpp> // CEIR-16-3c: RenderResolvers + execute_render_lowered (the replay path)
#include <crd/renderpass/executor_registry.hpp>

#include <utility> // std::swap

namespace crd::rendergraph
{
using namespace crd::gpu;
using crd::renderpass::ExecutorParamType;
using crd::renderpass::pass_param_id;
using crd::renderpass::ResourceRef;
using crd::renderpass::TypedValue;

// ── lookups ──
const GraphResource* FrameGraphTemplate::find_resource(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_resources.size(); ++i)
    {
        if (m_resources[i].name_hash == name_hash)
        {
            return &m_resources[i];
        }
    }
    return nullptr;
}

const CompiledResource* CompiledFrameGraph::find(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_resources.size(); ++i)
    {
        if (m_resources[i].name_hash == name_hash)
        {
            return &m_resources[i];
        }
    }
    return nullptr;
}

const ResolvedResource* ResourceTable::find(u64 name_hash) const noexcept
{
    for (u32 i = 0; i < m_resources.size(); ++i)
    {
        if (m_resources[i].name_hash == name_hash)
        {
            return &m_resources[i];
        }
    }
    return nullptr;
}

const DrawList* DrawListTable::find(u64 pass_name_hash) const noexcept
{
    for (u32 i = 0; i < m_lists.size(); ++i)
    {
        if (m_lists[i].pass_name_hash == pass_name_hash)
        {
            return &m_lists[i].list;
        }
    }
    return nullptr;
}

const PassPrograms* PassProgramsTable::find(u64 pass_name_hash) const noexcept
{
    for (u32 i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].pass_name_hash == pass_name_hash)
        {
            return &m_entries[i].programs;
        }
    }
    return nullptr;
}

// CEIR-16-3c: look up a migrated pass's CEIR replay plan by pass name hash (null for a non-migrated pass).
const CeirPassPlan* CeirPlanTable::find(u64 pass_name_hash) const noexcept
{
    for (u32 i = 0; i < m_plans.size(); ++i)
    {
        if (m_plans[i].pass_name_hash == pass_name_hash)
        {
            return &m_plans[i].plan;
        }
    }
    return nullptr;
}

// ── RecordContext ──
namespace
{
const ResourceRef* find_ref(const PassPayload& payload, u64 slot_name) noexcept
{
    for (u32 i = 0; i < payload.resources.size(); ++i)
    {
        if (payload.resources[i].slot_name_hash == slot_name)
        {
            return &payload.resources[i];
        }
    }
    return nullptr;
}
const TypedValue* find_param(const PassPayload& payload, u64 name) noexcept
{
    for (u32 i = 0; i < payload.params.size(); ++i)
    {
        if (payload.params[i].name_hash == name)
        {
            return &payload.params[i].value;
        }
    }
    return nullptr;
}
} // namespace

bool RecordContext::is_declared(u64 slot_name) const noexcept { return find_ref(*m_payload, slot_name) != nullptr; }

const ResolvedResource* resolve_declared(const RecordContext& /*unused*/, const PassPayload& payload,
                                         const ResourceTable& table, u64 slot_name)
{
    const ResourceRef* ref = find_ref(payload, slot_name);
    if (ref == nullptr)
    {
        return nullptr;
    }
    return table.find(ref->resource_id);
}

IRasterTarget* RecordContext::color_target(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->target : nullptr;
}
IRasterTarget* RecordContext::depth_target(u64 slot_name) const { return color_target(slot_name); }
IStorageBuffer* RecordContext::storage(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->buffer : nullptr;
}
IAccelerationStructure* RecordContext::accel(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->accel : nullptr;
}
ITexture* RecordContext::texture(u64 slot_name) const
{
    if (!is_declared(slot_name))
    {
        m_diags->error(DiagCode::InvalidSlot, "record function touched an undeclared resource slot");
        m_ok = false;
        return nullptr;
    }
    const ResolvedResource* r = resolve_declared(*this, *m_payload, *m_table, slot_name);
    return (r != nullptr) ? r->texture : nullptr;
}

// ── GraphExecutorTable ──
usize GraphExecutorTable::lower_bound(ExecutorTypeId id) const noexcept
{
    usize lo = 0;
    usize hi = m_entries.size();
    while (lo < hi)
    {
        const usize mid = lo + (hi - lo) / 2;
        if (m_entries[mid].id < id)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}
PassRecordFn GraphExecutorTable::find(ExecutorTypeId id) const noexcept
{
    const usize idx = lower_bound(id);
    if (idx < m_entries.size() && m_entries[idx].id == id)
    {
        return m_entries[idx].fn;
    }
    return nullptr;
}
bool GraphExecutorTable::register_record(ExecutorTypeId id, PassRecordFn fn, DiagnosticList& diags)
{
    const usize idx = lower_bound(id);
    if (idx < m_entries.size() && m_entries[idx].id == id)
    {
        diags.error(DiagCode::DuplicateExecutor, "a record function for this executor is already registered");
        return false;
    }
    m_entries.push_back(Entry{id, fn});
    for (usize j = m_entries.size() - 1; j > idx; --j)
    {
        std::swap(m_entries[j], m_entries[j - 1]);
    }
    return true;
}

// ── built-in record functions: payload + resources → the canonical command model ──
namespace
{
ClearColor clear_from(const PassPayload& payload)
{
    const TypedValue* cc = find_param(payload, pass_param_id("clear_color"));
    if (cc != nullptr && cc->type == ExecutorParamType::Vec4)
    {
        return ClearColor{cc->v4[0], cc->v4[1], cc->v4[2], cc->v4[3]};
    }
    return ClearColor{};
}
u32 u32_param(const PassPayload& payload, StringView name, u32 fallback)
{
    const TypedValue* v = find_param(payload, pass_param_id(name));
    return (v != nullptr && v->type == ExecutorParamType::U32) ? v->u : fallback;
}
// A BOOL param (`load` / `load_depth` are declared Bool by the scene.raster schema — reading them as U32 always
// missed, so the depth-prepass load flag never fired from a cooked payload).
bool bool_param(const PassPayload& payload, StringView name, bool fallback)
{
    const TypedValue* v = find_param(payload, pass_param_id(name));
    return (v != nullptr && v->type == ExecutorParamType::Bool) ? v->b : fallback;
}
// ⛔ An ENUM param (`shading_rate` / `conservative` are declared Enum by the fullscreen.raster schema). Reading them as
// U32 always MISSED — the enum ORDINAL rides the same u32 union member, but `u32_param` checks the U32 type TAG and so
// fell back to 0 (Rate1x1 / conservative Off), which is why VRS and conservative raster silently NEVER fired through
// the executor path (only the sync `draw_vrs`, which bypasses the payload, ever coarsened). An Enum read checks the
// Enum tag and reads `e`.
u32 enum_param(const PassPayload& payload, StringView name, u32 fallback)
{
    const TypedValue* v = find_param(payload, pass_param_id(name));
    return (v != nullptr && v->type == ExecutorParamType::Enum) ? v->e : fallback;
}

// One CPU multi-draw verb's cap — a run longer than this splits into consecutive multi commands (mirrors the
// frame-cook `kMaxDrawItems`). ⛔ SINGLE SOURCE OF TRUTH: aliased from ceir-gpu's render_materialize.hpp so the CEIR scene
// template (emit_scene_list) and this legacy recorder coalesce on IDENTICAL run boundaries — A/B pixel parity depends on it.
constexpr crd::u32 kMaxSceneRun = crd::ceir::gpu::kMaxSceneRun;

// Bind a base-colour MAP (a non-depth SampledTexture, no comparison sampler) — the encoder's `map_texture` reads it.
void bind_map(RasterDrawPacket& pk, ITexture* tex)
{
    pk.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::SampledTexture, 1U, nullptr, tex});
}
// Bind a shadow/moment ATLAS at slot 4/5. ⛔⛔ REN-40-D: the SAMPLER at slot 5 is a COMPARISON sampler for a DEPTH
// atlas (a PCF shadow lookup) but a PLAIN filtering sampler for a COLOUR-ARRAY atlas (the moment/variance tiers read
// the stored moments, not a compare result). Binding a comparison sampler where the moment shader declares a plain
// `sampler2DArray` made the moment sample return garbage → EVERY moment shadow rendered black (the failure mode this
// distinction exists to prevent; it regressed when RAF-8 flipped the live RasterGeometry sampler selection into here).
void bind_atlas(RasterDrawPacket& pk, ITexture* tex, bool comparison)
{
    pk.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::SampledTexture, 4U, nullptr, tex});
    ResourceBinding samp{};
    samp.frequency = BindingFrequency::Material;
    samp.kind      = comparison ? BindingKind::ComparisonSampler : BindingKind::Sampler;
    samp.slot      = 5U;
    pk.bindings.push_back(samp);
}
// Attach the per-item sampled state (map / atlas / combined) exactly as the live RasterGeometry selection does.
void attach_textures(RasterDrawPacket& pk, ITexture* item_tex, ITexture* pass_tex, ITexture* tex, bool combined,
                     bool depth_tex, bool comparison)
{
    if (combined)
    {
        bind_map(pk, item_tex);
        bind_atlas(pk, pass_tex, comparison);
    }
    else if (depth_tex)
    {
        bind_atlas(pk, tex, comparison);
    }
    else if (tex != nullptr)
    {
        bind_map(pk, tex);
    }
}

void record_scene_raster(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    // `color` is OPTIONAL: a DEPTH-ONLY geometry pass (a shadow cascade, a depth prepass) has no colour attachment —
    // it renders into `depth` alone (the encoder routes a colour-less scope to the depth-only verbs).
    IRasterTarget* color = ctx.has(pass_param_id("color")) ? ctx.color_target(pass_param_id("color")) : nullptr;
    IRasterTarget* depth = ctx.has(pass_param_id("depth")) ? ctx.depth_target(pass_param_id("depth")) : nullptr;
    // A scene pass into a color-DEPTH target uses that target's BUNDLED depth as its own depth-stencil (the live scene
    // verbs read depth off the colour target) — so a depth test works, and the multi-item load-vs-clear across draws
    // takes the depth-aware storage verbs (which respect load) rather than the always-clearing plain `draw_storage`.
    if (depth == nullptr && color != nullptr && color->has_depth())
    {
        depth = color;
    }
    if (!ctx.ok() || (color == nullptr && depth == nullptr))
    {
        return;
    }

    // ⭐⭐ RAF-12.2: a TRUE MULTI-COLOUR MRT G-BUFFER (n_writes>1) — a deferred G-buffer, or the WBOIT ACCUMULATE pass
    // (accum + revealage). This is the ONE live shape the executor path historically did NOT bind (it dropped every
    // attachment past color0), so the inline `record_pass` MRT arm was its only coverage; closing it here is the
    // prerequisite for retiring that arm in the 12.2 swap. Gather the extra colour attachments (color1..color3, filled
    // contiguously by the bridge / a host) and their per-attachment blend (blend0..blend3), then record ONE clearing
    // storage-pull draw per item into all N attachments. The encoder lowers a >=2-colour StoragePull scope to
    // draw_storage_mrt, which clears a Multiply / RevealageMultiply attachment to the multiplicative identity 1 (never
    // the pass clear colour) and applies the per-attachment blend — so `Additive` accum + `RevealageMultiply` reveal
    // are authorable as one ordinary pass. Depth (when present) RIDES color0 the same way the single-colour path and
    // the retired inline arm do: draw_storage_mrt keys the depth attachment off color0->has_depth().
    if (color != nullptr)
    {
        IRasterTarget* mrt_color[4] = {color, nullptr, nullptr, nullptr};
        u32 mrt_n = 1U;
        static constexpr StringView kExtraColor[3] = {StringView("color1"), StringView("color2"),
                                                      StringView("color3")};
        for (u32 k = 0; k < 3U; ++k)
        {
            const u64 eslot = pass_param_id(kExtraColor[k]);
            if (!ctx.has(eslot))
            {
                break; // color1..3 are filled in order; the first absent one ends the MRT set (contiguous by construction)
            }
            IRasterTarget* const c = ctx.color_target(eslot);
            if (c == nullptr)
            {
                return; // a DECLARED MRT attachment that does not resolve ABORTS — never a partial G-buffer down a slot
            }
            mrt_color[mrt_n++] = c;
        }
        if (mrt_n >= 2U)
        {
            static constexpr StringView kBlendName[4] = {StringView("blend0"), StringView("blend1"),
                                                         StringView("blend2"), StringView("blend3")};
            BlendMode mblend[4]{};
            for (u32 k = 0; k < mrt_n; ++k)
            {
                mblend[k] = static_cast<BlendMode>(enum_param(payload, kBlendName[k], static_cast<u32>(BlendMode::Opaque)));
            }
            const ClearColor  mclear = clear_from(payload);
            const TypedValue* mcd = find_param(payload, pass_param_id("clear_depth"));
            const f32 mclear_depth = (mcd != nullptr && mcd->type == ExecutorParamType::F32) ? mcd->f : 1.0F;
            const TypedValue* mdcp = find_param(payload, pass_param_id("depth_compare"));
            const DepthCompare mcmp = (mdcp != nullptr && mdcp->type == ExecutorParamType::Enum)
                                          ? static_cast<DepthCompare>(mdcp->e)
                                          : DepthCompare::LessEqual;
            IRasterProgram* const mdef = ctx.programs().raster;
            const DrawList        mdraws = ctx.draws();
            for (crd::u32 i = 0; i < mdraws.count; ++i)
            {
                const RenderDrawItem& it = mdraws.items[i];
                if (it.storage == nullptr)
                {
                    continue;
                }
                IRasterProgram* const prog = it.program != nullptr ? it.program : mdef;
                if (prog == nullptr)
                {
                    continue;
                }
                RenderingDesc mrt;
                mrt.width = mrt_color[0]->width();
                mrt.height = mrt_color[0]->height();
                for (u32 k = 0; k < mrt_n; ++k)
                {
                    mrt.color.push_back(
                        ColorAttachmentDesc{mrt_color[k], LoadOp::Clear, StoreOp::Store, mclear, mblend[k]});
                }
                if (depth != nullptr)
                {
                    mrt.depth = DepthStencilAttachmentDesc{depth, true, LoadOp::Clear, StoreOp::Store, mclear_depth, true,
                                                           mcmp};
                }
                encoder.begin_rendering(mrt);
                RasterDrawPacket pk;
                pk.program = prog;
                pk.command = RasterCommandKind::Draw;
                pk.geometry.kind = GeometryKind::StoragePull;
                pk.geometry.vertex_or_index_count = it.vertex_count;
                pk.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, it.storage});
                encoder.draw(pk);
                encoder.end_rendering();
            }
            return;
        }
    }

    // The rendering SCOPE. A pass declaring `load = true` STACKS on the previous pass (never clears) — the encoder's
    // `m_first` tracks the clear-once, so every packet below just records in order; and a depth-prepass consumer
    // (`load_depth`) LOADS depth while colour still clears (REN-40-G1).
    const bool load = bool_param(payload, "load", false);
    IRasterTarget* const dims = color != nullptr ? color : depth; // depth-only takes its extent from the depth target
    RenderingDesc rd;
    rd.width = dims->width();
    rd.height = dims->height();
    if (color != nullptr)
    {
        rd.color.push_back(ColorAttachmentDesc{color, load ? LoadOp::Load : LoadOp::Clear, StoreOp::Store,
                                               clear_from(payload), BlendMode::Opaque});
    }
    if (depth != nullptr)
    {
        const TypedValue* cd = find_param(payload, pass_param_id("clear_depth"));
        const f32 clear_depth = (cd != nullptr && cd->type == ExecutorParamType::F32) ? cd->f : 1.0F;
        const bool load_depth = load || bool_param(payload, "load_depth", false);
        // ⛔ the DEPTH COMPARE is a PARAM, not a constant: the live scene is REVERSE-Z (GreaterEqual), so a hardcoded
        // LessEqual would fail every depth test and render an empty (or z-fighting) frame. Default LessEqual only when
        // the pass declared none.
        const TypedValue* dcp = find_param(payload, pass_param_id("depth_compare"));
        const DepthCompare cmp =
            (dcp != nullptr && dcp->type == ExecutorParamType::Enum) ? static_cast<DepthCompare>(dcp->e)
                                                                     : DepthCompare::LessEqual;
        rd.depth = DepthStencilAttachmentDesc{depth, true, load_depth ? LoadOp::Load : LoadOp::Clear,
                                              StoreOp::Store, clear_depth, true, cmp};
    }

    const DrawList draws = ctx.draws();
    encoder.begin_rendering(rd);

    if (draws.count == 0U)
    {
        // Legacy single-draw contract (the pass's `geometry` slot) — a full-screen triangle pull. Kept for the
        // graph-shape gates that record a scene pass without a resolved draw list.
        IStorageBuffer* geo = ctx.has(pass_param_id("geometry")) ? ctx.storage(pass_param_id("geometry")) : nullptr;
        RasterDrawPacket p;
        p.program = ctx.programs().raster;
        p.command = RasterCommandKind::Draw;
        p.geometry.kind = GeometryKind::StoragePull;
        p.geometry.vertex_or_index_count = 3U;
        if (geo != nullptr)
        {
            p.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, geo});
        }
        encoder.draw(p);
        encoder.end_rendering();
        return;
    }

    // ── RAF-8: the RESOLVED scene draw list → canonical packets — the live RasterGeometry selection, exactly. ──
    IRasterProgram* const def_prog = ctx.programs().raster;
    ITexture* const pass_tex = draws.pass_texture;      // the pass's sampled read (e.g. a shadow atlas), if any
    // ⛔ a depth (shadow) read requires the atlas to actually EXIST: `pass_texture_is_depth` is meaningless without a
    // `pass_texture`. A DEPTH-ONLY pass (a shadow cascade) reads NOTHING, so `pass_tex == nullptr` — and its plain
    // draws must NOT be routed down the sampled/shadow arm (that binds a null atlas and renders garbage).
    const bool pass_depth = pass_tex != nullptr && draws.pass_texture_is_depth;
    const bool pass_cmp   = draws.pass_texture_comparison; // REN-40-D: comparison sampler for depth, plain for a moment array
    const bool batchable_pass = pass_tex == nullptr;     // a pass that reads a texture never coalesces runs

    for (crd::u32 i = 0; i < draws.count; ++i)
    {
        const RenderDrawItem& it = draws.items[i];
        if (it.storage == nullptr)
        {
            continue;
        }
        IRasterProgram* prog = it.program != nullptr ? it.program : def_prog;
        if (prog == nullptr)
        {
            continue;
        }
        // A draw's OWN texture (base-colour map) beats the pass's sampled read; a pass depth read with no per-item
        // map is a shadow lookup for this draw; a per-item map INSIDE a depth-reading pass is the COMBINED shape.
        // ⛔ A DEPTH-ONLY pass (no colour attachment — a shadow cascade / depth prepass) binds NO textures: it renders
        // geometry into depth alone. A per-item albedo map or the pass atlas is IRRELEVANT here, so it must NOT route
        // the draw down the sampled/shadow arm (which is a COLOUR verb and would misrender or drop the depth write).
        const bool      has_color = color != nullptr;
        ITexture* const item_map  = has_color ? it.texture : nullptr;
        ITexture*       tex       = nullptr;
        if (has_color)
        {
            tex = item_map != nullptr ? item_map : pass_tex;
        }
        const bool depth_tex = has_color && item_map == nullptr && pass_depth;
        const bool combined = has_color && item_map != nullptr && pass_tex != nullptr && pass_depth;
        const ResourceBinding sbind{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, it.storage};

        // GPU-DRIVEN indirect (count in device memory): geometry routes like the depth-only indirect, textures follow
        // the classic arms, DrawIndex ROW = i. It must never fall to a CPU-count verb (the count is not knowable here).
        if (it.args != nullptr && it.index_count > 0U)
        {
            RasterDrawPacket p;
            p.program = prog;
            p.command = RasterCommandKind::DrawIndexedIndirect;
            p.geometry.kind = GeometryKind::Indirect;
            p.geometry.args_buffer = it.args;
            p.geometry.args_offset = it.args_offset;
            p.geometry.max_draws = 1U;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);
            attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp);
            encoder.draw(p);
            continue;
        }
        // INDEXED-PULL carrying per-draw texture state → the indexed SAMPLED verb (one verb, DrawIndex row = i).
        if (it.index_count > 0U && (tex != nullptr || depth_tex))
        {
            RasterDrawPacket p;
            p.program = prog;
            p.command = RasterCommandKind::DrawIndexed;
            p.geometry.kind = GeometryKind::Indexed;
            p.geometry.vertex_or_index_count = it.index_count;
            p.geometry.instance_count = it.instance_count;
            p.geometry.first_index = it.first_index;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);
            attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp);
            encoder.draw(p);
            continue;
        }
        // COMBINED / SHADOWED / TEXTURED (non-indexed) — a single StoragePull draw carrying the resolved textures.
        if (combined || (tex != nullptr))
        {
            RasterDrawPacket p;
            p.program = prog;
            p.command = RasterCommandKind::Draw;
            p.geometry.kind = GeometryKind::StoragePull;
            p.geometry.vertex_or_index_count = it.vertex_count;
            p.bindings.push_back(sbind);
            attach_textures(p, it.texture, pass_tex, tex, combined, depth_tex, pass_cmp);
            encoder.draw(p);
            continue;
        }

        // PLAIN: coalesce a RUN of consecutive plain items (same program+storage, no texture, same indexed-ness) into
        // ONE multi verb — the batching perf contract (one descriptor reset per run, not per draw).
        crd::u32 run = 1U;
        if (batchable_pass && it.texture == nullptr)
        {
            while (i + run < draws.count && run < kMaxSceneRun)
            {
                const RenderDrawItem& nx = draws.items[i + run];
                IRasterProgram* nprog = nx.program != nullptr ? nx.program : def_prog;
                if (nx.storage == nullptr || nx.texture != nullptr || nx.args != nullptr || nprog != prog ||
                    nx.indexed != it.indexed || (nx.index_count > 0U) != (it.index_count > 0U) ||
                    nx.storage != it.storage)
                {
                    break;
                }
                ++run;
            }
        }
        if (it.index_count > 0U)
        {
            // an INDEXED-PULL run is ONE indexed-multi command; a run of one still routes here (the multi verb pushes
            // the DrawIndex row a rebased indexed program needs).
            crd::gpu::IRasterContext::IndexedDraw idraws[kMaxSceneRun];
            for (crd::u32 k = 0; k < run; ++k)
            {
                const RenderDrawItem& nk = draws.items[i + k];
                idraws[k] = {nk.index_count, nk.instance_count, nk.first_index};
            }
            RasterDrawPacket p;
            p.program = prog;
            p.command = RasterCommandKind::DrawMultiIndexed;
            p.geometry.kind = GeometryKind::MultiIndexed;
            p.geometry.multi_indexed = static_cast<const crd::gpu::IRasterContext::IndexedDraw*>(idraws);
            p.geometry.draw_count = run;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);            encoder.draw(p);
            i += run - 1U;
        }
        else if (run > 1U || it.indexed)
        {
            // a non-indexed run (or a single item flagged `indexed` — its program rebases loads by DrawIndex, and only
            // the multi verb pushes the row).
            crd::u32 counts[kMaxSceneRun];
            for (crd::u32 k = 0; k < run; ++k)
            {
                counts[k] = draws.items[i + k].vertex_count;
            }
            RasterDrawPacket p;
            p.program = prog;
            p.command = RasterCommandKind::DrawMulti;
            p.geometry.kind = GeometryKind::MultiStoragePull;
            p.geometry.multi_counts = static_cast<const crd::u32*>(counts);
            p.geometry.draw_count = run;
            p.geometry.first_draw_index = i;
            p.bindings.push_back(sbind);
            encoder.draw(p);
            i += run - 1U;
        }
        else
        {
            // a single plain item.
            RasterDrawPacket p;
            p.program = prog;
            p.command = RasterCommandKind::Draw;
            p.geometry.kind = GeometryKind::StoragePull;
            p.geometry.vertex_or_index_count = it.vertex_count;
            p.bindings.push_back(sbind);
            encoder.draw(p);
        }
    }
    encoder.end_rendering();
}

void record_compute_dispatch(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    if (!ctx.ok())
    {
        return;
    }
    IGpuProgram* const kernel = ctx.programs().kernel;
    // ⭐ RAF-8 gap (b): a SAMPLED texture read (the HZB) → dispatch_kernel_sampled at the fixed post-buffer position.
    ITexture* const sampled =
        ctx.has(pass_param_id("sampled")) ? ctx.texture(pass_param_id("sampled")) : nullptr;

    // ⭐⭐ RAF-8: a compute pass that walks a DRAW LIST dispatches ONCE PER ITEM — THIS item's storage (+ its indirect
    // args) bound at slots 0..1, THIS item's workgroup count. That is how one authored cull kernel covers every mesh
    // group without the asset naming any of them (the per-group symmetry the raster passes have).
    const DrawList draws = ctx.draws();
    if (draws.count > 0U)
    {
        for (u32 i = 0; i < draws.count; ++i)
        {
            const RenderDrawItem& it = draws.items[i];
            if (it.storage == nullptr || it.dispatch_groups == 0U)
            {
                continue;
            }
            DispatchDesc d;
            d.kernel   = kernel;
            d.kind     = DispatchKind::Direct;
            d.groups_x = it.dispatch_groups;
            d.groups_y = 1U;
            d.groups_z = 1U;
            d.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, it.storage});
            if (it.args != nullptr)
            {
                d.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, it.args});
            }
            if (sampled != nullptr)
            {
                d.bindings.push_back(
                    ResourceBinding{BindingFrequency::Material, BindingKind::SampledTexture, 0U, nullptr, sampled});
            }
            encoder.dispatch(d);
        }
        return;
    }

    // A single dispatch: the declared storage buffers (storage · storage1..3, in order) + the optional sampled read.
    DispatchDesc d;
    d.kernel   = kernel;
    d.kind     = DispatchKind::Direct;
    d.groups_x = u32_param(payload, "groups_x", 1U);
    d.groups_y = u32_param(payload, "groups_y", 1U);
    d.groups_z = u32_param(payload, "groups_z", 1U);
    // ⭐ RAF-8: an `args` slot makes this a GPU-DRIVEN INDIRECT dispatch — the workgroup count comes from that buffer
    // (dispatch_kernel_indirect), not groups_*. `args_offset` is the BYTE offset of the {x,y,z} triple.
    IStorageBuffer* const args = ctx.has(pass_param_id("args")) ? ctx.storage(pass_param_id("args")) : nullptr;
    if (args != nullptr)
    {
        d.kind        = DispatchKind::Indirect;
        d.args_buffer = args;
        d.args_offset = u32_param(payload, "args_offset", 0U);
    }
    static const StringView kStorage[4] = {StringView("storage"), StringView("storage1"), StringView("storage2"),
                                           StringView("storage3")};
    for (const StringView& name : kStorage)
    {
        const u64 slot = pass_param_id(name);
        if (ctx.has(slot))
        {
            if (IStorageBuffer* sb = ctx.storage(slot); sb != nullptr)
            {
                d.bindings.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, sb});
            }
        }
    }
    if (sampled != nullptr)
    {
        d.bindings.push_back(ResourceBinding{BindingFrequency::Material, BindingKind::SampledTexture, 0U, nullptr, sampled});
    }
    encoder.dispatch(d);
}

void record_transfer_clear(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IRasterTarget* target = ctx.color_target(pass_param_id("target"));
    if (!ctx.ok())
    {
        return;
    }
    TransferDesc t;
    t.kind = TransferKind::Clear;
    t.dst = target;
    t.clear = clear_from(payload);
    encoder.transfer(t);
}

void record_transfer_op(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder, TransferKind kind)
{
    IRasterTarget* src = ctx.color_target(pass_param_id("src"));
    IRasterTarget* dst = ctx.color_target(pass_param_id("dst"));
    if (!ctx.ok())
    {
        return;
    }
    TransferDesc t;
    t.kind = kind;
    t.src = src;
    t.dst = dst;
    // ⛔ the BLIT filter is a declared Enum param (Copy/Resolve ignore it). Same scar as VRS: not reading it left the
    // filter at its Linear default, so an authored NEAREST blit (a nearest downsample) silently filtered linearly.
    t.filter = static_cast<SamplerFilter>(enum_param(payload, "filter", static_cast<u32>(SamplerFilter::Linear)));
    encoder.transfer(t);
}
void record_transfer_copy(const PassPayload& p, RecordContext& ctx, ICommandEncoder& e)
{
    record_transfer_op(p, ctx, e, TransferKind::Copy);
}
void record_transfer_blit(const PassPayload& p, RecordContext& ctx, ICommandEncoder& e)
{
    record_transfer_op(p, ctx, e, TransferKind::Blit);
}
void record_transfer_resolve(const PassPayload& p, RecordContext& ctx, ICommandEncoder& e)
{
    record_transfer_op(p, ctx, e, TransferKind::Resolve);
}

// Bind the pass's storage buffers (storage · storage1..3, in declaration order) as StorageBuffer bindings — the shared
// shape for a compute-shaped pass (inline ray query, RT pipeline). Returns how many resolved.
u32 bind_storage_run(RecordContext& ctx, ResourceBindingTable& out)
{
    static const StringView kStorage[4] = {StringView("storage"), StringView("storage1"), StringView("storage2"),
                                           StringView("storage3")};
    u32 n = 0U;
    for (const StringView& name : kStorage)
    {
        const u64 slot = pass_param_id(name);
        if (ctx.has(slot))
        {
            if (IStorageBuffer* sb = ctx.storage(slot); sb != nullptr)
            {
                out.push_back(ResourceBinding{BindingFrequency::Object, BindingKind::StorageBuffer, 0U, sb});
                ++n;
            }
        }
    }
    return n;
}

// raytrace.dispatch — an INLINE RAY-QUERY dispatch (dispatch_kernel_rt): the ray-query KERNEL, the TLAS (bound by the
// encoder at set 0/binding 0), the storage buffers at 1..N. Selected by the DispatchDesc carrying an accel.
void record_raytrace_dispatch(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IAccelerationStructure* accel = ctx.accel(pass_param_id("accel"));
    if (!ctx.ok() || accel == nullptr || ctx.programs().kernel == nullptr)
    {
        return;
    }
    DispatchDesc d;
    d.kernel                 = ctx.programs().kernel;
    d.kind                   = DispatchKind::Direct;
    d.acceleration_structure = accel;
    d.groups_x               = u32_param(payload, "groups_x", 1U);
    d.groups_y               = u32_param(payload, "groups_y", 1U);
    d.groups_z               = u32_param(payload, "groups_z", 1U);
    bind_storage_run(ctx, d.bindings);
    encoder.dispatch(d);
}

// raytrace.pipeline — a ray-tracing PIPELINE trace (trace_rays / _anyhit / _full). The SBT stage programs come from the
// host-resolved PassPrograms; the ray-gen grid is groups_x × groups_y. The encoder selects the verb by which optional
// stages are present.
void record_raytrace_pipeline(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IAccelerationStructure* accel = ctx.accel(pass_param_id("accel"));
    const PassPrograms& pr = ctx.programs();
    if (!ctx.ok() || accel == nullptr || pr.raygen == nullptr || pr.miss == nullptr || pr.closest_hit == nullptr)
    {
        return;
    }
    TraceDesc t;
    t.raygen                 = pr.raygen;
    t.miss                   = pr.miss;
    t.closest_hit            = pr.closest_hit;
    t.any_hit                = pr.any_hit;
    t.intersection           = pr.intersection;
    t.callable               = pr.callable;
    t.acceleration_structure = accel;
    t.width                  = u32_param(payload, "groups_x", 1U);
    t.height                 = u32_param(payload, "groups_y", 1U);
    bind_storage_run(ctx, t.bindings);
    encoder.trace_rays(t);
}

// visbuffer.raster — HW-raster a VS→FS program into an R32_UINT visibility target, clearing the id to `clear_id`. Each
// resolved draw writes its primitive ids; the first clears, every later one LOADS (the one image keeps EVERY id).
void record_visbuffer_raster(const PassPayload& payload, RecordContext& ctx, ICommandEncoder& encoder)
{
    IRasterTarget* color = ctx.color_target(pass_param_id("color"));
    if (!ctx.ok() || color == nullptr)
    {
        return;
    }
    RenderingDesc rd;
    rd.width = color->width();
    rd.height = color->height();
    // ⭐ RAH-1: visibility is an ORDINARY TYPED ATTACHMENT — an R32_UINT id target whose typed clear (`clear_kind == Uint`,
    // `clear_uint` = the background id) IS the whole visbuffer semantics. No `rd.visbuffer` boolean; the encoder derives
    // the id-write draw (clear-once/load-rest) from the attachment. LoadOp::Clear ⇒ FIRST draw clears, later draws load.
    ColorAttachmentDesc att{color, LoadOp::Clear, StoreOp::Store, clear_from(payload), BlendMode::Opaque};
    att.clear_kind = ClearKind::Uint;
    att.clear_uint = u32_param(payload, "clear_id", 0U);
    rd.color.push_back(att);
    IRasterProgram* const def_prog = ctx.programs().raster;
    const DrawList draws = ctx.draws();
    encoder.begin_rendering(rd);
    if (draws.count == 0U)
    {
        encoder.end_rendering();
        return;
    }
    for (u32 i = 0; i < draws.count; ++i)
    {
        const RenderDrawItem& it = draws.items[i];
        if (it.vertex_count == 0U)
        {
            continue;
        }
        IRasterProgram* prog = it.program != nullptr ? it.program : def_prog;
        if (prog == nullptr)
        {
            continue;
        }
        RasterDrawPacket p;
        p.program = prog;
        p.command = RasterCommandKind::Draw;
        p.geometry.kind = GeometryKind::None; // a procedural VS (gl_VertexIndex); the id target is the visbuffer scope
        p.geometry.vertex_or_index_count = it.vertex_count;
        encoder.draw(p);
    }
    encoder.end_rendering();
}

void record_present(const PassPayload& /*payload*/, RecordContext& ctx, ICommandEncoder& /*encoder*/)
{
    // Present is the SURFACE seam (IPresentSurface), not a command-encoder op — it declares its source (validated as
    // read here) but records no canonical command. The surface blits the source into the backbuffer at present time.
    (void)ctx.color_target(pass_param_id("source"));
}
} // namespace

// ── CEIR-16-3c: the GENERIC CEIR replay executor — drives execute_render_lowered on the pass's CeirPassPlan through the
// (frame-recording) encoder, with resolvers backed by RecordContext. This is the DELETION-IS-THE-PROOF target: the imperative
// record_fullscreen_raster above becomes DATA (the plan) + this ONE generic driver. scene.raster (16d) reuses it verbatim. ──
namespace
{
// The resolver closure — the ceir Context (for reading each binding's `source` attr) + the RecordContext (for resolution) +
// storage for the gathered bindless array (⛔ advisor: the array is read at encoder.draw(), AFTER the resolver returns, so it
// must outlive the resolver frame — it lives HERE, in the bundle on record_ceir_render's stack).
struct FsResolverBundle
{
    const crd::ceir::Context* cctx = nullptr;
    RecordContext*            rctx = nullptr;
    ITexture*                 gathered[8]{};
};
// A binding operand's SOURCE-param identity (the pass_param_id hash the builder stored as an int `source` attr on the
// operand's defining resource.declare) — advisor constraint #2: the descriptor SLOT and the source-param are distinct.
u64 fs_source_of(const crd::ceir::Context& cctx, const crd::ceir::Value* operand)
{
    const crd::ceir::Operation* const def = operand->defining_op();
    if (def == nullptr) { return 0U; }
    const crd::ceir::AttrId a = def->attr(crd::containers::StringView("source"));
    if (!a.valid()) { return 0U; }
    const crd::ceir::AttrValue v = cctx.attr_value(a);
    return v.kind == crd::ceir::AttrKind::Int ? static_cast<u64>(v.i) : 0U;
}
IRasterTarget* fs_target(const crd::ceir::Operation*, void* user)
{
    return static_cast<FsResolverBundle*>(user)->rctx->color_target(pass_param_id("color")); // single color ⇒ the "color" slot
}
IRasterProgram* fs_program(const crd::ceir::Operation*, void* user)
{
    return static_cast<FsResolverBundle*>(user)->rctx->programs().raster; // the pass's raster program (record_fullscreen_raster parity)
}
ITexture* fs_texture(const crd::ceir::Value* operand, void* user)
{
    auto* const b = static_cast<FsResolverBundle*>(user);
    return b->rctx->texture(fs_source_of(*b->cctx, operand));
}
ITexture* const* fs_texture_array(const crd::ceir::Value*, void* user, u32& out_count)
{
    // ⛔ record_fullscreen_raster L644-656 parity: gather input0..input7 in order — an UNDECLARED slot is SKIPPED (compact
    // over it), a DECLARED-but-unresolved read ABORTS (null return → the draw fails, never a shifted binding).
    auto* const b = static_cast<FsResolverBundle*>(user);
    static constexpr StringView kInputs[8] = {StringView("input0"), StringView("input1"), StringView("input2"),
                                              StringView("input3"), StringView("input4"), StringView("input5"),
                                              StringView("input6"), StringView("input7")};
    u32 n = 0U;
    for (u32 i = 0; i < 8U; ++i)
    {
        const u64 slot = pass_param_id(kInputs[i]);
        if (!b->rctx->has(slot)) { continue; } // undeclared ⇒ not part of this pass's fan; skip (compact)
        ITexture* const tx = b->rctx->texture(slot);
        if (tx == nullptr)
        {
            out_count = 0U;
            return nullptr; // a declared read that does not resolve ABORTS the draw (materialize returns false)
        }
        b->gathered[n++] = tx;
    }
    out_count = n;
    return b->gathered;
}
IStorageBuffer* fs_storage(const crd::ceir::Value* operand, void* user)
{
    auto* const b = static_cast<FsResolverBundle*>(user);
    return b->rctx->storage(fs_source_of(*b->cctx, operand));
}
// ⭐ CEIR-16-mesh-2 / 16c: the DrawList — a render.mesh_dispatch_list (amplify) OR render.scene_draw_list (16d scene) expands
// over this pass's resolved scene draw list. Count + per-index item, mapping RenderDrawItem → the ceir-gpu-neutral
// RasterDrawItem ON DEMAND (the layering boundary). ⛔ amplify reads only the {program, vertex_count, storage} SUBSET (the
// amplify count IS `vertex_count` — task-mesh groups / tess patches, NOT a vertex count); the scene path reads the FULL set.
u32 fs_draws_count(void* user)
{
    return static_cast<FsResolverBundle*>(user)->rctx->draws().count;
}
void fs_draws_item(void* user, u32 index, crd::ceir::gpu::RasterDrawItem& out)
{
    const DrawList dl = static_cast<FsResolverBundle*>(user)->rctx->draws();
    if (index >= dl.count) { return; } // defensive: out stays default (count 0 ⇒ skipped); the walk already bounds by count
    const RenderDrawItem& it = dl.items[index];
    out.program              = it.program;
    out.storage              = it.storage;
    out.texture              = it.texture;
    out.args                 = it.args;
    out.vertex_count         = it.vertex_count;
    out.index_count          = it.index_count;
    out.instance_count       = it.instance_count;
    out.first_index          = it.first_index;
    out.args_offset          = it.args_offset;
    out.indexed              = it.indexed;
}
// ⭐ CEIR-16d: the PASS-level sampled atlas a render.scene_draw_list binds per-draw (DrawList::pass_texture) + its shape.
// ⛔⛔ REN-40-D: pass_texture_is_depth routes it to the atlas slot 4/5 (not the base-colour map slot 1); pass_texture_
// comparison picks a COMPARISON vs PLAIN sampler (a moment/variance COLOUR array reads through a plain one). Null ⇒ untextured.
ITexture* fs_pass_texture(void* user, bool& out_is_depth, bool& out_comparison)
{
    const DrawList dl = static_cast<FsResolverBundle*>(user)->rctx->draws();
    out_is_depth   = dl.pass_texture_is_depth;
    out_comparison = dl.pass_texture_comparison;
    return dl.pass_texture;
}
} // namespace

void record_ceir_render(const PassPayload& /*payload*/, RecordContext& ctx, ICommandEncoder& encoder)
{
    const CeirPassPlan* const plan = ctx.plan();
    if (plan == nullptr || plan->ctx == nullptr || plan->commands == nullptr || plan->count == 0U)
    {
        // ⛔ CEIR-16-3d-3: fullscreen.raster records ONLY through this generic replay now, so reaching it with NO plan is a
        // load-path bug (build_frame_plans failed, or a frame-install site did not rebuild the plan table). FAIL the record
        // LOUD (not a silent black pass) — the load-time build already logged the specific cause.
        ctx.fail();
        return;
    }
    FsResolverBundle bundle;
    bundle.cctx = plan->ctx;
    bundle.rctx = &ctx;
    crd::ceir::gpu::RenderResolvers r;
    r.target             = &fs_target;
    r.target_user        = &bundle;
    r.program            = &fs_program;
    r.program_user       = &bundle;
    r.texture            = &fs_texture;
    r.texture_user       = &bundle;
    r.texture_array      = &fs_texture_array;
    r.texture_array_user = &bundle;
    r.storage            = &fs_storage;
    r.storage_user       = &bundle;
    r.draws_count        = &fs_draws_count;
    r.draws_item         = &fs_draws_item;
    r.draws_user         = &bundle;
    r.pass_texture       = &fs_pass_texture;
    r.pass_texture_user  = &bundle;
    (void)crd::ceir::gpu::execute_render_lowered(
        *plan->ctx, crd::containers::ConstSpan<crd::ceir::gpu::LoweredCommand>(plan->commands, plan->count), encoder, r);
}

u32 register_builtin_records(GraphExecutorTable& table, DiagnosticList& diags)
{
    using crd::renderpass::executor_type_id;
    u32 n = 0;
    n += table.register_record(executor_type_id("scene.raster"), record_scene_raster, diags) ? 1U : 0U;
    // ⭐ CEIR-16-3d-3: fullscreen.raster records through the GENERIC CEIR replay (record_ceir_render, driven by the
    // per-pass plan built at frame load). The imperative record_fullscreen_raster is DELETED — the composite is DATA now.
    n += table.register_record(executor_type_id("fullscreen.raster"), record_ceir_render, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("compute.dispatch"), record_compute_dispatch, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.clear"), record_transfer_clear, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.copy"), record_transfer_copy, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.blit"), record_transfer_blit, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("transfer.resolve"), record_transfer_resolve, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("raytrace.dispatch"), record_raytrace_dispatch, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("raytrace.pipeline"), record_raytrace_pipeline, diags) ? 1U : 0U;
    // ⭐ CEIR-16-mesh-2: mesh.raster + tess.raster record through the GENERIC CEIR replay (record_ceir_render, driven by a
    // build_amplify_ceir plan whose mesh_dispatch_list the walk expands over ctx.draws()). record_amplify_raster is DELETED.
    n += table.register_record(executor_type_id("mesh.raster"), record_ceir_render, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("tess.raster"), record_ceir_render, diags) ? 1U : 0U;
    // ⭐ CEIR-16-mesh-1: mesh.indirect records through the GENERIC CEIR replay (a build_mesh_indirect_ceir plan built at
    // frame load); the imperative record_mesh_indirect is DELETED — descriptor-parity proven, deletion-is-the-proof.
    n += table.register_record(executor_type_id("mesh.indirect"), record_ceir_render, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("visbuffer.raster"), record_visbuffer_raster, diags) ? 1U : 0U;
    n += table.register_record(executor_type_id("present"), record_present, diags) ? 1U : 0U;
    return n;
}

// ── compile ──
namespace
{
bool pass_touches(const GraphPass& p, u64 res_id, bool want_write) noexcept
{
    for (u32 i = 0; i < p.payload.resources.size(); ++i)
    {
        const ResourceRef& r = p.payload.resources[i];
        if (r.resource_id != res_id)
        {
            continue;
        }
        const bool writes = (r.access == SlotAccess::Write || r.access == SlotAccess::ReadWrite);
        const bool reads = (r.access == SlotAccess::Read || r.access == SlotAccess::ReadWrite);
        if (want_write ? writes : reads)
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool compile(const FrameGraphTemplate& tmpl, const ExecutorRegistry& schemas, u32 width, u32 height,
             CompiledFrameGraph& out, DiagnosticList& diags)
{
    const Array<GraphPass>& passes = tmpl.passes();
    const Array<GraphResource>& resources = tmpl.resources();
    const u32 np = static_cast<u32>(passes.size());

    // 1. Validate: executor + payload against the schema registry; every bound resource is a declared graph resource.
    for (u32 i = 0; i < np; ++i)
    {
        if (!crd::renderpass::validate_payload(schemas, passes[i].payload, diags))
        {
            return false;
        }
        for (u32 j = 0; j < passes[i].payload.resources.size(); ++j)
        {
            if (tmpl.find_resource(passes[i].payload.resources[j].resource_id) == nullptr)
            {
                diags.error(DiagCode::InvalidSlot, "pass binds a resource that is not a declared graph resource");
                return false;
            }
        }
    }

    // 2. Deterministic schedule: an edge from a producer (latest earlier writer of a read resource) to the consumer;
    //    Kahn's algorithm with min-index tie-breaking.
    Array<u32> indeg(out.m_schedule.allocator());
    Array<u8> adj(out.m_schedule.allocator()); // np x np adjacency matrix (dedup edges)
    indeg.resize(np, 0U);
    adj.resize(static_cast<usize>(np) * np, u8{0});
    for (u32 i = 0; i < np; ++i)
    {
        for (u32 rr = 0; rr < passes[i].payload.resources.size(); ++rr)
        {
            const ResourceRef& ref = passes[i].payload.resources[rr];
            const bool reads = (ref.access == SlotAccess::Read || ref.access == SlotAccess::ReadWrite);
            if (!reads)
            {
                continue;
            }
            for (u32 j = i; j-- > 0;) // latest earlier writer
            {
                if (pass_touches(passes[j], ref.resource_id, true))
                {
                    if (adj[static_cast<usize>(j) * np + i] == 0U)
                    {
                        adj[static_cast<usize>(j) * np + i] = 1U;
                        ++indeg[i];
                    }
                    break;
                }
            }
        }
    }

    out.m_schedule.clear();
    Array<u8> emitted(out.m_schedule.allocator());
    emitted.resize(np, u8{0});
    for (u32 produced = 0; produced < np; ++produced)
    {
        u32 pick = np;
        for (u32 i = 0; i < np; ++i)
        {
            if (emitted[i] == 0U && indeg[i] == 0U)
            {
                pick = i;
                break;
            }
        }
        if (pick == np)
        {
            diags.error(DiagCode::CyclicDependency, "frame graph pass dependencies contain a cycle");
            return false;
        }
        emitted[pick] = 1U;
        out.m_schedule.push_back(pick);
        for (u32 k = 0; k < np; ++k)
        {
            if (adj[static_cast<usize>(pick) * np + k] != 0U && indeg[k] > 0U)
            {
                --indeg[k];
            }
        }
    }

    // 3. Lifetimes (in schedule order) + 4. aliasing. Persistent/History get dedicated slots; transients alias when
    //    their lifetimes do not overlap and their (size_class, kind) match.
    out.m_resources.clear();
    struct PhysSlot
    {
        u64 size_class;
        SlotResourceKind kind;
        u32 last_use;
        bool dedicated;
    };
    Array<PhysSlot> slots(out.m_resources.allocator());
    for (u32 ri = 0; ri < resources.size(); ++ri)
    {
        const GraphResource& res = resources[ri];
        // lifetime over schedule positions
        u32 first_use = np;
        u32 last_use = 0;
        bool used = false;
        for (u32 pos = 0; pos < out.m_schedule.size(); ++pos)
        {
            const GraphPass& p = passes[out.m_schedule[pos]];
            if (pass_touches(p, res.name_hash, false) || pass_touches(p, res.name_hash, true))
            {
                if (!used)
                {
                    first_use = pos;
                    used = true;
                }
                last_use = pos;
            }
        }

        u32 physical = 0;
        const bool transient = (res.lifetime == ResourceLifetime::Transient);
        bool assigned = false;
        if (transient && used)
        {
            for (u32 s = 0; s < slots.size(); ++s)
            {
                if (!slots[s].dedicated && slots[s].kind == res.kind && slots[s].size_class == res.size_class &&
                    slots[s].last_use < first_use)
                {
                    physical = s;
                    slots[s].last_use = last_use;
                    assigned = true;
                    break;
                }
            }
        }
        if (!assigned)
        {
            physical = static_cast<u32>(slots.size());
            slots.push_back(PhysSlot{res.size_class, res.kind, used ? last_use : 0U, !transient});
        }
        out.m_resources.push_back(CompiledResource{res.name_hash, physical, res.lifetime, res.kind, res.size_class});
    }

    out.m_physical_slot_count = static_cast<u32>(slots.size());
    out.m_width = width;
    out.m_height = height;
    return true;
}

// ── execute ──
bool execute(const CompiledFrameGraph& compiled, const FrameGraphTemplate& tmpl, const GraphExecutorTable& records,
             const ResourceTable& table, const PassPrograms& programs, ICommandEncoder& encoder, DiagnosticList& diags,
             const DrawListTable* draw_lists, const PassProgramsTable* pass_programs, const CeirPlanTable* plans)
{
    for (u32 pos = 0; pos < compiled.schedule().size(); ++pos)
    {
        const GraphPass& pass = tmpl.passes()[compiled.schedule()[pos]];
        const PassRecordFn fn = records.find(pass.payload.executor);
        if (fn == nullptr)
        {
            diags.error(DiagCode::UnknownExecutor, "no record function registered for this executor");
            return false;
        }
        const DrawList* dl = draw_lists != nullptr ? draw_lists->find(pass.name_hash) : nullptr;
        // ⭐⭐ RAF-12.2-b: this pass's OWN programs (shadow VS / lit program / tonemap FS / cull kernel / RT SBT) if the
        // host bound them; else the frame-wide `programs` (the single-program tests + the legacy one-program shape).
        const PassPrograms* pp = pass_programs != nullptr ? pass_programs->find(pass.name_hash) : nullptr;
        // CEIR-16-3c: this pass's CEIR replay plan (null for a non-migrated pass — its executor uses its C++ record path).
        const CeirPassPlan* plan = plans != nullptr ? plans->find(pass.name_hash) : nullptr;
        RecordContext ctx(pass.payload, table, pp != nullptr ? *pp : programs, diags, dl, plan);
        fn(pass.payload, ctx, encoder);
        if (!ctx.ok())
        {
            return false; // a record function touched an undeclared resource
        }
    }
    return true;
}

// ── execute_frame: device execution in ONE SUBMISSION via a gpu-context frame graph ──
namespace
{
struct PassClosure
{
    const PassPayload* payload = nullptr;
    PassRecordFn fn = nullptr;
    const ResourceTable* table = nullptr;
    const PassPrograms* programs = nullptr;
    ICommandEncoder* encoder = nullptr;
    DiagnosticList* diags = nullptr;
    const DrawList* draws = nullptr; // RAF-8: this pass's resolved scene draw list (null for a pass with none)
    const CeirPassPlan* plan = nullptr; // CEIR-16-3c: this pass's replay plan (null for a non-migrated pass)
    bool ok = true;
};

// The FgExecuteFn bridge: run one pass's record function against the frame-recording encoder, so the canonical
// command model records into the frame's ONE command buffer (the frame-graph-shaped verbs then take their native
// frame-recording path). The gpu-context frame graph is IN frame recording here, so `encoder`'s verbs record.
void run_pass_cb(crd::gpu::IFrameContext& /*fctx*/, void* user)
{
    auto* c = static_cast<PassClosure*>(user);
    RecordContext ctx(*c->payload, *c->table, *c->programs, *c->diags, c->draws, c->plan);
    c->fn(*c->payload, ctx, *c->encoder);
    c->ok = ctx.ok();
}

crd::gpu::FgPassKind fg_kind_for(crd::renderpass::QueueKind q) noexcept
{
    switch (q)
    {
    case crd::renderpass::QueueKind::Transfer:
        return crd::gpu::FgPassKind::Transfer;
    case crd::renderpass::QueueKind::Compute:
        return crd::gpu::FgPassKind::Compute;
    case crd::renderpass::QueueKind::Graphics:
    default:
        return crd::gpu::FgPassKind::Raster;
    }
}

// One imported resource's frame-graph handle (image or buffer), keyed by the graph resource's name hash.
struct ImportedHandle
{
    u64 name_hash = 0;
    crd::gpu::FgImage img{};
    crd::gpu::FgBuffer buf{};
    bool is_buffer = false;
};
const ImportedHandle* find_handle(const Array<ImportedHandle>& hs, u64 name_hash) noexcept
{
    for (u32 i = 0; i < hs.size(); ++i)
    {
        if (hs[i].name_hash == name_hash)
        {
            return &hs[i];
        }
    }
    return nullptr;
}
} // namespace

bool execute_frame(const CompiledFrameGraph& compiled, const FrameGraphTemplate& tmpl, const GraphExecutorTable& records,
                   const ResourceTable& table, const PassPrograms& programs, IRasterContext& raster,
                   memory::IAllocator& alloc, DiagnosticList& diags, u32* out_submit_count,
                   const DrawListTable* draw_lists, const PassProgramsTable* pass_programs, const CeirPlanTable* plans)
{
    auto fg = raster.create_frame_graph();
    if (fg == nullptr)
    {
        return false; // the backend has no frame graph — a capability absence; the caller skips (no error diag)
    }
    auto encoder = raster.create_command_encoder();
    if (encoder == nullptr)
    {
        return false;
    }

    // 1. Import every resolved resource into the frame graph, keyed by its graph name hash. The frame graph owns the
    //    cross-pass barriers + end-of-frame readback for these imports.
    Array<ImportedHandle> handles(&alloc);
    handles.reserve(tmpl.resources().size());
    for (u32 i = 0; i < tmpl.resources().size(); ++i)
    {
        const GraphResource& res = tmpl.resources()[i];
        const ResolvedResource* r = table.find(res.name_hash);
        if (r == nullptr)
        {
            continue; // a declared-but-unbound resource is legal if no pass touches it
        }
        ImportedHandle h;
        h.name_hash = res.name_hash;
        bool ok = false;
        switch (res.kind)
        {
        case SlotResourceKind::ColorTarget:
        case SlotResourceKind::DepthTarget:
            if (r->target != nullptr)
            {
                h.img = fg->import_target(*r->target);
                ok = h.img.valid();
            }
            break;
        case SlotResourceKind::Texture:
            if (r->texture != nullptr)
            {
                h.img = fg->import_texture(*r->texture); // read-only ordering; pre-uploaded textures still bind if unsupported
                ok = h.img.valid();
            }
            break;
        case SlotResourceKind::StorageBuffer:
            if (r->buffer != nullptr)
            {
                h.buf = fg->import_storage(*r->buffer);
                h.is_buffer = true;
                ok = h.buf.valid();
            }
            break;
        case SlotResourceKind::UniformBuffer:
        case SlotResourceKind::AccelStructure:
            break; // no gpu-context frame-graph import path — a record function reads these straight from the table
        }
        if (ok)
        {
            handles.push_back(h);
        }
    }

    // 2. One frame-graph pass per scheduled entry: declare its reads/writes from the payload, then a callback that
    //    records it through the frame-recording encoder. Reserve so closure pointers handed to execute() never move.
    Array<PassClosure> closures(&alloc);
    closures.reserve(compiled.schedule().size());
    for (u32 pos = 0; pos < compiled.schedule().size(); ++pos)
    {
        const GraphPass& pass = tmpl.passes()[compiled.schedule()[pos]];
        const PassRecordFn fn = records.find(pass.payload.executor);
        if (fn == nullptr)
        {
            diags.error(DiagCode::UnknownExecutor, "no record function registered for this executor");
            return false;
        }
        crd::gpu::IFramePassBuilder& b = fg->add_pass("rendergraph-pass", fg_kind_for(pass.payload.queue));
        for (u32 j = 0; j < pass.payload.resources.size(); ++j)
        {
            const ResourceRef& ref = pass.payload.resources[j];
            const ImportedHandle* h = find_handle(handles, ref.resource_id);
            if (h == nullptr)
            {
                continue; // an unbound optional slot — the record function guards on ctx.has()
            }
            const bool is_write = (ref.access == SlotAccess::Write || ref.access == SlotAccess::ReadWrite);
            const bool is_rw = (ref.access == SlotAccess::ReadWrite);
            if (h->is_buffer)
            {
                if (is_rw) { b.read_writes(h->buf); }
                else if (is_write) { b.writes(h->buf); }
                else { b.reads(h->buf); }
            }
            else if (is_rw) { b.read_writes(h->img); }
            else if (is_write) { b.writes(h->img); }
            else if (ref.kind == SlotResourceKind::DepthTarget) { b.reads_depth(h->img); }
            else { b.reads(h->img); }
        }
        const DrawList* dl = draw_lists != nullptr ? draw_lists->find(pass.name_hash) : nullptr;
        // ⭐⭐ RAF-12.2-b: this pass's OWN programs if the host bound them per pass; else the frame-wide default.
        const PassPrograms* pp = pass_programs != nullptr ? pass_programs->find(pass.name_hash) : nullptr;
        // CEIR-16-3c: this pass's CEIR replay plan (null for a non-migrated pass).
        const CeirPassPlan* plan = plans != nullptr ? plans->find(pass.name_hash) : nullptr;
        closures.push_back(
            PassClosure{&pass.payload, fn, &table, pp != nullptr ? pp : &programs, encoder.get(), &diags, dl, plan, true});
        b.execute(&run_pass_cb, &closures[closures.size() - 1U]);
    }

    if (!fg->build())
    {
        diags.error(DiagCode::ExecutionFailed, "gpu-context frame graph failed to build");
        return false;
    }
    fg->execute(); // ONE submission — barriers, transient handling and end-of-frame readback owned by the frame graph
    if (out_submit_count != nullptr)
    {
        *out_submit_count = fg->last_submit_count();
    }

    for (u32 i = 0; i < closures.size(); ++i)
    {
        if (!closures[i].ok)
        {
            return false; // a record function touched an undeclared resource
        }
    }
    return !diags.has_errors();
}

// ── RAF-12.2-b: the authored-frame runtime — the ONE generic dispatch + the orchestrator (replaces FrameRecorder) ──
namespace
{
// The ONE authored-pass record callback: the generic dispatch that replaces record_pass + the 11 per-kind wrappers. It
// resolves the host's FgImage/FgBuffer handles to device pointers at execute time (the only moment a transient exists),
// applies the pass's device setup, builds the RecordContext, and invokes the pass's registered executor.
void run_authored_cb(crd::gpu::IFrameContext& fctx, void* user)
{
    auto&                     ap = *static_cast<AuthoredPass*>(user);
    crd::gpu::IRasterContext& r  = fctx.raster();
    // per-pass DEVICE setup (mirrors the legacy record_pass preamble): compute diagnostics, counter zeroing, this pass's
    // sampler + raster state (context state, reset to defaults at the pass boundary so a pass never inherits a neighbour).
    r.compute_diag(8U);
    if (ap.device_kind == crd::gpu::FgPassKind::Compute) { r.compute_diag(9U); }
    for (crd::u32 i = 0; i < ap.n_counters; ++i)
    {
        if (crd::gpu::IStorageBuffer* cb = fctx.buffer(ap.counters[i]); cb != nullptr) { r.fill_buffer(*cb, 0U, 4U, 0U); }
    }
    if (ap.has_sampler) { r.set_sampler(ap.sampler); }
    r.set_pass_state(ap.state);

    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        ap.ok = false;
        return;
    }

    // resolve the payload slots → a ResourceTable of device pointers (a transient resolves NOW, via the frame context).
    ResourceTable table(ap.alloc);
    for (crd::u32 i = 0; i < ap.bindings.size(); ++i)
    {
        const SlotBinding& b = ap.bindings[i];
        ResolvedResource   rr{};
        rr.name_hash = b.name_hash;
        rr.kind      = b.kind;
        switch (b.resolve)
        {
        case SlotResolve::ImageLayer:
            rr.target = fctx.image_layer(b.image, b.layer);
            break;
        case SlotResolve::ImageWithDepth:
            rr.target = fctx.image_with_depth(b.image, b.depth);
            break;
        case SlotResolve::Texture:
            rr.texture = fctx.texture(b.image);
            break;
        case SlotResolve::Buffer:
            rr.buffer = fctx.buffer(b.buffer);
            break;
        case SlotResolve::Accel:
            rr.accel = b.accel;
            break;
        case SlotResolve::Image:
        default:
            rr.target = fctx.image(b.image);
            break;
        }
        table.bind(rr);
    }

    // resolve the draw/dispatch list (a GPU-cull output's storage/args are transients that resolve only here).
    Array<RenderDrawItem> items(ap.alloc);
    items.reserve(ap.draws.size());
    for (crd::u32 i = 0; i < ap.draws.size(); ++i)
    {
        const AuthoredDraw& d = ap.draws[i];
        // A draw with a DECLARED storage-pull buffer skips if it does not resolve (the legacy scene/compute skip). A
        // draw with NO storage (a PROCEDURAL amplification item — tess/mesh drawing `amplify_count` from VertexIndex) is
        // KEPT with a null storage; the amplify executor needs it. `has_storage` per-kind is the recorder's decision.
        crd::gpu::IStorageBuffer* sb = nullptr;
        if (d.has_storage)
        {
            sb = fctx.buffer(d.storage);
            if (sb == nullptr) { continue; }
        }
        RenderDrawItem ri{};
        ri.storage         = sb;
        ri.program         = d.program;
        ri.texture         = d.texture;
        ri.vertex_count    = d.vertex_count;
        ri.indexed         = d.indexed;
        ri.index_count     = d.index_count;
        ri.instance_count  = d.instance_count;
        ri.first_index     = d.first_index;
        ri.args            = d.args; // a host pointer, used directly (the legacy wrappers used DrawItem.args as-is)
        ri.args_offset     = d.args_offset;
        ri.dispatch_groups = d.dispatch_groups;
        items.push_back(ri);
    }
    DrawList dl{};
    dl.items                   = items.data();
    dl.count                   = static_cast<crd::u32>(items.size());
    dl.pass_texture            = ap.has_pass_texture ? fctx.texture(ap.pass_texture) : nullptr;
    dl.pass_texture_is_depth   = ap.pass_texture_is_depth;
    dl.pass_texture_comparison = ap.pass_texture_comparison;

    const PassRecordFn fn = ap.records != nullptr ? ap.records->find(ap.executor) : nullptr;
    if (fn == nullptr)
    {
        if (ap.diags != nullptr)
        {
            ap.diags->error(DiagCode::UnknownExecutor, "no record function registered for this pass's executor");
        }
        ap.ok = false;
        return;
    }
    RecordContext ctx(ap.payload, table, ap.programs, *ap.diags, &dl, ap.plan); // CEIR-16-3c: thread the pass's replay plan
    fn(ap.payload, ctx, *enc);
    ap.ok = ctx.ok();
}
} // namespace

crd::gpu::FgExecuteFn authored_pass_fn() noexcept { return &run_authored_cb; }
} // namespace crd::rendergraph
