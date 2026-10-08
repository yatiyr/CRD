#pragma once

// sandbox — ENGINE ASSET ROOT: which `assets/` tree the sandbox installs as its engine asset root (the disk tree that
// shadows the embedded pack; `SceneRenderer::set_asset_root`). Without a root the renderer sees only the embedded
// pack, `scene_programs.manifest` is not found and the window falls back to overlay-only.
//
// The rule, in order:
//   1. `CRD_ASSETS_DIR`, when set and non-empty, always wins. It is not probed: an explicit root that the renderer
//      rejects stays a loud REJECTED line, never a silent switch to another tree.
//   2. A developer (non-shipping) build falls back to the source tree it was built from (`<CRD_SOURCE_DIR>/assets`),
//      but only when that directory exists and holds `scene_programs.manifest`. A direct launch (a terminal, a
//      script, a smoke run) then renders the same scene as the Visual Studio debugger and ctest, which set the
//      variable.
//   3. Otherwise there is no root. A shipping build never falls back to a source tree.
//
// The decision is a pure function of its inputs (the probe is the only file-system access), so it is tested without
// a window or a device.

#include <crd/core/types.hpp>

#include <string_view>

namespace crd::sandbox
{
enum class AssetRootSource : u8
{
    None,               // no root: the renderer keeps the embedded pack only
    Environment,        // CRD_ASSETS_DIR
    SourceTree,         // the developer build's own source tree
    SourceTreeUnusable, // no root: the developer build's source tree was probed and is not a usable root
};

struct AssetRootChoice
{
    const char* root = nullptr; // one of the two candidates passed in; nullptr when there is no root
    AssetRootSource source = AssetRootSource::None;
};

// Whether `root` is a usable engine asset tree.
using AssetRootProbe = bool (*)(const char* root);

// The name of the scene programme manifest a usable root must hold.
inline constexpr std::string_view kAssetRootManifest = "scene_programs.manifest";

// Chooses the engine asset root. `env_value` is the CRD_ASSETS_DIR value (nullptr or empty means unset);
// `source_assets_dir` is the build's own `<source>/assets` (nullptr when the build has none); `usable` is called only
// for the source-tree candidate.
[[nodiscard]] AssetRootChoice choose_asset_root(const char* env_value, const char* source_assets_dir, bool shipping,
                                                AssetRootProbe usable) noexcept;

// The real probe: `root` is a directory that holds `scene_programs.manifest`.
[[nodiscard]] bool is_usable_asset_root(const char* root) noexcept;

// Why a root was chosen, for the sandbox's log line.
[[nodiscard]] std::string_view asset_root_source_name(AssetRootSource source) noexcept;
} // namespace crd::sandbox
