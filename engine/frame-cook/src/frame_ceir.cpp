#include <crd/framecook/frame_ceir.hpp>

#include <crd/ceir/frame.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/core/assert.hpp>          // CRD_ASSERT_MSG — the program-contract layer's broken-invariant guard (15c-1d-1)
#include <crd/gpu/raster_context.hpp>   // BlendMode/DepthCompare/SamplerFilter/… — the closed-vocab range-checks (15c-1d-6)

#include <cstring> // std::memcpy (AttrValue.f is a f64 bit pattern)
#include <utility> // std::move

namespace crd::framecook
{
namespace ce = crd::ceir;
using crd::containers::StringView;

namespace
{
// The 13 built-in mechanic NAMES (the strings that hash to the kExec* ids). The frame.pass `executor` SYMBOL carries the
// NAME; ExecutorTypeId is re-derived by fnv1a at the boundary (the cook==record gate). A custom pass uses p.executor.
[[nodiscard]] StringView builtin_executor_name(crd::renderpass::ExecutorTypeId id) noexcept
{
    if (id == kExecSceneRaster) { return StringView("scene.raster"); }
    if (id == kExecFullscreenRaster) { return StringView("fullscreen.raster"); }
    if (id == kExecComputeDispatch) { return StringView("compute.dispatch"); }
    if (id == kExecTransferClear) { return StringView("transfer.clear"); }
    if (id == kExecTransferCopy) { return StringView("transfer.copy"); }
    if (id == kExecTransferBlit) { return StringView("transfer.blit"); }
    if (id == kExecTransferResolve) { return StringView("transfer.resolve"); }
    if (id == kExecRaytraceDispatch) { return StringView("raytrace.dispatch"); }
    if (id == kExecRaytracePipeline) { return StringView("raytrace.pipeline"); }
    if (id == kExecTessRaster) { return StringView("tess.raster"); }
    if (id == kExecMeshRaster) { return StringView("mesh.raster"); }
    if (id == kExecMeshIndirect) { return StringView("mesh.indirect"); }
    if (id == kExecPresent) { return StringView("present"); }
    return StringView();
}
[[nodiscard]] bool kind_is_external(FrameResourceKind k) noexcept
{
    return k == FrameResourceKind::ExternalBuffer || k == FrameResourceKind::ExternalTexture
           || k == FrameResourceKind::AccelerationStructure;
}
[[nodiscard]] bool kind_is_buffer(FrameResourceKind k) noexcept
{
    return k == FrameResourceKind::TransientBuffer || k == FrameResourceKind::IndirectArgs
           || k == FrameResourceKind::ExternalBuffer || k == FrameResourceKind::StructuredBuffer
           || k == FrameResourceKind::CounterBuffer;
}
// The resource.declare `lifetime` attr for a graph-owned kind (transient/persistent/history).
[[nodiscard]] StringView kind_lifetime(FrameResourceKind k) noexcept
{
    if (k == FrameResourceKind::PersistentImage) { return StringView("persistent"); }
    if (k == FrameResourceKind::PingPongImage) { return StringView("history"); }
    return StringView("transient");
}
[[nodiscard]] StringView cull_str(FrameCullMode c) noexcept
{
    if (c == FrameCullMode::Frustum) { return StringView("frustum"); }
    if (c == FrameCullMode::FrustumOcclusion) { return StringView("frustum_occlusion"); }
    return StringView("none");
}
[[nodiscard]] StringView sort_str(FrameSortMode s) noexcept
{
    if (s == FrameSortMode::FrontToBack) { return StringView("front_to_back"); }
    if (s == FrameSortMode::BackToFront) { return StringView("back_to_front"); }
    if (s == FrameSortMode::Material) { return StringView("material"); }
    return StringView("none");
}
[[nodiscard]] StringView for_each_str(FrameForEach f) noexcept
{
    if (f == FrameForEach::LightCascades) { return StringView("light.cascades"); }
    if (f == FrameForEach::StereoViews) { return StringView("views.stereo"); }
    if (f == FrameForEach::CubeFaces) { return StringView("cube.faces"); }
    if (f == FrameForEach::ShadowCastingLights) { return StringView("lights.shadow_casting"); }
    return StringView("none");
}

// ── CEIR-15c-1d-6: the closed-vocab RANGE-CHECKS. The ceir.frame carries enum params as raw ints and from_ceir_frame copies
// them FAITHFULLY (no clamp); the desc-side parse rejects bad STRINGS, so validate_frame_graph never sees a bad int and there
// is NO oracle here (emit even NORMALIZES an out-of-range int to a default — from_sort→"none"). Each validator is a TOTAL
// switch (no `default`) so widening an enum breaks the linux-gcc `-Werror=switch` gate until this learns the new value (the
// positive use of that scar). ⛔ The `v > 0xFF` guard is MANDATORY: these enums are u8, so static_cast<E>(256) WRAPS to 0 —
// a VALID value — and the hole must be closed before the cast. ──
using ValidFn = bool (*)(crd::u32);
[[nodiscard]] bool blend_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::BlendMode>(v))
    {
    case crd::gpu::BlendMode::Opaque:
    case crd::gpu::BlendMode::Alpha:
    case crd::gpu::BlendMode::PremultipliedAlpha:
    case crd::gpu::BlendMode::Additive:
    case crd::gpu::BlendMode::Multiply:
    case crd::gpu::BlendMode::RevealageMultiply:
    case crd::gpu::BlendMode::RevealComposite: return true;
    }
    return false;
}
[[nodiscard]] bool compare_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::DepthCompare>(v))
    {
    case crd::gpu::DepthCompare::Never:
    case crd::gpu::DepthCompare::Less:
    case crd::gpu::DepthCompare::Equal:
    case crd::gpu::DepthCompare::LessEqual:
    case crd::gpu::DepthCompare::Greater:
    case crd::gpu::DepthCompare::NotEqual:
    case crd::gpu::DepthCompare::GreaterEqual:
    case crd::gpu::DepthCompare::Always: return true;
    }
    return false;
}
[[nodiscard]] bool material_pass_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<FrameMaterialPass>(v))
    {
    case FrameMaterialPass::None:
    case FrameMaterialPass::Shadow:
    case FrameMaterialPass::DepthPrepass:
    case FrameMaterialPass::GBuffer:
    case FrameMaterialPass::Forward: return true;
    }
    return false;
}
[[nodiscard]] bool filter_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::SamplerFilter>(v))
    {
    case crd::gpu::SamplerFilter::Nearest:
    case crd::gpu::SamplerFilter::Linear: return true;
    }
    return false;
}
[[nodiscard]] bool address_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::SamplerAddress>(v))
    {
    case crd::gpu::SamplerAddress::Repeat:
    case crd::gpu::SamplerAddress::ClampToEdge:
    case crd::gpu::SamplerAddress::ClampToBorder:
    case crd::gpu::SamplerAddress::Mirror: return true;
    }
    return false;
}
[[nodiscard]] bool face_cull_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::FaceCull>(v))
    {
    case crd::gpu::FaceCull::None:
    case crd::gpu::FaceCull::Back:
    case crd::gpu::FaceCull::Front: return true;
    }
    return false;
}
[[nodiscard]] bool front_face_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::FrontFace>(v))
    {
    case crd::gpu::FrontFace::CounterClockwise:
    case crd::gpu::FrontFace::Clockwise: return true;
    }
    return false;
}
[[nodiscard]] bool stencil_op_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::StencilOp>(v))
    {
    case crd::gpu::StencilOp::Keep:
    case crd::gpu::StencilOp::Zero:
    case crd::gpu::StencilOp::Replace:
    case crd::gpu::StencilOp::IncrClamp:
    case crd::gpu::StencilOp::DecrClamp:
    case crd::gpu::StencilOp::Invert:
    case crd::gpu::StencilOp::IncrWrap:
    case crd::gpu::StencilOp::DecrWrap: return true;
    }
    return false;
}
[[nodiscard]] bool shading_rate_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::ShadingRate>(v))
    {
    case crd::gpu::ShadingRate::Rate1x1:
    case crd::gpu::ShadingRate::Rate1x2:
    case crd::gpu::ShadingRate::Rate2x1:
    case crd::gpu::ShadingRate::Rate2x2:
    case crd::gpu::ShadingRate::Rate2x4:
    case crd::gpu::ShadingRate::Rate4x2:
    case crd::gpu::ShadingRate::Rate4x4: return true;
    }
    return false;
}
[[nodiscard]] bool rate_combiner_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::ShadingRateCombiner>(v))
    {
    case crd::gpu::ShadingRateCombiner::Keep:
    case crd::gpu::ShadingRateCombiner::Replace:
    case crd::gpu::ShadingRateCombiner::Min:
    case crd::gpu::ShadingRateCombiner::Max:
    case crd::gpu::ShadingRateCombiner::Mul: return true;
    }
    return false;
}
[[nodiscard]] bool conservative_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::ConservativeMode>(v))
    {
    case crd::gpu::ConservativeMode::Off:
    case crd::gpu::ConservativeMode::Overestimate:
    case crd::gpu::ConservativeMode::Underestimate: return true;
    }
    return false;
}
// The int-carried enum PARAM vocabs (blend0-3 share a validator, the three sampler filters + `filter` share one, the three
// stencil ops share one, depth+stencil compare share one). Draw-list sort/cull, the `queue` field and `format` are
// STRING-carried (from_ceir_frame normalizes a bad string to a default before we'd see it) → 15c-1d-6b checks them on the op
// attrs directly. `sampler_compare` is a bool, not an enum — excluded.
[[nodiscard]] FrameCookError enum_vocab_diag(const FramePassDesc& p) noexcept
{
    struct Check
    {
        const char*    name;
        ValidFn        valid;
        FrameCookError err;
    };
    static const Check kChecks[] = {
        {pp::kBlendSlot[0], blend_valid, FrameCookError::UnknownBlend},
        {pp::kBlendSlot[1], blend_valid, FrameCookError::UnknownBlend},
        {pp::kBlendSlot[2], blend_valid, FrameCookError::UnknownBlend},
        {pp::kBlendSlot[3], blend_valid, FrameCookError::UnknownBlend},
        {pp::kDepthCompare, compare_valid, FrameCookError::UnknownCompare},
        {pp::kStencilCompare, compare_valid, FrameCookError::UnknownCompare},
        {pp::kMaterialPass, material_pass_valid, FrameCookError::UnknownMaterialPass},
        {pp::kSamplerMin, filter_valid, FrameCookError::UnknownSamplerFilter},
        {pp::kSamplerMag, filter_valid, FrameCookError::UnknownSamplerFilter},
        {pp::kSamplerMip, filter_valid, FrameCookError::UnknownSamplerFilter},
        {pp::kFilter, filter_valid, FrameCookError::UnknownFilter},
        {pp::kSamplerAddr, address_valid, FrameCookError::UnknownSamplerAddress},
        {pp::kFaceCull, face_cull_valid, FrameCookError::UnknownFaceCull},
        {pp::kFrontFace, front_face_valid, FrameCookError::UnknownFrontFace},
        {pp::kStencilFail, stencil_op_valid, FrameCookError::UnknownStencilOp},
        {pp::kStencilDepthFail, stencil_op_valid, FrameCookError::UnknownStencilOp},
        {pp::kStencilPass, stencil_op_valid, FrameCookError::UnknownStencilOp},
        {pp::kShadingRate, shading_rate_valid, FrameCookError::UnknownShadingRate},
        {pp::kRateCombiner, rate_combiner_valid, FrameCookError::UnknownRateCombiner},
        {pp::kConservative, conservative_valid, FrameCookError::UnknownConservative},
    };
    for (const Check& c : kChecks)
    {
        const StringView nm(c.name);
        if (pass_has(p, nm) && !c.valid(pass_u32(p, nm, 0U))) { return c.err; }
    }
    return FrameCookError::Ok;
}
// Join an Array<String> of component names into `buf` as comma-separated; sets `len`.
void join_components(char* buf, crd::u32& len, crd::u32 cap, const crd::containers::Array<crd::containers::String>& items)
{
    len = 0U;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(items.size()); ++i)
    {
        if (i > 0U && len < cap) { buf[len++] = ','; }
        const crd::containers::String& s = items[i];
        for (crd::u32 j = 0; j < static_cast<crd::u32>(s.size()) && len < cap; ++j) { buf[len++] = s.data()[j]; }
    }
}
// CEIR-15c-1c-2 (Fork C-2): build the ceir.frame attr name for a param — "<pfx><param_name>" (+ ":<idx>" for a Vec4
// component). `buf` must outlive the returned view until set_attr interns it.
[[nodiscard]] StringView param_attr_name(char* buf, crd::u32 cap, StringView pfx, StringView name, crd::i32 idx)
{
    crd::u32 n = 0U;
    for (crd::u32 j = 0; j < static_cast<crd::u32>(pfx.size()) && n < cap; ++j) { buf[n++] = pfx[j]; }
    for (crd::u32 j = 0; j < static_cast<crd::u32>(name.size()) && n < cap; ++j) { buf[n++] = name[j]; }
    if (idx >= 0 && n + 2U <= cap)
    {
        buf[n++] = ':';
        buf[n++] = static_cast<char>('0' + idx);
    }
    return StringView(buf, n);
}
// The four params carried SPECIALLY (not through the generic p:/pt: loop): shader/kernel/technique are top-level SYMBOL
// attrs; draw_list is a frame.draw_list OPERAND. The generic loop skips these names.
[[nodiscard]] bool is_special_pass_param(StringView n) noexcept
{
    return n == StringView(pp::kShader) || n == StringView(pp::kKernel) || n == StringView(pp::kTechnique)
           || n == StringView(pp::kDrawList);
}
// A name -> ceir Value map (resources + draw-lists), so a pass' by-NAME reads/writes/draw_list resolve to operands.
struct NameMap
{
    static constexpr crd::u32 kMax = 256U;
    StringView                names[kMax];
    ce::Value*                vals[kMax] = {};
    crd::u32                  n          = 0U;
    void                      add(StringView nm, ce::Value* v)
    {
        if (n < kMax)
        {
            names[n] = nm;
            vals[n]  = v;
            ++n;
        }
    }
    [[nodiscard]] ce::Value* find(StringView nm) const
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (names[i] == nm) { return vals[i]; }
        }
        return nullptr;
    }
};
} // namespace

ce::Module* to_ceir_frame(const FrameGraphDesc& desc, ce::Context& ctx)
{
    // CEIR-15c 2e (RESOLVED — the PERMANENT contract, not interim): REN-37.6 composition (includes/anchors/injects) is a §39
    // NAMED-FORWARD subgraph capability that `flatten_frame_graph` resolves into plain passes BEFORE conversion — the §115
    // table routes it to PARSER ("the converter sees a FLATTENED graph"), so the analyses (15d hazards/cycle/lifetimes) never
    // need composition special-cases. PIPELINE: `parse → flatten_frame_graph → to_ceir_frame`; a desc still carrying
    // composition was NOT flattened first (a caller error) → reject. (Unlike `for_each`, which round-trips because its
    // expansion needs RUNTIME counts: frontend-resolvable ≠ runtime-deferred.)
    if (desc.includes.size() > 0U || desc.anchors.size() > 0U || desc.injects.size() > 0U) { return nullptr; }
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::frame::register_dialect(ctx);

    const ce::OpId   decl_id    = ctx.intern_op("resource", "declare");
    const ce::OpId   import_id  = ctx.intern_op("resource", "import");
    const ce::OpId   graph_id   = ctx.intern_op("frame", "graph");
    const ce::OpId   dl_id      = ctx.intern_op("frame", "draw_list");
    const ce::OpId   pass_id    = ctx.intern_op("frame", "pass");
    const ce::OpId   history_id = ctx.intern_op("frame", "history");
    const ce::TypeId f32        = ctx.type_f32();

    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m->body()->append(top);
    }
    ce::Operation* const fn = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(fn);
    ce::Block* const body = ce::func::func_body_block(fn);

    // the frame.graph container.
    ce::Operation* const g = ctx.create_operation(graph_id, {}, 0U, {}, 1U);
    if (desc.name.size() > 0U) { ctx.set_attr(g, "name", ctx.attr_symbol(StringView(desc.name.data(), desc.name.size()))); }
    ctx.set_attr(g, "schema", ctx.attr_int(static_cast<crd::i64>(desc.schema)));
    // CEIR-15c 2d: the GRAPH-LEVEL fields (capability tier / fallback graph / transient-memory ceiling) — emitted only when set.
    if (desc.requires_caps.size() > 0U)
    {
        char     capbuf[512];
        crd::u32 caplen = 0U;
        join_components(capbuf, caplen, sizeof(capbuf), desc.requires_caps); // comma-joined, split back on the way in
        ctx.set_attr(g, "requires_caps", ctx.attr_string(StringView(capbuf, caplen)));
    }
    if (desc.fallback.size() > 0U)
    {
        // a cross-asset graph reference by NAME → a SYMBOL attr (so a future CDEP dependency-extraction pass sees the fallback
        // graph as a cook dependency, like shader/kernel/technique). attr_sv reads symbols, so the backward path is unchanged.
        ctx.set_attr(g, "fallback", ctx.attr_symbol(StringView(desc.fallback.data(), desc.fallback.size())));
    }
    if (desc.memory_budget_bytes != 0U)
    {
        ctx.set_attr(g, "memory_budget_bytes", ctx.attr_int(static_cast<crd::i64>(desc.memory_budget_bytes)));
    }
    body->append(g);
    ce::Block* const rb = ctx.create_block(0U);
    g->region(0)->append(rb);

    NameMap nm;
    // resources -> resource.declare / import.
    for (crd::u32 i = 0; i < static_cast<crd::u32>(desc.resources.size()); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        const StringView         res_name(r.name.data(), r.name.size());
        const ce::TypeId         rt = kind_is_buffer(r.kind) ? ctx.type_buffer(ce::BufferMode::Plain, f32)
                                                             : ctx.type_image(ce::ImageDim::Dim2D, f32);
        ce::Operation* const     d  = ctx.create_operation(kind_is_external(r.kind) ? import_id : decl_id, {}, 1U, rt);
        ctx.set_attr(d, "name", ctx.attr_symbol(res_name)); // asset-id preservation + the backward name recovery
        if (!kind_is_external(r.kind))
        {
            ctx.set_attr(d, "lifetime", ctx.attr_string(kind_lifetime(r.kind))); // the CEIR-12b/c/d planner axis
            if (r.kind == FrameResourceKind::PingPongImage) { ctx.set_attr(d, "history_length", ctx.attr_int(1)); }
        }
        // CEIR-15a-3b: the FULL FrameResourceDesc shape as open attrs, so the backward converter recovers the EXACT kind +
        // sizing (lifetime alone is lossy: transient covers image/buffer/indirect-args/structured/counter). Enum-valued
        // fields (frame_kind/format) ride as INT (the enum value) — no format-string mapping to expose; emit_frame_toml
        // reads the reconstructed enum. Only non-default fields are set, so the round-trip stays byte-clean.
        ctx.set_attr(d, "frame_kind", ctx.attr_int(static_cast<crd::i64>(r.kind)));
        ctx.set_attr(d, "format", ctx.attr_int(static_cast<crd::i64>(r.format)));
        if (r.width != 0U) { ctx.set_attr(d, "width", ctx.attr_int(static_cast<crd::i64>(r.width))); }
        if (r.height != 0U) { ctx.set_attr(d, "height", ctx.attr_int(static_cast<crd::i64>(r.height))); }
        if (r.scale > 0.0F) { ctx.set_attr(d, "scale", ctx.attr_float(static_cast<double>(r.scale))); }
        if (r.layers != 1U) { ctx.set_attr(d, "layers", ctx.attr_int(static_cast<crd::i64>(r.layers))); }
        if (r.samples != 1U) { ctx.set_attr(d, "samples", ctx.attr_int(static_cast<crd::i64>(r.samples))); }
        if (r.mips != 1U) { ctx.set_attr(d, "mips", ctx.attr_int(static_cast<crd::i64>(r.mips))); }
        // CEIR-15c-1c-2a: the REN-38-B2 SHAPE — dimension (kind_2d: 2d/3d/cube/cube_array) + the 3-D slice `depth`. Carried
        // as the INT enum value / count (the frame_kind/format precedent), non-default only so a 2-D transient omits them
        // (round-trip byte-clean). ⛔ Before this, to_ceir_frame hardcoded Dim2D and dropped both — the round-trip-identity
        // gate never caught it because no fixture used a non-2D resource (the fixture-coverage scar). Needed by the 15c-1c-2
        // dimension verifiers (UnknownDimension/CubeNeedsSquare/VolumeNeedsDepth).
        if (r.kind_2d != crd::gpu::FgImageKind::Tex2D)
        {
            ctx.set_attr(d, "dimension", ctx.attr_int(static_cast<crd::i64>(r.kind_2d)));
        }
        if (r.depth != 1U) { ctx.set_attr(d, "depth", ctx.attr_int(static_cast<crd::i64>(r.depth))); }
        if (r.sampled) { ctx.set_attr(d, "sampled", ctx.attr_bool(true)); }
        if (r.storage) { ctx.set_attr(d, "storage", ctx.attr_bool(true)); }
        if (r.depth_buffer) { ctx.set_attr(d, "depth_buffer", ctx.attr_bool(true)); }
        if (r.no_alias) { ctx.set_attr(d, "no_alias", ctx.attr_bool(true)); }
        if (r.resizable) { ctx.set_attr(d, "resizable", ctx.attr_bool(true)); }
        if (r.stride != 0U) { ctx.set_attr(d, "stride", ctx.attr_int(static_cast<crd::i64>(r.stride))); }
        if (r.count != 0U) { ctx.set_attr(d, "count", ctx.attr_int(static_cast<crd::i64>(r.count))); }
        if (r.size_bytes != 0U) { ctx.set_attr(d, "size_bytes", ctx.attr_int(static_cast<crd::i64>(r.size_bytes))); }
        rb->append(d);
        nm.add(res_name, d->result(0U));
    }
    // draw lists -> frame.draw_list.
    char buf[512];
    for (crd::u32 i = 0; i < static_cast<crd::u32>(desc.draw_lists.size()); ++i)
    {
        const FrameDrawListDesc& dl = desc.draw_lists[i];
        ce::Operation* const     op = ctx.create_operation(dl_id, {}, 1U, ce::frame::type_draw_list(ctx));
        ctx.set_attr(op, "name", ctx.attr_symbol(StringView(dl.name.data(), dl.name.size()))); // round-trip name recovery
        crd::u32                 len = 0U;
        join_components(buf, len, sizeof(buf), dl.all);
        if (len > 0U) { ctx.set_attr(op, "all", ctx.attr_string(StringView(buf, len))); }
        join_components(buf, len, sizeof(buf), dl.any);
        if (len > 0U) { ctx.set_attr(op, "any", ctx.attr_string(StringView(buf, len))); }
        join_components(buf, len, sizeof(buf), dl.none);
        if (len > 0U) { ctx.set_attr(op, "none", ctx.attr_string(StringView(buf, len))); }
        ctx.set_attr(op, "cull", ctx.attr_string(cull_str(dl.cull)));
        ctx.set_attr(op, "sort", ctx.attr_string(sort_str(dl.sort)));
        if (dl.limit > 0U) { ctx.set_attr(op, "limit", ctx.attr_int(static_cast<crd::i64>(dl.limit))); }
        rb->append(op);
        nm.add(StringView(dl.name.data(), dl.name.size()), op->result(0U));
    }
    // CEIR-15a: IMPLICIT EXTERNAL references — a pass reads/writes a name that is NOT a declared resource or draw-list
    // (the swapchain "@output" endpoint, an "@input", ...). Mint a resource.import (an external the graph does not own);
    // ⛔ NO frame_kind attr marks it IMPLICIT, so the backward SKIPS it (emit_frame_toml never emits @output as a
    // [[resource]] — it is the frame's output endpoint, §39). Created ONCE, in reference order, before the passes.
    const auto ensure_ref = [&](StringView rn) {
        if (rn.size() > 0U && nm.find(rn) == nullptr)
        {
            ce::Operation* const im = ctx.create_operation(import_id, {}, 1U, ctx.type_image(ce::ImageDim::Dim2D, f32));
            ctx.set_attr(im, "name", ctx.attr_string(rn)); // ⛔ a STRING, not a symbol — "@output" has a '@' a CEIR symbol can't round-trip through text
            rb->append(im);
            nm.add(rn, im->result(0U));
        }
    };
    for (crd::u32 i = 0; i < static_cast<crd::u32>(desc.passes.size()); ++i)
    {
        const FramePassDesc& p = desc.passes[i];
        for (crd::u32 w = 0; w < static_cast<crd::u32>(p.writes.size()); ++w)
        {
            ensure_ref(StringView(p.writes[w].name.data(), p.writes[w].name.size()));
        }
        for (crd::u32 rd = 0; rd < static_cast<crd::u32>(p.reads.size()); ++rd)
        {
            ensure_ref(StringView(p.reads[rd].name.data(), p.reads[rd].name.size()));
        }
    }
    // ⭐ Fork B (B1): the set of lifetime=history (ping-pong) resource NAMES. A pass READ of one of these is a PREVIOUS-frame
    // read and must route through frame.history (below); a WRITE targets the declare Value directly (this frame's output).
    const auto is_history = [&](StringView rn) -> bool {
        for (crd::u32 i = 0; i < static_cast<crd::u32>(desc.resources.size()); ++i)
        {
            if (desc.resources[i].kind == FrameResourceKind::PingPongImage
                && StringView(desc.resources[i].name.data(), desc.resources[i].name.size()) == rn)
            {
                return true;
            }
        }
        return false;
    };
    // passes -> frame.pass.
    for (crd::u32 i = 0; i < static_cast<crd::u32>(desc.passes.size()); ++i)
    {
        const FramePassDesc& p = desc.passes[i];
        ce::Value*           operands[128];
        crd::u32             no = 0U;
        char                 access[256];
        crd::u32             na = 0U;
        char                 indexed_marks[128]; // CEIR-15c 2b: parallel to operands — '1' if the ref is the [$index] slice
        bool                 any_indexed = false;
        const auto           push = [&](ce::Value* v, char tok, bool idx) {
            if (v == nullptr || no >= 128U) { return false; }
            indexed_marks[no] = idx ? '1' : '0';
            operands[no++]    = v;
            if (na > 0U) { access[na++] = ','; }
            access[na++] = tok;
            if (idx) { any_indexed = true; }
            return true;
        };
        bool ok = true;
        for (crd::u32 w = 0; w < static_cast<crd::u32>(p.writes.size()); ++w)
        {
            ok = ok && push(nm.find(StringView(p.writes[w].name.data(), p.writes[w].name.size())), 'w', p.writes[w].indexed);
        }
        for (crd::u32 rd = 0; rd < static_cast<crd::u32>(p.reads.size()); ++rd)
        {
            const StringView rname(p.reads[rd].name.data(), p.reads[rd].name.size());
            const bool        ridx = p.reads[rd].indexed;
            ce::Value*        v    = nm.find(rname);
            if (v != nullptr && is_history(rname))
            {
                // ⭐ Fork B (B1): a READ of a lifetime=history (ping-pong) resource is the PREVIOUS frame — route it through
                // frame.history so its result is a DISTINCT resource_root. Otherwise a read-of-prev + a write-of-this-frame on
                // the same declare Value would derive a FALSE intra-frame RAW (the RMW scar) at the 15d hazard analysis.
                ce::Operation* const h =
                    ctx.create_operation(history_id, crd::containers::ConstSpan<ce::Value*>(&v, 1U), 1U, v->type());
                rb->append(h);
                v = h->result(0U);
            }
            ok = ok && push(v, 'r', ridx);
        }
        const StringView dln = pass_str(p, StringView(pp::kDrawList));
        if (dln.size() > 0U) { ok = ok && push(nm.find(dln), 'r', false); } // a draw-list operand is never [$index]
        if (!ok) { return nullptr; } // a pass referenced an undeclared resource/draw-list

        ce::Operation* const op = ctx.create_operation(pass_id, crd::containers::ConstSpan<ce::Value*>(operands, no), 0U);
        const StringView     ex = pass_is_custom(p) ? StringView(p.executor.data(), p.executor.size())
                                                    : builtin_executor_name(p.executor_id);
        ctx.set_attr(op, "executor", ctx.attr_symbol(ex));
        ctx.set_attr(op, "name", ctx.attr_symbol(StringView(p.name.data(), p.name.size()))); // round-trip name recovery
        ctx.set_attr(op, "access", ctx.attr_string(StringView(access, na)));
        if (any_indexed) { ctx.set_attr(op, "indexed", ctx.attr_string(StringView(indexed_marks, no))); } // 2b: per-operand [$index]
        if (p.for_each != FrameForEach::None)
        {
            ctx.set_attr(op, "for_each", ctx.attr_string(for_each_str(p.for_each)));
            if (p.for_each_arg != 0U) { ctx.set_attr(op, "for_each_arg", ctx.attr_int(static_cast<crd::i64>(p.for_each_arg))); }
        }
        if (p.queue == FrameQueue::Async) { ctx.set_attr(op, "queue", ctx.attr_string(StringView("async"))); }
        // the well-known reference params -> SYMBOL attrs (the CDEP dependency extraction generalizes at 15c).
        const StringView sh = pass_str(p, StringView(pp::kShader));
        if (sh.size() > 0U) { ctx.set_attr(op, "shader", ctx.attr_symbol(sh)); }
        const StringView kn = pass_str(p, StringView(pp::kKernel));
        if (kn.size() > 0U) { ctx.set_attr(op, "kernel", ctx.attr_symbol(kn)); }
        const StringView tc = pass_str(p, StringView(pp::kTechnique));
        if (tc.size() > 0U) { ctx.set_attr(op, "technique", ctx.attr_symbol(tc)); }
        // CEIR-15c-1c-2 (Fork C-2): the FULL RAF-12.3 param bag. Each param -> p:<name> (value, the natural attr KIND) +
        // pt:<name> (the exact FrameParamType byte, for blob fidelity — accessors are type-blind but cook stores the byte).
        // ⛔ SKIP the four carried specially (shader/kernel/technique top-level + draw_list operand) or the round-trip
        // double-emits. Vec4 rides four p:<name>:0..3 floats (CEIR has no vec4 attr). Values are f64 bit patterns (v[4] is
        // double[4]); attr_float stores the bit pattern, never a float32 detour.
        char pbuf[192];
        for (crd::u32 pp2 = 0; pp2 < static_cast<crd::u32>(p.params.size()); ++pp2)
        {
            const FrameParam& prm = p.params[pp2];
            const StringView  pn(prm.name.data(), prm.name.size());
            if (is_special_pass_param(pn)) { continue; }
            ctx.set_attr(op, param_attr_name(pbuf, sizeof(pbuf), StringView("pt:"), pn, -1),
                         ctx.attr_int(static_cast<crd::i64>(prm.type)));
            if (prm.type == FrameParamType::String)
            {
                ctx.set_attr(op, param_attr_name(pbuf, sizeof(pbuf), StringView("p:"), pn, -1),
                             ctx.attr_string(StringView(prm.str.data(), prm.str.size())));
            }
            else if (prm.type == FrameParamType::Bool)
            {
                ctx.set_attr(op, param_attr_name(pbuf, sizeof(pbuf), StringView("p:"), pn, -1), ctx.attr_bool(prm.v[0] != 0.0));
            }
            else if (prm.type == FrameParamType::Vec4)
            {
                for (crd::i32 c = 0; c < 4; ++c)
                {
                    ctx.set_attr(op, param_attr_name(pbuf, sizeof(pbuf), StringView("p:"), pn, c), ctx.attr_float(prm.v[c]));
                }
            }
            else // Float / Int / Enum / U32 — the v[0] family
            {
                ctx.set_attr(op, param_attr_name(pbuf, sizeof(pbuf), StringView("p:"), pn, -1), ctx.attr_float(prm.v[0]));
            }
        }
        rb->append(op);
    }
    return m;
}

// ── CEIR-15a-3b: the BACKWARD converter (ceir.frame -> FrameGraphDesc). ──
namespace
{
// The FIRST frame.graph op anywhere in the module (walk regions).
[[nodiscard]] const ce::Operation* find_graph(const ce::Context& ctx, const ce::Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return nullptr; }
    for (const ce::Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const ce::Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == StringView("frame.graph")) { return op; }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                if (const ce::Operation* g = find_graph(ctx, op->region(i))) { return g; }
            }
        }
    }
    return nullptr;
}
// ⛔ Each reader checks op->attr(name).valid() FIRST: attr_value(an INVALID AttrId) returns a DEFAULT AttrValue whose
// kind is Int / i == 0 — so a naive `kind==Int ? i : def` would read an ABSENT attr as 0, not the caller's default (the
// layers/mips/samples round-trip bug: absent ⇒ default 1, never 0).
[[nodiscard]] StringView attr_sv(const ce::Context& ctx, const ce::Operation* op, StringView name)
{
    const ce::AttrId a = op->attr(name);
    if (!a.valid()) { return StringView(); }
    const ce::AttrValue v = ctx.attr_value(a);
    return (v.kind == ce::AttrKind::SymbolRef || v.kind == ce::AttrKind::String) ? v.s : StringView();
}
[[nodiscard]] crd::i64 attr_i(const ce::Context& ctx, const ce::Operation* op, StringView name, crd::i64 def)
{
    const ce::AttrId a = op->attr(name);
    if (!a.valid()) { return def; }
    const ce::AttrValue v = ctx.attr_value(a);
    return v.kind == ce::AttrKind::Int ? v.i : def;
}
[[nodiscard]] bool attr_b(const ce::Context& ctx, const ce::Operation* op, StringView name)
{
    const ce::AttrId a = op->attr(name);
    if (!a.valid()) { return false; }
    const ce::AttrValue v = ctx.attr_value(a);
    return v.kind == ce::AttrKind::Bool && v.b;
}
[[nodiscard]] float attr_f(const ce::Context& ctx, const ce::Operation* op, StringView name, float def)
{
    const ce::AttrId a = op->attr(name);
    if (!a.valid()) { return def; }
    const ce::AttrValue v = ctx.attr_value(a);
    if (v.kind != ce::AttrKind::Float) { return def; }
    double d = 0.0;
    std::memcpy(&d, &v.f, sizeof(d)); // AttrValue.f is the f64 BIT PATTERN, not a double
    return static_cast<float>(d);
}
// CEIR-15c-1c-2 (Fork C-2): the FULL f64 (a FrameParam's v[] is double[4]) — never narrow to float, or an authored param
// with a non-float-representable value corrupts on the round-trip.
[[nodiscard]] double attr_f64(const ce::Context& ctx, const ce::Operation* op, StringView name, double def)
{
    const ce::AttrId a = op->attr(name);
    if (!a.valid()) { return def; }
    const ce::AttrValue v = ctx.attr_value(a);
    if (v.kind != ce::AttrKind::Float) { return def; }
    double d = 0.0;
    std::memcpy(&d, &v.f, sizeof(d));
    return d;
}
void set_str(crd::containers::String& s, StringView v) { s.append(v.data(), v.size()); }
[[nodiscard]] FrameCullMode cull_from(StringView s)
{
    if (s == StringView("frustum_occlusion")) { return FrameCullMode::FrustumOcclusion; }
    if (s == StringView("none")) { return FrameCullMode::None; }
    return FrameCullMode::Frustum; // absent/frustum
}
[[nodiscard]] FrameSortMode sort_from(StringView s)
{
    if (s == StringView("front_to_back")) { return FrameSortMode::FrontToBack; }
    if (s == StringView("back_to_front")) { return FrameSortMode::BackToFront; }
    if (s == StringView("material")) { return FrameSortMode::Material; }
    return FrameSortMode::None;
}
[[nodiscard]] FrameForEach for_each_from(StringView s)
{
    if (s == StringView("light.cascades")) { return FrameForEach::LightCascades; }
    if (s == StringView("views.stereo")) { return FrameForEach::StereoViews; }
    if (s == StringView("cube.faces")) { return FrameForEach::CubeFaces; }
    if (s == StringView("lights.shadow_casting")) { return FrameForEach::ShadowCastingLights; }
    return FrameForEach::None;
}
// ── CEIR-15c-1d-6b: the STRING-carried + format vocab checks. Unlike the 6a int params (desc-based, copied faithfully), these
// enums ride as STRING attrs — sort/cull on the frame.draw_list op, for_each/queue on the frame.pass op — whose BACKWARD readers
// NORMALIZE an unknown string to a DEFAULT (sort_from→None, cull_from→Frustum), so from_ceir_frame ERASES the garbage before a
// desc-based check would see it → these check the OP ATTR directly. A string is valid iff it ROUND-TRIPS: `X_str(X_from(s)) == s`
// (garbage maps to the default enum whose canonical string ≠ the garbage). An ABSENT attr (empty) is the default → unchecked.
// `format` is INT-carried on the resource op (like 6a) → a total-switch range-check with the same u8-wrap guard.
[[nodiscard]] bool sort_str_valid(StringView s) noexcept { return s.size() == 0U || sort_str(sort_from(s)) == s; }
[[nodiscard]] bool cull_str_valid(StringView s) noexcept { return s.size() == 0U || cull_str(cull_from(s)) == s; }
[[nodiscard]] bool for_each_str_valid(StringView s) noexcept { return s.size() == 0U || for_each_str(for_each_from(s)) == s; }
[[nodiscard]] bool queue_str_valid(StringView s) noexcept { return s.size() == 0U || s == StringView("async"); } // Graphics is absent
[[nodiscard]] bool format_valid(crd::u32 v) noexcept
{
    if (v > 0xFFU) { return false; }
    switch (static_cast<crd::gpu::FgImageFormat>(v))
    {
    case crd::gpu::FgImageFormat::RGBA8Unorm:
    case crd::gpu::FgImageFormat::RGBA8Srgb:
    case crd::gpu::FgImageFormat::RGBA16F:
    case crd::gpu::FgImageFormat::R16F:
    case crd::gpu::FgImageFormat::R32F:
    case crd::gpu::FgImageFormat::R32Uint:
    case crd::gpu::FgImageFormat::D32Float:
    case crd::gpu::FgImageFormat::RG16F:
    case crd::gpu::FgImageFormat::RG32F:
    case crd::gpu::FgImageFormat::RGBA32F:
    case crd::gpu::FgImageFormat::R11G11B10F:
    case crd::gpu::FgImageFormat::RGB10A2:
    case crd::gpu::FgImageFormat::R8:
    case crd::gpu::FgImageFormat::RG8:
    case crd::gpu::FgImageFormat::RGBA16Unorm:
    case crd::gpu::FgImageFormat::D24S8:
    case crd::gpu::FgImageFormat::D32FloatS8: return true;
    }
    return false;
}
void split_components(StringView s, crd::containers::Array<crd::containers::String>& out, crd::memory::IAllocator* a)
{
    crd::usize start = 0U;
    for (crd::usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            if (i > start)
            {
                crd::containers::String tok(a);
                tok.append(s.data() + start, i - start);
                out.push_back(std::move(tok));
            }
            start = i + 1U;
        }
    }
}
} // namespace

bool from_ceir_frame(const ce::Context& ctx, const ce::Module& m, crd::memory::IAllocator* alloc, FrameGraphDesc& out)
{
    const ce::Operation* const g = find_graph(ctx, m.body());
    if (g == nullptr) { return false; }
    out.resources.clear();
    out.draw_lists.clear();
    out.passes.clear();
    set_str(out.name, attr_sv(ctx, g, StringView("name")));
    // CEIR-15c 2d: recover the graph-level fields (absent ⇒ the desc defaults: empty caps/fallback, 0 budget).
    split_components(attr_sv(ctx, g, StringView("requires_caps")), out.requires_caps, alloc);
    set_str(out.fallback, attr_sv(ctx, g, StringView("fallback")));
    out.memory_budget_bytes = static_cast<crd::u64>(attr_i(ctx, g, StringView("memory_budget_bytes"), 0));
    out.schema = static_cast<crd::u32>(attr_i(ctx, g, StringView("schema"), static_cast<crd::i64>(kFrameSchemaVersion)));

    const ce::Region* const rg = g->region(0);
    for (const ce::Block* b = rg->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const ce::Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());
            if (nm == StringView("resource.declare") || nm == StringView("resource.import"))
            {
                // an IMPLICIT external (a resource.import with no frame_kind attr) is the @output endpoint — never a
                // [[resource]] in the desc (emit_frame_toml does not emit it); skip it on the way back.
                if (nm == StringView("resource.import") && !op->attr(StringView("frame_kind")).valid()) { continue; }
                FrameResourceDesc r(alloc);
                set_str(r.name, attr_sv(ctx, op, StringView("name")));
                r.kind        = static_cast<FrameResourceKind>(attr_i(ctx, op, StringView("frame_kind"), 0));
                r.format      = static_cast<crd::gpu::FgImageFormat>(attr_i(ctx, op, StringView("format"), 0));
                r.width       = static_cast<crd::u32>(attr_i(ctx, op, StringView("width"), 0));
                r.height      = static_cast<crd::u32>(attr_i(ctx, op, StringView("height"), 0));
                r.scale       = attr_f(ctx, op, StringView("scale"), 0.0F);
                r.layers      = static_cast<crd::u32>(attr_i(ctx, op, StringView("layers"), 1));
                r.samples     = static_cast<crd::u32>(attr_i(ctx, op, StringView("samples"), 1));
                r.mips        = static_cast<crd::u32>(attr_i(ctx, op, StringView("mips"), 1));
                r.kind_2d     = static_cast<crd::gpu::FgImageKind>(
                    attr_i(ctx, op, StringView("dimension"), static_cast<crd::i64>(crd::gpu::FgImageKind::Tex2D)));
                r.depth       = static_cast<crd::u32>(attr_i(ctx, op, StringView("depth"), 1));
                r.sampled     = attr_b(ctx, op, StringView("sampled"));
                r.storage     = attr_b(ctx, op, StringView("storage"));
                r.depth_buffer = attr_b(ctx, op, StringView("depth_buffer"));
                r.no_alias    = attr_b(ctx, op, StringView("no_alias"));
                r.resizable   = attr_b(ctx, op, StringView("resizable"));
                r.stride      = static_cast<crd::u32>(attr_i(ctx, op, StringView("stride"), 0));
                r.count       = static_cast<crd::u32>(attr_i(ctx, op, StringView("count"), 0));
                r.size_bytes  = static_cast<crd::u32>(attr_i(ctx, op, StringView("size_bytes"), 0));
                out.resources.push_back(std::move(r));
            }
            else if (nm == StringView("frame.draw_list"))
            {
                FrameDrawListDesc dl(alloc);
                set_str(dl.name, attr_sv(ctx, op, StringView("name")));
                split_components(attr_sv(ctx, op, StringView("all")), dl.all, alloc);
                split_components(attr_sv(ctx, op, StringView("any")), dl.any, alloc);
                split_components(attr_sv(ctx, op, StringView("none")), dl.none, alloc);
                dl.cull  = cull_from(attr_sv(ctx, op, StringView("cull")));
                dl.sort  = sort_from(attr_sv(ctx, op, StringView("sort")));
                dl.limit = static_cast<crd::u32>(attr_i(ctx, op, StringView("limit"), 0));
                out.draw_lists.push_back(std::move(dl));
            }
            else if (nm == StringView("frame.pass"))
            {
                FramePassDesc   p(alloc);
                set_str(p.name, attr_sv(ctx, op, StringView("name")));
                const StringView ex = attr_sv(ctx, op, StringView("executor"));
                p.executor_id       = crd::renderpass::executor_type_id(ex);
                if (!is_builtin_executor(p.executor_id)) { set_str(p.executor, ex); } // custom keeps the string
                // access tokens ∈ {r,w,rw}, one per operand. ⛔ NOT last-char-wins: `rw` has BOTH components, so decode
                // structurally — first char 'r' ⟺ a READ, last char 'w' ⟺ a WRITE. The FORWARD never EMITS `rw` (a desc
                // read-modify-write rides two operands, w+r), but the dialect ACCEPTS a hand-authored single `rw` operand;
                // map it to BOTH a read AND a write ref (the desc expresses RMW as reads∋X + writes∋X, and the forward
                // re-canonicalizes it to two operands — CEIR-15c-1b, NEW-IN-CEIR §4).
                const StringView ac   = attr_sv(ctx, op, StringView("access"));
                const StringView idxs = attr_sv(ctx, op, StringView("indexed")); // 2b: per-operand [$index] marks (empty ⇒ none)
                bool             tread[128]  = {};
                bool             twrite[128] = {};
                crd::u32         nt          = 0U;
                if (ac.size() > 0U) // an empty access = zero operands; ceir_parse_access counts no token for it
                {
                    crd::usize start = 0U;
                    for (crd::usize i = 0; i <= ac.size() && nt < 128U; ++i)
                    {
                        if (i != ac.size() && ac[i] != ',') { continue; }
                        const StringView tok(ac.data() + start, i - start);
                        start = i + 1U;
                        if (tok.size() > 0U)
                        {
                            tread[nt]  = tok[0] == 'r';
                            twrite[nt] = tok[tok.size() - 1U] == 'w';
                        }
                        ++nt;
                    }
                }
                for (crd::u32 i = 0; i < op->num_operands(); ++i)
                {
                    const ce::Operation* def = op->operand(i)->defining_op();
                    // ⭐ Fork B (B1): an operand DEFINED BY frame.history is a PREVIOUS-frame read — recover the resource name
                    // from the declare it wraps (its own operand). Its token is 'r', so it lands in the read branch below.
                    if (def != nullptr && ctx.op_name(def->kind()) == StringView("frame.history") && def->num_operands() > 0U)
                    {
                        def = def->operand(0)->defining_op();
                    }
                    const StringView dn = def != nullptr ? attr_sv(ctx, def, StringView("name")) : StringView();
                    if (def != nullptr && ctx.op_name(def->kind()) == StringView("frame.draw_list"))
                    {
                        set_pass_str(p, StringView(pp::kDrawList), dn);
                        continue; // a draw-list operand's token (always 'r') is meaningless — ignore it
                    }
                    // An untokened operand (i >= nt) cannot occur on a find_frame_misuse-clean module (PassAccessArity gates
                    // token-count == operand-count), so drop it rather than default it — garbage-in, and the caller validated.
                    const bool op_indexed = i < idxs.size() && idxs[i] == '1'; // 2b: this operand's [$index] mark
                    if (i < nt && twrite[i])
                    {
                        FrameResourceRef ref(alloc);
                        set_str(ref.name, dn);
                        ref.indexed = op_indexed;
                        p.writes.push_back(std::move(ref));
                    }
                    if (i < nt && tread[i])
                    {
                        FrameResourceRef ref(alloc);
                        set_str(ref.name, dn);
                        ref.indexed = op_indexed;
                        p.reads.push_back(std::move(ref));
                    }
                }
                const StringView fe = attr_sv(ctx, op, StringView("for_each"));
                if (fe.size() > 0U && fe != StringView("none"))
                {
                    p.for_each     = for_each_from(fe);
                    p.for_each_arg = static_cast<crd::u32>(attr_i(ctx, op, StringView("for_each_arg"), 0));
                }
                if (attr_sv(ctx, op, StringView("queue")) == StringView("async")) { p.queue = FrameQueue::Async; }
                const StringView sh = attr_sv(ctx, op, StringView("shader"));
                if (sh.size() > 0U) { set_pass_str(p, StringView(pp::kShader), sh); }
                const StringView kn = attr_sv(ctx, op, StringView("kernel"));
                if (kn.size() > 0U) { set_pass_str(p, StringView(pp::kKernel), kn); }
                const StringView tc = attr_sv(ctx, op, StringView("technique"));
                if (tc.size() > 0U) { set_pass_str(p, StringView(pp::kTechnique), tc); }
                // CEIR-15c-1c-2 (Fork C-2): reconstruct the full param bag. Enumerate the op's attrs; each pt:<name> (the
                // type byte) rebuilds a param from p:<name> (value). ⛔ Iterate-WHAT-EXISTS, pushing a FrameParam DIRECTLY —
                // no set_pass_* conditionality (set_pass_flag adds only-if-true; set_pass_str no-ops on empty) and no != 0
                // filter, so a param round-trips exactly as it sits in the bag.
                for (crd::u32 ai = 0; ai < op->num_attrs(); ++ai)
                {
                    const StringView an = op->attr_name(ai);
                    if (an.size() < 3U || an[0] != 'p' || an[1] != 't' || an[2] != ':') { continue; }
                    const StringView pname(an.data() + 3, static_cast<crd::usize>(an.size() - 3U));
                    const auto       ptype = static_cast<FrameParamType>(attr_i(ctx, op, an, 0));
                    char             vbuf[192];
                    FrameParam       fp(alloc);
                    set_str(fp.name, pname);
                    fp.type = ptype;
                    if (ptype == FrameParamType::String)
                    {
                        set_str(fp.str, attr_sv(ctx, op, param_attr_name(vbuf, sizeof(vbuf), StringView("p:"), pname, -1)));
                    }
                    else if (ptype == FrameParamType::Bool)
                    {
                        fp.v[0] = attr_b(ctx, op, param_attr_name(vbuf, sizeof(vbuf), StringView("p:"), pname, -1)) ? 1.0 : 0.0;
                    }
                    else if (ptype == FrameParamType::Vec4)
                    {
                        for (crd::i32 c = 0; c < 4; ++c)
                        {
                            fp.v[c] = attr_f64(ctx, op, param_attr_name(vbuf, sizeof(vbuf), StringView("p:"), pname, c), 0.0);
                        }
                    }
                    else // Float / Int / Enum / U32 — the v[0] family
                    {
                        fp.v[0] = attr_f64(ctx, op, param_attr_name(vbuf, sizeof(vbuf), StringView("p:"), pname, -1), 0.0);
                    }
                    p.params.push_back(std::move(fp));
                }
                out.passes.push_back(std::move(p));
            }
        }
    }
    return true;
}

// ── CEIR-15c-1c: the GRAPH-SEMANTIC verifier (validate_ceir_frame). ──
namespace
{
[[nodiscard]] StringView semantic_kind_name(FrameSemanticKind k) noexcept
{
    switch (k)
    {
    case FrameSemanticKind::None: return StringView("None");
    case FrameSemanticKind::NoOutputPass: return StringView("NoOutputPass");
    case FrameSemanticKind::DuplicateName: return StringView("DuplicateName");
    case FrameSemanticKind::PingPongNeedsBothWays: return StringView("PingPongNeedsBothWays");
    case FrameSemanticKind::ResourceNeverWritten: return StringView("ResourceNeverWritten");
    case FrameSemanticKind::CubeNeedsSquare: return StringView("CubeNeedsSquare");
    case FrameSemanticKind::VolumeNeedsDepth: return StringView("VolumeNeedsDepth");
    case FrameSemanticKind::BadMipCount: return StringView("BadMipCount");
    case FrameSemanticKind::LayersOutOfRange: return StringView("LayersOutOfRange");
    case FrameSemanticKind::StructuredNeedsStride: return StringView("StructuredNeedsStride");
    case FrameSemanticKind::StrideNotAligned: return StringView("StrideNotAligned");
    case FrameSemanticKind::AccelIsExternal: return StringView("AccelIsExternal");
    case FrameSemanticKind::ExternalTextureIsReadOnly: return StringView("ExternalTextureIsReadOnly");
    case FrameSemanticKind::KindLifetimeMismatch: return StringView("KindLifetimeMismatch");
    case FrameSemanticKind::UnknownDimension: return StringView("UnknownDimension");
    case FrameSemanticKind::BadResourceSize: return StringView("BadResourceSize");
    case FrameSemanticKind::PersistentNeedsSize: return StringView("PersistentNeedsSize");
    case FrameSemanticKind::ProgramContract: return StringView("ProgramContract");
    case FrameSemanticKind::UnknownEnumParam: return StringView("UnknownEnumParam");
    case FrameSemanticKind::DependencyCycle: return StringView("DependencyCycle");
    }
    return StringView("None");
}
// A tiny insert-or-reject name set (linear scan — frame graphs are small). Names are StringViews into ctx's arena (stable).
struct NameSet
{
    static constexpr crd::u32 kMax = 128U;
    StringView                names[kMax];
    crd::u32                  n = 0U;
    [[nodiscard]] bool        add(StringView s) // false ⇒ `s` was already present (a duplicate in this category)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (names[i] == s) { return false; }
        }
        if (n < kMax) { names[n++] = s; }
        return true;
    }
};
// The def-use usage of ONE graph-owned resource.declare — its producer/consumer flags across all passes. Imports (external
// kinds) are exempt from producer rules and are NOT tracked here. `lifetime` selects the rule: history ⇒ both-ways,
// persistent ⇒ exempt, transient ⇒ must be written.
struct ResUsage
{
    const ce::Operation* decl     = nullptr;
    StringView           lifetime;
    bool                 read  = false;
    bool                 wrote = false;
};
// The per-resource "what a resource may SAY" diagnosis for one resource.declare — a mirror of validate_frame_graph's
// resource-shape block (frame_asset.cpp). Dispatches by frame_kind (frame-cook: it interprets the FrameResourceKind /
// FgImageKind enums) and returns the FIRST violation in the desc's order, or None. ⛔ PingPongNeedsBothWays + the
// persistent/history producer exemptions are handled by the CALLER (they need the cross-op def-use flags).
[[nodiscard]] FrameSemanticKind declare_resource_diag(const ce::Context& ctx, const ce::Operation* d)
{
    const crd::i64 fk = attr_i(ctx, d, StringView("frame_kind"), 0);
    if (fk == static_cast<crd::i64>(FrameResourceKind::TransientImage))
    {
        // LayersOutOfRange first (the desc order), then the REN-38-B2 SHAPE (Cube → Volume → BadMip).
        const crd::u32 layers = static_cast<crd::u32>(attr_i(ctx, d, StringView("layers"), 1));
        if (layers == 0U || layers > crd::gpu::kFgMaxImageLayers) { return FrameSemanticKind::LayersOutOfRange; }
        const crd::i64 dim = attr_i(ctx, d, StringView("dimension"), static_cast<crd::i64>(crd::gpu::FgImageKind::Tex2D));
        const crd::u32 w   = static_cast<crd::u32>(attr_i(ctx, d, StringView("width"), 0));
        const crd::u32 h   = static_cast<crd::u32>(attr_i(ctx, d, StringView("height"), 0));
        const crd::u32 dep = static_cast<crd::u32>(attr_i(ctx, d, StringView("depth"), 1));
        const crd::u32 mip = static_cast<crd::u32>(attr_i(ctx, d, StringView("mips"), 1));
        if ((dim == static_cast<crd::i64>(crd::gpu::FgImageKind::Cube)
             || dim == static_cast<crd::i64>(crd::gpu::FgImageKind::CubeArray))
            && w != h)
        {
            return FrameSemanticKind::CubeNeedsSquare;
        }
        if (dim == static_cast<crd::i64>(crd::gpu::FgImageKind::Tex3D) && dep == 0U)
        {
            return FrameSemanticKind::VolumeNeedsDepth;
        }
        if (mip == 0U) { return FrameSemanticKind::BadMipCount; } // 0 is NOT "full chain" — a guessed length mismatches
        crd::u32 ext = w > h ? w : h;
        if (ext != 0U) // a chain cannot outlive its extent (levels halve to 1x1); scale-relative extents check at runtime
        {
            crd::u32 max_mips = 1U;
            while ((ext >> max_mips) != 0U) { ++max_mips; }
            if (mip > max_mips) { return FrameSemanticKind::BadMipCount; }
        }
        return FrameSemanticKind::None;
    }
    if (fk == static_cast<crd::i64>(FrameResourceKind::StructuredBuffer)
        || fk == static_cast<crd::i64>(FrameResourceKind::CounterBuffer))
    {
        const crd::u32 stride = static_cast<crd::u32>(attr_i(ctx, d, StringView("stride"), 0));
        if (stride == 0U) { return FrameSemanticKind::StructuredNeedsStride; } // elements with no size
        if ((stride % 4U) != 0U) { return FrameSemanticKind::StrideNotAligned; } // both APIs require a 4-byte-aligned stride
        return FrameSemanticKind::None;
    }
    return FrameSemanticKind::None; // persistent/history (caller) / imports / plain buffers say nothing shape-checkable here
}
} // namespace

StringView frame_semantic_kind_name(FrameSemanticKind k) noexcept { return semantic_kind_name(k); }

FrameSemanticDiag validate_ceir_frame(const ce::Context& ctx, const ce::Module& m, crd::memory::IAllocator* alloc)
{
    const ce::Operation* const g = find_graph(ctx, m.body());
    if (g == nullptr) { return {}; } // not a frame module — structure is find_frame_misuse's job, nothing semantic to check
    const ce::Region* const rg = g->region(0);

    NameSet resources; // resource.declare + import share ONE category (frame_asset's DuplicateName is per-category)
    NameSet draw_lists;
    NameSet passes;

    static constexpr crd::u32 kMaxRes = 128U;
    ResUsage                  usage[kMaxRes];
    crd::u32                  nres = 0U;
    const auto                find_usage = [&](const ce::Operation* d) -> ResUsage* {
        for (crd::u32 i = 0; i < nres; ++i)
        {
            if (usage[i].decl == d) { return &usage[i]; }
        }
        return nullptr;
    };
    const ce::Operation* imports[kMaxRes]; // the resource.import ops (external kinds) — for AccelIsExternal
    crd::u32             nimp = 0U;
    const ce::Operation* pass_ops[kMaxRes]; // the frame.pass ops in graph order — paired by index with from_ceir_frame's desc.passes
    crd::u32             npass = 0U;

    bool wrote_output = false;
    for (const ce::Block* b = rg != nullptr ? rg->first_block() : nullptr; b != nullptr; b = b->next_in_region())
    {
        for (const ce::Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());
            if (nm == StringView("resource.declare"))
            {
                if (!resources.add(attr_sv(ctx, op, StringView("name")))) { return {op, FrameSemanticKind::DuplicateName}; }
                if (!format_valid(static_cast<crd::u32>(attr_i(ctx, op, StringView("format"), 0)))) // 6b: closed-vocab format
                {
                    return {op, FrameSemanticKind::UnknownEnumParam, FrameCookError::UnknownFormat};
                }
                if (nres < kMaxRes) { usage[nres++] = {op, attr_sv(ctx, op, StringView("lifetime")), false, false}; }
            }
            else if (nm == StringView("resource.import"))
            {
                // the implicit @output import (no frame_kind) is the endpoint sentinel, never a desc resource — skip it.
                if (!op->attr(StringView("frame_kind")).valid()) { continue; }
                // an external import (texture/buffer/AS) IS a resource for DuplicateName, but is EXEMPT from producer rules
                // (the host owns it) — so it is not tracked in `usage`. It IS tracked in `imports` (for AccelIsExternal).
                if (!resources.add(attr_sv(ctx, op, StringView("name")))) { return {op, FrameSemanticKind::DuplicateName}; }
                if (!format_valid(static_cast<crd::u32>(attr_i(ctx, op, StringView("format"), 0)))) // 6b: an external texture's format
                {
                    return {op, FrameSemanticKind::UnknownEnumParam, FrameCookError::UnknownFormat};
                }
                if (nimp < kMaxRes) { imports[nimp++] = op; }
            }
            else if (nm == StringView("frame.draw_list"))
            {
                if (!draw_lists.add(attr_sv(ctx, op, StringView("name")))) { return {op, FrameSemanticKind::DuplicateName}; }
                // 6b: the draw-list closed vocabs — string-carried, checked on the op attr (from_ceir_frame normalizes garbage).
                if (!cull_str_valid(attr_sv(ctx, op, StringView("cull"))))
                {
                    return {op, FrameSemanticKind::UnknownEnumParam, FrameCookError::UnknownCull};
                }
                if (!sort_str_valid(attr_sv(ctx, op, StringView("sort"))))
                {
                    return {op, FrameSemanticKind::UnknownEnumParam, FrameCookError::UnknownSort};
                }
            }
            else if (nm == StringView("frame.pass"))
            {
                if (!passes.add(attr_sv(ctx, op, StringView("name")))) { return {op, FrameSemanticKind::DuplicateName}; }
                if (npass < kMaxRes) { pass_ops[npass++] = op; } // graph order — paired with from_ceir_frame's desc.passes below
                // 6b: the pass-level string vocabs (for_each generator, queue preference) — checked on the op attr.
                if (!for_each_str_valid(attr_sv(ctx, op, StringView("for_each"))))
                {
                    return {op, FrameSemanticKind::UnknownEnumParam, FrameCookError::UnknownForEach};
                }
                if (!queue_str_valid(attr_sv(ctx, op, StringView("queue"))))
                {
                    return {op, FrameSemanticKind::UnknownEnumParam, FrameCookError::UnknownQueue};
                }
                // attribute each operand's READ/WRITE to the underlying declare (chasing frame.history back to its resource),
                // and detect the @output write. Tokens ∈ {r,w,rw}, one per operand (find_frame_misuse guarantees arity).
                const StringView ac = attr_sv(ctx, op, StringView("access"));
                if (ac.size() > 0U)
                {
                    crd::u32   opi   = 0U;
                    crd::usize start = 0U;
                    for (crd::usize i = 0; i <= ac.size(); ++i)
                    {
                        if (i != ac.size() && ac[i] != ',') { continue; }
                        const StringView tok(ac.data() + start, i - start);
                        start                = i + 1U;
                        const bool has_read  = tok.size() > 0U && tok[0] == 'r';
                        const bool has_write = tok.size() > 0U && tok[tok.size() - 1U] == 'w';
                        if (opi < op->num_operands())
                        {
                            const ce::Operation* const def = op->operand(opi)->defining_op();
                            const StringView           dn  = def != nullptr ? ctx.op_name(def->kind()) : StringView();
                            if (dn == StringView("frame.history"))
                            {
                                // a frame.history operand is a PREVIOUS-frame READ of the wrapped declare (15c-0 routing).
                                const ce::Operation* const under =
                                    def->num_operands() > 0U ? def->operand(0)->defining_op() : nullptr;
                                if (ResUsage* const u = under != nullptr ? find_usage(under) : nullptr) { u->read = true; }
                            }
                            else if (dn == StringView("resource.import") && has_write
                                     && attr_sv(ctx, def, StringView("name")) == StringView("@output"))
                            {
                                wrote_output = true; // the @output endpoint write (ExternalTextureIsReadOnly is now a pass_contract_diag row)
                            }
                            else if (ResUsage* const u = def != nullptr ? find_usage(def) : nullptr)
                            {
                                if (has_write) { u->wrote = true; }
                                if (has_read) { u->read = true; }
                            }
                        }
                        ++opi;
                    }
                }
            }
        }
    }
    // ⭐ ORDER mirrors validate_frame_graph exactly (for a robust differential oracle): the resource-shape loop's
    // PingPongNeedsBothWays → NoOutputPass → the ResourceNeverWritten loop.
    for (crd::u32 i = 0; i < nres; ++i)
    {
        const ce::Operation* const d = usage[i].decl;
        // ── NEW-IN-CEIR consistency (no desc oracle) — checked FIRST per resource: the shape/PingPong rules below assume a
        // sane kind + dimension. ──
        // §6: frame_kind and lifetime must AGREE. ⛔ An ABSENT lifetime reads as "" (attr_sv), which kind_lifetime never
        // returns — so absent is a mismatch too, NOT silently normalized (the absent-reads-as-default scar).
        if (kind_lifetime(static_cast<FrameResourceKind>(attr_i(ctx, d, StringView("frame_kind"), 0))) != usage[i].lifetime)
        {
            return {d, FrameSemanticKind::KindLifetimeMismatch};
        }
        // A `dimension` attr, if present, must be a valid FgImageKind — for ANY image kind (a garbage int normalizes to "2d"
        // through emit_frame_toml, so the desc round-trip never catches it). Explicit membership, no contiguity assumption.
        if (d->attr(StringView("dimension")).valid())
        {
            const crd::i64 dim = attr_i(ctx, d, StringView("dimension"), 0);
            if (dim != static_cast<crd::i64>(crd::gpu::FgImageKind::Tex2D)
                && dim != static_cast<crd::i64>(crd::gpu::FgImageKind::Tex3D)
                && dim != static_cast<crd::i64>(crd::gpu::FgImageKind::Cube)
                && dim != static_cast<crd::i64>(crd::gpu::FgImageKind::CubeArray))
            {
                return {d, FrameSemanticKind::UnknownDimension};
            }
        }
        // Size rules. `BadResourceSize` is PARSE-time in the desc (so it out-prioritizes the validate-time rules below);
        // `PersistentNeedsSize` is validate-time. Both are decidable from frame_kind/lifetime + geometric attrs.
        const crd::i64 fk    = attr_i(ctx, d, StringView("frame_kind"), 0);
        const crd::u32 w     = static_cast<crd::u32>(attr_i(ctx, d, StringView("width"), 0));
        const crd::u32 h     = static_cast<crd::u32>(attr_i(ctx, d, StringView("height"), 0));
        const float    scale = attr_f(ctx, d, StringView("scale"), 0.0F);
        if (fk == static_cast<crd::i64>(FrameResourceKind::TransientImage) && (w == 0U || h == 0U) && scale <= 0.0F)
        {
            return {d, FrameSemanticKind::BadResourceSize}; // an image with neither an absolute size nor a scale
        }
        if (fk == static_cast<crd::i64>(FrameResourceKind::TransientBuffer)
            && attr_i(ctx, d, StringView("size_bytes"), 0) == 0)
        {
            return {d, FrameSemanticKind::BadResourceSize}; // a transient buffer with no byte size
        }
        // a persistent/history image sized ONLY by scale (its lookup key must be stable across frames) — unless the author
        // OPTS INTO the resize-discard with `resizable` (a TAA history buffer that follows the output).
        if ((usage[i].lifetime == StringView("persistent") || usage[i].lifetime == StringView("history"))
            && (w == 0U || h == 0U) && !(attr_b(ctx, d, StringView("resizable")) && scale > 0.0F))
        {
            return {d, FrameSemanticKind::PersistentNeedsSize};
        }
        if (usage[i].lifetime == StringView("history"))
        {
            // a ping-pong that is only read OR only written never rotates — every frame reads the same stale image.
            if (!(usage[i].read && usage[i].wrote)) { return {d, FrameSemanticKind::PingPongNeedsBothWays}; }
        }
        else
        {
            // the per-resource "may say" checks (Layers/Structured/Cube/Volume/BadMip) — dispatched by frame_kind.
            const FrameSemanticKind rd = declare_resource_diag(ctx, d);
            if (rd != FrameSemanticKind::None) { return {d, rd}; }
        }
    }
    // AccelIsExternal (the desc's separate import loop): an AccelerationStructure given a size/format means the author
    // believed the graph ALLOCATES it — but a BLAS/TLAS is host-built (scene geometry the frame must not depend on).
    for (crd::u32 i = 0; i < nimp; ++i)
    {
        if (attr_i(ctx, imports[i], StringView("frame_kind"), 0)
                == static_cast<crd::i64>(FrameResourceKind::AccelerationStructure)
            && (attr_i(ctx, imports[i], StringView("size_bytes"), 0) != 0 || attr_i(ctx, imports[i], StringView("width"), 0) != 0
                || attr_i(ctx, imports[i], StringView("height"), 0) != 0
                || attr_f(ctx, imports[i], StringView("scale"), 0.0F) > 0.0F))
        {
            return {imports[i], FrameSemanticKind::AccelIsExternal};
        }
    }
    // ── CEIR-15c-1d: the PROGRAM-CONTRACT layer (the desc's pass loop slot). Materialize the full desc ONCE (from_ceir_frame
    // is round-trip-proven; O(graph), no new reconstruction code) and run the SHARED pass_contract_diag per pass — pairing
    // each desc pass with its frame.pass op BY INDEX (both are graph order). The specific FrameCookError rides
    // FrameSemanticDiag::contract; ExternalTextureIsReadOnly maps back to its own kind (old tests hold), all else → ProgramContract.
    {
        FrameGraphDesc desc(alloc);
        const bool     ok = from_ceir_frame(ctx, m, alloc, desc);
        // A module that passed the structural walk above MUST reconstruct — a `false` here is a broken converter invariant, not
        // a valid frame. Assert LOUD (an unrunnable verifier layer must never read as green — the tidy-gate UNGATED doctrine);
        // the `if (ok)` keeps a release build (asserts compiled out) memory-safe.
        CRD_ASSERT_MSG(ok, "from_ceir_frame failed for a structurally-valid frame module");
        if (ok)
        {
            // Both walks visit the graph region in the SAME order, so desc.passes[i] IS pass_ops[i]. The whole verifier caps
            // each category at kMaxRes (nres/nimp/npass); honour that same bound here (check the first kMaxRes passes) instead
            // of skipping the layer wholesale on a > kMaxRes graph.
            const crd::u32 n = npass < desc.passes.size() ? npass : static_cast<crd::u32>(desc.passes.size());
            for (crd::u32 i = 0; i < n; ++i)
            {
                // 15c-1d-6: the closed-vocab check runs BEFORE pass_contract_diag — a garbage enum must never be interpreted
                // as SATISFYING a contract (a garbage blend0 truncates to a non-Opaque value that would "satisfy"
                // CompositeNeedsBlend). NEW-IN-CEIR (no oracle): the desc-side parse can't produce a bad enum int.
                const FrameCookError ve = enum_vocab_diag(desc.passes[i]);
                if (ve != FrameCookError::Ok) { return {pass_ops[i], FrameSemanticKind::UnknownEnumParam, ve}; }
                const FrameCookError e = pass_contract_diag(
                    desc.passes[i],
                    crd::containers::ConstSpan<FrameResourceDesc>(desc.resources.data(), desc.resources.size()), nullptr);
                if (e != FrameCookError::Ok)
                {
                    return {pass_ops[i],
                            e == FrameCookError::ExternalTextureIsReadOnly ? FrameSemanticKind::ExternalTextureIsReadOnly
                                                                          : FrameSemanticKind::ProgramContract,
                            e};
                }
            }
        }
    }
    if (!wrote_output) { return {nullptr, FrameSemanticKind::NoOutputPass}; } // a frame must produce its output endpoint
    for (crd::u32 i = 0; i < nres; ++i)
    {
        // a graph-owned TRANSIENT must have a producer; persistent + history (+ external imports, untracked) are exempt.
        if (usage[i].lifetime != StringView("history") && usage[i].lifetime != StringView("persistent")
            && !usage[i].wrote)
        {
            return {usage[i].decl, FrameSemanticKind::ResourceNeverWritten};
        }
    }
    // ── CEIR-15d-2b: the graph-level DEPENDENCY CYCLE — the SHARED dependency_cycle_diag (a Kahn topo-sort over the REN-41
    // authored-order-aware dep graph), run LAST to match its position at the END of validate_frame_graph (so the differential
    // oracle agrees on WHICH error wins). Re-materialize the desc (cheap, O(graph)) — a cycle has no single offending op, so
    // op == nullptr (like NoOutputPass); `contract` carries DependencyCycle. frame.history makes the ping-pong prev-frame read
    // a satisfiable frame-start value, so a TAA/SSR graph never false-cycles.
    {
        FrameGraphDesc dc(alloc);
        if (from_ceir_frame(ctx, m, alloc, dc) && dependency_cycle_diag(dc) == FrameCookError::DependencyCycle)
        {
            return {nullptr, FrameSemanticKind::DependencyCycle, FrameCookError::DependencyCycle};
        }
    }
    return {};
}
} // namespace crd::framecook
