// The f32 CNN kernels of crd/hesap/tensor/tensor_kernels.hpp (NnPool2d, NnConv2d), the fifth unit of the tensor family,
// compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5).
//
// The direct 3x3/s1/p1 convolution stages an image's rows once into a zero-padded buffer, so every kj shift is a full
// load, and keeps up to eight output channels' fma chains in registers over the output columns: a Vec16f while 16
// columns remain at avx512, then Vec8f, the tail stored partially. Other shapes run im2col into the scratch slot, the
// bias prefill and the batched unit's tiny GEMM. The max pool is IEEE 754 maximum (a NaN wins, +0 over -0), so its
// result does not depend on the window order, NaN payloads aside: the 2x2/s2 hot shape takes the vertical maximum in
// registers, the other windows fold row-major, and the direct convolution's fused pool gives the bits of conv, relu,
// pool. Every output is the same operations on every level: one (c, ki, kj)-ordered chain of single-rounded fmas from
// 0, one rounding for bias + chain, relu as maxps against +0 (BinaryOp::Max) and the order-free maximum.

#include "tensor_nn_kernels.hpp"

#include "tensor_batched_kernels.hpp"
#include "tensor_ew_kernels.hpp"

#include <crd/core/platform.hpp>
#include <crd/math/simd/simd.hpp>
#include <crd/math/simd/vec16f.hpp>

#include <cstring>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{
namespace
{

namespace simd = crd::math::simd;

using crd::f32;
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

// y[i] = maxps(y[i], +0) in place: the elementwise Max walk with a broadcast zero.
void relu_inplace(f32* y, u64 n) noexcept
{
    const f32 zero = 0.0F;
    const u64 shape[1] = {n};
    const i64 sa[1] = {1};
    const i64 sb[1] = {0};
    EwBinaryKernel<f32, BinaryOp::Max>::walk({y, y, &zero, shape, sa, sb, 1U});
}

// ---- the direct 3x3 / stride-1 / pad-1 convolution ------------------------------------------------------------------

// The staged image: a zero row, then the c * h input rows, each [0, x[0 .. w), 0 x 9] at stride rs = w + 10, so a load
// of 16 lanes at any output column below w - 15 (8 lanes at any column below w) and kj <= 2 stays inside its row.
struct Staged
{
    const f32* rows;
    const f32* zrow;
    u64 rs;
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
    for (u64 ci = 0; ci < c.chans; ++ci)
    {
        for (u64 ki = 0; ki < 3U; ++ki)
        {
            const i64 ih = static_cast<i64>(oi + ki) - 1;
            const bool oob = ih < 0 || ih >= static_cast<i64>(c.h);
            const f32* srow = oob ? s.zrow : s.rows + (ci * c.h + static_cast<u64>(ih)) * s.rs;
            for (u64 kj = 0; kj < 3U; ++kj)
            {
                const V xv = V::load(srow + j0 + kj);
                const u64 wo = (ci * 3U + ki) * 3U + kj;
                for (u32 r = 0; r < R; ++r)
                {
                    acc[r] = simd::fma(V(wblk[r * c.chans * 9U + wo]), xv, acc[r]);
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
    f32* zrow = stage;
    f32* rows = stage + rs;
    std::memset(zrow, 0, rs * sizeof(f32));
    for (u64 ci = 0; ci < c.chans; ++ci)
    {
        for (u64 ih = 0; ih < h; ++ih)
        {
            f32* dst = rows + (ci * h + ih) * rs;
            dst[0] = 0.0F;
            std::memcpy(dst + 1, xi + (ci * h + ih) * w, w * sizeof(f32));
            std::memset(dst + 1 + w, 0, 9U * sizeof(f32));
        }
    }
    const Staged s{rows, zrow, rs};
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
    f32* rowbuf = rows + c.chans * h * rs;
    for (u64 oi2 = 0; oi2 < oh2; ++oi2)
    {
        for (u64 o0 = 0; o0 < c.oc; o0 += 8U)
        {
            const u32 r = c.oc - o0 < 8U ? static_cast<u32>(c.oc - o0) : 8U;
            f32* rb0 = rowbuf;           // conv row 2 * oi2, r rows of rs
            f32* rb1 = rowbuf + 8U * rs; // conv row 2 * oi2 + 1
            conv3x3_rowblock_any(r, c, s, o0, rb0, rs, 2U * oi2);
            conv3x3_rowblock_any(r, c, s, o0, rb1, rs, 2U * oi2 + 1U);
            for (u32 rr = 0; rr < r; ++rr)
            {
                const f32* a = rb0 + static_cast<u64>(rr) * rs;
                const f32* b = rb1 + static_cast<u64>(rr) * rs;
                f32* yrow = yi + (o0 + rr) * oh2 * ow2 + oi2 * ow2;
                for (u64 oj = 0; oj < ow2; ++oj)
                {
                    yrow[oj] = maximum(maximum(a[2U * oj], b[2U * oj]), maximum(a[2U * oj + 1U], b[2U * oj + 1U]));
                }
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

// vm[j] = maximum(r0[j], r1[j]) for j in [j, w) in steps of V; returns where the steps stop.
template <typename V> CRD_FORCEINLINE u64 vertical_steps(const f32* r0, const f32* r1, f32* vm, u64 j, u64 w) noexcept
{
    for (; j + kLanes<V> <= w; j += kLanes<V>)
    {
        maximum(V::load(r0 + j), V::load(r1 + j)).store(vm + j);
    }
    return j;
}

// The 2x2/s2 max pool of a plane at most kPoolRowMax wide: each pair of rows' vertical maximum, then each pair of
// columns'.
void max_pool2_plane(const NnPool2d& c, const f32* xp, f32* yp) noexcept
{
    f32 vm[kPoolRowMax];
    for (u64 oi = 0; oi < c.oh; ++oi)
    {
        const f32* r0 = xp + (2U * oi) * c.w;
        const f32* r1 = r0 + c.w;
        u64 j = vertical_steps<Wide>(r0, r1, vm, 0U, c.w);
        j = vertical_steps<Group>(r0, r1, vm, j, c.w);
        for (; j < c.w; ++j)
        {
            vm[j] = maximum(r0[j], r1[j]);
        }
        f32* yrow = yp + oi * c.ow;
        for (u64 oj = 0; oj < c.ow; ++oj)
        {
            yrow[oj] = maximum(vm[2U * oj], vm[2U * oj + 1U]);
        }
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
            relu_inplace(yi, c.oc * ohw);
        }
    }
}

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
