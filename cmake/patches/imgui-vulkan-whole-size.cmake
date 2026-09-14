# imgui-vulkan-whole-size — applied by crd_patched_copy() to imgui/backends/imgui_impl_vulkan.cpp (1.92.0-docking).
#
# ImGui's Vulkan texture upload flushes `upload_size` bytes, the raw unaligned size, which trips the validation
# layer's VUID-VkMappedMemoryRange-size-01390 (a flush size must be a multiple of nonCoherentAtomSize or
# VK_WHOLE_SIZE). The upload buffer is allocated single-use for that texture, so mapping and flushing the whole
# allocation is always safe; the mapped bytes past `upload_size` are never written. Two replacements, both anchored
# on the upstream text; an anchor that stops matching fails the configure (see docs/design/pinned-inputs.md).
crd_patch_replace(
    [[range[0].size = upload_size;]]
    [[range[0].size = VK_WHOLE_SIZE; // crd-patch v1-close: nonCoherentAtomSize alignment]])
crd_patch_replace(
    [[vkMapMemory(v->Device, upload_buffer_memory, 0, upload_size, 0, (void**)(&map))]]
    [[vkMapMemory(v->Device, upload_buffer_memory, 0, VK_WHOLE_SIZE, 0, (void**)(&map)) /* crd-patch v1-close */]])
