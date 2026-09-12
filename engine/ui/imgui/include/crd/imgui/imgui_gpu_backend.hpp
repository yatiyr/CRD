#pragma once

// imgui_gpu_backend.hpp — RET-5 (ADR-0105): the Dear ImGui RENDER backend on the ONE graphics layer. Replaces the
// rhi-coupled half of ImGuiLayer (which dies at RET-8): device handles come from `VulkanGpuContext`, swapchain
// parameters from the RET-2 `IPresentSurface`, and draw data records into the surface's overlay pass
// (`IPresentSurface::present(target, overlay, user)` — the backbuffer composition seam).
//
// Deliberately APP-FREE and WINDOW-FREE: this class owns exactly the GPU half (descriptor pool + ImGui_ImplVulkan
// init/shutdown + draw-data recording). Platform input (GLFW/Win32 IO wiring) stays a separate concern layered on
// top — which is also what makes this backend gate-testable against a windowless ImGui context.
//
// Lifetime: an ImGui context must exist BEFORE construction and outlive this object. One backend per surface.

#include <crd/core/types.hpp>

namespace crd::gpu
{
class VulkanGpuContext;
class IGpuContext;
class IRasterContext;
class IPresentSurface;
} // namespace crd::gpu

namespace crd::imgui
{

class ImGuiGpuBackend
{
public:
    // Initializes ImGui_ImplVulkan against `ctx`'s device + `surface`'s swapchain parameters (image count + color
    // format, dynamic rendering). `valid()` reports success — a failed init leaves a safe inert object.
    ImGuiGpuBackend(crd::gpu::VulkanGpuContext& ctx, const crd::gpu::IPresentSurface& surface);
    // ⭐⭐ REN-39-D2: the DX12 twin. Takes the INTERFACE pair (`IGpuContext` names the backend; the raster
    // context owns the D3D12 device + queue ImGui needs) so an app selects its backend at runtime and this class
    // stays the one ImGui seam. `valid()` is still the only success signal.
    ImGuiGpuBackend(crd::gpu::IGpuContext& ctx, crd::gpu::IRasterContext& raster,
                    const crd::gpu::IPresentSurface& surface);
    ~ImGuiGpuBackend();

    ImGuiGpuBackend(const ImGuiGpuBackend&)            = delete;
    ImGuiGpuBackend& operator=(const ImGuiGpuBackend&) = delete;
    ImGuiGpuBackend(ImGuiGpuBackend&&)                 = delete;
    ImGuiGpuBackend& operator=(ImGuiGpuBackend&&)      = delete;

    [[nodiscard]] bool valid() const noexcept { return m_attached; }

    // Per-frame: call before ImGui::NewFrame() (the render backend's new-frame hook).
    void new_frame();

    // The overlay callback body: records ImGui::GetDrawData() into the surface's active overlay pass.
    // `backend_cmd` is the opaque command handle the present overlay delivers (Vulkan: VkCommandBuffer).
    void render(void* backend_cmd);

    // The IPresentSurface::OverlayFn trampoline — pass `this` as `user`:
    //   surface.present(canvas, &ImGuiGpuBackend::overlay_thunk, &backend);
    static void overlay_thunk(void* backend_cmd, void* user);

private:
    void* m_device          = nullptr; // VkDevice / ID3D12Device (opaque — the header stays backend-include-free)
    void* m_descriptor_pool = nullptr; // VkDescriptorPool / ID3D12DescriptorHeap (the SRV heap ImGui draws from)
    bool  m_attached        = false;
    bool  m_is_dx12         = false;   // which ImGui_Impl* owns `m_attached`
};

} // namespace crd::imgui
