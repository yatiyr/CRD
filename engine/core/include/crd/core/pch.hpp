#pragma once

// Shared precompiled-header payload for engine, tests, and runtime targets.
// Keep this intentionally small and stable: core config, platform, types,
// and assert macros that almost every translation unit already needs.

#include <crd/core/assert.hpp>
#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
