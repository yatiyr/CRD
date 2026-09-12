// v11-z — CLI registration for the comms cluster (hesap.comms.*).
//
//   hesap.comms.qam.modulate   : Gray-coded square-QAM modulation. syms = symbol values (f64, rounded), order = M.
//     Out blob = interleaved [re,im,...] constellation points (length 2·N).
//
//   hesap.comms.qam.demodulate : hard demodulation. data = interleaved [re,im,...], order = M.
//     Out blob = the recovered symbol values (f64, length N).

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace cm = crd::hesap::comms;
using Cd = crd::hesap::Complex<crd::f64>;

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

CommandSchema mod_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.comms.qam.modulate", a};
    s.description = crd::containers::String{"Gray square-QAM modulate. Out = interleaved re/im constellation points.", a};
    add_param(s, a, "syms", "symbol values 0..M-1 (f64 vector)", ParamKind::VectorId, true);
    add_param(s, a, "order", "constellation order M (4,16,64,256)", ParamKind::I64, true);
    return s;
}

CommandResult impl_mod(const CommandArgs& args)
{
    const auto syms = args.get_f64_array("syms");
    const auto order = args.get_i64("order");
    if (syms.size() == 0 || !order || *order < 4)
    {
        return error_result(args.alloc, "comms.qam.modulate: syms + order>=4 required");
    }
    cm::Modem<crd::f64> modem(args.alloc, cm::ModFamily::Qam, static_cast<crd::u32>(*order));
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(2 * syms.size());
    for (crd::usize i = 0; i < syms.size(); ++i)
    {
        const Cd c = modem.modulate(static_cast<crd::u32>(std::lround(syms[i])));
        out[2 * i] = c.re;
        out[2 * i + 1] = c.im;
    }
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), out.size()));
}

CommandSchema demod_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.comms.qam.demodulate", a};
    s.description = crd::containers::String{"Hard square-QAM demodulate. data = interleaved re/im. Out = symbols.", a};
    add_param(s, a, "data", "interleaved [re,im,...] received points (f64 vector, length 2N)", ParamKind::VectorId, true);
    add_param(s, a, "order", "constellation order M", ParamKind::I64, true);
    return s;
}

CommandResult impl_demod(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    const auto order = args.get_i64("order");
    if (data.size() < 2 || (data.size() & 1U) != 0 || !order || *order < 4)
    {
        return error_result(args.alloc, "comms.qam.demodulate: data (even len) + order>=4 required");
    }
    cm::Modem<crd::f64> modem(args.alloc, cm::ModFamily::Qam, static_cast<crd::u32>(*order));
    const crd::usize n = data.size() / 2;
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = static_cast<crd::f64>(modem.demodulate(Cd{data[2 * i], data[2 * i + 1]}));
    }
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), n));
}
} // namespace

namespace crd::hesap::comms
{
void register_comms_cli_anchor() noexcept {}
} // namespace crd::hesap::comms

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* a = crd::memory::default_allocator();
        reg.register_command(mod_schema(a), &impl_mod);
        reg.register_command(demod_schema(a), &impl_demod);
    });
