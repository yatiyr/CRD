#pragma once

#include <crd/app/application.hpp>
#include <crd/app/layer.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/visualizer_registry.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/preset/camera_preset.hpp>
#include <crd/preset/preset_target.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/preset/quality_preset.hpp>
#include <crd/preset/preset_loader.hpp>
#include <crd/profile/profile_context.hpp>
#include <crd/profile/profile_loader.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/renderer/forward_render_path.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/render_mesh_index.hpp>
#include <crd/renderer/render_upload_system.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/swapchain.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/world.hpp>
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

// Phase 3.0 v1o3 — IPresetTarget adapter that funnels CameraPreset values
// into the sandbox's runtime camera state. Lives next to SandboxLayer
// rather than directly on it because IPresetTarget's deleted-move ctor
// would conflict with the layer being constructed via `Application::add_layer<>`.
class SandboxCameraTarget final : public crd::preset::IPresetTarget
{
public:
    using crd::preset::IPresetTarget::apply;
    void apply(const crd::preset::CameraPreset& preset) override
    {
        m_preset = preset;
        ++m_apply_count;
    }

    [[nodiscard]] const crd::preset::CameraPreset& preset() const noexcept { return m_preset; }
    [[nodiscard]] crd::u32 apply_count() const noexcept { return m_apply_count; }

private:
    crd::preset::CameraPreset m_preset{};
    crd::u32                  m_apply_count = 0U;
};

class SandboxLayer final : public crd::app::Layer
{
public:
    SandboxLayer(crd::app::Application& app, crd::rhi::Device& device, crd::rhi::Swapchain& swapchain);
    ~SandboxLayer() override;

    void on_update(crd::f64 delta_seconds) override;
    void on_render() override;
    void on_event(crd::app::Event& event) override;

    // 3D scene rendering: runs ForwardRenderPath, blits result to swapchain, transitions to ColorWrite
    // so ImGui can render on top with LoadOp::Load.
    void render_scene(crd::rhi::CommandBuffer& cmd, crd::rhi::Image& sc_image, crd::u32 frame_index);

private:
    // ECS scene init — registers Renderable + PendingMeshUpload, the
    // RenderMeshIndex (drop-callback hook), and RenderUploadSystem.
    void init_scene_world();

    // Profile + Preset boot path — loads default.profile.toml from the
    // demo asset pack, resolves to a bundle, applies presets to the
    // ForwardRenderPath (QualityPreset) and the SandboxCameraTarget
    // (CameraPreset). Programmatic fallback on failure.
    void try_boot_profile_pipeline();

    // Asset selection paths. Both end up spawning a single ECS entity
    // carrying Renderable; imports also carry PendingMeshUpload until
    // the GPU upload's fence signals.
    void select_asset(int idx);
    void respawn_procedural(int idx);
    void kick_async_import(int idx);
    void try_finalize_pending_load();
    void destroy_current_entity_if_any();

    void build_wireframe_pipeline(const crd::platform::fs::Path& source_dir);
    void register_procedural_assets();
    void try_register_imported_assets();

    // Öbek runtime — instantiates obek_demo.obek.toml at boot if available.
    // The override / revert / unpack buttons in the ImGui panel exercise
    // ADR-0058's pillar 4 (override patches) and pillar 6 (unpack).
    void try_load_demo_obek();
    void apply_obek_translation_override();
    void revert_obek_translation_override();
    void unpack_obek_instantiation();

    crd::app::Application&            m_app;
    crd::rhi::Device&                 m_device;
    crd::rhi::Swapchain&              m_swapchain;
    OrbitCamera                       m_cam{};
    crd::memory::MallocAllocator      m_alloc;

    // Unified asset list: procedural + imported.
    crd::containers::Array<AssetEntry> m_assets;
    int                                m_selected      = -1;
    int                                m_pending_index = -1;   // -1 = none in flight
    int                                m_last_displayed = -1;  // last index whose entity is visible
    bool                               m_mesh_dirty    = false;

    // Async load tracking for imported assets.
    crd::resources::ResourceHandle<crd::renderer::MeshResource> m_pending_load;

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
    crd::renderer::Renderer                           m_renderer;

    // ECS world hosting all renderable entities (procedurals + imports +
    // öbek instantiations). RenderMeshIndex owns the GpuMeshes; the
    // RenderUploadSystem promotes async uploads in the RenderExtract
    // phase. Both are registered with the World below.
    std::unique_ptr<crd::scene::World>  m_world;
    crd::renderer::RenderMeshIndex*     m_mesh_idx       = nullptr; // owned by m_world
    crd::scene::EntityId                m_current_entity = crd::scene::EntityId::null();

    // Resource system for imported assets (cooked demo_assets.crdr).
    std::unique_ptr<crd::resources::ResourceManager> m_resource_mgr;
    bool                                             m_imported_available = false;

    // crd-draw retained per-frame buffer (v1a-draw d0d). Cleared at the
    // top of every render_scene; populated with debug primitives; consumed
    // by add_draw_overlay_pass.
    crd::draw::RenderBuffer m_draw_buffer;

    // d3: VisualizerRegistry teaches DebugVizSystem how to render each
    // component type. Owned here; non-owning pointer handed to the system.
    // System lifetime is bound to m_world (registered via register_system),
    // so the registry must outlive the world.
    crd::draw::VisualizerRegistry m_viz_registry;

    // Profile + Preset state ------------------------------------------------
    crd::profile::ProfileContext m_profile_context{};
    crd::resources::ResourceHandle<crd::profile::ProfileResource> m_profile_handle;
    crd::resources::ResourceHandle<crd::preset::PresetResource>   m_quality_handle;
    crd::resources::ResourceHandle<crd::preset::PresetResource>   m_camera_handle;
    SandboxCameraTarget                                           m_camera_target{};

    // L4 runtime overrides — sliders in the Quality / Camera panel mutate
    // these and re-apply to the targets. Default-init = same as schema
    // default = same as cooked default until the user touches a slider.
    crd::preset::QualityPreset m_quality_runtime{};
    crd::preset::CameraPreset  m_camera_runtime{};
    bool                       m_quality_runtime_dirty = false;
    bool                       m_camera_runtime_dirty  = false;
    bool                       m_profile_applied       = false;
    bool                       m_boot_kicked           = false;  // load_async kicks deferred
                                                                  // to first on_update so the
                                                                  // jobs system is initialised.
    crd::containers::String    m_profile_status;

    // Öbek demo state -------------------------------------------------------
    crd::resources::ResourceHandle<crd::scene::ObekResource>      m_obek_handle;
    std::unique_ptr<crd::scene::ObekInstantiation>                m_obek_instantiation;
    bool                                                          m_obek_loaded             = false;
    bool                                                          m_obek_child_override_active = false;
    crd::math::Vec3f                                              m_obek_child_override_translation{2.0F, 0.0F, 0.0F};
    crd::containers::String                                       m_obek_status;

    // Wireframe overlay pipeline (built once; no descriptor sets; 64-byte MVP push constant).
    std::unique_ptr<crd::rhi::PipelineLayout> m_wf_layout;
    std::unique_ptr<crd::rhi::Pipeline>       m_wf_pipeline;
};

} // namespace crd::sandbox
