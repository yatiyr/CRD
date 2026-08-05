// frame_emit.cpp — REN-36.2: description → `.frame.toml`, the EDITOR ROUND-TRIP half.
//
// A node editor loads a graph, the user drags wires, and the result must be writable back to the format a human
// reads and diffs. Losslessness is GATED, not asserted: `parse → emit → parse → cook` must produce bytes
// identical to `parse → cook`. That is what makes it safe for an editor — or an agent — to rewrite a file a
// person hand-authored: a save cannot quietly drop a field it did not understand.

#include <crd/framecook/frame_asset.hpp>

#include <cstdio>

namespace crd::framecook
{
namespace
{

const char* from_pass_kind(FramePassKind k)
{
    switch (k)
    {
    case FramePassKind::RasterGeometry:   return "raster.geometry";
    case FramePassKind::RasterDepthOnly:  return "raster.depth_only";
    case FramePassKind::RasterFullscreen: return "raster.fullscreen";
    case FramePassKind::RasterMrt:        return "raster.mrt";
    case FramePassKind::Compute:          return "compute";
    case FramePassKind::Present:          return "present";
    case FramePassKind::Clear:            return "clear";
    case FramePassKind::Copy:             return "copy";
    case FramePassKind::Blit:             return "blit";
    case FramePassKind::Resolve:          return "resolve";
    case FramePassKind::RasterTess:       return "raster.tess";
    case FramePassKind::RasterMesh:       return "raster.mesh";
    case FramePassKind::RasterVisbuffer:  return "raster.visbuffer";
    case FramePassKind::RasterComposite:  return "raster.composite";
    case FramePassKind::RayTrace:         return "raytrace";
    case FramePassKind::RayTracePipeline: return "raytrace.pipeline";
    case FramePassKind::ComputeIndirect:  return "compute.indirect";
    case FramePassKind::RasterMeshIndirect: return "raster.mesh.indirect";
    case FramePassKind::Custom:             return "custom"; // RAF-10: app executor by id (round-trips to Custom)
    }
    return "raster.geometry";
}
const char* from_format(crd::gpu::FgImageFormat f)
{
    using F = crd::gpu::FgImageFormat;
    switch (f)
    {
    case F::RGBA8Unorm: return "RGBA8Unorm";
    case F::RGBA8Srgb:  return "RGBA8Srgb";
    case F::RGBA16F:    return "RGBA16F";
    case F::R16F:       return "R16F";
    case F::R32F:       return "R32F";
    case F::R32Uint:    return "R32Uint";
    case F::D32Float:   return "D32Float";
    // ⛔ REN-38-B7: EVERY format, and the fall-through below is now unreachable for a valid enum. The old
    // `return "RGBA8Unorm"` tail meant any format the emitter did not name round-tripped INTO RGBA8 — so a cooked
    // pack would have silently turned an HDR light buffer, a motion-vector target or a STENCIL attachment into an
    // 8-bit colour image. Same class as the pass-kind default that turned `raytrace.pipeline` into geometry.
    case F::RG16F:       return "RG16F";
    case F::RG32F:       return "RG32F";
    case F::RGBA32F:     return "RGBA32F";
    case F::R11G11B10F:  return "R11G11B10F";
    case F::RGB10A2:     return "RGB10A2";
    case F::R8:          return "R8";
    case F::RG8:         return "RG8";
    case F::RGBA16Unorm: return "RGBA16Unorm";
    case F::D24S8:       return "D24S8";
    case F::D32FloatS8:  return "D32FloatS8";
    }
    return "RGBA8Unorm";
}
const char* from_compare(crd::gpu::DepthCompare c)
{
    using C = crd::gpu::DepthCompare;
    switch (c)
    {
    case C::Never:        return "Never";
    case C::Less:         return "Less";
    case C::Equal:        return "Equal";
    case C::LessEqual:    return "LessEqual";
    case C::Greater:      return "Greater";
    case C::NotEqual:     return "NotEqual";
    case C::GreaterEqual: return "GreaterEqual";
    case C::Always:       return "Always";
    }
    return "LessEqual";
}
const char* from_material(FrameMaterialPass m)
{
    switch (m)
    {
    case FrameMaterialPass::Shadow:       return "Shadow";
    case FrameMaterialPass::DepthPrepass: return "DepthPrepass";
    case FrameMaterialPass::GBuffer:      return "GBuffer";
    case FrameMaterialPass::Forward:      return "Forward";
    case FrameMaterialPass::None:         break;
    }
    return nullptr;
}
const char* from_cull(FrameCullMode c)
{
    switch (c)
    {
    case FrameCullMode::None:             return "none";
    case FrameCullMode::Frustum:          return "frustum";
    case FrameCullMode::FrustumOcclusion: return "frustum+occlusion";
    }
    return "frustum";
}
const char* from_sort(FrameSortMode s)
{
    switch (s)
    {
    case FrameSortMode::None:        return "none";
    case FrameSortMode::FrontToBack: return "front_to_back";
    case FrameSortMode::BackToFront: return "back_to_front";
    case FrameSortMode::Material:    return "material";
    }
    return "none";
}

void app(crd::containers::String& o, const char* t) { o.append(t); }
void app_u32(crd::containers::String& o, crd::u32 v)
{
    char buf[16];
    std::snprintf(static_cast<char*>(buf), sizeof(buf), "%u", v);
    o.append(static_cast<const char*>(buf));
}
// 17 significant digits is the IEEE-754 float64 exact-round-trip guarantee, so an editor save can never perturb
// an authored value by re-writing it.
void app_f64(crd::containers::String& o, double v)
{
    char buf[40];
    std::snprintf(static_cast<char*>(buf), sizeof(buf), "%.17g", v);
    o.append(static_cast<const char*>(buf));
}
void app_quoted(crd::containers::String& o, const crd::containers::String& v)
{
    app(o, "\"");
    o.append(v.c_str());
    app(o, "\"");
}
void app_list(crd::containers::String& o, const crd::containers::Array<crd::containers::String>& l)
{
    app(o, "[");
    for (crd::usize i = 0; i < l.size(); ++i)
    {
        if (i > 0) { app(o, ", "); }
        app_quoted(o, l[i]);
    }
    app(o, "]");
}
void app_refs(crd::containers::String& o, const crd::containers::Array<FrameResourceRef>& r)
{
    app(o, "[");
    for (crd::usize i = 0; i < r.size(); ++i)
    {
        if (i > 0) { app(o, ", "); }
        app(o, "\"");
        o.append(r[i].name.c_str());
        if (r[i].indexed) { app(o, "[$index]"); } // the subscript is part of the REFERENCE, so it round-trips
        app(o, "\"");
    }
    app(o, "]");
}

} // namespace

crd::containers::String emit_frame_toml(const FrameGraphDesc& desc, crd::memory::IAllocator* a)
{
    crd::containers::String o(a);
    app(o, "schema = ");
    app_u32(o, desc.schema);
    app(o, "\nname = ");
    app_quoted(o, desc.name);
    app(o, "\n");
    // REN-38-B6: the budget round-trips in MEGABYTES — the unit a platform target is actually written in.
    if (desc.memory_budget_bytes != 0U)
    {
        app(o, "memory_budget_mb = ");
        app_u32(o, static_cast<crd::u32>(desc.memory_budget_bytes / (1024ULL * 1024ULL)));
        app(o, "\n");
    }
    if (desc.requires_caps.size() > 0)
    {
        app(o, "requires = ");
        app_list(o, desc.requires_caps);
        app(o, "\n");
    }
    if (!desc.fallback.empty())
    {
        app(o, "fallback = ");
        app_quoted(o, desc.fallback);
        app(o, "\n");
    }

    // REN-37.6: composition, emitted BEFORE the resources so a reader sees what this graph is made OF first.
    for (crd::usize i = 0; i < desc.includes.size(); ++i)
    {
        const FrameIncludeDesc& inc = desc.includes[i];
        app(o, "\n[[include]]\ngraph = ");
        app_quoted(o, inc.graph);
        app(o, "\nas = ");
        app_quoted(o, inc.as);
        app(o, "\n");
        if (inc.atomic) { app(o, "atomic = true\n"); }
        if (inc.bind.size() > 0)
        {
            app(o, "bind = { ");
            for (crd::usize k = 0; k < inc.bind.size(); ++k)
            {
                if (k > 0) { app(o, ", "); }
                app_quoted(o, inc.bind[k].from); // ⛔ `@input` is not a bare TOML key — always quote it
                app(o, " = ");
                app_quoted(o, inc.bind[k].to);
            }
            app(o, " }\n");
        }
    }
    for (crd::usize i = 0; i < desc.anchors.size(); ++i)
    {
        const FrameAnchorDesc& an = desc.anchors[i];
        app(o, "\n[[anchor]]\nname = ");
        app_quoted(o, an.name);
        app(o, "\n");
        const auto emit_list = [&](const char* key, const crd::containers::Array<crd::containers::String>& l) {
            if (l.size() == 0) { return; }
            app(o, key);
            app(o, " = [");
            for (crd::usize k = 0; k < l.size(); ++k)
            {
                if (k > 0) { app(o, ", "); }
                app_quoted(o, l[k]);
            }
            app(o, "]\n");
        };
        emit_list("after", an.after);
        emit_list("before", an.before);
    }
    for (crd::usize i = 0; i < desc.injects.size(); ++i)
    {
        app(o, "\n[[inject]]\nat = ");
        app_quoted(o, desc.injects[i].anchor);
        app(o, "\npass = ");
        app_quoted(o, desc.injects[i].pass);
        app(o, "\n");
    }

    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        app(o, "\n[[resource]]\nname = ");
        app_quoted(o, r.name);
        app(o, "\nkind = ");
        // ⛔ REN-38-B3/B4: EVERY kind, not a two-way ternary. The old form emitted "transient_image" for an
        // `indirect_args` or `acceleration_structure` resource, so a cooked pack would round-trip a GPU-driven
        // graph into one whose args buffer was a texture — silently, and only at runtime.
        switch (r.kind)
        {
        case FrameResourceKind::TransientBuffer:       app(o, "\"transient_buffer\""); break;
        case FrameResourceKind::IndirectArgs:          app(o, "\"indirect_args\""); break;
        case FrameResourceKind::ExternalBuffer:        app(o, "\"external_buffer\""); break;
        case FrameResourceKind::PersistentImage:       app(o, "\"persistent_image\""); break;
        case FrameResourceKind::PingPongImage:         app(o, "\"pingpong_image\""); break;
        case FrameResourceKind::StructuredBuffer:      app(o, "\"structured_buffer\""); break;
        case FrameResourceKind::CounterBuffer:         app(o, "\"counter_buffer\""); break;
        case FrameResourceKind::ExternalTexture:       app(o, "\"external_texture\""); break;
        case FrameResourceKind::AccelerationStructure: app(o, "\"acceleration_structure\""); break;
        case FrameResourceKind::TransientImage:
        default:                                       app(o, "\"transient_image\""); break;
        }
        app(o, "\nformat = \"");
        app(o, from_format(r.format));
        app(o, "\"\n");
        if (r.width != 0U)      { app(o, "width = ");      app_u32(o, r.width);      app(o, "\n"); }
        if (r.height != 0U)     { app(o, "height = ");     app_u32(o, r.height);     app(o, "\n"); }
        if (r.scale > 0.0F)     { app(o, "scale = ");      app_f64(o, static_cast<double>(r.scale)); app(o, "\n"); }
        if (r.layers != 1U)     { app(o, "layers = ");     app_u32(o, r.layers);     app(o, "\n"); }
        // REN-38-B2: shape. Only when it differs from the default, so an ordinary 2-D transient
        // round-trips byte-clean.
        if (r.kind_2d != crd::gpu::FgImageKind::Tex2D)
        {
            app(o, "dimension = \"");
            switch (r.kind_2d)
            {
            case crd::gpu::FgImageKind::Tex3D:     app(o, "3d"); break;
            case crd::gpu::FgImageKind::Cube:      app(o, "cube"); break;
            case crd::gpu::FgImageKind::CubeArray: app(o, "cube_array"); break;
            case crd::gpu::FgImageKind::Tex2D:
            default:                               app(o, "2d"); break;
            }
            app(o, "\"\n");
        }
        if (r.depth != 1U)      { app(o, "depth = ");      app_u32(o, r.depth);      app(o, "\n"); }
        if (r.mips != 1U)       { app(o, "mips = ");       app_u32(o, r.mips);       app(o, "\n"); }
        if (r.samples != 1U)    { app(o, "samples = ");    app_u32(o, r.samples);    app(o, "\n"); }
        if (r.sampled)          { app(o, "sampled = true\n"); }
        if (r.depth_buffer)     { app(o, "depth_buffer = true\n"); }
        if (r.storage)          { app(o, "storage = true\n"); }
        if (r.no_alias)         { app(o, "no_alias = true\n"); } // REN-38-B6
        if (r.resizable)        { app(o, "resizable = true\n"); } // REN-41: persistent follows the output on resize
        // REN-38-B3: emit STRIDE and COUNT, not the derived `size_bytes`. Re-parsing a counter buffer's
        // size would add its 4-byte counter a SECOND time, so the round trip would grow it every cook.
        if (r.stride != 0U)     { app(o, "stride = ");     app_u32(o, r.stride);     app(o, "\n"); }
        if (r.count != 0U)      { app(o, "count = ");      app_u32(o, r.count);      app(o, "\n"); }
        if (r.size_bytes != 0U && r.stride == 0U) { app(o, "size_bytes = "); app_u32(o, r.size_bytes); app(o, "\n"); }
    }

    for (crd::usize i = 0; i < desc.draw_lists.size(); ++i)
    {
        const FrameDrawListDesc& d = desc.draw_lists[i];
        app(o, "\n[[draw_list]]\nname = ");
        app_quoted(o, d.name);
        app(o, "\n");
        if (d.all.size() > 0)  { app(o, "all = ");  app_list(o, d.all);  app(o, "\n"); }
        if (d.any.size() > 0)  { app(o, "any = ");  app_list(o, d.any);  app(o, "\n"); }
        if (d.none.size() > 0) { app(o, "none = "); app_list(o, d.none); app(o, "\n"); }
        app(o, "cull = \"");
        app(o, from_cull(d.cull));
        app(o, "\"\nsort = \"");
        app(o, from_sort(d.sort));
        app(o, "\"\n");
        if (d.limit != 0U) { app(o, "limit = "); app_u32(o, d.limit); app(o, "\n"); }
    }

    for (crd::usize i = 0; i < desc.passes.size(); ++i)
    {
        const FramePassDesc& p = desc.passes[i];
        app(o, "\n[[pass]]\nname = ");
        app_quoted(o, p.name);
        app(o, "\nkind = \"");
        app(o, from_pass_kind(p.kind));
        app(o, "\"\n");
        if (p.reads.size() > 0)   { app(o, "reads = ");     app_refs(o, p.reads);       app(o, "\n"); }
        if (p.writes.size() > 0)  { app(o, "writes = ");    app_refs(o, p.writes);      app(o, "\n"); }
        if (!p.draw_list.empty()) { app(o, "draw_list = "); app_quoted(o, p.draw_list); app(o, "\n"); }
        if (!p.view.empty())      { app(o, "view = ");      app_quoted(o, p.view);      app(o, "\n"); }
        if (!p.shader.empty())    { app(o, "shader = ");    app_quoted(o, p.shader);    app(o, "\n"); }
        if (!p.kernel.empty())    { app(o, "kernel = ");    app_quoted(o, p.kernel);    app(o, "\n"); }
        if (!p.raygen.empty())      { app(o, "raygen = ");      app_quoted(o, p.raygen);      app(o, "\n"); }
        if (!p.miss.empty())        { app(o, "miss = ");        app_quoted(o, p.miss);        app(o, "\n"); }
        if (!p.closest_hit.empty()) { app(o, "closest_hit = "); app_quoted(o, p.closest_hit); app(o, "\n"); }
        if (!p.any_hit.empty())     { app(o, "any_hit = ");     app_quoted(o, p.any_hit);     app(o, "\n"); }
        // REN-38-F13: the last two SBT roles
        if (!p.intersection.empty()) { app(o, "intersection = "); app_quoted(o, p.intersection); app(o, "\n"); }
        if (!p.callable.empty())     { app(o, "callable = ");     app_quoted(o, p.callable);     app(o, "\n"); }
        if (!p.technique.empty()) { app(o, "technique = "); app_quoted(o, p.technique); app(o, "\n"); }
        // REN-38-A6: only a BLIT rescales, so only a blit emits its filter — a round-trip that wrote `filter` on
        // every pass would make two identical graphs differ by a field neither kind reads.
        if (p.kind == FramePassKind::Blit)
        {
            app(o, "filter = \"");
            app(o, p.filter == FrameBlitFilter::Nearest ? "nearest" : "linear");
            app(o, "\"\n");
        }
        if (p.blend.size() > 0)
        {
            app(o, "blend = [");
            for (crd::usize k = 0; k < p.blend.size(); ++k)
            {
                if (k > 0) { app(o, ", "); }
                app(o, "\"");
                switch (p.blend[k])
                {
                case crd::gpu::BlendMode::Alpha:              app(o, "alpha"); break;
                case crd::gpu::BlendMode::PremultipliedAlpha: app(o, "premultiplied"); break;
                case crd::gpu::BlendMode::Additive:           app(o, "additive"); break;
                case crd::gpu::BlendMode::Multiply:           app(o, "multiply"); break;
                case crd::gpu::BlendMode::RevealageMultiply:  app(o, "revealage_multiply"); break;
                case crd::gpu::BlendMode::Opaque:
                default:                                      app(o, "opaque"); break;
                }
                app(o, "\"");
            }
            app(o, "]\n");
        }
        // REN-38-A13/A14: only emit what was DECLARED — writing the defaults on every pass would make two
        // identical graphs differ by fields neither meant to set.
        if (p.shading_rate != crd::gpu::ShadingRate::Rate1x1)
        {
            app(o, "shading_rate = \"");
            switch (p.shading_rate)
            {
            case crd::gpu::ShadingRate::Rate1x2: app(o, "1x2"); break;
            case crd::gpu::ShadingRate::Rate2x1: app(o, "2x1"); break;
            case crd::gpu::ShadingRate::Rate2x2: app(o, "2x2"); break;
            case crd::gpu::ShadingRate::Rate2x4: app(o, "2x4"); break;
            case crd::gpu::ShadingRate::Rate4x2: app(o, "4x2"); break;
            case crd::gpu::ShadingRate::Rate4x4: app(o, "4x4"); break;
            case crd::gpu::ShadingRate::Rate1x1:
            default:                             app(o, "1x1"); break;
            }
            app(o, "\"\n");
        }
        if (p.rate_combiner != crd::gpu::ShadingRateCombiner::Keep)
        {
            app(o, "rate_combiner = \"");
            switch (p.rate_combiner)
            {
            case crd::gpu::ShadingRateCombiner::Replace: app(o, "replace"); break;
            case crd::gpu::ShadingRateCombiner::Min:     app(o, "min"); break;
            case crd::gpu::ShadingRateCombiner::Max:     app(o, "max"); break;
            case crd::gpu::ShadingRateCombiner::Mul:     app(o, "mul"); break;
            case crd::gpu::ShadingRateCombiner::Keep:
            default:                                     app(o, "keep"); break;
            }
            app(o, "\"\n");
        }
        if (p.conservative != crd::gpu::ConservativeMode::Off)
        {
            app(o, "conservative = \"");
            app(o, p.conservative == crd::gpu::ConservativeMode::Underestimate ? "underestimate" : "overestimate");
            app(o, "\"\n");
        }
        if (p.queue != FrameQueue::Graphics) { app(o, "queue = \"async\"\n"); }
        // REN-38-B8: only a pass that DECLARED a sampler emits one — writing the defaults everywhere would make
        // two identical graphs differ by fields neither meant to set.
        if (p.has_sampler)
        {
            if (p.kind != FramePassKind::Blit) // `filter` on a blit already means the blit filter
            {
                app(o, "filter = \"");
                app(o, p.sampler.mag_filter == crd::gpu::SamplerFilter::Nearest ? "nearest" : "linear");
                app(o, "\"\n");
            }
            app(o, "address = \"");
            switch (p.sampler.address)
            {
            case crd::gpu::SamplerAddress::ClampToEdge:   app(o, "clamp"); break;
            case crd::gpu::SamplerAddress::ClampToBorder: app(o, "clamp_to_border"); break;
            case crd::gpu::SamplerAddress::Mirror:        app(o, "mirror"); break;
            case crd::gpu::SamplerAddress::Repeat:
            default:                                      app(o, "repeat"); break;
            }
            app(o, "\"\n");
            if (p.sampler.anisotropy > 1U) { app(o, "anisotropy = "); app_u32(o, p.sampler.anisotropy); app(o, "\n"); }
            if (p.sampler.compare) { app(o, "compare = true\n"); }
        }
        const char* mp = from_material(p.material_pass);
        if (mp != nullptr) { app(o, "material_pass = \""); app(o, mp); app(o, "\"\n"); }
        if (p.for_each != FrameForEach::None)
        {
            app(o, "for_each = \"");
            switch (p.for_each)
            {
            case FrameForEach::LightCascades:
                app(o, "light.");
                app_u32(o, p.for_each_arg);
                app(o, ".cascades");
                break;
            case FrameForEach::StereoViews:         app(o, "views.stereo"); break;
            case FrameForEach::CubeFaces:           app(o, "cube.faces"); break;
            case FrameForEach::ShadowCastingLights: app(o, "lights.shadow_casting"); break;
            case FrameForEach::None:                break;
            }
            app(o, "\"\n");
        }
        if (p.has_clear_color)
        {
            app(o, "clear_color = [");
            for (crd::u32 c = 0; c < 4U; ++c)
            {
                if (c > 0U) { app(o, ", "); }
                app_f64(o, static_cast<double>(p.clear_color[c]));
            }
            app(o, "]\n");
        }
        if (p.has_clear_depth)
        {
            app(o, "clear_depth = ");
            app_f64(o, static_cast<double>(p.clear_depth));
            app(o, "\n");
        }
        app(o, "depth = \"");
        app(o, from_compare(p.depth));
        app(o, "\"\n");
        // ── ⭐ REN-38 audit: the PASS-STATE vocabulary — non-default fields only, and BEFORE `[pass.params]`
        // (a bare key written after a table belongs to THAT table; the root-keys-before-tables scar). ──
        {
            const crd::gpu::PassRasterState& st = p.state;
            const auto from_stencil_op = [](crd::gpu::StencilOp op) {
                switch (op)
                {
                case crd::gpu::StencilOp::Keep:      return "keep";
                case crd::gpu::StencilOp::Zero:      return "zero";
                case crd::gpu::StencilOp::Replace:   return "replace";
                case crd::gpu::StencilOp::IncrClamp: return "incr_clamp";
                case crd::gpu::StencilOp::DecrClamp: return "decr_clamp";
                case crd::gpu::StencilOp::Invert:    return "invert";
                case crd::gpu::StencilOp::IncrWrap:  return "incr_wrap";
                case crd::gpu::StencilOp::DecrWrap:  return "decr_wrap";
                }
                return "keep";
            };
            // REN-38-F11: the pass-level LOAD flag (mask-then-test pass pairs stack on one target)
            if (p.load_target) { app(o, "load = true\n"); }
            // REN-40-G1: load depth only (depth-prepass pattern: clear colour, load depth)
            if (p.load_depth) { app(o, "load_depth = true\n"); }
            if (!p.shared_depth.empty()) { app(o, "shared_depth = "); app_quoted(o, p.shared_depth); app(o, "\n"); }
            if (p.depth_as_float) { app(o, "depth_as_float = true\n"); }
            if (p.untracked_storage) { app(o, "untracked_storage = true\n"); }
            if (!st.depth_write) { app(o, "depth_write = false\n"); }
            if (st.depth_bias != 0.0F) { app(o, "depth_bias = "); app_f64(o, static_cast<double>(st.depth_bias)); app(o, "\n"); }
            if (st.depth_bias_slope != 0.0F)
            {
                app(o, "depth_bias_slope = ");
                app_f64(o, static_cast<double>(st.depth_bias_slope));
                app(o, "\n");
            }
            if (st.depth_bias_clamp != 0.0F)
            {
                app(o, "depth_bias_clamp = ");
                app_f64(o, static_cast<double>(st.depth_bias_clamp));
                app(o, "\n");
            }
            if (st.face_cull != crd::gpu::FaceCull::None)
            {
                app(o, "face_cull = \"");
                app(o, st.face_cull == crd::gpu::FaceCull::Back ? "back" : "front");
                app(o, "\"\n");
            }
            if (st.front_face != crd::gpu::FrontFace::CounterClockwise) { app(o, "front_face = \"cw\"\n"); }
            if (st.stencil_enable)
            {
                app(o, "stencil = true\n");
                app(o, "stencil_compare = \"");
                app(o, from_compare(st.stencil_compare));
                app(o, "\"\n");
                app(o, "stencil_ref = ");
                app_u32(o, st.stencil_ref);
                app(o, "\n");
                if (st.stencil_read_mask != 0xFFU)
                {
                    app(o, "stencil_read_mask = ");
                    app_u32(o, st.stencil_read_mask);
                    app(o, "\n");
                }
                if (st.stencil_write_mask != 0xFFU)
                {
                    app(o, "stencil_write_mask = ");
                    app_u32(o, st.stencil_write_mask);
                    app(o, "\n");
                }
                app(o, "stencil_fail = \"");
                app(o, from_stencil_op(st.stencil_fail));
                app(o, "\"\n");
                app(o, "stencil_depth_fail = \"");
                app(o, from_stencil_op(st.stencil_depth_fail));
                app(o, "\"\n");
                app(o, "stencil_pass = \"");
                app(o, from_stencil_op(st.stencil_pass));
                app(o, "\"\n");
            }
        }
        if (p.params.size() > 0)
        {
            app(o, "[pass.params]\n");
            for (crd::usize k = 0; k < p.params.size(); ++k)
            {
                const FrameParam& prm = p.params[k];
                o.append(prm.name.c_str());
                app(o, " = ");
                if (prm.type == FrameParamType::Vec4)
                {
                    app(o, "[");
                    for (crd::u32 c = 0; c < 4U; ++c)
                    {
                        if (c > 0U) { app(o, ", "); }
                        app_f64(o, prm.v[c]);
                    }
                    app(o, "]");
                }
                else if (prm.type == FrameParamType::Bool) { app(o, prm.v[0] != 0.0 ? "true" : "false"); }
                else { app_f64(o, prm.v[0]); }
                app(o, "\n");
            }
        }
    }
    return o;
}

} // namespace crd::framecook

