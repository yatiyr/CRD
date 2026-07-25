// crd-perf v0d -- GPU scope substrate validated via a mock backend.
//
// The real Vulkan backend (VulkanProfilerBackend) requires a live device;
// it gets exercised in the smoke binary + sandbox integration at v0h.
// Here we validate the crd-perf-side API surface:
//
//   - set_gpu_backend / current_gpu_backend round-trip
//   - GpuScopedRegion calls begin_span / end_span on the backend
//   - emit_gpu_sample writes onto the "gpu" thread's ring with category=Gpu
//   - CRD_PERF_GPU_SCOPE macro produces matched begin/end via the backend
//   - When no backend is installed, the macro is a no-op

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <vector>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

// Mock backend that records every call. Resolve flushes a deterministic
// "begin_ticks/end_ticks" pair derived from call ordering, so tests can
// verify the resolved Samples on the gpu track.
class MockGpuBackend final : public crd::perf::IProfilerGpuBackend
{
public:
    struct Span
    {
        crd::perf::NameId name;
        crd::u32          frame_slot;
        crd::u32          span_idx;
    };

    void begin_frame(crd::u64 frame_index) noexcept override
    {
        ++begin_frame_calls;
        m_current_frame = frame_index;
    }

    [[nodiscard]] crd::perf::GpuSpanHandle begin_span(void* cmd_buffer,
                                                    crd::perf::NameId name) noexcept override
    {
        ++begin_span_calls;
        last_cmd_buffer = cmd_buffer;
        const auto idx  = static_cast<crd::u32>(spans.size());
        // The interface declares these `noexcept`, so a vector reallocation throwing bad_alloc here would be
        // std::terminate rather than a test failure. Swallow — the recorded call counts still tell the test
        // what happened. (bugprone-exception-escape.)
        try
        {
            spans.push_back({name, static_cast<crd::u32>(m_current_frame), idx});
        }
        catch (...)
        {
            record_failures.fetch_add(1U, std::memory_order_relaxed);
        }
        return crd::perf::GpuSpanHandle{idx};
    }

    void end_span(void* /*cmd*/, crd::perf::GpuSpanHandle span) noexcept override
    {
        ++end_span_calls;
        if (span.is_valid())
        {
            try // `noexcept` override — see begin_span. (bugprone-exception-escape.)
            {
                ended_indices.push_back(span.value);
            }
            catch (...)
            {
                record_failures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }

    void end_frame() noexcept override { ++end_frame_calls; }

    void resolve_completed_frames() noexcept override
    {
        ++resolve_calls;
        for (const auto& s : spans)
        {
            crd::perf::Sample sample{};
            // Deterministic synthetic timing: 100 + idx*10 ns wide.
            sample.begin_ns     = 0;
            sample.end_ns       = 100 + static_cast<crd::i64>(s.span_idx) * 10;
            sample.name_id      = s.name.value;
            sample.color_rgba   = 0U;
            sample.begin_thread = 0U; // overwritten by emit_gpu_sample
            sample.end_thread   = 0U;
            sample.depth        = 0U;
            sample.category     = static_cast<crd::u8>(crd::perf::Category::Gpu);
            sample.fiber_id     = 0U;
            crd::perf::emit_gpu_sample(sample);
        }
        spans.clear();
    }

    [[nodiscard]] crd::f64 ns_per_tick() const noexcept override { return 1.0; }

    std::atomic<crd::u32> begin_frame_calls{0U};
    std::atomic<crd::u32> record_failures{0U}; // a `noexcept` override could not grow its vector
    std::atomic<crd::u32> begin_span_calls{0U};
    std::atomic<crd::u32> end_span_calls{0U};
    std::atomic<crd::u32> end_frame_calls{0U};
    std::atomic<crd::u32> resolve_calls{0U};
    void*                  last_cmd_buffer = nullptr;
    std::vector<Span>      spans;
    std::vector<crd::u32>  ended_indices;

private:
    crd::u64 m_current_frame = 0U;
};

[[nodiscard]] crd::u32 count_gpu_samples()
{
    const crd::u8 idx = crd::perf::gpu_thread_index();
    if (idx == 0xFFU)
    {
        return 0U;
    }
    const auto view = crd::perf::thread_samples(idx);
    crd::u32 n = 0U;
    for (crd::u32 i = 0U; i < view.size; ++i)
    {
        if (view.data[i].category == static_cast<crd::u8>(crd::perf::Category::Gpu))
        {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("set_gpu_backend round-trips; clear with nullptr",
          "[perf][gpu][backend]")
{
    PerfFixture fx;
    MockGpuBackend be;
    CHECK(crd::perf::current_gpu_backend() == nullptr);
    crd::perf::set_gpu_backend(&be);
    CHECK(crd::perf::current_gpu_backend() == &be);
    crd::perf::set_gpu_backend(nullptr);
    CHECK(crd::perf::current_gpu_backend() == nullptr);
}

TEST_CASE("set_gpu_backend registers a 'gpu' thread for resolved samples",
          "[perf][gpu][thread]")
{
    PerfFixture fx;
    CHECK(crd::perf::gpu_thread_index() == 0xFFU);
    MockGpuBackend be;
    crd::perf::set_gpu_backend(&be);
    const auto idx = crd::perf::gpu_thread_index();
    CHECK(idx != 0xFFU);
    crd::perf::set_gpu_backend(nullptr);
}

TEST_CASE("GpuScopedRegion calls begin_span and end_span on the backend",
          "[perf][gpu][scope]")
{
    PerfFixture fx;
    MockGpuBackend be;
    crd::perf::set_gpu_backend(&be);

    int dummy_cmd_buffer = 0;
    {
        const auto name = crd::perf::intern_name("shadow_pass");
        crd::perf::GpuScopedRegion s{&dummy_cmd_buffer, name};
    }
    CHECK(be.begin_span_calls.load() == 1U);
    CHECK(be.end_span_calls.load() == 1U);
    CHECK(be.last_cmd_buffer == &dummy_cmd_buffer);
    REQUIRE(be.ended_indices.size() == 1U);
    CHECK(be.ended_indices[0] == 0U);

    crd::perf::set_gpu_backend(nullptr);
}

TEST_CASE("CRD_PERF_GPU_SCOPE macro produces matched begin/end via backend",
          "[perf][gpu][macro]")
{
    PerfFixture fx;
    MockGpuBackend be;
    crd::perf::set_gpu_backend(&be);

    int dummy_cmd_buffer = 0;
    {
        CRD_PERF_GPU_SCOPE(&dummy_cmd_buffer, "geometry_pass");
        // ...
    }
    CHECK(be.begin_span_calls.load() == 1U);
    CHECK(be.end_span_calls.load() == 1U);

    crd::perf::set_gpu_backend(nullptr);
}

TEST_CASE("emit_gpu_sample lands on the gpu thread's ring with category=Gpu",
          "[perf][gpu][emit]")
{
    PerfFixture fx;
    MockGpuBackend be;
    crd::perf::set_gpu_backend(&be);

    int dummy_cmd_buffer = 0;
    {
        CRD_PERF_GPU_SCOPE(&dummy_cmd_buffer, "pass_a");
    }
    {
        CRD_PERF_GPU_SCOPE(&dummy_cmd_buffer, "pass_b");
    }

    // Drive resolve to flush samples.
    crd::perf::resolve_gpu_frames();

    CHECK(be.resolve_calls.load() == 1U);
    CHECK(count_gpu_samples() == 2U);

    const auto view = crd::perf::thread_samples(crd::perf::gpu_thread_index());
    REQUIRE(view.size == 2U);
    CHECK(view.data[0].category == static_cast<crd::u8>(crd::perf::Category::Gpu));
    CHECK(view.data[1].category == static_cast<crd::u8>(crd::perf::Category::Gpu));
    CHECK(view.data[0].begin_thread == crd::perf::gpu_thread_index());
    CHECK(view.data[0].end_thread   == crd::perf::gpu_thread_index());

    crd::perf::set_gpu_backend(nullptr);
}

TEST_CASE("CRD_PERF_GPU_SCOPE is a no-op when no backend is installed",
          "[perf][gpu][off]")
{
    PerfFixture fx;
    CHECK(crd::perf::current_gpu_backend() == nullptr);

    int dummy = 0;
    {
        CRD_PERF_GPU_SCOPE(&dummy, "no_backend_path");
    }
    // No backend -> no spans recorded anywhere; resolving is a safe no-op.
    crd::perf::resolve_gpu_frames();
    CHECK(count_gpu_samples() == 0U);
}

TEST_CASE("begin_frame / end_frame are forwarded on the backend",
          "[perf][gpu][lifecycle]")
{
    PerfFixture fx;
    MockGpuBackend be;
    crd::perf::set_gpu_backend(&be);

    crd::perf::IProfilerGpuBackend* api = crd::perf::current_gpu_backend();
    REQUIRE(api != nullptr);
    api->begin_frame(7U);
    api->end_frame();
    CHECK(be.begin_frame_calls.load() == 1U);
    CHECK(be.end_frame_calls.load() == 1U);

    crd::perf::set_gpu_backend(nullptr);
}

TEST_CASE("backend reports ns_per_tick", "[perf][gpu][calibration]")
{
    PerfFixture fx;
    MockGpuBackend be;
    crd::perf::set_gpu_backend(&be);
    CHECK(crd::perf::current_gpu_backend()->ns_per_tick() == 1.0);
    crd::perf::set_gpu_backend(nullptr);
}

#endif // CRD_PERF_ENABLED
