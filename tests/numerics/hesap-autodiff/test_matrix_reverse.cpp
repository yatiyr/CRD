// test_matrix_reverse.cpp — Phase 3.1.6 v16-d: the matrix-calculus VJPs. The rigorous gate is the ADJOINT IDENTITY
// ⟨ȳ, JVP(v)⟩ == ⟨VJP(ȳ), v⟩ tested against the FD-gated v15-f JVPs (convention-free — both sides use the same
// tangent v and cotangent ȳ), plus direct central FD on the entrywise-clean ops, plus the VALUE-ONLY degeneracy
// check: logdet/eigvals/svdvals VJPs stay FINITE at repeated spectra where the eigenvector-derivative (F-matrix) NaNs.

#include <crd/hesap/autodiff/matrix_reverse.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace mr  = crd::hesap::autodiff::reverse::matrix;
namespace mj  = crd::hesap::autodiff::forward::matrix;
namespace spr = crd::hesap::autodiff::reverse::sparse;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kMaxN = 8;

f64 frob(const f64* a, const f64* b, int cnt) // Frobenius inner product Σ a_i b_i
{
    f64 s = 0.0;
    for (int i = 0; i < cnt; ++i) { s += a[i] * b[i]; }
    return s;
}

// Well-conditioned SPD A = B·Bᵀ + n·I (n×n, row-major).
void make_spd(f64* a, int n, f64 seed)
{
    f64 b[kMaxN * kMaxN];
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { b[i * n + j] = std::sin(seed + 1.3 * i + 0.7 * j); }
    }
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            f64 s = 0.0;
            for (int k = 0; k < n; ++k) { s += b[i * n + k] * b[j * n + k]; }
            a[i * n + j] = s + (i == j ? static_cast<f64>(n) : 0.0);
        }
    }
}
void make_sym_pert(f64* da, int n, f64 seed) // symmetric perturbation
{
    for (int i = 0; i < n; ++i)
    {
        for (int j = i; j < n; ++j)
        {
            const f64 v  = 0.1 * std::cos(seed + 0.9 * i + 1.1 * j);
            da[i * n + j] = v;
            da[j * n + i] = v;
        }
    }
}

// Cyclic Jacobi symmetric eigensolver: A (sym n×n) → lambda[n], q[n*n] with q[t*n+i] = comp t of eigenvector i.
void jacobi_eig(const f64* ain, f64* q, f64* lam, int n)
{
    f64 w[kMaxN * kMaxN];
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { w[i * n + j] = ain[i * n + j]; q[i * n + j] = (i == j) ? 1.0 : 0.0; }
    }
    for (int sweep = 0; sweep < 100; ++sweep)
    {
        f64 off = 0.0;
        for (int i = 0; i < n; ++i)
        {
            for (int j = i + 1; j < n; ++j) { off += w[i * n + j] * w[i * n + j]; }
        }
        if (off < 1e-30) { break; }
        for (int p = 0; p < n; ++p)
        {
            for (int qq = p + 1; qq < n; ++qq)
            {
                const f64 apq = w[p * n + qq];
                if (std::abs(apq) < 1e-300) { continue; }
                const f64 tau = (w[qq * n + qq] - w[p * n + p]) / (2.0 * apq);
                const f64 t   = (tau >= 0.0 ? 1.0 : -1.0) / (std::abs(tau) + std::sqrt(tau * tau + 1.0));
                const f64 cc  = 1.0 / std::sqrt(t * t + 1.0);
                const f64 ss  = t * cc;
                for (int k = 0; k < n; ++k) // A·J (columns p,qq)
                {
                    const f64 kp = w[k * n + p];
                    const f64 kq = w[k * n + qq];
                    w[k * n + p]  = cc * kp - ss * kq;
                    w[k * n + qq] = ss * kp + cc * kq;
                }
                for (int k = 0; k < n; ++k) // Jᵀ·(A·J) (rows p,qq)
                {
                    const f64 pk = w[p * n + k];
                    const f64 qk = w[qq * n + k];
                    w[p * n + k]  = cc * pk - ss * qk;
                    w[qq * n + k] = ss * pk + cc * qk;
                }
                for (int k = 0; k < n; ++k) // V·J
                {
                    const f64 kp = q[k * n + p];
                    const f64 kq = q[k * n + qq];
                    q[k * n + p]  = cc * kp - ss * kq;
                    q[k * n + qq] = ss * kp + cc * kq;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) { lam[i] = w[i * n + i]; }
}

// One-sided Jacobi SVD: A (m×n, m≥n) → sigma[n], u[m*n] (U cols), v[n*n] (V cols).
void jacobi_svd(const f64* ain, f64* u, f64* v, f64* sigma, int m, int n)
{
    f64 w[kMaxN * kMaxN];
    for (int i = 0; i < m * n; ++i) { w[i] = ain[i]; }
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { v[i * n + j] = (i == j) ? 1.0 : 0.0; }
    }
    for (int sweep = 0; sweep < 100; ++sweep)
    {
        f64 maxg = 0.0;
        for (int p = 0; p < n; ++p)
        {
            for (int qq = p + 1; qq < n; ++qq)
            {
                f64 alpha = 0.0;
                f64 beta = 0.0;
                f64 gamma = 0.0;
                for (int k = 0; k < m; ++k)
                {
                    alpha += w[k * n + p] * w[k * n + p];
                    beta += w[k * n + qq] * w[k * n + qq];
                    gamma += w[k * n + p] * w[k * n + qq];
                }
                maxg = std::max(maxg, std::abs(gamma));
                if (std::abs(gamma) < 1e-300) { continue; }
                const f64 zeta = (beta - alpha) / (2.0 * gamma);
                const f64 t    = (zeta >= 0.0 ? 1.0 : -1.0) / (std::abs(zeta) + std::sqrt(1.0 + zeta * zeta));
                const f64 cc   = 1.0 / std::sqrt(1.0 + t * t);
                const f64 ss   = cc * t;
                for (int k = 0; k < m; ++k)
                {
                    const f64 kp = w[k * n + p];
                    const f64 kq = w[k * n + qq];
                    w[k * n + p]  = cc * kp - ss * kq;
                    w[k * n + qq] = ss * kp + cc * kq;
                }
                for (int k = 0; k < n; ++k)
                {
                    const f64 kp = v[k * n + p];
                    const f64 kq = v[k * n + qq];
                    v[k * n + p]  = cc * kp - ss * kq;
                    v[k * n + qq] = ss * kp + cc * kq;
                }
            }
        }
        if (maxg < 1e-28) { break; }
    }
    for (int i = 0; i < n; ++i)
    {
        f64 nrm = 0.0;
        for (int k = 0; k < m; ++k) { nrm += w[k * n + i] * w[k * n + i]; }
        nrm       = std::sqrt(nrm);
        sigma[i]  = nrm;
        for (int k = 0; k < m; ++k) { u[k * n + i] = w[k * n + i] / nrm; }
    }
}
} // namespace

TEST_CASE("v16-d: gemm VJP == transpose of gemm JVP, and == central FD", "[autodiff][reverse][matrix]")
{
    constexpr int m = 3;
    constexpr int k = 4;
    constexpr int p = 2;
    f64           a[m * k];
    f64           b[k * p];
    f64           da[m * k];
    f64           db[k * p];
    f64           gc[m * p];
    for (int i = 0; i < m * k; ++i) { a[i] = 0.3 + 0.1 * i; da[i] = std::sin(0.4 + i); }
    for (int i = 0; i < k * p; ++i) { b[i] = -0.2 + 0.15 * i; db[i] = std::cos(0.7 + i); }
    for (int i = 0; i < m * p; ++i) { gc[i] = 0.5 - 0.2 * i; }
    // transpose identity: ⟨gc, dC⟩ == ⟨gA,dA⟩ + ⟨gB,dB⟩
    f64 dc[m * p];
    f64 scr[m * p];
    mj::gemm_jvp(a, b, da, db, dc, m, k, p, scr);
    f64 ga[m * k];
    f64 gb[k * p];
    mr::gemm_vjp(a, b, gc, ga, gb, m, k, p);
    CHECK_THAT(frob(gc, dc, m * p), WithinAbs(frob(ga, da, m * k) + frob(gb, db, k * p), 1e-11));
    // direct FD on a couple of A entries
    auto loss = [&]() -> f64 { f64 c[m * p]; mj::gemm(a, b, c, m, k, p); return frob(gc, c, m * p); };
    const f64 h = 1e-6;
    for (int e = 0; e < m * k; e += 3)
    {
        const f64 sv = a[e];
        a[e]         = sv + h;
        const f64 fp = loss();
        a[e]         = sv - h;
        const f64 fm = loss();
        a[e]         = sv;
        CHECK_THAT(ga[e], WithinAbs((fp - fm) / (2.0 * h), 1e-7));
    }
}

TEST_CASE("v16-d: general solve VJP (LU factor-reuse) == central FD, deterministic", "[autodiff][reverse][matrix]")
{
    constexpr int n = 5;
    constexpr int p = 2;
    f64           a[n * n];
    f64           b[n * p];
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { a[i * n + j] = std::sin(0.3 + 1.7 * i + 0.9 * j) + (i == j ? 6.0 : 0.0); }
    }
    for (int i = 0; i < n * p; ++i) { b[i] = 0.4 * std::cos(0.5 + i); }
    // forward solve x = A⁻¹B (fresh LU)
    f64 alu[n * n];
    f64 x[n * p];
    f64 tmp[n];
    f64 rhs[n];
    f64 sol[n];
    int piv[n];
    auto solve = [&](const f64* aa, const f64* bb, f64* xx)
    {
        for (int i = 0; i < n * n; ++i) { alu[i] = aa[i]; }
        spr::dense_lu_factor(alu, piv, n);
        for (int j = 0; j < p; ++j)
        {
            for (int i = 0; i < n; ++i) { rhs[i] = bb[i * p + j]; }
            spr::dense_lu_solve(alu, piv, rhs, sol, n);
            for (int i = 0; i < n; ++i) { xx[i * p + j] = sol[i]; }
        }
    };
    solve(a, b, x);
    // factor once, reuse for the VJP
    for (int i = 0; i < n * n; ++i) { alu[i] = a[i]; }
    spr::dense_lu_factor(alu, piv, n);
    f64 xbar[n * p];
    for (int i = 0; i < n * p; ++i) { xbar[i] = 0.3 * std::sin(1.0 + 0.7 * i); }
    f64 ga[n * n];
    f64 gb[n * p];
    f64 ga2[n * n];
    f64 gb2[n * p];
    mr::solve_lu_vjp(alu, piv, x, xbar, ga, gb, n, p, rhs, sol, tmp);
    mr::solve_lu_vjp(alu, piv, x, xbar, ga2, gb2, n, p, rhs, sol, tmp);
    for (int i = 0; i < n * n; ++i) { CHECK(ga[i] == ga2[i]); } // deterministic
    auto loss = [&]() -> f64 { f64 xx[n * p]; solve(a, b, xx); return frob(xbar, xx, n * p); };
    const f64 h = 1e-6;
    for (int e = 0; e < n * n; e += 4)
    {
        const f64 sv = a[e];
        a[e]         = sv + h;
        const f64 fp = loss();
        a[e]         = sv - h;
        const f64 fm = loss();
        a[e]         = sv;
        CHECK_THAT(ga[e], WithinAbs((fp - fm) / (2.0 * h), 1e-6));
    }
    for (int e = 0; e < n * p; ++e)
    {
        const f64 sv = b[e];
        b[e]         = sv + h;
        const f64 fp = loss();
        b[e]         = sv - h;
        const f64 fm = loss();
        b[e]         = sv;
        CHECK_THAT(gb[e], WithinAbs((fp - fm) / (2.0 * h), 1e-7));
    }
}

TEST_CASE("v16-d: SPD solve VJP == transpose of the v15-f SPD solve JVP", "[autodiff][reverse][matrix]")
{
    constexpr int n = 5;
    constexpr int p = 2;
    f64           a[n * n];
    f64           l[n * n];
    f64           b[n * p];
    make_spd(a, n, 0.6);
    REQUIRE(mj::cholesky(a, l, n));
    for (int i = 0; i < n * p; ++i) { b[i] = 0.5 * std::cos(0.4 + i); }
    // x = A⁻¹B
    f64 x[n * p];
    f64 t1[n * p];
    mj::trisolve_lower(l, b, t1, n, p);
    mj::trisolve_lower_t(l, t1, x, n, p);
    // random symmetric dA + random dB; cotangent x̄
    f64 da[n * n];
    f64 db[n * p];
    f64 xbar[n * p];
    make_sym_pert(da, n, 1.2);
    for (int i = 0; i < n * p; ++i) { db[i] = 0.1 * std::sin(0.8 + i); xbar[i] = 0.3 * std::cos(0.2 + i); }
    f64 dx[n * p];
    f64 r[n * p];
    mj::solve_spd_jvp(l, x, da, db, dx, n, p, r);
    f64 ga[n * n];
    f64 gb[n * p];
    f64 m1[n * n];
    mr::solve_spd_vjp(l, x, xbar, ga, gb, n, p, t1, m1);
    // ⟨x̄, dX⟩ == ⟨Ā, dA⟩ + ⟨B̄, dB⟩  (Ā symmetric, dA symmetric)
    CHECK_THAT(frob(xbar, dx, n * p), WithinAbs(frob(ga, da, n * n) + frob(gb, db, n * p), 1e-9));
}

TEST_CASE("v16-d: Cholesky VJP == transpose of the v15-f Cholesky JVP", "[autodiff][reverse][matrix]")
{
    constexpr int n = 5;
    f64           a[n * n];
    f64           l[n * n];
    make_spd(a, n, 0.9);
    REQUIRE(mj::cholesky(a, l, n));
    f64 da[n * n];
    f64 lbar[n * n];
    make_sym_pert(da, n, 2.1);
    for (int i = 0; i < n; ++i) // L̄ lower-tri random
    {
        for (int j = 0; j < n; ++j) { lbar[i * n + j] = (j <= i) ? 0.2 * std::sin(0.3 + 1.1 * i + 0.5 * j) : 0.0; }
    }
    f64 dl[n * n];
    f64 m1[n * n];
    f64 m2[n * n];
    mj::cholesky_jvp(l, da, dl, n, m1, m2);
    f64 ga[n * n];
    f64 s[n * n];
    f64 y[n * n];
    f64 w[n * n];
    mr::cholesky_vjp(l, lbar, ga, n, s, y, w);
    // ⟨L̄, dL⟩ == ⟨Ā, dA⟩  (dA symmetric, Ā symmetric)
    CHECK_THAT(frob(lbar, dl, n * n), WithinAbs(frob(ga, da, n * n), 1e-9));
}

TEST_CASE("v16-d: logdet VJP (SPD + general LU) == transpose JVP / FD, value-only", "[autodiff][reverse][matrix]")
{
    constexpr int n = 5;
    f64           a[n * n];
    f64           l[n * n];
    make_spd(a, n, 0.35);
    REQUIRE(mj::cholesky(a, l, n));
    f64 da[n * n];
    make_sym_pert(da, n, 1.7);
    const f64 gbar = 0.8;
    f64       m1[n * n];
    f64       m2[n * n];
    const f64 dy = mj::logdet_spd_jvp(l, da, n, m1, m2);
    f64       ga[n * n];
    f64       eye[n * n];
    f64       yy[n * n];
    mr::logdet_spd_vjp(l, gbar, ga, n, eye, yy);
    CHECK_THAT(gbar * dy, WithinAbs(frob(ga, da, n * n), 1e-9)); // ⟨ḡ, dy⟩ == ⟨Ā, dA⟩

    // general LU logdet: FD
    f64 ag[n * n];
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { ag[i * n + j] = std::cos(0.2 + 1.3 * i + 0.8 * j) + (i == j ? 7.0 : 0.0); }
    }
    f64 alu[n * n];
    f64 tmp[n];
    f64 e[n];
    f64 xx[n];
    int piv[n];
    for (int i = 0; i < n * n; ++i) { alu[i] = ag[i]; }
    spr::dense_lu_factor(alu, piv, n);
    f64 gag[n * n];
    mr::logdet_lu_vjp(alu, piv, gbar, gag, n, e, xx, tmp);
    auto logdet = [&](const f64* aa) -> f64 // via a fresh LU: logdet = Σ log|U_ii|
    {
        f64 w[n * n];
        int pv[n];
        for (int i = 0; i < n * n; ++i) { w[i] = aa[i]; }
        spr::dense_lu_factor(w, pv, n);
        f64 s = 0.0;
        for (int i = 0; i < n; ++i) { s += std::log(std::abs(w[i * n + i])); }
        return gbar * s;
    };
    const f64 h = 1e-6;
    for (int ee = 0; ee < n * n; ee += 4)
    {
        const f64 sv = ag[ee];
        ag[ee]       = sv + h;
        const f64 fp = logdet(ag);
        ag[ee]       = sv - h;
        const f64 fm = logdet(ag);
        ag[ee]       = sv;
        CHECK_THAT(gag[ee], WithinAbs((fp - fm) / (2.0 * h), 1e-6));
    }
}

TEST_CASE("v16-d: eigvals VJP == transpose JVP, FINITE at repeated eigenvalues (value-only)",
          "[autodiff][reverse][matrix]")
{
    constexpr int n = 5;
    f64           a[n * n];
    f64           q[n * n];
    f64           lam[n];
    make_spd(a, n, 1.5);
    jacobi_eig(a, q, lam, n);
    // validate the helper: A ≈ Q Λ Qᵀ
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            f64 s = 0.0;
            for (int t = 0; t < n; ++t) { s += q[i * n + t] * lam[t] * q[j * n + t]; }
            CHECK_THAT(s, WithinAbs(a[i * n + j], 1e-9));
        }
    }
    f64 da[n * n];
    f64 lbar[n];
    make_sym_pert(da, n, 0.4);
    for (int i = 0; i < n; ++i) { lbar[i] = 0.3 * std::sin(0.6 + i); }
    f64 dlam[n];
    f64 tmp[n * n];
    mj::eigvals_jvp(q, da, dlam, n, tmp);
    f64 ga[n * n];
    mr::eigvals_sym_vjp(q, lbar, ga, n, tmp);
    CHECK_THAT(frob(lbar, dlam, n), WithinAbs(frob(ga, da, n * n), 1e-9)); // ⟨λ̄, dλ⟩ == ⟨Ā, dA⟩

    // ★ degeneracy: A = 2·I (all eigenvalues repeated) — the VJP is FINITE (no 1/(λ_i−λ_j)); torch/JAX eigvec-deriv NaN.
    f64 arep[n * n];
    f64 qr[n * n];
    f64 lamr[n];
    for (int i = 0; i < n * n; ++i) { arep[i] = 0.0; }
    for (int i = 0; i < n; ++i) { arep[i * n + i] = 2.0; }
    jacobi_eig(arep, qr, lamr, n);
    f64 gar[n * n];
    mr::eigvals_sym_vjp(qr, lbar, gar, n, tmp);
    for (int i = 0; i < n * n; ++i) { CHECK(std::isfinite(gar[i])); }
}

TEST_CASE("v16-d: svdvals VJP == transpose JVP, FINITE at repeated singular values (value-only)",
          "[autodiff][reverse][matrix]")
{
    constexpr int m = 6;
    constexpr int n = 4;
    f64           a[m * n];
    f64           u[m * n];
    f64           v[n * n];
    f64           sig[n];
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j) { a[i * n + j] = std::sin(0.5 + 1.1 * i + 0.6 * j); }
    }
    jacobi_svd(a, u, v, sig, m, n);
    // validate: A ≈ U Σ Vᵀ
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            f64 s = 0.0;
            for (int t = 0; t < n; ++t) { s += u[i * n + t] * sig[t] * v[j * n + t]; }
            CHECK_THAT(s, WithinAbs(a[i * n + j], 1e-9));
        }
    }
    f64 da[m * n];
    f64 sbar[n];
    for (int i = 0; i < m * n; ++i) { da[i] = 0.1 * std::cos(0.3 + i); }
    for (int i = 0; i < n; ++i) { sbar[i] = 0.3 * std::sin(0.7 + i); }
    f64 dsig[n];
    f64 tmp[m * n];
    mj::svdvals_jvp(u, v, da, dsig, m, n, tmp);
    f64 ga[m * n];
    mr::svdvals_vjp(u, v, sbar, ga, m, n, tmp);
    CHECK_THAT(frob(sbar, dsig, n), WithinAbs(frob(ga, da, m * n), 1e-9)); // ⟨σ̄, dσ⟩ == ⟨Ā, dA⟩

    // ★ degeneracy: identity-columns matrix (repeated σ=1) — VJP finite where dU/dV would need 1/(σ_j²−σ_i²).
    f64 arep[m * n];
    f64 ur[m * n];
    f64 vr[n * n];
    f64 sr[n];
    for (int i = 0; i < m * n; ++i) { arep[i] = 0.0; }
    for (int i = 0; i < n; ++i) { arep[i * n + i] = 1.0; }
    jacobi_svd(arep, ur, vr, sr, m, n);
    f64 gar[m * n];
    mr::svdvals_vjp(ur, vr, sbar, gar, m, n, tmp);
    for (int i = 0; i < m * n; ++i) { CHECK(std::isfinite(gar[i])); }
}
