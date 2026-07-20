#pragma once

// ckir.hpp — Phase 3.1.6 v17-a: the CERID KERNEL IR (CKIR), graph level. A typed tensor op-DAG — the tensor-level
// generalization of the v16-h scalar `graph_ad` DAG (same pattern: a DAG of ops, later differentiated symbolically +
// optimized by passes, then lowered to backends). This header is the FOUNDATION: the minimal RISC primitive op set,
// the graph + builder (shape/dtype inferred), and the **deterministic CPU reference interpreter** — the single oracle
// every GPU backend proves against (bit/ulp) and the determinism ground truth. ADR-0098.
//
// The minimal op set (tinygrad/Luminal lesson — small set ⇒ small backends + composable op library + search-based
// tuning): leaves (Input/Const/Iota) · unary crd::math (Neg/Recip/Abs/Exp/Log/Sin/Cos/Sqrt/Tanh) · binary
// (Add/Sub/Mul/Div/Max/Min/CmpLt) · ternary (Select) · reduce (Sum/Max) · movement (Reshape/Permute/Broadcast) ·
// contraction (Contract = batched matmul) · Cast. Indexing (Slice/Pad/Gather/Scatter) lands in v17-b. The op library
// (GEMM/FFT/…) is COMPOSED from these — GEMM = Contract, einsum = Permute+Reshape+Contract, etc.
//
// Determinism: every reduction/contraction accumulates in a FIXED order (ascending index) — no reordering, ever. The
// reference is bit-reproducible run-to-run; matching the GPU fixed-TREE reduction bit-for-bit is the v17-f (T1/T2/T3)
// work. NO std containers; caller-owned allocator throughout.

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <initializer_list>

namespace crd::kir
{

// ADR-0096 dtypes. v17-a computes/rounds F32 + F64 (the reference tier); F16/BF16/int are storage, exact eval in later
// slices. round_dtype below rounds an f64 accumulator to the node's storage precision so the reference matches an
// IEEE f32 GPU kernel.
// Appended, never reordered: a DType is stored in the IR and will be serialized by the Phase-D cook, so an existing
// value's numeric tag must stay put (the vtable-stability discipline, applied to an enum).
enum class DType : crd::u8 { F32, F64, F16, BF16, I32, I64, U8, Bool, U32 };

[[nodiscard]] inline bool is_bool_dtype(DType d) noexcept { return d == DType::Bool; }

// The VALUE FORM of a node's per-element type. A CKIR value is a scalar, a vecN, or a matRxC of `scalar` components
// (column-major: `cols` columns of `rows` elements). Modelled as ONE composed type, the way SPIR-V (OpTypeVector /
// OpTypeMatrix) and Slang do, rather than a loose (dtype, component-count) pair — a component count alone cannot tell
// a vec4 from a mat2, and carries no scalar type, so `ivec3` / `bvec4` / `mat2` are unrepresentable without it.
// B2: `Texture` + `Sampler` are OPAQUE handle kinds (appended, never reordered — a TKind is cook-serialized). A separable
// model (texture ≠ sampler, combined only at the sample site) — the portable choice HLSL/WGSL/MSL all use and the one
// bindless needs. A texture's `scalar` = sampled component type (F32 colour · U32/I32 integer · F32 depth); `rows` = the
// `TexDim`; `cols` = flag bits (bit0 arrayed · bit1 multisampled · bit2 shadow/depth-compare). A sampler's `cols` bit2 =
// comparison (shadow) sampler. See `KType::texture`/`::sampler`.
enum class TKind : crd::u8 { Scalar, Vec, Mat, Struct, Texture, Sampler };

// B2: texture dimensionality (stored in a Texture KType's `rows`). 1D/2D/3D/Cube; arrayed/MS/shadow are the `cols` flags.
enum class TexDim : crd::u8 { Tex1D = 1, Tex2D = 2, Tex3D = 3, TexCube = 4 };

// B2: the Texture `cols` flag bits.
inline constexpr crd::u8 kTexArrayed = 1U << 0U;
inline constexpr crd::u8 kTexMS      = 1U << 1U;
inline constexpr crd::u8 kTexShadow  = 1U << 2U;

struct KType
{
    DType    scalar = DType::F32;
    TKind    kind   = TKind::Scalar;
    crd::u8  rows   = 1; // vec width, or matrix row count
    crd::u8  cols   = 1; // matrix column count (1 for scalar + vec)
    // B0-4 aggregates. `count` is a fixed ARRAY length — array-ness applies to ANY element form, so there is no separate
    // `TKind::Array` (a `vec3[2]` is kind=Vec, rows=3, count=2). `elem_comps` caches the flat scalar count of ONE element
    // for structs, whose real definition lives in the graph's struct registry; its only writer is `KGraph::struct_type()`,
    // so two types with the same `struct_id` always agree and `operator==` stays a plain value compare.
    crd::u16 count      = 1;  // fixed array length (1 = not an array)
    crd::i16 struct_id  = -1; // index into KGraph's struct registry (-1 = not a struct)
    crd::u16 elem_comps = 0;  // flat scalars in one element; 0 => derive as rows*cols

    [[nodiscard]] constexpr int elem_size() const noexcept
    {
        return elem_comps != 0 ? static_cast<int>(elem_comps) : static_cast<int>(rows) * static_cast<int>(cols);
    }
    // flat component count — what the eval buffers and the emitters index elements with.
    [[nodiscard]] constexpr int  comps() const noexcept { return elem_size() * static_cast<int>(count); }
    [[nodiscard]] constexpr bool is_array() const noexcept { return count > 1; }

    [[nodiscard]] constexpr bool operator==(const KType&) const noexcept = default;

    // same form, different component scalar — `cast` and the bit-reinterpret ops (floatBitsToInt / intBitsToFloat).
    [[nodiscard]] constexpr KType with_scalar(DType d) const noexcept { KType t = *this; t.scalar = d; return t; }

    [[nodiscard]] static constexpr KType make_scalar(DType d) noexcept { return KType{d, TKind::Scalar, 1, 1, 1, -1, 0}; }
    // a 1-wide vector IS a scalar — keep one canonical spelling so CSE never sees two types for one value.
    [[nodiscard]] static constexpr KType vec(DType d, int n) noexcept
    {
        return n <= 1 ? make_scalar(d) : KType{d, TKind::Vec, static_cast<crd::u8>(n), 1, 1, -1, 0};
    }
    [[nodiscard]] static constexpr KType mat(DType d, int r, int c) noexcept
    {
        return KType{d, TKind::Mat, static_cast<crd::u8>(r), static_cast<crd::u8>(c), 1, -1, 0};
    }
    // an ARRAY of `n` copies of `elem`. Nested arrays are not representable (there is one `count`); an array of arrays
    // goes through a struct — which is also the only way GLSL/HLSL let you spell it.
    [[nodiscard]] static constexpr KType array_of(KType elem, int n) noexcept
    {
        KType t = elem;
        t.count = static_cast<crd::u16>(n);
        return t;
    }
    // Rebuild the A3 form from a flat component count (1 scalar · 2/3/4 vecN · 9 mat3 · 16 mat4) — the exact mapping the
    // pre-KType emitters hard-coded. `4` is ambiguous (vec4 or mat2) and resolves to the A3 meaning, vec4; callers that
    // mean mat2 construct it explicitly via `mat()`.
    [[nodiscard]] static constexpr KType from_comps(DType d, int n) noexcept
    {
        if (n == 9) { return mat(d, 3, 3); }
        if (n == 16) { return mat(d, 4, 4); }
        return vec(d, n);
    }

    // B2: an OPAQUE texture handle. `sampled` = component type (F32 colour · U32/I32 integer · F32 depth); `dim` in `rows`;
    // arrayed/ms/shadow packed into `cols`. Not a value form — it names a binding, sampled via `KOp::TexSample`.
    [[nodiscard]] static constexpr KType texture(DType sampled, TexDim dim, bool arrayed = false, bool ms = false,
                                                 bool shadow = false) noexcept
    {
        const auto flags = static_cast<crd::u8>((arrayed ? kTexArrayed : 0U) | (ms ? kTexMS : 0U) | (shadow ? kTexShadow : 0U));
        return KType{sampled, TKind::Texture, static_cast<crd::u8>(dim), flags, 1, -1, 0};
    }
    // B2: an OPAQUE sampler handle. `shadow` = a comparison sampler (`samplerShadow` / `SamplerComparisonState`).
    [[nodiscard]] static constexpr KType sampler(bool shadow = false) noexcept
    {
        return KType{DType::F32, TKind::Sampler, 0, static_cast<crd::u8>(shadow ? kTexShadow : 0U), 1, -1, 0};
    }

    [[nodiscard]] constexpr bool is_texture() const noexcept { return kind == TKind::Texture; }
    [[nodiscard]] constexpr bool is_sampler() const noexcept { return kind == TKind::Sampler; }
    [[nodiscard]] constexpr TexDim tex_dim() const noexcept { return static_cast<TexDim>(rows); }
    [[nodiscard]] constexpr bool tex_arrayed() const noexcept { return (cols & kTexArrayed) != 0U; }
    [[nodiscard]] constexpr bool tex_ms() const noexcept { return (cols & kTexMS) != 0U; }
    [[nodiscard]] constexpr bool tex_shadow() const noexcept { return (cols & kTexShadow) != 0U; }
};

enum class KOp : crd::u8
{
    Input, Const, Iota,                                     // leaves
    Neg, Recip, Abs, Exp, Log, Sin, Cos, Sqrt, Tanh, Floor, Ceil, Sign, Trunc, Round, // unary elementwise (crd::math)
    Add, Sub, Mul, Div, Max, Min, CmpLt, CmpEq, CmpLe,      // binary elementwise (same-shape; broadcast is explicit)
    Shl, Shr, BitAnd, BitOr, BitXor,                        // integer bitwise (reinterpret through i64) — morton/radix/hashing
    CmpGt, CmpGe, CmpNe,                                    // comparisons completing Lt/Eq/Le: >, >=, !=
    BitNot, BitCount, FindLSB, FindMSB, BitfieldExtract,    // bit ops: ~ · popcount · lsb/msb index · extract (v>>off)&mask
    Pow, Step, Fract, Clamp, Mix,                           // shader intrinsics: Pow/Step binary · Fract unary · Clamp/Mix ternary
    Rsqrt, Exp2, Log2, Tan, Radians, Degrees, Atan2, Smoothstep, // A2: unary Rsqrt/Exp2/Log2/Tan/Radians/Degrees · binary Atan2 · ternary Smoothstep
    Asin, Acos, Atan, Sinh, Cosh, Cbrt, Mod, Fma,           // A2 tail: unary Asin/Acos/Atan/Sinh/Cosh/Cbrt · binary Mod · ternary Fma(a*b+c)
    Vec2, Vec3, VecComp, Dot, Cross, Normalize, VecLen,     // A3 vector values: build vec2/vec3 · extract comp · dot/cross/normalize/length
    VecConcat, Swizzle,                                     // A3: concat two vecs (→ vec4 etc.) · arbitrary swizzle (.x/.xy/.yzx/.wzyx — reorder+subset)
    MatVecMul, MatMatMul, MatTranspose,                     // A3 matrices (column-major flat: comps 9=mat3 / 16=mat4): mat*vec · mat*mat · transpose
    MatFromCols,                                            // A3: mat3 from 3 vec3 columns (GPU-constructible: emits mat3(c0,c1,c2) — no comps-6 intermediate)
    Splat, Reflect, Refract, Faceforward, VecAny, VecAll,   // A3: scalar→vecN splat · geometric reflect/refract/faceforward · relational any/all
    OuterProduct, Determinant, MatInverse,                  // A3 matrix: outer product (vec⊗vec→mat) · determinant · inverse
    Slerp, QuatMul, QuatConj, QuatRotate, QuatAxisAngle, QuatToMat3, // A3 interp+quats (quat=vec4 x,y,z,w): slerp · Hamilton mul · conj · rotate vec · from axis-angle · to mat3
    For, LoopIndex, LoopAcc,                                // A4 tier-2 dynamic control flow: bounded For loop + its body leaves (index, accumulator)
    BitReverse, Ldexp, FloatBitsToInt, IntBitsToFloat, Modf, // minor gaps: reverse 32 bits · m*2^e · reinterpret f32↔i32 bits · modf→vec2(int,frac)
    StructMake, ArrayMake, FieldGet, ArrayGet,              // B0-4 aggregates (variadic operands in the graph's ext pool)
    StageIn, Builtin, UniformBlock,                         // B3 raster leaves: location-indexed stage input · per-stage builtin · UBO at (set, binding)
    Select,                                                 // ternary: cond ? a : b
    ReduceSum, ReduceMax, ReduceMin, ReduceProd, ArgMax, ArgMin, // reductions over an axis mask (keepdims; Arg* -> index)
    ScanSum,                                                     // inclusive prefix-sum along the trailing axis (keeps shape)
    Reshape, Permute, Broadcast,                            // movement / layout
    Contract,                                               // batched matmul: [...,M,K] x [...,K,N] -> [...,M,N]
    Gather,                                                 // row-gather along axis 0: out[m,...] = data[idx[m],...]
    Scatter,                                                // row-scatter (last-wins): out=base, then out[idx[m],...]=updates[m,...]
    ScatterAdd,                                             // atomic scatter-ADD (histogram): out[M]=0, then out[idx[i]] += updates[i]
    Cast,                                                   // dtype conversion
    DFdx, DFdy, Fwidth,                                     // B1 fragment derivatives: ∂/∂x · ∂/∂y · |dFdx|+|dFdy| — FRAGMENT-ONLY (2×2 quad)
    StorageLoad,                                            // B1-f: read element `a` (uint index) of the FS storage buffer (set 0, binding 0)
    Texture, Sampler, TexSample,                            // B2: opaque texture/sampler binding leaves · sample(a=tex,b=samp,c=uv)
    SampleLod, SampleGrad, SampleCmp, TexelFetch, TexGather, TexSize, // B2-b: sample family — explicit-LOD/grad · shadow-compare · integer fetch · 4-texel gather · size query
    SampleIndexed,                                          // B2-d: BINDLESS sample — index a texture ARRAY (a=texArray, b=samp, c=uv, d=index)
    // ── B-cmp: the imperative COMPUTE-KERNEL layer (workgroup shared memory + barriers + storage buffers) ──────────────────
    // The substrate for hand-authored on-chip kernels (FFT, scan, sort, stencils, conv) composed IN the IR → all backends,
    // instead of the hardcoded per-backend emitter special-cases (tiled GEMM / reduce). Resource leaves + indexed reads;
    // the WRITES + barriers + loops live in the KEntry statement body (KStmt), not the value graph.
    BufferDecl,                                             // a bound global storage buffer: dtype=elem · dset=set · iidx=binding · axes bit0=writable
    SharedDecl,                                             // a workgroup-shared array: dtype=elem · iidx=length · axes=pad (bank-conflict padding, +elems per "row" is caller's index math)
    BufferLoad,                                             // read buffer[idx]: a=BufferDecl node · b=uint index node
    SharedLoad,                                             // read shared[idx]: a=SharedDecl node · b=uint index node
    KernelLoopVar,                                          // the induction value of an enclosing kernel `For` statement (a=For-stmt id) — a leaf the body reads
    // ── B-cmp: SUBGROUP (wave) ops — cross-lane reductions within a fixed-size (32-lane) subgroup. The cheap deterministic rank
    // for radix sort + warp-scan. Bit-exact requires a FIXED subgroup size across backends (32 — NVIDIA-native; forced via
    // subgroupSizeControl elsewhere). a = the per-lane predicate/value node.
    SubgroupBallot,                                         // a (u32, 0/1 predicate) → u32 bitmask of lanes (bit `lane`) whose predicate is nonzero
    SubgroupBallotExclCount,                                // a (u32 ballot mask) → popcount of set bits STRICTLY BELOW this lane (exclusive within-subgroup rank)
    SubgroupMatch,                                          // a (u32 value) → u32 bitmask of lanes whose value EQUALS this lane's (hardware match_any / partition; deterministic ⇒ bit-exact)
    // B16: BINDLESS + EXPLICIT-LOD sample — SampleIndexed with an explicit mip `lod` (in the ext pool). A VERTEX shader has no
    // derivatives, so displacing a grid from bindless cascade textures needs this combo (SampleIndexed is implicit-LOD, SampleLod
    // is non-bindless). a=texArray, b=samp, c=uv, d=index, ext[0]=lod. Appended at END for cook/serialization stability.
    SampleIndexedLod,
    // B17: the VALUE a value-returning atomic produces (the OLD element, before the op) — a placeholder LEAF that the
    // `BufferAtomicAddFetch` / `BufferAtomicExchange` statement MATERIALIZES into a temp exactly once (impure: never inlined,
    // never CSE-merged — each atomic executes once). Downstream reads it via that temp. dtype = the buffer's element type
    // (u32). The scalable A-buffer's node allocator (`atomicAdd(counter,1)`) + linked-list push (`atomicExchange(head,slot)`).
    AtomicResult,
    // B9/RT-1: HARDWARE RAY TRACING — inline ray query (VK_KHR_ray_query / DXR-1.1 inline). `AccelStructDecl` is an opaque
    // acceleration-structure (TLAS) resource leaf bound at (dset=set, iidx=binding) — the peer of BufferDecl for the RT world.
    // `RayHitResult` is the closest-hit distance `t` a `TraceRayClosest` statement MATERIALIZES into a temp (impure, once —
    // exactly like AtomicResult): downstream reads it as `t<result>`; `hit = t < tmax`. dtype F32. Both APPENDED at END
    // (cook/serialization stability). The FULL RT-pipeline stages (raygen/closest-hit/miss + SBT) + primId/barycentric/normal
    // hit attributes land in RT-2; RT-1 is the inline visibility/AO/shadow leaf (also the B14 ReSTIR ray-visibility leaf).
    AccelStructDecl,
    RayHitResult,
    // FA-2 (portable RT pipeline): the ray PAYLOAD carried across raygen ↔ closest-hit/miss. `RayPayloadDecl` declares an N-float
    // payload struct (iidx = component count) — `rayPayloadEXT`/`rayPayloadInEXT` in GLSL, an `inout` struct param in DXR HLSL.
    // `PayloadLoad` reads component `iidx` of the payload node `a`. Written by `PayloadStore`. Appended at END (cook-stable).
    RayPayloadDecl,
    PayloadLoad
};

// B1: fragment derivatives are the only ops that read NEIGHBOURING invocations (the 2×2 pixel quad), so they are legal
// ONLY in a fragment stage and have no single-invocation meaning (the CPU oracle evaluates them to 0). `entry_valid`
// rejects them in any other stage; the compute emitters never lower them (their `default:` refuses).
[[nodiscard]] inline bool is_fragment_only_op(KOp op) noexcept
{
    return op == KOp::DFdx || op == KOp::DFdy || op == KOp::Fwidth;
}

// Comparisons yield a BOOLEAN, not a numeric 0/1 — `bool` for scalar operands, `bvecN` componentwise for vectors, the
// way GLSL / HLSL / SPIR-V all define them. (Emitting `? 1.0 : 0.0` is a LOWERING choice a backend may still make; it
// is not the IR's type.) A `Select` accepts either a Bool condition or a numeric one — bit-extraction produces numeric
// flags (radix/morton), and forcing those through a cast would buy nothing.
// B0-4 aggregate ops. `StructMake`/`ArrayMake` carry VARIADIC operands (the ext pool); `FieldGet`/`ArrayGet` slice one
// field/element out. Every operand walk must handle the variadic pair; every emitter must route these to the value path.
[[nodiscard]] inline bool is_aggregate(KOp op) noexcept
{
    return op == KOp::StructMake || op == KOp::ArrayMake || op == KOp::FieldGet || op == KOp::ArrayGet;
}

// ── B3: the STAGE model (ADR-0101 "one core, two profiles" · ADR-0103 "gpu-context owns every GPU program") ───────────
// CKIR was compute-only: every emitter hardcoded a compute entry point. A stage is now explicit, because `dFdx`,
// `discard` and frag-depth (B1) are fragment constructs, materials (B5..B8) are vertex+fragment programs, and B4/B9 add
// mesh-shading and ray tracing.
//
// The enum is COMPLETE from day one — all 14 SPIR-V execution models — even though most stages have no emitter yet.
// Reference: `docs/systems/shader-ir-corpus-and-stages.md` §2 (mesh-first; geometry supported-but-discouraged).
// **Declaring only the stages we can emit today would bake a 3-stage assumption into every `switch` that consumes this
// enum, and each such `switch` is a silent `default:` waiting to mis-lower a mesh shader** — the same class of defect as
// `MatFromCols` hardcoding three columns (B0-2) and `KOp::Cast` hitting `default: return false` in three emitters
// (B0-4). A backend that cannot yet lower a stage must REFUSE LOUDLY; it must never fall back to compute.
//
// Ordered by pipeline position, not by SPIR-V's numbering. Once D1 serializes a `KEntry` this numbering FREEZES and the
// enum becomes append-only (the `DType::U32` rule).
enum class KStage : crd::u8
{
    Compute = 0,
    // classic raster — always available
    Vertex,
    TessControl, // hull
    TessEval,    // domain
    Geometry,    // LEGACY: single-thread amplification bottleneck. Supported for ports; never build a pipeline on it.
    Fragment,
    // modern raster — the amplification path (DX12 2019 / VK_EXT_mesh_shader 2022)
    Task, // amplification
    Mesh,
    // ray tracing — DXR / VK_KHR_ray_tracing_pipeline
    RayGen,
    Intersection,
    AnyHit,
    ClosestHit,
    Miss,
    Callable,
};
constexpr int kStageCount = 14;

[[nodiscard]] constexpr crd::u32 stage_bit(KStage s) noexcept { return 1U << static_cast<crd::u32>(s); }

// Stage-set masks. A constants namespace (the `crd::gpu::compute_usage` pattern), so `stage_mask::workgroup` reads as
// prose at the use site.
namespace stage_mask
{
constexpr crd::u32 kCompute     = stage_bit(KStage::Compute);
constexpr crd::u32 kVertex      = stage_bit(KStage::Vertex);
constexpr crd::u32 kTessControl = stage_bit(KStage::TessControl);
constexpr crd::u32 kTessEval    = stage_bit(KStage::TessEval);
constexpr crd::u32 kGeometry    = stage_bit(KStage::Geometry);
constexpr crd::u32 kFragment    = stage_bit(KStage::Fragment);
constexpr crd::u32 kTask        = stage_bit(KStage::Task);
constexpr crd::u32 kMesh        = stage_bit(KStage::Mesh);
constexpr crd::u32 kRayGen      = stage_bit(KStage::RayGen);
constexpr crd::u32 kIntersection = stage_bit(KStage::Intersection);
constexpr crd::u32 kAnyHit      = stage_bit(KStage::AnyHit);
constexpr crd::u32 kClosestHit  = stage_bit(KStage::ClosestHit);
constexpr crd::u32 kMiss        = stage_bit(KStage::Miss);
constexpr crd::u32 kCallable    = stage_bit(KStage::Callable);

// Stages dispatched as a workgroup grid — they share the compute builtin family verbatim.
constexpr crd::u32 kWorkgroup = kCompute | kTask | kMesh;
// Every ray-tracing stage.
constexpr crd::u32 kRayAny = kRayGen | kIntersection | kAnyHit | kClosestHit | kMiss | kCallable;
// RT stages that have an INCOMING ray (raygen has not traced yet; callable carries no ray).
constexpr crd::u32 kRayIncoming = kIntersection | kAnyHit | kClosestHit | kMiss;
// RT stages positioned at a hit — object space, instance data, and the object<->world transforms exist.
constexpr crd::u32 kRayHit = kIntersection | kAnyHit | kClosestHit;
} // namespace stage_mask

[[nodiscard]] constexpr bool is_raster_stage(KStage s) noexcept
{
    return (stage_bit(s)
            & (stage_mask::kVertex | stage_mask::kTessControl | stage_mask::kTessEval | stage_mask::kGeometry
               | stage_mask::kFragment | stage_mask::kTask | stage_mask::kMesh))
           != 0U;
}
[[nodiscard]] constexpr bool is_ray_tracing_stage(KStage s) noexcept
{
    return (stage_bit(s) & stage_mask::kRayAny) != 0U;
}

// Which stages emit the clip-space position. Fragment consumes it (as `FragCoord`); it never writes it.
[[nodiscard]] constexpr bool stage_writes_position(KStage s) noexcept
{
    return (stage_bit(s) & (stage_mask::kVertex | stage_mask::kTessEval | stage_mask::kGeometry | stage_mask::kMesh))
           != 0U;
}
[[nodiscard]] constexpr bool stage_writes_frag_depth(KStage s) noexcept { return s == KStage::Fragment; }

[[nodiscard]] inline const char* stage_name(KStage s) noexcept
{
    switch (s)
    {
    case KStage::Compute: return "Compute";
    case KStage::Vertex: return "Vertex";
    case KStage::TessControl: return "TessControl";
    case KStage::TessEval: return "TessEval";
    case KStage::Geometry: return "Geometry";
    case KStage::Fragment: return "Fragment";
    case KStage::Task: return "Task";
    case KStage::Mesh: return "Mesh";
    case KStage::RayGen: return "RayGen";
    case KStage::Intersection: return "Intersection";
    case KStage::AnyHit: return "AnyHit";
    case KStage::ClosestHit: return "ClosestHit";
    case KStage::Miss: return "Miss";
    case KStage::Callable: return "Callable";
    }
    return "?";
}

// Per-stage builtin INPUTS. Stage OUTPUTS (clip position, frag depth) are named by `KEntry`, not read as values.
// Each builtin carries ONE type and ONE set of stages in which it may be read — both in `builtin_info` below, so the
// type and the legality can never drift apart. GLSL/HLSL/MSL spellings belong to the emitters, not here.
enum class KBuiltin : crd::u8
{
    // workgroup family — Compute / Task / Mesh
    GlobalInvocationId,   // uvec3
    LocalInvocationId,    // uvec3
    WorkgroupId,          // uvec3
    NumWorkgroups,        // uvec3
    LocalInvocationIndex, // uint
    // vertex
    VertexIndex,   // int — gl_VertexIndex   / SV_VertexID
    InstanceIndex, // int — gl_InstanceIndex / SV_InstanceID
    // tessellation + geometry
    InvocationId,     // int  — gl_InvocationID (TCS, and the instanced-GS invocation)
    PatchVertexCount, // int  — gl_PatchVerticesIn
    TessCoord,        // vec3 — gl_TessCoord (TES)
    PrimitiveId,      // int  — gl_PrimitiveID / gl_PrimitiveIDIn; also readable at an RT hit
    // fragment
    FragCoord,   // vec4 — gl_FragCoord / SV_Position
    FrontFacing, // bool — gl_FrontFacing / SV_IsFrontFace
    PointCoord,  // vec2
    SampleId,    // int  — per-sample shading
    Layer,       // int  — the layer a GS/mesh stage routed this primitive to
    // ray tracing
    LaunchId,           // uvec3 — gl_LaunchIDEXT   (all RT stages)
    LaunchSize,         // uvec3 — gl_LaunchSizeEXT (all RT stages)
    WorldRayOrigin,     // vec3
    WorldRayDirection,  // vec3
    ObjectRayOrigin,    // vec3
    ObjectRayDirection, // vec3
    RayTmin,            // float
    RayTmax,            // float
    HitT,               // float — gl_HitTEXT (any-hit / closest-hit)
    HitKind,            // uint
    RayFlags,           // uint  — gl_IncomingRayFlagsEXT
    InstanceId,         // int
    InstanceCustomIndex,// int
    GeometryIndex,      // int
    ObjectToWorld,      // mat4x3 — 4 columns of 3 rows
    WorldToObject,      // mat4x3
    // B1-f: appended at END (enum values are cook-serialized — never insert mid-enum).
    InnerCoverage, // uint — 1 iff the pixel is FULLY inside the primitive (SV_InnerCoverage / gl_FragFullyCoveredNV under
                   // conservative rasterization)
    // B-cmp Phase 1: the FLATTENED 1-D workgroup index (gl_WorkGroupID.x / SV_GroupID.x / blockIdx.x) — a scalar uint, so a
    // BATCHED compute kernel (one workgroup per independent problem, e.g. one FFT per grid slot) offsets its global buffers.
    WorkgroupIndex, // uint
    // P4 (any-hit alpha): the hit's triangle BARYCENTRICS (vec2 u,v; w=1-u-v) — `hitAttributeEXT vec2` in GLSL /
    // `BuiltInTriangleIntersectionAttributes.barycentrics` in DXR HLSL. Read via VecComp to alpha-test a sub-triangle region.
    HitBary,
    // B4 task/amplification: the single-uint PAYLOAD a task shader wrote, read in the MESH stage — the GPU-driven data channel
    // from a task workgroup to the mesh workgroups it launched (glsl `taskPayloadSharedEXT` / HLSL `in payload`).
    TaskPayload, // uint — read in the MESH stage (the value the preceding Task entry's `task_payload` node computed)
    // B4-tess: the interpolated patch position in the TESS-EVAL (domain) stage — the emitter bilinearly interpolates the 4
    // control points by gl_TessCoord.xy, so a TES value graph adds a displacement to it (the portable ocean/heightfield path).
    TessPatchPosition, // vec4 — read in the TessEval stage (bilerp of the quad patch's 4 corners by TessCoord)
    // B4: the additional task→mesh PAYLOAD fields (v1..v3) — a meshlet amplification pass passes richer per-meshlet data
    // (cluster bounds / LOD / material id / flags), not just one uint. Distinct builtins ⇒ distinct CSE ⇒ no field-index
    // aliasing. The payload struct is a FIXED 4 uints so the task's + mesh's `taskPayloadSharedEXT` layouts always match.
    TaskPayload1, // uint — task→mesh payload field 1
    TaskPayload2, // uint — task→mesh payload field 2
    TaskPayload3, // uint — task→mesh payload field 3
};

struct KBuiltinInfo
{
    KType       type;
    crd::u32    stages; // bitmask of `stage_bit(...)` — the stages in which reading this builtin is legal
    const char* name;   // diagnostics only; each emitter owns its own spelling
};

[[nodiscard]] inline KBuiltinInfo builtin_info(KBuiltin b) noexcept
{
    const KType t_uvec3 = KType::vec(DType::U32, 3);
    const KType t_vec3  = KType::vec(DType::F32, 3);
    const KType t_uint  = KType::make_scalar(DType::U32);
    const KType t_int   = KType::make_scalar(DType::I32);
    const KType t_float = KType::make_scalar(DType::F32);

    switch (b)
    {
    case KBuiltin::GlobalInvocationId:   return {t_uvec3, stage_mask::kWorkgroup, "GlobalInvocationId"};
    case KBuiltin::LocalInvocationId:    return {t_uvec3, stage_mask::kWorkgroup, "LocalInvocationId"};
    case KBuiltin::WorkgroupId:          return {t_uvec3, stage_mask::kWorkgroup, "WorkgroupId"};
    case KBuiltin::NumWorkgroups:        return {t_uvec3, stage_mask::kWorkgroup, "NumWorkgroups"};
    case KBuiltin::LocalInvocationIndex: return {t_uint, stage_mask::kWorkgroup, "LocalInvocationIndex"};
    case KBuiltin::WorkgroupIndex:       return {t_uint, stage_mask::kWorkgroup, "WorkgroupIndex"}; // B-cmp: flattened 1-D wg id
    case KBuiltin::TaskPayload:          return {t_uint, stage_mask::kMesh, "TaskPayload"};          // B4: task→mesh payload (uint)
    case KBuiltin::TaskPayload1:         return {t_uint, stage_mask::kMesh, "TaskPayload1"};         // B4: payload field 1
    case KBuiltin::TaskPayload2:         return {t_uint, stage_mask::kMesh, "TaskPayload2"};         // B4: payload field 2
    case KBuiltin::TaskPayload3:         return {t_uint, stage_mask::kMesh, "TaskPayload3"};         // B4: payload field 3
    case KBuiltin::TessPatchPosition:    return {KType::vec(DType::F32, 4), stage_mask::kTessEval, "TessPatchPosition"}; // B4-tess

    case KBuiltin::VertexIndex:   return {t_int, stage_mask::kVertex, "VertexIndex"};
    case KBuiltin::InstanceIndex: return {t_int, stage_mask::kVertex, "InstanceIndex"};

    case KBuiltin::InvocationId:     return {t_int, stage_mask::kTessControl | stage_mask::kGeometry, "InvocationId"};
    case KBuiltin::PatchVertexCount: return {t_int, stage_mask::kTessControl | stage_mask::kTessEval, "PatchVertexCount"};
    case KBuiltin::TessCoord:        return {t_vec3, stage_mask::kTessEval, "TessCoord"};
    case KBuiltin::PrimitiveId:
        return {t_int,
                stage_mask::kTessControl | stage_mask::kTessEval | stage_mask::kGeometry | stage_mask::kFragment
                    | stage_mask::kRayHit,
                "PrimitiveId"};

    case KBuiltin::FragCoord:   return {KType::vec(DType::F32, 4), stage_mask::kFragment, "FragCoord"};
    case KBuiltin::FrontFacing: return {KType::make_scalar(DType::Bool), stage_mask::kFragment, "FrontFacing"};
    case KBuiltin::PointCoord:  return {KType::vec(DType::F32, 2), stage_mask::kFragment, "PointCoord"};
    case KBuiltin::SampleId:    return {t_int, stage_mask::kFragment, "SampleId"};
    case KBuiltin::Layer:       return {t_int, stage_mask::kFragment, "Layer"};
    case KBuiltin::InnerCoverage: return {t_uint, stage_mask::kFragment, "InnerCoverage"}; // B1-f

    case KBuiltin::LaunchId:            return {t_uvec3, stage_mask::kRayAny, "LaunchId"};
    case KBuiltin::LaunchSize:          return {t_uvec3, stage_mask::kRayAny, "LaunchSize"};
    case KBuiltin::WorldRayOrigin:      return {t_vec3, stage_mask::kRayIncoming, "WorldRayOrigin"};
    case KBuiltin::WorldRayDirection:   return {t_vec3, stage_mask::kRayIncoming, "WorldRayDirection"};
    case KBuiltin::ObjectRayOrigin:     return {t_vec3, stage_mask::kRayHit, "ObjectRayOrigin"};
    case KBuiltin::ObjectRayDirection:  return {t_vec3, stage_mask::kRayHit, "ObjectRayDirection"};
    case KBuiltin::RayTmin:             return {t_float, stage_mask::kRayIncoming, "RayTmin"};
    case KBuiltin::RayTmax:             return {t_float, stage_mask::kRayIncoming, "RayTmax"};
    case KBuiltin::HitT:                return {t_float, stage_mask::kAnyHit | stage_mask::kClosestHit, "HitT"};
    case KBuiltin::HitKind:             return {t_uint, stage_mask::kAnyHit | stage_mask::kClosestHit, "HitKind"};
    case KBuiltin::RayFlags:            return {t_uint, stage_mask::kRayIncoming, "RayFlags"};
    case KBuiltin::InstanceId:          return {t_int, stage_mask::kRayHit, "InstanceId"};
    case KBuiltin::InstanceCustomIndex: return {t_int, stage_mask::kRayHit, "InstanceCustomIndex"};
    case KBuiltin::GeometryIndex:       return {t_int, stage_mask::kRayHit, "GeometryIndex"};
    case KBuiltin::ObjectToWorld:       return {KType::mat(DType::F32, 3, 4), stage_mask::kRayHit, "ObjectToWorld"};
    case KBuiltin::WorldToObject:       return {KType::mat(DType::F32, 3, 4), stage_mask::kRayHit, "WorldToObject"};
    // ⛔ HitBary had NO case here, so builtin_info() fell off the end of the switch and returned an indeterminate KType — the
    //    emitters happened to special-case the name (GLSL `hattr`, HLSL `attr.barycentrics`) so nothing visibly broke, but any
    //    consumer that asked for its TYPE read garbage. It is the triangle barycentric hit attribute: vec2 (u,v), hit stages.
    case KBuiltin::HitBary:             return {KType::vec(DType::F32, 2), stage_mask::kAnyHit | stage_mask::kClosestHit, "HitBary"};
    }
    return {KType::make_scalar(DType::F32), 0U, "?"};
}

[[nodiscard]] inline KType builtin_type(KBuiltin b) noexcept { return builtin_info(b).type; }
[[nodiscard]] inline bool  builtin_allowed_in(KBuiltin b, KStage s) noexcept
{
    return (builtin_info(b).stages & stage_bit(s)) != 0U;
}

// The raster leaves. `StageIn` is location-indexed and serves BOTH a vertex attribute and a fragment interpolant —
// the entry point's stage disambiguates them, exactly as SPIR-V models it (one `Input` storage class per location).
[[nodiscard]] inline bool is_stage_leaf(KOp op) noexcept
{
    return op == KOp::StageIn || op == KOp::Builtin || op == KOp::UniformBlock;
}

// B1-c: how a VS→FS interpolant is interpolated across the primitive. `Smooth` = perspective-correct (the default);
// `Flat` = no interpolation (provoking-vertex value) — MANDATORY for integer interpolants (GLSL rejects a smooth int);
// `NoPerspective` = linear in screen space; `Centroid`/`Sample` = MSAA sampling controls. The VS OUTPUT and its matching
// FS INPUT must carry the SAME qualifier (like the location must match) — the author's responsibility, as in a real pipeline.
enum class Interp : crd::u8
{
    Smooth = 0,
    Flat,
    NoPerspective,
    Centroid,
    Sample,
};

// An integer interpolant CANNOT be smoothly interpolated — it must be `flat`. Enforced in `entry_valid` for both the VS
// output and the FS input, so an integer varying that forgot `flat` is a loud IR error, not a backend compile failure.
[[nodiscard]] inline bool requires_flat_interp(KType t) noexcept
{
    return t.scalar == DType::I32 || t.scalar == DType::I64 || t.scalar == DType::U32;
}

// B1-d: CONSERVATIVE DEPTH — a promise about how the fragment shader's `gl_FragDepth`/`SV_Depth` write relates to the
// interpolated primitive depth, which lets the hardware keep EARLY depth testing even though the shader writes depth.
// `Any` = the write can move depth anywhere (the default; DEFEATS early-Z). `Greater`/`Less` = the shader only ever makes
// the depth larger/smaller than the primitive's, so an early GreaterEqual/LessEqual test is still conservatively correct.
// SPIR-V DepthGreater/DepthLess · GLSL `layout(depth_greater/less)` · HLSL `SV_DepthGreaterEqual`/`SV_DepthLessEqual`.
enum class DepthMode : crd::u8
{
    Any = 0,
    Greater,
    Less,
};

// A stage ENTRY POINT over a graph: which node feeds each output. Position-writing stages must set `position` (the
// clip-space vec4); fragment entries write colour attachments and may set `frag_depth`. Interpolants (VS out -> FS in)
// and colour attachments are both location-indexed, so one table serves both.
constexpr int kMaxStageOutputs = 8;

struct KStageOutput
{
    int    node     = -1;
    int    location = 0;
    Interp interp   = Interp::Smooth; // B1-c: VS interpolant interpolation mode (ignored for a fragment colour attachment)
};

// ── B-cmp: the imperative compute-kernel STATEMENT model ─────────────────────────────────────────────────────────────────
// A compute KEntry's kernel is an ordered list of EFFECT statements (writes + barriers + loops/ifs), distinct from the pure
// value graph (which computes the RHS values + indices). Stored in a flat pool on KGraph; For/If carry a nested [begin,count).
enum class BarrierScope : crd::u8
{
    Workgroup = 0, // control + shared-memory barrier: all invocations sync + shared writes become visible (GLSL barrier()+groupMemoryBarrier)
    Buffer         // control + BUFFER-memory barrier: storage-buffer writes become visible across the workgroup
};
enum class KStmtKind : crd::u8
{
    BufferStore, // buffer[index] = value    (target = BufferDecl node)
    SharedStore, // shared[index] = value    (target = SharedDecl node)
    Barrier,     // workgroup sync (scope)
    For,         // for (loop_var = 0; loop_var < count; ++loop_var) { body }  — count = value node; loop_var read via KernelLoopVar(a=this stmt id)
    If,          // if (cond) { body }       — cond = a Bool value node
    Materialize, // FREEZE `value` into a per-thread register NOW (`precise float t<value> = <expr>;`) — subsequent reads of
                 // that node return the frozen snapshot, NOT a fresh re-read. Lets a shared value survive a shared OVERWRITE
                 // (register-residency / single-buffer time-multiplexed exchange, cuFFT's 4.2 KB-shared trick). `value` = the
                 // node to freeze; must be a FLOAT value. The oracle caches it per (node, thread); emitters emit one temp.
    SpinUntilNonzero, // SPIN-WAIT: block until a COHERENT global flag buffer[index] becomes nonzero (`while(buf[idx]==0){...}`).
                 // The inter-block sync for a SINGLE-PASS scan/chained primitive — one block waits for its predecessor to
                 // PUBLISH. target = the coherent BufferDecl, index = the element. The oracle runs workgroups SEQUENTIALLY so
                 // the predecessor already published ⇒ this is a no-op there (verifies nonzero); on the GPU it genuinely spins.
    SharedAtomicAdd, // ATOMIC ADD to a SHARED array element: `atomicAdd(sh[index], value)` (a histogram bin count, etc.). target =
                 // SharedDecl, index, value. Bit-exact because the RESULT is a SUM (order-independent) — the oracle accumulates
                 // every active thread's contribution to the bin; emitters emit the backend atomic. U32 shared only.
    BufferAtomicAdd, // ATOMIC ADD to a GLOBAL buffer element: `atomicAdd(buf[index], value)` — a device-wide histogram/counter.
                 // target = BufferDecl, index, value. Bit-exact (SUM is order-independent). U32 buffers only.
    ForBreakIf,  // per-thread BREAK out of the INNERMOST kernel For when `value` ≠ 0 (`if (cond) break;`) — the decoupled-lookback
                 // early-exit. MUST be a direct child of its For (not nested under a divergent If inside the loop): the oracle
                 // removes breaking threads from the loop's active set for the REMAINING iterations.
    BufferTicket, // BLOCK-scoped atomic ticket: thread 0 does `sh[0] = atomicAdd(&buf[index], 1)` — the block's DYNAMIC position.
                 // Onesweep's forward-progress guarantee: resident blocks always hold the LOWEST unprocessed ids, so a spinning
                 // block's predecessor is always resident or retired (blockIdx launch order is NOT guaranteed — using it raw
                 // DEADLOCKED on Ada). target = coherent BufferDecl, index = counter cell, value = the SharedDecl (slot 0).
                 // Oracle: executes once per workgroup (sequential ⇒ ticket == WorkgroupIndex).
    SyncWarp,    // WARP-scoped barrier + shared-memory visibility WITHIN the 32-lane subgroup — the warp-synchronous rank's
                 // ~2-cycle sync (vs a full block Barrier). CUDA `__syncwarp()`, GLSL `subgroupBarrier()`, MSL
                 // `simdgroup_barrier`; HLSL/WGSL lower to the conservative block barrier (uniform flow required anyway).
                 // Oracle: commits pending writes (lockstep interpreter ⇒ same semantics as Barrier).
    BufferAtomicMin, // ATOMIC MIN on a GLOBAL buffer element: `atomicMin(buf[index], value)` — the B4-vis software-rasterizer
                 // visibility key (a packed `(depth << idBits) | triangleId` u32; nearest triangle wins, ties broken by lowest
                 // id). target = BufferDecl, index, value. Bit-exact (MIN is order-independent, like the SUM atomics). U32 only.
                 // APPENDED at END (KStmtKind participates in cook-serialized kernel stmts — never renumber existing kinds).
    BufferAtomicAddFetch, // VALUE-RETURNING atomic add: `t<result> = atomicAdd(buf[index], value)` — materializes the OLD
                 // element into the `result` (AtomicResult) node's temp. target = BufferDecl, index, value, result = the node.
                 // NOT order-independent (the returned slot depends on execution order) ⇒ validate the DETERMINISTIC downstream
                 // (the sorted A-buffer resolve), not this directly. U32. The linked-list node allocator.
    BufferAtomicExchange, // VALUE-RETURNING atomic exchange: `t<result> = atomicExchange(buf[index], value)` — swaps `value`
                 // into the element, materializes the OLD value into `result`. The linked-list head-pointer push. Same fields.
    TraceRayClosest, // B9/RT-1: INLINE RAY QUERY — cast a ray at the bound TLAS and materialize the CLOSEST-HIT distance `t`
                 // (or `tmax` on miss) into `result` (a RayHitResult node). target = AccelStructDecl node · result = the F32
                 // RayHitResult · ext[0..7] = the ray scalars (ox,oy,oz,dx,dy,dz,tmin,tmax). Emits a `rayQueryEXT` block
                 // (GL_EXT_ray_query) / inline `RayQuery<>` (HLSL). Deterministic given ray+geometry ⇒ the CPU oracle
                 // brute-forces watertight ray-triangle over the AS's geometry buffer; validate GPU≈oracle within a geometric
                 // tolerance (RT traversal is NOT bit-exact across vendors). APPENDED at END (cook-stable).
    TraceRayHit,  // B9/RT-2 (reflections/shading): same trace, but ALSO materialize the hit's PRIMITIVE INDEX (which triangle)
                 // so the shader can fetch + shade the hit. Same fields as TraceRayClosest PLUS ext[8] = the primId RayHitResult
                 // node (dtype U32; = 0xFFFFFFFF on miss). Emits `rayQueryGetIntersectionPrimitiveIndexEXT` alongside the t.
    // FA-2 (portable RT PIPELINE — raygen stage): traceRayEXT/TraceRay that INVOKES the hit/miss shaders and fills a payload.
    // target = AccelStructDecl · value = the RayPayloadDecl node · ext[0..7] = ox,oy,oz,dx,dy,dz,tmin,tmax. Appended at END.
    TraceRayPipeline,
    PayloadStore,   // payload component write (hit/miss shaders): target = RayPayloadDecl node · index = component · value = value node
    ReorderThread,  // FA-2 SER: reorderThreadNV / MaybeReorderThread — a no-op where unsupported (perf only). No operands.
    IgnoreHitIf     // P4 (any-hit ALPHA test): `if (cond) ignoreIntersectionEXT()` / `if (cond) IgnoreHit()` — the portable
                    // OMM fallback (alpha-tested geometry in a shader). value = the BOOL condition node.
};
struct KStmt
{
    KStmtKind kind       = KStmtKind::Barrier;
    crd::i32  target     = -1; // BufferStore/SharedStore: the resource (BufferDecl/SharedDecl) node
    crd::i32  index      = -1; // BufferStore/SharedStore: the index node
    crd::i32  value      = -1; // *Store: value node · For: count node · If: cond node
    BarrierScope scope   = BarrierScope::Workgroup; // Barrier
    crd::i32  body_begin = -1; // For/If: nested statement range into KGraph's stmt pool
    crd::i32  body_count = 0;
    crd::i32  result     = -1; // B17: BufferAtomicAddFetch/BufferAtomicExchange — the AtomicResult node materialized with the OLD value
    crd::i32  ext        = -1; // B9/RT: offset into the graph's ext pool for a statement's VARIADIC scalar operands (TraceRayClosest = ox,oy,oz,dx,dy,dz,tmin,tmax)
    crd::u16  n_ext      = 0;  // ...count of them
};

struct KEntry
{
    KStage       stage        = KStage::Fragment;
    int          position     = -1; // clip-space vec4. Required iff `stage_writes_position(stage)`.
    int          frag_depth   = -1; // Fragment: optional explicit depth (defeats early-Z unless `depth_mode` narrows it).
    int          discard_cond = -1; // Fragment (B1-b): `if (cond) discard;` — alpha-test / cutout. A BOOL node; <0 = none.
    // B1-d: force the depth/stencil test to run BEFORE the fragment shader (`layout(early_fragment_tests) in;` /
    // `[earlydepthstencil]`). Contradicts a `frag_depth` write (the early test uses the interpolated depth), so
    // `entry_valid` refuses the combination.
    bool         early_fragment_tests = false;
    DepthMode    depth_mode           = DepthMode::Any; // B1-d: conservative-depth promise for a `frag_depth` write
    // B1-e: per-PRIMITIVE variable-rate-shading output — a position-writing (vertex) stage may emit a packed shading rate
    // (`gl_PrimitiveShadingRateEXT` / `SV_ShadingRate`). The packing is `(Yshift << 2) | Xshift` with shift 0=1×,1=2×,2=4×
    // (so 2×2 = 5) — identical on Vulkan and D3D12. An int/uint node; <0 = no per-primitive rate.
    int          shading_rate = -1;
    // B1-f: a fragment STORAGE-buffer write side-effect — `storage[storage_write_index] = storage_write_value` (both uint
    // nodes; <0 = no write). `interlock` makes the fragment's storage access RASTERIZER-ORDERED (GLSL fragment-shader
    // interlock / HLSL ROV) — the substrate for OIT / voxelization.
    int          storage_write_index = -1;
    int          storage_write_value = -1;
    bool         interlock           = false;
    int          n_out        = 0;
    KStageOutput out[kMaxStageOutputs] = {};
    // ── B-cmp: an imperative COMPUTE kernel. When `kernel_body_count > 0` (Compute stage only), this entry is a hand-authored
    // workgroup kernel — `local_size` threads per workgroup running the statement body [kernel_body_begin, +count) in the
    // KGraph stmt pool — NOT the functional map/reduce path. n_out/out are unused for a kernel (its outputs are BufferStores).
    crd::u32     local_size[3]      = {1, 1, 1};
    int          kernel_body_begin  = -1;
    int          kernel_body_count  = 0;
    // ── B4: MESH shader (stage == Mesh). Emits up to `mesh_vertices` vertices + `mesh_primitives` triangles per workgroup;
    // max(mesh_vertices, mesh_primitives) threads cooperate — thread `tid` writes vertex tid (`position` + `out[]`) when
    // tid < mesh_vertices, and primitive tid (`mesh_prim` → a uvec3 of LOCAL vertex indices) when tid < mesh_primitives. The
    // per-vertex `position`/`out[]` graphs read the workgroup builtins (typically a GLOBAL vertex id = WorkgroupIndex·
    // mesh_vertices + LocalInvocationIndex). Dispatched over a workgroup grid (draw_mesh). Portable core; WGSL falls back to
    // the vertex-pull path (WebGPU has no mesh shaders). Fields appended at END (KEntry is cook-serialized).
    crd::u32     mesh_vertices      = 0; // max_vertices (0 ⇒ not a mesh entry)
    crd::u32     mesh_primitives    = 0; // max_primitives (triangles)
    int          mesh_prim          = -1; // uvec3 node: primitive `LocalInvocationIndex`'s three LOCAL vertex indices
    // ── B4 TASK / AMPLIFICATION (stage == Task). A task workgroup computes `task_emit` = the number of MESH workgroups to
    // launch (GPU-driven amplification: `EmitMeshTasksEXT` / AS `DispatchMesh`) and optionally writes `task_payload` (a single
    // uint) into the task→mesh PAYLOAD, which the mesh stage reads via `KBuiltin::TaskPayload`. A task emits no geometry (no
    // position / out / mesh_*). `local_size` sets the task workgroup size. Appended at END (KEntry is cook-serialized).
    int          task_emit    = -1; // u32 node: the mesh-workgroup count the task emits (>= 0 ⇒ this is a task entry)
    // B4: the task→mesh PAYLOAD — up to 4 u32 fields (a meshlet pass passes bounds/LOD/material, not just one uint). The mesh
    // reads field i via KBuiltin::TaskPayload{,1,2,3}. `n_task_payload` = how many fields the task writes (0 = no payload).
    static constexpr int kMaxTaskPayload = 4;
    int                  task_payload[kMaxTaskPayload] = {-1, -1, -1, -1}; // u32 nodes, one per field
    crd::u32             n_task_payload                = 0;                // number of active payload fields (0..4)
    // ── B4-tess TESSELLATION (stage == TessControl / TessEval) — the PORTABLE displacement path (mobile / WebGPU / older HW
    // without mesh shaders). A QUAD patch of `tess_patch_size` control points (4). The TESS-CONTROL (hull) stage passes the
    // control points through and sets the tess levels from `tess_inner` / `tess_outer` (float nodes); the TESS-EVAL (domain)
    // stage reads `KBuiltin::TessPatchPosition` (the emitter's bilerp of the 4 corners by gl_TessCoord) + writes the displaced
    // clip `position` (+ `out[]` interpolants). Appended at END (KEntry is cook-serialized).
    crd::u32     tess_patch_size = 0; // control points per patch (4 = quad; 0 ⇒ not a tess entry)
    int          tess_inner      = -1; // TessControl: inner tess level (float node)
    int          tess_outer      = -1; // TessControl: outer tess level (float node)
    [[nodiscard]] bool is_kernel() const noexcept { return kernel_body_count > 0; }
    [[nodiscard]] bool is_mesh() const noexcept { return stage == KStage::Mesh && mesh_vertices > 0U; }
    [[nodiscard]] bool is_task() const noexcept { return stage == KStage::Task && task_emit >= 0; }
    [[nodiscard]] bool is_tess_control() const noexcept { return stage == KStage::TessControl && tess_patch_size > 0U; }
    [[nodiscard]] bool is_tess_eval() const noexcept { return stage == KStage::TessEval && tess_patch_size > 0U; }
};

[[nodiscard]] inline bool is_compare(KOp op) noexcept
{
    switch (op)
    {
    case KOp::CmpLt: case KOp::CmpEq: case KOp::CmpLe:
    case KOp::CmpGt: case KOp::CmpGe: case KOp::CmpNe: return true;
    default: return false;
    }
}

// a reduction op (fixed-order accumulation over a trailing-contiguous axis mask). Central classifier so every emitter
// + backend dispatch handles the whole family; adding a reduce = extend this + the per-op combine.
// Arg-reductions return the INDEX (f32-encoded) of the extremum along the axis, not the value (first occurrence wins).
[[nodiscard]] inline bool is_argreduce(KOp op) noexcept { return op == KOp::ArgMax || op == KOp::ArgMin; }
// Reductions with a T2 fast-parallel (workgroup tree) schedule. Max/Min are order-invariant ⇒ T2 stays BIT-EXACT vs T1;
// Sum/Prod reassociate ⇒ T2 is RFA (run-to-run deterministic, exact within ULP). Arg* stay T1-only (index tracking).
[[nodiscard]] inline bool is_fast_reduceable(KOp op) noexcept
{
    return op == KOp::ReduceSum || op == KOp::ReduceProd || op == KOp::ReduceMax || op == KOp::ReduceMin;
}
[[nodiscard]] inline bool is_reduce(KOp op) noexcept
{
    return op == KOp::ReduceSum || op == KOp::ReduceMax || op == KOp::ReduceMin || op == KOp::ReduceProd || is_argreduce(op);
}

constexpr int kMaxRank = 8;

// Determinism tier for a reduction/scan. T1 (Exact) = fixed ascending order ⇒ bit-exact vs the CPU oracle + run-to-run
// deterministic (the default). T2 (Fast) = a reordered parallel schedule (workgroup tree-reduce) ⇒ NOT bit-exact vs T1
// in float, but still RUN-TO-RUN deterministic (fixed tree order) and correct within ULP — the "push to the hardware
// limit" path where reduction reassociation is acceptable. Selected explicitly by the graph builder, never implicitly.
enum class DetTier : crd::u8 { Exact = 0, Fast = 1 };

struct Shape
{
    crd::i64 dims[kMaxRank] = {};
    int      rank = 0;

    [[nodiscard]] crd::i64 numel() const noexcept
    {
        crd::i64 n = 1;
        for (int i = 0; i < rank; ++i) { n *= dims[i]; }
        return n;
    }
    [[nodiscard]] bool operator==(const Shape& o) const noexcept
    {
        if (rank != o.rank) { return false; }
        for (int i = 0; i < rank; ++i) { if (dims[i] != o.dims[i]) { return false; } }
        return true;
    }
    // row-major strides into `s` (elements, not bytes).
    void row_major_strides(crd::i64* s) const noexcept
    {
        crd::i64 acc = 1;
        for (int i = rank - 1; i >= 0; --i) { s[i] = acc; acc *= dims[i]; }
    }
};

[[nodiscard]] inline Shape make_shape(std::initializer_list<crd::i64> d) noexcept
{
    Shape s;
    s.rank = static_cast<int>(d.size());
    int i  = 0;
    for (const crd::i64 v : d) { s.dims[i++] = v; }
    return s;
}

struct KNode
{
    KOp      op;
    KType    type;                   // per-element value type (scalar dtype + form) — the single source of truth
    Shape    shape;
    crd::i32 a = -1, b = -1, c = -1, d = -1; // operands (-1 = none); d added for 4-operand ops (mat4-from-columns)
    crd::f64 cval = 0.0;             // Const value
    crd::i32 iidx = 0;               // Input index / Iota axis
    crd::u32 axes = 0;               // ReduceSum/Max/Broadcast: axis bitmask
    crd::u8  perm[kMaxRank] = {};    // Permute permutation
    DetTier  tier = DetTier::Exact;  // reductions/scan: T1 exact (default) vs T2 fast-parallel
    // B0-4: VARIADIC operands. Four slots (a/b/c/d) cannot hold a struct's N fields, so aggregate constructors park
    // their operand list in `KGraph`'s flat ext pool at [ext, ext + n_ext). Every operand walk — DCE, the CSE key, the
    // `optimize()` renumber — must visit these too; that is exactly the bug class the missing `d` remap was (B0-0), so
    // `operands_valid()` checks them.
    crd::i32 ext   = -1;             // offset into KGraph's ext-operand pool (-1 = none)
    crd::u16 n_ext = 0;              // operand count at that offset
    // B3: `UniformBlock` lives at (dset, iidx=binding). `dset` IS ADR-0102's frequency: 0 frame · 1 pass/lighting ·
    // 2 material · 3 object. `StageIn` uses `iidx` as its location; `Builtin` uses `iidx` as the KBuiltin value.
    // B1-c: a `StageIn` (fragment interpolant) reuses `dset` to carry its `Interp` qualifier (dset is otherwise unused there).
    crd::u8 dset = 0;

    [[nodiscard]] constexpr DType dtype() const noexcept { return type.scalar; }
    [[nodiscard]] constexpr int   comps() const noexcept { return type.comps(); }
};

// round an f64 accumulator to a storage dtype so the reference is bit-faithful to that precision.
[[nodiscard]] inline crd::f64 round_dtype(crd::f64 v, DType dt) noexcept
{
    if (dt == DType::F32) { return static_cast<crd::f64>(static_cast<float>(v)); }
    if (dt == DType::Bool) { return v != 0.0 ? 1.0 : 0.0; } // a bool materializes as exactly 0.0 or 1.0 in the oracle
    // Integer storage types are INTEGRAL and truncate toward zero — a GPU int/uint is never fractional, and `uint/uint`
    // division + `(u)int(x)` casts truncate. Without this the oracle keeps fractional index arithmetic and diverges from
    // every backend (found wiring the FFT/transpose index math). Bit-op / add-mul results are already integral ⇒ no-op there.
    if (dt == DType::I32 || dt == DType::I64 || dt == DType::U8 || dt == DType::U32) { return static_cast<crd::f64>(static_cast<crd::i64>(v)); }
    return v; // F64 / F16 / BF16 (exact narrow rounding lands with their backends)
}

[[nodiscard]] inline crd::f64 apply_unary(KOp op, crd::f64 x) noexcept
{
    switch (op)
    {
    case KOp::Neg: return -x;
    case KOp::Recip: return 1.0 / x;
    case KOp::Abs: return x < 0.0 ? -x : x;
    case KOp::Exp: return crd::math::exp(x);
    case KOp::Log: return crd::math::log(x);
    case KOp::Sin: return crd::math::sin(x);
    case KOp::Cos: return crd::math::cos(x);
    case KOp::Sqrt: return crd::math::sqrt(x);
    case KOp::Tanh: return crd::math::tanh(x);
    case KOp::Floor: return crd::math::floor(x);
    case KOp::Ceil: return crd::math::ceil(x);
    case KOp::Sign: return (x > 0.0 ? 1.0 : 0.0) - (x < 0.0 ? 1.0 : 0.0);
    case KOp::Trunc: return crd::math::trunc(x);
    case KOp::Round: return crd::math::nearbyint(x); // ties-to-even — matches roundEven/rintf/round/rint on every backend
    case KOp::Fract: return x - crd::math::floor(x); // GLSL fract — exact
    case KOp::Rsqrt: return crd::math::rsqrt(x);     // ULP
    case KOp::Exp2: return crd::math::exp2(x);       // ULP
    case KOp::Log2: return crd::math::log2(x);       // ULP
    case KOp::Tan: return crd::math::tan(x);         // ULP
    case KOp::Radians: return x * 0.017453292519943295; // π/180 — exact single mul
    case KOp::Degrees: return x * 57.29577951308232;    // 180/π — exact single mul
    case KOp::Asin: return crd::math::asin(x);          // ULP
    case KOp::Acos: return crd::math::acos(x);          // ULP
    case KOp::Atan: return crd::math::atan(x);          // ULP
    case KOp::Sinh: return crd::math::sinh(x);          // ULP
    case KOp::Cosh: return crd::math::cosh(x);          // ULP
    case KOp::Cbrt: return crd::math::cbrt(x);          // ULP (no GPU builtin — emitted as sign·pow(abs,1/3))
    case KOp::BitNot: return static_cast<crd::f64>(~static_cast<crd::i64>(x)); // 32-bit-compatible for ≤31-bit values
    case KOp::BitCount: { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(x)); int cnt = 0; while (v != 0U) { cnt += static_cast<int>(v & 1U); v >>= 1U; } return static_cast<crd::f64>(cnt); }
    case KOp::FindLSB: { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(x)); if (v == 0U) { return -1.0; } int i = 0; while ((v & 1U) == 0U) { ++i; v >>= 1U; } return static_cast<crd::f64>(i); }
    case KOp::FindMSB: { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(x)); int i = -1; while (v != 0U) { ++i; v >>= 1U; } return static_cast<crd::f64>(i); }
    case KOp::BitReverse: { crd::u32 v = static_cast<crd::u32>(static_cast<crd::i64>(x)); crd::u32 r = 0U; for (int b = 0; b < 32; ++b) { r = (r << 1U) | (v & 1U); v >>= 1U; } return static_cast<crd::f64>(r); }
    case KOp::FloatBitsToInt: { const float f = static_cast<float>(x); crd::i32 b = 0; std::memcpy(&b, &f, 4); return static_cast<crd::f64>(b); } // reinterpret f32 bits → i32
    case KOp::IntBitsToFloat: { const crd::i32 b = static_cast<crd::i32>(static_cast<crd::i64>(x)); float f = 0.0F; std::memcpy(&f, &b, 4); return static_cast<crd::f64>(f); } // reinterpret i32 bits → f32
    // B1 fragment derivatives: the CPU oracle sees ONE invocation with no neighbours, so ∂/∂x = ∂/∂y = 0 (and fwidth = 0).
    // On the GPU these read the 2×2 quad; they are fragment-only and never appear in a CPU-run compute graph.
    case KOp::DFdx: case KOp::DFdy: case KOp::Fwidth: return 0.0;
    default: return x;
    }
}
[[nodiscard]] inline crd::f64 apply_binary(KOp op, crd::f64 x, crd::f64 y) noexcept
{
    switch (op)
    {
    case KOp::Add: return x + y;
    case KOp::Sub: return x - y;
    case KOp::Mul: return x * y;
    case KOp::Div: return x / y;
    case KOp::Max: return x > y ? x : y;
    case KOp::Min: return x < y ? x : y;
    case KOp::CmpLt: return x < y ? 1.0 : 0.0;
    case KOp::CmpEq: return x == y ? 1.0 : 0.0;
    case KOp::CmpLe: return x <= y ? 1.0 : 0.0;
    case KOp::CmpGt: return x > y ? 1.0 : 0.0;
    case KOp::CmpGe: return x >= y ? 1.0 : 0.0;
    case KOp::CmpNe: return x != y ? 1.0 : 0.0;
    case KOp::Pow: return crd::math::pow(x, y);          // ULP (transcendental)
    case KOp::Step: return y < x ? 0.0 : 1.0;            // GLSL step(edge=x, v=y): v<edge ? 0 : 1 — exact
    case KOp::Atan2: return crd::math::atan2(x, y);      // atan2(y=x, x=y) — ULP
    case KOp::Mod: return crd::math::fmod(x, y);         // C fmod (sign of x) — ULP
    case KOp::Ldexp: { crd::f64 p = 1.0; const crd::i64 e = static_cast<crd::i64>(y); if (e >= 0) { for (crd::i64 j = 0; j < e; ++j) { p *= 2.0; } } else { for (crd::i64 j = 0; j < -e; ++j) { p *= 0.5; } } return x * p; } // m * 2^e (exact)
    // Integer bitwise: reinterpret the (exactly-integer) f64 through i64, operate, return exact. Valid for values within
    // f64's exact-integer range (|v| < 2^53) — morton (≤30-bit), radix keys, hashing all fit.
    case KOp::Shl: return static_cast<crd::f64>(static_cast<crd::i64>(x) << static_cast<crd::i64>(y));
    case KOp::Shr: return static_cast<crd::f64>(static_cast<crd::i64>(x) >> static_cast<crd::i64>(y));
    case KOp::BitAnd: return static_cast<crd::f64>(static_cast<crd::i64>(x) & static_cast<crd::i64>(y));
    case KOp::BitOr: return static_cast<crd::f64>(static_cast<crd::i64>(x) | static_cast<crd::i64>(y));
    case KOp::BitXor: return static_cast<crd::f64>(static_cast<crd::i64>(x) ^ static_cast<crd::i64>(y));
    default: return 0.0;
    }
}
// Dtype-aware binary: Add/Sub/Mul/Shl on a 32-bit INTEGER type must WRAP mod 2^32 exactly as the GPU does — the f64 path in
// `apply_binary` silently loses bits once a product exceeds 2^53 (a full 32×32-bit hash multiply is ~2^64) and never wraps, so
// integer PRNG/hash kernels diverge from every backend. Compute those in real u32 (two's-complement, wrapping) and reinterpret
// for a signed result. Shr/And/Or/Xor and every float op are already exact for in-range integers ⇒ fall through to `apply_binary`.
[[nodiscard]] inline crd::f64 apply_binary_typed(KOp op, crd::f64 x, crd::f64 y, DType dt) noexcept
{
    const bool wrap32 = (dt == DType::U32 || dt == DType::I32) &&
                        (op == KOp::Add || op == KOp::Sub || op == KOp::Mul || op == KOp::Shl);
    if (wrap32)
    {
        const crd::u32 a = static_cast<crd::u32>(static_cast<crd::i64>(x));
        const crd::u32 b = static_cast<crd::u32>(static_cast<crd::i64>(y));
        crd::u32       z = 0U;
        switch (op)
        {
        case KOp::Add: z = a + b; break;                 // unsigned ⇒ two's-complement wrap mod 2^32 (matches GPU int + uint)
        case KOp::Sub: z = a - b; break;
        case KOp::Mul: z = a * b; break;
        case KOp::Shl: z = a << (b & 31U); break;
        default: break;
        }
        return dt == DType::U32 ? static_cast<crd::f64>(z) : static_cast<crd::f64>(static_cast<crd::i32>(z));
    }
    return apply_binary(op, x, y);
}
// Ternary shader intrinsics (a/b/c operands). Explicit formulas ⇒ the emitters match bit-for-bit (with NoContraction).
[[nodiscard]] inline crd::f64 apply_ternary(KOp op, crd::f64 a, crd::f64 b, crd::f64 c) noexcept
{
    switch (op)
    {
    case KOp::Clamp: { const crd::f64 m = a > b ? a : b; return m < c ? m : c; } // clamp(x=a, min=b, max=c) = min(max(a,b),c)
    case KOp::Mix: return a * (1.0 - c) + b * c;                                 // mix(x=a, y=b, t=c) = x*(1-t)+y*t
    case KOp::Smoothstep: { const crd::f64 u = (c - a) / (b - a); const crd::f64 hi = u > 1.0 ? 1.0 : u; const crd::f64 t = u < 0.0 ? 0.0 : hi; return t * t * (3.0 - 2.0 * t); } // smoothstep(e0=a,e1=b,x=c) — ULP; NaN u falls through both tests, as GLSL requires
    case KOp::Fma: return crd::math::fma(a, b, c);                               // a*b+c single-rounded (IEEE fma) — bit-exact
    case KOp::BitfieldExtract: { const crd::i64 iv = static_cast<crd::i64>(a); const crd::i64 off = static_cast<crd::i64>(b); const crd::i64 bits = static_cast<crd::i64>(c); return static_cast<crd::f64>((iv >> off) & ((static_cast<crd::i64>(1) << bits) - 1)); } // (v>>off)&mask
    default: return a;
    }
}

class KGraph
{
public:
    explicit KGraph(crd::memory::IAllocator* alloc) noexcept
        : m_nodes(alloc), m_ext(alloc), m_sfields(alloc), m_sbegin(alloc), m_stmts(alloc)
    {
    }

    // ── B0-4: struct registry ────────────────────────────────────────────────────────────────────────────────────────
    // A struct is a named, ordered list of field types, stored CSR-style (m_sbegin indexes into m_sfields). Fields are
    // laid out contiguously in the flat component run — the same interleaved storage vec/mat values already use — so no
    // std430 padding rules appear here. Buffer-BACKED structs (which do need std430) are the resource-binding slice, B3.
    [[nodiscard]] int define_struct(const KType* fields, int n_fields)
    {
        const int id = static_cast<int>(m_sbegin.size());
        m_sbegin.push_back(static_cast<crd::u32>(m_sfields.size())); // one `begin` per struct; the end is the next begin
        for (int i = 0; i < n_fields; ++i) { m_sfields.push_back(fields[i]); }
        return id;
    }
    [[nodiscard]] int struct_field_count(int id) const noexcept
    {
        const crd::u32 b = m_sbegin[static_cast<crd::usize>(id)];
        const crd::u32 e = (static_cast<crd::usize>(id) + 1 < m_sbegin.size()) ? m_sbegin[static_cast<crd::usize>(id) + 1]
                                                                              : static_cast<crd::u32>(m_sfields.size());
        return static_cast<int>(e - b);
    }
    [[nodiscard]] KType struct_field(int id, int k) const noexcept
    {
        return m_sfields[static_cast<crd::usize>(m_sbegin[static_cast<crd::usize>(id)]) + static_cast<crd::usize>(k)];
    }
    // flat scalar offset of field `k` within one struct element
    [[nodiscard]] int struct_field_offset(int id, int k) const noexcept
    {
        int off = 0;
        for (int i = 0; i < k; ++i) { off += struct_field(id, i).comps(); }
        return off;
    }
    [[nodiscard]] int struct_flat_comps(int id) const noexcept
    {
        return struct_field_offset(id, struct_field_count(id));
    }
    // the KType naming a registered struct (carries the cached flat size so `KType::comps()` needs no registry lookup).
    [[nodiscard]] KType struct_type(int id) const noexcept
    {
        KType t;
        t.kind       = TKind::Struct;
        t.struct_id  = static_cast<crd::i16>(id);
        t.elem_comps = static_cast<crd::u16>(struct_flat_comps(id));
        return t;
    }

    // variadic-operand accessors (the ext pool)
    [[nodiscard]] int ext_operand(const KNode& n, int k) const noexcept
    {
        return m_ext[static_cast<crd::usize>(n.ext) + static_cast<crd::usize>(k)];
    }

    [[nodiscard]] int input(const Shape& shape, DType dt) { KNode n; n.op = KOp::Input; n.type = KType::make_scalar(dt); n.shape = shape; n.iidx = m_ninput++; return push(n); }
    // A3: a vector/matrix input (comps>1) — per-element vecN/matN, fed as comps interleaved values per element.
    // NOTE `comps == 4` resolves to vec4 (the A3 meaning); a mat2 input is ambiguous there and must use `input_mat`.
    [[nodiscard]] int input_vec(const Shape& shape, DType dt, int comps) { KNode n; n.op = KOp::Input; n.type = KType::from_comps(dt, comps); n.shape = shape; n.iidx = m_ninput++; return push(n); }
    // B0-2: an explicit RxC matrix input, fed as rows*cols column-major values per element. The only way to feed a mat2
    // (comps 4 collides with vec4) or any non-square matrix.
    [[nodiscard]] int input_mat(const Shape& shape, DType dt, int rows, int cols) { KNode n; n.op = KOp::Input; n.type = KType::mat(dt, rows, cols); n.shape = shape; n.iidx = m_ninput++; return push(n); }

    // ── B-cmp: compute-KERNEL authoring — resources, indexed reads, and the effect-statement body ────────────────────────
    // A bound global storage buffer of `elem` scalars at (set, binding). `writable` ⇒ the kernel BufferStores it.
    [[nodiscard]] int buffer_decl(DType elem, int set, int binding, bool writable) { KNode n; n.op = KOp::BufferDecl; n.type = KType::make_scalar(elem); n.shape = make_shape({1}); n.dset = static_cast<crd::u8>(set); n.iidx = binding; n.axes = writable ? 1U : 0U; return push(n); }
    // COHERENT+VOLATILE writable buffer — cross-workgroup visible (device-scope). `axes` bit1 = coherent; the emitters add
    // `coherent volatile` so a SpinUntilNonzero re-reads global each iteration and a publish is seen by other blocks.
    [[nodiscard]] int buffer_decl_coherent(DType elem, int set, int binding) { KNode n; n.op = KOp::BufferDecl; n.type = KType::make_scalar(elem); n.shape = make_shape({1}); n.dset = static_cast<crd::u8>(set); n.iidx = binding; n.axes = 3U; return push(n); }
    // A workgroup-shared array of `length` scalars (+ `pad` extra elems for bank-conflict-free strided access). Compile-time size.
    [[nodiscard]] int shared_decl(DType elem, int length, int pad = 0) { KNode n; n.op = KOp::SharedDecl; n.type = KType::make_scalar(elem); n.shape = make_shape({1}); n.iidx = length; n.axes = static_cast<crd::u32>(pad); return push(n); }
    // Indexed reads (per-invocation scalar values fed into the value graph).
    [[nodiscard]] int buffer_load(int buf, int idx) { KNode n; n.op = KOp::BufferLoad; n.type = KType::make_scalar(t(buf).dtype()); n.shape = make_shape({1}); n.a = buf; n.b = idx; return push(n); }
    [[nodiscard]] int shared_load(int sh, int idx)  { KNode n; n.op = KOp::SharedLoad; n.type = KType::make_scalar(t(sh).dtype());  n.shape = make_shape({1}); n.a = sh;  n.b = idx; return push(n); }
    // Effect statements (appended to the kernel body pool in order). Mark the body start, append, then set the entry's range.
    [[nodiscard]] int kernel_stmt_mark() const noexcept { return static_cast<int>(m_stmts.size()); }
    void stmt_buffer_store(int buf, int idx, int val) { KStmt s; s.kind = KStmtKind::BufferStore; s.target = buf; s.index = idx; s.value = val; m_stmts.push_back(s); }
    void stmt_shared_store(int sh, int idx, int val)  { KStmt s; s.kind = KStmtKind::SharedStore; s.target = sh;  s.index = idx; s.value = val; m_stmts.push_back(s); }
    void stmt_barrier(BarrierScope scope = BarrierScope::Workgroup) { KStmt s; s.kind = KStmtKind::Barrier; s.scope = scope; m_stmts.push_back(s); }
    // SPIN-WAIT until the coherent flag buffer[idx] becomes nonzero (single-pass inter-block sync). Call from ONE thread inside
    // an `if (tid==0)` guard, then a workgroup barrier so the whole block sees the published value.
    void stmt_spin_until_nonzero(int buf, int idx) { KStmt s; s.kind = KStmtKind::SpinUntilNonzero; s.target = buf; s.index = idx; m_stmts.push_back(s); }
    // ATOMIC ADD to shared[idx] (U32 histogram bin, etc.). Bit-exact: the total is a sum (order-independent).
    void stmt_shared_atomic_add(int sh, int idx, int val) { KStmt s; s.kind = KStmtKind::SharedAtomicAdd; s.target = sh; s.index = idx; s.value = val; m_stmts.push_back(s); }
    void stmt_buffer_atomic_add(int buf, int idx, int val) { KStmt s; s.kind = KStmtKind::BufferAtomicAdd; s.target = buf; s.index = idx; s.value = val; m_stmts.push_back(s); }
    // B4-vis: ATOMIC MIN on a global u32 buffer element (the software-rasterizer visibility key: nearest depth|id wins).
    void stmt_buffer_atomic_min(int buf, int idx, int val) { KStmt s; s.kind = KStmtKind::BufferAtomicMin; s.target = buf; s.index = idx; s.value = val; m_stmts.push_back(s); }
    // B17: value-returning atomics — return the AtomicResult node holding the OLD element (materialized once). The scalable
    // A-buffer's `atomicAdd(counter,1)` node allocator + `atomicExchange(head[pixel], slot)` linked-list push.
    [[nodiscard]] int atomic_add_fetch(int buf, int idx, int val) { KNode n; n.op = KOp::AtomicResult; n.type = KType::make_scalar(t(buf).dtype()); n.shape = make_shape({1}); const int res = push(n); KStmt s; s.kind = KStmtKind::BufferAtomicAddFetch; s.target = buf; s.index = idx; s.value = val; s.result = res; m_stmts.push_back(s); return res; }
    [[nodiscard]] int atomic_exchange(int buf, int idx, int val)  { KNode n; n.op = KOp::AtomicResult; n.type = KType::make_scalar(t(buf).dtype()); n.shape = make_shape({1}); const int res = push(n); KStmt s; s.kind = KStmtKind::BufferAtomicExchange; s.target = buf; s.index = idx; s.value = val; s.result = res; m_stmts.push_back(s); return res; }
    // B9/RT-1: an opaque acceleration-structure (TLAS) resource leaf bound at (set, binding) — the RT peer of buffer_decl.
    [[nodiscard]] int accel_struct_decl(int set, int binding) { KNode n; n.op = KOp::AccelStructDecl; n.type = KType::make_scalar(DType::U32); n.shape = make_shape({1}); n.dset = static_cast<crd::u8>(set); n.iidx = binding; return push(n); }
    // B9/RT-1: INLINE RAY QUERY — cast a ray at `as` and return the CLOSEST-HIT distance `t` (or `tmax` on miss) as an F32
    // node; `hit = t < tmax`. The ray is 8 SCALAR nodes (origin ox/oy/oz, dir dx/dy/dz, tmin, tmax) — the compute-kernel
    // emitter is scalar-only, so origin/dir arrive component-wise and the emit builds the `vec3`s inline. Stored in the
    // statement's ext-operand list. `as` = an AccelStructDecl node.
    [[nodiscard]] int trace_ray_closest(int as, int ox, int oy, int oz, int dx, int dy, int dz, int tmin, int tmax)
    {
        KNode     n; n.op = KOp::RayHitResult; n.type = KType::make_scalar(DType::F32); n.shape = make_shape({1});
        const int res       = push(n);
        const int ops[8]    = {ox, oy, oz, dx, dy, dz, tmin, tmax};
        KStmt     s; s.kind = KStmtKind::TraceRayClosest; s.target = as; s.result = res;
        s.ext   = push_ext(ops, 8);
        s.n_ext = 8U;
        m_stmts.push_back(s);
        return res;
    }
    struct RtHit { int t = -1; int prim = -1; }; // closest-hit distance + the triangle index (0xFFFFFFFF on miss)
    // B9/RT-2: like trace_ray_closest, but ALSO returns the hit's PRIMITIVE INDEX (which triangle) for shading the hit.
    [[nodiscard]] RtHit trace_ray_hit(int as, int ox, int oy, int oz, int dx, int dy, int dz, int tmin, int tmax)
    {
        KNode     nt; nt.op = KOp::RayHitResult; nt.type = KType::make_scalar(DType::F32); nt.shape = make_shape({1});
        const int rt = push(nt);
        KNode     np; np.op = KOp::RayHitResult; np.type = KType::make_scalar(DType::U32); np.shape = make_shape({1});
        const int rp       = push(np);
        const int ops[9]   = {ox, oy, oz, dx, dy, dz, tmin, tmax, rp};
        KStmt     s; s.kind = KStmtKind::TraceRayHit; s.target = as; s.result = rt;
        s.ext   = push_ext(ops, 9);
        s.n_ext = 9U;
        m_stmts.push_back(s);
        return {rt, rp};
    }
    // FA-2 (portable RT PIPELINE): declare an N-float ray PAYLOAD carried across raygen ↔ closest-hit/miss.
    [[nodiscard]] int ray_payload_decl(int n_components) { KNode nd; nd.op = KOp::RayPayloadDecl; nd.type = KType::make_scalar(DType::F32); nd.shape = make_shape({1}); nd.iidx = n_components; return push(nd); }
    // read component `comp` of the payload (F32).
    [[nodiscard]] int payload_load(int payload, int comp) { KNode nd; nd.op = KOp::PayloadLoad; nd.type = KType::make_scalar(DType::F32); nd.shape = make_shape({1}); nd.a = payload; nd.iidx = comp; return push(nd); }
    // write a payload component (closest-hit / miss shaders).
    void stmt_payload_store(int payload, int comp, int value) { KStmt s; s.kind = KStmtKind::PayloadStore; s.target = payload; s.index = comp; s.value = value; m_stmts.push_back(s); }
    // trace a ray through the FULL pipeline (invokes the hit/miss shaders, fills `payload`). `as` = AccelStructDecl, 8 ray scalars.
    void stmt_trace_ray_pipeline(int as, int payload, int ox, int oy, int oz, int dx, int dy, int dz, int tmin, int tmax)
    {
        const int ops[8] = {ox, oy, oz, dx, dy, dz, tmin, tmax};
        KStmt     s; s.kind = KStmtKind::TraceRayPipeline; s.target = as; s.value = payload;
        s.ext = push_ext(ops, 8); s.n_ext = 8U;
        m_stmts.push_back(s);
    }
    // FA-2 SER: a shader-execution-reorder hint (perf only). Emits reorderThreadNV where supported, nothing where not.
    void stmt_reorder_thread() { KStmt s; s.kind = KStmtKind::ReorderThread; m_stmts.push_back(s); }
    // P4 (any-hit ALPHA test): ignore this candidate intersection when `cond` (a BOOL node) is true — the portable OMM fallback.
    void stmt_ignore_hit_if(int cond) { KStmt s; s.kind = KStmtKind::IgnoreHitIf; s.value = cond; m_stmts.push_back(s); }
    // Read the k-th variadic scalar operand of a statement (RT ray components) from the ext pool.
    [[nodiscard]] int stmt_ext_operand(const KStmt& s, int k) const { return m_ext[static_cast<crd::usize>(s.ext) + static_cast<crd::usize>(k)]; }
    void stmt_for_break_if(int cond) { KStmt s; s.kind = KStmtKind::ForBreakIf; s.value = cond; m_stmts.push_back(s); }
    void stmt_buffer_ticket(int buf, int idx, int sh) { KStmt s; s.kind = KStmtKind::BufferTicket; s.target = buf; s.index = idx; s.value = sh; m_stmts.push_back(s); }
    void stmt_sync_warp() { KStmt s; s.kind = KStmtKind::SyncWarp; m_stmts.push_back(s); }
    void stmt_materialize(int val) { KStmt s; s.kind = KStmtKind::Materialize; s.value = val; m_stmts.push_back(s); } // freeze `val` into a register
    // ── STRUCTURED control flow (For / If). LAYOUT: the control statement is followed CONTIGUOUSLY by its body in the pool;
    // the parent range spans BOTH, and every consumer (the CPU oracle + all 5 emitters) executes the body via `body_begin`/
    // `body_count` then SKIPS past it. Author with the scoped pattern:
    //   int f = stmt_for_begin(count); int lv = kernel_loop_var(f); <body using lv>; stmt_for_end(f);
    //   int c = stmt_if_begin(cond);   <body>;                       stmt_if_end(c);
    // Bodies nest (a For/If inside another's body); every `*_end` must pair with its `*_begin`, innermost first.
    [[nodiscard]] int stmt_for_begin(int count) { KStmt s; s.kind = KStmtKind::For; s.value = count; s.body_begin = static_cast<crd::i32>(m_stmts.size()) + 1; m_stmts.push_back(s); return static_cast<int>(m_stmts.size()) - 1; }
    void              stmt_for_end(int for_id)  { KStmt& s = m_stmts[static_cast<crd::usize>(for_id)]; s.body_count = static_cast<crd::i32>(m_stmts.size()) - s.body_begin; }
    [[nodiscard]] int stmt_if_begin(int cond)   { KStmt s; s.kind = KStmtKind::If;  s.value = cond;  s.body_begin = static_cast<crd::i32>(m_stmts.size()) + 1; m_stmts.push_back(s); return static_cast<int>(m_stmts.size()) - 1; }
    void              stmt_if_end(int if_id)    { KStmt& s = m_stmts[static_cast<crd::usize>(if_id)]; s.body_count = static_cast<crd::i32>(m_stmts.size()) - s.body_begin; }
    // The induction value (0..count-1, U32) of the enclosing `For` identified by `for_id` — a value leaf read inside its body.
    [[nodiscard]] int kernel_loop_var(int for_id) { KNode n; n.op = KOp::KernelLoopVar; n.type = KType::make_scalar(DType::U32); n.shape = make_shape({1}); n.a = for_id; return push(n); }
    [[nodiscard]] int          stmt_count() const noexcept { return static_cast<int>(m_stmts.size()); }
    [[nodiscard]] const KStmt& stmt(int i)  const noexcept { return m_stmts[static_cast<crd::usize>(i)]; }
    [[nodiscard]] int constant(crd::f64 v, const Shape& shape, DType dt) { KNode n; n.op = KOp::Const; n.type = KType::make_scalar(dt); n.shape = shape; n.cval = v; return push(n); }
    [[nodiscard]] int iota(const Shape& shape, int axis, DType dt) { KNode n; n.op = KOp::Iota; n.type = KType::make_scalar(dt); n.shape = shape; n.iidx = axis; return push(n); }

    [[nodiscard]] int unary(KOp op, int a) { KNode n; n.op = op; n.type = t(a).type; n.shape = t(a).shape; n.a = a; return push(n); }
    [[nodiscard]] int binary(KOp op, int a, int b) { KNode n; n.op = op; n.type = is_compare(op) ? t(a).type.with_scalar(DType::Bool) : t(a).type; n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int ternary(KOp op, int a, int b, int c) { KNode n; n.op = op; n.type = t(a).type; n.shape = t(a).shape; n.a = a; n.b = b; n.c = c; return push(n); } // Clamp/Mix/Fma
    [[nodiscard]] int select(int cond, int a, int b) { KNode n; n.op = KOp::Select; n.type = t(a).type; n.shape = t(a).shape; n.a = a; n.b = b; n.c = cond; return push(n); }
    // SUBGROUP (wave) ops — cross-lane within a fixed 32-lane subgroup, both → U32. ballot(pred)=bitmask of nonzero-pred lanes;
    // ballot_excl_count(mask)=popcount of set bits below this lane. Compose → a cheap deterministic radix rank / warp-scan.
    [[nodiscard]] int subgroup_ballot(int pred) { KNode n; n.op = KOp::SubgroupBallot; n.type = KType::make_scalar(DType::U32); n.shape = t(pred).shape; n.a = pred; return push(n); }
    [[nodiscard]] int subgroup_ballot_excl_count(int mask) { KNode n; n.op = KOp::SubgroupBallotExclCount; n.type = KType::make_scalar(DType::U32); n.shape = t(mask).shape; n.a = mask; return push(n); }
    [[nodiscard]] int subgroup_match(int value) { KNode n; n.op = KOp::SubgroupMatch; n.type = KType::make_scalar(DType::U32); n.shape = t(value).shape; n.a = value; return push(n); }
    // Structured control flow — FIXED-count loop (A4 tier 1): acc = init; for it in [0,count): acc = body(it, acc); return acc.
    // Compile-time UNROLL ⇒ pure dataflow ⇒ runs on EVERY backend through the existing emitters (no IR/eval/emit change).
    // `body(int it, int acc) -> int` returns the next accumulator node. For DYNAMIC/large trip counts, the region-based
    // dynamic For/While (tier 2) is the follow-on slice (needs loop-body scoping in the eval + emitters).
    // `body` is taken BY VALUE, not by forwarding reference: it is invoked once per iteration, so forwarding it would
    // move from it on the first call. Same reason for `for_loop` / `while_loop` below.
    template <typename BodyFn>
    [[nodiscard]] int unroll_for(int count, int init, BodyFn body)
    {
        int acc = init;
        for (int it = 0; it < count; ++it) { acc = body(it, acc); }
        return acc;
    }
    // A4 tier-2 DYNAMIC loop (real per-thread `for`): acc = init; for it in [0, count): acc = body(index, acc); return acc.
    // `count` may be a per-element (divergent) node; `body_fn(index_node, acc_node) -> next-acc node` builds the body from
    // the body-scoped `LoopIndex` (F32 iteration) + `LoopAcc` (current accumulator) leaves. Single-level (no nesting yet).
    template <typename BodyFn>
    [[nodiscard]] int for_loop(int count, int init, BodyFn body_fn)
    {
        KNode     ix; ix.op = KOp::LoopIndex; ix.type = KType::make_scalar(DType::F32); ix.shape = t(init).shape; const int idx = push(ix);
        KNode     ac; ac.op = KOp::LoopAcc; ac.type = t(init).type; ac.shape = t(init).shape; const int acc = push(ac);
        const int body = body_fn(idx, acc);
        KNode     n; n.op = KOp::For; n.type = t(init).type; n.shape = t(init).shape; n.a = count; n.b = init; n.c = body;
        return push(n);
    }
    // A4 tier-2 BOUNDED while (the GPU-safe form — no unbounded loops on a GPU): run up to max_iter, but each element
    // FREEZES its accumulator once `cond_fn(acc)` becomes 0. cond_fn(acc)->keep-node (nonzero = keep looping);
    // body_fn(index, acc)->next-acc node. Lowers to a For + a per-step Select ⇒ runs on every backend.
    template <typename CondFn, typename BodyFn>
    [[nodiscard]] int while_loop(int max_iter, int init, CondFn cond_fn, BodyFn body_fn)
    {
        const int mi = constant(static_cast<crd::f64>(max_iter), t(init).shape, t(init).dtype());
        return for_loop(mi, init, [&](int idx, int acc) { const int keep = cond_fn(acc); const int nxt = body_fn(idx, acc); return select(keep, nxt, acc); });
    }
    // A4 tier-2 SWITCH/if-branch multiplex: (selector == key) ? val : fallback. Chain these for a full switch; a plain
    // if/else on VALUES is just `select(cond, then, else)`. Branchless (both arms evaluated) — the shader-correct form.
    [[nodiscard]] int switch_case(int selector, int key, int val, int fallback) { return select(binary(KOp::CmpNe, selector, key), fallback, val); }
    [[nodiscard]] int cast(int a, DType dt) { KNode n; n.op = KOp::Cast; n.type = t(a).type.with_scalar(dt); n.shape = t(a).shape; n.a = a; return push(n); }
    // A3 vector values (per-element vecN; components stored interleaved in the eval buffer / emitter).
    [[nodiscard]] int vec2(int a, int b) { KNode n; n.op = KOp::Vec2; n.type = KType::vec(t(a).dtype(), 2); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int vec3(int a, int b, int c) { KNode n; n.op = KOp::Vec3; n.type = KType::vec(t(a).dtype(), 3); n.shape = t(a).shape; n.a = a; n.b = b; n.c = c; return push(n); }
    [[nodiscard]] int vec_comp(int v, int idx) { KNode n; n.op = KOp::VecComp; n.type = KType::make_scalar(t(v).dtype()); n.shape = t(v).shape; n.a = v; n.iidx = idx; return push(n); }
    [[nodiscard]] int dot(int a, int b) { KNode n; n.op = KOp::Dot; n.type = KType::make_scalar(t(a).dtype()); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int cross(int a, int b) { KNode n; n.op = KOp::Cross; n.type = KType::vec(t(a).dtype(), 3); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int normalize(int a) { KNode n; n.op = KOp::Normalize; n.type = t(a).type; n.shape = t(a).shape; n.a = a; return push(n); }
    [[nodiscard]] int vlength(int a) { KNode n; n.op = KOp::VecLen; n.type = KType::make_scalar(t(a).dtype()); n.shape = t(a).shape; n.a = a; return push(n); }
    // concat two vec values → a wider vec (comps sum). The primitive for vec4 = concat(vec3, w) and general assembly.
    [[nodiscard]] int vec_concat(int a, int b) { KNode n; n.op = KOp::VecConcat; n.type = KType::vec(t(a).dtype(), t(a).comps() + t(b).comps()); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int vec4(int x, int y, int z, int w) { return vec_concat(vec3(x, y, z), w); } // (x,y,z,w) — comps 4
    // arbitrary swizzle: out component k = source component idx[k]. width = count of valid (>=0) indices. Covers
    // .x (swizzle(v,0)), .xy (v,0,1), .yzx (v,1,2,0), .wzyx (v,3,2,1,0) — reorder + subset + broadcast (repeat) alike.
    [[nodiscard]] int swizzle(int v, int i0, int i1 = -1, int i2 = -1, int i3 = -1)
    {
        KNode n; n.op = KOp::Swizzle; n.shape = t(v).shape; n.a = v;
        int w = 0; const int idx[4] = {i0, i1, i2, i3};
        for (int k = 0; k < 4; ++k) { if (idx[k] >= 0) { n.perm[k] = static_cast<crd::u8>(idx[k]); ++w; } }
        n.type = KType::vec(t(v).dtype(), w); // a 1-wide swizzle (.x) is a scalar
        return push(n);
    }
    // Matrices (column-major, stored as concatenated columns): matRxC has C columns of R rows, comps = R*C. Square
    // mat2/mat3/mat4 = 4/9/16 comps. NOTE mat2 and vec4 share comps==4 — only `KType::kind` tells them apart, which is
    // why the emitters key on the TYPE and never on the component count.
    [[nodiscard]] int mat2(int c0, int c1) { KNode n; n.op = KOp::MatFromCols; n.type = KType::mat(t(c0).dtype(), t(c0).comps(), 2); n.shape = t(c0).shape; n.a = c0; n.b = c1; return push(n); }
    [[nodiscard]] int mat3(int c0, int c1, int c2) { KNode n; n.op = KOp::MatFromCols; n.type = KType::mat(t(c0).dtype(), t(c0).comps(), 3); n.shape = t(c0).shape; n.a = c0; n.b = c1; n.c = c2; return push(n); } // GPU-constructible
    [[nodiscard]] int mat4(int c0, int c1, int c2, int c3) { KNode n; n.op = KOp::MatFromCols; n.type = KType::mat(t(c0).dtype(), t(c0).comps(), 4); n.shape = t(c0).shape; n.a = c0; n.b = c1; n.c = c2; n.d = c3; return push(n); } // GPU-constructible (4 vec4 columns)
    // (RxC) * vecC -> vecR · (RxK) * (KxC) -> (RxC). Square is the special case, not the assumption.
    [[nodiscard]] int mat_mul_vec(int m, int v) { KNode n; n.op = KOp::MatVecMul; n.type = KType::vec(t(v).dtype(), t(m).type.rows); n.shape = t(v).shape; n.a = m; n.b = v; return push(n); }
    [[nodiscard]] int mat_mul(int a, int b) { KNode n; n.op = KOp::MatMatMul; n.type = KType::mat(t(a).dtype(), t(a).type.rows, t(b).type.cols); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    // transpose SWAPS the dimensions (identity for the square mat3/mat4 of A3; the correct form once B0-2 lands mat2/non-square).
    [[nodiscard]] int mat_transpose(int m) { KNode n; n.op = KOp::MatTranspose; n.type = KType::mat(t(m).dtype(), t(m).type.cols, t(m).type.rows); n.shape = t(m).shape; n.a = m; return push(n); }
    // scalar → vecN broadcast (the enabler for scalar*vec, mix(vec,vec,scalar), etc.).
    [[nodiscard]] int splat(int a, int width) { KNode n; n.op = KOp::Splat; n.type = KType::vec(t(a).dtype(), width); n.shape = t(a).shape; n.a = a; return push(n); }
    // geometric (GLSL semantics). reflect(I,N)=I-2*dot(N,I)*N · refract(I,N,eta) · faceforward(N,I,Nref) · distance=|a-b|.
    [[nodiscard]] int reflect(int i, int nrm) { KNode n; n.op = KOp::Reflect; n.type = t(i).type; n.shape = t(i).shape; n.a = i; n.b = nrm; return push(n); }
    [[nodiscard]] int refract(int i, int nrm, int eta) { KNode n; n.op = KOp::Refract; n.type = t(i).type; n.shape = t(i).shape; n.a = i; n.b = nrm; n.c = eta; return push(n); }
    [[nodiscard]] int faceforward(int nrm, int i, int nref) { KNode n; n.op = KOp::Faceforward; n.type = t(nrm).type; n.shape = t(nrm).shape; n.a = nrm; n.b = i; n.c = nref; return push(n); }
    [[nodiscard]] int distance(int a, int b) { return vlength(binary(KOp::Sub, a, b)); }
    // relational reductions over the components → a scalar BOOL (any/all of a bvec, or of "componentwise != 0").
    [[nodiscard]] int vany(int v) { KNode n; n.op = KOp::VecAny; n.type = KType::make_scalar(DType::Bool); n.shape = t(v).shape; n.a = v; return push(n); }
    [[nodiscard]] int vall(int v) { KNode n; n.op = KOp::VecAll; n.type = KType::make_scalar(DType::Bool); n.shape = t(v).shape; n.a = v; return push(n); }
    // matrix: outer product (vecR ⊗ vecC → RxC mat, column-major) · determinant (→ scalar) · inverse.
    [[nodiscard]] int outer_product(int a, int b) { KNode n; n.op = KOp::OuterProduct; n.type = KType::mat(t(a).dtype(), t(a).comps(), t(b).comps()); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int determinant(int m) { KNode n; n.op = KOp::Determinant; n.type = KType::make_scalar(t(m).dtype()); n.shape = t(m).shape; n.a = m; return push(n); }
    [[nodiscard]] int mat_inverse(int m) { KNode n; n.op = KOp::MatInverse; n.type = t(m).type; n.shape = t(m).shape; n.a = m; return push(n); }
    // interpolation + quaternions (quat = vec4 (x,y,z,w), w = scalar). lerp = mix ✅; nlerp = normalize(mix).
    [[nodiscard]] int slerp(int a, int b, int tt) { KNode n; n.op = KOp::Slerp; n.type = t(a).type; n.shape = t(a).shape; n.a = a; n.b = b; n.c = tt; return push(n); }
    [[nodiscard]] int nlerp(int a, int b, int tt) { return normalize(ternary(KOp::Mix, a, b, splat(tt, t(a).comps()))); }
    [[nodiscard]] int quat_mul(int a, int b) { KNode n; n.op = KOp::QuatMul; n.type = KType::vec(t(a).dtype(), 4); n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int quat_conj(int q) { KNode n; n.op = KOp::QuatConj; n.type = KType::vec(t(q).dtype(), 4); n.shape = t(q).shape; n.a = q; return push(n); }
    [[nodiscard]] int quat_rotate(int q, int v) { KNode n; n.op = KOp::QuatRotate; n.type = KType::vec(t(q).dtype(), 3); n.shape = t(q).shape; n.a = q; n.b = v; return push(n); }
    [[nodiscard]] int quat_axis_angle(int axis, int angle) { KNode n; n.op = KOp::QuatAxisAngle; n.type = KType::vec(t(axis).dtype(), 4); n.shape = t(axis).shape; n.a = axis; n.b = angle; return push(n); }
    [[nodiscard]] int quat_to_mat3(int q) { KNode n; n.op = KOp::QuatToMat3; n.type = KType::mat(t(q).dtype(), 3, 3); n.shape = t(q).shape; n.a = q; return push(n); }
    // minor gaps
    [[nodiscard]] int bit_reverse(int a) { return unary(KOp::BitReverse, a); }
    [[nodiscard]] int ldexp(int m, int e) { return binary(KOp::Ldexp, m, e); }
    [[nodiscard]] int float_bits_to_int(int a) { KNode n; n.op = KOp::FloatBitsToInt; n.type = t(a).type.with_scalar(DType::I32); n.shape = t(a).shape; n.a = a; return push(n); }
    [[nodiscard]] int int_bits_to_float(int a) { KNode n; n.op = KOp::IntBitsToFloat; n.type = t(a).type.with_scalar(DType::F32); n.shape = t(a).shape; n.a = a; return push(n); }
    [[nodiscard]] int modf(int x) { KNode n; n.op = KOp::Modf; n.type = KType::vec(t(x).dtype(), 2); n.shape = t(x).shape; n.a = x; return push(n); } // → vec2(intpart, fracpart)
    // bitfieldInsert(base, insert, off, bits) = (base & ~mask) | ((insert<<off) & mask), mask = ((1<<bits)-1)<<off — composed.
    [[nodiscard]] int bitfield_insert(int base, int ins, int off, int bits)
    {
        const int one  = constant(1.0, t(base).shape, t(base).dtype());
        const int mask = binary(KOp::Shl, binary(KOp::Sub, binary(KOp::Shl, one, bits), one), off);
        const int keep = binary(KOp::BitAnd, base, unary(KOp::BitNot, mask));
        const int set  = binary(KOp::BitAnd, binary(KOp::Shl, ins, off), mask);
        return binary(KOp::BitOr, keep, set);
    }
    // ── B1 fragment derivatives ─────────────────────────────────────────────────────────────────────────────────────
    // Screen-space partial derivatives over the 2×2 fragment quad. Same type as the input (scalar or vecN). FRAGMENT-ONLY
    // (`entry_valid` refuses them elsewhere). Uses: mip/LoD selection, analytic anti-aliasing of procedural patterns,
    // screen-space normal reconstruction. `fwidth(a) = abs(dFdx(a)) + abs(dFdy(a))`, the standard filter-width estimate.
    [[nodiscard]] int dfdx(int a) { return unary(KOp::DFdx, a); }
    [[nodiscard]] int dfdy(int a) { return unary(KOp::DFdy, a); }
    [[nodiscard]] int fwidth(int a) { return unary(KOp::Fwidth, a); }

    // ── B3 raster leaves ────────────────────────────────────────────────────────────────────────────────────────────
    // A location-indexed stage input: a VERTEX ATTRIBUTE in a vertex entry, an INTERPOLANT in a fragment entry. One op
    // for both, disambiguated by the entry's stage — SPIR-V models it the same way (one `Input` storage class).
    [[nodiscard]] int stage_in(KType t, int location, Interp interp = Interp::Smooth)
    {
        KNode n;
        n.op    = KOp::StageIn;
        n.type  = t;
        n.shape = make_shape({1}); // a stage value is per-invocation; the tensor Shape carries no meaning here
        n.iidx  = location;
        n.dset  = static_cast<crd::u8>(interp); // B1-c: the interpolation qualifier (dset is unused for StageIn)
        return push(n);
    }
    // A per-stage builtin INPUT. Its type is fixed by the builtin, so callers cannot get it wrong. LEGALITY (is this
    // builtin readable in this stage?) is not knowable here — a graph has no stage; it is checked by `entry_valid`.
    [[nodiscard]] int builtin(KBuiltin b)
    {
        KNode n;
        n.op    = KOp::Builtin;
        n.type  = builtin_type(b);
        n.shape = make_shape({1});
        n.iidx  = static_cast<int>(b);
        return push(n);
    }
    // A uniform block at (set, binding): a STRUCT-typed leaf, whose members are read with `field_get`. This reuses the
    // B0-4 struct registry wholesale rather than inventing a second aggregate. `set` is ADR-0102's frequency slot:
    // 0 = per-frame (camera/time/env) · 1 = per-pass/lighting · 2 = per-material · 3 = per-object.
    [[nodiscard]] int uniform_block(int struct_id, int set, int binding)
    {
        KNode n;
        n.op    = KOp::UniformBlock;
        n.type  = struct_type(struct_id);
        n.shape = make_shape({1});
        n.iidx  = binding;
        n.dset  = static_cast<crd::u8>(set);
        return push(n);
    }

    // B1-f: read element `index` (a uint) of the fragment-shader storage buffer (set 0, binding 0). The write side is a
    // KEntry side-effect (`storage_write_*`), and `KEntry::interlock` makes the whole access rasterizer-ordered.
    [[nodiscard]] int storage_load(int index)
    {
        KNode n;
        n.op   = KOp::StorageLoad;
        n.type = KType::make_scalar(DType::U32);
        n.shape = make_shape({1});
        n.a    = index;
        return push(n);
    }

    // B2: an opaque TEXTURE binding at (set, binding). Separable — pair it with a `sampler` at the SAMPLE site (`tex_sample`).
    // B2-d: `array_count > 1` declares a BINDLESS descriptor ARRAY of that many textures (indexed by `tex_sample_at`).
    [[nodiscard]] int texture(int set, int binding, DType sampled = DType::F32, TexDim dim = TexDim::Tex2D,
                              bool arrayed = false, bool ms = false, bool shadow = false, int array_count = 1)
    {
        KNode n;
        n.op    = KOp::Texture;
        n.type  = KType::texture(sampled, dim, arrayed, ms, shadow);
        n.type.count = static_cast<crd::u16>(array_count < 1 ? 1 : array_count); // B2-d: descriptor-array length
        n.shape = make_shape({1});
        n.iidx  = binding;
        n.dset  = static_cast<crd::u8>(set);
        return push(n);
    }
    // B2-d: BINDLESS sample — sample element `index` (a dynamic uint) of a texture ARRAY through `samp` at `uv`.
    [[nodiscard]] int tex_sample_at(int tex, int samp, int uv, int index)
    {
        KNode n;
        n.op    = KOp::SampleIndexed;
        n.type  = KType::vec(t(tex).type.scalar, 4);
        n.shape = t(uv).shape;
        n.a     = tex;
        n.b     = samp;
        n.c     = uv;
        n.d     = index;
        return push(n);
    }
    // B16: BINDLESS + EXPLICIT-LOD sample — element `index` of a texture ARRAY through `samp` at `uv`, at an explicit mip `lod`.
    // Legal in ANY stage (no derivatives) — this is the VERTEX-stage displacement fetch (bindless cascades, explicit LOD).
    [[nodiscard]] int tex_sample_at_lod(int tex, int samp, int uv, int index, int lod)
    {
        KNode n;
        n.op    = KOp::SampleIndexedLod;
        n.type  = KType::vec(t(tex).type.scalar, 4);
        n.shape = t(uv).shape;
        n.a     = tex;
        n.b     = samp;
        n.c     = uv;
        n.d     = index;
        const int e[1] = {lod}; n.ext = push_ext(e, 1); n.n_ext = 1U;
        return push(n);
    }
    // B2: an opaque SAMPLER binding at (set, binding). `shadow` ⇒ a comparison sampler (for `sampleCmp`, B2-b).
    [[nodiscard]] int sampler(int set, int binding, bool shadow = false)
    {
        KNode n;
        n.op    = KOp::Sampler;
        n.type  = KType::sampler(shadow);
        n.shape = make_shape({1});
        n.iidx  = binding;
        n.dset  = static_cast<crd::u8>(set);
        return push(n);
    }
    // B2: sample `tex` through `samp` at coordinate `uv` — implicit-LOD (uses fragment derivatives ⇒ fragment-stage only).
    // Returns a vec4 of the texture's sampled component type (GLSL/HLSL always widen a sample to 4 components).
    [[nodiscard]] int tex_sample(int tex, int samp, int uv)
    {
        KNode n;
        n.op    = KOp::TexSample;
        n.type  = KType::vec(t(tex).type.scalar, 4);
        n.shape = t(uv).shape;
        n.a     = tex;
        n.b     = samp;
        n.c     = uv;
        return push(n);
    }
    // B2-b: EXPLICIT-LOD sample — legal in ANY stage (no derivatives). `lod` is a float mip level.
    [[nodiscard]] int tex_sample_lod(int tex, int samp, int uv, int lod)
    {
        KNode n;
        n.op = KOp::SampleLod; n.type = KType::vec(t(tex).type.scalar, 4); n.shape = t(uv).shape;
        n.a = tex; n.b = samp; n.c = uv; n.d = lod;
        return push(n);
    }
    // B2-b: EXPLICIT-GRADIENT sample — `ddx`/`ddy` are the UV derivatives that pick the mip (ddy rides the ext pool).
    [[nodiscard]] int tex_sample_grad(int tex, int samp, int uv, int ddx, int ddy)
    {
        KNode n;
        n.op = KOp::SampleGrad; n.type = KType::vec(t(tex).type.scalar, 4); n.shape = t(uv).shape;
        n.a = tex; n.b = samp; n.c = uv; n.d = ddx;
        const int e[1] = {ddy}; n.ext = push_ext(e, 1); n.n_ext = 1U;
        return push(n);
    }
    // B2-b: DEPTH-COMPARE (shadow) sample — `tex` is a depth texture, `samp` a comparison sampler; returns the PCF result
    // (0..1) of comparing `ref` against the stored depth at `uv`. A scalar float.
    [[nodiscard]] int tex_sample_cmp(int tex, int samp, int uv, int ref)
    {
        KNode n;
        n.op = KOp::SampleCmp; n.type = KType::make_scalar(DType::F32); n.shape = t(uv).shape;
        n.a = tex; n.b = samp; n.c = uv; n.d = ref;
        return push(n);
    }
    // B2-b: INTEGER texel fetch — no filtering; `coord` is an integer texel coordinate, `lod` the mip level. `samp` is
    // referenced only to form GLSL's combined `sampler2D(t,s)` (texelFetch has a sampler-typed argument there); HLSL's
    // `.Load` ignores it. Any sampler bound to the same set works.
    [[nodiscard]] int tex_fetch(int tex, int samp, int coord, int lod)
    {
        KNode n;
        n.op = KOp::TexelFetch; n.type = KType::vec(t(tex).type.scalar, 4); n.shape = t(coord).shape;
        n.a = tex; n.b = samp; n.c = coord; n.d = lod;
        return push(n);
    }
    // B2-b: 4-TEXEL GATHER — the `comp`-th channel (0..3) of the 4 texels around `uv` (bilinear footprint), as a vec4.
    [[nodiscard]] int tex_gather(int tex, int samp, int uv, int comp)
    {
        KNode n;
        n.op = KOp::TexGather; n.type = KType::vec(t(tex).type.scalar, 4); n.shape = t(uv).shape;
        n.a = tex; n.b = samp; n.c = uv; n.d = comp;
        return push(n);
    }
    // B2-b: texture SIZE at `lod` — an ivec matching the texture's spatial dims (2D ⇒ ivec2). `samp` forms GLSL's combined
    // `textureSize(sampler2D(t,s), lod)`; HLSL's `.GetDimensions` ignores it.
    [[nodiscard]] int tex_size(int tex, int samp, int lod)
    {
        const TexDim dim = t(tex).type.tex_dim();
        int          nc  = 2;
        if (dim == TexDim::Tex3D) { nc = 3; }
        else if (dim == TexDim::Tex1D) { nc = 1; }
        KNode        n;
        n.op = KOp::TexSize; n.type = KType::vec(DType::I32, nc); n.shape = make_shape({1});
        n.a = tex; n.b = samp; n.d = lod;
        return push(n);
    }

    // ── B0-4 aggregates ─────────────────────────────────────────────────────────────────────────────────────────────
    // A struct/array VALUE is a contiguous run of components (fields/elements back to back) — the same flat storage
    // vec/mat already use. `StructMake`/`ArrayMake` take N operands from the ext pool; `FieldGet`/`ArrayGet` slice one
    // field/element out (index in `iidx`). The GPU emitters lower these by SROA — the aggregate is never materialized,
    // and a `FieldGet` resolves straight to the field's temp — which is exactly what Slang and DXC do.
    [[nodiscard]] int struct_make(int struct_id, const int* fields, int n_fields)
    {
        CRD_ASSERT(n_fields == struct_field_count(struct_id));
        KNode n;
        n.op    = KOp::StructMake;
        n.type  = struct_type(struct_id);
        n.shape = t(fields[0]).shape;
        n.ext   = push_ext(fields, n_fields);
        n.n_ext = static_cast<crd::u16>(n_fields);
        return push(n);
    }
    // an array of `n_elems` values, all of the same element type.
    [[nodiscard]] int array_make(const int* elems, int n_elems)
    {
        KNode n;
        n.op    = KOp::ArrayMake;
        n.type  = KType::array_of(t(elems[0]).type, n_elems);
        n.shape = t(elems[0]).shape;
        n.ext   = push_ext(elems, n_elems);
        n.n_ext = static_cast<crd::u16>(n_elems);
        return push(n);
    }
    [[nodiscard]] int field_get(int s, int field_idx)
    {
        KNode n;
        n.op    = KOp::FieldGet;
        n.type  = struct_field(t(s).type.struct_id, field_idx);
        n.shape = t(s).shape;
        n.a     = s;
        n.iidx  = field_idx;
        return push(n);
    }
    // constant-index element read. A DYNAMIC index needs a real array in memory, not an SSA value — that is a
    // buffer-backed access (B3), not a value-layer one.
    [[nodiscard]] int array_get(int arr, int elem_idx)
    {
        KNode n;
        n.op    = KOp::ArrayGet;
        n.type  = t(arr).type;
        n.type.count = 1; // one element of the array
        n.shape = t(arr).shape;
        n.a     = arr;
        n.iidx  = elem_idx;
        return push(n);
    }

    // GLM-style transform matrices — pure COMPOSITION of the mat4/vec4 primitives (column-major). translate/scale shown;
    // rotate = mat4-from-quat, perspective/ortho/lookAt = the same pattern (consumer/engine-math level, add on demand).
    [[nodiscard]] int translate(int tx, int ty, int tz)
    {
        const int z = constant(0.0, t(tx).shape, t(tx).dtype());
        const int o = constant(1.0, t(tx).shape, t(tx).dtype());
        return mat4(vec4(o, z, z, z), vec4(z, o, z, z), vec4(z, z, o, z), vec4(tx, ty, tz, o));
    }
    [[nodiscard]] int scale(int sx, int sy, int sz)
    {
        const int z = constant(0.0, t(sx).shape, t(sx).dtype());
        const int o = constant(1.0, t(sx).shape, t(sx).dtype());
        return mat4(vec4(sx, z, z, z), vec4(z, sy, z, z), vec4(z, z, sz, z), vec4(z, z, z, o));
    }

    // The TENSOR layer (reduce / movement / contraction / indexing) operates on tensors of SCALARS: the element type is
    // always scalar, the multi-dimensionality lives in `Shape`. (Per-element vec/mat values are the A3 VALUE layer above.)
    // reduce over the axes in `mask` (keepdims: reduced axes become 1). `tier` selects the determinism tier: Exact (T1,
    // bit-exact fixed order — default) or Fast (T2, parallel workgroup tree-reduce — reordered, RFA, run-to-run stable).
    [[nodiscard]] int reduce(KOp op, int a, crd::u32 mask, DetTier tier = DetTier::Exact)
    {
        KNode n; n.op = op; n.type = KType::make_scalar(t(a).dtype()); n.a = a; n.axes = mask; n.shape = t(a).shape; n.tier = tier;
        for (int i = 0; i < n.shape.rank; ++i) { if ((mask >> i) & 1U) { n.shape.dims[i] = 1; } }
        return push(n);
    }
    [[nodiscard]] int reshape(int a, const Shape& out) { KNode n; n.op = KOp::Reshape; n.type = KType::make_scalar(t(a).dtype()); n.shape = out; n.a = a; return push(n); }
    [[nodiscard]] int permute(int a, const crd::u8* p)
    {
        KNode n; n.op = KOp::Permute; n.type = KType::make_scalar(t(a).dtype()); n.a = a; n.shape.rank = t(a).shape.rank;
        for (int i = 0; i < n.shape.rank; ++i) { n.perm[i] = p[i]; n.shape.dims[i] = t(a).shape.dims[p[i]]; }
        return push(n);
    }
    [[nodiscard]] int broadcast(int a, const Shape& out) { KNode n; n.op = KOp::Broadcast; n.type = KType::make_scalar(t(a).dtype()); n.shape = out; n.a = a; return push(n); }
    // batched matmul: a[...,M,K], b[...,K,N] -> [...,M,N] (leading batch dims must match). `tier` selects determinism:
    // Exact (T1, `precise`/no-FMA fixed-order — bit-exact vs the CPU oracle, default) or Fast (T2, FMA + tiled schedule —
    // run-to-run deterministic, matches the FMA-tier oracle; the ported crush kernel).
    [[nodiscard]] int contract(int a, int b, DetTier tier = DetTier::Exact)
    {
        KNode n; n.op = KOp::Contract; n.type = KType::make_scalar(t(a).dtype()); n.a = a; n.b = b; n.tier = tier;
        const Shape& sa = t(a).shape;
        n.shape = sa;
        n.shape.dims[sa.rank - 1] = t(b).shape.dims[t(b).shape.rank - 1]; // N
        return push(n);
    }
    // row-gather along axis 0: data[R, trailing...], idx[M] (integer values, f32-encoded) -> out[M, trailing...] where
    // out[m, ...] = data[idx[m], ...]. The embedding-lookup / index-select pattern.
    [[nodiscard]] int gather(int data, int idx)
    {
        KNode n; n.op = KOp::Gather; n.type = KType::make_scalar(t(data).dtype()); n.a = data; n.b = idx;
        n.shape          = t(data).shape;
        n.shape.dims[0]  = t(idx).shape.dims[0]; // R -> M (leading axis becomes the index count)
        return push(n);
    }
    // row-scatter along axis 0 (deterministic LAST-WINS): out = base[R, trailing...], then out[idx[m], ...] = updates[m,
    // ...] for m=0..M-1 in order (a later m overrides an earlier one at the same index). The write-side inverse of gather.
    [[nodiscard]] int scatter(int base, int idx, int updates)
    {
        KNode n; n.op = KOp::Scatter; n.type = KType::make_scalar(t(base).dtype()); n.a = base; n.b = idx; n.c = updates;
        n.shape = t(base).shape; // output has the SAME shape as base
        return push(n);
    }
    // atomic scatter-ADD (histogram): out has shape `bins` (M elements), zero-initialized, then out[idx[i]] += updates[i]
    // for i=0..N-1. INTEGER accumulation ⇒ order-independent ⇒ deterministic + bit-exact even though the GPU uses
    // atomics (the determinism moat survives). The building block of the radix histogram / counting sort.
    [[nodiscard]] int scatter_add(int idx, int updates, const Shape& bins)
    {
        KNode n; n.op = KOp::ScatterAdd; n.type = KType::make_scalar(t(updates).dtype()); n.a = idx; n.b = updates; n.shape = bins;
        return push(n);
    }
    // inclusive prefix-sum along the TRAILING axis (out[..., c] = sum of a[..., 0..c]); keeps the input shape. Fixed
    // ascending order per row ⇒ deterministic + bit-exact vs the naive f32 GPU scan.
    [[nodiscard]] int scan(int a, DetTier tier = DetTier::Exact) { KNode n; n.op = KOp::ScanSum; n.type = KType::make_scalar(t(a).dtype()); n.a = a; n.shape = t(a).shape; n.tier = tier; return push(n); }

    [[nodiscard]] int          size() const noexcept { return static_cast<int>(m_nodes.size()); }
    [[nodiscard]] int          n_inputs() const noexcept { return m_ninput; }
    [[nodiscard]] const KNode& node(int i) const noexcept { return m_nodes[static_cast<crd::usize>(i)]; }

    // Structural invariant: every operand references a STRICTLY EARLIER node, because `push()` appends and an operand
    // always exists before its consumer — push order IS topological order. A pass that compacts and renumbers the array
    // must remap all FOUR operands; one left behind surfaces here as a forward or out-of-range reference.
    [[nodiscard]] bool operands_valid() const noexcept
    {
        for (int i = 0; i < size(); ++i)
        {
            const KNode&   n      = node(i);
            const crd::i32 ops[4] = {n.a, n.b, n.c, n.d};
            for (const crd::i32 o : ops)
            {
                if (o == -1) { continue; }
                if (o < 0 || o >= i) { return false; }
            }
            // the VARIADIC operands are operands too — the `d`-remap bug, one field further out
            for (int k = 0; k < static_cast<int>(n.n_ext); ++k)
            {
                const crd::i32 o = ext_operand(n, k);
                if (o < 0 || o >= i) { return false; }
            }
        }
        return true;
    }

    // Append a COPY of an already-validated node (op/shape/dtype/axes preserved; caller remaps a/b/c to this graph's ids).
    // For the multi-kernel scheduler's per-kernel mini-graphs — it clones a subgraph, so no shape/dtype re-inference.
    // A node with VARIADIC operands cannot be cloned this way: `ext` indexes the SOURCE graph's pool, and copying the
    // offset verbatim would silently read someone else's operands (the B0-0 bug, one field further out). Use
    // `clone_with_ext` so the operand list is re-pushed into THIS graph.
    [[nodiscard]] int clone(const KNode& n)
    {
        CRD_ASSERT_MSG(n.n_ext == 0, "clone() cannot carry variadic operands across graphs - use clone_with_ext()");
        return push(n);
    }
    // Clone a variadic node, copying its operand list from `src_ext` into this graph's pool (caller remaps the ids).
    [[nodiscard]] int clone_with_ext(const KNode& n, const int* src_ext)
    {
        KNode c = n;
        c.ext   = push_ext(src_ext, static_cast<int>(n.n_ext));
        return push(c);
    }

    // B7 SPECIALIZATION primitive: pin a node (a `ShaderOption` selector — a uniform flag / option leaf) to a compile-time
    // constant, IN PLACE (every consumer now sees the constant). A `Select`/switch reading it then const-folds under
    // `optimize()` and DCE drops the dead branch → a variant. Type + shape are preserved (an option is scalar); the pinned
    // node's former operands become unreferenced and DCE reclaims them. Destructive — build/copy the graph per variant.
    void pin_const(int node, crd::f64 value)
    {
        KNode&      n  = m_nodes[static_cast<crd::usize>(node)];
        const Shape sh = n.shape;
        const KType ty = n.type;
        n       = KNode{};
        n.op    = KOp::Const;
        n.shape = sh;
        n.type  = ty;
        n.cval  = value;
    }

    // B7 branch-elimination primitive: make node `node` an exact copy of `target` (every consumer now reads target's
    // computation). Used to collapse a `Select` with a compile-time-constant condition to its chosen branch. `target` is
    // always an operand of `node` (a/b), hence < `node` in topological order, so the copied operands stay backward
    // references; CSE then merges the duplicate and DCE reclaims the dead branch.
    void alias(int node, int target) noexcept { m_nodes[static_cast<crd::usize>(node)] = m_nodes[static_cast<crd::usize>(target)]; }

    // const-fold -> DCE (reachability from roots) -> CSE (hash-cons). Updates roots[] to their new ids. Semantics-
    // preserving + idempotent. (Kernel FUSION is a CKIR-Tile pass — v17-b — where kernels exist; not this level.)
    void optimize(int* roots, int n_roots)
    {
        const int n  = size();
        auto*     al = m_nodes.allocator();
        crd::containers::Array<crd::f64> cval(al);
        crd::containers::Array<crd::u8>  isc(al);
        cval.resize(static_cast<crd::usize>(n), 0.0);
        isc.resize(static_cast<crd::usize>(n), 0);
        for (int i = 0; i < n; ++i) // const-fold in place (Const is a uniform fill, so foldable ops track one value)
        {
            KNode& g = m_nodes[static_cast<crd::usize>(i)];
            if (g.op == KOp::Const) { isc[static_cast<crd::usize>(i)] = 1; cval[static_cast<crd::usize>(i)] = g.cval; continue; }
            if (g.op == KOp::Input || g.op == KOp::Iota || g.op == KOp::Contract || g.op == KOp::For || g.op == KOp::LoopIndex || g.op == KOp::LoopAcc) { continue; }
            if (is_stage_leaf(g.op)) { continue; } // B3 leaves have no operands — a SCALAR one (Builtin::VertexIndex, a
                                                  // scalar StageIn) would otherwise const-fold into a compile-time value
            if (g.n_ext != 0 || g.op == KOp::FieldGet || g.op == KOp::ArrayGet) { continue; } // aggregates never fold to one scalar Const
            if (g.comps() != 1) { continue; } // vec/mat values fold to multiple components — never a single scalar Const
            const bool ac = g.a < 0 || isc[static_cast<crd::usize>(g.a)];
            const bool bc = g.b < 0 || isc[static_cast<crd::usize>(g.b)];
            const bool cc = g.c < 0 || isc[static_cast<crd::usize>(g.c)];
            if (!(ac && bc && cc)) { continue; }
            const crd::f64 av = g.a >= 0 ? cval[static_cast<crd::usize>(g.a)] : 0.0;
            const crd::f64 bv = g.b >= 0 ? cval[static_cast<crd::usize>(g.b)] : 0.0;
            crd::f64       r  = 0.0;
            if (g.op == KOp::Select) { r = (g.c >= 0 && cval[static_cast<crd::usize>(g.c)] != 0.0) ? av : bv; }
            else if (g.op == KOp::Cast) { r = round_dtype(av, g.dtype()); }
            // movement (Reshape/Permute/Broadcast) + ReduceMax of a uniform fill are all identity on the value
            else if (g.op == KOp::Reshape || g.op == KOp::Permute || g.op == KOp::Broadcast || g.op == KOp::ReduceMax) { r = av; }
            else if (g.op == KOp::ReduceSum) { crd::i64 c = 1; const Shape& sa = m_nodes[static_cast<crd::usize>(g.a)].shape; for (int k = 0; k < sa.rank; ++k) { if ((g.axes >> k) & 1U) { c *= sa.dims[k]; } } r = av * static_cast<crd::f64>(c); }
            else if (g.b >= 0) { r = apply_binary_typed(g.op, av, bv, g.dtype()); }
            else { r = apply_unary(g.op, av); }
            const Shape sh = g.shape;
            const KType ty = g.type;
            g = KNode{};
            g.op = KOp::Const; g.shape = sh; g.type = ty; g.cval = r;
            isc[static_cast<crd::usize>(i)] = 1; cval[static_cast<crd::usize>(i)] = r;
        }
        crd::containers::Array<crd::u8> keep(al);
        crd::containers::Array<int>     stk(al);
        keep.resize(static_cast<crd::usize>(n), 0);
        for (int r = 0; r < n_roots; ++r) { stk.push_back(roots[r]); }
        while (stk.size() > 0)
        {
            const int i = stk[stk.size() - 1];
            stk.resize(stk.size() - 1);
            if (keep[static_cast<crd::usize>(i)]) { continue; }
            keep[static_cast<crd::usize>(i)] = 1;
            const KNode& g = m_nodes[static_cast<crd::usize>(i)];
            if (g.a >= 0) { stk.push_back(g.a); }
            if (g.b >= 0) { stk.push_back(g.b); }
            if (g.c >= 0) { stk.push_back(g.c); }
            if (g.d >= 0) { stk.push_back(g.d); }
            for (int k = 0; k < static_cast<int>(g.n_ext); ++k) { stk.push_back(ext_operand(g, k)); } // variadic operands keep their fields alive
        }
        int cap = 1;
        while (cap < 2 * n + 4) { cap <<= 1; }
        crd::containers::Array<int>      table(al);
        crd::containers::Array<int>      newid(al);
        crd::containers::Array<KNode>    nn(al);
        crd::containers::Array<crd::i32> nx(al); // the rebuilt ext pool — old offsets are meaningless after compaction
        table.resize(static_cast<crd::usize>(cap), -1);
        newid.resize(static_cast<crd::usize>(n), -1);
        for (int i = 0; i < n; ++i)
        {
            if (!keep[static_cast<crd::usize>(i)]) { continue; }
            KNode g = m_nodes[static_cast<crd::usize>(i)];
            if (g.a >= 0) { g.a = newid[static_cast<crd::usize>(g.a)]; }
            if (g.b >= 0) { g.b = newid[static_cast<crd::usize>(g.b)]; }
            if (g.c >= 0) { g.c = newid[static_cast<crd::usize>(g.c)]; }
            if (g.d >= 0) { g.d = newid[static_cast<crd::usize>(g.d)]; } // 4th operand (mat4 column) — renumbered like the rest
            if (g.n_ext != 0)
            {
                const int noff = static_cast<int>(nx.size());
                for (int k = 0; k < static_cast<int>(g.n_ext); ++k) { nx.push_back(newid[static_cast<crd::usize>(m_ext[static_cast<crd::usize>(g.ext) + static_cast<crd::usize>(k)])]); }
                g.ext = noff;
            }
            newid[static_cast<crd::usize>(i)] = intern(table, cap, nn, g, nx);
        }
        m_nodes = static_cast<crd::containers::Array<KNode>&&>(nn);
        m_ext   = static_cast<crd::containers::Array<crd::i32>&&>(nx);
        for (int r = 0; r < n_roots; ++r) { roots[r] = newid[static_cast<crd::usize>(roots[r])]; }
        CRD_ASSERT(operands_valid());
    }

private:
    int push(const KNode& n) { m_nodes.push_back(n); return static_cast<int>(m_nodes.size()) - 1; }
    [[nodiscard]] const KNode& t(int i) const noexcept { return m_nodes[static_cast<crd::usize>(i)]; }
    int push_ext(const int* ops, int n)
    {
        const int off = static_cast<int>(m_ext.size());
        for (int i = 0; i < n; ++i) { m_ext.push_back(ops[i]); }
        return off;
    }

    [[nodiscard]] static crd::u64 key_hash(const KNode& g, const crd::containers::Array<crd::i32>& pool) noexcept
    {
        crd::u64 h = 1469598103934665603ULL;
        const auto mix = [&h](crd::u64 v) { h ^= v; h *= 1099511628211ULL; };
        mix(static_cast<crd::u64>(g.op));
        // the FULL type participates in the key — two values that differ only in scalar type or form (ivec3 vs vec3,
        // vec4 vs mat2, Light vs Material) must never hash-cons together.
        mix(static_cast<crd::u64>(g.type.scalar));
        mix(static_cast<crd::u64>(g.type.kind));
        mix(static_cast<crd::u64>(g.type.rows));
        mix(static_cast<crd::u64>(g.type.cols));
        mix(static_cast<crd::u64>(g.type.count));
        mix(static_cast<crd::u64>(static_cast<crd::u16>(g.type.struct_id)));
        mix(static_cast<crd::u64>(g.type.elem_comps));
        // hash the variadic operand VALUES, never the pool offset — two identical StructMakes sit at different offsets.
        mix(static_cast<crd::u64>(g.n_ext));
        for (int k = 0; k < static_cast<int>(g.n_ext); ++k) { mix(static_cast<crd::u64>(static_cast<crd::u32>(pool[static_cast<crd::usize>(g.ext) + static_cast<crd::usize>(k)]))); }
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.a)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.b)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.c)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.d)));
        crd::u64 cb = 0;
        std::memcpy(&cb, &g.cval, sizeof(cb));
        mix(cb);
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.iidx)));
        mix(static_cast<crd::u64>(g.dset)); // B3: two UniformBlocks at the same binding but different SETS are distinct
        mix(static_cast<crd::u64>(g.axes));
        mix(static_cast<crd::u64>(g.shape.rank));
        for (int k = 0; k < g.shape.rank; ++k) { mix(static_cast<crd::u64>(g.shape.dims[k])); mix(static_cast<crd::u64>(g.perm[k])); }
        return h;
    }
    [[nodiscard]] static bool node_equal(const KNode& x, const KNode& y, const crd::containers::Array<crd::i32>& pool) noexcept
    {
        if (x.op == KOp::For || x.op == KOp::LoopIndex || x.op == KOp::LoopAcc) { return false; } // never CSE loop constructs — operandless leaves belong to a specific loop
        if (x.op != y.op || !(x.type == y.type) || x.a != y.a || x.b != y.b || x.c != y.c || x.d != y.d || x.iidx != y.iidx || x.dset != y.dset || x.axes != y.axes) { return false; }
        if (x.cval != y.cval || !(x.shape == y.shape)) { return false; }
        for (int k = 0; k < x.shape.rank; ++k) { if (x.perm[k] != y.perm[k]) { return false; } }
        if (x.n_ext != y.n_ext) { return false; }
        for (int k = 0; k < static_cast<int>(x.n_ext); ++k)
        {
            const crd::usize xk = static_cast<crd::usize>(x.ext) + static_cast<crd::usize>(k);
            const crd::usize yk = static_cast<crd::usize>(y.ext) + static_cast<crd::usize>(k);
            if (pool[xk] != pool[yk]) { return false; }
        }
        return true;
    }
    static int intern(crd::containers::Array<int>& table, int cap, crd::containers::Array<KNode>& nn, const KNode& g,
                      const crd::containers::Array<crd::i32>& pool)
    {
        const int mask = cap - 1;
        int       slot = static_cast<int>(key_hash(g, pool) & static_cast<crd::u64>(mask));
        while (table[static_cast<crd::usize>(slot)] >= 0)
        {
            if (node_equal(nn[static_cast<crd::usize>(table[static_cast<crd::usize>(slot)])], g, pool)) { return table[static_cast<crd::usize>(slot)]; }
            slot = (slot + 1) & mask;
        }
        const int id = static_cast<int>(nn.size());
        nn.push_back(g);
        table[static_cast<crd::usize>(slot)] = id;
        return id;
    }

    crd::containers::Array<KNode>    m_nodes;
    crd::containers::Array<crd::i32> m_ext;     // B0-4: flat variadic-operand pool
    crd::containers::Array<KType>    m_sfields; // struct registry: field types, CSR-packed
    crd::containers::Array<crd::u32> m_sbegin;  // struct registry: first field of struct id
    crd::containers::Array<KStmt>    m_stmts;   // B-cmp: the imperative compute-kernel statement pool
    int                              m_ninput = 0;
};

// ── B3-a': stage/entry VALIDATION ────────────────────────────────────────────────────────────────────────────────────
// A `KGraph` carries no stage, so `builtin()` cannot reject `gl_FragCoord` in a vertex shader — only an ENTRY knows the
// stage. This is the check that makes the per-stage builtin table load-bearing rather than decorative: without it the
// table is a comment. Backends call it before emitting; the emitted source then compiles by construction.
//
// `why` (optional) receives a static reason string — worth threading, because "the graph is invalid" is not a diagnosis.
[[nodiscard]] inline bool entry_valid(const KGraph& g, const KEntry& e, const char** why = nullptr)
{
    const auto fail = [&](const char* reason) {
        if (why != nullptr) { *why = reason; }
        return false;
    };

    const int n = g.size();
    const auto node_ok = [&](int id) { return id >= 0 && id < n; };

    // Position: required exactly where the stage produces one. A compute or fragment entry with a `position` is a
    // mis-built graph, not a harmless extra — it means the author thought they were writing a vertex stage.
    if (stage_writes_position(e.stage))
    {
        if (!node_ok(e.position)) { return fail("stage must write `position` (clip-space vec4)"); }
        if (g.node(e.position).type != KType::vec(DType::F32, 4)) { return fail("`position` must be a vec4"); }
    }
    else if (e.position >= 0) { return fail("stage does not write `position`"); }

    if (e.frag_depth >= 0)
    {
        if (!stage_writes_frag_depth(e.stage)) { return fail("only a fragment stage writes `frag_depth`"); }
        if (!node_ok(e.frag_depth)) { return fail("`frag_depth` names no node"); }
        if (g.node(e.frag_depth).type != KType::make_scalar(DType::F32)) { return fail("`frag_depth` must be a float"); }
    }

    if (e.discard_cond >= 0) // B1-b: alpha-test / cutout — a bool the fragment discards on
    {
        if (e.stage != KStage::Fragment) { return fail("only a fragment stage can `discard`"); }
        if (!node_ok(e.discard_cond)) { return fail("`discard_cond` names no node"); }
        if (g.node(e.discard_cond).type != KType::make_scalar(DType::Bool)) { return fail("`discard_cond` must be a bool"); }
    }

    if (e.early_fragment_tests) // B1-d: force the depth/stencil test before the fragment shader runs
    {
        if (e.stage != KStage::Fragment) { return fail("only a fragment stage can force `early_fragment_tests`"); }
        // The early test uses the INTERPOLATED depth, so a shader depth write would be meaningless — refuse the combo.
        if (e.frag_depth >= 0) { return fail("`early_fragment_tests` cannot coexist with a `frag_depth` write"); }
    }
    // B1-d: a conservative-depth promise only means something for a shader that actually writes depth.
    if (e.depth_mode != DepthMode::Any && e.frag_depth < 0)
    {
        return fail("a conservative `depth_mode` requires a `frag_depth` write");
    }

    if (e.shading_rate >= 0) // B1-e: per-primitive variable-rate-shading output (gl_PrimitiveShadingRateEXT / SV_ShadingRate)
    {
        if (!stage_writes_position(e.stage)) { return fail("only a position-writing stage can output a `shading_rate`"); }
        if (!node_ok(e.shading_rate)) { return fail("`shading_rate` names no node"); }
        const DType sr = g.node(e.shading_rate).type.scalar;
        if (g.node(e.shading_rate).type.kind != TKind::Scalar || (sr != DType::I32 && sr != DType::U32))
        {
            return fail("`shading_rate` must be an int/uint scalar (a packed VRS rate)");
        }
    }

    if (e.storage_write_index >= 0 || e.storage_write_value >= 0) // B1-f: a fragment storage-buffer write
    {
        if (e.stage != KStage::Fragment) { return fail("only a fragment stage can write the storage buffer"); }
        if (!node_ok(e.storage_write_index) || !node_ok(e.storage_write_value)) { return fail("`storage_write` names no node"); }
        if (g.node(e.storage_write_index).type != KType::make_scalar(DType::U32)) { return fail("storage write index must be uint"); }
        if (g.node(e.storage_write_value).type != KType::make_scalar(DType::U32)) { return fail("storage write value must be uint"); }
    }
    if (e.interlock && e.stage != KStage::Fragment) { return fail("only a fragment stage can use `interlock`"); }

    // B4: a MESH entry emits `mesh_primitives` triangles; `mesh_prim` is the uvec3 of LOCAL vertex indices for primitive tid.
    if (e.stage == KStage::Mesh)
    {
        if (e.mesh_vertices == 0U) { return fail("a mesh entry must set mesh_vertices > 0"); }
        if (e.mesh_primitives == 0U) { return fail("a mesh entry must set mesh_primitives > 0"); }
        if (!node_ok(e.mesh_prim)) { return fail("a mesh entry must set `mesh_prim` (uvec3 of local vertex indices)"); }
        if (g.node(e.mesh_prim).type != KType::vec(DType::U32, 3)) { return fail("`mesh_prim` must be a uvec3"); }
    }
    else if (e.mesh_prim >= 0 || e.mesh_vertices > 0U) { return fail("`mesh_*` fields are only for a mesh stage"); }

    // B4: a TASK / amplification entry computes `task_emit` (the mesh-workgroup count it launches) + an optional single-uint
    // `task_payload`. It emits NO geometry (no position / out / mesh_*), so it renders nothing itself — it drives the mesh.
    if (e.stage == KStage::Task)
    {
        if (!node_ok(e.task_emit)) { return fail("a task entry must set `task_emit` (u32 mesh-workgroup count)"); }
        if (g.node(e.task_emit).type != KType::make_scalar(DType::U32)) { return fail("`task_emit` must be a uint"); }
        if (e.n_task_payload > static_cast<crd::u32>(KEntry::kMaxTaskPayload)) { return fail("at most 4 payload fields"); }
        for (crd::u32 i = 0; i < e.n_task_payload; ++i) // every active payload field must be a written uint node
        {
            if (!node_ok(e.task_payload[i]) || g.node(e.task_payload[i]).type != KType::make_scalar(DType::U32))
            {
                return fail("each `task_payload` field must be a uint");
            }
        }
        if (e.position >= 0 || e.n_out > 0) { return fail("a task entry emits no geometry (no position/out)"); }
    }
    else if (e.task_emit >= 0 || e.n_task_payload > 0U) { return fail("`task_*` fields are only for a task stage"); }

    // B4-tess: a TESSELLATION entry. TessControl (hull) sets the tess levels (float `tess_inner`/`tess_outer`) + passes the
    // control points; TessEval (domain) writes the displaced clip `position` (checked by the position path below).
    if (e.stage == KStage::TessControl)
    {
        if (e.tess_patch_size == 0U) { return fail("a tess-control entry must set tess_patch_size > 0"); }
        if (!node_ok(e.tess_inner) || !node_ok(e.tess_outer)) { return fail("a tess-control entry needs tess_inner + tess_outer"); }
        if (g.node(e.tess_inner).type != KType::make_scalar(DType::F32)
            || g.node(e.tess_outer).type != KType::make_scalar(DType::F32))
        {
            return fail("tess levels must be float");
        }
    }
    else if (e.stage == KStage::TessEval)
    {
        if (e.tess_patch_size == 0U) { return fail("a tess-eval entry must set tess_patch_size > 0"); }
    }
    else if (e.tess_patch_size > 0U || e.tess_inner >= 0 || e.tess_outer >= 0)
    {
        return fail("`tess_*` fields are only for a tessellation stage");
    }

    if (e.n_out < 0 || e.n_out > kMaxStageOutputs) { return fail("output count out of range"); }
    for (int i = 0; i < e.n_out; ++i)
    {
        if (!node_ok(e.out[i].node)) { return fail("output names no node"); }
        if (e.out[i].location < 0) { return fail("output location is negative"); }
        // B1-c: an integer VS interpolant cannot be smoothly interpolated — it must be `flat`.
        if (e.stage == KStage::Vertex && requires_flat_interp(g.node(e.out[i].node).type) && e.out[i].interp != Interp::Flat)
        {
            return fail("an integer VS interpolant must be `flat`");
        }
        for (int j = 0; j < i; ++j)
        {
            if (e.out[j].location == e.out[i].location) { return fail("two outputs share one location"); }
        }
    }

    // Every builtin READ by the graph must be legal in this stage, and every location-indexed input distinct.
    for (int i = 0; i < n; ++i)
    {
        const KNode& nd = g.node(i);
        if (is_fragment_only_op(nd.op) && e.stage != KStage::Fragment)
        {
            return fail("fragment derivative (dFdx/dFdy/fwidth) is only legal in a fragment stage");
        }
        // B2: implicit-LOD sampling picks its mip from screen-space derivatives ⇒ fragment stage only (a VS/CS must use
        // the explicit-LOD `sampleLod`, B2-b). The texture/sampler binding LEAVES themselves are legal in any raster stage.
        if (nd.op == KOp::TexSample && e.stage != KStage::Fragment)
        {
            return fail("implicit-LOD texture sample is only legal in a fragment stage (use sampleLod elsewhere)");
        }
        if (nd.op == KOp::Builtin)
        {
            const auto b = static_cast<KBuiltin>(nd.iidx);
            if (!builtin_allowed_in(b, e.stage)) { return fail("builtin is not readable in this stage"); }
        }
        else if (nd.op == KOp::StageIn)
        {
            if (e.stage == KStage::Compute) { return fail("a compute stage has no location-indexed inputs"); }
            // B1-c: an integer FRAGMENT interpolant must be `flat` (a vertex ATTRIBUTE is not interpolated, so it is exempt).
            if (e.stage == KStage::Fragment && requires_flat_interp(nd.type)
                && static_cast<Interp>(nd.dset) != Interp::Flat)
            {
                return fail("an integer fragment interpolant must be `flat`");
            }
            for (int j = 0; j < i; ++j)
            {
                if (g.node(j).op == KOp::StageIn && g.node(j).iidx == nd.iidx)
                {
                    return fail("two stage inputs share one location");
                }
            }
        }
    }
    return true;
}

} // namespace crd::kir
