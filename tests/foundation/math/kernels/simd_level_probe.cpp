// The SIMD-type probe, compiled once per CPU level by crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5): every
// operation of the crd::math::simd types of this level's backend over the probe inputs. The table is this level's
// (CRD_KERNEL_TABLE); everything else has internal linkage or lives in the level namespace.

#include "../simd_level_probe.hpp"

#include <crd/math/exp_log_core.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/convert.hpp>
#include <crd/math/simd/mat4f.hpp>
#include <crd/math/simd/quatf.hpp>
#include <crd/math/simd/transcendental.hpp>
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec16i.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec4i.hpp>
#include <crd/math/simd/vec8d.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/math/simd/vec8i.hpp>

namespace crd::test::simd_probe
{
namespace
{

using crd::f32;
using crd::f64;
using crd::i32;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::simd::Mat4f;
using crd::math::simd::Quatf;
using crd::math::simd::Vec16f;
using crd::math::simd::Vec16i;
using crd::math::simd::Vec4d;
using crd::math::simd::Vec4f;
using crd::math::simd::Vec4i;
using crd::math::simd::Vec8d;
using crd::math::simd::Vec8f;
using crd::math::simd::Vec8i;
namespace s = crd::math::simd;

// ---- result words ----

usize put(f32 x, u64* out) noexcept
{
    out[0] = __builtin_bit_cast(u32, x);
    return 1;
}
usize put(f64 x, u64* out) noexcept
{
    out[0] = __builtin_bit_cast(u64, x);
    return 1;
}
usize put(Vec4f v, u64* out) noexcept
{
    f32 t[4];
    v.store(t);
    for (usize i = 0; i < 4; ++i)
    {
        put(t[i], out + i);
    }
    return 4;
}
usize put(Vec8f v, u64* out) noexcept
{
    f32 t[8];
    v.store(t);
    for (usize i = 0; i < 8; ++i)
    {
        put(t[i], out + i);
    }
    return 8;
}
usize put(Vec16f v, u64* out) noexcept
{
    f32 t[16];
    v.store(t);
    for (usize i = 0; i < 16; ++i)
    {
        put(t[i], out + i);
    }
    return 16;
}
usize put(Vec4d v, u64* out) noexcept
{
    f64 t[4];
    v.store(t);
    for (usize i = 0; i < 4; ++i)
    {
        put(t[i], out + i);
    }
    return 4;
}
usize put(Vec8d v, u64* out) noexcept
{
    f64 t[8];
    v.store(t);
    for (usize i = 0; i < 8; ++i)
    {
        put(t[i], out + i);
    }
    return 8;
}
template <typename VI> usize put_int(VI v, u64* out) noexcept
{
    constexpr usize lanes = sizeof(VI) / sizeof(i32);
    for (usize i = 0; i < lanes; ++i)
    {
        out[i] = static_cast<u32>(v.lane(i));
    }
    return lanes;
}
usize put(Vec4i v, u64* out) noexcept
{
    return put_int(v, out);
}
usize put(Vec8i v, u64* out) noexcept
{
    return put_int(v, out);
}
usize put(Vec16i v, u64* out) noexcept
{
    return put_int(v, out);
}

// ---- operand loads ----

template <typename V> struct Width;
template <> struct Width<Vec4f>
{
    using Int = Vec4i;
    static constexpr usize kWidth = 4;
    static Int ints(const i32* p) noexcept { return Vec4i(p[0], p[1], p[2], p[3]); }
};
template <> struct Width<Vec8f>
{
    using Int = Vec8i;
    static constexpr usize kWidth = 8;
    static Int ints(const i32* p) noexcept { return Vec8i(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }
};
template <> struct Width<Vec16f>
{
    using Int = Vec16i;
    static constexpr usize kWidth = 16;
    static Int ints(const i32* p) noexcept { return Vec16i::from(p); }
};

// fn(a, b, c) over each vector of the f32 inputs.
template <typename V, typename Fn> usize each_f32(const Input& in, u64* out, Fn fn) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += Width<V>::kWidth)
    {
        n += put(fn(V::load(in.a + i), V::load(in.b + i), V::load(in.c + i)), out + n);
    }
    return n;
}

// fn(a, b) over each vector of the i32 inputs.
template <typename V, typename Fn> usize each_i32(const Input& in, u64* out, Fn fn) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += Width<V>::kWidth)
    {
        n += put(fn(Width<V>::ints(in.ia + i), Width<V>::ints(in.ib + i)), out + n);
    }
    return n;
}

// fn(a, b, c) over each vector of the f64 inputs.
template <typename V, typename Fn> usize each_f64(const Input& in, u64* out, Fn fn) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += sizeof(V) / sizeof(f64))
    {
        n += put(fn(V::load(in.da + i), V::load(in.db + i), V::load(in.dc + i)), out + n);
    }
    return n;
}

// ---- the operations both f32 widths have ----

template <typename V> struct F32
{
    using VI = typename Width<V>::Int;

    static usize add(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return a + b; });
    }
    static usize sub(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return a - b; });
    }
    static usize mul(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return a * b; });
    }
    static usize div(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return a / b; });
    }
    static usize neg(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return -a; });
    }
    static usize scale(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return (a * 3.0F) / 0.75F; });
    }
    static usize mul_add(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V c) { return s::mul_add(a, b, c); });
    }
    static usize mul_sub(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V c) { return s::mul_sub(a, b, c); });
    }
    static usize fma(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V c) { return s::fma(a, b, c); });
    }
    static usize min(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::min(a, b); });
    }
    static usize max(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::max(a, b); });
    }
    static usize abs(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::abs(a); });
    }
    static usize clamp(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V c) { return s::clamp(a, s::min(b, c), s::max(b, c)); });
    }
    static usize sqrt(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::sqrt(a); });
    }
    static usize horizontal_sum(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::horizontal_sum(a); });
    }
    static usize dot(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::dot(a, b); });
    }
    static usize cmp_lt(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::cmp_lt(a, b); });
    }
    static usize cmp_le(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::cmp_le(a, b); });
    }
    static usize cmp_eq(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::cmp_eq(a, b); });
    }
    static usize cmp_gt(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::cmp_gt(a, b); });
    }
    static usize cmp_ge(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V) { return s::cmp_ge(a, b); });
    }
    static usize select(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V c) { return s::select(s::cmp_lt(a, b), b, c); });
    }
    static usize truncate(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::truncate(a); });
    }
    static usize round_nearest(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::round_nearest(a); });
    }
    static usize convert_truncate(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::convert_truncate(a); });
    }
    static usize convert_to_float(const Input& in, u64* out) noexcept
    {
        return each_i32<V>(in, out, [](VI a, VI) { return s::convert_to_float(a); });
    }
    static usize bitcast(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V, V) { return s::bitcast_to_float(s::bitcast_to_int(a) + VI(1)); });
    }
    static usize bit_ops(const Input& in, u64* out) noexcept
    {
        return each_f32<V>(in, out, [](V a, V b, V c)
                           { return s::bit_xor(s::bit_or(s::bit_and(a, b), s::bit_andnot(b, c)), a); });
    }
};

// ---- the integer operations of both widths ----

template <typename V> struct I32
{
    using VI = typename Width<V>::Int;

    static usize add(const Input& in, u64* out) noexcept
    {
        return each_i32<V>(in, out, [](VI a, VI b) { return a + b; });
    }
    static usize sub(const Input& in, u64* out) noexcept
    {
        return each_i32<V>(in, out, [](VI a, VI b) { return a - b; });
    }
    static usize bit_ops(const Input& in, u64* out) noexcept
    {
        return each_i32<V>(in, out, [](VI a, VI b) { return ((a & b) | s::and_not(b, a)) ^ (a | b); });
    }
    static usize shifts(const Input& in, u64* out) noexcept
    {
        return each_i32<V>(in, out, [](VI a, VI)
                           { return s::shift_left<7>(a) ^ s::shift_right_arith<5>(a) ^ s::shift_right_logical<9>(a); });
    }
    static usize compare(const Input& in, u64* out) noexcept
    {
        return each_i32<V>(in, out, [](VI a, VI b) { return s::cmp_eq(a, b) | (s::cmp_gt(a, b) & VI(2)); });
    }
};

// ---- the Vec8f-only operations ----

usize v8_fnmadd(const Input& in, u64* out) noexcept
{
    return each_f32<Vec8f>(in, out, [](Vec8f a, Vec8f b, Vec8f c) { return s::fnmadd(a, b, c); });
}
usize v8_fmaddsub(const Input& in, u64* out) noexcept
{
    return each_f32<Vec8f>(in, out, [](Vec8f a, Vec8f b, Vec8f c) { return s::fmaddsub(a, b, c); });
}
usize v8_addsub(const Input& in, u64* out) noexcept
{
    return each_f32<Vec8f>(in, out, [](Vec8f a, Vec8f b, Vec8f) { return s::addsub(a, b); });
}
usize v8_shuffles(const Input& in, u64* out) noexcept
{
    return each_f32<Vec8f>(in, out, [](Vec8f a, Vec8f b, Vec8f c)
                           { return s::blend_c_odd(s::unpack_c_lo(s::swap_pairs(a), b), s::unpack_c_hi(b, c)); });
}
usize v8_loads(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i + 8 <= kLanes; i += 8)
    {
        n += put(s::load_dup_pairs(in.a + i), out + n);
        n += put(s::load_c_quad(in.a + i, in.b + i), out + n);
    }
    return n;
}
usize v8_half_stores(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += 8)
    {
        f32 t[8] = {};
        s::store_c_lo(t, Vec8f::load(in.a + i));
        s::store_c_hi(t + 4, Vec8f::load(in.b + i));
        for (const f32 x : t)
        {
            n += put(x, out + n);
        }
    }
    return n;
}
usize v8_partial(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize count = 1; count < 8; ++count)
    {
        f32 t[8] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
        const Vec8f v = Vec8f::load_partial(in.a + count, count);
        n += put(v, out + n);
        (v + Vec8f::load(in.b)).store_partial(t, count);
        for (const f32 x : t)
        {
            n += put(x, out + n);
        }
    }
    return n;
}
usize v8_transpose(const Input& in, u64* out) noexcept
{
    Vec8f r[8];
    for (usize i = 0; i < 8; ++i)
    {
        r[i] = Vec8f::load(in.a + (8 * i));
    }
    s::transpose8x8(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    usize n = 0;
    for (const Vec8f& v : r)
    {
        n += put(v, out + n);
    }
    return n;
}
usize v8_complex(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i + 16 <= kLanes; i += 16)
    {
        Vec8f re;
        Vec8f im;
        s::load_complex_deinterleaved(in.a + i, re, im);
        n += put(re, out + n);
        n += put(im, out + n);
        f32 t[16];
        s::store_complex_interleaved(t, im, re);
        for (const f32 x : t)
        {
            n += put(x, out + n);
        }
    }
    return n;
}

// ---- the Vec16f-only operations ----

usize v16_fnmadd(const Input& in, u64* out) noexcept
{
    return each_f32<Vec16f>(in, out, [](Vec16f a, Vec16f b, Vec16f c) { return s::fnmadd(a, b, c); });
}
// Every partial count: the loaded lanes, then a store's buffer whose lanes past the count must keep their guard values.
usize v16_partial(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize count = 1; count < 16; ++count)
    {
        f32 t[16];
        for (usize i = 0; i < 16; ++i)
        {
            t[i] = static_cast<f32>(i + 1U);
        }
        const Vec16f v = Vec16f::load_partial(in.a + count, count);
        n += put(v, out + n);
        (v + Vec16f::load(in.b)).store_partial(t, count);
        for (const f32 x : t)
        {
            n += put(x, out + n);
        }
    }
    return n;
}
usize v16_halves(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += 16)
    {
        const Vec16f v = Vec16f::load_aligned(in.a + i);
        n += put(Vec16f(s::high_half(v), s::low_half(Vec16f::load(in.b + i))), out + n);
    }
    return n;
}

// ---- the operations both f64 widths have ----

template <typename V> struct F64
{
    static constexpr usize kWidth = sizeof(V) / sizeof(f64);

    static usize arith(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V b, V c) { return ((a + b) * c - a) / b; });
    }
    static usize neg_abs(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V, V) { return s::select(s::cmp_lt(a, V(0.5)), -a, s::abs(a)); });
    }
    static usize fused(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V b, V c) { return s::fma(a, b, c); });
    }
    static usize fnmadd(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V b, V c) { return s::fnmadd(a, b, c); });
    }
    static usize mul_add(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V b, V c) { return s::mul_sub(s::mul_add(a, b, c), c, a); });
    }
    static usize min_max(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V b, V c) { return s::clamp(a, s::min(b, c), s::max(b, c)); });
    }
    static usize sqrt(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(in, out, [](V a, V, V) { return s::sqrt(a); });
    }
    static usize reduce(const Input& in, u64* out) noexcept
    {
        usize n = 0;
        for (usize i = 0; i < kLanes; i += kWidth)
        {
            n += put(s::horizontal_sum(V::load(in.da + i)), out + n);
            n += put(s::dot(V::load(in.da + i), V::load(in.db + i)), out + n);
        }
        return n;
    }
    static usize compare(const Input& in, u64* out) noexcept
    {
        return each_f64<V>(
            in, out, [](V a, V b, V c)
            { return s::select(s::cmp_le(a, b), s::select(s::cmp_eq(b, c), a, b), s::select(s::cmp_gt(a, c), c, a)); });
    }
    static usize compare_masks(const Input& in, u64* out) noexcept
    {
        usize n = 0;
        for (usize i = 0; i < kLanes; i += kWidth)
        {
            const V a = V::load_aligned(in.da + i);
            const V b = V::load(in.db + i);
            n += put(s::cmp_lt(a, b), out + n);
            n += put(s::cmp_le(a, b), out + n);
            n += put(s::cmp_eq(a, b), out + n);
            n += put(s::cmp_ge(a, b), out + n);
        }
        return n;
    }
    // Every partial count, as v16_partial.
    static usize partial(const Input& in, u64* out) noexcept
    {
        usize n = 0;
        for (usize count = 1; count < kWidth; ++count)
        {
            f64 t[kWidth];
            for (usize i = 0; i < kWidth; ++i)
            {
                t[i] = static_cast<f64>(i + 1U);
            }
            const V v = V::load_partial(in.da + count, count);
            n += put(v, out + n);
            (v * V::load(in.db)).store_partial(t, count);
            for (const f64 x : t)
            {
                n += put(x, out + n);
            }
        }
        return n;
    }
};

// ---- the Vec4d-only operations ----

usize d_fmaddsub(const Input& in, u64* out) noexcept
{
    return each_f64<Vec4d>(in, out, [](Vec4d a, Vec4d b, Vec4d c) { return s::fmaddsub(a, b, c); });
}
usize d_addsub(const Input& in, u64* out) noexcept
{
    return each_f64<Vec4d>(in, out, [](Vec4d a, Vec4d b, Vec4d) { return s::addsub(a, b); });
}
usize d_shuffles(const Input& in, u64* out) noexcept
{
    return each_f64<Vec4d>(in, out, [](Vec4d a, Vec4d b, Vec4d c)
                           { return s::mix_lo_hi(s::concat_lo(s::swap_pairs(a), b), s::concat_hi(b, c)); });
}
usize d_loads(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i + 4 <= kLanes; i += 4)
    {
        n += put(s::load_pair128(in.da + i, in.db + i), out + n);
        n += put(s::load_dup_pairs(in.dc + i), out + n);
    }
    return n;
}
usize d8_halves(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += 8)
    {
        const Vec8d v = Vec8d::load(in.da + i);
        n += put(Vec8d(s::high_half(v), s::low_half(Vec8d::load_aligned(in.db + i))), out + n);
    }
    return n;
}
usize d_transpose(const Input& in, u64* out) noexcept
{
    Vec4d r0 = Vec4d::load(in.da);
    Vec4d r1 = Vec4d::load(in.da + 4);
    Vec4d r2 = Vec4d::load(in.db);
    Vec4d r3 = Vec4d::load(in.dc);
    s::transpose4x4(r0, r1, r2, r3);
    usize n = put(r0, out);
    n += put(r1, out + n);
    n += put(r2, out + n);
    return n + put(r3, out + n);
}
usize d_complex(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i + 8 <= kLanes; i += 8)
    {
        Vec4d re;
        Vec4d im;
        s::load_complex_deinterleaved(in.da + i, re, im);
        n += put(re, out + n);
        n += put(im, out + n);
        f64 t[8];
        s::store_complex_interleaved(t, im, re);
        for (const f64 x : t)
        {
            n += put(x, out + n);
        }
    }
    return n;
}
usize d_exp_log_core(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; ++i)
    {
        n += put(crd::math::crd_exp1(in.da[i]), out + n);
        n += put(crd::math::crd_log1(in.db[i]), out + n);
    }
    return n;
}

// The level's vector twins of those cores (four lanes on the 256-bit backends, two on SSE2 and SSE4.2; the cores
// themselves elsewhere): exp over the raw inputs and over [-750, 750], log over the raw inputs and their magnitudes.
usize d_exp_log_twins(const Input& in, u64* out) noexcept
{
    alignas(32) f64 x[4 * kLanes];
    for (usize i = 0; i < kLanes; ++i)
    {
        x[i] = in.da[i];
        x[kLanes + i] = (static_cast<f64>(__builtin_bit_cast(u64, in.dc[i]) % 3000001U) / 2000.0) - 750.0;
        x[(2 * kLanes) + i] = in.db[i];
        x[(3 * kLanes) + i] = __builtin_bit_cast(f64, __builtin_bit_cast(u64, in.db[i]) & ~(1ULL << 63U));
    }
    alignas(32) f64 r[4 * kLanes];
#if CRD_SIMD_HAS_AVX2
    for (usize i = 0; i < 4 * kLanes; i += 4)
    {
        const __m256d v = _mm256_load_pd(x + i);
        _mm256_store_pd(r + i, i < 2 * kLanes ? crd::math::crd_exp4(v) : crd::math::crd_log4(v));
    }
#elif CRD_SIMD_HAS_SSE2
    for (usize i = 0; i < 4 * kLanes; i += 2)
    {
        const __m128d v = _mm_load_pd(x + i);
        _mm_store_pd(r + i, i < 2 * kLanes ? crd::math::crd_exp_x2(v) : crd::math::crd_log_x2(v));
    }
#else
    for (usize i = 0; i < 4 * kLanes; ++i)
    {
        r[i] = i < 2 * kLanes ? crd::math::crd_exp1(x[i]) : crd::math::crd_log1(x[i]);
    }
#endif
    usize n = 0;
    for (const f64 value : r)
    {
        n += put(value, out + n);
    }
    return n;
}

// ---- Mat4f and Quatf ----

usize mat4(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i + 32 <= kLanes; i += 32)
    {
        const Mat4f a = Mat4f::load_column_major(in.a + i);
        const Mat4f b = Mat4f::load_column_major(in.a + i + 16);
        const Mat4f p = s::transpose(a * b) + (b * 0.5F);
        for (const Vec4f& col : p.cols)
        {
            n += put(col, out + n);
        }
        n += put(a * Vec4f::load(in.b + i), out + n);
        n += put(s::transform_point(a, in.c[i], in.c[i + 1], in.c[i + 2]), out + n);
    }
    return n;
}
usize quat(const Input& in, u64* out) noexcept
{
    usize n = 0;
    for (usize i = 0; i < kLanes; i += 4)
    {
        const Quatf q(Vec4f::load(in.a + i));
        const Quatf r(Vec4f::load(in.b + i));
        n += put((q * r).xyzw, out + n);
        n += put(s::normalize(q).xyzw, out + n);
        n += put(s::rotate(r, in.c[i], in.c[i + 1], in.c[i + 2]), out + n);
    }
    return n;
}

constexpr Op kOps[] = {
    {"vec4f +", Compare::F32Value, &F32<Vec4f>::add},
    {"vec8f +", Compare::F32Value, &F32<Vec8f>::add},
    {"vec16f +", Compare::F32Value, &F32<Vec16f>::add},
    {"vec4f -", Compare::F32Value, &F32<Vec4f>::sub},
    {"vec8f -", Compare::F32Value, &F32<Vec8f>::sub},
    {"vec16f -", Compare::F32Value, &F32<Vec16f>::sub},
    {"vec4f *", Compare::F32Value, &F32<Vec4f>::mul},
    {"vec8f *", Compare::F32Value, &F32<Vec8f>::mul},
    {"vec16f *", Compare::F32Value, &F32<Vec16f>::mul},
    {"vec4f /", Compare::F32Value, &F32<Vec4f>::div},
    {"vec8f /", Compare::F32Value, &F32<Vec8f>::div},
    {"vec16f /", Compare::F32Value, &F32<Vec16f>::div},
    {"vec4f negate", Compare::Bits, &F32<Vec4f>::neg},
    {"vec8f negate", Compare::Bits, &F32<Vec8f>::neg},
    {"vec16f negate", Compare::Bits, &F32<Vec16f>::neg},
    {"vec4f scalar * /", Compare::F32Value, &F32<Vec4f>::scale},
    {"vec8f scalar * /", Compare::F32Value, &F32<Vec8f>::scale},
    {"vec16f scalar * /", Compare::F32Value, &F32<Vec16f>::scale},
    {"vec4f mul_add", Compare::F32Value, &F32<Vec4f>::mul_add},
    {"vec8f mul_add", Compare::F32Value, &F32<Vec8f>::mul_add},
    {"vec16f mul_add", Compare::F32Value, &F32<Vec16f>::mul_add},
    {"vec4f mul_sub", Compare::F32Value, &F32<Vec4f>::mul_sub},
    {"vec8f mul_sub", Compare::F32Value, &F32<Vec8f>::mul_sub},
    {"vec16f mul_sub", Compare::F32Value, &F32<Vec16f>::mul_sub},
    {"vec4f fma", Compare::F32Value, &F32<Vec4f>::fma},
    {"vec8f fma", Compare::F32Value, &F32<Vec8f>::fma},
    {"vec16f fma", Compare::F32Value, &F32<Vec16f>::fma},
    {"vec4f min", Compare::Bits, &F32<Vec4f>::min},
    {"vec8f min", Compare::Bits, &F32<Vec8f>::min},
    {"vec16f min", Compare::Bits, &F32<Vec16f>::min},
    {"vec4f max", Compare::Bits, &F32<Vec4f>::max},
    {"vec8f max", Compare::Bits, &F32<Vec8f>::max},
    {"vec16f max", Compare::Bits, &F32<Vec16f>::max},
    {"vec4f abs", Compare::Bits, &F32<Vec4f>::abs},
    {"vec8f abs", Compare::Bits, &F32<Vec8f>::abs},
    {"vec16f abs", Compare::Bits, &F32<Vec16f>::abs},
    {"vec4f clamp", Compare::Bits, &F32<Vec4f>::clamp},
    {"vec8f clamp", Compare::Bits, &F32<Vec8f>::clamp},
    {"vec16f clamp", Compare::Bits, &F32<Vec16f>::clamp},
    {"vec4f sqrt", Compare::F32Value, &F32<Vec4f>::sqrt},
    {"vec8f sqrt", Compare::F32Value, &F32<Vec8f>::sqrt},
    {"vec16f sqrt", Compare::F32Value, &F32<Vec16f>::sqrt},
    {"vec4f horizontal_sum", Compare::F32Value, &F32<Vec4f>::horizontal_sum},
    {"vec8f horizontal_sum", Compare::F32Value, &F32<Vec8f>::horizontal_sum},
    {"vec16f horizontal_sum", Compare::F32Value, &F32<Vec16f>::horizontal_sum},
    {"vec4f dot", Compare::F32Value, &F32<Vec4f>::dot},
    {"vec8f dot", Compare::F32Value, &F32<Vec8f>::dot},
    {"vec16f dot", Compare::F32Value, &F32<Vec16f>::dot},
    {"vec4f cmp_lt", Compare::Bits, &F32<Vec4f>::cmp_lt},
    {"vec8f cmp_lt", Compare::Bits, &F32<Vec8f>::cmp_lt},
    {"vec16f cmp_lt", Compare::Bits, &F32<Vec16f>::cmp_lt},
    {"vec4f cmp_le", Compare::Bits, &F32<Vec4f>::cmp_le},
    {"vec8f cmp_le", Compare::Bits, &F32<Vec8f>::cmp_le},
    {"vec16f cmp_le", Compare::Bits, &F32<Vec16f>::cmp_le},
    {"vec4f cmp_eq", Compare::Bits, &F32<Vec4f>::cmp_eq},
    {"vec8f cmp_eq", Compare::Bits, &F32<Vec8f>::cmp_eq},
    {"vec16f cmp_eq", Compare::Bits, &F32<Vec16f>::cmp_eq},
    {"vec4f cmp_gt", Compare::Bits, &F32<Vec4f>::cmp_gt},
    {"vec8f cmp_gt", Compare::Bits, &F32<Vec8f>::cmp_gt},
    {"vec16f cmp_gt", Compare::Bits, &F32<Vec16f>::cmp_gt},
    {"vec4f cmp_ge", Compare::Bits, &F32<Vec4f>::cmp_ge},
    {"vec8f cmp_ge", Compare::Bits, &F32<Vec8f>::cmp_ge},
    {"vec16f cmp_ge", Compare::Bits, &F32<Vec16f>::cmp_ge},
    {"vec4f select", Compare::Bits, &F32<Vec4f>::select},
    {"vec8f select", Compare::Bits, &F32<Vec8f>::select},
    {"vec16f select", Compare::Bits, &F32<Vec16f>::select},
    {"vec4f truncate", Compare::F32Value, &F32<Vec4f>::truncate},
    {"vec8f truncate", Compare::F32Value, &F32<Vec8f>::truncate},
    {"vec16f truncate", Compare::F32Value, &F32<Vec16f>::truncate},
    {"vec4f round_nearest", Compare::F32Value, &F32<Vec4f>::round_nearest},
    {"vec8f round_nearest", Compare::F32Value, &F32<Vec8f>::round_nearest},
    {"vec16f round_nearest", Compare::F32Value, &F32<Vec16f>::round_nearest},
    {"vec4f convert_truncate", Compare::Bits, &F32<Vec4f>::convert_truncate},
    {"vec8f convert_truncate", Compare::Bits, &F32<Vec8f>::convert_truncate},
    {"vec16f convert_truncate", Compare::Bits, &F32<Vec16f>::convert_truncate},
    {"vec4i convert_to_float", Compare::Bits, &F32<Vec4f>::convert_to_float},
    {"vec8i convert_to_float", Compare::Bits, &F32<Vec8f>::convert_to_float},
    {"vec16i convert_to_float", Compare::Bits, &F32<Vec16f>::convert_to_float},
    {"vec4f bitcast", Compare::Bits, &F32<Vec4f>::bitcast},
    {"vec8f bitcast", Compare::Bits, &F32<Vec8f>::bitcast},
    {"vec16f bitcast", Compare::Bits, &F32<Vec16f>::bitcast},
    {"vec4f bit ops", Compare::Bits, &F32<Vec4f>::bit_ops},
    {"vec8f bit ops", Compare::Bits, &F32<Vec8f>::bit_ops},
    {"vec16f bit ops", Compare::Bits, &F32<Vec16f>::bit_ops},
    {"vec4i + (wrapping)", Compare::Bits, &I32<Vec4f>::add},
    {"vec8i + (wrapping)", Compare::Bits, &I32<Vec8f>::add},
    {"vec16i + (wrapping)", Compare::Bits, &I32<Vec16f>::add},
    {"vec4i - (wrapping)", Compare::Bits, &I32<Vec4f>::sub},
    {"vec8i - (wrapping)", Compare::Bits, &I32<Vec8f>::sub},
    {"vec16i - (wrapping)", Compare::Bits, &I32<Vec16f>::sub},
    {"vec4i bit ops", Compare::Bits, &I32<Vec4f>::bit_ops},
    {"vec8i bit ops", Compare::Bits, &I32<Vec8f>::bit_ops},
    {"vec16i bit ops", Compare::Bits, &I32<Vec16f>::bit_ops},
    {"vec4i shifts", Compare::Bits, &I32<Vec4f>::shifts},
    {"vec8i shifts", Compare::Bits, &I32<Vec8f>::shifts},
    {"vec16i shifts", Compare::Bits, &I32<Vec16f>::shifts},
    {"vec4i compares", Compare::Bits, &I32<Vec4f>::compare},
    {"vec8i compares", Compare::Bits, &I32<Vec8f>::compare},
    {"vec16i compares", Compare::Bits, &I32<Vec16f>::compare},
    {"vec8f fnmadd", Compare::F32Value, &v8_fnmadd},
    {"vec16f fnmadd", Compare::F32Value, &v16_fnmadd},
    {"vec16f partial load/store", Compare::F32Value, &v16_partial},
    {"vec16f halves", Compare::Bits, &v16_halves},
    {"vec8f fmaddsub", Compare::F32Value, &v8_fmaddsub},
    {"vec8f addsub", Compare::F32Value, &v8_addsub},
    {"vec8f complex shuffles", Compare::Bits, &v8_shuffles},
    {"vec8f pair loads", Compare::Bits, &v8_loads},
    {"vec8f half stores", Compare::Bits, &v8_half_stores},
    {"vec8f partial load/store", Compare::F32Value, &v8_partial},
    {"vec8f transpose8x8", Compare::Bits, &v8_transpose},
    {"vec8f complex (de)interleave", Compare::Bits, &v8_complex},
    {"vec4d arithmetic", Compare::F64Value, &F64<Vec4d>::arith},
    {"vec8d arithmetic", Compare::F64Value, &F64<Vec8d>::arith},
    {"vec4d negate/abs/select", Compare::Bits, &F64<Vec4d>::neg_abs},
    {"vec8d negate/abs/select", Compare::Bits, &F64<Vec8d>::neg_abs},
    {"vec4d fma", Compare::F64Value, &F64<Vec4d>::fused},
    {"vec8d fma", Compare::F64Value, &F64<Vec8d>::fused},
    {"vec4d fnmadd", Compare::F64Value, &F64<Vec4d>::fnmadd},
    {"vec8d fnmadd", Compare::F64Value, &F64<Vec8d>::fnmadd},
    {"vec4d mul_add/mul_sub", Compare::F64Value, &F64<Vec4d>::mul_add},
    {"vec8d mul_add/mul_sub", Compare::F64Value, &F64<Vec8d>::mul_add},
    {"vec4d min/max/clamp", Compare::Bits, &F64<Vec4d>::min_max},
    {"vec8d min/max/clamp", Compare::Bits, &F64<Vec8d>::min_max},
    {"vec4d sqrt", Compare::F64Value, &F64<Vec4d>::sqrt},
    {"vec8d sqrt", Compare::F64Value, &F64<Vec8d>::sqrt},
    {"vec4d horizontal_sum/dot", Compare::F64Value, &F64<Vec4d>::reduce},
    {"vec8d horizontal_sum/dot", Compare::F64Value, &F64<Vec8d>::reduce},
    {"vec4d compare/select", Compare::Bits, &F64<Vec4d>::compare},
    {"vec8d compare/select", Compare::Bits, &F64<Vec8d>::compare},
    {"vec4d compare masks", Compare::Bits, &F64<Vec4d>::compare_masks},
    {"vec8d compare masks", Compare::Bits, &F64<Vec8d>::compare_masks},
    {"vec4d partial load/store", Compare::F64Value, &F64<Vec4d>::partial},
    {"vec8d partial load/store", Compare::F64Value, &F64<Vec8d>::partial},
    {"vec8d halves", Compare::Bits, &d8_halves},
    {"vec4d fmaddsub", Compare::F64Value, &d_fmaddsub},
    {"vec4d addsub", Compare::F64Value, &d_addsub},
    {"vec4d shuffles", Compare::Bits, &d_shuffles},
    {"vec4d pair loads", Compare::Bits, &d_loads},
    {"vec4d transpose4x4", Compare::Bits, &d_transpose},
    {"vec4d complex (de)interleave", Compare::Bits, &d_complex},
    {"crd_exp1/crd_log1", Compare::F64Value, &d_exp_log_core},
    {"exp/log vector twins", Compare::F64Value, &d_exp_log_twins},
    {"mat4f", Compare::F32Value, &mat4},
    {"quatf", Compare::F32Value, &quat},
};

} // namespace

constinit const Table CRD_KERNEL_TABLE(kSimdProbe) = {
    crd::math::simd::backend_name(),
    crd::math::simd::kNativeLaneWidth,
    kOps,
    sizeof(kOps) / sizeof(kOps[0]),
};

} // namespace crd::test::simd_probe
