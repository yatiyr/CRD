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
// determinant of a d×d column-major matrix (d ≤ 4), cofactor expansion along row 0.
inline crd::f64 mat_det(const crd::f64* m, int d)
{
    if (d == 1) { return m[0]; }
    if (d == 2) { return m[0] * m[3] - m[2] * m[1]; }
    crd::f64 det = 0.0;
    crd::f64 minor[9];
    for (int col0 = 0; col0 < d; ++col0)
    {
        int mi = 0;
        for (int col = 0; col < d; ++col) { if (col == col0) { continue; } for (int row = 1; row < d; ++row) { minor[mi++] = m[col * d + row]; } }
        const crd::f64 cof = m[col0 * d] * mat_det(minor, d - 1);
        det += (col0 % 2 == 0) ? cof : -cof;
    }
    return det;
}
// determinant of the (d-1)×(d-1) minor of column-major m with row `sr` + column `sc` removed.
inline crd::f64 mat_minor_det(const crd::f64* m, int d, int sr, int sc)
{
    crd::f64 minor[9];
    int      mi = 0;
    for (int col = 0; col < d; ++col) { if (col == sc) { continue; } for (int row = 0; row < d; ++row) { if (row == sr) { continue; } minor[mi++] = m[col * d + row]; } }
    return mat_det(minor, d - 1);
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
    for (int i = 0; i < nn; ++i) { off.push_back(total); total += g.node(i).shape.numel() * g.node(i).comps; } // vecN: comps values/element

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
        if (v.op == KOp::LoopIndex || v.op == KOp::LoopAcc) { varying[static_cast<crd::usize>(i)] = 1; }
        else if ((v.a >= 0 && varying[static_cast<crd::usize>(v.a)]) || (v.b >= 0 && varying[static_cast<crd::usize>(v.b)]) || (v.c >= 0 && varying[static_cast<crd::usize>(v.c)]) || (v.d >= 0 && varying[static_cast<crd::usize>(v.d)])) { varying[static_cast<crd::usize>(i)] = 1; }
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
        const crd::i64 nc  = ne * n.comps; // componentwise element count (vecN); == ne for scalars

        if (n.op == KOp::Input) { const crd::f64* s = inputs[n.iidx]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = s[e]; } }
        else if (n.op == KOp::Const) { const crd::f64 v = round_dtype(n.cval, n.dtype); for (crd::i64 e = 0; e < ne; ++e) { dst[e] = v; } }
        else if (n.op == KOp::Iota) { for (crd::i64 e = 0; e < ne; ++e) { unflatten(e, n.shape, oc); dst[e] = round_dtype(static_cast<crd::f64>(oc[n.iidx]), n.dtype); } }
        else if (is_unary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(apply_unary(n.op, a[e]), n.dtype); } }
        else if (is_binary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(apply_binary(n.op, a[e], b[e]), n.dtype); } }
        else if (is_ternary(n.op)) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* c = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(apply_ternary(n.op, a[e], b[e], c[e]), n.dtype); } }
        else if (n.op == KOp::Select) { const crd::f64* cc = buf + off[static_cast<crd::usize>(n.c)]; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::i64 cw = n.comps; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < cw; ++k) { dst[e * cw + k] = cc[e] != 0.0 ? a[e * cw + k] : b[e * cw + k]; } } }
        else if (n.op == KOp::Cast) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < nc; ++e) { dst[e] = round_dtype(a[e], n.dtype); } }
        else if (n.op == KOp::Vec2) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e * 2] = a[e]; dst[e * 2 + 1] = b[e]; } }
        else if (n.op == KOp::Vec3) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* c = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e * 3] = a[e]; dst[e * 3 + 1] = b[e]; dst[e * 3 + 2] = c[e]; } }
        else if (n.op == KOp::VecComp) { const crd::i64 vc = g.node(n.a).comps; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = a[e * vc + n.iidx]; } }
        else if (n.op == KOp::Dot) { const crd::i64 cw = g.node(n.a).comps; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < cw; ++k) { s += a[e * cw + k] * b[e * cw + k]; } dst[e] = s; } }
        else if (n.op == KOp::Cross) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::i64 o = e * 3; dst[o] = a[o + 1] * b[o + 2] - a[o + 2] * b[o + 1]; dst[o + 1] = a[o + 2] * b[o] - a[o] * b[o + 2]; dst[o + 2] = a[o] * b[o + 1] - a[o + 1] * b[o]; } }
        else if (n.op == KOp::Normalize) { const crd::i64 cw = n.comps; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < cw; ++k) { s += a[e * cw + k] * a[e * cw + k]; } const crd::f64 len = crd::math::sqrt(s); for (crd::i64 k = 0; k < cw; ++k) { dst[e * cw + k] = a[e * cw + k] / len; } } }
        else if (n.op == KOp::VecLen) { const crd::i64 cw = g.node(n.a).comps; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < cw; ++k) { s += a[e * cw + k] * a[e * cw + k]; } dst[e] = crd::math::sqrt(s); } }
        else if (n.op == KOp::VecConcat) { const crd::i64 ac = g.node(n.a).comps; const crd::i64 bc = g.node(n.b).comps; const crd::i64 rc = ac + bc; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < ac; ++k) { dst[e * rc + k] = a[e * ac + k]; } for (crd::i64 k = 0; k < bc; ++k) { dst[e * rc + ac + k] = b[e * bc + k]; } } }
        else if (n.op == KOp::Swizzle) { const crd::i64 vc = g.node(n.a).comps; const crd::i64 w = n.comps; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < w; ++k) { dst[e * w + k] = a[e * vc + n.perm[k]]; } } }
        else if (n.op == KOp::MatVecMul) { const crd::i64 d = n.comps; const crd::i64 dd = d * d; const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* v = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 r = 0; r < d; ++r) { crd::f64 s = 0.0; for (crd::i64 col = 0; col < d; ++col) { s += m[e * dd + col * d + r] * v[e * d + col]; } dst[e * d + r] = s; } } } // column-major m[col*d+row]
        else if (n.op == KOp::MatMatMul) { const crd::i64 dd = n.comps; const crd::i64 d = dd == 16 ? 4 : (dd == 9 ? 3 : (dd == 4 ? 2 : 1)); const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 col = 0; col < d; ++col) { for (crd::i64 r = 0; r < d; ++r) { crd::f64 s = 0.0; for (crd::i64 k = 0; k < d; ++k) { s += a[e * dd + k * d + r] * b[e * dd + col * d + k]; } dst[e * dd + col * d + r] = s; } } } }
        else if (n.op == KOp::MatTranspose) { const crd::i64 dd = n.comps; const crd::i64 d = dd == 16 ? 4 : (dd == 9 ? 3 : (dd == 4 ? 2 : 1)); const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 col = 0; col < d; ++col) { for (crd::i64 r = 0; r < d; ++r) { dst[e * dd + col * d + r] = m[e * dd + r * d + col]; } } } }
        else if (n.op == KOp::Splat) { const crd::i64 w = n.comps; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 k = 0; k < w; ++k) { dst[e * w + k] = a[e]; } } }
        else if (n.op == KOp::Reflect) { const crd::i64 c = g.node(n.a).comps; const crd::f64* iv = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* nv = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp += nv[e * c + k] * iv[e * c + k]; } for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = iv[e * c + k] - 2.0 * dp * nv[e * c + k]; } } }
        else if (n.op == KOp::Refract) { const crd::i64 c = g.node(n.a).comps; const crd::f64* iv = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* nv = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* ev = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp += nv[e * c + k] * iv[e * c + k]; } const crd::f64 et = ev[e]; const crd::f64 kk = 1.0 - et * et * (1.0 - dp * dp); if (kk < 0.0) { for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = 0.0; } } else { const crd::f64 coef = et * dp + crd::math::sqrt(kk); for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = et * iv[e * c + k] - coef * nv[e * c + k]; } } } }
        else if (n.op == KOp::Faceforward) { const crd::i64 c = g.node(n.a).comps; const crd::f64* nv = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* iv = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* rv = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp += rv[e * c + k] * iv[e * c + k]; } const crd::f64 s = dp < 0.0 ? 1.0 : -1.0; for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = s * nv[e * c + k]; } } }
        else if (n.op == KOp::VecAny) { const crd::i64 c = g.node(n.a).comps; const crd::f64* v = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 r = 0.0; for (crd::i64 k = 0; k < c; ++k) { if (v[e * c + k] != 0.0) { r = 1.0; } } dst[e] = r; } }
        else if (n.op == KOp::VecAll) { const crd::i64 c = g.node(n.a).comps; const crd::f64* v = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { crd::f64 r = 1.0; for (crd::i64 k = 0; k < c; ++k) { if (v[e * c + k] == 0.0) { r = 0.0; } } dst[e] = r; } }
        else if (n.op == KOp::OuterProduct) { const crd::i64 ac = g.node(n.a).comps; const crd::i64 bc = g.node(n.b).comps; const crd::i64 rc = ac * bc; const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 col = 0; col < bc; ++col) { for (crd::i64 row = 0; row < ac; ++row) { dst[e * rc + col * ac + row] = a[e * ac + row] * b[e * bc + col]; } } } }
        else if (n.op == KOp::MatFromCols && n.comps == 16) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* c = buf + off[static_cast<crd::usize>(n.c)]; const crd::f64* dcol = buf + off[static_cast<crd::usize>(n.d)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 row = 0; row < 4; ++row) { dst[e * 16 + row] = a[e * 4 + row]; dst[e * 16 + 4 + row] = b[e * 4 + row]; dst[e * 16 + 8 + row] = c[e * 4 + row]; dst[e * 16 + 12 + row] = dcol[e * 4 + row]; } } } // 4 vec4 columns → mat4
        else if (n.op == KOp::MatFromCols) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* c = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { for (crd::i64 row = 0; row < 3; ++row) { dst[e * 9 + row] = a[e * 3 + row]; dst[e * 9 + 3 + row] = b[e * 3 + row]; dst[e * 9 + 6 + row] = c[e * 3 + row]; } } } // 3 vec3 columns → mat3 (column-major)
        else if (n.op == KOp::Determinant) { const crd::i64 dd = g.node(n.a).comps; const int d = dd == 16 ? 4 : (dd == 9 ? 3 : (dd == 4 ? 2 : 1)); const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e] = mat_det(m + e * dd, d); } }
        else if (n.op == KOp::MatInverse) { const crd::i64 dd = n.comps; const int d = dd == 16 ? 4 : (dd == 9 ? 3 : (dd == 4 ? 2 : 1)); const crd::f64* m = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64* me = m + e * dd; const crd::f64 det = mat_det(me, d); for (int ri = 0; ri < d; ++ri) { for (int cj = 0; cj < d; ++cj) { const crd::f64 sign = ((ri + cj) % 2 == 0) ? 1.0 : -1.0; dst[e * dd + cj * d + ri] = sign * mat_minor_det(me, d, cj, ri) / det; } } } }
        else if (n.op == KOp::Slerp) { const crd::i64 c = g.node(n.a).comps; const crd::f64* av = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* bv = buf + off[static_cast<crd::usize>(n.b)]; const crd::f64* tv = buf + off[static_cast<crd::usize>(n.c)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 tt = tv[e]; crd::f64 dp = 0.0; for (crd::i64 k = 0; k < c; ++k) { dp += av[e * c + k] * bv[e * c + k]; } crd::f64 bs = 1.0; if (dp < 0.0) { dp = -dp; bs = -1.0; } if (dp > 0.9995) { crd::f64 tmp[4]; crd::f64 s2 = 0.0; for (crd::i64 k = 0; k < c; ++k) { tmp[k] = av[e * c + k] + tt * (bs * bv[e * c + k] - av[e * c + k]); s2 += tmp[k] * tmp[k]; } const crd::f64 il = 1.0 / crd::math::sqrt(s2); for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = tmp[k] * il; } } else { const crd::f64 th = crd::math::acos(dp); const crd::f64 sn = crd::math::sin(th); const crd::f64 w1 = crd::math::sin((1.0 - tt) * th) / sn; const crd::f64 w2 = crd::math::sin(tt * th) / sn; for (crd::i64 k = 0; k < c; ++k) { dst[e * c + k] = w1 * av[e * c + k] + w2 * bs * bv[e * c + k]; } } } }
        else if (n.op == KOp::QuatMul) { const crd::f64* a = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* b = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 ax = a[e * 4], ay = a[e * 4 + 1], az = a[e * 4 + 2], aw = a[e * 4 + 3]; const crd::f64 bx = b[e * 4], by = b[e * 4 + 1], bz = b[e * 4 + 2], bw = b[e * 4 + 3]; dst[e * 4] = aw * bx + ax * bw + ay * bz - az * by; dst[e * 4 + 1] = aw * by - ax * bz + ay * bw + az * bx; dst[e * 4 + 2] = aw * bz + ax * by - ay * bx + az * bw; dst[e * 4 + 3] = aw * bw - ax * bx - ay * by - az * bz; } }
        else if (n.op == KOp::QuatConj) { const crd::f64* q = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { dst[e * 4] = -q[e * 4]; dst[e * 4 + 1] = -q[e * 4 + 1]; dst[e * 4 + 2] = -q[e * 4 + 2]; dst[e * 4 + 3] = q[e * 4 + 3]; } }
        else if (n.op == KOp::QuatRotate) { const crd::f64* q = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* v = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 qx = q[e * 4], qy = q[e * 4 + 1], qz = q[e * 4 + 2], qw = q[e * 4 + 3]; const crd::f64 vx = v[e * 3], vy = v[e * 3 + 1], vz = v[e * 3 + 2]; const crd::f64 tx = 2.0 * (qy * vz - qz * vy), ty = 2.0 * (qz * vx - qx * vz), tz = 2.0 * (qx * vy - qy * vx); dst[e * 3] = vx + qw * tx + (qy * tz - qz * ty); dst[e * 3 + 1] = vy + qw * ty + (qz * tx - qx * tz); dst[e * 3 + 2] = vz + qw * tz + (qx * ty - qy * tx); } }
        else if (n.op == KOp::QuatAxisAngle) { const crd::f64* ax = buf + off[static_cast<crd::usize>(n.a)]; const crd::f64* an = buf + off[static_cast<crd::usize>(n.b)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 h = an[e] * 0.5; const crd::f64 s = crd::math::sin(h); dst[e * 4] = ax[e * 3] * s; dst[e * 4 + 1] = ax[e * 3 + 1] * s; dst[e * 4 + 2] = ax[e * 3 + 2] * s; dst[e * 4 + 3] = crd::math::cos(h); } }
        else if (n.op == KOp::QuatToMat3) { const crd::f64* q = buf + off[static_cast<crd::usize>(n.a)]; for (crd::i64 e = 0; e < ne; ++e) { const crd::f64 x = q[e * 4], y = q[e * 4 + 1], z = q[e * 4 + 2], w = q[e * 4 + 3]; const crd::f64 xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z; dst[e * 9] = 1.0 - 2.0 * (yy + zz); dst[e * 9 + 1] = 2.0 * (xy + wz); dst[e * 9 + 2] = 2.0 * (xz - wy); dst[e * 9 + 3] = 2.0 * (xy - wz); dst[e * 9 + 4] = 1.0 - 2.0 * (xx + zz); dst[e * 9 + 5] = 2.0 * (yz + wx); dst[e * 9 + 6] = 2.0 * (xz + wy); dst[e * 9 + 7] = 2.0 * (yz - wx); dst[e * 9 + 8] = 1.0 - 2.0 * (xx + yy); } }
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
                    acc                    = round_dtype(acc + a[row * scanlen + c], n.dtype); // fixed ascending order = deterministic
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
        const crd::i64  comps  = f.comps;
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

    const crd::i64  one = g.node(output_node).shape.numel() * g.node(output_node).comps; // vecN: comps values/element
    const crd::f64* src = buf + off[static_cast<crd::usize>(output_node)];
    for (crd::i64 e = 0; e < one; ++e) { out[e] = src[e]; }
    scratch->deallocate(buf);
}

} // namespace crd::kir
