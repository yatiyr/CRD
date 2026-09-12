// anim_resources.cpp — GEO-8 (D-007 row 73): SKEL/ANIM builders + loaders. See anim_resources.hpp.

#include <crd/anim/anim_resources.hpp>

#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::anim
{

namespace
{

void push_bytes(crd::containers::Array<crd::u8>& out, const void* p, crd::usize n)
{
    const auto* b = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < n; ++i) { out.push_back(b[i]); }
}

} // namespace

// ── builders ───────────────────────────────────────────────────────────────────────────────────────────────────

crd::containers::Array<crd::u8> skeleton_build(const SkeletonResource& skeleton,
                                               const crd::resources::ResourceId& id, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    const crd::u32                  n = skeleton.joint_count();
    if (n == 0U || skeleton.rest.size() != static_cast<crd::usize>(n) * kRestFloats
        || skeleton.inverse_binds.size() != static_cast<crd::usize>(n) * 16U
        || skeleton.name_offsets.size() != n)
    {
        return empty;
    }
    for (crd::u32 i = 0; i < n; ++i) // the topological contract IS the format — refuse violations at build
    {
        if (skeleton.parents[i] >= static_cast<crd::i32>(i)) { return empty; }
    }

    crd::containers::Array<crd::u8> joints(alloc);
    push_bytes(joints, &n, 4U);
    push_bytes(joints, skeleton.parents.data(), skeleton.parents.size() * 4U);
    push_bytes(joints, skeleton.rest.data(), skeleton.rest.size() * 4U);
    push_bytes(joints, skeleton.inverse_binds.data(), skeleton.inverse_binds.size() * 4U);

    crd::containers::Array<crd::u8> names(alloc);
    push_bytes(names, skeleton.name_offsets.data(), skeleton.name_offsets.size() * 4U);
    push_bytes(names, skeleton.name_pool.data(), skeleton.name_pool.size());

    crd::resources::CrdrWriter w(alloc, id, kFourCC_SKEL);
    w.add_chunk(kFourCC_SklJ, crd::containers::as_const_span(joints));
    w.add_chunk(kFourCC_SklN, crd::containers::as_const_span(names));
    return w.finish();
}

crd::containers::Array<crd::u8> anim_clip_build(const AnimClipResource& clip, const crd::resources::ResourceId& id,
                                                crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (clip.tracks.size() == 0U) { return empty; }
    for (const AnimTrack& t : clip.tracks) // offsets must stay inside the blob — refuse at build, never at load
    {
        const crd::usize span = static_cast<crd::usize>(t.interp == 2 ? 3U : 1U) * t.components;
        if (t.key_count == 0U || t.components == 0U) { return empty; }
        if (static_cast<crd::usize>(t.times_off) + t.key_count > clip.data.size()) { return empty; }
        if (static_cast<crd::usize>(t.values_off) + static_cast<crd::usize>(t.key_count) * span > clip.data.size())
        {
            return empty;
        }
    }

    crd::containers::Array<crd::u8> dir(alloc);
    const crd::u32                  tc = static_cast<crd::u32>(clip.tracks.size());
    push_bytes(dir, &clip.duration, 4U);
    push_bytes(dir, &tc, 4U);
    push_bytes(dir, clip.tracks.data(), clip.tracks.size() * sizeof(AnimTrack));

    crd::resources::CrdrWriter w(alloc, id, kFourCC_ANIM);
    w.add_chunk(kFourCC_AnmT, crd::containers::as_const_span(dir));
    w.add_chunk_compressed(kFourCC_AnmD,
                           crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(clip.data.data()),
                                                               clip.data.size() * 4U));
    return w.finish();
}

// ── loaders ────────────────────────────────────────────────────────────────────────────────────────────────────

void* SkeletonLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file(&m_owned);
    if (crd::resources::crdr_read(ctx.bytes, file, &m_owned) != crd::resources::CrdrError::Ok) { return nullptr; }
    const crd::resources::CrdrChunk* jc = crd::resources::crdr_find_chunk(file, kFourCC_SklJ);
    const crd::resources::CrdrChunk* nc = crd::resources::crdr_find_chunk(file, kFourCC_SklN);
    if (jc == nullptr || nc == nullptr || jc->payload.size() < 4U) { return nullptr; }

    crd::u32 n = 0;
    std::memcpy(&n, jc->payload.data(), 4U);
    const crd::usize need = 4U + static_cast<crd::usize>(n) * (4U + kRestFloats * 4U + 64U);
    if (n == 0U || jc->payload.size() < need || nc->payload.size() < static_cast<crd::usize>(n) * 4U)
    {
        return nullptr;
    }

    void* raw = m_payload->try_allocate(sizeof(SkeletonResource), alignof(SkeletonResource));
    if (raw == nullptr) { return nullptr; }
    auto* skel = new (raw) SkeletonResource(m_payload);

    const crd::u8* p = jc->payload.data() + 4U;
    skel->parents.resize(n);
    std::memcpy(skel->parents.data(), p, static_cast<crd::usize>(n) * 4U);
    p += static_cast<crd::usize>(n) * 4U;
    skel->rest.resize(static_cast<crd::usize>(n) * kRestFloats);
    std::memcpy(skel->rest.data(), p, skel->rest.size() * 4U);
    p += skel->rest.size() * 4U;
    skel->inverse_binds.resize(static_cast<crd::usize>(n) * 16U);
    std::memcpy(skel->inverse_binds.data(), p, skel->inverse_binds.size() * 4U);

    for (crd::u32 i = 0; i < n; ++i) // the topological invariant gates the LOAD too (a corrupt artifact refuses)
    {
        if (skel->parents[i] >= static_cast<crd::i32>(i))
        {
            skel->~SkeletonResource();
            m_payload->deallocate(skel);
            return nullptr;
        }
    }

    skel->name_offsets.resize(n);
    std::memcpy(skel->name_offsets.data(), nc->payload.data(), static_cast<crd::usize>(n) * 4U);
    const crd::usize pool_size = nc->payload.size() - static_cast<crd::usize>(n) * 4U;
    skel->name_pool.resize(pool_size > 0U ? pool_size : 1U);
    skel->name_pool[skel->name_pool.size() - 1U] = '\0';
    if (pool_size > 0U)
    {
        std::memcpy(skel->name_pool.data(), nc->payload.data() + static_cast<crd::usize>(n) * 4U, pool_size);
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (skel->name_offsets[i] >= skel->name_pool.size()) { skel->name_offsets[i] = static_cast<crd::u32>(skel->name_pool.size() - 1U); }
    }
    return skel;
}

void SkeletonLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto* skel = static_cast<SkeletonResource*>(payload);
    skel->~SkeletonResource();
    m_payload->deallocate(skel);
}

void* AnimClipLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file(&m_owned);
    if (crd::resources::crdr_read(ctx.bytes, file, &m_owned) != crd::resources::CrdrError::Ok) { return nullptr; }
    const crd::resources::CrdrChunk* tc = crd::resources::crdr_find_chunk(file, kFourCC_AnmT);
    const crd::resources::CrdrChunk* dc = crd::resources::crdr_find_chunk(file, kFourCC_AnmD);
    if (tc == nullptr || dc == nullptr || tc->payload.size() < 8U || (dc->payload.size() % 4U) != 0U)
    {
        return nullptr;
    }

    crd::f32 duration = 0.0F;
    crd::u32 n        = 0;
    std::memcpy(&duration, tc->payload.data(), 4U);
    std::memcpy(&n, tc->payload.data() + 4U, 4U);
    if (n == 0U || tc->payload.size() < 8U + static_cast<crd::usize>(n) * sizeof(AnimTrack)) { return nullptr; }

    void* raw = m_payload->try_allocate(sizeof(AnimClipResource), alignof(AnimClipResource));
    if (raw == nullptr) { return nullptr; }
    auto* clip     = new (raw) AnimClipResource(m_payload);
    clip->duration = duration;
    clip->tracks.resize(n);
    std::memcpy(clip->tracks.data(), tc->payload.data() + 8U, static_cast<crd::usize>(n) * sizeof(AnimTrack));
    clip->data.resize(dc->payload.size() / 4U);
    std::memcpy(clip->data.data(), dc->payload.data(), dc->payload.size());

    for (const AnimTrack& t : clip->tracks) // blob-bounds gate on load — a corrupt directory never samples OOB
    {
        const crd::usize span = static_cast<crd::usize>(t.interp == 2 ? 3U : 1U) * t.components;
        const bool bad = t.key_count == 0U || t.components == 0U
                         || static_cast<crd::usize>(t.times_off) + t.key_count > clip->data.size()
                         || static_cast<crd::usize>(t.values_off) + static_cast<crd::usize>(t.key_count) * span
                                > clip->data.size();
        if (bad)
        {
            clip->~AnimClipResource();
            m_payload->deallocate(clip);
            return nullptr;
        }
    }
    return clip;
}

void AnimClipLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto* clip = static_cast<AnimClipResource*>(payload);
    clip->~AnimClipResource();
    m_payload->deallocate(clip);
}

void register_anim_loaders(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<SkeletonLoader>(payload_alloc));
    rm->register_loader(std::make_unique<AnimClipLoader>(payload_alloc));
}

} // namespace crd::anim
