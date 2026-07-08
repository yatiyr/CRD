#pragma once

// ckir_eval.hpp — Phase 3.1.6 v17-a: the CKIR **CPU reference interpreter** — the single oracle every GPU backend
// proves against (bit/ulp) and the determinism ground truth. Materializes each graph node into a contiguous row-major
// f64 buffer and computes it in fixed, deterministic order (reductions + contractions accumulate ascending — no
// reordering). Not perf-critical; it is the correctness reference, not a kernel. Matching the GPU fixed-TREE
// reduction bit-for-bit is the v17-f (T1/T2/T3) work; this establishes the deterministic baseline. ADR-0098.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

namespace eval_detail
{
// NOLINTNEXTLINE(readability-non-const-parameter) -- coord is written (coord[i]=...); tidy misfires on this header-only fn
inline void unflatten(crd::i64 flat, const Shape& s, crd::i64* coord) noexcept
{
    for (int i = s.rank - 1; i >= 0; --i) { coord[i] = flat % s.dims[i]; flat /= s.dims[i]; }
}
[[nodiscard]] inline crd::i64 flatten(const crd::i64* coord, const Shape& s) noexcept
{
    crd::i64 f = 0;
    crd::i64 str = 1;
    for (int i = s.rank - 1; i >= 0; --i) { f += coord[i] * str; str *= s.dims[i]; }
    return f;
}
[[nodiscard]] inline bool is_unary(KOp op) noexcept
{
    return op == KOp::Neg || op == KOp::Recip || op == KOp::Abs || op == KOp::Exp || op == KOp::Log
           || op == KOp::Sin || op == KOp::Cos || op == KOp::Sqrt || op == KOp::Tanh
           || op == KOp::Floor || op == KOp::Ceil || op == KOp::Sign || op == KOp::Trunc || op == KOp::Round;
}
[[nodiscard]] inline bool is_binary(KOp op) noexcept
{
    return op == KOp::Add || op == KOp::Sub || op == KOp::Mul || op == KOp::Div || op == KOp::Max
           || op == KOp::Min || op == KOp::CmpLt || op == KOp::CmpEq || op == KOp::CmpLe;
}
} // namespace eval_detail

// Interpret `g` on the CPU. `inputs[k]` = row-major f64 data for the k-th Input node (matching its shape). Fills `out`
// (numel = node(output_node).shape.numel()) with that node's result. `scratch` holds the per-node materialization.
// NOLINTBEGIN(readability-non-const-parameter) -- scratch (allocate/deallocate = non-const methods) + out (out[e]=... written) are genuinely non-const; the check misfires here
inline void eval_cpu(const KGraph& g, const crd::f64* const* inputs, crd::memory::IAllocator* scratch, int output_node,
                     crd::f64* out) noexcept
// NOLINTEND(readability-non-const-parameter)
{
    using namespace eval_detail;
    const int                        nn = g.size();
    crd::containers::Array<crd::i64>  off(scratch);
    off.reserve(static_cast<crd::usize>(nn));
    crd::i64 total = 0;
    for (int i = 0; i < nn; ++i) { off.push_back(total); total += g.node(i).shape.numel(); }

    auto*          buf = static_cast<crd::f64*>(scratch->allocate(sizeof(crd::f64) * static_cast<crd::usize>(total), alignof(crd::f64)));
    crd::i64       oc[kMaxRank];
    crd::i64       ic[kMaxRank];
    for (int i = 0; i < nn; ++i)
    {
        const KNode&   n   = g.node(i);
        crd::f64*      dst = buf + off[static_cast<crd::usize>(i)];
        const crd::i64 ne  = n.shape.numel();

        if (n.op == KOp::Input) { const crd::f64* s = inputs[n.iidx]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = s[e]; } }
        else if (n.op == KOp::Const) { const crd::f64 v = round_dtype(n.cval, n.dtype); for (crd::i64 e = 0; e < ne; ++e) { dst[e] = v; } }
        else if (n.op == KOp::Iota) { for (crd::i64 e = 0; e < ne; ++e) { unflatten(e, n.shape, oc); dst[e] = round_dtype(static_cast<crd::f64>(oc[n.iidx]), n.dtype); } }
        else if (is_unary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = round_dtype(apply_unary(n.op, a[e]), n.dtype); } }
        else if (is_binary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = round_dtype(apply_binary(n.op, a[e], b[e]), n.dtype); } }
        else if (n.op == KOp::Select) { const crd::f64* cc = buf + off[static_cast<crd::usize>(n.c)]; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = cc[e] != 0.0 ? a[e] : b[e]; } }
        else if (n.op == KOp::Cast) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = round_dtype(a[e], n.dtype); } }
        else if (n.op == KOp::Reshape) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = a[e]; } }
        else if (n.op == KOp::Permute)
        {
            const KNode&    an = g.node(n.a);
            const crd::f64* a  = buf + off[static_cast<crd::usize>(n.a)];
            for (crd::i64 e = 0; e < ne; ++e) { unflatten(e, n.shape, oc); for (int k = 0; k < n.shape.rank; ++k) { ic[n.perm[k]] = oc[k]; } dst[e] = a[flatten(ic, an.shape)]; }
        }
        else if (n.op == KOp::Broadcast)
        {
            const KNode&    an = g.node(n.a);
            const crd::f64* a  = buf + off[static_cast<crd::usize>(n.a)];
            for (crd::i64 e = 0; e < ne; ++e) { unflatten(e, n.shape, oc); for (int k = 0; k < n.shape.rank; ++k) { ic[k] = an.shape.dims[k] == 1 ? 0 : oc[k]; } dst[e] = a[flatten(ic, an.shape)]; }
        }
        else if (is_reduce(n.op))
        {
            const KNode&    an  = g.node(n.a);
            const crd::f64* a   = buf + off[static_cast<crd::usize>(n.a)];
            crd::i64        rsz = 1;
            for (int k = 0; k < an.shape.rank; ++k) { if ((n.axes >> k) & 1U) { rsz *= an.shape.dims[k]; } }
            for (crd::i64 e = 0; e < ne; ++e)
            {
                unflatten(e, n.shape, oc);
                crd::f64 acc     = (n.op == KOp::ReduceProd) ? 1.0 : 0.0; // fixed ascending accumulation order = deterministic
                crd::f64 arg_val = 0.0;                                   // best value seen (for Arg* variants)
                crd::i64 arg_idx = 0;                                     // index of the best (first occurrence wins)
                for (crd::i64 r = 0; r < rsz; ++r)
                {
                    crd::i64 rem = r;
                    for (int k = an.shape.rank - 1; k >= 0; --k)
                    {
                        if ((n.axes >> k) & 1U) { ic[k] = rem % an.shape.dims[k]; rem /= an.shape.dims[k]; }
                        else { ic[k] = oc[k]; }
                    }
                    const crd::f64 v = a[flatten(ic, an.shape)];
                    // round each accumulation step to the node dtype: for F32 this matches a naive f32 GPU kernel
                    // (bit-exact); for F64 round_dtype is identity (unchanged). Fixed ascending order = deterministic.
                    if (n.op == KOp::ReduceSum) { acc = round_dtype(acc + v, n.dtype); }
                    else if (n.op == KOp::ReduceProd) { acc = round_dtype(acc * v, n.dtype); }
                    else if (n.op == KOp::ReduceMax) { acc = (r == 0 || v > acc) ? v : acc; }
                    else if (n.op == KOp::ReduceMin) { acc = (r == 0 || v < acc) ? v : acc; }
                    else if (n.op == KOp::ArgMax) { if (r == 0 || v > arg_val) { arg_val = v; arg_idx = r; } }
                    else { if (r == 0 || v < arg_val) { arg_val = v; arg_idx = r; } } // ArgMin
                }
                dst[e] = is_argreduce(n.op) ? static_cast<crd::f64>(arg_idx) : round_dtype(acc, n.dtype);
            }
        }
        else if (n.op == KOp::Contract)
        {
            const KNode&    an    = g.node(n.a);
            const KNode&    bn    = g.node(n.b);
            const crd::f64* a     = buf + off[static_cast<crd::usize>(n.a)];
            const crd::f64* b     = buf + off[static_cast<crd::usize>(n.b)];
            const crd::i64  mm    = an.shape.dims[an.shape.rank - 2];
            const crd::i64  kk    = an.shape.dims[an.shape.rank - 1];
            const crd::i64  nnn   = bn.shape.dims[bn.shape.rank - 1];
            crd::i64        batch = 1;
            for (int k = 0; k < n.shape.rank - 2; ++k) { batch *= n.shape.dims[k]; }
            for (crd::i64 bi = 0; bi < batch; ++bi)
            {
                const crd::f64* ab = a + bi * mm * kk;
                const crd::f64* bb = b + bi * kk * nnn;
                crd::f64*       cb = dst + bi * mm * nnn;
                for (crd::i64 m = 0; m < mm; ++m)
                {
                    for (crd::i64 col = 0; col < nnn; ++col)
                    {
                        crd::f64 acc = 0.0; // fixed ascending k order; round product + accumulation to node dtype
                        for (crd::i64 k = 0; k < kk; ++k)
                        {
                            const crd::f64 prod = round_dtype(ab[m * kk + k] * bb[k * nnn + col], n.dtype);
                            acc                 = round_dtype(acc + prod, n.dtype);
                        }
                        cb[m * nnn + col] = round_dtype(acc, n.dtype);
                    }
                }
            }
        }
        else if (n.op == KOp::Gather)
        {
            const KNode&    dn      = g.node(n.a);
            const crd::f64* d       = buf + off[static_cast<crd::usize>(n.a)];
            const crd::f64* ix      = buf + off[static_cast<crd::usize>(n.b)];
            const crd::i64  rowsize = dn.shape.numel() / dn.shape.dims[0]; // product of trailing dims
            for (crd::i64 e = 0; e < ne; ++e)
            {
                const crd::i64 m = e / rowsize;
                const crd::i64 c = e % rowsize;
                const crd::i64 r = static_cast<crd::i64>(ix[m]); // f32-encoded integer index
                dst[e]           = d[r * rowsize + c];
            }
        }
        else if (n.op == KOp::Scatter)
        {
            const KNode&    bn      = g.node(n.a);
            const crd::f64* base    = buf + off[static_cast<crd::usize>(n.a)];
            const crd::f64* ix      = buf + off[static_cast<crd::usize>(n.b)];
            const crd::f64* upd     = buf + off[static_cast<crd::usize>(n.c)];
            const crd::i64  rowsize = bn.shape.numel() / bn.shape.dims[0];
            const crd::i64  mcount  = g.node(n.b).shape.dims[0];
            for (crd::i64 e = 0; e < ne; ++e)
            {
                const crd::i64 r      = e / rowsize;
                const crd::i64 c      = e % rowsize;
                crd::f64       result = base[e];
                for (crd::i64 m = 0; m < mcount; ++m) { if (static_cast<crd::i64>(ix[m]) == r) { result = upd[m * rowsize + c]; } } // last wins
                dst[e] = result;
            }
        }
        else if (n.op == KOp::ScanSum)
        {
            const KNode&    an      = g.node(n.a);
            const crd::f64* a       = buf + off[static_cast<crd::usize>(n.a)];
            const crd::i64  scanlen = an.shape.dims[an.shape.rank - 1];
            const crd::i64  nrows   = scanlen > 0 ? an.shape.numel() / scanlen : 0;
            for (crd::i64 row = 0; row < nrows; ++row)
            {
                crd::f64 acc = 0.0;
                for (crd::i64 c = 0; c < scanlen; ++c)
                {
                    acc                    = round_dtype(acc + a[row * scanlen + c], n.dtype); // fixed ascending order = deterministic
                    dst[row * scanlen + c] = acc;
                }
            }
        }
    }

    const crd::i64  one = g.node(output_node).shape.numel();
    const crd::f64* src = buf + off[static_cast<crd::usize>(output_node)];
    for (crd::i64 e = 0; e < one; ++e) { out[e] = src[e]; }
    scratch->deallocate(buf);
}

} // namespace crd::kir
