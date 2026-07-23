#pragma once

// ⛔ COMPAT SHIM (RET-3, ADR-0105): the MESH loader RE-HOMED to crd-resources. See mesh_resource.hpp.

#include <crd/resources/mesh_resource.hpp>

namespace crd::renderer
{
using crd::resources::register_mesh_loader;
} // namespace crd::renderer
