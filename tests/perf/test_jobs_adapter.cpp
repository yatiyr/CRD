// crd-perf v0c -- crd-jobs JobObserver adapter: every job becomes a
//                  Category::Job Sample, with begin_thread / end_thread
//                  recorded so fiber migration is visible.

#include <crd/jobs/jobs.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

#if CRD_PERF_ENABLED

namespace
{

struct PerfJobsFixture
{
    PerfJobsFixture()
    {
        crd::perf::init({});
        crd::perf::install_jobs_adapter();
        crd::jobs::init({});
    }
    ~PerfJobsFixture()
    {
        crd::jobs::shutdown();
        crd::perf::uninstall_jobs_adapter();
        crd::perf::shutdown();
    }
};

// Count how many Category::Job Samples are present across every registered
// profiler thread.
[[nodiscard]] crd::u32 count_job_samples()
{
    crd::u32 total = 0U;
    for (crd::u32 t = 0U; t < crd::perf::thread_count(); ++t)
    {
        const auto view = crd::perf::thread_samples(static_cast<crd::u8>(t));
        for (crd::u32 i = 0U; i < view.size; ++i)
        {
            if (view.data[i].category == static_cast<crd::u8>(crd::perf::Category::Job))
            {
                ++total;
            }
        }
    }
    return total;
}

} // namespace

TEST_CASE("install_jobs_adapter is idempotent", "[perf][jobs][adapter]")
{
    crd::perf::init({});
    CHECK_FALSE(crd::perf::jobs_adapter_installed());
    crd::perf::install_jobs_adapter();
    CHECK(crd::perf::jobs_adapter_installed());
    crd::perf::install_jobs_adapter(); // second call: no-op
    CHECK(crd::perf::jobs_adapter_installed());
    crd::perf::uninstall_jobs_adapter();
    CHECK_FALSE(crd::perf::jobs_adapter_installed());
    crd::perf::shutdown();
}

TEST_CASE("single job produces one Category::Job sample", "[perf][jobs][adapter]")
{
    PerfJobsFixture fx;

    std::atomic<bool> ran{false};
    auto job = crd::jobs::make_job([&]() { ran.store(true); });
    crd::jobs::run_and_wait(job);
    CHECK(ran.load());

    const auto stats = crd::perf::jobs_adapter_stats();
    CHECK(stats.jobs_begun >= 1U);
    CHECK(stats.jobs_ended >= 1U);
    CHECK(stats.jobs_begun == stats.jobs_ended);
    CHECK(stats.missing_tokens == 0U);

    CHECK(count_job_samples() >= 1U);
}

TEST_CASE("parallel_for captures one sample per job",
          "[perf][jobs][adapter][parallel]")
{
    PerfJobsFixture fx;

    constexpr crd::u32 kCount    = 64U;
    constexpr crd::u32 kNumJobs  = 8U;
    std::atomic<crd::u32> sum{0U};

    auto* c = crd::jobs::parallel_for(kCount, kNumJobs,
                                      [&](crd::u32 b, crd::u32 e) {
                                          for (crd::u32 i = b; i < e; ++i)
                                              sum.fetch_add(i, std::memory_order_relaxed);
                                      });
    crd::jobs::wait(c);

    // Verify the computation actually ran.
    crd::u32 expected = 0U;
    for (crd::u32 i = 0U; i < kCount; ++i)
    {
        expected += i;
    }
    CHECK(sum.load() == expected);

    const auto stats = crd::perf::jobs_adapter_stats();
    CHECK(stats.jobs_begun == kNumJobs);
    CHECK(stats.jobs_ended == kNumJobs);
    CHECK(stats.missing_tokens == 0U);
    CHECK(count_job_samples() == kNumJobs);
}

TEST_CASE("job samples carry the fiber_id for cross-thread reconstruction",
          "[perf][jobs][adapter][fiber]")
{
    PerfJobsFixture fx;

    constexpr crd::u32 kJobs = 16U;
    std::atomic<crd::u32> done{0U};
    auto* c = crd::jobs::parallel_for(kJobs, kJobs,
                                      [&](crd::u32 /*b*/, crd::u32 /*e*/) {
                                          done.fetch_add(1U, std::memory_order_relaxed);
                                      });
    crd::jobs::wait(c);
    CHECK(done.load() == kJobs);

    crd::u32 with_fiber = 0U;
    for (crd::u32 t = 0U; t < crd::perf::thread_count(); ++t)
    {
        const auto view = crd::perf::thread_samples(static_cast<crd::u8>(t));
        for (crd::u32 i = 0U; i < view.size; ++i)
        {
            if (view.data[i].category == static_cast<crd::u8>(crd::perf::Category::Job) &&
                view.data[i].fiber_id != 0U)
            {
                ++with_fiber;
            }
        }
    }
    CHECK(with_fiber == kJobs); // every job sample tagged with a fiber id
}

TEST_CASE("nested CRD_PERF_SCOPE inside a job becomes a child region",
          "[perf][jobs][adapter][nesting]")
{
    PerfJobsFixture fx;

    auto job = crd::jobs::make_job([]() {
        CRD_PERF_SCOPE("inner_work");
        // measurable body
        volatile int x = 0;
        for (int i = 0; i < 100; ++i)
            x += i;
        (void)x;
    });
    crd::jobs::run_and_wait(job);

    bool found_inner = false;
    bool found_job   = false;
    for (crd::u32 t = 0U; t < crd::perf::thread_count(); ++t)
    {
        const auto view = crd::perf::thread_samples(static_cast<crd::u8>(t));
        for (crd::u32 i = 0U; i < view.size; ++i)
        {
            const auto& s    = view.data[i];
            const char* name = crd::perf::resolve_name(crd::perf::NameId{s.name_id});
            if (s.category == static_cast<crd::u8>(crd::perf::Category::Job))
            {
                found_job = true;
            }
            if (std::string_view{name} == "inner_work")
            {
                found_inner = true;
                CHECK(s.depth >= 1U); // nested under the job region
            }
        }
    }
    CHECK(found_job);
    CHECK(found_inner);
}

TEST_CASE("adapter is a no-op when never installed",
          "[perf][jobs][adapter][off]")
{
    crd::perf::init({});
    crd::jobs::init({});
    auto job = crd::jobs::make_job([]() {});
    crd::jobs::run_and_wait(job);
    crd::jobs::shutdown();

    const auto stats = crd::perf::jobs_adapter_stats();
    CHECK(stats.jobs_begun == 0U); // no instrumentation
    CHECK(count_job_samples() == 0U);
    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
