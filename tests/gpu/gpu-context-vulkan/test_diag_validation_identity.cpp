// DIAG.7a(d1): the Vulkan validation capture resolves the stable Cerid identity from the debug-utils names of the
// objects a message references (falling back to the message prose). Device test on the real GPU here: it drives a
// synthetic message through vkSubmitDebugUtilsMessageEXT so it exercises the SAME callback the live validation layer
// uses, without needing to provoke a real validation error (that correlated-hazard proof is sub-unit (g)).

#include <crd/gpu/identity_registry.hpp>   // DIAG.7a(g): alive(id) after the hazard correlates
#include <crd/gpu/object_identity.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>

#include "vulkan_execution.hpp"      // DIAG.7a lifetime: the submit goes through the bounded completion seam
#include "vulkan_identity_naming.hpp" // DIAG.7a(g): detail::vk_attach_identity/vk_detach_identity (the real naming path)
#include <crd/gpu/vulkan_compute_context.hpp> // DIAG.7a(g-3): dispatch a kernel that OOBs a storage buffer
#include <crd/gpu/vulkan_shader_compile.hpp>  // DIAG.7a(g-3): compile_glsl_to_spirv
#include <crd/memory/allocator.hpp>           // DIAG.7a(g-3): default_allocator for the compute context

#include <vulkan/vulkan.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace
{
namespace gpu = crd::gpu;

// Submit one synthetic debug-utils message on `instance`, optionally naming one object. Returns false if the entry
// point is missing (the caller REQUIREs it -- never skip-as-pass).
struct Submitter
{
    PFN_vkSubmitDebugUtilsMessageEXT fn = nullptr;
    VkInstance                       instance = VK_NULL_HANDLE;

    void warn(crd::i32 id_number, const char* message, const char* object_name) const
    {
        VkDebugUtilsObjectNameInfoEXT object{};
        object.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        object.objectType   = VK_OBJECT_TYPE_INSTANCE;               // a REAL object handle+type, so the layer adds no VUID noise
        object.objectHandle = reinterpret_cast<crd::u64>(instance);
        object.pObjectName  = object_name;                           // may be nullptr (no named object)

        VkDebugUtilsMessengerCallbackDataEXT data{};
        data.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
        data.pMessageIdName  = "crd-diag-identity";
        data.messageIdNumber = id_number;
        data.pMessage        = message;
        data.objectCount     = object_name != nullptr ? 1U : 0U;
        data.pObjects        = object_name != nullptr ? &object : nullptr;

        // Severity + type inside the capture messenger's masks (INFO|WARNING|ERROR / GENERAL|VALIDATION|PERFORMANCE).
        fn(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);
    }
};

[[nodiscard]] const gpu::ValidationMessage* find_by_id(const gpu::ValidationCapture& capture, crd::i32 id_number)
{
    for (const auto& record : capture.messages())
    {
        if (record.message_id_number == id_number)
        {
            return &record;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("Vulkan validation resolves the Cerid identity from a named object then message prose",
          "[gpu-context][vulkan][gpu][validation][identity]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true; // the instance must enable VK_EXT_debug_utils or the capture stays silent
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) // absence of a GPU, not a failure
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    REQUIRE(vk->vk_instance() != VK_NULL_HANDLE);

    gpu::ValidationCapture capture(*vk);
    REQUIRE(gpu::validation_layer_spec_version() != 0U); // layer present, else the counts are falsely zero

    Submitter submit{reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
                         vkGetInstanceProcAddr(vk->vk_instance(), "vkSubmitDebugUtilsMessageEXT")),
                     vk->vk_instance()};
    REQUIRE(submit.fn != nullptr); // the entry point exists on this loader -- do not skip-as-pass

    // (1) token carried by the named object's debug-utils name; message prose has none.
    const crd::u32 before1 = capture.warning_count();
    submit.warn(0x7A0D01, "resource was reset", "[crd:res:0000002a:g00000007] shadowmap 2048x2048");
    CHECK(capture.warning_count() - before1 == 1U); // exactly one messenger saw it; no layer-side VUID noise
    const gpu::ValidationMessage* named = find_by_id(capture, 0x7A0D01);
    REQUIRE(named != nullptr);
    CHECK(named->identity.valid());
    CHECK(named->identity == (gpu::ObjectIdentity{gpu::ObjectKind::Resource, 0x2AU, 0x7U}));

    // (2) no named object, but the message prose carries the token -> prose fallback.
    const crd::u32 before2 = capture.warning_count();
    submit.warn(0x7A0D02, "program crd:prog:00000005:g00000002 failed to bind", nullptr);
    CHECK(capture.warning_count() - before2 == 1U);
    const gpu::ValidationMessage* prose = find_by_id(capture, 0x7A0D02);
    REQUIRE(prose != nullptr);
    CHECK(prose->identity.valid());
    CHECK(prose->identity == (gpu::ObjectIdentity{gpu::ObjectKind::Program, 0x5U, 0x2U}));

    // (3) neither the named object nor the prose carries a token -> default-invalid identity.
    const crd::u32 before3 = capture.warning_count();
    submit.warn(0x7A0D03, "plain warning, no cerid token", "shadowmap (no token)");
    CHECK(capture.warning_count() - before3 == 1U);
    const gpu::ValidationMessage* plain = find_by_id(capture, 0x7A0D03);
    REQUIRE(plain != nullptr);
    CHECK_FALSE(plain->identity.valid());

    CHECK(capture.dropped_count() == 0U); // three records, well under the 256 cap
}

// DIAG.7a(g): the correlated-hazard acceptance (Vulkan Core route). An intentional usage-bounds hazard on a NAMED Cerid buffer
// (an oversized vkCmdFillBuffer -- a record-time VUID, no submit, so the device is never removed) produces a real Core
// validation error whose object carries the buffer's debug-utils name; the capture resolves it back to the SAME
// ObjectIdentity that is still alive() in the registry. The valid consumer (a correctly-sized fill on the same named
// buffer) stays clean. This is the acceptance clause end-to-end: "correlated error ... valid consumers stay clean".
namespace
{
[[nodiscard]] const gpu::ValidationMessage* find_by_identity(const gpu::ValidationCapture& capture,
                                                             const gpu::ObjectIdentity& id)
{
    for (const auto& record : capture.messages())
    {
        if (record.identity == id)
        {
            return &record;
        }
    }
    return nullptr;
}

[[nodiscard]] crd::u32 find_memory_type(VkPhysicalDevice phys, crd::u32 type_bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (crd::u32 i = 0; i < props.memoryTypeCount; ++i)
    {
        if ((type_bits & (1U << i)) != 0U && (props.memoryTypes[i].propertyFlags & want) == want)
        {
            return i;
        }
    }
    return UINT32_MAX;
}
} // namespace

TEST_CASE("DIAG.7a(g): a Core hazard on a named Cerid buffer yields an error correlated to its identity; valid stays clean",
          "[gpu-context][vulkan][gpu][validation][identity][hazard]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true; // Core
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    REQUIRE(gpu::validation_layer_spec_version() != 0U); // layer present, else a clean count is meaningless

    const VkDevice         device = vk->vk_device();
    const VkPhysicalDevice phys   = vk->vk_physical_device();
    const crd::u32         family = vk->compute_family();

    // A small named Cerid buffer (TRANSFER_DST so vkCmdFillBuffer is a legal op whose SIZE the layer checks).
    constexpr VkDeviceSize size_bytes = 16;
    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = size_bytes;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    REQUIRE(vkCreateBuffer(device, &bci, nullptr, &buffer) == VK_SUCCESS);

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const crd::u32 mem_type = find_memory_type(phys, req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    REQUIRE(mem_type != UINT32_MAX);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = mem_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &mai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);

    // Name it via the REAL production path: mint a Resource identity + stamp the debug-utils name onto the buffer.
    const gpu::ObjectIdentity id = gpu::detail::vk_attach_identity(
        device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<crd::u64>(buffer), gpu::ObjectKind::Resource, "vk-hazard-buffer");
    REQUIRE(id.valid());
    REQUIRE(gpu::identity_registry().alive(id));

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    REQUIRE(vkCreateCommandPool(device, &pci, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(device, &cbai, &cb) == VK_SUCCESS);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    // ── HAZARD: fill 64 bytes into a 16-byte buffer -> a record-time VUID naming the buffer. ──
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS);
        vkCmdFillBuffer(cb, buffer, 0, 64, 0xDEADBEEFU); // 64 > 16: VUID-vkCmdFillBuffer-size
        (void)vkEndCommandBuffer(cb);

        for (const auto& rec : capture.messages())
        {
            UNSCOPED_INFO("msg id=" << rec.message_id_number << " ident.valid=" << rec.identity.valid()
                          << " text=" << rec.message_text.c_str());
        }
        CHECK(capture.dropped_count() == 0U);               // correlate over a complete set, not a partial one
        CHECK(capture.error_count() >= 1U);                 // the hazard fired
        const gpu::ValidationMessage* hit = find_by_identity(capture, id);
        REQUIRE(hit != nullptr);                            // an error carries THIS buffer's identity
        CHECK(hit->identity == id);
        CHECK(hit->severity == gpu::ValidationSeverity::Error); // the correlated record is the VUID, not a stray warning
        CHECK(gpu::identity_registry().alive(id));          // the correlated object is the live one
    }

    // ── VALID consumer on the SAME named buffer: a correctly-sized fill -> no correlated error. ──
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkResetCommandBuffer(cb, 0) == VK_SUCCESS);
        REQUIRE(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS);
        vkCmdFillBuffer(cb, buffer, 0, size_bytes, 0U);          // legal
        REQUIRE(vkEndCommandBuffer(cb) == VK_SUCCESS);      // a valid consumer ends cleanly
        CHECK(capture.error_or_warning_count() == 0U);      // clean, not merely "not correlated"
        CHECK(find_by_identity(capture, id) == nullptr);    // and nothing correlated to this buffer
        CHECK(capture.dropped_count() == 0U);
    }

    vkDestroyCommandPool(device, pool, nullptr);
    gpu::detail::vk_detach_identity(id);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
}

// DIAG.7a(g-2): Vulkan SYNCHRONIZATION validation correlated-hazard pair -- the true mode-ON/OFF discriminator (sync-val is
// per-INSTANCE, so both sides run in one process). A write-after-write on a NAMED Cerid buffer with no barrier, recorded
// under enable_sync_validation=true, is flagged by syncval at record time (no submit); the same two writes WITH a barrier
// stay clean; and a SECOND context with sync OFF (Core only) does NOT flag the hazard -- report and behaviour agree in both
// directions. The VUID NAME (SYNC-HAZARD-*) rides in pMessageIdName which the capture does not store, so the body carries
// "WRITE_AFTER_WRITE"; the buffer is resolved via pObjects (objects-first), the same route as g-1 (syncval lists it).
namespace
{
// A named Cerid buffer + its command pool/buffer on one context's device -- reused for the sync-on and sync-off contexts.
struct HazardRig
{
    VkDevice            device = VK_NULL_HANDLE;
    VkBuffer            buffer = VK_NULL_HANDLE;
    VkDeviceMemory      memory = VK_NULL_HANDLE;
    VkCommandPool       pool   = VK_NULL_HANDLE;
    VkCommandBuffer     cb     = VK_NULL_HANDLE;
    gpu::ObjectIdentity id{};

    [[nodiscard]] bool setup(gpu::VulkanGpuContext& vk, VkDeviceSize size, const char* name)
    {
        device                      = vk.vk_device();
        const VkPhysicalDevice phys = vk.vk_physical_device();
        const crd::u32         fam  = vk.compute_family();

        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = size;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS)
        {
            return false;
        }

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer, &req);
        const crd::u32 mt = find_memory_type(phys, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX)
        {
            return false;
        }
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS)
        {
            return false;
        }
        if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
        {
            return false;
        }

        if (name != nullptr) // DIAG.7a(g-4): name==nullptr leaves the buffer UNNAMED (so a Pass label is the only token)
        {
            id = gpu::detail::vk_attach_identity(device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<crd::u64>(buffer),
                                                 gpu::ObjectKind::Resource, name);
        }

        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = fam;
        if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        return vkAllocateCommandBuffers(device, &cbai, &cb) == VK_SUCCESS;
    }

    void teardown()
    {
        if (pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, pool, nullptr);
        }
        gpu::detail::vk_detach_identity(id);
        if (buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, memory, nullptr);
        }
    }
};

// A syncval hazard message names the class (WRITE_AFTER_WRITE) in its body; the VUID name is not stored.
[[nodiscard]] bool mentions_waw(const gpu::ValidationCapture& capture)
{
    return std::ranges::any_of(capture.messages(), [](const auto& r) {
        return std::string_view(r.message_text.c_str()).find("WRITE_AFTER_WRITE") != std::string_view::npos;
    });
}
} // namespace

TEST_CASE("DIAG.7a(g-2): sync validation flags a WAW on a named buffer (correlated); barrier and sync-off stay clean",
          "[gpu-context][vulkan][gpu][validation][identity][hazard][sync]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend                = gpu::GpuBackend::Vulkan;
    cfg.headless               = true;
    cfg.enable_validation      = true; // Core (also names the object)
    cfg.enable_sync_validation = true; // Synchronization
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    REQUIRE(gpu::validation_layer_spec_version() != 0U);
    const bool sync_active = vk->validation_activation().is_active(gpu::ValidationMode::Synchronization);

    HazardRig rig;
    REQUIRE(rig.setup(*vk, 256, "vk-sync-hazard-buffer"));
    REQUIRE(rig.id.valid());

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    // (a) HAZARD: two writes to the same range, NO barrier, sync ON -> WAW flagged, correlated to the buffer.
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkBeginCommandBuffer(rig.cb, &bi) == VK_SUCCESS);
        vkCmdFillBuffer(rig.cb, rig.buffer, 0, 256, 0x11111111U);
        vkCmdFillBuffer(rig.cb, rig.buffer, 0, 256, 0x22222222U); // WAW, no barrier
        (void)vkEndCommandBuffer(rig.cb);
        for (const auto& r : capture.messages())
        {
            UNSCOPED_INFO("msg ident.valid=" << r.identity.valid() << " text=" << r.message_text.c_str());
        }
        CHECK(capture.dropped_count() == 0U);
        REQUIRE(mentions_waw(capture));                 // sync validation fired at record time
        CHECK(sync_active);                             // the activation REPORT agrees with the observed behaviour
        const gpu::ValidationMessage* hit = find_by_identity(capture, rig.id);
        REQUIRE(hit != nullptr);                        // the hazard is correlated to THIS buffer's identity
        CHECK(gpu::identity_registry().alive(rig.id));
    }

    // (b) VALID: same two writes WITH a TRANSFER_WRITE->TRANSFER_WRITE barrier, sync ON -> clean.
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkResetCommandBuffer(rig.cb, 0) == VK_SUCCESS);
        REQUIRE(vkBeginCommandBuffer(rig.cb, &bi) == VK_SUCCESS);
        vkCmdFillBuffer(rig.cb, rig.buffer, 0, 256, 0x11111111U);
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(rig.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
        vkCmdFillBuffer(rig.cb, rig.buffer, 0, 256, 0x22222222U);
        REQUIRE(vkEndCommandBuffer(rig.cb) == VK_SUCCESS);
        CHECK_FALSE(mentions_waw(capture));             // the barrier resolves the hazard
        CHECK(capture.error_or_warning_count() == 0U);  // and the valid consumer is clean, not merely "no WAW"
    }
    rig.teardown();

    // (c) MODE-OFF discriminator: a SECOND context, Core only (sync OFF), same unbarriered writes -> NO WAW.
    gpu::GpuContextConfig core_cfg;
    core_cfg.backend           = gpu::GpuBackend::Vulkan;
    core_cfg.headless          = true;
    core_cfg.enable_validation = true; // sync deliberately OFF
    auto ctx2 = gpu::create_vulkan_gpu_context(core_cfg);
    if (ctx2 != nullptr && ctx2->valid())
    {
        auto* vk2 = static_cast<gpu::VulkanGpuContext*>(ctx2.get());
        CHECK_FALSE(vk2->validation_activation().is_active(gpu::ValidationMode::Synchronization)); // report: OFF
        HazardRig rig2;
        REQUIRE(rig2.setup(*vk2, 256, "vk-sync-core-only-buffer"));
        gpu::ValidationCapture capture(*vk2);
        VkCommandBufferBeginInfo bi2{};
        bi2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        REQUIRE(vkBeginCommandBuffer(rig2.cb, &bi2) == VK_SUCCESS);
        vkCmdFillBuffer(rig2.cb, rig2.buffer, 0, 256, 0x11111111U);
        vkCmdFillBuffer(rig2.cb, rig2.buffer, 0, 256, 0x22222222U); // WAW, but sync is OFF
        (void)vkEndCommandBuffer(rig2.cb);
        for (const auto& r : capture.messages())
        {
            UNSCOPED_INFO("core-only msg text=" << r.message_text.c_str()); // a WAW here => syncval enabled by the ENV
        }
        CHECK_FALSE(mentions_waw(capture));             // THE discriminator: sync off -> no synchronization hazard
        CHECK(capture.error_or_warning_count() == 0U);  // two unbarriered fills are Core-valid
        rig2.teardown();
    }
}

// DIAG.7a(g-3): GPU-ASSISTED (GPU-AV) correlated-hazard specimen -- the descriptor/buffer-OOB route. A compute kernel
// writes a storage buffer OUT OF BOUNDS via a data-driven index; GPU-AV instruments the shader and reports the access
// at the fence wait. MEMORY-SAFE BY CONSTRUCTION (2026-10-05): the first version trusted GPU-AV to neutralize the
// access, but on lavapipe the store executed and segfaulted the process (even with the layer's safe mode, even with
// instrumentation off). Now the 16-byte buffer the descriptor names is bound at offset 0 of an 8 MiB allocation the test
// owns, so the out-of-bounds index (byte 4,000,000) is out of bounds for the DESCRIPTOR -- what GPU-AV checks -- while
// the physical store, if any device executes it, lands inside that allocation. Raw Vulkan, because the compute context
// gives every buffer an allocation of exactly its size. The pipeline (Program) and the buffer (Resource) are Cerid-named
// through the production adapter; a GPU-AV record must resolve to one of those LIVE identities.
namespace
{
struct GpuavRig
{
    VkDevice              device   = VK_NULL_HANDLE;
    VkShaderModule        module   = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl      = VK_NULL_HANDLE;
    VkPipelineLayout      layout   = VK_NULL_HANDLE;
    VkPipeline            pipeline = VK_NULL_HANDLE;
    VkDescriptorPool      dpool    = VK_NULL_HANDLE;
    VkBuffer              out      = VK_NULL_HANDLE;
    VkBuffer              idx      = VK_NULL_HANDLE;
    VkDeviceMemory        out_mem  = VK_NULL_HANDLE;
    VkDeviceMemory        idx_mem  = VK_NULL_HANDLE;
    VkCommandPool         cpool    = VK_NULL_HANDLE;
    VkFence               fence    = VK_NULL_HANDLE;

    ~GpuavRig()
    {
        if (device == VK_NULL_HANDLE)
        {
            return;
        }
        (void)vkDeviceWaitIdle(device);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, cpool, nullptr);
        vkDestroyDescriptorPool(device, dpool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
        vkDestroyBuffer(device, out, nullptr);
        vkDestroyBuffer(device, idx, nullptr);
        vkFreeMemory(device, out_mem, nullptr);
        vkFreeMemory(device, idx_mem, nullptr);
    }
};

[[nodiscard]] VkBuffer make_storage_buffer(VkDevice device, VkDeviceSize size)
{
    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = size;
    bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    REQUIRE(vkCreateBuffer(device, &bci, nullptr, &buffer) == VK_SUCCESS);
    return buffer;
}

[[nodiscard]] VkDeviceMemory bind_memory(gpu::VulkanGpuContext& vk, VkBuffer buffer, VkDeviceSize at_least,
                                         VkMemoryPropertyFlags props)
{
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vk.vk_device(), buffer, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size > at_least ? req.size : at_least;
    mai.memoryTypeIndex = find_memory_type(vk.vk_physical_device(), req.memoryTypeBits, props);
    REQUIRE(mai.memoryTypeIndex != UINT32_MAX);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(vk.vk_device(), &mai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(vk.vk_device(), buffer, memory, 0) == VK_SUCCESS);
    return memory;
}
} // namespace

TEST_CASE("DIAG.7a(g-3): GPU-assisted validation flags a shader OOB access correlated to a Cerid identity",
          "[gpu-context][vulkan][gpu][validation][identity][hazard][gpuav]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend                        = gpu::GpuBackend::Vulkan;
    cfg.headless                       = true;
    cfg.enable_validation              = true; // Core (also names the objects)
    cfg.enable_gpu_assisted_validation = true; // GpuAssisted
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    if (!vk->validation_activation().is_active(gpu::ValidationMode::GpuAssisted))
    {
        WARN("GPU-assisted validation not active on this device; skipping"); // env capability, not a masked failure
        return;
    }
    CHECK(vk->validation_layer().gpu_assisted_guarded); // GPU-AV never runs unguarded

    // A tiny kernel that writes OUT OF BOUNDS via a data-driven index (so the compiler cannot fold the access away).
    static const char* const kSrc =
        "#version 450\n"
        "layout(local_size_x = 1) in;\n"
        "layout(set=0, binding=0) writeonly buffer OutBuf { uint data[]; } outb;\n"
        "layout(set=0, binding=1) readonly  buffer IdxBuf { uint idx[]; } idxb;\n"
        "void main() { outb.data[idxb.idx[0]] = 0xABCDu; }\n";
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::StringView(kSrc), "gpuav_oob",
                                                crd::memory::default_allocator());
    REQUIRE(spv.ok);

    constexpr VkDeviceSize out_bytes   = 16U;          // the descriptor range: 4 uints
    constexpr VkDeviceSize out_alloc   = 8U << 20U;    // the memory behind it: 8 MiB
    constexpr crd::u32     oob_index   = 1000000U;     // byte 4,000,000: past the descriptor, inside the allocation
    static_assert(static_cast<VkDeviceSize>(oob_index) * 4U + 4U <= out_alloc);

    GpuavRig r{};
    r.device = vk->vk_device();
    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.spirv.size();
    smci.pCode    = reinterpret_cast<const crd::u32*>(spv.spirv.data());
    REQUIRE(vkCreateShaderModule(r.device, &smci, nullptr, &r.module) == VK_SUCCESS);
    VkDescriptorSetLayoutBinding binds[2]{};
    for (crd::u32 b = 0; b < 2U; ++b)
    {
        binds[b].binding         = b;
        binds[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[b].descriptorCount = 1U;
        binds[b].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2U;
    dslci.pBindings    = binds;
    REQUIRE(vkCreateDescriptorSetLayout(r.device, &dslci, nullptr, &r.dsl) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1U;
    plci.pSetLayouts    = &r.dsl;
    REQUIRE(vkCreatePipelineLayout(r.device, &plci, nullptr, &r.layout) == VK_SUCCESS);
    VkComputePipelineCreateInfo cpci{};
    cpci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = r.module;
    cpci.stage.pName  = "main";
    cpci.layout       = r.layout;
    REQUIRE(vkCreateComputePipelines(r.device, VK_NULL_HANDLE, 1U, &cpci, nullptr, &r.pipeline) == VK_SUCCESS);
    const gpu::ObjectIdentity program = gpu::detail::vk_attach_identity(
        r.device, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<crd::u64>(r.pipeline), gpu::ObjectKind::Program, "vk-gpuav-pipe");

    r.out     = make_storage_buffer(r.device, out_bytes);
    r.out_mem = bind_memory(*vk, r.out, out_alloc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const gpu::ObjectIdentity resource = gpu::detail::vk_attach_identity(
        r.device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<crd::u64>(r.out), gpu::ObjectKind::Resource, "vk-gpuav-out");
    r.idx     = make_storage_buffer(r.device, 4U);
    r.idx_mem = bind_memory(*vk, r.idx, 4U, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    REQUIRE(vkMapMemory(r.device, r.idx_mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS);
    *static_cast<crd::u32*>(mapped) = oob_index;
    vkUnmapMemory(r.device, r.idx_mem);

    VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2U};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 1U;
    dpci.poolSizeCount = 1U;
    dpci.pPoolSizes    = &psize;
    REQUIRE(vkCreateDescriptorPool(r.device, &dpci, nullptr, &r.dpool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = r.dpool;
    dsai.descriptorSetCount = 1U;
    dsai.pSetLayouts        = &r.dsl;
    VkDescriptorSet set     = VK_NULL_HANDLE;
    REQUIRE(vkAllocateDescriptorSets(r.device, &dsai, &set) == VK_SUCCESS);
    const VkDescriptorBufferInfo infos[2] = {{r.out, 0, out_bytes}, {r.idx, 0, 4U}};
    VkWriteDescriptorSet writes[2]{};
    for (crd::u32 b = 0; b < 2U; ++b)
    {
        writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet          = set;
        writes[b].dstBinding      = b;
        writes[b].descriptorCount = 1U;
        writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo     = &infos[b];
    }
    vkUpdateDescriptorSets(r.device, 2U, writes, 0U, nullptr);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = vk->compute_family();
    REQUIRE(vkCreateCommandPool(r.device, &cpi, nullptr, &r.cpool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = r.cpool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1U;
    VkCommandBuffer cb      = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(r.device, &cbai, &cb) == VK_SUCCESS);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    REQUIRE(vkCreateFence(r.device, &fci, nullptr, &r.fence) == VK_SUCCESS);

    gpu::ValidationCapture   capture(*vk);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    REQUIRE(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0U, 1U, &set, 0U, nullptr);
    vkCmdDispatch(cb, 1U, 1U, 1U);
    REQUIRE(vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers    = &cb;
    REQUIRE(gpu::detail::vk_submit(r.device, vk->compute_queue(), si, r.fence, "gpuav submit") == VK_SUCCESS);
    // GPU-AV reads back its error buffer at the fence wait, not at record
    REQUIRE(gpu::detail::vk_wait_complete(r.device, r.fence, gpu::detail::kVkDefaultWaitNs, "gpuav wait") == VK_SUCCESS);

    for (const auto& m : capture.messages())
    {
        UNSCOPED_INFO("gpuav msg ident.valid=" << m.identity.valid()
                      << " kind=" << (m.identity.valid() ? static_cast<int>(m.identity.kind) : -1)
                      << " sev=" << static_cast<int>(m.severity) << " text=" << m.message_text.c_str());
    }
    CHECK(capture.dropped_count() == 0U);            // GPU-AV can emit several records; correlate over a complete set
    REQUIRE(capture.error_or_warning_count() >= 1U); // GPU-AV fired on the OOB access

    const gpu::ValidationMessage* hit = nullptr;     // a GPU-AV record resolves to THIS pipeline or THIS buffer
    for (const auto& m : capture.messages())
    {
        if (m.identity == program || m.identity == resource)
        {
            hit = &m;
            break;
        }
    }
    REQUIRE(hit != nullptr);
    UNSCOPED_INFO("correlated route kind=" << static_cast<int>(hit->identity.kind)); // 0=Resource 1=Program
    CHECK(gpu::identity_registry().alive(hit->identity));
    gpu::detail::vk_detach_identity(program);
    gpu::detail::vk_detach_identity(resource);
}

// DIAG.7a(g-4): the PASS route. A hazard recorded INSIDE a PassLabelScope carries no named object; the capture now resolves
// the Cerid identity from the active command-buffer debug label (format_debug_name of the Pass id). Three legs prove the
// route and its precedence: (a) unnamed buffer + oversized fill inside the scope -> resolves to the Pass id; (b) the same
// fill OUTSIDE any scope -> the Pass id does not resolve (the label is load-bearing -- the teeth, no code neutralization);
// (c) a NAMED buffer inside the scope -> objects win over labels, so it resolves to the buffer, not the pass.
TEST_CASE("DIAG.7a(g-4): a hazard inside a PassLabelScope correlates to the Pass identity; outside does not; objects win",
          "[gpu-context][vulkan][gpu][validation][identity][hazard][pass]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    REQUIRE(gpu::validation_layer_spec_version() != 0U);
    const VkDevice device = vk->vk_device();

    auto begin_fn = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
    auto end_fn = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
    REQUIRE(begin_fn != nullptr); // debug-utils labels present -> the scope is not a silent no-op
    REQUIRE(end_fn != nullptr);

    const gpu::ObjectIdentity pass_id = gpu::identity_registry().mint(gpu::ObjectKind::Pass); // the frame graph's own path
    REQUIRE(pass_id.valid());

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    // (a) HAZARD inside the scope, UNNAMED buffer -> the label is the only token -> resolves to the Pass id.
    HazardRig ru;
    REQUIRE(ru.setup(*vk, 16, nullptr)); // unnamed
    REQUIRE_FALSE(ru.id.valid());        // the buffer carries no token
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkBeginCommandBuffer(ru.cb, &bi) == VK_SUCCESS);
        {
            const gpu::detail::PassLabelScope lbl(begin_fn, end_fn, ru.cb, pass_id, "vk-pass-hazard");
            vkCmdFillBuffer(ru.cb, ru.buffer, 0, 64, 0xAAU); // 64 > 16: record-time VUID recorded INSIDE the label
        }
        (void)vkEndCommandBuffer(ru.cb);
        for (const auto& r : capture.messages())
        {
            UNSCOPED_INFO("pass msg ident.valid=" << r.identity.valid()
                          << " kind=" << (r.identity.valid() ? static_cast<int>(r.identity.kind) : -1)
                          << " text=" << r.message_text.c_str());
        }
        CHECK(capture.dropped_count() == 0U);
        const gpu::ValidationMessage* hit = find_by_identity(capture, pass_id);
        REQUIRE(hit != nullptr);                              // the hazard correlates to the Pass identity via the label
        CHECK(hit->identity.kind == gpu::ObjectKind::Pass);
        CHECK(hit->severity == gpu::ValidationSeverity::Error); // the VUID, not a stray warning inside the scope
        CHECK(gpu::identity_registry().alive(pass_id));
    }

    // (b) TEETH: the same fill OUTSIDE any scope -> no active label -> the Pass id does not resolve.
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkResetCommandBuffer(ru.cb, 0) == VK_SUCCESS);
        REQUIRE(vkBeginCommandBuffer(ru.cb, &bi) == VK_SUCCESS);
        vkCmdFillBuffer(ru.cb, ru.buffer, 0, 64, 0xAAU);
        (void)vkEndCommandBuffer(ru.cb);
        CHECK(capture.error_count() >= 1U);                   // the hazard still fired
        CHECK(find_by_identity(capture, pass_id) == nullptr); // but with no label the pass id is absent (label load-bearing)
    }
    ru.teardown();

    // (c) PRECEDENCE: a NAMED buffer inside the SAME scope -> objects win over labels -> resolves to the buffer, not the pass.
    HazardRig rn;
    REQUIRE(rn.setup(*vk, 16, "vk-pass-precedence")); // named Resource
    REQUIRE(rn.id.valid());
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkBeginCommandBuffer(rn.cb, &bi) == VK_SUCCESS);
        {
            const gpu::detail::PassLabelScope lbl(begin_fn, end_fn, rn.cb, pass_id, "vk-pass-precedence-scope");
            vkCmdFillBuffer(rn.cb, rn.buffer, 0, 64, 0xAAU);
        }
        (void)vkEndCommandBuffer(rn.cb);
        const gpu::ValidationMessage* hit = find_by_identity(capture, rn.id);
        REQUIRE(hit != nullptr);                              // resolves to the BUFFER (objects precede labels)
        CHECK(hit->identity.kind == gpu::ObjectKind::Resource);
        CHECK(find_by_identity(capture, pass_id) == nullptr); // the enclosing pass id did NOT win
    }
    rn.teardown();

    // (d) NESTED scopes: the INNERMOST pass wins (proves the backwards walk is a tested contract, not a claim -- a forward
    // walk would resolve the outer id). Also confirms VVL delivers the whole nested label stack in pCmdBufLabels.
    HazardRig rd;
    REQUIRE(rd.setup(*vk, 16, nullptr)); // unnamed
    const gpu::ObjectIdentity outer_id = gpu::identity_registry().mint(gpu::ObjectKind::Pass);
    const gpu::ObjectIdentity inner_id = gpu::identity_registry().mint(gpu::ObjectKind::Pass);
    REQUIRE(outer_id.valid());
    REQUIRE(inner_id.valid());
    {
        gpu::ValidationCapture capture(*vk);
        REQUIRE(vkBeginCommandBuffer(rd.cb, &bi) == VK_SUCCESS);
        {
            const gpu::detail::PassLabelScope outer(begin_fn, end_fn, rd.cb, outer_id, "vk-pass-outer");
            {
                const gpu::detail::PassLabelScope inner(begin_fn, end_fn, rd.cb, inner_id, "vk-pass-inner");
                vkCmdFillBuffer(rd.cb, rd.buffer, 0, 64, 0xAAU); // hazard inside the INNER scope
            }
        }
        (void)vkEndCommandBuffer(rd.cb);
        for (const auto& r : capture.messages())
        {
            UNSCOPED_INFO("nested msg ident.valid=" << r.identity.valid()
                          << " kind=" << (r.identity.valid() ? static_cast<int>(r.identity.kind) : -1));
        }
        CHECK(find_by_identity(capture, inner_id) != nullptr); // the innermost active pass wins
        CHECK(find_by_identity(capture, outer_id) == nullptr); // the enclosing pass does not
    }
    rd.teardown();
    (void)gpu::identity_registry().retire(outer_id);
    (void)gpu::identity_registry().retire(inner_id);

    (void)gpu::identity_registry().retire(pass_id);
}

// DIAG.7a lifetime class (user decision 2026-10-05: built, not waived). A Cerid-named buffer is recorded into a SECONDARY
// command buffer and then DESTROYED; a primary then references that secondary with vkCmdExecuteCommands, which the layer
// reports at RECORD time as a command buffer invalidated by a destroyed bound object. Measured: the record's object list
// carries the destroyed VkBuffer WITH its Cerid debug name (the layer keeps names of destroyed handles), so the
// production capture resolves it to the now-RETIRED identity -- no Cerid-side handle table is needed. Never submitted:
// a first version submitted the invalidated buffer and lavapipe, a CPU device, segfaulted dereferencing the destroyed
// buffer object (keeping only its memory alive was not enough). The control leg runs the same flow without the destroy
// and stays clean.
namespace
{
struct LifetimeLeg
{
    gpu::ObjectIdentity id{};
    bool                correlated_error = false; // an Error record carries `id`
    bool                retired          = false; // identity_registry().alive(id) == false at correlation time
    crd::u32            errors_or_warnings = 0;
    crd::u32            dropped          = 0;
};

[[nodiscard]] LifetimeLeg run_lifetime_leg(gpu::VulkanGpuContext& vk, bool destroy_before_use)
{
    LifetimeLeg            leg{};
    const VkDevice         device = vk.vk_device();
    VkBufferCreateInfo     bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = 16;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    REQUIRE(vkCreateBuffer(device, &bci, nullptr, &buffer) == VK_SUCCESS);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = find_memory_type(vk.vk_physical_device(), req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    REQUIRE(mai.memoryTypeIndex != UINT32_MAX);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &mai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    leg.id = gpu::detail::vk_attach_identity(device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<crd::u64>(buffer),
                                             gpu::ObjectKind::Resource, "vk-lifetime-buffer");
    REQUIRE(leg.id.valid());

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = vk.compute_family();
    VkCommandPool pool   = VK_NULL_HANDLE;
    REQUIRE(vkCreateCommandPool(device, &pci, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer secondary = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(device, &cbai, &secondary) == VK_SUCCESS);
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer primary = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(device, &cbai, &primary) == VK_SUCCESS);
    VkCommandBufferInheritanceInfo inherit{};
    inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    VkCommandBufferBeginInfo sbi{};
    sbi.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    sbi.pInheritanceInfo = &inherit;
    REQUIRE(vkBeginCommandBuffer(secondary, &sbi) == VK_SUCCESS);
    vkCmdFillBuffer(secondary, buffer, 0, 16, 0U);
    REQUIRE(vkEndCommandBuffer(secondary) == VK_SUCCESS);

    {
        gpu::ValidationCapture capture(vk);
        if (destroy_before_use)
        {
            gpu::detail::vk_detach_identity(leg.id); // the production retire-then-destroy order
            vkDestroyBuffer(device, buffer, nullptr); // the hazard
            buffer = VK_NULL_HANDLE;
        }
        // Referencing the (now invalid) secondary is reported at RECORD time; the primary is never submitted, so no
        // device -- a CPU one included -- ever executes a command that names the destroyed buffer.
        VkCommandBufferBeginInfo pbi{};
        pbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        REQUIRE(vkBeginCommandBuffer(primary, &pbi) == VK_SUCCESS);
        vkCmdExecuteCommands(primary, 1U, &secondary);
        (void)vkEndCommandBuffer(primary);

        for (const auto& rec : capture.messages())
        {
            UNSCOPED_INFO("msg id=" << rec.message_id_number << " sev=" << static_cast<int>(rec.severity)
                          << " ident.valid=" << rec.identity.valid() << " text=" << rec.message_text.c_str());
            if (rec.identity == leg.id && rec.severity == gpu::ValidationSeverity::Error)
            {
                leg.correlated_error = true;
            }
        }
        leg.retired            = !gpu::identity_registry().alive(leg.id);
        leg.errors_or_warnings = capture.error_or_warning_count();
        leg.dropped            = capture.dropped_count();
    }

    vkDestroyCommandPool(device, pool, nullptr);
    if (buffer != VK_NULL_HANDLE)
    {
        gpu::detail::vk_detach_identity(leg.id);
        vkDestroyBuffer(device, buffer, nullptr);
    }
    vkFreeMemory(device, memory, nullptr);
    return leg;
}
} // namespace

TEST_CASE("DIAG.7a lifetime: a destroyed Cerid buffer in a submitted command buffer correlates to its retired identity",
          "[gpu-context][vulkan][gpu][validation][identity][hazard][lifetime]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true; // Core
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr || !ctx->valid())
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->validation_activation().is_active(gpu::ValidationMode::Core))
    {
        WARN("the validation layer is not present here; skipping");
        return;
    }

    const LifetimeLeg hazard = run_lifetime_leg(*vk, true);
    CHECK(hazard.dropped == 0U);
    CHECK(hazard.correlated_error); // an ERROR names the destroyed buffer's identity...
    CHECK(hazard.retired);          // ...and that identity is retired: the provenance of a dead object

    const LifetimeLeg control = run_lifetime_leg(*vk, false);
    CHECK(control.dropped == 0U);
    CHECK(control.errors_or_warnings == 0U); // the same flow without the destroy is clean
    CHECK_FALSE(control.correlated_error);
}
