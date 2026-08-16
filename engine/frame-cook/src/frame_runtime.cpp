// frame_runtime.cpp — REN-36.2: drive `IFrameGraph` from a cooked `FrameGraphDesc`. See frame_runtime.hpp.

#include <crd/framecook/frame_runtime.hpp>

#include <crd/ceir/context.hpp>                     // CEIR-16-3c: the per-asset plan Context (build_frame_plans)
#include <crd/ceir/gpu/render_fullscreen_build.hpp> // CEIR-16-3c: build_fullscreen_ceir + FullscreenBuildDesc
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
namespace g  = crd::gpu;
namespace rg = crd::rendergraph;  // RAF-12.2-b: AuthoredPass / SlotBinding — used in to_authored_pass's signature
using SV     = crd::containers::StringView; // RAF-12.3 §7 fold: the accessors + pp:: names are keyed by StringView

bool name_is(const crd::containers::String& s, const char* lit)
{
    const crd::usize n = std::strlen(lit);
    return s.size() == n && std::memcmp(s.c_str(), lit, n) == 0;
}

// RAF-12.3 §7 fold: a stable `String*` for a folded STRING param — for the error-`where` reporting the old
// `&d.shader` / `&d.draw_list` gave. Points at the param's own stored string (which lives in `d.params`); falls
// back to the pass name if the param is somehow absent (never in the branches that use it).
const crd::containers::String* str_ptr(const FramePassDesc& d, SV name) noexcept
{
    const FrameParam* fp = find_pass_param(d, name);
    return fp != nullptr ? &fp->str : &d.name;
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

// ⭐⭐ RAF-12.2-b: translate a RESOLVED PassRec → an rg::AuthoredPass (payload + slot bindings + draw/dispatch list +
// programs + per-pass device setup). This REPLACES the 11 record_*_via_executor wrappers: each wrapper's payload +
// binding logic is reproduced here using the PassRec's FgImage/FgBuffer HANDLES (resolved to device pointers at execute
// time by the render-graph run_authored_cb) instead of resolving pointers inline. The graph declarations + overlay +
// present stay with the recorder loop; this fills only what the ONE generic dispatch needs.
void to_authored_pass(const PassRec& p, rg::AuthoredPass& out)
{
    namespace rp = crd::renderpass;
    const FramePassDesc& d = *p.desc; // SV is the file-scope alias (line 23)

    const auto param = [&](const char* name, rp::ExecutorParamType ty, auto set) {
        rp::TypedValue tv;
        tv.type = ty;
        set(tv);
        out.payload.params.push_back(rp::ParamValue{rp::pass_param_id(SV(name)), tv});
    };
    const auto p_vec4 = [&](const char* n, const float v[4]) {
        param(n, rp::ExecutorParamType::Vec4, [&](rp::TypedValue& t) { t.v4[0]=v[0]; t.v4[1]=v[1]; t.v4[2]=v[2]; t.v4[3]=v[3]; });
    };
    const auto p_f32  = [&](const char* n, float v)      { param(n, rp::ExecutorParamType::F32, [&](rp::TypedValue& t){ t.f=v; }); };
    const auto p_enum = [&](const char* n, crd::u32 e)   { param(n, rp::ExecutorParamType::Enum, [&](rp::TypedValue& t){ t.e=e; }); };
    const auto p_bool = [&](const char* n, bool b)       { param(n, rp::ExecutorParamType::Bool, [&](rp::TypedValue& t){ t.b=b; }); };
    const auto p_u32  = [&](const char* n, crd::u32 u)   { param(n, rp::ExecutorParamType::U32, [&](rp::TypedValue& t){ t.u=u; }); };
    // RAF-12.3 §7 fold: the clear colour comes from the `clear_color` param (default {0,0,0,1} — the old field default).
    const auto p_clear = [&]() { float cc[4]={0.0F,0.0F,0.0F,1.0F}; pass_vec4(d, SV(pp::kClearColor), cc); p_vec4("clear_color", cc); };

    // push a SlotBinding + its mirroring payload ResourceRef (resource_id == slot hash — the wrapper convention).
    const auto bind = [&](const char* slot, rp::SlotResourceKind kind, rp::SlotAccess access, rg::SlotResolve res,
                          g::FgImage img, g::FgBuffer buf, g::IAccelerationStructure* accel, g::FgImage depth,
                          crd::u32 layer) {
        const crd::u64 id = rp::pass_param_id(SV(slot));
        rg::SlotBinding b{};
        b.name_hash = id; b.kind = kind; b.resolve = res; b.image = img; b.buffer = buf; b.accel = accel;
        b.depth = depth; b.layer = layer;
        out.bindings.push_back(b);
        out.payload.resources.push_back(rp::ResourceRef{id, kind, access, id});
    };
    // the PRIMARY write target (color / depth): image_with_depth when a separate depth companion, image_layer for a
    // for_each cascade slice, else a plain image — exactly record_pass's `t` resolution.
    const rg::SlotResolve prim = [&]() -> rg::SlotResolve {
        if (p.depth_target.valid()) { return rg::SlotResolve::ImageWithDepth; }
        if (p.indexed_target) { return rg::SlotResolve::ImageLayer; }
        return rg::SlotResolve::Image;
    }();
    const auto bind_prim = [&](const char* slot, rp::SlotResourceKind kind, rp::SlotAccess access) {
        bind(slot, kind, access, prim, p.target, g::FgBuffer{}, nullptr, p.depth_target, p.layer);
    };
    const auto add_draws_scene = [&](bool depth_only, bool procedural) {
        out.draws.reserve(p.draws.count());
        for (crd::u32 i = 0; i < p.draws.count(); ++i)
        {
            const DrawItem it = p.draws.at(i);
            // ⛔ CEIR-16z-3 (§41): a PROCEDURAL (visbuffer) draw has NO storage (gl_VertexIndex geometry). The storage-null
            // skip is the STORAGE ladder's resolve-failure guard, so it must NOT drop a procedural draw (which legitimately
            // carries no storage). Storage mode is byte-identical to before.
            if (!procedural && it.storage == nullptr) { continue; }
            g::IRasterProgram* twin = nullptr;
            if (!p.program_is_instance)
            {
                if (depth_only)                            { twin = it.program_depth != nullptr ? it.program_depth : it.program; }
                // ⛔⛔ CEIR-18c: a G-buffer pass PACKS the surface (material_pass="GBuffer") — it must be checked
                // BEFORE the MRT-velocity arm (a G-buffer pass IS an MRT pass) and must NOT fall back to `program`:
                // the forward FS as "albedo" is a silent-wrong the deferred lighting then decodes as a surface (the
                // §128 class). Null twin ⇒ the draw is refused (twin stays null), never degraded to forward.
                // ⛔⛔ CEIR-18c: a G-buffer pass PACKS the surface (material_pass="GBuffer") — checked BEFORE the
                // MRT-velocity arm (a G-buffer pass IS an MRT pass) and NEVER falling back to `program` (the forward
                // FS as "albedo" is the §128 silent-wrong). Null twin ⇒ the draw is refused, never degraded.
                else if (pass_u32(d, SV(pp::kMaterialPass), 0U)
                         == static_cast<crd::u32>(crd::framecook::FrameMaterialPass::GBuffer)) { twin = it.program_gbuffer; }
                else if (pass_flag(d, SV(pp::kMrt)))       { twin = it.program_velocity != nullptr ? it.program_velocity : it.program; }
                else                                       { twin = it.program; }
            }
            rg::AuthoredDraw ad{};
            if (procedural)
            {
                // a plain gl_VertexIndex draw: program + vertex_count only (no storage/texture/args) — emit_scene_list's
                // procedural branch (16z-2) records it as GeometryKind::None with ZERO bindings.
                ad.has_storage = false; ad.program = twin; ad.vertex_count = it.vertex_count;
            }
            else
            {
                ad.has_storage = true; ad.storage = p.storage_of[i]; ad.program = twin; ad.texture = it.texture;
                ad.vertex_count = it.vertex_count; ad.indexed = it.indexed; ad.index_count = it.index_count;
                ad.instance_count = it.instance_count; ad.first_index = it.first_index; ad.args = it.args;
                ad.args_offset = it.args_offset;
            }
            out.draws.push_back(ad);
        }
    };

    // ⭐ RAF-12.3: the cooked mechanic is set ONCE from the pass' executor id — no record-time string hash (mission
    // condition 18). Each arm below keys off the pass' executor id + role bits, replacing the retired FramePassKind
    // switch; a non-builtin id (an app-registered executor) falls to the final `else` (the custom arm).
    out.executor = d.executor_id;

    if (pass_is_scene_raster(d))
    {
        out.payload.queue = rp::QueueKind::Graphics;
        out.programs.raster = p.program;
        const bool depth_only = pass_flag(d, SV(pp::kDepthOnly));
        const bool mrt        = pass_flag(d, SV(pp::kMrt)) && p.n_writes > 1U;
        const bool procedural = pass_flag(d, SV(pp::kProcedural)); // ⛔ CEIR-16z-3: the visbuffer role (gl_VertexIndex draws)
        bind_prim(depth_only ? "depth" : "color",
                  depth_only ? rp::SlotResourceKind::DepthTarget : rp::SlotResourceKind::ColorTarget,
                  depth_only ? rp::SlotAccess::ReadWrite : rp::SlotAccess::Write);
        p_clear();
        p_f32("clear_depth", pass_f32(d, SV(pp::kClearDepth), 1.0F));
        p_enum("depth_compare", pass_u32(d, SV(pp::kDepthCompare), static_cast<crd::u32>(g::DepthCompare::LessEqual)));
        if (pass_flag(d, SV(pp::kLoad)) || p.load_override) { p_bool("load", true); }
        if (pass_flag(d, SV(pp::kLoadDepth))) { p_bool("load_depth", true); }
        if (mrt)
        {
            static const char* const kMrt[3]   = {"color1", "color2", "color3"};
            static const char* const kBlend[4] = {"blend0", "blend1", "blend2", "blend3"};
            for (crd::u32 k = 1; k < p.n_writes && k <= 3U; ++k)
            {
                bind(kMrt[k - 1U], rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, rg::SlotResolve::Image,
                     p.writes_all[k], g::FgBuffer{}, nullptr, g::FgImage{}, 0U);
            }
            for (crd::u32 k = 0; k < p.n_writes && k <= 3U; ++k)
            {
                p_enum(kBlend[k], pass_u32(d, SV(pp::kBlendSlot[k]), static_cast<crd::u32>(g::BlendMode::Opaque)));
            }
        }
        // ⛔ CEIR-16z-3: a visbuffer (procedural) scene pass carries the background id into the payload (cook==record parity;
        // the CEIR plan bakes the typed uint clear from the R32Uint target, and the payload mirrors the authored id).
        if (procedural) { p_u32("clear_id", p.clear_id); }
        add_draws_scene(depth_only, procedural);
        // the pass's sampled read (shadow atlas / moment array) — the scene executor's atlas/sampler routing.
        if (p.n_sampled > 0U)
        {
            out.has_pass_texture       = true;
            out.pass_texture           = p.sampled[0];
            out.pass_texture_is_depth  = p.sampled_is_depth || p.sampled_is_array;
            out.pass_texture_comparison = p.sampled_is_depth;
        }
    }
    else if (pass_is_fullscreen(d))
    {
        out.payload.queue = rp::QueueKind::Graphics;
        out.programs.raster = p.program;
        bind_prim("color", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write);
        static const char* const kIn[8] = {"input0","input1","input2","input3","input4","input5","input6","input7"};
        for (crd::u32 i = 0; i < p.n_sampled && i < 8U; ++i)
        {
            bind(kIn[i], rp::SlotResourceKind::Texture, rp::SlotAccess::Read, rg::SlotResolve::Texture, p.sampled[i],
                 g::FgBuffer{}, nullptr, g::FgImage{}, 0U);
        }
        if (p.fs_constants.valid())
        {
            bind("constants", rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, rg::SlotResolve::Buffer,
                 g::FgImage{}, p.fs_constants, nullptr, g::FgImage{}, 0U);
        }
        p_clear();
        const crd::u32 sr = pass_u32(d, SV(pp::kShadingRate), static_cast<crd::u32>(g::ShadingRate::Rate1x1));
        if (sr != static_cast<crd::u32>(g::ShadingRate::Rate1x1)) { p_enum("shading_rate", sr); }
        const crd::u32 cons = pass_u32(d, SV(pp::kConservative), static_cast<crd::u32>(g::ConservativeMode::Off));
        if (cons != static_cast<crd::u32>(g::ConservativeMode::Off)) { p_enum("conservative", cons); }
        if (pass_flag(d, SV(pp::kDepthAsFloat))) { p_bool("depth_as_float", true); }
        if (pass_flag(d, SV(pp::kComposite)))
        {
            p_bool("load", true);
            p_enum("blend", pass_u32(d, SV(pp::kBlendSlot[0]), static_cast<crd::u32>(g::BlendMode::Alpha)));
        }
    }
    else if (pass_is_compute(d))
    {
        out.payload.queue = rp::QueueKind::Compute;
        out.programs.kernel = p.kernel_program;
        p_u32("groups_x", p.groups[0]); p_u32("groups_y", p.groups[1]); p_u32("groups_z", p.groups[2]);
        static const char* const kStor[4] = {"storage", "storage1", "storage2", "storage3"};
        for (crd::u32 i = 0; i < p.n_kernel_bufs && i < 4U; ++i)
        {
            bind(kStor[i], rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::ReadWrite, rg::SlotResolve::Buffer,
                 g::FgImage{}, p.kernel_bufs[i], nullptr, g::FgImage{}, 0U);
        }
        if (p.n_sampled > 0U)
        {
            bind("sampled", rp::SlotResourceKind::Texture, rp::SlotAccess::Read, rg::SlotResolve::Texture, p.sampled[0],
                 g::FgBuffer{}, nullptr, g::FgImage{}, 0U);
        }
        if (p.args_buf.valid())
        {
            bind("args", rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, rg::SlotResolve::Buffer,
                 g::FgImage{}, p.args_buf, nullptr, g::FgImage{}, 0U);
            p_u32("args_offset", static_cast<crd::u32>(p.args_offset));
        }
        for (crd::u32 i = 0; i < p.draws.count(); ++i)
        {
            const DrawItem it = p.draws.at(i);
            if (it.storage == nullptr) { continue; } // the compute wrapper skipped an unresolved per-item storage
            rg::AuthoredDraw ad{};
            ad.has_storage = true; ad.storage = p.storage_of[i]; ad.args = it.args;
            ad.dispatch_groups = it.dispatch_groups;
            out.draws.push_back(ad);
        }
    }
    else if (pass_is_tess(d) || pass_is_mesh(d))
    {
        out.payload.queue = rp::QueueKind::Graphics;
        out.programs.raster = p.program;
        bind_prim("color", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write);
        p_clear();
        if (p.draws.count() == 0U) { p_u32("amplify_count", p.amplify_count); }
        for (crd::u32 i = 0; i < p.draws.count(); ++i)
        {
            const DrawItem it = p.draws.at(i);
            rg::AuthoredDraw ad{};
            ad.has_storage = it.storage != nullptr; ad.storage = p.storage_of[i];
            ad.program = p.program_is_instance ? nullptr : it.program; ad.vertex_count = it.vertex_count;
            out.draws.push_back(ad);
        }
    }
    // ⛔ CEIR-16z-3b: the visbuffer to_authored_pass arm DELETED (§41 dissolution). A visbuffer is now a scene.raster PROCEDURAL
    // pass, handled by the scene arm above (add_draws_scene(procedural) carries the no-storage gl_VertexIndex draws + clear_id).
    else if (pass_is_mesh_indirect(d))
    {
        out.payload.queue = rp::QueueKind::Graphics;
        out.programs.raster = p.program;
        bind_prim("color", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write);
        bind("args", rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, rg::SlotResolve::Buffer, g::FgImage{},
             p.args_buf, nullptr, g::FgImage{}, 0U);
        p_clear();
        p_u32("args_offset", static_cast<crd::u32>(p.args_offset));
    }
    else if (pass_is_raytrace_dispatch(d))
    {
        out.payload.queue = rp::QueueKind::Compute;
        out.programs.kernel = p.kernel_program;
        bind("accel", rp::SlotResourceKind::AccelStructure, rp::SlotAccess::Read, rg::SlotResolve::Accel, g::FgImage{},
             g::FgBuffer{}, p.accel, g::FgImage{}, 0U);
        static const char* const kStor[4] = {"storage", "storage1", "storage2", "storage3"};
        for (crd::u32 i = 0; i < p.n_kernel_bufs && i < 4U; ++i)
        {
            bind(kStor[i], rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::ReadWrite, rg::SlotResolve::Buffer,
                 g::FgImage{}, p.kernel_bufs[i], nullptr, g::FgImage{}, 0U);
        }
        p_u32("groups_x", p.groups[0]); p_u32("groups_y", p.groups[1]); p_u32("groups_z", p.groups[2]);
    }
    else if (pass_is_raytrace_pipeline(d))
    {
        out.payload.queue = rp::QueueKind::Compute;
        out.programs.raygen = p.rt_raygen; out.programs.miss = p.rt_miss; out.programs.closest_hit = p.rt_chit;
        out.programs.any_hit = p.rt_anyhit; out.programs.intersection = p.rt_isect; out.programs.callable = p.rt_callable;
        bind("accel", rp::SlotResourceKind::AccelStructure, rp::SlotAccess::Read, rg::SlotResolve::Accel, g::FgImage{},
             g::FgBuffer{}, p.accel, g::FgImage{}, 0U);
        static const char* const kStor[4] = {"storage", "storage1", "storage2", "storage3"};
        for (crd::u32 i = 0; i < p.n_kernel_bufs && i < 4U; ++i)
        {
            bind(kStor[i], rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::ReadWrite, rg::SlotResolve::Buffer,
                 g::FgImage{}, p.kernel_bufs[i], nullptr, g::FgImage{}, 0U);
        }
        p_u32("groups_x", p.groups[0]); p_u32("groups_y", p.groups[1]);
    }
    else if (pass_is_transfer(d))
    {
        out.payload.queue = rp::QueueKind::Transfer;
        if (pass_is_transfer_clear(d))
        {
            bind_prim("target", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write);
            p_clear();
        }
        else
        {
            // copy/blit/resolve: src (sampled[0], as a colour target) → dst (the primary target).
            if (p.n_sampled == 1U)
            {
                bind("src", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Read, rg::SlotResolve::Image, p.sampled[0],
                     g::FgBuffer{}, nullptr, g::FgImage{}, 0U);
            }
            bind_prim("dst", rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write);
            if (pass_is_blit(d)) { p_enum("filter", pass_u32(d, SV(pp::kFilter), static_cast<crd::u32>(FrameBlitFilter::Linear))); }
        }
    }
    else if (pass_is_present(d))
    {
        out.payload.queue = rp::QueueKind::Graphics;
    }
    else // ⭐⭐ RAF-12.3: a CUSTOM pass — its `out.executor` (set above from d.executor_id) is the app-registered id.
    {
        out.payload.queue = rp::QueueKind::Graphics;
        out.programs.raster = p.program;
        static const char* const kCol[4] = {"color", "color1", "color2", "color3"};
        bind(kCol[0], rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, prim, p.target, g::FgBuffer{}, nullptr,
             p.depth_target, p.layer);
        for (crd::u32 w = 1U; w < p.n_writes && w < 4U; ++w)
        {
            bind(kCol[w], rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, rg::SlotResolve::Image,
                 p.writes_all[w], g::FgBuffer{}, nullptr, g::FgImage{}, 0U);
        }
        static const char* const kIn[8] = {"input0","input1","input2","input3","input4","input5","input6","input7"};
        for (crd::u32 i = 0; i < p.n_sampled && i < 8U; ++i)
        {
            bind(kIn[i], rp::SlotResourceKind::Texture, rp::SlotAccess::Read, rg::SlotResolve::Texture, p.sampled[i],
                 g::FgBuffer{}, nullptr, g::FgImage{}, 0U);
        }
        if (p.fs_constants.valid())
        {
            bind("constants", rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, rg::SlotResolve::Buffer,
                 g::FgImage{}, p.fs_constants, nullptr, g::FgImage{}, 0U);
        }
        p_clear();
        for (crd::usize k = 0; k < d.params.size(); ++k)
        {
            const FrameParam& fp = d.params[k];
            // ⭐ RAF-12.3 §7 fold: forward only GENUINE authored params into the custom payload — the folded engine
            // config (clear_color handled by p_clear() above, plus depth/blend/sampler/state/…) is not the app's.
            if (is_folded_pass_param(SV(fp.name.c_str(), fp.name.size()))) { continue; }
            switch (fp.type)
            {
            case FrameParamType::Float: p_f32(fp.name.c_str(), static_cast<float>(fp.v[0])); break;
            case FrameParamType::Int:
            case FrameParamType::U32:   p_u32(fp.name.c_str(), static_cast<crd::u32>(fp.v[0])); break;
            case FrameParamType::Bool:  p_bool(fp.name.c_str(), fp.v[0] != 0.0); break;
            case FrameParamType::Enum:  p_enum(fp.name.c_str(), static_cast<crd::u32>(fp.v[0])); break;
            case FrameParamType::Vec4:
            {
                const float v4[4] = {static_cast<float>(fp.v[0]), static_cast<float>(fp.v[1]),
                                     static_cast<float>(fp.v[2]), static_cast<float>(fp.v[3])};
                p_vec4(fp.name.c_str(), v4);
                break;
            }
            case FrameParamType::String: break; // a program/resource reference — not a scalar payload value
            }
        }
        for (crd::u32 i = 0; i < p.draws.count(); ++i)
        {
            const DrawItem it = p.draws.at(i);
            if (it.storage == nullptr) { continue; }
            rg::AuthoredDraw ad{};
            ad.has_storage = true; ad.storage = p.storage_of[i];
            ad.program = p.program_is_instance ? nullptr : it.program; ad.texture = it.texture;
            ad.vertex_count = it.vertex_count; ad.indexed = it.indexed; ad.index_count = it.index_count;
            ad.instance_count = it.instance_count; ad.first_index = it.first_index; ad.args = it.args;
            ad.args_offset = it.args_offset;
            out.draws.push_back(ad);
        }
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
    // ⭐⭐ RAF-12.2-b: parallel to `blocks` — the per-pass rg::AuthoredPass the ONE generic render-graph dispatch reads.
    // The graph stores &authored[ii] as the pass `void* user`, so this arena has the SAME lifetime discipline as `blocks`.
    crd::containers::Array<crd::containers::Array<crd::rendergraph::AuthoredPass>> authored;
    crd::u32                                               used = 0U;
    // REN-38-B1: which side of the ping-pong pair is CURRENT. Advanced by `begin_frame()`, so a graph recorded
    // twice in one frame (the multi-viewport path) sees ONE parity — two viewports must not disagree about which
    // image is history.
    crd::u32                                               frame_parity = 0U;
    // ⭐⭐ RAF-8a: the render-graph executor registry, built ONCE — every pass dispatches through it (RAF-12.2-b: the ONE
    // generic run-callback; the FramePassKind switch + per-kind wrappers are retired).
    crd::rendergraph::GraphExecutorTable                   records;
    // RAF-12.2-b: diagnostics the generic dispatch reports an unknown-executor into (a frame-persistent sink).
    crd::renderasset::DiagnosticList                       dispatch_diags;

    explicit Impl(crd::memory::IAllocator* a) : alloc(a), blocks(a), authored(a), records(a), dispatch_diags(a)
    {
        blocks.reserve(FrameRecorder::kMaxRecordingsPerFrame);
        authored.reserve(FrameRecorder::kMaxRecordingsPerFrame);
        for (crd::u32 i = 0; i < FrameRecorder::kMaxRecordingsPerFrame; ++i)
        {
            blocks.push_back(crd::containers::Array<PassRec>(a));
            authored.push_back(crd::containers::Array<crd::rendergraph::AuthoredPass>(a));
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
                           IFrameGraphHost& host, FrameExecError* err, crd::containers::String* where,
                           const FramePlans* plans)
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
    crd::containers::Array<crd::rendergraph::AuthoredPass>& aps = m_impl->authored[m_impl->used];
    crd::containers::Array<PassRec>& recs = m_impl->blocks[m_impl->used++];
    aps.clear();
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
    aps.reserve(plan.size());  // RAF-12.2-b: the parallel AuthoredPass arena the graph's user pointers refer to
    // ⭐⭐ REN-39 (the gizmo fix): the host's overlay is INSERTED after the LAST geometry pass, onto that
    // pass's own target — live depth, pre-tonemap. Passes execute in DECLARATION order, so an overlay merely
    // APPENDED after a frame with a post chain lands after the display transform, depth-testing against the
    // output's never-written depth (the exact "gizmo looks weird" the sandbox showed under the AgX frame).
    crd::i64 last_geom_ii = -1;
    for (crd::usize gi = 0; gi < plan.size(); ++gi)
    {
        if (pass_is_raster_geometry(desc.passes[plan[gi].pass]))
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
        // RAF-12.3 §7 fold: draw_list / shader are STRING PARAMS now. `resolve_query`/`fail` take a `const String&`,
        // so use the param's own stored string (stable in `d.params`) rather than a StringView temporary.
        if (const FrameParam* dlp = find_pass_param(d, SV(pp::kDrawList)); dlp != nullptr && !dlp->str.empty() && !rec.load_override)
        {
            // ⛔⛔ ONLY an EXPANDED pass carries its instance to the host. `plan[ii].index` is 0 for BOTH
            // "cascade 0" and "not expanded at all" — and handing 0 to the host made it stamp CASCADE 0's
            // vertex counts onto the FORWARD pass, truncating the whole scene draw (textures gone, shadows
            // wrong, geometry missing — the exact live-app symptom). kNoInstance = "this pass is not one of N".
            if (!resolve_query(dlp->str, bind,
                               d.for_each != FrameForEach::None ? plan[ii].index : 0xFFFFFFFFU))
            {
                return fail(FrameExecError::UnresolvedDrawList, &dlp->str);
            }
            rec.draws        = bind;
            rec.program      = bind.at(0).program;
            rec.vertex_count = bind.at(0).vertex_count;
        }
        if (!pass_str(d, SV(pp::kShader)).empty())
        {
            rec.program = host.program(pass_str(d, SV(pp::kShader)));
            // a missing program must FAIL, never render something plausible
            if (rec.program == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kShader))); }
        }
        // ⛔⛔ REN-38 llvmpipe campaign: a pass that DRAWS with no resolved program used to fall through to
        // `record_pass`, whose program guard returned SILENTLY — a black frame with draws reported and no
        // error anywhere (the exact class this band keeps killing, one layer further in). A draw-list pass
        // whose host binding carries no program is now the SAME named failure as a missing shader.
        {
            const bool draws_geometry = pass_draws_geometry(d);
            if (draws_geometry && rec.program == nullptr && !rec.load_override)
            {
                return fail(FrameExecError::UnresolvedProgram, &d.name);
            }
        }
        // ⭐ REN-38-B4: resolve every acceleration structure this pass READS, through the host. A raytrace pass
        // takes the first — the cooker already proved there is one.
        if (pass_is_raytrace_dispatch(d) || pass_is_raytrace_pipeline(d))
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
        if (pass_is_tess(d) || pass_is_mesh(d))
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
        if (pass_is_raytrace_pipeline(d))
        {
            rec.rt_raygen = host.kernel(pass_str(d, SV(pp::kRaygen)));
            if (rec.rt_raygen == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kRaygen))); }
            rec.rt_miss = host.kernel(pass_str(d, SV(pp::kMiss)));
            if (rec.rt_miss == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kMiss))); }
            rec.rt_chit = host.kernel(pass_str(d, SV(pp::kClosestHit)));
            if (rec.rt_chit == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kClosestHit))); }
            // REN-38 audit: the any-hit is OPTIONAL, but a NAMED one that does not resolve FAILS — a pipeline
            // silently built without its any-hit traces every transparent texel as solid, which renders.
            if (!pass_str(d, SV(pp::kAnyHit)).empty())
            {
                rec.rt_anyhit = host.kernel(pass_str(d, SV(pp::kAnyHit)));
                if (rec.rt_anyhit == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kAnyHit))); }
            }
            // REN-38-F13: the last two SBT roles - optional, but a NAMED one that does not resolve FAILS.
            if (!pass_str(d, SV(pp::kIntersection)).empty())
            {
                rec.rt_isect = host.kernel(pass_str(d, SV(pp::kIntersection)));
                if (rec.rt_isect == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kIntersection))); }
            }
            if (!pass_str(d, SV(pp::kCallable)).empty())
            {
                rec.rt_callable = host.kernel(pass_str(d, SV(pp::kCallable)));
                if (rec.rt_callable == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kCallable))); }
            }
        }
        // ⛔ REN-38-A16: the LAUNCH GRID is read for the RT-pipeline kind too. It was not, so `groups` stayed
        // {1,1,1} and a `raytrace.pipeline` pass fired exactly ONE ray however many the asset declared — one
        // correct pixel and an untouched buffer everywhere else, which reads as a traversal failure.
        if (pass_dispatches_kernel(d) || pass_is_raytrace_pipeline(d))
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
        if (pass_dispatches_kernel(d))
        {
            rec.kernel_program = host.kernel(pass_str(d, SV(pp::kKernel)));
            if (rec.kernel_program == nullptr) { return fail(FrameExecError::UnresolvedProgram, str_ptr(d, SV(pp::kKernel))); }
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
        if (pass_dispatches_kernel(d) || pass_is_raytrace_pipeline(d))
        {
            dev_kind = g::FgPassKind::Compute;
        }
        else if (pass_is_present(d)) { dev_kind = g::FgPassKind::Present; }
        // ⛔ REN-38-A6: copy/blit/resolve are TRANSFER passes so the barrier scheduler picks TRANSFER_SRC/DST.
        // A CLEAR is NOT: it is `LOAD_OP_CLEAR` on an attachment (`ClearRenderTargetView` on DX12), which needs
        // the ordinary colour-attachment layout — classifying it as transfer would clear an image the hardware
        // was told to treat as a copy destination.
        else if (pass_is_transfer_copy(d) || pass_is_blit(d) || pass_is_transfer_resolve(d))
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
            if (dummy_depth && pass_flag(d, SV(pp::kMrt)))
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
        if (const FrameParam* sdp = find_pass_param(d, SV(pp::kSharedDepth)); sdp != nullptr && !sdp->str.empty())
        {
            g::FgImage dh{};
            bool       d_depth = false;
            if (!resolve_image(sdp->str, dh, d_depth))
            {
                return fail(FrameExecError::UnresolvedResource, &sdp->str);
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
                    rec.sampled_is_depth = is_depth && !pass_flag(d, SV(pp::kDepthAsFloat));
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
            if (pass_flag(d, SV(pp::kUntracked))) { if (di == 0U) { rec.storage = rec.storage_of[0]; } continue; }
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
        if (pass_dispatches_kernel(d) || pass_is_raytrace_pipeline(d))
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
        if (pass_is_present(d))
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
        // ⭐⭐ RAF-12.2-b: build this pass's AuthoredPass (payload + slot bindings + draw/dispatch list + programs +
        // per-pass device setup) and record it through the ONE generic render-graph dispatch. The FramePassKind switch
        // + the 11 record_*_via_executor wrappers + record_pass are retired: there is now exactly one record path.
        aps.push_back(crd::rendergraph::AuthoredPass(m_impl->alloc));
        crd::rendergraph::AuthoredPass& ap = aps[aps.size() - 1U];
        to_authored_pass(rec, ap);
        // CEIR-16-3c: attach this pass's CEIR replay plan if the asset built one — keyed by the AUTHORED pass name (every
        // for_each-expanded instance shares it; the [$index] target difference is the target resolver's job at record). A
        // null plan leaves the pass on its C++ executor path (still correct for a NOT-YET-MIGRATED executor); a MIGRATED
        // executor (record_ceir_render) treats a null plan as a load-path bug and fails the record loud (ctx.fail()).
        ap.plan = plans != nullptr
                      ? plans->table.find(crd::renderpass::pass_param_id(
                            crd::containers::StringView(d.name.c_str(), d.name.size())))
                      : nullptr;
        // ⛔⛔ CEIR-17z: a MIGRATED executor (record_ceir_render) with NO plan is a LOUD, NAMED failure — never the silent
        // no-record the 16d-live-4c deletion could otherwise produce (the imperative fallback that masked it is GONE). The
        // caller MUST build_frame_plans and pass them (the scene renderer does; execute_frame_graph does; direct callers
        // must). Fires BEFORE any device work. This is the doctrine the L1243 comment promised and nothing implemented.
        if (ap.plan == nullptr && pass_is_migrated_ceir(d)) { return fail(FrameExecError::MissingCeirPlan, &d.name); }
        ap.device_kind = dev_kind;
        ap.has_sampler = pass_flag(d, SV(pp::kHasSampler));
        ap.sampler     = pass_sampler(d);
        ap.state       = pass_state(d);
        ap.n_counters  = rec.n_counters < crd::rendergraph::kMaxAuthoredCounters
                             ? rec.n_counters
                             : crd::rendergraph::kMaxAuthoredCounters;
        for (crd::u32 ci = 0; ci < ap.n_counters; ++ci) { ap.counters[ci] = rec.counters[ci]; }
        ap.records = &m_impl->records;
        ap.alloc   = m_impl->alloc;
        ap.diags   = &m_impl->dispatch_diags;
        pb.execute(crd::rendergraph::authored_pass_fn(), &ap);
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
    // ⛔⛔ CEIR-17z: the migrated executors (scene/fullscreen/mesh/tess/mesh.indirect → record_ceir_render) need a per-pass
    // CEIR replay plan. This synchronous wrapper owns record→build→execute in ONE scope, so a STACK FramePlans has exactly
    // the right lifetime — it outlives the fgraph->execute() below and dies on return (no Impl member, no reuse, no UAF).
    // build_frame_plans no-ops for a graph with no migrated pass; a build FAILURE is a real, NAMED load-path error, never
    // the silent no-record §128's deletion could otherwise produce.
    crd::renderasset::DiagnosticList plan_diags(desc.resources.allocator());
    FramePlans                       plans(desc.resources.allocator());
    if (!build_frame_plans(desc, plans, plan_diags))
    {
        if (err != nullptr) { *err = FrameExecError::MissingCeirPlan; }
        if (where != nullptr) { *where = desc.name; }
        return false;
    }
    FrameRecorder rec(desc.resources.allocator());
    rec.begin_frame();
    if (!rec.record(desc, *fgraph, raster, host, err, where, &plans)) { return false; }
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
    case FrameExecError::MissingCeirPlan:       return "a migrated executor (record_ceir_render) got no CEIR replay plan — the caller must build_frame_plans and pass them";
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

// ── CEIR-16-3c: build the per-fullscreen-pass CEIR replay plans ONCE, at load ──
namespace
{
const FrameResourceDesc* fs_find_resource(const FrameGraphDesc& desc, const crd::containers::String& name) noexcept
{
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        if (desc.resources[i].name.size() == name.size()
            && std::memcmp(desc.resources[i].name.c_str(), name.c_str(), name.size()) == 0)
        {
            return &desc.resources[i];
        }
    }
    return nullptr;
}
// A fullscreen READ that resolves to a BUFFER is the pass's CONSTANTS buffer (REN-41 TAA); every other read is a sampled
// texture. Mirrors the record loop's kind test (~L1114), minus the Accel case (never a fullscreen read).
bool fs_read_is_buffer(FrameResourceKind k) noexcept
{
    return k == FrameResourceKind::TransientBuffer || k == FrameResourceKind::StructuredBuffer
           || k == FrameResourceKind::CounterBuffer || k == FrameResourceKind::ExternalBuffer
           || k == FrameResourceKind::IndirectArgs;
}
} // namespace

FramePlans::FramePlans(crd::memory::IAllocator* a) : table(a), storage(a), alloc(a) {}
FramePlans::~FramePlans() { delete ctx; }

bool build_frame_plans(const FrameGraphDesc& desc, FramePlans& out, crd::renderasset::DiagnosticList& diags)
{
    namespace rp = crd::renderpass;
    static const char* const kIn[8] = {"input0", "input1", "input2", "input3", "input4", "input5", "input6", "input7"};
    // Reserve storage to the EXACT fullscreen-pass count: the CeirPassPlans bind pointers INTO `storage`, so it must never
    // relocate (the reserved-arena discipline the recorder itself uses).
    // ⛔ CEIR-16d-live-2: scene.raster (raster.scene / raster.depth_only / raster.mrt — all kExecSceneRaster) joins the
    // COUNT loop, not only the build loop: `out.storage` is reserved to EXACTLY this count and every CeirPassPlan binds a
    // `cmds.data()` pointer INTO it, so an under-count relocates the Array mid-build and dangles every earlier plan pointer.
    // A scene pass that will SKIP its plan (mrt>=2, below) is still counted — over-reserving a slot is harmless; the
    // reserve must be an UPPER bound on the pushes.
    crd::u32 nfs = 0U;
    for (crd::usize i = 0; i < desc.passes.size(); ++i)
    {
        if (pass_is_fullscreen(desc.passes[i]) || pass_is_mesh_indirect(desc.passes[i]) || pass_is_tess(desc.passes[i])
            || pass_is_mesh(desc.passes[i]) || pass_is_scene_raster(desc.passes[i]))
        {
            ++nfs;
        }
    }
    if (nfs == 0U) { return true; } // no migrated pass in this asset — nothing to build
    out.storage.reserve(nfs);
    if (out.ctx == nullptr) { out.ctx = new crd::ceir::Context(out.alloc); }

    for (crd::usize pi = 0; pi < desc.passes.size(); ++pi)
    {
        const FramePassDesc& d        = desc.passes[pi];
        const bool           is_fs    = pass_is_fullscreen(d);
        const bool           is_mind  = !is_fs && pass_is_mesh_indirect(d);
        const bool           is_amp   = !is_fs && !is_mind && (pass_is_tess(d) || pass_is_mesh(d));
        const bool           is_scene = !is_fs && !is_mind && !is_amp && pass_is_scene_raster(d);
        if (!is_fs && !is_mind && !is_amp && !is_scene) { continue; }

        // ⛔ CEIR-16d-live-4a-4: the scene COLOUR-attachment count = the build_scene_ceir `mrt_n`. Count COLOUR writes exactly
        // as the record-time resolver routes them (frame_runtime L1032-1085): a buffer write is not an attachment, and a
        // depth-format write routes to depth_target (excluded) — so a velocity pass's [colour, depth] is ONE colour (mrt_n=1,
        // the single-colour scene), and a deferred G-buffer / WBOIT accumulate is mrt_n>=2. build_scene_ceir now handles
        // mrt>=2 (4a-2/3: N colour_attachment ops + emit_scene_list_mrt's per-item scope) — so NO more skip; the N-colour plan
        // is built + the pass records via record_ceir_render like every migrated scene pass (the mrt>=2-skip of 16d-live-2 is
        // INVERTED, gap iii). ⛔ ORDER: d.writes is visited in authored order excluding depth/buffer, so colour 0 = the primary
        // write (color), colour 1..3 = the extra ColorTarget writes (color1..3) — the order fs_target/record_scene_raster expect.
        crd::u32 scene_colour_writes    = 0U;
        bool     scene_first_col_is_uint = false; // ⛔ CEIR-16z-3: colour-0's format R32Uint ⇒ the visbuffer typed uint clear
        if (is_scene)
        {
            for (crd::usize w = 0; w < d.writes.size(); ++w)
            {
                const FrameResourceDesc* const res = fs_find_resource(desc, d.writes[w].name);
                if (res == nullptr || fs_read_is_buffer(res->kind) || crd::gpu::fg_format_has_depth(res->format))
                {
                    continue;
                }
                if (scene_colour_writes == 0U) { scene_first_col_is_uint = crd::gpu::fg_format_is_uint(res->format); }
                ++scene_colour_writes;
            }
        }

        out.storage.push_back(crd::containers::Array<crd::ceir::gpu::LoweredCommand>(out.alloc));
        crd::containers::Array<crd::ceir::gpu::LoweredCommand>& cmds  = out.storage[out.storage.size() - 1U];
        bool                                                   built = false;
        if (is_fs)
        {
            // ── extract the FULLSCREEN composite recipe from the pass (mirrors to_authored_pass's fullscreen payload). ──
            crd::ceir::gpu::FullscreenBuildDesc bd;
            bd.depth_as_float = pass_flag(d, crd::containers::StringView(pp::kDepthAsFloat));
            bd.shading_rate   = static_cast<crd::gpu::ShadingRate>(pass_u32(
                d, crd::containers::StringView(pp::kShadingRate), static_cast<crd::u32>(crd::gpu::ShadingRate::Rate1x1)));
            bd.conservative = static_cast<crd::gpu::ConservativeMode>(pass_u32(
                d, crd::containers::StringView(pp::kConservative), static_cast<crd::u32>(crd::gpu::ConservativeMode::Off)));
            if (pass_flag(d, crd::containers::StringView(pp::kComposite)))
            {
                bd.load  = true;
                bd.blend = static_cast<crd::gpu::BlendMode>(pass_u32(
                    d, crd::containers::StringView(pp::kBlendSlot[0]), static_cast<crd::u32>(crd::gpu::BlendMode::Alpha)));
            }
            crd::u32 ni = 0U;
            for (crd::usize r = 0; r < d.reads.size(); ++r)
            {
                const FrameResourceDesc* const res = fs_find_resource(desc, d.reads[r].name);
                if (res != nullptr && fs_read_is_buffer(res->kind))
                {
                    bd.constants_param = rp::pass_param_id(crd::containers::StringView("constants"));
                }
                else if (ni < 8U)
                {
                    // a texture read → input{ni}. is_depth from the resource FORMAT — the atlas signal the builder pairs with
                    // !depth_as_float; an external/unfound resource defaults to colour (is_depth = false).
                    bd.inputs[ni].source_param = rp::pass_param_id(crd::containers::StringView(kIn[ni]));
                    bd.inputs[ni].is_depth     = res != nullptr && crd::gpu::fg_format_has_depth(res->format);
                    ++ni;
                }
            }
            bd.num_inputs = ni;
            built         = crd::ceir::gpu::build_fullscreen_ceir(*out.ctx, bd, cmds);
        }
        else if (is_mind)
        {
            // ── CEIR-16-mesh-1: the MESH-INDIRECT composite (record_mesh_indirect) — the %args buffer at slot "args", its
            // byte offset, and the pass clear colour (a mesh dispatch can leave uncovered pixels — carry the real clear). ──
            crd::ceir::gpu::MeshIndirectBuildDesc mbd;
            mbd.args_param  = rp::pass_param_id(crd::containers::StringView("args"));
            mbd.args_offset = pass_u32(d, crd::containers::StringView("args_offset"), 0U);
            float cc[4] = {0.0F, 0.0F, 0.0F, 1.0F};
            pass_vec4(d, SV(pp::kClearColor), cc);
            mbd.clear = crd::gpu::ClearColor{cc[0], cc[1], cc[2], cc[3]};
            built     = crd::ceir::gpu::build_mesh_indirect_ceir(*out.ctx, mbd, cmds);
        }
        else if (is_amp)
        {
            // ── CEIR-16-mesh-2: the AMPLIFY composite (record_amplify_raster) — mesh.raster (meshlet) or tess.raster
            // (patches); the record-time walk expands mesh_dispatch_list over ctx.draws(). fallback_count = the procedural
            // amplify_count arm; carry the clear colour (a dispatch can leave uncovered pixels). ──
            crd::ceir::gpu::AmplifyBuildDesc abd;
            abd.patches = pass_is_tess(d); // tess.raster -> patches; mesh.raster -> meshlet
            // ⛔ CEIR-16-mesh-2: the amplify count is AUTHORED as `groups` (mesh.raster) / `patches` (tess.raster) — the
            // recorder maps BOTH to rec.amplify_count (this file, ~L876), but the DESC carries the authored name. Read
            // whichever is present (mirror the recorder), NOT the schema's "amplify_count": the shipped scene_mesh/scene_tess
            // use groups/patches, so reading "amplify_count" gave 0 and every amplify pass rendered BLACK (flag-ON sweep bug).
            crd::u32 amp = pass_u32(d, crd::containers::StringView("groups"), 0U);
            if (amp == 0U) { amp = pass_u32(d, crd::containers::StringView("patches"), 0U); }
            if (amp == 0U) { amp = pass_u32(d, crd::containers::StringView("amplify_count"), 0U); }
            abd.fallback_count = amp;
            float cc[4] = {0.0F, 0.0F, 0.0F, 1.0F};
            pass_vec4(d, SV(pp::kClearColor), cc);
            abd.clear = crd::gpu::ClearColor{cc[0], cc[1], cc[2], cc[3]};
            built     = crd::ceir::gpu::build_amplify_ceir(*out.ctx, abd, cmds);
        }
        else // is_scene (single-colour): the §128 scene.raster template — build_scene_ceir mirrors record_scene_raster's
        {    // begin/scene_draw_list/end. The colour + depth attachments are TEMPLATES fs_target resolves at record.
            crd::ceir::gpu::SceneBuildDesc sbd;
            const bool                     depth_only = pass_flag(d, SV(pp::kDepthOnly));
            sbd.has_color = !depth_only;                    // a depth-only shadow cascade / prepass has NO colour attachment
            sbd.has_depth = true;                           // ALWAYS a template; fs_target drops it when the target has no depth
            // ⛔ BASE load ONLY. `load_override` is per-for_each-INSTANCE and FRAME-VARYING (cascade caching REN-40-E2), so a
            // static plan cannot bake it — it is OR-ed into the LoadOps at record (16d-live-2b). Depth LOADs when colour does
            // (legacy `load_depth = load || load_depth`), so the base depth load = kLoad || kLoadDepth.
            sbd.load       = pass_flag(d, SV(pp::kLoad));
            sbd.load_depth = pass_flag(d, SV(pp::kLoad)) || pass_flag(d, SV(pp::kLoadDepth));
            float cc[4] = {0.0F, 0.0F, 0.0F, 1.0F};
            pass_vec4(d, SV(pp::kClearColor), cc);
            sbd.clear         = crd::gpu::ClearColor{cc[0], cc[1], cc[2], cc[3]};
            sbd.clear_depth   = pass_f32(d, SV(pp::kClearDepth), 1.0F);
            sbd.depth_compare = static_cast<crd::gpu::DepthCompare>(
                pass_u32(d, SV(pp::kDepthCompare), static_cast<crd::u32>(crd::gpu::DepthCompare::LessEqual)));
            // ⛔ CEIR-16z-3 (§41 visbuffer dissolution): the procedural + typed-uint-clear role. `procedural` ⇒ emit_scene_list
            // emits gl_VertexIndex draws (GeometryKind::None, no storage). `clear_is_uint` is DERIVED from an R32Uint colour
            // target (RAH-1a.1 "visibility is an ordinary typed attachment", NOT an author boolean); clear_uint = the authored
            // background id. The cook contract (VisbufferNeedsUintTarget) already guaranteed a declared clear_id ⇒ R32Uint.
            sbd.procedural    = pass_flag(d, SV(pp::kProcedural));
            sbd.clear_is_uint = scene_first_col_is_uint;
            sbd.clear_uint    = pass_u32(d, SV("clear_id"), 0U);
            // ⛔ CEIR-16d-live-4a-4: the MULTI-COLOUR MRT set. mrt_n = the COLOUR-write count (1 = single-colour, byte-identical;
            // >=2 = a deferred G-buffer / WBOIT). Per-attachment blends read from blend0..3 — the SAME the to_authored_pass scene
            // MRT arm reads (this file ~L232), so record_scene_raster's payload blends and the baked CEIR blends AGREE. A
            // depth-only pass has 0 colour writes ⇒ mrt_n clamps to 1 but has_color=false emits no colour attachment anyway.
            sbd.mrt_n = scene_colour_writes >= 1U ? scene_colour_writes : 1U; // build_scene_ceir clamps to kMaxColorAttachments
            if (sbd.mrt_n >= 2U)
            {
                for (crd::u32 bk = 0; bk < sbd.mrt_n && bk < 4U; ++bk)
                {
                    sbd.blend[bk] = static_cast<crd::gpu::BlendMode>(
                        pass_u32(d, SV(pp::kBlendSlot[bk]), static_cast<crd::u32>(crd::gpu::BlendMode::Opaque)));
                }
            }
            built = crd::ceir::gpu::build_scene_ceir(*out.ctx, sbd, cmds);
        }
        if (!built)
        {
            diags.error(crd::renderasset::DiagCode::AssetCookFailed,
                        crd::containers::StringView("CEIR-16: build_*_ceir failed for a migrated pass"),
                        crd::containers::StringView(d.name.c_str(), d.name.size()));
            return false;
        }
        out.table.bind(rp::pass_param_id(crd::containers::StringView(d.name.c_str(), d.name.size())),
                       crd::rendergraph::CeirPassPlan{out.ctx, cmds.data(), static_cast<crd::u32>(cmds.size())});
    }
    return true;
}

} // namespace crd::framecook
