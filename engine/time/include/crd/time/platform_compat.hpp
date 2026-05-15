#pragma once

// ---------------------------------------------------------------------------
// crd-time -- backward-compat aliases for the former crd::platform::Timer /
// crd::platform::FrameClock surface (Detour D-006 migration shim).
//
// Per the ADR-0076 §13 move-and-delete pattern: `engine/platform/timer.hpp`
// is deleted; this header provides the `crd::platform::Timer` and
// `crd::platform::FrameClock` names as aliases into `crd::time::*` so
// existing consumers keep compiling while their imports + namespaces
// migrate in the v0b adoption pass.
//
// **Consumers should migrate to `crd::time::Stopwatch` + `crd::time::FrameClock`
// directly.** This shim exists for the brief migration window only; remove
// once all consumers point at the new names.
// ---------------------------------------------------------------------------

#include <crd/time/frame_clock.hpp>
#include <crd/time/stopwatch.hpp>

namespace crd::platform
{
using Timer      = crd::time::Stopwatch;
using FrameClock = crd::time::FrameClock;
} // namespace crd::platform
