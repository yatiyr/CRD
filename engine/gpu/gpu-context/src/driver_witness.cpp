#include <crd/gpu/driver_witness.hpp>

#include <atomic>

namespace crd::gpu
{
namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<crd::u64>    g_calls[kDriverCallCount]{};
std::atomic<crd::u64>    g_divergences[kDriverCallCount]{};
std::atomic<bool>        g_first_claimed{false};
std::atomic<const char*> g_first_operation{nullptr};
std::atomic<crd::u32>    g_first_fields{0U};
std::atomic<crd::u8>     g_first_call{0U};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

const char* driver_call_name(DriverCall c) noexcept
{
    switch (c) // no default: every kind is named
    {
    case DriverCall::CreateInstance: return "create-instance";
    case DriverCall::CreateDevice:   return "create-device";
    case DriverCall::CompileShader:  return "compile-shader";
    case DriverCall::Submit:         return "submit";
    case DriverCall::Present:        return "present";
    }
    return "unknown";
}

void note_driver_call(DriverCall kind, const char* operation, crd::u32 changed) noexcept
{
    const auto index = static_cast<crd::usize>(kind);
    if (index >= kDriverCallCount)
    {
        return;
    }
    g_calls[index].fetch_add(1U, std::memory_order_relaxed);
    if (changed == 0U)
    {
        return;
    }
    g_divergences[index].fetch_add(1U, std::memory_order_relaxed);
    // One thread claims the first divergence; it stores the fields and kind before it publishes the label, so a
    // reader that sees the label sees the claimant's fields.
    if (!g_first_claimed.exchange(true, std::memory_order_acq_rel))
    {
        g_first_fields.store(changed, std::memory_order_relaxed);
        g_first_call.store(static_cast<crd::u8>(kind), std::memory_order_relaxed);
        g_first_operation.store(operation != nullptr ? operation : "unnamed driver call", std::memory_order_release);
    }
}

DriverWitnessReport driver_witness() noexcept
{
    DriverWitnessReport out;
    out.first_operation = g_first_operation.load(std::memory_order_acquire);
    if (out.first_operation != nullptr)
    {
        out.first_fields = g_first_fields.load(std::memory_order_relaxed);
        out.first_call   = static_cast<DriverCall>(g_first_call.load(std::memory_order_relaxed));
    }
    for (crd::usize i = 0U; i < kDriverCallCount; ++i)
    {
        out.calls[i]       = g_calls[i].load(std::memory_order_relaxed);
        out.divergences[i] = g_divergences[i].load(std::memory_order_relaxed);
    }
    return out;
}

} // namespace crd::gpu
