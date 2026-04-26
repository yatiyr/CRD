#pragma once

#include <crd/log/log_channel.hpp>

namespace crd::containers
{
// Log channel for container-subsystem messages (capacity warnings,
// grow events at the verbose end, debug invariants on hash table).
// Subsystems calling container routines should use their own channels
// for normal messages.
CRD_DECLARE_LOG_CHANNEL(g_log_containers);

// Force-link helper. crd-containers is a header-only library in
// practice (the only .cpp is log_channel.cpp itself), so MSVC's linker
// happily strips the entire .obj from the static archive when the test
// executable doesn't reference any symbol from it. Result: the channel
// registrar never runs.
//
// The fix: this function is DEFINED in log_channel.cpp and CALLED from
// a per-TU anchor in the .cpp's same translation unit (see below).
// Anyone who pulls in the channel via the umbrella header gets a
// reference to the symbol, which keeps the .obj alive.
int force_link_log_channel() noexcept;
} // namespace crd::containers
