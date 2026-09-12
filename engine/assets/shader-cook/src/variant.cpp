// crd-shader-cook — the D3 variant/permutation system (ADR-0104). See variant.hpp for the contract.
#include <crd/shadercook/variant.hpp>

#include <crd/containers/span.hpp>
#include <crd/jobs/jobs.hpp>                          // D10: the fiber scheduler (parallel_for / wait)
#include <crd/kir/ckir_serialize.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>   // D10: a per-job scratch allocator
#include <crd/resources/crdr.hpp>

#include <cstdio>

namespace crd::shadercook
{
namespace
{
constexpr crd::u32 kVartChunk = crd::resources::make_fourcc('V', 'A', 'R', 'T'); // the variant table: key → unique-bundle index
// `VB<nn>` — the nn-th unique variant's bytecode (nn = two decimal digits, up to 100 unique variants per container).
[[nodiscard]] crd::u32 vb_fourcc(int i) noexcept
{
    return crd::resources::make_fourcc('V', 'B', static_cast<char>('0' + (i / 10) % 10), static_cast<char>('0' + i % 10));
}
inline void w32(crd::containers::Array<crd::u8>& b, crd::u32 v)
{
    b.push_back(static_cast<crd::u8>(v & 0xFFU));
    b.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
    b.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU));
    b.push_back(static_cast<crd::u8>((v >> 24U) & 0xFFU));
}
[[nodiscard]] crd::u32 r32(crd::containers::ConstSpan<crd::u8> b, crd::usize off) noexcept
{
    return static_cast<crd::u32>(b[off]) | (static_cast<crd::u32>(b[off + 1]) << 8U) | (static_cast<crd::u32>(b[off + 2]) << 16U)
         | (static_cast<crd::u32>(b[off + 3]) << 24U);
}
} // namespace

CookResult cook_one_variant(
    VariantBuildFn build, void* user, crd::u32 key, crd::containers::StringView name, const CookOptions& opts,
    crd::memory::IAllocator* a)
{
    crd::kir::KGraph g(a);
    crd::kir::KEntry e = build(g, key, user);
    return cook_compute_shader(g, e, name, opts, a);
}

VariantMatrixResult cook_variant_matrix(
    VariantBuildFn build, void* user, const crd::u32* keys, int n_keys, const CookOptions& opts, crd::memory::IAllocator* a)
{
    VariantMatrixResult out(a);
    out.requested = static_cast<crd::u32>(n_keys < 0 ? 0 : n_keys);

    crd::containers::Array<crd::resources::ResourceId> seen(a); // distinct content hashes ⇒ the unique cook count
    for (int i = 0; i < n_keys; ++i)
    {
        const crd::u32   key = keys[i];
        crd::kir::KGraph g(a);
        crd::kir::KEntry e = build(g, key, user);

        // content-hash of the specialized IR (matches the id the cook caches under ⇒ a deduped key is a cache hit, not re-cooked).
        crd::containers::Array<crd::u8> ir = crd::kir::serialize_graph(g, e, a);
        const crd::resources::ResourceId id = crd::resources::ResourceId::from_content(crd::containers::as_const_span(ir));
        bool is_new = true;
        for (crd::usize s = 0; s < seen.size(); ++s) { if (seen[s] == id) { is_new = false; break; } }

        char nm[32];
        std::snprintf(nm, sizeof(nm), "variant_%08x", key);
        CookResult r = cook_compute_shader(g, e, crd::containers::StringView(nm), opts, a);
        if (!r.ok) { out.error.append("variant cook failed for a key"); return out; }

        if (is_new) { seen.push_back(id); }
        VariantManifestEntry ent;
        ent.key  = key;
        ent.hash = id;
        out.entries.push_back(ent);
    }
    out.unique = static_cast<crd::u32>(seen.size());
    out.ok     = true;
    return out;
}

VariantMatrixResult cook_variant_matrix_parallel(
    VariantBuildFn build, void* user, const crd::u32* keys, int n_keys, const CookOptions& opts, crd::memory::IAllocator* a)
{
    VariantMatrixResult out(a);
    out.requested = static_cast<crd::u32>(n_keys < 0 ? 0 : n_keys);

    // Pass 1 (serial, cheap): build + hash each key → the unique set + the manifest.
    crd::containers::Array<crd::resources::ResourceId> seen(a);
    crd::containers::Array<crd::u32>                   unique_keys(a);
    for (int i = 0; i < n_keys; ++i)
    {
        const crd::u32   key = keys[i];
        crd::kir::KGraph g(a);
        crd::kir::KEntry e   = build(g, key, user);
        crd::containers::Array<crd::u8>  ir = crd::kir::serialize_graph(g, e, a);
        const crd::resources::ResourceId h  = crd::resources::ResourceId::from_content(crd::containers::as_const_span(ir));
        bool is_new = true;
        for (crd::usize s = 0; s < seen.size(); ++s) { if (seen[s] == h) { is_new = false; break; } }
        if (is_new) { seen.push_back(h); unique_keys.push_back(key); }
        VariantManifestEntry ent;
        ent.key  = key;
        ent.hash = h;
        out.entries.push_back(ent);
    }
    out.unique = static_cast<crd::u32>(seen.size());

    // Pass 2 (parallel): cook each UNIQUE variant concurrently on the fiber scheduler — one job per range, its own allocator.
    const crd::u32                  nu = static_cast<crd::u32>(unique_keys.size());
    crd::containers::Array<crd::u8> ok(a);
    ok.resize(nu, static_cast<crd::u8>(0));
    if (nu > 0)
    {
        struct Ctx
        {
            VariantBuildFn     build;
            void*              user;
            const crd::u32*    uk;
            const CookOptions* opts;
            crd::u8*           ok;
        };
        Ctx            ctx{build, user, unique_keys.data(), &opts, ok.data()};
        const crd::u32 nw    = crd::jobs::num_workers();
        crd::u32       njobs = (nw == 0U ? 1U : nw);
        if (njobs > nu) { njobs = nu; }
        // StackSize::Large (2 MB): the cook runs the GLSL front-end (shaderc/glslang) + emitter + serializer, which need a
        // real thread-sized stack — the default 64 KB Small fiber OVERFLOWS inside shaderc (silent 0xC0000005). A normal OS
        // thread gives shaderc 1 MB; the 2 MB Large fiber matches that with headroom. (16 Large fibers exist by default.)
        crd::jobs::Counter* c = crd::jobs::parallel_for(
            nu, njobs,
            [ctxp = &ctx](crd::u32 b, crd::u32 e) {
                for (crd::u32 i = b; i < e; ++i)
                {
                    crd::memory::TlsfAllocator wa(8U << 20U); // per-variant scratch — no cross-job sharing
                    crd::kir::KGraph           g(&wa);
                    crd::kir::KEntry           ke = ctxp->build(g, ctxp->uk[i], ctxp->user);
                    CookResult                 r  = cook_compute_shader(g, ke, crd::containers::StringView("v"), *ctxp->opts, &wa);
                    if (r.ok) { ctxp->ok[i] = static_cast<crd::u8>(1); } // disjoint indices — no race
                }
            },
            crd::jobs::StackSize::Large);
        crd::jobs::wait(c);
    }
    for (crd::u32 i = 0; i < nu; ++i)
    {
        if (ok[i] == 0U) { out.error.append("parallel: a variant cook failed"); return out; }
    }
    out.ok = true;
    return out;
}

CookResult cook_variant_container(
    VariantBuildFn build, void* user, const crd::u32* keys, int n_keys, const CookOptions& opts, crd::memory::IAllocator* a)
{
    CookResult out(a);

    crd::containers::Array<crd::resources::ResourceId> seen(a);    // unique content hashes (in first-seen order)
    crd::containers::Array<crd::u8>                    uspv(a);     // concatenated unique variant SPIR-V
    crd::containers::Array<crd::u32>                   uoff(a);     // per unique: offset into uspv
    crd::containers::Array<crd::u32>                   ulen(a);     // per unique: length
    crd::containers::Array<crd::u32>                   req_key(a);  // per requested key: the key
    crd::containers::Array<crd::u32>                   req_idx(a);  // per requested key: the unique index
    crd::containers::Array<crd::u8>                    hashcat(a);  // concat of unique hashes → the container id

    CookOptions spv = opts;
    spv.backends     = static_cast<crd::u32>(CookBackend::SpirV);
    spv.cache_dir    = nullptr; // the container owns dedup; the per-variant cache would just be churn

    for (int i = 0; i < n_keys; ++i)
    {
        const crd::u32   key = keys[i];
        crd::kir::KGraph g(a);
        crd::kir::KEntry e = build(g, key, user);

        crd::containers::Array<crd::u8>  ir = crd::kir::serialize_graph(g, e, a);
        const crd::resources::ResourceId h  = crd::resources::ResourceId::from_content(crd::containers::as_const_span(ir));
        int                              idx = -1;
        for (crd::usize s = 0; s < seen.size(); ++s) { if (seen[s] == h) { idx = static_cast<int>(s); break; } }
        if (idx < 0)
        {
            idx = static_cast<int>(seen.size());
            seen.push_back(h);
            w32(hashcat, static_cast<crd::u32>(h.hi & 0xFFFFFFFFULL));
            w32(hashcat, static_cast<crd::u32>(h.hi >> 32U));
            w32(hashcat, static_cast<crd::u32>(h.lo & 0xFFFFFFFFULL));
            w32(hashcat, static_cast<crd::u32>(h.lo >> 32U));

            CookResult r = cook_compute_shader(g, e, crd::containers::StringView("variant"), spv, a);
            if (!r.ok) { out.error.append("container: variant cook failed"); return out; }
            ShaderBundle b(a);
            if (!read_shader_bundle(crd::containers::as_const_span(r.crdr), b)) { out.error.append("container: bundle read failed"); return out; }
            const auto code = b.bytecode(CookBackend::SpirV);
            uoff.push_back(static_cast<crd::u32>(uspv.size()));
            ulen.push_back(static_cast<crd::u32>(code.size()));
            for (crd::usize k = 0; k < code.size(); ++k) { uspv.push_back(code[k]); }
        }
        req_key.push_back(key);
        req_idx.push_back(static_cast<crd::u32>(idx));
    }

    const crd::resources::ResourceId cid = crd::resources::ResourceId::from_content(crd::containers::as_const_span(hashcat));
    crd::resources::CrdrWriter        w(a, cid, crd::resources::kFourCC_SHDR);

    crd::containers::Array<crd::u8> vart(a); // [u32 n_unique][u32 n_requested][ (u32 key, u32 unique_idx) × n_requested ]
    w32(vart, static_cast<crd::u32>(seen.size()));
    w32(vart, static_cast<crd::u32>(req_key.size()));
    for (crd::usize i = 0; i < req_key.size(); ++i) { w32(vart, req_key[i]); w32(vart, req_idx[i]); }
    w.add_chunk(kVartChunk, crd::containers::as_const_span(vart));
    for (crd::usize i = 0; i < seen.size(); ++i)
    {
        w.add_chunk(vb_fourcc(static_cast<int>(i)), crd::containers::ConstSpan<crd::u8>{uspv.data() + uoff[i], ulen[i]});
    }

    out.crdr        = w.finish();
    out.ok          = !out.crdr.empty();
    out.spirv_bytes = static_cast<crd::u32>(uspv.size()); // total unique bytecode packed (the reader exposes requested/unique counts)
    return out;
}

// ── container read path ──────────────────────────────────────────────────────
bool read_variant_container(crd::containers::ConstSpan<crd::u8> bytes, VariantContainer& out)
{
    return crd::resources::crdr_read(bytes, out.file, out.file.chunks.allocator()) == crd::resources::CrdrError::Ok;
}
crd::u32 VariantContainer::requested_count() const noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, kVartChunk);
    return (c != nullptr && c->payload.size() >= 8U) ? r32(c->payload, 4) : 0U;
}
crd::u32 VariantContainer::unique_count() const noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, kVartChunk);
    return (c != nullptr && c->payload.size() >= 8U) ? r32(c->payload, 0) : 0U;
}
crd::containers::ConstSpan<crd::u8> VariantContainer::bytecode(crd::u32 key) const noexcept
{
    const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, kVartChunk);
    if (c == nullptr || c->payload.size() < 8U) { return {}; }
    const crd::u32 nreq = r32(c->payload, 4);
    for (crd::u32 i = 0; i < nreq; ++i)
    {
        const crd::usize off = 8U + static_cast<crd::usize>(i) * 8U;
        if (r32(c->payload, off) == key)
        {
            const crd::u32                   idx = r32(c->payload, off + 4);
            const crd::resources::CrdrChunk* vb  = crd::resources::crdr_find_chunk(file, vb_fourcc(static_cast<int>(idx)));
            return vb != nullptr ? vb->payload : crd::containers::ConstSpan<crd::u8>{};
        }
    }
    return {};
}

} // namespace crd::shadercook
