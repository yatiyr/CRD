// v11-z — CLI registration for the wavelet cluster (hesap.wavelet.*).
//
//   hesap.wavelet.dwt.f64     : single-level DWT. data = real signal, wavelet = name ("db4"), mode = symmetric.
//     Out blob = [len, cA[0..len), cD[0..len)] (len = the per-band coefficient count).
//
//   hesap.wavelet.denoise.f64 : VisuShrink soft-threshold denoising. data, wavelet, level. Out = denoised signal.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/wavelet/denoise.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace wv = crd::hesap::wavelet;

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

CommandSchema dwt_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.wavelet.dwt.f64", a};
    s.description = crd::containers::String{"Single-level DWT (symmetric). Out = [len, cA..., cD...].", a};
    add_param(s, a, "data", "real signal (f64 vector)", ParamKind::VectorId, true);
    add_param(s, a, "wavelet", "wavelet name, e.g. db4, sym8, coif2 (default db4)", ParamKind::String, false);
    return s;
}

CommandResult impl_dwt(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    crd::containers::StringView wname = args.get_string("wavelet");
    if (wname.empty())
    {
        wname = "db4";
    }
    const auto w = wv::wavelet_by_name(wname);
    if (data.size() == 0 || !w)
    {
        return error_result(args.alloc, "wavelet.dwt: data + a known wavelet name required");
    }
    crd::containers::Array<crd::f64> c_a(args.alloc);
    crd::containers::Array<crd::f64> c_d(args.alloc);
    wv::dwt<crd::f64>(args.alloc, data, *w, wv::SignalExtensionMode::Symmetric, c_a, c_d);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(static_cast<crd::f64>(c_a.size()));
    for (crd::usize i = 0; i < c_a.size(); ++i)
    {
        out.push_back(c_a[i]);
    }
    for (crd::usize i = 0; i < c_d.size(); ++i)
    {
        out.push_back(c_d[i]);
    }
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), out.size()));
}

CommandSchema denoise_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.wavelet.denoise.f64", a};
    s.description = crd::containers::String{"VisuShrink soft-threshold wavelet denoising. Out = denoised signal.", a};
    add_param(s, a, "data", "real noisy signal (f64 vector)", ParamKind::VectorId, true);
    add_param(s, a, "wavelet", "wavelet name (default db4)", ParamKind::String, false);
    add_param(s, a, "level", "decomposition level (default 4)", ParamKind::I64, false);
    return s;
}

CommandResult impl_denoise(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    crd::containers::StringView wname = args.get_string("wavelet");
    if (wname.empty())
    {
        wname = "db4";
    }
    const auto w = wv::wavelet_by_name(wname);
    if (data.size() == 0 || !w)
    {
        return error_result(args.alloc, "wavelet.denoise: data + a known wavelet name required");
    }
    const crd::usize level = static_cast<crd::usize>(args.get_i64("level").value_or(4));
    const auto y = wv::denoise<crd::f64>(args.alloc, data, *w, level, wv::ThresholdMode::Soft,
                                         wv::DenoiseRule::VisuShrink);
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(y.data(), y.size()));
}
} // namespace

namespace crd::hesap::wavelet
{
void register_wavelet_cli_anchor() noexcept {}
} // namespace crd::hesap::wavelet

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* a = crd::memory::default_allocator();
        reg.register_command(dwt_schema(a), &impl_dwt);
        reg.register_command(denoise_schema(a), &impl_denoise);
    });
