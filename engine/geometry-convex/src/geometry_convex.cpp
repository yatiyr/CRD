// crd-geometry-convex — force-link anchor.
//
// v2a is header-only templates today. This TU keeps `crd-geometry-convex` a
// real link target so ASan / the SIMD-emission checks have an `.obj` to
// inspect once v2h adds the out-of-line `hull_support_simd.cpp`. Same role
// as `engine/geometry-primitives/src/geometry_primitives.cpp`.

namespace crd::geometry::convex
{
int force_link_geometry_convex() noexcept
{
    return 0;
}
} // namespace crd::geometry::convex
