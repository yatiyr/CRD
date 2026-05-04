#pragma once

#include <crd/containers/array.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/shader/types.hpp>

namespace crd::resources
{
class ResourceManager;
} // namespace crd::resources

namespace crd::shader
{

// Runtime payload for a cooked SHDR artifact.
// Holds the raw SPIRV bytes plus reflection metadata derived from them at load time.
struct ShaderResource
{
    Stage                                      stage = Stage::Vertex;
    crd::containers::Array<crd::u8>            spirv;
    crd::containers::Array<DescriptorBindingDesc>    descriptor_bindings;
    crd::containers::Array<PushConstantRangeDesc>    push_constants;
    crd::containers::Array<VertexAttributeLayoutDesc> vertex_attributes;

    explicit ShaderResource(crd::memory::IAllocator* a)
        : spirv(a), descriptor_bindings(a), push_constants(a), vertex_attributes(a)
    {
    }
};

// Register the ShaderResourceLoader (handles kFourCC_SHDR) with the given manager.
// Call once at startup, before any mount_manifest().
void register_shader_loader(crd::resources::ResourceManager* rm);

} // namespace crd::shader
