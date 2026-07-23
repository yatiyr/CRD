#pragma once

// ⛔ COMPAT SHIM (RET-3, ADR-0105): TextureResource RE-HOMED to crd-resources — this header only aliases the new
// types into the retiring crd::renderer namespace so the FROZEN module (and its smokes) keep compiling until RET-8
// deletes them. New code includes <crd/resources/texture_resource.hpp> and uses crd::resources directly.

#include <crd/resources/texture_resource.hpp>

namespace crd::renderer
{
using crd::resources::MipLevel;
using crd::resources::TextureFormat;
using crd::resources::TextureResource;
} // namespace crd::renderer
