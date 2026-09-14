// DIAG.2b -- lifecycle-safe recording, reader tokens, bounded ring and emergency record.
// Contract: docs/design/runtime-diagnostics.md#diag-2b.

#include <crd/perf/diagnostic_recorder.hpp>
#include <crd/perf/diagnostics.hpp>

#include <crd/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;

cp::DiagnosticEvent ev(cp::EventCode code, crd::u32 line)
{
    cp::DiagnosticEvent e;
    e.code         = code;
    e.severity     = cp::Severity::Warning;
    e.source       = cp::SourceIdentity{cont::StringView{"rec.cpp"}, cont::StringView{"t"}, line};
    e.timestamp_ns = cp::diagnostic_now_ns();
    e.message      = cont::String{cont::StringView{"m"}};
    return e;
}
} // namespace

TEST_CASE("recorder: init/shutdown lifecycle and pre-init no-ops", "[diag][recorder]")
{
    cp::DiagnosticRecorder r;
    // Before init every entry point is a benign no-op, never a fault.
    CHECK_FALSE(r.is_initialized());
    r.record(ev(1U, 1U));
    CHECK(r.count() == 0U);
    cont::Array<cp::DiagnosticEvent> snap;
    r.snapshot(snap);
    CHECK(snap.size() == 0U);

    REQUIRE(r.init(16U));
    CHECK(r.is_initialized());
    CHECK(r.capacity() == 16U);
    r.shutdown();
    CHECK_FALSE(r.is_initialized());

    // Re-init after shutdown is legal.
    REQUIRE(r.init(8U));
    CHECK(r.capacity() == 8U);
    r.shutdown();
}

TEST_CASE("recorder: record then snapshot preserves order", "[diag][recorder]")
{
    cp::DiagnosticRecorder r;
    REQUIRE(r.init(8U));

    for (crd::u32 i = 0; i < 5U; ++i)
        r.record(ev(100U + i, i));

    CHECK(r.count() == 5U);
    CHECK(r.total_recorded() == 5U);

    cont::Array<cp::DiagnosticEvent> snap;
    r.snapshot(snap);
    REQUIRE(snap.size() == 5U);
    for (crd::u32 i = 0; i < 5U; ++i)
        CHECK(snap[i].code == 100U + i); // oldest first
    r.shutdown();
}

TEST_CASE("recorder: a full ring overwrites the oldest and keeps valid records", "[diag][recorder]")
{
    cp::DiagnosticRecorder r;
    REQUIRE(r.init(4U));

    for (crd::u32 i = 0; i < 10U; ++i) // 10 into a 4-slot ring
        r.record(ev(i, i));

    CHECK(r.count() == 4U);
    CHECK(r.total_recorded() == 10U);

    cont::Array<cp::DiagnosticEvent> snap;
    r.snapshot(snap);
    REQUIRE(snap.size() == 4U);
    // The four newest (codes 6,7,8,9), oldest-first, all with valid owned messages.
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK(snap[i].code == 6U + i);
        CHECK(snap[i].message.size() == 1U);
    }
    r.shutdown();
}

TEST_CASE("recorder: concurrent record is safe and bounded", "[diag][recorder][stress]")
{
    cp::DiagnosticRecorder r;
    REQUIRE(r.init(64U));

    static constexpr crd::u32 kThreads = 6U;
    static constexpr crd::u32 kEach    = 4000U;

    cont::Array<std::thread> threads;
    threads.reserve(kThreads);
    for (crd::u32 t = 0; t < kThreads; ++t)
        threads.emplace_back([&r, t]() {
            for (crd::u32 i = 0; i < kEach; ++i)
                r.record(ev(t * 1000U + (i & 0xFFU), i));
        });
    for (auto& th : threads)
        th.join();

    CHECK(r.count() == 64U);                              // bounded to capacity
    CHECK(r.total_recorded() == kThreads * kEach);        // every record accounted for
    cont::Array<cp::DiagnosticEvent> snap;
    r.snapshot(snap);
    CHECK(snap.size() == 64U);                            // no dangling / valid snapshot
    r.shutdown();
}

TEST_CASE("recorder: reader tokens register and retire; stale tokens are rejected", "[diag][recorder]")
{
    cp::DiagnosticRecorder r;
    REQUIRE(r.init(8U));

    cp::ReaderToken a = r.register_reader();
    cp::ReaderToken b = r.register_reader();
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(r.reader_count() == 2U);

    CHECK(r.deregister_reader(a));
    CHECK(r.reader_count() == 1U);
    // Double-deregister with the same (now stale) token is rejected, not a double-free.
    CHECK_FALSE(r.deregister_reader(a));

    // A new registration may reuse a's slot but gets a fresh generation; the old token stays stale.
    cp::ReaderToken c = r.register_reader();
    CHECK(c.valid());
    CHECK_FALSE(r.deregister_reader(a)); // stale generation
    CHECK(r.deregister_reader(c));
    CHECK(r.deregister_reader(b));
    CHECK(r.reader_count() == 0U);

    CHECK_FALSE(r.deregister_reader(cp::ReaderToken{})); // invalid token
    r.shutdown();
}

TEST_CASE("recorder: emergency record is captured independently of the ring", "[diag][recorder]")
{
    cp::DiagnosticRecorder r;
    REQUIRE(r.init(4U));

    CHECK_FALSE(r.emergency().present);

    cp::DiagnosticEvent fatal = ev(0xDEADU, 7U);
    fatal.severity            = cp::Severity::FatalInvariant;
    fatal.message             = cont::String{cont::StringView{"invariant violated"}};
    r.capture_emergency(fatal);

    const cp::EmergencyRecord em = r.emergency();
    CHECK(em.present);
    CHECK(em.code == 0xDEADU);
    CHECK(cp::is_fatal(em.severity));
    CHECK(em.line == 7U);
    CHECK(cont::StringView{em.message} == cont::StringView{"invariant violated"});
    CHECK(cont::StringView{em.file} == cont::StringView{"rec.cpp"});

    // The emergency capture does not disturb the ordinary ring.
    CHECK(r.count() == 0U);
    r.shutdown();
}
