#pragma once

// RAF-9 — the file-reading asset resolver (scene-render internal).
//
// render-asset-core stays a PURE leaf module: `on_disk_relative(AssetRef)` (folder+name+extension) is I/O-free. The
// mount table + the `platform::fs` read live HERE, in the layer that already owns `platform::fs` and the asset root.
// `engine://` mounts the asset root, so a resolved file is the EXACT file the pre-RAF-9 relative path loaded — output
// is unchanged by construction. `crd://` folds to `engine://` in `AssetRef::parse`, so a still-`crd://` live frame and
// its `engine://` rewrite resolve to the same bytes and hash to the same AssetId.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderasset/renderasset.hpp> // AssetRef · AssetScheme · on_disk_relative · DiagnosticList · DiagCode

namespace crd::gpu
{
class IRasterProgram; // fwd — the registry maps ids to these without pulling gpu-context into this header
class IGpuProgram;
} // namespace crd::gpu

namespace crd::scenerender
{
class AssetResolver
{
public:
    explicit AssetResolver(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_engine(alloc), m_app(alloc), m_plugin(alloc), m_test(alloc)
    {
    }

    void set_mount(crd::renderasset::AssetScheme scheme, crd::containers::StringView root)
    {
        crd::containers::String& m = mount(scheme);
        m.clear();
        m.append(root.data(), root.size());
    }
    [[nodiscard]] bool has_mount(crd::renderasset::AssetScheme scheme) { return mount(scheme).size() > 0U; }

    // Read a root-relative path WITH extension ("frame/x.frame.toml") from a scheme's mount root (Engine by default).
    // ⭐ THE SINGLE disk read path — lifted verbatim from the old `Impl::asset_text` body, so behaviour is identical.
    [[nodiscard]] bool read_relative(crd::containers::StringView rel, crd::containers::String& out,
                                     crd::renderasset::AssetScheme scheme = crd::renderasset::AssetScheme::Engine)
    {
        const crd::containers::String& root = mount(scheme);
        if (root.size() == 0U)
        {
            return false;
        }
        crd::containers::String p(m_alloc);
        p.append(root.c_str(), root.size());
        p.push_back('/');
        p.append(rel.data(), rel.size());
        const crd::platform::fs::Path path(crd::containers::StringView(p.c_str(), p.size()));
        if (!crd::platform::fs::exists(path))
        {
            return false;
        }
        return crd::platform::fs::read_file_text(path, out);
    }

    // Resolve a canonical ref (engine://frame/x — crd:// folds to engine://) to its bytes: `on_disk_relative` +
    // `read_relative` under the ref's scheme mount. ⛔ A miss emits a NAMED diagnostic (Gate 9: a missing default
    // reports a clear error, never a silent fallback).
    [[nodiscard]] bool read_ref(const crd::renderasset::AssetRef& ref, crd::containers::String& out,
                                crd::renderasset::DiagnosticList& diags)
    {
        if (!ref.valid())
        {
            diags.error(crd::renderasset::DiagCode::MalformedPath, "asset reference is invalid");
            return false;
        }
        crd::containers::String rel(m_alloc);
        if (!crd::renderasset::on_disk_relative(ref, rel))
        {
            diags.error(crd::renderasset::DiagCode::AssetNotFound, "asset id has no on-disk folder shape",
                        ref.canonical());
            return false;
        }
        if (!read_relative(crd::containers::StringView(rel.c_str(), rel.size()), out, ref.scheme()))
        {
            diags.error(crd::renderasset::DiagCode::AssetNotFound, "asset not found on disk under its mount root",
                        ref.canonical());
            return false;
        }
        return true;
    }

private:
    crd::containers::String& mount(crd::renderasset::AssetScheme s) noexcept
    {
        switch (s)
        {
        case crd::renderasset::AssetScheme::Engine:
            return m_engine;
        case crd::renderasset::AssetScheme::App:
            return m_app;
        case crd::renderasset::AssetScheme::Plugin:
            return m_plugin;
        case crd::renderasset::AssetScheme::Test:
            return m_test;
        }
        return m_engine;
    }

    crd::memory::IAllocator* m_alloc;
    crd::containers::String  m_engine;
    crd::containers::String  m_app;
    crd::containers::String  m_plugin;
    crd::containers::String  m_test;
};

// RAF-9 — the public program registry: AssetId -> provider. A provider is the engine's `FramePassFn` idiom (a
// function pointer + `void* user`, NO std::function). It replaces the hard-coded `str_is(id, "crd://scene/…")` chain in
// SceneHost::program()/kernel(): the incoming id is parsed (crd:// folds to engine://) to an AssetId and LOOKED UP.
// Engine defaults register captureless thunks over the `ensure_*` builders; an app registers its own the same way (the
// seam RAF-10 uses). Lookups run over a small insertion-ordered array (deterministic; ~25 ids, once-per-pass), so no
// hashing/ordering machinery is needed.
class ProgramRegistry
{
public:
    using RasterFn = crd::gpu::IRasterProgram* (*)(void* user);
    using KernelFn = crd::gpu::IGpuProgram* (*)(void* user);

    explicit ProgramRegistry(crd::memory::IAllocator* alloc) noexcept : m_raster(alloc), m_kernel(alloc) {}

    void register_raster(crd::renderasset::AssetId id, RasterFn fn, void* user)
    {
        m_raster.push_back(RasterEntry{id, fn, user});
    }
    void register_kernel(crd::renderasset::AssetId id, KernelFn fn, void* user)
    {
        m_kernel.push_back(KernelEntry{id, fn, user});
    }

    // Resolve a (parsed, folded) AssetId to a device program by invoking its provider. nullptr if unregistered —
    // SceneHost turns that into a clear "no program for id" error (the executor already reports it by name).
    [[nodiscard]] crd::gpu::IRasterProgram* raster(crd::renderasset::AssetId id) const
    {
        for (crd::usize i = 0; i < m_raster.size(); ++i)
        {
            if (m_raster[i].id == id) { return m_raster[i].fn(m_raster[i].user); }
        }
        return nullptr;
    }
    [[nodiscard]] crd::gpu::IGpuProgram* kernel(crd::renderasset::AssetId id) const
    {
        for (crd::usize i = 0; i < m_kernel.size(); ++i)
        {
            if (m_kernel[i].id == id) { return m_kernel[i].fn(m_kernel[i].user); }
        }
        return nullptr;
    }

    [[nodiscard]] crd::usize raster_count() const noexcept { return m_raster.size(); }
    [[nodiscard]] crd::usize kernel_count() const noexcept { return m_kernel.size(); }

private:
    struct RasterEntry
    {
        crd::renderasset::AssetId id;
        RasterFn                  fn;
        void*                     user;
    };
    struct KernelEntry
    {
        crd::renderasset::AssetId id;
        KernelFn                  fn;
        void*                     user;
    };
    crd::containers::Array<RasterEntry> m_raster;
    crd::containers::Array<KernelEntry> m_kernel;
};
} // namespace crd::scenerender
