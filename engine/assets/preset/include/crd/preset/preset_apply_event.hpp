#pragma once

#include <crd/core/types.hpp>

namespace crd::preset
{
class PresetResource;

// PresetApplyEvent — emitted on hot-reload to live IPresetTargets so they
// can re-pull resolved values without a per-frame poll. v1n1 declares the
// event shape; the dispatch wiring lands when hot-reload integrates (later
// in Phase 3.0 v1n / Phase 4 editor).
//
// `old_payload` is the last-good payload (may be nullptr on first apply).
// `new_payload` is the freshly-cooked payload that triggered this event.
// Targets diff fields they care about and re-apply only the changed ones.
struct PresetApplyEvent
{
    crd::u32              fourcc{};
    const PresetResource* old_payload = nullptr;
    const PresetResource* new_payload = nullptr;
};

} // namespace crd::preset
