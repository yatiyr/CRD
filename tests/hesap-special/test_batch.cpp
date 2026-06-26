// crd-hesap-special v12-a — parallel batch determinism moat. The batch result must be BIT-IDENTICAL to the scalar
// loop AND identical across {1,4,16} thread counts (disjoint-range write, no reduction ⇒ true by construction).

#include <crd/hesap/special/special.hpp>

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <thread>

namespace sf = crd::hesap::special;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;

namespace
{
usize fill(cont::Array<f64>& a, usize n, f64 lo, f64 hi)
{
    a.resize(n);
    crd::u64 s = 0xC0FFEEULL;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
        a[i] = lo + u * (hi - lo);
    }
    return n;
}
} // namespace

TEST_CASE("jobs policy: recommended_jobs sane + default unchanged (ADR-0094)", "[v12-a][jobs][policy]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 8U;
    crd::jobs::init(cfg);
    {
        const crd::u32 nw = crd::jobs::num_workers();
        REQUIRE(nw == 8U);
        // Default = one job per worker, clamped to the item count.
        CHECK(crd::jobs::recommended_jobs(crd::jobs::WorkerPreference::Default, 1000000U) == nw);
        CHECK(crd::jobs::recommended_jobs(crd::jobs::WorkerPreference::Default, 3U) == 3U);
        // MemoryBoundElementwise is a SAFE no-op without the env override / worker affinity (no auto-reduction =
        // no E-core-oversubscription regression). It must never exceed the pool.
        const crd::u32 mb = crd::jobs::recommended_jobs(crd::jobs::WorkerPreference::MemoryBoundElementwise, 1000000U);
        CHECK(mb == nw); // default: no reduction (affinity pending) ⇒ identical to Default
        CHECK(mb <= nw);
        // Topology detection returns a count (may be 0 = unknown, e.g. WSL); never exceeds the logical count.
        const crd::u32 pc = crd::jobs::performance_core_count();
        CHECK(pc <= static_cast<crd::u32>(std::thread::hardware_concurrency()));
    }
    crd::jobs::shutdown();
}

TEST_CASE("special batch: parallel == scalar + {1,4,16}-thread bit-identity (moat)", "[v12-a][special][batch][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const usize n = 200000; // > kBatchParallelThreshold ⇒ engages the parallel path at nw>1
    cont::Array<f64> in(&alloc);
    fill(in, n, 0.1, 20.0); // valid for erf / lgamma(>0) / gammainc_p(≥0)

    // Scalar references (the ground truth the batch must reproduce byte-for-byte).
    cont::Array<f64> ref_erf(&alloc);
    cont::Array<f64> ref_lg(&alloc);
    cont::Array<f64> ref_gp(&alloc);
    cont::Array<f64> ref_tg(&alloc);
    cont::Array<f64> ref_bj(&alloc);
    cont::Array<f64> ref_aa(&alloc);
    ref_erf.resize(n);
    ref_lg.resize(n);
    ref_gp.resize(n);
    ref_tg.resize(n);
    ref_bj.resize(n);
    ref_aa.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        ref_erf[i] = sf::erf(in[i]);
        ref_lg[i] = sf::detail::crd_lgamma_lz1(in[i]); // the batch uses the SIMD Lanczos twins
        ref_gp[i] = sf::gammainc_p(2.5, in[i]);
        ref_tg[i] = sf::detail::crd_tgamma_lz1(in[i]);
        ref_bj[i] = sf::cyl_bessel_j(2.5, in[i]);
        ref_aa[i] = sf::airy_ai(in[i]);
    }
    // The Lanczos lgamma/tgamma twins are accurate vs the public std::-based sf::lgamma/sf::gamma (≤ a few ulp).
    for (usize i = 0; i < n; ++i)
    {
        REQUIRE(std::abs(sf::detail::crd_lgamma_lz1(in[i]) - sf::lgamma(in[i])) <=
                1e-12 + 1e-12 * std::abs(sf::lgamma(in[i])));
        REQUIRE(std::abs(sf::detail::crd_tgamma_lz1(in[i]) - sf::gamma(in[i])) <= 1e-11 * std::abs(sf::gamma(in[i])));
    }

    cont::Array<f64> out(&alloc);
    out.resize(n);
    for (crd::u32 nw : {1U, 4U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sf::erf_batch<f64>(out.data(), in.data(), n);
            INFO("erf_batch threads " << nw);
            CHECK(std::memcmp(out.data(), ref_erf.data(), n * sizeof(f64)) == 0);

            sf::lgamma_batch<f64>(out.data(), in.data(), n);
            INFO("lgamma_batch threads " << nw);
            CHECK(std::memcmp(out.data(), ref_lg.data(), n * sizeof(f64)) == 0);

            sf::gammainc_p_batch<f64>(out.data(), in.data(), n, 2.5);
            INFO("gammainc_p_batch threads " << nw);
            CHECK(std::memcmp(out.data(), ref_gp.data(), n * sizeof(f64)) == 0);

            sf::gamma_batch<f64>(out.data(), in.data(), n);
            INFO("gamma_batch threads " << nw);
            CHECK(std::memcmp(out.data(), ref_tg.data(), n * sizeof(f64)) == 0);

            sf::cyl_bessel_j_batch<f64>(out.data(), in.data(), n, 2.5);
            INFO("cyl_bessel_j_batch threads " << nw);
            CHECK(std::memcmp(out.data(), ref_bj.data(), n * sizeof(f64)) == 0);

            sf::airy_ai_batch<f64>(out.data(), in.data(), n);
            INFO("airy_ai_batch threads " << nw);
            CHECK(std::memcmp(out.data(), ref_aa.data(), n * sizeof(f64)) == 0);
        }
        crd::jobs::shutdown();
    }

    // ADR-0094 opt-in P-core routing path: targeted-wake + parallel_for_pcores. Must produce byte-identical results
    // to the scalar refs (the moat) and must not deadlock (the timeout backstop guarantees progress). Forced to a
    // reduced worker subset via the env knob so the routing path is exercised even where topology is hidden (WSL).
#if defined(_WIN32)
    _putenv_s("CRD_JOBS_MEMBOUND_WORKERS", "4");
#else
    setenv("CRD_JOBS_MEMBOUND_WORKERS", "4", 1);
#endif
    for (crd::u32 nw : {4U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        cfg.pcore_routing = true; // ← the new path
        crd::jobs::init(cfg);
        {
            REQUIRE(crd::jobs::is_pcore_routing());
            sf::erf_batch<f64>(out.data(), in.data(), n);
            INFO("erf_batch pcore_routing threads " << nw);
            CHECK(std::memcmp(out.data(), ref_erf.data(), n * sizeof(f64)) == 0);

            sf::lgamma_batch<f64>(out.data(), in.data(), n);
            INFO("lgamma_batch pcore_routing threads " << nw);
            CHECK(std::memcmp(out.data(), ref_lg.data(), n * sizeof(f64)) == 0);

            sf::gamma_batch<f64>(out.data(), in.data(), n);
            INFO("gamma_batch pcore_routing threads " << nw);
            CHECK(std::memcmp(out.data(), ref_tg.data(), n * sizeof(f64)) == 0);

            sf::gammainc_p_batch<f64>(out.data(), in.data(), n, 2.5); // Default (all-cores) on the routing pool
            INFO("gammainc_p_batch pcore_routing threads " << nw);
            CHECK(std::memcmp(out.data(), ref_gp.data(), n * sizeof(f64)) == 0);
        }
        crd::jobs::shutdown();
    }
}
