// v9-z — CLI registration for the ODE solvers (hesap.ode.*). RHS functions are callables, so (the v7-z
// data-vs-callable split) agents reach the integrators through CANNED, named test problems + method and
// tolerance selection — the Bari/Hairer scoreboard corpus:
//
//   hesap.ode.solve.f64 : integrate a canned problem to t1 and return the final state + work counters.
//     problem : 0 = exponential decay (y'=-y, n=1) · 1 = Van der Pol (n=2, param mu) · 2 = Robertson (n=3,
//               stiff) · 3 = harmonic oscillator (n=2).
//     method  : 0 RK45 · 1 RK23 · 2 DOP853 · 3 BDF · 4 Radau · 5 RODAS4 · 6 TR-BDF2.
//     t1, rtol (def 1e-6), atol (def 1e-9), mu (def 1000, Van der Pol).
//     Out blob [status, nfev, njev, nlu, naccept, nreject, y(n)...]. status = the OdeStatus value.
// Anchor: register_ode_cli_anchor(). ADR-0091.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/radau.hpp>
#include <crd/hesap/ode/rosenbrock.hpp>
#include <crd/hesap/ode/sdirk.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace ode = crd::hesap::ode;

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

// ---- canned problems (each carries its analytic Jacobian so the stiff methods run scipy-exact) ----
class Decay final : public ode::OdeFunction<crd::f64>
{
public:
    Decay() : ode::OdeFunction<crd::f64>(true) {}
    void rhs(crd::f64, crd::containers::ConstSpan<crd::f64> y, crd::containers::Span<crd::f64> d) const override
    {
        d[0] = -y[0];
    }
    [[nodiscard]] bool jacobian(crd::f64, crd::containers::ConstSpan<crd::f64>,
                                crd::containers::Span<crd::f64> j) const override
    {
        j[0] = -1.0;
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return 1; }
};

class VanDerPol final : public ode::OdeFunction<crd::f64>
{
public:
    explicit VanDerPol(crd::f64 mu) : ode::OdeFunction<crd::f64>(true), m_mu(mu) {}
    void rhs(crd::f64, crd::containers::ConstSpan<crd::f64> y, crd::containers::Span<crd::f64> d) const override
    {
        d[0] = y[1];
        d[1] = m_mu * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
    }
    [[nodiscard]] bool jacobian(crd::f64, crd::containers::ConstSpan<crd::f64> y,
                                crd::containers::Span<crd::f64> j) const override
    {
        j[0] = 0.0;
        j[1] = 1.0;
        j[2] = m_mu * (-2.0 * y[0] * y[1]) - 1.0;
        j[3] = m_mu * (1.0 - y[0] * y[0]);
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return 2; }

private:
    crd::f64 m_mu;
};

class Robertson final : public ode::OdeFunction<crd::f64>
{
public:
    Robertson() : ode::OdeFunction<crd::f64>(true) {}
    void rhs(crd::f64, crd::containers::ConstSpan<crd::f64> y, crd::containers::Span<crd::f64> d) const override
    {
        d[0] = -0.04 * y[0] + 1e4 * y[1] * y[2];
        d[1] = 0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] * y[1];
        d[2] = 3e7 * y[1] * y[1];
    }
    [[nodiscard]] bool jacobian(crd::f64, crd::containers::ConstSpan<crd::f64> y,
                                crd::containers::Span<crd::f64> j) const override
    {
        j[0] = -0.04;
        j[1] = 1e4 * y[2];
        j[2] = 1e4 * y[1];
        j[3] = 0.04;
        j[4] = -1e4 * y[2] - 6e7 * y[1];
        j[5] = -1e4 * y[1];
        j[6] = 0.0;
        j[7] = 6e7 * y[1];
        j[8] = 0.0;
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return 3; }
};

class Oscillator final : public ode::OdeFunction<crd::f64>
{
public:
    Oscillator() : ode::OdeFunction<crd::f64>(true) {}
    void rhs(crd::f64, crd::containers::ConstSpan<crd::f64> y, crd::containers::Span<crd::f64> d) const override
    {
        d[0] = y[1];
        d[1] = -y[0];
    }
    [[nodiscard]] bool jacobian(crd::f64, crd::containers::ConstSpan<crd::f64>,
                                crd::containers::Span<crd::f64> j) const override
    {
        j[0] = 0.0;
        j[1] = 1.0;
        j[2] = -1.0;
        j[3] = 0.0;
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return 2; }
};

ode::OdeResult<crd::f64> dispatch(const ode::OdeFunction<crd::f64>& fn, crd::f64 t1, const ode::OdeOptions<crd::f64>& o,
                                  crd::u64 method, crd::containers::Span<crd::f64> y, crd::memory::IAllocator* alloc)
{
    switch (method)
    {
        case 1:
            return ode::integrate_erk<crd::f64>(fn, 0.0, t1, y, o, alloc, ode::ErkMethod::Rk23);
        case 2:
            return ode::integrate_erk<crd::f64>(fn, 0.0, t1, y, o, alloc, ode::ErkMethod::Dop853);
        case 3:
            return ode::integrate_bdf<crd::f64>(fn, 0.0, t1, y, o, alloc);
        case 4:
            return ode::integrate_radau<crd::f64>(fn, 0.0, t1, y, o, alloc);
        case 5:
            return ode::integrate_rosenbrock<crd::f64>(fn, 0.0, t1, y, o, alloc);
        case 6:
            return ode::integrate_trbdf2<crd::f64>(fn, 0.0, t1, y, o, alloc);
        case 0:
        default:
            return ode::integrate_erk<crd::f64>(fn, 0.0, t1, y, o, alloc, ode::ErkMethod::Rk45);
    }
}

CommandResult impl_solve(const CommandArgs& args)
{
    const auto prob = args.get_u64("problem");
    const auto t1opt = args.get_f64("t1");
    if (!prob || !t1opt)
    {
        return error_result(args.alloc, "ode.solve: problem and t1 are required");
    }
    const crd::u64 method = args.get_u64("method").value_or(0);
    if (method > 6)
    {
        return error_result(args.alloc, "ode.solve: method must be 0..6");
    }
    ode::OdeOptions<crd::f64> o;
    o.rtol = args.get_f64("rtol").value_or(1e-6);
    o.atol = args.get_f64("atol").value_or(1e-9);
    const crd::f64 mu = args.get_f64("mu").value_or(1000.0);
    const crd::f64 t1 = *t1opt;

    crd::containers::Array<crd::f64> y(args.alloc);
    Decay decay;
    VanDerPol vdp(mu);
    Robertson rob;
    Oscillator osc;
    const ode::OdeFunction<crd::f64>* fn = nullptr;
    switch (*prob)
    {
        case 0:
            y.resize(1);
            y[0] = 1.0;
            fn = &decay;
            break;
        case 1:
            y.resize(2);
            y[0] = 2.0;
            y[1] = 0.0;
            fn = &vdp;
            break;
        case 2:
            y.resize(3);
            y[0] = 1.0;
            y[1] = 0.0;
            y[2] = 0.0;
            fn = &rob;
            break;
        case 3:
            y.resize(2);
            y[0] = 1.0;
            y[1] = 0.0;
            fn = &osc;
            break;
        default:
            return error_result(args.alloc, "ode.solve: problem must be 0..3");
    }
    const crd::usize n = fn->dim();
    const ode::OdeResult<crd::f64> r = dispatch(*fn, t1, o, method, crd::containers::Span<crd::f64>(y.data(), n), args.alloc);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(6 + n);
    out.push_back(static_cast<crd::f64>(static_cast<int>(r.status)));
    out.push_back(static_cast<crd::f64>(r.work.nfev));
    out.push_back(static_cast<crd::f64>(r.work.njev));
    out.push_back(static_cast<crd::f64>(r.work.nlu));
    out.push_back(static_cast<crd::f64>(r.work.naccept));
    out.push_back(static_cast<crd::f64>(r.work.nreject));
    for (crd::usize i = 0; i < n; ++i)
    {
        out.push_back(y[i]);
    }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

CommandSchema make_solve_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.ode.solve.f64", alloc};
    s.description = crd::containers::String{
        "Integrate a canned ODE to t1: problem 0 = decay (n=1), 1 = Van der Pol (n=2, param mu), 2 = Robertson "
        "(n=3, stiff), 3 = harmonic oscillator (n=2). method 0 RK45, 1 RK23, 2 DOP853, 3 BDF, 4 Radau, "
        "5 RODAS4, 6 TR-BDF2. Returns [status, nfev, njev, nlu, naccept, nreject, y(n)...].",
        alloc};
    s.output.kind = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "problem", "0 decay, 1 Van der Pol, 2 Robertson, 3 oscillator", ParamKind::U64, true);
    add_param(s, alloc, "t1", "End time (t0 = 0)", ParamKind::F64, true);
    add_param(s, alloc, "method", "0 RK45, 1 RK23, 2 DOP853, 3 BDF, 4 Radau, 5 RODAS4, 6 TR-BDF2", ParamKind::U64,
              false);
    add_param(s, alloc, "rtol", "Relative tolerance (default 1e-6)", ParamKind::F64, false);
    add_param(s, alloc, "atol", "Absolute tolerance (default 1e-9)", ParamKind::F64, false);
    add_param(s, alloc, "mu", "Van der Pol stiffness parameter (default 1000)", ParamKind::F64, false);
    return s;
}
} // namespace

namespace crd::hesap::ode
{
void register_ode_cli_anchor() noexcept {}
} // namespace crd::hesap::ode

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_solve_schema(alloc), &impl_solve);
    });
