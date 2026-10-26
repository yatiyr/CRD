// DIAG.11b (q1) -- the flight recorder's per-thread rings under one global sequence (crd/perf/flight_recorder.hpp).
//
// Every kept event takes the next sequence of one recording word and goes into its thread's own ring; a capture
// places each ring's events of the window at their sequence's position. These cases hold what that must keep from the
// single global ring it replaced (ADR-0133's order guarantee, unchanged):
//
// - one total order that agrees with happens-before: events passed from thread to thread by release/acquire appear in
//   causal order, contiguous, whichever rings they came from;
// - an exact trigger window in claim order: an event recorded before the trigger in the trigger's own thread is in the
//   pre window, one after it in the post window, the post window holds exactly post_events, and an event recorded
//   after the window closed is never in a capture (it is counted, and its sequence is given back);
// - the rings' lifetime and storage: a thread that starts late takes a free ring, one that exits gives its ring back
//   and its events stay, a thread whose ring has no storage under the policy (or that finds every ring taken) records
//   into the shared ring, and the capture is complete in every case;
// - a policy reload while threads record, and a refused ring allocation (the test-only seam).
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b, DIAG.11b); ADR-0133.

#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/containers/string_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
using crd::u32;
using crd::u64;

constexpr cp::EventCode kHopCode    = 0x0071U;
constexpr cp::EventCode kNoiseCode  = 0x0072U;
constexpr cp::EventCode kMarkerCode = 0x0073U;

cp::FlightPolicy policy(u32 capacity, u32 pre, u32 post)
{
    cp::FlightPolicy p;
    p.capacity       = capacity;
    p.pre_events     = pre;
    p.post_events    = post;
    p.post_window_ns = 0U; // bounded by events only
    return p;
}

cp::FlightTriggerInput manual(const char* message)
{
    cp::FlightTriggerInput in;
    in.kind    = cp::FlightTrigger::Manual;
    in.code    = cp::kFlightCodeManual;
    in.message = cont::StringView{message};
    return in;
}

// Pre events strictly before the trigger, post events at or after it, contiguous ascending sequences from the trigger's
// sequence less pre_retained, and counts inside the policy bounds.
bool consistent(const cp::FlightCapture& c)
{
    if (c.events.size() != static_cast<crd::usize>(c.pre_retained) + c.post_retained ||
        c.pre_retained > c.policy.pre_events || c.post_retained > c.policy.post_events ||
        c.recorded_before_trigger != c.trigger.sequence)
    {
        return false;
    }
    for (crd::usize i = 0U; i < c.events.size(); ++i)
    {
        if (c.events[i].sequence != c.trigger.sequence - c.pre_retained + i)
        {
            return false;
        }
    }
    return true;
}

// Spin (yielding) until `done()` or 30 s; true when done.
template <typename F> bool wait_until(F done)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > until)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// The capture's take, retried until the post window filled (bounded).
bool take(cp::FlightRecorder& r, cp::FlightCapture& c)
{
    return wait_until([&] { return r.take_capture(c); });
}
} // namespace

TEST_CASE("flight rings: the merged capture is one global order that agrees with happens-before",
          "[perf][diag][diag11b][flight][concurrency]")
{
    // A token passes thread to thread: thread (h % k_threads) records hop h after acquiring the token at h and then
    // releases it at h + 1, so hop h happens before hop h + 1 and every consecutive pair is on two threads' rings.
    // Two more threads record freely, interleaving their own rings' sequences.
    constexpr u32 k_threads = 4U;
    constexpr u32 k_hops    = 4000U;
    cp::FlightRecorder r;
    REQUIRE(r.init(policy(4096U, 1024U, 256U)) == cp::FlightPolicyStatus::Committed);

    std::atomic<u32>  token{0U};
    std::atomic<bool> stop{false};
    std::atomic<bool> stalled{false};
    std::thread       hoppers[k_threads];
    for (u32 t = 0U; t < k_threads; ++t)
    {
        hoppers[t] = std::thread(
            [&r, &token, &stalled, t]
            {
                for (u32 h = t; h < k_hops; h += k_threads)
                {
                    if (!wait_until([&] { return token.load(std::memory_order_acquire) == h; }))
                    {
                        stalled.store(true);
                        return;
                    }
                    r.record(kHopCode, cp::Severity::Info, cont::StringView{"hop"}, h, t);
                    token.store(h + 1U, std::memory_order_release);
                }
            });
    }
    std::thread noise[2];
    for (u32 n = 0U; n < 2U; ++n)
    {
        noise[n] = std::thread(
            [&r, &stop, &token, n]
            {
                // At most one event a hop, so the window holds hops whatever the threads' speeds.
                u64 i    = 0U;
                u32 seen = ~0U;
                while (!stop.load(std::memory_order_acquire))
                {
                    const u32 now = token.load(std::memory_order_acquire);
                    if (now != seen)
                    {
                        r.record(kNoiseCode, cp::Severity::Info, cont::StringView{"noise"}, i++, n);
                        seen = now;
                    }
                    std::this_thread::yield();
                }
            });
    }
    REQUIRE(wait_until([&] { return token.load(std::memory_order_acquire) >= k_hops / 2U; }));
    REQUIRE(r.trigger(manual("half way")) == cp::FlightTriggerResult::Started);
    for (auto& t : hoppers)
    {
        t.join();
    }
    cp::FlightCapture c;
    const bool        taken = take(r, c);
    stop.store(true, std::memory_order_release);
    for (auto& t : noise)
    {
        t.join();
    }
    REQUIRE_FALSE(stalled.load());
    REQUIRE(taken);

    CHECK(consistent(c));
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK(c.pre_retained == 1024U);
    CHECK(c.post_retained == 256U);

    // The hops in the capture are consecutive and ascending in sequence order, and came from every hopping thread.
    u64  hops = 0U;
    u64  last = 0U;
    bool ordered = true;
    u32  threads_seen = 0U;
    for (const cp::FlightEvent& e : c.events)
    {
        if (e.code != kHopCode)
        {
            continue;
        }
        if (hops > 0U && e.value_a != last + 1U)
        {
            ordered = false;
        }
        last = e.value_a;
        threads_seen |= 1U << static_cast<u32>(e.value_b);
        ++hops;
    }
    CHECK(ordered);
    CHECK(hops >= (1024U + 256U) / 3U); // two noise events a hop at most
    CHECK(threads_seen == (1U << k_threads) - 1U);
}

TEST_CASE("flight rings: the trigger boundary is exact in the trigger's own thread while other threads record",
          "[perf][diag][diag11b][flight][concurrency]")
{
    // Windows large enough for every event, closed by its time bound once the workers stopped, so every event recorded
    // is in the capture: before the trigger in claim order is pre, after it post. 16384 events a ring leaves storage
    // for four rings, so one of the five recording threads records into the shared ring.
    constexpr u32 k_workers = 4U;
    constexpr u64 k_each    = 1000U;
    cp::FlightRecorder r;
    cp::FlightPolicy   p = policy(16384U, 8192U, 8192U);
    p.post_window_ns     = 1000000U; // 1 ms
    REQUIRE(r.init(p) == cp::FlightPolicyStatus::Committed);
    REQUIRE(r.ring_stats().thread_rings == 4U);
    r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"main first"}, 0U, 99U);

    std::thread workers[k_workers];
    for (u32 w = 0U; w < k_workers; ++w)
    {
        workers[w] = std::thread(
            [&r, w]
            {
                for (u64 i = 0U; i < k_each; ++i)
                {
                    r.record(kNoiseCode, cp::Severity::Info, cont::StringView{"worker"}, i, w);
                }
            });
    }
    REQUIRE(wait_until([&] { return r.total_recorded() > 1500U; }));
    r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"before trigger"}, 1U, 99U);
    REQUIRE(r.trigger(manual("exact")) == cp::FlightTriggerResult::Started);
    r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"after trigger"}, 2U, 99U);
    for (auto& t : workers)
    {
        t.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK(r.poll()); // no sink: the window closed by its time bound, waiting for take_capture()
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));

    CHECK(consistent(c));
    CHECK(c.end == cp::FlightEnd::PostWindowElapsed);
    REQUIRE(c.events.size() == k_workers * k_each + 3U); // every event, from four threads' rings and the shared ring
    CHECK(c.pre_retained == c.trigger.sequence);
    CHECK(r.ring_stats().shared_events > 0U);
    crd::usize before = c.events.size();
    crd::usize after  = c.events.size();
    for (crd::usize i = 0U; i < c.events.size(); ++i)
    {
        if (c.events[i].code == kMarkerCode && c.events[i].value_a == 1U)
        {
            before = i;
        }
        if (c.events[i].code == kMarkerCode && c.events[i].value_a == 2U)
        {
            after = i;
        }
    }
    CHECK(before < c.pre_retained);
    CHECK(after >= c.pre_retained);
    CHECK(after < c.events.size());
    // Each worker's events all, once each, in its program order.
    for (u32 w = 0U; w < k_workers; ++w)
    {
        u64 next = 0U;
        for (const cp::FlightEvent& e : c.events)
        {
            if (e.code == kNoiseCode && e.value_b == w && e.value_a == next)
            {
                ++next;
            }
        }
        CHECK(next == k_each);
    }
}

TEST_CASE("flight rings: the post window holds exactly post_events and void claims are given back",
          "[perf][diag][diag11b][flight][concurrency]")
{
    // Main records first, so it holds the lowest ring and the workers' rings are gathered after it.
    constexpr u32 k_workers = 4U;
    constexpr u64 k_each    = 20000U;
    cp::FlightRecorder r;
    REQUIRE(r.init(policy(256U, 64U, 32U)) == cp::FlightPolicyStatus::Committed);
    r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"main first"}, 0U, 99U);

    std::thread workers[k_workers];
    for (u32 w = 0U; w < k_workers; ++w)
    {
        workers[w] = std::thread(
            [&r, w]
            {
                for (u64 i = 0U; i < k_each; ++i)
                {
                    r.record(kNoiseCode, cp::Severity::Info, cont::StringView{"worker"}, i, w);
                }
            });
    }
    REQUIRE(wait_until([&] { return r.total_recorded() > 1000U; }));
    REQUIRE(r.trigger(manual("exact")) == cp::FlightTriggerResult::Started);
    for (auto& t : workers)
    {
        t.join();
    }
    const u64         frozen = r.total_recorded();
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));

    CHECK(consistent(c));
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK(c.pre_retained == 64U);
    CHECK(c.post_retained == 32U);
    CHECK(frozen == c.trigger.sequence + 32U); // the window's end: nothing kept past it
    // Each worker's events in the window are its own consecutive events: none lost between rings.
    bool runs = true;
    for (u32 w = 0U; w < k_workers; ++w)
    {
        bool seen = false;
        u64  prev = 0U;
        for (const cp::FlightEvent& e : c.events)
        {
            if (e.code != kNoiseCode || e.value_b != w)
            {
                continue;
            }
            if (seen && e.value_a != prev + 1U)
            {
                runs = false;
            }
            seen = true;
            prev = e.value_a;
        }
    }
    CHECK(runs);

    // Every event either has a sequence below the window's end or was dropped while the capture waited; the capture
    // gave the dropped events' sequences back, so the next event takes the window's end, and no event recorded after
    // the window closed is in any later capture.
    CHECK(r.total_recorded() == frozen);
    CHECK(k_workers * k_each + 1U == frozen + c.dropped_while_complete);
    CHECK(c.dropped_while_complete > 0U);
    for (u64 i = 0U; i < 8U; ++i)
    {
        r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"after collection"}, 10U + i, 99U);
    }
    REQUIRE(r.trigger(manual("again")) == cp::FlightTriggerResult::Started);
    for (u64 i = 0U; i < 32U; ++i)
    {
        r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"post"}, 100U + i, 99U);
    }
    cp::FlightCapture c2;
    REQUIRE(r.take_capture(c2));
    CHECK(consistent(c2));
    CHECK(c2.dropped_while_complete == 0U);
    CHECK(c2.trigger.sequence == frozen + 8U);
    bool markers = true;
    for (const cp::FlightEvent& e : c2.events)
    {
        const u64 expected =
            (e.sequence < frozen + 8U) ? 10U + (e.sequence - frozen) : 100U + (e.sequence - frozen - 8U);
        if (e.sequence >= frozen && (e.code != kMarkerCode || e.value_a != expected))
        {
            markers = false;
        }
    }
    CHECK(markers);
}

TEST_CASE("flight rings: late and exiting threads, rings without storage and threads past the rings keep every event",
          "[perf][diag][diag11b][flight][concurrency]")
{
    SECTION("a thread that exits gives its ring back and its events stay; a late thread takes the ring")
    {
        cp::FlightRecorder r;
        REQUIRE(r.init(policy(1024U, 512U, 4U)) == cp::FlightPolicyStatus::Committed);
        CHECK(r.ring_stats().thread_rings == cp::kFlightThreadRings);
        CHECK(r.ring_stats().threads_bound == 0U);
        std::thread first(
            [&r]
            {
                for (u64 i = 0U; i < 5U; ++i)
                {
                    r.record(kHopCode, cp::Severity::Info, cont::StringView{"first"}, i, 1U);
                }
            });
        first.join();
        CHECK(r.ring_stats().threads_bound == 0U); // given back at its exit
        std::thread late(
            [&r]
            {
                for (u64 i = 5U; i < 10U; ++i)
                {
                    r.record(kHopCode, cp::Severity::Info, cont::StringView{"late"}, i, 2U);
                }
            });
        late.join();
        CHECK(r.ring_stats().threads_bound == 0U);
        REQUIRE(r.trigger(manual("after both")) == cp::FlightTriggerResult::Started);
        for (u64 i = 10U; i < 14U; ++i)
        {
            r.record(kHopCode, cp::Severity::Info, cont::StringView{"main"}, i, 0U);
        }
        cp::FlightCapture c;
        REQUIRE(r.take_capture(c));
        CHECK(consistent(c));
        REQUIRE(c.events.size() == 14U);
        for (u64 i = 0U; i < 14U; ++i)
        {
            CHECK(c.events[i].value_a == i);
        }
        CHECK(c.events[4].value_b == 1U);
        CHECK(c.events[5].value_b == 2U);
        CHECK(r.ring_stats().threads_bound == 1U); // main
        CHECK(r.ring_stats().shared_events == 0U);
    }

    SECTION("a policy whose rings have storage for two threads: the others record into the shared ring")
    {
        // 65536 / 32768: two rings with storage. Four threads hop a token, so the expected order is known.
        constexpr u32 k_threads = 4U;
        constexpr u32 k_hops    = 400U;
        cp::FlightRecorder r;
        REQUIRE(r.init(policy(32768U, 256U, 8U)) == cp::FlightPolicyStatus::Committed);
        CHECK(r.ring_stats().thread_rings == 2U);
        CHECK(cp::flight_thread_rings(32768U) == 2U);
        CHECK(cp::flight_thread_rings(cp::kFlightMaxEvents) == 1U);
        CHECK(cp::flight_thread_rings(1024U) == cp::kFlightThreadRings);
        std::atomic<u32>  token{0U};
        std::atomic<bool> stalled{false};
        std::atomic<u32>  bound{0U};
        std::thread       hoppers[k_threads];
        for (u32 t = 0U; t < k_threads; ++t)
        {
            hoppers[t] = std::thread(
                [&r, &token, &stalled, &bound, t]
                {
                    for (u32 h = t; h < k_hops; h += k_threads)
                    {
                        if (!wait_until([&] { return token.load(std::memory_order_acquire) == h; }))
                        {
                            stalled.store(true);
                            return;
                        }
                        r.record(kHopCode, cp::Severity::Info, cont::StringView{"hop"}, h, t);
                        token.store(h + 1U, std::memory_order_release);
                    }
                    bound.fetch_add(1U);
                    (void)wait_until([&] { return bound.load() == k_threads + 1U; }); // stay alive until counted
                });
        }
        REQUIRE(wait_until([&] { return bound.load() == k_threads; }));
        const cp::FlightRingStats alive = r.ring_stats();
        bound.fetch_add(1U);
        for (auto& t : hoppers)
        {
            t.join();
        }
        REQUIRE_FALSE(stalled.load());
        CHECK(alive.threads_bound == k_threads);
        CHECK(alive.shared_events == static_cast<u64>(k_hops) / 2U); // two of the four threads' events
        REQUIRE(r.trigger(manual("shared")) == cp::FlightTriggerResult::Started);
        for (u32 i = 0U; i < 8U; ++i)
        {
            r.record(kHopCode, cp::Severity::Info, cont::StringView{"post"}, k_hops + i, 9U);
        }
        cp::FlightCapture c;
        REQUIRE(r.take_capture(c));
        CHECK(consistent(c));
        REQUIRE(c.pre_retained == 256U);
        bool ordered = true;
        for (crd::usize i = 0U; i < c.events.size(); ++i)
        {
            ordered = ordered && c.events[i].value_a == k_hops - 256U + i;
        }
        CHECK(ordered);
    }

    SECTION("more live threads than rings: the ones past them record into the shared ring")
    {
        constexpr u32 k_threads = cp::kFlightThreadRings + 6U;
        cp::FlightRecorder r;
        REQUIRE(r.init(policy(1024U, 512U, 4U)) == cp::FlightPolicyStatus::Committed);
        std::atomic<u32> token{0U};
        std::atomic<u32> done{0U};
        std::atomic<bool> stalled{false};
        std::thread      threads[k_threads];
        for (u32 t = 0U; t < k_threads; ++t)
        {
            threads[t] = std::thread(
                [&, t]
                {
                    // One event each, in thread order, every thread alive until all have recorded.
                    if (!wait_until([&] { return token.load(std::memory_order_acquire) == t; }))
                    {
                        stalled.store(true);
                        return;
                    }
                    r.record(kHopCode, cp::Severity::Info, cont::StringView{"one"}, t, t);
                    token.store(t + 1U, std::memory_order_release);
                    done.fetch_add(1U);
                    (void)wait_until([&] { return done.load() > k_threads; });
                });
        }
        REQUIRE(wait_until([&] { return done.load() == k_threads; }));
        const cp::FlightRingStats alive = r.ring_stats();
        done.fetch_add(1U);
        for (auto& t : threads)
        {
            t.join();
        }
        REQUIRE_FALSE(stalled.load());
        CHECK(alive.threads_bound == cp::kFlightThreadRings);
        CHECK(alive.shared_events == 6U);
        REQUIRE(r.trigger(manual("past the rings")) == cp::FlightTriggerResult::Started);
        for (u32 i = 0U; i < 4U; ++i)
        {
            r.record(kHopCode, cp::Severity::Info, cont::StringView{"post"}, k_threads + i, 0U);
        }
        cp::FlightCapture c;
        REQUIRE(r.take_capture(c));
        CHECK(consistent(c));
        REQUIRE(c.events.size() == k_threads + 4U);
        bool ordered = true;
        for (crd::usize i = 0U; i < c.events.size(); ++i)
        {
            ordered = ordered && c.events[i].value_a == i;
        }
        CHECK(ordered);
        CHECK(r.ring_stats().threads_bound == 1U);
    }
}

TEST_CASE("flight rings: a policy reload while threads record keeps every later capture complete and ordered",
          "[perf][diag][diag11b][flight][concurrency]")
{
    constexpr u32 k_workers = 3U;
    cp::FlightRecorder r;
    REQUIRE(r.init(policy(1024U, 256U, 16U)) == cp::FlightPolicyStatus::Committed);
    std::atomic<bool> stop{false};
    std::thread       workers[k_workers];
    for (u32 w = 0U; w < k_workers; ++w)
    {
        workers[w] = std::thread(
            [&r, &stop, w]
            {
                u64 i = 0U;
                while (!stop.load(std::memory_order_acquire))
                {
                    r.record(kNoiseCode, cp::Severity::Info, cont::StringView{"worker"}, i++, w);
                }
            });
    }
    // Between every reload the storage changes shape: all rings, two rings, all rings again.
    const u32 capacities[] = {32768U, 1024U, 32768U, 2048U};
    int       bad          = 0;
    for (const u32 capacity : capacities)
    {
        REQUIRE(r.reload_policy(policy(capacity, 256U, 16U)) == cp::FlightPolicyStatus::Committed);
        CHECK(r.ring_stats().thread_rings == cp::flight_thread_rings(capacity));
        REQUIRE(wait_until([&] { return r.total_recorded() > 2000U; }));
        REQUIRE(r.trigger(manual("between reloads")) == cp::FlightTriggerResult::Started);
        cp::FlightCapture c;
        REQUIRE(take(r, c));
        if (!consistent(c) || c.post_retained != 16U || c.policy.capacity != capacity)
        {
            ++bad;
        }
        // Each worker's events of the window are consecutive.
        for (u32 w = 0U; w < k_workers; ++w)
        {
            bool seen = false;
            u64  prev = 0U;
            for (const cp::FlightEvent& e : c.events)
            {
                if (e.value_b != w)
                {
                    continue;
                }
                if (seen && e.value_a != prev + 1U)
                {
                    ++bad;
                }
                seen = true;
                prev = e.value_a;
            }
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : workers)
    {
        t.join();
    }
    CHECK(bad == 0);
    CHECK(r.policy_generation() == 5U);
}

#if CRD_ENABLE_ASSERTS
TEST_CASE("flight rings: a ring allocation that fails refuses init and reload, and the last good rings stay",
          "[perf][diag][diag11b][flight]")
{
    cp::FlightRecorder r;
    cp::FlightRecorder::test_refuse_ring_storage(1U);
    CHECK(r.init(policy(64U, 32U, 4U)) == cp::FlightPolicyStatus::RejectedOutOfMemory);
    CHECK_FALSE(r.is_initialized());
    REQUIRE(r.init(policy(64U, 32U, 4U)) == cp::FlightPolicyStatus::Committed); // the seam refused one
    for (u64 i = 0U; i < 6U; ++i)
    {
        r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"kept"}, i, 0U);
    }
    cp::FlightRecorder::test_refuse_ring_storage(1U);
    CHECK(r.reload_policy(policy(128U, 64U, 4U)) == cp::FlightPolicyStatus::RejectedOutOfMemory);
    cp::FlightRecorder::test_refuse_ring_storage(0U);
    CHECK(r.policy_generation() == 1U);
    CHECK(r.policy().capacity == 64U);
    REQUIRE(r.trigger(manual("after the refusal")) == cp::FlightTriggerResult::Started);
    for (u64 i = 6U; i < 10U; ++i)
    {
        r.record(kMarkerCode, cp::Severity::Info, cont::StringView{"post"}, i, 0U);
    }
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(consistent(c));
    CHECK(c.pre_retained == 6U); // the rings were not replaced
    REQUIRE(c.events.size() == 10U);
    CHECK(c.events[0].value_a == 0U);
    CHECK(c.events[9].value_a == 9U);
}
#endif
