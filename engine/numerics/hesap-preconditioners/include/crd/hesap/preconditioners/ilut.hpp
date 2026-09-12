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
// IlutPreconditioner<T> -- ILUT(lfil, droptol): dual-threshold incomplete LU
// (Saad 1994, "ILUT: a dual threshold incomplete LU factorization"). Phase 3.1.6 v4g.
//
// For GENERAL A: A ≈ L·U with DROPPING-controlled fill (vs ILU(0)'s fixed level-0
// pattern), giving a much stronger preconditioner on hard nonsymmetric systems — the
// apples-to-apples peer of Eigen's IncompleteLUT. The action z = M⁻¹r = U⁻¹(L⁻¹r):
// forward solve L y = r (unit lower), back solve U z = y. Sequential triangular solves
// ⇒ trivially bit-deterministic + thread-count independent (the v4 moat; only the
// operator's block/SELL spmv is parallel).
//
// Transcribed from SPARSKIT ilut.f (Saad Alg. 10.6). DUAL DROPPING: (1) during the IKJ
// elimination, a multiplier with |fac| ≤ droptol·tnorm is dropped (tnorm = the AVERAGE
// 1-norm of A's row, sum|a_ij|/nnz_row — SPARSKIT's measure); (2) when the row is
// stored, only the `lfil` LARGEST-magnitude entries are kept in the L part and in the U
// part SEPARATELY (the diagonal is always kept, never counted against lfil), selected by
// a quickselect (qsplit). The U diagonal carries a PIVOT FLOOR (|U_ii| < √ε·max|A|
// → √ε·max|A|) so a collapsed pivot never blows the factor up.
//
// ROW SCALING (robustness): A is implicitly row-scaled to unit ∞-norm (D_r·A, D_r[i] =
// 1/max|A row i|) before factoring, so the relative `droptol` threshold is SCALE-INVARIANT
// — without it, a single large entry inflates the average-row-norm threshold and over-drops
// the moderate entries on ill-scaled matrices (e.g. sherman3 went 295→17 iters once scaled).
// The apply re-bakes it: z = U⁻¹L⁻¹(D_r·r).
//
// PARAMS (SPARSKIT defaults, pinned for reproducibility): lfil = nnz(A)/n + 5,
// droptol = 1e-4. (Eigen IncompleteLUT defaults differ — droptol 1e-12, fillfactor 10;
// the bench states both regimes.) The L/U factors are stored as dynamic CSR (fill
// exceeds A's pattern). Real + complex (drop by magnitude; general LU, no conjugation;
// the adjoint factors Aᴴ).
// -----------------------------------------------------------------------

template <typename T>
class IlutPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    // lfil = 0 ⇒ SPARSKIT default (nnz/n + 5); droptol < 0 ⇒ default 1e-4.
    IlutPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, crd::u32 lfil = 0, R droptol = R(-1))
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_lf(alloc), m_uf(alloc), m_lfh(alloc), m_ufh(alloc), m_rs(alloc)
        , m_sl(alloc), m_su(alloc), m_slh(alloc), m_suh(alloc), m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "IlutPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "IlutPreconditioner: requires a compressed CSR matrix");
        const crd::u32 avg  = m_n > 0 ? static_cast<crd::u32>(a.nnz() / m_n) : 0U;
        const crd::u32 fill = (lfil == 0) ? (avg + 5U) : lfil;
        const R        dt   = (droptol < R(0)) ? R(1e-4) : droptol;
        factor_into(a, m_lf, m_uf, fill, dt, alloc);
        Csr ah = build_conj_transpose(a, alloc);
        factor_into(ah, m_lfh, m_ufh, fill, dt, alloc);
        // Level schedules for the parallel triangular solves (built once; reused per apply).
        m_sl  = crd::hesap::sparse::build_lower_tri_schedule(m_lf.ptr.data(), m_lf.col.data(), m_n, alloc);
        m_su  = crd::hesap::sparse::build_upper_tri_schedule(m_uf.ptr.data(), m_uf.col.data(), m_n, alloc);
        m_slh = crd::hesap::sparse::build_lower_tri_schedule(m_lfh.ptr.data(), m_lfh.col.data(), m_n, alloc);
        m_suh = crd::hesap::sparse::build_upper_tri_schedule(m_ufh.ptr.data(), m_ufh.col.data(), m_n, alloc);
        m_rs.resize(m_n);
    }

    // z = M⁻¹ r = U⁻¹(L⁻¹ r), level-scheduled parallel triangular solves.
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

    // Diagnostics for the bench: the level structure of the L/U factors (parallelism regime).
    [[nodiscard]] crd::u32 lower_levels() const noexcept { return m_sl.n_levels(); }
    [[nodiscard]] crd::u32 lower_max_width() const noexcept { return m_sl.max_width; }
    [[nodiscard]] crd::u32 upper_levels() const noexcept { return m_su.n_levels(); }
    [[nodiscard]] crd::u32 upper_max_width() const noexcept { return m_su.max_width; }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    // nnz of the factor (L + U incl. diagonal) -- bench fill-ratio reporting.
    [[nodiscard]] crd::usize factor_nnz() const noexcept { return m_lf.col.size() + m_uf.col.size() + m_n; }

private:
    // Dynamic CSR triangular factor: L (unit lower, no diagonal) or U (off-diagonal) +
    // a separate per-row diagonal array (used only by U). row r occupies [ptr[r],ptr[r+1]).
    struct Factor
    {
        crd::containers::Array<crd::u32> ptr;
        crd::containers::Array<crd::u32> col;
        crd::containers::Array<T>        val;
        crd::containers::Array<T>        diag; // U only (empty for L)
        crd::containers::Array<T>        drow; // U only: row scaling D_r (1/max|A row|) — droptol scale-invariance
        explicit Factor(crd::memory::IAllocator* alloc) : ptr(alloc), col(alloc), val(alloc), diag(alloc), drow(alloc) {}
    };

    [[nodiscard]] static T ilut_conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }
    [[nodiscard]] static R ilut_mag(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    }
    [[nodiscard]] static Csr build_conj_transpose(const Csr& a, crd::memory::IAllocator* alloc)
    {
        Csr   at   = crd::hesap::sparse::transpose<T>(a, alloc);
        auto& vals = at.values().values;
        for (crd::usize k = 0; k < vals.size(); ++k) { vals[k] = ilut_conj(vals[k]); }
        return at;
    }

    // qsplit (SPARSKIT): rearrange w[0..len) + idx[0..len) so the `ncut` largest-magnitude
    // entries occupy [0, ncut) (unordered within). Quickselect on |w|.
    static void qsplit(T* w, crd::u32* idx, crd::u32 len, crd::u32 ncut)
    {
        if (ncut == 0 || ncut >= len) { return; }
        crd::u32 first = 0, last = len - 1;
        for (;;)
        {
            crd::u32 mid    = first;
            R        abskey = ilut_mag(w[mid]);
            for (crd::u32 j = first + 1; j <= last; ++j)
            {
                if (ilut_mag(w[j]) > abskey)
                {
                    ++mid;
                    const T tw = w[mid]; w[mid] = w[j]; w[j] = tw;
                    const crd::u32 ti = idx[mid]; idx[mid] = idx[j]; idx[j] = ti;
                }
            }
            { const T tw = w[mid]; w[mid] = w[first]; w[first] = tw;
              const crd::u32 ti = idx[mid]; idx[mid] = idx[first]; idx[first] = ti; }
            if (mid == ncut) { return; }
            if (mid > ncut) { last = mid - 1; } else { first = mid + 1; }
        }
    }

    // ILUT of `mat` into factor L (unit lower) + U (upper, diagonal in U.diag).
    void factor_into(const Csr& mat, Factor& L, Factor& U, crd::u32 lfil, R droptol, crd::memory::IAllocator* alloc)
    {
        const auto* ia = mat.pattern().outer_ptr.data();
        const auto* ja = mat.pattern().inner_idx.data();
        const T*    av = mat.values().values.data();
        const crd::usize nv = mat.values().values.size();
        R amax = R(0);
        for (crd::usize k = 0; k < nv; ++k) { const R m = ilut_mag(av[k]); amax = m > amax ? m : amax; }
        const R floor = std::sqrt(std::numeric_limits<R>::epsilon()) * amax + std::numeric_limits<R>::min();

        // Row scaling D_r[i] = 1/max|A row i| (∞-norm; robust to a single outlier entry that
        // would otherwise inflate the average-norm droptol threshold). Factor D_r·A; the apply
        // scales the input by D_r (z = U⁻¹L⁻¹(D_r·r)). Makes droptol scale-invariant.
        U.drow.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            R rmax = R(0);
            for (crd::u32 k = ia[i]; k < ia[i + 1]; ++k) { const R m = ilut_mag(av[k]); rmax = m > rmax ? m : rmax; }
            U.drow[i] = T(rmax > R(0) ? R(1) / rmax : R(1));
        }

        crd::containers::Array<T>        w(alloc);    // working row values (positions: [0,lenl) L, [ii,ii+lenu) U)
        crd::containers::Array<crd::i32> jw(alloc);   // column index per position
        crd::containers::Array<crd::i32> jr(alloc);   // reverse map: jr[col] = position, -1 if absent
        w.resize(m_n);
        jw.resize(m_n);
        jr.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i) { jr[i] = -1; }

        L.ptr.push_back(0);
        U.ptr.push_back(0);
        U.diag.resize(m_n);

        for (crd::u32 ii = 0; ii < m_n; ++ii)
        {
            const crd::u32 j1 = ia[ii];
            const crd::u32 j2 = ia[ii + 1];
            R              tnorm = R(0);
            for (crd::u32 k = j1; k < j2; ++k) { tnorm += ilut_mag(av[k]); }
            // average 1-norm of the SCALED row (D_r·A): · |drow[ii]| (SPARSKIT measure, scale-invariant).
            tnorm = (j2 > j1) ? (tnorm / static_cast<R>(j2 - j1)) * ilut_mag(U.drow[ii]) : R(0);
            const R droptau = droptol * tnorm;

            // Unpack row ii (scaled by D_r[ii]): L at [0,lenl), diagonal at slot ii, U at [ii, ii+lenu).
            const T dr = U.drow[ii];
            crd::u32 lenl = 0, lenu = 1;
            jw[ii] = static_cast<crd::i32>(ii); w[ii] = T{}; jr[ii] = static_cast<crd::i32>(ii);
            for (crd::u32 k = j1; k < j2; ++k)
            {
                const crd::u32 col = ja[k];
                const T        v   = av[k] * dr;
                if (col < ii)      { jw[lenl] = static_cast<crd::i32>(col); w[lenl] = v; jr[col] = static_cast<crd::i32>(lenl); ++lenl; }
                else if (col == ii){ w[ii] = v; }
                else               { const crd::u32 jpos = ii + lenu; jw[jpos] = static_cast<crd::i32>(col); w[jpos] = v; jr[col] = static_cast<crd::i32>(jpos); ++lenu; }
            }

            // Eliminate the L part in increasing column order.
            crd::u32 jj = 0;
            crd::u32 lenl_kept = 0; // L multipliers compacted into [0, lenl_kept) of a temp at the end
            // We compact kept L entries to the FRONT (positions overwritten) as we finish them.
            crd::u32 lfront = 0;
            while (jj < lenl)
            {
                // Find the smallest column in jw[jj..lenl); swap it to position jj.
                crd::u32 kmin = jj;
                crd::i32 jrow = jw[jj];
                for (crd::u32 j = jj + 1; j < lenl; ++j) { if (jw[j] < jrow) { jrow = jw[j]; kmin = j; } }
                if (kmin != jj)
                {
                    const crd::i32 tj = jw[jj]; jw[jj] = jw[kmin]; jw[kmin] = tj;
                    const T tw = w[jj]; w[jj] = w[kmin]; w[kmin] = tw;
                    jr[jw[jj]] = static_cast<crd::i32>(jj);
                    jr[jw[kmin]] = static_cast<crd::i32>(kmin);
                }
                const crd::u32 row = static_cast<crd::u32>(jw[jj]);
                jr[row] = -1; // remove from the active set

                const T fac = w[jj] * U.diag[row]; // U.diag stores 1/U_kk (inverse) ⇒ multiply
                if (ilut_mag(fac) <= droptau) { ++jj; continue; } // drop the multiplier

                // w -= fac · U row `row` (the stored off-diagonal U entries, cols > row).
                for (crd::u32 q = U.ptr[row]; q < U.ptr[row + 1]; ++q)
                {
                    const crd::u32 col = U.col[q];
                    const T        s   = fac * U.val[q];
                    const crd::i32 jpos = jr[col];
                    if (col >= ii) // U region
                    {
                        if (jpos == -1) { const crd::u32 ip = ii + lenu; jw[ip] = static_cast<crd::i32>(col); w[ip] = T{} - s; jr[col] = static_cast<crd::i32>(ip); ++lenu; }
                        else            { w[static_cast<crd::u32>(jpos)] = w[static_cast<crd::u32>(jpos)] - s; }
                    }
                    else // L region (fill below the diagonal)
                    {
                        if (jpos == -1) { jw[lenl] = static_cast<crd::i32>(col); w[lenl] = T{} - s; jr[col] = static_cast<crd::i32>(lenl); ++lenl; }
                        else            { w[static_cast<crd::u32>(jpos)] = w[static_cast<crd::u32>(jpos)] - s; }
                    }
                }
                // Keep the multiplier fac as an L entry (compacted to the front).
                w[lfront]  = fac;
                jw[lfront] = static_cast<crd::i32>(row);
                ++lfront;
                ++jj;
            }
            lenl_kept = lfront;

            // Reset jr for the U active set (the L set was reset during elimination).
            for (crd::u32 m = 0; m < lenu; ++m) { jr[static_cast<crd::u32>(jw[ii + m])] = -1; }

            // ---- Store L row ii: drop |w| ≤ droptau, keep the lfil largest. ----
            {
                crd::u32 len = 0;
                for (crd::u32 m = 0; m < lenl_kept; ++m)
                {
                    if (ilut_mag(w[m]) > droptau) { w[len] = w[m]; jw[len] = jw[m]; ++len; }
                }
                const crd::u32 keep = len < lfil ? len : lfil;
                qsplit(w.data(), reinterpret_cast<crd::u32*>(jw.data()), len, keep);
                for (crd::u32 m = 0; m < keep; ++m) { L.col.push_back(static_cast<crd::u32>(jw[m])); L.val.push_back(w[m]); }
                L.ptr.push_back(static_cast<crd::u32>(L.col.size()));
            }

            // ---- Diagonal: pivot floor, store the INVERSE (so elimination multiplies). ----
            T diag = w[ii];
            if (ilut_mag(diag) < floor) { diag = T(floor); }
            U.diag[ii] = T(1) / diag;

            // ---- Store U row ii (off-diagonal, cols > ii): drop + keep lfil largest. ----
            // Compact survivors IN-PLACE within the U region [ii+1, ii+lenu): the write
            // position ii+1+len ≤ the read position ii+m (len ≤ m-1), so an unread U source
            // is never clobbered. (Front-compaction to w[0..] corrupts U when lenu > ii — the
            // bug that made more fill give a WORSE factor.)
            {
                crd::u32 len = 0;
                for (crd::u32 m = 1; m < lenu; ++m) // skip slot ii (the diagonal); U entries at [ii+1, ii+lenu)
                {
                    const crd::u32 src = ii + m;
                    if (ilut_mag(w[src]) > droptau) { w[ii + 1 + len] = w[src]; jw[ii + 1 + len] = jw[src]; ++len; }
                }
                const crd::u32 keep = len < lfil ? len : lfil;
                qsplit(w.data() + ii + 1, reinterpret_cast<crd::u32*>(jw.data()) + ii + 1, len, keep);
                for (crd::u32 m = 0; m < keep; ++m)
                {
                    U.col.push_back(static_cast<crd::u32>(jw[ii + 1 + m]));
                    U.val.push_back(w[ii + 1 + m]);
                }
                U.ptr.push_back(static_cast<crd::u32>(U.col.size()));
            }
        }
    }

    // z = U⁻¹(L⁻¹·D_r·r): forward L y = D_r·r (unit lower), back U z = y. Both via the
    // level-scheduled parallel triangular solver (size-adaptive; bit-exact vs sequential).
    void solve(const Factor& L, const Factor& U, const crd::hesap::sparse::TriSchedule& sl,
               const crd::hesap::sparse::TriSchedule& su, crd::containers::ConstSpan<T> r,
               crd::containers::Span<T> z) const
    {
        for (crd::u32 i = 0; i < m_n; ++i) { m_rs[i] = r[i] * U.drow[i]; } // scale input by D_r
        crd::hesap::sparse::tri_solve_lower_levelsched<T>(L.ptr.data(), L.col.data(), L.val.data(), nullptr, sl,
                                                          m_rs.data(), z.data()); // L y = D_r·r (unit)
        crd::hesap::sparse::tri_solve_upper_levelsched<T>(U.ptr.data(), U.col.data(), U.val.data(), U.diag.data(), su,
                                                          z.data(), z.data()); // U z = y (1/U_ii in diag)
    }

    Factor                            m_lf, m_uf;   // ILUT(A)
    Factor                            m_lfh, m_ufh; // ILUT(Aᴴ) for the adjoint
    mutable crd::containers::Array<T> m_rs;         // D_r-scaled rhs scratch
    crd::hesap::sparse::TriSchedule   m_sl, m_su;   // level schedules for A's L / U
    crd::hesap::sparse::TriSchedule   m_slh, m_suh; // level schedules for Aᴴ's L / U
    crd::u32                          m_n;
};

} // namespace crd::hesap::preconditioners
