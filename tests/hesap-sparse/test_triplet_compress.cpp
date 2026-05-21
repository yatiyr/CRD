// crd-hesap-sparse v1a-2 -- COO TripletBuilder -> CSR compress() +
// uncompressed insert path tests.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

using crd::hesap::Complex32;
using crd::hesap::Complex64;
namespace sp = crd::hesap::sparse;

TEST_CASE("compress builds a canonical CSR from out-of-order triplets", "[hesap][sparse][compress]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    // 3x3:  row0: (0,2)=2, (0,0)=1 ; row1: (1,1)=3 ; row2: (2,0)=4, (2,2)=5
    sp::TripletBuilder<crd::f64> b(&alloc, 3, 3);
    b.add(0, 2, 2.0);
    b.add(0, 0, 1.0);  // out of column order within row 0
    b.add(2, 2, 5.0);
    b.add(1, 1, 3.0);  // out of row order
    b.add(2, 0, 4.0);

    auto m = b.compress();
    REQUIRE(m.rows() == 3);
    REQUIRE(m.cols() == 3);
    REQUIRE(m.nnz() == 5);
    REQUIRE(m.is_compressed());

    // outer_ptr = [0,2,3,5]; columns sorted within each row.
    const auto& pat = m.pattern();
    CHECK(pat.outer_ptr[0] == 0);
    CHECK(pat.outer_ptr[1] == 2);
    CHECK(pat.outer_ptr[2] == 3);
    CHECK(pat.outer_ptr[3] == 5);
    CHECK(pat.inner_idx[0] == 0);  // row0 col0 then col2
    CHECK(pat.inner_idx[1] == 2);

    CHECK(m.coeff(0, 0) == 1.0);
    CHECK(m.coeff(0, 2) == 2.0);
    CHECK(m.coeff(1, 1) == 3.0);
    CHECK(m.coeff(2, 0) == 4.0);
    CHECK(m.coeff(2, 2) == 5.0);
    CHECK(m.coeff(0, 1) == 0.0);  // structurally absent
    CHECK(m.coeff(1, 0) == 0.0);
}

TEST_CASE("compress sums duplicate (row,col) entries in insertion order", "[hesap][sparse][compress][dedup]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    sp::TripletBuilder<crd::f64> b(&alloc, 2, 2);
    b.add(0, 0, 1.0);
    b.add(0, 0, 10.0);
    b.add(0, 0, 100.0);  // 3 duplicates -> 111.0
    b.add(1, 1, 7.0);

    auto m = b.compress();
    CHECK(m.nnz() == 2);
    CHECK(m.coeff(0, 0) == 111.0);
    CHECK(m.coeff(1, 1) == 7.0);
}

TEST_CASE("compress is bit-reproducible across runs", "[hesap][sparse][compress][determinism]")
{
    crd::memory::TlsfAllocator alloc(256 * 1024);
    auto build = [&]() {
        sp::TripletBuilder<crd::f64> b(&alloc, 4, 4);
        for (int pass = 0; pass < 3; ++pass)
        {
            b.add(3, 1, 0.1);
            b.add(0, 0, 1.0);
            b.add(2, 3, 0.25);
            b.add(0, 3, 0.5);
            b.add(3, 1, 0.2);  // duplicate accumulation
        }
        return b.compress();
    };
    auto m1 = build();
    auto m2 = build();
    REQUIRE(m1.nnz() == m2.nnz());
    REQUIRE(m1.pattern().topology_hash == m2.pattern().topology_hash);
    for (crd::usize i = 0; i < m1.nnz(); ++i)
    {
        CHECK(m1.values().values[i] == m2.values().values[i]);  // bit-exact
        CHECK(m1.pattern().inner_idx[i] == m2.pattern().inner_idx[i]);
    }
}

TEST_CASE("compress handles empty and single-element builders", "[hesap][sparse][compress][edge]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    {
        sp::TripletBuilder<crd::f64> b(&alloc, 3, 3);
        auto m = b.compress();
        CHECK(m.nnz() == 0);
        CHECK(m.coeff(1, 1) == 0.0);
        CHECK(m.pattern().outer_ptr[3] == 0);
    }
    {
        sp::TripletBuilder<crd::f64> b(&alloc, 1, 1);
        b.add(0, 0, 42.0);
        auto m = b.compress();
        CHECK(m.nnz() == 1);
        CHECK(m.coeff(0, 0) == 42.0);
    }
}

TEST_CASE("compress works for all 4 type variants", "[hesap][sparse][compress][types]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    {
        sp::TripletBuilder<crd::f32> b(&alloc, 2, 2);
        b.add(0, 1, 1.5F);
        b.add(1, 0, 2.5F);
        auto m = b.compress();
        CHECK(m.coeff(0, 1) == 1.5F);
        CHECK(m.coeff(1, 0) == 2.5F);
    }
    {
        sp::TripletBuilder<Complex32> b(&alloc, 2, 2);
        b.add(0, 0, Complex32{1.0F, 2.0F});
        b.add(0, 0, Complex32{3.0F, -1.0F});  // dup sum -> (4,1)
        auto m = b.compress();
        const Complex32 v = m.coeff(0, 0);
        CHECK(v.re == 4.0F);
        CHECK(v.im == 1.0F);
    }
    {
        sp::TripletBuilder<Complex64> b(&alloc, 2, 2);
        b.add(1, 1, Complex64{-2.0, 0.5});
        auto m = b.compress();
        const Complex64 v = m.coeff(1, 1);
        CHECK(v.re == -2.0);
        CHECK(v.im == 0.5);
    }
}

TEST_CASE("uncompressed coeff_ref inserts and keeps columns sorted", "[hesap][sparse][uncompressed]")
{
    crd::memory::TlsfAllocator alloc(128 * 1024);
    auto m = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>::make_uncompressed(&alloc, 3, 3, /*slack_per_outer=*/4);
    REQUIRE_FALSE(m.is_compressed());
    CHECK(m.nnz() == 0);

    m.coeff_ref(0, 2) = 2.0;
    m.coeff_ref(0, 0) = 1.0;  // inserted before col 2
    m.coeff_ref(0, 1) = 9.0;  // inserted between
    m.coeff_ref(2, 0) = 4.0;

    CHECK(m.nnz() == 4);
    CHECK(m.coeff(0, 0) == 1.0);
    CHECK(m.coeff(0, 1) == 9.0);
    CHECK(m.coeff(0, 2) == 2.0);
    CHECK(m.coeff(2, 0) == 4.0);
    CHECK(m.coeff(1, 0) == 0.0);

    // coeff_ref on an existing entry returns the same slot (no new insert).
    m.coeff_ref(0, 1) = 90.0;
    CHECK(m.nnz() == 4);
    CHECK(m.coeff(0, 1) == 90.0);

    // Columns within row 0 stored ascending.
    const auto& pat = m.pattern();
    const crd::u32 s = pat.outer_ptr[0];
    CHECK(pat.inner_idx[s] == 0);
    CHECK(pat.inner_idx[s + 1] == 1);
    CHECK(pat.inner_idx[s + 2] == 2);
}

TEST_CASE("uncompressed grows when a row exceeds its initial slack", "[hesap][sparse][uncompressed][grow]")
{
    crd::memory::TlsfAllocator alloc(128 * 1024);
    auto m = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>::make_uncompressed(&alloc, 2, 16, /*slack_per_outer=*/2);
    // Insert 10 entries into row 0 -> forces multiple grows past slack=2.
    for (crd::u32 c = 0; c < 10; ++c)
    {
        m.coeff_ref(0, c) = static_cast<crd::f64>(c + 1);
    }
    CHECK(m.nnz() == 10);
    for (crd::u32 c = 0; c < 10; ++c)
    {
        CHECK(m.coeff(0, c) == static_cast<crd::f64>(c + 1));
    }
}

TEST_CASE("make_compressed compacts and matches the compressed-from-triplets hash",
          "[hesap][sparse][uncompressed][slack-invariance]")
{
    crd::memory::TlsfAllocator alloc(256 * 1024);

    // Reference: same matrix built compressed via triplets.
    sp::TripletBuilder<crd::f64> b(&alloc, 3, 3);
    b.add(0, 0, 1.0);
    b.add(0, 2, 2.0);
    b.add(1, 1, 3.0);
    b.add(2, 0, 4.0);
    auto ref = b.compress();

    // Same matrix built via uncompressed insert (slack-padded), then compacted.
    auto m = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>::make_uncompressed(&alloc, 3, 3, /*slack_per_outer=*/5);
    m.coeff_ref(2, 0) = 4.0;
    m.coeff_ref(0, 2) = 2.0;
    m.coeff_ref(0, 0) = 1.0;
    m.coeff_ref(1, 1) = 3.0;

    // Before compaction the logical hash already matches (slack-invariant).
    m.pattern().recompute_topology_hash();
    CHECK(m.pattern().topology_hash == ref.pattern().topology_hash);

    m.make_compressed();
    REQUIRE(m.is_compressed());
    REQUIRE(m.nnz() == ref.nnz());
    CHECK(m.pattern().topology_hash == ref.pattern().topology_hash);
    for (crd::usize i = 0; i < ref.nnz(); ++i)
    {
        CHECK(m.pattern().inner_idx[i] == ref.pattern().inner_idx[i]);
        CHECK(m.values().values[i] == ref.values().values[i]);
    }
}
