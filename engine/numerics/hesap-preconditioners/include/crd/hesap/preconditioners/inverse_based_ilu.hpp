#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>     // dense LU — the recursion base case (D(mlilu)-6)
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/ordering/amd.hpp> // AMD fill-reducing reorder per level (v4j-3: the deepening lever)
#include <crd/hesap/ordering/mc64.hpp> // MC64 max-weight matching + scaling front-end (v4j follow-on)
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/sparse/convert.hpp> // transpose -> A's columns
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>
#include <memory>

#ifdef CRD_MLILU_DEBUG
#include <cstdio>
#endif

namespace crd::hesap::preconditioners
{
// ---------------------------------------------------------------------------
// InverseBasedIlu<T> -- the ILUPACK numerical core (Bollhöfer-Saad, SIAM J.
// Sci. Comput. 27(5):1627-1650, 2006). Phase 3.1.6 v4j-2a.
//
// A Crout-form incomplete LDU (Li-Saad-Chow 2004, "Crout versions of ILU")
// whose pivots are accepted or DEFERRED by a bound κ on the inverse-triangular-
// factor norms (the "inverse-based pivoting"). At step k the incremental
// Cline-Moler-Stewart-Wilkinson condition estimator gives ‖eₖᵀL⁻¹‖ and
// ‖U⁻¹eₖ‖; if either exceeds κ the row/column is rejected (deferred) — its
// elimination would blow up the inverse factor and amplify the dropped mass
// (paper §2, Corollary 2). Accepted pivots form the leading block B; deferred
// ones form the Schur complement S̃ = C − Lᴱ Dᴮ Uꜰ (S-version, eq. 4/16).
//
// v4j-2b is MULTILEVEL: the Schur complement S̃ is factored RECURSIVELY by
// another InverseBasedIlu until it is small enough (n ≤ kDenseThreshold) or the
// recursion-depth cap is hit, at which point a DENSE LU closes it exactly
// (D(mlilu)-6: dense base, never an ILUT leaf — droptol-independent terminal).
// This is what gives mesh-independent iteration counts (the AMG-like property
// ILUPACK has and the v4j-2a single-level scaffold lacked). With κ ≥ 1 the first
// pivot of every level always accepts ⇒ the Schur strictly shrinks ⇒ the
// recursion terminates. The leaf is a LinearOp<T> so apply()'s `S̃⁻¹ t` step
// (below) recurses automatically — no change to the apply itself.
//
// apply  M⁻¹r  (eq. 20, folded):  split r = (r_B accepted, r_C deferred),
//   y_B = L_B⁻¹ r_B ;  t = r_C − Lᴱ·y_B ;  y_C = S̃⁻¹ t  (leaf) ;
//   x_B = U_B⁻¹( D_B⁻¹ y_B − Uꜰ·y_C ) ;  x_C = y_C ; then unpermute.
//
// Dropping is inverse-based (ILUC §3.2): drop l_jk when |l_jk|·‖eₖᵀL⁻¹‖ ≤ ε,
// drop u_kj when |u_kj|·‖U⁻¹eₖ‖ ≤ ε. The factorization is SERIAL over pivots ⇒
// trivially thread-count-independent; only the operator's spmv is parallel (the
// v4 determinism moat). Determinism pins D(mlilu)-1..5 (session log) hold here.
//
// Real now; complex + adjoint land in v4j-2c (has_adjoint=false for v4j-2a).
// ---------------------------------------------------------------------------

// MC64 max-weight-matching + scaling front-end mode (the v4j convection follow-on).
// ILUPACK applies MC64 + symmetric scaling at EVERY level of the multilevel ILU; it is
// what makes the single-pivot inverse-based test meaningful on strongly nonsymmetric
// (convection-dominated) operators where the largest entries sit off the diagonal.
//   None       — factor A directly (the v4j-2a/2b behaviour; diffusion is already fine).
//   TopLevel   — match+scale only the top-level matrix (cheap; clean attribution).
//   EveryLevel — match+scale at every level (the full ILUPACK pipeline; the Schur of each
//                level is re-matched by its recursive InverseBasedIlu child).
enum class Mc64Mode : crd::u8 { None, TopLevel, EveryLevel };

// Dense LU leaf — the exact base case that closes the multilevel recursion
// (D(mlilu)-6). Holds a partial-pivoting LU of the (small) coarsest Schur and
// applies its inverse via solve_lu. Deterministic + droptol-independent.
template <typename T>
class DenseLuLeaf final : public crd::hesap::LinearOp<T>
{
public:
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;
    using R   = crd::hesap::dense::RealType<T>;

    DenseLuLeaf(const Csr& s, crd::memory::IAllocator* alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_n(s.rows()), m_lu(alloc, s.rows()), m_lu_adj(alloc, s.rows())
    {
        crd::hesap::dense::Matrix<T> dense(alloc, m_n, m_n);
        crd::hesap::dense::Matrix<T> dense_adj(alloc, m_n, m_n); // S̃ᴴ (conjugate-transpose) for apply_adjoint
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            for (crd::u32 j = 0; j < m_n; ++j) { dense.at(i, j) = T{}; dense_adj.at(i, j) = T{}; }
        }
        const auto* rp = s.pattern().outer_ptr.data();
        const auto* ci = s.pattern().inner_idx.data();
        const T*    vv = s.values().values.data();
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            for (crd::u32 q = rp[i]; q < rp[i + 1]; ++q)
            {
                dense.at(i, ci[q]) = vv[q];
                if constexpr (crd::hesap::dense::is_complex_v<T>) { dense_adj.at(ci[q], i) = T{vv[q].re, -vv[q].im}; }
                else { dense_adj.at(ci[q], i) = vv[q]; }
            }
        }
        factor_robust(m_lu, dense, m_n);
        factor_robust(m_lu_adj, dense_adj, m_n);
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = r[i]; }
        crd::hesap::dense::solve_lu<T, crd::hesap::dense::Layout::RowMajor>(m_lu, z);
        return true;
    }

    // S̃⁻ᴴ r via the LU of S̃ᴴ (factored at construction; the leaf is small ⇒ the second factor is cheap).
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = r[i]; }
        crd::hesap::dense::solve_lu<T, crd::hesap::dense::Layout::RowMajor>(m_lu_adj, z);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize factor_nnz() const noexcept { return static_cast<crd::usize>(m_n) * m_n; }

private:
    // Graceful degradation (feedback_incomplete_factorization_robustness). A deferred
    // Schur leaf can be numerically singular on hard non-PDE inputs (e.g. a
    // structurally-singular power-network matrix like gemat11), where every pivot
    // defers down to a singular coarsest block. Rather than asserting in solve_lu,
    // shift the diagonal by a relative floor and refactor so the leaf stays an
    // applicable (perturbed) preconditioner — the outer Krylov iteration tolerates
    // the perturbation. A preconditioner must degrade, never abort.
    static void factor_robust(crd::hesap::dense::LU<T, crd::hesap::dense::Layout::RowMajor>& lu,
                              crd::hesap::dense::Matrix<T>& dense, crd::u32 n)
    {
        namespace hd = crd::hesap::dense;
        hd::factor_lu<T, hd::Layout::RowMajor>(lu, dense);
        if (!lu.is_singular()) { return; }
        auto mag = [](const T& v) -> R {
            if constexpr (hd::is_complex_v<T>) { return crd::hesap::abs(v); }
            else { return v < R(0) ? -v : v; }
        };
        R maxdiag = R(0);
        for (crd::u32 i = 0; i < n; ++i) { const R a = mag(dense.at(i, i)); if (a > maxdiag) { maxdiag = a; } }
        if (maxdiag <= R(0)) { maxdiag = R(1); }
        R shift = std::sqrt(std::numeric_limits<R>::epsilon()) * maxdiag; // relative pivot floor
        for (int tries = 0; tries < 40 && lu.is_singular(); ++tries)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                if constexpr (hd::is_complex_v<T>) { dense.at(i, i).re += shift; }
                else { dense.at(i, i) += shift; }
            }
            hd::factor_lu<T, hd::Layout::RowMajor>(lu, dense);
            shift *= R(2); // geometric growth until non-singular (bounded retries)
        }
    }

    crd::u32                                            m_n;
    crd::hesap::dense::LU<T, crd::hesap::dense::Layout::RowMajor> m_lu;     // LU(S̃) for apply
    crd::hesap::dense::LU<T, crd::hesap::dense::Layout::RowMajor> m_lu_adj; // LU(S̃ᴴ) for apply_adjoint
};

template <typename T>
class InverseBasedIlu final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    // kappa  : inverse-factor bound κ (ILUPACK default ~5-10; <=0 ⇒ 5).
    // droptol: inverse-based drop tolerance ε (<0 ⇒ 1e-2).
    // level / max_levels / dense_threshold: recursion control (defaults give the
    // public single call; the recursion passes level+1). Schur ≤ dense_threshold
    // or level ≥ max_levels ⇒ dense-LU base (D(mlilu)-6).
    // reorder: per-level AMD fill-reducing reorder. DEFAULT ON (v4z): matches ILUPACK's
    //   always-reorder posture; on in-regime matrices it cuts iterations ~2× (sherman3)
    //   to ~4× (cd2d β=0.3) at negligible fill cost with no measured regression (v4z
    //   Step 2 bench). Pass false explicitly for the natural-order path.
    InverseBasedIlu(const Csr& a, crd::memory::IAllocator* alloc, R kappa = R(-1), R droptol = R(-1),
                    crd::u32 level = 0, crd::u32 max_levels = 50, crd::u32 dense_threshold = 64,
                    Mc64Mode mc64_mode = Mc64Mode::None, R milu = R(0), bool reorder = true)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_alloc(alloc)
        , m_n(a.rows())
        , m_kappa(kappa > R(0) ? kappa : R(5))
        , m_droptol(droptol >= R(0) ? droptol : R(1e-2))
        , m_milu(milu > R(0) ? milu : R(0)) // MILU relaxation α∈[0,1]; 0 = plain ILU (default)
        , m_level(level)
        , m_max_levels(max_levels)
        , m_dense_threshold(dense_threshold)
        , m_mc64_mode(mc64_mode)
        , m_reorder(reorder)
        , m_lb(alloc), m_ub(alloc), m_le(alloc), m_uf(alloc)
        , m_dinv(alloc)
        , m_accperm(alloc), m_defperm(alloc), m_accof(alloc)
        , m_dr(alloc), m_dc(alloc), m_colperm(alloc)
        , m_rperm(alloc), m_rinv(alloc)
        , m_yb(alloc), m_yc(alloc), m_tmp(alloc)
        , m_rscaled(alloc), m_zb(alloc)
        , m_sortbuf(alloc)
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "InverseBasedIlu: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "InverseBasedIlu: requires a compressed CSR matrix");
        // MC64 front-end (v4j convection follow-on): factor B = D_r·A·D_c·Pᶜ instead of A
        // when matching is enabled at this level; apply() unwraps the transform. With
        // None the transform is identity (the v4j-2a/2b path), bit-for-bit unchanged.
        const bool do_mc64 = (m_mc64_mode == Mc64Mode::EveryLevel)
                          || (m_mc64_mode == Mc64Mode::TopLevel && m_level == 0);
        // Per-level AMD reorder (v4j-3): factor B = P·A·Pᵀ (AMD fill-reducing perm). Since the
        // recursion builds a fresh InverseBasedIlu per Schur, reorder applies at EVERY level ⇒ each
        // Schur stays "hard" ⇒ the hierarchy deepens (the ILUPACK/HILUCSI mechanism). apply unwraps
        // A⁻¹ = Pᵀ·B⁻¹·P. Mutually exclusive with MC64 here (the proven lever is reorder alone).
        if (m_reorder)
        {
            const Csr b = build_reorder_transformed(a);
            factor(b);
            m_rscaled.resize(m_n);
            m_zb.resize(m_n);
        }
        else if (do_mc64)
        {
            const Csr b = build_mc64_transformed(a);
            factor(b);
            m_rscaled.resize(m_n);
            m_zb.resize(m_n);
        }
        else { factor(a); }
    }

    // z = M⁻¹ r. If MC64 is active at this level, B = D_r·A·D_c·Pᶜ was factored, so
    // A⁻¹ = D_c·Pᶜ·B⁻¹·D_r ⇒ apply(r) = D_c·Pᶜ·( B⁻¹·(D_r·r) ). The unwrap mirrors the
    // (adjoint-tested) MultilevelIlu scaffold (v4j-1b). None ⇒ apply_core directly.
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        if (m_reorder)
        {
            // A⁻¹ = Pᵀ·B⁻¹·P : r' = P r (gather by perm), B⁻¹ r', scatter z = Pᵀ z'.
            for (crd::u32 i = 0; i < m_n; ++i) { m_rscaled[i] = r[m_rperm[i]]; }
            (void)apply_core(crd::containers::ConstSpan<T>{m_rscaled.data(), m_n}, crd::containers::Span<T>{m_zb.data(), m_n});
            for (crd::u32 i = 0; i < m_n; ++i) { z[m_rperm[i]] = m_zb[i]; }
            return true;
        }
        if (!m_has_transform) { return apply_core(r, z); }
        for (crd::u32 i = 0; i < m_n; ++i) { m_rscaled[i] = T(static_cast<R>(m_dr[i])) * r[i]; } // D_r·r
        (void)apply_core(crd::containers::ConstSpan<T>{m_rscaled.data(), m_n},
                         crd::containers::Span<T>{m_zb.data(), m_n});                            // B⁻¹·(D_r·r)
        const auto* cp = m_colperm.data();
        for (crd::u32 k = 0; k < m_n; ++k) { z[cp[k]] = T(static_cast<R>(m_dc[cp[k]])) * m_zb[k]; } // D_c·Pᶜ
        return true;
    }

    // z = M⁻¹ r  (eq. 20, folded). Sequential triangular solves ⇒ deterministic.
    [[nodiscard]] bool apply_core(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const
    {
        const crd::u32 p = static_cast<crd::u32>(m_accperm.size());
        const crd::u32 m = static_cast<crd::u32>(m_defperm.size());
        // y_B = L_B⁻¹ r_B  (gather r into accepted order, unit-lower forward solve).
        for (crd::u32 c = 0; c < p; ++c) { m_yb[c] = r[m_accperm[c]]; }
        solve_unit_lower(m_lb, p, m_yb.data());
        // t = r_C − Lᴱ·y_B  (Lᴱ is m×p, stored by accepted column c).
        for (crd::u32 d = 0; d < m; ++d) { m_yc[d] = r[m_defperm[d]]; }
        for (crd::u32 c = 0; c < p; ++c)
        {
            const T yc = m_yb[c];
            for (crd::u32 q = m_le.ptr[c]; q < m_le.ptr[c + 1]; ++q) { m_yc[m_le.idx[q]] -= m_le.val[q] * yc; }
        }
        // y_C = S̃⁻¹ t   (leaf solve; if no deferred block, nothing to do).
        if (m > 0 && m_leaf)
        {
            (void)m_leaf->apply(crd::containers::ConstSpan<T>{m_yc.data(), m}, crd::containers::Span<T>{m_tmp.data(), m});
            for (crd::u32 d = 0; d < m; ++d) { m_yc[d] = m_tmp[d]; }
        }
        // x_B = U_B⁻¹( D_B⁻¹ y_B − Uꜰ·y_C ).  m_yb currently holds L_B⁻¹ r_B = y_B.
        for (crd::u32 c = 0; c < p; ++c)
        {
            T v = m_yb[c] * m_dinv[c]; // D_B⁻¹ y_B
            for (crd::u32 q = m_uf.ptr[c]; q < m_uf.ptr[c + 1]; ++q) { v -= m_uf.val[q] * m_yc[m_uf.idx[q]]; } // − Uꜰ y_C
            m_yb[c] = v;
        }
        solve_unit_upper(m_ub, p, m_yb.data());
        // scatter: x_B into accepted positions, x_C into deferred positions.
        for (crd::u32 c = 0; c < p; ++c) { z[m_accperm[c]] = m_yb[c]; }
        for (crd::u32 d = 0; d < m; ++d) { z[m_defperm[d]] = m_yc[d]; }
        return true;
    }

    // z = M⁻ᴴ r. MC64 active ⇒ A⁻ᴴ = D_r·B⁻ᴴ·Pᶜᵀ·D_c (mirrors the adjoint-tested MultilevelIlu
    // wrap). None ⇒ apply_adjoint_core directly.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        if (m_reorder)
        {
            // A⁻ᴴ = Pᵀ·B⁻ᴴ·P (P real permutation ⇒ same gather/scatter as apply, with the adjoint core).
            for (crd::u32 i = 0; i < m_n; ++i) { m_rscaled[i] = r[m_rperm[i]]; }
            (void)apply_adjoint_core(crd::containers::ConstSpan<T>{m_rscaled.data(), m_n}, crd::containers::Span<T>{m_zb.data(), m_n});
            for (crd::u32 i = 0; i < m_n; ++i) { z[m_rperm[i]] = m_zb[i]; }
            return true;
        }
        if (!m_has_transform) { return apply_adjoint_core(r, z); }
        const auto* cp = m_colperm.data();
        for (crd::u32 k = 0; k < m_n; ++k) { m_rscaled[k] = T(static_cast<R>(m_dc[cp[k]])) * r[cp[k]]; } // Pᶜᵀ·D_c·r
        (void)apply_adjoint_core(crd::containers::ConstSpan<T>{m_rscaled.data(), m_n},
                                 crd::containers::Span<T>{m_zb.data(), m_n});                            // B⁻ᴴ
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = T(static_cast<R>(m_dr[i])) * m_zb[i]; }              // D_r
        return true;
    }

    // z = M⁻ᴴ r  — the conjugate-transpose of the folded eq.(20) apply, derived block-wise:
    //   b   = U_B⁻ᴴ r_B ;  w_C = S̃⁻ᴴ( r_C − Uꜰᴴ b ) ;  w_B = L_B⁻ᴴ( D_B⁻ᴴ b − Lᴱᴴ w_C ).
    // Each factor is conjugate-transposed and the order reversed vs apply_core (Lᴱ/Uꜰ stored
    // by accepted col/row ⇒ their ᴴ maps deferred→accepted, the reverse of apply's accepted→deferred).
    [[nodiscard]] bool apply_adjoint_core(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const
    {
        const crd::u32 p = static_cast<crd::u32>(m_accperm.size());
        const crd::u32 m = static_cast<crd::u32>(m_defperm.size());
        // b = U_B⁻ᴴ r_B
        for (crd::u32 c = 0; c < p; ++c) { m_yb[c] = r[m_accperm[c]]; }
        solve_unit_upper_transpose(m_ub, p, m_yb.data());
        // t_C = r_C − Uꜰᴴ b   ((Uꜰᴴ b)[d] = Σ_c conj(Uꜰ[c,d]) b[c]; Uꜰ stored CSR by acc row c)
        for (crd::u32 d = 0; d < m; ++d) { m_yc[d] = r[m_defperm[d]]; }
        for (crd::u32 c = 0; c < p; ++c)
        {
            const T bc = m_yb[c];
            for (crd::u32 q = m_uf.ptr[c]; q < m_uf.ptr[c + 1]; ++q) { m_yc[m_uf.idx[q]] -= conj(m_uf.val[q]) * bc; }
        }
        // w_C = S̃⁻ᴴ t_C   (leaf adjoint)
        if (m > 0 && m_leaf)
        {
            (void)m_leaf->apply_adjoint(crd::containers::ConstSpan<T>{m_yc.data(), m},
                                        crd::containers::Span<T>{m_tmp.data(), m});
            for (crd::u32 d = 0; d < m; ++d) { m_yc[d] = m_tmp[d]; }
        }
        // w_B = L_B⁻ᴴ( D_B⁻ᴴ b − Lᴱᴴ w_C )   ((Lᴱᴴ w_C)[c] = Σ_d conj(Lᴱ[d,c]) w_C[d]; Lᴱ CSC by acc col c)
        for (crd::u32 c = 0; c < p; ++c)
        {
            T v = conj(m_dinv[c]) * m_yb[c]; // D_B⁻ᴴ b
            for (crd::u32 q = m_le.ptr[c]; q < m_le.ptr[c + 1]; ++q) { v -= conj(m_le.val[q]) * m_yc[m_le.idx[q]]; }
            m_yb[c] = v;
        }
        solve_unit_lower_transpose(m_lb, p, m_yb.data());
        for (crd::u32 c = 0; c < p; ++c) { z[m_accperm[c]] = m_yb[c]; }
        for (crd::u32 d = 0; d < m; ++d) { z[m_defperm[d]] = m_yc[d]; }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    // Diagnostics for the bench / tests.
    [[nodiscard]] crd::u32   num_accepted() const noexcept { return static_cast<crd::u32>(m_accperm.size()); }
    [[nodiscard]] crd::u32   num_deferred() const noexcept { return static_cast<crd::u32>(m_defperm.size()); }
    [[nodiscard]] crd::u32   num_levels() const noexcept { return m_total_levels; } // this level + recursive leaf
    [[nodiscard]] crd::usize factor_nnz() const noexcept
    {
        // L_B + U_B + D_B + Lᴱ + Uꜰ at this level, plus the whole recursive leaf.
        return m_lb.idx.size() + m_ub.idx.size() + m_accperm.size() + m_le.idx.size() + m_uf.idx.size() + m_leaf_nnz;
    }

private:
    // Compressed factor block (CSC for L_B/Lᴱ; CSR for U_B/Uꜰ). idx = the
    // "inner" index (row for L, col for U), local to the block.
    struct Block
    {
        crd::containers::Array<crd::u32> ptr;
        crd::containers::Array<crd::u32> idx;
        crd::containers::Array<T>        val;
        explicit Block(crd::memory::IAllocator* alloc) : ptr(alloc), idx(alloc), val(alloc) {}
        void reset() { ptr.clear(); idx.clear(); val.clear(); ptr.push_back(0); }
    };

    [[nodiscard]] static R mag(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    }

    // Solve L x = x in place (L unit lower, CSC by column, off-diagonal only).
    static void solve_unit_lower(const Block& l, crd::u32 p, T* x)
    {
        for (crd::u32 c = 0; c < p; ++c)
        {
            const T xc = x[c];
            for (crd::u32 q = l.ptr[c]; q < l.ptr[c + 1]; ++q) { x[l.idx[q]] -= l.val[q] * xc; }
        }
    }
    // Solve U x = x in place (U unit upper, CSR by row, off-diagonal only).
    static void solve_unit_upper(const Block& u, crd::u32 p, T* x)
    {
        for (crd::u32 r = p; r-- > 0;)
        {
            T xr = x[r];
            for (crd::u32 q = u.ptr[r]; q < u.ptr[r + 1]; ++q) { xr -= u.val[q] * x[u.idx[q]]; }
            x[r] = xr;
        }
    }

    [[nodiscard]] static T conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }

    // Solve Lᴴ x = x  (Lᴴ unit UPPER; L is the unit-lower CSC-by-column block). Column c of L
    // holds l(i,c) for i>c ⇒ Lᴴ[c,i]=conj(l(i,c)); process c DESCENDING (the transpose of the
    // forward unit-lower solve). For real T conj is identity. Used by apply_adjoint.
    static void solve_unit_lower_transpose(const Block& l, crd::u32 p, T* x)
    {
        for (crd::u32 c = p; c-- > 0;)
        {
            T xc = x[c];
            for (crd::u32 q = l.ptr[c]; q < l.ptr[c + 1]; ++q) { xc -= conj(l.val[q]) * x[l.idx[q]]; }
            x[c] = xc;
        }
    }
    // Solve Uᴴ x = x  (Uᴴ unit LOWER; U is the unit-upper CSR-by-row block). Row r of U holds
    // u(r,j) for j>r ⇒ Uᴴ[j,r]=conj(u(r,j)); process r ASCENDING and propagate finalized x[r]
    // forward (the transpose of the back unit-upper solve). Used by apply_adjoint.
    static void solve_unit_upper_transpose(const Block& u, crd::u32 p, T* x)
    {
        for (crd::u32 r = 0; r < p; ++r)
        {
            const T xr = x[r];
            for (crd::u32 q = u.ptr[r]; q < u.ptr[r + 1]; ++q) { x[u.idx[q]] -= conj(u.val[q]) * xr; }
        }
    }

    // ---- the Crout inverse-based-pivoting factorization ------------------
    void factor(const Csr& a)
    {
        const crd::u32 n   = m_n;
        const Csr      at  = crd::hesap::sparse::transpose<T>(a, m_alloc); // row i of `at` = column i of A
        const auto*    ria = a.pattern().outer_ptr.data();
        const auto*    cia = a.pattern().inner_idx.data();
        const T*       va  = a.values().values.data();
        const auto*    rit = at.pattern().outer_ptr.data();
        const auto*    cit = at.pattern().inner_idx.data();
        const T*       vt  = at.values().values.data();

        // Global magnitude + pivot floor (collapsed pivot guard).
        R amax = R(0);
        for (crd::usize q = 0; q < a.values().values.size(); ++q) { const R t = mag(va[q]); amax = t > amax ? t : amax; }
        const R floor = std::sqrt(std::numeric_limits<R>::epsilon()) * amax + std::numeric_limits<R>::min();

        // Factor storage (built by accepted order). L_B/Lᴱ filled post-sweep from
        // the raw accepted columns; U_B/Uꜰ from the raw accepted rows. During the
        // sweep we store the FULL accepted column (rows>k) and row (cols>k) keyed
        // by ORIGINAL index, then partition once we know the accept/defer split.
        Block lcol(m_alloc); // raw accepted columns: idx = ORIGINAL row, sorted asc
        Block urow(m_alloc); // raw accepted rows:    idx = ORIGINAL col, sorted asc
        lcol.reset();
        urow.reset();
        crd::containers::Array<T> dval(m_alloc); // pivot per accepted column

        m_accperm.clear();
        m_defperm.clear();
        m_accof.resize(n);
        for (crd::u32 i = 0; i < n; ++i) { m_accof[i] = kInvalid; }

        // ICE accumulators (Algorithm 3.1), indexed by ORIGINAL index.
        crd::containers::Array<T> nu(m_alloc), mu(m_alloc); // L-est / U-est running sums
        nu.resize(n);
        mu.resize(n);
        for (crd::u32 i = 0; i < n; ++i) { nu[i] = T{}; mu[i] = T{}; }

        // Dense scatter for the current row z (cols>=k) and column w (rows>k).
        crd::containers::Array<T>        zval(m_alloc), wval(m_alloc);
        crd::containers::Array<crd::u32> zlist(m_alloc), wlist(m_alloc);
        crd::containers::Array<crd::u8>  zmark(m_alloc), wmark(m_alloc);
        zval.resize(n); wval.resize(n); zmark.resize(n); wmark.resize(n);
        zlist.resize(n); wlist.resize(n);
        for (crd::u32 i = 0; i < n; ++i) { zmark[i] = 0; wmark[i] = 0; }

        // Bi-index linked lists over ACCEPTED columns/rows (Li-Saad-Chow §2.2).
        // *first[c] = scan position into lcol/urow for accepted column/row c.
        // *list[k] = head accepted c whose current entry sits at original index k;
        // *next[c] chains them.
        crd::containers::Array<crd::u32> lfirst(m_alloc), llist(m_alloc), lnext(m_alloc);
        crd::containers::Array<crd::u32> ufirst(m_alloc), ulist(m_alloc), unext(m_alloc);
        lfirst.resize(n); ufirst.resize(n); lnext.resize(n); unext.resize(n);
        llist.resize(n); ulist.resize(n);
        for (crd::u32 i = 0; i < n; ++i) { llist[i] = kInvalid; ulist[i] = kInvalid; }

        for (crd::u32 k = 0; k < n; ++k)
        {
            // ---- z = row k over cols >= k  (z[k] = pivot accumulation) ----
            crd::u32 zn = 0;
            auto scatter_z = [&](crd::u32 j, T v) {
                if (zmark[j] == 0) { zmark[j] = 1; zval[j] = v; zlist[zn++] = j; }
                else { zval[j] += v; }
            };
            for (crd::u32 q = ria[k]; q < ria[k + 1]; ++q) { if (cia[q] >= k) { scatter_z(cia[q], va[q]); } }
            // updates: for accepted cols i in llist[k] (l(k,i)!=0): z -= l(k,i)·d_i·u(i,k:n)
            for (crd::u32 ci = llist[k]; ci != kInvalid;)
            {
                const crd::u32 cnext = lnext[ci];
                const T        lki   = lcol.val[lfirst[ci]];  // l(k,i), i = accperm[ci]
                const T        coef  = lki * dval[ci];        // l(k,i)·d_i
                for (crd::u32 qq = ufirst[ci]; qq < urow.ptr[ci + 1]; ++qq) { scatter_z(urow.idx[qq], -coef * urow.val[qq]); }
                advance_l(ci, lcol, lfirst, llist, lnext);    // advance past row k, re-thread llist
                ci = cnext;
            }

            const T d = zmark[k] ? zval[k] : T{};

            // ---- w = col k over rows > k ----
            crd::u32 wn = 0;
            auto scatter_w = [&](crd::u32 j, T v) {
                if (wmark[j] == 0) { wmark[j] = 1; wval[j] = v; wlist[wn++] = j; }
                else { wval[j] += v; }
            };
            for (crd::u32 q = rit[k]; q < rit[k + 1]; ++q) { if (cit[q] > k) { scatter_w(cit[q], vt[q]); } }
            // updates: for accepted rows i in ulist[k] (u(i,k)!=0): w -= u(i,k)·d_i·l(k+1:n,i)
            for (crd::u32 ci = ulist[k]; ci != kInvalid;)
            {
                const crd::u32 cnext = unext[ci];
                const T        uik   = urow.val[ufirst[ci]];  // u(i,k)
                const T        coef  = uik * dval[ci];        // u(i,k)·d_i
                for (crd::u32 qq = lfirst[ci]; qq < lcol.ptr[ci + 1]; ++qq)
                {
                    if (lcol.idx[qq] > k) { scatter_w(lcol.idx[qq], -coef * lcol.val[qq]); }
                }
                advance_u(ci, urow, ufirst, ulist, unext);
                ci = cnext;
            }

            // ---- inverse-based pivot test (CMSW incremental estimator, Algorithm 3.1) ----
            const T  nuk = nu[k];
            const T  xip = T(R(1)) - nuk;  // ξ+ = 1 - ν_k
            const T  xim = T(R(-1)) - nuk; // ξ- = -1 - ν_k
            const T  xi  = (mag(xip) > mag(xim)) ? xip : (mag(xip) < mag(xim) ? xim : xip); // tie → +1 (D(mlilu)-2)
            const T  muk = mu[k];
            const T  zep = T(R(1)) - muk;
            const T  zem = T(R(-1)) - muk;
            const T  ze  = (mag(zep) > mag(zem)) ? zep : (mag(zep) < mag(zem) ? zem : zep);
            const R  estL = mag(xi);
            const R  estU = mag(ze);
#ifdef CRD_MLILU_DEBUG
            if (estL > m_dbg_maxL) { m_dbg_maxL = estL; }
            if (estU > m_dbg_maxU) { m_dbg_maxU = estU; }
#endif

            const bool pivot_ok = mag(d) >= floor;
            const bool accept   = pivot_ok && estL <= m_kappa && estU <= m_kappa;

            if (!accept)
            {
                // defer: discard z,w; ICE + linked lists untouched. (D(mlilu)-1)
                m_defperm.push_back(k);
                clear_marks(zlist, zn, zmark);
                clear_marks(wlist, wn, wmark);
                continue;
            }

            const crd::u32 c = static_cast<crd::u32>(m_accperm.size());
            m_accperm.push_back(k);
            m_accof[k] = c;
            const T pivot = d; // already floored by the accept test (|d|>=floor)
            const T dinv  = T(R(1)) / pivot;

            // store U row c: u(k,j) = z[j]/d for j>k kept by inverse-based drop.
            // (sorted ascending by original col so the bi-index scan is monotone.)
            const T drop_u = insert_sorted_row(zlist, zn, zval, k, estU, dinv, urow);
            // store L col c: l(j,k) = w[j]/d for j>k kept by inverse-based drop.
            const T drop_l = insert_sorted_row(wlist, wn, wval, k, estL, dinv, lcol);

            // MILU diagonal compensation (D(mlilu)-7): fatten the pivot by α × the dropped
            // row+col mass so the factor's action on the constant/smooth vector is preserved
            // (M·e ≈ A·e). α=m_milu (0 = plain ILU, the v4j-2 default; 1 = full modified-ILU).
            // The accuracy lever for convection-dominated problems — kept entries keep their
            // 1/d scaling (the standard MILU heuristic); only D absorbs the dropped mass.
            dval.push_back(pivot + T(m_milu) * (drop_u + drop_l));

            // commit ICE: ν_j += ξ·l(j,k);  μ_j += ζ·u(k,j)  over KEPT entries.
            for (crd::u32 q = lcol.ptr[c]; q < lcol.ptr[c + 1]; ++q) { nu[lcol.idx[q]] += xi * lcol.val[q]; }
            for (crd::u32 q = urow.ptr[c]; q < urow.ptr[c + 1]; ++q) { mu[urow.idx[q]] += ze * urow.val[q]; }

            // thread this new column/row into the bi-index at its first entry.
            thread_first(c, lcol, lfirst, llist, lnext);
            thread_first(c, urow, ufirst, ulist, unext);

            clear_marks(zlist, zn, zmark);
            clear_marks(wlist, wn, wmark);
        }

        build_apply_blocks(a, lcol, urow, dval);
    }

    // MC64 transform: store D_r, D_c, Pᶜ for the apply unwrap, return B = D_r·A·D_c·Pᶜ
    // (matched/largest entries on the diagonal, scaled toward an I-matrix). Mirrors the
    // adjoint-tested MultilevelIlu (v4j-1b) build exactly.
    [[nodiscard]] Csr build_mc64_transformed(const Csr& a)
    {
        auto mc = crd::hesap::ordering::mc64_match_and_scale<T>(a, m_alloc);
        m_dr.resize(m_n);
        m_dc.resize(m_n);
        m_colperm.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i) { m_dr[i] = mc.dr[i]; m_dc[i] = mc.dc[i]; m_colperm[i] = mc.colperm[i]; }
        m_has_transform = true;

        crd::containers::Array<crd::u32> invperm(m_alloc); // invperm[colperm[k]] = k
        invperm.resize(m_n);
        for (crd::u32 k = 0; k < m_n; ++k) { invperm[mc.colperm[k]] = k; }

        const auto* outer = a.pattern().outer_ptr.data();
        const auto* inner = a.pattern().inner_idx.data();
        const T*    vals  = a.values().values.data();
        crd::hesap::sparse::TripletBuilder<T> tb(m_alloc, m_n, m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const T dri = T(static_cast<R>(mc.dr[i]));
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
            {
                const crd::u32 j = inner[q];
                tb.add(i, invperm[j], dri * vals[q] * T(static_cast<R>(mc.dc[j])));
            }
        }
        return tb.compress();
    }

    // AMD fill-reducing reorder: store P (m_rperm[i] = original at new slot i) + Pᵀ, return B = P·A·Pᵀ.
    // B[i,j] = A[perm[i], perm[j]] ⇒ for each A entry (r,c,v): B[inv[r], inv[c]] = v. The reordered
    // factorization keeps each Schur "hard" ⇒ the multilevel hierarchy deepens (v4j-3 the convection lever).
    [[nodiscard]] Csr build_reorder_transformed(const Csr& a)
    {
        auto perm = crd::hesap::ordering::amd_order(a.pattern(), m_alloc);
        m_rperm.resize(m_n);
        m_rinv.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i) { m_rperm[i] = perm.perm[i]; m_rinv[i] = perm.inv_perm[i]; }
        const auto* outer = a.pattern().outer_ptr.data();
        const auto* inner = a.pattern().inner_idx.data();
        const T*    vals  = a.values().values.data();
        crd::hesap::sparse::TripletBuilder<T> tb(m_alloc, m_n, m_n);
        for (crd::u32 r = 0; r < m_n; ++r)
        {
            for (crd::u32 q = outer[r]; q < outer[r + 1]; ++q) { tb.add(m_rinv[r], m_rinv[inner[q]], vals[q]); }
        }
        return tb.compress();
    }

    static void clear_marks(const crd::containers::Array<crd::u32>& list, crd::u32 cnt, crd::containers::Array<crd::u8>& mark)
    {
        for (crd::u32 t = 0; t < cnt; ++t) { mark[list[t]] = 0; }
    }

    // Keep the inverse-based survivors of a scattered row/col, scaled by 1/d,
    // sorted ascending by original index, appended as one block row/column.
    // est = the ICE estimate for this factor (estL for L, estU for U): drop
    // entry e when |e/d|·est <= droptol  (ILUC §3.2, using 1/d-scaled value).
    // Returns the UNSCALED sum of the DROPPED entries (for MILU diagonal compensation).
    [[nodiscard]] T insert_sorted_row(const crd::containers::Array<crd::u32>& list, crd::u32 cnt,
                                      const crd::containers::Array<T>& vals, crd::u32 k, R est, T dinv, Block& dst)
    {
        // collect kept (orig_index, scaled_value) for indices > k
        m_sortbuf.clear();
        T dropped = T{}; // Σ of dropped UNSCALED entries (row-/col-sum MILU compensation)
        for (crd::u32 t = 0; t < cnt; ++t)
        {
            const crd::u32 j = list[t];
            if (j <= k) { continue; }
            const T sv = vals[j] * dinv;
            if (mag(sv) * est <= m_droptol) { dropped += vals[j]; continue; } // inverse-based drop
            m_sortbuf.push_back(j);
        }
        // insertion sort by original index (counts are small per row/col)
        for (crd::u32 a = 1; a < m_sortbuf.size(); ++a)
        {
            const crd::u32 key = m_sortbuf[a];
            crd::u32 b = a;
            while (b > 0 && m_sortbuf[b - 1] > key) { m_sortbuf[b] = m_sortbuf[b - 1]; --b; }
            m_sortbuf[b] = key;
        }
        for (crd::u32 a = 0; a < m_sortbuf.size(); ++a)
        {
            const crd::u32 j = m_sortbuf[a];
            dst.idx.push_back(j);
            dst.val.push_back(vals[j] * dinv);
        }
        dst.ptr.push_back(static_cast<crd::u32>(dst.idx.size()));
        return dropped;
    }

    // Thread accepted block c into its linked list at the first stored entry.
    static void thread_first(crd::u32 c, const Block& blk, crd::containers::Array<crd::u32>& first,
                             crd::containers::Array<crd::u32>& list, crd::containers::Array<crd::u32>& next)
    {
        const crd::u32 s = blk.ptr[c];
        const crd::u32 e = blk.ptr[c + 1];
        if (s < e)
        {
            first[c]            = s;
            const crd::u32 head = blk.idx[s];
            next[c]             = list[head];
            list[head]          = c;
        }
        else { first[c] = e; }
    }

    // After consuming column c's current entry, advance its scan pointer to the
    // next entry and re-thread it into llist at that entry's original row index.
    // List nodes are keyed by accepted-col local id `c`.
    static void advance_l(crd::u32 c, const Block& lcol, crd::containers::Array<crd::u32>& lfirst,
                          crd::containers::Array<crd::u32>& llist, crd::containers::Array<crd::u32>& lnext)
    {
        const crd::u32 q = lfirst[c] + 1;
        lfirst[c]        = q;
        if (q < lcol.ptr[c + 1])
        {
            const crd::u32 row = lcol.idx[q];
            lnext[c]           = llist[row];
            llist[row]         = c;
        }
    }
    static void advance_u(crd::u32 c, const Block& urow, crd::containers::Array<crd::u32>& ufirst,
                          crd::containers::Array<crd::u32>& ulist, crd::containers::Array<crd::u32>& unext)
    {
        const crd::u32 q = ufirst[c] + 1;
        ufirst[c]        = q;
        if (q < urow.ptr[c + 1])
        {
            const crd::u32 col = urow.idx[q];
            unext[c]           = ulist[col];
            ulist[col]         = c;
        }
    }

    // Partition the raw accepted columns/rows into the apply blocks L_B/U_B
    // (accepted×accepted, local-indexed) and Lᴱ/Uꜰ (accepted col/row × deferred),
    // then form S̃ = C − Lᴱ Dᴮ Uꜰ and factor it with the ILUT leaf.
    void build_apply_blocks(const Csr& a, const Block& lcol, const Block& urow, const crd::containers::Array<T>& dval)
    {
        const crd::u32 p = static_cast<crd::u32>(m_accperm.size());
        const crd::u32 m = static_cast<crd::u32>(m_defperm.size());

        // deferred-local index for an original index (kInvalid if accepted).
        crd::containers::Array<crd::u32> defof(m_alloc);
        defof.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i) { defof[i] = kInvalid; }
        for (crd::u32 d = 0; d < m; ++d) { defof[m_defperm[d]] = d; }

        m_dinv.resize(p);
        for (crd::u32 c = 0; c < p; ++c) { m_dinv[c] = T(R(1)) / dval[c]; }

        m_lb.reset(); m_ub.reset(); m_le.reset(); m_uf.reset();
        for (crd::u32 c = 0; c < p; ++c)
        {
            // L column c: split rows into accepted (L_B, local) / deferred (Lᴱ, local).
            for (crd::u32 q = lcol.ptr[c]; q < lcol.ptr[c + 1]; ++q)
            {
                const crd::u32 orow = lcol.idx[q];
                const crd::u32 acc  = m_accof[orow];
                if (acc != kInvalid) { m_lb.idx.push_back(acc); m_lb.val.push_back(lcol.val[q]); }
                else { m_le.idx.push_back(defof[orow]); m_le.val.push_back(lcol.val[q]); }
            }
            m_lb.ptr.push_back(static_cast<crd::u32>(m_lb.idx.size()));
            m_le.ptr.push_back(static_cast<crd::u32>(m_le.idx.size()));
            // U row c: split cols into accepted (U_B) / deferred (Uꜰ).
            for (crd::u32 q = urow.ptr[c]; q < urow.ptr[c + 1]; ++q)
            {
                const crd::u32 ocol = urow.idx[q];
                const crd::u32 acc  = m_accof[ocol];
                if (acc != kInvalid) { m_ub.idx.push_back(acc); m_ub.val.push_back(urow.val[q]); }
                else { m_uf.idx.push_back(defof[ocol]); m_uf.val.push_back(urow.val[q]); }
            }
            m_ub.ptr.push_back(static_cast<crd::u32>(m_ub.idx.size()));
            m_uf.ptr.push_back(static_cast<crd::u32>(m_uf.idx.size()));
        }

        m_yb.resize(p == 0 ? 1 : p);
        m_yc.resize(m == 0 ? 1 : m);
        m_tmp.resize(m == 0 ? 1 : m);

        if (m == 0)
        {
#ifdef CRD_MLILU_DEBUG
            std::fprintf(stderr, "[mlilu lvl=%u] n=%u accepted=%u deferred=0  ALL-ACCEPTED  kappa=%.3g  max_estL=%.3g max_estU=%.3g\n",
                         m_level, m_n, p, double(m_kappa), double(m_dbg_maxL), double(m_dbg_maxU));
#endif
            m_leaf.reset(); m_total_levels = 1; m_leaf_nnz = 0; return; // all accepted: single level
        }

        // S̃ = C − Lᴱ Dᴮ Uꜰ.  C = deferred×deferred of A. Accumulate per deferred
        // row d via a dense scatter (D(mlilu)-5: deterministic row-major build).
        crd::hesap::sparse::TripletBuilder<T> tb(m_alloc, m, m);
        crd::containers::Array<T>        acc(m_alloc);
        crd::containers::Array<crd::u32> alist(m_alloc);
        crd::containers::Array<crd::u8>  amark(m_alloc);
        acc.resize(m == 0 ? 1 : m); amark.resize(m == 0 ? 1 : m);
        for (crd::u32 i = 0; i < m; ++i) { amark[i] = 0; }

        const auto* ria = a.pattern().outer_ptr.data();
        const auto* cia = a.pattern().inner_idx.data();
        const T*    va  = a.values().values.data();

        // Lᴱ by deferred row: which accepted columns c touch deferred row d.
        Block le_byrow(m_alloc);
        transpose_block(m_le, m, le_byrow); // idx becomes accepted-col c, grouped by deferred row

        for (crd::u32 d = 0; d < m; ++d)
        {
            crd::u32 an = 0;
            const crd::u32 orow = m_defperm[d];
            // + C row: deferred cols of A row orow
            for (crd::u32 q = ria[orow]; q < ria[orow + 1]; ++q)
            {
                const crd::u32 dc = defof[cia[q]];
                if (dc == kInvalid) { continue; }
                if (amark[dc] == 0) { amark[dc] = 1; acc[dc] = va[q]; alist[an_push(alist, an)] = dc; ++an; }
                else { acc[dc] += va[q]; }
            }
            // − Σ_c Lᴱ[d,c]·D[c]·Uꜰ[c,:]
            for (crd::u32 qq = le_byrow.ptr[d]; qq < le_byrow.ptr[d + 1]; ++qq)
            {
                const crd::u32 c    = le_byrow.idx[qq];
                const T        coef = le_byrow.val[qq] * dval[c]; // Lᴱ[d,c]·D[c]
                for (crd::u32 r = m_uf.ptr[c]; r < m_uf.ptr[c + 1]; ++r)
                {
                    const crd::u32 dc = m_uf.idx[r];
                    const T        contrib = -coef * m_uf.val[r];
                    if (amark[dc] == 0) { amark[dc] = 1; acc[dc] = contrib; alist[an_push(alist, an)] = dc; ++an; }
                    else { acc[dc] += contrib; }
                }
            }
            // emit row d in ascending col order
            insertion_sort_u32(alist, an);
            for (crd::u32 t = 0; t < an; ++t) { const crd::u32 dc = alist[t]; tb.add(d, dc, acc[dc]); amark[dc] = 0; }
        }
        Csr shat = tb.compress();
#ifdef CRD_MLILU_DEBUG
        std::fprintf(stderr, "[mlilu lvl=%u] n=%u acc=%u def=%u defer=%.2f kappa=%.3g maxE=%.3g | LB=%zu UB=%zu LE=%zu UF=%zu Cnnz=%zu schur_nnz=%zu\n",
                     m_level, m_n, p, m, m_n ? double(m) / double(m_n) : 0.0, double(m_kappa),
                     double(m_dbg_maxL > m_dbg_maxU ? m_dbg_maxL : m_dbg_maxU),
                     m_lb.idx.size(), m_ub.idx.size(), m_le.idx.size(), m_uf.idx.size(),
                     static_cast<crd::usize>(0), shat.nnz());
#endif
        // Recurse on the Schur complement, unless it is small enough or we hit the
        // depth cap → close it with an EXACT dense LU (D(mlilu)-6). With κ ≥ 1 the
        // Schur strictly shrinks each level (≥1 pivot always accepts), so this
        // terminates; the dense base makes the terminal behaviour droptol-independent.
        if (m <= m_dense_threshold || m_level + 1 >= m_max_levels)
        {
            auto leaf      = std::make_unique<DenseLuLeaf<T>>(shat, m_alloc);
            m_leaf_nnz     = leaf->factor_nnz();
            m_total_levels = 2; // this level + the dense base
            m_leaf         = std::move(leaf);
        }
        else
        {
            auto leaf      = std::make_unique<InverseBasedIlu<T>>(shat, m_alloc, m_kappa, m_droptol,
                                                                  m_level + 1, m_max_levels, m_dense_threshold,
                                                                  m_mc64_mode, m_milu, m_reorder);
            m_leaf_nnz     = leaf->factor_nnz();
            m_total_levels = 1 + leaf->num_levels();
            m_leaf         = std::move(leaf);
        }
    }

    // group m_le (CSC by accepted col, idx=deferred-local row) into le_byrow
    // (CSR by deferred row, idx=accepted col c, val=Lᴱ[d,c]).
    void transpose_block(const Block& src_bycol, crd::u32 m, Block& dst_byrow) const
    {
        const crd::u32 p = static_cast<crd::u32>(src_bycol.ptr.size()) - 1;
        dst_byrow.ptr.clear(); dst_byrow.idx.clear(); dst_byrow.val.clear();
        dst_byrow.ptr.resize(static_cast<crd::usize>(m) + 1);
        for (crd::u32 i = 0; i <= m; ++i) { dst_byrow.ptr[i] = 0; }
        for (crd::u32 q = 0; q < src_bycol.idx.size(); ++q) { dst_byrow.ptr[src_bycol.idx[q] + 1]++; }
        for (crd::u32 i = 0; i < m; ++i) { dst_byrow.ptr[i + 1] += dst_byrow.ptr[i]; }
        dst_byrow.idx.resize(src_bycol.idx.size());
        dst_byrow.val.resize(src_bycol.idx.size());
        crd::containers::Array<crd::u32> cur(m_alloc);
        cur.resize(m == 0 ? 1 : m);
        for (crd::u32 i = 0; i < m; ++i) { cur[i] = dst_byrow.ptr[i]; }
        for (crd::u32 c = 0; c < p; ++c)
        {
            for (crd::u32 q = src_bycol.ptr[c]; q < src_bycol.ptr[c + 1]; ++q)
            {
                const crd::u32 row = src_bycol.idx[q];
                const crd::u32 dst = cur[row]++;
                dst_byrow.idx[dst] = c;
                dst_byrow.val[dst] = src_bycol.val[q];
            }
        }
    }

    [[nodiscard]] static crd::u32 an_push(crd::containers::Array<crd::u32>& list, crd::u32 an)
    {
        if (an >= list.size()) { list.push_back(0); }
        return an;
    }
    static void insertion_sort_u32(crd::containers::Array<crd::u32>& v, crd::u32 cnt)
    {
        for (crd::u32 a = 1; a < cnt; ++a)
        {
            const crd::u32 key = v[a];
            crd::u32 b = a;
            while (b > 0 && v[b - 1] > key) { v[b] = v[b - 1]; --b; }
            v[b] = key;
        }
    }

    static constexpr crd::u32 kInvalid = ~crd::u32{0};

    crd::memory::IAllocator* m_alloc;
    crd::u32                 m_n;
    R                        m_kappa;
    R                        m_droptol;
    R                        m_milu;            // MILU diagonal-compensation relaxation α (0 = plain ILU)
    crd::u32                 m_level;           // recursion depth (0 = top)
    crd::u32                 m_max_levels;      // depth cap → dense base
    crd::u32                 m_dense_threshold; // Schur ≤ this → dense base
    Mc64Mode                 m_mc64_mode;       // MC64 match+scale front-end mode (per-level)
    bool                     m_reorder;         // per-level AMD reorder (factor B = P·A·Pᵀ); v4j-3 deepening lever
    bool                     m_has_transform = false; // this level factored B = D_r·A·D_c·Pᶜ
    crd::u32                 m_total_levels = 1; // this level + recursive leaf
    crd::usize               m_leaf_nnz     = 0; // factor_nnz of the recursive/dense leaf
#ifdef CRD_MLILU_DEBUG
    R                        m_dbg_maxL = R(0), m_dbg_maxU = R(0); // max ICE estimate seen (diagnostic)
#endif

    Block                            m_lb, m_ub; // L_B (CSC), U_B (CSR), local accepted indices
    Block                            m_le, m_uf; // Lᴱ (CSC by acc col, idx=def row), Uꜰ (CSR by acc row, idx=def col)
    crd::containers::Array<T>        m_dinv;     // 1/D_B per accepted column
    crd::containers::Array<crd::u32> m_accperm;  // accepted-local c -> original index
    crd::containers::Array<crd::u32> m_defperm;  // deferred-local d -> original index
    crd::containers::Array<crd::u32> m_accof;    // original index -> accepted-local (kInvalid if deferred)
    std::unique_ptr<crd::hesap::LinearOp<T>> m_leaf; // recursive InverseBasedIlu(S̃) or DenseLuLeaf(S̃)

    // MC64 transform (apply unwrap): A⁻¹ = D_c·Pᶜ·B⁻¹·D_r. Empty when m_has_transform=false.
    crd::containers::Array<crd::f64>  m_dr, m_dc;  // row / column scaling (f64, MC64 native)
    crd::containers::Array<crd::u32>  m_colperm;   // column permutation (matched → diagonal)
    crd::containers::Array<crd::u32>  m_rperm, m_rinv; // AMD symmetric reorder: m_rperm[i]=orig at slot i

    mutable crd::containers::Array<T> m_yb, m_yc, m_tmp;   // apply scratch
    mutable crd::containers::Array<T> m_rscaled, m_zb;     // MC64 unwrap scratch (D_r·r, B-space sol)
    crd::containers::Array<crd::u32>  m_sortbuf;
};

} // namespace crd::hesap::preconditioners
