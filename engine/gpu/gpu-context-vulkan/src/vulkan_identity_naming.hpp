#pragma once

// DIAG.7a(d2b-vk): attach a stable Cerid identity to a native Vulkan object as its debug name (via
// vkSetDebugUtilsObjectNameEXT) and mint/retire it in the process-wide identity registry. The Vulkan mirror of
// dx12_identity_naming -- same shared machinery (crd::gpu::identity_registry() + format_debug_name), a thin
// backend adapter over vkSetDebugUtilsObjectNameEXT. Src-private to gpu-context-vulkan.
//
// The debug name is the bracketed form format_debug_name() builds, e.g. "[crd:res:0000002a:g00000007] vk-storage".
// A validation message about the object then resolves back to the identity via the DIAG.7a(d1) capture parse. Unlike
// DX12 there is NO read-back (vkSetDebugUtilsObjectNameEXT is write-only), so the wire is proven by the identity
// registry's live_count deltas plus the format_debug_name string contract, not a GetPrivateData round-trip.

#include <crd/gpu/object_identity.hpp>

#include <vulkan/vulkan.h>

#include <string_view>

namespace crd::gpu::detail
{

// Mint an identity of `kind`, set `handle`'s debug name (a `type`-typed Vulkan object on `device`) to
// "[<encoded id>] <site>", and return the identity. The mint happens BEFORE the debug-utils null guard, so
// retire-on-destroy lifetime tracking holds even when VK_EXT_debug_utils is absent (the name is best-effort and never
// fails creation). Equivalent to mint(kind) + vk_name_object(...).
[[nodiscard]] ObjectIdentity vk_attach_identity(VkDevice device, VkObjectType type, crd::u64 handle, ObjectKind kind,
                                                std::string_view site) noexcept;

// Name a native object with an EXISTING identity -- no mint. A logical resource that owns several native objects (a
// storage buffer's device + readback buffers; a raster target's colour + resolve + depth) gets ONE identity, minted
// once on its primary via vk_attach_identity, then stamped onto every sibling here so a message about ANY of them
// resolves to the one logical identity ("one identity per logical resource, not per native object"). No-op on a null
// handle, VK_OBJECT_TYPE_UNKNOWN, or absent debug-utils; a failed name is non-fatal.
void vk_name_object(VkDevice device, VkObjectType type, crd::u64 handle, const ObjectIdentity& id,
                    std::string_view site) noexcept;

// Retire an identity when its logical resource is destroyed. The generation bump means a later message about the freed
// object parses to alive()==false -- the "recently retired provenance" primitive lifecycle coverage (e) builds on.
void vk_detach_identity(const ObjectIdentity& id) noexcept;

// DIAG.7a(d2c-vk): a balanced-by-construction command-buffer debug label around a frame-graph pass. The two PFNs
// (vkCmdBegin/EndDebugUtilsLabelEXT -- VK_EXT_debug_utils, the same extension as object naming) are loaded ONCE by the
// caller and passed in; any of {begin_fn, end_fn, cb} absent -> the scope is a NO-OP. The label string reuses
// format_debug_name, so a capture shows "[pass:N] <name>" beside the "[res:M] <site>" object names. RAII: End fires iff
// Begin ran, so a `continue`/return inside the labelled block can never unbalance the label. A null `name` -> "pass".
class PassLabelScope
{
public:
    PassLabelScope(PFN_vkCmdBeginDebugUtilsLabelEXT begin_fn, PFN_vkCmdEndDebugUtilsLabelEXT end_fn,
                   VkCommandBuffer cb, const ObjectIdentity& id, const char* name) noexcept;
    ~PassLabelScope() noexcept;
    PassLabelScope(const PassLabelScope&)            = delete;
    PassLabelScope& operator=(const PassLabelScope&) = delete;
    PassLabelScope(PassLabelScope&&)                 = delete;
    PassLabelScope& operator=(PassLabelScope&&)      = delete;

private:
    PFN_vkCmdEndDebugUtilsLabelEXT m_end_fn = nullptr; // set only if Begin ran -> the dtor is balanced
    VkCommandBuffer                m_cb     = VK_NULL_HANDLE;
};

} // namespace crd::gpu::detail
