#pragma once

// Umbrella header for the current crd-rhi slice.
//
// Shipped today (v1a):
//   - API-agnostic low-level GPU types and descriptors
//   - abstract interfaces for Instance / Device / Queue / Swapchain /
//     Buffer / Image / CommandBuffer / ShaderModule / Pipeline
//
// Not shipped today:
//   - Vulkan backend implementation (crd-rhi-vulkan)
//   - materials, scene, lighting, renderer policy

#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/image.hpp>
#include <crd/rhi/instance.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/shader_module.hpp>
#include <crd/rhi/swapchain.hpp>
#include <crd/rhi/types.hpp>
