// DIAG.6a(b): reproduce + fix the DG08 allocator-registry use-after-free.
//
// A worker registers a stack TlsfAllocator, unregisters it, then lets it destruct at scope end -- the realistic owner
// contract: an allocator is destroyed AFTER unregister_allocator returns. It does this in a tight loop while the main
// thread drives frame_mark(), whose per-frame snapshot reads every registered allocator's stats(). On the unfixed
// profiler the snapshot loads a slot's raw pointer, then (racing unregister) the slot is nulled and the allocator is
// destroyed, so the snapshot calls stats() on a freed object -> ASan heap-use-after-free at the snapshot deref. The fix
// (atomic slot publication + unregister quiescence) makes unregister wait for any in-flight snapshot, so an allocator
// is never destroyed under a live read. Capture-vs-shutdown is a separate concern (6a(e)): the worker is joined before
// shutdown here. Proof lane: win-asan. See docs/sessions/2026-09-15-diag-6a-profiler-registry-snapshot-census.md.
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#if CRD_PERF_ENABLED

#include <atomic>
#include <thread>

TEST_CASE("registry: snapshot vs unregister+destroy is free of use-after-free", "[perf][diag][registry]")
{
    crd::perf::init({});

    constexpr int     kCycles = 2000;
    std::atomic<bool> done{false};

    std::thread worker(
        [&]
        {
            for (int i = 0; i < kCycles; ++i)
            {
                crd::memory::TlsfAllocator a{1U << 16, nullptr, "stress"};
                const crd::u32             idx = crd::perf::register_allocator("stress", &a);
                crd::perf::unregister_allocator(idx);
                // `a` destructs here -- AFTER unregister_allocator returned (the owner contract).
            }
            done.store(true, std::memory_order_release);
        });

    while (!done.load(std::memory_order_acquire))
        crd::perf::frame_mark(); // the per-frame allocator snapshot reads every live slot's stats()

    worker.join();

    // Slot reuse: 2000 register/unregister cycles must reuse one slot, not saturate the high-water.
    CHECK(crd::perf::registered_allocator_count() <= 2U);
    // Live count returns to zero once every registration has been paired with an unregister.
    CHECK(crd::perf::live_allocator_count() == 0U);

    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
