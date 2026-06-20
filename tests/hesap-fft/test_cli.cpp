// v10-z — CLI registration + invocation tests for hesap.fft.*. Pulls the module anchor so the static-init
// registration block survives the static-lib link, then drives the forward FFT (vs the brute-force DFT +
// round-trip) and the sparse recovery (vs planted tones) through the command registry.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/fft/cli_anchor.hpp>
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
const bool kPullFft = (crd::hesap::fft::register_fft_cli_anchor(), true);
constexpr double kTwoPi = 6.283185307179586476925286766559;

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

TEST_CASE("CLI fft: hesap.fft.* commands are registered", "[fft][cli]")
{
    REQUIRE(kPullFft);
    REQUIRE(cli::CommandRegistry::global().find("hesap.fft.forward.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.fft.sparse.f64") != nullptr);
}

TEST_CASE("CLI fft: forward (arbitrary size) matches the brute-force DFT + round-trip", "[fft][cli]")
{
    REQUIRE(kPullFft);
    crd::memory::TlsfAllocator alloc(8U << 20);
    const usize n = 12; // non-power-of-two ⇒ exercises Bluestein through the CLI
    cont::Array<f64> sig(&alloc);
    sig.resize(2 * n);
    for (usize i = 0; i < n; ++i)
    {
        sig[2 * i] = std::cos(0.7 * static_cast<double>(i)) + 0.3 * static_cast<double>(i);
        sig[2 * i + 1] = std::sin(0.3 * static_cast<double>(i));
    }
    cli::CommandArgs args{&alloc};
    args.set_f64_array("data", cont::ConstSpan<f64>(sig.data(), 2 * n));
    args.set_i64("inverse", 0);
    cli::CommandResult store{&alloc};
    const Decoded d = invoke(args, "hesap.fft.forward.f64", store);
    REQUIRE(d.len == 2 * n);
    for (usize k = 0; k < n; ++k)
    {
        double re = 0.0;
        double im = 0.0;
        for (usize j = 0; j < n; ++j)
        {
            const double a = -kTwoPi * static_cast<double>((j * k) % n) / static_cast<double>(n);
            re += sig[2 * j] * std::cos(a) - sig[2 * j + 1] * std::sin(a);
            im += sig[2 * j] * std::sin(a) + sig[2 * j + 1] * std::cos(a);
        }
        CHECK(std::abs(d.v[2 * k] - re) < 1e-9);
        CHECK(std::abs(d.v[2 * k + 1] - im) < 1e-9);
    }
    // round-trip: inverse of the forward restores the input
    cont::Array<f64> fwd(&alloc);
    fwd.resize(2 * n);
    for (usize i = 0; i < 2 * n; ++i)
    {
        fwd[i] = d.v[i];
    }
    cli::CommandArgs args2{&alloc};
    args2.set_f64_array("data", cont::ConstSpan<f64>(fwd.data(), 2 * n));
    args2.set_i64("inverse", 1);
    cli::CommandResult store2{&alloc};
    const Decoded d2 = invoke(args2, "hesap.fft.forward.f64", store2);
    REQUIRE(d2.len == 2 * n);
    for (usize i = 0; i < 2 * n; ++i)
    {
        CHECK(std::abs(d2.v[i] - sig[i]) < 1e-9);
    }
}

TEST_CASE("CLI fft: sparse recovery returns the planted tones", "[fft][cli]")
{
    REQUIRE(kPullFft);
    crd::memory::TlsfAllocator alloc(64U << 20);
    const usize n = 4096;
    const usize k = 4;
    const usize freqs[] = {17U, 511U, 1234U, 3000U};
    const double cre[] = {1.0, -0.5, 0.7, 0.3};
    const double cim[] = {0.2, 0.9, -0.4, 0.6};
    cont::Array<f64> sig(&alloc);
    sig.resize(2 * n);
    for (usize i = 0; i < n; ++i)
    {
        double xr = 0.0;
        double xi = 0.0;
        for (usize j = 0; j < k; ++j)
        {
            const double a = kTwoPi * static_cast<double>((freqs[j] * i) % n) / static_cast<double>(n);
            xr += cre[j] * std::cos(a) - cim[j] * std::sin(a);
            xi += cre[j] * std::sin(a) + cim[j] * std::cos(a);
        }
        sig[2 * i] = xr / static_cast<double>(n);
        sig[2 * i + 1] = xi / static_cast<double>(n);
    }
    cli::CommandArgs args{&alloc};
    args.set_f64_array("data", cont::ConstSpan<f64>(sig.data(), 2 * n));
    args.set_i64("k", static_cast<crd::i64>(k));
    cli::CommandResult store{&alloc};
    const Decoded d = invoke(args, "hesap.fft.sparse.f64", store);
    REQUIRE(d.len >= 1);
    const usize got = static_cast<usize>(d.v[0]);
    CHECK(got == k);
    for (usize j = 0; j < k; ++j)
    {
        bool found = false;
        for (usize m = 0; m < got; ++m)
        {
            if (static_cast<usize>(d.v[1 + 3 * m]) == freqs[j])
            {
                found = true;
                CHECK(std::abs(d.v[1 + 3 * m + 1] - cre[j]) < 1e-3);
                CHECK(std::abs(d.v[1 + 3 * m + 2] - cim[j]) < 1e-3);
                break;
            }
        }
        CHECK(found);
    }
}
