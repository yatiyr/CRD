// bench_hesap_eigen_gen_lobpcg — v6-e-d GENERALIZED same-class floor: Cerid AMG-LOBPCG on the GENERALIZED FEM
// modal problem K·x = λ·M·x (the K·x = λ·M·x form eylem/structural modal analysis actually solves). Prints
// Cerid's iterations-to-tolerance; the same-class peer is `scripts/eigen_floor_gen_pyamg.py`
// (pyamg.smoothed_aggregation_solver(K) + scipy.sparse.linalg.lobpcg(K, X, B=M, M=amg_on_K)) — EXACTLY this
// method. The honest expectation (advisor-gated) is PARITY + the determinism MOAT, NOT a crush: same algorithm
// ⇒ the fair metric is ITERATIONS-TO-TOLERANCE (comparable across implementations).
//
// CRITICAL (advisor): the matrices must be reproduced BIT-FOR-BIT identically in C++ and Python, or the
// iteration-count parity is meaningless. Both build K with the SAME node ordering id = (i·my+j)·mz+l and the
// SAME diagonal mass m_node = 1 + ((node·37+11) mod 100)/100 (integer/100.0 ⇒ identical IEEE doubles). Both run
// single-threaded; cross-check the printed smallest-4 eigenvalues match the Python peer (same matrices ⇒ same
// eigenvalues = same accuracy), then compare the iteration counts. NOT linked into any external reference.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <algorithm>
#include <cstdio>

namespace
{
namespace sp = crd::hesap::sparse;
namespace eig = crd::hesap::eigen;
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// 7-point 3D Dirichlet Laplacian (the stiffness K) on a RECTANGULAR mx·my·mz grid: diag 6, 6 neighbours −1.
// Node id = (i·my+j)·mz+l — the SAME ordering eigen_floor_gen_pyamg.py builds with (NOT scipy kronsum, whose
// ordering would mismatch the per-node mass). Distinct dims ⇒ non-degenerate modes.
Csr stiffness_3d(crd::memory::IAllocator* a, crd::u32 mx, crd::u32 my, crd::u32 mz)
{
    const crd::u32 n = mx * my * mz;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto id = [my, mz](crd::u32 i, crd::u32 j, crd::u32 l) { return (i * my + j) * mz + l; };
    for (crd::u32 i = 0; i < mx; ++i)
    {
        for (crd::u32 j = 0; j < my; ++j)
        {
            for (crd::u32 l = 0; l < mz; ++l)
            {
                const crd::u32 d = id(i, j, l);
                tb.add(d, d, 6.0);
                if (i + 1 < mx)
                {
                    tb.add(d, id(i + 1, j, l), -1.0);
                    tb.add(id(i + 1, j, l), d, -1.0);
                }
                if (j + 1 < my)
                {
                    tb.add(d, id(i, j + 1, l), -1.0);
                    tb.add(id(i, j + 1, l), d, -1.0);
                }
                if (l + 1 < mz)
                {
                    tb.add(d, id(i, j, l + 1), -1.0);
                    tb.add(id(i, j, l + 1), d, -1.0);
                }
            }
        }
    }
    return tb.compress();
}

// Diagonal (lumped) mass M: m_node = 1 + ((node·37+11) mod 100)/100 ∈ [1, 2). Integer/100.0 ⇒ identical IEEE
// doubles in C++ and Python. SPD by construction.
Csr lumped_mass(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 node = 0; node < n; ++node)
    {
        tb.add(node, node, 1.0 + static_cast<double>((node * 37U + 11U) % 100U) / 100.0);
    }
    return tb.compress();
}
} // namespace

int main(int argc, char** argv)
{
    crd::u32 ss[] = {16, 20, 24, 28, 32};
    std::printf("%4s %9s %13s %9s %s\n", "s", "n", "amg_nnz", "nnz/row", "smallest4 (iters,conv)");
    for (crd::u32 mi = 0; mi < (argc > 1 ? static_cast<crd::u32>(argc - 1) : 5U); ++mi)
    {
        const crd::u32 s = (argc > 1) ? static_cast<crd::u32>(std::atoi(argv[mi + 1])) : ss[mi];
        const crd::u32 mx = s; // rectangular ⇒ non-degenerate
        const crd::u32 my = s + 3;
        const crd::u32 mz = s + 7;
        const crd::u32 n = mx * my * mz;
        crd::memory::GrowableTlsfAllocator alloc;
        Csr k = stiffness_3d(&alloc, mx, my, mz);
        Csr m = lumped_mass(&alloc, n);

        crd::hesap::amg::SaAmg<crd::f64> amg(k, &alloc); // AMG on the stiffness K (T ≈ K⁻¹) — matches the peer

        sp::SparseLinearOp<crd::f64> kop(k);
        sp::SparseLinearOp<crd::f64> mop(m);
        eig::EigenOptions<crd::f64> opts;
        opts.nev = 4;
        opts.which = eig::Which::SmallestAlgebraic;
        opts.tol = 1e-7;
        opts.max_restarts = 500;
        auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc, &amg);

        double sm[4];
        for (crd::u32 c = 0; c < 4; ++c)
        {
            sm[c] = r.values[c].re;
        }
        std::sort(sm, sm + 4);
        const crd::usize anz = amg.operator_complexity();
        std::printf("%4u %9u %13llu %9.1f [%.6f,%.6f,%.6f,%.6f] (%u,%s)\n", s, n,
                    static_cast<unsigned long long>(anz), static_cast<double>(anz) / n, sm[0], sm[1], sm[2], sm[3],
                    r.iterations, r.converged ? "yes" : "NO");
    }
    return 0;
}
