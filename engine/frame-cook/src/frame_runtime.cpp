// frame_runtime.cpp — REN-36.2: drive `IFrameGraph` from a cooked `FrameGraphDesc`. See frame_runtime.hpp.

#include <crd/framecook/frame_runtime.hpp>

#include <crd/containers/array.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

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
    // ⛔ REN-38-A3: ALL the images this pass READS, not just the first. A pass used to keep ONE `sampled` handle,
    // which made a DEFERRED LIGHTING pass — the canonical N-texture consumer, reading albedo + normal + material +
    // depth — literally inexpressible: the asset could declare four reads, the cooker validated them, the graph
    // ordered and barriered them, and the executor bound exactly one. The declared-but-ignored failure again.
    g::FgImage           sampled[kMaxPassReads]{};
    crd::u32             n_sampled        = 0U;
    bool                 sampled_is_depth = false; // of sampled[0] — the shadow-lookup case
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
    // ⛔ PRECEDENCE. A for_each instance's program (from `instance_program`) must beat the draw list's per-draw
    // program, or every expanded cascade renders with the FIRST cascade's shader — all slices identical, which
    // is exactly the degenerate state the cascade gate exists to reject. Tracked explicitly rather than inferred.
    bool                 program_is_instance = false;
};

void record_pass(g::IFrameContext& ctx, void* user)
{
    auto*                p = static_cast<PassRec*>(user);
    const FramePassDesc& d = *p->desc;
    g::IRasterContext&   r = ctx.raster();
    // REN-36.3: `shadow_atlas[$index]` renders into ONE SLICE; a plain write renders into the whole image.
    // `image_layer(h, 0)` on a non-layered resource IS `image(h)`, so a for_each pass over a non-layered target
    // needs no special case — one code path serves both shapes.
    g::IRasterTarget*    t = p->indexed_target ? ctx.image_layer(p->target, p->layer) : ctx.image(p->target);
    if (t == nullptr || p->program == nullptr) { return; }

    const g::ClearColor clear{d.clear_color[0], d.clear_color[1], d.clear_color[2], d.clear_color[3]};
    switch (d.kind)
    {
    case FramePassKind::RasterDepthOnly:
    {
        // ⛔ EVERY draw in the list, not just the first. A real scene resolves a draw list to many mesh groups;
        // recording only one silently rendered a shadow map containing a single object. The FIRST draw clears
        // the target, the rest LOAD — clearing per draw would leave only the last group in the map (the
        // multi-pass load-not-clear scar, in its shadow-atlas form).
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            if (it.storage == nullptr) { continue; }
            g::IStorageBuffer* sb = ctx.buffer(p->storage_of[i]);
            if (sb == nullptr) { continue; }
            g::IRasterProgram* prog = p->program;
            if (!p->program_is_instance && it.program != nullptr) { prog = it.program; }
            if (i == 0U) { r.draw_storage_depth_only(*t, *prog, d.clear_depth, d.depth, *sb, it.vertex_count); }
            else         { r.draw_storage_depth_only_load(*t, *prog, d.depth, *sb, it.vertex_count); }
        }
        break;
    }
    case FramePassKind::RasterGeometry:
    case FramePassKind::RasterMrt:
    {
        // A geometry pass that READS a sampled resource binds it — that is how an authored graph expresses a
        // SHADOWED forward pass without needing a bespoke PassKind: it declares `reads = ["shadow_atlas"]` and
        // the executor picks the comparison sampler because the resource's FORMAT is depth. Same rule the
        // fullscreen kind already used, so the asset never has to name a sampler.
        g::ITexture* pass_tex = p->n_sampled > 0U ? ctx.texture(p->sampled[0]) : nullptr;
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            if (it.storage == nullptr) { continue; }
            g::IStorageBuffer* sb = ctx.buffer(p->storage_of[i]);
            if (sb == nullptr) { continue; }
            g::IRasterProgram* prog = p->program;
            if (!p->program_is_instance && it.program != nullptr) { prog = it.program; }
            const bool first = (i == 0U);
            // REN-37.10: a draw's OWN texture wins over the pass's sampled read. Without this, a geometry pass
            // that reads the shadow atlas would bind that atlas for every draw and any group carrying an albedo
            // map would silently lose it the instant shadows turned on.
            g::ITexture* tex       = it.texture != nullptr ? it.texture : pass_tex;
            const bool   depth_tex = it.texture != nullptr ? false : p->sampled_is_depth;
            if (tex != nullptr && depth_tex)
            {
                if (first) { r.draw_storage_shadowed_depth(*t, *prog, clear, d.clear_depth, d.depth, *sb, *tex, it.vertex_count); }
                else       { r.draw_storage_shadowed_depth_load(*t, *prog, d.depth, *sb, *tex, it.vertex_count); }
            }
            else if (tex != nullptr)
            {
                if (first) { r.draw_storage_textured_depth(*t, *prog, clear, d.clear_depth, d.depth, *sb, *tex, it.vertex_count); }
                else       { r.draw_storage_textured_depth_load(*t, *prog, d.depth, *sb, *tex, it.vertex_count); }
            }
            else
            {
                if (first) { r.draw_storage_depth(*t, *prog, clear, d.clear_depth, d.depth, *sb, it.vertex_count); }
                else       { r.draw_storage_depth_load(*t, *prog, d.depth, *sb, it.vertex_count); }
            }
        }
        break;
    }
    case FramePassKind::RasterFullscreen:
    {
        if (p->n_sampled == 0U) { return; }
        // ⭐ REN-38-A3: N READS => N BOUND TEXTURES. A fullscreen pass that declares several reads gets all of
        // them, in DECLARATION ORDER, through the bindless path — which is what makes a deferred LIGHTING pass
        // (albedo + normal + material + depth) authorable at all. One read keeps the single-texture path, so the
        // shadow-lookup case and every existing graph are byte-unchanged.
        if (p->n_sampled > 1U)
        {
            g::ITexture* texs[kMaxPassReads]{};
            crd::u32     n = 0U;
            for (crd::u32 i = 0; i < p->n_sampled; ++i)
            {
                g::ITexture* tx = ctx.texture(p->sampled[i]);
                // ⛔ A read that does not resolve to a texture ABORTS the pass rather than silently shifting every
                // later texture down a slot — a shifted binding renders a plausible image from the wrong inputs.
                if (tx == nullptr) { return; }
                texs[n++] = tx;
            }
            r.draw_bindless(*t, *p->program, clear, static_cast<g::ITexture* const*>(texs), n, 3U);
            break;
        }
        g::ITexture* tex = ctx.texture(p->sampled[0]);
        if (tex == nullptr) { return; }
        // Sampling a DEPTH resource means the comparison sampler — that is what a shadow lookup is. Choosing it
        // from the resource FORMAT (rather than from a flag the author must remember) keeps the asset honest:
        // you cannot accidentally sample a shadow map with a colour sampler.
        if (p->sampled_is_depth) { r.draw_shadow(*t, *p->program, clear, *tex, 3U); }
        else                     { r.draw_textured(*t, *p->program, clear, *tex, 3U); }
        break;
    }
    case FramePassKind::Compute:
    case FramePassKind::Present:
    default:
        break; // Compute dispatch + Present land with their own gates (REN-36.3+)
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

    explicit Impl(crd::memory::IAllocator* a) : alloc(a), blocks(a)
    {
        blocks.reserve(FrameRecorder::kMaxRecordingsPerFrame);
        for (crd::u32 i = 0; i < FrameRecorder::kMaxRecordingsPerFrame; ++i)
        {
            blocks.push_back(crd::containers::Array<PassRec>(a));
        }
    }
};

FrameRecorder::FrameRecorder(crd::memory::IAllocator* alloc) : m_impl(new Impl(alloc)) {}
FrameRecorder::~FrameRecorder() { delete m_impl; }
void FrameRecorder::begin_frame() noexcept { m_impl->used = 0U; }

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
    crd::containers::Array<g::FgImage> images(alloc);
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        g::FgImageDesc           id{};
        // `scale` is relative to the OUTPUT target; an absolute width/height wins when given.
        id.width   = r.width != 0U ? r.width : static_cast<crd::u32>(static_cast<float>(out_target->width()) * r.scale);
        id.height  = r.height != 0U ? r.height : static_cast<crd::u32>(static_cast<float>(out_target->height()) * r.scale);
        id.format  = r.format;
        id.samples = r.samples;
        id.sampled = r.sampled;
        id.storage = r.storage;
        id.layers  = r.layers; // REN-3.2: >1 ⇒ the 2D-array cascade/cube/stereo atlas
        const g::FgImage h = fgraph->create_transient_image(id);
        if (!h.valid()) { return fail(FrameExecError::TransientFailed, &r.name); }
        images.push_back(h);
    }
    const g::FgImage out_handle = fgraph->import_target(*out_target);

    const auto resolve_image = [&](const crd::containers::String& n, g::FgImage& h, bool& is_depth) -> bool {
        if (name_is(n, "@output")) { h = out_handle; is_depth = false; return true; }
        for (crd::usize i = 0; i < desc.resources.size(); ++i)
        {
            if (desc.resources[i].name.size() == n.size()
                && std::memcmp(desc.resources[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                h        = images[i];
                is_depth = desc.resources[i].format == g::FgImageFormat::D32Float;
                return true;
            }
        }
        return false;
    };

    // REN-36.3-b: resolve a pass's draw-list NAME to the graph's declared QUERY, then hand the whole thing to
    // the host. Falls back to a synthesized name-only desc if the graph never declared the list — the cooker
    // rejects that (`MissingDrawList`), so it is unreachable from a validated graph, but the executor must not
    // depend on that to stay memory-safe.
    const auto resolve_query = [&](const crd::containers::String& n, DrawListBinding& out) -> bool {
        for (crd::usize i = 0; i < desc.draw_lists.size(); ++i)
        {
            if (desc.draw_lists[i].name.size() == n.size()
                && std::memcmp(desc.draw_lists[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                return host.draw_list_query(desc.draw_lists[i], out);
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
    for (crd::usize ii = 0; ii < plan.size(); ++ii)
    {
        const FramePassDesc& d = desc.passes[plan[ii].pass];
        PassRec              rec{};
        rec.desc  = &d;
        rec.layer = plan[ii].index;

        DrawListBinding bind{};
        if (!d.draw_list.empty())
        {
            if (!resolve_query(d.draw_list, bind)) { return fail(FrameExecError::UnresolvedDrawList, &d.draw_list); }
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

    for (crd::usize ii = 0; ii < plan.size(); ++ii)
    {
        const FramePassDesc& d   = desc.passes[plan[ii].pass];
        PassRec&             rec = recs[ii];

        bool                        dummy_depth = false;
        g::IFramePassBuilder&       pb          = fgraph->add_pass(d.name.c_str(), d.kind == FramePassKind::Compute
                                                                                       ? g::FgPassKind::Compute
                                                                                       : g::FgPassKind::Raster);
        bool first_write = true;
        for (crd::usize w = 0; w < d.writes.size(); ++w)
        {
            g::FgImage h{};
            if (!resolve_image(d.writes[w].name, h, dummy_depth)) { return fail(FrameExecError::UnresolvedResource, &d.writes[w].name); }
            pb.writes(h);
            if (first_write)
            {
                rec.target = h;
                // the SUBSCRIPT decides slice-vs-whole-image; the cooker already proved `[$index]` only appears
                // on a layered resource inside a for_each pass, so no runtime re-validation is needed here
                rec.indexed_target = d.writes[w].indexed;
                first_write        = false;
            }
        }
        bool first_read = true;
        for (crd::usize r = 0; r < d.reads.size(); ++r)
        {
            g::FgImage h{};
            bool       is_depth = false;
            if (resolve_image(d.reads[r].name, h, is_depth))
            {
                pb.reads(h);
                if (rec.n_sampled < kMaxPassReads) { rec.sampled[rec.n_sampled++] = h; }
                if (first_read)
                {
                    rec.sampled_is_depth = is_depth;
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
            pb.reads(rec.storage_of[di]);
            if (di == 0U) { rec.storage = rec.storage_of[0]; }
        }
        pb.execute(&record_pass, &recs[ii]);
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
