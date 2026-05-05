#pragma once

#include <crd/app/application.hpp>
#include <crd/app/layer.hpp>
#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/rhi/swapchain.hpp>

namespace crd::sandbox
{

// Exponential-lerp smoothed orbit camera (yaw/pitch/distance + focus target).
// Input-driven "target" state is smoothed into "s_*" state each frame.
struct OrbitCamera
{
    float yaw      = 0.0F;
    float pitch    = 0.0F;
    float distance = 5.0F;

    float s_yaw   = 0.0F;
    float s_pitch = 0.0F;
    float s_dist  = 5.0F;

    crd::math::Vec3f target{};
    crd::math::Vec3f s_target{};
};

struct ShapeInfo
{
    const char* name;
    crd::u32    verts;
    crd::u32    indices;
};

class SandboxLayer final : public crd::app::Layer
{
public:
    SandboxLayer(crd::app::Application& app, crd::rhi::Swapchain& swapchain);

    void on_update(crd::f64 delta_seconds) override;
    void on_render() override;

private:
    crd::app::Application&            m_app;
    crd::rhi::Swapchain&              m_swapchain;
    OrbitCamera                       m_cam{};
    crd::memory::MallocAllocator      m_alloc;
    crd::containers::Array<ShapeInfo> m_shapes;
    int                               m_selected = -1;
};

} // namespace crd::sandbox
