// v9-z — CLI registration + invocation tests for hesap.ode.*. Pulls the module anchor so the static-init
// registration block survives the static-lib link, then drives canned problems through several methods and
// checks the final state against analytic answers. Blob layout: [status, nfev, njev, nlu, naccept, nreject,
// y(n)...].

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/ode/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;
using crd::f64;
using crd::usize;

namespace
{
const bool kPullOde = (crd::hesap::ode::register_ode_cli_anchor(), true);

const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }

struct Decoded
{
    const f64* v;
    usize len;
};

Decoded invoke(cli::CommandArgs& args, const char* name, const cli::CommandResult*& holder, cli::CommandResult& store)
{
    const auto* rec = cli::CommandRegistry::global().find(name);
    REQUIRE(rec != nullptr);
    store = rec->impl(args);
    holder = &store;
    const auto* blob = as_blob(store);
    REQUIRE(blob != nullptr);
    return {reinterpret_cast<const f64*>(blob->bytes.data()), blob->bytes.size() / sizeof(f64)};
}
} // namespace

TEST_CASE("CLI ode: hesap.ode.solve.f64 is registered", "[ode][cli]")
{
    REQUIRE(kPullOde);
    const auto* rec = cli::CommandRegistry::global().find("hesap.ode.solve.f64");
    REQUIRE(rec != nullptr);
    REQUIRE(rec->impl != nullptr);
}

TEST_CASE("CLI ode: decay y'=-y matches e^{-t} across methods", "[ode][cli]")
{
    REQUIRE(kPullOde);
    crd::memory::TlsfAllocator alloc(8 << 20);
    for (crd::u64 method = 0; method <= 6; ++method)
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("problem", 0);
        args.set_f64("t1", 1.0);
        args.set_u64("method", method);
        args.set_f64("rtol", 1e-9);
        args.set_f64("atol", 1e-11);
        cli::CommandResult store{&alloc};
        const cli::CommandResult* holder = nullptr;
        const Decoded d = invoke(args, "hesap.ode.solve.f64", holder, store);
        REQUIRE(d.len == 7); // status + 5 counters + 1 state
        INFO("method " << method << " status " << d.v[0] << " y=" << d.v[6]);
        CHECK(d.v[0] == 0.0); // Success
        CHECK(std::abs(d.v[6] - std::exp(-1.0)) < 1e-6);
    }
}

TEST_CASE("CLI ode: stiff Robertson (BDF) conserves mass", "[ode][cli]")
{
    REQUIRE(kPullOde);
    crd::memory::TlsfAllocator alloc(8 << 20);
    cli::CommandArgs args{&alloc};
    args.set_u64("problem", 2);
    args.set_f64("t1", 100.0);
    args.set_u64("method", 3); // BDF
    args.set_f64("rtol", 1e-8);
    args.set_f64("atol", 1e-10);
    cli::CommandResult store{&alloc};
    const cli::CommandResult* holder = nullptr;
    const Decoded d = invoke(args, "hesap.ode.solve.f64", holder, store);
    REQUIRE(d.len == 9); // status + 5 counters + 3 state
    CHECK(d.v[0] == 0.0);
    CHECK(d.v[1] > 0.0); // nfev
    const f64 sum = d.v[6] + d.v[7] + d.v[8];
    INFO("Robertson sum=" << sum);
    CHECK(std::abs(sum - 1.0) < 1e-6);
}

TEST_CASE("CLI ode: harmonic oscillator matches cos/-sin", "[ode][cli]")
{
    REQUIRE(kPullOde);
    crd::memory::TlsfAllocator alloc(8 << 20);
    cli::CommandArgs args{&alloc};
    args.set_u64("problem", 3);
    args.set_f64("t1", 2.0);
    args.set_u64("method", 0); // RK45
    args.set_f64("rtol", 1e-10);
    args.set_f64("atol", 1e-12);
    cli::CommandResult store{&alloc};
    const cli::CommandResult* holder = nullptr;
    const Decoded d = invoke(args, "hesap.ode.solve.f64", holder, store);
    REQUIRE(d.len == 8);
    CHECK(d.v[0] == 0.0);
    CHECK(std::abs(d.v[6] - std::cos(2.0)) < 1e-7);
    CHECK(std::abs(d.v[7] + std::sin(2.0)) < 1e-7);
}

TEST_CASE("CLI ode: error paths", "[ode][cli]")
{
    REQUIRE(kPullOde);
    crd::memory::TlsfAllocator alloc(8 << 20);
    auto& reg = cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.ode.solve.f64");
    REQUIRE(rec != nullptr);
    {
        cli::CommandArgs args{&alloc}; // missing problem + t1
        const cli::CommandResult r = rec->impl(args);
        CHECK(!r.ok);
    }
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("problem", 9); // out of range
        args.set_f64("t1", 1.0);
        const cli::CommandResult r = rec->impl(args);
        CHECK(!r.ok);
    }
    {
        cli::CommandArgs args{&alloc};
        args.set_u64("problem", 0);
        args.set_f64("t1", 1.0);
        args.set_u64("method", 99); // out of range
        const cli::CommandResult r = rec->impl(args);
        CHECK(!r.ok);
    }
}
