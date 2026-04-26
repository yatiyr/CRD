#pragma once

#include <crd/log/log_channel.hpp>

namespace crd::memory
{
// Log channel for memory-subsystem messages (allocator init, exhaustion,
// OOM, leak summaries on shutdown). Subsystems calling memory routines
// should use their own channels for normal messages.
CRD_DECLARE_LOG_CHANNEL(g_log_memory);
} // namespace crd::memory
