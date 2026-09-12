#pragma once

// timeline_eval.hpp — GEO-9: DETERMINISTIC timeline evaluation. The doctrine split:
//   - STRUCTURE (which item covers t · where transitions sit · which curve segment) is decided in EXACT
//     rational time — two hosts asking the same question get the same answer, always;
//   - VALUES (dissolve weights · curve outputs) are floats at the named edge — they are render-domain data.
//
// Track model (OTIO's): a track's clips/gaps occupy SEQUENTIAL time (positions accumulate — items have no
// authored starts); transitions have ZERO width in the sequence and OVERLAY the cut between their neighbors,
// borrowing `in_offset` from the outgoing item's tail and `out_offset` from the incoming item's head. Inside
// that window BOTH neighbors are active with dissolve weights (media handles extend past the trims — the
// standard editorial contract).
//
// Time effects: LinearTimeWarp scales media time about the clip's trimmed start (the scalar converts through
// the same exact-rational f64 edge as OTIO import — 2.0 and 0.5 warp EXACTLY); FreezeFrame holds it.
//
// Automation: rational-exact segment selection over i64 ticks, then hesap-interp's keyframe semantics inside
// the segment (Step · Linear · CubicHermite via `interp_hermite` with split tangents — the ONE curve engine;
// out-of-range clamps to the boundary key).

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/time/rational_time.hpp>
#include <crd/timeline/timeline_resource.hpp>

namespace crd::timeline
{

// one active source on one track at the queried time
struct ActiveClip
{
    crd::u32                track_index = 0;
    crd::u32                item_index  = 0;             // into TimelineResource::items
    crd::u32                media_ref   = kInvalidIndex; // kInvalidIndex = a clip with no media (still an edit)
    crd::time::RationalTime media_time;                  // where in the MEDIA to sample (post time-effects)
    crd::f32                weight = 1.0F;               // 1, or the dissolve weight inside a transition window
};

// The duration an item occupies in its track's sequence (transitions: zero). Clips/gaps carry resolved ranges
// (the build contract), so this is exact and total.
[[nodiscard]] crd::time::RationalTime item_duration(const TimelineResource& tl, const ItemRec& item) noexcept;

// The track-local START time of item `index_in_track` (rational accumulation over the preceding items).
[[nodiscard]] crd::time::RationalTime item_start(const TimelineResource& tl, const TrackRec& track,
                                                 crd::u32 index_in_track) noexcept;

// Total duration of a track's sequence / of the whole timeline (the longest track).
[[nodiscard]] crd::time::RationalTime track_duration(const TimelineResource& tl, const TrackRec& track) noexcept;
[[nodiscard]] crd::time::RationalTime timeline_duration(const TimelineResource& tl) noexcept;

// Evaluate every track at timeline-local time `t` (0 = the first item's start; global_start is presentation
// metadata, not an eval offset). Appends 0..2 ActiveClips per track — 2 inside a transition window (outgoing
// first, incoming second; their weights sum to 1). Gaps contribute nothing. Deterministic: identical inputs
// yield identical outputs bit for bit.
void evaluate_tracks(const TimelineResource& tl, const crd::time::RationalTime& t,
                     crd::containers::Array<ActiveClip>& out);

// Sample automation track `index` at `t`. False only when `index` is out of range (a validated track always
// evaluates — clamping at the boundaries per the keyframe contract).
[[nodiscard]] bool automation_value(const TimelineResource& tl, crd::u32 index, const crd::time::RationalTime& t,
                                    crd::f32& out) noexcept;

// Find an automation track by its target path ("camera.fov"). kInvalidIndex when absent.
[[nodiscard]] crd::u32 find_automation(const TimelineResource& tl, const char* target) noexcept;

} // namespace crd::timeline
