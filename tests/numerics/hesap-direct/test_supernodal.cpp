// crd-hesap-direct v5a-1b — supernodal symbolic (relaxed amalgamation) tests.

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/detail/direct_picks.hpp>
#include <crd/hesap/direct/direct.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace dir = crd::hesap::direct;
namespace ord = crd::hesap::ordering;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

Csr dense_spd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 j = 0; j < n; ++j)
        {
            tb.add(i, j, i == j ? static_cast<crd::f64>(n + 1) : 1.0);
        }
    }
    return tb.compress();
}

Csr tridiag(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
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

Csr grid2d(crd::memory::IAllocator* a, crd::u32 w, crd::u32 h)
{
    const crd::u32 n = w * h;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto idx = [w](crd::u32 r, crd::u32 c)
    {
        return r * w + c;
    };
    for (crd::u32 r = 0; r < h; ++r)
    {
        for (crd::u32 c = 0; c < w; ++c)
        {
            const crd::u32 v = idx(r, c);
            tb.add(v, v, 4.0);
            if (r > 0)
            {
                tb.add(v, idx(r - 1, c), -1.0);
            }
            if (r + 1 < h)
            {
                tb.add(v, idx(r + 1, c), -1.0);
            }
            if (c > 0)
            {
                tb.add(v, idx(r, c - 1), -1.0);
            }
            if (c + 1 < w)
            {
                tb.add(v, idx(r, c + 1), -1.0);
            }
        }
    }
    return tb.compress();
}

// 3D 7-point Laplacian on an s×s×s grid. Under AMD the near-root separator is ~O(s²),
// so for s≥26 the top supernode exceeds kNodeParallelMinCols=512 → it goes NODE-parallel
// at N threads AND receives cmod from its descendants → exercises BOTH thread-divergent
// paths (no-pack cmod at 1 thread vs generic gemm_parallel at N; two-level cdiv serial vs
// parallel). The determinism-moat matrix.
Csr grid3d(crd::memory::IAllocator* a, crd::u32 s)
{
    const crd::u32 n = s * s * s;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto idx = [s](crd::u32 x, crd::u32 y, crd::u32 z)
    {
        return (z * s + y) * s + x;
    };
    for (crd::u32 z = 0; z < s; ++z)
    {
        for (crd::u32 y = 0; y < s; ++y)
        {
            for (crd::u32 x = 0; x < s; ++x)
            {
                const crd::u32 v = idx(x, y, z);
                tb.add(v, v, 6.0);
                if (x > 0)
                {
                    tb.add(v, idx(x - 1, y, z), -1.0);
                }
                if (x + 1 < s)
                {
                    tb.add(v, idx(x + 1, y, z), -1.0);
                }
                if (y > 0)
                {
                    tb.add(v, idx(x, y - 1, z), -1.0);
                }
                if (y + 1 < s)
                {
                    tb.add(v, idx(x, y + 1, z), -1.0);
                }
                if (z > 0)
                {
                    tb.add(v, idx(x, y, z - 1), -1.0);
                }
                if (z + 1 < s)
                {
                    tb.add(v, idx(x, y, z + 1), -1.0);
                }
            }
        }
    }
    return tb.compress();
}

// Bordered block-diagonal SPD: nb dense bs×bs blocks + a dense nbord-column border coupled to every block.
// Purpose-built for the v5a-4 NODE-parallel moat at a fraction of grid3d(28)'s cost: under AMD the low-degree
// block nodes eliminate first and the dense border becomes the ROOT supernode (nc ≈ nbord ≥ kNodeParallelMinCols)
// that RECEIVES cmod from every block supernode — the same divergent paths (two-level cdiv, no-pack cmod vs
// gemm_parallel) at ~450 Mflop instead of grid3d(28)'s tens of Gflop. (grid3d(28) took 456 s in win-debug and
// ~40 min under win-asan — 2026-07-02 diagnosis: slow, not hung; ASan shadow checks on every microkernel load.)
Csr bordered_spd(crd::memory::IAllocator* a, crd::u32 nb, crd::u32 bs, crd::u32 nbord)
{
    const crd::u32 n = nb * bs + nbord;
    const crd::u32 b0 = nb * bs; // first border row
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 b = 0; b < nb; ++b) // dense diagonal blocks
    {
        const crd::u32 r0 = b * bs;
        for (crd::u32 i = 0; i < bs; ++i)
        {
            for (crd::u32 j = 0; j < bs; ++j)
            {
                tb.add(r0 + i, r0 + j, i == j ? static_cast<crd::f64>(n + 1) : 1.0);
            }
        }
    }
    for (crd::u32 i = b0; i < n; ++i) // dense border block + border↔block coupling (both triangles)
    {
        for (crd::u32 j = b0; j < n; ++j)
        {
            tb.add(i, j, i == j ? static_cast<crd::f64>(n + 1) : 1.0);
        }
        for (crd::u32 j = 0; j < b0; ++j)
        {
            tb.add(i, j, 1.0);
            tb.add(j, i, 1.0);
        }
    }
    return tb.compress();
}

// Structural invariants every amalgamated symbolic must satisfy.
void check_valid(const dir::SupernodalSymbolic& s, const ord::SymbolicFactor& sf)
{
    REQUIRE(s.n == sf.n);
    REQUIRE(s.nsuper >= 1);
    REQUIRE(s.nsuper <= sf.nsuper); // amalgamation only merges
    REQUIRE(s.scol[0] == 0);
    REQUIRE(s.scol[s.nsuper] == sf.n); // columns fully covered
    for (crd::u32 t = 0; t < s.nsuper; ++t)
    {
        REQUIRE(s.scol[t] < s.scol[t + 1]); // each supernode non-empty + contiguous
    }
    for (crd::u32 c = 0; c < sf.n; ++c)
    {
        const crd::u32 su = s.col_super[c];
        REQUIRE(s.scol[su] <= c);
        REQUIRE(c < s.scol[su + 1]);
    }
    for (crd::u32 t = 0; t < s.nsuper; ++t)
    {
        const crd::u32 cols = s.scol[t + 1] - s.scol[t];
        const crd::u32 rb = s.srowp[t];
        const crd::u32 re = s.srowp[t + 1];
        REQUIRE(re - rb >= cols);           // the dense diagonal block fits
        for (crd::u32 k = 0; k < cols; ++k) // first `cols` rows == the supernode's columns
        {
            REQUIRE(s.srow[rb + k] == s.scol[t] + k);
        }
        for (crd::u32 q = rb + 1; q < re; ++q) // ascending row pattern
        {
            REQUIRE(s.srow[q] > s.srow[q - 1]);
        }
    }
    REQUIRE(s.lnz >= sf.nnz()); // amalgamation only adds explicit zeros to the factor
}
} // namespace

TEST_CASE("build_supernodal_symbolic: dense SPD is a single full supernode", "[hesap][direct][v5a-1b][amalg]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 12;
    auto m = dense_spd(&alloc, n);
    auto sf = ord::symbolic_factorize(m.pattern(), &alloc);
    auto s = dir::build_supernodal_symbolic(sf, &alloc);
    check_valid(s, sf);
    REQUIRE(s.nsuper == 1);
    REQUIRE(s.srowp[1] == n);                                 // all rows in the one panel
    REQUIRE(s.lnz == static_cast<crd::u64>(n) * (n + 1) / 2); // dense lower triangle, no extra zeros
    REQUIRE(s.lnz == sf.nnz());
}

TEST_CASE("build_supernodal_symbolic: tridiagonal path stays narrow + valid", "[hesap][direct][v5a-1b][amalg]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    auto m = tridiag(&alloc, 64);
    auto sf = ord::symbolic_factorize(m.pattern(), &alloc);
    auto s = dir::build_supernodal_symbolic(sf, &alloc);
    check_valid(s, sf);
}

TEST_CASE("build_supernodal_symbolic: 2D grid amalgamates + is deterministic", "[hesap][direct][v5a-1b][amalg]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    auto m = grid2d(&alloc, 8, 8);
    auto sf = ord::symbolic_factorize(m.pattern(), &alloc);
    auto s = dir::build_supernodal_symbolic(sf, &alloc);
    check_valid(s, sf);
    // The grid's chol has multi-column supernodes that amalgamate.
    CHECK(s.nsuper < sf.nsuper);
    // Deterministic: rebuild is bit-identical in shape.
    auto s2 = dir::build_supernodal_symbolic(sf, &alloc);
    REQUIRE(s.nsuper == s2.nsuper);
    REQUIRE(s.lnz == s2.lnz);
    REQUIRE(s.srow.size() == s2.srow.size());
}

namespace
{
// b = A·x for a symmetric matrix in `pat`/`vals` (outer o's entries are row o).
void spmv_sym(const sp::SparsePattern& pat, const crd::containers::Array<crd::f64>& vals,
              const crd::containers::Array<crd::f64>& x, crd::containers::Array<crd::f64>& b)
{
    const crd::u32 n = pat.rows;
    for (crd::u32 o = 0; o < n; ++o)
    {
        crd::f64 acc = 0.0;
        const crd::u32 st = pat.outer_ptr[o];
        const crd::u32 cnt = pat.inner_count(o);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            acc += vals[st + k] * x[pat.inner_idx[st + k]];
        }
        b[o] = acc;
    }
}
} // namespace

TEST_CASE("supernodal Cholesky factor+solve: residual ~0 on a 2D-grid SPD", "[hesap][direct][v5a-1b][numeric][kernels]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    auto m = grid2d(&alloc, 7, 6); // 42-node Laplacian-like SPD
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.13 * static_cast<crd::f64>(i);
    }
    spmv_sym(pat, vals, xtrue, b);

    auto f = dir::factor_supernodal_cholesky<crd::f64>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0);
    REQUIRE(f.n() == n);
    REQUIRE(f.factor_nnz() >= n);

    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(f.solve({x.data(), n}));
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(x[i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("supernodal Cholesky: dense SPD factor+solve matches the system", "[hesap][direct][v5a-1b][numeric]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    const crd::u32 n = 10;
    auto m = dense_spd(&alloc, n); // diag n+1, off 1 → SPD, single supernode
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> x(&alloc);
    xtrue.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = static_cast<crd::f64>(i) - 4.0;
    }
    spmv_sym(pat, vals, xtrue, x); // x := b
    auto f = dir::factor_supernodal_cholesky<crd::f64>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve({x.data(), n}));
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(x[i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("supernodal Cholesky: TREE-PARALLEL factor is bit-identical to serial (determinism moat)",
          "[hesap][direct][v5a-3][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    auto m = grid2d(&alloc, 12, 12); // 144-node SPD → multi-level supernode etree (real parallelism)
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::containers::ConstSpan<crd::f64> vspan{vals.data(), vals.size()};

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.011 * static_cast<crd::f64>(i);
    }
    spmv_sym(pat, vals, xtrue, b);

    crd::jobs::init();
    // Serial reference solution.
    crd::containers::Array<crd::f64> x1(&alloc);
    x1.resize(n);
    {
        auto f1 = dir::factor_supernodal_cholesky<crd::f64>(pat, vspan, &alloc, dir::kSupernodeRelax, 1);
        REQUIRE(f1.info() == 0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x1[i] = b[i];
        }
        REQUIRE(f1.solve({x1.data(), n}));
    }
    const crd::u32 w = crd::jobs::num_workers();
    // Parallel solutions at several worker counts — each must be BIT-identical to serial.
    for (crd::u32 nw : {2U, 4U, w > 4U ? w : 4U})
    {
        auto fp = dir::factor_supernodal_cholesky<crd::f64>(pat, vspan, &alloc, dir::kSupernodeRelax, nw);
        REQUIRE(fp.info() == 0);
        crd::containers::Array<crd::f64> xp(&alloc);
        xp.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xp[i] = b[i];
        }
        REQUIRE(fp.solve({xp.data(), n}));
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xp[i] == x1[i]); // bit-exact across worker counts — the moat
        }
    }
    crd::jobs::shutdown();
    // And the solution is correct.
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(x1[i] - xtrue[i]) < 1e-9);
    }
}

// v7-e-2 gate: refactorize() reuses the symbolic analysis (m_sym) for a NUMERIC re-factorization on a
// structurally-identical pattern with NEW values. The symbolic phase is a pure function of the pattern, so a
// refactorize() must be BIT-IDENTICAL to a fresh factorize() on those values — that lets the sparse-LM
// normal-equations loop pay symbolic once across all λ-trials without changing the answer. The determinism moat
// (bit-exact across {1,2,4,8} workers) must survive the reuse too.
TEST_CASE("supernodal Cholesky: refactorize (reuse symbolic) == fresh factorize, bit-identical (v7-e-2 gate)",
          "[hesap][direct][v7-e-2][determinism]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    // Two SPD matrices with the SAME nonzero structure (grid2d), DIFFERENT (still diag-dominant) values.
    auto build = [&alloc](crd::u32 w, crd::u32 h, crd::f64 diag, crd::f64 off) -> Csr
    {
        const crd::u32 n = w * h;
        sp::TripletBuilder<crd::f64> tb(&alloc, n, n);
        auto idx = [w](crd::u32 r, crd::u32 c)
        {
            return r * w + c;
        };
        for (crd::u32 r = 0; r < h; ++r)
        {
            for (crd::u32 c = 0; c < w; ++c)
            {
                const crd::u32 v = idx(r, c);
                tb.add(v, v, diag);
                if (r > 0)
                {
                    tb.add(v, idx(r - 1, c), off);
                }
                if (r + 1 < h)
                {
                    tb.add(v, idx(r + 1, c), off);
                }
                if (c > 0)
                {
                    tb.add(v, idx(r, c - 1), off);
                }
                if (c + 1 < w)
                {
                    tb.add(v, idx(r, c + 1), off);
                }
            }
        }
        return tb.compress();
    };
    auto m1 = build(12, 12, 4.0, -1.0);
    auto m2 = build(12, 12, 6.5, -1.3); // same structure, different values
    const crd::u32 n = m1.rows();

    const auto& pat1 = m1.pattern();
    const auto& pat2 = m2.pattern();
    const auto& vals1 = m1.values().values;
    const auto& vals2 = m2.values().values;
    const crd::containers::ConstSpan<crd::f64> v1{vals1.data(), vals1.size()};
    const crd::containers::ConstSpan<crd::f64> v2{vals2.data(), vals2.size()};

    crd::containers::Array<crd::f64> b(&alloc);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b[i] = 1.0 + 0.013 * static_cast<crd::f64>(i);
    }

    crd::jobs::init();

    // Fresh factorize on m2 (the reference).
    crd::containers::Array<crd::f64> x_fresh(&alloc);
    x_fresh.resize(n);
    crd::u64 nnz_fresh = 0;
    {
        dir::SupernodalCholesky<crd::f64> f(&alloc);
        f.factorize(pat2, v2, dir::kSupernodeRelax, 1);
        REQUIRE(f.info() == 0);
        nnz_fresh = f.factor_nnz();
        for (crd::u32 i = 0; i < n; ++i)
        {
            x_fresh[i] = b[i];
        }
        REQUIRE(f.solve({x_fresh.data(), n}));
    }

    // factorize on m1, then refactorize with m2's values (reuse the symbolic from m1).
    {
        dir::SupernodalCholesky<crd::f64> f(&alloc);
        f.factorize(pat1, v1, dir::kSupernodeRelax, 1);
        REQUIRE(f.info() == 0);
        f.refactorize(pat2, v2, 1); // reuse symbolic — same structure, new values
        REQUIRE(f.info() == 0);
        REQUIRE(f.factor_nnz() == nnz_fresh); // identical structure
        crd::containers::Array<crd::f64> x_reuse(&alloc);
        x_reuse.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x_reuse[i] = b[i];
        }
        REQUIRE(f.solve({x_reuse.data(), n}));
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(x_reuse[i] == x_fresh[i]); // refactorize == fresh factorize, bit-identical
        }
    }

    // The determinism moat survives refactorize: factorize(m1)+refactorize(m2)+solve is bit-identical to the
    // serial fresh-m2 reference across worker counts.
    const crd::u32 w = crd::jobs::num_workers();
    for (crd::u32 nw : {2U, 4U, w > 4U ? w : 4U})
    {
        dir::SupernodalCholesky<crd::f64> f(&alloc);
        f.factorize(pat1, v1, dir::kSupernodeRelax, nw);
        f.refactorize(pat2, v2, nw);
        REQUIRE(f.info() == 0);
        crd::containers::Array<crd::f64> xp(&alloc);
        xp.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xp[i] = b[i];
        }
        REQUIRE(f.solve({xp.data(), n}));
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xp[i] == x_fresh[i]); // bit-exact across worker counts after refactorize — the moat
        }
    }

    crd::jobs::shutdown();
}

// v5a-4 moat: the grid2d(144) test above never produces a supernode ≥ kNodeParallelMinCols=512,
// so it exercises only the TREE-parallel path. The two-level cdiv (v5a-4) and the no-pack cmod
// (v5a-4) introduce code that DIVERGES by worker count ONLY on a node-parallel front (serial-gemm
// /no-pack at 1 worker vs generic gemm_parallel at N). This test factors matrices whose fat
// supernode genuinely exceeds 512 — dense_spd(600) for the cdiv-only path and bordered_spd for a
// fat root WITH descendants sending cmod — so the divergent paths actually run — and REQUIRES the
// factor (hence the solve) bit-identical across {1,2,4,8} workers. Capped at 8 workers (i9-14900K
// host). Both shape guards (maxnc ≥ 512, nsuper ≥ min) fail loudly if the coverage ever narrows.
TEST_CASE("supernodal Cholesky: fat-front NODE-PARALLEL factor is bit-identical to serial (v5a-4 moat)",
          "[hesap][direct][v5a-4][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(512 << 20);
    crd::jobs::Config cfg;
    cfg.num_threads = 8; // cap host load AND still trigger node-parallel (cnt<8 at thin root levels)
    crd::jobs::init(cfg);
    const crd::u32 w = crd::jobs::num_workers();

    auto moat = [&](const Csr& m, crd::u32 min_nsuper)
    {
        const crd::u32 n = m.rows();
        const auto& pat = m.pattern();
        const auto& vals = m.values().values;
        const crd::containers::ConstSpan<crd::f64> vspan{vals.data(), vals.size()};
        // Guard: confirm a node-parallel-eligible fat front EXISTS — else the test silently
        // stops exercising the divergence it is meant to lock down.
        auto sf = ord::symbolic_factorize(pat, &alloc);
        auto sym = dir::build_supernodal_symbolic(sf, &alloc, dir::kSupernodeRelax);
        crd::u32 maxnc = 0;
        for (crd::u32 t = 0; t < sym.nsuper; ++t)
        {
            const crd::u32 nc = sym.scol[t + 1] - sym.scol[t];
            if (nc > maxnc)
            {
                maxnc = nc;
            }
        }
        REQUIRE(maxnc >= 512); // kNodeParallelMinCols — proves the divergent paths actually run
        // Guard #2: the fat front must have DESCENDANTS for the cmod/no-pack divergence to run (a
        // single-supernode matrix like dense_spd covers only the cdiv path). Loud, not silent: if
        // amalgamation ever merges the bordered matrix into one supernode, this fails immediately.
        REQUIRE(sym.nsuper >= min_nsuper);

        crd::containers::Array<crd::f64> xtrue(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        xtrue.resize(n);
        b.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xtrue[i] = 1.0 + 0.011 * static_cast<crd::f64>(i % 97);
        }
        spmv_sym(pat, vals, xtrue, b);

        crd::containers::Array<crd::f64> x1(&alloc);
        x1.resize(n);
        auto f1 = dir::factor_supernodal_cholesky<crd::f64>(pat, vspan, &alloc, dir::kSupernodeRelax, 1);
        REQUIRE(f1.info() == 0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x1[i] = b[i];
        }
        REQUIRE(f1.solve({x1.data(), n}));
        // Multi-RHS reference (nw=1, serial backward) for the level-PARALLEL backward bit-identity check.
        const crd::usize nrhs_m = 4;
        crd::containers::Array<crd::f64> xm1(&alloc);
        xm1.resize(static_cast<crd::usize>(n) * nrhs_m);
        for (crd::usize c = 0; c < nrhs_m; ++c)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                xm1[c * n + i] = b[i] * (1.0 + 0.3 * static_cast<crd::f64>(c));
            }
        }
        REQUIRE(f1.solve({xm1.data(), xm1.size()}, nrhs_m));
        // CORRECTNESS (catches deterministic index bugs the bit-identity check can't): A·x = b·(1+0.3c)
        // and b = A·xtrue ⇒ column c of the solution is (1+0.3c)·xtrue.
        for (crd::usize c = 0; c < nrhs_m; ++c)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                CHECK(std::abs(xm1[c * n + i] - (1.0 + 0.3 * static_cast<crd::f64>(c)) * xtrue[i]) < 1e-7);
            }
        }
        // SOLVE DETERMINISM MOAT (the sacred bar — proven EMPIRICALLY, not just by construction): the SAME
        // factor solved serially (nw=1 → RIGHT-looking forward + serial backward) vs in parallel (nw>1 →
        // LEFT-looking forward + level-parallel backward) must be BIT-IDENTICAL. solve_arm forces
        // the path — the public solve() reads num_workers() (fixed for the whole test run), so this is the
        // ONLY place the serial and parallel solve paths execute head-to-head on one factor.
        {
            crd::containers::Array<crd::f64> xss(&alloc); // serial path (nw=1): right-looking forward
            crd::containers::Array<crd::f64> xsp(&alloc); // parallel path (nw>1): left-looking forward
            xss.resize(static_cast<crd::usize>(n) * nrhs_m);
            xsp.resize(static_cast<crd::usize>(n) * nrhs_m);
            for (crd::usize c = 0; c < nrhs_m; ++c)
            {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    const crd::f64 v = b[i] * (1.0 + 0.3 * static_cast<crd::f64>(c));
                    xss[c * n + i] = v;
                    xsp[c * n + i] = v;
                }
            }
            REQUIRE(f1.solve_arm(dir::detail::SupernodalPick{.solve_workers = 1}, {xss.data(), xss.size()}, nrhs_m));
            REQUIRE(f1.solve_arm(dir::detail::SupernodalPick{.solve_workers = w > 1U ? w : 2U},
                                 {xsp.data(), xsp.size()}, nrhs_m));
            for (crd::usize k = 0; k < xss.size(); ++k)
            {
                REQUIRE(xss[k] == xsp[k]); // bit-identical: serial RIGHT-looking == parallel LEFT-looking
            }
        }

        for (crd::u32 nw : {2U, 4U, w > 4U ? w : 4U})
        {
            auto fp = dir::factor_supernodal_cholesky<crd::f64>(pat, vspan, &alloc, dir::kSupernodeRelax, nw);
            REQUIRE(fp.info() == 0);
            REQUIRE(fp.factor_nnz() == f1.factor_nnz()); // identical structure
            crd::containers::Array<crd::f64> xp(&alloc);
            xp.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                xp[i] = b[i];
            }
            REQUIRE(fp.solve({xp.data(), n}));
            for (crd::u32 i = 0; i < n; ++i)
            {
                REQUIRE(xp[i] == x1[i]); // bit-exact across worker counts on a NODE-PARALLEL front
            }
            // Multi-RHS solve: the level-PARALLEL backward must be bit-identical to the serial nw=1.
            crd::containers::Array<crd::f64> xmp(&alloc);
            xmp.resize(static_cast<crd::usize>(n) * nrhs_m);
            for (crd::usize c = 0; c < nrhs_m; ++c)
            {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    xmp[c * n + i] = b[i] * (1.0 + 0.3 * static_cast<crd::f64>(c));
                }
            }
            REQUIRE(fp.solve({xmp.data(), xmp.size()}, nrhs_m));
            for (crd::usize k = 0; k < xmp.size(); ++k)
            {
                REQUIRE(xmp[k] == xm1[k]); // bit-exact multi-RHS across worker counts (parallel backward)
            }
        }
    };

    moat(dense_spd(&alloc, 600), 1);            // ONE supernode nc=600 → two-level cdiv runs NODE-parallel
    moat(bordered_spd(&alloc, 24, 48, 560), 2); // 560-col ROOT front receiving cmod from ~24 block supernodes
                                                // → the no-pack divergence, at ~1/100 the cost of grid3d(28)
                                                // (456 s win-debug / ~40 min win-asan — the 2026-07-02 scar)

    crd::jobs::shutdown();
}

TEST_CASE("supernodal Cholesky: multi-RHS solve (column-major B)", "[hesap][direct][v5a-1b][numeric][kernels]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    auto m = grid2d(&alloc, 6, 6); // 36 SPD
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::usize nrhs = 3;

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> rhs(&alloc);
    xtrue.resize(static_cast<crd::usize>(n) * nrhs);
    rhs.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        crd::containers::Array<crd::f64> xc(&alloc);
        crd::containers::Array<crd::f64> bc(&alloc);
        xc.resize(n);
        bc.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xc[i] = 1.0 + static_cast<crd::f64>(c) + 0.07 * static_cast<crd::f64>(i);
            xtrue[c * n + i] = xc[i];
        }
        spmv_sym(pat, vals, xc, bc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            rhs[c * n + i] = bc[i];
        }
    }
    auto f = dir::factor_supernodal_cholesky<crd::f64>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve({rhs.data(), rhs.size()}, nrhs));
    for (crd::usize i = 0; i < rhs.size(); ++i)
    {
        CHECK(std::abs(rhs[i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("supernodal Cholesky: single-RHS solve bit-identical across worker counts",
          "[hesap][direct][v5a-5][determinism][kernels]")
{
    // v5a-5: single-RHS (nrhs==1) solve must be bit-identical across worker counts. nw=1 takes the
    // dedicated SERIAL hand-axpy path; nw>1 takes the level-parallel hand-axpy path in fwd_one/back_one
    // (same kernel, same k-ascending reduction order). The v5a-4 forced-worker test below covers nrhs>1;
    // this is the moat for the new single-RHS parallel path.
    crd::memory::TlsfAllocator alloc(256 << 20);
    auto m = grid3d(&alloc, 22); // n=10648, 3D => deep etree => many levels => exercises level-parallel
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::containers::ConstSpan<crd::f64> vspan{vals.data(), vals.size()};

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.011 * static_cast<crd::f64>(i % 97);
    }
    spmv_sym(pat, vals, xtrue, b);

    crd::jobs::init();
    const crd::u32 w = crd::jobs::num_workers();
    auto f = dir::factor_supernodal_cholesky<crd::f64>(pat, vspan, &alloc, dir::kSupernodeRelax, w);
    REQUIRE(f.info() == 0);

    // Serial reference (nw=1: the dedicated hand path).
    crd::containers::Array<crd::f64> xref(&alloc);
    xref.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xref[i] = b[i];
    }
    REQUIRE(f.solve_arm(dir::detail::SupernodalPick{.solve_workers = 1}, {xref.data(), n}, 1));

    // Parallel single-RHS at several worker counts (incl. intermediate 1<nw<pool) must be BIT-IDENTICAL.
    for (crd::u32 nw : {2U, 3U, w > 3U ? w : 4U})
    {
        crd::containers::Array<crd::f64> x(&alloc);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        REQUIRE(f.solve_arm(dir::detail::SupernodalPick{.solve_workers = nw}, {x.data(), n}, 1));
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(x[i] == xref[i]); // bit-exact across worker counts — the single-RHS solve moat
        }
    }
    crd::jobs::shutdown();

    // Correctness: the serial reference recovers xtrue.
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(std::abs(xref[i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("supernodal Cholesky: solve_arm bit-identical across forced worker counts",
          "[hesap][direct][v5a-4][numeric][kernels]")
{
    // The two-path forward (nw<=1 RIGHT-looking serial, nw>1 LEFT-looking parallel) + the level-parallel
    // backward must produce BIT-IDENTICAL x at EVERY forced worker count, INCLUDING intermediate
    // 1 < nw < num_workers() (which the public solve() never passes). That path stresses the POOL-GLOBAL
    // worker_index() scratch indexing: scratch is sized by the pool, not nw, so an intermediate nw must not
    // overflow tmp/dscr. grid3d(12) has many supernodes across levels => the parallel paths actually run.
    crd::memory::TlsfAllocator alloc(64 << 20);
    crd::jobs::init();           // the parallel solve paths (nw>1) dispatch onto the worker pool
    auto m = grid3d(&alloc, 12); // 1728 SPD, multi-level supernode tree
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::usize nrhs = 4;
    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.013 * static_cast<crd::f64>(i % 53);
    }
    spmv_sym(pat, vals, xtrue, b);
    auto f = dir::factor_supernodal_cholesky<crd::f64>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0);
    const crd::u32 w = crd::jobs::num_workers();
    crd::containers::Array<crd::f64> ref(&alloc); // serial path (nw=1): right-looking forward
    ref.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            ref[c * n + i] = b[i] * (1.0 + 0.2 * static_cast<crd::f64>(c));
        }
    }
    REQUIRE(f.solve_arm(dir::detail::SupernodalPick{.solve_workers = 1}, {ref.data(), ref.size()}, nrhs));
    for (crd::u32 nw : {2U, 3U, w > 3U ? w : 4U}) // 2,3 = intermediate 1<nw<pool; last = full pool
    {
        crd::containers::Array<crd::f64> x(&alloc);
        x.resize(static_cast<crd::usize>(n) * nrhs);
        for (crd::usize c = 0; c < nrhs; ++c)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[c * n + i] = b[i] * (1.0 + 0.2 * static_cast<crd::f64>(c));
            }
        }
        REQUIRE(f.solve_arm(dir::detail::SupernodalPick{.solve_workers = nw}, {x.data(), x.size()}, nrhs));
        for (crd::usize k = 0; k < x.size(); ++k)
        {
            REQUIRE(x[k] == ref[k]); // bit-identical across forced worker counts (the solve moat)
        }
    }
    for (crd::usize c = 0; c < nrhs; ++c) // correctness: column c of the solution is (1+0.2c)*xtrue
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            CHECK(std::abs(ref[c * n + i] - (1.0 + 0.2 * static_cast<crd::f64>(c)) * xtrue[i]) < 1e-7);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("supernodal Cholesky: wide-front multi-RHS solve (batched diagonal path)",
          "[hesap][direct][v5a-4][numeric][kernels]")
{
    // dense_spd(80) = ONE supernode nc=80 >= solve_batch_min_nc (48) -> exercises the BATCHED
    // multi-RHS diagonal solve (forward + backward), the path the grid2d test (small nc) does not
    // reach. Verifies the batched diagonal sweep produces the correct solution.
    crd::memory::TlsfAllocator alloc(64 << 20);
    auto m = dense_spd(&alloc, 80);
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::usize nrhs = 5;

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> rhs(&alloc);
    xtrue.resize(static_cast<crd::usize>(n) * nrhs);
    rhs.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        crd::containers::Array<crd::f64> xc(&alloc);
        crd::containers::Array<crd::f64> bc(&alloc);
        xc.resize(n);
        bc.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xc[i] = 0.4 + static_cast<crd::f64>(c) - 0.013 * static_cast<crd::f64>(i);
            xtrue[c * n + i] = xc[i];
        }
        spmv_sym(pat, vals, xc, bc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            rhs[c * n + i] = bc[i];
        }
    }
    auto f = dir::factor_supernodal_cholesky<crd::f64>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve({rhs.data(), rhs.size()}, nrhs));
    for (crd::usize i = 0; i < rhs.size(); ++i)
    {
        CHECK(std::abs(rhs[i] - xtrue[i]) < 1e-9);
    }
}

// ---- complex Hermitian LLᴴ (v5a-2) -------------------------------------
namespace
{
using C = crd::hesap::Complex64;
using CsrC = sp::SparseMatrix<C, sp::SparseFormat::Csr>;

// A genuinely Hermitian, positive-definite matrix with NON-TRIVIAL complex
// off-diagonals (A[i][j] = conj(A[j][i]), real diagonal). Strict diagonal
// dominance with a real-positive diagonal ⇒ HPD, so it exercises the conj
// path (a real-only "complex" matrix would silently pass on the real branch).
CsrC complex_hpd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<C> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, C{static_cast<crd::f64>(2 * n), 0.0}); // real, dominant
        for (crd::u32 j = i + 1; j < n; ++j)
        {
            const crd::f64 re = 0.5 / static_cast<crd::f64>(1 + j - i);
            const crd::f64 im = 0.31 / static_cast<crd::f64>(1 + j - i);
            tb.add(i, j, C{re, im});  // upper
            tb.add(j, i, C{re, -im}); // lower = conj(upper) → Hermitian
        }
    }
    return tb.compress();
}

// b = A·x for a Hermitian A stored in full (row o = outer o's entries).
void spmv_herm(const sp::SparsePattern& pat, const crd::containers::Array<C>& vals, const crd::containers::Array<C>& x,
               crd::containers::Array<C>& b)
{
    const crd::u32 n = pat.rows;
    for (crd::u32 o = 0; o < n; ++o)
    {
        C acc{0.0, 0.0};
        const crd::u32 st = pat.outer_ptr[o];
        const crd::u32 cnt = pat.inner_count(o);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            acc += vals[st + k] * x[pat.inner_idx[st + k]];
        }
        b[o] = acc;
    }
}
} // namespace

TEST_CASE("supernodal Cholesky: complex Hermitian LL^H factor+solve residual ~0", "[hesap][direct][v5a-2][complex]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 n = 24;
    auto m = complex_hpd(&alloc, n);
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;

    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.1 * static_cast<crd::f64>(i), -0.2 * static_cast<crd::f64>(i)};
    }
    spmv_herm(pat, vals, xtrue, b);

    auto f = dir::factor_supernodal_cholesky<C>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0); // HPD detected (real-positive pivots)
    REQUIRE(f.n() == n);

    crd::containers::Array<C> x(&alloc);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    REQUIRE(f.solve({x.data(), n}));
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(crd::hesap::abs(x[i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("supernodal Cholesky: complex Hermitian multi-RHS solve", "[hesap][direct][v5a-2][complex]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    const crd::u32 n = 20;
    const crd::usize nrhs = 4;
    auto m = complex_hpd(&alloc, n);
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;

    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> rhs(&alloc);
    xtrue.resize(static_cast<crd::usize>(n) * nrhs);
    rhs.resize(static_cast<crd::usize>(n) * nrhs);
    for (crd::usize c = 0; c < nrhs; ++c)
    {
        crd::containers::Array<C> xc(&alloc);
        crd::containers::Array<C> bc(&alloc);
        xc.resize(n);
        bc.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xc[i] = C{0.5 + static_cast<crd::f64>(c), 0.1 * static_cast<crd::f64>(i) - 0.3};
            xtrue[c * n + i] = xc[i];
        }
        spmv_herm(pat, vals, xc, bc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            rhs[c * n + i] = bc[i];
        }
    }
    auto f = dir::factor_supernodal_cholesky<C>(pat, {vals.data(), vals.size()}, &alloc);
    REQUIRE(f.info() == 0);
    REQUIRE(f.solve({rhs.data(), rhs.size()}, nrhs));
    for (crd::usize i = 0; i < rhs.size(); ++i)
    {
        CHECK(crd::hesap::abs(rhs[i] - xtrue[i]) < 1e-9);
    }
}

TEST_CASE("supernodal Cholesky: complex Hermitian TREE-PARALLEL is bit-identical to serial (moat)",
          "[hesap][direct][v5a-2][complex][determinism]")
{
    crd::memory::TlsfAllocator alloc(128 << 20);
    const crd::u32 n = 60; // multi-level supernode etree → real parallelism
    auto m = complex_hpd(&alloc, n);
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::containers::ConstSpan<C> vspan{vals.data(), vals.size()};

    crd::containers::Array<C> xtrue(&alloc);
    crd::containers::Array<C> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = C{1.0 + 0.01 * static_cast<crd::f64>(i), 0.02 * static_cast<crd::f64>(i)};
    }
    spmv_herm(pat, vals, xtrue, b);

    crd::jobs::init();
    crd::containers::Array<C> x1(&alloc);
    x1.resize(n);
    {
        auto f1 = dir::factor_supernodal_cholesky<C>(pat, vspan, &alloc, dir::kSupernodeRelax, 1);
        REQUIRE(f1.info() == 0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x1[i] = b[i];
        }
        REQUIRE(f1.solve({x1.data(), n}));
    }
    const crd::u32 w = crd::jobs::num_workers();
    for (crd::u32 nw : {2U, 4U, w > 4U ? w : 4U})
    {
        auto fp = dir::factor_supernodal_cholesky<C>(pat, vspan, &alloc, dir::kSupernodeRelax, nw);
        REQUIRE(fp.info() == 0);
        crd::containers::Array<C> xp(&alloc);
        xp.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            xp[i] = b[i];
        }
        REQUIRE(fp.solve({xp.data(), n}));
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xp[i].re == x1[i].re); // bit-exact across worker counts — the complex moat
            REQUIRE(xp[i].im == x1[i].im);
        }
    }
    crd::jobs::shutdown();
    for (crd::u32 i = 0; i < n; ++i)
    {
        CHECK(crd::hesap::abs(x1[i] - xtrue[i]) < 1e-9);
    }
}

// ---- detail::SupernodalPick's arms (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE) ----------------------------------
namespace
{
using dir::detail::SupernodalPick;
template <typename T> using Mat = sp::SparseMatrix<T, sp::SparseFormat::Csr>;
template <typename T> using Real = crd::hesap::dense::RealType<T>;
constexpr crd::u32 kNeverWidth = 0xFFFFFFFFU;
constexpr crd::u64 kNeverWork = ~crd::u64{0};
constexpr crd::usize kGuard = 7; // guard words after the solve's right-hand sides

enum class Special : crd::u8
{
    None,
    NegativePivot,       // A(k, k) = -1 at k = n / 2: every arm reports info() k + 1
    NaN,                 // A(k, j) and A(j, k) NaN at k = n / 2 and j < k: every arm reports info() k + 1
    SignedZeroSubnormal, // -0 and the type's smallest subnormal in place of some off-diagonals
};

template <typename T> T scalar(crd::f64 re, crd::f64 im)
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return T{static_cast<Real<T>>(re), static_cast<Real<T>>(im)};
    }
    else
    {
        (void)im;
        return static_cast<T>(re);
    }
}

// The symmetric f64 m in T, Hermitian for the complex types: the diagonal doubled, each off-diagonal v as v (1 + i/2)
// above the diagonal and its conjugate below, so it stays diagonally dominant, hence positive definite.
template <typename T> Mat<T> in_type(crd::memory::IAllocator* a, const Csr& m, Special special)
{
    const crd::u32 n = m.rows();
    const auto& pat = m.pattern();
    const auto& vals = m.values().values;
    const crd::u32 k = n / 2;
    crd::u32 nan_col = k;
    for (crd::u32 q = pat.outer_ptr[k]; q < pat.outer_ptr[k + 1]; ++q)
    {
        if (pat.inner_idx[q] < k)
        {
            nan_col = pat.inner_idx[q]; // the last neighbour before k
        }
    }
    REQUIRE(nan_col < k);
    const crd::f64 tiny = static_cast<crd::f64>(std::numeric_limits<Real<T>>::denorm_min());
    sp::TripletBuilder<T> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            const crd::u32 j = pat.inner_idx[q];
            const crd::f64 v = vals[q];
            T x = i == j ? scalar<T>(2.0 * v, 0.0) : scalar<T>(v, i < j ? 0.5 * v : -0.5 * v);
            if (special == Special::NegativePivot && i == k && j == k)
            {
                x = scalar<T>(-1.0, 0.0);
            }
            if (special == Special::NaN && ((i == k && j == nan_col) || (i == nan_col && j == k)))
            {
                x = scalar<T>(std::numeric_limits<crd::f64>::quiet_NaN(), 0.0);
            }
            if (special == Special::SignedZeroSubnormal && i != j && (i + j) % 5 == 0)
            {
                x = scalar<T>((i + j) % 10 == 0 ? -0.0 : tiny, 0.0);
            }
            tb.add(i, j, x);
        }
    }
    return tb.compress();
}

// b = A x for the full-storage Hermitian a.
template <typename T> void apply(const Mat<T>& a, const T* x, T* b)
{
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    for (crd::u32 i = 0; i < a.rows(); ++i)
    {
        T acc{};
        for (crd::u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            acc += vals[q] * x[pat.inner_idx[q]];
        }
        b[i] = acc;
    }
}

template <typename T> crd::f64 magnitude(const T& x)
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return static_cast<crd::f64>(crd::hesap::abs(x));
    }
    else
    {
        return std::fabs(static_cast<crd::f64>(x));
    }
}

// The normwise backward error of x for A x = b, |b - A x| / (|A| |x| + |b|) in the infinity norm, within the
// declared bound 4 n eps.
template <typename T> bool within_bound(const Mat<T>& a, const T* x, const T* b, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.rows();
    crd::containers::Array<T> ax(alloc);
    ax.resize(n);
    apply(a, x, ax.data());
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    crd::f64 r = 0.0;
    crd::f64 an = 0.0;
    crd::f64 xn = 0.0;
    crd::f64 bn = 0.0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        crd::f64 row = 0.0;
        for (crd::u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            row += magnitude(vals[q]);
        }
        an = row > an ? row : an;
        const crd::f64 ri = magnitude(b[i] - ax[i]);
        r = ri > r ? ri : r;
        xn = magnitude(x[i]) > xn ? magnitude(x[i]) : xn;
        bn = magnitude(b[i]) > bn ? magnitude(b[i]) : bn;
    }
    const crd::f64 eps = static_cast<crd::f64>(std::numeric_limits<Real<T>>::epsilon());
    return r <= 4.0 * static_cast<crd::f64>(n) * eps * (an * xn + bn);
}

// info() and the lower trapezoid of every panel (each diagonal block's upper triangle is scratch), bit for bit.
template <typename T> bool same_factor(const dir::SupernodalCholesky<T>& x, const dir::SupernodalCholesky<T>& y)
{
    if (x.info() != y.info() || x.panels().size() != y.panels().size())
    {
        return false;
    }
    const dir::SupernodalSymbolic& sym = x.symbolic();
    const auto off = x.panel_offsets();
    for (crd::u32 s = 0; s < sym.nsuper; ++s)
    {
        const crd::u32 nr = sym.srowp[s + 1] - sym.srowp[s];
        const crd::u32 nc = sym.scol[s + 1] - sym.scol[s];
        for (crd::u32 c = 0; c < nc; ++c)
        {
            const crd::usize at = off[s] + static_cast<crd::usize>(c) * nr + c;
            if (std::memcmp(x.panels().data() + at, y.panels().data() + at, (nr - c) * sizeof(T)) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

template <typename T>
dir::SupernodalCholesky<T> factor_arm(const SupernodalPick& pick, const Mat<T>& m, crd::u32 jobs,
                                      crd::memory::IAllocator* alloc)
{
    const auto& vals = m.values().values;
    dir::SupernodalCholesky<T> f(alloc);
    f.factorize_arm(pick, m.pattern(), {vals.data(), vals.size()}, dir::kSupernodeRelax, jobs);
    return f;
}

struct FactorArm
{
    SupernodalPick pick;
    crd::u32 jobs;
};

// Every lever of factorize_arm on one matrix: the scheduling arms give the one-job factor's bits; each blocking arm
// gives its own one-job bits on 8 and 32 jobs with every front node-parallel and split, and keeps the bound.
template <typename T> void check_factor_arms(const Csr& base, Special special, crd::memory::IAllocator* alloc)
{
    const Mat<T> m = in_type<T>(alloc, base, special);
    const crd::u32 n = m.rows();
    const bool fails = special == Special::NegativePivot || special == Special::NaN;
    const crd::usize want_info = fails ? n / 2 + 1 : 0;
    const auto ref = factor_arm<T>(SupernodalPick{}, m, 1, alloc);
    REQUIRE(ref.info() == want_info);
    const SupernodalPick node{.node_min_cols = 0};
    const SupernodalPick split{.node_min_cols = 0, .gemm_min_flop = 0};
    const FactorArm scheduling[] = {
        {{}, 4},
        {{}, 8},
        {{}, 16},
        {{}, 32},
        {node, 8},
        {split, 4},
        {split, 8},
        {split, 16},
        {split, 32},
        {{.node_min_cols = 0, .gemm_min_flop = kNeverWork}, 8},
        {{.node_min_cols = kNeverWidth}, 8},
        {{.cmod_syrk_min = 0}, 1},
        {{.cmod_syrk_min = kNeverWidth}, 1},
        {{.node_min_cols = 0, .gemm_min_flop = 0, .cmod_syrk_min = 0}, 8},
    };
    for (crd::usize i = 0; i < std::size(scheduling); ++i)
    {
        INFO("scheduling arm " << i << " on " << scheduling[i].jobs << " jobs");
        CHECK(same_factor(factor_arm<T>(scheduling[i].pick, m, scheduling[i].jobs, alloc), ref));
    }
    const SupernodalPick blocking[] = {
        {.cdiv_scalar_max = 0},
        {.cdiv_scalar_max = kNeverWidth},
        {.cdiv_scalar_max = 0, .cdiv_block = 64, .cdiv_inner = 16},
        {.cdiv_scalar_max = 0, .cdiv_block = 40, .cdiv_inner = 24},
        {.cdiv_scalar_max = 0, .cdiv_block = 8, .cdiv_inner = 8},
        {.cdiv_scalar_max = 0, .cdiv_block = 256, .cdiv_inner = 256},
        {.cdiv_scalar_max = 0, .cdiv_block = 1, .cdiv_inner = 1},
    };
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> x(alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = scalar<T>(1.0 + 0.01 * static_cast<crd::f64>(i % 37), -0.02 * static_cast<crd::f64>(i % 11));
    }
    apply(m, xtrue.data(), b.data());
    auto bounded = [&](const dir::SupernodalCholesky<T>& f)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        return f.solve({x.data(), n}) && within_bound(m, x.data(), b.data(), alloc);
    };
    if (!fails)
    {
        CHECK(bounded(ref));
    }
    for (crd::usize i = 0; i < std::size(blocking); ++i)
    {
        INFO("blocking arm " << i);
        const auto one = factor_arm<T>(blocking[i], m, 1, alloc);
        CHECK(one.info() == want_info);
        for (const crd::u32 jobs : {8U, 32U})
        {
            SupernodalPick pick = blocking[i];
            pick.node_min_cols = 0;
            pick.gemm_min_flop = 0;
            INFO("on " << jobs << " jobs");
            CHECK(same_factor(factor_arm<T>(pick, m, jobs, alloc), one));
        }
        if (!fails)
        {
            CHECK(bounded(one));
        }
    }
}

// Every lever of solve_arm on one factor, one and five right-hand sides: each job count and the gate give the serial
// solution's bits; each diagonal form (the batch width, a blocking lever) gives its own serial bits on 8 and 32 jobs
// and keeps the bound; every arm leaves the guard words after the right-hand sides alone, and a failed factor
// refuses on every arm and leaves the right-hand sides as they were.
template <typename T> void check_solve_arms(const Csr& base, crd::memory::IAllocator* alloc)
{
    const Mat<T> m = in_type<T>(alloc, base, Special::None);
    const crd::u32 n = m.rows();
    const auto f = factor_arm<T>(SupernodalPick{}, m, 1, alloc);
    const auto bad = factor_arm<T>(SupernodalPick{}, in_type<T>(alloc, base, Special::NegativePivot), 1, alloc);
    REQUIRE(f.info() == 0);
    REQUIRE(bad.info() != 0);
    const SupernodalPick scheduling[] = {
        {.solve_workers = 4},
        {.solve_workers = 8},
        {.solve_workers = 16},
        {.solve_workers = 32},
        {.solve_min_work = 0, .solve_max_workers = 32},
    };
    const T guard = scalar<T>(-7.25, 3.5);
    for (const crd::usize nrhs : {crd::usize{1}, crd::usize{5}})
    {
        const crd::usize len = static_cast<crd::usize>(n) * nrhs;
        crd::containers::Array<T> xtrue(alloc);
        crd::containers::Array<T> b(alloc);
        xtrue.resize(len);
        b.resize(len);
        for (crd::usize c = 0; c < nrhs; ++c)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                xtrue[c * n + i] = scalar<T>(1.0 + static_cast<crd::f64>(c) + 0.01 * static_cast<crd::f64>(i % 41),
                                             0.03 * static_cast<crd::f64>(c));
            }
            apply(m, xtrue.data() + c * n, b.data() + c * n);
        }
        auto solve =
            [&](const dir::SupernodalCholesky<T>& fac, const SupernodalPick& pick, crd::containers::Array<T>& out)
        {
            out.resize(len + kGuard);
            for (crd::usize i = 0; i < len; ++i)
            {
                out[i] = b[i];
            }
            for (crd::usize i = len; i < len + kGuard; ++i)
            {
                out[i] = guard;
            }
            const bool ok = fac.solve_arm(pick, {out.data(), len}, nrhs);
            for (crd::usize i = len; i < len + kGuard; ++i)
            {
                CHECK(std::memcmp(&out[i], &guard, sizeof(T)) == 0);
            }
            return ok;
        };
        crd::containers::Array<T> ref(alloc);
        REQUIRE(solve(f, SupernodalPick{.solve_workers = 1}, ref));
        for (crd::usize c = 0; c < nrhs; ++c)
        {
            CHECK(within_bound(m, ref.data() + c * n, b.data() + c * n, alloc));
        }
        auto refuses = [&](const SupernodalPick& pick)
        {
            crd::containers::Array<T> refused(alloc);
            CHECK_FALSE(solve(bad, pick, refused));
            CHECK(std::memcmp(refused.data(), b.data(), len * sizeof(T)) == 0);
        };
        for (crd::usize i = 0; i < std::size(scheduling); ++i)
        {
            INFO("solve arm " << i << ", " << nrhs << " right-hand sides");
            crd::containers::Array<T> x(alloc);
            CHECK(solve(f, scheduling[i], x));
            CHECK(std::memcmp(x.data(), ref.data(), len * sizeof(T)) == 0);
            refuses(scheduling[i]);
        }
        for (const crd::u32 width : {0U, kNeverWidth})
        {
            INFO("batch width " << width << ", " << nrhs << " right-hand sides");
            crd::containers::Array<T> one(alloc);
            CHECK(solve(f, SupernodalPick{.solve_workers = 1, .solve_batch_min_nc = width}, one));
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                CHECK(within_bound(m, one.data() + c * n, b.data() + c * n, alloc));
            }
            for (const crd::u32 jobs : {8U, 32U})
            {
                crd::containers::Array<T> x(alloc);
                CHECK(solve(f, SupernodalPick{.solve_workers = jobs, .solve_batch_min_nc = width}, x));
                CHECK(std::memcmp(x.data(), one.data(), len * sizeof(T)) == 0);
            }
            refuses(SupernodalPick{.solve_workers = 8, .solve_batch_min_nc = width});
        }
    }
}

template <typename T> void check_all_arms(crd::memory::IAllocator* alloc)
{
    // bordered_spd: 40-column block fronts (blocked cdiv, below rows) under a 72-column root that receives every
    // block's cmod; grid2d: a deep etree of narrow fronts.
    const Csr bordered = bordered_spd(alloc, 6, 40, 72);
    const Csr grid = grid2d(alloc, 24, 24);
    for (const Special special : {Special::None, Special::NegativePivot, Special::NaN, Special::SignedZeroSubnormal})
    {
        INFO("bordered, special " << static_cast<int>(special));
        check_factor_arms<T>(bordered, special, alloc);
    }
    {
        INFO("grid");
        check_factor_arms<T>(grid, Special::None, alloc);
    }
    check_solve_arms<T>(bordered, alloc);
    check_solve_arms<T>(grid, alloc);
}
} // namespace

TEST_CASE("supernodal_cholesky_arm: every lever keeps one job's bits or the backward-error bound",
          "[hesap][direct][jobs][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    crd::jobs::Config cfg;
    cfg.num_threads = 4; // a hosted runner's pool on every host: the 8 to 32 forced jobs outnumber its workers
    crd::jobs::init(cfg);
    {
        INFO("f64");
        check_all_arms<crd::f64>(&alloc);
    }
    {
        INFO("f32");
        check_all_arms<crd::f32>(&alloc);
    }
    {
        INFO("c64");
        check_all_arms<crd::hesap::Complex64>(&alloc);
    }
    {
        INFO("c32");
        check_all_arms<crd::hesap::Complex32>(&alloc);
    }
    crd::jobs::shutdown();
}
