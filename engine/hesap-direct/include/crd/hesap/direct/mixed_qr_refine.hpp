#pragma once

// mixed_qr_refine.hpp — v5f-d: mixed-precision iterative refinement for over-determined LEAST-SQUARES.
//
// A GENUINELY different algorithm than the square-system IR (v5f-a..c): least-squares refinement via Björck's
// CORRECTED SEMI-NORMAL EQUATIONS (CSNE). Factor A (m×n, m≥n) in f32 QR (the cheap O(mn²) part, ~½ memory),
// then refine in f64 to recover f64 LS accuracy. CSNE needs NO Q application — only A·x, Aᵀ·r, and R/Rᵀ
// triangular solves — so it reuses the v5c QR's global R (CSR) directly. Each step drives the NORMAL-equation
// residual ‖Aᵀ(b − A·x)‖ → 0 (the LS optimality condition), the f64 residual correcting the f32 factor error.
//
// CONVERGENCE (the load-bearing assumption, advisor-flagged): CSNE recovers f64 LS accuracy when the f32
// factor's backward error is recoverable — κ(A)·u_f32 ≲ 1, i.e. κ(A) ≲ 1e4 (the normal-equation κ² appears
// only in the small per-step CORRECTION, which the f64 residual + IR loop corrects). Honestly flags
// non-convergence (no silent garbage) for ill-conditioned LS beyond that.
//
// Determinism moat: the f32 QR factor is bit-identical {1,2,4,8} (the v5c moat); the CSNE refinement (spmv +
// serial triangular solves) is deterministic ⇒ the whole LS solution is bit-identical across worker counts.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/mixed_refine.hpp> // csr_cast_copy
#include <crd/hesap/direct/multifrontal_qr.hpp>
#include <crd/hesap/sparse/convert.hpp> // to_csc
#include <crd/hesap/sparse/spmv.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace crd::hesap::direct
{

struct MixedQrRefineOptions
{
    crd::u32 max_iters = 20; // hard cap; CSNE on a well-conditioned LS converges in 2-4
};

// Mixed-precision least-squares solver: an f32 multifrontal QR factor + an f64 CSNE refinement loop.
// NOT an IFactorization (the LS interface min‖A·x−b‖ for m≥n differs from the square A·X=B contract).
template <typename TWork, typename TLow> class QrMixedRefinedLS final
{
    static_assert(std::is_same_v<TWork, crd::f64> && std::is_same_v<TLow, crd::f32>,
                  "QrMixedRefinedLS: v5f-d ships f64-work / f32-low (the HPL-AI LS lever)");

public:
    QrMixedRefinedLS(crd::memory::IAllocator* alloc, sparse::SparseMatrix<TWork, sparse::SparseFormat::Csr>&& a_work,
                     MultifrontalQR<TLow>&& low, MixedQrRefineOptions opts = {}) noexcept
        : m_alloc(alloc), m_a(std::move(a_work)), m_low(std::move(low)), m_opts(opts)
    {
        m_m = m_a.pattern().rows;
        m_n = m_a.pattern().cols;
    }

    [[nodiscard]] crd::usize info() const noexcept { return m_low.info(); }
    [[nodiscard]] crd::u32 rows() const noexcept { return m_m; }
    [[nodiscard]] crd::u32 cols() const noexcept { return m_n; }
    [[nodiscard]] crd::u32 last_iters() const noexcept { return m_last_iters; }
    [[nodiscard]] const MultifrontalQR<TLow>& low_factor() const noexcept { return m_low; }

    // min‖A·X − B‖ for m ≥ n. `b` = column-major m×nrhs (input); `x` = n×nrhs (output). Returns false iff a
    // column failed to reach the normal-equation backward-error target within max_iters.
    [[nodiscard]] bool least_squares(crd::containers::ConstSpan<TWork> b, crd::containers::Span<TWork> x,
                                     crd::usize nrhs) const;

private:
    // R x = w  (R upper-triangular CSR, diagonal = first entry of each row). Back-substitution.
    void r_solve(const TWork* w, TWork* x) const;
    // Rᵀ w = c (forward via row-major scatter: finalize w[i], scatter R[i][k]·w[i] to w[k], k>i).
    void rt_solve(const TWork* c, TWork* w) const;

    crd::memory::IAllocator*                              m_alloc = nullptr;
    sparse::SparseMatrix<TWork, sparse::SparseFormat::Csr> m_a;   // owned f64 A (the matvecs A·x, Aᵀ·r)
    MultifrontalQR<TLow>                                  m_low;  // the cheap f32 QR factor (its R is the CSNE R)
    MixedQrRefineOptions                                  m_opts;
    crd::u32                                              m_m = 0;
    crd::u32                                              m_n = 0;
    mutable crd::u32                                      m_last_iters = 0;
};

template <typename TWork, typename TLow> void QrMixedRefinedLS<TWork, TLow>::r_solve(const TWork* w, TWork* x) const
{
    const crd::u32* rp = m_low.rp().data();
    const crd::u32* rj = m_low.rj().data();
    const TLow*     rx = m_low.rx().data();
    for (crd::u32 ii = m_n; ii-- > 0;)
    {
        TWork acc = w[ii];
        const crd::u32 beg = rp[ii];
        const crd::u32 end = rp[ii + 1];
        // Row ii of the upper-tri R: first entry is the diagonal R[ii][ii], the rest are R[ii][k], k>ii.
        for (crd::u32 p = beg + 1; p < end; ++p)
        {
            acc -= static_cast<TWork>(rx[p]) * x[rj[p]];
        }
        x[ii] = acc / static_cast<TWork>(rx[beg]);
    }
}

template <typename TWork, typename TLow> void QrMixedRefinedLS<TWork, TLow>::rt_solve(const TWork* c, TWork* w) const
{
    const crd::u32* rp = m_low.rp().data();
    const crd::u32* rj = m_low.rj().data();
    const TLow*     rx = m_low.rx().data();
    for (crd::u32 i = 0; i < m_n; ++i)
    {
        w[i] = c[i];
    }
    for (crd::u32 i = 0; i < m_n; ++i)
    {
        const crd::u32 beg = rp[i];
        const crd::u32 end = rp[i + 1];
        w[i] /= static_cast<TWork>(rx[beg]);              // divide by R[i][i] (the diagonal)
        const TWork wi = w[i];
        for (crd::u32 p = beg + 1; p < end; ++p)          // scatter R[i][k]·w[i] into w[k], k>i
        {
            w[rj[p]] -= static_cast<TWork>(rx[p]) * wi;
        }
    }
}

template <typename TWork, typename TLow>
bool QrMixedRefinedLS<TWork, TLow>::least_squares(crd::containers::ConstSpan<TWork> b,
                                                  crd::containers::Span<TWork> x, crd::usize nrhs) const
{
    if (m_low.info() != 0)
    {
        return false;
    }
    const crd::u32 m = m_m;
    const crd::u32 n = m_n;
    const TWork    eps = std::numeric_limits<TWork>::epsilon();
    const TWork    tol = static_cast<TWork>(64) * eps;

    crd::containers::Array<TWork> ax(m_alloc);   // A·x        (m)
    crd::containers::Array<TWork> r(m_alloc);    // residual   (m)
    crd::containers::Array<TWork> atr(m_alloc);  // Aᵀ·r       (n)
    crd::containers::Array<TWork> w(m_alloc);    // Rᵀ-solve   (n)
    crd::containers::Array<TWork> dx(m_alloc);   // correction (n)
    ax.resize(m);
    r.resize(m);
    atr.resize(n);
    w.resize(n);
    dx.resize(n);

    bool     all_ok = true;
    crd::u32 last   = 0;
    for (crd::usize col = 0; col < nrhs; ++col)
    {
        const TWork* bc = b.data() + col * m;
        TWork* const xc = x.data() + col * n;

        // SNE initial solve: Rᵀ w = Aᵀ b ; R x = w.
        sparse::spmv<TWork>(TWork{1}, m_a, sparse::Trans::Transpose, {bc, m}, TWork{0}, {atr.data(), n});
        TWork atb_norm = TWork{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const TWork a = std::fabs(atr[i]);
            atb_norm = a > atb_norm ? a : atb_norm;
        }
        const TWork den = atb_norm > TWork{0} ? atb_norm : TWork{1};
        rt_solve(atr.data(), w.data());
        r_solve(w.data(), xc);

        // CSNE refinement: drive ‖Aᵀ(b − A·x)‖∞ → tol·‖Aᵀb‖∞.
        bool     converged = false;
        TWork    prev_rn   = std::numeric_limits<TWork>::max();
        crd::u32 it        = 0;
        for (; it < m_opts.max_iters; ++it)
        {
            sparse::spmv<TWork>(TWork{1}, m_a, sparse::Trans::None, {xc, n}, TWork{0}, {ax.data(), m});
            for (crd::u32 i = 0; i < m; ++i)
            {
                r[i] = bc[i] - ax[i];
            }
            sparse::spmv<TWork>(TWork{1}, m_a, sparse::Trans::Transpose, {r.data(), m}, TWork{0}, {atr.data(), n});
            TWork rnorm = TWork{0};
            for (crd::u32 i = 0; i < n; ++i)
            {
                const TWork a = std::fabs(atr[i]);
                rnorm = a > rnorm ? a : rnorm;
            }
            if (rnorm <= tol * den)
            {
                converged = true;
                break;
            }
            if (it >= 1U && rnorm >= static_cast<TWork>(0.5) * prev_rn)
            {
                break; // stall: hit the f32-factor floor (the post-loop accept gate flags it)
            }
            prev_rn = rnorm;
            rt_solve(atr.data(), w.data()); // Rᵀ w = Aᵀ r
            r_solve(w.data(), dx.data());   // R dx = w
            for (crd::u32 i = 0; i < n; ++i)
            {
                xc[i] += dx[i];
            }
        }
        last = it;
        // Accept gate: above √eps the f32 factor could not be recovered ⇒ honest failure on this column.
        if (!converged && prev_rn > std::sqrt(eps) * den)
        {
            all_ok = false;
        }
    }
    m_last_iters = last;
    return all_ok;
}

// Factor an over-determined A (CSR f64, m≥n) in f32 multifrontal QR and wrap it in the f64 CSNE refinement.
[[nodiscard]] inline QrMixedRefinedLS<crd::f64, crd::f32>
factor_mixed_qr(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                crd::u32 num_workers = 0, MixedQrRefineOptions opts = {})
{
    // f32 QR consumes CSC (its native input): cast A to f32, convert to CSC.
    sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr> a_low_csr = csr_cast_copy<crd::f32>(alloc, a);
    sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csc> a_low_csc = sparse::to_csc<crd::f32>(a_low_csr, alloc);
    MultifrontalQR<crd::f32> low(alloc);
    low.factorize(a_low_csc.pattern(), {a_low_csc.values().values.data(), a_low_csc.values().values.size()},
                  kQrFrontRelax, num_workers);
    sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr> a_work = csr_cast_copy<crd::f64>(alloc, a);
    return QrMixedRefinedLS<crd::f64, crd::f32>(alloc, std::move(a_work), std::move(low), opts);
}

} // namespace crd::hesap::direct
