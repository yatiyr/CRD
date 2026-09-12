// crd-hesap-direct v5a-1 — Frontal<T> + extend_add assembly-kernel tests.

#include <crd/hesap/direct/direct.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

namespace dir = crd::hesap::direct;

namespace
{
// Set a front's index arrays (ascending) + zero its data.
template <typename T>
void init_front(dir::Frontal<T>& f, std::initializer_list<crd::u32> rows, std::initializer_list<crd::u32> cols)
{
    f.resize(static_cast<crd::u32>(rows.size()), static_cast<crd::u32>(cols.size()));
    crd::u32 i = 0;
    for (crd::u32 r : rows)
    {
        f.row_index[i++] = r;
    }
    crd::u32 j = 0;
    for (crd::u32 c : cols)
    {
        f.col_index[j++] = c;
    }
    f.zero_fill();
}
} // namespace

TEST_CASE("Frontal resize + at() addressing is row-major", "[hesap][direct][v5a-1][frontal]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    dir::Frontal<crd::f64> f(&alloc);
    init_front(f, {0, 1, 2}, {0, 1, 2, 3});
    REQUIRE(f.nrows == 3);
    REQUIRE(f.ncols == 4);
    REQUIRE(f.data.size() == 12);
    f.at(2, 3) = 7.0;
    CHECK(f.data[2 * 4 + 3] == 7.0);
    CHECK(f.at(2, 3) == 7.0);
}

TEST_CASE("extend_add scatters a child block into the matching parent cells", "[hesap][direct][v5a-1][frontal]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    // Parent rows {0,2,5,7} x cols {1,3,4,8}; child rows {2,7} x cols {3,8}.
    dir::Frontal<crd::f64> parent(&alloc);
    init_front(parent, {0, 2, 5, 7}, {1, 3, 4, 8});
    dir::Frontal<crd::f64> child(&alloc);
    init_front(child, {2, 7}, {3, 8});
    child.at(0, 0) = 10.0; // (row 2, col 3)
    child.at(0, 1) = 20.0; // (row 2, col 8)
    child.at(1, 0) = 30.0; // (row 7, col 3)
    child.at(1, 1) = 40.0; // (row 7, col 8)

    dir::extend_add(parent, child, &alloc);

    // global row 2 -> parent local 1; row 7 -> 3; col 3 -> 1; col 8 -> 3.
    CHECK(parent.at(1, 1) == 10.0);
    CHECK(parent.at(1, 3) == 20.0);
    CHECK(parent.at(3, 1) == 30.0);
    CHECK(parent.at(3, 3) == 40.0);
    // Everything else stays zero (sum over all entries == sum of the four).
    crd::f64 total = 0.0;
    for (crd::usize k = 0; k < parent.data.size(); ++k)
    {
        total += parent.data[k];
    }
    CHECK(total == 100.0);
}

TEST_CASE("extend_add accumulates (two children sum into the parent)", "[hesap][direct][v5a-1][frontal]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    dir::Frontal<crd::f64> parent(&alloc);
    init_front(parent, {0, 1, 2}, {0, 1, 2});
    dir::Frontal<crd::f64> a(&alloc);
    init_front(a, {0, 2}, {0, 2});
    a.at(0, 0) = 1.0;
    a.at(1, 1) = 2.0;
    dir::Frontal<crd::f64> b(&alloc);
    init_front(b, {2}, {2});
    b.at(0, 0) = 5.0;

    dir::extend_add(parent, a, &alloc);
    dir::extend_add(parent, b, &alloc);
    CHECK(parent.at(0, 0) == 1.0);
    CHECK(parent.at(2, 2) == 7.0); // 2 from a + 5 from b
}

TEST_CASE("extend_add on a symmetric (Cholesky-style) front: row_index == col_index",
          "[hesap][direct][v5a-1][frontal]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    // A Cholesky front is symmetric: the same global ids index rows and cols.
    dir::Frontal<crd::f64> parent(&alloc);
    init_front(parent, {3, 6, 9}, {3, 6, 9});
    dir::Frontal<crd::f64> sch(&alloc); // a child's Schur complement over {6,9}
    init_front(sch, {6, 9}, {6, 9});
    sch.at(0, 0) = 1.0;
    sch.at(0, 1) = -2.0;
    sch.at(1, 0) = -2.0;
    sch.at(1, 1) = 4.0;

    dir::extend_add(parent, sch, &alloc);
    CHECK(parent.at(1, 1) == 1.0);  // (6,6)
    CHECK(parent.at(1, 2) == -2.0); // (6,9)
    CHECK(parent.at(2, 1) == -2.0); // (9,6)
    CHECK(parent.at(2, 2) == 4.0);  // (9,9)
    CHECK(parent.at(0, 0) == 0.0);  // (3,3) untouched
}

TEST_CASE("extend_add is order-independent in value (commutative assembly)", "[hesap][direct][v5a-1][frontal]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    auto build = [&](bool a_first)
    {
        dir::Frontal<crd::f64> p(&alloc);
        init_front(p, {0, 1, 2, 3}, {0, 1, 2, 3});
        dir::Frontal<crd::f64> a(&alloc);
        init_front(a, {0, 2}, {0, 2});
        a.at(0, 0) = 3.0;
        a.at(1, 1) = 5.0;
        dir::Frontal<crd::f64> b(&alloc);
        init_front(b, {1, 2}, {1, 2});
        b.at(0, 0) = 7.0;
        b.at(1, 1) = 11.0;
        if (a_first)
        {
            dir::extend_add(p, a, &alloc);
            dir::extend_add(p, b, &alloc);
        }
        else
        {
            dir::extend_add(p, b, &alloc);
            dir::extend_add(p, a, &alloc);
        }
        crd::f64 s = 0.0;
        for (crd::usize k = 0; k < p.data.size(); ++k)
        {
            s += p.data[k];
        }
        return s;
    };
    CHECK(build(true) == build(false)); // 3 + 5 + 7 + 11 either way
    CHECK(build(true) == 26.0);
}
