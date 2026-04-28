#pragma once

#include <crd/rhi/instance.hpp>

#include <memory>

namespace crd::rhi
{
[[nodiscard]] std::unique_ptr<Instance> create_vulkan_instance(const InstanceDesc& desc = InstanceDesc{});
} // namespace crd::rhi
