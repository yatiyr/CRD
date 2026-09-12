#pragma once

// technique_asset.hpp — REN-37.2 (D-007 row 140): THE LIGHTING TECHNIQUE AS AN AUTHORED ASSET.
//
// A `.crdt` declares a technique's CONTRACT — what surface it consumes, what pass-frequency inputs it requires,
// what options it varies over, and where its body comes from — and cooks to a canonical `.crdk` blob. It is the
// shader-half twin of `.frame.toml`: the frame graph says WHICH passes exist, the technique says WHAT THE
// FRAGMENT COMPUTES, and neither one needs a scripting language (ADR-0081).
//
//   schema = 1
//   name    = "forward_csm"
//   surface = "OpenPBRSurface"
//   body    = "builtin:forward_csm"            # or "crd://technique/forward_csm.ckir" (a serialized CKIR graph)
//
//   [[binding]] name = "shadow_atlas"  type = "texture2DArrayShadow"  frequency = "pass"
//   [[binding]] name = "csm_splits"    type = "float[]"  count = 4    frequency = "pass"
//   [[binding]] name = "csm_light_vp"  type = "mat4[]"   count = 4    frequency = "pass"
//   [[option]]  name = "pcf_taps"      min = 1  max = 16  default = 8
//
// ⛔ THE BINDING CONTRACT IS THE POINT (REN-37.3). The technique declares its pass-frequency inputs BY NAME AND
// TYPE; the frame-graph pass declares what it `reads`; `verify_technique_bindings` checks they match. A mismatch
// is a COOK-TIME REJECTION WITH A NAME — never a black screen or a validation error on a user's machine. It is
// also what removes hand-picked descriptor slots: the frequency, not the author, decides the set.
//
// ⛔ WHY THIS IS NOT IN `crd-frame-cook`. The frame asset deliberately depends on nothing but the API-neutral GPU
// enums, which is what makes "the asset can never name a backend concept" structural. A technique's types are IR
// types, so this module owns the `crd-kir` edge instead: technique-cook -> {kir, frame-cook}; nothing depends
// back on it.

#include <crd/kir/ckir_technique.hpp>

#include <crd/framecook/frame_asset.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::techniquecook
{

inline constexpr crd::u32 kTechniqueSchemaVersion = 1U;

using crd::kir::technique::BindFrequency;
using crd::kir::technique::BindType;

// One declared input. `count` is the array length for `float[]` / `mat4[]` and 1 otherwise.
struct TechniqueBindingDesc
{
    crd::containers::String name;
    BindType                type  = BindType::Float;
    BindFrequency           freq  = BindFrequency::Pass;
    crd::u32                count = 1U;

    explicit TechniqueBindingDesc(crd::memory::IAllocator* a) : name(a) {}
};

// One declared VARIANT AXIS. Bounded on purpose — REN-37.7 enumerates the matrix from what is DECLARED, never
// from what it discovers, and an unbounded option is an unbounded matrix.
struct TechniqueOptionDesc
{
    crd::containers::String name;
    int                     min_value     = 0;
    int                     max_value     = 0;
    int                     default_value = 0;

    explicit TechniqueOptionDesc(crd::memory::IAllocator* a) : name(a) {}
};

// Where the technique's shading graph comes from.
enum class TechniqueBodyKind : crd::u8
{
    Builtin = 0, // `builtin:<name>` — a registered `TechniqueBody` in the engine or a plugin
    Graph,       // `crd://...` — a serialized CKIR graph, SPLICED at cook time. NO ENGINE CODE.
};

struct TechniqueDesc
{
    crd::containers::String                        name;
    crd::u32                                       schema  = kTechniqueSchemaVersion;
    crd::containers::String                        surface;   // the surface contract id ("OpenPBRSurface")
    TechniqueBodyKind                              body_kind = TechniqueBodyKind::Builtin;
    crd::containers::String                        body;      // the id after the `builtin:` / `crd://` prefix
    crd::containers::Array<TechniqueBindingDesc>   bindings;
    crd::containers::Array<TechniqueOptionDesc>    options;

    explicit TechniqueDesc(crd::memory::IAllocator* a)
        : name(a), surface(a), body(a), bindings(a), options(a)
    {
    }
};

// Every way a `.crdt` — or a pass that uses one — can be REJECTED, by name, at COOK time.
enum class TechniqueCookError : crd::u8
{
    Ok = 0,
    ParseFailed,          // not valid TOML
    BadSchema,            // missing/unsupported `schema`
    MissingName,
    MissingBody,          // no `body`, or a prefix that is neither `builtin:` nor `crd://`
    UnknownSurface,       // `surface` names a contract this engine does not define
    UnknownBindType,      // `type` is not in the closed set
    UnknownFrequency,     // `frequency` is not frame/pass/material/object
    DuplicateBinding,     // two bindings share a name
    DuplicateOption,      // two options share a name
    BadArrayCount,        // `float[]`/`mat4[]` with count 0, or a count on a non-array type
    BadOptionRange,       // min > max, or a default outside [min, max]
    // ── REN-37.3: the BINDING CONTRACT, checked against a frame-graph pass ──
    PassMissingBinding,   // the technique requires a pass-frequency input the pass never `reads`
    PassBindingNotLayered,// a `texture2DArrayShadow` binding wired to a resource with layers == 1
    PassUnknownTechnique, // a pass names a technique nothing defines
};

[[nodiscard]] const char* technique_cook_error_text(TechniqueCookError err) noexcept;

// Parse + VALIDATE a `.crdt` into `out`. `where` receives the offending name on failure (may be null).
[[nodiscard]] TechniqueCookError parse_technique_toml(crd::containers::StringView toml_text, TechniqueDesc& out,
                                                      crd::containers::String* where = nullptr);

// VALIDATE any description, whatever its provenance — parsed, read back from a blob, or built in memory by a node
// editor / a test / an agent. Same rejections either way (the frame cooker's rule, for the same reason).
[[nodiscard]] TechniqueCookError validate_technique(const TechniqueDesc& desc,
                                                    crd::containers::String* where = nullptr);

// Emit a description back to `.crdt` — the EDITOR ROUND-TRIP. Lossless by gate: `parse -> emit -> parse -> cook`
// is byte-identical to `parse -> cook`.
[[nodiscard]] crd::containers::String emit_technique_toml(const TechniqueDesc& desc, crd::memory::IAllocator* a);

// Serialize to the cooked blob — canonical, packed, padding-free, endian-defined (the `ckir_serialize` scar).
[[nodiscard]] crd::containers::Array<crd::u8> cook_technique(const TechniqueDesc& desc, crd::memory::IAllocator* a);

// Read a cooked blob back. False on bad magic / version / truncation — never a partial description.
[[nodiscard]] bool read_technique(crd::containers::ConstSpan<crd::u8> bytes, TechniqueDesc& out);

// ── REN-37.3: THE BINDING CONTRACT ───────────────────────────────────────────────────────────────────────────
// Check that `pass` (in `graph`) supplies every PASS-FREQUENCY input `tech` declares. Frame/material/object
// frequencies are the engine's to provide and are not the graph's business; pass frequency is exactly the set
// the graph author is responsible for, which is why it is the only frequency checked here.
//
// The check is by NAME and by SHAPE: a `texture2DArrayShadow` binding must resolve to a declared resource with
// `layers > 1`, because a single-layer atlas sampled as an array renders every cascade from slice 0 — the exact
// degenerate failure the REN-3.2 gate was designed to catch, moved from runtime to cook time.
[[nodiscard]] TechniqueCookError verify_technique_bindings(const TechniqueDesc& tech,
                                                           const crd::framecook::FrameGraphDesc& graph,
                                                           const crd::framecook::FramePassDesc&  pass,
                                                           crd::containers::String* where = nullptr);

// Build the runtime `Technique` record from a validated description plus its resolved body. `blob`/`blob_size`
// carry a serialized CKIR graph for `TechniqueBodyKind::Graph`; `body` is the registered builder for `Builtin`.
// The returned record BORROWS `desc`'s strings, so it must not outlive the description (the borrowed-lifetime
// rule this repo has paid for twice).
[[nodiscard]] crd::kir::technique::Technique
make_runtime_technique(const TechniqueDesc& desc, crd::containers::Array<crd::kir::technique::TechniqueBinding>& scratch_bindings,
                       crd::containers::Array<crd::kir::technique::TechniqueOption>& scratch_options,
                       crd::kir::technique::TechniqueBody body, void* user, const crd::u8* blob, crd::u64 blob_size);

} // namespace crd::techniquecook
