// ---------------------------------------------------------------------------
// crd-rhi-vulkan -- VulkanProfilerBackend implementation (Detour D-003 v0d).
//
// Maps crd::perf::IProfilerGpuBackend to a Vulkan VkQueryPool. Query slot
// layout:
//
//   slot[ frame * (max_spans * 2) + span * 2 + 0 ]  = span begin tick
//   slot[ frame * (max_spans * 2) + span * 2 + 1 ]  = span end tick
//
//   frame is in [0, frames_in_flight); we wrap on frame_index.
//
// Per-frame lifecycle:
//
//   1. vulkan_profiler_begin_frame(cb, frame_index)
//        -> vkCmdResetQueryPool over this frame's slot range
//   2. backend->begin_frame(frame_index)
//        -> bookmark current slot; reset span counter
//   3. begin_span/end_span x N
//        -> vkCmdWriteTimestamp(BOTTOM_OF_PIPE) into the next pair of slots
//   4. backend->end_frame()
//        -> mark frame as "pending host readback"
//   5. resolve_completed_frames() [some frames later]
//        -> vkGetQueryPoolResults for each frame older than frames_in_flight;
//           convert ticks -> ns; emit_gpu_sample(...)
// ---------------------------------------------------------------------------

#include <crd/rhi/vulkan_profiler_backend.hpp>

#include <crd/core/assert.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>
#include <crd/rhi/vulkan_native.hpp>
#include <crd/time/clocks.hpp>

#include <vulkan/vulkan.h>

#include <atomic>
#include <vector>

namespace crd::rhi
{

namespace
{

class VulkanProfilerBackend final : public crd::perf::IProfilerGpuBackend
{
public:
    VulkanProfilerBackend(VkDevice device, VkPhysicalDevice phys, crd::u32 max_spans_per_frame,
                          crd::u32 frames_in_flight)
        : m_device(device)
        , m_max_spans_per_frame(max_spans_per_frame)
        , m_frames_in_flight(frames_in_flight)
        , m_slots_per_frame(max_spans_per_frame * 2U)
        , m_total_slots(max_spans_per_frame * 2U * frames_in_flight)
    {
        CRD_ASSERT_MSG(device != VK_NULL_HANDLE, "VulkanProfilerBackend: null device");
        CRD_ASSERT_MSG(max_spans_per_frame > 0U, "VulkanProfilerBackend: max_spans_per_frame must be > 0");
        CRD_ASSERT_MSG(frames_in_flight > 0U, "VulkanProfilerBackend: frames_in_flight must be > 0");

        // Query the GPU timestamp period (ns per tick). On modern desktop
        // hardware this is ~1.0 (one tick per ns); on mobile / some discrete
        // cards it can be 38, 52, or 80+.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        m_ns_per_tick = static_cast<crd::f64>(props.limits.timestampPeriod);

        // The graphics queue family must have timestamp support. We assume
        // queue family 0 here -- the rest of crd-rhi-vulkan pins a single
        // graphics queue (queue family index returned by
        // vulkan_graphics_queue_family_index). Verify.
        const crd::u64 valid_bits_mask =
            props.limits.timestampComputeAndGraphics ? ~0ULL : 0ULL;
        (void)valid_bits_mask; // assert-only; the assert below is conditional.
        CRD_ASSERT_MSG(props.limits.timestampComputeAndGraphics != 0,
                       "VulkanProfilerBackend: device lacks timestampComputeAndGraphics; "
                       "GPU profiling unsupported on this device");

        VkQueryPoolCreateInfo qpci{};
        qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = m_total_slots;
        [[maybe_unused]] const VkResult res = vkCreateQueryPool(m_device, &qpci, nullptr, &m_pool);
        CRD_ASSERT_MSG(res == VK_SUCCESS, "VulkanProfilerBackend: vkCreateQueryPool failed");

        // Reset the entire pool host-side (Vulkan 1.2+) so the first frame's
        // queries are in a known state. If host reset isn't available the
        // caller's first begin_frame will reset via cmd buffer instead.
        // We rely on the cmd-buffer path uniformly to keep the code simple.

        m_frames.resize(m_frames_in_flight);
        for (auto& f : m_frames)
        {
            f.span_names.reserve(m_max_spans_per_frame);
        }
    }

    ~VulkanProfilerBackend() override
    {
        if (m_pool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_pool, nullptr);
            m_pool = VK_NULL_HANDLE;
        }
    }

    VulkanProfilerBackend(const VulkanProfilerBackend&)            = delete;
    VulkanProfilerBackend& operator=(const VulkanProfilerBackend&) = delete;
    VulkanProfilerBackend(VulkanProfilerBackend&&)                 = delete;
    VulkanProfilerBackend& operator=(VulkanProfilerBackend&&)      = delete;

    void begin_frame(crd::u64 frame_index) noexcept override
    {
        const crd::u32 slot = static_cast<crd::u32>(frame_index % m_frames_in_flight);
        FrameData&      f   = m_frames[slot];
        f.frame_index       = frame_index;
        f.spans_used        = 0U;
        f.span_names.clear();
        f.pending           = true;
        f.host_begin_ns     = crd::time::MonotonicClock::now().ns_since_epoch();
        m_current_frame_slot = slot;
    }

    // Called by vulkan_profiler_begin_frame; emits vkCmdResetQueryPool
    // for the current frame's slot range on the supplied cmd buffer.
    void reset_queries_on_cmd_buffer(VkCommandBuffer cb) noexcept
    {
        const crd::u32 first = m_current_frame_slot * m_slots_per_frame;
        vkCmdResetQueryPool(cb, m_pool, first, m_slots_per_frame);
    }

    [[nodiscard]] crd::perf::GpuSpanHandle begin_span(void* cmd_buffer,
                                                     crd::perf::NameId name) noexcept override
    {
        if (cmd_buffer == nullptr)
        {
            return crd::perf::kInvalidGpuSpan;
        }
        FrameData& f = m_frames[m_current_frame_slot];
        if (f.spans_used >= m_max_spans_per_frame)
        {
            ++m_overflow_count;
            return crd::perf::kInvalidGpuSpan;
        }
        const crd::u32 span_idx       = f.spans_used++;
        const crd::u32 query_begin_at = m_current_frame_slot * m_slots_per_frame + span_idx * 2U;

        auto* vk_cb = vulkan_command_buffer_from_void(cmd_buffer);
        vkCmdWriteTimestamp(vk_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, query_begin_at);
        f.span_names.push_back(name);
        return crd::perf::GpuSpanHandle{span_idx};
    }

    void end_span(void* cmd_buffer, crd::perf::GpuSpanHandle span) noexcept override
    {
        if (cmd_buffer == nullptr || !span.is_valid())
        {
            return;
        }
        const crd::u32 query_end_at =
            m_current_frame_slot * m_slots_per_frame + span.value * 2U + 1U;
        auto* vk_cb = vulkan_command_buffer_from_void(cmd_buffer);
        vkCmdWriteTimestamp(vk_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, query_end_at);
    }

    void end_frame() noexcept override
    {
        // Nothing to do here -- begin_frame already marked the slot as
        // pending. This method exists so a future v0c-extended UI can hook
        // "frame ended" between submit and resolve.
    }

    void resolve_completed_frames() noexcept override
    {
        // Walk every slot; resolve any pending frame whose results are
        // ready. Use a NON-blocking read: VK_QUERY_RESULT_64_BIT, no
        // VK_QUERY_RESULT_WAIT_BIT. vkGetQueryPoolResults returns VK_NOT_READY
        // if the GPU hasn't retired yet; we skip and retry next call.
        for (crd::u32 i = 0U; i < m_frames_in_flight; ++i)
        {
            FrameData& f = m_frames[i];
            if (!f.pending || f.spans_used == 0U)
            {
                if (f.pending && f.spans_used == 0U)
                {
                    // No spans recorded this frame; mark resolved without
                    // touching the query pool.
                    f.pending = false;
                }
                continue;
            }

            const crd::u32 first   = i * m_slots_per_frame;
            const crd::u32 count   = f.spans_used * 2U;
            const VkDeviceSize stride = sizeof(crd::u64);
            crd::u64 ticks[2 * 256] = {};
            CRD_ASSERT_MSG(count <= 2U * 256U,
                           "VulkanProfilerBackend: span count exceeds local readback buffer");

            const VkResult res = vkGetQueryPoolResults(m_device, m_pool, first, count,
                                                      sizeof(crd::u64) * count, &ticks[0], stride,
                                                      VK_QUERY_RESULT_64_BIT);
            if (res == VK_NOT_READY)
            {
                continue; // try again next call
            }
            if (res != VK_SUCCESS)
            {
                // Hard error -- mark resolved (drop the data) to avoid
                // looping forever; bump overflow so UI flags the frame.
                f.pending = false;
                ++m_overflow_count;
                continue;
            }

            // Convert ticks to ns; emit Samples on the gpu track.
            for (crd::u32 s = 0U; s < f.spans_used; ++s)
            {
                const crd::u64 begin_ticks = ticks[s * 2U + 0U];
                const crd::u64 end_ticks   = ticks[s * 2U + 1U];
                if (end_ticks < begin_ticks)
                {
                    continue; // hardware tick rollover or wraparound; skip
                }
                const crd::u64 delta_ticks = end_ticks - begin_ticks;
                const crd::i64 delta_ns    = static_cast<crd::i64>(
                    static_cast<crd::f64>(delta_ticks) * m_ns_per_tick);

                // Anchor the GPU span to the CPU monotonic timeline: we
                // place the begin at the frame's host_begin_ns and stretch
                // by delta_ns. This is the simplest deterministic placement
                // -- proper CPU/GPU calibration is v0g UI work.
                crd::perf::Sample sample{};
                sample.begin_ns     = f.host_begin_ns;
                sample.end_ns       = f.host_begin_ns + delta_ns;
                sample.name_id      = f.span_names[s].value;
                sample.color_rgba   = 0U;
                sample.begin_thread = 0U; // overwritten by emit_gpu_sample with gpu track idx
                sample.end_thread   = 0U;
                sample.depth        = 0U;
                sample.category     = static_cast<crd::u8>(crd::perf::Category::Gpu);
                sample.fiber_id     = 0U;
                crd::perf::emit_gpu_sample(sample);
            }

            f.pending = false;
        }
    }

    [[nodiscard]] crd::f64 ns_per_tick() const noexcept override { return m_ns_per_tick; }

private:
    struct FrameData
    {
        crd::u64 frame_index    = ~0ULL;
        crd::u32 spans_used     = 0U;
        bool     pending        = false;
        crd::i64 host_begin_ns  = 0;
        std::vector<crd::perf::NameId> span_names;
    };

    static VkCommandBuffer vulkan_command_buffer_from_void(void* ptr) noexcept
    {
        // The macro passes &crd::rhi::CommandBuffer; we cast back and use the
        // public helper to extract the VkCommandBuffer handle. If a caller
        // passes a raw VkCommandBuffer directly, that's a misuse -- the
        // header pins "must be a crd::rhi::CommandBuffer*".
        auto* cb = static_cast<crd::rhi::CommandBuffer*>(ptr);
        return crd::rhi::vulkan_command_buffer(*cb);
    }

    VkDevice         m_device = VK_NULL_HANDLE;
    VkQueryPool      m_pool   = VK_NULL_HANDLE;
    crd::u32         m_max_spans_per_frame = 0U;
    crd::u32         m_frames_in_flight    = 0U;
    crd::u32         m_slots_per_frame     = 0U;
    crd::u32         m_total_slots         = 0U;
    crd::u32         m_current_frame_slot  = 0U;
    crd::f64         m_ns_per_tick         = 1.0;
    std::atomic<crd::u64> m_overflow_count{0U};
    std::vector<FrameData> m_frames;
};

} // namespace

[[nodiscard]] std::unique_ptr<crd::perf::IProfilerGpuBackend>
create_vulkan_profiler_backend(crd::rhi::Device& device, VulkanProfilerBackendDesc desc)
{
    auto backend = std::make_unique<VulkanProfilerBackend>(
        crd::rhi::vulkan_device(device),
        crd::rhi::vulkan_physical_device(device),
        desc.max_spans_per_frame,
        desc.frames_in_flight);
    return backend;
}

void vulkan_profiler_begin_frame(crd::perf::IProfilerGpuBackend& backend,
                                 crd::rhi::CommandBuffer&        cb,
                                 crd::u64                        frame_index) noexcept
{
    // Downcast: we know this backend is a VulkanProfilerBackend because it
    // was minted by create_vulkan_profiler_backend(). A non-Vulkan backend
    // here is a user error caught by the dynamic_cast returning nullptr.
    auto* vk = dynamic_cast<VulkanProfilerBackend*>(&backend);
    CRD_ASSERT_MSG(vk != nullptr,
                   "vulkan_profiler_begin_frame: backend was not created by "
                   "create_vulkan_profiler_backend");
    vk->begin_frame(frame_index);
    vk->reset_queries_on_cmd_buffer(crd::rhi::vulkan_command_buffer(cb));
}

} // namespace crd::rhi
