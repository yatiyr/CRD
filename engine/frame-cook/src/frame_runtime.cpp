// frame_runtime.cpp — REN-36.2: drive `IFrameGraph` from a cooked `FrameGraphDesc`. See frame_runtime.hpp.

#include <crd/framecook/frame_runtime.hpp>

#include <crd/containers/array.hpp>
#include <crd/gpu/command_model.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/rendergraph/frame_graph.hpp>
#include <crd/renderpass/executor_registry.hpp>

#include <cstring>

CRD_DEFINE_LOG_CHANNEL(g_log_framecook, "FrameCook", crd::log::LogLevel::Info)

namespace crd::framecook
{
namespace
{
namespace g = crd::gpu;

bool name_is(const crd::containers::String& s, const char* lit)
{
    const crd::usize n = std::strlen(lit);
    return s.size() == n && std::memcmp(s.c_str(), lit, n) == 0;
}

// Per-pass recording state. Lives in an Array owned by the executor for the whole build+execute, because the
// graph stores the `void* user` pointer and calls back during execute().
// REN-38-A3: how many image reads one pass may bind. A stated cap, checked, never a silent truncation.
constexpr crd::u32 kMaxPassReads = 8U;

struct PassRec
{
    const FramePassDesc* desc = nullptr;
    g::FgImage           target{};      // what this pass writes (the first image write)
    // REN-38-A1b: ALL the images this pass WRITES. A `raster.mrt` pass declares N and every one is an attachment;
    // keeping only the first made a deferred G-buffer inexpressible in exactly the way keeping only the first
    // READ made a deferred LIGHTING pass inexpressible (38-A3).
    g::FgImage           writes_all[kMaxPassReads]{};
    crd::u32             n_writes = 0U;
    // ⛔ REN-38-A3: ALL the images this pass READS, not just the first. A pass used to keep ONE `sampled` handle,
    // which made a DEFERRED LIGHTING pass — the canonical N-texture consumer, reading albedo + normal + material +
    // depth — literally inexpressible: the asset could declare four reads, the cooker validated them, the graph
    // ordered and barriered them, and the executor bound exactly one. The declared-but-ignored failure again.
    g::FgImage           sampled[kMaxPassReads]{};
    crd::u32             n_sampled        = 0U;
    bool                 sampled_is_depth = false; // of sampled[0] — the shadow-lookup case
    bool                 sampled_is_array = false; // of sampled[0] — the ATLAS case (REN-40-D: depth OR moments)
    // ⭐⭐ REN-41 (TAA): a fullscreen pass's per-frame CONSTANTS buffer (the reproject matrix). A fullscreen pass
    // that declares a buffer read which is NOT the indirect-args buffer captures it here; the executor binds it
    // at set 0/binding 0 via draw_bindless_storage. Invalid for every pass that declares no such read.
    g::FgBuffer          fs_constants{};
    g::FgBuffer          storage{};
    g::IRasterProgram*   program = nullptr;
    crd::u32             vertex_count = 0U;
    // REN-36.3: which instance of an expanded `for_each` pass this is, and whether its target write carried the
    // `[$index]` subscript (⇒ it renders into ONE SLICE of a layered resource rather than the whole image).
    crd::u32             layer          = 0U;
    bool                 indexed_target = false;
    // REN-36: the RESOLVED draw list (N draws) plus each draw's imported storage handle. The handles must be
    // imported at BUILD time (so the graph tracks and barriers them) but resolved to buffers at RECORD time,
    // hence two parallel arrays rather than one.
    DrawListBinding      draws{};
    g::FgBuffer          storage_of[kMaxDrawItems]{};
    // REN-38-A2: a COMPUTE pass's kernel, its storage bindings (from the declared reads+writes, in order) and
    // its declared grid.
    g::IGpuProgram*      kernel_program = nullptr;
    g::FgBuffer          kernel_bufs[kMaxPassReads]{};
    crd::u32             n_kernel_bufs  = 0U;
    crd::u32             groups[3]      = {1U, 1U, 1U};
    // REN-38-A7/A8: the declared `patches` / `groups` count for an amplification pass with no draw list.
    crd::u32             amplify_count  = 0U;
    crd::u32             clear_id       = 0U; // REN-38-A11: the visibility buffer's background id
    // REN-38-B3: counter buffers this pass must ZERO before it runs. A counter that is not reset accumulates
    // across frames, so the append walks past the end of its list on frame 2 and the GPU-driven draw reads
    // garbage indices — a failure that gets WORSE the longer the app runs.
    g::FgBuffer          counters[kMaxPassReads]{};
    crd::u32             n_counters     = 0U;
    g::IGpuProgram*      rt_raygen      = nullptr; // REN-38-A16: the three programs a pipeline is built from
    g::IGpuProgram*      rt_miss        = nullptr;
    g::IGpuProgram*      rt_chit        = nullptr;
    g::IGpuProgram*      rt_anyhit      = nullptr; // REN-38 audit: the OPTIONAL any-hit joining the hit group
    g::IGpuProgram*      rt_isect       = nullptr; // REN-38-F13: procedural hit shapes (optional)
    g::IGpuProgram*      rt_callable    = nullptr; // REN-38-F13: the SBT fourth table (optional)
    // REN-38-A9/A10: the acceleration structure a raytrace pass traverses, and the buffer an indirect pass
    // takes its count from. Both are resolved at RECORD time and held here, like every other pass input.
    g::IAccelerationStructure* accel      = nullptr;
    g::FgBuffer          args_buf{};
    crd::u64             args_offset    = 0U;
    // ⛔ PRECEDENCE. A for_each instance's program (from `instance_program`) must beat the draw list's per-draw
    // program, or every expanded cascade renders with the FIRST cascade's shader — all slices identical, which
    // is exactly the degenerate state the cascade gate exists to reject. Tracked explicitly rather than inferred.
    bool                 program_is_instance = false;
    bool                 load_override       = false; // REN-40-E: for_each_load → preserve persistent contents
    g::FgImage           depth_target{};              // REN-40-G3: shared_depth — a separate depth attachment
    // ⭐⭐ RAF-8a (ADR-0106) the migration adapter: a MIGRATED FramePassKind records through the render-graph executor
    // instead of the inline switch below. `rec_alloc` backs the per-pass ResourceTable/payload; `records` finds the
    // executor by id. Both null on a not-yet-migrated pass ⇒ the inline path runs (both resolve during migration).
    crd::memory::IAllocator*                     rec_alloc = nullptr;
    const crd::rendergraph::GraphExecutorTable*  records   = nullptr;
};

// ⭐ REN-38-B1: a STABLE key for a persistent image — FNV-1a over the resource NAME. ⛔ Not the declaration
// index: an index shifts the moment someone adds a resource above it in the file, and a persistent image is
// looked up BY KEY across frames — so a shifted key silently swaps two histories (TAA reading the auto-exposure
// buffer) or discards one. The name is what the author actually stated, so the name is what keys it.
[[nodiscard]] crd::u32 name_key(const crd::containers::String& n) noexcept
{
    crd::u32 h = 2166136261U;
    for (crd::usize i = 0; i < n.size(); ++i)
    {
        h ^= static_cast<crd::u32>(static_cast<crd::u8>(n.c_str()[i]));
        h *= 16777619U;
    }
    return h;
}

// ⭐⭐ RAF-8a (ADR-0106) — THE MIGRATION ADAPTER (first kind: fullscreen). Record a fullscreen pass through the
// render-graph `fullscreen.raster` EXECUTOR (the canonical command model) instead of the inline verbs below — the SAME
// verbs the RAF-7 encoder==verb gates prove byte-identical. Builds a `ResourceTable`+`PassPayload` from the resolved
// `PassRec` and calls the registered executor `PassRecordFn` through `create_command_encoder()`. Returns false if the
// adapter is not wired (⇒ the inline path runs — both resolve during migration; RAF-12 deletes both).
bool record_fullscreen_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || p->program == nullptr || t == nullptr)
    {
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("fullscreen.raster")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    const FramePassDesc& d = *p->desc;
    rp::PassPayload    payload;
    payload.executor       = rp::executor_type_id(SV("fullscreen.raster"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    // resource_id == the slot-name hash (one resource per slot here) — RecordContext matches ref.resource_id to the
    // ResolvedResource.name_hash, so the two must agree; using the slot hash for both keeps them consistent.
    const auto declare = [&](const char* slot, rp::SlotResourceKind kind, rp::SlotAccess access, g::IRasterTarget* tgt,
                             g::IStorageBuffer* buf, g::ITexture* tex)
    {
        const crd::u64 id = rp::pass_param_id(SV(slot));
        table.bind(rg::ResolvedResource{id, kind, tgt, buf, nullptr, tex});
        payload.resources.push_back(rp::ResourceRef{id, kind, access, id});
    };
    declare("color", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, t, nullptr, nullptr);
    static const char* const kInputs[8] = {"input0", "input1", "input2", "input3",
                                           "input4", "input5", "input6", "input7"};
    for (crd::u32 i = 0; i < p->n_sampled && i < 8U; ++i)
    {
        g::ITexture* tx = ctx.texture(p->sampled[i]);
        if (tx == nullptr)
        {
            return true; // resolve-or-abort (the inline path's rule) — the pass is consumed but records nothing
        }
        declare(kInputs[i], rp::SlotResourceKind::Texture, rp::SlotAccess::Read, nullptr, nullptr, tx);
    }
    if (p->fs_constants.valid())
    {
        if (g::IStorageBuffer* cbuf = ctx.buffer(p->fs_constants); cbuf != nullptr)
        {
            declare("constants", rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, nullptr, cbuf, nullptr);
        }
    }
    const auto add_param = [&](const char* name, const rp::TypedValue& v)
    { payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV(name)), v}); };
    {
        rp::TypedValue cc;
        cc.type  = rp::ExecutorParamType::Vec4;
        cc.v4[0] = d.clear_color[0];
        cc.v4[1] = d.clear_color[1];
        cc.v4[2] = d.clear_color[2];
        cc.v4[3] = d.clear_color[3];
        add_param("clear_color", cc);
    }
    if (d.shading_rate != g::ShadingRate::Rate1x1)
    {
        rp::TypedValue e;
        e.type = rp::ExecutorParamType::Enum;
        e.e    = static_cast<crd::u32>(d.shading_rate);
        add_param("shading_rate", e);
    }
    if (d.conservative != g::ConservativeMode::Off)
    {
        rp::TypedValue e;
        e.type = rp::ExecutorParamType::Enum;
        e.e    = static_cast<crd::u32>(d.conservative);
        add_param("conservative", e);
    }
    if (d.depth_as_float)
    {
        rp::TypedValue b;
        b.type = rp::ExecutorParamType::Bool;
        b.b    = true;
        add_param("depth_as_float", b);
    }
    rg::PassPrograms programs;
    programs.raster = p->program;
    rg::RecordContext rctx(payload, table, programs, diags);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8a: the migration adapter for the TRANSFER family (clear · copy · blit · resolve). Same pattern as the
// fullscreen adapter — build the ResourceTable + payload from the resolved PassRec and call the transfer executor.
bool record_transfer_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr)
    {
        return false;
    }
    const FramePassDesc& d = *p->desc;
    const char*          exec = nullptr;
    switch (d.kind)
    {
    case FramePassKind::Clear:
        exec = "transfer.clear";
        break;
    case FramePassKind::Copy:
        exec = "transfer.copy";
        break;
    case FramePassKind::Blit:
        exec = "transfer.blit";
        break;
    case FramePassKind::Resolve:
        exec = "transfer.resolve";
        break;
    default:
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV(exec)));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV(exec));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Transfer;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    const auto declare = [&](const char* slot, rp::SlotAccess access, g::IRasterTarget* tgt)
    {
        const crd::u64 id = rp::pass_param_id(SV(slot));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::ColorTarget, tgt, nullptr, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::ColorTarget, access, id});
    };
    if (d.kind == FramePassKind::Clear)
    {
        declare("target", rp::SlotAccess::Write, t);
        rp::TypedValue cc;
        cc.type  = rp::ExecutorParamType::Vec4;
        cc.v4[0] = d.clear_color[0];
        cc.v4[1] = d.clear_color[1];
        cc.v4[2] = d.clear_color[2];
        cc.v4[3] = d.clear_color[3];
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("clear_color")), cc});
    }
    else
    {
        // copy / blit / resolve: src → dst. ⛔ resolve-or-abort exactly as the inline path (a missing src leaves a
        // stale destination that reads back plausible).
        if (p->n_sampled != 1U)
        {
            return true;
        }
        g::IRasterTarget* src = ctx.image(p->sampled[0]);
        if (src == nullptr)
        {
            return true;
        }
        declare("src", rp::SlotAccess::Read, src);
        declare("dst", rp::SlotAccess::Write, t);
        if (d.kind == FramePassKind::Blit)
        {
            rp::TypedValue e;
            e.type = rp::ExecutorParamType::Enum;
            e.e    = static_cast<crd::u32>(d.filter);
            payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("filter")), e});
        }
    }
    rg::PassPrograms  programs; // a transfer pass has no shader
    rg::RecordContext rctx(payload, table, programs, diags);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8a: the migration adapter for the SCENE families — `scene.raster` covers RasterGeometry (forward · impostor),
// RasterDepthOnly (shadow cascades · depth prepass) and single-colour RasterMrt (the velocity prepass). It builds a
// render-graph `DrawList` from `p->draws` (ALREADY host-resolved), binds colour OR depth by kind, and calls the
// scene.raster executor — the same draw_storage_* verbs the encoder==verb gates prove. A true MRT G-buffer (n_writes>1)
// keeps the inline path for now (no shipped frame uses one).
bool record_scene_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr || p->program == nullptr)
    {
        return false;
    }
    const FramePassDesc&   d          = *p->desc;
    const bool             depth_only = d.kind == FramePassKind::RasterDepthOnly;
    // ⛔⛔ THE CASCADE SCAR: a DEPTH-ONLY pass binds NO colour textures — `record_scene_raster` now ignores per-item
    // maps when there is no colour attachment. Before that, a TEXTURED shadow caster routed down the indexed-SAMPLED
    // (colour) arm with a null atlas, so the cascade's textured groups vanished from the shadow map and the forward
    // read "all occluded" → black instances. (Only the untextured casters rendered, which is why it looked total.)
    if (d.kind == FramePassKind::RasterMrt && p->n_writes > 1U)
    {
        return false; // a true multi-colour G-buffer — not yet in the executor; keep the inline MRT path
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("scene.raster")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }

    // 1. build the render-graph DrawList from the resolved DrawItems. The per-item program TWIN is chosen by kind
    //    (forward · depth-only · velocity); for a for_each INSTANCE pass every item uses the pass program (twin null →
    //    the executor's default = programs().raster = p->program).
    rg::DrawList             draws{};
    crd::containers::Array<rg::RenderDrawItem> items(p->rec_alloc);
    items.reserve(p->draws.count());
    for (crd::u32 i = 0; i < p->draws.count(); ++i)
    {
        const DrawItem it = p->draws.at(i);
        if (it.storage == nullptr)
        {
            continue;
        }
        g::IStorageBuffer* sb = ctx.buffer(p->storage_of[i]);
        if (sb == nullptr)
        {
            continue;
        }
        g::IRasterProgram* twin = nullptr;
        if (!p->program_is_instance)
        {
            if (depth_only) { twin = it.program_depth != nullptr ? it.program_depth : it.program; }
            else if (d.kind == FramePassKind::RasterMrt) { twin = it.program_velocity != nullptr ? it.program_velocity : it.program; }
            else { twin = it.program; }
        }
        rg::RenderDrawItem ri{};
        ri.storage        = sb;
        ri.program        = twin;
        ri.texture        = it.texture;
        ri.vertex_count   = it.vertex_count;
        ri.indexed        = it.indexed;
        ri.index_count    = it.index_count;
        ri.instance_count = it.instance_count;
        ri.first_index    = it.first_index;
        ri.args           = it.args;
        ri.args_offset    = it.args_offset;
        items.push_back(ri);
    }
    draws.items               = items.data();
    draws.count               = static_cast<crd::u32>(items.size());
    draws.pass_texture        = p->n_sampled > 0U ? ctx.texture(p->sampled[0]) : nullptr;
    // ⛔ only meaningful when a pass texture EXISTS — a depth-only cascade reads nothing (the `sampled_is_*` flags are
    // stale then), and a true-flag with a null texture routes the cascade's plain draws down the shadow-sample arm.
    draws.pass_texture_is_depth = draws.pass_texture != nullptr && (p->sampled_is_depth || p->sampled_is_array);
    // ⛔⛔ REN-40-D: the SAMPLER type is DEPTH-ness alone, NOT array-ness. A PCF depth atlas takes a comparison sampler;
    // a moment/variance COLOUR array takes a plain one. Merging the two into `is_depth` bound a comparison sampler on
    // the moment array → every moment shadow rendered black.
    draws.pass_texture_comparison = draws.pass_texture != nullptr && p->sampled_is_depth;

    // 2. payload + resource table: colour OR depth by kind (a depth-only pass has no colour attachment; the executor
    //    renders depth-only). A colour pass into a color-DEPTH target (`image_with_depth`) uses that bundled depth.
    rp::PassPayload    payload;
    payload.executor       = rp::executor_type_id(SV("scene.raster"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const char*                slot = depth_only ? "depth" : "color";
        const rp::SlotResourceKind kind =
            depth_only ? rp::SlotResourceKind::DepthTarget : rp::SlotResourceKind::ColorTarget;
        const rp::SlotAccess access = depth_only ? rp::SlotAccess::ReadWrite : rp::SlotAccess::Write;
        const crd::u64       id     = rp::pass_param_id(SV(slot));
        table.bind(rg::ResolvedResource{id, kind, t, nullptr, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, kind, access, id});
    }
    const auto add_param = [&](const char* name, const rp::TypedValue& v)
    { payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV(name)), v}); };
    {
        rp::TypedValue cc;
        cc.type  = rp::ExecutorParamType::Vec4;
        cc.v4[0] = d.clear_color[0];
        cc.v4[1] = d.clear_color[1];
        cc.v4[2] = d.clear_color[2];
        cc.v4[3] = d.clear_color[3];
        add_param("clear_color", cc);
    }
    {
        rp::TypedValue cd;
        cd.type = rp::ExecutorParamType::F32;
        cd.f    = d.clear_depth;
        add_param("clear_depth", cd);
    }
    {
        rp::TypedValue dc;
        dc.type = rp::ExecutorParamType::Enum;
        dc.e    = static_cast<crd::u32>(d.depth);
        add_param("depth_compare", dc);
    }
    if (d.load_target || p->load_override)
    {
        rp::TypedValue b;
        b.type = rp::ExecutorParamType::Bool;
        b.b    = true;
        add_param("load", b);
    }
    if (d.load_depth)
    {
        rp::TypedValue b;
        b.type = rp::ExecutorParamType::Bool;
        b.b    = true;
        add_param("load_depth", b);
    }
    rg::PassPrograms programs;
    programs.raster = p->program; // the default program (the for_each-instance program, or the pass program)
    rg::RecordContext rctx(payload, table, programs, diags, &draws);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8a: the migration adapter for COMPUTE (the GPU-cull kernels). A compute pass with a draw list dispatches
// ONCE PER ITEM (this item's storage + indirect args + workgroup count); one without dispatches once over the declared
// kernel buffers. Both go through the compute.dispatch executor via a DrawList (dispatch_groups on each item).
bool record_compute_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || p->kernel_program == nullptr)
    {
        return false;
    }
    // the live precondition: every declared kernel buffer resolves, and there is at least one. A missing buffer ABORTS
    // (returning false runs the inline path, which aborts identically) rather than shifting every later binding down.
    g::IStorageBuffer* bufs[kMaxPassReads]{};
    crd::u32           nb = 0U;
    for (crd::u32 i = 0; i < p->n_kernel_bufs && nb < kMaxPassReads; ++i)
    {
        g::IStorageBuffer* sb = ctx.buffer(p->kernel_bufs[i]);
        if (sb == nullptr)
        {
            return false;
        }
        bufs[nb++] = sb;
    }
    if (nb == 0U)
    {
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("compute.dispatch")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV("compute.dispatch"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Compute;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    const auto u32_param = [&](const char* name, crd::u32 v)
    {
        rp::TypedValue t;
        t.type = rp::ExecutorParamType::U32;
        t.u    = v;
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV(name)), t});
    };
    u32_param("groups_x", p->groups[0]);
    u32_param("groups_y", p->groups[1]);
    u32_param("groups_z", p->groups[2]);
    // the kernel buffers → storage · storage1..3 (the single-dispatch binding; also satisfies the required `storage`).
    static const char* const kStorage[4] = {"storage", "storage1", "storage2", "storage3"};
    for (crd::u32 i = 0; i < nb && i < 4U; ++i)
    {
        const crd::u64 id = rp::pass_param_id(SV(kStorage[i]));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::StorageBuffer, nullptr, bufs[i], nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::ReadWrite, id});
    }
    if (p->n_sampled > 0U)
    {
        if (g::ITexture* tx = ctx.texture(p->sampled[0]); tx != nullptr)
        {
            const crd::u64 id = rp::pass_param_id(SV("sampled"));
            table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, tx});
            payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::Texture, rp::SlotAccess::Read, id});
        }
    }
    // ⭐ RAF-8: a ComputeIndirect pass takes its workgroup count from `args_buf` (a buffer an earlier pass wrote) — bind
    // it to the `args` slot so the executor dispatches INDIRECT (dispatch_kernel_indirect). ⛔ resolve-or-abort: a
    // missing args buffer runs the inline path (which aborts identically), never a stale direct dispatch.
    if (p->args_buf.valid())
    {
        g::IStorageBuffer* args = ctx.buffer(p->args_buf);
        if (args == nullptr)
        {
            return false;
        }
        const crd::u64 id = rp::pass_param_id(SV("args"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::StorageBuffer, nullptr, args, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, id});
        u32_param("args_offset", static_cast<crd::u32>(p->args_offset));
    }
    // the per-item DRAW LIST (each item: its storage + indirect args + workgroup count).
    rg::DrawList                               draws{};
    crd::containers::Array<rg::RenderDrawItem> items(p->rec_alloc);
    if (p->draws.count() > 0U)
    {
        items.reserve(p->draws.count());
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            g::IStorageBuffer* sb = ctx.buffer(p->storage_of[i]);
            if (sb == nullptr)
            {
                continue;
            }
            rg::RenderDrawItem ri{};
            ri.storage         = sb;
            ri.args            = it.args;
            ri.dispatch_groups = it.dispatch_groups;
            items.push_back(ri);
        }
        draws.items = items.data();
        draws.count = static_cast<crd::u32>(items.size());
    }
    rg::PassPrograms programs;
    programs.kernel = p->kernel_program;
    rg::RecordContext rctx(payload, table, programs, diags, &draws);
    fn(payload, rctx, *enc);
    return true;
}

// Resolve a pass's kernel buffers (the RT / indirect shared shape) to the storage · storage1..3 slots. Returns the
// count, or -1 if a declared buffer does not resolve OR there are more than the 4 storage slots (⇒ the caller ABORTS to
// the inline path, which handles both — a missing buffer records nothing, and >4 buffers bind all 8). Matches the
// compute convention (4 tracked buffers per compute-shaped pass; the bridge caps there too).
int bind_kernel_storage(PassRec* p, g::IFrameContext& ctx, crd::renderpass::PassPayload& payload,
                        crd::rendergraph::ResourceTable& table)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    static const char* const kStorage[4] = {"storage", "storage1", "storage2", "storage3"};
    if (p->n_kernel_bufs > 4U)
    {
        return -1;
    }
    crd::u32 nb = 0U;
    for (crd::u32 i = 0; i < p->n_kernel_bufs && nb < 4U; ++i)
    {
        g::IStorageBuffer* sb = ctx.buffer(p->kernel_bufs[i]);
        if (sb == nullptr)
        {
            return -1;
        }
        const crd::u64 id = rp::pass_param_id(SV(kStorage[nb]));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::StorageBuffer, nullptr, sb, nullptr, nullptr});
        payload.resources.push_back(
            rp::ResourceRef{id, rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::ReadWrite, id});
        ++nb;
    }
    return static_cast<int>(nb);
}

// ⭐⭐ RAF-8: the AMPLIFICATION adapter (mesh.raster / tess.raster). A RasterMesh / RasterTess pass records through the
// amplification executor: the resolved draw list gives each draw its program + amplification count (+ optional
// storage-pull buffer), or the declared `amplify_count` drives one PROCEDURAL draw.
bool record_amplify_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr || p->program == nullptr)
    {
        return false;
    }
    const FramePassDesc& d    = *p->desc;
    const bool           mesh = d.kind == FramePassKind::RasterMesh;
    const char*          exec = mesh ? "mesh.raster" : "tess.raster";
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV(exec)));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV(exec));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const crd::u64 id = rp::pass_param_id(SV("color"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::ColorTarget, t, nullptr, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, id});
    }
    {
        rp::TypedValue cc;
        cc.type  = rp::ExecutorParamType::Vec4;
        cc.v4[0] = d.clear_color[0];
        cc.v4[1] = d.clear_color[1];
        cc.v4[2] = d.clear_color[2];
        cc.v4[3] = d.clear_color[3];
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("clear_color")), cc});
    }
    if (p->draws.count() == 0U)
    {
        rp::TypedValue ac;
        ac.type = rp::ExecutorParamType::U32;
        ac.u    = p->amplify_count;
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("amplify_count")), ac});
    }
    // the draw list: per-item program TWIN (the pass program wins for a for_each instance), the amplification COUNT in
    // `vertex_count`, and an optional storage-pull buffer (the GEO-1 seam).
    rg::DrawList                               draws{};
    crd::containers::Array<rg::RenderDrawItem> items(p->rec_alloc);
    if (p->draws.count() > 0U)
    {
        items.reserve(p->draws.count());
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem     it = p->draws.at(i);
            rg::RenderDrawItem ri{};
            ri.program      = p->program_is_instance ? nullptr : it.program;
            ri.vertex_count = it.vertex_count;
            ri.storage      = it.storage != nullptr ? ctx.buffer(p->storage_of[i]) : nullptr;
            items.push_back(ri);
        }
        draws.items = items.data();
        draws.count = static_cast<crd::u32>(items.size());
    }
    rg::PassPrograms programs;
    programs.raster = p->program;
    rg::RecordContext rctx(payload, table, programs, diags, &draws);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8: the VISIBILITY-BUFFER adapter (visbuffer.raster). Each resolved draw writes its ids into the R32_UINT
// target; the first clears to `clear_id`, every later one loads (the executor + encoder keep every draw's ids).
bool record_visbuffer_via_executor(PassRec* p, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr || p->program == nullptr)
    {
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("visbuffer.raster")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV("visbuffer.raster"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const crd::u64 id = rp::pass_param_id(SV("color"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::ColorTarget, t, nullptr, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, id});
    }
    {
        rp::TypedValue ci;
        ci.type = rp::ExecutorParamType::U32;
        ci.u    = p->clear_id;
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("clear_id")), ci});
    }
    rg::DrawList                               draws{};
    crd::containers::Array<rg::RenderDrawItem> items(p->rec_alloc);
    items.reserve(p->draws.count());
    for (crd::u32 i = 0; i < p->draws.count(); ++i)
    {
        const DrawItem     it = p->draws.at(i);
        rg::RenderDrawItem ri{};
        ri.program      = p->program_is_instance ? nullptr : it.program;
        ri.vertex_count = it.vertex_count;
        items.push_back(ri);
    }
    draws.items = items.data();
    draws.count = static_cast<crd::u32>(items.size());
    rg::PassPrograms programs;
    programs.raster = p->program;
    rg::RecordContext rctx(payload, table, programs, diags, &draws);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8: the COMPOSITE adapter (fullscreen.raster + load + blend). The pass's sampled reads become the bindless
// array; `load` + `blend` route the encoder to draw_bindless_blend_load (WBOIT's blend-over-background resolve).
bool record_composite_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr || p->program == nullptr || p->n_sampled == 0U)
    {
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("fullscreen.raster")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV("fullscreen.raster"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const crd::u64 id = rp::pass_param_id(SV("color"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::ColorTarget, t, nullptr, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, id});
    }
    static const char* const kInputs[8] = {"input0", "input1", "input2", "input3",
                                            "input4", "input5", "input6", "input7"};
    for (crd::u32 i = 0; i < p->n_sampled && i < 8U; ++i)
    {
        g::ITexture* tx = ctx.texture(p->sampled[i]);
        if (tx == nullptr)
        {
            return true; // ⛔ resolve-or-abort exactly as the inline path — a composite reading the wrong order is worse
        }
        const crd::u64 id = rp::pass_param_id(SV(kInputs[i]));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::Texture, nullptr, nullptr, nullptr, tx});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::Texture, rp::SlotAccess::Read, id});
    }
    {
        rp::TypedValue b;
        b.type = rp::ExecutorParamType::Bool;
        b.b    = true;
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("load")), b});
    }
    {
        const FramePassDesc& d  = *p->desc;
        rp::TypedValue       bm;
        bm.type = rp::ExecutorParamType::Enum;
        bm.e    = static_cast<crd::u32>(d.blend.size() > 0U ? d.blend[0] : g::BlendMode::Alpha);
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("blend")), bm});
    }
    rg::PassPrograms programs;
    programs.raster = p->program;
    rg::RecordContext rctx(payload, table, programs, diags);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8: the GPU-DRIVEN MESHLET adapter (mesh.indirect). The workgroup count comes from `args_buf` (a cull pass's
// output) — draw_mesh_indirect_buffer.
bool record_mesh_indirect_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr || p->program == nullptr)
    {
        return false;
    }
    g::IStorageBuffer* args = ctx.buffer(p->args_buf);
    if (args == nullptr)
    {
        return false; // the inline path aborts identically on a missing args buffer
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("mesh.indirect")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    const FramePassDesc& d = *p->desc;
    rp::PassPayload      payload;
    payload.executor       = rp::executor_type_id(SV("mesh.indirect"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const crd::u64 id = rp::pass_param_id(SV("color"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::ColorTarget, t, nullptr, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, id});
    }
    {
        const crd::u64 id = rp::pass_param_id(SV("args"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::StorageBuffer, nullptr, args, nullptr, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, id});
    }
    {
        rp::TypedValue cc;
        cc.type  = rp::ExecutorParamType::Vec4;
        cc.v4[0] = d.clear_color[0];
        cc.v4[1] = d.clear_color[1];
        cc.v4[2] = d.clear_color[2];
        cc.v4[3] = d.clear_color[3];
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("clear_color")), cc});
    }
    {
        rp::TypedValue ao;
        ao.type = rp::ExecutorParamType::U32;
        ao.u    = static_cast<crd::u32>(p->args_offset);
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("args_offset")), ao});
    }
    rg::PassPrograms programs;
    programs.raster = p->program;
    rg::RecordContext rctx(payload, table, programs, diags);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-8: the RAY-TRACING adapters. RayTrace ⇒ an INLINE RAY QUERY (raytrace.dispatch → dispatch_kernel_rt);
// RayTracePipeline ⇒ an SBT trace (raytrace.pipeline → trace_rays / _anyhit / _full). Both bind the TLAS + the kernel
// buffers and abort (⇒ the inline path) if a buffer does not resolve or the pass declares more than 4.
bool record_raytrace_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || p->kernel_program == nullptr || p->accel == nullptr)
    {
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("raytrace.dispatch")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV("raytrace.dispatch"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Compute;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const crd::u64 id = rp::pass_param_id(SV("accel"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::AccelStructure, nullptr, nullptr, p->accel, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::AccelStructure, rp::SlotAccess::Read, id});
    }
    const int nb = bind_kernel_storage(p, ctx, payload, table);
    if (nb <= 0)
    {
        return false; // a missing buffer / >4 buffers / zero buffers ⇒ the inline path (aborts identically)
    }
    const auto u32_param = [&](const char* name, crd::u32 v)
    {
        rp::TypedValue tv;
        tv.type = rp::ExecutorParamType::U32;
        tv.u    = v;
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV(name)), tv});
    };
    u32_param("groups_x", p->groups[0]);
    u32_param("groups_y", p->groups[1]);
    u32_param("groups_z", p->groups[2]);
    rg::PassPrograms programs;
    programs.kernel = p->kernel_program;
    rg::RecordContext rctx(payload, table, programs, diags);
    fn(payload, rctx, *enc);
    return true;
}

bool record_raytrace_pipeline_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || p->accel == nullptr || p->rt_raygen == nullptr ||
        p->rt_miss == nullptr || p->rt_chit == nullptr)
    {
        return false;
    }
    const rg::PassRecordFn fn = p->records->find(rp::executor_type_id(SV("raytrace.pipeline")));
    if (fn == nullptr)
    {
        return false;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr)
    {
        return false;
    }
    rp::PassPayload payload;
    payload.executor       = rp::executor_type_id(SV("raytrace.pipeline"));
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Compute;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    {
        const crd::u64 id = rp::pass_param_id(SV("accel"));
        table.bind(rg::ResolvedResource{id, rp::SlotResourceKind::AccelStructure, nullptr, nullptr, p->accel, nullptr});
        payload.resources.push_back(rp::ResourceRef{id, rp::SlotResourceKind::AccelStructure, rp::SlotAccess::Read, id});
    }
    const int nb = bind_kernel_storage(p, ctx, payload, table);
    if (nb < 0)
    {
        return false; // a missing buffer / >4 buffers ⇒ the inline path (aborts identically)
    }
    {
        rp::TypedValue gx;
        gx.type = rp::ExecutorParamType::U32;
        gx.u    = p->groups[0];
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("groups_x")), gx});
        rp::TypedValue gy;
        gy.type = rp::ExecutorParamType::U32;
        gy.u    = p->groups[1];
        payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV("groups_y")), gy});
    }
    rg::PassPrograms programs;
    programs.raygen       = p->rt_raygen;
    programs.miss         = p->rt_miss;
    programs.closest_hit  = p->rt_chit;
    programs.any_hit      = p->rt_anyhit;
    programs.intersection = p->rt_isect;
    programs.callable     = p->rt_callable;
    rg::RecordContext rctx(payload, table, programs, diags);
    fn(payload, rctx, *enc);
    return true;
}

// ⭐⭐ RAF-10: the CUSTOM (application-defined) executor adapter. A `kind = "custom"` pass names a REGISTERED executor id
// (`app://executor/…`); the renderer resolves the app's record fn in the SAME GraphExecutorTable a builtin uses and
// drives it with a RecordContext built from the pass's DECLARED writes (color0..3) + reads (input0..7 · constants) +
// params (clear_color + the authored `params`) + the resolved program + draw list. The app's record fn touches ONLY
// what it declared (RecordContext diagnoses an undeclared slot). ⛔ No engine code NAMES the app's executor — the id is
// the extension point, so an app adds a pass MECHANIC without editing FramePassKind or any engine file.
bool record_custom_via_executor(PassRec* p, g::IFrameContext& ctx, g::IRasterContext& r, g::IRasterTarget* t)
{
    namespace rp = crd::renderpass;
    namespace rg = crd::rendergraph;
    using SV = crd::containers::StringView;
    if (p->records == nullptr || p->rec_alloc == nullptr || t == nullptr) { return false; }
    const FramePassDesc& d = *p->desc;
    if (d.executor.empty()) { return false; }
    const rp::ExecutorTypeId eid = rp::executor_type_id(SV(d.executor.c_str(), d.executor.size()));
    const rg::PassRecordFn   fn  = p->records->find(eid);
    if (fn == nullptr)
    {
        // ⛔ a NAMED-but-unregistered executor is a clear error, not a silent no-op — the pass is consumed (return true)
        // so the inline switch does not then guess a fallback, and the miss is reported by name.
        CRD_LOG_ERROR(g_log_framecook, "custom pass '{}' names executor '{}' which is not registered",
                      d.name.c_str(), d.executor.c_str());
        return true;
    }
    auto enc = r.create_command_encoder();
    if (enc == nullptr) { return false; }
    rp::PassPayload payload;
    payload.executor       = eid;
    payload.schema_version = 1U;
    payload.queue          = rp::QueueKind::Graphics;
    rg::ResourceTable  table(p->rec_alloc);
    rg::DiagnosticList diags(p->rec_alloc);
    const auto declare = [&](const char* slot, rp::SlotResourceKind kind, rp::SlotAccess access, g::IRasterTarget* tgt,
                             g::IStorageBuffer* buf, g::ITexture* tex)
    {
        const crd::u64 id = rp::pass_param_id(SV(slot));
        table.bind(rg::ResolvedResource{id, kind, tgt, buf, nullptr, tex});
        payload.resources.push_back(rp::ResourceRef{id, kind, access, id});
    };
    // writes -> color0..3 (color0 == the resolved target t; extra colour writes resolved via ctx.image)
    static const char* const kColors[4] = {"color", "color1", "color2", "color3"};
    declare(kColors[0], rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, t, nullptr, nullptr);
    for (crd::u32 w = 1U; w < p->n_writes && w < 4U; ++w)
    {
        if (g::IRasterTarget* wt = ctx.image(p->writes_all[w]); wt != nullptr)
        {
            declare(kColors[w], rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, wt, nullptr, nullptr);
        }
    }
    // reads -> input0..7 (sampled textures) + constants (a buffer read)
    static const char* const kInputs[8] = {"input0", "input1", "input2", "input3",
                                            "input4", "input5", "input6", "input7"};
    for (crd::u32 i = 0; i < p->n_sampled && i < 8U; ++i)
    {
        g::ITexture* tx = ctx.texture(p->sampled[i]);
        if (tx == nullptr) { return true; } // resolve-or-abort (the inline rule): consumed, records nothing
        declare(kInputs[i], rp::SlotResourceKind::Texture, rp::SlotAccess::Read, nullptr, nullptr, tx);
    }
    if (p->fs_constants.valid())
    {
        if (g::IStorageBuffer* cbuf = ctx.buffer(p->fs_constants); cbuf != nullptr)
        {
            declare("constants", rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, nullptr, cbuf, nullptr);
        }
    }
    const auto add_param = [&](SV name, const rp::TypedValue& v)
    { payload.params.push_back(rp::ParamValue{rp::pass_param_id(name), v}); };
    {
        rp::TypedValue cc;
        cc.type  = rp::ExecutorParamType::Vec4;
        cc.v4[0] = d.clear_color[0];
        cc.v4[1] = d.clear_color[1];
        cc.v4[2] = d.clear_color[2];
        cc.v4[3] = d.clear_color[3];
        add_param(SV("clear_color"), cc);
    }
    // the authored `params` -> typed payload params (an app executor reads its own knobs by name).
    for (crd::usize k = 0; k < d.params.size(); ++k)
    {
        const FrameParam& fp = d.params[k];
        rp::TypedValue     tv;
        switch (fp.type)
        {
        case FrameParamType::Float: tv.type = rp::ExecutorParamType::F32; tv.f = static_cast<float>(fp.v[0]); break;
        case FrameParamType::Int:   tv.type = rp::ExecutorParamType::U32; tv.u = static_cast<crd::u32>(fp.v[0]); break;
        case FrameParamType::Bool:  tv.type = rp::ExecutorParamType::Bool; tv.b = fp.v[0] != 0.0; break;
        case FrameParamType::Vec4:
            tv.type = rp::ExecutorParamType::Vec4;
            for (crd::u32 j = 0; j < 4U; ++j) { tv.v4[j] = static_cast<float>(fp.v[j]); }
            break;
        }
        add_param(SV(fp.name.c_str(), fp.name.size()), tv);
    }
    // the resolved draw list (a custom SCENE-style executor iterates it; a fullscreen custom leaves it empty).
    rg::DrawList                               draws{};
    crd::containers::Array<rg::RenderDrawItem> items(p->rec_alloc);
    if (p->draws.count() > 0U)
    {
        items.reserve(p->draws.count());
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            if (it.storage == nullptr) { continue; }
            g::IStorageBuffer* sb = ctx.buffer(p->storage_of[i]);
            if (sb == nullptr) { continue; }
            rg::RenderDrawItem ri{};
            ri.storage        = sb;
            ri.program        = p->program_is_instance ? nullptr : it.program;
            ri.texture        = it.texture;
            ri.vertex_count   = it.vertex_count;
            ri.indexed        = it.indexed;
            ri.index_count    = it.index_count;
            ri.instance_count = it.instance_count;
            ri.first_index    = it.first_index;
            ri.args           = it.args;
            ri.args_offset    = it.args_offset;
            items.push_back(ri);
        }
        draws.items = items.data();
        draws.count = static_cast<crd::u32>(items.size());
    }
    rg::PassPrograms programs;
    programs.raster = p->program;
    programs.kernel = p->kernel_program;
    rg::RecordContext rctx(payload, table, programs, diags, &draws);
    fn(payload, rctx, *enc);
    return true;
}

void record_pass(g::IFrameContext& ctx, void* user)
{
    auto*                p = static_cast<PassRec*>(user);
    const FramePassDesc& d = *p->desc;
    g::IRasterContext&   r = ctx.raster();
    r.compute_diag(8U);
    if (d.kind == FramePassKind::Compute) { r.compute_diag(9U); }
    // REN-36.3: `shadow_atlas[$index]` renders into ONE SLICE; a plain write renders into the whole image.
    // `image_layer(h, 0)` on a non-layered resource IS `image(h)`, so a for_each pass over a non-layered target
    // needs no special case — one code path serves both shapes.
    g::IRasterTarget* t = nullptr;
    if (p->depth_target.valid())   { t = ctx.image_with_depth(p->target, p->depth_target); }
    else if (p->indexed_target)    { t = ctx.image_layer(p->target, p->layer); }
    else                           { t = ctx.image(p->target); }
    // ⛔ REN-38-A9/A10: a COMPUTE-SHAPED pass writes buffers, not attachments, so requiring a render target
    // would have made every ray-tracing and GPU-driven-dispatch pass silently do nothing — the exact shape this
    // band keeps finding, one kind further along.
    const bool needs_target = d.kind != FramePassKind::Compute && d.kind != FramePassKind::RayTrace
                              && d.kind != FramePassKind::ComputeIndirect && d.kind != FramePassKind::Present
                              && d.kind != FramePassKind::RayTracePipeline;
    if (t == nullptr && needs_target) { return; }
    // ⛔ REN-38-A6: the PROGRAM guard is for the kinds that DRAW. A clear/copy/blit/resolve pass has no shader
    // by design, so requiring one here would have made every utility pass silently do nothing — the exact shape
    // this band keeps finding. Compute is excluded too: its program is `kernel_program`, not `program`.
    const bool needs_program = d.kind != FramePassKind::Clear && d.kind != FramePassKind::Copy
                               && d.kind != FramePassKind::Blit && d.kind != FramePassKind::Resolve
                               && d.kind != FramePassKind::Compute && d.kind != FramePassKind::Present
                               && d.kind != FramePassKind::RayTrace && d.kind != FramePassKind::ComputeIndirect
                               && d.kind != FramePassKind::RayTracePipeline && d.kind != FramePassKind::Custom;
    if (needs_program && p->program == nullptr) { return; }

    // ⭐ REN-38-B3: ZERO this pass's counters FIRST. Recorded here rather than as a separate pass so the reset is
    // ORDERED against the append by construction — nothing between them can observe the old value.
    for (crd::u32 ci = 0; ci < p->n_counters; ++ci)
    {
        g::IStorageBuffer* cb = ctx.buffer(p->counters[ci]);
        if (cb != nullptr) { r.fill_buffer(*cb, 0U, 4U, 0U); }
    }
    // ⭐ REN-38-B8: install this pass's sampler BEFORE any draw records. A pass that declared none leaves the
    // context on its engine default, which `frame_rec_new_pass` restored at the pass boundary.
    if (d.has_sampler) { r.set_sampler(d.sampler); }
    // ⭐ REN-38 audit: install this pass's DECLARED raster state — depth-write, depth bias, face cull, stencil —
    // under the exact same discipline: context state, reset to the historical defaults at the pass boundary,
    // so a pass that says nothing never inherits its neighbour's bias or stencil configuration.
    r.set_pass_state(d.state);
    switch (d.kind)
    {
    case FramePassKind::RasterDepthOnly:
    case FramePassKind::RasterGeometry:
        // ⭐ RAF-12: the scene.raster executor is the ONE recording path for the depth-only and colour-geometry
        // scene-draw shapes. `record_scene_via_executor` dispatches by `d.kind` internally, and the executor registry
        // is wired into every PassRec since the RAF-8 flip — so the inline verb fallbacks these cases carried are dead.
        (void)record_scene_via_executor(p, ctx, r, t);
        break;
    case FramePassKind::RasterMrt:
    {
        // ⭐ RAF-12: the scene.raster executor handles the SINGLE-colour MRT (the velocity prepass — colour+depth).
        if (record_scene_via_executor(p, ctx, r, t)) { break; }
        // ⛔⛔ RAF-12.2: the executor declines a TRUE multi-colour G-buffer (n_writes>1) — the encoder does not yet
        // bind N colour attachments, so this inline MRT path is the ONLY coverage. 80c0736 deleted it before that
        // coverage existed, so every cooked deferred G-buffer rendered NOTHING (REN-38-A4). RAF-12.4-F6 relocates
        // these storage-MRT verbs onto the encoder WITH the parity gate — until then the inline path stays.
        // ⭐ REN-38-A1b: N DECLARED WRITES => N COLOUR ATTACHMENTS. ⭐⭐ REN-41: an MRT pass that produces a depth
        // (the velocity prepass) delivers its FIRST colour target with that depth attached — `t` is exactly that
        // combined target (image_with_depth, resolved at the top).
        const g::ClearColor clear{d.clear_color[0], d.clear_color[1], d.clear_color[2], d.clear_color[3]};
        g::IRasterTarget*   rts[kMaxPassReads]{};
        crd::u32            nrt        = 0U;
        const bool          with_depth = p->depth_target.valid();
        for (crd::u32 i = 0; i < p->n_writes; ++i)
        {
            g::IRasterTarget* rt = (with_depth && i == 0U) ? t : ctx.image(p->writes_all[i]);
            // ⛔ A write that does not resolve ABORTS the pass rather than shifting every later attachment down a slot.
            if (rt == nullptr) { return; }
            rts[nrt++] = rt;
        }
        if (nrt == 0U) { return; }
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            if (it.storage == nullptr) { continue; }
            g::IStorageBuffer* sb = ctx.buffer(p->storage_of[i]);
            if (sb == nullptr) { continue; }
            g::IRasterProgram* prog = p->program;
            // ⭐⭐ REN-41: a velocity item binds its OWN per-group program (the velocity VS twin).
            if (!p->program_is_instance && it.program_velocity != nullptr) { prog = it.program_velocity; }
            else if (!p->program_is_instance && it.program != nullptr) { prog = it.program; }
            // A true multi-colour G-buffer is authored with plain vertex draws (the only shape any frame/test uses —
            // the indexed/indirect MRT verbs had no caller once the velocity prepass moved to the single-colour scene
            // executor, so they were retired). One vertex draw per item into all N colour attachments.
            r.draw_storage_mrt(static_cast<g::IRasterTarget* const*>(rts), nrt, *prog, clear, d.clear_depth, d.depth,
                               *sb, it.vertex_count);
        }
        break;
    }
    case FramePassKind::RasterFullscreen:
        (void)record_fullscreen_via_executor(p, ctx, r, t);
        break;
    case FramePassKind::Compute:
        (void)record_compute_via_executor(p, ctx, r);
        break;
    case FramePassKind::RasterVisbuffer:
        (void)record_visbuffer_via_executor(p, r, t);
        break;
    case FramePassKind::RasterComposite:
        (void)record_composite_via_executor(p, ctx, r, t);
        break;
    case FramePassKind::RayTrace:
        (void)record_raytrace_via_executor(p, ctx, r);
        break;
    case FramePassKind::RayTracePipeline:
        (void)record_raytrace_pipeline_via_executor(p, ctx, r);
        break;
    case FramePassKind::ComputeIndirect:
        (void)record_compute_via_executor(p, ctx, r);
        break;
    case FramePassKind::RasterMeshIndirect:
        (void)record_mesh_indirect_via_executor(p, ctx, r, t);
        break;
    case FramePassKind::RasterTess:
    case FramePassKind::RasterMesh:
        (void)record_amplify_via_executor(p, ctx, r, t);
        break;
    case FramePassKind::Clear:
    case FramePassKind::Copy:
    case FramePassKind::Blit:
    case FramePassKind::Resolve:
        (void)record_transfer_via_executor(p, ctx, r, t); // the transfer executor dispatches clear/copy/blit/resolve by kind
        break;
    case FramePassKind::Custom:
        // ⭐⭐ RAF-10: an APPLICATION-DEFINED pass — record through the executor named in `executor`, resolved in the
        // SAME table a builtin uses. A named-but-unregistered executor is reported by name (record_custom returns true).
        (void)record_custom_via_executor(p, ctx, r, t);
        break;
    case FramePassKind::Present:
    default:
        // ⭐ REN-38-A5: a present pass records NO commands. Presenting is acquire → blit → present on the
        // SURFACE's own submission, which the graph performs after its own submit — see `.present(surface)` in
        // the recorder below and the post-submit loop in each backend's `execute()`. The pass exists in the graph
        // so the dependency sort keeps it last and the barrier scheduler leaves its source in the transfer layout
        // the surface expects; it deliberately has no body.
        break;
    }
}

} // namespace

// ── REN-37.10: the RECORDER. Owns the per-recording storage the graph's user pointers refer to. ──
struct FrameRecorder::Impl
{
    crd::memory::IAllocator* alloc = nullptr;
    // ⛔ RESERVED TO ITS EXACT TOTAL up front, so no growth can move a block the graph already points at. Each
    // block is itself reserved exactly at record time, for the same reason one level down.
    crd::containers::Array<crd::containers::Array<PassRec>> blocks;
    crd::u32                                               used = 0U;
    // REN-38-B1: which side of the ping-pong pair is CURRENT. Advanced by `begin_frame()`, so a graph recorded
    // twice in one frame (the multi-viewport path) sees ONE parity — two viewports must not disagree about which
    // image is history.
    crd::u32                                               frame_parity = 0U;
    // ⭐⭐ RAF-8a: the render-graph executor registry, built ONCE — the migration adapter routes a migrated pass kind
    // here (see PassRec::records). Retired with the FramePassKind switch at RAF-12.
    crd::rendergraph::GraphExecutorTable                   records;

    explicit Impl(crd::memory::IAllocator* a) : alloc(a), blocks(a), records(a)
    {
        blocks.reserve(FrameRecorder::kMaxRecordingsPerFrame);
        for (crd::u32 i = 0; i < FrameRecorder::kMaxRecordingsPerFrame; ++i)
        {
            blocks.push_back(crd::containers::Array<PassRec>(a));
        }
        crd::renderasset::DiagnosticList d(a);
        crd::rendergraph::register_builtin_records(records, d);
    }
};

FrameRecorder::FrameRecorder(crd::memory::IAllocator* alloc) : m_impl(new Impl(alloc)) {}
FrameRecorder::~FrameRecorder() { delete m_impl; }
void FrameRecorder::begin_frame() noexcept
{
    m_impl->used = 0U;
    ++m_impl->frame_parity; // REN-38-B1: rotate the ping-pong pair, once per frame
}

bool FrameRecorder::register_pass_executor(crd::containers::StringView id, crd::rendergraph::PassRecordFn fn)
{
    // ⭐⭐ RAF-10: an app's custom executor joins the SAME table `register_builtin_records` filled — the id it registers
    // under is exactly what a `kind = "custom"` pass' `executor =` names, hashed the same way (`executor_type_id`), so
    // `record_custom_via_executor` resolves it with the identical `find()` a builtin uses. This is the extension seam:
    // no new FramePassKind, no engine edit — the id IS the mechanic. `register_record` refuses a duplicate (a builtin's
    // id, or a second registration of the same app id), which surfaces here as `false`.
    if (m_impl == nullptr || fn == nullptr)
    {
        return false;
    }
    crd::renderasset::DiagnosticList diags(m_impl->alloc);
    return m_impl->records.register_record(crd::renderpass::executor_type_id(id), fn, diags);
}

bool FrameRecorder::record(const FrameGraphDesc& desc, g::IFrameGraph& fgraph_ref, g::IRasterContext& raster,
                           IFrameGraphHost& host, FrameExecError* err, crd::containers::String* where)
{
    (void)raster; // the graph is the caller's; the raster context is reached through it
    g::IFrameGraph* fgraph = &fgraph_ref;
    const auto fail = [&](FrameExecError e, const crd::containers::String* name) {
        if (err != nullptr) { *err = e; }
        if (where != nullptr && name != nullptr) { *where = *name; }
        return false;
    };
    if (err != nullptr) { *err = FrameExecError::Ok; }

    // Capability tier FIRST — a graph that needs what the device lacks must never half-run (REN-35's rule).
    for (crd::usize i = 0; i < desc.requires_caps.size(); ++i)
    {
        const crd::containers::String& c = desc.requires_caps[i];
        if (!host.capability(crd::containers::StringView(c.c_str(), c.size())))
        {
            return fail(FrameExecError::UnsupportedCapability, &c);
        }
    }

    g::IRasterTarget* out_target = host.output();
    if (out_target == nullptr) { return fail(FrameExecError::NoOutput, nullptr); }
    // ⛔ The arena cap is CHECKED, not hoped: a 33rd recording would reuse a block the graph still points at.
    if (m_impl->used >= kMaxRecordingsPerFrame) { return fail(FrameExecError::BuildRejected, &desc.name); }
    crd::containers::Array<PassRec>& recs = m_impl->blocks[m_impl->used++];
    recs.clear();

    auto* alloc = desc.resources.allocator();

    // ── resources: every declared transient, in declaration order ──
    // ⛔ REN-38-A2: TRANSIENT BUFFERS were declared by the schema and NEVER CREATED — the loop below only ever
    // built images, so a `kind = "transient_buffer"` resource validated, cooked and then did not exist. A compute
    // pass has nothing to bind without them, which is half of why `FramePassKind::Compute` could not work.
    // Parallel to `images` and indexed the same way, so `desc.resources[i]` maps to both.
    crd::containers::Array<g::FgBuffer> buffers(alloc);
    crd::containers::Array<g::FgImage> images(alloc);
    // REN-38-B1: for a ping-pong resource, the PREVIOUS frame's image. Index-parallel with `images`, which holds
    // the CURRENT one — so a read and a write of the same authored name resolve to different handles.
    crd::containers::Array<g::FgImage> pingpong(alloc);
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        // ⛔ REN-38-B4: an ACCELERATION STRUCTURE is EXTERNAL — the host built it. Creating nothing here keeps
        // `images`/`buffers` index-parallel with `desc.resources`, which every later lookup relies on.
        if (r.kind == FrameResourceKind::AccelerationStructure)
        {
            buffers.push_back(g::FgBuffer{});
            images.push_back(g::FgImage{});
            pingpong.push_back(g::FgImage{});
            continue;
        }
        // ⭐ REN-38-B5: an EXTERNAL TEXTURE is the host's — IMPORTED read-only. The graph tracks it for ORDERING
        // only; there is no barrier to derive because nothing in the frame writes it.
        if (r.kind == FrameResourceKind::ExternalTexture)
        {
            g::ITexture* tx = host.texture(crd::containers::StringView(r.name.c_str(), r.name.size()));
            if (tx == nullptr) { return fail(FrameExecError::UnresolvedResource, &r.name); }
            images.push_back(fgraph->import_texture(*tx));
            buffers.push_back(g::FgBuffer{});
            pingpong.push_back(g::FgImage{});
            continue;
        }
        // REN-38-B3: an EXTERNAL buffer is the host's — IMPORTED, not created, so the graph orders and barriers it
        // exactly like a transient while its storage outlives the frame.
        if (r.kind == FrameResourceKind::ExternalBuffer)
        {
            g::IStorageBuffer* sb = host.storage_buffer(crd::containers::StringView(r.name.c_str(), r.name.size()));
            if (sb == nullptr) { return fail(FrameExecError::UnresolvedResource, &r.name); }
            buffers.push_back(fgraph->import_storage(*sb));
            images.push_back(g::FgImage{});
            pingpong.push_back(g::FgImage{});
            continue;
        }
        // ── ⭐ REN-38-B1: PERSISTENT and PING-PONG images. ──
        // ⛔ The KEY is a hash of the resource NAME, not its index. An index would shift the moment someone added
        // a resource above it in the file — and a persistent image is looked up BY KEY across frames, so a shifted
        // key silently swaps two histories (TAA reading the auto-exposure buffer) or discards one.
        if (r.kind == FrameResourceKind::PersistentImage || r.kind == FrameResourceKind::PingPongImage)
        {
            g::FgImageDesc pid{};
            // ⭐⭐⭐ REN-41: a `resizable` persistent (scale, no absolute size) FOLLOWS THE OUTPUT — sized from the
            // target every build exactly like a transient. On a resize the desc size changes and the device's
            // `create_persistent_image` destroys+recreates it (history discarded for one frame, reconverged in a
            // few) — which is precisely the TAA-history contract. An absolute size still wins when the author gives one.
            pid.width   = r.width != 0U ? r.width : static_cast<crd::u32>(static_cast<float>(out_target->width()) * r.scale);
            pid.height  = r.height != 0U ? r.height : static_cast<crd::u32>(static_cast<float>(out_target->height()) * r.scale);
            pid.format  = r.format;
            pid.samples = r.samples;
            pid.sampled = r.sampled;
            pid.storage = r.storage;
            pid.layers  = r.layers;
            pid.kind    = r.kind_2d;
            pid.depth   = r.depth;
            pid.mips    = r.mips;
            const crd::u32 base = name_key(r.name);
            if (r.kind == FrameResourceKind::PersistentImage)
            {
                const g::FgImage h = fgraph->create_persistent_image(base, pid);
                if (!h.valid()) { return fail(FrameExecError::TransientFailed, &r.name); }
                images.push_back(h);
                buffers.push_back(g::FgBuffer{});
                pingpong.push_back(g::FgImage{}); // not a pair
                continue;
            }
            // ⭐ A PING-PONG resource is TWO persistent images under two keys, and the pair ROTATES BY FRAME
            // PARITY. ⛔ The author never sees the parity bit — which is precisely why they cannot get it wrong:
            // a READ resolves to the previous frame's image and a WRITE to this frame's, and that IS what a
            // history buffer means. The classic one-frame-stale bug has no place left to live.
            const bool     odd  = (m_impl->frame_parity & 1U) != 0U;
            const g::FgImage a  = fgraph->create_persistent_image(base ^ 0x9E3779B9U, pid);
            const g::FgImage b2 = fgraph->create_persistent_image(base ^ 0x85EBCA6BU, pid);
            if (!a.valid() || !b2.valid()) { return fail(FrameExecError::TransientFailed, &r.name); }
            images.push_back(odd ? b2 : a);   // CURR — what a write lands in
            pingpong.push_back(odd ? a : b2); // PREV — what a read comes from
            buffers.push_back(g::FgBuffer{});
            continue;
        }
        // REN-38-B3: an `indirect_args` resource is a transient BUFFER — the device already declares the indirect
        // usage on every one of them, so the two kinds differ in INTENT and in what the cooker will accept, not
        // in backing. Handling them together is what makes "a cull pass writes args" work with no special case.
        if (r.kind == FrameResourceKind::TransientBuffer || r.kind == FrameResourceKind::IndirectArgs
            || r.kind == FrameResourceKind::StructuredBuffer || r.kind == FrameResourceKind::CounterBuffer)
        {
            const g::FgBuffer bh = fgraph->create_transient_buffer(r.size_bytes);
            if (!bh.valid()) { return fail(FrameExecError::TransientFailed, &r.name); }
            buffers.push_back(bh);
            images.push_back(g::FgImage{}); // keep the arrays index-parallel
            pingpong.push_back(g::FgImage{});
            continue;
        }
        buffers.push_back(g::FgBuffer{});
        g::FgImageDesc           id{};
        // `scale` is relative to the OUTPUT target; an absolute width/height wins when given.
        id.width   = r.width != 0U ? r.width : static_cast<crd::u32>(static_cast<float>(out_target->width()) * r.scale);
        id.height  = r.height != 0U ? r.height : static_cast<crd::u32>(static_cast<float>(out_target->height()) * r.scale);
        id.format  = r.format;
        id.samples = r.samples;
        id.sampled      = r.sampled;
        id.depth_buffer = r.depth_buffer; // 38-G1: an intermediate render target's depth attachment
        id.storage = r.storage;
        id.layers  = r.layers; // REN-3.2: >1 ⇒ the 2D-array cascade/cube/stereo atlas
        // ⛔⛔ REN-38-B2 + B6: THE SHAPE AND THE ALIAS PIN REACH THE TRANSIENT PATH TOO. They were wired into the
        // PERSISTENT branch above and NOT here, so an authored `dimension = "cube"` TRANSIENT parsed, validated
        // and was created as an ordinary 2-D image — which a `samplerCube` binding cannot use and which no
        // validation layer complains about. ⛔ B2's own gate missed it because it called
        // `create_transient_image` DIRECTLY with a hand-built desc instead of going through the asset: a gate
        // that bypasses the layer it is meant to prove is not a gate for that layer.
        id.kind     = r.kind_2d;
        id.depth    = r.depth;
        id.mips     = r.mips;
        id.no_alias = r.no_alias;
        const g::FgImage h = fgraph->create_transient_image(id);
        if (!h.valid()) { return fail(FrameExecError::TransientFailed, &r.name); }
        images.push_back(h);
        pingpong.push_back(g::FgImage{});
    }
    // REN-38-B6: the graph-level transient budget, installed BEFORE `build()` — which is where it is enforced,
    // because the post-aliasing footprint is a DEVICE answer (alignment and memory-type rules differ per adapter),
    // not something the asset could compute for itself.
    fgraph->set_memory_budget(desc.memory_budget_bytes);
    const g::FgImage out_handle = fgraph->import_target(*out_target);

    // ⭐ REN-38-B1: `for_read` is what makes ping-pong work with no new syntax. Everything else ignores it.
    const auto resolve_image = [&](const crd::containers::String& n, g::FgImage& h, bool& is_depth,
                                   bool for_read = false, bool* is_array = nullptr) -> bool {
        if (name_is(n, "@output")) { h = out_handle; is_depth = false; return true; }
        for (crd::usize i = 0; i < desc.resources.size(); ++i)
        {
            if (desc.resources[i].name.size() == n.size()
                && std::memcmp(desc.resources[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                h = images[i];
                // ⭐⭐ REN-40-D: LAYERED-ness is reported alongside depth-ness, because ATLAS ROUTING keys on it.
                // The routing below used depth-ness as a proxy for "this read is the frame's atlas", which held
                // exactly as long as the only atlas was a depth atlas — a MOMENT atlas is a colour array, and
                // under the depth proxy it would be routed as a MATERIAL map: any draw carrying its own albedo
                // would then silently drop its shadows, the REN-37.10 regression in a new costume.
                if (is_array != nullptr) { *is_array = desc.resources[i].layers > 1U; }
                // ⭐ REN-38-B1: a READ of a PING-PONG resource resolves to the PREVIOUS frame's image, a write to
                // this frame's. That is the whole mechanism, and it needs no syntax the author can hold wrong.
                if (for_read && desc.resources[i].kind == FrameResourceKind::PingPongImage && pingpong[i].valid())
                {
                    h = pingpong[i];
                }
                // ⛔ REN-38-B7: ASK THE PREDICATE, never compare to one format. `is_depth` picks the COMPARISON
                // sampler for a pass that reads this resource, so a D24S8 or D32FloatS8 shadow map compared to
                // `D32Float` alone would come back false and be sampled with a FILTERING sampler — a shadow term
                // that is smooth, plausible and wrong.
                is_depth = g::fg_format_has_depth(desc.resources[i].format);
                return true;
            }
        }
        return false;
    };

    // REN-36.3-b: resolve a pass's draw-list NAME to the graph's declared QUERY, then hand the whole thing to
    // the host. Falls back to a synthesized name-only desc if the graph never declared the list — the cooker
    // rejects that (`MissingDrawList`), so it is unreachable from a validated graph, but the executor must not
    // depend on that to stay memory-safe.
    const auto resolve_query = [&](const crd::containers::String& n, DrawListBinding& out,
                                   crd::u32 instance) -> bool {
        for (crd::usize i = 0; i < desc.draw_lists.size(); ++i)
        {
            if (desc.draw_lists[i].name.size() == n.size()
                && std::memcmp(desc.draw_lists[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                // 38-G1 perf: the EXPANSION INDEX reaches the host, so a cascade pass can be answered with a
                // list culled for that cascade rather than the camera's.
                return host.draw_list_query(desc.draw_lists[i], out, instance);
            }
        }
        return host.draw_list(crd::containers::StringView(n.c_str(), n.size()), out);
    };

    // ── REN-36.3: MULTI-VIEW EXPANSION, before anything else touches the pass list. ──
    // A `for_each` pass becomes N ORDINARY passes here, so lifetime analysis, aliasing and the barrier schedule
    // downstream see nothing special — the user-locked design ("expanded AT BUILD so aliasing/barriers see
    // ordinary passes"). The expansion table is built FIRST and `recs` reserved to its exact total, because the
    // graph stores raw `&recs[i]` user pointers: a later push_back that reallocated would dangle every one.
    struct Instance
    {
        crd::usize pass  = 0;
        crd::u32   index = 0;
        crd::u32   count = 1;
    };
    crd::containers::Array<Instance> plan(alloc);
    for (crd::usize pi = 0; pi < desc.passes.size(); ++pi)
    {
        const FramePassDesc& d = desc.passes[pi];
        if (d.for_each == FrameForEach::None)
        {
            plan.push_back(Instance{pi, 0U, 1U});
            continue;
        }
        const crd::u32 n = host.for_each_count(d.for_each, d.for_each_arg);
        // ⛔ 0 is a REPORTED failure, never a silent skip — a shadow graph that renders no cascades is
        // indistinguishable from a scene that has no shadows.
        if (n == 0U) { return fail(FrameExecError::UnresolvedForEach, &d.name); }
        for (crd::u32 i = 0; i < n; ++i) { plan.push_back(Instance{pi, i, n}); }
    }

    // ── passes ──
    recs.reserve(plan.size()); // ⛔ exact, up front — see the dangling-pointer note above
    // ⭐⭐ REN-39 (the gizmo fix): the host's overlay is INSERTED after the LAST geometry pass, onto that
    // pass's own target — live depth, pre-tonemap. Passes execute in DECLARATION order, so an overlay merely
    // APPENDED after a frame with a post chain lands after the display transform, depth-testing against the
    // output's never-written depth (the exact "gizmo looks weird" the sandbox showed under the AgX frame).
    crd::i64 last_geom_ii = -1;
    for (crd::usize gi = 0; gi < plan.size(); ++gi)
    {
        if (desc.passes[plan[gi].pass].kind == FramePassKind::RasterGeometry)
        {
            last_geom_ii = static_cast<crd::i64>(gi);
        }
    }
    for (crd::usize ii = 0; ii < plan.size(); ++ii)
    {
        const FramePassDesc& d = desc.passes[plan[ii].pass];
        PassRec              rec{};
        rec.desc  = &d;
        rec.rec_alloc = m_impl->alloc;      // RAF-8a: the migration adapter's scratch allocator + executor registry
        rec.records   = &m_impl->records;
        rec.layer = plan[ii].index;

        // REN-40-E: a cached for_each instance skips draw-list resolution and program checks —
        // a cached instance has no draws and no program, and those checks would fail. Resolved
        // FIRST so the guards below can test load_override.
        if (d.for_each != FrameForEach::None && host.for_each_load(d.for_each, plan[ii].index))
        {
            rec.load_override = true;
        }

        DrawListBinding bind{};
        if (!d.draw_list.empty() && !rec.load_override)
        {
            // ⛔⛔ ONLY an EXPANDED pass carries its instance to the host. `plan[ii].index` is 0 for BOTH
            // "cascade 0" and "not expanded at all" — and handing 0 to the host made it stamp CASCADE 0's
            // vertex counts onto the FORWARD pass, truncating the whole scene draw (textures gone, shadows
            // wrong, geometry missing — the exact live-app symptom). kNoInstance = "this pass is not one of N".
            if (!resolve_query(d.draw_list, bind,
                               d.for_each != FrameForEach::None ? plan[ii].index : 0xFFFFFFFFU))
            {
                return fail(FrameExecError::UnresolvedDrawList, &d.draw_list);
            }
            rec.draws        = bind;
            rec.program      = bind.at(0).program;
            rec.vertex_count = bind.at(0).vertex_count;
        }
        if (!d.shader.empty())
        {
            rec.program = host.program(crd::containers::StringView(d.shader.c_str(), d.shader.size()));
            // a missing program must FAIL, never render something plausible
            if (rec.program == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.shader); }
        }
        // ⛔⛔ REN-38 llvmpipe campaign: a pass that DRAWS with no resolved program used to fall through to
        // `record_pass`, whose program guard returned SILENTLY — a black frame with draws reported and no
        // error anywhere (the exact class this band keeps killing, one layer further in). A draw-list pass
        // whose host binding carries no program is now the SAME named failure as a missing shader.
        {
            const bool draws_geometry = d.kind == FramePassKind::RasterGeometry
                                        || d.kind == FramePassKind::RasterMrt
                                        || d.kind == FramePassKind::RasterTess || d.kind == FramePassKind::RasterMesh
                                        || d.kind == FramePassKind::RasterDepthOnly;
            if (draws_geometry && rec.program == nullptr && !rec.load_override)
            {
                return fail(FrameExecError::UnresolvedProgram, &d.name);
            }
        }
        // ⭐ REN-38-B4: resolve every acceleration structure this pass READS, through the host. A raytrace pass
        // takes the first — the cooker already proved there is one.
        if (d.kind == FramePassKind::RayTrace || d.kind == FramePassKind::RayTracePipeline)
        {
            for (crd::usize rr = 0; rr < d.reads.size() && rec.accel == nullptr; ++rr)
            {
                for (crd::usize bi = 0; bi < desc.resources.size(); ++bi)
                {
                    if (desc.resources[bi].kind != FrameResourceKind::AccelerationStructure) { continue; }
                    if (!name_is(desc.resources[bi].name, d.reads[rr].name.c_str())) { continue; }
                    rec.accel = host.acceleration_structure(
                        crd::containers::StringView(d.reads[rr].name.c_str(), d.reads[rr].name.size()));
                    // ⛔ A NAMED failure. A raytrace pass that traversed nothing would render every ray as a miss
                    // — a black image indistinguishable from a scene with no geometry.
                    if (rec.accel == nullptr) { return fail(FrameExecError::UnresolvedAccel, &d.reads[rr].name); }
                    break;
                }
            }
        }
        // REN-38-A10: `args_offset` is a PARAMETER (a byte offset into the args buffer, so one buffer can hold
        // several dispatches' arguments). The buffer itself is resolved with the reads, below.
        for (crd::usize pi2 = 0; pi2 < d.params.size(); ++pi2)
        {
            if (name_is(d.params[pi2].name, "args_offset"))
            {
                rec.args_offset = static_cast<crd::u64>(d.params[pi2].v[0] > 0.0 ? d.params[pi2].v[0] : 0.0);
            }
        }
        // REN-38-A11: the background id a visibility buffer clears to — a PARAMETER, like every other count here.
        for (crd::usize pi3 = 0; pi3 < d.params.size(); ++pi3)
        {
            if (name_is(d.params[pi3].name, "clear_id"))
            {
                rec.clear_id = static_cast<crd::u32>(d.params[pi3].v[0] > 0.0 ? d.params[pi3].v[0] : 0.0);
            }
        }
        // REN-38-A7/A8: the amplification count, when the pass has no draw list. Same rule as the compute grid:
        // a dispatch size is a PARAMETER, not topology.
        if (d.kind == FramePassKind::RasterTess || d.kind == FramePassKind::RasterMesh)
        {
            for (crd::usize pi2 = 0; pi2 < d.params.size(); ++pi2)
            {
                const FrameParam& prm = d.params[pi2];
                if (name_is(prm.name, "patches") || name_is(prm.name, "groups"))
                {
                    rec.amplify_count = static_cast<crd::u32>(prm.v[0] > 0.0 ? prm.v[0] : 0.0);
                }
            }
        }
        // REN-38-A2: a COMPUTE pass names a KERNEL, and its grid comes from declared params. A missing kernel
        // FAILS by name — the whole point of this row is that a compute pass can no longer do nothing quietly.
        // REN-38-A16: the three RT-pipeline programs resolve through the SAME kernel seam — each is a single
        // CKIR stage, not a linked pair, which is exactly what `host.kernel` returns.
        if (d.kind == FramePassKind::RayTracePipeline)
        {
            rec.rt_raygen = host.kernel(crd::containers::StringView(d.raygen.c_str(), d.raygen.size()));
            if (rec.rt_raygen == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.raygen); }
            rec.rt_miss = host.kernel(crd::containers::StringView(d.miss.c_str(), d.miss.size()));
            if (rec.rt_miss == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.miss); }
            rec.rt_chit = host.kernel(crd::containers::StringView(d.closest_hit.c_str(), d.closest_hit.size()));
            if (rec.rt_chit == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.closest_hit); }
            // REN-38 audit: the any-hit is OPTIONAL, but a NAMED one that does not resolve FAILS — a pipeline
            // silently built without its any-hit traces every transparent texel as solid, which renders.
            if (d.any_hit.size() > 0U)
            {
                rec.rt_anyhit = host.kernel(crd::containers::StringView(d.any_hit.c_str(), d.any_hit.size()));
                if (rec.rt_anyhit == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.any_hit); }
            }
            // REN-38-F13: the last two SBT roles - optional, but a NAMED one that does not resolve FAILS.
            if (d.intersection.size() > 0U)
            {
                rec.rt_isect = host.kernel(crd::containers::StringView(d.intersection.c_str(), d.intersection.size()));
                if (rec.rt_isect == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.intersection); }
            }
            if (d.callable.size() > 0U)
            {
                rec.rt_callable = host.kernel(crd::containers::StringView(d.callable.c_str(), d.callable.size()));
                if (rec.rt_callable == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.callable); }
            }
        }
        // ⛔ REN-38-A16: the LAUNCH GRID is read for the RT-pipeline kind too. It was not, so `groups` stayed
        // {1,1,1} and a `raytrace.pipeline` pass fired exactly ONE ray however many the asset declared — one
        // correct pixel and an untouched buffer everywhere else, which reads as a traversal failure.
        if (d.kind == FramePassKind::Compute || d.kind == FramePassKind::RayTrace
            || d.kind == FramePassKind::ComputeIndirect || d.kind == FramePassKind::RayTracePipeline)
        {
            for (crd::usize pg = 0; pg < d.params.size(); ++pg)
            {
                const FrameParam& prm    = d.params[pg];
                const auto        as_u32 = [&]() { return static_cast<crd::u32>(prm.v[0] > 0.0 ? prm.v[0] : 1.0); };
                if (name_is(prm.name, "groups_x")) { rec.groups[0] = as_u32(); }
                else if (name_is(prm.name, "groups_y")) { rec.groups[1] = as_u32(); }
                else if (name_is(prm.name, "groups_z")) { rec.groups[2] = as_u32(); }
            }
        }
        if (d.kind == FramePassKind::Compute || d.kind == FramePassKind::RayTrace
            || d.kind == FramePassKind::ComputeIndirect)
        {
            rec.kernel_program = host.kernel(crd::containers::StringView(d.kernel.c_str(), d.kernel.size()));
            if (rec.kernel_program == nullptr) { return fail(FrameExecError::UnresolvedProgram, &d.kernel); }
            for (crd::usize pi2 = 0; pi2 < d.params.size(); ++pi2)
            {
                const FrameParam& prm = d.params[pi2];
                const auto        as_u32 = [&]() { return static_cast<crd::u32>(prm.v[0] > 0.0 ? prm.v[0] : 1.0); };
                if (name_is(prm.name, "groups_x")) { rec.groups[0] = as_u32(); }
                else if (name_is(prm.name, "groups_y")) { rec.groups[1] = as_u32(); }
                else if (name_is(prm.name, "groups_z")) { rec.groups[2] = as_u32(); }
            }
        }
        // A per-instance program is OPTIONAL: null means "use the pass's own", the common case where cascades
        // share a shader and differ only by a pass-frequency uniform.
        if (d.for_each != FrameForEach::None)
        {
            g::IRasterProgram* ip =
                host.instance_program(crd::containers::StringView(d.name.c_str(), d.name.size()), plan[ii].index);
            if (ip != nullptr)
            {
                rec.program             = ip;
                rec.program_is_instance = true;
            }
        }
        recs.push_back(rec);
    }

    // ── ⭐⭐ REN-41: WHERE THE OVERLAY GOES, now that TAA can sit between the scene and the display. ────────────
    // The gizmo/grid overlay composites onto the image the FINAL (display) pass READS — its INPUT — right BEFORE
    // that display pass runs. Without TAA that input is the scene image (the historical case). WITH TAA it is the
    // RESOLVED image (`scene_taa`), and weaving BEFORE the display pass places the overlay AFTER both the resolve
    // AND the history-store — so its thin grid lines are neither smeared by TAA nor leaked into next frame's
    // history (both of which "the grid is blurred" would otherwise show). Fallback (a display pass that writes
    // @output directly, no sampled input): the historical weave AFTER the last geometry pass.
    // ⛔ Resolve this from the DESC (pass names + resolve_image), NOT from `recs` — recs[].target/.sampled are
    // filled INSIDE the execute loop below, so they are empty here. The display pass is the one whose writes
    // include @output and which SAMPLES a scene image (a post/tonemap pass); its first read is the overlay canvas.
    crd::i64   overlay_before_ii = -1;           // weave BEFORE this pass (the display pass)
    const crd::i64 overlay_after_ii = last_geom_ii; // else weave AFTER this one (historical fallback)
    g::FgImage overlay_target{};
    for (crd::usize pp = 0; pp < plan.size(); ++pp)
    {
        const FramePassDesc& dp = desc.passes[plan[pp].pass];
        bool writes_output = false;
        for (crd::usize w = 0; w < dp.writes.size(); ++w)
        {
            if (name_is(dp.writes[w].name, "@output")) { writes_output = true; break; }
        }
        if (!writes_output || dp.reads.size() == 0U) { continue; }
        g::FgImage h;
        bool       is_depth = false;
        if (resolve_image(dp.reads[0].name, h, is_depth, /*for_read=*/true, nullptr))
        {
            overlay_before_ii = static_cast<crd::i64>(pp);
            overlay_target    = h;
        }
        break;
    }

    const auto weave_overlay = [&]() {
        g::FgExecuteFn ov_fn   = nullptr;
        void*          ov_user = nullptr;
        if (overlay_target.valid() && host.overlay_pass(&ov_fn, &ov_user, overlay_target) && ov_fn != nullptr)
        {
            fgraph->add_pass("overlay").read_writes(overlay_target).execute(ov_fn, ov_user);
        }
    };
    for (crd::usize ii = 0; ii < plan.size(); ++ii)
    {
        const FramePassDesc& d   = desc.passes[plan[ii].pass];
        PassRec&             rec = recs[ii];
        // ⛔⛔ The overlay pass must be ADDED TO THE GRAPH BEFORE the display pass's builder is created — this
        // graph orders passes by add_pass CALL ORDER, so weaving after the display pass's builder existed put the
        // overlay AFTER the display read (and nothing showed). Weave at the TOP of the display pass's iteration.
        if (overlay_before_ii >= 0 && static_cast<crd::i64>(ii) == overlay_before_ii) { weave_overlay(); }

        bool                        dummy_depth = false;
        // ⛔ The DEVICE pass kind is derived from the AUTHORED one, never assumed. It drives queue placement and
        // the barrier scheduler's layout choice, so a present pass recorded as Raster would have its source
        // transitioned to SHADER_READ_ONLY and the surface would blit from the wrong layout.
        g::FgPassKind dev_kind = g::FgPassKind::Raster;
        // REN-38-A9/A10: a ray-tracing pass and an indirect DISPATCH are compute work exactly as an authored
        // compute pass is; an indirect MESH draw is raster work that merely takes its count from a buffer.
        if (d.kind == FramePassKind::Compute || d.kind == FramePassKind::RayTrace
            || d.kind == FramePassKind::ComputeIndirect || d.kind == FramePassKind::RayTracePipeline)
        {
            dev_kind = g::FgPassKind::Compute;
        }
        else if (d.kind == FramePassKind::Present) { dev_kind = g::FgPassKind::Present; }
        // ⛔ REN-38-A6: copy/blit/resolve are TRANSFER passes so the barrier scheduler picks TRANSFER_SRC/DST.
        // A CLEAR is NOT: it is `LOAD_OP_CLEAR` on an attachment (`ClearRenderTargetView` on DX12), which needs
        // the ordinary colour-attachment layout — classifying it as transfer would clear an image the hardware
        // was told to treat as a copy destination.
        else if (d.kind == FramePassKind::Copy || d.kind == FramePassKind::Blit
                 || d.kind == FramePassKind::Resolve)
        {
            dev_kind = g::FgPassKind::Transfer;
        }
        g::IFramePassBuilder&       pb          = fgraph->add_pass(d.name.c_str(), dev_kind);
        // the buffer handles THIS pass declares as writes — see the draw-list note below
        g::FgBuffer write_bufs[kMaxPassReads]{};
        crd::u32    n_write_bufs = 0U;
        // REN-38-A14: pass the asset's QUEUE REQUEST through. The graph decides whether it can honour it and
        // reports the answer in `last_async_pass_count()` — the executor never claims it on the graph's behalf.
        if (d.queue == FrameQueue::Async) { pb.queue(g::FgQueue::Async); }
        bool first_write = true;
        for (crd::usize w = 0; w < d.writes.size(); ++w)
        {
            g::FgImage h{};
            bool w_buffer = false;
            for (crd::usize bi = 0; bi < desc.resources.size(); ++bi)
            {
                const FrameResourceKind wk = desc.resources[bi].kind;
                if (wk != FrameResourceKind::TransientBuffer && wk != FrameResourceKind::IndirectArgs
                    && wk != FrameResourceKind::ExternalBuffer && wk != FrameResourceKind::StructuredBuffer
                    && wk != FrameResourceKind::CounterBuffer)
                {
                    continue;
                }
                if (desc.resources[bi].name.size() != d.writes[w].name.size()
                    || std::memcmp(desc.resources[bi].name.c_str(), d.writes[w].name.c_str(),
                                   d.writes[w].name.size()) != 0)
                {
                    continue;
                }
                pb.writes(buffers[bi]);
                if (n_write_bufs < kMaxPassReads) { write_bufs[n_write_bufs++] = buffers[bi]; }
                // REN-38-B3: a pass that WRITES a counter buffer is the pass that appends into it, so it is the
                // pass whose reset must precede. Collected here, issued at the top of the body.
                if (wk == FrameResourceKind::CounterBuffer && rec.n_counters < kMaxPassReads)
                {
                    rec.counters[rec.n_counters++] = buffers[bi];
                }
                w_buffer = true;
                break;
            }
            if (w_buffer) { continue; }
            if (!resolve_image(d.writes[w].name, h, dummy_depth)) { return fail(FrameExecError::UnresolvedResource, &d.writes[w].name); }
            pb.writes(h);
            // ── ⭐⭐ REN-41: a DEPTH-format write on an MRT pass is the DEPTH ATTACHMENT, not an extra colour RTV. ──
            // The velocity prepass writes `["velocity", "scene_depth"]`: velocity is the one colour target and
            // scene_depth is the depth it PRODUCES. It stays a graph WRITE (`pb.writes` above — so hzb_build and
            // the forward pass, which read scene_depth, order after this prepass exactly as they did when the
            // depth-only prepass wrote it) but routes to `rec.depth_target` so `image_with_depth` binds it as
            // depth. ⛔ Gated to RasterMrt: a `raster.depth_only` pass's SOLE depth write is its PRIMARY target
            // (rec.target) and `t = image(target)` must keep that shape — re-routing it would leave it targetless.
            if (dummy_depth && d.kind == FramePassKind::RasterMrt)
            {
                rec.depth_target = h;
                continue;
            }
            if (rec.n_writes < kMaxPassReads) { rec.writes_all[rec.n_writes++] = h; }
            if (first_write)
            {
                rec.target = h;
                // the SUBSCRIPT decides slice-vs-whole-image; the cooker already proved `[$index]` only appears
                // on a layered resource inside a for_each pass, so no runtime re-validation is needed here
                rec.indexed_target = d.writes[w].indexed;
                first_write        = false;
            }
        }
        // ── ⭐ REN-40-G3: SHARED DEPTH — resolve the named depth image and register it as a READ. ──
        // ⛔ The forward pass LOADS the prepass depth and continues depth-testing against it, but no later pass
        // reads the updated depth values — the writes are self-contained. Declaring it as a graph WRITE would
        // create a backward edge to any earlier pass that reads the same image (hzb_build), causing a CYCLE:
        //   forward(writes scene_depth) → hzb_build(reads scene_depth) → occlusion_cull → forward.
        // The execute function transitions the image to DEPTH_ATTACHMENT via rec.depth_target regardless.
        if (d.shared_depth.size() > 0U)
        {
            g::FgImage dh{};
            bool       d_depth = false;
            if (!resolve_image(d.shared_depth, dh, d_depth))
            {
                return fail(FrameExecError::UnresolvedResource, &d.shared_depth);
            }
            pb.reads_depth(dh);
            rec.depth_target = dh;
        }
        bool first_read = true;
        for (crd::usize r = 0; r < d.reads.size(); ++r)
        {
            g::FgImage h{};
            bool       is_depth = false;
            // REN-38-A2: a BUFFER read is a graph-tracked read too — that is what orders a compute pass ahead of
            // whoever consumes its output, and it is why the kernel never needs an explicit barrier from the author.
            bool was_buffer = false;
            bool was_accel   = false;
            for (crd::usize bi = 0; bi < desc.resources.size(); ++bi)
            {
                const FrameResourceKind rk = desc.resources[bi].kind;
                if (rk != FrameResourceKind::TransientBuffer && rk != FrameResourceKind::IndirectArgs
                    && rk != FrameResourceKind::ExternalBuffer && rk != FrameResourceKind::AccelerationStructure
                    && rk != FrameResourceKind::StructuredBuffer && rk != FrameResourceKind::CounterBuffer)
                {
                    continue;
                }
                if (desc.resources[bi].name.size() != d.reads[r].name.size()
                    || std::memcmp(desc.resources[bi].name.c_str(), d.reads[r].name.c_str(),
                                   d.reads[r].name.size()) != 0)
                {
                    continue;
                }
                // ⛔ REN-38-B4: an ACCELERATION STRUCTURE is NOT a graph-tracked resource. It is external and
                // read-only for the whole frame, so there is no hazard to order and no barrier to derive —
                // declaring it as a read would ask the graph to schedule against a node it does not own.
                if (rk == FrameResourceKind::AccelerationStructure) { was_accel = true; break; }
                pb.reads(buffers[bi]);
                // REN-38-A10: remember WHICH buffer holds the arguments. It is a graph-tracked read like any
                // other, which is exactly what orders this pass after the cull pass that wrote it.
                if (rk == FrameResourceKind::IndirectArgs) { rec.args_buf = buffers[bi]; }
                // ⭐⭐ REN-41 (TAA): a non-args buffer read on a fullscreen pass is its CONSTANTS buffer. Harmless
                // to record for any kind (only the RasterFullscreen path binds it).
                else { rec.fs_constants = buffers[bi]; }
                was_buffer = true;
                break;
            }
            if (was_accel) { continue; }
            if (was_buffer) { continue; }
            bool is_array = false;
            if (resolve_image(d.reads[r].name, h, is_depth, /*for_read=*/true, &is_array))
            {
                pb.reads(h);
                if (rec.n_sampled < kMaxPassReads) { rec.sampled[rec.n_sampled++] = h; }
                if (first_read)
                {
                    rec.sampled_is_depth = is_depth && !d.depth_as_float;
                    rec.sampled_is_array = is_array;
                    first_read           = false;
                }
            }
            else { return fail(FrameExecError::UnresolvedResource, &d.reads[r].name); }
        }
        // The draw list's vertex-pull buffer is a graph-tracked READ, so the graph orders + barriers it like any
        // other resource (this is why a pass never has to think about upload/consume hazards).
        // ⛔ EVERY draw's vertex-pull buffer is a graph-tracked READ, not just the first one. Importing only the
        // first left the rest untracked: no ordering, no barrier, and an upload could race the draw that reads it.
        for (crd::u32 di = 0; di < rec.draws.count(); ++di)
        {
            const DrawItem it = rec.draws.at(di);
            if (it.storage == nullptr) { continue; }
            rec.storage_of[di] = fgraph->import_storage(*it.storage);
            if (d.untracked_storage) { if (di == 0U) { rec.storage = rec.storage_of[0]; } continue; }
            // ⛔⛔ NOT IF THIS PASS ALREADY DECLARED IT A WRITE. A GPU-driven cull pass walks this same draw list
            // to find the buffers it COMPACTS INTO — `writes = ["instances"]` — and adding a read of the very
            // same handle makes the pass both a writer and a reader of it. Two such passes then each depend on
            // the other's write and the device graph is a CYCLE: `build()` returns false, NOTHING is recorded,
            // and (before the report one layer up) the canvas kept its previous contents — a plausible frame
            // missing exactly the passes that mattered. The write declaration already carries the ordering and
            // the barrier; the read adds nothing but the cycle.
            bool already_written = false;
            for (crd::u32 wb = 0; wb < n_write_bufs; ++wb)
            {
                if (write_bufs[wb] == rec.storage_of[di]) { already_written = true; break; }
            }
            if (!already_written) { pb.reads(rec.storage_of[di]); }
            if (di == 0U) { rec.storage = rec.storage_of[0]; }
        }
        // ── REN-38-A2: a COMPUTE pass's kernel bindings are its declared BUFFER reads then writes, in that order.
        // ⛔ Reads before writes is the CONTRACT, stated here because it is the only place it can be: a kernel
        // binds by SLOT, and if the order were incidental (say, declaration order across a mixed list) then
        // reordering two lines in the asset would silently swap the kernel's input and output.
        if (d.kind == FramePassKind::Compute || d.kind == FramePassKind::RayTrace
            || d.kind == FramePassKind::ComputeIndirect || d.kind == FramePassKind::RayTracePipeline)
        {
            // ⛔ REN-38-A10: `as_args` decides whether an `indirect_args` resource counts as a KERNEL BINDING.
            // Reading one means "take my dispatch count from here" — consumed by the COMMAND PROCESSOR, not by the
            // shader — so binding it would shift every real binding down a slot and the kernel would read its own
            // arguments as data. WRITING one is an ordinary storage write: that is exactly what a cull pass does.
            const auto add_buf = [&](const crd::containers::String& n, bool as_args) {
                for (crd::usize bi = 0; bi < desc.resources.size(); ++bi)
                {
                    const FrameResourceKind rk = desc.resources[bi].kind;
                    const bool bindable = rk == FrameResourceKind::TransientBuffer
                                          || rk == FrameResourceKind::ExternalBuffer
                                          || rk == FrameResourceKind::StructuredBuffer
                                          || rk == FrameResourceKind::CounterBuffer
                                          || (rk == FrameResourceKind::IndirectArgs && as_args);
                    if (!bindable) { continue; }
                    if (desc.resources[bi].name.size() != n.size()
                        || std::memcmp(desc.resources[bi].name.c_str(), n.c_str(), n.size()) != 0)
                    {
                        continue;
                    }
                    if (rec.n_kernel_bufs < kMaxPassReads) { rec.kernel_bufs[rec.n_kernel_bufs++] = buffers[bi]; }
                    return;
                }
            };
            for (crd::usize rr = 0; rr < d.reads.size(); ++rr) { add_buf(d.reads[rr].name, false); }
            for (crd::usize ww = 0; ww < d.writes.size(); ++ww) { add_buf(d.writes[ww].name, true); }
        }
        // ⭐ REN-38-A5: THE PRESENT SEAM. The asset said WHEN in the frame to present and WHAT to present; the
        // host says WHERE. A missing surface FAILS by pass name — a graph that claims to present and silently
        // does not is precisely what this row exists to make impossible.
        if (d.kind == FramePassKind::Present)
        {
            g::IPresentSurface* surf = host.present_surface();
            if (surf == nullptr) { return fail(FrameExecError::NoPresentSurface, &d.name); }
            // ⛔ The source must be the IMPORTED output. The cooker already rejects a transient source
            // (`PresentSourceInternal`), but a PROGRAMMATIC graph never passes through the cooker — and the two
            // provenances are held to the same rules, so the check is repeated here where it cannot be bypassed.
            if (rec.n_sampled != 1U || !(rec.sampled[0] == out_handle))
            {
                return fail(FrameExecError::PresentSourceInvalid, &d.name);
            }
            pb.present(*surf);
        }
        pb.execute(&record_pass, &recs[ii]);
        // Fallback path (no post-style display pass reading a scene image): the historical weave AFTER the last
        // geometry pass, onto its target (populated by this iteration's record-build).
        if (overlay_before_ii < 0 && static_cast<crd::i64>(ii) == overlay_after_ii)
        {
            overlay_target = recs[static_cast<crd::usize>(ii)].target;
            weave_overlay();
        }
    }
    // a frame with NO geometry pass (a fullscreen/compute-only graph) keeps the historical behaviour: the
    // overlay composites over the final output
    if (last_geom_ii < 0)
    {
        g::FgExecuteFn ov_fn   = nullptr;
        void*          ov_user = nullptr;
        if (host.overlay_pass(&ov_fn, &ov_user, out_handle) && ov_fn != nullptr)
        {
            fgraph->add_pass("overlay").read_writes(out_handle).execute(ov_fn, ov_user);
        }
    }

    return true;
}

// Exactly `record` plus create / build / execute — so the one-view and multi-view paths cannot drift.
bool execute_frame_graph(const FrameGraphDesc& desc, g::IRasterContext& raster, IFrameGraphHost& host,
                         FrameExecError* err, crd::containers::String* where)
{
    auto fgraph = raster.create_frame_graph();
    if (fgraph == nullptr)
    {
        if (err != nullptr) { *err = FrameExecError::NoOutput; }
        return false;
    }
    FrameRecorder rec(desc.resources.allocator());
    rec.begin_frame();
    if (!rec.record(desc, *fgraph, raster, host, err, where)) { return false; }
    if (!fgraph->build())
    {
        if (err != nullptr) { *err = FrameExecError::BuildRejected; }
        if (where != nullptr) { *where = desc.name; }
        return false;
    }
    fgraph->execute();
    return true;
}

const char* frame_exec_error_text(FrameExecError e) noexcept
{
    switch (e)
    {
    case FrameExecError::Ok:                    return "ok";
    case FrameExecError::NoOutput:              return "the host resolved \"@output\" to null";
    case FrameExecError::UnresolvedProgram:     return "a pass names a shader the host could not resolve";
    case FrameExecError::UnresolvedDrawList:    return "a pass names a draw list the host does not know";
    case FrameExecError::UnresolvedResource:    return "a pass reads or writes a resource the graph does not declare";
    case FrameExecError::TransientFailed:       return "the device refused a declared transient";
    case FrameExecError::BuildRejected:         return "IFrameGraph::build() rejected the graph";
    case FrameExecError::UnsupportedCapability: return "the device does not provide a required capability";
    case FrameExecError::UnresolvedForEach:     return "a pass declares `for_each` but the host answered 0 instances";
    case FrameExecError::NoPresentSurface:      return "a present pass ran, and the host provided no present surface";
    case FrameExecError::PresentSourceInvalid:  return "a present pass's source is not a target that outlives the graph";
    case FrameExecError::UnresolvedAccel:       return "a raytrace pass names an acceleration structure the host does not know";
    case FrameExecError::UnresolvedArgs:        return "an indirect pass names an args buffer the graph did not create";
    }
    return "unknown error";
}

const char* frame_exec_status_text(FrameExecStatus s) noexcept
{
    switch (s)
    {
    case FrameExecStatus::Ok:                   return "the authored graph ran";
    case FrameExecStatus::FellBackToDefault:    return "FELL BACK to the default graph";
    case FrameExecStatus::FellBackToErrorGraph: return "FELL BACK to the built-in ERROR GRAPH";
    case FrameExecStatus::Failed:               return "FAILED - nothing ran";
    }
    return "unknown status";
}

FrameExecResult execute_frame_graph_with_fallback(const FrameGraphDesc& desc, g::IRasterContext& raster,
                                                  IFrameGraphHost& host, crd::memory::IAllocator* alloc)
{
    FrameExecResult res(alloc);

    // 1) the AUTHORED graph
    FrameExecError          err = FrameExecError::Ok;
    crd::containers::String where(alloc);
    if (execute_frame_graph(desc, raster, host, &err, &where))
    {
        res.status = FrameExecStatus::Ok;
        res.graph  = desc.name;
        return res;
    }
    res.error = err;
    res.where = where;
    // ⛔ REPORTED, never swallowed. A fallback that stays quiet is indistinguishable from a working frame.
    CRD_LOG_ERROR(g_log_framecook, "frame graph '{}' FAILED: {} (at '{}') - falling back",
                  desc.name.c_str(), frame_exec_error_text(err), where.c_str());

    // 2) the graph its `fallback` names
    if (!desc.fallback.empty())
    {
        const FrameGraphDesc* fb =
            host.fallback_graph(crd::containers::StringView(desc.fallback.c_str(), desc.fallback.size()));
        if (fb != nullptr)
        {
            FrameExecError          ferr = FrameExecError::Ok;
            crd::containers::String fwhere(alloc);
            if (execute_frame_graph(*fb, raster, host, &ferr, &fwhere))
            {
                res.status = FrameExecStatus::FellBackToDefault;
                res.graph  = fb->name;
                CRD_LOG_WARN(g_log_framecook, "frame graph fell back to '{}'", fb->name.c_str());
                return res;
            }
            CRD_LOG_ERROR(g_log_framecook, "the FALLBACK graph '{}' ALSO failed: {} (at '{}')", fb->name.c_str(),
                          frame_exec_error_text(ferr), fwhere.c_str());
        }
        else
        {
            CRD_LOG_ERROR(g_log_framecook, "the fallback graph '{}' does not resolve", desc.fallback.c_str());
        }
    }

    // 3) the built-in ERROR GRAPH. CODE, not an asset: it must survive a broken cook, a bad mount, a missing
    // pack and an unresolvable shader, so it depends on nothing but `clear` (which needs no program at all).
    // Loud magenta ⇒ a fallback is IMPOSSIBLE to mistake for a working frame on screen.
    if (g::IRasterTarget* t = host.output(); t != nullptr)
    {
        raster.clear(*t, g::ClearColor{kErrorGraphColor[0], kErrorGraphColor[1], kErrorGraphColor[2],
                                       kErrorGraphColor[3]});
        res.status = FrameExecStatus::FellBackToErrorGraph;
        res.graph.append("<error-graph>");
        CRD_LOG_ERROR(g_log_framecook, "rendered the ERROR GRAPH (magenta) - no usable frame graph");
        return res;
    }

    res.status = FrameExecStatus::Failed;
    CRD_LOG_ERROR(g_log_framecook, "no frame graph ran AND there is no output target to signal on");
    return res;
}

} // namespace crd::framecook
