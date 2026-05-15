// crd-units v0b-1 -- Vec<Quantity> compiles + zero-overhead + to_raw round-trip.
//
// Verifies ADR-0078 §2 D2 (Vec<Quantity> scope = element-wise only) and
// D3 (MathValue concept widens to accept Quantity).

#include <crd/math/vec.hpp>
#include <crd/units/quantity_aliases.hpp>
#include <crd/units/units.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <type_traits>

using crd::math::from_raw_vec;
using crd::math::to_raw_vec;
using crd::math::Vec3;
using crd::math::Vec3f;
using crd::units::Length;
using crd::units::Length32;
using crd::units::Length64;
using crd::units::Mass32;

TEST_CASE("Vec3<Length<f32>> is layout-equal to Vec3<f32>", "[units][vec][layout]")
{
    STATIC_REQUIRE(sizeof(Vec3<Length32>) == sizeof(Vec3f));
    STATIC_REQUIRE(alignof(Vec3<Length32>) == alignof(Vec3f));
    STATIC_REQUIRE(std::is_trivially_copyable_v<Vec3<Length32>>);
    STATIC_REQUIRE(std::is_standard_layout_v<Vec3<Length32>>);
}

TEST_CASE("Vec3<Length<f32>> default-constructs to zero", "[units][vec][ctor]")
{
    Vec3<Length32> v;
    CHECK(v.x.value == 0.0F);
    CHECK(v.y.value == 0.0F);
    CHECK(v.z.value == 0.0F);
}

TEST_CASE("Vec3<Length<f32>> element-wise addition preserves dimension",
          "[units][vec][add]")
{
    Vec3<Length32> a{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3<Length32> b{Length32{4.0F}, Length32{5.0F}, Length32{6.0F}};
    Vec3<Length32> c = a + b;
    CHECK(c.x.value == Catch::Approx(5.0));
    CHECK(c.y.value == Catch::Approx(7.0));
    CHECK(c.z.value == Catch::Approx(9.0));
}

TEST_CASE("Vec3<Length<f32>> subtraction preserves dimension",
          "[units][vec][sub]")
{
    Vec3<Length32> a{Length32{10.0F}, Length32{10.0F}, Length32{10.0F}};
    Vec3<Length32> b{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3<Length32> c = a - b;
    CHECK(c.x.value == Catch::Approx(9.0));
    CHECK(c.y.value == Catch::Approx(8.0));
    CHECK(c.z.value == Catch::Approx(7.0));
}

TEST_CASE("Vec3<Length<f32>> scaled by raw f32", "[units][vec][scale]")
{
    Vec3<Length32> v{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3<Length32> w = v * 2.0F;
    CHECK(w.x.value == Catch::Approx(2.0));
    CHECK(w.y.value == Catch::Approx(4.0));
    CHECK(w.z.value == Catch::Approx(6.0));

    Vec3<Length32> w2 = 3.0F * v;
    CHECK(w2.x.value == Catch::Approx(3.0));
}

TEST_CASE("Vec3<Length<f32>> divided by raw f32", "[units][vec][div]")
{
    Vec3<Length32> v{Length32{10.0F}, Length32{20.0F}, Length32{30.0F}};
    Vec3<Length32> w = v / 10.0F;
    CHECK(w.x.value == Catch::Approx(1.0));
    CHECK(w.y.value == Catch::Approx(2.0));
    CHECK(w.z.value == Catch::Approx(3.0));
}

TEST_CASE("Vec3<Length<f32>> negation preserves dimension",
          "[units][vec][neg]")
{
    Vec3<Length32> v{Length32{1.0F}, Length32{-2.0F}, Length32{3.0F}};
    Vec3<Length32> n = -v;
    CHECK(n.x.value == Catch::Approx(-1.0));
    CHECK(n.y.value == Catch::Approx(2.0));
    CHECK(n.z.value == Catch::Approx(-3.0));
}

TEST_CASE("Vec3<Quantity> equality compares value-wise", "[units][vec][eq]")
{
    Vec3<Length32> a{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3<Length32> b{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3<Length32> c{Length32{1.0F}, Length32{2.0F}, Length32{4.0F}};
    CHECK(a == b);
    CHECK_FALSE(a == c);
}

TEST_CASE("to_raw_vec / from_raw_vec round-trip", "[units][vec][raw]")
{
    Vec3<Length32> v{Length32{1.5F}, Length32{2.5F}, Length32{3.5F}};
    Vec3f raw = to_raw_vec(v);
    CHECK(raw.x == 1.5F);
    CHECK(raw.y == 2.5F);
    CHECK(raw.z == 3.5F);

    Vec3<Length32> tagged = from_raw_vec<crd::units::dim::Length>(raw);
    CHECK(tagged == v);
}

TEST_CASE("to_raw_vec produces bit-identical bytes to underlying storage",
          "[units][vec][raw][bits]")
{
    Vec3<Length32> v{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3f raw = to_raw_vec(v);
    // Layout is pinned identical; bit-cast must agree byte-for-byte.
    const auto* p_v = reinterpret_cast<const Vec3f*>(&v);
    CHECK(p_v->x == raw.x);
    CHECK(p_v->y == raw.y);
    CHECK(p_v->z == raw.z);
}

TEST_CASE("Vec3<Mass<f32>> and Vec3<Length<f32>> are distinct types",
          "[units][vec][type-safety]")
{
    STATIC_REQUIRE(!std::is_same_v<Vec3<Length32>, Vec3<Mass32>>);
    // The following would be a compile error -- intentional. Documented:
    //   Vec3<Length32> a{}; Vec3<Mass32> b{}; auto c = a + b;
    SUCCEED("type-distinction enforced at compile time");
}

TEST_CASE("Vec3<Length<f64>> works for the f64 precision tier",
          "[units][vec][f64]")
{
    Vec3<Length64> v;
    v.x = Length64{1.0};
    v.y = Length64{2.0};
    v.z = Length64{3.0};
    Vec3<Length64> w = v * 2.0;
    CHECK(w.x.value == Catch::Approx(2.0));
    CHECK(w.y.value == Catch::Approx(4.0));
    CHECK(w.z.value == Catch::Approx(6.0));
}

TEST_CASE("Determinism contract: Vec<Quantity> arithmetic produces bit-identical f32 to Vec<f32>",
          "[units][vec][determinism]")
{
    // Mirror the v0a determinism-contract test: prove the Quantity wrapper
    // is observable-effect-free on the underlying arithmetic.
    Vec3f raw_a{1.0F, 2.0F, 3.0F};
    Vec3f raw_b{4.0F, 5.0F, 6.0F};
    Vec3f raw_sum = (raw_a + raw_b) * 2.5F;

    Vec3<Length32> q_a{Length32{1.0F}, Length32{2.0F}, Length32{3.0F}};
    Vec3<Length32> q_b{Length32{4.0F}, Length32{5.0F}, Length32{6.0F}};
    Vec3<Length32> q_sum = (q_a + q_b) * 2.5F;

    CHECK(std::bit_cast<crd::u32>(raw_sum.x) == std::bit_cast<crd::u32>(q_sum.x.value));
    CHECK(std::bit_cast<crd::u32>(raw_sum.y) == std::bit_cast<crd::u32>(q_sum.y.value));
    CHECK(std::bit_cast<crd::u32>(raw_sum.z) == std::bit_cast<crd::u32>(q_sum.z.value));
}
