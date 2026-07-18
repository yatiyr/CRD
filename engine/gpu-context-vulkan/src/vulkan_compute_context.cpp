// vulkan_compute_context.cpp — multi-pass compute recorder over a VulkanGpuContext (ADR-0099, v17-i-c). Persistent
// buffers + cached pipelines + a recorder that records copies/barriers/dispatches into one command buffer and submits
// on the compute queue. The compute layer the geometry-bvh-gpu pipelines migrate onto (off the rendering RHI).

#include <crd/gpu/vulkan_compute_context.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdint>
#include <cstring>

namespace crd::gpu
{
namespace
{
constexpr int kMaxBindings = 8;

[[nodiscard]] VkAccessFlags access_mask(ComputeAccess a) noexcept
{
    switch (a)
    {
    case ComputeAccess::TransferSrc: return VK_ACCESS_TRANSFER_READ_BIT;
    case ComputeAccess::TransferDst: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case ComputeAccess::ShaderRead:  return VK_ACCESS_SHADER_READ_BIT;
    case ComputeAccess::ShaderWrite: return VK_ACCESS_SHADER_WRITE_BIT;
    case ComputeAccess::HostRead:    return VK_ACCESS_HOST_READ_BIT;
    }
    return 0;
}
[[nodiscard]] VkPipelineStageFlags access_stage(ComputeAccess a) noexcept
{
    switch (a)
    {
    case ComputeAccess::TransferSrc:
    case ComputeAccess::TransferDst: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case ComputeAccess::ShaderRead:
    case ComputeAccess::ShaderWrite: return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ComputeAccess::HostRead:    return VK_PIPELINE_STAGE_HOST_BIT;
    }
    return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}

class BufferImpl final : public ComputeBuffer
{
public:
    BufferImpl(VkDevice d, VkBuffer b, VkDeviceMemory m, crd::u64 bytes, bool mappable) noexcept
        : m_device(d), m_buffer(b), m_memory(m), m_bytes(bytes), m_mappable(mappable)
    {
    }
    ~BufferImpl() override
    {
        if (m_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_buffer, nullptr); }
        if (m_memory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_memory, nullptr); }
    }
    [[nodiscard]] void* map() noexcept override
    {
        if (!m_mappable) { return nullptr; }
        void* p = nullptr;
        vkMapMemory(m_device, m_memory, 0, m_bytes, 0, &p);
        return p;
    }
    void unmap() noexcept override { vkUnmapMemory(m_device, m_memory); }

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer; }
    [[nodiscard]] crd::u64 bytes() const noexcept { return m_bytes; }
    // B4: the native VkBuffer, so a compute-written INDIRECT-args buffer can drive the raster context's indirect mesh dispatch.
    [[nodiscard]] void* native_handle() const noexcept override { return reinterpret_cast<void*>(m_buffer); }

private:
    VkDevice       m_device;
    VkBuffer       m_buffer;
    VkDeviceMemory m_memory;
    crd::u64       m_bytes;
    bool           m_mappable;
};

class PipelineImpl final : public ComputePipeline
{
public:
    PipelineImpl(VkDevice d, VkShaderModule sh, VkDescriptorSetLayout sl, VkPipelineLayout pl, VkPipeline p, int nb) noexcept
        : m_device(d), m_shader(sh), m_set_layout(sl), m_pipe_layout(pl), m_pipeline(p), m_nb(nb)
    {
    }
    ~PipelineImpl() override
    {
        if (m_pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pipeline, nullptr); }
        if (m_pipe_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_pipe_layout, nullptr); }
        if (m_set_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr); }
        if (m_shader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_shader, nullptr); }
    }
    [[nodiscard]] VkDescriptorSetLayout set_layout() const noexcept { return m_set_layout; }
    [[nodiscard]] VkPipelineLayout      pipe_layout() const noexcept { return m_pipe_layout; }
    [[nodiscard]] VkPipeline            pipeline() const noexcept { return m_pipeline; }
    [[nodiscard]] int                   nb() const noexcept { return m_nb; }

private:
    VkDevice              m_device;
    VkShaderModule        m_shader;
    VkDescriptorSetLayout m_set_layout;
    VkPipelineLayout      m_pipe_layout;
    VkPipeline            m_pipeline;
    int                   m_nb;
};
} // namespace

struct VulkanComputeContext::Impl final : public ComputeRecorder
{
    VkDevice         device    = VK_NULL_HANDLE;
    VkPhysicalDevice physical  = VK_NULL_HANDLE;
    VkQueue          queue     = VK_NULL_HANDLE;
    crd::u32         family    = 0;
    crd::u32         gfx_family = UINT32_MAX; // B4: graphics family — for CONCURRENT sharing of an INDIRECT-args buffer read by a mesh draw
    bool             int64     = false;
    VkCommandPool    cmd_pool  = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd       = VK_NULL_HANDLE;
    VkFence          fence     = VK_NULL_HANDLE;
    bool             ok        = false;
    // Descriptor-set memo: an identical consecutive dispatch (same pipeline + same bound buffers) reuses ONE set instead
    // of allocating a fresh one — avoids GPU descriptor-cache thrash in a benchmark/rep loop (500 sets ⇒ ~20% slower) +
    // the pool pressure. Reset at begin().
    VkDescriptorSet  memo_set      = VK_NULL_HANDLE;
    crd::u64         memo_binds    = 0;
    VkPipeline       memo_pipeline = VK_NULL_HANDLE; // last-bound pipeline — skip redundant vkCmdBindPipeline in a rep loop
    VkDescriptorSet  memo_bound    = VK_NULL_HANDLE; // last-bound descriptor set — skip redundant vkCmdBindDescriptorSets
    VkQueryPool      ts_pool       = VK_NULL_HANDLE; // 2 timestamps bracketing the recorded work (GPU-only timing)
    double           ts_period     = 1.0;            // ns per timestamp tick
    double           last_gpu_ms   = 0.0;

    [[nodiscard]] crd::u32 find_mem_type(crd::u32 type_bits, VkMemoryPropertyFlags props) const noexcept
    {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (crd::u32 i = 0; i < mp.memoryTypeCount; ++i)
        {
            if ((type_bits & (1U << i)) != 0U && (mp.memoryTypes[i].propertyFlags & props) == props) { return i; }
        }
        return UINT32_MAX;
    }

    // ---- ComputeRecorder ----
    void copy(ComputeBuffer& src, ComputeBuffer& dst, crd::u64 src_off, crd::u64 dst_off, crd::u64 bytes) override
    {
        VkBufferCopy region{src_off, dst_off, bytes};
        vkCmdCopyBuffer(cmd, static_cast<BufferImpl&>(src).buffer(), static_cast<BufferImpl&>(dst).buffer(), 1, &region);
    }
    void barrier(ComputeBuffer& buf, ComputeAccess from, ComputeAccess to) override
    {
        VkBufferMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask       = access_mask(from);
        b.dstAccessMask       = access_mask(to);
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer              = static_cast<BufferImpl&>(buf).buffer();
        b.offset              = 0;
        b.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, access_stage(from), access_stage(to), 0, 0, nullptr, 1, &b, 0, nullptr);
    }
    void dispatch(ComputePipeline& pipeline, crd::containers::ConstSpan<ComputeBuffer*> bindings, const void* push,
                  crd::u32 push_size, crd::u32 gx, crd::u32 gy, crd::u32 gz) override
    {
        auto&     p  = static_cast<PipelineImpl&>(pipeline);
        const int nb = p.nb();

        // Hash the dispatch identity from the C++ object addresses (stable across a rep loop) — reuse the memoized set.
        crd::u64 bh = 1469598103934665603ULL ^ static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(&pipeline));
        for (int i = 0; i < nb && i < kMaxBindings; ++i)
        {
            bh = (bh ^ static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(bindings[static_cast<crd::usize>(i)]))) * 1099511628211ULL;
        }

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (memo_set != VK_NULL_HANDLE && memo_binds == bh) { set = memo_set; }
        else
        {
            VkDescriptorSetLayout       sl = p.set_layout();
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool     = desc_pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts        = &sl;
            if (vkAllocateDescriptorSets(device, &dsai, &set) != VK_SUCCESS) { return; }

            VkDescriptorBufferInfo bi[kMaxBindings];
            VkWriteDescriptorSet   wr[kMaxBindings];
            for (int i = 0; i < nb && i < kMaxBindings; ++i)
            {
                auto& b               = static_cast<BufferImpl&>(*bindings[static_cast<crd::usize>(i)]);
                bi[i]                 = {};
                bi[i].buffer          = b.buffer();
                bi[i].offset          = 0;
                bi[i].range           = b.bytes();
                wr[i]                 = {};
                wr[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[i].dstSet          = set;
                wr[i].dstBinding      = static_cast<crd::u32>(i);
                wr[i].descriptorCount = 1;
                wr[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[i].pBufferInfo     = &bi[i];
            }
            vkUpdateDescriptorSets(device, static_cast<crd::u32>(nb), wr, 0, nullptr);
            memo_set   = set;
            memo_binds = bh;
        }

        if (p.pipeline() != memo_pipeline) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline()); memo_pipeline = p.pipeline(); }
        if (set != memo_bound) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe_layout(), 0, 1, &set, 0, nullptr); memo_bound = set; }
        if (push_size > 0 && push != nullptr) { vkCmdPushConstants(cmd, p.pipe_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push); }
        vkCmdDispatch(cmd, gx > 0 ? gx : 1, gy > 0 ? gy : 1, gz > 0 ? gz : 1);
    }
};

VulkanComputeContext::VulkanComputeContext(VulkanGpuContext& ctx, crd::memory::IAllocator* /*alloc*/)
    : m_impl(std::make_unique<Impl>())
{
    auto& impl    = *m_impl;
    impl.device   = ctx.vk_device();
    impl.physical = ctx.vk_physical_device();
    impl.queue    = ctx.compute_queue();
    impl.family   = ctx.compute_family();
    impl.gfx_family = ctx.graphics_capable() ? ctx.graphics_family() : UINT32_MAX; // B4: for the indirect-args concurrent buffer
    impl.int64    = ctx.shader_int64();
    if (impl.device == VK_NULL_HANDLE || impl.queue == VK_NULL_HANDLE) { return; }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = impl.family;
    if (vkCreateCommandPool(impl.device, &cpci, nullptr, &impl.cmd_pool) != VK_SUCCESS) { return; }

    // One descriptor set is allocated PER recorded dispatch, freed at the next begin(). Size for MANY dispatches in one
    // submit — the 25-dispatch radix job AND the 500-rep tensor benchmark (500 sets × 3 buffers). Undersizing silently
    // no-ops the overflow dispatches (null set) — which under-measures a benchmark loop (looked like 218 TF once).
    VkDescriptorPoolSize ps{};
    ps.type                    = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount         = 8192;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 2048;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(impl.device, &dpci, nullptr, &impl.desc_pool) != VK_SUCCESS) { return; }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = impl.cmd_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(impl.device, &cbai, &impl.cmd) != VK_SUCCESS) { return; }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(impl.device, &fci, nullptr, &impl.fence) != VK_SUCCESS) { return; }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(impl.physical, &props);
    impl.ts_period = static_cast<double>(props.limits.timestampPeriod); // ns/tick
    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    vkCreateQueryPool(impl.device, &qpci, nullptr, &impl.ts_pool); // GPU-only timing (best-effort; null ⇒ last_gpu_ms=0)

    impl.ok = true;
}

VulkanComputeContext::~VulkanComputeContext()
{
    auto& impl = *m_impl;
    if (impl.device == VK_NULL_HANDLE) { return; }
    vkDeviceWaitIdle(impl.device);
    if (impl.ts_pool != VK_NULL_HANDLE) { vkDestroyQueryPool(impl.device, impl.ts_pool, nullptr); }
    if (impl.fence != VK_NULL_HANDLE) { vkDestroyFence(impl.device, impl.fence, nullptr); }
    if (impl.desc_pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(impl.device, impl.desc_pool, nullptr); }
    if (impl.cmd_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(impl.device, impl.cmd_pool, nullptr); }
}

bool VulkanComputeContext::valid() const noexcept { return m_impl->ok; }
bool VulkanComputeContext::supports_shader_int64() const noexcept { return m_impl->int64; }

std::unique_ptr<ComputeBuffer> VulkanComputeContext::create_buffer(crd::u64 bytes, crd::u32 usage, ComputeMemory memory)
{
    auto& impl = *m_impl;
    if (!impl.ok || bytes == 0) { return nullptr; }
    VkBufferUsageFlags uf = 0;
    if ((usage & compute_usage::storage) != 0U) { uf |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; }
    if ((usage & compute_usage::transfer_src) != 0U) { uf |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT; }
    if ((usage & compute_usage::transfer_dst) != 0U) { uf |= VK_BUFFER_USAGE_TRANSFER_DST_BIT; }
    const bool indirect = (usage & compute_usage::indirect) != 0U; // B4: also readable by vkCmdDrawMeshTasksIndirectEXT
    if (indirect) { uf |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; }
    // GpuToCpu readback wants HOST_CACHED — uncached/write-combined reads of a large buffer are ~15× slower on the CPU
    // (a 64 MB LBVH nodes readback: ~200 ms vs ~12 ms). CpuToGpu upload is fine host-coherent (write-combined writes are
    // fast + need no flush). GpuOnly = device-local VRAM.
    VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (memory == ComputeMemory::CpuToGpu) { props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; }
    else if (memory == ComputeMemory::GpuToCpu)
    {
        props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    const bool mappable = memory != ComputeMemory::GpuOnly;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = bytes;
    bci.usage       = uf;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // B4: an INDIRECT-args buffer is written on the COMPUTE queue then read on the GRAPHICS queue (the mesh draw). When those
    // are distinct families (a dedicated compute queue), CONCURRENT sharing lets both access it without an ownership transfer.
    const crd::u32 fams[2] = {impl.family, impl.gfx_family};
    if (indirect && impl.gfx_family != UINT32_MAX && impl.gfx_family != impl.family)
    {
        bci.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        bci.queueFamilyIndexCount = 2U;
        bci.pQueueFamilyIndices   = fams;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(impl.device, &bci, nullptr, &buffer) != VK_SUCCESS) { return nullptr; }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(impl.device, buffer, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = impl.find_mem_type(mr.memoryTypeBits, props);
    if (mai.memoryTypeIndex == UINT32_MAX && memory == ComputeMemory::GpuToCpu)
    {
        // No HOST_CACHED readback heap on this adapter — fall back to host-coherent (still correct, just slower reads).
        mai.memoryTypeIndex = impl.find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(impl.device, &mai, nullptr, &mem) != VK_SUCCESS)
    {
        vkDestroyBuffer(impl.device, buffer, nullptr);
        return nullptr;
    }
    vkBindBufferMemory(impl.device, buffer, mem, 0);
    return std::make_unique<BufferImpl>(impl.device, buffer, mem, bytes, mappable);
}

std::unique_ptr<ComputePipeline> VulkanComputeContext::create_pipeline(crd::containers::StringView shader_dir,
                                                                      crd::containers::StringView name, int n_bindings,
                                                                      crd::u32 push_size)
{
    // Source-agnostic entry point: THIS backend loads its own cooked kernel `<shader_dir>/<name>.comp.spv`.
    char       fname[512];
    crd::usize fn = 0;
    for (crd::usize i = 0; i < name.size() && fn < 500; ++i) { fname[fn++] = name[i]; }
    const char* ext = ".comp.spv";
    for (crd::usize i = 0; ext[i] != '\0' && fn < 511; ++i) { fname[fn++] = ext[i]; }
    crd::platform::fs::Path spv_path{shader_dir};
    spv_path = spv_path / crd::containers::StringView{fname, fn};
    crd::containers::Array<crd::u8> spv;
    if (!crd::platform::fs::read_file_binary(spv_path, spv)) { return nullptr; }
    return create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.data(), spv.size()), n_bindings, push_size);
}

std::unique_ptr<ComputePipeline> VulkanComputeContext::create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8> spirv,
                                                                                 int n_bindings, crd::u32 push_size)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_bindings <= 0 || n_bindings > kMaxBindings) { return nullptr; }

    VkShaderModuleCreateInfo smci{};
    smci.sType       = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize    = spirv.size();
    smci.pCode       = reinterpret_cast<const crd::u32*>(spirv.data());
    VkShaderModule sh = VK_NULL_HANDLE;
    if (vkCreateShaderModule(impl.device, &smci, nullptr, &sh) != VK_SUCCESS) { return nullptr; }

    VkDescriptorSetLayoutBinding binds[kMaxBindings];
    for (int i = 0; i < n_bindings; ++i)
    {
        binds[i]                 = {};
        binds[i].binding         = static_cast<crd::u32>(i);
        binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount  = static_cast<crd::u32>(n_bindings);
    dlci.pBindings     = binds;
    VkDescriptorSetLayout sl = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(impl.device, &dlci, nullptr, &sl) != VK_SUCCESS)
    {
        vkDestroyShaderModule(impl.device, sh, nullptr);
        return nullptr;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = push_size;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &sl;
    plci.pushConstantRangeCount = push_size > 0 ? 1 : 0;
    plci.pPushConstantRanges    = push_size > 0 ? &pcr : nullptr;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(impl.device, &plci, nullptr, &pl) != VK_SUCCESS)
    {
        vkDestroyDescriptorSetLayout(impl.device, sl, nullptr);
        vkDestroyShaderModule(impl.device, sh, nullptr);
        return nullptr;
    }

    VkComputePipelineCreateInfo cpci{};
    cpci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sh;
    cpci.stage.pName  = "main";
    cpci.layout       = pl;
    VkPipeline pipe   = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(impl.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe) != VK_SUCCESS)
    {
        vkDestroyPipelineLayout(impl.device, pl, nullptr);
        vkDestroyDescriptorSetLayout(impl.device, sl, nullptr);
        vkDestroyShaderModule(impl.device, sh, nullptr);
        return nullptr;
    }
    return std::make_unique<PipelineImpl>(impl.device, sh, sl, pl, pipe, n_bindings);
}

ComputeRecorder& VulkanComputeContext::begin()
{
    auto& impl = *m_impl;
    vkResetDescriptorPool(impl.device, impl.desc_pool, 0);
    impl.memo_set      = VK_NULL_HANDLE; // the pool reset freed every set — invalidate the memos
    impl.memo_pipeline = VK_NULL_HANDLE; // a fresh command buffer has no bound pipeline / set
    impl.memo_bound    = VK_NULL_HANDLE;
    vkResetCommandBuffer(impl.cmd, 0);
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(impl.cmd, &bbi);
    if (impl.ts_pool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(impl.cmd, impl.ts_pool, 0, 2);
        vkCmdWriteTimestamp(impl.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl.ts_pool, 0);
    }
    return impl;
}

void VulkanComputeContext::submit_and_wait()
{
    auto& impl = *m_impl;
    if (impl.ts_pool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(impl.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl.ts_pool, 1); }
    vkEndCommandBuffer(impl.cmd);
    vkResetFences(impl.device, 1, &impl.fence);
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &impl.cmd;
    vkQueueSubmit(impl.queue, 1, &si, impl.fence);
    vkWaitForFences(impl.device, 1, &impl.fence, VK_TRUE, UINT64_MAX);
    if (impl.ts_pool != VK_NULL_HANDLE)
    {
        crd::u64 ts[2] = {0, 0};
        if (vkGetQueryPoolResults(impl.device, impl.ts_pool, 0, 2, sizeof(ts), ts, sizeof(crd::u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
            == VK_SUCCESS)
        {
            impl.last_gpu_ms = (ts[1] >= ts[0]) ? static_cast<double>(ts[1] - ts[0]) * impl.ts_period / 1.0e6 : 0.0;
        }
    }
}

double VulkanComputeContext::last_gpu_ms() const noexcept { return m_impl->last_gpu_ms; }

} // namespace crd::gpu
