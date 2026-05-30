// crd-hesap-direct v5a-1b — supernodal symbolic (relaxed amalgamation) tests.

#include <crd/hesap/complex.hpp>
#include <crd/hesap/direct/direct.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <initializer_list>

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
            tb.add(i, i - 1, -1.0);
        if (i + 1 < n)
            tb.add(i, i + 1, -1.0);
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
                tb.add(v, idx(r - 1, c), -1.0);
            if (r + 1 < h)
                tb.add(v, idx(r + 1, c), -1.0);
            if (c > 0)
                tb.add(v, idx(r, c - 1), -1.0);
            if (c + 1 < w)
                tb.add(v, idx(r, c + 1), -1.0);
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
                    tb.add(v, idx(x - 1, y, z), -1.0);
                if (x + 1 < s)
                    tb.add(v, idx(x + 1, y, z), -1.0);
                if (y > 0)
                    tb.add(v, idx(x, y - 1, z), -1.0);
                if (y + 1 < s)
                    tb.add(v, idx(x, y + 1, z), -1.0);
                if (z > 0)
                    tb.add(v, idx(x, y, z - 1), -1.0);
                if (z + 1 < s)
                    tb.add(v, idx(x, y, z + 1), -1.0);
            }
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

TEST_CASE("supernodal Cholesky factor+solve: residual ~0 on a 2D-grid SPD", "[hesap][direct][v5a-1b][numeric]")
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
          "[hesap][direct][v5a-3][determinism]")
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

// v5a-4 moat: the grid2d(144) test above never produces a supernode ≥ kNodeParallelMinCols=512,
// so it exercises only the TREE-parallel path. The two-level cdiv (v5a-4) and the no-pack cmod
// (v5a-4) introduce code that DIVERGES by worker count ONLY on a node-parallel front (serial-gemm
// /no-pack at 1 worker vs generic gemm_parallel at N). This test factors matrices whose near-root
// supernode genuinely exceeds 512 — so the divergent paths actually run — and REQUIRES the factor
// (hence the solve) bit-identical across {1,2,4,8} workers. Capped at 8 workers (i9-14900K host).
TEST_CASE("supernodal Cholesky: fat-front NODE-PARALLEL factor is bit-identical to serial (v5a-4 moat)",
          "[hesap][direct][v5a-4][determinism]")
{
    crd::memory::TlsfAllocator alloc(512 << 20);
    crd::jobs::Config cfg;
    cfg.num_threads = 8; // cap host load AND still trigger node-parallel (cnt<8 at thin root levels)
    crd::jobs::init(cfg);
    const crd::u32 w = crd::jobs::num_workers();

    auto moat = [&](const Csr& m)
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
        // LEFT-looking forward + level-parallel backward) must be BIT-IDENTICAL. solve_with_workers forces
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
            REQUIRE(f1.solve_with_workers({xss.data(), xss.size()}, nrhs_m, 1));
            REQUIRE(f1.solve_with_workers({xsp.data(), xsp.size()}, nrhs_m, w > 1U ? w : 2U));
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

    moat(dense_spd(&alloc, 600)); // ONE supernode nc=600 → two-level cdiv runs NODE-parallel
    moat(grid3d(&alloc, 28));     // 21952-node; fat near-root front WITH cmod → no-pack divergence

    crd::jobs::shutdown();
}

TEST_CASE("supernodal Cholesky: multi-RHS solve (column-major B)", "[hesap][direct][v5a-1b][numeric]")
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
          "[hesap][direct][v5a-5][determinism]")
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
    REQUIRE(f.solve_with_workers({xref.data(), n}, 1, 1));

    // Parallel single-RHS at several worker counts (incl. intermediate 1<nw<pool) must be BIT-IDENTICAL.
    for (crd::u32 nw : {2U, 3U, w > 3U ? w : 4U})
    {
        crd::containers::Array<crd::f64> x(&alloc);
        x.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = b[i];
        }
        REQUIRE(f.solve_with_workers({x.data(), n}, 1, nw));
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

TEST_CASE("supernodal Cholesky: solve_with_workers bit-identical across forced worker counts",
          "[hesap][direct][v5a-4][numeric]")
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
    REQUIRE(f.solve_with_workers({ref.data(), ref.size()}, nrhs, 1));
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
        REQUIRE(f.solve_with_workers({x.data(), x.size()}, nrhs, nw));
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

TEST_CASE("supernodal Cholesky: wide-front multi-RHS solve (batched diagonal path)", "[hesap][direct][v5a-4][numeric]")
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
