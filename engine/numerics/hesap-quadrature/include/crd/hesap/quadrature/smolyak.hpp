#pragma once

// crd-hesap-quadrature v13-k - SMOLYAK SPARSE GRID cubature over [a,b]^d (combination technique with nested
// Clenshaw-Curtis 1-D rules). Breaks the curse of dimensionality for SMOOTH integrands: a level-q sparse grid uses
// O(2^q q^{d-1}) points instead of the tensor grid's O((2^q)^d). Build the deduplicated grid ONCE (the nested CC
// structure gives each node a canonical integer key ⇒ exact dedup, the sparse-grid efficiency), then integrate many.
//
// Reconstructed-and-verified in python (polynomial exactness exact to machine precision + tensor cross-check) before
// this port. Peer: Tasmanian (not installed here ⇒ gate is the analytic exactness degree + tensor-Gauss cross-check,
// stated). Moat: determinism (crd::math, fixed FP order, deterministic node enumeration) + the grid IS the bounded
// work set (no recursion) + the error-tier QuadResult (Tier-0 for a fixed level; raise q to bound).

#include <crd/hesap/quadrature/integrate.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::quadrature
{

constexpr int kSmolyakMaxDim = 6;  // d ≤ 6, q ≤ 10 ⇒ the canonical key packs d×10 bits into a u64
constexpr int kSmolyakMaxLvl = 10;

// A precomputed Smolyak sparse grid on [-1,1]^d: deduplicated nodes (npts × d) + combined weights (npts). Build once,
// integrate many (allocation-free apply).
template <typename T>
struct SmolyakGrid
{
    crd::containers::Array<T> nodes;
    crd::containers::Array<T> weights;
    int                       d    = 0;
    int                       npts = 0;
    bool                      valid = false;

    explicit SmolyakGrid(crd::memory::IAllocator* alloc) : nodes(alloc), weights(alloc) {}
};

namespace detail
{
// Clenshaw-Curtis nodes+weights for level `lvl` on [-1,1] (level 1 → 1 node {0} w=2; level i → 2^{i-1}+1 nodes).
template <typename T>
void cc_level(int lvl, T* x, T* w, int& n)
{
    if (lvl == 1)
    {
        n    = 1;
        x[0] = T{0};
        w[0] = T{2};
        return;
    }
    n            = (1 << (lvl - 1)) + 1;
    const T  pi  = static_cast<T>(3.14159265358979323846264338327950288);
    const int nm = n - 1;
    for (int j = 0; j < n; ++j)
    {
        const T theta = pi * static_cast<T>(j) / static_cast<T>(nm);
        x[j]          = crd::math::cos(theta);
        T s           = T{0};
        for (int m = 1; m <= nm / 2; ++m)
        {
            const T b = (2 * m != nm) ? T{2} : T{1};
            s += b / static_cast<T>(4 * m * m - 1) * crd::math::cos(static_cast<T>(2 * m) * theta);
        }
        const T cj = (j != 0 && j != nm) ? T{2} : T{1};
        w[j]       = cj / static_cast<T>(nm) * (T{1} - s);
    }
}

[[nodiscard]] inline crd::i64 binom(int n, int k) noexcept
{
    if (k < 0 || k > n)
    {
        return 0;
    }
    crd::i64 r = 1;
    for (int i = 0; i < k; ++i)
    {
        r = r * (n - i) / (i + 1);
    }
    return r;
}
} // namespace detail

// Build the Smolyak grid for dimension d (2..6) and level q (≥ d; higher = more accurate). The level-q nested grid has
// a finest 1-D resolution of 2^{q-1}+1 points per axis.
template <typename T>
[[nodiscard]] SmolyakGrid<T> build_smolyak_grid(crd::memory::IAllocator* alloc, int d, int q)
{
    SmolyakGrid<T> grid(alloc);
    if (d < 2 || d > kSmolyakMaxDim || q < d || q > kSmolyakMaxLvl)
    {
        return grid;
    }
    grid.d              = d;
    const int    nfine  = (1 << (q - 1)) + 1; // finest 1-D node count; fine index in [0, nfine-1]
    const T      pi     = static_cast<T>(3.14159265358979323846264338327950288);
    // dedup: canonical key = Σ fine_index_m · nfine^m (packed); value = node slot.
    crd::containers::HashMap<crd::u64, crd::u32> slot(alloc);
    int idx[kSmolyakMaxDim]   = {};
    int finei[kSmolyakMaxDim] = {};
    // enumerate multi-indices i_k in [1,q]^d with q-d+1 <= |i| <= q.
    crd::i64 totalmi = 1;
    for (int i = 0; i < d; ++i)
    {
        totalmi *= q;
    }
    int mi[kSmolyakMaxDim];
    for (crd::i64 c = 0; c < totalmi; ++c)
    {
        // decode c into mi[0..d-1] in [1,q]
        crd::i64 t   = c;
        int      sum = 0;
        for (int i = 0; i < d; ++i)
        {
            mi[i] = static_cast<int>(t % q) + 1;
            t /= q;
            sum += mi[i];
        }
        if (sum < q - d + 1 || sum > q)
        {
            continue;
        }
        const crd::i64 coeff = ((q - sum) % 2 == 0 ? 1 : -1) * detail::binom(d - 1, q - sum);
        // tensor-product the d 1-D CC rules for this multi-index.
        // build per-dim CC nodes/weights once; iterate the combo via a mixed-radix counter.
        // To avoid recomputing per combo, cache each dim's (x,w,n) — but kSmolyak small, recompute cheaply.
        T   dimx[kSmolyakMaxDim][(1 << (kSmolyakMaxLvl - 1)) + 1];
        T   dimw[kSmolyakMaxDim][(1 << (kSmolyakMaxLvl - 1)) + 1];
        int dimn[kSmolyakMaxDim];
        for (int i = 0; i < d; ++i)
        {
            detail::cc_level<T>(mi[i], dimx[i], dimw[i], dimn[i]);
        }
        for (int i = 0; i < d; ++i)
        {
            idx[i] = 0;
        }
        crd::i64 combos = 1;
        for (int i = 0; i < d; ++i)
        {
            combos *= dimn[i];
        }
        for (crd::i64 cc2 = 0; cc2 < combos; ++cc2)
        {
            crd::u64 key = 0;
            T        wprod = static_cast<T>(coeff);
            for (int i = 0; i < d; ++i)
            {
                const int k    = idx[i];
                const int fi   = k * (1 << (q - mi[i])); // map to the finest grid
                finei[i]       = fi;
                key            = key * static_cast<crd::u64>(nfine) + static_cast<crd::u64>(fi);
                wprod *= dimw[i][k];
            }
            crd::u32* found = slot.find(key);
            if (found != nullptr)
            {
                grid.weights[static_cast<crd::usize>(*found)] += wprod;
            }
            else
            {
                const crd::u32 ns = static_cast<crd::u32>(grid.npts);
                slot.insert(key, ns);
                for (int i = 0; i < d; ++i)
                {
                    grid.nodes.push_back(crd::math::cos(pi * static_cast<T>(finei[i]) / static_cast<T>(nfine - 1)));
                }
                grid.weights.push_back(wprod);
                ++grid.npts;
            }
            for (int i = 0; i < d; ++i) // mixed-radix increment of the combo
            {
                if (++idx[i] < dimn[i])
                {
                    break;
                }
                idx[i] = 0;
            }
        }
    }
    grid.valid = true;
    return grid;
}

// Integrate f over [a,b]^d with the precomputed Smolyak grid (mapped from [-1,1]^d). f: callable (const T*)->T.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_smolyak(const SmolyakGrid<T>& grid, F&& f, crd::containers::ConstSpan<T> a,
                                              crd::containers::ConstSpan<T> b)
{
    const int d = grid.d;
    if (!grid.valid || static_cast<int>(a.size()) != d || static_cast<int>(b.size()) != d)
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    T mid[kSmolyakMaxDim];
    T half[kSmolyakMaxDim];
    T vol = T{1};
    for (int i = 0; i < d; ++i)
    {
        mid[i]  = (a[static_cast<crd::usize>(i)] + b[static_cast<crd::usize>(i)]) / T{2};
        half[i] = (b[static_cast<crd::usize>(i)] - a[static_cast<crd::usize>(i)]) / T{2};
        vol *= half[i]; // each 1-D CC weight already integrates over [-1,1] (length 2); times half = (b-a)/2
    }
    T pt[kSmolyakMaxDim];
    T acc = T{0};
    for (int p = 0; p < grid.npts; ++p)
    {
        for (int i = 0; i < d; ++i)
        {
            pt[i] = mid[i] + half[i] * grid.nodes[static_cast<crd::usize>(p * d + i)];
        }
        acc += grid.weights[static_cast<crd::usize>(p)] * f(pt);
    }
    QuadResult<T> r;
    r.value      = acc * vol;
    r.eval_count = static_cast<crd::u32>(grid.npts);
    return r;
}

} // namespace crd::hesap::quadrature
