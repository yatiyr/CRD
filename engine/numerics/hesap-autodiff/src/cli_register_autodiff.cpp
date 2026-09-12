// v15-z — CLI registration for forward-mode AD (hesap.ad.*). Derivatives operate on CALLABLES, so (the data-vs-
// callable split) agents reach the drivers through CANNED, named scalar/1-D functions + a point, exactly like
// hesap.ode.*:
//
//   hesap.ad.gradient.f64 : ∇f(x) of a canned f:Rⁿ→R via the SIMD vector-forward driver. func 0 Rosenbrock,
//                           1 sphere Σxᵢ², 2 Σxᵢ³. x = the point (n ≤ 32). Out [f(x), ∇f(n)...].
//   hesap.ad.hessian.f64  : ∇²f(x) (exact, hyper-dual) of the same canned f (n ≤ 6). Out [f(x), H row-major(n²)...].
//   hesap.ad.taylor.f64   : the order-K normalized Taylor coefficients a[k]=f⁽ᵏ⁾/k! of a canned 1-D f at x0.
//                           func 0 exp, 1 sin, 2 1/(1−x), 3 √(1+x). order ∈ {4,8,12,16}. Out [a_0..a_K].
// Anchor: register_autodiff_cli_anchor(). ADR-0097.

#include <crd/hesap/autodiff/forward.hpp>
#include <crd/hesap/autodiff/hvp.hpp>            // v16-z: forward-over-reverse H·v
#include <crd/hesap/autodiff/implicit_diff.hpp>  // v16-z: IFT root VJP
#include <crd/hesap/autodiff/reverse.hpp>        // v16-z: reverse-mode gradient/jacobian (+ tape.hpp)
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace ad  = crd::hesap::autodiff::forward;
namespace rev = crd::hesap::autodiff::reverse;

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind    = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value         = std::move(e);
    return r;
}

CommandResult blob_f64_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto*      raw     = reinterpret_cast<const crd::u8*>(values.data());
    const crd::usize n_bytes = values.size() * sizeof(crd::f64);
    blob.bytes.reserve(n_bytes);
    for (crd::usize i = 0; i < n_bytes; ++i) { blob.bytes.push_back(raw[i]); }
    r.value = std::move(blob);
    return r;
}

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name        = crd::containers::String{name, alloc};
    p.description  = crd::containers::String{desc, alloc};
    p.kind        = kind;
    p.required    = required;
    s.params.push_back(std::move(p));
}

// ---- canned scalar functions (polynomial ⇒ valid on every carrier: double / JetPackD / HyperDual) ----
struct Rosenbrock
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        T s = T(0.0);
        for (int i = 0; i < n - 1; ++i)
        {
            const T a = x[i + 1] - x[i] * x[i];
            const T b = T(1.0) - x[i];
            s         = s + T(100.0) * a * a + b * b;
        }
        return s;
    }
};
struct Sphere
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        T s = T(0.0);
        for (int i = 0; i < n; ++i) { s = s + x[i] * x[i]; }
        return s;
    }
};
struct Cubes
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        T s = T(0.0);
        for (int i = 0; i < n; ++i) { s = s + x[i] * x[i] * x[i]; }
        return s;
    }
};

crd::f64 eval_value(crd::u64 func, const crd::f64* x, int n)
{
    if (func == 1) { return Sphere{}(x, n); }
    if (func == 2) { return Cubes{}(x, n); }
    return Rosenbrock{}(x, n);
}

CommandResult impl_gradient(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty()) { return error_result(args.alloc, "ad.gradient: x is required"); }
    const crd::u64 func = args.get_u64("func").value_or(0);
    if (func > 2) { return error_result(args.alloc, "ad.gradient: func must be 0..2"); }
    const int n = static_cast<int>(x.size());
    if (n > 32) { return error_result(args.alloc, "ad.gradient: n must be <= 32"); }

    ad::JetPackD<8> scratch[32];
    crd::f64        g[32];
    const crd::containers::Span<crd::f64>        gs{g, static_cast<crd::usize>(n)};
    const crd::containers::Span<ad::JetPackD<8>> ss{scratch, static_cast<crd::usize>(n)};
    if (func == 1) { ad::gradient<8>(Sphere{}, x, gs, ss); }
    else if (func == 2) { ad::gradient<8>(Cubes{}, x, gs, ss); }
    else { ad::gradient<8>(Rosenbrock{}, x, gs, ss); }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(n) + 1);
    out.push_back(eval_value(func, x.data(), n));
    for (int i = 0; i < n; ++i) { out.push_back(g[i]); }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

template <int N>
void hessian_run(crd::u64 func, const crd::f64* x, crd::f64* h)
{
    if (func == 1) { ad::hessian<N>(Sphere{}, x, h); }
    else if (func == 2) { ad::hessian<N>(Cubes{}, x, h); }
    else { ad::hessian<N>(Rosenbrock{}, x, h); }
}

CommandResult impl_hessian(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty()) { return error_result(args.alloc, "ad.hessian: x is required"); }
    const crd::u64 func = args.get_u64("func").value_or(0);
    if (func > 2) { return error_result(args.alloc, "ad.hessian: func must be 0..2"); }
    const int n = static_cast<int>(x.size());
    if (n < 1 || n > 6) { return error_result(args.alloc, "ad.hessian: n must be 1..6"); }

    crd::f64 h[36];
    switch (n)
    {
    case 1: hessian_run<1>(func, x.data(), h); break;
    case 2: hessian_run<2>(func, x.data(), h); break;
    case 3: hessian_run<3>(func, x.data(), h); break;
    case 4: hessian_run<4>(func, x.data(), h); break;
    case 5: hessian_run<5>(func, x.data(), h); break;
    default: hessian_run<6>(func, x.data(), h); break;
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(1 + static_cast<crd::usize>(n) * static_cast<crd::usize>(n));
    out.push_back(eval_value(func, x.data(), n));
    for (int i = 0; i < n * n; ++i) { out.push_back(h[i]); }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

template <int K>
void taylor_run(crd::u64 func, crd::f64 x0, crd::f64* out)
{
    using TJ = ad::TaylorJet<crd::f64, K>;
    const TJ t = TJ::var(x0);
    TJ       r;
    if (func == 1) { r = ad::sin(t); }
    else if (func == 2) { r = TJ(1.0) / (TJ(1.0) - t); }
    else if (func == 3) { r = ad::sqrt(TJ(1.0) + t); }
    else { r = ad::exp(t); }
    for (int k = 0; k <= K; ++k) { out[k] = r.a[k]; }
}

CommandResult impl_taylor(const CommandArgs& args)
{
    const auto x0 = args.get_f64("x0");
    if (!x0) { return error_result(args.alloc, "ad.taylor: x0 is required"); }
    const crd::u64 func  = args.get_u64("func").value_or(0);
    const crd::u64 order = args.get_u64("order").value_or(8);
    if (func > 3) { return error_result(args.alloc, "ad.taylor: func must be 0..3"); }

    crd::f64 co[17];
    int      len = 0;
    switch (order)
    {
    case 4: taylor_run<4>(func, *x0, co); len = 5; break;
    case 8: taylor_run<8>(func, *x0, co); len = 9; break;
    case 12: taylor_run<12>(func, *x0, co); len = 13; break;
    case 16: taylor_run<16>(func, *x0, co); len = 17; break;
    default: return error_result(args.alloc, "ad.taylor: order must be 4, 8, 12, or 16");
    }
    return blob_f64_result(args.alloc, {co, static_cast<crd::usize>(len)});
}

CommandSchema make_gradient_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.gradient.f64", alloc};
    s.description = crd::containers::String{
        "Exact gradient ∇f(x) of a canned f:Rⁿ→R by forward-mode AD (SIMD vector-forward driver). func 0 Rosenbrock, "
        "1 sphere Σxᵢ², 2 Σxᵢ³. x = the point (n ≤ 32). Returns [f(x), ∇f(n)...].",
        alloc};
    s.output.kind          = OutputKind::BinaryBlob;
    s.required_caps.bits   = Capability::kHesapCompute;
    s.idempotent           = true;
    add_param(s, alloc, "func", "0 Rosenbrock, 1 sphere, 2 cubes", ParamKind::U64, false);
    add_param(s, alloc, "x", "(F64Array) evaluation point, n ≤ 32", ParamKind::F64, true);
    return s;
}
CommandSchema make_hessian_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.hessian.f64", alloc};
    s.description = crd::containers::String{
        "Exact Hessian ∇²f(x) of a canned f:Rⁿ→R by hyper-dual forward-over-forward AD. func 0 Rosenbrock, 1 sphere, "
        "2 cubes. x = the point (n ≤ 6). Returns [f(x), H row-major(n²)...].",
        alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "func", "0 Rosenbrock, 1 sphere, 2 cubes", ParamKind::U64, false);
    add_param(s, alloc, "x", "(F64Array) evaluation point, n ≤ 6", ParamKind::F64, true);
    return s;
}
CommandSchema make_taylor_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.taylor.f64", alloc};
    s.description = crd::containers::String{
        "Order-K normalized Taylor coefficients a[k]=f⁽ᵏ⁾(x0)/k! of a canned 1-D f. func 0 exp, 1 sin, 2 1/(1−x), "
        "3 √(1+x). order ∈ {4,8,12,16}. Returns [a_0..a_K].",
        alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "func", "0 exp, 1 sin, 2 1/(1−x), 3 √(1+x)", ParamKind::U64, false);
    add_param(s, alloc, "x0", "Expansion point", ParamKind::F64, true);
    add_param(s, alloc, "order", "Truncation order K ∈ {4,8,12,16} (default 8)", ParamKind::U64, false);
    return s;
}

// ===================== v16-z: REVERSE MODE + IMPLICIT DIFF =====================
// Canned scalar losses, TEMPLATED on the carrier V so ONE definition drives reverse::Var (rgradient),
// RVar<Dual> (hvp), AND plain f64 (the value) — mixed-scalar ops only, no V(double) construction.
struct RSphere
{
    template <class V>
    V operator()(const V* x, int n) const { V s = x[0] * x[0]; for (int i = 1; i < n; ++i) { s = s + x[i] * x[i]; } return s; }
};
struct RCubes
{
    template <class V>
    V operator()(const V* x, int n) const { V s = x[0] * x[0] * x[0]; for (int i = 1; i < n; ++i) { s = s + x[i] * x[i] * x[i]; } return s; }
};
struct RRosen
{
    template <class V>
    V operator()(const V* x, int n) const
    {
        V a = x[1] - x[0] * x[0];
        V b = 1.0 - x[0];
        V s = 100.0 * a * a + b * b;
        for (int i = 1; i < n - 1; ++i)
        {
            V a2 = x[i + 1] - x[i] * x[i];
            V b2 = 1.0 - x[i];
            s    = s + 100.0 * a2 * a2 + b2 * b2;
        }
        return s;
    }
};
struct RExpSum
{
    template <class V>
    V operator()(const V* x, int n) const { using crd::math::exp; V s = exp(x[0]); for (int i = 1; i < n; ++i) { s = s + exp(x[i]); } return s; }
};

CommandResult impl_rgradient(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty()) { return error_result(args.alloc, "ad.rgradient: x is required"); }
    const crd::u64 func = args.get_u64("func").value_or(0);
    if (func > 3) { return error_result(args.alloc, "ad.rgradient: func must be 0..3"); }
    const int n = static_cast<int>(x.size());
    if (n < 2 || n > 256) { return error_result(args.alloc, "ad.rgradient: n must be 2..256"); }

    rev::Tape tape(args.alloc);
    rev::Var  scratch[256];
    crd::f64  g[256];
    crd::f64  fval = 0.0;
    const crd::containers::Span<crd::f64> gs{g, static_cast<crd::usize>(n)};
    const crd::containers::Span<rev::Var> ss{scratch, static_cast<crd::usize>(n)};
    if (func == 1) { rev::gradient(RSphere{}, x, gs, tape, ss); fval = RSphere{}(x.data(), n); }
    else if (func == 2) { rev::gradient(RCubes{}, x, gs, tape, ss); fval = RCubes{}(x.data(), n); }
    else if (func == 3) { rev::gradient(RExpSum{}, x, gs, tape, ss); fval = RExpSum{}(x.data(), n); }
    else { rev::gradient(RRosen{}, x, gs, tape, ss); fval = RRosen{}(x.data(), n); }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(n) + 1);
    out.push_back(fval);
    for (int i = 0; i < n; ++i) { out.push_back(g[i]); }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

// canned coupled vector map f:Rⁿ→Rⁿ, f_j = x_j² + x_{(j+1) mod n} — Jacobian J_jj=2x_j, J_{j,(j+1)}=1.
struct RCoupled
{
    void operator()(const rev::Var* x, int n, rev::Var* y, int m) const
    {
        for (int j = 0; j < m; ++j) { y[j] = x[j] * x[j] + x[(j + 1) % n]; }
    }
};

CommandResult impl_jacobian(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    if (x.empty()) { return error_result(args.alloc, "ad.jacobian: x is required"); }
    const int n = static_cast<int>(x.size());
    if (n < 2 || n > 64) { return error_result(args.alloc, "ad.jacobian: n must be 2..64"); }
    rev::Tape tape(args.alloc);
    rev::Var  xs[64];
    rev::Var  ys[64];
    crd::f64  jac[64 * 64];
    const crd::containers::Span<crd::f64> js{jac, (static_cast<crd::usize>(n) * static_cast<crd::usize>(n))};
    const crd::containers::Span<rev::Var> xss{xs, static_cast<crd::usize>(n)};
    const crd::containers::Span<rev::Var> yss{ys, static_cast<crd::usize>(n)};
    rev::jacobian(RCoupled{}, x, n, js, tape, xss, yss);
    return blob_f64_result(args.alloc, {jac, (static_cast<crd::usize>(n) * static_cast<crd::usize>(n))});
}

CommandResult impl_hvp(const CommandArgs& args)
{
    const auto x = args.get_f64_array("x");
    const auto v = args.get_f64_array("v");
    if (x.empty() || v.empty()) { return error_result(args.alloc, "ad.hvp: x and v are required"); }
    const crd::u64 func = args.get_u64("func").value_or(0);
    if (func > 2) { return error_result(args.alloc, "ad.hvp: func must be 0..2"); }
    const int n = static_cast<int>(x.size());
    if (n < 2 || n > 64) { return error_result(args.alloc, "ad.hvp: n must be 2..64"); }
    if (static_cast<int>(v.size()) != n) { return error_result(args.alloc, "ad.hvp: v must match x length"); }

    using RTD = rev::RTape<rev::Dual<crd::f64>>;
    RTD                            tape(args.alloc);
    rev::RVar<rev::Dual<crd::f64>> scr[64];
    crd::f64                       grad[64];
    crd::f64                       hv[64];
    const crd::containers::Span<crd::f64>                       gs{grad, static_cast<crd::usize>(n)};
    const crd::containers::Span<crd::f64>                       hs{hv, static_cast<crd::usize>(n)};
    const crd::containers::Span<rev::RVar<rev::Dual<crd::f64>>> ss{scr, static_cast<crd::usize>(n)};
    if (func == 1) { rev::hvp(RSphere{}, x, v, gs, hs, tape, ss); }
    else if (func == 2) { rev::hvp(RCubes{}, x, v, gs, hs, tape, ss); }
    else { rev::hvp(RRosen{}, x, v, gs, hs, tape, ss); }

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 * static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i) { out.push_back(grad[i]); }
    for (int i = 0; i < n; ++i) { out.push_back(hv[i]); }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

// canned root F_i(x,θ) = x_i² − θ_i  (x*_i = √θ_i ⇒ dx*_i/dθ_i = 1/(2√θ_i)) — exercises the IFT VJP.
struct RImplF
{
    void operator()(const rev::Var* x, const rev::Var* th, rev::Var* out, int n, int /*np*/) const
    {
        for (int i = 0; i < n; ++i) { out[i] = x[i] * x[i] - th[i]; }
    }
};

CommandResult impl_implicit(const CommandArgs& args)
{
    const auto theta = args.get_f64_array("theta");
    if (theta.empty()) { return error_result(args.alloc, "ad.implicit: theta is required"); }
    const int n = static_cast<int>(theta.size());
    if (n < 1 || n > 16) { return error_result(args.alloc, "ad.implicit: n must be 1..16"); }
    for (int i = 0; i < n; ++i) { if (theta[i] <= 0.0) { return error_result(args.alloc, "ad.implicit: theta must be > 0"); } }

    crd::f64 xstar[16];
    crd::f64 xbar[16];
    crd::f64 tbar[16];
    for (int i = 0; i < n; ++i) { xstar[i] = crd::math::sqrt(theta[i]); xbar[i] = 1.0; }
    rev::Tape tape(args.alloc);
    rev::Var  vscr[3 * 16];
    crd::f64  jac[16 * 16];
    int       piv[16];
    crd::f64  z[16];
    crd::f64  tmp[16];
    rev::root_vjp(RImplF{}, xstar, theta.data(), xbar, tbar, n, n, tape, vscr, jac, piv, z, tmp);

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(2 * static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i) { out.push_back(xstar[i]); }
    for (int i = 0; i < n; ++i) { out.push_back(tbar[i]); }
    return blob_f64_result(args.alloc, {out.data(), out.size()});
}

CommandSchema make_rgradient_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.rgradient.f64", alloc};
    s.description = crd::containers::String{
        "Exact gradient ∇f(x) of a canned f:Rⁿ→R by REVERSE-mode AD — the whole gradient in ONE backward pass (O(n), "
        "scales past the forward driver). func 0 Rosenbrock, 1 sphere, 2 cubes, 3 Σexp(xᵢ). x = the point (2 ≤ n ≤ "
        "256). Returns [f(x), ∇f(n)...].",
        alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "func", "0 Rosenbrock, 1 sphere, 2 cubes, 3 Σexp(xᵢ)", ParamKind::U64, false);
    add_param(s, alloc, "x", "(F64Array) evaluation point, 2 ≤ n ≤ 256", ParamKind::F64, true);
    return s;
}
CommandSchema make_jacobian_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.jacobian.f64", alloc};
    s.description = crd::containers::String{
        "Exact Jacobian J(x) of the canned coupled map f_j = x_j² + x_{(j+1) mod n} (f:Rⁿ→Rⁿ) by reverse-mode AD "
        "(graph once, one backward per output row). x = the point (2 ≤ n ≤ 64). Returns J row-major(n²).",
        alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "x", "(F64Array) evaluation point, 2 ≤ n ≤ 64", ParamKind::F64, true);
    return s;
}
CommandSchema make_hvp_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.hvp.f64", alloc};
    s.description = crd::containers::String{
        "Exact Hessian-vector product ∇²f(x)·v of a canned f:Rⁿ→R by forward-over-reverse AD (grad AND H·v in ONE "
        "build + ONE backward, never forms H). func 0 Rosenbrock, 1 sphere, 2 cubes. x, v = point + direction (2 ≤ n "
        "≤ 64). Returns [∇f(n)..., (∇²f·v)(n)...].",
        alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "func", "0 Rosenbrock, 1 sphere, 2 cubes", ParamKind::U64, false);
    add_param(s, alloc, "x", "(F64Array) evaluation point, 2 ≤ n ≤ 64", ParamKind::F64, true);
    add_param(s, alloc, "v", "(F64Array) direction, length n", ParamKind::F64, true);
    return s;
}
CommandSchema make_implicit_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name        = crd::containers::String{"hesap.ad.implicit.f64", alloc};
    s.description = crd::containers::String{
        "Implicit differentiation via the IFT: for the canned root F_i(x,θ)=x_i²−θ_i (x*_i=√θ_i), returns the root x* "
        "and the parameter gradient dL/dθ of L=Σx*_i (seed x̄=1) WITHOUT unrolling any solver. θ = the parameters "
        "(θ_i > 0, 1 ≤ n ≤ 16). Returns [x*(n)..., dL/dθ(n)...] (dL/dθ_i = 1/(2√θ_i)).",
        alloc};
    s.output.kind        = OutputKind::BinaryBlob;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "theta", "(F64Array) parameters θ_i > 0, 1 ≤ n ≤ 16", ParamKind::F64, true);
    return s;
}
} // namespace

namespace crd::hesap::autodiff
{
void register_autodiff_cli_anchor() noexcept {}
} // namespace crd::hesap::autodiff

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_gradient_schema(alloc), &impl_gradient);
        reg.register_command(make_hessian_schema(alloc), &impl_hessian);
        reg.register_command(make_taylor_schema(alloc), &impl_taylor);
        reg.register_command(make_rgradient_schema(alloc), &impl_rgradient); // v16-z reverse mode
        reg.register_command(make_jacobian_schema(alloc), &impl_jacobian);
        reg.register_command(make_hvp_schema(alloc), &impl_hvp);
        reg.register_command(make_implicit_schema(alloc), &impl_implicit);
    });
