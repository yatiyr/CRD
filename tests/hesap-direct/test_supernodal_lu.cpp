// crd-hesap-direct v5b-2a — MC64 static-pivot FRONT-END tests.
//
// The bar: validate the transform composition END-TO-END via the residual on the
// ORIGINAL A. Factor the transformed B = perm_cols(D_r·A·D_c) with the v5b-1 GP-LU
// oracle, solve through transform_rhs → B⁻¹ → untransform_solution, and check
// ‖A·x − b‖/‖b‖. A wrong permutation/scaling composition fails the residual, so the
// front-end is self-verifying. Also check the static-pivot amenability metric
// (min_diag_dominance → 1 ⇒ MC64 made the diagonal the column-max ⇒ static pivot is
// exact — the foundation of v5b-2's parallel-deterministic numeric).

#include <crd/hesap/complex.hpp>
#include <crd/hesap/direct/frontal.hpp>
#include <crd/hesap/direct/lu_symbolic.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/sparse_lu.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>

namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;

namespace
{
template <typename T> void csr_matvec(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, const T* x, T* y)
{
    const sp::SparsePattern& p = a.pattern();
    const T* v = a.values().values.data();
    for (crd::u32 r = 0; r < p.rows; ++r)
    {
        T acc{};
        for (crd::u32 k = p.outer_ptr[r]; k < p.outer_ptr[r + 1]; ++k)
        {
            acc = acc + v[k] * x[p.inner_idx[k]];
        }
        y[r] = acc;
    }
}

// Solve A·x = b through the static-pivot front-end (factoring B with the v5b-1 GP-LU oracle).
// Returns the min_diag_dominance metric; writes x. info != 0 ⇒ B singular ⇒ returns -1.
template <typename T>
double solve_static(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, const T* b, T* x,
                    crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.pattern().rows;
    sp::SparseMatrix<T, sp::SparseFormat::Csc> bmat(alloc);
    auto sc = dir::static_lu_prepare<T>(a, bmat, alloc);
    auto lu = dir::factor_gp_lu<T>(bmat, alloc);
    if (lu.info() != 0)
    {
        return -1.0;
    }
    crd::containers::Array<T> c(alloc);
    crd::containers::Array<T> y(alloc);
    c.resize(n);
    y.resize(n);
    sc.transform_rhs({b, n}, {c.data(), n}); // c = D_r·b
    for (crd::u32 i = 0; i < n; ++i)
    {
        y[i] = c[i];
    }
    if (!lu.solve({y.data(), n})) // y = B⁻¹·c
    {
        return -1.0;
    }
    sc.untransform_solution({y.data(), n}, {x, n}); // x[col_match[j]] = D_c·y[j]
    return sc.min_diag_dominance;
}

template <typename T> double max_err(const T* a, const T* b, crd::u32 n) // complex T: abs handles 0 ⇒ 0
{
    double e = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double d = crd::hesap::abs(a[i] - b[i]);
        e = d > e ? d : e;
    }
    return e;
}
double max_err(const crd::f64* a, const crd::f64* b, crd::u32 n)
{
    double e = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        e = std::max(e, std::abs(a[i] - b[i]));
    }
    return e;
}
} // namespace

TEST_CASE("v5b-2a static front-end: clean unsymmetric system (residual + diag-dominance)", "[lu][v5b-2a]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 40;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(n + 2));
        if (i + 1 < n)
            tb.add(i, i + 1, 1.0);
        if (i > 0)
            tb.add(i, i - 1, -2.0);
    }
    auto a = tb.compress();
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());
    const double dom = solve_static<crd::f64>(a, b.data(), x.data(), &alloc);
    CHECK(dom > 0.5); // diagonally dominant ⇒ MC64 keeps the diagonal, dom high
    CHECK(max_err(x.data(), xtrue.data(), n) < 1e-10);
}

TEST_CASE("v5b-2a static front-end: MC64 RESCUES a tiny-diagonal matrix (static-pivot enabler)", "[lu][v5b-2a]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // Two decoupled 2×2 blocks, each with a TINY diagonal + a LARGE anti-diagonal: a naive
    // diagonal pivot would hit ~0; MC64 matches the large entries onto the diagonal ⇒ B is
    // diagonally dominant ⇒ a STATIC diagonal pivot is safe. The whole point of v5b-2.
    const crd::u32 n = 4;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    tb.add(0, 0, 1e-9);
    tb.add(0, 1, 5.0);
    tb.add(1, 0, 4.0);
    tb.add(1, 1, 1e-9);
    tb.add(2, 2, 1e-9);
    tb.add(2, 3, 3.0);
    tb.add(3, 2, 2.0);
    tb.add(3, 3, 1e-9);
    auto a = tb.compress();
    const crd::f64 xtrue[4] = {1.0, -2.0, 3.0, -1.0};
    crd::f64 b[4];
    csr_matvec<crd::f64>(a, xtrue, b);
    crd::f64 x[4];
    const double dom = solve_static<crd::f64>(a, b, x, &alloc);
    CHECK(dom > 0.9);                   // MC64 put the large entries on the diagonal ⇒ ~1
    CHECK(max_err(x, xtrue, 4) < 1e-9); // and the solve is accurate through the transform
}

TEST_CASE("v5b-2a static front-end: MC64 balances a BADLY-SCALED matrix", "[lu][v5b-2a]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Row i scaled by 10^(i mod 6 - 3) — magnitudes span ~10^6. MC64's D_r/D_c scaling
    // rebalances toward an I-matrix so the transformed B is well-conditioned for static pivot.
    const crd::u32 n = 24;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 s = std::pow(10.0, static_cast<crd::f64>(static_cast<int>(i % 6) - 3));
        tb.add(i, i, s * static_cast<crd::f64>(n + 2));
        if (i + 1 < n)
            tb.add(i, i + 1, s * 1.0);
        if (i > 0)
            tb.add(i, i - 1, s * -2.0);
    }
    auto a = tb.compress();
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 - 0.03 * static_cast<crd::f64>(i);
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());
    const double dom = solve_static<crd::f64>(a, b.data(), x.data(), &alloc);
    CHECK(dom > 0.5);
    CHECK(max_err(x.data(), xtrue.data(), n) < 1e-9);
}

TEST_CASE("v5b-2a static front-end: complex (Complex64)", "[lu][v5b-2a][complex]")
{
    using C = crd::hesap::Complex64;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 20;
    sp::TripletBuilder<C> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, C{static_cast<crd::f64>(n + 2), 1.0});
        if (i + 1 < n)
            tb.add(i, i + 1, C{1.0, -0.5});
        if (i > 0)
            tb.add(i, i - 1, C{-2.0, 0.3});
    }
    auto a = tb.compress();
    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> b(&alloc);
    crd::containers::Array<C> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.1 * static_cast<crd::f64>(i), 0.2 * static_cast<crd::f64>(i)};
    }
    csr_matvec<C>(a, xtrue.data(), b.data());
    const double dom = solve_static<C>(a, b.data(), x.data(), &alloc);
    CHECK(dom > 0.0);
    CHECK(max_err<C>(x.data(), xtrue.data(), n) < 1e-9);
}

// =====================================================================================
// v5b-2b — supernodal-LU SYMBOLIC: the exact static-pivot L/U structure.
//
// The gate (advisor): the structure must be a SUPERSET of the actual fill the v5b-1
// Gilbert-Peierls oracle produces on B with the diagonal forced as the pivot (tol → 0 =
// the static-pivot sequence). Reachability is a superset of any numeric fill under the
// SAME pivots (exact modulo cancellation), so for these strongly-diagonally-dominant B
// it is exactly equal. Plus the Gilbert-Ng sandwich (symbolic nnz ≤ chol(BᵀB) bound),
// run-to-run determinism (pure function of B), and the supernode panel-density invariant.
// =====================================================================================
namespace
{
// Returns true iff every entry of the GP-LU(tol→0) factor of B lies in the v5b-2b symbolic
// structure of the same column. Reports the nnz figures + the Gilbert-Ng fill bound.
template <typename T>
bool symbolic_superset_of_gp(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                             crd::u64& sym_nnz, crd::u64& gp_nnz, crd::u64& fill_bound)
{
    sp::SparseMatrix<T, sp::SparseFormat::Csc> bmat(alloc);
    auto sc = dir::static_lu_prepare<T>(a, bmat, alloc);
    (void)sc;
    auto sym = dir::lu_symbolic(bmat.pattern(), alloc);
    auto lu = dir::factor_gp_lu<T>(bmat, alloc, 0.0); // tol → 0 ⇒ diagonal (static) pivot
    sym_nnz = sym.nnz();
    gp_nnz = lu.factor_nnz();
    fill_bound = sym.fill_bound;
    if (lu.info() != 0)
    {
        return false;
    }
    const crd::u32 n = sym.n;
    crd::containers::Array<crd::u8> mark(alloc);
    mark.resize(n);
    const auto lcp = lu.l_colptr();
    const auto lri = lu.l_rowidx();
    const auto ucp = lu.u_colptr();
    const auto uri = lu.u_rowidx();
    bool ok = true;
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 p = sym.lp[k]; p < sym.lp[k + 1]; ++p)
        {
            mark[sym.li[p]] = 1;
        }
        for (crd::u32 p = sym.up[k]; p < sym.up[k + 1]; ++p)
        {
            mark[sym.ui[p]] = 1;
        }
        for (crd::u32 p = lcp[k]; p < lcp[k + 1]; ++p)
        {
            if (mark[lri[p]] == 0)
            {
                ok = false;
            }
        }
        for (crd::u32 p = ucp[k]; p < ucp[k + 1]; ++p)
        {
            if (mark[uri[p]] == 0)
            {
                ok = false;
            }
        }
        for (crd::u32 p = sym.lp[k]; p < sym.lp[k + 1]; ++p)
        {
            mark[sym.li[p]] = 0;
        }
        for (crd::u32 p = sym.up[k]; p < sym.up[k + 1]; ++p)
        {
            mark[sym.ui[p]] = 0;
        }
    }
    return ok;
}

// Strongly diagonally dominant unsymmetric matrix: big diagonal + a sub-diagonal + a
// super-2 (breaks symmetry) + an arrow-ish column 0 (fattens the fill). MC64 keeps the
// diagonal (DD) ⇒ static pivot is exact.
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> dd_unsymmetric(crd::memory::IAllocator* alloc, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(4 * n + 10));
        if (i > 0)
        {
            tb.add(i, i - 1, -1.0);
        }
        if (i + 2 < n)
        {
            tb.add(i, i + 2, 0.5);
        }
    }
    for (crd::u32 i = 4; i < n; i += 4)
    {
        tb.add(i, 0, 0.25);
    }
    return tb.compress();
}

// STRONGLY-divergent unsymmetric pattern: the L and U structures point in different directions, to
// stress the v5b-3a multifrontal containment invariant (CB(child) subset parent front in BOTH dims).
// Big diagonal (MC64 keeps it ⇒ static pivot exact) + a sub-diagonal AND a long super-3 (L vs U
// diverge) + a DENSE COLUMN c=2 (tall L foot, rows below) + a DENSE ROW r=5 (wide U, columns right)
// at different positions — the asymmetry that makes the column-etree (rows) and row-structure (cols)
// disagree, which is exactly where a single-parent extend_add could break.
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> divergent_unsymmetric(crd::memory::IAllocator* alloc, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(8 * n + 16));
        if (i > 0)
        {
            tb.add(i, i - 1, -1.0); // sub-diagonal → L
        }
        if (i + 3 < n)
        {
            tb.add(i, i + 3, 0.3); // long super → U (diverges from L)
        }
    }
    for (crd::u32 i = 6; i < n; ++i)
    {
        tb.add(i, 2, 0.2); // dense column 2 → tall L foot
    }
    if (n > 8)
    {
        for (crd::u32 j = 8; j < n; ++j)
        {
            tb.add(5, j, 0.15); // dense row 5 → wide U
        }
    }
    return tb.compress();
}

// Structurally SYMMETRIC, strongly diagonally dominant ⇒ MC64 keeps the identity column perm and the
// AMD reorder is a symmetric permutation ⇒ B stays structurally symmetric. The POSITIVE control: for a
// symmetric pattern the LU front structure reduces to the Cholesky case, where CB(child) subset parent
// is a THEOREM ⇒ containment MUST hold. Validates the containment check itself (so unsymmetric
// violations are a real structural property, not a derivation bug).
sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr> symmetric_dd(crd::memory::IAllocator* alloc, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(8 * n + 16));
        if (i > 0)
        {
            tb.add(i, i - 1, -1.0); // symmetric pair with (i-1, i) below
            tb.add(i - 1, i, -1.0);
        }
        if (i + 2 < n)
        {
            tb.add(i, i + 2, 0.4); // symmetric pair
            tb.add(i + 2, i, 0.4);
        }
    }
    return tb.compress();
}

// Build the v5b-3a UNSYMMETRIC-pattern multifrontal symbolic from A and run the containment check.
dir::MfContainmentReport containment_of(const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& a,
                                        crd::memory::IAllocator* alloc)
{
    sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> bmat(alloc);
    auto sc = dir::static_lu_prepare<crd::f64>(a, bmat, alloc);
    (void)sc;
    auto sym = dir::lu_symbolic(bmat.pattern(), alloc);
    auto mf = dir::build_multifrontal_symbolic(sym, alloc);
    return dir::check_multifrontal_containment(mf);
}

// Build the v5b-3 SYMMETRIC-pattern (MUMPS-style) multifrontal symbolic from A and run the containment
// check — the fronts come from chol(B+Bᵀ), so containment must HOLD (the Cholesky theorem).
dir::MfContainmentReport sym_containment_of(const sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>& a,
                                            crd::memory::IAllocator* alloc)
{
    sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> bmat(alloc);
    auto sc = dir::static_lu_prepare<crd::f64>(a, bmat, alloc);
    (void)sc;
    auto mf = dir::build_symmetric_multifrontal_symbolic(bmat.pattern(), alloc);
    return dir::check_multifrontal_containment(mf);
}

// Supernode panel-density invariant: within each supernode [a,b), the leading column a's
// sub-diagonal pattern below row b is a SUPERSET of every member column c's — i.e. the
// supernode is a dense trapezoid (the prerequisite for the v5b-2c BLAS-3 panel).
bool supernode_panels_dense(const dir::LuSymbolic& sym, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> mark(alloc);
    mark.resize(sym.n);
    bool ok = true;
    for (crd::u32 g = 0; g < sym.nsuper; ++g)
    {
        const crd::u32 a = sym.super[g];
        const crd::u32 b = sym.super[g + 1];
        for (crd::u32 p = sym.lp[a]; p < sym.lp[a + 1]; ++p)
        {
            if (sym.li[p] >= b)
            {
                mark[sym.li[p]] = 1; // leading column's rows below the supernode
            }
        }
        for (crd::u32 c = a; c < b; ++c)
        {
            for (crd::u32 p = sym.lp[c]; p < sym.lp[c + 1]; ++p)
            {
                if (sym.li[p] >= b && mark[sym.li[p]] == 0)
                {
                    ok = false; // a member column has a row the leading column lacks
                }
            }
        }
        for (crd::u32 p = sym.lp[a]; p < sym.lp[a + 1]; ++p)
        {
            if (sym.li[p] >= b)
            {
                mark[sym.li[p]] = 0;
            }
        }
    }
    return ok;
}

// Per-column ascending + L unit-diagonal-first + U diagonal-last canonical-order check.
bool canonical_order(const dir::LuSymbolic& sym)
{
    for (crd::u32 k = 0; k < sym.n; ++k)
    {
        if (sym.lp[k + 1] <= sym.lp[k] || sym.li[sym.lp[k]] != k) // L(k,k) first
        {
            return false;
        }
        for (crd::u32 p = sym.lp[k] + 1; p < sym.lp[k + 1]; ++p)
        {
            if (sym.li[p] <= sym.li[p - 1] || sym.li[p] <= k) // ascending + strictly sub-diagonal
            {
                return false;
            }
        }
        if (sym.up[k + 1] <= sym.up[k] || sym.ui[sym.up[k + 1] - 1] != k) // U(k,k) last
        {
            return false;
        }
        for (crd::u32 p = sym.up[k] + 1; p < sym.up[k + 1]; ++p)
        {
            if (sym.ui[p] <= sym.ui[p - 1]) // ascending (off-diags < k, diagonal last)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

TEST_CASE("v5b-2b lu_symbolic: structure is a superset of the GP-LU(static) fill", "[lu][v5b-2b]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (crd::u32 n : {8U, 20U, 50U, 120U})
    {
        auto a = dd_unsymmetric(&alloc, n);
        crd::u64 sym_nnz = 0;
        crd::u64 gp_nnz = 0;
        crd::u64 bound = 0;
        const bool superset = symbolic_superset_of_gp<crd::f64>(a, &alloc, sym_nnz, gp_nnz, bound);
        CHECK(superset);
        CHECK(sym_nnz >= gp_nnz);    // relaxed amalgamation pads L with explicit zeros ⇒ superset
        CHECK(sym_nnz <= 2 * bound); // Gilbert-Ng sandwich: nnz(L)+nnz(U) ≤ 2·chol(BᵀB) bound
        CHECK(bound > 0);
    }
}

TEST_CASE("v5b-2b lu_symbolic: irregular wide-row matrix superset + sandwich", "[lu][v5b-2b]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 30;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 r = 0; r < n; ++r)
    {
        tb.add(r, r, static_cast<crd::f64>(8 * n));
        tb.add(r, (r * 7U + 3U) % n, 1.0);
        tb.add(r, (r * 11U + 1U) % n, -0.5);
    }
    auto a = tb.compress();
    crd::u64 sym_nnz = 0;
    crd::u64 gp_nnz = 0;
    crd::u64 bound = 0;
    const bool superset = symbolic_superset_of_gp<crd::f64>(a, &alloc, sym_nnz, gp_nnz, bound);
    CHECK(superset);
    CHECK(sym_nnz >= gp_nnz);
    CHECK(sym_nnz <= 2 * bound);
}

TEST_CASE("v5b-2b lu_symbolic: complex superset", "[lu][v5b-2b][complex]")
{
    using C = crd::hesap::Complex64;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 24;
    sp::TripletBuilder<C> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, C{static_cast<crd::f64>(4 * n + 10), 1.0});
        if (i > 0)
        {
            tb.add(i, i - 1, C{-1.0, 0.2});
        }
        if (i + 2 < n)
        {
            tb.add(i, i + 2, C{0.5, -0.3});
        }
    }
    auto a = tb.compress();
    crd::u64 sym_nnz = 0;
    crd::u64 gp_nnz = 0;
    crd::u64 bound = 0;
    const bool superset = symbolic_superset_of_gp<C>(a, &alloc, sym_nnz, gp_nnz, bound);
    CHECK(superset);
    CHECK(sym_nnz >= gp_nnz);
}

// =====================================================================================
// v5b-2c — SupernodalLU NUMERIC (serial, static-pivot, GESP + iterative refinement). The
// gate is the residual ‖A·x − b‖ / ‖b‖ through the FULL chain (MC64 → symbolic → numeric →
// solve → IR → untransform), validated end-to-end against the known solution. MC64 makes B
// diagonally dominant so the static diagonal pivot is accurate; IR drives the true residual
// to machine precision (the matched residual the v5b-2e bench compares at). Run-to-run
// determinism is the v5b-2c claim; the {1,2,4,8}-worker moat is v5b-2d.
// =====================================================================================
namespace
{
template <typename T>
double solve_supernodal(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, const T* b, T* x,
                        crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.pattern().rows;
    auto lu = dir::factor_supernodal_lu<T>(a, alloc);
    if (lu.info() != 0)
    {
        return -1.0;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    if (!lu.solve({x, n}))
    {
        return -1.0;
    }
    return 0.0;
}
} // namespace

TEST_CASE("v5b-2c SupernodalLU: unsymmetric DD residual vs known solution", "[lu][v5b-2c]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    for (crd::u32 n : {8U, 20U, 50U, 120U})
    {
        auto a = dd_unsymmetric(&alloc, n);
        crd::containers::Array<crd::f64> xtrue(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        crd::containers::Array<crd::f64> x(&alloc);
        xtrue.resize(n);
        b.resize(n);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
        }
        csr_matvec<crd::f64>(a, xtrue.data(), b.data());
        const double ok = solve_supernodal<crd::f64>(a, b.data(), x.data(), &alloc);
        REQUIRE(ok == 0.0);
        CHECK(max_err(x.data(), xtrue.data(), n) < 1e-10);
    }
}

TEST_CASE("v5b-2c SupernodalLU: MC64 rescues tiny-diagonal + IR (static-pivot enabler)", "[lu][v5b-2c]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // The v5b-2a tiny-diagonal case: a naive diagonal pivot hits ~0; MC64 swaps the large
    // anti-diagonal onto the diagonal ⇒ a STATIC pivot is safe + IR cleans up.
    const crd::u32 n = 4;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    tb.add(0, 0, 1e-9);
    tb.add(0, 1, 5.0);
    tb.add(1, 0, 4.0);
    tb.add(1, 1, 1e-9);
    tb.add(2, 2, 1e-9);
    tb.add(2, 3, 3.0);
    tb.add(3, 2, 2.0);
    tb.add(3, 3, 1e-9);
    auto a = tb.compress();
    const crd::f64 xtrue[4] = {1.0, -2.0, 3.0, -1.0};
    crd::f64 b[4];
    csr_matvec<crd::f64>(a, xtrue, b);
    crd::f64 x[4];
    const double ok = solve_supernodal<crd::f64>(a, b, x, &alloc);
    REQUIRE(ok == 0.0);
    CHECK(max_err(x, xtrue, 4) < 1e-9);
}

TEST_CASE("v5b-2c SupernodalLU: badly-scaled matrix (MC64 balances) residual", "[lu][v5b-2c]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 24;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 s = std::pow(10.0, static_cast<crd::f64>(static_cast<int>(i % 6) - 3));
        tb.add(i, i, s * static_cast<crd::f64>(n + 2));
        if (i + 1 < n)
            tb.add(i, i + 1, s * 1.0);
        if (i > 0)
            tb.add(i, i - 1, s * -2.0);
    }
    auto a = tb.compress();
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 - 0.03 * static_cast<crd::f64>(i);
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());
    const double ok = solve_supernodal<crd::f64>(a, b.data(), x.data(), &alloc);
    REQUIRE(ok == 0.0);
    CHECK(max_err(x.data(), xtrue.data(), n) < 1e-9);
}

TEST_CASE("v5b-2c SupernodalLU: complex non-Hermitian residual", "[lu][v5b-2c][complex]")
{
    using C = crd::hesap::Complex64;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 22;
    sp::TripletBuilder<C> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, C{static_cast<crd::f64>(4 * n + 10), 1.0});
        if (i > 0)
            tb.add(i, i - 1, C{-1.0, 0.2});
        if (i + 2 < n)
            tb.add(i, i + 2, C{0.5, -0.3});
    }
    auto a = tb.compress();
    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> b(&alloc);
    crd::containers::Array<C> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.1 * static_cast<crd::f64>(i), 0.2 * static_cast<crd::f64>(i)};
    }
    csr_matvec<C>(a, xtrue.data(), b.data());
    const double ok = solve_supernodal<C>(a, b.data(), x.data(), &alloc);
    REQUIRE(ok == 0.0);
    CHECK(max_err<C>(x.data(), xtrue.data(), n) < 1e-9);
}

TEST_CASE("v5b-2c SupernodalLU: multi-RHS + run-to-run determinism", "[lu][v5b-2c]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 80;
    const crd::usize nrhs = 4;
    auto a = dd_unsymmetric(&alloc, n);
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> rhs(&alloc);
    xtrue.resize(static_cast<crd::usize>(n) * nrhs);
    rhs.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::u32 c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[c * n + i] = 1.0 + 0.1 * static_cast<crd::f64>(i) - 0.5 * static_cast<crd::f64>(c);
        }
        csr_matvec<crd::f64>(a, xtrue.data() + c * n, rhs.data() + c * n);
    }
    auto lu = dir::factor_supernodal_lu<crd::f64>(a, &alloc);
    REQUIRE(lu.info() == 0);
    crd::containers::Array<crd::f64> x1(&alloc);
    x1.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::u32 i = 0; i < n * nrhs; ++i)
    {
        x1[i] = rhs[i];
    }
    REQUIRE(lu.solve({x1.data(), static_cast<crd::usize>(n) * nrhs}, nrhs));
    CHECK(max_err(x1.data(), xtrue.data(), n * static_cast<crd::u32>(nrhs)) < 1e-10);

    auto lu2 = dir::factor_supernodal_lu<crd::f64>(a, &alloc);
    crd::containers::Array<crd::f64> x2(&alloc);
    x2.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::u32 i = 0; i < n * nrhs; ++i)
    {
        x2[i] = rhs[i];
    }
    REQUIRE(lu2.solve({x2.data(), static_cast<crd::usize>(n) * nrhs}, nrhs));
    bool identical = (lu.l_nnz() == lu2.l_nnz()) && (lu.u_nnz() == lu2.u_nnz());
    for (crd::u32 i = 0; identical && i < n * nrhs; ++i)
    {
        identical = (x1[i] == x2[i]);
    }
    CHECK(identical);
}

// =====================================================================================
// v5b-2d — TREE-PARALLEL factor + the CROSS-THREAD DETERMINISM MOAT. The supernodal numeric is
// scheduled by etree level (same-level supernodes independent) over crd::jobs; static pivoting +
// the fixed structure + per-worker scratch make the factor a deterministic pure function ⇒ the L
// and U values are BIT-IDENTICAL across {1,2,4,8} workers — the moat no gold-standard LU peer
// (Eigen/UMFPACK/SuperLU) carries. Capped at 8 workers (i9-14900K host).
// =====================================================================================
TEST_CASE("v5b-2d SupernodalLU: tree-parallel factor bit-identical across {1,2,4,8} workers", "[lu][v5b-2d]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 26);
        const crd::u32 n = 220;
        auto a = dd_unsymmetric(&alloc, n);
        auto lu1 = dir::factor_supernodal_lu<crd::f64>(a, &alloc, 1); // serial reference
        REQUIRE(lu1.info() == 0);
        REQUIRE(lu1.supernode_count() >= 2);

        // RHS for a solve cross-check.
        crd::containers::Array<crd::f64> xtrue(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xtrue.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[i] = 1.0 + 0.07 * static_cast<crd::f64>(i);
        }
        csr_matvec<crd::f64>(a, xtrue.data(), b.data());

        const auto l1 = lu1.l_values();
        const auto u1 = lu1.u_values();
        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto lup = dir::factor_supernodal_lu<crd::f64>(a, &alloc, nw);
            REQUIRE(lup.info() == 0);
            REQUIRE(lup.l_nnz() == lu1.l_nnz());
            REQUIRE(lup.u_nnz() == lu1.u_nnz());
            const auto lp = lup.l_values();
            const auto up = lup.u_values();
            bool ident = (l1.size() == lp.size()) && (u1.size() == up.size());
            for (crd::usize p = 0; ident && p < l1.size(); ++p)
            {
                ident = (l1[p] == lp[p]); // BIT-exact L across worker counts
            }
            for (crd::usize p = 0; ident && p < u1.size(); ++p)
            {
                ident = (u1[p] == up[p]); // BIT-exact U
            }
            CHECK(ident); // the determinism moat

            crd::containers::Array<crd::f64> x(&alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            REQUIRE(lup.solve({x.data(), n}));
            CHECK(max_err(x.data(), xtrue.data(), n) < 1e-10); // parallel-built factor solves correctly
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("v5b-2b lu_symbolic: canonical order + supernode panel density + determinism", "[lu][v5b-2b]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 90;
    auto a = dd_unsymmetric(&alloc, n);
    sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc> bmat(&alloc);
    auto sc = dir::static_lu_prepare<crd::f64>(a, bmat, &alloc);
    (void)sc;
    auto s1 = dir::lu_symbolic(bmat.pattern(), &alloc);
    CHECK(canonical_order(s1));
    CHECK(supernode_panels_dense(s1, &alloc));
    CHECK(s1.nsuper >= 1);
    CHECK(s1.super[s1.nsuper] == n); // partition covers all columns

    // Determinism: lu_symbolic is a pure function of B's pattern (bit-identical re-run).
    auto s2 = dir::lu_symbolic(bmat.pattern(), &alloc);
    REQUIRE(s1.lnz == s2.lnz);
    REQUIRE(s1.unz == s2.unz);
    REQUIRE(s1.nsuper == s2.nsuper);
    bool identical = (s1.li.size() == s2.li.size()) && (s1.ui.size() == s2.ui.size());
    for (crd::u32 p = 0; identical && p < s1.li.size(); ++p)
    {
        identical = (s1.li[p] == s2.li[p]);
    }
    for (crd::u32 p = 0; identical && p < s1.ui.size(); ++p)
    {
        identical = (s1.ui[p] == s2.ui[p]);
    }
    for (crd::u32 g = 0; identical && g <= s1.nsuper; ++g)
    {
        identical = (s1.super[g] == s2.super[g]);
    }
    CHECK(identical);
}

// v5b-3a — THE GATING CHECK (the decisive design experiment). Does every child front's contribution
// block fit inside its parent front in BOTH dimensions (the extend_add subset contract)?
//
// FINDING (2026-06-01): for SYMMETRIC patterns it HOLDS (the Cholesky theorem); for genuinely
// UNSYMMETRIC patterns it FAILS — a contribution block's rows/cols can belong to an ANCESTOR, not the
// direct parent (precisely why UMFPACK splits assembly into LUson/Lson/Uson). ⇒ the naive
// "child CB → direct parent extend_add" (the v5a-1 Cholesky model) does NOT transfer to unsymmetric LU
// over the LU column etree. v5b-3 therefore adopts SYMMETRIC-PATTERN (MUMPS-style) fronts: build the
// assembly tree from the structure of B+Bᵀ (the proven v5a symmetric multifrontal substrate), do the
// unsymmetric dense-front BLAS-3 numeric within — containment then holds by the Cholesky theorem, and
// for near-structurally-symmetric CFD/FEM (the sim targets) the symmetrized fill is small.
// See docs/research/cerid-hesap-v5b-3-multifrontal-lu.md.
TEST_CASE("v5b-3a multifrontal containment: symmetric holds, unsymmetric needs symmetric-pattern fronts",
          "[lu][v5b-3a]")
{
    crd::memory::TlsfAllocator alloc(64ULL * 1024 * 1024);

    // POSITIVE CONTROL: symmetric pattern ⇒ containment HOLDS (validates the check; the Cholesky case).
    SECTION("symmetric_dd → containment holds")
    {
        for (crd::u32 n : {crd::u32{30}, crd::u32{120}, crd::u32{400}})
        {
            auto a = symmetric_dd(&alloc, n);
            auto rep = containment_of(a, &alloc);
            CAPTURE(n, rep.nfront, rep.nchild, rep.row_violations, rep.col_violations);
            CHECK(rep.ok());
        }
    }

    // FINDING: genuinely-unsymmetric patterns VIOLATE containment ⇒ unsymmetric-pattern fronts are not
    // assembly-safe (the documented reason v5b-3 uses symmetric-pattern fronts). When symmetric-pattern
    // fronts land, the positive-control assertion above extends to these and this section is removed.
    SECTION("divergent_unsymmetric → containment violated (unsymmetric-pattern fronts unsafe)")
    {
        auto a = divergent_unsymmetric(&alloc, 150);
        auto rep = containment_of(a, &alloc);
        CAPTURE(rep.nfront, rep.nchild, rep.row_violations, rep.col_violations);
        CHECK_FALSE(rep.ok()); // unsymmetric-pattern fronts break the single-parent extend_add contract
    }

    // THE v5b-3 PATH: SYMMETRIC-PATTERN (MUMPS-style) fronts (from chol(B+Bᵀ)) make containment HOLD on
    // the SAME genuinely-unsymmetric matrices that violated it above ⇒ child→parent extend_add is valid
    // ⇒ the proven v5a symmetric multifrontal assembly + determinism moat transfer to unsymmetric LU.
    SECTION("symmetric-pattern fronts → containment HOLDS on unsymmetric matrices (the v5b-3 foundation)")
    {
        for (crd::u32 n : {crd::u32{40}, crd::u32{150}, crd::u32{500}})
        {
            auto rd = sym_containment_of(divergent_unsymmetric(&alloc, n), &alloc);
            CAPTURE(n, rd.nfront, rd.nchild, rd.row_violations, rd.col_violations);
            CHECK(rd.ok());
            auto rdd = sym_containment_of(dd_unsymmetric(&alloc, n), &alloc);
            CAPTURE(n, rdd.nfront, rdd.nchild, rdd.row_violations, rdd.col_violations);
            CHECK(rdd.ok());
        }
    }
}

// v5b-3b-1 — the COL-MAJOR multifrontal front + its assembly kernel (the locked layout). Validate the
// col-major `mf_extend_add` against the PROVEN row-major `Frontal::extend_add`: the same logical assembly
// in two layouts must produce bit-identical parent entries (the assembly substrate for the v5b-3b numeric).
TEST_CASE("v5b-3b MfFront col-major extend_add matches the proven row-major Frontal extend_add", "[lu][v5b-3b]")
{
    crd::memory::TlsfAllocator alloc(8ULL * 1024 * 1024);

    // Parent front rows {0,1,2,4,5,7}, cols {0,2,3,4,7}; child CB rows {1,4,7}, cols {2,4} (⊆ parent,
    // both ascending — the extend_add subset contract). Fill the child with distinct values, assemble
    // into a zeroed parent via BOTH kernels; every logical parent entry at(i,j) must match.
    const crd::u32 prow[] = {0, 1, 2, 4, 5, 7};
    const crd::u32 pcol[] = {0, 2, 3, 4, 7};
    const crd::u32 crow[] = {1, 4, 7};
    const crd::u32 ccol[] = {2, 4};
    const crd::u32 pnr = 6;
    const crd::u32 pnc = 5;
    const crd::u32 cnr = 3;
    const crd::u32 cnc = 2;

    dir::MfFront<crd::f64> mp(&alloc);
    dir::MfFront<crd::f64> mc(&alloc);
    dir::Frontal<crd::f64> rp(&alloc);
    dir::Frontal<crd::f64> rc(&alloc);
    mp.resize(pnr, pnc);
    mc.resize(cnr, cnc);
    rp.resize(pnr, pnc);
    rc.resize(cnr, cnc);
    mp.zero_fill();
    rp.zero_fill();
    for (crd::u32 i = 0; i < pnr; ++i)
    {
        mp.row_index[i] = prow[i];
        rp.row_index[i] = prow[i];
    }
    for (crd::u32 j = 0; j < pnc; ++j)
    {
        mp.col_index[j] = pcol[j];
        rp.col_index[j] = pcol[j];
    }
    for (crd::u32 i = 0; i < cnr; ++i)
    {
        mc.row_index[i] = crow[i];
        rc.row_index[i] = crow[i];
    }
    for (crd::u32 j = 0; j < cnc; ++j)
    {
        mc.col_index[j] = ccol[j];
        rc.col_index[j] = ccol[j];
    }
    for (crd::u32 i = 0; i < cnr; ++i)
    {
        for (crd::u32 j = 0; j < cnc; ++j)
        {
            const crd::f64 v = 1.0 + static_cast<crd::f64>(i) * 10.0 + static_cast<crd::f64>(j);
            mc.at(i, j) = v;
            rc.at(i, j) = v;
        }
    }

    dir::mf_extend_add(mp, mc, &alloc);
    dir::extend_add(rp, rc, &alloc);

    bool match = true;
    for (crd::u32 i = 0; i < pnr; ++i)
    {
        for (crd::u32 j = 0; j < pnc; ++j)
        {
            if (mp.at(i, j) != rp.at(i, j))
            {
                match = false;
            }
        }
    }
    CHECK(match);
    // Spot checks: child(0,0)=1.0 → global (row1,col2) → parent local (1,1); child(2,1)=22.0 → (row7,col4) → (5,3).
    CHECK(mp.at(1, 1) == 1.0);
    CHECK(mp.at(5, 3) == 22.0);
    CHECK(mp.at(0, 0) == 0.0); // untouched (global row0/col0 not in the child)
}

// =====================================================================================
// v5b-3b-2 — factor_front (the dense-front BLAS-3 crush kernel) standalone vs the tight
// reconstruction oracle. Build a diagonally-dominant m×n COL-MAJOR front (so the static
// pivot never perturbs ⇒ EXACT reconstruction), factor `npiv` pivots, and verify the block
// identity A = [L11·U11, L11·U12; L21·U11, L21·U12 + Schur] == the saved original front. Any
// indexing / Schur-boundary / TRSM bug shows up loudly. Type-generic (f64 + Complex64).
// =====================================================================================
namespace
{
template <typename T>
void check_factor_front(crd::memory::IAllocator* alloc, crd::u32 m, crd::u32 n, crd::u32 npiv)
{
    namespace dl = crd::hesap::dense;
    crd::containers::Array<T> f0(alloc); // original front (col-major)
    crd::containers::Array<T> f(alloc);  // working copy factored in place
    f0.resize(static_cast<crd::usize>(m) * n);
    f.resize(static_cast<crd::usize>(m) * n);
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 i = 0; i < m; ++i)
        {
            const double re = 0.3 * std::sin(0.7 * i + 1.3 * j + 0.2) + 0.2 * std::cos(1.1 * i - 0.4 * j);
            T v;
            if constexpr (dl::is_complex_v<T>)
            {
                v = T{re, 0.1 * std::sin(0.5 * i + 0.9 * j)};
            }
            else
            {
                v = static_cast<T>(re);
            }
            if (i == j) // dominant diagonal ⇒ pivots stay O(50), the static pivot never perturbs
            {
                v = v + dir::lu2_from_real<T>(static_cast<dl::RealType<T>>(50 + i));
            }
            f0[static_cast<crd::usize>(j) * m + i] = v;
            f[static_cast<crd::usize>(j) * m + i] = v;
        }
    }
    dir::factor_front<T>(f.data(), m, m, n, npiv, dl::RealType<T>(0), alloc); // tiny=0 ⇒ no perturbation

    double maxd = 0.0;
    double maxa = 0.0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 i = 0; i < m; ++i)
        {
            T acc{}; // zero (real or complex)
            for (crd::u32 p = 0; p < npiv; ++p)
            {
                T lip = T{};
                if (i == p)
                {
                    lip = dir::lu2_from_real<T>(dl::RealType<T>(1));
                }
                else if (i > p)
                {
                    lip = f[static_cast<crd::usize>(p) * m + i];
                }
                const T upj = (j >= p) ? f[static_cast<crd::usize>(j) * m + p] : T{};
                acc = acc + lip * upj;
            }
            if (i >= npiv && j >= npiv) // + Schur complement in the trailing block
            {
                acc = acc + f[static_cast<crd::usize>(j) * m + i];
            }
            const double d = static_cast<double>(dir::lu2_mag<T>(acc - f0[static_cast<crd::usize>(j) * m + i]));
            const double a = static_cast<double>(dir::lu2_mag<T>(f0[static_cast<crd::usize>(j) * m + i]));
            maxd = d > maxd ? d : maxd;
            maxa = a > maxa ? a : maxa;
        }
    }
    CAPTURE(m, n, npiv, maxd, maxa);
    CHECK(maxd <= 1e-10 * (maxa > 0.0 ? maxa : 1.0));
}
} // namespace

TEST_CASE("v5b-3b-2 factor_front: blocked partial-LU reconstructs the dense front (real + complex)", "[lu][v5b-3b]")
{
    crd::memory::TlsfAllocator alloc(8ULL * 1024 * 1024);
    // Square fronts (the multifrontal usage), partial + full; one wider than the panel block (48) to
    // exercise the blocked rank-nb TRSM+GEMM path; npiv < n (Schur present) + npiv == n (full LU).
    for (auto mnp : {std::array<crd::u32, 3>{6, 6, 4}, std::array<crd::u32, 3>{8, 8, 8},
                     std::array<crd::u32, 3>{5, 5, 2}, std::array<crd::u32, 3>{60, 60, 40},
                     std::array<crd::u32, 3>{64, 64, 64}})
    {
        check_factor_front<crd::f64>(&alloc, mnp[0], mnp[1], mnp[2]);
        check_factor_front<crd::hesap::Complex64>(&alloc, mnp[0], mnp[1], mnp[2]);
    }
}

// =====================================================================================
// v5b-3b-3 — MultifrontalLU<T> driver: residual on the ORIGINAL A (the end-to-end bar; a wrong
// CSC L/U store, a misordered U-diagonal, or a broken assembly fails the residual loudly), the
// fill ≈ the symmetric-pattern fill (vs SupernodalLU's unsymmetric-pattern fill), and run-to-run
// determinism (the numeric is a pure function of the symbolic ⇒ the v5b-3c {1,2,4,8} moat seed).
// =====================================================================================
namespace
{
template <typename T>
double solve_multifrontal(const sp::SparseMatrix<T, sp::SparseFormat::Csr>& a, const T* b, T* x,
                          crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.pattern().rows;
    auto lu = dir::factor_multifrontal_lu<T>(a, alloc);
    if (lu.info() != 0)
    {
        return -1.0;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    if (!lu.solve({x, n}))
    {
        return -1.0;
    }
    return 0.0;
}
} // namespace

TEST_CASE("v5b-3b-3 MultifrontalLU: residual vs known solution (DD + MC64-rescue + badly-scaled)", "[lu][v5b-3b]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    SECTION("diagonally-dominant unsymmetric, several sizes")
    {
        for (crd::u32 n : {8U, 20U, 50U, 120U})
        {
            auto a = dd_unsymmetric(&alloc, n);
            crd::containers::Array<crd::f64> xtrue(&alloc);
            crd::containers::Array<crd::f64> b(&alloc);
            crd::containers::Array<crd::f64> x(&alloc);
            xtrue.resize(n);
            b.resize(n);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                xtrue[i] = 1.0 + 0.1 * static_cast<crd::f64>(i);
            }
            csr_matvec<crd::f64>(a, xtrue.data(), b.data());
            const double ok = solve_multifrontal<crd::f64>(a, b.data(), x.data(), &alloc);
            REQUIRE(ok == 0.0);
            CHECK(max_err(x.data(), xtrue.data(), n) < 1e-10);
        }
    }
    SECTION("MC64 rescues a tiny-diagonal matrix (static-pivot enabler)")
    {
        const crd::u32 n = 4;
        sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
        tb.add(0, 0, 1e-9);
        tb.add(0, 1, 5.0);
        tb.add(1, 0, 4.0);
        tb.add(1, 1, 1e-9);
        tb.add(2, 2, 1e-9);
        tb.add(2, 3, 3.0);
        tb.add(3, 2, 2.0);
        tb.add(3, 3, 1e-9);
        auto a = tb.compress();
        const crd::f64 xtrue[4] = {1.0, -2.0, 3.0, -1.0};
        crd::f64 b[4];
        csr_matvec<crd::f64>(a, xtrue, b);
        crd::f64 x[4];
        const double ok = solve_multifrontal<crd::f64>(a, b, x, &alloc);
        REQUIRE(ok == 0.0);
        CHECK(max_err(x, xtrue, 4) < 1e-9);
    }
    SECTION("badly-scaled matrix (MC64 balances)")
    {
        const crd::u32 n = 24;
        sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const crd::f64 s = std::pow(10.0, static_cast<crd::f64>(static_cast<int>(i % 6) - 3));
            tb.add(i, i, s * static_cast<crd::f64>(n + 2));
            if (i + 1 < n)
                tb.add(i, i + 1, s * 1.0);
            if (i > 0)
                tb.add(i, i - 1, s * -2.0);
        }
        auto a = tb.compress();
        crd::containers::Array<crd::f64> xtrue(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        crd::containers::Array<crd::f64> x(&alloc);
        xtrue.resize(n);
        b.resize(n);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[i] = 1.0 - 0.03 * static_cast<crd::f64>(i);
        }
        csr_matvec<crd::f64>(a, xtrue.data(), b.data());
        const double ok = solve_multifrontal<crd::f64>(a, b.data(), x.data(), &alloc);
        REQUIRE(ok == 0.0);
        CHECK(max_err(x.data(), xtrue.data(), n) < 1e-9);
    }
}

TEST_CASE("v5b-3b-3 MultifrontalLU: complex non-Hermitian residual", "[lu][v5b-3b][complex]")
{
    using C = crd::hesap::Complex64;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 22;
    sp::TripletBuilder<C> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, C{static_cast<crd::f64>(4 * n + 10), 1.0});
        if (i > 0)
            tb.add(i, i - 1, C{-1.0, 0.2});
        if (i + 2 < n)
            tb.add(i, i + 2, C{0.5, -0.3});
    }
    auto a = tb.compress();
    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> b(&alloc);
    crd::containers::Array<C> x(&alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.1 * static_cast<crd::f64>(i), 0.2 * static_cast<crd::f64>(i)};
    }
    csr_matvec<C>(a, xtrue.data(), b.data());
    const double ok = solve_multifrontal<C>(a, b.data(), x.data(), &alloc);
    REQUIRE(ok == 0.0);
    CHECK(max_err<C>(x.data(), xtrue.data(), n) < 1e-9);
}

TEST_CASE("v5b-3b-3 MultifrontalLU: matches SupernodalLU residual + run-to-run determinism", "[lu][v5b-3b]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const crd::u32 n = 80;
    auto a = dd_unsymmetric(&alloc, n);
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.07 * static_cast<crd::f64>(i);
    }
    csr_matvec<crd::f64>(a, xtrue.data(), b.data());

    // Multifrontal and supernodal both solve the SAME system to the SAME matched residual.
    crd::containers::Array<crd::f64> xm(&alloc);
    crd::containers::Array<crd::f64> xs(&alloc);
    xm.resize(n);
    xs.resize(n);
    REQUIRE(solve_multifrontal<crd::f64>(a, b.data(), xm.data(), &alloc) == 0.0);
    REQUIRE(solve_supernodal<crd::f64>(a, b.data(), xs.data(), &alloc) == 0.0);
    CHECK(max_err(xm.data(), xtrue.data(), n) < 1e-10);
    CHECK(max_err(xs.data(), xtrue.data(), n) < 1e-10);

    // Fill sanity: the multifrontal symmetric-pattern factor is in the same ballpark as the supernodal
    // unsymmetric-pattern one (measured ~free to symmetrize on the sim targets; here both are small).
    auto mf = dir::factor_multifrontal_lu<crd::f64>(a, &alloc);
    auto sn = dir::factor_supernodal_lu<crd::f64>(a, &alloc);
    REQUIRE(mf.info() == 0);
    REQUIRE(sn.info() == 0);
    CHECK(mf.factor_nnz() > 0);
    CAPTURE(mf.factor_nnz(), sn.factor_nnz(), mf.front_count());
    CHECK(mf.factor_nnz() <= 4 * sn.factor_nnz());

    // Run-to-run determinism: the numeric is a pure function of the (fixed) symbolic ⇒ bit-identical
    // L,U on a re-factor (the seed of the v5b-3c cross-thread {1,2,4,8} moat).
    auto mf2 = dir::factor_multifrontal_lu<crd::f64>(a, &alloc);
    const auto l1 = mf.l_values();
    const auto l2 = mf2.l_values();
    const auto u1 = mf.u_values();
    const auto u2 = mf2.u_values();
    REQUIRE(l1.size() == l2.size());
    REQUIRE(u1.size() == u2.size());
    crd::usize lmis = 0;
    crd::usize umis = 0;
    double lmaxd = 0.0;
    double umaxd = 0.0;
    for (crd::usize i = 0; i < l1.size(); ++i)
    {
        if (l1[i] != l2[i])
        {
            ++lmis;
            const double d = std::abs(l1[i] - l2[i]);
            lmaxd = d > lmaxd ? d : lmaxd;
        }
    }
    for (crd::usize i = 0; i < u1.size(); ++i)
    {
        if (u1[i] != u2[i])
        {
            ++umis;
            const double d = std::abs(u1[i] - u2[i]);
            umaxd = d > umaxd ? d : umaxd;
        }
    }
    CAPTURE(l1.size(), u1.size(), lmis, umis, lmaxd, umaxd);
    CHECK(lmis == 0);
    CHECK(umis == 0);
}

// v5b-3c — the determinism MOAT for the tree-parallel multifrontal front walk: L,U bit-identical across
// {1,2,4,8} workers AND equal to the serial reference. The numeric is a deterministic pure function of the
// symbolic (per-worker scratch, fixed-postorder child assembly, fixed uoff/m_lp write positions) ⇒ no race
// + no worker-order dependence. This is what UMFPACK/PARDISO/MUMPS cannot offer.
TEST_CASE("v5b-3c MultifrontalLU: tree-parallel factor bit-identical across {1,2,4,8} workers", "[lu][v5b-3c]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 27); // parallel keeps more front buffers + per-worker arenas live
        const crd::u32 n = 300;
        auto a = dd_unsymmetric(&alloc, n);
        auto lu1 = dir::factor_multifrontal_lu<crd::f64>(a, &alloc, 1); // serial reference
        REQUIRE(lu1.info() == 0);
        REQUIRE(lu1.front_count() >= 2); // a non-trivial assembly tree (otherwise parallelism is untested)

        crd::containers::Array<crd::f64> xtrue(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xtrue.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[i] = 1.0 + 0.07 * static_cast<crd::f64>(i);
        }
        csr_matvec<crd::f64>(a, xtrue.data(), b.data());

        const auto l1 = lu1.l_values();
        const auto u1 = lu1.u_values();
        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto lup = dir::factor_multifrontal_lu<crd::f64>(a, &alloc, nw);
            REQUIRE(lup.info() == 0);
            REQUIRE(lup.l_nnz() == lu1.l_nnz());
            REQUIRE(lup.u_nnz() == lu1.u_nnz());
            const auto lp = lup.l_values();
            const auto up = lup.u_values();
            bool ident = (l1.size() == lp.size()) && (u1.size() == up.size());
            for (crd::usize p = 0; ident && p < l1.size(); ++p)
            {
                ident = (l1[p] == lp[p]); // BIT-exact L across worker counts AND vs serial
            }
            for (crd::usize p = 0; ident && p < u1.size(); ++p)
            {
                ident = (u1[p] == up[p]); // BIT-exact U
            }
            CHECK(ident); // the determinism moat

            crd::containers::Array<crd::f64> x(&alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            REQUIRE(lup.solve({x.data(), n}));
            CHECK(max_err(x.data(), xtrue.data(), n) < 1e-10); // the parallel-built factor solves correctly
        }

        // DENSE-FRONT moat: a fully dense diagonally-dominant matrix gives ONE big front, so the parallel
        // walk factors it via the WITHIN-FRONT path (gemm_parallel_auto splits the Schur GEMM across workers,
        // AND the parallel TRSM splits the panel's U block-row — for nd ≥ panel+256 ⇒ after the first 128-wide
        // panel the trailing ncol ≥ 256 trips trsm_stage's parallel branch). Verifies BOTH within-front
        // parallel paths are bit-exact vs serial — the moat on the big near-root fronts.
        const crd::u32 nd = 512;
        sp::TripletBuilder<crd::f64> tb(&alloc, nd, nd);
        for (crd::u32 i = 0; i < nd; ++i)
        {
            for (crd::u32 j = 0; j < nd; ++j)
            {
                tb.add(i, j, (i == j) ? static_cast<crd::f64>(8 * nd) : (0.3 + 0.001 * static_cast<crd::f64>(i + 2 * j)));
            }
        }
        auto ad = tb.compress();
        auto d1 = dir::factor_multifrontal_lu<crd::f64>(ad, &alloc, 1);
        REQUIRE(d1.info() == 0);
        REQUIRE(d1.front_count() == 1); // one dense front ⇒ exercises the within-front parallel GEMM
        const auto dl1 = d1.l_values();
        const auto du1 = d1.u_values();
        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto dp = dir::factor_multifrontal_lu<crd::f64>(ad, &alloc, nw);
            REQUIRE(dp.info() == 0);
            const auto dlp = dp.l_values();
            const auto dup = dp.u_values();
            bool ident = (dl1.size() == dlp.size()) && (du1.size() == dup.size());
            for (crd::usize p = 0; ident && p < dl1.size(); ++p)
            {
                ident = (dl1[p] == dlp[p]);
            }
            for (crd::usize p = 0; ident && p < du1.size(); ++p)
            {
                ident = (du1[p] == dup[p]);
            }
            CHECK(ident); // gemm_parallel_auto Schur update is bit-exact vs serial across {2,4,8} workers
        }

        // FALLBACK moat: a ~zero-diagonal cyclic matrix the NATURAL static pivot cannot factor (tiny diagonals
        // ⇒ element growth blows past the bound) ⇒ the adaptive path FALLS BACK to MC64. The growth/early-abort
        // decision is computed from a bit-identical factor + reduced by an order-independent max ⇒ it must fire
        // at the SAME point regardless of worker count, so the (MC64) result is L,U bit-identical {1,2,4,8}.
        const crd::u32 nfb = 96;
        sp::TripletBuilder<crd::f64> tfb(&alloc, nfb, nfb);
        for (crd::u32 i = 0; i < nfb; ++i)
        {
            tfb.add(i, (i + 1) % nfb, 2.0 + 0.01 * static_cast<crd::f64>(i)); // large cyclic off-diagonal
            tfb.add(i, (i + 7) % nfb, 0.5);                                   // extra band ⇒ a non-trivial tree
            tfb.add(i, i, 1e-12);                                             // ~zero diagonal ⇒ natural fails
        }
        auto afb = tfb.compress();
        auto fb1 = dir::factor_multifrontal_lu<crd::f64>(afb, &alloc, 1); // serial (also falls back to MC64)
        REQUIRE(fb1.info() == 0);
        crd::containers::Array<crd::f64> xtf(&alloc);
        crd::containers::Array<crd::f64> bf(&alloc);
        xtf.resize(nfb);
        bf.resize(nfb);
        for (crd::u32 i = 0; i < nfb; ++i)
        {
            xtf[i] = 1.0 - 0.05 * static_cast<crd::f64>(i);
        }
        csr_matvec<crd::f64>(afb, xtf.data(), bf.data());
        const auto fl1 = fb1.l_values();
        const auto fu1 = fb1.u_values();
        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto fbp = dir::factor_multifrontal_lu<crd::f64>(afb, &alloc, nw);
            REQUIRE(fbp.info() == 0);
            const auto flp = fbp.l_values();
            const auto fup = fbp.u_values();
            bool ident = (fl1.size() == flp.size()) && (fu1.size() == fup.size());
            for (crd::usize p = 0; ident && p < fl1.size(); ++p)
            {
                ident = (fl1[p] == flp[p]);
            }
            for (crd::usize p = 0; ident && p < fu1.size(); ++p)
            {
                ident = (fu1[p] == fup[p]);
            }
            CHECK(ident); // the MC64-fallback factor is bit-identical across workers (decision is W-independent)

            crd::containers::Array<crd::f64> xf(&alloc);
            xf.resize(nfb);
            for (crd::u32 i = 0; i < nfb; ++i)
            {
                xf[i] = bf[i];
            }
            REQUIRE(fbp.solve({xf.data(), nfb}));
            CHECK(max_err(xf.data(), xtf.data(), nfb) < 1e-8); // MC64 fallback solves the ~singular-diag system
        }
    }
    crd::jobs::shutdown();
}
