#include <crd/cooker/cook_handler.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

namespace crd::cooker
{

namespace
{

struct HandlerEntry
{
    crd::containers::String ext;
    CookHandlerFn           fn;
};

crd::containers::Array<HandlerEntry>& registry()
{
    static crd::memory::MallocAllocator s_alloc;
    static crd::containers::Array<HandlerEntry> s_registry(&s_alloc);
    return s_registry;
}

} // anonymous namespace

void register_cook_handler(crd::containers::StringView ext, CookHandlerFn fn)
{
    auto& reg = registry();
    for (const HandlerEntry& entry : reg)
    {
        if (entry.ext == ext)
        {
            return; // idempotent: already registered
        }
    }
    reg.push_back({ crd::containers::String(ext.data(), ext.size()), fn });
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

} // namespace crd::cooker
