// crd-units — translation unit anchor.
//
// The substrate is header-only at v0a-1; out-of-line implementation lands in
// v0a-3 (format/parse) and later adoption passes. This TU exists so the
// static library has at least one .obj for the linker.

#include <crd/units/units.hpp>

namespace crd::units::detail
{
// Force-link anchor — keeps the static library non-empty across all the
// header-only v0a-1 + v0a-2 slices. v0a-3 adds real out-of-line bodies for
// format/parse.
[[maybe_unused]] inline constexpr int kUnitsLinkAnchor = 0;
} // namespace crd::units::detail
