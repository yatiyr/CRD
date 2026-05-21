// crd-hesap-sparse v1a-3 -- CSC compress + structural queries + CLI.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/cli_anchor.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <variant>

namespace sp = crd::hesap::sparse;
using crd::hesap::cli::CommandArgs;
using crd::hesap::cli::CommandRegistry;
using crd::hesap::cli::ResultBinaryBlob;
using crd::hesap::cli::ResultScalarF64;

namespace
{
const bool kPullAnchor = (crd::hesap::sparse::register_sparse_cli_anchor(), true);

// Same 3x3 used across the suite: (0,0)=1 (0,2)=2 (1,1)=3 (2,0)=4 (2,2)=5.
template <typename T>
sp::TripletBuilder<T> make_builder(crd::memory::IAllocator* alloc)
{
    sp::TripletBuilder<T> b(alloc, 3, 3);
    b.add(0, 0, static_cast<T>(1));
    b.add(0, 2, static_cast<T>(2));
    b.add(1, 1, static_cast<T>(3));
    b.add(2, 0, static_cast<T>(4));
    b.add(2, 2, static_cast<T>(5));
    return b;
}
} // namespace

TEST_CASE("compress_csc builds a canonical CSC", "[hesap][sparse][csc]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto b = make_builder<crd::f64>(&alloc);
    auto m = b.compress_csc();

    REQUIRE(m.format == sp::SparseFormat::Csc);
    REQUIRE(m.nnz() == 5);
    const auto& pat = m.pattern();
    // outer_ptr per column = [0,2,3,5]; rows within each column sorted.
    CHECK(pat.outer_ptr[0] == 0);
    CHECK(pat.outer_ptr[1] == 2);
    CHECK(pat.outer_ptr[2] == 3);
    CHECK(pat.outer_ptr[3] == 5);
    CHECK(pat.inner_idx[0] == 0);  // col0 rows 0,2
    CHECK(pat.inner_idx[1] == 2);

    // Cross-platform golden for the canonical CSC structure (CI linux-gcc
    // re-run proves the LE byte feed is reproducible for the CSC orientation
    // too, not just CSR).
    constexpr crd::u64 kGoldenCsc = 0x7B9FDC4345878357ULL;
    CHECK(pat.topology_hash == kGoldenCsc);

    // coeff(r,c) works through the CSC orientation.
    CHECK(m.coeff(0, 0) == 1.0);
    CHECK(m.coeff(2, 0) == 4.0);
    CHECK(m.coeff(1, 1) == 3.0);
    CHECK(m.coeff(0, 2) == 2.0);
    CHECK(m.coeff(2, 2) == 5.0);
    CHECK(m.coeff(0, 1) == 0.0);
}

TEST_CASE("CSR and CSC of the same matrix hash differently but agree on coeff", "[hesap][sparse][csc]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto csr = make_builder<crd::f64>(&alloc).compress();
    auto csc = make_builder<crd::f64>(&alloc).compress_csc();

    CHECK(csr.pattern().topology_hash != csc.pattern().topology_hash);  // format differs
    for (crd::u32 r = 0; r < 3; ++r)
    {
        for (crd::u32 c = 0; c < 3; ++c)
        {
            CHECK(csr.coeff(r, c) == csc.coeff(r, c));
        }
    }
}

TEST_CASE("compress_csc is bit-reproducible", "[hesap][sparse][csc][determinism]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m1 = make_builder<crd::f64>(&alloc).compress_csc();
    auto m2 = make_builder<crd::f64>(&alloc).compress_csc();
    REQUIRE(m1.pattern().topology_hash == m2.pattern().topology_hash);
    for (crd::usize i = 0; i < m1.nnz(); ++i)
    {
        CHECK(m1.values().values[i] == m2.values().values[i]);
    }
}

TEST_CASE("structural_stats reports rows/cols/nnz/density/inner extents", "[hesap][sparse][queries]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = make_builder<crd::f64>(&alloc).compress();
    const auto s = sp::structural_stats(m);
    CHECK(s.rows == 3);
    CHECK(s.cols == 3);
    CHECK(s.nnz == 5);
    CHECK(s.density == (5.0 / 9.0));
    CHECK(s.n_outer == 3);
    CHECK(s.min_inner_nnz == 1);  // row 1 has a single entry
    CHECK(s.max_inner_nnz == 2);  // rows 0 and 2 have two entries
    CHECK(s.is_compressed);
}

TEST_CASE("inner_indices returns the sorted indices of an inner vector", "[hesap][sparse][queries]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    auto m = make_builder<crd::f64>(&alloc).compress();
    const auto row0 = sp::inner_indices(m, 0);
    REQUIRE(row0.size() == 2);
    CHECK(row0[0] == 0);
    CHECK(row0[1] == 2);
    const auto row1 = sp::inner_indices(m, 1);
    REQUIRE(row1.size() == 1);
    CHECK(row1[0] == 1);
    CHECK(sp::inner_indices(m, 99).empty());  // out of range
}

namespace
{
void set_triplets(CommandArgs& args)
{
    args.set_u64("rows", 3);
    args.set_u64("cols", 3);
    const crd::i64 r[] = {0, 0, 1, 2, 2};
    const crd::i64 c[] = {0, 2, 1, 0, 2};
    const crd::f64 v[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{r, 5});
    args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{c, 5});
    args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{v, 5});
}
} // namespace

TEST_CASE("v1a-3 CLI commands are registered", "[hesap][sparse][cli][queries]")
{
    CHECK(kPullAnchor);
    auto& reg = CommandRegistry::global();
    const char* names[] = {"hesap.sparse.to_csc.f32", "hesap.sparse.to_csc.f64", "hesap.sparse.to_csc.c32",
                           "hesap.sparse.to_csc.c64", "hesap.sparse.nnz",        "hesap.sparse.density",
                           "hesap.sparse.structural_query"};
    for (const char* n : names)
    {
        INFO(n);
        CHECK(reg.find(n) != nullptr);
    }
}

TEST_CASE("CLI nnz/density/structural_query return expected values", "[hesap][sparse][cli][queries]")
{
    auto& reg = CommandRegistry::global();
    crd::memory::TlsfAllocator alloc(64 * 1024);

    {
        CommandArgs args(&alloc);
        set_triplets(args);
        const auto res = reg.find("hesap.sparse.nnz")->impl(args);
        REQUIRE(res.ok);
        CHECK(std::get_if<ResultScalarF64>(&res.value)->value == 5.0);
    }
    {
        CommandArgs args(&alloc);
        set_triplets(args);
        const auto res = reg.find("hesap.sparse.density")->impl(args);
        REQUIRE(res.ok);
        CHECK(std::get_if<ResultScalarF64>(&res.value)->value == (5.0 / 9.0));
    }
    {
        CommandArgs args(&alloc);
        set_triplets(args);
        const auto res = reg.find("hesap.sparse.structural_query")->impl(args);
        REQUIRE(res.ok);
        const auto* blob = std::get_if<ResultBinaryBlob>(&res.value);
        REQUIRE(blob != nullptr);
        REQUIRE(blob->bytes.size() == 8 * sizeof(crd::f64));
        const auto* f = reinterpret_cast<const crd::f64*>(blob->bytes.data());
        CHECK(f[0] == 3.0);        // rows
        CHECK(f[1] == 3.0);        // cols
        CHECK(f[2] == 5.0);        // nnz
        CHECK(f[4] == 3.0);        // n_outer
        CHECK(f[5] == 1.0);        // min_inner
        CHECK(f[6] == 2.0);        // max_inner
        CHECK(f[7] == 1.0);        // is_compressed
    }
}

TEST_CASE("CLI to_csc.f64 returns column-major values", "[hesap][sparse][cli][csc]")
{
    auto& reg = CommandRegistry::global();
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args(&alloc);
    set_triplets(args);
    const auto res = reg.find("hesap.sparse.to_csc.f64")->impl(args);
    REQUIRE(res.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&res.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 5 * sizeof(crd::f64));
    const auto* f = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    // column-major: col0[1,4], col1[3], col2[2,5]
    CHECK(f[0] == 1.0);
    CHECK(f[1] == 4.0);
    CHECK(f[2] == 3.0);
    CHECK(f[3] == 2.0);
    CHECK(f[4] == 5.0);
}
