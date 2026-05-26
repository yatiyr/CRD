#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::resources
{
class ResourceManager;
} // namespace crd::resources

namespace crd::memory
{
class IAllocator;
} // namespace crd::memory

namespace crd::hesap::resources
{
class SparseMatrixResource;

// Parse a 'HMTX' CRDR blob into `out` (header + CSR structure + value bytes).
// `scratch` deserialises the CRDR container; `out`'s own arrays are filled
// from `out`'s allocator. Returns false on a malformed / mistyped blob.
// Shared by the ILoader and unit tests (no ResourceManager needed).
[[nodiscard]] bool read_matrix_resource(crd::containers::ConstSpan<crd::u8> bytes, SparseMatrixResource& out,
                                        crd::memory::IAllocator* scratch);

// Register the 'HMTX' sparse-matrix loader into a ResourceManager.
// Call once at startup, before mount_manifest(). (Phase 3.1.6 v4-corpus.)
void register_hesap_matrix_loader(crd::resources::ResourceManager* rm);

// Anchor symbol: a static library .obj that only registers a loader can be
// dropped by the MSVC linker. Consumers reference this to force-link the TU.
// (See feedback_static_lib_anchor_symbol.)
void hesap_matrix_resource_anchor() noexcept;

// CLI anchor: the cli_register_matrix.cpp TU registers hesap.matrix.* via a
// static-init hook; consumers reference this to force the .obj to link.
void register_hesap_matrix_cli_anchor() noexcept;

} // namespace crd::hesap::resources
