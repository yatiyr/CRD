#pragma once

#include <crd/renderer/material_template.hpp>

namespace crd::resources
{
class ResourceManager;
} // namespace crd::resources

namespace crd::renderer
{

// Register the MaterialResourceLoader (handles kFourCC_MATR) with the given manager.
// Must be called after register_shader_loader() — MATR loading transitively
// loads SHDR artifacts for each pass shader pair.
void register_material_loader(crd::resources::ResourceManager* rm);

} // namespace crd::renderer
