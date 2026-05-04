#pragma once

#include <crd/resources/resource_handle.hpp>
#include <crd/shader/shader_resource_loader.hpp>

namespace crd::resources
{
class ResourceManager;
} // namespace crd::resources

namespace crd::renderer
{

// Runtime payload for a cooked MATR artifact.
// Holds ready handles to the vertex and fragment ShaderResource dependencies.
struct MaterialResource
{
    crd::resources::ResourceHandle<crd::shader::ShaderResource> vertex_shader;
    crd::resources::ResourceHandle<crd::shader::ShaderResource> fragment_shader;
};

// Register the MaterialResourceLoader (handles kFourCC_MATR) with the given manager.
// Both ShaderResourceLoader and MaterialResourceLoader must be registered before
// any material is loaded (vertex/fragment deps are loaded transitively).
void register_material_loader(crd::resources::ResourceManager* rm);

} // namespace crd::renderer
