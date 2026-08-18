#pragma once

// vulkan_dgc_context.hpp — CEIR-20c-2: the Vulkan DEVICE-GENERATED-COMMANDS context (VK_EXT_device_generated_commands), the
// CROSS-VENDOR third lowering of a ceir.work program. Where 20b host-records ONE vkCmdDispatchIndirect (device supplies only the
// grid) and 20c-1 (D3D12 Work Graphs) GPU-schedules a node launch, this rig has the GPU AUTHOR THE COMMAND STREAM ITSELF: a
// produce compute kernel writes both the SEQUENCE COUNT and the per-sequence DISPATCH payloads, then vkCmdExecuteGeneratedCommandsEXT
// GENERATES that many separate dispatch commands of a consume kernel — a thing a single dispatch-indirect literally cannot express.
// The minimal, spec-correct EXT shape: an indirect commands layout of a lone DISPATCH token, indirectExecutionSet = VK_NULL_HANDLE
// (the pipeline supplied via VkGeneratedCommandsPipelineInfoEXT in pNext), sequenceCountAddress = the device-written count. It owns
// its command pool, descriptor pool, pipelines, and device-address buffers, reusing the shared VkDevice from the VulkanGpuContext
// (the one-device unification). ⛔ Mandate #1: the produce/consume kernels are AUTHORED .ckir; this rig only cooks + dispatches them.

#include <crd/gpu/vulkan_context.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <memory>

namespace crd::gpu
{

// The offline rig for a device-generated-commands execution. valid() reports the adapter capability (the EXT + maintenance5 + a
// COMPUTE-capable command generator); a caller soft-skips its gate when it is false.
class VulkanDgcContext
{
public:
    explicit VulkanDgcContext(VulkanGpuContext& ctx);
    ~VulkanDgcContext();
    VulkanDgcContext(const VulkanDgcContext&)            = delete;
    VulkanDgcContext& operator=(const VulkanDgcContext&) = delete;

    // True when the adapter enables VK_EXT_device_generated_commands (cross-vendor) with COMPUTE generation + maintenance5 + BDA.
    [[nodiscard]] bool valid() const noexcept;

    // One global storage buffer in the execution. `upload != nullptr` copies host bytes in before the produce dispatch; `readback
    // != nullptr` copies device bytes out after execute; `bytes` sizes the device buffer. Every buffer is host-visible + carries a
    // device address (the token/count buffers are referenced by address; the rest are harmless supersets).
    struct Buffer
    {
        const void* upload   = nullptr;
        void*       readback = nullptr;
        crd::u64    bytes    = 0;
    };

    // The device-generated-commands execution. The produce kernel (`produce_spirv`, dispatched 1×1×1) writes the token stream +
    // the sequence count; then vkCmdExecuteGeneratedCommandsEXT generates one DISPATCH command per sequence of the consume kernel
    // (`consume_spirv`). `produce_binds`/`consume_binds` map each pipeline's std430 binding N (== the .ckir BufferDecl iidx the
    // GLSL emitter emits) to a global buffer index in `buffers`. `token_buffer` → the layout's indirectAddress (the command
    // stream), `count_buffer` → sequenceCountAddress (the device-written count). `dispatch_stride` = bytes per sequence in the
    // token buffer (sizeof VkDispatchIndirectCommand = 12). `max_seq` upper-bounds the sequence count for preprocess sizing.
    struct ExecuteDesc
    {
        crd::containers::ConstSpan<crd::u8> produce_spirv;
        crd::containers::ConstSpan<crd::u8> consume_spirv;
        const crd::u32*                     produce_binds   = nullptr;
        crd::u32                            n_produce_binds = 0;
        const crd::u32*                     consume_binds   = nullptr;
        crd::u32                            n_consume_binds = 0;
        crd::u32                            token_buffer    = 0;
        crd::u32                            count_buffer    = 0;
        crd::u32                            max_seq         = 0;
        crd::u32                            dispatch_stride = 12;
    };

    // Run the execution: build buffers/pipelines/descriptors, dispatch the produce, barrier its writes to the command-preprocess +
    // consume-read stages, vkCmdExecuteGeneratedCommandsEXT, wait, read back. Returns false on any setup failure or when !valid().
    [[nodiscard]] bool dispatch_generated(const ExecuteDesc& desc, crd::containers::ConstSpan<Buffer> buffers);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::gpu
