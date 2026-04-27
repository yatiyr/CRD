#pragma once

// Umbrella header for the currently shipped slice of crd-platform.
//
// Shipped today:
//   - PlatformContext: GLFW init/shutdown, error -> log bridge          (v1a)
//   - Window + WindowDesc: PIMPL'd OS window, GLFW_NO_API (Vulkan-ready) (v1a)
//   - Timer + FrameClock: chrono-based monotonic timing                  (v1b)
//
// Later slices add Input.

#include <crd/platform/context.hpp>
#include <crd/platform/log_channel.hpp>
#include <crd/platform/timer.hpp>
#include <crd/platform/window.hpp>
