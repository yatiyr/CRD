// crd-hesap-ordering v2a -- graph + RCM + nnz(L) fill metric.

#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdint>

namespace sp  = crd::hesap::sparse;
namespace ord = crd::hesap::ordering;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// Tridiagonal n x n (path graph): diagonal + sub/super-diagonal.
Csr tridiagonal(crd::memory::IAllocator* alloc, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 2.0);
        if (i > 0)
        {
            tb.add(i, i - 1, -1.0);
        }
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
        }
    }
    return tb.compress();
}

// 2D grid (W x H), 5-point stencil — a non-trivial fill pattern.
Csr grid2d(crd::memory::IAllocator* alloc, crd::u32 w, crd::u32 h)
{
    const crd::u32              n = w * h;
    sp::TripletBuilder<crd::f64> tb(alloc, n, n);
    auto idx = [w](crd::u32 r, crd::u32 c) { return r * w + c; };
    for (crd::u32 r = 0; r < h; ++r)
    {
        for (crd::u32 c = 0; c < w; ++c)
        {
            const crd::u32 v = idx(r, c);
            tb.add(v, v, 4.0);
            if (r > 0) tb.add(v, idx(r - 1, c), -1.0);
            if (r + 1 < h) tb.add(v, idx(r + 1, c), -1.0);
            if (c > 0) tb.add(v, idx(r, c - 1), -1.0);
            if (c + 1 < w) tb.add(v, idx(r, c + 1), -1.0);
        }
    }
    return tb.compress();
}

bool is_valid_permutation(const ord::Permutation& p, crd::u32 n, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> hit(alloc);
    hit.resize(n);
    if (p.size() != n)
    {
        return false;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 v = p.perm[i];
        if (v >= n || hit[v])
        {
            return false;
        }
        hit[v] = 1;
        if (p.inv_perm[v] != i)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("build_adjacency symmetrises + sorts + drops diagonal", "[hesap][ordering][graph]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    // Unsymmetric: 0->1, 0->2, 1->2, plus diagonals. Symmetric graph: 0-1, 0-2, 1-2.
    sp::TripletBuilder<crd::f64> tb(&alloc, 3, 3);
    tb.add(0, 0, 1.0);
    tb.add(0, 1, 1.0);
    tb.add(0, 2, 1.0);
    tb.add(1, 1, 1.0);
    tb.add(1, 2, 1.0);  // only 1->2 stored; symmetrise adds 2->1
    tb.add(2, 2, 1.0);
    auto       m = tb.compress();
    const auto g = ord::build_adjacency(m.pattern(), &alloc);
    REQUIRE(g.n == 3);
    REQUIRE(g.degree(0) == 2);  // {1,2}
    REQUIRE(g.degree(1) == 2);  // {0,2}
    REQUIRE(g.degree(2) == 2);  // {0,1}
    REQUIRE(g.num_edges() == 3);
    // adjacency ascending, no self-loops
    REQUIRE(g.adjncy[g.xadj[2]] == 0);
    REQUIRE(g.adjncy[g.xadj[2] + 1] == 1);
}

TEST_CASE("nnz_l + etree of a tridiagonal (path) matrix", "[hesap][ordering][symbolic]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    const crd::u32 n = 12;
    auto           t = tridiagonal(&alloc, n);
    // Cholesky of a tridiagonal is bidiagonal: n diagonal + (n-1) sub = 2n-1.
    REQUIRE(ord::nnz_l(t.pattern(), &alloc) == static_cast<crd::u64>(2 * n - 1));
    // Etree of a path is a chain: parent[i] = i+1, root = n-1.
    auto et = ord::elimination_tree(t.pattern(), &alloc);
    for (crd::u32 i = 0; i + 1 < n; ++i)
    {
        CHECK(et[i] == i + 1);
    }
    CHECK(et[n - 1] == ord::kNoParent);
}

TEST_CASE("apply_symmetric: identity is a no-op; PAP^T is consistent", "[hesap][ordering][perm]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    const crd::u32 n = 8;
    auto           t = tridiagonal(&alloc, n);

    ord::Permutation id(&alloc);
    id.perm.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        id.perm[i] = i;
    }
    id.rebuild_inverse();
    auto same = ord::apply_symmetric(t.pattern(), id, &alloc);
    REQUIRE(same.topology_hash == t.pattern().topology_hash);  // identity perm → same structure
    REQUIRE(same.nnz() == t.nnz());
}

TEST_CASE("RCM recovers a scrambled tridiagonal: bandwidth + fill drop", "[hesap][ordering][rcm]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    const crd::u32 n = 60;
    auto           t = tridiagonal(&alloc, n);

    // Scramble with a fixed permutation so the natural ordering is high-bandwidth.
    ord::Permutation scramble(&alloc);
    scramble.perm.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        scramble.perm[i] = (i * 37U + 11U) % n;  // a fixed bijection (gcd(37,60)=1)
    }
    // Ensure it's a bijection (37 coprime to 60 → yes); rebuild inverse.
    scramble.rebuild_inverse();
    auto s = ord::apply_symmetric(t.pattern(), scramble, &alloc);

    const crd::u32 bw_natural  = ord::bandwidth(s);
    const crd::u64 fill_natural = ord::nnz_l(s, &alloc);

    auto p = ord::rcm_order(s, &alloc);
    REQUIRE(is_valid_permutation(p, n, &alloc));
    auto rs = ord::apply_symmetric(s, p, &alloc);

    const crd::u32 bw_rcm   = ord::bandwidth(rs);
    const crd::u64 fill_rcm = ord::nnz_l(rs, &alloc);

    INFO("bw natural=" << bw_natural << " rcm=" << bw_rcm << " | fill natural=" << fill_natural
                       << " rcm=" << fill_rcm);
    CHECK(bw_rcm < bw_natural);                              // RCM reduces bandwidth
    CHECK(fill_rcm < fill_natural);                          // and fill
    CHECK(bw_rcm <= 2);                                      // a tridiagonal recovered → bandwidth 1-2
    CHECK(fill_rcm == static_cast<crd::u64>(2 * n - 1));     // exactly the tridiagonal fill
}

TEST_CASE("quotient_fill (AMD machinery) matches cs_counts nnz_l", "[hesap][ordering][amd]")
{
    crd::memory::TlsfAllocator alloc(32 << 20);

    auto check = [&](const Csr& m) {
        const crd::u32 n = m.rows();
        const auto     g = ord::build_adjacency(m.pattern(), &alloc);

        // Natural elimination order [0..n) -> must equal cs_counts nnz_l(natural).
        crd::containers::Array<crd::u32> natural(&alloc);
        natural.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            natural[i] = i;
        }
        const crd::u64 qf_nat = ord::detail::quotient_fill(g, {natural.data(), natural.size()}, &alloc);
        REQUIRE(qf_nat == ord::nnz_l(m.pattern(), &alloc));
        // Rung 1: packed workspace must equal the explicit-rep machinery + cs_counts.
        REQUIRE(ord::detail::packed_fill(g, {natural.data(), natural.size()}, &alloc) == qf_nat);

        // RCM elimination order -> must equal nnz_l of the RCM-permuted pattern.
        auto           p     = ord::rcm_order(m.pattern(), &alloc);
        const crd::u64 qf_rcm = ord::detail::quotient_fill(g, {p.perm.data(), p.perm.size()}, &alloc);
        auto           rp    = ord::apply_symmetric(m.pattern(), p, &alloc);
        REQUIRE(qf_rcm == ord::nnz_l(rp, &alloc));
        REQUIRE(ord::detail::packed_fill(g, {p.perm.data(), p.perm.size()}, &alloc) == qf_rcm);
    };

    check(tridiagonal(&alloc, 20));
    check(grid2d(&alloc, 8, 8));    // 64 vars, real fill
    check(grid2d(&alloc, 5, 11));   // non-square grid
}

TEST_CASE("amd_order (exact min-degree) is a valid, low-fill, deterministic ordering", "[hesap][ordering][amd]")
{
    crd::memory::TlsfAllocator alloc(32 << 20);
    auto check = [&](const Csr& m) {
        const crd::u32 n = m.rows();
        const auto     g = ord::build_adjacency(m.pattern(), &alloc);

        auto p = ord::amd_order(m.pattern(), &alloc);
        REQUIRE(is_valid_permutation(p, n, &alloc));

        const crd::u64 fill_amd = ord::detail::quotient_fill(g, {p.perm.data(), p.perm.size()}, &alloc);
        const crd::u64 fill_nat = ord::nnz_l(m.pattern(), &alloc);
        INFO("amd fill=" << fill_amd << " natural=" << fill_nat);
        CHECK(fill_amd <= fill_nat);  // min-degree is never worse than natural

        // Determinism: identical ordering across runs.
        auto p2 = ord::amd_order(m.pattern(), &alloc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(p.perm[i] == p2.perm[i]);
        }
    };
    check(tridiagonal(&alloc, 30));
    check(grid2d(&alloc, 10, 10));   // 2D grid: min-degree should cut fill hard vs natural
    check(grid2d(&alloc, 6, 14));
}

TEST_CASE("RCM is deterministic (bit-identical permutation across runs)", "[hesap][ordering][rcm]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 100;
    auto           t = tridiagonal(&alloc, n);
    ord::Permutation scramble(&alloc);
    scramble.perm.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        scramble.perm[i] = (i * 53U + 7U) % n;
    }
    scramble.rebuild_inverse();
    auto s  = ord::apply_symmetric(t.pattern(), scramble, &alloc);
    auto p1 = ord::rcm_order(s, &alloc);
    auto p2 = ord::rcm_order(s, &alloc);
    REQUIRE(p1.size() == p2.size());
    for (crd::u32 i = 0; i < n; ++i)
    {
        REQUIRE(p1.perm[i] == p2.perm[i]);
    }
}
