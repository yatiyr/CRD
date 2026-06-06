// crd-hesap-eigen v6-g — FEAST (contour-integration eigensolver). Validates: (1) a band over the SMALLEST
// 1D-Laplacian eigenvalues vs analytic (count + values); (2) the DISCRIMINATING test — an INTERIOR band with
// eigenvalues placed JUST OUTSIDE both endpoints, asserting they are EXCLUDED while the inside ones are returned
// (because Q is orthonormalized before the Rayleigh-Ritz, the Ritz values are invariant to the quadrature
// weight's scale, so a value-vs-analytic check alone would NOT catch a wrong contour filter — the in/out
// separation is what the contour governs); (3) count-correctness is robust to the m0 over-estimate (the rank-
// drop absorbs headroom); (4) the {1,2,4,8} determinism MOAT (complex factors built across worker counts).

#include <crd/containers/array.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
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

double lam_1d(crd::u32 k, crd::u32 n) // analytic eigenvalue k = 1..n, ascending in k
{
    return 2.0 - 2.0 * std::cos(k * kPi / (n + 1));
}

// 2D 5-point Dirichlet Laplacian on an mx×my grid (RECTANGULAR ⇒ non-degenerate). Its elimination tree is a
// genuine 2D tree (NOT the 1D chain), so the complex multifrontal LU factor has independent subtrees — the
// moat then exercises a real parallel reduction (a chain would factor serially regardless of worker count).
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

void sorted_vals(const eig::EigenResult<crd::f64>& r, double* out)
{
    const crd::u32 m = static_cast<crd::u32>(r.values.size());
    for (crd::u32 s = 0; s < m; ++s)
    {
        out[s] = r.values[s].re;
    }
    std::sort(out, out + m);
}
} // namespace

TEST_CASE("v6-g FEAST captures the smallest 1D-Laplacian band vs analytic", "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 60;
    Csr a = laplacian_1d(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.tol = 1e-9;
    opts.max_restarts = 30;
    const double lo = -0.01;                            // below λ_1
    const double hi = 0.5 * (lam_1d(4, n) + lam_1d(5, n)); // gap between λ_4 and λ_5
    auto r = eig::eigs_sym_feast<crd::f64>(a, lo, hi, /*m0=*/8, opts, &alloc);

    REQUIRE(r.values.size() == 4); // exactly λ_1..λ_4 inside
    REQUIRE(r.converged);
    double got[8];
    sorted_vals(r, got);
    for (crd::u32 k = 0; k < 4; ++k)
    {
        CHECK(std::fabs(got[k] - lam_1d(k + 1, n)) < 1e-7);
        CHECK(r.residuals[k] < 1e-7);
    }
}

TEST_CASE("v6-g FEAST INTERIOR band: inside returned, just-outside EXCLUDED (the contour filter)",
          "[hesap][eigen][v6]")
{
    // The discriminating test: target the interior eigenvalues k = 28,29,30; put lo in the gap below λ_28 and hi
    // in the gap above λ_30, so λ_27 (just below lo) and λ_31 (just above hi) MUST be excluded.
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 60;
    Csr a = laplacian_1d(&alloc, n);

    const double l27 = lam_1d(27, n);
    const double l28 = lam_1d(28, n);
    const double l29 = lam_1d(29, n);
    const double l30 = lam_1d(30, n);
    const double l31 = lam_1d(31, n);
    const double lo = 0.5 * (l27 + l28); // gap below λ_28 (λ_27 sits just outside)
    const double hi = 0.5 * (l30 + l31); // gap above λ_30 (λ_31 sits just outside)

    eig::EigenOptions<crd::f64> opts;
    opts.tol = 1e-9;
    opts.max_restarts = 30;
    auto r = eig::eigs_sym_feast<crd::f64>(a, lo, hi, /*m0=*/6, opts, &alloc); // headroom 6 > 3

    REQUIRE(r.values.size() == 3); // EXACTLY λ_28, λ_29, λ_30 — not the bracketing pair
    REQUIRE(r.converged);
    double got[6];
    sorted_vals(r, got);
    CHECK(std::fabs(got[0] - l28) < 1e-7);
    CHECK(std::fabs(got[1] - l29) < 1e-7);
    CHECK(std::fabs(got[2] - l30) < 1e-7);
    // EXCLUSION: neither just-outside eigenvalue appears in the result (the contour filter, not the RR).
    for (crd::u32 s = 0; s < 3; ++s)
    {
        CHECK(std::fabs(got[s] - l27) > 1e-4);
        CHECK(std::fabs(got[s] - l31) > 1e-4);
        CHECK(got[s] >= lo);
        CHECK(got[s] <= hi);
    }
}

TEST_CASE("v6-g FEAST count is robust to the m0 over-estimate (rank-drop absorbs headroom)",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 60;
    Csr a = laplacian_1d(&alloc, n);

    eig::EigenOptions<crd::f64> opts;
    opts.tol = 1e-9;
    opts.max_restarts = 30;
    const double lo = 0.5 * (lam_1d(27, n) + lam_1d(28, n));
    const double hi = 0.5 * (lam_1d(30, n) + lam_1d(31, n));

    auto r6 = eig::eigs_sym_feast<crd::f64>(a, lo, hi, /*m0=*/6, opts, &alloc);
    auto r10 = eig::eigs_sym_feast<crd::f64>(a, lo, hi, /*m0=*/10, opts, &alloc); // bigger headroom

    REQUIRE(r6.values.size() == 3);
    REQUIRE(r10.values.size() == 3); // same count regardless of the over-estimate
    REQUIRE(r6.converged);
    REQUIRE(r10.converged);
    double g6[10];
    double g10[10];
    sorted_vals(r6, g6);
    sorted_vals(r10, g10);
    for (crd::u32 s = 0; s < 3; ++s)
    {
        CHECK(std::fabs(g6[s] - g10[s]) < 1e-9); // m0 does not change the recovered eigenvalues
    }
}

TEST_CASE("v6-g FEAST determinism moat {1,2,4,8} (complex factors across worker counts)",
          "[hesap][eigen][v6][moat]")
{
    // A 2D grid (NOT a 1D chain) so the complex multifrontal-LU factor has a genuinely parallel elimination tree
    // AND the FORCED-parallel SELL spmv (num_workers>1) runs the row-balanced reduction — both bit-exact, so the
    // moat is non-vacuous (a 1D chain factors serially regardless of worker count).
    const crd::u32 mx = 16;
    const crd::u32 my = 20; // rectangular ⇒ non-degenerate smallest modes
    crd::memory::TlsfAllocator alloc(1U << 26);
    Csr a = laplacian_2d(&alloc, mx, my);

    eig::EigenOptions<crd::f64> opts;
    opts.tol = 1e-9;
    opts.max_restarts = 30;
    double s5[5];
    smallest_2d(mx, my, 5, s5);
    const double lo = -0.01;
    const double hi = 0.5 * (s5[3] + s5[4]); // smallest 4 of the 2D grid

    crd::containers::Array<crd::f64> val_ref(&alloc);
    crd::containers::Array<crd::f64> vec_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            // num_workers drives the complex multifrontal-LU factor build (the parallel, moat-critical step).
            auto r = eig::eigs_sym_feast<crd::f64>(a, lo, hi, /*m0=*/8, opts, &alloc, nw);
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
