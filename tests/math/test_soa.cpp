// AoSoA storage substrate tests — Phase 3.1 v0b.
//
// Strategy: build a small TChunk with a couple of Vec8f/Vec4f columns, push
// it through Soa<T, Lane>'s API, and check the chunk-vs-lane bookkeeping
// (size / chunk_count / last_chunk_active_lanes), iteration coverage,
// chunk/lane index decomposition round-trip, and gather/scatter parity
// across chunk boundaries.

#include <catch2/catch_test_macros.hpp>

#include <crd/math/simd/simd.hpp>

#include <bit>
#include <cstring>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::math::simd::Vec4f;
using crd::math::simd::Vec8f;
using crd::math::simd::Soa;
using crd::math::simd::soa_for_each_chunk;
using crd::math::simd::soa_for_each_lane;
using crd::math::simd::gather8;
using crd::math::simd::scatter8;
using crd::math::simd::gather4;
using crd::math::simd::scatter4;

namespace
{
[[nodiscard]] bool bit_eq(f32 a, f32 b) noexcept
{
    return std::bit_cast<crd::u32>(a) == std::bit_cast<crd::u32>(b);
}

// 8-lane chunk: two columns. Each chunk holds 8 logical entities.
struct alignas(32) Body8
{
    Vec8f pos_x;
    Vec8f vel_x;
};

// 4-lane chunk: two columns. Each chunk holds 4 logical entities.
struct alignas(16) Body4
{
    Vec4f pos_x;
    Vec4f vel_x;
};
}  // namespace

// ===========================================================================
// Static layout pins (catch alignment / lane-width regressions at compile time)
// ===========================================================================

TEST_CASE("soa Lane=8 chunk is 32-byte aligned and Vec8f-sized columns",
          "[simd][soa]")
{
    STATIC_REQUIRE(alignof(Body8) >= 32);
    STATIC_REQUIRE(sizeof(Body8) == 64);  // 2 × 32 B columns
    STATIC_REQUIRE(Soa<Body8, 8>::lanes_per_chunk == 8);
}

TEST_CASE("soa Lane=4 chunk is 16-byte aligned and Vec4f-sized columns",
          "[simd][soa]")
{
    STATIC_REQUIRE(alignof(Body4) >= 16);
    STATIC_REQUIRE(sizeof(Body4) == 32);  // 2 × 16 B columns
    STATIC_REQUIRE(Soa<Body4, 4>::lanes_per_chunk == 4);
}

// ===========================================================================
// Sizing + chunk-count math
// ===========================================================================

TEST_CASE("soa empty state", "[simd][soa]")
{
    Soa<Body8, 8> s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0U);
    REQUIRE(s.chunk_count() == 0U);
    REQUIRE(s.last_chunk_active_lanes() == 0U);
}

TEST_CASE("soa resize rounds up to whole chunks; logical size preserved",
          "[simd][soa]")
{
    Soa<Body8, 8> s;

    s.resize(0);  REQUIRE(s.size() == 0U);  REQUIRE(s.chunk_count() == 0U);
    s.resize(1);  REQUIRE(s.size() == 1U);  REQUIRE(s.chunk_count() == 1U);
    s.resize(7);  REQUIRE(s.size() == 7U);  REQUIRE(s.chunk_count() == 1U);
    s.resize(8);  REQUIRE(s.size() == 8U);  REQUIRE(s.chunk_count() == 1U);
    s.resize(9);  REQUIRE(s.size() == 9U);  REQUIRE(s.chunk_count() == 2U);
    s.resize(16); REQUIRE(s.size() == 16U); REQUIRE(s.chunk_count() == 2U);
    s.resize(17); REQUIRE(s.size() == 17U); REQUIRE(s.chunk_count() == 3U);
}

TEST_CASE("soa last_chunk_active_lanes math is correct",
          "[simd][soa]")
{
    Soa<Body8, 8> s;
    for (usize n = 1; n <= 24; ++n)
    {
        s.resize(n);
        const usize expected = ((n - 1) % 8) + 1;
        REQUIRE(s.last_chunk_active_lanes() == expected);
    }
}

TEST_CASE("soa Lane=4 sizing math", "[simd][soa]")
{
    Soa<Body4, 4> s;
    s.resize(0);  REQUIRE(s.chunk_count() == 0U);
    s.resize(1);  REQUIRE(s.chunk_count() == 1U); REQUIRE(s.last_chunk_active_lanes() == 1U);
    s.resize(4);  REQUIRE(s.chunk_count() == 1U); REQUIRE(s.last_chunk_active_lanes() == 4U);
    s.resize(5);  REQUIRE(s.chunk_count() == 2U); REQUIRE(s.last_chunk_active_lanes() == 1U);
    s.resize(7);  REQUIRE(s.chunk_count() == 2U); REQUIRE(s.last_chunk_active_lanes() == 3U);
    s.resize(13); REQUIRE(s.chunk_count() == 4U); REQUIRE(s.last_chunk_active_lanes() == 1U);
}

TEST_CASE("soa clear resets state", "[simd][soa]")
{
    Soa<Body8, 8> s;
    s.resize(20);
    REQUIRE(s.chunk_count() == 3U);
    s.clear();
    REQUIRE(s.empty());
    REQUIRE(s.chunk_count() == 0U);
    REQUIRE(s.last_chunk_active_lanes() == 0U);
}

// ===========================================================================
// Chunk/lane decomposition round-trip
// ===========================================================================

TEST_CASE("soa chunk_of / lane_of / make_index round-trip",
          "[simd][soa]")
{
    using S = Soa<Body8, 8>;
    for (usize i = 0; i < 64; ++i)
    {
        const usize ci = S::chunk_of(i);
        const usize li = S::lane_of(i);
        REQUIRE(ci == i / 8);
        REQUIRE(li == i % 8);
        REQUIRE(S::make_index(ci, li) == i);
    }
}

TEST_CASE("soa Lane=4 chunk_of / lane_of math", "[simd][soa]")
{
    using S = Soa<Body4, 4>;
    REQUIRE(S::chunk_of(0)  == 0U); REQUIRE(S::lane_of(0)  == 0U);
    REQUIRE(S::chunk_of(3)  == 0U); REQUIRE(S::lane_of(3)  == 3U);
    REQUIRE(S::chunk_of(4)  == 1U); REQUIRE(S::lane_of(4)  == 0U);
    REQUIRE(S::chunk_of(15) == 3U); REQUIRE(S::lane_of(15) == 3U);
}

// ===========================================================================
// Iteration helpers
// ===========================================================================

TEST_CASE("soa_for_each_chunk visits every chunk exactly once with right active count",
          "[simd][soa]")
{
    Soa<Body8, 8> s;
    s.resize(20);  // 3 chunks: 8, 8, 4 active

    usize chunk_count_seen   = 0;
    usize total_active_seen  = 0;
    soa_for_each_chunk(s, [&](Body8& /*chunk*/, usize active)
    {
        ++chunk_count_seen;
        total_active_seen += active;
    });

    REQUIRE(chunk_count_seen   == 3U);
    REQUIRE(total_active_seen  == 20U);  // sum of active counts == logical size
}

TEST_CASE("soa_for_each_chunk reports Lane for full chunks and partial for last",
          "[simd][soa]")
{
    Soa<Body8, 8> s;
    s.resize(17);  // 8 + 8 + 1

    crd::containers::Array<usize> actives;
    soa_for_each_chunk(s, [&](Body8&, usize active) { actives.push_back(active); });

    REQUIRE(actives.size() == 3U);
    REQUIRE(actives[0] == 8U);
    REQUIRE(actives[1] == 8U);
    REQUIRE(actives[2] == 1U);
}

TEST_CASE("soa_for_each_chunk on empty Soa is a no-op",
          "[simd][soa]")
{
    Soa<Body8, 8> s;
    int call_count = 0;
    soa_for_each_chunk(s, [&](Body8&, usize) { ++call_count; });
    REQUIRE(call_count == 0);
}

TEST_CASE("soa_for_each_lane visits every logical entity exactly once",
          "[simd][soa]")
{
    Soa<Body8, 8> s;
    s.resize(13);  // 8 + 5

    usize calls_seen = 0;
    soa_for_each_lane(s, [&](Body8&, usize /*lane*/) { ++calls_seen; });
    REQUIRE(calls_seen == 13U);
}

// ===========================================================================
// Round-trip via iteration: write per-lane, read per-chunk
// ===========================================================================

TEST_CASE("soa write per-lane via for_each_lane, read per-chunk via for_each_chunk",
          "[simd][soa]")
{
    Soa<Body8, 8> s;
    s.resize(12);  // 8 + 4

    // Populate pos_x as global_index * 1.0F.
    usize logical_idx = 0;
    soa_for_each_lane(s, [&](Body8& chunk, usize lane)
    {
        f32 col[8];
        chunk.pos_x.store(col);
        col[lane] = static_cast<f32>(logical_idx++);
        chunk.pos_x = Vec8f::load(col);
    });
    REQUIRE(logical_idx == 12U);

    // Read back per-chunk and verify.
    f32 read[12];
    usize cursor = 0;
    soa_for_each_chunk(s, [&](Body8& chunk, usize active)
    {
        f32 col[8];
        chunk.pos_x.store(col);
        for (usize i = 0; i < active; ++i)
        {
            read[cursor++] = col[i];
        }
    });

    REQUIRE(cursor == 12U);
    for (usize i = 0; i < 12; ++i)
    {
        REQUIRE(bit_eq(read[i], static_cast<f32>(i)));
    }
}

// ===========================================================================
// Gather / Scatter — Vec8f columns
// ===========================================================================

TEST_CASE("soa gather8 reads 8 lanes by global index",
          "[simd][soa][gather]")
{
    Soa<Body8, 8> s;
    s.resize(16);  // 2 chunks

    // Populate pos_x[i] = i * 10.0F across both chunks.
    usize logical_idx = 0;
    soa_for_each_lane(s, [&](Body8& chunk, usize lane)
    {
        f32 col[8]; chunk.pos_x.store(col);
        col[lane] = static_cast<f32>(logical_idx++) * 10.0F;
        chunk.pos_x = Vec8f::load(col);
    });

    // Gather 8 indices spanning both chunks.
    const u32 idx[8] = { 0, 7, 8, 15, 1, 6, 9, 14 };
    const Vec8f g = gather8(s, &Body8::pos_x, idx);
    f32 out[8]; g.store(out);

    REQUIRE(bit_eq(out[0], 0.0F));   // entity 0
    REQUIRE(bit_eq(out[1], 70.0F));  // entity 7 (last lane of chunk 0)
    REQUIRE(bit_eq(out[2], 80.0F));  // entity 8 (first lane of chunk 1)
    REQUIRE(bit_eq(out[3], 150.0F)); // entity 15 (last lane of chunk 1)
    REQUIRE(bit_eq(out[4], 10.0F));
    REQUIRE(bit_eq(out[5], 60.0F));
    REQUIRE(bit_eq(out[6], 90.0F));
    REQUIRE(bit_eq(out[7], 140.0F));
}

TEST_CASE("soa scatter8 writes 8 lanes by global index; round-trip via gather8",
          "[simd][soa][scatter]")
{
    Soa<Body8, 8> s;
    s.resize(16);

    const u32   idx[8]  = { 0, 7, 8, 15, 1, 6, 9, 14 };
    const Vec8f write_v(1.5F, -2.5F, 3.5F, -4.5F, 5.5F, -6.5F, 7.5F, -8.5F);
    scatter8(s, &Body8::pos_x, idx, write_v);

    const Vec8f read_v = gather8(s, &Body8::pos_x, idx);
    f32 expected[8]; write_v.store(expected);
    f32 got[8];      read_v.store(got);

    for (usize i = 0; i < 8; ++i)
    {
        REQUIRE(bit_eq(got[i], expected[i]));
    }
}

TEST_CASE("soa scatter8 leaves untouched lanes intact",
          "[simd][soa][scatter]")
{
    Soa<Body8, 8> s;
    s.resize(16);

    // Fill chunk 0 with sentinel 99.0F across all lanes.
    Body8& c0 = s.chunk(0);
    c0.pos_x = Vec8f(99.0F);
    Body8& c1 = s.chunk(1);
    c1.pos_x = Vec8f(99.0F);

    // Scatter a single lane into entity 5.
    const u32   idx[8] = { 5, 5, 5, 5, 5, 5, 5, 5 };  // all to lane 5
    const Vec8f vals(11.0F);  // last write to lane 5 wins (= 11.0)
    scatter8(s, &Body8::pos_x, idx, vals);

    // Lanes 0..4, 6..7 of chunk 0 must still be 99.0F.
    f32 c0_lanes[8]; c0.pos_x.store(c0_lanes);
    for (usize i = 0; i < 8; ++i)
    {
        if (i == 5) REQUIRE(bit_eq(c0_lanes[i], 11.0F));
        else        REQUIRE(bit_eq(c0_lanes[i], 99.0F));
    }
    // Chunk 1 untouched.
    f32 c1_lanes[8]; c1.pos_x.store(c1_lanes);
    for (usize i = 0; i < 8; ++i) REQUIRE(bit_eq(c1_lanes[i], 99.0F));
}

// ===========================================================================
// Gather / Scatter — Vec4f columns
// ===========================================================================

TEST_CASE("soa gather4 / scatter4 round-trip on Vec4f columns",
          "[simd][soa][gather]")
{
    Soa<Body4, 4> s;
    s.resize(8);  // 2 chunks of 4 lanes

    const u32   idx[4]  = { 0, 3, 4, 7 };
    const Vec4f write_v(2.0F, -3.0F, 4.0F, -5.0F);
    scatter4(s, &Body4::pos_x, idx, write_v);

    const Vec4f read_v = gather4(s, &Body4::pos_x, idx);
    f32 e[4]; write_v.store(e);
    f32 g[4]; read_v.store(g);

    for (usize i = 0; i < 4; ++i)
    {
        REQUIRE(bit_eq(g[i], e[i]));
    }
}

// ===========================================================================
// SIMD-style chunk update: position += velocity * dt, over all chunks
// ===========================================================================

TEST_CASE("soa SIMD-style integration: pos += vel * dt across all chunks",
          "[simd][soa][parity]")
{
    Soa<Body8, 8> s;
    s.resize(20);  // 3 chunks

    // Init pos_x[i] = 0, vel_x[i] = 1 for every entity.
    soa_for_each_chunk(s, [&](Body8& c, usize /*active*/)
    {
        c.pos_x = Vec8f::zero();
        c.vel_x = Vec8f::one();
    });

    const Vec8f dt_v(0.25F);
    constexpr int kSteps = 4;
    for (int step = 0; step < kSteps; ++step)
    {
        soa_for_each_chunk(s, [&](Body8& c, usize /*active*/)
        {
            c.pos_x = mul_add(c.vel_x, dt_v, c.pos_x);
        });
    }

    // After 4 steps of dt=0.25 with vel=1.0, pos must equal 1.0 in every lane
    // (including the unused tail lanes, which were also init'd above).
    soa_for_each_chunk(s, [&](Body8& c, usize /*active*/)
    {
        f32 col[8]; c.pos_x.store(col);
        for (usize i = 0; i < 8; ++i)
        {
            REQUIRE(bit_eq(col[i], 1.0F));
        }
    });
}
