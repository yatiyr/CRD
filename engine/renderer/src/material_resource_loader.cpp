#include <crd/renderer/material_resource_loader.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>

namespace crd::renderer
{
namespace
{

constexpr crd::u32 kMaterialLoaderVersion = 2U;

// ── INFO chunk ─────────────────────────────────────────────────────────────
// 4 bytes: loader_version u8, domain u8, flags u8, pad u8

constexpr crd::usize kInfoChunkSize = 4U;

// ── PASS chunk ─────────────────────────────────────────────────────────────
// count u32, then per-entry (36 bytes):
//   pass_type u8, pad u8[3], vert_id u8[16], frag_id u8[16]

constexpr crd::usize kPassEntrySize = 36U;

// ── PSOS chunk ─────────────────────────────────────────────────────────────
// present_mask u32 (4 bytes), then 3 x RasterState (3 x 8 = 24 bytes)

constexpr crd::usize kPsosChunkSize = 4U + static_cast<crd::usize>(kPassTypeCount) * sizeof(RasterState);

// ── META legacy chunk ──────────────────────────────────────────────────────
// 32 bytes: vert_id hi+lo (16), frag_id hi+lo (16)

constexpr crd::usize kMetaChunkSize = 32U;

// ── Helpers ────────────────────────────────────────────────────────────────

// Read a ResourceId (hi + lo, each u64 LE) from `src`.
crd::resources::ResourceId read_resource_id(const crd::u8* src) noexcept
{
    crd::resources::ResourceId id;
    std::memcpy(&id.hi, src,     8);
    std::memcpy(&id.lo, src + 8, 8);
    return id;
}

// Load a vert+frag shader pair synchronously; returns false if either fails.
bool load_shader_pair(const crd::resources::LoadContext& ctx,
                             crd::resources::ResourceId vert_id,
                             crd::resources::ResourceId frag_id,
                             PassShaderPair&            out)
{
    out.vert = ctx.manager->load_sync<crd::shader::ShaderResource>(vert_id);
    out.frag = ctx.manager->load_sync<crd::shader::ShaderResource>(frag_id);
    return out.vert.is_ready() && out.frag.is_ready();
}

// ── Loader ─────────────────────────────────────────────────────────────────

class MaterialResourceLoaderImpl final : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc()    const noexcept override
    {
        return crd::resources::kFourCC_MATR;
    }

    [[nodiscard]] crd::u32 loader_version() const noexcept override
    {
        return kMaterialLoaderVersion;
    }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override
    {
        crd::resources::CrdrFile file(&m_alloc);
        if (crd::resources::crdr_read(ctx.bytes, file, &m_alloc) != crd::resources::CrdrError::Ok)
        {
            return nullptr;
        }

        // Allocate and construct MaterialTemplate.
        void* raw = m_alloc.allocate(sizeof(MaterialTemplate), alignof(MaterialTemplate));
        auto* tmpl = new (raw) MaterialTemplate(&m_alloc);

        bool ok = false;

        // ── INFO chunk ──────────────────────────────────────────────────────
        const auto* info_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_INFO);
        if (info_chunk != nullptr && info_chunk->payload.size() >= kInfoChunkSize)
        {
            const crd::u8* p = info_chunk->payload.data();
            // p[0] = loader_version (informational)
            tmpl->domain = static_cast<MaterialDomain>(p[1]);
            // p[2] = flags (reserved)
        }

        // ── PRMS chunk ──────────────────────────────────────────────────────
        const auto* prms_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_PRMS);
        if (prms_chunk != nullptr && prms_chunk->payload.size() >= sizeof(crd::u32))
        {
            crd::u32 count = 0;
            std::memcpy(&count, prms_chunk->payload.data(), sizeof(crd::u32));
            const crd::usize expected = sizeof(crd::u32) + count * sizeof(CookedParameter);
            if (prms_chunk->payload.size() >= expected)
            {
                const crd::u8* entry_ptr = prms_chunk->payload.data() + sizeof(crd::u32);
                tmpl->parameters.resize(count);
                std::memcpy(tmpl->parameters.data(), entry_ptr, count * sizeof(CookedParameter));
            }
        }

        // ── DFLT chunk ──────────────────────────────────────────────────────
        const auto* dflt_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_DFLT);
        if (dflt_chunk != nullptr && !dflt_chunk->payload.empty())
        {
            tmpl->defaults_blob.resize(dflt_chunk->payload.size());
            std::memcpy(tmpl->defaults_blob.data(),
                        dflt_chunk->payload.data(),
                        dflt_chunk->payload.size());
        }

        // ── PASS chunk ──────────────────────────────────────────────────────
        const auto* pass_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_PASS);
        if (pass_chunk != nullptr && pass_chunk->payload.size() >= sizeof(crd::u32))
        {
            crd::u32 count = 0;
            std::memcpy(&count, pass_chunk->payload.data(), sizeof(crd::u32));
            const crd::usize required = sizeof(crd::u32) + count * kPassEntrySize;
            if (pass_chunk->payload.size() >= required)
            {
                const crd::u8* entry_ptr = pass_chunk->payload.data() + sizeof(crd::u32);
                for (crd::u32 i = 0; i < count; ++i)
                {
                    const crd::u8* e       = entry_ptr + i * kPassEntrySize;
                    const auto     pt      = static_cast<crd::u8>(e[0]);
                    const auto     vert_id = read_resource_id(e + 4U);
                    const auto     frag_id = read_resource_id(e + 4U + 16U);

                    if (pt >= kPassTypeCount || vert_id.is_null() || frag_id.is_null())
                    {
                        continue;
                    }

                    if (!load_shader_pair(ctx, vert_id, frag_id, tmpl->pass_shaders[pt]))
                    {
                        tmpl->~MaterialTemplate();
                        m_alloc.deallocate(tmpl);
                        return nullptr;
                    }
                    ok = true;
                }
            }
        }
        else
        {
            // ── Legacy META backward-compat ─────────────────────────────────
            const auto* meta_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_META);
            if (meta_chunk != nullptr && meta_chunk->payload.size() >= kMetaChunkSize)
            {
                const crd::u8* p       = meta_chunk->payload.data();
                const auto     vert_id = read_resource_id(p);
                const auto     frag_id = read_resource_id(p + 16U);

                if (!vert_id.is_null() && !frag_id.is_null())
                {
                    const auto fwd = static_cast<crd::u8>(PassType::Forward);
                    if (!load_shader_pair(ctx, vert_id, frag_id, tmpl->pass_shaders[fwd]))
                    {
                        tmpl->~MaterialTemplate();
                        m_alloc.deallocate(tmpl);
                        return nullptr;
                    }
                    ok = true;
                }
            }
        }

        // ── PSOS chunk ──────────────────────────────────────────────────────
        const auto* psos_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_PSOS);
        if (psos_chunk != nullptr && psos_chunk->payload.size() >= kPsosChunkSize)
        {
            const crd::u8* p = psos_chunk->payload.data();
            // p[0..3] = present_mask u32 (informational; we always read 3 entries)
            const crd::u8* states = p + sizeof(crd::u32);
            std::memcpy(tmpl->pso_states, states, kPassTypeCount * sizeof(RasterState));
        }

        // ── OPTS chunk ──────────────────────────────────────────────────────
        const auto* opts_chunk = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_OPTS);
        if (opts_chunk != nullptr && opts_chunk->payload.size() >= sizeof(crd::u32))
        {
            crd::u32 count = 0;
            std::memcpy(&count, opts_chunk->payload.data(), sizeof(crd::u32));
            const crd::usize expected = sizeof(crd::u32) + count * sizeof(ShaderOptionDecl);
            if (opts_chunk->payload.size() >= expected)
            {
                const crd::u8* entry_ptr = opts_chunk->payload.data() + sizeof(crd::u32);
                tmpl->options.resize(count);
                std::memcpy(tmpl->options.data(), entry_ptr, count * sizeof(ShaderOptionDecl));
            }
        }

        if (!ok)
        {
            // Neither PASS nor META chunk provided any valid shader pair.
            tmpl->~MaterialTemplate();
            m_alloc.deallocate(tmpl);
            return nullptr;
        }

        return tmpl;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* tmpl = static_cast<MaterialTemplate*>(payload);
        tmpl->~MaterialTemplate();
        m_alloc.deallocate(tmpl);
    }

private:
    // Concurrent async loads share this single loader instance; wrapper serializes the
    // single-threaded TLSF heap while keeping per-type pool locality (see texture loader).
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};
};

} // anonymous namespace

void register_material_loader(crd::resources::ResourceManager* rm)
{
    rm->register_loader(std::make_unique<MaterialResourceLoaderImpl>());
}

} // namespace crd::renderer
