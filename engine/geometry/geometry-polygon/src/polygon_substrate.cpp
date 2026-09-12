// crd-geometry-polygon — substrate translation unit.
//
// The v6a substrate (`polygon_types.hpp` + `polygon_predicates.hpp` + typed
// wrappers) is header-only. This TU exists to:
//   1) Give CMake a non-empty .cpp list for the static library target so it
//      links without the "Library has no source files" warning.
//   2) Pin the layout invariants the typed wrappers rely on via static_assert
//      (the ADR-0078 §2 D2 contract: `Vec2<Quantity<D,T>>` is bit-identical
//      to `Vec2<T>`). If a future refactor of `crd-math` or `crd-units`
//      breaks the contract, this TU fails to build immediately and the
//      typed wrappers' `reinterpret_cast` never reaches a release binary.

#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::polygon::polygon_layout_pins
{

// f32 / Length32 layout pin — typed wrapper boundary requires bit-identical
// element layout for the `reinterpret_cast<const Vec2<T>*>(typed.data())`
// bridge to be well-defined. If sizeof changes, layout has changed; halt.
static_assert(sizeof(crd::math::Vec2<crd::units::Length32>) == sizeof(crd::math::Vec2<crd::f32>),
              "Vec2<Length32> must be layout-identical to Vec2<f32> (ADR-0078 §2 D2 pin)");
static_assert(alignof(crd::math::Vec2<crd::units::Length32>) == alignof(crd::math::Vec2<crd::f32>),
              "Vec2<Length32> alignment must match Vec2<f32> (ADR-0078 §2 D2 pin)");

// f64 / Length64 layout pin — same invariant on the wider precision tier.
static_assert(sizeof(crd::math::Vec2<crd::units::Length64>) == sizeof(crd::math::Vec2<crd::f64>),
              "Vec2<Length64> must be layout-identical to Vec2<f64> (ADR-0078 §2 D2 pin)");
static_assert(alignof(crd::math::Vec2<crd::units::Length64>) == alignof(crd::math::Vec2<crd::f64>),
              "Vec2<Length64> alignment must match Vec2<f64> (ADR-0078 §2 D2 pin)");

// Ring2 / PolygonView2 are non-owning views — they MUST be cheap to pass by
// value. Pin the upper bound: a view is a pointer + a size + (for the
// polygon view) a second pointer + size = 4 machine words on 64-bit.
static_assert(sizeof(crd::geometry::polygon::Ring2<crd::f32>) <= 2U * sizeof(void*),
              "Ring2<f32> must be a 2-word view (data pointer + size)");
static_assert(sizeof(crd::geometry::polygon::PolygonView2<crd::f32>) <= 4U * sizeof(void*),
              "PolygonView2<f32> must be a 4-word view (vertices + ring_offsets)");

} // namespace crd::geometry::polygon::polygon_layout_pins
