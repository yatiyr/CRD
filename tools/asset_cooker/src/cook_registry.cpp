#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

namespace crd::cooker
{

namespace
{

struct HandlerEntry
{
    crd::containers::String ext;
    CookHandlerFn           fn;
    crd::u32                version;
};

crd::containers::Array<HandlerEntry>& registry()
{
    static crd::memory::GrowableTlsfAllocator s_alloc;
    static crd::containers::Array<HandlerEntry> s_registry(&s_alloc);
    return s_registry;
}

} // anonymous namespace

void register_cook_handler(crd::containers::StringView ext, CookHandlerFn fn, crd::u32 version)
{
    auto& reg = registry();
    for (const HandlerEntry& entry : reg)
    {
        if (entry.ext == ext)
        {
            return; // idempotent: already registered (first-wins — the GEO-3 retirement mechanism)
        }
    }
    reg.push_back({ crd::containers::String(ext.data(), ext.size()), fn, version });
}

CookHandlerFn find_cook_handler(crd::containers::StringView ext) noexcept
{
    const auto& reg = registry();
    for (const HandlerEntry& entry : reg)
    {
        if (entry.ext == ext)
        {
            return entry.fn;
        }
    }
    return nullptr;
}

crd::u32 find_cook_handler_version(crd::containers::StringView ext) noexcept
{
    const auto& reg = registry();
    for (const HandlerEntry& entry : reg)
    {
        if (entry.ext == ext)
        {
            return entry.version;
        }
    }
    return 0U;
}

} // namespace crd::cooker
