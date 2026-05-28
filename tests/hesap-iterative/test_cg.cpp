#include <crd/containers/span.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/iterative/cli_anchor.hpp>
#include <crd/hesap/preconditioners/block_jacobi.hpp>
#include <crd/hesap/preconditioners/cli_anchor.hpp>
#include <crd/hesap/preconditioners/jacobi.hpp>
#include <crd/hesap/preconditioners/ssor.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using namespace crd::hesap::iterative;
using namespace crd::hesap::sparse;
using namespace crd::hesap::preconditioners;
using crd::hesap::Complex;
namespace dense = crd::hesap::dense;

namespace
{
// 1D Laplacian (tridiagonal [-1, 2, -1]) -- SPD, the canonical CG test.
template <typename T> SparseMatrix<T, SparseFormat::Csr> laplacian_1d(crd::memory::IAllocator* a, crd::u32 n)
{
    TripletBuilder<T> b(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, T(2));
        if (i > 0)
        {
            b.add(i, i - 1, T(-1));
        }
        if (i + 1 < n)
        {
            b.add(i, i + 1, T(-1));
        }
    }
    return b.compress();
}

// Relative residual ‖A·x − b‖₂ / ‖b‖₂.
template <typename T>
crd::hesap::dense::RealType<T> rel_residual(const SparseLinearOp<T>& op, crd::containers::ConstSpan<T> x,
                                            crd::containers::ConstSpan<T> b, crd::memory::IAllocator* a)
{
    dense::Vector<T> ax(a, x.size());
    (void)op.apply(x, ax.span());
    dense::Vector<T> diff(a, x.size());
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        diff(i) = ax(i) - b[i];
    }
    return dense::nrm2<T>(diff.span()) / dense::nrm2<T>(b);
}
} // namespace

TEST_CASE("CG solves a 1D Laplacian SPD system (f64)", "[hesap-iterative][cg]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    const crd::u32 n = 64;
    auto a = laplacian_1d<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);

    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n); // x0 = 0

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    opts.record_residuals = true;
    KrylovWorkspace<crd::f64> ws(&alloc, n);

    auto res = cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);

    REQUIRE(res.converged);
    REQUIRE(res.reason == StopReason::Converged);
    REQUIRE(res.iterations <= n); // CG converges in ≤ n steps (exact arithmetic)
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-10);
    // residual history is monotone-ish and ends at the final norm.
    REQUIRE(res.residual_history.size() == res.iterations + 1);
}

TEST_CASE("PCG with Jacobi matches CG solution (f64)", "[hesap-iterative][pcg]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    const crd::u32 n = 64;
    auto a = laplacian_1d<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    JacobiPreconditioner<crd::f64> m(a, &alloc);

    dense::Vector<crd::f64> b(&alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b(i) = static_cast<crd::f64>(i + 1);
    }
    dense::Vector<crd::f64> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<crd::f64> ws(&alloc, n);

    auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("CG is bit-deterministic across repeated runs (f64)", "[hesap-iterative][cg][determinism]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    const crd::u32 n = 50;
    auto a = laplacian_1d<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);

    auto run = [&](dense::Vector<crd::f64>& x)
    {
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-12;
        KrylovWorkspace<crd::f64> ws(&alloc, n);
        return cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
    };

    dense::Vector<crd::f64> x1(&alloc, n);
    dense::Vector<crd::f64> x2(&alloc, n);
    auto r1 = run(x1);
    auto r2 = run(x2);
    REQUIRE(r1.iterations == r2.iterations);
    REQUIRE(r1.final_residual_norm == r2.final_residual_norm); // bit-exact
    for (crd::u32 i = 0; i < n; ++i)
    {
        REQUIRE(x1(i) == x2(i)); // bit-exact solution
    }
}

TEST_CASE("CG solves a Hermitian positive-definite system (c64)", "[hesap-iterative][cg][complex]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    using C = Complex<crd::f64>;
    const crd::u32 n = 16;

    // Diagonally dominant Hermitian: diag 4, off-diag (i,i+1)=(1,0.5),
    // (i+1,i)=conj=(1,-0.5). HPD (strictly diagonally dominant, real-positive diag).
    TripletBuilder<C> bld(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{4.0, 0.0});
        if (i + 1 < n)
        {
            bld.add(i, i + 1, C{1.0, 0.5});
            bld.add(i + 1, i, C{1.0, -0.5});
        }
    }
    auto a = bld.compress();
    SparseLinearOp<C> op(a);

    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<C> ws(&alloc, n);

    auto res = cg<C>(op, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-10);
}

namespace
{
// Diagonally dominant Hermitian PD: diag 4, off-diag (i,i+1)=(1,0.5), conj below.
SparseMatrix<Complex<crd::f64>, SparseFormat::Csr> hermitian_pd(crd::memory::IAllocator* a, crd::u32 n)
{
    using C = Complex<crd::f64>;
    TripletBuilder<C> bld(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        bld.add(i, i, C{4.0, 0.0});
        if (i + 1 < n)
        {
            bld.add(i, i + 1, C{1.0, 0.5});
            bld.add(i + 1, i, C{1.0, -0.5});
        }
    }
    return bld.compress();
}
} // namespace

TEST_CASE("PCG with complex block-Jacobi solves an HPD system (c64)", "[hesap-iterative][pcg][block-jacobi][complex]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    using C = Complex<crd::f64>;
    const crd::u32 n = 24;
    auto a = hermitian_pd(&alloc, n);
    SparseLinearOp<C> op(a);
    BlockJacobiPreconditioner<C> m(a, /*block_size=*/4, &alloc);

    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<C> ws(&alloc, n);

    auto res = pcg<C>(op, m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("PCG with complex SSOR solves an HPD system (c64)", "[hesap-iterative][pcg][ssor][complex]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    using C = Complex<crd::f64>;
    const crd::u32 n = 24;
    auto a = hermitian_pd(&alloc, n);
    SparseLinearOp<C> op(a);
    SsorPreconditioner<C> m(a, /*omega=*/1.0, &alloc);

    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<C> ws(&alloc, n);

    auto res = pcg<C>(op, m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("PCG with complex point-Jacobi solves an HPD system (c64; complex completeness)",
          "[hesap-iterative][pcg][jacobi][complex]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    using C = Complex<crd::f64>;
    const crd::u32 n = 24;
    auto a = hermitian_pd(&alloc, n);
    SparseLinearOp<C> op(a);
    JacobiPreconditioner<C> m(a, &alloc); // point-Jacobi: the complex inverse-diagonal apply path

    dense::Vector<C> b(&alloc, n);
    b.fill(C{1.0, 0.0});
    dense::Vector<C> x(&alloc, n);
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<C> ws(&alloc, n);

    auto res = pcg<C>(op, m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<C>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("Jacobi preconditioner applies the inverse diagonal", "[hesap-iterative][jacobi]")
{
    crd::memory::TlsfAllocator alloc{1U << 20};
    const crd::u32 n = 4;
    TripletBuilder<crd::f64> bld(&alloc, n, n);
    bld.add(0, 0, 2.0);
    bld.add(1, 1, 4.0);
    bld.add(2, 2, 8.0);
    bld.add(3, 3, 0.5);
    bld.add(0, 1, 9.0); // off-diagonal ignored by Jacobi
    auto a = bld.compress();
    JacobiPreconditioner<crd::f64> m(a, &alloc);

    dense::Vector<crd::f64> r(&alloc, n);
    r.fill(1.0);
    dense::Vector<crd::f64> z(&alloc, n);
    (void)m.apply(r.span(), z.span());
    REQUIRE(z(0) == 0.5);
    REQUIRE(z(1) == 0.25);
    REQUIRE(z(2) == 0.125);
    REQUIRE(z(3) == 2.0);
}

TEST_CASE("PCG with block-Jacobi solves the Laplacian (f64)", "[hesap-iterative][pcg][block-jacobi]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    const crd::u32 n = 64;
    auto a = laplacian_1d<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    BlockJacobiPreconditioner<crd::f64> m(a, /*block_size=*/8, &alloc);

    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<crd::f64> ws(&alloc, n);

    auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-10);
}

TEST_CASE("PCG with SSOR solves the Laplacian (f64)", "[hesap-iterative][pcg][ssor]")
{
    crd::memory::TlsfAllocator alloc{4U << 20};
    const crd::u32 n = 64;
    auto a = laplacian_1d<crd::f64>(&alloc, n);
    SparseLinearOp<crd::f64> op(a);
    SsorPreconditioner<crd::f64> m(a, /*omega=*/1.0, &alloc);

    dense::Vector<crd::f64> b(&alloc, n);
    b.fill(1.0);
    dense::Vector<crd::f64> x(&alloc, n);

    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-12;
    KrylovWorkspace<crd::f64> ws(&alloc, n);

    auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
    REQUIRE(res.converged);
    REQUIRE(rel_residual<crd::f64>(op, x.span(), b.span(), &alloc) < 1e-10);
}

// THE DETERMINISM MOAT: CG over the parallel SELL spmv yields a bit-identical
// {iterations, residual sequence, solution} as CG over the serial spmv. The
// parallel spmv is bit-exact vs serial at any worker count (v1b), and the
// Krylov reductions are KBN-serial-deterministic ⇒ the whole solve is
// thread-count independent. No frontier library ships this.
TEST_CASE("CG is bit-exact over serial vs parallel spmv (determinism moat)", "[hesap-iterative][determinism]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{16U << 20};
        const crd::u32 n = 200;
        auto a = laplacian_1d<crd::f64>(&alloc, n);

        SparseLinearOp<crd::f64> serial_op(a);
        // Force the parallel path (threshold 0) so the gate truly compares
        // parallel-vs-serial spmv, not serial-vs-serial.
        ParallelSparseLinearOp<crd::f64> parallel_op(a, &alloc, /*parallel_min_stored_bytes=*/0);
        REQUIRE(parallel_op.is_parallel());

        dense::Vector<crd::f64> b(&alloc, n);
        b.fill(1.0);

        auto solve = [&](const crd::hesap::LinearOp<crd::f64>& op, dense::Vector<crd::f64>& x)
        {
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-12;
            opts.record_residuals = true;
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            return cg<crd::f64>(op, b.span(), x.span(), opts, ws, &alloc);
        };

        dense::Vector<crd::f64> xs(&alloc, n);
        dense::Vector<crd::f64> xp(&alloc, n);
        auto rs = solve(serial_op, xs);
        auto rp = solve(parallel_op, xp);

        REQUIRE(rs.iterations == rp.iterations);
        REQUIRE(rs.residual_history.size() == rp.residual_history.size());
        for (crd::usize i = 0; i < rs.residual_history.size(); ++i)
        {
            REQUIRE(rs.residual_history[i] == rp.residual_history[i]); // bit-exact
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            REQUIRE(xs(i) == xp(i)); // bit-exact solution
        }
    }
    crd::jobs::shutdown();
}

// ---- CLI ---------------------------------------------------------------

namespace
{
const bool kPullIter = (crd::hesap::iterative::register_iterative_cli_anchor(), true);
const bool kPullPrecon = (crd::hesap::preconditioners::register_preconditioners_cli_anchor(), true);
} // namespace

TEST_CASE("CLI hesap.iterative.cg solves and returns [iters,resid,converged,x]", "[hesap-iterative][cli]")
{
    REQUIRE(kPullIter);
    using namespace crd::hesap::cli;
    crd::memory::TlsfAllocator alloc{4U << 20};

    const auto* rec = CommandRegistry::global().find("hesap.iterative.cg.f64");
    REQUIRE(rec != nullptr);

    // n=3 1D Laplacian, b = ones.
    crd::i64 tr[] = {0, 0, 1, 1, 1, 2, 2};
    crd::i64 tc[] = {0, 1, 0, 1, 2, 1, 2};
    crd::f64 vv[] = {2, -1, -1, 2, -1, -1, 2};
    crd::f64 bb[] = {1, 1, 1};
    CommandArgs args{&alloc};
    args.set_u64("rows", 3);
    args.set_u64("cols", 3);
    args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{tr, 7});
    args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{tc, 7});
    args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{vv, 7});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{bb, 3});

    auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&r.value);
    REQUIRE(blob != nullptr);
    REQUIRE(blob->bytes.size() == 6U * sizeof(crd::f64)); // [iters,resid,conv, x0,x1,x2]
    crd::f64 out[6];
    std::memcpy(out, blob->bytes.data(), sizeof(out));
    REQUIRE(out[2] == 1.0); // converged
    // Exact solution of [-1,2,-1] x = 1 (n=3): x = [1.5, 2, 1.5].
    REQUIRE(std::abs(out[3] - 1.5) < 1e-8);
    REQUIRE(std::abs(out[4] - 2.0) < 1e-8);
    REQUIRE(std::abs(out[5] - 1.5) < 1e-8);
}

TEST_CASE("CLI hesap.iterative.pcg (Jacobi) solves", "[hesap-iterative][cli][pcg]")
{
    REQUIRE(kPullPrecon);
    using namespace crd::hesap::cli;
    crd::memory::TlsfAllocator alloc{4U << 20};

    const auto* rec = CommandRegistry::global().find("hesap.iterative.pcg.f64");
    REQUIRE(rec != nullptr);

    crd::i64 tr[] = {0, 0, 1, 1, 1, 2, 2};
    crd::i64 tc[] = {0, 1, 0, 1, 2, 1, 2};
    crd::f64 vv[] = {2, -1, -1, 2, -1, -1, 2};
    crd::f64 bb[] = {1, 1, 1};
    CommandArgs args{&alloc};
    args.set_u64("rows", 3);
    args.set_u64("cols", 3);
    args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{tr, 7});
    args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{tc, 7});
    args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{vv, 7});
    args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{bb, 3});

    auto r = rec->impl(args);
    REQUIRE(r.ok);
    const auto* blob = std::get_if<ResultBinaryBlob>(&r.value);
    REQUIRE(blob != nullptr);
    crd::f64 out[6];
    std::memcpy(out, blob->bytes.data(), sizeof(out));
    REQUIRE(out[2] == 1.0);
    REQUIRE(std::abs(out[3] - 1.5) < 1e-8);
    REQUIRE(std::abs(out[4] - 2.0) < 1e-8);
    REQUIRE(std::abs(out[5] - 1.5) < 1e-8);
}
