// crd-perf v0a -- thread registration semantics.

#include <crd/perf/profiler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

#if CRD_PERF_ENABLED

TEST_CASE("init auto-registers the main thread", "[perf][thread]")
{
    PerfFixture fx;
    CHECK(crd::perf::thread_count() >= 1U);
    CHECK(crd::perf::current_thread_index() != 0xFFU);
}

TEST_CASE("register_thread is idempotent on the calling thread", "[perf][thread]")
{
    PerfFixture fx;
    const auto a = crd::perf::register_thread("main");
    const auto b = crd::perf::register_thread("main");
    CHECK(a == b);
}

TEST_CASE("re-registering takes a new name and never reuses another lifetime's index", "[perf][thread]")
{
    // register_thread returns a live cache without the reader pin (DIAG.11b); the cache must be of this lifetime and
    // carry the same name.
    crd::perf::init({});
    std::atomic<int> step{0};
    std::atomic<crd::u8> stale_again{0xFFU};
    std::thread stale(
        [&]()
        {
            (void)crd::perf::register_thread("worker.same");
            step.store(1, std::memory_order_release);
            while (step.load(std::memory_order_acquire) != 2)
            {
                std::this_thread::yield();
            }
            stale_again.store(crd::perf::register_thread("worker.same"), std::memory_order_release);
        });
    while (step.load(std::memory_order_acquire) != 1)
    {
        std::this_thread::yield();
    }
    crd::perf::shutdown(); // `stale` keeps its cache of the retired lifetime
    crd::perf::init({});
    std::atomic<crd::u8> fresh{0xFFU};
    std::thread([&]() { fresh.store(crd::perf::register_thread("worker.same"), std::memory_order_release); }).join();
    step.store(2, std::memory_order_release);
    stale.join();
    CHECK(fresh.load() != 0xFFU);
    CHECK(stale_again.load() != 0xFFU);
    CHECK(stale_again.load() != fresh.load()); // the stale thread did not take the ring another thread records into
    CHECK(crd::perf::thread_count() == 3U);

    const crd::u8 main_idx = crd::perf::current_thread_index();
    CHECK(crd::perf::register_thread("main.renamed") == main_idx);
    const auto view = crd::perf::thread_samples(main_idx);
    CHECK(std::strcmp(view.name != nullptr ? view.name : "", "main.renamed") == 0);
    crd::perf::shutdown();
}

TEST_CASE("worker threads get unique indices", "[perf][thread][workers]")
{
    PerfFixture fx;
    const auto main_idx = crd::perf::current_thread_index();
    std::atomic<crd::u8> worker_idx{0xFFU};
    std::thread t([&]() {
        worker_idx = crd::perf::register_thread("worker_a");
    });
    t.join();
    const crd::u8 worker_index_val = worker_idx.load();
    CHECK(worker_index_val != 0xFFU);
    CHECK(worker_index_val != main_idx);
    CHECK(crd::perf::thread_count() >= 2U);
}

TEST_CASE("set_current_fiber_id is reflected in subsequent samples", "[perf][thread][fiber]")
{
    PerfFixture fx;
    crd::perf::set_current_fiber_id(0x12345678U);
    CHECK(crd::perf::current_fiber_id() == 0x12345678U);
    {
        const auto id = crd::perf::intern_name("fibered");
        const auto tok = crd::perf::push_region(id);
        crd::perf::pop_region(id, tok);
    }
    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    REQUIRE(view.size == 1U);
    CHECK(view.data[0].fiber_id == 0x12345678U);
    crd::perf::set_current_fiber_id(0U);
}

#endif // CRD_PERF_ENABLED
