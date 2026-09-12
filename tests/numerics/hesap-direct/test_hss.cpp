#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/hss.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::hesap::dense::Matrix;
using crd::hesap::direct::build_cluster_tree;
using crd::hesap::direct::build_hss_from_dense;
using crd::hesap::direct::hss_matvec;
using crd::hesap::direct::hss_to_dense;
using crd::hesap::direct::HssMatrix;
using Catch::Matchers::WithinAbs;

namespace
{
// Fill a 1x1 generator matrix.
template <typename T>
void set_scalar(Matrix<double>& m, crd::memory::IAllocator* alloc, double v)
{
    m = Matrix<double>(alloc, 1, 1);
    m.at(0, 0) = v;
}

// Build the depth-2, 4-leaf SYMMETRIC HSS used as the hand oracle. All ranks
// are 1, so every translation/coupling is a scalar; the 2x1 leaf bases are
// distinct axis unit vectors so a U/Uᵀ transpose bug shifts the coupling entry.
// Returns the matrix; `expected` is filled INDEPENDENTLY (hardcoded ground
// truth, derived by hand) by the caller.
void build_hand_hss(HssMatrix<double>& h, crd::memory::IAllocator* alloc)
{
    build_cluster_tree<double>(h, 8, 2);
    // Topology (pre-order ids): 0=root(1,4) 1=int[0,4)(2,3) 2=leaf[0,2) 3=leaf[2,4)
    //                            4=int[4,8)(5,6) 5=leaf[4,6) 6=leaf[6,8)
    REQUIRE(h.num_nodes() == 7);
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        h.nodes[id].rank = 1;
    }

    // --- leaf diagonal blocks D (symmetric 2x2) + bases U (2x1 unit) + R (1x1) ---
    auto fill_leaf = [&](crd::usize id, double d00, double d01, double d11, double u0, double u1, double rval) {
        h.nodes[id].d = Matrix<double>(alloc, 2, 2);
        h.nodes[id].d.at(0, 0) = d00;
        h.nodes[id].d.at(0, 1) = d01;
        h.nodes[id].d.at(1, 0) = d01;
        h.nodes[id].d.at(1, 1) = d11;
        h.nodes[id].u = Matrix<double>(alloc, 2, 1);
        h.nodes[id].u.at(0, 0) = u0;
        h.nodes[id].u.at(1, 0) = u1;
        set_scalar<double>(h.nodes[id].r, alloc, rval);  // leaf -> internal translation
    };
    fill_leaf(2, 4.0, 1.0, 4.0, 1.0, 0.0, 2.0);  // leaf[0,2): U=e0, R_2=2
    fill_leaf(3, 5.0, 2.0, 5.0, 0.0, 1.0, 3.0);  // leaf[2,4): U=e1, R_3=3
    fill_leaf(5, 6.0, 1.0, 6.0, 1.0, 0.0, 1.5);  // leaf[4,6): U=e0, R_5=1.5
    fill_leaf(6, 7.0, 2.0, 7.0, 0.0, 1.0, 0.5);  // leaf[6,8): U=e1, R_6=0.5

    // --- internal nodes: coupling B + translation R (R of root's children is
    //     vacuous but must be allocated; value is unused) ---
    set_scalar<double>(h.nodes[1].b, alloc, 0.7);  // b_node1: couples leaf2,leaf3
    set_scalar<double>(h.nodes[1].r, alloc, 1.0);  // unused (parent is root)
    set_scalar<double>(h.nodes[4].b, alloc, 0.9);  // b_node4: couples leaf5,leaf6
    set_scalar<double>(h.nodes[4].r, alloc, 1.0);  // unused
    set_scalar<double>(h.nodes[0].b, alloc, 0.3);  // b_root: couples node1,node4
}

// The hand-derived dense 8x8 (symmetric). Diagonal blocks D_k; couplings:
//   (k,l) within node1 -> b_node1; within node4 -> b_node4; across -> R_k·b_root·R_l.
// With axis unit bases the coupling lands at one entry per off-diagonal block.
Matrix<double> hand_expected(crd::memory::IAllocator* alloc)
{
    Matrix<double> e(alloc, 8, 8);
    e.set_zero();
    // D_2 [0,2), D_3 [2,4), D_5 [4,6), D_6 [6,8)
    const double dblk[4][3] = {{4, 1, 4}, {5, 2, 5}, {6, 1, 6}, {7, 2, 7}};
    for (crd::usize b = 0; b < 4; ++b)
    {
        const crd::usize o = 2 * b;
        e.at(o, o) = dblk[b][0];
        e.at(o, o + 1) = dblk[b][1];
        e.at(o + 1, o) = dblk[b][1];
        e.at(o + 1, o + 1) = dblk[b][2];
    }
    // Couplings (and symmetric transposes).
    auto cpl = [&](crd::usize i, crd::usize j, double v) {
        e.at(i, j) = v;
        e.at(j, i) = v;
    };
    cpl(0, 3, 0.7);   // leaf2(U=e0,row0) x leaf3(U=e1,col3): b_node1
    cpl(4, 7, 0.9);   // leaf5(row4) x leaf6(col7): b_node4
    cpl(0, 4, 0.9);   // leaf2 x leaf5: 2*0.3*1.5
    cpl(0, 7, 0.3);   // leaf2 x leaf6: 2*0.3*0.5
    cpl(3, 4, 1.35);  // leaf3(row3) x leaf5(col4): 3*0.3*1.5
    cpl(3, 7, 0.45);  // leaf3 x leaf6: 3*0.3*0.5
    return e;
}

double rnd(crd::usize a, crd::usize b) noexcept
{
    return std::sin(static_cast<double>(a * 17 + b * 5 + 1) * 0.31) +
           std::cos(static_cast<double>(a * 3 + b * 11 + 2) * 0.19);
}

void fill_rand(Matrix<double>& m, crd::memory::IAllocator* alloc, crd::usize rows, crd::usize cols, crd::usize seed)
{
    m = Matrix<double>(alloc, rows, cols);
    for (crd::usize i = 0; i < rows; ++i)
    {
        for (crd::usize j = 0; j < cols; ++j)
        {
            m.at(i, j) = rnd(seed * 97 + i, j);
        }
    }
}

// A VARIED-RANK valid symmetric-tree HSS with pseudo-random generators. Ranks
// {root1, node1=2, leaf2=2, leaf3=2, node4=1, leaf5=2, leaf6=1} ⇒ rank>1 bases,
// a NON-SQUARE B (node4.b is 2x1, root.b is 2x1), and (via pairs whose LCA is
// the root with a right-child skeleton) the mm_bt B-orientation branch. No
// symmetry/orthonormality constraint is needed: any consistent-dimension
// generators define a valid HSS for matvec/reconstruct.
void build_varied_hss(HssMatrix<double>& h, crd::memory::IAllocator* alloc)
{
    build_cluster_tree<double>(h, 8, 2);
    const crd::usize ranks[7] = {1, 2, 2, 2, 1, 2, 1};
    for (crd::usize id = 0; id < 7; ++id)
    {
        h.nodes[id].rank = ranks[id];
    }
    for (crd::usize id = 0; id < 7; ++id)
    {
        auto& node = h.nodes[id];
        if (node.is_leaf)
        {
            fill_rand(node.d, alloc, node.size(), node.size(), id * 4 + 1);
            fill_rand(node.u, alloc, node.size(), node.rank, id * 4 + 2);
        }
        else
        {
            const crd::usize rl = h.nodes[static_cast<crd::usize>(node.left)].rank;
            const crd::usize rr = h.nodes[static_cast<crd::usize>(node.right)].rank;
            fill_rand(node.b, alloc, rl, rr, id * 4 + 3);  // B_{left,right}
        }
        if (node.parent >= 0)
        {
            const crd::usize rp = h.nodes[static_cast<crd::usize>(node.parent)].rank;
            fill_rand(node.r, alloc, node.rank, rp, id * 4 + 4);  // translation R_k
        }
    }
}
} // namespace

TEST_CASE("hss: reconstruct matches the hand-built dense oracle", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    HssMatrix<double> h(&alloc);
    build_hand_hss(h, &alloc);
    const Matrix<double> expected = hand_expected(&alloc);

    const Matrix<double> dense = hss_to_dense<double>(&alloc, h);
    REQUIRE(dense.rows() == 8);
    REQUIRE(dense.cols() == 8);
    for (crd::usize i = 0; i < 8; ++i)
    {
        for (crd::usize j = 0; j < 8; ++j)
        {
            CHECK_THAT(dense.at(i, j), WithinAbs(expected.at(i, j), 1e-12));
        }
    }
}

TEST_CASE("hss: matvec matches the hand-built dense oracle (independent path)", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    HssMatrix<double> h(&alloc);
    build_hand_hss(h, &alloc);
    const Matrix<double> expected = hand_expected(&alloc);

    // Column-by-column: matvec(H, e_j) must equal column j of the oracle. This
    // exercises the sweep path entirely independently of hss_to_dense.
    crd::containers::Array<double> x(&alloc);
    crd::containers::Array<double> y(&alloc);
    x.resize(8);
    y.resize(8);
    for (crd::usize j = 0; j < 8; ++j)
    {
        for (crd::usize i = 0; i < 8; ++i)
        {
            x[i] = (i == j) ? 1.0 : 0.0;
        }
        hss_matvec<double>(h, crd::containers::ConstSpan<double>{x.data(), 8},
                           crd::containers::Span<double>{y.data(), 8});
        for (crd::usize i = 0; i < 8; ++i)
        {
            CHECK_THAT(y[i], WithinAbs(expected.at(i, j), 1e-12));
        }
    }
}

TEST_CASE("hss: matvec on a general vector equals dense*x", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    HssMatrix<double> h(&alloc);
    build_hand_hss(h, &alloc);
    const Matrix<double> expected = hand_expected(&alloc);

    crd::containers::Array<double> x(&alloc);
    crd::containers::Array<double> y(&alloc);
    x.resize(8);
    y.resize(8);
    for (crd::usize i = 0; i < 8; ++i)
    {
        x[i] = std::sin(static_cast<double>(i) * 0.9 + 0.3) + 0.5;
    }
    hss_matvec<double>(h, crd::containers::ConstSpan<double>{x.data(), 8},
                       crd::containers::Span<double>{y.data(), 8});
    for (crd::usize i = 0; i < 8; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < 8; ++j)
        {
            s += expected.at(i, j) * x[j];
        }
        CHECK_THAT(y[i], WithinAbs(s, 1e-12));
    }
}

TEST_CASE("hss: f32 reconstruct matches the dense oracle", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(8U * 1024U * 1024U));
    // Reuse the same structure in f32 via a fresh build (double oracle compared loosely).
    HssMatrix<float> h(&alloc);
    build_cluster_tree<float>(h, 8, 2);
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        h.nodes[id].rank = 1;
    }
    auto fill_leaf = [&](crd::usize id, float d00, float d01, float d11, float u0, float u1, float rval) {
        h.nodes[id].d = Matrix<float>(&alloc, 2, 2);
        h.nodes[id].d.at(0, 0) = d00;
        h.nodes[id].d.at(0, 1) = d01;
        h.nodes[id].d.at(1, 0) = d01;
        h.nodes[id].d.at(1, 1) = d11;
        h.nodes[id].u = Matrix<float>(&alloc, 2, 1);
        h.nodes[id].u.at(0, 0) = u0;
        h.nodes[id].u.at(1, 0) = u1;
        h.nodes[id].r = Matrix<float>(&alloc, 1, 1);
        h.nodes[id].r.at(0, 0) = rval;
    };
    auto sc = [&](crd::usize id, char which, float v) {
        Matrix<float>& m = (which == 'b') ? h.nodes[id].b : h.nodes[id].r;
        m = Matrix<float>(&alloc, 1, 1);
        m.at(0, 0) = v;
    };
    fill_leaf(2, 4.0F, 1.0F, 4.0F, 1.0F, 0.0F, 2.0F);
    fill_leaf(3, 5.0F, 2.0F, 5.0F, 0.0F, 1.0F, 3.0F);
    fill_leaf(5, 6.0F, 1.0F, 6.0F, 1.0F, 0.0F, 1.5F);
    fill_leaf(6, 7.0F, 2.0F, 7.0F, 0.0F, 1.0F, 0.5F);
    sc(1, 'b', 0.7F);
    sc(1, 'r', 1.0F);
    sc(4, 'b', 0.9F);
    sc(4, 'r', 1.0F);
    sc(0, 'b', 0.3F);

    const Matrix<float> dense = hss_to_dense<float>(&alloc, h);
    const Matrix<double> expected = hand_expected(&alloc);
    for (crd::usize i = 0; i < 8; ++i)
    {
        for (crd::usize j = 0; j < 8; ++j)
        {
            CHECK_THAT(static_cast<double>(dense.at(i, j)), WithinAbs(expected.at(i, j), 1e-5));
        }
    }
}

TEST_CASE("hss: rank>1 matvec equals reconstruct*x (exercises mm / mm_bt / non-square B)",
          "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(16U * 1024U * 1024U));
    HssMatrix<double> h(&alloc);
    build_varied_hss(h, &alloc);

    // Non-vacuity: genuinely rank>1 bases + a NON-SQUARE coupling block.
    REQUIRE(h.nodes[1].rank == 2);
    REQUIRE(h.nodes[4].b.rows() == 2);
    REQUIRE(h.nodes[4].b.cols() == 1);

    const Matrix<double> dense = hss_to_dense<double>(&alloc, h);
    crd::containers::Array<double> x(&alloc);
    crd::containers::Array<double> y(&alloc);
    x.resize(8);
    y.resize(8);
    for (crd::usize trial = 0; trial < 4; ++trial)
    {
        for (crd::usize i = 0; i < 8; ++i)
        {
            x[i] = std::sin(static_cast<double>(i * 5 + trial * 13 + 1) * 0.4);
        }
        hss_matvec<double>(h, crd::containers::ConstSpan<double>{x.data(), 8},
                           crd::containers::Span<double>{y.data(), 8});
        for (crd::usize i = 0; i < 8; ++i)
        {
            double s = 0.0;
            for (crd::usize j = 0; j < 8; ++j)
            {
                s += dense.at(i, j) * x[j];
            }
            CHECK_THAT(y[i], WithinAbs(s, 1e-10));
        }
    }
}

namespace
{
template <typename T>
T frob(const Matrix<T>& m) noexcept
{
    T acc = T{0};
    for (crd::usize i = 0; i < m.size(); ++i)
    {
        acc += m.data()[i] * m.data()[i];
    }
    return std::sqrt(acc);
}

template <typename T>
T frob_diff(const Matrix<T>& a, const Matrix<T>& b) noexcept
{
    T acc = T{0};
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        const T d = a.data()[i] - b.data()[i];
        acc += d * d;
    }
    return std::sqrt(acc);
}

// Symmetric A = diag(2 + i/10) + Σ_{t=0}^{3} decay^t · w_t·w_tᵀ. The global part
// has EXACT rank 4 ⇒ every off-diagonal block has rank ≤ 4 (≪ leaf), with a
// geometrically-decaying spectrum (decay^t) so the compression tolerance
// genuinely selects the kept rank. Distinct frequencies keep the w_t
// independent (block-row rank == 4 generically).
template <typename T>
Matrix<T> kernel_matrix(crd::memory::IAllocator* alloc, crd::usize n)
{
    constexpr crd::usize terms = 4;
    const double decay = 0.05;
    const double freq[terms] = {0.21, 0.37, 0.53, 0.71};
    Matrix<T> a(alloc, n, n);
    a.set_zero();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i; j < n; ++j)
        {
            double g = 0.0;
            double wt = 1.0;
            for (crd::usize t = 0; t < terms; ++t)
            {
                const double wi = std::sin(static_cast<double>(i) * freq[t] + 0.3 * static_cast<double>(t) + 0.1);
                const double wj = std::sin(static_cast<double>(j) * freq[t] + 0.3 * static_cast<double>(t) + 0.1);
                g += wt * wi * wj;
                wt *= decay;
            }
            const T v = static_cast<T>(g);
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
        a.at(i, i) += static_cast<T>(2.0 + static_cast<double>(i) * 0.1);  // SPD-ish diagonal
    }
    return a;
}

template <typename T>
Matrix<T> sym_random(crd::memory::IAllocator* alloc, crd::usize n)
{
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = i; j < n; ++j)
        {
            const T v = static_cast<T>(std::sin(static_cast<double>(i * 13 + j * 7 + 1) * 0.37) +
                                       std::cos(static_cast<double>(i * 5 + j * 3 + 2) * 0.21));
            a.at(i, j) = v;
            a.at(j, i) = v;
        }
    }
    return a;
}

template <typename T>
crd::usize max_node_rank(const HssMatrix<T>& h) noexcept
{
    crd::usize mx = 0;
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        if (h.nodes[id].rank > mx)
        {
            mx = h.nodes[id].rank;
        }
    }
    return mx;
}
} // namespace

TEST_CASE("build_hss_from_dense: full-rank reconstructs the dense matrix exactly", "[hesap][hss][real]")
{
    // Generic symmetric A is NOT low-rank ⇒ tiny tol keeps full bases ⇒ the HSS
    // is lossless. Any B/R/orientation/nesting bug breaks exact reconstruction.
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize n = 32;
    const Matrix<double> a = sym_random<double>(&alloc, n);
    HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, /*leaf_size*/ 8, /*tol*/ 1e-12);

    const Matrix<double> dense = hss_to_dense<double>(&alloc, h);
    CHECK(frob_diff<double>(a, dense) < 1e-9 * frob<double>(a));

    // matvec agrees too.
    crd::containers::Array<double> x(&alloc);
    crd::containers::Array<double> y(&alloc);
    x.resize(n);
    y.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(static_cast<double>(i) * 0.7 + 0.2);
    }
    hss_matvec<double>(h, crd::containers::ConstSpan<double>{x.data(), n},
                       crd::containers::Span<double>{y.data(), n});
    for (crd::usize i = 0; i < n; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x[j];
        }
        CHECK_THAT(y[i], WithinAbs(s, 1e-9));
    }
}

TEST_CASE("build_hss_from_dense: compresses a low-rank kernel (rank gate)", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 64;
    constexpr crd::usize leaf = 8;  // 64 -> 32 -> 16 -> 8 : >= 3 levels
    const double tol = 1e-6;
    const Matrix<double> a = kernel_matrix<double>(&alloc, n);
    HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, leaf, tol);

    // HARD GATE: compression actually happened — genuine rank>1 bases AND every
    // node's rank is below its own block (span) size, not a vacuous full-rank
    // (or rank-0) reconstruction. (Internal spans are 16/32, so the bound is
    // per-node span, not leaf_size.)
    crd::usize max_leaf_rank = 0;
    bool all_compressed = true;
    for (crd::usize id = 0; id < h.num_nodes(); ++id)
    {
        const auto& nd = h.nodes[id];
        if (nd.parent >= 0 && nd.rank >= nd.size())  // non-root: block row of width n - span
        {
            all_compressed = false;
        }
        if (nd.is_leaf && nd.rank > max_leaf_rank)
        {
            max_leaf_rank = nd.rank;
        }
    }
    CHECK(max_leaf_rank >= 2);     // genuine rank>1 bases
    CHECK(max_leaf_rank < leaf);   // leaf blocks compressed below leaf_size
    CHECK(all_compressed);         // every node compressed below its span

    const Matrix<double> dense = hss_to_dense<double>(&alloc, h);
    const double anorm = frob<double>(a);
    CHECK(frob_diff<double>(a, dense) < 1e-4 * anorm);  // ~ tol * sqrt(#blocks)

    crd::containers::Array<double> x(&alloc);
    crd::containers::Array<double> y(&alloc);
    x.resize(n);
    y.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] = std::cos(static_cast<double>(i) * 0.5 + 0.1);
    }
    hss_matvec<double>(h, crd::containers::ConstSpan<double>{x.data(), n},
                       crd::containers::Span<double>{y.data(), n});
    double num = 0.0;
    double den = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        double s = 0.0;
        for (crd::usize j = 0; j < n; ++j)
        {
            s += a.at(i, j) * x[j];
        }
        num += (y[i] - s) * (y[i] - s);
        den += s * s;
    }
    CHECK(std::sqrt(num) < 1e-4 * std::sqrt(den));
}

TEST_CASE("build_hss_from_dense: looser tol gives lower rank, bounded error", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(128U * 1024U * 1024U));
    constexpr crd::usize n = 64;
    const Matrix<double> a = kernel_matrix<double>(&alloc, n);
    const double anorm = frob<double>(a);

    HssMatrix<double> tight = build_hss_from_dense<double>(&alloc, a, 8, 1e-9);
    HssMatrix<double> loose = build_hss_from_dense<double>(&alloc, a, 8, 1e-2);
    CHECK(max_node_rank<double>(loose) <= max_node_rank<double>(tight));

    const double e_tight = frob_diff<double>(a, hss_to_dense<double>(&alloc, tight));
    const double e_loose = frob_diff<double>(a, hss_to_dense<double>(&alloc, loose));
    CHECK(e_tight < 1e-6 * anorm);
    CHECK(e_loose >= e_tight);       // looser is no more accurate
    CHECK(e_loose < 0.5 * anorm);    // still a meaningful approximation
}

TEST_CASE("build_hss_from_dense: f32 kernel reconstruct", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(64U * 1024U * 1024U));
    constexpr crd::usize n = 64;
    const Matrix<float> a = kernel_matrix<float>(&alloc, n);
    HssMatrix<float> h = build_hss_from_dense<float>(&alloc, a, 8, 1e-3F);
    const Matrix<float> dense = hss_to_dense<float>(&alloc, h);
    CHECK(frob_diff<float>(a, dense) < 1e-2F * frob<float>(a));
}

namespace
{
// Gaussian kernel A_ij = exp(-((i-j)/L)^2): symmetric with a SMOOTHLY DECAYING
// off-diagonal singular-value spectrum (no exact cutoff) ⇒ the compression
// tolerance truncates mid-spectrum, leaving a genuine tail of dropped small σ.
template <typename T>
Matrix<T> gaussian_kernel(crd::memory::IAllocator* alloc, crd::usize n, double l)
{
    Matrix<T> a(alloc, n, n);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            const double d = (static_cast<double>(i) - static_cast<double>(j)) / l;
            a.at(i, j) = static_cast<T>(std::exp(-d * d));
        }
    }
    return a;
}
} // namespace

TEST_CASE("build_hss_from_dense: lossy compression error tracks the tolerance", "[hesap][hss][real]")
{
    // The accuracy contract: building at tolerance tau yields
    // ‖A - reconstruct‖_F <= C·tau·‖A‖_F (C absorbs the per-level projection
    // error of the independent-basis construction). A smoothly-decaying spectrum
    // (Gaussian kernel) with a large leaf so the rank truncates mid-spectrum.
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(256U * 1024U * 1024U));
    constexpr crd::usize n = 96;
    constexpr crd::usize leaf = 24;
    const Matrix<double> a = gaussian_kernel<double>(&alloc, n, 8.0);
    const double anorm = frob<double>(a);

    for (const double tau : {1e-4, 1e-8})
    {
        HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, leaf, tau);
        const double e = frob_diff<double>(a, hss_to_dense<double>(&alloc, h));
        CHECK(e <= 100.0 * tau * anorm);  // C ~ 100: validates accuracy + the construction design
    }

    // Genuine MID-SPECTRUM truncation at the tight tol: kept rank >= 2 and well
    // below the leaf block (else the "lossy" claim is vacuous).
    HssMatrix<double> h8 = build_hss_from_dense<double>(&alloc, a, leaf, 1e-8);
    crd::usize max_leaf_rank = 0;
    for (crd::usize id = 0; id < h8.num_nodes(); ++id)
    {
        if (h8.nodes[id].is_leaf && h8.nodes[id].rank > max_leaf_rank)
        {
            max_leaf_rank = h8.nodes[id].rank;
        }
    }
    CHECK(max_leaf_rank >= 2);
    CHECK(max_leaf_rank < leaf);
}

TEST_CASE("build_hss_from_dense: edge cases (single-leaf-root, uneven split)", "[hesap][hss][real]")
{
    crd::memory::TlsfAllocator alloc(static_cast<crd::usize>(32U * 1024U * 1024U));
    SECTION("single leaf == root (n <= leaf_size)")
    {
        const Matrix<double> a = sym_random<double>(&alloc, 5);
        HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, /*leaf_size*/ 8, 1e-12);
        REQUIRE(h.num_nodes() == 1);
        // Pure D block ⇒ reconstruct == A exactly.
        CHECK(frob_diff<double>(a, hss_to_dense<double>(&alloc, h)) < 1e-12 * (frob<double>(a) + 1.0));
    }
    SECTION("uneven leaf split (n not a power-of-two multiple of leaf)")
    {
        const Matrix<double> a = sym_random<double>(&alloc, 30);
        HssMatrix<double> h = build_hss_from_dense<double>(&alloc, a, /*leaf_size*/ 8, 1e-12);
        // Generic symmetric, tiny tol ⇒ full-rank ⇒ exact reconstruction even
        // with uneven leaf spans.
        CHECK(frob_diff<double>(a, hss_to_dense<double>(&alloc, h)) < 1e-9 * frob<double>(a));
    }
}
