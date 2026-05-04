#pragma once

#include <crd/core/types.hpp>

namespace crd::renderer
{

// Which rendering system owns and dispatches this material.
// Frozen values — do not reorder. Stored on disk in the INFO chunk (ADR-0048).
enum class MaterialDomain : crd::u8
{
    Surface    = 0, // standard rasterized surface (opaque / masked / transparent)
    PostProcess = 1, // full-screen post-FX pass
    Compute    = 2, // compute-only material
    Decal      = 3, // projected decal
    UI         = 4, // MTSDF / 2D UI elements
};

} // namespace crd::renderer
