#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::cooker
{

// glTF 2.0 spec mandates POSITION accessors carry SI meters. Real-world
// exporters violate this constantly (Blender's "Apply Transform" toggle,
// SolidWorks cm exports, Unity 3DS imports, etc.). The .meta file may
// opt in to a per-asset linear scale that the cooker multiplies into
// every position attribute at cook time, so the runtime always sees SI
// meters regardless of source authoring units.
//
// .meta authoring contract:
//   [id]
//   uuid = "..."
//   [cook]                         # optional section
//   position_scale = 0.01          # cm -> m
//
// Default = 1.0F (asset already SI). Anything <= 0 is rejected and
// falls back to 1.0F with a warning.

struct MeshCookOptions
{
    crd::f32 position_scale = 1.0F;
};

// Parses the [cook] section of a .meta file body. Returns defaults when
// the section / key is absent. Pure string processing; testable without
// touching the filesystem.
[[nodiscard]] MeshCookOptions parse_mesh_cook_options(crd::containers::StringView meta_text) noexcept;

// SI sanity threshold: warn if any final (post-scale) position magnitude
// exceeds this. 1e6 m == 1 000 km, well above any reasonable engine asset
// and a strong signal that the source unit is wrong (e.g. positions in
// raw millimetres being treated as metres).
inline constexpr crd::f32 kSiPositionSanityMeters = 1.0e6F;

} // namespace crd::cooker
