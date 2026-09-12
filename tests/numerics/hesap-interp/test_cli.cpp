// v13-z — CLI registration + invocation tests for hesap.interp.*. Pulls the module anchor so the static-init
// registration block survives the static-lib link, then drives PCHIP + cubic-spline through the command registry
// and checks the interpolation property (exact at the nodes) + monotone no-overshoot for PCHIP.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/interp/cli_anchor.hpp>
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
const bool kPullInterp = (crd::hesap::interp::register_interp_cli_anchor(), true);

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

TEST_CASE("CLI interp: hesap.interp.* commands are registered", "[interp][cli]")
{
    REQUIRE(kPullInterp);
    REQUIRE(cli::CommandRegistry::global().find("hesap.interp.pchip.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.interp.cubic_spline.f64") != nullptr);
}

TEST_CASE("CLI interp: PCHIP interpolates the nodes exactly + no overshoot on monotone data", "[interp][cli]")
{
    REQUIRE(kPullInterp);
    crd::memory::TlsfAllocator alloc(1U << 20);
    const f64 x[] = {0.0, 1.0, 2.0, 3.0, 4.0};
    const f64 y[] = {0.0, 1.0, 4.0, 9.0, 16.0}; // monotone increasing
    const f64 xq[] = {0.0, 0.5, 1.0, 2.0, 2.5, 4.0};
    cli::CommandArgs args{&alloc};
    args.set_f64_array("x", cont::ConstSpan<f64>(x, 5));
    args.set_f64_array("y", cont::ConstSpan<f64>(y, 5));
    args.set_f64_array("xq", cont::ConstSpan<f64>(xq, 6));
    cli::CommandResult store{&alloc};
    const Decoded d = invoke(args, "hesap.interp.pchip.f64", store);
    REQUIRE(d.len == 6);
    CHECK(std::abs(d.v[0] - 0.0) < 1e-12);  // node
    CHECK(std::abs(d.v[2] - 1.0) < 1e-12);  // node
    CHECK(std::abs(d.v[3] - 4.0) < 1e-12);  // node
    CHECK(std::abs(d.v[5] - 16.0) < 1e-12); // node
    // no overshoot: interior values stay within the bracketing node values (monotone data)
    CHECK(d.v[1] >= 0.0);
    CHECK(d.v[1] <= 1.0);
    CHECK(d.v[4] >= 4.0);
    CHECK(d.v[4] <= 9.0);
}

TEST_CASE("CLI interp: cubic spline interpolates the nodes exactly (natural + not-a-knot)", "[interp][cli]")
{
    REQUIRE(kPullInterp);
    crd::memory::TlsfAllocator alloc(1U << 20);
    const f64 x[] = {0.0, 1.0, 2.0, 3.0, 4.0};
    const f64 y[] = {1.0, 0.5, 2.0, -1.0, 3.0};
    const f64 xq[] = {0.0, 1.0, 2.0, 3.0, 4.0, 1.5};
    for (const char* bc : {"natural", "notaknot"})
    {
        cli::CommandArgs args{&alloc};
        args.set_f64_array("x", cont::ConstSpan<f64>(x, 5));
        args.set_f64_array("y", cont::ConstSpan<f64>(y, 5));
        args.set_f64_array("xq", cont::ConstSpan<f64>(xq, 6));
        args.set_string("bc", bc);
        cli::CommandResult store{&alloc};
        const Decoded d = invoke(args, "hesap.interp.cubic_spline.f64", store);
        REQUIRE(d.len == 6);
        for (usize i = 0; i < 5; ++i) // exact at every node
        {
            INFO("bc=" << bc << " node " << i);
            CHECK(std::abs(d.v[i] - y[i]) < 1e-10);
        }
    }
}
