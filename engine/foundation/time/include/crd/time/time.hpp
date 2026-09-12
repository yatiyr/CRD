#pragma once

// ---------------------------------------------------------------------------
// crd-time -- umbrella header.
//
// Includes: Instant + Duration + clocks (Monotonic / Wall / CycleCounter) +
// Stopwatch + FrameClock + DeterministicClock + Deadline + GPU timestamp API.
// ---------------------------------------------------------------------------

#include <crd/time/clocks.hpp>
#include <crd/time/deadline.hpp>
#include <crd/time/deterministic_clock.hpp>
#include <crd/time/duration.hpp>
#include <crd/time/frame_clock.hpp>
#include <crd/time/gpu_timestamp.hpp>
#include <crd/time/instant.hpp>
#include <crd/time/stopwatch.hpp>
