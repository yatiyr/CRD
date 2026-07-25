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
struct PassRec
{
    const FramePassDesc* desc = nullptr;
    g::FgImage           target{};      // what this pass writes (the first image write)
    g::FgImage           sampled{};     // the first image it READS (fullscreen kinds sample this)
    bool                 sampled_is_depth = false;
    g::FgBuffer          storage{};
    g::IRasterProgram*   program = nullptr;
    crd::u32             vertex_count = 0U;
    // REN-36.3: which instance of an expanded `for_each` pass this is, and whether its target write carried the
    // `[$index]` subscript (⇒ it renders into ONE SLICE of a layered resource rather than the whole image).
    crd::u32             layer          = 0U;
    bool                 indexed_target = false;
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
        g::IStorageBuffer* sb = ctx.buffer(p->storage);
        if (sb == nullptr) { return; }
        r.draw_storage_depth_only(*t, *p->program, d.clear_depth, d.depth, *sb, p->vertex_count);
        break;
    }
    case FramePassKind::RasterGeometry:
    case FramePassKind::RasterMrt:
    {
        g::IStorageBuffer* sb = ctx.buffer(p->storage);
        if (sb == nullptr) { return; }
        r.draw_storage_depth(*t, *p->program, clear, d.clear_depth, d.depth, *sb, p->vertex_count);
        break;
    }
    case FramePassKind::RasterFullscreen:
    {
        g::ITexture* tex = ctx.texture(p->sampled);
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

bool execute_frame_graph(const FrameGraphDesc& desc, g::IRasterContext& raster, IFrameGraphHost& host,
                         FrameExecError* err, crd::containers::String* where)
{
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

    auto fgraph = raster.create_frame_graph();
    if (fgraph == nullptr) { return fail(FrameExecError::NoOutput, nullptr); }

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
    crd::containers::Array<PassRec> recs(alloc);
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
            rec.program      = bind.program;
            rec.vertex_count = bind.vertex_count;
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
            if (ip != nullptr) { rec.program = ip; }
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
                if (first_read)
                {
                    rec.sampled          = h;
                    rec.sampled_is_depth = is_depth;
                    first_read           = false;
                }
            }
            else { return fail(FrameExecError::UnresolvedResource, &d.reads[r].name); }
        }
        // The draw list's vertex-pull buffer is a graph-tracked READ, so the graph orders + barriers it like any
        // other resource (this is why a pass never has to think about upload/consume hazards).
        DrawListBinding bind{};
        if (!d.draw_list.empty() && resolve_query(d.draw_list, bind) && bind.storage != nullptr)
        {
            rec.storage = fgraph->import_storage(*bind.storage);
            pb.reads(rec.storage);
        }
        pb.execute(&record_pass, &recs[ii]);
    }

    if (!fgraph->build()) { return fail(FrameExecError::BuildRejected, &desc.name); }
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
