// device_check.cpp -- the process's GPU device checks (HW-RESILIENCE.3). See crd/gpu/device_check.hpp.
#include <crd/core/fault_seam.hpp>
#include <crd/gpu/device_check.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace crd::gpu
{

namespace
{

[[nodiscard]] bool same_adapter(const GpuAdapter& a, const GpuAdapter& b) noexcept
{
    return a.vendor == b.vendor && a.device == b.device && a.driver == b.driver && a.api == b.api &&
           std::strcmp(a.backend, b.backend) == 0 && std::strcmp(a.name, b.name) == 0 &&
           std::strcmp(a.driver_version, b.driver_version) == 0;
}

[[nodiscard]] bool same_selection(const DeviceSelection& a, const DeviceSelection& b) noexcept
{
    const bool same_failure =
        (a.failed == nullptr) == (b.failed == nullptr) && (a.failed == nullptr || std::strcmp(a.failed, b.failed) == 0);
    return a.status == b.status && same_adapter(a.selected, b.selected) && same_adapter(a.runs, b.runs) &&
           a.runs.known == b.runs.known && same_failure && a.injected == b.injected &&
           (a.unchecked == nullptr) == (b.unchecked == nullptr) && a.adapters_failed == b.adapters_failed;
}

constexpr crd::u32 kUnwritten = 0xFFFFFFFFU; // a NaN no output of the kernel has: an output it never wrote fails

std::atomic<std::FILE*> g_demotion_out{nullptr};

void print_demotion(const DeviceSelection& selection)
{
    std::FILE* const out = g_demotion_out.load(std::memory_order_acquire);
    if (out == nullptr || selection.status == DeviceCheckStatus::Passed ||
        selection.status == DeviceCheckStatus::NotRun)
    {
        return;
    }
    char line[640] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)format_device_demotion(selection, line, sizeof(line));
    (void)std::fprintf(out, "%s\n", line);
    (void)std::fflush(out);
}

} // namespace

const char* device_check_status_name(DeviceCheckStatus status) noexcept
{
    switch (status)
    {
        case DeviceCheckStatus::NotRun:
            return "not-run";
        case DeviceCheckStatus::Passed:
            return "passed";
        case DeviceCheckStatus::Unchecked:
            return "unchecked";
        case DeviceCheckStatus::Demoted:
            return "demoted";
        case DeviceCheckStatus::NoDevicePassed:
            return "no-device-passed";
    }
    return "unknown";
}

bool DeviceChecks::find(const GpuAdapter& adapter, crd::u32 ordinal, DeviceCheck& out) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (crd::u32 i = 0U; i < m_check_count; ++i)
    {
        if (m_checks[i].ordinal == ordinal && same_adapter(m_checks[i].adapter, adapter))
        {
            out = m_checks[i];
            return true;
        }
    }
    return false;
}

bool DeviceChecks::note_check(const DeviceCheck& check) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (crd::u32 i = 0U; i < m_check_count; ++i)
    {
        if (m_checks[i].ordinal == check.ordinal && same_adapter(m_checks[i].adapter, check.adapter))
        {
            return true;
        }
    }
    if (m_check_count == kMaxChecks)
    {
        return false;
    }
    m_checks[m_check_count] = check;
    ++m_check_count;
    return true;
}

void DeviceChecks::note_selection(const DeviceSelection& selection) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    crd::u32 slot = 0U;
    while (slot < m_selection_count &&
           std::strcmp(m_selections[slot].selected.backend, selection.selected.backend) != 0)
    {
        ++slot;
    }
    if (slot < m_selection_count && same_selection(m_selections[slot], selection))
    {
        return;
    }
    if (slot == kMaxSelections)
    {
        return; // more backends than GpuBackend has: nothing to keep
    }
    m_selections[slot] = selection;
    if (slot == m_selection_count)
    {
        ++m_selection_count;
    }
    if (m_listener != nullptr)
    {
        m_listener(selection);
    }
}

void DeviceChecks::listen(Listener listener) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = listener;
    if (listener == nullptr)
    {
        return;
    }
    for (crd::u32 i = 0U; i < m_selection_count; ++i)
    {
        listener(m_selections[i]);
    }
}

crd::u32 DeviceChecks::check_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_check_count;
}

DeviceCheck DeviceChecks::check_at(crd::u32 index) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return index < m_check_count ? m_checks[index] : DeviceCheck{};
}

crd::u32 DeviceChecks::selection_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_selection_count;
}

DeviceSelection DeviceChecks::selection_at(crd::u32 index) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return index < m_selection_count ? m_selections[index] : DeviceSelection{};
}

void DeviceChecks::forget() noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_check_count = 0U;
    m_selection_count = 0U;
}

DeviceChecks& device_checks() noexcept
{
    static DeviceChecks log;
    return log;
}

DeviceCheck check_device_once(const GpuAdapter& adapter, crd::u32 ordinal, DeviceCheckOutcome (*run)(void* user),
                              void* user)
{
    static std::mutex one_at_a_time;
    const std::lock_guard<std::mutex> lock(one_at_a_time);
    DeviceCheck check;
    if (device_checks().find(adapter, ordinal, check))
    {
        return check;
    }
    check.adapter = adapter;
    check.ordinal = ordinal;
    const auto start = std::chrono::steady_clock::now();
    check.injected = crd::consult_device_check_fault(adapter.backend, adapter.name);
    const DeviceCheckOutcome outcome = check.injected ? DeviceCheckOutcome{"injected", nullptr} : run(user);
    check.failed = outcome.failed;
    check.unchecked = outcome.failed == nullptr ? outcome.unchecked : nullptr;
    check.nanoseconds = static_cast<crd::u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    (void)device_checks().note_check(check);
    return check;
}

crd::usize format_device_demotion(const DeviceSelection& selection, char* out, crd::usize bytes) noexcept
{
    const char* const why = selection.unchecked != nullptr ? selection.unchecked : "?";
    if (selection.status == DeviceCheckStatus::Unchecked)
    {
        const int n =
            std::snprintf(out, bytes,
                          "[crd] GPU device path %s on %s ran no known-answer check (%s); it runs unchecked "
                          "(%s, %u adapters failed)",
                          selection.selected.backend, selection.selected.name, why,
                          device_check_status_name(selection.status), static_cast<unsigned>(selection.adapters_failed));
        return n < 0 ? 0U : static_cast<crd::usize>(n);
    }
    const char* const runs = selection.runs.known ? selection.runs.name : "no device";
    const bool unchecked = selection.unchecked != nullptr;
    const int n =
        std::snprintf(out, bytes,
                      "[crd] GPU device path %s on %s failed its known answers in %s%s; it runs %s%s%s%s (%s, "
                      "%u adapters failed)",
                      selection.selected.backend, selection.selected.name,
                      selection.failed != nullptr ? selection.failed : "?", selection.injected ? " (fault seam)" : "",
                      runs, unchecked ? " unchecked (" : "", unchecked ? why : "", unchecked ? ")" : "",
                      device_check_status_name(selection.status), static_cast<unsigned>(selection.adapters_failed));
    return n < 0 ? 0U : static_cast<crd::usize>(n);
}

void report_device_demotions(std::FILE* out) noexcept
{
    g_demotion_out.store(out, std::memory_order_release);
    device_checks().listen(out != nullptr ? &print_demotion : nullptr);
}

const char* run_device_check(IComputeContext& compute, ComputePipeline* pipeline, const DeviceCheckProgram& program)
{
    if (pipeline == nullptr)
    {
        return "build";
    }
    constexpr crd::u32 buffer_count = 3U;
    const crd::u64 bytes[buffer_count] = {program.input.size() * sizeof(float),
                                          crd::u64{program.floats} * sizeof(float),
                                          crd::u64{program.words} * sizeof(crd::u32)};
    std::unique_ptr<ComputeBuffer> dev[buffer_count];
    std::unique_ptr<ComputeBuffer> up[buffer_count];
    std::unique_ptr<ComputeBuffer> back[buffer_count];
    for (crd::u32 b = 0U; b < buffer_count; ++b)
    {
        const crd::u32 usage = compute_usage::storage | compute_usage::transfer_dst | compute_usage::transfer_src;
        dev[b] = compute.create_buffer(bytes[b], usage, ComputeMemory::GpuOnly);
        up[b] = compute.create_buffer(bytes[b], compute_usage::transfer_src, ComputeMemory::CpuToGpu);
        if (b != 0U)
        {
            back[b] = compute.create_buffer(bytes[b], compute_usage::transfer_dst, ComputeMemory::GpuToCpu);
        }
        if (dev[b] == nullptr || up[b] == nullptr || (b != 0U && back[b] == nullptr))
        {
            return "buffers";
        }
        void* const p = up[b]->map();
        if (p == nullptr)
        {
            return "buffers";
        }
        if (b == 0U)
        {
            std::memcpy(p, program.input.data(), static_cast<crd::usize>(bytes[b]));
        }
        else
        {
            auto* const words = static_cast<crd::u32*>(p);
            for (crd::u64 i = 0U; i < bytes[b] / sizeof(crd::u32); ++i)
            {
                words[i] = kUnwritten;
            }
        }
        up[b]->unmap();
    }

    ComputeRecorder& rec = compute.begin();
    for (crd::u32 b = 0U; b < buffer_count; ++b)
    {
        rec.copy(*up[b], *dev[b], 0U, 0U, bytes[b]);
        rec.barrier(*dev[b], ComputeAccess::TransferDst,
                    b == 0U ? ComputeAccess::ShaderRead : ComputeAccess::ShaderWrite);
    }
    ComputeBuffer* bindings[buffer_count] = {dev[0].get(), dev[1].get(), dev[2].get()};
    rec.dispatch(*pipeline, crd::containers::ConstSpan<ComputeBuffer*>(bindings, buffer_count), nullptr, 0U,
                 program.workgroups, 1U, 1U);
    for (crd::u32 b = 1U; b < buffer_count; ++b)
    {
        rec.barrier(*dev[b], ComputeAccess::ShaderWrite, ComputeAccess::TransferSrc);
        rec.copy(*dev[b], *back[b], 0U, 0U, bytes[b]);
    }
    compute.submit_and_wait();

    const auto* const floats = static_cast<const float*>(back[1]->map());
    const auto* const words = static_cast<const crd::u32*>(back[2]->map());
    const char* const failed = floats == nullptr || words == nullptr ? "buffers" : program.verify(floats, words);
    if (floats != nullptr)
    {
        back[1]->unmap();
    }
    if (words != nullptr)
    {
        back[2]->unmap();
    }
    return failed;
}

} // namespace crd::gpu
