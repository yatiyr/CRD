// crd-perf v0a -- begin_thread vs end_thread fiber-migration capture.
//
// v0a does not actually run a fiber yield (that's v0c's JobObserver), but
// we can still verify the wire format records both thread ids correctly
// by simulating a migration via push_region on thread A + pop_region on
// thread B with a hand-built BeginToken.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("non-migrated scope records begin_thread == end_thread", "[perf][fiber]")
{
    PerfFixture fx;
    const auto id = crd::perf::intern_name("non_migrated");
    {
        const auto tok = crd::perf::push_region(id);
        crd::perf::pop_region(id, tok);
    }
    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    REQUIRE(view.size == 1U);
    CHECK(view.data[0].begin_thread == view.data[0].end_thread);
}

TEST_CASE("BeginToken survives across-thread pop (manual migration)", "[perf][fiber][migration]")
{
    PerfFixture fx;
    const auto main_idx = crd::perf::current_thread_index();
    const auto id       = crd::perf::intern_name("migrated_scope");

    // Push on the main thread; capture the token.
    crd::perf::BeginToken tok = crd::perf::push_region(id);
    CHECK(tok.begin_thread == main_idx);

    // Drop the begin onto a worker thread which "completes" the scope.
    // The worker writes the Sample into its OWN ring (push_region was a
    // begin-only on main; the engine's ScopedRegion always pops on the
    // thread that destroyed the object). The Sample's begin_thread thus
    // says "main"; end_thread says "worker_b". This is the wire-format
    // contract that v0c's JobObserver will leverage once fibers actually
    // migrate.
    std::atomic<bool> done{false};
    std::thread t([&]() {
        const auto worker_idx = crd::perf::register_thread("worker_b");
        CHECK(worker_idx != main_idx);
        crd::perf::pop_region(id, tok);
        done = true;
    });
    t.join();
    CHECK(done.load());

    // The Sample landed on the worker's ring (it was the popping thread).
    // Find it by scanning every registered thread; v0a tests don't yet
    // have a "find worker by name" helper.
    bool found = false;
    for (crd::u32 i = 0U; i < crd::perf::thread_count(); ++i)
    {
        const auto view = crd::perf::thread_samples(static_cast<crd::u8>(i));
        for (crd::u32 k = 0U; k < view.size; ++k)
        {
            const auto& s = view.data[k];
            if (s.name_id == id.value)
            {
                CHECK(s.begin_thread == main_idx);
                CHECK(s.end_thread != main_idx);
                CHECK(s.end_thread == static_cast<crd::u8>(i));
                found = true;
            }
        }
    }
    CHECK(found);
}

#endif // CRD_PERF_ENABLED
