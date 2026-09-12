// crd-hesap-direct v5f-(a) STEP 2 — within-front partial pivoting through the FULL multifrontal driver.
//
// The bar: factor_multifrontal_lu_pp (threshold>0) solves A·x=b to f64 on matrices whose WEAK diagonal forces
// the front factor to pivot (non-identity global row permutation P = rowperm()), and the {1,2,4,8}-worker
// solution + P are BIT-IDENTICAL (the moat). The existing static LU tests (threshold 0 default) are the
// regression net for the byte-unchanged static path; this exercises the new P / invperm / solve-permute path.

#include <crd/containers/array.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr64 = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

void csr_matvec(const Csr64& a, const crd::f64* x, crd::f64* y)
{
    const sp::SparsePattern& p = a.pattern();
    const crd::f64* v = a.values().values.data();
    for (crd::u32 r = 0; r < p.rows; ++r)
    {
        crd::f64 acc = 0.0;
        for (crd::u32 k = p.outer_ptr[r]; k < p.outer_ptr[r + 1]; ++k)
        {
            acc += v[k] * x[p.inner_idx[k]];
        }
        y[r] = acc;
    }
}

double rel_resid(crd::memory::IAllocator* alloc, const Csr64& a, const crd::f64* x, const crd::f64* b)
{
    const crd::u32 n = a.pattern().rows;
    crd::containers::Array<crd::f64> ax(alloc);
    ax.resize(n);
    csr_matvec(a, x, ax.data());
    double num = 0.0;
    double den = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double r = ax[i] - b[i];
        num += r * r;
        den += b[i] * b[i];
    }
    return std::sqrt(num) / (std::sqrt(den) + 1e-300);
}

// A WEAK-diagonal unsymmetric matrix: diagonal 1, STRONG sub-diagonal 3, weak super-diagonal -1. Column k's
// largest fully-summed candidate is the sub-diagonal (3 > 1) ⇒ within-front partial pivoting MUST swap (P is
// non-identity), yet the system is well-conditioned (stable once pivoted) ⇒ recovers f64.
Csr64 weak_diag(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 1.0);
        if (i + 1 < n)
        {
            tb.add(i + 1, i, 3.0); // strong sub-diagonal (column i, row i+1)
            tb.add(i, i + 1, -1.0);
        }
        if (i + 2 < n)
        {
            tb.add(i + 2, i, 0.5);
        }
    }
    return tb.compress();
}

// A CONNECTED, LARGE weak-diagonal 3D 7-point grid (k³ nodes): diagonal 1, unsymmetric couplings (3 below /
// −1 above the diagonal) ⇒ within-front pivoting MUST swap AND the deep nested-dissection tree means
// contribution rows get pivoted in ANCESTOR fronts (the cross-front invperm path the block-diagonal tests
// cannot exercise). Sized so the fill exceeds the parallel threshold (1M) ⇒ the {1,2,4,8} factor genuinely
// runs multi-worker, proving the cross-front remap is bit-identical serial-vs-parallel.
Csr64 grid3d_weak(crd::memory::IAllocator* a, crd::u32 k)
{
    const crd::u32 n = k * k * k;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto id = [k](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * k + j) * k + l; };
    for (crd::u32 i = 0; i < k; ++i)
    {
        for (crd::u32 j = 0; j < k; ++j)
        {
            for (crd::u32 l = 0; l < k; ++l)
            {
                const crd::u32 d = id(i, j, l);
                tb.add(d, d, 1.0); // weak diagonal ⇒ forces partial pivoting
                if (i + 1 < k)
                {
                    tb.add(d, id(i + 1, j, l), -1.0);
                    tb.add(id(i + 1, j, l), d, 3.0);
                }
                if (j + 1 < k)
                {
                    tb.add(d, id(i, j + 1, l), -1.0);
                    tb.add(id(i, j + 1, l), d, 3.0);
                }
                if (l + 1 < k)
                {
                    tb.add(d, id(i, j, l + 1), -1.0);
                    tb.add(id(i, j, l + 1), d, 3.0);
                }
            }
        }
    }
    return tb.compress();
}

// Block-diagonal of `nblk` weak-diagonal 5×5 blocks — independent fronts ⇒ nw>1 genuinely parallelizes.
Csr64 block_diag_weak(crd::memory::IAllocator* a, crd::u32 nblk)
{
    const crd::u32 bs = 5;
    const crd::u32 n = nblk * bs;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 blk = 0; blk < nblk; ++blk)
    {
        const crd::u32 o = blk * bs;
        for (crd::u32 i = 0; i < bs; ++i)
        {
            tb.add(o + i, o + i, 1.0);
            if (i + 1 < bs)
            {
                tb.add(o + i + 1, o + i, 3.0);
                tb.add(o + i, o + i + 1, -1.0);
            }
        }
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v5f-a partial-pivoting multifrontal LU solves a weak-diagonal system to f64", "[hesap][lu-pp-mf][v5f]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const crd::u32 n = 60;
    Csr64 a = weak_diag(&alloc, n);

    crd::containers::Array<crd::f64> xt(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xt.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xt[i] = 1.0 + 0.1 * static_cast<double>(i) - 0.05 * static_cast<double>(i % 6);
    }
    csr_matvec(a, xt.data(), b.data());

    auto lu = dir::factor_multifrontal_lu_pp(a, &alloc, 1); // threshold 1 = full within-front partial pivoting
    REQUIRE(lu.info() == 0);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(lu.solve({x.data(), n}));

    CHECK(rel_resid(&alloc, a, x.data(), b.data()) < 1e-10);
    double err = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        err += (x[i] - xt[i]) * (x[i] - xt[i]);
    }
    CHECK(std::sqrt(err) < 1e-9);

    // Confirm pivoting was genuinely exercised (P is non-identity) — otherwise the remap path is untested.
    const auto perm = lu.rowperm();
    REQUIRE(perm.size() == n);
    bool nonident = false;
    for (crd::u32 i = 0; i < n && !nonident; ++i)
    {
        nonident = (perm[i] != i);
    }
    CHECK(nonident);
}

TEST_CASE("v5f-a partial-pivoting multifrontal LU determinism moat {1,2,4,8}", "[hesap][lu-pp-mf][v5f][moat]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc(1U << 24);
        Csr64 a = block_diag_weak(&alloc, 8);
        const crd::u32 n = a.pattern().rows;
        crd::containers::Array<crd::f64> xt(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xt.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[i] = 1.0 + 0.1 * static_cast<double>(i);
        }
        csr_matvec(a, xt.data(), b.data());

        auto factory = [&](crd::u32 nw) { return dir::factor_multifrontal_lu_pp(a, &alloc, nw); };
        auto ref = factory(1U);
        REQUIRE(ref.info() == 0);
        crd::containers::Array<crd::f64> x_ref(&alloc);
        x_ref.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x_ref[i] = b[i];
        }
        REQUIRE(ref.solve({x_ref.data(), n}));
        const auto perm_ref = ref.rowperm();

        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto fp = factory(nw);
            REQUIRE(fp.info() == 0);
            // P bit-identical across worker counts.
            const auto perm = fp.rowperm();
            REQUIRE(perm.size() == perm_ref.size());
            bool pident = true;
            for (crd::u32 i = 0; i < perm.size() && pident; ++i)
            {
                pident = (perm[i] == perm_ref[i]);
            }
            CHECK(pident);
            // Solution bit-identical across worker counts.
            crd::containers::Array<crd::f64> x(&alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            REQUIRE(fp.solve({x.data(), n}));
            bool xident = true;
            for (crd::u32 i = 0; i < n && xident; ++i)
            {
                xident = (x[i] == x_ref[i]);
            }
            CHECK(xident);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("v5f-a partial-pivoting moat on a CONNECTED large grid (cross-front remap, PARALLEL)",
          "[hesap][lu-pp-mf][v5f][moat]")
{
    crd::jobs::init();
    {
        crd::memory::GrowableTlsfAllocator alloc; // large 3D factor across {1,2,4,8} ⇒ unbounded pooled
        Csr64 a = grid3d_weak(&alloc, 22);        // n=10648, fill > 1M ⇒ the factor runs MULTI-WORKER at nw>1
        const crd::u32 n = a.pattern().rows;
        crd::containers::Array<crd::f64> xt(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xt.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xt[i] = 1.0 + 0.001 * static_cast<double>(i);
        }
        csr_matvec(a, xt.data(), b.data());

        auto factory = [&](crd::u32 nw) { return dir::factor_multifrontal_lu_pp(a, &alloc, nw); };
        auto ref = factory(1U);
        REQUIRE(ref.info() == 0);
        REQUIRE(ref.factor_nnz() > (1ULL << 20)); // confirm the PARALLEL path is taken at nw>1 (not small_problem)
        // Pivoting + cross-front contribution rows genuinely present (deep nested-dissection tree).
        const auto perm_ref = ref.rowperm();
        REQUIRE(perm_ref.size() == n);
        bool nonident = false;
        for (crd::u32 i = 0; i < n && !nonident; ++i)
        {
            nonident = (perm_ref[i] != i);
        }
        CHECK(nonident);
        crd::containers::Array<crd::f64> x_ref(&alloc);
        x_ref.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x_ref[i] = b[i];
        }
        REQUIRE(ref.solve({x_ref.data(), n}));

        for (crd::u32 nw : {2U, 4U, 8U})
        {
            auto fp = factory(nw);
            REQUIRE(fp.info() == 0);
            const auto perm = fp.rowperm();
            bool pident = (perm.size() == perm_ref.size());
            for (crd::u32 i = 0; i < perm.size() && pident; ++i)
            {
                pident = (perm[i] == perm_ref[i]);
            }
            CHECK(pident); // P bit-identical serial-vs-parallel (the cross-front contribution-row remap)
            crd::containers::Array<crd::f64> x(&alloc);
            x.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = b[i];
            }
            REQUIRE(fp.solve({x.data(), n}));
            bool xident = true;
            for (crd::u32 i = 0; i < n && xident; ++i)
            {
                xident = (x[i] == x_ref[i]);
            }
            CHECK(xident); // solution bit-identical serial-vs-parallel
        }
    }
    crd::jobs::shutdown();
}
