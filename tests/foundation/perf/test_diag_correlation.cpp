// DIAG.6b(b): CPROF correlation side table. GPU/queue correlation is carried SLOT-PARALLEL to the sample ring (not in
// the pinned 32 B Sample) and saved as a SPARSE appended CPROF section, signalled by a flags bit and located by
// CprofHeader.correlation_section_offset -- so old readers ignore it and existing captures still load. The load-bearing
// property is that a saved sample's ordinal indexes its record by construction: the copier copies both arrays under one
// hold, at the same ring slot. Oracles: round-trip; slot-parallel under wrap (the desync oracle); mixed valid/cleared;
// existing-capture compatibility; and a corrupt section rejected by validation. Proof lanes: win-debug + win-asan.
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace crd::perf::detail
{
// White-box: the external-sample writer is the gpu-track producer (b) attaches correlation to.
void write_external_sample(crd::u8 thread_index, const Sample& s, const CorrelationRecord* corr) noexcept;
} // namespace crd::perf::detail

namespace
{
crd::perf::Sample make_sample(crd::i64 begin_ns, crd::u8 thread) noexcept
{
    crd::perf::Sample s{};
    s.begin_ns     = begin_ns;
    s.end_ns       = begin_ns + 100;
    s.name_id      = 0U;
    s.color_rgba   = 0U;
    s.begin_thread = thread;
    s.end_thread   = thread;
    s.depth        = 0U;
    s.category     = static_cast<crd::u8>(crd::perf::Category::Gpu);
    s.fiber_id     = 0U;
    return s;
}

crd::perf::CorrelationRecord make_corr(crd::u32 queue_id, crd::perf::NameId pass) noexcept
{
    crd::perf::CorrelationRecord r{};
    r.flags                = crd::perf::kCorrelationValid;
    r.queue_id             = queue_id;
    r.device_id            = 7U;
    r.clock_domain         = 1U;
    r.clock_uncertainty_ns = 0U;
    r.pass_id              = pass.value;
    r.resource_id          = crd::perf::kNoCorrelationName;
    return r;
}
} // namespace

TEST_CASE("correlation: round-trips through CPROF and resolves by (thread, ordinal)", "[perf][diag][correlation]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "corr-rt-cap"};
    crd::perf::init({});
    const crd::u8 th = crd::perf::current_thread_index();
    crd::perf::enable_thread_correlation(th);

    constexpr crd::u32 kN   = 16U;
    const crd::perf::NameId pass = crd::perf::intern_name("shadow_pass");
    for (crd::u32 i = 0U; i < kN; ++i)
    {
        const auto s = make_sample(1000 + static_cast<crd::i64>(i), th);
        const auto r = make_corr(1000U + i, pass); // queue_id == begin_ns, so we can join back
        crd::perf::detail::write_external_sample(th, s, &r);
    }

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    REQUIRE(buf.size() > 0U);
    REQUIRE(crd::perf::validate_capture_buffer(crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    REQUIRE(view.correlation_count() == kN);

    // Every sample's record is findable by (thread, ordinal) and joins back to the sample (queue_id == begin_ns).
    const auto samples = view.thread_samples(0U); // dense thread 0 == live main
    REQUIRE(samples.size() == kN);
    for (crd::u32 i = 0U; i < kN; ++i)
    {
        const auto* rec = view.correlation_for(th, i);
        REQUIRE(rec != nullptr);
        CHECK(rec->queue_id == static_cast<crd::u32>(samples[i].begin_ns));
        CHECK(rec->device_id == 7U);
        CHECK(std::strcmp(view.resolve_name(crd::perf::NameId{rec->pass_id}), "shadow_pass") == 0);
    }
    // An ordinal with no record returns nullptr.
    CHECK(view.correlation_for(th, kN) == nullptr);

    // Indexed access: records are sorted by (thread, ordinal), so index 0 is thread 0 / ordinal 0, and an
    // out-of-range index is nullptr.
    const auto* first = view.correlation_at(0U);
    REQUIRE(first != nullptr);
    CHECK(first->queue_id == static_cast<crd::u32>(samples[0].begin_ns));
    CHECK(view.correlation_at(kN) == nullptr);
    crd::perf::shutdown();
}

TEST_CASE("correlation: stays slot-parallel with its sample across a ring wrap", "[perf][diag][correlation]")
{
    // Small ring + a clear to push tail off zero, so the live window [tail, head) crosses the mask wrap. If the copier
    // ever desynced the two arrays, a saved record's queue_id would stop matching its sample's begin_ns.
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "corr-wrap-cap"};
    crd::perf::InitConfig cfg{};
    cfg.per_thread_ring_slots = 8U;
    crd::perf::init(cfg);
    const crd::u8 th = crd::perf::current_thread_index();
    crd::perf::enable_thread_correlation(th);

    for (crd::i64 i = 0; i < 5; ++i) // fill 5 slots, then discard them so tail advances to 5
    {
        const auto s = make_sample(2000 + i, th);
        const auto r = make_corr(static_cast<crd::u32>(2000 + i), crd::perf::kInvalidNameId);
        crd::perf::detail::write_external_sample(th, s, &r);
    }
    crd::perf::clear_samples();
    for (crd::i64 i = 0; i < 8; ++i) // now [tail=5, head=13): slot indices 5,6,7,0,1,2,3,4 -- crosses the wrap
    {
        const auto s = make_sample(3000 + i, th);
        const auto r = make_corr(static_cast<crd::u32>(3000 + i), crd::perf::kInvalidNameId);
        crd::perf::detail::write_external_sample(th, s, &r);
    }

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const auto samples = view.thread_samples(0U);
    REQUIRE(samples.size() == 8U);
    REQUIRE(view.correlation_count() == 8U);
    for (crd::u32 i = 0U; i < samples.size(); ++i)
    {
        const auto* rec = view.correlation_for(th, i);
        REQUIRE(rec != nullptr);
        // The join is the oracle: queue_id was set == begin_ns at emit. A slot-parallel desync breaks this.
        CHECK(rec->queue_id == static_cast<crd::u32>(samples[i].begin_ns));
    }
    crd::perf::shutdown();
}

TEST_CASE("correlation: mixed valid/none, and a slot reused without correlation is cleared (no stale record)",
          "[perf][diag][correlation]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "corr-mixed-cap"};

    // Part A (fresh ring, no reuse): alternate valid / none -> only the valid ordinals are reported.
    {
        crd::perf::init({});
        const crd::u8 th = crd::perf::current_thread_index();
        crd::perf::enable_thread_correlation(th);
        constexpr crd::u32 kN = 10U;
        for (crd::u32 i = 0U; i < kN; ++i)
        {
            const auto s = make_sample(5000 + static_cast<crd::i64>(i), th);
            if ((i & 1U) == 0U)
            {
                const auto r = make_corr(5000U + i, crd::perf::kInvalidNameId);
                crd::perf::detail::write_external_sample(th, s, &r);
            }
            else
            {
                crd::perf::detail::write_external_sample(th, s, nullptr);
            }
        }
        const auto buf = crd::perf::save_capture_to_buffer(&cap);
        REQUIRE(buf.size() > 0U);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
        REQUIRE(view.is_valid());
        CHECK(view.correlation_count() == kN / 2U);
        for (crd::u32 i = 0U; i < kN; ++i)
        {
            const auto* rec = view.correlation_for(th, i);
            if ((i & 1U) == 0U)
            {
                REQUIRE(rec != nullptr);
                CHECK(rec->queue_id == 5000U + i);
            }
            else
            {
                CHECK(rec == nullptr);
            }
        }
        crd::perf::shutdown();
    }

    // Part B (slot REUSE, the kValid-clear oracle): fill every slot with a VALID record, discard via clear_samples,
    // then refill the SAME slots with samples that carry NO correlation. write_external_sample must clear each reused
    // slot; otherwise the stale batch-1 records would be reported for batch-2 samples.
    {
        crd::perf::InitConfig cfg{};
        cfg.per_thread_ring_slots = 8U;
        crd::perf::init(cfg);
        const crd::u8 th = crd::perf::current_thread_index();
        crd::perf::enable_thread_correlation(th);
        for (crd::u32 i = 0U; i < 8U; ++i) // batch 1: all valid
        {
            const auto s = make_sample(8000 + static_cast<crd::i64>(i), th);
            const auto r = make_corr(8000U + i, crd::perf::kInvalidNameId);
            crd::perf::detail::write_external_sample(th, s, &r);
        }
        crd::perf::clear_samples();
        for (crd::u32 i = 0U; i < 8U; ++i) // batch 2: reuse the slots, NO correlation
        {
            const auto s = make_sample(9000 + static_cast<crd::i64>(i), th);
            crd::perf::detail::write_external_sample(th, s, nullptr);
        }
        const auto buf = crd::perf::save_capture_to_buffer(&cap);
        REQUIRE(buf.size() > 0U);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
        REQUIRE(view.is_valid());
        CHECK(view.correlation_count() == 0U); // TEETH: skip the clear -> stale batch-1 records reappear (count == 8)
        for (crd::u32 i = 0U; i < view.thread_samples(0U).size(); ++i)
        {
            CHECK(view.correlation_for(th, i) == nullptr);
        }
        crd::perf::shutdown();
    }
}

TEST_CASE("correlation: a capture with none enabled is flag-clear and loads unchanged (existing-capture compat)",
          "[perf][diag][correlation]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "corr-compat-cap"};
    crd::perf::init({});
    const crd::u8 th = crd::perf::current_thread_index();
    // NO enable_thread_correlation. Emit a few plain samples.
    for (crd::i64 i = 0; i < 4; ++i)
    {
        const auto s = make_sample(7000 + i, th);
        crd::perf::detail::write_external_sample(th, s, nullptr);
    }
    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    REQUIRE(buf.size() > 0U);
    REQUIRE(crd::perf::validate_capture_buffer(crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));

    const auto* hdr = reinterpret_cast<const crd::perf::CprofHeader*>(buf.data());
    CHECK((hdr->flags & crd::perf::kCprofFlagCorrelation) == 0U); // no section advertised
    CHECK(hdr->correlation_section_offset == 0U);

    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    CHECK(view.correlation_count() == 0U);
    CHECK(view.correlation_for(th, 0U) == nullptr);
    CHECK(view.thread_samples(0U).size() == 4U); // base sections load exactly as before
    crd::perf::shutdown();
}

TEST_CASE("correlation: a corrupt section is rejected by validation", "[perf][diag][correlation]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "corr-corrupt-cap"};
    crd::perf::init({});
    const crd::u8 th = crd::perf::current_thread_index();
    crd::perf::enable_thread_correlation(th);
    for (crd::u32 i = 0U; i < 4U; ++i)
    {
        const auto s = make_sample(9000 + static_cast<crd::i64>(i), th);
        const auto r = make_corr(9000U + i, crd::perf::kInvalidNameId);
        crd::perf::detail::write_external_sample(th, s, &r);
    }
    auto buf = crd::perf::save_capture_to_buffer(&cap);
    REQUIRE(buf.size() > 0U);
    REQUIRE(crd::perf::validate_capture_buffer(crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));

    // Corrupt the section offset to point past the end -> validation must reject (else CaptureView would OOB-read).
    auto* hdr = reinterpret_cast<crd::perf::CprofHeader*>(buf.data());
    REQUIRE((hdr->flags & crd::perf::kCprofFlagCorrelation) != 0U);
    hdr->correlation_section_offset = static_cast<crd::u32>(buf.size()); // past the 8 B header room
    CHECK_FALSE(crd::perf::validate_capture_buffer(crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));
    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
