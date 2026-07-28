// material_asset.cpp — REN-38-C1/C2/C3: parse, validate, emit and COOK a `.crdm`.
//
// ⛔ THE MATERIAL WAS A C++ FUNCTION POINTER. `MaterialTemplate::build_surface` meant inventing a material
// required editing and recompiling the engine — and the REN-37 design diagram already claimed `.crdm` existed.
// This file is that claim, made true: an OpenPBR surface as a NODE GRAPH the cooker turns into CKIR.

#include <crd/matcook/material_asset.hpp>

#include <crd/kir/ckir_material.hpp>
#include <crd/kir/ckir_nodes.hpp>
#include <crd/kir/ckir_post.hpp> // 38-G1: the post/tonemap family (post-context ops)
#include <crd/kir/ckir_shape.hpp> // REN-38 audit: the cook refuses a shape-invalid graph by name

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace crd::matcook
{
namespace
{

struct OpEntry
{
    const char* name;
    crd::u32    arity;
    crd::u32    attr_mask; // bit k set → argument k is a COMPILE-TIME INTEGER, not a wire
    crd::i32    attr_lo;   // inclusive bounds for every attribute slot of this op
    crd::i32    attr_hi;
    crd::u8     width[7];  // natural component count of each WIRE argument (0 where the slot is an attribute)
};

// ── THE OP TABLE — the whole of `ckir_nodes.hpp`'s public node library, transcribed from its signatures. ──
//
// ⛔⛔ TWO KINDS OF ARGUMENT, BOTH SPELLED `int`. Most are WIRES (node ids). A handful are COMPILE-TIME
// ATTRIBUTES: `extract`'s channel index, `convert_f_vec`'s width, `place2d`'s order, `range`'s doclamp,
// `facingratio`'s two flags, and the `location` every geometric reader binds its varying to. C++ cannot tell them
// apart — passing a node id as a swizzle index compiles, builds, and pulls component 47 out of a vec4. So the
// distinction lives HERE, and an asset writes an attribute as a plain integer rather than wiring it.
//
// ⛔ THE WIDTHS ARE NOT DECORATION either. `over` swizzles `.a` and `combine3` builds a vec3 from three scalars, so
// a slot's width is part of what an author (and a node editor) must know to wire it correctly.
const OpEntry kOps[] = {
    {"aastep", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    // ── ⭐⭐ 38-G1: the POST/TONEMAP family (ckir_post.hpp). ⛔ POST-CONTEXT ONLY: a material describes
    // SURFACE RESPONSE (ADR-0102, enforced below) — tonemapping is a FRAME operation, so these ops cook only
    // through `cook_post_graph`, never `cook_material`. One registry, two legality contexts — the same split
    // that keeps materials out of the lighting model keeps them out of the display transform.
    {"agx", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},
    {"absval", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"acos", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"add", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"asin", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"atan2", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"bitangent", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
    {"burn", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"ceil", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"checkerboard", 5U, 0U, 0, 0, {3, 3, 2, 2, 2, 0, 0}},
    {"contrast_curve", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}}, // 38-G1: the AgX look's sigmoid (post-only)
    {"clamp", 3U, 0U, 0, 0, {1, 1, 1, 0, 0, 0, 0}},
    {"clamp01", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"combine2", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"combine3", 3U, 0U, 0, 0, {1, 1, 1, 0, 0, 0, 0}},
    {"combine4", 4U, 0U, 0, 0, {1, 1, 1, 1, 0, 0, 0}},
    {"combine_c3f", 2U, 0U, 0, 0, {3, 1, 0, 0, 0, 0, 0}},
    {"comp_in", 3U, 0U, 0, 0, {4, 4, 1, 0, 0, 0, 0}},
    {"comp_out", 3U, 0U, 0, 0, {4, 4, 1, 0, 0, 0, 0}},
    {"contrast", 3U, 0U, 0, 0, {1, 1, 1, 0, 0, 0, 0}},
    {"convert_c3_c4", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},
    {"convert_f_vec", 2U, 0x2U, 1, 4, {1, 0, 0, 0, 0, 0, 0}},
    {"cos", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"crossproduct", 2U, 0U, 0, 0, {3, 3, 0, 0, 0, 0, 0}},
    {"difference", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"disjointover", 3U, 0U, 0, 0, {4, 4, 1, 0, 0, 0, 0}},
    {"distance", 2U, 0U, 0, 0, {3, 3, 0, 0, 0, 0, 0}},
    {"divide", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"dodge", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"dotproduct", 2U, 0U, 0, 0, {3, 3, 0, 0, 0, 0, 0}},
    {"exp", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"extract", 2U, 0x2U, 0, 3, {4, 0, 0, 0, 0, 0, 0}},
    {"facingratio", 4U, 0xCU, 0, 1, {3, 3, 0, 0, 0, 0, 0}},
    {"floor", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    // REN-38-F7: the screen-space derivative width — fragment-stage only (`entry_valid` enforces the stage),
    // the analytic-AA primitive the grid material's cell factor is built on.
    {"fwidth", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"geomcolor", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
    {"gooch_shade", 7U, 0U, 0, 0, {3, 3, 3, 3, 1, 1, 3}},
    {"heighttonormal", 3U, 0U, 0, 0, {1, 1, 2, 0, 0, 0, 0}},
    {"hsvadjust", 2U, 0U, 0, 0, {3, 3, 0, 0, 0, 0, 0}},
    {"hsvtorgb", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},
    {"ifequal", 4U, 0U, 0, 0, {1, 1, 1, 1, 0, 0, 0}},
    {"ifgreater", 4U, 0U, 0, 0, {1, 1, 1, 1, 0, 0, 0}},
    {"ifgreatereq", 4U, 0U, 0, 0, {1, 1, 1, 1, 0, 0, 0}},
    {"inside", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"invert", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"ln", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"logical_and", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"logical_not", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"logical_or", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"logical_xor", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"ev100", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},           // 38-G1: average luminance -> EV100 (post-only)
    {"exposure_scale", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},   // 38-G1: EV100 -> linear exposure factor (post-only)
    {"gamut_compress", 2U, 0U, 0, 0, {3, 1, 0, 0, 0, 0, 0}},   // 38-G1 (post-only)
    {"luminance", 2U, 0U, 0, 0, {3, 3, 0, 0, 0, 0, 0}},
    {"magnitude", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},
    {"mask", 3U, 0U, 0, 0, {4, 4, 1, 0, 0, 0, 0}},
    {"matte", 3U, 0U, 0, 0, {4, 4, 1, 0, 0, 0, 0}},
    {"maximum", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"minimum", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"minus", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"mix", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"modulo", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"multiply", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"normal", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
    {"normalize", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},
    {"outside", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"over", 3U, 0U, 0, 0, {4, 4, 1, 0, 0, 0, 0}},
    {"overlay", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"place2d", 6U, 0x20U, 0, 1, {2, 2, 2, 1, 2, 0, 0}},
    {"plus", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"position", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
    {"power", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"premult", 1U, 0U, 0, 0, {4, 0, 0, 0, 0, 0, 0}},
    {"ramplr", 3U, 0U, 0, 0, {3, 3, 2, 0, 0, 0, 0}},
    {"ramptb", 3U, 0U, 0, 0, {3, 3, 2, 0, 0, 0, 0}},
    {"range", 7U, 0x40U, 0, 1, {1, 1, 1, 1, 1, 1, 0}},
    {"remap", 5U, 0U, 0, 0, {1, 1, 1, 1, 1, 0, 0}},
    {"remap01", 3U, 0U, 0, 0, {1, 1, 1, 0, 0, 0, 0}},
    {"rgbtohsv", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},
    {"rotate2d", 2U, 0U, 0, 0, {2, 1, 0, 0, 0, 0, 0}},
    {"rotate3d", 3U, 0U, 0, 0, {3, 1, 3, 0, 0, 0, 0}},
    {"round", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    // ⛔⛔ THE ONE OP THAT IS NOT A `ckir_nodes` FUNCTION, and it has to exist: a material that cannot
    // SAMPLE A TEXTURE is not a material system. B2 gave CKIR texture/sampler/`tex_sample` leaves and no
    // authored vocabulary reached them, so every textured surface stayed a C++ builder. Its three descriptor
    // coordinates are ATTRIBUTES, not wires — a binding index is topology, not a value an instance overrides.
    {"sample2d", 4U, 0xEU, 0, 15, {2, 0, 0, 0, 0, 0, 0}},
    {"saturate", 2U, 0U, 0, 0, {3, 1, 0, 0, 0, 0, 0}},
    {"pbr_neutral", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},     // 38-G1: Khronos PBR Neutral tonemap (post-only)
    {"pq_encode", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},        // 38-G1: ST.2084 HDR10 encode (post-only)
    {"screen", 3U, 0U, 0, 0, {3, 3, 1, 0, 0, 0, 0}},
    {"srgb_encode", 1U, 0U, 0, 0, {3, 0, 0, 0, 0, 0, 0}},      // 38-G1: the sRGB OETF (post-only)
    {"sign", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"sin", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"smoothstep", 3U, 0U, 0, 0, {1, 1, 1, 0, 0, 0, 0}},
    {"splitlr", 4U, 0U, 0, 0, {3, 3, 1, 2, 0, 0, 0}},
    {"splittb", 4U, 0U, 0, 0, {3, 3, 1, 2, 0, 0, 0}},
    {"sqrt", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"subtract", 2U, 0U, 0, 0, {1, 1, 0, 0, 0, 0, 0}},
    {"switch5", 6U, 0U, 0, 0, {1, 1, 1, 1, 1, 1, 0}},
    {"tan", 1U, 0U, 0, 0, {1, 0, 0, 0, 0, 0, 0}},
    {"tangent", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
    {"texcoord", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
    {"triplanar", 4U, 0U, 0, 0, {3, 3, 3, 3, 0, 0, 0}},
    {"triplanar_weights", 2U, 0U, 0, 0, {3, 1, 0, 0, 0, 0, 0}},
    {"unpremult", 1U, 0U, 0, 0, {4, 0, 0, 0, 0, 0, 0}},
    {"viewdirection", 1U, 0x1U, 0, 15, {0, 0, 0, 0, 0, 0, 0}},
};

[[nodiscard]] bool op_is(crd::containers::StringView op, const char* lit) noexcept
{
    const crd::usize n = std::strlen(lit);
    return op.size() == n && std::memcmp(op.data(), lit, n) == 0;
}

[[nodiscard]] crd::i32 find_op(crd::containers::StringView op) noexcept
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(sizeof(kOps) / sizeof(kOps[0])); ++i)
    {
        if (op_is(op, kOps[i].name)) { return static_cast<crd::i32>(i); }
    }
    return -1;
}

// ⛔ ONE dispatch, by name, with the arity already validated. A switch on an enum would need the enum kept in
// step with the table by hand — which is the drift this generated pair exists to remove.
[[nodiscard]] int dispatch_op(crd::kir::KGraph& g, crd::containers::StringView op, const int* in)
{
    if (op_is(op, "aastep")) { return crd::kir::nodes::aastep(g, in[0], in[1]); }
    if (op_is(op, "absval")) { return crd::kir::nodes::absval(g, in[0]); }
    if (op_is(op, "acos")) { return crd::kir::nodes::acos(g, in[0]); }
    if (op_is(op, "add")) { return crd::kir::nodes::add(g, in[0], in[1]); }
    if (op_is(op, "asin")) { return crd::kir::nodes::asin(g, in[0]); }
    if (op_is(op, "atan2")) { return crd::kir::nodes::atan2(g, in[0], in[1]); }
    if (op_is(op, "bitangent")) { return crd::kir::nodes::bitangent(g, in[0]); }
    if (op_is(op, "burn")) { return crd::kir::nodes::burn(g, in[0], in[1], in[2]); }
    if (op_is(op, "ceil")) { return crd::kir::nodes::ceil(g, in[0]); }
    if (op_is(op, "checkerboard")) { return crd::kir::nodes::checkerboard(g, in[0], in[1], in[2], in[3], in[4]); }
    if (op_is(op, "clamp")) { return crd::kir::nodes::clamp(g, in[0], in[1], in[2]); }
    if (op_is(op, "clamp01")) { return crd::kir::nodes::clamp01(g, in[0]); }
    if (op_is(op, "combine2")) { return crd::kir::nodes::combine2(g, in[0], in[1]); }
    if (op_is(op, "combine3")) { return crd::kir::nodes::combine3(g, in[0], in[1], in[2]); }
    if (op_is(op, "combine4")) { return crd::kir::nodes::combine4(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "combine_c3f")) { return crd::kir::nodes::combine_c3f(g, in[0], in[1]); }
    if (op_is(op, "comp_in")) { return crd::kir::nodes::comp_in(g, in[0], in[1], in[2]); }
    if (op_is(op, "comp_out")) { return crd::kir::nodes::comp_out(g, in[0], in[1], in[2]); }
    if (op_is(op, "contrast")) { return crd::kir::nodes::contrast(g, in[0], in[1], in[2]); }
    if (op_is(op, "convert_c3_c4")) { return crd::kir::nodes::convert_c3_c4(g, in[0]); }
    if (op_is(op, "convert_f_vec")) { return crd::kir::nodes::convert_f_vec(g, in[0], in[1]); }
    if (op_is(op, "cos")) { return crd::kir::nodes::cos(g, in[0]); }
    if (op_is(op, "crossproduct")) { return crd::kir::nodes::crossproduct(g, in[0], in[1]); }
    if (op_is(op, "difference")) { return crd::kir::nodes::difference(g, in[0], in[1], in[2]); }
    if (op_is(op, "disjointover")) { return crd::kir::nodes::disjointover(g, in[0], in[1], in[2]); }
    if (op_is(op, "distance")) { return crd::kir::nodes::distance(g, in[0], in[1]); }
    if (op_is(op, "divide")) { return crd::kir::nodes::divide(g, in[0], in[1]); }
    if (op_is(op, "dodge")) { return crd::kir::nodes::dodge(g, in[0], in[1], in[2]); }
    if (op_is(op, "dotproduct")) { return crd::kir::nodes::dotproduct(g, in[0], in[1]); }
    if (op_is(op, "exp")) { return crd::kir::nodes::exp(g, in[0]); }
    if (op_is(op, "extract")) { return crd::kir::nodes::extract(g, in[0], in[1]); }
    // ⛔ `faceforward`/`invert` are BOOL parameters of the node function, not wires — the asset writes 0 or 1 and
    // the branch is taken at COOK time, which is what MaterialX's `ifequal`-switch nodegraph means by them.
    if (op_is(op, "facingratio")) { return crd::kir::nodes::facingratio(g, in[0], in[1], in[2] != 0, in[3] != 0); }
    if (op_is(op, "floor")) { return crd::kir::nodes::floor(g, in[0]); }
    if (op_is(op, "fwidth")) { return g.unary(crd::kir::KOp::Fwidth, in[0]); }
    if (op_is(op, "geomcolor")) { return crd::kir::nodes::geomcolor(g, in[0]); }
    if (op_is(op, "gooch_shade")) { return crd::kir::nodes::gooch_shade(g, in[0], in[1], in[2], in[3], in[4], in[5], in[6]); }
    if (op_is(op, "heighttonormal")) { return crd::kir::nodes::heighttonormal(g, in[0], in[1], in[2]); }
    if (op_is(op, "hsvadjust")) { return crd::kir::nodes::hsvadjust(g, in[0], in[1]); }
    if (op_is(op, "hsvtorgb")) { return crd::kir::nodes::hsvtorgb(g, in[0]); }
    if (op_is(op, "ifequal")) { return crd::kir::nodes::ifequal(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "ifgreater")) { return crd::kir::nodes::ifgreater(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "ifgreatereq")) { return crd::kir::nodes::ifgreatereq(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "inside")) { return crd::kir::nodes::inside(g, in[0], in[1]); }
    if (op_is(op, "invert")) { return crd::kir::nodes::invert(g, in[0], in[1]); }
    if (op_is(op, "ln")) { return crd::kir::nodes::ln(g, in[0]); }
    if (op_is(op, "logical_and")) { return crd::kir::nodes::logical_and(g, in[0], in[1]); }
    if (op_is(op, "logical_not")) { return crd::kir::nodes::logical_not(g, in[0]); }
    if (op_is(op, "logical_or")) { return crd::kir::nodes::logical_or(g, in[0], in[1]); }
    if (op_is(op, "logical_xor")) { return crd::kir::nodes::logical_xor(g, in[0], in[1]); }
    if (op_is(op, "luminance")) { return crd::kir::nodes::luminance(g, in[0], in[1]); }
    if (op_is(op, "magnitude")) { return crd::kir::nodes::magnitude(g, in[0]); }
    if (op_is(op, "mask")) { return crd::kir::nodes::mask(g, in[0], in[1], in[2]); }
    if (op_is(op, "matte")) { return crd::kir::nodes::matte(g, in[0], in[1], in[2]); }
    if (op_is(op, "maximum")) { return crd::kir::nodes::maximum(g, in[0], in[1]); }
    if (op_is(op, "minimum")) { return crd::kir::nodes::minimum(g, in[0], in[1]); }
    if (op_is(op, "minus")) { return crd::kir::nodes::minus(g, in[0], in[1], in[2]); }
    if (op_is(op, "mix")) { return crd::kir::nodes::mix(g, in[0], in[1], in[2]); }
    if (op_is(op, "modulo")) { return crd::kir::nodes::modulo(g, in[0], in[1]); }
    if (op_is(op, "multiply")) { return crd::kir::nodes::multiply(g, in[0], in[1]); }
    if (op_is(op, "normal")) { return crd::kir::nodes::normal(g, in[0]); }
    if (op_is(op, "normalize")) { return crd::kir::nodes::normalize(g, in[0]); }
    if (op_is(op, "outside")) { return crd::kir::nodes::outside(g, in[0], in[1]); }
    if (op_is(op, "over")) { return crd::kir::nodes::over(g, in[0], in[1], in[2]); }
    if (op_is(op, "overlay")) { return crd::kir::nodes::overlay(g, in[0], in[1], in[2]); }
    if (op_is(op, "place2d")) { return crd::kir::nodes::place2d(g, in[0], in[1], in[2], in[3], in[4], in[5]); }
    if (op_is(op, "plus")) { return crd::kir::nodes::plus(g, in[0], in[1], in[2]); }
    if (op_is(op, "position")) { return crd::kir::nodes::position(g, in[0]); }
    if (op_is(op, "power")) { return crd::kir::nodes::power(g, in[0], in[1]); }
    if (op_is(op, "premult")) { return crd::kir::nodes::premult(g, in[0]); }
    if (op_is(op, "ramplr")) { return crd::kir::nodes::ramplr(g, in[0], in[1], in[2]); }
    if (op_is(op, "ramptb")) { return crd::kir::nodes::ramptb(g, in[0], in[1], in[2]); }
    if (op_is(op, "range")) { return crd::kir::nodes::range(g, in[0], in[1], in[2], in[3], in[4], in[5], in[6] != 0); }
    if (op_is(op, "remap")) { return crd::kir::nodes::remap(g, in[0], in[1], in[2], in[3], in[4]); }
    if (op_is(op, "remap01")) { return crd::kir::nodes::remap01(g, in[0], in[1], in[2]); }
    if (op_is(op, "rgbtohsv")) { return crd::kir::nodes::rgbtohsv(g, in[0]); }
    if (op_is(op, "rotate2d")) { return crd::kir::nodes::rotate2d(g, in[0], in[1]); }
    if (op_is(op, "rotate3d")) { return crd::kir::nodes::rotate3d(g, in[0], in[1], in[2]); }
    if (op_is(op, "round")) { return crd::kir::nodes::round(g, in[0]); }
    if (op_is(op, "sample2d"))
    {
        // set/binding/sampler are the DESCRIPTOR coordinates; ADR-0102 puts a material map at set 2 by
        // frequency, but the set is declared rather than assumed so a renderer that lays out differently is
        // still expressible.
        const int tex  = g.texture(in[1], in[2], crd::kir::DType::F32, crd::kir::TexDim::Tex2D, false, false, false);
        const int samp = g.sampler(in[1], in[3], false);
        return g.tex_sample(tex, samp, in[0]);
    }
    if (op_is(op, "saturate")) { return crd::kir::nodes::saturate(g, in[0], in[1]); }
    // 38-G1: the POST/TONEMAP family — legal only under the POST context (`cook_post_graph`); `cook_material`
    // refuses them by name below, the same way it refuses lighting ops.
    if (op_is(op, "agx")) { return crd::kir::post::agx(g, in[0]); }
    if (op_is(op, "contrast_curve")) { return crd::kir::post::agx_detail::contrast(g, in[0]); }
    if (op_is(op, "ev100")) { return crd::kir::post::ev100_from_luminance(g, in[0]); }
    if (op_is(op, "exposure_scale")) { return crd::kir::post::exposure_from_ev100(g, in[0]); }
    if (op_is(op, "gamut_compress")) { return crd::kir::post::gamut_compress(g, in[0], in[1]); }
    if (op_is(op, "pbr_neutral")) { return crd::kir::post::pbr_neutral(g, in[0]); }
    if (op_is(op, "pq_encode")) { return crd::kir::post::pq_encode(g, in[0]); }
    if (op_is(op, "srgb_encode")) { return crd::kir::post::srgb_encode(g, in[0]); }
    if (op_is(op, "screen")) { return crd::kir::nodes::screen(g, in[0], in[1], in[2]); }
    if (op_is(op, "sign")) { return crd::kir::nodes::sign(g, in[0]); }
    if (op_is(op, "sin")) { return crd::kir::nodes::sin(g, in[0]); }
    if (op_is(op, "smoothstep")) { return crd::kir::nodes::smoothstep(g, in[0], in[1], in[2]); }
    if (op_is(op, "splitlr")) { return crd::kir::nodes::splitlr(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "splittb")) { return crd::kir::nodes::splittb(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "sqrt")) { return crd::kir::nodes::sqrt(g, in[0]); }
    if (op_is(op, "subtract")) { return crd::kir::nodes::subtract(g, in[0], in[1]); }
    if (op_is(op, "switch5")) { return crd::kir::nodes::switch5(g, in[0], in[1], in[2], in[3], in[4], in[5]); }
    if (op_is(op, "tan")) { return crd::kir::nodes::tan(g, in[0]); }
    if (op_is(op, "tangent")) { return crd::kir::nodes::tangent(g, in[0]); }
    if (op_is(op, "texcoord")) { return crd::kir::nodes::texcoord(g, in[0]); }
    if (op_is(op, "triplanar")) { return crd::kir::nodes::triplanar(g, in[0], in[1], in[2], in[3]); }
    if (op_is(op, "triplanar_weights")) { return crd::kir::nodes::triplanar_weights(g, in[0], in[1]); }
    if (op_is(op, "unpremult")) { return crd::kir::nodes::unpremult(g, in[0]); }
    if (op_is(op, "viewdirection")) { return crd::kir::nodes::viewdirection(g, in[0]); }
    return -1;
}

void set_str(crd::containers::String& d, std::string_view v)
{
    d.clear();
    for (char c : v) { const char one[2] = {c, 0}; d.append(static_cast<const char*>(one)); }
}
void set_where(crd::containers::String* w, std::string_view v)
{
    if (w != nullptr) { set_str(*w, v); }
}
[[nodiscard]] bool str_eq(const crd::containers::String& a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.c_str(), b.data(), b.size()) == 0;
}

// ⛔ ADR-0102, ENFORCED rather than documented. A material describes SURFACE RESPONSE; a node that reached for
// lighting or shadow state would make every material carry a copy of the lighting model — the exact coupling
// REN-37 removed. The check is on the OP NAME, because that is the only place such a node could enter.
[[nodiscard]] bool is_lighting_op(std::string_view op) noexcept
{
    return op == "shadow" || op == "light" || op == "lighting" || op == "ibl" || op == "exposure"
           || op == "tonemap";
}

// ── ⭐⭐ 38-G1: the POST-ONLY family — frame operations, not surface response. `cook_material` refuses them
// exactly as it refuses lighting ops (ADR-0102, two-sided); `cook_post_graph` is their one legal context.
[[nodiscard]] bool is_post_op(std::string_view op) noexcept
{
    return op == "agx" || op == "contrast_curve" || op == "ev100" || op == "exposure_scale"
           || op == "gamut_compress" || op == "pbr_neutral" || op == "pq_encode" || op == "srgb_encode";
}

// ── and the ops a POST graph must NOT reach: surface/geometry readers. A display transform that sampled a
// normal or a vertex colour would be a material wearing a post costume — the same coupling, reversed.
[[nodiscard]] bool is_surface_reader_op(std::string_view op) noexcept
{
    // `texcoord` stays LEGAL: in a fullscreen pass it is the SCREEN coordinate, exactly what a post graph
    // samples its input by — refusing it would make every post asset inexpressible.
    return op == "geomcolor" || op == "normal" || op == "tangent" || op == "bitangent" || op == "position"
           || op == "facingratio";
}

} // namespace


// ── PARSE ─────────────────────────────────────────────────────────────────────────────────────────────────
namespace
{
void read_value(const toml::node& n, double* v, crd::u32& comps)
{
    if (const auto* arr = n.as_array())
    {
        crd::u32 i = 0;
        for (const auto& e : *arr)
        {
            if (i < 4U) { v[i++] = e.value_or<double>(0.0); }
        }
        comps = i > 0U ? i : 1U;
        return;
    }
    v[0]  = n.value_or<double>(0.0);
    comps = 1U;
}
} // namespace

MaterialCookError parse_material_core(crd::containers::StringView toml_text, MaterialDesc& out,
                                      crd::containers::String* where)
{
    // NON-THROWING parse (TOML_EXCEPTIONS=0, the frame-cook pattern). A thrown parse_error unwinding
    // while a live GPU device (validation layer hooked into SEH dispatch) CRASHED the process — an
    // authored-asset TYPO became a process kill once disk-first loading made user edits reachable.
    // Result-checked also kills the mixed-mode ODR hazard (three cookers threw, three did not).
    toml::parse_result pr = toml::parse(std::string_view(toml_text.data(), toml_text.size()));
    if (!pr) { return MaterialCookError::ParseFailed; }
    toml::table root = std::move(pr).table();
    auto* alloc = out.nodes.allocator();

    // ⛔ RESET THE OUTPUT FIRST. Parsing into a descriptor that already held a material APPENDED to it: the second
    // asset's nodes joined the first's, and a name in both came back as `DuplicateName` — an error naming the
    // wrong thing. Worse when the names differ: a silently MERGED graph that cooks and shades from both.
    out.name.clear();
    out.params.clear();
    out.nodes.clear();
    out.instances.clear();
    out.surface = MatSurfaceDesc(alloc);

    const auto sch = root["schema"].value<int64_t>();
    if (!sch || *sch != static_cast<int64_t>(kMaterialSchemaVersion)) { return MaterialCookError::BadSchema; }
    out.schema = kMaterialSchemaVersion;
    const auto nm = root["name"].value<std::string_view>();
    if (!nm || nm->empty()) { return MaterialCookError::MissingName; }
    set_str(out.name, *nm);

    if (const auto* pt = root["param"].as_array())
    {
        for (const auto& e : *pt)
        {
            const toml::table* t = e.as_table();
            if (t == nullptr) { continue; }
            MatParamDesc p(alloc);
            const auto   pn = (*t)["name"].value<std::string_view>();
            if (!pn || pn->empty()) { return MaterialCookError::MissingName; }
            set_str(p.name, *pn);
            for (crd::usize i = 0; i < out.params.size(); ++i)
            {
                if (str_eq(out.params[i].name, *pn)) { set_where(where, *pn); return MaterialCookError::DuplicateName; }
            }
            if (const toml::node* v = (*t)["value"].node()) { read_value(*v, static_cast<double*>(p.value), p.comps); }
            out.params.push_back(static_cast<MatParamDesc&&>(p));
        }
    }

    if (const auto* nt = root["node"].as_array())
    {
        for (const auto& e : *nt)
        {
            const toml::table* t = e.as_table();
            if (t == nullptr) { continue; }
            MatNodeDesc n(alloc);
            const auto  nn = (*t)["name"].value<std::string_view>();
            const auto  op = (*t)["op"].value<std::string_view>();
            if (!nn || nn->empty() || !op || op->empty()) { return MaterialCookError::MissingName; }
            set_str(n.name, *nn);
            set_str(n.op, *op);
            for (crd::usize i = 0; i < out.nodes.size(); ++i)
            {
                if (str_eq(out.nodes[i].name, *nn)) { set_where(where, *nn); return MaterialCookError::DuplicateName; }
            }
            if (const auto* ia = (*t)["inputs"].as_array())
            {
                for (const auto& ie : *ia)
                {
                    if (n.inputs.size() >= kMaxNodeInputs) { set_where(where, *nn); return MaterialCookError::TooManyInputs; }
                    MatInput in(alloc);
                    // ⛔ A STRING input names something; a NUMBER is a literal. The `$` prefix distinguishes a
                    // PARAMETER from a node, because the two are overridden differently and guessing from context
                    // would make one spelling mean different things in different assets.
                    if (const auto sv = ie.value<std::string_view>())
                    {
                        if (!sv->empty() && (*sv)[0] == '$')
                        {
                            in.kind = MatInputKind::Param;
                            set_str(in.name, sv->substr(1));
                        }
                        else
                        {
                            in.kind = MatInputKind::Node;
                            set_str(in.name, *sv);
                        }
                    }
                    else
                    {
                        in.kind = MatInputKind::Literal;
                        read_value(ie, static_cast<double*>(in.value), in.comps);
                    }
                    n.inputs.push_back(static_cast<MatInput&&>(in));
                }
            }
            out.nodes.push_back(static_cast<MatNodeDesc&&>(n));
        }
    }

    if (const auto* it = root["instance"].as_array())
    {
        for (const auto& e : *it)
        {
            const toml::table* t = e.as_table();
            if (t == nullptr) { continue; }
            MatInstanceDesc inst(alloc);
            const auto      iname = (*t)["name"].value<std::string_view>();
            if (!iname || iname->empty()) { return MaterialCookError::MissingName; }
            set_str(inst.name, *iname);
            for (crd::usize i = 0; i < out.instances.size(); ++i)
            {
                if (str_eq(out.instances[i].name, *iname)) { set_where(where, *iname); return MaterialCookError::DuplicateName; }
            }
            if (const auto* ov = (*t)["set"].as_table())
            {
                for (const auto& [k, v] : *ov)
                {
                    MatParamDesc p(alloc);
                    set_str(p.name, std::string_view(k.str().data(), k.str().size()));
                    read_value(v, static_cast<double*>(p.value), p.comps);
                    inst.overrides.push_back(static_cast<MatParamDesc&&>(p));
                }
            }
            out.instances.push_back(static_cast<MatInstanceDesc&&>(inst));
        }
    }

    if (const auto* sf = root["surface"].as_table())
    {
        const auto get = [&](const char* k, crd::containers::String& d) {
            if (const auto v = (*sf)[k].value<std::string_view>()) { set_str(d, *v); }
        };
        get("base_color", out.surface.base_color);
        get("metallic", out.surface.metallic);
        get("roughness", out.surface.roughness);
        get("normal", out.surface.normal);
        get("emissive", out.surface.emissive);
        get("opacity", out.surface.opacity);
    }

    return MaterialCookError::Ok; // context validation is the FACE's job (material vs post)
}

MaterialCookError parse_material_toml(crd::containers::StringView toml_text, MaterialDesc& out,
                                      crd::containers::String* where)
{
    const MaterialCookError e = parse_material_core(toml_text, out, where);
    if (e != MaterialCookError::Ok) { return e; }
    return validate_material(out, where);
}

// 38-G1: the POST face — the same grammar, the OTHER legality. `cook_post_graph` enforces the post rules
// (post-only ops legal, surface readers refused, a named "output" required); running the MATERIAL validator
// here would refuse every tonemap by design.
MaterialCookError parse_post_toml(crd::containers::StringView toml_text, MaterialDesc& out,
                                  crd::containers::String* where)
{
    return parse_material_core(toml_text, out, where);
}

// ── VALIDATE ──────────────────────────────────────────────────────────────────────────────────────────────
MaterialCookError validate_material(const MaterialDesc& desc, crd::containers::String* where)
{
    const auto find_node = [&](const crd::containers::String& n) -> crd::i32 {
        for (crd::usize i = 0; i < desc.nodes.size(); ++i)
        {
            if (desc.nodes[i].name.size() == n.size()
                && std::memcmp(desc.nodes[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                return static_cast<crd::i32>(i);
            }
        }
        return -1;
    };
    const auto has_param = [&](const crd::containers::String& n) {
        for (crd::usize i = 0; i < desc.params.size(); ++i)
        {
            if (desc.params[i].name.size() == n.size()
                && std::memcmp(desc.params[i].name.c_str(), n.c_str(), n.size()) == 0)
            {
                return true;
            }
        }
        return false;
    };

    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const MatNodeDesc& n  = desc.nodes[i];
        const std::string_view opv(n.op.c_str(), n.op.size());
        if (is_lighting_op(opv)) { set_where(where, std::string_view(n.name.c_str(), n.name.size())); return MaterialCookError::ForbiddenLighting; }
        // 38-G1: the display transform is a FRAME operation — a material carrying `agx`/`srgb_encode` couples
        // every surface to the output encoding, the mirror image of the lighting coupling above. Same error:
        // the asset names an op this CONTEXT may not use, and the message points at the node.
        if (is_post_op(opv)) { set_where(where, std::string_view(n.name.c_str(), n.name.size())); return MaterialCookError::ForbiddenLighting; }
        const crd::i32 oi = find_op(crd::containers::StringView(n.op.c_str(), n.op.size()));
        if (oi < 0)
        {
            set_where(where, opv);
            return MaterialCookError::UnknownOp;
        }
        const OpEntry& oe = kOps[static_cast<crd::usize>(oi)];
        if (n.inputs.size() > kMaxNodeInputs) { set_where(where, std::string_view(n.name.c_str(), n.name.size())); return MaterialCookError::TooManyInputs; }
        // ⛔ ARITY IS CHECKED, not tolerated. A `mix` with two inputs would otherwise wire a garbage node id into
        // the third slot — CKIR does not bounds-check a node index, so the result is a graph that builds and
        // computes something unrelated.
        if (n.inputs.size() != oe.arity) { set_where(where, std::string_view(n.name.c_str(), n.name.size())); return MaterialCookError::WrongArity; }

        for (crd::usize k = 0; k < n.inputs.size(); ++k)
        {
            const MatInput& in = n.inputs[k];
            // ⛔⛔ AN ATTRIBUTE SLOT IS NOT A WIRE. `extract`'s index, `place2d`'s order, a geometric reader's
            // varying `location`: these choose graph TOPOLOGY at cook time, so they must be WRITTEN as an integer.
            // A node id landing in one of them type-checks in C++ and swizzles a component that does not exist —
            // the graph builds and renders something unrelated, which is why this is checked and not documented.
            if ((oe.attr_mask & (1U << k)) != 0U)
            {
                // ⛔ Nor may it be a PARAMETER: an instance changes VALUES, not topology. Letting an override
                // move a swizzle index would make two instances of one material two different graphs.
                if (in.kind != MatInputKind::Literal)
                {
                    set_where(where, std::string_view(n.name.c_str(), n.name.size()));
                    return MaterialCookError::AttrNotConstant;
                }
                const double d = in.value[0];
                const auto   v = static_cast<crd::i32>(d);
                if (static_cast<double>(v) != d || v < oe.attr_lo || v > oe.attr_hi)
                {
                    set_where(where, std::string_view(n.name.c_str(), n.name.size()));
                    return MaterialCookError::AttrOutOfRange;
                }
                continue;
            }
            if (in.kind == MatInputKind::Node)
            {
                const crd::i32 src = find_node(in.name);
                if (src < 0) { set_where(where, std::string_view(in.name.c_str(), in.name.size())); return MaterialCookError::UnknownInput; }
                // ⛔ A DAG, enforced by DECLARATION ORDER: a node may only read one declared BEFORE it. That is a
                // stricter rule than "no cycles" and it is deliberate — it makes the cook a single forward pass
                // with no topological sort, and it makes a cycle impossible to write rather than merely rejected.
                if (static_cast<crd::usize>(src) >= i)
                {
                    set_where(where, std::string_view(n.name.c_str(), n.name.size()));
                    return MaterialCookError::NodeCycle;
                }
            }
            else if (in.kind == MatInputKind::Param && !has_param(in.name))
            {
                set_where(where, std::string_view(in.name.c_str(), in.name.size()));
                return MaterialCookError::UnknownInput;
            }
        }
    }

    // ⛔ BASE COLOUR IS REQUIRED. Every other surface field has a defensible default (metal 0, rough 1, the
    // geometric normal); base colour does not — a material with none is either unfinished or misspelled, and
    // defaulting it to white renders a plausible object that is not the one the author described.
    if (desc.surface.base_color.empty()) { set_where(where, std::string_view(desc.name.c_str(), desc.name.size())); return MaterialCookError::NoBaseColor; }
    const crd::containers::String* fields[6] = {&desc.surface.base_color, &desc.surface.metallic,
                                                &desc.surface.roughness,  &desc.surface.normal,
                                                &desc.surface.emissive,   &desc.surface.opacity};
    for (const crd::containers::String* f : fields)
    {
        if (f->empty()) { continue; }
        if (find_node(*f) < 0) { set_where(where, std::string_view(f->c_str(), f->size())); return MaterialCookError::SurfaceUnbound; }
    }

    // ⛔ An instance may only override a parameter that EXISTS. A typo would otherwise be silently ignored and the
    // artist would see the default with nothing on screen or in the file to explain why.
    for (crd::usize i = 0; i < desc.instances.size(); ++i)
    {
        for (crd::usize k = 0; k < desc.instances[i].overrides.size(); ++k)
        {
            const crd::containers::String& on = desc.instances[i].overrides[k].name;
            if (!has_param(on)) { set_where(where, std::string_view(on.c_str(), on.size())); return MaterialCookError::UnknownOverride; }
        }
    }
    return MaterialCookError::Ok;
}

// ── EMIT ──────────────────────────────────────────────────────────────────────────────────────────────────
// ⛔ The frame asset's rule, one asset over: `parse → emit → parse` must produce an identical description, or a
// node-editor save silently drops what it did not understand — and a dropped wire is a picture that still renders.
namespace
{
void app(crd::containers::String& o, const char* t) { o.append(t); }
void app_quoted(crd::containers::String& o, const crd::containers::String& v)
{
    app(o, "\"");
    o.append(v.c_str());
    app(o, "\"");
}
// 17 significant digits is the IEEE-754 float64 exact-round-trip guarantee, so an editor save can never perturb an
// authored value by re-writing it. ⛔ The trailing `.0` is not cosmetic: `%.17g` renders 1.0 as `1`, which TOML
// reads back as an INTEGER, and a value that changes type on save is a value that can change meaning.
void app_f64(crd::containers::String& o, double v)
{
    char buf[40];
    std::snprintf(static_cast<char*>(buf), sizeof(buf), "%.17g", v);
    o.append(static_cast<const char*>(buf));
    bool plain = true;
    for (const char* p = static_cast<const char*>(buf); *p != 0; ++p)
    {
        if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') { plain = false; }
    }
    if (plain) { app(o, ".0"); }
}
void app_i32(crd::containers::String& o, crd::i32 v)
{
    char buf[16];
    std::snprintf(static_cast<char*>(buf), sizeof(buf), "%d", v);
    o.append(static_cast<const char*>(buf));
}
void app_value(crd::containers::String& o, const double* v, crd::u32 comps)
{
    if (comps <= 1U)
    {
        app_f64(o, v[0]);
        return;
    }
    app(o, "[");
    for (crd::u32 i = 0; i < comps && i < 4U; ++i)
    {
        if (i > 0U) { app(o, ", "); }
        app_f64(o, v[i]);
    }
    app(o, "]");
}
} // namespace

crd::containers::String emit_material_toml(const MaterialDesc& desc, crd::memory::IAllocator* a)
{
    crd::containers::String o(a);
    app(o, "schema = ");
    app_i32(o, static_cast<crd::i32>(kMaterialSchemaVersion));
    app(o, "\nname   = ");
    app_quoted(o, desc.name);
    app(o, "\n");

    for (crd::usize i = 0; i < desc.params.size(); ++i)
    {
        app(o, "\n[[param]]\nname  = ");
        app_quoted(o, desc.params[i].name);
        app(o, "\nvalue = ");
        app_value(o, static_cast<const double*>(desc.params[i].value), desc.params[i].comps);
        app(o, "\n");
    }

    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const MatNodeDesc& n = desc.nodes[i];
        const crd::i32     oi = find_op(crd::containers::StringView(n.op.c_str(), n.op.size()));
        app(o, "\n[[node]]\nname   = ");
        app_quoted(o, n.name);
        app(o, "\nop     = ");
        app_quoted(o, n.op);
        app(o, "\ninputs = [");
        for (crd::usize k = 0; k < n.inputs.size(); ++k)
        {
            if (k > 0U) { app(o, ", "); }
            const MatInput& in = n.inputs[k];
            if (in.kind == MatInputKind::Node) { app_quoted(o, in.name); }
            else if (in.kind == MatInputKind::Param)
            {
                // ⭐ The `$` is what distinguishes a PARAMETER from a node, and it has to be written back or an
                // instance override would silently stop reaching the graph it was authored against.
                app(o, "\"$");
                o.append(in.name.c_str());
                app(o, "\"");
            }
            // ⛔ An ATTRIBUTE goes back out as an INTEGER. Round-tripping a swizzle index as `0.0` still parses,
            // but it reads as a value where the file means a channel — and an editor's save is what an author
            // reads next.
            else if (oi >= 0 && (kOps[static_cast<crd::usize>(oi)].attr_mask & (1U << k)) != 0U)
            {
                app_i32(o, static_cast<crd::i32>(in.value[0]));
            }
            else { app_value(o, static_cast<const double*>(in.value), in.comps); }
        }
        app(o, "]\n");
    }

    for (crd::usize i = 0; i < desc.instances.size(); ++i)
    {
        const MatInstanceDesc& inst = desc.instances[i];
        app(o, "\n[[instance]]\nname = ");
        app_quoted(o, inst.name);
        app(o, "\nset  = {");
        for (crd::usize k = 0; k < inst.overrides.size(); ++k)
        {
            app(o, k > 0U ? ", " : " ");
            o.append(inst.overrides[k].name.c_str());
            app(o, " = ");
            app_value(o, static_cast<const double*>(inst.overrides[k].value), inst.overrides[k].comps);
        }
        app(o, inst.overrides.empty() ? "}\n" : " }\n");
    }

    app(o, "\n[surface]\n");
    const char* keys[6] = {"base_color", "metallic", "roughness", "normal", "emissive", "opacity"};
    const crd::containers::String* vals[6] = {&desc.surface.base_color, &desc.surface.metallic,
                                              &desc.surface.roughness,  &desc.surface.normal,
                                              &desc.surface.emissive,   &desc.surface.opacity};
    for (int i = 0; i < 6; ++i)
    {
        if (vals[i]->empty()) { continue; }
        app(o, keys[i]);
        app(o, " = ");
        app_quoted(o, *vals[i]);
        app(o, "\n");
    }
    return o;
}

// ── COOK ──────────────────────────────────────────────────────────────────────────────────────────────────

// ── ⭐⭐ 38-G1: the POST cook — see the header contract. Deliberately WITHOUT the material walker's
// param/instance machinery (a display transform is frame-authored, not instanced); a post graph that needs a
// tunable takes it as a wired literal today and a declared uniform when the vocabulary grows one.
int cook_post_graph(const MaterialDesc& desc, crd::kir::KGraph& g, crd::containers::String* where)
{
    const auto fail = [&](std::string_view what) {
        if (where != nullptr)
        {
            where->clear();
            where->append(what.data(), what.size());
        }
        return -1;
    };
    if (desc.nodes.size() == 0U) { return fail("empty"); }
    const crd::kir::Shape sh1 = crd::kir::make_shape({1});
    crd::containers::Array<int> built(crd::memory::default_allocator());
    built.resize(static_cast<crd::u32>(desc.nodes.size()), -1);
    int out_node = -1;
    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const MatNodeDesc&     n = desc.nodes[i];
        const std::string_view opv(n.op.c_str(), n.op.size());
        // the TWO-SIDED legality, post face: lighting ops stay out (a post pass shades nothing), and surface
        // readers stay out (a display transform that sampled a normal would be a material in a post costume)
        if (is_lighting_op(opv)) { return fail(std::string_view(n.name.c_str(), n.name.size())); }
        if (is_surface_reader_op(opv)) { return fail(std::string_view(n.name.c_str(), n.name.size())); }
        const crd::i32 oi = find_op(crd::containers::StringView(n.op.c_str(), n.op.size()));
        if (oi < 0) { return fail(opv); }
        const OpEntry& oe = kOps[static_cast<crd::usize>(oi)];
        if (n.inputs.size() != oe.arity) { return fail(std::string_view(n.name.c_str(), n.name.size())); }
        int in[kMaxNodeInputs] = {};
        for (crd::usize k = 0; k < n.inputs.size(); ++k)
        {
            const MatInput& mi = n.inputs[k];
            if ((oe.attr_mask >> k) & 1U) // a compile-time attribute rides through as its integer
            {
                in[k] = static_cast<int>(mi.value[0]);
                continue;
            }
            if (mi.kind == MatInputKind::Node)
            {
                int found = -1;
                for (crd::usize p = 0; p < i; ++p)
                {
                    if (desc.nodes[p].name.size() == mi.name.size()
                        && std::memcmp(desc.nodes[p].name.c_str(), mi.name.c_str(), mi.name.size()) == 0)
                    {
                        found = built[static_cast<crd::u32>(p)];
                        break;
                    }
                }
                if (found < 0) { return fail(std::string_view(mi.name.c_str(), mi.name.size())); }
                in[k] = found;
            }
            else // literal (Param has no post meaning yet — a param input is a refusal, not a silent default)
            {
                if (mi.kind == MatInputKind::Param) { return fail(std::string_view(n.name.c_str(), n.name.size())); }
                const int c0 = g.constant(mi.value[0], sh1, crd::kir::DType::F32);
                if (mi.comps <= 1U) { in[k] = c0; }
                else
                {
                    const int c1 = g.constant(mi.value[1], sh1, crd::kir::DType::F32);
                    const int c2 = g.constant(mi.value[2], sh1, crd::kir::DType::F32);
                    if (mi.comps == 2U) { in[k] = g.vec2(c0, c1); }
                    else if (mi.comps == 3U) { in[k] = g.vec3(c0, c1, c2); }
                    else { in[k] = g.vec4(c0, c1, c2, g.constant(mi.value[3], sh1, crd::kir::DType::F32)); }
                }
            }
        }
        const int r = material_build_op(g, crd::containers::StringView(n.op.c_str(), n.op.size()),
                                        static_cast<const int*>(in), static_cast<crd::u32>(n.inputs.size()));
        if (r < 0) { return fail(std::string_view(n.name.c_str(), n.name.size())); }
        built[static_cast<crd::u32>(i)] = r;
        if (n.name.size() == 6U && std::memcmp(n.name.c_str(), "output", 6U) == 0) { out_node = r; }
    }
    // ⛔ the OUTPUT is named, never inferred: "the last node" changes meaning under a reorder that alters
    // nothing else, which is exactly the silent-drift shape every vocabulary here refuses.
    if (out_node < 0) { return fail("output"); }
    return out_node;
}

int cook_material(const MaterialDesc& desc, crd::kir::KGraph& g, int struct_id, crd::containers::StringView instance,
                  crd::kir::ShapeIssue* shape_issue)
{
    if (validate_material(desc, nullptr) != MaterialCookError::Ok) { return -1; }

    // The parameter set this cook uses: the declared defaults, with an instance's overrides applied on top.
    // ⛔ Resolved to VALUES here rather than left as a runtime indirection, because that is what makes an instance
    // a COOK-TIME specialisation: the constant folder then sees a literal and a `mix` with weight 0 costs nothing.
    crd::containers::Array<double>   pv(crd::memory::default_allocator());
    crd::containers::Array<crd::u32> pc(crd::memory::default_allocator());
    for (crd::usize i = 0; i < desc.params.size(); ++i)
    {
        for (int k = 0; k < 4; ++k) { pv.push_back(desc.params[i].value[k]); }
        pc.push_back(desc.params[i].comps);
    }
    if (instance.size() > 0U)
    {
        crd::i32 found = -1;
        for (crd::usize i = 0; i < desc.instances.size(); ++i)
        {
            if (desc.instances[i].name.size() == instance.size()
                && std::memcmp(desc.instances[i].name.c_str(), instance.data(), instance.size()) == 0)
            {
                found = static_cast<crd::i32>(i);
                break;
            }
        }
        if (found < 0) { return -1; } // ⛔ an unknown instance FAILS; falling back to defaults would hide a typo
        const MatInstanceDesc& inst = desc.instances[static_cast<crd::usize>(found)];
        for (crd::usize o = 0; o < inst.overrides.size(); ++o)
        {
            for (crd::usize p = 0; p < desc.params.size(); ++p)
            {
                if (desc.params[p].name.size() != inst.overrides[o].name.size()
                    || std::memcmp(desc.params[p].name.c_str(), inst.overrides[o].name.c_str(),
                                   desc.params[p].name.size()) != 0)
                {
                    continue;
                }
                for (int k = 0; k < 4; ++k) { pv[p * 4U + static_cast<crd::usize>(k)] = inst.overrides[o].value[k]; }
                pc[p] = inst.overrides[o].comps;
                break;
            }
        }
    }

    const auto sh1 = crd::kir::make_shape({1});
    const auto make_const = [&](const double* v, crd::u32 comps) {
        const int c0 = g.constant(v[0], sh1, crd::kir::DType::F32);
        if (comps <= 1U) { return c0; }
        const int c1 = g.constant(v[1], sh1, crd::kir::DType::F32);
        if (comps == 2U) { return g.vec2(c0, c1); }
        const int c2 = g.constant(v[2], sh1, crd::kir::DType::F32);
        if (comps == 3U) { return g.vec3(c0, c1, c2); }
        return g.vec4(c0, c1, c2, g.constant(v[3], sh1, crd::kir::DType::F32));
    };

    crd::containers::Array<int> built(crd::memory::default_allocator());
    for (crd::usize i = 0; i < desc.nodes.size(); ++i)
    {
        const MatNodeDesc& n                  = desc.nodes[i];
        const crd::i32     oi                 = find_op(crd::containers::StringView(n.op.c_str(), n.op.size()));
        if (oi < 0) { return -1; }
        const crd::u32 attr_mask              = kOps[static_cast<crd::usize>(oi)].attr_mask;
        int            in[kMaxNodeInputs]     = {-1, -1, -1, -1, -1, -1, -1};
        for (crd::usize k = 0; k < n.inputs.size(); ++k)
        {
            const MatInput& mi = n.inputs[k];
            // ⛔ An ATTRIBUTE slot carries the integer ITSELF, not a node that evaluates to it. Validation has
            // already established it is a literal in range, so this is a pass-through and not a reinterpretation.
            if ((attr_mask & (1U << k)) != 0U)
            {
                in[k] = static_cast<int>(mi.value[0]);
                continue;
            }
            if (mi.kind == MatInputKind::Literal) { in[k] = make_const(static_cast<const double*>(mi.value), mi.comps); }
            else if (mi.kind == MatInputKind::Param)
            {
                for (crd::usize p = 0; p < desc.params.size(); ++p)
                {
                    if (desc.params[p].name.size() == mi.name.size()
                        && std::memcmp(desc.params[p].name.c_str(), mi.name.c_str(), mi.name.size()) == 0)
                    {
                        in[k] = make_const(&pv[p * 4U], pc[p]);
                        break;
                    }
                }
            }
            else
            {
                for (crd::usize s = 0; s < i; ++s)
                {
                    if (desc.nodes[s].name.size() == mi.name.size()
                        && std::memcmp(desc.nodes[s].name.c_str(), mi.name.c_str(), mi.name.size()) == 0)
                    {
                        in[k] = built[s];
                        break;
                    }
                }
            }
            if (in[k] < 0) { return -1; }
        }
        const int r = dispatch_op(g, crd::containers::StringView(n.op.c_str(), n.op.size()),
                                  static_cast<const int*>(in));
        if (r < 0) { return -1; }
        built.push_back(r);
    }

    const auto node_of = [&](const crd::containers::String& nm, int fallback) {
        if (nm.empty()) { return fallback; }
        for (crd::usize s = 0; s < desc.nodes.size(); ++s)
        {
            if (desc.nodes[s].name.size() == nm.size()
                && std::memcmp(desc.nodes[s].name.c_str(), nm.c_str(), nm.size()) == 0)
            {
                return built[s];
            }
        }
        return fallback;
    };
    // ⛔ The DEFAULTS are the OpenPBR ones, not zeroes: an unbound roughness of 0 is a mirror, and an unbound
    // normal of (0,0,0) is a degenerate frame. A field the author did not wire must render as the spec's neutral
    // surface, which is the only value that is obviously "not authored yet" rather than subtly wrong.
    const int one   = g.constant(1.0, sh1, crd::kir::DType::F32);
    const int zero  = g.constant(0.0, sh1, crd::kir::DType::F32);
    const int base  = node_of(desc.surface.base_color, g.vec3(one, one, one));
    const int metal = node_of(desc.surface.metallic, zero);
    const int rough = node_of(desc.surface.roughness, one);
    const int norm  = node_of(desc.surface.normal, g.vec3(zero, zero, one));
    const int emis  = node_of(desc.surface.emissive, g.vec3(zero, zero, zero));
    const int opac  = node_of(desc.surface.opacity, one);
    // Ambient OCCLUSION is not an authored surface field here: it is a SCREEN-SPACE or baked term the technique
    // supplies, so a material that could write it would be describing something it cannot know. Fixed at 1.
    const int surface = crd::kir::material::build_surface(g, struct_id, base, metal, rough, norm, emis, one, opac);
    // ⛔ THE SHAPE CHECK (REN-38 audit): a mis-built node — mismatched widths, an out-of-range component, a
    // comparison sampler through the wrong sample op — used to leave the cook with a VALID node id and fail in
    // the SHADER COMPILER, far from the asset, with nothing pointing at the cause. Refuse it here, by name.
    if (surface >= 0 && !crd::kir::graph_shapes_valid(g, surface, crd::memory::default_allocator(), shape_issue))
    {
        return -1;
    }
    return surface;
}

bool material_op_post_only(crd::u32 i) noexcept
{
    if (i >= material_op_count()) { return false; }
    return is_post_op(std::string_view(kOps[i].name));
}
crd::u32    material_op_count() noexcept { return static_cast<crd::u32>(sizeof(kOps) / sizeof(kOps[0])); }
const char* material_op_name(crd::u32 i) noexcept { return i < material_op_count() ? kOps[i].name : nullptr; }
crd::u32    material_op_arity(crd::u32 i) noexcept { return i < material_op_count() ? kOps[i].arity : 0U; }

bool material_op_exists(crd::containers::StringView op) noexcept { return find_op(op) >= 0; }

int material_build_op(crd::kir::KGraph& g, crd::containers::StringView op, const int* in, crd::u32 n)
{
    const crd::i32 oi = find_op(op);
    if (oi < 0 || in == nullptr || n != kOps[static_cast<crd::usize>(oi)].arity) { return -1; }
    return dispatch_op(g, op, in);
}

bool material_op_arg_is_attr(crd::u32 op_index, crd::u32 arg_index) noexcept
{
    if (op_index >= material_op_count() || arg_index >= kOps[op_index].arity) { return false; }
    return (kOps[op_index].attr_mask & (1U << arg_index)) != 0U;
}
crd::i32 material_op_attr_min(crd::u32 i) noexcept { return i < material_op_count() ? kOps[i].attr_lo : 0; }
crd::i32 material_op_attr_max(crd::u32 i) noexcept { return i < material_op_count() ? kOps[i].attr_hi : 0; }

crd::u32 material_op_arg_width(crd::u32 op_index, crd::u32 arg_index) noexcept
{
    if (op_index >= material_op_count() || arg_index >= kOps[op_index].arity) { return 0U; }
    return kOps[op_index].width[arg_index];
}

const char* material_cook_error_text(MaterialCookError e) noexcept
{
    switch (e)
    {
    case MaterialCookError::Ok:                return "ok";
    case MaterialCookError::ParseFailed:       return "not valid TOML";
    case MaterialCookError::BadSchema:         return "missing or unsupported `schema`";
    case MaterialCookError::MissingName:       return "a node, param or instance has no name";
    case MaterialCookError::DuplicateName:     return "two nodes, params or instances share a name";
    case MaterialCookError::UnknownOp:         return "a node names an operation the registry does not have";
    case MaterialCookError::UnknownInput:      return "an input names a node or param that does not exist";
    case MaterialCookError::TooManyInputs:     return "a node declares more inputs than the cap";
    case MaterialCookError::WrongArity:        return "the right op with the wrong number of inputs";
    case MaterialCookError::NodeCycle:         return "the nodes form a CYCLE; a material graph is a DAG";
    case MaterialCookError::SurfaceUnbound:    return "the surface names a node that does not exist";
    case MaterialCookError::NoBaseColor:       return "nothing feeds base colour";
    case MaterialCookError::UnknownOverride:   return "an instance overrides a parameter that was never declared";
    case MaterialCookError::ForbiddenLighting: return "a material may not reach for lighting state (ADR-0102)";
    case MaterialCookError::AttrNotConstant:   return "a compile-time argument (index/width/location/flag) was wired";
    case MaterialCookError::AttrOutOfRange:    return "a compile-time argument is outside the range it accepts";
    }
    return "unknown error";
}

} // namespace crd::matcook
