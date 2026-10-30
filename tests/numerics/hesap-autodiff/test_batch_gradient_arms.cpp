// batch_gradient's crd-jobs picks (reverse.hpp, detail::BatchGradientPick): every sample cut, fold split and job count
// gives the bits of the per-sample gradients folded in ascending sample order (each taken with `gradient` on one
// tape), IEEE specials included, writes only grad[0, n) and gbuf[0, S n), and runs serially on a pool that is down.

#include <crd/hesap/autodiff/reverse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;
using crd::u32;
using crd::usize;

namespace
{
constexpr u32 kMaxJobs = 32;
constexpr usize kGuard = 5;
constexpr std::uint64_t kGuardBits = 0x7FF8DEAD7FC0DEADULL; // a NaN payload no gradient carries
constexpr u32 kNever = ~u32{0};

// loss_s = (theta . x_s - y_s)^2 + theta_0 x_s0 / (1 + x_s(n-1)^2): sums, products and a quotient per sample.
struct Loss
{
    const f64* x;
    const f64* y;
    int n;
    rev::Var operator()(const rev::Var* theta, int, int s) const
    {
        const f64* xs = x + static_cast<usize>(s) * static_cast<usize>(n);
        rev::Var acc = theta[0] * xs[0];
        for (int i = 1; i < n; ++i)
        {
            acc = acc + theta[i] * xs[i];
        }
        const rev::Var r = acc - y[s];
        return r * r + (theta[0] * xs[0]) / (1.0 + xs[n - 1] * xs[n - 1]);
    }
};

// Element i of sample s's row of n.
usize at(int s, int n, int i)
{
    return static_cast<usize>(s) * static_cast<usize>(n) + static_cast<usize>(i);
}

f64 guard_value()
{
    f64 g = 0.0;
    std::memcpy(&g, &kGuardBits, sizeof(g));
    return g;
}

bool same_bits(const f64* a, const f64* b, usize count)
{
    return std::memcmp(a, b, count * sizeof(f64)) == 0;
}

bool guards_intact(const crd::containers::Array<f64>& v, usize from)
{
    const f64 g = guard_value();
    for (usize i = from; i < v.size(); ++i)
    {
        if (std::memcmp(&v[i], &g, sizeof(f64)) != 0)
        {
            return false;
        }
    }
    return true;
}

void fill_guard(crd::containers::Array<f64>& v, usize n)
{
    v.resize(n + kGuard);
    for (usize i = 0; i < v.size(); ++i)
    {
        v[i] = guard_value();
    }
}

// Features with every IEEE special on some sample: NaN, +-Inf, -0 and the smallest subnormal.
void fill_samples(crd::containers::Array<f64>& x, crd::containers::Array<f64>& y, int samples, int n, bool specials)
{
    x.resize(static_cast<usize>(samples) * static_cast<usize>(n));
    y.resize(static_cast<usize>(samples) + 1U);
    for (int s = 0; s < samples; ++s)
    {
        for (int i = 0; i < n; ++i)
        {
            x[at(s, n, i)] = std::sin(0.3 + 1.7 * s + 2.1 * i);
        }
        y[static_cast<usize>(s)] = 0.4 + 0.2 * std::cos(0.7 + s);
    }
    if (specials && samples > 4)
    {
        x[1] = std::numeric_limits<f64>::quiet_NaN();
        x[at(1, n, 0)] = std::numeric_limits<f64>::infinity();
        x[at(2, n, 0)] = -std::numeric_limits<f64>::infinity();
        x[at(3, n, 0)] = -0.0;
        x[at(4, n, 0)] = std::numeric_limits<f64>::denorm_min();
    }
}

// The per-sample gradients on one tape (`gradient`) and their fold in ascending sample order from 0.0.
void reference(const Loss& loss, const f64* theta, int samples, int n, rev::Tape& tape, f64* want_grad,
               crd::containers::Array<f64>& want_rows)
{
    want_rows.resize(static_cast<usize>(samples) * static_cast<usize>(n));
    rev::Var leaves[32];
    for (int s = 0; s < samples; ++s)
    {
        const auto fs = [&loss, s](const rev::Var* th, int k)
        {
            return loss(th, k, s);
        };
        rev::gradient(fs, {theta, static_cast<usize>(n)}, {&want_rows[at(s, n, 0)], static_cast<usize>(n)}, tape,
                      {leaves, static_cast<usize>(n)});
    }
    for (int i = 0; i < n; ++i)
    {
        want_grad[i] = 0.0;
    }
    for (int s = 0; s < samples; ++s)
    {
        for (int i = 0; i < n; ++i)
        {
            want_grad[i] += want_rows[at(s, n, i)];
        }
    }
}

struct Tapes
{
    crd::memory::TlsfAllocator base{16U << 20U};
    crd::memory::ThreadSafeAllocator shared{&base};
    crd::containers::Array<rev::Tape> tapes{&shared};
    rev::Tape* ptrs[kMaxJobs] = {};

    Tapes()
    {
        tapes.reserve(kMaxJobs);
        for (u32 j = 0; j < kMaxJobs; ++j)
        {
            tapes.emplace_back(&shared);
        }
        for (u32 j = 0; j < kMaxJobs; ++j)
        {
            ptrs[j] = &tapes[j];
        }
    }
};

const rev::detail::BatchGradientPick kPicks[] = {
    {},
    {.jobs_min_samples = kNever},
    {.jobs_min_samples = 64},
    {.fold_jobs = 2},
    {.fold_jobs = 4},
    {.fold_jobs = 32},
    {.jobs_min_samples = kNever, .fold_jobs = 8},
};

// Every pick at every job count against the reference, bit for bit, on grad and on gbuf, with the guards intact.
void check_every_pick(Tapes& t, int n, bool specials)
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const int sample_counts[] = {0, 1, 3, 37, 200};
    const u32 job_counts[] = {1, 2, 4, 8, 16, 32};
    for (const int samples : sample_counts)
    {
        crd::containers::Array<f64> x(&alloc);
        crd::containers::Array<f64> y(&alloc);
        fill_samples(x, y, samples, n, specials);
        const Loss loss{x.data(), y.data(), n};
        f64 theta[32];
        for (int i = 0; i < n; ++i)
        {
            theta[i] = 0.5 - 0.11 * i;
        }
        f64 want[32];
        crd::containers::Array<f64> want_rows(&alloc);
        reference(loss, theta, samples, n, *t.ptrs[0], want, want_rows);
        const usize rows = static_cast<usize>(samples) * static_cast<usize>(n);
        for (const auto& pick : kPicks)
        {
            for (const u32 jobs : job_counts)
            {
                crd::containers::Array<f64> grad(&alloc);
                crd::containers::Array<f64> gbuf(&alloc);
                fill_guard(grad, static_cast<usize>(n));
                fill_guard(gbuf, rows);
                rev::detail::batch_gradient_arm(pick, loss, {theta, static_cast<usize>(n)}, samples,
                                                {grad.data(), static_cast<usize>(n)}, {t.ptrs, jobs},
                                                {gbuf.data(), rows}, jobs);
                INFO("n " << n << " S " << samples << " jobs " << jobs << " jobs_min_samples " << pick.jobs_min_samples
                          << " fold_jobs " << pick.fold_jobs);
                const bool grad_same = same_bits(grad.data(), want, static_cast<usize>(n));
                const bool rows_same = same_bits(gbuf.data(), want_rows.data(), rows);
                CHECK(grad_same);
                CHECK(rows_same);
                CHECK(guards_intact(grad, static_cast<usize>(n)));
                CHECK(guards_intact(gbuf, rows));
            }
        }
    }
}
} // namespace

TEST_CASE("batch_gradient picks: every sample cut, fold split and job count gives the fixed-order fold's bits",
          "[autodiff][reverse][moat][jobs-picks]")
{
    crd::jobs::init(crd::jobs::Config{8U});
    {
        Tapes t;
        check_every_pick(t, 1, false);
        check_every_pick(t, 3, true);
        check_every_pick(t, 32, true);

        // The public entry is the default pick.
        crd::memory::TlsfAllocator alloc(1U << 20U);
        crd::containers::Array<f64> x(&alloc);
        crd::containers::Array<f64> y(&alloc);
        fill_samples(x, y, 37, 3, true);
        const Loss loss{x.data(), y.data(), 3};
        const f64 theta[3] = {0.5, -0.2, 0.9};
        f64 a[3];
        f64 b[3];
        f64 gbuf[37 * 3];
        rev::batch_gradient(loss, {theta, 3}, 37, {a, 3}, {t.ptrs, 8}, {gbuf, 37 * 3}, 8U);
        rev::detail::batch_gradient_arm(rev::detail::BatchGradientPick{}, loss, {theta, 3}, 37, {b, 3}, {t.ptrs, 1},
                                        {gbuf, 37 * 3}, 1U);
        CHECK(same_bits(a, b, 3));
    }
    crd::jobs::shutdown();
}

TEST_CASE("batch_gradient picks: a pool that is down runs every job count on the calling thread on tapes[0]",
          "[autodiff][reverse][moat][jobs-picks]")
{
    REQUIRE(crd::jobs::num_workers() == 0U);
    Tapes t;
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::containers::Array<f64> x(&alloc);
    crd::containers::Array<f64> y(&alloc);
    fill_samples(x, y, 37, 3, true);
    const Loss loss{x.data(), y.data(), 3};
    const f64 theta[3] = {0.5, -0.2, 0.9};
    f64 want[3];
    crd::containers::Array<f64> want_rows(&alloc);
    reference(loss, theta, 37, 3, *t.ptrs[1], want, want_rows);
    for (const auto& pick : kPicks)
    {
        crd::containers::Array<f64> grad(&alloc);
        crd::containers::Array<f64> gbuf(&alloc);
        fill_guard(grad, 3);
        fill_guard(gbuf, 37 * 3);
        // One tape is all the down pool's one job needs, whatever num_jobs says.
        rev::detail::batch_gradient_arm(pick, loss, {theta, 3}, 37, {grad.data(), 3}, {t.ptrs, 1},
                                        {gbuf.data(), 37 * 3}, 16U);
        const bool grad_same = same_bits(grad.data(), want, 3);
        const bool rows_same = same_bits(gbuf.data(), want_rows.data(), 37 * 3);
        CHECK(grad_same);
        CHECK(rows_same);
        CHECK(guards_intact(grad, 3));
        CHECK(guards_intact(gbuf, 37 * 3));
    }
}
