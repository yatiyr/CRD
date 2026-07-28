#pragma once

// material_asset.hpp — REN-38-C1/C2/C3 (D-007 rows 141): THE MATERIAL AS AN AUTHORED ASSET.
//
// ⛔ WHAT THIS ROW REPLACES. A material was a C++ FUNCTION POINTER — `MaterialTemplate::build_surface` — so
// inventing a material meant editing and recompiling the engine. The REN-37 design diagram already claimed a
// `.crdm` existed; it did not. This is that file: an OpenPBR surface expressed as DATA.
//
// ⭐ THE SHAPE IS THE ONE THE TECHNIQUE ASSET ALREADY USES (REN-37.2), on purpose. A `.crdm` is a NODE GRAPH that
// cooks to CKIR and is SPLICED into the pass's fragment program exactly as an authored technique is. Nothing here
// invents a second mechanism: the node library is `ckir_nodes.hpp` (the ~90 MaterialX-class nodes B6 built), the
// surface contract is `ckir_material.hpp`, and the splice is `splice_graph`.
//
// ⛔ THE ADR-0102 LINE, RESTATED BECAUSE THIS IS WHERE IT IS EASIEST TO CROSS. A material describes SURFACE
// RESPONSE — base colour, metallic, roughness, normal, emission. It does NOT describe lighting: no light loop, no
// shadow lookup, no exposure. Those belong to the TECHNIQUE (`.crdt`). A `.crdm` that could sample a shadow map
// would make every material carry a copy of the lighting model, which is precisely the coupling REN-37 removed.
//
// ⛔ NO LOGIC, SAME AS THE FRAME ASSET (ADR-0081). A node graph is a DAG of named operations over named inputs.
// There are no expressions, no branches, no loops the author writes — a `mix`/`ifgreater` node is a NODE, and
// what it computes is CKIR. Data describes topology; C++ describes mechanics; CKIR describes math.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{
struct ShapeIssue; // ckir_shape.hpp — the REN-38 shape check's offending-node report
}

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::matcook
{

inline constexpr crd::u32 kMaterialSchemaVersion = 1U;

// How many inputs one node may name. ⛔ A CAP, stated rather than hidden — and set from the ACTUAL widest node in
// the library (`gooch_shade` and `range` take seven) rather than from a guess. A cap below the widest node does not
// "leave headroom": it makes those nodes UNAUTHORABLE, with the asset rejected for a reason that names the wrong
// thing. Exceeding it is a NAMED cook error, never a truncated wiring.
inline constexpr crd::u32 kMaxNodeInputs = 7U;

// What a node input REFERS TO. ⛔ Three distinct things, distinguished at parse time rather than guessed from
// context: a literal, another node's output, and a named PARAMETER whose value an instance may override. Guessing
// would make `"roughness"` mean "the parameter" in one asset and "the node called roughness" in another.
enum class MatInputKind : crd::u8
{
    Literal = 0, // a number written in the file
    Node,        // another node in this graph, by name
    Param,       // a declared parameter, by name — the thing a material INSTANCE overrides
};

struct MatInput
{
    MatInputKind            kind = MatInputKind::Literal;
    crd::containers::String name;              // Node/Param only
    double                  value[4] = {0.0, 0.0, 0.0, 0.0}; // Literal only
    crd::u32                comps    = 1U;     // Literal only: 1..4

    explicit MatInput(crd::memory::IAllocator* a) : name(a) {}
};

// One node of the graph: an operation NAME from the CKIR node library, its inputs, and the name later nodes
// (and the surface mapping) refer to it by.
struct MatNodeDesc
{
    crd::containers::String        name;
    crd::containers::String        op; // "multiply", "mix", "fractal_noise", … — resolved by the registry
    crd::containers::Array<MatInput> inputs;

    explicit MatNodeDesc(crd::memory::IAllocator* a) : name(a), op(a), inputs(a) {}
};

// A declared PARAMETER — the material-frequency (set 2) block, as data. Its DEFAULT lives here; an INSTANCE
// overrides it without touching the graph, which is what makes one authored material serve N looks.
struct MatParamDesc
{
    crd::containers::String name;
    double                  value[4] = {0.0, 0.0, 0.0, 0.0};
    crd::u32                comps    = 1U;

    explicit MatParamDesc(crd::memory::IAllocator* a) : name(a) {}
};

// REN-38-C3: one INSTANCE — the same graph with a different parameter set. ⛔ An instance may only override
// parameters that EXIST; a typo would otherwise be silently ignored and the artist would see the default with
// nothing to explain why.
struct MatInstanceDesc
{
    crd::containers::String                name;
    crd::containers::Array<MatParamDesc>   overrides;

    explicit MatInstanceDesc(crd::memory::IAllocator* a) : name(a), overrides(a) {}
};

// Which node feeds which OpenPBR surface field. ⛔ Named fields, not positional: a material that wired roughness
// into the metallic slot would render plausibly and wrongly, and position is exactly the kind of mistake a
// diff cannot show.
struct MatSurfaceDesc
{
    crd::containers::String base_color;
    crd::containers::String metallic;
    crd::containers::String roughness;
    crd::containers::String normal;
    crd::containers::String emissive;
    crd::containers::String opacity;

    explicit MatSurfaceDesc(crd::memory::IAllocator* a)
        : base_color(a), metallic(a), roughness(a), normal(a), emissive(a), opacity(a)
    {
    }
};

struct MaterialDesc
{
    crd::containers::String                 name;
    crd::u32                                schema = kMaterialSchemaVersion;
    crd::containers::Array<MatParamDesc>    params;
    crd::containers::Array<MatNodeDesc>     nodes;
    crd::containers::Array<MatInstanceDesc> instances;
    MatSurfaceDesc                          surface;

    explicit MaterialDesc(crd::memory::IAllocator* a)
        : name(a), params(a), nodes(a), instances(a), surface(a)
    {
    }
};

// Every way a `.crdm` can be wrong. ⛔ One value per distinct mistake, because "invalid material" tells an author
// nothing — and these are all mistakes a diff cannot show.
enum class MaterialCookError : crd::u8
{
    Ok = 0,
    ParseFailed,
    BadSchema,
    MissingName,
    DuplicateName,       // two nodes, params or instances share a name
    UnknownOp,           // a node names an operation the registry does not have
    UnknownInput,        // an input names a node or param that does not exist
    TooManyInputs,       // more than kMaxNodeInputs
    WrongArity,          // the right op, the wrong number of inputs
    NodeCycle,           // node A takes B takes A — a graph, not a DAG
    SurfaceUnbound,      // the surface names a node that does not exist
    NoBaseColor,         // nothing feeds base colour — the one field with no sane default
    UnknownOverride,     // an instance overrides a parameter that was never declared
    ForbiddenLighting,   // a node reaches for lighting/shadow state — see the ADR-0102 note above
    AttrNotConstant,     // a COMPILE-TIME argument (a swizzle index, a varying location) was wired, not written
    AttrOutOfRange,      // …or written outside the range that argument accepts
};

[[nodiscard]] const char* material_cook_error_text(MaterialCookError err) noexcept;

// TEXT → description. `where` receives the offending name when there is one.
[[nodiscard]] MaterialCookError parse_material_toml(crd::containers::StringView toml_text, MaterialDesc& out,
                                                    crd::containers::String* where = nullptr);

// The same rules, applied to a PROGRAMMATIC description. ⛔ The two provenances are held to the SAME rules: a
// check only the text path performed would make the ergonomic path the unsafe one.
[[nodiscard]] MaterialCookError validate_material(const MaterialDesc& desc, crd::containers::String* where = nullptr);

// description → `.crdm`, the editor round-trip half (the frame asset's rule, one asset over): `parse → emit →
// parse` must produce an identical description, or a node-editor save can silently drop what it did not know.
[[nodiscard]] crd::containers::String emit_material_toml(const MaterialDesc& desc, crd::memory::IAllocator* a);

// ── ⭐ THE COOK: a validated description → CKIR. ───────────────────────────────────────────────────────────
// Builds the node graph into `g` and returns the OpenPBR surface struct node, ready for `build_fs_for_pass` —
// the same value a C++ `build_surface` used to return, which is what makes this a REPLACEMENT rather than a
// parallel path. `instance` selects a parameter set by name (empty = the declared defaults).
// Returns a negative node id when the description is invalid or the instance is unknown.
// `shape_issue` (optional) receives the offending node + reason when the REN-38 shape check refuses the built
// graph — a refusal with nothing pointing at the cause is the exact failure mode the check exists to end.
[[nodiscard]] int cook_material(const MaterialDesc& desc, crd::kir::KGraph& g, int struct_id,
                                crd::containers::StringView instance = {},
                                crd::kir::ShapeIssue* shape_issue = nullptr);

// ── THE NODE REGISTRY ─────────────────────────────────────────────────────────────────────────────────────
// How many operations an asset may name, and what arity each takes. Exposed so a gate can assert COVERAGE —
// ⛔ a registry that silently lacked half the node library would let a material cook and render the wrong thing,
// and nothing in the asset would look wrong.
[[nodiscard]] crd::u32    material_op_count() noexcept;
[[nodiscard]] const char* material_op_name(crd::u32 index) noexcept;
[[nodiscard]] crd::u32    material_op_arity(crd::u32 index) noexcept;
[[nodiscard]] bool        material_op_exists(crd::containers::StringView op) noexcept;
// 38-G1: true when the op is POST-CONTEXT ONLY (legal in `cook_post_graph`, refused in `cook_material`).
[[nodiscard]] bool        material_op_post_only(crd::u32 index) noexcept;

// ⛔⛔ NOT EVERY ARGUMENT IS A WIRE, and in C++ the two are spelled identically — both `int`. `extract(v, index)`,
// `convert_f_vec(a, width)`, `place2d(…, order)`, `range(…, doclamp)`, `facingratio(…, ff, invert)` and the
// geometric readers' `location` are COMPILE-TIME ATTRIBUTES: they choose a channel, a width, a branch or a varying
// slot. Passing a node id where one of those belongs type-checks, builds, and swizzles component 47 — a graph that
// compiles and computes something unrelated. So the registry says which is which, and the asset must WRITE an
// attribute as a plain integer rather than wire it. `min`/`max` bound every attribute slot of the op.
[[nodiscard]] bool        material_op_arg_is_attr(crd::u32 op_index, crd::u32 arg_index) noexcept;
[[nodiscard]] crd::i32    material_op_attr_min(crd::u32 op_index) noexcept;
[[nodiscard]] crd::i32    material_op_attr_max(crd::u32 op_index) noexcept;

// ⭐ BUILD one registered op into `g` — the registry's dispatch, exposed. A SECOND authored graph (the vertex
// program's displacement, 38-D3) must reach the SAME node library; a parallel one would be two vocabularies that
// drift, and an author would have no way to know which file understood which node. `in` holds `arity` values:
// a node id for a wire slot, the integer itself for an attribute slot. Returns <0 for an unknown op.
[[nodiscard]] int         material_build_op(crd::kir::KGraph& g, crd::containers::StringView op, const int* in,
                                            crd::u32 n);

// ── ⭐⭐ 38-G1: THE POST CONTEXT — the technique library's first asset family. ────────────────────────────
// The SAME `[[node]]` vocabulary and registry, under the OTHER legality: the post-only ops (`agx`,
// `srgb_encode`, `pq_encode`, `pbr_neutral`, `ev100`/`exposure_scale`, `gamut_compress`, `contrast_curve`)
// are LEGAL here and refused in materials; the surface readers (`geomcolor`, `normal`, …) are refused here
// (`texcoord` stays — in a fullscreen pass it is the screen coordinate a post graph samples by). The graph's
// OUTPUT is the node named "output" (validated, never inferred). Returns the colour node id, or a negative
// value with `where` naming the offending node — the same loud-refusal contract as `cook_material`.
// 38-G1: parse WITHOUT the material-context validation (the post face of the same grammar).
[[nodiscard]] MaterialCookError parse_post_toml(crd::containers::StringView toml_text, MaterialDesc& out,
                                                crd::containers::String* where = nullptr);
[[nodiscard]] int cook_post_graph(const MaterialDesc& desc, crd::kir::KGraph& g,
                                  crd::containers::String* where = nullptr);

// The natural component count of a WIRE argument (1 = scalar/polymorphic, 2/3/4 = a vector the op indexes into).
// ⛔ Real registry information, not test scaffolding: `over` swizzles `.a`, so feeding it a float is a silently
// wrong picture, and a node editor cannot draw or validate a socket it does not know the width of.
[[nodiscard]] crd::u32    material_op_arg_width(crd::u32 op_index, crd::u32 arg_index) noexcept;

} // namespace crd::matcook
