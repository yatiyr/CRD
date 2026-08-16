// vulkan_validation_capture.cpp — RET-4: the validation capture, ported from crd-rhi onto gpu-context (ADR-0105).

#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace crd::gpu
{

namespace
{

constexpr crd::usize kMaxRecordedMessages = 256;

[[nodiscard]] ValidationSeverity to_severity(VkDebugUtilsMessageSeverityFlagBitsEXT s) noexcept
{
    if ((s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) { return ValidationSeverity::Error; }
    if ((s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) { return ValidationSeverity::Warning; }
    return ValidationSeverity::Info;
}

} // namespace

struct ValidationCapture::Impl
{
    VkInstance                          instance   = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT            messenger  = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_fn = nullptr;

    std::atomic<crd::u32> errors{0};
    std::atomic<crd::u32> warnings{0};
    std::atomic<crd::u32> infos{0};

    mutable std::mutex                        records_mu;
    crd::containers::Array<ValidationMessage> records{};
    crd::containers::Array<crd::i32>          whitelisted{};
    crd::u32                                  records_dropped = 0;
};

namespace
{

VKAPI_ATTR VkBool32 VKAPI_CALL capture_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                                void*                                       user_data)
{
    if (user_data == nullptr || callback_data == nullptr) { return VK_FALSE; }
    auto* impl = static_cast<ValidationCapture::Impl*>(user_data);

    const crd::i32 msg_id      = callback_data->messageIdNumber;
    bool           whitelisted = false;
    {
        std::lock_guard<std::mutex> lk(impl->records_mu);
        for (const auto& w : impl->whitelisted)
        {
            if (w == msg_id)
            {
                whitelisted = true;
                break;
            }
        }
    }

    const auto sev = to_severity(severity);
    if (!whitelisted)
    {
        switch (sev)
        {
        case ValidationSeverity::Error: impl->errors.fetch_add(1, std::memory_order_relaxed); break;
        case ValidationSeverity::Warning: impl->warnings.fetch_add(1, std::memory_order_relaxed); break;
        case ValidationSeverity::Info: impl->infos.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(impl->records_mu);
        if (impl->records.size() < kMaxRecordedMessages)
        {
            ValidationMessage rec{};
            rec.severity          = sev;
            rec.message_id_number = msg_id;
            rec.message_text =
                crd::containers::String(callback_data->pMessage != nullptr ? callback_data->pMessage : "(null)");
            impl->records.push_back(std::move(rec));
        }
        else { ++impl->records_dropped; }
    }

    return VK_FALSE; // per the Vulkan spec: a capture messenger must return VK_FALSE
}

} // namespace

ValidationCapture::ValidationCapture(VulkanGpuContext& ctx) : m_impl(std::make_unique<Impl>())
{
    m_impl->instance = ctx.vk_instance();
    if (m_impl->instance == VK_NULL_HANDLE) { return; }

    const auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_impl->instance, "vkCreateDebugUtilsMessengerEXT"));
    m_impl->destroy_fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_impl->instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (create_fn == nullptr || m_impl->destroy_fn == nullptr) { return; } // debug_utils unavailable — capture silent

    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = &capture_callback;
    info.pUserData       = m_impl.get();
    (void)create_fn(m_impl->instance, &info, nullptr, &m_impl->messenger);
}

ValidationCapture::~ValidationCapture()
{
    if (m_impl == nullptr) { return; }
    if (m_impl->messenger != VK_NULL_HANDLE && m_impl->destroy_fn != nullptr)
    {
        m_impl->destroy_fn(m_impl->instance, m_impl->messenger, nullptr);
        m_impl->messenger = VK_NULL_HANDLE;
    }
}

crd::u32 ValidationCapture::error_count() const noexcept
{
    return m_impl ? m_impl->errors.load(std::memory_order_relaxed) : 0U;
}

crd::u32 ValidationCapture::warning_count() const noexcept
{
    return m_impl ? m_impl->warnings.load(std::memory_order_relaxed) : 0U;
}

crd::u32 ValidationCapture::info_count() const noexcept
{
    return m_impl ? m_impl->infos.load(std::memory_order_relaxed) : 0U;
}

crd::containers::ConstSpan<ValidationMessage> ValidationCapture::messages() const noexcept
{
    if (m_impl == nullptr) { return {}; }
    std::lock_guard<std::mutex> lk(m_impl->records_mu);
    return crd::containers::make_span(m_impl->records.data(), m_impl->records.size());
}

void ValidationCapture::whitelist(crd::i32 message_id_number)
{
    if (m_impl == nullptr) { return; }
    std::lock_guard<std::mutex> lk(m_impl->records_mu);
    m_impl->whitelisted.push_back(message_id_number);
}

void ValidationCapture::reset() noexcept
{
    if (m_impl == nullptr) { return; }
    m_impl->errors.store(0, std::memory_order_relaxed);
    m_impl->warnings.store(0, std::memory_order_relaxed);
    m_impl->infos.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_impl->records_mu);
    m_impl->records.clear();
    m_impl->records_dropped = 0;
}

crd::u32 validation_layer_spec_version() noexcept
{
    // `vkEnumerateInstanceLayerProperties` is a global command (no instance needed). A fixed stack array avoids an
    // allocator here (no hidden malloc); 64 covers any real machine's instance-layer count, and if the loader reports
    // more (VK_INCOMPLETE) the validation layer sorts early, so scanning the first 64 still finds it.
    crd::u32 count = 0U;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0U) { return 0U; }
    constexpr crd::u32 max_layers = 64U;
    if (count > max_layers) { count = max_layers; }
    VkLayerProperties props[max_layers];
    const VkResult    r = vkEnumerateInstanceLayerProperties(&count, props);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { return 0U; }
    for (crd::u32 i = 0U; i < count; ++i)
    {
        if (std::strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) { return props[i].specVersion; }
    }
    return 0U;
}

} // namespace crd::gpu
