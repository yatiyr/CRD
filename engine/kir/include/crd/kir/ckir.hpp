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
    Select,                                                 // ternary: cond ? a : b
    ReduceSum, ReduceMax, ReduceMin, ReduceProd, ArgMax, ArgMin, // reductions over an axis mask (keepdims; Arg* -> index)
    ScanSum,                                                     // inclusive prefix-sum along the trailing axis (keeps shape)
    Reshape, Permute, Broadcast,                            // movement / layout
    Contract,                                               // batched matmul: [...,M,K] x [...,K,N] -> [...,M,N]
    Gather,                                                 // row-gather along axis 0: out[m,...] = data[idx[m],...]
    Scatter,                                                // row-scatter (last-wins): out=base, then out[idx[m],...]=updates[m,...]
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
    crd::i32 a = -1, b = -1, c = -1; // operands (-1 = none)
    crd::f64 cval = 0.0;             // Const value
    crd::i32 iidx = 0;               // Input index / Iota axis
    crd::u32 axes = 0;               // ReduceSum/Max/Broadcast: axis bitmask
    crd::u8  perm[kMaxRank] = {};    // Permute permutation
    DetTier  tier = DetTier::Exact;  // reductions/scan: T1 exact (default) vs T2 fast-parallel
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
    default: return 0.0;
    }
}

class KGraph
{
public:
    explicit KGraph(crd::memory::IAllocator* alloc) noexcept : m_nodes(alloc) {}

    [[nodiscard]] int input(const Shape& shape, DType dt) { KNode n; n.op = KOp::Input; n.dtype = dt; n.shape = shape; n.iidx = m_ninput++; return push(n); }
    [[nodiscard]] int constant(crd::f64 v, const Shape& shape, DType dt) { KNode n; n.op = KOp::Const; n.dtype = dt; n.shape = shape; n.cval = v; return push(n); }
    [[nodiscard]] int iota(const Shape& shape, int axis, DType dt) { KNode n; n.op = KOp::Iota; n.dtype = dt; n.shape = shape; n.iidx = axis; return push(n); }

    [[nodiscard]] int unary(KOp op, int a) { KNode n; n.op = op; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; return push(n); }
    [[nodiscard]] int binary(KOp op, int a, int b) { KNode n; n.op = op; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; return push(n); }
    [[nodiscard]] int select(int cond, int a, int b) { KNode n; n.op = KOp::Select; n.dtype = t(a).dtype; n.shape = t(a).shape; n.a = a; n.b = b; n.c = cond; return push(n); }
    [[nodiscard]] int cast(int a, DType dt) { KNode n; n.op = KOp::Cast; n.dtype = dt; n.shape = t(a).shape; n.a = a; return push(n); }

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
    // batched matmul: a[...,M,K], b[...,K,N] -> [...,M,N] (leading batch dims must match).
    [[nodiscard]] int contract(int a, int b)
    {
        KNode n; n.op = KOp::Contract; n.dtype = t(a).dtype; n.a = a; n.b = b;
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
    // inclusive prefix-sum along the TRAILING axis (out[..., c] = sum of a[..., 0..c]); keeps the input shape. Fixed
    // ascending order per row ⇒ deterministic + bit-exact vs the naive f32 GPU scan.
    [[nodiscard]] int scan(int a, DetTier tier = DetTier::Exact) { KNode n; n.op = KOp::ScanSum; n.dtype = t(a).dtype; n.a = a; n.shape = t(a).shape; n.tier = tier; return push(n); }

    [[nodiscard]] int          size() const noexcept { return static_cast<int>(m_nodes.size()); }
    [[nodiscard]] int          n_inputs() const noexcept { return m_ninput; }
    [[nodiscard]] const KNode& node(int i) const noexcept { return m_nodes[static_cast<crd::usize>(i)]; }

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
            if (g.op == KOp::Input || g.op == KOp::Iota || g.op == KOp::Contract) { continue; }
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
        if (x.op != y.op || x.dtype != y.dtype || x.a != y.a || x.b != y.b || x.c != y.c || x.iidx != y.iidx || x.axes != y.axes) { return false; }
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
