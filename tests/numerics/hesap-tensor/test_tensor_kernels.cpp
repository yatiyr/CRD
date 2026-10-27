// The hesap-tensor kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the scalar oracle's
// bits for the f32 permute regions (unit and other source strides, edge strips, negative and broadcast strides, the
// scaled and non-temporal variants, NaN payloads, nothing written outside the region) and the portable level's bits
// for the Tier R binned sum (around the vector threshold and the endurance window, the speculative widening, the
// near-DBL_MAX compression regime, non-finite inputs, a non-empty accumulator). crd-hesap-tensor-kernels-at-<level>
// runs these [kernels] cases under each forced level.

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/tensor/reduce.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

namespace tn = crd::hesap::tensor;
using crd::CpuLevel;
using crd::f32;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::KernelLevel;
using tn::PermuteRegionF32;
using tn::Tensor;
using tn::TensorKernels;
using tn::detail::BinnedRungs;

namespace
{

constexpr u32 kSentinel = 0x7FA5A5A5U; // a quiet NaN no region writes

bool runs_here(const KernelLevel<TensorKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

u64 lcg(u64& s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 11U;
}

// f32 values with varied signs and magnitudes, every 13th a NaN with its own payload.
void fill_f32(f32* p, u64 n, u64 seed)
{
    u64 s = seed;
    for (u64 i = 0; i < n; ++i)
    {
        const u64 r = lcg(s);
        const u32 bits = (i % 13U) == 5U ? 0x7F800001U + static_cast<u32>(r & 0x3FFFFFU)
                                         : static_cast<u32>(r & 0xBFFFFFFFU); // exponent below the NaN range
        p[i] = std::bit_cast<f32>(bits);
    }
}

struct RegionCase
{
    u64 na;
    u64 nb;
    i64 sa;
    i64 sb;
    u64 tile_a;
    u64 tile_b;
};

// The scalar oracle: dst(ia, ib) = src(ia, ib) or alpha * src(ia, ib), one element at a time.
void region_reference(const PermuteRegionF32& r)
{
    for (u64 ia = 0; ia < r.na; ++ia)
    {
        for (u64 ib = 0; ib < r.nb; ++ib)
        {
            const f32 v = r.src[static_cast<i64>(ia) * r.sa + static_cast<i64>(ib) * r.sb];
            r.dst[static_cast<i64>(ia) * r.ra + static_cast<i64>(ib)] = r.scale ? r.alpha * v : v;
        }
    }
}

// The binned accumulator of p[0..n) at one level.
BinnedRungs binned_at(const TensorKernels& k, const f64* p, u64 n, f64 seed_value)
{
    BinnedRungs acc{};
    if (seed_value != 0.0) // a non-empty accumulator: the run merges into it
    {
        k.binned_sum_f64(acc, &seed_value, 1U);
    }
    k.binned_sum_f64(acc, p, n);
    return acc;
}

// Every rung's bits: a renormalised finite accumulator is one representation of its value, whatever the order of
// the deposits, so the levels must agree on it, not only on the rounded read-out.
bool same_rungs(const BinnedRungs& a, const BinnedRungs& b)
{
    bool same = true;
    for (crd::i32 r = 0; r < tn::detail::kBinFold; ++r)
    {
        same = same && std::bit_cast<u64>(a.pri[r]) == std::bit_cast<u64>(b.pri[r]) &&
               std::bit_cast<u64>(a.car[r]) == std::bit_cast<u64>(b.car[r]);
    }
    return same;
}

} // namespace

TEST_CASE("tensor kernels: the archive holds portable and each body level the build compiles", "[tensor][kernels]")
{
    const auto levels = tn::tensor_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool avx2 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar f32 permute region bits", "[tensor][kernels]")
{
    crd::memory::TlsfAllocator alloc(1U << 26U);
    // Unit source strides with extents on and off the 8x8 grid (both edge strips), tiles that cut the grid, a
    // region smaller than one microkernel, non-unit, negative and broadcast strides.
    const RegionCase cases[] = {
        {64U, 64U, 1, 64, 64U, 64U},    {67U, 61U, 1, 70, 64U, 64U},  {130U, 75U, 1, 131, 32U, 32U},
        {512U, 40U, 1, 512, 512U, 32U}, {7U, 5U, 1, 7, 64U, 64U},     {9U, 17U, 1, 9, 5U, 3U},
        {40U, 24U, 3, 121, 64U, 64U},   {33U, 16U, 1, -40, 64U, 64U}, {16U, 40U, 1, 0, 64U, 64U},
        {24U, 24U, -1, 30, 64U, 64U},   {8U, 8U, 1, 8, 8U, 8U},       {1U, 1U, 1, 1, 64U, 64U},
    };
    const f32 alphas[] = {1.0F, -0.5F, 3.0F};
    const u64 src_n = 1U << 20U;
    const u64 shape_src[] = {src_n};
    Tensor<f32> src(&alloc, shape_src);
    fill_f32(src.data(), src_n, 0x5EEDU);
    const u64 dst_n = 1U << 18U;
    const u64 shape_dst[] = {dst_n};
    Tensor<f32> want(&alloc, shape_dst);
    Tensor<f32> got(&alloc, shape_dst);
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        for (const RegionCase& c : cases)
        {
            // dst rows padded past nb (to a multiple of 8 for the non-temporal variant), so writes outside the
            // region land on sentinel words.
            const i64 ra = static_cast<i64>(((c.nb + 9U) + 7U) & ~u64{7});
            // A base inside src such that every negative stride stays in the buffer.
            const i64 lo = (c.sa < 0 ? (static_cast<i64>(c.na) - 1) * -c.sa : 0) +
                           (c.sb < 0 ? (static_cast<i64>(c.nb) - 1) * -c.sb : 0);
            const f32* base = src.data() + lo + 3; // 3: off the 32-byte grid
            REQUIRE(static_cast<u64>(lo + 3 + (static_cast<i64>(c.na) * (c.sa < 0 ? 0 : c.sa)) +
                                     static_cast<i64>(c.nb) * (c.sb < 0 ? 0 : c.sb) + 8) < src_n);
            REQUIRE(static_cast<u64>(ra) * c.na <= dst_n);
            for (const f32 alpha : alphas)
            {
                for (const bool stream : {false, true})
                {
                    for (u64 i = 0; i < dst_n; ++i)
                    {
                        want.data()[i] = std::bit_cast<f32>(kSentinel);
                        got.data()[i] = std::bit_cast<f32>(kSentinel);
                    }
                    if (stream && c.tile_b % 8U != 0U)
                    {
                        continue; // the non-temporal variant needs 32-byte aligned tile rows
                    }
                    const bool scale = alpha != 1.0F;
                    region_reference(
                        {want.data(), ra, base, c.sa, c.sb, c.na, c.nb, c.tile_a, c.tile_b, alpha, scale, stream});
                    level.table->permute_region_f32(
                        {got.data(), ra, base, c.sa, c.sb, c.na, c.nb, c.tile_a, c.tile_b, alpha, scale, stream});
                    u64 mismatches = 0;
                    for (u64 i = 0; i < dst_n; ++i)
                    {
                        mismatches += std::bit_cast<u32>(want.data()[i]) != std::bit_cast<u32>(got.data()[i]) ? 1U : 0U;
                    }
                    INFO(crd::cpu_level_name(level.level)
                         << " na " << c.na << " nb " << c.nb << " sa " << c.sa << " sb " << c.sb << " tiles "
                         << c.tile_a << "x" << c.tile_b << " alpha " << alpha << " stream " << stream);
                    CHECK(mismatches == 0U);
                }
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the portable binned sum bits", "[tensor][kernels]")
{
    crd::memory::TlsfAllocator alloc(1U << 24U);
    constexpr u64 window = 12U * tn::detail::kBinEndurance; // the AVX2 body's super-block
    const u64 n_max = 3U * window + 77U;
    const u64 shape[] = {n_max};
    Tensor<f64> x(&alloc, shape);
    u64 s = 0xC0FFEEU;
    // Heavy cancellation over many magnitudes (the ReproBLAS sum_sine workload class).
    for (u64 i = 0; i < n_max; ++i)
    {
        const f64 mag = 1.0 + 1e6 * static_cast<f64>(lcg(s) % 1000U) / 1000.0;
        x.data()[i] = ((i & 1U) != 0U ? -1.0 : 1.0) * mag * (static_cast<f64>(lcg(s) % 4096U) - 2048.0);
    }
    const u64 lengths[] = {0U, 1U, 11U, 95U, 96U, 97U, 107U, window - 1U, window, window + 13U, n_max};
    const KernelLevel<TensorKernels>& portable = tn::tensor_kernel_levels()[0];
    REQUIRE(portable.level == CpuLevel::Portable);

    // A late magnitude far above the first window's: the speculative pass rolls back and widens. A value near
    // DBL_MAX in a later window moves the accumulator to index 0 (the compression regime the scalar deposits handle).
    // An infinity is exact; NaNs with different payloads read out as the one quiet NaN. Powers of two from 2^30 down
    // to 2^-58 land exactly halfway between two values of some rung: the deposit's sticky bit decides those ties.
    Tensor<f64> ties(&alloc, shape);
    Tensor<f64> late(&alloc, shape);
    Tensor<f64> huge(&alloc, shape);
    Tensor<f64> inf(&alloc, shape);
    Tensor<f64> nan(&alloc, shape);
    for (u64 i = 0; i < n_max; ++i)
    {
        late.data()[i] = x.data()[i];
        huge.data()[i] = x.data()[i];
        inf.data()[i] = x.data()[i];
        nan.data()[i] = x.data()[i];
        const f64 power = std::bit_cast<f64>(static_cast<u64>(1023 + 30 - static_cast<i64>(i % 89U)) << 52U);
        const f64 signed_power = (i & 8U) != 0U ? -power : power;
        ties.data()[i] = (i % 7U) == 3U ? signed_power : x.data()[i];
    }
    late.data()[window + 5U] = 3.5e200;
    late.data()[2U * window + 1U] = -3.5e200;
    huge.data()[window + 7U] = 1.6e308;
    huge.data()[2U * window + 9U] = -1.5e308;
    inf.data()[window + 3U] = std::bit_cast<f64>(0x7FF0000000000000ULL);
    nan.data()[50U] = std::bit_cast<f64>(0x7FF8000000000000ULL);
    nan.data()[window + 3U] = std::bit_cast<f64>(0xFFF4000000000123ULL);
    const Tensor<f64>* inputs[] = {&x, &ties, &late, &huge, &inf, &nan};
    const f64 seeds[] = {0.0, -7.25e5};

    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        for (usize which = 0; which < 6U; ++which)
        {
            for (const u64 n : lengths)
            {
                for (const f64 seed : seeds)
                {
                    const f64* p = inputs[which]->data();
                    const BinnedRungs want_rungs = binned_at(*portable.table, p, n, seed);
                    const BinnedRungs got_rungs = binned_at(*level.table, p, n, seed);
                    const u64 want = std::bit_cast<u64>(tn::detail::binned_convert(want_rungs));
                    const u64 got = std::bit_cast<u64>(tn::detail::binned_convert(got_rungs));
                    INFO(crd::cpu_level_name(level.level) << " input " << which << " n " << n << " seed " << seed);
                    CHECK(got == want);
                    if (which < 4U) // finite inputs
                    {
                        CHECK(same_rungs(got_rungs, want_rungs));
                    }
                    if (which == 5U && n > 50U)
                    {
                        CHECK(got == 0x7FF8000000000000ULL);
                    }
                }
            }
        }
        // Partition independence across levels: this level's chunks merged equal the portable whole-run bits.
        for (const u32 chunks : {2U, 5U, 9U})
        {
            BinnedRungs merged{};
            u64 begin = 0;
            for (u32 c = 0; c < chunks; ++c)
            {
                const u64 end = (static_cast<u64>(c + 1U) * n_max) / chunks;
                BinnedRungs part{};
                level.table->binned_sum_f64(part, x.data() + begin, end - begin);
                tn::detail::binned_add(merged, part);
                begin = end;
            }
            INFO(crd::cpu_level_name(level.level) << " chunks " << chunks);
            CHECK(same_rungs(merged, binned_at(*portable.table, x.data(), n_max, 0.0)));
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: the process runs the level its CPU selection resolves", "[tensor][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = tn::tensor_kernel_levels();
    const KernelLevel<TensorKernels>& chosen = tn::tensor_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<TensorKernels>(levels, s.selected, crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
