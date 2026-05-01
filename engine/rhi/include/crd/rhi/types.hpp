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

enum class Format : crd::u16
{
    Undefined,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
    R32G32Sfloat,
    R32G32B32Sfloat,
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
    TransferSrc = 1u << 0u,
    TransferDst = 1u << 1u,
    Vertex    = 1u << 2u,
    Index     = 1u << 3u,
    Uniform   = 1u << 4u,
    Storage   = 1u << 5u,
};

enum class ImageUsage : crd::u32
{
    None                  = 0,
    TransferSrc           = 1u << 0u,
    TransferDst           = 1u << 1u,
    Sampled               = 1u << 2u,
    ColorAttachment       = 1u << 3u,
    DepthStencilAttachment = 1u << 4u,
    Storage               = 1u << 5u,
};

// ShaderStage is a bitmask so multiple stages can be combined in push constant ranges
// and descriptor bindings. Use operator| to combine:  ShaderStage::Vertex | ShaderStage::Fragment
enum class ShaderStage : crd::u8
{
    Vertex   = 1u << 0u,
    Fragment = 1u << 1u,
    Compute  = 1u << 2u,
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

enum class ImageAccess : crd::u8
{
    Undefined,   // initial / don't-care; no layout commitment
    ColorWrite,  // color attachment output write
    DepthWrite,  // depth/stencil attachment read-write
    DepthRead,   // depth attachment read-only (test without write)
    ShaderRead,  // sampled in a shader stage
    TransferSrc, // copy source
    TransferDst, // copy destination
    Present,     // swapchain presentation
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
    return (bits & enum_bits(flag)) != 0u;
}

[[nodiscard]] constexpr bool has_flag(crd::u32 bits, ImageUsage flag) noexcept
{
    return (bits & enum_bits(flag)) != 0u;
}

[[nodiscard]] constexpr bool has_stage(ShaderStage mask, ShaderStage stage) noexcept
{
    return (static_cast<crd::u8>(mask) & static_cast<crd::u8>(stage)) != 0u;
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
    crd::f32 r = 0.0f;
    crd::f32 g = 0.0f;
    crd::f32 b = 0.0f;
    crd::f32 a = 1.0f;
};

struct ClearDepthStencilValue
{
    crd::f32 depth   = 1.0f;
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
    bool enable_depth_test = false;
    bool enable_blend      = false;
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
} // namespace crd::rhi
