#include <crd/scene/component_registry.hpp>

namespace crd::scene
{

ComponentRegistry::ComponentRegistry(crd::memory::IAllocator* alloc) : m_infos(alloc), m_id_by_key(alloc) {}

} // namespace crd::scene
