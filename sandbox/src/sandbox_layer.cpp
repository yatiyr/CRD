#include "sandbox_layer.hpp"

#include <crd/app/events/input_events.hpp>
#include <crd/platform/input.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace crd::sandbox
{
namespace
{
constexpr float kOrbitSpeed    = 8.0F;
constexpr float kPanSpeed      = 0.005F;
constexpr float kZoomSpeed     = 0.5F;
constexpr float kMinDistance   = 0.1F;
constexpr float kMaxDistance   = 500.0F;
constexpr float kPitchClamp    = 89.0F * (std::numbers::pi_v<float> / 180.0F);

float exp_lerp(float a, float b, float speed, float dt) noexcept
{
    return a + (b - a) * (1.0F - std::exp(-speed * dt));
}

crd::math::Vec3f exp_lerp3(crd::math::Vec3f a, crd::math::Vec3f b, float speed, float dt) noexcept
{
    const float t = 1.0F - std::exp(-speed * dt);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
} // namespace

SandboxLayer::SandboxLayer(crd::app::Application& app, crd::rhi::Swapchain& swapchain)
    : Layer("SandboxLayer"), m_app(app), m_swapchain(swapchain)
{
}

void SandboxLayer::on_update(crd::f64 delta_seconds)
{
    const auto& input = m_app.window().input().state();
    const float dt    = static_cast<float>(delta_seconds);

    const bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

    if (!imgui_wants_mouse)
    {
        // Left-drag: orbit
        if (input.is_mouse_down(crd::platform::MouseButton::Left))
        {
            m_cam.yaw   += input.mouse_dx() * 0.4F;
            m_cam.pitch -= input.mouse_dy() * 0.4F;
            m_cam.pitch  = std::clamp(m_cam.pitch, -kPitchClamp, kPitchClamp);
        }

        // Ctrl+MMB: pan target
        if (input.is_mouse_down(crd::platform::MouseButton::Middle) &&
            (input.is_key_down(crd::platform::Key::LeftCtrl) || input.is_key_down(crd::platform::Key::RightCtrl)))
        {
            const float right_scale = m_cam.distance * kPanSpeed;
            const float up_scale    = m_cam.distance * kPanSpeed;
            m_cam.target.x -= input.mouse_dx() * right_scale;
            m_cam.target.y += input.mouse_dy() * up_scale;
        }

        // Scroll: zoom
        const float scroll = input.scroll_dy();
        if (scroll != 0.0F)
        {
            m_cam.distance -= scroll * kZoomSpeed * (m_cam.distance * 0.2F + 0.1F);
            m_cam.distance  = std::clamp(m_cam.distance, kMinDistance, kMaxDistance);
        }
    }

    // Smooth camera state toward target.
    m_cam.s_yaw    = exp_lerp(m_cam.s_yaw,   m_cam.yaw,      kOrbitSpeed, dt);
    m_cam.s_pitch  = exp_lerp(m_cam.s_pitch, m_cam.pitch,    kOrbitSpeed, dt);
    m_cam.s_dist   = exp_lerp(m_cam.s_dist,  m_cam.distance, kOrbitSpeed, dt);
    m_cam.s_target = exp_lerp3(m_cam.s_target, m_cam.target, kOrbitSpeed, dt);
}

void SandboxLayer::on_render()
{
    const auto& ext = m_swapchain.desc().extent;
    ImGui::SetNextWindowPos({8.0F, 8.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({320.0F, 160.0F}, ImGuiCond_Always);
    ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Viewport: %u x %u", ext.width, ext.height);
    ImGui::Separator();
    ImGui::Text("Camera (smoothed)");
    ImGui::Text("  yaw:      %.2f deg", static_cast<double>(m_cam.s_yaw * (180.0F / std::numbers::pi_v<float>)));
    ImGui::Text("  pitch:    %.2f deg", static_cast<double>(m_cam.s_pitch * (180.0F / std::numbers::pi_v<float>)));
    ImGui::Text("  distance: %.2f", static_cast<double>(m_cam.s_dist));
    ImGui::Text("  target:   (%.2f, %.2f, %.2f)",
                static_cast<double>(m_cam.s_target.x),
                static_cast<double>(m_cam.s_target.y),
                static_cast<double>(m_cam.s_target.z));
    ImGui::Separator();
    ImGui::TextDisabled("LMB drag=orbit  Ctrl+MMB=pan  Scroll=zoom");
    ImGui::End();
}

} // namespace crd::sandbox
