// crd-hesap-eigen v6-e-b — PRECONDITIONED LOBPCG. Validates the preconditioned path with a genuine SPD
// preconditioner (IC0 = SPD by construction; AMG V-cycle), proves the algorithmic MECHANISM (a real
// preconditioner cuts LOBPCG's iterations-to-tolerance vs unpreconditioned), and re-checks the {1,2,4,8} moat
// WITH the preconditioner in the loop (new code path vs v6-e-a's unpreconditioned moat).
//
// HONESTY (advisor-gated): this is NOT a crush claim. "Fewer iterations" is a MECHANISM proof — each
// preconditioned iteration costs a V-cycle / triangular solve, not an A-apply, so iteration count is not
// comparable across libraries. The fair crush metric is WALL-CLOCK + memory at matched accuracy vs the RIGHT
// peers (shift-invert ARPACK, scipy `lobpcg`) in the regime where it wins (large 3D / FEM, where direct
// factorization fill-in is the killer) — that benchmark is the v6 close, built fresh.

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

// 2D Laplacian (5-point, Dirichlet) on an mx×my grid (row-major), n = mx·my. diag = 4, 4 neighbours = −1.
// SPD M-matrix — the canonical IC0/AMG target. Eigenvalues λ_{p,q} = (2−2cos(pπ/(mx+1)))+(2−2cos(qπ/(my+1))).
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

void smallest_2d_eigs(crd::u32 mx, crd::u32 my, crd::u32 cnt, double* out)
{
    const double pi = 3.14159265358979323846;
    double buf[64];
    crd::u32 nb = 0;
    for (crd::u32 p = 1; p <= 7 && p <= mx; ++p)
    {
        for (crd::u32 q = 1; q <= 7 && q <= my; ++q)
        {
            const double lp = 2.0 - 2.0 * std::cos(p * pi / (mx + 1));
            const double lq = 2.0 - 2.0 * std::cos(q * pi / (my + 1));
            buf[nb++] = lp + lq;
        }
    }
    std::sort(buf, buf + nb);
    for (crd::u32 s = 0; s < cnt; ++s)
    {
        out[s] = buf[s];
    }
}
} // namespace

TEST_CASE("v6-e-b IC0-preconditioned LOBPCG finds the smallest 2D-Laplacian eigenvalues", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 mx = 20;
    const crd::u32 my = 24; // rectangular ⇒ non-degenerate smallest modes
    Csr a = laplacian_2d(&alloc, mx, my);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-7;
    opts.max_restarts = 800;

    sp::SparseLinearOp<crd::f64> op(a);
    pc::Ic0Preconditioner<crd::f64> ic0(a, &alloc); // SPD by construction
    auto r = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc, &ic0);
    REQUIRE(r.values.size() == 4);

    double got[4];
    double exp[4];
    smallest_2d_eigs(mx, my, 4, exp);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        got[s] = r.values[s].re;
    }
    std::sort(got, got + 4);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(got[s] - exp[s]) < 1e-6); // SPD preconditioning must not change WHICH eigenvalues
    }
    CHECK(r.converged);
}

TEST_CASE("v6-e-b preconditioning cuts LOBPCG iterations-to-tolerance (mechanism, not a wall-clock crush)",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 mx = 20;
    const crd::u32 my = 24;
    Csr a = laplacian_2d(&alloc, mx, my);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-7;
    opts.max_restarts = 1500;

    sp::SparseLinearOp<crd::f64> op(a);
    auto r_none = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc); // unpreconditioned

    pc::Ic0Preconditioner<crd::f64> ic0(a, &alloc);
    auto r_ic0 = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc, &ic0);

    crd::hesap::amg::SaAmg<crd::f64> amg(a, &alloc); // V-cycle (fwd-pre + bwd-post GS ⇒ SPD)
    auto r_amg = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc, &amg);

    INFO("iters: unpreconditioned=" << r_none.iterations << " IC0=" << r_ic0.iterations
                                    << " AMG=" << r_amg.iterations);
    REQUIRE(r_none.converged);
    REQUIRE(r_ic0.converged);
    REQUIRE(r_amg.converged);
    // All three find the SAME eigenvalues (a valid SPD preconditioner changes only the convergence rate).
    double exp[4];
    smallest_2d_eigs(mx, my, 4, exp);
    double g_ic0[4];
    double g_amg[4];
    for (crd::u32 s = 0; s < 4; ++s)
    {
        g_ic0[s] = r_ic0.values[s].re;
        g_amg[s] = r_amg.values[s].re;
    }
    std::sort(g_ic0, g_ic0 + 4);
    std::sort(g_amg, g_amg + 4);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(g_ic0[s] - exp[s]) < 1e-6);
        CHECK(std::fabs(g_amg[s] - exp[s]) < 1e-6);
    }
    // The mechanism: a real SPD preconditioner reaches tolerance in strictly fewer iterations.
    CHECK(r_ic0.iterations < r_none.iterations);
    CHECK(r_amg.iterations < r_none.iterations);
}

TEST_CASE("v6-e-b preconditioned LOBPCG determinism moat {1,2,4,8} (IC0 in the loop)", "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 mx = 16;
    const crd::u32 my = 20;
    Csr a = laplacian_2d(&alloc, mx, my);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-8;
    opts.max_restarts = 1000;

    // IC0 factor is deterministic + serial-applied; only the A-matvec is parallel ⇒ the moat must hold.
    pc::Ic0Preconditioner<crd::f64> ic0(a, &alloc);

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
            auto r = eig::eigs_sym_lobpcg<crd::f64>(op, opts, &alloc, &ic0);
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
                CHECK(ident); // eigenpairs bit-identical across worker counts WITH the preconditioner in the loop
            }
        }
        crd::jobs::shutdown();
    }
}
