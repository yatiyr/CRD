// crd-hesap-direct: the sparse LU factors' crd-jobs levers as forced arms (SIMD-DISPATCH, the folded
// HESAP-JOBS-RETUNE; detail::SupernodalLuPick, MultifrontalLuPick and DenseLuPick in direct_picks.hpp).
//
// A scheduling arm (the job count, the walk's and the fronts' cuts, the lookahead, the fronts' forced jobs) must give
// the one-job factor's bits: L, U, their fill and, with partial pivoting, the row permutation. A blocking arm (the
// inline foot update against gemm's, the diagonal block's and the fronts' panel widths) regroups sums: it must give
// its own one-job bits on 8 and 32 jobs and keep the raw factor's normwise backward error within 4 n eps. The pool
// has four workers on every host (a hosted runner's), so the 8 to 32 forced jobs outnumber it.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/detail/direct_picks.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace
{
namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;
using dir::detail::DenseLuPick;
using dir::detail::MultifrontalLuPick;
using dir::detail::SupernodalLuPick;
template <typename T> using Mat = sp::SparseMatrix<T, sp::SparseFormat::Csr>;
template <typename T> using Real = crd::hesap::dense::RealType<T>;
constexpr crd::u32 kAll = 0xFFFFFFFFU;    // a front_par_min no level reaches: every level on the main thread
constexpr crd::u64 kNever = ~crd::u64{0}; // a cut nothing reaches
constexpr crd::u32 kScheduleJobs[] = {4U, 8U, 16U, 32U};

enum class Special : crd::u8
{
    None,
    SignedZeroSubnormal, // -0 and the type's smallest subnormal in place of some off-diagonals
    NaN,                 // one off-diagonal NaN: every arm propagates it to the same entries
    WeakDiagonal,        // the diagonal scaled by 1e-6: the natural-diagonal attempt's growth falls back to MC64
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

template <typename T> crd::f64 magnitude(T v)
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return std::hypot(static_cast<crd::f64>(v.re), static_cast<crd::f64>(v.im));
    }
    else
    {
        return std::abs(static_cast<crd::f64>(v));
    }
}

crd::f64 hashed(crd::u32 i, crd::u32 j) // a deterministic value in [-1, 1)
{
    crd::u32 h = (i * 2654435761U) ^ (j * 2246822519U) ^ 0x9E3779B9U;
    h ^= h >> 15;
    h *= 2246822519U;
    h ^= h >> 13;
    return static_cast<crd::f64>(h % 2000U) / 1000.0 - 1.0;
}

// The unsymmetric pattern's off-diagonals of either fixture, (i, j, v) through add.
//   bordered: `blocks` dense blocks of `width` coupled to a dense border of `border` rows and columns, so the blocks
//   are leaf fronts and the border is one wide root front that every block extend-adds into;
//   grid: a g x g convection-diffusion stencil, a deep tree of narrow fronts.
template <typename Add> void off_diagonals(bool bordered, crd::u32 a, crd::u32 b, crd::u32 c, Add add)
{
    if (bordered)
    {
        const crd::u32 blocks = a;
        const crd::u32 width = b;
        const crd::u32 border = c;
        const crd::u32 nb = blocks * width;
        for (crd::u32 k = 0; k < blocks; ++k)
        {
            for (crd::u32 i = k * width; i < (k + 1) * width; ++i)
            {
                for (crd::u32 j = k * width; j < (k + 1) * width; ++j)
                {
                    if (i != j && (i + 2 * j) % 3 != 0) // unsymmetric: (i, j) without (j, i) in places
                    {
                        add(i, j, hashed(i, j));
                    }
                }
                for (crd::u32 r = nb; r < nb + border; ++r)
                {
                    if ((i + r) % 4 != 0)
                    {
                        add(i, r, 0.5 * hashed(i, r));
                        add(r, i, 0.25 * hashed(r, i));
                    }
                }
            }
        }
        for (crd::u32 i = nb; i < nb + border; ++i)
        {
            for (crd::u32 j = nb; j < nb + border; ++j)
            {
                if (i != j)
                {
                    add(i, j, hashed(i, j));
                }
            }
        }
        return;
    }
    const crd::u32 g = a;
    for (crd::u32 y = 0; y < g; ++y)
    {
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            if (x + 1 < g)
            {
                add(i, i + 1, -1.2);
            }
            if (x > 0)
            {
                add(i, i - 1, -0.8);
            }
            if (y + 1 < g)
            {
                add(i, i + g, -1.1);
            }
            if (y > 0)
            {
                add(i, i - g, -0.9);
            }
        }
    }
}

// The fixture in T, diagonally dominant (each diagonal 1.5 times its row's off-diagonal sum plus 1); a complex
// off-diagonal v is v (1 + i/2) above the diagonal and v (1 - i/3) below, so the matrix is not Hermitian.
template <typename T>
Mat<T> fixture(crd::memory::IAllocator* alloc, bool bordered, crd::u32 a, crd::u32 b, crd::u32 c, Special special)
{
    const crd::u32 n = bordered ? a * b + c : a * a;
    crd::containers::Array<crd::f64> row_sum(alloc);
    row_sum.resize(n);
    off_diagonals(bordered, a, b, c, [&](crd::u32 i, crd::u32, crd::f64 v) { row_sum[i] += std::abs(v); });
    const crd::f64 tiny = static_cast<crd::f64>(std::numeric_limits<Real<T>>::denorm_min());
    const crd::u32 nan_row = n / 2;
    bool nan_placed = false;
    sp::TripletBuilder<T> tb(alloc, n, n);
    off_diagonals(bordered, a, b, c,
                  [&](crd::u32 i, crd::u32 j, crd::f64 v)
                  {
                      T x = scalar<T>(v, i < j ? 0.5 * v : -v / 3.0);
                      if (special == Special::SignedZeroSubnormal && (i + j) % 5 == 0)
                      {
                          x = scalar<T>((i + j) % 10 == 0 ? -0.0 : tiny, 0.0);
                      }
                      if (special == Special::NaN && i == nan_row && !nan_placed)
                      {
                          x = scalar<T>(std::numeric_limits<crd::f64>::quiet_NaN(), 0.0);
                          nan_placed = true;
                      }
                      tb.add(i, j, x);
                  });
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::f64 d = 1.5 * row_sum[i] + 1.0;
        tb.add(i, i, scalar<T>(special == Special::WeakDiagonal ? 1e-6 * d : d, 0.0));
    }
    if (special == Special::NaN)
    {
        REQUIRE(nan_placed);
    }
    return tb.compress();
}

template <typename T> void apply(const Mat<T>& a, const T* x, T* y)
{
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        T acc{};
        for (crd::u32 q = pat.outer_ptr[i]; q < pat.outer_ptr[i + 1]; ++q)
        {
            acc = acc + vals[q] * x[pat.inner_idx[q]];
        }
        y[i] = acc;
    }
}

// The raw factor (apply_inverse: no refinement hides a worse factor) solves A x = b for a known x within the
// normwise backward error |b - A x| <= 4 n eps (|A| |x| + |b|).
template <typename F, typename T> bool within_bound(const F& f, const Mat<T>& a, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = a.rows();
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> ax(alloc);
    xtrue.resize(n);
    b.resize(n);
    x.resize(n);
    ax.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = scalar<T>(1.0 + 0.01 * static_cast<crd::f64>(i % 37), 0.02 * static_cast<crd::f64>(i % 11));
    }
    apply(a, xtrue.data(), b.data());
    for (crd::u32 i = 0; i < n; ++i)
    {
        x[i] = b[i];
    }
    f.apply_inverse({x.data(), n}, 1);
    apply(a, x.data(), ax.data());
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
    const crd::f64 bound = 4.0 * static_cast<crd::f64>(n) * eps * (an * xn + bn);
    INFO("backward error " << r << " against the bound " << bound);
    return r <= bound;
}

template <typename T> bool same_span(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
{
    return x.size() == y.size() && (x.size() == 0 || std::memcmp(x.data(), y.data(), x.size() * sizeof(T)) == 0);
}

// info(), the fill, L and U bit for bit, and (multifrontal) the row permutation.
template <typename F> bool same_factor(const F& x, const F& y)
{
    bool same = x.info() == y.info() && x.l_nnz() == y.l_nnz() && x.u_nnz() == y.u_nnz() &&
                same_span(x.l_values(), y.l_values()) && same_span(x.u_values(), y.u_values());
    if constexpr (requires { x.rowperm(); })
    {
        same = same && same_span(x.rowperm(), y.rowperm());
    }
    return same;
}

template <typename T>
dir::SupernodalLU<T> supernodal(const SupernodalLuPick& pick, const Mat<T>& a, crd::u32 jobs,
                                crd::memory::IAllocator* alloc)
{
    dir::SupernodalLU<T> f(alloc);
    f.factorize_arm(pick, a, jobs);
    return f;
}

template <typename T>
dir::MultifrontalLU<T> multifrontal(const MultifrontalLuPick& pick, const Mat<T>& a, crd::u32 jobs, bool pivot,
                                    crd::memory::IAllocator* alloc)
{
    dir::MultifrontalLU<T> f(alloc);
    f.factorize_arm(pick, a, jobs, pivot ? Real<T>(1) : Real<T>(0));
    return f;
}

template <typename T> void check_supernodal(const Mat<T>& a, bool bounded, crd::memory::IAllocator* alloc)
{
    const auto ref = supernodal<T>(SupernodalLuPick{}, a, 1, alloc);
    REQUIRE(ref.supernode_count() >= 2);
    if (bounded)
    {
        CHECK(within_bound(ref, a, alloc));
    }
    for (const crd::u32 jobs : kScheduleJobs)
    {
        INFO("default pick on " << jobs << " jobs");
        CHECK(same_factor(supernodal<T>(SupernodalLuPick{}, a, jobs, alloc), ref));
    }
    const SupernodalLuPick blocking[] = {
        {.cmod_gemm_min_flop = 0},      // every foot update through gemm
        {.cmod_gemm_min_flop = kNever}, // every foot update inline
        {.diag_block = 1},
        {.diag_block = 8},
        {.diag_block = 256}, // one panel: the unblocked diagonal LU
    };
    for (crd::usize i = 0; i < std::size(blocking); ++i)
    {
        INFO("blocking arm " << i);
        const auto one = supernodal<T>(blocking[i], a, 1, alloc);
        for (const crd::u32 jobs : {8U, 32U})
        {
            INFO("on " << jobs << " jobs");
            CHECK(same_factor(supernodal<T>(blocking[i], a, jobs, alloc), one));
        }
        if (bounded)
        {
            CHECK(within_bound(one, a, alloc));
        }
    }
}

// Every within-front stage on the jobs from its first column (DenseLuPick's cuts at 0) and every level on the main
// thread, so each front runs factor_front's parallel path; `front` keeps its blocking levers.
MultifrontalLuPick all_within(DenseLuPick front)
{
    front.trsm_min = 0;
    front.schur_min_mnk = 0;
    return {.small_fill = 0, .front_par_min = kAll, .zero_fill_min = 0, .extend_add_min = 0, .front = front};
}

template <typename T> void check_multifrontal(const Mat<T>& a, bool pivot, bool bounded, crd::memory::IAllocator* alloc)
{
    const auto ref = multifrontal<T>(MultifrontalLuPick{}, a, 1, pivot, alloc);
    REQUIRE(ref.front_count() >= 2);
    if (bounded)
    {
        CHECK(within_bound(ref, a, alloc));
    }
    MultifrontalLuPick no_lookahead = all_within(DenseLuPick{});
    no_lookahead.front.lookahead = false;
    MultifrontalLuPick forced_front = all_within(DenseLuPick{});
    forced_front.front.workers = 32;
    const MultifrontalLuPick scheduling[] = {
        {},                                    // the small-fill cut: the serial walk
        {.small_fill = 0},                     // the level walk at the default cuts
        {.small_fill = 0, .front_par_min = 1}, // every level front-parallel
        all_within(DenseLuPick{}),
        no_lookahead,
        forced_front,
    };
    for (crd::usize i = 0; i < std::size(scheduling); ++i)
    {
        for (const crd::u32 jobs : kScheduleJobs)
        {
            INFO("scheduling arm " << i << " on " << jobs << " jobs");
            CHECK(same_factor(multifrontal<T>(scheduling[i], a, jobs, pivot, alloc), ref));
        }
    }
    for (const crd::u32 panel : {1U, 8U, 16U, 256U})
    {
        INFO("panel " << panel);
        const DenseLuPick front{.panel = panel};
        const auto one = multifrontal<T>(MultifrontalLuPick{.front = front}, a, 1, pivot, alloc);
        for (const bool lookahead : {true, false})
        {
            MultifrontalLuPick pick = all_within(front);
            pick.front.lookahead = lookahead;
            for (const crd::u32 jobs : {8U, 32U})
            {
                INFO("lookahead " << lookahead << " on " << jobs << " jobs");
                CHECK(same_factor(multifrontal<T>(pick, a, jobs, pivot, alloc), one));
            }
        }
        if (bounded)
        {
            CHECK(within_bound(one, a, alloc));
        }
    }
}

template <typename T> void check_type(crd::memory::IAllocator* alloc)
{
    for (const Special special : {Special::None, Special::SignedZeroSubnormal, Special::NaN, Special::WeakDiagonal})
    {
        INFO("special " << static_cast<int>(special));
        const bool bounded = special == Special::None || special == Special::SignedZeroSubnormal;
        const Mat<T> bordered = fixture<T>(alloc, true, 4, 32, 48, special);
        check_supernodal<T>(bordered, bounded, alloc);
        for (const bool pivot : {false, true})
        {
            INFO("partial pivoting " << pivot);
            check_multifrontal<T>(bordered, pivot, bounded, alloc);
        }
    }
    INFO("grid");
    const Mat<T> grid = fixture<T>(alloc, false, 16, 0, 0, Special::None);
    check_supernodal<T>(grid, true, alloc);
    check_multifrontal<T>(grid, false, true, alloc);
}

template <typename F> void on_four_workers(F run)
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4; // a hosted runner's pool on every host: the 8 to 32 forced jobs outnumber its workers
    crd::jobs::init(cfg);
    run();
    crd::jobs::shutdown();
}
} // namespace

TEST_CASE("lu_arms: every SupernodalLU and MultifrontalLU lever keeps one job's bits or the backward-error bound",
          "[hesap][direct][jobs][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    on_four_workers(
        [&]
        {
            {
                INFO("f64");
                check_type<crd::f64>(&alloc);
            }
            {
                INFO("f32");
                check_type<crd::f32>(&alloc);
            }
            {
                INFO("c64");
                check_type<crd::hesap::Complex64>(&alloc);
            }
            {
                INFO("c32");
                check_type<crd::hesap::Complex32>(&alloc);
            }
        });
}
