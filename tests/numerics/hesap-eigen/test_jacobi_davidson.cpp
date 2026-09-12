// crd-hesap-eigen v6-f — Jacobi-Davidson (JDQR, symmetric). Validates: (1) EXTREME eigenpairs (smallest +
// largest) vs the analytic 1D-Laplacian spectrum; (2) CLUSTERED / MULTIPLE eigenvalues — a square 2D Laplacian
// has an EXACTLY degenerate pair among its smallest 4, and one-at-a-time deflation must find both (assert the
// VALUES; the eigenVECTORS in a degenerate subspace are non-unique, so we do NOT compare them); (3) the
// preconditioning MECHANISM — a real SPD preconditioner cuts the TOTAL matvec count vs unpreconditioned (NOT a
// cross-library wall-clock crush, which is v6-z); (4)+(5) the {1,2,4,8} determinism MOAT, both UNPRECONDITIONED
// (the core) and with IC0 in the loop (the projected-precond path — JD's identity, must not be dead-under-moat).

#include <crd/containers/array.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/preconditioners/ic0.hpp>
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
namespace pc = crd::hesap::preconditioners;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;
constexpr double kPi = 3.14159265358979323846;

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

// 2D 5-point Dirichlet Laplacian on an mx×my grid, n = mx·my. A SQUARE grid (mx==my) makes λ_{p,q} = λ_{q,p}
// ⇒ an exactly degenerate pair (e.g. λ_{1,2}=λ_{2,1}) among the smallest — the multiplicity test.
Csr laplacian_2d(crd::memory::IAllocator* a, crd::u32 mx, crd::u32 my)
{
    const crd::u32 n = mx * my;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto id = [&](crd::u32 i, crd::u32 j) { return i * my + j; };
    for (crd::u32 i = 0; i < mx; ++i)
    {
        for (crd::u32 j = 0; j < my; ++j)
        {
            const crd::u32 r = id(i, j);
            tb.add(r, r, 4.0);
            if (i + 1 < mx)
            {
                tb.add(r, id(i + 1, j), -1.0);
                tb.add(id(i + 1, j), r, -1.0);
            }
            if (j + 1 < my)
            {
                tb.add(r, id(i, j + 1), -1.0);
                tb.add(id(i, j + 1), r, -1.0);
            }
        }
    }
    return tb.compress();
}

double lam_1d(crd::u32 k, crd::u32 n) // k = 1..n
{
    return 2.0 - 2.0 * std::cos(k * kPi / (n + 1));
}

void smallest_2d(crd::u32 mx, crd::u32 my, crd::u32 cnt, double* out)
{
    double buf[256];
    crd::u32 nb = 0;
    for (crd::u32 p = 1; p <= mx && p <= 8; ++p)
    {
        for (crd::u32 q = 1; q <= my && q <= 8; ++q)
        {
            buf[nb++] = (2.0 - 2.0 * std::cos(p * kPi / (mx + 1))) + (2.0 - 2.0 * std::cos(q * kPi / (my + 1)));
        }
    }
    std::sort(buf, buf + nb);
    for (crd::u32 s = 0; s < cnt; ++s)
    {
        out[s] = buf[s];
    }
}

void sorted_vals(const eig::EigenResult<crd::f64>& r, double* out, crd::u32 cnt)
{
    for (crd::u32 s = 0; s < cnt; ++s)
    {
        out[s] = r.values[s].re;
    }
    std::sort(out, out + cnt);
}
} // namespace

TEST_CASE("v6-f Jacobi-Davidson finds the extreme 1D-Laplacian eigenpairs vs analytic", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 40;
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> op(a);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.tol = 1e-8;
    opts.max_restarts = 400;

    SECTION("smallest")
    {
        opts.which = eig::Which::SmallestAlgebraic;
        auto r = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc);
        REQUIRE(r.values.size() == 4);
        REQUIRE(r.converged);
        double got[4];
        sorted_vals(r, got, 4);
        for (crd::u32 k = 0; k < 4; ++k)
        {
            CHECK(std::fabs(got[k] - lam_1d(k + 1, n)) < 1e-6);
        }
        // true residual ‖A·x − θ·x‖ small for each returned pair
        for (crd::u32 j = 0; j < 4; ++j)
        {
            CHECK(r.residuals[j] < 1e-6);
        }
    }
    SECTION("largest (clustered near 4)")
    {
        opts.which = eig::Which::LargestAlgebraic;
        auto r = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc);
        REQUIRE(r.values.size() == 4);
        REQUIRE(r.converged);
        double got[4];
        sorted_vals(r, got, 4); // ascending; compare to the 4 largest analytic (k = n-3..n), also ascending
        for (crd::u32 t = 0; t < 4; ++t)
        {
            CHECK(std::fabs(got[t] - lam_1d(n - 3 + t, n)) < 1e-6);
        }
    }
}

TEST_CASE("v6-f Jacobi-Davidson resolves an EXACTLY degenerate eigenvalue (deflation through multiplicity)",
          "[hesap][eigen][v6]")
{
    // Square grid ⇒ λ_{1,2} = λ_{2,1} (multiplicity 2) sits in the smallest 4. One-at-a-time deflation must lock
    // both copies. The eigenvectors of a degenerate subspace are non-unique, so we assert the VALUES only.
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 m = 10;
    Csr a = laplacian_2d(&alloc, m, m);
    sp::SparseLinearOp<crd::f64> op(a);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-8;
    opts.max_restarts = 600;

    auto r = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc);
    REQUIRE(r.values.size() == 4);
    REQUIRE(r.converged);

    double got[4];
    double exp[4];
    sorted_vals(r, got, 4);
    smallest_2d(m, m, 4, exp);
    for (crd::u32 k = 0; k < 4; ++k)
    {
        CHECK(std::fabs(got[k] - exp[k]) < 1e-6);
    }
    // The degenerate pair (smallest 2nd and 3rd) is genuinely repeated — JD returned both, distinct slots.
    CHECK(std::fabs(got[1] - got[2]) < 1e-6);
    CHECK(std::fabs(exp[1] - exp[2]) < 1e-9);
}

TEST_CASE("v6-f preconditioning cuts JD's total matvec count (mechanism, not a wall-clock crush)",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 27);
    const crd::u32 mx = 16;
    const crd::u32 my = 20; // rectangular ⇒ non-degenerate smallest modes
    Csr a = laplacian_2d(&alloc, mx, my);
    sp::SparseLinearOp<crd::f64> op(a);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-8;
    opts.max_restarts = 600;

    auto r_none = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc); // unpreconditioned

    pc::Ic0Preconditioner<crd::f64> ic0(a, &alloc); // K ≈ A⁻¹ (SPD by construction)
    auto r_ic0 = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc, &ic0);

    crd::hesap::amg::SaAmg<crd::f64> amg(a, &alloc); // AMG V-cycle (SPD)
    auto r_amg = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc, &amg);

    INFO("JD matvecs: unpreconditioned=" << r_none.iterations << " IC0=" << r_ic0.iterations
                                         << " AMG=" << r_amg.iterations);
    REQUIRE(r_none.converged);
    REQUIRE(r_ic0.converged);
    REQUIRE(r_amg.converged);

    // Same eigenvalues regardless of preconditioner (it only changes the convergence rate / matvec count).
    double exp[4];
    double g_none[4];
    double g_ic0[4];
    double g_amg[4];
    smallest_2d(mx, my, 4, exp);
    sorted_vals(r_none, g_none, 4);
    sorted_vals(r_ic0, g_ic0, 4);
    sorted_vals(r_amg, g_amg, 4);
    for (crd::u32 k = 0; k < 4; ++k)
    {
        CHECK(std::fabs(g_none[k] - exp[k]) < 1e-6);
        CHECK(std::fabs(g_ic0[k] - exp[k]) < 1e-6);
        CHECK(std::fabs(g_amg[k] - exp[k]) < 1e-6);
    }
    // The mechanism: a real SPD preconditioner reaches tolerance in strictly fewer total matvecs.
    CHECK(r_ic0.iterations < r_none.iterations);
    CHECK(r_amg.iterations < r_none.iterations);
}

namespace
{
// Run JD across {1,2,4,8} workers (forced-parallel spmv) and assert the eigenpairs are bit-identical. `use_ic0`
// puts a deterministic IC0 preconditioner in the correction loop (covers the projected-precond path).
void jd_moat(crd::u32 n, bool use_ic0)
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    Csr a = laplacian_1d(&alloc, n);
    pc::Ic0Preconditioner<crd::f64> ic0(a, &alloc);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic; // well-separated ⇒ unique eigenvectors (moat ground rule)
    opts.tol = 1e-8;
    opts.max_restarts = 400;

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
            auto r = eig::eigs_sym_jd<crd::f64>(op, opts, &alloc, use_ic0 ? &ic0 : nullptr);
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
                CHECK(ident); // eigenpairs bit-identical across worker counts
            }
        }
        crd::jobs::shutdown();
    }
}
} // namespace

TEST_CASE("v6-f Jacobi-Davidson determinism moat {1,2,4,8} (unpreconditioned)", "[hesap][eigen][v6][moat]")
{
    jd_moat(64, /*use_ic0=*/false);
}

TEST_CASE("v6-f Jacobi-Davidson determinism moat {1,2,4,8} (IC0-preconditioned correction)",
          "[hesap][eigen][v6][moat]")
{
    jd_moat(64, /*use_ic0=*/true);
}
