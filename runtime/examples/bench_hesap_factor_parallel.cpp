// bench_hesap_factor_parallel — the dense factors' and trsm's crd-jobs board (SIMD-DISPATCH, the folded
// HESAP-JOBS-RETUNE).
//
// One process per thread count: `--threads N` starts crd::jobs on N workers (default: the jobs default) and times, at
// each size, the one-job factor (the control) and factor_lu_arm / factor_cholesky_arm / factor_qr_arm on N jobs down
// each arm of detail::LuPick, CholeskyPick and QrPick (production's levers; LU's trailing update serial below
// gemm_parallel_auto's 256K and its U12 solve split by columns; Cholesky's L21 solve and trailing syrk always or never
// split; QR's trailing gemms always or never on the jobs, square and tall 2n by n; the panel width and the unblocked
// cut), and trsm_arm's column split of an n by n B against the one-call solve, round by round in turn so the arms
// share the host's state; the median of the rounds is reported per size with its speedup over one job. Arms that keep
// the panel width and the cut are gated bit for bit against the control (ADR-0063: each job solves disjoint rows or
// columns); the others regroup the trailing sums and are gated by the factor's backward-error bound: elementwise
// |PA - LU| <= 4 n eps |L| |U| (|A - L L^T| for Cholesky), column by column |a_j - (QR)_j| <= m n eps |a_j| for QR
// (Higham 19.4, Q R formed in double), and |B - L X| <= 4 n eps |L| |X| for the solve (Higham 8.5). A miss exits 1.
// The level line names the dense kernels' level (CRD_CPU_LEVEL forces it).
//   bench_hesap_factor_parallel [--threads N]

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/detail/factor_arms.hpp>
#include <crd/hesap/dense/detail/gemm_arms.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::hesap::dense::Cholesky;
using crd::hesap::dense::Layout;
using crd::hesap::dense::LU;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::QR;
using crd::hesap::dense::Trans;
using crd::hesap::dense::Triangular;
using crd::hesap::dense::TriangularDiag;
using crd::hesap::dense::TriangularSide;
using crd::hesap::dense::detail::CholeskyPick;
using crd::hesap::dense::detail::LuPick;
using crd::hesap::dense::detail::QrPick;
using crd::hesap::dense::detail::TrsmPick;
using Clock = std::chrono::steady_clock;

constexpr int kRounds = 7;
constexpr usize kNever = ~usize{0};
bool g_failed = false;

template <typename Pick> struct Arm
{
    const char* name;
    Pick pick;
    bool same_bits; // the control's panel width and cut: gated bit for bit, otherwise by the backward-error bound
};

const Arm<LuPick> kLuArms[] = {
    {"production", LuPick{}, true},
    {"trailing-serial-256K", LuPick{.trailing_min = 256U * 1024U}, true},
    {"trsm-columns-64K", LuPick{.trsm_min = 64U * 1024U}, true},
    {"trsm-columns-all", LuPick{.trsm_min = 0}, true},
    {"block-32", LuPick{.block = 32}, false},
    {"block-128", LuPick{.block = 128}, false},
};

const Arm<CholeskyPick> kCholeskyArms[] = {
    {"production", CholeskyPick{}, true},
    {"trsm-rows-all", CholeskyPick{.trsm_min = 0}, true},
    {"trsm-rows-never", CholeskyPick{.trsm_min = kNever}, true},
    {"syrk-split-all", CholeskyPick{.syrk_mmk = 0}, true},
    {"syrk-split-never", CholeskyPick{.syrk_mmk = kNever}, true},
    {"unblocked-to-128", CholeskyPick{.unblocked_max = 128}, false},
    {"unblocked-to-512", CholeskyPick{.unblocked_max = 512}, false},
    {"block-32", CholeskyPick{.block = 32}, false},
    {"block-128", CholeskyPick{.block = 128}, false},
};

const Arm<QrPick> kQrArms[] = {
    {"production", QrPick{}, true},
    {"trailing-split-all", QrPick{.trailing_min = 0}, true},
    {"trailing-serial", QrPick{.trailing_min = kNever}, true},
    {"blocked-always", QrPick{.unblocked_max = 0}, false},
    {"unblocked-to-256", QrPick{.unblocked_max = 256}, false},
    {"block-16", QrPick{.block = 16}, false},
    {"block-64", QrPick{.block = 64}, false},
};

const Arm<TrsmPick> kTrsmArms[] = {
    {"production", TrsmPick{}, true},
    {"columns-all", TrsmPick{.parallel_min = 0}, true},
    {"columns-1M", TrsmPick{.parallel_min = 1024U * 1024U}, true},
};

// trsm's input: the lower L (well conditioned: the diagonal shifted by n) and the B it solves in place.
template <typename T> struct Solve
{
    Triangular<T, TriangularSide::Lower, TriangularDiag::Explicit> l;
    Matrix<T> b;

    [[nodiscard]] usize rows() const noexcept { return b.rows(); }
    [[nodiscard]] usize cols() const noexcept { return b.cols(); }
};

f64 median(f64* times, int n)
{
    std::sort(times, times + n);
    return times[n / 2];
}

f64 seconds(Clock::time_point t0)
{
    return std::chrono::duration<f64>(Clock::now() - t0).count();
}

template <typename T> void fill(Matrix<T>& x, u32 seed)
{
    u32 s = seed;
    for (usize i = 0; i < x.rows(); ++i)
    {
        for (usize j = 0; j < x.cols(); ++j)
        {
            s = s * 1664525U + 1013904223U;
            x(i, j) = static_cast<T>(static_cast<f64>(s >> 8) / 16777216.0 - 0.5);
        }
    }
}

template <typename T> void copy_into(const Matrix<T>& a, Matrix<T>& to)
{
    for (usize i = 0; i < a.rows(); ++i)
    {
        std::memcpy(&to(i, 0), &a(i, 0), a.cols() * sizeof(T));
    }
}

// The elementwise bound check: |want - lr| <= 4 n eps bound, with lr = op product and bound its magnitudes' product.
template <typename T>
bool within_bound(const Matrix<T>& want, const Matrix<T>& lr, const Matrix<T>& bound, usize n, bool lower_only)
{
    const f64 tol = 4.0 * static_cast<f64>(n) * static_cast<f64>(std::numeric_limits<T>::epsilon());
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < (lower_only ? i + 1 : n); ++j)
        {
            const f64 err = std::fabs(static_cast<f64>(want(i, j)) - static_cast<f64>(lr(i, j)));
            if (!(err <= tol * static_cast<f64>(bound(i, j)) + std::numeric_limits<f64>::min()))
            {
                return false;
            }
        }
    }
    return true;
}

// |PA - LU| <= 4 n eps |L| |U| elementwise (Higham 9.3), the products formed by gemm.
template <typename T> bool lu_backward_stable(const LU<T>& lu, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    const usize n = lu.n();
    if (lu.info() != 0)
    {
        return false;
    }
    Matrix<T> l(alloc, n, n);
    Matrix<T> u(alloc, n, n);
    Matrix<T> al(alloc, n, n);
    Matrix<T> au(alloc, n, n);
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < n; ++j)
        {
            const T x = lu.packed()(i, j);
            l(i, j) = j < i ? x : T{0};
            u(i, j) = j >= i ? x : T{0};
            al(i, j) = std::fabs(l(i, j));
            au(i, j) = std::fabs(u(i, j));
        }
        l(i, i) = T{1};
        al(i, i) = T{1};
    }
    Matrix<T> lr(alloc, n, n);
    Matrix<T> bound(alloc, n, n);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, l.cview(), u.cview(), T{0}, lr.view(), Trans::None, Trans::None,
                                                 alloc);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, al.cview(), au.cview(), T{0}, bound.view(), Trans::None,
                                                 Trans::None, alloc);
    // P A: the pivots replayed in order on the rows of A.
    Matrix<T> pa(alloc, n, n);
    copy_into(a, pa);
    for (usize k = 0; k < n; ++k)
    {
        const usize r = lu.permutation().pivot_at(k);
        for (usize j = 0; r != k && j < n; ++j)
        {
            std::swap(pa(k, j), pa(r, j));
        }
    }
    return within_bound(pa, lr, bound, n, false);
}

// |A - L L^T| <= 4 n eps |L| |L^T| elementwise on the lower triangle (Higham 10.3).
template <typename T>
bool cholesky_backward_stable(const Cholesky<T>& chol, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    const usize n = chol.n();
    if (chol.info() != 0)
    {
        return false;
    }
    Matrix<T> l(alloc, n, n);
    Matrix<T> al(alloc, n, n);
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < n; ++j)
        {
            l(i, j) = j <= i ? chol.packed()(i, j) : T{0};
            al(i, j) = std::fabs(l(i, j));
        }
    }
    Matrix<T> lr(alloc, n, n);
    Matrix<T> bound(alloc, n, n);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, l.cview(), l.cview(), T{0}, lr.view(), Trans::None,
                                                 Trans::Transpose, alloc);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, al.cview(), al.cview(), T{0}, bound.view(), Trans::None,
                                                 Trans::Transpose, alloc);
    return within_bound(a, lr, bound, n, true);
}

// One factor of a's copy (copy untimed); returns the factor's seconds.
template <typename T> f64 time_factor(const LuPick& pick, const Matrix<T>& a, LU<T>& lu, crd::memory::IAllocator* m)
{
    copy_into(a, lu.packed());
    const Clock::time_point t0 = Clock::now();
    crd::hesap::dense::detail::factor_lu_arm<T, Layout::RowMajor>(pick, lu, m);
    return seconds(t0);
}

template <typename T>
f64 time_factor(const CholeskyPick& pick, const Matrix<T>& a, Cholesky<T>& chol, crd::memory::IAllocator* m)
{
    copy_into(a, chol.packed());
    const Clock::time_point t0 = Clock::now();
    crd::hesap::dense::detail::factor_cholesky_arm<T, Layout::RowMajor>(pick, chol, m);
    return seconds(t0);
}

template <typename T> bool same_bits(const LU<T>& x, const LU<T>& y)
{
    const usize n = x.n();
    if (x.info() != y.info() || std::memcmp(x.packed().data(), y.packed().data(), n * x.packed().ld() * sizeof(T)) != 0)
    {
        return false;
    }
    for (usize i = 0; i < n; ++i)
    {
        if (x.permutation().pivot_at(i) != y.permutation().pivot_at(i))
        {
            return false;
        }
    }
    return true;
}

template <typename T> bool same_bits(const Cholesky<T>& x, const Cholesky<T>& y)
{
    if (x.info() != y.info())
    {
        return false;
    }
    for (usize i = 0; i < x.n(); ++i)
    {
        if (std::memcmp(&x.packed()(i, 0), &y.packed()(i, 0), (i + 1) * sizeof(T)) != 0)
        {
            return false;
        }
    }
    return true;
}

template <typename T> f64 time_factor(const QrPick& pick, const Matrix<T>& a, QR<T>& qr, crd::memory::IAllocator*)
{
    copy_into(a, qr.packed());
    const Clock::time_point t0 = Clock::now();
    crd::hesap::dense::detail::factor_qr_arm<T, Layout::RowMajor>(pick, qr);
    return seconds(t0);
}

template <typename T> f64 time_factor(const TrsmPick& pick, const Solve<T>& in, Matrix<T>& x, crd::memory::IAllocator*)
{
    copy_into(in.b, x);
    const Clock::time_point t0 = Clock::now();
    crd::hesap::dense::detail::trsm_arm<T, TriangularSide::Lower, TriangularDiag::Explicit>(pick, T{1}, in.l, x.view(),
                                                                                            Trans::None);
    return seconds(t0);
}

template <typename T> bool same_bits(const QR<T>& x, const QR<T>& y)
{
    return std::memcmp(x.packed().data(), y.packed().data(), x.rows() * x.packed().ld() * sizeof(T)) == 0 &&
           std::memcmp(x.taus().data(), y.taus().data(), x.num_reflectors() * sizeof(T)) == 0;
}

template <typename T> bool same_bits(const Matrix<T>& x, const Matrix<T>& y)
{
    return std::memcmp(x.data(), y.data(), x.rows() * x.ld() * sizeof(T)) == 0;
}

template <typename T> bool backward_stable(const LU<T>& f, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return lu_backward_stable(f, a, alloc);
}

template <typename T> bool backward_stable(const Cholesky<T>& f, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return cholesky_backward_stable(f, a, alloc);
}

// |a_j - (QR)_j| <= m n eps |a_j| column by column (Higham Theorem 19.4 with c = 1), Q R formed in double from the
// stored reflectors.
template <typename T> bool backward_stable(const QR<T>& qr, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    const usize m = qr.rows();
    const usize n = qr.cols();
    const usize k = qr.num_reflectors();
    const f64 tol = static_cast<f64>(m) * static_cast<f64>(n) * static_cast<f64>(std::numeric_limits<T>::epsilon());
    Matrix<f64> x(alloc, 1, m);
    for (usize j = 0; j < n; ++j)
    {
        for (usize i = 0; i < m; ++i)
        {
            x(0, i) = i <= j ? static_cast<f64>(qr.packed()(i, j)) : 0.0;
        }
        for (usize r = k; r-- > 0;)
        {
            f64 w = x(0, r);
            for (usize i = r + 1; i < m; ++i)
            {
                w += static_cast<f64>(qr.packed()(i, r)) * x(0, i);
            }
            w *= static_cast<f64>(qr.taus()[r]);
            x(0, r) -= w;
            for (usize i = r + 1; i < m; ++i)
            {
                x(0, i) -= w * static_cast<f64>(qr.packed()(i, r));
            }
        }
        f64 err = 0.0;
        f64 norm = 0.0;
        for (usize i = 0; i < m; ++i)
        {
            const f64 aij = static_cast<f64>(a(i, j));
            err += (aij - x(0, i)) * (aij - x(0, i));
            norm += aij * aij;
        }
        if (!(std::sqrt(err) <= tol * std::sqrt(norm) + std::numeric_limits<f64>::min()))
        {
            return false;
        }
    }
    return true;
}

// |B - L X| <= 4 n eps |L| |X| elementwise (Higham Theorem 8.5), the products formed by gemm.
template <typename T> bool backward_stable(const Matrix<T>& x, const Solve<T>& in, crd::memory::IAllocator* alloc)
{
    const usize n = in.b.rows();
    const usize cols = in.b.cols();
    Matrix<T> l(alloc, n, n);
    Matrix<T> al(alloc, n, n);
    Matrix<T> ax(alloc, n, cols);
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < n; ++j)
        {
            l(i, j) = in.l.at_value(i, j);
            al(i, j) = std::fabs(l(i, j));
        }
        for (usize j = 0; j < cols; ++j)
        {
            ax(i, j) = std::fabs(x(i, j));
        }
    }
    Matrix<T> lx(alloc, n, cols);
    Matrix<T> bound(alloc, n, cols);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, l.cview(), x.cview(), T{0}, lx.view(), Trans::None, Trans::None,
                                                 alloc);
    crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, al.cview(), ax.cview(), T{0}, bound.view(), Trans::None,
                                                 Trans::None, alloc);
    const f64 tol = 4.0 * static_cast<f64>(n) * static_cast<f64>(std::numeric_limits<T>::epsilon());
    for (usize i = 0; i < n; ++i)
    {
        for (usize j = 0; j < cols; ++j)
        {
            const f64 err = std::fabs(static_cast<f64>(in.b(i, j)) - static_cast<f64>(lx(i, j)));
            if (!(err <= tol * static_cast<f64>(bound(i, j)) + std::numeric_limits<f64>::min()))
            {
                return false;
            }
        }
    }
    return true;
}

// The output a board times into, shaped from its input.
template <typename T> LU<T> make_out(const LU<T>*, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return LU<T>(alloc, a.rows());
}

template <typename T> Cholesky<T> make_out(const Cholesky<T>*, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return Cholesky<T>(alloc, a.rows());
}

template <typename T> QR<T> make_out(const QR<T>*, const Matrix<T>& a, crd::memory::IAllocator* alloc)
{
    return QR<T>(alloc, a.rows(), a.cols());
}

template <typename T> Matrix<T> make_out(const Matrix<T>*, const Solve<T>& in, crd::memory::IAllocator* alloc)
{
    return Matrix<T>(alloc, in.b.rows(), in.b.cols());
}

// The board of one factor at one size: Factor is LU<T>, Cholesky<T> or QR<T> with a its input (A, or the SPD A in
// full), or the Matrix<T> X that trsm solves from a Solve<T>.
template <typename T, typename Factor, typename Pick, usize N, typename Input>
void board(const char* label, const char* factor, const Arm<Pick> (&arms)[N], const Input& a, f64 flops, u32 workers,
           crd::memory::IAllocator* alloc)
{
    const usize n = a.rows();
    const usize n_cols = a.cols();
    const int reps = static_cast<int>(std::clamp(5e7 / flops, 1.0, 256.0));
    Pick serial_pick{};
    serial_pick.workers = 1;
    Factor control = make_out(static_cast<const Factor*>(nullptr), a, alloc);
    Factor out = make_out(static_cast<const Factor*>(nullptr), a, alloc);
    f64 serial_times[kRounds] = {};
    f64 times[N][kRounds] = {};
    bool live[N] = {};
    std::fill(live, live + N, true);
    for (int round = -1; round < kRounds; ++round) // round -1 warms every path and gates it
    {
        f64 s = 0.0;
        for (int r = 0; r < reps; ++r)
        {
            s += time_factor(serial_pick, a, control, alloc);
        }
        if (round >= 0)
        {
            serial_times[round] = s;
        }
        for (usize i = 0; i < N; ++i)
        {
            if (!live[i])
            {
                continue;
            }
            Pick pick = arms[i].pick;
            pick.workers = workers;
            f64 t = 0.0;
            for (int r = 0; r < reps; ++r)
            {
                t += time_factor(pick, a, out, alloc);
            }
            if (round >= 0)
            {
                times[i][round] = t;
            }
            if (round == -1 && !(arms[i].same_bits ? same_bits(out, control) : backward_stable(out, a, alloc)))
            {
                std::fprintf(stderr, "GATE MISS: %s %s %zu x %zu arm %s: %s\n", label, factor, n, n_cols, arms[i].name,
                             arms[i].same_bits ? "the factor differs from one job's" : "past the backward-error bound");
                g_failed = true;
                live[i] = false;
            }
        }
    }
    if (!backward_stable(control, a, alloc))
    {
        std::fprintf(stderr, "GATE MISS: %s %s %zu x %zu: one job's factor is past the backward-error bound\n", label,
                     factor, n, n_cols);
        g_failed = true;
    }
    const f64 serial_s = median(serial_times, kRounds) / reps;
    std::printf("%s %s %zu x %zu: one job %.4f ms\n", label, factor, n, n_cols, serial_s * 1e3);
    for (usize i = 0; i < N; ++i)
    {
        if (!live[i])
        {
            continue;
        }
        const f64 s = median(times[i], kRounds) / reps;
        std::printf("  %-22s %10.4f ms  %8.2f GFLOPS  %5.2fx one job\n", arms[i].name, s * 1e3,
                    s > 0.0 ? flops / (s * 1e9) : 0.0, s > 0.0 ? serial_s / s : 0.0);
    }
}

template <typename T> void run_type(const char* label, u32 workers)
{
    const usize sizes[] = {64, 128, 256, 384, 512, 1024, 2048};
    for (const usize n : sizes)
    {
        crd::memory::TlsfAllocator alloc(1024ULL * 1024ULL * 1024ULL);
        const f64 nd = static_cast<f64>(n);
        Matrix<T> a(&alloc, n, n);
        fill(a, 1U + static_cast<u32>(n));
        board<T, LU<T>>(label, "LU", kLuArms, a, 2.0 * nd * nd * nd / 3.0, workers, &alloc);
        // The SPD A = B B^T + n I, its upper triangle zero as factor_cholesky's Symmetric form leaves it.
        Matrix<T> spd(&alloc, n, n);
        crd::hesap::dense::gemm<T, Layout::RowMajor>(T{1}, a.cview(), a.cview(), T{0}, spd.view(), Trans::None,
                                                     Trans::Transpose, &alloc);
        for (usize i = 0; i < n; ++i)
        {
            spd(i, i) += static_cast<T>(n);
            for (usize j = i + 1; j < n; ++j)
            {
                spd(i, j) = T{0};
            }
        }
        board<T, Cholesky<T>>(label, "Cholesky", kCholeskyArms, spd, nd * nd * nd / 3.0, workers, &alloc);
        // QR square and tall 2n by n (least squares, where the unblocked cut was set): 2 m n^2 - 2 n^3 / 3 flops.
        board<T, QR<T>>(label, "QR", kQrArms, a, 4.0 * nd * nd * nd / 3.0, workers, &alloc);
        Matrix<T> tall(&alloc, 2 * n, n);
        fill(tall, 2U + static_cast<u32>(n));
        board<T, QR<T>>(label, "QR", kQrArms, tall, 10.0 * nd * nd * nd / 3.0, workers, &alloc);
        // trsm: L X = B for an n by n B, L's diagonal shifted by n; n^3 flops.
        Solve<T> solve{Triangular<T, TriangularSide::Lower, TriangularDiag::Explicit>(&alloc, n),
                       Matrix<T>(&alloc, n, n)};
        fill(solve.b, 3U + static_cast<u32>(n));
        for (usize i = 0; i < n; ++i)
        {
            for (usize j = 0; j < i; ++j)
            {
                solve.l.at(i, j) = a(i, j);
            }
            solve.l.at(i, i) = a(i, i) + static_cast<T>(n);
        }
        board<T, Matrix<T>>(label, "trsm", kTrsmArms, solve, nd * nd * nd, workers, &alloc);
    }
}
} // namespace

int main(int argc, char** argv)
{
    u32 workers = 0;
    if (argc == 3 && std::strcmp(argv[1], "--threads") == 0)
    {
        workers = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    }
    else if (argc != 1)
    {
        std::fprintf(stderr, "usage: bench_hesap_factor_parallel [--threads N]\n");
        return 2;
    }
    crd::jobs::Config cfg;
    if (workers != 0)
    {
        cfg.num_threads = workers;
    }
    crd::jobs::init(cfg);
    const u32 jobs = crd::jobs::num_workers();
    std::printf("[bench_hesap_factor_parallel] the dense factors' levers on %u jobs against one job, median of %d "
                "rounds. hesap-dense level %s (selected %s)\n",
                jobs, kRounds, crd::cpu_level_name(crd::hesap::dense::dense_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected));
    run_type<f32>("f32", jobs);
    run_type<f64>("f64", jobs);
    crd::jobs::shutdown();
    if (g_failed)
    {
        std::fprintf(stderr, "bench_hesap_factor_parallel: a gate failed\n");
        return 1;
    }
    return 0;
}
