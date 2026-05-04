#pragma once

namespace crd::resources { class ResourceManager; }

namespace crd::renderer
{

void register_mesh_loader(crd::resources::ResourceManager* rm);

} // namespace crd::renderer
