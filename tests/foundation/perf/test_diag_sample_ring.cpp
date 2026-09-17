// DIAG.6a(c2): the per-thread sample ring is DROP-ON-FULL -- the writer refuses to write when full and never
// overwrites the unread [tail, head) region, so a single reader has no torn-payload hazard (this corrects census
// defect #3, which assumed overwrite-oldest). The real sample-ring defect is a WRONG-SAMPLES-AFTER-WRAP bug:
// thread_samples() returns the ring BASE + size, so once the ring has wrapped (tail != 0 -- e.g. after a perf-ui
// clear_samples) a consumer that reads data[0..size) reads the wrong slots. copy_thread_samples() copies the live
// samples oldest-first regardless of wrap, and the capture save path now uses it. Two proofs below: a direct
// copy-vs-raw-view divergence, and an end-to-end wrap-then-save read back through CaptureView. Proof lane: win-debug.
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};
} // namespace

TEST_CASE("sample ring: copy_thread_samples is wrap-correct after a clear", "[perf][diag][sample-ring]")
{
    PerfFixture fx;
    const auto  idx = crd::perf::current_thread_index();

    for (int i = 0; i < 5; ++i)
    {
        CRD_PERF_SCOPE("ring.a");
    }
    crd::perf::clear_samples(); // tail -> head (=5): the ring is now "wrapped" (tail != 0)
    for (int i = 0; i < 5; ++i)
    {
        CRD_PERF_SCOPE("ring.b");
    }

    // The fix: copy returns the live (post-clear) batch B, oldest-first, regardless of the wrap.
    crd::perf::Sample copied[16];
    const crd::u32    n = crd::perf::copy_thread_samples(idx, copied, 16U);
    REQUIRE(n == 5U);
    for (crd::u32 k = 0; k < n; ++k)
    {
        CHECK(std::strcmp(crd::perf::resolve_name(crd::perf::NameId{copied[k].name_id}), "ring.b") == 0);
    }

    // The zero-copy view still base-indexes: after a wrap its data[0] is a stale batch-A slot, so it DIVERGES from the
    // wrap-correct copy above -- exactly the bug copy_thread_samples supersedes and the capture path no longer relies on.
    // We assert the divergence property (not the exact stale value) so it survives the fix: if 6a(c3) makes the view
    // itself wrap-correct, data[0] becomes "ring.b" and THIS divergence CHECK is the one dropped, not the copy CHECK.
    const auto        v    = crd::perf::thread_samples(idx);
    REQUIRE(v.size == 5U);
    const char* const raw0 = crd::perf::resolve_name(crd::perf::NameId{v.data[0].name_id});
    INFO("raw zero-copy view after wrap sees: " << raw0);
    CHECK(std::strcmp(raw0, "ring.b") != 0); // diverges from the wrap-correct copy -- the bug the copy API supersedes
}

TEST_CASE("sample ring: a capture saved after a wrap holds the live samples, not stale ones",
          "[perf][diag][sample-ring]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "c2-capture"};
    PerfFixture                        fx;

    for (int i = 0; i < 5; ++i)
    {
        CRD_PERF_SCOPE("save.old");
    }
    crd::perf::clear_samples();
    for (int i = 0; i < 5; ++i)
    {
        CRD_PERF_SCOPE("save.new");
    }

    const auto buf = crd::perf::save_capture_to_buffer(&alloc);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    // Every saved sample must be a post-clear "save.new". The pre-fix save copied the ring base and would have written
    // stale "save.old" samples here (the wrong-samples-after-wrap bug, now in the persisted file).
    bool checked = false;
    for (crd::u32 t = 0; t < view.thread_count(); ++t)
    {
        const auto samples = view.thread_samples(t);
        for (const auto& s : samples)
        {
            CHECK(std::strcmp(view.resolve_name(crd::perf::NameId{s.name_id}), "save.new") == 0);
            checked = true;
        }
    }
    CHECK(checked); // the recording thread's samples were actually inspected (guards a vacuous pass)
}

#endif // CRD_PERF_ENABLED
