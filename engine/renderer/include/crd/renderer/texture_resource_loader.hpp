#pragma once

// ⛔ COMPAT SHIM (RET-3, ADR-0105): the TXTR loader RE-HOMED to crd-resources. See texture_resource.hpp.

#include <crd/resources/texture_resource.hpp>

namespace crd::renderer
{
using crd::resources::register_texture_loader;
} // namespace crd::renderer
