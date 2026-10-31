// adapter.cpp -- the driver version formats and the process's opened-adapter log (HW-RESILIENCE.2).
#include <crd/gpu/adapter.hpp>

#include <cstdio>
#include <cstring>

namespace crd::gpu
{

namespace
{

constexpr crd::u32 kVendorNvidia = 0x10DEU;
constexpr crd::u32 kVendorIntel = 0x8086U;

[[nodiscard]] bool same_adapter(const GpuAdapter& a, const GpuAdapter& b) noexcept
{
    return a.vendor == b.vendor && a.device == b.device && a.driver == b.driver && a.api == b.api &&
           std::strcmp(a.backend, b.backend) == 0 && std::strcmp(a.name, b.name) == 0 &&
           std::strcmp(a.driver_version, b.driver_version) == 0;
}

} // namespace

void format_windows_driver_version(crd::u64 version, char (&out)[kAdapterDriverVersionBytes]) noexcept
{
    (void)std::snprintf(out, sizeof(out), "%u.%u.%u.%u", static_cast<unsigned>((version >> 48U) & 0xFFFFU),
                        static_cast<unsigned>((version >> 32U) & 0xFFFFU),
                        static_cast<unsigned>((version >> 16U) & 0xFFFFU), static_cast<unsigned>(version & 0xFFFFU));
}

void format_vulkan_driver_version(crd::u32 vendor, crd::u32 version, bool windows,
                                  char (&out)[kAdapterDriverVersionBytes]) noexcept
{
    if (vendor == kVendorNvidia)
    {
        (void)std::snprintf(out, sizeof(out), "%u.%u.%u.%u", (version >> 22U) & 0x3FFU, (version >> 14U) & 0xFFU,
                            (version >> 6U) & 0xFFU, version & 0x3FU);
        return;
    }
    if (vendor == kVendorIntel && windows)
    {
        (void)std::snprintf(out, sizeof(out), "%u.%u", version >> 14U, version & 0x3FFFU);
        return;
    }
    // VK_MAKE_API_VERSION: variant 3 bits, major 7, minor 10, patch 12.
    (void)std::snprintf(out, sizeof(out), "%u.%u.%u", (version >> 22U) & 0x7FU, (version >> 12U) & 0x3FFU,
                        version & 0xFFFU);
}

bool OpenedAdapters::note(const GpuAdapter& adapter) noexcept
{
    if (!adapter.known)
    {
        return true;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (crd::u32 i = 0U; i < m_count; ++i)
    {
        if (same_adapter(m_adapters[i], adapter))
        {
            return true;
        }
    }
    if (m_count == kMaxOpenedAdapters)
    {
        return false;
    }
    m_adapters[m_count] = adapter;
    ++m_count;
    if (m_listener != nullptr)
    {
        m_listener(adapter);
    }
    return true;
}

void OpenedAdapters::listen(Listener listener) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = listener;
    if (listener == nullptr)
    {
        return;
    }
    for (crd::u32 i = 0U; i < m_count; ++i)
    {
        listener(m_adapters[i]);
    }
}

crd::u32 OpenedAdapters::count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_count;
}

GpuAdapter OpenedAdapters::at(crd::u32 index) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return index < m_count ? m_adapters[index] : GpuAdapter{};
}

OpenedAdapters& opened_adapters() noexcept
{
    static OpenedAdapters log;
    return log;
}

} // namespace crd::gpu
