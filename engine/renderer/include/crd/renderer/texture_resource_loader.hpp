#pragma once

namespace crd::resources
{
class ResourceManager;
}

namespace crd::renderer
{

// Register the TXTR loader with the given ResourceManager.
// Call once during startup, before any texture loads.
void register_texture_loader(crd::resources::ResourceManager* rm);

} // namespace crd::renderer
