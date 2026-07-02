// v13-z — CLI registration for the interpolation cluster (hesap.interp.*). The data (x, y, query points) are DATA
// (f64 vectors), so the agent reaches the interpolants directly via F64Array arguments (the v7-z data-vs-callable
// split; the same shape as hesap.fft.*). Build-once/evaluate-many under the hood; the CLI returns the evaluated
// query values as an interleaved blob.
//
//   hesap.interp.pchip.f64 : monotone (no-overshoot) PCHIP — the certifiable control-LUT default.
//     x, y : the sample points (strictly-increasing x, same length n >= 2).  xq : query points.
//     Out blob = yq (the interpolated value at each query point), length xq.
//
//   hesap.interp.cubic_spline.f64 : C2 cubic spline (all boundary conditions).
//     x, y, xq as above.  bc : natural | clamped | notaknot | periodic (default natural).
//     clamp_left / clamp_right : endpoint slopes for bc=clamped (default 0).
//     Out blob = yq, length xq.
// Anchor: register_interp_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/interp/interp.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace interp = crd::hesap::interp;
using crd::containers::ConstSpan;

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

CommandResult blob_f64_result(crd::memory::IAllocator* alloc, ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto* raw = reinterpret_cast<const crd::u8*>(values.data());
    const crd::usize n_bytes = values.size() * sizeof(crd::f64);
    blob.bytes.reserve(n_bytes);
    for (crd::usize i = 0; i < n_bytes; ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
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

CommandSchema make_pchip_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.interp.pchip.f64", alloc};
    s.description = crd::containers::String{
        "Monotone (no-overshoot) PCHIP interpolation. Build from (x,y), evaluate at xq. Out = yq.", alloc};
    add_param(s, alloc, "x", "sample abscissae, strictly increasing, length n >= 2", ParamKind::VectorId, true);
    add_param(s, alloc, "y", "sample ordinates, length n", ParamKind::VectorId, true);
    add_param(s, alloc, "xq", "query points to evaluate", ParamKind::VectorId, true);
    return s;
}

CommandResult impl_pchip(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    const auto y = args.get_f64_array("y");
    const auto xq = args.get_f64_array("xq");
    if (x.size() < 2 || y.size() != x.size() || xq.empty())
    {
        return error_result(args.alloc, "interp.pchip: need x,y (same length >= 2) and a non-empty xq");
    }
    // The interpolant references the caller's x/y; `args` (and thus the arrays) outlive this call, so no copy needed.
    interp::PchipInterpolant<crd::f64> p(args.alloc);
    if (p.build(x, y) != interp::InterpStatus::Ok)
    {
        return error_result(args.alloc, "interp.pchip: build failed (NaN/Inf or non-increasing x)");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(xq.size());
    for (crd::usize i = 0; i < xq.size(); ++i)
    {
        out[i] = p.eval(xq[i]);
    }
    return blob_f64_result(args.alloc, ConstSpan<crd::f64>(out.data(), out.size()));
}

CommandSchema make_cubic_spline_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.interp.cubic_spline.f64", alloc};
    s.description = crd::containers::String{
        "C2 cubic spline (natural/clamped/not-a-knot/periodic). Build from (x,y), evaluate at xq. Out = yq.", alloc};
    add_param(s, alloc, "x", "sample abscissae, strictly increasing, length n >= 2", ParamKind::VectorId, true);
    add_param(s, alloc, "y", "sample ordinates, length n", ParamKind::VectorId, true);
    add_param(s, alloc, "xq", "query points to evaluate", ParamKind::VectorId, true);
    ParamSchema bc{alloc};
    bc.name = crd::containers::String{"bc", alloc};
    bc.description = crd::containers::String{"boundary condition (default natural)", alloc};
    bc.kind = ParamKind::Enum;
    bc.enum_values = crd::containers::String{"natural|clamped|notaknot|periodic", alloc};
    bc.required = false;
    s.params.push_back(std::move(bc));
    add_param(s, alloc, "clamp_left", "left endpoint slope for bc=clamped (default 0)", ParamKind::F64, false);
    add_param(s, alloc, "clamp_right", "right endpoint slope for bc=clamped (default 0)", ParamKind::F64, false);
    return s;
}

CommandResult impl_cubic_spline(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    const auto y = args.get_f64_array("y");
    const auto xq = args.get_f64_array("xq");
    if (x.size() < 2 || y.size() != x.size() || xq.empty())
    {
        return error_result(args.alloc, "interp.cubic_spline: need x,y (same length >= 2) and a non-empty xq");
    }
    interp::SplineBC bc = interp::SplineBC::Natural;
    const auto bcs = args.get_string("bc");
    if (bcs == "clamped")
    {
        bc = interp::SplineBC::Clamped;
    }
    else if (bcs == "notaknot")
    {
        bc = interp::SplineBC::NotAKnot;
    }
    else if (bcs == "periodic")
    {
        bc = interp::SplineBC::Periodic;
    }
    const crd::f64 cl = args.get_f64("clamp_left").value_or(0.0);
    const crd::f64 cr = args.get_f64("clamp_right").value_or(0.0);
    interp::CubicSplineInterpolant<crd::f64> sp(args.alloc);
    if (sp.build(x, y, bc, cl, cr) != interp::InterpStatus::Ok)
    {
        return error_result(args.alloc, "interp.cubic_spline: build failed (NaN/Inf, non-increasing x, or bad periodic)");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(xq.size());
    for (crd::usize i = 0; i < xq.size(); ++i)
    {
        out[i] = sp.eval(xq[i]);
    }
    return blob_f64_result(args.alloc, ConstSpan<crd::f64>(out.data(), out.size()));
}
} // namespace

namespace crd::hesap::interp
{
void register_interp_cli_anchor() noexcept {}
} // namespace crd::hesap::interp

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_pchip_schema(alloc), &impl_pchip);
        reg.register_command(make_cubic_spline_schema(alloc), &impl_cubic_spline);
    });
