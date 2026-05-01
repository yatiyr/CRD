#include <crd/renderer/material.hpp>

namespace crd::renderer
{

std::unique_ptr<MaterialLayout>
MaterialLayout::create(crd::rhi::Device& device, const crd::rhi::DescriptorSetLayoutDesc& desc)
{
    auto set_layout = device.create_descriptor_set_layout(desc);
    if (set_layout == nullptr)
    {
        return nullptr;
    }
    return std::unique_ptr<MaterialLayout>(new MaterialLayout(std::move(set_layout)));
}

std::unique_ptr<MaterialInstance>
MaterialLayout::create_instance(crd::rhi::DescriptorAllocator& allocator) const
{
    auto descriptor_set = allocator.allocate(*m_set_layout);
    if (descriptor_set == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<MaterialInstance>(std::move(descriptor_set));
}

} // namespace crd::renderer
