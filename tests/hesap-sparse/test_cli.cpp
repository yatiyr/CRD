// crd-hesap-sparse v1a-2 -- CLI command tests.
//
// Exercises the static-init registered sparse commands through the global
// CommandRegistry, proving the agent-native plumbing end to end.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/sparse/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <variant>

using crd::hesap::cli::CommandArgs;
using crd::hesap::cli::CommandRegistry;
using crd::hesap::cli::ResultScalarF64;

namespace
{
// Reference the anchor so the linker keeps cli_register_sparse.cpp's static-init.
const bool kPullAnchor = (crd::hesap::sparse::register_sparse_cli_anchor(), true);

// Triplets for a 3x3: (0,0),(0,2),(1,1),(2,0),(2,2) -> 5 distinct nnz.
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

TEST_CASE("sparse CLI commands are registered (12 ops)", "[hesap][sparse][cli]")
{
    CHECK(kPullAnchor);
    auto& reg = CommandRegistry::global();
    const char* names[] = {
        "hesap.sparse.from_triplets.f32", "hesap.sparse.from_triplets.f64", "hesap.sparse.from_triplets.c32",
        "hesap.sparse.from_triplets.c64", "hesap.sparse.to_csr.f32",        "hesap.sparse.to_csr.f64",
        "hesap.sparse.to_csr.c32",        "hesap.sparse.to_csr.c64",        "hesap.sparse.build.f32",
        "hesap.sparse.build.f64",         "hesap.sparse.build.c32",         "hesap.sparse.build.c64"};
    for (const char* n : names)
    {
        INFO(n);
        CHECK(reg.find(n) != nullptr);
    }
}

TEST_CASE("sparse CLI from_triplets.f64 returns nnz", "[hesap][sparse][cli]")
{
    auto& reg = CommandRegistry::global();
    const auto* rec = reg.find("hesap.sparse.from_triplets.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args(&alloc);
    set_triplets(args);

    const auto res = rec->impl(args);
    REQUIRE(res.ok);
    const auto* scalar = std::get_if<ResultScalarF64>(&res.value);
    REQUIRE(scalar != nullptr);
    CHECK(scalar->value == 5.0);
}

TEST_CASE("sparse CLI build.f64 compressed and uncompressed paths agree", "[hesap][sparse][cli]")
{
    auto& reg = CommandRegistry::global();
    const auto* rec = reg.find("hesap.sparse.build.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(128 * 1024);

    CommandArgs compressed(&alloc);
    set_triplets(compressed);
    compressed.set_bool("uncompressed", false);
    const auto r1 = rec->impl(compressed);
    REQUIRE(r1.ok);

    CommandArgs uncompressed(&alloc);
    set_triplets(uncompressed);
    uncompressed.set_bool("uncompressed", true);
    const auto r2 = rec->impl(uncompressed);
    REQUIRE(r2.ok);

    const auto* s1 = std::get_if<ResultScalarF64>(&r1.value);
    const auto* s2 = std::get_if<ResultScalarF64>(&r2.value);
    REQUIRE(s1 != nullptr);
    REQUIRE(s2 != nullptr);
    CHECK(s1->value == 5.0);
    CHECK(s2->value == 5.0);  // both assembly paths produce the same nnz
}

TEST_CASE("sparse CLI rejects mismatched triplet array lengths", "[hesap][sparse][cli]")
{
    auto& reg = CommandRegistry::global();
    const auto* rec = reg.find("hesap.sparse.from_triplets.f64");
    REQUIRE(rec != nullptr);

    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandArgs args(&alloc);
    args.set_u64("rows", 2);
    args.set_u64("cols", 2);
    const crd::i64 r[] = {0, 1};
    const crd::i64 c[] = {0};  // length mismatch
    const crd::f64 v[] = {1.0, 2.0};
    args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{r, 2});
    args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{c, 1});
    args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{v, 2});

    const auto res = rec->impl(args);
    CHECK_FALSE(res.ok);
}
