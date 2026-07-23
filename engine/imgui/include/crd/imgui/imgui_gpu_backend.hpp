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
    void* m_device          = nullptr; // VkDevice (opaque here — the header stays Vulkan-include-free)
    void* m_descriptor_pool = nullptr; // VkDescriptorPool
    bool  m_attached        = false;
};

} // namespace crd::imgui
