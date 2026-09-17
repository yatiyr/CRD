// crd-perf -- DIAG.6c(b): CPROF version discipline.
//
// "Version CPROF without silently changing pinned layouts." Two teeth:
//   1. The on-disk layout is PINNED FIELD-BY-FIELD (offsetof + sizeof, compile-time). A reorder or width change to
//      any CPROF struct fails THIS build before it can change bytes on disk -- the sizeof pins in the headers catch a
//      size change, these offset pins catch a same-size reorder they would miss.
//   2. The reader REJECTS a wrong version / magic / struct-size and TOLERATES an unknown flag bit (an additive
//      optional section a newer writer may set) -- the forward-compat half of "old readers accept new files, new
//      readers accept old files". Both the cheap gate (`validate_capture_buffer`) and the parse path
//      (`CaptureView::is_valid`) are checked: a reject in one but not the other would be a hole.

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/sample.hpp>
#include <crd/perf/frame_record.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef> // offsetof

#if CRD_PERF_ENABLED

// ============================================================================================================
// Pinned on-disk layout contract (compile-time; init-free). Every field of every CPROF on-disk struct.
// offsetof requires standard-layout; all these are POD. Hardcoded numbers are the format ABI -- changing one
// here means the on-disk format changed and CprofVersion MUST bump (see the procedure note at kCprofVersion).
// ============================================================================================================
namespace cp = crd::perf;

// CprofHeader (72 B)
static_assert(sizeof(cp::CprofHeader) == 72, "CprofHeader size pin");
static_assert(offsetof(cp::CprofHeader, magic) == 0, "CprofHeader.magic @0");
static_assert(offsetof(cp::CprofHeader, version) == 4, "CprofHeader.version @4");
static_assert(offsetof(cp::CprofHeader, flags) == 8, "CprofHeader.flags @8");
static_assert(offsetof(cp::CprofHeader, captured_at_ns) == 16, "CprofHeader.captured_at_ns @16");
static_assert(offsetof(cp::CprofHeader, thread_count) == 24, "CprofHeader.thread_count @24");
static_assert(offsetof(cp::CprofHeader, counter_count) == 28, "CprofHeader.counter_count @28");
static_assert(offsetof(cp::CprofHeader, allocator_count) == 32, "CprofHeader.allocator_count @32");
static_assert(offsetof(cp::CprofHeader, frame_count) == 36, "CprofHeader.frame_count @36");
static_assert(offsetof(cp::CprofHeader, sample_struct_size) == 40, "CprofHeader.sample_struct_size @40");
static_assert(offsetof(cp::CprofHeader, frame_record_size) == 48, "CprofHeader.frame_record_size @48");
static_assert(offsetof(cp::CprofHeader, name_blob_byte_size) == 56, "CprofHeader.name_blob_byte_size @56");
static_assert(offsetof(cp::CprofHeader, name_blob_count) == 64, "CprofHeader.name_blob_count @64");
static_assert(offsetof(cp::CprofHeader, correlation_section_offset) == 68, "CprofHeader.correlation_section_offset @68");

// ThreadHeader (56 B)
static_assert(sizeof(cp::ThreadHeader) == 56, "ThreadHeader size pin");
static_assert(offsetof(cp::ThreadHeader, thread_index) == 0, "ThreadHeader.thread_index @0");
static_assert(offsetof(cp::ThreadHeader, sample_count) == 4, "ThreadHeader.sample_count @4");
static_assert(offsetof(cp::ThreadHeader, sample_byte_offset) == 8, "ThreadHeader.sample_byte_offset @8");
static_assert(offsetof(cp::ThreadHeader, name) == 16, "ThreadHeader.name @16");
static_assert(offsetof(cp::ThreadHeader, dropped_count) == 48, "ThreadHeader.dropped_count @48");
static_assert(offsetof(cp::ThreadHeader, _pad_a) == 52, "ThreadHeader._pad_a @52 (spare slot -- pinned so repurposing it is deliberate)");

// CounterMeta (64 B)
static_assert(sizeof(cp::CounterMeta) == 64, "CounterMeta size pin");
static_assert(offsetof(cp::CounterMeta, index) == 0, "CounterMeta.index @0");
static_assert(offsetof(cp::CounterMeta, kind) == 4, "CounterMeta.kind @4");
static_assert(offsetof(cp::CounterMeta, type) == 5, "CounterMeta.type @5");
static_assert(offsetof(cp::CounterMeta, _pad_a) == 6, "CounterMeta._pad_a @6 (spare slot)");
static_assert(offsetof(cp::CounterMeta, name) == 8, "CounterMeta.name @8");

// AllocatorMeta (64 B)
static_assert(sizeof(cp::AllocatorMeta) == 64, "AllocatorMeta size pin");
static_assert(offsetof(cp::AllocatorMeta, index) == 0, "AllocatorMeta.index @0");
static_assert(offsetof(cp::AllocatorMeta, _pad_a) == 4, "AllocatorMeta._pad_a @4 (spare slot)");
static_assert(offsetof(cp::AllocatorMeta, name) == 8, "AllocatorMeta.name @8");

// Sample (32 B) -- also runtime-CHECKed in test_sample_pod.cpp; pinned here at compile time so the whole
// on-disk contract lives in one place and a reorder fails the build, not just the test run.
static_assert(sizeof(cp::Sample) == 32, "Sample size pin");
static_assert(offsetof(cp::Sample, begin_ns) == 0, "Sample.begin_ns @0");
static_assert(offsetof(cp::Sample, end_ns) == 8, "Sample.end_ns @8");
static_assert(offsetof(cp::Sample, name_id) == 16, "Sample.name_id @16");
static_assert(offsetof(cp::Sample, color_rgba) == 20, "Sample.color_rgba @20");
static_assert(offsetof(cp::Sample, begin_thread) == 24, "Sample.begin_thread @24");
static_assert(offsetof(cp::Sample, end_thread) == 25, "Sample.end_thread @25");
static_assert(offsetof(cp::Sample, depth) == 26, "Sample.depth @26");
static_assert(offsetof(cp::Sample, category) == 27, "Sample.category @27");
static_assert(offsetof(cp::Sample, fiber_id) == 28, "Sample.fiber_id @28");

// CorrelationRecord (40 B)
static_assert(sizeof(cp::CorrelationRecord) == 40, "CorrelationRecord size pin");
static_assert(offsetof(cp::CorrelationRecord, flags) == 0, "CorrelationRecord.flags @0");
static_assert(offsetof(cp::CorrelationRecord, thread_index) == 4, "CorrelationRecord.thread_index @4");
static_assert(offsetof(cp::CorrelationRecord, sample_ordinal) == 8, "CorrelationRecord.sample_ordinal @8");
static_assert(offsetof(cp::CorrelationRecord, queue_id) == 12, "CorrelationRecord.queue_id @12");
static_assert(offsetof(cp::CorrelationRecord, device_id) == 16, "CorrelationRecord.device_id @16");
static_assert(offsetof(cp::CorrelationRecord, clock_domain) == 20, "CorrelationRecord.clock_domain @20");
static_assert(offsetof(cp::CorrelationRecord, clock_uncertainty_ns) == 24, "CorrelationRecord.clock_uncertainty_ns @24");
static_assert(offsetof(cp::CorrelationRecord, pass_id) == 32, "CorrelationRecord.pass_id @32");
static_assert(offsetof(cp::CorrelationRecord, resource_id) == 36, "CorrelationRecord.resource_id @36");

// FrameRecord (32 B scalar head + pinned arrays). The array offsets are expressed in the same sizing knobs the
// header's sizeof pin uses, so a knob change (kMaxCounters) moves them consistently and the runtime
// frame_record_size check still guards it.
static_assert(offsetof(cp::FrameRecord, frame_index) == 0, "FrameRecord.frame_index @0");
static_assert(offsetof(cp::FrameRecord, frame_begin_ns) == 8, "FrameRecord.frame_begin_ns @8");
static_assert(offsetof(cp::FrameRecord, frame_end_ns) == 16, "FrameRecord.frame_end_ns @16");
static_assert(offsetof(cp::FrameRecord, counter_count) == 24, "FrameRecord.counter_count @24");
static_assert(offsetof(cp::FrameRecord, allocator_count) == 28, "FrameRecord.allocator_count @28");
static_assert(offsetof(cp::FrameRecord, values) == 32, "FrameRecord.values @32");
static_assert(offsetof(cp::FrameRecord, allocators)
                  == 32 + sizeof(cp::RawCounterValue) * cp::kMaxCounters,
              "FrameRecord.allocators after the values array");

namespace
{

struct PerfCaptureFixture
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "perf-cprof-version-test"};
    PerfCaptureFixture() { crd::perf::init({}); }
    ~PerfCaptureFixture() { crd::perf::shutdown(); }
};

// A fresh, valid CPROF v1 buffer with a little content.
[[nodiscard]] crd::containers::Array<crd::u8> make_capture(crd::memory::IAllocator* alloc)
{
    {
        CRD_PERF_SCOPE("warm_up");
    }
    CRD_PERF_FRAME_MARK();
    return crd::perf::save_capture_to_buffer(alloc);
}

[[nodiscard]] bool valid(const crd::containers::Array<crd::u8>& b)
{
    return crd::perf::validate_capture_buffer(
        crd::containers::ConstSpan<crd::u8>{b.data(), b.size()});
}

[[nodiscard]] bool view_valid(const crd::containers::Array<crd::u8>& b)
{
    crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{b.data(), b.size()}};
    return view.is_valid();
}

} // namespace

TEST_CASE("CPROF reader rejects unknown versions (v0, v2)", "[perf][diag][cprof-version]")
{
    PerfCaptureFixture fx;
    auto buf = make_capture(&fx.alloc);
    REQUIRE(buf.size() >= sizeof(crd::perf::CprofHeader));
    auto* hdr = reinterpret_cast<crd::perf::CprofHeader*>(buf.data());
    REQUIRE(hdr->version == crd::perf::kCprofVersion);
    REQUIRE(valid(buf));       // baseline
    REQUIRE(view_valid(buf));

    // A future v2 file: a v1 reader must reject it cleanly, never silently misread it. //TEETH-VER (make the
    // reader accept a wrong version and this case flips).
    hdr->version = 2U;
    CHECK_FALSE(valid(buf));
    CHECK_FALSE(view_valid(buf));

    // A pre-release v0 file: same.
    hdr->version = 0U;
    CHECK_FALSE(valid(buf));
    CHECK_FALSE(view_valid(buf));

    hdr->version = crd::perf::kCprofVersion; // restore -> valid again
    CHECK(valid(buf));
    CHECK(view_valid(buf));
}

TEST_CASE("CPROF reader rejects byte-swapped (big-endian) magic", "[perf][diag][cprof-version]")
{
    PerfCaptureFixture fx;
    auto buf = make_capture(&fx.alloc);
    auto* hdr = reinterpret_cast<crd::perf::CprofHeader*>(buf.data());

    // CPROF is little-endian only (see capture.hpp). A big-endian device would write the magic byte-reversed;
    // that must be rejected as a foreign format, not misparsed. //TEETH-MAGIC (clear the magic check -> flips).
    const crd::u32 m = crd::perf::kCprofMagic;
    const crd::u32 be = (m >> 24) | ((m >> 8) & 0x0000FF00U) | ((m << 8) & 0x00FF0000U) | (m << 24);
    REQUIRE(be != m); // 'CPRO' is not a palindrome
    hdr->magic = be;
    CHECK_FALSE(valid(buf));
    CHECK_FALSE(view_valid(buf));

    hdr->magic = m; // restore
    CHECK(valid(buf));
}

TEST_CASE("CPROF reader tolerates an unknown flag bit (additive-section forward-compat)",
          "[perf][diag][cprof-version]")
{
    PerfCaptureFixture fx;
    auto buf = make_capture(&fx.alloc);
    auto* hdr = reinterpret_cast<crd::perf::CprofHeader*>(buf.data());
    REQUIRE(valid(buf));

    // A newer writer may set a flag bit this reader does not know, signalling an OPTIONAL appended section. The
    // reader must IGNORE it (only kCprofFlagCorrelation is consulted) and still accept the base capture -- this is
    // the "new files stay readable by old readers" contract. It must NOT be quietly tightened into a rejection.
    const crd::u64 unknown_bit = 0x8000'0000'0000'0000ULL;
    REQUIRE((unknown_bit & crd::perf::kCprofFlagCorrelation) == 0U);
    hdr->flags |= unknown_bit;
    CHECK(valid(buf));
    CHECK(view_valid(buf));
}

TEST_CASE("CPROF reader rejects a pinned struct-size mismatch", "[perf][diag][cprof-version]")
{
    PerfCaptureFixture fx;
    auto buf = make_capture(&fx.alloc);
    auto* hdr = reinterpret_cast<crd::perf::CprofHeader*>(buf.data());
    REQUIRE(valid(buf));

    // The header carries sizeof(Sample)/sizeof(FrameRecord) so a build with a different POD layout is rejected as
    // an ABI mismatch rather than misread field-by-field.
    const crd::u64 good_sample = hdr->sample_struct_size;
    hdr->sample_struct_size = good_sample + 1U;
    CHECK_FALSE(valid(buf));
    hdr->sample_struct_size = good_sample; // restore

    const crd::u64 good_frame = hdr->frame_record_size;
    hdr->frame_record_size = good_frame + 1U;
    CHECK_FALSE(valid(buf));
    hdr->frame_record_size = good_frame; // restore
    CHECK(valid(buf));
}

TEST_CASE("load_capture_from_file returns empty on failure (never truncated)", "[perf][diag][cprof-load]")
{
    PerfCaptureFixture fx;
    // The happy path (save file -> load -> export identical) is observed end-to-end by the cprof_export byte-identity
    // oracle in test_diag_perfetto_export.cpp; here we pin the failure contract the CLI relies on: a null path and a
    // nonexistent path each yield an EMPTY buffer, never a partial or garbage one.
    auto from_null = crd::perf::load_capture_from_file(nullptr, &fx.alloc);
    CHECK(from_null.size() == 0U);
    auto from_missing = crd::perf::load_capture_from_file("this_capture_does_not_exist_6c_e.cprof", &fx.alloc);
    CHECK(from_missing.size() == 0U);
    // An empty buffer is (correctly) not a valid capture.
    CHECK_FALSE(valid(from_missing));
}

#endif // CRD_PERF_ENABLED
