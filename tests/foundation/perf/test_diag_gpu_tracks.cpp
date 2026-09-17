// DIAG.6b(c): separate per-(device,queue) GPU tracks. GPU work no longer lands on one "gpu" track aliased onto the
// registering CPU thread -- each distinct (device_id, queue_id) key gets its own profiler slot (named "gpu d<dev>
// q<queue>", correlation-ready), routed by a GpuSampleKind onto Category Gpu/Io/Wait. The table is bounded by
// kMaxGpuTracks; over-cap keys fold onto a shared "gpu overflow" track and bump gpu_track_overflow_count() -- never
// dropped silently. Oracles: the latent-alias regression (the caller's "main" track is never renamed/reused for GPU
// work); three keys -> three distinct tracks whose samples carry the right (device,queue)+category after a CPROF
// round-trip; idempotent per-key registration; and the overflow fold. Proof lanes: win-debug + win-asan.
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
crd::perf::Sample make_gpu_sample(crd::i64 begin_ns, crd::perf::NameId pass) noexcept
{
    crd::perf::Sample s{};
    s.begin_ns   = begin_ns;
    s.end_ns     = begin_ns + 100;
    s.name_id    = pass.value;
    s.color_rgba = 0U;
    s.depth      = 0U;
    // category / begin_thread / end_thread are forced by emit_gpu_sample_on from the kind + resolved track.
    s.fiber_id = 0U;
    return s;
}

// Dense capture-side index of the track named `name`, or 0xFFFFFFFF if absent.
crd::u32 find_track(const crd::perf::CaptureView& v, const char* name) noexcept
{
    for (crd::u32 i = 0U; i < v.thread_count(); ++i)
    {
        const char* const n = v.thread_name(i);
        if (n != nullptr && std::strcmp(n, name) == 0)
        {
            return i;
        }
    }
    return 0xFFFF'FFFFU;
}
} // namespace

TEST_CASE("gpu-tracks: registering a GPU track never renames or reuses the caller's own thread", "[perf][diag][gpu-tracks]")
{
    crd::perf::init({});
    const crd::u8 caller = crd::perf::current_thread_index(); // the test thread == "main", index 0
    REQUIRE(caller != 0xFFU);
    REQUIRE(crd::perf::gpu_thread_index() == 0xFFU); // no GPU track yet

    // Emitting a GPU sample registers the default execution track (device 0, queue 0). Before (c) this took the TLS
    // register_thread refresh path and ALIASED the gpu track onto the caller's "main" track.
    const crd::perf::NameId pass = crd::perf::intern_name("frame");
    crd::perf::emit_gpu_sample(make_gpu_sample(1000, pass));

    const crd::u8 gpu = crd::perf::gpu_thread_index();
    REQUIRE(gpu != 0xFFU);
    // TEETH: swap register_external_track back to register_thread in gpu_scope and this flips (gpu == caller).
    CHECK(gpu != caller);
    // The caller's own track keeps its identity -- it was NOT re-owned to a "gpu ..." name.
    const char* const caller_name = crd::perf::thread_samples(caller).name;
    REQUIRE(caller_name != nullptr);
    CHECK(std::strcmp(caller_name, "main") == 0);
    const char* const gpu_name = crd::perf::thread_samples(gpu).name;
    REQUIRE(gpu_name != nullptr);
    CHECK(std::strcmp(gpu_name, "gpu d0 q0") == 0);

    crd::perf::shutdown();
}

TEST_CASE("gpu-tracks: three (device,queue) keys land on three distinct tracks and carry their identity + category",
          "[perf][diag][gpu-tracks]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-tracks-cap"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("pass");

    struct Emit
    {
        crd::perf::GpuTrackKey   key;
        crd::perf::GpuSampleKind kind;
        crd::u8                  expect_category;
        const char*              track_name;
    };
    const Emit plan[] = {
        {{0U, 0U}, crd::perf::GpuSampleKind::Execution, static_cast<crd::u8>(crd::perf::Category::Gpu), "gpu d0 q0"},
        {{0U, 1U}, crd::perf::GpuSampleKind::Transfer, static_cast<crd::u8>(crd::perf::Category::Io), "gpu d0 q1"},
        {{1U, 0U}, crd::perf::GpuSampleKind::QueueWait, static_cast<crd::u8>(crd::perf::Category::Wait), "gpu d1 q0"},
    };

    // Emit 3 samples per key; queue_id is stamped == begin_ns so we can join a record back to its sample after save.
    constexpr crd::u32 kPer = 3U;
    for (const auto& e : plan)
    {
        for (crd::u32 j = 0U; j < kPer; ++j)
        {
            const crd::i64 begin = static_cast<crd::i64>(1000U + e.key.device_id * 100U + e.key.queue_id * 10U + j);
            crd::perf::emit_gpu_sample_on(make_gpu_sample(begin, pass), e.key, e.kind);
        }
    }

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    REQUIRE(view.correlation_count() == static_cast<crd::u32>(kPer) * 3U);

    crd::u32 indices[3];
    for (crd::u32 t = 0U; t < 3U; ++t)
    {
        const crd::u32 idx = find_track(view, plan[t].track_name);
        REQUIRE(idx != 0xFFFF'FFFFU); // TEETH: collapse the lookup to entry 0 and two of these vanish
        indices[t] = idx;

        const auto samples = view.thread_samples(idx);
        REQUIRE(samples.size() == kPer);
        for (crd::u32 j = 0U; j < kPer; ++j)
        {
            CHECK(samples[j].category == plan[t].expect_category);
            const auto* rec = view.correlation_for(idx, j);
            REQUIRE(rec != nullptr);
            CHECK(rec->device_id == plan[t].key.device_id);
            CHECK(rec->queue_id == plan[t].key.queue_id);
            CHECK(rec->pass_id == pass.value);
        }
    }
    // The three tracks are genuinely distinct slots.
    CHECK(indices[0] != indices[1]);
    CHECK(indices[0] != indices[2]);
    CHECK(indices[1] != indices[2]);

    crd::perf::shutdown();
}

TEST_CASE("gpu-tracks: the same (device,queue) key is idempotent -- one track, all its samples", "[perf][diag][gpu-tracks]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-tracks-idem"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("dispatch");

    const crd::perf::GpuTrackKey key{2U, 3U};
    for (crd::u32 j = 0U; j < 5U; ++j)
    {
        crd::perf::emit_gpu_sample_on(make_gpu_sample(2000 + static_cast<crd::i64>(j), pass), key,
                                      crd::perf::GpuSampleKind::Execution);
    }

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    // Exactly one "gpu d2 q3" track, holding all 5 samples (not 5 tracks of 1).
    crd::u32 matches = 0U;
    crd::u32 idx     = 0xFFFF'FFFFU;
    for (crd::u32 i = 0U; i < view.thread_count(); ++i)
    {
        const char* const n = view.thread_name(i);
        if (n != nullptr && std::strcmp(n, "gpu d2 q3") == 0)
        {
            ++matches;
            idx = i;
        }
    }
    CHECK(matches == 1U);
    REQUIRE(idx != 0xFFFF'FFFFU);
    CHECK(view.thread_samples(idx).size() == 5U);

    crd::perf::shutdown();
}

TEST_CASE("gpu-tracks: keys beyond kMaxGpuTracks fold onto the overflow track and are counted", "[perf][diag][gpu-tracks]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-tracks-overflow"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("op");

    CHECK(crd::perf::gpu_track_overflow_count() == 0U);

    // Fill exactly kMaxGpuTracks distinct keys -- each gets its own track, no overflow yet.
    for (crd::u32 q = 0U; q < crd::perf::kMaxGpuTracks; ++q)
    {
        crd::perf::emit_gpu_sample_on(make_gpu_sample(3000 + static_cast<crd::i64>(q), pass),
                                      crd::perf::GpuTrackKey{0U, q}, crd::perf::GpuSampleKind::Execution);
    }
    CHECK(crd::perf::gpu_track_overflow_count() == 0U);

    // A key beyond the cap folds onto the shared "gpu overflow" track. The counter is per FOLDED SAMPLE, not per key:
    // over-cap keys are not tabled, so three emits on the same over-cap key fold three times.
    const crd::perf::GpuTrackKey over{0U, crd::perf::kMaxGpuTracks};
    crd::perf::emit_gpu_sample_on(make_gpu_sample(9999, pass), over, crd::perf::GpuSampleKind::Execution);
    CHECK(crd::perf::gpu_track_overflow_count() == 1U);
    crd::perf::emit_gpu_sample_on(make_gpu_sample(9998, pass), over, crd::perf::GpuSampleKind::Execution);
    crd::perf::emit_gpu_sample_on(make_gpu_sample(9997, pass), over, crd::perf::GpuSampleKind::Execution);
    CHECK(crd::perf::gpu_track_overflow_count() == 3U);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    // The overflow sample is not lost: a "gpu overflow" track exists with the folded sample.
    const crd::u32 ov = find_track(view, "gpu overflow");
    REQUIRE(ov != 0xFFFF'FFFFU);
    CHECK(view.thread_samples(ov).size() >= 1U);

    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
