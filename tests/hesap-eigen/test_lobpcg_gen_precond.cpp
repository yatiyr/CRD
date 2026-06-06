// crd-hesap-eigen v6-e-d — PRECONDITIONED GENERALIZED LOBPCG + the advisor-flagged DROP-branch coverage.
// This is the FIRST-ever exercise of the generalized precond path (lobpcg.hpp eigs_sym_gen_lobpcg, lines ~540):
// v6-e-c only ever ran it with precond == nullptr. The slice closes three v6-e-c ⚠OWED items:
//
//   (A) MECHANISM — a genuine SPD preconditioner built on the STIFFNESS A = K (T ≈ K⁻¹, the textbook
//       preconditioner for the SMALLEST generalized pairs of K·x = λ·M·x) cuts LOBPCG's iterations-to-tolerance
//       vs unpreconditioned, WITHOUT changing which eigenvalues are found. Honest framing (advisor-gated): this
//       is a MECHANISM proof, NOT a cross-library crush — a V-cycle / triangular solve is not an A-apply, so the
//       iteration count is not comparable across libraries (that fair fight is the v6-z bench).
//   (B) MOAT — {1,2,4,8} bit-identical eigenpairs WITH a preconditioner in the loop. Uses IC0 (deterministic +
//       serial-applied); AMG (parallel internals) stays in the mechanism test only.
//   (C) DROP-BRANCH coverage — the B-modified-Gram-Schmidt rank-deficiency branch (`if bn > drop`) never fired
//       in the v6-e-c tests. We PROVE it fires by a dimension argument: with n = 10, nev = 4 the subspace
//       S = [X, W, P] presents 3·nev = 12 candidate columns, but at most n = 10 can be B-orthonormal, so once P
//       is populated (iteration ≥ 1) at least 2 columns MUST be dropped. `iterations ≥ 3` proves the iteration-1
//       body ran (the loop sets iterations = iter+1 at the top, so reaching 3 means iter-1 did not break), and
//       the eigenvalues are still correct ⇒ the branch fired AND is handled gracefully.

#include <crd/containers/array.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
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
namespace dn = crd::hesap::dense;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// 2D Laplacian (5-point, Dirichlet) on an mx×my grid, n = mx·my. diag = 4, 4 neighbours = −1. SPD M-matrix,
// nonsingular (the Dirichlet stiffness K — the IC0/AMG target). Node id = i·my + j.
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

double diag_b(crd::u32 i) // a non-uniform SPD lumped mass in [1, 2) (bit-reproducible, IEEE division)
{
    return 1.0 + static_cast<double>((i * 37U + 11U) % 100U) / 100.0;
}

Csr diag_spd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, diag_b(i));
    }
    return tb.compress();
}

// The generalized eigenvalues of (K, diag(d)) are the eigenvalues of the symmetric C = D^{-1/2}·K·D^{-1/2}.
// Build C densely from K's CSR and run the dense eig_sym ⇒ the rigorous reference for the `cnt` smallest.
crd::containers::Array<crd::f64> gen_smallest_ref(crd::memory::IAllocator* a, const Csr& k, crd::u32 cnt)
{
    const crd::u32 n = k.rows();
    dn::Symmetric<crd::f64> c(a, n); // zero-initialized; we set the lower triangle
    const auto* outer = k.pattern().outer_ptr.data();
    const auto* inner = k.pattern().inner_idx.data();
    const auto& kv = k.values().values;
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (auto p = outer[i]; p < outer[i + 1]; ++p)
        {
            const crd::u32 j = inner[p];
            if (j <= i)
            {
                c.at(i, j) = kv[p] / std::sqrt(diag_b(i) * diag_b(j));
            }
        }
    }
    dn::EigSym<crd::f64> es = dn::eig_sym<crd::f64>(a, c);
    crd::containers::Array<crd::f64> out(a);
    out.resize(cnt);
    for (crd::u32 s = 0; s < cnt; ++s)
    {
        out[s] = es.values.data()[s];
    }
    return out;
}

void sorted4(const eig::EigenResult<crd::f64>& r, double* out)
{
    for (crd::u32 s = 0; s < 4; ++s)
    {
        out[s] = r.values[s].re;
    }
    std::sort(out, out + 4);
}
} // namespace

TEST_CASE("v6-e-d preconditioned generalized LOBPCG: SPD preconditioning cuts iterations (mechanism)",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 27);
    const crd::u32 mx = 20;
    const crd::u32 my = 24; // rectangular ⇒ non-degenerate smallest generalized modes
    Csr k = laplacian_2d(&alloc, mx, my);
    Csr m = diag_spd(&alloc, mx * my); // lumped mass M (diagonal SPD)

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-7;
    opts.max_restarts = 1500;

    sp::SparseLinearOp<crd::f64> kop(k);
    sp::SparseLinearOp<crd::f64> mop(m);

    auto r_none = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc); // unpreconditioned baseline

    pc::Ic0Preconditioner<crd::f64> ic0(k, &alloc);               // T ≈ K⁻¹ (SPD by construction)
    auto r_ic0 = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc, &ic0);

    crd::hesap::amg::SaAmg<crd::f64> amg(k, &alloc);              // AMG-on-K V-cycle (SPD)
    auto r_amg = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc, &amg);

    INFO("gen iters: unpreconditioned=" << r_none.iterations << " IC0=" << r_ic0.iterations
                                        << " AMG=" << r_amg.iterations);
    REQUIRE(r_none.converged);
    REQUIRE(r_ic0.converged);
    REQUIRE(r_amg.converged);

    // All three find the SAME smallest 4 generalized eigenvalues (a valid SPD preconditioner changes only the
    // convergence rate, never WHICH eigenpairs), and they match the dense reference.
    crd::containers::Array<crd::f64> ref = gen_smallest_ref(&alloc, k, 4);
    double g_none[4];
    double g_ic0[4];
    double g_amg[4];
    sorted4(r_none, g_none);
    sorted4(r_ic0, g_ic0);
    sorted4(r_amg, g_amg);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(g_none[s] - ref[s]) < 1e-6);
        CHECK(std::fabs(g_ic0[s] - ref[s]) < 1e-6);
        CHECK(std::fabs(g_amg[s] - ref[s]) < 1e-6);
    }
    // The mechanism: a real SPD preconditioner reaches tolerance in strictly fewer iterations than unprecond.
    CHECK(r_ic0.iterations < r_none.iterations);
    CHECK(r_amg.iterations < r_none.iterations);
}

TEST_CASE("v6-e-d preconditioned generalized LOBPCG determinism moat {1,2,4,8} (IC0 in the loop)",
          "[hesap][eigen][v6][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 mx = 16;
    const crd::u32 my = 20;
    Csr k = laplacian_2d(&alloc, mx, my);
    Csr m = diag_spd(&alloc, mx * my);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-8;
    opts.max_restarts = 1000;

    // IC0 factor is deterministic + serial-applied; only the A/B matvecs are parallel ⇒ the moat must hold.
    pc::Ic0Preconditioner<crd::f64> ic0(k, &alloc);

    crd::containers::Array<crd::f64> val_ref(&alloc);
    crd::containers::Array<crd::f64> vec_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> kop(k, &alloc, /*parallel_min_stored_bytes=*/0);
            sp::ParallelSparseLinearOp<crd::f64> mop(m, &alloc, /*parallel_min_stored_bytes=*/0);
            auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc, &ic0);
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
                CHECK(ident); // generalized eigenpairs bit-identical across workers WITH IC0 in the loop
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v6-e-d generalized LOBPCG B-MGS rank-deficiency DROP branch fires (and is handled)",
          "[hesap][eigen][v6]")
{
    // PROVING the DROP branch fires (advisor: a test that is *supposed* to trigger a branch but doesn't is
    // vacuous). n = 10, nev = 4 ⇒ the subspace S = [X, W, P] presents 3·nev = 12 candidate columns. At most
    // n = 10 vectors can be B-orthonormal in ℝ¹⁰, so once P is populated (iteration ≥ 1) at LEAST 2 of the 12
    // candidates MUST be dropped by the `if bn > drop` branch — this is a dimension argument, not a hope.
    // `iterations ≥ 3` proves the iteration-1 body executed (the loop sets iterations = iter+1 at the top, so
    // reaching 3 means iter-1 did not break before building S), hence the drop fired; and the eigenvalues match
    // the dense reference ⇒ the branch is handled gracefully (the dropped columns are genuinely redundant).
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 10;
    Csr k = laplacian_1d(&alloc, n);
    Csr m = diag_spd(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-13; // tight ⇒ does not converge in one step ⇒ reaches the iteration-1 body (drop fires)
    opts.max_restarts = 200;

    sp::SparseLinearOp<crd::f64> kop(k);
    sp::SparseLinearOp<crd::f64> mop(m);
    auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc);
    REQUIRE(r.values.size() == 4);

    INFO("DROP test iterations=" << r.iterations << " converged=" << r.converged);
    REQUIRE(r.iterations >= 3); // proves the iteration-1 body ran ⇒ 12 > 10 ⇒ the DROP branch fired
    REQUIRE(r.converged);

    crd::containers::Array<crd::f64> ref = gen_smallest_ref(&alloc, k, 4);
    double got[4];
    sorted4(r, got);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(got[s] - ref[s]) < 1e-7); // correct eigenvalues despite the forced rank-deficiency drops
    }
}
