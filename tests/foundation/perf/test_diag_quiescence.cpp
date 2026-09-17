// DIAG.6a(e): capture/shutdown quiescence (census defect #5). shutdown() used to free *g_state while a concurrent
// reader (a whole save_capture_to_buffer, a thread_samples copy, a frame-record read) still held a reference into it --
// a use-after-free. (e) makes g_state an atomic retired by a seq_cst exchange, and every reader increments a
// state-wide in-flight count (a Dekker handshake) that shutdown() DRAINS to zero before delete. A multi-call read
// (a capture) holds one outer detail::StateReadGuard so shutdown blocks until the whole capture completes.
//
// Three oracles: (1) DETERMINISTIC drain -- shutdown() provably WAITS while a reader holds a pin (the teeth: remove the
// drain and this CHECK fails); (2) post-shutdown readers return empty and take no pin; (3) a RACE loop of concurrent
// capture vs shutdown -- the ASan use-after-free oracle (win-asan). Catch2 macros are not thread-safe, so all
// assertions run on the main thread after join(); workers publish into atomics. Proof lanes: win-debug + win-asan.
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#if CRD_PERF_ENABLED

TEST_CASE("quiescence: shutdown waits for an in-flight state reader to finish", "[perf][diag][quiescence]")
{
    crd::perf::init({});

    std::atomic<bool> reader_pinned{false};   // reader has acquired a live pin
    std::atomic<bool> reader_pin_ok{false};    // the pin reported active
    std::atomic<bool> release_reader{false};   // main tells the reader to drop its pin
    std::atomic<bool> shutdown_returned{false};

    // Reader: hold a state-read pin until told to release. While held, g_in_flight >= 1.
    std::thread reader([&] {
        crd::perf::detail::StateReadGuard pin;
        reader_pin_ok.store(static_cast<bool>(pin), std::memory_order_release);
        reader_pinned.store(true, std::memory_order_release);
        while (!release_reader.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        // pin releases here at scope exit -> g_in_flight drops, unblocking a draining shutdown().
    });

    while (!reader_pinned.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    // Shutdown on another thread: it must retire the pointer then BLOCK in the drain until the reader releases.
    std::thread killer([&] {
        crd::perf::shutdown();
        shutdown_returned.store(true, std::memory_order_release);
    });

    // Give shutdown a real window to (wrongly) return early if the drain were missing.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool returned_while_pinned = shutdown_returned.load(std::memory_order_acquire);

    release_reader.store(true, std::memory_order_release);
    reader.join();
    killer.join();

    CHECK(reader_pin_ok.load(std::memory_order_acquire) == true); // the pin was live while the profiler was active
    CHECK(returned_while_pinned == false);                        // TEETH: shutdown did NOT return while pinned
    CHECK(shutdown_returned.load(std::memory_order_acquire) == true); // ...but did complete once released
    CHECK(crd::perf::is_active() == false);
}

TEST_CASE("quiescence: readers return empty and take no pin after shutdown", "[perf][diag][quiescence]")
{
    crd::perf::init({});
    crd::perf::frame_mark();
    crd::perf::shutdown();

    CHECK(crd::perf::thread_count() == 0U);
    CHECK(crd::perf::frame_record_count() == 0U);
    CHECK(crd::perf::live_allocator_count() == 0U);

    // A read pin cannot be acquired against a retired state.
    {
        crd::perf::detail::StateReadGuard pin;
        CHECK_FALSE(static_cast<bool>(pin));
    }

    // A capture attempt yields an empty buffer (the outer StateReadGuard is false -> immediate return).
    crd::memory::GrowableTlsfAllocator cap{1ULL << 20, nullptr, "q-empty-cap"};
    const auto                         buf = crd::perf::save_capture_to_buffer(&cap);
    CHECK(buf.size() == 0U);
}

TEST_CASE("quiescence: re-init on the init thread is safe after a cross-thread shutdown",
          "[perf][diag][quiescence]")
{
    crd::perf::init({});

    // Shut down on a DIFFERENT thread. shutdown() clears TLS only for its own caller, so THIS (init) thread keeps a
    // stale t_thread_index/t_ring pointing into the now-freed state -- the exact cross-thread teardown (e) legitimises.
    std::thread([] { crd::perf::shutdown(); }).join();

    // Re-init on this thread. Without the generation stamp, register_thread's refresh path would reuse the stale index
    // against the fresh state's empty ring, and the scope below would null-deref.
    crd::perf::init({});
    {
        CRD_PERF_SCOPE("q.reinit");
    }
    crd::perf::frame_mark();

    const crd::u8 idx = crd::perf::current_thread_index();
    CHECK(crd::perf::thread_count() == 1U);           // main re-registered cleanly into the new state
    CHECK(crd::perf::thread_samples(idx).size >= 1U); // the scope recorded into the NEW ring, not a freed one
    crd::perf::shutdown();
}

TEST_CASE("quiescence: concurrent capture and shutdown never touch freed state",
          "[perf][diag][quiescence][race]")
{
    // The ASan use-after-free oracle. Each iteration: build a state with real capture work, start a reader that
    // captures in a tight loop, then shutdown() from the main thread while a capture is in flight. Without the (e)
    // drain, shutdown would free *g_state under the reader's feet -> heap-use-after-free (win-asan). With it, shutdown
    // waits for the in-flight capture's outer pin before delete.
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "q-race-cap"};

    for (int iter = 0; iter < 64; ++iter)
    {
        crd::perf::init({});

        crd::memory::GrowableTlsfAllocator a0{1ULL << 20, nullptr, "q-race-a0"};
        crd::memory::GrowableTlsfAllocator a1{1ULL << 20, nullptr, "q-race-a1"};
        (void)crd::perf::register_allocator("q.race.a0", &a0);
        (void)crd::perf::register_allocator("q.race.a1", &a1);

        // Populate rings + frame history so a capture copies real bytes (a wider window for the race).
        for (int f = 0; f < 32; ++f)
        {
            {
                CRD_PERF_SCOPE("q.race.work");
            }
            crd::perf::frame_mark();
        }

        std::atomic<bool>     stop{false};
        std::atomic<crd::u64> captures{0U};
        std::thread           reader([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                const auto b = crd::perf::save_capture_to_buffer(&cap);
                (void)b;
                captures.fetch_add(1U, std::memory_order_relaxed);
            }
        });

        // Let at least one capture get in flight before pulling the state out from under it.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        crd::perf::shutdown();
        stop.store(true, std::memory_order_release);
        reader.join();
    }

    SUCCEED("no heap-use-after-free across concurrent capture/shutdown over 64 iterations");
}

#endif // CRD_PERF_ENABLED
