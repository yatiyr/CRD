// test_reverse_rules.cpp — Phase 3.1.6 v16-b: the full scalar VJP rule library. Every rule is gated 3 ways: the
// reverse VJP ≡ the v15 forward JVP (the TRANSPOSE identity — exact, both use the same forward::detail slope) ≡ a
// central finite difference (independent numerical oracle). Covers the whole crd::math unary surface, the binary
// rules (atan2/hypot/pow), and control flow (abs/min/max/select carry the taken branch's derivative).

#include <crd/hesap/autodiff/forward.hpp> // forward::Dual — the transpose oracle
#include <crd/hesap/autodiff/reverse.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
namespace fwd = crd::hesap::autodiff::forward;
using crd::f64;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
// scalar-generic unary functors (namespace scope — MSVC forbids template operator() on a LOCAL class).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VFN(NAME)                                                                                                     \
    struct Fn_##NAME                                                                                                  \
    {                                                                                                                 \
        template <class T>                                                                                           \
        T operator()(const T& x) const                                                                              \
        {                                                                                                           \
            using crd::math::NAME;                                                                                  \
            return NAME(x);                                                                                          \
        }                                                                                                           \
    };
VFN(exp) VFN(log) VFN(sin) VFN(cos) VFN(tan) VFN(sqrt) VFN(tanh) VFN(cbrt) VFN(rsqrt)
VFN(asin) VFN(acos) VFN(atan) VFN(sinh) VFN(cosh) VFN(asinh) VFN(acosh) VFN(atanh)
VFN(exp2) VFN(exp10) VFN(expm1) VFN(log2) VFN(log10) VFN(log1p)
#undef VFN

struct Atan2F
{
    template <class T>
    T operator()(const T& y, const T& x) const { using crd::math::atan2; return atan2(y, x); }
};
struct HypotF
{
    template <class T>
    T operator()(const T& x, const T& y) const { using crd::math::hypot; return hypot(x, y); }
};
struct PowF
{
    template <class T>
    T operator()(const T& x, const T& y) const { using crd::math::pow; return pow(x, y); }
};

// gate a unary rule: reverse VJP ≡ forward JVP (exact) ≡ central FD.
template <class Fn>
void gate1(const Fn& f, f64 x, f64 fd_tol)
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    rev::Tape                  t(&alloc);
    const rev::Var             lx = rev::make_leaf(t, x);
    const rev::Var             ly = f(lx);
    t.seed(ly.node, 1.0);
    t.backward();
    const f64 reverse = t.grad(lx.node);

    const f64 forward = f(fwd::Dual<f64>{x, 1.0}).d;
    const f64 h       = 1e-6;
    const f64 fd      = (f(x + h) - f(x - h)) / (2.0 * h);

    CHECK_THAT(reverse, WithinRel(forward, 1e-11)); // transpose identity
    CHECK_THAT(reverse, WithinAbs(fd, fd_tol));     // independent FD
}

template <class Fn>
void gate2(const Fn& f, f64 a, f64 b, f64 fd_tol)
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    rev::Tape                  t(&alloc);
    const rev::Var             la = rev::make_leaf(t, a);
    const rev::Var             lb = rev::make_leaf(t, b);
    const rev::Var             ly = f(la, lb);
    t.seed(ly.node, 1.0);
    t.backward();
    const f64 ra = t.grad(la.node);
    const f64 rb = t.grad(lb.node);

    const f64 fa = f(fwd::Dual<f64>{a, 1.0}, fwd::Dual<f64>{b, 0.0}).d;
    const f64 fb = f(fwd::Dual<f64>{a, 0.0}, fwd::Dual<f64>{b, 1.0}).d;
    const f64 h  = 1e-6;
    const f64 da = (f(a + h, b) - f(a - h, b)) / (2.0 * h);
    const f64 db = (f(a, b + h) - f(a, b - h)) / (2.0 * h);

    CHECK_THAT(ra, WithinRel(fa, 1e-11));
    CHECK_THAT(rb, WithinRel(fb, 1e-11));
    CHECK_THAT(ra, WithinAbs(da, fd_tol));
    CHECK_THAT(rb, WithinAbs(db, fd_tol));
}
} // namespace

TEST_CASE("reverse VJP: full crd::math unary surface == forward == FD", "[autodiff][reverse][rules]")
{
    gate1(Fn_exp{}, 0.7, 1e-5);
    gate1(Fn_log{}, 1.5, 1e-5);
    gate1(Fn_sin{}, 0.7, 1e-6);
    gate1(Fn_cos{}, 0.7, 1e-6);
    gate1(Fn_tan{}, 0.5, 1e-5);
    gate1(Fn_sqrt{}, 1.5, 1e-6);
    gate1(Fn_tanh{}, 0.6, 1e-6);
    gate1(Fn_cbrt{}, 1.5, 1e-6);
    gate1(Fn_rsqrt{}, 1.5, 1e-6);
    gate1(Fn_asin{}, 0.3, 1e-6);
    gate1(Fn_acos{}, 0.3, 1e-6);
    gate1(Fn_atan{}, 0.7, 1e-6);
    gate1(Fn_sinh{}, 0.6, 1e-5);
    gate1(Fn_cosh{}, 0.6, 1e-5);
    gate1(Fn_asinh{}, 0.7, 1e-6);
    gate1(Fn_acosh{}, 1.5, 1e-6);
    gate1(Fn_atanh{}, 0.3, 1e-6);
    gate1(Fn_exp2{}, 0.7, 1e-5);
    gate1(Fn_exp10{}, 0.4, 1e-4);
    gate1(Fn_expm1{}, 0.5, 1e-5);
    gate1(Fn_log2{}, 1.5, 1e-6);
    gate1(Fn_log10{}, 1.5, 1e-6);
    gate1(Fn_log1p{}, 0.5, 1e-6);
}

TEST_CASE("reverse VJP: binary rules atan2/hypot/pow == forward == FD", "[autodiff][reverse][rules]")
{
    gate2(Atan2F{}, 0.8, 1.3, 1e-6);
    gate2(HypotF{}, 0.8, 1.3, 1e-6);
    gate2(PowF{}, 1.4, 2.2, 1e-5); // x>0 holomorphic branch
}

TEST_CASE("reverse VJP: control flow carries the taken branch", "[autodiff][reverse][rules]")
{
    crd::memory::TlsfAllocator alloc(1 << 20);
    rev::Tape                  t(&alloc);
    // abs: grad = sign
    {
        t.reset();
        const rev::Var x = rev::make_leaf(t, -2.5);
        const rev::Var y = rev::abs(x);
        t.seed(y.node, 1.0);
        t.backward();
        CHECK(y.val() == 2.5);
        CHECK_THAT(t.grad(x.node), WithinRel(-1.0, 1e-14));
    }
    // max(a,b): grad flows to the larger; the other gets 0
    {
        t.reset();
        const rev::Var a = rev::make_leaf(t, 1.0);
        const rev::Var b = rev::make_leaf(t, 3.0);
        const rev::Var y = rev::max(a, b);
        t.seed(y.node, 1.0);
        t.backward();
        CHECK(y.val() == 3.0);
        CHECK_THAT(t.grad(b.node), WithinRel(1.0, 1e-14));
        CHECK_THAT(t.grad(a.node), WithinAbs(0.0, 1e-14));
    }
    // select
    {
        t.reset();
        const rev::Var a = rev::make_leaf(t, 5.0);
        const rev::Var b = rev::make_leaf(t, 7.0);
        const rev::Var y = rev::select(false, a, b);
        t.seed(y.node, 1.0);
        t.backward();
        CHECK(y.val() == 7.0);
        CHECK_THAT(t.grad(b.node), WithinRel(1.0, 1e-14));
        CHECK_THAT(t.grad(a.node), WithinAbs(0.0, 1e-14));
    }
}
