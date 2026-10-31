// The f32 CNN, layernorm and activation kernels of crd/hesap/tensor/tensor_kernels.hpp (NnPool2d, NnConv2d,
// NnLayerNormF32, NnActF32), the fifth unit of the tensor family, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5), so their single-rounded fmas, and the fmas of the f64 exp and tanh the activations
// run, are the level's instruction or its exact emulation, never a library call or a scalar-table call.
//
// The direct 3x3/s1/p1 convolution stages an image's rows once into a zero-padded buffer, each channel's rows between
// two zero rows, so every kj shift is a full load and every ki step the next row, and keeps up to eight output
// channels' fma chains in registers over the output columns: a Vec16f while 16 columns remain at avx512, then Vec8f,
// the tail stored partially. Other shapes run im2col into the scratch slot, the bias prefill and the batched unit's
// tiny GEMM. The max pool is IEEE 754 maximum (a NaN wins, +0 over -0), so its result does not depend on the window
// order, NaN payloads aside: the 2x2/s2 hot shape takes the vertical maximum in registers and the column pairs'
// maximum on their even and odd lanes, the other windows fold row-major, and the direct convolution's fused pool runs
// the same rows (after a relu, whose rows hold no NaN and no -0, as the plain larger value, the same bits), so it
// gives the bits of conv, relu, pool. Every output is the same operations on every level: one (c, ki, kj)-ordered
// chain of single-rounded fmas from 0, one rounding for bias + chain, relu as maxps against +0 (BinaryOp::Max) and the
// order-free maximum.

#include "tensor_nn_kernels.hpp"

#include "tensor_batched_kernels.hpp"
#include "tensor_ew_kernels.hpp"

#include <crd/core/platform.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/hyperbolic.hpp>
#include <crd/math/select.hpp>
#include <crd/math/simd/simd.hpp>
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/transcendental.hpp>

#include <cstring>
#include <type_traits>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{
namespace
{

namespace simd = crd::math::simd;

using crd::f32;
using crd::f64;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;

// One lane group of output columns, and the register a step takes: two groups at avx512, else one.
using Group = simd::Vec8f;
#if CRD_SIMD_HAS_AVX512
using Wide = simd::Vec16f;
#else
using Wide = Group;
#endif
template <typename V> inline constexpr u64 kLanes = sizeof(V) / sizeof(f32);
static_assert(kLanes<Group> == kEwLanes<f32>);

// The widest row the 2x2/s2 max pool stages its vertical maximum for.
constexpr u64 kPoolRowMax = 256U;

// IEEE 754 maximum: a NaN operand (a's when both are), +0 over -0, else the larger.
CRD_FORCEINLINE f32 maximum(f32 a, f32 b) noexcept
{
    if (a != a)
    {
        return a;
    }
    if (b != b)
    {
        return b;
    }
    if (a == 0.0F && b == 0.0F)
    {
        return a + b; // -0 only when both are
    }
    return a > b ? a : b;
}

template <typename V> CRD_FORCEINLINE V maximum(V a, V b) noexcept
{
    const V zero(0.0F);
    const V r = simd::max(a, b); // b on a NaN or a tie
    const V z = simd::select(simd::cmp_eq(a, zero), simd::select(simd::cmp_eq(b, zero), a + b, r), r);
    return simd::select(simd::cmp_eq(a, a), z, a);
}

// y[i] = maxps(x[i], +0), y may be x: the elementwise Max walk with a broadcast zero.
void relu(const f32* x, f32* y, u64 n) noexcept
{
    const f32 zero = 0.0F;
    const u64 shape[1] = {n};
    const i64 sa[1] = {1};
    const i64 sb[1] = {0};
    EwBinaryKernel<f32, BinaryOp::Max>::walk({y, x, &zero, shape, sa, sb, 1U});
}

// The exact gelu (torch approximate='none'), sigmoid and tanh of NnActF32, each rounded once to f32.
f32 gelu(f32 v) noexcept
{
    constexpr f64 inv_sqrt2 = 0.70710678118654752440;
    const f64 vd = static_cast<f64>(v);
    return static_cast<f32>(0.5 * vd * (1.0 + crd::math::deterministic::erf(vd * inv_sqrt2)));
}

f32 sigmoid(f32 v) noexcept
{
    return static_cast<f32>(1.0 / (1.0 + crd::math::exp(-static_cast<f64>(v))));
}

// The pool's maximum: IEEE 754 maximum, or for rows the relu has passed (maxps against +0: no NaN and no -0) the plain
// larger value, which on such values gives the same bits.
template <bool Relued, typename V> CRD_FORCEINLINE V pool_max(V a, V b) noexcept
{
    if constexpr (!Relued)
    {
        return maximum(a, b);
    }
    else if constexpr (std::is_same_v<V, f32>)
    {
        return a > b ? a : b;
    }
    else
    {
        return simd::max(a, b);
    }
}

// vm[j] = pool_max(r0[j], r1[j]) for j in [j, w) in steps of V; returns where the steps stop.
template <bool Relued, typename V>
CRD_FORCEINLINE u64 vertical_steps(const f32* r0, const f32* r1, f32* vm, u64 j, u64 w) noexcept
{
    for (; j + kLanes<V> <= w; j += kLanes<V>)
    {
        pool_max<Relued>(V::load(r0 + j), V::load(r1 + j)).store(vm + j);
    }
    return j;
}

// yrow[oj] = pool_max(vm[2 oj], vm[2 oj + 1]) for oj in [oj, ow) in steps of V, the pairs split into even and odd
// lanes; returns where the steps stop.
template <bool Relued, typename V> CRD_FORCEINLINE u64 pair_steps(const f32* vm, f32* yrow, u64 oj, u64 ow) noexcept
{
    for (; oj + kLanes<V> <= ow; oj += kLanes<V>)
    {
        V even;
        V odd;
        simd::load_complex_deinterleaved(vm + 2U * oj, even, odd);
        pool_max<Relued>(even, odd).store(yrow + oj);
    }
    return oj;
}

// One 2x2/s2 max-pooled row: vm[j] = pool_max(r0[j], r1[j]) over the w columns (vm may be r0), then yrow[oj] =
// pool_max(vm[2 oj], vm[2 oj + 1]) for oj < ow <= w / 2, in whole registers of each width down to four lanes, the
// last few columns one at a time: no copy, partial store or call for a short row.
template <bool Relued>
CRD_FORCEINLINE void max_pool2_row(const f32* r0, const f32* r1, f32* vm, f32* yrow, u64 w, u64 ow) noexcept
{
    u64 j = vertical_steps<Relued, Wide>(r0, r1, vm, 0U, w);
    j = vertical_steps<Relued, Group>(r0, r1, vm, j, w);
    j = vertical_steps<Relued, simd::Vec4f>(r0, r1, vm, j, w);
    for (; j < w; ++j)
    {
        vm[j] = pool_max<Relued>(r0[j], r1[j]);
    }
    u64 oj = pair_steps<Relued, Group>(vm, yrow, 0U, ow);
    oj = pair_steps<Relued, simd::Vec4f>(vm, yrow, oj, ow);
    for (; oj < ow; ++oj)
    {
        yrow[oj] = pool_max<Relued>(vm[2U * oj], vm[2U * oj + 1U]);
    }
}

// The fused pool of r conv rows of rs floats in rb0 (conv row 2 oi2) and rb1 (the next) into the pooled row oi2 of
// output channels o0 .. o0 + r: one call a row block.
template <bool Relued>
void max_pool2_rows(const NnConv2d& c, f32* rb0, const f32* rb1, u64 rs, u32 r, f32* yi, u64 o0, u64 oi2, u64 oh2,
                    u64 ow2) noexcept
{
    for (u32 rr = 0; rr < r; ++rr)
    {
        f32* a = rb0 + static_cast<u64>(rr) * rs;
        max_pool2_row<Relued>(a, rb1 + static_cast<u64>(rr) * rs, a, yi + (o0 + rr) * oh2 * ow2 + oi2 * ow2, c.w_in,
                              ow2);
    }
}

// ---- the direct 3x3 / stride-1 / pad-1 convolution ------------------------------------------------------------------

// The staged image: per channel a zero row, the h input rows and a zero row (plane = (h + 2) * rs floats), each row
// [0, x[0 .. w), 0 x 9] at stride rs = w + 10, so the padded row oi + ki of a channel is conv row oi's ki tap, and a
// load of 16 lanes at any output column below w - 15 (8 lanes at any column below w) and kj <= 2 stays inside its
// row.
struct Staged
{
    const f32* rows;
    u64 rs;
    u64 plane;
};

// The output columns [j0, j0 + cw) of conv row oi for R output channels into dst + r * dst_stride, cw <= the lanes of
// V: per channel the (c, ki, kj)-ordered fma chain from 0, then bias + chain and the relu.
template <typename V, u32 R>
CRD_FORCEINLINE void conv3x3_step(const NnConv2d& c, const Staged& s, const f32* wblk, const f32* bias, f32* dst,
                                  u64 dst_stride, u64 oi, u64 j0, u64 cw) noexcept
{
    V acc[R];
    for (u32 r = 0; r < R; ++r)
    {
        acc[r] = V(0.0F);
    }
    const u64 chans = c.chans;
    const f32* chan = s.rows + oi * s.rs + j0; // channel 0's padded row oi: the tap ki = 0
    for (u64 ci = 0; ci < chans; ++ci, chan += s.plane)
    {
        const f32* srow = chan;
        for (u64 ki = 0; ki < 3U; ++ki, srow += s.rs)
        {
            for (u64 kj = 0; kj < 3U; ++kj)
            {
                const V xv = V::load(srow + kj);
                const u64 wo = (ci * 3U + ki) * 3U + kj;
                for (u32 r = 0; r < R; ++r)
                {
                    acc[r] = simd::fma(V(wblk[r * chans * 9U + wo]), xv, acc[r]);
                }
            }
        }
    }
    for (u32 r = 0; r < R; ++r)
    {
        V out = V(bias[r]) + acc[r]; // fl(bias + chain): the GEMM's beta = 1 form
        if (c.relu)
        {
            out = simd::max(out, V(0.0F));
        }
        f32* d = dst + static_cast<u64>(r) * dst_stride + j0;
        if (cw == kLanes<V>)
        {
            out.store(d);
        }
        else
        {
            out.store_partial(d, static_cast<usize>(cw));
        }
    }
}

// One conv row for R output channels.
template <u32 R>
void conv3x3_rowblock(const NnConv2d& c, const Staged& s, const f32* wblk, const f32* bias, f32* dst, u64 dst_stride,
                      u64 oi) noexcept
{
    const u64 w = c.w_in;
    u64 j0 = 0;
    for (; j0 + kLanes<Wide> <= w; j0 += kLanes<Wide>)
    {
        conv3x3_step<Wide, R>(c, s, wblk, bias, dst, dst_stride, oi, j0, kLanes<Wide>);
    }
    for (; j0 < w; j0 += kLanes<Group>)
    {
        const u64 cw = w - j0 < kLanes<Group> ? w - j0 : kLanes<Group>;
        conv3x3_step<Group, R>(c, s, wblk, bias, dst, dst_stride, oi, j0, cw);
    }
}

// The output channels [o0, o0 + r), r <= 8.
void conv3x3_rowblock_any(u32 r, const NnConv2d& c, const Staged& s, u64 o0, f32* dst, u64 dst_stride, u64 oi) noexcept
{
    const f32* wblk = c.w + o0 * c.chans * 9U;
    const f32* bias = c.bias + o0;
    switch (r)
    {
        case 8U:
            conv3x3_rowblock<8U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 7U:
            conv3x3_rowblock<7U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 6U:
            conv3x3_rowblock<6U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 5U:
            conv3x3_rowblock<5U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 4U:
            conv3x3_rowblock<4U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 3U:
            conv3x3_rowblock<3U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 2U:
            conv3x3_rowblock<2U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        case 1U:
            conv3x3_rowblock<1U>(c, s, wblk, bias, dst, dst_stride, oi);
            return;
        default:
            return;
    }
}

// One image: the staged rows, then each output row (or, fused, each pair of conv rows pooled 2x2/s2 through 16 row
// buffers after the staging, so only the pooled rows reach memory).
void conv3x3_image(const NnConv2d& c, const f32* xi, f32* yi, f32* stage) noexcept
{
    const u64 h = c.h;
    const u64 w = c.w_in;
    const u64 rs = w + 10U;
    const u64 plane = (h + 2U) * rs;
    f32* rows = stage;
    for (u64 ci = 0; ci < c.chans; ++ci)
    {
        f32* chan = rows + ci * plane;
        std::memset(chan, 0, rs * sizeof(f32));
        for (u64 ih = 0; ih < h; ++ih)
        {
            f32* dst = chan + (ih + 1U) * rs;
            dst[0] = 0.0F;
            std::memcpy(dst + 1, xi + (ci * h + ih) * w, w * sizeof(f32));
            std::memset(dst + 1 + w, 0, 9U * sizeof(f32));
        }
        std::memset(chan + (h + 1U) * rs, 0, rs * sizeof(f32));
    }
    const Staged s{rows, rs, plane};
    if (!c.pool)
    {
        for (u64 oi = 0; oi < h; ++oi)
        {
            for (u64 o0 = 0; o0 < c.oc; o0 += 8U)
            {
                const u32 r = c.oc - o0 < 8U ? static_cast<u32>(c.oc - o0) : 8U;
                conv3x3_rowblock_any(r, c, s, o0, yi + o0 * h * w + oi * w, h * w, oi);
            }
        }
        return;
    }
    const u64 oh2 = (h - 2U) / 2U + 1U;
    const u64 ow2 = (w - 2U) / 2U + 1U;
    f32* rowbuf = rows + c.chans * plane;
    for (u64 oi2 = 0; oi2 < oh2; ++oi2)
    {
        for (u64 o0 = 0; o0 < c.oc; o0 += 8U)
        {
            const u32 r = c.oc - o0 < 8U ? static_cast<u32>(c.oc - o0) : 8U;
            f32* rb0 = rowbuf;           // conv row 2 * oi2, r rows of rs
            f32* rb1 = rowbuf + 8U * rs; // conv row 2 * oi2 + 1
            conv3x3_rowblock_any(r, c, s, o0, rb0, rs, 2U * oi2);
            conv3x3_rowblock_any(r, c, s, o0, rb1, rs, 2U * oi2 + 1U);
            if (c.relu)
            {
                max_pool2_rows<true>(c, rb0, rb1, rs, r, yi, o0, oi2, oh2, ow2);
            }
            else
            {
                max_pool2_rows<false>(c, rb0, rb1, rs, r, yi, o0, oi2, oh2, ow2);
            }
        }
    }
}

// ---- im2col + the tiny GEMM ---------------------------------------------------------------------------------------

// One image's column matrix col[(c * KH + ki) * KW + kj][oh * OW + ow]: stride-1 rows as memset edges and a memcpy
// middle, other strides through a bounds select.
void im2col_one(const NnConv2d& c, const f32* x, f32* col) noexcept
{
    const u64 h = c.h;
    const u64 w = c.w_in;
    const u64 oh = c.oh;
    const u64 ow = c.ow;
    const u64 ohw = oh * ow;
    u64 r = 0;
    for (u64 ci = 0; ci < c.chans; ++ci)
    {
        const f32* plane = x + ci * h * w;
        for (u64 ki = 0; ki < c.kh; ++ki)
        {
            for (u64 kj = 0; kj < c.kw; ++kj)
            {
                f32* dst = col + r * ohw;
                ++r;
                for (u64 oi = 0; oi < oh; ++oi)
                {
                    const i64 ih = static_cast<i64>(oi * c.stride + ki) - static_cast<i64>(c.pad);
                    f32* drow = dst + oi * ow;
                    if (ih < 0 || ih >= static_cast<i64>(h))
                    {
                        std::memset(drow, 0, ow * sizeof(f32));
                        continue;
                    }
                    const f32* srow = plane + static_cast<u64>(ih) * w;
                    if (c.stride == 1U)
                    {
                        const i64 shift = static_cast<i64>(kj) - static_cast<i64>(c.pad);
                        const u64 valid_lo = shift < 0 ? static_cast<u64>(-shift) : 0U;
                        const i64 hi_signed = static_cast<i64>(w) - shift;
                        u64 valid_hi = hi_signed < 0 ? 0U : static_cast<u64>(hi_signed);
                        valid_hi = valid_hi > ow ? ow : valid_hi;
                        if (valid_lo > 0U)
                        {
                            std::memset(drow, 0, valid_lo * sizeof(f32));
                        }
                        if (valid_hi > valid_lo)
                        {
                            std::memcpy(drow + valid_lo, srow + static_cast<i64>(valid_lo) + shift,
                                        (valid_hi - valid_lo) * sizeof(f32));
                        }
                        if (valid_hi < ow)
                        {
                            std::memset(drow + valid_hi, 0, (ow - valid_hi) * sizeof(f32));
                        }
                    }
                    else
                    {
                        for (u64 oj = 0; oj < ow; ++oj)
                        {
                            const i64 iw = static_cast<i64>(oj * c.stride + kj) - static_cast<i64>(c.pad);
                            drow[oj] = (iw >= 0 && iw < static_cast<i64>(w)) ? srow[iw] : 0.0F;
                        }
                    }
                }
            }
        }
    }
}

// Every row of y[oc][ohw] set to its bias: the GEMM's beta = 1 operand.
void bias_fill(f32* y, const f32* bias, u64 oc, u64 ohw) noexcept
{
    for (u64 o = 0; o < oc; ++o)
    {
        const Wide bw(bias[o]);
        const Group bg(bias[o]);
        f32* row = y + o * ohw;
        u64 p = 0;
        for (; p + kLanes<Wide> <= ohw; p += kLanes<Wide>)
        {
            bw.store(row + p);
        }
        for (; p + kLanes<Group> <= ohw; p += kLanes<Group>)
        {
            bg.store(row + p);
        }
        if (p < ohw)
        {
            bg.store_partial(row + p, static_cast<usize>(ohw - p));
        }
    }
}

// ---- pooling ----------------------------------------------------------------------------------------------------

// The 2x2/s2 max pool of a plane at most kPoolRowMax wide.
void max_pool2_plane(const NnPool2d& c, const f32* xp, f32* yp) noexcept
{
    f32 vm[kPoolRowMax];
    for (u64 oi = 0; oi < c.oh; ++oi)
    {
        const f32* r0 = xp + (2U * oi) * c.w;
        max_pool2_row<false>(r0, r0 + c.w, vm, yp + oi * c.ow, c.w, c.ow);
    }
}

} // namespace

void NnKernel::pool2d_planes(const NnPool2d& c, u32 lo, u32 hi) noexcept
{
    const bool pool2 = c.is_max && c.k == 2U && c.s == 2U && c.w <= kPoolRowMax;
    const f32 inv = 1.0F / static_cast<f32>(c.k * c.k);
    for (u32 p = lo; p < hi; ++p)
    {
        const f32* xp = c.x + static_cast<u64>(p) * c.h * c.w;
        f32* yp = c.y + static_cast<u64>(p) * c.oh * c.ow;
        if (pool2)
        {
            max_pool2_plane(c, xp, yp);
            continue;
        }
        for (u64 oi = 0; oi < c.oh; ++oi)
        {
            for (u64 oj = 0; oj < c.ow; ++oj)
            {
                const f32* win = xp + oi * c.s * c.w + oj * c.s;
                f32 acc = c.is_max ? win[0] : 0.0F;
                for (u64 ki = 0; ki < c.k; ++ki)
                {
                    for (u64 kj = 0; kj < c.k; ++kj)
                    {
                        const f32 v = win[ki * c.w + kj];
                        acc = c.is_max ? maximum(acc, v) : acc + v;
                    }
                }
                yp[oi * c.ow + oj] = c.is_max ? acc : acc * inv;
            }
        }
    }
}

void NnKernel::conv2d_images(const NnConv2d& c, f32* scratch, u32 lo, u32 hi) noexcept
{
    const u64 ckk = c.chans * c.kh * c.kw;
    const u64 ohw = c.oh * c.ow;
    const bool direct = c.kh == 3U && c.kw == 3U && c.pad == 1U && c.stride == 1U;
    const u64 yohw = c.pool ? ((c.oh - 2U) / 2U + 1U) * ((c.ow - 2U) / 2U + 1U) : ohw;
    for (u32 img = lo; img < hi; ++img)
    {
        const f32* xi = c.x + static_cast<u64>(img) * c.chans * c.h * c.w_in;
        f32* yi = c.y + static_cast<u64>(img) * c.oc * yohw;
        if (direct)
        {
            conv3x3_image(c, xi, yi, scratch);
            continue;
        }
        im2col_one(c, xi, scratch);
        bias_fill(yi, c.bias, c.oc, ohw);
        BatchedKernel<f32>::gemm_tiny({1.0F, 1.0F, c.w, scratch, yi, 0, 0, 0, c.oc, ckk, ohw}, 0U, 1U);
        if (c.relu)
        {
            relu(yi, yi, c.oc * ohw);
        }
    }
}

void NnKernel::layernorm_rows(const NnLayerNormF32& c, u32 lo, u32 hi) noexcept
{
    for (u32 r = lo; r < hi; ++r)
    {
        const f32* x = c.x + static_cast<u64>(r) * c.d;
        f32* y = c.y + static_cast<u64>(r) * c.d;
        f32 mean = 0.0F;
        for (u64 j = 0; j < c.d; ++j)
        {
            mean += x[j];
        }
        mean /= static_cast<f32>(c.d);
        f32 var = 0.0F;
        for (u64 j = 0; j < c.d; ++j)
        {
            const f32 t = x[j] - mean;
            var = crd::math::fma(t, t, var);
        }
        var /= static_cast<f32>(c.d);
        const f32 rstd = 1.0F / crd::math::sqrt(var + c.eps);
        for (u64 j = 0; j < c.d; ++j)
        {
            y[j] = crd::math::fma((x[j] - mean) * rstd, c.g[j], c.b[j]);
        }
    }
}

void NnKernel::act_rows(const NnActF32& c, u32 lo, u32 hi) noexcept
{
    const u64 first = static_cast<u64>(lo) * c.per;
    const u64 n = static_cast<u64>(hi - lo) * c.per;
    const f32* x = c.x + first;
    f32* y = c.y + first;
    switch (c.act)
    {
        case NnAct::Relu:
            relu(x, y, n);
            return;
        case NnAct::Gelu:
            for (u64 i = 0; i < n; ++i)
            {
                y[i] = gelu(x[i]);
            }
            return;
        case NnAct::Tanh:
            for (u64 i = 0; i < n; ++i)
            {
                y[i] = crd::math::tanh(x[i]);
            }
            return;
        case NnAct::Sigmoid:
            for (u64 i = 0; i < n; ++i)
            {
                y[i] = sigmoid(x[i]);
            }
            return;
    }
}

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
