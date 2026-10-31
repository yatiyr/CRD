// The hesap-tensor kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs (the avx2 and avx512
// levels' VNNI variants included, where the host has the feature) gives the scalar oracle's bits for the permute
// regions of f32, f64, Complex<f32> and Complex<f64> elements (unit and other source strides, edge strips, negative and
// broadcast strides, real and complex scales, the non-temporal variants on and off a 64-byte line, NaN payloads,
// nothing written outside the region, with and without a next region to prefetch) and serial walks (ranks 2 to 5,
// a-extents that cut the 16-row block, tiles below the micro transpose, signed-zero and infinite scales), the
// portable level's bits for the Tier R binned sum (around each body's vector threshold and endurance window, the
// speculative widening, the near-DBL_MAX compression regime, non-finite inputs, a non-empty accumulator), the scalar
// quantizers' bytes (ties, signed zeros, NaN and infinite inputs), the pinned order of the Q8_0 and int8 linears (every
// tile width, the saturating and zero-point extremes, -128 and every other i8 weight, subnormal scales, row ranges),
// the scalar softmax (vector tails, crd_exp4's scalar lanes, non-finite logits), the scalar Philox blocks (counter
// and position wraps, partial vectors), the scalar elementwise walk (every inner form, broadcast and reversed operands,
// IEEE specials, min/max ties), Tier D's pinned order (block, row and column edges), einsum's direct products (row,
// depth and column tails), the scalar tier's batched GEMM, Cholesky, LU, SVD and solves (every lane-tier edge of n,
// poisoned lanes, split ranges, guard words) and the scalar sparse TTM scatter, staged tile, part fold and CSF and COO
// MTTKRP walks (ranks 1 to 5, every mode, column tails at every width, sub-ranges, guard words) and nn's scalar
// pooling (IEEE maximum ties, the 256-wide row stage, every window), convolution (every output-channel block, column
// tails, the fused relu and pool, im2col shapes, guard words) and activations (IEEE specials, saturating and
// underflowing arguments, in place, guard words).
// crd-hesap-tensor-kernels-at-<level> runs these [kernels] cases, the SR converts and the sketch fill under each forced
// level.

#include "kernel_levels.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/tensor/batched_core.hpp>
#include <crd/hesap/tensor/dtypes.hpp>
#include <crd/hesap/tensor/reduce.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/hyperbolic.hpp>
#include <crd/math/select.hpp>
#include <crd/math/transcendental.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace tn = crd::hesap::tensor;
using crd::CpuLevel;
using crd::f32;
using crd::f64;
using crd::i64;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using tn::BlockQ8Zero;
using tn::PermuteRegion;
using tn::PermuteWalk;
using tn::Tensor;
using tn::TensorKernels;
using tn::detail::BinnedRungs;

namespace
{

constexpr u32 kSentinel = 0x7FA5A5A5U; // a quiet NaN no region writes

using crd::test::kernel_level_runs_here;

u64 lcg(u64& s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 11U;
}

// A variant's feature after its level's name in a failure message.
const char* variant_suffix(const KernelLevel<TensorKernels>& level)
{
    if (level.features.has(crd::CpuFeature::Avx512Vnni))
    {
        return "+avx512-vnni";
    }
    return level.features.has(crd::CpuFeature::AvxVnni) ? "+avx-vnni" : "";
}

// A permute element of the kernel table: Parts scalars S, a Complex<S> as its (re, im) pair when Parts is 2.
template <typename S, u64 Parts> struct Elem
{
    using Scalar = S;
    using Bits = std::conditional_t<sizeof(S) == 4U, u32, u64>;
    static constexpr u64 kParts = Parts;
    static constexpr i64 kStep = static_cast<i64>(Parts);
};

template <typename E> const char* elem_name()
{
    if constexpr (E::kParts == 1U)
    {
        return sizeof(typename E::Scalar) == 4U ? "f32" : "f64";
    }
    else
    {
        return sizeof(typename E::Scalar) == 4U ? "Complex<f32>" : "Complex<f64>";
    }
}

// A NaN no permute writes.
template <typename S> S sentinel()
{
    if constexpr (sizeof(S) == 4U)
    {
        return std::bit_cast<S>(kSentinel);
    }
    else
    {
        return std::bit_cast<S>(0x7FFA5A5A5A5A5A5AULL);
    }
}

// Values with varied signs and magnitudes, every 13th a signalling NaN with its own payload.
template <typename S> void fill_scalars(S* p, u64 n, u64 seed)
{
    u64 s = seed;
    for (u64 i = 0; i < n; ++i)
    {
        const u64 r = (lcg(s) << 32U) ^ lcg(s);
        if constexpr (sizeof(S) == 4U)
        {
            const u32 bits = (i % 13U) == 5U ? 0x7F800001U + static_cast<u32>(r & 0x3FFFFFU)
                                             : static_cast<u32>(r & 0xBFFFFFFFU); // exponent below the NaN range
            p[i] = std::bit_cast<S>(bits);
        }
        else
        {
            const u64 bits = (i % 13U) == 5U ? 0x7FF0000000000001ULL + (r & 0x7FFFFFFFFFFFFULL)
                                             : r & 0xBFFFFFFFFFFFFFFFULL; // exponent below the NaN range
            p[i] = std::bit_cast<S>(bits);
        }
    }
}

template <typename S> struct Alpha
{
    S re;
    S im;
};

// The scalar oracle of one element: a bit copy, or alpha times it (a real product, or Complex<S>'s operator*).
template <typename E>
void put_reference(typename E::Scalar* d, const typename E::Scalar* s, Alpha<typename E::Scalar> alpha, bool scale)
{
    using S = typename E::Scalar;
    if (!scale)
    {
        for (u64 p = 0; p < E::kParts; ++p)
        {
            d[p] = s[p];
        }
        return;
    }
    if constexpr (E::kParts == 1U)
    {
        d[0] = alpha.re * s[0];
    }
    else
    {
        const crd::hesap::Complex<S> z =
            crd::hesap::Complex<S>{alpha.re, alpha.im} * crd::hesap::Complex<S>{s[0], s[1]};
        d[0] = z.re;
        d[1] = z.im;
    }
}

// The scalars of [0, n) whose bits differ. A scaled complex product may also meet two NaNs in its commutative add,
// whose payload the compiler's operand order picks, so there two NaNs agree (ADR-0134 §7: NaN payloads aside); a
// sentinel must stay bit for bit.
template <typename E>
u64 count_mismatches(const typename E::Scalar* want, const typename E::Scalar* got, u64 n, bool scale)
{
    using S = typename E::Scalar;
    using B = typename E::Bits;
    const B guard = std::bit_cast<B>(sentinel<S>());
    u64 bad = 0;
    for (u64 i = 0; i < n; ++i)
    {
        const B w = std::bit_cast<B>(want[i]);
        const bool nans = E::kParts == 2U && scale && w != guard && std::isnan(want[i]) && std::isnan(got[i]);
        bad += w == std::bit_cast<B>(got[i]) || nans ? 0U : 1U;
    }
    return bad;
}

// The table entry of the element's scalar.
void run_region(const TensorKernels& k, const PermuteRegion<f32>& r)
{
    k.permute_region_f32(r);
}

void run_region(const TensorKernels& k, const PermuteRegion<f64>& r)
{
    k.permute_region_f64(r);
}

void run_walk(const TensorKernels& k, const PermuteWalk<f32>& w)
{
    k.permute_walk_f32(w);
}

void run_walk(const TensorKernels& k, const PermuteWalk<f64>& w)
{
    k.permute_walk_f64(w);
}

// The scales of a region or walk: unscaled first, then real (-0 included) or complex ones (signed zeros, a pure
// imaginary), and for the walks an infinite one over signed zeros (the default NaN).
template <typename E> void permute_alphas(Alpha<typename E::Scalar> (&alphas)[6])
{
    using S = typename E::Scalar;
    const S inf = std::numeric_limits<S>::infinity();
    if constexpr (E::kParts == 1U)
    {
        const Alpha<S> real[] = {{S(1), S(0)},     {S(-0.5), S(0)}, {S(3), S(0)},
                                 {S(0.125), S(0)}, {S(-0.0), S(0)}, {inf, S(0)}};
        std::memcpy(alphas, real, sizeof(real));
    }
    else
    {
        const Alpha<S> cplx[] = {{S(1), S(0)}, {S(-0.5), S(0.25)}, {S(3), S(-2)},
                                 {S(0), S(1)}, {S(-0.0), S(-0.0)}, {inf, S(0.5)}};
        std::memcpy(alphas, cplx, sizeof(cplx));
    }
}

// permute_copy's test: alpha == 1 is a bit copy (a complex 1 + 0i too, -0i included).
template <typename S> bool scaled(Alpha<S> alpha)
{
    return !(alpha.re == S(1) && alpha.im == S(0));
}

// The permute board's levers (PermutePick, a stage or non-temporal stores): the rules first (no next-tile prefetch),
// then other hints and distances (two tiles ahead, the next tile), no destination prefetch, the scalar tile, HPTT's
// outer order, each with and without the region's stage (or, on a walk, the stores' variant).
struct PickCase
{
    tn::PermutePick pick;
    bool other; // a region: staged; a walk: non-temporal stores
};

constexpr PickCase kPickCases[] = {
    {{}, false},
    {{tn::PermuteHint::T0, 2U, false, false, tn::PermuteOrder::Hptt}, false},
    {{tn::PermuteHint::Nta, 0U, true, false, tn::PermuteOrder::Hptt}, true},
    {{tn::PermuteHint::T1, 1U, true, true, tn::PermuteOrder::Src}, true},
    {{}, true},
};

struct RegionCase
{
    u64 na;
    u64 nb;
    i64 sa;
    i64 sb;
    u64 tile_a;
    u64 tile_b;
};

// Every level the host runs against the scalar oracle over the region cases, for one element type.
template <typename E> void check_permute_regions()
{
    using S = typename E::Scalar;
    constexpr i64 step = E::kStep;
    crd::memory::TlsfAllocator alloc(1U << 24U);
    // Unit source strides with extents on and off the micro grids (both edge strips), tiles that cut the grid, a
    // region smaller than one microkernel, non-unit, negative and broadcast strides; then a-extents around the 16-row
    // block (one short of it, one block, one past, a block and a half, two and one past) and tiles that cut a block.
    const RegionCase cases[] = {
        {64U, 64U, 1, 64, 64U, 64U},    {67U, 61U, 1, 70, 64U, 64U},  {130U, 75U, 1, 131, 32U, 32U},
        {512U, 40U, 1, 512, 512U, 32U}, {7U, 5U, 1, 7, 64U, 64U},     {9U, 17U, 1, 9, 5U, 3U},
        {40U, 24U, 3, 121, 64U, 64U},   {33U, 16U, 1, -40, 64U, 64U}, {16U, 40U, 1, 0, 64U, 64U},
        {24U, 24U, -1, 30, 64U, 64U},   {8U, 8U, 1, 8, 8U, 8U},       {1U, 1U, 1, 1, 64U, 64U},
        {15U, 16U, 1, 16, 64U, 64U},    {16U, 24U, 1, 33, 64U, 64U},  {17U, 24U, 1, 17, 64U, 64U},
        {24U, 9U, 1, 30, 16U, 8U},      {33U, 40U, 1, 40, 64U, 64U},  {40U, 32U, 1, 48, 24U, 16U},
        {7U, 9U, 1, 11, 2U, 4U},        {6U, 6U, 1, 6, 3U, 6U},
    };
    Alpha<S> alphas[6];
    permute_alphas<E>(alphas);
    const u64 src_n = 1U << 16U; // elements
    const u64 shape_src[] = {src_n * E::kParts};
    Tensor<S> src(&alloc, shape_src);
    fill_scalars(src.data(), src_n * E::kParts, 0x5EEDU);
    const u64 dst_n = 1U << 15U;
    const u64 shape_dst[] = {dst_n * E::kParts};
    Tensor<S> want(&alloc, shape_dst);
    Tensor<S> got(&alloc, shape_dst);
    const u64 shape_stage[] = {512U * 32U * E::kParts}; // the largest tile_a * tile_b of the cases
    Tensor<S> stage(&alloc, shape_stage);
    // The destination on a 64-byte line and 32 bytes past one (the non-temporal variant's contract is 32-byte rows;
    // a zmm row store must then split).
    for (const u64 lead : {u64{0}, 32U / sizeof(S)})
    {
        for (const RegionCase& c : cases)
        {
            // dst rows padded past nb (to a multiple of 8 for the non-temporal variant: 32 bytes, so f32 rows alternate
            // between a line start and its middle), so writes outside the region land on sentinel words.
            const i64 ra = static_cast<i64>(((c.nb + 9U) + 7U) & ~u64{7});
            // A base inside src such that every negative stride stays in the buffer.
            const i64 lo = (c.sa < 0 ? (static_cast<i64>(c.na) - 1) * -c.sa : 0) +
                           (c.sb < 0 ? (static_cast<i64>(c.nb) - 1) * -c.sb : 0);
            const S* base = src.data() + (lo + 3) * step; // 3: off the 32-byte grid
            REQUIRE(static_cast<u64>(lo + 3 + (static_cast<i64>(c.na) * (c.sa < 0 ? 0 : c.sa)) +
                                     static_cast<i64>(c.nb) * (c.sb < 0 ? 0 : c.sb) + 8) < src_n);
            // the region and its sentinels, after the last row too
            const u64 used = (static_cast<u64>(ra) * c.na + 64U) * E::kParts + lead;
            REQUIRE(used <= dst_n * E::kParts);
            for (const Alpha<S> alpha : alphas)
            {
                const bool scale = scaled(alpha);
                for (u64 i = 0; i < used; ++i)
                {
                    want.data()[i] = sentinel<S>();
                }
                for (u64 ia = 0; ia < c.na; ++ia)
                {
                    for (u64 ib = 0; ib < c.nb; ++ib)
                    {
                        const i64 at = (static_cast<i64>(ia) * ra + static_cast<i64>(ib)) * step;
                        put_reference<E>(want.data() + lead + at,
                                         base + (static_cast<i64>(ia) * c.sa + static_cast<i64>(ib) * c.sb) * step,
                                         alpha, scale);
                    }
                }
                for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
                {
                    if (!kernel_level_runs_here(level))
                    {
                        continue;
                    }
                    for (const bool stream : {false, true})
                    {
                        if (stream && (c.tile_b * sizeof(S) * E::kParts) % 32U != 0U)
                        {
                            continue; // the non-temporal variant needs 32-byte aligned tile rows
                        }
                        if (!stream && lead != 0U)
                        {
                            continue;
                        }
                        // Without a next region, and with the region itself as the next one (its prefetches run and
                        // stay inside it).
                        for (const bool next : {false, true})
                        {
                            const tn::PermuteNext hint{0, 0, next ? c.na : 0U, next ? c.nb : 0U};
                            for (const PickCase& pc : kPickCases)
                            {
                                for (u64 i = 0; i < used; ++i)
                                {
                                    got.data()[i] = sentinel<S>();
                                }
                                run_region(*level.table, {got.data() + lead, ra, base, c.sa, c.sb, c.na, c.nb, c.tile_a,
                                                          c.tile_b, alpha.re, alpha.im, E::kParts == 2U, scale, stream,
                                                          hint, pc.pick, pc.other ? stage.data() : nullptr});
                                INFO(crd::cpu_level_name(level.level)
                                     << variant_suffix(level) << " " << elem_name<E>() << " na " << c.na << " nb "
                                     << c.nb << " sa " << c.sa << " sb " << c.sb << " tiles " << c.tile_a << "x"
                                     << c.tile_b << " alpha " << alpha.re << "+" << alpha.im << "i stream " << stream
                                     << " next " << next << " lead " << lead << " hint "
                                     << static_cast<u32>(pc.pick.hint) << " distance "
                                     << static_cast<u32>(pc.pick.distance) << " dst prefetch " << pc.pick.dst_prefetch
                                     << " scalar " << pc.pick.scalar << " staged " << pc.other);
                                CHECK(count_mismatches<E>(want.data(), got.data(), used, scale) == 0U);
                            }
                        }
                    }
                }
            }
        }
    }
}

// A serial tiled walk (PermuteWalk): the collapsed dims in dst order, their source strides, the src-fastest dim a and
// the tile edge.
struct WalkCase
{
    u32 m;
    u64 shp[5];
    i64 s_in[5];
    u32 a;
    u64 tile;
};

// Every level the host runs against the scalar oracle (every dst element in row-major order over shp, read through
// s_in) over the walk cases, for one element type.
template <typename E> void check_permute_walks()
{
    using S = typename E::Scalar;
    constexpr i64 step = E::kStep;
    crd::memory::TlsfAllocator alloc(1U << 26U);
    // A 16^4 reversal (one tile per plane, the next plane prefetched), rank-4 walks whose a-extent cuts the 16-row
    // block (17, 33) with one and several tiles per plane, a source stride of 2 on a (the scalar tiles), a flipped b,
    // tiles smaller than and cutting the micro transpose, a single plane and a rank-5 walk.
    const WalkCase cases[] = {
        {4U, {16U, 16U, 16U, 16U}, {1, 16, 256, 4096}, 0U, 64U},
        {4U, {17U, 9U, 11U, 24U}, {1, 4488, 408, 17}, 0U, 64U},
        {4U, {33U, 12U, 10U, 40U}, {1, 1320, 15840, 33}, 0U, 32U},
        {3U, {20U, 6U, 24U}, {2, 960, 40}, 0U, 64U},
        {3U, {24U, 5U, 32U}, {1, 768, -24}, 0U, 64U},
        {3U, {40U, 7U, 37U}, {1, 1480, 40}, 0U, 5U},
        {3U, {40U, 7U, 37U}, {1, 1480, 40}, 0U, 24U},
        {2U, {64U, 72U}, {1, 64}, 0U, 64U},
        {5U, {9U, 3U, 2U, 4U, 16U}, {1, 144, 432, 864, 9}, 0U, 64U},
    };
    Alpha<S> alphas[6];
    permute_alphas<E>(alphas);
    const u64 src_n = 1U << 18U; // elements
    const u64 shape_src[] = {src_n * E::kParts};
    Tensor<S> src(&alloc, shape_src);
    fill_scalars(src.data(), src_n * E::kParts, 0xC0FFEEU);
    for (u64 i = 0; i < src_n * E::kParts; ++i) // signed zeros, so an infinite alpha gives the default NaN
    {
        if (i % 7U == 3U)
        {
            src.data()[i] = S(0);
        }
        else if (i % 11U == 4U)
        {
            src.data()[i] = S(-0.0);
        }
    }
    const u64 dst_n = 1U << 18U;
    const u64 shape_dst[] = {dst_n * E::kParts};
    Tensor<S> want(&alloc, shape_dst);
    Tensor<S> got(&alloc, shape_dst);
    for (const WalkCase& c : cases)
    {
        i64 s_out[5] = {};
        s_out[c.m - 1U] = 1;
        for (u32 d = c.m - 1U; d-- > 0U;)
        {
            s_out[d] = s_out[d + 1U] * static_cast<i64>(c.shp[d + 1U]);
        }
        i64 lo = 0;
        i64 hi = 0;
        for (u32 d = 0; d < c.m; ++d)
        {
            const i64 span = (static_cast<i64>(c.shp[d]) - 1) * c.s_in[d];
            lo += span < 0 ? -span : 0;
            hi += span > 0 ? span : 0;
        }
        const S* base = src.data() + (lo + 3) * step; // 3: off the 32-byte grid
        REQUIRE(static_cast<u64>(lo + hi + 4) < src_n);
        const u64 total = static_cast<u64>(s_out[0]) * c.shp[0];
        REQUIRE(total + 64U <= dst_n);              // sentinels after the last element
        const u64 used = (total + 64U) * E::kParts; // the elements and their sentinels
        for (const Alpha<S> alpha : alphas)
        {
            const bool scale = scaled(alpha); // -0 scales too
            for (u64 i = 0; i < used; ++i)
            {
                want.data()[i] = sentinel<S>();
            }
            u64 idx[tn::kPermuteMaxRank] = {};
            for (u64 e = 0; e < total; ++e)
            {
                i64 soff = 0;
                i64 doff = 0;
                for (u32 d = 0; d < c.m; ++d)
                {
                    soff += static_cast<i64>(idx[d]) * c.s_in[d];
                    doff += static_cast<i64>(idx[d]) * s_out[d];
                }
                put_reference<E>(want.data() + doff * step, base + soff * step, alpha, scale);
                for (u32 d = c.m; d-- > 0U;)
                {
                    if (++idx[d] < c.shp[d])
                    {
                        break;
                    }
                    idx[d] = 0;
                }
            }
            // Non-temporal stores need the dst-fastest extent and the tile edge in 32-byte multiples.
            const bool streams =
                (c.shp[c.m - 1U] * sizeof(S) * E::kParts) % 32U == 0U && (c.tile * sizeof(S) * E::kParts) % 32U == 0U;
            for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
            {
                if (!kernel_level_runs_here(level))
                {
                    continue;
                }
                for (const PickCase& pc : kPickCases)
                {
                    if (pc.other && !streams)
                    {
                        continue;
                    }
                    for (u64 i = 0; i < used; ++i)
                    {
                        got.data()[i] = sentinel<S>();
                    }
                    run_walk(*level.table, {got.data(), base, c.shp, c.s_in, s_out, c.m, c.a, c.tile, alpha.re,
                                            alpha.im, E::kParts == 2U, scale, pc.other, pc.pick});
                    INFO(crd::cpu_level_name(level.level)
                         << variant_suffix(level) << " " << elem_name<E>() << " rank " << c.m << " extents " << c.shp[0]
                         << "x" << c.shp[c.m - 1U] << " tile " << c.tile << " alpha " << alpha.re << "+" << alpha.im
                         << "i stream " << pc.other << " hint " << static_cast<u32>(pc.pick.hint) << " distance "
                         << static_cast<u32>(pc.pick.distance) << " scalar " << pc.pick.scalar << " order "
                         << static_cast<u32>(pc.pick.order));
                    CHECK(count_mismatches<E>(want.data(), got.data(), used, scale) == 0U);
                }
            }
        }
    }
}

// The levels the host runs (at least portable).
usize levels_run_here()
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        ran += kernel_level_runs_here(level) ? 1U : 0U;
    }
    return ran;
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

TEST_CASE("tensor kernels: the archive holds portable, each body level the build compiles and the VNNI variants",
          "[tensor][kernels]")
{
    const auto levels = tn::tensor_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    CHECK(levels[0].features == crd::CpuFeatureSet{});
    const crd::CpuFeatureSet vnni = crd::CpuFeatureSet{}.with(crd::CpuFeature::AvxVnni);
    const crd::CpuFeatureSet vnni512 = crd::CpuFeatureSet{}.with(crd::CpuFeature::Avx512Vnni);
    usize sse2 = 0;
    usize sse42 = 0;
    usize avx2 = 0;
    usize avx2_vnni = 0;
    usize avx512 = 0;
    usize avx512_vnni = 0;
    const auto batched_complete = [](const auto& kb)
    {
        return kb.gemm_tiny != nullptr && kb.cholesky != nullptr && kb.lu != nullptr && kb.svd != nullptr &&
               kb.cholesky_solve != nullptr && kb.lu_solve != nullptr;
    };
    const auto sparse_complete = [](const auto& ks)
    {
        return ks.ttm_scatter != nullptr && ks.ttm_staged != nullptr && ks.fold != nullptr &&
               ks.mttkrp_csf != nullptr && ks.mttkrp_coo != nullptr;
    };
    for (usize i = 0; i < levels.size(); ++i)
    {
        REQUIRE(levels[i].table != nullptr);
        CHECK(batched_complete(levels[i].table->batched_f32));
        CHECK(batched_complete(levels[i].table->batched_f64));
        CHECK(sparse_complete(levels[i].table->sparse_f32));
        CHECK(sparse_complete(levels[i].table->sparse_f64));
        CHECK(levels[i].table->nn_pool2d_planes != nullptr);
        CHECK(levels[i].table->nn_layernorm_rows != nullptr);
        CHECK(levels[i].table->nn_act_rows != nullptr);
        CHECK(levels[i].table->nn_conv2d_images != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            // ascending; a variant follows its level's plain table and adds features to it
            const bool higher = static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level);
            const bool variant = levels[i - 1U].level == levels[i].level &&
                                 levels[i].features.has_all(levels[i - 1U].features) &&
                                 levels[i].features != levels[i - 1U].features;
            CHECK((higher || variant));
        }
        sse2 += levels[i].level == CpuLevel::Sse2 ? 1U : 0U;
        sse42 += levels[i].level == CpuLevel::Sse42 ? 1U : 0U;
        avx2 += levels[i].level == CpuLevel::Avx2 && levels[i].features == crd::CpuFeatureSet{} ? 1U : 0U;
        avx2_vnni += levels[i].level == CpuLevel::Avx2 && levels[i].features == vnni ? 1U : 0U;
        avx512 += levels[i].level == CpuLevel::Avx512 && levels[i].features == crd::CpuFeatureSet{} ? 1U : 0U;
        avx512_vnni += levels[i].level == CpuLevel::Avx512 && levels[i].features == vnni512 ? 1U : 0U;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2) ? 1U : 0U));
    CHECK(sse42 == 0U); // no sse4.2 unit: its maddubs dots did not beat sse2's (the 2026-10-31 board)
    const usize built = x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2) ? 1U : 0U;
    CHECK(avx2 == built);
    CHECK(avx2_vnni == built);
    const usize built512 = x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512) ? 1U : 0U;
    CHECK(avx512 == built512);
    CHECK(avx512_vnni == built512);
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar permute region bits of each element type",
          "[tensor][kernels]")
{
    check_permute_regions<Elem<f32, 1U>>();
    check_permute_regions<Elem<f64, 1U>>();
    check_permute_regions<Elem<f32, 2U>>();
    check_permute_regions<Elem<f64, 2U>>();
    CHECK(levels_run_here() >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar permute walk bits of each element type",
          "[tensor][kernels]")
{
    check_permute_walks<Elem<f32, 1U>>();
    check_permute_walks<Elem<f64, 1U>>();
    check_permute_walks<Elem<f32, 2U>>();
    check_permute_walks<Elem<f64, 2U>>();
    CHECK(levels_run_here() >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the portable binned sum bits", "[tensor][kernels]")
{
    crd::memory::TlsfAllocator alloc(1U << 24U);
    constexpr u64 wide = 24U * tn::detail::kBinEndurance;   // the AVX-512 body's super-block
    constexpr u64 window = 12U * tn::detail::kBinEndurance; // the AVX2 body's
    constexpr u64 half = 6U * tn::detail::kBinEndurance;    // the SSE2 body's
    const u64 n_max = 3U * wide + 77U;
    const u64 shape[] = {n_max};
    Tensor<f64> x(&alloc, shape);
    u64 s = 0xC0FFEEU;
    // Heavy cancellation over many magnitudes (the ReproBLAS sum_sine workload class).
    for (u64 i = 0; i < n_max; ++i)
    {
        const f64 mag = 1.0 + 1e6 * static_cast<f64>(lcg(s) % 1000U) / 1000.0;
        x.data()[i] = ((i & 1U) != 0U ? -1.0 : 1.0) * mag * (static_cast<f64>(lcg(s) % 4096U) - 2048.0);
    }
    // Around each body's gate (8 steps of 24, 12 or 6 elements), its step tails and super-blocks.
    const u64 lengths[] = {0U,   1U,        5U,          11U,    23U,          47U,       48U,  49U,        53U,
                           95U,  96U,       97U,         107U,   191U,         192U,      193U, 215U,       half - 1U,
                           half, half + 7U, window - 1U, window, window + 13U, wide - 1U, wide, wide + 13U, n_max};
    const KernelLevel<TensorKernels>& portable = tn::tensor_kernel_levels()[0];
    REQUIRE(portable.level == CpuLevel::Portable);

    // Late magnitudes far above the first window's (in the second super-block of each body): the speculative pass
    // rolls back and widens. A value near DBL_MAX in a later window moves the accumulator to index 0 (the compression
    // regime the scalar deposits handle). An infinity is exact; NaNs with different payloads read out as the one quiet
    // NaN. Powers of two from 2^30 down to 2^-58 land exactly halfway between two values of some rung: the deposit's
    // sticky bit decides those ties.
    Tensor<f64> ties(&alloc, shape);
    Tensor<f64> late(&alloc, shape);
    Tensor<f64> huge(&alloc, shape);
    Tensor<f64> huge_wide(&alloc, shape);
    Tensor<f64> inf(&alloc, shape);
    Tensor<f64> nan(&alloc, shape);
    for (u64 i = 0; i < n_max; ++i)
    {
        late.data()[i] = x.data()[i];
        huge.data()[i] = x.data()[i];
        huge_wide.data()[i] = x.data()[i];
        inf.data()[i] = x.data()[i];
        nan.data()[i] = x.data()[i];
        const f64 power = std::bit_cast<f64>(static_cast<u64>(1023 + 30 - static_cast<i64>(i % 89U)) << 52U);
        const f64 signed_power = (i & 8U) != 0U ? -power : power;
        ties.data()[i] = (i % 7U) == 3U ? signed_power : x.data()[i];
    }
    late.data()[half + 5U] = 2.5e150;
    late.data()[window + 5U] = 3.5e200;
    late.data()[2U * window + 1U] = -3.5e200;
    late.data()[wide + 5U] = 4.5e250;
    huge.data()[window + 7U] = 1.6e308;
    huge.data()[2U * window + 9U] = -1.5e308;
    huge_wide.data()[wide + 7U] = 1.6e308; // in the AVX-512 body's second super-block (huge's are in its first)
    inf.data()[window + 3U] = std::bit_cast<f64>(0x7FF0000000000000ULL);
    nan.data()[50U] = std::bit_cast<f64>(0x7FF8000000000000ULL);
    nan.data()[window + 3U] = std::bit_cast<f64>(0xFFF4000000000123ULL);
    const Tensor<f64>* inputs[] = {&x, &ties, &late, &huge, &inf, &nan, &huge_wide};
    const f64 seeds[] = {0.0, -7.25e5};

    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        for (usize which = 0; which < 7U; ++which)
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
                    INFO(crd::cpu_level_name(level.level)
                         << variant_suffix(level) << " input " << which << " n " << n << " seed " << seed);
                    CHECK(got == want);
                    if (which < 4U || which == 6U) // finite inputs
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
            INFO(crd::cpu_level_name(level.level) << variant_suffix(level) << " chunks " << chunks);
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
            lanes[k] = crd::math::fma(scale, static_cast<f32>(lane), lanes[k]);
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
    const f32 v = crd::math::fma(scale_m, static_cast<f32>(dot - zp * colsum), bias);
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
    constexpr u64 blocks = 25U;
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
    for (u64 j = 0; j < tn::kQuantBlock; ++j) // subnormal scales: 1/d and 1/s overflow, every scaled value is +-inf
    {
        x[23U * 32U + j] = static_cast<f32>(j) * 1e-39F;
        x[24U * 32U + j] = static_cast<f32>(j) * -1e-40F; // u8: rmin < 0, so the zero point is 255
    }
    for (u64 j = 0; j < 4U * 32U; ++j) // post-ReLU, all-negative and constant rows for the u8 range
    {
        x[9U * 32U + j] = x[9U * 32U + j] < 0.0F ? 0.0F : x[9U * 32U + j];
        x[13U * 32U + j] = -1.0F - (x[13U * 32U + j] < 0.0F ? -x[13U * 32U + j] : x[13U * 32U + j]);
        x[17U * 32U + j] = 2.25F;
    }
    BlockQ8Zero want[blocks];
    BlockQ8Zero got[blocks];
    tn::detail::quantize_q8_0_blocks(x, want, blocks);
    for (u64 j = 0; j < tn::kQuantBlock; ++j) // the defined values: a NaN quantizes as 0, an infinite d zeroes
    {
        CHECK(want[0].qs[j] == 0);
        CHECK(want[6].qs[j] == 0);
        CHECK(want[7].qs[j] == 0);
        CHECK((want[5].qs[j] == 0) == ((j % 5U) == 2U || j == 9U)); // the NaNs, and x = 0
    }
    CHECK(want[4].qs[1] == -15); // -14.5 rounds away from zero
    CHECK(want[4].qs[16] == 1);  // 0.5 rounds away from zero
    for (u64 j = 0; j < tn::kQuantBlock; ++j)
    {
        CHECK(want[23].qs[j] == 0);
        CHECK(want[24].qs[j] == 0);
    }
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
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        level.table->quantize_q8_0(x, got, blocks);
        for (u64 b = 0; b < blocks; ++b)
        {
            INFO(crd::cpu_level_name(level.level) << variant_suffix(level) << " Q8_0 block " << b);
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
            INFO(crd::cpu_level_name(level.level)
                 << variant_suffix(level) << " u8 row at " << r.offset << " k " << r.k);
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
            if (r.offset == 24U * 32U && r.k == 32U)
            {
                CHECK(zw == 255);
                for (u64 j = 0; j < 32U; ++j)
                {
                    CHECK(qw[j] == 255U); // every scaled value is -inf (NaN at 0): the zero point
                }
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
            wq[i].qs[j] = static_cast<crd::i8>(static_cast<crd::i32>(lcg(s) % 256U) - 128); // every i8, -128 too
        }
    }
    wscale[5] = 1e-41F; // subnormal weight scales: subnormal products and sums in the fma combine
    wscale[9] = -3e-39F;
    fill_uniform(bias, max_n, 0xB1A5U, 0.5F);
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
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
            tn::detail::quantize_q8_0_blocks(x, xq_want, batch * nb);
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
                    INFO(crd::cpu_level_name(level.level)
                         << variant_suffix(level) << " k " << k << " n " << n << " relu " << relu);
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
        if (!kernel_level_runs_here(level))
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
                        INFO(crd::cpu_level_name(level.level) << variant_suffix(level) << " k " << k << " n " << n
                                                              << " relu " << relu << " packed " << use_packed);
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
        if (!kernel_level_runs_here(level))
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
            INFO(crd::cpu_level_name(level.level) << variant_suffix(level) << " d " << d);
            CHECK(f32_mismatches(want, got, rows * d) == 0U);
            if (d == 4U)
            {
                CHECK(std::bit_cast<u32>(got[1U * d]) == std::bit_cast<u32>(0.25F));
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar layernorm bits", "[tensor][kernels]")
{
    constexpr u64 rows = 7U;
    constexpr u64 max_d = 37U;
    static f32 x[rows * max_d];
    static f32 g[max_d];
    static f32 b[max_d];
    static f32 want[rows * max_d];
    static f32 got[rows * max_d];
    constexpr f32 eps = 1e-5F;
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        for (const u64 d : {1U, 2U, 3U, 8U, 31U, 32U, 37U})
        {
            fill_uniform(x, rows * d, 0x1A7EU + d, 4.0F);
            fill_uniform(g, d, 0x9A3U + d, 2.0F);
            fill_uniform(b, d, 0xB1A5U + d, 1.0F);
            for (u64 j = 0; j < d; ++j)
            {
                x[1U * d + j] = 3.25F;                                   // a constant row: zero variance
                x[2U * d + j] = 1e4F + static_cast<f32>(j % 5U) * 1e-3F; // a large offset, the mean's rounding
                x[3U * d + j] = (j % 4U) == 1U ? kQNaN : x[3U * d + j];  // NaN propagates
                x[4U * d + j] = static_cast<f32>(j % 2U) * 1e-20F;       // a variance below eps
                x[5U * d + j] = (j % 3U) == 0U ? -3e18F : x[5U * d + j]; // a variance that overflows f32
            }
            for (u64 r = 0; r < rows; ++r) // the scalar order: index-order mean, fma chain, rstd, affine fma
            {
                const f32* xr = x + r * d;
                f32* yr = want + r * d;
                f32 mean = 0.0F;
                for (u64 j = 0; j < d; ++j)
                {
                    mean += xr[j];
                }
                mean /= static_cast<f32>(d);
                f32 var = 0.0F;
                for (u64 j = 0; j < d; ++j)
                {
                    const f32 t = xr[j] - mean;
                    var = crd::math::fma(t, t, var);
                }
                var /= static_cast<f32>(d);
                const f32 rstd = 1.0F / crd::math::sqrt(var + eps);
                for (u64 j = 0; j < d; ++j)
                {
                    const f32 affine = crd::math::fma((xr[j] - mean) * rstd, g[j], b[j]);
                    yr[j] = r == 0U ? std::bit_cast<f32>(kSentinel) : affine; // row 0 lies outside [lo, hi)
                }
            }
            for (u64 i = 0; i < rows * d; ++i)
            {
                got[i] = std::bit_cast<f32>(kSentinel);
            }
            level.table->nn_layernorm_rows({x, g, b, got, d, eps}, 1U, static_cast<u32>(rows));
            INFO(crd::cpu_level_name(level.level) << variant_suffix(level) << " d " << d);
            CHECK(f32_mismatches(want, got, rows * d) == 0U);
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar Philox blocks", "[tensor][kernels]")
{
    // Positions straddling the 2^32 word wrap of the Philox counter and the 2^64 wrap of the position; the SR stream
    // (domain tag over step) and one with both words set; block counts through every body's whole groups, single
    // groups and scalar tail (AVX2 24 + 8, SSE2 16, then 4 and 3).
    const u64 firsts[] = {0U, 1U, 0xFFFFFFFCULL, 0xFFFFFFFFFFFFFFFAULL, 0x123456789ABCDEFULL};
    const u64 streams[] = {(static_cast<u64>(tn::detail::kSrTag) << 32U) | 7U, 0x0123456789ABCDEFULL};
    constexpr usize max_blocks = 39;
    const u64 seed = 0xDEADBEEFCAFEF00DULL;
    const u32 key[2] = {static_cast<u32>(seed), static_cast<u32>(seed >> 32U)};
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        usize bad = 0;
        for (usize blocks = 0; blocks <= max_blocks; ++blocks)
        {
            for (const u64 stream : streams)
            {
                for (const u64 first : firsts)
                {
                    u32 got[4U * max_blocks + 1U];
                    got[4U * blocks] = kSentinel; // a guard word past the blocks
                    level.table->philox4x32_blocks(seed, stream, first, got, blocks);
                    for (usize b = 0; b < blocks; ++b)
                    {
                        const u64 position = first + b; // mod 2^64
                        const u32 counter[4] = {static_cast<u32>(position), static_cast<u32>(position >> 32U),
                                                static_cast<u32>(stream), static_cast<u32>(stream >> 32U)};
                        const crd::hesap::stats::PhiloxBlock want = crd::hesap::stats::philox4x32(counter, key);
                        for (usize w = 0; w < 4U; ++w)
                        {
                            bad += got[4U * b + w] == want.v[w] ? 0U : 1U;
                        }
                    }
                    bad += got[4U * blocks] == kSentinel ? 0U : 1U;
                }
            }
        }
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(bad == 0U);
    }
    CHECK(ran >= 1U);
}

// ---- elementwise, Tier D and einsum kernels ------------------------------------

namespace
{

using tn::BinaryOp;
using tn::ReduceOp;

template <typename T> using Bits = std::conditional_t<sizeof(T) == 4U, u32, u64>;

// Values of varied signs and magnitudes; every `special`-th (0: none) is a quiet NaN (when nan), an infinity, a signed
// zero, a subnormal or an extreme. An input NaN has one payload, so one operation's NaN is the same whichever operand
// it comes from; a chain of operations that makes NaNs of its own (inf - inf) gets no input NaN, since the payload of
// two different NaNs depends on their operand order, which a compiler may swap in the scalar oracle.
template <typename T> void fill_values(T* p, u64 n, u64 seed, u64 special, bool nan = true)
{
    using L = std::numeric_limits<T>;
    const T specials[] = {L::quiet_NaN(),  L::infinity(),           -L::infinity(), T{0},       -T{0},
                          L::denorm_min(), -L::denorm_min() * T{3}, L::max(),       L::lowest()};
    u64 s = seed;
    for (u64 i = 0; i < n; ++i)
    {
        const u64 r = lcg(s);
        if (special != 0U && r % special == 0U)
        {
            p[i] = nan ? specials[(r >> 8U) % 9U] : specials[1U + (r >> 8U) % 8U];
        }
        else
        {
            p[i] = static_cast<T>(static_cast<i64>(r % 2000001U) - 1000000) / T{1024};
        }
    }
}

template <typename T> u64 mismatches(const T* want, const T* got, u64 n)
{
    u64 bad = 0;
    for (u64 i = 0; i < n; ++i)
    {
        bad += std::bit_cast<Bits<T>>(want[i]) != std::bit_cast<Bits<T>>(got[i]) ? 1U : 0U;
    }
    return bad;
}

// The scalar oracle: one IEEE operation; Min and Max as minps and maxps (the second operand on a NaN or a tie).
template <typename T> T apply_op(BinaryOp op, T a, T b)
{
    switch (op)
    {
        case BinaryOp::Add:
            return a + b;
        case BinaryOp::Sub:
            return a - b;
        case BinaryOp::Mul:
            return a * b;
        case BinaryOp::Div:
            return a / b;
        case BinaryOp::Min:
            return a < b ? a : b;
        case BinaryOp::Max:
            return a > b ? a : b;
    }
    return a;
}

BinaryOp fold_op(ReduceOp op)
{
    switch (op)
    {
        case ReduceOp::Sum:
            return BinaryOp::Add;
        case ReduceOp::Prod:
            return BinaryOp::Mul;
        case ReduceOp::Min:
            return BinaryOp::Min;
        default:
            return BinaryOp::Max;
    }
}

template <typename T> void (*ew_entry(const TensorKernels& k, BinaryOp op))(const tn::EwBinaryWalk<T>&) noexcept
{
    if constexpr (sizeof(T) == 4U)
    {
        return k.ew_binary_f32[static_cast<u32>(op)];
    }
    else
    {
        return k.ew_binary_f64[static_cast<u32>(op)];
    }
}

template <typename T> const tn::ReduceKernels<T>& reduce_entry(const TensorKernels& k, ReduceOp op)
{
    if constexpr (sizeof(T) == 4U)
    {
        return k.reduce_f32[static_cast<u32>(op)];
    }
    else
    {
        return k.reduce_f64[static_cast<u32>(op)];
    }
}

// One walk of the binary kernel against the row-major oracle, with guard words around dst.
struct EwCase
{
    u32 rank;
    u64 shape[3];
    i64 sa[3];
    i64 sb[3];
    i64 a_base; // the operands' first element in their buffers (negative strides start inside)
    i64 b_base;
};

template <typename T> u64 check_walk(const TensorKernels& k, BinaryOp op, const EwCase& c, const T* a, const T* b)
{
    constexpr u64 guard = 16U;
    static T want[8192 + 2U * guard];
    static T got[8192 + 2U * guard];
    u64 total = 1;
    for (u32 d = 0; d < c.rank; ++d)
    {
        total *= c.shape[d];
    }
    for (u64 i = 0; i < total + 2U * guard; ++i)
    {
        want[i] = sentinel<T>();
        got[i] = sentinel<T>();
    }
    for (u64 w = 0; w < total; ++w)
    {
        i64 oa = c.a_base;
        i64 ob = c.b_base;
        u64 rest = w;
        for (u32 d = c.rank; d-- > 0U;)
        {
            const i64 id = static_cast<i64>(rest % c.shape[d]);
            rest /= c.shape[d];
            oa += id * c.sa[d];
            ob += id * c.sb[d];
        }
        want[guard + w] = apply_op(op, a[oa], b[ob]);
    }
    const tn::EwBinaryWalk<T> walk{got + guard, a + c.a_base, b + c.b_base, c.shape, c.sa, c.sb, c.rank};
    ew_entry<T>(k, op)(walk);
    return mismatches(want, got, total + 2U * guard);
}

template <typename T> u64 check_binary_level(const TensorKernels& k)
{
    static T a[8192];
    static T b[8192];
    fill_values(a, 8192U, 0xE1U + sizeof(T), 5U);
    fill_values(b, 8192U, 0xE2U + sizeof(T), 7U);
    using L = std::numeric_limits<T>;
    const T ties[7][2] = {{T{0}, -T{0}},
                          {-T{0}, T{0}},
                          {L::quiet_NaN(), T{1}},
                          {T{1}, L::quiet_NaN()},
                          {L::quiet_NaN(), L::quiet_NaN()},
                          {L::infinity(), L::infinity()},
                          {T{2}, T{2}}};
    for (u64 i = 0; i < 64U; ++i) // the signed-zero, NaN and equal ties of min and max
    {
        a[5U * i + 2U] = ties[i % 7U][0];
        b[5U * i + 2U] = ties[i % 7U][1];
    }
    u64 bad = 0;
    for (const BinaryOp op : {BinaryOp::Add, BinaryOp::Sub, BinaryOp::Mul, BinaryOp::Div, BinaryOp::Min, BinaryOp::Max})
    {
        bad += check_walk<T>(k, op, {0U, {}, {}, {}, 0, 5}, a, b);
        for (const u64 n : {1U, 3U, 4U, 5U, 7U, 8U, 9U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 1000U})
        {
            const i64 sn = static_cast<i64>(n);
            bad += check_walk<T>(k, op, {1U, {n}, {1}, {1}, 0, 1}, a, b);   // both step
            bad += check_walk<T>(k, op, {1U, {n}, {1}, {0}, 2, 7}, a, b);   // b broadcast
            bad += check_walk<T>(k, op, {1U, {n}, {0}, {1}, 9, 3}, a, b);   // a broadcast
            bad += check_walk<T>(k, op, {1U, {n}, {0}, {0}, 4, 11}, a, b);  // both broadcast
            bad += check_walk<T>(k, op, {1U, {n}, {3}, {2}, 1, 0}, a, b);   // strided
            bad += check_walk<T>(k, op, {1U, {n}, {-1}, {1}, sn, 6}, a, b); // reversed
            bad += check_walk<T>(k, op, {3U, {2U, 3U, n}, {3 * sn, sn, 1}, {0, sn, 1}, 0, 0}, a, b);
            bad += check_walk<T>(k, op, {3U, {2U, 3U, n}, {3 * sn, sn, 1}, {1, 0, 0}, 0, 0}, a, b);
            bad += check_walk<T>(k, op, {3U, {3U, 2U, n}, {0, 0, 1}, {2 * sn, sn, 1}, 13, 0}, a, b);
            bad += check_walk<T>(k, op, {3U, {2U, 2U, n}, {4 * sn, 2 * sn, 2}, {sn, 0, 1}, 0, 0}, a, b);
        }
        bad += check_walk<T>(k, op, {3U, {2U, 0U, 9U}, {9, 9, 1}, {9, 9, 1}, 0, 0}, a, b); // an empty dim
    }
    return bad;
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar elementwise bits", "[tensor][kernels]")
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(check_binary_level<f32>(*level.table) == 0U);
        CHECK(check_binary_level<f64>(*level.table) == 0U);
    }
    CHECK(ran >= 1U);
}

// Tier D's pinned order: four kEwLanes-wide accumulators striped over the run, ((a0 op a1) op (a2 op a3)), the lanes
// left to right, the scalar tail last, all folded onto init.
template <typename T> T block_reference(ReduceOp rop, const T* p, u64 n, T init)
{
    const BinaryOp op = fold_op(rop);
    constexpr u64 group_lanes = tn::kEwLanes<T>;
    T acc = init;
    u64 i = 0;
    if (n >= 4U * group_lanes)
    {
        T lanes[4][group_lanes];
        for (u64 g = 0; g < 4U; ++g)
        {
            for (u64 l = 0; l < group_lanes; ++l)
            {
                lanes[g][l] = p[g * group_lanes + l];
            }
        }
        for (i = 4U * group_lanes; i + 4U * group_lanes <= n; i += 4U * group_lanes)
        {
            for (u64 g = 0; g < 4U; ++g)
            {
                for (u64 l = 0; l < group_lanes; ++l)
                {
                    lanes[g][l] = apply_op(op, lanes[g][l], p[i + g * group_lanes + l]);
                }
            }
        }
        T fold = T{};
        for (u64 l = 0; l < group_lanes; ++l)
        {
            const T c = apply_op(op, apply_op(op, lanes[0][l], lanes[1][l]), apply_op(op, lanes[2][l], lanes[3][l]));
            fold = l == 0U ? c : apply_op(op, fold, c);
        }
        acc = apply_op(op, acc, fold);
    }
    for (; i < n; ++i)
    {
        acc = apply_op(op, acc, p[i]);
    }
    return acc;
}

template <typename T> T tree_reference(ReduceOp rop, const T* p, u64 len)
{
    T init = p[0];
    if (rop == ReduceOp::Sum || rop == ReduceOp::Prod)
    {
        init = rop == ReduceOp::Sum ? T{0} : T{1};
    }
    T acc = init;
    for (u64 off = 0; off < len; off += tn::detail::kReduceBlock)
    {
        const u64 n = len - off < tn::detail::kReduceBlock ? len - off : tn::detail::kReduceBlock;
        const T partial = block_reference(rop, p + off, n, init);
        acc = off == 0U ? partial : apply_op(fold_op(rop), acc, partial);
    }
    return acc;
}

template <typename T> u64 check_reduce_level(const TensorKernels& k)
{
    constexpr u64 count = 24000U;
    constexpr u64 guard = 8U;
    static T p[count];
    static T want[count + 2U * guard];
    static T got[count + 2U * guard];
    u64 bad = 0;
    for (const u64 special : {0U, 97U, 11U})
    {
        for (const ReduceOp rop : {ReduceOp::Sum, ReduceOp::Prod, ReduceOp::Min, ReduceOp::Max})
        {
            // min and max select, so their NaNs keep the input's payload.
            fill_values(p, count, 0x7D0U + special, special, rop == ReduceOp::Min || rop == ReduceOp::Max);
            const tn::ReduceKernels<T>& r = reduce_entry<T>(k, rop);
            for (const u64 n :
                 {0U, 1U, 7U, 8U, 15U, 16U, 31U, 32U, 33U, 63U, 64U, 65U, 100U, 4095U, 4096U, 4097U, 9000U, 20000U})
            {
                const T init = rop == ReduceOp::Prod ? T{1} : p[n % 5U];
                const T expect = block_reference(rop, p, n, init);
                const T actual = r.block(p, n, init);
                bad += std::bit_cast<Bits<T>>(expect) != std::bit_cast<Bits<T>>(actual) ? 1U : 0U;
            }
            // rows: runs of len through the tree, a sub-range [o0, o1) of the outputs.
            for (const u64 len : {1U, 5U, 31U, 32U, 33U, 4095U, 4096U, 4097U, 8193U})
            {
                const u64 outs = count / len < 6U ? count / len : 6U;
                for (u64 i = 0; i < outs + 2U * guard; ++i)
                {
                    want[i] = sentinel<T>();
                    got[i] = sentinel<T>();
                }
                const u64 o0 = outs > 2U ? 1U : 0U;
                for (u64 o = o0; o < outs; ++o)
                {
                    want[guard + o] = tree_reference(rop, p + o * len, len);
                }
                r.rows(got + guard, p, len, o0, outs);
                bad += mismatches(want, got, outs + 2U * guard);
            }
            // columns: rows x w slices folded in row order, a sub-range [j0, j1).
            for (const u64 rows : {1U, 2U, 5U})
            {
                for (const u64 w : {1U, 7U, 8U, 9U, 16U, 33U, 4103U})
                {
                    for (u64 j = 0; j < w + 2U * guard; ++j)
                    {
                        want[j] = sentinel<T>();
                        got[j] = sentinel<T>();
                    }
                    const u64 j0 = w > 8U ? 3U : 0U;
                    const u64 j1 = w > 8U ? w - 1U : w;
                    for (u64 j = j0; j < j1; ++j)
                    {
                        T acc = p[j];
                        for (u64 i = 1; i < rows; ++i)
                        {
                            acc = apply_op(fold_op(rop), acc, p[i * w + j]);
                        }
                        want[guard + j] = acc;
                    }
                    r.columns(got + guard, p, rows, w, j0, j1);
                    bad += mismatches(want, got, w + 2U * guard);
                }
            }
        }
    }
    return bad;
}

TEST_CASE("tensor kernels: every level the host runs gives the pinned Tier D bits", "[tensor][kernels]")
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(check_reduce_level<f32>(*level.table) == 0U);
        CHECK(check_reduce_level<f64>(*level.table) == 0U);
    }
    CHECK(ran >= 1U);
}

// einsum's direct products: every output a + a*b chain in k order from +0, rounded after each operation.
template <typename T> u64 check_einsum_level(const TensorKernels& k)
{
    constexpr u64 guard = 8U;
    static T a[64U * 64U];
    static T b[64U * 128U];
    static T want[64U * 128U + 2U * guard];
    static T got[64U * 128U + 2U * guard];
    fill_values(a, 64U * 64U, 0xA0U + sizeof(T), 61U, false);
    fill_values(b, 64U * 128U, 0xB0U + sizeof(T), 0U);
    const auto run = [&](u64 m, u64 kk, u64 n, bool thin, bool a_trans)
    {
        for (u64 i = 0; i < m * n + 2U * guard; ++i)
        {
            want[i] = sentinel<T>();
            got[i] = sentinel<T>();
        }
        for (u64 r = 0; r < m; ++r)
        {
            for (u64 c = 0; c < n; ++c)
            {
                T acc = T{0};
                for (u64 q = 0; q < kk; ++q)
                {
                    const T av = a_trans ? a[q * m + r] : a[r * kk + q];
                    const T prod = av * b[q * n + c];
                    acc = acc + prod;
                }
                want[guard + r * n + c] = acc;
            }
        }
        if constexpr (sizeof(T) == 4U)
        {
            if (thin)
            {
                k.einsum_thin_f32(m, kk, n, a, b, got + guard);
            }
            else
            {
                k.einsum_smallm_f32(m, kk, n, a, b, got + guard, a_trans);
            }
        }
        else
        {
            if (thin)
            {
                k.einsum_thin_f64(m, kk, n, a, b, got + guard);
            }
            else
            {
                k.einsum_smallm_f64(m, kk, n, a, b, got + guard, a_trans);
            }
        }
        return mismatches(want, got, m * n + 2U * guard);
    };
    u64 bad = 0;
    constexpr u64 thin_max = 8U * tn::kEwLanes<T>;
    for (const u64 m : {1U, 2U, 3U, 7U, 8U, 9U, 17U})
    {
        for (const u64 kk : {0U, 1U, 3U, 7U, 48U})
        {
            for (const u64 n : {1U, 3U, 4U, 5U, 7U, 8U, 9U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U})
            {
                if (n <= thin_max)
                {
                    bad += run(m, kk, n, true, false);
                }
            }
            for (const u64 n : {16U, 17U, 24U, 31U, 32U, 33U, 47U, 48U, 49U, 64U, 65U, 100U, 128U})
            {
                bad += run(m, kk, n, false, false);
                bad += run(m, kk, n, false, true);
            }
        }
    }
    return bad;
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar einsum product bits", "[tensor][kernels]")
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(check_einsum_level<f32>(*level.table) == 0U);
        CHECK(check_einsum_level<f64>(*level.table) == 0U);
    }
    CHECK(ran >= 1U);
}

// ---- batched linear algebra -----------------------------------------------------

template <typename T> const tn::BatchedKernels<T>& batched_of(const TensorKernels& k)
{
    if constexpr (std::is_same_v<T, f32>)
    {
        return k.batched_f32;
    }
    else
    {
        return k.batched_f64;
    }
}

// A guard word: finite and unlike any result, so an overwrite by a value or a NaN shows.
template <typename T> constexpr T kGuard = T(-7777.25);

// Equal bits, or a NaN on both sides (ADR-0134 §7: NaN payloads aside; a negated NaN operand keeps or flips its sign
// depending on the instruction form).
template <typename T> u64 nan_aside_mismatches(const T* want, const T* got, u64 n)
{
    u64 bad = 0;
    for (u64 i = 0; i < n; ++i)
    {
        const bool both_nan = std::isnan(want[i]) && std::isnan(got[i]);
        bad += !both_nan && std::bit_cast<Bits<T>>(want[i]) != std::bit_cast<Bits<T>>(got[i]) ? 1U : 0U;
    }
    return bad;
}

u64 i32_mismatches(const crd::i32* want, const crd::i32* got, u64 n)
{
    u64 bad = 0;
    for (u64 i = 0; i < n; ++i)
    {
        bad += want[i] != got[i] ? 1U : 0U;
    }
    return bad;
}

// count matrices of `elems` values at stride elems + 3, the gaps and the tail guard words.
template <typename T> void guard_fill(T* p, u64 count, u64 elems)
{
    for (u64 i = 0; i < count * (elems + 3U) + 3U; ++i)
    {
        p[i] = kGuard<T>;
    }
}

// The tiny GEMM's contract: each element the k-ordered single-rounded fma chain, alpha * acc, then (beta != 0)
// fma(beta, c, alpha * acc); beta 0 never reads c (which holds NaNs here). Rows 1 to 9, depths 0 to 7 and column tails
// across one and two vectors of every width, three matrices at padded strides, the range [1, 3) leaving matrix 0.
template <typename T> u64 check_batched_gemm(const tn::BatchedKernels<T>& kb)
{
    constexpr u64 batch = 3U;
    static T a[batch * (9U * 7U + 3U) + 3U];
    static T b[batch * (7U * 33U + 3U) + 3U];
    static T c0[batch * (9U * 33U + 3U) + 3U];
    static T want[batch * (9U * 33U + 3U) + 3U];
    static T got[batch * (9U * 33U + 3U) + 3U];
    u64 bad = 0;
    for (const u64 m : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U})
    {
        for (const u64 k : {0U, 1U, 3U, 7U})
        {
            for (const u64 n : {1U, 3U, 4U, 7U, 8U, 9U, 15U, 16U, 17U, 24U, 31U, 32U, 33U})
            {
                for (const T beta : {T(0), T(0.75)})
                {
                    const u64 sa = m * k + 3U;
                    const u64 sb = k * n + 3U;
                    const u64 sc = m * n + 3U;
                    fill_values(a, batch * sa, 0xB1U + m, 11U);
                    fill_values(b, batch * sb, 0xB2U + n, 13U);
                    fill_values(c0, batch * sc, 0xB3U + k, 7U);
                    guard_fill(want, batch, m * n);
                    for (u64 q = 0; q < batch; ++q)
                    {
                        for (u64 e = 0; e < m * n; ++e)
                        {
                            want[q * sc + e] = c0[q * sc + e];
                        }
                    }
                    for (u64 i = 0; i < batch * sc + 3U; ++i)
                    {
                        got[i] = want[i];
                    }
                    const T alpha = T(-1.25);
                    for (u64 q = 1; q < batch; ++q)
                    {
                        for (u64 r = 0; r < m; ++r)
                        {
                            for (u64 j = 0; j < n; ++j)
                            {
                                T acc = T(0);
                                for (u64 p = 0; p < k; ++p)
                                {
                                    acc = crd::math::fma(a[q * sa + r * k + p], b[q * sb + p * n + j], acc);
                                }
                                T out = acc * alpha;
                                if (beta != T(0))
                                {
                                    out = crd::math::fma(beta, c0[q * sc + r * n + j], out);
                                }
                                want[q * sc + r * n + j] = out;
                            }
                        }
                    }
                    const tn::BatchedGemm<T> g{
                        alpha, beta, a, b, got, static_cast<i64>(sa), static_cast<i64>(sb), static_cast<i64>(sc),
                        m,     k,    n};
                    kb.gemm_tiny(g, 1U, batch);
                    bad += nan_aside_mismatches(want, got, batch * sc + 3U);
                }
            }
        }
    }
    return bad;
}

// n x n inputs at stride n*n + 3 for a batch of 19 (a group remainder at 4, 8 and 16 lanes): SPD M M^T + n I, or (spd
// false) the random values, with the poisons the lane tier must keep in their own lane.
template <typename T> void batched_inputs(T* p, u64 batch, u64 n, bool spd, u64 seed)
{
    static T m[33U * 33U];
    guard_fill(p, batch, n * n);
    u64 s = seed;
    for (u64 q = 0; q < batch; ++q)
    {
        T* x = p + q * (n * n + 3U);
        for (u64 e = 0; e < n * n; ++e)
        {
            m[e] = static_cast<T>(static_cast<i64>(lcg(s) % 2001U) - 1000) / T(1000);
        }
        for (u64 i = 0; i < n; ++i)
        {
            for (u64 j = 0; j < n; ++j)
            {
                T acc = i == j && spd ? static_cast<T>(n) : T(0);
                for (u64 l = 0; l < n && spd; ++l)
                {
                    acc += m[i * n + l] * m[j * n + l];
                }
                x[i * n + j] = spd ? acc : m[i * n + j];
            }
        }
        const u64 last = n * n - 1U;
        switch (q)
        {
            case 2U: // not SPD at column 1
                x[0] = T(-1);
                break;
            case 5U: // not SPD at the last column; a zero pivot for LU's elimination
                x[last] = spd ? T(-1e6) : T(0);
                break;
            case 7U:
                x[last / 2U] = std::numeric_limits<T>::quiet_NaN();
                break;
            case 9U:
                x[last] = std::numeric_limits<T>::infinity();
                break;
            case 11U: // subnormal and signed-zero entries
                x[n * n / 3U] = std::numeric_limits<T>::denorm_min();
                x[n * n / 2U] = -T(0);
                break;
            case 13U: // LU pivot ties: column 0 is +-2 down every row
                for (u64 i = 0; i < n; ++i)
                {
                    x[i * n] = (i % 2U) == 0U ? T(2) : T(-2);
                }
                break;
            case 15U: // LU: a zero column, singular at column 2 (n > 1)
                for (u64 i = 0; i < n && n > 1U; ++i)
                {
                    x[i * n + 1U] = T(0);
                }
                break;
            case 17U: // SVD: a zero matrix
                for (u64 e = 0; e < n * n; ++e)
                {
                    x[e] = T(0);
                }
                break;
            case 18U: // SVD: two equal columns (rank-deficient)
                for (u64 i = 0; i < n && n > 1U; ++i)
                {
                    x[i * n + 1U] = x[i * n];
                }
                break;
            default:
                break;
        }
    }
}

// Cholesky and LU against batched_core.hpp's scalar tier: every n of the lane tier's edges (one and two groups a
// step), past it (the scalar tier), the batch in two ranges that split a group.
template <typename T> u64 check_batched_factor(const tn::BatchedKernels<T>& kb, bool lu)
{
    constexpr u64 batch = 19U;
    constexpr u64 cap = batch * (33U * 33U + 3U) + 3U;
    static T want[cap];
    static T got[cap];
    crd::i32 want_info[batch];
    crd::i32 got_info[batch];
    static crd::i32 want_piv[batch * 33U];
    static crd::i32 got_piv[batch * 33U];
    u64 bad = 0;
    for (const u64 n : {1U, 2U, 3U, 4U, 5U, 8U, 11U, 16U, 22U, 23U, 32U, 33U})
    {
        const u64 s = n * n + 3U;
        batched_inputs(want, batch, n, !lu, 0xC0U + n);
        for (u64 i = 0; i < batch * s + 3U; ++i)
        {
            got[i] = want[i];
        }
        for (u64 q = 0; q < batch; ++q)
        {
            if (lu)
            {
                tn::batcheddetail::lu_scalar_one(want + q * s, n, want_piv + q * n, want_info + q);
            }
            else
            {
                tn::batcheddetail::chol_scalar_one(want + q * s, n, want_info + q);
            }
            got_info[q] = -1;
        }
        const tn::BatchedFactor<T> f{got, static_cast<i64>(s), got_info, got_piv, n};
        (lu ? kb.lu : kb.cholesky)(f, 0U, 5U);
        (lu ? kb.lu : kb.cholesky)(f, 5U, batch);
        bad += nan_aside_mismatches(want, got, batch * s + 3U);
        bad += i32_mismatches(want_info, got_info, batch);
        bad += lu ? i32_mismatches(want_piv, got_piv, batch * n) : 0U;
    }
    return bad;
}

// The SVD against batched_core.hpp's sweeps and post-pass, at max_sweeps 30 and 2 (some matrices still rotating).
template <typename T> u64 check_batched_svd(const tn::BatchedKernels<T>& kb)
{
    constexpr u64 batch = 19U;
    constexpr u64 cap = batch * (17U * 17U + 3U) + 3U;
    static T a[cap];
    static T want_u[cap];
    static T want_v[cap];
    static T got_u[cap];
    static T got_v[cap];
    T want_sigma[batch * 17U];
    T got_sigma[batch * 17U];
    crd::i32 want_info[batch];
    crd::i32 got_info[batch];
    u64 bad = 0;
    for (const u64 n : {1U, 2U, 3U, 4U, 5U, 8U, 11U, 12U, 16U, 17U})
    {
        for (const u32 sweeps : {30U, 2U})
        {
            const u64 s = n * n + 3U;
            batched_inputs(a, batch, n, false, 0xD0U + n);
            guard_fill(want_u, batch, n * n);
            guard_fill(want_v, batch, n * n);
            guard_fill(got_u, batch, n * n);
            guard_fill(got_v, batch, n * n);
            for (u64 q = 0; q < batch; ++q)
            {
                for (u64 e = 0; e < n * n; ++e)
                {
                    want_u[q * s + e] = a[q * s + e];
                }
                tn::batcheddetail::svd_scalar_sweeps(want_u + q * s, want_v + q * s, n, sweeps, want_info + q);
                tn::batcheddetail::svd_finalize_one(want_u + q * s, want_v + q * s, want_sigma + q * n, n);
                got_info[q] = -1;
            }
            const i64 si = static_cast<i64>(s);
            const tn::BatchedSvd<T> sv{a, got_u, got_v, got_sigma, got_info, si, si, si, n, sweeps};
            kb.svd(sv, 0U, 7U);
            kb.svd(sv, 7U, batch);
            bad += nan_aside_mismatches(want_u, got_u, batch * s + 3U);
            bad += nan_aside_mismatches(want_v, got_v, batch * s + 3U);
            bad += nan_aside_mismatches(want_sigma, got_sigma, batch * n);
            bad += i32_mismatches(want_info, got_info, batch);
        }
    }
    return bad;
}

// The solves against plain substitution loops with a factor of the scalar tier: r 1, 3 and 8 right-hand sides.
template <typename T> u64 check_batched_solves(const tn::BatchedKernels<T>& kb)
{
    constexpr u64 batch = 19U;
    static T f[batch * (32U * 32U + 3U) + 3U];
    static T x0[batch * (32U * 8U + 3U) + 3U];
    static T want[batch * (32U * 8U + 3U) + 3U];
    static T got[batch * (32U * 8U + 3U) + 3U];
    static crd::i32 piv[batch * 32U];
    crd::i32 info[batch];
    u64 bad = 0;
    for (const bool lu : {false, true})
    {
        for (const u64 n : {1U, 4U, 9U, 32U})
        {
            const u64 sf = n * n + 3U;
            batched_inputs(f, batch, n, !lu, 0xE0U + n);
            for (u64 q = 0; q < batch; ++q)
            {
                if (lu)
                {
                    tn::batcheddetail::lu_scalar_one(f + q * sf, n, piv + q * n, info + q);
                }
                else
                {
                    tn::batcheddetail::chol_scalar_one(f + q * sf, n, info + q);
                }
            }
            for (const u64 r : {1U, 3U, 8U})
            {
                const u64 sx = n * r + 3U;
                guard_fill(x0, batch, n * r);
                for (u64 q = 0; q < batch; ++q)
                {
                    fill_values(x0 + q * sx, n * r, 0xE5U + q, 0U);
                }
                for (u64 i = 0; i < batch * sx + 3U; ++i)
                {
                    want[i] = x0[i];
                    got[i] = x0[i];
                }
                for (u64 q = 0; q < batch; ++q)
                {
                    const T* m = f + q * sf;
                    T* x = want + q * sx;
                    for (u64 j = 0; j < n && lu; ++j)
                    {
                        const u64 pj = static_cast<u64>(piv[q * n + j]);
                        for (u64 c = 0; c < r; ++c)
                        {
                            const T t = x[j * r + c];
                            x[j * r + c] = x[pj * r + c];
                            x[pj * r + c] = t;
                        }
                    }
                    for (u64 i = 0; i < n; ++i) // lower, forward (unit diagonal for LU)
                    {
                        for (u64 c = 0; c < r; ++c)
                        {
                            T acc = x[i * r + c];
                            for (u64 p = 0; p < i; ++p)
                            {
                                acc = crd::math::fma(-m[i * n + p], x[p * r + c], acc);
                            }
                            x[i * r + c] = lu ? acc : acc * (T(1) / m[i * n + i]);
                        }
                    }
                    for (u64 i = n; i-- > 0U;) // upper (L^T for Cholesky), backward
                    {
                        for (u64 c = 0; c < r; ++c)
                        {
                            T acc = x[i * r + c];
                            for (u64 p = i + 1U; p < n; ++p)
                            {
                                acc = crd::math::fma(-(lu ? m[i * n + p] : m[p * n + i]), x[p * r + c], acc);
                            }
                            x[i * r + c] = acc * (T(1) / m[i * n + i]);
                        }
                    }
                }
                const tn::BatchedSolve<T> sol{f, got, piv, static_cast<i64>(sf), static_cast<i64>(sx), n, r};
                (lu ? kb.lu_solve : kb.cholesky_solve)(sol, 0U, batch);
                bad += nan_aside_mismatches(want, got, batch * sx + 3U);
            }
        }
    }
    return bad;
}

template <typename T> u64 check_batched_level(const TensorKernels& k)
{
    const tn::BatchedKernels<T>& kb = batched_of<T>(k);
    return check_batched_gemm<T>(kb) + check_batched_factor<T>(kb, false) + check_batched_factor<T>(kb, true) +
           check_batched_svd<T>(kb) + check_batched_solves<T>(kb);
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar tier's batched bits", "[tensor][kernels]")
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(check_batched_level<f32>(*level.table) == 0U);
        CHECK(check_batched_level<f64>(*level.table) == 0U);
    }
    CHECK(ran >= 1U);
}

// --- the sparse tensor walks ---

template <typename T> const tn::SparseTensorKernels<T>& sparse_of(const TensorKernels& k)
{
    if constexpr (std::is_same_v<T, f32>)
    {
        return k.sparse_f32;
    }
    else
    {
        return k.sparse_f64;
    }
}

// The guard words written past a scratch area's first len values.
template <typename T> u64 scratch_overruns(const T* p, u64 len)
{
    u64 bad = 0;
    for (u64 i = len; i < len + 3U; ++i)
    {
        bad += p[i] == kGuard<T> ? 0U : 1U;
    }
    return bad;
}

// A canonical COO: every tuple of dims in lexicographic order (mode 0 major), about two in three kept.
struct SparseCase
{
    u32 rank;
    u64 dims[5];
    u64 n;
    u32 idx[5][256];
};

void make_sparse_case(SparseCase& c, u32 rank, const u64* dims, u64 seed)
{
    c.rank = rank;
    c.n = 0;
    u64 total = 1;
    for (u32 m = 0; m < rank; ++m)
    {
        c.dims[m] = dims[m];
        total *= dims[m];
    }
    u64 s = seed;
    for (u64 lin = 0; lin < total; ++lin)
    {
        if (lcg(s) % 3U == 0U)
        {
            continue;
        }
        u64 rem = lin;
        for (u32 m = rank; m-- > 0U;)
        {
            c.idx[m][c.n] = static_cast<u32>(rem % dims[m]);
            rem /= dims[m];
        }
        ++c.n;
    }
}

// The identity-order CSF tree of a SparseCase: level l's nodes are its distinct (l + 1)-prefixes, the leaves its
// nonzeros; fptr[l][z] is node z's first child.
struct CsfTree
{
    static constexpr u32 kLevels = 5U;
    u64 nodes[kLevels];
    u32 fids[kLevels][256];
    u32 fptr[kLevels][257];
};

void make_csf_tree(const SparseCase& c, CsfTree& t)
{
    for (u32 l = 0; l < c.rank; ++l)
    {
        t.nodes[l] = 0;
    }
    for (u64 e = 0; e < c.n; ++e)
    {
        u32 d = 0;
        if (e > 0U)
        {
            d = c.rank;
            for (u32 m = 0; m < c.rank; ++m)
            {
                if (c.idx[m][e] != c.idx[m][e - 1U])
                {
                    d = m;
                    break;
                }
            }
        }
        for (u32 l = d; l < c.rank; ++l)
        {
            if (l + 1U < c.rank)
            {
                t.fptr[l][t.nodes[l]] = static_cast<u32>(t.nodes[l + 1U]);
            }
            t.fids[l][t.nodes[l]] = c.idx[l][e];
            ++t.nodes[l];
        }
    }
    for (u32 l = 0; l + 1U < c.rank; ++l)
    {
        t.fptr[l][t.nodes[l]] = static_cast<u32>(t.nodes[l + 1U]);
    }
}

// The TTM oracle: out[base + q * stride(mode)] = fma(val[e], u[i_mode * f + q], ...) per nonzero e in [eb, ee).
template <typename T>
void ttm_oracle(const SparseCase& c, const T* val, const T* u, const u64* ostride, u32 mode, u64 f, u64 eb, u64 ee,
                T* out)
{
    for (u64 e = eb; e < ee; ++e)
    {
        u64 base = 0;
        for (u32 m = 0; m < c.rank; ++m)
        {
            base += m == mode ? 0U : static_cast<u64>(c.idx[m][e]) * ostride[m];
        }
        for (u64 q = 0; q < f; ++q)
        {
            T& o = out[base + q * ostride[mode]];
            o = crd::math::fma(val[e], u[static_cast<u64>(c.idx[mode][e]) * f + q], o);
        }
    }
}

// Ranks 1 to 4, every mode (the strided scatter, the contiguous last mode and the staged inner modes), f across one,
// two and many vectors with tails, the whole range and a sub-range that leaves outputs untouched, guard words after
// the output and the tile.
template <typename T> u64 check_sparse_ttm(const tn::SparseTensorKernels<T>& ks)
{
    constexpr u64 lanes = tn::kEwLanes<T>;
    constexpr u64 out_cap = 6U * 4U * 3U * (8U * lanes + 3U);
    static SparseCase c{};
    static T val[256];
    static T u[6U * (8U * lanes + 3U)];
    static T want[out_cap + 3U];
    static T got[out_cap + 3U];
    static T ref[out_cap + 3U];
    static T tile[out_cap + 3U];
    const u64 dims_of[4][4] = {{5U}, {4U, 6U}, {3U, 4U, 5U}, {3U, 2U, 4U, 3U}};
    u64 bad = 0;
    for (u32 rank = 1; rank <= 4U; ++rank)
    {
        make_sparse_case(c, rank, dims_of[rank - 1U], 0x5100U + rank);
        fill_values(val, c.n, 0x5200U + rank, 7U);
        for (u32 mode = 0; mode < rank; ++mode)
        {
            for (const u64 f : {u64{1}, lanes - 1U, lanes, 2U * lanes - 1U, 4U * lanes + 1U, 8U * lanes + 3U})
            {
                fill_values(u, c.dims[mode] * f, 0x5300U + mode * 131U + f, 5U);
                tn::SparseTtmWalk<T> w{};
                u64 osize = 1;
                for (u32 m = rank; m-- > 0U;)
                {
                    w.ostride[m] = osize;
                    osize *= m == mode ? f : c.dims[m];
                }
                for (u32 m = 0; m < rank; ++m)
                {
                    w.planes[m] = c.idx[m];
                }
                w.val = val;
                w.u = u;
                w.f = f;
                w.rank = rank;
                w.mode = mode;
                const u64 suffix = w.ostride[mode];
                const u64 ranges[2][2] = {{0U, c.n}, {c.n / 3U, c.n - c.n / 4U}};
                for (const auto& range : ranges)
                {
                    for (u64 i = 0; i < osize + 3U; ++i)
                    {
                        want[i] = i < osize ? T(0) : kGuard<T>;
                        got[i] = want[i];
                    }
                    ttm_oracle(c, val, u, w.ostride, mode, f, range[0], range[1], want);
                    ks.ttm_scatter(w, range[0], range[1], got);
                    bad += nan_aside_mismatches(want, got, osize + 3U);
                    if (mode == 0U || mode + 1U == rank)
                    {
                        continue;
                    }
                    // staged: every prefix run in the range rewrites its whole [f, suffix] out slice
                    for (u64 i = 0; i < osize + 3U; ++i)
                    {
                        ref[i] = T(0);
                        want[i] = kGuard<T>;
                        got[i] = kGuard<T>;
                    }
                    ttm_oracle(c, val, u, w.ostride, mode, f, range[0], range[1], ref);
                    for (u64 e = range[0]; e < range[1]; ++e)
                    {
                        u64 pbase = 0;
                        for (u32 m = 0; m < mode; ++m)
                        {
                            pbase += static_cast<u64>(c.idx[m][e]) * w.ostride[m];
                        }
                        for (u64 i = 0; i < f * suffix; ++i)
                        {
                            want[pbase + i] = ref[pbase + i];
                        }
                    }
                    for (u64 i = 0; i < suffix * f + 3U; ++i)
                    {
                        tile[i] = kGuard<T>;
                    }
                    ks.ttm_staged(w, range[0], range[1], tile, got);
                    bad += nan_aside_mismatches(want, got, osize + 3U);
                    bad += scratch_overruns(tile, suffix * f);
                }
            }
        }
    }
    return bad;
}

// out[i] += parts[t * osize + i] in part order: around a vector, a register pair and the 2048-value fold block,
// 1, 2 and 7 parts, signed zeros and infinities in out and the parts.
template <typename T> u64 check_sparse_fold(const tn::SparseTensorKernels<T>& ks)
{
    constexpr u64 lanes = tn::kEwLanes<T>;
    static T parts[7U * 4101U];
    static T want[4101U + 3U];
    static T got[4101U + 3U];
    u64 bad = 0;
    for (const u64 osize : {u64{1}, lanes - 1U, 2U * lanes + 1U, u64{2048}, u64{2049}, u64{4101}})
    {
        for (const u32 nparts : {1U, 2U, 7U})
        {
            fill_values(parts, nparts * osize, 0x5400U + osize + nparts, 5U);
            fill_values(want, osize, 0x5500U + osize, 3U);
            for (u64 i = osize; i < osize + 3U; ++i)
            {
                want[i] = kGuard<T>;
            }
            for (u64 i = 0; i < osize + 3U; ++i)
            {
                got[i] = want[i];
            }
            for (u32 t = 0; t < nparts; ++t)
            {
                for (u64 i = 0; i < osize; ++i)
                {
                    want[i] = want[i] + parts[static_cast<u64>(t) * osize + i];
                }
            }
            ks.fold(got, parts, osize, nparts);
            bad += nan_aside_mismatches(want, got, osize + 3U);
        }
    }
    return bad;
}

// The MTTKRP oracle at level L of the tree: a leaf adds fma(val, row, dst); an inner node's children sum into a zeroed
// acc, which then adds fma(acc, row, dst), its level's factor row. The level is a template argument, so no recursion
// reaches past the tree's kLevels (GCC's link-time inlining of a runtime level reported fids[5]).
template <u32 L, typename T>
void mttkrp_oracle(const CsfTree& t, u32 rank, const T* const* fac, const T* val, u64 r, u64 z, T* dst)
{
    const T* row = fac[L] + static_cast<u64>(t.fids[L][z]) * r;
    if (L + 1U == rank)
    {
        for (u64 q = 0; q < r; ++q)
        {
            dst[q] = crd::math::fma(val[z], row[q], dst[q]);
        }
        return;
    }
    if constexpr (L + 1U < CsfTree::kLevels)
    {
        T acc[16U * 8U + 1U];
        for (u64 q = 0; q < r; ++q)
        {
            acc[q] = T(0);
        }
        for (u64 ch = t.fptr[L][z]; ch < t.fptr[L][z + 1U]; ++ch)
        {
            mttkrp_oracle<L + 1U>(t, rank, fac, val, r, ch, acc);
        }
        for (u64 q = 0; q < r; ++q)
        {
            dst[q] = crd::math::fma(acc[q], row[q], dst[q]);
        }
    }
}

// Ranks 2 (leaves under the roots), 3 (one fused leaf level) and 5 (accumulator levels above it), r across the four-
// register chunk, one register and the tail at every width, the CSF walk over all roots and a sub-range, the COO walk
// against both, guard words after the output and the scratch rows.
template <typename T> u64 check_sparse_mttkrp(const tn::SparseTensorKernels<T>& ks)
{
    constexpr u64 lanes = tn::kEwLanes<T>;
    constexpr u64 max_r = 16U * lanes + 1U;
    static SparseCase c{};
    static CsfTree tree{};
    static T fac[5][7U * max_r];
    static T val[256];
    static T want[5U * max_r + 3U];
    static T got[5U * max_r + 3U];
    static T coo[5U * max_r + 3U];
    static T tmp[3U * max_r + 3U];
    const u32 ranks[3] = {2U, 3U, 5U};
    const u64 dims_of[3][5] = {{5U, 7U}, {4U, 5U, 6U}, {3U, 3U, 2U, 3U, 4U}};
    u64 bad = 0;
    for (u32 ci = 0; ci < 3U; ++ci)
    {
        const u32 rank = ranks[ci];
        make_sparse_case(c, rank, dims_of[ci], 0x5600U + rank);
        make_csf_tree(c, tree);
        fill_values(val, c.n, 0x5700U + rank, 7U);
        const u64 n0 = tree.nodes[0];
        const u64 rows = c.dims[0];
        const u64 tmp_len = (rank - 2U) * max_r;
        const u64 widths[] = {
            1U,   lanes - 1U, lanes, 2U * lanes - 1U, 4U * lanes - 1U, 4U * lanes, 4U * lanes + 1U, 8U * lanes + 3U,
            max_r};
        for (const u64 r : widths)
        {
            const T* facp[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
            for (u32 m = 1; m < rank; ++m)
            {
                fill_values(fac[m], c.dims[m] * r, 0x5800U + m * 37U + r, 9U);
                facp[m] = fac[m];
            }
            tn::MttkrpCsfWalk<T> w{};
            for (u32 l = 0; l < rank; ++l)
            {
                w.fids[l] = tree.fids[l];
                w.fptr[l] = l + 1U < rank ? tree.fptr[l] : nullptr;
                w.fac[l] = facp[l];
            }
            w.val = val;
            w.out = got;
            w.r = r;
            w.rank = rank;
            const u64 ranges[2][2] = {{0U, n0}, {1U, n0 - 1U}};
            for (const auto& range : ranges)
            {
                for (u64 i = 0; i < rows * r + 3U; ++i)
                {
                    want[i] = i < rows * r ? T(0) : kGuard<T>;
                    got[i] = want[i];
                }
                for (u64 i = 0; i < tmp_len + 3U; ++i)
                {
                    tmp[i] = kGuard<T>;
                }
                for (u64 z0 = range[0]; z0 < range[1]; ++z0)
                {
                    T* dst = want + static_cast<u64>(tree.fids[0][z0]) * r;
                    for (u64 ch = tree.fptr[0][z0]; ch < tree.fptr[0][z0 + 1U]; ++ch)
                    {
                        mttkrp_oracle<1U>(tree, rank, facp, val, r, ch, dst);
                    }
                }
                ks.mttkrp_csf(w, range[0], range[1], tmp);
                bad += nan_aside_mismatches(want, got, rows * r + 3U);
                bad += scratch_overruns(tmp, (rank - 2U) * r);
            }
            // the COO walk is the whole-range CSF walk's operations
            tn::MttkrpCooWalk<T> cw{};
            for (u32 m = 0; m < rank; ++m)
            {
                cw.planes[m] = c.idx[m];
                cw.fac[m] = facp[m];
            }
            cw.val = val;
            cw.out = coo;
            cw.n = c.n;
            cw.r = r;
            cw.rank = rank;
            for (u64 i = 0; i < rows * r + 3U; ++i)
            {
                coo[i] = i < rows * r ? T(0) : kGuard<T>;
                want[i] = coo[i];
            }
            for (u64 z0 = 0; z0 < n0; ++z0)
            {
                T* dst = want + static_cast<u64>(tree.fids[0][z0]) * r;
                for (u64 ch = tree.fptr[0][z0]; ch < tree.fptr[0][z0 + 1U]; ++ch)
                {
                    mttkrp_oracle<1U>(tree, rank, facp, val, r, ch, dst);
                }
            }
            ks.mttkrp_coo(cw, tmp);
            bad += nan_aside_mismatches(want, coo, rows * r + 3U);
            bad += scratch_overruns(tmp, (rank - 2U) * r);
        }
    }
    return bad;
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar sparse TTM, fold and MTTKRP bits",
          "[tensor][kernels]")
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(check_sparse_ttm<f32>(sparse_of<f32>(*level.table)) == 0U);
        CHECK(check_sparse_ttm<f64>(sparse_of<f64>(*level.table)) == 0U);
        CHECK(check_sparse_fold<f32>(sparse_of<f32>(*level.table)) == 0U);
        CHECK(check_sparse_fold<f64>(sparse_of<f64>(*level.table)) == 0U);
        CHECK(check_sparse_mttkrp<f32>(sparse_of<f32>(*level.table)) == 0U);
        CHECK(check_sparse_mttkrp<f64>(sparse_of<f64>(*level.table)) == 0U);
    }
    CHECK(ran >= 1U);
}

// --- nn's f32 pooling and convolution ---

// IEEE 754 maximum, the max pool's rule: a NaN wins, +0 over -0.
f32 ieee_max(f32 a, f32 b)
{
    if (std::isnan(a) || std::isnan(b))
    {
        return std::isnan(a) ? a : b;
    }
    if (a == b)
    {
        return std::signbit(a) ? b : a;
    }
    return a > b ? a : b;
}

// Values from {-2, -1, -0, +0, 1, 2}, so windows tie and mix signed zeros, about every 11th a fill_values special.
void fill_ties(f32* p, u64 n, u64 seed)
{
    const f32 small[] = {-2.0F, -1.0F, -0.0F, 0.0F, 1.0F, 2.0F};
    fill_values(p, n, seed, 11U);
    u64 s = seed ^ 0x9E37U;
    for (u64 i = 0; i < n; ++i)
    {
        const u64 r = lcg(s);
        if (r % 11U != 0U)
        {
            p[i] = small[(r >> 8U) % 6U];
        }
    }
}

// Three planes at heights 2 to 5 and widths around a vector, the 2x2/s2 shape's 256-wide row stage and past it, every
// window and stride (max and average), split into the ranges [1, 3) then [0, 1); guard words after y.
u64 check_nn_pool(const TensorKernels& k)
{
    constexpr u64 planes = 3U;
    static f32 x[planes * 5U * 257U];
    static f32 y[planes * 5U * 257U + 3U];
    static f32 want[planes * 5U * 257U];
    const u64 heights[] = {2U, 3U, 5U};
    const u64 widths[] = {2U, 3U, 9U, 16U, 17U, 255U, 256U, 257U};
    const u64 windows[][2] = {{2U, 2U}, {1U, 1U}, {2U, 1U}, {3U, 1U}, {3U, 2U}, {3U, 3U}};
    const bool modes[] = {true, false};
    u64 bad = 0;
    u64 seed = 41U;
    for (const u64 h : heights)
    {
        for (const u64 w : widths)
        {
            for (const auto& win : windows)
            {
                const u64 kk = win[0];
                const u64 st = win[1];
                if (h < kk || w < kk)
                {
                    continue;
                }
                const u64 oh = (h - kk) / st + 1U;
                const u64 ow = (w - kk) / st + 1U;
                const u64 total = planes * oh * ow;
                for (const bool is_max : modes)
                {
                    fill_ties(x, planes * h * w, ++seed);
                    for (u64 i = 0; i < total + 3U; ++i)
                    {
                        y[i] = kGuard<f32>;
                    }
                    const tn::NnPool2d c{x, y, h, w, oh, ow, kk, st, is_max};
                    k.nn_pool2d_planes(c, 1U, 3U);
                    for (u64 i = 0; i < oh * ow; ++i)
                    {
                        bad += y[i] == kGuard<f32> ? 0U : 1U;
                    }
                    k.nn_pool2d_planes(c, 0U, 1U);
                    const f32 inv = 1.0F / static_cast<f32>(kk * kk);
                    for (u64 p = 0; p < planes; ++p)
                    {
                        for (u64 oi = 0; oi < oh; ++oi)
                        {
                            for (u64 oj = 0; oj < ow; ++oj)
                            {
                                const f32* wp = x + p * h * w + oi * st * w + oj * st;
                                f32 acc = is_max ? wp[0] : 0.0F;
                                for (u64 ki = 0; ki < kk; ++ki)
                                {
                                    for (u64 kj = 0; kj < kk; ++kj)
                                    {
                                        acc = is_max ? ieee_max(acc, wp[ki * w + kj]) : acc + wp[ki * w + kj];
                                    }
                                }
                                want[(p * oh + oi) * ow + oj] = is_max ? acc : acc * inv;
                            }
                        }
                    }
                    bad += nan_aside_mismatches(want, y, total);
                    bad += scratch_overruns(y, total);
                }
            }
        }
    }
    return bad;
}

// The scalar convolution: per output bias + the (c, ki, kj)-ordered fma chain from 0, relu as `v > 0 ? v : 0`, the
// fused pool as the IEEE maximum of each 2x2/s2 window.
void conv_oracle(const tn::NnConv2d& c, u64 images, f32* conv, f32* out)
{
    for (u64 img = 0; img < images; ++img)
    {
        for (u64 o = 0; o < c.oc; ++o)
        {
            for (u64 oi = 0; oi < c.oh; ++oi)
            {
                for (u64 oj = 0; oj < c.ow; ++oj)
                {
                    f32 acc = 0.0F;
                    for (u64 ci = 0; ci < c.chans; ++ci)
                    {
                        for (u64 ki = 0; ki < c.kh; ++ki)
                        {
                            for (u64 kj = 0; kj < c.kw; ++kj)
                            {
                                const i64 ih = static_cast<i64>(oi * c.stride + ki) - static_cast<i64>(c.pad);
                                const i64 iw = static_cast<i64>(oj * c.stride + kj) - static_cast<i64>(c.pad);
                                const bool in =
                                    ih >= 0 && ih < static_cast<i64>(c.h) && iw >= 0 && iw < static_cast<i64>(c.w_in);
                                const u64 at = in ? ((img * c.chans + ci) * c.h + static_cast<u64>(ih)) * c.w_in +
                                                        static_cast<u64>(iw)
                                                  : 0U;
                                const f32 xv = in ? c.x[at] : 0.0F;
                                acc = crd::math::fma(c.w[((o * c.chans + ci) * c.kh + ki) * c.kw + kj], xv, acc);
                            }
                        }
                    }
                    f32 v = c.bias[o] + acc;
                    if (c.relu)
                    {
                        v = v > 0.0F ? v : 0.0F;
                    }
                    conv[((img * c.oc + o) * c.oh + oi) * c.ow + oj] = v;
                }
            }
        }
    }
    const u64 oh2 = c.pool ? (c.oh - 2U) / 2U + 1U : c.oh;
    const u64 ow2 = c.pool ? (c.ow - 2U) / 2U + 1U : c.ow;
    for (u64 pl = 0; pl < images * c.oc; ++pl)
    {
        const f32* src = conv + pl * c.oh * c.ow;
        for (u64 oi = 0; oi < oh2; ++oi)
        {
            for (u64 oj = 0; oj < ow2; ++oj)
            {
                f32 v = src[oi * c.ow + oj];
                if (c.pool)
                {
                    const f32* q = src + 2U * oi * c.ow + 2U * oj;
                    v = ieee_max(ieee_max(q[0], q[1]), ieee_max(q[c.ow], q[c.ow + 1U]));
                }
                out[(pl * oh2 + oi) * ow2 + oj] = v;
            }
        }
    }
}

// The direct 3x3/s1/p1 kernel at every output-channel block R 1 to 8 (and 17 = 8 + 8 + 1), heights 1 to 5 and widths
// around one and two vectors (rows of at most eight columns, avx512's two rows a register, at R 3, 6, 7, 8 and 8 + 8
// with even and odd heights), with and without relu and the fused pool; im2col at kernels 1 to 5 (and 1 x 3), pads 0
// to 2 and strides 1 to 3, its bias prefill below, inside and past a vector. Two images, split into [1, 2) then
// [0, 1); NaNs, infinities, signed zeros and subnormals in x, the weights and the bias; guard words after y and after
// the scratch slot.
u64 check_nn_conv(const TensorKernels& k)
{
    struct Shape
    {
        u64 chans, h, w, oc, kh, kw, pad, stride;
    };
    const Shape shapes[] = {
        {1U, 1U, 1U, 1U, 3U, 3U, 1U, 1U},  {2U, 3U, 7U, 3U, 3U, 3U, 1U, 1U},  {1U, 2U, 8U, 8U, 3U, 3U, 1U, 1U},
        {3U, 5U, 9U, 17U, 3U, 3U, 1U, 1U}, {2U, 4U, 15U, 5U, 3U, 3U, 1U, 1U}, {1U, 3U, 16U, 7U, 3U, 3U, 1U, 1U},
        {2U, 5U, 17U, 9U, 3U, 3U, 1U, 1U}, {1U, 4U, 31U, 2U, 3U, 3U, 1U, 1U}, {2U, 2U, 33U, 4U, 3U, 3U, 1U, 1U},
        {1U, 5U, 33U, 6U, 3U, 3U, 1U, 1U}, {2U, 5U, 6U, 3U, 1U, 1U, 0U, 1U},  {1U, 6U, 7U, 4U, 2U, 2U, 0U, 1U},
        {2U, 7U, 9U, 5U, 3U, 3U, 0U, 2U},  {1U, 8U, 8U, 3U, 5U, 5U, 2U, 1U},  {3U, 6U, 11U, 2U, 3U, 3U, 1U, 3U},
        {1U, 4U, 4U, 9U, 3U, 3U, 0U, 1U},  {2U, 5U, 7U, 3U, 1U, 3U, 1U, 1U},  {1U, 4U, 6U, 2U, 3U, 3U, 2U, 2U},
        {3U, 4U, 8U, 16U, 3U, 3U, 1U, 1U}, {2U, 5U, 5U, 6U, 3U, 3U, 1U, 1U},  {1U, 4U, 2U, 7U, 3U, 3U, 1U, 1U},
    };
    constexpr u64 images = 2U;
    static f32 x[images * 3U * 8U * 33U];
    static f32 wt[17U * 3U * 25U];
    static f32 bias[17U];
    static f32 y[images * 17U * 5U * 33U + 3U];
    static f32 conv[images * 17U * 5U * 33U];
    static f32 want[images * 17U * 5U * 33U];
    static f32 scratch[4096U];
    u64 bad = 0;
    u64 seed = 77U;
    for (const Shape& sh : shapes)
    {
        const u64 oh = (sh.h + 2U * sh.pad - sh.kh) / sh.stride + 1U;
        const u64 ow = (sh.w + 2U * sh.pad - sh.kw) / sh.stride + 1U;
        const bool direct = sh.kh == 3U && sh.kw == 3U && sh.pad == 1U && sh.stride == 1U;
        const u64 slot = tn::nn_conv2d_scratch_floats(sh.chans, sh.kh, sh.kw, oh, ow);
        for (u32 flags = 0; flags < 4U; ++flags)
        {
            const bool relu = (flags & 1U) != 0U;
            const bool pool = (flags & 2U) != 0U;
            if (pool && (!direct || sh.h < 2U || sh.w < 2U))
            {
                continue;
            }
            fill_values(x, images * sh.chans * sh.h * sh.w, ++seed, 23U);
            fill_values(wt, sh.oc * sh.chans * sh.kh * sh.kw, ++seed, 29U);
            fill_values(bias, sh.oc, ++seed, 7U);
            const tn::NnConv2d c{x,     wt,     bias,      y,     sh.chans, sh.h, sh.w, sh.kh,
                                 sh.kw, sh.pad, sh.stride, sh.oc, oh,       ow,   relu, pool};
            const u64 per = sh.oc * (pool ? ((oh - 2U) / 2U + 1U) * ((ow - 2U) / 2U + 1U) : oh * ow);
            for (u64 i = 0; i < images * per + 3U; ++i)
            {
                y[i] = kGuard<f32>;
            }
            for (u64 i = 0; i < slot + 3U; ++i)
            {
                scratch[i] = kGuard<f32>;
            }
            k.nn_conv2d_images(c, scratch, 1U, 2U);
            for (u64 i = 0; i < per; ++i)
            {
                bad += y[i] == kGuard<f32> ? 0U : 1U;
            }
            k.nn_conv2d_images(c, scratch, 0U, 1U);
            conv_oracle(c, images, conv, want);
            bad += nan_aside_mismatches(want, y, images * per);
            bad += scratch_overruns(y, images * per);
            bad += scratch_overruns(scratch, slot);
        }
    }
    return bad;
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar nn pooling and convolution bits",
          "[tensor][kernels]")
{
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO(crd::cpu_level_name(level.level) << variant_suffix(level));
        CHECK(check_nn_pool(*level.table) == 0U);
        CHECK(check_nn_conv(*level.table) == 0U);
    }
    CHECK(ran >= 1U);
}

// The activations' scalar formulas in general code, whose crd::math routines run the scalar table at the process
// level: relu maxps(x, +0), the erf-form gelu and sigmoid in f64, tanh in f32, each rounded once.
f32 act_reference(tn::NnAct act, f32 v)
{
    const f64 vd = static_cast<f64>(v);
    switch (act)
    {
        case tn::NnAct::Relu:
            return v > 0.0F ? v : 0.0F;
        case tn::NnAct::Gelu:
            return static_cast<f32>(0.5 * vd * (1.0 + crd::math::deterministic::erf(vd * 0.70710678118654752440)));
        case tn::NnAct::Tanh:
            return crd::math::tanh(v);
        case tn::NnAct::Sigmoid:
            return static_cast<f32>(1.0 / (1.0 + crd::math::exp(-vd)));
    }
    return 0.0F;
}

TEST_CASE("tensor kernels: every level the host runs gives the scalar nn activation bits", "[tensor][kernels]")
{
    // Five samples, the rows [1, 4) leaving the first and the last to their guard words; every third value an IEEE
    // special, a subnormal, a saturating or underflowing argument of tanh, sigmoid and gelu, or a near-zero one.
    constexpr u64 samples = 5U;
    constexpr u64 max_per = 37U;
    constexpr u64 total = samples * max_per + kSlack;
    const f32 specials[] = {kQNaN,   kInf,    -kInf,    0.0F,  -0.0F,  1e-40F, -1e-40F, 1e-30F,
                            -1e-30F, 20.0F,   -20.0F,   90.0F, -90.0F, 104.0F, -104.0F, -5.5F,
                            -40.0F,  3.4e38F, -3.4e38F, 9.0F,  -9.0F,  0.5F,   -0.5F};
    constexpr usize n_specials = sizeof(specials) / sizeof(specials[0]);
    static f32 x[total];
    static f32 want[total];
    static f32 got[total];
    usize ran = 0;
    for (const KernelLevel<TensorKernels>& level : tn::tensor_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        for (const tn::NnAct act : {tn::NnAct::Relu, tn::NnAct::Gelu, tn::NnAct::Tanh, tn::NnAct::Sigmoid})
        {
            for (const u64 per : {1U, 3U, 8U, 37U})
            {
                for (const bool in_place : {false, true})
                {
                    fill_uniform(x, total, 0xAC7U + per, 30.0F);
                    for (u64 i = 0; i < samples * per; i += 3U)
                    {
                        x[i] = specials[(i / 3U) % n_specials];
                    }
                    for (u64 i = 0; i < total; ++i)
                    {
                        const bool inside = i >= per && i < 4U * per;
                        want[i] = inside ? act_reference(act, x[i]) : kGuard<f32>;
                        got[i] = in_place && inside ? x[i] : kGuard<f32>;
                    }
                    const f32* src = in_place ? got : x;
                    level.table->nn_act_rows({src, got, per, act}, 1U, 4U);
                    INFO(crd::cpu_level_name(level.level) << variant_suffix(level) << " act " << static_cast<int>(act)
                                                          << " per " << per << (in_place ? " in place" : ""));
                    CHECK(nan_aside_mismatches(want, got, total) == 0U);
                }
            }
        }
    }
    CHECK(ran >= 1U);
}

} // namespace

TEST_CASE("tensor kernels: the process runs the level its CPU selection resolves", "[tensor][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = tn::tensor_kernel_levels();
    const KernelLevel<TensorKernels>& chosen = tn::tensor_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level)
                     << variant_suffix(chosen));
    CHECK(&chosen ==
          &crd::select_kernel_level<TensorKernels>(levels, s.selected, s.kernel_features, crd::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::kernel_level_cap(), chosen.level));
    CHECK(s.kernel_features.has_all(chosen.features));
#if CRD_TEST_KERNEL_LEVELS_TOP == CRD_KERNEL_LEVEL_AVX2 || CRD_TEST_KERNEL_LEVELS_TOP == CRD_KERNEL_LEVEL_AVX512
    // The avx2 tables (built only here): with usable AVX-VNNI the dpbusd dots, under a forced level the plain ones.
    if (chosen.level == CpuLevel::Avx2)
    {
        const bool vnni = s.kernel_features.has(crd::CpuFeature::AvxVnni);
        CHECK(chosen.table == (vnni ? &tn::kTensorKernelsAvx2AvxVnni : &tn::kTensorKernelsAvx2));
    }
#endif
}
