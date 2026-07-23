// imgui_gpu_backend.cpp — RET-5: the Dear ImGui render backend on gpu-context (ADR-0105). See imgui_gpu_backend.hpp.

#include <crd/imgui/imgui_gpu_backend.hpp>

#include <crd/core/assert.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

namespace crd::imgui
{

namespace
{
void check_vk_result([[maybe_unused]] VkResult result)
{
    CRD_ASSERT(result == VK_SUCCESS && "ImGui_ImplVulkan reported a VkResult failure"); // compiled out in shipping
}
} // namespace

ImGuiGpuBackend::ImGuiGpuBackend(crd::gpu::VulkanGpuContext& ctx, const crd::gpu::IPresentSurface& surface)
{
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, // ImGui's font atlas + user textures
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = 64;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = pool_sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx.vk_device(), &pool_info, nullptr, &pool) != VK_SUCCESS) { return; }
    m_device          = ctx.vk_device();
    m_descriptor_pool = pool;

    const auto     image_count  = crd::gpu::vulkan_present_image_count(surface);
    const VkFormat color_format = static_cast<VkFormat>(crd::gpu::vulkan_present_color_format_raw(surface));

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance            = ctx.vk_instance();
    init_info.PhysicalDevice      = ctx.vk_physical_device();
    init_info.Device              = ctx.vk_device();
    init_info.QueueFamily         = ctx.graphics_family();
    init_info.Queue               = ctx.graphics_queue();
    init_info.PipelineCache       = VK_NULL_HANDLE;
    init_info.DescriptorPool      = pool;
    init_info.Subpass             = 0;
    init_info.MinImageCount       = image_count < 2U ? 2U : image_count;
    init_info.ImageCount          = image_count < 2U ? 2U : image_count;
    init_info.MSAASamples         = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = true; // matches the surface's overlay pass (vkCmdBeginRendering)
    init_info.PipelineRenderingCreateInfo                         = {};
    init_info.PipelineRenderingCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init_info.CheckVkResultFn                                     = &check_vk_result;

    m_attached = ImGui_ImplVulkan_Init(&init_info);
    if (!m_attached)
    {
        vkDestroyDescriptorPool(static_cast<VkDevice>(m_device), pool, nullptr);
        m_descriptor_pool = nullptr;
    }
}

ImGuiGpuBackend::~ImGuiGpuBackend()
{
    if (m_attached)
    {
        vkDeviceWaitIdle(static_cast<VkDevice>(m_device)); // in-flight ImGui pipelines drain before shutdown
        ImGui_ImplVulkan_Shutdown();
        m_attached = false;
    }
    if (m_descriptor_pool != nullptr)
    {
        vkDestroyDescriptorPool(static_cast<VkDevice>(m_device), static_cast<VkDescriptorPool>(m_descriptor_pool),
                                nullptr);
        m_descriptor_pool = nullptr;
    }
}

void ImGuiGpuBackend::new_frame()
{
    if (m_attached) { ImGui_ImplVulkan_NewFrame(); }
}

void ImGuiGpuBackend::render(void* backend_cmd)
{
    if (!m_attached || backend_cmd == nullptr) { return; }
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) { return; }
    ImGui_ImplVulkan_RenderDrawData(draw_data, static_cast<VkCommandBuffer>(backend_cmd));
}

void ImGuiGpuBackend::overlay_thunk(void* backend_cmd, void* user)
{
    static_cast<ImGuiGpuBackend*>(user)->render(backend_cmd);
}

} // namespace crd::imgui
