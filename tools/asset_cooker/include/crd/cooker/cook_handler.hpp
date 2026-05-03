#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::cooker
{

struct CookContext
{
    crd::containers::StringView source_path;
    crd::containers::StringView meta_path;
    crd::resources::ResourceId  id;
    crd::memory::IAllocator*    allocator = nullptr;
};

struct CookResult
{
    crd::u32                                   type_fourcc     = 0;
    crd::containers::Array<crd::u8>            cooked_bytes;
    crd::containers::Array<crd::resources::ResourceId> dependencies;
    crd::u64                                   source_hash     = 0;
    crd::u64                                   options_hash    = 0;
    crd::u32                                   handler_version = 0;
    bool                                       ok              = false;

    explicit CookResult(crd::memory::IAllocator* a = crd::memory::default_allocator())
        : cooked_bytes(a), dependencies(a)
    {
    }
};

using CookHandlerFn = CookResult (*)(const CookContext&);

void              register_cook_handler(crd::containers::StringView ext, CookHandlerFn fn);
[[nodiscard]] CookHandlerFn find_cook_handler(crd::containers::StringView ext) noexcept;
void              register_builtin_handlers();

} // namespace crd::cooker
