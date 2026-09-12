#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp> // eig_sym / eig_herm for the SVD-optimal recycle truncation
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>    // detail::krylov_inner / krylov_mag / krylov_smlnum / krylov_conj / GivensRot
#include <crd/hesap/iterative/gmres.hpp> // detail::gmres_givens / gmres_rot_apply / gmres_neg_conj
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// GCROT(m,k) -- recycling GMRES with truncation (Hicken-de Sturler / scipy
// `gcrotmk`). Phase 3.1.6 v4e-2. Built on the v4e-1 GCR substrate.
//
// Outer iteration maintains a RECYCLE SPACE of (c, u) pairs with c = A·u and the
// c's orthonormal. Each outer cycle: (1) project the current residual onto the
// recycle space (r -= C(Cᴴr), x += U(Cᴴr)); (2) run an inner FGMRES(m) Arnoldi
// kept ORTHOGONAL to C (the projection coefficients form B = Cᴴ A V); (3) form one
// new combined direction ux = Z y − U(B y) with cx = A·ux = V·H̄·y (⊥ C by
// construction), normalize so ‖cx‖ = 1, and take the optimal step x += ⟨cx,r⟩·u,
// r -= ⟨cx,r⟩·c (a GCR step along the inner-GMRES direction); (4) truncate the
// recycle space to k vectors and append (cx, ux). Reusing the recycle space across
// outer cycles deflates the slow modes that cripple restarted GMRES(m) ⇒ fewer
// iterations at the same m. v4e-2a uses the 'oldest' truncation (drop the oldest
// pair); the SVD-optimal 'smallest' truncation is v4e-2b. Cross-solve recycling
// (the de Sturler payoff) is v4e-3.
//
// Transcribed faithfully from scipy `_gcrotmk.py` (the inner Givens least-squares
// is the shared gmres machinery). D-divergence: cx is formed as V·H̄·y (exactly ⊥
// the prior C, as scipy notes) using the stored raw Hessenberg H̄, not a fresh
// A·ux matvec.
//
// PRECONDITIONING: optional RIGHT preconditioner N = m_inv (the inner Z = N⁻¹V).
// Determinism: every reduction is KBN-pairwise dotc; the only parallel step is the
// operator's spmv (bit-exact across threads) ⇒ thread-count-independent solve.
// -----------------------------------------------------------------------

// Recycle-space truncation strategy. 'Smallest' (scipy default) keeps the
// SVD-optimal k−1 directions of D = (R[:-1]ᵀ)⁻¹Bᵀ; 'Oldest' drops the oldest pair.
enum class GcrotTruncate
{
    Oldest,
    Smallest
};

namespace detail
{
// (FallbackArena is the shared per-iteration bump-arena from cg.hpp detail.)

// Build D (ncu×kcols, row-major dout[i*kcols+j]) = ((Rsq)ᵀ)⁻¹·Bᵀ transposed, where
// Rsq is the top kcols×kcols upper-triangular block of `rmat` (m-strided row-major)
// and B is ncu×kcols (`bmat`, m-strided). `xscratch` holds X (kcols×ncu, m... ncu-strided)
// = (Rsqᵀ)⁻¹Bᵀ via forward substitution (Rsqᵀ is lower-triangular). Standalone +
// unit-tested for the off-by-one in the R[:-1] / B-transpose indexing.
template <typename T>
inline void gcrot_build_d(const T* rmat, const T* bmat, crd::usize m, crd::usize ncu, crd::usize kcols, T* xscratch,
                          T* dout) noexcept
{
    for (crd::usize i = 0; i < ncu; ++i)
    {
        for (crd::usize rr = 0; rr < kcols; ++rr)
        {
            T acc = bmat[i * m + rr]; // Bᵀ[rr][i] = B[i][rr]
            for (crd::usize c2 = 0; c2 < rr; ++c2)
            {
                acc = acc - rmat[c2 * m + rr] * xscratch[c2 * ncu + i]; // Rsqᵀ[rr][c2] = Rsq[c2][rr]
            }
            xscratch[rr * ncu + i] = acc / rmat[rr * m + rr];
        }
    }
    for (crd::usize i = 0; i < ncu; ++i)
    {
        for (crd::usize j = 0; j < kcols; ++j)
        {
            dout[i * kcols + j] = xscratch[j * ncu + i]; // D = Xᵀ
        }
    }
}

// Dominant left singular vectors of D (ncu×kcols, row-major d[i*kcols+j]): the
// `keep` LARGEST-singular-value left vectors == the `keep` eigenvectors of the
// Hermitian Gram G = D Dᴴ (ncu×ncu) with the largest eigenvalues. Reuses the
// deterministic dense eig_sym (real) / eig_herm (complex) — the dense `svd` is
// real-only, but the Gram route is complex-capable and reuses our eig. Writes
// W (ncu×keep, row-major wout[i*keep+kc]).
template <typename T>
inline void gcrot_dominant_left_vectors(const T* d, crd::usize ncu, crd::usize kcols, crd::usize keep, T* wout,
                                        crd::memory::IAllocator* alloc)
{
    auto gentry = [&](crd::usize aa, crd::usize bb) -> T {
        T s{};
        for (crd::usize k = 0; k < kcols; ++k)
        {
            s = s + d[aa * kcols + k] * detail::krylov_conj<T>(d[bb * kcols + k]); // D Dᴴ
        }
        return s;
    };
    crd::hesap::dense::EigSym<T> e(alloc);
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        crd::hesap::dense::Hermitian<T> g(alloc, ncu);
        for (crd::usize i = 0; i < ncu; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                g.at_lower(i, j) = (i == j) ? T(gentry(i, i).re) : gentry(i, j); // exact-Hermitian diagonal
            }
        }
        e = crd::hesap::dense::eig_herm<T>(alloc, g);
    }
    else
    {
        crd::hesap::dense::Symmetric<T> g(alloc, ncu);
        for (crd::usize i = 0; i < ncu; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                g.at(i, j) = gentry(i, j);
            }
        }
        e = crd::hesap::dense::eig_sym<T>(alloc, g);
    }
    // EigSym values are ASCENDING ⇒ the `keep` largest are the last columns.
    for (crd::usize kc = 0; kc < keep; ++kc)
    {
        for (crd::usize i = 0; i < ncu; ++i)
        {
            wout[i * keep + kc] = e.vectors.at(i, ncu - keep + kc);
        }
    }
}
} // namespace detail

// -----------------------------------------------------------------------
// RecycleSpace<T> -- a PERSISTENT recycle space (the (c = A·u) pairs) carried
// ACROSS a sequence of solves (the de Sturler payoff). Phase 3.1.6 v4e-3a.
//
// OPAQUE + PERSISTENT contract: build it once with the max dimension k, pass it to
// `gcrot_recycled` for each solve in a sequence; the library fills + truncates it.
// `clear()` resets it (start a fresh sequence); `dimension()` reports the current
// pair count. CRITICAL: the stored U is matrix-free (just vectors in ℝⁿ), but
// c = A·u is operator-specific -- so `gcrot_recycled` REBUILDS C = A·U +
// re-orthonormalizes on entry whenever the space is non-empty, making it safe to
// reuse the SAME RecycleSpace across DIFFERENT operators A_i (parametric /
// time-stepping sequences). The rebuild costs k spmvs, amortized across the solve.
// -----------------------------------------------------------------------
template <typename T>
struct RecycleSpace
{
    crd::usize                   n;
    crd::usize                   kdim;    // capacity
    crd::usize                   ncu = 0; // current pair count (persists across solves)
    crd::hesap::dense::Vector<T> ccol;    // (kdim+1)·n  C (c = A·u, orthonormal)
    crd::hesap::dense::Vector<T> ucol;    // (kdim+1)·n  U

    RecycleSpace(crd::memory::IAllocator* alloc, crd::usize size, crd::usize recycle)
        : n(size), kdim(recycle), ccol(alloc, (recycle + 1) * size), ucol(alloc, (recycle + 1) * size)
    {
        CRD_ASSERT_MSG(recycle >= 1, "RecycleSpace: recycle dimension must be >= 1");
    }

    void                                   clear() noexcept { ncu = 0; }
    [[nodiscard]] crd::usize               dimension() const noexcept { return ncu; }
    [[nodiscard]] crd::containers::Span<T> c(crd::usize j) noexcept { return {ccol.data() + j * n, n}; }
    [[nodiscard]] crd::containers::Span<T> u(crd::usize j) noexcept { return {ucol.data() + j * n, n}; }
};

template <typename T>
struct GcrotWorkspace
{
    using R = crd::hesap::dense::RealType<T>;

    crd::usize    n;
    crd::usize    m;    // inner Krylov (Arnoldi) dimension
    crd::usize    kdim; // max recycle pairs kept
    GcrotTruncate truncate = GcrotTruncate::Smallest; // recycle-truncation strategy (SVD-optimal default)

    RecycleSpace<T>              own;  // single-solve recycle space (reset each plain `gcrot` call)
    crd::hesap::dense::Vector<T> vbuf; // (m+1)·n    -- inner Arnoldi basis V
    crd::hesap::dense::Vector<T> zbuf; // m·n        -- inner preconditioned Z
    crd::hesap::dense::Vector<T> w, r, ux, cx; // n each
    crd::containers::Array<T>    hbar; // (m+1)·m raw Hessenberg (col-major: hbar[j*(m+1)+i])
    crd::containers::Array<T>    bmat; // kdim·m projection coefficients B[i*m+j]
    crd::containers::Array<T>    rmat; // m·m Givens-triangular R (row-major)
    crd::containers::Array<T>    hcol; // m+1 working column
    crd::containers::Array<R>    cs;   // m Givens cosines
    crd::containers::Array<T>    sn;   // m Givens sines
    crd::containers::Array<T>    g;    // m+1 rotated RHS
    crd::containers::Array<T>    yv;   // m least-squares solution
    crd::containers::Array<T>    byv;  // kdim  (B·y)
    crd::containers::Array<T>    hyv;  // m+1   (H̄·y)
    // 'smallest'-truncation scratch (all small: ≤ kdim·m):
    crd::containers::Array<T>    dmat; // kdim·m  D (ncu×kcols, row-major)
    crd::containers::Array<T>    xmat; // kdim·m  X (kcols×ncu, row-major) = (R[:-1]ᵀ)⁻¹Bᵀ
    crd::containers::Array<T>    wmat; // kdim·kdim  W dominant left singular vectors
    crd::hesap::dense::Vector<T> ncbuf, nubuf; // kdim·n  new C/U during truncation
    // Per-cycle scratch arena for the truncation's small dense eig (eig_sym/eig_herm):
    // sized generously for a kdim×kdim eigendecomposition; reset every truncation so
    // there is NO per-cycle malloc/free (FallbackArena handles any over-size).
    crd::memory::LinearAllocator trunc_arena;

    GcrotWorkspace(crd::memory::IAllocator* alloc, crd::usize size, crd::usize inner, crd::usize recycle)
        : n(size), m(inner), kdim(recycle), own(alloc, size, recycle),
          vbuf(alloc, (inner + 1) * size), zbuf(alloc, inner * size), w(alloc, size), r(alloc, size), ux(alloc, size),
          cx(alloc, size), hbar(alloc), bmat(alloc), rmat(alloc), hcol(alloc), cs(alloc), sn(alloc), g(alloc),
          yv(alloc), byv(alloc), hyv(alloc), dmat(alloc), xmat(alloc), wmat(alloc), ncbuf(alloc, recycle * size),
          nubuf(alloc, recycle * size),
          trunc_arena(static_cast<crd::usize>(32) * (recycle + 1) * (recycle + 1) * sizeof(T) + 16384, alloc)
    {
        CRD_ASSERT_MSG(inner >= 1, "GcrotWorkspace: inner dimension must be >= 1");
        CRD_ASSERT_MSG(recycle >= 1, "GcrotWorkspace: recycle dimension must be >= 1");
        hbar.resize((inner + 1) * inner);
        bmat.resize(recycle * inner);
        rmat.resize(inner * inner);
        hcol.resize(inner + 1);
        cs.resize(inner);
        sn.resize(inner);
        g.resize(inner + 1);
        yv.resize(inner);
        byv.resize(recycle);
        hyv.resize(inner + 1);
        dmat.resize(recycle * inner);
        xmat.resize(recycle * inner);
        wmat.resize(recycle * recycle);
    }

    [[nodiscard]] crd::containers::Span<T> nc(crd::usize j) noexcept { return {ncbuf.data() + j * n, n}; }
    [[nodiscard]] crd::containers::Span<T> nu(crd::usize j) noexcept { return {nubuf.data() + j * n, n}; }

    [[nodiscard]] crd::usize size() const noexcept { return n; }
    [[nodiscard]] crd::containers::Span<T> v(crd::usize j) noexcept { return {vbuf.data() + j * n, n}; }
    [[nodiscard]] crd::containers::Span<T> z(crd::usize j) noexcept { return {zbuf.data() + j * n, n}; }
};

namespace detail
{
// Core recycling solve over a (persistent) RecycleSpace `rs`. `rebuild_c`: recompute
// C = A·U + re-orthonormalize on entry (required when reusing rs across operators).
// `SymLanczos`: false ⇒ full Arnoldi MGS (GCROT, general A); true ⇒ 3-term symmetric
// Lanczos (RMINRES, symmetric/Hermitian A — the inner basis is kept ⊥ C, which for
// symmetric A preserves the 3-term recurrence ⟨vᵢ,Avⱼ⟩=0, i<j−1; α=Re⟨vⱼ,Avⱼ⟩ real,
// symmetric real tridiagonal even for complex Hermitian A).
template <typename T, bool SymLanczos = false>
IterativeResult<crd::hesap::dense::RealType<T>> gcrot_core(const crd::hesap::LinearOp<T>&  a,
                                                           const crd::hesap::LinearOp<T>*  m_inv,
                                                           crd::containers::ConstSpan<T>   b,
                                                           crd::containers::Span<T>        x,
                                                           const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                           GcrotWorkspace<T>&              ws,
                                                           RecycleSpace<T>&                rs,
                                                           bool                            rebuild_c,
                                                           crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const crd::usize   m      = ws.m;
    const crd::usize   kdim   = ws.kdim;
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "gcrot: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "gcrot: span/workspace size mismatch");
    CRD_ASSERT_MSG(rs.n == n && rs.kdim == kdim, "gcrot: recycle-space / workspace size mismatch");

    const auto r  = ws.r.span();
    const auto wv = ws.w.span();
    const auto ux = ws.ux.span();
    const auto cx = ws.cx.span();
    T*         hbar = ws.hbar.data();

    // ---- rebuild C = A·U + re-orthonormalize (A may have changed since rs was filled) ----
    if (rebuild_c && rs.ncu > 0)
    {
        crd::usize keep = 0;
        for (crd::usize i = 0; i < rs.ncu; ++i)
        {
            dense::copy<T>(crd::containers::ConstSpan<T>{rs.u(i).data(), n}, ux); // uwork = u_i (i >= keep ⇒ intact)
            (void)a.apply(crd::containers::ConstSpan<T>{ux.data(), n}, cx);       // cwork = A·u_i
            for (crd::usize j = 0; j < keep; ++j)
            {
                const T beta = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{rs.c(j).data(), n},
                                                       crd::containers::ConstSpan<T>{cx.data(), n});
                dense::axpy<T>(-beta, crd::containers::ConstSpan<T>{rs.c(j).data(), n}, cx);
                dense::axpy<T>(-beta, crd::containers::ConstSpan<T>{rs.u(j).data(), n}, ux);
            }
            const R nrm = nrm2<T>(cx);
            if (nrm >= smlnum)
            {
                const T inv = T(R(1) / nrm);
                dense::scal<T>(inv, cx);
                dense::scal<T>(inv, ux);
                dense::copy<T>(crd::containers::ConstSpan<T>{cx.data(), n}, rs.c(keep));
                dense::copy<T>(crd::containers::ConstSpan<T>{ux.data(), n}, rs.u(keep));
                ++keep;
            }
        }
        rs.ncu = keep;
    }
    T*         bmat = ws.bmat.data();

    // r = b - A·x
    (void)a.apply(x, wv);
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = b[i] - wv[i];
    }
    const R res0 = nrm2<T>(r);
    R       res  = res0;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(res0);
    }
    if (is_converged<R>(res, res0, opts) || n == 0)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }

    while (result.iterations < opts.max_iter)
    {
        // ---- project the residual onto the recycle space: r -= C(Cᴴr), x += U(Cᴴr) ----
        for (crd::usize i = 0; i < rs.ncu; ++i)
        {
            const T yc = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{rs.c(i).data(), n},
                                                 crd::containers::ConstSpan<T>{r.data(), n});
            dense::axpy<T>(yc, crd::containers::ConstSpan<T>{rs.u(i).data(), n}, x);
            dense::axpy<T>(-yc, crd::containers::ConstSpan<T>{rs.c(i).data(), n}, r);
        }
        res = nrm2<T>(r);
        if (is_converged<R>(res, res0, opts))
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }

        // ---- inner FGMRES(m) Arnoldi, kept orthogonal to C ----
        const R beta = nrm2<T>(r);
        {
            const auto v0  = ws.v(0);
            const T    inv = T(R(1) / beta);
            for (crd::usize i = 0; i < n; ++i)
            {
                v0[i] = r[i] * inv;
            }
        }
        for (crd::usize i = 0; i <= m; ++i)
        {
            ws.g[i] = T{};
        }
        ws.g[0] = T(beta);
        for (crd::usize i = 0; i < (m + 1) * m; ++i)
        {
            hbar[i] = T{}; // zero the tridiagonal/Hessenberg so cx = V·H̄·y reads no stale entries
        }

        crd::usize     kcols     = 0;
        R              lanc_beta = R(0); // β_j carried across the symmetric-Lanczos 3-term recurrence
        const R        inner_tol = opts.rel_tol * res0;
        for (crd::usize j = 0; j < m && result.iterations < opts.max_iter; ++j)
        {
            // z_j = N⁻¹ v_j (right precond); w = A z_j.
            const auto zj = ws.z(j);
            if (prec)
            {
                (void)m_inv->apply(crd::containers::ConstSpan<T>{ws.v(j).data(), n}, zj);
            }
            else
            {
                dense::copy<T>(crd::containers::ConstSpan<T>{ws.v(j).data(), n}, zj);
            }
            (void)a.apply(crd::containers::ConstSpan<T>{zj.data(), n}, wv);

            // GCROT projection: orthogonalize w against C, recording B[i,j] = ⟨c_i, w⟩.
            for (crd::usize i = 0; i < rs.ncu; ++i)
            {
                const T alpha = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{rs.c(i).data(), n},
                                                        crd::containers::ConstSpan<T>{wv.data(), n});
                bmat[i * m + j] = alpha;
                dense::axpy<T>(-alpha, crd::containers::ConstSpan<T>{rs.c(i).data(), n}, wv);
            }

            R hn;
            if constexpr (SymLanczos)
            {
                // 3-term symmetric Lanczos: w = A v_j − α_j v_j − β_j v_{j-1} (v ⊥ C kept
                // by the projection above). Symmetric real tridiagonal (α real, β = sub = super).
                for (crd::usize i = 0; i < j; ++i)
                {
                    ws.hcol[i] = T{}; // column is tridiagonal: entries above j−1 are zero
                }
                const R alpha = detail::krylov_real<T>(
                    detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.v(j).data(), n},
                                            crd::containers::ConstSpan<T>{wv.data(), n}));
                dense::axpy<T>(-T(alpha), crd::containers::ConstSpan<T>{ws.v(j).data(), n}, wv);
                if (j > 0)
                {
                    dense::axpy<T>(-T(lanc_beta), crd::containers::ConstSpan<T>{ws.v(j - 1).data(), n}, wv);
                    hbar[j * (m + 1) + (j - 1)] = T(lanc_beta); // β_j (super-diagonal)
                    ws.hcol[j - 1]             = T(lanc_beta);
                }
                hbar[j * (m + 1) + j] = T(alpha);
                ws.hcol[j]            = T(alpha);
                hn                    = nrm2<T>(wv);
                lanc_beta             = hn; // β_{j+1} for the next step's super-diagonal
            }
            else
            {
                // Full modified Gram-Schmidt against v_0..v_j (general A; Hessenberg column).
                for (crd::usize i = 0; i <= j; ++i)
                {
                    const T h = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.v(i).data(), n},
                                                        crd::containers::ConstSpan<T>{wv.data(), n});
                    hbar[j * (m + 1) + i] = h;
                    ws.hcol[i]            = h;
                    dense::axpy<T>(-h, crd::containers::ConstSpan<T>{ws.v(i).data(), n}, wv);
                }
                hn = nrm2<T>(wv);
            }
            hbar[j * (m + 1) + (j + 1)] = T(hn);
            ws.hcol[j + 1]              = T(hn);

            const bool lucky = !(hn > smlnum);
            if (!lucky)
            {
                const auto vn  = ws.v(j + 1);
                const T    inv = T(R(1) / hn);
                for (crd::usize i = 0; i < n; ++i)
                {
                    vn[i] = wv[i] * inv;
                }
            }

            // Apply previous Givens to the new column; new Givens zeroing hcol[j+1].
            for (crd::usize i = 0; i + 1 <= j; ++i)
            {
                detail::gmres_rot_apply<T>(ws.hcol[i], ws.hcol[i + 1], ws.cs[i], ws.sn[i]);
            }
            const auto rot = detail::gmres_givens<T>(ws.hcol[j], ws.hcol[j + 1]);
            ws.cs[j]       = rot.c;
            ws.sn[j]       = rot.s;
            ws.hcol[j]     = rot.r;
            for (crd::usize i = 0; i <= j; ++i)
            {
                ws.rmat[i * m + j] = ws.hcol[i];
            }
            const T gj  = ws.g[j];
            ws.g[j]     = ws.cs[j] * gj;
            ws.g[j + 1] = detail::gmres_neg_conj<T>(ws.sn[j]) * gj;

            ++result.iterations;
            ++kcols;
            const R inner_res = detail::krylov_mag<T>(ws.g[j + 1]);
            if (inner_res <= inner_tol || lucky)
            {
                break;
            }
        }

        // ---- least-squares y: back-substitute R y = g (kcols × kcols) ----
        for (crd::usize ii = 0; ii < kcols; ++ii)
        {
            const crd::usize i   = kcols - 1 - ii;
            T                acc = ws.g[i];
            for (crd::usize jj = i + 1; jj < kcols; ++jj)
            {
                acc = acc - ws.rmat[i * m + jj] * ws.yv[jj];
            }
            ws.yv[i] = acc / ws.rmat[i * m + i];
        }

        // ---- form the new recycle direction: ux = Z y − U(B y) ; cx = V·H̄·y ----
        for (crd::usize i = 0; i < n; ++i)
        {
            ux[i] = T{};
        }
        for (crd::usize j = 0; j < kcols; ++j)
        {
            dense::axpy<T>(ws.yv[j], crd::containers::ConstSpan<T>{ws.z(j).data(), n}, ux);
        }
        for (crd::usize i = 0; i < rs.ncu; ++i)
        {
            T by = T{};
            for (crd::usize j = 0; j < kcols; ++j)
            {
                by = by + bmat[i * m + j] * ws.yv[j];
            }
            ws.byv[i] = by;
            dense::axpy<T>(-by, crd::containers::ConstSpan<T>{rs.u(i).data(), n}, ux);
        }
        for (crd::usize i = 0; i <= kcols; ++i)
        {
            T hy = T{};
            for (crd::usize j = 0; j < kcols; ++j)
            {
                hy = hy + hbar[j * (m + 1) + i] * ws.yv[j];
            }
            ws.hyv[i] = hy;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            cx[i] = T{};
        }
        for (crd::usize i = 0; i <= kcols; ++i)
        {
            dense::axpy<T>(ws.hyv[i], crd::containers::ConstSpan<T>{ws.v(i).data(), n}, cx);
        }

        // ---- normalize so cx is unit (cx = A·ux preserved), then the GCR step ----
        const R cxn = nrm2<T>(cx);
        if (cxn < smlnum)
        {
            // No usable new direction (inner solve made no progress): stop.
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        const T cinv = T(R(1) / cxn);
        dense::scal<T>(cinv, cx);
        dense::scal<T>(cinv, ux);

        const T gamma = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{cx.data(), n},
                                                crd::containers::ConstSpan<T>{r.data(), n});
        dense::axpy<T>(-gamma, crd::containers::ConstSpan<T>{cx.data(), n}, r);
        dense::axpy<T>(gamma, crd::containers::ConstSpan<T>{ux.data(), n}, x);

        res = nrm2<T>(r);
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        if (is_converged<R>(res, res0, opts))
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }

        // ---- truncate the recycle space to (kdim-1), then append the new pair ----
        const bool full = (rs.ncu >= kdim);
        if (full && ws.truncate == GcrotTruncate::Smallest && kdim >= 2)
        {
            // SVD-optimal: keep the kdim-1 dominant left singular vectors of
            // D = (R[:-1]ᵀ)⁻¹·Bᵀ (scipy 'smallest'). Rsq = ws.rmat top kcols×kcols.
            const crd::usize keep = kdim - 1;
            detail::gcrot_build_d<T>(ws.rmat.data(), bmat, m, rs.ncu, kcols, ws.xmat.data(), ws.dmat.data());
            // Per-cycle scratch arena (no malloc/free): the small dense eig allocates here.
            ws.trunc_arena.reset();
            detail::FallbackArena arena(&ws.trunc_arena, result_alloc);
            detail::gcrot_dominant_left_vectors<T>(ws.dmat.data(), rs.ncu, kcols, keep, ws.wmat.data(), &arena);

            // new c/u = Σ_i W[i,j]·(c_i, u_i), reorthonormalized (MGS) + normalized.
            for (crd::usize j = 0; j < keep; ++j)
            {
                auto ncj = ws.nc(j);
                auto nuj = ws.nu(j);
                for (crd::usize i = 0; i < n; ++i)
                {
                    ncj[i] = T{};
                    nuj[i] = T{};
                }
                for (crd::usize i2 = 0; i2 < rs.ncu; ++i2)
                {
                    const T wij = ws.wmat[i2 * keep + j];
                    dense::axpy<T>(wij, crd::containers::ConstSpan<T>{rs.c(i2).data(), n}, ncj);
                    dense::axpy<T>(wij, crd::containers::ConstSpan<T>{rs.u(i2).data(), n}, nuj);
                }
                for (crd::usize jp = 0; jp < j; ++jp)
                {
                    const T al = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.nc(jp).data(), n},
                                                         crd::containers::ConstSpan<T>{ncj.data(), n});
                    dense::axpy<T>(-al, crd::containers::ConstSpan<T>{ws.nc(jp).data(), n}, ncj);
                    dense::axpy<T>(-al, crd::containers::ConstSpan<T>{ws.nu(jp).data(), n}, nuj);
                }
                const R nn = nrm2<T>(ncj);
                if (nn >= smlnum)
                {
                    const T inv = T(R(1) / nn);
                    dense::scal<T>(inv, ncj);
                    dense::scal<T>(inv, nuj);
                }
            }
            for (crd::usize j = 0; j < keep; ++j)
            {
                dense::copy<T>(crd::containers::ConstSpan<T>{ws.nc(j).data(), n}, rs.c(j));
                dense::copy<T>(crd::containers::ConstSpan<T>{ws.nu(j).data(), n}, rs.u(j));
            }
            rs.ncu = keep;
        }
        else if (full)
        {
            // 'oldest': drop the oldest pair(s) until there is room for the new one.
            while (rs.ncu >= kdim && rs.ncu > 0)
            {
                for (crd::usize i = 1; i < rs.ncu; ++i)
                {
                    dense::copy<T>(crd::containers::ConstSpan<T>{rs.c(i).data(), n}, rs.c(i - 1));
                    dense::copy<T>(crd::containers::ConstSpan<T>{rs.u(i).data(), n}, rs.u(i - 1));
                }
                --rs.ncu;
            }
        }
        dense::copy<T>(crd::containers::ConstSpan<T>{cx.data(), n}, rs.c(rs.ncu));
        dense::copy<T>(crd::containers::ConstSpan<T>{ux.data(), n}, rs.u(rs.ncu));
        ++rs.ncu;
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}
} // namespace detail

// ---- single-solve GCROT(m,k) (fresh recycle space each call) ----
// With optional RIGHT preconditioner N = m_inv (nullptr ⇒ plain).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gcrot(const crd::hesap::LinearOp<T>&  a,
                                                      const crd::hesap::LinearOp<T>*  m_inv,
                                                      crd::containers::ConstSpan<T>   b,
                                                      crd::containers::Span<T>        x,
                                                      const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                      GcrotWorkspace<T>&              ws,
                                                      crd::memory::IAllocator*        result_alloc)
{
    ws.own.clear(); // fresh recycle space; no C-rebuild needed (empty)
    return detail::gcrot_core<T>(a, m_inv, b, x, opts, ws, ws.own, /*rebuild_c=*/false, result_alloc);
}

template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gcrot(const crd::hesap::LinearOp<T>&                          a,
                                                      crd::containers::ConstSpan<T>                          b,
                                                      crd::containers::Span<T>                               x,
                                                      const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                      GcrotWorkspace<T>&                                     ws,
                                                      crd::memory::IAllocator* result_alloc)
{
    return gcrot<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

// ---- cross-solve GCROT(m,k) (PERSISTENT recycle space `rs` reused across a
// sequence — the de Sturler payoff). On entry C = A·U is rebuilt + re-orthonormalized
// from the persisted U (safe across different operators A_i). Pass the SAME `rs` to
// every solve in the sequence; `rs.clear()` starts a new sequence. ----
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gcrot_recycled(const crd::hesap::LinearOp<T>&  a,
                                                               const crd::hesap::LinearOp<T>*  m_inv,
                                                               crd::containers::ConstSpan<T>   b,
                                                               crd::containers::Span<T>        x,
                                                               const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                               GcrotWorkspace<T>&              ws,
                                                               RecycleSpace<T>&                rs,
                                                               crd::memory::IAllocator*        result_alloc)
{
    return detail::gcrot_core<T>(a, m_inv, b, x, opts, ws, rs, /*rebuild_c=*/(rs.ncu > 0), result_alloc);
}

template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gcrot_recycled(const crd::hesap::LinearOp<T>&  a,
                                                               crd::containers::ConstSpan<T>   b,
                                                               crd::containers::Span<T>        x,
                                                               const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                               GcrotWorkspace<T>&              ws,
                                                               RecycleSpace<T>&                rs,
                                                               crd::memory::IAllocator*        result_alloc)
{
    return gcrot_recycled<T>(a, nullptr, b, x, opts, ws, rs, result_alloc);
}

} // namespace crd::hesap::iterative
