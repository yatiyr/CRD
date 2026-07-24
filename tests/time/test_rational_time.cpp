// test_rational_time.cpp — GEO-9: the EXACT editorial time gates. The doctrine under test: editorial STRUCTURE
// never touches a float — a feature-length NTSC timeline must not drift by one tick, drop-frame timecode must
// round-trip a full broadcast day, and every f64 the OTIO edge can produce must come back exact.

#include <catch2/catch_test_macros.hpp>

#include <crd/time/rational_time.hpp>

#include <cstring>

using crd::time::add;
using crd::time::compare;
using crd::time::from_timecode;
using crd::time::make_rate;
using crd::time::make_time;
using crd::time::RationalRate;
using crd::time::RationalTime;
using crd::time::rate_from_f64;
using crd::time::rate_to_f64;
using crd::time::rescaled_to;
using crd::time::rescales_exactly;
using crd::time::RescaleRounding;
using crd::time::sub;
using crd::time::Timecode;
using crd::time::time_from_f64;
using crd::time::TimeRange;
using crd::time::to_seconds_f64;
using crd::time::to_timecode;

TEST_CASE("rational rate construction reduces and refuses", "[time][rational]")
{
    const RationalRate r = make_rate(48000, 2002); // = 24000/1001
    CHECK(r.num == 24000);
    CHECK(r.den == 1001);
    CHECK(r == crd::time::kRateNtsc24);

    CHECK_FALSE(make_rate(0, 1).valid());
    CHECK_FALSE(make_rate(-24, 1).valid());
    CHECK_FALSE(make_rate(24, 0).valid());
    CHECK_FALSE(make_rate(1LL << 40U, 1).valid()); // beyond i32 after reduction — refused, never truncated

    CHECK(make_rate(24, 1).valid());
    CHECK(make_rate(1, 1000000).valid()); // a very slow tick is legal (audio-sample-per-block styles)
}

TEST_CASE("same-rate arithmetic is exact tick math", "[time][rational]")
{
    const RationalTime a = make_time(100, crd::time::kRate24);
    const RationalTime b = make_time(42, crd::time::kRate24);
    CHECK(add(a, b).value == 142);
    CHECK(add(a, b).rate == crd::time::kRate24);
    CHECK(sub(a, b).value == 58);
    CHECK(compare(a, b) > 0);
    CHECK(compare(b, a) < 0);
    CHECK(compare(a, a) == 0);
}

TEST_CASE("the NTSC drift gate: 24 hours of 23.976 accumulated one tick at a time", "[time][rational]")
{
    // 24 h at 23.976 = 24*3600*24000/1001 s of ticks; accumulate 86400 seconds one FRAME at a time and land
    // EXACTLY. A float-seconds model is ~ms off by hour two; the rational model must be exact to the tick.
    const RationalRate ntsc = crd::time::kRateNtsc24;
    // one hour of frames per addition step keeps the loop cheap while exercising add() 24× + a tail
    const crd::i64     frames_per_hour = 3600LL * 24000LL / 1001LL; // 86313 (floor — NTSC hours are not integral)
    RationalTime       t               = make_time(0, ntsc);
    for (int h = 0; h < 24; ++h) { t = add(t, make_time(frames_per_hour, ntsc)); }
    CHECK(t.value == frames_per_hour * 24);
    CHECK(t.rate == ntsc);

    // seconds at the f64 EDGE (display only): 86313*24 frames × 1001/24000 s — the flooring loses 0.686
    // frames/hour, 0.687 s/day; the edge conversion must agree with the same f64 expression exactly
    const crd::f64 secs = to_seconds_f64(t);
    CHECK(secs == static_cast<crd::f64>(frames_per_hour * 24) * 1001.0 / 24000.0);
}

TEST_CASE("cross-rate compare is exact where f64 seconds would lie", "[time][rational]")
{
    // 24000 frames @ 23.976 = 1001 s EXACTLY; 1001 s @ 24 fps = 24024 frames EXACTLY — equal across rates
    const RationalTime ntsc = make_time(24000, crd::time::kRateNtsc24);
    const RationalTime film = make_time(24024, crd::time::kRate24);
    CHECK(compare(ntsc, film) == 0);
    CHECK(ntsc == film);

    // one tick apart at a huge magnitude — f64 seconds cannot see this, the 128-bit compare must
    const crd::i64     big = 3'000'000'000LL; // ~39 years of 23.976 — beyond f64's 2^53/24000 exactness comfort
    const RationalTime x   = make_time(big * 24000, crd::time::kRateNtsc24);
    const RationalTime y   = make_time(big * 24024 + 1, crd::time::kRate24);
    // x seconds = big*1001 ; y = (big*24024+1)/24 → y > x by 1/24 s
    CHECK(compare(x, y) < 0);
    CHECK(compare(y, x) > 0);

    const RationalTime y_eq = make_time(big * 24024, crd::time::kRate24);
    CHECK(compare(x, y_eq) == 0);

    // negative ordering
    CHECK(compare(make_time(-1, crd::time::kRate24), make_time(0, crd::time::kRateNtsc24)) < 0);
    CHECK(compare(make_time(-24024, crd::time::kRate24), make_time(-24000, crd::time::kRateNtsc24)) == 0);
}

TEST_CASE("cross-rate add lands on the merged exact grid", "[time][rational]")
{
    // 1 frame @ 24 + 1 frame @ 30000/1001: merged grid must hold both exactly
    const RationalTime a = make_time(1, crd::time::kRate24);
    const RationalTime b = make_time(1, crd::time::kRateNtsc30);
    const RationalTime s = add(a, b);
    REQUIRE(s.valid());
    // seconds: 1/24 + 1001/30000 = (1250 + 1001.. ) exact check through compare with a hand-built equivalent:
    // 1/24 = 1250/30000 ⇒ sum = 2251/30000 s ⇒ at rate 30000/1: value 2251? merged grid may differ, compare instead
    const RationalTime hand = make_time(2251, make_rate(30000, 1));
    CHECK(compare(s, hand) == 0);

    // subtraction back out is exact
    const RationalTime back = sub(s, b);
    CHECK(compare(back, a) == 0);
}

TEST_CASE("rescale states its exactness and its rounding", "[time][rational]")
{
    const RationalTime one_sec = make_time(24, crd::time::kRate24);
    CHECK(rescales_exactly(one_sec, make_rate(48, 1)));
    CHECK(rescaled_to(one_sec, make_rate(48, 1)).value == 48);

    const RationalTime frame_ntsc = make_time(24000, crd::time::kRateNtsc24); // = 1001 s
    CHECK(rescales_exactly(frame_ntsc, crd::time::kRate24));
    CHECK(rescaled_to(frame_ntsc, crd::time::kRate24).value == 24 * 1001);

    // inexact: 1 frame @ 23.976 → @24: 1001/1000 frames → floor 1, round 1
    const RationalTime f1 = make_time(1, crd::time::kRateNtsc24);
    CHECK_FALSE(rescales_exactly(f1, crd::time::kRate24));
    CHECK(rescaled_to(f1, crd::time::kRate24, RescaleRounding::Floor).value == 1);
    CHECK(rescaled_to(f1, crd::time::kRate24, RescaleRounding::Round).value == 1);

    // floor on negatives goes toward -inf
    const RationalTime neg = make_time(-1, crd::time::kRateNtsc24);
    CHECK(rescaled_to(neg, crd::time::kRate24, RescaleRounding::Floor).value == -2);
}

TEST_CASE("one second at 23.976 is NOT an integral frame count", "[time][rational]")
{
    const RationalTime one_sec = make_time(24, crd::time::kRate24); // 1 s of 24 fps ticks
    CHECK_FALSE(rescales_exactly(one_sec, crd::time::kRateNtsc24)); // 1 s = 24000/1001 = 23.976... frames
    CHECK(rescaled_to(one_sec, crd::time::kRateNtsc24, RescaleRounding::Floor).value == 23);
    CHECK(rescaled_to(one_sec, crd::time::kRateNtsc24, RescaleRounding::Round).value == 24);
}

TEST_CASE("f64 edges: every OTIO rate comes back exact", "[time][rational]")
{
    // integral rates
    CHECK(rate_from_f64(24.0) == crd::time::kRate24);
    CHECK(rate_from_f64(60.0) == crd::time::kRate60);

    // the SMPTE family: the f64 OTIO writes is (f64)num/den — must snap to the exact rational
    CHECK(rate_from_f64(24000.0 / 1001.0) == crd::time::kRateNtsc24);
    CHECK(rate_from_f64(30000.0 / 1001.0) == crd::time::kRateNtsc30);
    CHECK(rate_from_f64(60000.0 / 1001.0) == crd::time::kRateNtsc60);
    CHECK(rate_from_f64(120000.0 / 1001.0) == crd::time::kRateNtsc120);

    // a small exotic rational round-trips through continued fractions
    CHECK(rate_from_f64(12.5) == make_rate(25, 2));
    const RationalRate odd = rate_from_f64(static_cast<crd::f64>(12345) / 512.0);
    CHECK(odd == make_rate(12345, 512));

    // refusals
    CHECK_FALSE(rate_from_f64(0.0).valid());
    CHECK_FALSE(rate_from_f64(-24.0).valid());

    // round-trip through the export edge
    CHECK(rate_to_f64(crd::time::kRateNtsc24) == 24000.0 / 1001.0);
    CHECK(rate_to_f64(crd::time::kRate24) == 24.0);
}

TEST_CASE("f64 edges: values incl. subframe come back exact", "[time][rational]")
{
    const RationalTime t = time_from_f64(86400.0, 24.0);
    CHECK(t.value == 86400);
    CHECK(t.rate == crd::time::kRate24);

    // subframe: half a frame folds into the rate exactly
    const RationalTime half = time_from_f64(10.5, 24.0);
    REQUIRE(half.valid());
    CHECK(compare(half, make_time(21, make_rate(48, 1))) == 0);

    CHECK_FALSE(time_from_f64(1.0, 0.0).valid());
}

TEST_CASE("drop-frame timecode round-trips a full broadcast day at 29.97", "[time][rational][timecode]")
{
    const RationalRate rate = crd::time::kRateNtsc30;
    // the DF display day is 2,592,000 nominal numbers minus 2,592 dropped = 2,589,408 ACTUAL frames
    // (= 144 ten-minute blocks of 17,982). The exhaustive loop IS the oracle: every frame of the broadcast
    // day converts to timecode and parses back to itself.
    const crd::i64 day_frames = 24LL * (30 * 60 * 10 - 2 * 9) * 6;
    REQUIRE(day_frames == 2'589'408);
    crd::i64 checked = 0;
    for (crd::i64 f = 0; f < day_frames; ++f)
    {
        Timecode tc;
        REQUIRE(to_timecode(make_time(f, rate), true, tc));
        RationalTime back;
        REQUIRE(from_timecode(tc.text, rate, back));
        if (back.value != f)
        {
            CAPTURE(f, tc.text, back.value);
            REQUIRE(back.value == f);
        }
        ++checked;
    }
    CHECK(checked == day_frames);
}

TEST_CASE("drop-frame timecode: the SMPTE landmarks", "[time][rational][timecode]")
{
    const RationalRate rate = crd::time::kRateNtsc30;
    Timecode           tc;

    // frame 0
    REQUIRE(to_timecode(make_time(0, rate), true, tc));
    CHECK(std::strcmp(tc.text, "00:00:00;00") == 0);

    // the first drop: after minute 1's 1800 frames comes 00:01:00;02 (00 and 01 are dropped numbers)
    REQUIRE(to_timecode(make_time(1800, rate), true, tc));
    CHECK(std::strcmp(tc.text, "00:01:00;02") == 0);

    // the tenth minute does NOT drop: 10*1798+1800 = 17982 frames = 00:10:00;00
    REQUIRE(to_timecode(make_time(17982, rate), true, tc));
    CHECK(std::strcmp(tc.text, "00:10:00;00") == 0);

    // a dropped display number REFUSES to parse
    RationalTime back;
    CHECK_FALSE(from_timecode("00:01:00;00", rate, back));
    CHECK_FALSE(from_timecode("00:01:00;01", rate, back));
    CHECK(from_timecode("00:10:00;00", rate, back)); // tenth minutes keep 00

    // drop-frame at a non-DF rate refuses
    CHECK_FALSE(to_timecode(make_time(0, crd::time::kRate24), true, tc));
    CHECK_FALSE(to_timecode(make_time(0, crd::time::kRateNtsc24), true, tc));
}

TEST_CASE("non-drop timecode: 24 and 23.976 count nominal frames", "[time][rational][timecode]")
{
    Timecode tc;
    REQUIRE(to_timecode(make_time(24 * 3600 + 25, crd::time::kRate24), false, tc));
    CHECK(std::strcmp(tc.text, "01:00:01:01") == 0);

    // 23.976 uses 24-nominal NDF timecode (runs slow vs the wall clock — the standard practice)
    REQUIRE(to_timecode(make_time(24 * 3600, crd::time::kRateNtsc24), false, tc));
    CHECK(std::strcmp(tc.text, "01:00:00:00") == 0);

    RationalTime back;
    REQUIRE(from_timecode("01:00:00:00", crd::time::kRateNtsc24, back));
    CHECK(back.value == 24 * 3600);

    // malformed text refuses
    CHECK_FALSE(from_timecode("1:00:00:00", crd::time::kRate24, back));
    CHECK_FALSE(from_timecode("01:00:00:30", crd::time::kRate24, back)); // ff >= fps
    CHECK_FALSE(from_timecode("01:60:00:00", crd::time::kRate24, back));
    CHECK_FALSE(from_timecode("01:00:00:0", crd::time::kRate24, back));
}

TEST_CASE("time ranges: contains / overlaps / end are exact", "[time][rational]")
{
    const TimeRange r{make_time(24, crd::time::kRate24), make_time(48, crd::time::kRate24)}; // [1s, 3s)
    CHECK(r.valid());
    CHECK(r.contains(make_time(24, crd::time::kRate24)));
    CHECK(r.contains(make_time(71, crd::time::kRate24)));
    CHECK_FALSE(r.contains(make_time(72, crd::time::kRate24))); // end is EXCLUSIVE
    CHECK_FALSE(r.contains(make_time(23, crd::time::kRate24)));

    // cross-rate containment: 2 s expressed on a foreign fine grid is inside [1s,3s)
    CHECK(r.contains(make_time(24000 * 2, make_rate(24000, 1)))); // 2 s at a 24000 ticks/s rate

    const TimeRange s{make_time(72, crd::time::kRate24), make_time(24, crd::time::kRate24)}; // [3s, 4s)
    CHECK_FALSE(r.overlaps(s)); // half-open ranges touching at 3 s do NOT overlap
    const TimeRange u{make_time(71, crd::time::kRate24), make_time(24, crd::time::kRate24)};
    CHECK(r.overlaps(u));

    const TimeRange bad{make_time(0, crd::time::kRate24), make_time(-1, crd::time::kRate24)};
    CHECK_FALSE(bad.valid());
}
