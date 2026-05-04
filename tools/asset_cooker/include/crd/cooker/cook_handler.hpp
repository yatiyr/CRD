#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
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

// One additional artifact produced by a multi-output handler.
// Used by handlers (e.g. glTF) that emit one artifact per named asset inside
// a single source file. The first artifact goes in CookResult::cooked_bytes;
// subsequent ones go here, each with its own stable UUID.
struct ExtraArtifact
{
    crd::resources::ResourceId      id;
    crd::u32                        type_fourcc = 0;
    crd::containers::Array<crd::u8> cooked_bytes;
    crd::containers::String         name; // display name for manifest/logging, e.g. "model.glb#Cube"

    explicit ExtraArtifact(crd::memory::IAllocator* a) : cooked_bytes(a), name(a) {}
    ExtraArtifact(ExtraArtifact&&)            = default;
    ExtraArtifact& operator=(ExtraArtifact&&) = default;
};

struct CookResult
{
    crd::u32                                          type_fourcc     = 0;
    crd::containers::Array<crd::u8>                   cooked_bytes;
    crd::containers::Array<crd::resources::ResourceId> dependencies;
    crd::u64                                          source_hash     = 0;
    crd::u64                                          options_hash    = 0;
    crd::u32                                          handler_version = 0;
    bool                                              ok              = false;

    // Additional artifacts from the same source file (multi-output handlers).
    // Existing single-output handlers leave this empty.
    crd::containers::Array<ExtraArtifact> extra_artifacts;

    explicit CookResult(crd::memory::IAllocator* a = crd::memory::default_allocator())
        : cooked_bytes(a), dependencies(a), extra_artifacts(a)
    {
    }
};

using CookHandlerFn = CookResult (*)(const CookContext&);

void              register_cook_handler(crd::containers::StringView ext, CookHandlerFn fn);
[[nodiscard]] CookHandlerFn find_cook_handler(crd::containers::StringView ext) noexcept;
void              register_builtin_handlers();

} // namespace crd::cooker
