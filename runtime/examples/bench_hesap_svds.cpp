// bench_hesap_svds — v6-z SVD crush verdict: Cerid IRLBA (svds) vs scipy.svds / primme.svds (scripts/svds_ref.py)
// for the largest singular triplets of a tall sparse matrix — the 2D rectangular-grid edge-NODE incidence matrix
// B (m_edges x n_nodes, m ~ 2n, 2 nonzeros/row). B's singular values are sqrt(graph-Laplacian eigenvalues), a
// real, reproducible sparse-SVD problem. Same matrix is built bit-identically in svds_ref.py (same edge order).
//
// HONEST metric (advisor-locked): WALL-CLOCK is the comparable metric (Cerid C++ vs scipy/PRIMME, all compiled).
// Singular values must AGREE across peers (the accuracy gate) before comparing cost. Expected honest verdict:
// PARITY (IRLBA and scipy/PRIMME svds are all Krylov-bidiagonalization) + the {1..16} determinism moat the peers
// lack. Build in release for the timing; the Python peer runs separately (scripts/svds_ref.py).

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace eig = crd::hesap::eigen;
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// Edge-node incidence matrix of a gx x gy grid (node id = i*gy + j). Horizontal edges first (i outer, j inner),
// then vertical — the SAME order as svds_ref.py. Row e: +1 at node_a, -1 at node_b. m x n, m = gx*(gy-1)+(gx-1)*gy.
Csr incidence_2d(crd::memory::IAllocator* a, crd::u32 gx, crd::u32 gy)
{
    const crd::u32 n = gx * gy;
    const crd::u32 m = gx * (gy - 1) + (gx - 1) * gy;
    sp::TripletBuilder<crd::f64> tb(a, m, n);
    auto id = [gy](crd::u32 i, crd::u32 j) { return i * gy + j; };
    crd::u32 e = 0;
    for (crd::u32 i = 0; i < gx; ++i)
    {
        for (crd::u32 j = 0; j + 1 < gy; ++j)
        {
            tb.add(e, id(i, j), 1.0);
            tb.add(e, id(i, j + 1), -1.0);
            ++e;
        }
    }
    for (crd::u32 i = 0; i + 1 < gx; ++i)
    {
        for (crd::u32 j = 0; j < gy; ++j)
        {
            tb.add(e, id(i, j), 1.0);
            tb.add(e, id(i + 1, j), -1.0);
            ++e;
        }
    }
    return tb.compress();
}

double secs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}
} // namespace

int main(int argc, char** argv)
{
    crd::u32 gs[] = {32, 48, 64, 96};
    std::printf("%4s %9s %9s %9s %9s  %s\n", "gx", "m", "n", "cycles", "svds_s", "largest4");
    for (crd::u32 mi = 0; mi < (argc > 1 ? static_cast<crd::u32>(argc - 1) : 4U); ++mi)
    {
        const crd::u32 gx = (argc > 1) ? static_cast<crd::u32>(std::atoi(argv[mi + 1])) : gs[mi];
        const crd::u32 gy = gx + 3; // rectangular ⇒ non-degenerate
        crd::memory::GrowableTlsfAllocator alloc;
        Csr b = incidence_2d(&alloc, gx, gy);
        const crd::u32 m = b.rows();
        const crd::u32 n = b.cols();

        sp::SparseLinearOp<crd::f64> op(b);
        eig::EigenOptions<crd::f64> opts;
        opts.nev = 4;
        opts.tol = 1e-7;
        opts.max_restarts = 300;
        const auto t0 = Clock::now();
        auto r = eig::svds<crd::f64>(op, opts, &alloc);
        const auto t1 = Clock::now();

        double sm[4] = {0, 0, 0, 0};
        for (crd::u32 c = 0; c < 4 && c < r.values.size(); ++c)
        {
            sm[c] = r.values[c];
        }
        std::sort(sm, sm + 4, [](double x, double y) { return x > y; }); // descending
        std::printf("%4u %9u %9u %9u %9.4f  [%.5f,%.5f,%.5f,%.5f] (conv=%s)\n", gx, m, n, r.iterations,
                    secs(t0, t1), sm[0], sm[1], sm[2], sm[3], r.converged ? "yes" : "NO");
    }
    return 0;
}
