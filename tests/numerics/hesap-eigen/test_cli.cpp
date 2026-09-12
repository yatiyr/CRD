// crd-hesap-eigen v6-z — CLI registration + invocation tests for hesap.eigen.*. Pulls the module anchor so the
// static-init registration block survives the static-lib link, then drives a symmetric eigenvalue solve, a
// sparse SVD, and a FEAST interval solve through the registry's CommandArgs / CommandResult wire shape.
// Output blob layout: [nconv, values... (k), residuals... (k)] with k = (len/8 - 1)/2.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/eigen/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Force the linker to keep cli_register_eigen.cpp's static-init block.
const bool kPullEigen = (crd::hesap::eigen::register_eigen_cli_anchor(), true);

const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r)
{
    return std::get_if<cli::ResultBinaryBlob>(&r.value);
}

// Append the full (symmetric) COO triplets of a 1D Laplacian (diag 2, off-diag -1) of size n.
void laplacian_1d_coo(crd::containers::Array<crd::i64>& tr, crd::containers::Array<crd::i64>& tc,
                      crd::containers::Array<crd::f64>& vals, crd::u32 n)
{
    for (crd::u32 i = 0; i < n; ++i)
    {
        tr.push_back(i);
        tc.push_back(i);
        vals.push_back(2.0);
        if (i + 1 < n)
        {
            tr.push_back(i);
            tc.push_back(static_cast<crd::i64>(i + 1));
            vals.push_back(-1.0);
            tr.push_back(static_cast<crd::i64>(i + 1));
            tc.push_back(i);
            vals.push_back(-1.0);
        }
    }
}
} // namespace

TEST_CASE("CLI eigen: all hesap.eigen.* commands are registered", "[hesap][eigen][v6][cli]")
{
    REQUIRE(kPullEigen);
    auto& reg = cli::CommandRegistry::global();
    for (const char* name : {"hesap.eigen.sym.f32", "hesap.eigen.sym.f64", "hesap.eigen.svds.f32",
                             "hesap.eigen.svds.f64", "hesap.eigen.shift_invert.f64", "hesap.eigen.feast.f64"})
    {
        const auto* rec = reg.find(name);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI hesap.eigen.sym.f64 returns the smallest eigenvalues", "[hesap][eigen][v6][cli]")
{
    REQUIRE(kPullEigen);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 24;
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    laplacian_1d_coo(tr, tc, vals, n);

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_u64("nev", 2);
    args.set_u64("which", 0); // smallest

    const auto* rec = cli::CommandRegistry::global().find("hesap.eigen.sym.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    const crd::usize len = blob->bytes.size() / sizeof(crd::f64);
    REQUIRE(len >= 5); // nconv + 2 values + 2 residuals
    CHECK(out[0] >= 2.0); // nconv >= 2
    double got[2] = {out[1], out[2]};
    std::sort(got, got + 2);
    CHECK(std::fabs(got[0] - (2.0 - 2.0 * std::cos(kPi / (n + 1)))) < 1e-6);       // lambda_1
    CHECK(std::fabs(got[1] - (2.0 - 2.0 * std::cos(2.0 * kPi / (n + 1)))) < 1e-6); // lambda_2
}

TEST_CASE("CLI hesap.eigen.feast.f64 returns the eigenvalues in an interval", "[hesap][eigen][v6][cli]")
{
    REQUIRE(kPullEigen);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 40;
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    laplacian_1d_coo(tr, tc, vals, n);

    const double l1 = 2.0 - 2.0 * std::cos(kPi / (n + 1));
    const double l2 = 2.0 - 2.0 * std::cos(2.0 * kPi / (n + 1));
    const double l3 = 2.0 - 2.0 * std::cos(3.0 * kPi / (n + 1));

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64("lo", -0.01);
    args.set_f64("hi", 0.5 * (l2 + l3)); // capture lambda_1, lambda_2
    args.set_u64("m0", 6);

    const auto* rec = cli::CommandRegistry::global().find("hesap.eigen.feast.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    const crd::usize len = blob->bytes.size() / sizeof(crd::f64);
    REQUIRE(len == 5); // nconv + 2 values + 2 residuals (exactly 2 in the interval)
    CHECK(out[0] == 2.0); // nconv == 2
    double got[2] = {out[1], out[2]};
    std::sort(got, got + 2);
    CHECK(std::fabs(got[0] - l1) < 1e-6);
    CHECK(std::fabs(got[1] - l2) < 1e-6);
}

TEST_CASE("CLI hesap.eigen.shift_invert.f64 returns eigenvalues near a shift", "[hesap][eigen][v6][cli]")
{
    REQUIRE(kPullEigen);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n = 20;
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    laplacian_1d_coo(tr, tc, vals, n);

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", n);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_f64("sigma", 2.0); // interior of the spectrum [0, 4]; not an eigenvalue (A - 2I nonsingular)
    args.set_u64("nev", 2);

    const auto* rec = cli::CommandRegistry::global().find("hesap.eigen.shift_invert.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    const crd::usize len = blob->bytes.size() / sizeof(crd::f64);
    REQUIRE(len >= 3);    // nconv + >=1 value + >=1 residual
    CHECK(out[0] >= 1.0); // nconv >= 1
    CHECK(std::fabs(out[1] - 2.0) < 0.3); // an eigenvalue near the shift sigma = 2
}

TEST_CASE("CLI hesap.eigen.svds.f64 returns the largest singular value", "[hesap][eigen][v6][cli]")
{
    REQUIRE(kPullEigen);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 m = 12;
    const crd::u32 n = 8;
    // Tall matrix: dominant descending diagonal {8,7,...,1} + small tall-tail entries ⇒ largest sigma ~ 8.
    crd::containers::Array<crd::i64> tr(&alloc);
    crd::containers::Array<crd::i64> tc(&alloc);
    crd::containers::Array<crd::f64> vals(&alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tr.push_back(i);
        tc.push_back(i);
        vals.push_back(static_cast<crd::f64>(n - i)); // 8, 7, ..., 1
    }
    for (crd::u32 i = n; i < m; ++i)
    {
        tr.push_back(i);
        tc.push_back(static_cast<crd::i64>(i - n));
        vals.push_back(0.2);
    }

    cli::CommandArgs args{&alloc};
    args.set_u64("rows", m);
    args.set_u64("cols", n);
    args.set_i64_array("triplet_rows", {tr.data(), tr.size()});
    args.set_i64_array("triplet_cols", {tc.data(), tc.size()});
    args.set_f64_array("values", {vals.data(), vals.size()});
    args.set_u64("nsv", 1);

    const auto* rec = cli::CommandRegistry::global().find("hesap.eigen.svds.f64");
    REQUIRE(rec != nullptr);
    const cli::CommandResult r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    const auto* out = reinterpret_cast<const crd::f64*>(blob->bytes.data());
    const crd::usize len = blob->bytes.size() / sizeof(crd::f64);
    REQUIRE(len >= 3); // nconv + 1 value + 1 residual
    CHECK(out[0] >= 1.0);                 // nconv >= 1
    CHECK(std::fabs(out[1] - 8.0) < 0.3); // largest singular value ~ dominant diagonal entry 8
}
