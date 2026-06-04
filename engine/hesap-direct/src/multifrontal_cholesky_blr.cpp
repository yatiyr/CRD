#include <crd/hesap/direct/multifrontal_cholesky_blr.hpp>

#include <crd/core/assert.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/blr.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>  // MultifrontalSymbolic + build_symmetric_multifrontal_symbolic
#include <crd/hesap/ordering/symbolic.hpp>        // ordering::kNoParent

#include <chrono>   // CRD_MFBLR_PROFILE phase timing (compile-gated; zero cost when off)
#include <cmath>
#include <cstdio>   // CRD_MFBLR_PROFILE report
#include <limits>
#include <utility>

namespace crd::hesap::direct
{
using crd::hesap::dense::Matrix;

template <typename T>
MultifrontalCholeskyBlr<T>::MultifrontalCholeskyBlr(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_lp(alloc), m_li(alloc), m_lx(alloc), m_ap(alloc), m_ai(alloc), m_ax(alloc)
{
}

template <typename T>
bool MultifrontalCholeskyBlr<T>::factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a,
                                           crd::usize block_size, RT tol, crd::u32 blr_min, crd::u32 num_workers)
{
    m_info = 0;
    const sparse::SparsePattern& ap = a.pattern();
    m_n = static_cast<crd::u32>(ap.cols);
    const crd::u32 n = m_n;
    const crd::u32* aouter = ap.outer_ptr.data();
    const crd::u32* ainner = ap.inner_idx.data();
    const T* aval = a.values().values.data();

    // Store A's LOWER triangle (CSC) for the iterative-refinement residual.
    m_ap.resize(static_cast<crd::usize>(n) + 1);
    m_ap[0] = 0;
    for (crd::u32 c = 0; c < n; ++c)
    {
        crd::u32 cnt = 0;
        for (crd::u32 p = aouter[c]; p < aouter[c + 1]; ++p)
        {
            if (ainner[p] >= c)
            {
                ++cnt;
            }
        }
        m_ap[c + 1] = m_ap[c] + cnt;
    }
    m_ai.resize(m_ap[n]);
    m_ax.resize(m_ap[n]);
    for (crd::u32 c = 0; c < n; ++c)
    {
        crd::u32 w = m_ap[c];
        for (crd::u32 p = aouter[c]; p < aouter[c + 1]; ++p)
        {
            if (ainner[p] >= c)
            {
                m_ai[w] = ainner[p];
                m_ax[w] = aval[p];
                ++w;
            }
        }
    }

    // ---- Phase-timing diagnostic, COMPILE-GATED behind -DCRD_MFBLR_PROFILE (off ⇒ `prof` is a false
    // const ⇒ every if(prof) branch is dead-eliminated, zero production cost; no getenv [MSVC C4996]).
    // Splits factor time into symbolic / assembly+extend-add / factor / extract + achieved GF/s; the
    // v5e-3 serial-gap probe. Build with the macro to re-profile.
    using Pclock = std::chrono::steady_clock;
#if defined(CRD_MFBLR_PROFILE)
    const bool prof = true;
#else
    const bool prof = false;
#endif
    double t_asm = 0.0;  // build front: scatter A pivots + extend-add children's Schur
    double t_fac = 0.0;  // factor_front_cholesky_dense/blr
    double t_ext = 0.0;  // L-extract triplets + Schur store
    const auto psym0 = Pclock::now();

    MultifrontalSymbolic mf = build_symmetric_multifrontal_symbolic(ap, m_alloc);
    const crd::u32 nf = mf.nfront;
    const double t_sym = std::chrono::duration<double>(Pclock::now() - psym0).count();

    // Child adjacency CSR from front_parent (ascending front order = valid postorder).
    crd::containers::Array<crd::u32> chld_ptr(m_alloc);
    chld_ptr.resize(static_cast<crd::usize>(nf) + 1);
    for (crd::u32 i = 0; i <= nf; ++i)
    {
        chld_ptr[i] = 0;
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        if (mf.front_parent[f] < nf)
        {
            ++chld_ptr[mf.front_parent[f] + 1];
        }
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        chld_ptr[f + 1] += chld_ptr[f];
    }
    crd::containers::Array<crd::u32> chld_idx(m_alloc);
    chld_idx.resize(chld_ptr[nf]);
    {
        crd::containers::Array<crd::u32> wc(m_alloc);
        wc.resize(nf);
        for (crd::u32 f = 0; f < nf; ++f)
        {
            wc[f] = chld_ptr[f];
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            if (mf.front_parent[f] < nf)
            {
                chld_idx[wc[mf.front_parent[f]]++] = f;
            }
        }
    }

    // ---- NODE-parallel: factor fronts SERIALLY; the within-front gemm (gemm_parallel_auto) uses the
    // jobs workers — parallel for big near-root fronts (where 3D flops concentrate), auto-serial for
    // small ones. No outer parallel_for ⇒ no nested-parallel_for/frame-arena hazard. The factor is a
    // pure function of the pattern + deterministic gemm reduction ⇒ bit-identical across worker counts
    // (the MOAT). `num_workers` is informational; the real parallelism is jobs::num_workers().
    (void)num_workers;
    const crd::u32 sw = 1U;
    crd::memory::IAllocator* const front_alloc = m_alloc;

    crd::containers::Array<Matrix<T>> cb(m_alloc);  // per-front Schur (front_alloc; freed once the parent consumes)
    cb.reserve(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        cb.push_back(Matrix<T>(front_alloc));
    }
    crd::containers::Array<crd::u32> loc(m_alloc);  // per-worker: global id -> front-local index
    loc.resize(static_cast<crd::usize>(sw) * n);
    crd::containers::Array<crd::u32> eamap(m_alloc);  // per-worker extend-add row map scratch (child-local -> parent-local)
    eamap.resize(static_cast<crd::usize>(sw) * n);
    crd::containers::Array<crd::u32> winfo(m_alloc);  // per-worker first-failure (non-PD) front id + 1
    winfo.resize(sw);
    for (crd::u32 w = 0; w < sw; ++w)
    {
        winfo[w] = 0;
    }

    // ---- L's CSC STRUCTURE (m_lp + m_li) PRECOMPUTED FROM THE SYMBOLIC, so the factor writes values
    // STRAIGHT INTO m_lx at known column slots (MUMPS-style direct assembly) — no triplet buffers, no
    // counting-sort scatter (the v5e-3 Leg-A serial lever: the prior extract+sort was ~22% + an untimed
    // strided scatter). Column c (the k-th pivot of front f) has rows rids[k..fnr-1] (ascending ⇒
    // m_li ascending ⇒ valid CSC), the DIAGONAL first (t=k) — matching solve_llt's "diagonal first".
    // Each global column is a pivot of exactly ONE front ⇒ disjoint m_lx slots ⇒ moat-clean (the written
    // value is the deterministic factor output, independent of worker count / front order).
    m_lp.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 c = 0; c <= n; ++c)
    {
        m_lp[c] = 0;
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 fnr = mf.row_ptr[f + 1] - mf.row_ptr[f];
        const crd::u32 np = mf.npiv(f);
        for (crd::u32 k = 0; k < np; ++k)
        {
            m_lp[mf.pivot_first[f] + k + 1] = fnr - k;  // column nnz = lower entries
        }
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        m_lp[c + 1] += m_lp[c];
    }
    m_li.resize(m_lp[n]);
    m_lx.resize(m_lp[n]);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 r0 = mf.row_ptr[f];
        const crd::u32 fnr = mf.row_ptr[f + 1] - r0;
        const crd::u32 np = mf.npiv(f);
        const crd::u32* rids = mf.row_idx.data() + r0;
        for (crd::u32 k = 0; k < np; ++k)
        {
            crd::u32 pos = m_lp[mf.pivot_first[f] + k];
            for (crd::u32 t = k; t < fnr; ++t)
            {
                m_li[pos++] = rids[t];  // ascending rows (diagonal rids[k]==c first)
            }
        }
    }

    // Per-front work; worker `wk` owns loc slice [wk*n,..) + triplet buffer wk. Each front's factor is
    // independent + deterministic; its children (lower level) are done before it runs (the level barrier).
    const auto factor_one_front = [&](crd::u32 f, crd::u32 wk)
    {
        if (winfo[wk] != 0)
        {
            return;
        }
        const auto pasm0 = prof ? Pclock::now() : Pclock::time_point{};
        crd::u32* lw = loc.data() + static_cast<crd::usize>(wk) * n;
        const crd::u32 r0 = mf.row_ptr[f];
        const crd::u32 fnr = mf.row_ptr[f + 1] - r0;
        const crd::u32 npiv = mf.npiv(f);
        const crd::u32* rids = mf.row_idx.data() + r0;
        for (crd::u32 t = 0; t < fnr; ++t)
        {
            lw[rids[t]] = t;
        }
        Matrix<T> front(front_alloc, fnr, fnr);
        front.set_zero();
        for (crd::u32 k = 0; k < npiv; ++k)
        {
            const crd::u32 c = mf.pivot_first[f] + k;
            const crd::u32 lc = lw[c];
            for (crd::u32 p = aouter[c]; p < aouter[c + 1]; ++p)
            {
                if (ainner[p] >= c)
                {
                    front.at(lw[ainner[p]], lc) += aval[p];
                }
            }
        }
        crd::u32* erow = eamap.data() + static_cast<crd::usize>(wk) * n;
        for (crd::u32 cc = chld_ptr[f]; cc < chld_ptr[f + 1]; ++cc)  // extend-add, fixed child order (moat)
        {
            const crd::u32 g = chld_idx[cc];
            const crd::u32 cnp = mf.npiv(g);
            const crd::u32 cs = (mf.row_ptr[g + 1] - mf.row_ptr[g]) - cnp;
            const crd::u32* crids = mf.row_idx.data() + mf.row_ptr[g] + cnp;
            const Matrix<T>& sch = cb[g];
            const crd::usize sld = sch.ld();
            for (crd::u32 a2 = 0; a2 < cs; ++a2)  // hoist the child→parent row gather out of the O(cs²) inner loop
            {
                erow[a2] = lw[crids[a2]];
            }
            const T* sdata = sch.data();
            for (crd::u32 a2 = 0; a2 < cs; ++a2)
            {
                const crd::u32 li = erow[a2];
                const T* srow = sdata + static_cast<crd::usize>(a2) * sld;  // contiguous child Schur row
                for (crd::u32 b2 = 0; b2 <= a2; ++b2)
                {
                    const crd::u32 lj = erow[b2];
                    if (li >= lj)
                    {
                        front.at(li, lj) += srow[b2];
                    }
                    else
                    {
                        front.at(lj, li) += srow[b2];
                    }
                }
            }
            cb[g] = Matrix<T>(front_alloc);  // free the consumed child (single freer = g's only parent)
        }
        const auto pfac0 = prof ? Pclock::now() : Pclock::time_point{};
        if (prof)
        {
            t_asm += std::chrono::duration<double>(pfac0 - pasm0).count();
        }
        const bool ok = (fnr >= blr_min) ? factor_front_cholesky_blr<T>(front_alloc, front, npiv, block_size, tol)
                                         : factor_front_cholesky_dense<T>(front_alloc, front, npiv);
        const auto pext0 = prof ? Pclock::now() : Pclock::time_point{};
        if (prof)
        {
            t_fac += std::chrono::duration<double>(pext0 - pfac0).count();
        }
        if (!ok)
        {
            winfo[wk] = f + 1;
            return;
        }
        for (crd::u32 k = 0; k < npiv; ++k)  // L values STRAIGHT into the CSC slot (rows precomputed in m_li)
        {
            crd::u32 pos = m_lp[mf.pivot_first[f] + k];
            for (crd::u32 t = k; t < fnr; ++t)
            {
                m_lx[pos++] = front.at(t, k);
            }
        }
        const crd::u32 cs = fnr - npiv;
        Matrix<T> s(front_alloc, cs, cs);
        for (crd::u32 a2 = 0; a2 < cs; ++a2)
        {
            for (crd::u32 b2 = 0; b2 <= a2; ++b2)
            {
                s.at(a2, b2) = front.at(npiv + a2, npiv + b2);
            }
        }
        cb[f] = std::move(s);
        if (prof)
        {
            t_ext += std::chrono::duration<double>(Pclock::now() - pext0).count();
        }
    };

    for (crd::u32 f = 0; f < nf; ++f)
    {
        factor_one_front(f, 0);
    }

    for (crd::u32 w = 0; w < sw; ++w)
    {
        if (winfo[w] != 0)
        {
            m_info = winfo[w];
            return false;
        }
    }
    // L CSC (m_lp/m_li/m_lx) was assembled DIRECTLY during the factor (no triplets, no counting sort).
    if (prof)
    {
        const double tot = t_sym + t_asm + t_fac + t_ext;
        // Cholesky flop count = Σ_c (col_nnz_c)² (col_nnz includes the diagonal). Achieved factor
        // GF/s = flop / t_fac DISCRIMINATES the gap: ~50 ⇒ kernel fine, gap is FILL (ordering, AMD vs
        // METIS-ND for 3D); ~20-25 ⇒ kernel headroom (then sub-profile chol/trsm/syrk). (advisor #2.)
        double flop = 0.0;
        for (crd::u32 c = 0; c < n; ++c)
        {
            const double d = static_cast<double>(m_lp[c + 1] - m_lp[c]);
            flop += d * d;
        }
        std::fprintf(stderr,
                     "[mfblr-prof] n=%u nf=%u | symbolic %.3fs (%.0f%%) | assembly %.3fs (%.0f%%) | "
                     "factor %.3fs (%.0f%%) | extract %.3fs (%.0f%%) | sum %.3fs | nnz(L)=%llu "
                     "factor-flop=%.3e GF/s=%.1f\n",
                     n, nf, t_sym, 100.0 * t_sym / tot, t_asm, 100.0 * t_asm / tot, t_fac,
                     100.0 * t_fac / tot, t_ext, 100.0 * t_ext / tot, tot,
                     static_cast<unsigned long long>(m_lp[n]), flop, flop / t_fac / 1e9);
    }
    m_info = 0;
    return true;
}

template <typename T>
void MultifrontalCholeskyBlr<T>::apply_a(const T* x, T* y) const noexcept
{
    for (crd::u32 i = 0; i < m_n; ++i)
    {
        y[i] = T{0};
    }
    for (crd::u32 c = 0; c < m_n; ++c)
    {
        for (crd::u32 p = m_ap[c]; p < m_ap[c + 1]; ++p)
        {
            const crd::u32 r = m_ai[p];
            const T v = m_ax[p];
            y[r] += v * x[c];
            if (r != c)
            {
                y[c] += v * x[r];  // symmetric upper
            }
        }
    }
}

template <typename T>
void MultifrontalCholeskyBlr<T>::solve_llt(T* x) const noexcept
{
    const crd::u32 n = m_n;
    for (crd::u32 c = 0; c < n; ++c)  // forward L·z = b
    {
        const crd::u32 d = m_lp[c];  // diagonal (m_li[d] == c)
        x[c] /= m_lx[d];
        for (crd::u32 p = d + 1; p < m_lp[c + 1]; ++p)
        {
            x[m_li[p]] -= m_lx[p] * x[c];
        }
    }
    for (crd::u32 c = n; c-- > 0;)  // backward Lᵀ·x = z
    {
        const crd::u32 d = m_lp[c];
        T s = x[c];
        for (crd::u32 p = d + 1; p < m_lp[c + 1]; ++p)
        {
            s -= m_lx[p] * x[m_li[p]];
        }
        x[c] = s / m_lx[d];
    }
}

template <typename T>
bool MultifrontalCholeskyBlr<T>::solve(crd::containers::Span<T> x, crd::u32 max_ir) const
{
    if (m_info != 0 || x.size() != m_n)
    {
        return false;
    }
    const crd::u32 n = m_n;
    crd::containers::Array<T> b(m_alloc);
    crd::containers::Array<T> r(m_alloc);
    crd::containers::Array<T> dx(m_alloc);
    b.resize(n);
    r.resize(n);
    dx.resize(n);
    RT bnorm = RT{0};
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = x[i];
        bnorm += static_cast<RT>(b[i] * b[i]);
    }
    bnorm = std::sqrt(bnorm);
    if (!(bnorm > RT{0}))
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = T{0};
        }
        return true;
    }
    solve_llt(x.data());  // x0 ≈ A⁻¹·b

    // Iterative refinement with a stagnation guard (the v5d-h discipline).
    const RT accept = static_cast<RT>(64) * std::numeric_limits<RT>::epsilon();
    RT prev = std::numeric_limits<RT>::max();
    for (crd::u32 it = 0; it < max_ir; ++it)
    {
        apply_a(x.data(), r.data());
        RT rn = RT{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            r[i] = b[i] - r[i];
            rn += static_cast<RT>(r[i] * r[i]);
        }
        rn = std::sqrt(rn) / bnorm;
        if (rn <= accept || rn >= static_cast<RT>(0.5) * prev)
        {
            break;  // converged, or stagnated at the round-off floor
        }
        prev = rn;
        for (crd::u32 i = 0; i < n; ++i)
        {
            dx[i] = r[i];
        }
        solve_llt(dx.data());
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] += dx[i];
        }
    }
    // Backward-error accept guard: report success only when the relative residual is
    // genuinely small (BLR is approximate — accurate-or-flagged, never silent garbage).
    apply_a(x.data(), r.data());
    RT rn = RT{0};
    for (crd::u32 i = 0; i < n; ++i)
    {
        r[i] = b[i] - r[i];
        rn += static_cast<RT>(r[i] * r[i]);
    }
    rn = std::sqrt(rn) / bnorm;
    return rn < static_cast<RT>(1e-7);
}

// ---- explicit instantiations (v5e-3d: real f32/f64) -------------------
template class MultifrontalCholeskyBlr<float>;
template class MultifrontalCholeskyBlr<double>;

} // namespace crd::hesap::direct
