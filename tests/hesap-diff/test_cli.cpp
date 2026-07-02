// v13-z — CLI registration + invocation tests for hesap.diff.*. Pulls the module anchor so the static-init
// registration block survives the static-lib link, then checks the Fornberg central stencils and Savitzky-Golay
// derivative through the command registry.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/diff/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;

namespace
{
const bool kPullDiff = (crd::hesap::diff::register_diff_cli_anchor(), true);

const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }

struct Decoded
{
    const f64* v;
    usize len;
};

Decoded invoke(cli::CommandArgs& args, const char* name, cli::CommandResult& store)
{
    const auto* rec = cli::CommandRegistry::global().find(name);
    REQUIRE(rec != nullptr);
    store = rec->impl(args);
    const auto* blob = as_blob(store);
    REQUIRE(blob != nullptr);
    return {reinterpret_cast<const f64*>(blob->bytes.data()), blob->bytes.size() / sizeof(f64)};
}
} // namespace

TEST_CASE("CLI diff: hesap.diff.* commands are registered", "[diff][cli]")
{
    REQUIRE(kPullDiff);
    REQUIRE(cli::CommandRegistry::global().find("hesap.diff.savgol.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.diff.fornberg.f64") != nullptr);
}

TEST_CASE("CLI diff: Fornberg reproduces the classic central stencils", "[diff][cli]")
{
    REQUIRE(kPullDiff);
    crd::memory::TlsfAllocator alloc(1U << 18);
    const f64 nodes[] = {-1.0, 0.0, 1.0};
    cli::CommandArgs args{&alloc};
    args.set_f64_array("nodes", cont::ConstSpan<f64>(nodes, 3));
    args.set_f64("z", 0.0);
    args.set_i64("max_deriv", 2);
    cli::CommandResult store{&alloc};
    const Decoded d = invoke(args, "hesap.diff.fornberg.f64", store);
    REQUIRE(d.len == 9); // (2+1) rows * 3 nodes
    // k=0 (value at 0):        [0, 1, 0]
    CHECK(std::abs(d.v[0] - 0.0) < 1e-12);
    CHECK(std::abs(d.v[1] - 1.0) < 1e-12);
    CHECK(std::abs(d.v[2] - 0.0) < 1e-12);
    // k=1 (first derivative):  [-1/2, 0, 1/2]
    CHECK(std::abs(d.v[3] - (-0.5)) < 1e-12);
    CHECK(std::abs(d.v[4] - 0.0) < 1e-12);
    CHECK(std::abs(d.v[5] - 0.5) < 1e-12);
    // k=2 (second derivative): [1, -2, 1]
    CHECK(std::abs(d.v[6] - 1.0) < 1e-12);
    CHECK(std::abs(d.v[7] - (-2.0)) < 1e-12);
    CHECK(std::abs(d.v[8] - 1.0) < 1e-12);
}

TEST_CASE("CLI diff: Savitzky-Golay derivative of a line recovers its slope", "[diff][cli]")
{
    REQUIRE(kPullDiff);
    crd::memory::TlsfAllocator alloc(1U << 18);
    const usize n = 9;
    cont::Array<f64> y(&alloc);
    y.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        y[i] = 2.0 * static_cast<f64>(i) + 1.0; // slope 2, order-1 poly (exact for polyorder >= 1)
    }
    cli::CommandArgs args{&alloc};
    args.set_f64_array("y", cont::ConstSpan<f64>(y.data(), n));
    args.set_i64("window", 5);
    args.set_i64("polyorder", 2);
    args.set_i64("deriv", 1);
    args.set_f64("delta", 1.0);
    cli::CommandResult store{&alloc};
    const Decoded d = invoke(args, "hesap.diff.savgol.f64", store);
    REQUIRE(d.len == n);
    for (usize i = 2; i + 2 < n; ++i) // interior (away from the mirrored edges): derivative == slope 2
    {
        INFO("savgol deriv at " << i);
        CHECK(std::abs(d.v[i] - 2.0) < 1e-9);
    }
}
