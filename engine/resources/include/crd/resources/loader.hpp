#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

class ResourceManager; // forward-declared; full type available in resource_manager.hpp

// Context passed to ILoader::load() and ILoader::load_placeholder().
struct LoadContext
{
    ResourceId                           id;
    crd::containers::ConstSpan<crd::u8>  bytes;     // entire cooked artifact (raw CRDR bytes)
    ResourceManager*                     manager;   // for transitive load_sync of dependencies
    crd::memory::IAllocator*             allocator; // payload allocator (per-loader policy)
};

// Type-erased resource loader. One implementation per FourCC type.
// Registered into ResourceManager at startup via register_loader().
class ILoader
{
public:
    virtual ~ILoader() = default;

    // FourCC type tag this loader handles (e.g. kFourCC_SHDR, kFourCC_MATR).
    // One loader per FourCC; duplicates are rejected by register_loader().
    [[nodiscard]] virtual crd::u32 type_fourcc() const noexcept = 0;

    // Monotonic version; participates in the cooker's incremental key.
    [[nodiscard]] virtual crd::u32 loader_version() const noexcept = 0;

    // Deserialise the cooked artifact and return an allocator-owned payload.
    // Implementations may call manager->load_sync<Dep>(dep_id) for transitive deps.
    // Must not return nullptr on success.
    [[nodiscard]] virtual void* load(const LoadContext& ctx) = 0;

    // Optional soft-failure fallback (magenta checker, error material, …).
    // Default returns nullptr → LoadState::Failed.
    // Override to return a valid payload → LoadState::Placeholder.
    // Simulation-critical loaders should leave this as default (hard fail).
    [[nodiscard]] virtual void* load_placeholder(const LoadContext& /*ctx*/) { return nullptr; }

    // Release a payload previously returned by load() or load_placeholder().
    virtual void unload(void* payload) noexcept = 0;

    ILoader(const ILoader&)             = delete;
    ILoader& operator=(const ILoader&)  = delete;
    ILoader(ILoader&&)                  = delete;
    ILoader& operator=(ILoader&&)       = delete;

protected:
    ILoader() noexcept = default;
};

} // namespace crd::resources
