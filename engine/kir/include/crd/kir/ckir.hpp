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
enum class DType : crd::u8 { F32, F64, F16, BF16, I32, I64, U8, Bool };

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
    Select,                                                 // ternary: cond ? a : b
    ReduceSum, ReduceMax, ReduceMin, ReduceProd, ArgMax, ArgMin, // reductions over an axis mask (keepdims; Arg* -> index)
    ScanSum,                                                     // inclusive prefix-sum along the trailing axis (keeps shape)
    Reshape, Permute, Broadcast,                            // movement / layout
    Contract,                                               // batched matmul: [...,M,K] x [...,K,N] -> [...,M,N]
    Gather,                                                 // row-gather along axis 0: out[m,...] = data[idx[m],...]
    Scatter,                                                // row-scatter (last-wins): out=base, then out[idx[m],...]=updates[m,...]
    ScatterAdd,                                             // atomic scatter-ADD (histogram): out[M]=0, then out[idx[i]] += updates[i]
    Cast                                                    // dtype conversion
};

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
    DType    dtype;
    Shape    shape;
    crd::i32 a = -1, b = -1, c = -1, d = -1; // operands (-1 = none); d added for 4-operand ops (mat4-from-columns)
    crd::f64 cval = 0.0;             // Const value
    crd::i32 iidx = 0;               // Input index / Iota axis
    crd::u32 axes = 0;               // ReduceSum/Max/Broadcast: axis bitmask
    crd::u8  perm[kMaxRank] = {};    // Permute permutation
    DetTier  tier = DetTier::Exact;  // reductions/scan: T1 exact (default) vs T2 fast-parallel
    crd::u8  comps = 1;              // A3 value width: 1=scalar, 2/3/4 = vecN (per-element vector value)
};

// round an f64 accumulator to a storage dtype so the reference is bit-faithful to that precision.
[[nodiscard]] inline crd::f64 round_dtype(crd::f64 v, DType dt) noexcept
{
    if (dt == DType::F32) { return static_cast<crd::f64>(static_cast<float>(v)); }
    return v; // F64 (and, for now, the storage dtypes — exact narrow rounding lands with their backends)
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
// Ternary shader intrinsics (a/b/c operands). Explicit formulas ⇒ the emitters match bit-for-bit (with NoContraction).
[[nodiscard]] inline crd::f64 apply_ternary(KOp op, crd::f64 a, crd::f64 b, crd::f64 c) noexcept
{
    switch (op)
    {
    case KOp::Clamp: { const crd::f64 m = a > b ? a : b; return m < c ? m : c; } // clamp(x=a, min=b, max=c) = min(max(a,b),c)
    case KOp::Mix: return a * (1.0 - c) + b * c;                                 // mix(x=a, y=b, t=c) = x*(1-t)+y*t
    case KOp::Smoothstep: { const crd::f64 u = (c - a) / (b - a); const crd::f64 t = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u); return t * t * (3.0 - 2.0 * t); } // smoothstep(e0=a,e1=b,x=c) — ULP
    case KOp::Fma: return crd::math::fma(a, b, c);                               // a*b+c single-rounded (IEEE fma) — bit-exact
    case KOp::BitfieldExtract: { const crd::i64 iv = static_cast<crd::i64>(a); const crd::i64 off = static_cast<crd::i64>(b); const crd::i64 bits = static_cast<crd::i64>(c); return static_cast<crd::f64>((iv >> off) & ((static_cast<crd::i64>(1) << bits) - 1)); } // (v>>off)&mask
    default: return a;
    }
}

class KGraph
{
public:
    explicit KGraph(crd::memory::IAllocator* alloc) noexcept : m_nodes(alloc) {}

    [[nodiscard]] int input(const Shape& shape, DType dt) { KNode n; n.op = KOp::Input; n.dtype = dt; n.shape = shape; n.iidx = m_ninput++; return push(n); }
    // A3: a vector/matrix input (comps>1) — per-element vecN/matN, fed as comps interleaved values per element.
    [[nodiscard]] int input_vec(const Shape& shape, DType dt, int comps) { KNode n; n.op = KOp::Input; n.dtype = dt; n.shape = shape; n.iidx = m_ninput++; n.comps = static_cast<crd::u8>(comps); return push(n); }
    [[nodiscard]] int constant(crd::f64 v, const Shape& shape, DType dt) { KNode n; n.op = KOp::Const; n.dtype = dt; n.shape = shape; n.cval = v; return push(n); }
    [[nodiscard]] int iota(const Shape& shape, int axis, DType dt) { KNode n; n.op = KOp::Iota; n.dtype = dt; n.shape = shape; n.iidx = axis; return push(n); }

    [[nodiscard]] int unary(KOp op, int a) { KNode n; n.op = op; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int binary(KOp op, int a, int b) { KNode n; n.op = op; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int ternary(KOp op, int a, int b, int c) { KNode n; n.op = op; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.c = c; n.comps = t(a).comps; return push(n); } // Clamp/Mix/Fma
    [[nodiscard]] int select(int cond, int a, int b) { KNode n; n.op = KOp::Select; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.c = cond; n.comps = t(a).comps; return push(n); }
    // Structured control flow — FIXED-count loop (A4 tier 1): acc = init; for it in [0,count): acc = body(it, acc); return acc.
    // Compile-time UNROLL ⇒ pure dataflow ⇒ runs on EVERY backend through the existing emitters (no IR/eval/emit change).
    // `body(int it, int acc) -> int` returns the next accumulator node. For DYNAMIC/large trip counts, the region-based
    // dynamic For/While (tier 2) is the follow-on slice (needs loop-body scoping in the eval + emitters).
    template <typename BodyFn>
    [[nodiscard]] int unroll_for(int count, int init, BodyFn&& body)
    {
        int acc = init;
        for (int it = 0; it < count; ++it) { acc = body(it, acc); }
        return acc;
    }
    // A4 tier-2 DYNAMIC loop (real per-thread `for`): acc = init; for it in [0, count): acc = body(index, acc); return acc.
    // `count` may be a per-element (divergent) node; `body_fn(index_node, acc_node) -> next-acc node` builds the body from
    // the body-scoped `LoopIndex` (F32 iteration) + `LoopAcc` (current accumulator) leaves. Single-level (no nesting yet).
    template <typename BodyFn>
    [[nodiscard]] int for_loop(int count, int init, BodyFn&& body_fn)
    {
        KNode     ix; ix.op = KOp::LoopIndex; ix.dtype = DType::F32; ix.shape = t(init).shape; ix.comps = 1; const int idx = push(ix);
        KNode     ac; ac.op = KOp::LoopAcc; ac.dtype = t(init).dtype; ac.shape = t(init).shape; ac.comps = t(init).comps; const int acc = push(ac);
        const int body = body_fn(idx, acc);
        KNode     n; n.op = KOp::For; n.dtype = t(init).dtype; n.shape = t(init).shape; n.a = count; n.b = init; n.c = body; n.comps = t(init).comps;
        return push(n);
    }
    // A4 tier-2 BOUNDED while (the GPU-safe form — no unbounded loops on a GPU): run up to max_iter, but each element
    // FREEZES its accumulator once `cond_fn(acc)` becomes 0. cond_fn(acc)->keep-node (nonzero = keep looping);
    // body_fn(index, acc)->next-acc node. Lowers to a For + a per-step Select ⇒ runs on every backend.
    template <typename CondFn, typename BodyFn>
    [[nodiscard]] int while_loop(int max_iter, int init, CondFn&& cond_fn, BodyFn&& body_fn)
    {
        const int mi = constant(static_cast<crd::f64>(max_iter), t(init).shape, t(init).dtype);
        return for_loop(mi, init, [&](int idx, int acc) { const int keep = cond_fn(acc); const int nxt = body_fn(idx, acc); return select(keep, nxt, acc); });
    }
    // A4 tier-2 SWITCH/if-branch multiplex: (selector == key) ? val : fallback. Chain these for a full switch; a plain
    // if/else on VALUES is just `select(cond, then, else)`. Branchless (both arms evaluated) — the shader-correct form.
    [[nodiscard]] int switch_case(int selector, int key, int val, int fallback) { return select(binary(KOp::CmpNe, selector, key), fallback, val); }
    [[nodiscard]] int cast(int a, DType dt) { KNode n; n.op = KOp::Cast; n.dtype = dt; n.shape = t(a).shape; n.a = a; n.comps = t(a).comps; return push(n); }
    // A3 vector values (per-element vecN; components stored interleaved in the eval buffer / emitter).
    [[nodiscard]] int vec2(int a, int b) { KNode n; n.op = KOp::Vec2; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = 2; return push(n); }
    [[nodiscard]] int vec3(int a, int b, int c) { KNode n; n.op = KOp::Vec3; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.c = c; n.comps = 3; return push(n); }
    [[nodiscard]] int vec_comp(int v, int idx) { KNode n; n.op = KOp::VecComp; n.dtype = t(v).dtype; n.shape = t(v).shape; n.a = v; n.iidx = idx; n.comps = 1; return push(n); }
    [[nodiscard]] int dot(int a, int b) { KNode n; n.op = KOp::Dot; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = 1; return push(n); }
    [[nodiscard]] int cross(int a, int b) { KNode n; n.op = KOp::Cross; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = 3; return push(n); }
    [[nodiscard]] int normalize(int a) { KNode n; n.op = KOp::Normalize; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int vlength(int a) { KNode n; n.op = KOp::VecLen; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.comps = 1; return push(n); }
    // concat two vec values → a wider vec (comps sum). The primitive for vec4 = concat(vec3, w) and general assembly.
    [[nodiscard]] int vec_concat(int a, int b) { KNode n; n.op = KOp::VecConcat; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = static_cast<crd::u8>(t(a).comps + t(b).comps); return push(n); }
    [[nodiscard]] int vec4(int x, int y, int z, int w) { return vec_concat(vec3(x, y, z), w); } // (x,y,z,w) — comps 4
    // arbitrary swizzle: out component k = source component idx[k]. width = count of valid (>=0) indices. Covers
    // .x (swizzle(v,0)), .xy (v,0,1), .yzx (v,1,2,0), .wzyx (v,3,2,1,0) — reorder + subset + broadcast (repeat) alike.
    [[nodiscard]] int swizzle(int v, int i0, int i1 = -1, int i2 = -1, int i3 = -1)
    {
        KNode n; n.op = KOp::Swizzle; n.dtype = t(v).dtype; n.shape = t(v).shape; n.a = v;
        int w = 0; const int idx[4] = {i0, i1, i2, i3};
        for (int k = 0; k < 4; ++k) { if (idx[k] >= 0) { n.perm[k] = static_cast<crd::u8>(idx[k]); ++w; } }
        n.comps = static_cast<crd::u8>(w);
        return push(n);
    }
    // Matrices (column-major, stored as concatenated columns): mat3 = 9 comps, mat4 = 16 comps.
    [[nodiscard]] int mat3(int c0, int c1, int c2) { KNode n; n.op = KOp::MatFromCols; n.dtype = t(c0).dtype; n.shape = t(c0).shape; n.a = c0; n.b = c1; n.c = c2; n.comps = 9; return push(n); } // GPU-constructible
    [[nodiscard]] int mat4(int c0, int c1, int c2, int c3) { KNode n; n.op = KOp::MatFromCols; n.dtype = t(c0).dtype; n.shape = t(c0).shape; n.a = c0; n.b = c1; n.c = c2; n.d = c3; n.comps = 16; return push(n); } // GPU-constructible (4 vec4 columns)
    [[nodiscard]] int mat_mul_vec(int m, int v) { KNode n; n.op = KOp::MatVecMul; n.dtype = t(v).dtype; n.shape = t(v).shape; n.a = m; n.b = v; n.comps = t(v).comps; return push(n); }
    [[nodiscard]] int mat_mul(int a, int b) { KNode n; n.op = KOp::MatMatMul; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int mat_transpose(int m) { KNode n; n.op = KOp::MatTranspose; n.dtype = t(m).dtype; n.shape = t(m).shape; n.a = m; n.comps = t(m).comps; return push(n); }
    // scalar → vecN broadcast (the enabler for scalar*vec, mix(vec,vec,scalar), etc.).
    [[nodiscard]] int splat(int a, int width) { KNode n; n.op = KOp::Splat; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.comps = static_cast<crd::u8>(width); return push(n); }
    // geometric (GLSL semantics). reflect(I,N)=I-2*dot(N,I)*N · refract(I,N,eta) · faceforward(N,I,Nref) · distance=|a-b|.
    [[nodiscard]] int reflect(int i, int nrm) { KNode n; n.op = KOp::Reflect; n.dtype = t(i).dtype; n.shape = t(i).shape; n.a = i; n.b = nrm; n.comps = t(i).comps; return push(n); }
    [[nodiscard]] int refract(int i, int nrm, int eta) { KNode n; n.op = KOp::Refract; n.dtype = t(i).dtype; n.shape = t(i).shape; n.a = i; n.b = nrm; n.c = eta; n.comps = t(i).comps; return push(n); }
    [[nodiscard]] int faceforward(int nrm, int i, int nref) { KNode n; n.op = KOp::Faceforward; n.dtype = t(nrm).dtype; n.shape = t(nrm).shape; n.a = nrm; n.b = i; n.c = nref; n.comps = t(nrm).comps; return push(n); }
    [[nodiscard]] int distance(int a, int b) { return vlength(binary(KOp::Sub, a, b)); }
    // relational reductions over the components → scalar 0/1.
    [[nodiscard]] int vany(int v) { KNode n; n.op = KOp::VecAny; n.dtype = t(v).dtype; n.shape = t(v).shape; n.a = v; n.comps = 1; return push(n); }
    [[nodiscard]] int vall(int v) { KNode n; n.op = KOp::VecAll; n.dtype = t(v).dtype; n.shape = t(v).shape; n.a = v; n.comps = 1; return push(n); }
    // matrix: outer product (vecN ⊗ vecN → NxN mat, column-major) · determinant (→ scalar) · inverse.
    [[nodiscard]] int outer_product(int a, int b) { KNode n; n.op = KOp::OuterProduct; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = static_cast<crd::u8>(t(a).comps * t(b).comps); return push(n); }
    [[nodiscard]] int determinant(int m) { KNode n; n.op = KOp::Determinant; n.dtype = t(m).dtype; n.shape = t(m).shape; n.a = m; n.comps = 1; return push(n); }
    [[nodiscard]] int mat_inverse(int m) { KNode n; n.op = KOp::MatInverse; n.dtype = t(m).dtype; n.shape = t(m).shape; n.a = m; n.comps = t(m).comps; return push(n); }
    // interpolation + quaternions (quat = vec4 (x,y,z,w), w = scalar). lerp = mix ✅; nlerp = normalize(mix).
    [[nodiscard]] int slerp(int a, int b, int tt) { KNode n; n.op = KOp::Slerp; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.c = tt; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int nlerp(int a, int b, int tt) { return normalize(ternary(KOp::Mix, a, b, splat(tt, t(a).comps))); }
    [[nodiscard]] int quat_mul(int a, int b) { KNode n; n.op = KOp::QuatMul; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.comps = 4; return push(n); }
    [[nodiscard]] int quat_conj(int q) { KNode n; n.op = KOp::QuatConj; n.dtype = t(q).dtype; n.shape = t(q).shape; n.a = q; n.comps = 4; return push(n); }
    [[nodiscard]] int quat_rotate(int q, int v) { KNode n; n.op = KOp::QuatRotate; n.dtype = t(q).dtype; n.shape = t(q).shape; n.a = q; n.b = v; n.comps = 3; return push(n); }
    [[nodiscard]] int quat_axis_angle(int axis, int angle) { KNode n; n.op = KOp::QuatAxisAngle; n.dtype = t(axis).dtype; n.shape = t(axis).shape; n.a = axis; n.b = angle; n.comps = 4; return push(n); }
    [[nodiscard]] int quat_to_mat3(int q) { KNode n; n.op = KOp::QuatToMat3; n.dtype = t(q).dtype; n.shape = t(q).shape; n.a = q; n.comps = 9; return push(n); }
    // minor gaps
    [[nodiscard]] int bit_reverse(int a) { return unary(KOp::BitReverse, a); }
    [[nodiscard]] int ldexp(int m, int e) { return binary(KOp::Ldexp, m, e); }
    [[nodiscard]] int float_bits_to_int(int a) { KNode n; n.op = KOp::FloatBitsToInt; n.dtype = DType::I32; n.shape = t(a).shape; n.a = a; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int int_bits_to_float(int a) { KNode n; n.op = KOp::IntBitsToFloat; n.dtype = DType::F32; n.shape = t(a).shape; n.a = a; n.comps = t(a).comps; return push(n); }
    [[nodiscard]] int modf(int x) { KNode n; n.op = KOp::Modf; n.dtype = t(x).dtype; n.shape = t(x).shape; n.a = x; n.comps = 2; return push(n); } // → vec2(intpart, fracpart)
    // bitfieldInsert(base, insert, off, bits) = (base & ~mask) | ((insert<<off) & mask), mask = ((1<<bits)-1)<<off — composed.
    [[nodiscard]] int bitfield_insert(int base, int ins, int off, int bits)
    {
        const int one  = constant(1.0, t(base).shape, t(base).dtype);
        const int mask = binary(KOp::Shl, binary(KOp::Sub, binary(KOp::Shl, one, bits), one), off);
        const int keep = binary(KOp::BitAnd, base, unary(KOp::BitNot, mask));
        const int set  = binary(KOp::BitAnd, binary(KOp::Shl, ins, off), mask);
        return binary(KOp::BitOr, keep, set);
    }
    // GLM-style transform matrices — pure COMPOSITION of the mat4/vec4 primitives (column-major). translate/scale shown;
    // rotate = mat4-from-quat, perspective/ortho/lookAt = the same pattern (consumer/engine-math level, add on demand).
    [[nodiscard]] int translate(int tx, int ty, int tz)
    {
        const int z = constant(0.0, t(tx).shape, t(tx).dtype);
        const int o = constant(1.0, t(tx).shape, t(tx).dtype);
        return mat4(vec4(o, z, z, z), vec4(z, o, z, z), vec4(z, z, o, z), vec4(tx, ty, tz, o));
    }
    [[nodiscard]] int scale(int sx, int sy, int sz)
    {
        const int z = constant(0.0, t(sx).shape, t(sx).dtype);
        const int o = constant(1.0, t(sx).shape, t(sx).dtype);
        return mat4(vec4(sx, z, z, z), vec4(z, sy, z, z), vec4(z, z, sz, z), vec4(z, z, z, o));
    }

    // reduce over the axes in `mask` (keepdims: reduced axes become 1). `tier` selects the determinism tier: Exact (T1,
    // bit-exact fixed order — default) or Fast (T2, parallel workgroup tree-reduce — reordered, RFA, run-to-run stable).
    [[nodiscard]] int reduce(KOp op, int a, crd::u32 mask, DetTier tier = DetTier::Exact)
    {
        KNode n; n.op = op; n.dtype = t(a).dtype; n.a = a; n.axes = mask; n.shape = t(a).shape; n.tier = tier;
        for (int i = 0; i < n.shape.rank; ++i) { if ((mask >> i) & 1U) { n.shape.dims[i] = 1; } }
        return push(n);
    }
    [[nodiscard]] int reshape(int a, const Shape& out) { KNode n; n.op = KOp::Reshape; n.dtype = t(a).dtype; n.shape = out; n.a = a; return push(n); }
    [[nodiscard]] int permute(int a, const crd::u8* p)
    {
        KNode n; n.op = KOp::Permute; n.dtype = t(a).dtype; n.a = a; n.shape.rank = t(a).shape.rank;
        for (int i = 0; i < n.shape.rank; ++i) { n.perm[i] = p[i]; n.shape.dims[i] = t(a).shape.dims[p[i]]; }
        return push(n);
    }
    [[nodiscard]] int broadcast(int a, const Shape& out) { KNode n; n.op = KOp::Broadcast; n.dtype = t(a).dtype; n.shape = out; n.a = a; return push(n); }
    // batched matmul: a[...,M,K], b[...,K,N] -> [...,M,N] (leading batch dims must match). `tier` selects determinism:
    // Exact (T1, `precise`/no-FMA fixed-order — bit-exact vs the CPU oracle, default) or Fast (T2, FMA + tiled schedule —
    // run-to-run deterministic, matches the FMA-tier oracle; the ported crush kernel).
    [[nodiscard]] int contract(int a, int b, DetTier tier = DetTier::Exact)
    {
        KNode n; n.op = KOp::Contract; n.dtype = t(a).dtype; n.a = a; n.b = b; n.tier = tier;
        const Shape& sa = t(a).shape;
        n.shape = sa;
        n.shape.dims[sa.rank - 1] = t(b).shape.dims[t(b).shape.rank - 1]; // N
        return push(n);
    }
    // row-gather along axis 0: data[R, trailing...], idx[M] (integer values, f32-encoded) -> out[M, trailing...] where
    // out[m, ...] = data[idx[m], ...]. The embedding-lookup / index-select pattern.
    [[nodiscard]] int gather(int data, int idx)
    {
        KNode n; n.op = KOp::Gather; n.dtype = t(data).dtype; n.a = data; n.b = idx;
        n.shape          = t(data).shape;
        n.shape.dims[0]  = t(idx).shape.dims[0]; // R -> M (leading axis becomes the index count)
        return push(n);
    }
    // row-scatter along axis 0 (deterministic LAST-WINS): out = base[R, trailing...], then out[idx[m], ...] = updates[m,
    // ...] for m=0..M-1 in order (a later m overrides an earlier one at the same index). The write-side inverse of gather.
    [[nodiscard]] int scatter(int base, int idx, int updates)
    {
        KNode n; n.op = KOp::Scatter; n.dtype = t(base).dtype; n.a = base; n.b = idx; n.c = updates;
        n.shape = t(base).shape; // output has the SAME shape as base
        return push(n);
    }
    // atomic scatter-ADD (histogram): out has shape `bins` (M elements), zero-initialized, then out[idx[i]] += updates[i]
    // for i=0..N-1. INTEGER accumulation ⇒ order-independent ⇒ deterministic + bit-exact even though the GPU uses
    // atomics (the determinism moat survives). The building block of the radix histogram / counting sort.
    [[nodiscard]] int scatter_add(int idx, int updates, const Shape& bins)
    {
        KNode n; n.op = KOp::ScatterAdd; n.dtype = t(updates).dtype; n.a = idx; n.b = updates; n.shape = bins;
        return push(n);
    }
    // inclusive prefix-sum along the TRAILING axis (out[..., c] = sum of a[..., 0..c]); keeps the input shape. Fixed
    // ascending order per row ⇒ deterministic + bit-exact vs the naive f32 GPU scan.
    [[nodiscard]] int scan(int a, DetTier tier = DetTier::Exact) { KNode n; n.op = KOp::ScanSum; n.dtype = t(a).dtype; n.a = a; n.shape = t(a).shape; n.tier = tier; return push(n); }

    [[nodiscard]] int          size() const noexcept { return static_cast<int>(m_nodes.size()); }
    [[nodiscard]] int          n_inputs() const noexcept { return m_ninput; }
    [[nodiscard]] const KNode& node(int i) const noexcept { return m_nodes[static_cast<crd::usize>(i)]; }

    // Append a COPY of an already-validated node (op/shape/dtype/axes preserved; caller remaps a/b/c to this graph's ids).
    // For the multi-kernel scheduler's per-kernel mini-graphs — it clones a subgraph, so no shape/dtype re-inference.
    [[nodiscard]] int clone(const KNode& n) { return push(n); }

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
            if (g.comps != 1) { continue; } // vec/mat values fold to multiple components — never a single scalar Const
            const bool ac = g.a < 0 || isc[static_cast<crd::usize>(g.a)];
            const bool bc = g.b < 0 || isc[static_cast<crd::usize>(g.b)];
            const bool cc = g.c < 0 || isc[static_cast<crd::usize>(g.c)];
            if (!(ac && bc && cc)) { continue; }
            const crd::f64 av = g.a >= 0 ? cval[static_cast<crd::usize>(g.a)] : 0.0;
            const crd::f64 bv = g.b >= 0 ? cval[static_cast<crd::usize>(g.b)] : 0.0;
            crd::f64       r  = 0.0;
            if (g.op == KOp::Select) { r = (g.c >= 0 && cval[static_cast<crd::usize>(g.c)] != 0.0) ? av : bv; }
            else if (g.op == KOp::Cast) { r = round_dtype(av, g.dtype); }
            // movement (Reshape/Permute/Broadcast) + ReduceMax of a uniform fill are all identity on the value
            else if (g.op == KOp::Reshape || g.op == KOp::Permute || g.op == KOp::Broadcast || g.op == KOp::ReduceMax) { r = av; }
            else if (g.op == KOp::ReduceSum) { crd::i64 c = 1; const Shape& sa = m_nodes[static_cast<crd::usize>(g.a)].shape; for (int k = 0; k < sa.rank; ++k) { if ((g.axes >> k) & 1U) { c *= sa.dims[k]; } } r = av * static_cast<crd::f64>(c); }
            else if (g.b >= 0) { r = apply_binary(g.op, av, bv); }
            else { r = apply_unary(g.op, av); }
            const Shape sh = g.shape;
            const DType dt = g.dtype;
            g = KNode{};
            g.op = KOp::Const; g.shape = sh; g.dtype = dt; g.cval = r;
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
        }
        int cap = 1;
        while (cap < 2 * n + 4) { cap <<= 1; }
        crd::containers::Array<int>   table(al);
        crd::containers::Array<int>   newid(al);
        crd::containers::Array<KNode> nn(al);
        table.resize(static_cast<crd::usize>(cap), -1);
        newid.resize(static_cast<crd::usize>(n), -1);
        for (int i = 0; i < n; ++i)
        {
            if (!keep[static_cast<crd::usize>(i)]) { continue; }
            KNode g = m_nodes[static_cast<crd::usize>(i)];
            if (g.a >= 0) { g.a = newid[static_cast<crd::usize>(g.a)]; }
            if (g.b >= 0) { g.b = newid[static_cast<crd::usize>(g.b)]; }
            if (g.c >= 0) { g.c = newid[static_cast<crd::usize>(g.c)]; }
            newid[static_cast<crd::usize>(i)] = intern(table, cap, nn, g);
        }
        m_nodes = static_cast<crd::containers::Array<KNode>&&>(nn);
        for (int r = 0; r < n_roots; ++r) { roots[r] = newid[static_cast<crd::usize>(roots[r])]; }
    }

private:
    int push(const KNode& n) { m_nodes.push_back(n); return static_cast<int>(m_nodes.size()) - 1; }
    [[nodiscard]] const KNode& t(int i) const noexcept { return m_nodes[static_cast<crd::usize>(i)]; }

    [[nodiscard]] static crd::u64 key_hash(const KNode& g) noexcept
    {
        crd::u64 h = 1469598103934665603ULL;
        const auto mix = [&h](crd::u64 v) { h ^= v; h *= 1099511628211ULL; };
        mix(static_cast<crd::u64>(g.op));
        mix(static_cast<crd::u64>(g.dtype));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.a)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.b)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.c)));
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.d)));
        mix(static_cast<crd::u64>(g.comps));
        crd::u64 cb = 0;
        std::memcpy(&cb, &g.cval, sizeof(cb));
        mix(cb);
        mix(static_cast<crd::u64>(static_cast<crd::u32>(g.iidx)));
        mix(static_cast<crd::u64>(g.axes));
        mix(static_cast<crd::u64>(g.shape.rank));
        for (int k = 0; k < g.shape.rank; ++k) { mix(static_cast<crd::u64>(g.shape.dims[k])); mix(static_cast<crd::u64>(g.perm[k])); }
        return h;
    }
    [[nodiscard]] static bool node_equal(const KNode& x, const KNode& y) noexcept
    {
        if (x.op == KOp::For || x.op == KOp::LoopIndex || x.op == KOp::LoopAcc) { return false; } // never CSE loop constructs — operandless leaves belong to a specific loop
        if (x.op != y.op || x.dtype != y.dtype || x.a != y.a || x.b != y.b || x.c != y.c || x.d != y.d || x.comps != y.comps || x.iidx != y.iidx || x.axes != y.axes) { return false; }
        if (x.cval != y.cval || !(x.shape == y.shape)) { return false; }
        for (int k = 0; k < x.shape.rank; ++k) { if (x.perm[k] != y.perm[k]) { return false; } }
        return true;
    }
    static int intern(crd::containers::Array<int>& table, int cap, crd::containers::Array<KNode>& nn, const KNode& g)
    {
        const int mask = cap - 1;
        int       slot = static_cast<int>(key_hash(g) & static_cast<crd::u64>(mask));
        while (table[static_cast<crd::usize>(slot)] >= 0)
        {
            if (node_equal(nn[static_cast<crd::usize>(table[static_cast<crd::usize>(slot)])], g)) { return table[static_cast<crd::usize>(slot)]; }
            slot = (slot + 1) & mask;
        }
        const int id = static_cast<int>(nn.size());
        nn.push_back(g);
        table[static_cast<crd::usize>(slot)] = id;
        return id;
    }

    crd::containers::Array<KNode> m_nodes;
    int                           m_ninput = 0;
};

} // namespace crd::kir
