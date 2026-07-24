// timeline_resource.cpp — GEO-9: TIML build + load. ONE validator runs at BOTH ends (a malformed timeline never
// becomes an artifact; a corrupt artifact never becomes a resource — the SKEL/ANIM doctrine).

#include <crd/timeline/timeline_resource.hpp>

#include <crd/hesap/interp/keyframe.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::timeline
{

namespace
{

void push_bytes(crd::containers::Array<crd::u8>& out, const void* p, crd::usize n)
{
    const auto* b = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < n; ++i) { out.push_back(b[i]); }
}

[[nodiscard]] bool valid_str_off(const TimelineResource& tl, crd::u32 off)
{
    return off < tl.strings.size();
}

[[nodiscard]] bool valid_attach(crd::u32 first, crd::u32 count, crd::usize pool)
{
    if (count == 0) { return true; } // first is kInvalidIndex or ignored
    return first != kInvalidIndex && static_cast<crd::usize>(first) + count <= pool;
}

// the shared truth: every structural rule the format promises (see timeline_resource.hpp)
[[nodiscard]] bool timeline_validate(const TimelineResource& tl)
{
    if (tl.strings.size() == 0 || tl.strings[0] != '\0') { return false; }
    if (tl.strings[tl.strings.size() - 1U] != '\0') { return false; }
    if (!valid_str_off(tl, tl.name_off)) { return false; }
    if (tl.has_global_start != 0 && !tl.global_start.valid()) { return false; }
    if (!valid_attach(tl.first_marker, tl.marker_count, tl.markers.size())) { return false; }

    // tracks partition the item array exactly, in order
    crd::usize covered = 0;
    for (const TrackRec& t : tl.tracks)
    {
        if (t.kind > static_cast<crd::u8>(TrackKind::Other)) { return false; }
        if (!valid_str_off(tl, t.name_off) || !valid_str_off(tl, t.kind_name_off)) { return false; }
        if (t.first_item != covered) { return false; }
        if (static_cast<crd::usize>(t.first_item) + t.item_count > tl.items.size()) { return false; }
        covered += t.item_count;
        if (!valid_attach(t.first_marker, t.marker_count, tl.markers.size())) { return false; }
    }
    if (covered != tl.items.size()) { return false; }

    for (const TrackRec& t : tl.tracks)
    {
        for (crd::u32 i = 0; i < t.item_count; ++i)
        {
            const ItemRec&  item = tl.items[t.first_item + i];
            const ItemType  type = static_cast<ItemType>(item.type);
            if (item.type > static_cast<crd::u8>(ItemType::Transition)) { return false; }
            if (!valid_str_off(tl, item.name_off) || !valid_str_off(tl, item.transition_type_off))
            {
                return false;
            }
            if (!valid_attach(item.first_effect, item.effect_count, tl.effects.size())) { return false; }
            if (!valid_attach(item.first_marker, item.marker_count, tl.markers.size())) { return false; }
            if (type == ItemType::Transition)
            {
                // BETWEEN two timed items: never first/last in its track, neighbors never transitions
                if (i == 0 || i + 1 >= t.item_count) { return false; }
                if (tl.items[t.first_item + i - 1].type == static_cast<crd::u8>(ItemType::Transition) ||
                    tl.items[t.first_item + i + 1].type == static_cast<crd::u8>(ItemType::Transition))
                {
                    return false;
                }
                if (!item.in_offset.valid() || !item.out_offset.valid() || item.in_offset.value < 0 ||
                    item.out_offset.value < 0)
                {
                    return false;
                }
            }
            else
            {
                // clips and gaps are TIMED: the builder requires a RESOLVED range (the cook resolves absent
                // clip trims from the media's available_range before building)
                if (item.has_source_range == 0 || !item.source_range.valid()) { return false; }
                if (type == ItemType::Clip && item.media_ref != kInvalidIndex &&
                    item.media_ref >= tl.media.size())
                {
                    return false;
                }
            }
        }
    }

    for (const MediaRec& m : tl.media)
    {
        if (m.kind > static_cast<crd::u8>(MediaKind::Resource)) { return false; }
        if (!valid_str_off(tl, m.name_off) || !valid_str_off(tl, m.url_off) ||
            !valid_str_off(tl, m.prefix_off) || !valid_str_off(tl, m.suffix_off))
        {
            return false;
        }
        if (m.has_available_range != 0 && !m.available_range.valid()) { return false; }
        if (static_cast<MediaKind>(m.kind) == MediaKind::ImageSequence &&
            (m.frame_step == 0 || !m.seq_rate.valid()))
        {
            return false;
        }
    }

    for (const EffectRec& e : tl.effects)
    {
        if (e.type > static_cast<crd::u8>(EffectType::FreezeFrame)) { return false; }
    }
    for (const MarkerRec& m : tl.markers)
    {
        if (!valid_str_off(tl, m.name_off) || !valid_str_off(tl, m.color_off) || !m.range.valid())
        {
            return false;
        }
    }

    for (const AutomationRec& a : tl.automation)
    {
        if (!valid_str_off(tl, a.target_off) || a.target_off == 0) { return false; } // a target is REQUIRED
        if (!a.rate.valid() || a.key_count == 0) { return false; }
        if (a.interp > static_cast<crd::u8>(crd::hesap::interp::KeyInterp::CubicHermite)) { return false; }
        const crd::usize elems =
            crd::hesap::interp::key_elements(static_cast<crd::hesap::interp::KeyInterp>(a.interp));
        if (static_cast<crd::usize>(a.ticks_off) + a.key_count > tl.auto_ticks.size()) { return false; }
        if (static_cast<crd::usize>(a.values_off) + static_cast<crd::usize>(a.key_count) * elems >
            tl.auto_values.size())
        {
            return false;
        }
        for (crd::u32 k = 1; k < a.key_count; ++k) // strictly increasing ticks — the curve engine's contract
        {
            if (tl.auto_ticks[a.ticks_off + k] <= tl.auto_ticks[a.ticks_off + k - 1]) { return false; }
        }
    }
    return true;
}

struct TimlHeader
{
    crd::u32                version          = 1;
    crd::u32                track_count      = 0;
    crd::u32                item_count       = 0;
    crd::u32                media_count      = 0;
    crd::u32                effect_count     = 0;
    crd::u32                marker_count     = 0;
    crd::u32                auto_count       = 0;
    crd::u32                tick_count       = 0;
    crd::u32                value_count      = 0;
    crd::u32                string_bytes     = 0;
    crd::u32                has_global_start = 0;
    crd::u32                name_off         = 0;
    crd::time::RationalTime global_start;
    crd::u32                first_marker     = kInvalidIndex; // timeline-level markers
    crd::u32                tl_marker_count  = 0;
};
static_assert(sizeof(TimlHeader) == 72, "TimlHeader is the on-disk 'TMHD' payload");

} // namespace

crd::containers::Array<crd::u8> timeline_build(const TimelineResource& tl, const crd::resources::ResourceId& id,
                                               crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (!timeline_validate(tl)) { return empty; }

    TimlHeader h;
    h.version          = tl.version;
    h.track_count      = static_cast<crd::u32>(tl.tracks.size());
    h.item_count       = static_cast<crd::u32>(tl.items.size());
    h.media_count      = static_cast<crd::u32>(tl.media.size());
    h.effect_count     = static_cast<crd::u32>(tl.effects.size());
    h.marker_count     = static_cast<crd::u32>(tl.markers.size());
    h.auto_count       = static_cast<crd::u32>(tl.automation.size());
    h.tick_count       = static_cast<crd::u32>(tl.auto_ticks.size());
    h.value_count      = static_cast<crd::u32>(tl.auto_values.size());
    h.string_bytes     = static_cast<crd::u32>(tl.strings.size());
    h.has_global_start = tl.has_global_start;
    h.name_off         = tl.name_off;
    h.global_start     = tl.global_start;
    h.first_marker     = tl.first_marker;
    h.tl_marker_count  = tl.marker_count;

    crd::containers::Array<crd::u8> head(alloc);
    push_bytes(head, &h, sizeof(h));

    crd::resources::CrdrWriter w(alloc, id, kFourCC_TIML);
    w.add_chunk(kFourCC_TmHd, crd::containers::as_const_span(head));
    w.add_chunk(kFourCC_TmSt, crd::containers::ConstSpan<crd::u8>(
                                  reinterpret_cast<const crd::u8*>(tl.strings.data()), tl.strings.size()));
    const auto span_of = [](const void* p, crd::usize bytes) {
        return crd::containers::ConstSpan<crd::u8>(static_cast<const crd::u8*>(p), bytes);
    };
    w.add_chunk(kFourCC_TmTk, span_of(tl.tracks.data(), tl.tracks.size() * sizeof(TrackRec)));
    w.add_chunk(kFourCC_TmIt, span_of(tl.items.data(), tl.items.size() * sizeof(ItemRec)));
    w.add_chunk(kFourCC_TmMd, span_of(tl.media.data(), tl.media.size() * sizeof(MediaRec)));
    w.add_chunk(kFourCC_TmFx, span_of(tl.effects.data(), tl.effects.size() * sizeof(EffectRec)));
    w.add_chunk(kFourCC_TmMk, span_of(tl.markers.data(), tl.markers.size() * sizeof(MarkerRec)));
    w.add_chunk(kFourCC_TmAu, span_of(tl.automation.data(), tl.automation.size() * sizeof(AutomationRec)));

    crd::containers::Array<crd::u8> adata(alloc);
    push_bytes(adata, tl.auto_ticks.data(), tl.auto_ticks.size() * 8U);
    push_bytes(adata, tl.auto_values.data(), tl.auto_values.size() * 4U);
    w.add_chunk_compressed(kFourCC_TmAd, crd::containers::as_const_span(adata));
    return w.finish();
}

void* TimelineLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file(&m_owned);
    if (crd::resources::crdr_read(ctx.bytes, file, &m_owned) != crd::resources::CrdrError::Ok) { return nullptr; }

    const crd::resources::CrdrChunk* hd = crd::resources::crdr_find_chunk(file, kFourCC_TmHd);
    const crd::resources::CrdrChunk* st = crd::resources::crdr_find_chunk(file, kFourCC_TmSt);
    if (hd == nullptr || st == nullptr || hd->payload.size() != sizeof(TimlHeader)) { return nullptr; }
    TimlHeader h;
    std::memcpy(&h, hd->payload.data(), sizeof(h));
    if (h.version != 1 || h.string_bytes == 0 || st->payload.size() != h.string_bytes) { return nullptr; }

    // every fixed-record chunk must be EXACTLY count × record — a torn artifact refuses
    const auto fetch = [&](crd::u32 fourcc, crd::usize count, crd::usize rec) -> const crd::resources::CrdrChunk* {
        const crd::resources::CrdrChunk* c = crd::resources::crdr_find_chunk(file, fourcc);
        if (c == nullptr || c->payload.size() != count * rec) { return nullptr; }
        return c;
    };
    const auto* tk = fetch(kFourCC_TmTk, h.track_count, sizeof(TrackRec));
    const auto* it = fetch(kFourCC_TmIt, h.item_count, sizeof(ItemRec));
    const auto* md = fetch(kFourCC_TmMd, h.media_count, sizeof(MediaRec));
    const auto* fx = fetch(kFourCC_TmFx, h.effect_count, sizeof(EffectRec));
    const auto* mk = fetch(kFourCC_TmMk, h.marker_count, sizeof(MarkerRec));
    const auto* au = fetch(kFourCC_TmAu, h.auto_count, sizeof(AutomationRec));
    const auto* ad = fetch(kFourCC_TmAd, 1, static_cast<crd::usize>(h.tick_count) * 8U +
                                                static_cast<crd::usize>(h.value_count) * 4U);
    if (tk == nullptr || it == nullptr || md == nullptr || fx == nullptr || mk == nullptr || au == nullptr ||
        ad == nullptr)
    {
        return nullptr;
    }

    void* raw = m_payload->try_allocate(sizeof(TimelineResource), alignof(TimelineResource));
    if (raw == nullptr) { return nullptr; }
    auto* tl = new (raw) TimelineResource(m_payload);

    tl->version          = h.version;
    tl->has_global_start = static_cast<crd::u8>(h.has_global_start);
    tl->global_start     = h.global_start;
    tl->name_off         = h.name_off;
    tl->first_marker     = h.first_marker;
    tl->marker_count     = h.tl_marker_count;

    const auto copy_into = [](auto& arr, const crd::resources::CrdrChunk* c, crd::usize count) {
        arr.resize(count);
        if (count > 0) { std::memcpy(arr.data(), c->payload.data(), c->payload.size()); }
    };
    tl->strings.clear();
    tl->strings.resize(h.string_bytes);
    std::memcpy(tl->strings.data(), st->payload.data(), h.string_bytes);
    copy_into(tl->tracks, tk, h.track_count);
    copy_into(tl->items, it, h.item_count);
    copy_into(tl->media, md, h.media_count);
    copy_into(tl->effects, fx, h.effect_count);
    copy_into(tl->markers, mk, h.marker_count);
    copy_into(tl->automation, au, h.auto_count);
    tl->auto_ticks.resize(h.tick_count);
    if (h.tick_count > 0)
    {
        std::memcpy(tl->auto_ticks.data(), ad->payload.data(), static_cast<crd::usize>(h.tick_count) * 8U);
    }
    tl->auto_values.resize(h.value_count);
    if (h.value_count > 0)
    {
        std::memcpy(tl->auto_values.data(), ad->payload.data() + static_cast<crd::usize>(h.tick_count) * 8U,
                    static_cast<crd::usize>(h.value_count) * 4U);
    }

    if (!timeline_validate(*tl)) // the SAME truth that gated the build gates the load
    {
        tl->~TimelineResource();
        m_payload->deallocate(tl);
        return nullptr;
    }
    return tl;
}

void TimelineLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto* tl = static_cast<TimelineResource*>(payload);
    crd::memory::IAllocator* a = m_payload;
    tl->~TimelineResource();
    a->deallocate(tl);
}

void register_timeline_loader(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<TimelineLoader>(payload_alloc));
}

} // namespace crd::timeline
