// DIAG.1b -- TSan qualification of the fiber happens-before model (positive controls).
//
// The fiber TSan model itself (create/switch/destroy through __tsan_*_fiber, every
// switch a synchronization point on the switching thread; cross-thread ordering via
// the work queues' acquire/release) lives in engine/foundation/jobs/src/sanitizer_fibers.hpp.
// These are the *positive* controls the design requires: known ordered specimens must
// pass, and the raw-thread and fiber versions of the same ordering must agree. They use
// only plain, non-atomic shared state deliberately -- each cross-fiber access is separated
// by a run()/wait() (or thread join), so the happens-before is real and TSan stays silent.
// If the model were wrong (e.g. a switch that failed to compose with the queue ordering),
// TSan would report these as races.
//
// The *negative* controls -- deliberately unsynchronized siblings that TSan MUST report --
// cannot be ordinary ctest cases: under TSAN_OPTIONS=halt_on_error=1 an intentional race
// aborts the process, so a passing suite cannot contain one. They are owned by the diag
// specimen harness (a specimen that races + a check that TSan caught it), mirroring the
// DIAG.0 crash/heap-overflow specimens, and run only on the hosted linux-clang-tsan lane.
// This file's cases run and pass everywhere (win-debug included); under the TSan preset
// they additionally certify the model is silent on correct orderings.
//
// Contract: docs/design/runtime-diagnostics.md#diag-1b; ADR-0133.

#include <catch2/catch_test_macros.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/core/types.hpp>

#include <thread>

namespace
{
// Two-hop relay so a reader job copies from one plain int to another.
struct Relay
{
    const int* in;
    int*       out;
};
} // namespace

// A write in one fiber, made visible to a read in a later fiber purely by the wait()
// between them. No atomics on the shared int: correctness rests entirely on the model's
// happens-before. Under TSan this must stay clean.
TEST_CASE("tsan: ordered write->wait->read across fibers is happens-before clean",
          "[jobs][tsan][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // a background worker is required for the main-thread spin-wait
    crd::jobs::init(cfg);

    int shared = 0; // plain, non-atomic -- safe only via the ordering established below

    crd::jobs::JobDecl writer{};
    writer.fn   = [](void* p) { *static_cast<int*>(p) = 42; };
    writer.data = &shared;
    crd::jobs::Counter* cw = crd::jobs::run(writer);
    crd::jobs::wait(cw); // writer completes happens-before everything after this point

    int         observed = 0;
    Relay       relay{&shared, &observed};
    crd::jobs::JobDecl reader{};
    reader.fn   = [](void* p) { auto* r = static_cast<Relay*>(p); *r->out = *r->in; };
    reader.data = &relay;
    crd::jobs::Counter* cr = crd::jobs::run(reader);
    crd::jobs::wait(cr);

    CHECK(observed == 42);

    crd::jobs::shutdown();
}

// The same ordering expressed two ways -- a raw std::thread joined, and a fiber job waited.
// The design requires the raw-thread and fiber versions to agree; both are happens-before
// clean, so TSan must be silent on both.
TEST_CASE("tsan: raw-thread and fiber orderings agree", "[jobs][tsan][diag]")
{
    // Raw thread: write in a thread, join (happens-before), read on main.
    {
        int         shared = 0;
        std::thread w([&shared]() { shared = 7; });
        w.join();
        CHECK(shared == 7);
    }

    // Fiber: identical shape, ordering via run()/wait() instead of join().
    {
        crd::jobs::Config cfg;
        cfg.num_threads = 2U;
        crd::jobs::init(cfg);

        int                shared = 0;
        crd::jobs::JobDecl w{};
        w.fn   = [](void* p) { *static_cast<int*>(p) = 7; };
        w.data = &shared;
        crd::jobs::Counter* c = crd::jobs::run(w);
        crd::jobs::wait(c);
        CHECK(shared == 7);

        crd::jobs::shutdown();
    }
}

// A fiber that migrates: run a chain where each hop's wait() re-establishes ordering, so a
// single plain accumulator is safely handed hop to hop even though hops may resume on
// different worker threads. Exercises the switch-composes-with-queue-ordering claim.
TEST_CASE("tsan: ordered hand-off chain across migrating fibers is clean",
          "[jobs][tsan][diag]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    int acc = 0; // plain: each hop is separated from the next by a wait()

    for (crd::u32 hop = 0; hop < 32U; ++hop)
    {
        crd::jobs::JobDecl step{};
        step.fn   = [](void* p) { ++(*static_cast<int*>(p)); };
        step.data = &acc;
        crd::jobs::Counter* c = crd::jobs::run(step);
        crd::jobs::wait(c);
    }

    CHECK(acc == 32);

    crd::jobs::shutdown();
}
