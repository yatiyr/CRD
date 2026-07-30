#pragma once

// ---------------------------------------------------------------------------
// crd-lod — REN-40-C1: THE `.crdlod` POLICY, AS AN AUTHORED ASSET.
//
// ⛔⛔ WHY THIS IS A FILE AND NOT A CONSTANT. An LOD policy is the single most
// content-dependent thing in the renderer: a rock can drop to 5% of its triangles
// at 100 px and nobody will ever see it, a character's face cannot. Baking ratios
// and switch distances into C++ makes every one of those decisions an engine edit
// by someone who is not looking at the asset. It also makes them invisible: the
// only way to find out why a mesh pops is to read the renderer.
//
// So the policy is a file an artist owns, resolved by NAME through the same
// disk-first asset path as every other authored vocabulary, and the cook is a
// consumer of it.
//
// ⭐ AN IDENTITY HASH SHIPS WITH IT. Two meshes cooked under different policies
// are different content, and a cache keyed on the mesh alone would serve one
// mesh's chain for the other's policy — silently, with the wrong switch
// distances baked in.
//
//   schema  = 1
//   name    = "crd://lod/scene_default"
//   boundary_weight = 1000.0     # Garland 1998 silhouette preservation
//
//   [[level]]                    # level 1 (level 0 is always the source mesh)
//   ratio         = 0.5          # of the SOURCE triangle count, never of the previous level
//   screen_height = 512.0        # use this level while the projected height is BELOW this
//
//   [[level]]
//   ratio         = 0.25
//   screen_height = 128.0
// ---------------------------------------------------------------------------

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/lod/lod_chain.hpp>

namespace crd::lod
{

enum class LodCookError : crd::u8
{
    Ok = 0,
    ParseFailed,
    BadSchema,
    NoLevels,
    RatioOutOfRange,     // a ratio outside (0, 1) — a level that is not a REDUCTION buys nothing
    RatiosNotDescending, // level i+1 must be coarser than level i, or selection is undefined
    ThresholdsNotDescending,
    TooManyLevels,
};

[[nodiscard]] const char* lod_cook_error_text(LodCookError err) noexcept;

// Parse a `.crdlod` into a policy. `where` (optional) receives the offending key
// or level index so a rejection names its cause rather than just failing.
[[nodiscard]] LodCookError parse_lod_toml(crd::containers::StringView text, LodPolicy& out,
                                          crd::containers::String* where = nullptr);

// The canonical form — the round-trip the cook's identity rests on: parse →
// write → parse must be a fixed point, which is what makes the hash a hash of the
// MEANING rather than of the author's whitespace.
void write_lod_toml(const LodPolicy& policy, crd::containers::String& out);

// ⛔ EVERY FIELD THAT CHANGES THE COOKED CHAIN. A policy field left out here
// collides two different chains onto one cache entry, and the wrong one ships.
[[nodiscard]] crd::u64 lod_policy_identity(const LodPolicy& policy) noexcept;

} // namespace crd::lod
