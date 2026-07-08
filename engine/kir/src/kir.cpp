// kir.cpp — Phase 3.1.6 v17: crd-kir anchor TU. Keeps the static library linkable while the IR + CPU reference are
// header-only (v17-a); later slices (CKIR-Tile, the backend code generators, the runtime) add real sources here.
// ADR-0098.

#include <crd/kir/ckir.hpp>

namespace crd::kir
{
// Anchor symbol — pulls this TU into the static-lib link so future registration blocks survive (the static-lib anchor
// pattern used across the engine).
void kir_anchor() noexcept {}
} // namespace crd::kir
