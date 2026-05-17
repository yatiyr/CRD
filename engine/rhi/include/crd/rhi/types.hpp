#pragma once

#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::rhi
{
enum class BackendApi : crd::u8
{
    Vulkan,
};

enum class AdapterType : crd::u8
{
    Other,
    IntegratedGpu,
    DiscreteGpu,
    VirtualGpu,
    Cpu,
};

// Depth comparison ops. Maps to Vulkan VK_COMPARE_OP_*. Default for
// pipelines is GreaterOrEqual (Cerid's reverse-Z convention: closer =
// larger depth value, so visible-when-closer means GREATER_OR_EQUAL).
//
// Used by crd-draw d2-depth to express XRay's "occluded portion" via the
// inverted Less compare op alongside the standard Test pipeline.
enum class DepthCompareOp : crd::u8
{
    Never          = 0,
    Less           = 1,
    Equal          = 2,
    LessOrEqual    = 3,
    Greater        = 4,
    NotEqual       = 5,
    GreaterOrEqual = 6,
    Always         = 7
};

enum class Format : crd::u16
{
    Undefined,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
    R32G32Sfloat,
    R32G32B32Sfloat,
    R32G32B32A32Sfloat,
    R32Uint,    // 32-bit unsigned (vertex attr packed colors / flags)
    R32Sfloat,  // 32-bit float (vertex attr scalar widths / weights)
    D24UnormS8Uint,
    D32Sfloat,
};

enum class PresentMode : crd::u8
{
    Immediate,
    Fifo,
    Mailbox,
};

enum class MemoryUsage : crd::u8
{
    GpuOnly,
    CpuToGpu,
    GpuToCpu,
};

enum class BufferUsage : crd::u32
{
    None      = 0,
    TransferSrc = 1U << 0U,
    TransferDst = 1U << 1U,
    Vertex    = 1U << 2U,
    Index     = 1U << 3U,
    Uniform   = 1U << 4U,
    Storage   = 1U << 5U,
    // Phase 3.1.7.6 v0b (ADR-0080) — indirect dispatch / draw arguments
    // buffer. VkDispatchIndirectCommand (3 × u32 workgroup counts) for
    // dispatch_indirect; VkDrawIndirectCommand for future draw_indirect.
    Indirect  = 1U << 6U,
};

enum class ImageUsage : crd::u32
{
    None                  = 0,
    TransferSrc           = 1U << 0U,
    TransferDst           = 1U << 1U,
    Sampled               = 1U << 2U,
    ColorAttachment       = 1U << 3U,
    DepthStencilAttachment = 1U << 4U,
    Storage               = 1U << 5U,
};

// ShaderStage is a bitmask so multiple stages can be combined in push constant ranges
// and descriptor bindings. Use operator| to combine:  ShaderStage::Vertex | ShaderStage::Fragment
enum class ShaderStage : crd::u8
{
    Vertex   = 1U << 0U,
    Fragment = 1U << 1U,
    Compute  = 1U << 2U,
};

// DescriptorType mirrors the Vulkan descriptor type taxonomy. Only the types listed
// here are supported at this layer; more can be added as the material system grows.
enum class DescriptorType : crd::u8
{
    UniformBuffer,         // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    StorageBuffer,         // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    CombinedImageSampler,  // VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    SampledImage,          // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
    Sampler,               // VK_DESCRIPTOR_TYPE_SAMPLER
    StorageImage,          // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};

enum class PrimitiveTopology : crd::u8
{
    TriangleList,
};

enum class IndexType : crd::u8
{
    Uint16,
    Uint32,
};

enum class VertexInputRate : crd::u8
{
    Vertex,
    Instance,
};

enum class LoadOp : crd::u8
{
    Load,
    Clear,
    DontCare,
};

enum class StoreOp : crd::u8
{
    Store,
    DontCare,
};

// Phase 3.1.7.6 v0d (ADR-0080 D10) — pipeline stage enum for semaphore
// wait masks. Distinct from BufferAccess: semaphore wait gates a stage
// (where the receiving queue blocks until signal) and does NOT carry
// an access mask (semaphores already imply memory visibility). Kept
// minimal — extend when a real consumer needs finer granularity.
enum class PipelineStage : crd::u8
{
    Top,             // VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT — over-syncs; use only when stage unknown
    ComputeShader,   // VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
    VertexInput,     // VK_PIPELINE_STAGE_VERTEX_INPUT_BIT — vertex pull from buffer
    VertexShader,    // VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
    FragmentShader,  // VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
    ColorAttachment, // VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
    Transfer,        // VK_PIPELINE_STAGE_TRANSFER_BIT
    BottomOfPipe,    // VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT — signal at end of work
};

// Phase 3.1.7.6 v0d (ADR-0080 D9) — async compute queue selection policy.
enum class AsyncComputePolicy : crd::u8
{
    // Try to find a dedicated compute queue family
    // (VK_QUEUE_COMPUTE_BIT && !VK_QUEUE_GRAPHICS_BIT). If absent,
    // `Device::compute_queue()` returns the same Queue& as
    // `graphics_queue()` (single VkQueue, serialized work). Default —
    // matches the broadest hardware envelope.
    FallbackGracefully,
    // Fail `Instance::create_device` if no dedicated compute queue
    // family exists. Use when downstream perf depends on true async
    // overlap (eylem v8 GPU broadphase, future).
    RequireDedicated,
};

enum class ImageAccess : crd::u8
{
    Undefined,   // initial / don't-care; no layout commitment
    ColorWrite,  // color attachment output write
    DepthWrite,  // depth/stencil attachment read-write
    DepthRead,   // depth attachment read-only (test without write)
    ShaderRead,  // sampled in fragment shader (legacy graphics-only name; equivalent to FragmentShaderRead)
    TransferSrc, // copy source
    TransferDst, // copy destination
    Present,     // swapchain presentation
    // Phase 3.1.7.6 v0c (ADR-0080 D8) — compute-stage image access.
    // ShaderRead above stays graphics-stage-only for back-compat with
    // existing transition_image consumers (renderer, frame graph).
    // New compute consumers (storage images, future v9 GPU-side BVH
    // viz, mesh-baked SDF) use these explicit variants.
    ComputeShaderRead,      // sampled / read in compute shader
    ComputeShaderWrite,     // written via storage image in compute
    ComputeShaderReadWrite, // both — typical SSBO-like image pattern
};

// Phase 3.1.7.6 v0c (ADR-0080 D8) — typed-enum buffer access state for
// `CommandBuffer::buffer_barrier`. Granular per Vulkan pipeline stage so
// the impl picks ONE specific srcStageMask/dstStageMask pair rather than
// over-barrier with a pessimistic graphics-wide mask. Mirrors ImageAccess
// shape but for buffers (no image-layout dimension).
enum class BufferAccess : crd::u8
{
    None,                    // initial / don't-care; no access
    ComputeShaderRead,       // compute shader reads buffer (SSBO read, UBO read)
    ComputeShaderWrite,      // compute shader writes buffer (SSBO write)
    ComputeShaderReadWrite,  // compute shader read+write (atomics, in-place transform)
    VertexShaderRead,        // vertex shader reads buffer (SSBO bound at vertex stage)
    FragmentShaderRead,      // fragment shader reads buffer (UBO/SSBO at fragment stage)
    VertexAttributeRead,     // pulled via vertex input (vkCmdBindVertexBuffers consumer)
    IndexRead,               // index buffer consumed by vkCmdDrawIndexed
    UniformRead,             // generic uniform-buffer read (any shader stage)
    IndirectRead,            // dispatch/draw indirect command read
    TransferSrc,             // vkCmdCopyBuffer source
    TransferDst,             // vkCmdCopyBuffer destination, vkCmdFillBuffer destination
    HostRead,                // host (CPU) reads via mapped pointer
    HostWrite,               // host (CPU) writes via mapped pointer
};

template <typename EnumType>
[[nodiscard]] constexpr auto enum_bits(EnumType value) noexcept -> std::underlying_type_t<EnumType>
{
    return static_cast<std::underlying_type_t<EnumType>>(value);
}

[[nodiscard]] constexpr crd::u32 operator|(BufferUsage a, BufferUsage b) noexcept
{
    return enum_bits(a) | enum_bits(b);
}

[[nodiscard]] constexpr crd::u32 operator|(ImageUsage a, ImageUsage b) noexcept
{
    return enum_bits(a) | enum_bits(b);
}

// ShaderStage bitmask operators — combine stages for push constant ranges / descriptor visibility.
[[nodiscard]] constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) noexcept
{
    return static_cast<ShaderStage>(static_cast<crd::u8>(a) | static_cast<crd::u8>(b));
}

[[nodiscard]] constexpr bool has_flag(crd::u32 bits, BufferUsage flag) noexcept
{
    return (bits & enum_bits(flag)) != 0U;
}

[[nodiscard]] constexpr bool has_flag(crd::u32 bits, ImageUsage flag) noexcept
{
    return (bits & enum_bits(flag)) != 0U;
}

[[nodiscard]] constexpr bool has_stage(ShaderStage mask, ShaderStage stage) noexcept
{
    return (static_cast<crd::u8>(mask) & static_cast<crd::u8>(stage)) != 0U;
}

// One slot in a DescriptorSetLayout. `count` > 1 declares an inline array of that type.
struct DescriptorBinding
{
    crd::u32       binding = 0;
    DescriptorType type    = DescriptorType::UniformBuffer;
    crd::u32       count   = 1; // array size; 1 for non-arrays
    ShaderStage    stages  = ShaderStage::Fragment;
};

// Input to Device::create_descriptor_set_layout().
struct DescriptorSetLayoutDesc
{
    crd::containers::ConstSpan<DescriptorBinding> bindings{};
};

// One push constant range. Offset and size are in bytes; both must be multiples of 4.
// Vulkan guarantees at least 128 bytes of push constant space on all hardware.
struct PushConstantRange
{
    ShaderStage stages = ShaderStage::Vertex | ShaderStage::Fragment;
    crd::u32    offset = 0;
    crd::u32    size   = 0;
};

// Input to Device::create_pipeline_layout().
// set_layouts[i] is bound at descriptor set index i (set 0 = per-frame, set 1 = per-material).
struct PipelineLayoutDesc
{
    crd::containers::ConstSpan<const class DescriptorSetLayout*> set_layouts{};
    crd::containers::ConstSpan<PushConstantRange>                push_constant_ranges{};
};

// Descriptor ring allocator configuration.
// Size per-type counts to the maximum you expect to bind in a single frame.
// The allocator holds `frames_in_flight` pools and resets the oldest one each frame.
struct DescriptorAllocatorDesc
{
    crd::u32 frames_in_flight                    = 2;
    crd::u32 max_sets_per_frame                  = 512;
    crd::u32 max_uniform_buffers_per_frame        = 1024;
    crd::u32 max_storage_buffers_per_frame        = 256;
    crd::u32 max_combined_image_samplers_per_frame = 512;
    crd::u32 max_sampled_images_per_frame         = 256;
    crd::u32 max_storage_images_per_frame         = 64;
    crd::u32 max_samplers_per_frame               = 64;
};

struct Extent2D
{
    crd::u32 width  = 0;
    crd::u32 height = 0;
};

struct Rect2D
{
    crd::i32 x      = 0;
    crd::i32 y      = 0;
    crd::u32 width  = 0;
    crd::u32 height = 0;
};

struct ClearColorValue
{
    crd::f32 r = 0.0F;
    crd::f32 g = 0.0F;
    crd::f32 b = 0.0F;
    crd::f32 a = 1.0F;
};

struct ClearDepthStencilValue
{
    crd::f32 depth   = 1.0F;
    crd::u32 stencil = 0;
};

struct AdapterInfo
{
    crd::containers::String name{};
    AdapterType type = AdapterType::Other;
    crd::u64 dedicated_video_memory_bytes = 0;
    bool supports_graphics = true;
    bool supports_present  = true;
};

struct DeviceDesc
{
    crd::u32 frames_in_flight      = 2;
    crd::u32 preferred_adapter_index = 0;
    // Phase 3.1.7.6 v0d (ADR-0080 D9) — async compute queue selection.
    AsyncComputePolicy async_compute_policy = AsyncComputePolicy::FallbackGracefully;
};

// Phase 3.1.7.6 v0d (ADR-0080 D10) — semaphore wait specification for
// Queue::submit(SubmitInfo). The waiting queue blocks at `wait_stage`
// until `semaphore` is signaled by another queue's submission.
struct SemaphoreWait
{
    class Semaphore* semaphore = nullptr;
    PipelineStage    wait_stage = PipelineStage::Top;
};

// Phase 3.1.7.6 v0d (ADR-0080 D10) — full submit specification. Single
// source of truth for queue submission shape; existing back-compat
// overloads (`submit(cmd, fence)`, `submit(cmd, swapchain)`,
// `submit_and_wait`) stay alongside this for v0a/v0b/v0c callers that
// don't need cross-queue semaphores.
struct SubmitInfo
{
    class CommandBuffer*                          command_buffer    = nullptr;
    class Fence*                                  signal_fence      = nullptr;
    crd::containers::ConstSpan<SemaphoreWait>     wait_semaphores{};
    crd::containers::ConstSpan<class Semaphore*>  signal_semaphores{};
};

struct InstanceDesc
{
    crd::containers::String application_name{"Cerid"};
    bool enable_validation = true;
};

struct SwapchainDesc
{
    void*       native_window_handle = nullptr;
    Extent2D    extent{1280, 720};
    Format      color_format  = Format::B8G8R8A8Unorm;
    PresentMode present_mode  = PresentMode::Fifo;
    crd::u32    image_count   = 2;
};

struct BufferDesc
{
    crd::u64    size_bytes   = 0;
    crd::u32    usage        = enum_bits(BufferUsage::None);
    MemoryUsage memory_usage = MemoryUsage::GpuOnly;
};

struct ImageDesc
{
    Extent2D extent{};
    Format   format       = Format::Undefined;
    crd::u32 usage        = enum_bits(ImageUsage::None);
    crd::u32 mip_levels   = 1;
    crd::u32 array_layers = 1;
};

struct ShaderModuleDesc
{
    ShaderStage                         stage       = ShaderStage::Vertex;
    crd::containers::StringView         entry_point = "main";
    crd::containers::ConstSpan<crd::u8> code{};
};

struct VertexBindingDesc
{
    crd::u32        binding      = 0;
    crd::u32        stride_bytes = 0;
    VertexInputRate input_rate   = VertexInputRate::Vertex;
};

struct VertexAttributeDesc
{
    crd::u32 location     = 0;
    crd::u32 binding      = 0;
    Format   format       = Format::Undefined;
    crd::u32 offset_bytes = 0;
};

// Phase 3.1.7.6 v0b (ADR-0080 D6) — specialization constants baked at
// pipeline create-time. One entry per `layout(constant_id = N) const`
// declared in the compute shader. `offset` + `size` index into a
// caller-provided `specialization_data` byte blob. Matches Vulkan's
// VkSpecializationMapEntry 1:1.
//
// Per-dispatch parameters use push constants instead (graphics
// `push_constants` API already handles compute via the ShaderStage
// mask — no compute-specific method).
struct SpecializationConstantEntry
{
    crd::u32 constant_id = 0;
    crd::u32 offset      = 0; // byte offset into specialization_data
    crd::u32 size        = 0; // byte size of this constant's value
};

// ComputePipelineDesc — input to `Device::create_compute_pipeline`.
//
// Phase 3.1.7.6 v0a (ADR-0080 D1 additive). Narrow surface: a compute
// pipeline is just a compute-stage shader module + a pipeline layout
// (descriptors + push constants). No vertex input / no viewport / no
// raster / no blend — none of those have meaning for compute.
//
// ADR-0080 D2 revision (discovered at v0a, consolidated at v0-close):
// storage buffers reuse the existing `Buffer` interface with
// `BufferUsage::Storage`; no separate `IStorageBuffer` type. The
// existing RHI already abstracted past the per-usage-type split.
//
// ADR-0080 D7 revision: descriptor-set conventions (set 0 = storage,
// set 1 = uniform) are a documented consumer guideline, not a
// type-level enforcement. `pipeline_layout` accepts any caller-
// constructed `PipelineLayout` (matching graphics flexibility).
//
// Phase 3.1.7.6 v0b (ADR-0080 D6) — added `specialization_entries` +
// `specialization_data` for compile-time-baked specialization
// constants. Both spans MUST outlive the create_compute_pipeline call
// (copied into VkSpecializationInfo during pipeline creation; not
// retained afterwards).
struct ComputePipelineDesc
{
    class ShaderModule*   compute_shader  = nullptr;
    class PipelineLayout* pipeline_layout = nullptr;
    crd::containers::ConstSpan<SpecializationConstantEntry> specialization_entries{};
    crd::containers::ConstSpan<crd::u8>                     specialization_data{};
};

struct GraphicsPipelineDesc
{
    class ShaderModule*   vertex_shader   = nullptr;
    class ShaderModule*   fragment_shader = nullptr;
    PrimitiveTopology     topology        = PrimitiveTopology::TriangleList;
    Extent2D              viewport_extent{1280, 720};
    Format                color_format    = Format::B8G8R8A8Unorm;
    Format                depth_format    = Format::Undefined;
    crd::containers::ConstSpan<VertexBindingDesc>   vertex_bindings{};
    crd::containers::ConstSpan<VertexAttributeDesc> vertex_attributes{};
    bool enable_depth_test    = false;
    bool enable_blend         = false;
    bool use_dynamic_viewport = false; // when true, viewport/scissor are set per-cmd (vkCmdSetViewport/Scissor)
    bool wireframe            = false; // VK_POLYGON_MODE_LINE; requires fillModeNonSolid device feature
    bool depth_write          = true;  // set false for overlay passes; ignored when enable_depth_test=false
    // Depth comparison op. Default GreaterOrEqual matches Cerid's reverse-Z
    // convention. Override to Less for "render where occluded" semantics
    // (used by crd-draw d2-depth XRay's GreaterDimmed pipeline).
    DepthCompareOp depth_compare_op = DepthCompareOp::GreaterOrEqual;
    // Optional explicit pipeline layout. nullptr = synthesise an empty layout (no push constants
    // or descriptor sets). Pass an explicit PipelineLayout for push constants + descriptor sets.
    class PipelineLayout* pipeline_layout = nullptr;
};

struct RenderingColorAttachmentInfo
{
    class Image* image    = nullptr;
    LoadOp       load_op  = LoadOp::Clear;
    StoreOp      store_op = StoreOp::Store;
    ClearColorValue clear_color{};
};

struct RenderingDepthAttachmentInfo
{
    class Image* image    = nullptr;
    LoadOp       load_op  = LoadOp::Clear;
    StoreOp      store_op = StoreOp::Store;
    ClearDepthStencilValue clear_depth_stencil{};
};

struct RenderingInfo
{
    Extent2D                            extent{};
    RenderingColorAttachmentInfo        color_attachment{};
    const RenderingDepthAttachmentInfo* depth_attachment = nullptr;
};

// Describes one region of a buffer-to-image copy operation.
struct BufferImageCopy
{
    crd::u64 buffer_offset = 0;
    crd::u32 mip_level     = 0;
    Extent2D extent{};
};
} // namespace crd::rhi
