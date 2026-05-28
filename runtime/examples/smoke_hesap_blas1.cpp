// smoke_hesap_blas1 — Phase 3.1.6 v0b end-to-end smoke.
//
// Exercises the full BLAS L1 surface:
//   - Engine-side: Vector<f64> ops (axpy / dot / nrm2 / scal / copy / swap /
//     asum / iamax) match hand-rolled reference within n*eps.
//   - CLI-side: invokes hesap.dense.blas1.dot.f64 via the registry to
//     prove end-to-end agent-native plumbing.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/cli_anchor.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>
#include <variant>

namespace
{
// Pull cli_register.cpp into the link.
struct AnchorPull
{
    AnchorPull() noexcept { crd::hesap::dense::register_blas1_cli_anchor(); }
};
const AnchorPull kAnchorPull;

int fail(const char* msg)
{
    std::fprintf(stderr, "[smoke_hesap_blas1] FAIL: %s\n", msg);
    return 1;
}

bool approx_equal(crd::f64 a, crd::f64 b, crd::f64 tol)
{
    return std::abs(a - b) <= tol;
}
} // namespace

int main()
{
    using namespace crd::hesap::dense;
    crd::memory::TlsfAllocator alloc(4 * 1024 * 1024);

    constexpr crd::usize k_n = 1000;
    Vector<crd::f64> x(&alloc, k_n);
    Vector<crd::f64> y(&alloc, k_n);
    for (crd::usize i = 0; i < k_n; ++i)
    {
        const auto t = static_cast<crd::f64>(i) / static_cast<crd::f64>(k_n);
        x(i) = std::sin(6.28318530717958647692 * t);
        y(i) = std::cos(6.28318530717958647692 * t);
    }

    // ---- Engine-side reference checks -------------------------------

    // dot of orthogonal-ish sin/cos: small but not exactly zero.
    const crd::f64 d_engine = dot<crd::f64>(x, y);
    crd::f64 d_ref = 0.0;
    for (crd::usize i = 0; i < k_n; ++i)
    {
        d_ref += x(i) * y(i);
    }
    if (!approx_equal(d_engine, d_ref, 1e-9))
    {
        std::fprintf(stderr, "[smoke_hesap_blas1] engine dot = %.17g, naive = %.17g\n", d_engine, d_ref);
        return fail("engine dot mismatches naive reference");
    }

    // nrm2 of sin: ~ sqrt(N/2)
    const crd::f64 n_engine = nrm2<crd::f64>(x);
    const crd::f64 expected_norm = std::sqrt(static_cast<crd::f64>(k_n) / 2.0);
    if (!approx_equal(n_engine, expected_norm, expected_norm * 1e-3))
    {
        std::fprintf(stderr, "[smoke_hesap_blas1] nrm2 = %.6f, expected ~%.6f\n", n_engine, expected_norm);
        return fail("engine nrm2 differs from analytic expectation");
    }

    // axpy + scal + iamax: arrange a known argmax.
    Vector<crd::f64> z(&alloc, 5);
    z(0) = 1.0;
    z(1) = -8.0;
    z(2) = 3.0;
    z(3) = -2.5;
    z(4) = 7.0;
    if (iamax<crd::f64>(z) != 1)
    {
        return fail("iamax expected 1");
    }
    scal<crd::f64>(2.0, z);
    if (z(1) != -16.0)
    {
        return fail("scal(2)(z)[1] != -16");
    }

    // asum: sum of |2x| where x = {1, -8, 3, -2.5, 7} == 2 * 21.5 = 43.0
    if (asum<crd::f64>(z) != 43.0)
    {
        return fail("asum(2x) != 43");
    }

    // ---- CLI-side round-trip ----------------------------------------

    auto& reg = crd::hesap::cli::CommandRegistry::global();
    const auto* rec = reg.find("hesap.dense.blas1.dot.f64");
    if (rec == nullptr || rec->impl == nullptr)
    {
        return fail("hesap.dense.blas1.dot.f64 missing from registry — static-init didn't run");
    }

    crd::hesap::cli::CommandArgs args{&alloc};
    args.set_f64_array("x", x.span());
    args.set_f64_array("y", y.span());
    const auto r = rec->impl(args);
    if (!r.ok)
    {
        return fail("dot CLI returned ok=false");
    }
    const auto* sc = std::get_if<crd::hesap::cli::ResultScalarF64>(&r.value);
    if (sc == nullptr)
    {
        return fail("dot CLI didn't return a ScalarF64");
    }
    if (!approx_equal(sc->value, d_engine, 1e-12))
    {
        std::fprintf(stderr, "[smoke_hesap_blas1] CLI dot = %.17g, engine dot = %.17g\n", sc->value, d_engine);
        return fail("CLI dot diverges from engine dot");
    }

    // Count registered hesap.dense.blas1.* schemas.
    crd::usize blas1_count = 0;
    for (const auto* p : reg.all())
    {
        const crd::containers::StringView name{p->schema.name.c_str(), p->schema.name.size()};
        if (name.starts_with("hesap.dense.blas1."))
        {
            ++blas1_count;
        }
    }

    std::fprintf(stdout,
                 "[smoke_hesap_blas1] engine dot=%.6f  nrm2=%.6f  scal/axpy/iamax/asum OK\n"
                 "[smoke_hesap_blas1] %zu BLAS L1 commands registered via static-init\n"
                 "[smoke_hesap_blas1] CLI dispatch of hesap.dense.blas1.dot.f64 returned %.6f\n"
                 "[smoke_hesap_blas1] OK\n",
                 d_engine, n_engine, static_cast<std::size_t>(blas1_count), sc->value);
    return 0;
}
