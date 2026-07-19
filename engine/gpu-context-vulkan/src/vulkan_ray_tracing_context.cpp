// vulkan_ray_tracing_context.cpp — D-007 C3/RT-1: hardware acceleration-structure build (BLAS/TLAS) + inline ray-query
// compute dispatch (VK_KHR_ray_query). The device half of the C3↔B9 RT pair; see the header.

#include <crd/gpu/vulkan_ray_tracing_context.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace crd::gpu
{
namespace
{
// A device buffer carrying a device address (the AS-build inputs, scratch, and AS backing all need one). Optionally
// host-visible for direct upload (vertices / instances / rays / readback). RAII within the RT context's arena.
struct DevBuffer
{
    VkBuffer        buffer = VK_NULL_HANDLE;
    VkDeviceMemory  memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void*           mapped  = nullptr;
    crd::u64        bytes   = 0;
};

[[nodiscard]] crd::u32 find_mem_type(VkPhysicalDevice phys, crd::u32 type_bits, VkMemoryPropertyFlags props) noexcept
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (crd::u32 i = 0; i < mp.memoryTypeCount; ++i)
    {
        if ((type_bits & (1U << i)) != 0U && (mp.memoryTypes[i].propertyFlags & props) == props) { return i; }
    }
    return UINT32_MAX;
}
} // namespace

struct VulkanRayTracingContext::Impl
{
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    crd::u32         family   = UINT32_MAX;
    VkCommandPool    cmd_pool = VK_NULL_HANDLE;
    bool             ok       = false;

    // VK_KHR_acceleration_structure / ray-query / buffer-device-address entry points (loaded via vkGetDeviceProcAddr).
    PFN_vkGetBufferDeviceAddressKHR              get_buffer_addr    = nullptr;
    PFN_vkCreateAccelerationStructureKHR         create_as          = nullptr;
    PFN_vkDestroyAccelerationStructureKHR        destroy_as         = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR  get_build_sizes    = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR      cmd_build_as       = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_as_addr      = nullptr;
    // FA-1: VK_EXT_opacity_micromap entry points (loaded only when the adapter reports it).
    PFN_vkCreateMicromapEXT         create_mm    = nullptr;
    PFN_vkDestroyMicromapEXT        destroy_mm   = nullptr;
    PFN_vkGetMicromapBuildSizesEXT  get_mm_sizes = nullptr;
    PFN_vkCmdBuildMicromapsEXT      cmd_build_mm = nullptr;
    bool                            has_omm      = false;
    // FA-2: VK_KHR_ray_tracing_pipeline entry points + the shader-group handle sizes (for the SBT layout).
    PFN_vkCreateRayTracingPipelinesKHR       create_rt_pipe = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR get_group_handles = nullptr;
    PFN_vkCmdTraceRaysKHR                    cmd_trace_rays = nullptr;
    bool                            has_rtpipe   = false;
    bool                            has_ser      = false; // FA-2: SER (VK_NV_ray_tracing_invocation_reorder) enabled
    crd::u32                        sbt_handle_size = 0;
    crd::u32                        sbt_handle_align = 0;
    crd::u32                        sbt_base_align = 0;
    // FA-3: VK_NV_cluster_acceleration_structure entry points + alignments.
    PFN_vkGetClusterAccelerationStructureBuildSizesNV     get_cluster_sizes = nullptr;
    PFN_vkCmdBuildClusterAccelerationStructureIndirectNV  cmd_build_cluster = nullptr;
    bool                            has_cluster   = false;
    crd::u32                        clas_scratch_align = 128;
    crd::u32                        clas_byte_align    = 128;
    crd::u32                        clas_bl_align      = 128;

    crd::containers::Array<DevBuffer>                 owned;    // every buffer the context creates — freed in the dtor
    crd::containers::Array<VkAccelerationStructureKHR> owned_as; // every AS created — destroyed in the dtor
    crd::containers::Array<VkMicromapEXT>              owned_mm; // FA-1: every micromap created — destroyed in the dtor

    explicit Impl(crd::memory::IAllocator* a) : owned(a), owned_as(a), owned_mm(a) {}

    [[nodiscard]] bool make_buffer(crd::u64 bytes, VkBufferUsageFlags usage, bool host_visible, DevBuffer& out)
    {
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = bytes;
        bci.usage       = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &out.buffer) != VK_SUCCESS) { return false; }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, out.buffer, &mr);
        VkMemoryAllocateFlagsInfo flags{};
        flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; // the buffer will be queried for a device address
        const VkMemoryPropertyFlags props = host_visible
            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext           = &flags;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_mem_type(physical, mr.memoryTypeBits, props);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(device, out.buffer, nullptr);
            out.buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device, out.buffer, out.memory, 0);
        out.bytes = bytes;
        if (host_visible) { vkMapMemory(device, out.memory, 0, bytes, 0, &out.mapped); }
        VkBufferDeviceAddressInfo bdai{};
        bdai.sType   = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bdai.buffer  = out.buffer;
        out.address  = get_buffer_addr(device, &bdai);
        owned.push_back(out);
        return true;
    }

    // Submit a one-shot command buffer and block until the GPU finishes (AS builds + the trace dispatch are one-shot here).
    template <typename F>
    void submit_oneshot(F&& record)
    {
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = cmd_pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) { return; }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);
    }
};

// A built scene: the TLAS handle (bound by the trace dispatch) — its backing buffers live in the context's `owned` arena.
class RtSceneImpl final : public RtScene
{
public:
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
};

VulkanRayTracingContext::VulkanRayTracingContext(VulkanGpuContext& ctx)
    : m_impl(std::make_unique<Impl>(crd::memory::default_allocator()))
{
    auto& impl    = *m_impl;
    impl.physical = ctx.vk_physical_device();
    impl.device   = ctx.vk_device();
    impl.queue    = ctx.compute_queue();
    impl.family   = ctx.compute_family();
    if (!ctx.ray_query() || impl.device == VK_NULL_HANDLE) { return; } // adapter without RT ⇒ invalid (tests skip)

    const auto load = [&](const char* name) { return vkGetDeviceProcAddr(impl.device, name); };
    // buffer-device-address is CORE in Vulkan 1.2+ (we enabled the FEATURE, not the VK_KHR alias extension) ⇒ the core name.
    impl.get_buffer_addr = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(load("vkGetBufferDeviceAddress"));
    impl.create_as       = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
    impl.destroy_as      = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
    impl.get_build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(load("vkGetAccelerationStructureBuildSizesKHR"));
    impl.cmd_build_as    = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(load("vkCmdBuildAccelerationStructuresKHR"));
    impl.get_as_addr     = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(load("vkGetAccelerationStructureDeviceAddressKHR"));
    if (impl.get_buffer_addr == nullptr || impl.create_as == nullptr || impl.get_build_sizes == nullptr
        || impl.cmd_build_as == nullptr || impl.get_as_addr == nullptr)
    {
        return;
    }
    // FA-1: opacity-micromap entry points (only when the adapter enabled VK_EXT_opacity_micromap).
    if (ctx.opacity_micromap())
    {
        impl.create_mm    = reinterpret_cast<PFN_vkCreateMicromapEXT>(load("vkCreateMicromapEXT"));
        impl.destroy_mm   = reinterpret_cast<PFN_vkDestroyMicromapEXT>(load("vkDestroyMicromapEXT"));
        impl.get_mm_sizes = reinterpret_cast<PFN_vkGetMicromapBuildSizesEXT>(load("vkGetMicromapBuildSizesEXT"));
        impl.cmd_build_mm = reinterpret_cast<PFN_vkCmdBuildMicromapsEXT>(load("vkCmdBuildMicromapsEXT"));
        impl.has_omm = impl.create_mm != nullptr && impl.get_mm_sizes != nullptr && impl.cmd_build_mm != nullptr;
    }
    // FA-2: ray-tracing-pipeline entry points + the shader-group handle geometry (SBT layout comes from the device properties).
    if (ctx.rt_pipeline())
    {
        impl.create_rt_pipe    = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(load("vkCreateRayTracingPipelinesKHR"));
        impl.get_group_handles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(load("vkGetRayTracingShaderGroupHandlesKHR"));
        impl.cmd_trace_rays    = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(load("vkCmdTraceRaysKHR"));
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp{};
        rtp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &rtp;
        vkGetPhysicalDeviceProperties2(impl.physical, &p2);
        impl.sbt_handle_size  = rtp.shaderGroupHandleSize;
        impl.sbt_handle_align = rtp.shaderGroupHandleAlignment;
        impl.sbt_base_align   = rtp.shaderGroupBaseAlignment;
        impl.has_rtpipe = impl.create_rt_pipe != nullptr && impl.get_group_handles != nullptr && impl.cmd_trace_rays != nullptr && impl.sbt_handle_size > 0;
        impl.has_ser    = impl.has_rtpipe && ctx.invocation_reorder(); // SER rides the RT pipeline
    }
    // FA-3: cluster-AS entry points + the required byte alignments.
    if (ctx.cluster_as())
    {
        impl.get_cluster_sizes = reinterpret_cast<PFN_vkGetClusterAccelerationStructureBuildSizesNV>(load("vkGetClusterAccelerationStructureBuildSizesNV"));
        impl.cmd_build_cluster = reinterpret_cast<PFN_vkCmdBuildClusterAccelerationStructureIndirectNV>(load("vkCmdBuildClusterAccelerationStructureIndirectNV"));
        VkPhysicalDeviceClusterAccelerationStructurePropertiesNV cp{};
        cp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &cp;
        vkGetPhysicalDeviceProperties2(impl.physical, &p2);
        if (cp.clusterScratchByteAlignment != 0U) { impl.clas_scratch_align = cp.clusterScratchByteAlignment; }
        if (cp.clusterByteAlignment != 0U) { impl.clas_byte_align = cp.clusterByteAlignment; }
        if (cp.clusterBottomLevelByteAlignment != 0U) { impl.clas_bl_align = cp.clusterBottomLevelByteAlignment; }
        impl.has_cluster = impl.get_cluster_sizes != nullptr && impl.cmd_build_cluster != nullptr;
    }
    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = impl.family;
    if (vkCreateCommandPool(impl.device, &cpci, nullptr, &impl.cmd_pool) != VK_SUCCESS) { return; }
    impl.ok = true;
}

VulkanRayTracingContext::~VulkanRayTracingContext()
{
    auto& impl = *m_impl;
    if (impl.device == VK_NULL_HANDLE) { return; }
    vkDeviceWaitIdle(impl.device);
    if (impl.destroy_as != nullptr)
    {
        for (crd::usize i = 0; i < impl.owned_as.size(); ++i) { impl.destroy_as(impl.device, impl.owned_as[i], nullptr); }
    }
    if (impl.destroy_mm != nullptr) // FA-1
    {
        for (crd::usize i = 0; i < impl.owned_mm.size(); ++i) { impl.destroy_mm(impl.device, impl.owned_mm[i], nullptr); }
    }
    for (crd::usize i = 0; i < impl.owned.size(); ++i)
    {
        const DevBuffer& b = impl.owned[i];
        if (b.mapped != nullptr) { vkUnmapMemory(impl.device, b.memory); }
        if (b.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(impl.device, b.buffer, nullptr); }
        if (b.memory != VK_NULL_HANDLE) { vkFreeMemory(impl.device, b.memory, nullptr); }
    }
    if (impl.cmd_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(impl.device, impl.cmd_pool, nullptr); }
}

bool VulkanRayTracingContext::valid() const noexcept { return m_impl->ok; }

RtCapabilities VulkanRayTracingContext::capabilities() const noexcept
{
    const auto&    impl = *m_impl;
    RtCapabilities c;
    c.set(RtFeature::InlineQuery, impl.ok); // the inline query is the baseline once the RT context is valid
    c.set(RtFeature::RtPipeline, impl.has_rtpipe);
    c.set(RtFeature::ShaderReorder, impl.has_ser);
    c.set(RtFeature::OpacityMicromap, impl.has_omm);
    c.set(RtFeature::ClusterAS, impl.has_cluster);
    return c;
}

std::unique_ptr<RtScene> VulkanRayTracingContext::build_scene(const float* vertices, crd::u32 ntris)
{
    const float identity[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    return build_scene_instanced(vertices, ntris, identity, 1U);
}

std::unique_ptr<RtScene> VulkanRayTracingContext::build_scene_instanced(const float* vertices, crd::u32 ntris,
                                                                        const float* transforms, crd::u32 ninst, bool opaque)
{
    auto& impl = *m_impl;
    if (!impl.ok || ntris == 0 || ninst == 0) { return nullptr; }
    const crd::u32 nverts = ntris * 3U;

    // ── vertex buffer (host-visible, AS-build input) ──
    DevBuffer vbuf{};
    if (!impl.make_buffer(static_cast<crd::u64>(nverts) * 3U * sizeof(float),
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, vbuf))
    {
        return nullptr;
    }
    std::memcpy(vbuf.mapped, vertices, static_cast<crd::usize>(nverts) * 3U * sizeof(float));

    // Build one AS (BLAS or TLAS). The backing + scratch buffers are LOCAL — make_buffer copies each into `owned` (whose dtor
    // frees them), so the Vulkan handles stay valid for the AS's lifetime without holding a reference into the (reallocating)
    // owned Array. `geom.pGeometries` is captured by the build info; both live on this stack frame through the submit.
    const auto build_as = [&](VkAccelerationStructureTypeKHR type, VkAccelerationStructureGeometryKHR& geom,
                              crd::u32 prim_count, VkAccelerationStructureKHR& as_out) -> bool {
        VkAccelerationStructureBuildGeometryInfoKHR bgi{};
        bgi.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bgi.type          = type;
        bgi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bgi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bgi.geometryCount = 1;
        bgi.pGeometries   = &geom;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        impl.get_build_sizes(impl.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &prim_count, &sizes);
        DevBuffer backing{};
        if (!impl.make_buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, backing)) { return false; }
        VkAccelerationStructureCreateInfoKHR aci{};
        aci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        aci.buffer = backing.buffer;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = type;
        if (impl.create_as(impl.device, &aci, nullptr, &as_out) != VK_SUCCESS) { return false; }
        impl.owned_as.push_back(as_out); // tracked for destruction in the context dtor
        DevBuffer scratch{};
        if (!impl.make_buffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratch)) { return false; }
        bgi.dstAccelerationStructure  = as_out;
        bgi.scratchData.deviceAddress = scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount                                   = prim_count;
        const VkAccelerationStructureBuildRangeInfoKHR* pranges = &range;
        impl.submit_oneshot([&](VkCommandBuffer cmd) { impl.cmd_build_as(cmd, 1, &bgi, &pranges); });
        return true;
    };

    auto scene = std::make_unique<RtSceneImpl>();

    // ── BLAS from the triangle soup ──
    VkAccelerationStructureGeometryKHR tri_geom{};
    tri_geom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tri_geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    tri_geom.flags        = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0U; // non-opaque ⇒ any-hit shaders run (P4 alpha fallback)
    tri_geom.geometry.triangles.sType                   = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri_geom.geometry.triangles.vertexFormat            = VK_FORMAT_R32G32B32_SFLOAT;
    tri_geom.geometry.triangles.vertexData.deviceAddress = vbuf.address;
    tri_geom.geometry.triangles.vertexStride            = 3U * sizeof(float);
    tri_geom.geometry.triangles.maxVertex               = nverts - 1U;
    tri_geom.geometry.triangles.indexType               = VK_INDEX_TYPE_NONE_KHR; // indexless: 3 verts per triangle in order
    if (!build_as(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, tri_geom, ntris, scene->blas)) { return nullptr; }
    VkAccelerationStructureDeviceAddressInfoKHR bai{};
    bai.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    bai.accelerationStructure = scene->blas;
    const VkDeviceAddress blas_addr = impl.get_as_addr(impl.device, &bai);

    // ── instance buffer: `ninst` instances of the BLAS, each with its row-major 3×4 world transform ──
    DevBuffer ibuf{};
    if (!impl.make_buffer(static_cast<crd::u64>(ninst) * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, ibuf)) { return nullptr; }
    auto* insts = static_cast<VkAccelerationStructureInstanceKHR*>(ibuf.mapped);
    for (crd::u32 n = 0; n < ninst; ++n)
    {
        VkAccelerationStructureInstanceKHR inst{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 4; ++c) { inst.transform.matrix[r][c] = transforms[static_cast<crd::usize>(n) * 12U + static_cast<crd::usize>(r) * 4U + static_cast<crd::usize>(c)]; }
        }
        inst.mask                           = 0xFFU;
        inst.instanceCustomIndex            = n; // surfaces as gl_InstanceCustomIndexEXT / InstanceID() for per-instance data
        inst.accelerationStructureReference = blas_addr;
        insts[n]                            = inst;
    }

    // ── TLAS over the `ninst` instances ──
    VkAccelerationStructureGeometryKHR inst_geom{};
    inst_geom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    inst_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    inst_geom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
    inst_geom.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    inst_geom.geometry.instances.arrayOfPointers    = VK_FALSE;
    inst_geom.geometry.instances.data.deviceAddress = ibuf.address;
    if (!build_as(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, inst_geom, ninst, scene->tlas)) { return nullptr; }
    return scene;
}

std::unique_ptr<RtScene> VulkanRayTracingContext::build_scene_omm(const float* vertices, crd::u32 ntris, const crd::u8* omm_bits, crd::u32 subdiv)
{
    auto& impl = *m_impl;
    if (!impl.ok || !impl.has_omm || ntris == 0) { return nullptr; }
    const crd::u32 nverts = ntris * 3U;
    const crd::u32 nmicro = 1U << (2U * subdiv);      // 4^subdiv micro-triangles
    const crd::u32 nbytes = (nmicro + 7U) / 8U;       // 2-state ⇒ 1 bit each

    // ── vertex buffer ──
    DevBuffer vbuf{};
    if (!impl.make_buffer(static_cast<crd::u64>(nverts) * 3U * sizeof(float), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, vbuf)) { return nullptr; }
    std::memcpy(vbuf.mapped, vertices, static_cast<crd::usize>(nverts) * 3U * sizeof(float));

    // ── the OPACITY MICROMAP for triangle 0 (upload the packed 2-state bits + a 1-entry triangle array) ──
    DevBuffer ommData{};
    if (!impl.make_buffer(nbytes, VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT, true, ommData)) { return nullptr; }
    std::memcpy(ommData.mapped, omm_bits, nbytes);
    VkMicromapTriangleEXT mtri{};
    mtri.dataOffset       = 0;
    mtri.subdivisionLevel = static_cast<crd::u16>(subdiv);
    mtri.format           = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    DevBuffer triArr{};
    if (!impl.make_buffer(sizeof(VkMicromapTriangleEXT), VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT, true, triArr)) { return nullptr; }
    std::memcpy(triArr.mapped, &mtri, sizeof(mtri));

    VkMicromapUsageEXT usage{};
    usage.count            = 1;
    usage.subdivisionLevel = subdiv;
    usage.format           = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    VkMicromapBuildInfoEXT mbi{};
    mbi.sType                     = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
    mbi.type                      = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    mbi.mode                      = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    mbi.usageCountsCount          = 1;
    mbi.pUsageCounts              = &usage;
    mbi.data.deviceAddress        = ommData.address;
    mbi.triangleArray.deviceAddress = triArr.address;
    mbi.triangleArrayStride       = sizeof(VkMicromapTriangleEXT);
    VkMicromapBuildSizesInfoEXT msz{};
    msz.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT;
    impl.get_mm_sizes(impl.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mbi, &msz);
    DevBuffer mmBack{};
    if (!impl.make_buffer(msz.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, false, mmBack)) { return nullptr; }
    VkMicromapCreateInfoEXT mci{};
    mci.sType  = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT;
    mci.buffer = mmBack.buffer;
    mci.size   = msz.micromapSize;
    mci.type   = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    VkMicromapEXT micromap = VK_NULL_HANDLE;
    if (impl.create_mm(impl.device, &mci, nullptr, &micromap) != VK_SUCCESS) { return nullptr; }
    impl.owned_mm.push_back(micromap);
    DevBuffer mmScratch{};
    if (!impl.make_buffer(msz.buildScratchSize > 0 ? msz.buildScratchSize : 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, mmScratch)) { return nullptr; }
    mbi.dstMicromap               = micromap;
    mbi.scratchData.deviceAddress = mmScratch.address;
    impl.submit_oneshot([&](VkCommandBuffer cmd) { impl.cmd_build_mm(cmd, 1, &mbi); });

    // ── per-triangle OMM index: triangle 0 → OMM index 0, the rest → FULLY_OPAQUE ──
    DevBuffer idxBuf{};
    if (!impl.make_buffer(static_cast<crd::u64>(ntris) * sizeof(crd::i32), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, idxBuf)) { return nullptr; }
    auto* idx = static_cast<crd::i32*>(idxBuf.mapped);
    idx[0] = 0;
    for (crd::u32 t = 1; t < ntris; ++t) { idx[t] = VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT; }

    // ── BLAS: triangle geometry with the OMM attached (NOT opaque ⇒ the OMM is consulted during traversal) ──
    auto scene = std::make_unique<RtSceneImpl>();
    VkAccelerationStructureTrianglesOpacityMicromapEXT ommGeo{};
    ommGeo.sType                     = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
    ommGeo.indexType                 = VK_INDEX_TYPE_UINT32; // per-triangle OMM index; the special values are negative bit patterns
    ommGeo.indexBuffer.deviceAddress = idxBuf.address;
    ommGeo.indexStride               = sizeof(crd::i32);
    ommGeo.micromap                  = micromap;
    VkAccelerationStructureGeometryKHR tri_geom{};
    tri_geom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tri_geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    tri_geom.flags        = 0; // NOT opaque ⇒ traversal consults the OMM
    tri_geom.geometry.triangles.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri_geom.geometry.triangles.pNext                    = &ommGeo;
    tri_geom.geometry.triangles.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
    tri_geom.geometry.triangles.vertexData.deviceAddress = vbuf.address;
    tri_geom.geometry.triangles.vertexStride             = 3U * sizeof(float);
    tri_geom.geometry.triangles.maxVertex                = nverts - 1U;
    tri_geom.geometry.triangles.indexType                = VK_INDEX_TYPE_NONE_KHR;

    const auto build_as = [&](VkAccelerationStructureTypeKHR type, VkAccelerationStructureGeometryKHR& geom, crd::u32 prim_count, VkAccelerationStructureKHR& as_out) -> bool {
        VkAccelerationStructureBuildGeometryInfoKHR bgi{};
        bgi.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bgi.type          = type;
        bgi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bgi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bgi.geometryCount = 1;
        bgi.pGeometries   = &geom;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        impl.get_build_sizes(impl.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &prim_count, &sizes);
        DevBuffer backing{};
        if (!impl.make_buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, backing)) { return false; }
        VkAccelerationStructureCreateInfoKHR aci{};
        aci.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        aci.buffer = backing.buffer;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = type;
        if (impl.create_as(impl.device, &aci, nullptr, &as_out) != VK_SUCCESS) { return false; }
        impl.owned_as.push_back(as_out);
        DevBuffer scratch{};
        if (!impl.make_buffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratch)) { return false; }
        bgi.dstAccelerationStructure  = as_out;
        bgi.scratchData.deviceAddress = scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount                                   = prim_count;
        const VkAccelerationStructureBuildRangeInfoKHR* pranges = &range;
        impl.submit_oneshot([&](VkCommandBuffer cmd) { impl.cmd_build_as(cmd, 1, &bgi, &pranges); });
        return true;
    };
    if (!build_as(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, tri_geom, ntris, scene->blas)) { return nullptr; }
    VkAccelerationStructureDeviceAddressInfoKHR bai{};
    bai.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    bai.accelerationStructure = scene->blas;
    const VkDeviceAddress blas_addr = impl.get_as_addr(impl.device, &bai);

    DevBuffer ibuf{};
    if (!impl.make_buffer(sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, ibuf)) { return nullptr; }
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = 1.0F; inst.transform.matrix[1][1] = 1.0F; inst.transform.matrix[2][2] = 1.0F;
    inst.mask                           = 0xFFU;
    inst.accelerationStructureReference = blas_addr;
    std::memcpy(ibuf.mapped, &inst, sizeof(inst));
    VkAccelerationStructureGeometryKHR inst_geom{};
    inst_geom.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    inst_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    inst_geom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
    inst_geom.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    inst_geom.geometry.instances.arrayOfPointers    = VK_FALSE;
    inst_geom.geometry.instances.data.deviceAddress = ibuf.address;
    if (!build_as(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, inst_geom, 1U, scene->tlas)) { return nullptr; }
    return scene;
}

std::unique_ptr<RtScene> VulkanRayTracingContext::build_scene_clusters(const float* vertices, crd::u32 ntris)
{
    auto& impl = *m_impl;
    if (!impl.ok || !impl.has_cluster || ntris == 0) { return nullptr; }
    const crd::u32 nverts = ntris * 3U;
    const auto align_up = [](crd::u64 v, crd::u64 a) { return a == 0U ? v : ((v + a - 1U) & ~(a - 1U)); };

    // vertices + indices (indexless soup ⇒ indices 0,1,2,...) in device-address buffers.
    DevBuffer vbuf{}, ibuf{};
    if (!impl.make_buffer(static_cast<crd::u64>(nverts) * 3U * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, vbuf)) { return nullptr; }
    std::memcpy(vbuf.mapped, vertices, static_cast<crd::usize>(nverts) * 3U * sizeof(float));
    if (!impl.make_buffer(static_cast<crd::u64>(nverts) * sizeof(crd::u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, ibuf)) { return nullptr; }
    auto* idx = static_cast<crd::u32*>(ibuf.mapped);
    for (crd::u32 i = 0; i < nverts; ++i) { idx[i] = i; }

    // ── PASS 1: build ONE triangle cluster (CLAS), implicit destinations ──
    VkClusterAccelerationStructureTriangleClusterInputNV triIn{};
    triIn.sType                        = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV;
    triIn.vertexFormat                 = VK_FORMAT_R32G32B32_SFLOAT;
    triIn.maxGeometryIndexValue        = 0;
    triIn.maxClusterUniqueGeometryCount = 1;
    triIn.maxClusterTriangleCount      = ntris;
    triIn.maxClusterVertexCount        = nverts;
    triIn.maxTotalTriangleCount        = ntris;
    triIn.maxTotalVertexCount          = nverts;
    triIn.minPositionTruncateBitCount  = 0;
    VkClusterAccelerationStructureInputInfoNV in1{};
    in1.sType                         = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV;
    in1.maxAccelerationStructureCount = 1;
    in1.flags                         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    in1.opType                        = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
    in1.opMode                        = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    in1.opInput.pTriangleClusters     = &triIn;
    VkAccelerationStructureBuildSizesInfoKHR sz1{};
    sz1.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    impl.get_cluster_sizes(impl.device, &in1, &sz1);

    // per-cluster build info (bitfields set directly) + the count buffer + dst-address / scratch / implicit-data buffers.
    DevBuffer clInfo{}, clCount{}, clDst{}, clScratch{}, clData{};
    if (!impl.make_buffer(sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, clInfo)) { return nullptr; }
    auto* ci = static_cast<VkClusterAccelerationStructureBuildTriangleClusterInfoNV*>(clInfo.mapped);
    *ci = VkClusterAccelerationStructureBuildTriangleClusterInfoNV{};
    ci->clusterID                = 0;
    ci->triangleCount            = ntris;
    ci->vertexCount              = nverts;
    ci->indexType                = VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_32BIT_NV;
    ci->baseGeometryIndexAndGeometryFlags.geometryIndex = 0;
    ci->baseGeometryIndexAndGeometryFlags.geometryFlags = VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV; // auto-commit under RayFlagsNone
    ci->indexBufferStride        = sizeof(crd::u32);
    ci->vertexBufferStride       = 3U * sizeof(float);
    ci->indexBuffer              = ibuf.address;
    ci->vertexBuffer             = vbuf.address;
    if (!impl.make_buffer(sizeof(crd::u32), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, clCount)) { return nullptr; }
    *static_cast<crd::u32*>(clCount.mapped) = 1U;
    // CLAS address out — also read as the clusterReferences input of pass 2 (both usages).
    if (!impl.make_buffer(sizeof(VkDeviceAddress), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, clDst)) { return nullptr; }
    if (!impl.make_buffer(align_up(sz1.buildScratchSize, impl.clas_scratch_align) + impl.clas_scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, clScratch)) { return nullptr; }
    if (!impl.make_buffer(sz1.accelerationStructureSize > 0 ? sz1.accelerationStructureSize : 4U, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, clData)) { return nullptr; }

    VkClusterAccelerationStructureCommandsInfoNV cmd1{};
    cmd1.sType                        = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
    cmd1.input                        = in1;
    cmd1.dstImplicitData              = clData.address;
    cmd1.scratchData                  = align_up(clScratch.address, impl.clas_scratch_align);
    cmd1.dstAddressesArray.deviceAddress = clDst.address; cmd1.dstAddressesArray.stride = sizeof(VkDeviceAddress); cmd1.dstAddressesArray.size = sizeof(VkDeviceAddress);
    cmd1.srcInfosArray.deviceAddress  = clInfo.address; cmd1.srcInfosArray.stride = sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV); cmd1.srcInfosArray.size = sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV);
    cmd1.srcInfosCount                = clCount.address;

    // ── PASS 2: build the cluster BLAS over that CLAS ──
    VkClusterAccelerationStructureClustersBottomLevelInputNV blIn{};
    blIn.sType                                   = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV;
    blIn.maxTotalClusterCount                    = 1;
    blIn.maxClusterCountPerAccelerationStructure = 1;
    VkClusterAccelerationStructureInputInfoNV in2{};
    in2.sType                         = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV;
    in2.maxAccelerationStructureCount = 1;
    in2.flags                         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    in2.opType                        = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
    in2.opMode                        = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
    in2.opInput.pClustersBottomLevel  = &blIn;
    VkAccelerationStructureBuildSizesInfoKHR sz2{};
    sz2.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    impl.get_cluster_sizes(impl.device, &in2, &sz2);
    DevBuffer blInfo{}, blCount{}, blDst{}, blScratch{}, blData{};
    if (!impl.make_buffer(sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, blInfo)) { return nullptr; }
    auto* bl = static_cast<VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV*>(blInfo.mapped);
    bl->clusterReferencesCount  = 1;
    bl->clusterReferencesStride = sizeof(VkDeviceAddress);
    bl->clusterReferences       = clDst.address; // GPU-side chain: the CLAS address pass 1 wrote here
    if (!impl.make_buffer(sizeof(crd::u32), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, blCount)) { return nullptr; }
    *static_cast<crd::u32*>(blCount.mapped) = 1U;
    if (!impl.make_buffer(sizeof(VkDeviceAddress), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true, blDst)) { return nullptr; } // BLAS address out (host-visible)
    if (!impl.make_buffer(align_up(sz2.buildScratchSize, impl.clas_scratch_align) + impl.clas_scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, blScratch)) { return nullptr; }
    if (!impl.make_buffer(sz2.accelerationStructureSize > 0 ? sz2.accelerationStructureSize : 4U, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, blData)) { return nullptr; }
    VkClusterAccelerationStructureCommandsInfoNV cmd2{};
    cmd2.sType                        = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV;
    cmd2.input                        = in2;
    cmd2.dstImplicitData              = blData.address;
    cmd2.scratchData                  = align_up(blScratch.address, impl.clas_scratch_align);
    cmd2.dstAddressesArray.deviceAddress = blDst.address; cmd2.dstAddressesArray.stride = sizeof(VkDeviceAddress); cmd2.dstAddressesArray.size = sizeof(VkDeviceAddress);
    cmd2.srcInfosArray.deviceAddress  = blInfo.address; cmd2.srcInfosArray.stride = sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV); cmd2.srcInfosArray.size = sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV);
    cmd2.srcInfosCount                = blCount.address;

    // record both passes with a full barrier between (pass 2 reads pass 1's CLAS + its written address).
    impl.submit_oneshot([&](VkCommandBuffer cmd) {
        impl.cmd_build_cluster(cmd, &cmd1);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);
        impl.cmd_build_cluster(cmd, &cmd2);
    });
    const VkDeviceAddress cluster_blas_addr = *static_cast<VkDeviceAddress*>(blDst.mapped);
    if (cluster_blas_addr == 0) { return nullptr; }

    // ── TLAS over the cluster BLAS (referenced by its device address) ──
    auto scene = std::make_unique<RtSceneImpl>();
    scene->blas = VK_NULL_HANDLE; // the cluster BLAS is address-referenced, not a VkAccelerationStructureKHR handle
    DevBuffer tinst{};
    if (!impl.make_buffer(sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, tinst)) { return nullptr; }
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = 1.0F; inst.transform.matrix[1][1] = 1.0F; inst.transform.matrix[2][2] = 1.0F;
    inst.mask                           = 0xFFU;
    inst.accelerationStructureReference = cluster_blas_addr;
    std::memcpy(tinst.mapped, &inst, sizeof(inst));
    VkAccelerationStructureGeometryKHR ig{};
    ig.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    ig.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    ig.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
    ig.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    ig.geometry.instances.arrayOfPointers    = VK_FALSE;
    ig.geometry.instances.data.deviceAddress = tinst.address;
    VkAccelerationStructureBuildGeometryInfoKHR bgi{};
    bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR; bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1; bgi.pGeometries = &ig;
    crd::u32 one = 1;
    VkAccelerationStructureBuildSizesInfoKHR tsz{}; tsz.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    impl.get_build_sizes(impl.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &one, &tsz);
    DevBuffer tback{}, tscratch{};
    if (!impl.make_buffer(tsz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, tback)) { return nullptr; }
    VkAccelerationStructureCreateInfoKHR aci{}; aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR; aci.buffer = tback.buffer; aci.size = tsz.accelerationStructureSize; aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (impl.create_as(impl.device, &aci, nullptr, &scene->tlas) != VK_SUCCESS) { return nullptr; }
    impl.owned_as.push_back(scene->tlas);
    if (!impl.make_buffer(tsz.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, tscratch)) { return nullptr; }
    bgi.dstAccelerationStructure = scene->tlas; bgi.scratchData.deviceAddress = tscratch.address;
    VkAccelerationStructureBuildRangeInfoKHR rng{}; rng.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* prng = &rng;
    impl.submit_oneshot([&](VkCommandBuffer cmd) { impl.cmd_build_as(cmd, 1, &bgi, &prng); });
    return scene;
}

std::unique_ptr<RtScene> VulkanRayTracingContext::build_scene_alpha(const float* vertices, crd::u32 ntris, const crd::u8* omm_bits, crd::u32 subdiv, bool* fell_back)
{
    if (m_impl->has_omm) // hardware opacity micromap resolves alpha in traversal (fast path)
    {
        if (fell_back != nullptr) { *fell_back = false; }
        return build_scene_omm(vertices, ntris, omm_bits, subdiv);
    }
    // fallback: non-opaque geometry the caller pairs with the CKIR any-hit alpha shader (correct, slower).
    if (fell_back != nullptr) { *fell_back = true; }
    const float identity[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    return build_scene_instanced(vertices, ntris, identity, 1U, /*opaque=*/false);
}

std::unique_ptr<RtScene> VulkanRayTracingContext::build_scene_scalable(const float* vertices, crd::u32 ntris, bool prefer_clusters, bool* fell_back)
{
    const bool use_clusters = prefer_clusters && m_impl->has_cluster;
    if (fell_back != nullptr) { *fell_back = prefer_clusters && !use_clusters; } // requested clusters but the adapter lacks them
    if (use_clusters) { return build_scene_clusters(vertices, ntris); }
    return build_scene(vertices, ntris); // standard BLAS — identical result (cluster topology is a memory-layout optimisation)
}

bool VulkanRayTracingContext::trace_dispatch(const RtScene& scene_base, crd::containers::ConstSpan<crd::u8> spirv,
                                             crd::containers::ConstSpan<Binding> bindings, crd::u32 groups)
{
    auto&              impl  = *m_impl;
    const RtSceneImpl& scene = static_cast<const RtSceneImpl&>(scene_base);
    const crd::usize   nbuf  = bindings.size();
    if (!impl.ok || scene.tlas == VK_NULL_HANDLE || nbuf == 0 || nbuf > 15) { return false; }

    // ── one host-visible storage buffer per Binding; upload inputs ──
    DevBuffer bufs[16]{};
    crd::u32  max_slot = 0;
    for (crd::usize i = 0; i < nbuf; ++i)
    {
        if (!impl.make_buffer(bindings[i].bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, bufs[i])) { return false; }
        if (bindings[i].upload != nullptr) { std::memcpy(bufs[i].mapped, bindings[i].upload, static_cast<crd::usize>(bindings[i].bytes)); }
        else { std::memset(bufs[i].mapped, 0, static_cast<crd::usize>(bindings[i].bytes)); }
        if (bindings[i].binding > max_slot) { max_slot = bindings[i].binding; }
    }

    // ── descriptor set layout: binding 0 = TLAS · each Binding's slot = SSBO ──
    const crd::u32               nslots = max_slot + 1U;
    VkDescriptorSetLayoutBinding lb[16]{};
    for (crd::u32 s = 0; s < nslots; ++s) { lb[s].binding = s; lb[s].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[s].descriptorCount = 1; lb[s].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    lb[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; // binding 0 is the TLAS
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = nslots;
    dlci.pBindings    = lb;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(impl.device, &dlci, nullptr, &set_layout) != VK_SUCCESS) { return false; }

    // ── shader module + compute pipeline ──
    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirv.size();
    smci.pCode    = reinterpret_cast<const crd::u32*>(spirv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(impl.device, &smci, nullptr, &module) != VK_SUCCESS) { vkDestroyDescriptorSetLayout(impl.device, set_layout, nullptr); return false; }
    VkPipelineLayoutCreateInfo plci{};
    plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &set_layout;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(impl.device, &plci, nullptr, &pipe_layout);
    VkComputePipelineCreateInfo cpci{};
    cpci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName  = "main";
    cpci.layout       = pipe_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr = vkCreateComputePipelines(impl.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    vkDestroyShaderModule(impl.device, module, nullptr);
    if (pr != VK_SUCCESS) { vkDestroyPipelineLayout(impl.device, pipe_layout, nullptr); vkDestroyDescriptorSetLayout(impl.device, set_layout, nullptr); return false; }

    // ── descriptor pool + set ──
    VkDescriptorPoolSize psizes[2]{};
    psizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; psizes[0].descriptorCount = 1;
    psizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             psizes[1].descriptorCount = static_cast<crd::u32>(nbuf);
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = psizes;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(impl.device, &dpci, nullptr, &desc_pool);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(impl.device, &dsai, &set);

    VkWriteDescriptorSetAccelerationStructureKHR as_write{};
    as_write.sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures    = &scene.tlas;
    VkDescriptorBufferInfo bi[16]{};
    VkWriteDescriptorSet   wr[17]{};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].pNext = &as_write; wr[0].dstSet = set; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (crd::usize i = 0; i < nbuf; ++i)
    {
        bi[i].buffer = bufs[i].buffer; bi[i].offset = 0; bi[i].range = bufs[i].bytes;
        VkWriteDescriptorSet& w = wr[i + 1];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set; w.dstBinding = bindings[i].binding; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(impl.device, static_cast<crd::u32>(nbuf) + 1U, wr, 0, nullptr);

    impl.submit_oneshot([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, groups > 0 ? groups : 1, 1, 1);
    });
    for (crd::usize i = 0; i < nbuf; ++i) // read back outputs
    {
        if (bindings[i].readback != nullptr) { std::memcpy(bindings[i].readback, bufs[i].mapped, static_cast<crd::usize>(bindings[i].bytes)); }
    }

    vkDestroyDescriptorPool(impl.device, desc_pool, nullptr);
    vkDestroyPipeline(impl.device, pipeline, nullptr);
    vkDestroyPipelineLayout(impl.device, pipe_layout, nullptr);
    vkDestroyDescriptorSetLayout(impl.device, set_layout, nullptr);
    return true;
}

bool VulkanRayTracingContext::trace_rays_pipeline(const RtScene& scene_base, crd::containers::ConstSpan<crd::u8> rgen,
                                                  crd::containers::ConstSpan<crd::u8> rmiss, crd::containers::ConstSpan<crd::u8> rchit,
                                                  crd::containers::ConstSpan<Binding> bindings, crd::u32 width, crd::u32 height,
                                                  crd::containers::ConstSpan<crd::u8> rahit)
{
    auto&              impl  = *m_impl;
    const RtSceneImpl& scene = static_cast<const RtSceneImpl&>(scene_base);
    const crd::usize   nbuf  = bindings.size();
    if (!impl.has_rtpipe || scene.tlas == VK_NULL_HANDLE || nbuf == 0 || nbuf > 15) { return false; }
    const bool have_ah = rahit.size() > 0;

    const auto mod = [&](crd::containers::ConstSpan<crd::u8> spv) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; smci.codeSize = spv.size(); smci.pCode = reinterpret_cast<const crd::u32*>(spv.data());
        VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(impl.device, &smci, nullptr, &m); return m;
    };
    VkShaderModule mrg = mod(rgen), mms = mod(rmiss), mch = mod(rchit);
    VkShaderModule mah = have_ah ? mod(rahit) : VK_NULL_HANDLE;
    if (mrg == VK_NULL_HANDLE || mms == VK_NULL_HANDLE || mch == VK_NULL_HANDLE || (have_ah && mah == VK_NULL_HANDLE)) { return false; }

    // ── device buffers per binding (upload inputs) ──
    DevBuffer bufs[16]{};
    crd::u32  max_slot = 0;
    for (crd::usize i = 0; i < nbuf; ++i)
    {
        if (!impl.make_buffer(bindings[i].bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, bufs[i])) { return false; }
        if (bindings[i].upload != nullptr) { std::memcpy(bufs[i].mapped, bindings[i].upload, static_cast<crd::usize>(bindings[i].bytes)); }
        else { std::memset(bufs[i].mapped, 0, static_cast<crd::usize>(bindings[i].bytes)); }
        if (bindings[i].binding > max_slot) { max_slot = bindings[i].binding; }
    }
    // ── descriptor set layout (b0 = TLAS, rest = SSBO), visible to all RT stages ──
    const crd::u32               nslots = max_slot + 1U;
    const VkShaderStageFlags     rtStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    VkDescriptorSetLayoutBinding lb[16]{};
    for (crd::u32 s = 0; s < nslots; ++s) { lb[s].binding = s; lb[s].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[s].descriptorCount = 1; lb[s].stageFlags = rtStages; }
    lb[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dlci.bindingCount = nslots; dlci.pBindings = lb;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(impl.device, &dlci, nullptr, &set_layout) != VK_SUCCESS) { return false; }
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; plci.setLayoutCount = 1; plci.pSetLayouts = &set_layout;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(impl.device, &plci, nullptr, &pipe_layout);

    // ── the RT pipeline: 3 stages (raygen/miss/closest-hit) → 3 groups (general/general/triangles-hit) ──
    VkPipelineShaderStageCreateInfo stages[4]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;      stages[0].module = mrg; stages[0].pName = "main";
    stages[1].sType = stages[0].sType;                                     stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;        stages[1].module = mms; stages[1].pName = "main";
    stages[2].sType = stages[0].sType;                                     stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; stages[2].module = mch; stages[2].pName = "main";
    crd::u32 nstage = 3;
    if (have_ah) { stages[3].sType = stages[0].sType; stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR; stages[3].module = mah; stages[3].pName = "main"; nstage = 4; }
    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
    for (int i = 0; i < 3; ++i) { groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR; groups[i].generalShader = VK_SHADER_UNUSED_KHR; groups[i].closestHitShader = VK_SHADER_UNUSED_KHR; groups[i].anyHitShader = VK_SHADER_UNUSED_KHR; groups[i].intersectionShader = VK_SHADER_UNUSED_KHR; }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;              groups[0].generalShader = 0; // raygen
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;              groups[1].generalShader = 1; // miss
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;  groups[2].closestHitShader = 2; // hit
    if (have_ah) { groups[2].anyHitShader = 3; } // P4: the hit group gains the alpha-test any-hit shader
    VkRayTracingPipelineCreateInfoKHR rtci{};
    rtci.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtci.stageCount = nstage; rtci.pStages = stages; rtci.groupCount = 3; rtci.pGroups = groups;
    rtci.maxPipelineRayRecursionDepth = 1; rtci.layout = pipe_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr = impl.create_rt_pipe(impl.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtci, nullptr, &pipeline);
    vkDestroyShaderModule(impl.device, mrg, nullptr); vkDestroyShaderModule(impl.device, mms, nullptr); vkDestroyShaderModule(impl.device, mch, nullptr);
    if (mah != VK_NULL_HANDLE) { vkDestroyShaderModule(impl.device, mah, nullptr); }
    if (pr != VK_SUCCESS) { vkDestroyPipelineLayout(impl.device, pipe_layout, nullptr); vkDestroyDescriptorSetLayout(impl.device, set_layout, nullptr); return false; }

    // ── shader binding table: 3 base-aligned regions (raygen / miss / hit), each holding its group handle ──
    const auto align_up = [](crd::u64 v, crd::u64 a) { return (v + a - 1U) & ~(a - 1U); };
    const crd::u64 region = align_up(impl.sbt_handle_size, impl.sbt_base_align);
    crd::containers::Array<crd::u8> handles(crd::memory::default_allocator());
    handles.resize(static_cast<crd::usize>(impl.sbt_handle_size) * 3U, 0U);
    impl.get_group_handles(impl.device, pipeline, 0, 3, handles.size(), handles.data());
    DevBuffer sbt{};
    if (!impl.make_buffer(region * 3U, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true, sbt)) { vkDestroyPipeline(impl.device, pipeline, nullptr); vkDestroyPipelineLayout(impl.device, pipe_layout, nullptr); vkDestroyDescriptorSetLayout(impl.device, set_layout, nullptr); return false; }
    auto* sbt_bytes = static_cast<crd::u8*>(sbt.mapped);
    std::memset(sbt_bytes, 0, static_cast<crd::usize>(region) * 3U);
    for (int gexp = 0; gexp < 3; ++gexp) { std::memcpy(sbt_bytes + static_cast<crd::usize>(region) * gexp, handles.data() + static_cast<crd::usize>(impl.sbt_handle_size) * gexp, impl.sbt_handle_size); }
    VkStridedDeviceAddressRegionKHR rgenR{}, missR{}, hitR{}, callR{};
    rgenR.deviceAddress = sbt.address;              rgenR.stride = region; rgenR.size = region;
    missR.deviceAddress = sbt.address + region;     missR.stride = region; missR.size = region;
    hitR.deviceAddress  = sbt.address + region * 2U; hitR.stride  = region; hitR.size  = region;

    // ── descriptor pool + set (TLAS + SSBOs) ──
    VkDescriptorPoolSize psizes[2]{};
    psizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; psizes[0].descriptorCount = 1;
    psizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             psizes[1].descriptorCount = static_cast<crd::u32>(nbuf);
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = psizes;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(impl.device, &dpci, nullptr, &desc_pool);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsai.descriptorPool = desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(impl.device, &dsai, &set);
    VkWriteDescriptorSetAccelerationStructureKHR as_write{};
    as_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR; as_write.accelerationStructureCount = 1; as_write.pAccelerationStructures = &scene.tlas;
    VkDescriptorBufferInfo bi[16]{};
    VkWriteDescriptorSet   wr[17]{};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].pNext = &as_write; wr[0].dstSet = set; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (crd::usize i = 0; i < nbuf; ++i)
    {
        bi[i].buffer = bufs[i].buffer; bi[i].offset = 0; bi[i].range = bufs[i].bytes;
        VkWriteDescriptorSet& w = wr[i + 1];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set; w.dstBinding = bindings[i].binding; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(impl.device, static_cast<crd::u32>(nbuf) + 1U, wr, 0, nullptr);

    impl.submit_oneshot([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipe_layout, 0, 1, &set, 0, nullptr);
        impl.cmd_trace_rays(cmd, &rgenR, &missR, &hitR, &callR, width > 0 ? width : 1, height > 0 ? height : 1, 1);
    });
    for (crd::usize i = 0; i < nbuf; ++i) { if (bindings[i].readback != nullptr) { std::memcpy(bindings[i].readback, bufs[i].mapped, static_cast<crd::usize>(bindings[i].bytes)); } }

    vkDestroyDescriptorPool(impl.device, desc_pool, nullptr);
    vkDestroyPipeline(impl.device, pipeline, nullptr);
    vkDestroyPipelineLayout(impl.device, pipe_layout, nullptr);
    vkDestroyDescriptorSetLayout(impl.device, set_layout, nullptr);
    return true;
}

} // namespace crd::gpu
