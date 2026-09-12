// v15-z — CLI registration + invocation tests for hesap.ad.*. Pulls the module anchor so the static-init block
// survives the static-lib link, then drives the canned functions through the gradient / hessian / taylor commands
// and checks against analytic answers.

#include <crd/hesap/autodiff/cli_anchor.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
const bool kPullAd = (crd::hesap::autodiff::register_autodiff_cli_anchor(), true);

const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }

struct Decoded
{
    const f64* v;
    usize      len;
};

Decoded invoke(cli::CommandArgs& args, const char* name, cli::CommandResult& store)
{
    const auto* rec = cli::CommandRegistry::global().find(name);
    REQUIRE(rec != nullptr);
    store            = rec->impl(args);
    const auto* blob = as_blob(store);
    REQUIRE(blob != nullptr);
    return {reinterpret_cast<const f64*>(blob->bytes.data()), blob->bytes.size() / sizeof(f64)};
}
} // namespace

TEST_CASE("CLI ad: commands are registered", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    for (const char* n : {"hesap.ad.gradient.f64", "hesap.ad.hessian.f64", "hesap.ad.taylor.f64",
                          "hesap.ad.rgradient.f64", "hesap.ad.jacobian.f64", "hesap.ad.hvp.f64", "hesap.ad.implicit.f64"})
    {
        const auto* rec = cli::CommandRegistry::global().find(n);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->impl != nullptr);
    }
}

TEST_CASE("CLI ad: gradient of sphere and Rosenbrock", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(8 << 20);
    // sphere Σxᵢ² at [1,2,3] → f=14, ∇=[2,4,6]
    {
        const f64        x[3] = {1.0, 2.0, 3.0};
        cli::CommandArgs args{&alloc};
        args.set_u64("func", 1);
        args.set_f64_array("x", {x, 3});
        cli::CommandResult store{&alloc};
        const Decoded      d = invoke(args, "hesap.ad.gradient.f64", store);
        REQUIRE(d.len == 4); // f + 3 grad
        CHECK_THAT(d.v[0], WithinRel(14.0, 1e-12));
        CHECK_THAT(d.v[1], WithinRel(2.0, 1e-12));
        CHECK_THAT(d.v[2], WithinRel(4.0, 1e-12));
        CHECK_THAT(d.v[3], WithinRel(6.0, 1e-12));
    }
    // Rosenbrock at [0.5,0.5] → ∇ = [-51, 50]
    {
        const f64        x[2] = {0.5, 0.5};
        cli::CommandArgs args{&alloc};
        args.set_u64("func", 0);
        args.set_f64_array("x", {x, 2});
        cli::CommandResult store{&alloc};
        const Decoded      d = invoke(args, "hesap.ad.gradient.f64", store);
        REQUIRE(d.len == 3);
        CHECK_THAT(d.v[1], WithinRel(-51.0, 1e-12));
        CHECK_THAT(d.v[2], WithinRel(50.0, 1e-12));
    }
}

TEST_CASE("CLI ad: Hessian of sphere is 2*I", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const f64        x[2] = {1.5, -2.0};
    cli::CommandArgs args{&alloc};
    args.set_u64("func", 1); // sphere
    args.set_f64_array("x", {x, 2});
    cli::CommandResult store{&alloc};
    const Decoded      d = invoke(args, "hesap.ad.hessian.f64", store);
    REQUIRE(d.len == 5); // f + 2x2 Hessian
    CHECK_THAT(d.v[1], WithinRel(2.0, 1e-12)); // H00
    CHECK_THAT(d.v[2], WithinAbs(0.0, 1e-12)); // H01
    CHECK_THAT(d.v[3], WithinAbs(0.0, 1e-12)); // H10
    CHECK_THAT(d.v[4], WithinRel(2.0, 1e-12)); // H11
}

TEST_CASE("CLI ad: Taylor coefficients of exp", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(8 << 20);
    cli::CommandArgs args{&alloc};
    args.set_u64("func", 0); // exp
    args.set_f64("x0", 0.5);
    args.set_u64("order", 8);
    cli::CommandResult store{&alloc};
    const Decoded      d = invoke(args, "hesap.ad.taylor.f64", store);
    REQUIRE(d.len == 9); // a_0..a_8
    f64 fact = 1.0;
    for (int k = 0; k <= 8; ++k)
    {
        CHECK_THAT(d.v[k], WithinRel(std::exp(0.5) / fact, 1e-12));
        fact *= static_cast<f64>(k + 1);
    }
}

TEST_CASE("CLI ad: reverse gradient matches analytic (sphere/cubes/exp)", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const f64                  x[4] = {1.0, 2.0, 3.0, 0.5};
    // sphere ∇=2x
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("func", 1);
        args.set_f64_array("x", {x, 4});
        cli::CommandResult store{&alloc};
        const Decoded      d = invoke(args, "hesap.ad.rgradient.f64", store);
        REQUIRE(d.len == 5); // f + 4 grad
        CHECK_THAT(d.v[0], WithinRel(1.0 + 4.0 + 9.0 + 0.25, 1e-12));
        for (int i = 0; i < 4; ++i) { CHECK_THAT(d.v[1 + i], WithinRel(2.0 * x[i], 1e-12)); }
    }
    // cubes ∇=3x²
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("func", 2);
        args.set_f64_array("x", {x, 4});
        cli::CommandResult store{&alloc};
        const Decoded      d = invoke(args, "hesap.ad.rgradient.f64", store);
        for (int i = 0; i < 4; ++i) { CHECK_THAT(d.v[1 + i], WithinRel(3.0 * x[i] * x[i], 1e-12)); }
    }
    // exp-sum ∇=exp(x)
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("func", 3);
        args.set_f64_array("x", {x, 4});
        cli::CommandResult store{&alloc};
        const Decoded      d = invoke(args, "hesap.ad.rgradient.f64", store);
        for (int i = 0; i < 4; ++i) { CHECK_THAT(d.v[1 + i], WithinRel(std::exp(x[i]), 1e-12)); }
    }
}

TEST_CASE("CLI ad: reverse Jacobian of the coupled map", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const f64                  x[3] = {2.0, -1.0, 0.5}; // f_j = x_j² + x_{(j+1)%3}
    cli::CommandArgs           args{&alloc};
    args.set_f64_array("x", {x, 3});
    cli::CommandResult store{&alloc};
    const Decoded      d = invoke(args, "hesap.ad.jacobian.f64", store);
    REQUIRE(d.len == 9); // 3x3 row-major
    for (int j = 0; j < 3; ++j)
    {
        for (int i = 0; i < 3; ++i)
        {
            f64 expect = 0.0;
            if (i == j) { expect += 2.0 * x[j]; }
            if (i == (j + 1) % 3) { expect += 1.0; }
            CHECK_THAT(d.v[j * 3 + i], WithinAbs(expect, 1e-12));
        }
    }
}

TEST_CASE("CLI ad: HVP of sphere is 2v", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const f64                  x[3] = {1.0, 2.0, -3.0};
    const f64                  v[3] = {0.5, -1.0, 2.0};
    cli::CommandArgs           args{&alloc};
    args.set_u64("func", 1); // sphere: ∇=2x, H=2I ⇒ H·v = 2v
    args.set_f64_array("x", {x, 3});
    args.set_f64_array("v", {v, 3});
    cli::CommandResult store{&alloc};
    const Decoded      d = invoke(args, "hesap.ad.hvp.f64", store);
    REQUIRE(d.len == 6); // grad(3) + hv(3)
    for (int i = 0; i < 3; ++i) { CHECK_THAT(d.v[i], WithinRel(2.0 * x[i], 1e-12)); }
    for (int i = 0; i < 3; ++i) { CHECK_THAT(d.v[3 + i], WithinRel(2.0 * v[i], 1e-12)); }
}

TEST_CASE("CLI ad: implicit-diff root x*=sqrt(theta), grad=1/(2 sqrt theta)", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(16 << 20);
    const f64                  theta[3] = {4.0, 9.0, 0.25};
    cli::CommandArgs           args{&alloc};
    args.set_f64_array("theta", {theta, 3});
    cli::CommandResult store{&alloc};
    const Decoded      d = invoke(args, "hesap.ad.implicit.f64", store);
    REQUIRE(d.len == 6); // x*(3) + dL/dtheta(3)
    for (int i = 0; i < 3; ++i) { CHECK_THAT(d.v[i], WithinRel(std::sqrt(theta[i]), 1e-12)); }
    for (int i = 0; i < 3; ++i) { CHECK_THAT(d.v[3 + i], WithinRel(1.0 / (2.0 * std::sqrt(theta[i])), 1e-10)); }
}

TEST_CASE("CLI ad: error paths", "[autodiff][cli]")
{
    REQUIRE(kPullAd);
    crd::memory::TlsfAllocator alloc(8 << 20);
    const auto* grad = cli::CommandRegistry::global().find("hesap.ad.gradient.f64");
    REQUIRE(grad != nullptr);
    {
        cli::CommandArgs args{&alloc}; // missing x
        CHECK(!grad->impl(args).ok);
    }
    {
        const f64        x[2] = {1.0, 2.0};
        cli::CommandArgs args{&alloc};
        args.set_u64("func", 9); // out of range
        args.set_f64_array("x", {x, 2});
        CHECK(!grad->impl(args).ok);
    }
}
