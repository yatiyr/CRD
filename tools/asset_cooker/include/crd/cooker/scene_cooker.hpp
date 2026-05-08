#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/serialize.hpp>

namespace crd::scene
{
class World;
}

namespace crd::cooker
{
// Phase 3.0 v1l — `.scene.toml` → SCEN cooker (ADR-0055).
//
// Parses a TOML scene file into an in-memory World, then emits a SCEN
// CRDR blob via `crd::scene::SceneArtifactBuilder`. The cooker is
// content-driven: each persistable component type registers a TOML
// reader function that knows how to deserialise its TOML representation
// into bytes. Built-in readers ship for Transform + the six built-in
// relations.
//
// Cross-domain (advisor pin #4 + #8 + #9):
//   - User-defined components register their own TOML readers via
//     `register_component_reader<T>(reader)` before cooking.
//   - Relation arity: arrays accepted for non-acyclic relations only.
//     Acyclic relations (ChildOf / AttachedTo / Owns / DependsOn) reject
//     `key = ["a", "b"]` at cook time with a diagnostic.
//   - Determinism: TOML walked in document order; component fields
//     within an entity walked in alphabetical key order.

// One cook-time error. Accumulated; cooker emits ALL errors before
// returning failure (advisor pin #3).
struct CookError
{
    crd::containers::String message;
    crd::u32 line = 0;
    crd::u32 column = 0;

    explicit CookError(crd::memory::IAllocator* a = crd::memory::default_allocator()) : message(a) {}
    CookError(CookError&&) = default;
    CookError& operator=(CookError&&) = default;
};

// Cooker context — supplied to scene_cooker_inline. Mirrors CookContext
// but tailored for in-memory cooking (tests don't need source_path).
struct SceneCookContext
{
    crd::resources::ResourceId  id;
    crd::memory::IAllocator*    allocator = nullptr;
};

// Component reader: deserialises a TOML inline-table value (or any
// supported TOML node) into the bytes for one component instance. The
// reader receives a void* writable buffer of `size` bytes (sized per
// the component's registered ComponentInfo.size) and an opaque TOML node
// pointer (toml::node* — declared opaque here so this header doesn't
// pull toml++ into runtime TUs).
//
// Returns true on success; on failure, the implementation pushes
// CookError(s) into the supplied errors array.
using ComponentTomlReaderFn = bool (*)(const void* toml_node,
                                       void*       dst,
                                       crd::usize  size,
                                       crd::u32    line,
                                       crd::containers::Array<CookError>* errors);

// Public scene cooker — opaque facade. Created via make_scene_cooker(),
// holds the component-reader registry and built-in relation handlers.
// Library users register additional readers via register_component_reader
// before invoking scene_cooker_inline.
class SceneCooker
{
public:
    explicit SceneCooker(crd::memory::IAllocator* alloc);
    ~SceneCooker();

    SceneCooker(const SceneCooker&)            = delete;
    SceneCooker& operator=(const SceneCooker&) = delete;
    SceneCooker(SceneCooker&&)                 = delete;
    SceneCooker& operator=(SceneCooker&&)      = delete;

    // Register a TOML reader for component-name `key` (case-sensitive).
    // The cooker matches per-entity TOML keys against the registered
    // names. Built-ins ("Transform" + six relation names) are auto-
    // registered by register_builtin_readers().
    void register_reader(crd::containers::StringView key,
                        ComponentTomlReaderFn       reader,
                        crd::scene::ComponentSerialize serialize_trait,
                        bool is_relation = false);

    // Type-safe convenience: registers a reader for `T` using the given
    // FourCC + reader function. Component must be trivially-copyable
    // (matches default_serialize_trait constraint).
    template <typename T>
    void register_component_reader(crd::containers::StringView key,
                                   ComponentTomlReaderFn       reader,
                                   crd::u32                    fourcc,
                                   crd::u32                    version = 1U)
    {
        crd::scene::ComponentSerialize cs{};
        cs.fourcc  = fourcc;
        cs.version = version;
        register_reader(key, reader, cs, /*is_relation=*/false);
    }

    template <typename Tag>
    void register_relation_reader(crd::containers::StringView key,
                                  ComponentTomlReaderFn       reader,
                                  crd::u32                    fourcc,
                                  crd::u32                    version = 1U)
    {
        crd::scene::ComponentSerialize cs{};
        cs.fourcc  = fourcc;
        cs.version = version;
        register_reader(key, reader, cs, /*is_relation=*/true);
    }

    // Register all built-in readers: Transform + six built-in relations.
    void register_builtin_readers();

    // Public cook entry point. Tests invoke it directly with TOML text;
    // the asset_cooker file-handler (cook_handlers/scene.cpp) wraps it
    // with file I/O. Returns the cooked SCEN CRDR bytes on success;
    // empty array + populated `errors_out` on failure.
    [[nodiscard]] crd::containers::Array<crd::u8> cook_inline(
        crd::containers::StringView      toml_text,
        const SceneCookContext&          ctx,
        crd::containers::Array<CookError>* errors_out);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// Convenience top-level — constructs a fresh cooker with built-in
// readers and runs cook_inline once. Typical test pattern.
[[nodiscard]] crd::containers::Array<crd::u8> scene_cooker_inline(
    crd::containers::StringView toml_text,
    const SceneCookContext&     ctx,
    crd::containers::Array<CookError>* errors_out = nullptr);

// Built-in TOML readers — exported for callers that want to register
// individual readers (rather than `register_builtin_readers()` all-or-
// nothing).
bool read_transform_from_toml(const void* node, void* dst, crd::usize size, crd::u32 line,
                              crd::containers::Array<CookError>* errors);

// Relations are read by an entity-name resolver; the bytes themselves
// are just an EntityId (8 B). The cooker's relation pass handles the
// name → file_idx resolution; individual relation readers aren't
// strictly necessary, but the registration entry (key + fourcc) is
// what the cooker matches against.

} // namespace crd::cooker
