// DIAG.6a(c3): ENFORCED SINGLE CONSUMER on the per-thread sample ring. copy_thread_samples admits exactly one consumer
// (a copier OR clear_samples) per ring at a time: a second concurrent copier is REFUSED -- it returns 0, sets
// *out_contended, and bumps the exact `contended` counter -- rather than racing the reader window, and clear_samples
// takes the same flag so it can never move `tail` out from under a copier. Three proofs plus a capture guard:
//   (1) contention is EXACT -- the number of refused returns equals sample_copy_contended_count();
//   (2) TEST POWER -- under heavy concurrent load contention actually occurs (else the exactness proof is vacuous);
//   (3) SERIALIZATION -- a copy never observes a batch straddling a clear (would tear without the flag);
//   (4) an uncontended single-threaded save reports zero abandoned threads (the capture retry path is not spurious).
// Assertions live only on the main thread after join() -- Catch2's macros are not thread-safe; workers accumulate into
// atomics. Proof lanes: win-debug + win-asan (ASan slows the copy loop, making contention MORE likely, not less).
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
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

TEST_CASE("sample ring: reader contention is exact and actually occurs under load",
          "[perf][diag][sample-ring][readers]")
{
    PerfFixture   fx;
    const crd::u8 idx = crd::perf::current_thread_index();

    // Fill the ring so every SUCCESSFUL copy returns exactly fill_count samples (distinguishes success from a genuinely
    // empty ring) and each copy is a long (fill_count-element) loop -- the window two copiers must overlap in.
    constexpr crd::u32 fill_count = 1024U;
    for (crd::u32 i = 0U; i < fill_count; ++i)
    {
        CRD_PERF_SCOPE("readers.fill");
    }

    constexpr crd::u32    n_threads = 8U;
    constexpr crd::u32    iters   = 1000U;
    std::atomic<bool>     go{false};
    std::atomic<crd::u64> refused{0U};       // copies that came back contended
    std::atomic<crd::u64> succeeded{0U};     // copies that returned the live batch
    std::atomic<crd::u64> bad_success{0U};   // a non-contended copy that did NOT return exactly fill_count (must stay 0)
    std::atomic<crd::u64> bad_contended{0U}; // a contended copy that returned != 0 (must stay 0)

    crd::containers::Array<std::thread> ts;
    ts.reserve(n_threads);
    for (crd::u32 w = 0U; w < n_threads; ++w)
    {
        ts.push_back(std::thread(
            [&]()
            {
                crd::perf::Sample buf[fill_count];
                while (!go.load(std::memory_order_acquire))
                {
                    // spin to a common start so the copies actually overlap
                }
                for (crd::u32 i = 0U; i < iters; ++i)
                {
                    bool           contended = false;
                    const crd::u32 n = crd::perf::copy_thread_samples(idx, buf, fill_count, &contended);
                    if (contended)
                    {
                        refused.fetch_add(1U, std::memory_order_relaxed);
                        if (n != 0U)
                        {
                            bad_contended.fetch_add(1U, std::memory_order_relaxed);
                        }
                    }
                    else
                    {
                        succeeded.fetch_add(1U, std::memory_order_relaxed);
                        if (n != fill_count)
                        {
                            bad_success.fetch_add(1U, std::memory_order_relaxed);
                        }
                    }
                }
            }));
    }
    go.store(true, std::memory_order_release);
    for (auto& t : ts)
    {
        t.join();
    }

    // (1) Exactness: every refused return bumped the per-ring counter exactly once, and nothing else did.
    CHECK(refused.load() == crd::perf::sample_copy_contended_count(idx));
    // Full accounting: every attempt either refused or succeeded.
    CHECK(refused.load() + succeeded.load() == static_cast<crd::u64>(n_threads) * iters);
    // A refused copy always returns 0; a successful copy always returns the whole fixed batch (tail/head are fixed --
    // no producer runs while the workers copy). If either ever fails, the refusal did not fully protect the copy.
    CHECK(bad_contended.load() == 0U);
    CHECK(bad_success.load() == 0U);
    // (2) Test power: with 8 threads x 1000 copies of a 1024-element window, at least one collision is effectively
    // certain (more so under ASan). A zero here is a TEST-POWER failure -- raise the load, never drop this REQUIRE.
    INFO("refused=" << refused.load() << " succeeded=" << succeeded.load()
                    << " counter=" << crd::perf::sample_copy_contended_count(idx));
    REQUIRE(crd::perf::sample_copy_contended_count(idx) >= 1U);
}

TEST_CASE("sample ring: a copy never straddles a clear (clear/copy serialization)",
          "[perf][diag][sample-ring][readers]")
{
    PerfFixture   fx;
    const crd::u8 idx = crd::perf::current_thread_index();

    std::atomic<bool>     go{false};
    std::atomic<bool>     stop{false};
    std::atomic<crd::u64> mixed{0U};        // copies containing BOTH names -- the tearing; must stay 0
    std::atomic<crd::u64> samples_seen{0U}; // total samples any copier observed (guards a vacuous pass)

    crd::containers::Array<std::thread> ts;
    ts.reserve(4U);
    for (crd::u32 w = 0U; w < 4U; ++w)
    {
        ts.push_back(std::thread(
            [&]()
            {
                crd::perf::Sample buf[256];
                while (!go.load(std::memory_order_acquire))
                {
                }
                while (!stop.load(std::memory_order_acquire))
                {
                    const crd::u32 n     = crd::perf::copy_thread_samples(idx, buf, 256U);
                    bool           saw_a = false;
                    bool           saw_b = false;
                    for (crd::u32 k = 0U; k < n; ++k)
                    {
                        const char* nm = crd::perf::resolve_name(crd::perf::NameId{buf[k].name_id});
                        if (std::strcmp(nm, "serial.a") == 0)
                        {
                            saw_a = true;
                        }
                        else if (std::strcmp(nm, "serial.b") == 0)
                        {
                            saw_b = true;
                        }
                    }
                    if (saw_a && saw_b)
                    {
                        mixed.fetch_add(1U, std::memory_order_relaxed);
                    }
                    if (n != 0U)
                    {
                        samples_seen.fetch_add(n, std::memory_order_relaxed);
                    }
                }
            }));
    }

    // Producer + clearer, both on this thread: alternate homogeneous batches separated by a clear. With the flag, a
    // copier's [tail, head) snapshot can never span the tail move, so no copy sees both names; without it, a torn tail
    // read would let a copy straddle the boundary and mix them.
    go.store(true, std::memory_order_release);
    for (crd::u32 round = 0U; round < 3000U; ++round)
    {
        for (int i = 0; i < 8; ++i)
        {
            CRD_PERF_SCOPE("serial.a");
        }
        crd::perf::clear_samples();
        for (int i = 0; i < 8; ++i)
        {
            CRD_PERF_SCOPE("serial.b");
        }
        crd::perf::clear_samples();
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : ts)
    {
        t.join();
    }

    CHECK(mixed.load() == 0U);          // (3) no copy ever straddled a clear
    CHECK(samples_seen.load() > 0U);    // the copiers actually observed live batches (non-vacuous)
}

TEST_CASE("sample ring: an uncontended save reports zero abandoned threads", "[perf][diag][sample-ring][readers]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "c3-capture"};
    PerfFixture                        fx;

    for (int i = 0; i < 10; ++i)
    {
        CRD_PERF_SCOPE("cap.sample");
    }
    const crd::u64 before = crd::perf::capture_contended_thread_count();
    const auto     buf    = crd::perf::save_capture_to_buffer(&alloc);
    REQUIRE(buf.size() > 0U);
    // (4) A single-threaded save has no competing consumer, so the retry/give-up path must never fire.
    CHECK(crd::perf::capture_contended_thread_count() == before);
}

TEST_CASE("sample ring: a save's copy retry outlasts a competing consumer (no silently dropped thread)",
          "[perf][diag][sample-ring][readers]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "c3-retry"};
    PerfFixture                        fx;
    const crd::u8                      idx = crd::perf::current_thread_index();

    constexpr crd::u32 fill_count = 1024U;
    for (crd::u32 i = 0U; i < fill_count; ++i)
    {
        CRD_PERF_SCOPE("cap.retry");
    }

    // Sustained hammers hold the ring back-to-back (each copy is a full fill_count window), keeping it busy ~continuously so
    // a save's per-thread copy reliably lands mid-window. Only a retry that OUTLASTS a copy window (time-bounded, not a
    // fixed spin count) still captures the batch; a too-short spin gives up inside the first window and drops the whole
    // thread to 0. A start-barrier guarantees the hammers are live before any save runs.
    constexpr crd::u32                  n_hammers = 2U;
    std::atomic<bool>                   stop{false};
    std::atomic<crd::u32>               running{0U};
    crd::containers::Array<std::thread> hammers;
    hammers.reserve(n_hammers);
    for (crd::u32 w = 0U; w < n_hammers; ++w)
    {
        hammers.push_back(std::thread(
            [&]()
            {
                crd::perf::Sample buf[fill_count];
                running.fetch_add(1U, std::memory_order_release);
                while (!stop.load(std::memory_order_acquire))
                {
                    (void)crd::perf::copy_thread_samples(idx, buf, fill_count);
                }
            }));
    }
    while (running.load(std::memory_order_acquire) < n_hammers)
    {
        // wait until every hammer is actively contending for the ring
    }

    const crd::u64 cap_before        = crd::perf::capture_contended_thread_count();
    bool           all_batches_intact = true;
    for (crd::u32 s = 0U; s < 4U; ++s)
    {
        const auto cap_buf = crd::perf::save_capture_to_buffer(&alloc);
        REQUIRE(cap_buf.size() > 0U);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{cap_buf.data(), cap_buf.size()}};
        REQUIRE(view.is_valid());
        // The only recorded samples are the fill_count "cap.retry" on this thread (hammers are unregistered). Retry outlasts
        // the window -> all present; retry gives up -> the thread is written as 0 and the sum collapses.
        crd::u32 total = 0U;
        for (crd::u32 t = 0U; t < view.thread_count(); ++t)
        {
            total += view.thread_sample_count(t);
        }
        if (total != fill_count)
        {
            all_batches_intact = false;
        }
    }
    const crd::u64 collisions = crd::perf::sample_copy_contended_count(idx);
    stop.store(true, std::memory_order_release);
    for (auto& h : hammers)
    {
        h.join();
    }

    CHECK(all_batches_intact);                                        // every save preserved the batch despite contention
    CHECK(crd::perf::capture_contended_thread_count() == cap_before); // no thread was ever abandoned to contention
    INFO("ring collisions during the run: " << collisions);
    CHECK(collisions > 0U);                                           // the ring really was contended (non-vacuous)
}

TEST_CASE("sample ring: per_thread_ring_capacity reflects config and inactivity (DIAG.6a(c3-ui))",
          "[perf][diag][sample-ring][readers]")
{
    // A cross-thread reader (perf-ui's LiveProfilerSource) sizes its copy buffer to this; it must be 0 when inactive
    // and reflect InitConfig, not a hardcoded default.
    CHECK(crd::perf::per_thread_ring_capacity() == 0U);
    crd::perf::init({});
    CHECK(crd::perf::per_thread_ring_capacity() == crd::perf::kPerThreadRingSlots);
    crd::perf::shutdown();
    CHECK(crd::perf::per_thread_ring_capacity() == 0U);

    crd::perf::InitConfig cfg{};
    cfg.per_thread_ring_slots = 1024U;
    crd::perf::init(cfg);
    CHECK(crd::perf::per_thread_ring_capacity() == 1024U);
    crd::perf::shutdown();
    CHECK(crd::perf::per_thread_ring_capacity() == 0U);
}

#endif // CRD_PERF_ENABLED
