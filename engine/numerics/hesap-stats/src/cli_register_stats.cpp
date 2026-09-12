// v12-z — CLI registration for the statistics cluster (hesap.stats.*). Samples are DATA (real f64 vectors), so the
// agent reaches the operations directly via an f64-vector argument (the v7-z/v10-z/v11-z data-vs-callable split).
//
//   hesap.stats.describe.f64 : mean / variance(ddof=1) / skewness / kurtosis of a sample.
//     data : the sample (f64 vector).   Out blob = [mean, variance, skewness, kurtosis] (4 f64).
//
//   hesap.stats.ttest_1samp.f64 : one-sample t-test, H0 mean(data) == popmean (scipy.stats.ttest_1samp).
//     data : the sample.   popmean : the null mean (f64).   Out blob = [statistic, pvalue, df] (3 f64).

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/stats/descriptive.hpp>
#include <crd/hesap/stats/hypothesis.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace stats = crd::hesap::stats;

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value = std::move(e);
    return r;
}

CommandResult blob_f64(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> v)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto* raw = reinterpret_cast<const crd::u8*>(v.data());
    const crd::usize nb = v.size() * sizeof(crd::f64);
    blob.bytes.reserve(nb);
    for (crd::usize i = 0; i < nb; ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
    return r;
}

void add_param(CommandSchema& s, crd::memory::IAllocator* a, const char* name, const char* desc, ParamKind k, bool req)
{
    ParamSchema p{a};
    p.name = crd::containers::String{name, a};
    p.description = crd::containers::String{desc, a};
    p.kind = k;
    p.required = req;
    s.params.push_back(std::move(p));
}

CommandSchema describe_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.stats.describe.f64", a};
    s.description = crd::containers::String{"mean/variance(ddof=1)/skewness/kurtosis. Out = [mean,var,skew,kurt].", a};
    add_param(s, a, "data", "sample (f64 vector)", ParamKind::VectorId, true);
    return s;
}

CommandResult impl_describe(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    if (data.size() < 2)
    {
        return error_result(args.alloc, "stats.describe: need >= 2 samples");
    }
    const crd::f64 out[4] = {stats::mean<crd::f64>(data), stats::variance<crd::f64>(data, 1),
                             stats::skewness<crd::f64>(data), stats::kurtosis<crd::f64>(data)};
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out, 4));
}

CommandSchema ttest_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.stats.ttest_1samp.f64", a};
    s.description = crd::containers::String{"One-sample t-test vs popmean. Out = [statistic, pvalue, df].", a};
    add_param(s, a, "data", "sample (f64 vector)", ParamKind::VectorId, true);
    add_param(s, a, "popmean", "null hypothesis mean", ParamKind::F64, true);
    return s;
}

CommandResult impl_ttest(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    const auto popmean = args.get_f64("popmean");
    if (data.size() < 2 || !popmean)
    {
        return error_result(args.alloc, "stats.ttest_1samp: need >= 2 samples + popmean");
    }
    const auto t = stats::t_test_1samp<crd::f64>(data, *popmean);
    const crd::f64 out[3] = {t.statistic, t.pvalue, t.df};
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out, 3));
}
} // namespace

namespace crd::hesap::stats
{
void register_stats_cli_anchor() noexcept {}
} // namespace crd::hesap::stats

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* a = crd::memory::default_allocator();
        reg.register_command(describe_schema(a), &impl_describe);
        reg.register_command(ttest_schema(a), &impl_ttest);
    });
