// crd-perf v0b -- counters are safe to write concurrently from any thread.

#include <crd/perf/perf.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("Add-kind i64 counter is atomic under concurrent writers",
          "[perf][counter][threaded][atomic]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("threaded.add", crd::perf::CounterKind::Add);

    constexpr crd::u32 kThreads     = 4U;
    constexpr crd::u32 kPerThread   = 25000U;
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (crd::u32 t = 0U; t < kThreads; ++t)
    {
        ts.emplace_back([&]() {
            crd::perf::register_thread("adder");
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (crd::u32 i = 0U; i < kPerThread; ++i)
            {
                crd::perf::counter_add_i64(id, 1);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : ts)
    {
        t.join();
    }
    CHECK(crd::perf::counter_current_i64(id) == static_cast<crd::i64>(kThreads * kPerThread));
}

TEST_CASE("Add-kind f64 counter is consistent under concurrent CAS-loop writers",
          "[perf][counter][threaded][f64]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_f64("threaded.add.f64", crd::perf::CounterKind::Add);

    constexpr crd::u32 kThreads   = 4U;
    constexpr crd::u32 kPerThread = 5000U;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (crd::u32 t = 0U; t < kThreads; ++t)
    {
        ts.emplace_back([&]() {
            crd::perf::register_thread("f64adder");
            for (crd::u32 i = 0U; i < kPerThread; ++i)
            {
                crd::perf::counter_add_f64(id, 0.001);
            }
        });
    }
    for (auto& t : ts)
    {
        t.join();
    }
    // 4 * 5000 * 0.001 = 20.0; allow f64 rounding margin.
    CHECK(crd::perf::counter_current_f64(id) ==
          Catch::Approx(static_cast<crd::f64>(kThreads * kPerThread) * 0.001).margin(1e-6));
}

#endif // CRD_PERF_ENABLED
