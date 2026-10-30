// crd-hesap-direct: the level walks' crd-jobs levers of MultifrontalLDLT, MultifrontalQR and MultifrontalCholeskyBlr
// as forced arms (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE; detail::MultifrontalLdltPick, MultifrontalQrPick and
// MultifrontalBlrPick in direct_picks.hpp).
//
// A scheduling arm (the job count, the level-width cut, a main-thread front's gemm cut and jobs, LDLT's flush tile,
// BLR's dense-front cut) must give the one-job factor's bits. A blocking arm (LDLT's blocked-kernel cut and panel,
// QR's compact-WY panel and cut) regroups sums: it must give its own one-job bits under the scheduling extremes on 8
// and 32 jobs and keep the raw factor's normwise backward error within 4 n eps. The pool has four workers on every
// host (a hosted runner's), so the 8 to 32 forced jobs outnumber it.

#include "stencil_3d.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/detail/direct_picks.hpp>
#include <crd/hesap/direct/multifrontal_cholesky_blr.hpp>
#include <crd/hesap/direct/multifrontal_ldlt.hpp>
#include <crd/hesap/direct/multifrontal_qr.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace
{
namespace dir = crd::hesap::direct;
namespace sp = crd::hesap::sparse;
using dir::detail::FrontWalkPick;
using dir::detail::MultifrontalBlrPick;
using dir::detail::MultifrontalLdltPick;
using dir::detail::MultifrontalQrPick;
template <typename T> using Csc = sp::SparseMatrix<T, sp::SparseFormat::Csc>;
template <typename T> using Real = crd::hesap::dense::RealType<T>;
constexpr crd::u32 kAll = 0xFFFFFFFFU;    // a level_min no level reaches: every level on the main thread
constexpr crd::u64 kNever = ~crd::u64{0}; // a gemm cut nothing reaches
constexpr crd::u32 kScheduleJobs[] = {4U, 8U, 16U, 32U};
constexpr double kTextbookAlpha = 0.6403882032022075; // Bunch-Kaufman's: delays on the indefinite fixture
// Every front on the main thread with every gemm on the jobs (the pool's, then 32 forced).
constexpr FrontWalkPick kWithinPool{.level_min = kAll, .front = {.gemm_min_mnk = 0, .workers = 0}};
constexpr FrontWalkPick kWithin32{.level_min = kAll, .front = {.gemm_min_mnk = 0, .workers = 32}};

enum class Special : crd::u8
{
    None,
    SignedZeroSubnormal, // -0 and the type's smallest subnormal in place of some couplings
    NaN,                 // one coupling NaN: every arm propagates it to the same entries
    Breakdown,           // LDLT, BLR: one negative pivot on the diagonal (BLR: non-SPD); QR: one zero column
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

template <typename T> T conj_of(T v)
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return T{v.re, -v.im};
    }
    else
    {
        return v;
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

// A coupling's value -(1 + h / 4) (complex: times 1 + 0.3 i), or the special's.
template <typename T> T coupling(crd::u32 i, crd::u32 j, Special special, bool& nan_placed, crd::u32 nan_row)
{
    const crd::f64 v = -(1.0 + 0.25 * hashed(i, j));
    T x = scalar<T>(v, 0.3 * v);
    if (special == Special::SignedZeroSubnormal && (i + j) % 5 == 0)
    {
        const crd::f64 tiny = static_cast<crd::f64>(std::numeric_limits<Real<T>>::denorm_min());
        x = scalar<T>((i + j) % 10 == 0 ? -0.0 : tiny, 0.0);
    }
    if (special == Special::NaN && i == nan_row && !nan_placed)
    {
        x = scalar<T>(std::numeric_limits<crd::f64>::quiet_NaN(), 0.0);
        nan_placed = true;
    }
    return x;
}

// The symmetric (Hermitian when asked) 3D stencil on k^3 nodes with `diag` on the diagonal; Breakdown makes the
// middle node's diagonal -diag.
template <typename T>
Csc<T> symmetric_fixture(crd::memory::IAllocator* alloc, crd::u32 k, crd::f64 diag, Special special, bool hermitian)
{
    const crd::u32 n = k * k * k;
    bool nan_placed = false;
    Csc<T> a = dir::test::stencil_3d<T>(
        k, scalar<T>(diag, 0.0), [&](crd::u32 i, crd::u32 j) { return coupling<T>(i, j, special, nan_placed, n / 2); },
        [hermitian](T v) { return hermitian ? conj_of(v) : v; }, alloc);
    if (special == Special::NaN)
    {
        REQUIRE(nan_placed);
    }
    if (special == Special::Breakdown)
    {
        const auto& pat = a.pattern();
        auto& vals = a.values().values;
        const crd::u32 c = n / 2;
        for (crd::u32 q = pat.outer_ptr[c]; q < pat.outer_ptr[c + 1]; ++q)
        {
            if (pat.inner_idx[q] == c)
            {
                vals[q] = scalar<T>(-diag, 0.0);
            }
        }
    }
    return a;
}

// An over-determined 2 g^2 x g^2 least-squares matrix: rows [0, n) a 2D x-coupled stencil, rows [n, 2n) its
// y-coupling plus a unit diagonal. Breakdown zeroes column n / 3's values (a dead pivot).
template <typename T> Csc<T> least_squares_fixture(crd::memory::IAllocator* alloc, crd::u32 g, Special special)
{
    const crd::u32 n = g * g;
    bool nan_placed = false;
    sp::TripletBuilder<T> tb(alloc, 2 * n, n);
    const crd::u32 dead = n / 3;
    const auto add = [&](crd::u32 r, crd::u32 c, T v)
    {
        tb.add(r, c, special == Special::Breakdown && c == dead ? T{} : v);
    };
    for (crd::u32 y = 0; y < g; ++y)
    {
        for (crd::u32 x = 0; x < g; ++x)
        {
            const crd::u32 i = y * g + x;
            add(i, i, scalar<T>(4.0 + 0.5 * hashed(i, i), 0.2));
            if (x > 0)
            {
                add(i, i - 1, coupling<T>(i, i - 1, special, nan_placed, n / 2));
            }
            if (x + 1 < g)
            {
                add(i, i + 1, coupling<T>(i + 1, i, special, nan_placed, n / 2));
            }
            add(n + i, i, scalar<T>(1.0, -0.1));
            if (y > 0)
            {
                add(n + i, i - g, coupling<T>(n + i, i - g, special, nan_placed, n / 2));
            }
            if (y + 1 < g)
            {
                add(n + i, i + g, coupling<T>(n + i, i + g, special, nan_placed, n / 2));
            }
        }
    }
    if (special == Special::NaN)
    {
        REQUIRE(nan_placed);
    }
    return sp::to_csc<T>(tb.compress(), alloc);
}

template <typename T> void apply(const Csc<T>& a, const T* x, T* y)
{
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        y[i] = T{};
    }
    for (crd::u32 c = 0; c < pat.cols; ++c)
    {
        for (crd::u32 q = pat.outer_ptr[c]; q < pat.outer_ptr[c + 1]; ++q)
        {
            y[pat.inner_idx[q]] = y[pat.inner_idx[q]] + vals[q] * x[c];
        }
    }
}

// solve(b, x) solves A x = b (least squares when A is tall) for a known x; the ratio of its normwise backward error
// |b - A x| to the bound 4 max(m, n) eps (|A| |x| + |b|), |A| the largest row sum (infinity when solve refuses).
template <typename T, typename Solve>
crd::f64 backward_ratio(const Csc<T>& a, Solve solve, crd::memory::IAllocator* alloc)
{
    const crd::u32 m = a.rows();
    const crd::u32 n = a.cols();
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> ax(alloc);
    crd::containers::Array<crd::f64> row(alloc);
    xtrue.resize(n);
    b.resize(m);
    x.resize(n);
    ax.resize(m);
    row.resize(m);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = scalar<T>(1.0 + 0.01 * static_cast<crd::f64>(i % 37), 0.02 * static_cast<crd::f64>(i % 11));
    }
    apply(a, xtrue.data(), b.data());
    if (!solve(b, x))
    {
        return std::numeric_limits<crd::f64>::infinity();
    }
    apply(a, x.data(), ax.data());
    const auto& pat = a.pattern();
    const auto& vals = a.values().values;
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 q = pat.outer_ptr[c]; q < pat.outer_ptr[c + 1]; ++q)
        {
            row[pat.inner_idx[q]] += magnitude(vals[q]);
        }
    }
    crd::f64 r = 0.0;
    crd::f64 an = 0.0;
    crd::f64 xn = 0.0;
    crd::f64 bn = 0.0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        an = row[i] > an ? row[i] : an;
        const crd::f64 ri = magnitude(b[i] - ax[i]);
        r = ri > r ? ri : r;
        bn = magnitude(b[i]) > bn ? magnitude(b[i]) : bn;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        xn = magnitude(x[i]) > xn ? magnitude(x[i]) : xn;
    }
    const crd::f64 eps = static_cast<crd::f64>(std::numeric_limits<Real<T>>::epsilon());
    const crd::f64 bound = 4.0 * static_cast<crd::f64>(m > n ? m : n) * eps * (an * xn + bn);
    return r / bound;
}

template <typename T> bool same_span(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
{
    return x.size() == y.size() && (x.size() == 0 || std::memcmp(x.data(), y.data(), x.size() * sizeof(T)) == 0);
}

template <typename T> bool same_array(const crd::containers::Array<T>& x, const crd::containers::Array<T>& y)
{
    return same_span<T>({x.data(), x.size()}, {y.data(), y.size()});
}

template <typename F> void on_four_workers(F run)
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4; // a hosted runner's pool on every host: the 8 to 32 forced jobs outnumber its workers
    crd::jobs::init(cfg);
    run();
    crd::jobs::shutdown();
}

// ---- MultifrontalLDLT ----

struct LdltSetup
{
    bool textbook = false; // Bunch-Kaufman's threshold on fundamental fronts (delays); else the production defaults
    bool hermitian = false;
};

template <typename T>
dir::MultifrontalLDLT<T> ldlt(const MultifrontalLdltPick& pick, const Csc<T>& a, crd::u32 jobs, LdltSetup setup,
                              crd::memory::IAllocator* alloc)
{
    dir::MultifrontalLDLT<T> f(alloc);
    if (setup.textbook)
    {
        f.set_pivot_threshold(kTextbookAlpha);
        f.set_amalgamation_relax(1);
    }
    f.factorize_arm(pick, a, jobs, setup.hermitian);
    return f;
}

// info(), the delays, L, D, the pivot kinds and the permutation bit for bit.
template <typename T> bool same_ldlt(const dir::MultifrontalLDLT<T>& x, const dir::MultifrontalLDLT<T>& y)
{
    return x.info() == y.info() && x.delayed_count() == y.delayed_count() && same_span(x.l_col_ptr(), y.l_col_ptr()) &&
           same_span(x.l_row_idx(), y.l_row_idx()) && same_span(x.l_values(), y.l_values()) &&
           same_span(x.d_diag(), y.d_diag()) && same_span(x.d_offdiag(), y.d_offdiag()) &&
           same_span(x.block_kinds(), y.block_kinds()) && same_span(x.perm(), y.perm());
}

template <typename T>
void check_ldlt_bound(const dir::MultifrontalLDLT<T>& f, const Csc<T>& a, crd::memory::IAllocator* alloc)
{
    REQUIRE(f.info() == 0);
    const crd::f64 ratio = backward_ratio<T>(
        a,
        [&](const crd::containers::Array<T>& b, crd::containers::Array<T>& x)
        {
            for (crd::usize i = 0; i < b.size(); ++i)
            {
                x[i] = b[i];
            }
            f.apply_inverse({x.data(), x.size()}, 1);
            return true;
        },
        alloc);
    INFO("backward error over its bound " << ratio);
    CHECK(ratio <= 1.0);
}

template <typename T> void check_ldlt(const Csc<T>& a, LdltSetup setup, bool bounded, crd::memory::IAllocator* alloc)
{
    const auto ref = ldlt<T>({}, a, 1, setup, alloc);
    REQUIRE(ref.front_count() >= 2);
    if (bounded)
    {
        check_ldlt_bound(ref, a, alloc);
    }
    const MultifrontalLdltPick scheduling[] = {
        {}, {.walk = {.level_min = 2}}, {.walk = {.level_min = kAll}}, {.walk = kWithinPool}, {.walk = kWithin32},
    };
    for (crd::usize i = 0; i < std::size(scheduling); ++i)
    {
        for (const crd::u32 jobs : kScheduleJobs)
        {
            INFO("scheduling arm " << i << " on " << jobs << " jobs");
            CHECK(same_ldlt(ldlt<T>(scheduling[i], a, jobs, setup, alloc), ref));
        }
    }
    if constexpr (!crd::hesap::dense::is_complex_v<T>)
    {
        const MultifrontalLdltPick blocking[] = {
            {.block_min = 0, .block_panel = 2},  {.block_min = 0, .block_panel = 3},
            {.block_min = 0, .block_panel = 16}, {.block_min = 0, .block_panel = 128},
            {.block_min = 12, .block_panel = 8},
        };
        for (crd::usize i = 0; i < std::size(blocking); ++i)
        {
            INFO("blocking arm " << i);
            const auto one = ldlt<T>(blocking[i], a, 1, setup, alloc);
            if (bounded)
            {
                check_ldlt_bound(one, a, alloc);
            }
            for (const crd::u32 tile : {1U, 5U, kAll})
            {
                INFO("flush tile " << tile);
                MultifrontalLdltPick pick = blocking[i];
                pick.flush_tile = tile;
                CHECK(same_ldlt(ldlt<T>(pick, a, 1, setup, alloc), one));
            }
            for (const FrontWalkPick& walk : {FrontWalkPick{}, kWithinPool, kWithin32})
            {
                MultifrontalLdltPick pick = blocking[i];
                pick.walk = walk;
                pick.flush_tile = 7;
                for (const crd::u32 jobs : {8U, 32U})
                {
                    INFO("walk " << walk.level_min << " / " << walk.front.workers << " on " << jobs << " jobs");
                    CHECK(same_ldlt(ldlt<T>(pick, a, jobs, setup, alloc), one));
                }
            }
        }
    }
}

template <typename T> void check_ldlt_type(crd::memory::IAllocator* alloc)
{
    for (const Special special : {Special::None, Special::SignedZeroSubnormal, Special::NaN, Special::Breakdown})
    {
        INFO("special " << static_cast<int>(special));
        const bool clean = special == Special::None || special == Special::SignedZeroSubnormal;
        for (const bool hermitian : {false, true})
        {
            if (hermitian && !crd::hesap::dense::is_complex_v<T>)
            {
                continue;
            }
            INFO("hermitian " << hermitian);
            // k = 4 at Bunch-Kaufman's threshold on fundamental fronts delays pivots (the moat-under-delay case);
            // k = 6 at the production threshold and amalgamation does not.
            const Csc<T> delaying = symmetric_fixture<T>(alloc, 4, 3.0, special, hermitian);
            if (special == Special::None && !crd::hesap::dense::is_complex_v<T>)
            {
                REQUIRE(ldlt<T>({}, delaying, 1, {.textbook = true}, alloc).delayed_count() > 0);
            }
            check_ldlt<T>(delaying, {.textbook = true, .hermitian = hermitian}, clean, alloc);
            const Csc<T> wide = symmetric_fixture<T>(alloc, 6, 3.0, special, hermitian);
            check_ldlt<T>(wide, {.textbook = false, .hermitian = hermitian}, false, alloc);
        }
    }
}

// ---- MultifrontalQR ----

template <typename T>
dir::MultifrontalQR<T> qr(const MultifrontalQrPick& pick, const Csc<T>& a, crd::u32 jobs,
                          crd::memory::IAllocator* alloc)
{
    dir::MultifrontalQR<T> f(alloc);
    const auto& vals = a.values().values;
    f.factorize_arm(pick, a.pattern(), {vals.data(), vals.size()}, dir::kQrFrontRelax, jobs);
    return f;
}

// The least-squares solution of A x = A x_true, empty when the solve refuses.
template <typename T>
crd::containers::Array<T> qr_solution(const dir::MultifrontalQR<T>& f, const Csc<T>& a, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> xtrue(alloc);
    crd::containers::Array<T> b(alloc);
    crd::containers::Array<T> x(alloc);
    xtrue.resize(a.cols());
    b.resize(a.rows());
    x.resize(a.cols());
    for (crd::u32 i = 0; i < a.cols(); ++i)
    {
        xtrue[i] = scalar<T>(1.0 - 0.02 * static_cast<crd::f64>(i % 13), 0.01 * static_cast<crd::f64>(i % 7));
    }
    apply(a, xtrue.data(), b.data());
    if (!f.least_squares({b.data(), b.size()}, {x.data(), x.size()}, 1))
    {
        x.clear();
    }
    return x;
}

// info(), the rank and dead columns, R bit for bit, and the least-squares solution (so the Householder vectors).
template <typename T>
bool same_qr(const dir::MultifrontalQR<T>& x, const dir::MultifrontalQR<T>& y, const Csc<T>& a,
             crd::memory::IAllocator* alloc)
{
    return x.info() == y.info() && x.rank() == y.rank() && same_span(x.dead(), y.dead()) &&
           same_array(x.rp(), y.rp()) && same_array(x.rj(), y.rj()) && same_array(x.rx(), y.rx()) &&
           same_array(qr_solution(x, a, alloc), qr_solution(y, a, alloc));
}

template <typename T> void check_qr(const Csc<T>& a, bool bounded, crd::memory::IAllocator* alloc)
{
    const auto ref = qr<T>({}, a, 1, alloc);
    REQUIRE(ref.symbolic().nf() >= 2);
    const auto qr_bound = [&](const dir::MultifrontalQR<T>& f) -> void
    {
        const crd::f64 ratio = backward_ratio<T>(
            a, [&](const crd::containers::Array<T>& b, crd::containers::Array<T>& x)
            { return f.least_squares({b.data(), b.size()}, {x.data(), x.size()}, 1); }, alloc);
        INFO("backward error over its bound " << ratio);
        CHECK(ratio <= 1.0);
    };
    if (bounded)
    {
        qr_bound(ref);
    }
    const MultifrontalQrPick scheduling[] = {
        {}, {.walk = {.level_min = 2}}, {.walk = {.level_min = kAll}}, {.walk = kWithinPool}, {.walk = kWithin32},
    };
    for (crd::usize i = 0; i < std::size(scheduling); ++i)
    {
        for (const crd::u32 jobs : kScheduleJobs)
        {
            INFO("scheduling arm " << i << " on " << jobs << " jobs");
            CHECK(same_qr(qr<T>(scheduling[i], a, jobs, alloc), ref, a, alloc));
        }
    }
    if constexpr (!crd::hesap::dense::is_complex_v<T>)
    {
        const MultifrontalQrPick blocking[] = {
            {.panel = 1, .block_min = 0},  {.panel = 2, .block_min = 0},   {.panel = 7, .block_min = 0},
            {.panel = 48, .block_min = 0}, {.panel = 256, .block_min = 0}, {.panel = 48, .block_min = kAll},
        };
        for (crd::usize i = 0; i < std::size(blocking); ++i)
        {
            INFO("blocking arm " << i);
            const auto one = qr<T>(blocking[i], a, 1, alloc);
            if (bounded)
            {
                qr_bound(one);
            }
            for (const FrontWalkPick& walk : {FrontWalkPick{}, kWithinPool, kWithin32})
            {
                MultifrontalQrPick pick = blocking[i];
                pick.walk = walk;
                for (const crd::u32 jobs : {8U, 32U})
                {
                    INFO("walk " << walk.level_min << " / " << walk.front.workers << " on " << jobs << " jobs");
                    CHECK(same_qr(qr<T>(pick, a, jobs, alloc), one, a, alloc));
                }
            }
        }
    }
}

template <typename T> void check_qr_type(crd::memory::IAllocator* alloc)
{
    for (const Special special : {Special::None, Special::SignedZeroSubnormal, Special::NaN, Special::Breakdown})
    {
        INFO("special " << static_cast<int>(special));
        const Csc<T> a = least_squares_fixture<T>(alloc, 9, special);
        if (special == Special::Breakdown)
        {
            REQUIRE(qr<T>({}, a, 1, alloc).rank() < a.cols());
        }
        check_qr<T>(a, special == Special::None || special == Special::SignedZeroSubnormal, alloc);
    }
}

// ---- MultifrontalCholeskyBlr ----

struct BlrRun
{
    bool ok = false;
    crd::u32 info = 0;
    crd::u64 nnz = 0;
    crd::containers::Array<crd::f64> l; // L's values widened to f64 (exact for f32)
};

template <typename T>
BlrRun blr(const MultifrontalBlrPick& pick, const Csc<T>& a, crd::u32 blr_min, crd::u32 jobs,
           crd::memory::IAllocator* alloc)
{
    dir::MultifrontalCholeskyBlr<T> f(alloc);
    BlrRun run{.l = crd::containers::Array<crd::f64>(alloc)};
    run.ok = f.factorize_arm(pick, a, 8, static_cast<Real<T>>(1e-9), blr_min, jobs);
    run.info = f.info();
    run.nnz = f.nnz();
    const auto lx = f.l_values();
    run.l.resize(lx.size());
    for (crd::usize i = 0; i < lx.size(); ++i)
    {
        run.l[i] = static_cast<crd::f64>(lx[i]);
    }
    return run;
}

bool same_blr(const BlrRun& x, const BlrRun& y)
{
    return x.ok == y.ok && x.info == y.info && x.nnz == y.nnz && same_array(x.l, y.l);
}

template <typename T> void check_blr(const Csc<T>& a, crd::u32 blr_min, crd::memory::IAllocator* alloc)
{
    const BlrRun ref = blr<T>({}, a, blr_min, 1, alloc);
    const MultifrontalBlrPick arms[] = {
        {},
        {.walk = {.level_min = 1}},                                                  // dense levels front-parallel
        {.walk = {.level_min = 1, .front = {.gemm_min_mnk = kNever, .workers = 1}}}, // and every gemm serial
        {.walk = kWithinPool},                                                       // every BLR gemm on the pool
        {.walk = kWithin32, .dense_gemm_min_mnk = 0},                                // every gemm on 32 jobs
        {.walk = {.level_min = 2, .front = {.gemm_min_mnk = 0, .workers = 32}}, .dense_gemm_min_mnk = 0},
    };
    for (crd::usize i = 0; i < std::size(arms); ++i)
    {
        for (const crd::u32 jobs : kScheduleJobs)
        {
            INFO("arm " << i << " on " << jobs << " jobs");
            CHECK(same_blr(blr<T>(arms[i], a, blr_min, jobs, alloc), ref));
        }
    }
}

template <typename T> void check_blr_type(crd::memory::IAllocator* alloc)
{
    for (const Special special : {Special::None, Special::SignedZeroSubnormal, Special::NaN, Special::Breakdown})
    {
        INFO("special " << static_cast<int>(special));
        const Csc<T> a = symmetric_fixture<T>(alloc, 6, 8.0, special, false);
        if (special == Special::Breakdown)
        {
            REQUIRE_FALSE(blr<T>({}, a, 16, 1, alloc).ok);
        }
        for (const crd::u32 blr_min : {16U, kAll})
        {
            INFO("blr_min " << blr_min);
            check_blr<T>(a, blr_min, alloc);
        }
    }
}
} // namespace

TEST_CASE("ldlt_arms: every MultifrontalLDLT lever keeps one job's bits or the backward-error bound",
          "[hesap][direct][jobs][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    on_four_workers(
        [&]
        {
            {
                INFO("f64");
                check_ldlt_type<crd::f64>(&alloc);
            }
            {
                INFO("f32");
                check_ldlt_type<crd::f32>(&alloc);
            }
            {
                INFO("c64");
                check_ldlt_type<crd::hesap::Complex64>(&alloc);
            }
            {
                INFO("c32");
                check_ldlt_type<crd::hesap::Complex32>(&alloc);
            }
        });
}

TEST_CASE("qr_arms: every MultifrontalQR lever keeps one job's bits or the backward-error bound",
          "[hesap][direct][jobs][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    on_four_workers(
        [&]
        {
            {
                INFO("f64");
                check_qr_type<crd::f64>(&alloc);
            }
            {
                INFO("f32");
                check_qr_type<crd::f32>(&alloc);
            }
            {
                INFO("c64");
                check_qr_type<crd::hesap::Complex64>(&alloc);
            }
            {
                INFO("c32");
                check_qr_type<crd::hesap::Complex32>(&alloc);
            }
        });
}

TEST_CASE("blr_arms: every MultifrontalCholeskyBlr lever keeps one job's bits",
          "[hesap][direct][jobs][determinism][kernels]")
{
    crd::memory::TlsfAllocator alloc(256 << 20);
    on_four_workers(
        [&]
        {
            {
                INFO("f64");
                check_blr_type<crd::f64>(&alloc);
            }
            {
                INFO("f32");
                check_blr_type<crd::f32>(&alloc);
            }
        });
}
