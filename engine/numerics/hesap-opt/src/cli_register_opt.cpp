// v7-z — CLI registration for the DATA-DEFINED optimization families (hesap.opt.*). The nonlinear members
// (L-BFGS/Newton/SQP/CMA-ES/...) need callable objectives and stay API-level; agents reach the array-shaped
// problems — QP, LP, MIP, conic — through the command layer:
//
//   hesap.opt.qp.f64    : min ½xᵀPx + qᵀx s.t. l ≤ Ax ≤ u (the OSQP form; eq rows via l == u).
//                         method 0 = ADMM (default) · 1 = Mehrotra IPM · 2 = Goldfarb-Idnani.
//                         Out blob [status, obj, x(n)..., y(m)...] (OSQP-sign duals).
//   hesap.opt.lp.f64    : min cᵀx s.t. l ≤ Ax ≤ u (+ optional variable bounds xlo/xup).
//                         method 0 = revised simplex (default) · 1 = Mehrotra-at-P=0.
//                         Out blob [status, obj, x(n)..., y(m)...].
//   hesap.opt.mip.f64   : the LP form + an integer mask — branch and bound over the simplex.
//                         Out blob [status, obj, nodes, x(n)...].
//   hesap.opt.conic.f64 : min cᵀx s.t. Ax + s = b, s ∈ K (the SCS form; cones Zero/Nonneg/Soc/Psd as
//                         (type, dim) pairs). Out blob [status, obj, x(n)..., y(m)..., s(m)...].
//
// status = the QpStatus value (0 Solved · 1 MaxIterations · 2 PrimalInfeasible · 3 DualInfeasible ·
// 4 NumericalError). Anchor: register_opt_cli_anchor(). ADR-0090.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/opt/conic.hpp>
#include <crd/hesap/opt/lp.hpp>
#include <crd/hesap/opt/mip.hpp>
#include <crd/hesap/opt/qp.hpp>
#include <crd/hesap/opt/qp_active_set.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace opt = crd::hesap::opt;

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

CommandResult blob_f64_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> values)
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

// The shared row-form params: a (m×n row-major), l, u (m), n, m.
void add_row_params(CommandSchema& s, crd::memory::IAllocator* alloc)
{
    add_param(s, alloc, "n", "Number of variables", ParamKind::U64, true);
    add_param(s, alloc, "m", "Number of rows", ParamKind::U64, true);
    add_param(s, alloc, "a", "Row matrix A, m*n row-major (F64Array)", ParamKind::F64, true);
    add_param(s, alloc, "l", "Row lower bounds (F64Array, m; -inf allowed)", ParamKind::F64, true);
    add_param(s, alloc, "u", "Row upper bounds (F64Array, m; +inf allowed)", ParamKind::F64, true);
}

// ----------------------------------------------------------------------------------------------------- QP
CommandResult impl_qp(const CommandArgs& args)
{
    const auto n64 = args.get_u64("n");
    const auto m64 = args.get_u64("m");
    if (!n64 || !m64)
    {
        return error_result(args.alloc, "qp: n and m are required");
    }
    const crd::usize n = static_cast<crd::usize>(*n64);
    const crd::usize m = static_cast<crd::usize>(*m64);
    const auto p = args.get_f64_array("p");
    const auto q = args.get_f64_array("q");
    const auto a = args.get_f64_array("a");
    const auto l = args.get_f64_array("l");
    const auto u = args.get_f64_array("u");
    if (p.size() != n * n || q.size() != n || a.size() != m * n || l.size() != m || u.size() != m)
    {
        return error_result(args.alloc, "qp: p must be n*n, q n, a m*n, l/u m");
    }
    const opt::QpProblem<crd::f64> prob{p, q, a, l, u, n, m};
    const crd::u64 method = args.get_u64("method").value_or(0);
    opt::QpResult<crd::f64> r{args.alloc};
    if (method == 1)
    {
        r = opt::solve_qp_mehrotra<crd::f64>(prob, {}, args.alloc);
    }
    else if (method == 2)
    {
        r = opt::solve_qp_goldfarb_idnani<crd::f64>(prob, args.alloc);
    }
    else
    {
        r = opt::solve_qp_admm<crd::f64>(prob, {}, args.alloc);
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 + n + m);
    out.push_back(static_cast<crd::f64>(static_cast<int>(r.status)));
    out.push_back(r.obj);
    for (crd::usize i = 0; i < n; ++i)
    {
        out.push_back(r.x[i]);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        out.push_back(r.y[i]);
    }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

// ----------------------------------------------------------------------------------------------------- LP
CommandResult impl_lp(const CommandArgs& args)
{
    const auto n64 = args.get_u64("n");
    const auto m64 = args.get_u64("m");
    if (!n64 || !m64)
    {
        return error_result(args.alloc, "lp: n and m are required");
    }
    const crd::usize n = static_cast<crd::usize>(*n64);
    const crd::usize m = static_cast<crd::usize>(*m64);
    const auto c = args.get_f64_array("c");
    const auto a = args.get_f64_array("a");
    const auto l = args.get_f64_array("l");
    const auto u = args.get_f64_array("u");
    const auto xlo = args.get_f64_array("xlo");
    const auto xup = args.get_f64_array("xup");
    if (c.size() != n || a.size() != m * n || l.size() != m || u.size() != m || (xlo.size() != 0 && xlo.size() != n) ||
        (xup.size() != 0 && xup.size() != n))
    {
        return error_result(args.alloc, "lp: c must be n, a m*n, l/u m, xlo/xup n or absent");
    }
    const opt::LpProblem<crd::f64> prob{c, a, l, u, xlo, xup, n, m};
    const crd::u64 method = args.get_u64("method").value_or(0);
    const opt::LpResult<crd::f64> r = method == 1 ? opt::solve_lp_mehrotra<crd::f64>(prob, args.alloc)
                                                  : opt::solve_lp_simplex<crd::f64>(prob, args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 + n + m);
    out.push_back(static_cast<crd::f64>(static_cast<int>(r.status)));
    out.push_back(r.obj);
    for (crd::usize i = 0; i < n; ++i)
    {
        out.push_back(r.x[i]);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        out.push_back(r.y[i]);
    }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

// ---------------------------------------------------------------------------------------------------- MIP
CommandResult impl_mip(const CommandArgs& args)
{
    const auto n64 = args.get_u64("n");
    const auto m64 = args.get_u64("m");
    if (!n64 || !m64)
    {
        return error_result(args.alloc, "mip: n and m are required");
    }
    const crd::usize n = static_cast<crd::usize>(*n64);
    const crd::usize m = static_cast<crd::usize>(*m64);
    const auto c = args.get_f64_array("c");
    const auto a = args.get_f64_array("a");
    const auto l = args.get_f64_array("l");
    const auto u = args.get_f64_array("u");
    const auto xlo = args.get_f64_array("xlo");
    const auto xup = args.get_f64_array("xup");
    const auto integer = args.get_i64_array("integer");
    if (c.size() != n || a.size() != m * n || l.size() != m || u.size() != m || integer.size() != n ||
        (xlo.size() != 0 && xlo.size() != n) || (xup.size() != 0 && xup.size() != n))
    {
        return error_result(args.alloc, "mip: c/integer must be n, a m*n, l/u m, xlo/xup n or absent");
    }
    const opt::LpProblem<crd::f64> prob{c, a, l, u, xlo, xup, n, m};
    crd::containers::Array<bool> mask(args.alloc);
    mask.resize(n);
    for (crd::usize j = 0; j < n; ++j)
    {
        mask[j] = integer[j] != 0;
    }
    const opt::MipResult<crd::f64> r = opt::solve_mip_branch_and_bound<crd::f64>(prob, {mask.data(), n}, args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(3 + n);
    out.push_back(static_cast<crd::f64>(static_cast<int>(r.status)));
    out.push_back(r.obj);
    out.push_back(static_cast<crd::f64>(r.nodes));
    for (crd::usize i = 0; i < n; ++i)
    {
        out.push_back(r.x[i]);
    }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

// -------------------------------------------------------------------------------------------------- conic
CommandResult impl_conic(const CommandArgs& args)
{
    const auto n64 = args.get_u64("n");
    const auto m64 = args.get_u64("m");
    if (!n64 || !m64)
    {
        return error_result(args.alloc, "conic: n and m are required");
    }
    const crd::usize n = static_cast<crd::usize>(*n64);
    const crd::usize m = static_cast<crd::usize>(*m64);
    const auto c = args.get_f64_array("c");
    const auto a = args.get_f64_array("a");
    const auto b = args.get_f64_array("b");
    const auto types = args.get_i64_array("cone_types");
    const auto dims = args.get_i64_array("cone_dims");
    if (c.size() != n || a.size() != m * n || b.size() != m || types.size() != dims.size() || types.size() == 0)
    {
        return error_result(args.alloc, "conic: c must be n, a m*n, b m, cone_types/dims same nonzero length");
    }
    crd::containers::Array<opt::ConeDesc> cones(args.alloc);
    cones.resize(types.size());
    for (crd::usize k = 0; k < types.size(); ++k)
    {
        if (types[k] < 0 || types[k] > 3 || dims[k] < 1)
        {
            return error_result(args.alloc, "conic: cone type must be 0..3 (Zero/Nonneg/Soc/Psd), dim >= 1");
        }
        cones[k].type = static_cast<opt::ConeType>(types[k]);
        cones[k].dim = static_cast<crd::usize>(dims[k]);
    }
    const opt::ConicProblem<crd::f64> prob{c, a, b, {cones.data(), cones.size()}, n, m};
    if (!prob.valid())
    {
        return error_result(args.alloc, "conic: the cone dims must tile exactly m rows (Psd uses dim^2 rows)");
    }
    const opt::ConicResult<crd::f64> r = opt::solve_conic_admm<crd::f64>(prob, args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 + n + 2 * m);
    out.push_back(static_cast<crd::f64>(static_cast<int>(r.status)));
    out.push_back(r.obj);
    for (crd::usize i = 0; i < n; ++i)
    {
        out.push_back(r.x[i]);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        out.push_back(r.y[i]);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        out.push_back(r.s[i]);
    }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

// ------------------------------------------------------------------------------------------------ schemas
CommandSchema make_qp_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.opt.qp.f64", alloc};
    s.description = crd::containers::String{
        "Convex QP: min 0.5 x'Px + q'x s.t. l <= Ax <= u (the OSQP form; equality rows via l == u). "
        "method 0 = ADMM (default), 1 = Mehrotra IPM, 2 = Goldfarb-Idnani dual active-set. "
        "Returns [status, obj, x(n)..., y(m)...] with OSQP-sign duals.",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_row_params(s, alloc);
    add_param(s, alloc, "p", "Hessian P, n*n row-major PSD (F64Array)", ParamKind::F64, true);
    add_param(s, alloc, "q", "Linear term q (F64Array, n)", ParamKind::F64, true);
    add_param(s, alloc, "method", "0 = ADMM (default), 1 = Mehrotra, 2 = Goldfarb-Idnani", ParamKind::U64, false);
    return s;
}

CommandSchema make_lp_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.opt.lp.f64", alloc};
    s.description = crd::containers::String{
        "LP: min c'x s.t. l <= Ax <= u, optional variable bounds xlo <= x <= xup. "
        "method 0 = revised simplex (default), 1 = Mehrotra IPM. Returns [status, obj, x(n)..., y(m)...].",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_row_params(s, alloc);
    add_param(s, alloc, "c", "Objective c (F64Array, n)", ParamKind::F64, true);
    add_param(s, alloc, "xlo", "Variable lower bounds (F64Array, n; optional)", ParamKind::F64, false);
    add_param(s, alloc, "xup", "Variable upper bounds (F64Array, n; optional)", ParamKind::F64, false);
    add_param(s, alloc, "method", "0 = simplex (default), 1 = Mehrotra", ParamKind::U64, false);
    return s;
}

CommandSchema make_mip_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.opt.mip.f64", alloc};
    s.description = crd::containers::String{
        "Mixed-integer LP: the LP form + an integer mask (I64Array of 0/1 per variable); branch and bound "
        "over the revised simplex (proven optimum when status = 0). Returns [status, obj, nodes, x(n)...].",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_row_params(s, alloc);
    add_param(s, alloc, "c", "Objective c (F64Array, n)", ParamKind::F64, true);
    add_param(s, alloc, "integer", "Integer mask (I64Array, n; nonzero = integral)", ParamKind::I64, true);
    add_param(s, alloc, "xlo", "Variable lower bounds (F64Array, n; optional)", ParamKind::F64, false);
    add_param(s, alloc, "xup", "Variable upper bounds (F64Array, n; optional)", ParamKind::F64, false);
    return s;
}

CommandSchema make_conic_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.opt.conic.f64", alloc};
    s.description = crd::containers::String{
        "Conic program (the SCS form): min c'x s.t. Ax + s = b, s in K; cones as (type, dim) pairs with "
        "type 0 = Zero, 1 = Nonneg, 2 = Soc, 3 = Psd (Psd consumes dim^2 rows, full-matrix vectorization). "
        "Returns [status, obj, x(n)..., y(m)..., s(m)...].",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "n", "Number of variables", ParamKind::U64, true);
    add_param(s, alloc, "m", "Number of rows", ParamKind::U64, true);
    add_param(s, alloc, "c", "Objective c (F64Array, n)", ParamKind::F64, true);
    add_param(s, alloc, "a", "Row matrix A, m*n row-major (F64Array)", ParamKind::F64, true);
    add_param(s, alloc, "b", "Right-hand side b (F64Array, m)", ParamKind::F64, true);
    add_param(s, alloc, "cone_types", "Cone types per block (I64Array; 0..3)", ParamKind::I64, true);
    add_param(s, alloc, "cone_dims", "Cone dims per block (I64Array)", ParamKind::I64, true);
    return s;
}
} // namespace

namespace crd::hesap::opt
{
void register_opt_cli_anchor() noexcept {}
} // namespace crd::hesap::opt

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_qp_schema(alloc), &impl_qp);
        reg.register_command(make_lp_schema(alloc), &impl_lp);
        reg.register_command(make_mip_schema(alloc), &impl_mip);
        reg.register_command(make_conic_schema(alloc), &impl_conic);
    });
