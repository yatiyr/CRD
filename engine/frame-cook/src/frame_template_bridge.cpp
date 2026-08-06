// RAF-8 (ADR-0106): FrameGraphDesc -> render-graph FrameGraphTemplate. See the header for the contract.

#include <crd/framecook/frame_template_bridge.hpp>

namespace crd::framecook
{
namespace
{
namespace rp = crd::renderpass;
namespace rg = crd::rendergraph;
using crd::renderasset::DiagCode;
using crd::renderasset::DiagnosticList;
using SV = crd::containers::StringView;

SV view_of(const crd::containers::String& s) noexcept { return SV(s.c_str(), s.size()); }
crd::u64 name_hash(const crd::containers::String& s) noexcept { return rp::pass_param_id(view_of(s)); }
crd::u64 slot_id(const char* s) noexcept { return rp::pass_param_id(SV(s)); }
bool name_is(const crd::containers::String& s, const char* lit) noexcept { return view_of(s) == SV(lit); }

// ── typed-value builders (the payload params) ──
rp::TypedValue tv_u32(crd::u32 v) noexcept
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::U32;
    t.u = v;
    return t;
}
rp::TypedValue tv_f32(float v) noexcept
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::F32;
    t.f = v;
    return t;
}
rp::TypedValue tv_bool(bool v) noexcept
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::Bool;
    t.b = v;
    return t;
}
rp::TypedValue tv_enum(crd::u32 v) noexcept
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::Enum;
    t.e = v;
    return t;
}
rp::TypedValue tv_vec4(const float v[4]) noexcept
{
    rp::TypedValue t;
    t.type = rp::ExecutorParamType::Vec4;
    t.v4[0] = v[0];
    t.v4[1] = v[1];
    t.v4[2] = v[2];
    t.v4[3] = v[3];
    return t;
}

// Which family a pass kind belongs to — the executor + its queue. Empty name ⇒ no mapping yet (a NAMED diagnostic).
struct Mapping
{
    const char* executor = nullptr;
    rp::QueueKind queue = rp::QueueKind::Graphics;
};
Mapping map_kind(FramePassKind k) noexcept
{
    switch (k)
    {
    case FramePassKind::RasterGeometry:
    case FramePassKind::RasterDepthOnly:
    case FramePassKind::RasterMrt:
        return {"scene.raster", rp::QueueKind::Graphics};
    case FramePassKind::RasterVisbuffer:
        return {"visbuffer.raster", rp::QueueKind::Graphics};
    case FramePassKind::RasterFullscreen:
    case FramePassKind::RasterComposite:
        return {"fullscreen.raster", rp::QueueKind::Graphics};
    case FramePassKind::Compute:
    case FramePassKind::ComputeIndirect:
        return {"compute.dispatch", rp::QueueKind::Compute};
    case FramePassKind::Clear:
        return {"transfer.clear", rp::QueueKind::Transfer};
    case FramePassKind::Copy:
        return {"transfer.copy", rp::QueueKind::Transfer};
    case FramePassKind::Blit:
        return {"transfer.blit", rp::QueueKind::Transfer};
    case FramePassKind::Resolve:
        return {"transfer.resolve", rp::QueueKind::Transfer};
    case FramePassKind::RayTrace:
        return {"raytrace.dispatch", rp::QueueKind::Compute}; // an INLINE ray query (dispatch_kernel_rt)
    case FramePassKind::RayTracePipeline:
        return {"raytrace.pipeline", rp::QueueKind::Compute}; // an SBT trace (trace_rays / _anyhit / _full)
    case FramePassKind::RasterTess:
        return {"tess.raster", rp::QueueKind::Graphics};
    case FramePassKind::RasterMesh:
        return {"mesh.raster", rp::QueueKind::Graphics};
    case FramePassKind::RasterMeshIndirect:
        return {"mesh.indirect", rp::QueueKind::Graphics};
    case FramePassKind::Present:
        return {"present", rp::QueueKind::Graphics};
    default:
        return {nullptr, rp::QueueKind::Graphics};
    }
}

// A pass's amplification COUNT (the `groups` of a mesh pass, the `patches` of a tess pass) — the procedural draw count
// when there is no draw list. Zero if the pass declares neither (a purely draw-list-driven amplification).
crd::u32 amplify_count_of(const FramePassDesc& d) noexcept
{
    for (crd::usize i = 0; i < d.params.size(); ++i)
    {
        const FrameParam& p = d.params[i];
        if (name_is(p.name, "groups") || name_is(p.name, "patches")) { return static_cast<crd::u32>(p.v[0]); }
    }
    return 0U;
}

// A resource's authored FrameResourceKind (the INDIRECT_ARGS distinction the SlotResourceKind erases — an indirect-args
// buffer and an ordinary storage buffer are both StorageBuffer slots, but only the former feeds an indirect dispatch).
FrameResourceKind res_kind(const FrameGraphDesc& desc, crd::u64 h) noexcept
{
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        if (name_hash(desc.resources[i].name) == h) { return desc.resources[i].kind; }
    }
    return FrameResourceKind::TransientBuffer;
}

bool is_buffer(FrameResourceKind k) noexcept
{
    return k == FrameResourceKind::TransientBuffer || k == FrameResourceKind::IndirectArgs
           || k == FrameResourceKind::ExternalBuffer || k == FrameResourceKind::StructuredBuffer
           || k == FrameResourceKind::CounterBuffer;
}

bool is_depth_format(crd::gpu::FgImageFormat f) noexcept
{
    return f == crd::gpu::FgImageFormat::D32Float || f == crd::gpu::FgImageFormat::D24S8
           || f == crd::gpu::FgImageFormat::D32FloatS8;
}

rp::SlotResourceKind resource_slot_kind(const FrameResourceDesc& r) noexcept
{
    if (r.kind == FrameResourceKind::AccelerationStructure)
    {
        return rp::SlotResourceKind::AccelStructure;
    }
    if (is_buffer(r.kind))
    {
        return rp::SlotResourceKind::StorageBuffer;
    }
    // A DEPTH-format image is a DepthTarget resource — so a pass WRITING it routes to the `depth` slot (the depth
    // prepass writes scene_depth alongside a velocity COLOUR), while a colour image routes to `color*`.
    return is_depth_format(r.format) ? rp::SlotResourceKind::DepthTarget : rp::SlotResourceKind::ColorTarget;
}

rg::ResourceLifetime resource_lifetime(FrameResourceKind k) noexcept
{
    switch (k)
    {
    case FrameResourceKind::PersistentImage:
        return rg::ResourceLifetime::Persistent;
    case FrameResourceKind::PingPongImage:
        return rg::ResourceLifetime::History;
    // Host-owned imports are never aliased — pin them (Persistent).
    case FrameResourceKind::ExternalTexture:
    case FrameResourceKind::ExternalBuffer:
    case FrameResourceKind::AccelerationStructure:
        return rg::ResourceLifetime::Persistent;
    default:
        return rg::ResourceLifetime::Transient;
    }
}

// A stable class grouping transients that MAY share a physical slot (same shape + non-overlapping lifetime). Absolute
// or scale-relative extent, format, samples, layers for images; the byte size for buffers.
crd::u64 size_class_of(const FrameResourceDesc& r) noexcept
{
    crd::u64 h = 1469598103934665603ULL; // FNV-1a offset
    const auto mix = [&h](crd::u64 v)
    {
        h ^= v;
        h *= 1099511628211ULL;
    };
    if (is_buffer(r.kind))
    {
        const crd::u64 bytes = r.size_bytes != 0U ? r.size_bytes : (static_cast<crd::u64>(r.stride) * r.count);
        mix(0x0B0FU);
        mix(bytes);
        return h;
    }
    mix(0x1A6EU);
    mix(static_cast<crd::u64>(r.format));
    mix(static_cast<crd::u64>(r.samples));
    mix(static_cast<crd::u64>(r.layers));
    // scale-relative extent (width 0, scale > 0) classes by scale; an absolute extent classes by (w,h).
    if (r.width == 0U && r.scale > 0.0F)
    {
        mix(0x5CA1EU);
        mix(static_cast<crd::u64>(r.scale * 4096.0F));
    }
    else
    {
        mix(static_cast<crd::u64>(r.width));
        mix(static_cast<crd::u64>(r.height));
    }
    return h;
}

// Push a resource ref onto the payload, failing loudly if the pass overflows the executor's slot table.
bool push_ref(rp::PassPayload& pl, crd::u64 slot, rp::SlotResourceKind kind, rp::SlotAccess access, crd::u64 res,
              const crd::containers::String& pass_name, DiagnosticList& diags)
{
    if (pl.resources.size() >= rp::kMaxExecutorSlots)
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                   "a pass exceeds the executor slot capacity (too many reads/writes for its kind)",
                   view_of(pass_name));
        return false;
    }
    pl.resources.push_back(rp::ResourceRef{slot, kind, access, res});
    return true;
}

// The colour/MRT slot names, in order. color0 == "color".
const char* color_slot(crd::u32 i) noexcept
{
    switch (i)
    {
    case 0:
        return "color";
    case 1:
        return "color1";
    case 2:
        return "color2";
    default:
        return "color3";
    }
}
// The per-attachment BLEND param names, in colour-attachment order (blend0 pairs `color`, blend1..3 pair color1..3).
// ⭐ RAF-12.2: a geometry MRT (the WBOIT accumulate) carries per-attachment blend as DATA the scene.raster executor reads.
const char* blend_slot(crd::u32 i) noexcept
{
    switch (i)
    {
    case 0:
        return "blend0";
    case 1:
        return "blend1";
    case 2:
        return "blend2";
    default:
        return "blend3";
    }
}
const char* input_slot(crd::u32 i) noexcept
{
    switch (i)
    {
    case 0:
        return "input0";
    case 1:
        return "input1";
    case 2:
        return "input2";
    case 3:
        return "input3";
    case 4:
        return "input4";
    case 5:
        return "input5";
    case 6:
        return "input6";
    default:
        return "input7";
    }
}
const char* storage_slot(crd::u32 i) noexcept
{
    switch (i)
    {
    case 0:
        return "storage";
    case 1:
        return "storage1";
    case 2:
        return "storage2";
    default:
        return "storage3";
    }
}

// Look up a declared resource's kind (after the resource pass populated `out`). Missing ⇒ ColorTarget (an external,
// auto-declared below).
rp::SlotResourceKind kind_of(const rg::FrameGraphTemplate& out, crd::u64 res) noexcept
{
    const rg::GraphResource* r = out.find_resource(res);
    return r != nullptr ? r->kind : rp::SlotResourceKind::ColorTarget;
}

// ── the two RASTER families: writes -> colour/depth, reads -> input* (declared for SCHEDULING; the record fn binds
// the sampler off the resolved DrawList). Caps at the schema's slot fan-out; overflow is a NAMED diagnostic. ──
bool map_raster(const FramePassDesc& d, bool fullscreen, rp::PassPayload& pl, const rg::FrameGraphTemplate& out,
                DiagnosticList& diags)
{
    // params: clear colour / depth / compare / load are scene.raster's; fullscreen carries VRS / conservative / blend.
    if (fullscreen)
    {
        if (d.shading_rate != crd::gpu::ShadingRate::Rate1x1)
        {
            pl.params.push_back(rp::ParamValue{slot_id("shading_rate"), tv_enum(static_cast<crd::u32>(d.shading_rate))});
        }
        if (d.conservative != crd::gpu::ConservativeMode::Off)
        {
            pl.params.push_back(rp::ParamValue{slot_id("conservative"), tv_enum(static_cast<crd::u32>(d.conservative))});
        }
        if (d.depth_as_float)
        {
            pl.params.push_back(rp::ParamValue{slot_id("depth_as_float"), tv_bool(true)});
        }
        // ⭐ RAF-8: the COMPOSITE shape (WBOIT resolve) — LOAD the target + BLEND the bindless draw over it
        // (draw_bindless_blend_load), so the background the OIT resolve reads survives.
        if (d.kind == FramePassKind::RasterComposite)
        {
            pl.params.push_back(rp::ParamValue{slot_id("load"), tv_bool(true)});
            const crd::gpu::BlendMode bm = d.blend.size() > 0U ? d.blend[0] : crd::gpu::BlendMode::Alpha;
            pl.params.push_back(rp::ParamValue{slot_id("blend"), tv_enum(static_cast<crd::u32>(bm))});
        }
    }
    else
    {
        pl.params.push_back(rp::ParamValue{slot_id("clear_color"), tv_vec4(d.clear_color)});
        pl.params.push_back(rp::ParamValue{slot_id("clear_depth"), tv_f32(d.clear_depth)});
        pl.params.push_back(rp::ParamValue{slot_id("depth_compare"), tv_enum(static_cast<crd::u32>(d.depth))});
        if (d.load_target)
        {
            pl.params.push_back(rp::ParamValue{slot_id("load"), tv_bool(true)});
        }
        if (d.load_depth)
        {
            pl.params.push_back(rp::ParamValue{slot_id("load_depth"), tv_bool(true)});
        }
    }

    crd::u32 color_i = 0U;
    bool has_depth = false;
    for (crd::usize i = 0; i < d.writes.size(); ++i)
    {
        const crd::u64 res = name_hash(d.writes[i].name);
        // A DEPTH-format write (a shadow cascade, a depth prepass — and the depth half of an MRT prepass) → the `depth`
        // slot; a colour write (incl. the velocity target of an MRT prepass) → `color*`. Routed by the resource's
        // kind, so `raster.depth_only` and the depth half of `raster.mrt` share ONE path. The `depth` slot is
        // ReadWrite (a depth test reads + writes); a first-writer prepass has nothing earlier to order against.
        if (kind_of(out, res) == rp::SlotResourceKind::DepthTarget)
        {
            if (!push_ref(pl, slot_id("depth"), rp::SlotResourceKind::DepthTarget, rp::SlotAccess::ReadWrite, res,
                          d.name, diags))
            {
                return false;
            }
            has_depth = true;
            continue;
        }
        if (fullscreen && color_i > 0U)
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                       "a fullscreen pass declares more than one colour write (MRT is a geometry-pass shape)",
                       view_of(d.name));
            return false;
        }
        if (color_i > 3U)
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                       "a geometry pass declares more than 4 colour writes", view_of(d.name));
            return false;
        }
        if (!push_ref(pl, slot_id(color_slot(color_i)), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write, res,
                      d.name, diags))
        {
            return false;
        }
        // ⭐ RAF-12.2: this colour attachment's per-attachment BLEND (fullscreen carries its single composite blend
        // separately above). Only emit when the pass DECLARED one for this index — an absent entry leaves the executor
        // on Opaque (the ordinary opaque geometry pass), so the velocity prepass + every existing frame are unchanged.
        if (!fullscreen && color_i < d.blend.size())
        {
            pl.params.push_back(
                rp::ParamValue{slot_id(blend_slot(color_i)), tv_enum(static_cast<crd::u32>(d.blend[color_i]))});
        }
        ++color_i;
    }
    // a separate depth image (REN-40-G3 shared_depth) the pass depth-tests against — only when the pass did not
    // already route a depth WRITE (the forward pass writes colour + depth-tests scene_depth; a prepass writes depth).
    if (!has_depth && !d.shared_depth.empty())
    {
        if (!push_ref(pl, slot_id("depth"), rp::SlotResourceKind::DepthTarget, rp::SlotAccess::ReadWrite,
                      name_hash(d.shared_depth), d.name, diags))
        {
            return false;
        }
    }

    // reads -> input* (Texture, Read), purely for barrier ordering (the shadow atlas write-before-read). A read that
    // resolves to a BUFFER is not a sampled texture: on a FULLSCREEN pass it is the per-frame constants (b0); on a
    // GEOMETRY pass it is a GPU-cull command buffer (`instances`/`cull_args`) declared for the write-before-read edge.
    const crd::u32 max_inputs = fullscreen ? 8U : 4U;
    crd::u32 in_i = 0U;
    crd::u32 rbuf_i = 0U;
    for (crd::usize i = 0; i < d.reads.size(); ++i)
    {
        const crd::u64 res = name_hash(d.reads[i].name);
        if (kind_of(out, res) == rp::SlotResourceKind::StorageBuffer)
        {
            if (fullscreen)
            {
                if (!push_ref(pl, slot_id("constants"), rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, res,
                              d.name, diags))
                {
                    return false;
                }
            }
            else
            {
                if (rbuf_i > 1U)
                {
                    diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                               "a geometry pass declares more than 2 scheduling buffer reads", view_of(d.name));
                    return false;
                }
                const char* slot = rbuf_i == 0U ? "read_buffer0" : "read_buffer1";
                if (!push_ref(pl, slot_id(slot), rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, res, d.name,
                              diags))
                {
                    return false;
                }
                ++rbuf_i;
            }
            continue;
        }
        if (in_i >= max_inputs)
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                       "a raster pass declares more sampled reads than the executor has input slots", view_of(d.name));
            return false;
        }
        if (!push_ref(pl, slot_id(input_slot(in_i)), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, res, d.name,
                      diags))
        {
            return false;
        }
        ++in_i;
    }
    return true;
}

bool map_compute(const FramePassDesc& d, const FrameGraphDesc& desc, rp::PassPayload& pl,
                 const rg::FrameGraphTemplate& out, DiagnosticList& diags)
{
    // the grid: from authored params if present, else 1 (a kernel-driven grid the host overrides at record).
    crd::u32 gx = 1U;
    crd::u32 gy = 1U;
    crd::u32 gz = 1U;
    for (crd::usize i = 0; i < d.params.size(); ++i)
    {
        const FrameParam& p = d.params[i];
        if (name_is(p.name, "groups_x")) { gx = static_cast<crd::u32>(p.v[0]); }
        else if (name_is(p.name, "groups_y")) { gy = static_cast<crd::u32>(p.v[0]); }
        else if (name_is(p.name, "groups_z")) { gz = static_cast<crd::u32>(p.v[0]); }
    }
    pl.params.push_back(rp::ParamValue{slot_id("groups_x"), tv_u32(gx)});
    pl.params.push_back(rp::ParamValue{slot_id("groups_y"), tv_u32(gy)});
    pl.params.push_back(rp::ParamValue{slot_id("groups_z"), tv_u32(gz)});

    // every buffer the kernel touches (writes then reads) → storage slots (schema ReadWrite; over-declared access is
    // safe — it only adds ordering). An `untracked_storage` read is deliberately NOT tracked (avoids a cull cycle).
    crd::u32 s_i = 0U;
    const auto put = [&](const crd::containers::String& n) -> bool
    {
        if (s_i > 3U)
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                       "a compute pass touches more than 4 buffers", view_of(d.name));
            return false;
        }
        const bool ok = push_ref(pl, slot_id(storage_slot(s_i)), rp::SlotResourceKind::StorageBuffer,
                                 rp::SlotAccess::ReadWrite, name_hash(n), d.name, diags);
        ++s_i;
        return ok;
    };
    for (crd::usize i = 0; i < d.writes.size(); ++i)
    {
        if (!put(d.writes[i].name)) { return false; }
    }
    for (crd::usize i = 0; i < d.reads.size(); ++i)
    {
        const crd::u64 res = name_hash(d.reads[i].name);
        // ⭐ RAF-8: a ComputeIndirect pass reads its INDIRECT ARGS buffer (the {x,y,z} count a cull pass wrote) → the
        // `args` slot (dispatch_kernel_indirect), NOT a storage slot. ⛔ Only for a ComputeIndirect pass: a plain
        // Compute pass reading an indirect-args buffer (the gpu-cull `cull` reading `cull_args`) is an ordinary storage
        // read — the DISPATCH args is what makes it the args slot, and only ComputeIndirect dispatches indirectly.
        if (d.kind == FramePassKind::ComputeIndirect && res_kind(desc, res) == FrameResourceKind::IndirectArgs)
        {
            if (!push_ref(pl, slot_id("args"), rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, res, d.name,
                          diags))
            {
                return false;
            }
            continue;
        }
        // ⭐ RAF-8 gap (b): a read that is NOT a storage buffer is an IMAGE the kernel SAMPLES (the HZB `occlusion_cull`
        // reads) → the `sampled` slot, bound via dispatch_kernel_sampled.
        if (kind_of(out, res) != rp::SlotResourceKind::StorageBuffer)
        {
            if (!push_ref(pl, slot_id("sampled"), rp::SlotResourceKind::Texture, rp::SlotAccess::Read, res, d.name, diags))
            {
                return false;
            }
            continue;
        }
        // an `untracked_storage` read is deliberately NOT tracked (avoids a cull cycle); a buffer read otherwise
        // takes a storage slot for the write-before-read edge.
        if (!d.untracked_storage)
        {
            if (!put(d.reads[i].name)) { return false; }
        }
    }
    return true;
}

bool map_transfer(const FramePassDesc& d, rp::PassPayload& pl, DiagnosticList& diags)
{
    if (d.kind == FramePassKind::Clear)
    {
        pl.params.push_back(rp::ParamValue{slot_id("clear_color"), tv_vec4(d.clear_color)});
        if (d.writes.empty())
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot, "a clear pass declares no target",
                       view_of(d.name));
            return false;
        }
        return push_ref(pl, slot_id("target"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write,
                        name_hash(d.writes[0].name), d.name, diags);
    }
    // copy / blit / resolve: src -> dst.
    if (d.kind == FramePassKind::Blit)
    {
        pl.params.push_back(rp::ParamValue{slot_id("filter"), tv_enum(static_cast<crd::u32>(d.filter))});
    }
    if (d.reads.empty() || d.writes.empty())
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                   "a copy/blit/resolve pass needs one src read and one dst write", view_of(d.name));
        return false;
    }
    return push_ref(pl, slot_id("src"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Read,
                    name_hash(d.reads[0].name), d.name, diags)
           && push_ref(pl, slot_id("dst"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write,
                       name_hash(d.writes[0].name), d.name, diags);
}

bool map_present(const FramePassDesc& d, rp::PassPayload& pl, DiagnosticList& diags)
{
    // present READS the final image (the swapchain source). Accept it from reads[0] or, if authored as a write, writes[0].
    const crd::containers::String* src = nullptr;
    if (!d.reads.empty()) { src = &d.reads[0].name; }
    else if (!d.writes.empty()) { src = &d.writes[0].name; }
    if (src == nullptr)
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot, "a present pass names no source",
                   view_of(d.name));
        return false;
    }
    return push_ref(pl, slot_id("source"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Read, name_hash(*src),
                    d.name, diags);
}

// ── the RAY-TRACING families. Both bind the acceleration structure read + the storage buffers the shaders touch
// (writes then non-accel reads → storage · storage1..3). The inline dispatch reads groups_x/y/z; the pipeline reads
// groups_x/y (the ray-gen width × height). A ray-trace with no acceleration structure is rejected LOUDLY. ──
bool map_rt_common(const FramePassDesc& d, rp::PassPayload& pl, const rg::FrameGraphTemplate& out, bool three_groups,
                   DiagnosticList& diags)
{
    crd::u32 gx = 1U;
    crd::u32 gy = 1U;
    crd::u32 gz = 1U;
    for (crd::usize i = 0; i < d.params.size(); ++i)
    {
        const FrameParam& p = d.params[i];
        if (name_is(p.name, "groups_x")) { gx = static_cast<crd::u32>(p.v[0]); }
        else if (name_is(p.name, "groups_y")) { gy = static_cast<crd::u32>(p.v[0]); }
        else if (name_is(p.name, "groups_z")) { gz = static_cast<crd::u32>(p.v[0]); }
    }
    pl.params.push_back(rp::ParamValue{slot_id("groups_x"), tv_u32(gx)});
    pl.params.push_back(rp::ParamValue{slot_id("groups_y"), tv_u32(gy)});
    if (three_groups)
    {
        pl.params.push_back(rp::ParamValue{slot_id("groups_z"), tv_u32(gz)});
    }
    // the storage buffers: writes first, then non-accel reads (the ray-hit output + scene buffers) → storage*.
    crd::u32   s_i        = 0U;
    const auto put_storage = [&](crd::u64 res) -> bool
    {
        if (s_i > 3U)
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                       "a raytrace pass touches more than 4 storage buffers", view_of(d.name));
            return false;
        }
        const bool ok = push_ref(pl, slot_id(storage_slot(s_i)), rp::SlotResourceKind::StorageBuffer,
                                 rp::SlotAccess::ReadWrite, res, d.name, diags);
        ++s_i;
        return ok;
    };
    for (crd::usize i = 0; i < d.writes.size(); ++i)
    {
        if (!put_storage(name_hash(d.writes[i].name))) { return false; }
    }
    bool accel_found = false;
    for (crd::usize i = 0; i < d.reads.size(); ++i)
    {
        const crd::u64 res = name_hash(d.reads[i].name);
        if (kind_of(out, res) == rp::SlotResourceKind::AccelStructure)
        {
            if (!push_ref(pl, slot_id("accel"), rp::SlotResourceKind::AccelStructure, rp::SlotAccess::Read, res, d.name,
                          diags))
            {
                return false;
            }
            accel_found = true;
            continue;
        }
        if (!put_storage(res)) { return false; }
    }
    if (!accel_found)
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot,
                   "a raytrace pass names no acceleration structure", view_of(d.name));
        return false;
    }
    return true;
}

// ── visbuffer.raster: writes → the R32_UINT id target; `clear_id` param (the background id). Draws come from the
// resolved draw list (bound at record). ──
bool map_visbuffer(const FramePassDesc& d, rp::PassPayload& pl, DiagnosticList& diags)
{
    crd::u32 clear_id = 0U;
    for (crd::usize i = 0; i < d.params.size(); ++i)
    {
        if (name_is(d.params[i].name, "clear_id")) { clear_id = static_cast<crd::u32>(d.params[i].v[0]); }
    }
    pl.params.push_back(rp::ParamValue{slot_id("clear_id"), tv_u32(clear_id)});
    if (d.writes.empty())
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot, "a visbuffer pass declares no id target",
                   view_of(d.name));
        return false;
    }
    return push_ref(pl, slot_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write,
                    name_hash(d.writes[0].name), d.name, diags);
}

// ── mesh.raster / tess.raster: writes → colour; `clear_color` + optional `amplify_count` (the procedural draw count).
// The per-draw amplification counts + storage-pull buffers come from the resolved draw list at record. ──
bool map_amplify(const FramePassDesc& d, rp::PassPayload& pl, DiagnosticList& diags)
{
    pl.params.push_back(rp::ParamValue{slot_id("clear_color"), tv_vec4(d.clear_color)});
    const crd::u32 amplify = amplify_count_of(d);
    if (amplify > 0U)
    {
        pl.params.push_back(rp::ParamValue{slot_id("amplify_count"), tv_u32(amplify)});
    }
    if (d.writes.empty())
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot, "an amplification pass declares no colour",
                   view_of(d.name));
        return false;
    }
    return push_ref(pl, slot_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write,
                    name_hash(d.writes[0].name), d.name, diags);
}

// ── mesh.indirect: writes → colour; the first buffer read is the INDIRECT ARGS (draw_mesh_indirect_buffer). ──
bool map_mesh_indirect(const FramePassDesc& d, rp::PassPayload& pl, const rg::FrameGraphTemplate& out,
                       DiagnosticList& diags)
{
    pl.params.push_back(rp::ParamValue{slot_id("clear_color"), tv_vec4(d.clear_color)});
    if (d.writes.empty())
    {
        diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot, "a mesh-indirect pass declares no colour",
                   view_of(d.name));
        return false;
    }
    if (!push_ref(pl, slot_id("color"), rp::SlotResourceKind::ColorTarget, rp::SlotAccess::Write,
                  name_hash(d.writes[0].name), d.name, diags))
    {
        return false;
    }
    for (crd::usize i = 0; i < d.reads.size(); ++i)
    {
        const crd::u64 res = name_hash(d.reads[i].name);
        if (kind_of(out, res) == rp::SlotResourceKind::StorageBuffer)
        {
            return push_ref(pl, slot_id("args"), rp::SlotResourceKind::StorageBuffer, rp::SlotAccess::Read, res, d.name,
                            diags);
        }
    }
    diags.emit(crd::renderasset::Severity::Error, DiagCode::InvalidSlot, "a mesh-indirect pass names no args buffer",
               view_of(d.name));
    return false;
}

// Declare any resource a pass REFERENCES but the asset did not declare (the `@output` canvas, a host-imported name) as
// an external persistent target — so every ref in the template resolves to a declared graph resource (compile requires
// it). Idempotent: only adds names not already present.
void declare_external(const crd::containers::String& n, rg::FrameGraphTemplate& out)
{
    const crd::u64 h = name_hash(n);
    if (out.find_resource(h) != nullptr)
    {
        return;
    }
    out.add_resource(rg::GraphResource{h, rp::SlotResourceKind::ColorTarget, rg::ResourceLifetime::Persistent, 0U});
}

} // namespace

bool build_frame_graph_template(const FrameGraphDesc& desc, ForEachCountFn for_each_count, void* user,
                                const rp::ExecutorRegistry& /*schemas*/, rg::FrameGraphTemplate& out,
                                DiagnosticList& diags)
{
    // 1. Resources -> GraphResources.
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        out.add_resource(rg::GraphResource{name_hash(r.name), resource_slot_kind(r), resource_lifetime(r.kind),
                                           size_class_of(r)});
    }
    // 1b. Auto-declare externals every pass references but the asset did not (e.g. `@output`).
    for (crd::usize pi = 0; pi < desc.passes.size(); ++pi)
    {
        const FramePassDesc& d = desc.passes[pi];
        for (crd::usize j = 0; j < d.reads.size(); ++j) { declare_external(d.reads[j].name, out); }
        for (crd::usize j = 0; j < d.writes.size(); ++j) { declare_external(d.writes[j].name, out); }
        if (!d.shared_depth.empty()) { declare_external(d.shared_depth, out); }
    }

    // 2. Passes -> GraphPasses (for_each expanded into N ordinary passes).
    bool ok = true;
    for (crd::usize pi = 0; pi < desc.passes.size(); ++pi)
    {
        const FramePassDesc& d = desc.passes[pi];
        const Mapping m = map_kind(d.kind);
        if (m.executor == nullptr)
        {
            diags.emit(crd::renderasset::Severity::Error, DiagCode::UnsupportedPassKind,
                       "a cooked pass kind has no render-graph executor mapping yet (the load bridge)", view_of(d.name));
            ok = false;
            continue;
        }
        crd::u32 instances = 1U;
        if (d.for_each != FrameForEach::None)
        {
            instances = for_each_count != nullptr ? for_each_count(d.for_each, d.for_each_arg, user) : 0U;
            if (instances == 0U)
            {
                diags.emit(crd::renderasset::Severity::Error, DiagCode::UnresolvedForEach,
                           "a for_each pass instance count did not resolve (host returned 0)", view_of(d.name));
                ok = false;
                continue;
            }
        }
        for (crd::u32 inst = 0; inst < instances; ++inst)
        {
            rg::GraphPass gp;
            // a per-instance name so N expanded passes are distinct nodes (name mixes the pass index + instance).
            gp.name_hash = name_hash(d.name) ^ (static_cast<crd::u64>(inst + 1U) * 0x9E3779B97F4A7C15ULL);
            gp.payload.executor = rp::executor_type_id(SV(m.executor));
            gp.payload.schema_version = 1U;
            gp.payload.queue = m.queue;

            bool pass_ok = true;
            switch (d.kind)
            {
            case FramePassKind::RasterGeometry:
            case FramePassKind::RasterDepthOnly:
            case FramePassKind::RasterMrt:
                pass_ok = map_raster(d, /*fullscreen*/ false, gp.payload, out, diags);
                break;
            case FramePassKind::RasterVisbuffer:
                pass_ok = map_visbuffer(d, gp.payload, diags);
                break;
            case FramePassKind::RasterFullscreen:
            case FramePassKind::RasterComposite:
                pass_ok = map_raster(d, /*fullscreen*/ true, gp.payload, out, diags);
                break;
            case FramePassKind::Compute:
            case FramePassKind::ComputeIndirect:
                pass_ok = map_compute(d, desc, gp.payload, out, diags);
                break;
            case FramePassKind::Clear:
            case FramePassKind::Copy:
            case FramePassKind::Blit:
            case FramePassKind::Resolve:
                pass_ok = map_transfer(d, gp.payload, diags);
                break;
            case FramePassKind::Present:
                pass_ok = map_present(d, gp.payload, diags);
                break;
            case FramePassKind::RayTrace:
                pass_ok = map_rt_common(d, gp.payload, out, /*three_groups*/ true, diags);
                break;
            case FramePassKind::RayTracePipeline:
                pass_ok = map_rt_common(d, gp.payload, out, /*three_groups*/ false, diags);
                break;
            case FramePassKind::RasterTess:
            case FramePassKind::RasterMesh:
                pass_ok = map_amplify(d, gp.payload, diags);
                break;
            case FramePassKind::RasterMeshIndirect:
                pass_ok = map_mesh_indirect(d, gp.payload, out, diags);
                break;
            default:
                pass_ok = false;
                break;
            }
            if (!pass_ok)
            {
                ok = false;
                break; // this pass is unmappable; stop expanding its instances
            }
            out.add_pass(gp);
        }
    }
    return ok;
}

} // namespace crd::framecook
