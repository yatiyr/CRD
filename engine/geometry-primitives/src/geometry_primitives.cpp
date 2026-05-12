#include <crd/geometry/primitives/primitives.hpp>

namespace crd::geometry::primitives
{
// Force-link anchor — v0a is header-only (templated types + inline helpers), so
// this is the only translation unit in the static library. It gives ASan / the
// SIMD-emission CI check a real .obj to inspect once v0c+ adds out-of-line SIMD
// batch kernels to this module.
int force_link_geometry_primitives() noexcept
{
    return 0;
}
} // namespace crd::geometry::primitives
