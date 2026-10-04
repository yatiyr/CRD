// DIAG.6a(c1): reproduce + fix the frame-history ring's torn-record hazard.
//
// The frame-history ring is overwrite-oldest: frame_mark() writes a ~3.6 KB FrameRecord in place and advances the head
// (no tail), and frame_record() hands back a const FrameRecord* INTO the ring. A reader that dereferences that pointer
// while frame_mark laps the ring reads a record half-written by two different frames -- a torn record. The design names
// this exactly: "head/tail atomics alone do not prove overwritten payload is safe for concurrent readers."
//
// Invariant that exposes a tear: each iteration sets a Set-kind counter to the frame index that this frame_mark will
// stamp, so within ONE record `values[id].bits == frame_index`. A torn read mixes one frame's index with another's
// counter value -> mismatch. A tiny 4-slot ring laps in microseconds, maximising the overwrite-during-read window.
//
// The phase-1 repro (a raw-pointer `frame_record()` reader — 15 torn records on the unfixed profiler) is recorded in
// docs/sessions/2026-09-15-diag-6a-profiler-registry-snapshot-census.md; THIS file is the phase-2 form: it reads via
// the seqlock `copy_frame_record()` API and asserts zero tears. Proof lane: win-debug (tearing is a logic error the
// self-check catches, not a memory error). The worker is joined before shutdown (capture-vs-shutdown is 6a(e)).
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#if CRD_PERF_ENABLED

#include <atomic>
#include <thread>

TEST_CASE("ring: frame-history reads are free of torn records under a wrapping writer", "[perf][diag][ring]")
{
    crd::perf::InitConfig cfg;
    cfg.frame_history_slots = 4U; // tiny -> laps fast, maximises the overwrite-during-read window
    crd::perf::init(cfg);

    const auto id = crd::perf::register_counter_i64("ring.frame", crd::perf::CounterKind::Set);

    constexpr crd::u64    kFrames = 200000U;
    std::atomic<bool>     done{false};
    std::atomic<crd::u64> torn{0U};

    std::atomic<crd::u64> copies{0U};
    std::thread           reader(
        [&]
        {
            crd::perf::FrameRecord rec{};
            while (!done.load(std::memory_order_acquire))
            {
                // Seqlock copy into a local -- the fix. A returned copy is always internally consistent.
                if (crd::perf::copy_frame_record(1U, rec))
                {
                    copies.fetch_add(1U, std::memory_order_relaxed);
                    const crd::u64 fi = rec.frame_index;
                    const crd::u64 v  = rec.values[id.value].bits;
                    if (fi != 0U && v != fi)
                    {
                        torn.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
            }
        });

    for (crd::u64 f = 0; f < kFrames; ++f)
    {
        crd::perf::counter_set_i64(id, static_cast<crd::i64>(crd::perf::frame_count()));
        crd::perf::frame_mark();
    }
    done.store(true, std::memory_order_release);
    reader.join();

    // A consistent (seqlock-verified) copy never mixes two frames' fields.
    CHECK(torn.load() == 0U);
    // The reader actually exercised the copy path (guards against a vacuous pass).
    CHECK(copies.load() > 0U);
    // Every read is either a consistent copy or an explicit unavailable -- the two must account for a torn-free run.
    INFO("unavailable=" << crd::perf::frame_history_unavailable_count());

    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
