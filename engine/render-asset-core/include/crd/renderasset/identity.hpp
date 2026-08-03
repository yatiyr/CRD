#pragma once

// crd-render-asset-core — canonical asset identity + virtual paths (RAF-1, mission §5).
//
// ONE identity model for every render asset. A raw authored reference like
// `engine://frame/forward_csm` is PARSED + NORMALIZED into an `AssetRef` (scheme
// + normalized relative path + inferred type) whose `AssetId` is a stable,
// deterministic 64-bit hash of the CANONICAL string. Because the scheme is part
// of the canonical string, `engine://x` and `app://x` get DIFFERENT ids —
// namespace separation is structural, and implicit engine/app shadowing is not
// even representable (mission Gate 1). `crd://` is a DEPRECATED COMPAT ALIAS that
// normalizes to `engine://` (every shipped default is engine-owned today); it is
// slated for deletion at RAF-12 once all references migrate.
//
// Determinism: FNV-1a over the canonical bytes is byte-deterministic across runs
// / platforms / toolchains, so cooked ids are reproducible. Path case is kept
// verbatim (filesystem-honest on case-sensitive hosts); only the SCHEME token is
// case-folded to lowercase.

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>

#include <compare>

namespace crd::renderasset
{
using crd::containers::String;
using crd::containers::StringView;

// The four canonical mounts. `crd` is an alias-only scheme (folds to Engine).
enum class AssetScheme : u8
{
    Engine, // engine://  — engine-shipped default assets (and the crd:// alias)
    App,    // app://     — application-authored assets
    Plugin, // plugin://  — plugin-provided assets
    Test,   // test://    — test-only fixtures
};

// Render-asset families. Identity does NOT depend on this (the canonical path
// already disambiguates folders); it is routing/diagnostic metadata, inferred
// from the first path segment when not supplied explicitly.
enum class AssetType : u8
{
    Unknown,
    Shader,
    Program,
    Material,
    Technique,
    RenderPhase,
    FrameGraph,
    Vertex,
    Lod,
    Light,
    Mesh,
    Texture,
    Sampler,
};

// "engine://", "app://", "plugin://", "test://".
[[nodiscard]] StringView scheme_prefix(AssetScheme scheme) noexcept;
// "engine" / "app" / "plugin" / "test".
[[nodiscard]] StringView scheme_token(AssetScheme scheme) noexcept;
// Parse a scheme token (case-insensitive). "crd" folds to Engine. Returns false
// on an unknown token (out is left untouched).
[[nodiscard]] bool parse_scheme(StringView token, AssetScheme& out) noexcept;

[[nodiscard]] StringView asset_type_name(AssetType type) noexcept;
// Best-effort family from a first path segment ("frame"->FrameGraph, ...).
[[nodiscard]] AssetType infer_type(StringView first_segment) noexcept;

// Stable, deterministic 64-bit identity. `value == 0` is the reserved invalid id.
struct AssetId
{
    u64 value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(const AssetId&, const AssetId&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const AssetId&, const AssetId&) noexcept = default;
};

// Hash a pre-normalized canonical string ("engine://frame/x") to an AssetId.
// Exposed so a cooker can compute an id without materializing an AssetRef.
[[nodiscard]] AssetId asset_id_of(StringView canonical) noexcept;

// A parsed + normalized virtual path. Owns its canonical string.
class AssetRef
{
public:
    // An empty ref (valid() == false). Needs an allocator for its owned string.
    explicit AssetRef(memory::IAllocator* alloc) noexcept : m_canonical(alloc) {}

    // Parse + normalize `raw`. On any structural problem emits a diagnostic into
    // `diags` (MalformedPath / UnknownScheme / EmptyPath / PathEscapesRoot) and
    // returns an invalid ref. `type_hint` overrides folder inference when set.
    [[nodiscard]] static AssetRef parse(StringView raw, DiagnosticList& diags, memory::IAllocator* alloc,
                                        AssetType type_hint = AssetType::Unknown);

    [[nodiscard]] bool valid() const noexcept { return m_valid; }
    [[nodiscard]] AssetScheme scheme() const noexcept { return m_scheme; }
    [[nodiscard]] AssetType type() const noexcept { return m_type; }
    [[nodiscard]] AssetId id() const noexcept { return m_id; }

    // Full canonical "scheme://path".
    [[nodiscard]] StringView canonical() const noexcept { return StringView{m_canonical.data(), m_canonical.size()}; }
    // Just the normalized relative path (no scheme).
    [[nodiscard]] StringView path() const noexcept
    {
        const usize sz = m_canonical.size();
        return StringView{m_canonical.data() + (m_path_offset < sz ? m_path_offset : sz),
                          m_path_offset < sz ? sz - m_path_offset : 0};
    }

    [[nodiscard]] memory::IAllocator* allocator() const noexcept { return m_canonical.allocator(); }

private:
    String m_canonical;      // "engine://frame/forward_csm" (owned)
    usize m_path_offset = 0; // index of the first relative-path byte in m_canonical
    AssetScheme m_scheme = AssetScheme::Engine;
    AssetType m_type = AssetType::Unknown;
    AssetId m_id{};
    bool m_valid = false;
};
} // namespace crd::renderasset
