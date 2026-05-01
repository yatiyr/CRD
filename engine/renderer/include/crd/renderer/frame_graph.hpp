#pragma once

#include <crd/containers/array.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/image.hpp>
#include <crd/rhi/types.hpp>

#include <functional>
#include <memory>

namespace crd::renderer
{

// Typed opaque handle to a frame graph image resource.
struct ImageHandle
{
    static constexpr crd::u32 k_invalid = ~0u;
    crd::u32 index = k_invalid;
    [[nodiscard]] bool is_valid() const noexcept { return index != k_invalid; }
};

// Description for a transient (frame-graph-owned) image.
struct TransientImageDesc
{
    rhi::Extent2D extent{};
    rhi::Format format = rhi::Format::Undefined;
    crd::u32 usage = 0; // rhi::ImageUsage bits
};

class FrameGraph;

// Passed to pass execute callbacks; resolves handles to concrete images.
class FrameResources
{
public:
    [[nodiscard]] rhi::Image* get(ImageHandle handle) const noexcept;

private:
    friend class FrameGraph;
    explicit FrameResources(FrameGraph& graph) noexcept;
    FrameGraph* m_graph = nullptr;
};

// Returned by FrameGraph::add_pass(); used to declare resource uses and the execute callback.
class PassBuilder
{
public:
    // Declare that this pass reads handle in the given access mode.
    void read(ImageHandle handle, rhi::ImageAccess access);
    // Declare that this pass writes handle in the given access mode.
    void write(ImageHandle handle, rhi::ImageAccess access);
    // Set the recording callback; called by FrameGraph::execute() after barriers are inserted.
    void set_execute(std::function<void(FrameResources&, rhi::CommandBuffer&)> fn);

private:
    friend class FrameGraph;
    PassBuilder(FrameGraph* graph, crd::u32 pass_index) noexcept;
    FrameGraph* m_graph = nullptr;
    crd::u32 m_pass_index = 0;
};

// Frame graph: centralises resource lifetime, barrier insertion, and pass execution order.
//
// Typical per-frame usage:
//   frame_graph.reset();
//   auto color = frame_graph.import(swapchain_image, rhi::ImageAccess::Undefined);
//   auto builder = frame_graph.add_pass("main-color");
//   builder.write(color, rhi::ImageAccess::ColorWrite);
//   builder.set_execute([&](FrameResources& res, rhi::CommandBuffer& cmd) { ... });
//   frame_graph.build();
//   frame_graph.execute(device, cmd);
//
// NOTE (v1): passes execute in declaration order. Full topological sort is v2.
// NOTE (v1): transient images are created each frame and destroyed on reset().
class FrameGraph
{
public:
    // --- Resource management ---

    // Declare a transient image (owned by this frame graph, allocated on execute).
    [[nodiscard]] ImageHandle create_transient(const TransientImageDesc& desc);

    // Import an externally-owned image (e.g. swapchain image) with its current access state.
    [[nodiscard]] ImageHandle import(rhi::Image* image, rhi::ImageAccess current_access);

    // --- Pass registration ---

    // Register a new pass. Returns a builder to declare resource uses and the execute callback.
    [[nodiscard]] PassBuilder add_pass(const char* name);

    // --- Lifecycle ---

    // Compile barriers from declared accesses. Call after all add_pass calls.
    // Returns false if a transient resource is read before being written.
    [[nodiscard]] bool build();

    // Allocate transients; insert barriers; run pass callbacks in order.
    void execute(rhi::Device& device, rhi::CommandBuffer& cmd);

    // Clear passes and release transient images. Call at the start of each frame.
    void reset();

    // Resolve a handle to its concrete image pointer (nullptr if invalid).
    [[nodiscard]] rhi::Image* resolve(ImageHandle handle) const noexcept;

private:
    friend class FrameResources;
    friend class PassBuilder;

    struct ResourceUse
    {
        ImageHandle handle{};
        rhi::ImageAccess access = rhi::ImageAccess::Undefined;
    };

    struct Barrier
    {
        ImageHandle handle{};
        rhi::ImageAccess from = rhi::ImageAccess::Undefined;
        rhi::ImageAccess to = rhi::ImageAccess::Undefined;
    };

    struct PassNode
    {
        const char* name = "";
        crd::containers::Array<ResourceUse> reads{};
        crd::containers::Array<ResourceUse> writes{};
        crd::containers::Array<Barrier> barriers_before{};
        std::function<void(FrameResources&, rhi::CommandBuffer&)> execute_fn;
    };

    enum class ResourceKind : crd::u8
    {
        Transient,
        External,
    };

    struct ImageResource
    {
        ResourceKind kind = ResourceKind::Transient;
        TransientImageDesc transient_desc{};
        rhi::ImageAccess initial_access = rhi::ImageAccess::Undefined;
        rhi::Image* image = nullptr;
        std::unique_ptr<rhi::Image> owned_image;
    };

    crd::containers::Array<ImageResource> m_resources{};
    crd::containers::Array<PassNode> m_passes{};
    bool m_built = false;
};

} // namespace crd::renderer
