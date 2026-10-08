// sandbox — ENGINE ASSET ROOT (see asset_root.hpp).

#include "asset_root.hpp"

#include <crd/platform/filesystem.hpp>

namespace crd::sandbox
{
AssetRootChoice choose_asset_root(const char* env_value, const char* source_assets_dir, bool shipping,
                                  AssetRootProbe usable) noexcept
{
    if (env_value != nullptr && env_value[0] != 0)
    {
        return AssetRootChoice{env_value, AssetRootSource::Environment};
    }
    if (shipping || source_assets_dir == nullptr || source_assets_dir[0] == 0 || usable == nullptr)
    {
        return AssetRootChoice{};
    }
    if (!usable(source_assets_dir))
    {
        return AssetRootChoice{nullptr, AssetRootSource::SourceTreeUnusable};
    }
    return AssetRootChoice{source_assets_dir, AssetRootSource::SourceTree};
}

bool is_usable_asset_root(const char* root) noexcept
{
    if (root == nullptr || root[0] == 0)
    {
        return false;
    }
    const platform::fs::Path dir(root);
    if (!platform::fs::is_directory(dir))
    {
        return false;
    }
    return platform::fs::is_file(dir / kAssetRootManifest);
}

std::string_view asset_root_source_name(AssetRootSource source) noexcept
{
    switch (source)
    {
        case AssetRootSource::Environment:
            return "CRD_ASSETS_DIR";
        case AssetRootSource::SourceTree:
            return "the build's source tree (CRD_ASSETS_DIR unset)";
        case AssetRootSource::SourceTreeUnusable:
            return "the build's source tree, which is not usable";
        case AssetRootSource::None:
            break;
    }
    return "none";
}
} // namespace crd::sandbox
