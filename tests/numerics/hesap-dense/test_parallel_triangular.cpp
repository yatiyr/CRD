// crd-hesap-dense v7-e-2 — balanced-triangular parallel partition primitive.

#include "hesap_jobs_fixture.hpp" // shared HesapJobsListener — inits jobs ONCE for the binary (double-init crashes)

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/detail/parallel_triangular.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace dtl = crd::hesap::dense::detail;

TEST_CASE("triangular_bound: endpoints + monotonic + full coverage", "[hesap][dense][v7-e-2][triangular]")
{
    for (crd::u32 n : {0U, 1U, 7U, 64U, 5385U, 100000U})
    {
        for (crd::u32 w : {1U, 2U, 4U, 8U, 16U})
        {
            REQUIRE(dtl::triangular_bound(n, 0, w) == 0U);     // first boundary
            REQUIRE(dtl::triangular_bound(n, w, w) == n);      // last boundary covers all of [0,n)
            crd::u32 prev = 0;
            for (crd::u32 k = 1; k <= w; ++k)
            {
                const crd::u32 b = dtl::triangular_bound(n, k, w);
                REQUIRE(b >= prev); // non-decreasing ⇒ ranges [b_k,b_{k+1}) tile [0,n) with no gaps/overlaps
                REQUIRE(b <= n);
                prev = b;
            }
        }
    }
}

TEST_CASE("triangular_bound: balances triangular work (vs the ~Wx row-slab imbalance)",
          "[hesap][dense][v7-e-2][triangular]")
{
    // Work for row r ∝ (r+1) (lower triangle touches cols [0,r]); part k's work ≈ Σ_{r∈[r_k,r_{k+1})}(r+1).
    const crd::u32 n = 5385; // a lattice huge-front nc
    const crd::u32 w = 8;
    double maxwork = 0.0;
    double minwork = 1e300;
    for (crd::u32 k = 0; k < w; ++k)
    {
        const crd::u32 r0 = dtl::triangular_bound(n, k, w);
        const crd::u32 r1 = dtl::triangular_bound(n, k + 1, w);
        double work = 0.0;
        for (crd::u32 r = r0; r < r1; ++r)
        {
            work += static_cast<double>(r) + 1.0;
        }
        maxwork = work > maxwork ? work : maxwork;
        minwork = work < minwork ? work : minwork;
    }
    // Balanced: max/min part work well under the ~8× a row-slab partition would give (last vs first).
    REQUIRE(maxwork / minwork < 1.30);
}

TEST_CASE("parallel_for_triangular: covers every row exactly once across workers",
          "[hesap][dense][v7-e-2][triangular]")
{
    crd_hesap_dense_tests::hesap_jobs_listener(); // shared init (no per-test init/shutdown — double-init crashes)
    for (crd::u32 n : {1U, 9U, 64U, 4096U})
    {
        crd::containers::Array<crd::u32> hit(crd::memory::default_allocator());
        hit.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            hit[i] = 0;
        }
        const crd::u32 w = crd::jobs::num_workers();
        dtl::parallel_for_triangular(n, w,
                                     [&](crd::u32 r0, crd::u32 r1, crd::u32 /*worker*/)
                                     {
                                         for (crd::u32 r = r0; r < r1; ++r)
                                         {
                                             ++hit[r]; // disjoint rows ⇒ no race
                                         }
                                     });
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(hit[i] == 1U); // each row processed exactly once (no gaps, no overlaps)
        }
    }
}
