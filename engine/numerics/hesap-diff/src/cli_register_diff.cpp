// v13-z — CLI registration for the numerical-differentiation cluster (hesap.diff.*). The sample-driven differentiators
// cross the CLI boundary as DATA (a function integrand cannot); the callable complex-step / Ridders drivers stay
// in-process. Two flagship data surfaces:
//
//   hesap.diff.savgol.f64 : Savitzky-Golay smoothed derivative of noisy samples (the IMU/encoder-rate use case).
//     y : samples.  window : odd window length.  polyorder : fit order (< window).
//     deriv : derivative order (default 0 = smoothing).  delta : sample spacing (default 1).
//     Out blob = the filtered/derivative signal, length = y.
//
//   hesap.diff.fornberg.f64 : Fornberg arbitrary-stencil FD weights (any node distribution, any derivative order).
//     nodes : the stencil abscissae.  z : the evaluation point.  max_deriv : highest derivative order.
//     Out blob = the weight table, (max_deriv+1) rows of nnodes: c[k*nnodes + i] = weight of node i for the k-th deriv.
// Anchor: register_diff_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/diff/diff.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace diff = crd::hesap::diff;
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

CommandSchema make_savgol_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.diff.savgol.f64", alloc};
    s.description = crd::containers::String{
        "Savitzky-Golay smoothed derivative of samples (noise-robust). Out = filtered signal, length = y.", alloc};
    add_param(s, alloc, "y", "sampled signal, length n", ParamKind::VectorId, true);
    add_param(s, alloc, "window", "odd window length (3..1023)", ParamKind::I64, true);
    add_param(s, alloc, "polyorder", "polynomial fit order (< window)", ParamKind::I64, true);
    add_param(s, alloc, "deriv", "derivative order (default 0 = smoothing)", ParamKind::I64, false);
    add_param(s, alloc, "delta", "sample spacing (default 1)", ParamKind::F64, false);
    return s;
}

CommandResult impl_savgol(const CommandArgs& args)
{
    const auto y = args.get_f64_array("y");
    const auto wo = args.get_i64("window");
    const auto po = args.get_i64("polyorder");
    if (y.empty() || !wo || !po)
    {
        return error_result(args.alloc, "diff.savgol: need y, window, and polyorder");
    }
    const crd::i64 window = *wo;
    const crd::i64 poly = *po;
    const crd::i64 deriv = args.get_i64("deriv").value_or(0);
    const crd::f64 delta = args.get_f64("delta").value_or(1.0);
    if (window < 1 || window > 1024 || poly < 0 || poly >= window || deriv < 0 || deriv > poly)
    {
        return error_result(args.alloc, "diff.savgol: need 1<=window<=1024, 0<=polyorder<window, 0<=deriv<=polyorder");
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.resize(y.size());
    if (!diff::savgol_filter<crd::f64>(y, static_cast<int>(window), static_cast<int>(poly), static_cast<int>(deriv),
                                       delta, out.data()))
    {
        return error_result(args.alloc, "diff.savgol: coefficient solve failed (check window/polyorder)");
    }
    return blob_f64_result(args.alloc, ConstSpan<crd::f64>(out.data(), out.size()));
}

CommandSchema make_fornberg_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.diff.fornberg.f64", alloc};
    s.description = crd::containers::String{
        "Fornberg finite-difference weights for any node set. Out = (max_deriv+1) rows of nnodes weights.", alloc};
    add_param(s, alloc, "nodes", "stencil abscissae, length nnodes >= 1", ParamKind::VectorId, true);
    add_param(s, alloc, "z", "evaluation point", ParamKind::F64, true);
    add_param(s, alloc, "max_deriv", "highest derivative order (>= 0)", ParamKind::I64, true);
    return s;
}

CommandResult impl_fornberg(const CommandArgs& args)
{
    const auto nodes = args.get_f64_array("nodes");
    const auto zo = args.get_f64("z");
    const auto mdo = args.get_i64("max_deriv");
    if (nodes.empty() || !zo || !mdo || *mdo < 0)
    {
        return error_result(args.alloc, "diff.fornberg: need nodes (non-empty), z, and max_deriv >= 0");
    }
    const int max_deriv = static_cast<int>(*mdo);
    crd::containers::Array<crd::f64> c(args.alloc);
    c.resize(static_cast<crd::usize>(max_deriv + 1) * nodes.size());
    diff::fornberg_weights<crd::f64>(*zo, nodes, max_deriv, c.data());
    return blob_f64_result(args.alloc, ConstSpan<crd::f64>(c.data(), c.size()));
}
} // namespace

namespace crd::hesap::diff
{
void register_diff_cli_anchor() noexcept {}
} // namespace crd::hesap::diff

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_savgol_schema(alloc), &impl_savgol);
        reg.register_command(make_fornberg_schema(alloc), &impl_fornberg);
    });
