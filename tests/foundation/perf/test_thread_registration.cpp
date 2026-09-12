// crd-perf v0a -- thread registration semantics.

#include <crd/perf/profiler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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
