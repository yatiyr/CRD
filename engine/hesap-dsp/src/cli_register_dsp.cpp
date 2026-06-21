// v11-z — CLI registration for the DSP cluster (hesap.dsp.*). Signals are DATA (real f64 vectors), so the agent
// reaches the operations directly via an f64-vector argument (the v7-z/v10-z data-vs-callable split).
//
//   hesap.dsp.welch.f64    : Welch power spectral density of a real signal.
//     data    : the real signal (f64 vector).   nperseg : segment length (default 256).
//     Out blob = the one-sided PSD (f64 vector, length nperseg/2 + 1).
//
//   hesap.dsp.resample.f64 : rational resampling (polyphase, scipy resample_poly).
//     data : the real signal.   up / down : the rational factor.   Out blob = the resampled signal.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dsp/multirate.hpp>
#include <crd/hesap/dsp/spectral.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace dsp = crd::hesap::dsp;

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

CommandSchema welch_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.dsp.welch.f64", a};
    s.description = crd::containers::String{"Welch PSD of a real signal. Out = one-sided PSD (length nperseg/2+1).", a};
    add_param(s, a, "data", "real signal (f64 vector)", ParamKind::VectorId, true);
    add_param(s, a, "nperseg", "segment length (default 256)", ParamKind::I64, false);
    return s;
}

CommandResult impl_welch(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    const crd::usize nperseg = static_cast<crd::usize>(args.get_i64("nperseg").value_or(256));
    if (data.size() < nperseg || nperseg < 2)
    {
        return error_result(args.alloc, "dsp.welch: need data.size() >= nperseg >= 2");
    }
    const auto psd = dsp::welch_psd<crd::f64>(args.alloc, data, 1.0, nperseg);
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(psd.data(), psd.size()));
}

CommandSchema resample_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.dsp.resample.f64", a};
    s.description = crd::containers::String{"Rational resampling (resample_poly). Out = the resampled signal.", a};
    add_param(s, a, "data", "real signal (f64 vector)", ParamKind::VectorId, true);
    add_param(s, a, "up", "upsample factor", ParamKind::I64, true);
    add_param(s, a, "down", "downsample factor", ParamKind::I64, true);
    return s;
}

CommandResult impl_resample(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    const auto up = args.get_i64("up");
    const auto down = args.get_i64("down");
    if (data.size() == 0 || !up || !down || *up < 1 || *down < 1)
    {
        return error_result(args.alloc, "dsp.resample: data + up>=1 + down>=1 required");
    }
    const auto y = dsp::resample_poly<crd::f64>(args.alloc, data, static_cast<crd::usize>(*up),
                                                static_cast<crd::usize>(*down));
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(y.data(), y.size()));
}
} // namespace

namespace crd::hesap::dsp
{
void register_dsp_cli_anchor() noexcept {}
} // namespace crd::hesap::dsp

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* a = crd::memory::default_allocator();
        reg.register_command(welch_schema(a), &impl_welch);
        reg.register_command(resample_schema(a), &impl_resample);
    });
