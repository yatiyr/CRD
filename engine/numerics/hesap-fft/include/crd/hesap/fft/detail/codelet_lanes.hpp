// The lanes of the generated FFT codelet bodies (SIMD-DISPATCH, ADR-0134 §5, ADR-0063).
//
// A generated body is a DAG of lane-wise IEEE operations over tiles of four f64 or eight f32 columns (kTile). The tile
// is the unit of every body's column ranges on every level, so which columns the tiles and a scalar tail compute, and
// so their bits, never depend on the level. Optimized builds above the portable level run a tile as one Vec4d or Vec8f
// (TileLane) and, at avx512, two tiles as one Vec8d or Vec16f (Lane), an odd last tile on a TileLane. Unoptimized
// builds and the portable level run the same body one column at a time on scalars: the same operations on every
// column, so the same bits, with a quarter or an eighth of the code. The portable vectors are arrays (a hierarchical
// codelet unit took 10.9 GB at /O2, 2026-10-28), and unoptimized builds give every temporary its own stack slot (whole
// SIMD bodies needed 1.2-1.4 MB frames, MSVC /Od /RTC1, 2026-07-03). __OPTIMIZE__ covers optimized GCC/Clang builds
// without NDEBUG; MSVC's optimized configurations all define NDEBUG here. Only codelet units include this header.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>

#if (defined(NDEBUG) || defined(__OPTIMIZE__)) && !CRD_SIMD_IS_SCALAR
#define CRD_FFT_GEN_SIMD_BODY
// Where a lane is one register (avx2, avx512) the codelet units compile each tile body whole, the largest in parts
// under MSVC's optimization limit: the split edition's slot traffic cost those levels up to a fifth of a large
// transform (fft_codegen_split.py). Elsewhere a lane is two registers or a column, and the parts bound the optimizer.
#if CRD_SIMD_HAS_AVX2
#define CRD_FFT_GEN_WHOLE_BODY
#endif
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec8d.hpp>
#endif
#endif

namespace crd::hesap::fft::lanes::inline CRD_MATH_LEVEL_NS
{

// The columns of a tile: 32 bytes of each type on every level.
template <typename T> inline constexpr crd::usize kTile = 32 / sizeof(T);

// LaneOf<T>::Tile computes one tile a step and LaneOf<T>::Wide the most whole tiles a register holds; kWidth<V> counts
// a step's columns.
template <typename T> struct LaneOf
{
    using Tile = T;
    using Wide = T;
};
template <typename V> inline constexpr crd::usize kWidth = 1;

#ifdef CRD_FFT_GEN_SIMD_BODY

template <> struct LaneOf<crd::f64>
{
    using Tile = crd::math::simd::Vec4d;
#if CRD_SIMD_HAS_AVX512
    using Wide = crd::math::simd::Vec8d;
#else
    using Wide = Tile;
#endif
};
template <> struct LaneOf<crd::f32>
{
    using Tile = crd::math::simd::Vec8f;
#if CRD_SIMD_HAS_AVX512
    using Wide = crd::math::simd::Vec16f;
#else
    using Wide = Tile;
#endif
};
template <> inline constexpr crd::usize kWidth<crd::math::simd::Vec4d> = 4;
template <> inline constexpr crd::usize kWidth<crd::math::simd::Vec8f> = 8;
#if CRD_SIMD_HAS_AVX512
template <> inline constexpr crd::usize kWidth<crd::math::simd::Vec8d> = 8;
template <> inline constexpr crd::usize kWidth<crd::math::simd::Vec16f> = 16;
#endif

using crd::math::simd::fma;
using crd::math::simd::fnmadd;
using crd::math::simd::load_complex_deinterleaved;
using crd::math::simd::store_complex_interleaved;

template <typename V, typename T> [[nodiscard]] CRD_FORCEINLINE V load(const T* p) noexcept
{
    return V::load(p);
}
template <typename V, typename T> CRD_FORCEINLINE void store(T* p, V v) noexcept
{
    v.store(p);
}

#else

// The vector operations' column semantics: fma and fnmadd round once, as the instructions and libm::fma do.
template <typename T> [[nodiscard]] CRD_FORCEINLINE T fma(T a, T b, T c) noexcept
{
    return crd::math::libm::fma(a, b, c);
}
template <typename T> [[nodiscard]] CRD_FORCEINLINE T fnmadd(T a, T b, T c) noexcept
{
    return crd::math::libm::fma(-a, b, c);
}
template <typename V, typename T> [[nodiscard]] CRD_FORCEINLINE V load(const T* p) noexcept
{
    return *p;
}
template <typename V, typename T> CRD_FORCEINLINE void store(T* p, V v) noexcept
{
    *p = v;
}
template <typename T> CRD_FORCEINLINE void load_complex_deinterleaved(const T* p, T& re, T& im) noexcept
{
    re = p[0];
    im = p[1];
}
template <typename T> CRD_FORCEINLINE void store_complex_interleaved(T* p, T re, T im) noexcept
{
    p[0] = re;
    p[1] = im;
}

#endif

template <typename T> using Lane = typename LaneOf<T>::Wide;
template <typename T> using TileLane = typename LaneOf<T>::Tile;
// Lane<T> holds two tiles: a body's last odd tile runs on a TileLane.
template <typename T> inline constexpr bool kPairedTiles = kWidth<Lane<T>> != kWidth<TileLane<T>>;

// The batched pipelines' AoSoA rows hold blocks of kTile real parts, then kTile imaginary parts: column c of a row sits
// at c / kTile * 2 * kTile + c % kTile and its imaginary part kTile further on. A step starts on a tile, so a TileLane
// is one block and a Lane of two tiles two adjacent blocks. The layout is the same on every level.
template <typename V, typename T> CRD_FORCEINLINE void load_block(const T* row, crd::usize t, V& re, V& im) noexcept
{
    constexpr crd::usize w = kTile<T>;
    if constexpr (kWidth<V> == 1)
    {
        const T* const at = row + t / w * 2 * w + t % w;
        re = at[0];
        im = at[w];
    }
    else if constexpr (kWidth<V> == w)
    {
        re = V::load(row + 2 * t);
        im = V::load(row + 2 * t + w);
    }
    else
    {
        using H = TileLane<T>;
        re = V(H::load(row + 2 * t), H::load(row + 2 * t + 2 * w));
        im = V(H::load(row + 2 * t + w), H::load(row + 2 * t + 3 * w));
    }
}
template <typename V, typename T> CRD_FORCEINLINE void store_block(T* row, crd::usize t, V re, V im) noexcept
{
    constexpr crd::usize w = kTile<T>;
    if constexpr (kWidth<V> == 1)
    {
        T* const at = row + t / w * 2 * w + t % w;
        at[0] = re;
        at[w] = im;
    }
    else if constexpr (kWidth<V> == w)
    {
        re.store(row + 2 * t);
        im.store(row + 2 * t + w);
    }
    else
    {
        low_half(re).store(row + 2 * t);
        high_half(re).store(row + 2 * t + 2 * w);
        low_half(im).store(row + 2 * t + w);
        high_half(im).store(row + 2 * t + 3 * w);
    }
}

template <typename H, crd::usize N> CRD_FORCEINLINE void transpose_tile(H (&x)[N]) noexcept
{
    if constexpr (N == 4)
    {
        transpose4x4(x[0], x[1], x[2], x[3]);
    }
    else
    {
        transpose8x8(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
    }
}

// Stores a step's group of kTile outputs transposed: column t + c of the step takes the group's values at
// p + c * stride, as an AoSoA block row of the pipelines' first stage. On scalars a step is one column and p[l] takes
// output l.
template <typename T, typename V, typename... R>
CRD_FORCEINLINE void store_transposed(T* p, crd::usize stride, V x0, R... rest) noexcept
{
    constexpr crd::usize w = kTile<T>;
    static_assert(sizeof...(R) + 1 == w, "A group holds one tile of outputs");
    V x[w] = {x0, rest...};
    if constexpr (kWidth<V> == 1)
    {
        for (crd::usize l = 0; l < w; ++l)
        {
            p[l] = x[l];
        }
    }
    else if constexpr (kWidth<V> == w)
    {
        transpose_tile(x);
        for (crd::usize c = 0; c < w; ++c)
        {
            x[c].store(p + c * stride);
        }
    }
    else
    {
        TileLane<T> lo[w];
        TileLane<T> hi[w];
        for (crd::usize l = 0; l < w; ++l)
        {
            lo[l] = low_half(x[l]);
            hi[l] = high_half(x[l]);
        }
        transpose_tile(lo);
        transpose_tile(hi);
        for (crd::usize c = 0; c < w; ++c)
        {
            lo[c].store(p + c * stride);
            hi[c].store(p + (w + c) * stride);
        }
    }
}

} // namespace crd::hesap::fft::lanes::inline CRD_MATH_LEVEL_NS
