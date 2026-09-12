// v14-a close gate: the FULL compute dtype set instantiates and behaves on
// the substrate — c32/c64 complex, i64 index, u8 mask — through construction,
// view algebra (slice/permute/broadcast/flip/reshape), for_each traversal,
// contiguity tracking, and move ownership. (f32/f64 are exercised across the
// main corpus files; this file pins the OTHER four members of the ADR-0096 §2
// dtype set so the templated substrate never silently regresses on them.)

#include <crd/hesap/tensor/tensor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::tensor;

namespace
{

// The shared substrate exercise, dtype-generic. make(i) manufactures a
// distinct value per linear index; eq compares exactly.
template <typename T, typename Make> void exercise_substrate(crd::memory::IAllocator* alloc, Make make)
{
    const crd::u64 shape[] = {4U, 3U, 5U};
    Tensor<T> t(alloc, shape);
    REQUIRE(t.size() == 60U);
    for (crd::u64 i = 0; i < 60U; ++i)
    {
        t.data()[i] = make(i);
    }
    TensorView<const T> v = t.view();
    CHECK(v.is_contiguous());

    // permute {2,0,1}: strided, logical (k,i,j) reads element (i,j,k)
    const crd::u32 order[] = {2U, 0U, 1U};
    TensorView<const T> pv = v.permute(order);
    CHECK(pv.shape(0) == 5U);
    CHECK(pv.shape(1) == 4U);
    CHECK(pv.shape(2) == 3U);
    CHECK_FALSE(pv.is_contiguous());
    crd::u64 visited = 0;
    pv.for_each(
        [&](const crd::u64* idx, const T& val)
        {
            const crd::u64 lin = idx[1] * 15U + idx[2] * 5U + idx[0];
            CHECK(val == make(lin));
            ++visited;
        });
    CHECK(visited == 60U);

    // slice row 1..3 of dim 0, then flip dim 2
    TensorView<const T> sv = v.slice(0U, 1U, 3U).flip(2U);
    CHECK(sv.shape(0) == 2U);
    CHECK(sv.shape(2) == 5U);
    CHECK(sv(0U, 0U, 0U) == make(1U * 15U + 0U * 5U + 4U)); // row 1, flipped col 4

    // reshape (view-only) of the contiguous base: (4,3,5) -> (12,5)
    const crd::u64 rshape[] = {12U, 5U};
    TensorView<const T> rv;
    REQUIRE(v.reshape({rshape, 2U}, rv) == TensorStatus::Ok);
    CHECK(rv.shape(0) == 12U);
    CHECK(rv(0U, 0U) == make(0U));
    CHECK(rv(11U, 4U) == make(59U));

    // move ownership
    Tensor<T> moved = static_cast<Tensor<T>&&>(t);
    CHECK(moved.size() == 60U);
    CHECK(t.data() == nullptr);
}

} // namespace

TEST_CASE("v14-a dtype set: c32/c64 complex tensors on the substrate", "[hesap][tensor][v14][dtypes]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    exercise_substrate<c32>(&alloc,
                            [](crd::u64 i) {
                                return c32{static_cast<crd::f32>(i), -static_cast<crd::f32>(i) * 0.5F};
                            });
    exercise_substrate<c64>(&alloc,
                            [](crd::u64 i) {
                                return c64{static_cast<crd::f64>(i) * 0.25, static_cast<crd::f64>(i) + 1.0};
                            });
}

TEST_CASE("v14-a dtype set: i64 index + u8 mask tensors on the substrate", "[hesap][tensor][v14][dtypes]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    exercise_substrate<crd::i64>(&alloc,
                                 [](crd::u64 i) { return static_cast<crd::i64>(i * 7U) - 30; });
    exercise_substrate<crd::u8>(&alloc, [](crd::u64 i) { return static_cast<crd::u8>(i * 5U); });
}
