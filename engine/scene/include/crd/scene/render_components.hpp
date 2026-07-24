#pragma once

// render_components.hpp — GEO-3 stage 3 (D-007 row 68): the RENDER COMPONENTS an imported scene decomposes into.
// The decompose philosophy (ADR-0055 + the GEO band): a scene ENTITY references cooked per-type resources by
// `ResourceId` — it never embeds geometry, pixels, or shader code. These are trivially-copyable PODs with SCEN
// serialize FourCCs (serialize.hpp), registered on any World that instantiates imported scenes; GEO-7 wires the
// chunk-grain draw submission that consumes them through the gpu-context renderer (ADR-0105).
//
// Angle/color conventions: angles are RADIANS; colors are LINEAR RGB (the glTF/KHR source conventions — no
// hidden degree or sRGB conversions between the parser and this struct).

#include <crd/core/types.hpp>
#include <crd/math/vec.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/world.hpp>

namespace crd::scene
{

// A drawable: cooked mesh + authored material, both by ResourceId. `material` is NULL until material authoring
// assigns one (GEO-3 stage 4) — a renderer draws a null-material mesh with its default surface.
struct MeshRenderer
{
    crd::resources::ResourceId mesh{};
    crd::resources::ResourceId material{};
};
static_assert(sizeof(MeshRenderer) == 32, "MeshRenderer layout pinned (SCEN v1 payload)");

// The glTF camera surface (perspective OR orthographic). `zfar == 0` = infinite projection; `aspect == 0` =
// "use the viewport's" (both per glTF's optional fields).
struct SceneCamera
{
    crd::f32 yfov_rad  = 1.0F; // perspective vertical field of view
    crd::f32 aspect    = 0.0F; // 0 = viewport-defined
    crd::f32 znear     = 0.1F;
    crd::f32 zfar      = 0.0F; // 0 = infinite
    crd::f32 ortho_xmag = 0.0F;
    crd::f32 ortho_ymag = 0.0F;
    crd::u8  is_ortho   = 0;
    crd::u8  reserved[3] = {};
};
static_assert(sizeof(SceneCamera) == 28, "SceneCamera layout pinned (SCEN v1 payload)");

enum class SceneLightType : crd::u8
{
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

// The KHR_lights_punctual surface. Intensity units follow the KHR spec: candela (lm/sr) for point/spot,
// lux (lm/m²) for directional. `range == 0` = unlimited.
struct SceneLight
{
    crd::math::Vec3f color{1.0F, 1.0F, 1.0F}; // linear RGB
    crd::f32         intensity      = 1.0F;
    crd::f32         range          = 0.0F;
    crd::f32         inner_cone_rad = 0.0F;            // spot only
    crd::f32         outer_cone_rad = 0.78539816339F;  // spot only; KHR default π/4
    crd::u8          type           = 0;               // SceneLightType byte value
    crd::u8          reserved[3]    = {};
};
static_assert(sizeof(SceneLight) == 32, "SceneLight layout pinned (SCEN v1 payload)");

// GEO-8 (appended): the skinned-animation driver — which SKELETON deforms this entity's mesh, which CLIP plays,
// and the playhead. The runtime advances `time` (chunk-grain) and the renderer samples/uploads the bone palette;
// AI/tooling can freely retarget by swapping `clip` or writing `time` (scrubbing = just setting the float).
struct SkeletonAnimator
{
    crd::resources::ResourceId skeleton{};
    crd::resources::ResourceId clip{};
    crd::f32                   time     = 0.0F;
    crd::f32                   speed    = 1.0F;
    crd::u32                   flags    = 1U; // bit 0: loop
    crd::u32                   reserved = 0U; // explicit tail pad — deterministic serialized bytes
};
static_assert(sizeof(SkeletonAnimator) == 48, "SkeletonAnimator layout pinned (SCEN v1 payload)");

// Register the render components (with their SCEN serialize traits) on a World. Both the cook-side temp World
// and any runtime World that instantiates imported scenes call this — registrations must match bit-for-bit.
inline void register_render_components(World& w)
{
    w.register_component<MeshRenderer>(default_serialize_trait<MeshRenderer>(kFourCC_MeshRenderer));
    w.register_component<SceneCamera>(default_serialize_trait<SceneCamera>(kFourCC_SceneCamera));
    w.register_component<SceneLight>(default_serialize_trait<SceneLight>(kFourCC_SceneLight));
    w.register_component<SkeletonAnimator>(default_serialize_trait<SkeletonAnimator>(kFourCC_SkeletonAnimator));
}

} // namespace crd::scene
