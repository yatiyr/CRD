// The hesap-tensor kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the scalar oracle's
// bits for the f32 permute regions (unit and other source strides, edge strips, negative and broadcast strides, the
// scaled and non-temporal variants, NaN payloads, nothing written outside the region), the portable level's bits for
// the Tier R binned sum (around the vector threshold and the endurance window, the speculative widening, the
// near-DBL_MAX compression regime, non-finite inputs, a non-empty accumulator), the scalar quantizers' bytes (ties,
// signed zeros, NaN and infinite inputs), the pinned order of the Q8_0 and int8 linears (every tile width, the
// saturating and zero-point extremes, row ranges) and the scalar softmax (vector tails, crd_exp4's scalar lanes,
// non-finite logits). crd-hesap-tensor-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/tensor/dtypes.hpp>
#include <crd/hesap/tensor/reduce.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/math/transcendental.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace tn = crd::hesap::tensor;
using crd::CpuLevel;
using crd::f32;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::KernelLevel;
using tn::BlockQ8Zero;
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

// ---- nn kernels -------------------------------------------------------------

namespace
{

constexpr f32 kQNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr u64 kSlack = 8U; // sentinel words after the last output row: no store past it

// f32 values in [-scale, scale].
void fill_uniform(f32* p, u64 n, u64 seed, f32 scale)
{
    u64 s = seed;
    for (u64 i = 0; i < n; ++i)
    {
        p[i] = scale * (static_cast<f32>(lcg(s) % 2000001U) / 1000000.0F - 1.0F);
    }
}

bool same_bytes(const void* a, const void* b, usize n)
{
    return std::memcmp(a, b, n) == 0;
}

u64 f32_mismatches(const f32* a, const f32* b, u64 n)
{
    u64 bad = 0;
    for (u64 i = 0; i < n; ++i)
    {
        bad += std::bit_cast<u32>(a[i]) != std::bit_cast<u32>(b[i]) ? 1U : 0U;
    }
    return bad;
}

// The Q8_0 linear's pinned order, one output at a time: eight exact i32 lanes of four products per block, one fma per
// lane per block, the tree ((l0+l1)+(l2+l3)) + ((l4+l5)+(l6+l7)), the bias, then ReLU.
f32 q8_output_reference(const BlockQ8Zero* xrow, const BlockQ8Zero* wrow, const f32* wscale, u64 nb, f32 bias,
                        bool relu)
{
    f32 lanes[8] = {};
    for (u64 bi = 0; bi < nb; ++bi)
    {
        const f32 scale = tn::f16_bits_to_f32(xrow[bi].d) * wscale[bi];
        for (u32 k = 0; k < 8U; ++k)
        {
            crd::i32 lane = 0;
            for (u32 j = 0; j < 4U; ++j)
            {
                lane += static_cast<crd::i32>(xrow[bi].qs[4U * k + j]) * static_cast<crd::i32>(wrow[bi].qs[4U * k + j]);
            }
            lanes[k] = std::fma(scale, static_cast<f32>(lane), lanes[k]);
        }
    }
    const f32 q0 = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    const f32 q1 = (lanes[4] + lanes[5]) + (lanes[6] + lanes[7]);
    const f32 v = (q0 + q1) + bias;
    if (relu && !(v > 0.0F))
    {
        return 0.0F; // ReLU (a NaN becomes 0)
    }
    return v;
}

// The int8 linear's formula: fma(sx * sw, f32(dot(u8 x, i8 w) - zp * colsum), bias), then ReLU.
f32 i8_output_reference(const crd::u8* xrow, const crd::i8* wrow, u64 k, f32 scale_m, crd::i32 zp, crd::i32 colsum,
                        f32 bias, bool relu)
{
    crd::i32 dot = 0;
    for (u64 j = 0; j < k; ++j)
    {
        dot += static_cast<crd::i32>(xrow[j]) * static_cast<crd::i32>(wrow[j]);
    }
    const f32 v = std::fma(scale_m, static_cast<f32>(dot - zp * colsum), bias);
    if (relu && !(v > 0.0F))
    {
        return 0.0F; // ReLU (a NaN becomes 0)
    }
    return v;
}

// The packed runner layout (nn.hpp i8_pack_weights): [ceil(N/8) blocks][K/4 slices][8 outputs x 4 k-bytes], zero
// padding columns.
void pack_i8(const crd::i8* w, u64 n, u64 k, crd::i8* packed)
{
    const u64 n8 = (n + 7U) & ~u64{7};
    for (u64 o = 0; o < n8; ++o)
    {
        for (u64 c = 0; c < k; ++c)
        {
            packed[(o / 8U) * (k * 8U) + (c / 4U) * 32U + (o % 8U) * 4U + (c % 4U)] =
                o < n ? w[o * k + c] : static_cast<crd::i8>(0);
        }
    }
}

} // namespace

TEST_CASE("tensor kernels: every level the host runs gives the scalar Q8_0 and u8 quantizer bytes", "[tensor][kernels]")
{
    constexpr u64 blocks = 23U;
    constexpr u64 total = blocks * tn::kQuantBlock;
    static f32 x[total];
    fill_uniform(x, total, 0xA11CEU, 100.0F);
    for (u64 j = 0; j < tn::kQuantBlock; ++j)
    {
        const f32 fj = static_cast<f32>(j);
        x[0U * 32U + j] = 0.0F;                                // amax 0: d = 0, every q = 0
        x[1U * 32U + j] = (j % 2U) == 0U ? 0.0F : -0.0F;       // signed zeros
        x[2U * 32U + j] = (j % 2U) == 0U ? 3.4e37F : -3.4e37F; // huge
        x[3U * 32U + j] = fj * 1e-30F;                         // tiny scale
        x[4U * 32U + j] = j == 0U ? 127.0F : fj - 15.5F;       // d = 1: exact .5 ties round away from zero
        x[5U * 32U + j] = (j % 5U) == 2U ? kQNaN : fj - 9.0F;  // NaN elements
        x[6U * 32U + j] = kQNaN;                               // all NaN
        x[7U * 32U + j] = j == 7U ? kInf : fj;                 // an infinity: d = inf
        x[8U * 32U + j] = j == 3U ? -kInf : 1.0F;              // both infinities (+inf below)
    }
    x[8U * 32U + 9U] = kInf;
    for (u64 j = 0; j < 4U * 32U; ++j) // post-ReLU, all-negative and constant rows for the u8 range
    {
        x[9U * 32U + j] = x[9U * 32U + j] < 0.0F ? 0.0F : x[9U * 32U + j];
        x[13U * 32U + j] = -1.0F - (x[13U * 32U + j] < 0.0F ? -x[13U * 32U + j] : x[13U * 32U + j]);
        x[17U * 32U + j] = 2.25F;
    }
    BlockQ8Zero want[blocks];
    BlockQ8Zero got[blocks];
    tn::quantize_q8_0({x, total}, {want, blocks});
    for (u64 j = 0; j < tn::kQuantBlock; ++j) // the defined values: a NaN quantizes as 0, an infinite d zeroes
    {
        CHECK(want[0].qs[j] == 0);
        CHECK(want[6].qs[j] == 0);
        CHECK(want[7].qs[j] == 0);
        CHECK((want[5].qs[j] == 0) == ((j % 5U) == 2U || j == 9U)); // the NaNs, and x = 0
    }
    CHECK(want[4].qs[1] == -15); // -14.5 rounds away from zero
    CHECK(want[4].qs[16] == 1);  // 0.5 rounds away from zero
    // u8 rows over the same inputs: each 32-value block on its own, then four 128-value rows.
    struct Row
    {
        u64 offset;
        u64 k;
    };
    Row rows[blocks + 4U];
    for (u64 b = 0; b < blocks; ++b)
    {
        rows[b] = {b * 32U, 32U};
    }
    rows[blocks + 0U] = {9U * 32U, 128U};
    rows[blocks + 1U] = {13U * 32U, 128U};
    rows[blocks + 2U] = {17U * 32U, 128U};
    rows[blocks + 3U] = {19U * 32U, 128U};
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        level.table->quantize_q8_0(x, got, blocks);
        for (u64 b = 0; b < blocks; ++b)
        {
            INFO(crd::cpu_level_name(level.level) << " Q8_0 block " << b);
            CHECK(same_bytes(&want[b], &got[b], sizeof(BlockQ8Zero)));
        }
        for (const Row& r : rows)
        {
            crd::u8 qw[128];
            crd::u8 qg[128];
            f32 sw = -1.0F;
            f32 sg = -2.0F;
            crd::i32 zw = -1;
            crd::i32 zg = -2;
            tn::nn_quantize_u8_asym({x + r.offset, r.k}, {qw, r.k}, sw, zw);
            level.table->quantize_u8_row(x + r.offset, r.k, qg, sg, zg);
            INFO(crd::cpu_level_name(level.level) << " u8 row at " << r.offset << " k " << r.k);
            CHECK(same_bytes(qw, qg, r.k));
            CHECK(std::bit_cast<u32>(sw) == std::bit_cast<u32>(sg));
            CHECK(zw == zg);
            if (r.offset == 5U * 32U && r.k == 32U)
            {
                CHECK(qw[2] == static_cast<crd::u8>(zw)); // a NaN sits at the zero point
            }
            if (r.offset == 8U * 32U && r.k == 32U)
            {
                CHECK(zw == 0); // inf/inf: no zero point
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the pinned Q8_0 linear bits", "[tensor][kernels]")
{
    constexpr u64 batch = 6U;
    constexpr u64 max_k = 256U;
    constexpr u64 max_n = 19U;
    static f32 x[batch * max_k];
    static BlockQ8Zero wq[max_n * (max_k / 32U)];
    static f32 wscale[max_n * (max_k / 32U)];
    static f32 bias[max_n];
    static BlockQ8Zero xq_want[batch * (max_k / 32U)];
    static BlockQ8Zero xq[batch * (max_k / 32U)];
    static f32 xscale[batch * (max_k / 32U)];
    static f32 want[batch * max_n + kSlack];
    static f32 got[batch * max_n + kSlack];
    u64 s = 0xD07U;
    for (u64 i = 0; i < max_n * (max_k / 32U); ++i)
    {
        wq[i].d = tn::f32_to_f16_bits(static_cast<f32>(lcg(s) % 1000U + 1U) * 1e-4F);
        wscale[i] = tn::f16_bits_to_f32(wq[i].d);
        for (u32 j = 0; j < 32U; ++j)
        {
            wq[i].qs[j] = static_cast<crd::i8>(static_cast<crd::i32>(lcg(s) % 255U) - 127); // [-127, 127]
        }
    }
    fill_uniform(bias, max_n, 0xB1A5U, 0.5F);
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        for (const u64 k : {32U, 96U, 256U})
        {
            const u64 nb = k / 32U;
            fill_uniform(x, batch * k, 0x5AU + k, 3.0F);
            for (u64 j = 0; j < k; ++j) // row 1 saturates its blocks at +-127 (every |x| is the amax)
            {
                x[k + j] = (j % 2U) == 0U ? 4.0F : -4.0F;
            }
            tn::quantize_q8_0({x, batch * k}, {xq_want, batch * nb});
            for (const u64 n : {1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U, 13U, 16U, 19U})
            {
                for (const bool relu : {false, true})
                {
                    for (u64 m = 0; m < batch; ++m)
                    {
                        for (u64 o = 0; o < n; ++o)
                        {
                            const bool outside = m == 0U || m == batch - 1U; // rows outside [lo, hi)
                            want[m * n + o] = outside ? std::bit_cast<f32>(kSentinel)
                                                      : q8_output_reference(xq_want + m * nb, wq + o * nb,
                                                                            wscale + o * nb, nb, bias[o], relu);
                            got[m * n + o] = std::bit_cast<f32>(kSentinel);
                        }
                    }
                    for (u64 i = batch * n; i < batch * n + kSlack; ++i)
                    {
                        want[i] = std::bit_cast<f32>(kSentinel);
                        got[i] = std::bit_cast<f32>(kSentinel);
                    }
                    const tn::NnLinearQ8 c{x, wq, wscale, bias, got, xq, xscale, k, n, relu};
                    level.table->nn_linear_q8_rows(c, 1U, static_cast<u32>(batch - 1U));
                    INFO(crd::cpu_level_name(level.level) << " k " << k << " n " << n << " relu " << relu);
                    CHECK(f32_mismatches(want, got, batch * n + kSlack) == 0U);
                    CHECK(same_bytes(xq_want + nb, xq + nb, (batch - 2U) * nb * sizeof(BlockQ8Zero)));
                }
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the int8 linear bits, row-major and packed",
          "[tensor][kernels]")
{
    constexpr u64 batch = 7U;
    constexpr u64 max_k = 160U;
    constexpr u64 max_n = 21U;
    constexpr u64 max_n8 = 24U;
    static f32 x[batch * max_k];
    static crd::i8 wq[max_n * max_k];
    static crd::i8 packed[max_n8 * max_k];
    static crd::i32 colsum[max_n8];
    static f32 bias[max_n8];
    static crd::u8 xq_want[batch * max_k];
    static crd::u8 xq[batch * max_k];
    static f32 xs_want[batch];
    static f32 xs[batch];
    static crd::i32 zp_want[batch];
    static crd::i32 zp[batch];
    static f32 want[batch * max_n + kSlack];
    static f32 got[batch * max_n + kSlack];
    u64 s = 0x18U;
    for (u64 i = 0; i < max_n * max_k; ++i)
    {
        wq[i] = static_cast<crd::i8>(static_cast<crd::i32>(lcg(s) % 255U) - 127);
    }
    fill_uniform(bias, max_n8, 0xB1A5U, 0.5F);
    const f32 wscale = 0.0123F;
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        for (const u64 k : {32U, 64U, 160U})
        {
            fill_uniform(x, batch * k, 0x77U + k, 2.0F);
            for (u64 j = 0; j < k; ++j)
            {
                const f32 v = x[1U * k + j] < 0.0F ? -x[1U * k + j] : x[1U * k + j];
                x[1U * k + j] = v;    // >= 0: zp 0, u8 values up to 255
                x[2U * k + j] = -v;   // <= 0: zp 255
                x[3U * k + j] = 0.0F; // s = 0
            }
            for (u64 m = 0; m < batch; ++m)
            {
                tn::nn_quantize_u8_asym({x + m * k, k}, {xq_want + m * k, k}, xs_want[m], zp_want[m]);
            }
            CHECK(zp_want[1] == 0);
            CHECK(zp_want[2] == 255);
            for (const u64 n : {1U, 3U, 4U, 7U, 8U, 9U, 15U, 16U, 21U})
            {
                const u64 n8 = (n + 7U) & ~u64{7};
                for (u64 o = 0; o < n8; ++o)
                {
                    colsum[o] = 0;
                    for (u64 j = 0; o < n && j < k; ++j)
                    {
                        colsum[o] += wq[o * k + j];
                    }
                }
                pack_i8(wq, n, k, packed);
                for (const bool relu : {false, true})
                {
                    for (u64 m = 0; m < batch; ++m)
                    {
                        for (u64 o = 0; o < n; ++o)
                        {
                            want[m * n + o] =
                                m == 0U ? std::bit_cast<f32>(kSentinel) // the row outside [lo, hi)
                                        : i8_output_reference(xq_want + m * k, wq + o * k, k, xs_want[m] * wscale,
                                                              zp_want[m], colsum[o], bias[o], relu);
                        }
                    }
                    for (const bool use_packed : {false, true})
                    {
                        for (u64 i = 0; i < batch * n + kSlack; ++i)
                        {
                            got[i] = std::bit_cast<f32>(kSentinel);
                            want[i] = i < batch * n ? want[i] : std::bit_cast<f32>(kSentinel);
                        }
                        const crd::i8* weights = use_packed ? packed : wq;
                        const tn::NnLinearI8 c{x, weights, colsum, bias, got, xq, xs, zp, wscale, k, n, relu};
                        if (use_packed) // rows [1, 7): a 4-row group, then a 2-row group
                        {
                            level.table->nn_linear_i8_packed_rows(c, 1U, static_cast<u32>(batch));
                        }
                        else
                        {
                            level.table->nn_linear_i8_rows(c, 1U, static_cast<u32>(batch));
                        }
                        INFO(crd::cpu_level_name(level.level)
                             << " k " << k << " n " << n << " relu " << relu << " packed " << use_packed);
                        CHECK(f32_mismatches(want, got, batch * n + kSlack) == 0U);
                        CHECK(same_bytes(xq_want + k, xq + k, (batch - 1U) * k));
                        CHECK(same_bytes(xs_want + 1, xs + 1, (batch - 1U) * sizeof(f32)));
                        CHECK(same_bytes(zp_want + 1, zp + 1, (batch - 1U) * sizeof(crd::i32)));
                    }
                }
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar softmax bits", "[tensor][kernels]")
{
    constexpr u64 rows = 9U;
    constexpr u64 max_d = 37U;
    static f32 x[rows * max_d];
    static f32 want[rows * max_d];
    static f32 got[rows * max_d];
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        for (const u64 d : {1U, 3U, 4U, 5U, 8U, 10U, 31U, 37U})
        {
            fill_uniform(x, rows * d, 0x50FU + d, 8.0F);
            for (u64 j = 0; j < d; ++j)
            {
                const f32 fj = static_cast<f32>(j);
                x[1U * d + j] = 2.5F;                                   // all equal: exactly 1/d
                x[2U * d + j] = j == d / 2U ? 0.0F : -800.0F - fj;      // below -708: crd_exp4's scalar lanes
                x[3U * d + j] = (j % 3U) == 1U ? -kInf : x[3U * d + j]; // exp(-inf) = 0
                x[4U * d + j] = (j % 4U) == 2U ? kQNaN : x[4U * d + j]; // NaN logits
                x[5U * d + j] = j == d - 1U ? kInf : x[5U * d + j];     // inf - inf
                x[6U * d + j] = j == 0U ? 1000.0F : -1000.0F;           // the stable max-subtract
                x[7U * d + j] = fj * 1e-3F - 700.0F;                    // the near-underflow band
            }
            for (u64 r = 0; r < rows; ++r) // the scalar order: max, exp through crd_exp1, index-order sum, divide
            {
                const f32* xr = x + r * d;
                f32* yr = want + r * d;
                f32 m = xr[0];
                for (u64 j = 1; j < d; ++j)
                {
                    m = xr[j] > m ? xr[j] : m;
                }
                f32 sum = 0.0F;
                for (u64 j = 0; j < d; ++j)
                {
                    yr[j] = static_cast<f32>(crd::math::exp(static_cast<f64>(xr[j] - m)));
                    sum += yr[j];
                }
                for (u64 j = 0; j < d; ++j)
                {
                    yr[j] = r == 0U ? std::bit_cast<f32>(kSentinel) : yr[j] / sum; // row 0 lies outside [lo, hi)
                }
            }
            for (u64 i = 0; i < rows * d; ++i)
            {
                got[i] = std::bit_cast<f32>(kSentinel);
            }
            level.table->nn_softmax_rows({x, got, d}, 1U, static_cast<u32>(rows));
            INFO(crd::cpu_level_name(level.level) << " d " << d);
            CHECK(f32_mismatches(want, got, rows * d) == 0U);
            if (d == 4U)
            {
                CHECK(std::bit_cast<u32>(got[1U * d]) == std::bit_cast<u32>(0.25F));
            }
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
