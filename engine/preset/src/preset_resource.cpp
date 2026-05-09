// Phase 3.0 v1n1 — PresetResource implementation (ADR-0059).
//
// All non-trivial members are inline in the header; this TU exists so the
// link unit is non-empty (matches v1k SceneResource pattern) and so the
// PCH coverage is uniform across the module's build targets.

#include <crd/preset/preset_resource.hpp>

namespace crd::preset
{
} // namespace crd::preset
