// The known-answer self-check of the hesap-tensor kernels (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp): every one
// of the 78 routines of a table (the permutes, the binned sum, the quantizers, the Philox blocks, the elementwise ops,
// Tier D, einsum's products, the batched linear algebra, the sparse walks and nn's rows), each on a few shapes that run
// its vector bodies at the widest level, their tails and its edge paths. Every routine gives the same bits on every
// level (tensor_kernels.hpp), so each answer is one FNV-1a digest of the portable table's outputs; the differential
// cases of test_tensor_kernels.cpp hold every table to the scalar oracles. The int8 linear's row-major and packed
// weights give one digest, as their contract says.
//
// The inputs are k / 7 for nonzero k in [-48, 48] from a fixed generator: no zero or NaN, and sums and products that
// round, so a step taken in another order changes bits. Every output buffer starts from a guard value and is digested
// whole, so a stray write or a missed one fails too; a NaN folds as one canonical NaN (payloads are no part of the
// contract). Nothing here reaches tensor_kernels(), whose first call runs this check. Each group runs in its own frame
// of under 12 KiB of stack (the scalar-tier LU and Cholesky at n = 33 in f64), well within a job fiber's 64 KiB; the
// 4,224-value inputs (a Tier D row of two blocks) are one function-local table per type; nothing is allocated.

#include <crd/core/fnv1a.hpp>
#include <crd/core/platform.hpp>
#include <crd/hesap/tensor/binned_core.hpp>
#include <crd/hesap/tensor/nn.hpp> // nndetail::i8_pack_weights
#include <crd/hesap/tensor/quant_core.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/math/float_convert_scalar.hpp>

#include <bit>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace crd::hesap::tensor
{
namespace
{

using crd::f32;
using crd::f64;
using crd::i32;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;

// ---- Routines and answers -------------------------------------------------------------------------------------------

// The routines' first answers, in check order; a pair of adjacent entries is f32 then f64 unless named otherwise.
constexpr usize kPermuteRegion = 0U; // f32, f64
constexpr usize kPermuteWalk = 2U;   // f32, f64
constexpr usize kBinnedSum = 4U;
constexpr usize kQuantizeQ8 = 5U;
constexpr usize kQuantizeU8 = 6U;
constexpr usize kSoftmax = 7U;
constexpr usize kLayerNorm = 8U;
constexpr usize kActivations = 9U;
constexpr usize kLinearQ8 = 10U;
constexpr usize kLinearI8 = 11U;
constexpr usize kLinearI8Packed = 12U;
constexpr usize kPhilox = 13U;
constexpr usize kEwBinary = 14U;     // f32's six ops, then f64's
constexpr usize kReduce = 26U;       // per type, per op: block, rows, columns
constexpr usize kEinsumThin = 50U;   // f32, f64
constexpr usize kEinsumSmallM = 52U; // f32, f64
constexpr usize kBatched = 54U;      // per type: gemm_tiny, cholesky, lu, svd, cholesky_solve, lu_solve
constexpr usize kSparseWalks = 66U;  // per type: ttm_scatter, ttm_staged, fold, mttkrp_csf, mttkrp_coo
constexpr usize kPool2d = 76U;
constexpr usize kConv2d = 77U;
constexpr usize kRoutines = 78U;

struct Answer
{
    const char* name;
    u64 digest;
};

constexpr Answer kAnswers[kRoutines] = {
    {"f32 permute_region", 0x9820EC1830479A5FULL},
    {"f64 permute_region", 0x58F95A5E763F91D7ULL},
    {"f32 permute_walk", 0xC7858E619FDD151AULL},
    {"f64 permute_walk", 0xD85E67F13D4B123DULL},
    {"binned_sum_f64", 0x87C2C3D649B96208ULL},
    {"quantize_q8_0", 0x0D4111870F60D77DULL},
    {"quantize_u8_row", 0x906D46E7F64AB060ULL},
    {"nn_softmax_rows", 0x9A486D26091B0202ULL},
    {"nn_layernorm_rows", 0xF57BF621CB00FC6AULL},
    {"nn_act_rows", 0xECEA494D21770237ULL},
    {"nn_linear_q8_rows", 0xCD0A27953D056BDEULL},
    {"nn_linear_i8_rows", 0xEAD725A963BD8929ULL},
    {"nn_linear_i8_packed_rows", 0xEAD725A963BD8929ULL},
    {"philox4x32_blocks", 0x60C33F48C0D6B767ULL},
    {"f32 ew_binary add", 0x27D6D18AAEA92255ULL},
    {"f32 ew_binary sub", 0x0C3E985A3E1E70DCULL},
    {"f32 ew_binary mul", 0x8D143769FC2DBB08ULL},
    {"f32 ew_binary div", 0xEBFAE1F757C694AFULL},
    {"f32 ew_binary min", 0x42AF1722DCFE3310ULL},
    {"f32 ew_binary max", 0x872A76D55A8F51B3ULL},
    {"f64 ew_binary add", 0xB7A14356D20426A5ULL},
    {"f64 ew_binary sub", 0xEBA79C8BD029ADB5ULL},
    {"f64 ew_binary mul", 0xAA077D4CE4C22A7CULL},
    {"f64 ew_binary div", 0x54A102269C62AFAFULL},
    {"f64 ew_binary min", 0x887CF551FB5002DFULL},
    {"f64 ew_binary max", 0x35CD5B1CCD9A961CULL},
    {"f32 reduce sum block", 0xD2802D3DD0DDE96AULL},
    {"f32 reduce sum rows", 0xDD2D34A998B79981ULL},
    {"f32 reduce sum columns", 0x3CCE2D9B951D9EDAULL},
    {"f32 reduce prod block", 0x34DA571229B9DB09ULL},
    {"f32 reduce prod rows", 0x94D181838C2E51FEULL},
    {"f32 reduce prod columns", 0x5B755A0D83C6B80AULL},
    {"f32 reduce min block", 0xAA4274B2312808B5ULL},
    {"f32 reduce min rows", 0x4EEB77BA16823D16ULL},
    {"f32 reduce min columns", 0xACC2599C5428F6A5ULL},
    {"f32 reduce max block", 0x3DE11EA3FE1CF5DBULL},
    {"f32 reduce max rows", 0x000A9E622657820DULL},
    {"f32 reduce max columns", 0x2ADC8E79956304EBULL},
    {"f64 reduce sum block", 0x8FB7BA2E6944DC22ULL},
    {"f64 reduce sum rows", 0x96172DD0310300AFULL},
    {"f64 reduce sum columns", 0x68502EE70AD82E3BULL},
    {"f64 reduce prod block", 0x44210BF4C6FF86BAULL},
    {"f64 reduce prod rows", 0x5691D21902493988ULL},
    {"f64 reduce prod columns", 0x6D82D22B0F90C31AULL},
    {"f64 reduce min block", 0x8121D07DAC29E061ULL},
    {"f64 reduce min rows", 0x79C1A0C03C7D8408ULL},
    {"f64 reduce min columns", 0x6A00476BA1224F14ULL},
    {"f64 reduce max block", 0x5061DD1CB05C3615ULL},
    {"f64 reduce max rows", 0x8AA81325624E5EE5ULL},
    {"f64 reduce max columns", 0xF08D452F8F456047ULL},
    {"einsum_thin_f32", 0xF051F243DE106E6EULL},
    {"einsum_thin_f64", 0x5B8559CC35CC37DBULL},
    {"einsum_smallm_f32", 0x81E9F9806E5A0965ULL},
    {"einsum_smallm_f64", 0x8399D9434FC8D275ULL},
    {"f32 batched gemm_tiny", 0x518104A203BC81D0ULL},
    {"f32 batched cholesky", 0x0D50B650E63F2C93ULL},
    {"f32 batched lu", 0xA820E7433706B45AULL},
    {"f32 batched svd", 0x8F98BED9AB59E445ULL},
    {"f32 batched cholesky_solve", 0x8F0A4D964B233D63ULL},
    {"f32 batched lu_solve", 0x1983EFFBA4475186ULL},
    {"f64 batched gemm_tiny", 0x395D4FB3DBE2CFCDULL},
    {"f64 batched cholesky", 0xCF4B0AB9D64D35CDULL},
    {"f64 batched lu", 0x68B095D3F1D494ADULL},
    {"f64 batched svd", 0x6E317550F479075FULL},
    {"f64 batched cholesky_solve", 0xF5E6538718C8626AULL},
    {"f64 batched lu_solve", 0xB9107C16B941E7E7ULL},
    {"f32 sparse ttm_scatter", 0xD106C3D4FD3FFDA6ULL},
    {"f32 sparse ttm_staged", 0xB38CC00AA90248D9ULL},
    {"f32 sparse fold", 0xD0CFA69C222E462CULL},
    {"f32 sparse mttkrp_csf", 0x4FDC6442C899B9CCULL},
    {"f32 sparse mttkrp_coo", 0x5F0582C60287DD64ULL},
    {"f64 sparse ttm_scatter", 0xC3F4738DF8280198ULL},
    {"f64 sparse ttm_staged", 0x81CE220E4F9728C9ULL},
    {"f64 sparse fold", 0x1BF60D7FEB0B6B7DULL},
    {"f64 sparse mttkrp_csf", 0x7AD7DE5ACCD086EDULL},
    {"f64 sparse mttkrp_coo", 0xFAC0655751849160ULL},
    {"nn_pool2d_planes", 0x40A183FC777FA5E3ULL},
    {"nn_conv2d_images", 0x0CDFB11192A56E58ULL},
};

// What a table gave, in kAnswers order.
struct Digests
{
    u64 at[kRoutines];
};

// 0 for f32, 1 for f64: the offset of an element type's entry in a pair or block of routines.
template <typename T> constexpr usize kType = std::is_same_v<T, f64> ? 1U : 0U;

// ---- Inputs and digests ---------------------------------------------------------------------------------------------

constexpr usize kInputCount = detail::kReduceBlock + 128U;

template <typename T> struct Inputs
{
    T v[kInputCount];
};

template <typename T> constexpr Inputs<T> make_inputs() noexcept
{
    Inputs<T> in{};
    u32 s = 0x2545F491U;
    for (usize i = 0U; i < kInputCount; ++i)
    {
        s = s * 1664525U + 1013904223U;
        const i32 k = static_cast<i32>((s >> 16U) % 96U) - 48; // [-48, 47], 0 taken as 48
        in.v[i] = static_cast<T>(k == 0 ? 48 : k) / static_cast<T>(7);
    }
    return in;
}

// The inputs of type T: initialized on first use (constant-initialized where the compiler evaluates it).
template <typename T> const T* inputs() noexcept
{
    static const Inputs<T> kIn = make_inputs<T>();
    return kIn.v;
}

// A finite value no routine writes, around every output.
template <typename T> constexpr T kGuard = static_cast<T>(-7777.25);

template <typename T> void fill(T* p, usize n, T v) noexcept
{
    for (usize i = 0U; i < n; ++i)
    {
        p[i] = v;
    }
}

struct Digest
{
    u64 h = crd::kFnv1a64Offset;

    void add(f32 v) noexcept
    {
        const u32 bits = std::bit_cast<u32>(v);
        h = crd::fnv1a_64_le((bits & 0x7FFFFFFFU) > 0x7F800000U ? 0x7FC00000U : bits, h);
    }

    void add(f64 v) noexcept
    {
        const u64 bits = std::bit_cast<u64>(v);
        h = crd::fnv1a_64_le((bits & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL ? 0x7FF8000000000000ULL : bits, h);
    }

    void add(i32 v) noexcept { h = crd::fnv1a_64_le(static_cast<u32>(v), h); }

    template <typename T> void add(const T* p, usize n) noexcept
    {
        for (usize i = 0U; i < n; ++i)
        {
            add(p[i]);
        }
    }

    void bytes(const void* p, usize n) noexcept { h = crd::fnv1a_64(p, n, h); }
};

// ---- Permutes -------------------------------------------------------------------------------------------------------

template <typename T> void permute_region(const TensorKernels& k, const PermuteRegion<T>& r) noexcept
{
    if constexpr (std::is_same_v<T, f32>)
    {
        k.permute_region_f32(r);
    }
    else
    {
        k.permute_region_f64(r);
    }
}

template <typename T> void permute_walk(const TensorKernels& k, const PermuteWalk<T>& w) noexcept
{
    if constexpr (std::is_same_v<T, f32>)
    {
        k.permute_walk_f32(w);
    }
    else
    {
        k.permute_walk_f64(w);
    }
}

// The regions: a plain copy whose tiles cut both extents, a real scale through a staged strip with a next region and
// HPTT's levers, a complex scale, non-temporal stores on a 64-byte line, and the scalar tile over non-unit strides.
// The walks: a rank-3 transpose with tiles that cut it, real and complex, non-temporal stores, and the scalar tile.
template <typename T> CRD_NOINLINE void check_permutes(const TensorKernels& k, Digests& got) noexcept
{
    constexpr usize dst_len = 2U * 360U + 32U; // the largest case (a complex walk) and its guards
    alignas(64) T dst[dst_len];
    alignas(64) T stage[2U * 8U * 8U];
    const T* x = inputs<T>();
    const T third = static_cast<T>(-1) / static_cast<T>(3);
    constexpr u64 nt = 32U / sizeof(T); // a 32-byte row
    constexpr i64 nt2 = static_cast<i64>(2U * nt);
    Digest region;
    const PermutePick hptt{PermuteHint::T0, 2U, false, false, PermuteOrder::Hptt};
    const PermutePick scalar{PermuteHint::T1, 1U, true, true, PermuteOrder::Src};
    const PermuteRegion<T> regions[] = {
        {dst, 24, x, 1, 23, 19U, 21U, 16U, 16U, T(1), T(0), false, false, false, {}, {}, nullptr},
        {dst, 24, x, 1, 23, 19U, 21U, 8U, 8U, third, T(0), false, true, false, {0, 0, 19U, 21U}, hptt, stage},
        {dst, 12, x, 1, 11, 9U, 10U, 4U, 8U, T(0.5), third, true, true, false, {}, {}, nullptr},
        {dst, nt2, x, 1, 17, 17U, 2U * nt, 64U, 64U, T(1.5), T(0), false, true, true, {}, {}, nullptr},
        {dst, 9, x, 2, 30, 13U, 7U, 64U, 64U, T(3), T(0), false, true, false, {}, scalar, nullptr},
    };
    for (const PermuteRegion<T>& r : regions)
    {
        fill(dst, dst_len, kGuard<T>);
        permute_region(k, r);
        region.add(dst, dst_len);
    }
    got.at[kPermuteRegion + kType<T>] = region.h;

    Digest walk;
    const u64 shp3[] = {9U, 4U, 10U};
    const i64 in3[] = {1, 90, 9};
    const i64 out3[] = {40, 10, 1};
    const u64 shp2[] = {12U, 4U * nt};
    const i64 in2[] = {1, 12};
    const i64 out2[] = {static_cast<i64>(4U * nt), 1};
    const PermuteWalk<T> walks[] = {
        {dst, x, shp3, in3, out3, 3U, 0U, 8U, T(1), T(0), false, false, false, {}},
        {dst, x, shp3, in3, out3, 3U, 0U, 8U, T(0.5), third, true, true, false, hptt},
        {dst, x, shp2, in2, out2, 2U, 0U, 64U, T(1.5), T(0), false, true, true, hptt},
        {dst, x, shp3, in3, out3, 3U, 0U, 64U, third, T(0), false, true, false, scalar},
    };
    for (const PermuteWalk<T>& w : walks)
    {
        fill(dst, dst_len, kGuard<T>);
        permute_walk(k, w);
        walk.add(dst, dst_len);
    }
    got.at[kPermuteWalk + kType<T>] = walk.h;
}

// ---- Binned sum, quantizers, Philox ---------------------------------------------------------------------------------

// The binned sum past every body's gate (8 steps of 24) with a tail, merged into a non-empty accumulator, and the whole
// input; the Q8_0 and u8 quantizers over four blocks and a 96-value row; Philox blocks over a 2^32 crossing of the
// position, with a partial vector, and from 0.
CRD_NOINLINE void check_bulk(const TensorKernels& k, Digests& got) noexcept
{
    const f64* x64 = inputs<f64>();
    const f32* x32 = inputs<f32>();
    Digest binned;
    detail::BinnedRungs acc{};
    const u64 runs[][2] = {{0U, 215U}, {215U, 3U}, {300U, kInputCount - 300U}};
    for (const auto& run : runs)
    {
        k.binned_sum_f64(acc, x64 + run[0], run[1]);
        binned.add(acc.pri, static_cast<usize>(detail::kBinFold));
        binned.add(acc.car, static_cast<usize>(detail::kBinFold));
    }
    got.at[kBinnedSum] = binned.h;

    BlockQ8Zero blocks[5];
    std::memset(blocks, 0x77, sizeof(blocks)); // the fifth block is a guard
    Digest q8;
    k.quantize_q8_0(x32 + 7U, blocks, 4U);
    q8.bytes(blocks, sizeof(blocks));
    got.at[kQuantizeQ8] = q8.h;

    crd::u8 q[97];
    q[96] = 0x77U;
    f32 scale = kGuard<f32>;
    i32 zero = -7;
    k.quantize_u8_row(x32 + 11U, 96U, q, scale, zero);
    Digest u8;
    u8.bytes(q, sizeof(q));
    u8.add(scale);
    u8.add(zero);
    got.at[kQuantizeU8] = u8.h;

    u32 words[4U * 23U + 1U];
    Digest philox;
    const u64 firsts[][2] = {{0xFFFFFFF9ULL, 23U}, {0U, 5U}};
    for (const auto& f : firsts)
    {
        fill(words, std::size(words), 0x77777777U);
        k.philox4x32_blocks(0xDEADBEEFCAFEF00DULL, 0x0123456789ABCDEFULL, f[0], words, static_cast<usize>(f[1]));
        philox.bytes(words, sizeof(words));
    }
    got.at[kPhilox] = philox.h;
}

// ---- nn rows --------------------------------------------------------------------------------------------------------

// Softmax and layernorm over rows of 37 and 8 values, the four activations over two samples of 37 (one in place), and
// the Q8_0 and int8 linears at K = 64 over 19 outputs (both int8 weight layouts, with and without ReLU), each over a
// row range that leaves its first row out.
CRD_NOINLINE void check_nn_rows(const TensorKernels& k, Digests& got) noexcept
{
    constexpr u64 m_rows = 7U;
    constexpr u64 depth = 64U;
    constexpr u64 outs = 19U;
    constexpr u64 outs8 = 24U;
    constexpr usize out_len = 3U * 37U + 4U;
    const f32* x = inputs<f32>();
    f32 y[m_rows * outs + 4U];

    Digest softmax;
    Digest layernorm;
    for (const u64 d : {37U, 8U})
    {
        fill(y, out_len, kGuard<f32>);
        k.nn_softmax_rows({x, y, d}, 1U, 3U);
        softmax.add(y, out_len);
        fill(y, out_len, kGuard<f32>);
        k.nn_layernorm_rows({x + 5U, x + 200U, x + 300U, y, d, 1e-5F}, 1U, 3U);
        layernorm.add(y, out_len);
    }
    got.at[kSoftmax] = softmax.h;
    got.at[kLayerNorm] = layernorm.h;

    Digest act;
    for (const NnAct a : {NnAct::Relu, NnAct::Gelu, NnAct::Tanh, NnAct::Sigmoid})
    {
        fill(y, out_len, kGuard<f32>);
        k.nn_act_rows({x + 400U, y, 37U, a}, 1U, 3U);
        act.add(y, out_len);
        for (usize i = 0U; i < 37U; ++i)
        {
            y[i] = x[500U + i];
        }
        k.nn_act_rows({y, y, 37U, a}, 0U, 1U); // in place
        act.add(y, out_len);
    }
    got.at[kActivations] = act.h;

    // Q8_0: weight blocks of every i8 value by a fixed walk, scales the f16 values the blocks name.
    BlockQ8Zero wq[outs * (depth / 32U)];
    f32 wscale[outs * (depth / 32U)];
    BlockQ8Zero xq[m_rows * (depth / 32U)];
    f32 xscale[m_rows * (depth / 32U)];
    for (usize b = 0U; b < std::size(wq); ++b)
    {
        wq[b].d = crd::math::f32_to_f16_bits(x[600U + b] * 1e-2F);
        wscale[b] = crd::math::f16_bits_to_f32(wq[b].d);
        for (usize j = 0U; j < 32U; ++j)
        {
            wq[b].qs[j] = static_cast<crd::i8>(static_cast<i32>((b * 37U + j * 11U) % 256U) - 128);
        }
    }
    Digest q8;
    for (const bool relu : {false, true})
    {
        fill(y, std::size(y), kGuard<f32>);
        k.nn_linear_q8_rows({x, wq, wscale, x + 700U, y, xq, xscale, depth, outs, relu}, 1U, static_cast<u32>(m_rows));
        q8.add(y, std::size(y));
        q8.bytes(xq + depth / 32U, (m_rows - 1U) * (depth / 32U) * sizeof(BlockQ8Zero));
    }
    got.at[kLinearQ8] = q8.h;

    crd::i8 w8[outs * depth];
    crd::i8 packed[outs8 * depth];
    i32 colsum[outs8] = {};
    f32 bias[outs8];
    for (u64 o = 0U; o < outs8; ++o)
    {
        bias[o] = x[800U + o];
        for (u64 c = 0U; o < outs && c < depth; ++c)
        {
            w8[o * depth + c] = static_cast<crd::i8>(static_cast<i32>((o * 53U + c * 29U) % 255U) - 127);
            colsum[o] += w8[o * depth + c];
        }
    }
    nndetail::i8_pack_weights(w8, outs, depth, packed);
    crd::u8 xq8[m_rows * depth];
    f32 xs[m_rows];
    i32 xzp[m_rows];
    Digest int8[2];
    for (const bool use_packed : {false, true})
    {
        for (const bool relu : {false, true})
        {
            fill(y, std::size(y), kGuard<f32>);
            const crd::i8* weights = use_packed ? packed : w8;
            const NnLinearI8 c{x + 900U, weights, colsum, bias, y, xq8, xs, xzp, 0.0123F, depth, outs, relu};
            if (use_packed)
            {
                k.nn_linear_i8_packed_rows(c, 1U, static_cast<u32>(m_rows));
            }
            else
            {
                k.nn_linear_i8_rows(c, 1U, static_cast<u32>(m_rows));
            }
            Digest& d = int8[use_packed ? 1U : 0U];
            d.add(y, std::size(y));
            d.bytes(xq8 + depth, (m_rows - 1U) * depth);
            d.add(xs + 1, m_rows - 1U);
            d.add(xzp + 1, m_rows - 1U);
        }
    }
    got.at[kLinearI8] = int8[0].h;
    got.at[kLinearI8Packed] = int8[1].h;
}

// Max and average pools at 2x2/s2 and 3x3/s1 over three planes of 5 x 19, the first plane outside the range; the
// direct 3x3/s1/p1 convolution with ReLU and the fused pool and without either, and an im2col shape (3x3, stride 2,
// no pad), over two images.
CRD_NOINLINE void check_nn_planes(const TensorKernels& k, Digests& got) noexcept
{
    const f32* x = inputs<f32>();
    constexpr usize pool_len = 3U * 5U * 19U + 4U;
    Digest pool;
    {
        f32 y[pool_len];
        const u64 windows[][2] = {{2U, 2U}, {3U, 1U}};
        for (const auto& win : windows)
        {
            for (const bool is_max : {true, false})
            {
                fill(y, pool_len, kGuard<f32>);
                const u64 oh = (5U - win[0]) / win[1] + 1U;
                const u64 ow = (19U - win[0]) / win[1] + 1U;
                k.nn_pool2d_planes({x, y, 5U, 19U, oh, ow, win[0], win[1], is_max}, 1U, 3U);
                pool.add(y, pool_len);
            }
        }
    }
    got.at[kPool2d] = pool.h;

    struct Shape
    {
        u64 chans, h, w, oc, kh, pad, stride;
        bool relu, pool;
    };
    constexpr Shape shapes[] = {
        {2U, 4U, 17U, 9U, 3U, 1U, 1U, true, true},
        {2U, 4U, 17U, 9U, 3U, 1U, 1U, false, false},
        {2U, 5U, 7U, 3U, 3U, 0U, 2U, true, false},
    };
    constexpr usize conv_len = 2U * 9U * 4U * 17U + 4U;
    f32 y[conv_len];
    f32 scratch[nn_conv2d_scratch_floats(2U, 3U, 3U, 4U, 17U) + 4U];
    Digest conv;
    for (const Shape& s : shapes)
    {
        const u64 oh = (s.h + 2U * s.pad - s.kh) / s.stride + 1U;
        const u64 ow = (s.w + 2U * s.pad - s.kh) / s.stride + 1U;
        fill(y, conv_len, kGuard<f32>);
        fill(scratch, std::size(scratch), kGuard<f32>);
        const NnConv2d c{x,    x + 1000U, x + 2000U, y,    s.chans, s.h, s.w,    s.kh,
                         s.kh, s.pad,     s.stride,  s.oc, oh,      ow,  s.relu, s.pool};
        k.nn_conv2d_images(c, scratch, 0U, 2U);
        conv.add(y, conv_len);
        conv.add(scratch + nn_conv2d_scratch_floats(s.chans, s.kh, s.kh, oh, ow), 4U);
    }
    got.at[kConv2d] = conv.h;
}

// ---- Elementwise, Tier D, einsum ------------------------------------------------------------------------------------

template <typename T> using EwEntry = void (*)(const EwBinaryWalk<T>&) noexcept;

template <typename T> EwEntry<T> ew_entry(const TensorKernels& k, u32 op) noexcept
{
    if constexpr (std::is_same_v<T, f32>)
    {
        return k.ew_binary_f32[op];
    }
    else
    {
        return k.ew_binary_f64[op];
    }
}

template <typename T> const ReduceKernels<T>& reduce_entry(const TensorKernels& k, u32 op) noexcept
{
    if constexpr (std::is_same_v<T, f32>)
    {
        return k.reduce_f32[op];
    }
    else
    {
        return k.reduce_f64[op];
    }
}

// Each op over 37 values (both operands stepping), a reversed operand against a broadcast one, and a rank-3 walk that
// broadcasts b over its first dim. Tier D: a block of 173 values (two steps of the widest level's four accumulators, a
// group and a tail), a row of two blocks and three short rows, and the columns [2, 35) of three rows of 37.
template <typename T> CRD_NOINLINE void check_ew_reduce(const TensorKernels& k, Digests& got) noexcept
{
    const T* x = inputs<T>();
    constexpr usize out_len = 2U * 3U * 37U + 4U;
    T y[out_len];
    const u64 n1[] = {37U};
    const i64 one[] = {1};
    const i64 back[] = {-1};
    const i64 none[] = {0};
    const u64 n3[] = {2U, 3U, 37U};
    const i64 a3[] = {111, 37, 1};
    const i64 b3[] = {0, 37, 1};
    for (u32 op = 0U; op < kBinaryOps; ++op)
    {
        const EwBinaryWalk<T> walks[] = {
            {y, x, x + 41, n1, one, one, 1U},
            {y, x + 136, x + 7, n1, back, none, 1U},
            {y, x + 3, x + 500, n3, a3, b3, 3U},
        };
        Digest d;
        for (const EwBinaryWalk<T>& w : walks)
        {
            fill(y, out_len, kGuard<T>);
            ew_entry<T>(k, op)(w);
            d.add(y, out_len);
        }
        got.at[kEwBinary + kType<T> * kBinaryOps + op] = d.h;
    }

    for (u32 op = 0U; op < kReduceKernelOps; ++op)
    {
        const ReduceKernels<T>& r = reduce_entry<T>(k, op);
        const usize at = kReduce + kType<T> * 3U * kReduceKernelOps + 3U * op;
        Digest block;
        block.add(r.block(x, 173U, op == 1U ? T(1) : x[200]));
        block.add(r.block(x + 9, 5U, op == 1U ? T(1) : x[201]));
        got.at[at] = block.h;

        Digest rows;
        fill(y, out_len, kGuard<T>);
        r.rows(y, x, detail::kReduceBlock + 37U, 0U, 1U);
        r.rows(y + 2, x, 37U, 1U, 4U);
        rows.add(y, 8U);
        got.at[at + 1U] = rows.h;

        Digest columns;
        fill(y, out_len, kGuard<T>);
        r.columns(y, x, 3U, 37U, 2U, 35U);
        columns.add(y, 41U);
        got.at[at + 2U] = columns.h;
    }
}

// The thin product at m 9, k 7 and n 29 (four lane groups of f64 and a tail), the small-m product at n 49 both ways.
template <typename T> CRD_NOINLINE void check_einsum(const TensorKernels& k, Digests& got) noexcept
{
    const T* x = inputs<T>();
    constexpr usize out_len = 9U * 49U + 4U;
    T c[out_len];
    Digest thin;
    fill(c, out_len, kGuard<T>);
    if constexpr (std::is_same_v<T, f32>)
    {
        k.einsum_thin_f32(9U, 7U, 29U, x, x + 100, c);
    }
    else
    {
        k.einsum_thin_f64(9U, 7U, 29U, x, x + 100, c);
    }
    thin.add(c, out_len);
    Digest smallm;
    for (const bool a_trans : {false, true})
    {
        fill(c, out_len, kGuard<T>);
        if constexpr (std::is_same_v<T, f32>)
        {
            k.einsum_smallm_f32(9U, 7U, 49U, x + 1000, x + 2000, c, a_trans);
        }
        else
        {
            k.einsum_smallm_f64(9U, 7U, 49U, x + 1000, x + 2000, c, a_trans);
        }
        smallm.add(c, out_len);
    }
    got.at[kEinsumThin + kType<T>] = thin.h;
    got.at[kEinsumSmallM + kType<T>] = smallm.h;
}

// ---- Batched linear algebra -----------------------------------------------------------------------------------------

template <typename T> const BatchedKernels<T>& batched_of(const TensorKernels& k) noexcept
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

constexpr u64 kBatch = 19U; // two lane groups of f32 and a tail, four of f64 and a tail
constexpr u64 kSmallN = 5U;
constexpr u64 kScalarN = kBatchedCholMaxN + 1U; // the scalar tier

// Diagonally dominant matrices, symmetric for Cholesky, at stride n * n + 1 (a guard after each).
template <typename T> void batched_matrices(T* a, u64 batch, u64 n, bool symmetric) noexcept
{
    const T* x = inputs<T>();
    const u64 s = n * n + 1U;
    for (u64 b = 0U; b < batch; ++b)
    {
        for (u64 i = 0U; i < n; ++i)
        {
            for (u64 j = 0U; j < n; ++j)
            {
                const u64 lo = i < j ? i : j;
                const u64 hi = i < j ? j : i;
                const u64 at = symmetric ? b * 97U + lo * 13U + hi * 5U : b * 97U + i * 13U + j;
                const T v = x[at % kInputCount];
                a[b * s + i * n + j] = i == j ? v + static_cast<T>(8 * n) : v;
            }
        }
        a[b * s + n * n] = kGuard<T>;
    }
}

// The tiny GEMM (m 5, k 3, n 9) with beta 0 and beta 1/2 over the batch in two ranges that split a group.
template <typename T> CRD_NOINLINE void check_batched_gemm(const BatchedKernels<T>& kb, Digests& got) noexcept
{
    const T* x = inputs<T>();
    constexpr usize c_len = kBatch * 45U + 4U;
    T c[c_len];
    Digest d;
    for (const T beta : {T(0), T(0.5)})
    {
        for (usize i = 0U; i < c_len; ++i)
        {
            c[i] = i < kBatch * 45U ? x[3000U + i] : kGuard<T>;
        }
        const BatchedGemm<T> g{static_cast<T>(-1) / static_cast<T>(3), beta, x, x + 400, c, 15, 27, 45, 5U, 3U, 9U};
        kb.gemm_tiny(g, 0U, 7U);
        kb.gemm_tiny(g, 7U, kBatch);
        d.add(c, c_len);
    }
    got.at[kBatched + kType<T> * 6U] = d.h;
}

// Cholesky and LU at n 5 (the lane tier) over the batch in two ranges, and at n 33 (the scalar tier); the solves of
// three right-hand sides with the lane tier's factors.
template <typename T> CRD_NOINLINE void check_batched_factor(const BatchedKernels<T>& kb, Digests& got) noexcept
{
    const T* x = inputs<T>();
    constexpr u64 stride = kSmallN * kSmallN + 1U;
    constexpr u64 big_len = kScalarN * kScalarN + 1U;
    T a[big_len > kBatch * stride ? big_len : kBatch * stride];
    i32 info[kBatch];
    i32 piv[kBatch * kSmallN > kScalarN ? kBatch * kSmallN : kScalarN];
    constexpr u64 rhs_cols = 3U;
    constexpr u64 rhs_stride = kSmallN * rhs_cols + 1U;
    T rhs[kBatch * rhs_stride];
    for (const bool lu : {false, true})
    {
        Digest factor;
        batched_matrices(a, 1U, kScalarN, !lu);
        BatchedFactor<T> f{a, static_cast<i64>(big_len), info, piv, kScalarN};
        (lu ? kb.lu : kb.cholesky)(f, 0U, 1U);
        factor.add(a, big_len);
        factor.add(info[0]);
        factor.add(piv, lu ? kScalarN : 0U);

        batched_matrices(a, kBatch, kSmallN, !lu);
        f = {a, static_cast<i64>(stride), info, piv, kSmallN};
        (lu ? kb.lu : kb.cholesky)(f, 0U, 7U);
        (lu ? kb.lu : kb.cholesky)(f, 7U, kBatch);
        factor.add(a, kBatch * stride);
        factor.add(info, kBatch);
        factor.add(piv, lu ? kBatch * kSmallN : 0U);
        got.at[kBatched + kType<T> * 6U + (lu ? 2U : 1U)] = factor.h;

        for (u64 b = 0U; b < kBatch; ++b)
        {
            for (u64 i = 0U; i < rhs_stride; ++i)
            {
                rhs[b * rhs_stride + i] = i + 1U < rhs_stride ? x[2500U + b * rhs_stride + i] : kGuard<T>;
            }
        }
        const BatchedSolve<T> s{a, rhs, piv, static_cast<i64>(stride), static_cast<i64>(rhs_stride), kSmallN, rhs_cols};
        (lu ? kb.lu_solve : kb.cholesky_solve)(s, 0U, kBatch);
        Digest solve;
        solve.add(rhs, kBatch * rhs_stride);
        got.at[kBatched + kType<T> * 6U + (lu ? 5U : 4U)] = solve.h;
    }
}

// The one-sided Jacobi SVD at n 4 (the lane tier) over the batch, at 30 and 2 sweeps (some still rotating), and at n
// 17 (the scalar tier).
template <typename T> CRD_NOINLINE void check_batched_svd(const BatchedKernels<T>& kb, Digests& got) noexcept
{
    constexpr u64 order = 4U;
    constexpr u64 stride = order * order + 1U;
    constexpr u64 big_n = kBatchedSvdMaxN + 1U;
    constexpr u64 big_len = big_n * big_n + 1U;
    constexpr usize len = big_len > kBatch * stride ? big_len : kBatch * stride;
    T a[len];
    T u[len];
    T v[len];
    T sigma[kBatch * order > big_n ? kBatch * order : big_n];
    i32 info[kBatch];
    Digest d;
    const auto run = [&](u64 batch, u64 n, u32 sweeps)
    {
        const u64 s = n * n + 1U;
        batched_matrices(a, batch, n, false);
        fill(u, batch * s, kGuard<T>);
        fill(v, batch * s, kGuard<T>);
        const i64 si = static_cast<i64>(s);
        const BatchedSvd<T> sv{a, u, v, sigma, info, si, si, si, n, sweeps};
        kb.svd(sv, 0U, batch < 7U ? batch : 7U);
        if (batch > 7U)
        {
            kb.svd(sv, 7U, batch);
        }
        d.add(u, batch * s);
        d.add(v, batch * s);
        d.add(sigma, batch * n);
        d.add(info, batch);
    };
    run(kBatch, order, 30U);
    run(kBatch, order, 2U);
    run(1U, big_n, 30U);
    got.at[kBatched + kType<T> * 6U + 3U] = d.h;
}

// ---- Sparse walks ---------------------------------------------------------------------------------------------------

// A canonical rank-4 COO over 2 x 3 x 2 x 4, every third tuple dropped, and its identity-order CSF tree (level l's
// nodes are its distinct (l + 1)-prefixes, fptr[l][z] node z's first child).
constexpr u32 kSparseRank = 4U;
constexpr u32 kSparseDims[kSparseRank] = {2U, 3U, 2U, 4U};
constexpr u64 kSparseTuples = 48U;
constexpr u64 kSparseNnz = 32U;

struct SparseCase
{
    u32 idx[kSparseRank][kSparseNnz];
    u32 fids[kSparseRank][kSparseNnz];
    u32 fptr[kSparseRank - 1U][kSparseNnz + 1U];
    u64 nodes[kSparseRank];
};

constexpr SparseCase make_sparse() noexcept
{
    SparseCase c{};
    u64 n = 0U;
    for (u64 lin = 0U; lin < kSparseTuples; ++lin)
    {
        if (lin % 3U == 1U)
        {
            continue;
        }
        u64 rem = lin;
        for (u32 m = kSparseRank; m-- > 0U;)
        {
            c.idx[m][n] = static_cast<u32>(rem % kSparseDims[m]);
            rem /= kSparseDims[m];
        }
        u32 first = 0U; // the shallowest level whose key changes
        if (n > 0U)
        {
            first = kSparseRank;
            for (u32 m = 0U; m < kSparseRank && first == kSparseRank; ++m)
            {
                first = c.idx[m][n] != c.idx[m][n - 1U] ? m : first;
            }
        }
        for (u32 l = first; l < kSparseRank; ++l)
        {
            if (l + 1U < kSparseRank)
            {
                c.fptr[l][c.nodes[l]] = static_cast<u32>(c.nodes[l + 1U]);
            }
            c.fids[l][c.nodes[l]] = c.idx[l][n];
            ++c.nodes[l];
        }
        ++n;
    }
    for (u32 l = 0U; l + 1U < kSparseRank; ++l)
    {
        c.fptr[l][c.nodes[l]] = static_cast<u32>(c.nodes[l + 1U]);
    }
    return c;
}

constexpr SparseCase kCoo = make_sparse();
static_assert(kCoo.nodes[kSparseRank - 1U] == kSparseNnz, "the sparse case keeps two tuples in three");

template <typename T> const SparseTensorKernels<T>& sparse_of(const TensorKernels& k) noexcept
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

// TTM at every mode with f two lane groups and a tail (the inner modes staged too) over the nonzeros and a sub-range;
// the fold of three parts over 37 values; both MTTKRPs at r four lane groups and one.
template <typename T> CRD_NOINLINE void check_sparse(const TensorKernels& k, Digests& got) noexcept
{
    const SparseTensorKernels<T>& ks = sparse_of<T>(k);
    const T* x = inputs<T>();
    constexpr u64 f_cols = 2U * kEwLanes<T> + 3U;
    constexpr usize out_len = 3U * 2U * 4U * f_cols + 4U; // the largest out: mode 0 replaced by f
    T out[out_len];
    T tile[out_len];
    Digest scatter;
    Digest staged;
    const u64 ranges[][2] = {{0U, kSparseNnz}, {5U, 27U}};
    for (u32 mode = 0U; mode < kSparseRank; ++mode)
    {
        SparseTtmWalk<T> w{};
        u64 osize = 1U;
        for (u32 m = kSparseRank; m-- > 0U;)
        {
            w.ostride[m] = osize;
            osize *= m == mode ? f_cols : kSparseDims[m];
            w.planes[m] = kCoo.idx[m];
        }
        w.val = x + 50;
        w.u = x + 300;
        w.f = f_cols;
        w.rank = kSparseRank;
        w.mode = mode;
        for (const auto& range : ranges)
        {
            fill(out, out_len, kGuard<T>);
            fill(out, osize, T(0));
            ks.ttm_scatter(w, range[0], range[1], out);
            scatter.add(out, out_len);
            if (mode == 0U || mode + 1U == kSparseRank)
            {
                continue;
            }
            fill(out, out_len, kGuard<T>);
            fill(tile, out_len, kGuard<T>);
            ks.ttm_staged(w, range[0], range[1], tile, out);
            staged.add(out, out_len);
            staged.add(tile + w.ostride[mode] * f_cols, 4U);
        }
    }
    const usize at = kSparseWalks + kType<T> * 5U;
    got.at[at] = scatter.h;
    got.at[at + 1U] = staged.h;

    Digest fold;
    for (usize i = 0U; i < out_len; ++i)
    {
        out[i] = i < 37U ? x[i] : kGuard<T>;
    }
    ks.fold(out, x + 1000, 37U, 3U);
    fold.add(out, out_len);
    got.at[at + 2U] = fold.h;

    Digest csf;
    Digest coo;
    T tmp[2U * (4U * kEwLanes<T> + 1U) + 4U];
    for (const u64 r : {4U * kEwLanes<T> + 1U, u64{1}})
    {
        MttkrpCsfWalk<T> w{};
        MttkrpCooWalk<T> cw{};
        for (u32 l = 0U; l < kSparseRank; ++l)
        {
            const T* factor = l == 0U ? nullptr : x + 1500U + 100U * l;
            w.fids[l] = kCoo.fids[l];
            w.fptr[l] = l + 1U < kSparseRank ? kCoo.fptr[l] : nullptr;
            w.fac[l] = factor;
            cw.planes[l] = kCoo.idx[l];
            cw.fac[l] = factor;
        }
        w.val = x + 2000;
        w.out = out;
        w.r = r;
        w.rank = kSparseRank;
        cw.val = x + 2000;
        cw.out = out;
        cw.n = kSparseNnz;
        cw.r = r;
        cw.rank = kSparseRank;
        const u64 roots[][2] = {{0U, kCoo.nodes[0]}, {1U, kCoo.nodes[0]}};
        for (const auto& range : roots)
        {
            fill(out, out_len, kGuard<T>);
            fill(out, kSparseDims[0] * r, T(0));
            fill(tmp, std::size(tmp), kGuard<T>);
            ks.mttkrp_csf(w, range[0], range[1], tmp);
            csf.add(out, out_len);
            csf.add(tmp + 2U * r, 4U);
        }
        fill(out, out_len, kGuard<T>);
        fill(out, kSparseDims[0] * r, T(0));
        fill(tmp, std::size(tmp), kGuard<T>);
        ks.mttkrp_coo(cw, tmp);
        coo.add(out, out_len);
        coo.add(tmp + 2U * r, 4U);
    }
    got.at[at + 3U] = csf.h;
    got.at[at + 4U] = coo.h;
}

template <typename T> void check_type(const TensorKernels& k, Digests& got) noexcept
{
    check_permutes<T>(k, got);
    check_ew_reduce<T>(k, got);
    check_einsum<T>(k, got);
    check_batched_gemm<T>(batched_of<T>(k), got);
    check_batched_factor<T>(batched_of<T>(k), got);
    check_batched_svd<T>(batched_of<T>(k), got);
    check_sparse<T>(k, got);
}

} // namespace

const char* tensor_self_check(const TensorKernels& table) noexcept
{
    Digests got{};
    check_type<f32>(table, got);
    check_type<f64>(table, got);
    check_bulk(table, got);
    check_nn_rows(table, got);
    check_nn_planes(table, got);
    for (usize i = 0U; i < kRoutines; ++i)
    {
        if (got.at[i] != kAnswers[i].digest)
        {
            return kAnswers[i].name;
        }
    }
    return nullptr;
}

} // namespace crd::hesap::tensor
