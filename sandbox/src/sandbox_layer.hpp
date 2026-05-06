#pragma once

#include <crd/app/application.hpp>
#include <crd/app/layer.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/forward_render_path.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/swapchain.hpp>
#include <crd/shader/runtime.hpp>

#include <memory>

namespace crd::sandbox
{

// Quaternion-native exponential-lerp smoothed orbit camera.
// q_target is driven by input; q_smooth slerp-follows each frame.
// Rest pose: q=identity → camera offset is {0, 0, distance} from target.
struct OrbitCamera
{
    crd::math::Quatf q_target = crd::math::Quatf::identity();
    crd::math::Quatf q_smooth = crd::math::Quatf::identity();
    float            distance = 5.0F;
    float            s_dist   = 5.0F;
    crd::math::Vec3f target{};
    crd::math::Vec3f s_target{};
};

// Per-shape adjustable parameters (defaults match meshgen API defaults).
struct PlaneParams    { float w = 1.0F; float d = 1.0F; int divs_x = 1; int divs_z = 1; };
struct BoxParams      { float w = 1.0F; float h = 1.0F; float d = 1.0F; };
struct SphereParams   { float radius = 1.0F; int lat = 16; int lon = 32; };
struct IcoParams      { float radius = 1.0F; int subdiv = 2; };
struct CylinderParams { float radius = 0.5F; float h = 1.0F; int segs = 32; };
struct ConeParams     { float radius = 0.5F; float h = 1.0F; int segs = 32; };
struct CapsuleParams  { float radius = 0.5F; float h = 1.0F; int segs = 32; int rings = 8; };
struct TorusParams    { float maj_r = 1.0F; float min_r = 0.25F; int maj_segs = 32; int min_segs = 16; };

// Asset Browser entry — a single user-selectable item (procedural or imported).
enum class AssetKind : crd::u8 { Procedural = 0, Imported = 1 };

struct AssetEntry
{
    crd::containers::String     display_name;
    AssetKind                   kind = AssetKind::Procedural;
    crd::u32                    procedural_idx = 0;             // 0..7 when kind == Procedural
    crd::resources::ResourceId  imported_id;                    // valid when kind == Imported
    crd::u32                    cached_verts   = 0;
    crd::u32                    cached_indices = 0;
};

// Pipeline resolver used by ForwardRenderPath in the sandbox.
// Compiles depth-only and full-color pipelines on first call from the variant's SPIR-V.
class SandboxPipelineResolver final : public crd::renderer::PipelineResolver
{
public:
    void init(crd::rhi::Device& device, crd::rhi::PipelineLayout& layout,
              crd::shader::Runtime& runtime, crd::shader::VariantHandle variant,
              crd::rhi::Extent2D extent);

    void begin_pass(crd::renderer::PassType pass) noexcept override;

    [[nodiscard]] crd::rhi::Pipeline*
    resolve_pipeline(const crd::shader::VariantPipelineDesc& handoff) noexcept override;

private:
    void ensure_compiled(const crd::shader::VariantPipelineDesc& handoff) noexcept;

    crd::rhi::Device*          m_device  = nullptr;
    crd::rhi::PipelineLayout*  m_layout  = nullptr;
    crd::shader::Runtime*      m_runtime = nullptr;
    crd::shader::VariantHandle m_variant{};
    crd::rhi::Extent2D         m_extent{};
    crd::renderer::PassType    m_current_pass = crd::renderer::PassType::Forward;

    std::unique_ptr<crd::rhi::Pipeline> m_depth_pipeline;
    std::unique_ptr<crd::rhi::Pipeline> m_color_pipeline;
    bool m_compiled = false;
};

class SandboxLayer final : public crd::app::Layer
{
public:
    SandboxLayer(crd::app::Application& app, crd::rhi::Device& device, crd::rhi::Swapchain& swapchain);

    void on_update(crd::f64 delta_seconds) override;
    void on_render() override;
    void on_event(crd::app::Event& event) override;

    // 3D scene rendering: runs ForwardRenderPath, blits result to swapchain, transitions to ColorWrite
    // so ImGui can render on top with LoadOp::Load.
    void render_scene(crd::rhi::CommandBuffer& cmd, crd::rhi::Image& sc_image, crd::u32 frame_index);

private:
    void upload_procedural(int idx);
    void kick_async_import_load(int idx);
    void try_finalize_pending_load();
    void build_wireframe_pipeline(const crd::platform::fs::Path& source_dir);
    void register_procedural_assets();
    void try_register_imported_assets();

    crd::app::Application&            m_app;
    crd::rhi::Device&                 m_device;
    crd::rhi::Swapchain&              m_swapchain;
    OrbitCamera                       m_cam{};
    crd::memory::MallocAllocator      m_alloc;

    // Unified asset list: procedural + imported.
    crd::containers::Array<AssetEntry> m_assets;
    int                                m_selected      = -1;
    int                                m_last_uploaded = -1;
    bool                               m_mesh_dirty    = false;

    // Async load tracking for imported assets. m_pending_index >= 0 means a
    // load_async() is in flight for that asset; m_pending_load is the typed
    // handle whose state() advances Queued → Loading → Ready/Failed on the
    // job pool. We poll it once per frame in try_finalize_pending_load() and
    // only invoke the (still-synchronous) GpuUploader when the CPU payload
    // has landed. The currently-rendered mesh keeps rendering until the swap.
    crd::resources::ResourceHandle<crd::renderer::MeshResource> m_pending_load;
    int                                                         m_pending_index = -1;

    // Per-shape parameters (only meaningful for procedural entries).
    PlaneParams    m_plane{};
    BoxParams      m_box{};
    SphereParams   m_sphere{};
    IcoParams      m_ico{};
    CylinderParams m_cylinder{};
    ConeParams     m_cone{};
    CapsuleParams  m_capsule{};
    TorusParams    m_torus{};

    // Render mode toggles.
    bool m_show_solid     = true;
    bool m_show_wireframe = false;

    std::unique_ptr<crd::shader::Runtime>             m_shader_runtime;
    crd::shader::VariantHandle                        m_surface_variant{};
    SandboxPipelineResolver                           m_resolver;
    std::unique_ptr<crd::rhi::DescriptorAllocator>    m_desc_alloc;
    std::unique_ptr<crd::renderer::ForwardRenderPath> m_frp;
    crd::renderer::GpuMesh                            m_gpu_mesh;
    crd::renderer::Renderer                           m_renderer;

    // Resource system for imported assets (cooked demo_assets.crdr).
    std::unique_ptr<crd::resources::ResourceManager> m_resource_mgr;
    bool                                             m_imported_available = false;

    // Wireframe overlay pipeline (built once; no descriptor sets; 64-byte MVP push constant).
    std::unique_ptr<crd::rhi::PipelineLayout> m_wf_layout;
    std::unique_ptr<crd::rhi::Pipeline>       m_wf_pipeline;
};

} // namespace crd::sandbox
