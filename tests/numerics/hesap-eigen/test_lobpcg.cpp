// crd-hesap-eigen v6-e-a — block LOBPCG: the `nev` extreme eigenpairs of a symmetric operator at once.
// 1D Laplacian has analytic eigenvalues λ_k = 2−2cos(kπ/(n+1)); LOBPCG recovers the SMALLEST few. Plus the
// optional-preconditioner interface (a Jacobi diagonal op) and the {1,2,4,8} determinism moat.

#include <crd/containers/array.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

namespace eig = crd::hesap::eigen;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

Csr laplacian_1d(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 2.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
            tb.add(i + 1, i, -1.0);
        }
    }
    return tb.compress();
}

// A NON-TRIVIAL SPD diagonal preconditioner T = diag(d_i), d_i NON-UNIFORM in [0.4, 0.6). Any positive diagonal
// is a valid preconditioner (it changes the convergence rate, not which eigenvalues), and a non-uniform d makes
// W = T·R a genuinely different search direction per component (a uniform/scalar diagonal would be a no-op).
Csr spd_diagonal_precond(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double d = 0.4 + 0.2 * (static_cast<double>((i * 37U + 11U) % 100U) / 100.0);
        tb.add(i, i, d);
    }
    return tb.compress();
}

double lambda_k(crd::u32 n, crd::u32 k) // k = 1..n
{
    const double pi = 3.14159265358979323846;
    return 2.0 - 2.0 * std::cos(static_cast<double>(k) * pi / static_cast<double>(n + 1));
}
} // namespace

TEST_CASE("v6-e LOBPCG finds the smallest Laplacian eigenvalues (block, unpreconditioned)", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 100;
    Csr a = laplacian_1d(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-7;
    opts.max_restarts = 500;

    sp::SparseLinearOp<crd::f64> op(a);
    auto r = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc);
    REQUIRE(r.values.size() == 4);

    double got[4];
    double maxres = 0.0;
    for (crd::u32 s = 0; s < 4; ++s)
    {
        got[s] = r.values[s].re;
        maxres = std::max(maxres, r.residuals[s]);
    }
    std::sort(got, got + 4);
    INFO("max residual = " << maxres << "  iters = " << r.iterations << "  got = " << got[0] << "," << got[1] << ","
                           << got[2] << "," << got[3]);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(got[s] - lambda_k(n, s + 1)) < 1e-6); // smallest 4 = λ_1..4
    }
    CHECK(maxres < 1e-6);
    CHECK(r.converged);
}

TEST_CASE("v6-e LOBPCG with a preconditioner LinearOp finds the same eigenvalues", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 100;
    Csr a = laplacian_1d(&alloc, n);
    Csr m = spd_diagonal_precond(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 3;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-7;
    opts.max_restarts = 500;

    sp::SparseLinearOp<crd::f64> op(a);
    sp::SparseLinearOp<crd::f64> precond(m); // non-uniform SPD diagonal ⇒ a real W=T·R direction
    auto r = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc, &precond);
    REQUIRE(r.values.size() == 3);

    double got[3];
    for (crd::u32 s = 0; s < 3; ++s)
    {
        got[s] = r.values[s].re;
    }
    std::sort(got, got + 3);
    for (crd::u32 s = 0; s < 3; ++s)
    {
        CHECK(std::fabs(got[s] - lambda_k(n, s + 1)) < 1e-6);
    }
    CHECK(r.converged);
}

TEST_CASE("v6-e LOBPCG determinism moat {1,2,4,8}", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 120;
    Csr a = laplacian_1d(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-8;
    opts.max_restarts = 500;

    crd::containers::Array<crd::f64> val_ref(&alloc);
    crd::containers::Array<crd::f64> vec_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc);
            REQUIRE(r.values.size() == 4);
            if (!have_ref)
            {
                val_ref.resize(r.values.size());
                for (crd::u32 s = 0; s < r.values.size(); ++s)
                {
                    val_ref[s] = r.values[s].re;
                }
                vec_ref.resize(r.vectors.size());
                for (crd::usize i = 0; i < r.vectors.size(); ++i)
                {
                    vec_ref[i] = r.vectors[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (crd::u32 s = 0; s < r.values.size() && ident; ++s)
                {
                    ident = (r.values[s].re == val_ref[s]);
                }
                for (crd::usize i = 0; i < r.vectors.size() && ident; ++i)
                {
                    ident = (r.vectors[i] == vec_ref[i]);
                }
                CHECK(ident); // eigenvalues + eigenvectors bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
