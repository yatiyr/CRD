#pragma once

// Umbrella header for the currently shipped slice of crd-platform.
//
// Shipped today:
//   - PlatformContext: GLFW init/shutdown, error -> log bridge            (v1a)
//   - Window + WindowDesc: PIMPL'd OS window, GLFW_NO_API (Vulkan-ready)  (v1a)
//   - Timer + FrameClock: chrono-based monotonic timing                   (v1b)
//   - Input: polling snapshot + opt-in event queue                        (v1c)
//   - Filesystem / DynamicLibrary / threading helpers                     (v1d)
//   - FileWatcher: polling mtime watcher for hot-reload scenarios         (v1e)

#include <crd/platform/context.hpp>
#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/file_watcher.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/log_channel.hpp>
#include <crd/platform/threading.hpp>
#include <crd/platform/timer.hpp>
#include <crd/platform/window.hpp>
