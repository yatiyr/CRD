#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hesap_jobs_fixture.hpp"

#include <cmath>
#include <cstring>

namespace
{
// Force the cli_register_solvers.cpp TU to be pulled in by the linker
// so its static-init block runs and registers the v0e-g solver commands.
struct AnchorPull
{
    AnchorPull() noexcept { crd::hesap::dense::register_solvers_cli_anchor(); }
};
const AnchorPull kAnchorPull;
} // namespace

using crd::hesap::cli::CommandArgs;
using crd::hesap::cli::CommandRegistry;
using crd::hesap::cli::ResultBinaryBlob;

namespace
{
const crd::hesap::cli::CommandRecord* find(const char* name)
{
    return CommandRegistry::global().find(name);
}

crd::containers::ConstSpan<crd::f64> as_f64_array(const ResultBinaryBlob& blob)
{
    return crd::containers::ConstSpan<crd::f64>{
        reinterpret_cast<const crd::f64*>(blob.bytes.data()),
        blob.bytes.size() / sizeof(crd::f64)};
}
} // namespace

TEST_CASE("CLI: all 8 v0e solver commands are registered",
          "[hesap][solver][cli]")
{
    const char* names[] = {
        "hesap.dense.solver.lu.f32",
        "hesap.dense.solver.lu.f64",
        "hesap.dense.solver.cholesky.f32",
        "hesap.dense.solver.cholesky.f64",
        "hesap.dense.solver.ldlt.f32",
        "hesap.dense.solver.ldlt.f64",
        "hesap.dense.solver.qr.f32",
        "hesap.dense.solver.qr.f64",
    };
    for (const char* n : names)
    {
        INFO("missing command: " << n);
        REQUIRE(find(n) != nullptr);
    }
}

TEST_CASE("CLI: hesap.dense.solver.lu.f64 solves A*x = b at N=4",
          "[hesap][solver][cli][lu]")
{
    (void)crd_hesap_dense_tests::hesap_jobs_listener();
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(1U * 1024U * 1024U));
    // Diagonally-dominant 4x4.
    const crd::f64 a_flat[] = {
        10.0,  2.0,  1.0,  3.0,
         1.0, 12.0,  4.0,  2.0,
         2.0,  3.0, 15.0,  1.0,
         1.0,  1.0,  2.0, 20.0};
    // x_true = [1, 2, 3, 4]; b = A·x.
    crd::f64 b_data[4] = {0, 0, 0, 0};
    const crd::f64 x_true[4] = {1.0, 2.0, 3.0, 4.0};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            b_data[i] += a_flat[i * 4 + j] * x_true[j];
        }
    }

    CommandArgs args{&alloc};
    args.set_u64("n", 4);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 16});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b_data, 4});
    const auto* rec = find("hesap.dense.solver.lu.f64");
    REQUIRE(rec != nullptr);
    const auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&r.value);
    REQUIRE(blob != nullptr);
    const auto x = as_f64_array(*blob);
    REQUIRE(x.size() == 4U);
    for (crd::usize i = 0; i < 4; ++i)
    {
        CHECK(std::abs(x[i] - x_true[i]) < 1e-10);
    }
}

TEST_CASE("CLI: hesap.dense.solver.cholesky.f64 solves SPD A*x = b at N=3",
          "[hesap][solver][cli][cholesky]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U));
    // SPD: A = [[4, 1, 0], [1, 4, 1], [0, 1, 4]] (lower-half storage).
    const crd::f64 a_flat[] = {
        4.0, 0.0, 0.0,
        1.0, 4.0, 0.0,
        0.0, 1.0, 4.0};
    const crd::f64 x_true[3] = {1.0, 2.0, 3.0};
    crd::f64 b_data[3] = {0, 0, 0};
    // Full symmetric A·x.
    const crd::f64 a_full[9] = {4, 1, 0, 1, 4, 1, 0, 1, 4};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            b_data[i] += a_full[i * 3 + j] * x_true[j];
        }
    }

    CommandArgs args{&alloc};
    args.set_u64("n", 3);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 9});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b_data, 3});
    const auto* rec = find("hesap.dense.solver.cholesky.f64");
    REQUIRE(rec != nullptr);
    const auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto x = as_f64_array(*std::get_if<ResultBinaryBlob>(&r.value));
    for (crd::usize i = 0; i < 3; ++i)
    {
        CHECK(std::abs(x[i] - x_true[i]) < 1e-12);
    }
}

TEST_CASE("CLI: hesap.dense.solver.ldlt.f64 solves indefinite at N=2",
          "[hesap][solver][cli][ldlt]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U));
    // A = [[1, 2], [2, 1]] (indefinite). x_true = [1, 2]; b = A·x = [5, 4]
    const crd::f64 a_flat[] = {1.0, 0.0, 2.0, 1.0};
    const crd::f64 b_data[] = {5.0, 4.0};

    CommandArgs args{&alloc};
    args.set_u64("n", 2);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 4});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b_data, 2});
    const auto* rec = find("hesap.dense.solver.ldlt.f64");
    REQUIRE(rec != nullptr);
    const auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto x = as_f64_array(*std::get_if<ResultBinaryBlob>(&r.value));
    CHECK(std::abs(x[0] - 1.0) < 1e-12);
    CHECK(std::abs(x[1] - 2.0) < 1e-12);
}

TEST_CASE("CLI: hesap.dense.solver.qr.f64 solves LS over-determined 4x2",
          "[hesap][solver][cli][qr][lstsq]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U));
    // Polynomial fit: A[i,:] = [1, i] for i = 0..3; x_true = [2.5, -1.5]
    // b[i] = 2.5 - 1.5 * i  → exact fit.
    const crd::f64 a_flat[] = {
        1.0, 0.0,
        1.0, 1.0,
        1.0, 2.0,
        1.0, 3.0};
    const crd::f64 b_data[] = {2.5, 1.0, -0.5, -2.0};

    CommandArgs args{&alloc};
    args.set_u64("m", 4);
    args.set_u64("n", 2);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 8});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b_data, 4});
    const auto* rec = find("hesap.dense.solver.qr.f64");
    REQUIRE(rec != nullptr);
    const auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto x = as_f64_array(*std::get_if<ResultBinaryBlob>(&r.value));
    REQUIRE(x.size() == 2U);
    CHECK(std::abs(x[0] - 2.5) < 1e-10);
    CHECK(std::abs(x[1] - (-1.5)) < 1e-10);
}

TEST_CASE("CLI: solver dispatch rejects singular matrix",
          "[hesap][solver][cli][error]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U));
    // Rank-deficient: row 2 = row 0 + row 1.
    const crd::f64 a_flat[] = {
        1.0, 2.0, 3.0,
        2.0, 4.0, 8.0,
        3.0, 6.0, 11.0};
    const crd::f64 b_data[] = {1.0, 2.0, 3.0};

    CommandArgs args{&alloc};
    args.set_u64("n", 3);
    args.set_f64_array("A", crd::containers::ConstSpan<crd::f64>{a_flat, 9});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b_data, 3});
    const auto* rec = find("hesap.dense.solver.lu.f64");
    REQUIRE(rec != nullptr);
    const auto r = rec->impl(args);
    REQUIRE_FALSE(r.ok);
}
