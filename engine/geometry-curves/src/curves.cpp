// ---------------------------------------------------------------------------
// crd-geometry-curves — Module anchor TU.
//
// crd-geometry-curves is mostly header-only (curve types + evaluators are
// inline templates over `MathScalar T`). `validate.cpp` provides the
// explicit-instantiated validators; this TU is the force-link anchor that
// keeps the static-lib non-empty across all build configs.
// ---------------------------------------------------------------------------

#include <crd/geometry/curves/curves.hpp>

namespace crd::geometry::curves
{
// Anchor symbol — referenced indirectly by `validate.cpp`'s explicit
// instantiations. Keeps the static lib link-resolvable.
[[maybe_unused]] void crd_geometry_curves_anchor() noexcept {}
} // namespace crd::geometry::curves
