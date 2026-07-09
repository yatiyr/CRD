// vulkan_context.cpp — the headless Vulkan compute context (ADR-0099, v17-i-a). Raw Vulkan: instance (1.3, no surface)
// → discrete physical device → logical device with a compute queue + the cooperative-matrix / coopmat2 / fp16 /
// 16-bit-storage / memory-model feature chain (guarded by adapter support). No rendering, no swapchain.

#include <crd/gpu/vulkan_context.hpp>

#include <cstdint>
#include <cstring>

namespace crd::gpu
{
namespace
{

class VulkanGpuContextImpl final : public VulkanGpuContext
{
public:
    explicit VulkanGpuContextImpl(const GpuContextConfig& config) { init(config); }
    ~VulkanGpuContextImpl() override
    {
        if (m_device != VK_NULL_HANDLE) { vkDestroyDevice(m_device, nullptr); }
        if (m_instance != VK_NULL_HANDLE) { vkDestroyInstance(m_instance, nullptr); }
    }

    [[nodiscard]] bool             valid() const noexcept override { return m_valid; }
    [[nodiscard]] GpuBackend       backend() const noexcept override { return GpuBackend::Vulkan; }
    [[nodiscard]] const char*      adapter_name() const noexcept override { return m_name; }
    [[nodiscard]] VkInstance       vk_instance() const noexcept override { return m_instance; }
    [[nodiscard]] VkPhysicalDevice vk_physical_device() const noexcept override { return m_physical; }
    [[nodiscard]] VkDevice         vk_device() const noexcept override { return m_device; }
    [[nodiscard]] VkQueue          compute_queue() const noexcept override { return m_compute_queue; }
    [[nodiscard]] crd::u32         compute_family() const noexcept override { return m_compute_family; }
    [[nodiscard]] bool             cooperative_matrix2() const noexcept override { return m_coopmat2; }
    [[nodiscard]] bool             shader_int64() const noexcept override { return m_int64; }

private:
    void init(const GpuContextConfig& config)
    {
        VkApplicationInfo app{};
        app.sType         = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pEngineName   = "Cerid";
        app.apiVersion    = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{};
        ici.sType             = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo  = &app;
        const char* layers[]  = {"VK_LAYER_KHRONOS_validation"};
        if (config.enable_validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
        if (vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) { return; }

        std::uint32_t    npd = 16;
        VkPhysicalDevice pds[16];
        if (vkEnumeratePhysicalDevices(m_instance, &npd, pds) != VK_SUCCESS || npd == 0) { return; }
        m_physical = pds[0];
        for (std::uint32_t i = 0; i < npd; ++i)
        {
            VkPhysicalDeviceProperties pr{};
            vkGetPhysicalDeviceProperties(pds[i], &pr);
            if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { m_physical = pds[i]; break; }
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physical, &props);
        for (int i = 0; i < 255 && props.deviceName[i] != '\0'; ++i) { m_name[i] = props.deviceName[i]; } // m_name zero-init

        // Compute queue family — prefer a DEDICATED compute family (async vs a renderer), else any compute-capable one.
        std::uint32_t           nqf = 16;
        VkQueueFamilyProperties qf[16];
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &nqf, qf);
        std::uint32_t any_compute = UINT32_MAX;
        std::uint32_t dedicated   = UINT32_MAX;
        for (std::uint32_t i = 0; i < nqf; ++i)
        {
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U)
            {
                if (any_compute == UINT32_MAX) { any_compute = i; }
                if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U && dedicated == UINT32_MAX) { dedicated = i; }
            }
        }
        m_compute_family = (dedicated != UINT32_MAX) ? dedicated : any_compute;
        if (m_compute_family == UINT32_MAX) { return; }

        // Cooperative matrix (tensor cores) — enable coopmat + coopmat2 + fp16/16-bit/memory-model IF the adapter has them.
        std::uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &ne, nullptr);
        auto exts = std::make_unique<VkExtensionProperties[]>(ne == 0 ? 1 : ne);
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &ne, exts.get());
        bool has_cm1 = false;
        bool has_cm2 = false;
        for (std::uint32_t i = 0; i < ne; ++i)
        {
            if (std::strcmp(exts[i].extensionName, "VK_KHR_cooperative_matrix") == 0) { has_cm1 = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_cooperative_matrix2") == 0) { has_cm2 = true; }
        }
        m_coopmat2 = has_cm1 && has_cm2;

        // shaderInt64 — the geometry 60-bit Morton / LBVH paths need u64 in the shader. Enabled if the adapter has it.
        VkPhysicalDeviceFeatures avail_feats{};
        vkGetPhysicalDeviceFeatures(m_physical, &avail_feats);
        m_int64 = avail_feats.shaderInt64 == VK_TRUE;
        VkPhysicalDeviceFeatures enabled_feats{};
        enabled_feats.shaderInt64 = avail_feats.shaderInt64;

        const float             qp = 1.0F;
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_compute_family;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &qp;

        VkPhysicalDevice16BitStorageFeatures s16{};
        s16.sType                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        s16.storageBuffer16BitAccess = VK_TRUE;
        VkPhysicalDeviceShaderFloat16Int8Features f16{};
        f16.sType         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        f16.shaderFloat16 = VK_TRUE;
        f16.pNext         = &s16;
        VkPhysicalDeviceVulkanMemoryModelFeatures vmm{};
        vmm.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        vmm.vulkanMemoryModel = VK_TRUE;
        vmm.pNext            = &f16;
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmk{};
        cmk.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
        cmk.cooperativeMatrix = VK_TRUE;
        cmk.pNext            = &vmm;
        VkPhysicalDeviceCooperativeMatrix2FeaturesNV cm2{};
        cm2.sType                              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV;
        cm2.cooperativeMatrixWorkgroupScope    = VK_TRUE;
        cm2.cooperativeMatrixFlexibleDimensions = VK_TRUE;
        cm2.pNext                              = &cmk;

        VkDeviceCreateInfo dci{};
        dci.sType                 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount  = 1;
        dci.pQueueCreateInfos     = &qci;
        dci.pEnabledFeatures      = &enabled_feats; // shaderInt64 (no VkPhysicalDeviceFeatures2 in the pNext chain)
        const char* devexts[2]    = {"VK_KHR_cooperative_matrix", "VK_NV_cooperative_matrix2"};
        if (m_coopmat2)
        {
            dci.pNext                   = &cm2;
            dci.enabledExtensionCount   = 2;
            dci.ppEnabledExtensionNames = devexts;
        }
        if (vkCreateDevice(m_physical, &dci, nullptr, &m_device) != VK_SUCCESS) { return; }
        vkGetDeviceQueue(m_device, m_compute_family, 0, &m_compute_queue);
        m_valid = true;
    }

    VkInstance       m_instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical       = VK_NULL_HANDLE;
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkQueue          m_compute_queue  = VK_NULL_HANDLE;
    crd::u32         m_compute_family = UINT32_MAX;
    bool             m_coopmat2       = false;
    bool             m_int64          = false;
    bool             m_valid          = false;
    char             m_name[256]      = {};
};

} // namespace

std::unique_ptr<IGpuContext> create_vulkan_gpu_context(const GpuContextConfig& config)
{
    if (config.backend != GpuBackend::Vulkan) { return nullptr; }
    auto ctx = std::make_unique<VulkanGpuContextImpl>(config);
    if (!ctx->valid()) { return nullptr; }
    return ctx;
}

} // namespace crd::gpu
