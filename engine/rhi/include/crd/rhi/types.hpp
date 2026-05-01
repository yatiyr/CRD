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
    None = 0,
    TransferSrc = 1u << 0u,
    TransferDst = 1u << 1u,
    Vertex = 1u << 2u,
    Index = 1u << 3u,
    Uniform = 1u << 4u,
};

enum class ImageUsage : crd::u32
{
    None = 0,
    TransferSrc = 1u << 0u,
    TransferDst = 1u << 1u,
    Sampled = 1u << 2u,
    ColorAttachment = 1u << 3u,
    DepthStencilAttachment = 1u << 4u,
};

enum class ShaderStage : crd::u8
{
    Vertex,
    Fragment,
    Compute,
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

[[nodiscard]] constexpr bool has_flag(crd::u32 bits, BufferUsage flag) noexcept
{
    return (bits & enum_bits(flag)) != 0u;
}

[[nodiscard]] constexpr bool has_flag(crd::u32 bits, ImageUsage flag) noexcept
{
    return (bits & enum_bits(flag)) != 0u;
}

struct Extent2D
{
    crd::u32 width = 0;
    crd::u32 height = 0;
};

struct Rect2D
{
    crd::i32 x = 0;
    crd::i32 y = 0;
    crd::u32 width = 0;
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
    crd::f32 depth = 1.0f;
    crd::u32 stencil = 0;
};

struct AdapterInfo
{
    crd::containers::String name{};
    AdapterType type = AdapterType::Other;
    crd::u64 dedicated_video_memory_bytes = 0;
    bool supports_graphics = true;
    bool supports_present = true;
};

struct DeviceDesc
{
    crd::u32 frames_in_flight = 2;
    crd::u32 preferred_adapter_index = 0;
};

struct InstanceDesc
{
    crd::containers::String application_name{"Cerid"};
    bool enable_validation = true;
};

struct SwapchainDesc
{
    void* native_window_handle = nullptr;
    Extent2D extent{1280, 720};
    Format color_format = Format::B8G8R8A8Unorm;
    PresentMode present_mode = PresentMode::Fifo;
    crd::u32 image_count = 2;
};

struct BufferDesc
{
    crd::u64 size_bytes = 0;
    crd::u32 usage = enum_bits(BufferUsage::None);
    MemoryUsage memory_usage = MemoryUsage::GpuOnly;
};

struct ImageDesc
{
    Extent2D extent{};
    Format format = Format::Undefined;
    crd::u32 usage = enum_bits(ImageUsage::None);
    crd::u32 mip_levels = 1;
    crd::u32 array_layers = 1;
};

struct ShaderModuleDesc
{
    ShaderStage stage = ShaderStage::Vertex;
    crd::containers::StringView entry_point = "main";
    crd::containers::ConstSpan<crd::u8> code{};
};

struct VertexBindingDesc
{
    crd::u32 binding = 0;
    crd::u32 stride_bytes = 0;
    VertexInputRate input_rate = VertexInputRate::Vertex;
};

struct VertexAttributeDesc
{
    crd::u32 location = 0;
    crd::u32 binding = 0;
    Format format = Format::Undefined;
    crd::u32 offset_bytes = 0;
};

struct GraphicsPipelineDesc
{
    class ShaderModule* vertex_shader = nullptr;
    class ShaderModule* fragment_shader = nullptr;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    Extent2D viewport_extent{1280, 720};
    Format color_format = Format::B8G8R8A8Unorm;
    Format depth_format = Format::Undefined;
    crd::containers::ConstSpan<VertexBindingDesc> vertex_bindings{};
    crd::containers::ConstSpan<VertexAttributeDesc> vertex_attributes{};
    bool enable_depth_test = false;
    bool enable_blend = false;
};

struct RenderingColorAttachmentInfo
{
    class Image* image = nullptr;
    LoadOp load_op = LoadOp::Clear;
    StoreOp store_op = StoreOp::Store;
    ClearColorValue clear_color{};
};

struct RenderingDepthAttachmentInfo
{
    class Image* image = nullptr;
    LoadOp load_op = LoadOp::Clear;
    StoreOp store_op = StoreOp::Store;
    ClearDepthStencilValue clear_depth_stencil{};
};

struct RenderingInfo
{
    Extent2D extent{};
    RenderingColorAttachmentInfo color_attachment{};
    const RenderingDepthAttachmentInfo* depth_attachment = nullptr;
};
} // namespace crd::rhi
