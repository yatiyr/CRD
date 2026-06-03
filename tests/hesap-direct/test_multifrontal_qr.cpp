// crd-hesap-direct v5c-1a — Multifrontal QR SYMBOLIC structural tests.
//
// Validates the front structure (= chol(AᵀA) supernodes), the assembly tree,
// the leftmost-column row merge, and the AᵀA two-path cross-check. The numeric
// `numeric-R ⊆ predicted` relation lands in v5c-1b (needs the dense front QR).

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/multifrontal_qr.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <initializer_list>

namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;
namespace dense = crd::hesap::dense;

namespace
{
// Build a CSC pattern from a row-major m×n 0/1 mask (nonzero = structural entry).
sp::SparsePattern csc_from_mask(crd::u32 m, crd::u32 n, std::initializer_list<int> mask, crd::memory::IAllocator* alloc)
{
    REQUIRE(mask.size() == static_cast<crd::usize>(m) * n);
    const int* d = mask.begin();
    sp::SparsePattern p(alloc);
    p.rows = m;
    p.cols = n;
    p.format = sp::SparseFormat::Csc;
    p.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 cnt = 0;
        for (crd::u32 i = 0; i < m; ++i)
        {
            if (d[static_cast<crd::usize>(i) * n + k] != 0)
            {
                ++cnt;
            }
        }
        p.outer_ptr[k + 1] = p.outer_ptr[k] + cnt;
    }
    p.inner_idx.resize(p.outer_ptr[n]);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 w = p.outer_ptr[k];
        for (crd::u32 i = 0; i < m; ++i)
        {
            if (d[static_cast<crd::usize>(i) * n + k] != 0)
            {
                p.inner_idx[w++] = i;
            }
        }
    }
    p.recompute_topology_hash();
    return p;
}

// Run the full battery of structural invariants on the QR symbolic of A.
void validate_symbolic(const sp::SparsePattern& a, crd::memory::IAllocator* alloc)
{
    const crd::u32 m = a.rows;
    const crd::u32 n = a.cols;
    dir::QrSymbolic s = dir::multifrontal_qr_symbolic(a, alloc);
    const crd::u32 nf = s.nf();

    // --- (A) AᵀA two-path cross-check (amalgamation-independent) ---
    sp::SparsePattern atap = dir::ata_pattern(a, alloc);
    REQUIRE(atap.rows == n);
    REQUIRE(atap.cols == n);
    ord::SymbolicFactor sf = ord::symbolic_factorize(atap, alloc, /*supernodal_patterns=*/false);
    crd::containers::Array<crd::u32> etree = ord::column_elimination_tree(a, alloc);
    crd::containers::Array<crd::u32> cnt = ord::column_counts_ata(a, {etree.data(), etree.size()}, alloc);
    // etrees from the explicit-AᵀA path and the implicit-ata path are bit-identical.
    REQUIRE(sf.parent.size() == etree.size());
    for (crd::u32 j = 0; j < n; ++j)
    {
        CHECK(sf.parent[j] == etree[j]);
    }
    // both count nnz(chol(AᵀA)) incl. the (full) diagonal.
    crd::u64 sum_sf = 0;
    crd::u64 sum_impl = 0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        sum_sf += sf.colcount[j];
        sum_impl += cnt[j];
    }
    CHECK(sum_sf == sum_impl);

    // --- front partition: pivot columns tile [0,n) exactly once, ascending ---
    REQUIRE(s.fronts.scol.size() == static_cast<crd::usize>(nf) + 1);
    CHECK(s.fronts.scol[0] == 0);
    CHECK(s.fronts.scol[nf] == n);
    crd::containers::Array<crd::u32> col_seen(alloc);
    col_seen.resize(n);
    for (crd::u32 j = 0; j < n; ++j)
    {
        col_seen[j] = 0;
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        CHECK(s.fronts.scol[f] < s.fronts.scol[f + 1]); // non-empty pivot range
        for (crd::u32 c = s.fronts.scol[f]; c < s.fronts.scol[f + 1]; ++c)
        {
            ++col_seen[c];
            CHECK(s.fronts.col_super[c] == f);
        }
        // front column set ascending + the first nc are exactly the pivot columns.
        const crd::u32 rb = s.fronts.srowp[f];
        const crd::u32 nrow = s.fronts.srowp[f + 1] - rb;
        const crd::u32 nc = s.fronts.scol[f + 1] - s.fronts.scol[f];
        REQUIRE(nrow >= nc);
        for (crd::u32 t = 0; t < nc; ++t)
        {
            CHECK(s.fronts.srow[rb + t] == s.fronts.scol[f] + t);
        }
        for (crd::u32 t = 1; t < nrow; ++t)
        {
            CHECK(s.fronts.srow[rb + t - 1] < s.fronts.srow[rb + t]); // strictly ascending
        }
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        CHECK(col_seen[j] == 1);
    }

    // --- assembly tree: contribution columns ⊆ parent front (the extend_add precondition) ---
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 nc = s.fronts.scol[f + 1] - s.fronts.scol[f];
        const crd::u32 rb = s.fronts.srowp[f];
        const crd::u32 nrow = s.fronts.srowp[f + 1] - rb;
        const crd::u32 par = s.front_parent[f];
        if (nc == nrow)
        {
            CHECK(par == ord::kNoParent); // no contribution block ⇒ a root
            continue;
        }
        REQUIRE(par != ord::kNoParent);
        REQUIRE(par < nf);
        CHECK(par > f); // parents are numbered after children (column order)
        // every contribution column of f is a column of the parent front.
        const crd::u32 pb = s.fronts.srowp[par];
        const crd::u32 pn = s.fronts.srowp[par + 1] - pb;
        crd::u32 q = 0;
        for (crd::u32 t = nc; t < nrow; ++t)
        {
            const crd::u32 gcol = s.fronts.srow[rb + t];
            while (q < pn && s.fronts.srow[pb + q] < gcol)
            {
                ++q;
            }
            const bool found = (q < pn && s.fronts.srow[pb + q] == gcol);
            CHECK(found);
        }
    }

    // --- postorder: every child precedes its parent ---
    REQUIRE(s.front_post.size() == nf);
    crd::containers::Array<crd::u32> post_pos(alloc);
    post_pos.resize(nf);
    crd::containers::Array<crd::u32> post_seen(alloc);
    post_seen.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        post_seen[f] = 0;
    }
    for (crd::u32 k = 0; k < nf; ++k)
    {
        const crd::u32 f = s.front_post[k];
        REQUIRE(f < nf);
        post_pos[f] = k;
        ++post_seen[f];
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        CHECK(post_seen[f] == 1); // a permutation
        if (s.front_parent[f] != ord::kNoParent)
        {
            CHECK(post_pos[f] < post_pos[s.front_parent[f]]);
        }
    }

    // --- row merge: every non-empty row assigned once, to its true leftmost column ---
    REQUIRE(s.sleft.size() == static_cast<crd::usize>(n) + 1);
    CHECK(s.sleft[0] == 0);
    const crd::u32 assigned = (n == 0) ? 0U : s.sleft[n];
    CHECK(static_cast<crd::u64>(assigned) + s.n_empty_rows == m);
    CHECK(s.row_by_leftcol.size() == assigned);
    crd::containers::Array<crd::u32> row_seen(alloc);
    row_seen.resize(m == 0 ? 1 : m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        row_seen[i] = 0;
    }
    // reference leftmost column per row, computed independently.
    crd::containers::Array<crd::u32> ref_left(alloc);
    ref_left.resize(m == 0 ? 1 : m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        ref_left[i] = n; // empty sentinel
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 p = a.outer_ptr[k]; p < a.outer_ptr[k + 1]; ++p)
        {
            const crd::u32 i = a.inner_idx[p];
            if (ref_left[i] == n)
            {
                ref_left[i] = k;
            }
        }
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 p = s.sleft[j]; p < s.sleft[j + 1]; ++p)
        {
            const crd::u32 i = s.row_by_leftcol[p];
            REQUIRE(i < m);
            ++row_seen[i];
            CHECK(ref_left[i] == j); // bucketed under its true leftmost column
        }
    }
    crd::u32 empties = 0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        if (ref_left[i] == n)
        {
            CHECK(row_seen[i] == 0);
            ++empties;
        }
        else
        {
            CHECK(row_seen[i] == 1);
        }
    }
    CHECK(empties == s.n_empty_rows);
}
} // namespace

TEST_CASE("v5c-1a ata_pattern: brute-force AtA, symmetric + full diagonal", "[hesap][direct][v5c][v5c-1a][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // A (4×3):
    //   col0: rows {0,1}   col1: rows {1,2}   col2: rows {2,3}
    sp::SparsePattern a = csc_from_mask(4, 3,
                                        {
                                            1, 0, 0, // row0
                                            1, 1, 0, // row1
                                            0, 1, 1, // row2
                                            0, 0, 1, // row3
                                        },
                                        &alloc);
    sp::SparsePattern ata = dir::ata_pattern(a, &alloc);
    REQUIRE(ata.rows == 3);
    REQUIRE(ata.cols == 3);
    // AᵀA pattern: col0 shares row1 with col1 ⇒ {0,1}; col1 shares with col0,col2 ⇒ {0,1,2};
    // col2 shares with col1 ⇒ {1,2}. Symmetric, diagonal present.
    auto colset = [&](crd::u32 k, std::initializer_list<crd::u32> want)
    {
        const crd::u32 b = ata.outer_ptr[k];
        const crd::u32 e = ata.outer_ptr[k + 1];
        REQUIRE(e - b == want.size());
        crd::u32 t = 0;
        for (crd::u32 v : want)
        {
            CHECK(ata.inner_idx[b + t++] == v);
        }
    };
    colset(0, {0, 1});
    colset(1, {0, 1, 2});
    colset(2, {1, 2});
}

TEST_CASE("v5c-1a QR symbolic: square banded", "[hesap][direct][v5c][v5c-1a][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 6×6 tridiagonal-ish (each col touches itself + neighbours).
    sp::SparsePattern a = csc_from_mask(6, 6,
                                        {
                                            1, 1, 0, 0, 0, 0, //
                                            1, 1, 1, 0, 0, 0, //
                                            0, 1, 1, 1, 0, 0, //
                                            0, 0, 1, 1, 1, 0, //
                                            0, 0, 0, 1, 1, 1, //
                                            0, 0, 0, 0, 1, 1, //
                                        },
                                        &alloc);
    validate_symbolic(a, &alloc);
}

TEST_CASE("v5c-1a QR symbolic: rectangular over-determined (m>n)", "[hesap][direct][v5c][v5c-1a][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 8×4, over-determined, irregular (non-strong-Hall in places).
    sp::SparsePattern a = csc_from_mask(8, 4,
                                        {
                                            1, 0, 0, 1, //
                                            1, 1, 0, 0, //
                                            0, 1, 1, 0, //
                                            0, 0, 1, 1, //
                                            1, 0, 0, 0, //
                                            0, 1, 0, 1, //
                                            0, 0, 1, 0, //
                                            1, 0, 0, 1, //
                                        },
                                        &alloc);
    validate_symbolic(a, &alloc);
}

TEST_CASE("v5c-1a QR symbolic: unsymmetric + empty row", "[hesap][direct][v5c][v5c-1a][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 6×4 with row 4 entirely empty (no nonzero) — must be parked, no OOB.
    sp::SparsePattern a = csc_from_mask(6, 4,
                                        {
                                            1, 0, 1, 0, //
                                            1, 1, 0, 0, //
                                            0, 1, 0, 1, //
                                            0, 0, 1, 1, //
                                            0, 0, 0, 0, // empty row
                                            1, 0, 0, 1, //
                                        },
                                        &alloc);
    dir::QrSymbolic s = dir::multifrontal_qr_symbolic(a, &alloc);
    CHECK(s.n_empty_rows == 1);
    validate_symbolic(a, &alloc);
}

TEST_CASE("v5c-1a QR symbolic: deterministic (bit-identical re-runs)", "[hesap][direct][v5c][v5c-1a][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    sp::SparsePattern a = csc_from_mask(7, 5,
                                        {
                                            1, 0, 0, 0, 1, //
                                            1, 1, 0, 0, 0, //
                                            0, 1, 1, 0, 0, //
                                            0, 0, 1, 1, 0, //
                                            0, 0, 0, 1, 1, //
                                            1, 0, 1, 0, 0, //
                                            0, 1, 0, 1, 0, //
                                        },
                                        &alloc);
    dir::QrSymbolic s1 = dir::multifrontal_qr_symbolic(a, &alloc);
    dir::QrSymbolic s2 = dir::multifrontal_qr_symbolic(a, &alloc);
    REQUIRE(s1.nf() == s2.nf());
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
    CHECK(same(s1.fronts.scol, s2.fronts.scol));
    CHECK(same(s1.fronts.srowp, s2.fronts.srowp));
    CHECK(same(s1.fronts.srow, s2.fronts.srow));
    CHECK(same(s1.front_parent, s2.front_parent));
    CHECK(same(s1.front_post, s2.front_post));
    CHECK(same(s1.sleft, s2.sleft));
    CHECK(same(s1.row_by_leftcol, s2.row_by_leftcol));
}

// ---------------------------------------------------------------------------
// v5c-1e — AᵀA-free implicit symbolic. `symbolic_factorize_ata(A)` (never forms
// AᵀA) must be BIT-FOR-BIT identical to the explicit oracle
// `symbolic_factorize(ata_pattern(A), supernodal_patterns=true)`. Verified-by-
// oracle ⇒ the numeric/solve cannot diverge (they consume only this symbolic).
// ---------------------------------------------------------------------------
namespace
{
void check_ata_oracle(const sp::SparsePattern& a, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.cols;
    sp::SparsePattern atap = dir::ata_pattern(a, alloc);
    ord::SymbolicFactor expl = ord::symbolic_factorize(atap, alloc, /*supernodal_patterns=*/true);
    ord::SymbolicFactor impl = ord::symbolic_factorize_ata(a, alloc);

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

    REQUIRE(impl.n == n);
    REQUIRE(impl.nsuper == expl.nsuper);
    CHECK(same(impl.parent, expl.parent));
    CHECK(same(impl.post, expl.post));
    CHECK(same(impl.colcount, expl.colcount));
    CHECK(same(impl.lp, expl.lp));
    CHECK(same(impl.super, expl.super));
    CHECK(same(impl.slead_ptr, expl.slead_ptr)); // the QR-front column structure (the new emit)
    CHECK(same(impl.slead_idx, expl.slead_idx)); // ascending, diagonal-first — where bit-identity breaks
}
} // namespace

TEST_CASE("v5c-1e implicit ata symbolic: bit-identical to explicit oracle", "[hesap][direct][v5c][v5c-1e][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    SECTION("square banded")
    {
        sp::SparsePattern a = csc_from_mask(6, 6,
                                            {
                                                1, 1, 0, 0, 0, 0, //
                                                1, 1, 1, 0, 0, 0, //
                                                0, 1, 1, 1, 0, 0, //
                                                0, 0, 1, 1, 1, 0, //
                                                0, 0, 0, 1, 1, 1, //
                                                0, 0, 0, 0, 1, 1, //
                                            },
                                            &alloc);
        check_ata_oracle(a, &alloc);
    }
    SECTION("rectangular over-determined (m>n), non-strong-Hall")
    {
        sp::SparsePattern a = csc_from_mask(8, 4,
                                            {
                                                1, 0, 0, 1, //
                                                1, 1, 0, 0, //
                                                0, 1, 1, 0, //
                                                0, 0, 1, 1, //
                                                1, 0, 0, 0, //
                                                0, 1, 0, 1, //
                                                0, 0, 1, 0, //
                                                1, 0, 0, 1, //
                                            },
                                            &alloc);
        check_ata_oracle(a, &alloc);
    }
    SECTION("unsymmetric + empty row")
    {
        sp::SparsePattern a = csc_from_mask(6, 4,
                                            {
                                                1, 0, 1, 0, //
                                                1, 1, 0, 0, //
                                                0, 1, 0, 1, //
                                                0, 0, 1, 1, //
                                                0, 0, 0, 0, // empty row
                                                1, 0, 0, 1, //
                                            },
                                            &alloc);
        check_ata_oracle(a, &alloc);
    }
    SECTION("denser — wide rows exercise the adj gather + amalgamation")
    {
        sp::SparsePattern a = csc_from_mask(7, 7,
                                            {
                                                1, 1, 1, 0, 0, 0, 1, //
                                                1, 1, 0, 1, 0, 0, 0, //
                                                0, 1, 1, 1, 1, 0, 0, //
                                                1, 0, 1, 1, 0, 1, 0, //
                                                0, 0, 1, 0, 1, 1, 1, //
                                                0, 1, 0, 1, 1, 1, 0, //
                                                1, 0, 0, 0, 1, 0, 1, //
                                            },
                                            &alloc);
        check_ata_oracle(a, &alloc);
    }
}

// ---------------------------------------------------------------------------
// v5c-1b — numeric factor. R from QR(A) is the Cholesky factor of AᵀA, so
// RᵀR == AᵀA (sign- and identity-P_c-robust) is the factor's correctness oracle.
// ---------------------------------------------------------------------------
namespace
{
// Build a CSC pattern + parallel values from a row-major m×n dense matrix (zeros dropped).
sp::SparsePattern csc_from_dense(crd::u32 m, crd::u32 n, std::initializer_list<double> dense,
                                 crd::containers::Array<crd::f64>& vals, crd::memory::IAllocator* alloc)
{
    REQUIRE(dense.size() == static_cast<crd::usize>(m) * n);
    const double* d = dense.begin();
    sp::SparsePattern p(alloc);
    p.rows = m;
    p.cols = n;
    p.format = sp::SparseFormat::Csc;
    p.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 cnt = 0;
        for (crd::u32 i = 0; i < m; ++i)
        {
            if (d[static_cast<crd::usize>(i) * n + k] != 0.0)
            {
                ++cnt;
            }
        }
        p.outer_ptr[k + 1] = p.outer_ptr[k] + cnt;
    }
    p.inner_idx.resize(p.outer_ptr[n]);
    vals.resize(p.outer_ptr[n]);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 w = p.outer_ptr[k];
        for (crd::u32 i = 0; i < m; ++i)
        {
            const double v = d[static_cast<crd::usize>(i) * n + k];
            if (v != 0.0)
            {
                p.inner_idx[w] = i;
                vals[w] = v;
                ++w;
            }
        }
    }
    p.recompute_topology_hash();
    return p;
}

// Factor A, then assert ‖RᵀR − AᵀA‖_max < tol (dense, with values).
void check_rtr_equals_ata(crd::u32 m, crd::u32 n, const sp::SparsePattern& a, crd::containers::ConstSpan<crd::f64> vals,
                          crd::memory::IAllocator* alloc)
{
    dir::MultifrontalQR<crd::f64> qr = dir::factor_multifrontal_qr<crd::f64>(a, vals, alloc);
    REQUIRE(qr.info() == 0); // full column rank
    REQUIRE(qr.n() == n);
    REQUIRE(qr.rows() == m);

    // dense AᵀA from the CSC values.
    crd::containers::Array<crd::f64> ata(alloc);
    ata.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize z = 0; z < ata.size(); ++z)
    {
        ata[z] = 0.0;
    }
    // accumulate AᵀA(j,k) = Σ_i A(i,j)·A(i,k) via a per-row dense scatter.
    crd::containers::Array<crd::f64> rowbuf(alloc);
    rowbuf.resize(static_cast<crd::usize>(n));
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            rowbuf[j] = 0.0;
        }
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
            {
                if (a.inner_idx[q] == i)
                {
                    rowbuf[k] = vals[q];
                }
            }
        }
        for (crd::u32 j = 0; j < n; ++j)
        {
            if (rowbuf[j] != 0.0)
            {
                for (crd::u32 k = 0; k < n; ++k)
                {
                    ata[static_cast<crd::usize>(j) * n + k] += rowbuf[j] * rowbuf[k];
                }
            }
        }
    }

    // dense RᵀR from the CSR R (row r ascending columns).
    crd::containers::Array<crd::f64> rtr(alloc);
    rtr.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize z = 0; z < rtr.size(); ++z)
    {
        rtr[z] = 0.0;
    }
    const auto& rp = qr.rp();
    const auto& rj = qr.rj();
    const auto& rx = qr.rx();
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 a1 = rp[r]; a1 < rp[r + 1]; ++a1)
        {
            const crd::u32 j = rj[a1];
            CHECK(j >= r); // R is upper-triangular (row r = pivot column r)
            for (crd::u32 a2 = rp[r]; a2 < rp[r + 1]; ++a2)
            {
                rtr[static_cast<crd::usize>(j) * n + rj[a2]] += rx[a1] * rx[a2];
            }
        }
    }

    crd::f64 maxerr = 0.0;
    crd::f64 maxata = 1.0;
    for (crd::usize z = 0; z < ata.size(); ++z)
    {
        const crd::f64 e = (rtr[z] > ata[z]) ? (rtr[z] - ata[z]) : (ata[z] - rtr[z]);
        if (e > maxerr)
        {
            maxerr = e;
        }
        const crd::f64 av = (ata[z] < 0.0) ? -ata[z] : ata[z];
        if (av > maxata)
        {
            maxata = av;
        }
    }
    CHECK(maxerr < 1e-9 * maxata);
}
} // namespace

TEST_CASE("v5c-1b QR factor: RtR == AtA, square full-rank", "[hesap][direct][v5c][v5c-1b][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<crd::f64> vals(&alloc);
    // 5×5 diagonally-dominant banded ⇒ full column rank.
    sp::SparsePattern a = csc_from_dense(5, 5,
                                         {
                                             4.0,  -1.0, 0.0,  0.0,  0.5,  //
                                             -1.0, 4.0,  -1.0, 0.0,  0.0,  //
                                             0.0,  -1.0, 4.0,  -1.0, 0.0,  //
                                             0.0,  0.0,  -1.0, 4.0,  -1.0, //
                                             0.3,  0.0,  0.0,  -1.0, 4.0,  //
                                         },
                                         vals, &alloc);
    check_rtr_equals_ata(5, 5, a, {vals.data(), vals.size()}, &alloc);
}

TEST_CASE("v5c-1b QR factor: RtR == AtA, rectangular over-determined", "[hesap][direct][v5c][v5c-1b][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<crd::f64> vals(&alloc);
    // 7×4, full column rank, irregular sparsity.
    sp::SparsePattern a = csc_from_dense(7, 4,
                                         {
                                             2.0, 0.0, 0.0, 1.0, //
                                             1.0, 3.0, 0.0, 0.0, //
                                             0.0, 1.0, 2.0, 0.0, //
                                             0.0, 0.0, 1.0, 3.0, //
                                             1.5, 0.0, 0.0, 0.0, //
                                             0.0, 2.0, 0.0, 1.0, //
                                             0.0, 0.0, 1.0, 0.0, //
                                         },
                                         vals, &alloc);
    check_rtr_equals_ata(7, 4, a, {vals.data(), vals.size()}, &alloc);
}

TEST_CASE("v5c-1b QR factor: RtR == AtA, larger banded over-determined", "[hesap][direct][v5c][v5c-1b][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    crd::containers::Array<crd::f64> vals(&alloc);
    // 12×8 banded (each column hits 3 rows), full column rank by construction.
    const crd::u32 m = 12;
    const crd::u32 n = 8;
    sp::SparsePattern a(&alloc);
    a.rows = m;
    a.cols = n;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    // column k touches rows k, k+1, k+4 (clamped) with distinct values.
    crd::containers::Array<crd::u32> rowbuf(&alloc);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 r0 = k;
        crd::u32 r1 = k + 1;
        crd::u32 r2 = k + 4;
        crd::u32 cnt = 0;
        if (r0 < m)
        {
            ++cnt;
        }
        if (r1 < m)
        {
            ++cnt;
        }
        if (r2 < m)
        {
            ++cnt;
        }
        a.outer_ptr[k + 1] = a.outer_ptr[k] + cnt;
    }
    a.inner_idx.resize(a.outer_ptr[n]);
    vals.resize(a.outer_ptr[n]);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 w = a.outer_ptr[k];
        const crd::u32 rs[3] = {k, k + 1, k + 4};
        const crd::f64 vs[3] = {5.0, -1.0, -1.0};
        for (crd::u32 t = 0; t < 3; ++t)
        {
            if (rs[t] < m)
            {
                a.inner_idx[w] = rs[t];
                vals[w] = vs[t];
                ++w;
            }
        }
    }
    a.recompute_topology_hash();
    check_rtr_equals_ata(m, n, a, {vals.data(), vals.size()}, &alloc);
}

// ---------------------------------------------------------------------------
// v5c-1c — solve (square) + least_squares (m ≥ n).
// ---------------------------------------------------------------------------
namespace
{
// y = A·x  (A CSC m×n, values parallel to inner_idx).
void spmv(const sp::SparsePattern& a, crd::containers::ConstSpan<crd::f64> v, const crd::f64* x, crd::f64* y,
          crd::u32 m, crd::u32 n)
{
    for (crd::u32 i = 0; i < m; ++i)
    {
        y[i] = 0.0;
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            y[a.inner_idx[q]] += v[q] * x[k];
        }
    }
}
// r = Aᵀ·g  (g length m → r length n).
void atv(const sp::SparsePattern& a, crd::containers::ConstSpan<crd::f64> v, const crd::f64* g, crd::f64* r, crd::u32 m,
         crd::u32 n)
{
    (void)m;
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            acc += v[q] * g[a.inner_idx[q]];
        }
        r[k] = acc;
    }
}
[[nodiscard]] crd::f64 norm2(const crd::f64* z, crd::u32 len)
{
    crd::f64 s = 0.0;
    for (crd::u32 i = 0; i < len; ++i)
    {
        s += z[i] * z[i];
    }
    return std::sqrt(s);
}
} // namespace

TEST_CASE("v5c-1c QR solve: square Ax = b recovers x_true", "[hesap][direct][v5c][v5c-1c][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<crd::f64> vals(&alloc);
    sp::SparsePattern a = csc_from_dense(5, 5,
                                         {
                                             4.0,  -1.0, 0.0,  0.0,  0.5,  //
                                             -1.0, 4.0,  -1.0, 0.0,  0.0,  //
                                             0.0,  -1.0, 4.0,  -1.0, 0.0,  //
                                             0.0,  0.0,  -1.0, 4.0,  -1.0, //
                                             0.3,  0.0,  0.0,  -1.0, 4.0,  //
                                         },
                                         vals, &alloc);
    dir::MultifrontalQR<crd::f64> qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    const crd::f64 xtrue[5] = {1.0, -2.0, 3.0, 0.5, -1.5};
    crd::f64 b[5];
    spmv(a, {vals.data(), vals.size()}, xtrue, b, 5, 5);
    crd::containers::Array<crd::f64> rhs(&alloc);
    rhs.resize(5);
    for (crd::u32 i = 0; i < 5; ++i)
    {
        rhs[i] = b[i];
    }
    REQUIRE(qr.solve({rhs.data(), rhs.size()}));
    crd::f64 err[5];
    for (crd::u32 i = 0; i < 5; ++i)
    {
        err[i] = rhs[i] - xtrue[i];
    }
    CHECK(norm2(err, 5) < 1e-9);
}

TEST_CASE("v5c-1c QR least_squares: over-determined consistent recovers x_true", "[hesap][direct][v5c][v5c-1c][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<crd::f64> vals(&alloc);
    sp::SparsePattern a = csc_from_dense(7, 4,
                                         {
                                             2.0, 0.0, 0.0, 1.0, //
                                             1.0, 3.0, 0.0, 0.0, //
                                             0.0, 1.0, 2.0, 0.0, //
                                             0.0, 0.0, 1.0, 3.0, //
                                             1.5, 0.0, 0.0, 0.0, //
                                             0.0, 2.0, 0.0, 1.0, //
                                             0.0, 0.0, 1.0, 0.0, //
                                         },
                                         vals, &alloc);
    dir::MultifrontalQR<crd::f64> qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    const crd::f64 xtrue[4] = {1.0, -1.0, 2.0, 0.5};
    crd::f64 b[7];
    spmv(a, {vals.data(), vals.size()}, xtrue, b, 7, 4);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(4);
    REQUIRE(qr.least_squares({b, 7}, {x.data(), x.size()}, 1));
    crd::f64 err[4];
    for (crd::u32 i = 0; i < 4; ++i)
    {
        err[i] = x[i] - xtrue[i];
    }
    CHECK(norm2(err, 4) < 1e-9);
}

TEST_CASE("v5c-1c QR least_squares: normal-equation optimality At(Ax-b)=0", "[hesap][direct][v5c][v5c-1c][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<crd::f64> vals(&alloc);
    sp::SparsePattern a = csc_from_dense(7, 4,
                                         {
                                             2.0, 0.0, 0.0, 1.0, //
                                             1.0, 3.0, 0.0, 0.0, //
                                             0.0, 1.0, 2.0, 0.0, //
                                             0.0, 0.0, 1.0, 3.0, //
                                             1.5, 0.0, 0.0, 0.0, //
                                             0.0, 2.0, 0.0, 1.0, //
                                             0.0, 0.0, 1.0, 0.0, //
                                         },
                                         vals, &alloc);
    dir::MultifrontalQR<crd::f64> qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    // arbitrary (generally inconsistent) RHS.
    const crd::f64 b[7] = {1.0, 2.0, -1.0, 0.5, 3.0, -2.0, 1.5};
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(4);
    REQUIRE(qr.least_squares({b, 7}, {x.data(), x.size()}, 1));
    // residual r = A·x − b; the LS optimum has Aᵀr = 0.
    crd::f64 ax[7];
    spmv(a, {vals.data(), vals.size()}, x.data(), ax, 7, 4);
    crd::f64 r[7];
    for (crd::u32 i = 0; i < 7; ++i)
    {
        r[i] = ax[i] - b[i];
    }
    crd::f64 atr[4];
    atv(a, {vals.data(), vals.size()}, r, atr, 7, 4);
    CHECK(norm2(atr, 4) < 1e-9);
}

TEST_CASE("v5c-1c QR solve: multi-RHS square", "[hesap][direct][v5c][v5c-1c][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    crd::containers::Array<crd::f64> vals(&alloc);
    sp::SparsePattern a = csc_from_dense(4, 4,
                                         {
                                             3.0, 1.0, 0.0, 0.0, //
                                             1.0, 3.0, 1.0, 0.0, //
                                             0.0, 1.0, 3.0, 1.0, //
                                             0.0, 0.0, 1.0, 3.0, //
                                         },
                                         vals, &alloc);
    dir::MultifrontalQR<crd::f64> qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    const crd::usize nrhs = 3;
    // column-major n×nrhs true solutions.
    const crd::f64 xt[12] = {1.0, 2.0, 3.0, 4.0, -1.0, 0.5, -2.0, 1.0, 0.0, 1.0, 0.0, -1.0};
    crd::containers::Array<crd::f64> rhs(&alloc);
    rhs.resize(4 * nrhs);
    for (crd::usize s = 0; s < nrhs; ++s)
    {
        crd::f64 col[4];
        spmv(a, {vals.data(), vals.size()}, &xt[s * 4], col, 4, 4);
        for (crd::u32 i = 0; i < 4; ++i)
        {
            rhs[s * 4 + i] = col[i];
        }
    }
    REQUIRE(qr.solve({rhs.data(), rhs.size()}, nrhs));
    crd::f64 err[12];
    for (crd::usize i = 0; i < 12; ++i)
    {
        err[i] = rhs[i] - xt[i];
    }
    CHECK(norm2(err, 12) < 1e-9);
}

// ---------------------------------------------------------------------------
// v5c-1d — blocked-WY front factor. A DENSE 150x100 matrix forms ONE 150-row
// front (>= block gate, npiv=100 => 3 compact-WY sub-panels), exercising the
// BLAS-3 trailing-update path (small fronts stay on the unblocked path).
// ---------------------------------------------------------------------------
namespace
{
sp::SparsePattern dense_tall(crd::u32 m, crd::u32 n, crd::containers::Array<crd::f64>& vals,
                             crd::memory::IAllocator* alloc)
{
    sp::SparsePattern a(alloc);
    a.rows = m;
    a.cols = n;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 k = 0; k < n; ++k)
    {
        a.outer_ptr[k + 1] = a.outer_ptr[k] + m; // dense column
    }
    a.inner_idx.resize(static_cast<crd::usize>(m) * n);
    vals.resize(static_cast<crd::usize>(m) * n);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 w = a.outer_ptr[k];
        for (crd::u32 i = 0; i < m; ++i)
        {
            a.inner_idx[w] = i;
            // strongly diagonal-dominant top block (well-conditioned) + small dense filler.
            crd::f64 v = 1.0 / (5.0 + i + k);
            if (i == k)
            {
                v = 50.0;
            }
            else if (i < n)
            {
                v = 1.0 / (3.0 + i + k);
            }
            vals[w] = v;
            ++w;
        }
    }
    a.recompute_topology_hash();
    return a;
}
} // namespace

TEST_CASE("v5c-1d blocked-WY: dense tall full-rank RtR == AtA", "[hesap][direct][v5c][v5c-1d][qr]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 26);
    crd::containers::Array<crd::f64> vals(&alloc);
    sp::SparsePattern a = dense_tall(150, 100, vals, &alloc);
    check_rtr_equals_ata(150, 100, a, {vals.data(), vals.size()}, &alloc);
}

TEST_CASE("v5c-1d blocked-WY: dense tall least_squares recovers x_true", "[hesap][direct][v5c][v5c-1d][qr]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 26);
    crd::containers::Array<crd::f64> vals(&alloc);
    const crd::u32 m = 150;
    const crd::u32 n = 100;
    sp::SparsePattern a = dense_tall(m, n, vals, &alloc);
    dir::MultifrontalQR<crd::f64> qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    crd::containers::Array<crd::f64> xt(&alloc);
    xt.resize(n);
    for (crd::u32 j = 0; j < n; ++j)
    {
        xt[j] = 1.0 + 0.01 * static_cast<crd::f64>(j);
    }
    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(m);
    spmv(a, {vals.data(), vals.size()}, xt.data(), b.data(), m, n);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    REQUIRE(qr.least_squares({b.data(), m}, {x.data(), x.size()}, 1));
    crd::containers::Array<crd::f64> err(&alloc);
    err.resize(n);
    for (crd::u32 j = 0; j < n; ++j)
    {
        err[j] = x[j] - xt[j];
    }
    CHECK(norm2(err.data(), n) < 1e-7);
}

// ---------------------------------------------------------------------------
// v5c tree-parallel — the DETERMINISM MOAT: R and the Householder vectors (hence
// the least-squares solution) are BIT-IDENTICAL across worker counts {1,2,4,8}.
// No sparse-QR library (SPQR/Eigen) carries cross-thread bit-exact factors. Uses a
// BLOCK-DIAGONAL matrix (K independent banded blocks ⇒ a level with K mutually
// independent fronts ⇒ the per-worker scratch isolation is genuinely exercised by
// concurrent fronts — the place a scratch-aliasing race would surface).
// ---------------------------------------------------------------------------
namespace
{
// K diagonal blocks; block b occupies rows [b*bh, b*bh+bh) × cols [b*bw, b*bw+bw). Within a block,
// column (local kk) touches rows kk, kk+1, kk+5 (banded, full column rank).
template <typename T>
sp::SparsePattern block_banded_ls(crd::u32 nblk, crd::u32 bh, crd::u32 bw, crd::containers::Array<T>& vals,
                                  crd::memory::IAllocator* alloc)
{
    const crd::u32 n = nblk * bw;
    const crd::u32 m = nblk * bh;
    auto rows_of = [&](crd::u32 k, crd::u32(&rs)[3]) -> crd::u32
    {
        const crd::u32 b = k / bw;
        const crd::u32 kk = k % bw;
        const crd::u32 cand[3] = {kk, kk + 1, kk + 5};
        crd::u32 cnt = 0;
        for (crd::u32 t = 0; t < 3; ++t)
        {
            if (cand[t] < bh)
            {
                rs[cnt++] = b * bh + cand[t];
            }
        }
        return cnt;
    };
    sp::SparsePattern a(alloc);
    a.rows = m;
    a.cols = n;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 rs[3];
        a.outer_ptr[k + 1] = a.outer_ptr[k] + rows_of(k, rs);
    }
    a.inner_idx.resize(a.outer_ptr[n]);
    vals.resize(a.outer_ptr[n]);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 rs[3];
        const crd::u32 c = rows_of(k, rs);
        const T vs[3] = {T{5}, T{-1}, T{-1}};
        crd::u32 w = a.outer_ptr[k];
        for (crd::u32 t = 0; t < c; ++t)
        {
            a.inner_idx[w] = rs[t];
            vals[w] = vs[t];
            ++w;
        }
    }
    a.recompute_topology_hash();
    return a;
}

template <typename T> void run_qr_parallel_moat(crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> vals(alloc);
    auto a = block_banded_ls<T>(/*nblk=*/4, /*bh=*/40, /*bw=*/20, vals, alloc); // 160×80, 4 independent arms
    const crd::u32 m = a.rows;
    const crd::u32 n = a.cols;

    // Serial reference factor.
    auto qr1 = dir::factor_multifrontal_qr<T>(a, {vals.data(), vals.size()}, alloc, dir::kQrFrontRelax, 1);
    REQUIRE(qr1.info() == 0);
    REQUIRE(qr1.symbolic().nf() >= 4); // ≥1 front per block ⇒ a level with ≥2 concurrent fronts (real parallelism)

    // x_true and a consistent RHS b = A·x_true ⇒ least-squares recovers x_true.
    crd::containers::Array<T> xt(alloc);
    crd::containers::Array<T> b(alloc);
    xt.resize(n);
    b.resize(m);
    for (crd::u32 j = 0; j < n; ++j)
    {
        // real-valued (the moat tests determinism, not conjugation); built at T's precision then promoted
        // to T so the complex instantiation gets Complex{rv,0} with no narrowing in arithmetic.
        const dense::RealType<T> rv = static_cast<dense::RealType<T>>(1.0 + 0.01 * static_cast<crd::f64>(j));
        xt[j] = static_cast<T>(rv);
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        b[i] = static_cast<T>(dense::RealType<T>{0});
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            b[a.inner_idx[q]] += vals[q] * xt[k];
        }
    }
    crd::containers::Array<T> x1(alloc);
    x1.resize(n);
    REQUIRE(qr1.least_squares({b.data(), m}, {x1.data(), n}, 1));

    const auto& rx1 = qr1.rx();
    const auto& rj1 = qr1.rj();
    // {1,2,4,8,16} workers (v5c-close moat). num_workers is the parallel_for hint; the per-worker scratch
    // is pool-sized, so 16 over-requests on a smaller pool (parallel_for caps at the front count) — this
    // proves WORKER-COUNT INVARIANCE of R + the solution, not 16-way concurrency.
    for (crd::u32 nw : {2U, 4U, 8U, 16U})
    {
        auto qrp = dir::factor_multifrontal_qr<T>(a, {vals.data(), vals.size()}, alloc, dir::kQrFrontRelax, nw);
        REQUIRE(qrp.info() == 0);
        REQUIRE(qrp.r_nnz() == qr1.r_nnz());
        const auto& rxp = qrp.rx();
        const auto& rjp = qrp.rj();
        bool ident = (rx1.size() == rxp.size()) && (rj1.size() == rjp.size());
        for (crd::usize p = 0; ident && p < rx1.size(); ++p)
        {
            ident = (rx1[p] == rxp[p]) && (rj1[p] == rjp[p]); // BIT-exact R across worker counts AND vs serial
        }
        CHECK(ident); // the determinism moat — R is a pure function of the pattern

        crd::containers::Array<T> xp(alloc);
        xp.resize(n);
        REQUIRE(qrp.least_squares({b.data(), m}, {xp.data(), n}, 1));
        bool xident = true;
        for (crd::u32 j = 0; j < n && xident; ++j)
        {
            xident = (xp[j] == x1[j]); // bit-identical solution ⇒ the stored Householder vectors match too
        }
        CHECK(xident);
    }
}
} // namespace

TEST_CASE("v5c tree-parallel QR: factor + solve bit-identical across {1,2,4,8} workers",
          "[hesap][direct][v5c][v5c-1g][qr]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 25);
        run_qr_parallel_moat<crd::f64>(&alloc); // f64
        run_qr_parallel_moat<crd::f32>(&alloc); // f32 — float reassociation is where worker-count drift shows first
        run_qr_parallel_moat<crd::hesap::Complex64>(&alloc); // v5c-2a complex moat (Qᴴ path, {1,2,4,8} bit-identical)
        run_qr_parallel_moat<crd::hesap::Complex32>(&alloc);
    }
    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// v5c-2a — complex QR (Complex32/Complex64). A·P = Q·R with the Hermitian transpose: the factor's
// correctness oracle is RᴴR == AᴴA (R = chol(AᴴA)). Genuinely-complex values (nonzero imaginary part)
// so the conjugation in the Qᴴ-apply is actually exercised (real-valued data would not distinguish
// Qᵀ from Qᴴ). Plus a complex least-squares solve recovering a complex x_true.
// ---------------------------------------------------------------------------
namespace
{
template <typename T> void run_qr_complex(crd::memory::IAllocator* alloc)
{
    using R = dense::RealType<T>;
    // 14×7 banded complex, full column rank: column k touches rows k, k+1, k+4 with nonzero-imaginary values.
    const crd::u32 m = 14;
    const crd::u32 n = 7;
    sp::SparsePattern a(alloc);
    a.rows = m;
    a.cols = n;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    auto nrows = [&](crd::u32 k)
    {
        return (k < m ? 1U : 0U) + (k + 1 < m ? 1U : 0U) + (k + 4 < m ? 1U : 0U);
    };
    for (crd::u32 k = 0; k < n; ++k)
    {
        a.outer_ptr[k + 1] = a.outer_ptr[k] + nrows(k);
    }
    a.inner_idx.resize(a.outer_ptr[n]);
    crd::containers::Array<T> vals(alloc);
    vals.resize(a.outer_ptr[n]);
    for (crd::u32 k = 0; k < n; ++k)
    {
        const crd::u32 rs[3] = {k, k + 1, k + 4};
        const T vs[3] = {T{R{5}, R{1}}, T{R{-1}, static_cast<R>(0.5)}, T{R{-1}, static_cast<R>(-0.3)}};
        crd::u32 w = a.outer_ptr[k];
        for (crd::u32 t = 0; t < 3; ++t)
        {
            if (rs[t] < m)
            {
                a.inner_idx[w] = rs[t];
                vals[w] = vs[t];
                ++w;
            }
        }
    }
    a.recompute_topology_hash();

    auto qr = dir::factor_multifrontal_qr<T>(a, {vals.data(), vals.size()}, alloc);
    REQUIRE(qr.info() == 0);

    // dense AᴴA[j][k] = Σ_i conj(A[i][j])·A[i][k]
    crd::containers::Array<T> aha(alloc);
    aha.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize z = 0; z < aha.size(); ++z)
    {
        aha[z] = T{R{0}, R{0}};
    }
    crd::containers::Array<T> rowbuf(alloc);
    rowbuf.resize(n);
    for (crd::u32 i = 0; i < m; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            rowbuf[j] = T{R{0}, R{0}};
        }
        for (crd::u32 k = 0; k < n; ++k)
        {
            for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
            {
                if (a.inner_idx[q] == i)
                {
                    rowbuf[k] = vals[q];
                }
            }
        }
        for (crd::u32 j = 0; j < n; ++j)
        {
            for (crd::u32 k = 0; k < n; ++k)
            {
                aha[static_cast<crd::usize>(j) * n + k] += crd::hesap::conj(rowbuf[j]) * rowbuf[k];
            }
        }
    }
    // dense RᴴR from the CSR R (row r ascending columns)
    crd::containers::Array<T> rhr(alloc);
    rhr.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize z = 0; z < rhr.size(); ++z)
    {
        rhr[z] = T{R{0}, R{0}};
    }
    const auto& rp = qr.rp();
    const auto& rj = qr.rj();
    const auto& rx = qr.rx();
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 a1 = rp[r]; a1 < rp[r + 1]; ++a1)
        {
            CHECK(rj[a1] >= r); // R upper-triangular
            for (crd::u32 a2 = rp[r]; a2 < rp[r + 1]; ++a2)
            {
                rhr[static_cast<crd::usize>(rj[a1]) * n + rj[a2]] += crd::hesap::conj(rx[a1]) * rx[a2];
            }
        }
    }
    R maxerr = R{0};
    R maxaha = R{1};
    for (crd::usize z = 0; z < aha.size(); ++z)
    {
        const T d = rhr[z] - aha[z];
        const R e = std::sqrt(d.re * d.re + d.im * d.im);
        const R av = std::sqrt(aha[z].re * aha[z].re + aha[z].im * aha[z].im);
        if (e > maxerr)
        {
            maxerr = e;
        }
        if (av > maxaha)
        {
            maxaha = av;
        }
    }
    CHECK(maxerr < static_cast<R>(1e-5) * maxaha); // RᴴR == AᴴA ⇒ the complex Qᴴ factor is correct

    // complex least-squares: b = A·x_true (consistent) ⇒ recovers x_true.
    crd::containers::Array<T> xt(alloc);
    crd::containers::Array<T> b(alloc);
    xt.resize(n);
    b.resize(m);
    for (crd::u32 j = 0; j < n; ++j)
    {
        xt[j] = T{R{1} + static_cast<R>(0.1) * static_cast<R>(j),
                  static_cast<R>(0.2) * static_cast<R>(j) - static_cast<R>(0.3)};
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        b[i] = T{R{0}, R{0}};
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            b[a.inner_idx[q]] += vals[q] * xt[k];
        }
    }
    crd::containers::Array<T> x(alloc);
    x.resize(n);
    REQUIRE(qr.least_squares({b.data(), m}, {x.data(), n}, 1));
    R xerr = R{0};
    for (crd::u32 j = 0; j < n; ++j)
    {
        const T d = x[j] - xt[j];
        xerr += d.re * d.re + d.im * d.im;
    }
    CHECK(std::sqrt(xerr) < static_cast<R>(1e-4));
}
} // namespace

TEST_CASE("v5c-2a complex QR: RhR == AhA + complex least-squares", "[hesap][direct][v5c][v5c-2a][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    run_qr_complex<crd::hesap::Complex64>(&alloc);
    run_qr_complex<crd::hesap::Complex32>(&alloc);
}

// ---------------------------------------------------------------------------
// v5c-2b — rank-revealing (Heath: NO column pivoting ⇒ the fill order + the determinism moat hold).
// A pivot column with |R diagonal| ≤ rcond·max|R diagonal| is DEAD; rank() = #live; the least-squares
// solve returns the BASIC solution (dead variables = 0). Tested on an EXACTLY rank-deficient A (an
// integer column literally the sum of two others ⇒ the dependence is exact in floating point) so the
// rank is clean and the basic-solution residual + normal-equation orthogonality are ~machine-eps (per
// the advisor: do NOT assert ‖Aᵀr‖==0 on a *numerically* deficient matrix — that bound is O(tol·‖A‖)).
// ---------------------------------------------------------------------------
TEST_CASE("v5c-2b rank-revealing QR: exact rank deficiency + basic solution", "[hesap][direct][v5c][v5c-2b][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // 8×4, integer values; col3 = col0 + col1 EXACTLY ⇒ rank 3, col3 dead (eliminated last). Cols 0,1,2
    // independent. (col0: rows 0,1,2 = 2,1,1; col1: rows 2,3,4 = 1,3,1; col2: rows 4,5,6 = 1,2,1;
    //  col3 = col0+col1: rows 0,1,2,3,4 = 2,1,2,3,1.)
    const crd::u32 m = 8;
    const crd::u32 n = 4;
    sp::SparsePattern a(&alloc);
    a.rows = m;
    a.cols = n;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    const crd::u32 colrows[4][5] = {{0, 1, 2, 0, 0}, {2, 3, 4, 0, 0}, {4, 5, 6, 0, 0}, {0, 1, 2, 3, 4}};
    const crd::f64 colvals[4][5] = {{2, 1, 1, 0, 0}, {1, 3, 1, 0, 0}, {1, 2, 1, 0, 0}, {2, 1, 2, 3, 1}};
    const crd::u32 colcnt[4] = {3, 3, 3, 5};
    for (crd::u32 k = 0; k < n; ++k)
    {
        a.outer_ptr[k + 1] = a.outer_ptr[k] + colcnt[k];
    }
    a.inner_idx.resize(a.outer_ptr[n]);
    crd::containers::Array<crd::f64> vals(&alloc);
    vals.resize(a.outer_ptr[n]);
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::u32 w = a.outer_ptr[k];
        for (crd::u32 t = 0; t < colcnt[k]; ++t)
        {
            a.inner_idx[w] = colrows[k][t];
            vals[w] = colvals[k][t];
            ++w;
        }
    }
    a.recompute_topology_hash();

    auto qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    CHECK(qr.rank() == 3); // col3 = col0 + col1 ⇒ numerical rank 3
    const auto dead = qr.dead();
    REQUIRE(dead.size() == n);
    CHECK(dead[0] == 0);
    CHECK(dead[1] == 0);
    CHECK(dead[2] == 0);
    CHECK(dead[3] == 1); // the dependent column (eliminated last) is the dead one

    // b = A·[1,1,1,1] (consistent ⇒ min residual is 0); the basic solution sets x[3]=0.
    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(m);
    for (crd::u32 j = 0; j < n; ++j)
    {
        xt[j] = 1.0;
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        b[i] = 0.0;
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            b[a.inner_idx[q]] += vals[q] * xt[k];
        }
    }
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    REQUIRE(qr.least_squares({b.data(), m}, {x.data(), n}, 1));
    CHECK(x[3] == 0.0); // dead variable forced to 0 (basic solution)

    // residual r = A·x − b ⇒ ‖r‖ ≈ 0 (b consistent + exact deficiency) and Aᵀr ≈ 0 (machine-eps).
    crd::containers::Array<crd::f64> r(&alloc);
    r.resize(m);
    for (crd::u32 i = 0; i < m; ++i)
    {
        r[i] = -b[i];
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            r[a.inner_idx[q]] += vals[q] * x[k];
        }
    }
    crd::f64 rnorm = 0.0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        rnorm += r[i] * r[i];
    }
    CHECK(std::sqrt(rnorm) < 1e-9); // b is consistent ⇒ the basic solution attains zero residual
    crd::f64 atr = 0.0;          // ‖Aᵀr‖ — normal-equation optimality (≈ eps for EXACT deficiency)
    for (crd::u32 k = 0; k < n; ++k)
    {
        crd::f64 g = 0.0;
        for (crd::u32 q = a.outer_ptr[k]; q < a.outer_ptr[k + 1]; ++q)
        {
            g += vals[q] * r[a.inner_idx[q]];
        }
        atr += g * g;
    }
    CHECK(std::sqrt(atr) < 1e-9);
}

TEST_CASE("v5c-2a complex QR: square 4x4 RhR==AhA + solve (diagnostic)", "[hesap][direct][v5c][v5c-2a][qr]")
{
    using T = crd::hesap::Complex64;
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 4;
    auto aij = [](crd::u32 i, crd::u32 j) -> T
    {
        if (i == j)
            return T{7.0, 0.5};
        if (j == i + 1)
            return T{1.0, 0.3};
        if (i == j + 1)
            return T{-1.0, 0.2};
        return T{0.0, 0.0};
    };
    sp::SparsePattern a(&alloc);
    a.rows = n;
    a.cols = n;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    crd::containers::Array<T> vals(&alloc);
    // CSC: column j, rows i where aij != 0.
    crd::containers::Array<crd::u32> tmprows(&alloc);
    for (crd::u32 j = 0; j < n; ++j)
    {
        crd::u32 cnt = 0;
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (aij(i, j).re != 0.0 || aij(i, j).im != 0.0)
            {
                ++cnt;
            }
        }
        a.outer_ptr[j + 1] = a.outer_ptr[j] + cnt;
    }
    a.inner_idx.resize(a.outer_ptr[n]);
    vals.resize(a.outer_ptr[n]);
    for (crd::u32 j = 0; j < n; ++j)
    {
        crd::u32 w = a.outer_ptr[j];
        for (crd::u32 i = 0; i < n; ++i)
        {
            const T v = aij(i, j);
            if (v.re != 0.0 || v.im != 0.0)
            {
                a.inner_idx[w] = i;
                vals[w] = v;
                ++w;
            }
        }
    }
    a.recompute_topology_hash();

    auto qr = dir::factor_multifrontal_qr<T>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    CHECK(qr.rank() == n);

    // RᴴR == AᴴA (factor correctness).
    crd::containers::Array<T> aha(&alloc);
    aha.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize z = 0; z < aha.size(); ++z)
    {
        aha[z] = T{0.0, 0.0};
    }
    for (crd::u32 col0 = 0; col0 < n; ++col0)
    {
        for (crd::u32 col1 = 0; col1 < n; ++col1)
        {
            T acc{0.0, 0.0};
            for (crd::u32 i = 0; i < n; ++i)
            {
                acc += crd::hesap::conj(aij(i, col0)) * aij(i, col1);
            }
            aha[static_cast<crd::usize>(col0) * n + col1] = acc;
        }
    }
    crd::containers::Array<T> rhr(&alloc);
    rhr.resize(static_cast<crd::usize>(n) * n);
    for (crd::usize z = 0; z < rhr.size(); ++z)
    {
        rhr[z] = T{0.0, 0.0};
    }
    const auto& rp = qr.rp();
    const auto& rj = qr.rj();
    const auto& rx = qr.rx();
    for (crd::u32 r = 0; r < n; ++r)
    {
        for (crd::u32 a1 = rp[r]; a1 < rp[r + 1]; ++a1)
        {
            for (crd::u32 a2 = rp[r]; a2 < rp[r + 1]; ++a2)
            {
                rhr[static_cast<crd::usize>(rj[a1]) * n + rj[a2]] += crd::hesap::conj(rx[a1]) * rx[a2];
            }
        }
    }
    crd::f64 rhr_err = 0.0;
    for (crd::usize z = 0; z < aha.size(); ++z)
    {
        const T d = rhr[z] - aha[z];
        rhr_err += d.re * d.re + d.im * d.im;
    }
    CHECK(std::sqrt(rhr_err) < 1e-9); // factor correct?

    // solve A x = b (b = A x_true), square via least_squares separate arrays.
    crd::containers::Array<T> xt(&alloc);
    crd::containers::Array<T> b(&alloc);
    crd::containers::Array<T> x(&alloc);
    xt.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = T{1.0 + static_cast<crd::f64>(i), -0.4 * static_cast<crd::f64>(i)};
        b[i] = T{0.0, 0.0};
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            b[i] += aij(i, j) * xt[j];
        }
    }
    REQUIRE(qr.least_squares({b.data(), n}, {x.data(), n}, 1));
    crd::f64 xe = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const T d = x[i] - xt[i];
        xe += d.re * d.re + d.im * d.im;
    }
    CHECK(std::sqrt(xe) < 1e-9); // solve correct?
}

TEST_CASE("v5c-2b rank-revealing QR: full rank, no false positive on a small diagonal",
          "[hesap][direct][v5c][v5c-2b][qr]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Diagonal 3×3 with a legitimately small (but ≫ rcond·max) entry 1e-6 ⇒ full rank, nothing dead.
    sp::SparsePattern a(&alloc);
    a.rows = 3;
    a.cols = 3;
    a.format = sp::SparseFormat::Csc;
    a.outer_ptr.resize(4);
    for (crd::u32 k = 0; k < 3; ++k)
    {
        a.outer_ptr[k + 1] = a.outer_ptr[k] + 1;
    }
    a.inner_idx.resize(3);
    crd::containers::Array<crd::f64> vals(&alloc);
    vals.resize(3);
    const crd::f64 dv[3] = {1.0, 2.0, 1e-6};
    for (crd::u32 k = 0; k < 3; ++k)
    {
        a.inner_idx[k] = k;
        vals[k] = dv[k];
    }
    a.recompute_topology_hash();
    auto qr = dir::factor_multifrontal_qr<crd::f64>(a, {vals.data(), vals.size()}, &alloc);
    REQUIRE(qr.info() == 0);
    CHECK(qr.rank() == 3); // 1e-6 ≫ rcond·max (= 3·eps·2) ⇒ NOT flagged dead
}
