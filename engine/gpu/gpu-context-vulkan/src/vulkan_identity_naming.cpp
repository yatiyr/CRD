// DIAG.7a(d2b-vk): the single out-of-line definition of the Vulkan identity-naming adapter. A thin wrapper over the
// shared registry + format_debug_name (both device-free, in gpu-context) and vkSetDebugUtilsObjectNameEXT. See the
// header for the no-read-back note (Vulkan names are write-only; the wire is proven by live_count + the string).

#include "vulkan_identity_naming.hpp"

#include <crd/gpu/identity_registry.hpp>

#include <vulkan/vulkan.h>

namespace crd::gpu::detail
{

void vk_name_object(VkDevice device, VkObjectType type, crd::u64 handle, const ObjectIdentity& id,
                    std::string_view site) noexcept
{
    // A null handle or VK_OBJECT_TYPE_UNKNOWN is rejected by some ICDs/layers -- never call the PFN with either.
    if (device == VK_NULL_HANDLE || handle == 0U || type == VK_OBJECT_TYPE_UNKNOWN) { return; }
    const auto fn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
    if (fn == nullptr) { return; } // VK_EXT_debug_utils not provided by any layer/ICD -- best-effort, non-fatal
    char             name[kDebugNamePrefixChars + 96];
    const crd::usize n = format_debug_name(id, site, name, sizeof(name)); // NUL-terminated; 0 if invalid/too small
    if (n == 0U) { return; }
    VkDebugUtilsObjectNameInfoEXT ni{};
    ni.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ni.objectType   = type;
    ni.objectHandle = handle;
    ni.pObjectName  = static_cast<const char*>(name);
    (void)fn(device, &ni); // best-effort; a failed debug name must not fail creation
}

ObjectIdentity vk_attach_identity(VkDevice device, VkObjectType type, crd::u64 handle, ObjectKind kind,
                                  std::string_view site) noexcept
{
    // Mint BEFORE naming: the identity (and its retire-on-destroy lifetime) must hold even when the debug-utils PFN is
    // absent and vk_name_object no-ops. One mint, then name the primary native object with it.
    const ObjectIdentity id = identity_registry().mint(kind);
    vk_name_object(device, type, handle, id, site);
    return id;
}

void vk_detach_identity(const ObjectIdentity& id) noexcept
{
    (void)identity_registry().retire(id);
}

PassLabelScope::PassLabelScope(PFN_vkCmdBeginDebugUtilsLabelEXT begin_fn, PFN_vkCmdEndDebugUtilsLabelEXT end_fn,
                               VkCommandBuffer cb, const ObjectIdentity& id, const char* name) noexcept
{
    if (begin_fn == nullptr || end_fn == nullptr || cb == VK_NULL_HANDLE) { return; } // best-effort no-op
    const std::string_view nm  = (name != nullptr) ? std::string_view(name) : std::string_view("pass");
    char                   buf[kDebugNamePrefixChars + 96];
    const crd::usize       n = format_debug_name(id, nm, buf, sizeof(buf)); // "[pass:N] <name>"; 0 if invalid/too small
    if (n == 0U) { return; }                                               // do NOT Begin -> dtor stays a no-op (balanced)
    VkDebugUtilsLabelEXT label{};
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = static_cast<const char*>(buf);
    begin_fn(cb, &label);
    m_end_fn = end_fn; // Begin ran -> arm End
    m_cb     = cb;
}

PassLabelScope::~PassLabelScope() noexcept
{
    if (m_end_fn != nullptr) { m_end_fn(m_cb); }
}

} // namespace crd::gpu::detail
