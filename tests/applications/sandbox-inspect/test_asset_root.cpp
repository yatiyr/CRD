// The sandbox's engine asset root (sandbox/src/asset_root.cpp, compiled into this test the way the inspect panel is):
// which `assets/` tree crd-sandbox installs before it cooks its scene programmes. Covered: a set CRD_ASSETS_DIR wins
// over a usable source tree and is never probed, in developer and shipping builds; with the variable unset a developer
// build falls back to its source tree when the probe accepts it; an unusable source tree gives no root and says so;
// a shipping build never falls back and never probes; an empty variable counts as unset. The real probe accepts the
// repository's assets/ tree and a scratch tree holding scene_programs.manifest, and refuses a scratch tree without it,
// a missing directory and a file. Scratch trees live under the OS temp directory, never a relative path.
// ASCII test names.

#include "asset_root.hpp"

#include <crd/containers/string.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <string_view>

using crd::sandbox::asset_root_source_name;
using crd::sandbox::AssetRootChoice;
using crd::sandbox::AssetRootSource;
using crd::sandbox::choose_asset_root;
using crd::sandbox::is_usable_asset_root;

namespace fs = crd::platform::fs;

namespace
{
constexpr const char* kSourceAssets = "D:/src/cerid/assets";
constexpr const char* kEnvAssets = "E:/elsewhere/assets";

// The fake probes record each call, so a test can tell the source tree was (or was not) consulted.
crd::u32 g_probe_calls = 0;
const char* g_probed_root = nullptr;

bool probe_usable(const char* root)
{
    ++g_probe_calls;
    g_probed_root = root;
    return true;
}

bool probe_unusable(const char* root)
{
    ++g_probe_calls;
    g_probed_root = root;
    return false;
}

void reset_probe()
{
    g_probe_calls = 0;
    g_probed_root = nullptr;
}

// A scratch tree under the OS temp directory, removed before and after use.
struct ScratchTree
{
    fs::Path dir;

    explicit ScratchTree(std::string_view name) : dir(fs::temp_directory() / name)
    {
        (void)fs::remove_all(dir);
        REQUIRE(fs::create_directories(dir));
    }
    ~ScratchTree() { (void)fs::remove_all(dir); }
    ScratchTree(const ScratchTree&) = delete;
    ScratchTree& operator=(const ScratchTree&) = delete;
    ScratchTree(ScratchTree&&) = delete;
    ScratchTree& operator=(ScratchTree&&) = delete;

    [[nodiscard]] crd::containers::String root() const
    {
        const std::string_view g = dir.generic();
        return crd::containers::String(g);
    }
};
} // namespace

TEST_CASE("asset root: a set CRD_ASSETS_DIR wins and is not probed", "[sandbox][asset-root]")
{
    for (const bool shipping : {false, true})
    {
        reset_probe();
        const AssetRootChoice c = choose_asset_root(kEnvAssets, kSourceAssets, shipping, &probe_usable);
        CHECK(c.source == AssetRootSource::Environment);
        CHECK(c.root == kEnvAssets);
        CHECK(g_probe_calls == 0U);
    }
    // An explicit root wins even when the source tree would not be usable.
    reset_probe();
    const AssetRootChoice c = choose_asset_root(kEnvAssets, kSourceAssets, false, &probe_unusable);
    CHECK(c.source == AssetRootSource::Environment);
    CHECK(c.root == kEnvAssets);
    CHECK(g_probe_calls == 0U);
    CHECK(asset_root_source_name(c.source) == "CRD_ASSETS_DIR");
}

TEST_CASE("asset root: unset falls back to a usable source tree", "[sandbox][asset-root]")
{
    reset_probe();
    const AssetRootChoice c = choose_asset_root(nullptr, kSourceAssets, false, &probe_usable);
    CHECK(c.source == AssetRootSource::SourceTree);
    CHECK(c.root == kSourceAssets);
    CHECK(g_probe_calls == 1U);
    CHECK(g_probed_root == kSourceAssets);
    CHECK(asset_root_source_name(c.source) == "the build's source tree (CRD_ASSETS_DIR unset)");
}

TEST_CASE("asset root: an unusable source tree gives no root", "[sandbox][asset-root]")
{
    reset_probe();
    const AssetRootChoice c = choose_asset_root(nullptr, kSourceAssets, false, &probe_unusable);
    CHECK(c.source == AssetRootSource::SourceTreeUnusable);
    CHECK(c.root == nullptr);
    CHECK(g_probe_calls == 1U);
    CHECK(g_probed_root == kSourceAssets);

    // No source tree at all (or no probe) is no root, without a probe call.
    reset_probe();
    CHECK(choose_asset_root(nullptr, nullptr, false, &probe_usable).root == nullptr);
    CHECK(choose_asset_root(nullptr, "", false, &probe_usable).root == nullptr);
    CHECK(choose_asset_root(nullptr, kSourceAssets, false, nullptr).root == nullptr);
    CHECK(g_probe_calls == 0U);
}

TEST_CASE("asset root: a shipping build never falls back to the source tree", "[sandbox][asset-root]")
{
    reset_probe();
    const AssetRootChoice c = choose_asset_root(nullptr, kSourceAssets, true, &probe_usable);
    CHECK(c.source == AssetRootSource::None);
    CHECK(c.root == nullptr);
    CHECK(g_probe_calls == 0U);
    CHECK(asset_root_source_name(c.source) == "none");
}

TEST_CASE("asset root: an empty CRD_ASSETS_DIR counts as unset", "[sandbox][asset-root]")
{
    reset_probe();
    const AssetRootChoice dev = choose_asset_root("", kSourceAssets, false, &probe_usable);
    CHECK(dev.source == AssetRootSource::SourceTree);
    CHECK(dev.root == kSourceAssets);
    CHECK(g_probe_calls == 1U);

    reset_probe();
    const AssetRootChoice ship = choose_asset_root("", kSourceAssets, true, &probe_usable);
    CHECK(ship.source == AssetRootSource::None);
    CHECK(ship.root == nullptr);
    CHECK(g_probe_calls == 0U);
}

TEST_CASE("asset root: the real probe needs a directory holding scene_programs.manifest", "[sandbox][asset-root]")
{
    // The repository's own assets/ tree (the source-tree fallback of every developer build).
    CHECK(is_usable_asset_root(CRD_SANDBOX_INSPECT_ENGINE_ASSETS));

    const ScratchTree tree("crd-sandbox-asset-root-test");
    const crd::containers::String root = tree.root();
    // An existing directory without the manifest is not a root.
    CHECK_FALSE(is_usable_asset_root(root.c_str()));
    // The manifest makes it one.
    REQUIRE(fs::write_file_text(tree.dir / "scene_programs.manifest", "# scratch\n"));
    CHECK(is_usable_asset_root(root.c_str()));
    // The path must be the directory, not the manifest file itself.
    const fs::Path manifest = tree.dir / "scene_programs.manifest";
    const std::string_view manifest_text = manifest.generic();
    const crd::containers::String manifest_path(manifest_text);
    CHECK_FALSE(is_usable_asset_root(manifest_path.c_str()));
    // A missing directory and an empty or null path are not roots.
    const fs::Path missing = tree.dir / "missing";
    const std::string_view missing_text = missing.generic();
    const crd::containers::String missing_path(missing_text);
    CHECK_FALSE(is_usable_asset_root(missing_path.c_str()));
    CHECK_FALSE(is_usable_asset_root(""));
    CHECK_FALSE(is_usable_asset_root(nullptr));

    // End to end: with the variable unset, a developer build takes the scratch tree only once it is usable.
    const AssetRootChoice c = choose_asset_root(nullptr, root.c_str(), false, &is_usable_asset_root);
    CHECK(c.source == AssetRootSource::SourceTree);
    CHECK(c.root == root.c_str());
    REQUIRE(fs::remove_file(tree.dir / "scene_programs.manifest"));
    CHECK(choose_asset_root(nullptr, root.c_str(), false, &is_usable_asset_root).source ==
          AssetRootSource::SourceTreeUnusable);
}
