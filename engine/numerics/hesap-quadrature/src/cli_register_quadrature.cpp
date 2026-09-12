// v13-z — CLI registration for the quadrature cluster (hesap.quad.*). The natural agent surface is composite
// integration of SAMPLED data (telemetry / odometry / measured curves) — a function integrand can't cross the CLI
// boundary, so the callable adaptive drivers (QAGS/Gauss-Kronrod/…) stay in-process; the CLI exposes the
// sample-driven rules (the scipy.integrate.{trapezoid,simpson,romb} surface).
//
//   hesap.quad.samples.f64 : integrate uniformly-spaced samples y with step dx.
//     y    : the sampled integrand, length n >= 2.  dx : the (positive) sample spacing.
//     rule : trapezoid | simpson (default) | romberg.  (romberg requires n = 2^k + 1.)
//     Out  : the scalar integral estimate.
// Anchor: register_quadrature_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/quadrature/quadrature.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace quad = crd::hesap::quadrature;

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

CommandResult scalar_result(crd::memory::IAllocator* alloc, crd::f64 v)
{
    CommandResult r{alloc};
    r.ok = true;
    r.value = ResultScalarF64{v};
    return r;
}

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

CommandSchema make_samples_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.quad.samples.f64", alloc};
    s.description = crd::containers::String{
        "Integrate uniformly-spaced samples y (step dx) via trapezoid/simpson/romberg. Out = scalar integral.", alloc};
    add_param(s, alloc, "y", "sampled integrand values, length n >= 2", ParamKind::VectorId, true);
    add_param(s, alloc, "dx", "uniform sample spacing (> 0)", ParamKind::F64, true);
    ParamSchema rule{alloc};
    rule.name = crd::containers::String{"rule", alloc};
    rule.description = crd::containers::String{"trapezoid | simpson (default) | romberg (needs n = 2^k+1)", alloc};
    rule.kind = ParamKind::Enum;
    rule.enum_values = crd::containers::String{"trapezoid|simpson|romberg", alloc};
    rule.required = false;
    s.params.push_back(std::move(rule));
    s.output.kind = OutputKind::Scalar;
    return s;
}

CommandResult impl_samples(const CommandArgs& args)
{
    const auto y = args.get_f64_array("y");
    const auto dxo = args.get_f64("dx");
    if (y.size() < 2 || !dxo || *dxo <= 0.0)
    {
        return error_result(args.alloc, "quad.samples: need y (length >= 2) and dx > 0");
    }
    const auto rule = args.get_string("rule");
    if (rule == "trapezoid")
    {
        return scalar_result(args.alloc, quad::trapezoid<crd::f64>(y, *dxo));
    }
    if (rule == "romberg")
    {
        const crd::usize m = y.size() - 1; // n = 2^k + 1  <=>  (n-1) is a non-zero power of two
        if (m == 0 || (m & (m - 1)) != 0)
        {
            return error_result(args.alloc, "quad.samples: rule=romberg requires n = 2^k + 1 samples");
        }
        return scalar_result(args.alloc, quad::romberg_samples<crd::f64>(y, *dxo));
    }
    return scalar_result(args.alloc, quad::simpson<crd::f64>(y, *dxo)); // default
}
} // namespace

namespace crd::hesap::quadrature
{
void register_quadrature_cli_anchor() noexcept {}
} // namespace crd::hesap::quadrature

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_samples_schema(alloc), &impl_samples);
    });
