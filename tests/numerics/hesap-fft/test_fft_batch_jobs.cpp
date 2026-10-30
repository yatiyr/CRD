// The batched transforms' crd-jobs levers (fft/detail/batch_arms.hpp; SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE):
// batch_jobs's cuts and forced jobs, run_batch's ranges, and NdFftPlan's lines on the jobs, bit for bit against one job
// at every forced job count from 1 to 32, on the default fiber tiers of a pool wider than the Large tier's base.
#include <crd/containers/array.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/detail/batch_arms.hpp>
#include <crd/hesap/fft/nd_fft.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
namespace fft = crd::hesap::fft;
namespace cont = crd::containers;
using crd::hesap::Complex;
using crd::jobs::StackSize;
using fft::detail::BatchPick;

namespace
{

// A pool of `threads` workers on the default fiber tiers, for one scope.
struct Pool
{
    explicit Pool(u32 threads)
    {
        crd::jobs::Config cfg;
        cfg.num_threads = threads;
        crd::jobs::init(cfg);
    }
    ~Pool() { crd::jobs::shutdown(); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
};

template <typename T> void fill(Complex<T>* x, usize n, crd::u64 seed)
{
    crd::u64 s = seed;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto re = static_cast<T>((static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)) * 2.0 - 1.0);
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto im = static_cast<T>((static_cast<double>(s >> 11) / static_cast<double>(1ULL << 53)) * 2.0 - 1.0);
        x[i] = Complex<T>{re, im};
    }
}

constexpr usize kGuard = 8; // words of guard on each side of a transformed span

// One N-d transform of `dims` through a plan of `job_plans` sets at `pick`, on a copy of `input` held between guard
// words; the guards must come back untouched. Returns the result's bytes in `out`.
template <typename T>
void nd_run(cont::Array<Complex<T>>& out, const cont::Array<Complex<T>>& input, const usize* dims, usize nd,
            u32 job_plans, const BatchPick& pick, fft::FftDirection dir)
{
    crd::memory::TlsfAllocator alloc(1U << 26); // 32 sets of f32 256's hierarchical 1 MiB block
    const usize total = input.size();
    cont::Array<Complex<T>> buf(&alloc);
    buf.resize(total + 2 * kGuard);
    const Complex<T> guard{static_cast<T>(-7.25), static_cast<T>(3.5)};
    for (usize i = 0; i < kGuard; ++i)
    {
        buf[i] = guard;
        buf[kGuard + total + i] = guard;
    }
    std::memcpy(buf.data() + kGuard, input.data(), total * sizeof(Complex<T>));
    const fft::NdFftPlan<T> plan(&alloc, cont::ConstSpan<usize>(dims, nd), job_plans);
    REQUIRE(plan.job_plans() == job_plans);
    plan.execute_arm(cont::Span<Complex<T>>(buf.data() + kGuard, total), dir, pick);
    bool guards = true;
    for (usize i = 0; i < kGuard; ++i)
    {
        guards = guards && std::memcmp(&buf[i], &guard, sizeof(guard)) == 0 &&
                 std::memcmp(&buf[kGuard + total + i], &guard, sizeof(guard)) == 0;
    }
    CHECK(guards);
    out.resize(total);
    std::memcpy(out.data(), buf.data() + kGuard, total * sizeof(Complex<T>));
}

template <typename T> void nd_jobs_bit_exact()
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    struct Shape
    {
        usize nd;
        usize d[3];
    };
    // pow-2 axes, a Bluestein axis, a degenerate axis between two others, fewer lines than jobs.
    const Shape shapes[] = {{2, {64, 32, 0}}, {2, {6, 100, 0}}, {3, {12, 1, 20}},
                            {3, {8, 16, 4}},  {2, {2, 3, 0}},   {1, {256, 0, 0}}};
    const Pool pool(8);
    for (const Shape& sh : shapes)
    {
        usize total = 1;
        for (usize i = 0; i < sh.nd; ++i)
        {
            total *= sh.d[i];
        }
        cont::Array<Complex<T>> x(&alloc);
        x.resize(total);
        fill(x.data(), total, 0x5EEDULL ^ total);
        // IEEE specials: a NaN, both infinities, a -0 and a subnormal, each spreading through its lines.
        if (total >= 8)
        {
            x[1].re = std::numeric_limits<T>::quiet_NaN();
            x[2].im = std::numeric_limits<T>::infinity();
            x[3].re = -std::numeric_limits<T>::infinity();
            x[4].im = static_cast<T>(-0.0);
            x[5].re = std::numeric_limits<T>::denorm_min();
        }
        for (const fft::FftDirection dir : {fft::FftDirection::Forward, fft::FftDirection::Inverse})
        {
            cont::Array<Complex<T>> one(&alloc);
            nd_run(one, x, sh.d, sh.nd, 1U, BatchPick{}, dir);
            // The default pick on a one-set plan is today's serial transform.
            cont::Array<Complex<T>> serial(&alloc);
            nd_run(serial, x, sh.d, sh.nd, 1U, BatchPick{.min_transforms = 0, .workers = 32}, dir);
            CHECK(std::memcmp(one.data(), serial.data(), total * sizeof(Complex<T>)) == 0);
            for (const u32 jobs : {2U, 3U, 4U, 7U, 8U, 16U, 32U})
            {
                for (const usize min_transforms : {usize{0}, usize{4}, fft::detail::kBatchNever})
                {
                    cont::Array<Complex<T>> got(&alloc);
                    nd_run(got, x, sh.d, sh.nd, jobs, BatchPick{.min_transforms = min_transforms, .workers = jobs},
                           dir);
                    INFO("nd " << sh.nd << " dims " << sh.d[0] << " " << sh.d[1] << " " << sh.d[2] << " jobs " << jobs
                               << " min_transforms " << min_transforms << " inverse "
                               << (dir == fft::FftDirection::Inverse));
                    CHECK(std::memcmp(one.data(), got.data(), total * sizeof(Complex<T>)) == 0);
                }
            }
        }
    }
}

} // namespace

TEST_CASE("fft batch jobs: batch_jobs's cuts and forced jobs", "[fft][batch-jobs]")
{
    // A pool that is down: the calling thread.
    CHECK(fft::detail::batch_jobs(BatchPick{}, 1000, 1024) == 1U);
    CHECK(fft::detail::batch_work(3, 1024) == 3U * 1024U * 10U);
    CHECK(fft::detail::batch_work(5, 1000) == 5U * 1000U * 10U);
    CHECK(fft::detail::batch_work(2, 1) == 2U);
    {
        const Pool pool(1);
        CHECK(fft::detail::batch_jobs(BatchPick{.min_transforms = 0, .workers = 8}, 1000, 64) == 1U);
    }
    {
        const Pool pool(8);
        // The default: four transforms and up, one job a worker, at most one a transform.
        CHECK(fft::detail::batch_jobs(BatchPick{}, 3, 64) == 1U);
        CHECK(fft::detail::batch_jobs(BatchPick{}, 4, 64) == 4U);
        CHECK(fft::detail::batch_jobs(BatchPick{}, 1000, 64) == 8U);
        // The work cut: transforms x n x ceil(log2 n).
        const usize work = fft::detail::batch_work(1000, 64);
        CHECK(fft::detail::batch_jobs(BatchPick{.min_work = work}, 1000, 64) == 8U);
        CHECK(fft::detail::batch_jobs(BatchPick{.min_work = work + 1}, 1000, 64) == 1U);
        // Forced jobs, past the pool too, at most one a transform.
        CHECK(fft::detail::batch_jobs(BatchPick{.workers = 2}, 1000, 64) == 2U);
        CHECK(fft::detail::batch_jobs(BatchPick{.workers = 32}, 1000, 64) == 32U);
        CHECK(fft::detail::batch_jobs(BatchPick{.workers = 32}, 5, 64) == 5U);
        CHECK(fft::detail::batch_jobs(BatchPick{.min_transforms = fft::detail::kBatchNever}, 1000, 64) == 1U);
    }
}

TEST_CASE("fft batch jobs: run_batch covers every transform once, a contiguous range a job", "[fft][batch-jobs]")
{
    const Pool pool(4);
    for (const usize count : {usize{1}, usize{2}, usize{5}, usize{31}, usize{32}, usize{33}, usize{1000}})
    {
        for (const u32 jobs : {1U, 2U, 3U, 4U, 16U, 32U})
        {
            if (jobs > count)
            {
                continue;
            }
            u32 owner[1000];
            for (usize i = 0; i < count; ++i)
            {
                owner[i] = ~0U;
            }
            fft::detail::run_batch(jobs, count, StackSize::Small,
                                   [&owner](u32 job, usize first, usize last)
                                   {
                                       for (usize i = first; i < last; ++i)
                                       {
                                           owner[i] = owner[i] == ~0U ? job : ~1U;
                                       }
                                   });
            bool ok = owner[0] == 0U && owner[count - 1] == jobs - 1U;
            for (usize i = 0; i < count; ++i)
            {
                ok = ok && owner[i] < jobs && (i == 0 || owner[i] == owner[i - 1] || owner[i] == owner[i - 1] + 1U);
            }
            INFO("count " << count << " jobs " << jobs);
            CHECK(ok);
        }
    }
}

TEST_CASE("fft batch jobs: Large-stack batches on every worker of a pool wider than 16 never empty the tier",
          "[fft][batch-jobs]")
{
    // Twenty workers on the default tiers: the Large tier holds a fiber a thread. With the 16 it held before, rounds of
    // one Large job a worker found it empty (each worker keeps the fiber of the job it ran), an always-on fatal.
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize dims[2] = {64, 64};
    cont::Array<Complex<f64>> x(&alloc);
    x.resize(64 * 64);
    fill(x.data(), x.size(), 77);
    cont::Array<Complex<f64>> one(&alloc);
    const Pool pool(20);
    nd_run(one, x, dims, 2, 1U, BatchPick{}, fft::FftDirection::Forward);
    for (int round = 0; round < 16; ++round)
    {
        cont::Array<Complex<f64>> got(&alloc);
        nd_run(got, x, dims, 2, 20U, BatchPick{}, fft::FftDirection::Forward);
        CHECK(std::memcmp(one.data(), got.data(), one.size() * sizeof(Complex<f64>)) == 0);
    }
    const crd::jobs::JobsPressure p = crd::jobs::pressure_snapshot();
    CHECK(p.fibers[2].capacity == 20U);
    CHECK(p.fibers[2].refusals == 0U);
}

TEST_CASE("fft batch jobs: NdFftPlan's lines on 1 to 32 jobs give one job's bits (f64)", "[fft][batch-jobs][nd]")
{
    nd_jobs_bit_exact<f64>();
}

TEST_CASE("fft batch jobs: NdFftPlan's lines on 1 to 32 jobs give one job's bits (f32)", "[fft][batch-jobs][nd]")
{
    nd_jobs_bit_exact<f32>();
}
