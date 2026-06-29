// v12-z — CLI registration for the special-functions cluster (hesap.special.*). Inputs are DATA (real f64 vectors of
// abscissae), evaluated element-wise (the v7-z/v10-z/v11-z data-vs-callable split).
//
//   hesap.special.gamma.f64 : the gamma function Γ(x) element-wise. data : f64 vector. Out blob = [Γ(x_i)].
//   hesap.special.erf.f64   : the error function erf(x) element-wise. data : f64 vector. Out blob = [erf(x_i)].

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/special/erf.hpp>
#include <crd/hesap/special/gamma.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace special = crd::hesap::special;

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

CommandSchema gamma_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.special.gamma.f64", a};
    s.description = crd::containers::String{"Gamma function Γ(x) element-wise. Out = [Γ(x_i)].", a};
    add_param(s, a, "data", "abscissae (f64 vector)", ParamKind::VectorId, true);
    return s;
}

CommandResult impl_gamma(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    if (data.size() == 0)
    {
        return error_result(args.alloc, "special.gamma: empty data");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(data.size());
    for (crd::usize i = 0; i < data.size(); ++i)
    {
        out[i] = special::gamma<crd::f64>(data[i]);
    }
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), out.size()));
}

CommandSchema erf_schema(crd::memory::IAllocator* a)
{
    CommandSchema s{a};
    s.name = crd::containers::String{"hesap.special.erf.f64", a};
    s.description = crd::containers::String{"Error function erf(x) element-wise. Out = [erf(x_i)].", a};
    add_param(s, a, "data", "abscissae (f64 vector)", ParamKind::VectorId, true);
    return s;
}

CommandResult impl_erf(const CommandArgs& args)
{
    const auto data = args.get_f64_array("data");
    if (data.size() == 0)
    {
        return error_result(args.alloc, "special.erf: empty data");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(data.size());
    for (crd::usize i = 0; i < data.size(); ++i)
    {
        out[i] = special::erf<crd::f64>(data[i]);
    }
    return blob_f64(args.alloc, crd::containers::ConstSpan<crd::f64>(out.data(), out.size()));
}
} // namespace

namespace crd::hesap::special
{
void register_special_cli_anchor() noexcept {}
} // namespace crd::hesap::special

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* a = crd::memory::default_allocator();
        reg.register_command(gamma_schema(a), &impl_gamma);
        reg.register_command(erf_schema(a), &impl_erf);
    });
