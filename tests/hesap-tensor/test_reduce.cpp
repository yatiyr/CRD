// v14-c gates: reductions + the two named reproducibility tiers.
//   Tier D — fixed-order trees: values vs analytic/quasi-exact references AND
//   bit-identity across {1,2,4,8,16} workers (the moat) and serial==parallel.
//   Tier R — the ReproBLAS-transcribed binned sum: bit-identity under forced
//   REPARTITION (chunk counts + merge orders) and element SHUFFLE, exactness
//   on integers, and accuracy >= naive on a cancellation workload.
//   SR accumulation — run-twice bit-identity + grid unbiasedness.

#include <crd/hesap/tensor/reduce.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::tensor;

namespace
{

// Deterministic LCG (test-local; no std RNG) for value generation + shuffles.
struct Lcg
{
    crd::u64 s;
    crd::u64 next() noexcept
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return s >> 11U;
    }
    crd::f64 uniform() noexcept { return static_cast<crd::f64>(next() & 0xFFFFFFFFU) / 4294967296.0; }
};

} // namespace

TEST_CASE("v14-c Tier D: values vs analytic references", "[hesap][tensor][v14][reduce]")
{
    crd::memory::TlsfAllocator alloc(1U << 24U);
    const crd::u64 n = 100000U;
    const crd::u64 shape[] = {n};
    Tensor<crd::f64> t(&alloc, shape);
    for (crd::u64 i = 0; i < n; ++i)
    {
        t.data()[i] = static_cast<crd::f64>(i % 1000U); // integer-valued: sums are exact
    }
    TensorView<const crd::f64> v = t.view();

    // 100 full cycles of 0..999: sum = 100 * 999*1000/2
    CHECK(reduce_sum(v) == 100.0 * 499500.0);
    CHECK(reduce_mean(v) == (100.0 * 499500.0) / static_cast<crd::f64>(n));
    CHECK(reduce_min(v) == 0.0);
    CHECK(reduce_max(v) == 999.0);
    CHECK(reduce_argmin(v) == 0U);   // first tie wins (NumPy semantics)
    CHECK(reduce_argmax(v) == 999U); // first 999 is at index 999

    // prod on a small exact case
    const crd::u64 s8[] = {8U};
    Tensor<crd::f64> p8(&alloc, s8);
    for (crd::u64 i = 0; i < 8U; ++i)
    {
        p8.data()[i] = static_cast<crd::f64>(i + 1U);
    }
    CHECK(reduce_prod(TensorView<const crd::f64>(p8.view())) == 40320.0); // 8!

    // cumsum == manual prefix (exact on integers)
    Tensor<crd::f64> cs(&alloc, s8);
    REQUIRE(reduce_cumsum(TensorView<const crd::f64>(p8.view()), cs.view()) == TensorStatus::Ok);
    crd::f64 acc = 0.0;
    for (crd::u64 i = 0; i < 8U; ++i)
    {
        acc += p8.data()[i];
        CHECK(cs.data()[i] == acc);
    }

    // logsumexp: constant c over n -> log(n) + c
    Tensor<crd::f64> lc(&alloc, s8);
    for (crd::u64 i = 0; i < 8U; ++i)
    {
        lc.data()[i] = 2.5;
    }
    const crd::f64 lse = reduce_logsumexp(TensorView<const crd::f64>(lc.view()));
    CHECK(lse > 2.5 + 2.0794415416798357 - 1e-12);
    CHECK(lse < 2.5 + 2.0794415416798357 + 1e-12);
}

TEST_CASE("v14-c Tier D: the {1,2,4,8,16} moat - serial == parallel, bit-identical",
          "[hesap][tensor][v14][reduce][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 25U);
    const crd::u64 n = 300000U; // 74 blocks
    const crd::u64 shape[] = {n};
    Tensor<crd::f64> t(&alloc, shape);
    Tensor<crd::f32> tf(&alloc, shape);
    Lcg rng{20260702U};
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f64 x = (rng.uniform() - 0.5) * 1e6; // wide-range cancellation-prone
        t.data()[i] = x;
        tf.data()[i] = static_cast<crd::f32>(x);
    }
    const crd::u64 nblocks = (n + detail::kReduceBlock - 1U) / detail::kReduceBlock;
    Tensor<crd::f64> ws(&alloc, {&nblocks, 1U});
    Tensor<crd::f32> wsf(&alloc, {&nblocks, 1U});

    const crd::f64 serial = reduce_sum(TensorView<const crd::f64>(t.view()));
    const crd::f32 serial_f = reduce_sum(TensorView<const crd::f32>(tf.view()));

    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        const crd::f64 par =
            reduce_sum(TensorView<const crd::f64>(t.view()), {ws.data(), static_cast<crd::usize>(nblocks)});
        const crd::f32 par_f =
            reduce_sum(TensorView<const crd::f32>(tf.view()), {wsf.data(), static_cast<crd::usize>(nblocks)});
        crd::jobs::shutdown();
        INFO("workers " << nw);
        CHECK(std::bit_cast<crd::u64>(par) == std::bit_cast<crd::u64>(serial));
        CHECK(std::bit_cast<crd::u32>(par_f) == std::bit_cast<crd::u32>(serial_f));
    }
}

TEST_CASE("v14-c Tier R: partition/shuffle-INDEPENDENT bits + accuracy", "[hesap][tensor][v14][reduce][repro]")
{
    crd::memory::TlsfAllocator alloc(1U << 25U);
    const crd::u64 n = 100000U;
    const crd::u64 shape[] = {n};
    Tensor<crd::f64> t(&alloc, shape);
    // The ReproBLAS sum_sine workload class: heavy cancellation, true sum ~ 0.
    // (2*pi via the deterministic engine constant expression, not std::)
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f64 phase = 6.283185307179586 * (static_cast<crd::f64>(i) / static_cast<crd::f64>(n) - 0.5);
        // cheap deterministic sine-like cancellation series without libm:
        // alternating rational values spanning many magnitudes
        const crd::f64 mag = 1.0 + 1e6 * static_cast<crd::f64>((i * 2654435761U) % 1000U) / 1000.0;
        t.data()[i] = ((i & 1U) != 0U ? -phase : phase) * mag;
    }
    TensorView<const crd::f64> v = t.view();

    const crd::f64 whole = reduce_sum_reproducible(v);
    const crd::u64 whole_bits = std::bit_cast<crd::u64>(whole);

    // 1. Forced REPARTITION: chunk counts {3, 7, 16}, merged forward AND reverse.
    for (crd::u32 chunks : {3U, 7U, 16U})
    {
        BinnedAccumulatorF64 parts[16];
        crd::u64 begin = 0;
        for (crd::u32 c = 0; c < chunks; ++c)
        {
            const crd::u64 end = (static_cast<crd::u64>(c + 1U) * n) / chunks;
            binned_accumulate(parts[c], {t.data() + begin, static_cast<crd::usize>(end - begin)});
            begin = end;
        }
        BinnedAccumulatorF64 fwd;
        for (crd::u32 c = 0; c < chunks; ++c)
        {
            fwd.add(parts[c]);
        }
        BinnedAccumulatorF64 rev;
        for (crd::u32 c = chunks; c-- > 0U;)
        {
            rev.add(parts[c]);
        }
        INFO("chunks " << chunks);
        CHECK(std::bit_cast<crd::u64>(fwd.convert()) == whole_bits);
        CHECK(std::bit_cast<crd::u64>(rev.convert()) == whole_bits);
    }

    // 2. Element SHUFFLE (Fisher-Yates with the test LCG) — identical bits.
    Tensor<crd::f64> sh(&alloc, shape);
    for (crd::u64 i = 0; i < n; ++i)
    {
        sh.data()[i] = t.data()[i];
    }
    Lcg rng{42U};
    for (crd::u64 i = n - 1U; i > 0U; --i)
    {
        const crd::u64 j = rng.next() % (i + 1U);
        const crd::f64 tmp = sh.data()[i];
        sh.data()[i] = sh.data()[j];
        sh.data()[j] = tmp;
    }
    CHECK(std::bit_cast<crd::u64>(reduce_sum_reproducible(TensorView<const crd::f64>(sh.view()))) == whole_bits);
    // ...while the NAIVE sums of the two orderings genuinely differ (the property matters):
    const crd::f64 naive_o = reduce_sum(v);
    const crd::f64 naive_s = reduce_sum(TensorView<const crd::f64>(sh.view()));
    CHECK(std::bit_cast<crd::u64>(naive_o) != std::bit_cast<crd::u64>(naive_s));

    // 3. Exact on integer-valued data.
    Tensor<crd::f64> ints(&alloc, shape);
    for (crd::u64 i = 0; i < n; ++i)
    {
        ints.data()[i] = static_cast<crd::f64>((i * 7919U) % 20011U);
    }
    crd::f64 exact = 0.0;
    for (crd::u64 i = 0; i < n; ++i)
    {
        exact += ints.data()[i]; // integer adds below 2^53: exact in f64
    }
    CHECK(reduce_sum_reproducible(TensorView<const crd::f64>(ints.view())) == exact);

    // 4. Accuracy on cancellation: repro at least as close to the quasi-exact
    //    (long-double Kahan) reference as the naive left-to-right sum.
    long double kahan = 0.0L;
    long double comp = 0.0L;
    for (crd::u64 i = 0; i < n; ++i)
    {
        const long double y = static_cast<long double>(t.data()[i]) - comp;
        const long double s = kahan + y;
        comp = (s - kahan) - y;
        kahan = s;
    }
    const crd::f64 ref = static_cast<crd::f64>(kahan);
    const crd::f64 err_repro = whole > ref ? whole - ref : ref - whole;
    const crd::f64 err_naive = naive_o > ref ? naive_o - ref : ref - naive_o;
    CHECK(err_repro <= err_naive + 1e-30);
}

TEST_CASE("v14-c SR accumulation: reproducible-by-seed + on-grid", "[hesap][tensor][v14][reduce]")
{
    crd::memory::TlsfAllocator alloc(1U << 22U);
    const crd::u64 n = 4096U;
    const crd::u64 shape[] = {n};
    Tensor<crd::f32> t(&alloc, shape);
    Lcg rng{7U};
    for (crd::u64 i = 0; i < n; ++i)
    {
        t.data()[i] = static_cast<crd::f32>(rng.uniform()); // in [0,1): sum ~ n/2
    }
    TensorView<const crd::f32> v = t.view();

    // Run-twice bit-identity; seed-sensitivity.
    const crd::u16 a1 = reduce_sum_sr_bf16(v, 123U);
    const crd::u16 a2 = reduce_sum_sr_bf16(v, 123U);
    const crd::u16 b1 = reduce_sum_sr_bf16(v, 456U);
    CHECK(a1 == a2);
    (void)b1; // different seed MAY differ; no hard claim (both valid SR outcomes)

    // Mean over seeds tracks the f64 sum within a few bf16 ulps at this scale
    // (unbiasedness on the grid; generous statistical window).
    const crd::f64 ref = static_cast<crd::f64>(reduce_sum(v));
    crd::f64 mean = 0.0;
    constexpr crd::u32 num_seeds = 64U;
    for (crd::u32 s = 0; s < num_seeds; ++s)
    {
        mean += static_cast<crd::f64>(crd::math::bf16_bits_to_f32(reduce_sum_sr_bf16(v, 1000U + s)));
    }
    mean /= num_seeds;
    // bf16 ulp near 2048 is 16; allow a few ulps of statistical + drift window.
    CHECK(mean > ref - 64.0);
    CHECK(mean < ref + 64.0);
}
