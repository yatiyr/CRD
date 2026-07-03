// v14-d permute_copy gates (ADR-0096 §5, HPTT-class transpose/permute,
// increment 1: serial + SIMD-blocked).
//
// Two oracles: (1) the trivially-correct reference walk — src.permute(order)
// visited element-by-element via TensorView::for_each, compared bit-exact
// against dst's contiguous storage — over an adversarial shape corpus, and
// (2) the NumPy corpus (scripts/v14d_permute_corpus.py -> ref_permute.inc):
// np.transpose(...).copy() f64 bits for three representative cases, gated
// bit-exact (reconstruct-verify-first, plain C arrays — no std containers).

#include "ref_permute.inc"

#include <crd/hesap/tensor/permute.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>

using crd::hesap::tensor::permute_copy;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

// Deterministic LCG fill — varied mantissa/sign patterns, reproducible.
template <typename T> void fill_pattern(Tensor<T>& t)
{
    crd::u64 s = 0x9E3779B97F4A7C15ULL;
    for (crd::u64 i = 0; i < t.size(); ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        t.data()[i] = static_cast<T>(static_cast<crd::i64>(s >> 20U)) / static_cast<T>(1U << 16U);
    }
}

// Gate permute_copy against the trivially-correct reference: walk
// src.permute(order) in row-major order via for_each and compare every
// element bit-exact against dst's contiguous storage.
template <typename T>
void check_permute_case(crd::memory::IAllocator* alloc, TensorView<const T> src,
                        crd::containers::ConstSpan<crd::u32> order, T alpha = T{1})
{
    Tensor<T> dst(alloc);
    REQUIRE(permute_copy(src, order, dst, alpha) == TensorStatus::Ok);

    const TensorView<const T> ref = src.permute(order);
    REQUIRE(dst.rank() == ref.rank());
    for (crd::u32 d = 0; d < dst.rank(); ++d)
    {
        CHECK(dst.shape(d) == ref.shape(d));
    }
    REQUIRE(dst.size() == ref.size());

    crd::u64 k = 0;
    crd::u64 mismatches = 0;
    ref.for_each(
        [&](const crd::u64* /*idx*/, const T& v)
        {
            const T expect = (alpha == T{1}) ? v : alpha * v;
            if (std::memcmp(&dst.data()[k], &expect, sizeof(T)) != 0)
            {
                ++mismatches;
            }
            ++k;
        });
    CHECK(k == dst.size());
    CHECK(mismatches == 0U);
}

// Construct a Tensor<f64> from baked u64 bit patterns (NumPy corpus sources).
crd::f64* from_bits(Tensor<crd::f64>& t, const crd::u64* bits)
{
    std::memcpy(t.data(), bits, t.size() * sizeof(crd::f64));
    return t.data();
}

// Compare dst's f64 storage bit-exact against a baked u64 array.
void check_bits(const Tensor<crd::f64>& dst, const crd::u64* expect, crd::u64 count)
{
    REQUIRE(dst.size() == count);
    for (crd::u64 k = 0; k < count; ++k)
    {
        crd::u64 got = 0;
        std::memcpy(&got, &dst.data()[k], sizeof(got));
        CHECK(got == expect[k]);
    }
}

template <typename T> void run_2d_transpose_corpus(crd::memory::IAllocator* alloc)
{
    // Square / tall / wide / odd / tile-edge / micro-edge sizes: full 32x32
    // tiles, full 8x8 microkernel grids, partial strips, and the tiny path.
    constexpr crd::u64 sizes[][2] = {{1U, 1U},   {1U, 7U},   {7U, 13U},   {8U, 8U},    {13U, 7U},  {32U, 32U},
                                     {33U, 31U}, {64U, 64U}, {65U, 129U}, {129U, 65U}, {257U, 96U}};
    const crd::u32 transpose[] = {1U, 0U};
    const crd::u32 identity[] = {0U, 1U};
    for (const auto& sz : sizes)
    {
        Tensor<T> t(alloc, sz);
        fill_pattern(t);
        check_permute_case<T>(alloc, t.view(), transpose);
        check_permute_case<T>(alloc, t.view(), identity); // collapse -> single memcpy
    }
}

} // namespace

TEST_CASE("v14-d permute_copy: 2-D transposes bit-exact vs the reference walk", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    run_2d_transpose_corpus<crd::f32>(&alloc);
    run_2d_transpose_corpus<crd::f64>(&alloc);
}

TEST_CASE("v14-d permute_copy: every order of a 5x6x7x8 and of a 4x5x6", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    {
        const crd::u64 shape[] = {5U, 6U, 7U, 8U};
        Tensor<crd::f32> tf(&alloc, shape);
        Tensor<crd::f64> td(&alloc, shape);
        fill_pattern(tf);
        fill_pattern(td);
        crd::u32 perm[] = {0U, 1U, 2U, 3U};
        do
        {
            check_permute_case<crd::f32>(&alloc, tf.view(), perm);
            check_permute_case<crd::f64>(&alloc, td.view(), perm);
        } while (std::next_permutation(perm, perm + 4));
    }
    {
        const crd::u64 shape[] = {4U, 5U, 6U};
        Tensor<crd::f64> t(&alloc, shape);
        fill_pattern(t);
        crd::u32 perm[] = {0U, 1U, 2U};
        do
        {
            check_permute_case<crd::f64>(&alloc, t.view(), perm);
        } while (std::next_permutation(perm, perm + 3));
    }
}

TEST_CASE("v14-d permute_copy: rank-1 identity and rank-0 scalar", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);

    {
        const crd::u64 shape[] = {17U};
        Tensor<crd::f64> t(&alloc, shape);
        fill_pattern(t);
        const crd::u32 order[] = {0U};
        check_permute_case<crd::f64>(&alloc, t.view(), order);
    }
    {
        Tensor<crd::f64> t(&alloc, crd::containers::ConstSpan<crd::u64>{});
        t.data()[0] = 42.5;
        Tensor<crd::f64> dst(&alloc);
        REQUIRE(permute_copy<crd::f64>(t.view(), crd::containers::ConstSpan<crd::u32>{}, dst) == TensorStatus::Ok);
        REQUIRE(dst.rank() == 0U);
        REQUIRE(dst.size() == 1U);
        CHECK(dst.data()[0] == 42.5);
    }
}

TEST_CASE("v14-d permute_copy: dims of size 1 collapse away", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);

    {
        const crd::u64 shape[] = {1U, 5U, 1U, 3U};
        Tensor<crd::f64> t(&alloc, shape);
        fill_pattern(t);
        const crd::u32 o1[] = {2U, 0U, 3U, 1U};
        const crd::u32 o2[] = {3U, 2U, 1U, 0U};
        const crd::u32 o3[] = {1U, 3U, 0U, 2U};
        check_permute_case<crd::f64>(&alloc, t.view(), o1);
        check_permute_case<crd::f64>(&alloc, t.view(), o2);
        check_permute_case<crd::f64>(&alloc, t.view(), o3);
    }
    {
        const crd::u64 shape[] = {1U, 1U, 1U};
        Tensor<crd::f32> t(&alloc, shape);
        t.data()[0] = -3.25F;
        const crd::u32 order[] = {1U, 2U, 0U};
        check_permute_case<crd::f32>(&alloc, t.view(), order);
    }
}

TEST_CASE("v14-d permute_copy: zero-size tensors", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);

    {
        const crd::u64 shape[] = {0U, 4U};
        Tensor<crd::f32> t(&alloc, shape);
        Tensor<crd::f32> dst(&alloc);
        const crd::u32 order[] = {1U, 0U};
        REQUIRE(permute_copy<crd::f32>(t.view(), order, dst) == TensorStatus::Ok);
        REQUIRE(dst.rank() == 2U);
        CHECK(dst.shape(0U) == 4U);
        CHECK(dst.shape(1U) == 0U);
        CHECK(dst.size() == 0U);
    }
    {
        const crd::u64 shape[] = {3U, 0U, 5U};
        Tensor<crd::f64> t(&alloc, shape);
        Tensor<crd::f64> dst(&alloc);
        const crd::u32 order[] = {2U, 1U, 0U};
        REQUIRE(permute_copy<crd::f64>(t.view(), order, dst) == TensorStatus::Ok);
        CHECK(dst.shape(0U) == 5U);
        CHECK(dst.shape(1U) == 0U);
        CHECK(dst.shape(2U) == 3U);
        CHECK(dst.size() == 0U);
    }
}

TEST_CASE("v14-d permute_copy: sliced, flipped and non-collapsible sources", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    // Permuted SLICED view as source (non-contiguous rows block the collapse).
    {
        const crd::u64 shape[] = {6U, 7U, 8U};
        Tensor<crd::f64> t(&alloc, shape);
        fill_pattern(t);
        const TensorView<crd::f64> sliced = t.view().slice(0U, 0U, 6U, 2U).slice(1U, 1U, 6U); // (3,5,8)
        const crd::u32 o1[] = {2U, 1U, 0U};
        const crd::u32 o2[] = {1U, 2U, 0U};
        const crd::u32 o3[] = {0U, 1U, 2U};
        check_permute_case<crd::f64>(&alloc, sliced, o1);
        check_permute_case<crd::f64>(&alloc, sliced, o2);
        check_permute_case<crd::f64>(&alloc, sliced, o3);

        // Flipped (negative stride) source.
        const TensorView<crd::f64> flipped = t.view().flip(2U).flip(0U);
        check_permute_case<crd::f64>(&alloc, flipped, o1);
        check_permute_case<crd::f64>(&alloc, flipped, o3);
    }

    // Non-collapsible stride pattern: neither dim has unit stride.
    {
        const crd::u64 shape[] = {40U, 60U};
        Tensor<crd::f32> t(&alloc, shape);
        fill_pattern(t);
        const TensorView<crd::f32> strided = t.view().slice(0U, 0U, 40U, 2U).slice(1U, 1U, 60U, 3U); // (20,20)
        const crd::u32 transpose[] = {1U, 0U};
        const crd::u32 identity[] = {0U, 1U};
        check_permute_case<crd::f32>(&alloc, strided, transpose);
        check_permute_case<crd::f32>(&alloc, strided, identity);
    }

    // Broadcast (stride-0) source materializes the repeats.
    {
        const crd::u64 shape[] = {5U, 1U, 7U};
        Tensor<crd::f64> t(&alloc, shape);
        fill_pattern(t);
        const crd::u64 target[] = {5U, 6U, 7U};
        TensorView<crd::f64> bcast;
        REQUIRE(t.view().broadcast_to(target, bcast) == TensorStatus::Ok);
        const crd::u32 o1[] = {1U, 2U, 0U};
        const crd::u32 o2[] = {2U, 0U, 1U};
        check_permute_case<crd::f64>(&alloc, bcast, o1);
        check_permute_case<crd::f64>(&alloc, bcast, o2);
    }
}

TEST_CASE("v14-d permute_copy: fused alpha scale", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    {
        const crd::u64 shape[] = {129U, 65U};
        Tensor<crd::f64> t(&alloc, shape);
        fill_pattern(t);
        const crd::u32 transpose[] = {1U, 0U};
        check_permute_case<crd::f64>(&alloc, t.view(), transpose, 1.5);
        check_permute_case<crd::f64>(&alloc, t.view(), transpose, -0.375);
    }
    {
        const crd::u64 shape[] = {129U, 65U};
        Tensor<crd::f32> t(&alloc, shape);
        fill_pattern(t);
        const crd::u32 transpose[] = {1U, 0U};
        check_permute_case<crd::f32>(&alloc, t.view(), transpose, 2.0F); // AVX2 micro scale path
        check_permute_case<crd::f32>(&alloc, t.view(), transpose, 0.125F);
    }
    {
        const crd::u64 shape[] = {5U, 6U, 7U, 8U};
        Tensor<crd::f32> t(&alloc, shape);
        fill_pattern(t);
        const crd::u32 order[] = {3U, 1U, 0U, 2U};
        check_permute_case<crd::f32>(&alloc, t.view(), order, 2.0F);
    }
}

TEST_CASE("v14-d permute_copy: run-twice bit-identity", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    const crd::u64 shape[] = {37U, 21U, 18U};
    Tensor<crd::f64> t(&alloc, shape);
    fill_pattern(t);
    const crd::u32 order[] = {2U, 0U, 1U};

    Tensor<crd::f64> a(&alloc);
    Tensor<crd::f64> b(&alloc);
    REQUIRE(permute_copy<crd::f64>(t.view(), order, a, 1.25) == TensorStatus::Ok);
    REQUIRE(permute_copy<crd::f64>(t.view(), order, b, 1.25) == TensorStatus::Ok);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(crd::f64)) == 0);
}

TEST_CASE("v14-d permute_copy: bad orders are BadInput", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);

    const crd::u64 shape[] = {3U, 4U};
    Tensor<crd::f64> t(&alloc, shape);
    fill_pattern(t);
    Tensor<crd::f64> dst(&alloc);

    const crd::u32 wrong_rank[] = {0U};
    const crd::u32 duplicate[] = {0U, 0U};
    const crd::u32 out_of_range[] = {0U, 2U};
    CHECK(permute_copy<crd::f64>(t.view(), wrong_rank, dst) == TensorStatus::BadInput);
    CHECK(permute_copy<crd::f64>(t.view(), duplicate, dst) == TensorStatus::BadInput);
    CHECK(permute_copy<crd::f64>(t.view(), out_of_range, dst) == TensorStatus::BadInput);

    const crd::u32 good[] = {1U, 0U};
    CHECK(permute_copy<crd::f64>(t.view(), good, dst) == TensorStatus::Ok); // still usable after errors
}

TEST_CASE("v14-d permute_copy: NumPy transpose-copy corpus bit-exact", "[hesap][tensor][v14][permute]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);

    // Case A: (7,13) odd-size 2-D transpose, plus the fused alpha = 1.5.
    {
        const crd::u64 shape[] = {7U, 13U};
        Tensor<crd::f64> t(&alloc, shape);
        from_bits(t, kV14dASrc);
        const crd::u32 order[] = {1U, 0U};
        Tensor<crd::f64> dst(&alloc);
        REQUIRE(permute_copy<crd::f64>(t.view(), order, dst) == TensorStatus::Ok);
        check_bits(dst, kV14dAOut, 91U);
        REQUIRE(permute_copy<crd::f64>(t.view(), order, dst, 1.5) == TensorStatus::Ok);
        check_bits(dst, kV14dAOutScaled, 91U);
    }

    // Case B: rank-4 (2,3,4,5) with order (3,0,2,1).
    {
        const crd::u64 shape[] = {2U, 3U, 4U, 5U};
        Tensor<crd::f64> t(&alloc, shape);
        from_bits(t, kV14dBSrc);
        const crd::u32 order[] = {3U, 0U, 2U, 1U};
        Tensor<crd::f64> dst(&alloc);
        REQUIRE(permute_copy<crd::f64>(t.view(), order, dst) == TensorStatus::Ok);
        check_bits(dst, kV14dBOut, 120U);
    }

    // Case C: permuted SLICED view — base (6,7,8), view [0:6:2, 1:6, :], order (2,1,0).
    {
        const crd::u64 shape[] = {6U, 7U, 8U};
        Tensor<crd::f64> t(&alloc, shape);
        from_bits(t, kV14dCBase);
        const TensorView<crd::f64> sliced = t.view().slice(0U, 0U, 6U, 2U).slice(1U, 1U, 6U);
        const crd::u32 order[] = {2U, 1U, 0U};
        Tensor<crd::f64> dst(&alloc);
        REQUIRE(permute_copy<crd::f64>(sliced, order, dst) == TensorStatus::Ok);
        check_bits(dst, kV14dCOut, 120U);
    }
}

// Informal 4096x4096 f32 transpose timing vs the naive per-element loop.
// Hidden by default (run explicitly: crd-hesap-tensor-permute-tests "[permute-timing]").
// The real HPTT peer bench is a later v14-d increment, not this.
TEST_CASE("v14-d permute_copy informal 4096 f32 transpose timing", "[.][hesap][tensor][v14][permute-timing]")
{
    constexpr crd::u64 n = 4096U;
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(400U) << 20U);

    const crd::u64 shape[] = {n, n};
    Tensor<crd::f32> src(&alloc, shape);
    fill_pattern(src);
    Tensor<crd::f32> naive(&alloc, shape);
    Tensor<crd::f32> dst(&alloc);
    const crd::u32 order[] = {1U, 0U};

    using Clock = std::chrono::steady_clock;
    double naive_ms = 1e300;
    double blocked_ms = 1e300;
    for (int rep = 0; rep < 3; ++rep)
    {
        const auto t0 = Clock::now();
        const crd::f32* s = src.data();
        crd::f32* d = naive.data();
        for (crd::u64 i = 0; i < n; ++i)
        {
            for (crd::u64 j = 0; j < n; ++j)
            {
                d[(i * n) + j] = s[(j * n) + i];
            }
        }
        const auto t1 = Clock::now();
        REQUIRE(permute_copy<crd::f32>(src.view(), order, dst) == TensorStatus::Ok);
        const auto t2 = Clock::now();
        const double ms_naive = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double ms_blocked = std::chrono::duration<double, std::milli>(t2 - t1).count();
        naive_ms = ms_naive < naive_ms ? ms_naive : naive_ms;
        blocked_ms = ms_blocked < blocked_ms ? ms_blocked : blocked_ms;
    }

    REQUIRE(dst.size() == naive.size());
    CHECK(std::memcmp(dst.data(), naive.data(), dst.size() * sizeof(crd::f32)) == 0);

    const double gib =
        (2.0 * static_cast<double>(n) * static_cast<double>(n) * sizeof(crd::f32)) / (1024.0 * 1024.0 * 1024.0);
    std::printf("[v14-d timing] 4096x4096 f32 transpose: naive %.2f ms (%.2f GiB/s) | blocked %.2f ms (%.2f GiB/s) "
                "| speedup %.2fx\n",
                naive_ms, gib / (naive_ms / 1000.0), blocked_ms, gib / (blocked_ms / 1000.0), naive_ms / blocked_ms);
    CHECK(blocked_ms > 0.0);
}

TEST_CASE("v14-d MT: {1,2,4,8,16} workers produce bit-identical permutes", "[hesap][tensor][v14][permute][moat]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 28U);
    // Big enough to cross kPermuteMtBytes on both cases; adversarial shapes.
    const crd::u64 s2[] = {2048U, 1536U}; // 12 MB: crosses the MT stream gate (the staged kernel is covered)
    const crd::u32 p2[] = {1U, 0U};
    const crd::u64 s4[] = {24U, 40U, 32U, 48U};
    const crd::u32 p4[] = {3U, 1U, 2U, 0U};

    Tensor<crd::f32> a2(&alloc, s2);
    Tensor<crd::f32> a4(&alloc, s4);
    for (crd::u64 i = 0; i < a2.size(); ++i)
    {
        a2.data()[i] = static_cast<crd::f32>((i * 2654435761ULL) % 65536ULL) * 0.001F;
    }
    for (crd::u64 i = 0; i < a4.size(); ++i)
    {
        a4.data()[i] = static_cast<crd::f32>((i * 40503ULL) % 65536ULL) * 0.001F - 20.0F;
    }

    Tensor<crd::f32> ref2(&alloc);
    Tensor<crd::f32> ref4(&alloc);
    REQUIRE(permute_copy(TensorView<const crd::f32>(a2.view()), p2, ref2) == TensorStatus::Ok); // serial
    REQUIRE(permute_copy(TensorView<const crd::f32>(a4.view()), p4, ref4) == TensorStatus::Ok);

    Tensor<crd::f32> out2(&alloc);
    Tensor<crd::f32> out4(&alloc);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        REQUIRE(permute_copy(TensorView<const crd::f32>(a2.view()), p2, out2) == TensorStatus::Ok);
        REQUIRE(permute_copy(TensorView<const crd::f32>(a4.view()), p4, out4) == TensorStatus::Ok);
        crd::jobs::shutdown();
        INFO("workers " << nw);
        bool same2 = true;
        for (crd::u64 i = 0; i < ref2.size(); ++i)
        {
            same2 = same2 && std::bit_cast<crd::u32>(out2.data()[i]) == std::bit_cast<crd::u32>(ref2.data()[i]);
        }
        bool same4 = true;
        for (crd::u64 i = 0; i < ref4.size(); ++i)
        {
            same4 = same4 && std::bit_cast<crd::u32>(out4.data()[i]) == std::bit_cast<crd::u32>(ref4.data()[i]);
        }
        CHECK(same2);
        CHECK(same4);
    }
}
