#pragma once

// host_dialects.hpp — the CEIR dialects ceridc registers wherever it loads an authored program: the inspect verb's
// executor and the diag service's program and replay commands. One set, so they all answer for the same ops.

namespace crd::ceir
{
class Context;
} // namespace crd::ceir

namespace crd::ceridc
{
// The dialects the CEIR host executors run: the compiled plan's scalar host subset (arith, core, func) and the host
// provider's task and async ops. A Registrar: `user` is unused.
void register_host_dialects(crd::ceir::Context& ctx, void* user);
} // namespace crd::ceridc
