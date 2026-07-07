// test_sparse_reverse.cpp — Phase 3.1.6 v16-c: reverse-mode VJPs over the CSR sparse-LA surface (spmv / spmm / sparse
// solve), differentiated wrt BOTH the dense operand AND the sparse-matrix ENTRIES — a capability PyTorch/TensorFlow
// lack. Gate: central-FD gradcheck of every gradient (per-nonzero gvals + the dense operand), run-to-run determinism,
// and a solve sanity check (A·x == b).

#include <crd/hesap/autodiff/sparse_reverse.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace sp = crd::hesap::autodiff::reverse::sparse;
using crd::f64;
using Catch::Matchers::WithinAbs;

TEST_CASE("v16-c: CSR spmv VJP (values + x) == central FD, deterministic", "[autodiff][reverse][sparse]")
{
    constexpr int m = 3;
    constexpr int n = 4;
    constexpr int nnz = 6;
    int           row_ptr[m + 1] = {0, 2, 4, 6};
    int           col_idx[nnz]   = {0, 2, 1, 3, 0, 3};
    f64           vals[nnz]      = {1.5, -0.7, 2.1, 0.9, -1.2, 0.4};
    f64           x[n]           = {0.6, -0.4, 1.1, 0.3};
    f64           c[m]           = {0.7, -0.5, 0.9}; // the output cotangent ȳ

    f64 gvals[nnz];
    f64 gx[n];
    f64 gvals2[nnz];
    f64 gx2[n];
    sp::csr_spmv_vjp(row_ptr, col_idx, vals, x, c, gvals, gx, m, n);
    sp::csr_spmv_vjp(row_ptr, col_idx, vals, x, c, gvals2, gx2, m, n);

    auto loss = [&]() -> f64
    {
        f64 y[m];
        sp::csr_spmv(row_ptr, col_idx, vals, x, y, m);
        f64 s = 0.0;
        for (int i = 0; i < m; ++i) { s += c[i] * y[i]; }
        return s;
    };
    const f64 h = 1e-6;
    for (int e = 0; e < nnz; ++e)
    {
        CHECK(gvals[e] == gvals2[e]);
        const f64 sv = vals[e];
        vals[e]      = sv + h;
        const f64 fp = loss();
        vals[e]      = sv - h;
        const f64 fm = loss();
        vals[e]      = sv;
        CHECK_THAT(gvals[e], WithinAbs((fp - fm) / (2.0 * h), 1e-8));
    }
    for (int j = 0; j < n; ++j)
    {
        CHECK(gx[j] == gx2[j]);
        const f64 sv = x[j];
        x[j]         = sv + h;
        const f64 fp = loss();
        x[j]         = sv - h;
        const f64 fm = loss();
        x[j]         = sv;
        CHECK_THAT(gx[j], WithinAbs((fp - fm) / (2.0 * h), 1e-8));
    }
}

TEST_CASE("v16-c: CSR spmm VJP (values + X) == central FD", "[autodiff][reverse][sparse]")
{
    constexpr int m = 3;
    constexpr int n = 4;
    constexpr int p = 2;
    constexpr int nnz = 6;
    int           row_ptr[m + 1] = {0, 2, 4, 6};
    int           col_idx[nnz]   = {0, 2, 1, 3, 0, 3};
    f64           vals[nnz]      = {1.5, -0.7, 2.1, 0.9, -1.2, 0.4};
    f64           x[n * p]       = {0.6, -0.4, 1.1, 0.3, -0.2, 0.8, 0.5, -0.9};
    f64           c[m * p]       = {0.7, -0.5, 0.9, 0.2, -0.6, 0.4}; // Ȳ

    f64 gvals[nnz];
    f64 gx[n * p];
    sp::csr_spmm_vjp(row_ptr, col_idx, vals, x, c, gvals, gx, m, n, p);
    auto loss = [&]() -> f64
    {
        f64 y[m * p];
        sp::csr_spmm(row_ptr, col_idx, vals, x, y, m, p);
        f64 s = 0.0;
        for (int i = 0; i < m * p; ++i) { s += c[i] * y[i]; }
        return s;
    };
    const f64 h = 1e-6;
    for (int e = 0; e < nnz; ++e)
    {
        const f64 sv = vals[e];
        vals[e]      = sv + h;
        const f64 fp = loss();
        vals[e]      = sv - h;
        const f64 fm = loss();
        vals[e]      = sv;
        CHECK_THAT(gvals[e], WithinAbs((fp - fm) / (2.0 * h), 1e-8));
    }
    for (int j = 0; j < n * p; ++j)
    {
        const f64 sv = x[j];
        x[j]         = sv + h;
        const f64 fp = loss();
        x[j]         = sv - h;
        const f64 fm = loss();
        x[j]         = sv;
        CHECK_THAT(gx[j], WithinAbs((fp - fm) / (2.0 * h), 1e-8));
    }
}

TEST_CASE("v16-c: CSR sparse solve VJP (values + b) == central FD, factor-reuse, deterministic",
          "[autodiff][reverse][sparse]")
{
    constexpr int n = 4;
    constexpr int nnz = 10;
    int           row_ptr[n + 1] = {0, 2, 5, 8, 10};
    int           col_idx[nnz]   = {0, 1, 0, 1, 2, 1, 2, 3, 2, 3};
    f64           vals[nnz]      = {4.0, 1.0, 1.0, 5.0, 1.0, 1.0, 6.0, 1.0, 1.0, 4.0}; // diagonally dominant
    f64           b[n]           = {1.0, 2.0, 3.0, 4.0};
    f64           c[n]           = {0.5, -0.3, 0.7, 0.2}; // ∂L/∂x

    f64 x[n];
    f64 a[n * n];
    int piv[n];
    sp::csr_solve(row_ptr, col_idx, vals, b, x, n, a, piv);

    // sanity: A·x == b
    f64 chk[n];
    sp::csr_spmv(row_ptr, col_idx, vals, x, chk, n);
    for (int i = 0; i < n; ++i) { CHECK_THAT(chk[i], WithinAbs(b[i], 1e-10)); }

    f64 gvals[nnz];
    f64 gb[n];
    f64 z[n];
    f64 tmp[n];
    f64 gvals2[nnz];
    f64 gb2[n];
    f64 z2[n];
    f64 tmp2[n];
    sp::csr_solve_vjp(row_ptr, col_idx, x, c, gvals, gb, n, a, piv, z, tmp);
    sp::csr_solve_vjp(row_ptr, col_idx, x, c, gvals2, gb2, n, a, piv, z2, tmp2);

    auto loss = [&]() -> f64
    {
        f64 xl[n];
        f64 al[n * n];
        int pl[n];
        sp::csr_solve(row_ptr, col_idx, vals, b, xl, n, al, pl);
        f64 s = 0.0;
        for (int i = 0; i < n; ++i) { s += c[i] * xl[i]; }
        return s;
    };
    const f64 h = 1e-6;
    for (int e = 0; e < nnz; ++e) // nonlinear in the sparse entries → the factor-reuse VJP vs central FD
    {
        CHECK(gvals[e] == gvals2[e]);
        const f64 sv = vals[e];
        vals[e]      = sv + h;
        const f64 fp = loss();
        vals[e]      = sv - h;
        const f64 fm = loss();
        vals[e]      = sv;
        CHECK_THAT(gvals[e], WithinAbs((fp - fm) / (2.0 * h), 1e-6));
    }
    for (int j = 0; j < n; ++j) // linear in b
    {
        CHECK(gb[j] == gb2[j]);
        const f64 sv = b[j];
        b[j]         = sv + h;
        const f64 fp = loss();
        b[j]         = sv - h;
        const f64 fm = loss();
        b[j]         = sv;
        CHECK_THAT(gb[j], WithinAbs((fp - fm) / (2.0 * h), 1e-8));
    }
}
