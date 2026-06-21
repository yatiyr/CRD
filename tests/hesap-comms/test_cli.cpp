// v11-z — CLI registration + invocation tests for hesap.comms.*. Round-trips QAM modulate → demodulate.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/comms/cli_anchor.hpp>
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
const bool kPull = (crd::hesap::comms::register_comms_cli_anchor(), true);
const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }
} // namespace

TEST_CASE("CLI comms: qam modulate -> demodulate round trip", "[v11-z][comms][cli]")
{
    REQUIRE(kPull);
    REQUIRE(cli::CommandRegistry::global().find("hesap.comms.qam.modulate") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.comms.qam.demodulate") != nullptr);

    crd::memory::TlsfAllocator alloc(4U << 20);
    const usize n = 64;
    cont::Array<f64> syms(&alloc);
    syms.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        syms[i] = static_cast<f64>((i * 7 + 3) % 16); // QAM16 symbol values
    }
    cli::CommandArgs ma{&alloc};
    ma.set_f64_array("syms", cont::ConstSpan<f64>(syms.data(), n));
    ma.set_i64("order", 16);
    cli::CommandResult mr = cli::CommandRegistry::global().find("hesap.comms.qam.modulate")->impl(ma);
    const auto* mblob = as_blob(mr);
    REQUIRE(mblob != nullptr);
    REQUIRE(mblob->bytes.size() / sizeof(f64) == 2 * n);

    cont::Array<f64> iq(&alloc);
    iq.resize(2 * n);
    const f64* mv = reinterpret_cast<const f64*>(mblob->bytes.data());
    for (usize i = 0; i < 2 * n; ++i)
    {
        iq[i] = mv[i];
    }
    cli::CommandArgs da{&alloc};
    da.set_f64_array("data", cont::ConstSpan<f64>(iq.data(), 2 * n));
    da.set_i64("order", 16);
    cli::CommandResult dr = cli::CommandRegistry::global().find("hesap.comms.qam.demodulate")->impl(da);
    const auto* dblob = as_blob(dr);
    REQUIRE(dblob != nullptr);
    REQUIRE(dblob->bytes.size() / sizeof(f64) == n);
    const f64* dv = reinterpret_cast<const f64*>(dblob->bytes.data());
    for (usize i = 0; i < n; ++i)
    {
        CHECK(std::lround(dv[i]) == std::lround(syms[i])); // noise-free round trip
    }
}
