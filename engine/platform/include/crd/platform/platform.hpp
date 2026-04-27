#pragma once

// Umbrella header for the currently shipped slice of crd-platform.
//
// Shipped today (v1a):
//   - PlatformContext: GLFW init/shutdown, error -> log bridge
//   - Window + WindowDesc: PIMPL'd OS window, GLFW_NO_API (Vulkan-ready)
//
// Later slices add Timer/FrameClock and Input.

#include <crd/platform/context.hpp>
#include <crd/platform/log_channel.hpp>
#include <crd/platform/window.hpp>
