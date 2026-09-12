#pragma once

// lobpcg.hpp — Phase 3.1.6 v6-e: LOBPCG (Locally Optimal Block Preconditioned Conjugate Gradient, Knyazev
// 2001) — a BLOCK eigensolver for the symmetric eigenproblem A·x = λ·x, converging the `nev` extreme
// eigenpairs SIMULTANEOUSLY. v6-e-a = the standard (B = I) symmetric core; the SPD-generalized A·x = λ·B·x and
// the preconditioned crush are v6-e-b/c. The preconditioner is an OPTIONAL crd::hesap::LinearOp (T ≈ A⁻¹,
// default identity) — it is the algorithmic-crush lever (fewer matvecs), wired here, exercised at v6-e-b.
//
// Each iteration works in the subspace S = [X, W, P] (n × up-to-3k): X = current Ritz block, W = preconditioned
// residual T·(A·X − X·Θ), P = previous search directions ("locally optimal" ≡ CG). We keep S ORTHONORMAL via
// block modified-Gram-Schmidt (dropping rank-deficient columns), so the Rayleigh-Ritz reduces to a plain dense
// `eig_sym(SᵀAS)` (no generalized Gram solve). The A-images of X (and the new Ritz block) are carried through
// the small q×k combination, so the only new matvecs per iteration are A·W and A·P.
//
// MOAT: SplitMix start + fixed-order block MGS + deterministic dense `eig_sym` + fixed-order combinations; only
// the matvec (a.apply) is parallel + bit-exact ⇒ the eigenpairs are bit-identical across {1,2,4,8} workers.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/eigen/lanczos.hpp> // detail::splitmix_pm1 + the spec
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace crd::hesap::eigen
{

// Compute `opts.nev` extreme eigenpairs of a symmetric (matrix-free) operator `a` via block LOBPCG. `precond`
// (optional, T ≈ A⁻¹) preconditions the residual block; nullptr ⇒ T = I. `which` must be Smallest/LargestAlgebraic.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym_lobpcg(const crd::hesap::LinearOp<T>& a, const EigenOptions<T>& opts,
                                             crd::memory::IAllocator* alloc,
                                             const crd::hesap::LinearOp<T>* precond = nullptr)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "eigs_sym_lobpcg: real symmetric");
    namespace dn = crd::hesap::dense;
    using R = T;

    EigenResult<T> result(alloc);
    const crd::u32 n = static_cast<crd::u32>(a.n_rows());
    result.n = n;
    if (n == 0 || opts.nev == 0)
    {
        return result;
    }
    crd::u32 k = opts.nev;
    if (k > n)
    {
        k = n;
    }
    const bool want_smallest = (opts.which == Which::SmallestAlgebraic || opts.which == Which::SmallestMagnitude ||
                               opts.which == Which::SmallestReal);
    const crd::u32 maxcols = 3 * k; // [X | W | P]

    // Column-major block buffers (each n × width); col(buf, j) = the j-th length-n vector.
    auto mk = [&](crd::u32 width) {
        crd::containers::Array<T> b(alloc);
        b.resize(static_cast<crd::usize>(n) * width);
        return b;
    };
    crd::containers::Array<T> x = mk(k);
    crd::containers::Array<T> ax = mk(k);
    crd::containers::Array<T> p = mk(k);
    crd::containers::Array<T> s = mk(maxcols);
    crd::containers::Array<T> as = mk(maxcols);
    crd::containers::Array<T> xn = mk(k);
    crd::containers::Array<T> axn = mk(k);
    crd::containers::Array<T> pn = mk(k);
    crd::containers::Array<T> rblk = mk(k);
    crd::containers::Array<T> wbuf = mk(k);
    auto col = [&](crd::containers::Array<T>& b, crd::u32 j) noexcept -> T* {
        return b.data() + static_cast<crd::usize>(j) * n;
    };
    auto ccol = [&](const crd::containers::Array<T>& b, crd::u32 j) noexcept -> const T* {
        return b.data() + static_cast<crd::usize>(j) * n;
    };

    const R eps = std::numeric_limits<R>::epsilon();
    const R drop = static_cast<R>(16) * eps; // rank-deficiency threshold for block MGS
    const R tol = opts.effective_tol();

    // Orthonormalize column `j` of buffer `b` against b[0..base-1] AND b[base..j-1] (already orthonormal),
    // twice (for stability). Returns the post-projection norm (caller drops the column if < drop).
    auto ortho_col = [&](crd::containers::Array<T>& b, crd::u32 j, crd::u32 ndone) noexcept -> R {
        T* bj = col(b, j);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::u32 i = 0; i < ndone; ++i)
            {
                const T c = dn::dot<T>({ccol(b, i), n}, {bj, n});
                dn::axpy<T>(-c, {ccol(b, i), n}, {bj, n});
            }
        }
        return dn::nrm2<T>({bj, n});
    };

    // ---- initial block X = deterministic random, orthonormalized ----
    crd::u32 xcols = 0;
    for (crd::u32 j = 0; j < k; ++j)
    {
        T* xj = col(x, j);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xj[i] = detail::splitmix_pm1<R>(opts.seed + static_cast<crd::u64>(j) * 0x100000001B3ULL, i);
        }
        // orthonormalize against the kept X columns [0..xcols-1]; place at slot xcols on success.
        if (xcols != j)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(x, xcols)[i] = xj[i];
            }
        }
        R nr = ortho_col(x, xcols, xcols);
        if (nr > drop)
        {
            dn::scal<T>(static_cast<T>(R{1} / nr), {col(x, xcols), n});
            ++xcols;
        }
    }
    k = xcols; // (n ≫ k ⇒ full rank in practice)
    result.iterations = 0;

    auto block_matvec = [&](const crd::containers::Array<T>& src, crd::u32 jsrc, crd::containers::Array<T>& dst,
                            crd::u32 jdst) {
        (void)a.apply({ccol(src, jsrc), n}, {col(dst, jdst), n});
    };

    // AX = A·X, then one RR on X alone to seed Θ (rotate X, AX into the Ritz basis).
    for (crd::u32 j = 0; j < k; ++j)
    {
        block_matvec(x, j, ax, j);
    }
    crd::containers::Array<R> theta(alloc);
    theta.resize(k);
    {
        dn::Symmetric<T> g(alloc, k);
        for (crd::u32 i = 0; i < k; ++i)
        {
            for (crd::u32 j = 0; j <= i; ++j)
            {
                g.at(i, j) = dn::dot<T>({ccol(x, i), n}, {ccol(ax, j), n});
            }
        }
        dn::EigSym<T> es = dn::eig_sym<T>(alloc, g);
        // Select k wanted (eig_sym ascending): smallest ⇒ first k, largest ⇒ last k.
        for (crd::u32 t = 0; t < k; ++t)
        {
            const crd::u32 src = want_smallest ? t : (k - 1 - t);
            theta[t] = es.values.data()[src];
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(xn, t)[i] = T{0};
                col(axn, t)[i] = T{0};
            }
            for (crd::u32 l = 0; l < k; ++l)
            {
                const T cc = es.vectors.at(l, src);
                dn::axpy<T>(cc, {ccol(x, l), n}, {col(xn, t), n});
                dn::axpy<T>(cc, {ccol(ax, l), n}, {col(axn, t), n});
            }
        }
        std::swap(x, xn);
        std::swap(ax, axn);
    }

    crd::u32 pcols = 0; // P is empty on the first iteration
    crd::u32 nconv = 0;
    for (crd::u32 iter = 0; iter < opts.max_restarts; ++iter)
    {
        result.iterations = iter + 1;
        // ---- residual R = AX − X·Θ + convergence ----
        R maxrel = R{0};
        nconv = 0;
        for (crd::u32 j = 0; j < k; ++j)
        {
            T* rj = col(rblk, j);
            const T* xj = ccol(x, j);
            const T* axj = ccol(ax, j);
            for (crd::u32 i = 0; i < n; ++i)
            {
                rj[i] = axj[i] - static_cast<T>(theta[j]) * xj[i];
            }
            const R rn = dn::nrm2<T>({rj, n});
            const R sc = crd::math::fabs(theta[j]) > R{1} ? crd::math::fabs(theta[j]) : R{1};
            const R rel = rn / sc;
            if (rel <= tol)
            {
                ++nconv;
            }
            maxrel = rel > maxrel ? rel : maxrel;
        }
        if (nconv >= k || iter + 1 >= opts.max_restarts)
        {
            break;
        }

        // ---- build the orthonormal subspace S = [X | W | P] ----
        for (crd::u32 j = 0; j < k; ++j) // X (already orthonormal)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(s, j)[i] = ccol(x, j)[i];
            }
        }
        crd::u32 q = k;
        // W = T·R (preconditioned residual), orthonormalized against S[0..q-1] + within.
        for (crd::u32 j = 0; j < k; ++j)
        {
            if (precond != nullptr)
            {
                (void)precond->apply({ccol(rblk, j), n}, {col(wbuf, j), n});
            }
            else
            {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    col(wbuf, j)[i] = ccol(rblk, j)[i];
                }
            }
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(s, q)[i] = ccol(wbuf, j)[i];
            }
            const R nr = ortho_col(s, q, q);
            if (nr > drop)
            {
                dn::scal<T>(static_cast<T>(R{1} / nr), {col(s, q), n});
                ++q;
            }
        }
        // P (previous search directions), orthonormalized against S[0..q-1] + within.
        for (crd::u32 j = 0; j < pcols; ++j)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(s, q)[i] = ccol(p, j)[i];
            }
            const R nr = ortho_col(s, q, q);
            if (nr > drop)
            {
                dn::scal<T>(static_cast<T>(R{1} / nr), {col(s, q), n});
                ++q;
            }
        }

        // ---- AS for the new columns [k..q) (X's images are carried in AX), then SAS = SᵀAS ----
        for (crd::u32 j = 0; j < k; ++j)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(as, j)[i] = ccol(ax, j)[i];
            }
        }
        for (crd::u32 j = k; j < q; ++j)
        {
            block_matvec(s, j, as, j);
        }
        dn::Symmetric<T> sas(alloc, q);
        for (crd::u32 i = 0; i < q; ++i)
        {
            for (crd::u32 j = 0; j <= i; ++j)
            {
                sas.at(i, j) = dn::dot<T>({ccol(s, i), n}, {ccol(as, j), n});
            }
        }
        dn::EigSym<T> es = dn::eig_sym<T>(alloc, sas);

        // ---- new Ritz block X_new = S·C, AX_new = AS·C, P_new = S[k:q]·C[k:q] ----
        for (crd::u32 t = 0; t < k; ++t)
        {
            const crd::u32 src = want_smallest ? t : (q - 1 - t);
            theta[t] = es.values.data()[src];
            T* xnt = col(xn, t);
            T* axnt = col(axn, t);
            T* pnt = col(pn, t);
            for (crd::u32 i = 0; i < n; ++i)
            {
                xnt[i] = T{0};
                axnt[i] = T{0};
                pnt[i] = T{0};
            }
            for (crd::u32 l = 0; l < q; ++l)
            {
                const T cc = es.vectors.at(l, src);
                dn::axpy<T>(cc, {ccol(s, l), n}, {xnt, n});
                dn::axpy<T>(cc, {ccol(as, l), n}, {axnt, n});
                if (l >= k)
                {
                    dn::axpy<T>(cc, {ccol(s, l), n}, {pnt, n}); // non-X part = the conjugate direction
                }
            }
        }
        std::swap(x, xn);
        std::swap(ax, axn);
        std::swap(p, pn);
        pcols = k;
    }

    // ---- assemble result: values + vectors (sign-pinned: largest-|component| positive) + residuals ----
    result.values.resize(k);
    result.vectors.resize(static_cast<crd::usize>(n) * k);
    result.residuals.resize(k);
    nconv = 0;
    for (crd::u32 j = 0; j < k; ++j)
    {
        result.values[j] = crd::hesap::Complex<R>{theta[j], R{0}};
        const T* xj = ccol(x, j);
        crd::u32 imax = 0;
        R vmax = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R m = crd::math::fabs(static_cast<R>(xj[i]));
            if (m > vmax)
            {
                vmax = m;
                imax = i;
            }
        }
        const T sgn = (xj[imax] < T{0}) ? T{-1} : T{1};
        T* vj = result.vectors.data() + static_cast<crd::usize>(j) * n;
        for (crd::u32 i = 0; i < n; ++i)
        {
            vj[i] = sgn * xj[i];
        }
        const T* axj = ccol(ax, j);
        R rn2 = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R e = static_cast<R>(axj[i]) - theta[j] * static_cast<R>(xj[i]);
            rn2 += e * e;
        }
        const R rn = crd::math::sqrt(rn2);
        result.residuals[j] = rn;
        const R sc = crd::math::fabs(theta[j]) > R{1} ? crd::math::fabs(theta[j]) : R{1};
        if (rn / sc <= tol)
        {
            ++nconv;
        }
    }
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    return result;
}

// v6-e-c — GENERALIZED symmetric eigenproblem A·x = λ·B·x (A symmetric, B SPD), block LOBPCG. The FEM
// modal/buckling K·x = λ·M·x form. Identical to `eigs_sym_lobpcg` except every inner product is the B-inner-
// product ⟨u,v⟩_B = uᵀ·B·v: the subspace S=[X,W,P] is kept B-ORTHONORMAL (SᵀBS=I) by block MGS, so Rayleigh-
// Ritz stays the plain `eig_sym(SᵀAS)`; the residual is R = A·X − B·X·Θ; each column carries BOTH its A-image
// and B-image through the MGS subtractions (so the only fresh matvecs per iter are A·W and B·W). `precond`
// (T ≈ A⁻¹, default identity) preconditions the residual. Same determinism MOAT as the standard path.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym_gen_lobpcg(const crd::hesap::LinearOp<T>& a, const crd::hesap::LinearOp<T>& b,
                                                 const EigenOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                 const crd::hesap::LinearOp<T>* precond = nullptr)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "eigs_sym_gen_lobpcg: real symmetric");
    namespace dn = crd::hesap::dense;
    using R = T;

    EigenResult<T> result(alloc);
    const crd::u32 n = static_cast<crd::u32>(a.n_rows());
    result.n = n;
    if (n == 0 || opts.nev == 0)
    {
        return result;
    }
    crd::u32 k = opts.nev;
    if (k > n)
    {
        k = n;
    }
    const bool want_smallest = (opts.which == Which::SmallestAlgebraic || opts.which == Which::SmallestMagnitude ||
                               opts.which == Which::SmallestReal);
    const crd::u32 maxcols = 3 * k;

    auto mk = [&](crd::u32 width) {
        crd::containers::Array<T> buf(alloc);
        buf.resize(static_cast<crd::usize>(n) * width);
        return buf;
    };
    crd::containers::Array<T> x = mk(k), ax = mk(k), bx = mk(k);
    crd::containers::Array<T> p = mk(k), ap = mk(k), bp = mk(k);
    crd::containers::Array<T> s = mk(maxcols), as = mk(maxcols), bs = mk(maxcols);
    crd::containers::Array<T> xn = mk(k), axn = mk(k), bxn = mk(k);
    crd::containers::Array<T> pn = mk(k), apn = mk(k), bpn = mk(k);
    crd::containers::Array<T> rblk = mk(k), wv = mk(k), aw = mk(k), bw = mk(k);
    auto col = [&](crd::containers::Array<T>& buf, crd::u32 j) noexcept -> T* {
        return buf.data() + static_cast<crd::usize>(j) * n;
    };
    auto ccol = [&](const crd::containers::Array<T>& buf, crd::u32 j) noexcept -> const T* {
        return buf.data() + static_cast<crd::usize>(j) * n;
    };

    const R eps = std::numeric_limits<R>::epsilon();
    const R drop = static_cast<R>(16) * eps;
    const R tol = opts.effective_tol();

    // B-orthonormalize column j of S against S[0..ndone-1] (B-orthonormal), carrying the A- and B-images;
    // returns the post-projection B-norm sqrt(sᵀ·B·s) (caller drops the column if < drop).
    auto ortho_col_b = [&](crd::u32 j, crd::u32 ndone) noexcept -> R {
        T* sj = col(s, j);
        T* asj = col(as, j);
        T* bsj = col(bs, j);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (crd::u32 i = 0; i < ndone; ++i)
            {
                const T c = dn::dot<T>({ccol(s, i), n}, {bsj, n}); // ⟨s_i, s_j⟩_B = s_iᵀ·(B·s_j)
                dn::axpy<T>(-c, {ccol(s, i), n}, {sj, n});
                dn::axpy<T>(-c, {ccol(as, i), n}, {asj, n});
                dn::axpy<T>(-c, {ccol(bs, i), n}, {bsj, n});
            }
        }
        const R q = dn::dot<T>({sj, n}, {bsj, n}); // sᵀ·B·s
        return q > R{0} ? crd::math::sqrt(q) : R{0};
    };

    // ---- initial B-orthonormal block X (deterministic random) ----
    crd::u32 xcols = 0;
    for (crd::u32 j = 0; j < k; ++j)
    {
        T* sj = col(s, xcols);
        for (crd::u32 i = 0; i < n; ++i)
        {
            sj[i] = detail::splitmix_pm1<R>(opts.seed + static_cast<crd::u64>(j) * 0x100000001B3ULL, i);
        }
        (void)b.apply({sj, n}, {col(bs, xcols), n}); // B·x (A-image not needed for the X ortho)
        for (crd::u32 i = 0; i < n; ++i)
        {
            col(as, xcols)[i] = T{0};
        }
        const R bn = ortho_col_b(xcols, xcols);
        if (bn > drop)
        {
            const T inv = static_cast<T>(R{1} / bn);
            dn::scal<T>(inv, {col(s, xcols), n});
            dn::scal<T>(inv, {col(bs, xcols), n});
            ++xcols;
        }
    }
    k = xcols;
    for (crd::u32 j = 0; j < k; ++j) // copy the B-orthonormal start into X/BX, form A·X
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            col(x, j)[i] = ccol(s, j)[i];
            col(bx, j)[i] = ccol(bs, j)[i];
        }
        (void)a.apply({ccol(x, j), n}, {col(ax, j), n});
    }
    result.iterations = 0;

    crd::containers::Array<R> theta(alloc);
    theta.resize(k);
    {
        dn::Symmetric<T> g(alloc, k); // X is B-orthonormal ⇒ the RR is eig_sym(XᵀAX)
        for (crd::u32 i = 0; i < k; ++i)
        {
            for (crd::u32 j = 0; j <= i; ++j)
            {
                g.at(i, j) = dn::dot<T>({ccol(x, i), n}, {ccol(ax, j), n});
            }
        }
        dn::EigSym<T> es = dn::eig_sym<T>(alloc, g);
        for (crd::u32 t = 0; t < k; ++t)
        {
            const crd::u32 src = want_smallest ? t : (k - 1 - t);
            theta[t] = es.values.data()[src];
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(xn, t)[i] = T{0};
                col(axn, t)[i] = T{0};
                col(bxn, t)[i] = T{0};
            }
            for (crd::u32 l = 0; l < k; ++l)
            {
                const T cc = es.vectors.at(l, src);
                dn::axpy<T>(cc, {ccol(x, l), n}, {col(xn, t), n});
                dn::axpy<T>(cc, {ccol(ax, l), n}, {col(axn, t), n});
                dn::axpy<T>(cc, {ccol(bx, l), n}, {col(bxn, t), n});
            }
        }
        std::swap(x, xn);
        std::swap(ax, axn);
        std::swap(bx, bxn);
    }

    crd::u32 pcols = 0;
    crd::u32 nconv = 0;
    for (crd::u32 iter = 0; iter < opts.max_restarts; ++iter)
    {
        result.iterations = iter + 1;
        // residual R = A·X − B·X·Θ; relative generalized residual ‖R‖ / (‖A·x‖ + |θ|·‖B·x‖).
        nconv = 0;
        for (crd::u32 j = 0; j < k; ++j)
        {
            T* rj = col(rblk, j);
            const T* axj = ccol(ax, j);
            const T* bxj = ccol(bx, j);
            for (crd::u32 i = 0; i < n; ++i)
            {
                rj[i] = axj[i] - static_cast<T>(theta[j]) * bxj[i];
            }
            const R rn = dn::nrm2<T>({rj, n});
            const R den = dn::nrm2<T>({axj, n}) + crd::math::fabs(theta[j]) * dn::nrm2<T>({bxj, n});
            if (rn <= tol * (den > R{0} ? den : R{1}))
            {
                ++nconv;
            }
        }
        if (nconv >= k || iter + 1 >= opts.max_restarts)
        {
            break;
        }

        // build the B-orthonormal subspace S = [X | W | P].
        for (crd::u32 j = 0; j < k; ++j) // X (already B-orthonormal): copy with its images
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(s, j)[i] = ccol(x, j)[i];
                col(as, j)[i] = ccol(ax, j)[i];
                col(bs, j)[i] = ccol(bx, j)[i];
            }
        }
        crd::u32 q = k;
        for (crd::u32 j = 0; j < k; ++j) // W = T·R, with fresh A·W and B·W, then B-orthonormalize
        {
            if (precond != nullptr)
            {
                (void)precond->apply({ccol(rblk, j), n}, {col(wv, j), n});
            }
            else
            {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    col(wv, j)[i] = ccol(rblk, j)[i];
                }
            }
            (void)a.apply({ccol(wv, j), n}, {col(aw, j), n});
            (void)b.apply({ccol(wv, j), n}, {col(bw, j), n});
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(s, q)[i] = ccol(wv, j)[i];
                col(as, q)[i] = ccol(aw, j)[i];
                col(bs, q)[i] = ccol(bw, j)[i];
            }
            const R bn = ortho_col_b(q, q);
            if (bn > drop)
            {
                const T inv = static_cast<T>(R{1} / bn);
                dn::scal<T>(inv, {col(s, q), n});
                dn::scal<T>(inv, {col(as, q), n});
                dn::scal<T>(inv, {col(bs, q), n});
                ++q;
            }
        }
        for (crd::u32 j = 0; j < pcols; ++j) // P (images maintained from the last combination)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                col(s, q)[i] = ccol(p, j)[i];
                col(as, q)[i] = ccol(ap, j)[i];
                col(bs, q)[i] = ccol(bp, j)[i];
            }
            const R bn = ortho_col_b(q, q);
            if (bn > drop)
            {
                const T inv = static_cast<T>(R{1} / bn);
                dn::scal<T>(inv, {col(s, q), n});
                dn::scal<T>(inv, {col(as, q), n});
                dn::scal<T>(inv, {col(bs, q), n});
                ++q;
            }
        }

        dn::Symmetric<T> sas(alloc, q); // SᵀBS = I ⇒ RR = eig_sym(SᵀAS)
        for (crd::u32 i = 0; i < q; ++i)
        {
            for (crd::u32 j = 0; j <= i; ++j)
            {
                sas.at(i, j) = dn::dot<T>({ccol(s, i), n}, {ccol(as, j), n});
            }
        }
        dn::EigSym<T> es = dn::eig_sym<T>(alloc, sas);

        for (crd::u32 t = 0; t < k; ++t)
        {
            const crd::u32 src = want_smallest ? t : (q - 1 - t);
            theta[t] = es.values.data()[src];
            T* xnt = col(xn, t);
            T* axnt = col(axn, t);
            T* bxnt = col(bxn, t);
            T* pnt = col(pn, t);
            T* apnt = col(apn, t);
            T* bpnt = col(bpn, t);
            for (crd::u32 i = 0; i < n; ++i)
            {
                xnt[i] = T{0};
                axnt[i] = T{0};
                bxnt[i] = T{0};
                pnt[i] = T{0};
                apnt[i] = T{0};
                bpnt[i] = T{0};
            }
            for (crd::u32 l = 0; l < q; ++l)
            {
                const T cc = es.vectors.at(l, src);
                dn::axpy<T>(cc, {ccol(s, l), n}, {xnt, n});
                dn::axpy<T>(cc, {ccol(as, l), n}, {axnt, n});
                dn::axpy<T>(cc, {ccol(bs, l), n}, {bxnt, n});
                if (l >= k) // the non-X part = the conjugate direction P (with its images)
                {
                    dn::axpy<T>(cc, {ccol(s, l), n}, {pnt, n});
                    dn::axpy<T>(cc, {ccol(as, l), n}, {apnt, n});
                    dn::axpy<T>(cc, {ccol(bs, l), n}, {bpnt, n});
                }
            }
        }
        std::swap(x, xn);
        std::swap(ax, axn);
        std::swap(bx, bxn);
        std::swap(p, pn);
        std::swap(ap, apn);
        std::swap(bp, bpn);
        pcols = k;
    }

    // assemble: values + B-orthonormal vectors (sign-pinned) + generalized residual ‖A·x − λ·B·x‖.
    result.values.resize(k);
    result.vectors.resize(static_cast<crd::usize>(n) * k);
    result.residuals.resize(k);
    nconv = 0;
    for (crd::u32 j = 0; j < k; ++j)
    {
        result.values[j] = crd::hesap::Complex<R>{theta[j], R{0}};
        const T* xj = ccol(x, j);
        const T* axj = ccol(ax, j);
        const T* bxj = ccol(bx, j);
        crd::u32 imax = 0;
        R vmax = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R m = crd::math::fabs(static_cast<R>(xj[i]));
            if (m > vmax)
            {
                vmax = m;
                imax = i;
            }
        }
        const T sgn = (xj[imax] < T{0}) ? T{-1} : T{1};
        T* vj = result.vectors.data() + static_cast<crd::usize>(j) * n;
        R rn2 = R{0};
        R an2 = R{0};
        R bn2 = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            vj[i] = sgn * xj[i];
            const R e = static_cast<R>(axj[i]) - theta[j] * static_cast<R>(bxj[i]);
            rn2 += e * e;
            an2 += static_cast<R>(axj[i]) * static_cast<R>(axj[i]);
            bn2 += static_cast<R>(bxj[i]) * static_cast<R>(bxj[i]);
        }
        const R rn = crd::math::sqrt(rn2);
        result.residuals[j] = rn;
        const R den = crd::math::sqrt(an2) + crd::math::fabs(theta[j]) * crd::math::sqrt(bn2);
        if (rn <= tol * (den > R{0} ? den : R{1}))
        {
            ++nconv;
        }
    }
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    return result;
}

} // namespace crd::hesap::eigen
