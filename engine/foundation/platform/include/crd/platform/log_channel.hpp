#pragma once

#include <crd/log/log_channel.hpp>

namespace crd::platform
{
// Log channel for platform-subsystem messages: GLFW init/shutdown, window
// lifecycle, GLFW error callbacks bridged to the engine logger.
//
// Unlike `g_log_containers` (whose definition lives inside crd-log to break
// a historic cycle), `g_log_platform` is owned by crd-platform itself.
// Dependency direction is one-way: crd-platform -> crd-log, and we never
// need crd-log to know about crd-platform.
CRD_DECLARE_LOG_CHANNEL(g_log_platform);
} // namespace crd::platform
