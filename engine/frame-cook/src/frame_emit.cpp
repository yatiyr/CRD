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

    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        app(o, "\n[[resource]]\nname = ");
        app_quoted(o, r.name);
        app(o, "\nkind = ");
        app(o, r.kind == FrameResourceKind::TransientBuffer ? "\"transient_buffer\"" : "\"transient_image\"");
        app(o, "\nformat = \"");
        app(o, from_format(r.format));
        app(o, "\"\n");
        if (r.width != 0U)      { app(o, "width = ");      app_u32(o, r.width);      app(o, "\n"); }
        if (r.height != 0U)     { app(o, "height = ");     app_u32(o, r.height);     app(o, "\n"); }
        if (r.scale > 0.0F)     { app(o, "scale = ");      app_f64(o, static_cast<double>(r.scale)); app(o, "\n"); }
        if (r.layers != 1U)     { app(o, "layers = ");     app_u32(o, r.layers);     app(o, "\n"); }
        if (r.samples != 1U)    { app(o, "samples = ");    app_u32(o, r.samples);    app(o, "\n"); }
        if (r.sampled)          { app(o, "sampled = true\n"); }
        if (r.storage)          { app(o, "storage = true\n"); }
        if (r.size_bytes != 0U) { app(o, "size_bytes = "); app_u32(o, r.size_bytes); app(o, "\n"); }
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
