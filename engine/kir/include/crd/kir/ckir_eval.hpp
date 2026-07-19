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
           || op == KOp::Floor || op == KOp::Ceil || op == KOp::Sign || op == KOp::Trunc || op == KOp::Round
           || op == KOp::Fract || op == KOp::Rsqrt || op == KOp::Exp2 || op == KOp::Log2 || op == KOp::Tan
           || op == KOp::Radians || op == KOp::Degrees || op == KOp::Asin || op == KOp::Acos || op == KOp::Atan
           || op == KOp::Sinh || op == KOp::Cosh || op == KOp::Cbrt || op == KOp::BitNot || op == KOp::BitCount
           || op == KOp::FindLSB || op == KOp::FindMSB || op == KOp::BitReverse || op == KOp::FloatBitsToInt
           || op == KOp::IntBitsToFloat;
}
[[nodiscard]] inline bool is_binary(KOp op) noexcept
{
    return op == KOp::Add || op == KOp::Sub || op == KOp::Mul || op == KOp::Div || op == KOp::Max
           || op == KOp::Min || op == KOp::CmpLt || op == KOp::CmpEq || op == KOp::CmpLe || op == KOp::Shl
           || op == KOp::Shr || op == KOp::BitAnd || op == KOp::BitOr || op == KOp::BitXor || op == KOp::Pow
           || op == KOp::Step || op == KOp::Atan2 || op == KOp::Mod || op == KOp::CmpGt || op == KOp::CmpGe
           || op == KOp::CmpNe || op == KOp::Ldexp;
}
[[nodiscard]] inline bool is_ternary(KOp op) noexcept { return op == KOp::Clamp || op == KOp::Mix || op == KOp::Smoothstep || op == KOp::Fma || op == KOp::BitfieldExtract; }
// ── f32-FAITHFUL evaluation (2026-07-10) ─────────────────────────────────────────────────────────────────────────────
// The oracle computes in f64 but must round EVERY elementary IEEE operation to the node's dtype, or an F32 graph is not
// what an f32 GPU kernel computes. `Contract` always did this (`acc = round_dtype(acc + prod, dtype)`); the A3 vec/mat
// corpus did not — it accumulated in f64 and rounded once on store, making the oracle ~1 ULP MORE accurate than any f32
// kernel and leaving ADR-0098's T1 "certified bit-exact core" unreachable for vec/mat. `rnd()` restores the contract.
// For a F64 graph `round_dtype` is the identity, so those results are untouched.
[[nodiscard]] inline crd::f64 rnd(crd::f64 v, DType dt) noexcept { return round_dtype(v, dt); }

// determinant of a d×d column-major matrix (d ≤ 4), cofactor expansion along row 0, rounded per elementary op.
inline crd::f64 mat_det(const crd::f64* m, int d, DType dt)
{
    if (d == 1) { return m[0]; }
    if (d == 2) { return rnd(rnd(m[0] * m[3], dt) - rnd(m[2] * m[1], dt), dt); }
    crd::f64 det = 0.0;
    crd::f64 minor[9];
    for (int col0 = 0; col0 < d; ++col0)
    {
        int mi = 0;
        for (int col = 0; col < d; ++col) { if (col == col0) { continue; } for (int row = 1; row < d; ++row) { minor[mi++] = m[col * d + row]; } }
        const crd::f64 cof = rnd(m[col0 * d] * mat_det(minor, d - 1, dt), dt);
        det                = rnd(det + ((col0 % 2 == 0) ? cof : -cof), dt); // negation is exact
    }
    return det;
}
// determinant of the (d-1)×(d-1) minor of column-major m with row `sr` + column `sc` removed.
inline crd::f64 mat_minor_det(const crd::f64* m, int d, int sr, int sc, DType dt)
{
    crd::f64 minor[9];
    int      mi = 0;
    for (int col = 0; col < d; ++col) { if (col == sc) { continue; } for (int row = 0; row < d; ++row) { if (row == sr) { continue; } minor[mi++] = m[col * d + row]; } }
    return mat_det(minor, d - 1, dt);
}
} // namespace eval_detail

// Interpret `g` on the CPU. `inputs[k]` = row-major f64 data for the k-th Input node (matching its shape). Fills `out`
// (numel = node(output_node).shape.numel()) with that node's result. `scratch` holds the per-node materialization.
// NOLINTBEGIN(readability-non-const-parameter) -- scratch (allocate/deallocate = non-const methods) + out (out[e]=... written) are genuinely non-const; the check misfires here
// NOLINTBEGIN(readability-function-size) -- this is the op-dispatch of an interpreter: one branch per KOp, each a few
// lines of index arithmetic. Splitting it into per-op functions would scatter the single CPU ORACLE every GPU backend
// proves against across ~70 call sites without removing one line of logic. Size here is the shape of the problem.
inline void eval_cpu(const KGraph& g, const crd::f64* const* inputs, crd::memory::IAllocator* scratch, int output_node,
                     crd::f64* out) noexcept
// NOLINTEND(readability-function-size)
// NOLINTEND(readability-non-const-parameter)
{
    using namespace eval_detail;
    const int                        nn = g.size();
    crd::containers::Array<crd::i64>  off(scratch);
    off.reserve(static_cast<crd::usize>(nn));
    crd::i64 total = 0;
    for (int i = 0; i < nn; ++i) { off.push_back(total); total += g.node(i).shape.numel() * g.node(i).comps(); } // vecN: comps values/element

    auto*          buf = static_cast<crd::f64*>(scratch->allocate(sizeof(crd::f64) * static_cast<crd::usize>(total), alignof(crd::f64)));
    crd::i64       oc[kMaxRank];
    crd::i64       ic[kMaxRank];
    // A4 tier-2: mark body-scoped (loop-varying) nodes — LoopIndex/LoopAcc + their consumers; a For is a barrier (its
    // result is loop-invariant). body_of[i] = the For that owns varying node i (single-level; no nesting yet).
    crd::containers::Array<crd::u8> varying(scratch);
    varying.resize(static_cast<crd::usize>(nn), 0);
    for (int i = 0; i < nn; ++i)
    {
        const KNode& v = g.node(i);
        if (v.op == KOp::For) { continue; }
        const bool loop_leaf   = v.op == KOp::LoopIndex || v.op == KOp::LoopAcc;
        bool       from_operand = (v.a >= 0 && varying[static_cast<crd::usize>(v.a)]) || (v.b >= 0 && varying[static_cast<crd::usize>(v.b)]) || (v.c >= 0 && varying[static_cast<crd::usize>(v.c)]) || (v.d >= 0 && varying[static_cast<crd::usize>(v.d)]);
        for (int k = 0; k < static_cast<int>(v.n_ext); ++k) { if (varying[static_cast<crd::usize>(g.ext_operand(v, k))]) { from_operand = true; } }
        if (loop_leaf || from_operand) { varying[static_cast<crd::usize>(i)] = 1; }
    }
    crd::containers::Array<int> body_of(scratch);
    body_of.resize(static_cast<crd::usize>(nn), -1);
    crd::containers::Array<int> rstk(scratch);
    for (int fi = 0; fi < nn; ++fi)
    {
        if (g.node(fi).op != KOp::For) { continue; }
        rstk.push_back(g.node(fi).c);
        while (rstk.size() > 0)
        {
            const int bid = rstk[rstk.size() - 1];
            rstk.resize(rstk.size() - 1);
            if (bid < 0 || !varying[static_cast<crd::usize>(bid)] || body_of[static_cast<crd::usize>(bid)] != -1) { continue; }
            body_of[static_cast<crd::usize>(bid)] = fi;
            const KNode& bn = g.node(bid);
            rstk.push_back(bn.a); rstk.push_back(bn.b); rstk.push_back(bn.c); rstk.push_back(bn.d);
        }
    }

    const auto eval_node = [&](int i)
    {
        const KNode&   n   = g.node(i);
        crd::f64*      dst = buf + off[static_cast<crd::usize>(i)];
        const crd::i64 ne  = n.shape.numel();
        const crd::i64 nc  = ne * n.comps(); // componentwise element count (vecN); == ne for scalars

        if (n.op == KOp::Input) { const crd::f64* s = inputs[n.iidx]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = s[e]; } }
        else if (n.op == KOp::Const) { const crd::f64 v = round_dtype(n.cval, n.dtype()); for (crd::i64 e = 0; e < ne; ++e) { dst[e] = v; } }
        else if (n.op == KOp::Iota) { for (crd::i64 e = 0; e < ne; ++e) { unflatten(e, n.shape, oc); dst[e] = round_dtype(static_cast<crd::f64>(oc[n.iidx]), n.dtype()); } }
        else if (is_unary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(apply_unary(n.op, a[e]), n.dtype()); } }
        else if (is_binary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(apply_binary_typed(n.op, a[e], b[e], n.dtype()), n.dtype()); } }
        else if (is_ternary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* c = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(apply_ternary(n.op, a[e], b[e], c[e]), n.dtype()); } }
        else if (n.op == KOp::Select) { const crd::f64* cc = buf + off[static_cast<crd::usize>(n.c)]; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::i64 cw = n.comps(); for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < cw; ++k) { dst[e * cw + k] = cc[e] != 0.0 ? a[e * cw + k] : b[e * cw + k]; } } }
        else if (n.op == KOp::Cast) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(a[e], n.dtype()); } }
        else if (n.op == KOp::Vec2) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e * 2] = a[e]; dst[e * 2 + 1] = b[e]; } }
        else if (n.op == KOp::Vec3) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* c = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e * 3] = a[e]; dst[e * 3 + 1] = b[e]; dst[e * 3 + 2] = c[e]; } }
        else if (n.op == KOp::VecComp) { const crd::i64 vc = g.node(n.a).comps(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = a[e * vc + n.iidx]; } }
        // B0-4 aggregates: a struct/array value is a contiguous run of components (fields/elements back to back). Make =
        // concatenate; Get = slice. The ORACLE is fully general here; the GPU emitters lower these by SROA instead.
        else if (n.op == KOp::StructMake || n.op == KOp::ArrayMake)
        {
            const crd::i64 tc = n.comps();
            crd::i64       fo = 0;
            for (int k = 0; k < static_cast<int>(n.n_ext); ++k)
            {
                const int       src = g.ext_operand(n, k);
                const crd::i64  fc  = g.node(src).comps();
                const crd::f64* sp  = buf + off[static_cast<crd::usize>(src)];
                for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 j = 0; j < fc; ++j) { dst[e * tc + fo + j] = sp[e * fc + j]; } }
                fo += fc;
            }
        }
        else if (n.op == KOp::FieldGet || n.op == KOp::ArrayGet)
        {
            const KNode&   agg = g.node(n.a);
            const crd::i64 ac  = agg.comps();
            const crd::i64 fc  = n.comps();
            const crd::i64 fo  = (n.op == KOp::FieldGet) ? g.struct_field_offset(agg.type.struct_id, n.iidx)
                                                         : static_cast<crd::i64>(n.iidx) * agg.type.elem_size();
            const crd::f64* sp = buf + off[static_cast<crd::usize>(n.a)];
            for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 j = 0; j < fc; ++j) { dst[e * fc + j] = sp[e * ac + fo + j]; } }
        }
        // A3 corpus: round every elementary op to the node dtype (see `rnd` above) so an F32 graph == an f32 kernel.
        else if (n.op == KOp::Dot) { const DType dt = n.dtype(); const crd::i64 cw = g.node(n.a).comps(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < cw; ++k) { s = rnd(s + rnd(a[e * cw + k] * b[e * cw + k], dt), dt); } dst[e] = s; } }
        else if (n.op == KOp::Cross) { const DType dt = n.dtype(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::i64 o = e * 3; dst[o] = rnd(rnd(a[o + 1] * b[o + 2], dt) - rnd(a[o + 2] * b[o + 1], dt), dt); dst[o + 1] = rnd(rnd(a[o + 2] * b[o], dt) - rnd(a[o] * b[o + 2], dt), dt); dst[o + 2] = rnd(rnd(a[o] * b[o + 1], dt) - rnd(a[o + 1] * b[o], dt), dt); } }
        else if (n.op == KOp::Normalize) { const DType dt = n.dtype(); const crd::i64 cw = n.comps(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < cw; ++k) { s = rnd(s + rnd(a[e * cw + k] * a[e * cw + k], dt), dt); } const crd::f64 len = rnd(crd::math::sqrt(s), dt); for (crd::i64 k = 0; k < cw; ++k) { dst[e * cw + k] = rnd(a[e * cw + k] / len, dt); } } }
        else if (n.op == KOp::VecLen) { const DType dt = n.dtype(); const crd::i64 cw = g.node(n.a).comps(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < cw; ++k) { s = rnd(s + rnd(a[e * cw + k] * a[e * cw + k], dt), dt); } dst[e] = rnd(crd::math::sqrt(s), dt); } }
        else if (n.op == KOp::VecConcat) { const crd::i64 ac = g.node(n.a).comps(); const crd::i64 bc = g.node(n.b).comps(); const crd::i64 rc = ac + bc; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < ac; ++k) { dst[e * rc + k] = a[e * ac + k]; } for (crd::i64 k = 0; k < bc; ++k) { dst[e * rc + ac + k] = b[e * bc + k]; } } }
        else if (n.op == KOp::Swizzle) { const crd::i64 vc = g.node(n.a).comps(); const crd::i64 w = n.comps(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < w; ++k) { dst[e * w + k] = a[e * vc + n.perm[k]]; } } }
        // matrices are column-major flat: element (row, col) of an RxC matrix lives at [col*R + row].
        else if (n.op == KOp::MatVecMul) { const DType dt = n.dtype(); const crd::i64 mr = g.node(n.a).type.rows; const crd::i64 mc = g.node(n.a).type.cols; const crd::i64 mm = mr * mc; const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* v = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 r = 0; r < mr; ++r) { crd::f64 s = 0.0; for (crd::i64 col = 0; col < mc; ++col) { s = rnd(s + rnd(m[e * mm + col * mr + r] * v[e * mc + col], dt), dt); } dst[e * mr + r] = s; } } }
        else if (n.op == KOp::MatMatMul) { const DType dt = n.dtype(); const crd::i64 ar = g.node(n.a).type.rows; const crd::i64 ak = g.node(n.a).type.cols; const crd::i64 bc = g.node(n.b).type.cols; const crd::i64 am = ar * ak; const crd::i64 bm = ak * bc; const crd::i64 om = ar * bc; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 col = 0; col < bc; ++col) { for (crd::i64 r = 0; r < ar; ++r) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < ak; ++k) { s = rnd(s + rnd(a[e * am + k * ar + r] * b[e * bm + col * ak + k], dt), dt); } dst[e * om + col * ar + r] = s; } } } }
        else if (n.op == KOp::MatTranspose) { const crd::i64 orows = n.type.rows; const crd::i64 ocols = n.type.cols; const crd::i64 mm = orows * ocols; const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 col = 0; col < ocols; ++col) { for (crd::i64 r = 0; r < orows; ++r) { dst[e * mm + col * orows + r] = m[e * mm + r * ocols + col]; } } } }
        else if (n.op == KOp::Splat) { const crd::i64 w = n.comps(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < w; ++k) { dst[e * w + k] = a[e]; } } }
        else if (n.op == KOp::Reflect) { const DType dt = n.dtype(); const crd::i64 c = g.node(n.a).comps(); const crd::f64* iv = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* nv = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp = rnd(dp + rnd(nv[e * c + k] * iv[e * c + k], dt), dt); } for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = rnd(iv[e * c + k] - rnd(rnd(2.0 * dp, dt) * nv[e * c + k], dt), dt); } } }
        else if (n.op == KOp::Refract) { const DType dt = n.dtype(); const crd::i64 c = g.node(n.a).comps(); const crd::f64* iv = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* nv = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* ev = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp = rnd(dp + rnd(nv[e * c + k] * iv[e * c + k], dt), dt); } const crd::f64 et = ev[e]; const crd::f64 kk = rnd(1.0 - rnd(rnd(et * et, dt) * rnd(1.0 - rnd(dp * dp, dt), dt), dt), dt); if (kk < 0.0) { for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = 0.0; } } else { const crd::f64 coef = rnd(rnd(et * dp, dt) + rnd(crd::math::sqrt(kk), dt), dt); for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = rnd(rnd(et * iv[e * c + k], dt) - rnd(coef * nv[e * c + k], dt), dt); } } } }
        else if (n.op == KOp::Faceforward) { const DType dt = n.dtype(); const crd::i64 c = g.node(n.a).comps(); const crd::f64* nv = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* iv = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* rv = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp = rnd(dp + rnd(rv[e * c + k] * iv[e * c + k], dt), dt); } const crd::f64 s = dp < 0.0 ? 1.0 : -1.0; for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = s * nv[e * c + k]; } } }
        else if (n.op == KOp::VecAny) { const crd::i64 c = g.node(n.a).comps(); const crd::f64* v = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 r = 0.0; for (crd::i64 k = 0; k < c; ++k) { if (v[e * c + k] != 0.0) { r = 1.0; } } dst[e] = r; } }
        else if (n.op == KOp::VecAll) { const crd::i64 c = g.node(n.a).comps(); const crd::f64* v = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 r = 1.0; for (crd::i64 k = 0; k < c; ++k) { if (v[e * c + k] == 0.0) { r = 0.0; } } dst[e] = r; } }
        else if (n.op == KOp::OuterProduct) { const DType dt = n.dtype(); const crd::i64 ac = g.node(n.a).comps(); const crd::i64 bc = g.node(n.b).comps(); const crd::i64 rc = ac * bc; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 col = 0; col < bc; ++col) { for (crd::i64 row = 0; row < ac; ++row) { dst[e * rc + col * ac + row] = rnd(a[e * ac + row] * b[e * bc + col], dt); } } } }
        // C column vectors of R rows -> an RxC column-major matrix. Operands a/b/c/d are columns 0..C-1 (C = type.cols).
        else if (n.op == KOp::MatFromCols) { const crd::i64 mr = n.type.rows; const crd::i64 mc = n.type.cols; const crd::i64 mm = mr * mc; const int cols[4] = {n.a, n.b, n.c, n.d}; for (crd::i64 col = 0; col < mc; ++col) { const crd::f64* src = buf + off[static_cast<crd::usize>(cols[col])]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 row = 0; row < mr; ++row) { dst[e * mm + col * mr + row] = src[e * mr + row]; } } } }
        else if (n.op == KOp::Determinant) { const DType dt = n.dtype(); const crd::i64 dd = g.node(n.a).comps(); const int d = g.node(n.a).type.rows; const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = mat_det(m + e * dd, d, dt); } }
        else if (n.op == KOp::MatInverse) { const DType dt = n.dtype(); const crd::i64 dd = n.comps(); const int d = n.type.rows; const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64* me = m + e * dd; const crd::f64 det = mat_det(me, d, dt); for (int ri = 0; ri < d; ++ri) { for (int cj = 0; cj < d; ++cj) { const crd::f64 sign = ((ri + cj) % 2 == 0) ? 1.0 : -1.0; dst[e * dd + cj * d + ri] = rnd(sign * mat_minor_det(me, d, cj, ri, dt) / det, dt); } } } }
        else if (n.op == KOp::Slerp) { const DType dt = n.dtype(); const crd::i64 c = g.node(n.a).comps(); const crd::f64* av = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* bv = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* tv = buf + off[static_cast<crd::usize>(n.c)]; const auto p = [dt](crd::f64 x, crd::f64 y) { return rnd(x * y, dt); }; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 tt = tv[e]; crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp = rnd(dp + p(av[e * c + k], bv[e * c + k]), dt); } crd::f64 bs = 1.0; if (dp < 0.0) { dp = -dp; bs = -1.0; } if (dp > 0.9995) { crd::f64 tmp[4]; crd::f64 s2 = 0.0; for (crd::i64 k = 0; k < c; ++k) { tmp[k] = rnd(av[e * c + k] + p(tt, rnd(p(bs, bv[e * c + k]) - av[e * c + k], dt)), dt); s2 = rnd(s2 + p(tmp[k], tmp[k]), dt); } const crd::f64 il = rnd(1.0 / rnd(crd::math::sqrt(s2), dt), dt); for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = p(tmp[k], il); } } else { const crd::f64 th = rnd(crd::math::acos(dp), dt); const crd::f64 sn = rnd(crd::math::sin(th), dt); const crd::f64 w1 = rnd(rnd(crd::math::sin(rnd(rnd(1.0 - tt, dt) * th, dt)), dt) / sn, dt); const crd::f64 w2 = rnd(rnd(crd::math::sin(p(tt, th)), dt) / sn, dt); for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = rnd(p(w1, av[e * c + k]) + p(p(w2, bs), bv[e * c + k]), dt); } } } }
        else if (n.op == KOp::QuatMul) { const DType dt = n.dtype(); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const auto p = [dt](crd::f64 x, crd::f64 y) { return rnd(x * y, dt); }; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 ax = a[e * 4]; const crd::f64 ay = a[e * 4 + 1]; const crd::f64 az = a[e * 4 + 2]; const crd::f64 aw = a[e * 4 + 3]; const crd::f64 bx = b[e * 4]; const crd::f64 by = b[e * 4 + 1]; const crd::f64 bz = b[e * 4 + 2]; const crd::f64 bw = b[e * 4 + 3]; dst[e * 4] = rnd(rnd(rnd(p(aw, bx) + p(ax, bw), dt) + p(ay, bz), dt) - p(az, by), dt); dst[e * 4 + 1] = rnd(rnd(rnd(p(aw, by) - p(ax, bz), dt) + p(ay, bw), dt) + p(az, bx), dt); dst[e * 4 + 2] = rnd(rnd(rnd(p(aw, bz) + p(ax, by), dt) - p(ay, bx), dt) + p(az, bw), dt); dst[e * 4 + 3] = rnd(rnd(rnd(p(aw, bw) - p(ax, bx), dt) - p(ay, by), dt) - p(az, bz), dt); } }
        else if (n.op == KOp::QuatConj) { const crd::f64* q = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e * 4] = -q[e * 4]; dst[e * 4 + 1] = -q[e * 4 + 1]; dst[e * 4 + 2] = -q[e * 4 + 2]; dst[e * 4 + 3] = q[e * 4 + 3]; } }
        else if (n.op == KOp::QuatRotate) { const DType dt = n.dtype(); const crd::f64* q = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* v = buf + off[static_cast<crd::usize>(n.b)]; const auto p = [dt](crd::f64 x, crd::f64 y) { return rnd(x * y, dt); }; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 qx = q[e * 4]; const crd::f64 qy = q[e * 4 + 1]; const crd::f64 qz = q[e * 4 + 2]; const crd::f64 qw = q[e * 4 + 3]; const crd::f64 vx = v[e * 3]; const crd::f64 vy = v[e * 3 + 1]; const crd::f64 vz = v[e * 3 + 2]; const crd::f64 tx = rnd(2.0 * rnd(p(qy, vz) - p(qz, vy), dt), dt); const crd::f64 ty = rnd(2.0 * rnd(p(qz, vx) - p(qx, vz), dt), dt); const crd::f64 tz = rnd(2.0 * rnd(p(qx, vy) - p(qy, vx), dt), dt); dst[e * 3] = rnd(rnd(vx + p(qw, tx), dt) + rnd(p(qy, tz) - p(qz, ty), dt), dt); dst[e * 3 + 1] = rnd(rnd(vy + p(qw, ty), dt) + rnd(p(qz, tx) - p(qx, tz), dt), dt); dst[e * 3 + 2] = rnd(rnd(vz + p(qw, tz), dt) + rnd(p(qx, ty) - p(qy, tx), dt), dt); } }
        else if (n.op == KOp::QuatAxisAngle) { const DType dt = n.dtype(); const crd::f64* ax = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* an = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 h = rnd(an[e] * 0.5, dt); const crd::f64 s = rnd(crd::math::sin(h), dt); dst[e * 4] = rnd(ax[e * 3] * s, dt); dst[e * 4 + 1] = rnd(ax[e * 3 + 1] * s, dt); dst[e * 4 + 2] = rnd(ax[e * 3 + 2] * s, dt); dst[e * 4 + 3] = rnd(crd::math::cos(h), dt); } }
        else if (n.op == KOp::QuatToMat3) { const DType dt = n.dtype(); const crd::f64* q = buf + off[static_cast<crd::usize>(n.a)]; const auto p = [dt](crd::f64 u, crd::f64 v) { return rnd(u * v, dt); }; const auto d2 = [dt](crd::f64 v) { return rnd(2.0 * v, dt); }; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 x = q[e * 4]; const crd::f64 y = q[e * 4 + 1]; const crd::f64 z = q[e * 4 + 2]; const crd::f64 w = q[e * 4 + 3]; const crd::f64 xx = p(x, x); const crd::f64 yy = p(y, y); const crd::f64 zz = p(z, z); const crd::f64 xy = p(x, y); const crd::f64 xz = p(x, z); const crd::f64 yz = p(y, z); const crd::f64 wx = p(w, x); const crd::f64 wy = p(w, y); const crd::f64 wz = p(w, z); dst[e * 9] = rnd(1.0 - d2(rnd(yy + zz, dt)), dt); dst[e * 9 + 1] = d2(rnd(xy + wz, dt)); dst[e * 9 + 2] = d2(rnd(xz - wy, dt)); dst[e * 9 + 3] = d2(rnd(xy - wz, dt)); dst[e * 9 + 4] = rnd(1.0 - d2(rnd(xx + zz, dt)), dt); dst[e * 9 + 5] = d2(rnd(yz + wx, dt)); dst[e * 9 + 6] = d2(rnd(xz + wy, dt)); dst[e * 9 + 7] = d2(rnd(yz - wx, dt)); dst[e * 9 + 8] = rnd(1.0 - d2(rnd(xx + yy, dt)), dt); } }
        else if (n.op == KOp::Modf) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 ip = crd::math::trunc(a[e]); dst[e * 2] = ip; dst[e * 2 + 1] = a[e] - ip; } }
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
                    if (n.op == KOp::ReduceSum) { acc = round_dtype(acc + v, n.dtype()); }
                    else if (n.op == KOp::ReduceProd) { acc = round_dtype(acc * v, n.dtype()); }
                    else if (n.op == KOp::ReduceMax) { acc = (r == 0 || v > acc) ? v : acc; }
                    else if (n.op == KOp::ReduceMin) { acc = (r == 0 || v < acc) ? v : acc; }
                    else if (n.op == KOp::ArgMax) { if (r == 0 || v > arg_val) { arg_val = v; arg_idx = r; } }
                    else { if (r == 0 || v < arg_val) { arg_val = v; arg_idx = r; } } // ArgMin
                }
                dst[e] = is_argreduce(n.op) ? static_cast<crd::f64>(arg_idx) : round_dtype(acc, n.dtype());
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
                            const crd::f64 prod = round_dtype(ab[m * kk + k] * bb[k * nnn + col], n.dtype());
                            acc                 = round_dtype(acc + prod, n.dtype());
                        }
                        cb[m * nnn + col] = round_dtype(acc, n.dtype());
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
        else if (n.op == KOp::ScatterAdd) // out[M]=0, then out[idx[i]] += updates[i] (order-independent integer sum)
        {
            const crd::f64* ix  = buf + off[static_cast<crd::usize>(n.a)];
            const crd::f64* upd = buf + off[static_cast<crd::usize>(n.b)];
            const crd::i64  nin = g.node(n.a).shape.numel();
            for (crd::i64 e = 0; e < ne; ++e) { dst[e] = 0.0; }
            for (crd::i64 s = 0; s < nin; ++s)
            {
                const crd::i64 b = static_cast<crd::i64>(ix[s]);
                if (b >= 0 && b < ne) { dst[b] += upd[s]; }
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
                    acc                    = round_dtype(acc + a[row * scanlen + c], n.dtype()); // fixed ascending order = deterministic
                    dst[row * scanlen + c] = acc;
                }
            }
        }
    };

    // A4 tier-2 For handler: acc=init; loop max-count times, each element updating while it < its own (divergent) count —
    // simulates per-thread GPU loop divergence ⇒ bit-exact vs the native `for` the emitters generate.
    const auto eval_for_loop = [&](int forId)
    {
        const KNode&    f      = g.node(forId);
        const crd::i64  ne     = f.shape.numel();
        const crd::i64  comps  = f.comps();
        crd::f64*       acc    = buf + off[static_cast<crd::usize>(forId)];
        const crd::f64* initb  = buf + off[static_cast<crd::usize>(f.b)];
        for (crd::i64 e = 0; e < ne * comps; ++e) { acc[e] = initb[e]; }
        const crd::f64* countb = buf + off[static_cast<crd::usize>(f.a)];
        crd::i64        maxc   = 0;
        for (crd::i64 e = 0; e < ne; ++e) { const crd::i64 c = static_cast<crd::i64>(countb[e]); if (c > maxc) { maxc = c; } }
        for (crd::i64 it = 0; it < maxc; ++it)
        {
            for (int bid = 0; bid < forId; ++bid) // body nodes precede the For (topo); ascending = dependency order
            {
                if (body_of[static_cast<crd::usize>(bid)] != forId) { continue; }
                const KNode& bn = g.node(bid);
                crd::f64*    p  = buf + off[static_cast<crd::usize>(bid)];
                if (bn.op == KOp::LoopIndex) { for (crd::i64 e = 0; e < ne; ++e) { p[e] = static_cast<crd::f64>(it); } }
                else if (bn.op == KOp::LoopAcc) { for (crd::i64 e = 0; e < ne * comps; ++e) { p[e] = acc[e]; } }
                else { eval_node(bid); }
            }
            const crd::f64* br = buf + off[static_cast<crd::usize>(f.c)];
            for (crd::i64 e = 0; e < ne; ++e) { if (it < static_cast<crd::i64>(countb[e])) { for (crd::i64 k = 0; k < comps; ++k) { acc[e * comps + k] = br[e * comps + k]; } } }
        }
    };

    for (int i = 0; i < nn; ++i) // top-level: body-scoped nodes run INSIDE their For; everything else once, in topo order
    {
        if (varying[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::For) { eval_for_loop(i); }
        else { eval_node(i); }
    }

    const crd::i64  one = g.node(output_node).shape.numel() * g.node(output_node).comps(); // vecN: comps values/element
    const crd::f64* src = buf + off[static_cast<crd::usize>(output_node)];
    for (crd::i64 e = 0; e < one; ++e) { out[e] = src[e]; }
    scratch->deallocate(buf);
}

} // namespace crd::kir
