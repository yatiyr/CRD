// v12-z — CLI registration + invocation tests for hesap.stats.*.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/stats/cli_anchor.hpp>
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
const bool kPull = (crd::hesap::stats::register_stats_cli_anchor(), true);
const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }
} // namespace

TEST_CASE("CLI stats: describe + ttest_1samp registered + run", "[v12-z][stats][cli]")
{
    REQUIRE(kPull);
    REQUIRE(cli::CommandRegistry::global().find("hesap.stats.describe.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.stats.ttest_1samp.f64") != nullptr);

    crd::memory::TlsfAllocator alloc(1U << 20);
    f64 d[] = {1, 2, 3, 4, 5, 6, 7, 8};
    cli::CommandArgs args{&alloc};
    args.set_f64_array("data", cont::ConstSpan<f64>(d, 8));
    const auto* rec = cli::CommandRegistry::global().find("hesap.stats.describe.f64");
    cli::CommandResult r = rec->impl(args);
    const auto* blob = as_blob(r);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() / sizeof(f64) == 4); // [mean, var, skew, kurt]
    const auto* out = reinterpret_cast<const f64*>(blob->bytes.data());
    CHECK(std::fabs(out[0] - 4.5) < 1e-12);          // mean of 1..8
    CHECK(std::fabs(out[1] - 6.0) < 1e-12);          // sample variance (ddof=1)

    cli::CommandArgs ta{&alloc};
    ta.set_f64_array("data", cont::ConstSpan<f64>(d, 8));
    ta.set_f64("popmean", 4.5);
    const auto* trec = cli::CommandRegistry::global().find("hesap.stats.ttest_1samp.f64");
    cli::CommandResult tr = trec->impl(ta);
    const auto* tblob = as_blob(tr);
    REQUIRE(tblob != nullptr);
    REQUIRE(tblob->bytes.size() / sizeof(f64) == 3);
    const auto* tout = reinterpret_cast<const f64*>(tblob->bytes.data());
    CHECK(std::fabs(tout[0]) < 1e-9); // t-statistic ~0 when popmean == sample mean
}
