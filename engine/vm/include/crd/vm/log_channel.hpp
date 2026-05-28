#pragma once

#include <crd/log/log_channel.hpp>

namespace crd::vm
{
// Log channel for the virtual-memory substrate: reserve/commit/decommit/protect
// failures (VirtualAlloc/mmap/mprotect errors). Owned by crd-vm itself.
// Dependency direction is one-way: crd-vm -> crd-log.
CRD_DECLARE_LOG_CHANNEL(g_log_vm);
} // namespace crd::vm
