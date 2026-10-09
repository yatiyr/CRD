#pragma once

// crd-sandbox -- the CEIR dialects the sandbox's diagnostic commands load programs with (DIAG.8c, DIAG.9a). One
// registrar serves every program command the sandbox registers (program.provenance, program.inspect, replay.prepare,
// replay.record and replay.run), so a program the panel can name is one the sandbox can read: arith, core, func, the
// host programs' task and async (replay.record executor=host), host inputs (input) and device programs (resource and
// compute; replay.record executor=device). crd-sandbox-inspect-tests compiles this source too, so the panel cases load
// programs exactly as the sandbox does.

#include <crd/ceir/context.hpp>

namespace crd::sandbox
{
// A cook::Registrar: registers the sandbox's dialects into `ctx`. `user` is unused.
void register_sandbox_dialects(ceir::Context& ctx, void* user);
} // namespace crd::sandbox
