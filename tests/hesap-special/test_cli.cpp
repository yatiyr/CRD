// v12-z — CLI registration + invocation tests for hesap.special.*.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/special/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;
namespace cont = crd::containers;
using crd::f64;

namespace
{
const bool kPull = (crd::hesap::special::register_special_cli_anchor(), true);
const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }
} // namespace

TEST_CASE("CLI special: gamma + erf registered + run", "[v12-z][special][cli]")
{
    REQUIRE(kPull);
    REQUIRE(cli::CommandRegistry::global().find("hesap.special.gamma.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.special.erf.f64") != nullptr);

    crd::memory::TlsfAllocator alloc(1U << 20);
    f64 d[] = {1, 2, 3, 4, 5}; // Γ: 1, 1, 2, 6, 24
    cli::CommandArgs args{&alloc};
    args.set_f64_array("data", cont::ConstSpan<f64>(d, 5));
    const auto* rec = cli::CommandRegistry::global().find("hesap.special.gamma.f64");
    cli::CommandResult r = rec->impl(args);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() / sizeof(f64) == 5);
    const auto* out = reinterpret_cast<const f64*>(blob->bytes.data());
    CHECK(std::fabs(out[3] - 6.0) < 1e-9);  // Γ(4) = 3! = 6
    CHECK(std::fabs(out[4] - 24.0) < 1e-9); // Γ(5) = 4! = 24

    f64 e[] = {0.0, 1.0};
    cli::CommandArgs ea{&alloc};
    ea.set_f64_array("data", cont::ConstSpan<f64>(e, 2));
    const auto* erec = cli::CommandRegistry::global().find("hesap.special.erf.f64");
    cli::CommandResult er = erec->impl(ea);
    const auto* eblob = as_blob(er);
    REQUIRE(eblob != nullptr);
    const auto* eout = reinterpret_cast<const f64*>(eblob->bytes.data());
    CHECK(std::fabs(eout[0]) < 1e-12);                     // erf(0) = 0
    CHECK(std::fabs(eout[1] - 0.8427007929497149) < 1e-9); // erf(1)
}
