// crd-perf v0f -- CPROF capture format save + CaptureView round-trip.

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{

struct PerfCaptureFixture
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "perf-capture-test"};
    PerfCaptureFixture() { crd::perf::init({}); }
    ~PerfCaptureFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("save_capture_to_buffer produces a CPROF v1 buffer", "[perf][capture][save]")
{
    PerfCaptureFixture fx;

    // Record a tiny workload so the capture has content.
    {
        CRD_PERF_SCOPE("warm_up");
    }
    CRD_PERF_FRAME_MARK();

    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() >= sizeof(crd::perf::CprofHeader));

    const auto* hdr = reinterpret_cast<const crd::perf::CprofHeader*>(buf.data());
    CHECK(hdr->magic              == crd::perf::kCprofMagic);
    CHECK(hdr->version            == crd::perf::kCprofVersion);
    CHECK(hdr->sample_struct_size == sizeof(crd::perf::Sample));
    CHECK(hdr->frame_record_size  == sizeof(crd::perf::FrameRecord));
    CHECK(hdr->thread_count >= 1U);
    CHECK(hdr->frame_count  >= 1U);

    CHECK(crd::perf::validate_capture_buffer(
        crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));
}

TEST_CASE("save_capture round-trips through CaptureView", "[perf][capture][roundtrip]")
{
    PerfCaptureFixture fx;

    // Mixed workload: scope + counter + frame mark.
    {
        CRD_PERF_SCOPE("alpha");
    }
    {
        CRD_PERF_SCOPE("beta");
    }
    const auto cid = crd::perf::register_counter_i64("draws", crd::perf::CounterKind::Set);
    crd::perf::counter_set_i64(cid, 42);

    crd::memory::TlsfAllocator tlsf{1U << 16, nullptr, "tlsf_one"};
    const auto aid = crd::perf::register_allocator("tlsf_one", &tlsf);
    void* p = tlsf.allocate(4096U);
    REQUIRE(p != nullptr);

    CRD_PERF_FRAME_MARK();

    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() > 0U);

    crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    // Threads.
    CHECK(view.thread_count() >= 1U);
    bool found_alpha = false;
    bool found_beta  = false;
    for (crd::u32 t = 0U; t < view.thread_count(); ++t)
    {
        const auto samples = view.thread_samples(t);
        for (const auto& s : samples)
        {
            const char* nm = view.resolve_name(crd::perf::NameId{s.name_id});
            if (std::strcmp(nm, "alpha") == 0) found_alpha = true;
            if (std::strcmp(nm, "beta")  == 0) found_beta  = true;
        }
    }
    CHECK(found_alpha);
    CHECK(found_beta);

    // Counters.
    REQUIRE(view.counter_count() >= 1U);
    const auto cinfo = view.counter_info(cid.value);
    CHECK(std::strcmp(cinfo.name, "draws") == 0);
    CHECK(cinfo.kind == crd::perf::CounterKind::Set);
    CHECK(cinfo.type == crd::perf::CounterType::I64);

    // Allocators.
    REQUIRE(view.allocator_count() >= 1U);
    const auto ainfo = view.allocator_info(aid);
    CHECK(std::strcmp(ainfo.name, "tlsf_one") == 0);

    // Frame records.
    REQUIRE(view.frame_record_count() >= 1U);
    const auto recs = view.frame_records();
    REQUIRE(recs.size() >= 1U);
    // Latest record = last element (oldest-first ordering).
    const auto& latest = recs[recs.size() - 1U];
    CHECK(latest.allocator_count >= 1U);
    CHECK(latest.counter_count >= 1U);
    // Counter[cid] should hold 42.
    CHECK(static_cast<crd::i64>(latest.values[cid.value].bits) == 42);
    // Allocator[aid] should report >= 4096 bytes_in_use at capture time.
    CHECK(latest.allocators[aid].bytes_in_use >= 4096U);

    tlsf.deallocate(p);
}

TEST_CASE("validate_capture_buffer rejects bogus inputs", "[perf][capture][validate]")
{
    // Too short.
    crd::u8 tiny[8] = {};
    CHECK_FALSE(crd::perf::validate_capture_buffer(
        crd::containers::ConstSpan<crd::u8>{tiny, sizeof(tiny)}));

    // Right size but wrong magic.
    PerfCaptureFixture fx;
    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() > 0U);
    buf[0] = 'X';
    CHECK_FALSE(crd::perf::validate_capture_buffer(
        crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));
    // Restore magic, break version.
    buf[0] = 'C';
    auto* hdr = reinterpret_cast<crd::perf::CprofHeader*>(buf.data());
    hdr->version = 9999U;
    CHECK_FALSE(crd::perf::validate_capture_buffer(
        crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}));
}

TEST_CASE("CaptureView on bogus buffer is_valid() == false",
          "[perf][capture][view][robustness]")
{
    crd::u8 buf[1] = {0};
    crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf, sizeof(buf)}};
    CHECK_FALSE(view.is_valid());
    CHECK(view.thread_count() == 0U);
    CHECK(view.frame_records().size() == 0U);
    CHECK(std::strcmp(view.resolve_name(crd::perf::NameId{0}), "") == 0);
}

TEST_CASE("interned names round-trip via name blob", "[perf][capture][names]")
{
    PerfCaptureFixture fx;
    const auto na = crd::perf::intern_name("first_scope_name");
    const auto nb = crd::perf::intern_name("second_scope_name_longer");
    const auto nc = crd::perf::intern_name("c");

    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() > 0U);

    crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    CHECK(std::strcmp(view.resolve_name(na), "first_scope_name") == 0);
    CHECK(std::strcmp(view.resolve_name(nb), "second_scope_name_longer") == 0);
    CHECK(std::strcmp(view.resolve_name(nc), "c") == 0);
}

TEST_CASE("save_capture on inactive profiler returns empty buffer",
          "[perf][capture][save][robustness]")
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "inactive-test"};
    CHECK_FALSE(crd::perf::is_active());
    auto buf = crd::perf::save_capture_to_buffer(&alloc);
    CHECK(buf.size() == 0U);
}

TEST_CASE("CPROF header layout is pinned", "[perf][capture][format]")
{
    STATIC_REQUIRE(sizeof(crd::perf::CprofHeader)   == 72);
    STATIC_REQUIRE(sizeof(crd::perf::ThreadHeader)  == 56);
    STATIC_REQUIRE(sizeof(crd::perf::CounterMeta)   == 64);
    STATIC_REQUIRE(sizeof(crd::perf::AllocatorMeta) == 64);
    STATIC_REQUIRE(sizeof(crd::perf::Sample)        == 32);
    STATIC_REQUIRE(sizeof(crd::perf::FrameRecord)   == 3616);
}

#endif // CRD_PERF_ENABLED
