// v11-z — CLI registration + invocation tests for hesap.wavelet.*.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/wavelet/cli_anchor.hpp>
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
const bool kPull = (crd::hesap::wavelet::register_wavelet_cli_anchor(), true);
const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }
} // namespace

TEST_CASE("CLI wavelet: commands registered + dwt/denoise run", "[v11-z][wavelet][cli]")
{
    REQUIRE(kPull);
    REQUIRE(cli::CommandRegistry::global().find("hesap.wavelet.dwt.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.wavelet.denoise.f64") != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20);
    const usize n = 1024;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(0.05 * static_cast<f64>(i));
    }
    cli::CommandArgs args{&alloc};
    args.set_f64_array("data", cont::ConstSpan<f64>(x.data(), n));
    args.set_string("wavelet", "db4");
    cli::CommandResult r = cli::CommandRegistry::global().find("hesap.wavelet.dwt.f64")->impl(args);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    const f64* v = reinterpret_cast<const f64*>(blob->bytes.data());
    const usize nc_a = static_cast<usize>(v[0]);
    CHECK(nc_a > 0);
    CHECK(blob->bytes.size() / sizeof(f64) == 1 + 2 * nc_a); // [len, cA..., cD...]

    cli::CommandArgs da{&alloc};
    da.set_f64_array("data", cont::ConstSpan<f64>(x.data(), n));
    da.set_string("wavelet", "db4");
    da.set_i64("level", 4);
    cli::CommandResult dr = cli::CommandRegistry::global().find("hesap.wavelet.denoise.f64")->impl(da);
    const auto* dblob = as_blob(dr);
    REQUIRE(dblob != nullptr);
    CHECK(dblob->bytes.size() / sizeof(f64) == n); // denoised signal, same length
}
