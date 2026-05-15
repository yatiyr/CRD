// crd-time -- Instant + Duration tests.

#include <crd/time/duration.hpp>
#include <crd/time/instant.hpp>
#include <crd/units/literals.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
using namespace crd::time;
using namespace crd::units::literals;
using crd::f64;
using crd::i64;
} // namespace

TEST_CASE("Duration: alias to Quantity<dim::Time, f64>", "[d-006][duration]")
{
    Duration d{1.5};
    CHECK(d.value == 1.5);

    Duration sum = d + d;
    CHECK(sum.value == 3.0);

    Duration diff = sum - d;
    CHECK(diff.value == 1.5);

    Duration scaled = d * 2.0;
    CHECK(scaled.value == 3.0);
}

TEST_CASE("Duration: ingress from crd-units UDLs", "[d-006][duration]")
{
    Duration one_sec = 1.0_s;
    CHECK(one_sec.value == 1.0);

    Duration one_ms = 1.0_ms;
    CHECK(one_ms.value == 0.001);

    Duration one_us = 1.0_us;
    CHECK(one_us.value == 1e-6);

    Duration one_min = 1.0_min;
    CHECK(one_min.value == 60.0);
}

TEST_CASE("Instant: default construction = epoch", "[d-006][instant]")
{
    Instant t;
    CHECK(t.ns_since_epoch() == 0);
}

TEST_CASE("Instant: explicit ns construction", "[d-006][instant]")
{
    Instant t{1'000'000'000};  // 1 second
    CHECK(t.ns_since_epoch() == 1'000'000'000);
}

TEST_CASE("Instant: subtraction yields Duration in seconds", "[d-006][instant]")
{
    Instant a{1'000'000'000};   // 1 sec
    Instant b{3'500'000'000};   // 3.5 sec

    Duration elapsed = b - a;
    CHECK(elapsed.value == 2.5);
}

TEST_CASE("Instant: addition with Duration moves forward", "[d-006][instant]")
{
    Instant t{1'000'000'000};
    Duration d{0.5};
    Instant later = t + d;
    CHECK(later.ns_since_epoch() == 1'500'000'000);
}

TEST_CASE("Instant: subtraction with Duration moves backward", "[d-006][instant]")
{
    Instant t{1'000'000'000};
    Duration d{0.25};
    Instant earlier = t - d;
    CHECK(earlier.ns_since_epoch() == 750'000'000);
}

TEST_CASE("Instant: compound assignment", "[d-006][instant]")
{
    Instant t{1'000'000'000};
    t += Duration{0.5};
    CHECK(t.ns_since_epoch() == 1'500'000'000);
    t -= Duration{0.25};
    CHECK(t.ns_since_epoch() == 1'250'000'000);
}

TEST_CASE("Instant: comparison ordering", "[d-006][instant]")
{
    Instant a{1000};
    Instant b{2000};
    Instant c{1000};

    CHECK(a == c);
    CHECK(a != b);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a <= c);
    CHECK(a >= c);
}

TEST_CASE("Instant + UDL Duration", "[d-006][instant]")
{
    Instant t{0};
    Instant later = t + 1.5_s;
    CHECK(later.ns_since_epoch() == 1'500'000'000);

    Instant much_later = t + 1.0_min;
    CHECK(much_later.ns_since_epoch() == 60'000'000'000);
}

TEST_CASE("Instant subtraction: 60 second interval", "[d-006][instant]")
{
    Instant start{0};
    Instant end{60'000'000'000};  // 60 seconds in ns

    Duration d = end - start;
    // 60e9 ns * 1e-9 in f64 has 1 ULP drift from the literal 60.0.
    CHECK(std::abs(d.value - 60.0) < 1e-12);

    // Convert to minutes via crd-units boundary accessor (1 ULP tolerance —
    // f64 division of 60.0 / 60.0 may drift slightly through Minute::factor).
    f64 in_min = crd::units::value_in<crd::units::Minute>(d);
    CHECK(std::abs(in_min - 1.0) < 1e-14);
}
