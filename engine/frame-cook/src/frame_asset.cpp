// frame_asset.cpp — REN-36.1: parse + VALIDATE a `.frame.toml`, and cook it to a canonical `.crdr` blob.
// Contract + rationale: frame_asset.hpp and docs/design/ren-36-authorable-frame-graph.md.

#include <crd/framecook/frame_asset.hpp>

#include <toml++/toml.hpp>

#include <cstring>
#include <string_view>

namespace crd::framecook
{
namespace
{

// ── the cooked container ─────────────────────────────────────────────────────────────────────────────────────
// FourCC 'FRAM'. CANONICAL + PACKED + PADDING-FREE + little-endian by construction: every field is written
// explicitly, so the bytes are a pure function of the DESCRIPTION and a graph cooked under MSVC loads
// byte-identically under gcc/clang. (The ckir_serialize scar — never memcpy a POD into an artifact.)
constexpr crd::u32 kFourCC = (static_cast<crd::u32>('F')) | (static_cast<crd::u32>('R') << 8U)
                             | (static_cast<crd::u32>('A') << 16U) | (static_cast<crd::u32>('M') << 24U);
// v2 = REN-37.2 added `FramePassDesc::technique` to the pass record. A v1 blob is cleanly REJECTED (recook)
// rather than read with a shifted layout — the same discipline `ckir_serialize` uses.
// v3 = REN-37.6 appended the include/anchor/inject records. Appended at the END of the blob so the earlier
// sections are byte-unchanged; the version still bumps because a v2 reader would stop short and silently produce
// a graph with no composition at all.
// v4 (REN-38 audit): the per-pass record grew the fields v3 was silently DROPPING — the RT pipeline's three
// program names, VRS state, conservative, queue, the sampler and the blit filter — plus the new pass-state
// block. ⛔ The byte-identity gate could not see the loss: a field dropped by BOTH the writer and the reader
// round-trips "byte-identically", which is why the round-trip gate now asserts FIELD SURVIVAL instead.
constexpr crd::u32 kBlobVersion = 7U; // v7 (REN-40-G3): + shared_depth (v6: intersection/callable SBT roles)

using Bytes = crd::containers::Array<crd::u8>;

void put_u8(Bytes& b, crd::u8 v) { b.push_back(v); }
void put_u32(Bytes& b, crd::u32 v)
{
    for (crd::u32 s = 0; s < 32U; s += 8U) { b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU)); }
}
void put_f32(Bytes& b, float v)
{
    crd::u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(b, bits); // the BIT PATTERN, never a decimal round-trip
}
void put_f64(Bytes& b, double v)
{
    crd::u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    for (crd::u32 s = 0; s < 64U; s += 8U) { b.push_back(static_cast<crd::u8>((bits >> s) & 0xFFU)); }
}
void put_str(Bytes& b, const crd::containers::String& s)
{
    put_u32(b, static_cast<crd::u32>(s.size()));
    for (crd::usize i = 0; i < s.size(); ++i) { b.push_back(static_cast<crd::u8>(s.c_str()[i])); }
}

struct Cursor
{
    crd::containers::ConstSpan<crd::u8> in;
    crd::u64                            pos = 0;
    bool                                ok  = true;

    bool have(crd::u64 n) noexcept
    {
        if (!ok || pos + n > in.size()) { ok = false; }
        return ok;
    }
    crd::u8  u8v() noexcept { return have(1U) ? in[pos++] : static_cast<crd::u8>(0); }
    crd::u32 u32v() noexcept
    {
        if (!have(4U)) { return 0U; }
        crd::u32 v = 0;
        for (crd::u32 i = 0; i < 4U; ++i) { v |= static_cast<crd::u32>(in[pos + i]) << (i * 8U); }
        pos += 4U;
        return v;
    }
    float f32v() noexcept
    {
        const crd::u32 bits = u32v();
        float          v    = 0.0F;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    double f64v() noexcept
    {
        if (!have(8U)) { return 0.0; }
        crd::u64 bits = 0;
        for (crd::u32 i = 0; i < 8U; ++i) { bits |= static_cast<crd::u64>(in[pos + i]) << (i * 8U); }
        pos += 8U;
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void strv(crd::containers::String& out) noexcept
    {
        const crd::u32 n = u32v();
        if (!have(n)) { return; }
        out.clear();
        for (crd::u32 i = 0; i < n; ++i)
        {
            const char c[2] = {static_cast<char>(in[pos + i]), '\0'};
            out.append(static_cast<const char*>(c));
        }
        pos += n;
    }
};

// ── string → enum. Every miss is a NAMED cook error, never a silent default. ─────────────────────────────────
bool to_pass_kind(std::string_view s, FramePassKind& out)
{
    if (s == "raster.geometry")   { out = FramePassKind::RasterGeometry;   return true; }
    if (s == "raster.depth_only") { out = FramePassKind::RasterDepthOnly;  return true; }
    if (s == "raster.fullscreen") { out = FramePassKind::RasterFullscreen; return true; }
    if (s == "raster.mrt")        { out = FramePassKind::RasterMrt;        return true; }
    if (s == "compute")           { out = FramePassKind::Compute;          return true; }
    if (s == "present")           { out = FramePassKind::Present;          return true; }
    // REN-38-A6: the utility passes.
    if (s == "clear")             { out = FramePassKind::Clear;            return true; }
    if (s == "copy")              { out = FramePassKind::Copy;             return true; }
    if (s == "blit")              { out = FramePassKind::Blit;             return true; }
    if (s == "resolve")           { out = FramePassKind::Resolve;          return true; }
    // REN-38-A7 / A8: the amplification kinds.
    if (s == "raster.tess")       { out = FramePassKind::RasterTess;       return true; }
    if (s == "raster.mesh")       { out = FramePassKind::RasterMesh;       return true; }
    // REN-38-A9 / A10: ray tracing and the GPU-driven loop.
    if (s == "raster.visbuffer")     { out = FramePassKind::RasterVisbuffer;    return true; }
    if (s == "raster.composite")     { out = FramePassKind::RasterComposite;    return true; }
    if (s == "raytrace")             { out = FramePassKind::RayTrace;           return true; }
    if (s == "raytrace.pipeline")    { out = FramePassKind::RayTracePipeline;   return true; }
    if (s == "compute.indirect")     { out = FramePassKind::ComputeIndirect;    return true; }
    if (s == "raster.mesh.indirect") { out = FramePassKind::RasterMeshIndirect; return true; }
    if (s == "custom")               { out = FramePassKind::Custom;             return true; } // RAF-10: app executor by id
    return false;
}
// REN-38-A13: the closed sets for per-pass render state. Every miss is a NAMED cook error — a typo that fell
// back to the default would render at 1×1 while the asset says 2×2, and nothing in the frame would disagree.
bool to_shading_rate(std::string_view s, crd::gpu::ShadingRate& out)
{
    using R = crd::gpu::ShadingRate;
    if (s == "1x1") { out = R::Rate1x1; return true; }
    if (s == "1x2") { out = R::Rate1x2; return true; }
    if (s == "2x1") { out = R::Rate2x1; return true; }
    if (s == "2x2") { out = R::Rate2x2; return true; }
    if (s == "2x4") { out = R::Rate2x4; return true; }
    if (s == "4x2") { out = R::Rate4x2; return true; }
    if (s == "4x4") { out = R::Rate4x4; return true; }
    return false;
}
bool to_rate_combiner(std::string_view s, crd::gpu::ShadingRateCombiner& out)
{
    using C = crd::gpu::ShadingRateCombiner;
    if (s == "keep")    { out = C::Keep;    return true; }
    if (s == "replace") { out = C::Replace; return true; }
    if (s == "min")     { out = C::Min;     return true; }
    if (s == "max")     { out = C::Max;     return true; }
    if (s == "mul")     { out = C::Mul;     return true; }
    return false;
}
bool to_conservative(std::string_view s, crd::gpu::ConservativeMode& out)
{
    using M = crd::gpu::ConservativeMode;
    if (s == "off")          { out = M::Off;          return true; }
    if (s == "overestimate") { out = M::Overestimate; return true; }
    if (s == "underestimate"){ out = M::Underestimate;return true; }
    return false;
}
// REN-38-B8: the closed sampler sets.
bool to_sampler_filter(std::string_view s, crd::gpu::SamplerFilter& out)
{
    if (s == "nearest") { out = crd::gpu::SamplerFilter::Nearest; return true; }
    if (s == "linear")  { out = crd::gpu::SamplerFilter::Linear;  return true; }
    return false;
}
bool to_sampler_address(std::string_view s, crd::gpu::SamplerAddress& out)
{
    using A = crd::gpu::SamplerAddress;
    if (s == "repeat")          { out = A::Repeat;        return true; }
    if (s == "clamp")           { out = A::ClampToEdge;   return true; }
    if (s == "clamp_to_border") { out = A::ClampToBorder; return true; }
    if (s == "mirror")          { out = A::Mirror;        return true; }
    return false;
}
bool to_queue(std::string_view s, FrameQueue& out)
{
    if (s == "graphics") { out = FrameQueue::Graphics; return true; }
    if (s == "async")    { out = FrameQueue::Async;    return true; }
    return false;
}
// REN-38-B2: the closed set of resource SHAPES.
bool to_dimension(std::string_view s, crd::gpu::FgImageKind& out)
{
    using K = crd::gpu::FgImageKind;
    if (s == "2d")         { out = K::Tex2D;     return true; }
    if (s == "3d")         { out = K::Tex3D;     return true; }
    if (s == "cube")       { out = K::Cube;      return true; }
    if (s == "cube_array") { out = K::CubeArray; return true; }
    return false;
}
bool to_blit_filter(std::string_view s, FrameBlitFilter& out)
{
    if (s == "nearest") { out = FrameBlitFilter::Nearest; return true; }
    if (s == "linear")  { out = FrameBlitFilter::Linear;  return true; }
    return false;
}
bool to_format(std::string_view s, crd::gpu::FgImageFormat& out)
{
    using F = crd::gpu::FgImageFormat;
    if (s == "RGBA8Unorm") { out = F::RGBA8Unorm; return true; }
    if (s == "RGBA8Srgb")  { out = F::RGBA8Srgb;  return true; }
    if (s == "RGBA16F")    { out = F::RGBA16F;    return true; }
    if (s == "R16F")       { out = F::R16F;       return true; }
    if (s == "R32F")       { out = F::R32F;       return true; }
    if (s == "R32Uint")    { out = F::R32Uint;    return true; }
    if (s == "D32Float")   { out = F::D32Float;   return true; }
    // REN-38-B7: the rest of the vocabulary. Names match the enum exactly — an asset should never have to learn a
    // second spelling for a format it can already read in the header.
    if (s == "RG16F")      { out = F::RG16F;      return true; }
    if (s == "RG32F")      { out = F::RG32F;      return true; }
    if (s == "RGBA32F")    { out = F::RGBA32F;    return true; }
    if (s == "R11G11B10F") { out = F::R11G11B10F; return true; }
    if (s == "RGB10A2")    { out = F::RGB10A2;    return true; }
    if (s == "R8")         { out = F::R8;         return true; }
    if (s == "RG8")        { out = F::RG8;        return true; }
    if (s == "RGBA16Unorm"){ out = F::RGBA16Unorm;return true; }
    if (s == "D24S8")      { out = F::D24S8;      return true; }
    if (s == "D32FloatS8") { out = F::D32FloatS8; return true; }
    return false;
}
bool to_compare(std::string_view s, crd::gpu::DepthCompare& out)
{
    using C = crd::gpu::DepthCompare;
    if (s == "Never")        { out = C::Never;        return true; }
    if (s == "Less")         { out = C::Less;         return true; }
    if (s == "Equal")        { out = C::Equal;        return true; }
    if (s == "LessEqual")    { out = C::LessEqual;    return true; }
    if (s == "Greater")      { out = C::Greater;      return true; }
    if (s == "NotEqual")     { out = C::NotEqual;     return true; }
    if (s == "GreaterEqual") { out = C::GreaterEqual; return true; }
    if (s == "Always")       { out = C::Always;       return true; }
    return false;
}
// ── REN-38 audit: the PASS-STATE closed sets. Same rule as every set here: a typo is a NAMED rejection, never
// a silent fall-back to the default — a pass that says `face_cull = "bcak"` and renders uncalled reads as a
// winding bug, not as a spelling one.
bool to_face_cull(std::string_view s, crd::gpu::FaceCull& out)
{
    using F = crd::gpu::FaceCull;
    if (s == "none")  { out = F::None;  return true; }
    if (s == "back")  { out = F::Back;  return true; }
    if (s == "front") { out = F::Front; return true; }
    return false;
}
bool to_front_face(std::string_view s, crd::gpu::FrontFace& out)
{
    using F = crd::gpu::FrontFace;
    if (s == "ccw") { out = F::CounterClockwise; return true; }
    if (s == "cw")  { out = F::Clockwise;        return true; }
    return false;
}
bool to_stencil_op(std::string_view s, crd::gpu::StencilOp& out)
{
    using O = crd::gpu::StencilOp;
    if (s == "keep")       { out = O::Keep;      return true; }
    if (s == "zero")       { out = O::Zero;      return true; }
    if (s == "replace")    { out = O::Replace;   return true; }
    if (s == "incr_clamp") { out = O::IncrClamp; return true; }
    if (s == "decr_clamp") { out = O::DecrClamp; return true; }
    if (s == "invert")     { out = O::Invert;    return true; }
    if (s == "incr_wrap")  { out = O::IncrWrap;  return true; }
    if (s == "decr_wrap")  { out = O::DecrWrap;  return true; }
    return false;
}
bool to_material_pass(std::string_view s, FrameMaterialPass& out)
{
    if (s == "Shadow")       { out = FrameMaterialPass::Shadow;       return true; }
    if (s == "DepthPrepass") { out = FrameMaterialPass::DepthPrepass; return true; }
    if (s == "GBuffer")      { out = FrameMaterialPass::GBuffer;      return true; }
    if (s == "Forward")      { out = FrameMaterialPass::Forward;      return true; }
    return false;
}
bool to_cull(std::string_view s, FrameCullMode& out)
{
    if (s == "none")              { out = FrameCullMode::None;             return true; }
    if (s == "frustum")           { out = FrameCullMode::Frustum;          return true; }
    if (s == "frustum+occlusion") { out = FrameCullMode::FrustumOcclusion; return true; }
    return false;
}
// REN-38-A15: the closed blend set. A small named set rather than raw src/dst/op factors, so the cooker can
// VERIFY what an asset asked for — an open factor triple can only be obeyed, never checked.
bool to_blend(std::string_view s, crd::gpu::BlendMode& out)
{
    using B = crd::gpu::BlendMode;
    if (s == "opaque")             { out = B::Opaque;             return true; }
    if (s == "alpha")              { out = B::Alpha;              return true; }
    if (s == "premultiplied")      { out = B::PremultipliedAlpha; return true; }
    if (s == "additive")           { out = B::Additive;           return true; }
    if (s == "multiply")           { out = B::Multiply;           return true; }
    if (s == "revealage_multiply") { out = B::RevealageMultiply;  return true; }
    if (s == "reveal_composite")   { out = B::RevealComposite;    return true; }
    return false;
}
bool to_sort(std::string_view s, FrameSortMode& out)
{
    if (s == "none")           { out = FrameSortMode::None;        return true; }
    if (s == "front_to_back")  { out = FrameSortMode::FrontToBack; return true; }
    if (s == "back_to_front")  { out = FrameSortMode::BackToFront; return true; }
    if (s == "material")       { out = FrameSortMode::Material;    return true; }
    return false;
}
// `light.0.cascades` → (LightCascades, 0). The numeric arg is parsed out of the generator name.
bool to_for_each(std::string_view s, FrameForEach& out, crd::u32& arg)
{
    arg = 0U;
    if (s == "views.stereo")          { out = FrameForEach::StereoViews;         return true; }
    if (s == "cube.faces")            { out = FrameForEach::CubeFaces;           return true; }
    if (s == "lights.shadow_casting") { out = FrameForEach::ShadowCastingLights; return true; }
    if (s.size() > 7U && s.starts_with("light.") && s.ends_with(".cascades"))
    {
        crd::u32 n   = 0U;
        bool     any = false;
        for (crd::usize i = 6; i < s.size() - 9U; ++i)
        {
            if (s[i] < '0' || s[i] > '9') { return false; }
            n = (n * 10U) + static_cast<crd::u32>(s[i] - '0');
            any = true;
        }
        if (!any) { return false; }
        out = FrameForEach::LightCascades;
        arg = n;
        return true;
    }
    return false;
}

void set_str(crd::containers::String& dst, std::string_view s)
{
    dst.clear();
    for (const char c : s)
    {
        const char one[2] = {c, '\0'};
        dst.append(static_cast<const char*>(one));
    }
}
void set_where(crd::containers::String* where, std::string_view s)
{
    if (where != nullptr) { set_str(*where, s); }
}
bool str_eq(const crd::containers::String& a, std::string_view b)
{
    return a.size() == b.size() && std::memcmp(a.c_str(), b.data(), b.size()) == 0;
}

// A reference like `shadow_atlas[$index]` splits into the name + the indexed flag.
void parse_ref(std::string_view s, FrameResourceRef& out)
{
    constexpr std::string_view idx_suffix = "[$index]";
    if (s.size() > idx_suffix.size() && s.ends_with(idx_suffix))
    {
        out.indexed = true;
        set_str(out.name, s.substr(0, s.size() - idx_suffix.size()));
        return;
    }
    out.indexed = false;
    set_str(out.name, s);
}

} // namespace

const char* frame_cook_error_text(FrameCookError err) noexcept
{
    switch (err)
    {
    case FrameCookError::Ok:                    return "ok";
    case FrameCookError::ParseFailed:           return "not valid TOML";
    case FrameCookError::BadSchema:             return "missing or unsupported `schema`";
    case FrameCookError::MissingName:           return "the graph, a resource, a draw list or a pass has no `name`";
    case FrameCookError::DuplicateName:         return "two entries in the same category share a name";
    case FrameCookError::UnknownPassKind:       return "unknown pass `kind`";
    case FrameCookError::UnknownFormat:         return "unknown resource `format`";
    case FrameCookError::UnknownCompare:        return "unknown `depth` comparison";
    case FrameCookError::UnknownSort:           return "unknown draw-list `sort`";
    case FrameCookError::UnknownBlend:          return "unknown `blend` mode";
    case FrameCookError::UnknownCull:           return "unknown draw-list `cull`";
    case FrameCookError::UnknownMaterialPass:   return "unknown `material_pass`";
    case FrameCookError::UnknownForEach:        return "unknown `for_each` generator";
    case FrameCookError::UnknownResource:       return "a pass reads or writes a resource that was never declared";
    case FrameCookError::ResourceNeverWritten:  return "a declared resource is never written by any pass";
    case FrameCookError::DependencyCycle:       return "the passes form a dependency CYCLE";
    case FrameCookError::MissingShader:         return "a fullscreen pass needs `shader` (a compute pass needs `kernel`)";
    case FrameCookError::MissingDrawList:       return "a geometry or depth-only pass needs `draw_list`";
    case FrameCookError::SubscriptOnNonLayered: return "`[$index]` used on a resource with layers == 1";
    case FrameCookError::IndexWithoutForEach:   return "`[$index]` used by a pass that declares no `for_each`";
    case FrameCookError::NoOutputPass:          return "no pass writes `@output`";
    case FrameCookError::BadResourceSize:       return "a resource needs either width+height or scale";
    case FrameCookError::LayersOutOfRange:      return "`layers` must be between 1 and 16";
    case FrameCookError::UnknownDimension:      return "unknown `dimension` (2d/3d/cube/cube_array)";
    case FrameCookError::CubeNeedsSquare:       return "a cube face must be SQUARE";
    case FrameCookError::BadMipCount:           return "`mips` is 0, or more levels than the extent can halve to";
    case FrameCookError::VolumeNeedsDepth:      return "`dimension = \"3d\"` needs a non-zero `depth`";
    case FrameCookError::PersistentNeedsSize:   return "a persistent/ping-pong image needs an absolute width+height";
    case FrameCookError::PingPongNeedsBothWays: return "a ping-pong resource must be both READ and WRITTEN";
    case FrameCookError::StructuredNeedsStride: return "a structured/counter buffer needs a `stride`";
    case FrameCookError::StrideNotAligned:      return "a structured `stride` must be a multiple of 4";
    case FrameCookError::ExternalTextureIsReadOnly: return "a pass writes an external texture; the app owns it";
    case FrameCookError::UnknownSamplerFilter:  return "unknown sampler `filter` (nearest/linear)";
    case FrameCookError::UnknownSamplerAddress: return "unknown sampler `address` (repeat/clamp/clamp_to_border/mirror)";
    case FrameCookError::IncludeMissingName:    return "an `[[include]]` needs both `graph` and `as`";
    case FrameCookError::DuplicateInclude:      return "two includes share an `as` namespace";
    case FrameCookError::UnknownAnchor:         return "an `[[inject]]` names an anchor no graph declares";
    case FrameCookError::InjectUnknownPass:     return "an `[[inject]]` names a pass this asset does not declare";
    case FrameCookError::AnchorUnknownPass:     return "an `[[anchor]]` names a pass that does not exist";
    case FrameCookError::UnresolvedInclude:     return "an included graph could not be resolved by name";
    case FrameCookError::IncludeCycle:          return "the includes form a CYCLE";
    case FrameCookError::PresentNeedsOneRead:   return "a present pass must read EXACTLY ONE resource";
    case FrameCookError::PresentWritesNothing:  return "a present pass declares a write; a present produces nothing";
    case FrameCookError::PresentSourceInternal: return "a present pass reads a TRANSIENT; its memory is aliased away";
    case FrameCookError::TransferNeedsOneRead:  return "a copy/blit/resolve pass must read EXACTLY ONE source";
    case FrameCookError::TransferNeedsOneWrite: return "a copy/blit/resolve/clear pass must write EXACTLY ONE target";
    case FrameCookError::ClearReadsNothing:     return "a clear pass declares a read; a clear consumes nothing";
    case FrameCookError::UnknownFilter:         return "a `filter` that is not `nearest` or `linear`";
    case FrameCookError::AmplifyNeedsCount:     return "a tess/mesh pass needs a draw list or a `patches`/`groups` count";
    case FrameCookError::RayTraceNeedsAccel:    return "a raytrace pass reads no acceleration structure";
    case FrameCookError::IndirectNeedsArgs:     return "an indirect pass reads no `indirect_args` buffer";
    case FrameCookError::IndirectArgsNotArgs:   return "an indirect pass's declared reads contain no `indirect_args` buffer";
    case FrameCookError::AccelIsExternal:       return "an acceleration structure was given a size/format; the graph never creates one";
    case FrameCookError::VisbufferNeedsUintTarget: return "a visibility-buffer pass writes a target that is not R32Uint";
    case FrameCookError::CompositeNeedsBlend:      return "a composite pass declares no blend; it would erase the background";
    case FrameCookError::UnknownShadingRate:       return "unknown `shading_rate` (1x1/1x2/2x1/2x2/2x4/4x2/4x4)";
    case FrameCookError::UnknownRateCombiner:      return "unknown `rate_combiner` (keep/replace/min/max/mul)";
    case FrameCookError::UnknownConservative:      return "unknown `conservative` (off/overestimate/underestimate)";
    case FrameCookError::UnknownQueue:             return "unknown `queue` (graphics/async)";
    case FrameCookError::AsyncQueueNeedsCompute:   return "a raster pass asked for the async-compute queue";
    case FrameCookError::RtPipelineNeedsThree:     return "a raytrace.pipeline pass needs raygen + miss + closest_hit";
    case FrameCookError::UnknownFaceCull:          return "unknown `face_cull` (none/back/front)";
    case FrameCookError::UnknownFrontFace:         return "unknown `front_face` (ccw/cw)";
    case FrameCookError::UnknownStencilOp:         return "unknown stencil op";
    case FrameCookError::BadStencilValue:          return "a stencil ref/mask outside 0..255";
    case FrameCookError::LoadNeedsGeometry:        return "`load = true` is only supported on raster.geometry passes";
    }
    return "unknown error";
}

FrameCookError parse_frame_toml(crd::containers::StringView toml_text, FrameGraphDesc& out,
                                crd::containers::String* where)
{
    auto* alloc = out.resources.allocator();
    const std::string_view text(toml_text.data(), toml_text.size());
    const toml::parse_result res = toml::parse(text);
    if (!res) { return FrameCookError::ParseFailed; }
    const toml::table& root = res.table();

    // ⛔ RESET THE OUTPUT FIRST — the scar every cooker parser carries (material/vertex/light fixed it first):
    // parsing into a descriptor that already held a graph APPENDED to it (a silently merged frame), and the
    // SCALARS carried over too — a reused desc kept the previous graph's `memory_budget_bytes` and `fallback`
    // when the new asset declared neither, which is a budget and a fallback the file never wrote.
    out.name.clear();
    out.resources.clear();
    out.draw_lists.clear();
    out.passes.clear();
    out.requires_caps.clear();
    out.fallback.clear();
    out.includes.clear();
    out.anchors.clear();
    out.injects.clear();
    out.memory_budget_bytes = 0U;

    const auto sch = root["schema"].value<int64_t>();
    if (!sch || *sch != static_cast<int64_t>(kFrameSchemaVersion)) { return FrameCookError::BadSchema; }
    out.schema = kFrameSchemaVersion;
    // REN-38-B6: the graph-level transient budget, stated in MEGABYTES because that is the unit a platform target
    // is actually written in. Converted here so nothing downstream has to remember the factor.
    if (const auto mb = root["memory_budget_mb"].value<int64_t>())
    {
        out.memory_budget_bytes = static_cast<crd::u64>(*mb > 0 ? *mb : 0) * 1024ULL * 1024ULL;
    }

    const auto nm = root["name"].value<std::string_view>();
    if (!nm || nm->empty()) { return FrameCookError::MissingName; }
    set_str(out.name, *nm);

    if (const auto fb = root["fallback"].value<std::string_view>()) { set_str(out.fallback, *fb); }
    if (const auto* caps = root["requires"].as_array())
    {
        for (const auto& c : *caps)
        {
            const auto s = c.value<std::string_view>();
            if (!s) { return FrameCookError::ParseFailed; }
            crd::containers::String cap(alloc);
            set_str(cap, *s);
            out.requires_caps.push_back(static_cast<crd::containers::String&&>(cap));
        }
    }

    // ── REN-37.6: includes / anchors / injects ──
    if (const auto* arr = root["include"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameIncludeDesc inc(alloc);
            const auto gname = (*t)["graph"].value<std::string_view>();
            const auto as    = (*t)["as"].value<std::string_view>();
            if (!gname || gname->empty() || !as || as->empty()) { return FrameCookError::IncludeMissingName; }
            set_str(inc.graph, *gname);
            set_str(inc.as, *as);
            inc.atomic = (*t)["atomic"].value_or(false);
            for (crd::usize i = 0; i < out.includes.size(); ++i)
            {
                if (str_eq(out.includes[i].as, *as)) { set_where(where, *as); return FrameCookError::DuplicateInclude; }
            }
            if (const toml::table* bt = (*t)["bind"].as_table())
            {
                for (const auto& [k, v] : *bt)
                {
                    const auto sv = v.value<std::string_view>();
                    if (!sv) { return FrameCookError::ParseFailed; }
                    FrameBinding b(alloc);
                    set_str(b.from, std::string_view(k.str()));
                    set_str(b.to, *sv);
                    inc.bind.push_back(static_cast<FrameBinding&&>(b));
                }
            }
            out.includes.push_back(static_cast<FrameIncludeDesc&&>(inc));
        }
    }
    if (const auto* arr = root["anchor"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameAnchorDesc a(alloc);
            const auto an = (*t)["name"].value<std::string_view>();
            if (!an || an->empty()) { return FrameCookError::MissingName; }
            set_str(a.name, *an);
            const auto list = [&](const char* key, crd::containers::Array<crd::containers::String>& dst) -> bool {
                if (const auto* ar = (*t)[key].as_array())
                {
                    for (const auto& e : *ar)
                    {
                        const auto sv = e.value<std::string_view>();
                        if (!sv) { return false; }
                        crd::containers::String v(alloc);
                        set_str(v, *sv);
                        dst.push_back(static_cast<crd::containers::String&&>(v));
                    }
                }
                return true;
            };
            if (!list("after", a.after) || !list("before", a.before)) { return FrameCookError::ParseFailed; }
            out.anchors.push_back(static_cast<FrameAnchorDesc&&>(a));
        }
    }
    if (const auto* arr = root["inject"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameInjectDesc inj(alloc);
            const auto at = (*t)["at"].value<std::string_view>();
            const auto ps = (*t)["pass"].value<std::string_view>();
            if (!at || at->empty() || !ps || ps->empty()) { return FrameCookError::MissingName; }
            set_str(inj.anchor, *at);
            set_str(inj.pass, *ps);
            out.injects.push_back(static_cast<FrameInjectDesc&&>(inj));
        }
    }

    // ── resources ──
    if (const auto* arr = root["resource"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameResourceDesc r(alloc);
            const auto rn = (*t)["name"].value<std::string_view>();
            if (!rn || rn->empty()) { return FrameCookError::MissingName; }
            set_str(r.name, *rn);
            for (crd::usize i = 0; i < out.resources.size(); ++i)
            {
                if (str_eq(out.resources[i].name, *rn)) { set_where(where, *rn); return FrameCookError::DuplicateName; }
            }
            const auto kind = (*t)["kind"].value_or(std::string_view{"transient_image"});
            // REN-38-B3/B4: the closed set of resource kinds. ⛔ An UNKNOWN kind must be a NAMED rejection — the
            // old `?:` silently treated every typo as a transient IMAGE, so `kind = "indirect_args"` with a
            // misspelling would have produced a 2-D texture and a pass that reads garbage arguments.
            if (kind == "transient_buffer")           { r.kind = FrameResourceKind::TransientBuffer; }
            else if (kind == "indirect_args")         { r.kind = FrameResourceKind::IndirectArgs; }
            else if (kind == "external_buffer")       { r.kind = FrameResourceKind::ExternalBuffer; }
            else if (kind == "persistent_image")      { r.kind = FrameResourceKind::PersistentImage; }
            else if (kind == "pingpong_image")        { r.kind = FrameResourceKind::PingPongImage; }
            else if (kind == "structured_buffer")     { r.kind = FrameResourceKind::StructuredBuffer; }
            else if (kind == "counter_buffer")        { r.kind = FrameResourceKind::CounterBuffer; }
            else if (kind == "external_texture")      { r.kind = FrameResourceKind::ExternalTexture; }
            else if (kind == "acceleration_structure"){ r.kind = FrameResourceKind::AccelerationStructure; }
            else if (kind == "transient_image")       { r.kind = FrameResourceKind::TransientImage; }
            else { set_where(where, kind); return FrameCookError::UnknownFormat; }
            if (const auto f = (*t)["format"].value<std::string_view>())
            {
                if (!to_format(*f, r.format)) { set_where(where, *f); return FrameCookError::UnknownFormat; }
            }
            r.width      = static_cast<crd::u32>((*t)["width"].value_or<int64_t>(0));
            r.height     = static_cast<crd::u32>((*t)["height"].value_or<int64_t>(0));
            r.scale      = static_cast<float>((*t)["scale"].value_or<double>(0.0));
            r.layers     = static_cast<crd::u32>((*t)["layers"].value_or<int64_t>(1));
            r.samples    = static_cast<crd::u32>((*t)["samples"].value_or<int64_t>(1));
            r.sampled    = (*t)["sampled"].value_or(false);
            r.depth_buffer = (*t)["depth_buffer"].value_or(false); // 38-G1: an intermediate render target's depth
            r.storage    = (*t)["storage"].value_or(false);
            r.depth      = static_cast<crd::u32>((*t)["depth"].value_or<int64_t>(1));
            r.mips       = static_cast<crd::u32>((*t)["mips"].value_or<int64_t>(1));
            if (const auto dv = (*t)["dimension"].value<std::string_view>())
            {
                if (!to_dimension(*dv, r.kind_2d)) { set_where(where, *dv); return FrameCookError::UnknownDimension; }
            }
            r.no_alias   = (*t)["no_alias"].value_or(false); // REN-38-B6
            r.resizable  = (*t)["resizable"].value_or(false); // REN-41: persistent image follows the output on resize
            r.stride     = static_cast<crd::u32>((*t)["stride"].value_or<int64_t>(0));
            r.count      = static_cast<crd::u32>((*t)["count"].value_or<int64_t>(0));
            r.size_bytes = static_cast<crd::u32>((*t)["size_bytes"].value_or<int64_t>(0));
            // REN-38-B3: elements × stride IS the size. Stating both would let them disagree, and the resulting
            // buffer would be a different length than the shader indexes — so one is derived, never checked.
            if (r.size_bytes == 0U && r.stride != 0U && r.count != 0U) { r.size_bytes = r.stride * r.count; }
            // ⛔ A COUNTER buffer's first 4 bytes ARE the counter, so its payload starts after them. Folding that
            // into the declared size here means no author ever has to remember the +4, and no two techniques can
            // disagree about whether it was already included.
            if (r.kind == FrameResourceKind::CounterBuffer) { r.size_bytes += 4U; }
            if (r.kind == FrameResourceKind::TransientImage
                && ((r.width == 0U || r.height == 0U) && r.scale <= 0.0F))
            {
                set_where(where, *rn);
                return FrameCookError::BadResourceSize;
            }
            if (r.kind == FrameResourceKind::TransientBuffer && r.size_bytes == 0U)
            {
                set_where(where, *rn);
                return FrameCookError::BadResourceSize;
            }
            out.resources.push_back(static_cast<FrameResourceDesc&&>(r));
        }
    }

    // ── draw lists (ECS queries the graph declares itself) ──
    if (const auto* arr = root["draw_list"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FrameDrawListDesc d(alloc);
            const auto dn = (*t)["name"].value<std::string_view>();
            if (!dn || dn->empty()) { return FrameCookError::MissingName; }
            set_str(d.name, *dn);
            for (crd::usize i = 0; i < out.draw_lists.size(); ++i)
            {
                if (str_eq(out.draw_lists[i].name, *dn)) { set_where(where, *dn); return FrameCookError::DuplicateName; }
            }
            const auto comps = [&](const char* key, crd::containers::Array<crd::containers::String>& dst) {
                if (const auto* a2 = (*t)[key].as_array())
                {
                    for (const auto& c : *a2)
                    {
                        const auto s = c.value<std::string_view>();
                        if (!s) { continue; }
                        crd::containers::String cs(alloc);
                        set_str(cs, *s);
                        dst.push_back(static_cast<crd::containers::String&&>(cs));
                    }
                }
            };
            comps("all", d.all);
            comps("any", d.any);
            comps("none", d.none);
            if (const auto c = (*t)["cull"].value<std::string_view>())
            {
                if (!to_cull(*c, d.cull)) { set_where(where, *c); return FrameCookError::UnknownCull; }
            }
            if (const auto s = (*t)["sort"].value<std::string_view>())
            {
                if (!to_sort(*s, d.sort)) { set_where(where, *s); return FrameCookError::UnknownSort; }
            }
            d.limit = static_cast<crd::u32>((*t)["limit"].value_or<int64_t>(0));
            out.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
        }
    }

    // ── passes ──
    if (const auto* arr = root["pass"].as_array())
    {
        for (const auto& node : *arr)
        {
            const toml::table* t = node.as_table();
            if (t == nullptr) { return FrameCookError::ParseFailed; }
            FramePassDesc p(alloc);
            const auto pn = (*t)["name"].value<std::string_view>();
            if (!pn || pn->empty()) { return FrameCookError::MissingName; }
            set_str(p.name, *pn);
            for (crd::usize i = 0; i < out.passes.size(); ++i)
            {
                if (str_eq(out.passes[i].name, *pn)) { set_where(where, *pn); return FrameCookError::DuplicateName; }
            }
            const auto kd = (*t)["kind"].value<std::string_view>();
            if (!kd || !to_pass_kind(*kd, p.kind))
            {
                set_where(where, kd ? *kd : std::string_view{"<missing>"});
                return FrameCookError::UnknownPassKind;
            }
            const auto refs = [&](const char* key, crd::containers::Array<FrameResourceRef>& dst) {
                if (const auto* a2 = (*t)[key].as_array())
                {
                    for (const auto& c : *a2)
                    {
                        const auto s = c.value<std::string_view>();
                        if (!s) { continue; }
                        FrameResourceRef r(alloc);
                        parse_ref(*s, r);
                        dst.push_back(static_cast<FrameResourceRef&&>(r));
                    }
                }
            };
            refs("reads", p.reads);
            refs("writes", p.writes);
            if (const auto v = (*t)["draw_list"].value<std::string_view>()) { set_str(p.draw_list, *v); }
            if (const auto v = (*t)["view"].value<std::string_view>())      { set_str(p.view, *v); }
            if (const auto v = (*t)["shader"].value<std::string_view>())    { set_str(p.shader, *v); }
            if (const auto v = (*t)["kernel"].value<std::string_view>())    { set_str(p.kernel, *v); }
            if (const auto v = (*t)["executor"].value<std::string_view>())  { set_str(p.executor, *v); } // RAF-10: custom
            if (const auto v = (*t)["raygen"].value<std::string_view>())      { set_str(p.raygen, *v); }
            if (const auto v = (*t)["miss"].value<std::string_view>())        { set_str(p.miss, *v); }
            if (const auto v = (*t)["closest_hit"].value<std::string_view>()) { set_str(p.closest_hit, *v); }
            if (const auto v = (*t)["any_hit"].value<std::string_view>())     { set_str(p.any_hit, *v); }
            if (const auto v = (*t)["intersection"].value<std::string_view>()) { set_str(p.intersection, *v); }
            if (const auto v = (*t)["callable"].value<std::string_view>())     { set_str(p.callable, *v); }
            if (const auto v = (*t)["technique"].value<std::string_view>()) { set_str(p.technique, *v); }
            if (const auto v = (*t)["filter"].value<std::string_view>())
            {
                if (!to_blit_filter(*v, p.filter)) { set_where(where, *v); return FrameCookError::UnknownFilter; }
            }
            // REN-38-A13 / A14: per-pass render state and queue placement.
            if (const auto v = (*t)["shading_rate"].value<std::string_view>())
            {
                if (!to_shading_rate(*v, p.shading_rate)) { set_where(where, *v); return FrameCookError::UnknownShadingRate; }
            }
            if (const auto v = (*t)["rate_combiner"].value<std::string_view>())
            {
                if (!to_rate_combiner(*v, p.rate_combiner)) { set_where(where, *v); return FrameCookError::UnknownRateCombiner; }
            }
            if (const auto v = (*t)["conservative"].value<std::string_view>())
            {
                if (!to_conservative(*v, p.conservative)) { set_where(where, *v); return FrameCookError::UnknownConservative; }
            }
            if (const auto v = (*t)["queue"].value<std::string_view>())
            {
                if (!to_queue(*v, p.queue)) { set_where(where, *v); return FrameCookError::UnknownQueue; }
            }
            // ── ⭐ REN-38-B8: the pass's sampler. Any one field present makes the pass sampler-declaring. ──
            if (const auto v = (*t)["filter"].value<std::string_view>())
            {
                // NOTE: `filter` is ALSO the blit filter (38-A6). A blit is not a sampled draw and a sampled draw
                // is not a blit, so the two never collide on one pass — and reusing the word keeps the asset from
                // having `filter` and `sample_filter` mean nearly the same thing in different places.
                crd::gpu::SamplerFilter sf{};
                if (to_sampler_filter(*v, sf))
                {
                    p.sampler.min_filter = sf;
                    p.sampler.mag_filter = sf;
                    p.sampler.mip_filter = sf;
                    p.has_sampler        = true;
                }
            }
            if (const auto v = (*t)["address"].value<std::string_view>())
            {
                if (!to_sampler_address(*v, p.sampler.address))
                {
                    set_where(where, *v);
                    return FrameCookError::UnknownSamplerAddress;
                }
                p.has_sampler = true;
            }
            if (const auto v = (*t)["anisotropy"].value<int64_t>())
            {
                p.sampler.anisotropy = static_cast<crd::u32>(*v > 0 ? *v : 1);
                p.has_sampler        = true;
            }
            if (const auto v = (*t)["mip_bias"].value<double>())
            {
                p.sampler.mip_bias = static_cast<float>(*v);
                p.has_sampler      = true;
            }
            if (const auto v = (*t)["compare"].value<bool>())
            {
                p.sampler.compare = *v;
                p.has_sampler     = true;
            }
            if (const auto* barr = (*t)["blend"].as_array())
            {
                for (const auto& b : *barr)
                {
                    const auto bs = b.value<std::string_view>();
                    crd::gpu::BlendMode bm{};
                    if (!bs || !to_blend(*bs, bm)) { set_where(where, bs ? *bs : ""); return FrameCookError::UnknownBlend; }
                    p.blend.push_back(bm);
                }
            }
            if (const auto v = (*t)["material_pass"].value<std::string_view>())
            {
                if (!to_material_pass(*v, p.material_pass)) { set_where(where, *v); return FrameCookError::UnknownMaterialPass; }
            }
            if (const auto v = (*t)["for_each"].value<std::string_view>())
            {
                if (!to_for_each(*v, p.for_each, p.for_each_arg)) { set_where(where, *v); return FrameCookError::UnknownForEach; }
            }
            if (const auto v = (*t)["depth"].value<std::string_view>())
            {
                if (!to_compare(*v, p.depth)) { set_where(where, *v); return FrameCookError::UnknownCompare; }
            }
            // ── ⭐ REN-38 audit: the PASS-STATE vocabulary. Every default is the historical hardwired value. ──
            if (const auto v = (*t)["depth_write"].value<bool>()) { p.state.depth_write = *v; }
            if (const auto v = (*t)["depth_bias"].value<double>()) { p.state.depth_bias = static_cast<float>(*v); }
            if (const auto v = (*t)["depth_bias_slope"].value<double>())
            {
                p.state.depth_bias_slope = static_cast<float>(*v);
            }
            if (const auto v = (*t)["depth_bias_clamp"].value<double>())
            {
                p.state.depth_bias_clamp = static_cast<float>(*v);
            }
            if (const auto v = (*t)["face_cull"].value<std::string_view>())
            {
                if (!to_face_cull(*v, p.state.face_cull)) { set_where(where, *v); return FrameCookError::UnknownFaceCull; }
            }
            if (const auto v = (*t)["front_face"].value<std::string_view>())
            {
                if (!to_front_face(*v, p.state.front_face)) { set_where(where, *v); return FrameCookError::UnknownFrontFace; }
            }
            // REN-38-F11: this pass LOADS its target instead of clearing (mask-then-test pass pairs)
            if (const auto v = (*t)["load"].value<bool>()) { p.load_target = *v; }
            // REN-40-G1: load DEPTH only (clear colour) — the depth-prepass pattern
            if (const auto v = (*t)["load_depth"].value<bool>()) { p.load_depth = *v; }
            // REN-40-G3: a separate depth image used as the depth attachment
            if (const auto v = (*t)["shared_depth"].value<std::string_view>()) { set_str(p.shared_depth, *v); }
            if (const auto v = (*t)["depth_as_float"].value<bool>()) { p.depth_as_float = *v; }
            if (const auto v = (*t)["untracked_storage"].value<bool>()) { p.untracked_storage = *v; }
            if (const auto v = (*t)["stencil"].value<bool>()) { p.state.stencil_enable = *v; }
            if (const auto v = (*t)["stencil_compare"].value<std::string_view>())
            {
                if (!to_compare(*v, p.state.stencil_compare)) { set_where(where, *v); return FrameCookError::UnknownCompare; }
                p.state.stencil_enable = true;
            }
            if (const auto v = (*t)["stencil_ref"].value<int64_t>())
            {
                // ⛔ Stencil is EIGHT BITS on every backend; a reference of 256 silently truncating to 0 would
                // make a portal pass mark one value and test another — refused by name instead.
                if (*v < 0 || *v > 255) { set_where(where, p.name.size() > 0U ? p.name.c_str() : "stencil_ref"); return FrameCookError::BadStencilValue; }
                p.state.stencil_ref    = static_cast<crd::u32>(*v);
                p.state.stencil_enable = true;
            }
            if (const auto v = (*t)["stencil_read_mask"].value<int64_t>())
            {
                if (*v < 0 || *v > 255) { set_where(where, p.name.size() > 0U ? p.name.c_str() : "stencil_read_mask"); return FrameCookError::BadStencilValue; }
                p.state.stencil_read_mask = static_cast<crd::u32>(*v);
                p.state.stencil_enable    = true;
            }
            if (const auto v = (*t)["stencil_write_mask"].value<int64_t>())
            {
                if (*v < 0 || *v > 255) { set_where(where, p.name.size() > 0U ? p.name.c_str() : "stencil_write_mask"); return FrameCookError::BadStencilValue; }
                p.state.stencil_write_mask = static_cast<crd::u32>(*v);
                p.state.stencil_enable     = true;
            }
            if (const auto v = (*t)["stencil_fail"].value<std::string_view>())
            {
                if (!to_stencil_op(*v, p.state.stencil_fail)) { set_where(where, *v); return FrameCookError::UnknownStencilOp; }
                p.state.stencil_enable = true;
            }
            if (const auto v = (*t)["stencil_depth_fail"].value<std::string_view>())
            {
                if (!to_stencil_op(*v, p.state.stencil_depth_fail)) { set_where(where, *v); return FrameCookError::UnknownStencilOp; }
                p.state.stencil_enable = true;
            }
            if (const auto v = (*t)["stencil_pass"].value<std::string_view>())
            {
                if (!to_stencil_op(*v, p.state.stencil_pass)) { set_where(where, *v); return FrameCookError::UnknownStencilOp; }
                p.state.stencil_enable = true;
            }
            if (const auto* cc = (*t)["clear_color"].as_array())
            {
                p.has_clear_color = true;
                crd::u32 i = 0;
                for (const auto& c : *cc)
                {
                    if (i < 4U) { p.clear_color[i++] = static_cast<float>(c.value_or<double>(0.0)); }
                }
            }
            if (const auto cd = (*t)["clear_depth"].value<double>())
            {
                p.has_clear_depth = true;
                p.clear_depth     = static_cast<float>(*cd);
            }
            if (const toml::table* pp = (*t)["params"].as_table())
            {
                for (const auto& [k, v] : *pp)
                {
                    FrameParam prm(alloc);
                    set_str(prm.name, std::string_view(k.str().data(), k.str().size()));
                    if (const auto* av = v.as_array())
                    {
                        prm.type = FrameParamType::Vec4;
                        crd::u32 i = 0;
                        for (const auto& e : *av) { if (i < 4U) { prm.v[i++] = e.value_or<double>(0.0); } }
                    }
                    else if (v.is_boolean())      { prm.type = FrameParamType::Bool;  prm.v[0] = v.value_or(false) ? 1.0 : 0.0; }
                    else if (v.is_integer())      { prm.type = FrameParamType::Int;   prm.v[0] = static_cast<double>(v.value_or<int64_t>(0)); }
                    else                          { prm.type = FrameParamType::Float; prm.v[0] = v.value_or<double>(0.0); }
                    p.params.push_back(static_cast<FrameParam&&>(prm));
                }
            }
            out.passes.push_back(static_cast<FramePassDesc&&>(p));
        }
    }

    return validate_frame_graph(out, where);
}

FrameCookError validate_frame_graph(const FrameGraphDesc& desc, crd::containers::String* where)
{
    auto* alloc = desc.resources.allocator();
    // ── VALIDATION. Every rejection is named, at COOK time — never at runtime on a user's machine. ──
    const auto find_resource = [&](const crd::containers::String& n) -> const FrameResourceDesc* {
        for (crd::usize i = 0; i < desc.resources.size(); ++i)
        {
            if (desc.resources[i].name.size() == n.size()
                && std::memcmp(desc.resources[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                return &desc.resources[i];
            }
        }
        return nullptr;
    };
    const auto is_output = [](const crd::containers::String& n) { return str_eq(n, "@output"); };
    // ⛔ REN-37.6: an `@`-prefixed name is an EXTERNAL SENTINEL, not a graph resource. `@output` was always
    // one; a SUBGRAPH additionally has `@input`-style parameters its includer binds. Neither is declared by the
    // graph that names it, so neither can be checked against the resource table - that check belongs to the
    // FLATTENED graph, after binding has resolved them.
    const auto is_sentinel = [](const crd::containers::String& n) {
        return n.size() > 0U && n.c_str()[0] == '@';
    };
    // A graph WITH INCLUDES is legitimately PARTIAL: its `@output` may be produced by an included subgraph, and
    // its resources may be consumed by one. Deferring the completeness checks to `flatten_frame_graph` - which
    // runs this same validator on the result - is what lets a subgraph be authored and shipped on its own.
    const bool composed = desc.includes.size() > 0U;

    // REN-3.2: the layer-count bound lives HERE, in the validator, not in the TOML parser — a PROGRAMMATIC graph
    // never passes through the parser, and the hard rule is that the two provenances are equal. A check only the
    // text path performed would make the ergonomic path the unsafe one.
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        if (r.kind == FrameResourceKind::TransientImage
            && (r.layers == 0U || r.layers > crd::gpu::kFgMaxImageLayers))
        {
            set_where(where, std::string_view(r.name.c_str(), r.name.size()));
            return FrameCookError::LayersOutOfRange;
        }
        // ── ⭐ REN-38-B3: what a STRUCTURED / COUNTER buffer may say. ──
        if (r.kind == FrameResourceKind::StructuredBuffer || r.kind == FrameResourceKind::CounterBuffer)
        {
            const auto ew = std::string_view(r.name.c_str(), r.name.size());
            // ⛔ Elements with no SIZE. DX12 carries the stride in the UAV, and a wrong one reads every element at
            // the wrong offset — an error that GROWS with the index, so element 0 looks right and element 1000 is
            // nonsense. There is no safe default to pick here, so there is none.
            if (r.stride == 0U) { set_where(where, ew); return FrameCookError::StructuredNeedsStride; }
            // ⛔ Both APIs require a 4-byte-aligned structure stride. Rounding it up silently would change the
            // element the shader lands on; refusing names the resource while the author can still fix it.
            if ((r.stride % 4U) != 0U) { set_where(where, ew); return FrameCookError::StrideNotAligned; }
        }
        // ── ⭐ REN-38-B1: what a PERSISTENT / PING-PONG resource may say. ──
        if (r.kind == FrameResourceKind::PersistentImage || r.kind == FrameResourceKind::PingPongImage)
        {
            // ⛔ An ABSOLUTE size, never `scale` alone — UNLESS `resizable = true`. A persistent image is looked up
            // by a STABLE KEY across frames, and a scale-relative extent changes the moment the output resizes,
            // which recreates the image and DISCARDS the history, silently, mid-session. The author must state the
            // size they mean — OR explicitly opt into the discard with `resizable` (⭐ REN-41: what a TAA history
            // buffer WANTS — it follows the window and reconverges in a few frames after a resize).
            if ((r.width == 0U || r.height == 0U) && !(r.resizable && r.scale > 0.0F))
            {
                set_where(where, std::string_view(r.name.c_str(), r.name.size()));
                return FrameCookError::PersistentNeedsSize;
            }
            // ⛔ A PING-PONG resource that is only read, or only written, never rotates — so it is a persistent
            // image the author mislabelled, and every frame would read the same stale image forever.
            if (r.kind == FrameResourceKind::PingPongImage)
            {
                bool read = false;
                bool wrote = false;
                for (crd::usize pi2 = 0; pi2 < desc.passes.size(); ++pi2)
                {
                    for (crd::usize k = 0; k < desc.passes[pi2].reads.size(); ++k)
                    {
                        if (str_eq(r.name, std::string_view(desc.passes[pi2].reads[k].name.c_str(),
                                                            desc.passes[pi2].reads[k].name.size()))) { read = true; }
                    }
                    for (crd::usize k = 0; k < desc.passes[pi2].writes.size(); ++k)
                    {
                        if (str_eq(r.name, std::string_view(desc.passes[pi2].writes[k].name.c_str(),
                                                            desc.passes[pi2].writes[k].name.size()))) { wrote = true; }
                    }
                }
                if (!(read && wrote) && !composed)
                {
                    set_where(where, std::string_view(r.name.c_str(), r.name.size()));
                    return FrameCookError::PingPongNeedsBothWays;
                }
            }
        }
        // ── ⭐ REN-38-B2: what a SHAPED resource may say. ──
        if (r.kind == FrameResourceKind::TransientImage)
        {
            const auto ew = std::string_view(r.name.c_str(), r.name.size());
            // ⛔ A CUBE FACE MUST BE SQUARE — the hardware has no other shape for one, so a non-square request is
            // either silently squashed or refused at creation. Refusing it HERE names the resource.
            if ((r.kind_2d == crd::gpu::FgImageKind::Cube || r.kind_2d == crd::gpu::FgImageKind::CubeArray)
                && r.width != r.height)
            {
                set_where(where, ew);
                return FrameCookError::CubeNeedsSquare;
            }
            // ⛔ A VOLUME with no depth is a 2-D texture the author believes is a volume; every froxel index into
            // it would then read slice 0. `depth = 1` is a legal volume, so only ZERO is the mistake.
            if (r.kind_2d == crd::gpu::FgImageKind::Tex3D && r.depth == 0U)
            {
                set_where(where, ew);
                return FrameCookError::VolumeNeedsDepth;
            }
            // ⛔ MIPS: 0 is not "full chain" — guessing what an author meant is how a bloom chain silently gets a
            // different length than the technique reading it expects. And a chain cannot outlive its extent: the
            // levels halve to 1x1 and no further, so more levels than that is a request the device must refuse.
            if (r.mips == 0U) { set_where(where, ew); return FrameCookError::BadMipCount; }
            crd::u32 ext = r.width > r.height ? r.width : r.height;
            if (ext == 0U && r.scale > 0.0F) { ext = 0U; } // scale-relative: the runtime checks against the output
            if (ext != 0U)
            {
                crd::u32 max_mips = 1U;
                while ((ext >> max_mips) != 0U) { ++max_mips; }
                if (r.mips > max_mips) { set_where(where, ew); return FrameCookError::BadMipCount; }
            }
        }
    }

    // ── ⭐ REN-38-B4: an ACCELERATION STRUCTURE is EXTERNAL. ──
    // ⛔ It is the host's, like `@output`: building a BLAS/TLAS needs scene geometry, which lives in the World,
    // which this module must never depend on. So a size or a format on one is not a harmless extra field — it
    // means the author believed the graph would ALLOCATE it, and every later question ("why is it empty?") starts
    // from that wrong belief. Rejected while the file is still open.
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        if (r.kind != FrameResourceKind::AccelerationStructure) { continue; }
        if (r.size_bytes != 0U || r.width != 0U || r.height != 0U || r.scale > 0.0F)
        {
            set_where(where, std::string_view(r.name.c_str(), r.name.size()));
            return FrameCookError::AccelIsExternal;
        }
    }

    bool wrote_output = false;
    for (crd::usize pi = 0; pi < desc.passes.size(); ++pi)
    {
        const FramePassDesc& p = desc.passes[pi];
        if ((p.kind == FramePassKind::RasterGeometry || p.kind == FramePassKind::RasterDepthOnly
             || p.kind == FramePassKind::RasterMrt)
            && p.draw_list.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingDrawList;
        }
        if (p.kind == FramePassKind::RasterFullscreen && p.shader.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingShader;
        }
        // REN-38-F11: `load = true` is honoured only by kinds with load draw verbs — anywhere else it would
        // silently clear, which is the exact wrongness the flag exists to prevent.
        if (p.load_target && p.kind != FramePassKind::RasterGeometry)
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::LoadNeedsGeometry;
        }
        // REN-40-G1: `load_depth` shares the same kind restriction and is mutually exclusive with `load`
        if (p.load_depth && p.kind != FramePassKind::RasterGeometry)
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::LoadNeedsGeometry;
        }
        if (p.load_depth && p.load_target)
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::LoadNeedsGeometry;
        }
        // REN-40-G3: `shared_depth` is only valid on raster passes and must reference a declared depth resource
        if (p.shared_depth.size() > 0U)
        {
            const bool is_raster = p.kind == FramePassKind::RasterGeometry || p.kind == FramePassKind::RasterMrt
                                   || p.kind == FramePassKind::RasterDepthOnly;
            if (!is_raster)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::UnknownResource;
            }
            bool found_depth_res = false;
            for (crd::usize ri = 0; ri < desc.resources.size(); ++ri)
            {
                if (desc.resources[ri].name.size() == p.shared_depth.size()
                    && std::memcmp(desc.resources[ri].name.c_str(), p.shared_depth.c_str(), p.shared_depth.size()) == 0)
                {
                    found_depth_res = crd::gpu::fg_format_has_depth(desc.resources[ri].format);
                    break;
                }
            }
            if (!found_depth_res)
            {
                set_where(where, std::string_view(p.shared_depth.c_str(), p.shared_depth.size()));
                return FrameCookError::UnknownResource;
            }
        }
        if (p.kind == FramePassKind::Compute && p.kernel.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingShader;
        }
        // ── ⭐ REN-38-A14: only COMPUTE-SHAPED work can go on the async-compute queue. ──
        // ⛔ A compute queue cannot rasterise. A raster pass that asked for it would either be silently moved back
        // to graphics (a perf claim the frame never delivered) or submitted somewhere it cannot run. Rejected.
        if (p.queue == FrameQueue::Async && p.kind != FramePassKind::Compute
            && p.kind != FramePassKind::ComputeIndirect && p.kind != FramePassKind::RayTrace)
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::AsyncQueueNeedsCompute;
        }
        // ── ⭐ REN-38-B5: an EXTERNAL TEXTURE is READ-ONLY. ──
        // ⛔ A pass that writes one would have the graph schedule a barrier and a layout transition on content the
        // APPLICATION owns and may be updating from another thread — so the write is rejected, by pass name,
        // rather than producing a frame that races with whoever fills the atlas.
        for (crd::usize w = 0; w < p.writes.size(); ++w)
        {
            const FrameResourceDesc* wr = find_resource(p.writes[w].name);
            if (wr != nullptr && wr->kind == FrameResourceKind::ExternalTexture)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::ExternalTextureIsReadOnly;
            }
        }
        // ── ⭐ REN-38-A11: what a VISIBILITY-BUFFER pass may say. ──
        // ⛔ Its target MUST be R32Uint. A primitive id written into an RGBA8 attachment is quantised to 8 bits
        // per channel, so ids beyond 255 alias onto each other and the deferred materialisation shades the WRONG
        // MESH — a plausible picture with the wrong materials, which is far worse than a black one.
        if (p.kind == FramePassKind::RasterVisbuffer)
        {
            if (p.draw_list.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::MissingDrawList;
            }
            for (crd::usize w = 0; w < p.writes.size(); ++w)
            {
                const FrameResourceDesc* rd = find_resource(p.writes[w].name);
                if (rd != nullptr && rd->format != crd::gpu::FgImageFormat::R32Uint)
                {
                    set_where(where, std::string_view(p.writes[w].name.c_str(), p.writes[w].name.size()));
                    return FrameCookError::VisbufferNeedsUintTarget;
                }
            }
        }
        // ── ⭐ REN-38-A12: what a COMPOSITE pass may say. ──
        // ⛔ A composite with no blend is a fullscreen pass that OVERWRITES — which is exactly the bug this kind
        // exists to prevent, and it would look like "the transparency layer is opaque" rather than like a missing
        // declaration. So the blend is REQUIRED, not defaulted.
        if (p.kind == FramePassKind::RasterComposite)
        {
            if (p.shader.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::MissingShader;
            }
            if (p.blend.size() == 0U || p.blend[0] == crd::gpu::BlendMode::Opaque)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::CompositeNeedsBlend;
            }
        }
        // ── ⭐ REN-38-A9: what a RAY-TRACING pass may say. ──
        // It names a KERNEL (an inline-ray-query compute shader) and READS an acceleration structure. ⛔ Without
        // the AS it would traverse nothing and every ray would miss — a black image that looks exactly like a
        // scene with no geometry, which is the single hardest RT bug to attribute.
        // ── ⭐ REN-38-A16: a ray-tracing PIPELINE needs ALL THREE programs and an acceleration structure. ──
        // ⛔ Two of three is not a degraded pipeline, it is an INVALID state object — and a missing miss shader in
        // particular produces rays that hit nothing and write nothing, which reads as an empty scene.
        if (p.kind == FramePassKind::RayTracePipeline)
        {
            if (p.raygen.empty() || p.miss.empty() || p.closest_hit.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::RtPipelineNeedsThree;
            }
            bool has_as = false;
            for (crd::usize r = 0; r < p.reads.size() && !has_as; ++r)
            {
                const FrameResourceDesc* rd = find_resource(p.reads[r].name);
                has_as = rd != nullptr && rd->kind == FrameResourceKind::AccelerationStructure;
            }
            if (!has_as)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::RayTraceNeedsAccel;
            }
        }
        if (p.kind == FramePassKind::RayTrace)
        {
            if (p.kernel.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::MissingShader;
            }
            bool has_as = false;
            for (crd::usize r = 0; r < p.reads.size() && !has_as; ++r)
            {
                const FrameResourceDesc* rd = find_resource(p.reads[r].name);
                has_as = rd != nullptr && rd->kind == FrameResourceKind::AccelerationStructure;
            }
            if (!has_as)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::RayTraceNeedsAccel;
            }
        }
        // ── ⭐ REN-38-A10: what an INDIRECT pass may say. ──
        // The `args` parameter names the buffer holding the count. ⛔ It must exist AND be an `indirect_args`
        // resource: an ordinary transient buffer is created without the INDIRECT usage flag, so binding one as
        // arguments is invalid on Vulkan and an untransitionable state on D3D12 — and neither flag can be added
        // after creation. Catching the kind here is the only place it can be caught before the device refuses.
        // ⭐⭐ RAF-10: a CUSTOM pass must NAME its registered executor id — an empty `executor` is a pass with no
        // mechanic, caught here rather than as a silent no-op at record time (the loud-failure discipline).
        if (p.kind == FramePassKind::Custom && p.executor.empty())
        {
            set_where(where, std::string_view(p.name.c_str(), p.name.size()));
            return FrameCookError::MissingShader;
        }
        if (p.kind == FramePassKind::ComputeIndirect || p.kind == FramePassKind::RasterMeshIndirect)
        {
            if (p.kind == FramePassKind::ComputeIndirect && p.kernel.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::MissingShader;
            }
            if (p.kind == FramePassKind::RasterMeshIndirect && p.shader.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::MissingShader;
            }
            const FrameResourceDesc* args = nullptr;
            for (crd::usize r = 0; r < p.reads.size() && args == nullptr; ++r)
            {
                const FrameResourceDesc* rd = find_resource(p.reads[r].name);
                if (rd != nullptr && rd->kind == FrameResourceKind::IndirectArgs) { args = rd; }
            }
            if (args == nullptr)
            {
                // Distinguish "named nothing" from "named the wrong kind" — they are different mistakes and the
                // author fixes them differently.
                bool any_read = p.reads.size() > 0U;
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return any_read ? FrameCookError::IndirectArgsNotArgs : FrameCookError::IndirectNeedsArgs;
            }
        }
        // ── ⭐ REN-38-A7 / A8: what an AMPLIFICATION pass may say. ──
        // The program is named exactly as a fullscreen pass names one — the HOST decides whether that id resolves
        // to a `create_tess_program` or a `create_mesh_program`, because which stages a cooked program carries is
        // a property of the PROGRAM, not of the graph. What the graph must state is WHAT TO DISPATCH.
        if (p.kind == FramePassKind::RasterTess || p.kind == FramePassKind::RasterMesh)
        {
            if (p.shader.empty())
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::MissingShader;
            }
            // ⛔ A DRAW LIST or an explicit count — one or the other, never neither. With neither, the runtime
            // would dispatch ZERO patches / ZERO workgroups: a black image, no error, and nothing in the asset to
            // point at. `patches` / `groups` are PARAMETERS (a dispatch size is not topology), and a draw list
            // wins when both are present because a scene-driven pass is per-mesh by definition.
            bool has_count = !p.draw_list.empty();
            for (crd::usize k = 0; !has_count && k < p.params.size(); ++k)
            {
                const std::string_view pn(p.params[k].name.c_str(), p.params[k].name.size());
                if ((pn == "patches" || pn == "groups") && p.params[k].v[0] > 0.0) { has_count = true; }
            }
            if (!has_count)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::AmplifyNeedsCount;
            }
        }
        // ── ⭐ REN-38-A6: what a UTILITY pass may say. ──
        // ⛔ EXACTLY ONE source and EXACTLY ONE destination for copy/blit/resolve. Zero of either has nothing to
        // do; two of either means the executor picks one and the author is never told which. Both are silent, and
        // "the image looks almost right" is the worst possible symptom to debug.
        if (p.kind == FramePassKind::Copy || p.kind == FramePassKind::Blit || p.kind == FramePassKind::Resolve)
        {
            if (p.reads.size() != 1U)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::TransferNeedsOneRead;
            }
            if (p.writes.size() != 1U)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::TransferNeedsOneWrite;
            }
        }
        // A clear PRODUCES a target and consumes nothing. A declared read would make the dependency sort order
        // this pass AFTER whoever wrote that resource, for no reason — and, worse, would read as intent.
        if (p.kind == FramePassKind::Clear)
        {
            if (p.writes.size() == 0U)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::TransferNeedsOneWrite;
            }
            if (p.reads.size() != 0U)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::ClearReadsNothing;
            }
        }
        // ── ⭐ REN-38-A5: what a PRESENT pass may say. ──
        // A present is a SINK: it consumes one finished canvas and produces nothing. All three rules below reject
        // a graph that would otherwise build, execute and put a wrong (or no) image on screen:
        if (p.kind == FramePassKind::Present)
        {
            // ⛔ EXACTLY ONE read. Zero ⇒ nothing to present and the executor would have to guess `@output`;
            // two ⇒ the executor picks one and the author never learns which. Both are silent.
            if (p.reads.size() != 1U)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::PresentNeedsOneRead;
            }
            // A write would make a present look like a producer to the dependency sort, so a later pass could be
            // ordered AFTER the frame was already on screen.
            if (p.writes.size() != 0U)
            {
                set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                return FrameCookError::PresentWritesNothing;
            }
            // ⛔ THE SOURCE MUST OUTLIVE THE GRAPH. A transient's memory is ALIASED and retired the instant its
            // last reader finishes — by the time the surface blits, another transient may legally own those
            // bytes. Rejected at COOK time, because at run time it is a garbage frame, not a crash.
            if (!is_sentinel(p.reads[0].name))
            {
                set_where(where, std::string_view(p.reads[0].name.c_str(), p.reads[0].name.size()));
                return FrameCookError::PresentSourceInternal;
            }
        }
        const auto check_refs = [&](const crd::containers::Array<FrameResourceRef>& refs) -> FrameCookError {
            for (crd::usize i = 0; i < refs.size(); ++i)
            {
                if (is_sentinel(refs[i].name)) { continue; } // `@output` and subgraph `@`-parameters
                const FrameResourceDesc* r = find_resource(refs[i].name);
                if (r == nullptr)
                {
                    set_where(where, std::string_view(refs[i].name.c_str(), refs[i].name.size()));
                    return FrameCookError::UnknownResource;
                }
                if (refs[i].indexed && r->layers <= 1U)
                {
                    set_where(where, std::string_view(refs[i].name.c_str(), refs[i].name.size()));
                    return FrameCookError::SubscriptOnNonLayered;
                }
                if (refs[i].indexed && p.for_each == FrameForEach::None)
                {
                    set_where(where, std::string_view(p.name.c_str(), p.name.size()));
                    return FrameCookError::IndexWithoutForEach;
                }
            }
            return FrameCookError::Ok;
        };
        const FrameCookError e1 = check_refs(p.reads);
        if (e1 != FrameCookError::Ok) { return e1; }
        const FrameCookError e2 = check_refs(p.writes);
        if (e2 != FrameCookError::Ok) { return e2; }
        for (crd::usize i = 0; i < p.writes.size(); ++i)
        {
            if (is_output(p.writes[i].name)) { wrote_output = true; }
        }
    }
    if (!wrote_output && !composed) { return FrameCookError::NoOutputPass; }

    // every DECLARED resource must be produced by some pass (an unwritten transient is dead weight and, more
    // importantly, a sign the author mistyped a name — `build()` already rejects it at runtime; we reject earlier)
    for (crd::usize ri = 0; ri < desc.resources.size(); ++ri)
    {
        // ⛔ REN-38-B4: an ACCELERATION STRUCTURE is EXEMPT — it is external (the host built it), so no pass in
        // this graph writes it and requiring one would make every ray-tracing asset invalid. The rule this loop
        // enforces is "a resource the GRAPH owns must have a producer"; an AS is not one the graph owns.
        // ⛔ REN-38-B5: an EXTERNAL TEXTURE is the host's too — read-only content with its own update schedule.
        if (desc.resources[ri].kind == FrameResourceKind::ExternalTexture) { continue; }
        // ⛔ An ACCELERATION STRUCTURE and an EXTERNAL BUFFER are both the HOST's — no pass in this graph writes
        // them, and demanding a producer would make every ray-tracing asset, and every graph that consumes scene
        // geometry, invalid. The rule is "a resource the GRAPH owns must have a producer"; neither is one.
        if (desc.resources[ri].kind == FrameResourceKind::AccelerationStructure
            || desc.resources[ri].kind == FrameResourceKind::ExternalBuffer)
        {
            continue;
        }
        // ⛔ REN-38-B1: a PERSISTENT image need not be written THIS frame — that is the entire point. A cached
        // thumbnail or a converged accumulation buffer is read for many frames and rewritten only when something
        // changed, so demanding a producer would forbid exactly the steady state the row exists to make possible.
        if (desc.resources[ri].kind == FrameResourceKind::PersistentImage
            || desc.resources[ri].kind == FrameResourceKind::PingPongImage)
        {
            continue;
        }
        bool written = false;
        for (crd::usize pi = 0; pi < desc.passes.size() && !written; ++pi)
        {
            for (crd::usize wi = 0; wi < desc.passes[pi].writes.size(); ++wi)
            {
                if (str_eq(desc.resources[ri].name,
                           std::string_view(desc.passes[pi].writes[wi].name.c_str(), desc.passes[pi].writes[wi].name.size())))
                {
                    written = true;
                    break;
                }
            }
        }
        if (!written && !composed)
        {
            set_where(where, std::string_view(desc.resources[ri].name.c_str(), desc.resources[ri].name.size()));
            return FrameCookError::ResourceNeverWritten;
        }
    }

    // CYCLE detection — Kahn's algorithm over the pass DAG.
    // RAW: pass A reads something an EARLIER writer B produces → A depends on B (writer precedes reader).
    // WAR: pass A reads something a LATER writer B overwrites → B depends on A — but ONLY when the read is
    //      satisfiable at A (the resource has a frame-start value or an earlier producer); otherwise the "later
    //      writer" IS the only producer and it is a forward RAW (B precedes A), which is how a real cycle shows.
    // WAW: pass A writes something pass B also writes AND A < B → B depends on A (declaration order).
    const crd::usize np = desc.passes.size();
    crd::containers::Array<crd::u32> indeg(alloc);
    indeg.resize(np, 0U);
    crd::containers::Array<crd::u8> edge(alloc);
    edge.resize(np * np, 0U);
    const auto add_dep = [&](crd::usize from, crd::usize to) {
        if (from == to) { return; }
        if (edge[(from * np) + to] == 0U) { edge[(from * np) + to] = 1U; ++indeg[to]; }
    };
    // ── ⭐⭐ REN-41: WHEN "read then a LATER pass writes it" is a WAR (no cycle) vs a forward RAW (a cycle). ──
    // A read that matches a writer declared AFTER the reader is a legitimate WAR (reader-before-writer, no
    // dependency edge from writer→reader) ONLY when the resource already HAS A VALUE at that point: it is
    // host-provided (external buffer / texture / acceleration structure) or CROSS-FRAME (persistent / ping-pong —
    // TAA history is READ old by taa_resolve and WRITTEN new by the later taa_store), OR some pass WRITES it
    // BEFORE the reader (the nearest earlier writer is the producer; the later write feeds a subsequent reader —
    // the two-phase occlusion re-cull writes `instances` / `cull_args` again after the depth prepass read them).
    // Otherwise it is an UNSATISFIABLE forward reference on a single-frame resource: the writer MUST precede the
    // reader (a genuine RAW), and when two passes each read what the other writes, that pair is the real cycle the
    // DependencyCycle gate must catch — which declaration order alone (the previous heuristic) silently allowed.
    const auto has_frame_start_value = [&](const crd::containers::String& name) -> bool {
        for (crd::usize i = 0; i < desc.resources.size(); ++i)
        {
            if (str_eq(name, std::string_view(desc.resources[i].name.c_str(), desc.resources[i].name.size())))
            {
                const FrameResourceKind k = desc.resources[i].kind;
                return k == FrameResourceKind::ExternalBuffer || k == FrameResourceKind::ExternalTexture
                       || k == FrameResourceKind::AccelerationStructure || k == FrameResourceKind::PersistentImage
                       || k == FrameResourceKind::PingPongImage;
            }
        }
        return false; // `@output` and any un-declared name: no frame-start value
    };
    const auto written_before = [&](const crd::containers::String& name, crd::usize before) -> bool {
        for (crd::usize q = 0; q < before; ++q)
        {
            for (crd::usize w = 0; w < desc.passes[q].writes.size(); ++w)
            {
                if (str_eq(name, std::string_view(desc.passes[q].writes[w].name.c_str(),
                                                  desc.passes[q].writes[w].name.size())))
                {
                    return true;
                }
            }
        }
        return false;
    };
    for (crd::usize a = 0; a < np; ++a)
    {
        for (crd::usize b = 0; b < np; ++b)
        {
            if (a == b) { continue; }
            for (crd::usize r = 0; r < desc.passes[a].reads.size(); ++r)
            {
                const crd::containers::String& rn = desc.passes[a].reads[r].name;
                bool                           matched = false;
                for (crd::usize w = 0; w < desc.passes[b].writes.size(); ++w)
                {
                    if (str_eq(rn, std::string_view(desc.passes[b].writes[w].name.c_str(),
                                                    desc.passes[b].writes[w].name.size())))
                    {
                        matched = true;
                        break;
                    }
                }
                if (!matched) { continue; }
                // A LATER writer (b > a) is a legitimate WAR (reader before writer, edge a→b) only when the read
                // is satisfiable — the resource has a frame-start value or an earlier producer this frame.
                // Otherwise, and for any EARLIER writer, the writer must precede the reader (edge b→a): an ordinary
                // RAW, or the forward-reference RAW on a producer-less single-frame resource that surfaces a cycle.
                const bool war = b > a && (has_frame_start_value(rn) || written_before(rn, a));
                if (war) { add_dep(a, b); }
                else     { add_dep(b, a); }
            }
        }
    }
    for (crd::usize a = 0; a < np; ++a)
    {
        for (crd::usize b = a + 1; b < np; ++b)
        {
            bool waw = false;
            for (crd::usize wa = 0; wa < desc.passes[a].writes.size() && !waw; ++wa)
            {
                for (crd::usize wb = 0; wb < desc.passes[b].writes.size(); ++wb)
                {
                    if (str_eq(desc.passes[a].writes[wa].name,
                               std::string_view(desc.passes[b].writes[wb].name.c_str(), desc.passes[b].writes[wb].name.size())))
                    {
                        waw = true;
                        break;
                    }
                }
            }
            if (waw) { add_dep(a, b); } // WAW: later writer b depends on earlier writer a
        }
    }
    crd::containers::Array<crd::u32> queue(alloc);
    for (crd::usize i = 0; i < np; ++i)
    {
        if (indeg[i] == 0U) { queue.push_back(static_cast<crd::u32>(i)); }
    }
    crd::usize visited = 0;
    for (crd::usize qi = 0; qi < queue.size(); ++qi)
    {
        const crd::u32 n = queue[qi];
        ++visited;
        for (crd::usize a = 0; a < np; ++a)
        {
            if (edge[(static_cast<crd::usize>(n) * np) + a] != 0U && --indeg[a] == 0U)
            {
                queue.push_back(static_cast<crd::u32>(a));
            }
        }
    }
    if (visited != np) { return FrameCookError::DependencyCycle; }

    return FrameCookError::Ok;
}

crd::containers::Array<crd::u8> cook_frame_graph(const FrameGraphDesc& desc, crd::memory::IAllocator* a)
{
    Bytes out(a);
    put_u32(out, kFourCC);
    put_u32(out, kBlobVersion);
    put_u32(out, desc.schema);
    put_str(out, desc.name);
    put_str(out, desc.fallback);

    put_u32(out, static_cast<crd::u32>(desc.requires_caps.size()));
    for (crd::usize i = 0; i < desc.requires_caps.size(); ++i) { put_str(out, desc.requires_caps[i]); }

    put_u32(out, static_cast<crd::u32>(desc.resources.size()));
    for (crd::usize i = 0; i < desc.resources.size(); ++i)
    {
        const FrameResourceDesc& r = desc.resources[i];
        put_str(out, r.name);
        put_u8(out, static_cast<crd::u8>(r.kind));
        put_u8(out, static_cast<crd::u8>(r.format));
        put_u32(out, r.width);
        put_u32(out, r.height);
        put_f32(out, r.scale);
        put_u32(out, r.layers);
        put_u32(out, r.samples);
        put_u8(out, r.sampled ? 1U : 0U);
        put_u8(out, r.depth_buffer ? 1U : 0U); // 38-G1
        put_u8(out, r.storage ? 1U : 0U);
        put_u32(out, r.size_bytes);
        put_u8(out, r.resizable ? 1U : 0U); // REN-41
    }

    put_u32(out, static_cast<crd::u32>(desc.draw_lists.size()));
    for (crd::usize i = 0; i < desc.draw_lists.size(); ++i)
    {
        const FrameDrawListDesc& d = desc.draw_lists[i];
        put_str(out, d.name);
        const auto put_list = [&](const crd::containers::Array<crd::containers::String>& l) {
            put_u32(out, static_cast<crd::u32>(l.size()));
            for (crd::usize k = 0; k < l.size(); ++k) { put_str(out, l[k]); }
        };
        put_list(d.all);
        put_list(d.any);
        put_list(d.none);
        put_u8(out, static_cast<crd::u8>(d.cull));
        put_u8(out, static_cast<crd::u8>(d.sort));
        put_u32(out, d.limit);
    }

    put_u32(out, static_cast<crd::u32>(desc.passes.size()));
    for (crd::usize i = 0; i < desc.passes.size(); ++i)
    {
        const FramePassDesc& p = desc.passes[i];
        put_str(out, p.name);
        put_u8(out, static_cast<crd::u8>(p.kind));
        const auto put_refs = [&](const crd::containers::Array<FrameResourceRef>& refs) {
            put_u32(out, static_cast<crd::u32>(refs.size()));
            for (crd::usize k = 0; k < refs.size(); ++k)
            {
                put_str(out, refs[k].name);
                put_u8(out, refs[k].indexed ? 1U : 0U);
            }
        };
        put_refs(p.reads);
        put_refs(p.writes);
        put_str(out, p.draw_list);
        put_str(out, p.view);
        put_str(out, p.shader);
        put_str(out, p.kernel);
        put_str(out, p.technique);
        put_u32(out, static_cast<crd::u32>(p.blend.size()));
        for (crd::usize k = 0; k < p.blend.size(); ++k) { put_u8(out, static_cast<crd::u8>(p.blend[k])); }
        put_u8(out, static_cast<crd::u8>(p.material_pass));
        put_u8(out, static_cast<crd::u8>(p.for_each));
        put_u32(out, p.for_each_arg);
        put_u8(out, p.has_clear_color ? 1U : 0U);
        for (crd::u32 c = 0; c < 4U; ++c) { put_f32(out, p.clear_color[c]); }
        put_u8(out, p.has_clear_depth ? 1U : 0U);
        put_f32(out, p.clear_depth);
        put_u8(out, static_cast<crd::u8>(p.depth));
        put_u32(out, static_cast<crd::u32>(p.params.size()));
        for (crd::usize k = 0; k < p.params.size(); ++k)
        {
            put_str(out, p.params[k].name);
            put_u8(out, static_cast<crd::u8>(p.params[k].type));
            for (crd::u32 c = 0; c < 4U; ++c) { put_f64(out, p.params[k].v[c]); }
        }
        // ── v4 (REN-38 audit): the fields v3 dropped, appended at the record's END. ──
        put_str(out, p.raygen);
        put_str(out, p.miss);
        put_str(out, p.closest_hit);
        put_str(out, p.any_hit);
        put_str(out, p.intersection); // v6 (REN-38-F13)
        put_str(out, p.callable);
        put_u8(out, static_cast<crd::u8>(p.shading_rate));
        put_u8(out, static_cast<crd::u8>(p.rate_combiner));
        put_u8(out, static_cast<crd::u8>(p.conservative));
        put_u8(out, static_cast<crd::u8>(p.queue));
        put_u8(out, static_cast<crd::u8>(p.filter));
        put_u8(out, p.has_sampler ? 1U : 0U);
        put_u8(out, static_cast<crd::u8>(p.sampler.min_filter));
        put_u8(out, static_cast<crd::u8>(p.sampler.mag_filter));
        put_u8(out, static_cast<crd::u8>(p.sampler.mip_filter));
        put_u8(out, static_cast<crd::u8>(p.sampler.address));
        put_u8(out, p.sampler.compare ? 1U : 0U);
        put_u32(out, p.sampler.anisotropy);
        put_f32(out, p.sampler.mip_bias);
        put_u8(out, p.state.depth_write ? 1U : 0U);
        put_f32(out, p.state.depth_bias);
        put_f32(out, p.state.depth_bias_slope);
        put_f32(out, p.state.depth_bias_clamp);
        put_u8(out, static_cast<crd::u8>(p.state.face_cull));
        put_u8(out, static_cast<crd::u8>(p.state.front_face));
        put_u8(out, p.state.stencil_enable ? 1U : 0U);
        put_u8(out, static_cast<crd::u8>(p.state.stencil_compare));
        put_u32(out, p.state.stencil_ref);
        put_u32(out, p.state.stencil_read_mask);
        put_u32(out, p.state.stencil_write_mask);
        put_u8(out, static_cast<crd::u8>(p.state.stencil_fail));
        put_u8(out, static_cast<crd::u8>(p.state.stencil_depth_fail));
        put_u8(out, static_cast<crd::u8>(p.state.stencil_pass));
        // v5: the pass-level load flag rides the same record (field-survival gated like the rest)
        put_u8(out, p.load_target ? 1U : 0U);
        // v6: load-depth-only flag (depth prepass pattern)
        put_u8(out, p.load_depth ? 1U : 0U);
        // v7: shared_depth — a separate depth attachment (empty string if none)
        put_str(out, p.shared_depth);
        put_u8(out, p.depth_as_float ? 1U : 0U);
        put_u8(out, p.untracked_storage ? 1U : 0U);
    }

    // REN-37.6: composition records, appended at the END.
    put_u32(out, static_cast<crd::u32>(desc.includes.size()));
    for (crd::usize i = 0; i < desc.includes.size(); ++i)
    {
        put_str(out, desc.includes[i].graph);
        put_str(out, desc.includes[i].as);
        put_u8(out, desc.includes[i].atomic ? 1U : 0U);
        put_u32(out, static_cast<crd::u32>(desc.includes[i].bind.size()));
        for (crd::usize k = 0; k < desc.includes[i].bind.size(); ++k)
        {
            put_str(out, desc.includes[i].bind[k].from);
            put_str(out, desc.includes[i].bind[k].to);
        }
    }
    put_u32(out, static_cast<crd::u32>(desc.anchors.size()));
    for (crd::usize i = 0; i < desc.anchors.size(); ++i)
    {
        put_str(out, desc.anchors[i].name);
        const auto put_list = [&](const crd::containers::Array<crd::containers::String>& l) {
            put_u32(out, static_cast<crd::u32>(l.size()));
            for (crd::usize k = 0; k < l.size(); ++k) { put_str(out, l[k]); }
        };
        put_list(desc.anchors[i].after);
        put_list(desc.anchors[i].before);
    }
    put_u32(out, static_cast<crd::u32>(desc.injects.size()));
    for (crd::usize i = 0; i < desc.injects.size(); ++i)
    {
        put_str(out, desc.injects[i].anchor);
        put_str(out, desc.injects[i].pass);
    }
    return out;
}

bool read_frame_graph(crd::containers::ConstSpan<crd::u8> bytes, FrameGraphDesc& out)
{
    Cursor c{bytes, 0, true};
    if (c.u32v() != kFourCC || !c.ok) { return false; }
    if (c.u32v() != kBlobVersion) { return false; }
    // ⛔ RESET FIRST — the same load-button scar the TOML parser above carries: deserializing into a reused
    // descriptor APPENDED every list and kept stale scalars the blob never wrote.
    out.name.clear();
    out.resources.clear();
    out.draw_lists.clear();
    out.passes.clear();
    out.requires_caps.clear();
    out.fallback.clear();
    out.includes.clear();
    out.anchors.clear();
    out.injects.clear();
    out.memory_budget_bytes = 0U;
    out.schema = c.u32v();
    c.strv(out.name);
    c.strv(out.fallback);
    auto* alloc = out.resources.allocator();

    const crd::u32 ncap = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < ncap; ++i)
    {
        crd::containers::String s(alloc);
        c.strv(s);
        out.requires_caps.push_back(static_cast<crd::containers::String&&>(s));
    }

    const crd::u32 nres = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < nres; ++i)
    {
        FrameResourceDesc r(alloc);
        c.strv(r.name);
        r.kind       = static_cast<FrameResourceKind>(c.u8v());
        r.format     = static_cast<crd::gpu::FgImageFormat>(c.u8v());
        r.width      = c.u32v();
        r.height     = c.u32v();
        r.scale      = c.f32v();
        r.layers     = c.u32v();
        r.samples    = c.u32v();
        r.sampled    = c.u8v() != 0U;
        r.depth_buffer = c.u8v() != 0U; // 38-G1
        r.storage    = c.u8v() != 0U;
        r.size_bytes = c.u32v();
        r.resizable  = c.u8v() != 0U; // REN-41
        out.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    }

    const crd::u32 ndl = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < ndl; ++i)
    {
        FrameDrawListDesc d(alloc);
        c.strv(d.name);
        const auto get_list = [&](crd::containers::Array<crd::containers::String>& l) {
            const crd::u32 n = c.u32v();
            for (crd::u32 k = 0; k < n && c.ok; ++k)
            {
                crd::containers::String s(alloc);
                c.strv(s);
                l.push_back(static_cast<crd::containers::String&&>(s));
            }
        };
        get_list(d.all);
        get_list(d.any);
        get_list(d.none);
        d.cull  = static_cast<FrameCullMode>(c.u8v());
        d.sort  = static_cast<FrameSortMode>(c.u8v());
        d.limit = c.u32v();
        out.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
    }

    const crd::u32 npass = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < npass; ++i)
    {
        FramePassDesc p(alloc);
        c.strv(p.name);
        p.kind = static_cast<FramePassKind>(c.u8v());
        const auto get_refs = [&](crd::containers::Array<FrameResourceRef>& refs) {
            const crd::u32 n = c.u32v();
            for (crd::u32 k = 0; k < n && c.ok; ++k)
            {
                FrameResourceRef r(alloc);
                c.strv(r.name);
                r.indexed = c.u8v() != 0U;
                refs.push_back(static_cast<FrameResourceRef&&>(r));
            }
        };
        get_refs(p.reads);
        get_refs(p.writes);
        c.strv(p.draw_list);
        c.strv(p.view);
        c.strv(p.shader);
        c.strv(p.kernel);
        c.strv(p.technique);
        const crd::u32 nbl = c.u32v();
        for (crd::u32 k = 0; k < nbl && c.ok; ++k) { p.blend.push_back(static_cast<crd::gpu::BlendMode>(c.u8v())); }
        p.material_pass   = static_cast<FrameMaterialPass>(c.u8v());
        p.for_each        = static_cast<FrameForEach>(c.u8v());
        p.for_each_arg    = c.u32v();
        p.has_clear_color = c.u8v() != 0U;
        for (crd::u32 k = 0; k < 4U; ++k) { p.clear_color[k] = c.f32v(); }
        p.has_clear_depth = c.u8v() != 0U;
        p.clear_depth     = c.f32v();
        p.depth           = static_cast<crd::gpu::DepthCompare>(c.u8v());
        const crd::u32 nprm = c.u32v();
        for (crd::u32 k = 0; k < nprm && c.ok; ++k)
        {
            FrameParam prm(alloc);
            c.strv(prm.name);
            prm.type = static_cast<FrameParamType>(c.u8v());
            for (crd::u32 v = 0; v < 4U; ++v) { prm.v[v] = c.f64v(); }
            p.params.push_back(static_cast<FrameParam&&>(prm));
        }
        // ── v4 (REN-38 audit): the fields v3 dropped, appended at the record's END. ──
        c.strv(p.raygen);
        c.strv(p.miss);
        c.strv(p.closest_hit);
        c.strv(p.any_hit);
        c.strv(p.intersection); // v6 (REN-38-F13)
        c.strv(p.callable);
        p.shading_rate  = static_cast<crd::gpu::ShadingRate>(c.u8v());
        p.rate_combiner = static_cast<crd::gpu::ShadingRateCombiner>(c.u8v());
        p.conservative  = static_cast<crd::gpu::ConservativeMode>(c.u8v());
        p.queue         = static_cast<FrameQueue>(c.u8v());
        p.filter        = static_cast<FrameBlitFilter>(c.u8v());
        p.has_sampler   = c.u8v() != 0U;
        p.sampler.min_filter = static_cast<crd::gpu::SamplerFilter>(c.u8v());
        p.sampler.mag_filter = static_cast<crd::gpu::SamplerFilter>(c.u8v());
        p.sampler.mip_filter = static_cast<crd::gpu::SamplerFilter>(c.u8v());
        p.sampler.address    = static_cast<crd::gpu::SamplerAddress>(c.u8v());
        p.sampler.compare    = c.u8v() != 0U;
        p.sampler.anisotropy = c.u32v();
        p.sampler.mip_bias   = c.f32v();
        p.state.depth_write        = c.u8v() != 0U;
        p.state.depth_bias         = c.f32v();
        p.state.depth_bias_slope   = c.f32v();
        p.state.depth_bias_clamp   = c.f32v();
        p.state.face_cull          = static_cast<crd::gpu::FaceCull>(c.u8v());
        p.state.front_face         = static_cast<crd::gpu::FrontFace>(c.u8v());
        p.state.stencil_enable     = c.u8v() != 0U;
        p.state.stencil_compare    = static_cast<crd::gpu::DepthCompare>(c.u8v());
        p.state.stencil_ref        = c.u32v();
        p.state.stencil_read_mask  = c.u32v();
        p.state.stencil_write_mask = c.u32v();
        p.state.stencil_fail       = static_cast<crd::gpu::StencilOp>(c.u8v());
        p.state.stencil_depth_fail = static_cast<crd::gpu::StencilOp>(c.u8v());
        p.state.stencil_pass       = static_cast<crd::gpu::StencilOp>(c.u8v());
        p.load_target              = c.u8v() != 0U; // v5
        p.load_depth               = c.ok ? c.u8v() != 0U : false; // v6
        if (c.ok) { c.strv(p.shared_depth); } // v7
        if (c.ok) { p.depth_as_float = c.u8v() != 0U; }
        if (c.ok) { p.untracked_storage = c.u8v() != 0U; }
        out.passes.push_back(static_cast<FramePassDesc&&>(p));
    }

    // REN-37.6: composition records.
    const crd::u32 ninc = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < ninc; ++i)
    {
        FrameIncludeDesc inc(alloc);
        c.strv(inc.graph);
        c.strv(inc.as);
        inc.atomic       = c.u8v() != 0U;
        const crd::u32 nb = c.u32v();
        for (crd::u32 k = 0; k < nb && c.ok; ++k)
        {
            FrameBinding b(alloc);
            c.strv(b.from);
            c.strv(b.to);
            inc.bind.push_back(static_cast<FrameBinding&&>(b));
        }
        out.includes.push_back(static_cast<FrameIncludeDesc&&>(inc));
    }
    const crd::u32 nanc = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < nanc; ++i)
    {
        FrameAnchorDesc a(alloc);
        c.strv(a.name);
        const auto get_list = [&](crd::containers::Array<crd::containers::String>& l) {
            const crd::u32 n = c.u32v();
            for (crd::u32 k = 0; k < n && c.ok; ++k)
            {
                crd::containers::String v(alloc);
                c.strv(v);
                l.push_back(static_cast<crd::containers::String&&>(v));
            }
        };
        get_list(a.after);
        get_list(a.before);
        out.anchors.push_back(static_cast<FrameAnchorDesc&&>(a));
    }
    const crd::u32 nij = c.u32v();
    if (!c.ok) { return false; }
    for (crd::u32 i = 0; i < nij; ++i)
    {
        FrameInjectDesc inj(alloc);
        c.strv(inj.anchor);
        c.strv(inj.pass);
        out.injects.push_back(static_cast<FrameInjectDesc&&>(inj));
    }
    return c.ok;
}

// ── FrameGraphBuilder — the PROGRAMMATIC path. Same description, same validator, different provenance. ────────
namespace
{
void bset(crd::containers::String& dst, crd::containers::StringView s)
{
    dst.clear();
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char one[2] = {s[i], '\0'};
        dst.append(static_cast<const char*>(one));
    }
}
} // namespace

FrameGraphBuilder::FrameGraphBuilder(crd::memory::IAllocator* alloc, crd::containers::StringView name)
    : m_desc(alloc), m_alloc(alloc)
{
    m_desc.schema = kFrameSchemaVersion;
    bset(m_desc.name, name);
}

crd::u32 FrameGraphBuilder::add_image(crd::containers::StringView name, crd::gpu::FgImageFormat format,
                                      crd::u32 width, crd::u32 height, bool sampled, crd::u32 layers)
{
    FrameResourceDesc r(m_alloc);
    bset(r.name, name);
    r.kind    = FrameResourceKind::TransientImage;
    r.format  = format;
    r.width   = width;
    r.height  = height;
    r.layers  = layers;
    r.sampled = sampled;
    m_desc.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    return static_cast<crd::u32>(m_desc.resources.size() - 1U);
}

crd::u32 FrameGraphBuilder::add_scaled_image(crd::containers::StringView name, crd::gpu::FgImageFormat format,
                                             float scale, bool sampled)
{
    FrameResourceDesc r(m_alloc);
    bset(r.name, name);
    r.format  = format;
    r.scale   = scale;
    r.sampled = sampled;
    m_desc.resources.push_back(static_cast<FrameResourceDesc&&>(r));
    return static_cast<crd::u32>(m_desc.resources.size() - 1U);
}

crd::u32 FrameGraphBuilder::add_draw_list(crd::containers::StringView name)
{
    FrameDrawListDesc d(m_alloc);
    bset(d.name, name);
    m_desc.draw_lists.push_back(static_cast<FrameDrawListDesc&&>(d));
    return static_cast<crd::u32>(m_desc.draw_lists.size() - 1U);
}
void FrameGraphBuilder::draw_list_all(crd::u32 list, crd::containers::StringView component)
{
    crd::containers::String c(m_alloc);
    bset(c, component);
    m_desc.draw_lists[list].all.push_back(static_cast<crd::containers::String&&>(c));
}
void FrameGraphBuilder::draw_list_none(crd::u32 list, crd::containers::StringView component)
{
    crd::containers::String c(m_alloc);
    bset(c, component);
    m_desc.draw_lists[list].none.push_back(static_cast<crd::containers::String&&>(c));
}
void FrameGraphBuilder::draw_list_policy(crd::u32 list, FrameCullMode cull, FrameSortMode sort)
{
    m_desc.draw_lists[list].cull = cull;
    m_desc.draw_lists[list].sort = sort;
}

crd::u32 FrameGraphBuilder::add_pass(crd::containers::StringView name, FramePassKind kind)
{
    FramePassDesc p(m_alloc);
    bset(p.name, name);
    p.kind = kind;
    m_desc.passes.push_back(static_cast<FramePassDesc&&>(p));
    return static_cast<crd::u32>(m_desc.passes.size() - 1U);
}
void FrameGraphBuilder::pass_reads(crd::u32 pass, crd::containers::StringView resource, bool indexed)
{
    FrameResourceRef r(m_alloc);
    bset(r.name, resource);
    r.indexed = indexed;
    m_desc.passes[pass].reads.push_back(static_cast<FrameResourceRef&&>(r));
}
void FrameGraphBuilder::pass_writes(crd::u32 pass, crd::containers::StringView resource, bool indexed)
{
    FrameResourceRef r(m_alloc);
    bset(r.name, resource);
    r.indexed = indexed;
    m_desc.passes[pass].writes.push_back(static_cast<FrameResourceRef&&>(r));
}
void FrameGraphBuilder::pass_shader(crd::u32 pass, crd::containers::StringView id) { bset(m_desc.passes[pass].shader, id); }
void FrameGraphBuilder::pass_kernel(crd::u32 pass, crd::containers::StringView id) { bset(m_desc.passes[pass].kernel, id); }
void FrameGraphBuilder::pass_technique(crd::u32 pass, crd::containers::StringView id) { bset(m_desc.passes[pass].technique, id); }
void FrameGraphBuilder::pass_draw_list(crd::u32 pass, crd::containers::StringView n) { bset(m_desc.passes[pass].draw_list, n); }
void FrameGraphBuilder::pass_view(crd::u32 pass, crd::containers::StringView n) { bset(m_desc.passes[pass].view, n); }
void FrameGraphBuilder::pass_material(crd::u32 pass, FrameMaterialPass mp) { m_desc.passes[pass].material_pass = mp; }
void FrameGraphBuilder::pass_for_each(crd::u32 pass, FrameForEach gen, crd::u32 arg)
{
    m_desc.passes[pass].for_each     = gen;
    m_desc.passes[pass].for_each_arg = arg;
}
void FrameGraphBuilder::pass_clear_color(crd::u32 pass, float r, float g, float b, float a)
{
    FramePassDesc& p    = m_desc.passes[pass];
    p.has_clear_color   = true;
    p.clear_color[0]    = r;
    p.clear_color[1]    = g;
    p.clear_color[2]    = b;
    p.clear_color[3]    = a;
}
void FrameGraphBuilder::pass_clear_depth(crd::u32 pass, float d)
{
    m_desc.passes[pass].has_clear_depth = true;
    m_desc.passes[pass].clear_depth     = d;
}
void FrameGraphBuilder::pass_depth(crd::u32 pass, crd::gpu::DepthCompare cmp) { m_desc.passes[pass].depth = cmp; }
void FrameGraphBuilder::pass_param(crd::u32 pass, crd::containers::StringView name, double value)
{
    FrameParam prm(m_alloc);
    bset(prm.name, name);
    prm.type = FrameParamType::Float;
    prm.v[0] = value;
    m_desc.passes[pass].params.push_back(static_cast<FrameParam&&>(prm));
}
void FrameGraphBuilder::requires_capability(crd::containers::StringView cap)
{
    crd::containers::String c(m_alloc);
    bset(c, cap);
    m_desc.requires_caps.push_back(static_cast<crd::containers::String&&>(c));
}
void FrameGraphBuilder::fallback_to(crd::containers::StringView graph_name) { bset(m_desc.fallback, graph_name); }

} // namespace crd::framecook
