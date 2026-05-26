#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp> // transpose (for the adjoint)
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triangular_solve.hpp> // level-scheduled parallel tri-solve
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// IlupPreconditioner<T> -- ILU(p): level-of-fill incomplete LU (Saad Alg. 10.5).
// Phase 3.1.6 v4h.
//
// For GENERAL A: A ≈ L·U where fill is kept by STRUCTURAL LEVEL rather than magnitude
// (ILUT) or A's pattern only (ILU(0)). Each entry carries a fill level: A's nonzeros are
// level 0; a fill (i,j) produced by eliminating k has level lev(i,k)+lev(k,j)+1 (the
// MINIMUM over all such fill paths). An entry is kept iff its level ≤ p. ILU(0) is p=0.
// Higher p ⇒ more fill ⇒ stronger preconditioner ⇒ predictable structural fill ratio
// (the complement to ILUT's magnitude-adaptive dropping; both ship in hesap).
//
// Fused symbolic+numeric IKJ pass (Saad): dense working row + jw/jr index maps + a level
// array; fill is inserted only when newlev ≤ p (existing entries take the MIN level).
// No threshold, no lfil, no qsplit, no row-scaling — purely level-thresholded. Pivot
// floor (√ε·max|A|) on the U diagonal. L (unit lower) + U (off-diag + 1/U_ii + per-entry
// level, since U entries are fill sources for later rows) as dynamic CSR. Apply
// z = U⁻¹(L⁻¹r) via the level-scheduled parallel triangular solver (v4g infra; bit-exact,
// size-adaptive). Real + complex (general LU, no conjugation; the adjoint factors Aᴴ).
//
// Eigen ships no level-of-fill ILU(p) (only the dual-threshold IncompleteLUT) → breadth +
// the parallel SELL-spmv / level-sched-tri-solve win where the preconditioner is strong.
// -----------------------------------------------------------------------

template <typename T>
class IlupPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    IlupPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, crd::u32 p = 0)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_lf(alloc), m_uf(alloc), m_lfh(alloc), m_ufh(alloc), m_t(alloc)
        , m_sl(alloc), m_su(alloc), m_slh(alloc), m_suh(alloc), m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "IlupPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "IlupPreconditioner: requires a compressed CSR matrix");
        factor_into(a, m_lf, m_uf, p, alloc);
        Csr ah = build_conj_transpose(a, alloc);
        factor_into(ah, m_lfh, m_ufh, p, alloc);
        m_sl  = crd::hesap::sparse::build_lower_tri_schedule(m_lf.ptr.data(), m_lf.col.data(), m_n, alloc);
        m_su  = crd::hesap::sparse::build_upper_tri_schedule(m_uf.ptr.data(), m_uf.col.data(), m_n, alloc);
        m_slh = crd::hesap::sparse::build_lower_tri_schedule(m_lfh.ptr.data(), m_lfh.col.data(), m_n, alloc);
        m_suh = crd::hesap::sparse::build_upper_tri_schedule(m_ufh.ptr.data(), m_ufh.col.data(), m_n, alloc);
        m_t.resize(m_n);
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        solve(m_lf, m_uf, m_sl, m_su, r, z);
        return true;
    }
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        solve(m_lfh, m_ufh, m_slh, m_suh, r, z);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    // Factor fill (nnz of L + U incl. diagonal) — for the bench fill-ratio reporting.
    [[nodiscard]] crd::usize factor_nnz() const noexcept { return m_lf.col.size() + m_uf.col.size() + m_n; }

private:
    struct Factor
    {
        crd::containers::Array<crd::u32> ptr;
        crd::containers::Array<crd::u32> col;
        crd::containers::Array<T>        val;
        crd::containers::Array<T>        diag; // U only: 1/U_ii
        crd::containers::Array<crd::u32> lev;  // U only: per-entry fill level (fill source for later rows)
        explicit Factor(crd::memory::IAllocator* alloc) : ptr(alloc), col(alloc), val(alloc), diag(alloc), lev(alloc) {}
    };

    [[nodiscard]] static T ilup_conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }
    [[nodiscard]] static R ilup_mag(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    }
    [[nodiscard]] static Csr build_conj_transpose(const Csr& a, crd::memory::IAllocator* alloc)
    {
        Csr   at   = crd::hesap::sparse::transpose<T>(a, alloc);
        auto& vals = at.values().values;
        for (crd::usize k = 0; k < vals.size(); ++k) { vals[k] = ilup_conj(vals[k]); }
        return at;
    }

    // ILU(p) of `mat` into L (unit lower) + U (off-diag + diag + level). Fused symbolic+numeric.
    void factor_into(const Csr& mat, Factor& L, Factor& U, crd::u32 p, crd::memory::IAllocator* alloc)
    {
        const auto* ia = mat.pattern().outer_ptr.data();
        const auto* ja = mat.pattern().inner_idx.data();
        const T*    av = mat.values().values.data();
        const crd::usize nv = mat.values().values.size();
        R amax = R(0);
        for (crd::usize k = 0; k < nv; ++k) { const R m = ilup_mag(av[k]); amax = m > amax ? m : amax; }
        const R floor = std::sqrt(std::numeric_limits<R>::epsilon()) * amax + std::numeric_limits<R>::min();

        crd::containers::Array<T>        w(alloc);   // working row values
        crd::containers::Array<crd::i32> jw(alloc);  // column per working position
        crd::containers::Array<crd::i32> jr(alloc);  // reverse map col → position, -1 absent
        crd::containers::Array<crd::u32> lev(alloc); // fill level per working position
        w.resize(m_n);
        jw.resize(m_n);
        jr.resize(m_n);
        lev.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i) { jr[i] = -1; }

        L.ptr.push_back(0);
        U.ptr.push_back(0);
        U.diag.resize(m_n);

        for (crd::u32 ii = 0; ii < m_n; ++ii)
        {
            const crd::u32 j1 = ia[ii];
            const crd::u32 j2 = ia[ii + 1];
            // Unpack row ii: L at [0,lenl), diagonal at slot ii, U at [ii, ii+lenu). All level 0.
            crd::u32 lenl = 0, lenu = 1;
            jw[ii] = static_cast<crd::i32>(ii); w[ii] = T{}; jr[ii] = static_cast<crd::i32>(ii); lev[ii] = 0;
            for (crd::u32 k = j1; k < j2; ++k)
            {
                const crd::u32 col = ja[k];
                if (col < ii)      { jw[lenl] = static_cast<crd::i32>(col); w[lenl] = av[k]; lev[lenl] = 0; jr[col] = static_cast<crd::i32>(lenl); ++lenl; }
                else if (col == ii){ w[ii] = av[k]; }
                else               { const crd::u32 jp = ii + lenu; jw[jp] = static_cast<crd::i32>(col); w[jp] = av[k]; lev[jp] = 0; jr[col] = static_cast<crd::i32>(jp); ++lenu; }
            }

            crd::u32 jj = 0, lfront = 0;
            while (jj < lenl)
            {
                // Smallest column in jw[jj..lenl); swap to position jj (carry w, lev, jr).
                crd::u32 kmin = jj;
                crd::i32 jrow = jw[jj];
                for (crd::u32 j = jj + 1; j < lenl; ++j) { if (jw[j] < jrow) { jrow = jw[j]; kmin = j; } }
                if (kmin != jj)
                {
                    const crd::i32 tj = jw[jj]; jw[jj] = jw[kmin]; jw[kmin] = tj;
                    const T tw = w[jj]; w[jj] = w[kmin]; w[kmin] = tw;
                    const crd::u32 tl = lev[jj]; lev[jj] = lev[kmin]; lev[kmin] = tl;
                    jr[jw[jj]] = static_cast<crd::i32>(jj);
                    jr[jw[kmin]] = static_cast<crd::i32>(kmin);
                }
                const crd::u32 row    = static_cast<crd::u32>(jw[jj]);
                const crd::u32 levrow = lev[jj];
                jr[row] = -1;

                const T fac = w[jj] * U.diag[row]; // U.diag = 1/U_row,row
                // Combine: w -= fac · U row `row`. An EXISTING working entry is always updated
                // numerically (its level is refined to the min path); a NEW fill is created only
                // when its level levrow + lev(k,j) + 1 ≤ p (the level-of-fill drop is on fill
                // CREATION, never on entries already in the kept pattern).
                for (crd::u32 q = U.ptr[row]; q < U.ptr[row + 1]; ++q)
                {
                    const crd::u32 col    = U.col[q];
                    const crd::i32 jpos   = jr[col];
                    const crd::u32 newlev = levrow + U.lev[q] + 1;
                    if (jpos == -1 && newlev > p) { continue; } // new fill over budget ⇒ drop
                    const T s = fac * U.val[q];
                    if (jpos != -1) // existing entry: always update + refine level
                    {
                        const crd::u32 u = static_cast<crd::u32>(jpos);
                        w[u] = w[u] - s;
                        if (newlev < lev[u]) { lev[u] = newlev; }
                    }
                    else if (col >= ii) // new fill, U region
                    {
                        const crd::u32 jp = ii + lenu;
                        jw[jp] = static_cast<crd::i32>(col); w[jp] = T{} - s; lev[jp] = newlev; jr[col] = static_cast<crd::i32>(jp); ++lenu;
                    }
                    else // new fill, L region
                    {
                        jw[lenl] = static_cast<crd::i32>(col); w[lenl] = T{} - s; lev[lenl] = newlev; jr[col] = static_cast<crd::i32>(lenl); ++lenl;
                    }
                }
                // Keep the L multiplier (level ≤ p already, since it is a working-row entry); compact.
                w[lfront] = fac; jw[lfront] = static_cast<crd::i32>(row); ++lfront;
                ++jj;
            }

            // Reset jr for the U active set (L was reset during elimination).
            for (crd::u32 m = 0; m < lenu; ++m) { jr[static_cast<crd::u32>(jw[ii + m])] = -1; }

            // Store L row ii (all kept).
            for (crd::u32 m = 0; m < lfront; ++m) { L.col.push_back(static_cast<crd::u32>(jw[m])); L.val.push_back(w[m]); }
            L.ptr.push_back(static_cast<crd::u32>(L.col.size()));

            // Diagonal: pivot floor, store inverse.
            T diag = w[ii];
            if (ilup_mag(diag) < floor) { diag = T(floor); }
            U.diag[ii] = T(1) / diag;

            // Store U row ii off-diagonal (all kept), with levels.
            for (crd::u32 m = 1; m < lenu; ++m)
            {
                const crd::u32 src = ii + m;
                U.col.push_back(static_cast<crd::u32>(jw[src]));
                U.val.push_back(w[src]);
                U.lev.push_back(lev[src]);
            }
            U.ptr.push_back(static_cast<crd::u32>(U.col.size()));
        }
    }

    // z = U⁻¹(L⁻¹ r): forward L y=r (unit lower), back U z=y. Level-scheduled parallel solves.
    void solve(const Factor& L, const Factor& U, const crd::hesap::sparse::TriSchedule& sl,
               const crd::hesap::sparse::TriSchedule& su, crd::containers::ConstSpan<T> r,
               crd::containers::Span<T> z) const
    {
        crd::hesap::sparse::tri_solve_lower_levelsched<T>(L.ptr.data(), L.col.data(), L.val.data(), nullptr, sl,
                                                          r.data(), m_t.data()); // L y = r (unit)
        crd::hesap::sparse::tri_solve_upper_levelsched<T>(U.ptr.data(), U.col.data(), U.val.data(), U.diag.data(), su,
                                                          m_t.data(), z.data()); // U z = y
    }

    Factor                            m_lf, m_uf;   // ILU(p) of A
    Factor                            m_lfh, m_ufh; // ILU(p) of Aᴴ (adjoint)
    mutable crd::containers::Array<T> m_t;          // intermediate y scratch
    crd::hesap::sparse::TriSchedule   m_sl, m_su;   // level schedules (A)
    crd::hesap::sparse::TriSchedule   m_slh, m_suh; // level schedules (Aᴴ)
    crd::u32                          m_n;
};

} // namespace crd::hesap::preconditioners
