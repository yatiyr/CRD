// crd-hesap-ordering v5b-2b — column (AᵀA) symbolic: column elimination tree +
// column counts of chol(AᵀA), the sparse-LU fill structure.
//
// The bar (advisor-directed, tightest constraint first): validate the implicit
// ata = 1 routines (AᵀA never formed) against an INDEPENDENT oracle that forms
// struct(BᵀB) EXPLICITLY on small matrices and runs the proven SYMMETRIC symbolic
// (elimination_tree / column_counts) on it. A wrong prev[]/transpose immediately
// diverges from the explicit tree. cs_etree(ata) and cs_counts(ata) are the
// Davis-faithful column-etree primitives that the v5b supernodal LU + v5c
// multifrontal QR consume.

#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace sp = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;
using Csc = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csc>;

// Form struct(BᵀB) EXPLICITLY as a symmetric CSR (the independent oracle; O(n²·nnz_col),
// fine for the small test matrices). (BᵀB)(i,j) != 0 iff columns i,j of B share a nonzero
// row. Diagonal (i,i) always present for a non-empty column; build_adjacency drops it.
Csr explicit_btb(const sp::SparsePattern& bcsc, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = bcsc.cols;
    const crd::u32 m = bcsc.rows;
    const crd::u32* cp = bcsc.outer_ptr.data();
    const crd::u32* ri = bcsc.inner_idx.data();
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    crd::containers::Array<crd::u8> mark(alloc);
    mark.resize(m); // value-init 0
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 p = cp[i]; p < cp[i + 1]; ++p)
        {
            mark[ri[p]] = 1;
        }
        for (crd::u32 j = 0; j < n; ++j)
        {
            bool shared = false;
            for (crd::u32 p = cp[j]; p < cp[j + 1]; ++p)
            {
                if (mark[ri[p]] != 0)
                {
                    shared = true;
                    break;
                }
            }
            if (shared)
            {
                tb.add(i, j, 1.0);
            }
        }
        for (crd::u32 p = cp[i]; p < cp[i + 1]; ++p)
        {
            mark[ri[p]] = 0;
        }
    }
    return tb.compress();
}

// Compare a column-symbolic result (computed implicitly from B's CSC) against the
// explicit struct(BᵀB) oracle. Returns true iff every entry matches.
bool etree_matches_explicit_btb(const Csc& bcsc, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = bcsc.pattern().cols;
    auto col_et = ord::column_elimination_tree(bcsc.pattern(), alloc);
    auto btb = explicit_btb(bcsc.pattern(), alloc);
    auto ref_et = ord::elimination_tree(btb.pattern(), alloc);
    if (col_et.size() != n || ref_et.size() != n)
    {
        return false;
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        if (col_et[j] != ref_et[j])
        {
            return false;
        }
    }
    return true;
}

bool counts_matches_explicit_btb(const Csc& bcsc, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = bcsc.pattern().cols;
    auto col_et = ord::column_elimination_tree(bcsc.pattern(), alloc);
    auto col_cnt = ord::column_counts_ata(bcsc.pattern(), {col_et.data(), col_et.size()}, alloc);
    auto btb = explicit_btb(bcsc.pattern(), alloc);
    auto ref_et = ord::elimination_tree(btb.pattern(), alloc);
    auto ref_cnt = ord::column_counts(btb.pattern(), {ref_et.data(), ref_et.size()}, alloc);
    if (col_cnt.size() != n || ref_cnt.size() != n)
    {
        return false;
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        if (col_cnt[j] != ref_cnt[j])
        {
            return false;
        }
    }
    return true;
}

// A heavily unsymmetric square matrix: a lower-triangular band + a few far-above
// entries + a dense-ish column (stresses prev[] across many rows).
Csc unsymmetric_a(crd::memory::IAllocator* alloc, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, static_cast<crd::f64>(n + 2));
        if (i > 0)
        {
            tb.add(i, i - 1, -1.0); // sub-diagonal
        }
        if (i + 2 < n)
        {
            tb.add(i, i + 2, 0.5); // super-2 (unsymmetric vs sub-diagonal)
        }
    }
    // a near-dense column 0 (every 3rd row references column 0) — fattens AᵀA
    for (crd::u32 i = 3; i < n; i += 3)
    {
        tb.add(i, 0, 0.25);
    }
    auto csr = tb.compress();
    return sp::to_csc<crd::f64>(csr, alloc);
}
} // namespace

TEST_CASE("v5b-2b column etree: small unsymmetric matches explicit BtB", "[lu][v5b-2b][ordering]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // 4x4 unsymmetric: 0->1, 1->2, 2->3 lower + 0->3 upper bridge + diagonal.
    sp::TripletBuilder<crd::f64> tb(&alloc, 4, 4);
    for (crd::u32 i = 0; i < 4; ++i)
    {
        tb.add(i, i, 4.0);
    }
    tb.add(1, 0, 1.0);
    tb.add(2, 1, 1.0);
    tb.add(3, 2, 1.0);
    tb.add(0, 3, 1.0); // unsymmetric bridge row 0, col 3
    auto csr = tb.compress();
    auto bcsc = sp::to_csc<crd::f64>(csr, &alloc);
    CHECK(etree_matches_explicit_btb(bcsc, &alloc));
}

TEST_CASE("v5b-2b column etree: banded unsymmetric + dense column matches explicit BtB", "[lu][v5b-2b][ordering]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    for (crd::u32 n : {7U, 16U, 33U, 50U})
    {
        auto bcsc = unsymmetric_a(&alloc, n);
        CHECK(etree_matches_explicit_btb(bcsc, &alloc));
    }
}

TEST_CASE("v5b-2b column etree: rectangular-feeling wide-row stress matches explicit BtB", "[lu][v5b-2b][ordering]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const crd::u32 n = 24;
    // Each row r references columns r, (r*7)%n, (r*13+1)%n — irregular, makes AᵀA dense and
    // the prev[] column-tracking non-trivial. Diagonal guaranteed (r,r).
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 r = 0; r < n; ++r)
    {
        tb.add(r, r, 10.0);
        tb.add(r, (r * 7U) % n, 1.0);
        tb.add(r, (r * 13U + 1U) % n, 1.0);
    }
    auto csr = tb.compress();
    auto bcsc = sp::to_csc<crd::f64>(csr, &alloc);
    CHECK(etree_matches_explicit_btb(bcsc, &alloc));
}

TEST_CASE("v5b-2b column counts (chol(AtA) bound): matches explicit BtB", "[lu][v5b-2b][ordering]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    for (crd::u32 n : {7U, 16U, 33U, 50U})
    {
        auto bcsc = unsymmetric_a(&alloc, n);
        CHECK(counts_matches_explicit_btb(bcsc, &alloc));
    }
    // also the irregular wide-row case
    const crd::u32 n = 24;
    sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
    for (crd::u32 r = 0; r < n; ++r)
    {
        tb.add(r, r, 10.0);
        tb.add(r, (r * 7U) % n, 1.0);
        tb.add(r, (r * 13U + 1U) % n, 1.0);
    }
    auto csr = tb.compress();
    auto bcsc = sp::to_csc<crd::f64>(csr, &alloc);
    CHECK(counts_matches_explicit_btb(bcsc, &alloc));
}
