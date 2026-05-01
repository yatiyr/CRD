#include <crd/renderer/frame_graph.hpp>
#include <crd/core/assert.hpp>

namespace crd::renderer
{

// ---- FrameResources -------------------------------------------------------

FrameResources::FrameResources(FrameGraph& graph) noexcept : m_graph(&graph) {}

rhi::Image* FrameResources::get(ImageHandle handle) const noexcept
{
    return m_graph->resolve(handle);
}

// ---- PassBuilder ----------------------------------------------------------

PassBuilder::PassBuilder(FrameGraph* graph, crd::u32 pass_index) noexcept
    : m_graph(graph), m_pass_index(pass_index)
{
}

void PassBuilder::read(ImageHandle handle, rhi::ImageAccess access)
{
    m_graph->m_passes[m_pass_index].reads.push_back({handle, access});
}

void PassBuilder::write(ImageHandle handle, rhi::ImageAccess access)
{
    m_graph->m_passes[m_pass_index].writes.push_back({handle, access});
}

void PassBuilder::set_execute(std::function<void(FrameResources&, rhi::CommandBuffer&)> fn)
{
    m_graph->m_passes[m_pass_index].execute_fn = std::move(fn);
}

// ---- FrameGraph -----------------------------------------------------------

ImageHandle FrameGraph::create_transient(const TransientImageDesc& desc)
{
    const crd::u32 index = static_cast<crd::u32>(m_resources.size());
    ImageResource resource;
    resource.kind = ResourceKind::Transient;
    resource.transient_desc = desc;
    resource.initial_access = rhi::ImageAccess::Undefined;
    m_resources.push_back(std::move(resource));
    return {index};
}

ImageHandle FrameGraph::import(rhi::Image* image, rhi::ImageAccess current_access)
{
    const crd::u32 index = static_cast<crd::u32>(m_resources.size());
    ImageResource resource;
    resource.kind = ResourceKind::External;
    resource.initial_access = current_access;
    resource.image = image;
    m_resources.push_back(std::move(resource));
    return {index};
}

PassBuilder FrameGraph::add_pass(const char* name)
{
    const crd::u32 index = static_cast<crd::u32>(m_passes.size());
    PassNode node;
    node.name = name;
    m_passes.push_back(std::move(node));
    return PassBuilder{this, index};
}

bool FrameGraph::build()
{
    m_built = false;

    const crd::u32 resource_count = static_cast<crd::u32>(m_resources.size());

    // Seed per-resource tracking state. External resources count as "already written"
    // (they arrive with a valid layout); transient resources must be written before read.
    for (crd::u32 i = 0; i < resource_count; ++i)
    {
        m_resources[i].tracked_access    = m_resources[i].initial_access;
        m_resources[i].written_this_build = (m_resources[i].kind == ResourceKind::External);
    }

    for (auto& pass : m_passes)
    {
        pass.barriers_before.clear();

        for (const auto& use : pass.reads)
        {
            if (!use.handle.is_valid() || use.handle.index >= resource_count)
                return false;
            if (!m_resources[use.handle.index].written_this_build)
                return false; // reading a transient that hasn't been written
            const rhi::ImageAccess prev = m_resources[use.handle.index].tracked_access;
            if (prev != use.access)
                pass.barriers_before.push_back({use.handle, prev, use.access});
            // reads don't update tracked_access
        }

        for (const auto& use : pass.writes)
        {
            if (!use.handle.is_valid() || use.handle.index >= resource_count)
                return false;
            const rhi::ImageAccess prev = m_resources[use.handle.index].tracked_access;
            if (prev != use.access)
                pass.barriers_before.push_back({use.handle, prev, use.access});
            m_resources[use.handle.index].tracked_access    = use.access;
            m_resources[use.handle.index].written_this_build = true;
        }
    }

    m_built = true;
    return true;
}

void FrameGraph::execute(rhi::Device& device, rhi::CommandBuffer& cmd)
{
    CRD_ASSERT(m_built);

    // Allocate transient images on first execute
    for (auto& resource : m_resources)
    {
        if (resource.kind == ResourceKind::Transient && resource.owned_image == nullptr)
        {
            rhi::ImageDesc desc;
            desc.extent = resource.transient_desc.extent;
            desc.format = resource.transient_desc.format;
            desc.usage = resource.transient_desc.usage;
            resource.owned_image = device.create_image(desc);
            resource.image = resource.owned_image.get();
        }
    }

    FrameResources frame_resources{*this};

    for (const auto& pass : m_passes)
    {
        for (const auto& barrier : pass.barriers_before)
        {
            rhi::Image* img = resolve(barrier.handle);
            CRD_ASSERT(img != nullptr);
            cmd.transition_image(*img, barrier.from, barrier.to);
        }
        if (pass.execute_fn)
            pass.execute_fn(frame_resources, cmd);
    }
}

void FrameGraph::reset()
{
    m_passes.clear();
    m_resources.clear(); // also destroys owned transient images
    m_built = false;
}

rhi::Image* FrameGraph::resolve(ImageHandle handle) const noexcept
{
    if (!handle.is_valid() || handle.index >= static_cast<crd::u32>(m_resources.size()))
        return nullptr;
    return m_resources[handle.index].image;
}

} // namespace crd::renderer
