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
    g::IGpuProgram*      rt_raygen      = nullptr; // REN-38-A16: the three programs a pipeline is built from
    g::IGpuProgram*      rt_miss        = nullptr;
    g::IGpuProgram*      rt_chit        = nullptr;
    // REN-38-A9/A10: the acceleration structure a raytrace pass traverses, and the buffer an indirect pass
    // takes its count from. Both are resolved at RECORD time and held here, like every other pass input.
    g::IAccelerationStructure* accel      = nullptr;
    g::FgBuffer          args_buf{};
    crd::u64             args_offset    = 0U;
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
                               && d.kind != FramePassKind::RayTracePipeline;
    if (needs_program && p->program == nullptr) { return; }

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
    case FramePassKind::RasterMrt:
    {
        // ⭐ REN-38-A1b: N DECLARED WRITES => N COLOUR ATTACHMENTS. This is what makes a DEFERRED G-buffer
        // authorable: `writes = ["albedo", "normal", "material"]` and the executor binds all three.
        g::IRasterTarget* rts[kMaxPassReads]{};
        crd::u32          nrt = 0U;
        for (crd::u32 i = 0; i < p->n_writes; ++i)
        {
            g::IRasterTarget* rt = ctx.image(p->writes_all[i]);
            // ⛔ A write that does not resolve ABORTS the pass rather than shifting every later attachment down a
            // slot — the shader writes SV_Target1 and it would land in attachment 0's image.
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
            if (!p->program_is_instance && it.program != nullptr) { prog = it.program; }
            r.draw_storage_mrt(static_cast<g::IRasterTarget* const*>(rts), nrt, *prog, clear, d.clear_depth, d.depth,
                               *sb, it.vertex_count);
        }
        break;
    }
    case FramePassKind::RasterGeometry:
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
        // ── ⭐ REN-38-A13: PER-PASS RENDER STATE, applied before anything else looks at this pass. ──
        // ⛔ VRS and conservative raster are ATTRIBUTES of a draw, not pass kinds — so they are checked HERE, on
        // the kind that has no other reason to branch, rather than multiplying the kind enum. A fullscreen pass
        // that declares a shading rate or conservative raster takes the matching verb; one that declares neither
        // is byte-unchanged, which is why every existing graph keeps working.
        if (d.shading_rate != g::ShadingRate::Rate1x1)
        {
            r.draw_vrs(*t, *p->program, clear, d.shading_rate, d.rate_combiner, 3U);
            break;
        }
        if (d.conservative != g::ConservativeMode::Off)
        {
            r.draw_conservative(*t, *p->program, clear, d.conservative, 3U);
            break;
        }
        // ⛔ REN-38-A13: a fullscreen pass with NO READS is PROCEDURAL — a gradient, a noise field, an analytic
        // sky. It used to `return` here, so such a pass parsed, validated, executed and drew NOTHING. Found while
        // gating VRS, because a shading rate is exactly the kind of thing you measure on a procedural ramp with
        // no texture to confuse the comparison.
        if (p->n_sampled == 0U)
        {
            r.draw(*t, *p->program, clear, 3U);
            break;
        }
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
    {
        // ⭐ REN-38-A2: THE COMPUTE PASS. Until this it fell through to `break` — an authored compute pass
        // VALIDATED, COOKED, RAN and did NOTHING, with every check green.
        //
        // The kernel's storage bindings come from the pass's declared reads and writes, in declaration order, so
        // a kernel never names a slot and the graph still owns the ordering and the barriers. The grid comes from
        // `params` (`groups_x/y/z`), because a dispatch size is a PARAMETER, not topology.
        if (p->kernel_program == nullptr) { return; }
        g::IStorageBuffer* bufs[kMaxPassReads]{};
        crd::u32           nb = 0U;
        for (crd::u32 i = 0; i < p->n_kernel_bufs && nb < kMaxPassReads; ++i)
        {
            g::IStorageBuffer* sb = ctx.buffer(p->kernel_bufs[i]);
            // ⛔ ABORT rather than shift: a missing buffer would slide every later binding down a slot and the
            // kernel would read and WRITE the wrong memory — silently, and persistently.
            if (sb == nullptr) { return; }
            bufs[nb++] = sb;
        }
        if (nb == 0U) { return; }
        r.dispatch_kernel(*p->kernel_program, p->groups[0], p->groups[1], p->groups[2],
                          static_cast<g::IStorageBuffer* const*>(bufs), nb);
        break;
    }
    // ── ⭐ REN-38-A11: THE VISIBILITY-BUFFER PASS. ──
    // ⛔ The FIRST draw clears the id, every later one LOADS — one image must hold EVERY visible primitive's id.
    case FramePassKind::RasterVisbuffer:
    {
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            g::IRasterProgram* prog = p->program;
            if (!p->program_is_instance && it.program != nullptr) { prog = it.program; }
            if (prog == nullptr || it.vertex_count == 0U) { continue; }
            if (i == 0U) { r.draw_visbuffer(*t, *prog, p->clear_id, it.vertex_count); }
            else         { r.draw_visbuffer_load(*t, *prog, it.vertex_count); }
        }
        break;
    }
    // ── ⭐ REN-38-A12: THE COMPOSITE PASS — what makes OIT authorable as two ordinary passes. ──
    case FramePassKind::RasterComposite:
    {
        if (p->n_sampled == 0U) { return; }
        g::ITexture* texs[kMaxPassReads]{};
        crd::u32     n = 0U;
        for (crd::u32 i = 0; i < p->n_sampled; ++i)
        {
            g::ITexture* tx = ctx.texture(p->sampled[i]);
            // ⛔ ABORT rather than shift — a composite reading accum+revealage in the wrong order resolves a
            // plausible but wrong image, which is the hardest kind of OIT bug to see.
            if (tx == nullptr) { return; }
            texs[n++] = tx;
        }
        const g::BlendMode bm = d.blend.size() > 0U ? d.blend[0] : g::BlendMode::Alpha;
        r.draw_bindless_blend_load(*t, *p->program, static_cast<g::ITexture* const*>(texs), n, 3U, bm);
        break;
    }
    // ── ⭐ REN-38-A9: THE RAY-TRACING PASS. ──
    // An INLINE RAY QUERY dispatch: the TLAS at binding 0, the pass's buffers at 1..N, recorded into the frame's
    // one submission like any other kernel. ⛔ Not a ray-tracing PIPELINE — see the note on `FramePassKind`.
    case FramePassKind::RayTrace:
    {
        if (p->kernel_program == nullptr || p->accel == nullptr) { return; }
        g::IStorageBuffer* bufs[kMaxPassReads]{};
        crd::u32           nb = 0U;
        for (crd::u32 i = 0; i < p->n_kernel_bufs && nb < kMaxPassReads; ++i)
        {
            g::IStorageBuffer* sb = ctx.buffer(p->kernel_bufs[i]);
            // ⛔ ABORT rather than shift: a missing buffer slides every later binding down a slot, so the kernel
            // would read the wrong memory — and with a TLAS at binding 0 the shift is off-by-one by construction.
            if (sb == nullptr) { return; }
            bufs[nb++] = sb;
        }
        if (nb == 0U) { return; }
        r.dispatch_kernel_rt(*p->kernel_program, *p->accel, p->groups[0], p->groups[1], p->groups[2],
                             static_cast<g::IStorageBuffer* const*>(bufs), nb);
        break;
    }
    // ── ⭐ REN-38-A16: THE RAY-TRACING PIPELINE PASS. ──
    // Three programs and a shader binding table, recorded into the frame's one submission. ⛔ The ray-generation
    // grid comes from the pass's declared `groups_x`/`groups_y`, exactly like a dispatch: a grid that disagreed
    // with the image it writes would leave a border unwritten or run rays with no pixel to land in.
    case FramePassKind::RayTracePipeline:
    {
        if (p->rt_raygen == nullptr || p->rt_miss == nullptr || p->rt_chit == nullptr || p->accel == nullptr) { return; }
        g::IStorageBuffer* bufs[kMaxPassReads]{};
        crd::u32           nb = 0U;
        for (crd::u32 i = 0; i < p->n_kernel_bufs && nb < kMaxPassReads; ++i)
        {
            g::IStorageBuffer* sb = ctx.buffer(p->kernel_bufs[i]);
            if (sb == nullptr) { return; }
            bufs[nb++] = sb;
        }
        if (nb == 0U) { return; }
        r.trace_rays(*p->rt_raygen, *p->rt_miss, *p->rt_chit, *p->accel, p->groups[0], p->groups[1],
                     static_cast<g::IStorageBuffer* const*>(bufs), nb);
        break;
    }
    // ── ⭐ REN-38-A10: THE GPU-DRIVEN PASSES. ──
    // The workgroup / meshlet count comes from a buffer an EARLIER pass wrote, so the CPU never learns it. That
    // is the whole cull→draw loop: culled work never dispatches, and nothing round-trips to the host.
    case FramePassKind::ComputeIndirect:
    {
        if (p->kernel_program == nullptr) { return; }
        g::IStorageBuffer* args = ctx.buffer(p->args_buf);
        if (args == nullptr) { return; }
        g::IStorageBuffer* bufs[kMaxPassReads]{};
        crd::u32           nb = 0U;
        for (crd::u32 i = 0; i < p->n_kernel_bufs && nb < kMaxPassReads; ++i)
        {
            g::IStorageBuffer* sb = ctx.buffer(p->kernel_bufs[i]);
            if (sb == nullptr) { return; }
            bufs[nb++] = sb;
        }
        if (nb == 0U) { return; }
        r.dispatch_kernel_indirect(*p->kernel_program, *args, p->args_offset,
                                   static_cast<g::IStorageBuffer* const*>(bufs), nb);
        break;
    }
    case FramePassKind::RasterMeshIndirect:
    {
        g::IStorageBuffer* args = ctx.buffer(p->args_buf);
        if (args == nullptr || p->program == nullptr) { return; }
        r.draw_mesh_indirect_buffer(*t, *p->program, clear, *args, p->args_offset);
        break;
    }
    // ── ⭐ REN-38-A7 / A8: THE AMPLIFICATION PASSES. ──
    // ⛔ THE FIRST DRAW CLEARS, EVERY LATER ONE LOADS. The device's `draw_tess` / `draw_mesh` both clear — right
    // for the single-draw proof they were written for, and catastrophic here: a list of three tessellated meshes
    // would render exactly ONE, the last, and look entirely plausible. That is why 38-A7/A8 had to add
    // `draw_tess_load` / `draw_mesh_load` before this loop could exist at all.
    //
    // The COUNT comes from the draw list when there is one (a patch count, or a meshlet/task-workgroup count, is
    // per-mesh by definition), and from the declared `patches` / `groups` parameter otherwise — which is how a
    // purely PROCEDURAL amplification pass (ocean, terrain, a generated grid) is authored with no scene at all.
    case FramePassKind::RasterTess:
    case FramePassKind::RasterMesh:
    {
        const bool mesh = (d.kind == FramePassKind::RasterMesh);
        if (p->draws.count() == 0U)
        {
            if (p->amplify_count == 0U) { return; } // the cooker rejects this; the executor never guesses
            if (mesh) { r.draw_mesh(*t, *p->program, clear, p->amplify_count); }
            else      { r.draw_tess(*t, *p->program, clear, p->amplify_count); }
            break;
        }
        for (crd::u32 i = 0; i < p->draws.count(); ++i)
        {
            const DrawItem it = p->draws.at(i);
            // ⛔ A per-draw program still wins over the pass's, exactly as the geometry kinds do — otherwise a
            // list of meshes would all be amplified by the FIRST one's shader.
            g::IRasterProgram* prog = p->program;
            if (!p->program_is_instance && it.program != nullptr) { prog = it.program; }
            if (prog == nullptr) { continue; }
            // For an amplification draw the draw item's `vertex_count` IS the dispatch count: patches for a tess
            // pass, task/mesh workgroups for a mesh pass. A zero-count item is SKIPPED, never dispatched as one.
            const crd::u32 n = it.vertex_count;
            if (n == 0U) { continue; }
            if (i == 0U)
            {
                if (mesh) { r.draw_mesh(*t, *prog, clear, n); }
                else      { r.draw_tess(*t, *prog, clear, n); }
            }
            else
            {
                if (mesh) { r.draw_mesh_load(*t, *prog, n); }
                else      { r.draw_tess_load(*t, *prog, n); }
            }
        }
        break;
    }
    // ── ⭐ REN-38-A6: THE UTILITY PASSES. ──
    case FramePassKind::Clear:
    {
        // A clear needs no shader and no draw list, which is why the `p->program == nullptr` guard at the top of
        // this function would have swallowed it — see the note there.
        r.clear(*t, clear);
        break;
    }
    case FramePassKind::Copy:
    case FramePassKind::Blit:
    case FramePassKind::Resolve:
    {
        if (p->n_sampled != 1U) { return; }
        g::IRasterTarget* src = ctx.image(p->sampled[0]);
        // ⛔ A source that does not resolve ABORTS. Copying nothing leaves the destination holding whatever it
        // held before, which reads back as a plausible — and completely stale — image.
        if (src == nullptr) { return; }
        if (d.kind == FramePassKind::Copy)    { r.copy_image(*t, *src); }
        else if (d.kind == FramePassKind::Resolve) { r.resolve_image(*t, *src); }
        else
        {
            r.blit_image(*t, *src,
                         d.filter == FrameBlitFilter::Nearest ? g::IRasterContext::BlitFilter::Nearest
                                                              : g::IRasterContext::BlitFilter::Linear);
        }
        break;
    }
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
    // ⛔ REN-38-A2: TRANSIENT BUFFERS were declared by the schema and NEVER CREATED — the loop below only ever
    // built images, so a `kind = "transient_buffer"` resource validated, cooked and then did not exist. A compute
    // pass has nothing to bind without them, which is half of why `FramePassKind::Compute` could not work.
    // Parallel to `images` and indexed the same way, so `desc.resources[i]` maps to both.
    crd::containers::Array<g::FgBuffer> buffers(alloc);
    crd::containers::Array<g::FgImage> images(alloc);
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        // ⛔ REN-38-B4: an ACCELERATION STRUCTURE is EXTERNAL — the host built it. Creating nothing here keeps
        // `images`/`buffers` index-parallel with `desc.resources`, which every later lookup relies on.
        if (r.kind == FrameResourceKind::AccelerationStructure)
        {
            buffers.push_back(g::FgBuffer{});
            images.push_back(g::FgImage{});
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
            continue;
        }
        // REN-38-B3: an `indirect_args` resource is a transient BUFFER — the device already declares the indirect
        // usage on every one of them, so the two kinds differ in INTENT and in what the cooker will accept, not
        // in backing. Handling them together is what makes "a cull pass writes args" work with no special case.
        if (r.kind == FrameResourceKind::TransientBuffer || r.kind == FrameResourceKind::IndirectArgs)
        {
            const g::FgBuffer bh = fgraph->create_transient_buffer(r.size_bytes);
            if (!bh.valid()) { return fail(FrameExecError::TransientFailed, &r.name); }
            buffers.push_back(bh);
            images.push_back(g::FgImage{}); // keep the two arrays index-parallel
            continue;
        }
        buffers.push_back(g::FgBuffer{});
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

    for (crd::usize ii = 0; ii < plan.size(); ++ii)
    {
        const FramePassDesc& d   = desc.passes[plan[ii].pass];
        PassRec&             rec = recs[ii];

        bool                        dummy_depth = false;
        // ⛔ The DEVICE pass kind is derived from the AUTHORED one, never assumed. It drives queue placement and
        // the barrier scheduler's layout choice, so a present pass recorded as Raster would have its source
        // transitioned to SHADER_READ_ONLY and the surface would blit from the wrong layout.
        g::FgPassKind dev_kind = g::FgPassKind::Raster;
        if (d.kind == FramePassKind::Compute) { dev_kind = g::FgPassKind::Compute; }
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
        // REN-38-A9/A10: a ray-tracing pass and an indirect DISPATCH are compute work; an indirect MESH draw is
        // raster work that merely takes its count from a buffer.
        else if (d.kind == FramePassKind::RayTrace || d.kind == FramePassKind::ComputeIndirect
                 || d.kind == FramePassKind::RayTracePipeline)
        {
            dev_kind = g::FgPassKind::Compute;
        }
        g::IFramePassBuilder&       pb          = fgraph->add_pass(d.name.c_str(), dev_kind);
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
                    && wk != FrameResourceKind::ExternalBuffer)
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
                w_buffer = true;
                break;
            }
            if (w_buffer) { continue; }
            if (!resolve_image(d.writes[w].name, h, dummy_depth)) { return fail(FrameExecError::UnresolvedResource, &d.writes[w].name); }
            pb.writes(h);
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
                    && rk != FrameResourceKind::ExternalBuffer && rk != FrameResourceKind::AccelerationStructure)
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
                was_buffer = true;
                break;
            }
            if (was_accel) { continue; }
            if (was_buffer) { continue; }
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
