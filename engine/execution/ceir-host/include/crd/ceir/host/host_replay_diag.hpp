#pragma once

// crd-ceir-host -- the host provider as the host executor of crd-ceir-cook's replay commands (DIAG.9a). crd-ceir-cook
// stays free of crd-jobs, so `replay.record executor=host` and `replay.run` of a host record call this executor
// through `cook::ReplayCommands::host`; a host binds it there:
//
//     static crd::ceir::cook::ReplayCommands replay{&register_dialects, nullptr};
//     replay.host = &crd::ceir::host::host_replay_executor();
//
// Its record runs `record_host_run` and its replay `replay_host_record` (crd/ceir/host/host_replay.hpp), so a command
// answers exactly what the library calls do. Refusals map to the command's statuses: a plan record, another build and
// a missing input are Unavailable; a program that does not load and a record whose program is not its recorded
// content are Failed; a job split or step budget out of range is BadArgument.
//
// ⛔ The process owns the crd::jobs pool: it must be initialised before a request reaches this executor (as for
// HostProvider), and the executor never initialises or shuts it down. Contract: docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/replay_diag.hpp>

namespace crd::ceir::host
{
// The host provider's executor table (static storage; never null).
[[nodiscard]] const cook::ReplayHostExecutor& host_replay_executor() noexcept;
} // namespace crd::ceir::host
