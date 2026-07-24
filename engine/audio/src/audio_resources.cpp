// audio_resources.cpp — GEO-10: ABUF + AGRF build/load, ONE validator both ends (the TIML doctrine).

#include <crd/audio/audio_resources.hpp>

#include <crd/hesap/interp/keyframe.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::audio
{

namespace
{

void push_bytes(crd::containers::Array<crd::u8>& out, const void* p, crd::usize n)
{
    const auto* b = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < n; ++i) { out.push_back(b[i]); }
}

struct AbufHeader
{
    crd::u32 sample_rate = 0;
    crd::u16 channels    = 0;
    crd::u16 source_bits = 0;
    crd::u64 frames      = 0;
};
static_assert(sizeof(AbufHeader) == 16);

struct AgrfHeader
{
    crd::u32 sample_rate  = 0;
    crd::u32 out_node     = kInvalidNode;
    crd::u32 name_off     = 0;
    crd::u32 node_count   = 0;
    crd::u32 edge_count   = 0;
    crd::u32 auto_count   = 0;
    crd::u32 tick_count   = 0;
    crd::u32 value_count  = 0;
    crd::u32 string_bytes = 0;
};
static_assert(sizeof(AgrfHeader) == 36);

// the shared truth for AGRF: bounds, the DAG (Kahn), one live output, automation contracts
[[nodiscard]] bool graph_validate(const AudioGraphResource& g)
{
    const crd::u32 n = static_cast<crd::u32>(g.nodes.size());
    if (n == 0 || g.out_node >= n) { return false; }
    if (g.strings.size() == 0 || g.strings[0] != '\0' || g.strings[g.strings.size() - 1U] != '\0')
    {
        return false;
    }
    if (g.sample_rate == 0) { return false; }
    for (const AudioNodeRec& node : g.nodes)
    {
        if (node.type > static_cast<crd::u8>(AudioNodeType::Send)) { return false; }
        if (node.name_off >= g.strings.size()) { return false; }
        if (static_cast<AudioNodeType>(node.type) == AudioNodeType::Biquad)
        {
            if (node.filter > static_cast<crd::u8>(BiquadType::Notch)) { return false; }
            if (!(node.cutoff > 0.0F) || !(node.cutoff < 1.0F) || !(node.q > 0.0F)) { return false; }
        }
        if (static_cast<AudioNodeType>(node.type) == AudioNodeType::Source && node.start_frame < 0)
        {
            return false;
        }
    }
    // Kahn's algorithm on a small scratch — cycles refuse (feedback lands with its own delay-line story)
    crd::u32 indegree[256];
    if (n > 256) { return false; } // v1 graph cap — far above any gate; lift with a real scratch when needed
    for (crd::u32 i = 0; i < n; ++i) { indegree[i] = 0; }
    for (const AudioEdgeRec& e : g.edges)
    {
        if (e.from >= n || e.to >= n || e.from == e.to) { return false; }
        ++indegree[e.to];
    }
    crd::u32 queue[256];
    crd::u32 head = 0;
    crd::u32 tail = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (indegree[i] == 0) { queue[tail++] = i; }
    }
    crd::u32 visited = 0;
    while (head < tail)
    {
        const crd::u32 v = queue[head++];
        ++visited;
        for (const AudioEdgeRec& e : g.edges)
        {
            if (e.from == v && --indegree[e.to] == 0) { queue[tail++] = e.to; }
        }
    }
    if (visited != n) { return false; } // a cycle

    for (const AudioAutoRec& a : g.automation)
    {
        if (a.node >= n || a.param > static_cast<crd::u8>(AudioParam::CutoffNyq)) { return false; }
        if (!a.rate.valid() || a.key_count == 0) { return false; }
        if (a.interp > static_cast<crd::u8>(crd::hesap::interp::KeyInterp::CubicHermite)) { return false; }
        const crd::usize elems =
            crd::hesap::interp::key_elements(static_cast<crd::hesap::interp::KeyInterp>(a.interp));
        if (static_cast<crd::usize>(a.ticks_off) + a.key_count > g.auto_ticks.size()) { return false; }
        if (static_cast<crd::usize>(a.values_off) + static_cast<crd::usize>(a.key_count) * elems >
            g.auto_values.size())
        {
            return false;
        }
        for (crd::u32 k = 1; k < a.key_count; ++k)
        {
            if (g.auto_ticks[a.ticks_off + k] <= g.auto_ticks[a.ticks_off + k - 1]) { return false; }
        }
    }
    return true;
}

} // namespace

// ── ABUF ───────────────────────────────────────────────────────────────────────────────────────────────────────

crd::containers::Array<crd::u8> audio_buffer_build(const AudioPcm& pcm, const crd::resources::ResourceId& id,
                                                   crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (!pcm.valid()) { return empty; }

    crd::containers::Array<crd::f32> f32s(alloc);
    pcm_to_f32(pcm, f32s);

    AbufHeader h;
    h.sample_rate = pcm.sample_rate;
    h.channels    = pcm.channels;
    h.source_bits = pcm.bits_per_sample;
    h.frames      = pcm.frame_count();

    crd::containers::Array<crd::u8> payload(alloc);
    push_bytes(payload, &h, sizeof(h));
    push_bytes(payload, f32s.data(), f32s.size() * 4U);

    crd::resources::CrdrWriter w(alloc, id, kFourCC_ABUF);
    w.add_chunk_compressed(kFourCC_AbDt, crd::containers::as_const_span(payload));
    return w.finish();
}

void* AudioBufferLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file(&m_owned);
    if (crd::resources::crdr_read(ctx.bytes, file, &m_owned) != crd::resources::CrdrError::Ok) { return nullptr; }
    const crd::resources::CrdrChunk* dt = crd::resources::crdr_find_chunk(file, kFourCC_AbDt);
    if (dt == nullptr || dt->payload.size() < sizeof(AbufHeader)) { return nullptr; }
    AbufHeader h;
    std::memcpy(&h, dt->payload.data(), sizeof(h));
    const crd::usize count = static_cast<crd::usize>(h.frames) * h.channels;
    if (h.sample_rate == 0 || h.channels == 0 ||
        dt->payload.size() != sizeof(h) + count * 4U)
    {
        return nullptr;
    }
    void* raw = m_payload->try_allocate(sizeof(AudioBufferResource), alignof(AudioBufferResource));
    if (raw == nullptr) { return nullptr; }
    auto* buf        = new (raw) AudioBufferResource(m_payload);
    buf->sample_rate = h.sample_rate;
    buf->channels    = h.channels;
    buf->source_bits = h.source_bits;
    buf->samples.resize(count);
    if (count > 0) { std::memcpy(buf->samples.data(), dt->payload.data() + sizeof(h), count * 4U); }
    return buf;
}

void AudioBufferLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto*                    buf = static_cast<AudioBufferResource*>(payload);
    crd::memory::IAllocator* a   = m_payload;
    buf->~AudioBufferResource();
    a->deallocate(buf);
}

// ── AGRF ───────────────────────────────────────────────────────────────────────────────────────────────────────

crd::containers::Array<crd::u8> audio_graph_build(const AudioGraphResource& graph,
                                                  const crd::resources::ResourceId& id,
                                                  crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (!graph_validate(graph)) { return empty; }

    AgrfHeader h;
    h.sample_rate  = graph.sample_rate;
    h.out_node     = graph.out_node;
    h.name_off     = graph.name_off;
    h.node_count   = static_cast<crd::u32>(graph.nodes.size());
    h.edge_count   = static_cast<crd::u32>(graph.edges.size());
    h.auto_count   = static_cast<crd::u32>(graph.automation.size());
    h.tick_count   = static_cast<crd::u32>(graph.auto_ticks.size());
    h.value_count  = static_cast<crd::u32>(graph.auto_values.size());
    h.string_bytes = static_cast<crd::u32>(graph.strings.size());

    crd::containers::Array<crd::u8> nodes(alloc);
    push_bytes(nodes, &h, sizeof(h));
    push_bytes(nodes, graph.nodes.data(), graph.nodes.size() * sizeof(AudioNodeRec));

    crd::resources::CrdrWriter w(alloc, id, kFourCC_AGRF);
    w.add_chunk(kFourCC_AgNd, crd::containers::as_const_span(nodes));
    w.add_chunk(kFourCC_AgEg,
                crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(graph.edges.data()),
                                                    graph.edges.size() * sizeof(AudioEdgeRec)));
    w.add_chunk(kFourCC_AgAu,
                crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(graph.automation.data()),
                                                    graph.automation.size() * sizeof(AudioAutoRec)));
    crd::containers::Array<crd::u8> adata(alloc);
    push_bytes(adata, graph.auto_ticks.data(), graph.auto_ticks.size() * 8U);
    push_bytes(adata, graph.auto_values.data(), graph.auto_values.size() * 4U);
    w.add_chunk_compressed(kFourCC_AgAd, crd::containers::as_const_span(adata));
    w.add_chunk(kFourCC_AgSt, crd::containers::ConstSpan<crd::u8>(
                                  reinterpret_cast<const crd::u8*>(graph.strings.data()), graph.strings.size()));
    return w.finish();
}

void* AudioGraphLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file(&m_owned);
    if (crd::resources::crdr_read(ctx.bytes, file, &m_owned) != crd::resources::CrdrError::Ok) { return nullptr; }
    const crd::resources::CrdrChunk* nd = crd::resources::crdr_find_chunk(file, kFourCC_AgNd);
    const crd::resources::CrdrChunk* eg = crd::resources::crdr_find_chunk(file, kFourCC_AgEg);
    const crd::resources::CrdrChunk* au = crd::resources::crdr_find_chunk(file, kFourCC_AgAu);
    const crd::resources::CrdrChunk* ad = crd::resources::crdr_find_chunk(file, kFourCC_AgAd);
    const crd::resources::CrdrChunk* st = crd::resources::crdr_find_chunk(file, kFourCC_AgSt);
    if (nd == nullptr || eg == nullptr || au == nullptr || ad == nullptr || st == nullptr ||
        nd->payload.size() < sizeof(AgrfHeader))
    {
        return nullptr;
    }
    AgrfHeader h;
    std::memcpy(&h, nd->payload.data(), sizeof(h));
    if (nd->payload.size() != sizeof(h) + static_cast<crd::usize>(h.node_count) * sizeof(AudioNodeRec) ||
        eg->payload.size() != static_cast<crd::usize>(h.edge_count) * sizeof(AudioEdgeRec) ||
        au->payload.size() != static_cast<crd::usize>(h.auto_count) * sizeof(AudioAutoRec) ||
        ad->payload.size() != static_cast<crd::usize>(h.tick_count) * 8U +
                                  static_cast<crd::usize>(h.value_count) * 4U ||
        st->payload.size() != h.string_bytes || h.string_bytes == 0)
    {
        return nullptr;
    }

    void* raw = m_payload->try_allocate(sizeof(AudioGraphResource), alignof(AudioGraphResource));
    if (raw == nullptr) { return nullptr; }
    auto* g        = new (raw) AudioGraphResource(m_payload);
    g->sample_rate = h.sample_rate;
    g->out_node    = h.out_node;
    g->name_off    = h.name_off;
    g->nodes.resize(h.node_count);
    std::memcpy(g->nodes.data(), nd->payload.data() + sizeof(h),
                static_cast<crd::usize>(h.node_count) * sizeof(AudioNodeRec));
    g->edges.resize(h.edge_count);
    if (h.edge_count > 0) { std::memcpy(g->edges.data(), eg->payload.data(), eg->payload.size()); }
    g->automation.resize(h.auto_count);
    if (h.auto_count > 0) { std::memcpy(g->automation.data(), au->payload.data(), au->payload.size()); }
    g->auto_ticks.resize(h.tick_count);
    if (h.tick_count > 0)
    {
        std::memcpy(g->auto_ticks.data(), ad->payload.data(), static_cast<crd::usize>(h.tick_count) * 8U);
    }
    g->auto_values.resize(h.value_count);
    if (h.value_count > 0)
    {
        std::memcpy(g->auto_values.data(), ad->payload.data() + static_cast<crd::usize>(h.tick_count) * 8U,
                    static_cast<crd::usize>(h.value_count) * 4U);
    }
    g->strings.clear();
    g->strings.resize(h.string_bytes);
    std::memcpy(g->strings.data(), st->payload.data(), h.string_bytes);

    if (!graph_validate(*g))
    {
        g->~AudioGraphResource();
        m_payload->deallocate(g);
        return nullptr;
    }
    return g;
}

void AudioGraphLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto*                    g = static_cast<AudioGraphResource*>(payload);
    crd::memory::IAllocator* a = m_payload;
    g->~AudioGraphResource();
    a->deallocate(g);
}

void register_audio_loaders(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<AudioBufferLoader>(payload_alloc));
    rm->register_loader(std::make_unique<AudioGraphLoader>(payload_alloc));
}

} // namespace crd::audio
