#pragma once

// Material system — render-path-agnostic resource binding.
//
// Frequency layout (set 0 = per-frame, set 1 = per-material, push = per-draw):
//
//   MaterialBindLayout    describes the set-1 descriptor layout for a material family
//                         (e.g. "PBR: binding 0 = material params UBO")
//
//   MaterialBindGroup     a concrete set of resource bindings for one material.
//                         Allocated each frame from a DescriptorAllocator.
//                         Update it after begin_frame(), bind it before draw.
//
// Typical frame loop:
//   allocator->begin_frame(frame_index);
//   auto group = layout.create_bind_group(*allocator);
//   group->update_buffer(0, *params_ubo);
//   cmd.bind_descriptor_sets(*pipeline_layout, 1, {&group->descriptor_set()});

#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/device.hpp>

#include <memory>

namespace crd::renderer
{

// Describes the descriptor set layout used by a family of materials (set 1).
// Create once at startup; reuse across frames and material bind groups.
class MaterialBindLayout
{
public:
    // Factory: allocates the DescriptorSetLayout on the given device.
    [[nodiscard]] static std::unique_ptr<MaterialBindLayout>
    create(crd::rhi::Device& device, const crd::rhi::DescriptorSetLayoutDesc& desc);

    [[nodiscard]] crd::rhi::DescriptorSetLayout& descriptor_set_layout() noexcept
    {
        return *m_set_layout;
    }

    [[nodiscard]] const crd::rhi::DescriptorSetLayout& descriptor_set_layout() const noexcept
    {
        return *m_set_layout;
    }

    // Allocate one MaterialBindGroup bound to this layout.
    // Call once per visible material per frame, after allocator->begin_frame().
    [[nodiscard]] std::unique_ptr<class MaterialBindGroup>
    create_instance(crd::rhi::DescriptorAllocator& allocator) const;

private:
    explicit MaterialBindLayout(std::unique_ptr<crd::rhi::DescriptorSetLayout> set_layout)
        : m_set_layout(std::move(set_layout))
    {
    }

    std::unique_ptr<crd::rhi::DescriptorSetLayout> m_set_layout;
};

// One concrete binding table for a material instance (set 1).
// Created via MaterialBindLayout::create_instance(). Lifetime: one frame.
// Do NOT hold across frame boundaries — the underlying pool is recycled.
class MaterialBindGroup
{
public:
    explicit MaterialBindGroup(std::unique_ptr<crd::rhi::DescriptorSet> descriptor_set)
        : m_set(std::move(descriptor_set))
    {
    }

    // Bind a buffer at the given slot (must match the MaterialBindLayout binding index).
    // size_bytes == 0 means VK_WHOLE_SIZE (full buffer range).
    void update_buffer(crd::u32 binding, crd::rhi::Buffer& buffer,
                       crd::u64 offset_bytes = 0, crd::u64 size_bytes = 0)
    {
        m_set->update_buffer(binding, buffer, offset_bytes, size_bytes);
    }

    [[nodiscard]] crd::rhi::DescriptorSet& descriptor_set() noexcept { return *m_set; }
    [[nodiscard]] const crd::rhi::DescriptorSet& descriptor_set() const noexcept { return *m_set; }

private:
    std::unique_ptr<crd::rhi::DescriptorSet> m_set;
};

} // namespace crd::renderer
