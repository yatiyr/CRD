// v13-z — CLI registration + invocation tests for hesap.quad.*. Pulls the module anchor so the static-init
// registration block survives the static-lib link, then integrates sampled x^2 through the command registry with
// each rule (trapezoid/simpson/romberg) and checks against the analytic integral.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/quadrature/cli_anchor.hpp>
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
const bool kPullQuad = (crd::hesap::quadrature::register_quadrature_cli_anchor(), true);

f64 invoke_scalar(cli::CommandArgs& args, const char* name, cli::CommandResult& store)
{
    const auto* rec = cli::CommandRegistry::global().find(name);
    REQUIRE(rec != nullptr);
    store = rec->impl(args);
    const auto* s = std::get_if<cli::ResultScalarF64>(&store.value);
    REQUIRE(s != nullptr);
    return s->value;
}
} // namespace

TEST_CASE("CLI quad: hesap.quad.* commands are registered", "[quadrature][cli]")
{
    REQUIRE(kPullQuad);
    REQUIRE(cli::CommandRegistry::global().find("hesap.quad.samples.f64") != nullptr);
}

TEST_CASE("CLI quad: samples integration matches the analytic integral of x^2", "[quadrature][cli]")
{
    REQUIRE(kPullQuad);
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize n = 9; // 2^3 + 1 ⇒ romberg is valid
    const f64 dx = 0.25;
    cont::Array<f64> y(&alloc);
    y.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 x = static_cast<f64>(i) * dx;
        y[i] = x * x;
    }
    const f64 exact = 8.0 / 3.0; // ∫_0^2 x^2 dx

    // simpson (default): exact for a quadratic
    {
        cli::CommandArgs args{&alloc};
        args.set_f64_array("y", cont::ConstSpan<f64>(y.data(), n));
        args.set_f64("dx", dx);
        cli::CommandResult store{&alloc};
        CHECK(std::abs(invoke_scalar(args, "hesap.quad.samples.f64", store) - exact) < 1e-12);
    }
    // romberg (n = 2^k + 1): converges to the exact value
    {
        cli::CommandArgs args{&alloc};
        args.set_f64_array("y", cont::ConstSpan<f64>(y.data(), n));
        args.set_f64("dx", dx);
        args.set_string("rule", "romberg");
        cli::CommandResult store{&alloc};
        CHECK(std::abs(invoke_scalar(args, "hesap.quad.samples.f64", store) - exact) < 1e-9);
    }
    // trapezoid: approximate but close
    {
        cli::CommandArgs args{&alloc};
        args.set_f64_array("y", cont::ConstSpan<f64>(y.data(), n));
        args.set_f64("dx", dx);
        args.set_string("rule", "trapezoid");
        cli::CommandResult store{&alloc};
        CHECK(std::abs(invoke_scalar(args, "hesap.quad.samples.f64", store) - exact) < 0.05);
    }
}
