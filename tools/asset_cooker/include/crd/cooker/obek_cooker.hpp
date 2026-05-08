#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/cooker/scene_cooker.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/serialize.hpp>

namespace crd::cooker
{
// Phase 3.0 v1m3 — `.obek.toml` → OBEK cooker (ADR-0058).
//
// Parses an öbek TOML file into an in-memory World, then emits an OBEK
// CRDR blob via `crd::scene::ObekArtifactBuilder`. Riding on the v1l
// SceneCooker's built-in TOML reader registry — the same Transform +
// six built-in relation readers handle both formats. The öbek-specific
// behaviours layer on top:
//
//   v1m3a (this slice) — flat öbek substrate: parse + cook a single-file
//                        öbek with components + built-in relations. No
//                        extends, no nested öbek refs, no overrides.
//
//   v1m3b — `extends = "..."` chain resolution at cook time, cycle
//           detection, deepest-wins variant resolution per field, OCHN
//           chunk emission.
//
//   v1m3c — `obek = "..."` nested öbek references, recursive cook of
//           dependencies, eager flatten into parent's entity table,
//           sub-instance source_obek_root tracking, OCHN nested entries.
//
//   v1m3d — `overrides = [...]` cook-time override patches baked into
//           OOVR chunks at the cooker layer.
//
// Multi-error accumulation matches v1l (advisor pin #3): every cook
// problem accumulates into `Array<CookError>` before the cooker fails.

// Caller-provided file-content resolver for öbek references.
// Returns true on success and assigns the file's TOML text into
// `out_text` (allocated via `alloc`). Returns false on miss; the cooker
// emits a "path not found" error.
//
// Tests pass an in-memory map; production cookers pass a filesystem
// reader. v1m3b uses this for `extends = "..."` chain walks; v1m3c
// reuses it for nested `obek = "..."` references.
using ObekFileResolverFn = bool (*)(crd::containers::StringView path,
                                    crd::memory::IAllocator*    alloc,
                                    crd::containers::String&    out_text,
                                    void*                       user_data);

// Cook context for öbek cooks. Mirrors SceneCookContext but tagged for
// öbek so callers can't accidentally pass an öbek context to a scene
// cooker (or vice versa).
struct ObekCookContext
{
    crd::resources::ResourceId  id;
    crd::memory::IAllocator*    allocator    = nullptr;

    // Stable 64-bit content identity for this öbek source (typically
    // FNV-1a 64 of canonical path + content version). Stored in OINF
    // and combined with file_idx to produce ObekEntityGuid.
    crd::u64                    obek_root_id = 0U;

    // Optional caller-provided file resolver for `extends = "..."`
    // (v1m3b) and `obek = "..."` (v1m3c). When null, both keys are
    // rejected at cook time with a "no resolver" diagnostic. Tests
    // typically wire an in-memory map; production cookers wire a
    // filesystem reader.
    ObekFileResolverFn          file_resolver         = nullptr;
    void*                       file_resolver_ud      = nullptr;
};

// ObekCooker — opaque facade. Holds the component-reader registry plus
// the öbek-specific pass schedule (extends/nested/overrides). Library
// users register additional component readers via
// `register_component_reader<T>(name, reader, fourcc, version)` before
// invoking `cook_inline`.
//
// The reader registry shape MATCHES SceneCooker — built-ins
// (Transform + the six relations) auto-register on
// `register_builtin_readers()`, and user types register the same way.
// This means a content team can author the SAME components in either a
// `.scene.toml` or `.obek.toml` without re-wiring the cooker.
class ObekCooker
{
public:
    explicit ObekCooker(crd::memory::IAllocator* alloc);
    ~ObekCooker();

    ObekCooker(const ObekCooker&)            = delete;
    ObekCooker& operator=(const ObekCooker&) = delete;
    ObekCooker(ObekCooker&&)                 = delete;
    ObekCooker& operator=(ObekCooker&&)      = delete;

    void register_reader(crd::containers::StringView    key,
                         ComponentTomlReaderFn          reader,
                         crd::scene::ComponentSerialize serialize_trait,
                         bool                           is_relation = false);

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
    // Mirrors SceneCooker::register_builtin_readers exactly — content
    // authored in either format uses the same reader code.
    void register_builtin_readers();

    // Public cook entry point. Tests invoke it directly with TOML text;
    // an asset_cooker file-handler (future) wraps it with file I/O.
    // Returns the cooked OBEK CRDR bytes on success; empty array +
    // populated `errors_out` on failure.
    [[nodiscard]] crd::containers::Array<crd::u8> cook_inline(
        crd::containers::StringView        toml_text,
        const ObekCookContext&             ctx,
        crd::containers::Array<CookError>* errors_out);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// Convenience top-level — constructs a fresh cooker with built-in
// readers and runs cook_inline once. Typical test pattern; mirrors
// scene_cooker_inline.
[[nodiscard]] crd::containers::Array<crd::u8> obek_cooker_inline(
    crd::containers::StringView        toml_text,
    const ObekCookContext&             ctx,
    crd::containers::Array<CookError>* errors_out = nullptr);

} // namespace crd::cooker
