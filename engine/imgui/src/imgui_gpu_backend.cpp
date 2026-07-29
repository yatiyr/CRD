// imgui_gpu_backend.cpp — RET-5: the Dear ImGui render backend on gpu-context (ADR-0105). See imgui_gpu_backend.hpp.

#include <crd/imgui/imgui_gpu_backend.hpp>

#include <crd/core/assert.hpp>
#include <crd/gpu/context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#if defined(_WIN32)
#include <crd/gpu/dx12_raster_context.hpp> // REN-39-D2: the native-handle accessors

#include <d3d12.h>

#include <backends/imgui_impl_dx12.h>
#endif

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

// ⭐⭐ REN-39-D2: the DX12 constructor. ImGui's D3D12 backend needs the device, the queue it can upload the font
// atlas on, the backbuffer format, and a SHADER-VISIBLE CBV/SRV/UAV heap it owns a descriptor in. We give it a
// dedicated one-descriptor heap so it can never collide with the renderer's frame heaps — and because ImGui's
// draw call SETS ITS OWN descriptor heap on the command list, the present-seam overlay is the only place this is
// legal (a mid-frame-graph overlay would stomp the graph's bound heaps).
#if defined(_WIN32)
ImGuiGpuBackend::ImGuiGpuBackend(crd::gpu::IGpuContext& ctx, crd::gpu::IRasterContext& raster,
                                 const crd::gpu::IPresentSurface& surface)
{
    if (ctx.backend() != crd::gpu::GpuBackend::Dx12) { return; } // Vulkan uses the other ctor — never guess
    auto* device = static_cast<ID3D12Device*>(crd::gpu::dx12_device_raw(raster));
    auto* queue  = static_cast<ID3D12CommandQueue*>(crd::gpu::dx12_graphics_queue_raw(raster));
    if (device == nullptr || queue == nullptr) { return; }

    ID3D12DescriptorHeap*      heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) { return; }

    ImGui_ImplDX12_InitInfo info{};
    info.Device            = device;
    info.CommandQueue      = queue;
    info.NumFramesInFlight = static_cast<int>(crd::gpu::dx12_present_image_count(surface));
    if (info.NumFramesInFlight < 2) { info.NumFramesInFlight = 2; }
    info.RTVFormat         = static_cast<DXGI_FORMAT>(crd::gpu::dx12_present_color_format_raw(surface));
    info.DSVFormat         = DXGI_FORMAT_UNKNOWN; // the overlay binds NO depth (present-seam composition)
    info.SrvDescriptorHeap = heap;
    info.LegacySingleSrvCpuDescriptor = heap->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor = heap->GetGPUDescriptorHandleForHeapStart();

    m_is_dx12         = true;
    m_device          = static_cast<void*>(device);
    m_descriptor_pool = static_cast<void*>(heap);
    m_attached        = ImGui_ImplDX12_Init(&info);
    if (!m_attached)
    {
        heap->Release();
        m_descriptor_pool = nullptr;
    }
}
#else
ImGuiGpuBackend::ImGuiGpuBackend(crd::gpu::IGpuContext&, crd::gpu::IRasterContext&, const crd::gpu::IPresentSurface&)
{
}
#endif

ImGuiGpuBackend::~ImGuiGpuBackend()
{
#if defined(_WIN32)
    if (m_is_dx12)
    {
        if (m_attached)
        {
            ImGui_ImplDX12_Shutdown();
            m_attached = false;
        }
        if (m_descriptor_pool != nullptr)
        {
            static_cast<ID3D12DescriptorHeap*>(m_descriptor_pool)->Release();
            m_descriptor_pool = nullptr;
        }
        return;
    }
#endif
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
    if (!m_attached) { return; }
#if defined(_WIN32)
    if (m_is_dx12)
    {
        ImGui_ImplDX12_NewFrame();
        return;
    }
#endif
    ImGui_ImplVulkan_NewFrame();
}

void ImGuiGpuBackend::render(void* backend_cmd)
{
    if (!m_attached || backend_cmd == nullptr) { return; }
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) { return; }
#if defined(_WIN32)
    if (m_is_dx12)
    {
        // ⛔ ImGui's DX12 backend does NOT set the descriptor heap itself — the caller must, and it must be the
        // shader-visible heap its font SRV lives in. Doing it here (not in the present surface) keeps the seam's
        // knowledge of ImGui at zero.
        auto* list = static_cast<ID3D12GraphicsCommandList*>(backend_cmd);
        auto* heap = static_cast<ID3D12DescriptorHeap*>(m_descriptor_pool);
        if (heap != nullptr) { list->SetDescriptorHeaps(1, &heap); }
        ImGui_ImplDX12_RenderDrawData(draw_data, list);
        return;
    }
#endif
    ImGui_ImplVulkan_RenderDrawData(draw_data, static_cast<VkCommandBuffer>(backend_cmd));
}

void ImGuiGpuBackend::overlay_thunk(void* backend_cmd, void* user)
{
    static_cast<ImGuiGpuBackend*>(user)->render(backend_cmd);
}

} // namespace crd::imgui
