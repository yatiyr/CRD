// timeline_eval.cpp — GEO-9: the deterministic evaluator (see timeline_eval.hpp for the doctrine split).

#include <crd/timeline/timeline_eval.hpp>

#include <crd/hesap/interp/keyframe.hpp> // KeyInterp + interp_hermite — the ONE curve engine's primitives

#include <cstring>

namespace crd::timeline
{

namespace
{
    using crd::time::add;
    using crd::time::compare;
    using crd::time::RationalTime;
    using crd::time::sub;

    // media_time' = clip_start + (media_time - clip_start) * scalar — the LinearTimeWarp contract. The scalar
    // converts through the exact f64→rational edge (2.0 · 0.5 · 1.5 warp exactly); a scalar too exotic for the
    // rational fields falls back to floor-of-f64 ticks at the clip's rate (documented, never silent NaN).
    [[nodiscard]] RationalTime apply_time_scalar(const RationalTime& clip_start, const RationalTime& offset,
                                                 crd::f64 scalar)
    {
        const RationalTime frac = crd::time::time_from_f64(scalar, 1.0); // scalar as ticks at rate num/den=1/1…
        if (frac.valid())
        {
            // offset × (frac.value / frac_rate_den⁻¹): scale the tick count, fold the scalar's denominator
            // into the rate — exact while the fields hold
            const crd::i64                sv = frac.value;
            const crd::time::RationalRate fr = frac.rate; // scalar = sv * fr.den / fr.num
            const crd::time::RationalRate scaled_rate =
                crd::time::make_rate(static_cast<crd::i64>(offset.rate.num) * fr.num,
                                     static_cast<crd::i64>(offset.rate.den) * fr.den);
            if (scaled_rate.valid())
            {
                return add(clip_start, RationalTime{offset.value * sv, scaled_rate});
            }
        }
        const crd::f64 secs = crd::time::to_seconds_f64(offset) * scalar; // the documented fallback edge
        return add(clip_start, crd::time::time_from_f64(secs * crd::time::rate_to_f64(offset.rate),
                                                        crd::time::rate_to_f64(offset.rate)));
    }
} // namespace

RationalTime item_duration(const TimelineResource& tl, const ItemRec& item) noexcept
{
    (void)tl;
    if (item.type == static_cast<crd::u8>(ItemType::Transition))
    {
        return RationalTime{0, crd::time::kRate24}; // zero width in the sequence — any valid rate carries a zero
    }
    return item.source_range.duration;
}

RationalTime item_start(const TimelineResource& tl, const TrackRec& track, crd::u32 index_in_track) noexcept
{
    RationalTime pos{0, crd::time::kRate24}; // rational zero; add() merges onto the items' real grids
    for (crd::u32 i = 0; i < index_in_track && i < track.item_count; ++i)
    {
        pos = add(pos, item_duration(tl, tl.items[track.first_item + i]));
    }
    return pos;
}

RationalTime track_duration(const TimelineResource& tl, const TrackRec& track) noexcept
{
    return item_start(tl, track, track.item_count);
}

RationalTime timeline_duration(const TimelineResource& tl) noexcept
{
    RationalTime longest{0, crd::time::kRate24};
    for (const TrackRec& t : tl.tracks)
    {
        const RationalTime d = track_duration(tl, t);
        if (compare(d, longest) > 0) { longest = d; }
    }
    return longest;
}

namespace
{
    // the clip's media time for track-local `t`, item starting at `start` — trims + time effects applied
    [[nodiscard]] RationalTime clip_media_time(const TimelineResource& tl, const ItemRec& item,
                                               const RationalTime& start, const RationalTime& t)
    {
        const RationalTime offset     = sub(t, start);
        const RationalTime clip_start = item.source_range.start;
        // time effects apply in order; FreezeFrame pins to the trimmed start (scalar 0 composes the same way)
        crd::f64 scalar     = 1.0;
        bool     has_effect = false;
        for (crd::u32 e = 0; e < item.effect_count; ++e)
        {
            const EffectRec& fx = tl.effects[item.first_effect + e];
            scalar *= static_cast<EffectType>(fx.type) == EffectType::FreezeFrame ? 0.0 : fx.time_scalar;
            has_effect = true;
        }
        if (!has_effect) { return add(clip_start, offset); }
        return apply_time_scalar(clip_start, offset, scalar);
    }

    // dissolve weight for the INCOMING side at `t` inside the window [win_start, win_start+win_len) — the f32 edge
    [[nodiscard]] crd::f32 dissolve_weight(const RationalTime& t, const RationalTime& win_start,
                                           const RationalTime& win_len)
    {
        const crd::f64 num = crd::time::to_seconds_f64(sub(t, win_start));
        const crd::f64 den = crd::time::to_seconds_f64(win_len);
        if (den <= 0.0) { return 1.0F; }
        crd::f64 w = num / den;
        if (w < 0.0) { w = 0.0; }
        if (w > 1.0) { w = 1.0; }
        return static_cast<crd::f32>(w);
    }
} // namespace

void evaluate_tracks(const TimelineResource& tl, const RationalTime& t, crd::containers::Array<ActiveClip>& out)
{
    for (crd::usize ti = 0; ti < tl.tracks.size(); ++ti)
    {
        const TrackRec& track = tl.tracks[ti];
        RationalTime    pos{0, crd::time::kRate24};

        for (crd::u32 i = 0; i < track.item_count; ++i)
        {
            const ItemRec& item = tl.items[track.first_item + i];
            if (item.type == static_cast<crd::u8>(ItemType::Transition)) { continue; }
            const RationalTime dur = item.source_range.duration;
            const RationalTime end = add(pos, dur);
            // half-open [pos, end) — the covering item; boundaries land EXACTLY once (rational compare)
            const bool covers = compare(t, pos) >= 0 && compare(t, end) < 0;

            // transition windows adjacent to this item (the transition OVERLAYS the cut)
            //   before: [pos - prev_in, pos + out) owned by the transition sitting between prev and this
            //   after:  [end - in, end + out)
            crd::f32 weight     = 1.0F;
            bool     active     = covers;
            bool     from_window = false;
            if (i > 0 && tl.items[track.first_item + i - 1].type == static_cast<crd::u8>(ItemType::Transition))
            {
                const ItemRec&     tr        = tl.items[track.first_item + i - 1];
                const RationalTime win_start = sub(pos, tr.in_offset);
                const RationalTime win_len   = add(tr.in_offset, tr.out_offset);
                const RationalTime win_end   = add(win_start, win_len);
                if (compare(t, win_start) >= 0 && compare(t, win_end) < 0)
                {
                    weight      = dissolve_weight(t, win_start, win_len); // INCOMING ramps 0→1
                    active      = true;                                   // active even BEFORE its start (handles)
                    from_window = true;
                }
            }
            if (i + 1 < track.item_count &&
                tl.items[track.first_item + i + 1].type == static_cast<crd::u8>(ItemType::Transition))
            {
                const ItemRec&     tr        = tl.items[track.first_item + i + 1];
                const RationalTime win_start = sub(end, tr.in_offset);
                const RationalTime win_len   = add(tr.in_offset, tr.out_offset);
                const RationalTime win_end   = add(win_start, win_len);
                if (compare(t, win_start) >= 0 && compare(t, win_end) < 0)
                {
                    weight      = 1.0F - dissolve_weight(t, win_start, win_len); // OUTGOING ramps 1→0
                    active      = true; // active even AFTER its end (handles)
                    from_window = true;
                }
            }
            if (!active) { pos = end; continue; }
            if (item.type == static_cast<crd::u8>(ItemType::Gap)) { pos = end; continue; } // gaps show nothing

            ActiveClip clip;
            clip.track_index = static_cast<crd::u32>(ti);
            clip.item_index  = track.first_item + i;
            clip.media_ref   = item.media_ref;
            clip.media_time  = clip_media_time(tl, item, pos, t);
            clip.weight      = from_window ? weight : 1.0F;
            out.push_back(clip);
            pos = end;
        }
    }
}

bool automation_value(const TimelineResource& tl, crd::u32 index, const RationalTime& t, crd::f32& out) noexcept
{
    out = 0.0F;
    if (index >= tl.automation.size()) { return false; }
    const AutomationRec& a     = tl.automation[index];
    const auto           inter = static_cast<crd::hesap::interp::KeyInterp>(a.interp);
    const crd::u32       elems = crd::hesap::interp::key_elements(inter);
    const crd::i64*      ticks = tl.auto_ticks.data() + a.ticks_off;
    const crd::f32*      vals  = tl.auto_values.data() + a.values_off;

    const auto key_value = [&](crd::u32 k) -> crd::f32 {
        return inter == crd::hesap::interp::KeyInterp::CubicHermite ? vals[k * elems * 1U + 1U]
                                                                    : vals[k * elems * 1U];
    };
    const auto key_time = [&](crd::u32 k) -> RationalTime { return RationalTime{ticks[k], a.rate}; };

    // boundary clamps (the keyframe contract) — decided in EXACT rational time
    if (compare(t, key_time(0)) <= 0)
    {
        out = key_value(0);
        return true;
    }
    if (compare(t, key_time(a.key_count - 1)) >= 0)
    {
        out = key_value(a.key_count - 1);
        return true;
    }

    // rational-exact segment selection (linear scan — automation tracks are small; the structure edge)
    crd::u32 seg = 0;
    for (crd::u32 k = 1; k < a.key_count; ++k)
    {
        if (compare(t, key_time(k)) < 0)
        {
            seg = k - 1;
            break;
        }
    }

    switch (inter)
    {
    case crd::hesap::interp::KeyInterp::Step:
        out = key_value(seg);
        return true;
    case crd::hesap::interp::KeyInterp::Linear:
    {
        const crd::f64 sec0 = crd::time::to_seconds_f64(key_time(seg));
        const crd::f64 sec1 = crd::time::to_seconds_f64(key_time(seg + 1));
        const crd::f64 u    = (crd::time::to_seconds_f64(t) - sec0) / (sec1 - sec0); // the f64 value edge
        const crd::f64 v0   = static_cast<crd::f64>(key_value(seg));
        const crd::f64 v1   = static_cast<crd::f64>(key_value(seg + 1));
        out                 = static_cast<crd::f32>(v0 + u * (v1 - v0));
        return true;
    }
    case crd::hesap::interp::KeyInterp::CubicHermite:
    default:
    {
        // the ONE curve engine's Hermite primitive on the bracketing pair — split tangents, seconds domain
        const crd::f64   x2[2] = {crd::time::to_seconds_f64(key_time(seg)),
                                  crd::time::to_seconds_f64(key_time(seg + 1))};
        const crd::f64   y2[2] = {static_cast<crd::f64>(vals[seg * 3U + 1U]),
                                  static_cast<crd::f64>(vals[(seg + 1U) * 3U + 1U])};
        const crd::f64   d2[2] = {static_cast<crd::f64>(vals[seg * 3U + 2U]),        // out-tangent of key seg
                                  static_cast<crd::f64>(vals[(seg + 1U) * 3U])};     // in-tangent of key seg+1
        crd::usize       cache = 0;
        const crd::f64   v = crd::hesap::interp::interp_hermite(
            crd::containers::ConstSpan<crd::f64>(x2, 2U), crd::containers::ConstSpan<crd::f64>(y2, 2U),
            crd::containers::ConstSpan<crd::f64>(d2, 2U), crd::time::to_seconds_f64(t), cache);
        out = static_cast<crd::f32>(v);
        return true;
    }
    }
}

crd::u32 find_automation(const TimelineResource& tl, const char* target) noexcept
{
    if (target == nullptr) { return kInvalidIndex; }
    for (crd::usize i = 0; i < tl.automation.size(); ++i)
    {
        if (std::strcmp(tl.str(tl.automation[i].target_off), target) == 0)
        {
            return static_cast<crd::u32>(i);
        }
    }
    return kInvalidIndex;
}

} // namespace crd::timeline
