// v11-z — CLI registration + invocation tests for hesap.dsp.*.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/dsp/cli_anchor.hpp>
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
const bool kPull = (crd::hesap::dsp::register_dsp_cli_anchor(), true);
const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }
} // namespace

TEST_CASE("CLI dsp: commands registered + welch/resample run", "[v11-z][dsp][cli]")
{
    REQUIRE(kPull);
    REQUIRE(cli::CommandRegistry::global().find("hesap.dsp.welch.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.dsp.resample.f64") != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20);
    const usize n = 4096;
    const usize nperseg = 256;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(2.0 * 3.14159265358979 * 0.1 * static_cast<f64>(i));
    }
    cli::CommandArgs args{&alloc};
    args.set_f64_array("data", cont::ConstSpan<f64>(x.data(), n));
    args.set_i64("nperseg", static_cast<crd::i64>(nperseg));
    const auto* rec = cli::CommandRegistry::global().find("hesap.dsp.welch.f64");
    cli::CommandResult r = rec->impl(args);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    CHECK(blob->bytes.size() / sizeof(f64) == nperseg / 2 + 1); // one-sided PSD length

    cli::CommandArgs ra{&alloc};
    ra.set_f64_array("data", cont::ConstSpan<f64>(x.data(), n));
    ra.set_i64("up", 3);
    ra.set_i64("down", 2);
    const auto* rrec = cli::CommandRegistry::global().find("hesap.dsp.resample.f64");
    cli::CommandResult rr = rrec->impl(ra);
    const auto* rblob = as_blob(rr);
    REQUIRE(rblob != nullptr);
    CHECK(rblob->bytes.size() / sizeof(f64) > n); // up=3/down=2 ⇒ ~1.5x longer
}
