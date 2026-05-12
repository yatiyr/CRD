#include <crd/geometry/shader_helpers/shader_helpers.hpp>

namespace crd::geometry::shader_helpers
{
// See the header — v0e skeleton; v9e adds the formula-IR cooker + GLSL/HLSL
// backends + the ULP-conformance test against `crd/geometry/primitives/formulary.hpp`.
int force_link_geometry_shader_helpers() noexcept
{
    return 0;
}
} // namespace crd::geometry::shader_helpers
