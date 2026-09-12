#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/block_cg.hpp> // detail::block_gram / block_gemm_update / block_qr
#include <crd/hesap/iterative/cg.hpp>       // detail::krylov_mag
#include <crd/hesap/iterative/gmres.hpp>    // detail::gmres_givens / gmres_rot_apply / GivensRot
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/sparse/block_linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// Block-GMRES(m) -- restarted block Generalized Minimal RESidual. Phase 3.1.6 v4f-3.
//
// Solves A·X = B for s right-hand sides at once (X, B are n×s ROW-MAJOR), A GENERAL
// (nonsymmetric / non-Hermitian) -- the regime block-CG cannot touch. The s columns
// share ONE block Krylov space K_m(A, R₀) = span{R₀, A·R₀, …, A^{m-1}·R₀}: one block
// spmm per step for all s RHS (vs s spmvs), and each column's residual is minimized
// over the SAME basis (often fewer iters/column than independent GMRES). Eigen ships
// no block-GMRES → breadth + the A-pass-reuse / shared-Krylov win over per-column.
//
// FLEXIBLE (Saad 1993): the preconditioner z_j = M_j⁻¹ v_j may change every step; the
// update is built from the preconditioned blocks Z (m_inv == nullptr ⇒ plain block-GMRES).
//
// Block Arnoldi: V₀,ρ = QR(R₀); for j: W = A·Z_j, block-MGS against V₀..V_j (s×s
// H_ij = V_iᴴW via block_gram, W -= V_i·H_ij), V_{j+1},H_{j+1,j} = QR(W). The block
// upper-Hessenberg H̄ has H_{j+1,j} UPPER-TRIANGULAR (the QR R factor), so column
// c = j·s+cc has subdiagonal nonzeros exactly in rows c+1..c+s (an s-wide band) ⇒ it
// triangularizes with SCALAR Givens (the verbatim gmres_givens / gmres_rot_apply,
// applied to all s columns of the RHS G), zeroing each column's band bottom-up. The
// least-squares residual estimate per column l is ‖G[used.., l]‖ (cheap early-stop).
//
// Determinism: block_gram / block_qr (packed-MGS) / Givens all run SERIALLY on the
// calling thread; only the operator's block spmm is parallel (bit-exact across
// threads), so the whole solve is thread-count independent (the v4 determinism moat).
// -----------------------------------------------------------------------

template <typename T>
struct BlockGmresWorkspace
{
    using R = crd::hesap::dense::RealType<T>;

    crd::usize n;
    crd::u32   s;
    crd::usize m; // block restart length (# block-Arnoldi steps / cycle)

    crd::hesap::dense::Vector<T> vbuf; // (m+1)·n·s  block Arnoldi basis
    crd::hesap::dense::Vector<T> zbuf; // m·n·s      preconditioned blocks (flexible)
    crd::hesap::dense::Vector<T> wblk; // n·s        Arnoldi work block
    crd::hesap::dense::Vector<T> rblk; // n·s        residual block
    crd::hesap::dense::Vector<T> cm;   // n·s        block_qr transpose scratch
    crd::containers::Array<T>    hblk; // s·s        one block-MGS coefficient
    crd::containers::Array<T>    rho;  // s·s        QR R factor
    crd::containers::Array<T>    hmat; // (m+1)s × ms  block Hessenberg (row-major, ld = ms)
    crd::containers::Array<T>    gmat; // (m+1)s × s   rotated RHS (row-major)
    crd::containers::Array<T>    ymat; // ms × s        least-squares solution
    crd::containers::Array<R>    cs;   // m·s·s  Givens cosines
    crd::containers::Array<T>    sn;   // m·s·s  Givens sines
    crd::containers::Array<crd::u32> gp; // m·s·s  Givens top-row index (rotates rows p, p+1)
    crd::containers::Array<R>    bnorm; // s      per-column ‖B·,l‖

    BlockGmresWorkspace(crd::memory::IAllocator* alloc, crd::usize size, crd::u32 nrhs, crd::usize restart)
        : n(size), s(nrhs), m(restart)
        , vbuf(alloc, (restart + 1) * size * nrhs), zbuf(alloc, restart * size * nrhs), wblk(alloc, size * nrhs)
        , rblk(alloc, size * nrhs), cm(alloc, size * nrhs), hblk(alloc), rho(alloc), hmat(alloc), gmat(alloc)
        , ymat(alloc), cs(alloc), sn(alloc), gp(alloc), bnorm(alloc)
    {
        CRD_ASSERT_MSG(nrhs >= 1, "BlockGmresWorkspace: nrhs must be >= 1");
        CRD_ASSERT_MSG(restart >= 1, "BlockGmresWorkspace: restart must be >= 1");
        const crd::usize ss   = static_cast<crd::usize>(nrhs) * nrhs;
        const crd::usize rows = (restart + 1) * nrhs;
        const crd::usize cols = restart * nrhs;
        hblk.resize(ss);
        rho.resize(ss);
        hmat.resize(rows * cols);
        gmat.resize(rows * nrhs);
        ymat.resize(cols * nrhs);
        cs.resize(restart * ss);
        sn.resize(restart * ss);
        gp.resize(restart * ss);
        bnorm.resize(nrhs);
    }

    [[nodiscard]] crd::containers::Span<T> v(crd::usize j) noexcept
    {
        return crd::containers::Span<T>{vbuf.data() + j * n * s, n * s};
    }
    [[nodiscard]] crd::containers::Span<T> z(crd::usize j) noexcept
    {
        return crd::containers::Span<T>{zbuf.data() + j * n * s, n * s};
    }
};

namespace detail
{
// ‖column l of the (rows × s) row-major block, rows [r0, r1)‖ (Euclidean).
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> block_col_tail_norm(const T* g, crd::u32 s, crd::usize r0,
                                                                       crd::usize r1, crd::u32 l) noexcept
{
    using R   = crd::hesap::dense::RealType<T>;
    R     acc = R(0);
    for (crd::usize r = r0; r < r1; ++r)
    {
        const R mg = detail::krylov_mag<T>(g[r * s + l]);
        acc += mg * mg;
    }
    return std::sqrt(acc);
}
} // namespace detail

template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_fgmres(
    const crd::hesap::sparse::BlockLinearOp<T>&             a,
    const crd::hesap::sparse::BlockLinearOp<T>*             m_inv,
    crd::containers::ConstSpan<T>                           b,
    crd::containers::Span<T>                                x,
    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
    BlockGmresWorkspace<T>&                                 ws,
    crd::memory::IAllocator*                                result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const crd::usize   n  = a.n_rows();
    const crd::u32     s  = ws.s;
    const crd::usize   m  = ws.m;
    const crd::usize   ld = m * s; // Hessenberg leading dim (cols)
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "block_gmres: operator must be square");
    CRD_ASSERT_MSG(b.size() == n * s && x.size() == n * s, "block_gmres: B/X must be n×s row-major");

    T* W   = ws.wblk.data();
    T* Rb  = ws.rblk.data();
    T* H   = ws.hmat.data();
    T* G   = ws.gmat.data();

    // Per-column ‖B·,l‖ for the relative-residual test.
    for (crd::u32 l = 0; l < s; ++l)
    {
        R acc = R(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            const R mg = detail::krylov_mag<T>(b[k * s + l]);
            acc += mg * mg;
        }
        ws.bnorm[l] = std::sqrt(acc) + detail::krylov_smlnum<R>();
    }
    auto all_converged = [&](const T* g, crd::usize used) -> bool {
        for (crd::u32 l = 0; l < s; ++l)
        {
            if (detail::block_col_tail_norm<T>(g, s, used, (m + 1) * s, l) > opts.rel_tol * ws.bnorm[l])
            {
                return false;
            }
        }
        return true;
    };

    while (result.iterations < opts.max_iter)
    {
        // R₀ = B - A·X ; V₀,ρ = QR(R₀).
        (void)a.apply_block(x, s, crd::containers::Span<T>{W, n * s}, s, s);
        for (crd::usize i = 0; i < n * s; ++i) { Rb[i] = b[i] - W[i]; }

        // Cycle-start convergence (true residual): tail of an all-in-top-block G.
        {
            bool conv = true;
            for (crd::u32 l = 0; l < s; ++l)
            {
                R acc = R(0);
                for (crd::usize k = 0; k < n; ++k) { const R mg = detail::krylov_mag<T>(Rb[k * s + l]); acc += mg * mg; }
                if (std::sqrt(acc) > opts.rel_tol * ws.bnorm[l]) { conv = false; break; }
            }
            if (conv)
            {
                result.converged           = true;
                result.reason              = StopReason::Converged;
                result.final_residual_norm = R(0);
                return result;
            }
        }

        detail::block_qr<T>(Rb, n, s, ws.cm.data(), ws.rho.data()); // Rb → V₀ (orthonormal), rho = initial RHS
        dense::copy<T>(crd::containers::ConstSpan<T>{Rb, n * s}, ws.v(0));
        // G = [ρ; 0]  ((m+1)s × s).
        for (crd::usize i = 0; i < (m + 1) * static_cast<crd::usize>(s) * s; ++i) { G[i] = T{}; }
        for (crd::u32 row = 0; row < s; ++row)
        {
            for (crd::u32 col = 0; col < s; ++col) { G[static_cast<crd::usize>(row) * s + col] = ws.rho[static_cast<crd::usize>(row) * s + col]; }
        }
        for (crd::usize i = 0; i < (m + 1) * static_cast<crd::usize>(s) * ld; ++i) { H[i] = T{}; }

        crd::u32   ngiv = 0; // Givens generated this cycle
        crd::usize kdone = 0; // block-Arnoldi steps completed
        for (crd::usize j = 0; j < m && result.iterations < opts.max_iter; ++j)
        {
            // Z_j = M⁻¹ V_j (flexible) ; W = A·Z_j.
            const auto zj = ws.z(j);
            if (m_inv != nullptr)
            {
                (void)m_inv->apply_block(ws.v(j), s, zj, s, s);
            }
            else
            {
                dense::copy<T>(ws.v(j), zj);
            }
            (void)a.apply_block(crd::containers::ConstSpan<T>{zj.data(), n * s}, s, crd::containers::Span<T>{W, n * s}, s, s);

            // Block-MGS against V₀..V_j ; store H_ij blocks.
            for (crd::usize i = 0; i <= j; ++i)
            {
                detail::block_gram<T>(ws.v(i).data(), W, n, s, ws.hblk.data());          // H_ij = V_iᴴ W
                detail::block_gemm_update<T>(ws.v(i).data(), ws.hblk.data(), n, s, W, -1); // W -= V_i H_ij
                for (crd::u32 aa = 0; aa < s; ++aa)
                {
                    for (crd::u32 bb = 0; bb < s; ++bb)
                    {
                        H[(i * s + aa) * ld + (j * s + bb)] = ws.hblk[static_cast<crd::usize>(aa) * s + bb];
                    }
                }
            }
            // V_{j+1}, H_{j+1,j} = QR(W).
            detail::block_qr<T>(W, n, s, ws.cm.data(), ws.rho.data());
            dense::copy<T>(crd::containers::ConstSpan<T>{W, n * s}, ws.v(j + 1));
            for (crd::u32 aa = 0; aa < s; ++aa)
            {
                for (crd::u32 bb = 0; bb < s; ++bb)
                {
                    H[((j + 1) * s + aa) * ld + (j * s + bb)] = ws.rho[static_cast<crd::usize>(aa) * s + bb];
                }
            }

            // Triangularize the s new scalar columns via banded scalar Givens.
            for (crd::u32 cc = 0; cc < s; ++cc)
            {
                const crd::usize c = j * s + cc;
                // Apply all previous Givens to column c.
                for (crd::u32 g = 0; g < ngiv; ++g)
                {
                    const crd::u32 p = ws.gp[g];
                    detail::gmres_rot_apply<T>(H[static_cast<crd::usize>(p) * ld + c], H[static_cast<crd::usize>(p + 1) * ld + c], ws.cs[g], ws.sn[g]);
                }
                // Zero rows c+1..c+s bottom-up with s new Givens; apply each to G (all s columns).
                for (crd::u32 t = s; t >= 1; --t)
                {
                    const crd::u32   p   = static_cast<crd::u32>(c) + t - 1;
                    const auto       rot = detail::gmres_givens<T>(H[static_cast<crd::usize>(p) * ld + c], H[static_cast<crd::usize>(p + 1) * ld + c]);
                    ws.cs[ngiv] = rot.c;
                    ws.sn[ngiv] = rot.s;
                    ws.gp[ngiv] = p;
                    ++ngiv;
                    H[static_cast<crd::usize>(p) * ld + c]       = rot.r;
                    H[static_cast<crd::usize>(p + 1) * ld + c]   = T{};
                    for (crd::u32 l = 0; l < s; ++l)
                    {
                        detail::gmres_rot_apply<T>(G[static_cast<crd::usize>(p) * s + l], G[static_cast<crd::usize>(p + 1) * s + l], rot.c, rot.s);
                    }
                }
            }
            ++result.iterations;
            kdone = j + 1;
            const crd::usize used = kdone * s;
            if (opts.record_residuals)
            {
                R worst = R(0);
                for (crd::u32 l = 0; l < s; ++l) { worst = std::max(worst, detail::block_col_tail_norm<T>(G, s, used, (m + 1) * s, l)); }
                result.residual_history.push_back(worst);
            }
            if (all_converged(G, used)) { break; }
        }

        // Back-substitute R·Y = G[0:used,:] (upper-triangular, used×used), per RHS column.
        const crd::usize used   = kdone * s;
        const R          smlnum = detail::krylov_smlnum<R>();
        T*               Y      = ws.ymat.data();
        for (crd::u32 l = 0; l < s; ++l)
        {
            for (crd::usize cc = 0; cc < used; ++cc)
            {
                const crd::usize c   = used - 1 - cc;
                T                acc = G[c * s + l];
                for (crd::usize k = c + 1; k < used; ++k)
                {
                    acc = acc - H[c * ld + k] * Y[k * s + l];
                }
                const T diag = H[c * ld + c];
                // A deflated / happy-breakdown column (zero QR diagonal) contributes
                // nothing to the solution — guard the division (else NaN).
                Y[c * s + l] = (detail::krylov_mag<T>(diag) > smlnum) ? acc / diag : T{};
            }
        }
        // X += Σ_j Z_j · Y_block_j   (flexible: preconditioned blocks).
        for (crd::usize j = 0; j < kdone; ++j)
        {
            detail::block_gemm_update<T>(ws.z(j).data(), Y + j * static_cast<crd::usize>(s) * s, n, s, x.data(), +1);
        }

        // True residual for the restart / convergence test.
        (void)a.apply_block(x, s, crd::containers::Span<T>{W, n * s}, s, s);
        bool conv = true;
        R    worst = R(0);
        for (crd::u32 l = 0; l < s; ++l)
        {
            R acc = R(0);
            for (crd::usize k = 0; k < n; ++k) { const R d = detail::krylov_mag<T>(b[k * s + l] - W[k * s + l]); acc += d * d; }
            const R rn = std::sqrt(acc);
            worst      = std::max(worst, rn);
            if (rn > opts.rel_tol * ws.bnorm[l]) { conv = false; }
        }
        result.final_residual_norm = worst;
        if (conv)
        {
            result.converged = true;
            result.reason    = StopReason::Converged;
            return result;
        }
    }

    result.reason = StopReason::MaxIterations;
    return result;
}

// Plain (unpreconditioned) restarted block-GMRES(m).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_gmres(const crd::hesap::sparse::BlockLinearOp<T>& a,
                                                            crd::containers::ConstSpan<T>               b,
                                                            crd::containers::Span<T>                    x,
                                                            const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                            BlockGmresWorkspace<T>&                     ws,
                                                            crd::memory::IAllocator*                    result_alloc)
{
    return block_fgmres<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

// Preconditioned block-GMRES (right/flexible preconditioner via M⁻¹ block apply).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> block_pgmres(const crd::hesap::sparse::BlockLinearOp<T>& a,
                                                             const crd::hesap::sparse::BlockLinearOp<T>& m_inv,
                                                             crd::containers::ConstSpan<T>               b,
                                                             crd::containers::Span<T>                    x,
                                                             const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                             BlockGmresWorkspace<T>&                     ws,
                                                             crd::memory::IAllocator*                    result_alloc)
{
    return block_fgmres<T>(a, &m_inv, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
