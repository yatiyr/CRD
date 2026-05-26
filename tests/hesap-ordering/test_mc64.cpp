#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/complex.hpp>
#include <crd/hesap/ordering/mc64.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::hesap::sparse;
using crd::hesap::Complex;
using crd::hesap::ordering::mc64_match_and_scale;
using crd::hesap::ordering::Mc64Scaling;

namespace
{
inline double cmag(crd::f64 v) { return v < 0.0 ? -v : v; }
template <typename R>
inline double cmag(const Complex<R>& v) { return std::sqrt(double(v.re) * v.re + double(v.im) * v.im); }

// I-matrix property: |D_r[i]·a[i,j]·D_c[j]| ≤ 1 (+eps) everywhere, == 1 on the matched entry.
template <typename T>
void check_i_matrix(const SparseMatrix<T, SparseFormat::Csr>& a, const Mc64Scaling& s)
{
    const crd::u32 n = a.rows();
    const auto*    outer = a.pattern().outer_ptr.data();
    const auto*    inner = a.pattern().inner_idx.data();
    const T*       vals  = a.values().values.data();
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const crd::u32 j      = inner[k];
            const double   scaled = s.dr[i] * cmag(vals[k]) * s.dc[j];
            CHECK(scaled <= 1.0 + 1e-7);
        }
        // matched entry scales to ~1
        const T mv = a.coeff(i, s.colperm[i]);
        if (cmag(mv) > 0.0)
        {
            const double sm = s.dr[i] * cmag(mv) * s.dc[s.colperm[i]];
            CHECK(std::abs(sm - 1.0) < 1e-6);
        }
    }
}

template <typename T>
bool is_permutation(const Mc64Scaling& s, crd::u32 n)
{
    crd::containers::Array<crd::u8> seen(s.colperm.allocator());
    seen.resize(n);
    for (crd::u32 j = 0; j < n; ++j) { seen[j] = 0; }
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (s.colperm[i] >= n || seen[s.colperm[i]]) { return false; }
        seen[s.colperm[i]] = 1;
    }
    return true;
}
} // namespace

TEST_CASE("MC64 matches the large off-diagonal entries to the diagonal (cyclic)", "[hesap-ordering][mc64]")
{
    crd::memory::TlsfAllocator alloc{8U << 20};
    const crd::u32             n = 4;
    TripletBuilder<crd::f64>   b(&alloc, n, n);
    // Each row's LARGEST entry is one column to the right (cyclic) ⇒ optimal matching = the cycle.
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, 0.1);
        b.add(i, (i + 1) % n, 5.0);
    }
    auto a = b.compress();
    auto s = mc64_match_and_scale<crd::f64>(a, &alloc);
    REQUIRE(s.full_rank);
    REQUIRE(is_permutation<crd::f64>(s, n));
    for (crd::u32 i = 0; i < n; ++i) { REQUIRE(s.colperm[i] == (i + 1) % n); } // matched the big entries
    check_i_matrix<crd::f64>(a, s);
}

TEST_CASE("MC64 produces the I-matrix property on a badly-scaled general matrix", "[hesap-ordering][mc64]")
{
    crd::memory::TlsfAllocator alloc{32U << 20};
    const crd::u32             n = 60;
    TripletBuilder<crd::f64>   b(&alloc, n, n);
    // Wildly varying row scales + off-diagonal dominance ⇒ exercises the matching + scaling.
    for (crd::u32 i = 0; i < n; ++i)
    {
        const double s = std::pow(10.0, static_cast<double>(i % 7) - 3.0); // 1e-3 .. 1e3 row scale
        b.add(i, i, s * 0.5);
        if (i + 1 < n) { b.add(i, i + 1, s * 3.0); }
        if (i > 0) { b.add(i, i - 1, s * 2.0); }
        if (i + 3 < n) { b.add(i, i + 3, s * 0.7); }
    }
    auto a = b.compress();
    auto s = mc64_match_and_scale<crd::f64>(a, &alloc);
    REQUIRE(is_permutation<crd::f64>(s, n)); // structurally full rank here
    check_i_matrix<crd::f64>(a, s);
}

TEST_CASE("MC64 is deterministic (bit-identical across runs)", "[hesap-ordering][mc64][determinism]")
{
    crd::memory::TlsfAllocator alloc{32U << 20};
    const crd::u32             n = 50;
    TripletBuilder<crd::f64>   b(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, 1.0 + 0.5 * static_cast<double>(i % 3));
        if (i + 2 < n) { b.add(i, i + 2, 4.0 - 0.1 * static_cast<double>(i % 5)); }
        if (i > 1) { b.add(i, i - 2, 2.5); }
    }
    auto a  = b.compress();
    auto s1 = mc64_match_and_scale<crd::f64>(a, &alloc);
    auto s2 = mc64_match_and_scale<crd::f64>(a, &alloc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        REQUIRE(s1.colperm[i] == s2.colperm[i]);
        REQUIRE(s1.dr[i] == s2.dr[i]);
        REQUIRE(s1.dc[i] == s2.dc[i]);
    }
}

TEST_CASE("MC64 handles a complex matrix (matches by magnitude)", "[hesap-ordering][mc64][complex]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    using C        = Complex<crd::f64>;
    const crd::u32 n = 5;
    TripletBuilder<C> b(&alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b.add(i, i, C{0.2, 0.1});                  // small diagonal
        b.add(i, (i + 2) % n, C{3.0, 4.0});        // |·| = 5, the largest ⇒ matched
    }
    auto a = b.compress();
    auto s = mc64_match_and_scale<C>(a, &alloc);
    REQUIRE(is_permutation<C>(s, n));
    for (crd::u32 i = 0; i < n; ++i) { REQUIRE(s.colperm[i] == (i + 2) % n); }
    check_i_matrix<C>(a, s);
}
