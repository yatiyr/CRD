#pragma once

#include <crd/app/application.hpp>
#include <crd/app/layer.hpp>
#include <crd/config/config.hpp>
#include <crd/containers/string.hpp>
#include <crd/imgui/settings.hpp>
#include <crd/rhi/rhi.hpp>

struct ImDrawData;

namespace crd::imgui
{
class ImGuiLayer final : public crd::app::Layer
{
public:
    // D-008 C2-c2: no `crd::rhi::Instance&` — the VkInstance is pulled from the Device (`vulkan_instance(Device&)`), so
    // ImGui works on the ADOPTED path (a VulkanGpuContext owns the instance; there is no rhi Instance).
    ImGuiLayer(crd::app::Application& app, crd::rhi::Device& device, crd::rhi::Swapchain& swapchain,
               const crd::config::Config& config);
    ~ImGuiLayer() override;

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void on_attach() override;
    void on_detach() override;
    void on_frame_begin() override;
    void on_render() override;
    void on_event(crd::app::Event& event) override;

    void render(crd::rhi::CommandBuffer& command_buffer);

    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }

private:
    void apply_style();
    void build_default_panels();

    crd::app::Application& m_app;
    crd::rhi::Device& m_device;
    crd::rhi::Swapchain& m_swapchain;
    Settings m_settings{};
    void* m_descriptor_pool = nullptr;
    bool m_attached = false;
    crd::containers::String m_ini_path{}; // backing storage for ImGuiIO::IniFilename
};
} // namespace crd::imgui
