// crd-hesap-direct v5d-a — Multifrontal LDLᵀ SYMBOLIC structure tests.
//
// LDLᵀ (symmetric indefinite) reuses the v5b-3 symmetric multifrontal symbolic: the front tree is the
// chol(A) supernode tree, each front symmetric (row extent == col extent), and the assembly precondition
// (child contribution block ⊆ parent in both dims) is a Cholesky theorem. The per-front INDEFINITE
// numeric (Bunch-Kaufman 1×1/2×2) + solve land in v5d-b+. The symbolic is value-agnostic ⇒ identical for
// definite and indefinite A (the indefinite case is a v5d-b numeric concern, not structural).

#include <crd/hesap/direct/dense_ldlt_kernels.hpp> // factor_front_ldlt (v5d-b)
#include <crd/hesap/direct/multifrontal_ldlt.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp> // check_multifrontal_containment
#include <crd/hesap/sparse/convert.hpp>         // to_csc
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>

namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;

namespace
{
// Build a SYMMETRIC CSC pattern from a row-major n×n 0/1 mask (must be symmetric).
sp::SparsePattern sym_csc_from_mask(crd::u32 n, std::initializer_list<int> mask, crd::memory::IAllocator* alloc)
{
    REQUIRE(mask.size() == static_cast<crd::usize>(n) * n);
    const int* d = mask.begin();
    sp::SparsePattern p(alloc);
    p.rows = n;
    p.cols = n;
    p.format = sp::SparseFormat::Csc;
    p.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 j = 0; j < n; ++j)
    {
        crd::u32 cnt = 0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (d[static_cast<crd::usize>(i) * n + j] != 0)
            {
                ++cnt;
            }
        }
        p.outer_ptr[j + 1] = p.outer_ptr[j] + cnt;
    }
    p.inner_idx.resize(p.outer_ptr[n]);
    for (crd::u32 j = 0; j < n; ++j)
    {
        crd::u32 w = p.outer_ptr[j];
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (d[static_cast<crd::usize>(i) * n + j] != 0)
            {
                p.inner_idx[w++] = i;
            }
        }
    }
    p.recompute_topology_hash();
    return p;
}

// Validate the LDLᵀ multifrontal symbolic structure of a symmetric pattern A (n×n).
void validate_ldlt_symbolic(const sp::SparsePattern& a, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.cols;
    dir::MultifrontalSymbolic mf = dir::build_ldlt_symbolic(a, alloc);
    REQUIRE(mf.n == n);
    REQUIRE(mf.nfront >= 1);

    // Assembly precondition (Cholesky theorem): every child CB ⊆ its parent front in BOTH dimensions.
    const dir::MfContainmentReport rep = dir::check_multifrontal_containment(mf);
    CHECK(rep.ok());
    CHECK(rep.nfront == mf.nfront);

    // Pivots tile [0,n) exactly once, ascending and contiguous per front.
    REQUIRE(mf.pivot_first.size() == static_cast<crd::usize>(mf.nfront) + 1);
    CHECK(mf.pivot_first[0] == 0);
    CHECK(mf.pivot_first[mf.nfront] == n);
    for (crd::u32 f = 0; f < mf.nfront; ++f)
    {
        CHECK(mf.pivot_first[f] < mf.pivot_first[f + 1]); // non-empty pivot range

        // SYMMETRIC front: the row index set EQUALS the column index set (the LDLᵀ front is square,
        // L lower-triangular + D block-diagonal fill within it). This is the v5d structural invariant.
        const crd::u32 rb = mf.row_ptr[f];
        const crd::u32 rn = mf.row_ptr[f + 1] - rb;
        const crd::u32 cb = mf.col_ptr[f];
        const crd::u32 cn = mf.col_ptr[f + 1] - cb;
        REQUIRE(rn == cn); // symmetric front extent
        for (crd::u32 t = 0; t < rn; ++t)
        {
            CHECK(mf.row_idx[rb + t] == mf.col_idx[cb + t]); // same global ids ⇒ square symmetric front
            if (t > 0)
            {
                CHECK(mf.row_idx[rb + t - 1] < mf.row_idx[rb + t]); // ascending (extend_add contract)
            }
        }
        // the front's leading entries are exactly its pivot columns.
        const crd::u32 np = mf.pivot_first[f + 1] - mf.pivot_first[f];
        REQUIRE(rn >= np);
        for (crd::u32 t = 0; t < np; ++t)
        {
            CHECK(mf.row_idx[rb + t] == mf.pivot_first[f] + t);
        }
    }
}
} // namespace

TEST_CASE("v5d-a LDLt symbolic: symmetric tridiagonal", "[hesap][direct][v5d][v5d-a][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 6×6 symmetric tridiagonal (diag + sub/super) — a classic indefinite-capable pattern.
    sp::SparsePattern a = sym_csc_from_mask(6,
                                            {
                                                1, 1, 0, 0, 0, 0, //
                                                1, 1, 1, 0, 0, 0, //
                                                0, 1, 1, 1, 0, 0, //
                                                0, 0, 1, 1, 1, 0, //
                                                0, 0, 0, 1, 1, 1, //
                                                0, 0, 0, 0, 1, 1, //
                                            },
                                            &alloc);
    validate_ldlt_symbolic(a, &alloc);
}

TEST_CASE("v5d-a LDLt symbolic: symmetric arrow + 2D-grid-like (branching tree)", "[hesap][direct][v5d][v5d-a][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 7×7 symmetric: two independent tridiagonal arms (cols 0-2, 3-5) coupled to a root col 6 ⇒ a
    // BRANCHING front tree (two leaf subtrees merge at the root) — exercises non-trivial containment.
    sp::SparsePattern a = sym_csc_from_mask(7,
                                            {
                                                1, 1, 0, 0, 0, 0, 1, //
                                                1, 1, 1, 0, 0, 0, 0, //
                                                0, 1, 1, 0, 0, 0, 1, //
                                                0, 0, 0, 1, 1, 0, 1, //
                                                0, 0, 0, 1, 1, 1, 0, //
                                                0, 0, 0, 0, 1, 1, 1, //
                                                1, 0, 1, 1, 0, 1, 1, //
                                            },
                                            &alloc);
    validate_ldlt_symbolic(a, &alloc);
}

TEST_CASE("v5d-a LDLt symbolic: deterministic (bit-identical re-runs)", "[hesap][direct][v5d][v5d-a][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    sp::SparsePattern a = sym_csc_from_mask(5,
                                            {
                                                1, 1, 0, 0, 1, //
                                                1, 1, 1, 0, 0, //
                                                0, 1, 1, 1, 0, //
                                                0, 0, 1, 1, 1, //
                                                1, 0, 0, 1, 1, //
                                            },
                                            &alloc);
    dir::MultifrontalSymbolic m1 = dir::build_ldlt_symbolic(a, &alloc);
    dir::MultifrontalSymbolic m2 = dir::build_ldlt_symbolic(a, &alloc);
    REQUIRE(m1.nfront == m2.nfront);
    auto same = [](const crd::containers::Array<crd::u32>& x, const crd::containers::Array<crd::u32>& y)
    {
        if (x.size() != y.size())
        {
            return false;
        }
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            if (x[i] != y[i])
            {
                return false;
            }
        }
        return true;
    };
    CHECK(same(m1.pivot_first, m2.pivot_first));
    CHECK(same(m1.front_parent, m2.front_parent));
    CHECK(same(m1.row_ptr, m2.row_ptr));
    CHECK(same(m1.row_idx, m2.row_idx));
    CHECK(same(m1.col_ptr, m2.col_ptr));
    CHECK(same(m1.col_idx, m2.col_idx));
}

// =======================================================================
// v5d-b — the per-front indefinite Bunch-Kaufman kernel (factor_front_ldlt).
// =======================================================================

namespace
{
// Build an m×m COL-MAJOR lower-triangle front (ld=m) from a full row-major symmetric matrix.
template <typename T>
crd::containers::Array<T> front_from_sym(crd::u32 m, std::initializer_list<double> full,
                                         crd::memory::IAllocator* alloc)
{
    REQUIRE(full.size() == static_cast<crd::usize>(m) * m);
    const double* a = full.begin();
    crd::containers::Array<T> d(alloc);
    d.resize(static_cast<crd::usize>(m) * m);
    for (crd::usize i = 0; i < d.size(); ++i)
    {
        d[i] = T{0};
    }
    for (crd::u32 j = 0; j < m; ++j)
    {
        for (crd::u32 i = j; i < m; ++i)
        {
            d[static_cast<crd::usize>(j) * m + i] = static_cast<T>(a[static_cast<crd::usize>(i) * m + j]);
        }
    }
    return d;
}

// Reconstruct the full m×m matrix P·L·D·Lᵀ·Pᵀ from a fully factored front (r == m, no trailing).
template <typename T>
crd::containers::Array<T> reconstruct_full(crd::u32 m, const crd::containers::Array<T>& front,
                                           const crd::containers::Array<crd::u8>& bk,
                                           const crd::containers::Array<crd::u32>& piv,
                                           crd::memory::IAllocator* alloc)
{
    const crd::usize ld = m;
    auto el = [&](crd::u32 i, crd::u32 j) -> T { return front[static_cast<crd::usize>(j) * ld + i]; };

    crd::containers::Array<T> lmat(alloc);
    lmat.resize(static_cast<crd::usize>(m) * m);
    crd::containers::Array<T> dmat(alloc);
    dmat.resize(static_cast<crd::usize>(m) * m);
    for (crd::usize i = 0; i < lmat.size(); ++i)
    {
        lmat[i] = T{0};
        dmat[i] = T{0};
    }
    auto lr = [&](crd::u32 i, crd::u32 j) -> T& { return lmat[static_cast<crd::usize>(i) * m + j]; };
    auto dr = [&](crd::u32 i, crd::u32 j) -> T& { return dmat[static_cast<crd::usize>(i) * m + j]; };
    for (crd::u32 i = 0; i < m; ++i)
    {
        lr(i, i) = T{1};
    }

    crd::u32 k = 0;
    while (k < m)
    {
        if (bk[k] == 1U)
        {
            dr(k, k) = el(k, k);
            for (crd::u32 i = k + 1; i < m; ++i)
            {
                lr(i, k) = el(i, k);
            }
            k += 1;
        }
        else // 2×2 at (k, k+1); L[k+1,k] is implicitly 0 (slot holds D[k+1,k]).
        {
            dr(k, k) = el(k, k);
            dr(k + 1, k) = el(k + 1, k);
            dr(k, k + 1) = el(k + 1, k);
            dr(k + 1, k + 1) = el(k + 1, k + 1);
            for (crd::u32 i = k + 2; i < m; ++i)
            {
                lr(i, k) = el(i, k);
                lr(i, k + 1) = el(i, k + 1);
            }
            k += 2;
        }
    }

    // M = L·D·Lᵀ.
    crd::containers::Array<T> ld_mat(alloc);
    ld_mat.resize(static_cast<crd::usize>(m) * m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            T s = T{0};
            for (crd::u32 t = 0; t < m; ++t)
            {
                s += lr(i, t) * dr(t, j);
            }
            ld_mat[static_cast<crd::usize>(i) * m + j] = s;
        }
    }
    crd::containers::Array<T> mmat(alloc);
    mmat.resize(static_cast<crd::usize>(m) * m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            T s = T{0};
            for (crd::u32 t = 0; t < m; ++t)
            {
                s += ld_mat[static_cast<crd::usize>(i) * m + t] * lr(j, t); // (Lᵀ)[t,j] = L[j,t]
            }
            mmat[static_cast<crd::usize>(i) * m + j] = s;
        }
    }

    // Composite permutation π (replay the factor swaps forward, in the same order).
    crd::containers::Array<crd::u32> pi(alloc);
    pi.resize(m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        pi[i] = i;
    }
    k = 0;
    while (k < m)
    {
        if (bk[k] == 1U)
        {
            const crd::u32 r = piv[k];
            if (r != k)
            {
                const crd::u32 t = pi[k];
                pi[k] = pi[r];
                pi[r] = t;
            }
            k += 1;
        }
        else
        {
            const crd::u32 r = piv[k];
            if (r != k + 1)
            {
                const crd::u32 t = pi[k + 1];
                pi[k + 1] = pi[r];
                pi[r] = t;
            }
            k += 2;
        }
    }

    // A_recon[π[i]][π[j]] = M[i][j].
    crd::containers::Array<T> amat(alloc);
    amat.resize(static_cast<crd::usize>(m) * m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            amat[static_cast<crd::usize>(pi[i]) * m + pi[j]] = mmat[static_cast<crd::usize>(i) * m + j];
        }
    }
    return amat;
}

// Factor the whole front (npiv == m), reconstruct P·L·D·Lᵀ·Pᵀ, and check it equals the input A.
template <typename T>
void run_factor_reconstruct(crd::u32 m, std::initializer_list<double> full, double tol)
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<T> front = front_from_sym<T>(m, full, &alloc);
    crd::containers::Array<crd::u8> bk(&alloc);
    bk.resize(m);
    crd::containers::Array<crd::u32> piv(&alloc);
    piv.resize(m);
    const crd::u32 r = dir::factor_front_ldlt<T>(front.data(), m, m, m, bk.data(), piv.data());
    REQUIRE(r == m); // a wholly fully-summed front eliminates every pivot.
    for (crd::u32 k = 0; k < m; ++k)
    {
        CHECK(piv[k] < m); // pivots stay within the fully-summed block.
    }

    crd::containers::Array<T> recon = reconstruct_full<T>(m, front, bk, piv, &alloc);
    const double* orig = full.begin();
    double err = 0.0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < m; ++j)
        {
            const double diff = std::abs(static_cast<double>(recon[static_cast<crd::usize>(i) * m + j]) -
                                         orig[static_cast<crd::usize>(i) * m + j]);
            err = (diff > err) ? diff : err;
        }
    }
    CHECK(err < tol);
}
} // namespace

TEST_CASE("v5d-b factor_front_ldlt: col-major symmetric swap_sym", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // 4×4 symmetric with distinct entries so a swap is detectable.
    const crd::containers::Array<double> front =
        front_from_sym<double>(4, {1, 2, 3, 4, 2, 5, 6, 7, 3, 6, 8, 9, 4, 7, 9, 10}, &alloc);
    crd::containers::Array<double> d = front; // copy to mutate
    dir::ldlt_swap_sym<double>(d.data(), 4, 4, 1, 3);

    // Expected lower triangle after swapping rows AND cols {1,3} of the symmetric matrix.
    const double exp[16] = {
        1, 4, 3, 2, //
        4, 10, 9, 7, //
        3, 9, 8, 6, //
        2, 7, 6, 5, //
    };
    for (crd::u32 j = 0; j < 4; ++j)
    {
        for (crd::u32 i = j; i < 4; ++i) // lower triangle only (the kernel reads i >= j)
        {
            CHECK(d[static_cast<crd::usize>(j) * 4 + i] == exp[static_cast<crd::usize>(i) * 4 + j]);
        }
    }
}

TEST_CASE("v5d-b factor_front_ldlt: 1x1 pivot with row/col swap (f64)", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    // Weak (0,0), strong (1,1) ⇒ Bunch-Kaufman picks a 1×1 at imax=1 with a symmetric swap.
    run_factor_reconstruct<double>(2, {0.1, 0.5, 0.5, 5.0}, 1e-12);
}

TEST_CASE("v5d-b factor_front_ldlt: forced 2x2 indefinite block (f64)", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    // Zero diagonal, unit off-diagonal ⇒ no 1×1 is stable ⇒ a 2×2 pivot.
    run_factor_reconstruct<double>(2, {0.0, 1.0, 1.0, 0.0}, 1e-12);
}

TEST_CASE("v5d-b factor_front_ldlt: 5x5 mixed indefinite reconstruct (f64)", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    // Symmetric INDEFINITE (negative diagonals) — exercises a mix of 1×1 and 2×2 pivots + swaps.
    run_factor_reconstruct<double>(5,
                                   {
                                       2, 1, 0, 0, 1, //
                                       1, -3, 2, 0, 0, //
                                       0, 2, 1, 4, 0, //
                                       0, 0, 4, -1, 1, //
                                       1, 0, 0, 1, 3, //
                                   },
                                   1e-11);
}

TEST_CASE("v5d-b factor_front_ldlt: f32 reconstruct", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    run_factor_reconstruct<float>(4,
                                  {
                                      4, 1, 0, 1, //
                                      1, -2, 1, 0, //
                                      0, 1, 3, 1, //
                                      1, 0, 1, 5, //
                                  },
                                  1e-4);
}

TEST_CASE("v5d-b factor_front_ldlt: Schur identity npiv<m (f64)", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 m = 5;
    const crd::u32 npiv = 3;
    // Diagonally DOMINANT symmetric ⇒ all 1×1 pivots, no swaps, the CB-row restriction never fires.
    std::initializer_list<double> full = {
        10, 1, 0, 2, 0, //
        1, 12, 3, 0, 1, //
        0, 3, 11, 1, 0, //
        2, 0, 1, 9, 2, //
        0, 1, 0, 2, 8, //
    };
    crd::containers::Array<double> front = front_from_sym<double>(m, full, &alloc);
    crd::containers::Array<crd::u8> bk(&alloc);
    bk.resize(npiv);
    crd::containers::Array<crd::u32> piv(&alloc);
    piv.resize(npiv);
    const crd::u32 r = dir::factor_front_ldlt<double>(front.data(), m, m, npiv, bk.data(), piv.data());
    REQUIRE(r == npiv); // all fully-summed pivots eliminate (diagonally dominant).
    for (crd::u32 k = 0; k < npiv; ++k)
    {
        CHECK(bk[k] == 1U);   // diagonally dominant ⇒ pure 1×1.
        CHECK(piv[k] == k);   // no swaps.
    }

    // Reference Schur via plain symmetric Gaussian elimination (no pivot) on the full matrix.
    const double* a = full.begin();
    crd::containers::Array<double> w(&alloc);
    w.resize(static_cast<crd::usize>(m) * m);
    for (crd::usize i = 0; i < w.size(); ++i)
    {
        w[i] = a[i];
    }
    auto wr = [&](crd::u32 i, crd::u32 j) -> double& { return w[static_cast<crd::usize>(i) * m + j]; };
    for (crd::u32 p = 0; p < npiv; ++p)
    {
        const double dpp = wr(p, p);
        for (crd::u32 i = p + 1; i < m; ++i)
        {
            const double l = wr(i, p) / dpp;
            for (crd::u32 j = p + 1; j < m; ++j)
            {
                wr(i, j) -= l * wr(p, j);
            }
        }
    }
    // Trailing front lower triangle must equal the reference Schur.
    double err = 0.0;
    for (crd::u32 j = npiv; j < m; ++j)
    {
        for (crd::u32 i = j; i < m; ++i)
        {
            const double got = front[static_cast<crd::usize>(j) * m + i];
            const double diff = std::abs(got - wr(i, j));
            err = (diff > err) ? diff : err;
        }
    }
    CHECK(err < 1e-11);
}

TEST_CASE("v5d-b factor_front_ldlt: delayed pivot returns partial (f64)", "[hesap][direct][v5d][v5d-b][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // npiv=1, m=2: the single fully-summed variable has a zero diagonal and its only coupling is to the
    // non-fully-summed CB row 1 ⇒ no stable pivot exists within the fully-summed block ⇒ DELAYED. The
    // kernel must STOP at 0 and NEVER pivot onto the CB row (that would eliminate a non-fully-summed var).
    crd::containers::Array<double> front = front_from_sym<double>(2, {0.0, 1.0, 1.0, 5.0}, &alloc);
    crd::containers::Array<crd::u8> bk(&alloc);
    bk.resize(1);
    crd::containers::Array<crd::u32> piv(&alloc);
    piv.resize(1);
    const crd::u32 r = dir::factor_front_ldlt<double>(front.data(), 2, 2, 1, bk.data(), piv.data());
    CHECK(r == 0); // the fully-summed pivot is delayed to the parent (v5d-c / Duff-Reid).
}

// =======================================================================
// v5d-c — the multifrontal LDLᵀ driver (postorder walk + symmetric extend_add).
// =======================================================================

namespace
{
// Build a SparseMatrix<T,Csc> from a full row-major symmetric matrix (inserts every nonzero of both
// triangles; the factorize reads only the lower triangle, row >= col).
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csc> sym_csc_matrix(crd::u32 n, std::initializer_list<double> full,
                                                          crd::memory::IAllocator* alloc)
{
    REQUIRE(full.size() == static_cast<crd::usize>(n) * n);
    const double* d = full.begin();
    sp::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const double v = d[static_cast<crd::usize>(i) * n + j];
            if (v != 0.0)
            {
                tb.add(i, j, static_cast<T>(v));
            }
        }
    }
    auto csr = tb.compress();
    return sp::to_csc<T>(csr, alloc);
}

// Factor a symmetric matrix, then verify P·L·D·Lᵀ·Pᵀ == A by reconstruction: build the dense
// factor-position L (unit lower) + D (block diagonal), M = L·D·Lᵀ, and check M[i][j] == A[perm[i]][perm[j]].
template <typename T>
void check_ldlt_reconstruct(crd::u32 n, std::initializer_list<double> full, double tol, crd::memory::IAllocator* alloc,
                            bool want_multifront = false, bool want_perm_nontrivial = false, bool want_2x2 = false)
{
    auto a = sym_csc_matrix<T>(n, full, alloc);
    dir::MultifrontalLDLT<T> ldlt(alloc);
    // Force textbook BK (0.64) + FUNDAMENTAL fronts (relax=1, no amalgamation): these small matrices exercise
    // the 2×2 / swap / delayed-pivot / multifront-walk MECHANICS, which the relaxed perf default (α=0.001 +
    // amalgamation) deliberately collapses/avoids. The default path is covered by the dense-indef tests + bench.
    ldlt.set_pivot_threshold(0.6403882032022075);
    ldlt.set_amalgamation_relax(1);
    ldlt.factorize(a);
    REQUIRE(ldlt.info() == 0); // no delayed pivot
    REQUIRE(ldlt.n() == n);
    if (want_multifront)
    {
        CHECK(ldlt.front_count() > 1); // a genuine multifrontal walk (not a single dense front)
    }
    if (want_perm_nontrivial)
    {
        // ∃ i: perm[i] != i ⇒ a real BK swap occurred ⇒ the CB-row remap (ipos on a non-identity ancestor
        // perm) is genuinely exercised (the one piece of v5d-c with no v5d-b analog).
        const auto pm = ldlt.perm();
        bool nontrivial = false;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (pm[i] != i)
            {
                nontrivial = true;
            }
        }
        CHECK(nontrivial);
    }
    if (want_2x2)
    {
        // ∃ k: block_kinds[k] == 2 ⇒ a 2×2 pivot was used ⇒ the driver's 2×2 D-store (m_doff + d11/d22
        // split) + the blocksz=2 L-store (skip the partner row) are genuinely exercised (the defining LDLᵀ
        // path with no Cholesky analog).
        const auto bkc = ldlt.block_kinds();
        bool has2 = false;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (bkc[i] == 2U)
            {
                has2 = true;
            }
        }
        CHECK(has2);
    }

    const auto lp = ldlt.l_col_ptr();
    const auto li = ldlt.l_row_idx();
    const auto lx = ldlt.l_values();
    const auto dd = ldlt.d_diag();
    const auto doff = ldlt.d_offdiag();
    const auto bk = ldlt.block_kinds();
    const auto perm = ldlt.perm();

    crd::containers::Array<T> lmat(alloc);
    lmat.resize(static_cast<crd::usize>(n) * n);
    crd::containers::Array<T> dmat(alloc);
    dmat.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize i = 0; i < lmat.size(); ++i)
    {
        lmat[i] = T{0};
        dmat[i] = T{0};
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        lmat[static_cast<crd::usize>(i) * n + i] = T{1};
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 q = lp[c]; q < lp[c + 1]; ++q)
        {
            lmat[static_cast<crd::usize>(li[q]) * n + c] = lx[q]; // L[row][col]
        }
    }
    crd::u32 k = 0;
    while (k < n)
    {
        if (bk[k] == 1U)
        {
            dmat[static_cast<crd::usize>(k) * n + k] = dd[k];
            k += 1;
        }
        else
        {
            dmat[static_cast<crd::usize>(k) * n + k] = dd[k];
            dmat[static_cast<crd::usize>(k + 1) * n + k] = doff[k];
            dmat[static_cast<crd::usize>(k) * n + (k + 1)] = doff[k];
            dmat[static_cast<crd::usize>(k + 1) * n + (k + 1)] = dd[k + 1];
            k += 2;
        }
    }

    // M = L·D·Lᵀ.
    crd::containers::Array<T> ldm(alloc);
    ldm.resize(static_cast<crd::usize>(n) * n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::u32 t = 0; t < n; ++t)
            {
                s += lmat[static_cast<crd::usize>(i) * n + t] * dmat[static_cast<crd::usize>(t) * n + j];
            }
            ldm[static_cast<crd::usize>(i) * n + j] = s;
        }
    }
    crd::containers::Array<T> mmat(alloc);
    mmat.resize(static_cast<crd::usize>(n) * n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            T s = T{0};
            for (crd::u32 t = 0; t < n; ++t)
            {
                s += ldm[static_cast<crd::usize>(i) * n + t] * lmat[static_cast<crd::usize>(j) * n + t]; // Lᵀ[t,j]=L[j,t]
            }
            mmat[static_cast<crd::usize>(i) * n + j] = s;
        }
    }

    // Compare M[i][j] to A[perm[i]][perm[j]] (the input full symmetric matrix).
    const double* a_full = full.begin();
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const double got = static_cast<double>(mmat[static_cast<crd::usize>(i) * n + j]);
            const double want = a_full[static_cast<crd::usize>(perm[i]) * n + perm[j]];
            const double diff = std::abs(got - want);
            err = (diff > err) ? diff : err;
        }
    }
    CHECK(err < tol);
}
} // namespace

TEST_CASE("v5d-c MultifrontalLDLt: multi-front diagonally dominant (no swap, P=identity)",
          "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 7×7 branching tree (two tridiagonal arms 0-2, 3-5 coupled to root 6), diagonally dominant ⇒ all 1×1,
    // no swaps ⇒ P=identity ⇒ L·D·Lᵀ == A. Validates assembly + symmetric extend_add + postorder + storage.
    check_ldlt_reconstruct<double>(7,
                                   {
                                       9, 1, 0, 0, 0, 0, 1, //
                                       1, 8, 1, 0, 0, 0, 0, //
                                       0, 1, 7, 0, 0, 0, 1, //
                                       0, 0, 0, 9, 1, 0, 1, //
                                       0, 0, 0, 1, 8, 1, 0, //
                                       0, 0, 0, 0, 1, 7, 1, //
                                       1, 0, 1, 1, 0, 1, 12, //
                                   },
                                   1e-11, &alloc);
}

TEST_CASE("v5d-c MultifrontalLDLt: single-front indefinite (2x2 + swaps + perm via reconstruction)",
          "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // DENSE 4×4 symmetric INDEFINITE ⇒ a single front (npiv=4); the driver-level twin of the v5d-b
    // reconstruction — exercises 2×2 pivots / swaps + the block-local permutation through the CSC store/remap.
    check_ldlt_reconstruct<double>(4,
                                   {
                                       2, 1, 1, 1, //
                                       1, -3, 1, 1, //
                                       1, 1, 4, 1, //
                                       1, 1, 1, -2, //
                                   },
                                   1e-11, &alloc);
}

TEST_CASE("v5d-c MultifrontalLDLt: multi-front indefinite with a dense trailing block",
          "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 6×6: arrow vars 0..3 each couple to a DENSE indefinite trailing block {4,5} ⇒ a multi-pivot root
    // supernode (npiv>=2) inside the multifrontal walk. Reconstruction (== A[perm,perm]) is the oracle for
    // whatever pivoting + permutation the driver produced (incl. a cross-front 2×2 / swap at the root).
    check_ldlt_reconstruct<double>(6,
                                   {
                                       6, 0, 0, 0, 1, 1, //
                                       0, 6, 0, 0, 1, 1, //
                                       0, 0, 6, 0, 1, 1, //
                                       0, 0, 0, 6, 1, 1, //
                                       1, 1, 1, 1, -1, 3, //
                                       1, 1, 1, 1, 3, -1, //
                                   },
                                   1e-10, &alloc);
}

TEST_CASE("v5d-c MultifrontalLDLt: cross-front swap exercises the CB-row remap", "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Leaves {0,1} each couple to a DENSE root block {2,3}; 0,1 NOT coupled ⇒ fronts {0},{1} (CB {2,3}) +
    // root {2,3} (npiv=2). The root block is tuned so that after the leaves' Schur, its FIRST diagonal is
    // weak and its SECOND strong ⇒ Bunch-Kaufman takes a 1×1-at-imax WITH A SWAP at the root. Those swapped
    // pivots {2,3} are the CB rows of the leaves ⇒ the leaves' stored L21 must be remapped through the root's
    // non-identity permutation. This is the ONE code path (CB-row remap on a non-identity ancestor perm) with
    // no v5d-b analog; want_multifront + want_perm_nontrivial assert it is genuinely executed.
    check_ldlt_reconstruct<double>(4,
                                   {
                                       10, 0, 1, 1, //
                                       0, 10, 1, 1, //
                                       1, 1, 0.5, 2.5, //
                                       1, 1, 2.5, 9, //
                                   },
                                   1e-10, &alloc, /*want_multifront=*/true, /*want_perm_nontrivial=*/true);
}

TEST_CASE("v5d-c MultifrontalLDLt: forced 2x2 pivot through the driver store", "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Zero leading diagonals + unit off-diagonals ⇒ Bunch-Kaufman MUST take a 2×2 (the defining LDLᵀ path).
    // want_2x2 asserts the driver's 2×2 D-store (m_doff + d11/d22 split) + blocksz=2 L-store (partner-row
    // skip) are genuinely executed — none of that plumbing has a v5d-b (raw-kernel) analog.
    check_ldlt_reconstruct<double>(3,
                                   {
                                       0, 2, 1, //
                                       2, 0, 1, //
                                       1, 1, 5, //
                                   },
                                   1e-11, &alloc, /*want_multifront=*/false, /*want_perm_nontrivial=*/false,
                                   /*want_2x2=*/true);
}

TEST_CASE("v5d-c MultifrontalLDLt: f32 reconstruct", "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_ldlt_reconstruct<float>(5,
                                  {
                                      10, 1, 0, 2, 0, //
                                      1, 12, 3, 0, 1, //
                                      0, 3, 11, 1, 0, //
                                      2, 0, 1, 9, 2, //
                                      0, 1, 0, 2, 8, //
                                  },
                                  1e-3, &alloc);
}

TEST_CASE("v5d-h MultifrontalLDLt: a DELAYED pivot now factors correctly (Duff-Reid)",
          "[hesap][direct][v5d][v5d-h][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // var 0 has a ZERO diagonal and couples only to var 2 (a CB row of front {0}): no stable pivot exists in
    // front {0}'s fully-summed block, so var 0 is DELAYED up to its parent (front {2}), where it pairs with
    // var 2 into a stable 2×2. The matrix is nonsingular (det = -5) ⇒ v5d-h must factor it correctly (this
    // exact matrix used to set info != 0 — the "refuse-on-delay" placeholder before delayed pivots existed).
    // At the parent front {0,2} the BK pivots the strong diagonal (var 2) in via a 1×1 swap, then eliminates
    // the delayed var 0 — so a non-trivial permutation occurs (not a 2×2). Reconstruction proves correctness.
    check_ldlt_reconstruct<double>(3,
                                   {
                                       0, 0, 1, //
                                       0, 5, 1, //
                                       1, 1, 4, //
                                   },
                                   1e-9, &alloc, /*want_multifront=*/false, /*want_perm_nontrivial=*/true,
                                   /*want_2x2=*/false);
}

TEST_CASE("v5d-c MultifrontalLDLt: deterministic (bit-identical re-runs)", "[hesap][direct][v5d][v5d-c][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    auto a = sym_csc_matrix<double>(4,
                                    {
                                        2, 1, 1, 1, //
                                        1, -3, 1, 1, //
                                        1, 1, 4, 1, //
                                        1, 1, 1, -2, //
                                    },
                                    &alloc);
    dir::MultifrontalLDLT<double> f1(&alloc);
    f1.factorize(a);
    dir::MultifrontalLDLT<double> f2(&alloc);
    f2.factorize(a);
    REQUIRE(f1.info() == 0);
    REQUIRE(f2.info() == 0);
    const auto x1 = f1.l_values();
    const auto x2 = f2.l_values();
    REQUIRE(x1.size() == x2.size());
    bool same = true;
    for (crd::usize i = 0; i < x1.size(); ++i)
    {
        if (x1[i] != x2[i])
        {
            same = false;
        }
    }
    CHECK(same);
    const auto p1 = f1.perm();
    const auto p2 = f2.perm();
    REQUIRE(p1.size() == p2.size());
    for (crd::usize i = 0; i < p1.size(); ++i)
    {
        CHECK(p1[i] == p2[i]);
    }
}

// =======================================================================
// v5d-d — the L·D·Lᵀ solve (P / forward-L / block-diag-D / backward-Lᵀ / Pᵀ).
// =======================================================================

namespace
{
// Factor A, solve A·X = B for a known X (B = A·X computed densely), and check both ‖x − x_true‖ and the
// residual ‖A·x − b‖ are tiny. nrhs columns are column-major. The residual is the direct equation check.
template <typename T>
void check_ldlt_solve(crd::u32 n, std::initializer_list<double> full, crd::usize nrhs, double tol,
                      crd::memory::IAllocator* alloc, bool want_2x2 = false)
{
    auto a = sym_csc_matrix<T>(n, full, alloc);
    dir::MultifrontalLDLT<T> ldlt(alloc);
    ldlt.set_pivot_threshold(0.6403882032022075); // textbook BK + fundamental fronts — exercise the 2×2/swap/
    ldlt.set_amalgamation_relax(1);               // multifront mechanics (see reconstruct); default path = bench
    ldlt.factorize(a);
    REQUIRE(ldlt.info() == 0);
    if (want_2x2)
    {
        const auto bkc = ldlt.block_kinds(); // ⇒ the 2×2 D-solve (determinant inverse) is genuinely exercised
        bool has2 = false;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (bkc[i] == 2U)
            {
                has2 = true;
            }
        }
        CHECK(has2);
    }
    const double* af = full.begin();

    crd::containers::Array<T> xtrue(alloc);
    xtrue.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::usize q = 0; q < nrhs; ++q)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            const double sign = (((i + q) % 3) == 0) ? 1.0 : -1.0;
            xtrue[q * n + i] = static_cast<T>(sign * (1.0 + 0.25 * static_cast<double>(i) + 0.5 * static_cast<double>(q)));
        }
    }
    crd::containers::Array<T> rhs(alloc);
    rhs.resize(static_cast<crd::usize>(n) * nrhs);
    crd::containers::Array<T> bsave(alloc);
    bsave.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::usize q = 0; q < nrhs; ++q)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            T s = T{0};
            for (crd::u32 j = 0; j < n; ++j)
            {
                s += static_cast<T>(af[static_cast<crd::usize>(i) * n + j]) * xtrue[q * n + j];
            }
            rhs[q * n + i] = s;
            bsave[q * n + i] = s;
        }
    }
    REQUIRE(ldlt.solve({rhs.data(), rhs.size()}, nrhs));

    double err = 0.0;
    for (crd::usize e = 0; e < rhs.size(); ++e)
    {
        const double d = std::abs(static_cast<double>(rhs[e]) - static_cast<double>(xtrue[e]));
        err = (d > err) ? d : err;
    }
    CHECK(err < tol);

    double res = 0.0;
    for (crd::usize q = 0; q < nrhs; ++q)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            double ax = 0.0;
            for (crd::u32 j = 0; j < n; ++j)
            {
                ax += af[static_cast<crd::usize>(i) * n + j] * static_cast<double>(rhs[q * n + j]);
            }
            const double d = std::abs(ax - static_cast<double>(bsave[q * n + i]));
            res = (d > res) ? d : res;
        }
    }
    CHECK(res < tol);
}
} // namespace

TEST_CASE("v5d-d MultifrontalLDLt solve: multi-front diagonally dominant", "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_ldlt_solve<double>(7,
                             {
                                 9, 1, 0, 0, 0, 0, 1, //
                                 1, 8, 1, 0, 0, 0, 0, //
                                 0, 1, 7, 0, 0, 0, 1, //
                                 0, 0, 0, 9, 1, 0, 1, //
                                 0, 0, 0, 1, 8, 1, 0, //
                                 0, 0, 0, 0, 1, 7, 1, //
                                 1, 0, 1, 1, 0, 1, 12, //
                             },
                             1, 1e-9, &alloc);
}

TEST_CASE("v5d-d MultifrontalLDLt solve: single-front dense indefinite (2x2 pivots)",
          "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_ldlt_solve<double>(4,
                             {
                                 2, 1, 1, 1, //
                                 1, -3, 1, 1, //
                                 1, 1, 4, 1, //
                                 1, 1, 1, -2, //
                             },
                             1, 1e-9, &alloc);
}

TEST_CASE("v5d-d MultifrontalLDLt solve: cross-front swap (end-to-end perm through the solve)",
          "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // The v5d-c cross-front-swap matrix — here the solve exercises the same block-local permutation
    // end-to-end (gather Pᵀb / scatter Px through a non-identity perm touching the leaves' CB rows).
    check_ldlt_solve<double>(4,
                             {
                                 10, 0, 1, 1, //
                                 0, 10, 1, 1, //
                                 1, 1, 0.5, 2.5, //
                                 1, 1, 2.5, 9, //
                             },
                             1, 1e-9, &alloc);
}

TEST_CASE("v5d-d MultifrontalLDLt solve: forced 2x2 pivot (block-aware D-solve)",
          "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Zero leading diagonals ⇒ a 2×2 pivot ⇒ the solve's 2×2 D-step (determinant-based inverse reading
    // m_doff) is genuinely exercised; the residual ‖A·x−b‖ against this indefinite A is the oracle.
    check_ldlt_solve<double>(3,
                             {
                                 0, 2, 1, //
                                 2, 0, 1, //
                                 1, 1, 5, //
                             },
                             1, 1e-10, &alloc, /*want_2x2=*/true);
}

TEST_CASE("v5d-d MultifrontalLDLt solve: multi-RHS (nrhs=3, multi-front indefinite)",
          "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_ldlt_solve<double>(6,
                             {
                                 6, 0, 0, 0, 1, 1, //
                                 0, 6, 0, 0, 1, 1, //
                                 0, 0, 6, 0, 1, 1, //
                                 0, 0, 0, 6, 1, 1, //
                                 1, 1, 1, 1, -1, 3, //
                                 1, 1, 1, 1, 3, -1, //
                             },
                             3, 1e-9, &alloc);
}

TEST_CASE("v5d-d MultifrontalLDLt solve: f32", "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_ldlt_solve<float>(5,
                            {
                                10, 1, 0, 2, 0, //
                                1, 12, 3, 0, 1, //
                                0, 3, 11, 1, 0, //
                                2, 0, 1, 9, 2, //
                                0, 1, 0, 2, 8, //
                            },
                            1, 2e-4, &alloc);
}

TEST_CASE("v5d-d MultifrontalLDLt solve: invalid factor (singular matrix) returns false",
          "[hesap][direct][v5d][v5d-d][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // var 0 is structurally NULL (zero diagonal, no couplings) ⇒ it can never be eliminated at any front and
    // has no parent to delay to ⇒ the postorder position pass ends with gp < n ⇒ info != 0 (genuinely
    // singular, distinct from a recoverable delayed pivot). The solve must refuse an invalid factor.
    auto a = sym_csc_matrix<double>(3,
                                    {
                                        0, 0, 0, //
                                        0, 1, 0, //
                                        0, 0, 1, //
                                    },
                                    &alloc);
    dir::MultifrontalLDLT<double> ldlt(&alloc);
    ldlt.factorize(a);
    REQUIRE(ldlt.info() != 0); // structurally singular ⇒ factor invalid
    crd::containers::Array<double> rhs(&alloc);
    rhs.resize(3);
    for (crd::u32 i = 0; i < 3; ++i)
    {
        rhs[i] = 1.0;
    }
    CHECK_FALSE(ldlt.solve({rhs.data(), 3})); // solve must refuse an invalid factor
}

// =======================================================================
// v5d-e — tree-parallel factorization + the cross-thread determinism moat.
// =======================================================================

namespace
{
// 12×12 BLOCK-DIAGONAL: nblk independent copies of the 2×2-forcing indefinite block [[0,2,1],[2,0,1],[1,1,5]]
// (zero leading diagonals ⇒ each block takes a 2×2 pivot). No coupling between blocks ⇒ nblk INDEPENDENT
// single-front trees ⇒ a level-0 with nblk concurrent fronts AND a 2×2 in each ⇒ the moat is proven for the
// 2×2 path UNDER real parallelism, not just the all-1×1 (Cholesky-like) case.
template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csc> block_diag_indef(crd::u32 nblk, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = nblk * 3;
    sp::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 b = 0; b < nblk; ++b)
    {
        const crd::u32 o = b * 3; // diagonals (o+0,o+0) and (o+1,o+1) are 0 (omitted) ⇒ zero-filled in the front
        tb.add(o + 0, o + 1, static_cast<T>(2));
        tb.add(o + 1, o + 0, static_cast<T>(2));
        tb.add(o + 0, o + 2, static_cast<T>(1));
        tb.add(o + 2, o + 0, static_cast<T>(1));
        tb.add(o + 1, o + 2, static_cast<T>(1));
        tb.add(o + 2, o + 1, static_cast<T>(1));
        tb.add(o + 2, o + 2, static_cast<T>(5));
    }
    auto csr = tb.compress();
    return sp::to_csc<T>(csr, alloc);
}

template <typename T> bool span_eq(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
{
    if (x.size() != y.size())
    {
        return false;
    }
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        if (x[i] != y[i])
        {
            return false;
        }
    }
    return true;
}

template <typename T> void run_ldlt_parallel_moat(crd::memory::IAllocator* alloc)
{
    const crd::u32 nblk = 4;
    auto a = block_diag_indef<T>(nblk, alloc);
    const crd::u32 n = nblk * 3;

    dir::MultifrontalLDLT<T> f1(alloc); // serial reference
    f1.factorize(a, 1);
    REQUIRE(f1.info() == 0);
    REQUIRE(f1.front_count() >= nblk); // ≥nblk independent root fronts ⇒ a level with ≥nblk concurrent fronts
    {
        const auto bkc = f1.block_kinds();
        bool has2 = false;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (bkc[i] == 2U)
            {
                has2 = true;
            }
        }
        REQUIRE(has2); // a 2×2 pivot occurs ⇒ the parallel determinism is tested for the 2×2 path
    }

    // A consistent RHS so the solution is well-defined (b = A·x_true).
    crd::containers::Array<T> xt(alloc);
    xt.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = static_cast<T>(1.0 + 0.1 * static_cast<double>(i));
    }
    auto make_b = [&]()
    {
        crd::containers::Array<T> b(alloc);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b[i] = T{0};
        }
        for (crd::u32 c = 0; c < n; ++c)
        {
            for (crd::u32 q = a.pattern().outer_ptr[c]; q < a.pattern().outer_ptr[c + 1]; ++q)
            {
                b[a.pattern().inner_idx[q]] += a.values().values[q] * xt[c];
            }
        }
        return b;
    };
    crd::containers::Array<T> x1 = make_b();
    REQUIRE(f1.solve({x1.data(), n}));

    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        dir::MultifrontalLDLT<T> fp(alloc);
        fp.factorize(a, nw);
        REQUIRE(fp.info() == 0);
        // L (lp/li/lx) + D (dd/doff/block_kinds) + perm BIT-IDENTICAL across worker counts AND vs serial.
        bool ident = span_eq<crd::u32>(f1.l_col_ptr(), fp.l_col_ptr()) &&
                     span_eq<crd::u32>(f1.l_row_idx(), fp.l_row_idx()) && span_eq<T>(f1.l_values(), fp.l_values()) &&
                     span_eq<T>(f1.d_diag(), fp.d_diag()) && span_eq<T>(f1.d_offdiag(), fp.d_offdiag()) &&
                     span_eq<crd::u8>(f1.block_kinds(), fp.block_kinds()) && span_eq<crd::u32>(f1.perm(), fp.perm());
        CHECK(ident); // the determinism moat — L, D, perm are a pure function of the pattern

        crd::containers::Array<T> xp = make_b();
        REQUIRE(fp.solve({xp.data(), n}));
        bool xident = true;
        for (crd::u32 i = 0; i < n && xident; ++i)
        {
            xident = (xp[i] == x1[i]); // bit-identical solution too
        }
        CHECK(xident);
    }
}
} // namespace

TEST_CASE("v5d-e MultifrontalLDLt: factor + solve bit-identical across {1,2,4,8,16} workers",
          "[hesap][direct][v5d][v5d-e][ldlt]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 23);
        run_ldlt_parallel_moat<crd::f64>(&alloc);
        run_ldlt_parallel_moat<crd::f32>(&alloc); // f32 — where worker-count FP drift would show first
    }
    crd::jobs::shutdown();
}

// =======================================================================
// v5d-f — complex LDLᵀ (complex-symmetric, no conj) + LDLᴴ (Hermitian, conj).
// =======================================================================

namespace
{
template <typename T> T cval(double re, double im)
{
    using R = crd::hesap::dense::RealType<T>;
    return T{static_cast<R>(re), static_cast<R>(im)};
}

template <typename T>
sp::SparseMatrix<T, sp::SparseFormat::Csc> csc_from_dense(crd::u32 n, const crd::containers::Array<T>& full,
                                                          crd::memory::IAllocator* alloc)
{
    sp::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            const T v = full[static_cast<crd::usize>(i) * n + j];
            if (v.re != 0 || v.im != 0)
            {
                tb.add(i, j, v);
            }
        }
    }
    return sp::to_csc<T>(tb.compress(), alloc);
}

// Factor a complex matrix (hermitian = LDLᴴ vs LDLᵀ), then validate by reconstruction against the RIGHT
// product (LDLᵀ → A=P·L·D·Lᵀ·Pᵀ ; LDLᴴ → A=P·L·D·Lᴴ·Pᵀ) AND a solve residual ‖A·x−b‖. Asserts a 2×2 pivot
// ran and (Hermitian) the recovered 1×1 D entries are real. `full` MUST have genuinely nonzero imaginary
// parts (else the conj path is untested) and be symmetric/Hermitian to match `hermitian`.
template <typename T>
void check_complex_ldlt(crd::u32 n, const crd::containers::Array<T>& full, bool hermitian, double tol,
                        crd::memory::IAllocator* alloc)
{
    auto a = csc_from_dense<T>(n, full, alloc);
    dir::MultifrontalLDLT<T> ldlt(alloc);
    ldlt.factorize(a, 1, hermitian);
    REQUIRE(ldlt.info() == 0);
    REQUIRE(ldlt.hermitian() == hermitian);

    const auto lp = ldlt.l_col_ptr();
    const auto li = ldlt.l_row_idx();
    const auto lx = ldlt.l_values();
    const auto dd = ldlt.d_diag();
    const auto doff = ldlt.d_offdiag();
    const auto bkc = ldlt.block_kinds();
    const auto perm = ldlt.perm();

    bool has2 = false;
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bkc[i] == 2U)
        {
            has2 = true;
        }
        if (hermitian && bkc[i] == 1U)
        {
            CHECK(std::abs(static_cast<double>(dd[i].im)) < 1e-11); // Hermitian D 1×1 is real
        }
    }
    REQUIRE(has2); // a 2×2 pivot ran ⇒ the complex 2×2 store/inverse is exercised

    // Densify L (unit lower) + D (block diag; Hermitian off-diag conjugated above the diagonal).
    crd::containers::Array<T> lm(alloc);
    crd::containers::Array<T> dm(alloc);
    lm.resize(static_cast<crd::usize>(n) * n);
    dm.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize i = 0; i < lm.size(); ++i)
    {
        lm[i] = T{0, 0};
        dm[i] = T{0, 0};
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        lm[static_cast<crd::usize>(i) * n + i] = T{1, 0};
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 q = lp[c]; q < lp[c + 1]; ++q)
        {
            lm[static_cast<crd::usize>(li[q]) * n + c] = lx[q];
        }
    }
    crd::u32 k = 0;
    while (k < n)
    {
        if (bkc[k] == 1U)
        {
            dm[static_cast<crd::usize>(k) * n + k] = dd[k];
            k += 1;
        }
        else
        {
            dm[static_cast<crd::usize>(k) * n + k] = dd[k];
            dm[static_cast<crd::usize>(k + 1) * n + k] = doff[k];
            dm[static_cast<crd::usize>(k) * n + (k + 1)] = hermitian ? crd::hesap::conj(doff[k]) : doff[k];
            dm[static_cast<crd::usize>(k + 1) * n + (k + 1)] = dd[k + 1];
            k += 2;
        }
    }

    // M = L·D·L^(T or H): the right factor is conjugated for Hermitian.
    crd::containers::Array<T> ldm(alloc);
    ldm.resize(static_cast<crd::usize>(n) * n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            T s{0, 0};
            for (crd::u32 t = 0; t < n; ++t)
            {
                s = s + lm[static_cast<crd::usize>(i) * n + t] * dm[static_cast<crd::usize>(t) * n + j];
            }
            ldm[static_cast<crd::usize>(i) * n + j] = s;
        }
    }
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            T s{0, 0};
            for (crd::u32 t = 0; t < n; ++t)
            {
                const T ljt = lm[static_cast<crd::usize>(j) * n + t];
                const T rfac = hermitian ? crd::hesap::conj(ljt) : ljt; // (L^H)[t,j] = conj(L[j,t])
                s = s + ldm[static_cast<crd::usize>(i) * n + t] * rfac;
            }
            const T want = full[static_cast<crd::usize>(perm[i]) * n + perm[j]];
            const double d = static_cast<double>(crd::hesap::abs(s - want));
            err = (d > err) ? d : err;
        }
    }
    CHECK(err < tol);

    // Solve residual: b = A·x_true (with nonzero-imaginary x_true), solve, check ‖A·x−b‖.
    crd::containers::Array<T> xt(alloc);
    xt.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = cval<T>(1.0 + 0.3 * static_cast<double>(i), 0.5 - 0.2 * static_cast<double>(i));
    }
    crd::containers::Array<T> rhs(alloc);
    crd::containers::Array<T> bsave(alloc);
    rhs.resize(n);
    bsave.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        T s{0, 0};
        for (crd::u32 j = 0; j < n; ++j)
        {
            s = s + full[static_cast<crd::usize>(i) * n + j] * xt[j];
        }
        rhs[i] = s;
        bsave[i] = s;
    }
    REQUIRE(ldlt.solve({rhs.data(), n}));
    double res = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        T ax{0, 0};
        for (crd::u32 j = 0; j < n; ++j)
        {
            ax = ax + full[static_cast<crd::usize>(i) * n + j] * rhs[j];
        }
        const double d = static_cast<double>(crd::hesap::abs(ax - bsave[i]));
        res = (d > res) ? d : res;
    }
    CHECK(res < tol);
}

// A 3×3 complex indefinite block: zero leading diagonals (0,0),(1,1) ⇒ Bunch-Kaufman MUST take a 2×2 (both
// diagonal candidates weak); (2,2)=5; strong off-diagonals with NONZERO imaginary. `hermitian` ⇒ build it
// Hermitian (A[j][i]=conj(A[i][j]), real diagonal) else complex-symmetric (A[j][i]=A[i][j]). Non-singular
// (det of the leading 2×2 = −(2+1i)² ≠ 0); this is the same block the moat uses, so it factors in both modes.
template <typename T> crd::containers::Array<T> complex_block_3x3(bool hermitian, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> a(alloc);
    a.resize(9);
    for (auto& z : a)
    {
        z = T{0, 0};
    }
    auto put = [&](crd::u32 i, crd::u32 j, T v)
    {
        a[static_cast<crd::usize>(i) * 3 + j] = v;
        a[static_cast<crd::usize>(j) * 3 + i] = hermitian ? crd::hesap::conj(v) : v;
    };
    a[static_cast<crd::usize>(2) * 3 + 2] = cval<T>(5, 0); // real (the other two diagonals are 0 ⇒ a 2×2)
    put(0, 1, cval<T>(2, 1));
    put(0, 2, cval<T>(1, 0.5));
    put(1, 2, cval<T>(1, -0.25));
    return a;
}
} // namespace

TEST_CASE("v5d-f MultifrontalLDLt: complex-symmetric LDLT (no conj) reconstruct + solve",
          "[hesap][direct][v5d][v5d-f][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_complex_ldlt<crd::hesap::Complex64>(3, complex_block_3x3<crd::hesap::Complex64>(false, &alloc),
                                              /*hermitian=*/false, 1e-10, &alloc);
    check_complex_ldlt<crd::hesap::Complex32>(3, complex_block_3x3<crd::hesap::Complex32>(false, &alloc), false, 2e-4,
                                              &alloc);
}

TEST_CASE("v5d-f MultifrontalLDLt: Hermitian-indefinite LDLH (conj, real D) reconstruct + solve",
          "[hesap][direct][v5d][v5d-f][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_complex_ldlt<crd::hesap::Complex64>(3, complex_block_3x3<crd::hesap::Complex64>(true, &alloc),
                                              /*hermitian=*/true, 1e-10, &alloc);
    check_complex_ldlt<crd::hesap::Complex32>(3, complex_block_3x3<crd::hesap::Complex32>(true, &alloc), true, 2e-4,
                                              &alloc);
}

TEST_CASE("v5d-f MultifrontalLDLt: the hermitian flag is load-bearing (same matrix, modes differ)",
          "[hesap][direct][v5d][v5d-f][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    using C = crd::hesap::Complex64;
    // ONE genuinely-Hermitian matrix (nonzero imaginary). Factor it BOTH ways: LDLᴴ (correct) reads it as
    // Hermitian; LDLᵀ (wrong) reads the lower triangle as complex-symmetric ⇒ a DIFFERENT matrix. Proving the
    // factors differ + only LDLᴴ recovers the true solution proves m_hermitian actually changes the math
    // (not a decorative flag) — the selection analog of v5d-e's worker-count check.
    const crd::u32 n = 3;
    crd::containers::Array<C> full = complex_block_3x3<C>(/*hermitian=*/true, &alloc);
    auto a = csc_from_dense<C>(n, full, &alloc);

    crd::containers::Array<C> xt(&alloc);
    xt.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = cval<C>(1.0 + 0.3 * static_cast<double>(i), 0.5 - 0.2 * static_cast<double>(i));
    }
    auto resid_for = [&](bool hermitian) -> double
    {
        crd::containers::Array<C> b(&alloc);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            C s{0, 0};
            for (crd::u32 j = 0; j < n; ++j)
            {
                s = s + full[static_cast<crd::usize>(i) * n + j] * xt[j];
            }
            b[i] = s;
        }
        dir::MultifrontalLDLT<C> f(&alloc);
        f.factorize(a, 1, hermitian);
        REQUIRE(f.info() == 0); // both modes factor (the symmetric completion is a valid complex-sym matrix)
        crd::containers::Array<C> bsave(&alloc);
        bsave.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            bsave[i] = b[i];
        }
        REQUIRE(f.solve({b.data(), n}));
        double res = 0.0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            C ax{0, 0};
            for (crd::u32 j = 0; j < n; ++j)
            {
                ax = ax + full[static_cast<crd::usize>(i) * n + j] * b[j];
            }
            const double d = static_cast<double>(crd::hesap::abs(ax - bsave[i]));
            res = (d > res) ? d : res;
        }
        return res;
    };
    const double res_h = resid_for(/*hermitian=*/true);
    const double res_s = resid_for(/*hermitian=*/false);
    CHECK(res_h < 1e-10);  // LDLᴴ solves the true Hermitian system
    CHECK(res_s > 1e-3);   // LDLᵀ solves the (different) symmetric-completion system ⇒ wrong for Hermitian A
}

namespace
{
// Block-diagonal complex moat: nblk independent copies of a 2×2-forcing complex block (symmetric or
// Hermitian per `hermitian`) ⇒ nblk concurrent fronts + a 2×2 each ⇒ proves bit-identity UNDER parallelism.
template <typename T> void run_complex_ldlt_moat(bool hermitian, crd::memory::IAllocator* alloc)
{
    const crd::u32 nblk = 4;
    const crd::u32 n = nblk * 3;
    crd::containers::Array<T> full(alloc);
    full.resize(static_cast<crd::usize>(n) * n);
    for (auto& z : full)
    {
        z = T{0, 0};
    }
    auto put = [&](crd::u32 i, crd::u32 j, T v)
    {
        full[static_cast<crd::usize>(i) * n + j] = v;
        full[static_cast<crd::usize>(j) * n + i] = hermitian ? crd::hesap::conj(v) : v;
    };
    for (crd::u32 b = 0; b < nblk; ++b)
    {
        const crd::u32 o = b * 3;
        // diagonals (o,o) and (o+1,o+1) are 0 (real) ⇒ a 2×2; (o+2,o+2) = 5 (real).
        full[static_cast<crd::usize>(o + 2) * n + (o + 2)] = cval<T>(5, 0);
        put(o + 0, o + 1, cval<T>(2, 1));
        put(o + 0, o + 2, cval<T>(1, 0.5));
        put(o + 1, o + 2, cval<T>(1, -0.25));
    }
    auto a = csc_from_dense<T>(n, full, alloc);

    dir::MultifrontalLDLT<T> f1(alloc);
    f1.factorize(a, 1, hermitian);
    REQUIRE(f1.info() == 0);
    REQUIRE(f1.front_count() >= nblk);
    {
        const auto bkc = f1.block_kinds();
        bool has2 = false;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (bkc[i] == 2U)
            {
                has2 = true;
            }
        }
        REQUIRE(has2);
    }
    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        dir::MultifrontalLDLT<T> fp(alloc);
        fp.factorize(a, nw, hermitian);
        REQUIRE(fp.info() == 0);
        const bool ident = span_eq<crd::u32>(f1.l_col_ptr(), fp.l_col_ptr()) &&
                           span_eq<crd::u32>(f1.l_row_idx(), fp.l_row_idx()) &&
                           span_eq<T>(f1.l_values(), fp.l_values()) && span_eq<T>(f1.d_diag(), fp.d_diag()) &&
                           span_eq<T>(f1.d_offdiag(), fp.d_offdiag()) &&
                           span_eq<crd::u8>(f1.block_kinds(), fp.block_kinds()) &&
                           span_eq<crd::u32>(f1.perm(), fp.perm());
        CHECK(ident); // the determinism moat for the complex path
    }
}
} // namespace

TEST_CASE("v5d-f MultifrontalLDLt: complex moat (LDLT + LDLH) bit-identical across {1,2,4,8,16} workers",
          "[hesap][direct][v5d][v5d-f][ldlt]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 23);
        run_complex_ldlt_moat<crd::hesap::Complex64>(/*hermitian=*/false, &alloc); // complex-symmetric LDLᵀ
        run_complex_ldlt_moat<crd::hesap::Complex64>(/*hermitian=*/true, &alloc);  // Hermitian LDLᴴ
        run_complex_ldlt_moat<crd::hesap::Complex32>(false, &alloc);
        run_complex_ldlt_moat<crd::hesap::Complex32>(true, &alloc);
    }
    crd::jobs::shutdown();
}

// =======================================================================
// v5d-perf — the BLOCKED-BLAS-3 front factor (engaged for BIG fronts; the MUMPS-crush lever).
// =======================================================================

TEST_CASE("v5d-perf MultifrontalLDLt: big dense SPD front uses the blocked path (solve residual)",
          "[hesap][direct][v5d][v5d-perf][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 n = 160; // > kBlockMin(128) ⇒ a single dense front factored by the BLOCKED path
    // Dense SPD (diag n+1, off 1) ⇒ a single front, all 1×1, no bail ⇒ exercises factor_front_ldlt_blocked.
    sp::TripletBuilder<double> tb(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            tb.add(i, j, (i == j) ? static_cast<double>(n + 1) : 1.0);
        }
    }
    auto a = sp::to_csc<double>(tb.compress(), &alloc);
    dir::MultifrontalLDLT<double> ldlt(&alloc);
    ldlt.factorize(a);
    REQUIRE(ldlt.info() == 0);
    REQUIRE(ldlt.front_count() == 1); // one dense front (npiv=160 > kBlockMin) ⇒ blocked path

    crd::containers::Array<double> xt(&alloc);
    xt.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 1.0 + 0.25 * static_cast<double>(i);
    }
    crd::containers::Array<double> b(&alloc);
    crd::containers::Array<double> bsave(&alloc);
    b.resize(n);
    bsave.resize(n);
    const double* av = a.values().values.data();
    const auto& ap = a.pattern();
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 0.0;
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 q = ap.outer_ptr[c]; q < ap.outer_ptr[c + 1]; ++q)
        {
            b[ap.inner_idx[q]] += av[q] * xt[c];
        }
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        bsave[i] = b[i];
    }
    REQUIRE(ldlt.solve({b.data(), n}));
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::abs(b[i] - xt[i]));
    }
    CHECK(err < 1e-9); // the blocked factor + solve recover x_true ⇒ blocked path is correct
}

TEST_CASE("v5d-perf MultifrontalLDLt: big-front blocked path bit-identical across {1,2,4} workers",
          "[hesap][direct][v5d][v5d-perf][ldlt]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 27);
        // Block-diagonal of 4 independent 140×140 SPD blocks ⇒ 4 BIG concurrent fronts, each blocked ⇒ the
        // blocked path is exercised UNDER parallelism; L must be bit-identical across worker counts (the moat).
        const crd::u32 nb = 4;
        const crd::u32 bs = 140;
        const crd::u32 n = nb * bs;
        sp::TripletBuilder<double> tb(&alloc, n, n);
        for (crd::u32 b = 0; b < nb; ++b)
        {
            const crd::u32 o = b * bs;
            for (crd::u32 i = 0; i < bs; ++i)
            {
                for (crd::u32 j = 0; j < bs; ++j)
                {
                    tb.add(o + i, o + j, (i == j) ? static_cast<double>(bs + 1) : 1.0);
                }
            }
        }
        auto a = sp::to_csc<double>(tb.compress(), &alloc);
        dir::MultifrontalLDLT<double> f1(&alloc);
        f1.factorize(a, 1);
        REQUIRE(f1.info() == 0);
        REQUIRE(f1.front_count() >= nb);
        const auto x1 = f1.l_values();
        for (crd::u32 nw : {2U, 4U})
        {
            dir::MultifrontalLDLT<double> fp(&alloc);
            fp.factorize(a, nw);
            REQUIRE(fp.info() == 0);
            const auto xp = fp.l_values();
            bool ident = (x1.size() == xp.size());
            for (crd::usize i = 0; ident && i < x1.size(); ++i)
            {
                ident = (x1[i] == xp[i]); // blocked front factor bit-identical across worker counts (the moat)
            }
            CHECK(ident);
        }
    }
    crd::jobs::shutdown();
}

// =======================================================================
// v5d-h — DELAYED PIVOTS (Duff-Reid): genuinely-indefinite fronts whose fully-summed block cannot be
// stably eliminated in-place relay the offending pivots to their parent. Exercised with a 3D shifted
// Laplacian A−σ·I (the case that USED to FAIL with info!=0 before delayed pivots existed).
// =======================================================================

namespace
{
// 3D 7-point Laplacian on a k³ grid, diagonal shifted by −σ ⇒ INDEFINITE (σ=3: diag 3 < Σ|offdiag|=6).
// Same big dense fronts as the SPD 3D problem, but the per-front Bunch-Kaufman MUST delay pivots whose
// stabilizing entries live in not-yet-assembled contribution rows. Returns a symmetric CSC matrix.
sp::SparseMatrix<double, sp::SparseFormat::Csc> indef_laplacian_3d(crd::u32 k, double sigma,
                                                                   crd::memory::IAllocator* alloc)
{
    const crd::u32 n = k * k * k;
    auto id = [k](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * k + j) * k + l; };
    sp::TripletBuilder<double> tb(alloc, n, n);
    for (crd::u32 i = 0; i < k; ++i)
    {
        for (crd::u32 j = 0; j < k; ++j)
        {
            for (crd::u32 l = 0; l < k; ++l)
            {
                const crd::u32 d = id(i, j, l);
                tb.add(d, d, 6.0 - sigma);
                if (i + 1 < k) { tb.add(d, id(i + 1, j, l), -1.0); tb.add(id(i + 1, j, l), d, -1.0); }
                if (j + 1 < k) { tb.add(d, id(i, j + 1, l), -1.0); tb.add(id(i, j + 1, l), d, -1.0); }
                if (l + 1 < k) { tb.add(d, id(i, j, l + 1), -1.0); tb.add(id(i, j, l + 1), d, -1.0); }
            }
        }
    }
    return sp::to_csc<double>(tb.compress(), alloc);
}

// Solve residual ‖A·x − b‖∞ for b = A·x_true (x_true ramped), via the factored ldlt.
double indef_solve_residual(const sp::SparseMatrix<double, sp::SparseFormat::Csc>& a,
                            const dir::MultifrontalLDLT<double>& ldlt, crd::u32 n, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<double> xt(alloc);
    crd::containers::Array<double> b(alloc);
    crd::containers::Array<double> bsave(alloc);
    xt.resize(n);
    b.resize(n);
    bsave.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 1.0 + 0.5 * static_cast<double>(i % 7);
        b[i] = 0.0;
    }
    const double* av = a.values().values.data();
    const auto& ap = a.pattern();
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 q = ap.outer_ptr[c]; q < ap.outer_ptr[c + 1]; ++q)
        {
            b[ap.inner_idx[q]] += av[q] * xt[c];
        }
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        bsave[i] = b[i];
    }
    REQUIRE(ldlt.solve({b.data(), n})); // b ← x
    double err = 0.0;
    // residual ‖A·x − bsave‖∞
    crd::containers::Array<double> ax(alloc);
    ax.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        ax[i] = 0.0;
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 q = ap.outer_ptr[c]; q < ap.outer_ptr[c + 1]; ++q)
        {
            ax[ap.inner_idx[q]] += av[q] * b[c];
        }
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        err = std::max(err, std::abs(ax[i] - bsave[i]));
    }
    return err;
}
} // namespace

TEST_CASE("v5d-h MultifrontalLDLt: big indefinite (3D shifted Laplacian) factors via delayed pivots",
          "[hesap][direct][v5d][v5d-h][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // This matrix returned info!=0 (delayed pivot unsupported) before v5d-h. It MUST now factor correctly.
    for (crd::u32 k : {3U, 4U})
    {
        const crd::u32 n = k * k * k;
        auto a = indef_laplacian_3d(k, 3.0, &alloc);
        dir::MultifrontalLDLT<double> ldlt(&alloc);
        ldlt.set_pivot_threshold(0.6403882032022075); // textbook BK + fundamental fronts (relax=1) ⇒ this matrix
        ldlt.set_amalgamation_relax(1);               // DOES delay; the perf default (0.001 + amalgamation)
                                                      // deliberately avoids delays — that path is the bench's.
        ldlt.factorize(a);
        REQUIRE(ldlt.info() == 0);          // delayed pivots resolve it (was a hard FAIL pre-v5d-h)
        REQUIRE(ldlt.delayed_count() > 0);  // delays GENUINELY occurred (else this proves nothing about delay)
        const double res = indef_solve_residual(a, ldlt, n, &alloc);
        CHECK(res < 1e-9);                  // and the factor is numerically correct
    }
}

TEST_CASE("v5d-h MultifrontalLDLt: delayed-pivot factorization is bit-identical across {1,2,4,8} workers",
          "[hesap][direct][v5d][v5d-h][ldlt]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 25);
        // A delay-TRIGGERING matrix factored at several worker counts: delays are value-dependent decisions, so
        // the moat holds ONLY because each front is assembled in fixed postorder ⇒ its delay set is a pure
        // function of the pattern. Prove it on a case that actually delays (the SPD/complex moat cases never do).
        auto a = indef_laplacian_3d(4, 3.0, &alloc); // n=64, multifrontal, genuinely indefinite
        dir::MultifrontalLDLT<double> f1(&alloc);
        f1.set_pivot_threshold(0.6403882032022075); // textbook BK + fundamental fronts ⇒ this matrix delays
        f1.set_amalgamation_relax(1);               // (the moat-under-delay point)
        f1.factorize(a, 1);
        REQUIRE(f1.info() == 0);
        REQUIRE(f1.delayed_count() > 0); // delays must occur for this test to mean anything
        REQUIRE(f1.front_count() > 1);   // a genuine multifrontal walk
        for (crd::u32 nw : {2U, 4U, 8U})
        {
            dir::MultifrontalLDLT<double> fp(&alloc);
            fp.set_pivot_threshold(0.6403882032022075); // SAME threshold + fronts as f1 ⇒ compare like-for-like
            fp.set_amalgamation_relax(1);
            fp.factorize(a, nw);
            REQUIRE(fp.info() == 0);
            REQUIRE(fp.delayed_count() == f1.delayed_count()); // same delays regardless of worker count
            const bool ident = span_eq<crd::u32>(f1.l_col_ptr(), fp.l_col_ptr()) &&
                               span_eq<crd::u32>(f1.l_row_idx(), fp.l_row_idx()) &&
                               span_eq<double>(f1.l_values(), fp.l_values()) &&
                               span_eq<double>(f1.d_diag(), fp.d_diag()) &&
                               span_eq<double>(f1.d_offdiag(), fp.d_offdiag()) &&
                               span_eq<crd::u8>(f1.block_kinds(), fp.block_kinds()) &&
                               span_eq<crd::u32>(f1.perm(), fp.perm());
            CHECK(ident); // L/D/perm bit-identical UNDER DELAYS across worker counts — the determinism moat
        }
    }
    crd::jobs::shutdown();
}

namespace
{
// A dense INDEFINITE symmetric matrix: ZERO diagonal ⇒ no 1×1 pivot is ever stable ⇒ Bunch-Kaufman MUST take
// 2×2 pivots; a strong super-diagonal coupling (=1) pairs (i,i+1) into well-conditioned 2×2 blocks (det = −1),
// with a weaker decaying background (0.2/(1+|i-j|)) coupling the blocks ⇒ nonsingular indefinite. With nblk=1,
// bs ≥ 128 ⇒ a SINGLE dense front (no CB ⇒ no delay) via the BLOCKED-BK path ⇒ exercises the blocked 2×2 +
// flush logic. With nblk>1 ⇒ nblk independent big indefinite fronts ⇒ the blocked kernel under parallelism.
sp::SparseMatrix<double, sp::SparseFormat::Csc> dense_indef_blocks(crd::u32 nblk, crd::u32 bs,
                                                                   crd::memory::IAllocator* alloc)
{
    const crd::u32 n = nblk * bs;
    sp::TripletBuilder<double> tb(alloc, n, n);
    for (crd::u32 b = 0; b < nblk; ++b)
    {
        const crd::u32 o = b * bs;
        for (crd::u32 i = 0; i < bs; ++i)
        {
            tb.add(o + i, o + i, 0.0); // zero diagonal ⇒ forces 2×2
            for (crd::u32 j = i + 1; j < bs; ++j)
            {
                const double v = (j == i + 1) ? 1.0 : 0.2 / (1.0 + static_cast<double>(j - i));
                tb.add(o + i, o + j, v);
                tb.add(o + j, o + i, v);
            }
        }
    }
    return sp::to_csc<double>(tb.compress(), alloc);
}
} // namespace

TEST_CASE("v5d-h MultifrontalLDLt: big MULTIFRONT indefinite solves (default settings + relaxed-front amalgamation)",
          "[hesap][direct][v5d][v5d-h][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    auto a = indef_laplacian_3d(8, 3.0, &alloc); // n=512, genuinely multifrontal + indefinite
    const crd::u32 n = 512;
    // (1) Default settings (α=0.001 + IR) on a real multifrontal tree.
    {
        dir::MultifrontalLDLT<double> ldlt(&alloc);
        ldlt.factorize(a);
        REQUIRE(ldlt.info() == 0);
        REQUIRE(ldlt.front_count() > 1); // genuinely multifrontal
        CHECK(indef_solve_residual(a, ldlt, n, &alloc) < 1e-9);
    }
    // (2) Relaxed-front amalgamation ON (the EXPERIMENTAL knob, default-off): reconstruction-correct here (the
    // amalgamate_fronts merged structure still factors + solves). It explodes fill on bigger 3D (the OOM that
    // gates it off by default — needs the CHOLMOD zrelax incremental merge); kept tested at this safe scale.
    {
        dir::MultifrontalLDLT<double> ldlt(&alloc);
        ldlt.set_amalgamation_relax(32);
        ldlt.factorize(a);
        REQUIRE(ldlt.info() == 0);
        CHECK(indef_solve_residual(a, ldlt, n, &alloc) < 1e-9);
    }
}

TEST_CASE("v5d-h MultifrontalLDLt: big dense INDEFINITE front uses the blocked-BK path (2x2 + flush)",
          "[hesap][direct][v5d][v5d-h][v5d-perf][ldlt]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const crd::u32 n = 160; // > kBlockMin(128) ⇒ a single dense front via the BLOCKED-BK kernel
    auto a = dense_indef_blocks(1, n, &alloc);
    dir::MultifrontalLDLT<double> ldlt(&alloc);
    ldlt.factorize(a);
    REQUIRE(ldlt.info() == 0);
    REQUIRE(ldlt.front_count() == 1); // single dense front (npiv=160 ≥ block_min) ⇒ the blocked-BK path
    bool has2 = false;
    const auto bkc = ldlt.block_kinds();
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (bkc[i] == 2U)
        {
            has2 = true;
        }
    }
    REQUIRE(has2);                                              // 2×2 pivots ran inside the blocked panel
    const double res = indef_solve_residual(a, ldlt, n, &alloc);
    CHECK(res < 1e-9);                                         // the blocked indefinite factor is correct
}

TEST_CASE("v5d-h MultifrontalLDLt: blocked-BK indefinite fronts bit-identical across {1,2,4} workers",
          "[hesap][direct][v5d][v5d-h][ldlt]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 27);
        // 3 independent 140×140 dense indefinite blocks ⇒ 3 concurrent BIG fronts, each via the blocked-BK
        // 2×2 path ⇒ the blocked indefinite kernel is exercised UNDER parallelism; L/D/perm must be identical.
        auto a = dense_indef_blocks(3, 140, &alloc);
        dir::MultifrontalLDLT<double> f1(&alloc);
        f1.factorize(a, 1);
        REQUIRE(f1.info() == 0);
        REQUIRE(f1.front_count() >= 3);
        for (crd::u32 nw : {2U, 4U})
        {
            dir::MultifrontalLDLT<double> fp(&alloc);
            fp.factorize(a, nw);
            REQUIRE(fp.info() == 0);
            const bool ident = span_eq<crd::u32>(f1.l_col_ptr(), fp.l_col_ptr()) &&
                               span_eq<crd::u32>(f1.l_row_idx(), fp.l_row_idx()) &&
                               span_eq<double>(f1.l_values(), fp.l_values()) &&
                               span_eq<double>(f1.d_diag(), fp.d_diag()) &&
                               span_eq<double>(f1.d_offdiag(), fp.d_offdiag()) &&
                               span_eq<crd::u8>(f1.block_kinds(), fp.block_kinds()) &&
                               span_eq<crd::u32>(f1.perm(), fp.perm());
            CHECK(ident); // the BLOCKED indefinite kernel is deterministic across worker counts
        }
    }
    crd::jobs::shutdown();
}
