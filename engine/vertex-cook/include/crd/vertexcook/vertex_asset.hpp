#pragma once

// vertex_asset.hpp — REN-38-D1/D2/D3/D4 (D-007 row 141): THE VERTEX PROGRAM AS AN AUTHORED ASSET.
//
// ⛔ WHAT THIS ROW REPLACES. `build_scene_vs_shadowed` / `_skinned` / `build_shadow_vs` are ~200 lines of C++ with
// the vertex-pull layout COMPILED IN: the header word map is a list of magic numbers (`hdru(3)` is the vertex
// section), the vertex record is `pos3 · normal3 · uv2 · tangent4` in code, the instance record is `mat4 + rgba`,
// skinning is exactly four influences of linear blending, and the varying set is four hardcoded `ve.out[]` slots.
// Changing ANY of it — reading the tangent that is already in the buffer and that NO SHADER CAN SEE, adding a
// per-instance material index, skinning with eight influences — is an engine edit and a recompile.
//
// ⭐ SAME SHAPE AS `.crdm` AND `.crdt`, on purpose. A `.crdv` is a DECLARATION plus (D3) a node graph over the
// vertex, cooked to a CKIR `KEntry` the raster context consumes exactly as it consumes a hand-built one. The node
// library is the material registry (`crd-material-cook`), not a second one.
//
// ⛔ NO LOGIC (ADR-0081). The asset says WHERE the data is and WHAT it becomes; C++ says how a pull works; CKIR
// says the math.
//
// ⛔⛔ THE ONE THING THAT SILENTLY RENDERS WRONG (38-D4). The VS emits varyings at locations 0..3 and every cooked
// FS reads locations 0..3 BY CONVENTION. Nothing checked that they agree, and nothing on either backend can: a
// renamed, reordered or dropped varying still links, still binds, and feeds the fragment shader a different field.
// `verify_varying_contract` is that check, by NAME, at cook time.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{
struct ShapeIssue; // ckir_shape.hpp — the REN-38 shape check's offending-node report
}

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::vertcook
{

inline constexpr crd::u32 kVertexSchemaVersion = 1U;

// Caps, stated rather than hidden. A malformed asset must be REFUSED, never allowed to allocate unboundedly.
inline constexpr crd::u32 kMaxAttributes  = 16U; // vertex-stream attributes
inline constexpr crd::u32 kMaxVaryings    = 8U;  // == kMaxStageOutputs; a VS cannot emit more than CKIR carries
inline constexpr crd::u32 kMaxVaryingSrc  = 4U;  // source terms concatenated into one varying
inline constexpr crd::u32 kMaxInfluences  = 8U;  // skin influences per vertex
inline constexpr crd::u32 kMaxMorphTargets = 8U;

// ⛔⛔ HOW AN ATTRIBUTE TRANSFORMS IS PART OF WHAT IT IS. A position is affine (the translation column applies); a
// direction is rotation-only. Transforming a normal with the translation column shifts it by the instance's world
// position — lighting that MOVES with the object, which reads as a broken material rather than a broken transform.
// A value (a uv, a vertex colour) is not transformed at all. Declared per attribute so the cook cannot guess.
enum class AttrKind : crd::u8
{
    Position = 0, // affine: M · (p, 1)
    Direction,    // rotation only: M3x3 · d  (a 4-component direction passes its 4th component through — the
                  // tangent's handedness sign is not a coordinate)
    Value,        // untransformed
};

struct VertexAttrDesc
{
    crd::containers::String name;
    crd::u32                offset = 0U; // word offset inside the vertex record
    crd::u32                comps  = 1U; // 1..4
    AttrKind                kind   = AttrKind::Value;

    explicit VertexAttrDesc(crd::memory::IAllocator* a) : name(a) {}
};

// ⛔ THE HEADER WORD MAP, AS DATA. Every one of these was a bare integer inside the VS (`hdru(3)`). The header is
// APPEND-ONLY by discipline (see `scene_renderer.hpp`), and the moment a shader hardcodes a word, growing the
// header silently feeds every pull shader the wrong field. Declaring the map is what makes the discipline
// checkable instead of remembered.
struct VertexHeaderMap
{
    crd::u32 index_count  = 0U;
    crd::u32 index_off    = 2U;
    crd::u32 vertex_off   = 3U;
    crd::u32 instance_off = 4U;
    crd::u32 visible_off  = 5U;
    crd::u32 view_proj    = 6U;  // 16 floats, column-major
    crd::u32 light_vp     = 32U; // N × 16 floats, column-major
    crd::u32 skin_off     = 25U;
    crd::u32 palette_off  = 26U;
    crd::u32 joint_count  = 27U;
    crd::u32 morph_off    = 0U;  // 0 = the asset declares no morph stream
    crd::u32 morph_weights = 0U; // 0 = none
    // REN-38-F6+ (appended at the END — the emitter iterates this struct as a POD word array): the header word
    // holding the TOTAL instance count. ⛔ The cull kernel MUST guard on it: threads past the instance section
    // read garbage transforms, and their verdicts polluted the indirect-args atomic (the VK cull gate caught
    // exactly that — 16 marked of 8 visible).
    crd::u32 instance_count = 1U;
    // ⭐⭐ REN-41 (velocity): the header WORDS holding the previous-frame SECTION offsets — ALSO appended at the
    // END (the POD-word-array emit walk + its name table both grow to match). 0 = the asset declares no velocity
    // (the cook emits no `prev:clip`). prev_world is per-instance (16 words); prev_palette is the skinned previous
    // pose (0 for a non-skinned group).
    crd::u32 prev_world_off   = 0U;
    crd::u32 prev_palette_off = 0U;
};

// Which matrix this program projects by. ⛔ A SHADOW pass is not "the scene VS with a different matrix" as a
// special case in C++ — it is the same declaration with a different transform, which is what makes a per-cascade
// variant a cook-time parameter rather than a hand-written function.
enum class VertexTransform : crd::u8
{
    ViewProj = 0,
    LightVp,  // `cascade` selects the header slice — a COMPILE-TIME constant, so one binding serves every cascade
    None,     // already-clip-space geometry (a fullscreen pass)
};

// ── 38-D2: SKINNING, DECLARED ────────────────────────────────────────────────────────────────────────────────
// ⛔ Today: exactly four influences, linear blending, a 6-word skin record — all in C++. A character rig that
// needs eight influences, or a shoulder that needs dual-quaternion blending to stop collapsing, is an engine edit.
enum class SkinScheme : crd::u8
{
    None = 0,
    LinearBlend,     // LBS: Σ wₖ·(Mₖ·p). Cheap, and it COLLAPSES a twisted joint toward the axis (the candy wrapper)
    DualQuaternion,  // DQS: blend the rigid parts as unit dual quaternions — volume-preserving through a twist
};

struct SkinDesc
{
    SkinScheme scheme     = SkinScheme::None;
    crd::u32   influences = 4U;
    // The packed skin record: `stride` words per vertex, `joint_words` u32s of 16-bit joint-index PAIRS followed
    // by `influences` f32 weights. Declared because a rig with eight influences packs differently.
    crd::u32   stride      = 6U;
    crd::u32   joint_words = 2U;
    crd::u32   weight_off  = 2U;
    // ⛔ THE PALETTE STRIDE IS PART OF THE SCHEME, not a constant. LBS reads 16 words (a mat4) per joint; DQS
    // reads 8 (a unit dual quaternion). A DQS program pointed at a matrix palette reads every joint at half
    // stride — it animates, smoothly, and it is wrong everywhere.
    crd::u32   palette_stride = 16U;
};

// ── 38-D2: MORPH TARGETS ─────────────────────────────────────────────────────────────────────────────────────
// ⛔ These do not exist AT ALL today — no code, no data path. A blend-shape face is unreachable.
struct MorphDesc
{
    crd::u32 target_count = 0U; // 0 = no morphing
    crd::u32 stride       = 3U; // words per (target, vertex) delta record
    // Which attribute each delta applies to, by name — a morph that displaced the position while the normals
    // stayed put would light a moving face as if it were still.
    crd::containers::Array<crd::containers::String> targets_apply_to;

    explicit MorphDesc(crd::memory::IAllocator* a) : targets_apply_to(a) {}
};

// ── THE VARYINGS ─────────────────────────────────────────────────────────────────────────────────────────────
// One source TERM. A varying is a concatenation of terms, which is how `vec4(world_pos, clip.w)` — the packing the
// hand-written VS did to keep the shadowed path at one extra interpolant — is expressible as data.
enum class VaryingSourceKind : crd::u8
{
    Attribute = 0, // a vertex attribute, in OBJECT space
    World,         // …transformed to world by the instance matrix (per its declared `AttrKind`)
    Instance,      // an instance-record attribute
    ClipW,         // the clip w — the view-space distance a cascade selection needs, already computed
    Node,          // a displacement-graph node (38-D3)
    PrevClip,      // ⭐⭐ REN-41 (velocity): the PREVIOUS-FRAME clip position — `cur_vp · prev_world · skin_prev(pos)`.
                   // 4 comps. Jitter cancels vs the current clip (both use cur_vp), so the FS delta is object motion.
    Clip,          // ⭐⭐ REN-41 (velocity): the CURRENT clip position (4 comps) as an interpolant, so the velocity FS
                   // forms the motion vector from two clips (prev vs cur) with no dependence on the render resolution.
};

struct VaryingSource
{
    VaryingSourceKind       kind = VaryingSourceKind::Attribute;
    crd::containers::String name;
    // ⛔ REQUIRED for a `Node` term and derived for every other kind. A displacement node's width is a property of
    // the graph, not of the declaration — but 38-D4 has to answer "how wide is this varying?" from the ASSET, or
    // the contract check would have to cook first and could not run at all when the pair disagrees. So a node
    // term states its width, and the cook CROSS-CHECKS it against what the graph actually built.
    crd::u32                comps = 0U;

    explicit VaryingSource(crd::memory::IAllocator* a) : name(a) {}
};

struct VaryingDesc
{
    crd::containers::String              name;     // the NAME the fragment side asks for (38-D4)
    crd::u32                             location = 0U;
    bool                                 flat     = false; // Interp::Flat vs Smooth
    crd::containers::Array<VaryingSource> source;

    explicit VaryingDesc(crd::memory::IAllocator* a) : name(a), source(a) {}
};

// ── 38-D3: VS DISPLACEMENT ───────────────────────────────────────────────────────────────────────────────────
// A node graph over the vertex, in OBJECT space, applied before the world transform. Same node vocabulary as a
// material's (`crd-material-cook`'s registry) — a second node library would be the real mistake.
enum class VertInputKind : crd::u8
{
    Literal = 0,
    Node,
    Attribute, // `@position`, `@normal`, … — a vertex attribute by name
    // ── ⭐ REN-38-F7: the PROCEDURAL vertex vocabulary (appended at the END — the kind is cooked identity).
    // A procedural vertex stage (`position = "node:…"`) has no vertex record; its nameable inputs are the
    // EXPANSION indices, the per-instance record fields and the header words of the draw data contract.
    Corner,    // `@corner` — f32 corner index: vid − instance·verts_per_instance
    Instance,  // `@instance` — f32 instance index: vid / verts_per_instance
    Field,     // `field:<k>` — instance-record word k, f32 BIT pattern (int_bits_to_float)
    FieldU,    // `fieldu:<k>` — instance-record word k, raw u32
    FieldC,    // `fieldc:<k>` — instance-record word k, packed RGBA8 → vec4 in [0,1]
    Hdr,       // `hdr:<k>` — header word k, f32 bits
    HdrU,      // `hdru:<k>` — header word k, raw u32
    HdrC,      // `hdrc:<k>` — header word k, packed RGBA8 → vec4
    Category,  // `@category` — 1.0 when this instance's category bit is set in the header mask, else 0.0
};

struct VertInput
{
    VertInputKind           kind = VertInputKind::Literal;
    crd::containers::String name;
    double                  value[4] = {0.0, 0.0, 0.0, 0.0};
    crd::u32                comps    = 1U;
    crd::u32                word     = 0U; // Field*/Hdr* only: the word index after the `:`

    explicit VertInput(crd::memory::IAllocator* a) : name(a) {}
};

struct VertNodeDesc
{
    crd::containers::String          name;
    crd::containers::String          op;
    crd::containers::Array<VertInput> inputs;

    explicit VertNodeDesc(crd::memory::IAllocator* a) : name(a), op(a), inputs(a) {}
};

struct InstanceLayoutDesc
{
    crd::u32                                  stride    = 20U;
    crd::u32                                  transform = 0U;  // mat4 word offset, column-major
    // ⭐ OPTIONAL NORMAL MATRIX. Without one, a `Direction` attribute rides the instance transform's 3×3 — correct
    // only under uniform scale. A non-uniformly scaled instance needs the inverse-transpose, and now that the
    // instance record is AUTHORED an artist can add that slot without an engine edit.
    bool                                      has_normal_transform = false;
    crd::u32                                  normal_transform     = 0U;
    crd::containers::Array<VertexAttrDesc>    attrs;

    explicit InstanceLayoutDesc(crd::memory::IAllocator* a) : attrs(a) {}
};

// ── ⭐⭐ REN-38-F1..F5: THE ADVANCED STAGES. ────────────────────────────────
// ⛔⛔ CKIR HAS FOURTEEN STAGES AND THE ASSET REACHED TWO. The A band gave the raster context every verb —
// `draw_tess_load`, `draw_mesh_load`, `trace_rays`, `dispatch_kernel_indirect`, `draw_visbuffer_load` — and
// nothing could AUTHOR a program for any of them: a tessellated surface, a meshlet pipeline or a ray-traced pass
// still meant writing C++. This is the same `.crdv` with a declared STAGE.
//
// ⭐ ONE DECLARATION, not five formats. Tessellation, mesh shading and the visibility buffer all pull from the
// SAME vertex record and emit the SAME declared varyings — which is exactly why they belong here rather than in
// a parallel vocabulary that would drift from it.
enum class StageKind : crd::u8
{
    Vertex = 0,
    TessControl, // F1 hull: sets the tess levels, passes the patch through
    TessEval,    // F1 domain: displaces the interpolated patch point and projects it
    Task,        // F2 amplification: how many mesh workgroups to launch
    Mesh,        // F2: emits vertices + primitives directly, no index buffer
    VisBuffer,   // F5 fragment: writes the packed instance|primitive id instead of shading
    Cull,        // F4 compute: GPU-driven culling that writes the indirect args
    RayGen,      // F3
    ClosestHit,  // F3
    Miss,        // F3
    // REN-38 audit (the full hit group). APPENDED at the end — the stage is part of the cooked identity.
    AnyHit,      // called per traversal candidate; may IGNORE the hit (alpha-tested RT geometry)
    // REN-38-F13 (appended): the LAST two unreachable CKIR stages.
    Intersection, // procedural geometry: tests the object-space ray against an analytic shape, reports t
    CallableStage, // an SBT subroutine: transforms its callable-data block ("callable" in the asset)
};

// F1. ⛔ A QUAD patch of 4 control points. The tess LEVELS are declared because they are what a technique tunes,
// and a level of 0 collapses the patch to nothing — geometry that silently disappears rather than erroring.
struct TessDesc
{
    crd::u32 patch_size = 4U;
    double   inner      = 8.0;
    double   outer      = 8.0;
};

// F2. ⛔ `max_vertices`/`max_primitives` are a HARDWARE-CAPPED promise the pipeline is created against; a mesh
// program that emits more than it declared writes past its own output arrays. ⛔ REN-38-F6: the procedural
// meshlet grid writes vertex tid as corner (tid % 3) of triangle (tid / 3), so the budgets must satisfy
// max_vertices == 3 * max_primitives — validated, and the defaults honour it (the old 64/124 default put
// primitive 124 at vertices 372..374 of a 64-vertex budget: device UB out of the box).
struct MeshDesc
{
    crd::u32 max_vertices   = 126U;
    crd::u32 max_primitives = 42U;
    crd::u32 workgroup      = 64U;
    // ── REN-38-F6+ (appended). `fetch = true` PULLS real geometry through the scene pull contract (indices →
    // vertex records → visible slot → instance matrix → view_proj) instead of generating a procedural grid —
    // legal now that the mesh emitters lower the sbuf storage read on both backends. Requires a [vertex] record
    // with a position attribute; a declared morph/skin deformer is REFUSED loudly (BadMesh), never silently
    // dropped — the deforming paths belong to the pulling VS.
    bool fetch = false;
    // `payload = true` declares this mesh is DISPATCHED BY A TASK: the emitters then declare the fixed 4-field
    // payload input even when no field is read — HLSL's DispatchMesh always passes one, and the D3D12 PSO
    // validator rejects an AS→MS pair whose payload sizes disagree. A standalone mesh keeps this false.
    bool payload = false;
};

// F2 task/amplification: how many MESH workgroups this task workgroup launches.
struct TaskDesc
{
    crd::u32 emit = 1U;
    // REN-38-F6+ (appended): when >= 0, the mesh-workgroup COUNT is read from this header WORD of the bound
    // storage buffer at dispatch time (GPU-driven amplification — the culled count drives the meshlets);
    // `emit` is then ignored.
    crd::i32 emit_header = -1;
};

// F3. ⛔ The PAYLOAD WIDTH is part of the pipeline contract: raygen, miss and closest-hit must agree on it, and
// a mismatch reads the next payload slot rather than failing to link.
struct RtDesc
{
    crd::u32 payload_words = 1U;
    crd::u32 as_set        = 0U;
    crd::u32 as_binding    = 0U;
    crd::u32 out_binding   = 1U;
    // REN-38 audit: the ANY-HIT's declared alpha cutoff — hits whose barycentric u+v falls below it are
    // IGNORED (`ignoreIntersectionEXT` / `IgnoreHit()`), the portable OMM fallback for alpha-tested geometry.
    double   alpha_cutoff  = 0.0;
    // ── REN-38-F13 (appended). ──
    double   sphere_radius  = 0.5; // crd-lint-allow-untagged-physical: OBJECT-SPACE radius — the modeling space the instance matrix scales, unit-agnostic like the vertex positions themselves (Intersection: analytic sphere centred at 0)
    double   callable_scale = 2.0; // Callable: cd.m0 = cd.m0 * scale + bias — a transform a gate can verify
    double   callable_bias  = 1.0;
    bool     use_callable   = false; // RayGen: after the trace, run the callable over the payload and store ITS result
};

// F4 GPU-driven culling: a compute program that tests each instance and writes the surviving indirect args.
// REN-40-A: `CullDesc::base_word` sentinel — the bound buffer's base IS word 0 (a private per-group buffer).
inline constexpr crd::u32 kCullNoBaseWord = 0xFFFFFFFFU;

struct CullDesc
{
    bool     frustum   = true;
    crd::u32 workgroup = 64U;
    crd::u32 args_off  = 0U; // header word holding the indirect-args offset
    // ── ⭐⭐ REN-40-A: THE COMPACTING CULL — what turns a per-instance FLAG into a GPU-driven draw. ───────────
    // `compact` swaps the flags output for (a) a COMPACTED visible-index list and (b) a real indexed-indirect
    // DRAW command whose instance_count the kernel itself accumulates, so the CPU never learns the count and an
    // empty batch costs nothing. Measured motivation: at 1M instances the CPU cull + list uploads were ~160 ms
    // of a 337 ms frame (docs/bench/2026-07-29-ren40-million-instance-baseline.md).
    bool     compact   = false;
    // ⛔⛔ REN-40-A: the RESET half. `instance_count` is an ATOMIC ACCUMULATOR, so it cannot also be its own
    // initializer once the dispatch spans more than one workgroup — the 38-F15 in-kernel reset is correct ONLY
    // at groups_x = 1, and silently races above it. `reset = true` cooks a one-thread kernel that lays down the
    // command's CONSTANT fields (index_count, instance_count = 0, first_index, base_vertex = 0, first_instance
    // = 0, plus the leading DrawIndex where the backend's layout carries one) as its OWN authored pass, which
    // the frame graph orders BEFORE the cull by the declared write→read edge. Two passes, one vocabulary.
    bool     reset     = false;
    // The per-instance WORLD AABB section (6 floats: min.xyz, max.xyz) as a header word. ⛔ The bounds are what
    // make GPU/CPU parity a DERIVATION rather than a hope: the kernel runs the SAME positive-vertex test over
    // the SAME box the CPU's `aabb_in_frustum` reads. Culling on the transform's translation instead (what the
    // 38-F15 flag variant does) is a POINT test and silently disagrees for anything larger than a texel.
    crd::u32 bounds_off = 0U;
    // ⛔⛔ THE INDIRECT COMMAND LAYOUT IS A BACKEND FACT, stamped at cook time from
    // `IRasterContext::indirect_command_stride()/_arg_offset()`. Vulkan: 20-byte commands, args at 0. D3D12:
    // 24-byte, args at 4, the leading u32 carrying DrawIndex (its command signature prepends a root constant and
    // D3D12 requires the draw argument LAST). Same fields, different ORDER and STRIDE — a kernel that assumed
    // one layout would write garbage on the other backend, which is the clip-space-Y failure shape again.
    crd::u32 draw_stride   = 20U;
    crd::u32 draw_arg_off  = 0U;
    crd::u32 draw_index    = 0U; // the DrawIndex this command carries (written when draw_arg_off != 0)
    crd::u32 index_count   = 0U; // indices per instance — the draw command's first field
    crd::u32 first_index   = 0U; // this batch's index-section base
    // ⭐⭐ REN-40-A: WHICH VIEW this dispatch culls, and therefore WHICH visible list it fills.
    // ⛔ The kernel writes into EXACTLY the region the CPU cull wrote — `visible_off + view_index * capacity`,
    // camera at view 0 and cascade c at view c+1 — so the vertex programs need NO change at all. The only
    // difference between the CPU and GPU paths becomes WHO fills the list and HOW the count reaches the draw.
    // That is the whole elegance of the switch: one authored kernel, five dispatches, zero shader churn.
    crd::u32 view_index    = 0U;
    crd::u32 capacity_word = 101U; // header word holding the per-view visible-list STRIDE (instance capacity)
    // ⭐⭐ REN-40-A: how many VIEW commands a RESET lays down (camera + N cascades). One reset pass writing all
    // of them beats one pass per view: the constant fields are cheap, and an atomic accumulator only needs its
    // zero to land before any cull adds into it — which the graph's write→read edge already guarantees.
    crd::u32 views         = 1U;
    // ⭐⭐ REN-40-A: WHICH FRUSTUM this dispatch tests against, as a header WORD OFFSET.
    // ⛔⛔ THIS IS THE DIFFERENCE BETWEEN A CULL AND A SHADOW-SHAPED LIE. Every view read the CAMERA's
    // view-projection at first, so all four cascade dispatches produced the camera's visible set — the cascade
    // draws then rendered a plausible-looking depth atlas covering the wrong volume, and the frame came back with
    // NO SHADOWS AT ALL while every count matched and nothing failed. Cascade c's clip matrix lives at
    // `light_vp + c*16`, so the offset is a COOK-TIME constant like every other view stamp: 0 means "use
    // `header.view_proj`" (the camera), anything else is the absolute word offset of a 16-float column-major clip
    // matrix. ⭐ The Y-flip the emitters apply to a light matrix is invariant here — negating the Y row swaps the
    // TOP and BOTTOM plane expressions, which leaves the six-plane SET (and therefore every AABB verdict) alone.
    crd::u32 frustum_off   = 0U;
    // ⭐⭐ REN-40-A: WHERE THE GROUP'S BUFFER BASE LIVES, as a word offset inside the ARGS buffer (binding 1).
    // ⛔ A consolidated group's header is at `region_base`, not word 0, and its section offsets are
    // region-RELATIVE — the vertex programs add the base from the draw table via DrawIndex, which a kernel does
    // not have. So the base is handed to the kernel as data: every header read becomes `base + word` and every
    // section offset READ OUT of the header becomes `base + value`. `kCullNoBaseWord` = the buffer's base IS 0
    // (a private per-group buffer), which keeps the non-consolidated path free of the extra load.
    crd::u32 base_word     = 0xFFFFFFFFU;
    // ── ⭐⭐ REN-40-C2: DISCRETE LOD SELECTION, ON THE DEVICE. ───────────────────────────────────────────────
    // The cull kernel already has everything the decision needs — the instance's world AABB and the view's clip
    // matrix — so choosing a level is a few more lines in the pass that is already reading them, not a new pass.
    // Each (view, slot) gets its OWN indirect command and its OWN compacted list, so one instance contributes to
    // exactly one draw and the whole selection stays a single atomic per survivor.
    //
    // `lod_slots` = levels the layout reserves per view. **1 is the historical behaviour byte for byte**, which
    // is what makes "force everything to level 0 and diff the frame" a real parity gate rather than a hope.
    crd::u32 lod_slots       = 1U;
    // Header words: the chain LENGTH, the (first_index, index_count) TABLE, and the per-level screen HEIGHTS.
    // ⛔ Read from the header rather than baked, because the chain is a property of the MESH and ONE cooked
    // kernel serves every mesh group in the scene. A cook-time constant here would need a kernel per mesh.
    crd::u32 lod_count_word  = 0U;
    crd::u32 lod_table_word  = 0U;
    crd::u32 lod_height_word = 0U;
    // ⭐⭐ THE VIEW'S HEIGHT IN PIXELS, as a word offset inside the ARGS params block (beside `base_word`).
    // ⛔⛔ IT CANNOT BE A COOK-TIME CONSTANT AND IT CANNOT LIVE IN THE HEADER. Not cook-time: the window resizes.
    // Not the header: the header is per GROUP while this is per VIEW, and the camera's viewport and a cascade's
    // atlas slice are different sizes — putting it in the header would have every group publish five numbers that
    // are identical across groups and different across views, i.e. the wrong axis. The params block is already
    // per (group, dispatch) and already carries `base_word`, so it is the one place with the right shape.
    // 0 = no pixel height ⇒ selection is disabled and every instance takes slot 0.
    crd::u32 pixel_height_word = 0U;
    // ⛔⛔ REN-40-C2: the arg offset WITHIN one command, kept separately from `draw_arg_off` (which is the
    // ABSOLUTE word of this dispatch's first arg, params block and view stride folded in). Without it a kernel
    // cannot recover where a COMMAND starts, only where its ARGS start — and the reset needs the command start to
    // write D3D12's leading DrawIndex root constant. It had been writing that constant at `view * stride` with
    // the params block NOT added, so view 0's DrawIndex landed on params word 0 — THE GROUP'S REGION BASE — and
    // every consolidated group's cull then read group 0's header. Vulkan never saw it (its arg offset is 0, so
    // the store is skipped entirely), which is exactly the shape of a backend-specific silent wrong answer.
    crd::u32 draw_arg_within = 0U;
    // ⭐⭐ REN-40-C2 / D3D12: the params-block word holding this group's FIRST DRAW-LIST ROW. D3D12's command
    // signature prepends a DrawIndex root constant, so the reset must WRITE the row into each command; the
    // row of (group, slot) is `base_row + slot`, and the base is per GROUP while one cooked kernel serves
    // every group — so it arrives as data, exactly like `base_word`. 0 = no base row (write slot alone,
    // the historical behaviour).
    crd::u32 base_row_word = 0U;
    // ⭐⭐ REN-40-C2: the header word naming the per-instance LOD-OVERRIDE section (2 words each: the screen
    // bias as f32 bits, then `min_level | (max_level << 8)`). ⛔ A SECTION beside the world AABBs rather than
    // extra words on the instance record, because the instance stride is DECLARED by every `.crdv` and read by
    // every vertex program — only the cull needs this. 0 = no overrides (bias 1, levels unclamped).
    crd::u32 lod_override_off = 0U;
    // ⭐⭐ REN-40-C4: the dither cross-dissolve band, as a fraction of each switch height. When > 0 the kernel
    // packs alpha into the visible entry's high 8 bits (`(alpha_byte << 24) | instance_id`) and DUAL-WRITES
    // inside the band — so both adjacent levels draw and the FS discards pixels via a 4x4 Bayer threshold.
    // 0.0 = no dither, the historical single-write path byte for byte.
    crd::f32 dither_band = 0.0F;
    // ⭐⭐ REN-40-G3: HZB OCCLUSION TEST. When true the kernel declares a sampled texture at binding 8 (the HZB)
    // and samples it at the AABB center's projected UV. Reverse-Z: if the center's z_ndc < HZB value → the object
    // is farther than the farthest visible surface at that pixel → occluded. The result is multiplied into `vis`
    // after the frustum test, before LOD selection and compaction.
    bool     occlusion  = false;
    // ── SCREEN-SIZE CULL (0 = off) — one mechanism, two meanings by view. A CASCADE dispatch drops casters
    // whose CAMERA-projected height is below the threshold (a 2048px atlas cannot resolve their shadow into
    // anything but a flickering dot — UE5's "Min Screen Radius For Shadows"). The CAMERA dispatch drops
    // instances below a ~pixel (sub-pixel geometry contributes nothing but aliasing energy — shimmer, not
    // image). ⛔ The metric is ALWAYS the camera's — relevance is size ON SCREEN — so the kernel reads the
    // camera matrix (`header.view_proj`) and the camera's pixel height (`cam_pixel_height_word`) even when
    // its own frustum is a cascade's.
    crd::f32 caster_min_px = 0.0F;
    // the params-block word holding the CAMERA's pixel height (view 0's), for the screen-size cull above.
    crd::u32 cam_pixel_height_word = 0U;
    // the params-block word holding the HZB's texel dimensions (w at word, h at word+1, f32 bits) — what makes
    // the occlusion test CONSERVATIVE. 0 = dims unknown ⇒ the occlusion test never rejects (accept-all).
    crd::u32 hzb_size_word = 0U;
};
// ⭐ REN-38-F7: the EXPANSION contract of a procedural vertex stage — how a flat VertexIndex decomposes into
// (instance, corner) and where the per-instance record lives. ⛔ The corner table itself is AUTHORED as `ifequal`
// chains over `@corner` (the registry already has them); a canned table here would be a second vocabulary.
struct ExpandDesc
{
    crd::u32 verts_per_instance = 1U;  // instance = vid / N, corner = vid − instance·N
    crd::u32 instance_words     = 0U;  // per-instance record width in words (0 = no instance record)
    crd::u32 instance_off       = 32U; // first instance word (after the draw header)
    // `@category`: visible ⇔ ((header[mask_word] >> ((flags >> 2) & 0xF)) & 1) != 0 — the draw contract's
    // category test, a SCHEME like skinning is, because bit-twiddling is not surface vocabulary.
    bool     has_category       = false;
    crd::u32 category_field     = 0U;  // instance word holding the flags
    crd::u32 category_mask_word = 0U;  // header word holding the visibility mask
};

struct VertexProgramDesc
{
    crd::containers::String                name;
    crd::u32                               schema = kVertexSchemaVersion;
    VertexHeaderMap                        header;
    crd::u32                               vertex_stride = 12U;
    crd::containers::Array<VertexAttrDesc> attrs;
    InstanceLayoutDesc                     instance;
    SkinDesc                               skin;
    MorphDesc                              morph;
    VertexTransform                        transform = VertexTransform::ViewProj;
    crd::u32                               cascade   = 0U; // LightVp only
    crd::containers::Array<VertNodeDesc>   nodes;           // 38-D3 displacement graph
    crd::containers::String                displace;        // the node whose value REPLACES the object position
    crd::containers::Array<VaryingDesc>    varyings;
    // REN-38-F: the STAGE this declaration cooks for, and its per-stage parameters.
    StageKind                              stage = StageKind::Vertex;
    TessDesc                               tess;
    MeshDesc                               mesh;
    TaskDesc                               task;
    RtDesc                                 rt;
    CullDesc                               cull;
    // ── ⭐⭐ REN-38-F7: the PROCEDURAL vertex mode (appended at the END). ──
    // `position = "node:<name>"` names the node whose vec4 IS the clip position: the stage then builds NO vertex
    // pull at all — its inputs are the expansion indices, the instance-record fields and the header words below.
    // This is what lets a screen-space line quad, a corner-table patch VS or a fullscreen triangle pair be a
    // declaration instead of a C++ builder.
    crd::containers::String                position_node;
    // ⭐⭐ REN-38 (scene-buffer consolidation): non-zero = the DRAW-TABLE word offset. The pull VS reads its
    // region base from `sbuf[rebase_table + DrawIndex]` and rebases EVERY storage load by it — one scene
    // buffer, per-group regions, cross-group multi-draw. 0 keeps the historical absolute layout.
    crd::u32                               rebase_table = 0U;
    // ── ⭐⭐ REN-40-C2: THE DRAW TABLE IS A PER-DRAW RECORD, not a bare base. ──────────────────────────────────
    // `rebase_stride` is how many words one row occupies; word 0 stays the region base (every existing reader is
    // unchanged when the stride is 1), word 1 carries this draw's LOD SLOT.
    // ⛔⛔ WHY THE SLOT HAS TO ARRIVE THIS WAY. A frame-graph PASS binds ONE program for its whole draw list, so
    // the slot cannot be a cook-time constant the way the CASCADE is (each cascade already has its own pass and
    // therefore its own program). It has to be PER DRAW ITEM, and the only per-draw channel a GPU-written
    // multi-draw has is the draw index. This is the standard GPU-driven indirection and both backends already
    // carry it: `gl_DrawIDARB` on Vulkan, a command-signature root constant on D3D12 (whose
    // `D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT` is the same idea formalised). Frontier engines call
    // the row a "render item" — an atomic (mesh x material x LOD) unit, one indirect draw each.
    // ⛔ The row stores the SLOT, not the finished list address, because the table is uploaded ONCE and shared by
    // every pass while the list address depends on the VIEW. `cascade` is cook-time per pass, `slot` is per row,
    // and the stage combines them — one table serves the camera and every cascade.
    crd::u32                               rebase_stride = 1U;
    // How many LOD slots the visible-list layout reserves per view. 1 = the historical single-list-per-view
    // layout, byte for byte.
    crd::u32                               lod_slots     = 1U;
    // ⭐⭐ 38-G1 perf: the header word holding the INSTANCE CAPACITY (the stride between the per-cascade
    // visible lists). Non-zero on a LIGHT_VP stage makes it read cascade `cascade`'s own list instead of the
    // camera's — the per-cascade shadow cull. 0 keeps the historical single-list behaviour.
    crd::u32                               instance_capacity_word = 0U;
    ExpandDesc                             expand;
    // ── ⭐⭐ REN-39-B2: the INDEXED pull mode (appended at the END). ──
    // `indexed = true` re-addresses the pull chain for an INDEXED draw (`draw_storage_indexed_depth` /
    // `draw_storage_multi_indexed_depth`): the IA fetched `indices[]` through the bound index section, so
    // VertexIndex ARRIVES as the index value — the per-vertex index load is GONE (`vidx = VertexIndex`) — and
    // the instance rides `InstanceIndex` — the `vid / index_count` division is GONE. Everything downstream
    // (vertex base, visible slot, morphs, skinning, `rebase_table`, per-cascade lists) addresses off the SAME
    // vidx/instance values and composes UNCHANGED. Vertex-record pull stages only: a procedural
    // (`position_node`) or non-Vertex stage declaration with `indexed` is REFUSED (BadIndexed) — their vertex
    // ids are expansion indices, not mesh indices, and an indexed draw would silently mis-decompose them.
    bool indexed = false;
    // ⭐⭐ REN-40-C4: when > 0 AND `lod_slots > 1`, the visible-list entry is packed:
    // `(alpha_byte << 24) | instance_id`. The VS masks the entry and outputs fade alpha as a flat f32 varying
    // at location 4 (Vertex stages only — shadow stages mask but emit no varying).
    crd::f32 dither_band = 0.0F;

    explicit VertexProgramDesc(crd::memory::IAllocator* a)
        : name(a), attrs(a), instance(a), morph(a), nodes(a), displace(a), varyings(a), position_node(a)
    {
    }
};

// Every way a `.crdv` can be wrong. ⛔ One value per distinct mistake: "invalid vertex program" tells an author
// nothing, and every one of these is a mistake that renders rather than failing.
enum class VertexCookError : crd::u8
{
    Ok = 0,
    ParseFailed,
    BadSchema,
    MissingName,
    DuplicateName,     // two attributes, nodes or varyings share a name
    AttrOutOfRecord,   // an attribute reads past the declared stride — pulls the NEXT vertex's data
    BadComponentCount, // comps outside 1..4
    TooManyAttributes,
    TooManyVaryings,   // more than CKIR carries
    DuplicateLocation, // two varyings at one location — one silently wins
    UnknownSource,     // a varying names an attribute/instance field/node that does not exist
    EmptySource,       // a varying with no terms is a zero-width interpolant
    VaryingTooWide,    // the concatenated terms exceed 4 components
    UnknownOp,         // a displacement node names an operation the registry does not have
    WrongArity,
    NodeCycle,       // declaration-order DAG, as in `.crdm`
    AttrNotConstant, // a compile-time argument was wired
    AttrOutOfRange,
    BadSkin, // influences/stride/scheme disagree
    BadMorph,
    BadTransform,      // LightVp with a cascade the header cannot hold
    NoPosition,        // no attribute of kind Position — nothing to project
    ContractMismatch,  // 38-D4: the fragment side asked for a varying this VS does not emit
    NodeWidthMismatch, // a varying's declared node width is not what the displacement graph built
    BadStage,          // a stage name the cooker does not have
    BadTess,           // a patch size or tess level that cannot tessellate
    BadMesh,           // a meshlet budget outside what a mesh pipeline can promise
    BadTask,           // an amplification factor of zero launches nothing
    BadRt,             // a ray payload width the three stages could not agree on
    BadCull,           // a culling workgroup that cannot cover the instance list
    // REN-38-F7 (appended — the value is part of the reporting contract)
    BadExpand,       // an expansion whose indices cannot decompose, or a record term outside it
    BadPositionNode, // `position = "node:…"` names no node, a non-vec4 node, or a non-vertex stage
    // REN-39-B2 (appended)
    BadIndexed, // `indexed = true` on a procedural or non-Vertex stage — their vertex ids are not mesh indices
};

[[nodiscard]] const char* vertex_cook_error_text(VertexCookError err) noexcept;

[[nodiscard]] VertexCookError parse_vertex_toml(crd::containers::StringView toml_text, VertexProgramDesc& out,
                                                crd::containers::String* where = nullptr);

// The same rules on a PROGRAMMATIC description. ⛔ Both provenances are held to the same rules; a check only the
// text path performed would make the ergonomic path the unsafe one.
[[nodiscard]] VertexCookError validate_vertex_program(const VertexProgramDesc& desc,
                                                      crd::containers::String* where = nullptr);

// description → `.crdv`. `parse → emit → parse` must round-trip, or a tool's save silently drops what it did not
// understand — and a dropped attribute is a shader that reads the wrong words.
[[nodiscard]] crd::containers::String emit_vertex_toml(const VertexProgramDesc& desc, crd::memory::IAllocator* a);

// ── ⭐ THE COOK: a validated description → a CKIR vertex entry. ───────────────────────────────────────────────
// Builds the pull path, the (optional) morph + skin, the (optional) displacement graph, the transform and every
// declared varying into `g`/`ve` — the same artifact `build_scene_vs_shadowed` returned, which is what makes this
// a REPLACEMENT rather than a parallel path. False = the description is invalid.
// `shape_issue` (optional) receives the offending node + reason when the REN-38 shape check refuses the built
// entry — a refusal with nothing pointing at the cause is the exact failure mode the check exists to end.
[[nodiscard]] bool cook_vertex_program(const VertexProgramDesc& desc, crd::kir::KGraph& g, crd::kir::KEntry& ve,
                                       crd::kir::ShapeIssue* shape_issue = nullptr);

// ── ⭐⭐ 38-D4: THE VARYING CONTRACT ──────────────────────────────────────────────────────────────────────────
// What a fragment program requires: a name and the location + width it will read it at. ⛔ ALL THREE are checked.
// A name-only check passes a VS that emits `world_normal` as a vec2 at location 3 while the FS reads a vec3 at 0 —
// which links, binds, and shades from whatever is in the neighbouring slot.
struct VaryingRequirement
{
    // ⛔ `name == nullptr` matches by LOCATION instead — the form `fs_varying_requirements` produces, because a
    // cooked fragment graph knows its read locations, widths and interpolation but not the asset-side names.
    const char* name     = nullptr;
    crd::u32    location = 0U;
    crd::u32    comps    = 0U;
    bool        flat     = false;
};

[[nodiscard]] VertexCookError verify_varying_contract(const VertexProgramDesc& desc, const VaryingRequirement* req,
                                                      crd::u32 n_req, crd::containers::String* where = nullptr);

// ── REN-38 audit: the LIVE half of the 38-D4 contract. ──────────────────────────────────────────────────────
// Derive the varying requirements a cooked FRAGMENT entry actually reads — its reachable `StageIn` nodes, one
// requirement per location (nameless, so the contract matches by location). This is what joins a `.crdv` to a
// cooked FS at PROGRAM-CREATION time rather than only in a test: the check runs against the real read set, not
// a hand-maintained list that can drift from it. Returns false when the graph reads one location at two widths
// or interpolations — a fragment program that disagrees with itself.
[[nodiscard]] bool fs_varying_requirements(const crd::kir::KGraph& g, const crd::kir::KEntry& fs,
                                           VaryingRequirement* out, crd::u32 cap, crd::u32* n_out,
                                           crd::memory::IAllocator* alloc);

// The width a declared varying resolves to (the sum of its source terms). Exposed so a consumer can build its
// requirement list from the asset rather than from a duplicated constant.
[[nodiscard]] crd::u32 varying_width(const VertexProgramDesc& desc, crd::u32 varying_index) noexcept;

// The LAYOUT IDENTITY — a content hash over everything that changes what the cooked VS reads or emits.
// ⛔⛔ `VariantKey::vertex` has been a RESERVED FIELD THAT NOTHING FILLED. Two different layouts therefore hashed
// to the same variant key, and the variant cache would hand the second one the FIRST one's program: a dedup
// COLLISION, which `ckir_variant.hpp` names as the failure mode an undeclared axis produces.
[[nodiscard]] crd::u64 vertex_layout_id(const VertexProgramDesc& desc) noexcept;

} // namespace crd::vertcook
