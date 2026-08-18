// vulkan_dgc_context.cpp — CEIR-20c-2: VK_EXT_device_generated_commands execution. See the header for the "GPU authors the
// command stream" contract. The minimal spec-correct shape: an indirect commands layout of a single DISPATCH token +
// indirectExecutionSet = VK_NULL_HANDLE (the consume pipeline supplied via VkGeneratedCommandsPipelineInfoEXT in pNext, and bound
// as inherited state), sequenceCountAddress = the produce-written count. Barriers use the LEGACY vkCmdPipelineBarrier (sync2 is
// not enabled on the headless compute device) to publish the produce writes to the COMMAND_PREPROCESS + COMPUTE_SHADER read stages.

#include <crd/gpu/vulkan_dgc_context.hpp>

#include <cstring>

namespace crd::gpu
{
namespace
{
// A device buffer, host-visible for direct upload/readback unless it carries its own private (device-local) memory. Every buffer
// here also carries a device address (the token/count/preprocess buffers are referenced by address; on the rest it is harmless).
struct DgcBuf
{
    VkBuffer        buffer  = VK_NULL_HANDLE;
    VkDeviceMemory  memory  = VK_NULL_HANDLE;
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

struct VulkanDgcContext::Impl
{
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    crd::u32         family   = UINT32_MAX;
    VkCommandPool    cmd_pool = VK_NULL_HANDLE;
    bool             ok       = false;

    // VK_EXT_device_generated_commands entry points (loaded via vkGetDeviceProcAddr once the cap is confirmed).
    PFN_vkCreateIndirectCommandsLayoutEXT          create_layout  = nullptr;
    PFN_vkDestroyIndirectCommandsLayoutEXT         destroy_layout = nullptr;
    PFN_vkGetGeneratedCommandsMemoryRequirementsEXT get_gc_memreq = nullptr;
    PFN_vkCmdExecuteGeneratedCommandsEXT           cmd_execute    = nullptr;

    // Allocate a buffer. `usage64 != 0` uses VkBufferUsageFlags2 (the DGC preprocess bit is 64-bit only); else `usage32`. The
    // memory type is chosen from (buffer requirements) ∩ `extra_type_bits` with `want` properties; host-visible buffers are mapped.
    [[nodiscard]] DgcBuf make_buf(crd::u64 bytes, VkBufferUsageFlags usage32, VkBufferUsageFlags2 usage64, crd::u32 extra_type_bits,
                                  VkMemoryPropertyFlags want) const noexcept
    {
        DgcBuf b{};
        b.bytes = bytes;
        VkBufferUsageFlags2CreateInfo u2{};
        u2.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO;
        u2.usage = usage64;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.pNext = usage64 != 0U ? &u2 : nullptr;
        bci.size  = bytes;
        bci.usage = usage64 != 0U ? 0U : usage32; // when Flags2 is chained the 32-bit field is ignored (and must be 0)
        if (vkCreateBuffer(device, &bci, nullptr, &b.buffer) != VK_SUCCESS) { return b; }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        VkMemoryAllocateFlagsInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext           = &fi;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_mem_type(physical, mr.memoryTypeBits & extra_type_bits, want);
        if (mai.memoryTypeIndex == UINT32_MAX) { return b; }
        if (vkAllocateMemory(device, &mai, nullptr, &b.memory) != VK_SUCCESS) { return b; }
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        VkBufferDeviceAddressInfo ai{};
        ai.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        ai.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &ai);
        if ((want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) { vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped); }
        return b;
    }

    void free_buf(DgcBuf& b) const noexcept
    {
        if (b.mapped != nullptr) { vkUnmapMemory(device, b.memory); }
        if (b.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, b.buffer, nullptr); }
        if (b.memory != VK_NULL_HANDLE) { vkFreeMemory(device, b.memory, nullptr); }
        b = DgcBuf{};
    }

    // Build a plain compute pipeline from SPIR-V with `nbind` std430 storage-buffer bindings (0..nbind-1). Returns the pipeline,
    // its layout, descriptor-set layout, and shader module (all owned by the caller).
    struct CompPipe
    {
        VkPipeline            pipe   = VK_NULL_HANDLE;
        VkPipelineLayout      layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl    = VK_NULL_HANDLE;
        VkShaderModule        module = VK_NULL_HANDLE;
    };
    [[nodiscard]] CompPipe make_pipe(crd::containers::ConstSpan<crd::u8> spirv, crd::u32 nbind) const noexcept
    {
        CompPipe p{};
        VkDescriptorSetLayoutBinding binds[16]{};
        for (crd::u32 i = 0; i < nbind; ++i)
        {
            binds[i].binding         = i;
            binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = nbind;
        dslci.pBindings    = binds;
        if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &p.dsl) != VK_SUCCESS) { return p; }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts    = &p.dsl;
        if (vkCreatePipelineLayout(device, &plci, nullptr, &p.layout) != VK_SUCCESS) { return p; }
        VkShaderModuleCreateInfo smci{};
        smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spirv.size();
        smci.pCode    = reinterpret_cast<const crd::u32*>(spirv.data());
        if (vkCreateShaderModule(device, &smci, nullptr, &p.module) != VK_SUCCESS) { return p; }
        VkComputePipelineCreateInfo cpci{};
        cpci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.layout       = p.layout;
        cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = p.module;
        cpci.stage.pName  = "main";
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipe);
        return p;
    }
    void free_pipe(CompPipe& p) const noexcept
    {
        if (p.pipe != VK_NULL_HANDLE) { vkDestroyPipeline(device, p.pipe, nullptr); }
        if (p.module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, p.module, nullptr); }
        if (p.layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, p.layout, nullptr); }
        if (p.dsl != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, p.dsl, nullptr); }
        p = CompPipe{};
    }
};

VulkanDgcContext::VulkanDgcContext(VulkanGpuContext& ctx) : m_impl(std::make_unique<Impl>())
{
    Impl& impl    = *m_impl;
    impl.physical = ctx.vk_physical_device();
    impl.device   = ctx.vk_device();
    impl.queue    = ctx.compute_queue();
    impl.family   = ctx.compute_family();
    if (!ctx.device_generated_commands_ext() || impl.device == VK_NULL_HANDLE) { return; } // adapter without the cap ⇒ invalid

    impl.create_layout  = reinterpret_cast<PFN_vkCreateIndirectCommandsLayoutEXT>(vkGetDeviceProcAddr(impl.device, "vkCreateIndirectCommandsLayoutEXT"));
    impl.destroy_layout = reinterpret_cast<PFN_vkDestroyIndirectCommandsLayoutEXT>(vkGetDeviceProcAddr(impl.device, "vkDestroyIndirectCommandsLayoutEXT"));
    impl.get_gc_memreq  = reinterpret_cast<PFN_vkGetGeneratedCommandsMemoryRequirementsEXT>(vkGetDeviceProcAddr(impl.device, "vkGetGeneratedCommandsMemoryRequirementsEXT"));
    impl.cmd_execute    = reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsEXT>(vkGetDeviceProcAddr(impl.device, "vkCmdExecuteGeneratedCommandsEXT"));
    if (impl.create_layout == nullptr || impl.destroy_layout == nullptr || impl.get_gc_memreq == nullptr || impl.cmd_execute == nullptr) { return; }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = impl.family;
    if (vkCreateCommandPool(impl.device, &cpci, nullptr, &impl.cmd_pool) != VK_SUCCESS) { return; }
    impl.ok = true;
}

VulkanDgcContext::~VulkanDgcContext()
{
    if (m_impl && m_impl->cmd_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_impl->device, m_impl->cmd_pool, nullptr); }
}

bool VulkanDgcContext::valid() const noexcept { return m_impl && m_impl->ok; }

bool VulkanDgcContext::dispatch_generated(const ExecuteDesc& desc, crd::containers::ConstSpan<Buffer> buffers)
{
    if (!valid()) { return false; }
    Impl&          impl = *m_impl;
    const VkDevice dev  = impl.device;
    const crd::u32 nbuf = static_cast<crd::u32>(buffers.size());
    if (nbuf == 0U || nbuf > 8U || desc.max_seq == 0U) { return false; }

    // ── the global buffers: host-visible + device-address, STORAGE|INDIRECT (the token/count buffers are read as the command
    //    stream / sequence count; the rest are plain storage). Zero-init, then apply any host upload. ──
    DgcBuf gbuf[8]{};
    bool   built = true;
    for (crd::u32 i = 0; i < nbuf; ++i)
    {
        gbuf[i] = impl.make_buf(buffers[i].bytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                0U, 0xFFFFFFFFU, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (gbuf[i].mapped == nullptr) { built = false; break; }
        std::memset(gbuf[i].mapped, 0, static_cast<crd::usize>(buffers[i].bytes));
        if (buffers[i].upload != nullptr) { std::memcpy(gbuf[i].mapped, buffers[i].upload, static_cast<crd::usize>(buffers[i].bytes)); }
    }

    // ── the produce (plain compute) + consume pipelines. The consume needs NO indirect-bindable flag: with a NULL execution set
    //    the pipeline is supplied via VkGeneratedCommandsPipelineInfoEXT and bound as inherited state (spec-confirmed minimal path). ──
    Impl::CompPipe prod = built ? impl.make_pipe(desc.produce_spirv, desc.n_produce_binds) : Impl::CompPipe{};
    Impl::CompPipe cons = built ? impl.make_pipe(desc.consume_spirv, desc.n_consume_binds) : Impl::CompPipe{};
    built = built && prod.pipe != VK_NULL_HANDLE && cons.pipe != VK_NULL_HANDLE;

    // ── the indirect commands layout: a lone DISPATCH token (offset 0), COMPUTE stage, the consume's pipeline layout, NO
    //    execution-set token (⇒ indirectExecutionSet must be NULL, pipeline via pNext). indirectStride = sizeof VkDispatchIndirectCommand. ──
    VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
    if (built)
    {
        VkIndirectCommandsLayoutTokenEXT tok{};
        tok.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
        tok.type  = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT;
        tok.offset = 0;
        VkIndirectCommandsLayoutCreateInfoEXT lci{};
        lci.sType          = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
        lci.shaderStages   = VK_SHADER_STAGE_COMPUTE_BIT;
        lci.indirectStride = desc.dispatch_stride;
        lci.pipelineLayout = cons.layout;
        lci.tokenCount     = 1;
        lci.pTokens        = &tok;
        built = impl.create_layout(dev, &lci, nullptr, &layout) == VK_SUCCESS;
    }

    // ── the preprocess buffer (device-generated-commands memory requirements). PREPROCESS_BUFFER is a 64-bit usage flag; its
    //    memory type must be within the generated-commands requirement's mask. Skipped when the requirement is zero-size. ──
    DgcBuf preproc{};
    VkDeviceSize preproc_size = 0;
    if (built)
    {
        VkGeneratedCommandsMemoryRequirementsInfoEXT gmri{};
        gmri.sType                  = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
        gmri.indirectExecutionSet   = VK_NULL_HANDLE;
        gmri.indirectCommandsLayout = layout;
        gmri.maxSequenceCount       = desc.max_seq;
        gmri.maxDrawCount           = 0;
        VkMemoryRequirements2 gmr{};
        gmr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        impl.get_gc_memreq(dev, &gmri, &gmr);
        preproc_size = gmr.memoryRequirements.size;
        if (preproc_size > 0U)
        {
            preproc = impl.make_buf(preproc_size, 0U,
                                    VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
                                    gmr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            built = preproc.buffer != VK_NULL_HANDLE && preproc.memory != VK_NULL_HANDLE;
        }
    }

    // ── descriptor pool + the two sets (produce, consume); write each binding N to buffers[binds[N]]. ──
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet  prod_set = VK_NULL_HANDLE;
    VkDescriptorSet  cons_set = VK_NULL_HANDLE;
    if (built)
    {
        VkDescriptorPoolSize dps{};
        dps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dps.descriptorCount = desc.n_produce_binds + desc.n_consume_binds;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets       = 2;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &dps;
        built = vkCreateDescriptorPool(dev, &dpci, nullptr, &pool) == VK_SUCCESS;
        if (built)
        {
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool     = pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts        = &prod.dsl;
            built = vkAllocateDescriptorSets(dev, &dsai, &prod_set) == VK_SUCCESS;
            dsai.pSetLayouts        = &cons.dsl;
            built = built && vkAllocateDescriptorSets(dev, &dsai, &cons_set) == VK_SUCCESS;
        }
        if (built)
        {
            VkDescriptorBufferInfo dbi[16]{};
            VkWriteDescriptorSet   wr[16]{};
            crd::u32               nw = 0;
            const auto             add_writes = [&](VkDescriptorSet set, const crd::u32* binds, crd::u32 n) {
                for (crd::u32 i = 0; i < n; ++i)
                {
                    dbi[nw].buffer       = gbuf[binds[i]].buffer;
                    dbi[nw].range        = VK_WHOLE_SIZE;
                    wr[nw].sType         = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    wr[nw].dstSet        = set;
                    wr[nw].dstBinding    = i;
                    wr[nw].descriptorCount = 1;
                    wr[nw].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    wr[nw].pBufferInfo   = &dbi[nw];
                    ++nw;
                }
            };
            add_writes(prod_set, desc.produce_binds, desc.n_produce_binds);
            add_writes(cons_set, desc.consume_binds, desc.n_consume_binds);
            vkUpdateDescriptorSets(dev, nw, wr, 0, nullptr);
        }
    }

    // ── record: dispatch the produce, publish its writes to the command-preprocess + consume-read stages (legacy barrier — sync2
    //    is off headless), bind the consume pipeline + set as inherited state, then GENERATE + execute the command stream. ──
    bool ran = false;
    if (built)
    {
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = impl.cmd_pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &cbai, &cmd) == VK_SUCCESS)
        {
            VkCommandBufferBeginInfo cbbi{};
            cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbbi);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prod.pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prod.layout, 0, 1, &prod_set, 0, nullptr);
            vkCmdDispatch(cmd, 1, 1, 1); // the produce is a 1-thread kernel that writes the whole command stream + count

            // publish: the produce's SHADER_WRITE → the command-stream read (COMMAND_PREPROCESS + INDIRECT_COMMAND) AND the
            // consume's read of the count buffer as a storage descriptor (COMPUTE_SHADER).
            VkMemoryBarrier mb{};
            mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_COMMAND_PREPROCESS_READ_BIT_EXT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMMAND_PREPROCESS_BIT_EXT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT
                                     | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);

            // the generated commands inherit the bound pipeline + descriptor set (there is no execution set to switch them).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cons.pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cons.layout, 0, 1, &cons_set, 0, nullptr);

            VkGeneratedCommandsPipelineInfoEXT gpi{};
            gpi.sType    = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT;
            gpi.pipeline = cons.pipe;
            VkGeneratedCommandsInfoEXT gci{};
            gci.sType                  = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
            gci.pNext                  = &gpi;
            gci.shaderStages           = VK_SHADER_STAGE_COMPUTE_BIT;
            gci.indirectExecutionSet   = VK_NULL_HANDLE;
            gci.indirectCommandsLayout = layout;
            gci.indirectAddress        = gbuf[desc.token_buffer].address;
            gci.indirectAddressSize    = gbuf[desc.token_buffer].bytes;
            gci.preprocessAddress      = preproc_size > 0U ? preproc.address : 0;
            gci.preprocessSize         = preproc_size;
            gci.maxSequenceCount       = desc.max_seq;
            gci.sequenceCountAddress   = gbuf[desc.count_buffer].address;
            gci.maxDrawCount           = 0;
            impl.cmd_execute(cmd, VK_FALSE, &gci);

            // publish the consume's writes to the host for readback.
            VkMemoryBarrier hb{};
            hb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);

            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{};
            si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers    = &cmd;
            if (vkQueueSubmit(impl.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS)
            {
                vkQueueWaitIdle(impl.queue);
                ran = true;
            }
            vkFreeCommandBuffers(dev, impl.cmd_pool, 1, &cmd);
        }
    }

    // ── read back ──
    if (ran)
    {
        for (crd::u32 i = 0; i < nbuf; ++i)
        {
            if (buffers[i].readback != nullptr && gbuf[i].mapped != nullptr)
            {
                std::memcpy(buffers[i].readback, gbuf[i].mapped, static_cast<crd::usize>(buffers[i].bytes));
            }
        }
    }

    // ── teardown ──
    if (pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(dev, pool, nullptr); }
    if (layout != VK_NULL_HANDLE) { impl.destroy_layout(dev, layout, nullptr); }
    impl.free_buf(preproc);
    impl.free_pipe(cons);
    impl.free_pipe(prod);
    for (crd::u32 i = 0; i < nbuf; ++i) { impl.free_buf(gbuf[i]); }
    return ran;
}

} // namespace crd::gpu
