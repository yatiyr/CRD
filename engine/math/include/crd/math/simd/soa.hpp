// Soa<TChunk, Lane> — AoSoA storage substrate. Phase 3.1 v0b.
//
// User defines TChunk explicitly as a SIMD-friendly struct holding columns
// of Lane f32s (typically Vec8f columns for Lane=8 on AVX2, Vec4f columns
// for Lane=4 elsewhere). Soa stores Array<TChunk> with chunk-aligned
// growth + tracks the logical entity count separately so the last chunk
// can be partially populated without breaking iteration.
//
// SIMD path:  soa_for_each_chunk — lambda gets (chunk&, active_lane_count).
// Slow path:  soa_for_each_lane  — lambda gets (chunk&, lane_idx) per entity.
// Cross-chunk: gather8 / scatter8 / gather4 / scatter4 move 8/4 lanes
//              by global index across chunk boundaries.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::math::simd
{
template <typename TChunk, usize Lane = k_native_lane_width>
class Soa
{
public:
    static_assert(Lane > 0,
                  "Soa lane width must be positive");
    static_assert(Lane == 4 || Lane == 8,
                  "Soa lane widths supported in v0b: 4 (Vec4f columns) or 8 (Vec8f columns)");
    static_assert(alignof(TChunk) >= 16,
                  "Soa TChunk must be at least 16-byte aligned (SIMD requirement)");

    static constexpr usize lanes_per_chunk = Lane;
    using ChunkType = TChunk;

    explicit Soa(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept
        : m_chunks(alloc)
    {
    }

    // ---- size / capacity ----

    [[nodiscard]] CRD_FORCEINLINE usize size()        const noexcept { return m_logical_size; }
    [[nodiscard]] CRD_FORCEINLINE usize chunk_count() const noexcept { return m_chunks.size(); }
    [[nodiscard]] CRD_FORCEINLINE bool  empty()       const noexcept { return m_logical_size == 0; }

    // 0 when empty; 1..Lane otherwise. Lane for full final chunk.
    [[nodiscard]] CRD_FORCEINLINE usize last_chunk_active_lanes() const noexcept
    {
        if (m_logical_size == 0) return 0;
        return ((m_logical_size - 1) % Lane) + 1;
    }

    // Resize to `logical_size` entities. Underlying chunk array rounds up
    // to ceil(n / Lane). New chunks are value-initialised (zero-filled
    // columns; safe to read but caller should still populate before
    // SIMD ops to avoid relying on the zero default).
    void resize(usize logical_size)
    {
        const usize chunk_n = chunks_needed_for(logical_size);
        m_chunks.resize(chunk_n);
        m_logical_size = logical_size;
    }

    void reserve(usize logical_size)
    {
        const usize chunk_n = chunks_needed_for(logical_size);
        m_chunks.reserve(chunk_n);
    }

    void clear() noexcept
    {
        m_chunks.clear();
        m_logical_size = 0;
    }

    // ---- chunk access (the SIMD path) ----

    [[nodiscard]] CRD_FORCEINLINE TChunk&       chunk(usize i)       noexcept { return m_chunks[i]; }
    [[nodiscard]] CRD_FORCEINLINE const TChunk& chunk(usize i) const noexcept { return m_chunks[i]; }

    [[nodiscard]] crd::containers::Span<TChunk> chunks() noexcept
    {
        return crd::containers::Span<TChunk>(m_chunks.data(), m_chunks.size());
    }

    [[nodiscard]] crd::containers::ConstSpan<TChunk> chunks() const noexcept
    {
        return crd::containers::ConstSpan<TChunk>(m_chunks.data(), m_chunks.size());
    }

    // ---- index decomposition ----

    [[nodiscard]] static constexpr usize chunk_of (usize global_idx) noexcept { return global_idx / Lane; }
    [[nodiscard]] static constexpr usize lane_of  (usize global_idx) noexcept { return global_idx % Lane; }
    [[nodiscard]] static constexpr usize make_index(usize chunk_idx, usize lane_idx) noexcept
    {
        return chunk_idx * Lane + lane_idx;
    }

private:
    [[nodiscard]] static constexpr usize chunks_needed_for(usize logical) noexcept
    {
        return (logical + Lane - 1) / Lane;
    }

    crd::containers::Array<TChunk> m_chunks;
    usize                          m_logical_size = 0;
};

// ===========================================================================
// Iteration helpers
// ===========================================================================

// SIMD path. Lambda receives (chunk_ref, active_lane_count). For all chunks
// except the last, active_lane_count == Lane. The last chunk reports the
// actual active lane count (1..Lane) so callers can mask off the partial
// tail when correctness requires it.
template <typename TChunk, usize Lane, typename Fn>
CRD_FORCEINLINE void soa_for_each_chunk(Soa<TChunk, Lane>& soa, Fn&& fn)
{
    const usize n = soa.chunk_count();
    if (n == 0) return;
    for (usize i = 0; i + 1 < n; ++i)
    {
        fn(soa.chunk(i), Lane);
    }
    fn(soa.chunk(n - 1), soa.last_chunk_active_lanes());
}

template <typename TChunk, usize Lane, typename Fn>
CRD_FORCEINLINE void soa_for_each_chunk(const Soa<TChunk, Lane>& soa, Fn&& fn)
{
    const usize n = soa.chunk_count();
    if (n == 0) return;
    for (usize i = 0; i + 1 < n; ++i)
    {
        fn(soa.chunk(i), Lane);
    }
    fn(soa.chunk(n - 1), soa.last_chunk_active_lanes());
}

// Slow path. One callback per logical entity; lambda receives
// (chunk_ref, lane_idx). For ad-hoc lookups, debug inspection, and
// small-N code paths where SIMD setup overhead exceeds the work.
template <typename TChunk, usize Lane, typename Fn>
void soa_for_each_lane(Soa<TChunk, Lane>& soa, Fn&& fn)
{
    using SoaT = Soa<TChunk, Lane>;
    const usize n = soa.size();
    for (usize i = 0; i < n; ++i)
    {
        fn(soa.chunk(SoaT::chunk_of(i)), SoaT::lane_of(i));
    }
}

template <typename TChunk, usize Lane, typename Fn>
void soa_for_each_lane(const Soa<TChunk, Lane>& soa, Fn&& fn)
{
    using SoaT = Soa<TChunk, Lane>;
    const usize n = soa.size();
    for (usize i = 0; i < n; ++i)
    {
        fn(soa.chunk(SoaT::chunk_of(i)), SoaT::lane_of(i));
    }
}

// ===========================================================================
// Gather / Scatter — Vec8f columns
// ===========================================================================
//
// Software implementation (extract-and-pack). Hardware gather
// (_mm256_i32gather_ps) is faster on some micro-architectures but its
// performance characteristics + edge-case rounding vary across CPUs;
// substrate keeps the determinism contract straightforward by going
// scalar-by-scalar. Hardware-gather fast path is reserved for v0e
// benchmark-driven optimisation, not v0b substrate work.

template <typename TChunk, usize Lane>
[[nodiscard]] CRD_FORCEINLINE Vec8f gather8(
    const Soa<TChunk, Lane>& soa,
    Vec8f TChunk::*           member,
    const u32 (&indices)[8]) noexcept
{
    using SoaT = Soa<TChunk, Lane>;
    f32 v[8];
    for (usize i = 0; i < 8; ++i)
    {
        const usize ci = SoaT::chunk_of(indices[i]);
        const usize li = SoaT::lane_of(indices[i]);
        v[i] = (soa.chunk(ci).*member).lane(li);
    }
    return Vec8f(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

template <typename TChunk, usize Lane>
CRD_FORCEINLINE void scatter8(
    Soa<TChunk, Lane>& soa,
    Vec8f TChunk::*    member,
    const u32 (&indices)[8],
    Vec8f              values) noexcept
{
    using SoaT = Soa<TChunk, Lane>;
    f32 in[8]; values.store(in);
    for (usize i = 0; i < 8; ++i)
    {
        const usize ci = SoaT::chunk_of(indices[i]);
        const usize li = SoaT::lane_of(indices[i]);
        // Round-trip through stack: Vec8f doesn't expose a per-lane write,
        // so we read the column, modify the lane, write back.
        f32 col[8];
        (soa.chunk(ci).*member).store(col);
        col[li] = in[i];
        soa.chunk(ci).*member = Vec8f::load(col);
    }
}

// ===========================================================================
// Gather / Scatter — Vec4f columns
// ===========================================================================

template <typename TChunk, usize Lane>
[[nodiscard]] CRD_FORCEINLINE Vec4f gather4(
    const Soa<TChunk, Lane>& soa,
    Vec4f TChunk::*           member,
    const u32 (&indices)[4]) noexcept
{
    using SoaT = Soa<TChunk, Lane>;
    f32 v[4];
    for (usize i = 0; i < 4; ++i)
    {
        const usize ci = SoaT::chunk_of(indices[i]);
        const usize li = SoaT::lane_of(indices[i]);
        v[i] = (soa.chunk(ci).*member).lane(li);
    }
    return Vec4f(v[0], v[1], v[2], v[3]);
}

template <typename TChunk, usize Lane>
CRD_FORCEINLINE void scatter4(
    Soa<TChunk, Lane>& soa,
    Vec4f TChunk::*    member,
    const u32 (&indices)[4],
    Vec4f              values) noexcept
{
    using SoaT = Soa<TChunk, Lane>;
    f32 in[4]; values.store(in);
    for (usize i = 0; i < 4; ++i)
    {
        const usize ci = SoaT::chunk_of(indices[i]);
        const usize li = SoaT::lane_of(indices[i]);
        f32 col[4];
        (soa.chunk(ci).*member).store(col);
        col[li] = in[i];
        soa.chunk(ci).*member = Vec4f::load(col);
    }
}

}  // namespace crd::math::simd
