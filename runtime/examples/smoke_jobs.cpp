#include <crd/jobs/jobs.hpp>
#include <crd/core/assert.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <span>

// ---------------------------------------------------------------------------
// smoke_jobs — public API integration demo (crd-jobs v1k)
//
// Exercises: init/shutdown, run/wait, make_job<F>, parallel_for,
//            H/N/L priority all-ran, frame_alloc/frame_reset.
// ---------------------------------------------------------------------------

int main()
{
    std::printf("=== smoke_jobs (crd-jobs v1k — public API integration) ===\n");

    crd::jobs::Config cfg{};
    cfg.num_threads = 4;
    crd::jobs::init(cfg);

    // ------------------------------------------------------------------
    // 1. Basic run + wait
    // ------------------------------------------------------------------
    {
        std::atomic<int> done{0};
        auto job = crd::jobs::make_job([&done]() { done.fetch_add(1, std::memory_order_relaxed); });
        crd::jobs::Counter* c = crd::jobs::run(job);
        crd::jobs::wait(c);
        CRD_ASSERT(done.load() == 1);
        std::printf("[smoke_jobs] 1. run+wait          OK  (done=%d)\n", done.load());
    }

    // ------------------------------------------------------------------
    // 2. run_and_wait (convenience overload)
    // ------------------------------------------------------------------
    {
        std::atomic<int> done{0};
        auto job = crd::jobs::make_job([&done]() { done.fetch_add(1, std::memory_order_relaxed); });
        crd::jobs::run_and_wait(job);
        CRD_ASSERT(done.load() == 1);
        std::printf("[smoke_jobs] 2. run_and_wait       OK\n");
    }

    // ------------------------------------------------------------------
    // 3. parallel_for — sum of range indices
    // ------------------------------------------------------------------
    {
        constexpr crd::u32 kCount = 1000U;
        std::atomic<crd::u64> total{0};
        crd::jobs::Counter* c = crd::jobs::parallel_for(
            kCount, 4U,
            [&total](crd::u32 begin, crd::u32 end)
            {
                crd::u64 local = 0U;
                for (crd::u32 i = begin; i < end; ++i)
                    local += i;
                total.fetch_add(local, std::memory_order_relaxed);
            });
        crd::jobs::wait(c);
        constexpr crd::u64 kExpected = static_cast<crd::u64>(kCount) * (kCount - 1U) / 2U;
        CRD_ASSERT(total.load() == kExpected);
        std::printf("[smoke_jobs] 3. parallel_for       OK  (sum=%llu, expected=%llu)\n",
                    static_cast<unsigned long long>(total.load()),
                    static_cast<unsigned long long>(kExpected));
    }

    // ------------------------------------------------------------------
    // 4. H / N / L priority — all jobs ran
    // ------------------------------------------------------------------
    {
        constexpr int kHigh   = 10;
        constexpr int kNormal = 20;
        constexpr int kLow    = 40;

        std::atomic<int> high_ran{0};
        std::atomic<int> normal_ran{0};
        std::atomic<int> low_ran{0};

        crd::jobs::JobDecl jobs[kHigh + kNormal + kLow];
        for (int i = 0; i < kHigh; ++i)
            jobs[i] = crd::jobs::make_job(
                [&high_ran]() { high_ran.fetch_add(1, std::memory_order_relaxed); },
                crd::jobs::StackSize::Small, crd::jobs::Priority::High);
        for (int i = 0; i < kNormal; ++i)
            jobs[kHigh + i] = crd::jobs::make_job(
                [&normal_ran]() { normal_ran.fetch_add(1, std::memory_order_relaxed); },
                crd::jobs::StackSize::Small, crd::jobs::Priority::Normal);
        for (int i = 0; i < kLow; ++i)
            jobs[kHigh + kNormal + i] = crd::jobs::make_job(
                [&low_ran]() { low_ran.fetch_add(1, std::memory_order_relaxed); },
                crd::jobs::StackSize::Small, crd::jobs::Priority::Low);

        crd::jobs::Counter* c = crd::jobs::run(
            std::span(jobs, static_cast<crd::usize>(kHigh + kNormal + kLow)));
        crd::jobs::wait(c);

        CRD_ASSERT(high_ran.load()   == kHigh);
        CRD_ASSERT(normal_ran.load() == kNormal);
        CRD_ASSERT(low_ran.load()    == kLow);
        std::printf("[smoke_jobs] 4. H/N/L priorities   OK  (High=%d, Normal=%d, Low=%d)\n",
                    high_ran.load(), normal_ran.load(), low_ran.load());
    }

    // ------------------------------------------------------------------
    // 5. frame_alloc + frame_reset
    // ------------------------------------------------------------------
    {
        [[maybe_unused]] void* p1 = crd::jobs::frame_alloc(256U, 8U);
        CRD_ASSERT(p1 != nullptr);
        CRD_ASSERT((reinterpret_cast<crd::usize>(p1) & 7U) == 0U);
        std::memset(p1, 0xAB, 256U);

        [[maybe_unused]] void* p2 = crd::jobs::frame_alloc(64U, 16U);
        CRD_ASSERT(p2 != nullptr);
        CRD_ASSERT((reinterpret_cast<crd::usize>(p2) & 15U) == 0U);

        crd::jobs::frame_reset();

        [[maybe_unused]] void* p3 = crd::jobs::frame_alloc(1024U, 8U);
        CRD_ASSERT(p3 != nullptr);
        std::printf("[smoke_jobs] 5. frame_alloc/reset  OK\n");
    }

    crd::jobs::shutdown();

    std::printf("[smoke_jobs] PASS\n");
    return 0;
}
